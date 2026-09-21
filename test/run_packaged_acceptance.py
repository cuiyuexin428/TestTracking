"""Build and exercise relocatable Release packages, retaining all evidence.

The native workload is a compiled fixture, not a STEP geometry converter.
OpenCppCoverage, PostgreSQL, scheduler, clients and Python reports are real.
Run with --keep-running to leave the owned environment available for browser QA.
Nothing connects to an existing database or replaces an existing output directory.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'test'))
from test_coverage_settle import PrivatePostgres, sql_text
from test_runtime_integration import CapturedProcess, unused_port


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding='utf-8')


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


class Acceptance:
    def __init__(self, options):
        self.options = options
        self.root = options.out.resolve()
        existing = options.reuse or options.serve_existing
        if existing and not (self.root / '.packaged-acceptance').is_file():
            raise RuntimeError('Reuse requires the marker of an owned acceptance environment')
        if not existing and self.root.exists() and any(self.root.iterdir()):
            raise RuntimeError(f'Refusing to overwrite nonempty acceptance directory: {self.root}')
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / '.packaged-acceptance').touch()
        self.logs = self.root / 'evidence'
        self.logs.mkdir(exist_ok=True)
        if existing:
            self.logs = self.logs / ('round-' + str(time.time_ns()))
            self.logs.mkdir()
        self.server = self.root / 'server'
        self.clients = [self.root / f'client-{i:02}' for i in (1, 2)]
        self.inputs = self.root / 'inputs'
        self.inputs.mkdir(exist_ok=True)
        self.endpoint = f'tcp://127.0.0.1:{unused_port()}'
        self.management = f'http://127.0.0.1:{unused_port()}'
        self.port = unused_port()
        self.base = f'http://127.0.0.1:{self.port}'
        self.opener = build_opener(ProxyHandler({}))
        self.processes = {}
        self.generations = {}
        self.evidence = []
        self.pg = None
        self.runs = []
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(('RESULT_SERVICE_', 'PG'))}
        # Package processes must find dependencies beside themselves.
        self.env['PATH'] = os.pathsep.join(p for p in self.env.get('PATH', '').split(os.pathsep)
                                          if str(ROOT).lower() not in p.lower())

    def note(self, label, **details):
        print(label, flush=True)
        self.evidence.append({'check': label, **details})
        save(self.logs / 'acceptance.json', self.evidence)

    def shell(self, label, script, *args):
        with (self.logs / (label + '.log')).open('wb') as log:
            result = subprocess.run([
                shutil.which('powershell'), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', str(ROOT / 'tools' / script), *map(str, args)],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=600,
                creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode:
            raise RuntimeError(f'{label} failed; see {self.logs / (label + ".log")}')

    def prepare(self):
        if self.options.reuse or self.options.serve_existing:
            self.reuse()
            return
        dataset = self.inputs / 'dataset'
        program = self.inputs / 'program'
        dataset.mkdir()
        program.mkdir()
        for index in range(16):
            (dataset / f'part-{index:02}.stp').write_text(
                'ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n' + str(index) * (index + 1)
                + '\nENDSEC;\nEND-ISO-10303-21;\n', encoding='utf-8')
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
                   '-PackageVersion', 'acceptance-multiplugin',
                   '-ServerPort', self.endpoint.rsplit(':', 1)[1],
                   '-ManagementPort', self.management.rsplit(':', 1)[1])
        self.pg = PrivatePostgres(self.server / 'postgres/bin', self.server / 'bin/scheduler.exe')
        self.pg.root.rmdir()
        self.pg.root = self.root / 'database'
        self.pg.root.mkdir()
        self.pg.data = self.pg.root / 'pgdata'
        self.pg.start()
        cfg = read(self.server / 'config/scheduler.json')
        cfg['server'].update(db_conn=self.pg.conn, client_timeout_ms=1500, poll_timeout_ms=10)
        for name, plugin in cfg['plugins'].items():
            if name in {'coverage', 'stpcompare'}:
                plugin.update(enabled=True, db_conn=self.pg.conn, task_size=1)
        save(self.server / 'config/scheduler.json', cfg)
        for index, client in enumerate(self.clients, 1):
            self.shell(f'build-client-{index}', 'build_package.ps1',
                       '-OutDir', client, '-BinDir', self.options.bin_dir, '-Plugin', 'all',
                       '-Endpoint', self.endpoint, '-DeploymentEndpoint', self.base,
                       '-WorkerId', f'acceptance-{index:02}', '-DbConn', self.pg.conn,
                       '-TaskThreads', 1, '-StepTestExe', program / 'acceptance_program.exe',
                       '-BaselineConverterExe', program / 'acceptance_program.exe',
                       '-CandidateConverterExe', program / 'acceptance_program.exe',
                       '-EngineDlls', 'acceptance_engine.dll', '-DatasetDir', dataset,
                       '-OpenCppCoverageDir', self.options.coverage_dir,
                       '-ProgramVersion', 'acceptance-v1', '-BaselineProgramVersion', 'acceptance-v1',
                       '-CandidateProgramVersion', 'acceptance-v1', '-DatasetVersion', 'acceptance-d1')
            cfg = read(client / 'config/client.json')
            cfg['client'].update(heartbeat_interval_ms=200, deployment_poll_seconds=1,
                                 request_backoff_initial_ms=100, request_backoff_max_ms=500)
            save(client / 'config/client.json', cfg)
        service = read(self.server / 'config/result_service.json')
        service.update(host='0.0.0.0', port=self.port, db_conn=self.pg.conn)
        dsh = shutil.which('dsh.cmd') or shutil.which('dsh')
        if dsh:
            service['providers'] = {'dsh': {'command': dsh}}
        save(self.server / 'config/result_service.json', service)
        shutil.copy2(self.clients[0] / 'config/client.json', self.server / 'config/client.template.json')
        manifest_path = self.server / 'release-manifest.json'
        manifest = read(manifest_path)
        # Rebuild hash inventory after the acceptance environment configuration.
        for entry in manifest.get('files', []):
            path = self.server / entry['path']
            entry['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
            if 'size' in entry:
                entry['size'] = path.stat().st_size
        save(manifest_path, manifest)
        self.note('Release server and two multi-plugin client packages built',
                  server=str(self.server), clients=list(map(str, self.clients)), dry_run=False)

    def reuse(self):
        """Reuse packages with a fresh owned database schema, retaining old evidence."""
        import re
        if self.options.reuse and any((client / 'deployments/active.json').exists() for client in self.clients):
            raise RuntimeError('Use --serve-existing after managed deployment; --reuse is only for initial acceptance retries')
        cfg = read(self.server / 'config/scheduler.json')
        self.endpoint = cfg['server']['router_endpoint'].replace('*', '127.0.0.1')
        self.management = cfg['server']['management_endpoint']
        service = read(self.server / 'config/result_service.json')
        self.port = service['port']
        self.base = f'http://127.0.0.1:{self.port}'
        self.pg = PrivatePostgres(self.server / 'postgres/bin', self.server / 'bin/scheduler.exe')
        self.pg.root.rmdir()
        self.pg.root = self.root / 'database'
        self.pg.data = self.pg.root / 'pgdata'
        self.pg.port = int(re.search(r'port = (\d+)', (self.pg.data / 'postgresql.auto.conf').read_text())[1])
        self.pg.conn = f'host=127.0.0.1 port={self.pg.port} user=coverage_test dbname=postgres connect_timeout=5'
        if (self.pg.data / 'postmaster.pid').exists():
            raise RuntimeError('Stop the previous acceptance supervisor before reusing its packages')
        self.pg.run([self.pg.program('pg_ctl'), '-D', self.pg.data, '-l', self.pg.root / 'postgres.log',
                     '-w', '-t', '30', 'start'], output_file=self.pg.root / 'pg_ctl-restart.log')
        self.pg.started = True
        assert Path(self.pg.sql('SHOW data_directory')).resolve() == self.pg.data.resolve()
        (self.root / 'stop.request').unlink(missing_ok=True)
        if self.options.serve_existing:
            self.pg.conn = cfg['server']['db_conn']
            self.runs = read(self.root / 'environment.json').get('runs', [])
            self.note('Started retained acceptance database and packages')
            return
        schema = 'acceptance_' + str(time.time_ns())
        self.pg.sql('CREATE SCHEMA ' + schema)
        self.pg.conn += f" options='-csearch_path={schema}'"
        cfg['server']['db_conn'] = self.pg.conn
        for name in ('coverage', 'stpcompare'):
            cfg['plugins'][name]['db_conn'] = self.pg.conn
        save(self.server / 'config/scheduler.json', cfg)
        for client in self.clients:
            config = read(client / 'config/client.json')
            for plugin in config['plugins'].values():
                plugin['db_conn'] = self.pg.conn
            save(client / 'config/client.json', config)
        service['db_conn'] = self.pg.conn
        save(self.server / 'config/result_service.json', service)
        shutil.copy2(self.clients[0] / 'config/client.json', self.server / 'config/client.template.json')
        for directory in ('app', 'web'):
            shutil.copytree(ROOT / 'result_service' / directory,
                            self.server / 'result_service' / directory, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns('__pycache__'))
        self.note('Reused owned packages with fresh database schema and current application resources', schema=schema)

    def start(self, name):
        generation = self.generations.get(name, 0) + 1
        self.generations[name] = generation
        if name == 'service':
            output = (self.logs / f'service-{generation}.log').open('wb')
            process = subprocess.Popen([
                str(self.server / 'runtime/python/python.exe'), '-B',
                str(self.server / 'result_service/launcher.py'), '--config',
                str(self.server / 'config/result_service.json')], cwd=self.server, env=self.env,
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            self.processes[name] = (process, output)
        else:
            directory = self.server if name == 'scheduler' else self.clients[int(name[-1]) - 1]
            executable = directory / ('bin/scheduler.exe' if name == 'scheduler' else 'client.exe')
            config = directory / ('config/scheduler.json' if name == 'scheduler' else 'config/client.json')
            self.processes[name] = CapturedProcess(executable, config, directory,
                                                  f'{name}-{time.time_ns()}-{generation}', self.env)

    def stop(self, name, crash=False):
        process = self.processes.pop(name, None)
        if process is None:
            return
        if name == 'service':
            child, output = process
            if child.poll() is None:
                # Python terminate() on Windows skips ResultService.close().
                # Stop only this owned process tree, including its DSH children.
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'],
                               capture_output=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
                child.wait(15)
            output.close()
        elif crash:
            process.crash()
        else:
            process.close()

    def request(self, path, body=None, method=None, expected=200):
        req = Request(self.base + path,
                      data=None if body is None else json.dumps(body).encode('utf-8'),
                      headers={'Content-Type': 'application/json'}, method=method)
        try:
            response = self.opener.open(req, timeout=60)
        except HTTPError as error:
            response = error
        with response:
            data = json.load(response)
            if expected is not None and response.status != expected:
                raise AssertionError(f'{path}: expected {expected}, got {response.status}: {data}')
            return (response.status, data) if expected is None else data

    def wait(self, predicate, label, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                result = predicate()
                if result:
                    return result
            except (OSError, HTTPError):
                pass
            time.sleep(.2)
        raise AssertionError(f'Timed out: {label}; evidence: {self.logs}')

    def run(self, plugin='coverage', force=False):
        body = {'action': 'run', 'plugin_type': plugin, 'dataset_version': 'acceptance-d1'}
        if force:
            body.update(force_rerun=True, force_rerun_reason='packaged acceptance recovery test')
        result = self.request('/api/scheduler/commands', body)
        self.runs.append(result['run_id'])
        return result

    def finished(self, run):
        return self.pg.sql('SELECT status FROM runs WHERE run_id=' + sql_text(run)) == 'finished'

    def verify(self):
        for name in ('scheduler', 'service', 'client1', 'client2'):
            self.start(name)
        self.wait(lambda: len(self.request('/api/scheduler').get('clients', [])) == 4,
                  'two physical clients / four plugin workers')
        state = self.request('/api/scheduler')
        save(self.logs / 'workers.json', state)
        assert {row['physical_client_id'] for row in state['clients']} == {'acceptance-01', 'acceptance-02'}
        assert {row['plugin_key'] for row in state['clients']} == {'coverage', 'stpcompare'}
        self.note('Two physical clients register four isolated plugin workers')
        body = {'action': 'run', 'plugin_type': 'coverage', 'dataset_version': 'acceptance-d1'}
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: self.request('/api/scheduler/commands', body, expected=None), range(2)))
        assert sorted(code for code, _ in results) == [200, 409], results
        first = next(value for code, value in results if code == 200)['run_id']
        self.runs.append(first)
        queued = self.run(force=True)
        assert queued['status'] == 'queued', queued
        stp = self.run('stpcompare')['run_id']
        self.note('Concurrent users deduplicate identical runs; forced repeat queues; other plugin runs in parallel',
                  runs=list(self.runs))
        self.wait(lambda: self.pg.sql("SELECT count(*) FROM tasks WHERE status='acked'") != '0',
                  'active task before scheduler crash')
        self.stop('scheduler', crash=True)
        self.start('scheduler')
        self.wait(lambda: self.request('/api/scheduler').get('online'), 'scheduler restart')
        self.stop('client1', crash=True)
        self.start('client1')
        for run in self.runs:
            self.wait(lambda run=run: self.finished(run), 'durable finish ' + run, timeout=180)
        self.note('Scheduler crash and whole-client crash recover; persisted queue drains', runs=list(self.runs))
        for run in self.runs:
            count = self.pg.sql("SELECT count(*) FROM plugin_file_outcomes WHERE status='success' AND run_id=" + sql_text(run))
            assert count == '16', (run, count)
        assert self.pg.sql('SELECT count(*) FROM stp_compare_old WHERE run_id=' + sql_text(stp)) == '64'
        assert self.pg.sql('SELECT count(*) FROM coverage_file_points WHERE octet_length(points)>0 AND run_id=' + sql_text(first)) == '16'
        assert self.pg.sql('SELECT count(*) FROM plugin_run_files WHERE size_bytes>0 AND run_id=' + sql_text(first)) == '16'
        self.note('Actual OpenCppCoverage points and STP four-output contents persist for every input; file sizes recorded')
        code, duplicate = self.request('/api/scheduler/commands', body, expected=None)
        assert code == 409, duplicate
        self.note('Completed identical workload blocked without force')
        saved = self.request('/api/config/client')
        saved['config']['client']['task_threads'] = 2
        updated = self.request('/api/config/client', saved, 'PUT')
        self.request('/api/config/client', saved, 'PUT', expected=409)
        self.note('Concurrent configuration save uses revision check and rejects stale overwrite', revision=updated['revision'])
        script = self.request('/api/plugins/coverage/report-script')
        changed = {'source': script['source'] + '\n# Packaged acceptance edit\n',
                   'revision': script['revision'], 'operator_id': '123e4567-e89b-42d3-a456-426614174001'}
        self.request('/api/plugins/coverage/report-script', changed, 'PUT')
        self.request('/api/plugins/coverage/report-script', changed, 'PUT', expected=409)
        self.stop('scheduler')
        self.wait(lambda: self.request('/api/scheduler').get('online') is False, 'offline execution service')
        report = self.request('/api/plugins/coverage/reports', {'run_id': first})
        assert any((self.server / 'data/reports').rglob('index.html'))
        self.note('Replaceable analysis script CAS and database-only Python report work with scheduler stopped', report=report)
        self.start('scheduler')
        self.wait(lambda: self.request('/api/scheduler').get('online'), 'scheduler resumed')
        self.note('Packaged API acceptance passed', ui=self.base)

    def metadata(self):
        metadata = {'url': self.base, 'root': str(self.root), 'supervisor_pid': os.getpid(),
                    'runs': self.runs, 'postgres_port': self.pg.port if self.pg else None,
                    'control': 'Create stop.request here to stop owned services. control.json supports restart_scheduler, restart_service, restart_client1, restart_client2.',
                    'fixture_notice': 'Native executable fixture; no production geometry conversion is claimed.'}
        save(self.root / 'environment.json', metadata)

    def serve(self):
        self.metadata()
        print('READY ' + self.base, flush=True)
        while not (self.root / 'stop.request').exists():
            control = self.root / 'control.json'
            if control.exists():
                command = read(control)
                control.unlink()
                action = command.get('action', '')
                if action.startswith('restart_') and action[8:] in self.processes:
                    name = action[8:]
                    self.stop(name)
                    self.start(name)
            time.sleep(.3)

    def close(self):
        for name in list(reversed(self.processes)):
            self.stop(name)
        if self.pg and self.pg.started and self.pg.data.exists():
            self.pg.run([self.pg.program('pg_ctl'), '-D', self.pg.data, '-m', 'fast',
                         '-w', '-t', '30', 'stop'], check=False, timeout=45)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, default=ROOT / 'build/offline_verify/bin/Release')
    parser.add_argument('--out', type=Path, default=ROOT / 'real-test')
    parser.add_argument('--coverage-dir', type=Path, default=Path('C:/Program Files/OpenCppCoverage'))
    parser.add_argument('--keep-running', action='store_true')
    parser.add_argument('--reuse', action='store_true', help='Reuse owned packages with a new isolated DB schema')
    parser.add_argument('--serve-existing', action='store_true', help='Start retained packages and data without rerunning tests')
    options = parser.parse_args()
    test = Acceptance(options)
    try:
        test.prepare()
        test.metadata()
        if options.serve_existing:
            for name in ('scheduler', 'service', 'client1', 'client2'):
                test.start(name)
            test.wait(lambda: test.request('/healthz'), 'result service startup')
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
