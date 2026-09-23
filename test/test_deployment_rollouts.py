"""Durable rollout invariants; native protocol is covered by integration tests."""
import copy
import io
import json
import sys
import tempfile
import unittest
import uuid
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fleet_fixture import FleetManagement
from result_service.app.artifacts import ArtifactStore
from result_service.app.deployments import DeploymentError, DeploymentStore
from result_service.app.deployment_profiles import ProfileStore
from result_service.app.deployment_rollouts import CAPABILITY, DeploymentRollouts
from result_service.app.fleet import ClientFleet
from result_service.app.management import ManagementError


class RolloutManagement(FleetManagement):
    def __init__(self):
        super().__init__()
        self.configuration = {"plugins": {name: {"enabled": True, "dll": name + ".dll"}
                                         for name in ("coverage", "stpcompare")}}
        self.revision = "scheduler-1"
        self.rollouts = {}
        self.blockers = {}
        self.lose_begin = False
        self.lose_apply = False
        self.fail_begin_plugin = None

    def register(self, client, **values):
        super().register(client, capabilities=[CAPABILITY], update_window_open=True, **values)

    def get_config(self, kind):
        return {"available": True, "revision": self.revision, "config": copy.deepcopy(self.configuration),
                "restart_required": False}

    def get_status(self):
        result = super().get_status()
        result.update(plugins=list(self.configuration["plugins"]), configuration=copy.deepcopy(self.configuration),
                      unfinished_runs=[{**row, "plugin_type": name} for name, rows in self.blockers.items() for row in rows])
        return result

    def plugin_rollout(self, action, plugin, rollout_id, **values):
        self.calls.append((action, {"plugin": plugin, "rollout_id": rollout_id, **copy.deepcopy(values)}))
        if not self.online:
            raise ManagementError("scheduler offline", 503)
        existing = self.rollouts.get((plugin, rollout_id))
        if action == "begin_plugin_rollout":
            if plugin == self.fail_begin_plugin:
                raise ManagementError("begin conflict", 409)
            if not existing:
                if values["config_revision"] != self.revision:
                    raise ManagementError("configuration changed", 409)
                previous = values.get("replaces_rollout_id")
                if previous:
                    old = self.rollouts[(plugin, previous)]
                    if old["phase"] not in {"switching", "completed"}:
                        raise ManagementError("cannot replace", 409)
                    old["phase"] = "superseded"
                existing = {"ok": True, "plugin_type": plugin, "rollout_id": rollout_id,
                            "target_id": values["target_id"], "target_patch": values["target_patch"],
                            "phase": "switching" if previous else "waiting",
                            "boundary": [row["run_id"] for row in self.blockers.get(plugin, [])]}
                self.rollouts[(plugin, rollout_id)] = existing
            if self.lose_begin:
                self.lose_begin = False
                raise ManagementError("lost begin response", 503)
        elif not existing:
            raise ManagementError("rollout not found", 404)
        elif action == "activate_plugin_rollout":
            if self.blockers.get(plugin):
                raise ManagementError("unfinished", 409)
            existing["phase"] = "switching"
        elif action == "finish_plugin_rollout":
            existing["phase"] = "completed"
        elif action == "cancel_plugin_rollout":
            if existing["phase"] != "waiting":
                raise ManagementError("already switching", 409)
            existing["phase"] = "cancelled"
        elif action in {"resume_rollout_run", "abandon_rollout_run"}:
            row = next(row for row in self.blockers[plugin] if row["run_id"] == values["run_id"])
            if row["status"] != "stopped":
                raise ManagementError("not stopped", 409)
            if action.startswith("resume"):
                row["status"] = "running"
            else:
                self.blockers[plugin].remove(row)
        return {**copy.deepcopy(existing), "blockers": copy.deepcopy(self.blockers.get(plugin, [])),
                "ready_to_switch": not self.blockers.get(plugin)}

    def apply_plugin_deployment(self, revision, plugins, *, rollout_id=None):
        if revision != self.revision:
            raise ManagementError("configuration changed", 409)
        for name, fields in plugins.items():
            if self.rollouts[(name, rollout_id)]["target_patch"] != fields:
                raise ManagementError("wrong frozen patch", 409)
            self.configuration["plugins"][name].update(fields)
        self.revision += "-applied"
        if self.lose_apply:
            self.lose_apply = False
            raise ManagementError("lost apply response", 503)
        return self.get_config("scheduler")


class DeploymentRolloutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rollout-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifacts = ArtifactStore(self.root / "artifacts")
        self.deployments = DeploymentStore(self.root / "deployments.json", self.artifacts)
        self.profiles = ProfileStore(self.root / "profiles.json", self.deployments)
        self.management = RolloutManagement()
        self.fleet = ClientFleet(self.deployments, self.management)
        self.rollouts = DeploymentRollouts(self.root / "rollouts.json", self.profiles, self.fleet)
        for name in ("one", "two", "three"):
            self.management.register(name)
        self.profile = self.create_profile()

    def artifact(self, kind, filename, content=b"binary", version="1"):
        raw = io.BytesIO()
        with zipfile.ZipFile(raw, "w") as archive:
            archive.writestr(filename, content)
        value = raw.getvalue()
        return self.artifacts.upload(io.BytesIO(value), len(value), kind=kind, name=filename,
                                     version=version, filename=filename + ".zip")[0]["artifact_id"]

    def create_profile(self, *, both=False):
        plugin = self.artifact("plugin", "coverage.dll")
        program = self.artifact("program", "convert.exe")
        collector = self.artifact("program", "collector.exe")
        data = self.artifact("dataset", "part.stp", b"geometry")
        definition = {"revision": None, "name": "Coverage", "plugins": {"coverage": {
            "dry_run": True, "resources": {"plugin": {"artifact_id": plugin},
                "program": {"artifact_id": program}, "collector": {"artifact_id": collector},
                "dataset": {"artifact_id": data, "root": ""}}}},
            "schedule": {"task_threads": 2, "execution_windows": [], "update_windows": []}}
        if both:
            definition["plugins"]["stpcompare"] = {"dry_run": True, "resources": {
                "plugin": {"artifact_id": plugin}, "baseline": {"artifact_id": program},
                "candidate": {"artifact_id": collector}, "dataset": {"artifact_id": data, "root": ""}}}
        return self.profiles.save(None, definition)

    def preview(self, profile=None, **values):
        profile = profile or self.profile
        body = {"revision": profile["revision"], "mode": "assign", "client_ids": ["one", "two", "three"], **values}
        return body, self.rollouts.preview(profile["profile_id"], body)

    def publish(self, profile=None, **values):
        profile = profile or self.profile
        body, preview = self.preview(profile, **values)
        payload = {**body, "plan_fingerprint": preview["plan_fingerprint"], "request_id": uuid.uuid4().hex}
        return self.rollouts.publish(profile["profile_id"], payload), payload

    def ready(self, client, *, failed=False):
        desired = self.deployments.get(client)["desired"]
        self.assertIsNotNone(desired)
        self.fleet.report(client, {"desired_revision": desired["revision"], "state": "failed" if failed else "ready",
            "components": [{"slot": item["slot"], "artifact_id": item["artifact_id"],
                            "state": "failed" if failed else "active"} for item in desired["components"]],
            **({"error": "bad dll"} if failed else {})})

    def initial(self):
        record, _ = self.publish()
        self.rollouts.tick()
        for client in ("one", "two", "three"):
            self.ready(client)
        self.rollouts.tick()
        self.assertEqual("completed", self.rollouts.get(record["rollout_id"])["status"])

    def changed(self):
        profile = copy.deepcopy(self.profile)
        profile["plugins"]["coverage"]["resources"]["program"] = {
            "artifact_id": self.artifact("program", "convert.exe", b"new binary", "2")}
        return self.profiles.save(profile["profile_id"], profile)

    def test_waiting_keeps_old_runs_and_does_not_reserve_or_publish(self):
        self.management.blockers["coverage"] = [{"run_id": "A", "status": "running"}, {"run_id": "B", "status": "queued"}]
        record, _ = self.publish()
        self.rollouts.tick()
        self.assertEqual("waiting_runs", self.rollouts.get(record["rollout_id"])["status"])
        self.assertEqual(["A", "B"], [row["run_id"] for row in self.rollouts.get(record["rollout_id"])["waiting_runs"]])
        self.assertEqual({}, self.management.reservations)
        self.assertEqual([], self.deployments.list())
        with self.assertRaises(ManagementError):
            self.rollouts.guard_run({"action": "run", "plugin_type": "coverage"})

    def test_offline_and_window_do_not_block_successful_machine(self):
        self.management.rows["three"]["online"] = False
        self.management.rows["two"]["update_window_open"] = False
        record, _ = self.publish()
        self.rollouts.tick()
        self.assertEqual({"one"}, set(self.management.reservations))
        self.ready("one")
        self.rollouts.tick()
        value = self.rollouts.get(record["rollout_id"])
        self.assertEqual("ready", value["status"])
        self.assertEqual({"one": "ready", "two": "waiting_window", "three": "offline"},
                         {row["client_id"]: row["status"] for row in value["clients"]})
        self.management.rows["two"]["update_window_open"] = True
        self.management.rows["three"]["online"] = True
        self.rollouts.tick()
        self.ready("two")
        self.ready("three")
        self.rollouts.tick()
        self.assertEqual("completed", self.rollouts.get(record["rollout_id"])["status"])

    def test_begin_lost_reply_and_process_restart_preserve_queue_boundary(self):
        self.management.blockers["coverage"] = [{"run_id": "A", "status": "queued"}]
        self.management.lose_begin = True
        record, payload = self.publish()
        self.assertEqual("blocked", record["status"])
        self.rollouts = DeploymentRollouts(self.root / "rollouts.json", self.profiles, self.fleet)
        self.rollouts.tick()
        self.assertEqual(["A"], self.management.rollouts[("coverage", record["rollout_id"])]["boundary"])
        self.assertEqual(record["rollout_id"], self.rollouts.publish(self.profile["profile_id"], payload)["rollout_id"])
        self.assertEqual(1, len(self.rollouts.list()["rollouts"]))

    def test_apply_lost_reply_reconciles_without_rewriting_target(self):
        record, _ = self.publish()
        self.management.lose_apply = True
        self.rollouts.tick()
        revision = self.management.revision
        self.assertEqual("blocked", self.rollouts.get(record["rollout_id"])["status"])
        self.rollouts = DeploymentRollouts(self.root / "rollouts.json", self.profiles, self.fleet)
        self.rollouts.tick()
        self.assertEqual(revision, self.management.revision)
        self.assertEqual(3, len(self.deployments.list()))

    def test_preview_cas_includes_template_and_automatic_machine_set(self):
        template = self.root / "client.json"
        self.management.config.client_config_path = str(template)
        template.write_text(json.dumps({"plugins": {"coverage": {"db_conn": "secret=first"}}}))
        body, preview = self.preview()
        template.write_text(json.dumps({"plugins": {"coverage": {"db_conn": "secret=second"}}}))
        with self.assertRaises(DeploymentError):
            self.rollouts.publish(self.profile["profile_id"], {**body, "plan_fingerprint": preview["plan_fingerprint"], "request_id": "stale"})
        self.assertEqual({}, self.management.rollouts)

    def test_update_includes_offline_desired_and_preserves_machine_preferences(self):
        self.initial()
        old = self.deployments.get("one")["desired"]
        value = copy.deepcopy(old["config_patch"])
        value["client"]["task_threads"] = 7
        value["plugins"]["unrelated"] = {"enabled": False, "custom": 42}
        self.deployments.put_desired("one", {"revision": old["revision"], "components": old["components"], "config_patch": value})
        self.management.rows["three"]["online"] = False
        self.management.rows["three"]["plugin_keys"] = []
        profile = self.changed()
        record, _ = self.publish(profile, mode="update", client_ids=[])
        self.assertEqual({"one", "two", "three"}, {item["client_id"] for item in record["clients"]})
        self.rollouts.tick()
        current = self.deployments.get("one")["desired"]["config_patch"]
        self.assertEqual(7, current["client"]["task_threads"])
        self.assertEqual({"enabled": False, "custom": 42}, current["plugins"]["unrelated"])

    def test_cancel_partial_begin_does_not_begin_failed_plugin(self):
        self.profile = self.create_profile(both=True)
        self.management.fail_begin_plugin = "stpcompare"
        record, _ = self.publish()
        self.assertEqual("pending", record["phase"])
        cancelled = self.rollouts.operation(record["rollout_id"], "cancel", {"revision": record["revision"]})
        self.assertEqual("cancelled", cancelled["status"])
        self.assertEqual("cancelled", self.management.rollouts[("coverage", record["rollout_id"])]["phase"])
        self.assertNotIn(("stpcompare", record["rollout_id"]), self.management.rollouts)

    def test_cancel_before_switch_and_reject_after_switch(self):
        self.management.blockers["coverage"] = [{"run_id": "A", "status": "stopped"}]
        record, _ = self.publish()
        self.rollouts.operation(record["rollout_id"], "cancel", {"revision": record["revision"]})
        self.management.blockers.clear()
        record, _ = self.publish()
        self.rollouts.tick()
        current = self.rollouts.get(record["rollout_id"])
        with self.assertRaises(DeploymentError):
            self.rollouts.operation(record["rollout_id"], "cancel", {"revision": current["revision"]})

    def test_failed_machine_retry_does_not_redeploy_successes(self):
        record, _ = self.publish()
        self.rollouts.tick()
        self.ready("one")
        self.ready("two", failed=True)
        self.ready("three")
        self.rollouts.tick()
        previous = self.deployments.get("one")["desired"]["revision"]
        current = self.rollouts.get(record["rollout_id"])
        self.rollouts.operation(record["rollout_id"], "retry", {"revision": current["revision"], "client_ids": ["two"]})
        self.rollouts.tick()
        self.assertEqual(previous, self.deployments.get("one")["desired"]["revision"])
        self.ready("two")
        self.rollouts.tick()
        self.assertEqual("completed", self.rollouts.get(record["rollout_id"])["status"])

    def test_bad_package_can_be_replaced_with_new_target(self):
        record, _ = self.publish()
        self.rollouts.tick()
        for client in ("one", "two", "three"):
            self.ready(client, failed=True)
        self.rollouts.tick()
        current = self.rollouts.get(record["rollout_id"])
        replacement, _ = self.publish(self.changed(), mode="update", client_ids=[],
            replaces_rollout_id=current["rollout_id"], replaces_revision=current["revision"])
        self.assertFalse(replacement["can_cancel"])
        self.rollouts.tick()
        self.assertEqual("superseded", self.rollouts.get(record["rollout_id"])["status"])
        for client in ("one", "two", "three"):
            self.ready(client)
        self.rollouts.tick()
        self.assertEqual("completed", self.rollouts.get(replacement["rollout_id"])["status"])

    def test_advanced_resources_rejected_but_schedule_only_preserved(self):
        self.initial()
        current = self.deployments.get("one")["desired"]
        proposal = {"client_ids": ["one"], "components": [], "config_patch": {"client": {"task_threads": 9}}}
        preview = self.fleet.preview(proposal)
        self.fleet.publish({**proposal, "targets": [{"client_id": "one", "revision": current["revision"]}],
                            "plan_fingerprint": preview["plan_fingerprint"]})
        self.assertEqual(9, self.deployments.get("one")["desired"]["config_patch"]["client"]["task_threads"])
        proposal["config_patch"]["plugins"] = {"coverage": {"dry_run": False}}
        with self.assertRaises(DeploymentError):
            self.fleet.publish({**proposal, "targets": [{"client_id": "one", "revision": None}]})

    def test_old_agent_is_visible_and_cannot_commit(self):
        self.management.rows["two"]["capabilities"] = []
        body, preview = self.preview()
        self.assertEqual(["two"], preview["unsupported_clients"])
        self.assertTrue(preview["publish_blocked"])
        with self.assertRaisesRegex(DeploymentError, "two"):
            self.rollouts.publish(self.profile["profile_id"], {**body, "plan_fingerprint": preview["plan_fingerprint"], "request_id": "old-agent"})
        self.assertEqual({}, self.management.rollouts)

    def test_lost_machine_commit_reply_reuses_published_token(self):
        record, _ = self.publish()
        self.management.fail_commit = True
        self.rollouts.tick()
        desired = self.deployments.get("one")["desired"]
        self.assertIsNotNone(desired)
        self.management.fail_commit = False
        self.rollouts = DeploymentRollouts(self.root / "rollouts.json", self.profiles, self.fleet)
        self.rollouts.tick()
        self.assertEqual(desired["revision"], self.deployments.get("one")["desired"]["revision"])
        self.assertEqual(desired["reservation_id"], self.management.reservations["one"]["token"])

    def test_assignment_cannot_create_a_second_plugin_target(self):
        self.initial()
        other = copy.deepcopy(self.profile)
        other.pop("profile_id")
        other.pop("updated_at")
        other.update(revision=None, name="Other profile")
        other = self.profiles.save(None, other)
        with self.assertRaisesRegex(DeploymentError, "已发布方案"):
            self.preview(other, client_ids=["one"])

    def test_assignment_uses_published_resources_template_and_schedule_after_draft_edit(self):
        template = self.root / "published-template.json"
        template.write_text(json.dumps({"plugins": {"coverage": {"db_conn": "published database"}}}))
        self.management.config.client_config_path = str(template)
        self.initial()
        published = self.rollouts.list()["rollouts"][0]
        original = {row["client_id"]: row["desired"]["revision"] for row in self.deployments.list()}
        self.management.register("four")
        draft = self.changed()
        draft["schedule"]["task_threads"] = 13
        draft["name"] = "Unpublished draft name"
        draft = self.profiles.save(draft["profile_id"], draft)
        template.write_text("malformed unpublished template")
        with patch.object(self.profiles, "compile", side_effect=AssertionError("draft must not compile")):
            body, preview = self.preview(draft, client_ids=["four"])
            self.assertTrue(preview["published_target"])
            self.assertEqual(published["resource_summary"], preview["resource_summary"])
            self.assertEqual(published["target_ids"], preview["target_ids"])
            self.assertEqual(published["profile_revision"], preview["profile_revision"])
            self.assertEqual(["four"], [row["client_id"] for row in preview["clients"]])
            plan = preview["clients"][0]["config_patch"]
            self.assertEqual(2, plan["client"]["task_threads"])
            # A subsequent editor save is unrelated to the immutable target CAS.
            latest = self.profiles.get(draft["profile_id"])
            latest["name"] = "Another unpublished name"
        self.profiles.save(latest["profile_id"], latest)
        before_calls = len(self.management.calls)
        with patch.object(self.profiles, "compile", side_effect=AssertionError("draft must not compile")):
            record = self.rollouts.publish(draft["profile_id"], {
                **body, "request_id": "assign-published", "plan_fingerprint": preview["plan_fingerprint"]})
        self.rollouts.tick()
        desired = self.deployments.get("four")["desired"]["config_patch"]
        self.assertEqual("published database", desired["plugins"]["coverage"]["db_conn"])
        self.assertEqual(2, desired["client"]["task_threads"])
        self.assertEqual(original, {row["client_id"]: row["desired"]["revision"] for row in self.deployments.list()
                                    if row["client_id"] != "four"})
        self.assertTrue(record["published_target"])
        self.assertFalse(any(action in {"begin_plugin_rollout", "activate_plugin_rollout", "finish_plugin_rollout"}
                             for action, _ in self.management.calls[before_calls:]))

    def test_stopped_requires_resume_or_explicit_abandon_reason(self):
        self.management.blockers["coverage"] = [{"run_id": "A", "status": "stopped"}]
        record, _ = self.publish()
        self.rollouts.tick()
        record = self.rollouts.get(record["rollout_id"])
        with self.assertRaises(DeploymentError):
            self.rollouts.operation(record["rollout_id"], "abandon", {"revision": record["revision"], "plugin_type": "coverage", "run_id": "A"})
        self.rollouts.operation(record["rollout_id"], "abandon", {"revision": record["revision"], "plugin_type": "coverage", "run_id": "A", "reason": "discard old work"})
        self.rollouts.tick()
        self.assertEqual("deploying", self.rollouts.get(record["rollout_id"])["status"])

    def test_submitted_target_does_not_change_with_later_template_or_recipe_edit(self):
        template = self.root / "template.json"
        template.write_text(json.dumps({"plugins": {"coverage": {"db_conn": "frozen connection"}}}))
        self.management.config.client_config_path = str(template)
        self.management.blockers["coverage"] = [{"run_id": "A", "status": "running"}]
        record, _ = self.publish()
        original_target = record["target_ids"]["coverage"]
        self.changed()
        template.write_text(json.dumps({"plugins": {"coverage": {"db_conn": "changed connection"}}}))
        self.management.blockers.clear()
        self.rollouts.tick()
        plugin = self.deployments.get("one")["desired"]["config_patch"]["plugins"]["coverage"]
        self.assertEqual("frozen connection", plugin["db_conn"])
        self.assertEqual(original_target, plugin["deployment_target_id"])

    def test_one_reservation_failure_does_not_prevent_other_machines_updating(self):
        record, _ = self.publish()
        original = self.management.client_control
        def reserve(action, **values):
            if action == "reserve_clients" and values["client_ids"] == ["one"]:
                raise ManagementError("results still awaiting acknowledgement", 409)
            return original(action, **values)
        with patch.object(self.management, "client_control", side_effect=reserve):
            self.rollouts.tick()
        self.assertIsNone(self.deployments.get("one")["desired"])
        self.assertIsNotNone(self.deployments.get("two")["desired"])
        self.assertIsNotNone(self.deployments.get("three")["desired"])
        value = self.rollouts.get(record["rollout_id"])
        failure = next(row for row in value["clients"] if row["client_id"] == "one")
        self.assertEqual("failed", failure["status"])
        self.assertIn("acknowledgement", failure["error"])

    def test_corrupt_persistent_request_store_fails_closed(self):
        self.publish()
        self.rollouts.path.write_text('{"schema_version":1, "requests":')
        with self.assertRaises(DeploymentError):
            self.rollouts.tick()
        with self.assertRaises(DeploymentError):
            self.rollouts.guard_run({"action": "run", "plugin_type": "coverage"})
        self.assertEqual({}, self.management.reservations)

    def test_window_closing_after_publication_releases_only_after_agent_ack(self):
        record, _ = self.publish()
        self.rollouts.tick()
        desired = self.deployments.get("one")["desired"]
        self.management.rows["one"]["update_window_open"] = False
        # An already owned generation may be delivered once to obtain the
        # Agent's waiting acknowledgement. It cannot activate without ownership.
        self.assertEqual(desired["revision"], self.fleet.get("one")["desired"]["revision"])
        self.fleet.report("one", {"desired_revision": desired["revision"], "state": "waiting_window",
            "components": [{"slot": item["slot"], "artifact_id": item["artifact_id"], "state": "pending"}
                           for item in desired["components"]]})
        self.rollouts.tick()
        self.assertNotIn("one", self.management.reservations)
        with self.assertRaisesRegex(ManagementError, "更新时间窗"):
            self.fleet.get("one")
        self.management.rows["one"]["update_window_open"] = True
        self.assertEqual(desired["revision"], self.fleet.get("one")["desired"]["revision"])
        self.assertEqual(desired["reservation_id"], self.management.reservations["one"]["token"])
        self.assertTrue(any(action == "reserve_clients" and values.get("require_update_window_open") is True
                            for action, values in self.management.calls))


if __name__ == "__main__":
    unittest.main()
