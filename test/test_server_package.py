"""Manual smoke test for a relocated server package.

The test deliberately runs from a temporary directory whose name contains a
space. It does not register with CTest because it starts a private PostgreSQL
cluster and is intended for release-package validation.

Example::

    python -B test/test_server_package.py \
        --package build/server-package-smoke-final \
        --pg-bin third_party/pgsql/bin -v
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[1]

# Reuse the existing private PostgreSQL fixture instead of maintaining a
# second database bootstrap implementation in this package test.
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres  # noqa: E402


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def clean_environment(package_root: Path, pg_bin: Path) -> dict[str, str]:
    """Keep only package/runtime paths needed by the child processes."""
    env = dict(os.environ)
    for key in ("PYTHON", "PYTHONPATH", "PYTHONHOME", "PYTHONSTARTUP", "PYTHONUSERBASE", "VIRTUAL_ENV"):
        env.pop(key, None)
    for key in list(env):
        if key.upper().startswith(("PG", "RESULT_SERVICE_")):
            env.pop(key, None)
    path_entries = [
        str(package_root / "runtime" / "python"),
        str(pg_bin),
    ]
    # Keep Windows system directories for native process loading, but do not
    # inherit repository, virtualenv, or user-tool PATH entries.
    system_root = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    path_entries.extend(str(system_root / name) for name in ("System32", "System32\\Wbem"))
    env["PATH"] = os.pathsep.join(path_entries)
    return env


def request_json(url: str) -> tuple[int, object]:
    with urlopen(url, timeout=3) as response:
        return response.status, json.loads(response.read().decode("utf-8"))


def request_text(url: str) -> tuple[int, str]:
    with urlopen(url, timeout=3) as response:
        return response.status, response.read().decode("utf-8", errors="replace")


def wait_for_ready(base_url: str, process: subprocess.Popen[str], timeout: float = 20) -> dict:
    deadline = time.monotonic() + timeout
    last_error = "service did not become ready"
    while time.monotonic() < deadline:
        if process.poll() is not None and process.returncode not in (None, 0):
            output = ""
            try:
                output = process.stdout.read() if process.stdout else ""
            except OSError:
                pass
            raise RuntimeError(f"PowerShell launcher failed ({process.returncode}): {output}")
        try:
            status, payload = request_json(base_url + "/readyz")
            if status == 200 and isinstance(payload, dict) and payload.get("status") == "ready":
                return payload
            last_error = f"readyz returned {status}: {payload}"
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            last_error = str(exc)
        time.sleep(0.25)
    raise RuntimeError(last_error)


def stop_process_tree(pid: int | None) -> None:
    if not pid:
        return
    subprocess.run(
        ["taskkill", "/PID", str(pid), "/T", "/F"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )


class ServerPackageSmoke(unittest.TestCase):
    package: Path
    pg_bin: Path
    env: dict[str, str]
    pg: PrivatePostgres
    service_process: subprocess.Popen[str] | None = None
    service_pid: int | None = None

    @classmethod
    def setUpClass(cls) -> None:
        options = getattr(SmokeOptions, "value", None)
        source = Path(options.package).resolve()
        if not (source / "scripts" / "start_result_service.ps1").is_file():
            raise RuntimeError(f"not a server package: {source}")
        runtime_python = source / "runtime" / "python" / "python.exe"
        if not runtime_python.is_file():
            raise RuntimeError(f"package has no embedded Python: {runtime_python}")

        # tempfile's prefix provides a stable space in the relocation path.
        cls._temp_parent = Path(tempfile.mkdtemp(prefix="server package smoke "))
        cls.addClassCleanup(cls._cleanup)
        cls.package = cls._temp_parent / "relocated server package"
        shutil.copytree(source, cls.package)
        packaged_pg_bin = cls.package / "postgres" / "bin"
        cls.pg_bin = packaged_pg_bin if packaged_pg_bin.is_dir() else Path(options.pg_bin).resolve()
        if not cls.pg_bin.is_dir():
            raise RuntimeError(f"PostgreSQL bin directory not found: {cls.pg_bin}")
        cls.env = clean_environment(cls.package, cls.pg_bin)
        cls.pg = PrivatePostgres(cls.pg_bin, cls.package / "runtime" / "python" / "python.exe")
        cls.pg.env = dict(cls.env, PGCLIENTENCODING="UTF8", LC_ALL="C")
        cls.pg.start()
        cls._prepare_database()

    @classmethod
    def _prepare_database(cls) -> None:
        cls.pg.sql(
            """
            CREATE TABLE plugin_runs(
                run_id text PRIMARY KEY,
                plugin_type text NOT NULL,
                expected_files int,
                manifest_fingerprint text,
                status text,
                program_version text,
                dataset_version text,
                created_at timestamptz,
                finished_at timestamptz
            );
            CREATE TABLE plugin_file_outcomes(
                run_id text,
                plugin_type text,
                file text,
                status text,
                detail text
            );
            INSERT INTO plugin_runs VALUES (
                'smoke-run', 'coverage', 1, 'manifest-smoke', 'success',
                'program-1', 'dataset-1', now(), now());
            INSERT INTO plugin_file_outcomes VALUES (
                'smoke-run', 'coverage', 'sample.stp', 'success', 'ok');
            """
        )

    @classmethod
    def _cleanup(cls) -> None:
        # Read the PID even if readiness failed before the test could observe
        # it. The launcher exits with -NoWait; killing it is not sufficient.
        if hasattr(cls, "package"):
            pid_file = cls.package / "data" / "result_service.pid"
            if pid_file.is_file():
                stop_process_tree(int(pid_file.read_text(encoding="utf-8").strip()))
                pid_file.unlink(missing_ok=True)
        if cls.service_process is not None:
            # -NoWait intentionally leaves the Python service detached; this
            # launcher process only needs cleanup when PowerShell itself failed.
            if cls.service_process.poll() is None:
                stop_process_tree(cls.service_process.pid)
            try:
                cls.service_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            if cls.service_process.stdout:
                cls.service_process.stdout.close()
        if hasattr(cls, "pg"):
            cls.pg.close()
        if hasattr(cls, "_temp_parent"):
            target = cls._temp_parent.resolve()
            if target.parent != Path(tempfile.gettempdir()).resolve() or not target.name.startswith("server package smoke "):
                raise RuntimeError(f"Refusing unsafe test cleanup path: {target}")
            shutil.rmtree(target)

    def test_result_service_from_relocated_package(self) -> None:
        config_path = self.package / "config" / "result_service.json"
        config = json.loads(config_path.read_text(encoding="utf-8-sig"))
        port = free_port()
        config["host"] = "127.0.0.1"
        config["port"] = port
        config["db_conn"] = self.pg.conn
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")

        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        if not powershell:
            self.skipTest("PowerShell is required by the package launcher")
        script = self.package / "scripts" / "start_result_service.ps1"
        type(self).service_process = subprocess.Popen(
            [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), "-NoWait"],
            cwd=self.package,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        base_url = f"http://127.0.0.1:{port}"
        wait_for_ready(base_url, self.service_process)
        pid_path = self.package / "data" / "result_service.pid"
        self.assertTrue(pid_path.is_file(), "launcher did not write service PID")
        self.assertGreater(int(pid_path.read_text(encoding="utf-8").strip()), 0)
        status, runs = request_json(base_url + "/api/runs?plugin=coverage")
        self.assertEqual(status, 200)
        self.assertTrue(any(item.get("run_id") == "smoke-run" for item in runs))
        status, html = request_text(base_url + "/")
        self.assertEqual(status, 200)
        self.assertIn("<!doctype html>", html.lower())

    def test_scheduler_starts_and_stops_with_package_dlls(self) -> None:
        config_path = self.package / "config" / "scheduler-smoke.json"
        config_path.write_text(
            json.dumps(
                {
                    "server": {
                        "router_endpoint": f"tcp://127.0.0.1:{free_port()}",
                        "poll_timeout_ms": 10,
                        "client_timeout_ms": 1000,
                        "task_lease_timeout_ms": 0,
                        "db_conn": "",
                    },
                    "plugins": {},
                },
                indent=2,
            ),
            encoding="utf-8",
        )
        scheduler = self.package / "bin" / "scheduler.exe"
        scheduler_env = dict(self.env)
        scheduler_env["PATH"] = os.pathsep.join([
            str(self.package / "bin"),
            str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"),
        ])
        process = subprocess.Popen(
            [str(scheduler), str(config_path)],
            cwd=self.package,
            env=scheduler_env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        try:
            output, _ = process.communicate("quit\n", timeout=20)
        finally:
            if process.poll() is None:
                stop_process_tree(process.pid)
                process.wait(timeout=5)
        self.assertEqual(process.returncode, 0, output)
        self.assertIn("scheduler started", output)


class SmokeOptions:
    value: argparse.Namespace


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", default=str(ROOT / "build" / "server-package-smoke-final"))
    parser.add_argument("--pg-bin", default=str(ROOT / "third_party" / "pgsql" / "bin"))
    parser.add_argument("-v", "--verbose", action="store_true")
    SmokeOptions.value = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ServerPackageSmoke)
    result = unittest.TextTestRunner(verbosity=2 if SmokeOptions.value.verbose else 1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
