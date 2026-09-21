from __future__ import annotations

import json
import logging
import mimetypes
import shutil
import socket
from dataclasses import replace
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, quote, unquote, urlparse

from .artifacts import ArtifactError, ArtifactStore
from .config import ServiceConfig
from .deployments import DeploymentError, DeploymentStore
from .dsh_proxy import DSH_PROXY_PATH, http_origin, proxy_request
from .management import ManagementError, ManagementService
from .plugins import (PluginAIContextStore, ResultPluginError, default_plugins,
                      save_versioned_plugin_view, versioned_plugin_view)
from .providers import DshWebManager, ProviderError, canonical_operator_id
from .repository import create_repository
from .reports import ReportError, ReportStore
from .server_plugins import ServerPluginInstaller
from .views import PluginViewError, PluginViewStore

LOG = logging.getLogger("result_service")


def _plugin_resource_name(path: str, resource: str) -> str | None:
    parts = path.strip("/").split("/")
    if len(parts) == 4 and parts[:2] == ["api", "plugins"] and parts[3] == resource:
        return parts[2]
    return None


def _view_plugin_name(path: str) -> str | None:
    return _plugin_resource_name(path, "view")


def _dsh_proxy_route(path: str) -> tuple[str, str] | None:
    if not path.startswith(DSH_PROXY_PATH):
        return None
    operator, _, _ = path[len(DSH_PROXY_PATH):].partition("/")
    if not operator:
        raise ProviderError("operator_id is required in the DSH Web path")
    normalized = canonical_operator_id(operator)
    return normalized, f"{DSH_PROXY_PATH}{normalized}/"


REPORT_CONTRACT = {
    "entrypoint": "render(connection, context) -> str",
    "connection": "psycopg PostgreSQL connection, dict rows, read-only transaction",
    "context": {"plugin": "plugin name", "run_id": "selected durable run ID",
                "options": "plugin options", "parameters": "user-supplied object"},
    "output": "Standalone HTML with inline CSS/JavaScript; offline interactive charts and tables. No fixed widget schema.",
    "dependencies": "Python standard library and psycopg are bundled; other libraries must be installed in the service Python runtime.",
    "data": "Read durable database results only. Do not depend on scheduler processes, tasks/checkpoints, client files, or executor code. Distinguish raw results, completeness, and optional settled summaries.",
}


def _json_default(value: Any) -> Any:
    if hasattr(value, "isoformat"):
        return value.isoformat()
    if isinstance(value, (bytes, bytearray)):
        return "<binary>"
    return str(value)


class ResultService:
    def __init__(self, config: ServiceConfig):
        self.config = config
        self.management = ManagementService(config)
        runtime = Path(config.workspace or Path(__file__).resolve().parents[2]) / "runtime" / "result_service"
        self.artifacts = ArtifactStore(
            config.artifact_store_dir or runtime / "artifacts", config.artifact_import_roots
        )
        installer = ServerPluginInstaller(config, self.artifacts, self.management)
        if installer.recovered_configuration:
            # A crash may have left only one of the two config files updated.
            # Restore them before loading Python adapters or any result resource.
            config = ServiceConfig.from_file(config.config_path)
            self.config = config
            self.management = ManagementService(config)
        self.server_plugins = ServerPluginInstaller(config, self.artifacts, self.management)
        plugin_options = {name: dict(value) for name, value in config.plugin_options.items()}
        if config.stp_result_table and "result_table" not in plugin_options.get("stpcompare", {}):
            plugin_options.setdefault("stpcompare", {})["result_table"] = config.stp_result_table
        self.plugin_options = plugin_options
        self.plugins = default_plugins(
            plugin_options, config.plugin_views_dir, config.plugin_adapters_dir,
            config.report_templates_dir, config.plugin_ai_contexts_dir,
        )
        self.plugin_ai_contexts = PluginAIContextStore(config.plugin_ai_contexts_dir)
        self.plugin_views = PluginViewStore(config.plugin_views_dir)
        self.repository = create_repository(config.db_conn, self.plugins)
        self.deployments = DeploymentStore(self.artifacts.root / "deployments.json", self.artifacts)
        self.reports = ReportStore(
            config.analysis_scripts_dir or runtime / "analysis_scripts",
            config.analysis_reports_dir or runtime / "reports",
            default_scripts_dir=config.report_templates_dir,
        )
        dsh = config.providers.get("dsh")
        self.dsh_web = DshWebManager(replace(dsh, cwd=dsh.cwd or config.workspace)) if dsh else None
        static_dir = config.static_dir or str(Path(__file__).resolve().parents[1] / "web")
        self.static_dir = Path(static_dir).resolve()

    def payload(self, path: str, query: dict[str, list[str]]) -> tuple[int, Any]:
        if path == "/api/scheduler":
            return self.management_request(self.management.get_status)
        if path == "/api/logs":
            return self.management_request(self.management.logs, query)
        if path.startswith("/api/config/"):
            return self.management_request(self.management.get_config, path.removeprefix("/api/config/"))
        if path == "/healthz":
            return 200, {"status": "ok", "service": "result_service"}
        if path == "/readyz":
            ready = self.repository.ready()
            payload = {"status": "ready" if ready else "not_ready",
                       "database_configured": self.repository.configured}
            if not self.repository.configured:
                payload["reason"] = "database_not_configured"
            elif not ready:
                payload["reason"] = "database_unavailable"
            return (200 if ready else 503), payload
        if path == "/api/plugins":
            return 200, [
                {
                    "name": name,
                    "configured": plugin.configured,
                    "view_editable": True,
                    "view": versioned_plugin_view(plugin),
                }
                for name, plugin in sorted(self.plugins.items())
            ]
        if path == "/api/artifacts":
            filters = {key: query[key][0] for key in ("kind", "name", "version", "sha256")
                       if query.get(key)}
            try:
                return 200, {"artifacts": self.artifacts.list(filters)}
            except ArtifactError as exc:
                return exc.status, {"error": str(exc)}
        if path == "/api/deployments":
            try:
                return 200, {"clients": self.deployments.list()}
            except (DeploymentError, ArtifactError) as exc:
                return exc.status, {"error": str(exc)}
        if path.startswith("/api/deployments/"):
            parts = path.removeprefix("/api/deployments/").split("/")
            if len(parts) != 1:
                return 404, {"error": "not found"}
            try:
                return 200, self.deployments.get(unquote(parts[0]))
            except (DeploymentError, ArtifactError) as exc:
                return exc.status, {"error": str(exc)}
        if path.startswith("/api/artifacts/"):
            artifact_id = path.removeprefix("/api/artifacts/")
            artifact = self.artifacts.get(artifact_id)
            return (200, artifact) if artifact else (404, {"error": "artifact not found"})
        if path == "/api/plugin-view-schema":
            return 200, self.plugin_views.schema()
        context_plugin = _plugin_resource_name(path, "ai-context")
        if context_plugin is not None:
            if context_plugin not in self.plugins:
                return 404, {"error": f"unknown plugin: {context_plugin}"}
            try:
                return 200, self.plugin_ai_contexts.load(context_plugin)
            except ResultPluginError as exc:
                return 404, {"error": str(exc)}
        report_plugin = _plugin_resource_name(path, "report-script")
        if report_plugin is not None:
            if report_plugin not in self.plugins:
                return 404, {"error": f"unknown plugin: {report_plugin}"}
            return 200, self.reports.get_script(report_plugin)
        report_plugin = _plugin_resource_name(path, "reports")
        if report_plugin is not None:
            if report_plugin not in self.plugins:
                return 404, {"error": f"unknown plugin: {report_plugin}"}
            return 200, {"latest": self.reports.latest(report_plugin, query.get("run_id", [""])[0])}
        if path == "/api/analysis/catalog":
            return 200, self.analysis_catalog(
                query.get("plugin", [None])[0], int(query.get("limit", ["100"])[0]),
                int(query.get("offset", ["0"])[0]), query.get("search", [""])[0])
        if path.startswith("/api/analysis/runs/"):
            plugin = query.get("plugin", [None])[0]
            if plugin not in self.plugins:
                return 400, {"error": "a known plugin query parameter is required"}
            result = self.repository.analysis_run(unquote(path.rsplit("/", 1)[-1]), plugin)
            return (200, result) if result else (404, {"error": "run not found"})
        plugin_name = _view_plugin_name(path)
        if plugin_name is not None:
            plugin = self.plugins.get(plugin_name)
            if not plugin:
                return 404, {"error": f"unknown plugin: {plugin_name}"}
            return 200, versioned_plugin_view(plugin)
        if path == "/api/runs":
            plugin = query.get("plugin", [None])[0]
            limit = query.get("limit", ["50"])[0]
            return 200, self.repository.list_runs(
                plugin, int(limit), int(query.get("offset", ["0"])[0]),
                query.get("search", [""])[0])
        if path.startswith("/api/runs/"):
            run_id = unquote(path.rsplit("/", 1)[-1])
            plugin = query.get("plugin", [None])[0]
            if not plugin:
                return 400, {"error": "plugin query parameter is required"}
            if plugin not in self.plugins:
                return 400, {"error": f"unknown plugin: {plugin}"}
            result = self.repository.get_run(run_id, plugin)
            return (200, result) if result else (404, {"error": "run not found"})
        if path == "/api/ai/providers":
            return 200, ([{"name": "dsh", "available": self.dsh_web.available()}]
                         if self.dsh_web else [])
        return 404, {"error": "not found"}

    @staticmethod
    def management_request(operation, *args) -> tuple[int, Any]:
        try:
            return 200, operation(*args)
        except ManagementError as exc:
            return exc.status, {**exc.details, "error": str(exc)}

    def analysis_catalog(self, plugin: str | None = None, limit: int = 100,
                         offset: int = 0, search: str = "") -> dict[str, Any]:
        return {"runs": self.repository.analysis_runs(plugin, limit, offset, search),
                "tables": self.repository.database_schema(),
                "database_configured": self.repository.configured,
                "script_contract": REPORT_CONTRACT,
                "plugin_options": self.plugin_options.get(plugin, {}) if plugin else {}}

    def update_report_script(self, plugin: str, body: Any) -> tuple[int, Any]:
        if plugin not in self.plugins:
            return 404, {"error": f"unknown plugin: {plugin}"}
        if (not isinstance(body, dict) or not isinstance(body.get("source"), str)
                or not isinstance(body.get("revision"), str)):
            return 400, {"error": "source and revision must be strings"}
        try:
            operator_id = canonical_operator_id(body.get("operator_id"))
            return 200, self.reports.save_script(
                plugin, body["source"], body["revision"], operator_id
            )
        except ProviderError as exc:
            return 400, {"error": str(exc)}
        except ReportError as exc:
            return exc.status, {"error": str(exc)}

    def generate_report(self, plugin: str, body: Any) -> tuple[int, Any]:
        if plugin not in self.plugins:
            return 404, {"error": f"unknown plugin: {plugin}"}
        if not isinstance(body, dict) or not isinstance(body.get("run_id"), str) or not body["run_id"].strip():
            return 400, {"error": "run_id must be a nonempty string"}
        if not isinstance(body.get("parameters", {}), dict):
            return 400, {"error": "parameters must be an object"}
        if not self.repository.configured:
            return 503, {"error": "database is not configured"}
        try:
            if not self.repository.analysis_run(body["run_id"], plugin, include_summary=False):
                return 404, {"error": "run not found"}
            return 200, self.reports.generate(
                plugin, body["run_id"], self.config.db_conn,
                options=self.plugin_options.get(plugin, {}), parameters=body.get("parameters", {}),
            )
        except ReportError as exc:
            return 422, {"error": str(exc)}

    def register_artifact(self, body: Any) -> tuple[int, Any]:
        if not isinstance(body, dict):
            return 400, {"error": "request body must be an object"}
        try:
            artifact, created = self.artifacts.register(
                body.get("source_path"), kind=body.get("kind"), name=body.get("name"),
                version=body.get("version"), filename=body.get("filename"),
                metadata=body.get("metadata"), expected_sha256=body.get("sha256"),
            )
            return (201 if created else 200), artifact
        except ArtifactError as exc:
            return exc.status, {"error": str(exc)}

    def update_deployment(self, path: str, body: Any) -> tuple[int, Any]:
        parts = path.removeprefix("/api/deployments/").split("/")
        if len(parts) != 2 or parts[1] not in {"desired", "reported"}:
            return 404, {"error": "not found"}
        try:
            operation = (self.deployments.put_desired if parts[1] == "desired"
                         else self.deployments.put_reported)
            value, _changed = operation(unquote(parts[0]), body)
            return 200, value
        except (DeploymentError, ArtifactError) as exc:
            result = {"error": str(exc)}
            if isinstance(exc, DeploymentError) and exc.status == 409:
                result["current_revision"] = exc.current_revision
            return exc.status, result

    def preview_deployment(self, body: Any) -> dict[str, Any]:
        if not isinstance(body, dict):
            raise ManagementError("request body must be an object")
        try:
            prepared = self.deployments.prepare(body.get("components"), body.get("config_patch", {}))
        except (DeploymentError, ArtifactError) as exc:
            raise ManagementError(str(exc), exc.status) from exc
        scheduler = self.management.get_config("scheduler")
        warnings = []
        if not prepared["program_bindings"]:
            warnings.append("没有可绑定的内置插件程序制品；本机路径和自定义插件仍使用原有版本配置。")
        if not scheduler["available"]:
            warnings.append("尚未配置可编辑的服务端配置文件，不能自动保存程序匹配配置。")
        for binding in prepared["program_bindings"]:
            if binding["plugin"] not in (scheduler.get("config") or {}).get("plugins", {}):
                warnings.append(f"服务端尚未配置插件 {binding['plugin']}，请先安装配套服务端插件。")
        warnings.append("绑定使用制品内容摘要及执行路径、参数；包外依赖的内容仍需自行保持一致。")
        return {**prepared, "scheduler_revision": scheduler["revision"],
                "scheduler_restart_required": scheduler["restart_required"], "warnings": warnings}

    def bind_deployment_scheduler(self, body: Any) -> dict[str, Any]:
        # Serialize against config edits, plugin installation and UI task creation.
        with self.management._lock:
            prepared = self.preview_deployment(body)
            if body.get("scheduler_revision") != prepared["scheduler_revision"]:
                raise ManagementError("服务端配置已改变，请重新预览后再保存", 409)
            expected = prepared["scheduler_plugins"]
            if not expected:
                raise ManagementError("请先选择程序制品并通过配置向导生成执行路径", 400)
            scheduler = self.management.get_config("scheduler")
            if not scheduler["available"]:
                raise ManagementError("未配置可编辑的服务端配置文件", 409)
            config = scheduler["config"]
            for plugin, fields in expected.items():
                current = config.get("plugins", {}).get(plugin)
                if not isinstance(current, dict):
                    raise ManagementError(f"请先安装并配置服务端插件 {plugin}", 409)
                if any(current.get(key) != value for key, value in fields.items()):
                    self.management.require_plugin_idle(plugin)
                    current.update(fields)
            saved = self.management.save_config("scheduler", {
                "revision": scheduler["revision"], "config": config})
            return {"scheduler_revision": saved["revision"],
                    "scheduler_restart_required": saved["restart_required"],
                    "config": saved, "scheduler_plugins": expected,
                    "program_bindings": prepared["program_bindings"]}

    def deployment_operation(self, path: str, body: Any) -> tuple[int, Any]:
        if path == "/api/deployments/preview":
            return self.management_request(self.preview_deployment, body)
        if path == "/api/deployments/bind-scheduler":
            return self.management_request(self.bind_deployment_scheduler, body)
        if path in {"/api/server-plugins/preview", "/api/server-plugins/install"}:
            def install_operation():
                installer = ServerPluginInstaller(self.config, self.artifacts, self.management)
                operation = installer.preview if path.endswith("/preview") else installer.install
                return operation(body)
            return self.management_request(install_operation)
        return 404, {"error": "not found"}

    def update_plugin_view(self, path: str, body: Any) -> tuple[int, Any]:
        plugin_name = _view_plugin_name(path)
        if plugin_name is None:
            return 404, {"error": "not found"}
        plugin = self.plugins.get(plugin_name)
        if not plugin:
            return 404, {"error": f"unknown plugin: {plugin_name}"}
        try:
            return 200, save_versioned_plugin_view(plugin, body)
        except PluginViewError as exc:
            payload = {"error": str(exc)}
            if exc.current_revision is not None:
                payload["current_revision"] = exc.current_revision
            return exc.status, payload

    def start_dsh_web(self, operator_id: Any) -> tuple[int, Any]:
        if not self.dsh_web:
            return 404, {"error": "dsh provider is not configured"}
        try:
            operator = canonical_operator_id(operator_id)
        except ProviderError as exc:
            return 400, {"error": str(exc)}
        if not self.dsh_web.available():
            return 503, {"error": "dsh executable is not available"}
        try:
            self.dsh_web.start(operator)
            return 200, {"url": f"{DSH_PROXY_PATH}{operator}/"}
        except (ProviderError, OSError) as exc:
            return 502, {"error": str(exc)}

    def close(self) -> None:
        if self.dsh_web:
            self.dsh_web.close()


class ResultHTTPServer(ThreadingHTTPServer):
    """Own the listen address so stale and current services cannot coexist."""

    allow_reuse_address = False
    allow_reuse_port = False

    def server_bind(self) -> None:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


class Handler(BaseHTTPRequestHandler):
    service: ResultService

    def _reject_request(self, status: int, message: str) -> None:
        # Drain ordinary request bodies before closing: on Windows, closing a
        # socket with unread POST data can reset it before the JSON error arrives.
        # Invalid/large/streaming bodies are not consumed, and slow senders are
        # bounded so a rejection never waits indefinitely for their payload.
        self.close_connection = True
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if 0 < length <= 256 * 1024 and not self.headers.get("Transfer-Encoding"):
            previous_timeout = self.connection.gettimeout()
            try:
                self.connection.settimeout(1)
                self.rfile.read(length)
            except OSError:
                pass
            finally:
                self.connection.settimeout(previous_timeout)
        self._send(status, {"error": message})

    def _allow_request_source(self) -> bool:
        # This is a trusted LAN service, not an authenticated API. Browsers must
        # act from the console's own origin; CLI callers may omit browser headers.
        # Do not trust X-Forwarded-* to widen the expected origin.
        expected = http_origin("http://" + self.headers.get("Host", ""))
        origin = self.headers.get("Origin")
        referer = self.headers.get("Referer")
        fetch_site = self.headers.get("Sec-Fetch-Site", "").lower()
        allowed = expected is not None and fetch_site not in {"cross-site", "same-site"}
        if origin is not None:
            allowed = allowed and http_origin(origin) == expected
        elif referer is not None:
            allowed = allowed and http_origin(referer, referer=True) == expected
        if not allowed:
            self._reject_request(403, "request must come from this console's origin")
        return allowed

    def _allow_json_body(self) -> bool:
        media_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if media_type != "application/json":
            self._reject_request(415, "Content-Type must be application/json")
            return False
        return True

    def _send(self, status: int, value: Any, content_type: str = "application/json") -> None:
        body = value if isinstance(value, bytes) else (
            json.dumps(value, ensure_ascii=False, default=_json_default).encode("utf-8")
            if content_type == "application/json" else str(value).encode("utf-8")
        )
        self.send_response(status)
        self.send_header("Content-Type", f"{content_type}; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_artifact_object(self, sha256: str) -> None:
        try:
            path = self.service.artifacts.object_path(sha256)
        except ArtifactError as exc:
            self._send(exc.status, {"error": str(exc)})
            return
        records = self.service.artifacts.list({"sha256": sha256.lower()})
        filename = records[0]["filename"] if records else sha256.lower()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header("Content-Disposition", "attachment; filename*=UTF-8''" + quote(filename))
        self.send_header("ETag", '"' + sha256.lower() + '"')
        self.send_header("Cache-Control", "private, max-age=31536000, immutable")
        self.end_headers()
        with path.open("rb") as source:
            shutil.copyfileobj(source, self.wfile, 1024 * 1024)

    def do_GET(self) -> None:  # noqa: N802
        if self._proxy_dsh():
            return
        parsed = urlparse(self.path)
        if parsed.path.startswith("/api/artifacts/objects/"):
            self._send_artifact_object(parsed.path.removeprefix("/api/artifacts/objects/"))
            return
        if parsed.path.startswith("/reports/"):
            parts = parsed.path.strip("/").split("/")
            try:
                if len(parts) != 3 or parts[2] != "index.html":
                    raise ValueError("report not found")
                artifact = self.service.reports.artifact_path(parts[1])
                self._send(200, artifact.read_bytes(), "text/html")
            except (ReportError, ValueError, OSError):
                self._send(404, {"error": "report not found"})
            return
        if parsed.path.startswith("/api/") or parsed.path in {"/healthz", "/readyz"}:
            try:
                status, value = self.service.payload(parsed.path, parse_qs(parsed.query))
            except (ValueError, RuntimeError) as exc:
                LOG.exception("GET %s failed", parsed.path)
                self._send(500, {"error": str(exc)})
                return
            self._send(status, value)
            return
        self._serve_static(parsed.path)

    def do_POST(self) -> None:  # noqa: N802
        if not self._allow_request_source():
            return
        if self._proxy_dsh():
            return
        if self.path == "/api/ai/dsh-web":
            if not self._allow_json_body():
                return
            try:
                body = self._read_json_body()
                if not isinstance(body, dict):
                    raise ValueError("request body must be an object")
                status, value = self.service.start_dsh_web(body.get("operator_id"))
            except (ValueError, json.JSONDecodeError) as exc:
                status, value = 400, {"error": str(exc)}
            self._send(status, value)
            return
        parsed = urlparse(self.path)
        if parsed.path == "/api/artifacts/upload":
            media_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
            if media_type != "application/octet-stream":
                self._reject_request(415, "Content-Type must be application/octet-stream")
                return
            if self.headers.get("Transfer-Encoding"):
                self._reject_request(411, "artifact upload requires Content-Length")
                return
            if self.headers.get("Content-Length") is None:
                self._reject_request(411, "artifact upload requires Content-Length")
                return
            try:
                length = int(self.headers["Content-Length"])
                query = parse_qs(parsed.query, keep_blank_values=True)
                def one(name: str) -> str | None:
                    values = query.get(name, [])
                    if len(values) > 1:
                        raise ArtifactError(f"{name} must be provided once")
                    return values[0] if values else None
                raw_metadata = one("metadata")
                metadata = json.loads(raw_metadata) if raw_metadata else {}
                artifact, created = self.service.artifacts.upload(
                    self.rfile, length, kind=one("kind"), name=one("name"),
                    version=one("version"), filename=one("filename"), metadata=metadata,
                    expected_sha256=self.headers.get("X-Content-SHA256"),
                )
                status, value = (201 if created else 200), artifact
            except (ValueError, json.JSONDecodeError) as exc:
                status, value = 400, {"error": str(exc)}
            except ArtifactError as exc:
                status, value = exc.status, {"error": str(exc)}
            except OSError as exc:
                LOG.exception("artifact upload failed")
                status, value = 500, {"error": str(exc)}
            self._send(status, value)
            return
        if parsed.path == "/api/artifacts/register":
            if not self._allow_json_body():
                return
            try:
                body = self._read_json_body()
                status, value = self.service.register_artifact(body)
            except (ValueError, json.JSONDecodeError) as exc:
                status, value = 400, {"error": str(exc)}
            self._send(status, value)
            return
        report_plugin = _plugin_resource_name(parsed.path, "reports")
        if parsed.path in {"/api/deployments/preview", "/api/deployments/bind-scheduler",
                            "/api/server-plugins/preview", "/api/server-plugins/install"}:
            if not self._allow_json_body():
                return
            try:
                status, value = self.service.deployment_operation(parsed.path, self._read_json_body())
            except (ValueError, json.JSONDecodeError) as exc:
                status, value = 400, {"error": str(exc)}
            except (RuntimeError, OSError) as exc:
                LOG.exception("deployment operation failed")
                status, value = 500, {"error": str(exc)}
            self._send(status, value)
            return
        if self.path != "/api/scheduler/commands" and report_plugin is None:
            self._send(404, {"error": "not found"})
            return
        if not self._allow_json_body():
            return
        try:
            body = self._read_json_body()
            if not isinstance(body, dict):
                raise ValueError("request body must be an object")
            if self.path == "/api/scheduler/commands":
                status, value = self.service.management_request(self.service.management.command, body)
            else:
                status, value = self.service.generate_report(report_plugin, body)
        except (ValueError, json.JSONDecodeError) as exc:
            status, value = 400, {"error": str(exc)}
        except (RuntimeError, OSError) as exc:
            LOG.exception("POST %s failed", self.path)
            status, value = 500, {"error": str(exc)}
        self._send(status, value)

    def do_PUT(self) -> None:  # noqa: N802
        if not self._allow_request_source():
            return
        if self._proxy_dsh():
            return
        if not self._allow_json_body():
            return
        parsed = urlparse(self.path)
        try:
            body = self._read_json_body()
            report_plugin = _plugin_resource_name(parsed.path, "report-script")
            if parsed.path.startswith("/api/config/"):
                status, value = self.service.management_request(
                    self.service.management.save_config, parsed.path.removeprefix("/api/config/"), body)
            elif parsed.path.startswith("/api/deployments/"):
                status, value = self.service.update_deployment(parsed.path, body)
            else:
                status, value = (self.service.update_report_script(report_plugin, body)
                                 if report_plugin is not None else self.service.update_plugin_view(parsed.path, body))
        except (ValueError, json.JSONDecodeError) as exc:
            status, value = 400, {"error": str(exc)}
        except OSError as exc:
            LOG.exception("PUT %s failed", parsed.path)
            status, value = 500, {"error": str(exc)}
        self._send(status, value)

    def do_HEAD(self) -> None:  # noqa: N802
        if not self._proxy_dsh():
            self.send_error(404)

    def do_DELETE(self) -> None:  # noqa: N802
        if not self._allow_request_source():
            return
        if not self._proxy_dsh():
            self._send(404, {"error": "not found"})

    def do_PATCH(self) -> None:  # noqa: N802
        if not self._allow_request_source():
            return
        if not self._proxy_dsh():
            self._send(404, {"error": "not found"})

    def do_OPTIONS(self) -> None:  # noqa: N802
        if not self._proxy_dsh():
            self._send(404, {"error": "not found"})

    def _proxy_dsh(self) -> bool:
        parsed = urlparse(self.path)
        is_dsh_path = parsed.path == DSH_PROXY_PATH.rstrip("/") or parsed.path.startswith(DSH_PROXY_PATH)
        if is_dsh_path:
            # GET can start DSH and WebSocket carries interactive commands.
            # Check before lazy startup or translating Origin for the upstream.
            if not self._allow_request_source():
                return True
        if not is_dsh_path:
            return False
        try:
            route = _dsh_proxy_route(parsed.path)
        except ProviderError as exc:
            self._send(400, {"error": str(exc)})
            return True
        if route is None:
            self._send(400, {"error": "operator_id is required in the DSH Web path"})
            return True
        operator_id, proxy_path = route
        if parsed.path == proxy_path.rstrip("/"):
            self.send_response(307)
            self.send_header("Location", proxy_path)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        if not self.service.dsh_web:
            self._send(404, {"error": "dsh provider is not configured"})
            return True
        try:
            with self.service.dsh_web.acquire(operator_id) as upstream:
                proxy_request(self, upstream, proxy_path)
        except (ProviderError, OSError, ValueError) as exc:
            self._send(502, {"error": f"DSH Web unavailable: {exc}"})
        return True

    def _read_json_body(self) -> Any:
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 256 * 1024:
            raise ValueError("request is too large")
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def _serve_static(self, path: str) -> None:
        relative = "index.html" if path in {"", "/"} else path.lstrip("/")
        candidate = (self.service.static_dir / relative).resolve()
        if self.service.static_dir not in candidate.parents and candidate != self.service.static_dir:
            self._send(403, {"error": "invalid path"})
            return
        if not candidate.is_file():
            self._send(404, {"error": "not found"})
            return
        self._send(200, candidate.read_bytes(), mimetypes.guess_type(candidate.name)[0] or "application/octet-stream")

    def log_message(self, fmt: str, *args: Any) -> None:
        LOG.info("%s - %s", self.address_string(), fmt % args)


def serve(config: ServiceConfig) -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    service = ResultService(config)
    handler = type("ConfiguredHandler", (Handler,), {"service": service})
    server = ResultHTTPServer((config.host, config.port), handler)
    if service.dsh_web:
        local_host = "127.0.0.1" if config.host in {"0.0.0.0", ""} else config.host
        service.dsh_web.service_url = f"http://{local_host}:{server.server_port}"
    LOG.info("result service listening on http://%s:%d", config.host, config.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        service.close()
        server.server_close()
