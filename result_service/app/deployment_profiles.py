"""Reusable, revision-checked deployment profiles compiled into Agent plans."""
from __future__ import annotations

import copy
import hashlib
import json
import re
import threading
import uuid
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from .deployments import DeploymentError, DeploymentStore
from .resource_catalog import ResourceCatalog, atomic_write, json_bytes, relative_path


_PLUGINS = {"coverage", "stpcompare"}
_ARGUMENTS = ["--input", "{stp}", "--output-dir", "{outdir}"]


class ProfileStore:
    def __init__(self, path: str | Path, deployments: DeploymentStore):
        self.path = Path(path).resolve()
        self.deployments = deployments
        self.artifacts = deployments.artifacts
        self.catalog = ResourceCatalog(self.artifacts)
        self._lock = threading.RLock()

    def list(self) -> list[dict]:
        with self._lock:
            return sorted(copy.deepcopy(list(self._read()["profiles"].values())),
                          key=lambda item: (item["updated_at"], item["profile_id"]), reverse=True)

    def get(self, profile_id: str) -> dict:
        with self._lock:
            result = self._read()["profiles"].get(profile_id)
            if result is None:
                raise DeploymentError("部署方案不存在", 404)
            return copy.deepcopy(result)

    def resources(self, artifact_id: str) -> dict:
        return self.catalog.resources(artifact_id)

    def save(self, profile_id: str | None, body: Any) -> dict:
        if not isinstance(body, dict) or "revision" not in body:
            raise DeploymentError("保存方案必须提供 revision；创建时为 null")
        revision = DeploymentStore._optional_revision(body["revision"], "revision")
        with self._lock:
            state = self._read()
            previous = state["profiles"].get(profile_id) if profile_id is not None else None
            if profile_id is not None and previous is None:
                raise DeploymentError("部署方案不存在", 404)
            current = previous["revision"] if previous else None
            if revision != current:
                raise DeploymentError("部署方案已被其他使用者修改，请重新加载", 409, current_revision=current)
            if body.get("profile_id") is not None and body["profile_id"] != profile_id:
                raise DeploymentError("profile_id 与保存目标不一致")
            normalized = self._normalize(body)
            # Validate all entries and prepare immutable data before publishing.
            self._compile(normalized)
            selected_id = profile_id or uuid.uuid4().hex
            content = {"profile_id": selected_id, **normalized}
            next_revision = hashlib.sha256(json_bytes({"previous_revision": current, **content})).hexdigest()
            record = {**content, "revision": next_revision,
                      "updated_at": datetime.now(timezone.utc).isoformat()}
            state["profiles"][selected_id] = record
            atomic_write(self.path, json_bytes(state))
            return copy.deepcopy(record)

    def compile(self, record: dict) -> dict:
        return self._compile(self._normalize(record))

    def _normalize(self, body: Any) -> dict:
        if not isinstance(body, dict) or set(body) - {"profile_id", "revision", "name", "description", "plugins", "schedule", "updated_at"}:
            raise DeploymentError("部署方案包含未知字段")
        name = body.get("name")
        if not isinstance(name, str) or not name.strip() or len(name) > 128:
            raise DeploymentError("方案名称必须为 1 到 128 个字符")
        description = body.get("description", "")
        if not isinstance(description, str) or len(description) > 4096:
            raise DeploymentError("方案说明不能超过 4096 个字符")
        plugins = body.get("plugins")
        if not isinstance(plugins, dict) or not plugins or set(plugins) - _PLUGINS:
            raise DeploymentError("请选择 coverage 或 stpcompare 插件")
        normalized = {}
        for name_key, plugin in plugins.items():
            allowed = {"resources", "dry_run", "store_cov", "engine_dlls"} if name_key == "coverage" else {"resources", "dry_run", "arguments"}
            if not isinstance(plugin, dict) or set(plugin) - allowed:
                raise DeploymentError(f"{name_key} 包含未知配置字段")
            if not isinstance(plugin.get("dry_run"), bool):
                raise DeploymentError(f"请明确选择 {name_key} 真实执行或模拟执行")
            roles = {"plugin": "plugin", "dataset": "dataset"}
            roles.update({"program": "program", "collector": "program"} if name_key == "coverage"
                         else {"baseline": "program", "candidate": "program"})
            resources = plugin.get("resources")
            if not isinstance(resources, dict) or set(resources) != set(roles):
                raise DeploymentError(f"请为 {name_key} 选择全部插件、程序、工具和数据资源")
            result = {"dry_run": plugin["dry_run"], "resources": {
                role: self._selection(resources[role], kind, role) for role, kind in roles.items()}}
            if name_key == "coverage":
                store_cov = plugin.get("store_cov", False)
                if not isinstance(store_cov, bool):
                    raise DeploymentError("store_cov 必须为 true 或 false")
                engine_dlls = plugin.get("engine_dlls", [])
                if not isinstance(engine_dlls, list) or not all(isinstance(item, str) for item in engine_dlls):
                    raise DeploymentError("engine_dlls 必须是业务程序包内 DLL 的数组")
                available = self.resources(result["resources"]["program"]["artifact_id"])["dlls"]
                if any(relative_path(item) not in available for item in engine_dlls):
                    raise DeploymentError("采集 DLL 必须存在于业务程序包中", 422)
                result.update(store_cov=store_cov, engine_dlls=list(dict.fromkeys(engine_dlls)))
            else:
                arguments = plugin.get("arguments", {})
                if not isinstance(arguments, dict) or set(arguments) - {"baseline", "candidate"}:
                    raise DeploymentError("arguments 只能配置 baseline 和 candidate")
                selected = {}
                for role in ("baseline", "candidate"):
                    values = arguments.get(role, _ARGUMENTS)
                    if (not isinstance(values, list) or not values or len(values) > 128
                            or not all(isinstance(value, str) and len(value) <= 8192 for value in values)
                            or not all(any(token in value for value in values) for token in ("{stp}", "{outdir}"))):
                        raise DeploymentError("程序参数必须包含 {stp} 和 {outdir}，且使用字符串数组")
                    selected[role] = copy.deepcopy(values)
                result["arguments"] = selected
            normalized[name_key] = result
        return {"name": name.strip(), "description": description,
                "plugins": normalized, "schedule": self._schedule(body.get("schedule", {}))}

    def _selection(self, selection: Any, kind: str, role: str) -> dict:
        field = "root" if kind == "dataset" else "entry"
        if not isinstance(selection, dict) or set(selection) - {"artifact_id", field}:
            raise DeploymentError(f"{role} 资源描述无效")
        record = self.catalog._artifact(selection.get("artifact_id"))
        if record["kind"] != kind:
            raise DeploymentError(f"{role} 需要 {kind} 类型资源", 422)
        listing = self.resources(record["artifact_id"])
        if kind == "dataset":
            value = relative_path(selection.get("root", ""), root=True)
            if value not in listing["dataset_roots"]:
                raise DeploymentError("所选数据目录中没有 STP 文件", 422)
        else:
            candidates = listing["dlls"] if kind == "plugin" else listing["executables"]
            value = selection.get("entry")
            if value in (None, ""):
                if len(candidates) != 1:
                    raise DeploymentError(f"{role} 包有多个入口或没有入口，请从文件列表选择", 422)
                value = candidates[0]
            value = relative_path(value)
            if value not in candidates or not any(item["path"] == value and item["size_bytes"] > 0 for item in listing["files"]):
                raise DeploymentError(f"{role} 入口不存在或文件类型不正确", 422)
        return {"artifact_id": record["artifact_id"], field: value}

    @staticmethod
    def _schedule(value: Any) -> dict:
        if not isinstance(value, dict) or set(value) - {"task_threads", "execution_windows", "update_windows"}:
            raise DeploymentError("运行安排包含未知字段")
        threads = value.get("task_threads", 1)
        if isinstance(threads, bool) or not isinstance(threads, int) or not 1 <= threads <= 1024:
            raise DeploymentError("每插件并发必须是 1 到 1024 的整数")
        result = {"task_threads": threads}
        for key in ("execution_windows", "update_windows"):
            windows = value.get(key, [])
            if not isinstance(windows, list) or len(windows) > 128:
                raise DeploymentError("时间窗必须是最多 128 项的数组")
            for window in windows:
                if not isinstance(window, dict) or set(window) - {"start", "end", "days"}:
                    raise DeploymentError("时间窗只允许 start、end 和 days")
                if any(not isinstance(window.get(field), str) or not re.fullmatch(r"(?:[01][0-9]|2[0-3]):[0-5][0-9]", window[field])
                       for field in ("start", "end")):
                    raise DeploymentError("时间必须使用 HH:MM")
                days = window.get("days", [])
                if not isinstance(days, list) or any(type(day) is not int or not 1 <= day <= 7 for day in days):
                    raise DeploymentError("星期必须是 1（周一）到 7（周日）的整数数组")
            result[key] = copy.deepcopy(windows)
        return result

    def _compile(self, profile: dict) -> dict:
        components = []
        patch = {"client": copy.deepcopy(profile["schedule"]), "plugins": {}}
        datasets = []
        for name, settings in profile["plugins"].items():
            resources = settings["resources"]
            references = {}
            for role, selection in resources.items():
                slot = f"profile.{name}.{role}"
                artifact_id = selection["artifact_id"]
                if role == "dataset":
                    data = self.catalog.prepare_dataset(artifact_id, selection["root"])
                    artifact_id = data["artifact_id"]
                    datasets.append({"plugin": name, **data})
                    entry = selection["root"]
                else:
                    entry = selection["entry"]
                components.append({"slot": slot, "artifact_id": artifact_id})
                references[role] = "{artifact:" + slot + "}" + ("/" + entry if entry else "")
            config = {"enabled": True, "dll": references["plugin"], "stp_root": references["dataset"],
                      "dataset_version_file": "", "dry_run": settings["dry_run"],
                      "work_dir": f"work/{name}"}
            if name == "coverage":
                slot_root = "{artifact:profile.coverage.program}"
                parent = str(PurePosixPath(resources["program"]["entry"]).parent)
                config.update(target_exe=references["program"],
                    program_dir=slot_root + ("/" + parent if parent != "." else ""),
                    opencppcoverage=references["collector"], store_cov=settings["store_cov"],
                    engine_dlls=[slot_root + "/" + item for item in settings["engine_dlls"]],
                    export_dir="exports/coverage")
            else:
                config["programs"] = {role: {"executable": references[role], "version_file": "",
                    "arguments": settings["arguments"][role]} for role in ("baseline", "candidate")}
            patch["plugins"][name] = config
        prepared = self.deployments.prepare(components, patch)
        for data in datasets:
            prepared["scheduler_plugins"].setdefault(data["plugin"], {}).update(
                stp_manifest=data["manifest_path"], expected_dataset_version=data["version"])
        return {**prepared, "dataset_bindings": datasets}

    def _read(self) -> dict:
        if not self.path.exists():
            return {"schema_version": 1, "profiles": {}}
        try:
            value = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError, UnicodeError) as exc:
            raise DeploymentError("部署方案目录无法读取", 500) from exc
        if not isinstance(value, dict) or value.get("schema_version") != 1 or not isinstance(value.get("profiles"), dict):
            raise DeploymentError("部署方案目录格式不受支持", 500)
        return value
