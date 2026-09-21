"""Report persistence tests and optional real PostgreSQL/worker integration.

python test/test_reports.py --pg-bin third_party/pgsql/bin \
    --report-python build/server-package-smoke-final/runtime/python/python.exe -v
"""
from __future__ import annotations

import argparse
from base64 import b64decode
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from result_service.app.reports import ReportError, ReportStore

OPTIONS = None


def load_coverage_report():
    spec = importlib.util.spec_from_file_location(
        "testtracking_coverage_report", ROOT / "plugins/coverage/result_report.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReportStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="report store ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = ReportStore(self.root / "scripts", self.root / "reports")

    def test_edit_is_reloadable_and_invalid_edit_preserves_script(self):
        source = 'def render(connection, context):\n    return "<h1>新报告</h1>"\n'
        self.store.save_script("coverage", source)
        with self.assertRaises(ReportError):
            self.store.save_script("coverage", "def render(: invalid")
        reopened = ReportStore(self.store.scripts_dir, self.store.artifacts_dir)
        self.assertEqual(reopened.get_script("coverage")["source"], source)
        self.assertEqual(reopened.get_script("coverage")["origin"], "runtime")

    def test_script_revision_prevents_overwrite_and_records_operator_audit(self):
        original = self.store.get_script("coverage")
        source = 'def render(connection, context):\n    return "revision one"\n'
        saved = self.store.save_script(
            "coverage", source, original["revision"], "123e4567-e89b-42d3-a456-426614174000"
        )
        self.assertNotEqual(original["revision"], saved["revision"])
        with self.assertRaisesRegex(ReportError, "changed after it was loaded") as conflict:
            self.store.save_script("coverage", source.replace("one", "two"), original["revision"])
        self.assertEqual(409, conflict.exception.status)
        audit = json.loads((self.store.scripts_dir / "audit.jsonl").read_text(encoding="utf-8"))
        self.assertEqual("123e4567-e89b-42d3-a456-426614174000", audit["operator_id"])
        self.assertEqual(saved["revision"], audit["revision"])

    def test_template_directory_and_generic_template(self):
        templates = self.root / "templates"
        templates.mkdir()
        source = 'def render(connection, context):\n    return "packaged"'
        (templates / "coverage.py").write_text(source)
        store = ReportStore(self.store.scripts_dir, self.store.artifacts_dir, templates)
        self.assertEqual(store.get_script("coverage")["source"], source)
        fallback = store.get_script("vendor-plugin")
        self.assertEqual(fallback["origin"], "generic")
        store._validate_script(fallback["source"])

    def test_only_coverage_ships_a_plugin_analysis_script(self):
        self.assertEqual(self.store.get_script("coverage")["origin"], "default")
        self.assertEqual(self.store.get_script("stpcompare")["origin"], "generic")

    def test_coverage_compact_set_prefers_smaller_file_then_path(self):
        report = load_coverage_report()
        pack = lambda values: b"".join(value.to_bytes(8, "little") for value in values)
        rows = [
            {"file": "large.stp", "size_bytes": 20, "status": "success", "point_data": pack([1, 2])},
            {"file": "small-z.stp", "size_bytes": 10, "status": "success", "point_data": pack([1, 2])},
            {"file": "small-a.stp", "size_bytes": 10, "status": "success", "point_data": pack([1, 2])},
            {"file": "extra.stp", "size_bytes": 1, "status": "success", "point_data": pack([3])},
        ]
        selected, covered = report._select_compact_cover(rows)
        self.assertEqual(["small-a.stp", "extra.stp"], [item["file"] for item in selected])
        self.assertEqual({1, 2, 3}, covered)

    def test_worker_receives_database_privately_and_artifact_is_immutable(self):
        captured = []

        def worker(command, **kwargs):
            captured.append(json.loads(kwargs["input"]))
            Path(command[-1]).write_text("<!doctype html><h1>报告</h1>", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, "", "")

        with patch("result_service.app.reports.subprocess.run", side_effect=worker):
            result = self.store.generate("coverage", "run-1", "password=SECRET", parameters={"file_filter": "a"})
        report = self.store.artifact_path(result["report_id"])
        self.assertIn("报告", report.read_text(encoding="utf-8"))
        self.assertEqual(captured[0]["context"]["parameters"], {"file_filter": "a"})
        self.assertEqual(captured[0]["db_conn"], "password=SECRET")
        self.assertEqual(result["url"], f'/reports/{result["report_id"]}/index.html')
        for path in report.parent.iterdir():
            self.assertNotIn("SECRET", path.read_text(encoding="utf-8"))
        self.assertTrue((report.parent / "script.py").is_file())

    def test_failed_or_timed_out_run_keeps_existing_report(self):
        old = self.store.artifacts_dir / ("a" * 32)
        old.mkdir(parents=True)
        (old / "index.html").write_text("previous")
        failures = [subprocess.CompletedProcess([], 1, "", "bad query"),
                    subprocess.TimeoutExpired([], 0.01)]
        for failure in failures:
            kwargs = {"side_effect": failure} if isinstance(failure, Exception) else {"return_value": failure}
            with patch("result_service.app.reports.subprocess.run", **kwargs), self.assertRaises(ReportError):
                self.store.generate("coverage", "run-1", "host=localhost")
            self.assertEqual((old / "index.html").read_text(), "previous")
            self.assertEqual(list(self.store.artifacts_dir.iterdir()), [old])

    def test_only_report_identifier_can_resolve_artifact(self):
        for value in ("../scripts/coverage.py", "", "a" * 32 + "/index.html"):
            with self.assertRaises(ReportError):
                self.store.artifact_path(value)

    def test_latest_ignores_uncommitted_missing_and_other_run_reports(self):
        self.assertIsNone(self.store.latest("coverage", "run-1"))
        cases = [("1" * 32, "coverage", "run-1", "2026-09-16T10:00:00+00:00", True),
                 ("2" * 32, "coverage", "run-1", "2026-09-16T11:00:00+00:00", True),
                 ("3" * 32, "coverage", "other", "2026-09-16T12:00:00+00:00", True),
                 ("4" * 32, "stpcompare", "run-1", "2026-09-16T12:00:00+00:00", True),
                 ("5" * 32, "coverage", "run-1", "2026-09-16T12:00:00+00:00", False),
                 ("." + "6" * 32 + ".tmp", "coverage", "run-1", "2026-09-16T12:00:00+00:00", True)]
        for report_id, plugin, run_id, timestamp, has_html in cases:
            directory = self.store.artifacts_dir / report_id
            directory.mkdir(parents=True)
            (directory / "metadata.json").write_text(json.dumps({
                "report_id": report_id, "plugin": plugin, "run_id": run_id, "created_at": timestamp,
                "url": f"/reports/{report_id}/index.html",
            }), encoding="utf-8")
            if has_html:
                (directory / "index.html").write_text("report", encoding="utf-8")
        self.assertEqual(self.store.latest("coverage", "run-1")["report_id"], "2" * 32)


class ReportDatabaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if OPTIONS is None or OPTIONS.pg_bin is None:
            raise unittest.SkipTest("pass --pg-bin for real PostgreSQL report tests")
        sys.path.insert(0, str(ROOT / "test"))
        from test_coverage_settle import PrivatePostgres
        cls.pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.pg_bin / "psql.exe")
        cls.addClassCleanup(cls.pg.close)
        cls.pg.start()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="report integration ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = ReportStore(self.root / "scripts", self.root / "reports",
                                 python_executable=OPTIONS.report_python)
        self.pg.sql("DROP SCHEMA public CASCADE; CREATE SCHEMA public;" + """
            CREATE TABLE plugin_runs(run_id text, plugin_type text, expected_files int,
                                     status text, dataset_version text, program_version text,
                                     created_at timestamptz DEFAULT now(), finished_at timestamptz,
                                     manifest_fingerprint text, PRIMARY KEY(run_id,plugin_type));
            CREATE TABLE plugin_run_files(run_id text, plugin_type text, file text,
                                          size_bytes bigint, content_sha256 text);
            CREATE TABLE plugin_file_outcomes(run_id text, plugin_type text, file text, status text, detail text);
            INSERT INTO plugin_runs(run_id,plugin_type,expected_files,status,dataset_version,program_version)
                VALUES('cov','coverage',2,'finished','v1','v1'),('stp','stpcompare',2,'finished','v1','v1');
            INSERT INTO plugin_run_files(run_id,plugin_type,file,size_bytes,content_sha256)
                VALUES('cov','coverage','a.stp',20,repeat('a',64)),
                      ('cov','coverage','b.stp',10,repeat('b',64)),
                      ('stp','stpcompare','same.stp',NULL,NULL),
                      ('stp','stpcompare','<diff>.stp',NULL,NULL);
            INSERT INTO plugin_file_outcomes SELECT run_id,plugin_type,file,'success','' FROM plugin_run_files;
            CREATE TABLE coverage_file_points(run_id text,file text,points bytea);
            INSERT INTO coverage_file_points VALUES('cov','a.stp',decode('01000000000000000200000000000000','hex')),
                                                  ('cov','b.stp',decode('01000000000000000200000000000000','hex'));
            CREATE TABLE custom_stp_results(run_id text,file text,info_txt text,info_new_txt text);
            INSERT INTO custom_stp_results VALUES('stp','same.stp','a','a'),
                                                 ('stp','<diff>.stp','<script>alert(1)</script>','new');
        """)

    def test_coverage_report_selects_and_downloads_compact_set(self):
        first = self.store.generate("coverage", "cov", self.pg.conn)
        text = self.store.artifact_path(first["report_id"]).read_text(encoding="utf-8")
        self.assertIn("原始数据完整", text)
        self.assertIn("a.stp", text)
        self.assertIn("coverage-selected-stp.txt", text)
        self.assertIn("精简集合文件", text)
        encoded = text.split("data:text/plain;charset=utf-8;base64,", 1)[1].split('"', 1)[0]
        self.assertEqual("b.stp\n", b64decode(encoded).decode("utf-8"))
        self.assertIn("b.stp", text)

    def test_plugin_without_built_in_analysis_uses_generic_report(self):
        result = self.store.generate("stpcompare", "stp", self.pg.conn,
                                     options={"result_table": "custom_stp_results"})
        text = self.store.artifact_path(result["report_id"]).read_text(encoding="utf-8")
        self.assertIn("stpcompare 结果报告", text)
        self.assertIn("&lt;diff&gt;.stp", text)
        self.assertNotIn("<script>alert(1)</script>", text)

    def test_replacement_script_runs_fresh_and_database_is_read_only(self):
        source = 'def render(connection, context):\n    return "<h1>version1</h1>"\n'
        self.store.save_script("coverage", source)
        first = self.store.generate("coverage", "cov", self.pg.conn)
        self.store.save_script("coverage", source.replace("version1", "version2"))
        second = self.store.generate("coverage", "cov", self.pg.conn)
        self.assertIn("version2", self.store.artifact_path(second["report_id"]).read_text())
        self.assertNotEqual(first["script_sha256"], second["script_sha256"])
        self.store.save_script("coverage", 'def render(connection, context):\n    connection.execute("DELETE FROM plugin_runs")\n    return "bad"\n')
        with self.assertRaisesRegex(ReportError, "read-only"):
            self.store.generate("coverage", "cov", self.pg.conn)
        self.assertEqual(self.pg.sql("SELECT count(*) FROM plugin_runs"), "2")
        self.assertIn("version1", self.store.artifact_path(first["report_id"]).read_text())

    def test_repository_history_and_completeness_use_only_durable_results(self):
        from result_service.app.repository import PostgresRepository, RepositoryError
        repository = PostgresRepository(self.pg.conn, {})
        self.pg.sql("""
            INSERT INTO plugin_runs(run_id,plugin_type,expected_files,status,created_at)
              SELECT 'history-' || lpad(i::text,3,'0'), 'coverage', 1, 'finished',
                     now() + i * interval '1 second' FROM generate_series(1,120) i;
            DELETE FROM plugin_file_outcomes WHERE run_id='cov' AND file='b.stp';
        """)
        newest = repository.analysis_runs("coverage", 100)
        older = repository.analysis_runs("coverage", 100, 100)
        self.assertEqual(100, len(newest))
        self.assertEqual(21, len(older))
        self.assertNotIn("cov", [run["run_id"] for run in newest])
        self.assertIn("cov", [run["run_id"] for run in older])
        self.assertEqual(["history-001"], [run["run_id"] for run in repository.analysis_runs("coverage", 50, search="history-001")])
        self.assertEqual([], repository.analysis_runs("coverage", search="history-%"))
        detail = repository.analysis_run("cov", "coverage")
        self.assertEqual(1, detail["result_completeness"]["pending_files"])
        self.assertEqual(detail["result_readiness"]["missing_files"], detail["result_completeness"]["pending_files"])
        with self.assertRaises(RepositoryError):
            with repository._connect() as connection:
                self.assertEqual("on", connection.execute("SHOW transaction_read_only").fetchone()["transaction_read_only"])
                connection.execute("DELETE FROM plugin_runs")
        self.assertEqual("122", self.pg.sql("SELECT count(*) FROM plugin_runs"))

    def test_generic_report_executes_for_unregistered_business_plugin(self):
        self.pg.sql("UPDATE plugin_runs SET plugin_type='vendor-plugin' WHERE run_id='cov';"
                    "UPDATE plugin_run_files SET plugin_type='vendor-plugin' WHERE run_id='cov';"
                    "UPDATE plugin_file_outcomes SET plugin_type='vendor-plugin' WHERE run_id='cov';")
        result = self.store.generate("vendor-plugin", "cov", self.pg.conn)
        self.assertIn("a.stp", self.store.artifact_path(result["report_id"]).read_text(encoding="utf-8"))

    def test_http_report_workflow_survives_service_restart_without_scheduler(self):
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
        config = self.root / "service.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "db_conn": self.pg.conn,
            "analysis_scripts_dir": str(self.store.scripts_dir),
            "analysis_reports_dir": str(self.store.artifacts_dir),
            "plugin_options": {"stpcompare": {"result_table": "custom_stp_results"}},
        }), encoding="utf-8")
        base = f"http://127.0.0.1:{port}"
        processes = []

        def stop(process):
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

        def request(path, body=None, method=None):
            req = Request(base + path, data=json.dumps(body).encode("utf-8") if body is not None else None,
                          headers={"Content-Type": "application/json"}, method=method)
            try:
                response = urlopen(req, timeout=15)
            except HTTPError as exc:
                response = exc
            with response:
                content = response.read().decode("utf-8")
                return response.status, (json.loads(content) if "application/json" in response.headers.get("Content-Type", "") else content)

        def start():
            env = {key: value for key, value in os.environ.items() if not key.startswith("RESULT_SERVICE_")}
            process = subprocess.Popen(
                [str(Path(OPTIONS.report_python).resolve()), "-B", str(ROOT / "result_service" / "launcher.py"),
                 "--config", str(config)], cwd=self.root, env=env,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            processes.append(process)
            self.addCleanup(stop, process)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    self.fail(f"HTTP service exited: {process.returncode}")
                try:
                    if request("/readyz")[0] == 200:
                        return process
                except (URLError, TimeoutError):
                    pass
                time.sleep(0.05)
            self.fail("HTTP service did not become ready")

        process = start()
        status, catalog = request("/api/analysis/catalog?plugin=coverage")
        self.assertEqual(status, 200, catalog)
        self.assertEqual(catalog["runs"][0]["run_id"], "cov")
        status, detail = request("/api/analysis/runs/cov?plugin=coverage")
        self.assertEqual(status, 200, detail)
        self.assertEqual(detail["run_id"], "cov")
        source = ('def render(connection, context):\n'
                  '    value = connection.execute("SELECT count(*) AS count FROM coverage_file_points WHERE run_id=%s", '
                  '(context["run_id"],)).fetchone()["count"]\n'
                  '    return "<!doctype html><h1>HTTP live rows=" + str(value) + "</h1>"\n')
        script_state = request("/api/plugins/coverage/report-script")[1]
        status, saved_script = request("/api/plugins/coverage/report-script", {
            "source": source, "revision": script_state["revision"],
            "operator_id": "123e4567-e89b-42d3-a456-426614174000",
        }, "PUT")
        self.assertEqual(status, 200)
        status, report = request("/api/plugins/coverage/reports", {"run_id": "cov"}, "POST")
        self.assertEqual(status, 200, report)
        self.assertIn("HTTP live rows=2", request(report["url"])[1])
        self.assertEqual(request("/api/plugins/coverage/reports?run_id=cov")[1]["latest"], report)
        stop(process)
        start()
        self.assertEqual(request("/api/plugins/coverage/report-script")[1]["source"], source)
        self.assertIn("HTTP live rows=2", request(report["url"])[1])
        self.assertEqual(request("/api/plugins/coverage/reports?run_id=cov")[1]["latest"], report)
        self.assertEqual(request("/api/plugins/coverage/report-script", {
            "source": 'def render(connection, context):\n    raise RuntimeError("report failure")\n',
            "revision": saved_script["revision"],
            "operator_id": "123e4567-e89b-42d3-a456-426614174000",
        }, "PUT")[0], 200)
        self.assertEqual(request("/api/plugins/coverage/reports", {"run_id": "cov"}, "POST")[0], 422)
        self.assertIn("HTTP live rows=2", request(report["url"])[1])
        self.assertEqual(request("/api/plugins/coverage/reports?run_id=cov")[1]["latest"], report)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--pg-bin", type=Path)
    parser.add_argument("--report-python", type=Path)
    OPTIONS, unittest_args = parser.parse_known_args()
    if OPTIONS.report_python is None:
        candidates = [ROOT / "runtime/python/python.exe", Path(sys.executable)]
        for candidate in candidates:
            if candidate.is_file() and subprocess.run(
                [str(candidate), "-B", "-c", "import psycopg"], capture_output=True,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            ).returncode == 0:
                OPTIONS.report_python = candidate
                break
    if OPTIONS.pg_bin is None and OPTIONS.report_python is not None:
        pg_bin = ROOT / "third_party/pgsql/bin"
        if (pg_bin / "pg_ctl.exe").is_file():
            OPTIONS.pg_bin = pg_bin
    unittest.main(argv=[sys.argv[0], *unittest_args])
