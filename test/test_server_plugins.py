from __future__ import annotations

import base64
import hashlib
import io
import json
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import ProxyHandler, Request, build_opener
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.artifacts import ArtifactStore
from result_service.app.config import ServiceConfig
from result_service.app.management import ManagementError, ManagementService
from result_service.app.plugins import default_plugins
from result_service.app.server import Handler, ResultHTTPServer, ResultService
from result_service.app.server_plugins import ServerPluginInstaller, inspect_bundle
from tools.build_plugin_bundle import build_bundle


def make_bundle(plugin="custom", version="1", *, adapter=None, extras=None, definition=None):
    """Reusable ZIP fixture for HTTP tests; no DLL or Python code is executed."""
    manifest = {"schema_version": 1, "package_type": "testtracking-server-plugin",
                "plugin": plugin, "version": version,
                "scheduler": {"dll": "bin/custom.dll", "defaults": {"task_size": 4}},
                "resources": {"view": "result_view.json", "ai_context": "AI_CONTEXT.md",
                              "manifest": "dataset/manifest.txt"}}
    if definition:
        manifest.update(definition)
    files = {"bin/custom.dll": b"test dll bytes", "AI_CONTEXT.md": b"# Plugin owned background\n",
             "result_view.json": json.dumps({"schema_version": 1, "plugin": plugin,
                                              "title": "Custom results", "widgets": []}).encode(),
             "dataset/manifest.txt": b"input.stp\n"}
    if adapter is not None:
        manifest["resources"]["adapter"] = "result_adapter.py"
        files["result_adapter.py"] = adapter.encode()
    files.update(extras or {})
    files["server_plugin.json"] = json.dumps(manifest).encode()
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            info = zipfile.ZipInfo(name)
            # ZipInfo normalizes backslashes on Windows. Keep the supplied
            # spelling so this fixture can represent unsafe incoming archives.
            info.filename = info.orig_filename = name
            archive.writestr(info, data)
    return output.getvalue()


class ServerPluginInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="server-plugin-插件-")
        self.root = Path(self.temporary.name)
        self.scheduler_path = self.root / "scheduler.json"
        self.result_path = self.root / "result_service.json"
        self.scheduler_path.write_text(json.dumps({"server": {}, "plugins": {
            "existing": {"enabled": True, "dll": "old.dll", "task_size": 7,
                         "stp_manifest": "dataset/old.txt", "db_conn": "dbname=test"}}}), encoding="utf-8")
        self.resources = self.root / "original"
        for directory in ("views", "adapters", "ai_contexts", "report_templates", "scripts"):
            (self.resources / directory).mkdir(parents=True)
        (self.resources / "views/existing.json").write_text(json.dumps({
            "schema_version": 1, "plugin": "existing", "title": "Existing", "widgets": []}), encoding="utf-8")
        (self.resources / "adapters/existing.py").write_text(
            "class Adapter:\n configured = True\n def summary(self, connection, run_id): return {}\n"
            "def create_result_adapter(options): return Adapter()\n", encoding="utf-8")
        (self.resources / "ai_contexts/existing.md").write_text("# Existing plugin", encoding="utf-8")
        (self.resources / "report_templates/existing.py").write_text("# Original template", encoding="utf-8")
        (self.resources / "scripts/existing.py").write_text("# User custom report", encoding="utf-8")
        self.result_path.write_text(json.dumps({"workspace": ".", "scheduler_config_path": "scheduler.json",
            "artifact_store_dir": "artifacts",
            "plugin_views_dir": "original/views", "plugin_adapters_dir": "original/adapters",
            "plugin_ai_contexts_dir": "original/ai_contexts", "report_templates_dir": "original/report_templates",
            "analysis_scripts_dir": "original/scripts", "providers": {},
            "plugin_options": {"existing": {"custom_table": "saved_table"}}}), encoding="utf-8")
        self.config = ServiceConfig.from_file(self.result_path)
        self.management = ManagementService(self.config)
        self.management.require_plugin_idle = Mock(return_value=None)
        self.management.plugin_maintenance = Mock(return_value={"ok": True})
        self.management.get_status = Mock(return_value={"online": True, "instance_id": "scheduler-1"})
        self.store = ArtifactStore(self.root / "artifacts")
        self.installer = ServerPluginInstaller(self.config, self.store, self.management)

    def tearDown(self):
        self.temporary.cleanup()

    def upload(self, payload=None, plugin="custom", version="1", **fields):
        payload = payload if payload is not None else make_bundle(plugin, version)
        record, _ = self.store.upload(io.BytesIO(payload), len(payload), kind="plugin",
                                     name=plugin + "-server", version=version,
                                     filename=plugin + "-server.zip", **fields)
        return record

    def plan(self, record):
        return self.installer.preview({"artifact_id": record["artifact_id"]})

    def install(self, record):
        plan = self.plan(record)
        return self.installer.install({"artifact_id": record["artifact_id"],
            "scheduler_revision": plan["scheduler_revision"],
            "result_service_revision": plan["result_service_revision"], "operator_id": "alice"})

    def test_preview_is_read_only_and_never_executes_adapter(self):
        sentinel = self.root / "adapter-executed"
        source = ("from pathlib import Path\nPath(" + repr(str(sentinel)) + ").write_text('unsafe')\n"
                  "def create_result_adapter(options): return None\n")
        record = self.upload(make_bundle(adapter=source))
        before = (self.scheduler_path.read_bytes(), self.result_path.read_bytes())
        plan = self.plan(record)
        self.assertEqual("install", plan["action"])
        self.assertFalse(plan["enabled_after_install"])
        self.assertIn("adapter", plan["resources"])
        self.assertFalse(sentinel.exists())
        self.assertFalse(self.installer.root.exists())
        self.assertEqual(before, (self.scheduler_path.read_bytes(), self.result_path.read_bytes()))
        self.management.require_plugin_idle.assert_not_called()

    def test_install_adds_disabled_plugin_preserves_resources_and_user_scripts(self):
        previous_scheduler = self.scheduler_path.read_bytes()
        previous_result = self.result_path.read_bytes()
        record = self.upload()
        outcome = self.install(record)
        scheduler = json.loads(self.scheduler_path.read_text(encoding="utf-8"))
        result = json.loads(self.result_path.read_text(encoding="utf-8"))
        installed = scheduler["plugins"]["custom"]
        self.assertFalse(installed["enabled"])
        self.assertEqual(4, installed["task_size"])
        self.assertFalse(Path(installed["dll"]).is_absolute())
        self.assertEqual(b"test dll bytes", (self.root / installed["dll"]).read_bytes())
        self.assertTrue((self.root / installed["stp_manifest"]).is_file())
        self.assertEqual(json.loads(previous_scheduler)["plugins"]["existing"], scheduler["plugins"]["existing"])
        self.assertEqual({"existing": {"custom_table": "saved_table"}}, result["plugin_options"])
        self.assertEqual("# User custom report", (self.resources / "scripts/existing.py").read_text())
        self.assertEqual("original/scripts", result["analysis_scripts_dir"])
        self.assertTrue((self.resources / "views/existing.json").is_file())
        loaded = ServiceConfig.from_file(self.result_path)
        plugins = default_plugins(loaded.plugin_options, loaded.plugin_views_dir, loaded.plugin_adapters_dir,
                                  loaded.report_templates_dir, loaded.plugin_ai_contexts_dir)
        self.assertEqual({"existing", "custom"}, set(plugins))
        self.assertTrue(plugins["existing"].configured)
        self.assertFalse(plugins["custom"].configured)
        destination = Path(outcome["installation_path"])
        self.assertEqual(previous_scheduler, (destination / "scheduler.before.json").read_bytes())
        self.assertEqual(previous_result, (destination / "result_service.before.json").read_bytes())
        self.assertEqual(["scheduler", "result_service"], outcome["restart_required"])
        self.assertEqual(2, self.management.require_plugin_idle.call_count)
        self.assertTrue(self.management._scheduler_save_pending)
        self.assertFalse(self.installer.journal.exists())

    def test_install_commits_native_maintenance_until_restart(self):
        outcome = self.install(self.upload())
        calls = self.management.plugin_maintenance.call_args_list
        self.assertEqual(["begin_plugin_maintenance", "end_plugin_maintenance"], [call.args[0] for call in calls])
        self.assertEqual(calls[0].kwargs["token"], calls[1].kwargs["token"])
        self.assertTrue(calls[1].kwargs["committed"])
        self.assertEqual(outcome["scheduler_revision"], calls[1].kwargs["revision"])
        self.assertFalse(self.installer.maintenance_journal.exists())

    def test_configuration_alias_uses_persisted_runtime_type_for_maintenance(self):
        self.management.get_status.return_value = {'online': True, 'instance_id': 'scheduler-1',
            'plugin_config_keys': {'coverage': 'existing'}}
        record = self.upload(make_bundle('existing', '2'), plugin='existing', version='2')
        self.install(record)
        self.assertTrue(all(call.args == ('coverage',) for call in self.management.require_plugin_idle.call_args_list))
        self.assertTrue(all(call.kwargs['plugin'] == 'coverage'
                            for call in self.management.plugin_maintenance.call_args_list))

    def test_unreachable_native_barrier_never_changes_configuration(self):
        before = self.scheduler_path.read_bytes(), self.result_path.read_bytes()
        self.management.plugin_maintenance.side_effect = ManagementError("offline", 503)
        with self.assertRaisesRegex(ManagementError, "offline"):
            self.install(self.upload())
        self.assertEqual(before, (self.scheduler_path.read_bytes(), self.result_path.read_bytes()))
        self.assertTrue(self.installer.maintenance_journal.exists())
        self.management.plugin_maintenance.side_effect = None
        recovered = ServerPluginInstaller(self.config, self.store, self.management)
        last = self.management.plugin_maintenance.call_args
        self.assertEqual("end_plugin_maintenance", last.args[0])
        self.assertFalse(last.kwargs["committed"])
        self.assertEqual(hashlib.sha256(before[0]).hexdigest(), last.kwargs["revision"])
        self.assertFalse(recovered.maintenance_journal.exists())

    def test_lost_commit_reply_preserves_configuration_and_retries_same_token(self):
        def maintenance(action, **values):
            if action == "end_plugin_maintenance":
                raise ManagementError("lost acknowledgement", 503)
            return {"ok": True}
        self.management.plugin_maintenance.side_effect = maintenance
        outcome = self.install(self.upload())
        self.assertTrue(outcome["installed"])
        self.assertTrue(outcome["warnings"])
        self.assertTrue(Path(outcome["installation_path"]).is_dir())
        self.assertTrue(self.installer.maintenance_journal.exists())
        original_token = self.management.plugin_maintenance.call_args.kwargs["token"]
        # A disconnected scheduler cannot take down result-service startup.
        offline = ServerPluginInstaller(self.config, self.store, self.management)
        self.assertTrue(offline.transaction_warning)
        self.assertTrue(offline.maintenance_journal.exists())
        self.management.plugin_maintenance.side_effect = None
        ServerPluginInstaller(self.config, self.store, self.management)
        retried = self.management.plugin_maintenance.call_args
        self.assertTrue(retried.kwargs["committed"])
        self.assertEqual(original_token, retried.kwargs["token"])
        self.assertFalse(self.installer.maintenance_journal.exists())

    def test_maintenance_journal_cleanup_failure_does_not_remove_committed_installation(self):
        original_unlink = Path.unlink
        def fail_cleanup(path, *args, **kwargs):
            if path == self.installer.maintenance_journal:
                raise PermissionError('maintenance cleanup denied')
            return original_unlink(path, *args, **kwargs)
        with patch.object(Path, 'unlink', fail_cleanup):
            outcome = self.install(self.upload())
        self.assertTrue(outcome['installed'])
        self.assertTrue(outcome['warnings'])
        self.assertTrue(Path(outcome['installation_path']).is_dir())
        self.assertTrue(self.installer.maintenance_journal.exists())
        self.assertTrue(self.management.plugin_maintenance.call_args.kwargs['committed'])
        ServerPluginInstaller(self.config, self.store, self.management)
        self.assertFalse(self.installer.maintenance_journal.exists())

    def test_upgrade_preserves_business_configuration_and_does_not_overwrite_old_files(self):
        old_dll = self.root / "old.dll"
        old_dll.write_bytes(b"original deployment")
        record = self.upload(make_bundle("existing", "2"), plugin="existing", version="2")
        self.assertEqual("upgrade", self.plan(record)["action"])
        outcome = self.install(record)
        scheduler = json.loads(self.scheduler_path.read_text())
        upgraded = scheduler["plugins"]["existing"]
        self.assertTrue(upgraded["enabled"])
        self.assertEqual(7, upgraded["task_size"])
        self.assertEqual("dbname=test", upgraded["db_conn"])
        self.assertEqual("dataset/old.txt", upgraded["stp_manifest"])
        self.assertEqual(b"original deployment", old_dll.read_bytes())
        self.assertFalse((Path(outcome["installation_path"]) / "adapters/existing.py").exists())
        self.assertTrue((self.resources / "adapters/existing.py").is_file())

    def test_multiple_installs_preserve_previously_installed_plugins_before_restart(self):
        first = self.install(self.upload())
        second = self.install(self.upload(make_bundle("another"), plugin="another"))
        loaded = ServiceConfig.from_file(self.result_path)
        plugins = default_plugins(loaded.plugin_options, loaded.plugin_views_dir, loaded.plugin_adapters_dir,
                                  loaded.report_templates_dir, loaded.plugin_ai_contexts_dir)
        self.assertEqual({"existing", "custom", "another"}, set(plugins))
        self.assertNotEqual(first["installation_path"], second["installation_path"])
        self.assertTrue(Path(first["installation_path"]).is_dir())

    def test_stale_configuration_or_unfinished_work_cannot_install(self):
        record = self.upload()
        plan = self.plan(record)
        self.result_path.write_bytes(self.result_path.read_bytes() + b"\n")
        with self.assertRaises(ManagementError) as caught:
            self.installer.install(plan)
        self.assertEqual(409, caught.exception.status)
        self.management.require_plugin_idle.side_effect = ManagementError("unfinished", 409)
        with self.assertRaisesRegex(ManagementError, "unfinished"):
            self.install(record)
        self.assertFalse(self.installer.root.exists())

    def test_config_changed_during_preparation_is_not_overwritten(self):
        record = self.upload()
        original = self.installer._snapshot_resources
        def change_configuration(*args):
            original(*args)
            self.scheduler_path.write_bytes(self.scheduler_path.read_bytes() + b"\n")
        with patch.object(self.installer, "_snapshot_resources", side_effect=change_configuration):
            with self.assertRaises(ManagementError) as caught:
                self.install(record)
        self.assertEqual(409, caught.exception.status)
        self.assertTrue(self.scheduler_path.read_bytes().endswith(b"\n"))
        self.assertNotIn("custom", json.loads(self.scheduler_path.read_text())["plugins"])

    def test_second_configuration_write_failure_restores_both_and_pending_marker(self):
        record = self.upload()
        marker = self.management._scheduler_state_path
        old_marker = b'{"instance_id":"previous","revision":"previous"}'
        marker.write_bytes(old_marker)
        original = self.management._atomic_write
        before = self.scheduler_path.read_bytes(), self.result_path.read_bytes()
        failures = []
        def fail_once(path, data):
            if path == self.result_path and not failures:
                failures.append(True)
                raise OSError("injected write failure")
            return original(path, data)
        with patch.object(self.management, "_atomic_write", side_effect=fail_once):
            with self.assertRaisesRegex(ManagementError, "injected write failure"):
                self.install(record)
        self.assertEqual(before, (self.scheduler_path.read_bytes(), self.result_path.read_bytes()))
        self.assertEqual(old_marker, marker.read_bytes())
        self.assertEqual("previous", self.management._pending_scheduler_revision)
        self.assertFalse(self.installer.journal.exists())
        self.assertEqual("end_plugin_maintenance", self.management.plugin_maintenance.call_args.args[0])
        self.assertFalse(self.management.plugin_maintenance.call_args.kwargs["committed"])
        self.assertEqual(hashlib.sha256(before[0]).hexdigest(), self.management.plugin_maintenance.call_args.kwargs["revision"])

    def test_interrupted_configuration_commit_is_recovered_on_next_initialization(self):
        old_scheduler, old_result = self.scheduler_path.read_bytes(), self.result_path.read_bytes()
        new_scheduler, new_result = old_scheduler + b"\n", old_result + b"\n"
        self.installer.root.mkdir(parents=True)
        self.installer.journal.write_text(json.dumps({"entries": [
            {"path": str(path), "old": base64.b64encode(old).decode(),
             "new_revision": hashlib.sha256(new).hexdigest()} for path, old, new in (
                (self.scheduler_path, old_scheduler, new_scheduler), (self.result_path, old_result, new_result))]}))
        self.scheduler_path.write_bytes(new_scheduler)
        recovered = ServerPluginInstaller(self.config, self.store, self.management)
        self.assertTrue(recovered.recovered_configuration)
        self.assertEqual(old_scheduler, self.scheduler_path.read_bytes())
        self.assertEqual(old_result, self.result_path.read_bytes())
        self.assertFalse(self.installer.journal.exists())

    def test_completed_commit_with_cleanup_failure_remains_installed(self):
        record = self.upload()
        original_unlink = Path.unlink
        def fail_journal_cleanup(path, *args, **kwargs):
            if path == self.installer.journal:
                raise PermissionError("journal cleanup temporarily denied")
            return original_unlink(path, *args, **kwargs)
        with patch.object(Path, "unlink", fail_journal_cleanup):
            outcome = self.install(record)
            self.assertTrue(outcome["installed"])
            self.assertTrue(outcome["warnings"])
            self.assertTrue(self.installer.journal.exists())
            self.assertTrue(Path(outcome["installation_path"]).is_dir())
            restarted = ServerPluginInstaller(self.config, self.store, self.management)
            self.assertFalse(restarted.recovered_configuration)
            self.assertTrue(restarted.transaction_warning)
        restarted = ServerPluginInstaller(self.config, self.store, self.management)
        self.assertFalse(restarted.recovered_configuration)
        self.assertFalse(self.installer.journal.exists())
        self.assertIn("custom", json.loads(self.scheduler_path.read_text())["plugins"])

    def test_missing_previously_declared_resource_prevents_silent_downgrade(self):
        manifest = self.resources / "views/resources.manifest"
        manifest.write_text(json.dumps({"schema_version": 1,
                                      "plugins": {"existing": ["view", "adapter", "ai_context"]}}))
        (self.resources / "adapters/existing.py").unlink()
        before = self.scheduler_path.read_bytes(), self.result_path.read_bytes()
        with self.assertRaisesRegex(ManagementError, "现有插件资源不完整"):
            self.install(self.upload())
        self.assertEqual(before, (self.scheduler_path.read_bytes(), self.result_path.read_bytes()))

    def test_service_startup_restores_partial_configuration_before_loading_adapter(self):
        old_scheduler, old_result = self.scheduler_path.read_bytes(), self.result_path.read_bytes()
        sentinel = self.root / "wrong-adapter-loaded"
        poison = self.root / "poison-adapters"
        poison.mkdir()
        (poison / "existing.py").write_text(
            "from pathlib import Path\nPath(" + repr(str(sentinel)) + ").write_text('loaded')\n"
            "raise RuntimeError('must restore the transaction before loading this adapter')\n",
            encoding="utf-8")
        next_result = json.loads(old_result)
        next_result["plugin_adapters_dir"] = "poison-adapters"
        next_result["plugin_ai_contexts_dir"] = "missing-contexts"
        new_scheduler = old_scheduler + b"\n"
        new_result = json.dumps(next_result).encode()
        self.installer.root.mkdir(parents=True)
        self.installer.journal.write_text(json.dumps({"entries": [
            {"path": str(path), "old": base64.b64encode(old).decode(),
             "new_revision": hashlib.sha256(new).hexdigest()} for path, old, new in (
                (self.scheduler_path, old_scheduler, new_scheduler),
                (self.result_path, old_result, new_result))]}))
        # A partially completed rollback can leave the result config pointing
        # at the attempted version while the scheduler config is already old.
        self.result_path.write_bytes(new_result)
        service = ResultService(ServiceConfig.from_file(self.result_path))
        try:
            self.assertEqual(old_scheduler, self.scheduler_path.read_bytes())
            self.assertEqual(old_result, self.result_path.read_bytes())
            self.assertEqual(str(self.resources / "adapters"), service.config.plugin_adapters_dir)
            self.assertEqual((200, {"status": "ok", "service": "result_service"}),
                             service.payload("/healthz", {}))
            self.assertEqual({"existing"}, set(service.plugins))
            self.assertTrue(service.plugins["existing"].configured)
            self.assertFalse(sentinel.exists())
            self.assertFalse(self.installer.journal.exists())
        finally:
            service.close()

    def test_http_upload_preview_install_and_restart_load_plugin_and_ai_context(self):
        service = ResultService(self.config)
        service.management.require_plugin_idle = Mock(return_value=None)
        service.management.plugin_maintenance = Mock(return_value={"ok": True})
        service.management.get_status = Mock(return_value={"online": True, "instance_id": "scheduler-1"})
        handler = type("ServerPluginHTTPHandler", (Handler,), {
            "service": service, "log_message": lambda *args: None})
        server = ResultHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        opener = build_opener(ProxyHandler({}))
        def request(path, body=None, *, raw=False):
            headers = {"Content-Type": "application/octet-stream" if raw else "application/json"}
            data = body if raw or body is None else json.dumps(body).encode()
            try:
                response = opener.open(Request(base + path, data=data, headers=headers), timeout=5)
            except HTTPError as exc:
                response = exc
            with response:
                return response.status, json.loads(response.read())
        restarted = None
        try:
            adapter = ("class Adapter:\n configured = True\n"
                       " def summary(self, connection, run_id): return {'source': 'custom'}\n"
                       "def create_result_adapter(options): return Adapter()\n")
            status, artifact = request(
                "/api/artifacts/upload?kind=plugin&name=custom-server&version=1&filename=custom-server.zip",
                make_bundle(adapter=adapter), raw=True)
            self.assertEqual(201, status)
            status, plan = request("/api/server-plugins/preview", {"artifact_id": artifact["artifact_id"]})
            self.assertEqual(200, status)
            self.assertEqual("custom", plan["plugin"])
            self.assertFalse(plan["enabled_after_install"])
            status, installed = request("/api/server-plugins/install", {
                "artifact_id": artifact["artifact_id"], "scheduler_revision": plan["scheduler_revision"],
                "result_service_revision": plan["result_service_revision"], "operator_id": "test-operator"})
            self.assertEqual(200, status)
            self.assertTrue(installed["installed"])
            self.assertEqual(["scheduler", "result_service"], installed["restart_required"])
            self.assertEqual({"existing"}, {item["name"] for item in request("/api/plugins")[1]})
            self.assertEqual(404, request("/api/plugins/custom/ai-context")[0])
            self.assertEqual(409, request("/api/server-plugins/install", plan)[0])
            restarted = ResultService(ServiceConfig.from_file(self.result_path))
            handler.service = restarted
            status, plugins = request("/api/plugins")
            self.assertEqual(200, status)
            self.assertEqual({"existing", "custom"}, {item["name"] for item in plugins})
            self.assertTrue(next(item for item in plugins if item["name"] == "custom")["configured"])
            status, context = request("/api/plugins/custom/ai-context")
            self.assertEqual(200, status)
            self.assertEqual("# Plugin owned background\n", context["content"])
            self.assertEqual("custom", context["plugin"])
            self.assertEqual({"source": "custom"}, restarted.plugins["custom"].summary(None, "run-1"))
        finally:
            server.shutdown()
            thread.join(5)
            server.server_close()
            service.close()
            if restarted is not None:
                restarted.close()

    def test_corrupt_artifact_or_metadata_mismatch_is_rejected(self):
        record = self.upload()
        path = self.store.object_path(record["sha256"])
        path.write_bytes(b"x" * path.stat().st_size)
        with self.assertRaisesRegex(ManagementError, "SHA-256"):
            self.plan(record)
        record = self.upload(make_bundle(version="9"), version="2")
        with self.assertRaisesRegex(ManagementError, "版本"):
            self.plan(record)

    def test_unsafe_archive_and_invalid_plugin_resources_are_rejected(self):
        for index, extra in enumerate(("../escape.dll", "bin/CUSTOM.dll", "C:/bad.dll", "NUL.txt",
                                       "dir\\file.py", "trailing. ", "bin", "bin/custom.dll/nested")):
            with self.subTest(path=extra):
                record = self.upload(make_bundle(version=str(index), extras={extra: b"bad"}), version=str(index))
                with self.assertRaises(ManagementError):
                    self.plan(record)
        for index, source in enumerate(("this is not python !", "answer = 42"), 20):
            record = self.upload(make_bundle(version=str(index), adapter=source), version=str(index))
            with self.assertRaises(ManagementError):
                self.plan(record)
        record = self.upload(make_bundle(version="30", extras={"AI_CONTEXT.md": b" "}), version="30")
        with self.assertRaises(ManagementError):
            self.plan(record)

    def test_source_resource_discovery_is_preserved_without_explicit_directories(self):
        original = json.loads(self.result_path.read_text())
        for key in ("plugin_views_dir", "plugin_adapters_dir", "plugin_ai_contexts_dir", "report_templates_dir"):
            original.pop(key)
        self.result_path.write_text(json.dumps(original))
        self.install(self.upload())
        loaded = ServiceConfig.from_file(self.result_path)
        names = {path.stem for path in Path(loaded.plugin_views_dir).glob("*.json")}
        self.assertTrue({"coverage", "stpcompare", "custom"}.issubset(names))


class ServerPluginBundleBuilderTests(unittest.TestCase):
    def test_builder_produces_deterministic_offline_bundle_with_optional_files(self):
        with tempfile.TemporaryDirectory(prefix="bundle-build-") as directory:
            root = Path(directory)
            plugin = root / "custom"
            plugin.mkdir()
            (plugin / "result_view.json").write_text(json.dumps({"schema_version": 1, "plugin": "custom",
                                                                "title": "Custom", "widgets": []}))
            (plugin / "AI_CONTEXT.md").write_text("# Schema and semantics")
            dll = root / "custom.dll"
            dll.write_bytes(b"dll bytes")
            dependency = root / "local.dll"
            dependency.write_bytes(b"dependency")
            manifest = root / "input.txt"
            manifest.write_text("one.stp\n")
            defaults = root / "defaults.json"
            defaults.write_text('{"task_size":10}')
            output = root / "custom-server-1.zip"
            options = dict(plugin_dir=plugin, dll=dll, version="1", output=output,
                           dependencies=(dependency,), manifest=manifest, defaults=defaults)
            build_bundle(**options)
            first = output.read_bytes()
            definition, files = inspect_bundle(output)
            self.assertEqual("custom", definition["plugin"])
            self.assertEqual(10, definition["scheduler"]["defaults"]["task_size"])
            self.assertIn("bin/local.dll", {item["path"] for item in files})
            build_bundle(**options)
            self.assertEqual(first, output.read_bytes())
            (plugin / "AI_CONTEXT.md").unlink()
            with self.assertRaises(ManagementError):
                build_bundle(**options)
            self.assertEqual(first, output.read_bytes())


if __name__ == "__main__":
    unittest.main()
