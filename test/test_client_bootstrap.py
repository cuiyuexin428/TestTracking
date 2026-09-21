"""Native managed Agent discovery remains live without plugins or PowerShell progress."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = None


@unittest.skipUnless(os.name == 'nt', 'Windows native Agent')
class BootstrapAgentTests(unittest.TestCase):
    def test_empty_agent_heartbeat_identity_failed_activation_and_empty_release(self):
        root = Path(tempfile.mkdtemp(prefix='agent-bootstrap-', dir=ROOT / 'build/test'))
        state = {'heartbeats': [], 'reports': [], 'desired': None, 'get_paths': []}
        blocked = threading.Event()
        release_get = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def reply(self, value):
                raw = json.dumps(value).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def body(self):
                return json.loads(self.rfile.read(int(self.headers['Content-Length'])))

            def do_POST(self):
                state['heartbeats'].append((time.monotonic(), self.path, self.body()))
                self.reply({'ok': True})

            def do_GET(self):
                state['get_paths'].append(self.path)
                blocked.set()
                release_get.wait(15)
                self.reply({'desired': state['desired']})

            def do_PUT(self):
                state['reports'].append(self.body())
                self.reply({'ok': True})

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.addCleanup(release_get.set)
        template = {'scheduler': {'endpoint': 'tcp://127.0.0.1:1', 'poll_timeout_ms': 5},
                    'client': {'worker_id': 'auto', 'deployment_endpoint': f'http://127.0.0.1:{server.server_port}',
                               'deployment_poll_seconds': 1, 'register_timeout_ms': 100,
                               'register_max_attempts': 1, 'update_windows': []}, 'plugins': {}}
        processes = []

        def stop(process):
            if process.poll() is None:
                process.stdin.write(b'quit\n')
                process.stdin.flush()
                process.wait(timeout=6)

        def cleanup():
            release_get.set()
            for process, output in processes:
                try:
                    stop(process)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=3)
                    process.stdin.close()
                    output.close()

        self.addCleanup(cleanup)

        def start(directory, tag, config=None):
            directory.mkdir(parents=True, exist_ok=True)
            path = directory / 'client.json'
            if config is not None:
                path.write_text(json.dumps(config), encoding='utf-8')
            output = (directory / (tag + '.log')).open('wb')
            process = subprocess.Popen([str(OPTIONS.client), str(path)], cwd=directory,
                                       stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            processes.append((process, output))
            return process, path

        def wait(predicate, timeout=15):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                value = predicate()
                if value:
                    return value
                time.sleep(.05)
            logs = '\n'.join(path.read_text(encoding='utf-8', errors='replace') for path in root.rglob('*.log'))
            self.fail(f'condition not reached; artifacts: {root}\n{logs}')

        client_root = root / '空客户端 A'
        first_process, path = start(client_root, 'first', template)
        wait(blocked.is_set)
        wait(lambda: len(state['heartbeats']) >= 3, timeout=10)
        self.assertIsNone(first_process.poll())
        first_id = json.loads(path.read_text(encoding='utf-8'))['client']['worker_id']
        self.assertRegex(first_id, r'^client-[0-9a-f]{32}$')
        samples = list(state['heartbeats'])
        self.assertGreaterEqual(samples[-1][0] - samples[0][0], 5.0)
        self.assertLess(max(b[0] - a[0] for a, b in zip(samples, samples[1:])), 5.0)
        for _, route, heartbeat in samples:
            self.assertEqual(f'/api/agents/{first_id}/heartbeat', route)
            self.assertEqual('unconfigured', heartbeat['state'])
            self.assertEqual([], heartbeat['plugin_keys'])
            self.assertEqual('2', heartbeat['agent_version'])
            self.assertTrue(heartbeat['hostname'])
            self.assertTrue(heartbeat['update_window_open'])
        first_instance = samples[0][2]['instance_id']
        release_get.set()
        stop(first_process)

        restarted, _ = start(client_root, 'restarted')
        wait(lambda: any(body['instance_id'] != first_instance and route == f'/api/agents/{first_id}/heartbeat'
                         for _, route, body in state['heartbeats']))
        self.assertEqual(first_id, json.loads(path.read_text(encoding='utf-8'))['client']['worker_id'])
        invalid_dll = root / 'invalid.dll'
        invalid_dll.write_bytes(b'invalid plugin fixture')
        state['desired'] = {'revision': 'a' * 64, 'reservation_id': 'bootstrap-bad', 'components': [],
                            'config_patch': {'plugins': {'broken': {'enabled': True, 'dll': str(invalid_dll)}}}}
        failure = wait(lambda: next((item for item in state['reports']
                                     if item['desired_revision'] == 'a' * 64 and item['state'] == 'failed'), None))
        self.assertEqual('bootstrap-bad', failure['reservation_id'])
        self.assertIsNone(restarted.poll())
        self.assertFalse((client_root / 'deployments/active.json').exists())
        failed_at = time.monotonic()
        wait(lambda: any(when >= failed_at and route == f'/api/agents/{first_id}/heartbeat'
                         and body['state'] == 'unconfigured' for when, route, body in state['heartbeats']))

        state['desired'] = {'revision': 'b' * 64, 'reservation_id': 'bootstrap-empty', 'components': [],
                            'config_patch': {'plugins': {'broken': {'enabled': False}}}}
        ready = wait(lambda: next((item for item in state['reports']
                                   if item['desired_revision'] == 'b' * 64 and item['state'] == 'ready'), None))
        self.assertEqual('bootstrap-empty', ready['reservation_id'])
        active = json.loads((client_root / 'deployments/active.json').read_text(encoding='utf-8'))
        self.assertEqual('b' * 64, active['revision'])
        self.assertIsNone(restarted.poll())
        stop(restarted)

        heartbeat_count = len(state['heartbeats'])
        empty_restart, _ = start(client_root, 'empty-release-restarted')
        wait(lambda: any(route == f'/api/agents/{first_id}/heartbeat'
                         and body['active_revision'] == 'b' * 64 and body['state'] == 'unconfigured'
                         for _, route, body in state['heartbeats'][heartbeat_count:]))
        self.assertIsNone(empty_restart.poll())
        self.assertEqual(first_id, json.loads(path.read_text(encoding='utf-8'))['client']['worker_id'])
        stop(empty_restart)

        state['desired'] = None
        other, other_path = start(root / '复制客户端 B', 'copy', template)
        wait(lambda: json.loads(other_path.read_text(encoding='utf-8'))['client']['worker_id'] != 'auto')
        other_id = json.loads(other_path.read_text(encoding='utf-8'))['client']['worker_id']
        self.assertNotEqual(first_id, other_id)
        wait(lambda: any(route == f'/api/agents/{other_id}/heartbeat' for _, route, _ in state['heartbeats']))
        stop(other)
        print(f'  bootstrap Agent artifacts: {root}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.client = OPTIONS.client.resolve()
    unittest.main(argv=[__file__, *remaining])
