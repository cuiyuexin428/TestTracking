"""Real scheduler/client regressions using owned processes and private PostgreSQL.

No database connection string is accepted. Configs, logs and file stores are
created below build/test/runtime-integration-*. Failed-case logs are retained.
"""

import argparse
import ctypes
from ctypes import wintypes
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
        for name in ('scheduler.exe', 'client.exe', 'coverage.dll', 'scheduler_probe.dll'):
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
        plugin = {'dll': str(OPTIONS.bin_dir / 'coverage.dll'), 'enabled': True,
                  'store_type': 'file' if file_store else 'postgres',
                  'store_dir': str(self.store), 'db_conn': self.conn}
        self.server_json = {
            'server': {'router_endpoint': self.endpoint, 'poll_timeout_ms': 10,
                       'process_threads': 2, 'client_timeout_ms': client_timeout_ms,
                       'task_lease_timeout_ms': 0},
            'plugins': {'coverage': dict(plugin, stp_manifest=str(self.manifest), task_size=1)}}
        if framework_db:
            self.server_json['server']['db_conn'] = self.conn
        self.client_json = {
            'scheduler': {'endpoint': self.endpoint, 'poll_timeout_ms': 10},
            'client': {'worker_id': 'runtime-client', 'heartbeat_interval_ms': 100,
                       'task_threads': 1, 'request_backoff_initial_ms': 20,
                       'request_backoff_max_ms': 100, 'request_timeout_ms': 500,
                       'register_timeout_ms': 1000, 'register_max_attempts': 3},
            'plugins': {'coverage': dict(plugin, dry_run=True, dry_run_delay_ms=delay,
                                        store_cov=False, export_dir=str(self.root / 'reports'))}}
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

    def start_run(self, version='d1'):
        previous = self.server.text().count('run started:')
        self.server.send(f'run coverage {version}')
        self.until(lambda: self.server.text().count('run started:') > previous, 'run start')
        return re.findall(r'run started: (\S+)', self.server.text())[-1]

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

    def test_stop_then_run_and_wakeup_follow_command_order(self):
        self.start_server()
        first = self.start_run()
        self.server.send('stop coverage', 'run coverage d2', 'wakeup coverage')
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
        old_run = self.start_run('original')
        self.server.send('stop coverage')
        self.until(lambda: self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(old_run))
                   == 'stopped', 'original run stopped before replacement')

        self.manifest.write_text('', encoding='utf-8')
        self.server.send('run coverage invalid-replacement')
        self.until(lambda: 'createRun: plan failed for coverage' in self.server.text(),
                   'replacement plan failure')
        self.assertEqual(self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(old_run)),
                         'stopped', 'failed replacement must preserve the resumable run')
        self.assertEqual(
            self.sql("SELECT count(*) FROM runs WHERE dataset_version='invalid-replacement' "
                     "AND status='failed'"),
            '1', 'the rejected replacement must be recorded as failed')

        self.manifest.write_text(original_manifest, encoding='utf-8')
        self.server.crash()
        self.start_server()
        self.until(lambda: 'run restored: ' + old_run in self.server.text(),
                   'original stopped run restoration after failed replacement')
        self.start_client()
        self.finished(old_run)

    def test_restart_interrupts_unactivated_replacement_and_recovers_stopped_run(self):
        self.start_server()
        old_run = self.start_run('original')
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

            self.server.send('run coverage replacement')
            self.until(
                lambda: self.sql(
                    "SELECT count(*) FROM runs WHERE dataset_version='replacement' "
                    "AND status='planning'") == '1',
                'replacement persisted before its coverage plan can commit')
            planning_run = self.sql(
                "SELECT run_id FROM runs WHERE dataset_version='replacement' "
                "AND status='planning'")
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
        self.server.send('run coverage blocked')
        self.until(lambda: 'runtime injected persistence failure' in self.server.text(), 'SaveRun failure')
        time.sleep(.4)
        self.assertEqual(self.sql('SELECT count(*) FROM runs'), '0')
        self.assertEqual(self.sql('SELECT count(*) FROM tasks'), '0')
        self.assertNotIn('task batch', self.client.text())
        self.sql('UPDATE fault_gate SET enabled=false')
        run = self.start_run('retry')
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
        second = self.start_run('after-restart')
        self.finished(second)

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
        self.assertEqual(journal.read_text(), task_id)

    def test_recovery_reconciles_completion_committed_before_framework_report(self):
        self.start_server()
        self.start_client()
        self.fault_trigger("TG_OP='UPDATE' AND NEW.status='reported'", table='tasks')
        run = self.start_run()
        self.until(
            lambda: self.sql(
                "SELECT count(*) FROM plugin_task_completions WHERE run_id=" + sql_text(run)) == '1',
            'plugin completion before blocked framework report')
        self.until(lambda: 'runtime injected persistence failure' in self.server.text(),
                   'blocked framework report')
        self.assertNotEqual(
            self.sql("SELECT status FROM tasks WHERE run_id=" + sql_text(run)), 'reported')
        self.server.crash()

        self.server_number += 1
        failed_restart = CapturedProcess(
            OPTIONS.bin_dir / 'scheduler.exe', self.scheduler_config, self.root,
            f'scheduler-{self.server_number}', self.pg.env)
        self.procs.append(failed_restart)
        self.assertNotEqual(failed_restart.process.wait(timeout=10), 0)
        self.assertIn('cannot reconcile completed tasks for run ' + run, failed_restart.text())
        self.assertNotIn('scheduler started', failed_restart.text())
        self.assertEqual(
            self.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)), 'running',
            'failed recovery must leave the original run available for the next start')

        self.sql('UPDATE fault_gate SET enabled=false')
        self.start_server()
        self.until(lambda: 'run restored: ' + run in self.server.text(),
                   'run restoration after split commit')
        self.finished(run)
        self.assertEqual(
            self.sql("SELECT status FROM tasks WHERE run_id=" + sql_text(run)), 'reported')

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
                 "now() AS created_at,NULL::timestamptz AS finished_at;")
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
        self.start_server()
        self.until(lambda: 'run restored: ' + run in self.server.text(),
                   'run restoration after the crash')
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
        old_run = self.start_run('recoverable')
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
