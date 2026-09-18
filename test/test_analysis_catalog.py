from __future__ import annotations

import json
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.config import ServiceConfig
from result_service.app.providers import ProviderResult
from result_service.app.repository import PostgresRepository, result_readiness
from result_service.app.server import ResultService


class Rows:
    def __init__(self, values):
        self.values = values

    def fetchall(self):
        return self.values

    def fetchone(self):
        return self.values[0] if self.values else None


class DurableDatabase:
    """No scheduler tables or plugin execution code exist in this fixture."""
    def __init__(self):
        self.queries = []

    @contextmanager
    def transaction(self):
        yield

    def execute(self, query, params=()):
        self.queries.append(query)
        if "FROM tasks" in query or "JOIN runs" in query or "FROM runs" in query:
            raise AssertionError("analysis cannot require scheduler tables")
        if query.startswith("SELECT 1"):
            return Rows([])
        if query.startswith(("SELECT r.run_id", "SELECT p.run_id")):
            return Rows([{"run_id": "archived-run", "plugin_type": "coverage",
                          "expected_files": 3, "success_files": 1, "failed_files": 1,
                          "status": "planned"}])
        if query.startswith("SELECT status"):
            return Rows([{"status": "success", "count": 1}, {"status": "failed", "count": 1}])
        if "information_schema.columns" in query:
            return Rows([{"table_schema": "public", "table_name": "vendor_result",
                          "column_name": "run_id", "data_type": "text"}])
        raise AssertionError(query)


class AnalysisCatalogTests(unittest.TestCase):
    def repository(self):
        database = DurableDatabase()
        repository = PostgresRepository("unused", {})

        @contextmanager
        def connect():
            yield database

        repository._connect = connect
        return repository, database

    def test_analysis_catalog_and_detail_need_only_durable_database(self):
        repository, database = self.repository()
        self.assertTrue(repository.ready())
        runs = repository.analysis_runs()
        detail = repository.analysis_run("archived-run", "coverage")
        self.assertEqual("partial", runs[0]["result_readiness"]["status"])
        self.assertEqual(1, detail["result_readiness"]["missing_files"])
        self.assertEqual(1, detail["result_completeness"]["pending_files"])
        self.assertEqual(2, detail["result_completeness"]["reported_files"])
        self.assertTrue(detail["result_readiness"]["can_analyze"])
        self.assertEqual("public.vendor_result", repository.database_schema()[0]["name"])
        self.assertFalse(any("FROM tasks" in query for query in database.queries))

    def test_readiness_is_not_inferred_from_execution_or_settlement(self):
        complete = result_readiness({"status": "running", "expected_files": 3,
                                     "success_files": 2, "failed_files": 1})
        empty = result_readiness({"status": "finished", "expected_files": 3})
        unknown = result_readiness({"success_files": 1})
        self.assertEqual("complete", complete["status"])
        self.assertEqual(1, complete["failed_files"])
        self.assertEqual("empty", empty["status"])
        self.assertFalse(empty["can_analyze"])
        self.assertEqual("unknown", unknown["status"])

    def test_api_forwards_history_search_and_paging(self):
        service = ResultService(ServiceConfig.defaults())
        with patch.object(service.repository, "list_runs", return_value=[]) as listing:
            status, _ = service.payload("/api/runs", {"limit": ["51"], "offset": ["100"], "search": ["old_%"]})
            self.assertEqual(200, status)
            listing.assert_called_once_with(None, 51, 100, "old_%")
        with patch.object(service.repository, "analysis_runs", return_value=[]) as listing:
            status, _ = service.payload("/api/analysis/catalog", {"plugin": ["coverage"], "limit": ["50"], "offset": ["100"], "search": ["old"]})
            self.assertEqual(200, status)
            listing.assert_called_once_with("coverage", 50, 100, "old")

    def test_report_preflight_does_not_execute_legacy_summary_adapter(self):
        repository, _ = self.repository()

        class LegacyAdapter:
            configured = True

            def summary(self, connection, run_id):
                raise AssertionError("independent report must not invoke the summary adapter")

        repository.plugins["coverage"] = LegacyAdapter()
        detail = repository.analysis_run("archived-run", "coverage", include_summary=False)
        self.assertEqual("archived-run", detail["run_id"])
        self.assertNotIn("analysis", detail)

    def test_ai_receives_live_database_catalog_without_selected_run(self):
        service = ResultService(ServiceConfig.defaults())
        service.repository, _ = self.repository()
        with patch.object(service.providers["codex"], "complete", return_value=ProviderResult("codex", "ok")) as complete:
            status, _ = service.ai_chat({"message": "哪些结果可以分析？", "context": {}})
        self.assertEqual(200, status)
        context = complete.call_args.args[1]
        self.assertEqual("archived-run", context["analysis_catalog"]["runs"][0]["run_id"])
        self.assertNotIn("db_conn", json.dumps(context))

    def test_failed_database_does_not_block_general_ai_questions(self):
        service = ResultService(ServiceConfig.defaults())
        service.repository, _ = self.repository()
        with patch.object(service.repository, "analysis_runs", side_effect=RuntimeError("offline")), \
                patch.object(service.providers["codex"], "complete", return_value=ProviderResult("codex", "ok")) as complete:
            status, _ = service.ai_chat({"message": "为什么没运行？"})
        self.assertEqual(200, status)
        self.assertEqual("database_unavailable", complete.call_args.args[1]["analysis_catalog"]["error"])

    def test_configuration_resolves_report_paths_after_move(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config_dir = root / "config"
            config_dir.mkdir()
            config_file = config_dir / "result_service.json"
            config_file.write_text(json.dumps({"analysis_scripts_dir": "../data/analysis_scripts",
                                               "analysis_reports_dir": "../data/reports",
                                               "report_templates_dir": "../result_service/report_templates"}))
            config = ServiceConfig.from_file(config_file)
        self.assertEqual("0.0.0.0", config.host)
        self.assertEqual(str(root / "data" / "analysis_scripts"), config.analysis_scripts_dir)
        self.assertEqual(str(root / "data" / "reports"), config.analysis_reports_dir)


if __name__ == "__main__":
    unittest.main()
