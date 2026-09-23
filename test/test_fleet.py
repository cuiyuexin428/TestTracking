from __future__ import annotations

import io
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fleet_fixture import FleetManagement
from result_service.app.artifacts import ArtifactStore
from result_service.app.deployments import DeploymentError, DeploymentStore
from result_service.app.fleet import ClientFleet
from result_service.app.management import ManagementError


class FleetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fleet-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.artifacts = ArtifactStore(self.root / "artifacts")
        self.store = DeploymentStore(self.root / "deployments.json", self.artifacts)
        self.template = self.root / "client.json"
        self.template.write_text(json.dumps({"plugins": {"custom": {
            "enabled": True, "dll": "custom.dll", "db_conn": "host=db password=secret",
            "options": {"format": "info"}}}}), encoding="utf-8")
        self.management = FleetManagement(self.template)
        for name in ("a", "b"): self.management.register(name)
        self.fleet = ClientFleet(self.store, self.management)
        self.plugin = self.upload("custom", b"dll")

    def upload(self, name, data):
        return self.artifacts.upload(io.BytesIO(data), len(data), kind="plugin", name=name,
                                     version="1", filename=name + ".dll")[0]

    def request(self, clients=("a", "b"), **extra):
        return {"client_ids": list(clients), "components": [{"slot": "plugin", "artifact_id": self.plugin["artifact_id"]}],
                "config_patch": {"plugins": {"custom": {"enabled": True, "dll": "{artifact:plugin}/custom.dll"}}}, **extra}

    def publish(self, request):
        preview = self.fleet.preview(request)
        return self.fleet.publish({**request, "plan_fingerprint": preview["plan_fingerprint"], "targets": [
            {"client_id": row["client_id"], "revision": row["revision"]} for row in preview["clients"]]})

    def ready(self, client):
        desired = self.store.get(client)["desired"]
        return self.fleet.report(client, {"desired_revision": desired["revision"], "state": "ready",
            "components": [{"slot": item["slot"], "artifact_id": item["artifact_id"], "state": "active"}
                           for item in desired["components"]]})

    def test_empty_agents_defaults_and_batch_activation(self):
        rows = self.fleet.agents()["agents"]
        self.assertEqual(["a", "b"], [row["client_id"] for row in rows])
        self.assertTrue(all(row["configurable"] for row in rows))
        defaults = self.fleet.defaults()["config_patch"]["plugins"]["custom"]
        self.assertFalse(defaults["enabled"])
        self.assertNotIn("secret", defaults["db_conn"])
        preview = self.fleet.preview(self.request())
        self.assertNotIn("secret", json.dumps(preview))
        result = self.publish(self.request())
        self.assertTrue(all(item["committed"] for item in self.management.reservations.values()))
        self.assertEqual(2, len(result["clients"]))
        self.assertEqual("host=db password=secret", result["clients"][0]["desired"]["config_patch"]["plugins"]["custom"]["db_conn"])
        self.assertFalse(self.fleet.agents()["agents"][0]["configurable"])
        self.assertEqual("current", self.ready("a")["state"])
        self.assertNotIn("a", self.management.reservations)
        self.ready("b")
        self.assertEqual({}, self.management.reservations)

    def test_incremental_patch_preserves_each_machine_configuration(self):
        for client, threads in (("a", 1), ("b", 5)):
            self.store.put_desired(client, {"revision": None, "components": [], "config_patch": {
                "client": {"task_threads": threads}, "plugins": {"other": {"enabled": False, "tag": client}}}})
        result = self.publish(self.request())
        for row, threads in zip(result["clients"], (1, 5)):
            config = row["desired"]["config_patch"]
            self.assertEqual(threads, config["client"]["task_threads"])
            self.assertEqual(row["client_id"], config["plugins"]["other"]["tag"])

    def test_first_enable_cannot_inherit_server_local_binary_path(self):
        with self.assertRaisesRegex(DeploymentError, "首次启用"):
            self.fleet.preview(self.request(config_patch={"plugins": {"custom": {"enabled": True}}}))
        self.assertEqual([], self.store.list())

    def test_shared_slot_not_retargeted_for_untouched_plugin(self):
        other = self.upload("other", b"new")
        self.store.put_desired("a", {"revision": None,
            "components": [{"slot": "shared", "artifact_id": self.plugin["artifact_id"]}],
            "config_patch": {"plugins": {"untouched": {"dll": "{artifact:shared}/custom.dll"},
                                           "custom": {"dll": "{artifact:shared}/custom.dll"}}}})
        result = self.publish(self.request(("a",), components=[{"slot": "shared", "artifact_id": other["artifact_id"]}],
            config_patch={"plugins": {"custom": {"dll": "{artifact:shared}/other.dll"}}}))
        desired = result["clients"][0]["desired"]
        self.assertEqual("{artifact:shared}/custom.dll", desired["config_patch"]["plugins"]["untouched"]["dll"])
        self.assertEqual("{artifact:shared." + other["artifact_id"][:8] + "}/other.dll",
                         desired["config_patch"]["plugins"]["custom"]["dll"])
        self.assertEqual(2, len(desired["components"]))

    def test_publish_rechecks_busy_after_preview_and_never_partially_writes(self):
        request = self.request()
        preview = self.fleet.preview(request)
        self.management.rows["b"]["busy"] = True
        with self.assertRaises(ManagementError) as caught:
            self.fleet.publish({**request, "plan_fingerprint": preview["plan_fingerprint"], "targets": [{"client_id": r["client_id"], "revision": r["revision"]} for r in preview["clients"]]})
        self.assertEqual(409, caught.exception.status)
        self.assertEqual([], self.store.list())
        self.assertEqual({}, self.management.reservations)

    def test_workers_with_unacknowledged_results_are_not_selectable(self):
        self.management.workers = [{"physical_client_id": "a", "worker_id": "a.custom", "status": "idle", "tasks": ["result-pending"]}]
        self.assertFalse(self.fleet.agents()["agents"][0]["configurable"])
        with self.assertRaises(ManagementError): self.fleet.preview(self.request())

    def test_idle_starting_agent_can_receive_corrected_program_version(self):
        # After changing the server's expected program version the old Worker
        # cannot register; the independent Agent must still accept its upgrade.
        self.management.rows["a"]["state"] = "starting"
        self.assertTrue(self.fleet.agents()["agents"][0]["configurable"])
        self.publish(self.request(("a",)))
        self.assertIn("a", self.management.reservations)
        self.ready("a")
        self.management.rows["a"]["busy"] = True
        self.assertFalse(self.fleet.agents()["agents"][0]["configurable"])
        with self.assertRaises(ManagementError): self.fleet.preview(self.request(("a",)))

    def test_two_operators_cannot_overwrite_same_preview(self):
        preview = self.fleet.preview(self.request())
        payload = {**self.request(), "plan_fingerprint": preview["plan_fingerprint"], "targets": [{"client_id": r["client_id"], "revision": r["revision"]} for r in preview["clients"]]}
        outcomes = []
        barrier = threading.Barrier(3)
        def save():
            barrier.wait()
            try:
                self.fleet.publish(payload)
                outcomes.append(200)
            except (DeploymentError, ManagementError) as exc: outcomes.append(exc.status)
        threads = [threading.Thread(target=save) for _ in range(2)]
        for thread in threads: thread.start()
        barrier.wait()
        for thread in threads: thread.join(5)
        self.assertEqual([200, 409], sorted(outcomes))
        self.assertTrue(all(not row["history"] for row in self.store.list()))

    def test_template_change_invalidates_batch_preview_before_reservation(self):
        request = self.request()
        preview = self.fleet.preview(request)
        publication = {**request, "plan_fingerprint": preview["plan_fingerprint"], "targets": [
            {"client_id": row["client_id"], "revision": row["revision"]} for row in preview["clients"]]}
        template = json.loads(self.template.read_text(encoding="utf-8"))
        template["plugins"]["custom"]["db_conn"] = "host=other-db password=another-secret"
        self.template.write_text(json.dumps(template), encoding="utf-8")
        with self.assertRaises(DeploymentError) as rejected:
            self.fleet.publish(publication)
        self.assertEqual(409, rejected.exception.status)
        self.assertEqual([], self.store.list())
        self.assertEqual({}, self.management.reservations)
        fresh = self.fleet.preview(request)
        self.assertNotEqual(preview["plan_fingerprint"], fresh["plan_fingerprint"])
        publication["plan_fingerprint"] = fresh["plan_fingerprint"]
        result = self.fleet.publish(publication)
        self.assertTrue(all(row["desired"]["config_patch"]["plugins"]["custom"]["db_conn"]
                            == "host=other-db password=another-secret" for row in result["clients"]))

    def test_batch_requires_fingerprint_and_checks_the_actual_requested_patch(self):
        request = self.request()
        preview = self.fleet.preview(request)
        publication = {**request, "targets": [
            {"client_id": row["client_id"], "revision": row["revision"]} for row in preview["clients"]]}
        with self.assertRaises(DeploymentError):
            self.fleet.publish(publication)
        publication["plan_fingerprint"] = preview["plan_fingerprint"]
        publication["config_patch"]["client"] = {"task_threads": 99}
        with self.assertRaises(DeploymentError):
            self.fleet.publish(publication)
        self.assertEqual([], self.store.list())
        self.assertEqual({}, self.management.reservations)

    def test_persistence_failure_releases_entire_reservation(self):
        with patch.object(self.store, "_write", side_effect=OSError("disk unavailable")):
            with self.assertRaises(OSError): self.publish(self.request())
        self.assertEqual({}, self.management.reservations)
        self.assertEqual([], self.store.list())

    def test_lost_commit_recovers_before_poll_returns_new_revision(self):
        self.management.fail_commit = True
        with self.assertRaises(ManagementError): self.publish(self.request())
        self.assertEqual(2, len(self.store.list()))
        self.assertEqual(2, len(self.management.reservations))
        self.management.fail_commit = False
        restarted = ClientFleet(DeploymentStore(self.store.path, self.artifacts), self.management)
        desired = restarted.get("a")["desired"]
        self.assertEqual(self.management.reservations["a"]["token"], desired["reservation_id"])
        self.assertTrue(self.management.reservations["a"]["committed"])

    def test_expired_pending_reservation_is_reacquired_or_blocks_delivery(self):
        self.publish(self.request())
        self.management.reservations.clear()
        self.management.rows["a"]["busy"] = True
        with self.assertRaises(ManagementError) as caught: self.fleet.get("a")
        self.assertEqual(409, caught.exception.status)
        self.management.rows["a"]["busy"] = False
        self.fleet.get("a")
        self.assertTrue(self.management.reservations["a"]["committed"])

    def test_report_release_retries_and_completed_details_remain_readable_offline(self):
        self.publish(self.request())
        self.management.online = False
        with self.assertRaises(ManagementError): self.ready("a")
        self.assertEqual("current", self.fleet.get("a")["state"])
        self.management.online = True
        self.fleet.get("a")
        self.assertNotIn("a", self.management.reservations)

    def test_stale_report_cannot_release_new_generation(self):
        first = self.publish(self.request(("a",)))["clients"][0]["desired"]
        self.ready("a")
        self.publish(self.request(("a",), config_patch={"client": {"task_threads": 2}}))
        with self.assertRaises(DeploymentError):
            self.fleet.report("a", {"desired_revision": first["revision"], "state": "failed", "components": [
                {"slot": item["slot"], "artifact_id": item["artifact_id"], "state": "failed"} for item in first["components"]]})
        self.assertIn("a", self.management.reservations)

    def test_rollback_can_use_full_retained_history(self):
        revision = first = None
        for index in range(15):
            saved, _ = self.store.put_desired("a", {"revision": revision, "components": [], "config_patch": {"client": {"task_threads": index + 1}}})
            revision = saved["desired"]["revision"]
            first = first or revision
        result = self.fleet.publish_single("a", {"revision": revision, "action": "rollback", "target_revision": first})
        self.assertEqual(1, result["desired"]["config_patch"]["client"]["task_threads"])


if __name__ == "__main__":
    unittest.main()
