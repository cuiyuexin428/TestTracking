from __future__ import annotations

import base64
import hashlib
import http.client
import io
import os
import json
import socket
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import contextmanager, nullcontext
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from dataclasses import replace
from urllib.request import Request, urlopen
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.config import ServiceConfig
from result_service.app.dsh_proxy import _BROWSER_BRIDGE
from result_service.app.providers import DshWebManager
from result_service.app.reports import ReportStore
from result_service.app.server import Handler, ResultHTTPServer, ResultService


class _DshFixture(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path == "/api/events.host" and self.headers.get("Upgrade") == "websocket":
            key = self.headers["Sec-WebSocket-Key"]
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
            ).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            frame = json.dumps({"origin": self.headers.get("Origin"),
                                "host": self.headers["Host"]}).encode()
            self.wfile.write(bytes([0x81, len(frame)]) + frame)
            self.wfile.flush()
            # Exercise client-to-upstream forwarding as well as downstream.
            client_frame = self.rfile.read(8)
            if client_frame:
                mask = client_frame[2:6]
                payload = bytes(value ^ mask[index % 4]
                                for index, value in enumerate(client_frame[6:]))
                self.wfile.write(bytes([0x81, len(payload)]) + payload)
                self.wfile.flush()
            self.close_connection = True
            return
        if self.path == "/plugins/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(b"data: ready\n\n")
            self.wfile.flush()
            self.server.finish_stream.wait(4)
            self.close_connection = True
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/session?one=1")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/":
            self._send(b'<html><head><script>window.__DSH_BOOT__={"entries":'
                       b'[{"url":"/plugins/connection/client.js"}]}</script>'
                       b'<script src="/assets/app.js"></script></head></html>', "text/html")
            return
        if self.path == "/assets/app.js":
            self._send(b"window.assetLoaded = true;", "text/javascript")
            return
        self._send(b"missing", "text/plain", status=404)

    def do_POST(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self._send(json.dumps({"path": self.path, "body": data.decode(),
                               "host": self.headers["Host"],
                               "origin": self.headers.get("Origin"),
                               "fetch_site": self.headers.get("Sec-Fetch-Site")}).encode())

    do_PUT = do_POST
    do_PATCH = do_POST
    do_DELETE = do_POST

    def _send(self, body, content_type="application/json", status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


@contextmanager
def _serve(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_port
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


class DshProxyTests(unittest.TestCase):
    operator_id = "123e4567-e89b-42d3-a456-426614174001"
    mount = f"/ai/dsh/{operator_id}/"

    @classmethod
    def setUpClass(cls):
        cls.upstream = ThreadingHTTPServer(("127.0.0.1", 0), _DshFixture)
        cls.upstream.daemon_threads = True
        cls.upstream.finish_stream = threading.Event()
        cls.upstream_context = _serve(cls.upstream)
        cls.upstream_port = cls.upstream_context.__enter__()
        cls.service = ResultService(ServiceConfig.defaults())
        cls.service.dsh_web = MagicMock()
        cls.service.dsh_web.acquire.side_effect = lambda operator: nullcontext(
            f"http://127.0.0.1:{cls.upstream_port}/"
        )
        handler = type("TestHandler", (Handler,), {"service": cls.service})
        cls.server = ResultHTTPServer(("127.0.0.1", 0), handler)
        cls.server_context = _serve(cls.server)
        cls.port = cls.server_context.__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.upstream.finish_stream.set()
        cls.server_context.__exit__(None, None, None)
        cls.upstream_context.__exit__(None, None, None)

    def setUp(self):
        self.service.dsh_web.acquire.reset_mock()

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        connection.request(method, path, body, headers={
            "Host": f"192.168.100.83:{self.port}",
            "Origin": f"http://192.168.100.83:{self.port}",
            "Sec-Fetch-Site": "same-origin",
            **(headers or {}),
        })
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response, data

    def test_remote_entry_rebases_assets_and_boot_graph_before_dsh_scripts(self):
        response, data = self.request("GET", self.mount)
        self.assertEqual(200, response.status)
        self.assertIn(f'src="{self.mount}assets/app.js"'.encode(), data)
        self.assertIn(f'"url":"{self.mount}plugins/connection/client.js"'.encode(), data)
        self.assertLess(data.index(b"window.fetch ="), data.index(b"window.__DSH_BOOT__"))
        self.assertEqual(len(data), int(response.getheader("Content-Length")))
        self.service.dsh_web.acquire.assert_called_with(self.operator_id)
        response, data = self.request("GET", self.mount + "assets/app.js")
        self.assertEqual(200, response.status)
        self.assertEqual(b"window.assetLoaded = true;", data)

    def test_missing_or_invalid_operator_namespace_is_rejected(self):
        for path in ("/ai/dsh/", "/ai/dsh/not-an-operator/"):
            with self.subTest(path=path):
                response, data = self.request("GET", path)
                self.assertEqual(400, response.status, data)
        self.service.dsh_web.acquire.assert_not_called()

    def test_http_methods_preserve_body_query_and_translate_same_origin(self):
        for method in ("POST", "PUT", "PATCH", "DELETE"):
            with self.subTest(method=method):
                body = '{"message":"你好"}'.encode()
                response, data = self.request(method, self.mount + "api/session?run=1", body)
                result = json.loads(data)
                self.assertEqual(200, response.status)
                self.assertEqual("/api/session?run=1", result["path"])
                self.assertEqual(body.decode(), result["body"])
                self.assertEqual(f"127.0.0.1:{self.upstream_port}", result["host"])
                self.assertEqual(f"http://127.0.0.1:{self.upstream_port}", result["origin"])
                self.assertEqual("same-origin", result["fetch_site"])

    def test_redirect_stays_inside_dsh_mount(self):
        response, _ = self.request("GET", self.mount + "redirect")
        self.assertEqual(302, response.status)
        self.assertEqual(self.mount + "session?one=1", response.getheader("Location"))

    def test_equivalent_host_spelling_is_translated_for_dsh(self):
        for host, origin in [("tracking-server:80", "http://tracking-server"),
                             ("Tracking-Server:56330", "http://tracking-server:56330")]:
            with self.subTest(host=host):
                response, data = self.request("POST", self.mount + "api/session", b"native payload",
                    {"Host": host, "Origin": origin, "Content-Type": "text/plain"})
                self.assertEqual(200, response.status)
                result = json.loads(data)
                self.assertEqual("native payload", result["body"])
                self.assertEqual(f"http://127.0.0.1:{self.upstream_port}", result["origin"])

    def test_foreign_origin_cannot_start_or_reach_dsh_including_websocket(self):
        for method, path, extra in (
            ('GET', self.mount, {}),
            ('POST', self.mount + 'api/session', {}),
            ('PATCH', self.mount + 'api/session', {}),
            ('DELETE', self.mount + 'api/session', {}),
            ('GET', self.mount + 'api/events.host', {'Connection': 'Upgrade', 'Upgrade': 'websocket'}),
        ):
            with self.subTest(method=method, path=path), patch.object(self.service.dsh_web, 'acquire') as acquire:
                connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=2)
                try:
                    connection.request(method, path, headers={'Origin': 'http://unrelated.invalid', **extra})
                    response = connection.getresponse()
                    self.assertEqual(403, response.status, response.read())
                    response.read()
                    acquire.assert_not_called()
                finally:
                    connection.close()

    def test_sse_is_flushed_before_upstream_completes(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        try:
            connection.request("GET", self.mount + "plugins/events")
            response = connection.getresponse()
            self.assertEqual("text/event-stream", response.getheader("Content-Type"))
            self.assertEqual(b"data: ready\n", response.readline())
            self.assertFalse(self.upstream.finish_stream.is_set())
            health, _ = self.request("GET", "/healthz")
            self.assertEqual(200, health.status)
        finally:
            connection.close()

    def test_websocket_upgrade_and_frames_work_with_remote_origin(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as client:
            authority = "Tracking-Server:80"
            client.sendall((f"GET {self.mount}api/events.host HTTP/1.1\r\n"
                            f"Host: {authority}\r\nOrigin: http://tracking-server\r\n"
                            "Connection: Upgrade\r\nUpgrade: websocket\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n").encode())
            stream = client.makefile("rb")
            self.assertIn(b"101", stream.readline())
            headers = b""
            while (line := stream.readline()) != b"\r\n":
                headers += line
            self.assertIn(b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", headers)
            frame = stream.read(2)
            self.assertEqual(0x81, frame[0])
            result = json.loads(stream.read(frame[1]))
            self.assertEqual(f"http://127.0.0.1:{self.upstream_port}", result["origin"])
            self.assertEqual(f"127.0.0.1:{self.upstream_port}", result["host"])
            mask = b"abcd"
            client.sendall(bytes([0x81, 0x82]) + mask +
                           bytes(value ^ mask[index] for index, value in enumerate(b"ok")))
            self.assertEqual(b"\x81\x02ok", stream.read(4))
            stream.close()


class DshBrowserBridgeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node.js is needed to exercise browser URL routing")
    def test_lan_browser_transport_and_dynamic_assets_keep_the_service_origin(self):
        # Execute the injected bridge against browser API stand-ins. Unlike
        # source-string checks this verifies URL objects, Request bodies,
        # WebSocket constants and inherited DOM property setters.
        mount = "/ai/dsh/123e4567-e89b-42d3-a456-426614174001"
        bridge = _BROWSER_BRIDGE.replace("__DSH_PROXY_PREFIX__", json.dumps(mount))
        script = r"""
const assert = require('node:assert/strict');
globalThis.window = globalThis;
globalThis.location = new URL('http://192.168.100.83:56330/ai/dsh/123e4567-e89b-42d3-a456-426614174001/');
globalThis.crypto = {getRandomValues: require('node:crypto').webcrypto.getRandomValues.bind(require('node:crypto').webcrypto)};
const requests = [];
globalThis.fetch = async (input, init) => { requests.push([input, init]); };
globalThis.WebSocket = class { static OPEN = 1; constructor(url) { this.url = url; } };
globalThis.EventSource = class { constructor(url) { this.url = url; } };
for (const [name, property] of [['HTMLScriptElement','src'],['HTMLLinkElement','href'],
    ['HTMLImageElement','src'],['HTMLAnchorElement','href']]) {
  globalThis[name] = class {};
  Object.defineProperty(globalThis[name].prototype, property, {
    configurable: true, get() {return this.value;}, set(value) {this.value=value;}
  });
}
""" + bridge.removeprefix("<script>").removesuffix("</script>") + r"""
(async () => {
  await fetch(new URL('/api/session.list', location.origin));
  assert.equal(requests[0][0], location.origin + '/ai/dsh/123e4567-e89b-42d3-a456-426614174001/api/session.list');
  await fetch(new Request(location.origin + '/api/session.prompt', {method:'POST', body:'hello'}));
  assert.equal(requests[1][0].url, location.origin + '/ai/dsh/123e4567-e89b-42d3-a456-426614174001/api/session.prompt');
  assert.equal(requests[1][0].method, 'POST');
  assert.equal(await requests[1][0].text(), 'hello');
  assert.equal(new WebSocket('ws://192.168.100.83:56330/api/events.host').url,
    'ws://192.168.100.83:56330/ai/dsh/123e4567-e89b-42d3-a456-426614174001/api/events.host');
  assert.equal(WebSocket.OPEN, 1);
  assert.equal(new EventSource('/plugins/events').url, location.origin + '/ai/dsh/123e4567-e89b-42d3-a456-426614174001/plugins/events');
  const link = new HTMLLinkElement();
  link.href = '/assets/langs/sql.js';
  assert.equal(link.href, location.origin + '/ai/dsh/123e4567-e89b-42d3-a456-426614174001/assets/langs/sql.js');
  link.href = '/ai/dsh/123e4567-e89b-42d3-a456-426614174001/assets/langs/sql.js';
  assert.equal(link.href, location.origin + '/ai/dsh/123e4567-e89b-42d3-a456-426614174001/assets/langs/sql.js');
  link.href = 'https://example.org/help';
  assert.equal(link.href, 'https://example.org/help');
  const report = new HTMLAnchorElement();
  report.href = '/reports/abc123/index.html';
  assert.equal(report.href, location.origin + '/reports/abc123/index.html');
  report.href = location.origin + '/reports/abc123/index.html';
  assert.equal(report.href, location.origin + '/reports/abc123/index.html');
  await fetch('/reports/abc123/index.html');
  assert.equal(requests[2][0], location.origin + '/reports/abc123/index.html');
  assert.match(crypto.randomUUID(), /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
})().catch(error => {console.error(error);process.exitCode=1;});
"""
        completed = subprocess.run([shutil.which("node"), "-"], input=script, text=True,
                                   encoding="utf-8", capture_output=True, timeout=10)
        self.assertEqual(0, completed.returncode, completed.stderr)


class DshReadinessTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("TESTTRACKING_DSH_WEB_COMMAND"), "set TESTTRACKING_DSH_WEB_COMMAND for native two-user isolation test")
    def test_native_dsh_users_have_private_workspaces_and_persistent_session_lists(self):
        test_root = Path(__file__).resolve().parents[1] / "build/test"
        test_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="dsh-users-", dir=test_root) as directory:
            config = replace(ServiceConfig.defaults().providers["dsh"],
                             command=os.environ["TESTTRACKING_DSH_WEB_COMMAND"], cwd=directory)
            operators = ["123e4567-e89b-42d3-a456-426614174001", "123e4567-e89b-42d3-a456-426614174002"]
            manager = DshWebManager(config, startup_timeout=40)
            def rpc(url, method, payload):
                request = Request(url + "api/" + method, data=json.dumps({
                    "type": "client-request", "rpcId": "isolation-test", "method": method, "payload": payload,
                }).encode(), headers={"Content-Type": "application/json"})
                with urlopen(request, timeout=15) as response:
                    result = json.load(response)["result"]
                self.assertTrue(result["ok"], result)
                return result["value"]
            try:
                urls = [manager.start(operator) for operator in operators]
                self.assertNotEqual(*urls)
                work = rpc(urls[0], "workspace.create", {"path": str(Path(__file__).resolve().parents[1])})["workspace"]
                created = rpc(urls[0], "session.create", {"workspaceId": work["workspaceId"]})
                rpc(urls[0], "session.rename", {"sessionId": created["sessionId"], "title": "User A private analysis"})
                self.assertEqual([], rpc(urls[1], "workspace.list", {})["items"])
                self.assertEqual([], rpc(urls[1], "session.list", {})["items"])
                self.assertIn(created["sessionId"], [item["sessionId"] for item in rpc(urls[0], "session.list", {})["items"]])
                # Native DSH batches persistence for 200 ms. Wait for its own
                # durable artifact before testing a stopped process's reload.
                root = Path(directory) / "runtime/result_service/dsh_web" / operators[0] / "sessions"
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not any(path.stat().st_size for path in root.rglob("*.jsonl*")):
                    time.sleep(0.05)
                self.assertTrue(any(path.stat().st_size for path in root.rglob("*.jsonl*")))
                manager.close()
                manager = DshWebManager(config, startup_timeout=40)
                reloaded = manager.start(operators[0])
                self.assertIn(created["sessionId"], [item["sessionId"] for item in rpc(reloaded, "session.list", {})["items"]])
                self.assertEqual(work["workspaceId"], rpc(reloaded, "workspace.list", {})["items"][0]["workspaceId"])
            finally:
                manager.close()

    def test_managed_dsh_receives_bound_service_context_and_cleans_overlay(self):
        manager = DshWebManager(ServiceConfig.defaults().providers["dsh"])
        manager.service_url = "http://127.0.0.1:54321"
        process = MagicMock()
        process.poll.return_value = None
        response = MagicMock()
        response.__enter__.return_value.status = 200
        response.__enter__.return_value.read.return_value = b"__DSH_BOOT__"
        with patch("result_service.app.providers.resolve_provider_command", return_value="dsh"), \
                patch.object(manager, "_free_loopback_port", return_value=56335), \
                patch("result_service.app.providers.subprocess.Popen", return_value=process) as popen, \
                patch("result_service.app.providers.urlopen", return_value=response), \
                patch.object(manager, "_api_ready", return_value=True):
            operator = "123e4567-e89b-42d3-a456-426614174001"
            manager.start(operator)
        argv = popen.call_args.args[0]
        overlays = [Path(argv[index + 1]) for index, arg in enumerate(argv) if arg == "--patch"]
        self.assertEqual(2, len(overlays))
        context = json.loads(overlays[1].read_text(encoding="utf-8"))
        integration = next(row for row in context if "insert" in row)["insert"][0]
        self.assertEqual("testtracking-result-context", integration["id"])
        self.assertTrue(integration["name"].startswith("file:///"))
        roots = {row["id"]: row["config"] for row in context if "config" in row}
        self.assertIn(operator, roots["session-persistence-jsonl"]["root"])
        self.assertIn(operator, roots["storage-json"]["root"])
        self.assertIn(operator, roots["attachment-local"]["dshHome"])
        self.assertEqual(manager.service_url, popen.call_args.kwargs["env"]["RESULT_SERVICE_URL"])
        self.assertEqual(operator, popen.call_args.kwargs["env"]["TESTTRACKING_OPERATOR_ID"])
        with patch.object(manager, "_stop_process"):
            manager.close()
        self.assertFalse(overlays[1].exists())

    def test_static_page_is_not_mistaken_for_a_ready_api(self):
        responses = []
        for data in (b"<!doctype html><html></html>",
                     b'{"result":{"ok":true,"value":{}}}'):
            response = MagicMock()
            response.__enter__.return_value = io.BytesIO(data)
            response.__enter__.return_value.status = 200
            responses.append(response)
        with patch("result_service.app.providers.urlopen", side_effect=responses) as open_url:
            self.assertFalse(DshWebManager._api_ready("http://127.0.0.1:12345/"))
            self.assertTrue(DshWebManager._api_ready("http://127.0.0.1:12345/"))
        request = open_url.call_args.args[0]
        self.assertEqual("http://127.0.0.1:12345/api/host.describe", request.full_url)
        self.assertEqual("host.describe", json.loads(request.data)["method"])

    @unittest.skipUnless(shutil.which("node"), "Node.js is needed to validate the DSH prompt plugin")
    def test_prompt_plugin_registers_db_only_report_workflow_and_shell_url(self):
        module = (Path(__file__).resolve().parents[1] /
                  "result_service/app/dsh_context.mjs").as_uri()
        script = "import {apply} from " + json.dumps(module) + ";\n" + r"""
import assert from 'node:assert/strict';
const sections = [], environments = [];
apply({systemPrompt: {section: value => sections.push(value)},
  inject: (names, callback) => callback({shellEnv: {register: value => environments.push(value)}})});
assert.equal(sections.length, 1);
assert.ok(sections[0].text.includes(process.env.RESULT_SERVICE_URL + '/api/analysis/catalog'));
assert.ok(sections[0].text.includes('read-only psycopg connection'));
assert.ok(sections[0].text.includes('render(connection, context)'));
assert.ok(sections[0].text.includes('/api/plugins/<plugin>/report-script'));
const workflow = sections[0].text;
assert.ok(workflow.indexOf('First GET ') < workflow.indexOf('PUT '));
const request = JSON.parse(workflow.match(/PUT \S+ with JSON (\{[^\n]+?\})/)[1]);
assert.deepEqual(Object.keys(request).sort(), ['operator_id', 'revision', 'source']);
assert.equal(request.operator_id, process.env.TESTTRACKING_OPERATOR_ID);
assert.ok(workflow.includes('HTTP 409'));
assert.ok(workflow.includes('preserve the draft'));
assert.ok(workflow.includes('merge the changes before saving'));
assert.ok(workflow.includes('Do not blindly retry'));
assert.equal(environments[0].resolve().DSH_RESULT_SERVICE_URL, process.env.RESULT_SERVICE_URL);
assert.equal(environments[0].resolve().TESTTRACKING_OPERATOR_ID, process.env.TESTTRACKING_OPERATOR_ID);
console.log(JSON.stringify(request));
"""
        operator = "123e4567-e89b-42d3-a456-426614174001"
        completed = subprocess.run([shutil.which("node"), "--input-type=module", "-"],
                                   input=script, text=True, encoding="utf-8", capture_output=True,
                                   timeout=10, env={**os.environ, "RESULT_SERVICE_URL": "http://127.0.0.1:54321",
                                                    "TESTTRACKING_OPERATOR_ID": operator})
        self.assertEqual(0, completed.returncode, completed.stderr)
        # Follow the generated request example against the actual API contract.
        request = json.loads(completed.stdout)
        with tempfile.TemporaryDirectory(prefix="dsh report contract ") as directory:
            service = ResultService(ServiceConfig.defaults())
            self.addCleanup(service.close)
            service.reports = ReportStore(Path(directory) / "scripts", Path(directory) / "reports")
            status, original = service.payload("/api/plugins/coverage/report-script", {})
            self.assertEqual(200, status)
            request["revision"] = original["revision"]
            request["source"] = 'def render(connection, context):\n    return "Saved by DSH"\n'
            status, saved = service.update_report_script("coverage", request)
            self.assertEqual(200, status, saved)
            request["source"] = request["source"].replace("Saved", "Stale")
            status, conflict = service.update_report_script("coverage", request)
            self.assertEqual(409, status, conflict)
            self.assertEqual(saved["source"], service.reports.get_script("coverage")["source"])


if __name__ == "__main__":
    unittest.main()
