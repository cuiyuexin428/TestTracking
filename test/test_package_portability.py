"""Package build checks using private paths with spaces, without external services."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'nt', 'Windows deployment packages')
class ClientPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='package paths ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'build binaries'
        self.crt = self.root / 'VC CRT'
        self.program = self.root / 'program source'
        self.dataset = self.root / 'dataset source'
        self.coverage = self.root / 'coverage tool'
        for directory in (self.bin, self.crt, self.program, self.dataset, self.coverage):
            directory.mkdir()
        for name in ('client.exe', 'coverage.dll', 'stpcompare.dll'):
            (self.bin / name).write_bytes(b'fixture never executed')
        for name in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140_atomic_wait.dll'):
            (self.crt / name).write_bytes(b'fixture CRT never executed')
        (self.program / 'CustomRunner.exe').write_bytes(b'fixture never executed')
        (self.program / 'TKernel.dll').write_bytes(b'fixture never executed')
        (self.coverage / 'OpenCppCoverage.exe').write_bytes(b'fixture never executed')
        (self.dataset / 'sample.stp').write_text('fixture data')

    def build(self, *args, succeeds=True):
        result = subprocess.run(
            [shutil.which('powershell') or 'pwsh', '-NoProfile', '-ExecutionPolicy', 'Bypass',
             '-File', str(ROOT / 'tools/build_package.ps1'), '-OutDir', str(self.root / 'client package'),
             '-BinDir', str(self.bin), '-ServerIp', '192.0.2.1', '-VCRuntimeDir', str(self.crt),
             *map(str, args)],
            cwd=self.root, capture_output=True, encoding='utf-8', errors='replace', timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return self.root / 'client package', result

    def test_complete_client_keeps_custom_program_and_runtime_after_move(self):
        output, _ = self.build('-StepTestExe', self.program / 'CustomRunner.exe',
                               '-DatasetDir', self.dataset, '-OpenCppCoverageDir', self.coverage)
        moved = self.root / 'relocated client'
        shutil.move(str(output), str(moved))
        config_text = (moved / 'config/client.json').read_text(encoding='utf-8')
        config = json.loads(config_text)
        self.assertNotIn(str(self.root).replace('\\', '/'), config_text.replace('\\\\', '/'))
        plugin = config['plugins']['coverage']
        self.assertEqual(plugin['target_exe'], 'testenv/program/CustomRunner.exe')
        for field in ('target_exe', 'opencppcoverage', 'program_dir', 'stp_root'):
            self.assertTrue((moved / plugin[field]).exists(), field)
        self.assertTrue((moved / 'testenv/dataset/manifest.txt').exists())
        for directory in (moved, moved / 'testenv/program'):
            self.assertTrue((directory / 'msvcp140_atomic_wait.dll').exists())

    def test_component_selection_is_exclusive_and_release_only(self):
        _, result = self.build('-OnlyClient', '-OnlyProgram', succeeds=False)
        self.assertIn('at most one', result.stderr)
        _, result = self.build('-OnlyClient', '-Config', 'Debug', succeeds=False)
        self.assertIn('Release', result.stderr)

    def test_client_component_includes_runtime_without_program_inputs(self):
        output, _ = self.build('-OnlyClient', '-Plugin', 'stpcompare')
        plugin = json.loads((output / 'config/client.json').read_text())['plugins']['stpcompare']
        self.assertEqual(plugin['program_exe'], 'testenv/program/Converter.exe')
        self.assertTrue((output / 'vcruntime140.dll').exists())

    def test_dataset_update_needs_only_dataset_inputs(self):
        shutil.rmtree(self.bin)
        shutil.rmtree(self.crt)
        output, _ = self.build('-OnlyDataset', '-DatasetDir', self.dataset)
        self.assertTrue((output / 'testenv/dataset/sample.stp').exists())
        self.assertFalse((output / 'config/client.json').exists())

    def test_component_updates_keep_executable_config_in_sync(self):
        output, _ = self.build('-StepTestExe', self.program / 'CustomRunner.exe',
                               '-DatasetDir', self.dataset, '-OpenCppCoverageDir', self.coverage)
        self.build('-OnlyClient')
        config = json.loads((output / 'config/client.json').read_text())
        self.assertEqual(config['plugins']['coverage']['target_exe'], 'testenv/program/CustomRunner.exe')
        replacement = self.program / 'RunnerV2.exe'
        replacement.write_bytes(b'replacement program fixture')
        self.build('-OnlyProgram', '-StepTestExe', replacement, '-OpenCppCoverageDir', self.coverage)
        config = json.loads((output / 'config/client.json').read_text())
        self.assertEqual(config['plugins']['coverage']['target_exe'], 'testenv/program/RunnerV2.exe')
        self.assertTrue((output / config['plugins']['coverage']['target_exe']).exists())
        self.assertTrue((output / 'testenv/dataset/sample.stp').exists())


if __name__ == '__main__':
    unittest.main()
