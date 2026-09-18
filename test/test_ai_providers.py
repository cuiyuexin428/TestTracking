"""Regression tests for local CLI transport and durable DSH chat continuity."""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from result_service.app.config import ProviderConfig
from result_service.app.providers import (
    CommandProvider, DshProvider, ProviderError, _build_prompt, _command_argv, _run_command,
    _stop_process_tree,
)


def _windows_process_exited(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    kernel.WaitForSingleObject.restype = ctypes.c_ulong
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    ctypes.set_last_error(0)
    handle = kernel.OpenProcess(0x00100000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:  # ERROR_INVALID_PARAMETER: this PID no longer exists.
            return True
        raise ctypes.WinError(error)  # Access denied is unknown, not exited.
    try:
        result = kernel.WaitForSingleObject(handle, 0)
        if result == 0:
            return True
        if result == 258:  # WAIT_TIMEOUT: the process is still running.
            return False
        raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)


@unittest.skipUnless(os.name == "nt", "Windows process cleanup")
class WindowsProcessCleanupTests(unittest.TestCase):
    def process(self, returncode=None):
        process = Mock(pid=12345)
        process.poll.return_value = returncode
        return process

    def test_successful_taskkill_records_return_code_and_skips_fallback(self):
        process = self.process(returncode=0)
        with patch("result_service.app.providers.subprocess.run", return_value=
                   subprocess.CompletedProcess([], 0, stderr="")), self.assertLogs(
                       "result_service.providers", level="INFO") as logs:
            _stop_process_tree(process)
        process.kill.assert_not_called()
        self.assertIn("pid=12345 returncode=0", logs.output[0])

    def test_failed_taskkill_records_stderr_and_uses_fallback(self):
        process = self.process()
        with patch("result_service.app.providers.subprocess.run", return_value=
                   subprocess.CompletedProcess([], 1, stderr="ERROR: Access denied\n")), self.assertLogs(
                       "result_service.providers", level="WARNING") as logs:
            _stop_process_tree(process)
        process.kill.assert_called_once_with()
        self.assertIn("pid=12345 returncode=1 stderr=ERROR: Access denied", logs.output[0])

    def test_taskkill_launch_or_timeout_failure_still_uses_fallback(self):
        for error in (FileNotFoundError("taskkill unavailable"),
                      subprocess.TimeoutExpired("taskkill", 10)):
            with self.subTest(error=type(error).__name__):
                process = self.process()
                with patch("result_service.app.providers.subprocess.run", side_effect=error), \
                        self.assertLogs("result_service.providers", level="WARNING") as logs:
                    _stop_process_tree(process)
                process.kill.assert_called_once_with()
                self.assertIn("taskkill failed for provider pid=12345", logs.output[0])
                self.assertIn(type(error).__name__, logs.output[0])

    def test_fallback_failure_is_logged_and_not_reported_as_success(self):
        process = self.process()
        process.kill.side_effect = PermissionError("fallback access denied")
        with patch("result_service.app.providers.subprocess.run", return_value=
                   subprocess.CompletedProcess([], 1, stderr="ERROR: Access denied")), self.assertLogs(
                       "result_service.providers", level="WARNING") as logs:
            with self.assertRaisesRegex(PermissionError, "fallback access denied"):
                _stop_process_tree(process)
        self.assertTrue(any("Direct process cleanup failed for provider pid=12345" in line
                            and "fallback access denied" in line for line in logs.output))

    def test_process_probe_distinguishes_missing_pid_from_access_denied(self):
        for error in (87, 5):
            with self.subTest(winerror=error):
                kernel = Mock()

                def failed_open(*args):
                    ctypes.set_last_error(error)
                    return None

                kernel.OpenProcess.side_effect = failed_open
                with patch("ctypes.WinDLL", return_value=kernel):
                    if error == 87:
                        self.assertTrue(_windows_process_exited(12345))
                    else:
                        with self.assertRaises(OSError) as failure:
                            _windows_process_exited(12345)
                        self.assertEqual(failure.exception.winerror, 5)
                kernel.CloseHandle.assert_not_called()

    def test_process_probe_checks_exit_state_and_closes_handle(self):
        for wait_result, exited in ((0, True), (258, False)):
            with self.subTest(wait_result=wait_result):
                kernel = Mock()
                kernel.OpenProcess.return_value = 123
                kernel.WaitForSingleObject.return_value = wait_result
                with patch("ctypes.WinDLL", return_value=kernel):
                    self.assertEqual(_windows_process_exited(12345), exited)
                kernel.CloseHandle.assert_called_once_with(123)


class DshConversationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dsh conversation ")
        self.addCleanup(self.temp.cleanup)
        self.config = ProviderConfig(name="dsh", command="dsh", cwd=self.temp.name)
        self.prompts = []

    def run_prompt(self, prompt, invocation):
        self.assertEqual(prompt, "")
        overlay = Path(invocation.args[invocation.args.index("--patch") + 1])
        self.prompts.append(json.loads(overlay.read_text(encoding="utf-8"))[0]["config"]["task"])
        self.assertNotIn(self.prompts[-1], invocation.args)
        return "The remembered answer is ALPHA.", "The remembered answer is ALPHA."

    def test_report_source_is_transferred_exactly_without_command_line_limits(self):
        provider = DshProvider(self.config)
        source = (ROOT / "plugins/coverage/result_report.py").read_text(encoding="utf-8")
        context = {"current_script": source, "schema": "x" * 40000}
        with patch.object(provider, "_run_prompt", side_effect=self.run_prompt):
            result = provider.complete('生成报告 "<html>" & 100%\n保留换行', context)
        self.assertEqual(self.prompts[0], _build_prompt('生成报告 "<html>" & 100%\n保留换行', context))
        self.assertIsNotNone(result.conversation_id)

    def test_followup_survives_provider_restart_and_new_chats_are_independent(self):
        provider = DshProvider(self.config)
        with patch.object(provider, "_run_prompt", side_effect=self.run_prompt):
            first = provider.complete("Remember ALPHA", {})
        reopened = DshProvider(self.config)
        with patch.object(reopened, "_run_prompt", side_effect=self.run_prompt):
            second = reopened.complete("Repeat my label", {}, first.conversation_id)
            fresh = reopened.complete("Another question", {})
        self.assertEqual(first.conversation_id, second.conversation_id)
        self.assertIn("Remember ALPHA", self.prompts[1])
        self.assertIn("The remembered answer is ALPHA.", self.prompts[1])
        self.assertNotIn("Remember ALPHA", self.prompts[2])
        self.assertNotEqual(first.conversation_id, fresh.conversation_id)

    def test_failed_followup_does_not_enter_history(self):
        provider = DshProvider(self.config)
        with patch.object(provider, "_run_prompt", side_effect=self.run_prompt):
            first = provider.complete("Remember ALPHA", {})
        with patch.object(provider, "_run_prompt", side_effect=ProviderError("timed out")):
            with self.assertRaises(ProviderError):
                provider.complete("Failed question", {}, first.conversation_id)
        with patch.object(provider, "_run_prompt", side_effect=self.run_prompt):
            provider.complete("Continue", {}, first.conversation_id)
        self.assertNotIn("Failed question", self.prompts[-1])

    def test_unknown_conversation_is_not_silently_restarted(self):
        provider = DshProvider(self.config)
        with self.assertRaisesRegex(ProviderError, "start a new conversation"):
            provider.complete("continue", {}, "123e4567-e89b-12d3-a456-426614174000")

    def test_concurrent_followups_save_both_turns_in_order(self):
        provider = DshProvider(self.config)
        with patch.object(provider, "_run_prompt", side_effect=self.run_prompt):
            first = provider.complete("Remember ALPHA", {})
            with ThreadPoolExecutor(max_workers=2) as executor:
                results = list(executor.map(
                    lambda message: provider.complete(message, {}, first.conversation_id),
                    ["Followup one", "Followup two"],
                ))
        self.assertTrue(all(result.conversation_id == first.conversation_id for result in results))
        history = json.loads((provider.conversations_dir / f"{first.conversation_id}.json").read_text(encoding="utf-8"))
        self.assertEqual(len(history), 6)
        self.assertEqual({turn["content"] for turn in history if turn["role"] == "user"},
                         {"Remember ALPHA", "Followup one", "Followup two"})
        self.assertIn("Previous conversation", self.prompts[-1])
        self.assertTrue("Followup one" in self.prompts[-1] and "Followup two" in self.prompts[-1])


@unittest.skipUnless(os.name == "nt", "Windows command wrappers")
class WindowsProviderTransportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="provider with spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def wrapper(self, source):
        program = self.root / "provider.py"
        program.write_text(source, encoding="utf-8")
        wrapper = self.root / "provider.cmd"
        wrapper.write_text('@echo off\n"' + sys.executable + '" "' + str(program) + '" %*\n', encoding="utf-8")
        return str(wrapper)

    def test_codex_style_stdin_and_web_arguments_work_in_paths_with_spaces(self):
        command = self.wrapper('import json,sys\nsys.stdin.reconfigure(encoding="utf-8")\nprint(json.dumps({"text":sys.stdin.read(),"args":sys.argv[1:]}))')
        provider = CommandProvider(ProviderConfig(name="custom", command=command, args=("exec", "-")))
        message = 'Unicode 中文 & <html> "quoted"\n' * 800
        self.assertEqual(provider.complete(message, {}).text, _build_prompt(message, {}).strip())
        config = ProviderConfig(name="dsh", command=command, args=("--profile", "web", "--patch", str(self.root / "web patch.json")))
        result = _run_command(_command_argv(config), input="", timeout=5, cwd=self.temp.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["args"], list(config.args))

    def test_real_cmd_wrapper_receives_large_dsh_report_patch(self):
        command = self.wrapper('import json,sys,pathlib\na=sys.argv[1:]\np=pathlib.Path(a[a.index("--patch")+1])\nprint(json.dumps({"text":json.loads(p.read_text(encoding="utf-8"))[0]["config"]["task"]}))')
        provider = DshProvider(ProviderConfig(name="dsh", command=command, cwd=self.temp.name))
        context = {"current_script": (ROOT / "plugins/coverage/result_report.py").read_text(encoding="utf-8"),
                   "schema": "schema中文" * 6000}
        result = provider.complete('Generate "HTML" & plots', context)
        self.assertEqual(result.text, _build_prompt('Generate "HTML" & plots', context))

    def test_timeout_stops_the_wrapper_and_its_child_processes(self):
        pid_file = self.root / "child.pid"
        source = ('import subprocess,sys,pathlib,time\n'
                  'child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(30)"])\n'
                  f'pathlib.Path({str(pid_file)!r}).write_text(str(child.pid))\n'
                  'child.wait()\n')
        command = self.wrapper(source)
        provider = CommandProvider(ProviderConfig(name="custom", command=command, timeout_seconds=0.5))
        started = time.monotonic()
        with self.assertRaisesRegex(ProviderError, "timed out"):
            provider.complete("hello", {})
        elapsed = time.monotonic() - started
        self.assertTrue(pid_file.is_file(), "child must have started before timeout")
        self.assertTrue(_windows_process_exited(int(pid_file.read_text())),
                        f"child is still running after provider returned in {elapsed:.3f}s")
        self.assertLess(elapsed, 5)


@unittest.skipUnless(os.name == "nt" and os.environ.get("TESTTRACKING_DSH_BUNDLE"),
                     "set TESTTRACKING_DSH_BUNDLE to run the actual offline DSH against a local model fixture")
class BundledDshTests(unittest.TestCase):
    def test_actual_headless_uses_patch_and_continues_after_provider_restart(self):
        requests = []

        class ModelHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.append(request)
                text = "ALPHA is the previous label." if len(requests) > 1 else "Remembered ALPHA."
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                for delta, finish in [({"role": "assistant", "content": text}, None), ({}, "stop")]:
                    self.wfile.write(("data: " + json.dumps({
                        "id": "fixture-response", "object": "chat.completion.chunk", "created": 1,
                        "model": "fixture", "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
                    }) + "\n\n").encode())
                self.wfile.write(b"data: [DONE]\n\n")

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="actual dsh with spaces ") as directory:
                root = Path(directory)
                home = root / "home"
                home.mkdir()
                bundle = Path(os.environ["TESTTRACKING_DSH_BUNDLE"]).resolve()
                node = next((bundle / "node").rglob("node.exe"))
                (home / "settings.yaml").write_text(
                    "llm-pi-ai:\n  providers:\n    fixture:\n      api: openai-completions\n"
                    f"      baseURL: http://127.0.0.1:{server.server_port}/v1\n"
                    "      apiKeyEnv: TESTTRACKING_FIXTURE_KEY\n      models:\n        - id: fixture\n"
                    "          contextWindow: 131072\n          maxTokens: 8192\n"
                    "agent-default-model:\n  provider: fixture\n  model: fixture\n",
                    encoding="utf-8",
                )
                (home / ".credentials.yaml").write_text("TESTTRACKING_FIXTURE_KEY: fixture-only\n", encoding="utf-8")
                wrapper = root / "dsh.cmd"
                wrapper.write_text('@echo off\nsetlocal\nset "DSH_HOME=' + str(home) + '"\n"' +
                                   str(node) + '" "' +
                                   str(bundle / "runtime/node_modules/@deepseek-ai/dsh/lib/bin.js") + '" %*\n',
                                   encoding="utf-8")
                config = ProviderConfig(name="dsh", command=str(wrapper), cwd=directory, timeout_seconds=30)
                context = {"current_script": (ROOT / "plugins/coverage/result_report.py").read_text(encoding="utf-8"),
                           "schema": "x" * 40000}
                first = DshProvider(config).complete("Remember ALPHA.", context)
                second = DshProvider(config).complete("What label did I give you?", {}, first.conversation_id)
                self.assertEqual(first.text, "Remembered ALPHA.")
                self.assertEqual(second.text, "ALPHA is the previous label.")
                self.assertEqual(first.conversation_id, second.conversation_id)
                user1 = next(message for message in requests[0]["messages"]
                             if message["role"] == "user" and "User question:" in str(message["content"]))
                self.assertEqual(user1["content"], _build_prompt("Remember ALPHA.", context))
                user2 = next(message for message in requests[1]["messages"]
                             if message["role"] == "user" and "Previous conversation" in str(message["content"]))
                self.assertIn("Remembered ALPHA.", user2["content"])
                self.assertIn("What label did I give you?", user2["content"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
