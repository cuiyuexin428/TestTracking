from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.artifacts import ArtifactError, ArtifactStore
from result_service.app.config import ServiceConfig
from result_service.app.server import Handler, ResultHTTPServer, ResultService


class ArtifactStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="artifact-store-")
        self.root = Path(self.temporary.name)
        self.import_root = self.root / "imports"
        self.import_root.mkdir()
        self.store = ArtifactStore(self.root / "repository", (self.import_root,))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def upload(self, data: bytes, **overrides):
        fields = {"kind": "program", "name": "coverage-engine", "version": "1.0.0",
                  "filename": "coverage.exe", "metadata": {"architecture": "windows-x64"}}
        fields.update(overrides)
        return self.store.upload(io.BytesIO(data), len(data), **fields)

    def test_content_is_addressed_by_sha256_and_survives_reopen(self) -> None:
        payload = b"portable executable placeholder"
        artifact, created = self.upload(payload)
        digest = hashlib.sha256(payload).hexdigest()

        self.assertTrue(created)
        self.assertEqual(digest, artifact["sha256"])
        self.assertEqual(payload, self.store.object_path(digest).read_bytes())
        self.assertEqual([artifact], ArtifactStore(
            self.root / "repository", (self.import_root,)
        ).list())

    def test_same_version_is_idempotent_but_cannot_change_content(self) -> None:
        first, created = self.upload(b"first")
        repeated, repeated_created = self.upload(b"first", metadata={"ignored": True})

        self.assertTrue(created)
        self.assertFalse(repeated_created)
        self.assertEqual(first, repeated)
        with self.assertRaisesRegex(ArtifactError, "different content") as raised:
            self.upload(b"second")
        self.assertEqual(409, raised.exception.status)
        self.assertEqual(1, len(self.store.list()))

    def test_idempotent_upload_repairs_a_missing_object_but_rejects_corruption(self) -> None:
        payload = b"immutable"
        artifact, _ = self.upload(payload)
        path = self.store.object_path(artifact["sha256"])
        path.unlink()

        repaired, created = self.upload(payload)
        self.assertFalse(created)
        self.assertEqual(artifact, repaired)
        self.assertEqual(payload, path.read_bytes())
        path.write_bytes(b"x" * len(payload))
        with self.assertRaisesRegex(ArtifactError, "corrupted") as raised:
            self.upload(payload)
        self.assertEqual(500, raised.exception.status)

    def test_identical_content_is_deduplicated_across_logical_artifacts(self) -> None:
        data = b"shared archive"
        first, _ = self.upload(data)
        second, _ = self.upload(
            data, kind="dataset", name="coverage-input", version="d1", filename="data.zip"
        )

        self.assertNotEqual(first["artifact_id"], second["artifact_id"])
        self.assertEqual(first["sha256"], second["sha256"])
        objects = [path for path in (self.root / "repository" / "objects").rglob("*")
                   if path.is_file()]
        self.assertEqual(1, len(objects))

    def test_digest_mismatch_does_not_publish_content_or_metadata(self) -> None:
        with self.assertRaisesRegex(ArtifactError, "does not match") as raised:
            self.upload(b"wrong", expected_sha256="0" * 64)

        self.assertEqual(422, raised.exception.status)
        self.assertEqual([], self.store.list())
        self.assertEqual([], list((self.root / "repository" / "tmp").iterdir()))

    def test_local_registration_is_confined_to_configured_roots(self) -> None:
        source = self.import_root / "coverage-plugin.zip"
        source.write_bytes(b"plugin package")
        artifact, created = self.store.register(
            str(source), kind="plugin", name="coverage", version="2.0"
        )

        self.assertTrue(created)
        self.assertEqual(source.name, artifact["filename"])
        outside = self.root / "outside.zip"
        outside.write_bytes(b"private")
        with self.assertRaisesRegex(ArtifactError, "outside") as raised:
            self.store.register(str(outside), kind="plugin", name="outside", version="1")
        self.assertEqual(403, raised.exception.status)


class ArtifactHTTPTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="artifact-http-")
        root = Path(self.temporary.name)
        self.import_root = root / "imports"
        self.import_root.mkdir()
        config = ServiceConfig(
            workspace=str(root), artifact_store_dir=str(root / "artifacts"),
            artifact_import_roots=(str(self.import_root),), providers={},
        )
        self.service = ResultService(config)
        handler = type("ArtifactHandler", (Handler,), {
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

    def request(self, path: str, method: str = "GET", data: bytes | None = None,
                headers: dict[str, str] | None = None):
        request = Request(self.base + path, method=method, data=data, headers=headers or {})
        try:
            response = urlopen(request, timeout=5)
        except HTTPError as exc:
            response = exc
        with response:
            return response.status, dict(response.headers), response.read()

    def test_raw_upload_list_metadata_and_stream_download(self) -> None:
        content = b"dataset archive\x00\x01"
        query = urlencode({"kind": "dataset", "name": "stp-input", "version": "d1",
                           "filename": "stp-data.zip",
                           "metadata": json.dumps({"format": "zip"})})
        status, _, body = self.request(
            "/api/artifacts/upload?" + query, "POST", content,
            {"Content-Type": "application/octet-stream",
             "X-Content-SHA256": hashlib.sha256(content).hexdigest()},
        )
        artifact = json.loads(body)

        self.assertEqual(201, status)
        self.assertEqual({"format": "zip"}, artifact["metadata"])
        listed = json.loads(self.request("/api/artifacts?kind=dataset&name=stp-input")[2])
        self.assertEqual([artifact], listed["artifacts"])
        detail = json.loads(self.request("/api/artifacts/" + artifact["artifact_id"])[2])
        self.assertEqual(artifact, detail)
        download_status, headers, downloaded = self.request(artifact["download_url"])
        self.assertEqual(200, download_status)
        self.assertEqual(content, downloaded)
        self.assertEqual('"' + artifact["sha256"] + '"', headers["ETag"])
        self.assertIn("stp-data.zip", headers["Content-Disposition"])

    def test_register_endpoint_and_immutable_version_conflict(self) -> None:
        source = self.import_root / "worker.exe"
        source.write_bytes(b"version one")
        body = {"source_path": str(source), "kind": "program", "name": "worker",
                "version": "1", "metadata": {"role": "baseline"}}
        status, _, first_body = self.request(
            "/api/artifacts/register", "POST", json.dumps(body).encode(),
            {"Content-Type": "application/json"},
        )
        self.assertEqual(201, status)

        source.write_bytes(b"changed")
        conflict, _, conflict_body = self.request(
            "/api/artifacts/register", "POST", json.dumps(body).encode(),
            {"Content-Type": "application/json"},
        )
        self.assertEqual(409, conflict)
        self.assertIn("different content", json.loads(conflict_body)["error"])
        self.assertEqual(b"version one", self.request(json.loads(first_body)["download_url"])[2])

    def test_upload_rejects_wrong_digest_and_cross_origin_request(self) -> None:
        query = urlencode({"kind": "plugin", "name": "coverage", "version": "1",
                           "filename": "coverage.zip"})
        path = "/api/artifacts/upload?" + query
        mismatch = self.request(path, "POST", b"payload", {
            "Content-Type": "application/octet-stream", "X-Content-SHA256": "0" * 64,
        })
        self.assertEqual(422, mismatch[0])
        rejected = self.request(path, "POST", b"payload", {
            "Content-Type": "application/octet-stream", "Origin": "http://unrelated.invalid",
        })
        self.assertEqual(403, rejected[0])
        self.assertEqual([], json.loads(self.request("/api/artifacts")[2])["artifacts"])


if __name__ == "__main__":
    unittest.main()
