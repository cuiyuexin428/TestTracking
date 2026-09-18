from __future__ import annotations

import json
import logging
import mimetypes
import socket
from dataclasses import replace
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

from .config import ServiceConfig
from .dsh_proxy import DSH_PROXY_PATH, http_origin, proxy_request
from .management import ManagementError, ManagementService
from .plugins import default_plugins
from .providers import DshWebManager, ProviderError, build_providers
from .repository import create_repository
from .reports import ReportError, ReportStore
from .views import PluginViewError, PluginViewStore

LOG = logging.getLogger("result_service")


def _plugin_resource_name(path: str, resource: str) -> str | None:
    parts = path.strip("/").split("/")
    if len(parts) == 4 and parts[:2] == ["api", "plugins"] and parts[3] == resource:
        return parts[2]
    return None


def _view_plugin_name(path: str) -> str | None:
    return _plugin_resource_name(path, "view")


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
        plugin_options = {name: dict(value) for name, value in config.plugin_options.items()}
        if config.stp_result_table and "result_table" not in plugin_options.get("stpcompare", {}):
            plugin_options.setdefault("stpcompare", {})["result_table"] = config.stp_result_table
        self.plugin_options = plugin_options
        self.plugins = default_plugins(
            plugin_options, config.plugin_views_dir, config.plugin_adapters_dir,
            config.report_templates_dir,
        )
        self.plugin_views = PluginViewStore(config.plugin_views_dir)
        self.repository = create_repository(config.db_conn, self.plugins)
        runtime = Path(config.workspace or Path(__file__).resolve().parents[2]) / "runtime" / "result_service"
        self.reports = ReportStore(
            config.analysis_scripts_dir or runtime / "analysis_scripts",
            config.analysis_reports_dir or runtime / "reports",
            default_scripts_dir=config.report_templates_dir,
        )
        self.providers = build_providers({
            name: replace(provider, cwd=provider.cwd or config.workspace)
            for name, provider in config.providers.items()
        })
        dsh = self.providers.get("dsh")
        self.dsh_web = DshWebManager(dsh.config) if dsh else None
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
                    "view": plugin.view_definition(),
                }
                for name, plugin in sorted(self.plugins.items())
            ]
        if path == "/api/plugin-view-schema":
            return 200, self.plugin_views.schema()
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
            return 200, plugin.view_definition()
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
            return 200, [{"name": name, "available": provider.available()}
                         for name, provider in sorted(self.providers.items())]
        return 404, {"error": "not found"}

    @staticmethod
    def management_request(operation, *args) -> tuple[int, Any]:
        try:
            return 200, operation(*args)
        except ManagementError as exc:
            return exc.status, {"error": str(exc)}

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
        if not isinstance(body, dict) or not isinstance(body.get("source"), str):
            return 400, {"error": "source must be a Python source string"}
        try:
            return 200, self.reports.save_script(plugin, body["source"])
        except ReportError as exc:
            return 400, {"error": str(exc)}

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

    def update_plugin_view(self, path: str, body: Any) -> tuple[int, Any]:
        plugin_name = _view_plugin_name(path)
        if plugin_name is None:
            return 404, {"error": "not found"}
        plugin = self.plugins.get(plugin_name)
        if not plugin:
            return 404, {"error": f"unknown plugin: {plugin_name}"}
        try:
            return 200, plugin.save_view_definition(body)
        except PluginViewError as exc:
            return 400, {"error": str(exc)}

    def ai_chat(self, body: dict[str, Any]) -> tuple[int, Any]:
        provider_name = str(body.get("provider", "codex"))
        message = str(body.get("message", "")).strip()
        context = body.get("context", {})
        conversation_id = body.get("conversation_id")
        if not message or not isinstance(context, dict):
            return 400, {"error": "provider, message and object context are required"}
        if conversation_id is not None and not isinstance(conversation_id, str):
            return 400, {"error": "conversation_id must be a string or null"}
        if len(message) > 8000:
            return 400, {"error": "message is too long"}
        provider = self.providers.get(provider_name)
        if not provider:
            return 404, {"error": f"unknown provider: {provider_name}"}
        context = dict(context)
        if self.repository.configured:
            try:
                plugin = context.get("result_type")
                catalog = self.analysis_catalog(plugin if isinstance(plugin, str) else None, limit=20)
                catalog["scope"] = "Latest 20 durable runs for the selected plugin; table metadata is live. Outcome completeness does not prove raw business fields are present or post-processing is complete; report scripts check those separately."
                context["analysis_catalog"] = catalog
            except RuntimeError:
                context["analysis_catalog"] = {"error": "database_unavailable"}
        if context.get("purpose") == "analysis_report_design":
            context["script_contract"] = REPORT_CONTRACT
        try:
            result = provider.complete(message, context, conversation_id)
        except (ProviderError, OSError) as exc:
            return 502, {"error": str(exc), "provider": provider_name}
        return 200, {
            "provider": result.provider,
            "text": result.text,
            "conversation_id": result.conversation_id,
        }

    def start_dsh_web(self) -> tuple[int, Any]:
        if not self.dsh_web:
            return 404, {"error": "dsh provider is not configured"}
        if not self.providers["dsh"].available():
            return 503, {"error": "dsh executable is not available"}
        try:
            self.dsh_web.start()
            return 200, {"url": DSH_PROXY_PATH}
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

    def do_GET(self) -> None:  # noqa: N802
        if self._proxy_dsh():
            return
        parsed = urlparse(self.path)
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
            # The console starts DSH with an empty POST; do not impose a JSON
            # body on that action, but reject non-JSON bodies if one is supplied.
            if self.headers.get("Content-Length", "0") != "0" and not self._allow_json_body():
                return
            status, value = self.service.start_dsh_web()
            self._send(status, value)
            return
        report_plugin = _plugin_resource_name(urlparse(self.path).path, "reports")
        if self.path not in {"/api/ai/chat", "/api/scheduler/commands"} and report_plugin is None:
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
                status, value = (self.service.generate_report(report_plugin, body)
                                 if report_plugin is not None else self.service.ai_chat(body))
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
        if parsed.path == DSH_PROXY_PATH.rstrip("/") or parsed.path.startswith(DSH_PROXY_PATH):
            # GET can start DSH and WebSocket carries interactive commands.
            # Check before lazy startup or translating Origin for the upstream.
            if not self._allow_request_source():
                return True
        if parsed.path == DSH_PROXY_PATH.rstrip("/"):
            self.send_response(307)
            self.send_header("Location", DSH_PROXY_PATH)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        if not parsed.path.startswith(DSH_PROXY_PATH):
            return False
        if not self.service.dsh_web:
            self._send(404, {"error": "dsh provider is not configured"})
            return True
        try:
            proxy_request(self, self.service.dsh_web.start())
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
