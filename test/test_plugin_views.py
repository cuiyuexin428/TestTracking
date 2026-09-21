from __future__ import annotations

import copy
import json
import os
import sys
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from result_service.app.config import ServiceConfig
from result_service.app.plugins import (ResultPluginError, default_plugins,
                                        save_versioned_plugin_view, versioned_plugin_view)
from result_service.app.server import Handler, ResultHTTPServer, ResultService
from result_service.app.views import PluginViewError, PluginViewStore, validate_plugin_view


class PluginViewContractTests(unittest.TestCase):
    def test_business_plugins_own_valid_builtin_views(self) -> None:
        store = PluginViewStore()

        coverage = store.load("coverage")
        stpcompare = store.load("stpcompare")

        self.assertEqual("coverage", coverage["plugin"])
        self.assertEqual("analysis.percent", coverage["primary_metric"]["value"])
        self.assertTrue((ROOT / "plugins" / "coverage" / "result_view.json").is_file())
        self.assertEqual("stpcompare", stpcompare["plugin"])
        self.assertEqual("analysis.complete", stpcompare["primary_metric"]["value"])
        self.assertTrue((ROOT / "plugins" / "stpcompare" / "result_view.json").is_file())

    def test_runtime_view_is_persisted_and_reloaded(self) -> None:
        definition = PluginViewStore().load("coverage")
        with tempfile.TemporaryDirectory() as temp_dir:
            store = PluginViewStore(temp_dir)
            changed = copy.deepcopy(definition)
            changed["title"] = "自定义覆盖率看板"

            saved = store.save("coverage", changed)

            self.assertEqual("自定义覆盖率看板", saved["title"])
            self.assertEqual(saved, store.load("coverage"))
            persisted = json.loads(
                (Path(temp_dir) / "coverage.json").read_text(encoding="utf-8")
            )
            self.assertEqual(saved, persisted)
            self.assertEqual([], list(Path(temp_dir).glob("*.tmp")))

    def test_packaged_plugin_view_and_adapter_are_discovered_without_registration(self) -> None:
        definitions = {
            name: PluginViewStore().load(name) for name in ("coverage", "stpcompare")
        }
        custom = copy.deepcopy(definitions["coverage"])
        custom.update(plugin="vendor-plugin", title="Vendor result")
        with tempfile.TemporaryDirectory() as temp_dir:
            view_dir = Path(temp_dir)
            adapter_dir = view_dir / "adapters"
            adapter_dir.mkdir()
            for name, definition in definitions.items():
                (view_dir / f"{name}.json").write_text(
                    json.dumps(definition), encoding="utf-8"
                )
            (view_dir / "vendor-plugin.json").write_text(
                json.dumps(custom), encoding="utf-8"
            )
            (adapter_dir / "vendor-plugin.py").write_text(
                "class Adapter:\n"
                "    configured = True\n"
                "    def summary(self, connection, run_id):\n"
                "        return {'vendor_value': 42}\n"
                "def create_result_adapter(options):\n"
                "    return Adapter()\n",
                encoding="utf-8",
            )

            service = ResultService(ServiceConfig(
                plugin_views_dir=temp_dir,
                plugin_adapters_dir=str(adapter_dir),
            ))
            status, plugins = service.payload("/api/plugins", {})

            vendor = next(item for item in plugins if item["name"] == "vendor-plugin")
            self.assertEqual(200, status)
            self.assertTrue(vendor["configured"])
            self.assertEqual("Vendor result", vendor["view"]["title"])
            self.assertEqual(
                {"vendor_value": 42},
                service.plugins["vendor-plugin"].summary(None, "run-1"),
            )

    def test_unsupported_executable_widget_does_not_replace_existing_view(self) -> None:
        definition = PluginViewStore().load("coverage")
        with tempfile.TemporaryDirectory() as temp_dir:
            store = PluginViewStore(temp_dir)
            store.save("coverage", definition)
            original = (Path(temp_dir) / "coverage.json").read_bytes()
            invalid = copy.deepcopy(definition)
            invalid["widgets"] = [{
                "id": "custom_html",
                "type": "html",
                "title": "Custom",
                "script": "alert(1)",
            }]

            with self.assertRaisesRegex(PluginViewError, "type is not supported"):
                store.save("coverage", invalid)

            self.assertEqual(original, (Path(temp_dir) / "coverage.json").read_bytes())

    def test_view_cannot_be_saved_for_a_different_plugin(self) -> None:
        definition = PluginViewStore().load("coverage")

        with self.assertRaisesRegex(PluginViewError, "view.plugin must be stpcompare"):
            validate_plugin_view(definition, expected_plugin="stpcompare")

    def test_value_references_are_limited_to_result_view_model(self) -> None:
        definition = PluginViewStore().load("coverage")
        definition["primary_metric"]["value"] = "window.location"

        with self.assertRaisesRegex(PluginViewError, "must reference run"):
            validate_plugin_view(definition)

    def test_schema_endpoint_describes_data_only_widgets(self) -> None:
        service = ResultService(ServiceConfig.defaults())

        status, schema = service.payload("/api/plugin-view-schema", {})

        self.assertEqual(200, status)
        self.assertEqual(1, schema["properties"]["schema_version"]["const"])
        serialized = json.dumps(schema)
        self.assertNotIn('"html"', serialized)
        self.assertNotIn('"script"', serialized)

    def test_plugin_api_returns_and_updates_editable_definition(self) -> None:
        definition = PluginViewStore().load("coverage")
        with tempfile.TemporaryDirectory() as temp_dir:
            view_dir = Path(temp_dir)
            (view_dir / "coverage.json").write_text(
                json.dumps(definition), encoding="utf-8"
            )
            stpcompare = PluginViewStore().load("stpcompare")
            (view_dir / "stpcompare.json").write_text(
                json.dumps(stpcompare), encoding="utf-8"
            )
            service = ResultService(ServiceConfig(plugin_views_dir=temp_dir))
            _, changed = service.payload("/api/plugins/coverage/view", {})
            original_revision = changed["revision"]
            changed["title"] = "AI 生成的覆盖率视图"

            status, saved = service.update_plugin_view(
                "/api/plugins/coverage/view", changed
            )
            get_status, loaded = service.payload("/api/plugins/coverage/view", {})
            _, plugins = service.payload("/api/plugins", {})

            self.assertEqual(200, status)
            self.assertEqual(200, get_status)
            self.assertEqual(saved, loaded)
            self.assertNotEqual(original_revision, saved["revision"])
            self.assertNotIn("revision", json.loads((view_dir / "coverage.json").read_text(encoding="utf-8")))
            coverage = next(item for item in plugins if item["name"] == "coverage")
            self.assertTrue(coverage["view_editable"])
            self.assertEqual("AI 生成的覆盖率视图", coverage["view"]["title"])

    def test_valid_update_requires_revision_and_keeps_existing_definition(self) -> None:
        service = ResultService(ServiceConfig.defaults())
        self.addCleanup(service.close)
        _, original = service.payload("/api/plugins/coverage/view", {})
        changed = {key: value for key, value in original.items() if key != "revision"}
        changed["title"] = "Missing revision"

        status, payload = service.update_plugin_view("/api/plugins/coverage/view", changed)

        self.assertEqual(400, status)
        self.assertIn("revision is required", payload["error"])
        self.assertEqual((200, original), service.payload("/api/plugins/coverage/view", {}))

    def test_concurrent_service_instances_only_accept_one_save_from_same_revision(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            store = PluginViewStore(temp_dir)
            store.save("coverage", PluginViewStore().load("coverage"))
            services = [ResultService(ServiceConfig(plugin_views_dir=temp_dir)) for _ in range(2)]
            try:
                drafts = [service.payload("/api/plugins/coverage/view", {})[1] for service in services]
                self.assertEqual(drafts[0]["revision"], drafts[1]["revision"])
                barrier = threading.Barrier(2)

                def save(index):
                    draft = drafts[index]
                    draft["title"] = f"Operator {index}"
                    barrier.wait(timeout=3)
                    return services[index].update_plugin_view("/api/plugins/coverage/view", draft)

                with ThreadPoolExecutor(max_workers=2) as executor:
                    results = list(executor.map(save, range(2)))

                self.assertEqual([200, 409], sorted(status for status, _ in results))
                saved = next(payload for status, payload in results if status == 200)
                conflict = next(payload for status, payload in results if status == 409)
                self.assertEqual(saved["revision"], conflict["current_revision"])
                self.assertIn("其他使用者", conflict["error"])
                self.assertEqual((200, saved), services[0].payload("/api/plugins/coverage/view", {}))
                self.assertEqual(saved["title"], store.load("coverage")["title"])
                self.assertNotIn("revision", store.load("coverage"))
                self.assertEqual([], list(Path(temp_dir).glob("*.tmp")))
            finally:
                for service in services:
                    service.close()

    def test_versioned_api_preserves_plugin_owned_single_argument_save_contract(self) -> None:
        class CustomPlugin:
            name = "coverage"

            def __init__(self):
                self.definition = PluginViewStore().load(self.name)

            def view_definition(self):
                return copy.deepcopy(self.definition)

            def save_view_definition(self, value):
                validate_plugin_view(value, expected_plugin=self.name)
                self.definition = copy.deepcopy(value)
                return self.view_definition()

        plugin = CustomPlugin()
        draft = versioned_plugin_view(plugin)
        draft["title"] = "Custom plugin"
        saved = save_versioned_plugin_view(plugin, draft)
        self.assertEqual("Custom plugin", saved["title"])
        self.assertNotEqual(draft["revision"], saved["revision"])
        self.assertNotIn("revision", plugin.definition)

    def test_invalid_update_returns_clear_client_error(self) -> None:
        service = ResultService(ServiceConfig.defaults())
        status, payload = service.update_plugin_view(
            "/api/plugins/coverage/view",
            {"schema_version": 1, "plugin": "coverage"},
        )

        self.assertEqual(400, status)
        self.assertIn("missing required field", payload["error"])

    def test_http_put_makes_the_new_view_visible_to_get(self) -> None:
        definitions = {
            name: PluginViewStore().load(name) for name in ("coverage", "stpcompare")
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            for name, definition in definitions.items():
                (Path(temp_dir) / f"{name}.json").write_text(
                    json.dumps(definition), encoding="utf-8"
                )
            service = ResultService(ServiceConfig(plugin_views_dir=temp_dir))
            handler = type("ViewHandler", (Handler,), {"service": service})
            server = ResultHTTPServer(("127.0.0.1", 0), handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                url = f"http://127.0.0.1:{server.server_address[1]}/api/plugins/coverage/view"
                with urlopen(url, timeout=3) as response:
                    changed = json.loads(response.read().decode("utf-8"))
                changed["title"] = "HTTP 更新后的视图"
                request = Request(
                    url,
                    data=json.dumps(changed).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="PUT",
                )
                with urlopen(request, timeout=3) as response:
                    self.assertEqual(200, response.status)
                with urlopen(url, timeout=3) as response:
                    loaded = json.loads(response.read().decode("utf-8"))
                with self.assertRaises(HTTPError) as rejected:
                    urlopen(request, timeout=3)
                self.assertEqual(409, rejected.exception.code)
                with rejected.exception as response:
                    conflict = json.loads(response.read().decode("utf-8"))
                self.assertEqual(loaded["revision"], conflict["current_revision"])
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=3)
                service.close()

            self.assertEqual("HTTP 更新后的视图", loaded["title"])


class PackagedPluginResourceTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="plugin resources ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.views = self.root / "views"
        self.adapters = self.root / "adapters"
        self.reports = self.root / "reports"
        self.contexts = self.root / "contexts"
        for directory in (self.views, self.adapters, self.reports, self.contexts):
            directory.mkdir()
        view = PluginViewStore().load("coverage")
        for name in ("vendor-plugin", "view-only"):
            definition = copy.deepcopy(view)
            definition["plugin"] = name
            (self.views / f"{name}.json").write_text(json.dumps(definition), encoding="utf-8")
        (self.adapters / "vendor-plugin.py").write_text(
            "class Adapter:\n"
            "    configured = True\n"
            "    def summary(self, connection, run_id): return {'value': 7}\n"
            "def create_result_adapter(options): return Adapter()\n", encoding="utf-8")
        (self.reports / "vendor-plugin.py").write_text(
            "def render(connection, context): return '<html>Vendor report</html>'\n", encoding="utf-8")
        (self.contexts / "vendor-plugin.md").write_text(
            "# Vendor plugin\n\nResults are stored by the vendor adapter.\n", encoding="utf-8")
        (self.views / "resources.manifest").write_text(json.dumps({
            "schema_version": 1,
            "plugins": {"vendor-plugin": ["view", "adapter", "report_template", "ai_context"],
                        "view-only": ["view"]},
        }), encoding="utf-8")

    def load(self, adapters=None, reports=None, contexts=None):
        return default_plugins(views_dir=str(self.views),
                               adapters_dir=str(adapters or self.adapters),
                               report_templates_dir=str(reports or self.reports),
                               ai_contexts_dir=str(contexts or self.contexts))

    def test_missing_declared_resource_names_its_plugin_and_file(self) -> None:
        for resource, path in (
            ("view", self.views / "vendor-plugin.json"),
            ("adapter", self.adapters / "vendor-plugin.py"),
            ("report_template", self.reports / "vendor-plugin.py"),
            ("ai_context", self.contexts / "vendor-plugin.md"),
        ):
            with self.subTest(resource=resource):
                original = path.read_bytes()
                path.unlink()
                try:
                    with self.assertRaises(ResultPluginError) as raised:
                        self.load()
                    self.assertIn("vendor-plugin", str(raised.exception))
                    self.assertIn(resource, str(raised.exception))
                    self.assertIn(str(path), str(raised.exception))
                finally:
                    path.write_bytes(original)

    def test_manifest_preserves_view_only_and_dynamic_external_plugins(self) -> None:
        extra = json.loads((self.views / "view-only.json").read_text())
        extra["plugin"] = "extra-vendor"
        (self.views / "extra-vendor.json").write_text(json.dumps(extra), encoding="utf-8")
        plugins = self.load()
        self.assertEqual({"vendor-plugin", "view-only", "extra-vendor"}, set(plugins))
        self.assertTrue(plugins["vendor-plugin"].configured)
        self.assertFalse(plugins["view-only"].configured)
        self.assertFalse(plugins["extra-vendor"].configured)
        self.assertEqual({"value": 7}, plugins["vendor-plugin"].summary(None, "run"))

    def test_checks_configured_directories_and_service_startup(self) -> None:
        with self.assertRaisesRegex(ResultPluginError, "missing declared adapter"):
            self.load(adapters=self.root / "wrong adapters")
        with self.assertRaisesRegex(ResultPluginError, "missing declared report_template"):
            ResultService(ServiceConfig(plugin_views_dir=str(self.views),
                                        plugin_adapters_dir=str(self.adapters),
                                        plugin_ai_contexts_dir=str(self.contexts),
                                        report_templates_dir=str(self.root / "wrong reports")))
        with self.assertRaisesRegex(ResultPluginError, "missing declared ai_context"):
            self.load(contexts=self.root / "wrong contexts")


class PluginViewConfigTests(unittest.TestCase):
    def test_relative_runtime_view_directory_is_resolved_from_config(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = Path(temp_dir) / "config" / "result_service.json"
            config_path.parent.mkdir()
            config_path.write_text(
                json.dumps({
                    "plugin_views_dir": "../data/result_views",
                    "plugin_adapters_dir": "../result_service/plugin_adapters",
                    "plugin_ai_contexts_dir": "../result_service/plugin_ai_contexts",
                }),
                encoding="utf-8",
            )
            with patch.dict(os.environ, {}, clear=True):
                config = ServiceConfig.from_file(config_path)

            self.assertEqual(
                str((Path(temp_dir) / "data" / "result_views").resolve()),
                config.plugin_views_dir,
            )
            self.assertEqual(
                str((Path(temp_dir) / "result_service" / "plugin_adapters").resolve()),
                config.plugin_adapters_dir,
            )
            self.assertEqual(
                str((Path(temp_dir) / "result_service" / "plugin_ai_contexts").resolve()),
                config.plugin_ai_contexts_dir,
            )

    def test_plugin_specific_adapter_options_are_generic(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = Path(temp_dir) / "result_service.json"
            config_path.write_text(json.dumps({
                "plugin_options": {
                    "stpcompare": {"result_table": "custom_stp_results"},
                    "vendor": {"mode": "summary"},
                },
            }), encoding="utf-8")

            with patch.dict(os.environ, {}, clear=True):
                config = ServiceConfig.from_file(config_path)

            self.assertEqual("custom_stp_results", config.plugin_options["stpcompare"]["result_table"])
            self.assertEqual("summary", config.plugin_options["vendor"]["mode"])


if __name__ == "__main__":
    unittest.main()
