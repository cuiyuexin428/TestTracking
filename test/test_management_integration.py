"""Exercise the browser API against actual scheduler/client and private PostgreSQL."""
import argparse
import hashlib
import io
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
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_coverage_settle import PrivatePostgres
from test_runtime_integration import CapturedProcess, unused_port

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from result_service.app.config import ServiceConfig
from result_service.app.management import ManagementError, ManagementService
OPTIONS = None


class ManagementIntegrationTests(unittest.TestCase):
    def test_native_plugin_installation_barrier_covers_commit_and_rollback(self):
        from result_service.app.artifacts import ArtifactStore
        from result_service.app.resource_catalog import ResourceCatalog
        from result_service.app.server_plugins import ServerPluginInstaller
        from test_server_plugins import make_bundle
        import zipfile

        temporary = tempfile.TemporaryDirectory(prefix='installer-maintenance-', dir=ROOT / 'build/test')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        store = ArtifactStore(root / 'artifacts')
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w') as archive:
            archive.writestr('part.stp', b'new geometry')
            archive.writestr('VERSION.TXT', b' release-1 \n')
        raw = data.getvalue()
        source = store.upload(io.BytesIO(raw), len(raw), kind='dataset', name='parts',
                              version='2', filename='parts.zip')[0]
        dataset = ResourceCatalog(store).prepare_dataset(source['artifact_id'], '')
        self.assertEqual('sha256:' + dataset['manifest_sha256'], dataset['version'])
        old_payload = io.BytesIO()
        with zipfile.ZipFile(old_payload, 'w') as archive:
            archive.writestr('part.stp', b'old geometry')
            archive.writestr('VERSION.TXT', b' release-1 \n')
        old_raw = old_payload.getvalue()
        old_source = store.upload(io.BytesIO(old_raw), len(old_raw), kind='dataset', name='parts',
                                 version='1', filename='parts.zip')[0]
        old_dataset = ResourceCatalog(store).prepare_dataset(old_source['artifact_id'], '')
        self.assertEqual(old_dataset['source_version'], dataset['source_version'])
        self.assertNotEqual(old_dataset['version'], dataset['version'])
        old_data = root / 'old-client-data'
        with zipfile.ZipFile(store.object_path(store.get(old_dataset['artifact_id'])['sha256'])) as archive:
            archive.extractall(old_data)
        url = f'http://127.0.0.1:{unused_port()}'
        config = root / 'scheduler.json'
        settings = {'server': {'router_endpoint': f'tcp://127.0.0.1:{unused_port()}',
                              'management_endpoint': url, 'poll_timeout_ms': 5},
                    'plugins': {'coverage': {'enabled': True, 'dll': str(OPTIONS.bin_dir / 'coverage.dll'),
                        'stp_manifest': dataset['manifest_path'], 'expected_dataset_version': dataset['version'],
                        'expected_program_version': 'program-1', 'store_type': 'file', 'store_dir': str(root / 'store')}}}
        config.write_text(json.dumps(settings), encoding='utf-8')
        service_path = root / 'result_service.json'
        service_path.write_text(json.dumps({'workspace': str(root), 'scheduler_config_path': str(config),
            'scheduler_management_url': url, 'artifact_store_dir': str(store.root), 'providers': {}}), encoding='utf-8')
        service_config = ServiceConfig.from_file(service_path)
        management = ManagementService(service_config)

        def spawn(tag):
            process = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', config, root, tag, os.environ.copy())
            self.addCleanup(process.close)
            end = time.monotonic() + 12
            while time.monotonic() < end:
                if management.get_status()['online']:
                    return process
                time.sleep(.025)
            self.fail(process.text())

        def upload(version):
            raw = make_bundle('coverage', version,
                extras={'bin/custom.dll': (OPTIONS.bin_dir / 'coverage.dll').read_bytes()})
            return store.upload(io.BytesIO(raw), len(raw), kind='plugin', name='coverage-server',
                version=version, filename='coverage-server.zip')[0]

        scheduler = spawn('scheduler-original')
        installer = ServerPluginInstaller(service_config, store, management)
        plan = installer.preview({'artifact_id': upload('2')['artifact_id']})
        original_write = management._atomic_write
        rejected_run = []
        def try_run_at_commit(path, data):
            if path.name == 'installation.json':
                try:
                    management._request('/commands', {'action': 'run', 'plugin_type': 'coverage',
                        'dataset_version': dataset['version']})
                except ManagementError as error:
                    rejected_run.append(error.status)
                    self.assertIn('maintenance', str(error))
                else:
                    self.fail('run passed the native installation barrier')
            return original_write(path, data)
        with patch.object(management, '_atomic_write', side_effect=try_run_at_commit):
            installed = installer.install(plan)
        self.assertTrue(installed['installed'])
        self.assertEqual([409], rejected_run)
        self.assertFalse(management.get_status()['active_runs'])
        self.assertEqual(1, len(management.get_status()['plugin_maintenance']))
        with self.assertRaises(ManagementError):
            management._request('/commands', {'action': 'run', 'plugin_type': 'coverage',
                'dataset_version': dataset['version']})
        scheduler.close()
        spawn('scheduler-installed')
        self.assertEqual([], management.get_status()['plugin_maintenance'])
        self.assertEqual(json.loads(config.read_bytes()), management.get_status()['configuration'])

        installer = ServerPluginInstaller(service_config, store, management)
        plan = installer.preview({'artifact_id': upload('3')['artifact_id']})
        before = config.read_bytes(), service_path.read_bytes()
        failures = []
        def fail_result_write(path, data):
            if path == service_path and not failures:
                failures.append(True)
                raise OSError('injected second configuration write failure')
            return original_write(path, data)
        with patch.object(management, '_atomic_write', side_effect=fail_result_write):
            with self.assertRaises(ManagementError):
                installer.install(plan)
        self.assertEqual(before, (config.read_bytes(), service_path.read_bytes()))
        self.assertEqual([], management.get_status()['plugin_maintenance'])
        self.assertFalse(installer.maintenance_journal.exists())
        # A source label with whitespace now produces a canonical content
        # identity that the ordinary web run command can submit unchanged.
        started = management.command({'action': 'run', 'plugin_type': 'coverage',
                                      'dataset_version': dataset['version']})
        self.assertTrue(started['ok'])
        self.assertEqual('running', started['status'])
        # An older managed package with the same user-supplied version.txt
        # cannot register or claim work for the changed dataset.
        client_config = root / 'old-client.json'
        client_config.write_text(json.dumps({
            'scheduler': {'endpoint': settings['server']['router_endpoint'], 'poll_timeout_ms': 5},
            'client': {'worker_id': 'old-dataset-client', 'heartbeat_interval_ms': 100,
                       'task_threads': 1, 'register_timeout_ms': 500, 'register_max_attempts': 2},
            'plugins': {'coverage': {'enabled': True, 'dll': str(OPTIONS.bin_dir / 'coverage.dll'),
                'store_type': 'file', 'store_dir': str(root / 'old-client-store'), 'dry_run': True,
                'store_cov': False, 'program_version': 'program-1', 'stp_root': str(old_data)}}
        }), encoding='utf-8')
        old_client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', client_config, root,
                                     'old-dataset-client', os.environ.copy())
        self.addCleanup(old_client.close)
        self.assertNotEqual(0, old_client.process.wait(timeout=10), old_client.text())
        self.assertIn('registration rejected', old_client.text())
        self.assertIn('version mismatch', old_client.text())
        self.assertNotIn('client: task batch run=', old_client.text())
        status = management.get_status()
        self.assertEqual([], status['clients'])
        self.assertEqual(0, status['in_flight'])

    def test_online_deployment_checks_durable_runs_without_mutating_recovery_state(self):
        pg = PrivatePostgres(OPTIONS.pg_bin, OPTIONS.bin_dir / 'scheduler.exe')
        self.addCleanup(pg.close)
        pg.start()
        temporary = tempfile.TemporaryDirectory(prefix='online-deployment-storage-', dir=ROOT / 'build/test')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        management = f'http://127.0.0.1:{unused_port()}'
        config = root / 'scheduler.json'
        config.write_text(json.dumps({
            'server': {'router_endpoint': f'tcp://127.0.0.1:{unused_port()}',
                       'management_endpoint': management, 'db_conn': pg.conn},
            'plugins': {'probe': {'enabled': True, 'dll': str(OPTIONS.bin_dir / 'scheduler_probe.dll'),
                                  'entered_file': str(root / 'entered'), 'expected_program_version': 'v1'}}
        }), encoding='utf-8')
        original = config.read_bytes()
        scheduler = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', config,
                                    root, 'scheduler-process', pg.env)
        self.addCleanup(scheduler.close)

        def request(body=None):
            req = Request(management + ('/commands' if body else '/status'),
                          data=json.dumps(body).encode() if body else None,
                          headers={'Content-Type': 'application/json'})
            try:
                response = urlopen(req, timeout=15)
            except HTTPError as error:
                response = error
            with response:
                return response.status, json.load(response)

        deadline = time.monotonic() + 15
        while True:
            try:
                if request()[0] == 200:
                    break
            except (OSError, URLError):
                pass
            if time.monotonic() > deadline:
                self.fail('private scheduler did not start: ' + str(root))
            time.sleep(.05)
        command = {'action': 'apply_plugin_deployment', 'config_path': str(config),
                   'config_revision': hashlib.sha256(original).hexdigest(),
                   'plugins': {'scheduler-probe': {'expected_program_version': 'v2'}}}
        # These rows deliberately arrive after startup and are absent from the
        # scheduler's in-memory maps. Only a read-only durable guard sees them.
        pg.sql("INSERT INTO runs(run_id,plugin_type,status) VALUES('unrelated','other-plugin','planning')")
        for status in ('queued', 'planning', 'running', 'stopped'):
            with self.subTest(status=status):
                pg.sql("INSERT INTO runs(run_id,plugin_type,status) VALUES('recoverable','scheduler-probe','" + status + "')")
                code, rejected = request(command)
                self.assertEqual(409, code, rejected)
                self.assertIn('durable unfinished', rejected['error'])
                maintenance_code, maintenance_rejected = request({
                    'action': 'begin_plugin_maintenance', 'plugin_type': 'scheduler-probe',
                    'reservation_id': 'durable-guard', 'config_path': str(config),
                    'config_revision': hashlib.sha256(original).hexdigest()})
                self.assertEqual(409, maintenance_code, maintenance_rejected)
                self.assertIn('durable unfinished', maintenance_rejected['error'])
                self.assertEqual(status, pg.sql("SELECT status FROM runs WHERE run_id='recoverable'"))
                self.assertEqual('planning', pg.sql("SELECT status FROM runs WHERE run_id='unrelated'"))
                self.assertEqual(original, config.read_bytes())
                pg.sql("DELETE FROM runs WHERE run_id='recoverable'")
        pg.sql('ALTER TABLE runs RENAME COLUMN status TO unavailable_status')
        code, rejected = request(command)
        self.assertEqual(503, code, rejected)
        maintenance_code, maintenance_rejected = request({
            'action': 'begin_plugin_maintenance', 'plugin_type': 'scheduler-probe',
            'reservation_id': 'durable-guard', 'config_path': str(config),
            'config_revision': hashlib.sha256(original).hexdigest()})
        self.assertEqual(503, maintenance_code, maintenance_rejected)
        self.assertEqual(original, config.read_bytes())
        pg.sql('ALTER TABLE runs RENAME COLUMN unavailable_status TO status')
        code, applied = request(command)
        self.assertEqual(200, code, applied)
        self.assertTrue(applied['online_applied'])
        self.assertEqual('planning', pg.sql("SELECT status FROM runs WHERE run_id='unrelated'"))
        self.assertEqual(hashlib.sha256(config.read_bytes()).hexdigest(), applied['configuration_revision'])
        service = ManagementService(ServiceConfig(scheduler_config_path=str(config),
                                                    scheduler_management_url=management))
        preview = service.get_config('scheduler')
        self.assertFalse(preview['restart_required'])
        manifest = root / 'next-manifest.txt'
        manifest.write_text('another.stp,12\n', encoding='utf-8')
        synchronized = service.apply_plugin_deployment(preview['revision'], {
            'scheduler-probe': {'expected_program_version': 'v3', 'stp_manifest': str(manifest)}})
        self.assertFalse(synchronized['restart_required'])
        self.assertEqual(hashlib.sha256(config.read_bytes()).hexdigest(), synchronized['revision'])
        self.assertEqual('v3', synchronized['effective_values']['plugins']['probe']['expected_program_version'])
        self.assertEqual(str(manifest), synchronized['effective_values']['plugins']['probe']['stp_manifest'])
        self.assertFalse(service.get_config('scheduler')['restart_required'])
        self.assertEqual('planning', pg.sql("SELECT status FROM runs WHERE run_id='unrelated'"))

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
