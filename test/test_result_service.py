from __future__ import annotations

import os
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from dataclasses import replace
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.config import ProviderConfig, ServiceConfig
from result_service.app.plugins import CoverageResultPlugin, ResultPluginError
from result_service.app.providers import (
    CodexProvider,
    DshWebManager,
    _argument_prompt,
    _build_prompt,
    _command_argv,
    parse_provider_output,
    ProviderError,
    ProviderResult,
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
        if query.startswith("SELECT run_id"):
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


class ResultServiceContractTests(unittest.TestCase):
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

    def test_ai_can_be_used_without_a_selected_run(self) -> None:
        provider = self.service.providers["codex"]
        with patch.object(provider, "complete", return_value=ProviderResult("codex", "ready")) as complete:
            status, payload = self.service.ai_chat({
                "provider": "codex",
                "message": "what can you do?",
                "context": {"result_type": "coverage"},
            })

        self.assertEqual(200, status)
        self.assertEqual({"provider": "codex", "text": "ready", "conversation_id": None}, payload)
        self.assertEqual("what can you do?", complete.call_args.args[0])
        self.assertEqual({"result_type": "coverage"}, complete.call_args.args[1])
        self.assertIsNone(complete.call_args.args[2])

    def test_ai_defaults_to_empty_context(self) -> None:
        provider = self.service.providers["codex"]
        with patch.object(provider, "complete", return_value=ProviderResult("codex", "ready")) as complete:
            status, payload = self.service.ai_chat({
                "provider": "codex",
                "message": "hello",
            })

        self.assertEqual(200, status)
        self.assertEqual("ready", payload["text"])
        self.assertEqual({}, complete.call_args.args[1])

    def test_ai_passes_through_codex_conversation_id(self) -> None:
        conversation_id = "123e4567-e89b-12d3-a456-426614174000"
        provider = self.service.providers["codex"]
        result = ProviderResult("codex", "continued", conversation_id=conversation_id)
        with patch.object(provider, "complete", return_value=result) as complete:
            status, payload = self.service.ai_chat({
                "provider": "codex",
                "message": "continue",
                "conversation_id": conversation_id,
            })

        self.assertEqual(200, status)
        self.assertEqual(conversation_id, payload["conversation_id"])
        self.assertEqual(conversation_id, complete.call_args.args[2])

    def test_ai_still_rejects_non_object_context(self) -> None:
        status, payload = self.service.ai_chat({
            "provider": "codex",
            "message": "summarize",
            "context": [],
        })

        self.assertEqual(400, status)
        self.assertEqual("provider, message and object context are required", payload["error"])

    def test_ai_uses_service_workspace_unless_provider_sets_its_own(self) -> None:
        defaults = ServiceConfig.defaults()
        providers = dict(defaults.providers)
        providers["dsh"] = replace(providers["dsh"], cwd="provider-workspace")

        service = ResultService(replace(defaults, workspace="service-workspace", providers=providers))

        self.assertEqual("service-workspace", service.providers["codex"].config.cwd)
        self.assertEqual("provider-workspace", service.providers["dsh"].config.cwd)


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
    def test_codex_default_invocation_keeps_local_permission_policy(self) -> None:
        provider = CodexProvider(ServiceConfig.defaults().providers["codex"])
        with patch("result_service.app.providers.resolve_provider_command", return_value="codex"), \
                patch("result_service.app.providers.subprocess.run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = '\n'.join([
                '{"type":"thread.started","thread_id":"123e4567-e89b-12d3-a456-426614174000"}',
                '{"type":"item.completed","item":{"type":"agent_message","text":"ready"}}',
            ])
            result = provider.complete("help me inspect this project", {})

        self.assertEqual("ready", result.text)
        self.assertEqual("123e4567-e89b-12d3-a456-426614174000", result.conversation_id)
        argv = run.call_args.args[0]
        self.assertNotIn("--sandbox", argv)
        self.assertNotIn("--dangerously-bypass-approvals-and-sandbox", argv)
        self.assertNotIn("--ephemeral", argv)
        self.assertIn("--json", argv)
        self.assertEqual("-", argv[-1])

    def test_codex_resumes_an_existing_conversation(self) -> None:
        conversation_id = "123e4567-e89b-12d3-a456-426614174000"
        provider = CodexProvider(ServiceConfig.defaults().providers["codex"])
        with patch("result_service.app.providers.resolve_provider_command", return_value="codex"), \
                patch("result_service.app.providers.subprocess.run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = (
                '{"type":"item.completed","item":{"type":"agent_message","text":"continued"}}'
            )
            result = provider.complete("continue", {}, conversation_id)

        self.assertEqual("continued", result.text)
        self.assertEqual(conversation_id, result.conversation_id)
        self.assertEqual([
            "codex", "exec", "resume", "--json", "--skip-git-repo-check",
            conversation_id, "-",
        ], run.call_args.args[0])

    def test_codex_rejects_non_uuid_conversation_before_execution(self) -> None:
        provider = CodexProvider(ServiceConfig.defaults().providers["codex"])
        with patch("result_service.app.providers.subprocess.run") as run:
            with self.assertRaisesRegex(ProviderError, "invalid Codex conversation id"):
                provider.complete("continue", {}, "--last")

        run.assert_not_called()

    def test_codex_serializes_requests_for_the_same_conversation(self) -> None:
        conversation_id = "123e4567-e89b-12d3-a456-426614174000"
        provider = CodexProvider(ServiceConfig.defaults().providers["codex"])
        active = 0
        max_active = 0
        state_lock = threading.Lock()

        def run_prompt(prompt, invocation_config):
            nonlocal active, max_active
            with state_lock:
                active += 1
                max_active = max(max_active, active)
            time.sleep(0.03)
            with state_lock:
                active -= 1
            return "continued", ""

        with patch.object(provider, "_run_prompt", side_effect=run_prompt):
            with ThreadPoolExecutor(max_workers=2) as executor:
                futures = [
                    executor.submit(provider.complete, "continue", {}, conversation_id)
                    for _ in range(2)
                ]
                results = [future.result() for future in futures]

        self.assertEqual(1, max_active)
        self.assertEqual([conversation_id, conversation_id], [
            result.conversation_id for result in results
        ])

    def test_codex_preserves_explicit_permission_settings(self) -> None:
        provider = CodexProvider(ProviderConfig(name="codex", command="codex",
                                               args=("exec", "--sandbox", "workspace-write")))

        self.assertEqual(("exec", "--sandbox", "workspace-write", "--json", "-"), provider.config.args)

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

    def test_plain_multiline_provider_output_is_preserved(self) -> None:
        output = "Summary:\n- first\n- second\n"

        self.assertEqual("Summary:\n- first\n- second", parse_provider_output("dsh", output))

    def test_jsonl_provider_output_uses_final_message(self) -> None:
        output = '\n'.join([
            '{"type":"thread.started","thread_id":"one"}',
            '{"type":"item.completed","item":{"type":"agent_message","text":"done"}}',
        ])

        self.assertEqual("done", parse_provider_output("codex", output))


class DshWebManagerTests(unittest.TestCase):
    def test_starts_once_and_reuses_the_ready_web_process(self) -> None:
        config = ServiceConfig.defaults().providers["dsh"]
        manager = DshWebManager(config)
        process = MagicMock()
        process.poll.return_value = None
        response = MagicMock()
        response.__enter__.return_value.status = 200

        with patch("result_service.app.providers.resolve_provider_command", return_value="dsh"), \
                patch.object(manager, "_free_loopback_port", return_value=56335), \
                patch("result_service.app.providers.subprocess.Popen", return_value=process) as popen, \
                patch("result_service.app.providers.urlopen", return_value=response):
            first_url = manager.start()
            second_url = manager.start()

        self.assertEqual("http://127.0.0.1:56335/", first_url)
        self.assertEqual(first_url, second_url)
        popen.assert_called_once()
        argv = popen.call_args.args[0]
        self.assertEqual(["--profile", "web", "--host", "127.0.0.1",
                          "--port", "56335", "--no-open"], argv[-7:])

        with patch.object(manager, "_stop_process") as stop_process:
            manager.close()
        stop_process.assert_called_once_with(process)

    def test_service_reports_dsh_web_availability_and_url(self) -> None:
        service = ResultService(ServiceConfig.defaults())
        with patch.object(service.providers["dsh"], "available", return_value=True), \
                patch.object(service.dsh_web, "start", return_value="http://127.0.0.1:56335/"):
            status, payload = service.start_dsh_web()

        self.assertEqual(200, status)
        self.assertEqual({"url": "http://127.0.0.1:56335/"}, payload)

    def test_service_does_not_start_dsh_web_when_command_is_unavailable(self) -> None:
        service = ResultService(ServiceConfig.defaults())
        with patch.object(service.providers["dsh"], "available", return_value=False), \
                patch.object(service.dsh_web, "start") as start:
            status, payload = service.start_dsh_web()

        self.assertEqual(503, status)
        self.assertEqual("dsh executable is not available", payload["error"])
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

        result = CoverageResultPlugin().summary(connection, "run-1")

        self.assertEqual({}, result)
        self.assertFalse(connection.summary_queried)

    def test_existing_summary_table_returns_run_result(self) -> None:
        summary = {"run_id": "run-1", "percent": 80}
        connection = _CoverageConnection("coverage_run_summary", summary)

        result = CoverageResultPlugin().summary(connection, "run-1")

        self.assertEqual(summary, result)
        self.assertTrue(connection.summary_queried)


@unittest.skipUnless(os.name == "nt", "Windows provider discovery")
class WindowsProviderDiscoveryTests(unittest.TestCase):
    def test_codex_desktop_install_is_found_without_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            executable = Path(temp_dir) / "OpenAI" / "Codex" / "bin" / "version" / "codex.exe"
            executable.parent.mkdir(parents=True)
            executable.touch()
            config = ProviderConfig(name="codex", command="codex")
            environment = {"LOCALAPPDATA": temp_dir, "APPDATA": "", "USERPROFILE": "", "PNPM_HOME": ""}
            with patch.dict(os.environ, environment, clear=True), \
                    patch("result_service.app.providers.shutil.which", return_value=None):
                resolved = resolve_provider_command(config)

            self.assertEqual(executable.resolve(), Path(resolved))

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

    def test_directory_is_not_an_available_command(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config = ProviderConfig(name="custom", command=temp_dir)
            with patch("result_service.app.providers.shutil.which", return_value=None):
                self.assertIsNone(resolve_provider_command(config))

    def test_bare_command_does_not_use_same_named_working_directory_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            command_file = Path(temp_dir) / "codex"
            command_file.write_text("not an executable", encoding="utf-8")
            config = ProviderConfig(name="codex", command="codex")
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
