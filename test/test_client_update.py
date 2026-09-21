"""Execute the Windows staging helper against the real deployment HTTP APIs."""
from __future__ import annotations

import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import zipfile
from datetime import datetime, timedelta
from pathlib import Path
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from result_service.app.config import ServiceConfig
from result_service.app.server import Handler, ResultHTTPServer, ResultService


@unittest.skipUnless(os.name == "nt" and shutil.which("powershell"), "Windows PowerShell required")
class ClientUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="client-update-")
        self.root = Path(self.temporary.name)
        self.client = self.root / "客户端 with spaces"
        self.client.mkdir()
        self.config = self.client / "config" / "client.json"
        self.config.parent.mkdir()
        self.result = self.client / "deployments" / "result.json"
        self.service = ResultService(ServiceConfig(
            workspace=str(self.root), artifact_store_dir=str(self.root / "repository"), providers={},
        ))
        self.requests = []
        requests = self.requests

        class CountingHandler(Handler):
            def do_GET(handler):
                requests.append(handler.path)
                super().do_GET()

            def do_PUT(handler):
                requests.append(handler.path)
                super().do_PUT()

            def log_message(*args):
                pass

        CountingHandler.service = self.service
        self.server = ResultHTTPServer(("127.0.0.1", 0), CountingHandler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.endpoint = f"http://127.0.0.1:{self.server.server_port}"
        self.base_config = {
            "scheduler": {"endpoint": "tcp://127.0.0.1:5555"},
            "client": {"worker_id": "test-worker", "deployment_endpoint": self.endpoint,
                       "update_windows": []},
            "plugins": {"custom": {"enabled": True, "dll": "plugins/custom.dll",
                                  "work_dir": "work", "arguments": ["--input", "{stp}"],
                                  "engine_dlls": ["program/engine.dll"],
                                  "nested": {"preserved": 1}}},
        }
        self.write_config()
        self.sentinel = self.client / "active-program.txt"
        self.sentinel.write_text("running version", encoding="utf-8")

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(5)
        self.server.server_close()
        self.service.close()
        self.temporary.cleanup()

    def write_config(self):
        self.config.write_text(json.dumps(self.base_config), encoding="utf-8")

    def artifact(self, entries=None, *, raw=None, filename="data.zip", version="1"):
        if raw is None:
            output = io.BytesIO()
            with zipfile.ZipFile(output, "w") as archive:
                for name, content in entries or [("nested/input.stp", b"STP data")]:
                    archive.writestr(name, content)
            raw = output.getvalue()
        artifact, _ = self.service.artifacts.upload(
            io.BytesIO(raw), len(raw), kind="dataset", name="data", version=version,
            filename=filename,
        )
        return artifact

    def plan(self, artifact, *, patch=None, revision=None):
        body = {"revision": revision,
                "components": [{"slot": "dataset", "artifact_id": artifact["artifact_id"]}],
                "config_patch": patch or {"plugins": {"custom": {"data_root": "{artifact:dataset}"}}}}
        request = Request(self.endpoint + "/api/deployments/test-worker/desired",
                          data=json.dumps(body).encode(), method="PUT",
                          headers={"Content-Type": "application/json"})
        with urlopen(request, timeout=5) as response:
            return json.load(response)["desired"]

    def run_update(self, *, expected=0, report=None, effective=None, current=None, prepared=None):
        command = [shutil.which("powershell"), "-NoLogo", "-NoProfile", "-NonInteractive",
                   "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "tools" / "client_update.ps1"),
                   "-ClientRoot", str(self.client), "-ConfigPath", str(self.config),
                   "-ResultPath", str(self.result)]
        if report:
            command += ["-ReportPath", str(report)]
        if effective:
            command += ["-EffectiveConfigPath", str(effective)]
        if current:
            command += ["-CurrentRevision", current]
        if prepared:
            command += ["-PreparedRevision", prepared]
        process = subprocess.run(command, cwd=self.root, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=45)
        self.assertEqual(expected, process.returncode, process.stdout + process.stderr)
        self.assertTrue(self.result.is_file(), process.stdout + process.stderr)
        result = json.loads(self.result.read_text(encoding="utf-8"))
        self.assertEqual("running version", self.sentinel.read_text(encoding="utf-8"))
        return result

    def downloads(self):
        return [path for path in self.requests if path.startswith("/api/artifacts/objects/")]

    def test_zip_stage_preserves_base_and_resolves_generic_patch_and_cached_release(self):
        artifact = self.artifact()
        desired = self.plan(artifact, patch={"plugins": {"custom": {
            "data_root": "{artifact:dataset}", "nested": {"added": 2},
            "parameters": [None, {"file_path": "{artifact:dataset}/nested/input.stp"}, []],
        }}})
        result = self.run_update()
        self.assertEqual("staged", result["status"])
        self.assertEqual(desired["revision"], result["revision"])
        self.assertFalse((self.client / "deployments" / "active.json").exists())
        config = json.loads(Path(result["config_path"]).read_text(encoding="utf-8"))
        content = self.client / "deployments" / "objects" / artifact["sha256"] / "content"
        plugin = config["plugins"]["custom"]
        self.assertEqual(str(content), plugin["data_root"])
        self.assertEqual(str(self.client / "plugins" / "custom.dll"), plugin["dll"])
        self.assertEqual(str(self.client / "work"), plugin["work_dir"])
        self.assertEqual([str(self.client / "program" / "engine.dll")], plugin["engine_dlls"])
        self.assertEqual({"preserved": 1, "added": 2}, plugin["nested"])
        self.assertEqual([None, {"file_path": str(content / "nested" / "input.stp")}, []], plugin["parameters"])
        self.assertEqual(["--input", "{stp}"], plugin["arguments"])
        self.assertEqual(self.base_config["client"], config["client"])
        self.assertEqual(b"STP data", (content / "nested" / "input.stp").read_bytes())
        self.assertEqual(1, len(self.downloads()))
        old_mtime = Path(result["config_path"]).stat().st_mtime_ns
        again = self.run_update()
        self.assertEqual("staged", again["status"])
        self.assertEqual(1, len(self.downloads()), "cached content must not be downloaded again")
        self.assertEqual(old_mtime, Path(result["config_path"]).stat().st_mtime_ns)
        current = self.run_update(current=result["revision"])
        self.assertEqual("current", current["status"])
        self.assertEqual("active", current["components"][0]["state"])
        self.assertEqual(1, len(self.downloads()))

    def test_plain_file_and_failed_next_download_retain_staged_release(self):
        first = self.artifact(raw=b"executable bytes", filename="tool.exe")
        original = self.plan(first)
        staged = self.run_update()
        old_config = Path(staged["config_path"])
        old_bytes = old_config.read_bytes()
        content = Path(json.loads(old_bytes)["plugins"]["custom"]["data_root"])
        self.assertEqual(b"executable bytes", (content / "tool.exe").read_bytes())
        second = self.artifact(raw=b"expected new content", filename="tool.exe", version="2")
        self.plan(second, revision=original["revision"])
        self.service.artifacts.object_path(second["sha256"]).write_bytes(b"corrupt transport")
        failed = self.run_update(expected=1)
        self.assertEqual("failed", failed["status"])
        self.assertIn("SHA-256 mismatch", failed["message"])
        self.assertEqual("failed", failed["components"][0]["state"])
        self.assertEqual(old_bytes, old_config.read_bytes())
        self.assertFalse((self.client / "deployments" / "objects" / second["sha256"] / "archive").exists())

    def test_merged_multi_plugin_deployment_stages_without_disabling_existing_plugin(self):
        first = self.plan(self.artifact())
        staged = self.run_update()
        old_config = Path(staged["config_path"])
        old_bytes = old_config.read_bytes()
        active = self.client / "deployments" / "active.json"
        active.write_text(json.dumps(staged), encoding="utf-8")
        active_bytes = active.read_bytes()
        base_bytes = self.config.read_bytes()
        artifact = self.artifact(raw=b"new program", filename="tool.exe", version="2")
        # A partial patch adds a second plugin while retaining the first.
        desired = self.plan(artifact, revision=first["revision"], patch={"plugins": {
            "replacement": {"enabled": True, "dll": "{artifact:dataset}/tool.exe"},
        }})
        self.requests.clear()
        next_stage = self.run_update(current=first["revision"])
        self.assertEqual("staged", next_stage["status"])
        prepared = json.loads(Path(next_stage["config_path"]).read_text(encoding="utf-8"))
        self.assertTrue(prepared["plugins"]["custom"]["enabled"])
        self.assertTrue(prepared["plugins"]["replacement"]["enabled"])
        self.assertEqual({"preserved": 1}, prepared["plugins"]["custom"]["nested"])
        self.assertEqual(b"new program", Path(prepared["plugins"]["replacement"]["dll"]).read_bytes())
        self.assertEqual(1, len(self.downloads()))
        self.assertEqual(old_bytes, old_config.read_bytes())
        self.assertEqual(active_bytes, active.read_bytes())
        self.assertEqual(base_bytes, self.config.read_bytes())
        self.assertTrue((self.client / "deployments" / "releases" / desired["revision"]).exists())
        self.requests.clear()
        cached = self.run_update(prepared=desired["revision"])
        self.assertEqual("staged", cached["status"])
        self.assertEqual(next_stage["config_path"], cached["config_path"])
        self.assertEqual([], self.downloads())

    def test_deployment_with_no_enabled_plugin_is_rejected_before_download(self):
        self.plan(self.artifact(), patch={"plugins": {"custom": {"enabled": False}}})
        failed = self.run_update(expected=1)
        self.assertEqual("failed", failed["status"])
        self.assertIn("at least one enabled plugin; found 0", failed["message"])
        self.assertEqual([], self.downloads())
        self.assertTrue(json.loads(self.config.read_text(encoding="utf-8"))["plugins"]["custom"]["enabled"])

    def test_switching_plugins_explicitly_disables_previous_plugin(self):
        artifact = self.artifact(raw=b"new plugin", filename="replacement.dll")
        self.plan(artifact, patch={"plugins": {
            "custom": {"enabled": False},
            "replacement": {"enabled": True, "dll": "{artifact:dataset}/replacement.dll"},
        }})
        result = self.run_update()
        self.assertEqual("staged", result["status"])
        config = json.loads(Path(result["config_path"]).read_text(encoding="utf-8"))
        self.assertFalse(config["plugins"]["custom"]["enabled"])
        self.assertTrue(config["plugins"]["replacement"]["enabled"])
        self.assertEqual(b"new plugin", Path(config["plugins"]["replacement"]["dll"]).read_bytes())
        self.assertEqual(self.base_config["client"], config["client"])
        self.assertEqual(1, len(self.downloads()))

    def test_prepared_release_cannot_bypass_plugin_enabled_boolean_validation(self):
        desired = self.plan(self.artifact())
        staged = self.run_update()
        prepared = Path(staged["config_path"])
        config = json.loads(prepared.read_text(encoding="utf-8"))
        config["plugins"]["legacy-second"] = {"enabled": "true", "dll": "other.dll"}
        prepared.write_text(json.dumps(config), encoding="utf-8")
        self.requests.clear()
        failed = self.run_update(expected=1, prepared=desired["revision"])
        self.assertEqual("failed", failed["status"])
        self.assertIn("Plugin enabled must be a boolean: legacy-second", failed["message"])
        self.assertEqual([], self.downloads())
        self.assertFalse((self.client / "deployments" / "active.json").exists())

    def test_merged_configuration_rejects_non_boolean_enabled_before_download(self):
        self.base_config["plugins"]["other"] = {"enabled": "false", "dll": "other.dll"}
        self.write_config()
        self.plan(self.artifact())
        failed = self.run_update(expected=1)
        self.assertIn("Plugin enabled must be a boolean: other", failed["message"])
        self.assertEqual([], self.downloads())

    def test_prepared_release_requires_at_least_one_enabled_plugin(self):
        desired = self.plan(self.artifact())
        staged = self.run_update()
        prepared = Path(staged["config_path"])
        config = json.loads(prepared.read_text(encoding="utf-8"))
        config["plugins"]["custom"]["enabled"] = False
        prepared.write_text(json.dumps(config), encoding="utf-8")
        self.requests.clear()
        failed = self.run_update(expected=1, prepared=desired["revision"])
        self.assertIn("at least one enabled plugin; found 0", failed["message"])
        self.assertEqual([], self.downloads())

    def test_outside_window_polls_and_reports_but_never_downloads(self):
        artifact = self.artifact()
        desired = self.plan(artifact)
        tomorrow = (datetime.now().isoweekday() % 7) + 1
        self.base_config["client"]["update_windows"] = [{"days": [tomorrow], "start": "00:00", "end": "00:00"}]
        self.write_config()
        report = self.client / "report.json"
        body = {"desired_revision": desired["revision"], "state": "waiting_window",
                "components": [{"slot": "dataset", "artifact_id": artifact["artifact_id"], "state": "pending"}]}
        report.write_text(json.dumps(body), encoding="utf-8")
        result = self.run_update(report=report)
        self.assertEqual("waiting_window", result["status"])
        self.assertEqual(desired["revision"], result["revision"])
        self.assertEqual([], self.downloads())
        self.assertTrue(Path(str(report) + ".sent").is_file())
        self.assertEqual("waiting_window", self.service.deployments.get("test-worker")["state"])
        count = self.requests.count("/api/deployments/test-worker/reported")
        self.run_update(report=report)
        self.assertEqual(count, self.requests.count("/api/deployments/test-worker/reported"))

    def test_effective_windows_and_overnight_start_day(self):
        artifact = self.artifact()
        self.plan(artifact)
        now = datetime.now()
        # A 23-hour overnight window containing now; its days name the start day.
        start = (now + timedelta(minutes=60)).strftime("%H:%M")
        end = (now + timedelta(minutes=30)).strftime("%H:%M")
        if start < end or end < now.strftime("%H:%M"):
            # Near midnight choose a normal full-day window to avoid a clock-boundary test.
            window = {"days": [now.isoweekday()], "start": "00:00", "end": "00:00"}
        else:
            window = {"days": [7 if now.isoweekday() == 1 else now.isoweekday() - 1],
                      "start": start, "end": end}
        self.base_config["client"]["update_windows"] = [{"days": [(now.isoweekday() % 7) + 1],
                                                         "start": "00:00", "end": "00:00"}]
        self.write_config()
        effective = self.client / "effective.json"
        effective.write_text(json.dumps({"client": {"update_windows": [window]}}), encoding="utf-8")
        self.assertEqual("staged", self.run_update(effective=effective)["status"])
        self.assertEqual(1, len(self.downloads()))

    def test_unsafe_zip_entries_are_rejected_before_activation(self):
        for index, entries in enumerate([
            [("../escape.txt", b"bad")], [("C:/escape.txt", b"bad")],
            [("same.txt", b"one"), ("SAME.txt", b"two")],
        ]):
            with self.subTest(entries=entries):
                artifact = self.artifact(entries, version=str(index + 1))
                previous = self.service.deployments.get("test-worker")["desired"]
                self.plan(artifact, revision=previous["revision"] if previous else None)
                result = self.run_update(expected=1)
                self.assertEqual("failed", result["status"])
                self.assertRegex(result["message"], "Unsafe archive entry|Duplicate archive path")
                self.assertFalse((self.client / "deployments" / "objects" / artifact["sha256"] / "content").exists())
                self.assertFalse((self.client / "deployments" / "objects" / artifact["sha256"] / "escape.txt").exists())

    def test_zip_links_are_rejected(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            entry = zipfile.ZipInfo("linked.stp")
            entry.create_system = 3
            entry.external_attr = (0o120777 << 16)
            archive.writestr(entry, "../../external.stp")
        self.plan(self.artifact(raw=output.getvalue()))
        result = self.run_update(expected=1)
        self.assertIn("Archive links are not allowed", result["message"])

    def test_damaged_cache_is_not_reused_or_replaced(self):
        artifact = self.artifact()
        self.plan(artifact)
        staged = self.run_update()
        content = self.client / "deployments" / "objects" / artifact["sha256"] / "content"
        (content / "nested" / "input.stp").write_bytes(b"changed!")
        result = self.run_update(expected=1)
        self.assertIn("Cached artifact content is inconsistent", result["message"])
        self.assertEqual(1, len(self.downloads()))
        self.assertEqual(b"changed!", (content / "nested" / "input.stp").read_bytes())
        self.assertTrue(Path(staged["config_path"]).is_file())

    def test_stale_report_ack_does_not_block_new_desired_stage(self):
        first = self.artifact()
        old = self.plan(first)
        next_artifact = self.artifact(raw=b"next", filename="input.stp", version="2")
        new = self.plan(next_artifact, revision=old["revision"])
        report = self.client / "report.json"
        report.write_text(json.dumps({
            "desired_revision": old["revision"], "state": "ready",
            "components": [{"slot": "dataset", "artifact_id": first["artifact_id"], "state": "active"}],
        }), encoding="utf-8")
        result = self.run_update(report=report)
        self.assertEqual("staged", result["status"])
        self.assertEqual(new["revision"], result["revision"])
        self.assertTrue(Path(str(report) + ".sent").is_file())
        self.assertTrue(report.is_file())

    def test_prepared_revision_reuses_release_and_still_obeys_effective_window(self):
        desired = self.plan(self.artifact())
        initial = self.run_update()
        repeated = self.run_update(prepared=desired["revision"])
        self.assertEqual("staged", repeated["status"])
        self.assertEqual(initial["config_path"], repeated["config_path"])
        effective = self.client / "effective.json"
        tomorrow = datetime.now().isoweekday() % 7 + 1
        effective.write_text(json.dumps({"client": {"update_windows": [
            {"days": [tomorrow], "start": "00:00", "end": "00:00"},
        ]}}), encoding="utf-8")
        waiting = self.run_update(prepared=desired["revision"], effective=effective)
        self.assertEqual("waiting_window", waiting["status"])
        self.assertEqual(initial["config_path"], waiting["config_path"])
        self.assertEqual("staged", waiting["components"][0]["state"])
        self.assertEqual(1, len(self.downloads()))

    def test_identical_plain_content_can_be_staged_under_different_filenames(self):
        first = self.artifact(raw=b"same executable bytes", filename="first.exe")
        desired = self.plan(first)
        first_result = self.run_update()
        first_config = json.loads(Path(first_result["config_path"]).read_text(encoding="utf-8"))
        first_content = Path(first_config["plugins"]["custom"]["data_root"])
        second = self.artifact(raw=b"same executable bytes", filename="second.exe", version="2")
        self.assertEqual(first["sha256"], second["sha256"])
        self.plan(second, revision=desired["revision"])
        second_result = self.run_update()
        second_config = json.loads(Path(second_result["config_path"]).read_text(encoding="utf-8"))
        second_content = Path(second_config["plugins"]["custom"]["data_root"])
        self.assertNotEqual(first_content, second_content)
        self.assertEqual(b"same executable bytes", (first_content / "first.exe").read_bytes())
        self.assertEqual(b"same executable bytes", (second_content / "second.exe").read_bytes())
        self.assertEqual(1, len(self.downloads()), "only the raw filename changes; archive bytes are reused")
        self.assertEqual("staged", self.run_update()["status"])

    def test_identical_bytes_can_be_materialized_as_zip_and_plain_file(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            archive.writestr("input.stp", "original STP")
        content = output.getvalue()
        opaque = self.artifact(raw=content, filename="opaque.bin")
        desired = self.plan(opaque)
        plain_result = self.run_update()
        plain_config = json.loads(Path(plain_result["config_path"]).read_text(encoding="utf-8"))
        plain_root = Path(plain_config["plugins"]["custom"]["data_root"])
        zipped = self.artifact(raw=content, filename="data.zip", version="2")
        desired = self.plan(zipped, revision=desired["revision"])
        zip_result = self.run_update()
        zip_config = json.loads(Path(zip_result["config_path"]).read_text(encoding="utf-8"))
        zip_root = Path(zip_config["plugins"]["custom"]["data_root"])
        self.assertEqual(content, (plain_root / "opaque.bin").read_bytes())
        self.assertEqual("original STP", (zip_root / "input.stp").read_text(encoding="utf-8"))
        self.assertNotEqual(plain_root, zip_root)
        self.plan(opaque, revision=desired["revision"])
        self.assertEqual("staged", self.run_update()["status"])
        self.assertEqual(1, len(self.downloads()))


if __name__ == "__main__":
    unittest.main()
