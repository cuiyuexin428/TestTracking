"""Immutable, content-addressed deployment artifacts for the management service."""
from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
import threading
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO


ARTIFACT_KINDS = frozenset({"plugin", "program", "dataset"})
_DIGEST = re.compile(r"[0-9a-f]{64}")
_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
_VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,127}")


class ArtifactError(RuntimeError):
    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.status = status


class ArtifactStore:
    """Store immutable artifact metadata and de-duplicated content on local disk."""

    def __init__(self, root: str | Path, import_roots: tuple[str | Path, ...] = ()):
        self.root = Path(root).resolve()
        self.objects_dir = self.root / "objects"
        self.temporary_dir = self.root / "tmp"
        self.catalog_path = self.root / "catalog.json"
        self.import_roots = tuple(Path(path).resolve() for path in import_roots)
        self._lock = threading.RLock()
        self.objects_dir.mkdir(parents=True, exist_ok=True)
        self.temporary_dir.mkdir(parents=True, exist_ok=True)

    def list(self, filters: dict[str, str] | None = None) -> list[dict[str, Any]]:
        filters = filters or {}
        allowed = {"kind", "name", "version", "sha256"}
        if set(filters) - allowed:
            raise ArtifactError("unsupported artifact filter")
        with self._lock:
            artifacts = self._read_catalog()["artifacts"]
        result = [artifact for artifact in artifacts if all(
            str(artifact.get(key, "")) == value for key, value in filters.items() if value
        )]
        return sorted(result, key=lambda item: (item["created_at"], item["artifact_id"]), reverse=True)

    def get(self, artifact_id: str) -> dict[str, Any] | None:
        if not re.fullmatch(r"[0-9a-f]{32}", artifact_id):
            return None
        with self._lock:
            return next((item for item in self._read_catalog()["artifacts"]
                         if item["artifact_id"] == artifact_id), None)

    def upload(self, stream: BinaryIO, length: int, *, kind: Any, name: Any,
               version: Any, filename: Any, metadata: Any = None,
               expected_sha256: Any = None) -> tuple[dict[str, Any], bool]:
        fields = self._validate_fields(kind, name, version, filename, metadata, expected_sha256)
        if isinstance(length, bool) or not isinstance(length, int) or length < 0:
            raise ArtifactError("Content-Length must be a nonnegative integer")
        temporary = self._new_temporary_path()
        digest = hashlib.sha256()
        remaining = length
        try:
            with temporary.open("xb") as output:
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ArtifactError("upload ended before Content-Length bytes were received")
                    output.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
                output.flush()
                os.fsync(output.fileno())
            actual = digest.hexdigest()
            if fields["expected_sha256"] and actual != fields["expected_sha256"]:
                raise ArtifactError("uploaded content SHA-256 does not match X-Content-SHA256", 422)
            return self._commit(temporary, length, actual, **fields)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise

    def register(self, source_path: Any, *, kind: Any, name: Any, version: Any,
                 filename: Any = None, metadata: Any = None,
                 expected_sha256: Any = None) -> tuple[dict[str, Any], bool]:
        if not isinstance(source_path, str) or not source_path.strip():
            raise ArtifactError("source_path must be a nonempty string")
        source = Path(source_path).resolve()
        if not self.import_roots:
            raise ArtifactError("local artifact registration is disabled", 403)
        if not any(source == root or root in source.parents for root in self.import_roots):
            raise ArtifactError("source_path is outside configured artifact_import_roots", 403)
        if not source.is_file():
            raise ArtifactError("source_path is not a readable file", 404)
        selected_filename = filename if filename is not None else source.name
        fields = self._validate_fields(
            kind, name, version, selected_filename, metadata, expected_sha256
        )
        try:
            with source.open("rb") as input_file:
                return self.upload(
                    input_file, source.stat().st_size, kind=fields["kind"], name=fields["name"],
                    version=fields["version"], filename=fields["filename"],
                    metadata=fields["metadata"], expected_sha256=fields["expected_sha256"],
                )
        except ArtifactError:
            raise
        except OSError as exc:
            raise ArtifactError("source_path cannot be read", 404) from exc

    def object_path(self, sha256: str) -> Path:
        digest = str(sha256).lower()
        if not _DIGEST.fullmatch(digest):
            raise ArtifactError("invalid artifact SHA-256")
        path = self.objects_dir / digest[:2] / digest
        if not path.is_file():
            raise ArtifactError("artifact content not found", 404)
        return path

    def _commit(self, temporary: Path, size: int, sha256: str, *, kind: str, name: str,
                version: str, filename: str, metadata: dict[str, Any],
                expected_sha256: str | None) -> tuple[dict[str, Any], bool]:
        del expected_sha256
        with self._lock:
            catalog = self._read_catalog()
            existing = next((item for item in catalog["artifacts"] if
                             (item["kind"], item["name"], item["version"])
                             == (kind, name, version)), None)
            if existing:
                if existing["sha256"] != sha256:
                    raise ArtifactError(
                        "artifact kind/name/version already refers to different content", 409
                    )

            destination = self.objects_dir / sha256[:2] / sha256
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists():
                if (destination.stat().st_size != size
                        or self._file_sha256(destination) != sha256):
                    raise ArtifactError("artifact repository contains corrupted content", 500)
                temporary.unlink(missing_ok=True)
            else:
                os.replace(temporary, destination)
            if existing:
                return dict(existing), False
            record = {
                "artifact_id": uuid.uuid4().hex,
                "kind": kind,
                "name": name,
                "version": version,
                "filename": filename,
                "size_bytes": size,
                "sha256": sha256,
                "created_at": datetime.now(timezone.utc).isoformat(),
                "metadata": metadata,
                "download_url": f"/api/artifacts/objects/{sha256}",
            }
            catalog["artifacts"].append(record)
            self._write_catalog(catalog)
            return dict(record), True

    def _read_catalog(self) -> dict[str, Any]:
        if not self.catalog_path.exists():
            return {"schema_version": 1, "artifacts": []}
        try:
            value = json.loads(self.catalog_path.read_text(encoding="utf-8"))
        except (OSError, ValueError, UnicodeError) as exc:
            raise ArtifactError("artifact catalog cannot be read", 500) from exc
        if (not isinstance(value, dict) or value.get("schema_version") != 1
                or not isinstance(value.get("artifacts"), list)):
            raise ArtifactError("artifact catalog has an unsupported format", 500)
        return value

    def _write_catalog(self, catalog: dict[str, Any]) -> None:
        data = (json.dumps(catalog, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        descriptor, name = tempfile.mkstemp(prefix="catalog-", suffix=".tmp", dir=self.root)
        temporary = Path(name)
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.catalog_path)
        finally:
            temporary.unlink(missing_ok=True)

    def _new_temporary_path(self) -> Path:
        return self.temporary_dir / f"upload-{uuid.uuid4().hex}.tmp"

    @staticmethod
    def _file_sha256(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
        return digest.hexdigest()

    @staticmethod
    def _validate_fields(kind: Any, name: Any, version: Any, filename: Any,
                         metadata: Any, expected_sha256: Any) -> dict[str, Any]:
        if kind not in ARTIFACT_KINDS:
            raise ArtifactError("kind must be plugin, program, or dataset")
        if not isinstance(name, str) or not _IDENTIFIER.fullmatch(name):
            raise ArtifactError("name must use 1-128 ASCII letters, digits, dots, underscores, or hyphens")
        if not isinstance(version, str) or not _VERSION.fullmatch(version):
            raise ArtifactError("version must use 1-128 ASCII letters, digits, dots, underscores, pluses, or hyphens")
        if (not isinstance(filename, str) or not filename or len(filename) > 255
                or filename in {".", ".."} or Path(filename).name != filename
                or any(ord(character) < 32 for character in filename)):
            raise ArtifactError("filename must be a plain file name of at most 255 characters")
        if metadata is None:
            metadata = {}
        if not isinstance(metadata, dict):
            raise ArtifactError("metadata must be an object")
        try:
            encoded_metadata = json.dumps(metadata, ensure_ascii=False).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise ArtifactError("metadata must contain JSON values") from exc
        if len(encoded_metadata) > 64 * 1024:
            raise ArtifactError("metadata is too large")
        expected = None
        if expected_sha256 not in (None, ""):
            expected = str(expected_sha256).lower()
            if not _DIGEST.fullmatch(expected):
                raise ArtifactError("X-Content-SHA256 must be a lowercase or uppercase SHA-256")
        return {"kind": kind, "name": name, "version": version, "filename": filename,
                "metadata": metadata, "expected_sha256": expected}
