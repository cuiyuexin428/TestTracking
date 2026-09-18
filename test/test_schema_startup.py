"""First deployment may start the scheduler and many clients together."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import URLError
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres
from test_runtime_integration import CapturedProcess, unused_port

ROOT = Path(__file__).resolve().parents[1]


class ConcurrentSchemaStartupTests(unittest.TestCase):
    def test_scheduler_and_both_plugin_clients_initialize_empty_schema_together(self):
        pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.bin_dir / 'scheduler.exe')
        self.addCleanup(pg.close)
        pg.start()
        root = Path(tempfile.mkdtemp(prefix='schema-startup-', dir=ROOT / 'build/test'))
        self.addCleanup(lambda: print(f'  schema startup artifacts: {root}', flush=True))
        manifest = root / 'manifest.txt'
        manifest.write_text('sample.stp,1\n', encoding='utf-8')
        (root / 'version.txt').write_text('d1', encoding='utf-8')
        endpoint = f'tcp://127.0.0.1:{unused_port()}'
        management = f'http://127.0.0.1:{unused_port()}'
        plugins = {name: {'dll': str(OPTIONS.bin_dir / (name + '.dll')), 'enabled': True,
                         'store_type': 'postgres', 'db_conn': pg.conn, 'stp_manifest': str(manifest),
                         'stp_root': str(root), 'program_dir': str(root), 'dry_run': True}
                   for name in ('coverage', 'stpcompare')}
        scheduler_config = root / 'scheduler.json'
        scheduler_config.write_text(json.dumps({'server': {'router_endpoint': endpoint,
            'management_endpoint': management, 'db_conn': pg.conn}, 'plugins': plugins}), encoding='utf-8')

        # Hold the same schema lock so all processes meet at their first DDL.
        # Their own transaction owns/relinquishes the lock; no retry loop hides
        # a process that failed initialization.
        barrier = subprocess.Popen([str(pg.program('psql')), '-X', '-A', '-t', '-q', '--dbname', pg.conn],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=pg.env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        def close_barrier():
            if barrier.poll() is None:
                barrier.kill()
            barrier.communicate(timeout=5)
        self.addCleanup(close_barrier)
        barrier.stdin.write("BEGIN; SELECT pg_advisory_xact_lock(1414810699, hashtext(current_schema())); SELECT 'LOCKED';\n")
        barrier.stdin.flush()
        # psql writes only a single empty row and the marker for the two SELECTs.
        self.assertEqual('', barrier.stdout.readline().strip())
        self.assertEqual('LOCKED', barrier.stdout.readline().strip())
        processes = []
        for index in range(8):
            name = 'coverage' if index % 2 == 0 else 'stpcompare'
            config = root / f'client-{index}.json'
            config.write_text(json.dumps({'scheduler': {'endpoint': endpoint},
                'client': {'worker_id': f'first-start-{index}', 'heartbeat_interval_ms': 100,
                           'register_timeout_ms': 2000, 'register_max_attempts': 5},
                'plugins': {name: plugins[name]}}), encoding='utf-8')
            client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', config, root, f'client-{index}', pg.env)
            self.addCleanup(client.close)
            processes.append(client)
        scheduler = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', scheduler_config,
                                    root, 'scheduler', pg.env)
        self.addCleanup(scheduler.close)
        processes.append(scheduler)
        barrier.communicate('COMMIT;\n\\q\n', timeout=5)
        deadline = time.monotonic() + 12
        status = {}
        while time.monotonic() < deadline:
            for process in processes:
                self.assertIsNone(process.process.poll(), process.text())
            try:
                with urlopen(management + '/status', timeout=2) as response:
                    status = json.load(response)
                if len(status['clients']) == 8:
                    break
            except (URLError, OSError):
                pass
            time.sleep(.05)
        self.assertEqual(8, len(status.get('clients', [])), '\n'.join(p.text() for p in processes))
        self.assertEqual({'coverage', 'stpcompare'}, set(status['plugins']))
        relations = pg.sql("SELECT tablename FROM pg_tables WHERE schemaname=current_schema()").splitlines()
        self.assertTrue({'runs', 'tasks', 'plugin_runs', 'plugin_run_files', 'plugin_file_outcomes',
                         'plugin_task_completions', 'coverage_file_points', 'coverage_cov_files',
                         'coverage_checkpoints', 'stp_compare_checkpoints', 'stp_compare_results'}.issubset(relations), relations)
        # A long-running analysis transaction may still be reading results when
        # another client starts. Existing-column migrations must not request an
        # exclusive table lock during ordinary initialization.
        reader = subprocess.Popen([str(pg.program('psql')), '-X', '-A', '-t', '-q', '--dbname', pg.conn],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=pg.env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        def close_reader():
            if reader.poll() is None:
                reader.kill()
            reader.communicate(timeout=5)
        self.addCleanup(close_reader)
        reader.stdin.write("BEGIN; LOCK TABLE plugin_runs, tasks IN ACCESS SHARE MODE; SELECT 'READING';\n")
        reader.stdin.flush()
        self.assertEqual('READING', reader.stdout.readline().strip())
        config = root / 'client-after-analysis.json'
        config.write_text(json.dumps({'scheduler': {'endpoint': endpoint},
            'client': {'worker_id': 'analysis-time-start', 'heartbeat_interval_ms': 100},
            'plugins': {'coverage': plugins['coverage']}}), encoding='utf-8')
        worker = CapturedProcess(OPTIONS.bin_dir / 'client.exe', config, root, 'client-after-analysis', pg.env)
        self.addCleanup(worker.close)
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            self.assertIsNone(worker.process.poll(), worker.text())
            with urlopen(management + '/status', timeout=2) as response:
                status = json.load(response)
            if any(client['worker_id'] == 'analysis-time-start' for client in status['clients']):
                break
            time.sleep(.05)
        self.assertTrue(any(client['worker_id'] == 'analysis-time-start' for client in status['clients']), worker.text())
        reader.communicate('COMMIT;\n\\q\n', timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--pg-bin', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.bin_dir = OPTIONS.bin_dir.resolve()
    OPTIONS.pg_bin = OPTIONS.pg_bin.resolve()
    unittest.main(argv=[__file__, *remaining])
