"""Derive execution identities from the program artifacts actually referenced.

The existing plugin version handshake carries this identity. Human catalog
labels and machine-local cache paths do not participate in it.
"""
from __future__ import annotations

import copy
import hashlib
import json
import re
import zipfile
from pathlib import PurePosixPath
from typing import Any

from .artifacts import ArtifactError, ArtifactStore


REFERENCE = re.compile(r"\{artifact:([A-Za-z0-9][A-Za-z0-9._-]{0,127})\}(?:[/\\](.*))?")


def bind_programs(artifacts: ArtifactStore, components: list[dict[str, str]],
                  patch: dict[str, Any], *, validate_files: bool = True
                  ) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, Any]]:
    """Return a copy with server-derived program versions and scheduler fields.

    Traditional local executable paths remain supported and are not advertised
    as content-bound. An artifact-backed executable is always bound at publish.
    """
    proposed = copy.deepcopy(patch)
    slots = {item["slot"]: artifacts.get(item["artifact_id"]) for item in components}
    bindings: list[dict[str, Any]] = []
    expected: dict[str, Any] = {}

    def reference(value: Any, *, executable: bool = False) -> tuple[str, dict, str] | None:
        match = REFERENCE.fullmatch(value) if isinstance(value, str) else None
        if match is None:
            return None
        slot, relative = match.groups()
        record = slots.get(slot)
        if record is None:
            raise ArtifactError(f"unknown program artifact slot: {slot}")
        if executable and record["kind"] != "program":
            raise ArtifactError(f"executable and program dependencies must use a program artifact: {slot}")
        relative = (relative or "").replace("\\", "/")
        parts = PurePosixPath(relative).parts
        if ".." in parts or relative.startswith("/") or ":" in relative:
            raise ArtifactError("program path must remain inside its artifact directory")
        relative = "/".join(part for part in parts if part != ".")
        if executable and validate_files:
            if not relative:
                raise ArtifactError("program executable requires its filename inside the artifact")
            source = artifacts.object_path(record["sha256"])
            if record["filename"].lower().endswith(".zip"):
                try:
                    with zipfile.ZipFile(source) as archive:
                        entries = {item.filename.replace("\\", "/") for item in archive.infolist()
                                   if not item.is_dir()}
                    if relative not in entries:
                        raise ArtifactError(f"executable is not in program artifact: {relative}")
                except (OSError, zipfile.BadZipFile) as exc:
                    raise ArtifactError(f"program artifact is not a readable ZIP: {record['name']}") from exc
            elif relative != record["filename"]:
                raise ArtifactError("single-file program path must use the uploaded filename")
        return slot, record, relative

    def dependency(value: Any) -> Any:
        if isinstance(value, list):
            return [dependency(item) for item in value]
        bound = reference(value)
        if bound:
            _, record, relative = bound
            return {"artifact_sha256": record["sha256"], "path": relative}
        # Unmanaged dependencies retain their configured path in the identity;
        # their bytes can only be tracked when uploaded as part of a program.
        return value

    def bind(plugin: str, role: str, executable: Any, semantics: dict[str, Any],
             destination: dict[str, Any], version_key: str, scheduler_key: str) -> None:
        bound = reference(executable, executable=True)
        if bound is None:
            return
        slot, record, relative = bound
        identity = {"schema_version": 1, "artifact_sha256": record["sha256"],
                    "executable": relative, "execution": semantics}
        encoded = json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        version = "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()
        destination[version_key] = version
        expected.setdefault(plugin, {})[scheduler_key] = version
        bindings.append({"plugin": plugin, "role": role, "slot": slot,
                         "artifact_id": record["artifact_id"], "artifact_sha256": record["sha256"],
                         "version": version, "executable": relative})

    plugins = proposed.get("plugins", {})
    if not isinstance(plugins, dict):
        raise ArtifactError("config_patch.plugins must be an object")
    for name, config in plugins.items():
        if not isinstance(config, dict):
            raise ArtifactError(f"config_patch.plugins.{name} must be an object")
        if config.get("enabled") is False:
            continue
        if name == "coverage":
            if not REFERENCE.fullmatch(str(config.get("target_exe", ""))):
                continue
            required = ("dry_run", "store_cov", "program_dir", "opencppcoverage", "engine_dlls")
            if validate_files and any(key not in config for key in required):
                raise ArtifactError("artifact-backed coverage requires explicit dry_run, store_cov, program_dir, "
                                    "opencppcoverage and engine_dlls; complete the deployment wizard")
            if validate_files and (not isinstance(config["dry_run"], bool)
                                   or not isinstance(config["store_cov"], bool)
                                   or not isinstance(config["engine_dlls"], list)
                                   or not all(isinstance(item, str) for item in config["engine_dlls"])):
                raise ArtifactError("coverage dry_run must be boolean and engine_dlls an array of strings")
            execution = {key: dependency(config.get(key, default)) for key, default in (
                ("program_dir", ""), ("opencppcoverage", "OpenCppCoverage.exe"), ("engine_dlls", []))}
            execution["dry_run"] = config.get("dry_run", False)
            execution["store_cov"] = config.get("store_cov", False)
            bind(name, "program", config.get("target_exe"), execution, config,
                 "program_version", "expected_program_version")
        elif name == "stpcompare":
            programs = config.get("programs", {})
            if not isinstance(programs, dict):
                raise ArtifactError("stpcompare.programs must be an object")
            for role in ("baseline", "candidate"):
                program = programs.get(role, {})
                if not isinstance(program, dict):
                    raise ArtifactError(f"stpcompare.programs.{role} must be an object")
                if not REFERENCE.fullmatch(str(program.get("executable", ""))):
                    continue
                if validate_files and (not isinstance(config.get("dry_run"), bool) or "arguments" not in program):
                    raise ArtifactError("artifact-backed stpcompare requires explicit dry_run and program arguments")
                arguments = program.get("arguments", ["--input", "{stp}", "--output-dir", "{outdir}"])
                if not isinstance(arguments, list) or not all(isinstance(item, str) for item in arguments):
                    raise ArtifactError(f"stpcompare.programs.{role}.arguments must be an array of strings")
                bind(name, role, program.get("executable"),
                     {"arguments": dependency(arguments), "dry_run": config.get("dry_run", False)}, program,
                     "version", f"expected_{role}_program_version")
    return proposed, bindings, expected
