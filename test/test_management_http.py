"""HTTP contracts for management without coupling analysis to the scheduler."""
import json
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from result_service.app.config import ServiceConfig
from result_service.app.management import ManagementError
from result_service.app.server import Handler, ResultHTTPServer, ResultService


class ManagementHTTPTests(unittest.TestCase):
    def setUp(self):
        self.service = ResultService(ServiceConfig.defaults())
        handler = type('TestHandler', (Handler,), {'service': self.service,
                                                  'log_message': lambda *args: None})
        self.server = ResultHTTPServer(('127.0.0.1', 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f'http://127.0.0.1:{self.server.server_port}'

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(5)
        self.server.server_close()
        self.service.close()

    def request(self, path, method='GET', body=None, headers=None):
        request = Request(self.base + path, method=method,
                          data=json.dumps(body).encode() if body is not None else None,
                          headers={'Content-Type': 'application/json', **(headers or {})})
        try:
            response = urlopen(request, timeout=3)
        except HTTPError as exc:
            response = exc
        with response:
            return response.status, json.load(response)

    def test_management_routes_delegate_and_preserve_rejections(self):
        with patch.object(self.service.management, 'get_status', return_value={'online': True}):
            self.assertEqual((200, {'online': True}), self.request('/api/scheduler'))
        command = {'action': 'stop', 'plugin_type': 'coverage', 'run_id': 'old-run'}
        with patch.object(self.service.management, 'command',
                          side_effect=ManagementError('run changed', status=409)) as execute:
            self.assertEqual((409, {'error': 'run changed'}),
                             self.request('/api/scheduler/commands', 'POST', command))
            execute.assert_called_once_with(command)
        with patch.object(self.service.management, 'logs', return_value={'lines': ['hello']}) as logs:
            self.assertEqual(200, self.request('/api/logs?file=scheduler.log&limit=20')[0])
            logs.assert_called_once_with({'file': ['scheduler.log'], 'limit': ['20']})

    def test_duplicate_metadata_survives_scheduler_proxy_and_public_http(self):
        class SchedulerHandler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                response = {'error': 'equivalent work already exists', 'duplicate': True,
                            'run_id': 'previous-run', 'status': request['dataset_version'],
                            'force_rerun_available': True, 'internal_detail': 'private'}
                payload = json.dumps(response).encode()
                self.send_response(409)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        scheduler = ThreadingHTTPServer(('127.0.0.1', 0), SchedulerHandler)
        thread = threading.Thread(target=scheduler.serve_forever, daemon=True)
        thread.start()
        try:
            with patch.object(self.service.management, '_url', f'http://127.0.0.1:{scheduler.server_port}'):
                for status in ('finished', 'running', 'queued'):
                    with self.subTest(status=status):
                        self.assertEqual((409, {'error': 'equivalent work already exists',
                            'duplicate': True, 'run_id': 'previous-run', 'status': status,
                            'force_rerun_available': True}), self.request('/api/scheduler/commands', 'POST',
                            {'action': 'run', 'plugin_type': 'coverage', 'dataset_version': status}))
        finally:
            scheduler.shutdown()
            thread.join(5)
            scheduler.server_close()

    def test_config_conflict_keeps_http_status_and_bad_body_rejected(self):
        with patch.object(self.service.management, 'get_config', return_value={'available': True}) as read:
            self.assertEqual(200, self.request('/api/config/scheduler')[0])
            read.assert_called_once_with('scheduler')
        with patch.object(self.service.management, 'save_config',
                          side_effect=ManagementError('config changed', status=409)):
            self.assertEqual(409, self.request('/api/config/scheduler', 'PUT',
                                             {'revision': 'stale', 'config': {}})[0])
        self.assertEqual(400, self.request('/api/scheduler/commands', 'POST', [1])[0])

    def test_analysis_endpoints_do_not_use_management(self):
        with patch.object(self.service.management, 'get_status', side_effect=AssertionError('offline')):
            self.assertEqual(200, self.request('/healthz')[0])
            self.assertEqual(200, self.request('/api/plugins')[0])
            self.assertEqual((200, []), self.request('/api/runs'))

    def test_lan_same_origin_and_cli_commands_remain_available_without_login(self):
        # Route the test connection locally while using a real LAN-style Host.
        # The browser's origin must match its visible URL, not the bind address.
        authorities = ['192.168.100.83:56330', 'tracking-server:56330', '[::1]:56330']
        with patch.object(self.service.management, 'command', return_value={'ok': True}) as execute:
            for authority in authorities:
                with self.subTest(authority=authority):
                    status, _ = self.request('/api/scheduler/commands', 'POST', {'action': 'wakeup'},
                        {'Host': authority, 'Origin': 'http://' + authority, 'Sec-Fetch-Site': 'same-origin',
                         'Content-Type': 'application/json; charset=utf-8'})
                    self.assertEqual(200, status)
            self.assertEqual(200, self.request('/api/scheduler/commands', 'POST', {'action': 'wakeup'})[0])
            self.assertEqual(4, execute.call_count)

    def test_custom_chat_endpoint_is_removed(self):
        self.assertEqual((404, {"error": "not found"}),
                         self.request('/api/ai/chat', 'POST', {'message': 'hello'}))

    def test_cross_origin_actions_are_rejected_before_any_side_effect(self):
        cases = [
            {'Origin': 'http://unrelated.invalid'},
            {'Origin': 'null'},  # Sandboxed report frames have an opaque origin.
            {'Origin': self.base.replace('http:', 'https:')},
            {'Origin': self.base, 'Sec-Fetch-Site': 'cross-site'},
            {'Referer': 'http://unrelated.invalid/page'},
            {'Sec-Fetch-Site': 'same-site'},
        ]
        with patch.object(self.service.management, 'command') as command, \
                patch.object(self.service.management, 'save_config') as save, \
                patch.object(self.service, 'generate_report') as report, \
                patch.object(self.service, 'start_dsh_web') as start:
            for headers in cases:
                for method, path in [('POST', '/api/scheduler/commands'), ('PUT', '/api/config/scheduler'),
                                     ('POST', '/api/plugins/coverage/reports'),
                                     ('POST', '/api/ai/dsh-web')]:
                    with self.subTest(headers=headers, path=path):
                        self.assertEqual(403, self.request(path, method, {}, headers)[0])
            for operation in (command, save, report, start):
                operation.assert_not_called()

    def test_same_origin_referer_is_accepted_but_plain_text_json_cannot_execute(self):
        with patch.object(self.service.management, 'command', return_value={'ok': True}) as command:
            self.assertEqual(200, self.request('/api/scheduler/commands', 'POST', {},
                                             {'Referer': self.base + '/?page=overview'})[0])
            command.reset_mock()
            for media_type in ('text/plain', 'application/x-www-form-urlencoded', ''):
                self.assertEqual(415, self.request('/api/scheduler/commands', 'POST', {},
                                                 {'Content-Type': media_type})[0])
            command.assert_not_called()
        with patch.object(self.service, 'update_report_script') as save:
            self.assertEqual(415, self.request('/api/plugins/coverage/report-script', 'PUT', {},
                                             {'Content-Type': 'text/plain'})[0])
            save.assert_not_called()

    def test_same_origin_dsh_start_requires_operator_identity_and_preflight_is_not_enabled(self):
        operator = '123e4567-e89b-42d3-a456-426614174001'
        with patch.object(self.service, 'start_dsh_web', return_value=(
                200, {'url': f'/ai/dsh/{operator}/'})) as start:
            self.assertEqual(200, self.request('/api/ai/dsh-web', 'POST',
                                             {'operator_id': operator},
                                             {'Origin': self.base})[0])
            start.assert_called_once_with(operator)
        self.assertEqual(404, self.request('/api/config/scheduler', 'OPTIONS', headers={
            'Origin': 'http://unrelated.invalid', 'Access-Control-Request-Method': 'PUT',
            'Access-Control-Request-Headers': 'content-type'})[0])


if __name__ == '__main__':
    unittest.main()
