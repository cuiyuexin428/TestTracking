'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const script = fs.readFileSync(path.join(__dirname, '../result_service/web/app.js'), 'utf8');

const coverageView = {
  schema_version: 1,
  plugin: 'coverage',
  title: '覆盖率结果',
  description: 'coverage view',
  primary_metric: {
    label: '行覆盖率',
    value: 'analysis.percent',
    format: 'percent',
  },
  widgets: [{
    id: 'summary',
    type: 'metrics',
    title: '摘要',
    items: [{
      label: '文件',
      value: 'run.expected_files',
      format: 'integer',
    }],
  }],
};

const pluginViewSchema = {
  type: 'object',
  required: ['schema_version', 'plugin', 'widgets'],
};

// The console uses plain DOM properties. Keep this stub limited to the
// properties and event hooks exercised by initialization and console actions.
function element() {
  const style = {
    properties: {},
    setProperty(name, value) { this.properties[name] = String(value); },
  };
  const classes = new Set();
  return {
    value: '', disabled: false, hidden: false, textContent: '', innerHTML: '',
    style, dataset: {}, children: [], listeners: {}, attributes: {}, capturedPointer: null,
    classList: {
      toggle(name, force) { const enabled = force === undefined ? !classes.has(name) : force; if (enabled) classes.add(name); else classes.delete(name); return enabled; },
      add(...names) { names.forEach((name) => classes.add(name)); },
      remove(...names) { names.forEach((name) => classes.delete(name)); },
      contains(name) { return classes.has(name); },
    },
    querySelector() { return element(); },
    querySelectorAll() { return []; },
    addEventListener(name, callback) { this.listeners[name] = callback; },
    setAttribute(name, value) { this.attributes[name] = String(value); },
    getAttribute(name) { return this.attributes[name] ?? null; },
    removeAttribute(name) { delete this.attributes[name]; },
    focus() { this.focused = true; },
    select() { this.selected = true; },
    setPointerCapture(pointerId) { this.capturedPointer = pointerId; },
    appendChild(child) { this.children.push(child); },
    replaceChildren(...children) { this.children = children; },
  };
}

async function createConsole({
  available = 'dsh',
  databaseReady = false,
  runs = [],
  runDetails = {},
  runsError = false,
  nativeGate = null,
  nativeFailure = null,
  clipboardAvailable = true,
  clipboardFailure = false,
  detailGate = null,
  viewportWidth = 1440,
  sidebarOpen = true,
  storedPanelWidth = null,
  storedUiScale = null,
  storedOperatorId = null,
  pluginMetadata = null,
  reportSource = 'def render(connection, context):\n    return "<html>Report</html>"',
  reportRevision = 'revision-1',
  reportGate = null,
  scriptGate = null,
  scriptReadFailure = null,
  scriptSaveFailure = null,
  reportFailure = null,
  catalogs = null,
  latestReports = null,
  scheduler = { online: false, plugins: [], clients: [], active_runs: [], in_flight: 0 },
  commandGate = null,
  commandError = null,
  logs = { files: [], selected: null, lines: [], truncated: false },
  configs = {},
  configError = null,
  artifacts = [],
  deployments = [],
  deploymentError = null,
  deploymentSaveGate = null,
  deploymentPreviewGate = null,
  deploymentPreviewResult = null,
  schedulerBindError = null,
  serverPluginError = null,
} = {}) {
  const elements = new Map();
  const get = (id) => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  const posts = [];
  const requests = [];
  const clipboardWrites = [];
  const puts = [];
  let savedView = { ...coverageView, revision: '1'.repeat(64) };
  const currentPlugins = () => (typeof pluginMetadata === 'function' ? pluginMetadata() : pluginMetadata) || [{
    name: 'coverage', configured: true, view_editable: true, view: savedView,
  }];
  const reportPosts = [];
  const scriptPuts = [];
  const scriptSaveRequests = [];
  const savedScripts = {};
  const savedScriptRevisions = {};
  const currentScriptRevision = (plugin) => savedScriptRevisions[plugin]
    ?? (typeof reportRevision === 'function' ? reportRevision(plugin) : reportRevision);
  const generatedReports = {};
  const intervals = [];
  const commands = [];
  const logRequests = [];
  const configSaves = [];
  const configGets = [];
  const artifactUploads = [];
  const deploymentSaves = [];
  const deploymentPreviews = [];
  const schedulerBindings = [];
  const serverPluginInstalls = [];
  const navButtons = ['overview', 'results', 'clients', 'history', 'plugins', 'logs'].map((page) => {
    const button = element(); button.dataset.page = page; return button;
  });
  const workspaceTabs = ['plugins', 'artifacts', 'deployments', 'settings'].map((tab) => {
    const button = get(`workspace-tab-${tab}`); button.dataset.workspaceTab = tab; return button;
  });
  const workspacePanels = workspaceTabs.map((button) => {
    const panel = get(`workspace-panel-${button.dataset.workspaceTab}`); panel.dataset.workspacePanel = button.dataset.workspaceTab; return panel;
  });
  const deploymentSteps = Array.from({ length: 5 }, (_, index) => {
    const button = get(`deployment-step-${index}`); button.dataset.deploymentStep = String(index); return button;
  });
  const deploymentStages = deploymentSteps.map((_, index) => {
    const panel = get(`deployment-stage-${index}`); panel.dataset.deploymentStage = String(index); return panel;
  });
  let detailRequests = 0;
  const storage = new Map();
  if (storedPanelWidth !== null) storage.set('testtracking.ai.panel-width.v1', String(storedPanelWidth));
  if (storedUiScale !== null) storage.set('testtracking.ui.scale.v1', String(storedUiScale));
  if (storedOperatorId !== null) storage.set('testtracking.ai.operator-id.v1', String(storedOperatorId));
  const windowListeners = {};
  const documentElement = element();
  const browserWindow = {
    innerWidth: viewportWidth,
    matchMedia: () => ({ matches: false }),
    addEventListener(name, callback) { windowListeners[name] = callback; },
    setInterval(callback, milliseconds) { intervals.push({ callback, milliseconds }); },
  };
  get('result-type').value = 'coverage';
  get('run-all-clients').checked = true;
  get('console').dataset.aiOpen = String(sidebarOpen);
  const context = vm.createContext({
    document: {
      body: element(),
      documentElement,
      getElementById: get,
      createElement: element,
      querySelectorAll: (selector) => {
        if (selector === '.nav-item') return navButtons;
        if (selector === '[data-workspace-tab]') return workspaceTabs;
        if (selector === '[data-workspace-panel]') return workspacePanels;
        if (selector === '[data-deployment-step]') return deploymentSteps;
        if (selector === '[data-deployment-stage]') return deploymentStages;
        return [];
      },
    },
    localStorage: {
      getItem: (key) => storage.get(key) ?? null,
      setItem: (key, value) => storage.set(key, String(value)),
    },
    window: browserWindow,
    navigator: clipboardAvailable ? { clipboard: { writeText: async (value) => {
      if (clipboardFailure) throw new Error('clipboard permission denied');
      clipboardWrites.push(value);
    } } } : {},
    URLSearchParams,
    fetch: async (url, options = {}) => {
      requests.push(url);
      let status = 200;
      let data;
      switch (url) {
        case '/healthz': data = { status: 'ok' }; break;
        case '/readyz':
          status = databaseReady ? 200 : 503;
          data = { database_configured: databaseReady };
          break;
        case '/api/plugins':
          data = currentPlugins();
          break;
        case '/api/plugin-view-schema': data = pluginViewSchema; break;
        case '/api/scheduler': data = typeof scheduler === 'function' ? scheduler() : scheduler; break;
        case '/api/artifacts': data = { artifacts }; break;
        case '/api/deployments': data = { clients: deployments }; break;
        case '/api/scheduler/commands':
          commands.push(JSON.parse(options.body));
          if (commandGate) await commandGate;
          status = commandError ? 409 : 200;
          data = commandError ? (typeof commandError === 'string' ? { error: commandError } : commandError) : { ok: true, run_id: 'new-run' };
          break;
        case '/api/ai/providers':
          data = [{ name: 'dsh', available: (typeof available === 'function' ? available() : available) === 'dsh' }];
          break;
        case '/api/runs?limit=100':
          status = runsError ? 500 : 200;
          data = runsError ? { error: 'temporary query failure' }
            : (typeof runs === 'function' ? runs() : runs).slice(0, 100);
          break;
        case '/api/ai/dsh-web':
          assert.equal(options.method, 'POST');
          const dshWebBody = JSON.parse(options.body);
          posts.push({ dshWeb: true, operator_id: dshWebBody.operator_id });
          if (nativeGate) await nativeGate;
          const nativeError = typeof nativeFailure === 'function' ? nativeFailure() : nativeFailure;
          status = nativeError ? 503 : 200;
          data = nativeError ? { error: nativeError } : { url: `/ai/dsh/${dshWebBody.operator_id}/` };
          break;
        default:
          if (url.startsWith('/api/artifacts/upload?')) {
            artifactUploads.push({ url, ...options });
            data = { sha256: 'a'.repeat(64) };
          } else if (url === '/api/deployments/preview') {
            const payload = JSON.parse(options.body); deploymentPreviews.push(payload);
            if (deploymentPreviewGate) await deploymentPreviewGate;
            data = deploymentPreviewResult ? deploymentPreviewResult(payload) : { ...payload, program_bindings: [], scheduler_revision: 'scheduler-original', warnings: [] };
          } else if (url === '/api/deployments/bind-scheduler') {
            schedulerBindings.push(JSON.parse(options.body));
            status = schedulerBindError ? 409 : 200;
            data = schedulerBindError ? { error: schedulerBindError } : { scheduler_revision: 'scheduler-next', scheduler_restart_required: true };
          } else if (url === '/api/server-plugins/preview') {
            data = { plugin: 'example', version: '1', scheduler_revision: 's1', result_service_revision: 'r1', files: ['example.dll'], changes: ['install example'], warnings: [], restart_required: true };
          } else if (url === '/api/server-plugins/install') {
            serverPluginInstalls.push(JSON.parse(options.body));
            status = serverPluginError ? 409 : 200;
            data = serverPluginError ? { error: serverPluginError } : { installed: true, restart_required: true };
          } else if (url.startsWith('/api/deployments/')) {
            const parts = url.slice('/api/deployments/'.length).split('/');
            const client = decodeURIComponent(parts[0]);
            const current = deployments.find((item) => item.client_id === client) || { client_id: client, desired: null, history: [] };
            if (options.method === 'PUT') {
              const payload = JSON.parse(options.body);
              deploymentSaves.push({ client, ...payload });
              if (deploymentSaveGate) await deploymentSaveGate;
              const failure = typeof deploymentError === 'function' ? deploymentError() : deploymentError;
              status = failure ? 409 : 200;
              data = failure ? { error: failure } : { ...current, desired: { ...payload, revision: 'next-revision' } };
            } else data = current;
          } else if (url.startsWith('/api/logs?')) {
            const query = Object.fromEntries(new URLSearchParams(url.split('?')[1]));
            logRequests.push(query);
            data = typeof logs === 'function' ? logs(query) : logs;
            if (data.error) status = 500;
          } else if (url.startsWith('/api/config/')) {
            const kind = url.split('/').pop();
            if (options.method === 'PUT') {
              const payload = JSON.parse(options.body);
              configSaves.push({ kind, ...payload });
              status = configError ? 409 : 200;
              data = configError ? { error: configError } : { ...configs[kind], config: payload.config, revision: 'next', saved: true, restart_required: true };
              if (!configError) configs[kind] = data;
            } else {
              configGets.push(kind);
              data = configs[kind] || { kind, available: false, config: null, fields: [], reason: 'No configuration file' };
              if (data.error) status = 500;
            }
          } else if (url.startsWith('/api/runs?')) {
            const query = new URLSearchParams(url.split('?')[1]);
            const currentRuns = typeof runs === 'function' ? runs() : runs;
            const matches = currentRuns.filter((run) => run.run_id.toLowerCase().includes((query.get('search') || '').toLowerCase()));
            const offset = Number(query.get('offset') || 0);
            data = matches.slice(offset, offset + Number(query.get('limit') || 50));
          } else if (url === '/api/plugins/coverage/view' && options.method === 'PUT') {
            const definition = JSON.parse(options.body);
            puts.push(definition);
            const revision = currentPlugins().find((item) => item.name === 'coverage').view.revision;
            status = definition.revision === revision ? 200 : 409;
            if (status === 200) savedView = { ...definition, revision: '2'.repeat(64) };
            data = status === 200 ? savedView : {
              error: '展示已被其他使用者修改。当前草稿已保留，请读取最新版本后合并修改。',
              current_revision: revision,
            };
          } else if (url.startsWith('/api/analysis/catalog?plugin=')) {
            const plugin = new URLSearchParams(url.split('?')[1]).get('plugin');
            const currentRuns = typeof runs === 'function' ? runs() : runs;
            data = catalogs?.[plugin] || {
              runs: currentRuns.filter((run) => run.plugin_type === plugin).slice(0, 100),
              tables: [{ name: 'file_results', columns: [{ name: 'run_id', type: 'text' }] }],
              script_contract: { entrypoint: 'render(connection, context)', returns: 'HTML' },
            };
          } else if (/\/report-script$/.test(url)) {
            const plugin = decodeURIComponent(url.split('/')[3]);
            if (options.method === 'PUT') {
              const payload = JSON.parse(options.body);
              scriptPuts.push({ plugin, source: payload.source });
              scriptSaveRequests.push({ plugin, ...payload });
              const conflict = payload.revision !== currentScriptRevision(plugin);
              status = scriptSaveFailure ? 400 : (conflict ? 409 : 200);
              if (status === 200) {
                savedScripts[plugin] = payload.source;
                savedScriptRevisions[plugin] = 'revision-2';
              }
              data = scriptSaveFailure ? { error: scriptSaveFailure } : (conflict
                ? { error: 'report script changed after it was loaded; reload the latest script before saving' }
                : { plugin, source: payload.source, revision: 'revision-2' });
            } else {
              if (scriptGate) await scriptGate(plugin);
              const failure = typeof scriptReadFailure === 'function' ? scriptReadFailure(plugin) : scriptReadFailure;
              status = failure ? 503 : 200;
              data = failure ? { error: failure } : { plugin, source: savedScripts[plugin] ?? (typeof reportSource === 'function' ? reportSource(plugin) : reportSource), revision: currentScriptRevision(plugin) };
            }
          } else if (/\/reports\?run_id=/.test(url) && options.method === undefined) {
            const plugin = decodeURIComponent(url.split('/')[3]);
            const runId = decodeURIComponent(url.split('run_id=')[1]);
            data = { latest: (typeof latestReports === 'function' ? latestReports(plugin, runId) : latestReports?.[runId]) || generatedReports[runId] || null };
          } else if (/\/reports$/.test(url) && options.method === 'POST') {
            const plugin = decodeURIComponent(url.split('/')[3]);
            const payload = JSON.parse(options.body);
            reportPosts.push({ plugin, ...payload });
            if (reportGate) await reportGate;
            const error = typeof reportFailure === 'function' ? reportFailure() : reportFailure;
            status = error ? 500 : 200;
            data = error ? { error } : { plugin, run_id: payload.run_id, report_id: 'report-1', url: `/reports/${payload.run_id}/report-1.html` };
            if (!error) generatedReports[payload.run_id] = data;
          } else if (url.startsWith('/api/analysis/runs/') && options.method === undefined) {
            const runId = decodeURIComponent(url.slice('/api/analysis/runs/'.length).split('?')[0]);
            detailRequests += 1;
            if (detailGate && detailRequests > 1) await detailGate;
            const currentRuns = typeof runs === 'function' ? runs() : runs;
            data = runDetails[runId] || currentRuns.find((run) => run.run_id === runId)
              || Object.values(catalogs || {}).flatMap((catalog) => catalog.runs).find((run) => run.run_id === runId);
            if (!data) {
              status = 404;
              data = { error: `run not found: ${runId}` };
            }
          } else {
            throw new Error(`unexpected request: ${url}`);
          }
      }
      return { ok: status === 200, status, json: async () => data };
    },
  });
  vm.runInContext(script, context, { filename: 'app.js' });
  // Fetch responses are immediate promises; let initialization and its error
  // handler finish before inspecting the visible controls.
  await new Promise(setImmediate);
  return { get, posts, requests, clipboardWrites, puts, reportPosts, scriptPuts, scriptSaveRequests, intervals, storage, windowListeners, documentElement, window: browserWindow,
    commands, logRequests, configSaves, configGets, artifactUploads, deploymentSaves, deploymentPreviews, schedulerBindings, serverPluginInstalls,
    workspaceTabs, workspacePanels, deploymentSteps, deploymentStages,
    wizard: {
      read: (...args) => JSON.parse(JSON.stringify(context.readDeploymentWizard(...args))),
      build: (...args) => JSON.parse(JSON.stringify(context.buildDeploymentWizard(...args))),
    },
    navigate: async (page) => {
      navButtons.find((button) => button.dataset.page === page).listeners.click();
      await new Promise(setImmediate);
    },
    workspace: async (tab) => {
      workspaceTabs.find((button) => button.dataset.workspaceTab === tab).listeners.click();
      await new Promise(setImmediate);
    } };
}

test('scheduler status is live independently of database and shows clients tasks and resources', async () => {
  const ui = await createConsole({ scheduler: {
    online: true, plugins: ['coverage'], active_runs: [], in_flight: 1,
    clients: [{ worker_id: 'worker-07', plugin_type: 'coverage', registered: true, status: 'busy',
      ip: '192.168.1.7', last_seen_age_ms: 1500, memory_bytes: 1073741824, disk_free_bytes: 2147483648,
      cpu_percent: 32, plugin_info: 'program-v1', tasks: [{ task_id: 'task-1', run_id: 'run-1', attempt_id: 'attempt-1', acked: true }] }],
  } });
  assert.equal(ui.get('run-start').disabled, false);
  assert.match(ui.get('client-list').innerHTML, /worker-07/);
  assert.match(ui.get('client-list').innerHTML, /1.5 秒前/);
  assert.match(ui.get('client-list').innerHTML, /task-1/);
  assert.match(ui.get('client-list').innerHTML, /客户端已确认/);
  assert.match(ui.get('client-list').innerHTML, /32%/);
  assert.match(ui.get('plugin-grid').innerHTML, /已启用/);
  assert.match(ui.get('plugin-grid').innerHTML, /分析适配器/);
});

test('LAN HTTP artifact upload sends the file without relying on secure-context crypto', async () => {
  const ui = await createConsole();
  await ui.navigate('plugins');
  const file = { name: 'stp.zip' };
  ui.get('artifact-kind').value = 'dataset';
  ui.get('artifact-name').value = 'stp';
  ui.get('artifact-version').value = 'd1';
  ui.get('artifact-file').files = [file];
  await ui.get('artifact-upload-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.artifactUploads.length, 1);
  assert.equal(ui.artifactUploads[0].body, file);
  assert.equal(ui.artifactUploads[0].headers['Content-Type'], 'application/octet-stream');
  assert.match(ui.get('artifact-status').textContent, /上传成功/);
});

test('deployment publish retains the loaded revision and draft across polling and conflicts', async () => {
  const artifact = { artifact_id: 'a'.repeat(32), kind: 'dataset', name: 'stp', version: 'd1', size_bytes: 12, sha256: '1'.repeat(64) };
  const deployments = [{ client_id: 'worker-1', state: 'current', desired: { revision: 'original', config_patch: {}, components: [{ slot: 'dataset.stp', artifact_id: artifact.artifact_id, artifact }] } }];
  const ui = await createConsole({ artifacts: [artifact], deployments, deploymentError: 'other operator changed deployment' });
  await ui.navigate('plugins');
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  const draft = '{"plugins":{"coverage":{"stp_root":"{artifact:dataset.stp}"}}}';
  ui.get('deployment-patch').value = draft;
  deployments[0].desired.revision = 'external';
  deployments[0].state = 'waiting_window';
  await ui.intervals[1].callback();
  assert.match(ui.get('deployment-list').innerHTML, /等待更新时间窗/);
  await ui.get('artifacts-refresh').listeners.click();
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves[0].revision, 'original');
  assert.equal(ui.deploymentSaves[0].components[0].artifact_id, artifact.artifact_id);
  assert.equal(ui.get('deployment-patch').value, draft);
  assert.match(ui.get('deployment-status').textContent, /other operator/);
});

test('deployment cannot reuse another client revision after changing client input', async () => {
  const ui = await createConsole();
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  ui.get('deployment-client').value = 'worker-2';
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves.length, 0);
  assert.match(ui.get('deployment-status').textContent, /先读取/);
});

const wizardArtifact = (id, kind, name = id, version = 'v1') => ({ artifact_id: id.repeat(32), kind, name, version, size_bytes: 10, sha256: 'a'.repeat(64) });

test('workspace tabs preserve both deployment and service drafts while showing one panel at a time', async () => {
  const ui = await createConsole({ configs: { scheduler: {
    available: true, config: { server: { port: 5555 } }, revision: 'original', fields: [],
  } } });
  await ui.navigate('plugins');
  await ui.workspace('deployments');
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  ui.get('deployment-field-coverage-target_exe').value = 'C:/tools/draft.exe';
  ui.get('deployment-threads').value = '6';
  await ui.workspace('settings');
  const configDraft = '{"server":{"port":5557},"custom":"keep this draft"}';
  ui.get('config-json').value = configDraft;
  ui.get('config-json').listeners.input();
  await ui.workspace('artifacts');
  await ui.intervals[1].callback();
  await ui.workspace('deployments');
  assert.equal(ui.get('deployment-field-coverage-target_exe').value, 'C:/tools/draft.exe');
  assert.equal(ui.get('deployment-threads').value, '6');
  assert.deepEqual(ui.workspacePanels.filter((panel) => !panel.hidden).map((panel) => panel.dataset.workspacePanel), ['deployments']);
  assert.equal(ui.get('workspace-tab-deployments').attributes['aria-selected'], 'true');
  assert.equal(ui.get('workspace-tab-settings').attributes['aria-selected'], 'false');
  await ui.workspace('settings');
  assert.equal(ui.get('config-json').value, configDraft);
  assert.equal(ui.configSaves.length, 0);
  assert.equal(ui.deploymentSaves.length, 0);
});

test('guided deployment validates each step, previews before confirmation and publishes once while pending', async () => {
  let finishSave;
  const saveGate = new Promise((resolve) => { finishSave = resolve; });
  const program = wizardArtifact('a', 'program', 'coverage-tool');
  const ui = await createConsole({ artifacts: [program], deploymentSaveGate: saveGate });
  await ui.navigate('plugins');
  await ui.get('deployment-new').listeners.click();
  const next = () => ui.get('deployment-next').listeners.click();
  const currentStep = () => ui.deploymentSteps.findIndex((button) => button.attributes['aria-current'] === 'step');
  await next();
  assert.equal(currentStep(), 0);
  assert.match(ui.get('deployment-status').textContent, /客户端/);
  ui.get('deployment-client').value = 'worker-guided';
  await next();
  assert.equal(currentStep(), 1);
  ui.get('deployment-plugin').value = '';
  await next();
  assert.equal(currentStep(), 1);
  assert.match(ui.get('deployment-status').textContent, /执行插件/);
  ui.get('deployment-plugin').value = 'coverage';
  ui.get('deployment-plugin').listeners.change();
  await next();
  assert.equal(currentStep(), 2);
  ui.get('deployment-artifact-coverage').value = program.artifact_id;
  ui.get('deployment-field-coverage-target_exe').value = '../outside.exe';
  ui.get('deployment-field-collector-opencppcoverage').value = 'C:/tools/OpenCppCoverage.exe';
  ui.get('deployment-dry-run').value = 'real';
  await next();
  assert.equal(currentStep(), 2);
  assert.equal(ui.deploymentPreviews.length, 0);
  assert.equal(ui.deploymentSaves.length, 0);
  ui.get('deployment-field-coverage-target_exe').value = 'bin/run.exe';
  await next();
  assert.equal(currentStep(), 3);
  ui.get('deployment-threads').value = '4';
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves.length, 0);
  await next();
  assert.equal(currentStep(), 4);
  assert.equal(ui.deploymentPreviews.length, 1);
  assert.match(ui.get('deployment-summary').innerHTML, /worker-guided/);
  assert.match(ui.get('deployment-summary').innerHTML, /coverage-tool/);
  const pending = ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves.length, 1);
  assert.equal(ui.get('deployment-client').disabled, true);
  assert.equal(ui.get('deployment-new').disabled, true);
  assert.equal(ui.get('deployment-close').disabled, true);
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  await ui.get('deployment-new').listeners.click();
  assert.equal(ui.deploymentSaves.length, 1);
  assert.equal(ui.get('deployment-client').value, 'worker-guided');
  finishSave();
  await pending;
  assert.equal(ui.deploymentSaves[0].config_patch.client.task_threads, 4);
  assert.equal(ui.deploymentSaves[0].config_patch.plugins.coverage.target_exe, '{artifact:program.coverage-tool}/bin/run.exe');
  assert.equal(ui.get('deployment-client').disabled, false);
  assert.match(ui.get('deployment-status').textContent, /已发布/);
});

test('artifact search and type filters leave loaded deployment components intact', async () => {
  const program = wizardArtifact('a', 'program', 'runner');
  const dataset = wizardArtifact('b', 'dataset', 'stp-data');
  const components = [
    { slot: 'program.runner', artifact_id: program.artifact_id },
    { slot: 'dataset.stp', artifact_id: dataset.artifact_id },
  ];
  const ui = await createConsole({ artifacts: [program, dataset], deployments: [{
    client_id: 'worker-1', state: 'current', desired: { revision: 'original', components, config_patch: { custom: 'retained' } },
  }] });
  await ui.navigate('plugins');
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  await ui.workspace('artifacts');
  ui.get('artifact-filter').value = 'program';
  ui.get('artifact-filter').listeners.change();
  assert.match(ui.get('artifact-list').innerHTML, /runner/);
  assert.doesNotMatch(ui.get('artifact-list').innerHTML, /stp-data/);
  ui.get('artifact-search').value = 'does-not-exist';
  ui.get('artifact-search').listeners.input();
  assert.doesNotMatch(ui.get('artifact-list').innerHTML, /runner/);
  ui.get('artifact-search').value = '';
  ui.get('artifact-search').listeners.input();
  ui.get('artifact-filter').value = '';
  ui.get('artifact-filter').listeners.change();
  assert.match(ui.get('artifact-list').innerHTML, /runner/);
  assert.match(ui.get('artifact-list').innerHTML, /stp-data/);
  await ui.workspace('deployments');
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.deepEqual(ui.deploymentSaves[0].components, components);
  assert.deepEqual(ui.deploymentSaves[0].config_patch, { custom: 'retained' });
});

test('plugin details open read-only and return focus without replacing deployment drafts', async () => {
  const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'], clients: [], active_runs: [] } });
  await ui.navigate('plugins');
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  ui.get('deployment-field-coverage-target_exe').value = 'C:/tools/not-yet-published.exe';
  const trigger = element();
  trigger.dataset.pluginDetail = 'coverage';
  await ui.get('plugin-grid').listeners.click({ target: { closest: (selector) => selector === '[data-plugin-detail]' ? trigger : null } });
  assert.equal(ui.get('workspace-drawer').hidden, false);
  assert.match(ui.get('workspace-drawer-title').textContent, /coverage/);
  assert.match(ui.get('workspace-drawer-content').innerHTML, /调度器已启用/);
  assert.match(ui.get('workspace-drawer-content').innerHTML, /分析适配器已配置/);
  assert.equal(ui.deploymentSaves.length, 0);
  ui.get('workspace-drawer-close').listeners.click();
  assert.equal(ui.get('workspace-drawer').hidden, true);
  assert.equal(trigger.focused, true);
  assert.equal(ui.get('deployment-field-coverage-target_exe').value, 'C:/tools/not-yet-published.exe');
});

test('reopening the same deployment preserves advanced edits and imports them when returning to the form', async () => {
  const ui = await createConsole({ deployments: [{
    client_id: 'worker-1', state: 'current', desired: { revision: 'original', components: [],
      config_patch: { plugins: { coverage: { enabled: true, target_exe: 'C:/tools/old.exe' } } },
    },
  }] });
  await ui.navigate('plugins');
  const open = () => ui.get('deployment-list').listeners.click({ target: {
    closest: (selector) => selector === '[data-edit-deployment]' ? { dataset: { editDeployment: 'worker-1' } } : null,
  } });
  await open();
  assert.equal(ui.get('deployment-advanced').open, false);
  ui.get('deployment-editor-mode').value = 'json';
  ui.get('deployment-editor-mode').listeners.change();
  const draft = '{"plugins":{"coverage":{"enabled":true,"target_exe":"C:/tools/edited.exe"}},"future_setting":17}';
  ui.get('deployment-patch').value = draft;
  ui.get('deployment-close').listeners.click();
  await open();
  assert.equal(ui.get('deployment-editor-mode').value, 'json');
  assert.equal(ui.get('deployment-advanced').open, true);
  assert.equal(ui.get('deployment-patch').value, draft);
  ui.get('deployment-editor-mode').value = 'wizard';
  ui.get('deployment-editor-mode').listeners.change();
  assert.equal(ui.get('deployment-field-coverage-target_exe').value, 'C:/tools/edited.exe');
  assert.equal(JSON.parse(ui.get('deployment-patch').value).future_setting, 17);
  assert.equal(ui.deploymentSaves.length, 0);
});

test('switching to advanced JSON publishes current form edits and preserves unrepresented settings', async () => {
  const program = wizardArtifact('a', 'program', 'runner');
  const dataset = wizardArtifact('b', 'dataset', 'stp-data');
  const ui = await createConsole({ artifacts: [program, dataset], deployments: [{
    client_id: 'worker-1', state: 'current', desired: { revision: 'original', components: [],
      config_patch: { custom: { retain: 7 }, plugins: { coverage: { enabled: true, future_option: 'keep' } } },
    },
  }] });
  await ui.navigate('plugins');
  await ui.get('deployment-list').listeners.click({ target: {
    closest: (selector) => selector === '[data-edit-deployment]' ? { dataset: { editDeployment: 'worker-1' } } : null,
  } });
  await ui.get('deployment-next').listeners.click();
  await ui.get('deployment-next').listeners.click();
  ui.get('deployment-artifact-coverage').value = program.artifact_id;
  ui.get('deployment-artifact-dataset').value = dataset.artifact_id;
  ui.get('deployment-field-coverage-target_exe').value = 'bin/edited.exe';
  ui.get('deployment-field-dataset-stp_root').value = 'stp';
  ui.get('deployment-field-collector-opencppcoverage').value = 'C:/tools/OpenCppCoverage.exe';
  ui.get('deployment-dry-run').value = 'real';
  ui.get('deployment-editor-mode').value = 'json';
  ui.get('deployment-editor-mode').listeners.change();
  assert.equal(ui.get('deployment-editor-mode').value, 'json');
  const draft = JSON.parse(ui.get('deployment-patch').value);
  assert.equal(draft.plugins.coverage.target_exe, '{artifact:program.coverage-tool}/bin/edited.exe');
  assert.equal(draft.plugins.coverage.stp_root, '{artifact:dataset.stp}/stp');
  assert.equal(draft.plugins.coverage.future_option, 'keep');
  assert.deepEqual(draft.custom, { retain: 7 });
  assert.equal(ui.deploymentPreviews.length, 0);
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.deepEqual(ui.deploymentSaves[0].config_patch, draft);
  assert.deepEqual(ui.deploymentSaves[0].components, [
    { slot: 'dataset.stp', artifact_id: dataset.artifact_id },
    { slot: 'program.coverage-tool', artifact_id: program.artifact_id },
  ]);
});

test('invalid form values cannot switch into advanced mode and publish an older JSON draft', async () => {
  const program = wizardArtifact('a', 'program', 'runner');
  const ui = await createConsole({ artifacts: [program] });
  await ui.navigate('plugins');
  await ui.get('deployment-new').listeners.click();
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-next').listeners.click();
  await ui.get('deployment-next').listeners.click();
  const previous = ui.get('deployment-patch').value;
  ui.get('deployment-artifact-coverage').value = program.artifact_id;
  ui.get('deployment-field-coverage-target_exe').value = '../outside.exe';
  ui.get('deployment-dry-run').value = 'real';
  ui.get('deployment-editor-mode').value = 'json';
  ui.get('deployment-editor-mode').listeners.change();
  assert.equal(ui.get('deployment-editor-mode').value, 'wizard');
  assert.equal(ui.get('deployment-patch').value, previous);
  assert.equal(ui.get('deployment-field-coverage-target_exe').value, '../outside.exe');
  assert.equal(ui.deploymentSteps[2].attributes['aria-current'], 'step');
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves.length, 0);
});

test('deployment form round trip preserves unknown fields and independent time windows', async () => {
  const ui = await createConsole();
  const program = wizardArtifact('a', 'program');
  const patch = { scheduler: { custom_endpoint: 'keep' }, client: { future_option: { enabled: true }, execution_windows: [{ days: [5], start: '22:00', end: '07:00', comment: 'overnight' }] }, plugins: {
    coverage: { enabled: true, dry_run: true, store_cov: true, program_dir: '{artifact:custom.program}/bin', target_exe: '{artifact:custom.program}/bin/run.exe', opencppcoverage: 'C:/tools/coverage.exe', engine_dlls: ['{artifact:custom.program}/lib/a.dll'], future: { setting: 8 } },
    custom: { enabled: false, dll: 'custom.dll', own_config: { retain: true } },
  } };
  const original = JSON.stringify(patch);
  const components = [{ slot: 'custom.program', artifact_id: program.artifact_id }];
  const model = ui.wizard.read(patch, components);
  const result = ui.wizard.build(patch, components, model, [program], ['custom']);
  assert.equal(JSON.stringify(patch), original);
  assert.deepEqual(result.config_patch.scheduler, patch.scheduler);
  assert.deepEqual(result.config_patch.client, patch.client);
  assert.deepEqual(result.config_patch.plugins.coverage.future, { setting: 8 });
  assert.equal(result.config_patch.plugins.coverage.store_cov, true);
  assert.deepEqual(result.config_patch.plugins.coverage.engine_dlls, patch.plugins.coverage.engine_dlls);
  assert.deepEqual(result.config_patch.plugins.custom, patch.plugins.custom);
  assert.equal(result.config_patch.plugins.demo.enabled, false);
  assert.equal(result.config_patch.plugins.stpcompare.enabled, false);
  assert.deepEqual(result.components, components);
});

test('STP form uses two explicit program slots and retains plugin settings when switching', async () => {
  const ui = await createConsole();
  const oldProgram = wizardArtifact('a', 'program', 'converter', 'v1');
  const newProgram = wizardArtifact('b', 'program', 'converter', 'v2');
  const patch = { plugins: { coverage: { enabled: true, engine_dlls: ['keep.dll'] }, stpcompare: { keep_work: true }, custom: { enabled: true, custom_option: 3 } } };
  const model = ui.wizard.read(patch, [], 'stpcompare');
  model.dryRun = false;
  for (const [role, artifact] of [['baseline', oldProgram], ['candidate', newProgram]]) {
    model.groups[role].artifactId = artifact.artifact_id;
    model.groups[role].values[`programs.${role}.executable`] = 'bin/run.exe';
    model.groups[role].values[`programs.${role}.version_file`] = 'metadata/version.txt';
  }
  const result = ui.wizard.build(patch, [], model, [oldProgram, newProgram]);
  assert.deepEqual(result.components, [{ slot: 'program.stp-baseline', artifact_id: oldProgram.artifact_id }, { slot: 'program.stp-candidate', artifact_id: newProgram.artifact_id }]);
  assert.equal(result.config_patch.plugins.stpcompare.programs.baseline.executable, '{artifact:program.stp-baseline}/bin/run.exe');
  assert.equal(result.config_patch.plugins.stpcompare.programs.candidate.version_file, '{artifact:program.stp-candidate}/metadata/version.txt');
  assert.deepEqual(result.config_patch.plugins.stpcompare.programs.candidate.arguments, ['--input', '{stp}', '--output-dir', '{outdir}']);
  assert.deepEqual(result.config_patch.plugins.coverage.engine_dlls, ['keep.dll']);
  assert.equal(result.config_patch.plugins.coverage.enabled, false);
  assert.equal(result.config_patch.plugins.custom.enabled, false);
  assert.equal(result.config_patch.plugins.custom.custom_option, 3);
  assert.equal(result.config_patch.plugins.stpcompare.keep_work, true);
});

test('switching coverage from an artifact to local paths clears its generated identity and detached component', async () => {
  const ui = await createConsole();
  const program = wizardArtifact('a', 'program');
  const extra = wizardArtifact('b', 'dataset');
  const patch = { plugins: { coverage: { enabled: true, dry_run: false, store_cov: false,
    program_dir: '{artifact:program.coverage-tool}', target_exe: '{artifact:program.coverage-tool}/run.exe',
    program_version: 'sha256:old-content', opencppcoverage: 'C:/collector.exe',
    engine_dlls: ['{artifact:program.coverage-tool}/engine.dll'], future_option: 'keep' } } };
  const components = [{ slot: 'program.coverage-tool', artifact_id: program.artifact_id }, { slot: 'dataset.extra', artifact_id: extra.artifact_id }];
  const model = ui.wizard.read(patch, components);
  model.groups.coverage.artifactId = '';
  model.groups.coverage.values.program_dir = 'D:/local';
  model.groups.coverage.values.target_exe = 'D:/local/replacement.exe';
  model.groups.coverage.values.engine_dlls = '';
  const result = ui.wizard.build(patch, components, model, [program, extra]);
  assert.equal(result.config_patch.plugins.coverage.program_version, '');
  assert.equal(result.config_patch.plugins.coverage.target_exe, 'D:/local/replacement.exe');
  assert.equal(result.config_patch.plugins.coverage.future_option, 'keep');
  assert.deepEqual(result.config_patch.plugins.coverage.engine_dlls, []);
  assert.deepEqual(result.components, [components[1]]);
  const roundTrip = ui.wizard.read(result.config_patch, result.components);
  assert.equal(roundTrip.groups.coverage.artifactId, '');
  assert.deepEqual(ui.wizard.build(result.config_patch, result.components, roundTrip, [program, extra]), result);
  model.groups.coverage.values.program_dir = '';
  assert.equal(ui.wizard.build(patch, components, model, [program, extra]).config_patch.plugins.coverage.program_dir, undefined);
});

test('switching STP programs to local paths retains an explicit replacement version and shared dependencies', async () => {
  const ui = await createConsole();
  const baseline = wizardArtifact('a', 'program');
  const candidate = wizardArtifact('b', 'program');
  const patch = { plugins: { stpcompare: { enabled: true, dry_run: false, programs: {
    baseline: { executable: '{artifact:program.stp-baseline}/run.exe', version: 'sha256:old-baseline', arguments: ['{stp}', '{outdir}'] },
    candidate: { executable: '{artifact:program.stp-candidate}/run.exe', version: 'sha256:old-candidate', arguments: ['{stp}', '{outdir}'] },
  }, custom_dependency: '{artifact:program.stp-baseline}/retained.dat' } } };
  const components = [{ slot: 'program.stp-baseline', artifact_id: baseline.artifact_id }, { slot: 'program.stp-candidate', artifact_id: candidate.artifact_id }];
  const model = ui.wizard.read(patch, components);
  for (const role of ['baseline', 'candidate']) {
    model.groups[role].artifactId = '';
    model.groups[role].values[`programs.${role}.executable`] = `D:/local/${role}.exe`;
  }
  model.groups.candidate.values['programs.candidate.version'] = 'manual-v2';
  const result = ui.wizard.build(patch, components, model, [baseline, candidate]);
  assert.equal(result.config_patch.plugins.stpcompare.programs.baseline.version, '');
  assert.equal(result.config_patch.plugins.stpcompare.programs.candidate.version, 'manual-v2');
  assert.deepEqual(result.components, [components[0]]);
  assert.equal(result.config_patch.plugins.stpcompare.custom_dependency, patch.plugins.stpcompare.custom_dependency);
  const roundTrip = ui.wizard.read(result.config_patch, result.components);
  assert.equal(roundTrip.groups.baseline.artifactId, '');
  assert.equal(roundTrip.groups.candidate.artifactId, '');
  assert.deepEqual(ui.wizard.build(result.config_patch, result.components, roundTrip, [baseline, candidate]), result);
});

test('local program versions are preserved when no artifact identity was generated', async () => {
  const ui = await createConsole();
  const patch = { plugins: { coverage: { enabled: true, target_exe: 'D:/old.exe', program_version: 'manual-v1' } } };
  const model = ui.wizard.read(patch, []);
  model.groups.coverage.values.target_exe = 'D:/new.exe';
  assert.equal(ui.wizard.build(patch, [], model, []).config_patch.plugins.coverage.program_version, 'manual-v1');
});

test('new deployments inherit execution mode and require an explicit mode before distributing programs', async () => {
  const ui = await createConsole();
  const model = ui.wizard.read({}, []);
  assert.equal(model.dryRun, null);
  assert.equal(ui.wizard.build({}, [], model, []).config_patch.plugins.coverage.dry_run, undefined);
  assert.equal(ui.wizard.build({}, [], model, []).config_patch.plugins.coverage.store_cov, false);
  const program = wizardArtifact('a', 'program');
  model.groups.coverage.artifactId = program.artifact_id;
  assert.throws(() => ui.wizard.build({}, [], model, [program]), /明确选择/);
  const existing = { client: { task_threads: 4, execution_windows: [{ start: '20:00', end: '06:00' }] }, plugins: { coverage: { enabled: true, dry_run: true } } };
  const current = ui.wizard.read(existing, []);
  assert.deepEqual(ui.wizard.build(existing, [], current, []).config_patch.client.execution_windows, existing.client.execution_windows);
  current.dryRun = null; current.threads = '';
  const inherited = ui.wizard.build(existing, [], current, []).config_patch;
  assert.equal(inherited.client.task_threads, undefined);
  assert.equal(inherited.plugins.coverage.dry_run, undefined);
});

test('unsaved deployment form fields survive plugin and editor switches', async () => {
  const ui = await createConsole();
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  ui.get('deployment-field-coverage-target_exe').value = 'C:/tools/unpublished.exe';
  ui.get('deployment-plugin').value = 'stpcompare';
  ui.get('deployment-plugin').listeners.change();
  ui.get('deployment-plugin').value = 'coverage';
  ui.get('deployment-plugin').listeners.change();
  assert.equal(ui.get('deployment-field-coverage-target_exe').value, 'C:/tools/unpublished.exe');
  ui.get('deployment-editor-mode').value = 'json';
  ui.get('deployment-editor-mode').listeners.change();
  ui.get('deployment-editor-mode').value = 'wizard';
  ui.get('deployment-editor-mode').listeners.change();
  assert.equal(ui.get('deployment-field-coverage-target_exe').value, 'C:/tools/unpublished.exe');
});

test('deployment form validates executable paths and STP output arguments before preview', async () => {
  const ui = await createConsole();
  const program = wizardArtifact('a', 'program');
  const model = ui.wizard.read({}, [], 'stpcompare');
  model.dryRun = false;
  model.groups.baseline.artifactId = program.artifact_id;
  model.groups.baseline.values['programs.baseline.executable'] = '../run.exe';
  assert.throws(() => ui.wizard.build({}, [], model, [program]), /相对路径/);
  model.groups.baseline.values['programs.baseline.executable'] = 'bin/run.exe';
  model.groups.baseline.values['programs.baseline.arguments'] = '--input\n{stp}';
  assert.throws(() => ui.wizard.build({}, [], model, [program]), /outdir/);
  model.groups.baseline.values['programs.baseline.arguments'] = '--input\n{stp}\n--output-dir\n{outdir}';
  model.windows.execution = { mode: 'custom', items: [{ days: [1, 2], start: '22:00', end: '07:00' }] };
  model.windows.update = { mode: 'all', items: [] };
  const result = ui.wizard.build({}, [], model, [program]);
  assert.deepEqual(result.config_patch.client.execution_windows, model.windows.execution.items);
  assert.deepEqual(result.config_patch.client.update_windows, []);
});

test('deployment wizard previews canonical program identity and preserves drafts on publish conflicts', async () => {
  const artifact = wizardArtifact('a', 'program', 'tool');
  const ui = await createConsole({ artifacts: [artifact], deploymentError: 'changed by another operator', deploymentPreviewResult: (payload) => ({ ...payload, config_patch: { ...payload.config_patch, plugins: { ...payload.config_patch.plugins, coverage: { ...payload.config_patch.plugins.coverage, program_version: 'sha256:content' } } }, program_bindings: [{ plugin: 'coverage', role: 'program', version: 'sha256:content', executable: 'run.exe' }], scheduler_revision: 'scheduler-original', warnings: [] }) });
  await ui.navigate('plugins');
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  ui.get('deployment-artifact-coverage').value = artifact.artifact_id;
  ui.get('deployment-field-coverage-target_exe').value = 'run.exe';
  ui.get('deployment-field-collector-opencppcoverage').value = 'C:/tools/coverage.exe';
  ui.get('deployment-dry-run').value = 'real';
  ui.get('deployment-store-cov').checked = true;
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves.length, 0);
  await ui.get('deployment-preview').listeners.click();
  assert.equal(ui.deploymentPreviews.length, 1);
  assert.match(ui.get('deployment-binding-list').innerHTML, /sha256:content/);
  const draft = ui.get('deployment-patch').value;
  await ui.get('deployment-bind-scheduler').listeners.click();
  assert.equal(ui.schedulerBindings[0].scheduler_revision, 'scheduler-original');
  assert.match(ui.get('deployment-binding-status').textContent, /重启调度器/);
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves.length, 1);
  assert.equal(ui.deploymentSaves[0].revision, null);
  assert.equal(ui.deploymentSaves[0].config_patch.plugins.coverage.program_version, 'sha256:content');
  assert.equal(ui.deploymentSaves[0].config_patch.plugins.coverage.store_cov, true);
  assert.equal(ui.get('deployment-patch').value, draft);
  assert.match(ui.get('deployment-status').textContent, /another operator/);
});

test('editing during server preview cannot replace the current deployment draft', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const ui = await createConsole({ deploymentPreviewGate: gate });
  ui.get('deployment-client').value = 'worker-1';
  await ui.get('deployment-load').listeners.click();
  const pending = ui.get('deployment-preview').listeners.click();
  ui.get('deployment-threads').value = '4';
  release(); await pending;
  assert.equal(ui.get('deployment-patch').value, '{}');
  assert.match(ui.get('deployment-preview-status').textContent, /草稿已修改/);
  await ui.get('deployment-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.deploymentSaves.length, 0);
});

test('service plugin install uses both reviewed configuration revisions and preserves failed preview', async () => {
  const artifact = wizardArtifact('a', 'plugin');
  const ui = await createConsole({ artifacts: [artifact], serverPluginError: 'configuration changed' });
  await ui.navigate('plugins');
  ui.get('server-plugin-artifact').value = artifact.artifact_id;
  await ui.get('server-plugin-preview').listeners.click();
  const preview = ui.get('server-plugin-plan').textContent;
  await ui.get('server-plugin-install').listeners.click();
  assert.equal(ui.serverPluginInstalls[0].scheduler_revision, 's1');
  assert.equal(ui.serverPluginInstalls[0].result_service_revision, 'r1');
  assert.equal(ui.serverPluginInstalls[0].artifact_id, artifact.artifact_id);
  assert.ok(ui.serverPluginInstalls[0].operator_id);
  assert.equal(ui.get('server-plugin-plan').textContent, preview);
  assert.match(ui.get('server-plugin-status').textContent, /configuration changed/);
  assert.equal(ui.get('server-plugin-install').disabled, true);
});

test('a queued run blocked by changed inputs explains the cause and stays cancellable', async () => {
  const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'],
    active_runs: [], clients: [], queued_runs: [{ run_id: 'blocked-run', plugin_type: 'coverage',
      dataset_version: 'd1', blocked: true, blocked_reason: 'manifest changed <d1>',
      worker_ids: ['worker-1/coverage'] }] } });
  assert.match(ui.get('active-runs').innerHTML, /等待恢复原配置/);
  assert.match(ui.get('active-runs').innerHTML, /manifest changed &lt;d1&gt;/);
  assert.match(ui.get('active-runs').innerHTML, /worker-1\/coverage/);
  assert.match(ui.get('active-runs').innerHTML, /data-stop-run="blocked-run"[^>]*>取消排队/);
});

test('task targets use exact selected worker identities and cannot silently become unrestricted', async () => {
  const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'], active_runs: [], clients: [
    { worker_id: 'worker-1/coverage', plugin_type: 'coverage', registered: true, status: 'idle' },
    { worker_id: 'worker-2/coverage', plugin_type: 'coverage', registered: true, status: 'idle' },
  ] } });
  ui.get('run-dataset').value = 'd1';
  ui.get('run-all-clients').checked = false;
  ui.get('run-all-clients').listeners.change();
  await ui.get('run-start-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.commands.length, 0);
  ui.get('run-clients').selectedOptions = [{ value: 'worker-2/coverage' }];
  ui.get('run-clients').listeners.change();
  await ui.get('run-start-form').listeners.submit({ preventDefault() {} });
  assert.deepEqual(ui.commands[0].worker_ids, ['worker-2/coverage']);
});

test('offline scheduler disables execution controls while reports and AI remain usable', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [{ run_id: 'done', plugin_type: 'coverage' }] });
  assert.equal(ui.get('run-start').disabled, true);
  assert.equal(ui.get('run-wakeup').disabled, true);
  assert.match(ui.get('dsh-native-frame').src, /^\/ai\/dsh\//);
  assert.equal(ui.get('run-report').disabled, false);
  assert.match(ui.get('clients-status').textContent, /不可达/);
});

test('delayed scheduler snapshot is labelled as cached while its clients and commands stay usable', async () => {
  const snapshot = { online: true, snapshot_age_ms: 12500, plugins: ['coverage'], active_runs: [], clients: [
    { worker_id: 'slow-worker', plugin_type: 'coverage', registered: true, tasks: [] },
  ] };
  const ui = await createConsole({ scheduler: () => snapshot });
  assert.match(ui.get('scheduler-status').textContent, /状态更新延迟 12.5 秒/);
  assert.match(ui.get('clients-status').textContent, /缓存状态/);
  assert.match(ui.get('client-list').innerHTML, /slow-worker/);
  assert.equal(ui.get('run-start').disabled, false);
  snapshot.snapshot_age_ms = 100;
  await ui.get('clients-refresh').listeners.click();
  assert.match(ui.get('clients-status').textContent, /实时状态/);
  assert.doesNotMatch(ui.get('scheduler-status').textContent, /延迟/);
});

test('slow report workspace loading does not delay execution controls', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const ui = await createConsole({
    scheduler: { online: true, plugins: ['coverage'], active_runs: [], clients: [] },
    scriptGate: () => gate,
  });
  try {
    assert.equal(ui.get('run-start').disabled, false);
    assert.match(ui.get('scheduler-status').textContent, /调度器在线/);
  } finally { release(); }
  await new Promise(setImmediate);
});

test('run command sends version and params once while pending and exposes command errors', async () => {
  let release;
  const commandGate = new Promise((resolve) => { release = resolve; });
  const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'], active_runs: [], clients: [] }, commandGate, commandError: 'dataset mismatch' });
  ui.get('run-dataset').value = 'd7';
  ui.get('run-params').value = '--mode full';
  const pending = ui.get('run-start-form').listeners.submit({ preventDefault() {} });
  await new Promise(setImmediate);
  await ui.get('run-start-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.commands.length, 1);
  assert.equal(ui.get('run-start').disabled, true);
  release(); await pending;
  assert.deepEqual(ui.commands[0], { action: 'run', plugin_type: 'coverage', dataset_version: 'd7', params: '--mode full' });
  assert.match(ui.get('scheduler-command-status').textContent, /dataset mismatch/);
  assert.equal(ui.get('run-start').disabled, false);
});

test('duplicate work explains completion or active state, identifies the existing run and offers explicit rerun', async () => {
  for (const [status, expected] of [['finished', '相同任务已全部完成'], ['running', '相同任务正在执行'], ['queued', '相同任务已在排队'], ['planning', '相同任务正在规划'], ['stopped', '待恢复的进度']]) {
    const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'], active_runs: [], clients: [] },
      commandError: { error: 'equivalent work already exists', duplicate: true, run_id: `existing-${status}`, status, force_rerun_available: true } });
    ui.get('run-dataset').value = 'd1';
    await ui.get('run-start-form').listeners.submit({ preventDefault() {} });
    const notice = ui.get('scheduler-command-status').textContent;
    assert.ok(notice.includes(expected), notice);
    assert.ok(notice.includes(`existing-${status}`), notice);
    assert.match(notice, /强制重跑.*原因/);
    assert.equal(ui.commands.length, 1);
    assert.equal(Boolean(ui.get('run-force').checked), false);
  }
});

test('duplicate hints require structured metadata and respect rerun availability', async () => {
  for (const [commandError, expected] of [
    ['equivalent work already exists', /命令失败：equivalent work already exists/],
    [{ error: 'equivalent work already exists', duplicate: true, status: 'finished', run_id: 'existing', force_rerun_available: false }, /相同任务已全部完成/],
  ]) {
    const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'], active_runs: [], clients: [] }, commandError });
    ui.get('run-dataset').value = 'd1';
    await ui.get('run-start-form').listeners.submit({ preventDefault() {} });
    assert.match(ui.get('scheduler-command-status').textContent, expected);
    assert.doesNotMatch(ui.get('scheduler-command-status').textContent, /强制重跑/);
  }
});

test('stop command carries expected run id and ignores stale run buttons', async () => {
  const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'], clients: [],
    active_runs: [{ plugin_type: 'coverage', run_id: 'active-run', stopping: false }] } });
  assert.equal(ui.get('run-start').disabled, false);
  assert.equal(ui.get('run-start').textContent, '加入队列');
  const click = (run) => ui.get('active-runs').listeners.click({ target: { closest: () => ({ dataset: { stopPlugin: 'coverage', stopRun: run } }) } });
  await click('stale-run');
  assert.equal(ui.commands.length, 0);
  await click('active-run');
  assert.deepEqual(ui.commands[0], { action: 'stop', plugin_type: 'coverage', run_id: 'active-run' });
});

test('logs filters use API and an error preserves the previous tail', async () => {
  let failed = false;
  const ui = await createConsole({ logs: () => failed ? { error: 'log unavailable' } : {
    files: [{ name: 'scheduler.log', size: 1234 }], selected: 'scheduler.log', lines: ['[INFO] hello', '<script>literal</script>'], truncated: true,
  } });
  await ui.navigate('logs');
  ui.get('logs-level').value = 'ERROR'; ui.get('logs-search').value = 'run-7';
  await ui.get('logs-form').listeners.submit({ preventDefault() {} });
  assert.deepEqual(ui.logRequests.at(-1), { file: 'scheduler.log', limit: '200', search: 'run-7', level: 'ERROR' });
  assert.equal(ui.get('logs-output').textContent, '[INFO] hello\n<script>literal</script>');
  failed = true;
  await ui.get('logs-form').listeners.submit({ preventDefault() {} });
  assert.match(ui.get('logs-output').textContent, /hello/);
  assert.match(ui.get('logs-status').textContent, /保留上次内容/);
});

test('config drafts survive navigation and polling, save revision and masked secrets', async () => {
  const ui = await createConsole({ configs: { scheduler: {
    available: true, config: { server: { port: 5555, password: '***' } }, revision: 'original', fields: [], path: 'config/scheduler.json',
  } } });
  await ui.navigate('plugins');
  const edited = { server: { port: 5556, password: '***' } };
  ui.get('config-json').value = JSON.stringify(edited);
  ui.get('config-json').listeners.input();
  assert.equal(ui.get('config-reload').disabled, true);
  await ui.intervals[1].callback();
  await ui.navigate('results'); await ui.navigate('plugins');
  assert.deepEqual(JSON.parse(ui.get('config-json').value), edited);
  assert.equal(ui.configGets.length, 1);
  await ui.get('config-save').listeners.click();
  assert.deepEqual(ui.configSaves[0], { kind: 'scheduler', config: edited, revision: 'original' });
  assert.equal(ui.get('config-dirty').hidden, true);
  assert.match(ui.get('config-status').textContent, /重启/);
});

test('invalid config and conflicting save retain user draft', async () => {
  const ui = await createConsole({ configs: { scheduler: { available: true, config: { x: 1 }, revision: 'old', fields: [] } }, configError: 'configuration changed' });
  await ui.navigate('plugins');
  ui.get('config-json').value = '{bad';
  await ui.get('config-save').listeners.click();
  assert.equal(ui.configSaves.length, 0);
  assert.match(ui.get('config-status').textContent, /JSON 无效/);
  ui.get('config-json').value = '{"x":2}';
  await ui.get('config-save').listeners.click();
  assert.equal(ui.get('config-json').value, '{"x":2}');
  assert.match(ui.get('config-status').textContent, /configuration changed/);
  assert.equal(ui.get('config-discard').hidden, false);
});

test('failed config switch clears previous file content and preserves its draft', async () => {
  const ui = await createConsole({ configs: {
    scheduler: { available: true, config: { server: { port: 5555 } }, revision: 'old', fields: [], path: 'scheduler.json' },
    client: { error: 'client configuration unreadable' },
  } });
  await ui.navigate('plugins');
  ui.get('config-json').value = '{"server":{"port":5556}}';
  ui.get('config-json').listeners.input();
  ui.get('config-kind').value = 'client';
  await ui.get('config-kind').listeners.change();
  assert.equal(ui.get('config-json').value, '');
  assert.equal(ui.get('config-path').textContent, '');
  assert.equal(ui.get('config-save').disabled, true);
  assert.match(ui.get('config-status').textContent, /client configuration unreadable/);
  ui.get('config-kind').value = 'scheduler';
  await ui.get('config-kind').listeners.change();
  assert.equal(ui.get('config-json').value, '{"server":{"port":5556}}');
  assert.equal(ui.get('config-save').disabled, false);
});

test('configuration warnings stay visible after loading and saving', async () => {
  const ui = await createConsole({ configs: { scheduler: {
    available: true, config: { server: { port: 5555 } }, revision: 'original', fields: [],
    notice: '配置文件已读取', warnings: ['管理地址与结果服务配置不一致'],
  } } });
  await ui.navigate('plugins');
  assert.match(ui.get('config-notice').textContent, /管理地址与结果服务配置不一致/);
  ui.get('config-json').value = '{"server":{"port":5556}}';
  ui.get('config-json').listeners.input();
  await ui.get('config-save').listeners.click();
  assert.match(ui.get('config-notice').textContent, /管理地址与结果服务配置不一致/);
  assert.match(ui.get('config-notice').textContent, /重启/);
});

test('common service fields merge into the current draft without losing custom settings or masked secrets', async () => {
  const initial = {
    plugins: { coverage: { enabled: true, task_size: 25, future_option: { keep: ['a', 'b'] } } },
    db_conn: 'host=internal password=***',
    custom_service: { value: 'original' },
  };
  const ui = await createConsole({ configs: { scheduler: {
    available: true, config: initial, revision: 'common-fields-original', fields: [
      { path: 'plugins.coverage.task_size', label: '每任务文件数', type: 'number', description: '任务分组大小' },
      { path: 'plugins.coverage.enabled', label: '启用插件', type: 'boolean', description: '服务端执行插件' },
      { path: 'custom_service.value', label: '自定义选项', type: 'string', description: '' },
    ],
  } } });
  await ui.navigate('plugins');
  const draft = { ...initial, custom_service: { value: 'edited in advanced JSON' } };
  ui.get('config-json').value = JSON.stringify(draft);
  ui.get('config-json').listeners.input();
  assert.doesNotMatch(ui.get('config-form').innerHTML, /data-config-field="custom_service.value"/);
  const size = ui.get('config-common-0');
  size.dataset.configField = 'plugins.coverage.task_size';
  size.closest = () => size;
  size.value = '64';
  ui.get('config-form').listeners.input({ target: size });
  const enabled = ui.get('config-common-1');
  enabled.dataset.configField = 'plugins.coverage.enabled';
  enabled.closest = () => enabled;
  enabled.value = 'false';
  ui.get('config-form').listeners.change({ target: enabled });
  await ui.get('config-save').listeners.click();
  assert.deepEqual(ui.configSaves[0], {
    kind: 'scheduler', revision: 'common-fields-original', config: {
      ...draft, plugins: { coverage: { ...initial.plugins.coverage, task_size: 64, enabled: false } },
    },
  });
});

test('an incomplete numeric common setting blocks saving and keeps the last valid draft recoverable', async () => {
  const ui = await createConsole({ configs: { scheduler: {
    available: true, config: { plugins: { coverage: { task_size: 25 } }, server: { router_endpoint: 'tcp://*:5555' } }, revision: 'original',
    fields: [
      { path: 'plugins.coverage.task_size', label: '每任务文件数', type: 'number', description: '' },
      { path: 'server.router_endpoint', label: '任务服务地址', type: 'string', description: '' },
    ],
  } } });
  await ui.navigate('plugins');
  const size = ui.get('config-common-0');
  size.dataset.configField = 'plugins.coverage.task_size';
  size.closest = () => size;
  size.value = '64';
  ui.get('config-form').listeners.input({ target: size });
  size.value = '';
  ui.get('config-form').listeners.input({ target: size });
  assert.equal(ui.get('config-save').disabled, true);
  assert.match(ui.get('config-form-status').textContent, /需要填写数字/);
  const endpoint = ui.get('config-common-1');
  endpoint.dataset.configField = 'server.router_endpoint';
  endpoint.closest = () => endpoint;
  endpoint.value = 'tcp://*:5557';
  ui.get('config-form').listeners.input({ target: endpoint });
  assert.equal(ui.get('config-save').disabled, true);
  await ui.get('config-save').listeners.click();
  assert.equal(ui.configSaves.length, 0);
  assert.equal(JSON.parse(ui.get('config-json').value).plugins.coverage.task_size, 64);
  size.value = '32';
  ui.get('config-form').listeners.input({ target: size });
  await ui.get('config-save').listeners.click();
  assert.equal(ui.configSaves[0].config.plugins.coverage.task_size, 32);
});

test('history pages and searches all runs and preserves an older selection on refresh', async () => {
  const runs = Array.from({ length: 121 }, (_, index) => ({
    run_id: `history-${String(index).padStart(3, '0')}`, plugin_type: 'coverage',
    status: 'finished', expected_files: 1, success_files: 1, failed_files: 0,
  }));
  const ui = await createConsole({ databaseReady: true, runs });
  await ui.get('history-search-form').listeners.submit({ preventDefault() {} });
  assert.match(ui.get('history-table').innerHTML, /history-049/);
  assert.doesNotMatch(ui.get('history-table').innerHTML, /history-050/);
  await ui.get('history-next').listeners.click();
  assert.match(ui.get('history-table').innerHTML, /history-050/);
  assert.equal(ui.get('history-page').textContent, '第 2 页');
  ui.get('history-search').value = '120';
  await ui.get('history-search-form').listeners.submit({ preventDefault() {} });
  assert.match(ui.get('history-table').innerHTML, /history-120/);
  assert.equal(ui.get('history-next').disabled, true);
  await ui.get('history-table').listeners.click({ target: { closest: () => ({ dataset: { openRun: 'history-120', openPlugin: 'coverage' } }) } });
  assert.equal(ui.get('result-run').value, 'history-120');
  await ui.intervals[0].callback();
  assert.equal(ui.get('result-run').value, 'history-120');
  assert.match(ui.get('ai-context').textContent, /history-120/);
  await ui.get('refresh').listeners.click();
  assert.equal(ui.get('result-run').value, 'history-120');
});

test('direct run lookup opens an archived run outside the catalog', async () => {
  const ui = await createConsole({ databaseReady: true, runDetails: {
    archived: { run_id: 'archived', plugin_type: 'coverage', expected_files: 1, outcomes: { success: 1 } },
  } });
  ui.get('open-run-id').value = 'archived';
  await ui.get('open-run-form').listeners.submit({ preventDefault() {} });
  assert.equal(ui.get('result-run').value, 'archived');
  assert.match(ui.get('result-run').innerHTML, /archived/);
  await ui.intervals[0].callback();
  assert.equal(ui.get('result-run').value, 'archived');
});

test('automatic polling cannot replace an explicit archived selection while loading', async () => {
  let releaseDetail;
  const detailGate = new Promise((resolve) => { releaseDetail = resolve; });
  const ui = await createConsole({ databaseReady: true, detailGate,
    runs: [{ run_id: 'current', plugin_type: 'coverage' }],
    runDetails: { archived: { run_id: 'archived', plugin_type: 'coverage' } },
  });
  ui.get('open-run-id').value = 'archived';
  const opening = ui.get('open-run-form').listeners.submit({ preventDefault() {} });
  await new Promise(setImmediate);
  await ui.intervals[0].callback();
  releaseDetail();
  await opening;
  assert.equal(ui.get('result-run').value, 'archived');
  assert.match(ui.get('ai-context').textContent, /archived/);
});

test('DSH native interface starts automatically without a database run or client', async () => {
  const ui = await createConsole();
  const operatorId = ui.storage.get('testtracking.ai.operator-id.v1');
  assert.equal(ui.get('dsh-native-frame').src, `/ai/dsh/${operatorId}/`);
  assert.deepEqual(ui.posts, [{ dshWeb: true, operator_id: operatorId }]);
  assert.equal(ui.get('dsh-native-open').disabled, false);
  await ui.get('refresh').listeners.click();
  await ui.intervals[0].callback();
  assert.equal(ui.posts.length, 1, 'refreshing task state must not restart the native session');
});

test('browser operator identity persists and separate browsers receive independent DSH sessions', async () => {
  const operatorId = '123e4567-e89b-42d3-a456-426614174001';
  const ui = await createConsole({ storedOperatorId: operatorId });
  const other = await createConsole();
  assert.equal(ui.posts[0].operator_id, operatorId);
  assert.equal(ui.storage.get('testtracking.ai.operator-id.v1'), operatorId);
  assert.notEqual(other.posts[0].operator_id, operatorId);
  assert.notEqual(other.get('dsh-native-frame').src, ui.get('dsh-native-frame').src);
});

test('database query failure does not block the DSH native interface', async () => {
  const ui = await createConsole({ databaseReady: true, runsError: true });
  assert.equal(ui.get('footer-status').textContent, 'temporary query failure');
  assert.match(ui.get('dsh-native-frame').src, /^\/ai\/dsh\//);
  assert.equal(ui.get('dsh-native-open').disabled, false);
});

test('a closed sidebar starts DSH only when opened and does not reload it after reopening', async () => {
  const ui = await createConsole({ sidebarOpen: false });
  assert.equal(ui.posts.length, 0);
  ui.get('ai-toggle').listeners.click();
  await new Promise(setImmediate);
  assert.equal(ui.posts.length, 1);
  const url = ui.get('dsh-native-frame').src;
  ui.get('ai-close').listeners.click();
  ui.get('ai-toggle').listeners.click();
  await new Promise(setImmediate);
  assert.equal(ui.posts.length, 1);
  assert.equal(ui.get('dsh-native-frame').src, url);
});

test('unavailable DSH remains explicitly retryable without waiting for another provider scan', async () => {
  const ui = await createConsole({ available: null });
  assert.equal(ui.posts.length, 0);
  assert.equal(ui.get('dsh-native-open').disabled, false);
  await ui.get('dsh-native-open').listeners.click();
  assert.match(ui.get('dsh-native-frame').src, /^\/ai\/dsh\//);
  assert.equal(ui.posts.length, 1);
});

test('native startup failure is visible and retries only on explicit action', async () => {
  let failure = 'DSH web could not start';
  const ui = await createConsole({ nativeFailure: () => failure });
  assert.match(ui.get('dsh-native-status').textContent, /DSH web could not start/);
  assert.equal(ui.get('dsh-native-open').disabled, false);
  await ui.get('refresh').listeners.click();
  await ui.intervals[0].callback();
  assert.equal(ui.posts.length, 1);
  failure = null;
  await ui.get('dsh-native-open').listeners.click();
  assert.equal(ui.posts.length, 2);
  assert.match(ui.get('dsh-native-frame').src, /^\/ai\/dsh\//);
});

test('pending native startup prevents duplicate start requests', async () => {
  let release;
  const nativeGate = new Promise((resolve) => { release = resolve; });
  const ui = await createConsole({ nativeGate });
  try {
    assert.equal(ui.posts.length, 1);
    assert.equal(ui.get('dsh-native-open').disabled, true);
    await ui.get('dsh-native-open').listeners.click();
    await ui.get('refresh').listeners.click();
    assert.equal(ui.posts.length, 1);
  } finally { release(); }
  await new Promise(setImmediate);
  assert.equal(ui.get('dsh-native-open').disabled, false);
  assert.match(ui.get('dsh-native-frame').src, /^\/ai\/dsh\//);
});

test('changing result selection retains the running native DSH session', async () => {
  const ui = await createConsole();
  const url = ui.get('dsh-native-frame').src;
  ui.get('result-run').value = '';
  await ui.get('result-run').listeners.change();
  assert.equal(ui.get('dsh-native-frame').src, url);
  assert.equal(ui.posts.length, 1);
});

test('assistant markup exposes only the native DSH interface', () => {
  const html = fs.readFileSync(path.join(__dirname, '../result_service/web/index.html'), 'utf8');
  assert.match(html, /id="dsh-native-frame"/);
  assert.doesNotMatch(html, /codex|id="(?:chat-form|question|send-button|ai-chat-panel|ai-tab-chat|new-conversation|report-candidate-index)"/i);
  assert.doesNotMatch(script, /\/api\/ai\/chat|data-ai-tab|testtracking\.ai\.conversations/);
});

test('context can be prepared without a selected run or an available DSH command', async () => {
  const ui = await createConsole({ available: null });
  ui.get('dsh-context-current').listeners.click();
  const prompt = ui.get('dsh-context-prompt').value;
  const context = JSON.parse(prompt.slice(prompt.indexOf('{')));
  assert.equal(context.run, null);
  assert.equal(context.plugin, 'coverage');
  assert.match(prompt, /未选择运行/);
  assert.equal(ui.get('dsh-context-section').open, true);
  assert.equal(ui.get('dsh-context-copy').disabled, false);
  assert.equal(ui.clipboardWrites.length, 0);
  assert.equal(ui.posts.length, 0);
});

test('LAN browsers without clipboard permission retain selectable context for manual copying', async () => {
  for (const options of [{ clipboardAvailable: false }, { clipboardFailure: true }]) {
    const ui = await createConsole(options);
    ui.get('dsh-context-current').listeners.click();
    const prompt = ui.get('dsh-context-prompt').value;
    await ui.get('dsh-context-copy').listeners.click();
    assert.equal(ui.get('dsh-context-prompt').value, prompt);
    assert.equal(ui.get('dsh-context-prompt').selected, true);
    assert.match(ui.get('dsh-context-status').textContent, /手动复制/);
    assert.equal(ui.clipboardWrites.length, 0);
  }
});

test('persisted panel width is clamped and dragging works in both directions', async () => {
  const ui = await createConsole({ viewportWidth: 3840, storedPanelWidth: 3400 });
  const resize = ui.get('ai-resize');
  assert.equal(ui.get('console').style.properties['--assistant-width'], '1600px');

  resize.listeners.pointerdown({
    clientX: 1000, pointerId: 7, currentTarget: resize, preventDefault() {},
  });
  assert.equal(resize.capturedPointer, 7);
  ui.windowListeners.pointermove({ clientX: 2000 });
  assert.equal(ui.get('console').style.properties['--assistant-width'], '600px');
  ui.windowListeners.pointermove({ clientX: 2300 });
  assert.equal(ui.get('console').style.properties['--assistant-width'], '360px');
  ui.windowListeners.pointerup();
  assert.equal(ui.storage.get('testtracking.ai.panel-width.v1'), '360');
});

test('cancelled resize clears drag state and preserves the preferred width across breakpoints', async () => {
  const ui = await createConsole({ viewportWidth: 1024, storedPanelWidth: 900 });
  const resize = ui.get('ai-resize');
  assert.equal(ui.get('console').style.properties['--assistant-width'], '900px');

  resize.listeners.pointerdown({
    clientX: 400, pointerId: 9, currentTarget: resize, preventDefault() {},
  });
  ui.windowListeners.pointercancel();
  ui.windowListeners.pointermove({ clientX: 100 });
  assert.equal(ui.get('console').style.properties['--assistant-width'], '900px');

  ui.window.innerWidth = 1025;
  ui.windowListeners.resize();
  assert.equal(ui.get('console').style.properties['--assistant-width'], '381px');
  ui.window.innerWidth = 1024;
  ui.windowListeners.resize();
  assert.equal(ui.get('console').style.properties['--assistant-width'], '900px');
});

test('desktop AI resizing leaves an operable main column at common resolutions', async () => {
  for (const viewportWidth of [1025, 1280, 1366, 1440, 1920]) {
    const ui = await createConsole({ viewportWidth, storedPanelWidth: 1200 });
    const sidebar = viewportWidth <= 1440 ? 164 : 196;
    const panel = () => Number(ui.get('console').style.properties['--assistant-width'].replace('px', ''));
    assert.ok(viewportWidth - sidebar - panel() >= 480, `${viewportWidth}px leaves at least 480px for main`);
    const before = panel();
    ui.get('ai-resize').listeners.keydown({ key: 'ArrowRight', preventDefault() {} });
    assert.ok(panel() < before);
    ui.get('ai-resize').listeners.keydown({ key: 'ArrowLeft', preventDefault() {} });
    assert.equal(panel(), before);
    ui.get('ai-expand').listeners.click();
    assert.ok(viewportWidth - sidebar - panel() >= 480);
  }
});

test('text scale applies to the console and embedded DSH interface', async () => {
  const ui = await createConsole({ storedUiScale: 1.2 });
  assert.equal(ui.documentElement.style.fontSize, '19.2px');
  assert.equal(ui.get('dsh-native-frame').style.transform, 'scale(1.2)');

  ui.get('text-larger').listeners.click();
  assert.equal(ui.documentElement.style.fontSize, '20.8px');
  assert.equal(ui.get('dsh-native-frame').style.transform, 'scale(1.3)');
  assert.equal(ui.storage.get('testtracking.ui.scale.v1'), '1.3');
});

test('overview run card shows persisted task progress and each task state', async () => {
  const run = {
    run_id: 'coverage-run-42',
    plugin_type: 'coverage',
    status: 'running',
    dataset_version: '2026.09',
    total_tasks: 20,
    task_status_counts: {
      pending: 8,
      dispatched: 1,
      acked: 2,
      reclaimed: 2,
      reported: 7,
    },
  };
  const ui = await createConsole({ databaseReady: true, runs: [run] });
  const html = ui.get('run-cards').innerHTML;

  assert.match(html, /任务处理进度/);
  assert.match(html, /7 \/ 20 · 35%/);
  assert.match(html, /待派发 <b>8<\/b>/);
  assert.match(html, /处理中 <b>3<\/b>/);
  assert.match(html, /重试等待 <b>2<\/b>/);
  assert.match(html, /已完成 <b>7<\/b>/);
  assert.match(html, /style="width:35%"/);
});

test('overview filters are generated from discovered plugins', async () => {
  const ui = await createConsole({
    pluginMetadata: [
      { name: 'coverage', configured: true, view_editable: true, view: coverageView },
      { name: 'custom-plugin', configured: false, view_editable: true, view: { ...coverageView, plugin: 'custom-plugin' } },
    ],
    databaseReady: true,
    runs: [
      { run_id: 'coverage-run', plugin_type: 'coverage', task_status_counts: {} },
      { run_id: 'custom-run', plugin_type: 'custom-plugin', task_status_counts: {} },
    ],
  });

  const html = ui.get('overview-filter').innerHTML;
  assert.match(html, /data-filter="coverage"/);
  assert.match(html, /data-filter="custom-plugin"/);

  const customButton = element();
  customButton.dataset.filter = 'custom-plugin';
  ui.get('overview-filter').listeners.click({ target: { closest: () => customButton } });
  assert.match(ui.get('run-cards').innerHTML, /custom-run/);
  assert.doesNotMatch(ui.get('run-cards').innerHTML, /coverage-run/);
});

test('view design prepares the definition and schema for native DSH without saving or sending a chat', async () => {
  const ui = await createConsole({ sidebarOpen: false });
  await ui.get('ai-design-view').listeners.click();
  await new Promise(setImmediate);
  const prompt = ui.get('dsh-context-prompt').value;
  assert.match(prompt, /编辑展示/);
  const context = JSON.parse(prompt.slice(prompt.indexOf('{')));
  assert.equal(context.operator_id, ui.storage.get('testtracking.ai.operator-id.v1'));
  assert.equal(context.plugin, 'coverage');
  assert.deepEqual(context.current_view, coverageView);
  assert.deepEqual(context.view_schema, pluginViewSchema);
  assert.equal(ui.get('console').dataset.aiOpen, 'true');
  assert.equal(ui.get('dsh-context-section').open, true);
  assert.equal(ui.clipboardWrites.length, 0, 'opening the native assistant must not write the clipboard');
  assert.equal(ui.puts.length, 0);
  assert.ok(ui.posts.every((post) => post.dshWeb));
  assert.ok(!ui.requests.includes('/api/ai/chat'));
});

test('manual JSON editor saves through PUT and immediately rerenders the plugin view', async () => {
  const ui = await createConsole();
  ui.get('edit-plugin-view').listeners.click();
  assert.equal(ui.get('view-editor-dialog').open, true);
  assert.deepEqual(JSON.parse(ui.get('view-editor-text').value), coverageView);

  const editedView = {
    ...coverageView,
    title: '手工调整的覆盖率结果',
    widgets: [{
      id: 'manual-summary',
      type: 'metrics',
      title: '手工摘要',
      items: [{ label: '总文件', value: 'analysis.total_files', format: 'integer' }],
    }],
  };
  ui.get('view-editor-text').value = JSON.stringify(editedView);
  await ui.get('save-view-editor').listeners.click();

  assert.equal(ui.puts.length, 1);
  assert.equal(ui.puts[0].title, '手工调整的覆盖率结果');
  assert.equal(ui.puts[0].revision, '1'.repeat(64));
  assert.equal(ui.get('view-editor-dialog').open, false);
  assert.equal(ui.get('plugin-view-title').textContent, '手工调整的覆盖率结果');
  assert.match(ui.get('plugin-result-view').innerHTML, /手工摘要/);
});

test('manual view draft retains its original revision across refresh and conflict', async () => {
  let currentView = { ...coverageView, revision: '1'.repeat(64) };
  const ui = await createConsole({ pluginMetadata: () => [{
    name: 'coverage', configured: true, view_editable: true, view: currentView,
  }] });
  ui.get('edit-plugin-view').listeners.click();
  const draft = JSON.stringify({ ...coverageView, title: 'My unsaved draft' });
  ui.get('view-editor-text').value = draft;

  currentView = { ...coverageView, title: 'Other operator saved', revision: '2'.repeat(64) };
  await ui.get('refresh').listeners.click();
  assert.equal(ui.get('plugin-view-title').textContent, currentView.title);
  assert.equal(ui.get('view-editor-text').value, draft);
  await ui.get('save-view-editor').listeners.click();

  assert.equal(ui.puts[0].revision, '1'.repeat(64));
  assert.equal(ui.get('view-editor-dialog').open, true);
  assert.equal(ui.get('view-editor-text').value, draft);
  assert.match(ui.get('view-editor-status').textContent, /其他使用者修改/);
  assert.equal(ui.get('plugin-view-title').textContent, currentView.title);
  await ui.get('save-view-editor').listeners.click();
  assert.equal(ui.puts[1].revision, '1'.repeat(64), 'retry must not silently rebase the draft');
});

test('primary metric can use common run data before plugin analysis is ready', async () => {
  const run = {
    run_id: 'coverage-run-12', plugin_type: 'coverage', status: 'running',
    expected_files: 12, total_tasks: 20, task_status_counts: { pending: 20 },
    analysis: {}, analysis_status: 'pending', result_completeness: {}, outcomes: {},
  };
  const ui = await createConsole({ databaseReady: true, runs: [run] });
  ui.get('edit-plugin-view').listeners.click();
  const editedView = {
    ...coverageView,
    primary_metric: { label: '计划文件', value: 'run.expected_files', format: 'integer' },
  };
  ui.get('view-editor-text').value = JSON.stringify(editedView);

  await ui.get('save-view-editor').listeners.click();

  assert.equal(ui.get('analysis-value').textContent, '12');
});

test('progress refresh updates running task counts every five seconds', async () => {
  let reported = 2;
  const runs = () => [{
    run_id: 'coverage-run-live', plugin_type: 'coverage', status: 'running',
    total_tasks: 10,
    task_status_counts: { pending: 8 - reported, dispatched: 2, reported },
  }];
  const ui = await createConsole({ databaseReady: true, runs });
  assert.equal(ui.intervals[0].milliseconds, 5000);
  assert.match(ui.get('run-cards').innerHTML, /2 \/ 10 · 20%/);

  reported = 5;
  await ui.intervals[0].callback();

  assert.match(ui.get('run-cards').innerHTML, /5 \/ 10 · 50%/);
});

test('progress refresh keeps the current run context while refreshed details are pending', async () => {
  let releaseDetail;
  const detailGate = new Promise((resolve) => { releaseDetail = resolve; });
  const run = {
    run_id: 'coverage-run-context', plugin_type: 'coverage', status: 'running',
    total_tasks: 10, task_status_counts: { pending: 8, reported: 2 },
    analysis: { percent: 0.25 }, analysis_status: 'ready',
    result_completeness: {}, outcomes: {},
  };
  const ui = await createConsole({ databaseReady: true, runs: [run], detailGate });
  assert.match(ui.get('ai-context').textContent, /coverage-run-context/);
  assert.equal(ui.get('analysis-value').textContent, '25.0%');

  const pending = ui.intervals[0].callback();
  await new Promise(setImmediate);

  assert.match(ui.get('ai-context').textContent, /coverage-run-context/);
  assert.equal(ui.get('analysis-value').textContent, '25.0%');

  releaseDetail();
  await pending;
});

const reportRun = {
  run_id: 'durable-run', plugin_type: 'coverage', status: 'planned',
  analysis_status: 'pending', analysis: {},
  result_readiness: { status: 'partial', can_analyze: true, expected_files: 10, reported_files: 4, successful_files: 3, failed_files: 1 },
};

test('report uses database catalog runs and can run before summary postprocessing', async () => {
  const ui = await createConsole({
    databaseReady: true,
    runs: [],
    catalogs: { coverage: { runs: [reportRun], tables: [], script_contract: {} } },
  });
  assert.equal(ui.get('result-run').value, 'durable-run');
  assert.equal(ui.get('execution-status').textContent, '部分入库');
  assert.equal(ui.get('analysis-state').textContent, '已有部分结果，可分析');
  assert.equal(ui.get('run-report').disabled, false);
  await ui.get('run-report').listeners.click();
  assert.deepEqual(ui.reportPosts, [{ plugin: 'coverage', run_id: 'durable-run', parameters: {} }]);
  assert.equal(ui.get('report-frame').hidden, false);
  assert.equal(ui.get('report-frame').src, '/reports/durable-run/report-1.html');
  assert.equal(ui.get('report-empty').hidden, true);
});

test('unavailable scheduler overview does not prevent database reports', async () => {
  const ui = await createConsole({
    databaseReady: true, runsError: true,
    catalogs: { coverage: { runs: [reportRun], tables: [], script_contract: {} } },
  });
  assert.equal(ui.get('result-run').value, 'durable-run');
  assert.equal(ui.get('run-report').disabled, false);
  await ui.get('run-report').listeners.click();
  await ui.intervals[0].callback();
  assert.equal(ui.get('report-frame').hidden, false);
  assert.equal(ui.get('result-run').value, 'durable-run');
  assert.match(ui.get('footer-status').textContent, /任务总览刷新失败/);
});

test('Python editor saves a replacement script and runs the selected result', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun] });
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-dialog').open, true);
  assert.match(ui.get('report-editor-text').value, /def render/);
  ui.get('report-editor-text').value = 'def render(connection, context):\n    return "<h1>Changed</h1>"';
  await ui.get('save-run-report-editor').listeners.click();
  assert.deepEqual(ui.scriptPuts, [{ plugin: 'coverage', source: ui.get('report-editor-text').value }]);
  assert.equal(ui.reportPosts.length, 1);
  assert.equal(ui.get('report-editor-dialog').open, false);
  assert.equal(ui.get('report-frame').hidden, false);
});

test('report design opens native DSH with a copyable script contract and database snapshot', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun], sidebarOpen: false });
  await ui.get('ai-design-report').listeners.click();
  await new Promise(setImmediate);
  const prompt = ui.get('dsh-context-prompt').value;
  const context = JSON.parse(prompt.slice(prompt.indexOf('{')));
  assert.equal(context.plugin, 'coverage');
  assert.equal(context.operator_id, ui.storage.get('testtracking.ai.operator-id.v1'));
  assert.equal(context.run.run_id, reportRun.run_id);
  assert.match(context.current_script, /def render/);
  assert.equal(context.script_contract.entrypoint, 'render(connection, context)');
  assert.equal(context.analysis_catalog.tables[0].name, 'file_results');
  assert.match(prompt, /只依赖数据库/);
  assert.match(prompt, /不依赖外网/);
  assert.match(prompt, /最新 source 和 revision/);
  assert.equal(ui.get('console').dataset.aiOpen, 'true');
  assert.match(ui.get('dsh-native-frame').src, /^\/ai\/dsh\//);
  assert.equal(ui.clipboardWrites.length, 0);
  assert.equal(ui.scriptPuts.length, 0);
  assert.equal(ui.reportPosts.length, 0);
  assert.ok(!ui.requests.includes('/api/ai/chat'));

  await ui.get('dsh-context-copy').listeners.click();
  assert.deepEqual(ui.clipboardWrites, [prompt]);
  assert.match(ui.get('dsh-context-status').textContent, /已复制/);
});

test('changing the selected run preserves the prepared DSH snapshot until explicitly refreshed', async () => {
  const nextRun = { ...reportRun, run_id: 'next-context-run' };
  const ui = await createConsole({ databaseReady: true, runs: [reportRun, nextRun] });
  await ui.get('edit-report-script').listeners.click();
  ui.get('report-editor-text').value = '# my unsaved report';
  ui.get('report-editor-text').listeners.input();
  ui.get('close-report-editor').listeners.click();
  await ui.get('ai-design-report').listeners.click();
  const original = ui.get('dsh-context-prompt').value;
  ui.get('result-run').value = nextRun.run_id;
  await ui.get('result-run').listeners.change();
  assert.equal(ui.get('dsh-context-prompt').value, original);
  assert.match(ui.get('dsh-context-status').textContent, /durable-run/);
  ui.get('dsh-context-current').listeners.click();
  const updated = ui.get('dsh-context-prompt').value;
  assert.equal(JSON.parse(updated.slice(updated.indexOf('{'))).run.run_id, nextRun.run_id);
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# my unsaved report');
  assert.equal(ui.scriptPuts.length, 0);
  assert.equal(ui.reportPosts.length, 0);
  assert.equal(ui.posts.length, 1, 'context changes must retain the native DSH conversation');
});

test('manual script validation rejection retains visible edits and a previously successful report', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun],
    scriptSaveFailure: 'render(connection, context) is required' });
  await ui.get('run-report').listeners.click();
  const previousUrl = ui.get('report-frame').src;
  await ui.get('edit-report-script').listeners.click();
  ui.get('report-editor-text').value = 'print("user edit")';
  await ui.get('save-run-report-editor').listeners.click();
  assert.equal(ui.get('report-editor-text').value, 'print("user edit")');
  assert.match(ui.get('report-editor-status').textContent, /render\(connection, context\) is required/);
  assert.equal(ui.get('report-editor-dialog').open, true);
  assert.equal(ui.get('save-report-editor').disabled, false);
  assert.equal(ui.reportPosts.length, 1, 'a rejected save does not start a new report');
  assert.equal(ui.get('report-frame').src, previousUrl);
  assert.equal(ui.get('report-frame').hidden, false);
});

test('failed report rerun retains the last successful report', async () => {
  let fail = false;
  const ui = await createConsole({ databaseReady: true, runs: [reportRun], reportFailure: () => fail ? 'SQL syntax error' : null });
  await ui.get('run-report').listeners.click();
  const url = ui.get('report-frame').src;
  fail = true;
  await ui.get('run-report').listeners.click();
  assert.equal(ui.get('report-frame').src, url);
  assert.equal(ui.get('report-frame').hidden, false);
  assert.match(ui.get('report-state').textContent, /SQL syntax error.*保留上一次/);
  assert.equal(ui.get('run-report').disabled, false);
  const failureStatus = ui.get('report-state').textContent;
  await ui.intervals[0].callback();
  assert.equal(ui.get('report-state').textContent, failureStatus, 'polling the same report preserves the latest execution failure');
});

test('opening the page restores the persisted latest report', async () => {
  const report = { plugin: 'coverage', run_id: reportRun.run_id, report_id: 'restored', url: '/reports/restored/index.html' };
  const ui = await createConsole({ databaseReady: true, runs: [reportRun], latestReports: { [reportRun.run_id]: report } });
  assert.equal(ui.get('report-frame').src, '/reports/restored/index.html');
  assert.equal(ui.get('report-frame').hidden, false);
  assert.equal(ui.get('report-empty').hidden, true);
  assert.equal(ui.reportPosts.length, 0);
  await ui.get('refresh').listeners.click();
  assert.equal(ui.get('report-frame').src, '/reports/restored/index.html');
  assert.equal(ui.get('report-frame').hidden, false);
});

test('polling displays reports generated externally by the native AI client', async () => {
  let latest = null;
  const ui = await createConsole({ databaseReady: true, runs: [reportRun], latestReports: () => latest });
  assert.equal(ui.get('report-frame').hidden, true);
  latest = { plugin: 'coverage', run_id: reportRun.run_id, report_id: 'external', url: '/reports/external/index.html' };
  await ui.intervals[0].callback();
  assert.equal(ui.get('report-frame').src, '/reports/external/index.html');
  assert.equal(ui.get('report-frame').hidden, false);
  assert.equal(ui.reportPosts.length, 0);
});

test('opening script editor loads external changes while retaining an unsaved draft', async () => {
  let source = '# original';
  const ui = await createConsole({ databaseReady: true, runs: [reportRun], reportSource: () => source });
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# original');
  ui.get('close-report-editor').listeners.click();
  source = '# changed by native DSH';
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# changed by native DSH');
  ui.get('report-editor-text').value = '# my unsaved draft';
  ui.get('report-editor-text').listeners.input();
  ui.get('close-report-editor').listeners.click();
  source = '# a newer server script';
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# my unsaved draft');
});

test('manual report drafts keep their original revision across reopening or refresh and retain edits on conflict', async () => {
  for (const refreshMode of ['reopen', 'refresh']) {
    let source = 'def render(connection, context):\n    return "original"';
    let revision = 'revision-1';
    const ui = await createConsole({ databaseReady: true, runs: [reportRun],
      reportSource: () => source, reportRevision: () => revision });
    await ui.get('edit-report-script').listeners.click();
    const draft = 'def render(connection, context):\n    return "my unsaved changes"';
    ui.get('report-editor-text').value = draft;
    ui.get('report-editor-text').listeners.input();
    if (refreshMode === 'reopen') ui.get('close-report-editor').listeners.click();

    source = 'def render(connection, context):\n    return "updated by native DSH"';
    revision = 'revision-2';
    if (refreshMode === 'reopen') await ui.get('edit-report-script').listeners.click();
    else await ui.get('refresh').listeners.click();
    assert.equal(ui.get('report-editor-text').value, draft);
    await ui.get('save-run-report-editor').listeners.click();
    assert.equal(ui.scriptSaveRequests[0].revision, 'revision-1', refreshMode);
    assert.equal(ui.scriptSaveRequests[0].operator_id, ui.storage.get('testtracking.ai.operator-id.v1'));
    assert.equal(ui.get('report-editor-dialog').open, true);
    assert.equal(ui.get('report-editor-text').value, draft);
    assert.match(ui.get('report-editor-status').textContent, /新版本.*原草稿已保留/);
    assert.equal(ui.get('report-load-latest').hidden, false);
    assert.equal(ui.reportPosts.length, 0, 'a conflicted save must not generate a report');
    await ui.get('save-report-editor').listeners.click();
    assert.equal(ui.scriptSaveRequests[1].revision, 'revision-1', 'retry must not silently rebase the unchanged draft');
    ui.get('close-report-editor').listeners.click();
    await ui.get('edit-report-script').listeners.click();
    assert.equal(ui.get('report-editor-text').value, draft);
  }
});

test('successful manual report saves establish the revision for the next edit', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun] });
  await ui.get('edit-report-script').listeners.click();
  ui.get('report-editor-text').value = 'def render(connection, context):\n    return "first edit"';
  ui.get('report-editor-text').listeners.input();
  await ui.get('save-report-editor').listeners.click();
  assert.equal(ui.scriptSaveRequests[0].revision, 'revision-1');
  ui.get('report-editor-text').value = 'def render(connection, context):\n    return "second edit"';
  ui.get('report-editor-text').listeners.input();
  await ui.get('save-run-report-editor').listeners.click();
  assert.equal(ui.scriptSaveRequests[1].revision, 'revision-2');
  assert.equal(ui.reportPosts.length, 1);
  assert.equal(ui.get('report-editor-dialog').open, false);
});

test('a conflicted report can explicitly load the latest revision while keeping the old draft for merging', async () => {
  let source = 'def render(connection, context):\n    return "original"';
  let revision = 'revision-1';
  const ui = await createConsole({ databaseReady: true, runs: [reportRun],
    reportSource: () => source, reportRevision: () => revision });
  await ui.get('edit-report-script').listeners.click();
  const draft = 'def render(connection, context):\n    return "my unsaved changes"';
  ui.get('report-editor-text').value = draft;
  ui.get('report-editor-text').listeners.input();
  source = 'def render(connection, context):\n    return "native DSH changes"';
  revision = 'revision-2';
  await ui.get('save-report-editor').listeners.click();
  assert.equal(ui.get('report-load-latest').hidden, false);

  await ui.get('report-load-latest').listeners.click();
  assert.equal(ui.get('report-editor-text').value, source);
  assert.equal(ui.get('report-conflict-draft').value, draft);
  assert.equal(ui.get('report-conflict-section').hidden, false);
  assert.equal(ui.scriptSaveRequests.length, 1, 'loading a new base must not silently save the old draft');
  const merged = 'def render(connection, context):\n    return "native DSH changes and my changes"';
  ui.get('report-editor-text').value = merged;
  ui.get('report-editor-text').listeners.input();
  await ui.get('save-run-report-editor').listeners.click();
  assert.equal(ui.scriptSaveRequests[1].revision, 'revision-2');
  assert.equal(ui.scriptSaveRequests[1].source, merged);
  assert.equal(ui.reportPosts.length, 1);
  assert.equal(ui.get('report-editor-dialog').open, false);
});

test('failed conflict recovery keeps the report draft and its original revision retryable', async () => {
  let revision = 'revision-1';
  let readError = null;
  let waitForRead = false;
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const ui = await createConsole({ databaseReady: true, runs: [reportRun],
    reportRevision: () => revision, scriptReadFailure: () => readError,
    scriptGate: () => waitForRead ? gate : Promise.resolve() });
  await ui.get('edit-report-script').listeners.click();
  const draft = 'def render(connection, context):\n    return "keep my draft"';
  ui.get('report-editor-text').value = draft;
  ui.get('report-editor-text').listeners.input();
  revision = 'revision-2';
  await ui.get('save-report-editor').listeners.click();
  readError = 'temporary script read failure';
  waitForRead = true;
  const recovery = ui.get('report-load-latest').listeners.click();
  await new Promise(setImmediate);
  try {
    assert.equal(ui.get('report-editor-text').disabled, true);
    assert.equal(ui.get('save-report-editor').disabled, true);
    assert.equal(ui.get('save-run-report-editor').disabled, true);
  } finally { release(); }
  await recovery;
  assert.equal(ui.get('report-editor-text').value, draft);
  assert.match(ui.get('report-editor-status').textContent, /temporary script read failure/);
  assert.equal(ui.get('report-editor-text').disabled, false);
  assert.equal(ui.get('report-load-latest').hidden, false);
  assert.equal(ui.get('report-load-latest').disabled, false);
  await ui.get('save-report-editor').listeners.click();
  assert.equal(ui.scriptSaveRequests[1].revision, 'revision-1');
  assert.equal(ui.get('report-editor-text').value, draft);
  assert.equal(ui.reportPosts.length, 0);
});

test('typing during a delayed script reload preserves the draft base when DSH saves another revision', async () => {
  let source = 'def render(connection, context):\n    return "original"';
  let revision = 'revision-1';
  let scriptReads = 0;
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const ui = await createConsole({ databaseReady: true, runs: [reportRun],
    reportSource: () => source, reportRevision: () => revision,
    scriptGate: () => ++scriptReads > 1 ? gate : Promise.resolve() });
  const opening = ui.get('edit-report-script').listeners.click();
  await new Promise(setImmediate);
  const draft = 'def render(connection, context):\n    return "typed while loading"';
  ui.get('report-editor-text').value = draft;
  ui.get('report-editor-text').listeners.input();
  source = 'def render(connection, context):\n    return "updated by native DSH"';
  revision = 'revision-2';
  release();
  await opening;
  assert.equal(ui.get('report-editor-text').value, draft);
  await ui.get('save-report-editor').listeners.click();
  assert.equal(ui.scriptSaveRequests[0].revision, 'revision-1');
  assert.match(ui.get('report-editor-status').textContent, /新版本.*原草稿已保留/);
  assert.equal(ui.get('report-load-latest').hidden, false);
  assert.equal(ui.get('report-editor-text').value, draft);
});

test('changing run hides the old report and ignores an earlier pending response', async () => {
  let releaseReport;
  const reportGate = new Promise((resolve) => { releaseReport = resolve; });
  const nextRun = { ...reportRun, run_id: 'next-run' };
  const ui = await createConsole({ databaseReady: true, runs: [reportRun, nextRun], reportGate });
  const pending = ui.get('run-report').listeners.click();
  await new Promise(setImmediate);
  ui.get('result-run').value = 'next-run';
  await ui.get('result-run').listeners.change();
  assert.equal(ui.get('report-frame').hidden, true);
  releaseReport();
  await pending;
  assert.equal(ui.get('report-frame').hidden, true);
  assert.equal(ui.get('report-frame').src, 'about:blank');
  assert.equal(ui.get('run-report').disabled, false);
});

test('progress polling preserves a successful report and Python draft', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun] });
  await ui.get('run-report').listeners.click();
  await ui.get('edit-report-script').listeners.click();
  ui.get('report-editor-text').value = '# unsaved user draft';
  ui.get('report-editor-text').listeners.input();
  await ui.intervals[0].callback();
  assert.equal(ui.get('report-frame').hidden, false);
  assert.equal(ui.get('report-editor-text').value, '# unsaved user draft');
  ui.get('close-report-editor').listeners.click();
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# unsaved user draft');
});

test('late script response from an earlier plugin cannot replace the current editor', async () => {
  let releaseCustom;
  const gate = new Promise((resolve) => { releaseCustom = resolve; });
  const customRun = { ...reportRun, run_id: 'custom-run', plugin_type: 'custom' };
  const ui = await createConsole({
    databaseReady: true,
    runs: [reportRun, customRun],
    pluginMetadata: [
      { name: 'coverage', configured: true, view: coverageView },
      { name: 'custom', configured: true, view: { ...coverageView, plugin: 'custom' } },
    ],
    reportSource: (plugin) => `# ${plugin} script`,
    scriptGate: (plugin) => plugin === 'custom' ? gate : Promise.resolve(),
  });
  ui.get('result-type').value = 'custom';
  const previousSwitch = ui.get('result-type').listeners.change();
  await new Promise(setImmediate);
  ui.get('result-type').value = 'coverage';
  await ui.get('result-type').listeners.change();
  await ui.get('edit-report-script').listeners.click();
  releaseCustom();
  await previousSwitch;
  assert.equal(ui.get('result-type').value, 'coverage');
  assert.equal(ui.get('result-run').value, 'durable-run');
  assert.equal(ui.get('report-editor-text').value, '# coverage script');
});

test('report frame enables interaction without access to the host page', () => {
  const html = fs.readFileSync(path.join(__dirname, '../result_service/web/index.html'), 'utf8');
  assert.match(html, /id="report-frame"[^>]*sandbox="allow-scripts allow-downloads"/);
});
