"""Prepare a portable Windows x64 Python and PostgreSQL driver for server releases.

Run on the build machine once; subsequent package builds use the verified cache.
No global Python installation or pip is modified. An offline build may instead
pass a prepared runtime directory to build_server_package.ps1.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import urllib.request
import zipfile

PYTHON_VERSION = "3.12.10"
PACKAGES = {"psycopg": "3.2.10", "psycopg-binary": "3.2.10",
            "typing-extensions": "4.15.0", "tzdata": "2025.2"}


def download(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def verify(directory: Path) -> None:
    subprocess.run([str(directory / "python.exe"), "-B", "-I", "-c",
                    "import sys, struct, psycopg, psycopg_binary; "
                    "assert sys.version_info >= (3, 10); assert struct.calcsize('P') == 8; "
                    "print('Portable Python and psycopg ready')"], check=True)


def prepare(directory: Path) -> None:
    marker = directory / "runtime-manifest.json"
    expected = {"python": PYTHON_VERSION, "packages": PACKAGES, "platform": "win_amd64"}
    if marker.is_file() and json.loads(marker.read_text()) == expected:
        verify(directory)
        return
    if directory.exists() and any(directory.iterdir()):
        raise RuntimeError(f"Runtime directory is not an empty build cache: {directory}. "
                           "Use another output or pass it explicitly with -PythonRuntimeDir.")
    directory.mkdir(parents=True, exist_ok=True)
    archive = download(f"https://www.python.org/ftp/python/{PYTHON_VERSION}/"
                       f"python-{PYTHON_VERSION}-embed-amd64.zip")
    with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
        bundle.extractall(directory)
    # Embedded Python deliberately ignores the host installation and PYTHONPATH.
    # Its own site-packages is the only additional package search directory.
    (directory / "python312._pth").write_text(
        "python312.zip\n.\nLib/site-packages\nimport site\n", encoding="utf-8")
    site = directory / "Lib/site-packages"
    for package, version in PACKAGES.items():
        metadata = json.loads(download(f"https://pypi.org/pypi/{package}/{version}/json"))
        wheels = [entry for entry in metadata["urls"] if entry["filename"].endswith(
            ("-py3-none-any.whl", "-py2.py3-none-any.whl", "-cp312-cp312-win_amd64.whl"))]
        if len(wheels) != 1:
            raise RuntimeError(f"Expected one Windows/Python wheel for {package}=={version}")
        wheel = wheels[0]
        content = download(wheel["url"])
        if hashlib.sha256(content).hexdigest() != wheel["digests"]["sha256"]:
            raise RuntimeError(f"Wheel hash mismatch: {wheel['filename']}")
        with zipfile.ZipFile(io.BytesIO(content)) as bundle:
            bundle.extractall(site)
    verify(directory)
    marker.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    prepare(options.out.resolve())
