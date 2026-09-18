"""Offline tests for analysis dependency preparation and verification."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from python_analysis_support import (  # noqa: E402
    REQUIRED_IMPORTS,
    TARGET,
    build_manifest,
    read_requirements,
    verify_wheelhouse,
    verify_runtime,
    write_json_atomic,
)
from prepare_python_analysis_wheelhouse import prepare  # noqa: E402


class AnalysisDependencyTests(unittest.TestCase):
    requirements = TOOLS / "python_analysis_requirements.txt"

    def make_wheelhouse(self, root: Path) -> Path:
        wheelhouse = root / "wheelhouse"
        wheelhouse.mkdir()
        (wheelhouse / "requirements.txt").write_bytes(self.requirements.read_bytes())
        (wheelhouse / "pip.pyz").write_bytes(b"test pip zipapp")
        (wheelhouse / "numpy-2.2.6-cp312-cp312-win_amd64.whl").write_bytes(b"test wheel")
        write_json_atomic(wheelhouse / "wheelhouse-manifest.json", build_manifest(wheelhouse))
        return wheelhouse

    def test_requirements_pin_core_and_transitive_packages(self) -> None:
        packages = read_requirements(self.requirements)
        self.assertTrue(set(REQUIRED_IMPORTS).issubset(packages))
        self.assertGreater(len(packages), len(REQUIRED_IMPORTS))
        self.assertEqual(packages["numpy"], "2.2.6")

    def test_manifest_targets_windows_x64_python_312(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            wheelhouse = self.make_wheelhouse(Path(temporary))
            manifest = verify_wheelhouse(wheelhouse)
            self.assertEqual(manifest["target"], TARGET)
            self.assertEqual(TARGET["platform"], "win_amd64")
            self.assertEqual(TARGET["abi"], "cp312")

    def test_tampered_wheel_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            wheelhouse = self.make_wheelhouse(Path(temporary))
            wheel = next(wheelhouse.glob("*.whl"))
            wheel.write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "SHA256"):
                verify_wheelhouse(wheelhouse)

    def test_verify_cli_needs_no_network(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            wheelhouse = self.make_wheelhouse(Path(temporary))
            result = subprocess.run(
                [sys.executable, str(TOOLS / "python_analysis_support.py"),
                 "verify-wheelhouse", "--wheelhouse", str(wheelhouse)],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("verification passed", result.stdout)

    def test_verified_cache_is_reused_without_network(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            wheelhouse = self.make_wheelhouse(Path(temporary))
            with patch("prepare_python_analysis_wheelhouse.subprocess.run") as run, \
                    patch("prepare_python_analysis_wheelhouse.download_pip_zipapp") as download:
                prepare(wheelhouse, self.requirements)
            run.assert_not_called()
            download.assert_not_called()

    def test_failed_refresh_preserves_existing_wheelhouse(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            wheelhouse = self.make_wheelhouse(Path(temporary))
            before = (wheelhouse / "wheelhouse-manifest.json").read_bytes()
            def fake_download(destination: Path) -> None:
                destination.write_bytes(b"test pip zipapp")

            with patch("prepare_python_analysis_wheelhouse.download_pip_zipapp",
                       side_effect=fake_download), patch(
                           "prepare_python_analysis_wheelhouse.subprocess.run",
                           side_effect=subprocess.CalledProcessError(1, "pip")):
                with self.assertRaises(subprocess.CalledProcessError):
                    prepare(wheelhouse, self.requirements, force=True)
            self.assertEqual(before, (wheelhouse / "wheelhouse-manifest.json").read_bytes())
            verify_wheelhouse(wheelhouse)

    def test_installed_manifest_records_verified_versions(self) -> None:
        packages = read_requirements(self.requirements)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "analysis-runtime-manifest.json"
            with patch("python_analysis_support.metadata.version", side_effect=packages.__getitem__), \
                    patch("python_analysis_support.import_module") as import_module:
                result = verify_runtime(self.requirements, output)
            self.assertEqual(packages, json.loads(output.read_text())["packages"])
            self.assertEqual(sorted(REQUIRED_IMPORTS), result["verified_imports"])
            self.assertEqual(len(REQUIRED_IMPORTS), import_module.call_count)

    def test_version_failure_does_not_publish_success_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "analysis-runtime-manifest.json"
            with patch("python_analysis_support.metadata.version", return_value="0.0"):
                with self.assertRaisesRegex(RuntimeError, "version mismatch"):
                    verify_runtime(self.requirements, output)
            self.assertFalse(output.exists())

    def test_prepare_uses_zipapp_and_writes_verified_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "wheelhouse"

            def fake_download(destination: Path) -> None:
                destination.write_bytes(b"test pip zipapp")

            def fake_pip(command: list[str], check: bool) -> None:
                self.assertTrue(check)
                self.assertTrue(command[1].endswith("pip.pyz"))
                self.assertIn("--platform", command)
                destination = Path(command[command.index("--dest") + 1])
                (destination / "numpy-2.2.6-cp312-cp312-win_amd64.whl").write_bytes(b"wheel")

            with patch("prepare_python_analysis_wheelhouse.download_pip_zipapp",
                       side_effect=fake_download), patch(
                           "prepare_python_analysis_wheelhouse.subprocess.run",
                           side_effect=fake_pip):
                prepare(output, self.requirements)
            manifest = verify_wheelhouse(output)
            self.assertEqual(manifest["target"], TARGET)


if __name__ == "__main__":
    unittest.main()
