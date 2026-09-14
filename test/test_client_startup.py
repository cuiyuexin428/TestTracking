"""Client startup/quit regressions with owned configs and an unavailable endpoint."""

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
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
        assert "exactly one enabled plugin" in output, output
        del config["plugins"]["second"]
        config["client"]["register_timeout_ms"] = 3000
        path.write_text(json.dumps(config), encoding="utf-8")
        for _ in range(3):
            started = time.monotonic()
            run_case(arguments.client.resolve(), path, 0, b"quit\n")
            assert time.monotonic() - started < 2, "quit was lost during startup"
    print("PASS: registration failure, single plugin validation, startup quit; artifacts:", directory)


if __name__ == "__main__":
    main()
