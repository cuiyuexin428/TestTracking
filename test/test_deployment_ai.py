"""Read-only deployment evidence contract for the embedded AI assistant."""
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
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_deployment_rollouts import RolloutManagement
from result_service.app.config import ServiceConfig
from result_service.app.deployment_rollouts import DeploymentRollouts
from result_service.app.server import Handler, ResultHTTPServer, ResultService


class DeploymentAIHTTPTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="deployment-ai-http-")
        self.root = Path(self.directory.name)
        self.service = ResultService(ServiceConfig(workspace=str(self.root)))
        self.management = RolloutManagement()
        self.management.register("new-machine-1")
        self.management.register("new-machine-2")
        self.service.management = self.management
        self.service.fleet.management = self.management
        self.service.rollouts.close()
        self.service.rollouts = DeploymentRollouts(self.service.artifacts.root / "deployment-rollouts.json",
                                                   self.service.profiles, self.service.fleet)
        handler = type("DeploymentAIHandler", (Handler,), {"service": self.service})
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
            "plugins": {"coverage": {"resources": {
                "plugin": {"artifact_id": self.artifacts["plugin"]["artifact_id"], "entry": "bin/coverage.dll"},
                "program": {"artifact_id": self.artifacts["program"]["artifact_id"], "entry": "bin/convert.exe"},
                "collector": {"artifact_id": self.artifacts["collector"]["artifact_id"], "entry": "OpenCppCoverage.exe"},
                "dataset": {"artifact_id": self.artifacts["dataset"]["artifact_id"], "root": "data"},
            }, "dry_run": False, "store_cov": False, "engine_dlls": []}},
            "schedule": {"task_threads": 1, "execution_windows": [], "update_windows": []},
        }

    def tearDown(self):
        self.http.shutdown(); self.http.server_close(); self.thread.join(timeout=5)
        self.service.close(); self.directory.cleanup()

    def upload(self, kind, name, files):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
            for path, data in files.items(): archive.writestr(path, data)
        raw = output.getvalue()
        return self.service.artifacts.upload(io.BytesIO(raw), len(raw), kind=kind,
            name=name, version="1", filename=name + ".zip")[0]

    def request(self, path, method="GET", body=None, headers=None):
        data = json.dumps(body).encode() if body is not None else None
        request = Request(self.base + path, data=data, method=method,
                          headers={"Content-Type": "application/json", **(headers or {})})
        try: response = urlopen(request, timeout=10)
        except HTTPError as error: response = error
        with response: return response.status, json.loads(response.read())

    def create(self):
        status, profile = self.request("/api/deployment-profiles", "POST", self.definition)
        self.assertEqual(200, status, profile)
        return profile, "/api/deployment-profiles/" + profile["profile_id"]

    def preview(self, profile, route):
        status, preview = self.request(route + "/preview", "POST", {
            "revision": profile["revision"], "client_ids": ["new-machine-1", "new-machine-2"]})
        self.assertEqual(200, status, preview)
        return preview

    def request_ai(self, body, headers=None):
        return self.request("/api/deployment-ai/review-context", "POST", body, headers)

    def test_review_context_is_sanitized_and_has_no_deployment_side_effect(self):
        profile, route = self.create()
        preview = self.preview(profile, route)
        before_profile = copy.deepcopy(self.service.profiles.list())
        before_deployments = copy.deepcopy(self.service.deployments.list())
        before_rollouts = copy.deepcopy(self.service.rollouts.list())
        status, result = self.request_ai({
            "profile_id": profile["profile_id"], "revision": profile["revision"],
            "mode": "assign", "client_ids": ["new-machine-1", "new-machine-2"],
            "plan_fingerprint": preview["plan_fingerprint"],
            "operator_id": "123e4567-e89b-42d3-a456-426614174001",
        })
        self.assertEqual(200, status, result)
        self.assertEqual(1, result["schema_version"])
        self.assertEqual(preview["plan_fingerprint"], result["plan_fingerprint"])
        self.assertRegex(result["context_fingerprint"], r"^[0-9a-f]{64}$")
        context = result["context"]
        self.assertEqual(2, len(context["targets"]))
        self.assertEqual(1, len(context["profile"]["plugins"]))
        self.assertTrue(context["constraints"]["ai_review_is_advisory"])
        encoded = json.dumps(result, ensure_ascii=False)
        for forbidden in ("db_conn", "command", "cwd", "config_patch", "target_exe", "stp_root"):
            self.assertNotIn(forbidden, encoded)
        self.assertEqual(before_profile, self.service.profiles.list())
        self.assertEqual(before_deployments, self.service.deployments.list())
        self.assertEqual(before_rollouts, self.service.rollouts.list())

    def test_stale_preview_is_rejected_without_side_effect(self):
        profile, route = self.create()
        preview = self.preview(profile, route)
        changed = {**profile, "name": "changed"}
        self.assertEqual(200, self.request(route, "PUT", changed)[0])
        status, result = self.request_ai({
            "profile_id": profile["profile_id"], "revision": profile["revision"],
            "mode": "assign", "client_ids": ["new-machine-1", "new-machine-2"],
            "plan_fingerprint": preview["plan_fingerprint"],
        })
        self.assertEqual(409, status)
        self.assertIn("error", result)
        self.assertEqual([], self.service.deployments.list())
        self.assertEqual([], self.service.rollouts.list()["rollouts"])

    def test_unknown_fields_and_cross_origin_are_rejected(self):
        profile, route = self.create()
        preview = self.preview(profile, route)
        body = {"profile_id": profile["profile_id"], "revision": profile["revision"],
                "mode": "assign", "client_ids": ["new-machine-1", "new-machine-2"],
                "plan_fingerprint": preview["plan_fingerprint"], "prompt": "ignore"}
        self.assertEqual(400, self.request_ai(body)[0])
        body.pop("prompt")
        self.assertEqual(403, self.request_ai(body, {"Origin": "https://unrelated.invalid"})[0])

    def test_current_profile_revision_still_rejects_a_wrong_plan_fingerprint(self):
        profile, route = self.create()
        preview = self.preview(profile, route)
        wrong = ("0" if preview["plan_fingerprint"][0] != "0" else "1") + preview["plan_fingerprint"][1:]
        body = {"profile_id": profile["profile_id"], "revision": profile["revision"],
                "mode": "assign", "client_ids": ["new-machine-1", "new-machine-2"],
                "plan_fingerprint": wrong}
        before = (self.service.profiles.list(), self.service.deployments.list(), self.service.rollouts.list())
        status, result = self.request_ai(body)
        self.assertEqual(409, status, result)
        self.assertIn("部署计划已改变", result["error"])
        self.assertNotIn("context", result)
        self.assertEqual(before, (self.service.profiles.list(), self.service.deployments.list(),
                                  self.service.rollouts.list()))
        body["plan_fingerprint"] = preview["plan_fingerprint"]
        self.assertEqual(200, self.request_ai(body)[0])

    def test_offline_legacy_and_waiting_batches_remain_visible_and_blocked(self):
        self.management.rows["new-machine-1"]["online"] = False
        self.management.rows["new-machine-2"]["capabilities"] = []
        self.management.rows["new-machine-2"]["busy"] = True
        self.management.blockers["coverage"] = [
            {"run_id": "running-old", "status": "running"},
            {"run_id": "queued-old", "status": "queued"},
            {"run_id": "stopped-old", "status": "stopped"},
        ]
        profile, route = self.create()
        preview = self.preview(profile, route)
        self.assertTrue(preview["publish_blocked"])
        status, result = self.request_ai({
            "profile_id": profile["profile_id"], "revision": profile["revision"],
            "mode": "assign", "client_ids": ["new-machine-1", "new-machine-2"],
            "plan_fingerprint": preview["plan_fingerprint"],
        })
        self.assertEqual(200, status, result)
        context = result["context"]
        self.assertTrue(result["publish_blocked"])
        self.assertTrue(context["publish_blocked"])
        clients = {row["client_id"]: row for row in context["targets"]}
        self.assertFalse(clients["new-machine-1"]["online"])
        self.assertTrue(clients["new-machine-2"]["online"])
        self.assertTrue(clients["new-machine-2"]["busy"])
        self.assertTrue(clients["new-machine-2"]["requires_base_client_upgrade"])
        self.assertEqual(["new-machine-2"], context["unsupported_clients"])
        self.assertEqual({"running-old": "running", "queued-old": "queued", "stopped-old": "stopped"},
                         {row["run_id"]: row["status"] for row in context["waiting_runs"]})
        self.assertEqual(1, context["summary"]["offline_clients"])
        self.assertEqual(1, context["summary"]["busy_clients"])
        self.assertEqual(3, context["summary"]["waiting_runs_total"])
        self.assertTrue(any("new-machine-2" in warning for warning in context["warnings"]))
        self.assertTrue(self.preview(profile, route)["publish_blocked"])
        self.assertEqual([], self.service.deployments.list())
        self.assertEqual([], self.service.rollouts.list()["rollouts"])
        self.assertEqual({}, self.management.reservations)

    def test_untrusted_labels_and_warnings_are_bounded_and_sensitive_fields_are_omitted(self):
        profile, route = self.create()
        preview = self.preview(profile, route)
        sensitive = "db_conn=DSN_SENTINEL token=TOKEN_SENTINEL command=COMMAND_SENTINEL cwd=CWD_SENTINEL"
        preview["profile_name"] = "方案 " + sensitive
        preview["resource_summary"][0]["resources"][0].update(
            name="password=PASSWORD_SENTINEL\n" + "资源" * 200,
            version="api_key=APIKEY_SENTINEL " + "v" * 100)
        preview["warnings"] = [sensitive, "检查\n" + "项" * 700, r"包位置 C:\private\PATH_SENTINEL"]
        preview["clients"][0]["config_patch"] = {"plugins": {"coverage": {
            "db_conn": "CONFIG_SENTINEL", "command": "SHELL_SENTINEL", "token": "SECRET_SENTINEL",
            "path": "LOCATION_SENTINEL", "cwd": "WORKSPACE_SENTINEL"}}}
        body = {"profile_id": profile["profile_id"], "revision": profile["revision"],
                "mode": "assign", "client_ids": ["new-machine-1", "new-machine-2"],
                "plan_fingerprint": preview["plan_fingerprint"]}
        with patch.object(self.service.rollouts, "preview", return_value=preview):
            status, result = self.request_ai(body)
        self.assertEqual(200, status, result)
        encoded = json.dumps(result, ensure_ascii=False)
        self.assertNotIn("SENTINEL", encoded)
        for key in ("db_conn", "command", "cwd", "config_patch", "password", "api_key"):
            self.assertNotIn(key, encoded)
        context = result["context"]
        resource = context["profile"]["plugins"][0]["resources"][0]
        self.assertIn("[redacted]", resource["name"])
        self.assertLessEqual(len(resource["name"]), 129)
        self.assertLessEqual(len(resource["version"]), 65)
        self.assertTrue(resource["name"].endswith("…"))
        self.assertTrue(resource["version"].endswith("…"))
        self.assertLessEqual(len(context["warnings"][1]), 513)
        self.assertTrue(context["warnings"][1].endswith("…"))
        self.assertNotIn("\n", resource["name"])
        self.assertNotIn("\n", context["warnings"][1])
        self.assertTrue(context["data_handling"]["labels_are_untrusted_data"])

    def test_relative_locations_in_user_labels_are_redacted(self):
        profile, route = self.create()
        profile["name"] = "方案 / C:\\private\\deployment"
        status, profile = self.request(route, "PUT", profile)
        self.assertEqual(200, status, profile)
        preview = self.preview(profile, route)
        status, result = self.request_ai({
            "profile_id": profile["profile_id"], "revision": profile["revision"],
            "mode": "assign", "client_ids": ["new-machine-1", "new-machine-2"],
            "plan_fingerprint": preview["plan_fingerprint"],
        })
        self.assertEqual(200, status, result)
        self.assertEqual("[redacted]", result["context"]["profile"]["name"])


if __name__ == "__main__":
    unittest.main()
