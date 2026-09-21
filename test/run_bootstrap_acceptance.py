"""Exercise real packages from two empty Agents to distributed plugin execution.

Uses a new owned PostgreSQL cluster and real Release binaries, OpenCppCoverage,
HTTP artifact uploads, deployment publication and process activation. The native
workload is the compiled acceptance fixture, not production STEP conversion.
Use --keep-running for browser QA; create stop.request to stop only this run.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
from urllib.parse import urlencode
from urllib.request import Request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_packaged_acceptance import Acceptance, PrivatePostgres, ROOT, read, save, sql_text
from verify_packaged_deployment import require_owned_service


class BootstrapAcceptance(Acceptance):
    client_ids = ['bootstrap-01', 'bootstrap-02']

    def prepare(self):
        if self.options.serve_existing or self.options.resume:
            # Retain the exact cluster and its failed deployment evidence. A
            # retry publishes a new revision through the ordinary HTTP API.
            was_reuse, was_serve = self.options.reuse, self.options.serve_existing
            self.options.reuse, self.options.serve_existing = False, True
            self.reuse()
            self.options.reuse, self.options.serve_existing = was_reuse, was_serve
            if self.options.resume:
                shutil.copy2(self.options.bin_dir / 'scheduler.exe', self.server / 'bin/scheduler.exe')
                for client in self.clients:
                    shutil.copy2(ROOT / 'tools/client_update.ps1', client / 'tools/client_update.ps1')
                for directory in ('app', 'web'):
                    shutil.copytree(ROOT / 'result_service' / directory,
                                    self.server / 'result_service' / directory, dirs_exist_ok=True,
                                    ignore=shutil.ignore_patterns('__pycache__'))
                self.note('Resumed owned database and packages with current scheduler, helper and UI; no database reset',
                          scheduler_sha256=hashlib.sha256((self.server / 'bin/scheduler.exe').read_bytes()).hexdigest())
            return
        dataset, program = self.inputs / 'dataset', self.inputs / 'program'
        dataset.mkdir()
        program.mkdir()
        for index in range(16):
            (dataset / f'part-{index:02}.stp').write_text(
                'ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n' + str(index) * (index + 1)
                + '\nENDSEC;\nEND-ISO-10303-21;\n', encoding='utf-8')
        (dataset / 'version.txt').write_text('acceptance-d1\n', encoding='utf-8')
        for filename in ('acceptance_program.exe', 'acceptance_program.pdb',
                         'acceptance_engine.dll', 'acceptance_engine.pdb'):
            shutil.copy2(self.options.bin_dir / filename, program / filename)
        self.shell('build-server', 'build_server_package.ps1',
                   '-OutDir', self.server, '-BinDir', self.options.bin_dir,
                   '-PythonRuntimeDir', ROOT / 'runtime/python',
                   '-IncludePostgresRuntime', '-PgBinDir', ROOT / 'third_party/pgsql/bin',
                   '-DatasetDir', dataset, '-DatasetVersion', 'acceptance-d1',
                   '-ProgramVersion', 'acceptance-v1', '-BaselineProgramVersion', 'acceptance-v1',
                   '-CandidateProgramVersion', 'acceptance-v1', '-StpResultTable', 'stp_compare_old',
                   '-PackageVersion', 'acceptance-bootstrap',
                   '-ServerPort', self.endpoint.rsplit(':', 1)[1],
                   '-ManagementPort', self.management.rsplit(':', 1)[1])
        self.pg = PrivatePostgres(self.server / 'postgres/bin', self.server / 'bin/scheduler.exe')
        self.pg.root.rmdir()
        self.pg.root = self.root / 'database'
        self.pg.root.mkdir()
        self.pg.data = self.pg.root / 'pgdata'
        self.pg.start()
        config = read(self.server / 'config/scheduler.json')
        config['server'].update(db_conn=self.pg.conn, client_timeout_ms=1500,
                                agent_timeout_ms=15000, poll_timeout_ms=10)
        for key in ('coverage', 'stpcompare'):
            config['plugins'][key].update(enabled=True, db_conn=self.pg.conn, task_size=1)
        save(self.server / 'config/scheduler.json', config)
        for index, client in enumerate(self.clients):
            self.shell(f'build-bootstrap-{index + 1}', 'build_package.ps1',
                       '-Bootstrap', '-OutDir', client, '-BinDir', self.options.bin_dir,
                       '-Endpoint', self.endpoint, '-DeploymentEndpoint', self.base,
                       '-WorkerId', self.client_ids[index], '-TaskThreads', 1)
            config = read(client / 'config/client.json')
            assert config['plugins'] == {}
            assert not (client / 'coverage.dll').exists() and not (client / 'stpcompare.dll').exists()
            assert not (client / 'testenv').exists()
            config['client'].update(heartbeat_interval_ms=200, deployment_poll_seconds=1,
                                    request_backoff_initial_ms=100, request_backoff_max_ms=500)
            save(client / 'config/client.json', config)
        service = read(self.server / 'config/result_service.json')
        service.update(host='0.0.0.0', port=self.port, db_conn=self.pg.conn)
        dsh = shutil.which('dsh.cmd') or shutil.which('dsh')
        if dsh:
            service['providers'] = {'dsh': {'command': dsh}}
        save(self.server / 'config/result_service.json', service)
        template = read(self.server / 'config/client.template.json')
        for key in ('coverage', 'stpcompare'):
            template['plugins'][key].update(enabled=False, db_conn=self.pg.conn, dry_run=False)
        save(self.server / 'config/client.template.json', template)
        manifest_path = self.server / 'release-manifest.json'
        manifest = read(manifest_path)
        for entry in manifest.get('files', []):
            path = self.server / entry['path']
            entry['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
            if 'size' in entry:
                entry['size'] = path.stat().st_size
        save(manifest_path, manifest)
        self.note('Built independent server and two genuinely empty Bootstrap packages',
                  server=str(self.server), clients=list(map(str, self.clients)),
                  binary_sha256={name: hashlib.sha256((self.options.bin_dir / name).read_bytes()).hexdigest()
                                 for name in ('scheduler.exe', 'client.exe', 'coverage.dll', 'stpcompare.dll')})

    def upload(self, kind, name, files, version='acceptance-v1'):
        existing = next((item for item in self.request('/api/artifacts')['artifacts']
                         if (item['kind'], item['name'], item['version']) == (kind, name, version)), None)
        if existing:
            with self.opener.open(self.base + existing['download_url'], timeout=60) as response:
                contents = response.read()
            with zipfile.ZipFile(io.BytesIO(contents)) as archive:
                assert {key: archive.read(key) for key in archive.namelist()} == files
            return existing
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for path, content in sorted(files.items()):
                archive.writestr(path, content)
        contents = stream.getvalue()
        bundles = self.inputs / 'bundles'
        bundles.mkdir(exist_ok=True)
        (bundles / (name + '.zip')).write_bytes(contents)
        request = Request(self.base + '/api/artifacts/upload?' + urlencode({
            'kind': kind, 'name': name, 'version': version, 'filename': name + '.zip'}),
            data=contents, headers={'Content-Type': 'application/octet-stream'})
        with self.opener.open(request, timeout=90) as response:
            artifact = json.load(response)
        assert artifact['sha256'] == hashlib.sha256(contents).hexdigest()
        return artifact

    @staticmethod
    def files(directory):
        # OpenCppCoverage requires Plugins/Exporter even when it is empty.
        # ZIP deployment must preserve the complete supplied directory tree.
        return {path.relative_to(directory).as_posix() + ('/' if path.is_dir() else ''):
                    b'' if path.is_dir() else path.read_bytes()
                for path in directory.rglob('*') if path.is_file() or path.is_dir()}

    def upload_plan(self):
        artifacts = {key: self.upload('plugin', key, {key + '.dll': (self.options.bin_dir / (key + '.dll')).read_bytes()})
                     for key in ('coverage', 'stpcompare')}
        artifacts['dataset'] = self.upload('dataset', 'bootstrap-stp', self.files(self.inputs / 'dataset'), 'acceptance-d1')
        files = self.files(self.inputs / 'program')
        for key in ('baseline', 'candidate'):
            artifacts[key] = self.upload('program', key, files)
        coverage_files = {**files, **{'opencppcoverage/' + key: content
                                    for key, content in self.files(self.options.coverage_dir).items()}}
        artifacts['coverage-program'] = self.upload('program', 'coverage-program', coverage_files, 'acceptance-v2')
        reference = lambda slot, path='': '{artifact:' + slot + '}' + ('/' + path if path else '')
        coverage = dict(enabled=True, dll=reference('coverage', 'coverage.dll'),
                        stp_root=reference('dataset'), program_dir=reference('coverage-program'),
                        target_exe=reference('coverage-program', 'acceptance_program.exe'),
                        engine_dlls=[reference('coverage-program', 'acceptance_engine.dll')],
                        opencppcoverage=reference('coverage-program', 'opencppcoverage/OpenCppCoverage.exe'),
                        db_conn=self.pg.conn, store_type='postgres', store_dir='store', export_dir='reports',
                        store_cov=False, dry_run=False)
        stp = dict(enabled=True, dll=reference('stpcompare', 'stpcompare.dll'), stp_root=reference('dataset'),
                   db_conn=self.pg.conn, store_type='postgres', store_dir='store', work_dir='stp_compare_work',
                   table='stp_compare_old', dry_run=False,
                   programs={role: {'executable': reference(role, 'acceptance_program.exe'),
                                    'arguments': ['--input', '{stp}', '--output-dir', '{outdir}']}
                             for role in ('baseline', 'candidate')})
        body = {'client_ids': self.client_ids,
                'components': [{'slot': slot, 'artifact_id': artifact['artifact_id']} for slot, artifact in artifacts.items()],
                'config_patch': {'plugins': {'coverage': coverage, 'stpcompare': stp}},
                'operator_id': '123e4567-e89b-42d3-a456-426614174001',
                'reason': 'real Bootstrap client deployment acceptance'}
        save(self.logs / 'uploaded-artifacts.json', artifacts)
        self.note('Uploaded plugin DLLs, both converter programs, full dataset and real OpenCppCoverage',
                  artifacts={key: item['artifact_id'] for key, item in artifacts.items()})
        return body

    def preview(self, body):
        return self.request('/api/deployments/batch/preview', body)

    def publication(self, body, preview):
        return {**copy.deepcopy(body), 'targets': [
            {'client_id': row['client_id'], 'revision': row['revision']} for row in preview['clients']]}

    def publish(self, body):
        return self.request('/api/deployments/batch', self.publication(body, self.preview(body)))

    def agents(self):
        return self.request('/api/agents')['agents']

    def current(self):
        states = [self.request('/api/deployments/' + client) for client in self.client_ids]
        failures = {row['client_id']: (row.get('reported') or {}).get('message')
                    for row in states if row['state'] == 'failed'}
        if failures:
            save(self.logs / 'failed-deployments.json', states)
            raise AssertionError('Deployment failed: ' + json.dumps(failures, ensure_ascii=False))
        return all(row['state'] == 'current' for row in states)

    def configurable(self):
        rows = {row['client_id']: row for row in self.agents()}
        return all(rows.get(client, {}).get('configurable') for client in self.client_ids)

    def verify(self):
        for name in ('scheduler', 'service', 'client1', 'client2'):
            self.start(name)
        self.wait(lambda: self.request('/healthz'), 'packaged result service')
        require_owned_service(self.root, self.request)
        if self.options.resume:
            self.note('Earlier attempt retained for diagnosis', previous_runs=self.runs)
            self.runs = []
            self.wait(lambda: not self.request('/api/scheduler')['active_runs'] and
                      not self.request('/api/scheduler')['queued_runs'], 'retained unfinished runs settle', timeout=180)
        self.wait(lambda: len(self.agents()) == 2 and self.configurable(), 'independent empty Agent discovery')
        if not self.options.resume:
            assert self.request('/api/scheduler')['clients'] == []
            assert all(row['state'] == 'unconfigured' and row['plugin_keys'] == [] for row in self.agents())
            save(self.logs / 'empty-agents.json', self.agents())
            self.note('Two empty Agents independently register without loading any plugin')
        body = self.upload_plan()
        preview = self.preview(body)
        assert len(preview['program_bindings']) == 3
        self.request('/api/deployments/bind-scheduler', {**body, 'scheduler_revision': preview['scheduler_revision']})
        self.stop('scheduler')
        self.start('scheduler')
        self.wait(self.configurable, 'Agents reconnect after program binding restart')
        preview = self.preview(body)
        publication = self.publication(body, preview)
        first = self.request('/api/deployments/batch', publication)
        self.wait(self.current, 'full artifact download and both plugins activated', timeout=120)
        self.wait(lambda: len(self.request('/api/scheduler')['clients']) == 4 and self.configurable()
                  and all(set(row['plugin_keys']) == {'coverage', 'stpcompare'} and row['state'] == 'ready'
                          for row in self.agents()),
                  'four plugin Workers registered and reservations released')
        for client, row in zip(self.clients, first['clients']):
            active = read(client / 'deployments/active.json')
            assert active['revision'] == row['desired']['revision']
            configuration = read(Path(active['config_path']))
            assert all(configuration['plugins'][key]['enabled'] for key in ('coverage', 'stpcompare'))
            for plugin in configuration['plugins'].values():
                for field in ('dll', 'stp_root'):
                    assert Path(plugin[field]).is_relative_to(client)
        save(self.logs / 'activated-agents.json', self.agents())
        self.note('Batch deployment activates two isolated plugin Workers on each formerly empty machine')
        stale = self.request('/api/deployments/batch', publication, expected=409)
        self.note('A second operator cannot overwrite a published deployment with stale revisions', conflict=stale)

        busy_body = {'client_ids': self.client_ids, 'components': [], 'config_patch': {'client': {'task_threads': 2}}}
        busy_publication = self.publication(busy_body, self.preview(busy_body))
        run_body = {'action': 'run', 'plugin_type': 'coverage', 'dataset_version': 'acceptance-d1'}
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: self.request('/api/scheduler/commands', run_body, expected=None), range(2)))
        if self.options.resume and all(code == 409 and value.get('duplicate') is True and
                                       value.get('status') == 'finished' for code, value in results):
            coverage_run = self.run(force=True)['run_id']
            self.note('Completed workload remains deduplicated on retry; explicitly forced a new acceptance run')
        else:
            assert sorted(code for code, _ in results) == [200, 409], results
            coverage_run = next(value for code, value in results if code == 200)['run_id']
            self.runs.append(coverage_run)
        queued = self.run(force=True)
        assert queued['status'] == 'queued'
        stp_run = self.run('stpcompare', force=self.options.resume)['run_id']
        self.metadata()
        self.wait(lambda: any(row['busy'] for row in self.agents()), 'busy machine before deployment submission')
        rejected = self.request('/api/deployments/batch', busy_publication, expected=409)
        assert any(row['reason'] == 'busy' for row in rejected['unavailable']), rejected
        assert all(not row.get('reservation_id') for row in self.agents())
        self.note('Publish rechecks physical-machine busy state after idle preview and rejects the whole batch', conflict=rejected)
        self.stop('scheduler', crash=True)
        self.start('scheduler')
        self.wait(lambda: self.request('/api/scheduler').get('online'), 'scheduler crash recovery')
        self.stop('client1', crash=True)
        self.start('client1')
        for run in self.runs:
            self.wait(lambda run=run: self.finished(run), 'all persisted tasks finished ' + run, timeout=180)
            count = self.pg.sql("SELECT count(*) FROM plugin_file_outcomes WHERE status='success' AND run_id=" + sql_text(run))
            assert count == '16', (run, 'expected 16 successful inputs', count)
        assert self.pg.sql('SELECT count(*) FROM stp_compare_old WHERE run_id=' + sql_text(stp_run)) == '64'
        assert self.pg.sql('SELECT count(*) FROM coverage_file_points WHERE octet_length(points)>0 AND run_id=' + sql_text(coverage_run)) == '16'
        assert self.pg.sql('SELECT count(*) FROM plugin_run_files WHERE size_bytes>0 AND run_id=' + sql_text(coverage_run)) == '16'
        self.note('Actual coverage and four STP info outputs persist after scheduler/client crashes; queued work completes', runs=self.runs)
        self.wait(self.configurable, 'idle Agents before disabling all plugins')
        disable = {'client_ids': self.client_ids, 'components': [],
                   'config_patch': {'plugins': {key: {'enabled': False} for key in ('coverage', 'stpcompare')}}}
        self.publish(disable)
        self.wait(lambda: self.current() and self.request('/api/scheduler')['clients'] == [] and self.configurable()
                  and all(row['plugin_keys'] == [] and row['state'] == 'unconfigured' for row in self.agents()),
                  'all plugins disabled, empty Agents still online', timeout=60)
        save(self.logs / 'disabled-agents.json', self.agents())
        self.note('Disabling every plugin leaves both physical Agents online and available for reconfiguration')
        enable = {'client_ids': self.client_ids, 'components': [],
                  'config_patch': {'plugins': {key: {'enabled': True} for key in ('coverage', 'stpcompare')}}}
        self.publish(enable)
        self.wait(lambda: self.current() and len(self.request('/api/scheduler')['clients']) == 4 and self.configurable()
                  and all(set(row['plugin_keys']) == {'coverage', 'stpcompare'} and row['state'] == 'ready'
                          for row in self.agents()),
                  'existing artifact references retained when re-enabling plugins', timeout=90)
        save(self.logs / 'final-agents.json', self.agents())
        self.note('Re-enabling plugins reuses retained resources and restores all four Workers', ui=self.base)
        self.note('Bootstrap packaged acceptance passed', ui=self.base, fixture_notice='Compiled native fixture, not production STEP conversion')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, default=ROOT / 'build/offline_verify/bin/Release')
    parser.add_argument('--out', type=Path, default=ROOT / 'real-test/bootstrap-round')
    parser.add_argument('--coverage-dir', type=Path, default=Path('C:/Program Files/OpenCppCoverage'))
    parser.add_argument('--keep-running', action='store_true')
    parser.add_argument('--serve-existing', action='store_true', help='Restart only a previously owned environment for inspection')
    parser.add_argument('--resume', action='store_true', help='Retry initial deployment after fixing a failed Bootstrap round; retain its private database')
    options = parser.parse_args()
    if options.resume and options.serve_existing:
        parser.error('--resume and --serve-existing are mutually exclusive')
    options.reuse = options.resume
    test = BootstrapAcceptance(options)
    try:
        test.prepare()
        test.metadata()
        if options.serve_existing:
            for name in ('scheduler', 'service', 'client1', 'client2'):
                test.start(name)
            test.wait(lambda: test.request('/healthz'), 'owned service startup')
        else:
            test.verify()
        test.metadata()
        if options.keep_running or options.serve_existing:
            test.serve()
    except Exception as error:
        test.note('FAILED', error=str(error))
        raise
    finally:
        test.close()


if __name__ == '__main__':
    main()
