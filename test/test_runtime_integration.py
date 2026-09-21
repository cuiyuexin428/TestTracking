"""Real scheduler/client regressions using owned processes and private PostgreSQL.

No database connection string is accepted. Configs, logs and file stores are
created below build/test/runtime-integration-*. Failed-case logs are retained.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres, sql_text

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserve:
        if os.name == 'nt':
            reserve.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        reserve.bind(('127.0.0.1', 0))
        return reserve.getsockname()[1]


class CapturedProcess:
    def __init__(self, executable, config, directory, tag, env):
        self.log = directory / (tag + '.log')
        self.output = self.log.open('wb')
        self.process = subprocess.Popen(
            [str(executable), str(config)], cwd=directory, env=env,
            stdin=subprocess.PIPE, stdout=self.output, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)

    def text(self):
        return self.log.read_text(encoding='utf-8', errors='replace')

    def send(self, *commands):
        self.process.stdin.write(('\n'.join(commands) + '\n').encode())
        self.process.stdin.flush()

    def crash(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)
        self.output.close()

    def close(self):
        if self.process.poll() is None:
            try:
                self.send('quit')
                self.process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                self.crash()
        if self.process.stdin:
            self.process.stdin.close()
        self.output.close()


class ExclusiveFile:
    """Deny writes/deletes to one owned file, including atomic replacement."""
    def __init__(self, path):
        self.path = path

    def __enter__(self):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                       wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                                       wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel = kernel
        self.handle = kernel.CreateFileW(str(self.path), 0x80000000, 0, None, 3, 0x80, None)
        if self.handle == wintypes.HANDLE(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        return self

    def __exit__(self, *_):
        self.kernel.CloseHandle(self.handle)


class RuntimeIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for name in ('scheduler.exe', 'client.exe', 'coverage.dll', 'stpcompare.dll', 'scheduler_probe.dll'):
            if not (OPTIONS.bin_dir / name).is_file():
                raise RuntimeError(f'Missing runtime binary: {OPTIONS.bin_dir / name}')
        cls.pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.bin_dir / 'scheduler.exe')
        cls.addClassCleanup(cls.pg.close)
        cls.pg.start()

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='runtime-integration-', dir=ROOT / 'build/test'))
        self.schema = 'runtime_' + uuid.uuid4().hex
        self.pg.sql(f'CREATE SCHEMA "{self.schema}";')
        self.conn = self.pg.conn + f" options='-csearch_path={self.schema}'"
        self.procs = []
        self.server_number = 0
        self.client_number = 0
        self.endpoint = f'tcp://127.0.0.1:{unused_port()}'
        self.management_url = f'http://127.0.0.1:{unused_port()}'
        self.store = self.root / 'store'
        self.store.mkdir()
        self.manifest = self.root / 'manifest.txt'
        self.manifest.write_text('sample.stp,1\n', encoding='utf-8')
        self.scheduler_config = self.root / 'scheduler.json'
        self.client_config = self.root / 'client.json'
        self.configure()

    def tearDown(self):
        for process in reversed(self.procs):
            process.close()
        # Keep compact configs/logs as evidence; the PG fixture removes only its
        # own cluster. No wildcard deletion or process-name termination is used.
        print(f'  runtime artifacts: {self.root}', flush=True)

    def configure(self, *, file_store=False, delay=1200, framework_db=True,
                  client_timeout_ms=650):
        self.program = self.root / 'program'
        self.dataset = self.root / 'dataset'
        self.program.mkdir(exist_ok=True)
        self.dataset.mkdir(exist_ok=True)
        (self.program / 'version.txt').write_text('v1', encoding='utf-8')
        (self.dataset / 'version.txt').write_text('d1', encoding='utf-8')
        plugin = {'dll': str(OPTIONS.bin_dir / 'coverage.dll'), 'enabled': True,
                  'store_type': 'file' if file_store else 'postgres',
                  'store_dir': str(self.store), 'db_conn': self.conn}
        self.server_json = {
            'server': {'router_endpoint': self.endpoint, 'poll_timeout_ms': 10,
                       'management_endpoint': self.management_url,
                       'process_threads': 2, 'client_timeout_ms': client_timeout_ms,
                       'task_lease_timeout_ms': 0},
            'plugins': {'coverage': dict(plugin, stp_manifest=str(self.manifest), task_size=1,
                                        expected_program_version='v1', expected_dataset_version='d1')}}
        if framework_db:
            self.server_json['server']['db_conn'] = self.conn
        self.client_json = {
            'scheduler': {'endpoint': self.endpoint, 'poll_timeout_ms': 10},
            'client': {'worker_id': 'runtime-client', 'heartbeat_interval_ms': 100,
                       'task_threads': 1, 'request_backoff_initial_ms': 20,
                       'request_backoff_max_ms': 100, 'request_timeout_ms': 500,
                       'register_timeout_ms': 1000, 'register_max_attempts': 3},
            'plugins': {'coverage': dict(plugin, dry_run=True, dry_run_delay_ms=delay,
                                        store_cov=False, export_dir=str(self.root / 'reports'),
                                        program_dir=str(self.program), stp_root=str(self.dataset))}}
        self.write_configs()

    def write_configs(self):
        self.scheduler_config.write_text(json.dumps(self.server_json), encoding='utf-8')
        self.client_config.write_text(json.dumps(self.client_json), encoding='utf-8')

    def until(self, condition, description, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(.05)
        logs = '\n'.join(p.text()[-7000:] for p in self.procs)
        self.fail(f'Timed out: {description}; artifacts: {self.root}\n{logs}')

    def sql(self, query):
        return self.pg.sql(query, self.conn)

    def rows(self, query):
        """Parse unaligned psql output (`-A -t`) into field lists."""
        return [line.split('|') for line in self.sql(query).splitlines() if line]

    def start_server(self):
        self.server_number += 1
        self.server = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config,
                                     self.root, f'scheduler-{self.server_number}', self.pg.env)
        self.procs.append(self.server)
        self.until(lambda: 'scheduler started' in self.server.text() or self.server.process.poll() is not None,
                   'scheduler startup')
        self.assertIsNone(self.server.process.poll(), self.server.text())

    def start_client(self):
        self.client_number += 1
        self.client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', self.client_config,
                                     self.root, f'client-{self.client_number}', self.pg.env)
        self.procs.append(self.client)
        self.until(lambda: 'registered: plugin_type=' in self.client.text() or self.client.process.poll() is not None,
                   'client registration')
        self.assertIsNone(self.client.process.poll(), self.client.text())

    def start_run(self):
        previous = self.server.text().count('run started:')
        self.server.send('run coverage d1')
        self.until(lambda: self.server.text().count('run started:') > previous, 'run start')
        return re.findall(r'run started: (\S+)', self.server.text())[-1]

    def management_command(self, **body):
        request = Request(self.management_url + '/commands',
                          data=json.dumps(body).encode('utf-8'),
                          headers={'Content-Type': 'application/json'})
        try:
            response = urlopen(request, timeout=10)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def management_status(self):
        with urlopen(self.management_url + '/status', timeout=3) as response:
            return json.load(response)

    def finished(self, run_id):
        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run_id)) == 'finished',
                   f'durable finish of {run_id}', timeout=25)

    def fault_trigger(self, expression, table='runs'):
        self.sql("CREATE TABLE fault_gate(enabled boolean NOT NULL); INSERT INTO fault_gate VALUES(true);"
                 "CREATE FUNCTION inject_fault() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
                 f"IF ({expression}) AND (SELECT enabled FROM fault_gate) THEN "
                 "RAISE EXCEPTION 'runtime injected persistence failure'; END IF; RETURN NEW; END $$;"
                 f"CREATE TRIGGER runtime_failure BEFORE INSERT OR UPDATE ON {table} "
                 "FOR EACH ROW EXECUTE FUNCTION inject_fault();")

    def test_forced_rerun_queues_and_starts_after_original_run(self):
        self.start_server()
        self.start_client()
        code, first = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1')
        self.assertEqual(200, code, first)
        self.assertEqual('running', first['status'])
        code, second = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1', force_rerun=True,
            force_rerun_reason='repeat the same workload after the current run completes')
        self.assertEqual(200, code, second)
        self.assertEqual(first['run_id'], second['rerun_of'])
        self.assertEqual('queued', second['status'])
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(second['run_id'])),
            'queued')
        self.finished(first['run_id'])
        self.finished(second['run_id'])
        starts = re.findall(r'run started: (\S+)', self.server.text())
        self.assertLess(starts.index(first['run_id']), starts.index(second['run_id']))

    def test_queue_survives_restart_behind_restored_active_run(self):
        self.configure(delay=3500)
        self.start_server()
        self.start_client()
        code, first = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1')
        self.assertEqual(200, code, first)
        code, second = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1', force_rerun=True,
            force_rerun_reason='repeat the workload after recovery of the active run')
        self.assertEqual(200, code, second)
        self.assertEqual('queued', second['status'])
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='acked'") == '1',
                   'active task delivery before queue recovery')
        self.server.crash()
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(second['run_id'])),
            'queued')
        self.start_server()
        self.until(lambda: 'run restored: ' + first['run_id'] in self.server.text(),
                   'active run restored before queued work')
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(second['run_id'])),
            'queued')
        self.finished(first['run_id'])
        self.finished(second['run_id'])

    def test_multiplugin_client_runs_independent_queues_and_recovers_both_after_restart(self):
        self.configure(delay=2200, client_timeout_ms=1000)
        coverage_server = dict(self.server_json['plugins']['coverage'])
        coverage_client = dict(self.client_json['plugins']['coverage'])
        self.use_stpcompare()
        self.server_json['plugins']['coverage'] = coverage_server
        self.client_json['plugins']['coverage'] = coverage_client
        self.write_configs()
        self.start_server()
        self.start_client()
        self.until(lambda: len(self.management_status()['clients']) == 2,
                   'two isolated plugins registered on one physical client')
        clients = self.management_status()['clients']
        self.assertEqual({'runtime-client/coverage', 'runtime-client/stpcompare'},
                         {client['worker_id'] for client in clients})
        self.assertEqual({'runtime-client'}, {client['physical_client_id'] for client in clients})
        self.assertEqual({'coverage', 'stpcompare'}, {client['plugin_key'] for client in clients})
        runs = {}
        for plugin in ('coverage', 'stpcompare'):
            code, first = self.management_command(
                action='run', plugin_type=plugin, dataset_version='d1', worker_ids=['runtime-client'])
            self.assertEqual(200, code, first)
            code, queued = self.management_command(
                action='run', plugin_type=plugin, dataset_version='d1', worker_ids=['runtime-client'],
                force_rerun=True, force_rerun_reason='multi-plugin durable queue recovery')
            self.assertEqual(200, code, queued)
            self.assertEqual('queued', queued['status'])
            runs[plugin] = (first['run_id'], queued['run_id'])
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='acked'") == '2',
                   'both plugin tasks executing concurrently')
        self.assertEqual('2', self.sql("SELECT count(*) FROM runs WHERE status='running'"))
        self.assertEqual('2', self.sql("SELECT count(*) FROM runs WHERE status='queued'"))
        self.server.crash()
        self.client.crash()
        self.start_server()
        self.start_client()
        for plugin, (first, queued) in runs.items():
            self.assertIn('run restored: ' + first, self.server.text())
            self.finished(first)
            self.finished(queued)
            for run in (first, queued):
                self.assertEqual('runtime-client/' + plugin, self.sql(
                    'SELECT DISTINCT client_identity FROM tasks WHERE run_id=' + sql_text(run)))
                self.assertEqual('1', self.sql(
                    'SELECT count(*) FROM plugin_task_completions WHERE run_id=' + sql_text(run)))
        self.assertEqual('4', self.sql("SELECT count(*) FROM runs WHERE status='finished'"))
        self.assertEqual('0', self.sql("SELECT count(*) FROM tasks WHERE status<>'reported'"))

    def test_finished_duplicate_is_blocked_and_forced_rerun_is_audited(self):
        self.configure(delay=100)
        self.start_server()
        self.start_client()
        command = {'action': 'run', 'plugin_type': 'coverage', 'dataset_version': 'd1'}
        code, missing = self.management_command(
            **command, force_rerun=True, force_rerun_reason='there is no previous run')
        self.assertEqual(409, code, missing)
        self.assertEqual('0', self.sql('SELECT count(*) FROM runs'))
        code, first = self.management_command(**command)
        self.assertEqual(200, code, first)
        self.finished(first['run_id'])
        for params in ({}, {'params': ''}, {'params': {}}, {'params': ' { } '}, {'params': ' \t\r\n '}):
            with self.subTest(params=params):
                code, duplicate = self.management_command(**command, **params)
                self.assertEqual(409, code, duplicate)
                self.assertTrue(duplicate['duplicate'])
                self.assertEqual(first['run_id'], duplicate['run_id'])
                self.assertEqual('finished', duplicate['status'])
                self.assertEqual('1', self.sql('SELECT count(*) FROM runs'))
        self.assertEqual('t', self.sql('SELECT params=\'\' FROM runs WHERE run_id=' + sql_text(first['run_id'])))
        code, forced = self.management_command(
            **command, force_rerun=True,
            force_rerun_reason='validate after repairing the execution environment')
        self.assertEqual(200, code, forced)
        self.assertEqual(first['run_id'], forced['rerun_of'])
        audit = self.sql(
            "SELECT force_rerun,force_rerun_reason,rerun_of,"
            "work_fingerprint=(SELECT work_fingerprint FROM runs WHERE run_id=" +
            sql_text(first['run_id']) + ") FROM runs WHERE run_id=" +
            sql_text(forced['run_id']))
        self.assertEqual(
            't|validate after repairing the execution environment|' + first['run_id'] + '|t',
            audit)
        self.finished(forced['run_id'])

    def test_business_failure_allows_retry_but_older_success_still_blocks_duplicates(self):
        self.use_stpcompare()
        (self.dataset / 'sample.stp').write_text('STP', encoding='utf-8')
        failure = self.program / 'fail.marker'
        failure.write_text('unavailable execution dependency', encoding='utf-8')
        converter = self.program / 'converter.py'
        converter.write_text(
            'import sys\nfrom pathlib import Path\n'
            'args=dict(zip(sys.argv[1::2],sys.argv[2::2])); '
            'out=Path(args["--output-dir"])\n'
            'if Path(__file__).with_name("fail.marker").exists(): sys.exit(7)\n'
            '(out/"info.txt").write_text("first")\n'
            '(out/"info_new.txt").write_text("second")\n', encoding='utf-8')
        plugin = self.client_json['plugins']['stpcompare']
        plugin.update(dry_run=False, work_dir=str(self.root / 'work'))
        for role in ('baseline', 'candidate'):
            plugin['programs'][role]['arguments'] = [
                str(converter), '--input', '{stp}', '--output-dir', '{outdir}']
        self.write_configs()
        self.start_server()
        self.start_client()
        command = {'action': 'run', 'plugin_type': 'stpcompare', 'dataset_version': 'd1'}
        self.fault_trigger("TG_OP='UPDATE' AND NEW.status='reported'", table='tasks')
        code, first = self.management_command(**command)
        self.assertEqual(200, code, first)
        self.until(lambda: self.sql('SELECT exit_code FROM plugin_task_completions WHERE run_id='
                                   + sql_text(first['run_id'])) == '1',
                   'failed business outcome committed before the framework report')
        self.server.crash()
        self.sql('DROP TRIGGER runtime_failure ON tasks; DROP FUNCTION inject_fault(); '
                 'DROP TABLE fault_gate;')
        self.start_server()
        self.finished(first['run_id'])
        self.assertEqual('reported|1', self.sql('SELECT status,exit_code FROM tasks WHERE run_id='
                                               + sql_text(first['run_id'])))
        code, forced_failure = self.management_command(
            **command, force_rerun=True, force_rerun_reason='diagnose the failed execution')
        self.assertEqual(200, code, forced_failure)
        self.assertEqual(first['run_id'], forced_failure['rerun_of'])
        self.finished(forced_failure['run_id'])
        self.assertEqual('1', self.sql('SELECT exit_code FROM tasks WHERE run_id='
                                      + sql_text(forced_failure['run_id'])))
        # Repair the environment without changing program/data identity.
        failure.unlink()
        code, successful = self.management_command(**command)
        self.assertEqual(200, code, successful)
        self.finished(successful['run_id'])
        self.assertEqual('reported|0', self.sql('SELECT status,exit_code FROM tasks WHERE run_id='
                                               + sql_text(successful['run_id'])))
        failure.write_text('dependency unavailable again', encoding='utf-8')
        code, latest = self.management_command(
            **command, force_rerun=True, force_rerun_reason='verify repaired dependency')
        self.assertEqual(200, code, latest)
        self.assertEqual(successful['run_id'], latest['rerun_of'])
        self.finished(latest['run_id'])
        self.assertEqual('1', self.sql('SELECT exit_code FROM tasks WHERE run_id='
                                      + sql_text(latest['run_id'])))
        self.server.crash()
        self.start_server()
        code, duplicate = self.management_command(**command)
        self.assertEqual(409, code, duplicate)
        self.assertEqual(successful['run_id'], duplicate['run_id'])
        self.assertEqual('finished', duplicate['status'])
        self.assertEqual('4', self.sql('SELECT count(*) FROM runs'))
        # A legacy/cleaned record without task evidence cannot prove success.
        self.sql('DELETE FROM tasks WHERE run_id=' + sql_text(successful['run_id']))
        code, unknown = self.management_command(**command)
        self.assertEqual(200, code, unknown)
        self.finished(unknown['run_id'])
        self.assertEqual('5', self.sql('SELECT count(*) FROM runs'))

    def test_selected_workers_are_preserved_across_active_and_queued_recovery(self):
        self.configure(delay=200)
        self.start_server()
        code, first = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1',
            worker_ids=['selected/coverage'])
        self.assertEqual(200, code, first)
        code, second = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1',
            worker_ids=['later/coverage'], force_rerun=True,
            force_rerun_reason='verify the same workload on the other selected worker')
        self.assertEqual(200, code, second)
        self.assertEqual('queued', second['status'])
        self.server.crash()
        self.start_server()
        with urlopen(self.management_url + '/status', timeout=3) as response:
            status = json.load(response)
        self.assertEqual(['selected/coverage'], status['active_runs'][0]['worker_ids'])
        self.assertEqual(['later/coverage'], status['queued_runs'][0]['worker_ids'])

        # Similar names must not match. The logical base/plugin target must,
        # however, remain valid when the selected machine has one plugin and
        # therefore retains its historical base transport identity.
        self.client_json['client']['worker_id'] = 'selected-other'
        self.write_configs()
        self.start_client()
        time.sleep(.3)
        self.assertEqual('0', self.sql('SELECT count(*) FROM tasks'))
        self.client_json['client']['worker_id'] = 'selected'
        self.write_configs()
        self.start_client()
        self.finished(first['run_id'])
        self.assertEqual('selected', self.sql(
            'SELECT DISTINCT client_identity FROM tasks WHERE run_id=' + sql_text(first['run_id'])))
        self.assertEqual('0', self.sql(
            'SELECT count(*) FROM tasks WHERE run_id=' + sql_text(second['run_id'])))
        self.client_json['client']['worker_id'] = 'later'
        self.write_configs()
        self.start_client()
        self.finished(second['run_id'])
        self.assertEqual('later', self.sql(
            'SELECT DISTINCT client_identity FROM tasks WHERE run_id=' + sql_text(second['run_id'])))
        code, duplicate = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1',
            worker_ids=['different-machine'])
        self.assertEqual(409, code, duplicate)
        self.assertEqual(second['run_id'], duplicate['run_id'])

    def test_manifest_change_blocks_queued_run_across_restart_until_restored(self):
        self.configure(delay=200)
        original_manifest = self.manifest.read_text(encoding='utf-8')
        self.start_server()
        code, first = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1')
        self.assertEqual(200, code, first)
        code, queued = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1', force_rerun=True,
            force_rerun_reason='repeat the original manifest after the active run')
        self.assertEqual(200, code, queued)
        original_identity = self.sql('SELECT work_fingerprint,execution_fingerprint FROM runs WHERE run_id='
                                     + sql_text(queued['run_id']))
        self.manifest.write_text('replacement.stp,1\n', encoding='utf-8')
        self.start_client()
        self.finished(first['run_id'])
        self.until(lambda: self.management_status()['queued_runs'][0]['blocked'],
                   'changed manifest blocks queued planning')
        self.assertIn('restore the original', self.management_status()['queued_runs'][0]['blocked_reason'])
        self.assertEqual('0', self.sql('SELECT count(*) FROM plugin_runs WHERE run_id='
                                      + sql_text(queued['run_id'])))
        self.server.crash()
        self.start_server()
        self.assertTrue(self.management_status()['queued_runs'][0]['blocked'])
        self.assertEqual('queued', self.sql('SELECT status FROM runs WHERE run_id='
                                           + sql_text(queued['run_id'])))
        self.assertEqual(original_identity, self.sql(
            'SELECT work_fingerprint,execution_fingerprint FROM runs WHERE run_id=' + sql_text(queued['run_id'])))
        self.manifest.write_text(original_manifest, encoding='utf-8')
        self.finished(queued['run_id'])
        self.assertEqual('sample.stp', self.sql(
            'SELECT file FROM plugin_run_files WHERE run_id=' + sql_text(queued['run_id'])))

    def test_changed_expected_program_keeps_queue_cancellable_after_restart(self):
        self.configure(delay=200)
        original_manifest = self.manifest.read_text(encoding='utf-8')
        self.start_server()
        code, first = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1')
        self.assertEqual(200, code, first)
        code, queued = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1', force_rerun=True,
            force_rerun_reason='queue a repeat before the expected program is changed')
        self.assertEqual(200, code, queued)
        # Let the existing frozen plan finish while keeping the next one queued.
        self.manifest.write_text('hold-next-plan.stp,1\n', encoding='utf-8')
        self.start_client()
        self.finished(first['run_id'])
        self.server.crash()
        self.manifest.write_text(original_manifest, encoding='utf-8')
        self.server_json['plugins']['coverage']['expected_program_version'] = 'v2'
        self.write_configs()
        self.start_server()
        status = self.management_status()
        self.assertEqual([], status['active_runs'])
        self.assertTrue(status['queued_runs'][0]['blocked'])
        code, cancelled = self.management_command(
            action='stop', plugin_type='coverage', run_id=queued['run_id'])
        self.assertEqual(200, code, cancelled)
        self.assertEqual('cancelled', cancelled['status'])
        self.assertEqual([], self.management_status()['queued_runs'])
        self.assertEqual('0', self.sql('SELECT count(*) FROM plugin_runs WHERE run_id='
                                      + sql_text(queued['run_id'])))

    def test_active_recovery_checks_full_plugin_configuration_snapshot(self):
        self.configure(delay=100)
        self.start_server()
        run_id = self.start_run()
        identity = self.sql('SELECT execution_fingerprint FROM runs WHERE run_id=' + sql_text(run_id))
        self.assertEqual(64, len(identity))
        self.server.crash()
        self.server_json['plugins']['coverage']['task_size'] = 2
        self.write_configs()
        rejected = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config,
                                   self.root, 'changed-plugin-configuration', self.pg.env)
        self.procs.append(rejected)
        self.assertNotEqual(0, rejected.process.wait(timeout=10))
        self.assertIn('recovery blocked; original progress retained', rejected.text())
        self.assertEqual('running', self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run_id)))
        self.assertEqual('0', self.sql('SELECT count(*) FROM tasks'))
        self.server_json['plugins']['coverage']['task_size'] = 1
        self.write_configs()
        self.start_server()
        self.start_client()
        self.finished(run_id)

    def test_missing_program_identity_does_not_false_positive_as_duplicate(self):
        self.configure(delay=100)
        self.server_json['plugins']['coverage'].pop('expected_program_version')
        self.write_configs()
        self.start_server()
        self.start_client()
        command = {'action': 'run', 'plugin_type': 'coverage', 'dataset_version': 'd1'}
        code, first = self.management_command(**command)
        self.assertEqual(200, code, first)
        self.finished(first['run_id'])
        code, second = self.management_command(**command)
        self.assertEqual(200, code, second)
        self.assertNotEqual(first['run_id'], second['run_id'])
        self.assertIn('duplicate protection is disabled', self.server.text())
        self.finished(second['run_id'])

    def _check_program_artifact_versions(self, plugin_type):
        self.configure(delay=100)
        if plugin_type == 'stpcompare':
            self.use_stpcompare()
        legacy_config = json.dumps(self.client_json)
        # This is the publisher's identity format, derived from immutable
        # program artifact bytes, independent of its human version.txt label.
        versions = ['sha256:' + hashlib.sha256(content).hexdigest()
                    for content in (b'program artifact A', b'program artifact B',
                                    b'program artifact C')]
        combinations = ([(versions[0],), (versions[1],)] if plugin_type == 'coverage'
                        else [(versions[0], versions[1]), (versions[0], versions[2]),
                              (versions[2], versions[2]), (versions[1], versions[0])])

        def configure_versions(selected):
            server_plugin = self.server_json['plugins'][plugin_type]
            client_plugin = self.client_json['plugins'][plugin_type]
            if plugin_type == 'coverage':
                server_plugin['expected_program_version'] = selected[0]
                client_plugin['program_version'] = selected[0]
            else:
                for role, version in zip(('baseline', 'candidate'), selected):
                    server_plugin[f'expected_{role}_program_version'] = version
                    client_plugin['programs'][role]['version'] = version
            self.write_configs()

        command = {'action': 'run', 'plugin_type': plugin_type, 'dataset_version': 'd1'}
        runs, fingerprints = [], set()
        for index, selected in enumerate(combinations):
            with self.subTest(plugin=plugin_type, selected=selected):
                configure_versions(selected)
                self.start_server()
                code, created = self.management_command(**command)
                self.assertEqual(200, code, created)
                run = created['run_id']
                runs.append(run)
                if index == 0:
                    legacy_path = self.root / 'legacy-client.json'
                    legacy_path.write_text(legacy_config, encoding='utf-8')
                    legacy = CapturedProcess(OPTIONS.bin_dir / 'client.exe', legacy_path,
                                             self.root, 'legacy-client', self.pg.env)
                    self.procs.append(legacy)
                    self.assertNotEqual(0, legacy.process.wait(timeout=10), legacy.text())
                    self.assertIn('version mismatch', legacy.text())
                    self.assertEqual('0', self.sql('SELECT count(*) FROM tasks'))
                    self.assertEqual([], self.management_status()['clients'])
                self.start_client()
                self.finished(run)
                fingerprint = self.sql('SELECT work_fingerprint FROM runs WHERE run_id=' + sql_text(run))
                self.assertNotIn(fingerprint, fingerprints,
                                 'each program artifact combination must have a distinct work identity')
                fingerprints.add(fingerprint)
                expected = (selected[0] if plugin_type == 'coverage' else
                            f'baseline={selected[0]};candidate={selected[1]}')
                self.assertEqual(expected, self.sql(
                    'SELECT program_version FROM plugin_runs WHERE run_id=' + sql_text(run)))
                code, duplicate = self.management_command(**command)
                self.assertEqual(409, code, duplicate)
                self.assertTrue(duplicate['duplicate'])
                self.assertEqual(run, duplicate['run_id'])
                self.client.crash()
                self.server.crash()

        # Returning to the original immutable artifact also returns to its
        # persisted success identity; process restarts do not permit repetition.
        configure_versions(combinations[0])
        self.start_server()
        code, duplicate = self.management_command(**command)
        self.assertEqual(409, code, duplicate)
        self.assertEqual(runs[0], duplicate['run_id'])
        self.assertEqual('finished', duplicate['status'])
        self.assertEqual(str(len(combinations)), self.sql('SELECT count(*) FROM runs'))
        self.assertEqual('v1', (self.program / 'version.txt').read_text(encoding='utf-8'))

    def test_coverage_program_artifact_identity_gates_clients_and_deduplicates(self):
        self._check_program_artifact_versions('coverage')

    def test_stp_program_artifact_pair_gates_clients_and_deduplicates(self):
        self._check_program_artifact_versions('stpcompare')

    def test_manifest_change_changes_the_work_fingerprint_without_restart(self):
        self.configure(delay=100)
        self.start_server()
        self.start_client()
        command = {'action': 'run', 'plugin_type': 'coverage', 'dataset_version': 'd1'}
        code, first = self.management_command(**command)
        self.assertEqual(200, code, first)
        self.finished(first['run_id'])
        self.manifest.write_text('different.stp,1\n', encoding='utf-8')
        code, second = self.management_command(**command)
        self.assertEqual(200, code, second)
        self.assertNotEqual(first['run_id'], second['run_id'])
        fingerprints = self.rows(
            'SELECT run_id,work_fingerprint FROM runs ORDER BY created_at,run_id')
        self.assertEqual(2, len({row[1] for row in fingerprints}))

    def test_concurrent_duplicate_submissions_create_one_database_run(self):
        self.start_server()
        second_endpoint = f'tcp://127.0.0.1:{unused_port()}'
        second_management = f'http://127.0.0.1:{unused_port()}'
        second_config = self.root / 'scheduler-second.json'
        settings = json.loads(json.dumps(self.server_json))
        settings['server']['router_endpoint'] = second_endpoint
        settings['server']['management_endpoint'] = second_management
        second_config.write_text(json.dumps(settings), encoding='utf-8')
        second_server = CapturedProcess(
            OPTIONS.bin_dir / 'scheduler.exe', second_config, self.root,
            'scheduler-concurrent', self.pg.env)
        self.procs.append(second_server)
        self.until(lambda: 'scheduler started' in second_server.text() or
                   second_server.process.poll() is not None, 'second scheduler startup')
        self.assertIsNone(second_server.process.poll(), second_server.text())

        command = {'action': 'run', 'plugin_type': 'coverage', 'dataset_version': 'd1'}
        def submit(url):
            request = Request(url + '/commands', data=json.dumps(command).encode('utf-8'),
                              headers={'Content-Type': 'application/json'})
            try:
                response = urlopen(request, timeout=10)
            except HTTPError as error:
                response = error
            with response:
                return response.status, json.load(response)

        with ThreadPoolExecutor(max_workers=2) as workers:
            results = list(workers.map(submit, (self.management_url, second_management)))
        self.assertEqual([200, 409], sorted(code for code, _ in results))
        created = next(body for code, body in results if code == 200)
        duplicate = next(body for code, body in results if code == 409)
        self.assertTrue(duplicate['duplicate'])
        self.assertEqual(created['run_id'], duplicate['run_id'])
        self.assertEqual('1', self.sql('SELECT count(*) FROM runs'))

    def test_graceful_shutdown_keeps_queued_run_behind_restored_active(self):
        self.start_server()
        code, first = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1')
        self.assertEqual(200, code, first)
        code, second = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1', force_rerun=True,
            force_rerun_reason='repeat the workload after graceful shutdown and recovery')
        self.assertEqual(200, code, second)
        self.assertEqual('queued', second['status'])
        self.server.close()
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(first['run_id'])),
            'stopped')
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(second['run_id'])),
            'queued')
        self.start_server()
        self.until(lambda: 'run restored: ' + first['run_id'] in self.server.text(),
                   'active run restored after graceful shutdown')
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(second['run_id'])),
            'queued')
        self.start_client()
        self.finished(first['run_id'])
        self.finished(second['run_id'])

    def test_stop_then_changed_manifest_run_and_wakeup_follow_command_order(self):
        self.start_server()
        first = self.start_run()
        self.manifest.write_text('second.stp,1\n', encoding='utf-8')
        self.server.send('stop coverage', 'run coverage d1', 'wakeup coverage')
        self.until(lambda: self.server.text().count('run started:') == 2, 'second run after stop')
        text = self.server.text()
        self.assertNotIn('run already active', text)
        self.assertLess(text.index('run stopped: ' + first), text.rindex('run started:'))
        self.start_client()
        second = re.findall(r'run started: (\S+)', text)[-1]
        self.finished(second)
        self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(first)),
                         'cancelled', 'a replacement run must supersede the stopped run')
        self.server.crash()
        self.start_server()
        self.assertNotIn('run restored: ' + first, self.server.text(),
                         'a superseded stopped run must not revive after restart')

    def test_pause_maintains_heartbeat_and_server_restart_recovers_session(self):
        self.start_server()
        self.start_client()
        run = self.start_run()
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='acked'") == '1', 'task delivery')
        self.server.send('stop coverage')
        self.until(lambda: 'STOP received' in self.client.text(), 'client pause')
        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)) == 'stopped',
                   'durable stopped run before restart')
        time.sleep(1.5)  # More than twice the offline threshold, while paused.
        self.assertNotRegex(self.server.text(), r'client offline:|client .* offline|client .* timed out')
        self.server.send('clients')
        self.until(lambda: 'registered=yes' in self.server.text(), 'paused registered session')
        registrations = self.client.text().count('registered: plugin_type=')
        self.server.crash()
        self.start_server()
        self.until(lambda: 'run restored: ' + run in self.server.text(),
                   'stopped run restoration after scheduler restart')
        self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)),
                         'running', 'restored stopped run must become active')
        self.until(lambda: self.client.text().count('registered: plugin_type=') > registrations,
                   'paused client registration after scheduler restart')
        self.server.send('wakeup coverage')
        self.until(lambda: 'wakeup received' in self.client.text(), 'wake after restart')
        self.finished(run)

    def test_failed_replacement_preserves_stopped_run_for_restart_recovery(self):
        original_manifest = self.manifest.read_text(encoding='utf-8')
        self.start_server()
        old_run = self.start_run()
        self.server.send('stop coverage')
        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(old_run))
                   == 'stopped', 'original run stopped before replacement')

        self.manifest.write_text('', encoding='utf-8')
        self.server.send('run coverage d1')
        self.until(lambda: 'createRun: plan failed for coverage' in self.server.text(),
                   'replacement plan failure')
        self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(old_run)),
                         'stopped', 'failed replacement must preserve the resumable run')
        self.assertEqual(
            self.sql("SELECT count(*) FROM runs WHERE run_id<>" + sql_text(old_run) +
                     " AND params='' AND status='failed'"),
            '1', 'the rejected replacement must be recorded as failed')

        self.manifest.write_text(original_manifest, encoding='utf-8')
        self.server.crash()
        self.start_server()
        self.until(lambda: 'run restored: ' + old_run in self.server.text(),
                   'original stopped run restoration after failed replacement')
        self.start_client()
        self.finished(old_run)

    def test_restart_interrupts_unactivated_replacement_and_recovers_stopped_run(self):
        original_manifest = self.manifest.read_text(encoding='utf-8')
        self.start_server()
        old_run = self.start_run()
        self.server.send('stop coverage')
        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(old_run))
                   == 'stopped', 'original run stopped before interrupted replacement')

        lock_name = 'runtime_planning_lock_' + uuid.uuid4().hex
        lock_log = self.root / 'planning-lock.log'
        lock_output = lock_log.open('wb')
        lock_process = subprocess.Popen(
            [str(self.pg.program('psql')), '-X', '--no-password', '--set=ON_ERROR_STOP=1',
             '-q', '-A', '-t', '-d', self.conn + f' application_name={lock_name}'],
            cwd=self.pg.root, env=self.pg.env, stdin=subprocess.PIPE, stdout=lock_output,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            lock_process.stdin.write(
                b'BEGIN;\nLOCK TABLE coverage_checkpoints IN ACCESS EXCLUSIVE MODE;\n')
            lock_process.stdin.flush()
            held_lock = (
                "SELECT count(*) FROM pg_locks AS lock "
                "JOIN pg_stat_activity AS activity USING(pid) "
                f"WHERE activity.application_name={sql_text(lock_name)} "
                "AND lock.relation='coverage_checkpoints'::regclass "
                "AND lock.mode='AccessExclusiveLock' AND lock.granted")
            self.until(lambda: lock_process.poll() is not None or self.sql(held_lock) == '1',
                       'coverage checkpoint planning lock')
            self.assertIsNone(
                lock_process.poll(), lock_log.read_text(encoding='utf-8', errors='replace'))

            self.manifest.write_text('replacement.stp,1\n', encoding='utf-8')
            self.server.send('run coverage d1')
            self.until(
                lambda: self.sql(
                    "SELECT count(*) FROM runs WHERE run_id<>" + sql_text(old_run) +
                    " AND status='planning'") == '1',
                'replacement persisted before its coverage plan can commit')
            planning_run = self.sql(
                "SELECT run_id FROM runs WHERE run_id<>" + sql_text(old_run) +
                " AND status='planning'")
            self.assertNotIn('run started: ' + planning_run, self.server.text())
            self.server.crash()
        finally:
            if lock_process.poll() is None:
                try:
                    lock_process.stdin.write(b'ROLLBACK;\n\\q\n')
                    lock_process.stdin.flush()
                    lock_process.wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    if lock_process.poll() is None:
                        lock_process.terminate()
                    try:
                        lock_process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        lock_process.kill()
                        lock_process.wait(timeout=10)
            if lock_process.stdin:
                try:
                    lock_process.stdin.close()
                except OSError:
                    pass
            lock_output.close()
            self.until(
                lambda: self.sql(
                    "SELECT count(*) FROM pg_stat_activity WHERE application_name="
                    + sql_text(lock_name)) == '0',
                'planning lock database session cleanup')

        self.manifest.write_text(original_manifest, encoding='utf-8')
        self.start_server()

        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' +
                                    sql_text(planning_run)) == 'interrupted',
                   'unactivated replacement closed after scheduler restart')
        self.until(lambda: 'run restored: ' + old_run in self.server.text(),
                   'stopped run restoration after interrupted replacement')
        self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(old_run)),
                         'running', 'the original run must remain the active recovery target')
        self.start_client()
        self.finished(old_run)

    def test_save_run_failure_prevents_dispatch_and_allows_retry(self):
        self.start_server()
        self.start_client()
        self.fault_trigger("TG_OP='INSERT'")
        self.server.send('run coverage d1')
        self.until(lambda: 'runtime injected persistence failure' in self.server.text(), 'SaveRun failure')
        time.sleep(.4)
        self.assertEqual(self.sql('SELECT count(*) FROM runs'), '0')
        self.assertEqual(self.sql('SELECT count(*) FROM tasks'), '0')
        self.assertNotIn('task batch', self.client.text())
        self.sql('UPDATE fault_gate SET enabled=false')
        run = self.start_run()
        self.finished(run)

    def test_finish_run_failure_remains_running_and_retries(self):
        self.start_server()
        self.start_client()
        self.fault_trigger("TG_OP='UPDATE' AND NEW.status='finished'")
        run = self.start_run()
        self.until(lambda: 'runtime injected persistence failure' in self.server.text(), 'FinishRun failure')
        self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)), 'running')
        self.assertNotIn('run finished: ' + run, self.server.text())
        self.sql('UPDATE fault_gate SET enabled=false')
        self.finished(run)

    def test_database_restart_recovers_framework_and_plugin_connections(self):
        self.start_server()
        self.start_client()
        run = self.start_run()
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='acked'") == '1', 'task delivery before database restart')
        self.pg.run([self.pg.program('pg_ctl'), '-D', self.pg.data, '-m', 'fast', '-w', 'stop'])
        time.sleep(.4)
        self.pg.run([self.pg.program('pg_ctl'), '-D', self.pg.data, '-l', self.pg.root / 'postgres.log',
                     '-w', 'start'], output_file=self.pg.root / 'restart.log')
        self.assertEqual(Path(self.pg.sql('SHOW data_directory')).resolve(), self.pg.data.resolve())
        self.finished(run)
        # Also prove new work can be accepted through the recovered connection.
        code, second = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1', force_rerun=True,
            force_rerun_reason='verify run creation after the database connection recovers')
        self.assertEqual(200, code, second)
        self.assertEqual(run, second['rerun_of'])
        self.finished(second['run_id'])

    def test_final_file_commit_failure_is_retried(self):
        self.configure(file_store=True)
        self.start_server()
        run = self.start_run()
        # New versions use the durable run metadata as the finish barrier;
        # older versions rewrite checkpoint.bin on the final report.
        metadata = self.store / run / 'run-metadata.json'
        target = metadata if metadata.exists() else self.store / run / 'checkpoint.bin'
        self.assertTrue(target.is_file())
        with ExclusiveFile(target):
            self.start_client()
            if metadata.exists():
                self.until(lambda: len(list((self.store / run / 'completed').glob('*.done'))) == 1,
                           'final completion journal written while finish metadata is locked')
                time.sleep(.4)
            else:
                self.until(lambda: 'failed to save checkpoint' in self.server.text(), 'checkpoint commit failure')
            self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)), 'running')
            self.assertNotIn('run finished: ' + run, self.server.text())
        self.finished(run)

    def test_initial_database_connection_failure_exits_cleanly(self):
        self.server_json['server']['db_conn'] = f'host=127.0.0.1 port={unused_port()} user=runtime dbname=postgres connect_timeout=1'
        self.write_configs()
        process = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config,
                                  self.root, 'startup-failure', self.pg.env)
        self.procs.append(process)
        self.assertNotEqual(process.process.wait(timeout=10), 0)
        self.assertNotIn('scheduler started', process.text())

    def test_completion_journal_failure_retries_without_losing_final_report(self):
        self.configure(file_store=True)
        self.start_server()
        run = self.start_run()
        task_id = run + '-t0'
        fingerprint = 14695981039346656037
        for byte in task_id.encode():
            fingerprint = ((fingerprint ^ byte) * 1099511628211) & ((1 << 64) - 1)
        completed = self.store / run / 'completed'
        completed.mkdir(exist_ok=True)
        journal = completed / (str(fingerprint) + '.done')
        journal.write_bytes(b'')
        with ExclusiveFile(journal):
            self.start_client()
            self.until(lambda: 'publish failed' in self.server.text(), 'completion journal failure')
            self.assertEqual(self.sql("SELECT count(*) FROM tasks WHERE status='reported'"), '0')
            self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)), 'running')
        self.finished(run)
        self.assertEqual(json.loads(journal.read_text()),
                         {'version': 1, 'task_id': task_id, 'exit_code': 0})

    def test_recovery_reconciles_completion_committed_before_framework_report(self):
        # Existing completion tables are migrated without inventing success for
        # historical entries that did not record an exit code.
        self.sql('CREATE TABLE plugin_task_completions('
                 'run_id text NOT NULL,plugin_type text NOT NULL,task_id text NOT NULL,'
                 'completed_at timestamptz DEFAULT now(),PRIMARY KEY(run_id,plugin_type,task_id));'
                 "INSERT INTO plugin_task_completions(run_id,plugin_type,task_id) "
                 "VALUES('legacy','coverage','unknown');")
        self.start_server()
        self.assertEqual('t', self.sql("SELECT exit_code IS NULL FROM plugin_task_completions "
                                      "WHERE run_id='legacy'"))
        self.start_client()
        self.fault_trigger("TG_OP='UPDATE' AND NEW.status='reported'", table='tasks')
        run = self.start_run()
        self.until(
            lambda: self.sql(
                "SELECT count(*) FROM plugin_task_completions WHERE run_id=" + sql_text(run)) == '1',
            'plugin completion before blocked framework report')
        self.until(lambda: 'runtime injected persistence failure' in self.server.text(),
                   'blocked framework report')
        self.assertEqual('0', self.sql('SELECT exit_code FROM plugin_task_completions WHERE run_id='
                                      + sql_text(run)))
        self.assertEqual('t', self.sql('SELECT exit_code IS NULL FROM tasks WHERE run_id='
                                      + sql_text(run)))
        task_status_before_recovery = self.sql(
            "SELECT status FROM tasks WHERE run_id=" + sql_text(run))
        self.assertNotEqual(task_status_before_recovery, 'reported')
        self.server.crash()

        self.sql('DROP TRIGGER runtime_failure ON tasks; DROP FUNCTION inject_fault(); '
                 'DROP TABLE fault_gate;')
        self.fault_trigger("TG_OP='UPDATE' AND NEW.status='running'", table='runs')

        self.server_number += 1
        failed_restart = CapturedProcess(
            OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config, self.root,
            f'scheduler-{self.server_number}', self.pg.env)
        self.procs.append(failed_restart)
        self.assertNotEqual(failed_restart.process.wait(timeout=10), 0)
        self.assertIn('cannot persist recovered state for run ' + run, failed_restart.text())
        self.assertNotIn('scheduler started', failed_restart.text())
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)), 'running',
            'failed recovery must leave the original run available for the next start')
        self.assertEqual(
            self.sql("SELECT status FROM tasks WHERE run_id=" + sql_text(run)),
            task_status_before_recovery,
            'failed recovery must roll back task reconciliation and reclaim together')

        self.sql('UPDATE fault_gate SET enabled=false')
        self.start_server()
        self.until(lambda: 'run restored: ' + run in self.server.text(),
                   'run restoration after recovery retry')
        self.finished(run)
        self.assertEqual(
            self.sql("SELECT status,exit_code FROM tasks WHERE run_id=" + sql_text(run)), 'reported|0')
        code, duplicate = self.management_command(
            action='run', plugin_type='coverage', dataset_version='d1')
        self.assertEqual(409, code, duplicate)
        self.assertEqual(run, duplicate['run_id'])

    def test_throwing_plan_marks_framework_run_failed(self):
        entered = self.root / 'probe-entered'
        self.server_json['plugins'] = {
            'probe': {'enabled': True,
                      'dll': str(OPTIONS.bin_dir / 'scheduler_probe.dll'),
                      'entered_file': str(entered), 'throw_plan': True}}
        self.write_configs()
        self.start_server()
        self.server.send('run scheduler-probe d1')
        self.until(lambda: 'plan threw for scheduler-probe' in self.server.text(),
                   'throwing plan handled')
        self.assertEqual(self.sql("SELECT status FROM runs"), 'failed')
        self.assertEqual(self.sql("SELECT count(*) FROM tasks"), '0')
        self.assertNotIn('run started:', self.server.text())

    def test_recovery_query_failure_does_not_look_like_an_empty_database(self):
        self.sql("CREATE FUNCTION reject_recovery_read() RETURNS text LANGUAGE plpgsql AS $$ "
                 "BEGIN RAISE EXCEPTION 'runtime injected recovery read failure'; END $$;"
                 "CREATE VIEW runs AS SELECT reject_recovery_read() AS run_id, 'coverage'::text AS plugin_type,"
                 "'d1'::text AS dataset_version,''::text AS params,'running'::text AS status,"
                 "now() AS created_at,NULL::timestamptz AS finished_at,NULL::text AS work_fingerprint,"
                 "false AS force_rerun,NULL::text AS force_rerun_reason,NULL::text AS rerun_of,"
                 "'[]'::jsonb AS worker_ids,NULL::text AS execution_fingerprint;")
        process = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config,
                                  self.root, 'recovery-failure', self.pg.env)
        self.procs.append(process)
        self.assertNotEqual(process.process.wait(timeout=10), 0)
        self.assertIn('failed to load unfinished runs', process.text())
        self.assertNotIn('scheduler started', process.text())

    def test_server_crash_resumes_task_progress_without_redoing_committed_work(self):
        self.manifest.write_text('one.stp,1\ntwo.stp,1\nthree.stp,1\n', encoding='utf-8')
        self.configure(delay=2500)
        self.start_server()
        self.start_client()
        run = self.start_run()
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='reported'") == '1',
                   'first durable report before the crash')
        committed = {row[0]: row[1] for row in
                     self.rows(f"SELECT task_id,attempt_id FROM tasks WHERE run_id={sql_text(run)}"
                               " AND status='reported'")}
        self.assertEqual(len(committed), 1, 'exactly one committed task before the crash')
        self.until(
            lambda: self.sql(
                f"SELECT count(*) FROM tasks WHERE run_id={sql_text(run)} AND status='acked'") == '1',
            'one uncommitted task before the crash')
        uncommitted = {row[0]: row[1] for row in
                       self.rows(f"SELECT task_id,attempt_id FROM tasks WHERE run_id={sql_text(run)}"
                                 " AND status='acked'")}
        self.server.crash()
        self.client.crash()
        self.start_server()
        self.until(lambda: 'run restored: ' + run in self.server.text(),
                   'run restoration after the crash')
        restored_statuses = {
            row[0]: row[1] for row in self.rows(
                f"SELECT task_id,status FROM tasks WHERE run_id={sql_text(run)}")
        }
        self.assertEqual(
            {task_id: 'reported' for task_id in committed},
            {task_id: restored_statuses.get(task_id) for task_id in committed},
            'durably committed tasks must remain reported during recovery')
        self.assertEqual(
            {task_id: 'reclaimed' for task_id in uncommitted},
            {task_id: restored_statuses.get(task_id) for task_id in uncommitted},
            'unfinished attempts must be reclaimable before a client reconnects')
        self.start_client()
        self.finished(run)
        final = {row[0]: row[1] for row in
                 self.rows(f"SELECT task_id,attempt_id FROM tasks WHERE run_id={sql_text(run)}")}
        self.assertEqual(len(final), 3, 'every manifest entry keeps exactly one durable task row')
        for task_id, attempt in committed.items():
            self.assertEqual(final.get(task_id), attempt,
                             f'committed task {task_id} must not be re-executed after recovery')
        for task_id, attempt in uncommitted.items():
            self.assertNotEqual(final.get(task_id), attempt,
                                f'uncommitted task {task_id} must be re-dispatched after recovery')
        self.assertEqual(
            self.sql(f"SELECT count(*) FROM tasks WHERE run_id={sql_text(run)} AND status='reported'"),
            '3', 'all tasks reported after recovery')

    def test_client_crash_reclaims_task_and_restarted_client_continues(self):
        self.manifest.write_text('one.stp,1\ntwo.stp,1\n', encoding='utf-8')
        self.configure(delay=1500, client_timeout_ms=10000)
        self.start_server()
        self.start_client()
        run = self.start_run()
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='acked'") == '1',
                   'task delivery')
        task_id, attempt = self.rows(
            f"SELECT task_id,attempt_id FROM tasks WHERE run_id={sql_text(run)}"
            " AND status='acked'")[0]
        self.assertEqual(
            self.sql(f"SELECT client_identity FROM tasks WHERE task_id={sql_text(task_id)}"),
            'runtime-client')
        registrations = self.server.text().count('register: client runtime-client')
        self.client.crash()
        server_log_after_crash = len(self.server.text())
        self.assertNotIn('client offline:', self.server.text(),
                         'the crashed client must not time out before immediate restart')
        self.start_client()
        self.until(
            lambda: self.sql(
                f"SELECT attempt_id FROM tasks WHERE task_id={sql_text(task_id)}") != attempt,
            'restarted client receiving a new task attempt')
        self.assertNotIn(
            'client offline:', self.server.text()[server_log_after_crash:],
            'registration takeover must reclaim the task before the offline timeout')
        self.assertGreater(self.server.text().count('register: client runtime-client'), registrations,
                           'the restarted worker must create a new scheduler session')
        self.assertEqual(
            self.sql(f"SELECT client_identity FROM tasks WHERE task_id={sql_text(task_id)}"),
            'runtime-client', 'the task must be re-dispatched to the restarted worker identity')
        self.assertIn('client: task batch run=' + run, self.client.text(),
                      'the restarted client must receive the recovered task')
        self.finished(run)
        self.assertEqual(self.sql(f"SELECT status FROM tasks WHERE task_id={sql_text(task_id)}"),
                         'reported', 'reclaimed task must be completed by the restarted client')
        self.assertNotEqual(
            self.sql(f"SELECT attempt_id FROM tasks WHERE task_id={sql_text(task_id)}"), attempt,
            'the crashed attempt must be reclaimed and re-dispatched as a new attempt')
        self.assertEqual(self.sql(f"SELECT count(*) FROM tasks WHERE run_id={sql_text(run)}"), '2')

    def use_stpcompare(self, *, task_size=1):
        server_plugin = self.server_json['plugins'].pop('coverage')
        server_plugin.update(dll=str(OPTIONS.bin_dir / 'stpcompare.dll'), task_size=task_size)
        server_plugin.pop('expected_program_version', None)
        server_plugin.update(expected_baseline_program_version='v1',
                             expected_candidate_program_version='v1')
        self.server_json['plugins']['stpcompare'] = server_plugin
        client_plugin = self.client_json['plugins'].pop('coverage')
        client_plugin.update(
            dll=str(OPTIONS.bin_dir / 'stpcompare.dll'), table='stp_runtime_results',
            programs={
                'baseline': {'executable': sys.executable, 'version': 'v1',
                             'arguments': ['--input', '{stp}', '--output-dir', '{outdir}']},
                'candidate': {'executable': sys.executable, 'version': 'v1',
                              'arguments': ['--input', '{stp}', '--output-dir', '{outdir}']}})
        self.client_json['plugins']['stpcompare'] = client_plugin

    def test_requested_dataset_rejects_conflict_and_is_persisted_without_config_default(self):
        self.start_server()
        self.server.send('run coverage d2')
        self.until(lambda: self.sql("SELECT count(*) FROM runs WHERE status='failed'") == '1',
                   'conflicting request rejected')
        self.assertEqual(self.sql('SELECT count(*) FROM plugin_runs'), '0')
        self.assertEqual(self.sql('SELECT count(*) FROM tasks'), '0')
        self.server.crash()

        self.server_json['plugins']['coverage'].pop('expected_dataset_version')
        self.write_configs()
        self.start_server()
        run = self.start_run()
        self.assertEqual(self.sql('SELECT dataset_version FROM plugin_runs WHERE run_id=' + sql_text(run)), 'd1')
        self.start_client()
        self.finished(run)

    def test_pre_registered_client_cannot_receive_another_dataset(self):
        self.server_json['plugins']['coverage'].pop('expected_dataset_version')
        self.write_configs()
        self.start_server()
        self.start_client()  # d1 registration succeeds before a run exists.
        self.server.send('run coverage d2')
        self.until(lambda: 'run started:' in self.server.text(), 'd2 run planned')
        run = re.findall(r'run started: (\S+)', self.server.text())[-1]
        time.sleep(.4)
        self.assertEqual(self.sql('SELECT count(*) FROM tasks'), '0')
        self.assertEqual(self.sql('SELECT dataset_version FROM plugin_runs'), 'd2')
        self.client.crash()
        (self.dataset / 'version.txt').write_text('d2', encoding='utf-8')
        self.start_client()
        self.finished(run)

    def test_recovery_blocks_changed_deployment_without_losing_original_progress(self):
        self.use_stpcompare()
        self.manifest.write_text('one.stp,1\ntwo.stp,1\n', encoding='utf-8')
        self.client_json['plugins']['stpcompare']['dry_run_delay_ms'] = 2500
        self.write_configs()
        self.start_server()
        self.start_client()
        self.server.send('run stpcompare d1')
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='reported'") == '1',
                   'first committed STP task')
        run = re.findall(r'run started: (\S+)', self.server.text())[-1]
        committed = self.sql("SELECT task_id||'|'||attempt_id FROM tasks WHERE status='reported'")
        self.server.crash()
        self.client.crash()
        plugin = self.server_json['plugins']['stpcompare']
        original_manifest = self.manifest.read_text(encoding='utf-8')

        for field, value in (('expected_baseline_program_version', 'v2'),
                             ('expected_candidate_program_version', 'v2'),
                             ('expected_dataset_version', 'd2'),
                             ('manifest', 'different.stp,1\n')):
            with self.subTest(changed=field):
                if field == 'manifest':
                    self.manifest.write_text(value, encoding='utf-8')
                else:
                    plugin[field] = value
                self.write_configs()
                rejected = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config,
                                           self.root, 'blocked-' + field, self.pg.env)
                self.procs.append(rejected)
                self.assertNotEqual(rejected.process.wait(timeout=10), 0)
                self.assertIn('recovery blocked; original progress retained', rejected.text())
                self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)), 'running')
                self.assertEqual(self.sql("SELECT task_id||'|'||attempt_id FROM tasks WHERE status='reported'"), committed)
                if field == 'manifest':
                    self.manifest.write_text(original_manifest, encoding='utf-8')
                else:
                    plugin[field] = 'd1' if field == 'expected_dataset_version' else 'v1'

        self.write_configs()
        self.start_server()
        self.start_client()
        self.finished(run)
        self.assertEqual(self.sql('SELECT program_version,dataset_version FROM plugin_runs'),
                         'baseline=v1;candidate=v1|d1')

    def test_stp_client_crash_reuses_persisted_success_and_reports_consistent_outcomes(self):
        self.use_stpcompare(task_size=2)
        self.manifest.write_text('a.stp,3\nb.stp,3\n', encoding='utf-8')
        for filename in ('a.stp', 'b.stp'):
            (self.dataset / filename).write_text('STP', encoding='utf-8')
        converter = self.program / 'converter.py'
        converter.write_text(
            'import sys,time\nfrom pathlib import Path\n'
            'role=sys.argv[1]; args=dict(zip(sys.argv[2::2],sys.argv[3::2])); '
            'source=Path(args["--input"]); out=Path(args["--output-dir"]); '
            'marker=source.parent/("first-success-"+role+".marker")\n'
            'if source.name=="a.stp":\n'
            ' if marker.exists(): sys.exit(7)\n'
            ' marker.write_text("done")\n'
            'else: time.sleep(2)\n'
            '(out/"info.txt").write_text(role+" current")\n'
            '(out/"info_new.txt").write_text(role+" converted")\n', encoding='utf-8')
        plugin = self.client_json['plugins']['stpcompare']
        plugin.update(dry_run=False, work_dir=str(self.root / 'work'))
        for role in ('baseline', 'candidate'):
            plugin['programs'][role].update(
                executable=sys.executable,
                arguments=[str(converter), role, '--input', '{stp}', '--output-dir', '{outdir}'])
        self.write_configs()
        self.start_server()
        self.start_client()
        self.server.send('run stpcompare d1')
        self.until(lambda: self.sql("SELECT count(*) FROM plugin_file_outcomes "
                                    "WHERE file='a.stp' AND status='success'") == '1',
                   'first STP file persisted before client crash')
        run = re.findall(r'run started: (\S+)', self.server.text())[-1]
        self.client.crash()
        self.start_client()
        self.finished(run)
        self.assertEqual(self.sql('SELECT status,exit_code FROM tasks'), 'reported|0')
        self.assertEqual(self.sql('SELECT file,status FROM plugin_file_outcomes ORDER BY file'),
                         'a.stp|success\nb.stp|success')
        self.assertNotIn('converter failed', self.client.text())

    def assert_plugin_read_failures_preserve_progress(self, plugin_type):
        self.configure(delay=1800)
        self.manifest.write_text('one.stp,1\ntwo.stp,1\n', encoding='utf-8')
        if plugin_type == 'stpcompare':
            self.use_stpcompare()
        self.write_configs()
        self.start_server()
        code, started = self.management_command(
            action='run', plugin_type=plugin_type, dataset_version='d1')
        self.assertEqual(200, code, started)
        run = started['run_id']
        self.start_client()
        self.until(lambda: self.sql("SELECT count(*) FROM tasks WHERE status='reported'") == '1',
                   'one committed task before recovery read failures')
        self.server.crash()
        self.client.crash()
        committed = self.sql("SELECT task_id,attempt_id FROM tasks WHERE status='reported'")
        self.assertEqual(1, len(committed.splitlines()))
        self.sql("CREATE FUNCTION reject_plugin_recovery_read(value anyelement) RETURNS anyelement "
                 "LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'injected plugin recovery read failure'; END $$;")
        checkpoint_table = ('coverage_checkpoints' if plugin_type == 'coverage'
                            else 'stp_compare_checkpoints')
        projections = (
            (checkpoint_table, 'run_id,reject_plugin_recovery_read(blob) AS blob,updated_at',
             'cannot read checkpoint'),
            ('plugin_task_completions',
             'run_id,plugin_type,reject_plugin_recovery_read(task_id) AS task_id,exit_code,completed_at',
             'cannot read completion journal'))
        for table, projection, expected_error in projections:
            with self.subTest(plugin=plugin_type, failing_read=table):
                # Initialization still sees the relation and its complete schema;
                # only SELECT of the durable checkpoint/journal fails.
                saved = table + '_recovery_saved'
                self.sql(f'ALTER TABLE {table} RENAME TO {saved}; '
                         f'CREATE VIEW {table} AS SELECT {projection} FROM {saved};')
                try:
                    self.server_number += 1
                    failed = CapturedProcess(
                        OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config, self.root,
                        f'scheduler-{self.server_number}', self.pg.env)
                    self.procs.append(failed)
                    self.assertNotEqual(0, failed.process.wait(timeout=10), failed.text())
                    self.assertIn('recovery blocked; original progress retained', failed.text())
                    self.assertIn(expected_error, failed.text())
                    self.assertNotIn('scheduler started', failed.text())
                    self.assertEqual('running', self.sql(
                        'SELECT status FROM runs WHERE run_id=' + sql_text(run)))
                    self.assertEqual(committed, self.sql(
                        "SELECT task_id,attempt_id FROM tasks WHERE status='reported'"))
                finally:
                    self.sql(f'DROP VIEW {table}; ALTER TABLE {saved} RENAME TO {table};')
        self.sql('DROP FUNCTION reject_plugin_recovery_read(anyelement);')
        self.start_server()
        self.assertIn('run restored: ' + run, self.server.text())
        self.start_client()
        self.finished(run)
        task_id, attempt = committed.split('|')
        self.assertEqual(attempt, self.sql(
            'SELECT attempt_id FROM tasks WHERE task_id=' + sql_text(task_id)))
        self.assertEqual('2', self.sql("SELECT count(*) FROM tasks WHERE status='reported'"))

    def test_coverage_recovery_read_failures_preserve_committed_progress(self):
        self.assert_plugin_read_failures_preserve_progress('coverage')

    def test_stpcompare_recovery_read_failures_preserve_committed_progress(self):
        self.assert_plugin_read_failures_preserve_progress('stpcompare')

    def test_unrestorable_run_is_closed_instead_of_blocking_startup(self):
        self.sql("CREATE TABLE IF NOT EXISTS runs (run_id text PRIMARY KEY, plugin_type text,"
                 " dataset_version text, params text, status text NOT NULL DEFAULT 'running',"
                 " created_at timestamptz DEFAULT now(), finished_at timestamptz);")
        poison = 'run-without-checkpoint'
        self.sql("INSERT INTO runs(run_id,plugin_type,dataset_version,params,status) "
                 f"VALUES({sql_text(poison)},'coverage','d1','','running')")
        self.start_server()
        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(poison))
                   == 'interrupted', 'unrestorable run closed as interrupted')
        self.assertNotIn('run restored: ' + poison, self.server.text())
        # The scheduler must still accept new work after closing the poison row.
        self.start_client()
        run = self.start_run()
        self.finished(run)

    def test_newer_unrestorable_run_does_not_hide_older_stopped_run(self):
        self.start_server()
        old_run = self.start_run()
        self.server.send('stop coverage')
        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(old_run))
                   == 'stopped', 'older run stopped before recovery ordering test')

        poison = 'newer-run-without-checkpoint'
        self.sql("INSERT INTO runs(run_id,plugin_type,dataset_version,params,status,created_at) "
                 f"VALUES({sql_text(poison)},'coverage','invalid','','running',now() + interval '1 second')")
        self.server.crash()
        self.start_server()

        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(poison))
                   == 'interrupted', 'newer unrestorable run closed')
        self.until(lambda: 'run restored: ' + old_run in self.server.text(),
                   'older stopped run restored in the same startup')
        self.start_client()
        self.finished(old_run)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=ROOT / 'build/rt/bin/Debug')
    parser.add_argument('--pg-bin', type=Path, default=ROOT / 'third_party/pgsql/bin')
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.bin_dir = OPTIONS.bin_dir.resolve()
    OPTIONS.pg_bin = OPTIONS.pg_bin.resolve()
    unittest.main(argv=[__file__, *remaining])
