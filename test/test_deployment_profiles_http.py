"""Saved deployment recipes retain scheduler and fleet concurrency guarantees."""
from __future__ import annotations

import copy
import io
import json
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_deployment_rollouts import RolloutManagement
from result_service.app.deployment_rollouts import DeploymentRollouts
from result_service.app.config import ServiceConfig
from result_service.app.management import ManagementError
from result_service.app.server import Handler, ResultHTTPServer, ResultService


class ProfileManagement(RolloutManagement):
    pass


class DeploymentProfilesHTTPTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="deployment-profiles-http-")
        self.root = Path(self.directory.name)
        self.service = ResultService(ServiceConfig(workspace=str(self.root)))
        self.management = ProfileManagement()
        self.management.register("new-machine-1")
        self.management.register("new-machine-2")
        self.service.management = self.management
        self.service.fleet.management = self.management
        self.service.rollouts.close()
        self.service.rollouts = DeploymentRollouts(self.service.artifacts.root / "deployment-rollouts.json",
                                                   self.service.profiles, self.service.fleet)
        handler = type("ProfileHandler", (Handler,), {"service": self.service})
        self.http = ResultHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:" + str(self.http.server_port)
        self.artifacts = {
            "plugin": self.upload("plugin", "coverage", {"bin/coverage.dll": b"plugin"}),
            "program": self.upload("program", "convert", {"bin/convert.exe": b"program"}),
            "collector": self.upload("program", "collector", {"OpenCppCoverage.exe": b"collector"}),
            "dataset": self.upload("dataset", "parts", {"data/part.stp": b"STP content"}),
        }
        self.definition = {
            "revision": None, "name": "标准覆盖率方案", "description": "新机器首次部署",
            "plugins": {"coverage": {
                "resources": {
                    "plugin": {"artifact_id": self.artifacts["plugin"]["artifact_id"], "entry": "bin/coverage.dll"},
                    "program": {"artifact_id": self.artifacts["program"]["artifact_id"], "entry": "bin/convert.exe"},
                    "collector": {"artifact_id": self.artifacts["collector"]["artifact_id"], "entry": "OpenCppCoverage.exe"},
                    "dataset": {"artifact_id": self.artifacts["dataset"]["artifact_id"], "root": "data"},
                }, "dry_run": False, "store_cov": False, "engine_dlls": [],
            }},
            "schedule": {"task_threads": 1, "execution_windows": [], "update_windows": []},
        }

    def tearDown(self):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join(timeout=5)
        self.service.close()
        self.directory.cleanup()

    def upload(self, kind, name, files):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
            for path, data in files.items():
                archive.writestr(path, data)
        raw = output.getvalue()
        return self.service.artifacts.upload(io.BytesIO(raw), len(raw), kind=kind,
            name=name, version="1", filename=name + ".zip")[0]

    def request(self, path, method="GET", body=None, headers=None):
        data = json.dumps(body).encode() if body is not None else None
        request = Request(self.base + path, data=data, method=method,
                          headers={"Content-Type": "application/json", **(headers or {})})
        try:
            response = urlopen(request, timeout=10)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.loads(response.read())

    def create(self):
        status, profile = self.request("/api/deployment-profiles", "POST", self.definition)
        self.assertEqual(200, status, profile)
        return profile, "/api/deployment-profiles/" + profile["profile_id"]

    def preview(self, profile, route):
        status, preview = self.request(route + "/preview", "POST", {
            "revision": profile["revision"], "client_ids": ["new-machine-1", "new-machine-2"]})
        self.assertEqual(200, status, preview)
        return preview

    def publication(self, profile, preview):
        return {"revision": profile["revision"], "mode": "assign", "request_id": "http-request",
                "client_ids": ["new-machine-1", "new-machine-2"],
                "plan_fingerprint": preview["plan_fingerprint"]}

    def test_blank_machines_use_saved_recipe_and_automatically_prepared_dataset(self):
        artifact = self.artifacts["program"]
        status, contents = self.request("/api/artifacts/" + artifact["artifact_id"] + "/resources")
        self.assertEqual(200, status)
        self.assertIn("bin/convert.exe", contents["executables"])
        profile, route = self.create()
        self.assertEqual(profile, self.request(route)[1])
        preview = self.preview(profile, route)
        self.assertEqual(2, len(preview["clients"]))
        self.assertFalse(preview["publish_blocked"])
        for client in preview["clients"]:
            plugin = client["config_patch"]["plugins"]["coverage"]
            self.assertTrue(plugin["target_exe"].startswith("{artifact:"))
            self.assertTrue(plugin["stp_root"].startswith("{artifact:"))
            self.assertEqual(preview["target_ids"]["coverage"], plugin["deployment_target_id"])
        self.assertEqual(409, self.request(route + "/bind-scheduler", "POST", {})[0])
        payload = self.publication(profile, preview)
        status, published = self.request(route + "/publish", "POST", payload)
        self.assertEqual(200, status, published)
        self.assertEqual("waiting_runs", published["status"])
        self.assertEqual([], self.service.deployments.list())
        self.assertEqual({}, self.management.reservations)
        self.assertEqual(published["rollout_id"], self.request(route + "/publish", "POST", payload)[1]["rollout_id"])
        self.service.rollouts.tick()
        self.assertEqual(2, len(self.service.deployments.list()))
        expected = self.management.configuration["plugins"]["coverage"]
        lines = Path(expected["stp_manifest"]).read_text(encoding="utf-8").splitlines()
        self.assertEqual("part.stp", json.loads(lines[0])["path"])
        self.assertTrue(expected["expected_dataset_version"].startswith("sha256:"))
        self.assertEqual(1, len(self.request("/api/deployment-rollouts")[1]["rollouts"]))
        value = self.request("/api/deployment-rollouts/" + published["rollout_id"])[1]
        self.assertEqual("deploying", value["status"])
        self.assertEqual(preview["resource_summary"], value["resource_summary"])

    def test_edited_recipe_rejects_old_preview_without_publishing(self):
        profile, route = self.create()
        preview = self.preview(profile, route)
        updated = {**profile, "name": "另一个名称"}
        status, saved = self.request(route, "PUT", updated)
        self.assertEqual(200, status, saved)
        self.assertNotEqual(profile["revision"], saved["revision"])
        status, conflict = self.request(route + "/publish", "POST", self.publication(profile, preview))
        self.assertEqual(409, status)
        self.assertEqual(saved["revision"], conflict["current_revision"])
        self.assertEqual([], self.service.deployments.list())
        self.assertEqual({}, self.management.reservations)

    def test_stale_machine_revision_refuses_entire_batch(self):
        profile, route = self.create()
        preview = self.preview(profile, route)
        self.service.deployments.put_desired("new-machine-1", {
            "revision": None, "components": [], "config_patch": {"client": {"task_threads": 3}}})
        status, result = self.request(route + "/publish", "POST", self.publication(profile, preview))
        self.assertEqual(409, status, result)
        self.assertIsNone(self.service.deployments.get("new-machine-2")["desired"])
        self.assertEqual({}, self.management.reservations)

    def test_cross_origin_recipe_write_is_rejected(self):
        status, _ = self.request("/api/deployment-profiles", "POST", self.definition,
                                 {"Origin": "https://unrelated.invalid"})
        self.assertEqual(403, status)
        self.assertEqual([], self.service.profiles.list())

    def test_template_defaults_changed_after_preview_require_new_confirmation(self):
        template = self.root / "client-template.json"
        self.management.config.client_config_path = str(template)
        template.write_text(json.dumps({"plugins": {"coverage": {
            "db_conn": "host=db dbname=previewed", "store_type": "postgres"}}}), encoding="utf-8")
        profile, route = self.create()
        preview = self.preview(profile, route)
        publication = self.publication(profile, preview)
        template.write_text(json.dumps({"plugins": {"coverage": {
            "db_conn": "host=db dbname=changed", "store_type": "postgres"}}}), encoding="utf-8")
        self.assertEqual(409, self.request(route + "/publish", "POST", publication)[0])
        self.assertEqual([], self.service.deployments.list())
        self.assertEqual({}, self.management.reservations)
        publication["plan_fingerprint"] = self.preview(profile, route)["plan_fingerprint"]
        code, published = self.request(route + "/publish", "POST", publication)
        self.assertEqual(200, code, published)
        self.service.rollouts.tick()
        self.assertTrue(all(row["desired"]["config_patch"]["plugins"]["coverage"]["db_conn"]
                            == "host=db dbname=changed" for row in self.service.deployments.list()))

    def test_batch_http_also_rejects_an_unreviewed_template(self):
        template = self.root / "batch-template.json"
        self.management.config.client_config_path = str(template)
        template.write_text(json.dumps({"plugins": {"coverage": {"db_conn": "host=db dbname=first"}}}), encoding="utf-8")
        profile, _ = self.create()
        prepared = self.service.profiles.compile(profile)
        proposal = {key: prepared[key] for key in ("components", "config_patch")}
        code, preview = self.request("/api/deployments/batch/preview", "POST",
            {**proposal, "client_ids": ["new-machine-1", "new-machine-2"]})
        self.assertEqual(200, code, preview)
        publication = {**proposal, "plan_fingerprint": preview["plan_fingerprint"], "targets": [
            {"client_id": row["client_id"], "revision": row["revision"]} for row in preview["clients"]]}
        template.write_text(json.dumps({"plugins": {"coverage": {"db_conn": "host=db dbname=second"}}}), encoding="utf-8")
        self.assertEqual(409, self.request("/api/deployments/batch", "POST", publication)[0])
        self.assertEqual({}, self.management.reservations)
        self.assertEqual([], self.service.deployments.list())
        fresh = self.request("/api/deployments/batch/preview", "POST",
            {**proposal, "client_ids": ["new-machine-1", "new-machine-2"]})[1]
        publication["plan_fingerprint"] = fresh["plan_fingerprint"]
        self.assertEqual(200, self.request("/api/deployments/batch", "POST", publication)[0])


if __name__ == "__main__":
    unittest.main()
