"""Program artifact identities stay consistent from publication to scheduler config."""
from __future__ import annotations

import copy
import io
import json
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from result_service.app.artifacts import ArtifactError
from result_service.app.config import ServiceConfig
from result_service.app.deployments import DeploymentError, DeploymentStore
from result_service.app.management import ManagementError
from result_service.app.server import Handler, ResultHTTPServer, ResultService


class ProgramBindingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='program-bindings-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scheduler_path = self.root / 'scheduler.json'
        self.scheduler_config = {
            'server': {'router_endpoint': 'tcp://*:5555'},
            'plugins': {
                'coverage': {'enabled': True, 'dll': 'coverage.dll',
                             'expected_program_version': 'manual-v1', 'task_size': 25},
                'stpcompare': {'enabled': True, 'dll': 'stpcompare.dll',
                               'expected_baseline_program_version': 'manual-old',
                               'expected_candidate_program_version': 'manual-new'},
            },
        }
        self.scheduler_path.write_text(json.dumps(self.scheduler_config), encoding='utf-8')
        self.service = ResultService(ServiceConfig(
            workspace=str(self.root), artifact_store_dir=str(self.root / 'artifacts'),
            scheduler_config_path=str(self.scheduler_path), providers={}))
        self.addCleanup(self.service.close)
        status = {'online': True, 'instance_id': 'original-scheduler',
                  'config_path': str(self.scheduler_path),
                  'configuration': copy.deepcopy(self.scheduler_config),
                  'agents': [{'client_id': 'worker-http', 'online': True, 'state': 'ready', 'busy': False}],
                  'active_runs': [], 'queued_runs': []}
        mocked = patch.object(self.service.management, 'get_status', return_value=status)
        mocked.start()
        self.addCleanup(mocked.stop)
        self.store = self.service.deployments
        self.bundle = self.zip_bytes({
            'bin/engine.exe': b'engine A', 'bin/alternate.exe': b'engine B',
            'OpenCppCoverage.exe': b'coverage tool', 'engine.dll': b'engine dependency'})
        self.program = self.upload(self.bundle, filename='program.zip')

    @staticmethod
    def zip_bytes(files):
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w') as archive:
            for name, content in files.items():
                archive.writestr(zipfile.ZipInfo(name), content)
        return output.getvalue()

    def upload(self, content, *, kind='program', name='engine', version='1', filename='engine.exe'):
        record, _ = self.service.artifacts.upload(
            io.BytesIO(content), len(content), kind=kind, name=name, version=version,
            filename=filename)
        return record

    def coverage_body(self, record=None, *, slot='program', executable='bin/engine.exe'):
        record = record or self.program
        prefix = '{artifact:' + slot + '}'
        return {
            'components': [{'slot': slot, 'artifact_id': record['artifact_id']}],
            'config_patch': {'plugins': {'coverage': {
                'enabled': True, 'dry_run': False, 'store_cov': False, 'program_dir': prefix + '/bin',
                'target_exe': prefix + '/' + executable,
                'opencppcoverage': prefix + '/OpenCppCoverage.exe',
                'engine_dlls': [prefix + '/engine.dll'],
                'program_version': 'caller-supplied-wrong-version',
            }}},
        }

    def stp_body(self, baseline=None, candidate=None):
        return {
            'components': [
                {'slot': 'baseline', 'artifact_id': (baseline or self.program)['artifact_id']},
                {'slot': 'candidate', 'artifact_id': (candidate or self.program)['artifact_id']},
            ],
            'config_patch': {'plugins': {'stpcompare': {
                'enabled': True, 'dry_run': False,
                'programs': {role: {
                    'executable': '{artifact:' + role + '}/bin/engine.exe',
                    'version': 'caller-supplied-wrong-version',
                    'arguments': ['--input', '{stp}', '--output-dir', '{outdir}'],
                } for role in ('baseline', 'candidate')},
            }}},
        }

    def prepare(self, body):
        return self.store.prepare(body['components'], body['config_patch'])

    def coverage_version(self, body):
        return self.prepare(body)['scheduler_plugins']['coverage']['expected_program_version']

    def test_catalog_labels_and_slot_names_do_not_change_content_identity(self):
        for filename, content, executable in (
            ('program.zip', self.bundle, 'bin/engine.exe'),
            ('engine.exe', b'plain executable', 'engine.exe'),
        ):
            with self.subTest(filename=filename):
                first = self.upload(content, name=filename, version='label-1', filename=filename)
                second = self.upload(content, name=filename, version='label-2', filename=filename)
                self.assertNotEqual(first['artifact_id'], second['artifact_id'])
                original = self.coverage_body(first, executable=executable)
                renamed = self.coverage_body(second, slot='renamed', executable=executable)
                self.assertEqual(self.coverage_version(original), self.coverage_version(renamed))
                result = self.prepare(original)
                self.assertEqual(first['sha256'], result['program_bindings'][0]['artifact_sha256'])
                self.assertTrue(result['program_bindings'][0]['version'].startswith('sha256:'))
                self.assertEqual('caller-supplied-wrong-version',
                                 original['config_patch']['plugins']['coverage']['program_version'])

    def test_content_entrypoint_and_coverage_execution_settings_affect_identity(self):
        original = self.coverage_body()
        version = self.coverage_version(original)
        changed_content = self.upload(self.zip_bytes({
            'bin/engine.exe': b'changed engine', 'OpenCppCoverage.exe': b'coverage tool'}),
            version='2', filename='program.zip')
        variants = [self.coverage_body(changed_content),
                    self.coverage_body(executable='bin/alternate.exe')]
        for field, value in (('dry_run', True), ('store_cov', True), ('engine_dlls', []),
                             ('opencppcoverage', 'C:/tools/another.exe')):
            modified = copy.deepcopy(original)
            modified['config_patch']['plugins']['coverage'][field] = value
            variants.append(modified)
        for body in variants:
            self.assertNotEqual(version, self.coverage_version(body))

    def test_stp_roles_and_arguments_bind_independently(self):
        second = self.upload(self.zip_bytes({'bin/engine.exe': b'new implementation'}),
                             version='2', filename='program.zip')
        body = self.stp_body(candidate=second)
        prepared = self.prepare(body)
        expected = prepared['scheduler_plugins']['stpcompare']
        self.assertEqual({'baseline', 'candidate'},
                         {item['role'] for item in prepared['program_bindings']})
        for role in ('baseline', 'candidate'):
            self.assertEqual(expected[f'expected_{role}_program_version'],
                             prepared['config_patch']['plugins']['stpcompare']['programs'][role]['version'])
        swapped = self.prepare(self.stp_body(baseline=second))['scheduler_plugins']['stpcompare']
        self.assertEqual(expected['expected_baseline_program_version'],
                         swapped['expected_candidate_program_version'])
        self.assertEqual(expected['expected_candidate_program_version'],
                         swapped['expected_baseline_program_version'])
        body['config_patch']['plugins']['stpcompare']['programs']['candidate']['arguments'] += ['--new-format']
        changed = self.prepare(body)['scheduler_plugins']['stpcompare']
        self.assertEqual(expected['expected_baseline_program_version'], changed['expected_baseline_program_version'])
        self.assertNotEqual(expected['expected_candidate_program_version'], changed['expected_candidate_program_version'])

    def test_stp_argument_artifact_content_changes_identity_but_slot_alias_does_not(self):
        first = self.upload(b'first converter settings', kind='dataset', name='settings',
                            version='1', filename='settings.json')
        second = self.upload(b'second converter settings', kind='dataset', name='settings',
                             version='2', filename='settings.json')

        def body_for(record, slot):
            body = self.stp_body()
            body['components'].append({'slot': slot, 'artifact_id': record['artifact_id']})
            body['config_patch']['plugins']['stpcompare']['programs']['candidate']['arguments'] += [
                '--settings', '{artifact:' + slot + '}/settings.json']
            return body

        initial = self.prepare(body_for(first, 'settings'))['scheduler_plugins']['stpcompare']
        renamed = self.prepare(body_for(first, 'renamed'))['scheduler_plugins']['stpcompare']
        changed = self.prepare(body_for(second, 'settings'))['scheduler_plugins']['stpcompare']
        self.assertEqual(initial, renamed)
        self.assertEqual(initial['expected_baseline_program_version'], changed['expected_baseline_program_version'])
        self.assertNotEqual(initial['expected_candidate_program_version'], changed['expected_candidate_program_version'])

    def test_invalid_program_kind_or_executable_path_is_rejected(self):
        dataset = self.upload(self.bundle, kind='dataset', name='inputs', filename='data.zip')
        raw = self.upload(b'executable', name='raw', filename='engine.exe')
        cases = [self.coverage_body(dataset), self.coverage_body(executable='../escape.exe'),
                 self.coverage_body(executable='missing.exe'),
                 self.coverage_body(raw, executable='wrong-name.exe')]
        for body in cases:
            with self.subTest(body=body), self.assertRaises((ArtifactError, DeploymentError)):
                self.prepare(body)
        self.assertFalse(self.store.path.exists())

    def test_bound_configs_require_explicit_execution_semantics(self):
        for field in ('dry_run', 'store_cov', 'program_dir', 'opencppcoverage', 'engine_dlls'):
            body = self.coverage_body()
            del body['config_patch']['plugins']['coverage'][field]
            with self.subTest(field=field), self.assertRaisesRegex(ArtifactError, 'explicit'):
                self.prepare(body)
        for missing in ('dry_run', 'arguments'):
            body = self.stp_body()
            plugin = body['config_patch']['plugins']['stpcompare']
            if missing == 'dry_run':
                del plugin['dry_run']
            else:
                del plugin['programs']['baseline']['arguments']
            with self.subTest(field=missing), self.assertRaisesRegex(ArtifactError, 'explicit'):
                self.prepare(body)

    def test_legacy_local_programs_keep_manual_versions(self):
        body = {'components': [], 'config_patch': {'plugins': {
            'coverage': {'target_exe': 'C:/tools/engine.exe', 'program_version': 'manual-v1'},
            'stpcompare': {'programs': {'baseline': {'executable': 'C:/tools/old.exe', 'version': 'old-v1'},
                                      'candidate': {'executable': 'C:/tools/new.exe', 'version': 'new-v1'}}},
        }}}
        prepared = self.prepare(body)
        self.assertEqual(body['config_patch'], prepared['config_patch'])
        self.assertEqual([], prepared['program_bindings'])
        self.assertEqual({}, prepared['scheduler_plugins'])

    def test_normalization_persists_and_rollback_restores_original_binding(self):
        body = self.coverage_body()
        first, changed = self.store.put_desired('worker-1', {'revision': None, **body})
        self.assertTrue(changed)
        version = self.coverage_version(body)
        self.assertEqual(version, first['desired']['config_patch']['plugins']['coverage']['program_version'])
        body['config_patch']['plugins']['coverage']['program_version'] = 'another-fake-version'
        unchanged, changed = self.store.put_desired('worker-1', {'revision': None, **body})
        self.assertFalse(changed)
        self.assertEqual(first['desired']['revision'], unchanged['desired']['revision'])
        second_body = self.coverage_body(executable='bin/alternate.exe')
        second, _ = self.store.put_desired('worker-1', {
            'revision': first['desired']['revision'], **second_body})
        reopened = DeploymentStore(self.store.path, self.service.artifacts)
        restored, _ = reopened.put_desired('worker-1', {
            'revision': second['desired']['revision'], 'action': 'rollback',
            'target_revision': first['desired']['revision']})
        self.assertEqual(first['desired']['config_patch'], restored['desired']['config_patch'])
        self.assertEqual(first['desired']['program_bindings'], restored['desired']['program_bindings'])
        self.assertEqual(version, restored['desired']['scheduler_plugins']['coverage']['expected_program_version'])

    def test_preview_has_no_config_or_deployment_side_effects(self):
        before_config = self.scheduler_path.read_bytes()
        before_catalog = self.service.artifacts.catalog_path.read_bytes()
        body = self.coverage_body()
        original = copy.deepcopy(body)
        preview = self.service.preview_deployment(body)
        self.assertEqual(original, body)
        self.assertEqual(self.coverage_version(body), preview['scheduler_plugins']['coverage']['expected_program_version'])
        self.assertEqual(before_config, self.scheduler_path.read_bytes())
        self.assertEqual(before_catalog, self.service.artifacts.catalog_path.read_bytes())
        self.assertFalse(self.store.path.exists())
        self.assertFalse(self.scheduler_path.with_suffix('.json.bak').exists())

    def test_binding_saves_expected_version_and_checks_config_revision(self):
        body = self.coverage_body()
        preview = self.service.preview_deployment(body)
        original = self.scheduler_path.read_bytes()
        with patch.object(self.service.management, 'require_plugin_idle') as idle:
            result = self.service.bind_deployment_scheduler({
                **body, 'scheduler_revision': preview['scheduler_revision']})
            idle.assert_called_once_with('coverage')
        saved = json.loads(self.scheduler_path.read_text(encoding='utf-8'))
        self.assertEqual(result['scheduler_plugins']['coverage']['expected_program_version'],
                         saved['plugins']['coverage']['expected_program_version'])
        self.assertEqual(self.scheduler_config['plugins']['stpcompare'], saved['plugins']['stpcompare'])
        self.assertEqual(25, saved['plugins']['coverage']['task_size'])
        self.assertEqual(original, self.scheduler_path.with_suffix('.json.bak').read_bytes())
        self.assertTrue(result['scheduler_restart_required'])
        with patch.object(self.service.management, 'require_plugin_idle') as idle:
            with self.assertRaises(ManagementError) as raised:
                self.service.bind_deployment_scheduler({**body, 'scheduler_revision': preview['scheduler_revision']})
            self.assertEqual(409, raised.exception.status)
            idle.assert_not_called()
            self.service.bind_deployment_scheduler({**body, 'scheduler_revision': result['scheduler_revision']})
            idle.assert_not_called()  # Saving the already matched identity changes nothing.

    def test_busy_binding_keeps_original_configuration(self):
        original = self.scheduler_path.read_bytes()
        body = self.stp_body()
        preview = self.service.preview_deployment(body)
        with patch.object(self.service.management, 'require_plugin_idle',
                          side_effect=ManagementError('unfinished run exists', 409)) as idle:
            with self.assertRaises(ManagementError) as raised:
                self.service.bind_deployment_scheduler({**body, 'scheduler_revision': preview['scheduler_revision']})
            self.assertEqual(409, raised.exception.status)
            idle.assert_called_once_with('stpcompare')
        self.assertEqual(original, self.scheduler_path.read_bytes())
        self.assertFalse(self.store.path.exists())

    def test_batch_binding_derives_identity_from_each_merged_configuration(self):
        base = self.coverage_body()
        base['config_patch']['plugins']['coverage']['dll'] = 'coverage.dll'
        self.store.put_desired('worker-http', {'revision': None, **base})
        request = {'client_ids': ['worker-http'], 'components': [],
                   'config_patch': {'plugins': {'coverage': {'store_cov': True}}}}
        preview = self.service.fleet.preview(request)
        with patch.object(self.service.management, 'require_plugin_idle'):
            saved = self.service.bind_deployment_scheduler({**request, 'scheduler_revision': preview['scheduler_revision']})
        self.assertEqual(preview['scheduler_plugins'], saved['scheduler_plugins'])
        self.assertNotEqual(self.coverage_version(base), saved['scheduler_plugins']['coverage']['expected_program_version'])
        self.assertFalse(self.store.get('worker-http')['desired']['config_patch']['plugins']['coverage']['store_cov'])

    def test_invalid_batch_binding_returns_validation_error(self):
        status, _ = self.service.deployment_operation('/api/deployments/bind-scheduler',
            {'client_ids': [], 'components': [], 'config_patch': {}})
        self.assertEqual(400, status)

    def test_http_preview_bind_and_desired_share_canonical_identity(self):
        handler = type('ProgramBindingHandler', (Handler,), {
            'service': self.service, 'log_message': lambda *args: None})
        server = ResultHTTPServer(('127.0.0.1', 0), handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()

        def request(path, method, body):
            req = Request(f'http://127.0.0.1:{server.server_port}' + path, method=method,
                          data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
            try:
                response = urlopen(req, timeout=5)
            except HTTPError as error:
                response = error
            with response:
                return response.status, json.load(response)

        try:
            body = self.stp_body()
            status, preview = request('/api/deployments/preview', 'POST', body)
            self.assertEqual(200, status, preview)
            with patch.object(self.service.management, 'require_plugin_idle') as idle:
                status, bound = request('/api/deployments/bind-scheduler', 'POST', {
                    **body, 'scheduler_revision': preview['scheduler_revision']})
                self.assertEqual(200, status, bound)
                idle.assert_called_once_with('stpcompare')
                status, stale = request('/api/deployments/bind-scheduler', 'POST', {
                    **body, 'scheduler_revision': preview['scheduler_revision']})
                self.assertEqual(409, status, stale)
            with patch.object(self.service.management, 'client_control', return_value={'ok': True}):
                status, published = request('/api/deployments/worker-http/desired', 'PUT', {
                    'revision': None, **body})
            self.assertEqual(200, status, published)
            desired = published['desired']
            self.assertEqual(preview['config_patch'], desired['config_patch'])
            self.assertEqual(bound['scheduler_plugins'], desired['scheduler_plugins'])
            disk = json.loads(self.scheduler_path.read_text(encoding='utf-8'))['plugins']['stpcompare']
            for field, version in bound['scheduler_plugins']['stpcompare'].items():
                self.assertEqual(version, disk[field])
        finally:
            server.shutdown()
            worker.join(5)
            server.server_close()


if __name__ == '__main__':
    unittest.main()
