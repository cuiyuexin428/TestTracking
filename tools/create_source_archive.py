#!/usr/bin/env python3
"""Create the portable source archive used for offline delivery."""

from __future__ import annotations

import argparse
import os
import tempfile
import zipfile
from pathlib import Path, PurePosixPath


EXCLUDED_DIRECTORIES = {
    ".vs",
    "build",
    "dist",
    "out",
    "runtime/logs",
    "runtime/pgdata",
    "runtime/result_service",
    "third_party_src/build",
    "third_party_src/src",
    # PostgreSQL's optional GUI is not used by deployment or compilation and
    # contains paths too deep for Windows ZIP extraction at typical locations.
    "third_party/pgsql/pgAdmin 4",
}

# Generated workstation launchers and old ad-hoc probes are not portable source.
# setup_offline_server.ps1 regenerates launchers for the destination machine.
EXCLUDED_FILES = {"runtime/start_scheduler.bat", "runtime/rt_test.ps1"}


def normalized_relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def should_exclude_directory(relative: str) -> bool:
    return relative.casefold() in {
        item.casefold() for item in EXCLUDED_DIRECTORIES
    }


def iter_source_files(root: Path):
    for current, directories, files in os.walk(root):
        current_path = Path(current)
        kept_directories = []
        for directory in directories:
            candidate = current_path / directory
            if not should_exclude_directory(normalized_relative(candidate, root)):
                kept_directories.append(directory)
        directories[:] = kept_directories

        for filename in files:
            path = current_path / filename
            if (path.suffix.casefold() not in {".pyc", ".pid"}
                    and normalized_relative(path, root).casefold() not in EXCLUDED_FILES):
                yield path


def create_archive(source: Path, destination: Path) -> tuple[int, int]:
    source = source.resolve(strict=True)
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)

    file_count = 0
    total_bytes = 0
    with tempfile.NamedTemporaryFile(
        dir=destination.parent, prefix=destination.name + ".", suffix=".tmp", delete=False
    ) as temporary:
        temporary_path = Path(temporary.name)

    try:
        with zipfile.ZipFile(
            temporary_path,
            mode="w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=6,
            allowZip64=True,
        ) as archive:
            for path in iter_source_files(source):
                relative = PurePosixPath(source.name) / PurePosixPath(
                    normalized_relative(path, source)
                )
                archive.write(path, relative.as_posix())
                file_count += 1
                total_bytes += path.stat().st_size

        with zipfile.ZipFile(temporary_path, mode="r") as archive:
            corrupt_entry = archive.testzip()
            if corrupt_entry is not None:
                raise RuntimeError(f"archive validation failed at {corrupt_entry}")

        os.replace(temporary_path, destination)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise

    return file_count, total_bytes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    file_count, total_bytes = create_archive(args.source, args.destination)
    print(f"Created {args.destination} with {file_count} files ({total_bytes} bytes).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
