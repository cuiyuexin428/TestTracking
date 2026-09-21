"""Client startup/quit regressions with owned configs and an unavailable endpoint."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


def run_case(executable, config, expected, command=None):
    process = subprocess.Popen(
        [str(executable), str(config)], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    try:
        if command:
            process.stdin.write(command)
            process.stdin.flush()
        # Keep stdin open: failed initialization must stop its own input worker.
        code = process.wait(timeout=4)
        output = process.stdout.read().decode("utf-8", errors="replace")
        if code != expected:
            raise AssertionError(f"Expected exit {expected}, got {code}:\n{output}")
        return output
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=3)
        process.stdin.close()
        process.stdout.close()


def test_update_without_scheduler(executable, directory, config):
    """The Agent remains repairable after restoring an unregistered old config."""
    state = {"desired": None, "gets": 0, "reports": [], "heartbeats": []}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            state["gets"] += 1
            self.reply({"client_id": "updater-test", "desired": state["desired"]})

        def do_PUT(self):
            state["reports"].append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            self.reply({"ok": True})

        def do_POST(self):
            state['heartbeats'].append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
            self.reply({'ok': True})

        def reply(self, value):
            data = json.dumps(value).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    config["client"].update(worker_id="updater-test", register_timeout_ms=100,
                             register_max_attempts=2, deployment_poll_seconds=1,
                             deployment_endpoint=f"http://127.0.0.1:{server.server_port}")
    path = directory / "updater.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    log = directory / "updater.log"
    process = None
    try:
        with log.open("wb") as output:
            process = subprocess.Popen([str(executable), str(path)], cwd=directory,
                stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 8
            result = directory / "deployments/update_result.json"
            while time.monotonic() < deadline:
                if result.is_file() and json.loads(result.read_text())["status"] == "current":
                    break
                assert process.poll() is None, log.read_text(errors="replace")
                time.sleep(.05)
            else:
                raise AssertionError("real updater did not poll while scheduler was unavailable:\n" + log.read_text(errors="replace"))
            assert state["gets"], "updater made no real HTTP requests"
            state["desired"] = {"revision": "a" * 64, "components": [],
                                  "config_patch": {"client": {"task_threads": 2}}}
            deadline = time.monotonic() + 10
            failed_path = directory / 'deployments/failed.json'
            while time.monotonic() < deadline:
                if (failed_path.is_file()
                        and any(heartbeat['state'] == 'error' for heartbeat in state['heartbeats'])
                        and any(report['state'] == 'failed' for report in state['reports'])):
                    break
                assert process.poll() is None, log.read_text(errors="replace")
                time.sleep(.05)
            else:
                raise AssertionError("restored configuration did not report failure while remaining repairable:\n" + log.read_text(errors="replace"))
            assert json.loads(failed_path.read_text())['revision'] == 'a' * 64
            assert any(report['state'] == 'failed' and report['desired_revision'] == 'a' * 64
                       for report in state['reports']), 'restored but unregistered workers must not hold the update forever'
            assert not (directory / "deployments/active.json").exists(), "unregistered release was activated"
            process.stdin.write(b"quit\n")
            process.stdin.flush()
            assert process.wait(timeout=3) == 0
    finally:
        if process:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            process.stdin.close()
        server.shutdown()
        server.server_close()
        thread.join(3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--demo", type=Path, required=True)
    arguments = parser.parse_args()
    directory = Path(tempfile.mkdtemp(prefix="testtracking-client-startup-"))
    with socket.socket() as endpoint:
        # Bind but do not listen; no other local service can acquire this port.
        endpoint.bind(("127.0.0.1", 0))
        config = {
            "scheduler": {"endpoint": f"tcp://127.0.0.1:{endpoint.getsockname()[1]}",
                          "poll_timeout_ms": 5},
            "client": {"register_timeout_ms": 100, "register_max_attempts": 2},
            "plugins": {"demo": {"dll": str(arguments.demo.resolve()), "enabled": True}}}
        path = directory / "client.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        output = run_case(arguments.client.resolve(), path, 1)
        assert "registration failed" in output, output
        config["plugins"]["second"] = dict(config["plugins"]["demo"])
        path.write_text(json.dumps(config), encoding="utf-8")
        output = run_case(arguments.client.resolve(), path, 1)
        assert "registration failed" in output, output
        assert "client agent starting 2 plugin workers" in output, output
        config["plugins"]["second"]["enabled"] = False
        path.write_text(json.dumps(config), encoding="utf-8")
        output = run_case(arguments.client.resolve(), path, 1)
        assert "registration failed" in output, output
        config["plugins"]["demo"]["enabled"] = False
        path.write_text(json.dumps(config), encoding="utf-8")
        output = run_case(arguments.client.resolve(), path, 1)
        assert "no enabled plugin" in output, output
        config["plugins"]["demo"]["enabled"] = True
        config["client"]["register_timeout_ms"] = 3000
        path.write_text(json.dumps(config), encoding="utf-8")
        for _ in range(3):
            started = time.monotonic()
            run_case(arguments.client.resolve(), path, 0, b"quit\n")
            assert time.monotonic() - started < 2, "quit was lost during startup"
        if os.name == "nt":
            test_update_without_scheduler(arguments.client.resolve(), directory, config)
    print("PASS: registration failure, multi-plugin startup, startup quit, real updater and reports without scheduler; artifacts:", directory)


if __name__ == "__main__":
    main()
