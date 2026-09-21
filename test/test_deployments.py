from __future__ import annotations

import io
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from result_service.app.artifacts import ArtifactStore
from result_service.app.config import ServiceConfig
from result_service.app.deployments import DeploymentError, DeploymentStore
from result_service.app.server import Handler, ResultHTTPServer, ResultService
from fleet_fixture import FleetManagement


class DeploymentStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="deployment-store-")
        self.root = Path(self.temporary.name)
        self.artifacts = ArtifactStore(self.root / "artifacts")
        self.store = DeploymentStore(self.root / "artifacts" / "deployments.json", self.artifacts)
        self.plugin = self.upload("plugin", "coverage", "1", b"plugin")
        self.program = self.upload("program", "coverage", "2", b"program")
        self.dataset = self.upload("dataset", "stp", "d1", b"dataset")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def upload(self, kind: str, name: str, version: str, content: bytes) -> dict:
        value, _ = self.artifacts.upload(
            io.BytesIO(content), len(content), kind=kind, name=name, version=version,
            filename=f"{name}-{version}.zip",
        )
        return value

    def plan(self, *artifacts: tuple[str, dict]) -> list[dict[str, str]]:
        return [{"slot": slot, "artifact_id": artifact["artifact_id"]}
                for slot, artifact in artifacts]

    def test_desired_and_reported_state_survive_reopen(self) -> None:
        plan = self.plan(("dataset", self.dataset), ("plugin.coverage", self.plugin),
                         ("program.coverage", self.program))
        desired, changed = self.store.put_desired(
            "worker-1", {"revision": None, "components": list(reversed(plan)),
                         "operator_id": "operator-1"}
        )

        self.assertTrue(changed)
        self.assertEqual("pending", desired["state"])
        self.assertEqual(["dataset", "plugin.coverage", "program.coverage"],
                         [item["slot"] for item in desired["desired"]["components"]])
        revision = desired["desired"]["revision"]
        reported, changed = self.store.put_reported("worker-1", {
            "desired_revision": revision,
            "state": "ready",
            "components": [{**item, "state": "active"} for item in plan],
            "agent_version": "1.2.0",
        })

        self.assertTrue(changed)
        self.assertEqual("current", reported["state"])
        reopened = DeploymentStore(self.store.path, self.artifacts).get("worker-1")
        self.assertEqual(reported, reopened)
        self.assertEqual(self.program["sha256"],
                         reopened["reported"]["components"][2]["artifact"]["sha256"])

    def test_desired_revision_prevents_lost_updates_and_is_idempotent(self) -> None:
        initial, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": self.plan(("plugin", self.plugin)),
        })
        revision = initial["desired"]["revision"]
        same, changed = self.store.put_desired("worker-1", {
            "revision": None, "components": self.plan(("plugin", self.plugin)),
        })
        self.assertFalse(changed)
        self.assertEqual(revision, same["desired"]["revision"])

        updated, _ = self.store.put_desired("worker-1", {
            "revision": revision, "components": self.plan(("program", self.program)),
        })
        with self.assertRaisesRegex(DeploymentError, "changed") as raised:
            self.store.put_desired("worker-1", {
                "revision": revision, "components": self.plan(("dataset", self.dataset)),
            })
        self.assertEqual(409, raised.exception.status)
        self.assertEqual(updated["desired"]["revision"], raised.exception.current_revision)

    def test_concurrent_desired_writers_cannot_overwrite_each_other(self) -> None:
        initial, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": self.plan(("plugin", self.plugin)),
        })
        expected = initial["desired"]["revision"]
        outcomes: list[int] = []
        barrier = threading.Barrier(3)

        def update(slot: str, artifact: dict) -> None:
            barrier.wait()
            try:
                self.store.put_desired("worker-1", {
                    "revision": expected, "components": self.plan((slot, artifact)),
                })
                outcomes.append(200)
            except DeploymentError as exc:
                outcomes.append(exc.status)

        threads = [threading.Thread(target=update, args=("program", self.program)),
                   threading.Thread(target=update, args=("dataset", self.dataset))]
        for thread in threads:
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join()
        self.assertEqual([200, 409], sorted(outcomes))

    def test_stale_report_is_retained_but_marks_new_desired_state_pending(self) -> None:
        first, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": self.plan(("plugin", self.plugin)),
        })
        revision = first["desired"]["revision"]
        self.store.put_reported("worker-1", {
            "desired_revision": revision, "state": "ready",
            "components": [{"slot": "plugin", "artifact_id": self.plugin["artifact_id"],
                            "state": "active"}],
        })
        changed, _ = self.store.put_desired("worker-1", {
            "revision": revision, "components": self.plan(("program", self.program)),
        })

        self.assertEqual("pending", changed["state"])
        self.assertEqual(revision, changed["reported"]["desired_revision"])

    def test_returning_to_an_old_plan_creates_a_new_generation(self) -> None:
        first, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": self.plan(("plugin", self.plugin)),
        })
        second, _ = self.store.put_desired("worker-1", {
            "revision": first["desired"]["revision"],
            "components": self.plan(("program", self.program)),
        })
        third, _ = self.store.put_desired("worker-1", {
            "revision": second["desired"]["revision"],
            "components": self.plan(("plugin", self.plugin)),
        })

        self.assertNotEqual(first["desired"]["revision"], third["desired"]["revision"])
        with self.assertRaisesRegex(DeploymentError, "no longer current") as raised:
            self.store.put_reported("worker-1", {
                "desired_revision": first["desired"]["revision"], "state": "ready",
                "components": [{"slot": "plugin", "artifact_id": self.plugin["artifact_id"],
                                "state": "active"}],
            })
        self.assertEqual(409, raised.exception.status)
        self.assertEqual(third["desired"]["revision"], raised.exception.current_revision)

    def test_invalid_artifact_and_inconsistent_ready_report_are_rejected(self) -> None:
        with self.assertRaisesRegex(DeploymentError, "artifact not found") as missing:
            self.store.put_desired("worker-1", {
                "revision": None,
                "components": [{"slot": "plugin", "artifact_id": "0" * 32}],
            })
        self.assertEqual(404, missing.exception.status)

        desired, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": self.plan(("plugin", self.plugin)),
        })
        with self.assertRaisesRegex(DeploymentError, "must be active"):
            self.store.put_reported("worker-1", {
                "desired_revision": desired["desired"]["revision"], "state": "ready",
                "components": [{"slot": "plugin", "artifact_id": self.plugin["artifact_id"],
                                "state": "staged"}],
            })

    def test_config_patch_changes_revision_and_preserves_arbitrary_plugin_fields(self) -> None:
        components = self.plan(("executor", self.program), ("inputs", self.dataset))
        patch = {"plugins": {"custom-plugin": {
            "tool": "{artifact:executor}/bin/worker.exe", "input": "{artifact:inputs}",
            "parameters": ["custom", {"output": "D:\\results"}],
        }}, "client": {"task_threads": 2}}
        first, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": components, "config_patch": patch,
            "reason": "initial deployment",
        })
        self.assertEqual(patch, first["desired"]["config_patch"])
        self.assertEqual("initial deployment", first["desired"]["reason"])
        patch["client"]["task_threads"] = 3
        changed, _ = self.store.put_desired("worker-1", {
            "revision": first["desired"]["revision"], "components": components,
            "config_patch": patch,
        })
        self.assertNotEqual(first["desired"]["revision"], changed["desired"]["revision"])
        self.assertEqual(2, changed["history"][0]["config_patch"]["client"]["task_threads"])
        same, written = self.store.put_desired("worker-1", {
            "revision": changed["desired"]["revision"],
            "components": list(reversed(components)), "config_patch": patch,
        })
        self.assertFalse(written)
        self.assertEqual(changed["desired"]["revision"], same["desired"]["revision"])
        self.assertEqual(changed, DeploymentStore(self.store.path, self.artifacts).get("worker-1"))

    def test_patch_protects_client_identity_and_requires_existing_artifact_slots(self) -> None:
        base = {"revision": None, "components": self.plan(("executor", self.program))}
        for patch in ({"client": {"worker_id": "new-id"}},
                      {"client": {"deployment_endpoint": "http://other-server"}},
                      {"client": None}):
            with self.subTest(patch=patch), self.assertRaisesRegex(DeploymentError, "client"):
                self.store.put_desired("worker-1", {**base, "config_patch": patch})
        with self.assertRaisesRegex(DeploymentError, "unknown artifact slot"):
            self.store.put_desired("worker-1", {
                **base, "config_patch": {"plugins": {"custom": {
                    "arguments": ["input", "{artifact:missing}/input.stp"],
                }}},
            })
        with self.assertRaisesRegex(DeploymentError, "inside its artifact"):
            self.store.put_desired("worker-1", {
                **base, "config_patch": {"tool": "{artifact:executor}/../other.exe"},
            })
        self.assertEqual([], self.store.list())

    def test_config_patch_json_types_participate_in_revision(self) -> None:
        components = self.plan(("plugin", self.plugin))
        first, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": components,
            "config_patch": {"plugins": {"custom": {"value": True}}},
        })
        second, changed = self.store.put_desired("worker-1", {
            "revision": first["desired"]["revision"], "components": components,
            "config_patch": {"plugins": {"custom": {"value": 1}}},
        })
        self.assertTrue(changed)
        self.assertNotEqual(first["desired"]["revision"], second["desired"]["revision"])

    def test_patch_accepts_multiple_enabled_plugins_with_revision(self) -> None:
        components = self.plan(("plugin", self.plugin))
        original, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": components,
            "config_patch": {"plugins": {"coverage": {"enabled": True}}},
        })
        saved, changed = self.store.put_desired("worker-1", {
            "revision": original["desired"]["revision"], "components": components,
            "config_patch": {"plugins": {
                "coverage": {"enabled": True}, "stpcompare": {"enabled": True},
            }},
        })
        self.assertTrue(changed)
        self.assertTrue(saved["desired"]["config_patch"]["plugins"]["coverage"]["enabled"])
        self.assertTrue(saved["desired"]["config_patch"]["plugins"]["stpcompare"]["enabled"])

    def test_partial_patch_leaves_final_enabled_count_to_client(self) -> None:
        patches = [
            {"client": {"task_threads": 2}},
            {"plugins": {"coverage": {"enabled": False}}},
            {"plugins": {"stpcompare": {"enabled": True}}},
            {"plugins": {"coverage": {"enabled": False}, "stpcompare": {"enabled": True}}},
        ]
        for index, patch in enumerate(patches):
            with self.subTest(patch=patch):
                desired, changed = self.store.put_desired(f"worker-{index}", {
                    "revision": None, "components": [], "config_patch": patch,
                })
                self.assertTrue(changed)
                self.assertEqual(patch, desired["desired"]["config_patch"])

    def test_rollback_republishes_saved_plan_and_patch_with_new_revision(self) -> None:
        first, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": self.plan(("executor", self.program)),
            "config_patch": {"client": {"task_threads": 1}},
        })
        second, _ = self.store.put_desired("worker-1", {
            "revision": first["desired"]["revision"],
            "components": self.plan(("inputs", self.dataset)),
            "config_patch": {"client": {"task_threads": 4}},
        })
        request = {"revision": second["desired"]["revision"], "action": "rollback",
                   "target_revision": first["desired"]["revision"],
                   "reason": "restore the verified deployment", "operator_id": "operator-1"}
        restored, changed = self.store.put_desired("worker-1", request)
        self.assertTrue(changed)
        self.assertEqual(first["desired"]["components"], restored["desired"]["components"])
        self.assertEqual(first["desired"]["config_patch"], restored["desired"]["config_patch"])
        self.assertNotEqual(first["desired"]["revision"], restored["desired"]["revision"])
        self.assertEqual(first["desired"]["revision"], restored["desired"]["rollback_of"])
        self.assertEqual("rollback", restored["desired"]["action"])
        self.assertEqual([second["desired"]["revision"], first["desired"]["revision"]],
                         [item["revision"] for item in restored["history"]])
        repeated, changed = self.store.put_desired("worker-1", request)
        self.assertFalse(changed)
        self.assertEqual(restored, repeated)
        with self.assertRaisesRegex(DeploymentError, "changed") as stale:
            self.store.put_desired("worker-1", {**request, "target_revision": second["desired"]["revision"]})
        self.assertEqual(409, stale.exception.status)

    def test_history_is_limited_to_ten_previous_deployments(self) -> None:
        revision = None
        for index in range(12):
            latest, _ = self.store.put_desired("worker-1", {
                "revision": revision, "components": self.plan(("plugin", self.plugin)),
                "config_patch": {"client": {"task_threads": index + 1}},
            })
            revision = latest["desired"]["revision"]
        self.assertEqual(10, len(latest["history"]))
        self.assertEqual([11, 10, 9, 8, 7, 6, 5, 4, 3, 2], [
            item["config_patch"]["client"]["task_threads"] for item in latest["history"]
        ])

    def test_waiting_and_staged_reports_still_require_the_full_component_plan(self) -> None:
        plan = self.plan(("executor", self.program), ("dataset", self.dataset))
        desired, _ = self.store.put_desired("worker-1", {
            "revision": None, "components": plan,
        })
        revision = desired["desired"]["revision"]
        for state in ("waiting_window", "waiting_idle", "staged"):
            with self.subTest(state=state):
                updated, _ = self.store.put_reported("worker-1", {
                    "desired_revision": revision, "state": state,
                    "components": [{**item, "state": "staged"} for item in plan],
                })
                self.assertEqual(state, updated["state"])
        with self.assertRaisesRegex(DeploymentError, "do not match"):
            self.store.put_reported("worker-1", {
                "desired_revision": revision, "state": "waiting_window",
                "components": [{**plan[0], "state": "pending"}],
            })


class DeploymentHTTPTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="deployment-http-")
        root = Path(self.temporary.name)
        self.service = ResultService(ServiceConfig(
            workspace=str(root), artifact_store_dir=str(root / "artifacts"), providers={},
        ))
        self.control = FleetManagement()
        self.control.register("worker-1")
        self.service.fleet.management = self.control
        content = b"coverage plugin"
        self.artifact, _ = self.service.artifacts.upload(
            io.BytesIO(content), len(content), kind="plugin", name="coverage", version="1",
            filename="coverage.zip",
        )
        handler = type("DeploymentHandler", (Handler,), {
            "service": self.service, "log_message": lambda *args: None,
        })
        self.server = ResultHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.thread.join(5)
        self.server.server_close()
        self.service.close()
        self.temporary.cleanup()

    def request(self, path: str, method: str = "GET", value: dict | None = None):
        data = json.dumps(value).encode() if value is not None else None
        request = Request(self.base + path, method=method, data=data,
                          headers={"Content-Type": "application/json"} if data else {})
        try:
            response = urlopen(request, timeout=5)
        except HTTPError as exc:
            response = exc
        with response:
            return response.status, json.loads(response.read())

    def test_desired_poll_report_and_list_api(self) -> None:
        desired_status, desired = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": None,
            "components": [{"slot": "plugin.coverage",
                            "artifact_id": self.artifact["artifact_id"]}],
            "operator_id": "operator-1",
        })
        revision = desired["desired"]["revision"]
        self.assertEqual(200, desired_status)
        self.assertEqual("pending", desired["state"])
        self.assertEqual(self.artifact["download_url"],
                         desired["desired"]["components"][0]["artifact"]["download_url"])

        detail_status, detail = self.request("/api/deployments/worker-1")
        self.assertEqual((200, desired), (detail_status, detail))
        report_status, reported = self.request("/api/deployments/worker-1/reported", "PUT", {
            "desired_revision": revision, "state": "ready",
            "components": [{"slot": "plugin.coverage",
                            "artifact_id": self.artifact["artifact_id"], "state": "active"}],
        })
        self.assertEqual(200, report_status)
        self.assertEqual("current", reported["state"])
        self.assertEqual([reported], self.request("/api/deployments")[1]["clients"])

    def test_api_returns_current_revision_on_conflict(self) -> None:
        _, desired = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": None,
            "components": [{"slot": "plugin", "artifact_id": self.artifact["artifact_id"]}],
        })
        status, conflict = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": "0" * 64, "components": [],
        })
        self.assertEqual(409, status)
        self.assertEqual(desired["desired"]["revision"], conflict["current_revision"])

    def test_api_accepts_patch_explicitly_enabling_multiple_plugins(self) -> None:
        status, response = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": None, "components": [], "config_patch": {"plugins": {
                "coverage": {"enabled": True}, "stpcompare": {"enabled": True},
            }},
        })
        self.assertEqual(200, status)
        self.assertTrue(response["desired"]["config_patch"]["plugins"]["coverage"]["enabled"])
        self.assertTrue(response["desired"]["config_patch"]["plugins"]["stpcompare"]["enabled"])

    def test_config_patch_and_rollback_use_the_desired_endpoint(self) -> None:
        components = [{"slot": "worker-plugin", "artifact_id": self.artifact["artifact_id"]}]
        status, first = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": None, "components": components,
            "config_patch": {"plugins": {"custom": {
                "dll": "{artifact:worker-plugin}/custom.dll", "enabled": True,
            }}},
        })
        self.assertEqual(200, status)
        self.service.fleet.report("worker-1", {"desired_revision": first["desired"]["revision"],
            "state": "ready", "components": [{**item, "state": "active"} for item in components]})
        _, second = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": first["desired"]["revision"], "components": components,
            "config_patch": {"plugins": {"custom": {"enabled": False}}},
        })
        self.service.fleet.report("worker-1", {"desired_revision": second["desired"]["revision"],
            "state": "ready", "components": [{**item, "state": "active"} for item in components]})
        status, rollback = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": second["desired"]["revision"], "action": "rollback",
            "target_revision": first["desired"]["revision"], "reason": "restore plugin",
        })
        self.assertEqual(200, status)
        self.assertEqual(first["desired"]["config_patch"], rollback["desired"]["config_patch"])
        self.assertEqual(2, len(self.request("/api/deployments/worker-1")[1]["history"]))
        status, _ = self.request("/api/deployments/worker-1/desired", "PUT", {
            "revision": rollback["desired"]["revision"], "components": components,
            "config_patch": {"client": {"worker_id": "replacement"}},
        })
        self.assertEqual(400, status)


if __name__ == "__main__":
    unittest.main()
