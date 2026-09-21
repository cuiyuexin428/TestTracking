"""Create a server plugin ZIP using only the Python standard library."""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from result_service.app.server_plugins import inspect_bundle


def build_bundle(*, plugin_dir: Path, dll: Path, version: str, output: Path,
                 plugin: str | None = None, defaults: Path | None = None,
                 manifest: Path | None = None, dependencies: tuple[Path, ...] = ()) -> Path:
    plugin_dir, dll, output = plugin_dir.resolve(), dll.resolve(), output.resolve()
    name = plugin or plugin_dir.name
    settings = json.loads(defaults.read_text(encoding="utf-8-sig")) if defaults else {}
    resources: dict[str, str] = {}
    sources = {"bin/" + dll.name: dll}
    for role, filename in {"view": "result_view.json", "adapter": "result_adapter.py",
                           "ai_context": "AI_CONTEXT.md", "report_template": "result_report.py"}.items():
        source = plugin_dir / filename
        if source.is_file():
            resources[role] = filename
            sources[filename] = source
    if manifest:
        resources["manifest"] = "dataset/manifest.txt"
        sources["dataset/manifest.txt"] = manifest.resolve()
    for dependency in dependencies:
        filename = "bin/" + dependency.name
        if filename.casefold() in {key.casefold() for key in sources}:
            raise ValueError("Duplicate DLL/dependency filename: " + dependency.name)
        sources[filename] = dependency.resolve()
    definition = {"schema_version": 1, "package_type": "testtracking-server-plugin",
                  "plugin": name, "version": version,
                  "scheduler": {"dll": "bin/" + dll.name, "defaults": settings},
                  "resources": resources}
    output.parent.mkdir(parents=True, exist_ok=True)
    # Build and validate before publishing. An invalid bundle cannot replace an
    # existing delivery artifact, and the build never imports adapter code.
    with tempfile.TemporaryDirectory(prefix="plugin-bundle-", dir=output.parent) as temporary:
        candidate = Path(temporary) / "bundle.zip"
        with zipfile.ZipFile(candidate, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            payload = json.dumps(definition, ensure_ascii=False, indent=2).encode("utf-8")
            info = zipfile.ZipInfo("server_plugin.json", (2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, payload)
            for filename, source in sorted(sources.items()):
                if source == output:
                    raise ValueError("The output ZIP cannot be an input resource")
                info = zipfile.ZipInfo(filename, (2020, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, source.read_bytes())
        inspect_bundle(candidate)
        candidate.replace(output)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description="Build an offline server plugin bundle")
    parser.add_argument("--plugin-dir", type=Path, required=True,
                        help="Plugin resource directory containing result_view.json and AI_CONTEXT.md")
    parser.add_argument("--dll", type=Path, required=True, help="Scheduler plugin DLL from matching Release build")
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--plugin", help="Plugin id; defaults to plugin-dir basename")
    parser.add_argument("--defaults", type=Path, help="JSON defaults for a new disabled plugin")
    parser.add_argument("--manifest", type=Path, help="Optional dataset task manifest; does not include client data")
    parser.add_argument("--dependency", type=Path, action="append", default=[],
                        help="Additional plugin-local runtime file; repeat as needed")
    args = parser.parse_args()
    output = build_bundle(plugin_dir=args.plugin_dir, dll=args.dll, version=args.version,
                          output=args.output, plugin=args.plugin, defaults=args.defaults,
                          manifest=args.manifest, dependencies=tuple(args.dependency))
    print(output)


if __name__ == "__main__":
    main()
