"""Download a verified CPython 3.12/Windows x64 analysis wheelhouse."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request

if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
from python_analysis_support import (
    TARGET, build_manifest, read_requirements, verify_wheelhouse, write_json_atomic,
)


PIP_ZIPAPP_URL = "https://bootstrap.pypa.io/pip/pip.pyz"


def download_pip_zipapp(destination: Path) -> None:
    request = urllib.request.Request(PIP_ZIPAPP_URL, headers={"User-Agent": "TestTracking-release-builder"})
    with urllib.request.urlopen(request, timeout=120) as response, destination.open("wb") as stream:
        shutil.copyfileobj(response, stream)


def existing_matches(output: Path, requirements: Path) -> bool:
    try:
        verify_wheelhouse(output)
        return (output / "requirements.txt").read_bytes() == requirements.read_bytes()
    except (OSError, ValueError):
        return False


def prepare(output: Path, requirements: Path, force: bool = False) -> None:
    output = output.resolve()
    read_requirements(requirements)
    if output.exists() and existing_matches(output, requirements) and not force:
        print(f"Analysis wheelhouse already verified: {output}")
        return
    if output.exists() and not force:
        raise RuntimeError(f"Wheelhouse already exists but does not match: {output}; pass --force to replace it")
    if output.exists() and not (output / "wheelhouse-manifest.json").is_file():
        raise RuntimeError(f"Refusing to replace a directory without a wheelhouse manifest: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        shutil.copy2(requirements, staging / "requirements.txt")
        download_pip_zipapp(staging / "pip.pyz")
        command = [
            sys.executable, str(staging / "pip.pyz"), "download",
            "--disable-pip-version-check",
            "--only-binary=:all:",
            "--implementation", TARGET["implementation"],
            "--python-version", TARGET["python_version"].replace(".", ""),
            "--abi", TARGET["abi"],
            "--platform", TARGET["platform"],
            "--dest", str(staging),
            "--requirement", str(staging / "requirements.txt"),
        ]
        subprocess.run(command, check=True)
        write_json_atomic(staging / "wheelhouse-manifest.json", build_manifest(staging))
        verify_wheelhouse(staging)
        if output.exists():
            shutil.rmtree(output)
        staging.replace(output)
        print(f"Prepared analysis wheelhouse: {output}")
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=root / "runtime/python-analysis-wheelhouse")
    parser.add_argument("--requirements", type=Path,
                        default=Path(__file__).with_name("python_analysis_requirements.txt"))
    parser.add_argument("--force", action="store_true")
    options = parser.parse_args()
    prepare(options.out.resolve(), options.requirements.resolve(), options.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
