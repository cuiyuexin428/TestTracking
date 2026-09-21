"""Real Agent, PowerShell staging and deployment API activation contract."""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from urllib.error import URLError
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
    def test_real_download_activation_failed_update_rollback_and_restart(self):
        root = Path(tempfile.mkdtemp(prefix='deployment-e2e-', dir=ROOT / 'build/test'))
        client_root = root / '客户端 运行目录'
        client_root.mkdir()
        service = ResultService(ServiceConfig(workspace=str(root), providers={},
                                              artifact_store_dir=str(root / 'repository')))
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
        management = f'http://127.0.0.1:{unused_port()}'
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

        def upload(contents, version):
            path = f'/api/artifacts/upload?kind=plugin&name=demo&version={version}&filename=demo.dll'
            req = Request(endpoint + path, data=contents, method='POST',
                          headers={'Content-Type': 'application/octet-stream'})
            with urlopen(req, timeout=10) as response:
                return json.load(response)

        desired_path = '/api/deployments/e2e-worker/desired'
        state_path = '/api/deployments/e2e-worker'
        wait(lambda: request(management, '/status')['clients'])
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
