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
// properties and event hooks exercised by initialization and the chat form.
function element() {
  const style = {
    properties: {},
    setProperty(name, value) { this.properties[name] = String(value); },
  };
  return {
    value: '', disabled: false, hidden: false, textContent: '', innerHTML: '',
    style, dataset: {}, children: [], listeners: {}, attributes: {}, capturedPointer: null,
    classList: { toggle() {} },
    querySelector() { return element(); },
    addEventListener(name, callback) { this.listeners[name] = callback; },
    setAttribute(name, value) { this.attributes[name] = String(value); },
    removeAttribute(name) { delete this.attributes[name]; },
    setPointerCapture(pointerId) { this.capturedPointer = pointerId; },
    appendChild(child) { this.children.push(child); },
    replaceChildren(...children) { this.children = children; },
  };
}

async function createConsole({
  available = 'codex',
  databaseReady = false,
  runs = [],
  runDetails = {},
  runsError = false,
  chatConversationId = null,
  chatGate = null,
  detailGate = null,
  chatResponse = 'Local AI response',
  viewportWidth = 1440,
  storedPanelWidth = null,
  storedUiScale = null,
  pluginMetadata = null,
  reportSource = 'def render(connection, context):\n    return "<html>Report</html>"',
  reportGate = null,
  scriptGate = null,
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
} = {}) {
  const elements = new Map();
  const get = (id) => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  const quickActions = [element(), element()];
  const aiTabs = [get('ai-tab-chat'), get('ai-tab-dsh')];
  aiTabs[0].dataset.aiTab = 'chat';
  aiTabs[1].dataset.aiTab = 'dsh';
  const posts = [];
  const puts = [];
  const reportPosts = [];
  const scriptPuts = [];
  const savedScripts = {};
  const generatedReports = {};
  const intervals = [];
  const commands = [];
  const logRequests = [];
  const configSaves = [];
  const configGets = [];
  const navButtons = ['overview', 'results', 'clients', 'history', 'plugins', 'logs'].map((page) => {
    const button = element(); button.dataset.page = page; return button;
  });
  let detailRequests = 0;
  const storage = new Map();
  if (storedPanelWidth !== null) storage.set('testtracking.ai.panel-width.v1', String(storedPanelWidth));
  if (storedUiScale !== null) storage.set('testtracking.ui.scale.v1', String(storedUiScale));
  const windowListeners = {};
  const documentElement = element();
  const browserWindow = {
    innerWidth: viewportWidth,
    matchMedia: () => ({ matches: false }),
    addEventListener(name, callback) { windowListeners[name] = callback; },
    setInterval(callback, milliseconds) { intervals.push({ callback, milliseconds }); },
  };
  get('result-type').value = 'coverage';
  const context = vm.createContext({
    document: {
      body: element(),
      documentElement,
      getElementById: get,
      createElement: element,
      querySelectorAll: (selector) => {
        if (selector === '#quick-actions button') return quickActions;
        if (selector === '[data-ai-tab]') return aiTabs;
        if (selector === '.nav-item') return navButtons;
        return [];
      },
    },
    localStorage: {
      getItem: (key) => storage.get(key) ?? null,
      setItem: (key, value) => storage.set(key, String(value)),
    },
    window: browserWindow,
    URLSearchParams,
    fetch: async (url, options = {}) => {
      let status = 200;
      let data;
      switch (url) {
        case '/healthz': data = { status: 'ok' }; break;
        case '/readyz':
          status = databaseReady ? 200 : 503;
          data = { database_configured: databaseReady };
          break;
        case '/api/plugins':
          data = pluginMetadata || [{
            name: 'coverage',
            configured: true,
            view_editable: true,
            view: coverageView,
          }];
          break;
        case '/api/plugin-view-schema': data = pluginViewSchema; break;
        case '/api/scheduler': data = typeof scheduler === 'function' ? scheduler() : scheduler; break;
        case '/api/scheduler/commands':
          commands.push(JSON.parse(options.body));
          if (commandGate) await commandGate;
          status = commandError ? 409 : 200;
          data = commandError ? { error: commandError } : { ok: true, run_id: 'new-run' };
          break;
        case '/api/ai/providers':
          data = ['codex', 'dsh'].map((name) => ({ name, available: name === available }));
          break;
        case '/api/runs?limit=100':
          status = runsError ? 500 : 200;
          data = runsError ? { error: 'temporary query failure' }
            : (typeof runs === 'function' ? runs() : runs).slice(0, 100);
          break;
        case '/api/ai/chat':
          assert.equal(options.method, 'POST');
          posts.push(JSON.parse(options.body));
          if (chatGate) await chatGate;
          data = { text: chatResponse, conversation_id: chatConversationId };
          break;
        case '/api/ai/dsh-web':
          assert.equal(options.method, 'POST');
          posts.push({ dshWeb: true });
          data = { url: '/ai/dsh/' };
          break;
        default:
          if (url.startsWith('/api/logs?')) {
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
            data = definition;
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
              scriptPuts.push({ plugin, ...payload });
              status = scriptSaveFailure ? 400 : 200;
              if (!scriptSaveFailure) savedScripts[plugin] = payload.source;
              data = scriptSaveFailure ? { error: scriptSaveFailure } : { plugin, source: payload.source };
            } else {
              if (scriptGate) await scriptGate(plugin);
              data = { plugin, source: savedScripts[plugin] ?? (typeof reportSource === 'function' ? reportSource(plugin) : reportSource) };
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
  return { get, quickActions, aiTabs, posts, puts, reportPosts, scriptPuts, intervals, storage, windowListeners, documentElement, window: browserWindow,
    commands, logRequests, configSaves, configGets, navigate: async (page) => {
      navButtons.find((button) => button.dataset.page === page).listeners.click();
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
  assert.match(ui.get('plugin-grid').innerHTML, /执行插件/);
  assert.match(ui.get('plugin-grid').innerHTML, /分析适配器/);
});

test('offline scheduler disables execution controls while reports and AI remain usable', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [{ run_id: 'done', plugin_type: 'coverage' }] });
  assert.equal(ui.get('run-start').disabled, true);
  assert.equal(ui.get('run-wakeup').disabled, true);
  assert.equal(ui.get('send-button').disabled, false);
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

test('stop command carries expected run id and ignores stale run buttons', async () => {
  const ui = await createConsole({ scheduler: { online: true, plugins: ['coverage'], clients: [],
    active_runs: [{ plugin_type: 'coverage', run_id: 'active-run', stopping: false }] } });
  assert.equal(ui.get('run-start').disabled, true);
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

for (const provider of ['codex', 'dsh']) {
  test(`only ${provider} installed enables AI without a selected run`, async () => {
    const ui = await createConsole({ available: provider });
    assert.equal(ui.get('provider').value, provider);
    assert.equal(ui.get('question').disabled, false);
    assert.equal(ui.get('send-button').disabled, false);
    assert.ok(ui.quickActions.every((button) => !button.disabled));
  });
}

test('database query failure does not block a local AI provider', async () => {
  const ui = await createConsole({ databaseReady: true, runsError: true });
  assert.equal(ui.get('footer-status').textContent, 'temporary query failure');
  assert.equal(ui.get('provider').value, 'codex');
  assert.equal(ui.get('question').disabled, false);
  assert.equal(ui.get('send-button').disabled, false);
});

test('AI controls are disabled when neither provider is installed', async () => {
  const ui = await createConsole({ available: null });
  assert.equal(ui.get('question').disabled, true);
  assert.equal(ui.get('send-button').disabled, true);
  assert.ok(ui.quickActions.every((button) => button.disabled));
});

test('chat form sends a general question without a run and displays the response', async () => {
  const ui = await createConsole({ available: 'dsh' });
  ui.get('question').value = 'Help me understand this project';
  await ui.get('chat-form').listeners.submit({ preventDefault() {} });
  assert.deepEqual(ui.posts, [{
    provider: 'dsh',
    message: 'Help me understand this project',
    conversation_id: null,
    context: { run: null, result_type: 'coverage' },
  }]);
  assert.equal(ui.get('question').value, '');
  assert.equal(ui.get('chat').children.at(-1).textContent, 'Local AI response');
});

test('Enter sends a chat message and Shift+Enter keeps editing', async () => {
  const ui = await createConsole({ available: 'codex' });
  let prevented = false;
  ui.get('question').value = 'Send with Enter';
  await ui.get('question').listeners.keydown({
    key: 'Enter', shiftKey: false, isComposing: false,
    preventDefault() { prevented = true; },
  });

  assert.equal(prevented, true);
  assert.equal(ui.posts.at(-1).message, 'Send with Enter');
  assert.equal(ui.get('question').value, '');

  ui.get('question').value = 'Keep this line';
  await ui.get('question').listeners.keydown({
    key: 'Enter', shiftKey: true, isComposing: false,
    preventDefault() { throw new Error('Shift+Enter must not submit'); },
  });
  assert.equal(ui.posts.length, 1);
  assert.equal(ui.get('question').value, 'Keep this line');
});

test('editor remains usable while the current AI reply is pending', async () => {
  let releaseChat;
  const chatGate = new Promise((resolve) => { releaseChat = resolve; });
  const ui = await createConsole({ available: 'codex', chatGate });
  ui.get('question').value = 'First message';

  const pending = ui.get('chat-form').listeners.submit({ preventDefault() {} });
  await new Promise(setImmediate);
  assert.equal(ui.get('question').disabled, false);
  assert.equal(ui.get('send-button').disabled, true);
  assert.equal(ui.get('send-button').textContent, '…');

  ui.get('question').value = 'Draft next message';
  releaseChat();
  await pending;
  assert.equal(ui.get('question').value, 'Draft next message');
  assert.equal(ui.get('send-button').disabled, false);
  assert.equal(ui.get('send-button').textContent, '➤');
});

test('conversation id is saved per provider and reused on the next message', async () => {
  const ui = await createConsole({ available: 'codex', chatConversationId: 'codex-thread-123' });
  ui.get('question').value = 'First question';
  await ui.get('chat-form').listeners.submit({ preventDefault() {} });
  ui.get('question').value = 'Follow-up question';
  await ui.get('chat-form').listeners.submit({ preventDefault() {} });

  assert.equal(ui.posts[0].conversation_id, null);
  assert.equal(ui.posts[1].conversation_id, 'codex-thread-123');
  assert.equal(JSON.parse(ui.storage.get('testtracking.ai.conversations.v1')).codex, 'codex-thread-123');
  assert.equal(ui.get('conversation-status').textContent, '会话 codex-th');
});

test('changing result selection keeps existing chat messages', async () => {
  const ui = await createConsole({ available: 'codex' });
  const message = element();
  message.textContent = 'Keep this conversation';
  ui.get('chat').appendChild(message);
  ui.get('result-run').value = '';
  await ui.get('result-run').listeners.change();
  assert.ok(ui.get('chat').children.includes(message));
});

test('DSH native tab starts web UI and loads returned URL', async () => {
  const ui = await createConsole({ available: 'dsh' });
  await ui.aiTabs[1].listeners.click();
  await new Promise(setImmediate);
  assert.equal(ui.get('dsh-native-panel').hidden, false);
  assert.equal(ui.get('ai-chat-panel').hidden, true);
  assert.equal(ui.get('dsh-native-frame').src, '/ai/dsh/');
  assert.deepEqual(ui.posts.at(-1), { dshWeb: true });
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

test('AI view design request includes the current definition and schema', async () => {
  const generatedView = { ...coverageView, title: 'AI 覆盖率结果' };
  const ui = await createConsole({ chatResponse: JSON.stringify(generatedView) });
  ui.get('question').value = '突出失败结果';

  await ui.get('ai-design-view').listeners.click();

  const request = ui.posts.at(-1);
  assert.equal(request.provider, 'codex');
  assert.equal(request.context.result_type, 'coverage');
  assert.equal(request.context.purpose, 'plugin_result_view_design');
  assert.deepEqual(JSON.parse(JSON.stringify(request.context.current_view)), coverageView);
  assert.deepEqual(JSON.parse(JSON.stringify(request.context.view_schema)), pluginViewSchema);
  assert.match(request.message, /突出失败结果/);
});

test('valid AI view JSON can be applied and immediately rerenders the plugin view', async () => {
  const generatedView = {
    ...coverageView,
    title: 'AI 生成的覆盖率结果',
    widgets: [{
      id: 'ai-summary',
      type: 'metrics',
      title: 'AI 摘要',
      items: [{ label: '完成文件', value: 'run.success_files', format: 'integer' }],
    }],
  };
  const ui = await createConsole({ chatResponse: `\`\`\`json\n${JSON.stringify(generatedView)}\n\`\`\`` });

  await ui.get('ai-design-view').listeners.click();
  const candidate = ui.get('chat').children.find((child) => child.className.includes('view-candidate'));
  assert.ok(candidate, 'AI JSON should create an applicable view candidate');
  const applyButton = candidate.children.find((child) => child.textContent === '应用此展示');
  assert.ok(applyButton);

  await applyButton.listeners.click();

  assert.equal(ui.puts.length, 1);
  assert.equal(ui.puts[0].title, 'AI 生成的覆盖率结果');
  assert.equal(applyButton.textContent, '已应用');
  assert.equal(ui.get('plugin-view-title').textContent, 'AI 生成的覆盖率结果');
  assert.match(ui.get('plugin-result-view').innerHTML, /AI 摘要/);
});

test('invalid AI view output is explained and never sent to the view endpoint', async () => {
  const ui = await createConsole({ chatResponse: '这里没有可用的 JSON 展示定义。' });

  await ui.get('ai-design-view').listeners.click();

  assert.equal(ui.puts.length, 0);
  assert.equal(
    ui.get('chat').children.at(-1).textContent,
    '回复中没有可应用的展示 JSON。可以补充要求后再次生成。',
  );
  assert.equal(
    ui.get('chat').children.some((child) => child.className.includes('view-candidate')),
    false,
  );
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
  assert.equal(ui.get('view-editor-dialog').open, false);
  assert.equal(ui.get('plugin-view-title').textContent, '手工调整的覆盖率结果');
  assert.match(ui.get('plugin-result-view').innerHTML, /手工摘要/);
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

test('AI report generation includes script contract and applies only after explicit action', async () => {
  const source = 'def render(connection, context):\n    return "<h1>AI report</h1>"';
  const ui = await createConsole({ databaseReady: true, runs: [reportRun], chatResponse: `\`\`\`python\n${source}\n\`\`\`` });
  ui.get('question').value = '显示失败结果的可筛选表格';
  await ui.get('ai-design-report').listeners.click();
  const request = ui.posts.at(-1);
  assert.equal(request.context.purpose, 'analysis_report_design');
  assert.match(request.context.current_script, /def render/);
  assert.equal(request.context.script_contract.entrypoint, 'render(connection, context)');
  assert.equal(request.context.analysis_catalog.tables[0].name, 'file_results');
  assert.match(request.message, /显示失败结果/);
  assert.equal(ui.scriptPuts.length, 0);
  assert.equal(ui.reportPosts.length, 0);
  const candidate = ui.get('chat').children.find((child) => child.className.includes('report-candidate'));
  assert.ok(candidate);
  await candidate.children[1].listeners.click();
  assert.equal(ui.get('report-editor-dialog').open, true);
  assert.equal(ui.get('report-editor-text').value, source);
  assert.equal(ui.scriptPuts.length, 0, 'previewing a single candidate must not save it');
  assert.equal(ui.reportPosts.length, 0);
  await ui.get('save-run-report-editor').listeners.click();
  assert.deepEqual(ui.scriptPuts, [{ plugin: 'coverage', source }]);
  assert.equal(ui.reportPosts.length, 1);
});

test('all Python candidates can be previewed and only the selected edited source is saved', async () => {
  const first = 'def render(connection, context):\n    return "first"';
  const second = 'def render(connection, context):\n    return "second"';
  const third = 'def render(connection, context):\n    return "third"';
  const ui = await createConsole({ databaseReady: true, runs: [reportRun],
    chatResponse: `First\n\`\`\`python\n${first}\n\`\`\`\n\`\`\`json\n{}\n\`\`\`\nSecond\n\`\`\`py\n${second}\n\`\`\`\nThird\n~~~Python\n${third}\n~~~` });
  await ui.get('ai-design-report').listeners.click();
  const candidate = ui.get('chat').children.find((child) => child.className.includes('report-candidate'));
  assert.match(candidate.children[0].textContent, /3 个/);
  await candidate.children[1].listeners.click();
  assert.equal(ui.get('report-editor-text').value, first);
  const editedFirst = first + '\n# edited first';
  ui.get('report-editor-text').value = editedFirst;
  ui.get('report-candidate-index').value = '2';
  ui.get('report-candidate-index').listeners.change();
  assert.equal(ui.get('report-editor-text').value, third);
  ui.get('report-candidate-index').value = '0';
  ui.get('report-candidate-index').listeners.change();
  assert.equal(ui.get('report-editor-text').value, editedFirst);
  ui.get('report-candidate-index').value = '1';
  ui.get('report-candidate-index').listeners.change();
  assert.equal(ui.get('report-editor-text').value, second);
  const editedSecond = second.replace('second', 'visible and edited');
  ui.get('report-editor-text').value = editedSecond;
  await ui.get('save-run-report-editor').listeners.click();
  assert.deepEqual(ui.scriptPuts, [{ plugin: 'coverage', source: editedSecond }]);
  assert.equal(ui.reportPosts.length, 1);
});

test('candidate edits survive closing and do not overwrite a manual script draft', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun],
    chatResponse: '```python\ndef render(connection, context):\n    return "candidate"\n```' });
  await ui.get('edit-report-script').listeners.click();
  ui.get('report-editor-text').value = '# my manual draft';
  ui.get('close-report-editor').listeners.click();
  await ui.get('ai-design-report').listeners.click();
  const candidate = ui.get('chat').children.find((child) => child.className.includes('report-candidate'));
  await candidate.children[1].listeners.click();
  assert.match(ui.get('report-editor-status').textContent, /手写.*草稿.*保留/);
  const source = 'def render(connection, context):\n    return "edited candidate"';
  ui.get('report-editor-text').value = source;
  ui.get('close-report-editor').listeners.click();
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# my manual draft');
  ui.get('close-report-editor').listeners.click();
  await candidate.children[1].listeners.click();
  assert.equal(ui.get('report-editor-text').value, source);
  await ui.intervals[0].callback();
  assert.equal(ui.get('save-report-editor').disabled, false, 'polling the same run keeps the preview usable');
  await ui.get('save-report-editor').listeners.click();
  assert.deepEqual(ui.scriptPuts, [{ plugin: 'coverage', source }]);
  ui.get('close-report-editor').listeners.click();
  await ui.get('edit-report-script').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# my manual draft');
});

test('candidate AST rejection retains visible edits and a previously successful report', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun],
    scriptSaveFailure: 'render(connection, context) is required', chatResponse: '```python\nprint("incomplete")\n```' });
  await ui.get('run-report').listeners.click();
  const previousUrl = ui.get('report-frame').src;
  await ui.get('ai-design-report').listeners.click();
  const candidate = ui.get('chat').children.find((child) => child.className.includes('report-candidate'));
  await candidate.children[1].listeners.click();
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

test('AI reply without a Python fence explains that no script was saved or run', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun], chatResponse: 'def render(connection, context): return "no fence"' });
  await ui.get('ai-design-report').listeners.click();
  assert.equal(ui.get('chat').children.some((child) => child.className.includes('report-candidate')), false);
  assert.match(ui.get('chat').children.at(-1).textContent, /没有可预览的 Python.*尚未保存或运行/);
  assert.equal(ui.scriptPuts.length, 0);
  assert.equal(ui.reportPosts.length, 0);
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

test('AI report candidate cannot overwrite a different plugin or run selection', async () => {
  const ui = await createConsole({
    databaseReady: true, runs: [reportRun, { ...reportRun, run_id: 'other-run' }],
    chatResponse: '```python\ndef render(connection, context):\n    return "done"\n```',
  });
  await ui.get('ai-design-report').listeners.click();
  const candidate = ui.get('chat').children.find((child) => child.className.includes('report-candidate'));
  ui.get('result-run').value = 'other-run';
  await ui.get('result-run').listeners.change();
  await candidate.children[1].listeners.click();
  assert.equal(ui.scriptPuts.length, 0);
  assert.equal(ui.reportPosts.length, 0);
  assert.match(ui.get('chat').children.at(-1).textContent, /请为当前运行重新生成/);
});

test('an open candidate cannot be saved after switching away and back to its run', async () => {
  const ui = await createConsole({ databaseReady: true, runs: [reportRun, { ...reportRun, run_id: 'other-run' }],
    chatResponse: '```python\ndef render(connection, context):\n    return "candidate"\n```' });
  await ui.get('ai-design-report').listeners.click();
  const candidate = ui.get('chat').children.find((child) => child.className.includes('report-candidate'));
  await candidate.children[1].listeners.click();
  ui.get('report-editor-text').value = '# candidate edit remains';
  ui.get('report-editor-text').listeners.input();
  for (const runId of ['other-run', reportRun.run_id]) {
    ui.get('result-run').value = runId;
    await ui.get('result-run').listeners.change();
  }
  assert.equal(ui.get('save-report-editor').disabled, true);
  assert.equal(ui.get('save-run-report-editor').disabled, true);
  await ui.get('save-report-editor').listeners.click();
  await ui.get('save-run-report-editor').listeners.click();
  assert.equal(ui.get('report-editor-text').value, '# candidate edit remains');
  assert.equal(ui.scriptPuts.length, 0);
  assert.equal(ui.reportPosts.length, 0);
  assert.match(ui.get('report-editor-status').textContent, /运行选择已变化/);
});

test('an AI response arriving after a run switch retains its original selection generation', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const ui = await createConsole({ databaseReady: true, runs: [reportRun, { ...reportRun, run_id: 'other-run' }],
    chatGate: gate, chatResponse: '```python\ndef render(connection, context):\n    return "candidate"\n```' });
  const generating = ui.get('ai-design-report').listeners.click();
  for (const runId of ['other-run', reportRun.run_id]) {
    ui.get('result-run').value = runId;
    await ui.get('result-run').listeners.change();
  }
  release();
  await generating;
  const candidate = ui.get('chat').children.find((child) => child.className.includes('report-candidate'));
  await candidate.children[1].listeners.click();
  assert.match(ui.get('chat').children.at(-1).textContent, /运行选择已变化/);
  assert.notEqual(ui.get('report-editor-dialog').open, true);
  assert.equal(ui.scriptPuts.length, 0);
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
  assert.match(html, /id="report-frame"[^>]*sandbox="allow-scripts"/);
});
