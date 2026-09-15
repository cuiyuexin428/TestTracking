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
from urllib.parse import parse_qs, urlparse

from .config import ServiceConfig
from .plugins import default_plugins
from .providers import DshWebManager, ProviderError, build_providers
from .repository import create_repository

LOG = logging.getLogger("result_service")


def _json_default(value: Any) -> Any:
    if hasattr(value, "isoformat"):
        return value.isoformat()
    if isinstance(value, (bytes, bytearray)):
        return "<binary>"
    return str(value)


class ResultService:
    def __init__(self, config: ServiceConfig):
        self.config = config
        self.plugins = default_plugins(config.stp_result_table)
        self.repository = create_repository(config.db_conn, self.plugins)
        self.providers = build_providers({
            name: replace(provider, cwd=provider.cwd or config.workspace)
            for name, provider in config.providers.items()
        })
        dsh = self.providers.get("dsh")
        self.dsh_web = DshWebManager(dsh.config) if dsh else None
        static_dir = config.static_dir or str(Path(__file__).resolve().parents[1] / "web")
        self.static_dir = Path(static_dir).resolve()

    def payload(self, path: str, query: dict[str, list[str]]) -> tuple[int, Any]:
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
            return 200, [{"name": name} for name in sorted(self.plugins)]
        if path == "/api/runs":
            plugin = query.get("plugin", [None])[0]
            limit = query.get("limit", ["50"])[0]
            return 200, self.repository.list_runs(plugin, int(limit))
        if path.startswith("/api/runs/"):
            run_id = path.rsplit("/", 1)[-1]
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
            return 200, {"url": self.dsh_web.start()}
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
        parsed = urlparse(self.path)
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
        if self.path == "/api/ai/dsh-web":
            status, value = self.service.start_dsh_web()
            self._send(status, value)
            return
        if self.path != "/api/ai/chat":
            self._send(404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length > 128 * 1024:
                raise ValueError("request is too large")
            body = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(body, dict):
                raise ValueError("request body must be an object")
            status, value = self.service.ai_chat(body)
        except (ValueError, json.JSONDecodeError) as exc:
            status, value = 400, {"error": str(exc)}
        self._send(status, value)

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
    LOG.info("result service listening on http://%s:%d", config.host, config.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        service.close()
        server.server_close()
