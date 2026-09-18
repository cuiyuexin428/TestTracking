const AI_CONVERSATIONS_KEY = 'testtracking.ai.conversations.v1';
const AI_PANEL_WIDTH_KEY = 'testtracking.ai.panel-width.v1';
const UI_SCALE_KEY = 'testtracking.ui.scale.v1';
const PANEL_MIN_WIDTH = 360;
const HISTORY_PAGE_SIZE = 50;

function readStoredJson(key, fallback) {
  try {
    if (typeof localStorage === 'undefined') return fallback;
    return JSON.parse(localStorage.getItem(key) || '') || fallback;
  } catch (_) { return fallback; }
}

function readStoredNumber(key, fallback) {
  try {
    if (typeof localStorage === 'undefined') return fallback;
    const value = Number(localStorage.getItem(key));
    return Number.isFinite(value) && value > 0 ? value : fallback;
  } catch (_) { return fallback; }
}

const storedPanelWidth = readStoredNumber(AI_PANEL_WIDTH_KEY, 400);
const state = {
  runs: [], plugins: [], pluginViews: {}, pluginViewSchema: null,
  selected: null, page: 'results', filter: '', readiness: null,
  providers: [], selectionRequest: 0, selectionPending: 0, loadRequest: 0, apiHealthy: false,
  conversationIds: readStoredJson(AI_CONVERSATIONS_KEY, {}), aiBusy: false,
  panelWidth: storedPanelWidth, panelPreferredWidth: storedPanelWidth,
  dshWebUrl: '', dshWebLoading: false,
  panelWidthBeforeExpand: 0, uiScale: readStoredNumber(UI_SCALE_KEY, 1.2),
  progressRefreshBusy: false,
  analysisCatalogs: {}, reportScripts: {}, reportLoads: {}, reportDrafts: {},
  reportScriptLoads: {},
  report: null, reportRequest: 0, reportBusy: false, reportSelectionKey: '', reportSelectionGeneration: 0,
  reportEditorPlugin: '', reportEditorRequest: 0, reportEditorOriginal: '',
  reportEditorCandidate: null, reportEditorSaving: false,
  historyRuns: [], historyOffset: 0, historySearch: '', historyHasMore: false,
  historyRequest: 0, historyBusy: false,
  scheduler: null, schedulerRequest: 0, schedulerBusy: false, commandBusy: false,
  logsRequest: 0, logsBusy: false, logsLoaded: false,
  configKind: 'scheduler', configs: {}, configDrafts: {}, configRequest: 0, configBusy: false,
};
const $ = (id) => document.getElementById(id);

const pages = {
  overview: ['运行总览', '查看持久化任务进度；记录状态不代表调度进程当前在线。'],
  results: ['结果分析', '从已入库结果生成可替换的 Python 交互报告。'],
  clients: ['客户端', '查看实时连接、心跳、资源和正在处理的任务。'],
  history: ['历史运行', '查看长期保存的运行记录。'],
  plugins: ['插件与配置', '查看执行插件、分析适配器并维护部署配置。'],
  logs: ['运行日志', '查询服务器日志，按级别和内容筛选。']
};

async function jsonFetch(url, options) {
  const response = await fetch(url, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

async function statusFetch(url) {
  const response = await fetch(url);
  const data = await response.json();
  return { ok: response.ok, status: response.status, data };
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[char]));
}

function number(value) { return Number(value || 0).toLocaleString('zh-CN'); }
function statusLabel(value) { return ({ finished: '已完成', running: '运行中', planning: '规划中', planned: '已计划', stopped: '已停止', failed: '失败', interrupted: '已中断', cancelled: '已取消' })[value] || value || '--'; }
function timeLabel(value) { return value ? new Date(value).toLocaleString('zh-CN', { hour12: false }) : '--'; }
function executionChipClass(value) { return value === 'running' ? 'blue' : (value === 'stopped' || value === 'planning' || value === 'planned' ? 'amber' : (value === 'failed' || value === 'interrupted' ? 'red' : '')); }

function emptyRunState() {
  if (state.readiness && !state.readiness.ready) {
    return state.readiness.configured
      ? ['数据库连接异常', '结果服务无法读取运行记录，请检查只读数据库连接。']
      : ['数据库未配置', '配置只读数据库连接后，这里会显示长期运行清单。'];
  }
  return ['暂无运行记录', '数据库中还没有可展示的运行记录。'];
}

function setDotState(id, status) {
  const dot = $(id);
  dot.classList.toggle('offline', status === 'offline');
  dot.classList.toggle('warning', status === 'warning');
}

function setAnalysisState(status, label) {
  const analysisStatus = $('analysis-status');
  const error = status === 'error';
  analysisStatus.classList.toggle('waiting', status !== 'ready' && !error);
  analysisStatus.classList.toggle('error', error);
  const dot = analysisStatus.querySelector('.dot');
  dot.classList.toggle('warning', status !== 'ready' && !error);
  dot.classList.toggle('offline', error);
  $('analysis-state').textContent = label;
}

function renderServiceState(readiness, providers) {
  const ready = readiness.ok;
  const configured = Boolean(readiness.data.database_configured);
  const availableProviders = providers.filter((provider) => provider.available).map((provider) => provider.name);
  state.readiness = { ready, configured };

  $('api-status').textContent = 'API 已连接';
  setDotState('api-dot', 'online');
  if (ready) {
    $('health').textContent = '结果数据可用';
    $('db-status').textContent = '数据库已连接';
    $('footer-status').textContent = '结果服务与数据库运行正常';
    setDotState('health-dot', 'online');
    setDotState('db-dot', 'online');
    setDotState('footer-dot', 'online');
  } else if (!configured) {
    $('health').textContent = '数据库未配置';
    $('db-status').textContent = '数据库未配置';
    $('footer-status').textContent = '结果服务运行中，数据库未配置';
    setDotState('health-dot', 'warning');
    setDotState('db-dot', 'warning');
    setDotState('footer-dot', 'warning');
  } else {
    $('health').textContent = '数据库连接异常';
    $('db-status').textContent = '数据库不可用';
    $('footer-status').textContent = '结果服务运行中，数据库连接异常';
    setDotState('health-dot', 'offline');
    setDotState('db-dot', 'offline');
    setDotState('footer-dot', 'offline');
  }

  $('provider-status').textContent = availableProviders.length ? `AI 可用：${availableProviders.join(' / ')}` : 'AI 服务不可用';
  const noticeParts = [];
  if (!ready) noticeParts.push(configured ? '只读数据库当前无法连接。' : '请配置只读数据库连接以读取运行记录。');
  if (!availableProviders.length) noticeParts.push('未找到可用的 Codex 或 DSH 命令，请安装并确保命令位于 PATH 中。');
  $('system-notice').hidden = noticeParts.length === 0;
  $('notice-title').textContent = !ready
    ? (configured ? '数据库连接异常' : '数据库未配置')
    : 'AI 服务不可用';
  $('notice-detail').textContent = noticeParts.join(' ');
}

function updateAiControls() {
  const provider = state.providers.find((item) => item.name === $('provider').value);
  const hasProvider = Boolean(provider?.available);
  const hasRun = Boolean(state.selected?.run_id);
  // A server provider can answer general questions without a selected run.
  // Selecting a run only enriches the context sent with the request.
  const enabled = hasProvider && !state.aiBusy;
  // Keep the editor usable while a reply is running so the next question is
  // not lost; only submission must wait for the active provider call.
  $('question').disabled = !hasProvider;
  $('send-button').disabled = !enabled;
  $('send-button').textContent = state.aiBusy ? '…' : '➤';
  $('send-button').title = state.aiBusy ? 'AI 正在回复' : '发送';
  $('send-button').setAttribute('aria-label', $('send-button').title);
  $('new-conversation').disabled = state.aiBusy || !hasProvider;
  document.querySelectorAll('#quick-actions button').forEach((button) => { button.disabled = !enabled; });
  $('ai-design-view').disabled = !enabled || !state.pluginViews[$('result-type').value];
  $('ai-design-report').disabled = !enabled || !$('result-type').value;
  $('design-view-from-chat').disabled = !enabled || !$('result-type').value;
  $('question').placeholder = hasProvider ? (hasRun ? '询问当前结果...' : '询问 Codex 或 DSH...') : '当前 AI 服务不可用';
  $('compose-hint').textContent = hasProvider
    ? (state.aiBusy ? `${provider.name} 正在回复，可继续编辑下一条` : (hasRun ? '当前运行的结构化结果' : '未选择运行，使用通用上下文'))
    : 'AI 服务不可用';
  const providerStatus = $('provider-availability');
  providerStatus.className = hasProvider ? 'available' : 'unavailable';
  providerStatus.textContent = provider ? `${provider.name} ${hasProvider ? '可用' : '不可用'}` : '没有可用的 AI 服务';
  const conversationId = state.conversationIds[$('provider').value];
  $('conversation-status').textContent = conversationId ? `会话 ${conversationId.slice(0, 8)}` : '新会话';

  const dshAvailable = state.providers.some((item) => item.name === 'dsh' && item.available);
  $('dsh-native-open').disabled = !dshAvailable || state.dshWebLoading;
  if (!dshAvailable && !state.dshWebUrl) $('dsh-native-status').textContent = '服务器 DSH 命令不可用。';
  updateReportControls();
}

function navigate(page) {
  state.page = page;
  Object.keys(pages).forEach((name) => { $(`page-${name}`).hidden = name !== page; });
  document.querySelectorAll('.nav-item').forEach((button) => {
    if (button.dataset.page === page) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  });
  $('page-title').textContent = pages[page][0];
  $('page-subtitle').textContent = pages[page][1];
  $('crumb').textContent = pages[page][0];
  if (page === 'history') loadHistory().catch(showHistoryError);
  if (page === 'clients' || page === 'overview' || page === 'plugins') loadScheduler();
  if (page === 'logs') loadLogs();
  if (page === 'plugins') loadConfig($('config-kind').value || state.configKind);
}

function executionPluginNames() {
  return (state.scheduler?.plugins || []).map((plugin) => typeof plugin === 'string' ? plugin : plugin.name).filter(Boolean);
}

function formatBytes(value) {
  if (value === null || value === undefined) return '--';
  const amount = Number(value);
  if (amount >= 1024 ** 3) return `${(amount / 1024 ** 3).toFixed(1)} GB`;
  if (amount >= 1024 ** 2) return `${(amount / 1024 ** 2).toFixed(1)} MB`;
  if (amount >= 1024) return `${(amount / 1024).toFixed(1)} KB`;
  return `${amount} B`;
}

function renderScheduler() {
  const online = Boolean(state.scheduler?.online);
  const delayed = Number(state.scheduler?.snapshot_age_ms || 0) > 5000;
  const names = executionPluginNames();
  const preferred = $('run-plugin').value;
  $('run-plugin').innerHTML = names.length ? names.map((name) => `<option value="${escapeHtml(name)}">${escapeHtml(name)}</option>`).join('') : '<option value="">没有可用执行插件</option>';
  $('run-plugin').value = names.includes(preferred) ? preferred : (names[0] || '');
  $('scheduler-status').textContent = online ? (delayed ? `调度器响应中 · 状态更新延迟 ${(state.scheduler.snapshot_age_ms / 1000).toFixed(1)} 秒` : `调度器在线 · ${number(state.scheduler.in_flight)} 个在途任务`) : '调度器离线';
  $('scheduler-status').className = `chip ${online && !delayed ? 'green' : 'amber'}`;
  const active = state.scheduler?.active_runs || [];
  const selectedActive = active.some((run) => run.plugin_type === $('run-plugin').value);
  $('run-plugin').disabled = !online || state.commandBusy || !names.length;
  $('run-dataset').disabled = !online || state.commandBusy;
  $('run-params').disabled = !online || state.commandBusy;
  $('run-start').disabled = !online || state.commandBusy || !names.length || selectedActive;
  $('run-start').title = selectedActive ? '该插件已有活动运行，请先停止并等待完成' : '';
  $('run-wakeup').disabled = !online || state.commandBusy || !names.length;
  $('active-runs').innerHTML = online && active.length ? active.map((run) => `<div class="active-run"><div><strong>${escapeHtml(run.plugin_type)}</strong><span>${escapeHtml(run.run_id)}</span></div><button type="button" class="button" data-stop-run="${escapeHtml(run.run_id)}" data-stop-plugin="${escapeHtml(run.plugin_type)}" ${state.commandBusy || run.stopping ? 'disabled' : ''}>${run.stopping ? '正在停止…' : '停止运行'}</button></div>`).join('') : `<p class="management-status">${online ? '当前没有活动运行。' : '执行控制暂不可用，已入库结果仍可分析。'}</p>`;
  renderClients();
  renderPlugins(state.plugins);
}

function renderClients() {
  const online = Boolean(state.scheduler?.online);
  const delayed = Number(state.scheduler?.snapshot_age_ms || 0) > 5000;
  const clients = online ? (state.scheduler.clients || []) : [];
  $('client-count').textContent = online ? `${clients.length} 台` : '--';
  $('clients-status').textContent = online ? (delayed ? `调度器响应中，以下为缓存状态 · 更新延迟 ${(state.scheduler.snapshot_age_ms / 1000).toFixed(1)} 秒` : '来自调度器的实时状态 · 每 5 秒刷新') : '调度器当前不可达，无法确认客户端是否在线。';
  $('client-list').innerHTML = clients.length ? clients.map((client) => {
    const tasks = client.tasks || [];
    const heartbeat = Number.isFinite(Number(client.last_seen_age_ms)) ? `${(Number(client.last_seen_age_ms) / 1000).toFixed(1)} 秒前` : '--';
    return `<article class="client-card"><div class="panel-head"><h2>${escapeHtml(client.worker_id)}</h2><span class="chip ${client.registered ? (client.status === 'busy' ? 'blue' : 'green') : 'amber'}">${client.registered ? (client.status === 'busy' ? '处理中' : '空闲') : '尚未注册'}</span></div><div class="client-facts"><span>插件 <b>${escapeHtml(client.plugin_type)}</b></span><span>心跳 <b>${heartbeat}</b></span><span>地址 <b>${escapeHtml(client.ip || client.peer_address || '--')}</b></span><span>内存 <b>${formatBytes(client.memory_bytes)}</b></span><span>可用磁盘 <b>${formatBytes(client.disk_free_bytes)}</b></span><span>CPU <b>${client.cpu_percent === null || client.cpu_percent === undefined ? '--' : `${Number(client.cpu_percent).toFixed(0)}%`}</b></span></div><p class="client-info">${escapeHtml(client.plugin_info || '')}</p>${tasks.length ? `<div class="table-wrap"><table><thead><tr><th>当前任务</th><th>运行</th><th>尝试</th><th>状态</th></tr></thead><tbody>${tasks.map((task) => `<tr><td>${escapeHtml(task.task_id)}</td><td>${escapeHtml(task.run_id)}</td><td>${escapeHtml(task.attempt_id)}</td><td>${task.acked ? '客户端已确认' : '等待确认'}</td></tr>`).join('')}</tbody></table></div>` : '<p class="management-status">当前没有分配的任务</p>'}</article>`;
  }).join('') : `<div class="empty-panel"><h2>${online ? '暂无客户端连接' : '调度器离线'}</h2><p>${online ? '启动客户端后，这里会显示连接、心跳和任务。' : '恢复调度服务后会自动更新。'}</p></div>`;
}

async function loadScheduler() {
  if (state.schedulerBusy) return;
  state.schedulerBusy = true;
  const requestId = ++state.schedulerRequest;
  try {
    const snapshot = await jsonFetch('/api/scheduler');
    if (requestId === state.schedulerRequest) state.scheduler = snapshot;
  } catch (error) {
    if (requestId === state.schedulerRequest) {
      state.scheduler = { online: false, plugins: [], clients: [], active_runs: [] };
      $('scheduler-command-status').textContent = `调度状态读取失败：${error.message}`;
    }
  } finally {
    state.schedulerBusy = false;
    renderScheduler();
  }
}

async function schedulerCommand(action, plugin, runId = '') {
  if (state.commandBusy || !state.scheduler?.online || !plugin) return;
  if (action === 'run' && (state.scheduler.active_runs || []).some((run) => run.plugin_type === plugin)) return;
  const payload = { action, plugin_type: plugin };
  if (action === 'run') {
    payload.dataset_version = $('run-dataset').value.trim();
    payload.params = $('run-params').value;
    if (!payload.dataset_version) { $('scheduler-command-status').textContent = '请输入数据版本。'; return; }
  }
  if (action === 'stop') {
    const active = (state.scheduler.active_runs || []).find((run) => run.run_id === runId && run.plugin_type === plugin);
    if (!active || active.stopping) return;
    payload.run_id = runId;
  }
  state.commandBusy = true;
  renderScheduler();
  $('scheduler-command-status').textContent = '正在提交命令…';
  try {
    const result = await jsonFetch('/api/scheduler/commands', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
    $('scheduler-command-status').textContent = action === 'run' ? `已启动运行 ${result.run_id || ''}` : action === 'stop' ? `已请求停止 ${runId}` : `已唤醒 ${plugin} 客户端`;
    await loadScheduler();
  } catch (error) {
    $('scheduler-command-status').textContent = `命令失败：${error.message}`;
  } finally { state.commandBusy = false; renderScheduler(); }
}

async function loadLogs() {
  const requestId = ++state.logsRequest;
  state.logsBusy = true;
  $('logs-refresh').disabled = true;
  const query = new URLSearchParams({ file: $('logs-file').value, limit: $('logs-limit').value || '200', search: $('logs-search').value.trim(), level: $('logs-level').value });
  try {
    const result = await jsonFetch(`/api/logs?${query}`);
    if (requestId !== state.logsRequest) return;
    $('logs-file').innerHTML = result.files.length ? result.files.map((file) => `<option value="${escapeHtml(file.name)}">${escapeHtml(file.name)} · ${formatBytes(file.size)}</option>`).join('') : '<option value="">暂无日志</option>';
    $('logs-file').value = result.selected || '';
    $('logs-output').textContent = result.lines.join('\n') || '没有匹配的日志。';
    $('logs-status').textContent = `${result.selected || '暂无日志文件'} · 显示 ${result.lines.length} 行${result.truncated ? ' · 已截取最近匹配内容' : ''} · ${new Date().toLocaleTimeString('zh-CN', { hour12: false })}`;
    state.logsLoaded = true;
  } catch (error) {
    if (requestId === state.logsRequest) $('logs-status').textContent = `读取失败：${error.message}${state.logsLoaded ? '；保留上次内容。' : ''}`;
  } finally { if (requestId === state.logsRequest) { state.logsBusy = false; $('logs-refresh').disabled = false; } }
}

function configDirty(kind = state.configKind) {
  return state.configDrafts[kind] !== undefined && state.configDrafts[kind] !== JSON.stringify(state.configs[kind]?.config, null, 2);
}

function rememberConfigDraft() {
  if (!state.configs[state.configKind]?.available) return;
  state.configDrafts[state.configKind] = $('config-json').value;
  updateConfigControls();
}

function updateConfigControls() {
  const available = Boolean(state.configs[state.configKind]?.available);
  const dirty = configDirty();
  $('config-save').disabled = !available || !dirty || state.configBusy;
  $('config-json').disabled = !available || state.configBusy;
  $('config-kind').disabled = state.configBusy;
  $('config-reload').disabled = state.configBusy || dirty;
  $('config-discard').hidden = !dirty;
  $('config-discard').disabled = state.configBusy;
  $('config-dirty').hidden = !dirty;
}

function renderConfig() {
  const config = state.configs[state.configKind];
  $('config-path').textContent = config?.path || '';
  $('config-notice').textContent = [config?.notice, ...(config?.warnings || []), config?.restart_required ? '配置已更新，需重启相应服务生效。' : ''].filter(Boolean).join(' ');
  $('config-notice').classList.toggle('warning', Boolean(config?.warnings?.length));
  $('config-json').value = state.configDrafts[state.configKind] ?? (config?.available ? JSON.stringify(config.config, null, 2) : '');
  $('config-fields').innerHTML = config?.fields?.length ? `<div class="table-wrap"><table><thead><tr><th>配置字段</th><th>类型</th><th>说明</th></tr></thead><tbody>${config.fields.map((field) => `<tr><td><strong>${escapeHtml(field.label)}</strong><br><code>${escapeHtml(field.path)}</code></td><td>${escapeHtml(field.type)}</td><td>${escapeHtml(field.description)}</td></tr>`).join('')}</tbody></table></div>` : '';
  $('config-effective').textContent = config?.effective_values ? JSON.stringify(config.effective_values, null, 2) : '';
  updateConfigControls();
}

async function loadConfig(kind = state.configKind, force = false) {
  if (state.configBusy) return;
  if (kind !== state.configKind) rememberConfigDraft();
  state.configKind = kind;
  $('config-kind').value = kind;
  if (!force && state.configDrafts[kind] !== undefined) { renderConfig(); $('config-status').textContent = configDirty(kind) ? '已恢复未保存的草稿。' : ''; return; }
  const requestId = ++state.configRequest;
  state.configBusy = true;
  renderConfig();
  $('config-status').textContent = '正在读取配置…';
  try {
    const config = await jsonFetch(`/api/config/${encodeURIComponent(kind)}`);
    if (requestId !== state.configRequest) return;
    state.configs[kind] = config;
    delete state.configDrafts[kind];
    renderConfig();
    $('config-status').textContent = config.available ? '配置已读取。' : (config.reason || '当前部署未提供此配置文件。');
  } catch (error) { $('config-status').textContent = `读取配置失败：${error.message}`; }
  finally { state.configBusy = false; updateConfigControls(); }
}

async function saveConfig() {
  if (state.configBusy || !state.configs[state.configKind]?.available) return;
  rememberConfigDraft();
  let config;
  try {
    config = JSON.parse($('config-json').value);
    if (!config || typeof config !== 'object' || Array.isArray(config)) throw new Error('配置必须是 JSON 对象');
  } catch (error) { $('config-status').textContent = `JSON 无效：${error.message}`; return; }
  const kind = state.configKind;
  state.configBusy = true;
  updateConfigControls();
  $('config-status').textContent = '正在保存…';
  try {
    const saved = await jsonFetch(`/api/config/${encodeURIComponent(kind)}`, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ config, revision: state.configs[kind].revision }) });
    state.configs[kind] = saved;
    delete state.configDrafts[kind];
    renderConfig();
    $('config-status').textContent = '已保存。重启相应服务后生效。';
  } catch (error) { $('config-status').textContent = `保存失败：${error.message}。草稿已保留；配置冲突时请放弃草稿后读取最新内容。`; }
  finally { state.configBusy = false; updateConfigControls(); }
}

function renderMetrics() {
  const running = state.runs.filter((run) => run.status === 'running').length;
  const reported = state.runs.reduce((sum, run) => sum + taskCount(run, 'reported'), 0);
  const active = state.runs.reduce((sum, run) => sum + taskCount(run, 'acked') + taskCount(run, 'dispatched'), 0);
  $('metric-runs').innerHTML = `${number(state.runs.length)}<small>个运行</small>`;
  $('metric-finished').innerHTML = `${number(running)}<small>个运行</small>`;
  $('metric-success').innerHTML = `${number(reported)}<small>个</small>`;
  $('metric-failed').innerHTML = `${number(active)}<small>个</small>`;
}

function taskCount(run, status) { return Number(run.task_status_counts?.[status] || 0); }

function taskProgress(run) {
  const total = Number(run.total_tasks || 0);
  const reported = taskCount(run, 'reported');
  const active = taskCount(run, 'acked') + taskCount(run, 'dispatched');
  const reclaimed = taskCount(run, 'reclaimed');
  const pending = taskCount(run, 'pending');
  const percent = total > 0 ? Math.min(100, reported / total * 100) : 0;
  return { total, reported, active, reclaimed, pending, percent };
}

function renderOverviewFilters(plugins) {
  const names = plugins.map((plugin) => plugin.name);
  if (state.filter && !names.includes(state.filter)) state.filter = '';
  $('overview-filter').innerHTML = [
    `<button data-filter="" aria-pressed="${String(!state.filter)}">全部</button>`,
    ...names.map((name) => `<button data-filter="${escapeHtml(name)}" aria-pressed="${String(state.filter === name)}">${escapeHtml(name)}</button>`),
  ].join('');
}

function renderRunCards() {
  const runs = state.runs.filter((run) => !state.filter || run.plugin_type === state.filter);
  const [emptyTitle, emptyDetail] = emptyRunState();
  $('overview-count').textContent = `${runs.length} 条`;
  $('run-cards').innerHTML = runs.length ? runs.slice(0, 8).map((run) => {
    const progress = taskProgress(run);
    return `
    <article class="run-card">
      <div class="run-card-top"><div class="run-title"><span class="plugin-badge">${escapeHtml(run.plugin_type.slice(0, 1))}</span><div><h3>${escapeHtml(run.plugin_type)} <span class="chip ${executionChipClass(run.status)}">${escapeHtml(statusLabel(run.status))}</span></h3><div class="run-id">${escapeHtml(run.run_id)} · 数据集 ${escapeHtml(run.dataset_version || '--')}</div></div></div><button type="button" class="button" data-open-run="${escapeHtml(run.run_id)}" data-open-plugin="${escapeHtml(run.plugin_type)}">查看结果</button></div>
      <div class="task-progress-head"><span>任务处理进度</span><strong>${number(progress.reported)} / ${number(progress.total)} · ${progress.percent.toFixed(0)}%</strong></div>
      <div class="task-progress-track"><i style="width:${progress.percent}%"></i></div>
      <div class="run-meta"><span>待派发 <b>${number(progress.pending)}</b></span><span>处理中 <b>${number(progress.active)}</b></span><span>重试等待 <b>${number(progress.reclaimed)}</b></span><span>已完成 <b>${number(progress.reported)}</b></span><span>创建时间 <b>${escapeHtml(timeLabel(run.created_at))}</b></span></div>
    </article>`;
  }).join('') : `<div class="empty-panel"><span>◇</span><h2>${emptyTitle}</h2><p>${emptyDetail}</p></div>`;
}

function renderHistory() {
  const [emptyTitle] = emptyRunState();
  const runs = state.historyRuns;
  $('history-count').textContent = `本页 ${runs.length} 条`;
  $('history-page').textContent = `第 ${Math.floor(state.historyOffset / HISTORY_PAGE_SIZE) + 1} 页`;
  $('history-prev').disabled = state.historyBusy || state.historyOffset === 0;
  $('history-next').disabled = state.historyBusy || !state.historyHasMore;
  $('history-table').innerHTML = runs.length ? runs.map((run) => `<tr><td><button type="button" class="run-link" data-open-run="${escapeHtml(run.run_id)}" data-open-plugin="${escapeHtml(run.plugin_type)}">${escapeHtml(run.run_id)}</button></td><td>${escapeHtml(run.plugin_type)}</td><td><span class="chip ${executionChipClass(run.status)}">${escapeHtml(statusLabel(run.status))}</span></td><td>${number(run.success_files)} 成功 / ${number(run.failed_files)} 失败</td><td>${escapeHtml(timeLabel(run.finished_at))}</td></tr>`).join('') : `<tr><td colspan="5" class="empty-row">${state.historySearch ? '没有匹配的运行' : emptyTitle}</td></tr>`;
}

async function loadHistory(offset = state.historyOffset) {
  const requestId = ++state.historyRequest;
  state.historyBusy = true;
  renderHistory();
  $('history-status').textContent = '正在读取历史运行…';
  try {
    const runs = await jsonFetch(`/api/runs?limit=${HISTORY_PAGE_SIZE + 1}&offset=${offset}&search=${encodeURIComponent(state.historySearch)}`);
    if (requestId !== state.historyRequest) return;
    state.historyOffset = offset;
    state.historyRuns = runs.slice(0, HISTORY_PAGE_SIZE);
    state.historyHasMore = runs.length > HISTORY_PAGE_SIZE;
    $('history-status').textContent = '';
  } finally {
    if (requestId === state.historyRequest) { state.historyBusy = false; renderHistory(); }
  }
}

function showHistoryError(error) {
  $('history-status').textContent = `历史运行加载失败：${error.message}`;
}

function fillRunSelect(plugin) {
  const runs = [...(state.analysisCatalogs[plugin]?.runs || state.runs.filter((run) => run.plugin_type === plugin))];
  if (state.selected?.plugin_type === plugin && !runs.some((run) => run.run_id === state.selected.run_id)) runs.unshift(state.selected);
  $('result-run').innerHTML = runs.length ? runs.map((run) => `<option value="${escapeHtml(run.run_id)}">${escapeHtml(run.run_id)} · ${escapeHtml(run.dataset_version || '--')}</option>`).join('') : '<option value="">暂无运行记录</option>';
  return runs;
}

function resetResult(plugin) {
  state.selected = null;
  setReportSelection(plugin, '');
  $('execution-status').textContent = '--';
  $('completeness').innerHTML = '--<small> / --</small>';
  $('analysis-label').textContent = state.pluginViews[plugin]?.primary_metric?.label || '分析状态';
  $('analysis-value').textContent = '--';
  $('analysis-state').textContent = '等待选择运行';
  $('ai-context').textContent = `上下文：${plugin} / 尚未选择运行`;
  renderAnalysis(plugin, {});
}

function resolveViewValue(path, run) {
  if (!path || !run) return undefined;
  const context = {
    run,
    analysis: run.analysis || {},
    result_completeness: run.result_completeness || {},
    outcomes: run.outcomes || {},
  };
  return String(path).split('.').reduce((value, part) => value?.[part], context);
}

function formatViewValue(value, format = 'text', empty = '--') {
  if (value === undefined || value === null || value === '') return empty;
  if (format === 'datetime') return timeLabel(value);
  if (format === 'duration') {
    const milliseconds = Number(value);
    return Number.isFinite(milliseconds) ? (milliseconds >= 1000 ? `${(milliseconds / 1000).toFixed(1)} 秒` : `${milliseconds.toFixed(0)} ms`) : String(value);
  }
  if (format === 'percent') {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return String(value);
    const percent = Math.abs(numeric) <= 1 ? numeric * 100 : numeric;
    return `${percent.toFixed(1)}%`;
  }
  if (format === 'integer') return Number.isFinite(Number(value)) ? number(Math.round(Number(value))) : String(value);
  if (format === 'number') return Number.isFinite(Number(value)) ? Number(value).toLocaleString('zh-CN', { maximumFractionDigits: 2 }) : String(value);
  return String(value);
}

function renderViewMetric(item, run) {
  const value = formatViewValue(resolveViewValue(item.value, run), item.format, item.empty);
  return `<div class="view-metric ${escapeHtml(item.tone || 'neutral')}"><span>${escapeHtml(item.label)}</span><strong>${escapeHtml(value)}${escapeHtml(item.suffix || '')}</strong></div>`;
}

function renderViewWidget(widget, run) {
  const description = widget.description ? `<p>${escapeHtml(widget.description)}</p>` : '';
  let content = '';
  if (widget.type === 'metrics') {
    content = `<div class="view-metrics">${widget.items.map((item) => renderViewMetric(item, run)).join('')}</div>`;
  } else if (widget.type === 'key_value') {
    content = `<div class="table-wrap"><table><tbody>${widget.items.map((item) => `<tr><th>${escapeHtml(item.label)}</th><td>${escapeHtml(formatViewValue(resolveViewValue(item.value, run), item.format, item.empty))}${escapeHtml(item.suffix || '')}</td></tr>`).join('')}</tbody></table></div>`;
  } else if (widget.type === 'progress') {
    content = `<div class="view-progress-list">${widget.items.map((item) => {
      const value = Number(resolveViewValue(item.value, run) || 0);
      const total = Number(resolveViewValue(item.total, run) || 0);
      const ratio = total > 0 ? Math.max(0, Math.min(100, value / total * 100)) : 0;
      return `<div class="view-progress ${escapeHtml(item.tone || 'accent')}"><div><span>${escapeHtml(item.label)}</span><strong>${escapeHtml(formatViewValue(value, item.format))} / ${escapeHtml(formatViewValue(total, item.format))}</strong></div><div class="task-progress-track"><i style="width:${ratio}%"></i></div></div>`;
    }).join('')}</div>`;
  } else if (widget.type === 'data_table') {
    const rows = resolveViewValue(widget.source, run);
    const values = Array.isArray(rows) ? rows : [];
    content = `<div class="table-wrap"><table><thead><tr>${widget.columns.map((column) => `<th>${escapeHtml(column.label)}</th>`).join('')}</tr></thead><tbody>${values.length ? values.map((row) => `<tr>${widget.columns.map((column) => `<td>${escapeHtml(formatViewValue(String(column.value).split('.').reduce((value, part) => value?.[part], row), column.format))}</td>`).join('')}</tr>`).join('') : `<tr><td colspan="${widget.columns.length}" class="empty-row">${escapeHtml(widget.empty_text || '暂无数据')}</td></tr>`}</tbody></table></div>`;
  }
  return `<article class="view-widget view-widget-${escapeHtml(widget.type)}"><div class="view-widget-head"><h3>${escapeHtml(widget.title)}</h3>${description}</div>${content}</article>`;
}

function renderPluginView(plugin, run, analysisStatus) {
  const view = state.pluginViews[plugin];
  $('plugin-view-title').textContent = view?.title || `${plugin} 结果`;
  $('plugin-view-description').textContent = view?.description || '当前插件尚未提供展示定义。';
  $('edit-plugin-view').disabled = !view;
  $('ai-design-view').disabled = !view || state.aiBusy;
  const stateText = view?.empty_states?.[analysisStatus];
  $('plugin-view-state').hidden = !stateText || analysisStatus === 'ready';
  $('plugin-view-state').textContent = stateText || '';
  $('plugin-result-view').innerHTML = view
    ? view.widgets.map((widget) => renderViewWidget(widget, run)).join('')
    : '<div class="empty-panel"><span>◇</span><h2>没有展示定义</h2><p>请在插件中提供 result_view.json。</p></div>';
}

function renderAnalysis(plugin, run) {
  const hasRun = Boolean(run.run_id);
  const completeness = run.result_completeness || {};
  const outcomes = run.outcomes || {};
  const analysis = run.analysis || {};
  const expected = Number(completeness.expected_files ?? run.expected_files ?? 0);
  const success = Number(completeness.successful_files ?? outcomes.success ?? 0);
  const failed = Number(completeness.failed_files ?? outcomes.failed ?? 0);
  $('execution-status').textContent = ({ complete: '完整入库', partial: '部分入库', empty: '暂无结果', unknown: '数量未知' })[run.result_readiness?.status] || (hasRun ? '正在确认' : '--');
  const readiness = run.result_readiness;
  $('completeness').innerHTML = hasRun
    ? `${number(readiness?.reported_files ?? success + failed)}<small> / ${readiness?.expected_files === null ? '未知' : number(readiness?.expected_files ?? expected)}</small>` : '--<small> / --</small>';
  const analysisStatus = run.analysis_status || (Object.keys(analysis).length ? 'ready' : (hasRun ? 'pending' : 'not_selected'));
  const analysisLabels = { ready: '分析结果已发布', error: '后处理结果读取失败', not_configured: '后处理未配置', pending: '等待后处理结果', not_selected: '等待选择运行' };
  if (readiness) {
    const readinessLabels = { complete: '结果已完整入库，可分析', partial: '已有部分结果，可分析', empty: '暂无已入库结果', unknown: '已有结果，预期数量未知' };
    setAnalysisState(readiness.can_analyze ? 'ready' : 'pending', readinessLabels[readiness.status] || '正在读取数据库结果');
  } else setAnalysisState(analysisStatus, analysisLabels[analysisStatus] || '等待后处理结果');
  $('ai-context').textContent = `上下文：${plugin} / ${run.run_id || '尚未选择运行'} / 当前结果`;
  const primary = state.pluginViews[plugin]?.primary_metric;
  $('analysis-label').textContent = primary?.label || '分析状态';
  $('analysis-value').textContent = primary
    ? `${formatViewValue(resolveViewValue(primary.value, run), primary.format, primary.empty)}${primary.suffix || ''}`
    : '--';
  renderPluginView(plugin, run, analysisStatus);
  updateAiControls();
}

async function selectRun(runId, plugin, preserveCurrent = false) {
  const requestId = ++state.selectionRequest;
  state.selectionPending = requestId;
  try {
  setReportSelection(plugin, runId);
  if (!runId) {
    state.selected = null;
    resetResult(plugin);
    return;
  }
  const preserving = preserveCurrent && state.selected?.run_id === runId;
  if (!preserving) {
    state.selected = null;
    renderAnalysis(plugin, {});
    setAnalysisState('pending', '正在加载运行');
    $('ai-context').textContent = `上下文：${plugin} / ${runId} / 加载中`;
  }
  let selected;
  try {
    selected = await jsonFetch(`/api/analysis/runs/${encodeURIComponent(runId)}?plugin=${encodeURIComponent(plugin)}`);
  } catch (error) {
    if (requestId === state.selectionRequest && !preserving) {
      resetResult(plugin);
      setAnalysisState('error', '运行加载失败');
    }
    throw error;
  }
  if (requestId !== state.selectionRequest) return;
  state.selected = selected;
  fillRunSelect(plugin);
  $('result-run').value = runId;
  renderAnalysis(plugin, state.selected);
  await loadLatestReport(plugin, runId);
  } finally {
    if (state.selectionPending === requestId) state.selectionPending = 0;
  }
}

async function switchPlugin(plugin, preferredRun = '') {
  $('result-type').value = plugin;
  const selectionRequest = ++state.selectionRequest;
  state.selectionPending = selectionRequest;
  try {
  setReportSelection(plugin, preferredRun);
  state.selected = null;
  updateAiControls();
  await loadReportWorkspace(plugin);
  if ($('result-type').value !== plugin || state.selectionRequest !== selectionRequest) return;
  const runs = fillRunSelect(plugin);
  const runId = preferredRun || runs[0]?.run_id || '';
  $('result-run').value = runId;
  await selectRun(runId, plugin);
  } finally {
    if (state.selectionPending === selectionRequest) state.selectionPending = 0;
  }
}

function renderPlugins(plugins) {
  const execution = executionPluginNames();
  const names = [...new Set([...plugins.map((plugin) => plugin.name), ...execution])];
  $('plugin-grid').innerHTML = names.map((name) => {
    const plugin = plugins.find((item) => item.name === name);
    const executionState = state.scheduler?.online ? (execution.includes(name) ? '已启用' : '未启用') : '调度器离线，待确认';
    return `<article class="plugin-card"><div class="panel-head"><h2>${escapeHtml(name)}</h2></div><p>${escapeHtml(plugin?.view?.description || '此插件由调度器加载执行')}</p><div class="plugin-capability"><span>执行插件</span><b>${executionState}</b></div><div class="plugin-capability"><span>分析适配器</span><b>${plugin ? (plugin.configured ? '已配置' : '待配置') : '未提供'}</b></div><div class="plugin-capability"><span>结果展示</span><b>${plugin?.view_editable ? '可编辑' : '未提供可编辑视图'}</b></div></article>`;
  }).join('');
}

async function loadAll() {
  const requestId = ++state.loadRequest;
  // Execution controls do not depend on database readiness or report loading.
  const schedulerRefresh = loadScheduler();
  state.apiHealthy = false;
  const preferredPlugin = $('result-type').value;
  const preferredRun = $('result-run').value;
  const preferredProvider = $('provider').value;
  try {
    const [health, readiness] = await Promise.all([jsonFetch('/healthz'), statusFetch('/readyz')]);
    if (requestId !== state.loadRequest) return;
    if (health.status !== 'ok') throw new Error('结果服务健康检查失败');
    state.apiHealthy = true;

    const [plugins, providers, viewSchema] = await Promise.all([
      jsonFetch('/api/plugins'),
      jsonFetch('/api/ai/providers'),
      jsonFetch('/api/plugin-view-schema'),
    ]);
    if (requestId !== state.loadRequest) return;
    // Provider availability is independent of database queries. Publish it
    // before loading runs so an unavailable database cannot disable local AI.
    state.providers = providers;
    state.plugins = plugins;
    state.pluginViews = Object.fromEntries(plugins.filter((plugin) => plugin.view).map((plugin) => [plugin.name, plugin.view]));
    state.pluginViewSchema = viewSchema;
    renderServiceState(readiness, providers);
    const wantedProvider = $('provider').value || preferredProvider;
    $('provider').innerHTML = providers.map((provider) => `<option value="${escapeHtml(provider.name)}" ${provider.available ? '' : 'disabled'}>${escapeHtml(provider.name)}${provider.available ? '' : ' · 不可用'}</option>`).join('');
    const selectedProvider = providers.find((provider) => provider.name === wantedProvider && provider.available)
      || providers.find((provider) => provider.available);
    if (selectedProvider) $('provider').value = selectedProvider.name;
    updateAiControls();
    let overviewError = null;
    const runs = readiness.ok ? await jsonFetch('/api/runs?limit=100').catch((error) => { overviewError = error; return []; }) : [];
    if (requestId !== state.loadRequest) return;

    const currentPlugin = $('result-type').value;
    const currentRun = $('result-run').value;
    const selectionChanged = currentPlugin !== preferredPlugin || currentRun !== preferredRun;

    state.runs = runs;
    const availablePlugins = plugins.map((plugin) => plugin.name);
    const selectedPlugin = availablePlugins.includes(preferredPlugin) ? preferredPlugin : (availablePlugins[0] || '');
    $('result-type').innerHTML = plugins.map((plugin) => `<option value="${escapeHtml(plugin.name)}">${escapeHtml(plugin.name)} · ${escapeHtml(plugin.view?.title || '结果')}</option>`).join('');
    $('result-type').value = selectedPlugin;
    renderPlugins(plugins); renderOverviewFilters(plugins); renderMetrics(); renderRunCards(); renderHistory();
    if (state.page === 'history') await loadHistory();
    const livePlugin = availablePlugins.includes(currentPlugin) ? currentPlugin : selectedPlugin;
    const targetPlugin = selectionChanged ? livePlugin : selectedPlugin;
    const targetRun = selectionChanged && livePlugin === currentPlugin ? currentRun : preferredRun;
    await switchPlugin(targetPlugin, targetRun);
    if (requestId !== state.loadRequest) return;
    $('snapshot-time').textContent = new Date().toLocaleString('zh-CN', { hour12: false });
    if (overviewError) {
      $('footer-status').textContent = overviewError.message;
      setDotState('footer-dot', 'warning');
    }
    updateAiControls();
    await schedulerRefresh;
    if (state.page === 'logs') await loadLogs();
    if (state.page === 'plugins') await loadConfig($('config-kind').value || state.configKind);
  } catch (error) {
    if (requestId !== state.loadRequest) return;
    throw error;
  }
}

async function refreshProgress() {
  if (state.progressRefreshBusy || state.selectionPending || !state.readiness?.ready || document.hidden) return;
  state.progressRefreshBusy = true;
  const plugin = $('result-type').value;
  const runId = $('result-run').value;
  const loadRequest = state.loadRequest;
  const selectionRequest = state.selectionRequest;
  try {
    let overviewError = null;
    const runs = await jsonFetch('/api/runs?limit=100').catch((error) => { overviewError = error; return state.runs; });
    if (loadRequest !== state.loadRequest) return;
    state.runs = runs;
    renderMetrics(); renderRunCards(); renderHistory();
    if (state.page === 'history' && !state.historyBusy) await loadHistory();
    if (state.selectionRequest !== selectionRequest || $('result-type').value !== plugin || $('result-run').value !== runId) return;
    if (plugin) {
      const catalog = await jsonFetch(`/api/analysis/catalog?plugin=${encodeURIComponent(plugin)}`);
      if (state.selectionRequest !== selectionRequest || $('result-type').value !== plugin || $('result-run').value !== runId || loadRequest !== state.loadRequest) return;
      state.analysisCatalogs[plugin] = catalog;
    }
    const pluginRuns = fillRunSelect(plugin);
    const selectedRun = pluginRuns.some((run) => run.run_id === runId)
      ? runId
      : (pluginRuns[0]?.run_id || '');
    $('result-run').value = selectedRun;
    await selectRun(selectedRun, plugin, selectedRun === runId);
    $('snapshot-time').textContent = new Date().toLocaleString('zh-CN', { hour12: false });
    $('footer-status').textContent = overviewError ? `任务总览刷新失败：${overviewError.message}` : '结果服务与数据库运行正常';
    setDotState('footer-dot', overviewError ? 'warning' : 'online');
  } finally {
    state.progressRefreshBusy = false;
  }
}

function addMessage(role, text) {
  const node = document.createElement('div');
  node.className = `message ${role === 'user' ? 'user-message' : 'assistant-message'}`;
  node.textContent = text;
  $('chat').appendChild(node);
  $('chat').scrollTop = $('chat').scrollHeight;
  return node;
}

function updateReportControls() {
  const plugin = $('result-type').value;
  const canRun = Boolean(state.selected?.run_id && state.readiness?.configured && state.reportScripts[plugin] !== undefined);
  $('run-report').disabled = !canRun || state.reportBusy;
  $('run-report').textContent = state.reportBusy ? '正在生成…' : (state.report ? '重新运行报告' : '运行报告');
  $('edit-report-script').disabled = !plugin;
  const candidate = state.reportEditorCandidate;
  const staleCandidate = candidate && !reportCandidateMatchesSelection(candidate);
  $('save-report-editor').disabled = state.reportEditorSaving || Boolean(staleCandidate);
  $('save-run-report-editor').disabled = !canRun || state.reportBusy || state.reportEditorPlugin !== plugin || state.reportEditorSaving || Boolean(staleCandidate);
  $('report-editor-text').disabled = state.reportEditorSaving;
  $('report-candidate-index').disabled = state.reportEditorSaving || (candidate?.sources.length || 0) < 2;
  if (staleCandidate) $('report-editor-status').textContent = '运行选择已变化，此候选不能应用。请为当前运行重新生成脚本；候选编辑内容已保留。';
}

function setReportSelection(plugin, runId) {
  const key = `${plugin}/${runId || ''}`;
  if (key === state.reportSelectionKey) return;
  state.reportSelectionKey = key;
  state.reportSelectionGeneration += 1;
  state.reportRequest += 1;
  state.reportBusy = false;
  state.report = null;
  $('report-frame').hidden = true;
  $('report-frame').src = 'about:blank';
  $('report-empty').hidden = false;
  $('report-state').textContent = runId ? '运行脚本即可生成当前运行的报告。' : '选择运行后可以生成报告。';
  updateReportControls();
}

async function loadReportWorkspace(plugin) {
  if (!plugin) return;
  const generation = (state.reportLoads[plugin] || 0) + 1;
  state.reportLoads[plugin] = generation;
  const [sourceResult, catalogResult] = await Promise.allSettled([
    refreshReportScript(plugin),
    state.readiness?.ready ? jsonFetch(`/api/analysis/catalog?plugin=${encodeURIComponent(plugin)}`) : Promise.resolve(null),
  ]);
  if (state.reportLoads[plugin] !== generation) return;
  if (catalogResult.status === 'fulfilled' && catalogResult.value) state.analysisCatalogs[plugin] = catalogResult.value;
  if ($('result-type').value === plugin) {
    const failure = sourceResult.status === 'rejected' ? sourceResult.reason : (catalogResult.status === 'rejected' ? catalogResult.reason : null);
    if (failure) $('report-state').textContent = `报告准备失败：${failure.message}`;
    updateReportControls();
  }
}

async function refreshReportScript(plugin) {
  const requestId = (state.reportScriptLoads[plugin] || 0) + 1;
  state.reportScriptLoads[plugin] = requestId;
  const result = await jsonFetch(`/api/plugins/${encodeURIComponent(plugin)}/report-script`);
  if (state.reportScriptLoads[plugin] === requestId) state.reportScripts[plugin] = result.source;
  return result;
}

function showReport(report, runId) {
  if (!report.url || !report.url.startsWith('/') || report.url.startsWith('//')) throw new Error('报告服务未返回有效的站内地址');
  state.report = report;
  $('report-frame').src = report.url;
  $('report-frame').hidden = false;
  $('report-empty').hidden = true;
  $('report-state').textContent = `${runId} · 已显示最近一次生成的报告 · 数据更新后可重新运行`;
  updateReportControls();
}

async function loadLatestReport(plugin, runId) {
  if (!runId || state.reportBusy) return;
  const selectionKey = state.reportSelectionKey;
  const reportRequest = state.reportRequest;
  try {
    const result = await jsonFetch(`/api/plugins/${encodeURIComponent(plugin)}/reports?run_id=${encodeURIComponent(runId)}`);
    if (selectionKey !== state.reportSelectionKey || reportRequest !== state.reportRequest || state.reportBusy) return;
    if (result.latest && result.latest.report_id !== state.report?.report_id) showReport(result.latest, runId);
  } catch (error) {
    if (selectionKey === state.reportSelectionKey && reportRequest === state.reportRequest && !state.report) {
      $('report-state').textContent = `历史报告读取失败：${error.message}。仍可运行脚本生成报告。`;
    }
  }
}

async function runReport(plugin = $('result-type').value, runId = state.selected?.run_id) {
  if (!runId || $('result-type').value !== plugin || state.selected?.run_id !== runId || state.reportBusy) return;
  const requestId = ++state.reportRequest;
  const selectionKey = state.reportSelectionKey;
  state.reportBusy = true;
  $('report-state').textContent = '正在读取数据库并生成报告…';
  updateReportControls();
  try {
    const report = await jsonFetch(`/api/plugins/${encodeURIComponent(plugin)}/reports`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ run_id: runId, parameters: {} }),
    });
    if (requestId !== state.reportRequest || selectionKey !== state.reportSelectionKey) return;
    showReport(report, runId);
    $('report-state').textContent = `${runId} · 报告已生成 ${new Date().toLocaleTimeString('zh-CN', { hour12: false })} · 数据更新后可重新运行`;
  } catch (error) {
    if (requestId === state.reportRequest && selectionKey === state.reportSelectionKey) {
      $('report-state').textContent = `报告生成失败：${error.message}${state.report ? '。保留上一次成功生成的报告。' : ''}`;
    }
  } finally {
    if (requestId === state.reportRequest) {
      state.reportBusy = false;
      updateReportControls();
    }
  }
}

async function saveReportScript(plugin, source) {
  const saved = await jsonFetch(`/api/plugins/${encodeURIComponent(plugin)}/report-script`, {
    method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ source }),
  });
  state.reportLoads[plugin] = (state.reportLoads[plugin] || 0) + 1;
  state.reportScriptLoads[plugin] = (state.reportScriptLoads[plugin] || 0) + 1;
  state.reportScripts[plugin] = saved.source;
  if (state.reportDrafts[plugin] === source) delete state.reportDrafts[plugin];
  updateReportControls();
  return saved;
}

async function openReportEditor() {
  const plugin = $('result-type').value;
  if (!plugin || state.reportEditorSaving) return;
  if ($('report-editor-dialog').open) rememberReportDraft();
  const requestId = ++state.reportEditorRequest;
  state.reportEditorCandidate = null;
  state.reportEditorPlugin = plugin;
  state.reportEditorOriginal = state.reportScripts[plugin] ?? '';
  $('report-editor-title').textContent = '编辑 Python 报告脚本';
  $('report-candidate-picker').hidden = true;
  $('report-editor-plugin').textContent = plugin;
  $('report-editor-text').value = state.reportDrafts[plugin] ?? state.reportScripts[plugin] ?? '';
  $('report-editor-status').textContent = '保存后可用于当前插件的其他运行。';
  const dialog = $('report-editor-dialog');
  if (typeof dialog.showModal === 'function') dialog.showModal();
  else dialog.open = true;
  updateReportControls();
  $('report-editor-status').textContent = '正在检查最新脚本…';
  try {
    await refreshReportScript(plugin);
    if (requestId !== state.reportEditorRequest) return;
    state.reportEditorOriginal = state.reportScripts[plugin] ?? '';
    $('report-editor-text').value = state.reportDrafts[plugin] ?? state.reportEditorOriginal;
    $('report-editor-status').textContent = state.reportDrafts[plugin] !== undefined ? '已保留未保存的草稿。保存将替换当前插件脚本。' : '已加载最新脚本。保存后可用于当前插件的其他运行。';
  } catch (error) {
    if (requestId === state.reportEditorRequest) $('report-editor-status').textContent = `最新脚本读取失败：${error.message}`;
  }
}

function rememberReportDraft() {
  const plugin = state.reportEditorPlugin;
  const source = $('report-editor-text').value;
  if (state.reportEditorCandidate) {
    state.reportEditorCandidate.drafts[state.reportEditorCandidate.index] = source;
    return;
  }
  if (source !== state.reportEditorOriginal) state.reportDrafts[plugin] = source;
  else delete state.reportDrafts[plugin];
}

function closeReportEditor() {
  rememberReportDraft();
  state.reportEditorRequest += 1;
  const dialog = $('report-editor-dialog');
  if (typeof dialog.close === 'function') dialog.close();
  else dialog.open = false;
}

async function saveReportEditor(andRun = false) {
  const plugin = state.reportEditorPlugin;
  if (!plugin || state.reportEditorSaving) return;
  const candidate = state.reportEditorCandidate;
  if (candidate && !reportCandidateMatchesSelection(candidate)) {
    updateReportControls();
    return;
  }
  if (andRun && ($('result-type').value !== plugin || !state.selected?.run_id)) return;
  const runId = state.selected?.run_id;
  const editorRequest = state.reportEditorRequest;
  const selectionGeneration = state.reportSelectionGeneration;
  const source = $('report-editor-text').value;
  rememberReportDraft();
  state.reportEditorSaving = true;
  updateReportControls();
  $('report-editor-status').textContent = '正在保存…';
  try {
    await saveReportScript(plugin, source);
    if (editorRequest !== state.reportEditorRequest) return;
    state.reportEditorOriginal = source;
    $('report-editor-status').textContent = '脚本已保存。';
    if (andRun && selectionGeneration === state.reportSelectionGeneration) {
      closeReportEditor();
      await runReport(plugin, runId);
    }
  } catch (error) {
    if (editorRequest === state.reportEditorRequest) $('report-editor-status').textContent = `无法保存：${error.message}`;
  } finally {
    state.reportEditorSaving = false;
    updateReportControls();
  }
}

function extractReportScripts(text) {
  const blocks = String(text || '').matchAll(/(?:^|\n)[ \t]*(`{3,}|~{3,})[ \t]*(?:python|py)[ \t]*\r?\n([\s\S]*?)\r?\n[ \t]*\1[ \t]*(?=\r?\n|$)/gi);
  return [...blocks].map((match) => match[2].trim()).filter(Boolean);
}

function reportCandidateMatchesSelection(candidate) {
  return $('result-type').value === candidate.plugin && state.selected?.run_id === candidate.runId
    && state.reportSelectionGeneration === candidate.selectionGeneration;
}

function showReportCandidateSource() {
  const candidate = state.reportEditorCandidate;
  if (!candidate) return;
  $('report-candidate-index').value = String(candidate.index);
  $('report-editor-text').value = candidate.drafts[candidate.index] ?? candidate.sources[candidate.index];
  $('report-editor-status').textContent = `正在预览候选 ${candidate.index + 1} / ${candidate.sources.length}。保存将应用编辑框中的完整源码。${state.reportDrafts[candidate.plugin] !== undefined ? '原先手写的未保存草稿仍保留在“编辑 Python”中。' : ''}`;
  updateReportControls();
}

function openReportCandidate(candidate) {
  if (state.reportEditorSaving) return;
  if (!reportCandidateMatchesSelection(candidate)) {
    addMessage('assistant', `此脚本针对 ${candidate.plugin} / ${candidate.runId || '未选择运行'} 生成，运行选择已变化。请为当前运行重新生成脚本。`);
    return;
  }
  if ($('report-editor-dialog').open) rememberReportDraft();
  state.reportEditorRequest += 1;
  state.reportEditorPlugin = candidate.plugin;
  state.reportEditorCandidate = candidate;
  $('report-editor-title').textContent = '预览 AI 候选脚本';
  $('report-editor-plugin').textContent = `${candidate.plugin} / ${candidate.runId || '未选择运行'}`;
  $('report-candidate-picker').hidden = false;
  $('report-candidate-index').innerHTML = candidate.sources.map((source, index) => `<option value="${index}">候选 ${index + 1} · ${source.split('\n').length} 行</option>`).join('');
  showReportCandidateSource();
  const dialog = $('report-editor-dialog');
  if (typeof dialog.showModal === 'function') dialog.showModal();
  else dialog.open = true;
}

function addReportCandidate(plugin, sources, runId, selectionGeneration) {
  const candidate = { plugin, sources, runId, selectionGeneration, drafts: {}, index: 0 };
  const node = document.createElement('div');
  node.className = 'message assistant-message report-candidate';
  const summary = document.createElement('span');
  summary.textContent = `找到 ${sources.length} 个 ${plugin} Python 候选脚本。先查看、选择并编辑源码，再保存或保存并运行。`;
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'button primary';
  button.textContent = '查看候选脚本';
  button.addEventListener('click', () => openReportCandidate(candidate));
  node.appendChild(summary);
  node.appendChild(button);
  $('chat').appendChild(node);
}

function persistConversationIds() {
  try {
    if (typeof localStorage !== 'undefined') localStorage.setItem(AI_CONVERSATIONS_KEY, JSON.stringify(state.conversationIds));
  } catch (_) { /* A blocked local store must not prevent local AI use. */ }
}

function newAiConversation() {
  const provider = $('provider').value;
  if (provider) {
    delete state.conversationIds[provider];
    persistConversationIds();
  }
  $('chat').replaceChildren();
  addMessage('assistant', `${provider || 'AI'} 新会话已就绪。选择运行只会附加结果上下文。`);
  updateAiControls();
}

function extractViewDefinition(text, plugin) {
  const raw = String(text || '').trim();
  const fenced = raw.match(/```(?:json)?\s*([\s\S]*?)```/i);
  const candidates = [fenced?.[1], raw];
  const firstBrace = raw.indexOf('{');
  const lastBrace = raw.lastIndexOf('}');
  if (firstBrace >= 0 && lastBrace > firstBrace) candidates.push(raw.slice(firstBrace, lastBrace + 1));
  for (const candidate of candidates) {
    if (!candidate) continue;
    try {
      const value = JSON.parse(candidate);
      if (value && typeof value === 'object' && value.plugin === plugin && Array.isArray(value.widgets)) return value;
    } catch (_) { /* Try the next representation. */ }
  }
  return null;
}

async function savePluginView(plugin, definition) {
  const saved = await jsonFetch(`/api/plugins/${encodeURIComponent(plugin)}/view`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(definition),
  });
  state.pluginViews[plugin] = saved;
  const metadata = state.plugins.find((item) => item.name === plugin);
  if (metadata) metadata.view = saved;
  if ($('result-type').value === plugin) renderAnalysis(plugin, state.selected || {});
  renderPlugins(state.plugins);
  return saved;
}

function addViewCandidate(plugin, definition) {
  const node = document.createElement('div');
  node.className = 'message assistant-message view-candidate';
  const summary = document.createElement('span');
  summary.textContent = `已生成 ${definition.widgets.length} 个展示组件。服务端会先校验定义，再保存并立即重绘结果页。`;
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'button primary';
  button.textContent = '应用此展示';
  button.addEventListener('click', async () => {
    button.disabled = true;
    button.textContent = '正在应用';
    try {
      await savePluginView(plugin, definition);
      button.textContent = '已应用';
      addMessage('assistant', `${plugin} 的结果展示已保存并刷新。`);
    } catch (error) {
      button.disabled = false;
      button.textContent = '重新应用';
      addMessage('assistant', `展示定义未应用：${error.message}`);
    }
  });
  node.appendChild(summary);
  node.appendChild(button);
  $('chat').appendChild(node);
}

async function askAi(message, options = {}) {
  if (!message.trim()) return;
  const provider = $('provider').value;
  const resultType = $('result-type').value;
  const reportRunId = state.selected?.run_id;
  const reportSelectionGeneration = state.reportSelectionGeneration;
  if (!state.providers.some((item) => item.name === provider && item.available)) { addMessage('assistant', '当前 AI 服务不可用。'); return; }
  state.aiBusy = true;
  updateAiControls();
  addMessage('user', options.displayMessage || message);
  const waiting = document.createElement('div');
  waiting.className = 'message assistant-message'; waiting.textContent = `${provider} 正在分析...`; $('chat').appendChild(waiting);
  try {
    const result = await jsonFetch('/api/ai/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        provider,
        message,
        conversation_id: state.conversationIds[provider] || null,
        context: {
          run: state.selected,
          result_type: resultType,
          ...(options.viewDesign ? {
            purpose: 'plugin_result_view_design',
            current_view: state.pluginViews[resultType],
            view_schema: state.pluginViewSchema,
          } : {}),
          ...(options.reportDesign ? {
            purpose: 'analysis_report_design',
            current_script: state.reportScripts[resultType] || '',
            script_contract: state.analysisCatalogs[resultType]?.script_contract,
            analysis_catalog: state.analysisCatalogs[resultType],
          } : {}),
        },
      }),
    });
    if (result.conversation_id) {
      state.conversationIds[provider] = result.conversation_id;
      persistConversationIds();
    }
    waiting.textContent = result.text;
    if (options.viewDesign) {
      const definition = extractViewDefinition(result.text, resultType);
      if (definition) addViewCandidate(resultType, definition);
      else addMessage('assistant', '回复中没有可应用的展示 JSON。可以补充要求后再次生成。');
    }
    if (options.reportDesign) {
      const sources = extractReportScripts(result.text);
      if (sources.length) addReportCandidate(resultType, sources, reportRunId, reportSelectionGeneration);
      else addMessage('assistant', '回复中没有可预览的 Python 代码块。请让 AI 使用 python 或 py 围栏提供完整脚本。尚未保存或运行任何脚本。');
    }
  } catch (error) { waiting.textContent = `请求失败：${error.message}`; }
  finally { state.aiBusy = false; updateAiControls(); }
  $('chat').scrollTop = $('chat').scrollHeight;
}

async function designPluginView() {
  const plugin = $('result-type').value;
  const requirements = $('question').value.trim();
  $('question').value = '';
  $('console').dataset.aiOpen = 'true';
  selectAiTab('chat');
  const prompt = `为 ${plugin} 插件生成结果展示定义。${requirements ? `使用者要求：${requirements}` : '保持信息清晰，突出最重要的分析指标。'} 严格遵循上下文中的 view_schema，只引用上下文允许的数据路径。plugin 必须是 ${plugin}。只回复一个 JSON 对象，不要附加解释或 Markdown。`;
  await askAi(prompt, { viewDesign: true, displayMessage: requirements || `为 ${plugin} 设计结果展示` });
}

async function designPythonReport() {
  const plugin = $('result-type').value;
  if (!plugin || state.aiBusy) return;
  const requirements = $('question').value.trim();
  $('question').value = '';
  $('console').dataset.aiOpen = 'true';
  selectAiTab('chat');
  const prompt = `为 ${plugin} 插件生成可替换的 Python 交互报告脚本。${requirements ? `使用者要求：${requirements}` : '先说明数据库中已有结果的完整性，再展示有用的图表和表格。'} 遵循上下文中的 script_contract 和真实数据库表结构。实现 render(connection, context) 并返回独立 HTML 字符串，connection 为只读数据库连接，行是字典，context['run_id'] 是当前运行。查询只依赖数据库中的结果，不依赖任务执行过程或调度状态。HTML 的样式和交互脚本内联，不依赖外部网络。以一个完整的 python 代码块回复。`;
  await askAi(prompt, { reportDesign: true, displayMessage: requirements || `为 ${plugin} 生成 Python 交互报告` });
}

function openViewEditor() {
  const plugin = $('result-type').value;
  const definition = state.pluginViews[plugin];
  if (!definition) return;
  $('view-editor-plugin').textContent = plugin;
  $('view-editor-text').value = JSON.stringify(definition, null, 2);
  $('view-editor-status').textContent = '保存后立即应用到结果页';
  const dialog = $('view-editor-dialog');
  if (typeof dialog.showModal === 'function') dialog.showModal();
  else dialog.open = true;
}

function closeViewEditor() {
  const dialog = $('view-editor-dialog');
  if (typeof dialog.close === 'function') dialog.close();
  else dialog.open = false;
}

async function saveViewEditor() {
  const plugin = $('result-type').value;
  $('save-view-editor').disabled = true;
  try {
    const definition = JSON.parse($('view-editor-text').value);
    await savePluginView(plugin, definition);
    closeViewEditor();
    addMessage('assistant', `${plugin} 的结果展示已保存并刷新。`);
  } catch (error) {
    $('view-editor-status').textContent = `无法保存：${error.message}`;
  } finally {
    $('save-view-editor').disabled = false;
  }
}

async function submitChat() {
  const question = $('question');
  const message = question.value;
  if (!message.trim() || state.aiBusy) return;
  question.value = '';
  await askAi(message);
}

function selectAiTab(tab) {
  document.querySelectorAll('[data-ai-tab]').forEach((button) => {
    button.setAttribute('aria-selected', String(button.dataset.aiTab === tab));
  });
  $('ai-chat-panel').hidden = tab !== 'chat';
  $('dsh-native-panel').hidden = tab !== 'dsh';
  if (tab === 'dsh' && !state.dshWebUrl) openDshWeb();
}

async function openDshWeb() {
  if (state.dshWebLoading) return;
  const dshAvailable = state.providers.some((item) => item.name === 'dsh' && item.available);
  if (!dshAvailable) {
    $('dsh-native-status').textContent = '服务器 DSH 命令不可用。';
    return;
  }
  state.dshWebLoading = true;
  $('dsh-native-open').disabled = true;
  $('dsh-native-open').textContent = '正在启动';
  $('dsh-native-status').textContent = '正在启动服务器 DSH Web...';
  try {
    const result = await jsonFetch('/api/ai/dsh-web', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
    if (!result.url) throw new Error('服务未返回 DSH Web 地址');
    state.dshWebUrl = result.url;
    $('dsh-native-frame').src = result.url;
    $('dsh-native-frame').hidden = false;
    $('dsh-native-status').textContent = 'DSH 已启动，正在连接界面...';
    $('dsh-native-open').textContent = '重新连接';
  } catch (error) {
    state.dshWebUrl = '';
    $('dsh-native-frame').hidden = true;
    $('dsh-native-status').textContent = `启动失败：${error.message}`;
    $('dsh-native-open').textContent = '重试';
  } finally {
    state.dshWebLoading = false;
    updateAiControls();
  }
}

function setPanelWidth(width, persist = true, remember = true) {
  const viewport = Number(window.innerWidth || 1440);
  const sidebarWidth = viewport <= 1440 ? 164 : 196;
  const mainMinWidth = Math.min(720, Math.max(480, viewport * 0.375));
  const maxWidth = viewport <= 1024
    ? Math.max(PANEL_MIN_WIDTH, viewport - 24)
    : Math.max(PANEL_MIN_WIDTH, Math.floor(Math.min(1600, viewport - sidebarWidth - mainMinWidth)));
  const requestedWidth = Math.max(PANEL_MIN_WIDTH, Number(width) || 400);
  if (remember) state.panelPreferredWidth = requestedWidth;
  state.panelWidth = Math.min(requestedWidth, maxWidth);
  if (typeof $('console').style.setProperty === 'function') $('console').style.setProperty('--assistant-width', `${state.panelWidth}px`);
  $('ai-resize').setAttribute('aria-valuenow', String(state.panelWidth));
  $('ai-resize').setAttribute('aria-valuemin', String(PANEL_MIN_WIDTH));
  $('ai-resize').setAttribute('aria-valuemax', String(maxWidth));
  if (persist) {
    try { if (typeof localStorage !== 'undefined') localStorage.setItem(AI_PANEL_WIDTH_KEY, String(state.panelWidth)); } catch (_) { /* Keep the current in-memory width. */ }
  }
}

function setAiExpanded(expanded) {
  $('console').dataset.aiExpanded = String(expanded);
  $('ai-expand').textContent = expanded ? '↙' : '↗';
  $('ai-expand').title = expanded ? '恢复 AI 助手宽度' : '展开 AI 助手';
  $('ai-expand').setAttribute('aria-label', $('ai-expand').title);
}

function toggleAiExpanded() {
  const expanded = $('console').dataset.aiExpanded === 'true';
  if (expanded) {
    setPanelWidth(state.panelWidthBeforeExpand || 400);
    state.panelWidthBeforeExpand = 0;
    setAiExpanded(false);
    return;
  }
  state.panelWidthBeforeExpand = state.panelWidth;
  setPanelWidth(Math.max(720, Number(window.innerWidth || 1440) * 0.5), false, false);
  setAiExpanded(true);
}

function setUiScale(value, persist = true) {
  state.uiScale = Math.max(0.9, Math.min(1.5, Math.round(Number(value || 1.2) * 10) / 10));
  if (document.documentElement?.style) document.documentElement.style.fontSize = `${16 * state.uiScale}px`;
  const inverse = 100 / state.uiScale;
  $('dsh-native-frame').style.width = `${inverse}%`;
  $('dsh-native-frame').style.height = `${inverse}%`;
  $('dsh-native-frame').style.transform = `scale(${state.uiScale})`;
  $('text-smaller').disabled = state.uiScale <= 0.9;
  $('text-larger').disabled = state.uiScale >= 1.5;
  $('text-smaller').title = `缩小界面文字（当前 ${Math.round(state.uiScale * 100)}%）`;
  $('text-larger').title = `放大界面文字（当前 ${Math.round(state.uiScale * 100)}%）`;
  if (persist) {
    try { if (typeof localStorage !== 'undefined') localStorage.setItem(UI_SCALE_KEY, String(state.uiScale)); } catch (_) { /* Keep the current in-memory scale. */ }
  }
}

document.querySelectorAll('.nav-item').forEach((button) => button.addEventListener('click', () => navigate(button.dataset.page)));
document.querySelectorAll('[data-prompt]').forEach((button) => button.addEventListener('click', () => askAi(button.dataset.prompt)));
$('overview-filter').addEventListener('click', (event) => { const button = event.target.closest('button'); if (!button) return; state.filter = button.dataset.filter; document.querySelectorAll('#overview-filter button').forEach((item) => item.setAttribute('aria-pressed', String(item === button))); renderRunCards(); });
$('run-start-form').addEventListener('submit', async (event) => { event.preventDefault(); await schedulerCommand('run', $('run-plugin').value); });
$('run-plugin').addEventListener('change', renderScheduler);
$('run-wakeup').addEventListener('click', () => schedulerCommand('wakeup', $('run-plugin').value));
$('active-runs').addEventListener('click', (event) => {
  const button = event.target.closest('[data-stop-run]');
  if (button && !button.disabled) return schedulerCommand('stop', button.dataset.stopPlugin, button.dataset.stopRun);
});
$('clients-refresh').addEventListener('click', loadScheduler);
$('logs-form').addEventListener('submit', async (event) => { event.preventDefault(); await loadLogs(); });
$('logs-file').addEventListener('change', loadLogs);
$('logs-level').addEventListener('change', loadLogs);
$('logs-limit').addEventListener('change', loadLogs);
$('config-kind').addEventListener('change', () => loadConfig($('config-kind').value));
$('config-json').addEventListener('input', rememberConfigDraft);
$('config-reload').addEventListener('click', () => { if (!configDirty()) return loadConfig(state.configKind, true); });
$('config-discard').addEventListener('click', () => loadConfig(state.configKind, true));
$('config-save').addEventListener('click', saveConfig);
$('run-cards').addEventListener('click', async (event) => { const button = event.target.closest('[data-open-run]'); if (!button) return; navigate('results'); await switchPlugin(button.dataset.openPlugin, button.dataset.openRun); });
$('history-table').addEventListener('click', async (event) => { const row = event.target.closest('[data-open-run]'); if (!row) return; navigate('results'); await switchPlugin(row.dataset.openPlugin, row.dataset.openRun); });
$('history-search-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  state.historySearch = $('history-search').value.trim();
  await loadHistory(0).catch(showHistoryError);
});
$('history-prev').addEventListener('click', () => loadHistory(Math.max(0, state.historyOffset - HISTORY_PAGE_SIZE)).catch(showHistoryError));
$('history-next').addEventListener('click', () => loadHistory(state.historyOffset + HISTORY_PAGE_SIZE).catch(showHistoryError));
$('open-run-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const runId = $('open-run-id').value.trim();
  if (!runId) return;
  $('open-run-status').textContent = '正在打开…';
  try {
    await selectRun(runId, $('result-type').value, true);
    $('open-run-status').textContent = '';
  } catch (error) { $('open-run-status').textContent = `无法打开运行：${error.message}`; }
});
$('result-type').addEventListener('change', () => switchPlugin($('result-type').value).catch((error) => addMessage('assistant', error.message)));
$('result-run').addEventListener('change', () => selectRun($('result-run').value, $('result-type').value).catch((error) => addMessage('assistant', error.message)));
$('provider').addEventListener('change', updateAiControls);
$('refresh').addEventListener('click', () => loadAll().catch(showLoadError));
$('ai-toggle').addEventListener('click', () => { $('console').dataset.aiOpen = String($('console').dataset.aiOpen !== 'true'); });
$('ai-close').addEventListener('click', () => { $('console').dataset.aiOpen = 'false'; });
$('ai-expand').addEventListener('click', toggleAiExpanded);
$('text-smaller').addEventListener('click', () => setUiScale(state.uiScale - 0.1));
$('text-larger').addEventListener('click', () => setUiScale(state.uiScale + 0.1));
$('edit-plugin-view').addEventListener('click', openViewEditor);
$('close-view-editor').addEventListener('click', closeViewEditor);
$('save-view-editor').addEventListener('click', saveViewEditor);
$('ai-design-view').addEventListener('click', designPluginView);
$('design-view-from-chat').addEventListener('click', designPythonReport);
$('ai-design-report').addEventListener('click', designPythonReport);
$('run-report').addEventListener('click', () => runReport());
$('edit-report-script').addEventListener('click', openReportEditor);
$('close-report-editor').addEventListener('click', closeReportEditor);
$('report-editor-text').addEventListener('input', rememberReportDraft);
$('report-candidate-index').addEventListener('change', () => {
  const candidate = state.reportEditorCandidate;
  if (!candidate || state.reportEditorSaving) return;
  const index = Number($('report-candidate-index').value);
  if (!Number.isInteger(index) || index < 0 || index >= candidate.sources.length) return;
  rememberReportDraft();
  candidate.index = index;
  showReportCandidateSource();
});
$('report-editor-dialog').addEventListener('cancel', () => { rememberReportDraft(); state.reportEditorRequest += 1; });
$('save-report-editor').addEventListener('click', () => saveReportEditor());
$('save-run-report-editor').addEventListener('click', () => saveReportEditor(true));
$('new-conversation').addEventListener('click', newAiConversation);
$('dsh-native-open').addEventListener('click', openDshWeb);
$('dsh-native-frame').addEventListener('load', () => { $('dsh-native-status').textContent = 'DSH 原生界面已连接'; });
$('dsh-native-frame').addEventListener('error', () => { $('dsh-native-status').textContent = 'DSH 界面加载失败，请重新连接。'; });
document.querySelectorAll('[data-ai-tab]').forEach((button) => button.addEventListener('click', () => selectAiTab(button.dataset.aiTab)));
$('chat-form').addEventListener('submit', async (event) => { event.preventDefault(); await submitChat(); });
$('question').addEventListener('keydown', async (event) => {
  if (event.key !== 'Enter' || event.shiftKey || event.isComposing) return;
  event.preventDefault();
  await submitChat();
});

let resizeStart = null;
$('ai-resize').addEventListener('pointerdown', (event) => {
  setAiExpanded(false);
  state.panelWidthBeforeExpand = 0;
  resizeStart = { x: event.clientX, width: state.panelWidth };
  document.body.classList.toggle('resizing-ai', true);
  if (typeof event.currentTarget?.setPointerCapture === 'function') event.currentTarget.setPointerCapture(event.pointerId);
  event.preventDefault();
});
$('ai-resize').addEventListener('keydown', (event) => {
  if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
  setPanelWidth(state.panelWidth + (event.key === 'ArrowLeft' ? 24 : -24));
  event.preventDefault();
});
if (typeof window.addEventListener === 'function') {
  window.addEventListener('pointermove', (event) => { if (resizeStart) setPanelWidth(resizeStart.width + resizeStart.x - event.clientX, false); });
  const finishResize = () => {
    if (!resizeStart) return;
    resizeStart = null;
    document.body.classList.toggle('resizing-ai', false);
    setPanelWidth(state.panelWidth);
  };
  window.addEventListener('pointerup', finishResize);
  window.addEventListener('pointercancel', finishResize);
  $('ai-resize').addEventListener('lostpointercapture', finishResize);
  window.addEventListener('resize', () => setPanelWidth(state.panelPreferredWidth, false, false));
}

function showLoadError(error) {
  state.selectionRequest += 1;
  state.runs = [];
  state.selected = null;
  const plugin = $('result-type').value;
  fillRunSelect(plugin);
  renderMetrics(); renderRunCards(); renderHistory(); resetResult(plugin);
  $('health').textContent = state.apiHealthy ? '结果数据加载失败' : '结果服务异常';
  $('api-status').textContent = state.apiHealthy ? 'API 已连接' : 'API 连接失败';
  $('footer-status').textContent = error.message;
  setDotState('health-dot', 'offline');
  setDotState('api-dot', state.apiHealthy ? 'online' : 'offline');
  setDotState('footer-dot', 'offline');
  addMessage('assistant', `加载失败：${error.message}`);
}

$('snapshot-time').textContent = '--';
setPanelWidth(state.panelWidth, false);
setUiScale(state.uiScale, false);
if (window.matchMedia('(max-width: 1024px)').matches) $('console').dataset.aiOpen = 'false';
navigate('results');
loadAll().catch(showLoadError);
if (typeof window.setInterval === 'function') {
  window.setInterval(() => refreshProgress().catch((error) => {
    $('footer-status').textContent = `进度刷新失败：${error.message}`;
    setDotState('footer-dot', 'warning');
  }), 5000);
  window.setInterval(async () => {
    if (document.hidden) return;
    if (['overview', 'clients', 'plugins'].includes(state.page)) await loadScheduler();
    if (state.page === 'logs' && $('logs-auto').checked && !state.logsBusy) await loadLogs();
  }, 5000);
}
