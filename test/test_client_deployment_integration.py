"""Real Agent, PowerShell staging and deployment API activation contract."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from result_service.app.config import ServiceConfig
from result_service.app.server import Handler, ResultHTTPServer, ResultService
from test_runtime_integration import CapturedProcess, unused_port

OPTIONS = None


@unittest.skipUnless(os.name == 'nt', 'Windows Agent deployment')
class ClientDeploymentIntegrationTests(unittest.TestCase):
    def test_multiplugin_update_rolls_back_every_worker_and_retains_release_after_restart(self):
        root = Path(tempfile.mkdtemp(prefix='deployment-multi-e2e-', dir=ROOT / 'build/test'))
        client_root = root / '双插件 client'
        client_root.mkdir()
        management = f'http://127.0.0.1:{unused_port()}'
        service = ResultService(ServiceConfig(workspace=str(root), providers={},
                                              artifact_store_dir=str(root / 'repository'),
                                              scheduler_management_url=management))

        class QuietHandler(Handler):
            def log_message(self, *args):
                pass

        QuietHandler.service = service
        server = ResultHTTPServer(('127.0.0.1', 0), QuietHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def close_server():
            server.shutdown()
            thread.join(5)
            server.server_close()
            service.close()

        self.addCleanup(close_server)
        endpoint = f'http://127.0.0.1:{server.server_port}'
        scheduler_endpoint = f'tcp://127.0.0.1:{unused_port()}'
        entered = root / 'probe-result.txt'
        scheduler_config = root / 'scheduler.json'
        scheduler_config.write_text(json.dumps({
            'server': {'router_endpoint': scheduler_endpoint, 'management_endpoint': management,
                       'poll_timeout_ms': 5, 'client_timeout_ms': 1000},
            'plugins': {
                'demo': {'dll': str(OPTIONS.bin_dir / 'demo.dll'), 'enabled': True},
                'probe': {'dll': str(OPTIONS.bin_dir / 'scheduler_probe.dll'), 'enabled': True,
                          'plugin_type': 'client-retry-probe', 'entered_file': str(entered)},
            }}), encoding='utf-8')
        scheduler = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', scheduler_config,
                                    root, 'scheduler', os.environ.copy())
        self.addCleanup(scheduler.close)
        client_config = client_root / 'client.json'
        client_config.write_text(json.dumps({
            'scheduler': {'endpoint': scheduler_endpoint, 'poll_timeout_ms': 5},
            'client': {'worker_id': 'multi-worker', 'heartbeat_interval_ms': 100,
                       'task_threads': 2, 'deployment_endpoint': endpoint,
                       'deployment_poll_seconds': 1, 'register_timeout_ms': 200,
                       'register_max_attempts': 5, 'execution_windows': [], 'update_windows': []},
            'plugins': {
                'demo': {'dll': str(OPTIONS.bin_dir / 'demo.dll'), 'enabled': True, 'task_sleep_ms': 400},
                'probe': {'dll': str(OPTIONS.bin_dir / 'client_retry_probe.dll'), 'enabled': True,
                          'process_probe': True, 'task_sleep_ms': 1200},
            }}), encoding='utf-8')
        client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', client_config,
                                 client_root, 'client', os.environ.copy())
        self.addCleanup(client.close)

        def request(base, path, body=None, method=None):
            req = Request(base + path, data=None if body is None else json.dumps(body).encode(),
                          method=method, headers={'Content-Type': 'application/json'})
            with urlopen(req, timeout=3) as response:
                return json.load(response)

        def wait(predicate, timeout=25):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    value = predicate()
                    if value:
                        return value
                except (OSError, URLError, json.JSONDecodeError):
                    pass
                time.sleep(.1)
            self.fail(f'condition not reached; artifacts: {root}\n{client.text()}\n{scheduler.text()}')

        def active():
            return json.loads((client_root / 'deployments/active.json').read_text(encoding='utf-8'))

        def workers():
            return {item['plugin_key']: item for item in request(management, '/status')['clients']
                    if item['physical_client_id'] == 'multi-worker' and item['registered']}

        def both_workers():
            current = workers()
            return current if set(current) == {'demo', 'probe'} else None

        def configurable():
            return any(item['client_id'] == 'multi-worker' and item['configurable']
                       for item in request(endpoint, '/api/agents')['agents'])

        def worker_pids():
            return {key: int(pid) for key, pid in re.findall(
                r'client worker started: plugin=(\S+) pid=(\d+)', client.text())}

        def upload(name, filename, contents, version):
            req = Request(endpoint + f'/api/artifacts/upload?kind=plugin&name={name}&version={version}&filename={filename}',
                          data=contents, method='POST', headers={'Content-Type': 'application/octet-stream'})
            with urlopen(req, timeout=10) as response:
                return json.load(response)

        good_demo = upload('demo', 'demo.dll', (OPTIONS.bin_dir / 'demo.dll').read_bytes(), '1')
        good_probe = upload('probe', 'client_retry_probe.dll', (OPTIONS.bin_dir / 'client_retry_probe.dll').read_bytes(), '1')
        desired_path = '/api/deployments/multi-worker/desired'
        state_path = '/api/deployments/multi-worker'

        def publish(probe_artifact, revision, generation):
            return request(endpoint, desired_path, {
                'revision': revision,
                'components': [{'slot': 'plugin.demo', 'artifact_id': good_demo['artifact_id']},
                               {'slot': 'plugin.probe', 'artifact_id': probe_artifact['artifact_id']}],
                'config_patch': {'plugins': {
                    'demo': {'dll': '{artifact:plugin.demo}/demo.dll', 'generation': generation},
                    'probe': {'dll': '{artifact:plugin.probe}/client_retry_probe.dll', 'generation': generation},
                }}, 'reason': 'multi-plugin activation contract'}, 'PUT')['desired']['revision']

        def run_both(label):
            if entered.exists():
                entered.unlink()
            finished_before = scheduler.text().count('demo plugin: run finished')
            run_ids = []
            for plugin_type in ('demo', 'client-retry-probe'):
                result = request(management, '/commands', {'action': 'run', 'plugin_type': plugin_type,
                                 'dataset_version': label, 'worker_ids': ['multi-worker']})
                self.assertTrue(result['ok'], result)
                run_ids.append(result['run_id'])
            def both_have_tasks():
                current = both_workers()
                return current and all(item['tasks'] for item in current.values())

            wait(both_have_tasks)
            if label == 'healthy':
                with self.assertRaises(HTTPError) as rejection:
                    publish(good_probe, first, 'must-not-update-busy-client')
                self.assertEqual(409, rejection.exception.code)
                rejection.exception.close()
                self.assertEqual(first, request(endpoint, state_path)['desired']['revision'])
            wait(lambda: entered.is_file() and scheduler.text().count('demo plugin: run finished') > finished_before)
            wait(lambda: not any(run['run_id'] in run_ids for run in request(management, '/status')['active_runs']))

        initial = wait(both_workers)
        self.assertEqual({'multi-worker/demo', 'multi-worker/probe'}, {item['worker_id'] for item in initial.values()})
        self.assertEqual({'demo', 'client-retry-probe'}, {item['plugin_type'] for item in initial.values()})
        wait(configurable)
        first = publish(good_probe, None, 'healthy-v1')
        wait(lambda: request(endpoint, state_path)['state'] == 'current')
        self.assertEqual(first, active()['revision'])
        old_release = Path(active()['config_path']).read_bytes()
        effective = json.loads(old_release)
        for key in ('demo', 'probe'):
            self.assertEqual('healthy-v1', effective['plugins'][key]['generation'])
            self.assertTrue(Path(effective['plugins'][key]['dll']).is_file())
        run_both('healthy')
        previous_pids = worker_pids()

        bad_probe = upload('probe', 'client_retry_probe.dll', b'invalid second plugin DLL', '2')
        wait(configurable)
        rejected = publish(bad_probe, first, 'rejected-v2')
        wait(lambda: request(endpoint, state_path)['state'] == 'failed')
        wait(lambda: 'restoring previous configuration' in client.text())
        wait(lambda: all(worker_pids().get(key) != pid for key, pid in previous_pids.items()))
        wait(both_workers)
        self.assertEqual(first, active()['revision'])
        self.assertEqual(old_release, Path(active()['config_path']).read_bytes())
        failed = json.loads((client_root / 'deployments/failed.json').read_text(encoding='utf-8'))
        self.assertEqual(rejected, failed['revision'])
        run_both('restored')

        client.crash()
        client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', client_config,
                                 client_root, 'restarted', os.environ.copy())
        self.addCleanup(client.close)
        wait(lambda: all(f'registered: plugin_type={plugin}' in client.text()
                         for plugin in ('demo', 'client-retry-probe')))
        wait(both_workers)
        wait(lambda: request(endpoint, state_path)['state'] == 'failed')
        self.assertEqual(first, active()['revision'])
        self.assertEqual(old_release, Path(active()['config_path']).read_bytes())
        self.assertNotIn('deployment activation failed', client.text())
        run_both('restarted')
        print(f'  multi-plugin deployment e2e artifacts: {root}', flush=True)

    def test_real_download_activation_failed_update_rollback_and_restart(self):
        root = Path(tempfile.mkdtemp(prefix='deployment-e2e-', dir=ROOT / 'build/test'))
        client_root = root / '客户端 运行目录'
        client_root.mkdir()
        management = f'http://127.0.0.1:{unused_port()}'
        service = ResultService(ServiceConfig(workspace=str(root), providers={},
                                              artifact_store_dir=str(root / 'repository'),
                                              scheduler_management_url=management))
        class QuietHandler(Handler):
            def log_message(self, *args):
                pass
        QuietHandler.service = service
        server = ResultHTTPServer(('127.0.0.1', 0), QuietHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        def close_server():
            server.shutdown()
            thread.join(5)
            server.server_close()
            service.close()
        self.addCleanup(close_server)
        endpoint = f'http://127.0.0.1:{server.server_port}'
        scheduler_endpoint = f'tcp://127.0.0.1:{unused_port()}'
        plugin = {'dll': str(OPTIONS.bin_dir / 'demo.dll'), 'enabled': True}
        scheduler_config = root / 'scheduler.json'
        scheduler_config.write_text(json.dumps({
            'server': {'router_endpoint': scheduler_endpoint,
                       'management_endpoint': management, 'poll_timeout_ms': 5,
                       'client_timeout_ms': 1000}, 'plugins': {'demo': plugin}}), encoding='utf-8')
        scheduler = CapturedProcess(OPTIONS.bin_dir / 'scheduler.exe', scheduler_config,
                                    root, 'scheduler', os.environ.copy())
        self.addCleanup(scheduler.close)
        client_config = client_root / 'client.json'
        client_config.write_text(json.dumps({
            'scheduler': {'endpoint': scheduler_endpoint, 'poll_timeout_ms': 5},
            'client': {'worker_id': 'e2e-worker', 'heartbeat_interval_ms': 100,
                       'deployment_endpoint': endpoint, 'deployment_poll_seconds': 1,
                       'register_timeout_ms': 200, 'register_max_attempts': 5,
                       'execution_windows': [], 'update_windows': []},
            'plugins': {'demo': plugin}}), encoding='utf-8')
        client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', client_config,
                                 client_root, 'client', os.environ.copy())
        self.addCleanup(client.close)

        def request(base, path, body=None, method=None):
            req = Request(base + path, data=None if body is None else json.dumps(body).encode(),
                          method=method, headers={'Content-Type': 'application/json'})
            with urlopen(req, timeout=3) as response:
                return json.load(response)

        def wait(predicate, timeout=20):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    value = predicate()
                    if value:
                        return value
                except (OSError, URLError, json.JSONDecodeError):
                    pass
                time.sleep(.1)
            self.fail(f'condition not reached; artifacts: {root}\n{client.text()}\n{scheduler.text()}')

        def active():
            return json.loads((client_root / 'deployments/active.json').read_text(encoding='utf-8'))

        def configurable():
            return any(item['client_id'] == 'e2e-worker' and item['configurable']
                       for item in request(endpoint, '/api/agents')['agents'])

        def upload(contents, version):
            path = f'/api/artifacts/upload?kind=plugin&name=demo&version={version}&filename=demo.dll'
            req = Request(endpoint + path, data=contents, method='POST',
                          headers={'Content-Type': 'application/octet-stream'})
            with urlopen(req, timeout=10) as response:
                return json.load(response)

        desired_path = '/api/deployments/e2e-worker/desired'
        state_path = '/api/deployments/e2e-worker'
        wait(lambda: request(management, '/status')['clients'])
        wait(configurable)
        good = upload((OPTIONS.bin_dir / 'demo.dll').read_bytes(), '1')
        def publish(artifact, revision):
            return request(endpoint, desired_path, {
                'revision': revision, 'components': [{'slot': 'plugin.demo', 'artifact_id': artifact['artifact_id']}],
                'config_patch': {'plugins': {'demo': {'dll': '{artifact:plugin.demo}/demo.dll', 'task_sleep_ms': 10}}},
                'reason': 'isolated e2e'}, 'PUT')['desired']['revision']

        first = publish(good, None)
        wait(lambda: request(endpoint, state_path)['state'] == 'current')
        self.assertEqual(first, active()['revision'])
        effective = json.loads(Path(active()['config_path']).read_text(encoding='utf-8'))
        deployed_dll = Path(effective['plugins']['demo']['dll'])
        self.assertEqual((OPTIONS.bin_dir / 'demo.dll').read_bytes(), deployed_dll.read_bytes())
        run = request(management, '/commands', {'action': 'run', 'plugin_type': 'demo',
                                               'dataset_version': 'd1'})
        self.assertTrue(run['ok'], run)
        wait(lambda: 'demo plugin: run finished' in scheduler.text())

        bad = upload(b'invalid DLL fixture', '2')
        wait(configurable)
        second = publish(bad, first)
        wait(lambda: request(endpoint, state_path)['state'] == 'failed')
        self.assertEqual(first, active()['revision'], 'failed registration must retain the old active release')
        wait(lambda: 'restoring previous configuration' in client.text())

        # Reproduce the durable boundary between failed.json and the failed
        # HTTP report: the rejected release must stay rejected after restart,
        # while its terminal failure must eventually replace "applying".
        client.close()
        report_path = client_root / 'deployments/agent_report.json'
        interrupted_report = json.loads(report_path.read_text(encoding='utf-8'))
        self.assertEqual(second, interrupted_report['desired_revision'])
        interrupted_report['state'] = 'applying'
        interrupted_report['message'] = None
        for component in interrupted_report['components']:
            component['state'] = 'staged'
        request(endpoint, state_path + '/reported', interrupted_report, 'PUT')
        report_path.write_text(json.dumps(interrupted_report), encoding='utf-8')
        client = CapturedProcess(OPTIONS.bin_dir / 'client.exe', client_config,
                                 client_root, 'failure-restart', os.environ.copy())
        self.addCleanup(client.close)
        wait(lambda: request(endpoint, state_path)['state'] == 'failed')
        self.assertEqual(first, active()['revision'])
        self.assertNotIn('deployment activation failed', client.text(),
                         'a rejected release must not execute again merely to recover its report')

        wait(configurable)
        rolled_back = request(endpoint, desired_path, {'revision': second, 'action': 'rollback',
                              'target_revision': first, 'reason': 'return to healthy release'}, 'PUT')
        final_revision = rolled_back['desired']['revision']
        wait(lambda: request(endpoint, state_path)['state'] == 'current')
        self.assertEqual(final_revision, active()['revision'])
        client.close()
        restarted = CapturedProcess(OPTIONS.bin_dir / 'client.exe', client_config,
                                    client_root, 'restarted', os.environ.copy())
        self.addCleanup(restarted.close)
        wait(lambda: 'registration accepted' in restarted.text() or 'registered' in restarted.text())
        self.assertEqual(final_revision, active()['revision'])
        print(f'  deployment e2e artifacts: {root}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.bin_dir = OPTIONS.bin_dir.resolve()
    unittest.main(argv=[__file__, *remaining])
