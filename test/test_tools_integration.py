"""Exercise deployment/analysis scripts against private paths and PostgreSQL."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres, sql_text

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None


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
        if name == 'init_postgres_offline.ps1':
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
        self.sql("CREATE TABLE runs(run_id text PRIMARY KEY,status text,created_at timestamptz);"
                 "CREATE TABLE tasks(run_id text); CREATE TABLE coverage_checkpoints(run_id text);"
                 "CREATE TABLE plugin_task_completions(run_id text); CREATE TABLE plugin_runs(run_id text);"
                 "CREATE TABLE coverage_file_points(run_id text);"
                 "INSERT INTO runs VALUES('running','running',now()-interval '10 days'),"
                 "('stopped','stopped',now()-interval '9 days'),('finished-quote''','finished',now());"
                 "INSERT INTO tasks SELECT run_id FROM runs; INSERT INTO coverage_checkpoints SELECT run_id FROM runs;"
                 "INSERT INTO plugin_task_completions SELECT run_id FROM runs;"
                 "INSERT INTO plugin_runs SELECT run_id FROM runs; INSERT INTO coverage_file_points SELECT run_id FROM runs;")
        self.script('prune_old_runs.ps1', '-Keep', '0', *self.database_args())
        self.assertEqual(self.sql("SELECT string_agg(run_id,',' ORDER BY run_id) FROM runs"), 'running,stopped')
        for table in ('tasks', 'coverage_checkpoints', 'plugin_task_completions'):
            self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '2')
        for table in ('plugin_runs', 'coverage_file_points'):
            self.assertEqual(self.sql(f'SELECT count(*) FROM {table}'), '3')

    def test_prune_sql_error_rolls_back_and_never_reports_success(self):
        self.sql("CREATE TABLE runs(run_id text PRIMARY KEY,status text,created_at timestamptz);"
                 "INSERT INTO runs VALUES('finished','finished',now());")
        result = self.script('prune_old_runs.ps1', '-Keep', '0', *self.database_args(), succeeds=False)
        self.assertNotIn('pruned operational history:', result.stdout)
        self.assertEqual(self.sql('SELECT count(*) FROM runs'), '1')

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

    def test_compare_reports_missing_files_and_replaces_previous_report(self):
        self.sql("CREATE TABLE plugin_runs(run_id text,plugin_type text,expected_files int,status text,finished_at timestamptz);"
                 "CREATE TABLE plugin_run_files(run_id text,plugin_type text,file text);"
                 "CREATE TABLE plugin_file_outcomes(run_id text,plugin_type text,file text,status text,detail text);"
                 "CREATE TABLE stp_compare_old(run_id text,file text,info_txt text,info_new_txt text);"
                 "INSERT INTO plugin_runs VALUES('run','stpcompare',2,'finished',now());"
                 "INSERT INTO plugin_run_files VALUES('run','stpcompare','one.stp'),('run','stpcompare','two.stp');"
                 "INSERT INTO plugin_file_outcomes VALUES('run','stpcompare','one.stp','success','');"
                 "INSERT INTO stp_compare_old VALUES('run','one.stp','old','new');")
        output = self.root / 'compare.txt'
        self.script('settle_stpcompare.ps1', '-RunId', 'run', '-OutFile', output, *self.database_args(), succeeds=False)
        self.assertIn('incomplete\ttwo.stp', output.read_text())
        self.assertIn('different\tone.stp', output.read_text())
        self.sql("INSERT INTO plugin_file_outcomes VALUES('run','stpcompare','two.stp','success','');"
                 "INSERT INTO stp_compare_old VALUES('run','two.stp','same','same');"
                 "UPDATE stp_compare_old SET info_new_txt=info_txt;")
        self.script('settle_stpcompare.ps1', '-RunId', 'run', '-OutFile', output, *self.database_args())
        self.assertIn('different=0', output.read_text())
        self.assertNotIn('different\tone.stp', output.read_text())

    def test_deploy_nondefault_ports_paths_and_single_plugin_packages(self):
        project = self.root / 'project with spaces'
        (project / 'scheduler').mkdir(parents=True)
        shutil.copyfile(ROOT / 'scheduler/scheduler.json', project / 'scheduler/scheduler.json')
        build = project / 'build outputs'
        binaries = build / 'bin/Debug'
        binaries.mkdir(parents=True)
        for filename in ('client.exe', 'scheduler.exe', 'coverage.dll', 'stpcompare.dll'):
            (binaries / filename).write_bytes(b'configuration fixture; never executed')
        deploy = project / 'deployment.json'
        deploy.write_text(json.dumps({'server_ip':'192.0.2.17','server_port':6123,'db_port':6543,
                                      'db_name':'test_db','db_user':'test_user','program_version':'p17',
                                      'dataset_version':'d17'}))
        self.script('setup_offline_server.ps1', '-ProjectRoot', project, '-BuildDir', build,
                    '-DeployConfig', deploy, '-SkipBuild', '-SkipDatabase', '-SkipDataset',
                    '-SkipPackage', '-SkipFirewall')
        config = json.loads((binaries / 'config/scheduler.json').read_text())
        self.assertEqual(config['server']['router_endpoint'], 'tcp://*:6123')
        for name in ('coverage', 'stpcompare'):
            self.assertIn('port=6543', config['plugins'][name]['db_conn'])
            self.assertEqual(config['plugins'][name]['stp_manifest'], str(project / 'runtime/dataset/manifest.txt').replace('\\','/'))
            self.assertEqual(config['plugins'][name]['expected_program_version'], 'p17')
        for name in ('coverage', 'stpcompare'):
            output = self.root / ('package ' + name)
            self.script('build_package.ps1', '-OutDir', output, '-BinDir', binaries, '-OnlyClient',
                        '-Plugin', name, '-ServerIp', '192.0.2.17', '-ServerPort', '6123', '-DbPort', '6543')
            client = json.loads((output / 'config/client.json').read_text())
            self.assertEqual(client['scheduler']['endpoint'], 'tcp://192.0.2.17:6123')
            enabled = [key for key,value in client['plugins'].items() if value['enabled']]
            self.assertEqual(enabled, [name])
            self.assertIn('port=6543', client['plugins'][name]['db_conn'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pg-bin', type=Path, default=ROOT / 'third_party/pgsql/bin')
    parser.add_argument('--powershell', default=shutil.which('pwsh') or 'powershell')
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.pg_bin = OPTIONS.pg_bin.resolve()
    unittest.main(argv=[__file__, *remaining])
