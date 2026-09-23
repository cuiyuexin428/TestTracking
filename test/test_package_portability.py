"""Package build checks using private paths with spaces, without external services."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


class SourceArchiveTests(unittest.TestCase):
    def test_archive_keeps_offline_inputs_without_browser_review_state(self):
        with tempfile.TemporaryDirectory(prefix='source delivery ') as temporary:
            root = Path(temporary)
            source = root / 'TestTracking-02'
            included = {
                'client/src/Client.cpp',
                'result_service/app/program_bindings.py',
                'result_service/app/server_plugins.py',
                'plugins/coverage/AI_CONTEXT.md',
                'tools/build_plugin_bundle.py',
                'tools/client_update.ps1',
                'runtime/python/python.exe',
                'runtime/python-analysis-wheelhouse/numpy.whl',
                'runtime/dataset/sample.stp',
                'third_party/lib/example.lib',
                'third_party_src/downloads/dependency.zip',
            }
            excluded = {
                'runtime/deployment_ui_review/service.json',
                'runtime/deployment_ui_review/stdout.log',
                'runtime/deployment_ui_review/artifacts/catalog.json',
                'runtime/deployment_ui_review/artifacts/deployments.json',
                'runtime/deployment_ui_review/artifacts/objects/test-object',
                'runtime/scheduler.json',
                'runtime/source_launch.json',
                'runtime/pgdata/postgresql.conf',
                'runtime/logs/service.log',
                '.git/config',
                'service-workspace/server/runtime/result_service.json',
                'build/stale.exe',
                'real-test/state.db',
            }
            for relative in included | excluded:
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(relative.encode('utf-8'))
            output = root / 'offline source.zip'
            result = subprocess.run(
                [sys.executable, '-B', str(ROOT / 'tools/create_source_archive.py'),
                 str(source), str(output)],
                cwd=root, capture_output=True, text=True, timeout=15)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual({source.name + '/' + relative for relative in included},
                                 set(archive.namelist()))
                for relative in included:
                    self.assertEqual(relative.encode('utf-8'),
                                     archive.read(source.name + '/' + relative))


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
        for name in ('client.exe', 'common.dll', 'coverage.dll', 'stpcompare.dll'):
            (self.bin / name).write_bytes(b'fixture never executed')
        for name in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140_atomic_wait.dll'):
            (self.crt / name).write_bytes(b'fixture CRT never executed')
        (self.program / 'CustomRunner.exe').write_bytes(b'fixture never executed')
        (self.program / 'BaselineRunner.exe').write_bytes(b'baseline fixture')
        (self.program / 'CandidateRunner.exe').write_bytes(b'candidate fixture')
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

    def test_bootstrap_package_has_auto_identity_and_only_framework_runtime(self):
        (self.bin / 'libprotobuf.dll').write_bytes(b'protobuf fixture')
        (self.bin / 'custom_business.dll').write_bytes(b'must not ship')
        output, _ = self.build('-Bootstrap', '-DeploymentEndpoint', 'http://192.0.2.1:56330/',
                               '-Endpoint', 'tcp://192.0.2.1:5555')
        config = json.loads((output / 'config/client.json').read_text())
        self.assertEqual({}, config['plugins'])
        self.assertEqual('auto', config['client']['worker_id'])
        self.assertEqual('http://192.0.2.1:56330', config['client']['deployment_endpoint'])
        self.assertEqual('tcp://192.0.2.1:5555', config['scheduler']['endpoint'])
        self.assertNotIn('db_conn', json.dumps(config))
        for name in ('client.exe', 'common.dll', 'libprotobuf.dll', 'vcruntime140.dll',
                     'tools/client_update.ps1', 'start_client.bat'):
            self.assertTrue((output / name).is_file(), name)
        for name in ('coverage.dll', 'stpcompare.dll', 'custom_business.dll', 'testenv', 'store'):
            self.assertFalse((output / name).exists(), name)

    def test_bootstrap_requires_service_and_preserves_existing_output(self):
        _, result = self.build('-Bootstrap', succeeds=False)
        self.assertIn('requires -DeploymentEndpoint', result.stderr)
        output, _ = self.build('-Bootstrap', '-DeploymentEndpoint', 'http://192.0.2.1:56330')
        config_path = output / 'config/client.json'
        before = config_path.read_bytes()
        _, result = self.build('-Bootstrap', '-DeploymentEndpoint', 'http://192.0.2.2:56330', succeeds=False)
        self.assertIn('empty output directory', result.stderr)
        self.assertEqual(before, config_path.read_bytes())

    def test_bootstrap_defaults_scheduler_to_remote_service_host_and_custom_port(self):
        # No ServerIp is supplied here: the service host is enough for a client
        # copied to another machine to reach the scheduler after deployment.
        output = self.root / 'client package'
        command = [shutil.which('powershell') or 'pwsh', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                   '-File', str(ROOT / 'tools/build_package.ps1'), '-OutDir', str(output),
                   '-BinDir', str(self.bin), '-VCRuntimeDir', str(self.crt), '-Bootstrap',
                   '-DeploymentEndpoint', 'http://tracking.intranet:56330', '-ServerPort', '6012']
        result = subprocess.run(command, cwd=self.root, capture_output=True, encoding='utf-8',
                                errors='replace', timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        config = json.loads((output / 'config/client.json').read_text())
        self.assertEqual('tcp://tracking.intranet:6012', config['scheduler']['endpoint'])
        self.assertIn('-ServerPort', (output / 'README.md').read_text(encoding='utf-8-sig'))

    def test_client_component_includes_runtime_without_program_inputs(self):
        output, _ = self.build('-OnlyClient', '-Plugin', 'stpcompare',
                               '-DeploymentEndpoint', 'http://192.0.2.1:56330')
        config = json.loads((output / 'config/client.json').read_text())
        plugin = config['plugins']['stpcompare']
        self.assertEqual(plugin['programs']['baseline']['executable'],
                         'testenv/program/baseline/BaselineConverter.exe')
        self.assertEqual(plugin['programs']['candidate']['executable'],
                         'testenv/program/candidate/CandidateConverter.exe')
        self.assertTrue((output / 'vcruntime140.dll').exists())
        self.assertTrue((output / 'tools/client_update.ps1').is_file())
        self.assertEqual(config['client']['deployment_endpoint'], 'http://192.0.2.1:56330')
        self.build('-OnlyClient')
        retained = json.loads((output / 'config/client.json').read_text())
        self.assertEqual(retained['client']['deployment_endpoint'], 'http://192.0.2.1:56330')

    def test_stpcompare_package_contains_two_versioned_programs(self):
        output, _ = self.build(
            '-Plugin', 'stpcompare',
            '-BaselineConverterExe', self.program / 'BaselineRunner.exe',
            '-CandidateConverterExe', self.program / 'CandidateRunner.exe',
            '-BaselineProgramVersion', 'old-7', '-CandidateProgramVersion', 'new-9',
            '-DatasetDir', self.dataset)
        plugin = json.loads((output / 'config/client.json').read_text())['plugins']['stpcompare']
        baseline = output / plugin['programs']['baseline']['executable']
        candidate = output / plugin['programs']['candidate']['executable']
        self.assertTrue(baseline.is_file())
        self.assertTrue(candidate.is_file())
        self.assertNotEqual(baseline.parent, candidate.parent)
        self.assertEqual((baseline.parent / 'version.txt').read_text(), 'old-7')
        self.assertEqual((candidate.parent / 'version.txt').read_text(), 'new-9')

        next_baseline = self.program / 'BaselineV2.exe'
        next_candidate = self.program / 'CandidateV2.exe'
        next_baseline.write_bytes(b'next baseline')
        next_candidate.write_bytes(b'next candidate')
        self.build('-OnlyProgram', '-Plugin', 'stpcompare',
                   '-BaselineConverterExe', next_baseline,
                   '-CandidateConverterExe', next_candidate,
                   '-BaselineProgramVersion', 'old-8', '-CandidateProgramVersion', 'new-10')
        updated = json.loads((output / 'config/client.json').read_text())['plugins']['stpcompare']
        self.assertEqual(updated['programs']['baseline']['executable'],
                         'testenv/program/baseline/BaselineV2.exe')
        self.assertEqual(updated['programs']['candidate']['executable'],
                         'testenv/program/candidate/CandidateV2.exe')
        self.assertTrue((output / 'testenv/dataset/sample.stp').is_file())

    def test_stpcompare_program_update_requires_both_programs(self):
        _, result = self.build('-OnlyProgram', '-Plugin', 'stpcompare',
                               '-BaselineConverterExe', self.program / 'BaselineRunner.exe',
                               succeeds=False)
        self.assertIn('both -BaselineConverterExe and -CandidateConverterExe', result.stderr)

    def build_all_plugins(self):
        return self.build('-Plugin', 'all',
                          '-StepTestExe', self.program / 'CustomRunner.exe',
                          '-BaselineConverterExe', self.program / 'BaselineRunner.exe',
                          '-CandidateConverterExe', self.program / 'CandidateRunner.exe',
                          '-ProgramVersion', 'cov-3', '-BaselineProgramVersion', 'old-7',
                          '-CandidateProgramVersion', 'new-9', '-DatasetDir', self.dataset,
                          '-OpenCppCoverageDir', self.coverage)[0]

    def test_multi_plugin_package_has_isolated_versioned_programs_and_relocates(self):
        output = self.build_all_plugins()
        moved = self.root / 'moved multi plugin client'
        shutil.move(output, moved)
        config = json.loads((moved / 'config/client.json').read_text())
        self.assertEqual({'coverage', 'stpcompare'}, set(config['plugins']))
        self.assertTrue(all(item['enabled'] for item in config['plugins'].values()))
        coverage = config['plugins']['coverage']
        compare = config['plugins']['stpcompare']
        self.assertEqual('testenv/program/coverage', coverage['program_dir'])
        self.assertEqual('testenv/program/coverage/CustomRunner.exe', coverage['target_exe'])
        self.assertTrue(all(path.startswith('testenv/program/coverage/') for path in coverage['engine_dlls']))
        for program, version in [(coverage['target_exe'], 'cov-3'),
                                 (compare['programs']['baseline']['executable'], 'old-7'),
                                 (compare['programs']['candidate']['executable'], 'new-9')]:
            executable = moved / program
            self.assertTrue(executable.is_file())
            self.assertEqual(version, (executable.parent / 'version.txt').read_text())
            self.assertTrue((executable.parent / 'vcruntime140.dll').is_file())
        self.assertEqual(coverage['stp_root'], compare['stp_root'])
        self.assertTrue((moved / coverage['stp_root'] / 'manifest.txt').is_file())

    def test_multi_plugin_component_updates_preserve_other_programs_and_configuration(self):
        output = self.build_all_plugins()
        config_path = output / 'config/client.json'
        original = json.loads(config_path.read_text())
        original['client']['custom'] = {'keep': True}
        original['plugins']['coverage']['custom'] = 'coverage-setting'
        original['plugins']['stpcompare']['custom'] = 'compare-setting'
        config_path.write_text(json.dumps(original), encoding='utf-8')
        self.build('-OnlyClient')
        self.assertEqual(original, json.loads(config_path.read_text()))
        before_stp = {str(path.relative_to(output)): path.read_bytes()
                      for directory in ('baseline', 'candidate')
                      for path in (output / 'testenv/program' / directory).rglob('*') if path.is_file()}
        replacement = self.program / 'CoverageV4.exe'
        replacement.write_bytes(b'coverage v4')
        self.build('-OnlyProgram', '-Plugin', 'coverage', '-StepTestExe', replacement,
                   '-ProgramVersion', 'cov-4', '-OpenCppCoverageDir', self.coverage)
        after_coverage = json.loads(config_path.read_text())
        self.assertEqual(original['plugins']['stpcompare'], after_coverage['plugins']['stpcompare'])
        self.assertEqual('testenv/program/coverage/CoverageV4.exe', after_coverage['plugins']['coverage']['target_exe'])
        for path, content in before_stp.items():
            self.assertEqual(content, (output / path).read_bytes())
        self.assertFalse((output / 'testenv/program/coverage/CustomRunner.exe').exists())
        coverage_files = {str(path.relative_to(output)): path.read_bytes()
                          for directory in ('testenv/program/coverage', 'testenv/opencppcoverage')
                          for path in (output / directory).rglob('*') if path.is_file()}
        next_baseline = self.program / 'OldV8.exe'
        next_candidate = self.program / 'NewV10.exe'
        next_baseline.write_bytes(b'baseline v8')
        next_candidate.write_bytes(b'candidate v10')
        self.build('-OnlyProgram', '-Plugin', 'stpcompare', '-BaselineConverterExe', next_baseline,
                   '-CandidateConverterExe', next_candidate, '-BaselineProgramVersion', 'old-8',
                   '-CandidateProgramVersion', 'new-10')
        after_stp = json.loads(config_path.read_text())
        self.assertEqual(after_coverage['plugins']['coverage'], after_stp['plugins']['coverage'])
        self.assertEqual('testenv/program/baseline/OldV8.exe', after_stp['plugins']['stpcompare']['programs']['baseline']['executable'])
        for path, content in coverage_files.items():
            self.assertEqual(content, (output / path).read_bytes())
        self.assertEqual('fixture data', (output / 'testenv/dataset/sample.stp').read_text())
        _, result = self.build('-OnlyProgram', '-StepTestExe', replacement,
                               '-OpenCppCoverageDir', self.coverage, succeeds=False)
        self.assertIn('Multiple plugins are enabled', result.stderr)

    def test_legacy_coverage_program_update_keeps_stp_sibling_directories(self):
        output, _ = self.build('-StepTestExe', self.program / 'CustomRunner.exe',
                               '-DatasetDir', self.dataset, '-OpenCppCoverageDir', self.coverage)
        self.build('-OnlyClient', '-Plugin', 'all')
        config_path = output / 'config/client.json'
        before = json.loads(config_path.read_text())
        self.assertEqual('testenv/program/CustomRunner.exe', before['plugins']['coverage']['target_exe'])
        baseline = output / 'testenv/program/baseline/keep.exe'
        baseline.parent.mkdir()
        baseline.write_bytes(b'keep baseline')
        self.build('-OnlyProgram', '-Plugin', 'coverage', '-StepTestExe', self.program / 'CustomRunner.exe',
                   '-OpenCppCoverageDir', self.coverage)
        updated = json.loads(config_path.read_text())
        self.assertEqual(b'keep baseline', baseline.read_bytes())
        self.assertEqual(before['plugins']['stpcompare'], updated['plugins']['stpcompare'])
        self.assertEqual('testenv/program/coverage/CustomRunner.exe', updated['plugins']['coverage']['target_exe'])
        self.assertEqual('testenv/program/coverage', updated['plugins']['coverage']['program_dir'])

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
        _, result = self.build('-OnlyProgram', '-StepTestExe', replacement,
                               '-OpenCppCoverageDir', self.coverage,
                               '-ServerIp', '192.0.2.99', '-ServerPort', 6789,
                               succeeds=False)
        self.assertIn('do not rewrite deployment endpoints', result.stderr)

    def test_program_update_retains_custom_engine_symbols_when_module_option_is_omitted(self):
        engine = self.program / 'FixtureEngine.dll'
        symbols = self.program / 'FixtureEngine.pdb'
        engine.write_bytes(b'custom engine v1')
        symbols.write_bytes(b'custom engine symbols v1')
        output, _ = self.build('-StepTestExe', self.program / 'CustomRunner.exe',
                               '-DatasetDir', self.dataset, '-OpenCppCoverageDir', self.coverage,
                               '-EngineDlls', 'FixtureEngine.dll')
        config_path = output / 'config/client.json'
        original = json.loads(config_path.read_text())['plugins']['coverage']
        self.assertEqual(['testenv/program/FixtureEngine.dll'], original['engine_dlls'])
        self.assertEqual(b'custom engine symbols v1', (output / 'testenv/program/FixtureEngine.pdb').read_bytes())

        replacement = self.program / 'RunnerV2.exe'
        replacement.write_bytes(b'runner v2')
        engine.write_bytes(b'custom engine v2')
        symbols.write_bytes(b'custom engine symbols v2')
        self.build('-OnlyProgram', '-StepTestExe', replacement, '-OpenCppCoverageDir', self.coverage)
        updated = json.loads(config_path.read_text())['plugins']['coverage']
        self.assertEqual(original['engine_dlls'], updated['engine_dlls'])
        deployed_engine = output / updated['engine_dlls'][0]
        self.assertEqual(b'custom engine v2', deployed_engine.read_bytes())
        self.assertEqual(b'custom engine symbols v2', deployed_engine.with_suffix('.pdb').read_bytes())
        self.assertFalse((output / 'testenv/program/CustomRunner.exe').exists())

    def test_client_update_preserves_deployed_configuration_and_plugin(self):
        output, _ = self.build('-OnlyClient', '-Plugin', 'stpcompare')
        config_path = output / 'config/client.json'
        config = json.loads(config_path.read_text())
        config['scheduler'] = {'endpoint': 'tcp://production:6012', 'poll_timeout_ms': 91}
        config['client'].update(worker_id='worker-production-07', task_threads=8, custom_setting={'keep': True})
        config['plugins']['stpcompare'].update(
            db_conn="host='db-production' port=6543 dbname='production' user='writer' password='secret' sslmode=require",
            table='production_results',
            programs={
                'baseline': {'executable': 'testenv/program/baseline/ProductionOld.exe'},
                'candidate': {'executable': 'testenv/program/candidate/ProductionNew.exe'}},
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
            'enabled': False, 'dll': 'stpcompare.dll',
            'programs': {
                'baseline': {'executable': 'testenv/program/baseline/KeepOld.exe'},
                'candidate': {'executable': 'testenv/program/candidate/KeepNew.exe'}},
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
        self.assertEqual('../data/artifacts', config['artifact_store_dir'])
        self.assertEqual(['../data/artifact_import'], config['artifact_import_roots'])
        self.assertTrue((output / 'data/artifact_import').is_dir())
        self.assertIn('-NoNewWindow -PassThru', start_server)
        readme = (output / 'README.md').read_text(encoding='utf-8')
        for name in ('自动部署使用指南.md', '服务端插件包格式.md', '已知限制.md'):
            self.assertEqual((ROOT / 'docs' / name).read_bytes(),
                             (output / 'docs' / name).read_bytes())
            self.assertIn('docs/' + name, readme)
        self.assertIn('plugin_options.stpcompare.result_table', readme)
        self.assertNotIn('Set stp_result_table', readme)
        for port in ('TCP 5555', 'TCP 5432', 'TCP 56330'):
            self.assertIn(port, readme)
        self.assertIn('result service remains available', readme)
        self.assertIn('-RemoteAddress 192.168.100.0/24', readme)

        manifest = json.loads((output / 'data/result_views/resources.manifest').read_text(encoding='utf-8'))
        expected = {}
        for view in (ROOT / 'plugins').glob('*/result_view.json'):
            resources = ['view', 'ai_context']
            self.assertTrue((output / 'result_service/plugin_ai_contexts' / (view.parent.name + '.md')).is_file())
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
            "print(json.dumps({'plugins':sorted(service.plugins),'artifacts':str(service.artifacts.root.relative_to(root))})); service.close()",
            str(moved)], cwd=self.root, capture_output=True, encoding='utf-8', errors='replace', timeout=15,
            env={key: value for key, value in os.environ.items() if not key.startswith('RESULT_SERVICE_')},
            creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(0, probe.returncode, probe.stdout + probe.stderr)
        loaded = json.loads(probe.stdout)
        self.assertEqual(sorted(expected), loaded['plugins'])
        self.assertEqual('data\\artifacts', loaded['artifacts'])


if __name__ == '__main__':
    unittest.main()
