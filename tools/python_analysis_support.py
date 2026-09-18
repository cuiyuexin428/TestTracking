"""Verify the offline analysis wheelhouse and its installed runtime."""
from __future__ import annotations

import argparse
import hashlib
from importlib import import_module, metadata
import json
from pathlib import Path
import platform
import re
import struct
import sys
from typing import Any


SCHEMA_VERSION = 1
TARGET = {
    "implementation": "cp",
    "python_version": "3.12",
    "abi": "cp312",
    "platform": "win_amd64",
}
REQUIRED_IMPORTS = {
    "numpy": "numpy",
    "pandas": "pandas",
    "openpyxl": "openpyxl",
    "matplotlib": "matplotlib",
    "plotly": "plotly",
}
PIN_RE = re.compile(r"^([A-Za-z0-9_.-]+)==([^\s;]+)$")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalize_package(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def read_requirements(path: Path) -> dict[str, str]:
    packages: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = PIN_RE.fullmatch(line)
        if not match:
            raise ValueError(f"Requirement must use an exact == pin at {path}:{line_number}: {line}")
        name, version = normalize_package(match.group(1)), match.group(2)
        if name in packages:
            raise ValueError(f"Duplicate requirement at {path}:{line_number}: {name}")
        packages[name] = version
    missing = sorted(set(REQUIRED_IMPORTS) - set(packages))
    if missing:
        raise ValueError(f"Missing analysis requirements: {', '.join(missing)}")
    return packages


def artifact_paths(directory: Path) -> list[Path]:
    required = [directory / "requirements.txt", directory / "pip.pyz"]
    wheels = sorted(directory.glob("*.whl"), key=lambda item: item.name.lower())
    missing = [path.name for path in required if not path.is_file()]
    if missing:
        raise ValueError(f"Wheelhouse is missing: {', '.join(missing)}")
    if not wheels:
        raise ValueError("Wheelhouse contains no wheels")
    return required + wheels


def build_manifest(directory: Path) -> dict[str, Any]:
    requirements = directory / "requirements.txt"
    read_requirements(requirements)
    files = [
        {"name": path.name, "size": path.stat().st_size, "sha256": sha256(path)}
        for path in artifact_paths(directory)
    ]
    return {
        "schema_version": SCHEMA_VERSION,
        "target": TARGET,
        "requirements_sha256": sha256(requirements),
        "files": files,
    }


def write_json_atomic(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def verify_wheelhouse(directory: Path) -> dict[str, Any]:
    manifest_path = directory / "wheelhouse-manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"Wheelhouse manifest not found: {manifest_path}") from exc
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("Unsupported wheelhouse manifest schema")
    if manifest.get("target") != TARGET:
        raise ValueError(f"Wheelhouse target must be {TARGET}")
    expected_entries = manifest.get("files")
    if not isinstance(expected_entries, list):
        raise ValueError("Wheelhouse manifest files must be an array")
    actual_paths = artifact_paths(directory)
    actual_names = {path.name for path in actual_paths}
    expected_names = {entry.get("name") for entry in expected_entries if isinstance(entry, dict)}
    if actual_names != expected_names:
        missing = sorted(str(name) for name in expected_names - actual_names)
        extra = sorted(actual_names - expected_names)
        raise ValueError(f"Wheelhouse file set mismatch; missing={missing}, extra={extra}")
    for entry in expected_entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError("Invalid wheelhouse file entry")
        path = directory / entry["name"]
        if path.stat().st_size != entry.get("size") or sha256(path) != entry.get("sha256"):
            raise ValueError(f"Wheelhouse artifact failed SHA256 verification: {path.name}")
    requirements = directory / "requirements.txt"
    if sha256(requirements) != manifest.get("requirements_sha256"):
        raise ValueError("Requirements hash does not match the wheelhouse manifest")
    read_requirements(requirements)
    return manifest


def verify_runtime(requirements_path: Path, output: Path) -> dict[str, Any]:
    expected = read_requirements(requirements_path)
    installed: dict[str, str] = {}
    for name, version in expected.items():
        try:
            actual = metadata.version(name)
        except metadata.PackageNotFoundError as exc:
            raise RuntimeError(f"Required package is not installed: {name}=={version}") from exc
        if actual != version:
            raise RuntimeError(f"Installed version mismatch for {name}: expected {version}, got {actual}")
        installed[name] = actual
    imported = []
    for distribution, module in REQUIRED_IMPORTS.items():
        import_module(module)
        imported.append(distribution)
    result = {
        "schema_version": SCHEMA_VERSION,
        "python": platform.python_version(),
        "architecture": platform.machine(),
        "requirements_sha256": sha256(requirements_path),
        "packages": installed,
        "verified_imports": sorted(imported),
    }
    write_json_atomic(output, result)
    return result


def verify_target() -> None:
    if (sys.implementation.name != "cpython" or sys.platform != "win32"
            or sys.version_info[:2] != (3, 12) or struct.calcsize("P") != 8):
        raise RuntimeError("Analysis wheelhouse requires Windows x64 CPython 3.12")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("verify-target")
    verify_parser = subparsers.add_parser("verify-wheelhouse")
    verify_parser.add_argument("--wheelhouse", type=Path, required=True)
    runtime_parser = subparsers.add_parser("verify-runtime")
    runtime_parser.add_argument("--requirements", type=Path, required=True)
    runtime_parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args(argv)
    try:
        if options.command == "verify-target":
            verify_target()
            print("Python target verification passed")
        elif options.command == "verify-wheelhouse":
            verify_wheelhouse(options.wheelhouse.resolve())
            print("Analysis wheelhouse verification passed")
        else:
            verify_runtime(options.requirements.resolve(), options.out.resolve())
            print(f"Analysis runtime verification passed: {options.out.resolve()}")
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
