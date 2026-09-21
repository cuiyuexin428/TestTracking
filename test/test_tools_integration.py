"""Exercise deployment/analysis scripts against private paths and PostgreSQL."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
import uuid
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres, sql_text

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None


def plain_output(text):
    """PowerShell 7 wraps/colorizes errors while Windows PowerShell uses plain text."""
    text = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', text)
    text = re.sub(r'(?m)^\s*\|\s?', '', text)
    return ' '.join(text.split())


class ToolIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.pg_bin / 'psql.exe')
        cls.addClassCleanup(cls.pg.close)
        cls.pg.start()

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='tools-integration-', dir=ROOT / 'build/test'))
        self.schema = 'tools_' + uuid.uuid4().hex
        self.pg.sql(f'CREATE SCHEMA "{self.schema}";')
        self.conn = self.pg.conn + f" options='-csearch_path={self.schema}'"

    def tearDown(self):
        print(f'  tool artifacts: {self.root}', flush=True)

    def sql(self, sql):
        return self.pg.sql(sql, self.conn)

    def script(self, name, *args, succeeds=True):
        command = [OPTIONS.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                   '-File', str(ROOT / 'tools' / name), *map(str, args)]
        if name in {'init_postgres_offline.ps1', 'source_services.ps1'}:
            # A restarted PostgreSQL can inherit the script's output handles.
            # A file lets us wait for the script without waiting for server EOF.
            result = self.pg.run(command, check=False, timeout=45,
                                 output_file=self.root / ('init-' + uuid.uuid4().hex + '.log'))
        else:
            result = subprocess.run(command, cwd=self.root, env=self.pg.env, capture_output=True,
                                    encoding='utf-8', errors='replace', timeout=30,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + '\n' + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + '\n' + result.stderr)
        return result

    def database_args(self):
        return ['-DbConn', self.conn, '-Psql', self.pg.program('psql')]

    def test_dataset_manifest_records_size_and_sha256(self):
        dataset = self.root / 'dataset'
        nested = dataset / 'nested'
        nested.mkdir(parents=True)
        content = b'STP fixture\r\n'
        (nested / 'part.stp').write_bytes(content)
        (dataset / 'ignored.txt').write_text('not a task')
        manifest = self.root / 'manifest.txt'
        self.script('build_dataset_manifest.ps1', '-DatasetDir', dataset, '-OutFile', manifest)
        rows = [json.loads(line) for line in manifest.read_text(encoding='utf-8').splitlines()]
        self.assertEqual([{
            'path': 'nested/part.stp',
            'size_bytes': len(content),
            'content_sha256': hashlib.sha256(content).hexdigest(),
        }], rows)

    def test_postgres_replaces_legacy_remote_rules_and_is_idempotent(self):
        # auto.conf keeps this private server on loopback; matching the main
        # config also exercises pg_ctl reload without restarting the fixture.
        conf = self.pg.data / 'postgresql.conf'
        original_conf = conf.read_bytes()
        hba = self.pg.data / 'pg_hba.conf'
        original_hba = hba.read_bytes()
        self.addCleanup(conf.write_bytes, original_conf)
        self.addCleanup(hba.write_bytes, original_hba)
        conf.write_text(f"port = {self.pg.port}\nlisten_addresses = '*'\n")
        local = 'host    all             all             127.0.0.1/32            trust\n'
        unrelated = 'host reporting analyst 198.51.100.0/24 scram-sha-256\n'
        legacy_broad = 'host    all             all             0.0.0.0/0            trust\n'
        legacy_narrow = 'host    all             all             192.0.2.0/24            trust\n'
        hba.write_text(local + unrelated + legacy_broad + legacy_narrow)
        args = ['-PgRoot', self.pg.pg_bin.parent, '-PgData', self.pg.data,
                '-DbPort', self.pg.port, '-DbUser', 'coverage_test', '-DbName', 'postgres',
                '-ClientCidr', '192.0.2.0/24', '-AuthMethod', 'scram-sha-256',
                '-NoService', '-NoFirewall']
        self.script('init_postgres_offline.ps1', *args)
        expected = hba.read_bytes()
        text = hba.read_text()
        self.assertNotIn('0.0.0.0/0', text)
        self.assertIn(local, text)
        self.assertIn(unrelated, text)
        self.assertEqual(text.count('192.0.2.0/24'), 1)
        self.assertEqual(self.pg.sql('SELECT count(*) FROM pg_hba_file_rules WHERE error IS NOT NULL'), '0')
        self.assertEqual(self.pg.sql("SELECT auth_method FROM pg_hba_file_rules WHERE address='192.0.2.0'"),
                         'scram-sha-256')
        first_write = hba.stat().st_mtime_ns
        self.script('init_postgres_offline.ps1', *args)
        self.assertEqual(hba.read_bytes(), expected)
        self.assertEqual(hba.stat().st_mtime_ns, first_write)

        # Later changes migrate the marked rule too, including a CIDR change.
        args[args.index('-ClientCidr') + 1] = '203.0.113.0/24'
        self.script('init_postgres_offline.ps1', *args)
        self.assertNotIn('192.0.2.0/24', hba.read_text())
        self.assertEqual(hba.read_text().count('203.0.113.0/24'), 1)

    def test_prune_preserves_active_runs_raw_and_durable_metadata(self):
        self.sql("CREATE TABLE runs(run_id text PRIMARY KEY,plugin_type text,status text,created_at timestamptz);"
                 "CREATE TABLE tasks(run_id text); CREATE TABLE coverage_checkpoints(run_id text);"
                 "CREATE TABLE third_party_checkpoints(run_id text);"
                 "CREATE TABLE plugin_checkpoint_tables(plugin_type text,checkpoint_table text);"
                 "CREATE TABLE plugin_task_completions(run_id text); CREATE TABLE plugin_runs(run_id text);"
                 "CREATE TABLE coverage_file_points(run_id text);"
                 "INSERT INTO runs VALUES('running','coverage','running',now()-interval '10 days'),"
                 "('stopped','third-party','stopped',now()-interval '9 days'),"
                 "('finished-coverage','coverage','finished',now()-interval '1 day'),"
                 "('finished-quote''','third-party','finished',now());"
                 "INSERT INTO tasks SELECT run_id FROM runs;"
                 "INSERT INTO coverage_checkpoints SELECT run_id FROM runs WHERE plugin_type='coverage';"
                 "INSERT INTO third_party_checkpoints SELECT run_id FROM runs WHERE plugin_type='third-party';"
                 "INSERT INTO plugin_checkpoint_tables VALUES"
                 "('coverage','coverage_checkpoints'),('third-party','third_party_checkpoints');"
                 "INSERT INTO plugin_task_completions SELECT run_id FROM runs;"
                 "INSERT INTO plugin_runs SELECT run_id FROM runs; INSERT INTO coverage_file_points SELECT run_id FROM runs;")
        preview = self.script('prune_old_runs.ps1', '-Keep', '0', '-DryRun', *self.database_args())
        self.assertIn('would prune:', preview.stdout)
        self.assertEqual(self.sql('SELECT count(*) FROM runs'), '4')
        self.script('prune_old_runs.ps1', '-Keep', '0', *self.database_args())
        self.assertEqual(self.sql("SELECT string_agg(run_id,',' ORDER BY run_id) FROM runs"), 'running,stopped')
        for table in ('tasks', 'plugin_task_completions'):
            self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '2')
        for table in ('coverage_checkpoints', 'third_party_checkpoints'):
            self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '1')
        self.assertEqual(self.sql('SELECT count(*) FROM plugin_checkpoint_tables'), '2')
        for table in ('plugin_runs', 'coverage_file_points'):
            self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '4')

    def test_prune_requires_registration_before_deleting_old_runs(self):
        self.sql("CREATE TABLE runs(run_id text PRIMARY KEY,plugin_type text,status text,created_at timestamptz);"
                 "CREATE TABLE tasks(run_id text); CREATE TABLE vendor_checkpoints(run_id text);"
                 "INSERT INTO runs VALUES('old-run','vendor','finished',now());"
                 "INSERT INTO tasks VALUES('old-run'); INSERT INTO vendor_checkpoints VALUES('old-run');")
        for registry_exists in (False, True):
            with self.subTest(registry_exists=registry_exists):
                if registry_exists:
                    self.sql("CREATE TABLE plugin_checkpoint_tables(plugin_type text,checkpoint_table text);"
                             "INSERT INTO plugin_checkpoint_tables VALUES('another-plugin','another_checkpoints');")
                result = self.script('prune_old_runs.ps1', '-Keep', '0', *self.database_args(), succeeds=False)
                self.assertNotIn('pruned operational history:', result.stdout)
                self.assertIn('initialize', result.stderr)
                for table in ('runs', 'tasks', 'vendor_checkpoints'):
                    self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '1')
        # Initializing the old plugin registers its existing table, so a later prune succeeds.
        self.sql("INSERT INTO plugin_checkpoint_tables VALUES('vendor','vendor_checkpoints');")
        self.script('prune_old_runs.ps1', '-Keep', '0', *self.database_args())
        for table in ('runs', 'tasks', 'vendor_checkpoints'):
            self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '0')

    def test_prune_sql_error_rolls_back_and_never_reports_success(self):
        self.sql("CREATE TABLE runs(run_id text PRIMARY KEY,plugin_type text,status text,created_at timestamptz);"
                 "CREATE TABLE tasks(run_id text); CREATE TABLE vendor_checkpoints(run_id text);"
                 "CREATE TABLE plugin_checkpoint_tables(plugin_type text,checkpoint_table text);"
                 "INSERT INTO plugin_checkpoint_tables VALUES('vendor','vendor_checkpoints');"
                 "INSERT INTO runs VALUES('finished','vendor','finished',now());"
                 "INSERT INTO tasks VALUES('finished'); INSERT INTO vendor_checkpoints VALUES('finished');"
                 "CREATE FUNCTION reject_prune() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
                 "RAISE EXCEPTION 'injected checkpoint delete failure'; END; $$;"
                 "CREATE TRIGGER reject_prune BEFORE DELETE ON vendor_checkpoints FOR EACH ROW "
                 "EXECUTE FUNCTION reject_prune();")
        result = self.script('prune_old_runs.ps1', '-Keep', '0', *self.database_args(), succeeds=False)
        self.assertNotIn('pruned operational history:', result.stdout)
        self.assertIn('injected checkpoint delete failure', result.stderr)
        for table in ('runs', 'tasks', 'vendor_checkpoints'):
            self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '1')

    def test_export_quotes_run_id_and_preserves_output_on_sql_failure(self):
        run = "run-'quoted"
        self.sql("CREATE TABLE coverage_run_summary(run_id text,total_files int,selected_files int,cumulative_points int,finished_at timestamptz);"
                 "CREATE TABLE coverage_selected(run_id text,file text);"
                 f"INSERT INTO coverage_run_summary VALUES({sql_text(run)},2,1,4,now());"
                 f"INSERT INTO coverage_selected VALUES({sql_text(run)},'folder/file,with comma.stp');")
        output = self.root / 'selected list.txt'
        self.script('export_selected.ps1', '-RunId', run, '-OutFile', output, *self.database_args())
        expected = 'folder/file,with comma.stp\n'
        self.assertEqual(output.read_text(), expected)
        self.sql('DROP TABLE coverage_selected')
        self.script('export_selected.ps1', '-RunId', run, '-OutFile', output, *self.database_args(), succeeds=False)
        self.assertEqual(output.read_text(), expected)

    def test_stp_raw_result_check_reports_missing_files_and_replaces_previous_report(self):
        self.sql("CREATE TABLE plugin_runs(run_id text,plugin_type text,expected_files int,status text,finished_at timestamptz);"
                 "CREATE TABLE plugin_run_files(run_id text,plugin_type text,file text);"
                 "CREATE TABLE plugin_file_outcomes(run_id text,plugin_type text,file text,status text,detail text);"
                 "CREATE TABLE stp_compare_old(run_id text,file text,program_role text,export_stage text,"
                 "content bytea,content_sha256 text,content_size bigint);"
                 "INSERT INTO plugin_runs VALUES('run','stpcompare',2,'finished',now());"
                 "INSERT INTO plugin_run_files VALUES('run','stpcompare','one.stp'),('run','stpcompare','two.stp');"
                 "INSERT INTO plugin_file_outcomes VALUES('run','stpcompare','one.stp','success','');"
                 "INSERT INTO stp_compare_old SELECT 'run','one.stp',role,stage,decode('01','hex'),repeat('a',64),1 "
                 "FROM (VALUES('baseline'),('candidate')) p(role) CROSS JOIN (VALUES('first'),('second')) s(stage);")
        output = self.root / 'compare.txt'
        self.script('settle_stpcompare.ps1', '-RunId', 'run', '-OutFile', output, *self.database_args(), succeeds=False)
        self.assertIn('incomplete\ttwo.stp', output.read_text())
        self.sql("INSERT INTO plugin_file_outcomes VALUES('run','stpcompare','two.stp','success','');"
                 "INSERT INTO stp_compare_old SELECT 'run','two.stp',role,stage,decode('02','hex'),repeat('b',64),1 "
                 "FROM (VALUES('baseline'),('candidate')) p(role) CROSS JOIN (VALUES('first'),('second')) s(stage);")
        self.script('settle_stpcompare.ps1', '-RunId', 'run', '-OutFile', output, *self.database_args())
        self.assertIn('complete=2', output.read_text())
        self.assertNotIn('incomplete\t', output.read_text())

    def test_deploy_relative_paths_from_non_project_working_directory(self):
        project_name = 'project with spaces 源码'
        project = self.root / project_name
        (project / 'scheduler').mkdir(parents=True)
        shutil.copyfile(ROOT / 'scheduler/scheduler.json', project / 'scheduler/scheduler.json')
        (project / 'result_service').mkdir()
        shutil.copyfile(ROOT / 'result_service/config.example.json', project / 'result_service/config.example.json')
        (project / 'client').mkdir()
        for name in ('client.json', 'client.stpcompare.json'):
            shutil.copyfile(ROOT / 'client' / name, project / 'client' / name)
        build = project / 'build outputs'
        binaries = build / 'bin/Release'
        binaries.mkdir(parents=True)
        for filename in ('client.exe', 'scheduler.exe', 'coverage.dll', 'stpcompare.dll'):
            (binaries / filename).write_bytes(b'configuration fixture; never executed')
        # A fixture CRT keeps this configuration test independent of whether
        # Visual Studio redistributables are installed on the test host.
        crt = project / 'VC runtime'
        crt.mkdir()
        for filename in ('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll'):
            (crt / filename).write_bytes(b'configuration fixture; never executed')
        deploy = project / 'deployment.json'
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            web_port = reserve.getsockname()[1]
        deploy.write_text(json.dumps({'build_config':'Release','server_ip':'192.0.2.17','server_port':6123,'db_port':6543,
                                      'management_port':6124,'result_port':web_port,'plugin':'stpcompare',
                                      'db_name':'test_db','db_user':'test_user','program_version':'p17',
                                      'baseline_program_version':'old-p17',
                                      'candidate_program_version':'new-p17',
                                      'dataset_version':'d17'}))
        # Invoke from the parent directory while every deployment path is
        # relative to the project root. This catches accidental resolution
        # against the caller's working directory.
        self.script('setup_offline_server.ps1', '-ProjectRoot', project_name,
                    '-BuildDir', 'build outputs', '-DeployConfig', 'deployment.json',
                    '-SkipBuild', '-SkipDatabase', '-SkipDataset',
                    '-SkipPackage', '-SkipFirewall', '-SkipPythonAnalysis')
        runtime_scheduler = project / 'runtime/scheduler.json'
        config = json.loads(runtime_scheduler.read_text(encoding='utf-8'))
        self.assertEqual(config['server']['router_endpoint'], 'tcp://*:6123')
        self.assertEqual(Path(config['server']['log_file']), project / 'runtime/logs/scheduler.log')
        self.assertEqual(config['server']['management_endpoint'], 'http://127.0.0.1:6124')
        result = json.loads((project / 'runtime/result_service.json').read_text(encoding='utf-8'))
        self.assertEqual('http://127.0.0.1:6124', result['scheduler_management_url'])
        self.assertEqual(web_port, result['port'])
        self.assertEqual('0.0.0.0', result['host'])
        self.assertEqual('logs', result['logs_dir'])
        self.assertEqual('result_service/artifacts', result['artifact_store_dir'])
        self.assertEqual(['artifact_import'], result['artifact_import_roots'])
        self.assertTrue((project / 'runtime/artifact_import').is_dir())
        self.assertFalse(result['plugin_views_dir'])
        self.assertFalse(result['plugin_adapters_dir'])
        self.assertFalse(result['report_templates_dir'])
        self.assertEqual('stp_compare_old', result['plugin_options']['stpcompare']['result_table'])
        self.assertEqual(runtime_scheduler, Path(result['scheduler_config_path']))
        template = json.loads(Path(result['client_config_path']).read_text(encoding='utf-8'))
        self.assertEqual('tcp://192.0.2.17:6123', template['scheduler']['endpoint'])
        self.assertEqual(['stpcompare'], [key for key, value in template['plugins'].items() if value['enabled']])
        self.assertIn('port=6543', template['plugins']['stpcompare']['db_conn'])
        self.assertEqual('old-p17', template['plugins']['stpcompare']['programs']['baseline']['version'])
        self.assertEqual('new-p17', template['plugins']['stpcompare']['programs']['candidate']['version'])
        for name in ('start_server', 'start_scheduler', 'start_result_service', 'stop_result_service'):
            batch = (project / 'runtime' / (name + '.bat')).read_text(encoding='ascii')
            self.assertIn('%~dp0..\\tools\\source_services.ps1', batch)
        settings = json.loads((project / 'runtime/source_launch.json').read_text(encoding='utf-8'))
        self.assertEqual(binaries, Path(settings['bin_dir']))
        self.assertIsNone(settings['managed_database'])
        self.assertEqual(runtime_scheduler, Path(settings['scheduler_config_path']))
        # CMake's scheduler_config ALL target copies its default template on
        # every build. A subsequent offline Release compile must not overwrite
        # the deployed database/ports/manifest or change either launch path.
        (binaries / 'config').mkdir(exist_ok=True)
        shutil.copyfile(ROOT / 'scheduler/scheduler.json', binaries / 'config/scheduler.json')
        self.assertEqual(config, json.loads(runtime_scheduler.read_text(encoding='utf-8')))
        self.assertEqual(runtime_scheduler, Path(json.loads(
            (project / 'runtime/source_launch.json').read_text(encoding='utf-8'))['scheduler_config_path']))
        # Run the source-specific web launcher with the prepared embedded
        # runtime. Reuse immutable stdlib/extension files; site-packages remain
        # in the repository runtime, so no network or installed system Python
        # is needed and the test does not copy thousands of third-party files.
        portable = ROOT / 'runtime/python'
        if (portable / 'python.exe').is_file():
            shutil.copytree(ROOT / 'result_service', project / 'result_service', dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
            for name in ('coverage', 'stpcompare'):
                target = project / 'plugins' / name
                target.mkdir(parents=True)
                for artifact in ('result_adapter.py', 'result_view.json', 'result_report.py'):
                    source = ROOT / 'plugins' / name / artifact
                    if source.is_file():
                        shutil.copyfile(source, target / artifact)
            local_python = project / 'runtime/python'
            local_python.mkdir()
            for source in portable.iterdir():
                if source.is_file() and source.suffix != '._pth':
                    os.link(source, local_python / source.name)
            path_file = next(portable.glob('python*._pth'))
            original_paths = path_file.read_text(encoding='utf-8').splitlines()
            relocated_paths = [str((portable / item).resolve()) if item and not item.startswith(('#', 'import '))
                               else item for item in original_paths]
            (local_python / path_file.name).write_text('\n'.join(relocated_paths) + '\n', encoding='utf-8')
            runtime = project / 'runtime'
            self.addCleanup(self.script, 'source_services.ps1', '-Action', 'stop_result_service', '-RuntimeDir', runtime)
            self.script('source_services.ps1', '-Action', 'start_result_service', '-RuntimeDir', runtime, '-NoWait')
            first_pid = (runtime / 'result_service.pid').read_text()
            self.script('source_services.ps1', '-Action', 'start_result_service', '-RuntimeDir', runtime, '-NoWait')
            self.assertEqual(first_pid, (runtime / 'result_service.pid').read_text())
            with urlopen(f'http://127.0.0.1:{web_port}/api/plugins', timeout=5) as response:
                plugin_response = json.load(response)
            self.assertEqual({'coverage', 'stpcompare'}, {entry['name'] for entry in plugin_response})
            self.script('source_services.ps1', '-Action', 'stop_result_service', '-RuntimeDir', runtime)
            self.assertFalse((runtime / 'result_service.pid').exists())
        # Reject a source inside the owned staging tree before deleting it.
        dataset = project / 'runtime/dataset'
        dataset.mkdir()
        precious = dataset / 'only-copy.stp'
        precious.write_text('the only complete copy')
        refused = self.script('setup_offline_server.ps1', '-ProjectRoot', project_name,
                             '-BuildDir', 'build outputs', '-DeployConfig', 'deployment.json',
                             '-DatasetDir', 'runtime/dataset', '-SkipBuild', '-SkipDatabase',
                             '-SkipPackage', '-SkipFirewall', '-SkipPythonAnalysis', succeeds=False)
        self.assertIn('no data was removed', plain_output(refused.stderr))
        self.assertEqual('the only complete copy', precious.read_text())
        for name in ('coverage', 'stpcompare'):
            self.assertIn('port=6543', config['plugins'][name]['db_conn'])
            self.assertEqual(config['plugins'][name]['stp_manifest'], str(project / 'runtime/dataset/manifest.txt').replace('\\','/'))
        self.assertEqual(config['plugins']['coverage']['expected_program_version'], 'p17')
        self.assertEqual(config['plugins']['stpcompare']['expected_baseline_program_version'], 'old-p17')
        self.assertEqual(config['plugins']['stpcompare']['expected_candidate_program_version'], 'new-p17')
        self.assertNotIn('expected_program_version', config['plugins']['stpcompare'])
        for name in ('coverage', 'stpcompare'):
            output = self.root / ('package ' + name)
            self.script('build_package.ps1', '-OutDir', output, '-BinDir', binaries, '-OnlyClient',
                        '-Plugin', name, '-ServerIp', '192.0.2.17', '-ServerPort', '6123', '-DbPort', '6543',
                        '-VCRuntimeDir', crt)
            client = json.loads((output / 'config/client.json').read_text())
            self.assertEqual(client['scheduler']['endpoint'], 'tcp://192.0.2.17:6123')
            enabled = [key for key,value in client['plugins'].items() if value['enabled']]
            self.assertEqual(enabled, [name])
            self.assertIn('port=6543', client['plugins'][name]['db_conn'])

    def test_source_start_restarts_only_its_prepared_database(self):
        project = self.root / 'source restart'
        runtime = project / 'runtime'
        runtime.mkdir(parents=True)
        private = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.pg_bin / 'psql.exe')
        self.addCleanup(private.close)
        private.start()
        settings = {'bin_dir': str(project / 'missing-build'),
                    'scheduler_config_path': str(runtime / 'scheduler.json'),
                    'managed_database': {'pg_ctl': str(private.program('pg_ctl')),
                                         'data_dir': str(private.data)}}
        (runtime / 'source_launch.json').write_text(json.dumps(settings), encoding='utf-8')
        private.run([private.program('pg_ctl'), '-D', private.data, '-m', 'fast', '-w', 'stop'])
        private.started = False
        self.assertEqual(3, private.run([private.program('pg_ctl'), 'status', '-D', private.data], check=False).returncode)
        # The rest of the application is intentionally absent. PostgreSQL
        # must be ready before the launcher reports the missing web config.
        result = self.script('source_services.ps1', '-Action', 'start_server', '-RuntimeDir', runtime, succeeds=False)
        self.assertIn('Managed PostgreSQL ready', result.stdout)
        self.assertIn('Required source deployment file not found', plain_output(result.stdout + result.stderr))
        self.assertEqual('1', private.sql('SELECT 1'))
        private.started = True
        actual = Path(private.sql('SHOW data_directory')).resolve()
        self.assertEqual(private.data.resolve(), actual)
        # External database mode must not start any stopped database.
        private.run([private.program('pg_ctl'), '-D', private.data, '-m', 'fast', '-w', 'stop'])
        private.started = False
        settings['managed_database'] = None
        (runtime / 'source_launch.json').write_text(json.dumps(settings), encoding='utf-8')
        self.script('source_services.ps1', '-Action', 'start_server', '-RuntimeDir', runtime, succeeds=False)
        self.assertEqual(3, private.run([private.program('pg_ctl'), 'status', '-D', private.data], check=False).returncode)
        private.started = False


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pg-bin', type=Path, default=ROOT / 'third_party/pgsql/bin')
    parser.add_argument('--powershell', default=shutil.which('pwsh') or 'powershell')
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.pg_bin = OPTIONS.pg_bin.resolve()
    unittest.main(argv=[__file__, *remaining])
