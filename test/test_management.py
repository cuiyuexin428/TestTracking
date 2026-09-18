from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
from dataclasses import replace
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.config import ServiceConfig
from result_service.app.management import ManagementError, ManagementService


class _SchedulerHandler(BaseHTTPRequestHandler):
    commands = []

    def log_message(self, *args):
        pass

    def reply(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/status":
            self.reply(200, {"online": True, "clients": [{"worker_id": "worker-1"}],
                             "plugins": ["coverage"], "active_runs": [], "in_flight": 2})
        else:
            self.reply(404, {"error": "unknown route"})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.commands.append(body)
        if body.get("dataset_version") == "bad":
            self.reply(409, {"error": "dataset version mismatch"})
        else:
            self.reply(200, {"ok": True, "action": body["action"], "run_id": "run-1"})


class SchedulerManagementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), _SchedulerHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        self.service = ManagementService(replace(ServiceConfig.defaults(), scheduler_management_url=
                                                f"http://127.0.0.1:{self.server.server_port}"))

    def test_status_and_commands_reach_the_scheduler(self):
        status = self.service.get_status()
        self.assertTrue(status["online"])
        self.assertEqual("worker-1", status["clients"][0]["worker_id"])
        result = self.service.command({"action": "run", "plugin_type": "coverage", "dataset_version": "d1",
                                       "params": {"tag": "试用"}})
        self.assertEqual("run-1", result["run_id"])
        self.assertEqual({"tag": "试用"}, _SchedulerHandler.commands[-1]["params"])
        self.service.command({"action": "stop", "plugin_type": "coverage", "run_id": "run-1"})
        self.assertEqual("run-1", _SchedulerHandler.commands[-1]["run_id"])

    def test_upstream_rejection_preserves_status_and_reason(self):
        with self.assertRaises(ManagementError) as failure:
            self.service.command({"action": "run", "plugin_type": "coverage", "dataset_version": "bad"})
        self.assertEqual(409, failure.exception.status)
        self.assertEqual("dataset version mismatch", str(failure.exception))

    def test_offline_scheduler_does_not_raise_from_status(self):
        with patch.object(self.service, "_request", side_effect=ManagementError("offline", 503)):
            status = self.service.get_status()
        self.assertFalse(status["online"])
        self.assertEqual([], status["clients"])
        self.assertEqual("offline", status["reason"])

    def test_remote_upstream_and_invalid_commands_are_rejected(self):
        with self.assertRaises(ManagementError):
            ManagementService(replace(ServiceConfig.defaults(), scheduler_management_url="http://192.168.1.10:5556"))
        for body in ({"action": "quit"}, {"action": []}, {"action": {}},
                     {"action": "run", "plugin_type": "coverage"},
                     {"action": "stop", "plugin_type": []}):
            with self.subTest(body=body), self.assertRaises(ManagementError):
                self.service.command(body)


class ManagementConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scheduler = self.root / "scheduler.json"
        self.client = self.root / "client.json"
        self.result = self.root / "result_service.json"
        self.scheduler.write_text(json.dumps({
            "server": {"router_endpoint": "tcp://*:5555", "db_conn": "host=localhost password='secret phrase' dbname=test"},
            "plugins": {"coverage": {"enabled": True, "dll": "coverage.dll", "task_size": 25,
                                        "custom_option": {"flags": ["a", "b"]}}}}), encoding="utf-8")
        self.client.write_text(json.dumps({"scheduler": {"endpoint": "tcp://127.0.0.1:5555"},
                                          "client": {"worker_id": "worker-1", "task_threads": 1},
                                          "plugins": {"coverage": {"enabled": True, "dll": "coverage.dll"},
                                                      "stpcompare": {"enabled": False, "dll": "stpcompare.dll"}}}), encoding="utf-8")
        self.result.write_text(json.dumps({"port": 56330, "scheduler_config_path": "scheduler.json",
                                          "client_config_path": "client.json", "logs_dir": "logs",
                                          "db_conn": "postgresql://postgres:p%40ss@localhost/db"}), encoding="utf-8")
        self.service = ManagementService(ServiceConfig.from_file(self.result))
        offline = patch.object(self.service, "get_status", return_value={"online": False, "reason": "test offline"})
        offline.start()
        self.addCleanup(offline.stop)

    def test_relative_configuration_paths_are_resolved_and_effective_values_exposed(self):
        loaded = self.service.get_config("result_service")
        self.assertEqual(str(self.scheduler), loaded["effective_values"]["scheduler_config_path"])
        self.assertEqual(str(self.result), loaded["effective_values"]["config_path"])
        self.assertFalse(loaded["restart_required"])
        self.assertEqual(56330, loaded["effective_values"]["port"])

    def test_masked_password_roundtrip_preserves_secret_extensions_and_backup(self):
        before = self.scheduler.read_bytes()
        loaded = self.service.get_config("scheduler")
        self.assertNotIn("secret phrase", json.dumps(loaded))
        loaded["config"]["server"]["db_conn"] = loaded["config"]["server"]["db_conn"].replace("localhost", "dbhost")
        loaded["config"]["plugins"]["coverage"]["task_size"] = 10
        saved = self.service.save_config("scheduler", loaded)
        written = json.loads(self.scheduler.read_text(encoding="utf-8"))
        self.assertIn("password='secret phrase'", written["server"]["db_conn"])
        self.assertIn("host=dbhost", written["server"]["db_conn"])
        self.assertEqual({"flags": ["a", "b"]}, written["plugins"]["coverage"]["custom_option"])
        self.assertEqual(before, self.scheduler.with_suffix(".json.bak").read_bytes())
        self.assertTrue(saved["restart_required"])
        self.assertTrue(saved["saved"])

    def test_uri_password_and_effective_values_stay_masked_after_save(self):
        loaded = self.service.get_config("result_service")
        self.assertNotIn("p%40ss", json.dumps(loaded))
        loaded["config"]["db_conn"] = loaded["config"]["db_conn"].replace("localhost", "dbhost")
        loaded["config"]["port"] = 56331
        saved = self.service.save_config("result_service", loaded)
        written = json.loads(self.result.read_text(encoding="utf-8"))
        self.assertEqual("postgresql://postgres:p%40ss@dbhost/db", written["db_conn"])
        self.assertEqual(56330, saved["effective_values"]["port"])
        self.assertEqual(56331, saved["config"]["port"])
        self.assertTrue(saved["restart_required"])

    def test_stale_revision_cannot_overwrite_new_configuration(self):
        loaded = self.service.get_config("scheduler")
        self.scheduler.write_text(self.scheduler.read_text() + "\n", encoding="utf-8")
        with self.assertRaises(ManagementError) as failure:
            self.service.save_config("scheduler", loaded)
        self.assertEqual(409, failure.exception.status)

    def test_invalid_task_size_and_multiple_client_plugins_do_not_write(self):
        original = self.scheduler.read_bytes()
        loaded = self.service.get_config("scheduler")
        loaded["config"]["plugins"]["coverage"]["task_size"] = 0
        with self.assertRaises(ManagementError):
            self.service.save_config("scheduler", loaded)
        self.assertEqual(original, self.scheduler.read_bytes())
        loaded = self.service.get_config("client")
        loaded["config"]["plugins"]["stpcompare"]["enabled"] = True
        with self.assertRaises(ManagementError):
            self.service.save_config("client", loaded)

    def test_atomic_write_failure_keeps_original_configuration(self):
        loaded = self.service.get_config("scheduler")
        original = self.scheduler.read_bytes()
        loaded["config"]["server"]["client_timeout_ms"] = 10000
        real_write = self.service._atomic_write
        def write(path, data):
            if path == self.scheduler:
                raise OSError("read only")
            real_write(path, data)
        with patch.object(self.service, "_atomic_write", side_effect=write), self.assertRaises(ManagementError):
            self.service.save_config("scheduler", loaded)
        self.assertEqual(original, self.scheduler.read_bytes())

    def test_unknown_kind_and_missing_config_are_not_arbitrary_file_access(self):
        with self.assertRaises(ManagementError):
            self.service.get_config(str(self.scheduler))
        result = ManagementService(ServiceConfig.defaults()).get_config("client")
        self.assertFalse(result["available"])
        self.assertIsNone(result["config"])

    def test_management_endpoint_mismatch_warns_and_can_be_resolved_in_two_saves(self):
        loaded = self.service.get_config("scheduler")
        loaded["config"]["server"]["management_endpoint"] = "http://127.0.0.1:5566"
        first = self.service.save_config("scheduler", loaded)
        self.assertTrue(first["warnings"])
        loaded = self.service.get_config("result_service")
        loaded["config"]["scheduler_management_url"] = "http://127.0.0.1:5566"
        second = self.service.save_config("result_service", loaded)
        self.assertEqual([], second["warnings"])
        self.assertEqual([], self.service.get_config("scheduler")["warnings"])

    def test_invalid_connections_and_management_endpoints_do_not_write(self):
        for value in ("not a connection", "host=localhost password='unfinished", "postgresql://db:99999/test"):
            loaded = self.service.get_config("scheduler")
            loaded["config"]["server"]["db_conn"] = value
            with self.subTest(value=value), self.assertRaises(ManagementError):
                self.service.save_config("scheduler", loaded)
        for value in ("http://0.0.0.0:5556", "http://localhost:5556"):
            loaded = self.service.get_config("scheduler")
            loaded["config"]["server"]["management_endpoint"] = value
            with self.subTest(value=value), self.assertRaises(ManagementError):
                self.service.save_config("scheduler", loaded)

    def test_scheduler_restart_clears_pending_flag_from_live_configuration(self):
        original = json.loads(self.scheduler.read_text())
        with patch.object(self.service, "get_status", return_value={"online": True, "instance_id": "before",
                                                                   "configuration": original}):
            loaded = self.service.get_config("scheduler")
            self.assertFalse(loaded["restart_required"])
            loaded["config"]["server"]["db_conn"] = "host=localhost password='new password' dbname=test"
            saved = self.service.save_config("scheduler", loaded)
            self.assertTrue(saved["restart_required"])
        new_config = json.loads(self.scheduler.read_text())
        with patch.object(self.service, "get_status", return_value={"online": True, "instance_id": "after",
                                                                   "configuration": new_config}):
            loaded = self.service.get_config("scheduler")
            self.assertFalse(loaded["restart_required"])
            self.assertNotIn("new password", json.dumps(loaded))

    def test_different_running_config_file_is_explicitly_reported(self):
        configuration = json.loads(self.scheduler.read_text())
        with patch.object(self.service, "get_status", return_value={"online": True,
                "configuration": configuration, "config_path": str(self.root / "other.json")}):
            result = self.service.get_config("scheduler")
        self.assertTrue(any("另一份配置文件" in warning for warning in result["warnings"]))

    def test_hidden_scheduler_change_survives_result_service_restart_until_scheduler_restarts(self):
        runtime = json.loads(self.scheduler.read_text())
        runtime["server"]["db_conn"] = "[redacted]"
        live = {"online": True, "instance_id": "original-instance", "config_path": str(self.scheduler),
                "configuration": runtime}
        with patch.object(self.service, "get_status", return_value=live):
            loaded = self.service.get_config("scheduler")
            loaded["config"]["server"]["db_conn"] = "host=new-server password='new secret' dbname=new-db"
            self.assertTrue(self.service.save_config("scheduler", loaded)["restart_required"])
        marker = self.scheduler.with_name(self.scheduler.name + ".management-state.json")
        self.assertNotIn("new secret", marker.read_text())
        restarted_ui = ManagementService(ServiceConfig.from_file(self.result))
        with patch.object(restarted_ui, "get_status", return_value=live):
            self.assertTrue(restarted_ui.get_config("scheduler")["restart_required"])
        with patch.object(restarted_ui, "get_status", return_value={"online": False}):
            self.assertTrue(restarted_ui.get_config("scheduler")["restart_required"])
        live["instance_id"] = "restarted-instance"
        with patch.object(restarted_ui, "get_status", return_value=live):
            self.assertFalse(restarted_ui.get_config("scheduler")["restart_required"])
        self.assertFalse(marker.exists())
        another_ui = ManagementService(ServiceConfig.from_file(self.result))
        with patch.object(another_ui, "get_status", return_value=live):
            self.assertFalse(another_ui.get_config("scheduler")["restart_required"])

    def test_invalid_provider_prompt_mode_reports_validation_error(self):
        loaded = self.service.get_config("result_service")
        loaded["config"]["providers"] = {"codex": {"prompt_mode": []}}
        with self.assertRaises(ManagementError):
            self.service.save_config("result_service", loaded)

    def test_scheduler_zero_timeouts_and_disabled_management_match_runtime(self):
        loaded = self.service.get_config("scheduler")
        loaded["config"]["server"].update(client_timeout_ms=0, task_lease_timeout_ms=0,
                                           management_endpoint="http://127.0.0.1:0")
        result = self.service.save_config("scheduler", loaded)
        self.assertTrue(result["saved"])


class ManagementLogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.logs = self.root / "logs"
        self.logs.mkdir()
        self.service = ManagementService(replace(ServiceConfig.defaults(), logs_dir=str(self.logs)))

    def test_filtered_tail_lists_files_and_bounds_results(self):
        (self.logs / "scheduler.log").write_text(
            "[INFO] start\n[ERROR] task 1\n[WARN] other\n[ERROR] task 2\n[ERROR] task 3\n", encoding="utf-8")
        (self.logs / "ignored.json").write_text("secret")
        result = self.service.logs({"level": ["ERROR"], "search": ["task"], "limit": ["2"]})
        self.assertEqual(["[ERROR] task 2", "[ERROR] task 3"], result["lines"])
        self.assertEqual("scheduler.log", result["selected"])
        self.assertEqual(1, len(result["files"]))
        self.assertTrue(result["truncated"])

    def test_empty_logs_and_arbitrary_paths(self):
        self.assertEqual([], self.service.logs({})["lines"])
        (self.root / "private.log").write_text("private")
        for filename in ("../private.log", str(self.root / "private.log")):
            with self.subTest(filename=filename), self.assertRaises(ManagementError):
                self.service.logs({"file": [filename]})

    def test_large_logs_read_only_the_bounded_tail(self):
        with (self.logs / "scheduler.log").open("wb") as stream:
            stream.write(b"[INFO] old line\n" * 150000)
            stream.write("[ERROR] 最后一行\n".encode("utf-8"))
        result = self.service.logs({"limit": ["1"]})
        self.assertEqual(["[ERROR] 最后一行"], result["lines"])
        self.assertTrue(result["truncated"])


if __name__ == "__main__":
    unittest.main()
