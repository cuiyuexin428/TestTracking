"""Install trusted server plugin bundles without changing a running deployment.

An installation creates new directories and switches configuration references.
Existing DLLs, plugin resources, saved analysis scripts and run records are never
overwritten. The scheduler and result service load the new files on restart.
"""
from __future__ import annotations

import ast
import base64
import copy
import hashlib
import json
import os
import re
import shutil
import stat
import uuid
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from .artifacts import ArtifactError, ArtifactStore
from .config import ServiceConfig
from .management import ManagementError, ManagementService, _revision, _validate
from .plugins import (PluginAIContextStore, PluginAdapterStore, ResultPluginError,
                      _validate_packaged_resources)
from .views import PluginViewError, PluginViewStore, validate_plugin_view


MANIFEST = "server_plugin.json"
MAX_FILES = 4096
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_EXPANDED_BYTES = 1024 * 1024 * 1024
_PLUGIN = re.compile(r"[A-Za-z_][A-Za-z0-9_-]{0,63}")
_VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,127}")
_ROLES = {
    "view": ("plugin_views_dir", "views", ".json", "result_view.json"),
    "adapter": ("plugin_adapters_dir", "adapters", ".py", "result_adapter.py"),
    "ai_context": ("plugin_ai_contexts_dir", "ai_contexts", ".md", "AI_CONTEXT.md"),
    "report_template": ("report_templates_dir", "report_templates", ".py", "result_report.py"),
}
_RESERVED = re.compile(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?")


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")


def _relative_path(value: Any) -> str:
    if (not isinstance(value, str) or not value or "\\" in value
            or any(ord(character) < 32 for character in value)):
        raise ManagementError("插件包路径必须是使用 / 的相对路径", 422)
    parts = value.split("/")
    if (PurePosixPath(value).is_absolute() or any(
            part in {"", ".", ".."} or part[-1:] in {" ", "."}
            or any(character in part for character in ':*?"<>|')
            or _RESERVED.fullmatch(part) for part in parts)):
        raise ManagementError("插件包包含不安全的路径：" + value, 422)
    return value


def _read_json(raw: bytes, label: str) -> dict[str, Any]:
    def reject_constant(value: str) -> None:
        raise ValueError("non-finite JSON number: " + value)
    try:
        value = json.loads(raw.decode("utf-8-sig"), parse_constant=reject_constant)
    except (ValueError, UnicodeError) as exc:
        raise ManagementError(label + " 必须是有效 UTF-8 JSON", 422) from exc
    if not isinstance(value, dict):
        raise ManagementError(label + " 必须是 JSON 对象", 422)
    return value


def _text(raw: bytes, label: str, limit: int) -> str:
    if not raw or len(raw) > limit:
        raise ManagementError(label + " 为空或超过大小限制", 422)
    try:
        value = raw.decode("utf-8-sig")
    except UnicodeError as exc:
        raise ManagementError(label + " 必须使用 UTF-8", 422) from exc
    if not value.strip():
        raise ManagementError(label + " 不能为空", 422)
    return value


def inspect_bundle(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Validate an archive using data/AST only; never import its Python code."""
    try:
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            if not members or len(members) > MAX_FILES:
                raise ManagementError("插件包文件数量必须在 1 到 4096 之间", 422)
            files: dict[str, zipfile.ZipInfo] = {}
            seen: set[str] = set()
            total = 0
            for member in members:
                # ZipInfo normalizes Windows backslashes in .filename; inspect
                # the original spelling so noncanonical paths cannot hide here.
                name = _relative_path(member.orig_filename.rstrip("/") if member.is_dir() else member.orig_filename)
                mode = stat.S_IFMT(member.external_attr >> 16)
                if mode not in {0, stat.S_IFREG, stat.S_IFDIR} or member.flag_bits & 1:
                    raise ManagementError("插件包不能包含链接、特殊文件或加密文件", 422)
                normalized = name.casefold()
                if normalized in seen:
                    raise ManagementError("插件包包含重复路径：" + name, 422)
                seen.add(normalized)
                if member.is_dir():
                    continue
                if member.file_size < 0 or member.file_size > MAX_FILE_BYTES:
                    raise ManagementError("插件包单文件不能超过 512 MiB", 422)
                total += member.file_size
                if total > MAX_EXPANDED_BYTES:
                    raise ManagementError("插件包解压后不能超过 1 GiB", 422)
                files[name] = member
            normalized_files = {name.casefold() for name in files}
            for name in files:
                if any(str(parent).casefold() in normalized_files for parent in PurePosixPath(name).parents
                       if str(parent) != "."):
                    raise ManagementError("插件包文件与目录路径冲突", 422)
            if MANIFEST not in files or files[MANIFEST].file_size > 64 * 1024:
                raise ManagementError("插件包根目录必须包含 server_plugin.json（不超过 64 KiB）", 422)
            definition = _read_json(archive.read(MANIFEST), MANIFEST)
            if set(definition) - {"schema_version", "package_type", "plugin", "version", "scheduler", "resources"}:
                raise ManagementError("server_plugin.json 包含未知字段", 422)
            if (type(definition.get("schema_version")) is not int or definition["schema_version"] != 1
                    or definition.get("package_type") != "testtracking-server-plugin"):
                raise ManagementError("不支持的服务端插件包类型或 schema_version", 422)
            plugin, version = definition.get("plugin"), definition.get("version")
            if not isinstance(plugin, str) or not _PLUGIN.fullmatch(plugin):
                raise ManagementError("插件名称必须为合法的 plugin id", 422)
            if not isinstance(version, str) or not _VERSION.fullmatch(version):
                raise ManagementError("插件版本无效", 422)
            scheduler, resources = definition.get("scheduler"), definition.get("resources")
            if (not isinstance(scheduler, dict) or set(scheduler) - {"dll", "defaults"}
                    or not isinstance(scheduler.get("defaults", {}), dict)):
                raise ManagementError("scheduler 必须声明 dll 和可选 defaults 对象", 422)
            defaults = scheduler.get("defaults", {})
            if any(key in defaults for key in ("dll", "enabled", "stp_manifest")):
                raise ManagementError("scheduler.defaults 不能设置 dll、enabled 或 stp_manifest", 422)
            if (not isinstance(resources, dict) or set(resources) - (set(_ROLES) | {"manifest"})
                    or not {"view", "ai_context"}.issubset(resources)):
                raise ManagementError("resources 必须声明 view、ai_context，其他资源只能使用已定义名称", 422)
            required = {"dll": scheduler.get("dll"), **resources}
            for role, source in required.items():
                _relative_path(source)
                if source not in files or files[source].file_size == 0:
                    raise ManagementError("插件包缺少非空资源：" + role, 422)
            if not scheduler["dll"].lower().endswith(".dll"):
                raise ManagementError("scheduler.dll 必须指向 DLL 文件", 422)
            if files[resources["view"]].file_size > 1024 * 1024:
                raise ManagementError("view 不能超过 1 MiB", 422)
            try:
                validate_plugin_view(_read_json(archive.read(resources["view"]), "view"), plugin)
            except PluginViewError as exc:
                raise ManagementError(str(exc), 422) from exc
            if files[resources["ai_context"]].file_size > PluginAIContextStore.MAX_BYTES:
                raise ManagementError("AI_CONTEXT.md 不能超过 64 KiB", 422)
            _text(archive.read(resources["ai_context"]), "AI_CONTEXT.md", PluginAIContextStore.MAX_BYTES)
            for role in ("adapter", "report_template"):
                if role in resources:
                    if files[resources[role]].file_size > 1024 * 1024:
                        raise ManagementError(role + " 不能超过 1 MiB", 422)
                    source = _text(archive.read(resources[role]), role, 1024 * 1024)
                    try:
                        tree = ast.parse(source, filename=resources[role])
                    except SyntaxError as exc:
                        raise ManagementError(role + " Python 语法无效", 422) from exc
                    if role == "adapter" and not any(isinstance(node, ast.FunctionDef)
                            and node.name == "create_result_adapter" for node in tree.body):
                        raise ManagementError("adapter 必须定义 create_result_adapter", 422)
            _validate("scheduler", {"server": {}, "plugins": {plugin: {**defaults, "enabled": False}}})
            return definition, [{"path": name, "size_bytes": item.file_size}
                                for name, item in sorted(files.items())]
    except (OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as exc:
        if isinstance(exc, ManagementError):
            raise
        raise ManagementError("插件包不是可读取的 ZIP 文件", 422) from exc


class ServerPluginInstaller:
    def __init__(self, config: ServiceConfig, artifacts: ArtifactStore, management: ManagementService):
        self.config = config
        self.artifacts = artifacts
        self.management = management
        self.root = artifacts.root / "server-plugins"
        self.journal = self.root / "pending-install.json"
        self.transaction_warning: str | None = None
        with self.management._lock:
            self.recovered_configuration = self._recover_transaction()

    def _configuration(self) -> tuple[Path, Path, bytes, dict[str, Any], bytes, dict[str, Any]]:
        scheduler = self.management._path("scheduler")
        result = self.management._path("result_service")
        if scheduler is None or result is None or scheduler == result:
            raise ManagementError("安装插件需要配置独立的 scheduler_config_path 和结果服务配置文件", 409)
        scheduler_raw, scheduler_config = self.management._read_config(scheduler)
        result_raw, result_config = self.management._read_config(result)
        _validate("scheduler", scheduler_config)
        _validate("result_service", result_config)
        return scheduler, result, scheduler_raw, scheduler_config, result_raw, result_config

    def _bundle(self, body: Any) -> tuple[dict[str, Any], Path, dict[str, Any], list[dict[str, Any]]]:
        if not isinstance(body, dict) or not isinstance(body.get("artifact_id"), str):
            raise ManagementError("artifact_id 必须是已上传的服务端插件制品标识")
        artifact = self.artifacts.get(body["artifact_id"])
        if artifact is None:
            raise ManagementError("服务端插件制品不存在", 404)
        if artifact["kind"] != "plugin":
            raise ManagementError("服务端插件安装只接受 plugin 制品", 422)
        try:
            path = self.artifacts.object_path(artifact["sha256"])
            if path.stat().st_size != artifact["size_bytes"]:
                raise ManagementError("制品文件大小与上传记录不一致", 422)
            digest = hashlib.sha256()
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() != artifact["sha256"]:
                raise ManagementError("制品 SHA-256 与上传记录不一致", 422)
            definition, files = inspect_bundle(path)
        except ArtifactError as exc:
            raise ManagementError(str(exc), exc.status) from exc
        except OSError as exc:
            raise ManagementError("无法读取插件制品", 422) from exc
        if artifact["name"] != definition["plugin"] + "-server" or artifact["version"] != definition["version"]:
            raise ManagementError("制品名称必须是 <plugin>-server，版本必须与 server_plugin.json 一致", 422)
        return artifact, path, definition, files

    def preview(self, body: Any) -> dict[str, Any]:
        artifact, _, definition, files = self._bundle(body)
        with self.management._lock:
            scheduler, result, scheduler_raw, scheduler_config, result_raw, _ = self._configuration()
            plugin = definition["plugin"]
            existing = scheduler_config["plugins"].get(plugin)
            warnings = ["安装后需重启调度器和结果服务；安装不会自动启用新插件。",
                        "插件 DLL 必须与本次框架构建兼容；安装预览不执行 DLL 或 Python 代码。"]
            if not self.config.workspace:
                warnings.append("未配置 workspace，调度器资源将使用绝对路径；搬移目录前需重新配置。")
            if existing is not None and "manifest" in definition["resources"] and existing.get("stp_manifest"):
                warnings.append("升级保留已有 stp_manifest；包内 manifest 仅供新插件默认配置使用。")
            return {"artifact_id": artifact["artifact_id"], "sha256": artifact["sha256"],
                    "plugin": plugin, "version": definition["version"],
                    "action": "upgrade" if existing is not None else "install",
                    "scheduler_revision": _revision(scheduler_raw),
                    "result_service_revision": _revision(result_raw),
                    "scheduler_config_path": str(scheduler), "result_service_config_path": str(result),
                    "resources": sorted(definition["resources"]), "files": files,
                    "expanded_size_bytes": sum(item["size_bytes"] for item in files),
                    "enabled_after_install": bool(existing.get("enabled", False)) if existing else False,
                    "preserves_existing_configuration": existing is not None,
                    "restart_required": ["scheduler", "result_service"], "warnings": warnings}

    def install(self, body: Any) -> dict[str, Any]:
        artifact, archive_path, definition, files = self._bundle(body)
        operator = body.get("operator_id", "")
        if (not isinstance(operator, str) or len(operator) > 128
                or any(ord(character) < 32 for character in operator)):
            raise ManagementError("operator_id 必须是不超过 128 字符的字符串")
        with self.management._lock:
            self._recover_transaction()
            if self.journal.exists():
                raise ManagementError("上次插件安装已完成，但事务日志尚未清理，请先检查文件写入权限", 409)
            scheduler, result, scheduler_raw, scheduler_config, result_raw, result_config = self._configuration()
            if (body.get("scheduler_revision") != _revision(scheduler_raw)
                    or body.get("result_service_revision") != _revision(result_raw)):
                raise ManagementError("配置已被其他操作修改，请重新预览安装计划", 409)
            plugin = definition["plugin"]
            self.management.require_plugin_idle(plugin)
            installation_id = uuid.uuid4().hex
            destination = self.root / "installations" / installation_id
            destination.mkdir(parents=True)
            package = destination / "package"
            try:
                self._extract(archive_path, files, package)
                self._snapshot_resources(result_config, result.parent, definition, package, destination)
                next_scheduler = copy.deepcopy(scheduler_config)
                existing = next_scheduler["plugins"].get(plugin)
                proposed = (copy.deepcopy(existing) if existing is not None else
                            {**definition["scheduler"].get("defaults", {}), "enabled": False})
                proposed["dll"] = self._scheduler_path(package / definition["scheduler"]["dll"])
                if "manifest" in definition["resources"] and not proposed.get("stp_manifest"):
                    proposed["stp_manifest"] = self._scheduler_path(package / definition["resources"]["manifest"])
                next_scheduler["plugins"][plugin] = proposed
                next_result = copy.deepcopy(result_config)
                for config_key, directory, _, _ in _ROLES.values():
                    next_result[config_key] = self._path_from(destination / directory, result.parent)
                _validate("scheduler", next_scheduler)
                _validate("result_service", next_result)
                scheduler_encoded, result_encoded = _json_bytes(next_scheduler), _json_bytes(next_result)
                # Recheck after preparing the immutable tree. No active or queued
                # work may appear between a slow extraction and activation.
                self.management.require_plugin_idle(plugin)
                if (scheduler.read_bytes() != scheduler_raw or result.read_bytes() != result_raw):
                    raise ManagementError("配置在准备插件期间已变化，请重新预览", 409)
                record = {"installation_id": installation_id, "artifact_id": artifact["artifact_id"],
                          "sha256": artifact["sha256"], "plugin": plugin,
                          "version": definition["version"], "operator_id": operator,
                          "installed_at": datetime.now(timezone.utc).isoformat(),
                          "scheduler_revision": _revision(scheduler_encoded),
                          "result_service_revision": _revision(result_encoded),
                          "restart_required": ["scheduler", "result_service"]}
                self.management._atomic_write(destination / "installation.json", _json_bytes(record))
                warnings = self._commit_configuration(scheduler, result, scheduler_raw, result_raw,
                                                      scheduler_encoded, result_encoded, destination)
                return {**record, "installed": True, "installation_path": str(destination),
                        "enabled": bool(proposed.get("enabled", False)),
                        "warnings": warnings,
                        "notice": "文件和配置已安装；重启调度器及结果服务后生效。旧文件、备份和用户分析脚本均保留。"}
            except Exception as exc:
                # Keep failed trees for diagnosis if a journal still needs
                # recovery. Otherwise they are unreferenced and safe to remove.
                if not self.journal.exists():
                    shutil.rmtree(destination, ignore_errors=True)
                if isinstance(exc, ManagementError):
                    raise
                raise ManagementError("插件安装失败，原配置已保留或恢复：" + str(exc), 500) from exc

    def _scheduler_path(self, path: Path) -> str:
        return self._path_from(path, Path(self.config.workspace)) if self.config.workspace else path.as_posix()

    @staticmethod
    def _path_from(path: Path, base: Path) -> str:
        try:
            return Path(os.path.relpath(path, base)).as_posix()
        except ValueError:  # Different Windows drives cannot use a relative path.
            return path.as_posix()

    @staticmethod
    def _extract(archive_path: Path, files: list[dict[str, Any]], destination: Path) -> None:
        with zipfile.ZipFile(archive_path) as archive:
            for item in files:
                target = destination.joinpath(*PurePosixPath(item["path"]).parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item["path"]) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)

    def _snapshot_resources(self, current: dict[str, Any], config_root: Path, definition: dict[str, Any],
                            package: Path, destination: Path) -> None:
        plugin = definition["plugin"]
        source_plugins = Path(__file__).resolve().parents[2] / "plugins"
        def configured_path(key: str) -> Path | None:
            value = current.get(key)
            return (config_root / value).resolve() if value else None
        try:
            _validate_packaged_resources(
                PluginViewStore(configured_path("plugin_views_dir")),
                PluginAdapterStore(configured_path("plugin_adapters_dir")),
                PluginAIContextStore(configured_path("plugin_ai_contexts_dir")),
                configured_path("report_templates_dir"))
        except ResultPluginError as exc:
            raise ManagementError("现有插件资源不完整，安装已停止：" + str(exc), 409) from exc
        for role, (config_key, directory, suffix, source_name) in _ROLES.items():
            target = destination / directory
            target.mkdir()
            configured = current.get(config_key)
            if configured:
                source = (config_root / configured).resolve()
                if not source.is_dir():
                    raise ManagementError("现有插件资源目录不存在：" + config_key, 409)
                candidates = [(item, item.name) for item in source.glob("*" + suffix)]
            else:
                candidates = [(item, item.parent.name + suffix) for item in source_plugins.glob("*/" + source_name)]
            for source, filename in candidates:
                if source.is_file():
                    shutil.copy2(source, target / filename)
            owned = target / (plugin + suffix)
            if role in definition["resources"]:
                shutil.copy2(package / definition["resources"][role], owned)
            else:
                owned.unlink(missing_ok=True)
        declarations = {}
        for view in (destination / "views").glob("*.json"):
            name = view.stem
            validate_plugin_view(_read_json(view.read_bytes(), name + " view"), name)
            declarations[name] = [role for role, (_, directory, suffix, _) in _ROLES.items()
                                  if (destination / directory / (name + suffix)).is_file()]
        self.management._atomic_write(destination / "views" / "resources.manifest",
                                      _json_bytes({"schema_version": 1, "plugins": declarations}))

    def _commit_configuration(self, scheduler: Path, result: Path, scheduler_raw: bytes,
                              result_raw: bytes, scheduler_new: bytes, result_new: bytes,
                              destination: Path) -> list[str]:
        marker = self.management._scheduler_state_path
        marker_original = marker.read_bytes() if marker and marker.exists() else None
        instance = self.management.get_status().get("instance_id")
        marker_new = _json_bytes({"instance_id": instance, "revision": _revision(scheduler_new)})
        entries = [{"path": str(path), "old": base64.b64encode(old).decode("ascii"),
                    "new_revision": _revision(new)} for path, old, new in (
                        (scheduler, scheduler_raw, scheduler_new), (result, result_raw, result_new))]
        if marker:
            entries.append({"path": str(marker), "old": base64.b64encode(marker_original).decode("ascii")
                            if marker_original is not None else None, "new_revision": _revision(marker_new)})
        # Durable backups are stored with this immutable installation, rather
        # than replacing the management editor's independent .bak files.
        self.management._atomic_write(destination / "scheduler.before.json", scheduler_raw)
        self.management._atomic_write(destination / "result_service.before.json", result_raw)
        self.management._atomic_write(self.journal, _json_bytes({"entries": entries}))
        try:
            if marker:
                self.management._atomic_write(marker, marker_new)
            self.management._atomic_write(scheduler, scheduler_new)
            self.management._atomic_write(result, result_new)
        except Exception:
            self._recover_transaction(force_rollback=True)
            raise
        self.management._pending_scheduler_instance = instance
        self.management._pending_scheduler_revision = _revision(scheduler_new)
        self.management._scheduler_save_pending = True
        try:
            self.journal.unlink()
        except OSError:
            # Both config files are committed. A housekeeping failure must not
            # remove their referenced directory or report a rolled-back install.
            return ["安装已提交，但事务日志暂未清理；下次启动或安装时会重新核对，请检查文件写入权限。"]
        return []

    def _recover_transaction(self, force_rollback: bool = False) -> bool:
        if not self.journal.exists():
            return False
        try:
            journal = _read_json(self.journal.read_bytes(), "插件安装恢复记录")
            entries = journal["entries"]
            allowed = {self.management._path("scheduler"), self.management._path("result_service"),
                       self.management._scheduler_state_path}
            if not isinstance(entries, list) or len(entries) not in {2, 3}:
                raise ValueError("invalid transaction")
            originals = []
            all_committed = True
            configuration_changed = False
            for entry in entries:
                path = Path(entry["path"]).resolve()
                if path not in allowed:
                    raise ValueError("transaction refers to another configuration")
                old = base64.b64decode(entry["old"], validate=True) if entry["old"] is not None else None
                current = path.read_bytes() if path.exists() else None
                committed = current is not None and _revision(current) == entry["new_revision"]
                if current != old and not committed:
                    raise ManagementError("未完成的插件安装配置已被外部修改，请保留 pending-install.json 并核对备份", 409)
                all_committed = all_committed and committed
                if path != self.management._scheduler_state_path and old != current:
                    configuration_changed = True
                originals.append((path, old))
            if force_rollback or not all_committed:
                for path, old in originals:
                    if old is None:
                        path.unlink(missing_ok=True)
                    else:
                        self.management._atomic_write(path, old)
            else:
                configuration_changed = False
            marker = self.management._scheduler_state_path
            state = _read_json(marker.read_bytes(), "配置状态") if marker and marker.exists() else {}
            self.management._pending_scheduler_instance = state.get("instance_id")
            self.management._pending_scheduler_revision = state.get("revision")
            self.management._scheduler_save_pending = bool(state)
            try:
                self.journal.unlink()
                self.transaction_warning = None
            except OSError:
                self.transaction_warning = "插件配置已恢复一致，但事务日志暂未清理，请检查文件写入权限。"
            return configuration_changed
        except ManagementError:
            raise
        except Exception as exc:
            raise ManagementError("插件安装事务尚未恢复，请检查 pending-install.json 与文件写入权限", 500) from exc
