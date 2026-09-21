"""Exercise artifact upload, two-plugin deployment and rollback in a running
run_packaged_acceptance.py environment. The root must have its ownership marker.
"""
import argparse
import io
import json
from pathlib import Path
import time
from urllib.parse import urlencode
from urllib.request import Request, build_opener, ProxyHandler
from urllib.error import HTTPError
import zipfile


def require_owned_service(root, request):
    """Confirm the live processes belong to this package before any mutation.

    environment.json may survive after its service stops and another process
    binds the same port. A local ownership marker alone cannot identify that
    process, so check both the result service's effective paths and the running
    scheduler's own configuration path.
    """
    server = root.resolve() / 'server'

    def require_path(value, expected, label):
        if not isinstance(value, str) or not value or Path(value).resolve() != expected.resolve():
            raise RuntimeError(f'Refusing deployment: {label} does not belong to {server}')

    result = request('/api/config/result_service')
    if not isinstance(result, dict) or result.get('available') is not True:
        raise RuntimeError('Refusing deployment: result service configuration is unavailable')
    require_path(result.get('path'), server / 'config/result_service.json', 'result service configuration')
    effective = result.get('effective_values')
    if not isinstance(effective, dict):
        raise RuntimeError('Refusing deployment: effective result service configuration is unavailable')
    for field, expected in (
        ('config_path', server / 'config/result_service.json'),
        ('workspace', server),
        ('artifact_store_dir', server / 'data/artifacts'),
        ('scheduler_config_path', server / 'config/scheduler.json'),
    ):
        require_path(effective.get(field), expected, 'effective ' + field)

    scheduler_config = request('/api/config/scheduler')
    if not isinstance(scheduler_config, dict) or scheduler_config.get('available') is not True:
        raise RuntimeError('Refusing deployment: scheduler configuration is unavailable')
    require_path(scheduler_config.get('path'), server / 'config/scheduler.json', 'scheduler configuration')
    scheduler = request('/api/scheduler')
    if not isinstance(scheduler, dict) or scheduler.get('online') is not True:
        raise RuntimeError('Refusing deployment: the owned scheduler is not online')
    require_path(scheduler.get('config_path'), server / 'config/scheduler.json', 'running scheduler configuration')


def verify(root):
    root = root.resolve()
    if not (root / '.packaged-acceptance').is_file():
        raise RuntimeError('Expected an owned packaged acceptance environment')
    base = json.loads((root / 'environment.json').read_text())['url']
    opener = build_opener(ProxyHandler({}))
    evidence = []

    def request(path, body=None, method=None, expected=200):
        req = Request(base + path, data=None if body is None else json.dumps(body).encode(),
                      method=method, headers={'Content-Type': 'application/json'})
        try:
            response = opener.open(req, timeout=30)
        except HTTPError as error:
            response = error
        with response:
            value = json.load(response)
            assert response.status == expected, (response.status, value)
            return value

    # No upload, deployment publication or evidence file replacement occurs
    # until the URL has been matched to this exact owned package.
    require_owned_service(root, request)
    evidence.append('Live result service and scheduler paths match the owned acceptance package')

    def wait(predicate, label):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if predicate():
                evidence.append(label)
                print(label, flush=True)
                return
            time.sleep(.2)
        raise AssertionError('Timed out: ' + label)

    bundles = root / 'inputs/bundles'
    bundles.mkdir(exist_ok=True)

    def upload(kind, name, files, version='acceptance-v1'):
        existing = next((item for item in request('/api/artifacts')['artifacts']
                         if (item['kind'], item['name'], item['version']) == (kind, name, version)), None)
        if existing:
            with opener.open(base + existing['download_url'], timeout=30) as response:
                contents = response.read()
            with zipfile.ZipFile(io.BytesIO(contents)) as archive:
                assert {key: archive.read(key) for key in archive.namelist()} == files
            (bundles / (name + '-' + version + '.zip')).write_bytes(contents)
            return existing
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for filename, content in files.items():
                archive.writestr(filename, content)
        contents = stream.getvalue()
        (bundles / (name + '-' + version + '.zip')).write_bytes(contents)
        req = Request(base + '/api/artifacts/upload?' + urlencode({
            'kind': kind, 'name': name, 'version': version, 'filename': name + '.zip'}),
            data=contents, headers={'Content-Type': 'application/octet-stream'})
        with opener.open(req, timeout=60) as response:
            return json.load(response)

    client = root / 'client-01'
    plugins = {key: upload('plugin', key, {key + '.dll': (client / (key + '.dll')).read_bytes()})
               for key in ('coverage', 'stpcompare')}
    data_root = client / 'testenv/dataset'
    dataset = upload('dataset', 'acceptance-stp', {
        path.relative_to(data_root).as_posix(): path.read_bytes() for path in data_root.rglob('*') if path.is_file()},
        'acceptance-d1')
    # Leave real program artifacts in the UI repository for manual wizard QA.
    for name, folder in [('coverage-program', 'coverage'), ('baseline', 'baseline'), ('candidate', 'candidate')]:
        directory = client / 'testenv/program' / folder
        files = {path.relative_to(directory).as_posix(): path.read_bytes()
                 for path in directory.rglob('*') if path.is_file()}
        if name == 'coverage-program':
            occ = client / 'testenv/opencppcoverage'
            files.update({'opencppcoverage/' + path.relative_to(occ).as_posix(): path.read_bytes()
                          for path in occ.rglob('*') if path.is_file()})
        upload('program', name, files)
    path = '/api/deployments/acceptance-01'
    current = request(path)
    plan = {'revision': current.get('desired', {}).get('revision') if current.get('desired') else None,
            'components': [{'slot': 'plugin.' + key, 'artifact_id': item['artifact_id']}
                           for key, item in plugins.items()] + [{'slot': 'dataset', 'artifact_id': dataset['artifact_id']}],
            'config_patch': {'plugins': {key: {'enabled': True, 'dll': '{artifact:plugin.' + key + '}/' + key + '.dll',
                                             'stp_root': '{artifact:dataset}'} for key in plugins}},
            'operator_id': '123e4567-e89b-42d3-a456-426614174001',
            'reason': 'packaged multi-plugin acceptance'}
    desired = request(path + '/desired', plan, 'PUT')['desired']['revision']
    wait(lambda: request(path)['state'] == 'current', 'Two native plugins and full dataset download and activate')
    assert json.loads((client / 'deployments/active.json').read_text())['revision'] == desired
    conflict = json.loads(json.dumps(plan))
    conflict['revision'] = None
    conflict['config_patch']['client'] = {'task_threads': 2}
    request(path + '/desired', conflict, 'PUT', 409)
    evidence.append('A second operator with a stale revision cannot overwrite deployment')
    bad = upload('plugin', 'stpcompare-invalid', {'stpcompare.dll': b'explicit invalid DLL test fixture'})
    broken = json.loads(json.dumps(plan))
    broken['revision'] = desired
    broken['components'][1]['artifact_id'] = bad['artifact_id']
    failed_revision = request(path + '/desired', broken, 'PUT')['desired']['revision']
    wait(lambda: request(path)['state'] == 'failed', 'Invalid second plugin fails activation')
    assert json.loads((client / 'deployments/active.json').read_text())['revision'] == desired
    wait(lambda: len([row for row in request('/api/scheduler').get('clients', [])
                      if row.get('physical_client_id') == 'acceptance-01']) == 2,
         'Both old plugin workers recover after failed activation')
    rollback = request(path + '/desired', {'revision': failed_revision, 'action': 'rollback',
                                         'target_revision': desired, 'reason': 'return to verified release'}, 'PUT')
    wait(lambda: request(path)['state'] == 'current', 'Explicit rollback becomes the current desired release')
    (root / 'evidence/packaged-deployment.json').write_text(json.dumps({
        'checks': evidence, 'desired': rollback['desired']['revision'], 'bundles': str(bundles)}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    verify(parser.parse_args().root)
