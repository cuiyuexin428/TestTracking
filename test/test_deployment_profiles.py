from __future__ import annotations

import copy
import hashlib
import io
import json
import stat
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.artifacts import ArtifactStore
from result_service.app.deployments import DeploymentError, DeploymentStore
from result_service.app.deployment_profiles import ProfileStore


class ProfileTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="deployment-profiles-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.artifacts = ArtifactStore(self.root / "artifacts")
        self.deployments = DeploymentStore(self.root / "deployments.json", self.artifacts)
        self.store = ProfileStore(self.root / "profiles.json", self.deployments)
        self.plugin = self.upload("plugin", "coverage", b"plugin DLL", "coverage.dll")
        self.program = self.upload_zip("program", "engine", {
            "bin/engine.exe": b"main executable", "bin/helper.dll": b"dependency"})
        self.collector = self.upload("program", "collector", b"collector", "OpenCppCoverage.exe")
        self.dataset = self.upload_zip("dataset", "inputs", {
            "inputs/version.txt": b"release-1\r\n", "inputs/a.stp": b"first input",
            "inputs/nested/b.stp": b"second input", "readme.txt": b"not a task"})

    @staticmethod
    def zip_bytes(files):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        return output.getvalue()

    def upload(self, kind, name, data, filename):
        return self.artifacts.upload(io.BytesIO(data), len(data), kind=kind, name=name,
                                     version="1", filename=filename)[0]

    def upload_zip(self, kind, name, files):
        return self.upload(kind, name, self.zip_bytes(files), name + ".zip")

    def body(self):
        return {"revision": None, "name": "覆盖率常规运行", "description": "复用资源方案", "plugins": {
            "coverage": {"dry_run": False, "resources": {
                "plugin": {"artifact_id": self.plugin["artifact_id"]},
                "program": {"artifact_id": self.program["artifact_id"]},
                "collector": {"artifact_id": self.collector["artifact_id"]},
                "dataset": {"artifact_id": self.dataset["artifact_id"], "root": "inputs"}},
                "engine_dlls": ["bin/helper.dll"]}}}

    def test_save_resolves_unique_entries_and_compile_manages_dataset(self):
        saved = self.store.save(None, self.body())
        self.assertEqual("coverage.dll", saved["plugins"]["coverage"]["resources"]["plugin"]["entry"])
        self.assertEqual({"task_threads": 1, "execution_windows": [], "update_windows": []}, saved["schedule"])
        prepared = self.store.compile(saved)
        config = prepared["config_patch"]["plugins"]["coverage"]
        self.assertEqual({"coverage"}, set(prepared["config_patch"]["plugins"]))
        self.assertFalse(config["dry_run"])
        self.assertEqual("{artifact:profile.coverage.program}/bin", config["program_dir"])
        self.assertEqual("{artifact:profile.coverage.program}/bin/engine.exe", config["target_exe"])
        self.assertEqual("sha256:", config["program_version"][:7])
        binding = prepared["dataset_bindings"][0]
        self.assertEqual("sha256:" + binding["manifest_sha256"], binding["version"])
        self.assertEqual("release-1", binding["source_version"])
        self.assertNotEqual(self.dataset["artifact_id"], binding["artifact_id"])
        manifest = Path(binding["manifest_path"])
        rows = [json.loads(line) for line in manifest.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(["a.stp", "nested/b.stp"], [row["path"] for row in rows])
        self.assertEqual(hashlib.sha256(b"first input").hexdigest(), rows[0]["content_sha256"])
        self.assertEqual(11, rows[0]["size_bytes"])
        self.assertEqual(binding["version"], prepared["scheduler_plugins"]["coverage"]["expected_dataset_version"])
        self.assertEqual(manifest.as_posix(), prepared["scheduler_plugins"]["coverage"]["stp_manifest"])
        reopened = ProfileStore(self.store.path, self.deployments)
        self.assertEqual(saved, reopened.get(saved["profile_id"]))
        self.assertEqual([saved], reopened.list())

    def test_missing_version_creates_cached_derived_archive_without_changing_source(self):
        dataset = self.upload_zip("dataset", "unversioned", {"part/a.stp": b"geometry", "empty/": b""})
        original = self.artifacts.object_path(dataset["sha256"]).read_bytes()
        body = self.body()
        body["plugins"]["coverage"]["resources"]["dataset"] = {"artifact_id": dataset["artifact_id"], "root": "part"}
        saved = self.store.save(None, body)
        with patch.object(self.store.catalog, "_prepare_dataset", side_effect=AssertionError("large dataset re-read")):
            prepared = self.store.compile(saved)
        binding = prepared["dataset_bindings"][0]
        self.assertNotEqual(dataset["artifact_id"], binding["artifact_id"])
        actual = self.artifacts.get(binding["artifact_id"])
        with zipfile.ZipFile(self.artifacts.object_path(actual["sha256"])) as archive:
            self.assertEqual(b"geometry", archive.read("part/a.stp"))
            self.assertEqual(binding["version"] + "\n", archive.read("part/version.txt").decode())
            self.assertIn("empty/", archive.namelist())
        self.assertEqual(original, self.artifacts.object_path(dataset["sha256"]).read_bytes())
        self.assertEqual(binding["version"], prepared["scheduler_plugins"]["coverage"]["expected_dataset_version"])
        self.assertIn(binding["artifact_id"], [item["artifact_id"] for item in prepared["components"]])
        reopened = ProfileStore(self.store.path, self.deployments)
        with patch.object(reopened.catalog, "_prepare_dataset", side_effect=AssertionError("cache not persistent")):
            self.assertEqual(binding, reopened.compile(saved)["dataset_bindings"][0])

    def test_single_stp_upload_is_prepared_as_dataset(self):
        data = self.upload("dataset", "single", b"solid", "example.stp")
        body = self.body()
        body["plugins"]["coverage"]["resources"]["dataset"] = {"artifact_id": data["artifact_id"], "root": ""}
        saved = self.store.save(None, body)
        binding = self.store.compile(saved)["dataset_bindings"][0]
        self.assertEqual(1, binding["file_count"])
        with zipfile.ZipFile(self.artifacts.object_path(self.artifacts.get(binding["artifact_id"])["sha256"])) as archive:
            self.assertEqual(b"solid", archive.read("example.stp"))
            self.assertIn("version.txt", archive.namelist())

    def test_existing_versions_are_replaced_by_content_identity_without_duplicate_entries(self):
        identities = []
        for index, (version, data) in enumerate(((b"release-1\n", b"old"),
                                               (b" release-1 \r\n", b"new"))):
            source = self.upload_zip("dataset", f"same-version-{index}", {
                "part/VeRsIoN.TxT": version, "part/file.stp": data,
                "elsewhere/version.txt": b"untouched"})
            original = self.artifacts.object_path(source["sha256"]).read_bytes()
            prepared = self.store.catalog.prepare_dataset(source["artifact_id"], "part")
            self.assertEqual("sha256:" + prepared["manifest_sha256"], prepared["version"])
            self.assertEqual(prepared["version"].strip(), prepared["version"])
            identities.append(prepared["version"])
            deployed = self.artifacts.get(prepared["artifact_id"])
            with zipfile.ZipFile(self.artifacts.object_path(deployed["sha256"])) as archive:
                versions = [name for name in archive.namelist() if name.casefold() == "part/version.txt"]
                self.assertEqual(["part/version.txt"], versions)
                self.assertEqual((prepared["version"] + "\n").encode(), archive.read(versions[0]))
                self.assertEqual(data, archive.read("part/file.stp"))
                self.assertEqual(b"untouched", archive.read("elsewhere/version.txt"))
            self.assertEqual(original, self.artifacts.object_path(source["sha256"]).read_bytes())
        self.assertNotEqual(*identities)

    def test_schema_one_dataset_cache_is_retained_but_never_reused(self):
        from result_service.app.resource_catalog import json_bytes
        key = hashlib.sha256(json_bytes({"schema_version": 1, "sha256": self.dataset["sha256"],
                                         "filename": self.dataset["filename"], "root": "inputs"})).hexdigest()
        old = self.artifacts.root / "managed-datasets" / key
        old.mkdir(parents=True)
        old_manifest = b'{"path":"a.stp","size_bytes":11}\n'
        (old / "manifest.txt").write_bytes(old_manifest)
        metadata = {"key": key, "source_artifact_id": self.dataset["artifact_id"],
                    "artifact_id": self.dataset["artifact_id"], "root": "inputs", "version": "release-1",
                    "manifest_sha256": hashlib.sha256(old_manifest).hexdigest(), "file_count": 1}
        (old / "dataset.json").write_bytes(json_bytes(metadata))
        prepared = self.store.catalog.prepare_dataset(self.dataset["artifact_id"], "inputs")
        self.assertNotEqual(key, prepared["key"])
        self.assertEqual(2, prepared["schema_version"])
        self.assertNotEqual(self.dataset["artifact_id"], prepared["artifact_id"])
        self.assertEqual("sha256:" + prepared["manifest_sha256"], prepared["version"])
        self.assertEqual(old_manifest, (old / "manifest.txt").read_bytes())
        self.assertEqual(metadata, json.loads((old / "dataset.json").read_bytes()))
        with patch.object(self.store.catalog, "_prepare_dataset", side_effect=AssertionError("schema 2 cache missed")):
            self.assertEqual(prepared, self.store.catalog.prepare_dataset(self.dataset["artifact_id"], "inputs"))

    def test_multiple_entries_require_selection_and_list_all_options(self):
        program = self.upload_zip("program", "multi", {"one.exe": b"one", "two.exe": b"two"})
        listing = self.store.resources(program["artifact_id"])
        self.assertEqual(["one.exe", "two.exe"], listing["executables"])
        body = self.body()
        body["plugins"]["coverage"]["engine_dlls"] = []
        body["plugins"]["coverage"]["resources"]["program"] = {"artifact_id": program["artifact_id"]}
        with self.assertRaisesRegex(DeploymentError, "多个入口"):
            self.store.save(None, body)
        self.assertEqual([], self.store.list())
        body["plugins"]["coverage"]["resources"]["program"]["entry"] = "two.exe"
        saved = self.store.save(None, body)
        self.assertTrue(self.store.compile(saved)["config_patch"]["plugins"]["coverage"]["target_exe"].endswith("/two.exe"))

    def test_stale_revision_and_invalid_changes_never_overwrite_profile(self):
        saved = self.store.save(None, self.body())
        changed = copy.deepcopy(saved)
        changed["name"] = "新版方案"
        latest = self.store.save(saved["profile_id"], changed)
        with self.assertRaises(DeploymentError) as caught:
            self.store.save(saved["profile_id"], saved)
        self.assertEqual(409, caught.exception.status)
        self.assertEqual(latest["revision"], caught.exception.current_revision)
        invalid = copy.deepcopy(latest)
        invalid["plugins"]["coverage"]["resources"]["program"]["entry"] = "missing.exe"
        with self.assertRaises(DeploymentError):
            self.store.save(saved["profile_id"], invalid)
        self.assertEqual(latest, self.store.get(saved["profile_id"]))
        with self.assertRaises(DeploymentError) as missing:
            self.store.get("missing")
        self.assertEqual(404, missing.exception.status)

    def test_concurrent_profile_updates_use_compare_and_swap(self):
        saved = self.store.save(None, self.body())
        results = []
        barrier = threading.Barrier(3)
        def update():
            barrier.wait()
            try:
                self.store.save(saved["profile_id"], {**saved, "description": "changed"})
                results.append(200)
            except DeploymentError as exc:
                results.append(exc.status)
        threads = [threading.Thread(target=update) for _ in range(2)]
        for thread in threads:
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join(10)
        self.assertEqual([200, 409], sorted(results))

    def test_wrong_resource_kinds_entries_and_nonboolean_execution_are_rejected(self):
        for role, change in (("program", {"artifact_id": self.dataset["artifact_id"], "entry": "inputs/a.stp"}),
                             ("program", {"artifact_id": self.program["artifact_id"], "entry": "../engine.exe"}),
                             ("dataset", {"artifact_id": self.dataset["artifact_id"], "root": "../inputs"}),
                             ("dataset", {"artifact_id": self.dataset["artifact_id"], "root": "missing"})):
            with self.subTest(role=role, change=change):
                body = self.body()
                body["plugins"]["coverage"]["resources"][role] = change
                with self.assertRaises(DeploymentError):
                    self.store.save(None, body)
        for value in (None, 0, "false"):
            body = self.body()
            body["plugins"]["coverage"]["dry_run"] = value
            with self.assertRaises(DeploymentError):
                self.store.save(None, body)
        body = self.body()
        body["config_patch"] = {"client": {"worker_id": "forged"}}
        with self.assertRaises(DeploymentError):
            self.store.save(None, body)

    def test_stpcompare_compiles_two_programs_without_disabling_other_plugins(self):
        body = self.body()
        resources = body["plugins"]["coverage"]["resources"]
        body["plugins"] = {"stpcompare": {"dry_run": False, "resources": {
            "plugin": resources["plugin"], "dataset": resources["dataset"],
            "baseline": resources["program"], "candidate": resources["program"]}}}
        saved = self.store.save(None, body)
        prepared = self.store.compile(saved)
        config = prepared["config_patch"]["plugins"]
        self.assertEqual({"stpcompare"}, set(config))
        self.assertEqual("", config["stpcompare"]["dataset_version_file"])
        for role in ("baseline", "candidate"):
            self.assertIn("{stp}", config["stpcompare"]["programs"][role]["arguments"])
            self.assertTrue(config["stpcompare"]["programs"][role]["version"].startswith("sha256:"))

    def test_zip_traversal_case_duplicates_symlinks_and_file_directory_conflicts_rejected(self):
        invalid_archives = [self.zip_bytes({"../bad.exe": b"bad"}),
                            self.zip_bytes({"A.exe": b"a", "a.exe": b"b"}),
                            self.zip_bytes({"Dir/one.exe": b"a", "dir/two.exe": b"b"}),
                            self.zip_bytes({"dir": b"file", "dir/tool.exe": b"exe"}),
                            self.zip_bytes({"nul.exe": b"exe"})]
        linked = io.BytesIO()
        with zipfile.ZipFile(linked, "w") as archive:
            info = zipfile.ZipInfo("linked.exe")
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, "elsewhere")
        invalid_archives.append(linked.getvalue())
        reparse = io.BytesIO()
        with zipfile.ZipFile(reparse, "w") as archive:
            info = zipfile.ZipInfo("reparse.exe")
            info.create_system = 0
            info.external_attr = 0x400
            archive.writestr(info, b"windows reparse entry")
        invalid_archives.append(reparse.getvalue())
        for index, payload in enumerate(invalid_archives):
            with self.subTest(index=index):
                artifact = self.upload("program", "invalid-" + str(index), payload, "invalid.zip")
                with self.assertRaises(DeploymentError):
                    self.store.resources(artifact["artifact_id"])

    def test_corrupt_deflate_payload_is_a_validation_error_and_does_not_save_profile(self):
        raw = bytearray(self.zip_bytes({"a.stp": b"geometry" * 10}))
        filename_size = int.from_bytes(raw[26:28], "little")
        extra_size = int.from_bytes(raw[28:30], "little")
        # Set the first DEFLATE block type to the reserved value 3, while
        # preserving a valid ZIP directory and uploaded artifact SHA-256.
        raw[30 + filename_size + extra_size] = 7
        data = self.upload("dataset", "corrupt-stream", bytes(raw), "broken.zip")
        body = self.body()
        body["plugins"]["coverage"]["resources"]["dataset"] = {"artifact_id": data["artifact_id"], "root": ""}
        with self.assertRaises(DeploymentError) as caught:
            self.store.save(None, body)
        self.assertEqual(422, caught.exception.status)
        self.assertEqual([], self.store.list())
        self.assertEqual([], list((self.artifacts.root / "managed-datasets").rglob("*.tmp")))

    def test_invalid_version_and_schedule_are_rejected(self):
        bad_data = self.upload_zip("dataset", "bad-version", {"a.stp": b"a", "version.txt": b"\n"})
        body = self.body()
        body["plugins"]["coverage"]["resources"]["dataset"] = {"artifact_id": bad_data["artifact_id"], "root": ""}
        with self.assertRaisesRegex(DeploymentError, "version.txt"):
            self.store.save(None, body)
        for schedule in ({"task_threads": True}, {"task_threads": 0},
                         {"update_windows": [{"start": "24:00", "end": "01:00"}]},
                         {"execution_windows": [{"start": "08:00", "end": "18:00", "days": [0]}]}):
            body = self.body()
            body["schedule"] = schedule
            with self.assertRaises(DeploymentError):
                self.store.save(None, body)


if __name__ == "__main__":
    unittest.main()
