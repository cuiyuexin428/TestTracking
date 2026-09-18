"""Exercise the real local management listener and scheduler/client lifecycle."""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def unused_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


class SchedulerManagementTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='scheduler management ')
        self.root = Path(self.temporary.name)
        self.processes = []
        self.router = f'tcp://127.0.0.1:{unused_port()}'
        self.port = unused_port()
        self.url = f'http://127.0.0.1:{self.port}'
        self.config = self.root / 'scheduler.json'
        self.settings = {'server': {'router_endpoint': self.router, 'management_endpoint': self.url,
                         'poll_timeout_ms': 5, 'client_timeout_ms': 600},
                         'plugins': {'probe': {'dll': str(PROBE), 'enabled': True,
                                     'entered_file': str(self.root / 'entered')}}}

    def tearDown(self):
        for process, log in reversed(self.processes):
            if process.poll() is None:
                try:
                    process.stdin.write(b'quit\n')
                    process.stdin.flush()
                    process.wait(timeout=8)
                except (OSError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait(timeout=5)
            process.stdin.close()
            log.close()
        self.temporary.cleanup()

    def spawn(self, executable, config, name):
        log = (self.root / (name + '.log')).open('wb')
        process = subprocess.Popen([str(executable), str(config)], cwd=self.root,
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.processes.append((process, log))
        return process

    def wait_for(self, condition, timeout=8):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                value = condition()
                if value:
                    return value
            except (URLError, ConnectionError, TimeoutError):
                pass
            time.sleep(.025)
        logs = '\n'.join(path.read_text(encoding='utf-8', errors='replace')
                         for path in self.root.glob('*.log'))
        self.fail('condition did not become true\n' + logs)

    def request(self, body=None, path='/status', raw=None):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        request = Request(self.url + path, data=data, headers={'Content-Type': 'application/json'})
        try:
            response = urlopen(request, timeout=3)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def start(self):
        self.config.write_text(json.dumps(self.settings), encoding='utf-8')
        self.server = self.spawn(SCHEDULER, self.config, 'scheduler')
        self.wait_for(lambda: self.request()[0] == 200)

    def command(self, action, **fields):
        return self.request({'action': action, **fields}, '/commands')

    def test_status_run_conflict_stale_stop_and_actual_run_id(self):
        self.settings['server']['secret'] = 'do-not-expose'
        self.start()
        code, status = self.request()
        self.assertEqual(200, code)
        self.assertEqual(['scheduler-probe'], status['plugins'])
        self.assertEqual([], status['clients'])
        self.assertEqual(str(self.config), status['config_path'])
        self.assertEqual('[redacted]', status['configuration']['server']['secret'])
        self.assertLess(status['snapshot_age_ms'], 1000)
        code, created = self.command('run', plugin_type='scheduler-probe', dataset_version='d1', params={})
        self.assertEqual(200, code, created)
        run_id = created['run_id']
        self.assertEqual(run_id, self.request()[1]['active_runs'][0]['run_id'])
        self.assertEqual(409, self.command('run', plugin_type='scheduler-probe', dataset_version='d1')[0])
        self.assertEqual(409, self.command('stop', plugin_type='scheduler-probe', run_id='old-run')[0])
        self.assertEqual(run_id, self.request()[1]['active_runs'][0]['run_id'])
        code, stopped = self.command('stop', plugin_type='scheduler-probe', run_id=run_id)
        self.assertEqual(200, code, stopped)
        self.assertEqual(run_id, stopped['run_id'])
        self.assertEqual([], self.request()[1]['active_runs'])

    def test_invalid_requests_and_plugin_failure_are_not_reported_as_success(self):
        self.settings['plugins']['probe']['throw_plan'] = True
        self.start()
        for body in ([], {}, {'action': 3}, {'action': 'unknown'}, {'action': 'run'},
                     {'action': 'stop', 'plugin_type': 3},
                     {'action': 'run', 'plugin_type': 'scheduler-probe', 'dataset_version': 'd1', 'params': []}):
            with self.subTest(body=body):
                code, response = self.request(body, '/commands')
                self.assertEqual(400, code)
                self.assertFalse(response['ok'])
        self.assertEqual(400, self.request(path='/commands', raw=b'{broken')[0])
        self.assertEqual(409, self.command('wakeup', plugin_type='missing')[0])
        code, result = self.command('run', plugin_type='scheduler-probe', dataset_version='d1')
        self.assertEqual(409, code)
        self.assertIn('could not be created', result['error'])
        self.assertEqual([], self.request()[1]['active_runs'])
        self.assertEqual(200, self.command('wakeup')[0])

    def test_partial_http_disconnect_does_not_block_other_requests_or_shutdown(self):
        self.start()
        peer = socket.create_connection(('127.0.0.1', self.port))
        self.addCleanup(peer.close)
        peer.sendall(b'POST /commands HTTP/1.1\r\nContent-Length: 99\r\n\r\n{')
        started = time.monotonic()
        self.assertEqual(200, self.request()[0])
        self.assertLess(time.monotonic() - started, 1.5)
        peer.close()
        # A short body is discarded, never executed as a partial command.
        self.assertEqual([], self.request()[1]['active_runs'])
        self.server.stdin.write(b'quit\n')
        self.server.stdin.flush()
        self.assertEqual(0, self.server.wait(timeout=3))

    def test_non_loopback_binding_and_port_conflict_fail_startup(self):
        for endpoint in ('http://0.0.0.0:5556', 'http://127.0.0.1:bad'):
            self.settings['server']['management_endpoint'] = endpoint
            self.config.write_text(json.dumps(self.settings), encoding='utf-8')
            result = subprocess.run([str(SCHEDULER), str(self.config)], cwd=self.root,
                input=b'quit\n', capture_output=True, timeout=6,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertNotEqual(0, result.returncode)
        with socket.socket() as bound:
            bound.bind(('127.0.0.1', self.port))
            bound.listen()
            self.settings['server']['management_endpoint'] = self.url
            self.config.write_text(json.dumps(self.settings), encoding='utf-8')
            result = subprocess.run([str(SCHEDULER), str(self.config)], cwd=self.root,
                input=b'quit\n', capture_output=True, timeout=6,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertNotEqual(0, result.returncode)
            self.assertIn(b'cannot listen on management endpoint', result.stdout + result.stderr)

    def test_real_client_wakes_immediately_shows_task_and_disconnect_reclaims(self):
        binary = SCHEDULER.parent
        manifest = self.root / 'manifest.txt'
        manifest.write_text('sample.stp,1\n', encoding='utf-8')
        for name, version in [('program', 'v1'), ('dataset', 'd1')]:
            directory = self.root / name
            directory.mkdir()
            (directory / 'version.txt').write_text(version, encoding='utf-8')
        plugin = {'dll': str(binary / 'coverage.dll'), 'enabled': True, 'store_type': 'file',
                  'store_dir': str(self.root / 'store')}
        self.settings['plugins'] = {'coverage': dict(plugin, stp_manifest=str(manifest), task_size=1,
            expected_program_version='v1', expected_dataset_version='d1')}
        client = {'scheduler': {'endpoint': self.router, 'poll_timeout_ms': 5},
            'client': {'worker_id': 'management-worker', 'heartbeat_interval_ms': 100, 'task_threads': 1,
                       'request_backoff_initial_ms': 30000, 'request_backoff_max_ms': 30000},
            'plugins': {'coverage': dict(plugin, dry_run=True, dry_run_delay_ms=3000, store_cov=False,
                program_dir=str(self.root / 'program'), stp_root=str(self.root / 'dataset'),
                export_dir=str(self.root / 'reports'))}}
        client_config = self.root / 'client.json'
        client_config.write_text(json.dumps(client), encoding='utf-8')
        self.start()
        worker = self.spawn(binary / 'client.exe', client_config, 'client')
        self.wait_for(lambda: self.request()[1]['clients'])
        # Allow the initial empty task request to enter the 30-second backoff.
        time.sleep(.3)
        started = time.monotonic()
        code, run = self.command('run', plugin_type='coverage', dataset_version='d1')
        self.assertEqual(200, code, run)
        status = self.wait_for(lambda: (state if state['in_flight'] else None)
                               if (state := self.request()[1]) else None, timeout=2)
        self.assertLess(time.monotonic() - started, 2)
        current = status['clients'][0]
        self.assertEqual('management-worker', current['worker_id'])
        self.assertEqual('coverage', current['plugin_type'])
        self.assertTrue(current['registered'])
        self.assertEqual('busy', current['status'])
        self.assertEqual(run['run_id'], current['tasks'][0]['run_id'])
        self.assertTrue(current['tasks'][0]['attempt_id'])
        worker.kill()
        worker.wait(timeout=3)
        self.wait_for(lambda: not self.request()[1]['clients'])
        self.wait_for(lambda: self.request()[1]['in_flight'] == 0)
        self.assertEqual(run['run_id'], self.request()[1]['active_runs'][0]['run_id'])
        self.assertEqual(200, self.command('stop', plugin_type='coverage', run_id=run['run_id'])[0])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--scheduler', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    options, remaining = parser.parse_known_args()
    SCHEDULER = options.scheduler.resolve()
    PROBE = options.probe.resolve()
    unittest.main(argv=[__file__, *remaining])
