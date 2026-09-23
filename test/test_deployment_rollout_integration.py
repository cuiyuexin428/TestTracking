"""Real unified rollout, durable run boundary and offline Agent catch-up.

Owns every native process and a private PostgreSQL cluster. Business execution
uses the coverage dry-run fixture; downloads, Worker activation, queueing,
publication, result commits and recovery use production implementations.
"""
from __future__ import annotations

import argparse
import copy
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
import uuid
import zipfile
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'test')]
from result_service.app.config import ServiceConfig
from result_service.app.server import Handler, ResultHTTPServer, ResultService
from test_coverage_settle import PrivatePostgres, sql_text
from test_runtime_integration import CapturedProcess, unused_port

OPTIONS = None


class RolloutEnvironment:
    def __init__(self, binary: Path, pg_bin: Path, *, root: Path | None = None):
        self.binary = binary.resolve()
        self.root = root or Path(tempfile.mkdtemp(prefix='rollout-e2e-', dir=ROOT / 'build/test'))
        self.root.mkdir(parents=True, exist_ok=True)
        self.pg = PrivatePostgres(pg_bin, self.binary / 'scheduler.exe')
        self.processes = []
        self.agents = {}
        self.sequence = 0
        self.service = self.http = self.scheduler = None
        self.management = f'http://127.0.0.1:{unused_port()}'
        self.router = f'tcp://127.0.0.1:{unused_port()}'
        self.scheduler_path = self.root / 'scheduler.json'
        self.template_path = self.root / 'client-template.json'

    @staticmethod
    def save(path, value):
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')

    def start(self):
        self.pg.start()
        manifest = self.root / 'bootstrap-manifest.txt'
        manifest.write_text('bootstrap.stp,1\n', encoding='utf-8')
        self.save(self.scheduler_path, {
            'server': {'router_endpoint': self.router, 'management_endpoint': self.management,
                       'db_conn': self.pg.conn, 'poll_timeout_ms': 5,
                       'agent_timeout_ms': 7000, 'client_timeout_ms': 2500,
                       'task_lease_timeout_ms': 30000},
            'plugins': {'coverage': {'enabled': True, 'dll': str(self.binary / 'coverage.dll'),
                'store_type': 'postgres', 'db_conn': self.pg.conn, 'task_size': 1,
                'stp_manifest': str(manifest), 'expected_dataset_version': 'bootstrap',
                'expected_program_version': 'bootstrap'}}})
        self.save(self.template_path, {'plugins': {'coverage': {
            'enabled': False, 'store_type': 'postgres', 'db_conn': self.pg.conn,
            'dry_run_delay_ms': 1400, 'process_timeout_ms': 15000}}})
        self.config = ServiceConfig(workspace=str(self.root), providers={}, db_conn=self.pg.conn,
            scheduler_management_url=self.management, scheduler_config_path=str(self.scheduler_path),
            client_config_path=str(self.template_path), artifact_store_dir=str(self.root / 'resources'))
        self.start_scheduler()
        self.service = ResultService(self.config)

        class QuietHandler(Handler):
            def log_message(self, *args):
                pass

        self.handler = QuietHandler
        self.handler.service = self.service
        self.http = ResultHTTPServer(('127.0.0.1', 0), self.handler)
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.http.server_port}'
        self.wait(lambda: self.get('/api/scheduler').get('online'), 'result API connected')
        return self

    def start_scheduler(self):
        self.sequence += 1
        self.scheduler = CapturedProcess(self.binary / 'scheduler.exe', self.scheduler_path,
            self.root, 'scheduler-' + str(self.sequence), self.pg.env)
        self.processes.append(self.scheduler)
        self.wait(lambda: self.request('/status', base=self.management)[0] == 200,
                  'native scheduler started')

    def restart_scheduler(self):
        self.scheduler.close()
        self.start_scheduler()

    def restart_service(self):
        self.service.close()
        self.service = ResultService(self.config)
        self.handler.service = self.service

    def agent(self, name):
        directory = self.root / name
        directory.mkdir(exist_ok=True)
        config = directory / 'client.json'
        if not config.exists():
            self.save(config, {
                'scheduler': {'endpoint': self.router, 'poll_timeout_ms': 5},
                'client': {'worker_id': name, 'heartbeat_interval_ms': 100,
                    'deployment_endpoint': self.url, 'deployment_poll_seconds': 1,
                    'task_threads': 1, 'register_timeout_ms': 500, 'register_max_attempts': 5,
                    'request_backoff_initial_ms': 30, 'request_backoff_max_ms': 200},
                'plugins': {}})
        self.sequence += 1
        process = CapturedProcess(self.binary / 'client.exe', config, directory,
            'client-' + str(self.sequence), self.pg.env)
        self.processes.append(process)
        self.agents[name] = process
        return process

    def request(self, path, body=None, *, base=None, method=None):
        request = Request((base or self.url) + path,
            data=json.dumps(body).encode('utf-8') if body is not None else None,
            headers={'Content-Type': 'application/json'}, method=method)
        try:
            response = urlopen(request, timeout=15)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def get(self, path):
        code, data = self.request(path)
        if code != 200:
            raise AssertionError((code, data))
        return data

    def post(self, path, body):
        code, data = self.request(path, body)
        if not 200 <= code < 300:
            raise AssertionError((path, code, data))
        return data

    def wait(self, predicate, label, timeout=65):
        end = time.monotonic() + timeout
        last = None
        while time.monotonic() < end:
            try:
                value = predicate()
                if value:
                    return value
            except (OSError, HTTPError, URLError) as error:
                last = str(error)
            time.sleep(.1)
        tail = '\n'.join(process.text()[-3500:] for process in self.processes)
        raise AssertionError(f'{label} timed out ({last}); evidence: {self.root}\n{tail}')

    def upload(self, kind, name, version, files):
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
            for filename, content in files.items():
                archive.writestr(filename, content)
        raw = output.getvalue()
        return self.service.artifacts.upload(io.BytesIO(raw), len(raw), kind=kind,
            name=name, version=version, filename=name + '.zip')[0]['artifact_id']

    def create_profile(self):
        plugin = self.upload('plugin', 'coverage', '1',
                             {'coverage.dll': (self.binary / 'coverage.dll').read_bytes()})
        program = self.upload('program', 'acceptance', '1',
            {'acceptance_program.exe': (self.binary / 'acceptance_program.exe').read_bytes()})
        self.dataset_v1 = self.upload('dataset', 'standard-parts', '1',
            {**{f'part-{n}.stp': f'old geometry {n}'.encode() for n in range(8)},
             'version.txt': b' release-1 \n'})
        self.dataset_v2 = self.upload('dataset', 'standard-parts', '2',
            {**{f'part-{n}.stp': f'new geometry {n}'.encode() for n in range(4)},
             'version.txt': b' release-1 \n'})
        definition = {'revision': None, 'name': 'Coverage unified release',
            'description': 'Owned native rollout integration', 'plugins': {'coverage': {
                'resources': {'plugin': {'artifact_id': plugin, 'entry': 'coverage.dll'},
                    'program': {'artifact_id': program, 'entry': 'acceptance_program.exe'},
                    'collector': {'artifact_id': program, 'entry': 'acceptance_program.exe'},
                    'dataset': {'artifact_id': self.dataset_v1, 'root': ''}},
                'dry_run': True, 'store_cov': False, 'engine_dlls': []}},
            'schedule': {'task_threads': 1, 'execution_windows': [], 'update_windows': []}}
        self.profile = self.post('/api/deployment-profiles', definition)
        return self.profile

    def preview(self, *, mode='update', clients=None):
        body = {'revision': self.profile['revision'], 'mode': mode}
        if clients is not None:
            body['client_ids'] = clients
        route = '/api/deployment-profiles/' + self.profile['profile_id']
        return body, self.post(route + '/preview', body)

    def publish(self, *, mode='update', clients=None):
        body, review = self.preview(mode=mode, clients=clients)
        body.update(plan_fingerprint=review['plan_fingerprint'], request_id=uuid.uuid4().hex,
                    operator_id='rollout-integration')
        route = '/api/deployment-profiles/' + self.profile['profile_id']
        result = self.post(route + '/publish', body)
        return result, body, review

    def rollout(self, identifier):
        return self.get('/api/deployment-rollouts/' + identifier)

    def run(self, *, force=False):
        body = {'action': 'run', 'plugin_type': 'coverage',
                'dataset_version': self.get('/api/scheduler')['run_datasets']['coverage']['dataset_version']}
        if force:
            body.update(force_rerun=True, force_rerun_reason='independent queue boundary regression')
        return self.post('/api/scheduler/commands', body)

    def run_state(self, run_id):
        return self.pg.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run_id))

    def close(self):
        if self.service:
            self.service.close()
        for process in reversed(self.processes):
            process.close()
        if self.http:
            self.http.shutdown()
            self.thread.join(10)
            self.http.server_close()
        self.pg.close()


@unittest.skipUnless(os.name == 'nt', 'Windows Agent rollout')
class DeploymentRolloutIntegrationTests(unittest.TestCase):
    def test_queued_boundary_survives_restart_and_offline_client_catches_up(self):
        env = RolloutEnvironment(OPTIONS.bin_dir, OPTIONS.pg_bin)
        self.addCleanup(env.close)
        env.start()
        names = ['rollout-01', 'rollout-02', 'rollout-03']
        for name in names:
            env.agent(name)
        env.wait(lambda: len(env.get('/api/agents')['agents']) == 3, 'three blank Agents online')
        env.create_profile()
        initial, _, _ = env.publish(mode='assign', clients=names)
        initial_id = initial['rollout_id']
        env.wait(lambda: env.rollout(initial_id)['status'] == 'completed', 'initial assignment complete')
        old_version = env.get('/api/scheduler')['run_datasets']['coverage']['dataset_version']
        old_desired = {row['client_id']: row['desired']['revision']
                       for row in env.get('/api/deployments')['clients']}

        env.agents['rollout-03'].close()
        env.wait(lambda: not next(row for row in env.get('/api/agents')['agents']
                                  if row['client_id'] == 'rollout-03')['online'], 'third Agent offline')
        first = env.run()['run_id']
        second = env.run(force=True)['run_id']
        self.assertEqual('queued', env.run_state(second))
        changed = copy.deepcopy(env.profile)
        changed['plugins']['coverage']['resources']['dataset']['artifact_id'] = env.dataset_v2
        code, env.profile = env.request('/api/deployment-profiles/' + env.profile['profile_id'], changed, method='PUT')
        self.assertEqual(200, code, env.profile)
        update, publication, review = env.publish()
        identifier = update['rollout_id']
        self.assertEqual(set(names), {row['client_id'] for row in review['clients']})
        self.assertEqual(identifier, env.post('/api/deployment-profiles/' + env.profile['profile_id']
                                           + '/publish', publication)['rollout_id'])
        self.assertIn(env.rollout(identifier)['status'], {'pending', 'waiting_runs'})
        self.assertEqual(old_version, env.get('/api/scheduler')['run_datasets']['coverage']['dataset_version'])
        self.assertEqual(old_desired, {row['client_id']: row['desired']['revision']
                                      for row in env.get('/api/deployments')['clients']})
        code, rejected = env.request('/api/scheduler/commands', {'action': 'run',
            'plugin_type': 'coverage', 'dataset_version': old_version, 'force_rerun': True,
            'force_rerun_reason': 'must be rejected after update boundary'})
        self.assertEqual(409, code, rejected)

        env.restart_service()
        env.restart_scheduler()
        self.assertEqual(identifier, env.rollout(identifier)['rollout_id'])
        env.wait(lambda: env.run_state(first) == 'finished' and env.run_state(second) == 'finished',
                 'both pre-update batches finish with original resources')
        result = env.wait(lambda: (row if (row := env.rollout(identifier))['status'] == 'ready' else None),
                          'online machines complete without waiting for offline one')
        self.assertEqual(2, sum(row['status'] == 'ready' for row in result['clients']), result)
        self.assertEqual('offline', next(row for row in result['clients']
                                        if row['client_id'] == 'rollout-03')['status'])
        new_version = env.get('/api/scheduler')['run_datasets']['coverage']['dataset_version']
        self.assertNotEqual(old_version, new_version)
        for run_id in (first, second):
            self.assertEqual(old_version, env.pg.sql('SELECT dataset_version FROM runs WHERE run_id=' + sql_text(run_id)))
            self.assertEqual('8', env.pg.sql('SELECT count(*) FROM coverage_file_points WHERE run_id=' + sql_text(run_id)))
        third = env.run()['run_id']
        env.agent('rollout-03')
        env.wait(lambda: env.rollout(identifier)['status'] == 'completed', 'offline Agent catches up')
        env.wait(lambda: env.run_state(third) == 'finished', 'new release run completes')
        self.assertEqual('4', env.pg.sql('SELECT count(*) FROM coverage_file_points WHERE run_id=' + sql_text(third)))

        # A DLL-only release retains program/data versions but must change both
        # Worker admission and run deduplication identity. Windows PE permits
        # an inert overlay, so exercise a different actual downloaded package.
        plugin_v2 = env.upload('plugin', 'coverage', '2',
            {'coverage.dll': (env.binary / 'coverage.dll').read_bytes() + b'\nrollout-release-2\n'})
        changed = copy.deepcopy(env.profile)
        changed['plugins']['coverage']['resources']['plugin']['artifact_id'] = plugin_v2
        code, env.profile = env.request('/api/deployment-profiles/' + env.profile['profile_id'], changed, method='PUT')
        self.assertEqual(200, code, env.profile)
        dll_release, _, _ = env.publish()
        env.wait(lambda: env.rollout(dll_release['rollout_id'])['status'] == 'completed',
                 'DLL-only release activates all Workers')
        self.assertNotEqual(update['target_ids'], dll_release['target_ids'])
        self.assertEqual(new_version, env.get('/api/scheduler')['run_datasets']['coverage']['dataset_version'])
        fourth = env.run()['run_id']
        self.assertNotEqual(third, fourth, 'a new plugin release is a distinct workload')
        env.wait(lambda: env.run_state(fourth) == 'finished', 'DLL-only release gets fresh run')
        self.assertEqual('4', env.pg.sql('SELECT count(*) FROM coverage_file_points WHERE run_id=' + sql_text(fourth)))
        env.save(env.root / 'evidence.json', {'old_runs': [first, second], 'new_run': third, 'dll_only_run': fourth,
            'old_version': old_version, 'new_version': new_version,
            'rollout': env.rollout(identifier), 'dll_only_rollout': env.rollout(dll_release['rollout_id']),
            'scheduler': env.get('/api/scheduler')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--pg-bin', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.bin_dir = OPTIONS.bin_dir.resolve()
    OPTIONS.pg_bin = OPTIONS.pg_bin.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
