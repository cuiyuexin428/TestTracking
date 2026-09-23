"""Stage saved deployment profiles on an empty client using the real updater."""
from __future__ import annotations

import copy
import hashlib
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
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from result_service.app.config import ServiceConfig
from result_service.app.server import Handler, ResultHTTPServer, ResultService


@unittest.skipUnless(os.name == "nt" and shutil.which("powershell"), "Windows PowerShell required")
class DeploymentProfileUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="profile-client-update-")
        self.root = Path(self.temporary.name)
        self.client = self.root / "新机器 with spaces"
        self.config = self.client / "config" / "client.json"
        self.config.parent.mkdir(parents=True)
        self.result = self.client / "deployments" / "update-result.json"
        self.service = ResultService(ServiceConfig(workspace=str(self.root),
            artifact_store_dir=str(self.root / "repository"), providers={}))
        self.requests = []
        fixture = self

        class UpdateHandler(Handler):
            def do_GET(handler):
                fixture.requests.append(handler.path)
                if handler.path == "/api/deployments/new-machine":
                    # Reserve/activate behavior belongs to Agent integration;
                    # this suite invokes only the immutable staging helper.
                    handler._send(200, fixture.service.deployments.get("new-machine"))
                    return
                super().do_GET()

            def log_message(*args):
                pass

        UpdateHandler.service = self.service
        self.http = ResultHTTPServer(("127.0.0.1", 0), UpdateHandler)
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.endpoint = f"http://127.0.0.1:{self.http.server_port}"
        self.base = {"scheduler": {"endpoint": "tcp://127.0.0.1:5555"},
                     "client": {"worker_id": "new-machine", "deployment_endpoint": self.endpoint},
                     "plugins": {}}
        self.config.write_text(json.dumps(self.base), encoding="utf-8")
        self.base_bytes = self.config.read_bytes()
        # Deliberately inert bytes: no plugin, EXE, or Worker is ever launched.
        self.bootstrap = self.client / "client.exe"
        self.bootstrap.write_bytes(b"bootstrap client must remain untouched")

    def tearDown(self):
        self.http.shutdown()
        self.thread.join(5)
        self.http.server_close()
        self.service.close()
        self.temporary.cleanup()

    def upload(self, kind, name, files):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
            for filename, content in files.items():
                archive.writestr(filename, content)
        raw = output.getvalue()
        return self.service.artifacts.upload(io.BytesIO(raw), len(raw), kind=kind,
            name=name, version="1", filename=name + ".zip")[0]

    def run_update(self):
        command = [shutil.which("powershell"), "-NoLogo", "-NoProfile", "-NonInteractive",
                   "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "tools" / "client_update.ps1"),
                   "-ClientRoot", str(self.client), "-ConfigPath", str(self.config),
                   "-ResultPath", str(self.result)]
        process = subprocess.run(command, cwd=self.root, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=60)
        self.assertEqual(0, process.returncode, process.stdout + process.stderr)
        result = json.loads(self.result.read_text(encoding="utf-8"))
        self.assertEqual("staged", result["status"], result)
        self.assertFalse((self.client / "deployments" / "active.json").exists())
        self.assertEqual(self.base_bytes, self.config.read_bytes())
        self.assertEqual(b"bootstrap client must remain untouched", self.bootstrap.read_bytes())
        return result, json.loads(Path(result["config_path"]).read_text(encoding="utf-8"))

    def publish(self, profile, revision=None):
        compiled = self.service.profiles.compile(profile)
        desired = self.service.deployments.put_desired("new-machine", {
            "revision": revision, "components": compiled["components"],
            "config_patch": compiled["config_patch"]})[0]["desired"]
        return compiled, desired

    def assert_local_file(self, value, contents):
        path = Path(value)
        self.assertTrue(path.is_absolute(), value)
        self.assertTrue(path.is_relative_to(self.client / "deployments" / "objects"), value)
        self.assertEqual(contents, path.read_bytes())

    def test_two_plugin_profile_downloads_all_resources_and_versions_on_empty_client(self):
        plugins = {name: self.upload("plugin", name, {"plugins/" + name + ".dll": name.encode()})
                   for name in ("coverage", "stpcompare")}
        program = self.upload("program", "baseline", {
            "bin/convert.exe": b"baseline program", "bin/engine.dll": b"engine dependency"})
        candidate = self.upload("program", "candidate", {"run/new.exe": b"candidate program"})
        collector = self.upload("program", "collector", {
            "coverage/OpenCppCoverage.exe": b"coverage collector", "coverage/Plugins/Exporter/": b""})
        dataset = self.upload("dataset", "parts-without-version", {
            "dataset/零件 a.stp": b"first geometry", "dataset/nested/b.stp": b"second geometry",
            "docs/readme.txt": b"not part of the dataset"})
        original_dataset = self.service.artifacts.object_path(dataset["sha256"]).read_bytes()
        definition = {"revision": None, "name": "覆盖率与程序比较", "plugins": {}}
        for name in ("coverage", "stpcompare"):
            resources = {"plugin": {"artifact_id": plugins[name]["artifact_id"]},
                         "dataset": {"artifact_id": dataset["artifact_id"], "root": "dataset"}}
            plugin = {"dry_run": False, "resources": resources}
            if name == "coverage":
                resources.update(program={"artifact_id": program["artifact_id"]},
                                 collector={"artifact_id": collector["artifact_id"]})
                plugin["engine_dlls"] = ["bin/engine.dll"]
            else:
                resources.update(baseline={"artifact_id": program["artifact_id"]},
                                 candidate={"artifact_id": candidate["artifact_id"]})
            definition["plugins"][name] = plugin
        saved = self.service.profiles.save(None, definition)
        compiled, desired = self.publish(saved)
        self.assertEqual({}, json.loads(self.config.read_text(encoding="utf-8"))["plugins"])
        staged, effective = self.run_update()
        self.assertEqual(desired["revision"], staged["revision"])
        self.assertEqual({"coverage", "stpcompare"}, set(effective["plugins"]))
        self.assertEqual("new-machine", effective["client"]["worker_id"])
        self.assertEqual(self.endpoint, effective["client"]["deployment_endpoint"])
        self.assertEqual(1, effective["client"]["task_threads"])
        self.assertEqual([], effective["client"]["execution_windows"])
        for name in ("coverage", "stpcompare"):
            self.assertTrue(effective["plugins"][name]["enabled"])
            self.assert_local_file(effective["plugins"][name]["dll"], name.encode())
        coverage = effective["plugins"]["coverage"]
        compare = effective["plugins"]["stpcompare"]
        self.assert_local_file(coverage["target_exe"], b"baseline program")
        self.assert_local_file(coverage["engine_dlls"][0], b"engine dependency")
        self.assert_local_file(coverage["opencppcoverage"], b"coverage collector")
        self.assertTrue((Path(coverage["opencppcoverage"]).parent / "Plugins" / "Exporter").is_dir())
        self.assertEqual(Path(coverage["target_exe"]).parent, Path(coverage["program_dir"]))
        self.assert_local_file(compare["programs"]["baseline"]["executable"], b"baseline program")
        self.assert_local_file(compare["programs"]["candidate"]["executable"], b"candidate program")
        self.assertEqual("", compare["dataset_version_file"])
        self.assertEqual(str(self.client / "work" / "stpcompare"), compare["work_dir"])
        self.assertEqual(str(self.client / "exports" / "coverage"), coverage["export_dir"])

        for binding in compiled["dataset_bindings"]:
            name = binding["plugin"]
            local_root = Path(effective["plugins"][name]["stp_root"])
            self.assertTrue(local_root.is_relative_to(self.client / "deployments" / "objects"))
            self.assertNotEqual(dataset["artifact_id"], binding["artifact_id"])
            expected = compiled["scheduler_plugins"][name]
            self.assertEqual(expected["expected_dataset_version"], (local_root / "version.txt").read_text().strip())
            manifest = Path(expected["stp_manifest"])
            manifest_rows = [json.loads(line) for line in manifest.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(2, len(manifest_rows))
            self.assertEqual({path.relative_to(local_root).as_posix() for path in local_root.rglob("*.stp")},
                             {row["path"] for row in manifest_rows})
            for row in manifest_rows:
                data = local_root.joinpath(*row["path"].split("/")).read_bytes()
                self.assertEqual(len(data), row["size_bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), row["content_sha256"])
        self.assertEqual(coverage["stp_root"], compare["stp_root"])
        self.assertEqual(original_dataset, self.service.artifacts.object_path(dataset["sha256"]).read_bytes())
        downloaded = [path for path in self.requests if path.startswith("/api/artifacts/objects/")]
        expected_downloads = {self.service.artifacts.get(item["artifact_id"])["download_url"]
                              for item in compiled["components"]}
        self.assertEqual(expected_downloads, set(downloaded))
        self.assertEqual(len(expected_downloads), len(downloaded), "shared resources download once")
        self.assertNotIn(dataset["download_url"], downloaded, "the generated versioned bundle must be used")

        # A subsequent central edit stages a new immutable release, preserving
        # the first deployment and the original bootstrap installation.
        previous_config = Path(staged["config_path"])
        previous_bytes = previous_config.read_bytes()
        old_candidate_path = Path(compare["programs"]["candidate"]["executable"])
        replacement = self.upload("program", "candidate-upgrade", {"run/new.exe": b"upgraded candidate"})
        changed = copy.deepcopy(saved)
        changed["plugins"]["stpcompare"]["resources"]["candidate"] = {"artifact_id": replacement["artifact_id"]}
        saved_again = self.service.profiles.save(saved["profile_id"], changed)
        _, next_desired = self.publish(saved_again, desired["revision"])
        self.requests.clear()
        next_staged, next_effective = self.run_update()
        self.assertEqual(next_desired["revision"], next_staged["revision"])
        self.assertNotEqual(staged["config_path"], next_staged["config_path"])
        self.assertEqual(previous_bytes, previous_config.read_bytes())
        self.assertEqual(b"candidate program", old_candidate_path.read_bytes())
        self.assert_local_file(next_effective["plugins"]["stpcompare"]["programs"]["candidate"]["executable"], b"upgraded candidate")
        self.assertEqual([replacement["download_url"]],
                         [path for path in self.requests if path.startswith("/api/artifacts/objects/")])
        self.requests.clear()
        repeated, _ = self.run_update()
        self.assertEqual(next_staged["config_path"], repeated["config_path"])
        self.assertFalse(any(path.startswith("/api/artifacts/objects/") for path in self.requests))


if __name__ == "__main__":
    unittest.main()
