"""Package build checks using private paths with spaces, without external services."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
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
        deployment_args = [] if (self.root / 'client package/config/client.json').exists() else ['-ServerIp', '192.0.2.1']
        result = subprocess.run(
            [shutil.which('powershell') or 'pwsh', '-NoProfile', '-ExecutionPolicy', 'Bypass',
             '-File', str(ROOT / 'tools/build_package.ps1'), '-OutDir', str(self.root / 'client package'),
             '-BinDir', str(self.bin), '-VCRuntimeDir', str(self.crt), *deployment_args,
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

    def test_component_update_rejects_its_own_input_before_deleting_data(self):
        output, _ = self.build('-OnlyDataset', '-DatasetDir', self.dataset)
        deployed = output / 'testenv/dataset'
        _, result = self.build('-OnlyDataset', '-DatasetDir', deployed, succeeds=False)
        self.assertIn('Input is inside', result.stderr)
        self.assertEqual('fixture data', (deployed / 'sample.stp').read_text())

    def test_coverage_input_directory_name_does_not_delete_program_component(self):
        named_program = self.root / 'tools input/program'
        named_program.mkdir(parents=True)
        (named_program / 'OpenCppCoverage.exe').write_bytes(b'fixture tool')
        output, _ = self.build('-StepTestExe', self.program / 'CustomRunner.exe',
                               '-DatasetDir', self.dataset, '-OpenCppCoverageDir', named_program)
        self.assertTrue((output / 'testenv/program/CustomRunner.exe').is_file())
        self.assertTrue((output / 'testenv/opencppcoverage/OpenCppCoverage.exe').is_file())

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

    def test_client_update_preserves_deployed_configuration_and_plugin(self):
        output, _ = self.build('-OnlyClient', '-Plugin', 'stpcompare')
        config_path = output / 'config/client.json'
        config = json.loads(config_path.read_text())
        config['scheduler'] = {'endpoint': 'tcp://production:6012', 'poll_timeout_ms': 91}
        config['client'].update(worker_id='worker-production-07', task_threads=8, custom_setting={'keep': True})
        config['plugins']['stpcompare'].update(
            db_conn="host='db-production' port=6543 dbname='production' user='writer' password='secret' sslmode=require",
            table='production_results', program_exe='testenv/program/ProductionConverter.exe',
            custom_plugin_setting={'threshold': 7})
        config_path.write_text(json.dumps(config), encoding='utf-8')
        self.build('-OnlyClient')
        self.assertEqual(config, json.loads(config_path.read_text()))

        self.build('-OnlyClient', '-WorkerId', 'worker-production-08', '-DbName', 'next_database')
        updated = json.loads(config_path.read_text())
        expected = json.loads(json.dumps(config))
        expected['client']['worker_id'] = 'worker-production-08'
        expected['plugins']['stpcompare']['db_conn'] += " dbname='next_database'"
        self.assertEqual(expected, updated)

    def test_explicit_plugin_selection_preserves_each_plugins_configuration(self):
        output, _ = self.build('-OnlyClient')
        config_path = output / 'config/client.json'
        config = json.loads(config_path.read_text())
        config['plugins']['stpcompare'] = {
            'enabled': False, 'dll': 'stpcompare.dll', 'program_exe': 'testenv/program/KeepConverter.exe',
            'table': 'custom_results', 'db_conn': 'dbname=custom user=custom', 'custom_setting': 42}
        config_path.write_text(json.dumps(config), encoding='utf-8')
        self.build('-OnlyClient', '-Plugin', 'stpcompare')
        config['plugins']['coverage']['enabled'] = False
        config['plugins']['stpcompare']['enabled'] = True
        self.assertEqual(config, json.loads(config_path.read_text()))

    def test_binary_discovery_uses_offline_build_and_requires_explicit_other_outputs(self):
        stale = self.root / 'build/rt/bin/Release'
        current = self.root / 'build/offline_verify/bin/Release'
        for directory in (stale, current):
            directory.mkdir(parents=True)
            for filename in ('client.exe', 'scheduler.exe'):
                (directory / filename).write_bytes(b'fixture')
        quote = lambda value: str(value).replace("'", "''")
        command = (f". '{quote(ROOT / 'tools/PackageSupport.ps1')}'; "
                   f"Resolve-PackageBinaryDir -ProjectRoot '{quote(self.root)}' -Executable client.exe; "
                   f"Resolve-PackageBinaryDir -ProjectRoot '{quote(self.root)}' -Executable scheduler.exe; "
                   f"Resolve-PackageBinaryDir -ProjectRoot '{quote(self.root)}' -Explicit 'build/rt' -Executable scheduler.exe")
        result = subprocess.run([shutil.which('powershell') or 'pwsh', '-NoProfile', '-Command', command],
                                capture_output=True, text=True, timeout=15,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual([str(current), str(current), str(stale)], result.stdout.splitlines())
        shutil.rmtree(current)
        missing = command.split('; Resolve-PackageBinaryDir')[0] + (
            f"; Resolve-PackageBinaryDir -ProjectRoot '{quote(self.root)}' -Executable client.exe")
        result = subprocess.run([shutil.which('powershell') or 'pwsh', '-NoProfile', '-Command', missing],
                                capture_output=True, text=True, timeout=15,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('current build', result.stderr)


@unittest.skipUnless(os.name == 'nt', 'Windows deployment packages')
class ServerPackageTests(unittest.TestCase):
    def setUp(self):
        test_root = ROOT / 'build/test'
        test_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='server package paths ', dir=test_root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'server binaries'
        self.crt = self.root / 'VC CRT'
        self.bin.mkdir()
        self.crt.mkdir()
        (self.bin / 'scheduler.exe').write_bytes(b'fixture never executed')
        (self.bin / 'common.dll').write_bytes(b'fixture never executed')
        for name in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
            (self.crt / name).write_bytes(b'fixture never executed')

    def test_generated_server_keeps_result_service_independent_and_documents_lan(self):
        output = self.root / 'server package'
        result = subprocess.run(
            [shutil.which('pwsh') or shutil.which('powershell') or 'powershell',
             '-NoProfile', '-ExecutionPolicy', 'Bypass',
             '-File', str(ROOT / 'tools/build_server_package.ps1'),
             '-ProjectRoot', str(ROOT), '-OutDir', str(output), '-BinDir', str(self.bin),
             '-VCRuntimeDir', str(self.crt), '-Config', 'Release', '-UseSystemPython',
             '-SkipDataset', '-StpResultTable', 'stp_compare_old',
             '-ResultDbConn', 'host=127.0.0.1 dbname=testtracking user=reader'],
            cwd=self.root, capture_output=True, encoding='utf-8', errors='replace', timeout=45,
            creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        powershell = shutil.which('pwsh') or shutil.which('powershell') or 'powershell'
        for relative_script in ('scripts/start_server.ps1', 'scripts/start_result_service.ps1',
                                'scripts/stop_result_service.ps1',
                                'tools/open_result_service_firewall.ps1'):
            script = output / relative_script
            quoted = str(script).replace("'", "''")
            parser = ("$tokens=$null; $errors=$null; "
                      "[void][System.Management.Automation.Language.Parser]::ParseFile("
                      f"'{quoted}', [ref]$tokens, [ref]$errors); "
                      "if($errors.Count){$errors | Out-String | Write-Error; exit 1}")
            parsed = subprocess.run(
                [powershell, '-NoProfile', '-Command', parser], capture_output=True,
                encoding='utf-8', errors='replace', timeout=15,
                creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(parsed.returncode, 0,
                             relative_script + ': ' + parsed.stdout + parsed.stderr)

        start_server = (output / 'scripts/start_server.ps1').read_text(encoding='utf-8')
        self.assertIn('& $resultStart -NoWait', start_server)
        self.assertIn('scheduler will continue', start_server)
        self.assertNotIn('taskkill.exe', start_server)
        self.assertNotIn('result_service.pid', start_server)

        start_result = (output / 'scripts/start_result_service.ps1').read_text(encoding='utf-8')
        self.assertIn('result service already running', start_result)
        self.assertIn('oldCommand.IndexOf($launcher', start_result)
        self.assertIn('/healthz', start_result)
        self.assertIn('result service ready', start_result)
        self.assertIn('UI (LAN): http://${address}:$listenPort/', start_result)
        self.assertIn('open_result_service_firewall.ps1 as administrator', start_result)
        self.assertIn("$probeHost = if ($listenHost -in $wildcardHosts) { '127.0.0.1' }", start_result)
        stop_result = (output / 'scripts/stop_result_service.ps1').read_text(encoding='utf-8')
        self.assertIn('PID file does not refer to this result service', stop_result)

        firewall = (output / 'tools/open_result_service_firewall.ps1').read_text(encoding='utf-8')
        self.assertIn("[string[]]$RemoteAddress = @('LocalSubnet')", firewall)
        self.assertIn('Administrator privileges are required', firewall)
        self.assertIn('-RemoteAddress $RemoteAddress', firewall)

        config = json.loads((output / 'config/result_service.json').read_text(encoding='utf-8'))
        self.assertEqual(config['plugin_options']['stpcompare']['result_table'], 'stp_compare_old')
        scheduler = json.loads((output / 'config/scheduler.json').read_text(encoding='utf-8'))
        self.assertEqual('data/logs/scheduler.log', scheduler['server']['log_file'])
        self.assertEqual('http://127.0.0.1:5556', scheduler['server']['management_endpoint'])
        self.assertEqual(scheduler['server']['management_endpoint'], config['scheduler_management_url'])
        self.assertEqual('scheduler.json', config['scheduler_config_path'])
        self.assertEqual('client.template.json', config['client_config_path'])
        self.assertTrue((output / 'config/client.template.json').is_file())
        template = json.loads((output / 'config/client.template.json').read_text(encoding='utf-8-sig'))
        self.assertFalse(template['plugins']['coverage']['dry_run'])
        self.assertEqual('../data/logs', config['logs_dir'])
        self.assertIn('-NoNewWindow -PassThru', start_server)
        readme = (output / 'README.md').read_text(encoding='utf-8')
        self.assertIn('plugin_options.stpcompare.result_table', readme)
        self.assertNotIn('Set stp_result_table', readme)
        for port in ('TCP 5555', 'TCP 5432', 'TCP 56330'):
            self.assertIn(port, readme)
        self.assertIn('result service remains available', readme)
        self.assertIn('-RemoteAddress 192.168.100.0/24', readme)

        manifest = json.loads((output / 'data/result_views/resources.manifest').read_text(encoding='utf-8'))
        expected = {}
        for view in (ROOT / 'plugins').glob('*/result_view.json'):
            resources = ['view']
            for filename, resource in (('result_adapter.py', 'adapter'), ('result_report.py', 'report_template')):
                if (view.parent / filename).is_file():
                    resources.append(resource)
            expected[view.parent.name] = resources
        self.assertEqual({'schema_version': 1, 'plugins': expected}, manifest)

        moved = self.root / 'relocated server package'
        shutil.move(str(output), str(moved))
        probe = subprocess.run([
            sys.executable, '-B', '-c',
            "import sys,json; from pathlib import Path; from dataclasses import replace; "
            "root=Path(sys.argv[1]); sys.path.insert(0,str(root)); "
            "from result_service.app.config import ServiceConfig; "
            "from result_service.app.server import ResultService; "
            "service=ResultService(replace(ServiceConfig.from_file(root/'config/result_service.json'),db_conn='')); "
            "print(json.dumps(sorted(service.plugins))); service.close()",
            str(moved)], cwd=self.root, capture_output=True, encoding='utf-8', errors='replace', timeout=15,
            env={key: value for key, value in os.environ.items() if not key.startswith('RESULT_SERVICE_')},
            creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(0, probe.returncode, probe.stdout + probe.stderr)
        self.assertEqual(sorted(expected), json.loads(probe.stdout))


if __name__ == '__main__':
    unittest.main()
