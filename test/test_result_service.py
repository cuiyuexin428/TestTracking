from __future__ import annotations

import os
import sys
import tempfile
import unittest
from contextlib import contextmanager
from dataclasses import replace
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.config import ProviderConfig, ServiceConfig
from result_service.app.plugins import ResultPluginError, default_plugins
from result_service.app.providers import (
    DshWebManager,
    _argument_prompt,
    _build_prompt,
    _command_argv,
    parse_provider_output,
    ProviderError,
    resolve_provider_command,
)
from result_service.app.repository import PostgresRepository
from result_service.app.server import Handler, ResultHTTPServer, ResultService


class _Rows:
    def __init__(self, one=None, all_rows=None):
        self.one = one
        self.all_rows = all_rows or []

    def fetchone(self):
        return self.one

    def fetchall(self):
        return self.all_rows


class _RepositoryConnection:
    def execute(self, query, params=()):
        if query.startswith("SELECT p.run_id"):
            return _Rows(one={"run_id": "run-1", "plugin_type": "coverage",
                              "expected_files": 2, "status": "finished"})
        if query.startswith("SELECT status"):
            return _Rows(all_rows=[{"status": "success", "count": 1}])
        raise AssertionError(f"unexpected query: {query}")

    @contextmanager
    def transaction(self):
        yield


class _AnalysisPlugin:
    name = "coverage"

    def __init__(self, configured=True, result=None, error=False):
        self.configured = configured
        self.result = result or {}
        self.error = error

    def summary(self, connection, run_id):
        if self.error:
            raise ResultPluginError("query failed")
        return self.result


class _CoverageConnection:
    def __init__(self, relation, summary=None):
        self.relation = relation
        self.summary = summary
        self.summary_queried = False

    def execute(self, query, params=()):
        if query.startswith("SELECT to_regclass"):
            return _Rows(one={"relation": self.relation})
        if "FROM coverage_run_summary" in query:
            self.summary_queried = True
            return _Rows(one=self.summary)
        raise AssertionError(f"unexpected query: {query}")


class _ReadyConnection:
    def __init__(self, fail_on=""):
        self.fail_on = fail_on
        self.queries = []

    def execute(self, query, params=()):
        self.queries.append(query)
        if self.fail_on and self.fail_on in query:
            raise RuntimeError("relation unavailable")
        return _Rows()


class _ListRunsConnection:
    def __init__(self, runs, task_counts=None, scheduler_runs=None):
        self.runs = runs
        self.task_counts = task_counts or []
        self.scheduler_runs = scheduler_runs or []
        self.queries = []

    @contextmanager
    def transaction(self):
        yield

    def execute(self, query, params=()):
        self.queries.append((query, params))
        if query.startswith("SELECT r.run_id"):
            return _Rows(all_rows=self.runs)
        if query.startswith("SELECT run_id, status, count(*)"):
            return _Rows(all_rows=self.task_counts)
        if query.startswith("SELECT run_id, status, finished_at FROM runs"):
            return _Rows(all_rows=self.scheduler_runs)
        raise AssertionError(f"unexpected query: {query}")


class ResultServiceContractTests(unittest.TestCase):
    operator_id = "123e4567-e89b-42d3-a456-426614174001"

    def setUp(self) -> None:
        self.service = ResultService(ServiceConfig.defaults())

    def test_unconfigured_database_is_not_ready(self) -> None:
        status, payload = self.service.payload("/readyz", {})

        self.assertEqual(503, status)
        self.assertEqual("not_ready", payload["status"])
        self.assertFalse(payload["database_configured"])
        self.assertEqual("database_not_configured", payload["reason"])

    def test_run_detail_requires_plugin_identity(self) -> None:
        status, payload = self.service.payload("/api/runs/run-1", {})

        self.assertEqual(400, status)
        self.assertEqual("plugin query parameter is required", payload["error"])

    def test_run_detail_rejects_unknown_plugin(self) -> None:
        status, payload = self.service.payload("/api/runs/run-1", {"plugin": ["missing"]})

        self.assertEqual(400, status)
        self.assertEqual("unknown plugin: missing", payload["error"])

    def test_native_ai_does_not_depend_on_database_or_selected_run(self) -> None:
        with patch.object(self.service.repository, "ready", side_effect=AssertionError("offline")), \
                patch.object(self.service.repository, "analysis_runs", side_effect=AssertionError("offline")), \
                patch.object(self.service.dsh_web, "available", return_value=True), \
                patch.object(self.service.dsh_web, "start") as start:
            status, payload = self.service.start_dsh_web(self.operator_id)
        self.assertEqual(200, status)
        self.assertEqual({"url": f"/ai/dsh/{self.operator_id}/"}, payload)
        start.assert_called_once_with(self.operator_id)

    def test_non_dsh_provider_configuration_is_not_exposed(self) -> None:
        defaults = ServiceConfig.defaults()
        configured = dict(defaults.providers)
        configured["codex"] = ProviderConfig(name="codex", command="codex")
        service = ResultService(replace(defaults, providers=configured))
        try:
            status, payload = service.payload("/api/ai/providers", {})
            self.assertEqual(200, status)
            self.assertEqual(["dsh"], [item["name"] for item in payload])
        finally:
            service.close()

    def test_ai_uses_service_workspace_unless_provider_sets_its_own(self) -> None:
        defaults = ServiceConfig.defaults()
        providers = dict(defaults.providers)
        providers["dsh"] = replace(providers["dsh"], cwd="provider-workspace")

        service = ResultService(replace(defaults, workspace="service-workspace", providers=providers))

        self.assertEqual("provider-workspace", service.dsh_web.config.cwd)


@unittest.skipUnless(os.name == "nt", "Windows exclusive socket behavior")
class ResultHTTPServerTests(unittest.TestCase):
    def test_second_service_cannot_share_the_same_port(self) -> None:
        first = ResultHTTPServer(("127.0.0.1", 0), Handler)
        try:
            port = first.server_address[1]
            with self.assertRaises(OSError):
                second = ResultHTTPServer(("127.0.0.1", port), Handler)
                second.server_close()
        finally:
            first.server_close()


class ProviderPromptTests(unittest.TestCase):
    def test_general_prompt_preserves_question_and_removes_credentials(self) -> None:
        prompt = _build_prompt("help me update the report", {
            "run": {"run_id": "run-1", "password": "example-secret"},
            "db_conn": "example-connection",
        })

        self.assertIn("help me update the report", prompt)
        self.assertIn('"run_id": "run-1"', prompt)
        self.assertNotIn("example-secret", prompt)
        self.assertNotIn("example-connection", prompt)
        self.assertNotIn("Use only the supplied context", prompt)
        self.assertNotIn("Do not execute SQL or modify files", prompt)

    def test_argument_prompt_keeps_context_on_one_command_line(self) -> None:
        prompt = "Instruction\n\nResult context (JSON):\n{\"run_id\":\"run-1\"}\n\nQuestion:\nSummarize"

        result = _argument_prompt(prompt)

        self.assertNotIn("\n", result)
        self.assertIn('{"run_id":"run-1"}', result)
        self.assertTrue(result.endswith("Question: Summarize"))

    def test_plugin_view_schema_reaches_provider_without_depth_truncation(self) -> None:
        schema = ResultService(ServiceConfig.defaults()).plugin_views.schema()

        prompt = _build_prompt("design a result view", {
            "purpose": "plugin_result_view_design",
            "view_schema": schema,
        })

        self.assertNotIn("<depth-limit>", prompt)
        self.assertIn('"data_table"', prompt)
        self.assertIn('"required": ["id", "type", "title", "source", "columns"]', prompt)

    def test_plain_multiline_provider_output_is_preserved(self) -> None:
        output = "Summary:\n- first\n- second\n"

        self.assertEqual("Summary:\n- first\n- second", parse_provider_output("dsh", output))

    def test_jsonl_provider_output_uses_final_message(self) -> None:
        output = '\n'.join([
            '{"type":"thread.started","thread_id":"one"}',
            '{"type":"item.completed","item":{"type":"agent_message","text":"done"}}',
        ])

        self.assertEqual("done", parse_provider_output("dsh", output))


class DshWebManagerTests(unittest.TestCase):
    operator_a = "123e4567-e89b-42d3-a456-426614174001"
    operator_b = "123e4567-e89b-42d3-a456-426614174002"

    def test_starts_once_and_reuses_the_ready_web_process(self) -> None:
        # Existing deployments may still contain the removed chat settings.
        config = replace(ServiceConfig.defaults().providers["dsh"],
                         args=("--profile", "headless"), prompt_mode="argument")
        manager = DshWebManager(config)
        process = MagicMock()
        process.poll.return_value = None
        response = MagicMock()
        response.__enter__.return_value.status = 200
        response.__enter__.return_value.read.return_value = b'__DSH_BOOT__'

        with patch("result_service.app.providers.resolve_provider_command", return_value="dsh"), \
                patch.object(manager, "_free_loopback_port", return_value=56335), \
                patch("result_service.app.providers.subprocess.Popen", return_value=process) as popen, \
                patch("result_service.app.providers.urlopen", return_value=response), \
                patch.object(manager, "_api_ready", return_value=True):
            first_url = manager.start(self.operator_a)
            second_url = manager.start(self.operator_a)

        self.assertEqual("http://127.0.0.1:56335/", first_url)
        self.assertEqual(first_url, second_url)
        popen.assert_called_once()
        argv = popen.call_args.args[0]
        self.assertIn("--patch", argv)
        self.assertIn("web", argv)
        self.assertNotIn("headless", argv)
        self.assertEqual(["--host", "127.0.0.1", "--port", "56335", "--no-open"], argv[-5:])

        with patch.object(manager, "_stop_process") as stop_process:
            manager.close()
        stop_process.assert_called_once_with(process)

    def test_different_operators_get_different_processes_and_idle_capacity_is_reclaimed(self) -> None:
        manager = DshWebManager(
            ServiceConfig.defaults().providers["dsh"], max_sessions=1, idle_timeout=10
        )
        processes = [MagicMock(), MagicMock()]
        for process in processes:
            process.poll.return_value = None
        response = MagicMock()
        response.__enter__.return_value.status = 200
        response.__enter__.return_value.read.return_value = b"__DSH_BOOT__"
        with patch("result_service.app.providers.resolve_provider_command", return_value="dsh"), \
                patch.object(manager, "_free_loopback_port", side_effect=[56335, 56336]), \
                patch("result_service.app.providers.subprocess.Popen", side_effect=processes) as popen, \
                patch("result_service.app.providers.urlopen", return_value=response), \
                patch.object(manager, "_api_ready", return_value=True), \
                patch.object(manager, "_stop_process") as stop_process:
            self.assertEqual("http://127.0.0.1:56335/", manager.start(self.operator_a))
            with self.assertRaisesRegex(ProviderError, "session limit"):
                manager.start(self.operator_b)
            manager._sessions[self.operator_a].last_used -= 11
            manager._sessions[self.operator_a].active_requests = 1
            with self.assertRaisesRegex(ProviderError, "session limit"):
                manager.start(self.operator_b)
            manager._sessions[self.operator_a].active_requests = 0
            self.assertEqual("http://127.0.0.1:56336/", manager.start(self.operator_b))
            self.assertNotIn(self.operator_a, manager._sessions)
            self.assertIn(self.operator_b, manager._sessions)
            self.assertEqual(2, popen.call_count)
            stop_process.assert_called_once_with(processes[0])
            manager.close()

    def test_service_reports_dsh_web_availability_and_url(self) -> None:
        service = ResultService(ServiceConfig.defaults())
        with patch.object(service.dsh_web, "available", return_value=True), \
                patch.object(service.dsh_web, "start", return_value="http://127.0.0.1:56335/"):
            status, payload = service.start_dsh_web(self.operator_a)

        self.assertEqual(200, status)
        self.assertEqual({"url": f"/ai/dsh/{self.operator_a}/"}, payload)

    def test_service_does_not_start_dsh_web_when_command_is_unavailable(self) -> None:
        service = ResultService(ServiceConfig.defaults())
        with patch.object(service.dsh_web, "available", return_value=False), \
                patch.object(service.dsh_web, "start") as start:
            status, payload = service.start_dsh_web(self.operator_a)

        self.assertEqual(503, status)
        self.assertEqual("dsh executable is not available", payload["error"])
        start.assert_not_called()

    def test_service_rejects_missing_operator_before_provider_discovery(self) -> None:
        service = ResultService(ServiceConfig.defaults())
        with patch.object(service.dsh_web, "available") as available, \
                patch.object(service.dsh_web, "start") as start:
            status, payload = service.start_dsh_web(None)
        self.assertEqual(400, status)
        self.assertEqual("invalid operator_id", payload["error"])
        available.assert_not_called()
        start.assert_not_called()


class RepositoryReadinessTests(unittest.TestCase):
    def repository_with(self, connection):
        repository = PostgresRepository("unused", {})

        @contextmanager
        def connect():
            yield connection

        repository._connect = connect
        return repository

    def test_ready_requires_framework_result_tables(self) -> None:
        connection = _ReadyConnection()

        ready = self.repository_with(connection).ready()

        self.assertTrue(ready)
        self.assertEqual([
            "SELECT 1 FROM plugin_runs LIMIT 0",
            "SELECT 1 FROM plugin_file_outcomes LIMIT 0",
        ], connection.queries)

    def test_missing_framework_result_table_is_not_ready(self) -> None:
        connection = _ReadyConnection("plugin_file_outcomes")

        self.assertFalse(self.repository_with(connection).ready())

    def test_analysis_readiness_does_not_require_scheduler_tasks(self) -> None:
        connection = _ReadyConnection("FROM tasks")

        self.assertTrue(self.repository_with(connection).ready())


class RepositoryRunProgressTests(unittest.TestCase):
    def repository_with(self, connection):
        repository = PostgresRepository("unused", {})

        @contextmanager
        def connect():
            yield connection

        repository._connect = connect
        return repository

    def test_list_runs_includes_task_totals_and_status_counts(self) -> None:
        runs = [{
            "run_id": "run-1", "plugin_type": "coverage", "success_files": 4,
            "failed_files": 1,
        }]
        connection = _ListRunsConnection(runs, [
            {"run_id": "run-1", "status": "reported", "count": 4},
            {"run_id": "run-1", "status": "acked", "count": 2},
            {"run_id": "run-1", "status": "dispatched", "count": 1},
            {"run_id": "run-1", "status": "reclaimed", "count": 1},
            {"run_id": "run-1", "status": "queued", "count": 3},
        ])

        result = self.repository_with(connection).list_runs("coverage", 25)

        self.assertEqual(11, result[0]["total_tasks"])
        self.assertEqual({
            "reported": 4,
            "acked": 2,
            "dispatched": 1,
            "reclaimed": 1,
            "pending": 0,
            "queued": 3,
        }, result[0]["task_status_counts"])
        self.assertEqual(4, result[0]["success_files"])
        self.assertEqual(1, result[0]["failed_files"])
        task_query, task_params = next(
            item for item in connection.queries if "FROM tasks" in item[0]
        )
        self.assertIn("FROM tasks", task_query)
        self.assertEqual((["run-1"],), task_params)

    def test_list_runs_returns_zero_progress_without_task_rows(self) -> None:
        runs = [{
            "run_id": "legacy-run", "plugin_type": "coverage", "success_files": 2,
            "failed_files": 0,
        }]
        connection = _ListRunsConnection(runs)

        result = self.repository_with(connection).list_runs()

        self.assertEqual(0, result[0]["total_tasks"])
        self.assertEqual({
            "reported": 0,
            "acked": 0,
            "dispatched": 0,
            "reclaimed": 0,
            "pending": 0,
        }, result[0]["task_status_counts"])
        self.assertEqual(2, result[0]["success_files"])
        self.assertTrue(any("FROM tasks" in query for query, _ in connection.queries))

    def test_planned_task_count_includes_tasks_not_dispatched_yet(self) -> None:
        runs = [{
            "run_id": "run-1", "plugin_type": "coverage", "planned_tasks": 20,
            "success_files": 4, "failed_files": 1,
        }]
        connection = _ListRunsConnection(runs, [
            {"run_id": "run-1", "status": "reported", "count": 8},
            {"run_id": "run-1", "status": "acked", "count": 2},
            {"run_id": "run-1", "status": "reclaimed", "count": 1},
        ])

        result = self.repository_with(connection).list_runs()

        self.assertEqual(20, result[0]["total_tasks"])
        self.assertEqual(9, result[0]["task_status_counts"]["pending"])

    def test_scheduler_run_status_overrides_plugin_plan_status(self) -> None:
        runs = [{
            "run_id": "run-1", "plugin_type": "coverage", "status": "planned",
            "finished_at": None, "success_files": 0, "failed_files": 0,
        }]
        connection = _ListRunsConnection(
            runs,
            scheduler_runs=[{
                "run_id": "run-1", "status": "running", "finished_at": None,
            }],
        )

        result = self.repository_with(connection).list_runs()

        self.assertEqual("running", result[0]["status"])

    def test_finished_pruned_run_keeps_complete_progress(self) -> None:
        runs = [{
            "run_id": "old-run", "plugin_type": "coverage", "status": "finished",
            "planned_tasks": 12, "success_files": 12, "failed_files": 0,
        }]

        result = self.repository_with(_ListRunsConnection(runs)).list_runs()

        self.assertEqual(12, result[0]["total_tasks"])
        self.assertEqual(12, result[0]["task_status_counts"]["reported"])
        self.assertEqual(0, result[0]["task_status_counts"]["pending"])


class RepositoryAnalysisStatusTests(unittest.TestCase):
    def get_run(self, plugin):
        repository = PostgresRepository("unused", {"coverage": plugin})

        @contextmanager
        def connect():
            yield _RepositoryConnection()

        repository._connect = connect
        return repository.get_run("run-1", "coverage")

    def test_published_analysis_is_ready(self) -> None:
        result = self.get_run(_AnalysisPlugin(result={"percent": 80}))

        self.assertEqual("ready", result["analysis_status"])
        self.assertEqual({"percent": 80}, result["analysis"])

    def test_run_detail_uses_scheduler_execution_status(self) -> None:
        class Connection(_RepositoryConnection):
            def execute(self, query, params=()):
                if query.startswith("SELECT p.run_id"):
                    return _Rows(one={
                        "run_id": "run-1", "plugin_type": "coverage",
                        "expected_files": 2, "status": "running",
                    })
                return super().execute(query, params)

        repository = PostgresRepository("unused", {
            "coverage": _AnalysisPlugin(result={"percent": 50}),
        })

        @contextmanager
        def connect():
            yield Connection()

        repository._connect = connect

        self.assertEqual("running", repository.get_run("run-1", "coverage")["status"])

    def test_missing_analysis_row_is_pending(self) -> None:
        result = self.get_run(_AnalysisPlugin())

        self.assertEqual("pending", result["analysis_status"])

    def test_unconfigured_adapter_is_reported(self) -> None:
        result = self.get_run(_AnalysisPlugin(configured=False))

        self.assertEqual("not_configured", result["analysis_status"])

    def test_analysis_query_failure_does_not_hide_run(self) -> None:
        with self.assertLogs("result_service.repository", level="WARNING"):
            result = self.get_run(_AnalysisPlugin(error=True))

        self.assertEqual("error", result["analysis_status"])
        self.assertEqual({}, result["analysis"])
        self.assertEqual("finished", result["status"])


class CoverageAnalysisTests(unittest.TestCase):
    def test_missing_summary_table_is_pending(self) -> None:
        connection = _CoverageConnection(None)

        result = default_plugins(None)["coverage"].summary(connection, "run-1")

        self.assertEqual({}, result)
        self.assertFalse(connection.summary_queried)

    def test_existing_summary_table_returns_run_result(self) -> None:
        summary = {"run_id": "run-1", "percent": 80}
        connection = _CoverageConnection("coverage_run_summary", summary)

        result = default_plugins(None)["coverage"].summary(connection, "run-1")

        self.assertEqual(summary, result)
        self.assertTrue(connection.summary_queried)


@unittest.skipUnless(os.name == "nt", "Windows provider discovery")
class WindowsProviderDiscoveryTests(unittest.TestCase):
    def test_dsh_npm_command_uses_cmd_wrapper(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            executable = Path(temp_dir) / "npm" / "dsh.cmd"
            executable.parent.mkdir(parents=True)
            executable.touch()
            config = ProviderConfig(name="dsh", command="dsh", args=("--profile", "headless"))
            environment = {"APPDATA": temp_dir, "LOCALAPPDATA": "", "USERPROFILE": "", "PNPM_HOME": ""}
            with patch.dict(os.environ, environment, clear=True), \
                    patch("result_service.app.providers.shutil.which", return_value=None):
                resolved = resolve_provider_command(config)
                argv = _command_argv(config, resolved)

            self.assertEqual(["cmd.exe", "/d", "/c", str(executable.resolve()),
                              "--profile", "headless"], argv)

    def test_portable_dsh_command_is_found_without_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            executable = Path(temp_dir) / "dsh" / "dsh.cmd"
            executable.parent.mkdir(parents=True)
            executable.touch()
            config = ProviderConfig(name="dsh", command="dsh")
            environment = {"LOCALAPPDATA": temp_dir, "APPDATA": "", "USERPROFILE": "", "PNPM_HOME": ""}
            with patch.dict(os.environ, environment, clear=True), \
                    patch("result_service.app.providers.shutil.which", return_value=None):
                resolved = resolve_provider_command(config)

            self.assertEqual(executable.resolve(), Path(resolved))

    def test_directory_is_not_an_available_command(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config = ProviderConfig(name="custom", command=temp_dir)
            with patch("result_service.app.providers.shutil.which", return_value=None):
                self.assertIsNone(resolve_provider_command(config))

    def test_bare_command_does_not_use_same_named_working_directory_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            command_file = Path(temp_dir) / "dsh"
            command_file.write_text("not an executable", encoding="utf-8")
            config = ProviderConfig(name="dsh", command="dsh")
            environment = {"LOCALAPPDATA": "", "APPDATA": "", "USERPROFILE": "", "PNPM_HOME": ""}
            previous_cwd = os.getcwd()
            try:
                os.chdir(temp_dir)
                with patch.dict(os.environ, environment, clear=True), \
                        patch("result_service.app.providers.shutil.which", return_value=None):
                    self.assertIsNone(resolve_provider_command(config))
            finally:
                os.chdir(previous_cwd)

    def test_powershell_script_requires_an_available_host(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            script = Path(temp_dir) / "provider.ps1"
            script.touch()
            config = ProviderConfig(name="custom", command=str(script))
            with patch("result_service.app.providers.shutil.which", return_value=None):
                self.assertIsNone(resolve_provider_command(config))


if __name__ == "__main__":
    unittest.main()
