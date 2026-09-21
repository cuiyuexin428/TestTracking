"""Optional local operations for the web console, independent of result analysis."""
from __future__ import annotations

import copy
import hashlib
import ipaddress
import json
import math
import os
import re
import tempfile
import threading
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit, urlunsplit
from urllib.request import ProxyHandler, Request, build_opener

from .config import ServiceConfig


class ManagementError(Exception):
    def __init__(self, message: str, status: int = 400, *, details: dict[str, Any] | None = None):
        super().__init__(message)
        self.status = status
        self.details = details or {}


_MASK = "***"
_PASSWORD = re.compile(r"(?i)(\bpassword\s*=\s*)('(?:\\.|[^'])*'|[^\s]*)")
_CONNECTION_PART = re.compile(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*('(?:\\.|[^'\\])*'|(?:\\.|[^\s'\\])*)")
_SECRET_KEYS = {"password", "passwd", "api_key", "apikey", "access_token", "token", "secret"}
_PORT_KEYS = {"port"}
_POSITIVE_KEYS = {"task_size", "task_threads", "heartbeat_interval_ms", "poll_timeout_ms",
                  "client_timeout_ms", "register_timeout_ms", "register_max_attempts",
                  "request_backoff_initial_ms", "request_backoff_max_ms", "request_timeout_ms",
                  "report_retry_ms", "timeout_seconds"}
_BOOLEAN_KEYS = {"enabled", "dry_run", "store_cov"}
_STRING_KEYS = {"host", "db_conn", "endpoint", "router_endpoint", "worker_id", "dll",
                "stp_manifest", "expected_program_version", "expected_dataset_version",
                "program_version", "dataset_version", "store_type", "store_dir", "table",
                "result_table", "stp_result_table", "program_dir", "program_exe", "program_cmd",
                "target_exe", "stp_root", "opencppcoverage", "work_dir", "export_dir", "log_file",
                "info_file", "info_new_file", "command", "prompt_mode", "management_endpoint"}
_PATH_KEYS = {"workspace", "static_dir", "plugin_views_dir", "plugin_adapters_dir",
              "analysis_scripts_dir", "analysis_reports_dir", "report_templates_dir",
              "artifact_store_dir", "scheduler_config_path", "client_config_path", "logs_dir", "cwd"}
_LABELS = {
    "server.router_endpoint": ("任务服务监听地址", "客户端连接此地址对应的服务器 IP 和端口。"),
    "server.client_timeout_ms": ("客户端超时（毫秒）", "超过此时间未收到心跳，回收客户端任务。"),
    "server.task_lease_timeout_ms": ("任务租约（毫秒）", "0 表示禁用独立任务租约，仍保留心跳恢复。"),
    "server.management_endpoint": ("本机管理地址", "必须与结果服务 scheduler_management_url 一致，例如 http://127.0.0.1:5556。"),
    "scheduler.endpoint": ("服务端地址", "客户端连接的 ZeroMQ 地址，如 tcp://192.168.1.10:5555。"),
    "client.worker_id": ("客户端标识", "每台运行中的客户端必须使用不同标识。"),
    "client.task_threads": ("每插件并行任务数", "每个启用插件分别使用此数量的执行槽。"),
    "client.execution_windows": ("任务执行时间窗", "只在时间窗内领取新任务，已开始任务继续完成。"),
    "client.update_windows": ("部署更新时间窗", "供客户端制品更新使用，与任务执行时间窗独立。"),
    "host": ("网页监听地址", "0.0.0.0 允许局域网访问。"),
    "port": ("网页端口", "修改后需重启结果服务并访问新端口。"),
    "scheduler_management_url": ("本机调度管理地址", "仅接受本机 HTTP 地址；调度器离线不影响数据库分析。"),
    "scheduler_config_path": ("调度器配置文件", "相对路径以结果服务配置文件所在目录为基准。"),
    "client_config_path": ("本机客户端配置或模板", "仅修改此文件，不会远程修改已部署客户端。"),
    "logs_dir": ("日志目录", "页面只能读取此目录下的日志文件。"),
    "providers.dsh.command": ("DSH 命令", "服务器上 dsh 命令或 dsh.cmd 的完整路径；助手固定启动 DSH 原生 Web 界面。"),
    "providers.dsh.cwd": ("DSH 工作目录", "留空沿用结果服务工作目录；所有路径属于服务器，修改后重启结果服务生效。"),
    "providers.dsh.args": ("旧版命令参数", "为兼容旧配置保留；原生界面固定使用 web profile，不读取此字段。"),
    "providers.dsh.prompt_mode": ("旧版提示词模式", "为兼容旧配置保留；原生界面不读取此字段。"),
}


def _revision(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _secret_key(key: str) -> bool:
    key = key.lower()
    return key in _SECRET_KEYS or key.endswith(("_password", "_api_key", "_token", "_secret"))


def _mask_connection(value: str) -> str:
    if value.startswith(("postgres://", "postgresql://")):
        parsed = urlsplit(value)
        if parsed.password is not None:
            credentials, separator, address = parsed.netloc.rpartition("@")
            user, _, _password = credentials.partition(":")
            return urlunsplit(parsed._replace(netloc=f"{user}:{_MASK}{separator}{address}"))
    return _PASSWORD.sub(lambda match: match.group(1) + "'" + _MASK + "'", value)


def _validate_connection(value: str, path: str) -> None:
    if not value.strip():
        return
    if value.startswith(("postgres://", "postgresql://")):
        try:
            parsed = urlsplit(value)
            port = parsed.port
            if not parsed.hostname and not parsed.path and not parsed.query:
                raise ValueError("empty connection")
            if port is not None and not 1 <= port <= 65535:
                raise ValueError("invalid port")
        except ValueError as exc:
            raise ManagementError(f"{path} 必须是有效的 PostgreSQL 连接串") from exc
        return
    position = 0
    while position < len(value):
        match = _CONNECTION_PART.match(value, position)
        if match is None:
            if not value[position:].strip():
                return
            raise ManagementError(f"{path} 必须是 PostgreSQL key=value 连接串或 postgres URI")
        position = match.end()
        if position < len(value) and not value[position].isspace():
            raise ManagementError(f"{path} 连接串参数之间必须使用空格")


def _masked(value: Any, key: str = "") -> Any:
    if isinstance(value, dict):
        return {name: _masked(item, name) for name, item in value.items()}
    if isinstance(value, list):
        return [_masked(item, key) for item in value]
    if isinstance(value, str):
        if _secret_key(key) and value:
            return _MASK
        if key == "db_conn":
            return _mask_connection(value)
    return value


def _comparable_configuration(value: Any, key: str = "") -> Any:
    """Match the scheduler's runtime redaction without exposing its secrets."""
    lowered = key.lower()
    if lowered == "db_conn" or any(part in lowered for part in ("password", "secret", "token", "api_key")):
        return "[redacted]"
    if isinstance(value, dict):
        return {name: _comparable_configuration(item, name) for name, item in value.items()}
    if isinstance(value, list):
        return [_comparable_configuration(item) for item in value]
    return value


def _restore(value: Any, old: Any, key: str = "") -> Any:
    if isinstance(value, dict):
        previous = old if isinstance(old, dict) else {}
        return {name: _restore(item, previous.get(name), name) for name, item in value.items()}
    if isinstance(value, list):
        previous = old if isinstance(old, list) else []
        return [_restore(item, previous[index] if index < len(previous) else None, key)
                for index, item in enumerate(value)]
    if not isinstance(value, str) or not isinstance(old, str):
        return value
    if _secret_key(key) and value == _MASK:
        return old
    if key == "db_conn":
        if value == _mask_connection(old):
            return old
        if value.startswith(("postgres://", "postgresql://")):
            parsed, previous = urlsplit(value), urlsplit(old)
            if parsed.password == _MASK and previous.password is not None:
                credentials, separator, address = parsed.netloc.rpartition("@")
                user, _, _password = credentials.partition(":")
                return urlunsplit(parsed._replace(netloc=f"{user}:{previous.password}{separator}{address}"))
        previous_password = _PASSWORD.search(old)
        if previous_password:
            return _PASSWORD.sub(lambda match: match.group(1) + (
                previous_password.group(2) if match.group(2).strip("'") == _MASK else match.group(2)), value)
    return value


def _local_management_url(value: Any) -> str:
    if value == "":
        return ""
    if not isinstance(value, str):
        raise ManagementError("scheduler_management_url 必须是字符串")
    try:
        parsed = urlsplit(value)
        host = parsed.hostname or ""
        loopback = host.lower() == "localhost" or ipaddress.ip_address(host).is_loopback
        valid = (parsed.scheme == "http" and loopback and parsed.port is not None
                 and 1 <= parsed.port <= 65535 and parsed.path in {"", "/"}
                 and not parsed.username and not parsed.password and not parsed.query and not parsed.fragment)
    except ValueError:
        valid = False
    if not valid:
        raise ManagementError("scheduler_management_url 必须为带端口的本机 HTTP 地址，例如 http://127.0.0.1:5556")
    return value.rstrip("/")


def _validate(kind: str, value: dict[str, Any]) -> None:
    def validate_windows(settings: dict[str, Any], key: str) -> None:
        windows = settings.get(key, [])
        if not isinstance(windows, list):
            raise ManagementError(f"client.{key} 必须是数组")
        for index, window in enumerate(windows):
            path = f"client.{key}[{index}]"
            if not isinstance(window, dict):
                raise ManagementError(f"{path} 必须是对象")
            for name in ("start", "end"):
                if not isinstance(window.get(name), str) or not re.fullmatch(
                        r"(?:[01][0-9]|2[0-3]):[0-5][0-9]", window[name]):
                    raise ManagementError(f"{path}.{name} 必须是 HH:MM")
            days = window.get("days", [])
            if (not isinstance(days, list) or any(isinstance(day, bool) or not isinstance(day, int)
                                                   or day < 1 or day > 7 for day in days)):
                raise ManagementError(f"{path}.days 必须是 1（周一）到 7（周日）的整数数组")

    def walk(item: Any, path: str = "") -> None:
        if not isinstance(item, dict):
            return
        for key, child in item.items():
            here = f"{path}.{key}" if path else key
            if key in _BOOLEAN_KEYS and not isinstance(child, bool):
                raise ManagementError(f"{here} 必须是 true 或 false")
            if key in _PORT_KEYS | _POSITIVE_KEYS | {"task_lease_timeout_ms"}:
                integer = key != "timeout_seconds"
                if (isinstance(child, bool) or not isinstance(child, (int, float))
                        or not math.isfinite(child) or (integer and not isinstance(child, int))):
                    raise ManagementError(f"{here} 必须是{'整数' if integer else '数字'}")
                minimum = 0 if key in {"task_lease_timeout_ms", "client_timeout_ms"} else 1
                if child < minimum or (key in _PORT_KEYS and child > 65535):
                    raise ManagementError(f"{here} 必须在 {minimum} 到 65535 之间" if key in _PORT_KEYS
                                          else f"{here} 必须大于等于 {minimum}")
            if key in _STRING_KEYS and not isinstance(child, str):
                raise ManagementError(f"{here} 必须是字符串")
            if key in _PATH_KEYS and child is not None and not isinstance(child, str):
                raise ManagementError(f"{here} 必须是路径字符串")
            if key in {"db_conn", "worker_id", "endpoint", "router_endpoint", "command", "dll"}:
                if isinstance(child, str) and not child.strip():
                    # A result-only instance can legitimately be configured before its database exists.
                    if not (kind == "result_service" and key == "db_conn"):
                        raise ManagementError(f"{here} 不能为空")
            if key == "db_conn" and isinstance(child, str):
                _validate_connection(child, here)
            if key in {"endpoint", "router_endpoint"} and isinstance(child, str):
                if not re.fullmatch(r"tcp://[^\s]+:[1-9][0-9]{0,4}", child):
                    raise ManagementError(f"{here} 必须是 tcp://地址:端口")
                if int(child.rsplit(":", 1)[1]) > 65535:
                    raise ManagementError(f"{here} 端口必须在 1 到 65535 之间")
            if key in {"args", "engine_dlls"} and (
                    not isinstance(child, list) or not all(isinstance(part, str) for part in child)):
                raise ManagementError(f"{here} 必须是字符串数组")
            if key == "artifact_import_roots" and (
                    not isinstance(child, list) or not all(
                        isinstance(part, str) and part.strip() for part in child)):
                raise ManagementError(f"{here} 必须是非空路径字符串数组")
            if key == "prompt_mode" and child not in {"stdin", "argument"}:
                raise ManagementError(f"{here} 必须是 stdin 或 argument")
            walk(child, here)
    walk(value)
    if kind in {"scheduler", "client"}:
        required = ("server", "plugins") if kind == "scheduler" else ("scheduler", "client", "plugins")
        for key in required:
            if not isinstance(value.get(key), dict):
                raise ManagementError(f"{key} 必须是 JSON 对象")
        for name, plugin in value["plugins"].items():
            if not isinstance(plugin, dict):
                raise ManagementError(f"plugins.{name} 必须是 JSON 对象")
            if plugin.get("enabled") and not plugin.get("dll"):
                raise ManagementError(f"plugins.{name}.dll 不能为空")
        if kind == "client":
            if (not value["client"].get("deployment_endpoint") and
                    not any(item.get("enabled", False) is True for item in value["plugins"].values())):
                raise ManagementError("客户端至少启用一个执行插件")
            settings = value["client"]
            validate_windows(settings, "execution_windows")
            validate_windows(settings, "update_windows")
            if settings.get("request_backoff_initial_ms", 200) > settings.get("request_backoff_max_ms", 30000):
                raise ManagementError("request_backoff_initial_ms 不能大于 request_backoff_max_ms")
        elif "management_endpoint" in value["server"]:
            configured_endpoint = value["server"]["management_endpoint"]
            endpoint = "" if configured_endpoint.rstrip("/") == "http://127.0.0.1:0" else _local_management_url(configured_endpoint)
            if endpoint and urlsplit(endpoint).hostname != "127.0.0.1":
                raise ManagementError("server.management_endpoint 必须使用 http://127.0.0.1:端口")
    else:
        _local_management_url(value.get("scheduler_management_url", "http://127.0.0.1:5556"))
        if not isinstance(value.get("providers", {}), dict):
            raise ManagementError("providers 必须是 JSON 对象")
        try:
            ServiceConfig._merge(ServiceConfig.defaults(), value)
        except (ValueError, TypeError, AttributeError) as exc:
            raise ManagementError(str(exc)) from exc


def _fields(config: dict[str, Any], prefix: str = "") -> list[dict[str, str]]:
    result = []
    for key, value in config.items():
        path = f"{prefix}.{key}" if prefix else key
        if isinstance(value, dict):
            result.extend(_fields(value, path))
            continue
        label, description = _LABELS.get(path, (key, "修改后重启对应进程生效。"))
        if key == "db_conn":
            label, description = "数据库连接", "PostgreSQL 连接串；密码以 *** 隐藏，保持占位符可保留原密码。"
        elif key == "task_size":
            label, description = "每任务文件数", "影响新运行的任务分组，不改变数据库中已保存的历史结果。"
        elif key in {"dry_run", "target_exe", "program_exe", "opencppcoverage", "stp_root", "stp_manifest"}:
            description = "执行插件配置；客户端路径必须指向该客户端机器上的程序或全量数据。"
        value_type = ("boolean" if isinstance(value, bool) else "number" if isinstance(value, (int, float))
                      else "array" if isinstance(value, list) else "string")
        result.append({"path": path, "label": label, "type": value_type, "description": description})
    return result


class ManagementService:
    def __init__(self, config: ServiceConfig):
        self.config = config
        self._url = _local_management_url(config.scheduler_management_url)
        self._paths = {"scheduler": config.scheduler_config_path, "result_service": config.config_path,
                       "client": config.client_config_path}
        self._initial: dict[str, str | None] = {}
        self._pending_scheduler_instance: str | None = None
        self._pending_scheduler_revision: str | None = None
        self._scheduler_save_pending = False
        scheduler_path = self._path("scheduler")
        self._scheduler_state_path = (scheduler_path.with_name(scheduler_path.name + ".management-state.json")
                                      if scheduler_path else None)
        self._lock = threading.RLock()
        for kind, location in self._paths.items():
            try:
                self._initial[kind] = _revision(Path(location).read_bytes()) if location else None
            except OSError:
                self._initial[kind] = None
        if self._scheduler_state_path:
            try:
                state = json.loads(self._scheduler_state_path.read_text(encoding="utf-8"))
                if isinstance(state, dict) and isinstance(state.get("revision"), str):
                    self._pending_scheduler_instance = state.get("instance_id")
                    self._pending_scheduler_revision = state["revision"]
                    self._scheduler_save_pending = True
            except (OSError, ValueError):
                pass

    def _request(self, route: str, body: dict[str, Any] | None = None) -> dict[str, Any]:
        if not self._url:
            raise ManagementError("未配置本机调度管理地址", 503)
        data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
        request = Request(self._url + route, data=data, headers={"Content-Type": "application/json"},
                          method="POST" if body is not None else "GET")
        try:
            with build_opener(ProxyHandler({})).open(request, timeout=35 if body is not None else 3) as response:
                result = json.loads(response.read(4 * 1024 * 1024).decode("utf-8"))
            if not isinstance(result, dict):
                raise ManagementError("调度器返回了无效响应", 502)
            return result
        except HTTPError as exc:
            details = {}
            try:
                rejection = json.loads(exc.read(65536).decode("utf-8"))
                error = rejection.get("error", str(exc)) if isinstance(rejection, dict) else str(exc)
                # Keep the duplicate-work contract through the local proxy.
                # Other upstream error fields are not public console metadata.
                if isinstance(rejection, dict) and rejection.get("duplicate") is True:
                    details["duplicate"] = True
                    for key in ("run_id", "status"):
                        if isinstance(rejection.get(key), str):
                            details[key] = rejection[key]
                    if isinstance(rejection.get("force_rerun_available"), bool):
                        details["force_rerun_available"] = rejection["force_rerun_available"]
                if isinstance(rejection, dict) and isinstance(rejection.get("unavailable"), list):
                    details["unavailable"] = [
                        {key: item[key] for key in ("client_id", "reason") if isinstance(item.get(key), str)}
                        for item in rejection["unavailable"] if isinstance(item, dict)]
            except (ValueError, UnicodeError):
                error = str(exc)
            raise ManagementError(str(error), exc.code if 400 <= exc.code < 600 else 502,
                                  details=details) from exc
        except (URLError, OSError, TimeoutError) as exc:
            raise ManagementError("调度服务不可用，请检查调度进程及本机管理端口", 503) from exc
        except (ValueError, UnicodeError) as exc:
            raise ManagementError("调度器返回了无效 JSON", 502) from exc

    def get_status(self) -> dict[str, Any]:
        try:
            result = self._request("/status")
            return {"online": True, "clients": [], "agents": [], "plugins": [], "active_runs": [],
                    "queued_runs": [], "in_flight": 0,
                    **result}
        except ManagementError as exc:
            return {"online": False, "reason": str(exc), "clients": [], "agents": [], "plugins": [],
                    "active_runs": [], "queued_runs": [], "in_flight": 0}

    def client_control(self, action: str, **values: Any) -> dict[str, Any]:
        if action not in {"agent_heartbeat", "reserve_clients", "commit_reservation", "release_clients"}:
            raise ManagementError("unknown client control action")
        result = self._request("/commands", {**values, "action": action})
        if result.get("ok") is not True:
            raise ManagementError(result.get("error", "客户端管理操作失败"), 409,
                                  details={"unavailable": result.get("unavailable", [])})
        return result

    def command(self, body: dict[str, Any]) -> dict[str, Any]:
        # Installation/binding and UI task submission share the same lock.
        with self._lock:
            if isinstance(body, dict) and body.get("action") == "run":
                path = self._path("scheduler")
                if path and path.is_file() and self.get_config("scheduler")["restart_required"]:
                    raise ManagementError("服务端配置已修改，请重启调度器使新配置生效后再创建任务", 409)
            return self._command(body)

    def _command(self, body: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(body, dict):
            raise ManagementError("命令必须是 JSON 对象")
        action = body.get("action")
        if not isinstance(action, str) or action not in {"run", "stop", "wakeup"}:
            raise ManagementError("action 必须是 run、stop 或 wakeup")
        plugin = body.get("plugin_type", "")
        if not isinstance(plugin, str) or (action != "wakeup" and not plugin.strip()):
            raise ManagementError("plugin_type 不能为空")
        payload = {"action": action, "plugin_type": plugin.strip()}
        if action != "run" and "worker_ids" in body:
            raise ManagementError("worker_ids 仅适用于 run 命令")
        if action == "run":
            version = body.get("dataset_version", "")
            if not isinstance(version, str) or not version.strip():
                raise ManagementError("dataset_version 不能为空")
            params = body.get("params", {})
            if not isinstance(params, (str, dict)):
                raise ManagementError("params 必须是字符串或 JSON 对象")
            payload.update(dataset_version=version.strip(), params=params)
            worker_ids = body.get("worker_ids", [])
            if (not isinstance(worker_ids, list) or any(
                    not isinstance(worker, str) or not worker.strip()
                    or len(worker.encode("utf-8")) > 255
                    or any(ord(character) < 32 or ord(character) == 127 for character in worker)
                    for worker in worker_ids)):
                raise ManagementError("worker_ids 必须是非空客户端标识字符串数组，每项不超过 255 字节")
            payload["worker_ids"] = sorted(set(worker_ids))
            force_rerun = body.get("force_rerun", False)
            if not isinstance(force_rerun, bool):
                raise ManagementError("force_rerun 必须是布尔值")
            reason = body.get("force_rerun_reason", "")
            if not isinstance(reason, str):
                raise ManagementError("force_rerun_reason 必须是字符串")
            if force_rerun and not reason.strip():
                raise ManagementError("强制重跑需要填写原因")
            if not force_rerun and "force_rerun_reason" in body:
                raise ManagementError("force_rerun_reason 需要 force_rerun=true")
            payload["force_rerun"] = force_rerun
            if force_rerun:
                payload["force_rerun_reason"] = reason.strip()
        if "run_id" in body:
            if not isinstance(body["run_id"], str):
                raise ManagementError("run_id 必须是字符串")
            payload["run_id"] = body["run_id"]
        return self._request("/commands", payload)

    def require_plugin_idle(self, plugin: str) -> None:
        """Guard deployment changes against live and durable unfinished work.

        Caller holds _lock through the config write. Offline is permitted only
        when the scheduler's own database confirms there is no recoverable run.
        """
        path = self._path("scheduler")
        if path is None:
            raise ManagementError("未配置服务端配置文件", 409)
        _, config = self._read_config(path)
        status = self.get_status()
        loaded = status.get("config_path")
        if status.get("online") and loaded and Path(loaded).resolve() != path:
            raise ManagementError("在线调度器加载的是另一份配置，无法安全发布插件", 409)
        for collection in ("active_runs", "queued_runs", "unfinished_runs"):
            for run in status.get(collection, []):
                if run.get("plugin_type") == plugin:
                    raise ManagementError(f"插件 {plugin} 还有未完成或排队任务，请先按原配置完成任务", 409)
        connection = config.get("server", {}).get("db_conn", "")
        if connection:
            try:
                import psycopg
                with psycopg.connect(connection, connect_timeout=5, autocommit=True) as database:
                    database.execute("SET statement_timeout = '5s'")
                    exists = database.execute("SELECT to_regclass('runs')").fetchone()[0]
                    if exists:
                        run = database.execute(
                            "SELECT run_id FROM runs WHERE plugin_type=%s "
                            "AND status IN ('queued','planning','running','stopped') LIMIT 1", (plugin,),
                        ).fetchone()
                        if run:
                            raise ManagementError(f"插件 {plugin} 有待恢复任务 {run[0]}，请先按原配置完成", 409)
            except ManagementError:
                raise
            except Exception as exc:
                raise ManagementError("无法核对调度数据库中的未完成任务，暂不更改部署配置", 503) from exc
        elif not status.get("online") or not isinstance(status.get("unfinished_runs"), list):
            raise ManagementError("无法确认插件是否存在待恢复任务，请启动调度器或配置其数据库后再发布", 503)

    def _path(self, kind: str) -> Path | None:
        if kind not in self._paths:
            raise ManagementError("未知配置类别", 404)
        location = self._paths[kind]
        return Path(location).resolve() if location else None

    def _read_config(self, path: Path) -> tuple[bytes, dict[str, Any]]:
        try:
            data = path.read_bytes()
            parsed = json.loads(data.decode("utf-8-sig"))
        except OSError as exc:
            raise ManagementError("配置文件不可读取", 404) from exc
        except (ValueError, UnicodeError) as exc:
            raise ManagementError("配置文件不是有效 JSON", 422) from exc
        if not isinstance(parsed, dict):
            raise ManagementError("配置文件必须是 JSON 对象", 422)
        return data, parsed

    def get_config(self, kind: str) -> dict[str, Any]:
        with self._lock:
            path = self._path(kind)
            notice = {"scheduler": "保存后重启调度器生效；正在运行的任务会按已有恢复机制恢复。",
                      "result_service": "保存后重启结果服务生效；当前页面继续使用进程启动时的配置。",
                      "client": "仅修改本机客户端配置或部署模板。需重启对应客户端；不会修改其他机器的客户端。"}[kind]
            if path is None or not path.is_file():
                return {"kind": kind, "available": False, "config": None, "revision": None, "fields": [],
                        "path": str(path) if path else None, "effective_values": None, "restart_required": False,
                        "notice": notice, "warnings": [],
                        "reason": "未配置可编辑文件" if path is None else "配置文件不存在"}
            data, parsed = self._read_config(path)
            revision = _revision(data)
            effective = _masked(asdict(self.config)) if kind == "result_service" else None
            restart_required = revision != self._initial.get(kind)
            warnings = self._configuration_warnings(kind, parsed)
            if kind == "scheduler":
                status = self.get_status()
                pending = self._scheduler_save_pending and revision == self._pending_scheduler_revision
                restart_required = restart_required or pending
                if status.get("online") and isinstance(status.get("configuration"), dict):
                    effective = _masked(status["configuration"])
                    compared = {name: parsed[name] for name in ("server", "plugins") if name in parsed}
                    restart_required = _comparable_configuration(compared) != _comparable_configuration(effective)
                    loaded_path = status.get("config_path")
                    matching_path = not loaded_path or Path(loaded_path).resolve() == path
                    if pending:
                        current_instance = status.get("instance_id")
                        if current_instance and current_instance != self._pending_scheduler_instance and matching_path:
                            try:
                                self._scheduler_state_path.unlink(missing_ok=True)
                                self._scheduler_save_pending = False
                            except OSError:
                                warnings.append("调度器已重启，但配置生效标记暂时无法清理。")
                        else:
                            restart_required = True
                    if loaded_path and Path(loaded_path).resolve() != path:
                        warnings.append("当前在线调度器加载的是另一份配置文件：" + str(loaded_path) +
                                        "。保存本文件不会修改该进程的配置，请按本文件路径重新启动调度器。")
                else:
                    notice += " 当前调度器离线，无法确认实际加载的配置。"
            return {"kind": kind, "available": True, "path": str(path), "config": _masked(parsed),
                    "revision": revision, "fields": _fields(parsed), "effective_values": effective,
                    "restart_required": restart_required, "notice": notice,
                    "warnings": warnings}

    def _configuration_warnings(self, kind: str, parsed: dict[str, Any]) -> list[str]:
        if kind not in {"scheduler", "result_service"}:
            return []
        counterpart = "result_service" if kind == "scheduler" else "scheduler"
        other_path = self._path(counterpart)
        if other_path is None or not other_path.is_file():
            return []
        try:
            _, other = self._read_config(other_path)
            scheduler = parsed if kind == "scheduler" else other
            result = other if kind == "scheduler" else parsed
            endpoint = scheduler.get("server", {}).get("management_endpoint", "")
            upstream = result.get("scheduler_management_url", "http://127.0.0.1:5556")
            if endpoint.rstrip("/") != upstream.rstrip("/"):
                return ["调度器 server.management_endpoint 与结果服务 scheduler_management_url 不一致，"
                        "请同步修改并重启两者，客户端状态和任务管理才能连接。"]
        except (ManagementError, AttributeError):
            return ["无法核对调度器与结果服务的管理地址，请检查另一份配置文件。"]
        return []

    def save_config(self, kind: str, body: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            path = self._path(kind)
            if path is None:
                raise ManagementError("未配置可编辑文件", 404)
            if not isinstance(body, dict) or not isinstance(body.get("config"), dict):
                raise ManagementError("config 必须是 JSON 对象")
            data, previous = self._read_config(path)
            if body.get("revision") != _revision(data):
                raise ManagementError("配置已被其他操作修改，请重新加载后保存", 409)
            proposed = _restore(copy.deepcopy(body["config"]), previous)
            _validate(kind, proposed)
            try:
                encoded = (json.dumps(proposed, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
            except (ValueError, TypeError) as exc:
                raise ManagementError("配置包含无效 JSON 值") from exc
            if proposed != previous:
                scheduler_instance = self.get_status().get("instance_id") if kind == "scheduler" else None
                try:
                    self._atomic_write(path.with_name(path.name + ".bak"), data)
                    if kind == "scheduler":
                        # Persist before replacing the config. A result-service
                        # restart must not forget hidden connection/password
                        # changes while the same scheduler instance still runs.
                        marker = {"instance_id": scheduler_instance, "revision": _revision(encoded)}
                        self._atomic_write(self._scheduler_state_path, json.dumps(marker).encode("utf-8"))
                    self._atomic_write(path, encoded)
                except OSError as exc:
                    raise ManagementError("配置保存失败，请检查文件写入权限", 500) from exc
                if kind == "scheduler":
                    self._pending_scheduler_instance = scheduler_instance
                    self._pending_scheduler_revision = _revision(encoded)
                    self._scheduler_save_pending = True
            result = self.get_config(kind)
            result["saved"] = True
            return result

    @staticmethod
    def _atomic_write(path: Path, data: bytes) -> None:
        descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
        try:
            with os.fdopen(descriptor, "wb") as handle:
                handle.write(data)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, path)
        finally:
            Path(temporary).unlink(missing_ok=True)

    def logs(self, query: dict[str, Any]) -> dict[str, Any]:
        def parameter(name: str, default: str = "") -> str:
            raw = query.get(name, default)
            return str(raw[0] if isinstance(raw, list) and raw else raw)
        try:
            limit = int(parameter("limit", "500"))
        except ValueError as exc:
            raise ManagementError("limit 必须是整数") from exc
        if not 1 <= limit <= 2000:
            raise ManagementError("limit 必须在 1 到 2000 之间")
        root = Path(self.config.logs_dir).resolve() if self.config.logs_dir else None
        files: dict[str, Path] = {}
        metadata = []
        if root and root.is_dir():
            for candidate in root.iterdir():
                path = candidate.resolve()
                if (path.is_file() and path.is_relative_to(root)
                        and candidate.suffix.lower() in {".log", ".txt", ".out", ".err"}):
                    try:
                        stat = path.stat()
                    except OSError:
                        continue
                    files[candidate.name] = path
                    metadata.append({"name": candidate.name, "size": stat.st_size,
                                     "modified_at": datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat()})
        metadata.sort(key=lambda item: item["modified_at"], reverse=True)
        selected = parameter("file") or ("scheduler.log" if "scheduler.log" in files else
                                         metadata[0]["name"] if metadata else None)
        if selected is not None and selected not in files:
            raise ManagementError("日志文件不存在或不在已配置日志目录内", 404)
        lines: list[str] = []
        truncated = False
        if selected:
            try:
                with files[selected].open("rb") as handle:
                    handle.seek(0, os.SEEK_END)
                    length = handle.tell()
                    start = max(0, length - 2 * 1024 * 1024)
                    handle.seek(start)
                    if start:
                        handle.readline()  # Discard an incomplete first line.
                    lines = handle.read(2 * 1024 * 1024).decode("utf-8", errors="replace").splitlines()
                    truncated = start > 0
            except OSError as exc:
                raise ManagementError("日志文件暂时不可读取", 503) from exc
            level, search = parameter("level").upper(), parameter("search").casefold()
            if level and level not in {"ALL", "DEBUG", "INFO", "WARN", "WARNING", "ERROR", "FATAL"}:
                raise ManagementError("未知日志级别")
            if level and level != "ALL":
                match_level = "WARN(?:ING)?" if level in {"WARN", "WARNING"} else level
                lines = [line for line in lines if re.search(r"\b" + match_level + r"\b", line, re.IGNORECASE)]
            if search:
                lines = [line for line in lines if search in line.casefold()]
            truncated = truncated or len(lines) > limit
            lines = lines[-limit:]
        return {"files": metadata, "selected": selected, "lines": lines, "truncated": truncated,
                "configured": root is not None, "limit": limit}
