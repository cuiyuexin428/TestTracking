"""Exercise the browser API against actual scheduler/client and private PostgreSQL."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres
from test_runtime_integration import CapturedProcess, unused_port

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from result_service.app.config import ServiceConfig
from result_service.app.management import ManagementError, ManagementService
OPTIONS = None


class ManagementIntegrationTests(unittest.TestCase):
    def test_offline_deployment_guard_reads_durable_unfinished_runs(self):
        pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.bin_dir / 'scheduler.exe')
        self.addCleanup(pg.close)
        pg.start()
        temporary = tempfile.TemporaryDirectory(prefix='deployment-guard-', dir=ROOT / 'build/test')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        scheduler = root / 'scheduler.json'
        scheduler.write_text(json.dumps({'server': {'db_conn': pg.conn}, 'plugins': {}}))
        service = ManagementService(ServiceConfig(
            scheduler_config_path=str(scheduler), scheduler_management_url=''))
        # A first installation with no framework tables is allowed.
        service.require_plugin_idle('coverage')
        pg.sql("CREATE TABLE runs(run_id text PRIMARY KEY, plugin_type text, status text)")
        for status in ('queued', 'planning', 'running', 'stopped'):
            pg.sql("INSERT INTO runs VALUES('recoverable','coverage','" + status + "')")
            with self.assertRaises(ManagementError) as rejected:
                service.require_plugin_idle('coverage')
            self.assertEqual(409, rejected.exception.status)
            self.assertEqual(status, pg.sql("SELECT status FROM runs WHERE run_id='recoverable'"))
            service.require_plugin_idle('stpcompare')
            pg.sql("DELETE FROM runs")
        pg.sql("INSERT INTO runs VALUES('complete','coverage','finished')")
        service.require_plugin_idle('coverage')
        pg.sql("ALTER TABLE runs RENAME COLUMN status TO unavailable_status")
        with self.assertRaises(ManagementError) as rejected:
            service.require_plugin_idle('coverage')
        self.assertEqual(503, rejected.exception.status)

    def test_live_clients_commands_logs_config_and_independent_report(self):
        pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.bin_dir / 'scheduler.exe')
        self.addCleanup(pg.close)
        pg.start()
        root = Path(tempfile.mkdtemp(prefix='management-integration-', dir=ROOT / 'build/test'))
        logs = root / 'logs'
        logs.mkdir()
        dataset = root / 'dataset'
        dataset.mkdir()
        (dataset / 'version.txt').write_text('d1', encoding='utf-8')
        (dataset / 'manifest.txt').write_text('a.stp,1\nb.stp,1\nc.stp,1\n', encoding='utf-8')
        for name in ('a', 'b', 'c'):
            (dataset / f'{name}.stp').write_text('x')
        endpoint = f'tcp://127.0.0.1:{unused_port()}'
        management = f'http://127.0.0.1:{unused_port()}'
        port = unused_port()
        base = f'http://127.0.0.1:{port}'
        plugin = {'dll': str(OPTIONS.bin_dir / 'coverage.dll'), 'enabled': True,
                  'db_conn': pg.conn, 'store_type': 'postgres'}
        scheduler_config = root / 'scheduler.json'
        scheduler_config.write_text(json.dumps({
            'server': {'router_endpoint': endpoint, 'management_endpoint': management,
                       'db_conn': pg.conn, 'client_timeout_ms': 1200,
                       'log_file': str(logs / 'scheduler.log')},
            'plugins': {'coverage': dict(plugin, stp_manifest=str(dataset / 'manifest.txt'),
                                        task_size=1, expected_dataset_version='d1')}}))
        client_config = root / 'client.json'
        client_config.write_text(json.dumps({
            'scheduler': {'endpoint': endpoint},
            'client': {'worker_id': 'management-client', 'heartbeat_interval_ms': 100,
                       'task_threads': 1, 'request_backoff_max_ms': 100,
                       'request_backoff_initial_ms': 20},
            'plugins': {'coverage': dict(plugin, dry_run=True, dry_run_delay_ms=1000,
                                        stp_root=str(dataset), program_dir=str(root))}}))
        scheduler = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', scheduler_config,
                                    root, 'scheduler-process', pg.env)
        self.addCleanup(scheduler.close)
        client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', client_config,
                                 root, 'client-process', pg.env)
        self.addCleanup(client.close)
        service_config = root / 'result_service.json'
        service_config.write_text(json.dumps({
            'host': '127.0.0.1', 'port': port, 'db_conn': pg.conn,
            'scheduler_management_url': management, 'scheduler_config_path': str(scheduler_config),
            'client_config_path': str(client_config), 'logs_dir': str(logs),
            'workspace': str(root), 'analysis_scripts_dir': str(root / 'analysis'),
            'analysis_reports_dir': str(root / 'reports')}))
        output = (logs / 'result_service.log').open('wb')
        self.addCleanup(output.close)
        service = subprocess.Popen([sys.executable, '-B', str(ROOT / 'result_service/launcher.py'),
                                    '--config', str(service_config)], cwd=root, env=pg.env,
                                   stdout=output, stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.addCleanup(lambda: (service.terminate(), service.wait(10)) if service.poll() is None else None)

        def request(path, body=None, method=None):
            req = Request(base + path, data=json.dumps(body).encode() if body is not None else None,
                          method=method, headers={'Content-Type': 'application/json'})
            try:
                response = urlopen(req, timeout=40)
            except HTTPError as exc:
                response = exc
            with response:
                return response.status, json.load(response)

        def wait(predicate, timeout=25):
            deadline = time.monotonic() + timeout
            last = None
            while time.monotonic() < deadline:
                try:
                    last = predicate()
                    if last:
                        return last
                except (OSError, URLError):
                    pass
                time.sleep(.1)
            self.fail(f'condition not reached: {last}; artifacts: {root}')

        wait(lambda: request('/api/scheduler')[1].get('clients'))
        status = request('/api/scheduler')[1]
        self.assertTrue(status['online'])
        self.assertEqual('management-client', status['clients'][0]['worker_id'])
        code, run = request('/api/scheduler/commands', {
            'action': 'run', 'plugin_type': 'coverage', 'dataset_version': 'd1', 'params': ''})
        self.assertEqual(200, code, run)
        run_id = run['run_id']
        code, rejection = request('/api/scheduler/commands', {
            'action': 'stop', 'plugin_type': 'coverage', 'run_id': 'unrelated-run'})
        self.assertEqual(409, code, rejection)
        wait(lambda: request('/api/runs')[1] and request('/api/runs')[1][0]['status'] == 'finished')
        detail = request('/api/runs/' + run_id + '?plugin=coverage')[1]
        self.assertEqual(3, detail['result_completeness']['successful_files'])
        runs = request('/api/runs')[1]
        self.assertEqual(3, next(item for item in runs if item['run_id'] == run_id)['task_status_counts']['reported'])
        log = request('/api/logs?file=scheduler.log&limit=200&search=run')[1]
        self.assertTrue(any(run_id in line for line in log['lines']))
        code, saved = request('/api/config/client')
        self.assertEqual(200, code, saved)
        changed = saved['config']
        changed['client']['task_threads'] = 2
        code, saved_result = request('/api/config/client',
                                     {'config': changed, 'revision': saved['revision']}, 'PUT')
        self.assertEqual(200, code, saved_result)
        self.assertTrue(saved_result['restart_required'])
        self.assertEqual(2, json.loads(client_config.read_text())['client']['task_threads'])
        self.assertEqual(409, request('/api/config/client',
                                      {'config': changed, 'revision': saved['revision']}, 'PUT')[0])
        # Analysis remains functional after the execution service is fully stopped.
        scheduler.close()
        wait(lambda: request('/api/scheduler')[1].get('online') is False)
        self.assertEqual(200, request('/api/analysis/catalog?plugin=coverage')[0])
        code, report = request('/api/plugins/coverage/reports', {'run_id': run_id})
        self.assertEqual(200, code, report)
        self.assertTrue(any((root / 'reports').rglob('index.html')))
        print(f'  management artifacts: {root}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--pg-bin', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.bin_dir = OPTIONS.bin_dir.resolve()
    OPTIONS.pg_bin = OPTIONS.pg_bin.resolve()
    unittest.main(argv=[__file__, *remaining])
