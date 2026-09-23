'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ui = require('../result_service/web/profiles.js');

const listings = {
  dll: { dlls: ['lib/coverage.dll', 'other.dll'], executables: [], dataset_roots: [], files: [] },
  exe: { dlls: ['engine.dll'], executables: ['bin/convert.exe'], dataset_roots: [], files: [] },
  cov: { dlls: [], executables: ['OpenCppCoverage.exe', 'helper.exe'], dataset_roots: [], files: [] },
  data: { dlls: [], executables: [], dataset_roots: ['', 'parts'], directories: ['parts'], files: [] },
};
const artifacts = [
  { artifact_id: 'dll', kind: 'plugin', name: 'coverage-client', version: '1' },
  { artifact_id: 'exe', kind: 'program', name: 'converter', version: '2' },
  { artifact_id: 'cov', kind: 'program', name: 'OpenCppCoverage', version: '3' },
  { artifact_id: 'data', kind: 'dataset', name: 'parts', version: '4' },
];
function record() {
  const value = ui.newProfile();
  Object.assign(value, { profile_id: 'profile-one', revision: 'r1', name: '生产方案' });
  value.plugins.coverage.resources = { plugin: { artifact_id: 'dll', entry: 'lib/coverage.dll' }, program: { artifact_id: 'exe', entry: 'bin/convert.exe' }, collector: { artifact_id: 'cov', entry: 'OpenCppCoverage.exe' }, dataset: { artifact_id: 'data', root: '' } };
  value.plugins.coverage.engine_dlls = ['engine.dll'];
  return value;
}
function element(id) {
  return { id, dataset: {}, value: '', hidden: false, disabled: false, checked: false, innerHTML: '', textContent: '', listeners: {},
    addEventListener(name, fn) { this.listeners[name] = fn; }, querySelectorAll() { return []; }, focus() { this.focused = true; }, scrollIntoView() { this.scrolled = true; },
  };
}
async function harness(overrides = {}) {
  const elements = new Map(), calls = [], saved = [record()], dshReviews = [];
  const document = overrides.document || { getElementById(id) { if (!elements.has(id)) elements.set(id, element(id)); return elements.get(id); }, querySelectorAll() { return []; } };
  const request = async (url, options) => {
    const body = options?.body ? JSON.parse(options.body) : undefined;
    calls.push({ url, options, body });
    if (overrides.request) { const result = await overrides.request(url, options, body); if (result !== undefined) return result; }
    if (url === '/api/deployment-profiles' && !options) return { profiles: structuredClone(saved) };
    if (url === '/api/deployment-rollouts') return { rollouts: [], targets: {} };
    if (url === '/api/artifacts') return { artifacts: structuredClone(artifacts) };
    if (url === '/api/deployment-ai/review-context') return overrides.aiReview || { schema_version: 1, context_fingerprint: 'c'.repeat(64), plan_fingerprint: body.plan_fingerprint, profile_name: '生产方案', context: { profile: { name: '生产方案' }, targets: [], waiting_runs: [], warnings: [], constraints: [] } };
    if (url.endsWith('/resources')) return structuredClone(listings[url.split('/')[3]]);
    if (url.endsWith('/preview')) return { plan_fingerprint: 'f'.repeat(64), clients: [{ client_id: 'a', revision: 'a1' }, { client_id: 'b', revision: 'b1' }], scheduler_revision: 's1', scheduler_sync_required: false, scheduler_restart_required: false, warnings: [], ...overrides.preview };
    if (url.endsWith('/bind-scheduler')) return { scheduler_restart_required: false };
    if (url.endsWith('/publish')) return { rollout_id: 'rollout-one', revision: 'u1', profile_id: 'profile-one', profile_name: '生产方案', plugins: ['coverage'], status: 'waiting_runs', clients: [{ client_id: 'a', status: 'pending' }, { client_id: 'b', status: 'offline' }], waiting_runs: [], can_cancel: true, can_retry: false };
    if (options?.method === 'PUT' || options?.method === 'POST') return { ...body, profile_id: body.profile_id || 'created', revision: 'r2' };
    throw new Error(url);
  };
  let refreshes = 0;
  const controller = ui.createController({ document, request, navigate: overrides.navigate || (() => {}), tab() {}, operatorId: 'operator-one', uuid: () => '00000000-0000-4000-8000-000000000001', setBusy() {}, setSelected() {}, refresh: async () => { refreshes++; }, prepareDshDeploymentContext: (payload, returnToPreview) => { dshReviews.push({ payload, returnToPreview }); }, clearDshDeploymentContext: () => { dshReviews.push({ cleared: true }); } });
  await controller.load();
  return { controller, model: controller.model, get: (id) => document.getElementById(id), calls, saved, dshReviews, refreshes: () => refreshes };
}
function change(controller, dataset, value, checked) { controller.handleEditorChange({ target: { dataset, value, checked } }); }

test('defaults are real execution, one thread and all-day with an explicit create revision', () => {
  const profile = ui.newProfile();
  assert.equal(profile.revision, null);
  assert.equal(profile.plugins.coverage.dry_run, false);
  assert.deepEqual(profile.schedule, { task_threads: 1, execution_windows: [], update_windows: [] });
  assert.deepEqual(ui.defaultPlugin('stpcompare').arguments.baseline, ['--input', '{stp}', '--output-dir', '{outdir}']);
});

test('thread and time inputs update the draft without waiting for a change event', async () => {
  const h = await harness(); h.controller.init(); await h.controller.load(); await h.controller.edit('profile-one');
  const thread = h.get('profile-threads'); thread.value = '2';
  h.get('profile-form').listeners.input({ target: thread });
  assert.equal(h.model.draft.schedule.task_threads, 2);
  h.model.draft.schedule.execution_windows = [{ days: [1, 2], start: '09:00', end: '18:00' }];
  h.get('profile-form').listeners.input({ target: { value: '10:30', dataset: { profileWindow: 'execution_windows', time: 'start', index: '0' } } });
  assert.equal(h.model.draft.schedule.execution_windows[0].start, '10:30');
  assert.equal(await h.controller.save(), true);
  const schedule = h.calls.find((call) => call.options?.method === 'PUT').body.schedule;
  assert.equal(schedule.task_threads, 2); assert.equal(schedule.execution_windows[0].start, '10:30');
});

test('submit reads the visible thread and time values even if input and change events were missed', async () => {
  const h = await harness(); await h.controller.edit('profile-one');
  h.get('profile-threads').value = '3';
  h.model.draft.schedule.update_windows = [{ days: [6, 7], start: '09:00', end: '18:00' }];
  h.get('profile-windows').querySelectorAll = () => [{ value: '21:15', dataset: { profileWindow: 'update_windows', time: 'start', index: '0' } }, { value: '23:45', dataset: { profileWindow: 'update_windows', time: 'end', index: '0' } }];
  assert.equal(await h.controller.save(), true);
  const schedule = h.calls.find((call) => call.options?.method === 'PUT').body.schedule;
  assert.equal(schedule.task_threads, 3);
  assert.deepEqual(schedule.update_windows, [{ days: [6, 7], start: '21:15', end: '23:45' }]);
});

test('an empty visible thread input cannot save the previous valid count', async () => {
  const h = await harness(); await h.controller.edit('profile-one'); h.get('profile-threads').value = '';
  assert.equal(await h.controller.save(), false);
  assert.equal(h.calls.some((call) => call.options?.method === 'PUT'), false);
  assert.match(h.get('profile-editor-status').textContent, /并发数/);
});

test('resource discovery selects unique or matching entries without filesystem path input', () => {
  assert.equal(ui.autoEntry(listings.dll, 'plugin', 'coverage'), 'lib/coverage.dll');
  assert.equal(ui.autoEntry(listings.exe, 'program', 'coverage'), 'bin/convert.exe');
  assert.equal(ui.autoEntry(listings.cov, 'collector', 'coverage'), 'OpenCppCoverage.exe');
  assert.equal(ui.autoEntry({ executables: ['a.exe', 'b.exe'] }, 'program', 'coverage'), '');
  assert.equal(ui.autoEntry({ dataset_roots: ['data'] }, 'dataset', 'coverage'), 'data');
  assert.equal(ui.autoEntry(listings.data, 'dataset', 'coverage'), '');
  assert.deepEqual(ui.entryChoices(listings.data, 'dataset'), ['', 'parts']);
});

test('ambiguous executable and missing data roots block saving before the API call', async () => {
  const h = await harness(); await h.controller.edit('profile-one');
  h.model.draft.plugins.coverage.resources.program.entry = '';
  assert.equal(await h.controller.save(), false);
  assert.match(h.get('profile-editor-status').textContent, /入口/);
  assert.equal(h.calls.some((call) => call.options?.method === 'PUT'), false);
  h.model.draft.plugins.coverage.resources.program.entry = 'bin/convert.exe';
  h.model.inventories.data.dataset_roots = [];
  assert.equal(await h.controller.save(), false);
});

test('editing a name preserves saved advanced settings and original revision', async () => {
  const h = await harness();
  h.saved[0].plugins.stpcompare = { ...ui.defaultPlugin('stpcompare'), resources: { plugin: { artifact_id: 'dll', entry: 'other.dll' }, baseline: { artifact_id: 'exe', entry: 'bin/convert.exe' }, candidate: { artifact_id: 'exe', entry: 'bin/convert.exe' }, dataset: { artifact_id: 'data', root: 'parts' } }, arguments: { baseline: ['custom', '{stp}', '{outdir}'], candidate: ['custom2', '{stp}', '{outdir}'] } };
  await h.controller.load(); await h.controller.edit('profile-one');
  h.controller.handleEditorChange({ target: { id: 'profile-name', value: '新名称', dataset: {} } });
  assert.equal(await h.controller.save(), true);
  const body = h.calls.find((call) => call.options?.method === 'PUT').body;
  assert.equal(body.revision, 'r1');
  assert.deepEqual(body.plugins.coverage.engine_dlls, ['engine.dll']);
  assert.deepEqual(body.plugins.stpcompare.arguments.candidate, ['custom2', '{stp}', '{outdir}']);
  assert.equal(body.name, '新名称');
});

test('plugin toggling and switching editor preserve drafts', async () => {
  const h = await harness(); await h.controller.edit('profile-one');
  h.model.draft.name = '未保存名称';
  change(h.controller, { profilePlugin: 'coverage' }, '', false);
  change(h.controller, { profilePlugin: 'coverage' }, '', true);
  assert.deepEqual(h.model.draft.plugins.coverage.engine_dlls, ['engine.dll']);
  await h.controller.edit(); await h.controller.edit('profile-one');
  assert.equal(h.model.draft.name, '未保存名称');
});

test('coverage module selection uses package DLL paths and saves an explicit empty choice as all modules', async () => {
  const h = await harness(); await h.controller.edit('profile-one');
  assert.match(h.get('profile-resource-entries').innerHTML, /data-profile-engine-dlls multiple/);
  assert.match(h.get('profile-resource-entries').innerHTML, /value="engine.dll" selected/);
  h.controller.handleEditorChange({ target: { dataset: { profileEngineDlls: '' }, selectedOptions: [] } });
  assert.deepEqual(h.model.draft.plugins.coverage.engine_dlls, []);
  assert.equal(await h.controller.save(), true);
  assert.deepEqual(h.calls.find((call) => call.options?.method === 'PUT').body.plugins.coverage.engine_dlls, []);
  h.controller.handleEditorChange({ target: { dataset: { profileEngineDlls: '' }, selectedOptions: [{ value: 'engine.dll' }] } });
  assert.deepEqual(h.model.draft.plugins.coverage.engine_dlls, ['engine.dll']);
});

test('changing the coverage program retains only module selections present in the new package', async () => {
  const h = await harness({ request: async (url) => url.includes('/replacement/resources') ? { executables: ['new.exe'], dlls: ['new-engine.dll'] } : undefined });
  await h.controller.edit('profile-one');
  change(h.controller, { profileResource: 'coverage', role: 'program' }, 'replacement');
  await new Promise(setImmediate);
  assert.deepEqual(h.model.draft.plugins.coverage.engine_dlls, []);
  assert.match(h.get('profile-resource-entries').innerHTML, /new-engine.dll/);
  assert.doesNotMatch(h.get('profile-resource-entries').innerHTML, /value="engine.dll"/);
});

test('STP parameters edit one argument per line and survive resource changes and saves', async () => {
  const h = await harness();
  h.saved[0].plugins.stpcompare = { ...ui.defaultPlugin('stpcompare'), resources: { plugin: { artifact_id: 'dll', entry: 'other.dll' }, baseline: { artifact_id: 'exe', entry: 'bin/convert.exe' }, candidate: { artifact_id: 'exe', entry: 'bin/convert.exe' }, dataset: { artifact_id: 'data', root: 'parts' } } };
  await h.controller.load(); await h.controller.edit('profile-one');
  assert.match(h.get('profile-resource-entries').innerHTML, /data-profile-arguments="baseline"/);
  change(h.controller, { profileArguments: 'baseline' }, '--mode\nlegacy mode\n--source\n{stp}\n--dest\n{outdir}\n');
  change(h.controller, { profileArguments: 'candidate' }, '--source={stp}\r\n--destination={outdir}\r\n--precise');
  change(h.controller, { profileResource: 'stpcompare', role: 'baseline' }, 'exe');
  await new Promise(setImmediate);
  assert.match(h.get('profile-resource-entries').innerHTML, /legacy mode/);
  assert.equal(await h.controller.save(), true);
  const argumentsSaved = h.calls.find((call) => call.options?.method === 'PUT').body.plugins.stpcompare.arguments;
  assert.deepEqual(argumentsSaved.baseline, ['--mode', 'legacy mode', '--source', '{stp}', '--dest', '{outdir}']);
  assert.deepEqual(argumentsSaved.candidate, ['--source={stp}', '--destination={outdir}', '--precise']);
});

test('multiple executable entries immediately expand settings and focus the unresolved selector', async () => {
  const h = await harness({ request: async (url) => url.includes('/multiple/resources') ? { executables: ['first.exe', 'second.exe'], dlls: [] } : undefined });
  await h.controller.edit('profile-one'); assert.equal(h.get('profile-settings').open, false);
  change(h.controller, { profileResource: 'coverage', role: 'program' }, 'multiple');
  await new Promise(setImmediate);
  assert.equal(h.get('profile-settings').open, true);
  assert.equal(h.get('profile-entry-coverage-program').focused, true);
  assert.equal(h.get('profile-entry-coverage-program').scrolled, true);
  assert.match(h.get('profile-editor-status').textContent, /有多个入口/);
  change(h.controller, { profileEntry: 'coverage', role: 'program' }, 'second.exe');
  assert.equal(h.model.draft.plugins.coverage.resources.program.entry, 'second.exe');
});

test('a save conflict keeps draft and original revision through refresh and reopening', async () => {
  const h = await harness({ request: async (_url, options) => { if (options?.method === 'PUT') throw Object.assign(new Error('changed'), { status: 409 }); } });
  await h.controller.edit('profile-one'); h.model.draft.name = '我的草稿';
  assert.equal(await h.controller.save(), false);
  h.saved[0].revision = 'someone-else'; h.saved[0].name = '他人修改';
  await h.controller.load(); await h.controller.edit('profile-one');
  assert.equal(h.model.draft.name, '我的草稿'); assert.equal(h.model.draft.revision, 'r1');
  assert.equal(h.get('profile-load-latest').hidden, false);
});

test('late resource discovery cannot overwrite a newly selected artifact', async () => {
  let finish;
  const h = await harness({ request: async (url) => { if (url.includes('/slow/resources')) return new Promise((resolve) => { finish = resolve; }); } });
  await h.controller.edit('profile-one');
  change(h.controller, { profileResource: 'coverage', role: 'program' }, 'slow');
  change(h.controller, { profileResource: 'coverage', role: 'program' }, 'exe');
  finish({ executables: ['wrong.exe'] });
  await new Promise(setImmediate);
  assert.deepEqual(h.model.draft.plugins.coverage.resources.program, { artifact_id: 'exe', entry: 'bin/convert.exe' });
});

test('apply preview uses profile and machine identities and renders business summary only', async () => {
  const h = await harness(); await h.controller.openApply(['a', 'b']);
  assert.equal(await h.controller.preview(), true);
  const call = h.calls.find((entry) => entry.url.endsWith('/preview'));
  assert.deepEqual(call.body, { revision: 'r1', mode: 'assign', client_ids: ['a', 'b'] });
  assert.match(h.get('profile-apply-summary').innerHTML, /生产方案/);
  assert.match(h.get('profile-apply-summary').innerHTML, /并发数.*保持原值/);
  assert.doesNotMatch(h.get('profile-apply-summary').innerHTML, /config_patch|artifact_id|<pre>/);
  assert.equal(h.get('profile-apply-publish').disabled, false);
});

test('selection and profile revision changes invalidate previously reviewed plans', async () => {
  const h = await harness(); await h.controller.openApply(['a', 'b']); await h.controller.preview();
  h.controller.selectionChanged(['b']); assert.equal(h.model.preview, null);
  await h.controller.preview(); h.saved[0].revision = 'r3'; await h.controller.load();
  assert.equal(h.model.preview, null);
});

test('editing a reviewed profile clears preview and requires saving before applying', async () => {
  const h = await harness(); await h.controller.openApply(['a', 'b']); await h.controller.preview();
  await h.controller.edit('profile-one');
  h.controller.handleEditorChange({ target: { id: 'profile-name', value: '编辑中的方案', dataset: {} } });
  assert.equal(h.model.preview, null); assert.equal(h.get('profile-apply-preview').disabled, true);
  assert.equal(await h.controller.preview(), false);
  assert.equal(await h.controller.save(), true);
  assert.equal(h.get('profile-apply-preview').disabled, false);
  assert.equal(await h.controller.preview(), true);
});

test('one confirmation submits a durable update without a separate server sync step', async () => {
  const h = await harness({ preview: { scheduler_sync_required: true, scheduler_restart_required: true } });
  await h.controller.openUpdate('profile-one');
  assert.equal(h.calls.filter((call) => call.url.endsWith('/preview')).length, 1);
  assert.equal(h.get('profile-apply-bind').hidden, true);
  assert.equal(h.get('profile-apply-confirm-row').hidden, true);
  assert.equal(await h.controller.publish(), true);
  const call = h.calls.find((entry) => entry.url.endsWith('/publish'));
  assert.equal(call.body.mode, 'update'); assert.deepEqual(call.body.client_ids, []);
  assert.equal(call.body.plan_fingerprint, 'f'.repeat(64));
  assert.equal(call.body.request_id, '00000000-0000-4000-8000-000000000001');
  assert.equal(h.model.rollouts[0].status, 'waiting_runs');
  assert.equal(h.get('profile-apply').hidden, true);
  assert.match(h.get('client-rollouts').innerHTML, /关闭页面不影响推进/);
});

test('assignment preview is automatic and submits the selected machine identities once', async () => {
  const h = await harness(); await h.controller.openApply(['a', 'b']);
  assert.equal(h.calls.filter((entry) => entry.url.endsWith('/preview')).length, 1);
  assert.equal(await h.controller.publish(), true);
  const call = h.calls.find((entry) => entry.url.endsWith('/publish'));
  assert.deepEqual(call.body.client_ids, ['a', 'b']); assert.equal(call.body.mode, 'assign');
  assert.equal(call.body.operator_id, 'operator-one'); assert.equal(call.body.revision, 'r1');
  assert.equal(call.body.plan_fingerprint, 'f'.repeat(64));
  assert.equal(h.model.preview, null); assert.equal(h.refreshes(), 1);
});

test('deployment preview can be sent to DSH for advisory review without publishing', async () => {
  const h = await harness();
  assert.equal(await h.controller.openUpdate('profile-one'), true);
  const fingerprint = h.model.preview.plan_fingerprint;
  assert.equal(await h.controller.reviewWithDsh(), true);
  const review = h.calls.find((call) => call.url === '/api/deployment-ai/review-context');
  assert.equal(review.body.profile_id, 'profile-one');
  assert.equal(review.body.revision, 'r1');
  assert.equal(review.body.mode, 'update');
  assert.equal(review.body.plan_fingerprint, fingerprint);
  assert.equal(review.body.operator_id, 'operator-one');
  const prepared = h.dshReviews.filter((item) => item.payload).at(-1);
  assert.ok(prepared);
  assert.equal(prepared.payload.plan_fingerprint, fingerprint);
  assert.equal(h.get('profile-apply').hidden, true, 'the modal must close before the DSH sidebar can receive focus');
  assert.equal(h.model.preview.plan_fingerprint, fingerprint);
  assert.equal(h.model.requestId !== '', true);
  assert.equal(h.calls.some((call) => call.url.endsWith('/publish')), false);
  assert.equal(prepared.returnToPreview(), true);
  assert.equal(h.get('profile-apply').hidden, false);
});

test('changing the deployment preview clears the previous DSH review', async () => {
  const h = await harness();
  assert.equal(await h.controller.openUpdate('profile-one'), true);
  assert.equal(await h.controller.reviewWithDsh(), true);
  assert.equal(h.model.aiReview !== null, true);
  await h.controller.preview();
  assert.equal(h.model.aiReview, null);
  assert.equal(h.get('profile-apply-ai-status').textContent, '');
  assert.equal(h.dshReviews.some((item) => item.cleared), true);
});

test('empty legacy weekdays load as every day but clearing all days blocks save and survives switching drafts', async () => {
  const h = await harness(); h.saved[0].schedule.execution_windows = [{ days: [], start: '09:00', end: '18:00' }];
  await h.controller.load(); await h.controller.edit('profile-one');
  assert.deepEqual(h.model.draft.schedule.execution_windows[0].days, [1, 2, 3, 4, 5, 6, 7]);
  assert.equal((h.get('profile-windows').innerHTML.match(/data-day="\d" checked/g) || []).length, 7);
  for (let day = 1; day <= 7; day++) change(h.controller, { profileWindow: 'execution_windows', index: '0', day: String(day) }, '', false);
  assert.equal(await h.controller.save(), false);
  assert.match(h.get('profile-editor-status').textContent, /至少选择一天/);
  assert.equal(h.calls.some((call) => call.options?.method === 'PUT'), false);
  await h.controller.edit(); await h.controller.edit('profile-one');
  assert.deepEqual(h.model.draft.schedule.execution_windows[0].days, []);
});

test('a preview response cannot revive a plan invalidated while the editor remains usable', async () => {
  let release;
  const h = await harness({ request: async (url) => url.endsWith('/preview') ? new Promise((resolve) => { release = resolve; }) : undefined });
  h.model.selected = 'profile-one'; h.model.targets = ['a']; h.model.mode = 'assign'; const pending = h.controller.preview();
  await h.controller.edit('profile-one'); change(h.controller, { profileMode: 'coverage' }, 'simulate');
  release({ plan_fingerprint: 'f'.repeat(64), clients: [{ client_id: 'a', revision: 'a1' }] });
  assert.equal(await pending, false); assert.equal(h.model.preview, null);
  h.get('profile-apply-confirm').checked = true;
  assert.equal(await h.controller.publish(), false);
  assert.equal(h.calls.some((call) => call.url.endsWith('/publish')), false);
  assert.match(h.get('profile-apply-status').textContent, /未保存修改/);
});

test('publish rejects dirty drafts even if a stale preview was retained by another caller', async () => {
  const h = await harness(); await h.controller.openApply(['a']); await h.controller.preview();
  h.model.dirtyProfiles.add('profile-one'); h.get('profile-apply-confirm').checked = true;
  assert.equal(await h.controller.publish(), false);
  assert.equal(h.calls.some((call) => call.url.endsWith('/publish')), false);
});

test('conflict reload locks the editor until latest data and resources replace the draft', async () => {
  let release, hold = false;
  const h = await harness({ request: async (url, options) => {
    if (options?.method === 'PUT') throw Object.assign(new Error('changed'), { status: 409 });
    if (hold && url === '/api/deployment-profiles' && !options) return new Promise((resolve) => { release = resolve; });
  } });
  h.controller.init(); await h.controller.load(); await h.controller.edit('profile-one');
  h.controller.handleEditorChange({ target: { id: 'profile-description', dataset: {}, value: 'Preserve my draft' } });
  await h.controller.save(); hold = true;
  const pending = h.get('profile-load-latest').listeners.click();
  assert.equal(h.model.editorBusy, true);
  h.controller.handleEditorChange({ target: { id: 'profile-description', dataset: {}, value: 'Blocked while reloading' } });
  assert.equal(h.model.draft.description, 'Preserve my draft');
  release({ profiles: [{ ...record(), revision: 'r2', description: 'Latest' }] }); await pending;
  assert.equal(h.model.editorBusy, false); assert.equal(h.model.draft.description, 'Latest');
  assert.equal(h.model.conflictDraft.description, 'Preserve my draft');
  assert.equal(JSON.parse(h.get('profile-conflict-draft').value).description, 'Preserve my draft');
});

test('an in-flight publish is submitted once and a conflict retains choices for fresh preview', async () => {
  let reject;
  const h = await harness({ request: async (url) => { if (url.endsWith('/publish')) return new Promise((_, fail) => { reject = fail; }); } });
  await h.controller.openApply(['a', 'b']); await h.controller.preview(); h.get('profile-apply-confirm').checked = true;
  const pending = h.controller.publish(); assert.equal(await h.controller.publish(), false);
  reject(Object.assign(new Error('revision changed'), { status: 409 })); await pending;
  assert.equal(h.calls.filter((call) => call.url.endsWith('/publish')).length, 1);
  assert.deepEqual(h.model.targets, ['a', 'b']); assert.equal(h.model.selected, 'profile-one'); assert.equal(h.model.preview, null);
});

test('global preview includes busy and offline machines and old queued batches without sending a machine subset', async () => {
  const h = await harness({ preview: { clients: [{ client_id: 'busy', online: true, busy: true }, { client_id: 'offline', online: false }], waiting_runs: [{ run_id: 'A', plugin_type: 'coverage', status: 'running' }, { run_id: 'B', plugin_type: 'coverage', status: 'queued' }] } });
  assert.equal(await h.controller.openUpdate('profile-one'), true);
  const body = h.calls.find((call) => call.url.endsWith('/preview')).body;
  assert.deepEqual(body, { revision: 'r1', mode: 'update' });
  assert.match(h.get('profile-apply-summary').innerHTML, /1 台离线待补齐/);
  assert.match(h.get('profile-apply-summary').innerHTML, /已排队/);
  assert.match(h.get('profile-apply-summary').innerHTML, /已有机器.*保持原值.*首次配置使用方案默认安排/);
  assert.equal(h.get('profile-apply-publish').disabled, false);
});

test('a lost publish reply retries the same request id instead of creating a second update', async () => {
  let attempts = 0;
  const h = await harness({ request: async (url) => { if (url.endsWith('/publish') && ++attempts === 1) throw new Error('connection lost'); } });
  await h.controller.openUpdate('profile-one'); assert.equal(await h.controller.publish(), false);
  assert.ok(h.model.preview); assert.match(h.get('profile-apply-status').textContent, /同一请求标识/);
  assert.equal(await h.controller.publish(), true);
  const calls = h.calls.filter((call) => call.url.endsWith('/publish'));
  assert.equal(calls.length, 2); assert.equal(calls[0].body.request_id, calls[1].body.request_id);
});

test('unsupported Agent capabilities retain the scope preview and prevent confirmation or direct publishing', async () => {
  const h = await harness({ preview: { publish_blocked: true, unsupported_clients: ['legacy-agent'], warnings: ['legacy-agent 需要升级'] } });
  assert.equal(await h.controller.openUpdate('profile-one'), true);
  assert.match(h.get('profile-apply-summary').innerHTML, /legacy-agent 需要升级/);
  assert.match(h.get('profile-apply-status').textContent, /legacy-agent.*升级 Agent/);
  assert.equal(h.get('profile-apply-publish').disabled, true);
  assert.equal(await h.controller.publish(), false);
  assert.equal(h.calls.some((call) => call.url.endsWith('/publish')), false);
});

test('assignment review uses frozen published resources instead of the newer profile draft', async () => {
  const h = await harness({ preview: { published_target: true, profile_name: '已发布方案名', resource_summary: [{ plugin: 'coverage', resources: [{ label: 'program', name: 'published-converter', version: 'previous-v1' }] }] } });
  await h.controller.openApply(['new-machine']);
  const html = h.get('profile-apply-summary').innerHTML;
  assert.match(html, /published-converter.*previous-v1/); assert.match(html, /业务程序/);
  assert.doesNotMatch(html, /converter · 2|OpenCppCoverage/);
  assert.match(html, /尚未发布的变更不参与/);
  assert.match(html, /已发布方案名/); assert.doesNotMatch(html, /生产方案/);
});

test('persistent progress retains partial readiness and retries only the failed machine with current revision', async () => {
  let record = { rollout_id: 'rollout-one', revision: 'v1', profile_id: 'profile-one', profile_name: '生产方案', plugins: ['coverage'], status: 'ready', phase: 'ready', can_cancel: false, can_retry: true, clients: [{ client_id: 'ok', status: 'ready' }, { client_id: 'bad', status: 'failed', error: 'disk full' }, { client_id: 'gone', status: 'offline' }], waiting_runs: [] };
  const h = await harness({ request: async (url, _options, body) => {
    if (url === '/api/deployment-rollouts') return { rollouts: [record], targets: {} };
    if (url.endsWith('/retry')) { record = { ...record, revision: 'v2' }; return record; }
  } });
  await h.controller.loadRollouts();
  assert.match(h.get('client-rollouts').innerHTML, /新版可运行/);
  assert.match(h.get('client-rollouts').innerHTML, /1 \/ 3 台机器已就绪/);
  assert.match(h.get('client-rollouts').innerHTML, /disk full/);
  assert.match(h.get('client-rollouts').innerHTML, /离线待补齐/);
  assert.doesNotMatch(h.get('client-rollouts').innerHTML, /取消本次更新/);
  assert.equal(await h.controller.rolloutAction('cancel', record.rollout_id), false);
  assert.equal(await h.controller.rolloutAction('retry', record.rollout_id, { client_ids: ['bad'] }), true);
  assert.deepEqual(h.calls.find((call) => call.url.endsWith('/retry')).body, { revision: 'v1', client_ids: ['bad'] });
});

test('stopped batches expose viewing and recovery while abandonment requires a typed reason', async () => {
  const record = { rollout_id: 'rollout-one', revision: 'v1', profile_id: 'profile-one', profile_name: '生产方案', plugins: ['coverage'], status: 'waiting_runs', phase: 'waiting_runs', can_cancel: true, clients: [], waiting_runs: [{ run_id: 'stopped-A', plugin_type: 'coverage', status: 'stopped' }] };
  const h = await harness({ request: async (url) => url === '/api/deployment-rollouts' ? { rollouts: [record], targets: {} } : url.endsWith('/abandon') ? record : undefined });
  h.controller.init(); await h.controller.loadRollouts();
  assert.match(h.get('client-rollouts').innerHTML, /恢复旧批次/);
  assert.match(h.get('client-rollouts').innerHTML, /查看批次/);
  h.get('client-rollouts').listeners.click({ target: { closest: () => ({ dataset: { rolloutAbandon: 'stopped-A', rolloutId: 'rollout-one', plugin: 'coverage' } }) } });
  await h.get('rollout-abandon-form').listeners.submit({ preventDefault() {} });
  assert.equal(h.calls.some((call) => call.url.endsWith('/abandon')), false);
  h.get('rollout-abandon-reason').value = 'Input is obsolete';
  await h.get('rollout-abandon-form').listeners.submit({ preventDefault() {} });
  assert.deepEqual(h.calls.find((call) => call.url.endsWith('/abandon')).body, { revision: 'v1', run_id: 'stopped-A', plugin_type: 'coverage', reason: 'Input is obsolete' });
});

test('failed rollout repair preserves its replacement CAS through saving and previewing', async () => {
  const record = { rollout_id: 'failed-rollout', revision: 'failed-v1', profile_id: 'profile-one', profile_name: '生产方案', plugins: ['coverage'], status: 'blocked', phase: 'deploying', error: 'bad DLL', can_retry: true, clients: [{ client_id: 'a', status: 'failed' }], waiting_runs: [] };
  const h = await harness({ request: async (url) => url === '/api/deployment-rollouts' ? { rollouts: [record], targets: {} } : undefined });
  await h.controller.loadRollouts(); assert.match(h.get('client-rollouts').innerHTML, /修正方案并重新发布/);
  assert.match(h.get('client-rollouts').innerHTML, /class="current">应用更新/);
  assert.equal(await h.controller.repairRollout('failed-rollout'), true);
  h.controller.handleEditorChange({ target: { id: 'profile-description', dataset: {}, value: 'Corrected DLL' } });
  assert.equal(await h.controller.openUpdate('', true), true);
  const preview = h.calls.find((call) => call.url.endsWith('/preview')).body;
  assert.equal(preview.replaces_rollout_id, 'failed-rollout'); assert.equal(preview.replaces_revision, 'failed-v1');
  assert.equal(preview.revision, 'r2');
  assert.equal(await h.controller.publish(), true);
  const publish = h.calls.find((call) => call.url.endsWith('/publish')).body;
  assert.equal(publish.replaces_rollout_id, 'failed-rollout'); assert.equal(publish.replaces_revision, 'failed-v1');
});

test('a polling response from before publication cannot remove the newly accepted persistent request', async () => {
  let release;
  const h = await harness({ request: async (url) => url === '/api/deployment-rollouts' ? new Promise((resolve) => { release = resolve; }) : undefined });
  const pending = h.controller.loadRollouts();
  await h.controller.openUpdate('profile-one'); await h.controller.publish();
  release({ rollouts: [], targets: {} }); await pending;
  assert.equal(h.model.rollouts[0].rollout_id, 'rollout-one');
});

test('an obsolete polling failure cannot overwrite a newly accepted update', async () => {
  let reject;
  const h = await harness({ request: async (url) => url === '/api/deployment-rollouts' ? new Promise((_resolve, rejectRequest) => { reject = rejectRequest; }) : undefined });
  const pending = h.controller.loadRollouts();
  await h.controller.openUpdate('profile-one'); await h.controller.publish();
  reject(new Error('old connection failed')); await pending;
  assert.equal(h.model.rollouts[0].rollout_id, 'rollout-one'); assert.equal(h.model.rolloutError, '');
});

test('action conflicts remain visible after the automatic status refresh', async () => {
  const record = { rollout_id: 'rollout-one', revision: 'u1', status: 'waiting_runs', clients: [], can_cancel: true };
  const h = await harness({ request: async (url) => {
    if (url === '/api/deployment-rollouts') return { rollouts: [record], targets: {} };
    if (url.endsWith('/cancel')) throw Object.assign(new Error('revision changed'), { status: 409 });
  } });
  await h.controller.loadRollouts();
  assert.equal(await h.controller.rolloutAction('cancel', 'rollout-one'), false);
  assert.match(h.get('client-rollouts-status').textContent, /revision changed/);
});

test('managed derived datasets are excluded from the normal resource picker', async () => {
  const h = await harness(); h.model.artifacts.push({ artifact_id: 'managed', kind: 'dataset', name: '内部派生数据', version: '1', metadata: { managed: true } });
  await h.controller.edit('profile-one');
  assert.doesNotMatch(h.get('profile-resources').innerHTML, /内部派生数据/);
});

test('entry fields are selectors in a collapsed disclosure and module loads after the console', () => {
  const html = fs.readFileSync(path.join(__dirname, '../result_service/web/index.html'), 'utf8');
  assert.ok(html.indexOf('src="/app.js"') < html.indexOf('src="/profiles.js"'));
  assert.match(html, /<details id="profile-settings" class="workspace-advanced">/);
  assert.match(html, /id="workspace-tab-profiles"[^>]+aria-selected="true"/);
  assert.match(html, /id="clients-configure"[^>]*>分配部署方案/);
  assert.match(html, /<details class="workspace-advanced profile-legacy">[\s\S]*?id="deployment-new"[\s\S]*?<\/details>/);
  assert.doesNotMatch(html, /clients-deployment-heading/);
  const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map((match) => match[1]);
  assert.equal(new Set(ids).size, ids.length);
});

if (process.env.TESTTRACKING_DOM_MODULE) {
  const { JSDOM } = require(process.env.TESTTRACKING_DOM_MODULE);
  test('DOM update and assignment reviews open above pages, restore focus, and focus published progress', async () => {
    const dom = new JSDOM(fs.readFileSync(path.join(__dirname, '../result_service/web/index.html'), 'utf8'), { pretendToBeVisual: true });
    const pages = [];
    const h = await harness({ document: dom.window.document, navigate: (page) => { pages.push(page); dom.window.document.querySelectorAll('.page').forEach((element) => { element.hidden = element.id !== 'page-' + page; }); } });
    h.controller.init(); await h.controller.load(); await h.controller.loadRollouts();
    const dialog = h.get('profile-apply');
    dialog.showModal = () => { assert.equal(dialog.hidden, false); dialog.open = true; };
    dialog.close = () => { dialog.open = false; };
    assert.equal(dialog.parentElement, dom.window.document.body);
    h.get('page-plugins').hidden = false;
    const opener = dom.window.document.querySelector('[data-profile-update]'); opener.focus();
    await h.controller.openUpdate('profile-one');
    assert.equal(dialog.open, true); assert.equal(h.get('page-plugins').hidden, false);
    assert.equal(dom.window.document.activeElement, h.get('profile-apply-title'));
    assert.equal(h.get('profile-apply-publish').closest('.profile-apply-footer').parentElement, dialog);
    assert.equal(h.get('profile-apply-summary').closest('.profile-apply-body').parentElement, dialog);
    h.get('profile-apply-close').click();
    assert.equal(dialog.open, false); assert.equal(dom.window.document.activeElement.dataset.profileUpdate, 'profile-one');
    h.get('clients-configure').disabled = false; h.get('clients-configure').focus();
    await h.controller.openApply(['new-machine']); assert.equal(dialog.open, true);
    h.get('profile-apply-close').click(); assert.equal(dom.window.document.activeElement, h.get('clients-configure'));
    await h.controller.openUpdate('profile-one'); await h.controller.publish();
    assert.equal(dialog.open, false); assert.equal(dialog.hidden, true);
    assert.equal(dom.window.document.activeElement.dataset.rolloutRow, 'rollout-one');
    assert.equal(pages.at(-1), 'clients');
    dom.window.close();
  });

  test('DOM historical publication cannot promise runnable state while a newer global target is pending', async () => {
    const dom = new JSDOM(fs.readFileSync(path.join(__dirname, '../result_service/web/index.html'), 'utf8'));
    const old = { rollout_id: 'old', profile_name: '旧发布', status: 'completed', phase: 'completed', plugins: ['coverage'], target_ids: { coverage: 'v1' }, clients: [{ client_id: 'a', status: 'ready' }], waiting_runs: [{ run_id: 'old-run', plugin_type: 'coverage', status: 'running' }], resource_summary: [{ plugin: 'coverage', resources: [{ label: 'collector', name: 'collector-package', version: '1' }, { label: 'dataset', name: 'parts', version: '1' }] }] };
    const pending = { ...old, rollout_id: 'new', profile_name: '新发布', status: 'waiting_runs', phase: 'waiting_runs', target_ids: { coverage: 'v2' }, clients: [{ client_id: 'a', status: 'pending' }] };
    let rows = [pending, old];
    const h = await harness({ document: dom.window.document, request: async (url) => url === '/api/deployment-rollouts' ? { rollouts: rows, targets: { coverage: { target_id: 'v1', rollout_id: 'old', ready: true } } } : undefined });
    await h.controller.loadRollouts();
    let oldCard = h.get('client-rollouts').querySelector('[data-rollout-row="old"]');
    assert.equal(oldCard.dataset.rolloutCurrent, 'false'); assert.match(oldCard.textContent, /历史发布/);
    assert.doesNotMatch(oldCard.textContent, /可执行新版任务|新版可运行|先完成的旧版本批次/);
    assert.equal(oldCard.querySelector('[data-rollout-runs]'), null);
    assert.match(oldCard.textContent, /覆盖率采集工具/); assert.match(oldCard.textContent, /STP 数据/);
    assert.equal(h.get('client-rollouts').querySelector('[data-rollout-row="new"]').dataset.rolloutCurrent, 'true');
    rows = [old]; await h.controller.loadRollouts();
    oldCard = h.get('client-rollouts').querySelector('[data-rollout-row="old"]');
    assert.equal(oldCard.dataset.rolloutCurrent, 'true'); assert.ok(oldCard.querySelector('[data-rollout-runs]'));
    rows = [{ ...old, status: 'cancelled', phase: 'cancelled', waiting_runs: [{ run_id: 'stopped', status: 'stopped', plugin_type: 'coverage' }] }];
    await h.controller.loadRollouts(); oldCard = h.get('client-rollouts').querySelector('[data-rollout-row="old"]');
    assert.doesNotMatch(oldCard.textContent, /先完成的旧版本批次|更新才能继续|恢复旧批次/);
    dom.window.close();
  });

  test('DOM waiting blockers group results by run and show machine blockers without empty batch links', async () => {
    const dom = new JSDOM(fs.readFileSync(path.join(__dirname, '../result_service/web/index.html'), 'utf8'));
    const h = await harness({ document: dom.window.document, preview: { waiting_runs: [{ plugin_type: 'coverage', run_id: 'A', status: 'running' }, { plugin_type: 'coverage', run_id: 'A', status: 'result_pending', task_id: 'one' }, { plugin_type: 'coverage', run_id: '', status: 'client_busy', client_id: 'busy-agent' }] } });
    await h.controller.openUpdate('profile-one');
    const summary = h.get('profile-apply-summary');
    assert.equal(summary.querySelectorAll('[data-run-row]').length, 1);
    assert.equal(summary.querySelectorAll('[data-rollout-view-run]').length, 1);
    assert.match(summary.textContent, /1 个任务结果待确认/); assert.match(summary.textContent, /busy-agent.*等待机器任务与结果确认/);
    assert.doesNotMatch(summary.textContent, /client_busy|result_pending/);
    dom.window.close();
  });

  test('DOM weekday controls and disabled conflict fields follow actual browser form state', async () => {
    const dom = new JSDOM(fs.readFileSync(path.join(__dirname, '../result_service/web/index.html'), 'utf8'));
    let hold = false, release;
    const h = await harness({ document: dom.window.document, request: async (url, options) => {
      if (options?.method === 'PUT') throw Object.assign(new Error('changed'), { status: 409 });
      if (hold && url === '/api/deployment-profiles' && !options) return new Promise((resolve) => { release = resolve; });
    } });
    h.saved[0].schedule.update_windows = [{ days: [], start: '09:00', end: '18:00' }];
    h.controller.init(); await h.controller.load(); await h.controller.edit('profile-one');
    const days = [...dom.window.document.querySelectorAll('[data-day]')];
    assert.equal(days.length, 7); assert.ok(days.every((input) => input.checked));
    for (const input of days) { input.checked = false; input.dispatchEvent(new dom.window.Event('change', { bubbles: true })); }
    assert.equal(await h.controller.save(), false); assert.match(h.get('profile-editor-status').textContent, /至少选择一天/);
    days[0].checked = true; days[0].dispatchEvent(new dom.window.Event('change', { bubbles: true }));
    h.get('profile-description').value = 'Last draft'; h.get('profile-description').dispatchEvent(new dom.window.Event('input', { bubbles: true }));
    await h.controller.save(); hold = true; h.get('profile-load-latest').click();
    assert.equal(h.get('profile-description').disabled, true); assert.equal(h.get('profile-name').disabled, true);
    release({ profiles: [{ ...record(), revision: 'r2', description: 'Latest' }] });
    for (let attempt = 0; attempt < 20 && h.model.editorBusy; attempt++) await new Promise(setImmediate);
    assert.equal(h.get('profile-description').disabled, false);
    assert.equal(h.get('profile-description').value, 'Latest'); assert.equal(h.model.conflictDraft.description, 'Last draft');
    dom.window.close();
  });
}
