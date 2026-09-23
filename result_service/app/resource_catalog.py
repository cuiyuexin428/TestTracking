"""Inspect immutable resource files and prepare managed STP dataset bundles."""
from __future__ import annotations

import copy
import hashlib
import json
import os
import re
import shutil
import stat
import tempfile
import threading
import zipfile
import zlib
from pathlib import Path, PurePosixPath
from typing import Any

from .artifacts import ArtifactError, ArtifactStore
from .deployments import DeploymentError


MAX_ENTRIES = 200_000
MAX_FILE_BYTES = 16 * 1024 ** 3
MAX_EXPANDED_BYTES = 2 * 1024 ** 4
DATASET_SCHEMA_VERSION = 2
_RESERVED = re.compile(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?")


def relative_path(value: Any, *, root: bool = False) -> str:
    if root and value == "":
        return ""
    if (not isinstance(value, str) or not value or "\\" in value
            or any(ord(char) < 32 for char in value)):
        raise DeploymentError("资源入口必须是包内相对路径", 422)
    if any(part in {"", ".", ".."} or part.endswith((" ", "."))
           or any(char in part for char in ':*?"<>|') or _RESERVED.fullmatch(part)
           for part in value.split("/")):
        raise DeploymentError("资源包包含不安全的路径：" + value, 422)
    return value


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=path.name + "-", suffix=".tmp", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


class ResourceCatalog:
    def __init__(self, artifacts: ArtifactStore):
        self.artifacts = artifacts
        self._lock = threading.RLock()
        self._inspections: dict[tuple, dict] = {}

    def _artifact(self, artifact_id: Any) -> dict:
        if not isinstance(artifact_id, str):
            raise DeploymentError("请选择有效的资源", 422)
        record = self.artifacts.get(artifact_id)
        if record is None:
            raise DeploymentError("资源不存在：" + artifact_id, 404)
        return record

    def resources(self, artifact_id: Any) -> dict:
        record = self._artifact(artifact_id)
        source = self.artifacts.object_path(record["sha256"])
        details = source.stat()
        if details.st_size != record["size_bytes"]:
            raise DeploymentError("资源仓库文件大小与登记信息不一致", 500)
        key = (record["sha256"], record["filename"], details.st_size, details.st_mtime_ns)
        with self._lock:
            cached = self._inspections.get(key)
            if cached is None:
                cached = self._inspect(source, record)
                self._inspections[key] = cached
            return {"artifact_id": artifact_id, **copy.deepcopy(cached)}

    @staticmethod
    def _inspect(source: Path, record: dict) -> dict:
        files: list[dict] = []
        directories = {""}
        if record["filename"].lower().endswith(".zip"):
            try:
                with zipfile.ZipFile(source) as archive:
                    members = archive.infolist()
                    if not members or len(members) > MAX_ENTRIES:
                        raise DeploymentError(f"资源包项目数量必须在 1 到 {MAX_ENTRIES} 之间", 422)
                    seen: dict[str, bool] = {}
                    total = 0
                    for member in members:
                        name = relative_path(member.orig_filename.rstrip("/") if member.is_dir()
                                             else member.orig_filename)
                        mode = stat.S_IFMT(member.external_attr >> 16)
                        if (mode not in {0, stat.S_IFREG, stat.S_IFDIR} or member.flag_bits & 1
                                or member.external_attr & 0x400):
                            raise DeploymentError("资源包不能包含链接、特殊文件或加密文件", 422)
                        if member.compress_type not in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}:
                            raise DeploymentError("资源包必须使用客户端支持的 ZIP Store 或 Deflate 压缩", 422)
                        normalized = name.casefold()
                        if normalized in seen:
                            raise DeploymentError("资源包包含重复路径：" + name, 422)
                        seen[normalized] = member.is_dir()
                        if member.file_size < 0 or member.file_size > MAX_FILE_BYTES:
                            raise DeploymentError("资源包单文件超过 16 GiB 限制", 422)
                        total += member.file_size
                        if total > MAX_EXPANDED_BYTES:
                            raise DeploymentError("资源包解压后超过 2 TiB 限制", 422)
                        if member.is_dir():
                            directories.add(name)
                        else:
                            files.append({"path": name, "size_bytes": member.file_size})
                        directories.update(str(parent) for parent in PurePosixPath(name).parents
                                           if str(parent) != ".")
                    file_names = {name for name, is_dir in seen.items() if not is_dir}
                    if len({name.casefold() for name in directories}) != len(directories):
                        raise DeploymentError("资源包目录大小写冲突", 422)
                    if any(directory.casefold() in file_names for directory in directories if directory):
                        raise DeploymentError("资源包文件与目录路径冲突", 422)
            except (OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as exc:
                if isinstance(exc, DeploymentError):
                    raise
                raise DeploymentError("资源不是可读取的 ZIP 文件", 422) from exc
        else:
            name = relative_path(record["filename"])
            if record["size_bytes"] > MAX_FILE_BYTES:
                raise DeploymentError("资源单文件超过 16 GiB 限制", 422)
            files = [{"path": name, "size_bytes": record["size_bytes"]}]
        files.sort(key=lambda item: item["path"])
        data_roots = set()
        for item in files:
            if item["path"].lower().endswith(".stp"):
                data_roots.add("")
                data_roots.update(str(parent) for parent in PurePosixPath(item["path"]).parents
                                  if str(parent) != ".")
        return {"files": files, "directories": sorted(directories),
                "executables": [item["path"] for item in files if item["path"].lower().endswith(".exe")],
                "dlls": [item["path"] for item in files if item["path"].lower().endswith(".dll")],
                "dataset_roots": sorted(data_roots)}

    def prepare_dataset(self, artifact_id: str, root: str) -> dict:
        record = self._artifact(artifact_id)
        if record["kind"] != "dataset":
            raise DeploymentError("数据资源必须使用 dataset 制品", 422)
        root = relative_path(root, root=True)
        listing = self.resources(artifact_id)
        if root not in listing["dataset_roots"]:
            raise DeploymentError("所选数据目录中没有 STP 文件", 422)
        # Data content and the selected root fully determine the prepared bundle.
        key = hashlib.sha256(json_bytes({"schema_version": DATASET_SCHEMA_VERSION, "sha256": record["sha256"],
                                         "filename": record["filename"], "root": root})).hexdigest()
        directory = self.artifacts.root / "managed-datasets" / key
        metadata_path = directory / "dataset.json"
        manifest_path = directory / "manifest.txt"
        with self._lock:
            if metadata_path.is_file() and manifest_path.is_file():
                try:
                    cached = json.loads(metadata_path.read_text(encoding="utf-8"))
                    deployed = self.artifacts.get(cached["artifact_id"])
                    if (cached.get("schema_version") == DATASET_SCHEMA_VERSION
                            and cached["key"] == key and deployed is not None
                            and cached["version"] == "sha256:" + cached["manifest_sha256"]
                            and self.artifacts._file_sha256(manifest_path) == cached["manifest_sha256"]):
                        self.artifacts.object_path(deployed["sha256"])
                        return {**cached, "source_artifact_id": artifact_id,
                                "manifest_path": manifest_path.as_posix()}
                except (OSError, ValueError, KeyError, TypeError):
                    pass
            directory.mkdir(parents=True, exist_ok=True)
            return self._prepare_dataset(record, listing, root, key, directory)

    def _prepare_dataset(self, record: dict, listing: dict, root: str, key: str, directory: Path) -> dict:
        source = self.artifacts.object_path(record["sha256"])
        prefix = root + "/" if root else ""
        version_path = prefix + "version.txt"
        if any(item.casefold() == version_path.casefold() for item in listing["directories"]):
            raise DeploymentError("数据版本文件与目录路径冲突", 422)
        version_entry = next((item for item in listing["files"]
                              if item["path"].casefold() == version_path.casefold()), None)
        inputs = [item for item in listing["files"] if item["path"].startswith(prefix)
                  and item["path"].lower().endswith(".stp")]
        descriptor, temporary_name = tempfile.mkstemp(prefix="manifest-", suffix=".tmp", dir=directory)
        temporary = Path(temporary_name)
        archive = None
        try:
            if record["filename"].lower().endswith(".zip"):
                archive = zipfile.ZipFile(source)
            def open_member(name: str):
                return archive.open(name) if archive is not None else source.open("rb")
            version = None
            if version_entry is not None:
                if version_entry["size_bytes"] > 4096:
                    raise DeploymentError("数据 version.txt 不能超过 4096 字节", 422)
                with open_member(version_entry["path"]) as stream:
                    # Retain a human label only; execution identity is always
                    # derived from the actual selected input content below.
                    version = stream.read(4097).decode("utf-8").split("\n", 1)[0].removesuffix("\r")
                if not version.strip() or len(version.encode("utf-8")) > 1024 or any(ord(c) < 32 for c in version):
                    raise DeploymentError("数据 version.txt 首行必须是非空版本（最多 1024 字节）", 422)
            manifest_hash = hashlib.sha256()
            with os.fdopen(descriptor, "wb") as output:
                descriptor = -1
                for item in inputs:
                    digest = hashlib.sha256()
                    size = 0
                    with open_member(item["path"]) as stream:
                        while chunk := stream.read(1024 * 1024):
                            digest.update(chunk)
                            size += len(chunk)
                    if size != item["size_bytes"]:
                        raise DeploymentError("数据包文件长度不一致", 422)
                    row = json_bytes({"path": item["path"][len(prefix):], "size_bytes": size,
                                      "content_sha256": digest.hexdigest()})
                    output.write(row)
                    manifest_hash.update(row)
                output.flush()
                os.fsync(output.fileno())
            manifest_sha = manifest_hash.hexdigest()
            source_version = version
            version = "sha256:" + manifest_sha
            deployed = self._derived_dataset(record, listing, version_path, version, key, open_member, directory)
            manifest_path = directory / "manifest.txt"
            os.replace(temporary, manifest_path)
            result = {"schema_version": DATASET_SCHEMA_VERSION, "key": key,
                      "source_artifact_id": record["artifact_id"], "source_version": source_version,
                      "artifact_id": deployed["artifact_id"], "root": root, "version": version,
                      "manifest_sha256": manifest_sha, "file_count": len(inputs)}
            atomic_write(directory / "dataset.json", json_bytes(result))
            return {**result, "manifest_path": manifest_path.as_posix()}
        except (OSError, ValueError, UnicodeError, zipfile.BadZipFile, zlib.error, RuntimeError, NotImplementedError) as exc:
            if isinstance(exc, (DeploymentError, ArtifactError)):
                raise
            raise DeploymentError("无法读取并准备数据资源", 422) from exc
        finally:
            if descriptor != -1:
                os.close(descriptor)
            if archive is not None:
                archive.close()
            temporary.unlink(missing_ok=True)

    def _derived_dataset(self, record: dict, listing: dict, version_path: str, version: str,
                         key: str, open_member, directory: Path) -> dict:
        descriptor, name = tempfile.mkstemp(prefix="dataset-", suffix=".zip", dir=directory)
        os.close(descriptor)
        temporary = Path(name)
        try:
            with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as output:
                for entry in listing["directories"]:
                    if entry:
                        output.writestr(zipfile.ZipInfo(entry + "/", (1980, 1, 1, 0, 0, 0)), b"")
                for item in listing["files"]:
                    # Windows treats version.txt and VERSION.TXT as the same
                    # file. Replace either spelling once in the derived ZIP.
                    if item["path"].casefold() == version_path.casefold():
                        continue
                    info = zipfile.ZipInfo(item["path"], (1980, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    with open_member(item["path"]) as source, output.open(info, "w", force_zip64=True) as destination:
                        shutil.copyfileobj(source, destination, 1024 * 1024)
                output.writestr(zipfile.ZipInfo(version_path, (1980, 1, 1, 0, 0, 0)), (version + "\n").encode("utf-8"))
            with temporary.open("rb") as stream:
                prepared, _ = self.artifacts.upload(stream, temporary.stat().st_size, kind="dataset",
                    name="managed-dataset-" + key, version="1", filename="dataset.zip",
                    metadata={"managed": True, "source_artifact_id": record["artifact_id"], "dataset_version": version})
            return prepared
        finally:
            temporary.unlink(missing_ok=True)
