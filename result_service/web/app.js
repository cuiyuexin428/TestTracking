const AI_OPERATOR_KEY = 'testtracking.ai.operator-id.v1';
const AI_PANEL_WIDTH_KEY = 'testtracking.ai.panel-width.v1';
const UI_SCALE_KEY = 'testtracking.ui.scale.v1';
const PANEL_MIN_WIDTH = 360;
const HISTORY_PAGE_SIZE = 50;

function readStoredNumber(key, fallback) {
  try {
    if (typeof localStorage === 'undefined') return fallback;
    const value = Number(localStorage.getItem(key));
    return Number.isFinite(value) && value > 0 ? value : fallback;
  } catch (_) { return fallback; }
}

function createUuid() {
  const bytes = new Uint8Array(16);
  if (globalThis.crypto?.getRandomValues) globalThis.crypto.getRandomValues(bytes);
  else for (let index = 0; index < bytes.length; index += 1) bytes[index] = Math.floor(Math.random() * 256);
  bytes[6] = (bytes[6] & 15) | 64;
  bytes[8] = (bytes[8] & 63) | 128;
  const hex = Array.from(bytes, (value) => value.toString(16).padStart(2, '0')).join('');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function readOperatorId() {
  try {
    const stored = typeof localStorage === 'undefined' ? '' : (localStorage.getItem(AI_OPERATOR_KEY) || '');
    if (/^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(stored)) return stored.toLowerCase();
    const generated = createUuid();
    if (typeof localStorage !== 'undefined') localStorage.setItem(AI_OPERATOR_KEY, generated);
    return generated;
  } catch (_) { return createUuid(); }
}

const storedPanelWidth = readStoredNumber(AI_PANEL_WIDTH_KEY, 400);
const state = {
  runs: [], plugins: [], pluginViews: {}, pluginViewSchema: null,
  selected: null, page: 'results', filter: '', readiness: null,
  providers: [], selectionRequest: 0, selectionPending: 0, loadRequest: 0, apiHealthy: false,
  operatorId: readOperatorId(),
  panelWidth: storedPanelWidth, panelPreferredWidth: storedPanelWidth,
  dshWebUrl: '', dshWebLoading: false, dshWebAttempted: false,
  panelWidthBeforeExpand: 0, uiScale: readStoredNumber(UI_SCALE_KEY, 1.2),
  progressRefreshBusy: false,
  analysisCatalogs: {}, reportScripts: {}, reportScriptRevisions: {}, reportLoads: {}, reportDrafts: {}, reportDraftBases: {},
  reportScriptLoads: {},
  report: null, reportRequest: 0, reportBusy: false, reportSelectionKey: '', reportSelectionGeneration: 0,
  reportEditorPlugin: '', reportEditorRequest: 0, reportEditorOriginal: '', reportEditorRevision: '',
  reportEditorSaving: false, reportEditorLoading: false, reportEditorConflicted: false, reportConflictDrafts: {},
  viewEditorPlugin: '', viewEditorRevision: '',
  historyRuns: [], historyOffset: 0, historySearch: '', historyHasMore: false,
  historyRequest: 0, historyBusy: false,
  scheduler: null, schedulerRequest: 0, schedulerBusy: false, commandBusy: false,
  agents: null, agentsBusy: false, agentsLoad: null, agentsError: '', selectedAgentIds: [],
  deploymentBatchTargets: null, deploymentBatchReview: null, deploymentBatchHistory: null,
  selectedWorkerIds: [],
  artifacts: [], deployments: [], artifactBusy: false, deploymentBusy: false,
  workspaceTab: 'plugins', artifactSearch: '', artifactFilter: '', workspaceDrawerTrigger: null,
  deploymentStep: 0, deploymentEditorOpen: false,
  deploymentClient: '', deploymentRevision: null, deploymentSelected: {}, deploymentDraftLoaded: false,
  deploymentWizard: null, deploymentWizardDrafts: {}, deploymentEnabledPlugins: [], deploymentWizardSource: '', deploymentWizardDirty: false, deploymentPreview: '', deploymentBinding: null,
  serverPluginPreview: null, serverPluginBusy: false,
  logsRequest: 0, logsBusy: false, logsLoaded: false,
  configKind: 'scheduler', configs: {}, configDrafts: {}, configRequest: 0, configBusy: false, configFormInvalid: false, configFormErrors: {},
};
const $ = (id) => document.getElementById(id);

const pages = {
  overview: ['运行总览', '查看持久化任务进度；记录状态不代表调度进程当前在线。'],
  results: ['结果分析', '从已入库结果生成可替换的 Python 交互报告。'],
  clients: ['客户端', '选择空闲机器，配置插件和资源，追踪每台机器的执行与部署。'],
  history: ['历史运行', '查看长期保存的运行记录。'],
  plugins: ['插件与部署', '管理插件和制品，按步骤配置客户端部署。'],
  logs: ['运行日志', '查询服务器日志，按级别和内容筛选。']
};

async function jsonFetch(url, options) {
  const response = await fetch(url, options);
  const data = await response.json();
  if (!response.ok) {
    const error = new Error(data.error || `HTTP ${response.status}`);
    error.status = response.status;
    error.data = data;
    throw error;
  }
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
function statusLabel(value) { return ({ queued: '排队中', finished: '已完成', running: '运行中', planning: '规划中', planned: '已计划', stopped: '已停止', failed: '失败', interrupted: '已中断', cancelled: '已取消' })[value] || value || '--'; }
function timeLabel(value) { return value ? new Date(value).toLocaleString('zh-CN', { hour12: false }) : '--'; }
function executionChipClass(value) { return value === 'running' ? 'blue' : (value === 'queued' || value === 'stopped' || value === 'planning' || value === 'planned' ? 'amber' : (value === 'failed' || value === 'interrupted' ? 'red' : '')); }

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
  const dshAvailable = Boolean(providers.find((provider) => provider.name === 'dsh')?.available);
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

  $('provider-status').textContent = dshAvailable ? 'AI 可用：DSH' : 'AI 服务不可用';
  const noticeParts = [];
  if (!ready) noticeParts.push(configured ? '只读数据库当前无法连接。' : '请配置只读数据库连接以读取运行记录。');
  if (!dshAvailable) noticeParts.push('未找到可用的 DSH 命令，请安装并确保命令位于 PATH 中。');
  $('system-notice').hidden = noticeParts.length === 0;
  $('notice-title').textContent = !ready
    ? (configured ? '数据库连接异常' : '数据库未配置')
    : 'AI 服务不可用';
  $('notice-detail').textContent = noticeParts.join(' ');
}

function updateAiControls() {
  const provider = state.providers.find((item) => item.name === 'dsh');
  const hasProvider = Boolean(provider?.available);
  $('ai-design-view').disabled = !state.pluginViews[$('result-type').value];
  $('ai-design-report').disabled = !$('result-type').value;
  const providerStatus = $('provider-availability');
  providerStatus.className = hasProvider ? 'available' : 'unavailable';
  providerStatus.textContent = hasProvider ? 'DSH 可用' : 'DSH 未就绪';
  $('dsh-native-open').disabled = state.dshWebLoading;
  if (!hasProvider && !state.dshWebAttempted) $('dsh-native-status').textContent = '服务器 DSH 命令未就绪，配置后可重新连接。';
  $('dsh-context-copy').disabled = !$('dsh-context-prompt').value;
  ensureDshWeb();
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
  if (page === 'clients') { loadAgents(); loadArtifacts(); }
  if (page === 'logs') loadLogs();
  if (page === 'plugins') loadConfig($('config-kind').value || state.configKind);
  if (page === 'plugins') loadArtifacts();
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
  const queued = state.scheduler?.queued_runs || [];
  const selectedActive = active.some((run) => run.plugin_type === $('run-plugin').value);
  $('run-plugin').disabled = !online || state.commandBusy || !names.length;
  $('run-dataset').disabled = !online || state.commandBusy;
  $('run-params').disabled = !online || state.commandBusy;
  $('run-all-clients').disabled = !online || state.commandBusy;
  $('run-clients-field').hidden = $('run-all-clients').checked;
  const candidates = (state.scheduler?.clients || []).filter((item) => item.registered && item.plugin_type === $('run-plugin').value);
  $('run-clients').innerHTML = candidates.map((item) => `<option value="${escapeHtml(item.worker_id)}" ${state.selectedWorkerIds.includes(item.worker_id) ? 'selected' : ''} ${item.status === 'busy' && !state.selectedWorkerIds.includes(item.worker_id) ? 'disabled' : ''}>${escapeHtml(item.worker_id)} · ${item.status === 'busy' ? '处理中' : '空闲'}</option>`).join('');
  $('run-clients').disabled = !online || state.commandBusy || $('run-all-clients').checked;
  $('run-force').disabled = !online || state.commandBusy;
  $('run-force-reason').disabled = !online || state.commandBusy || !$('run-force').checked;
  $('run-force-reason-field').hidden = !$('run-force').checked;
  $('run-start').disabled = !online || state.commandBusy || !names.length;
  $('run-start').textContent = selectedActive ? '加入队列' : '启动运行';
  $('run-start').title = selectedActive ? '该插件正在运行，新任务会进入持久队列' : '';
  $('run-wakeup').disabled = !online || state.commandBusy || !names.length;
  const activeRows = active.map((run) => `<div class="active-run"><div><strong>${escapeHtml(run.plugin_type)} · 执行中</strong><span>${escapeHtml(run.run_id)}</span></div><button type="button" class="button" data-stop-run="${escapeHtml(run.run_id)}" data-stop-plugin="${escapeHtml(run.plugin_type)}" ${state.commandBusy || run.stopping ? 'disabled' : ''}>${run.stopping ? '正在停止…' : '停止运行'}</button></div>`);
  const queuedRows = queued.map((run, index) => `<div class="active-run"><div><strong>${escapeHtml(run.plugin_type)} · ${run.blocked ? '等待恢复原配置' : `队列 ${index + 1}`}</strong><span>${escapeHtml(run.run_id)} · 数据集 ${escapeHtml(run.dataset_version || '--')}</span>${run.blocked ? `<span class="management-status warning">${escapeHtml(run.blocked_reason || '配置或数据已变化，请恢复原配置，或取消后创建新运行。')}</span>` : ''}${run.worker_ids?.length ? `<span>指定客户端：${escapeHtml(run.worker_ids.join(', '))}</span>` : ''}</div><button type="button" class="button" data-stop-run="${escapeHtml(run.run_id)}" data-stop-plugin="${escapeHtml(run.plugin_type)}" ${state.commandBusy ? 'disabled' : ''}>取消排队</button></div>`);
  $('active-runs').innerHTML = online && (activeRows.length || queuedRows.length) ? [...activeRows, ...queuedRows].join('') : `<p class="management-status">${online ? '当前没有活动运行。' : '执行控制暂不可用，已入库结果仍可分析。'}</p>`;
  renderClients();
  renderPlugins(state.plugins);
}

function renderClients() {
  if (state.agents !== null) return renderAgentClients();
  const online = Boolean(state.scheduler?.online);
  const delayed = Number(state.scheduler?.snapshot_age_ms || 0) > 5000;
  const clients = online ? (state.scheduler.clients || []) : [];
  const machines = new Map();
  for (const client of clients) {
    const physical = client.physical_client_id || client.worker_id;
    if (!machines.has(physical)) machines.set(physical, []);
    machines.get(physical).push(client);
  }
  $('client-count').textContent = online ? `${machines.size} 台${clients.length > machines.size ? ` · ${clients.length} 个插件进程` : ''}` : '--';
  $('clients-status').textContent = online ? (delayed ? `调度器响应中，以下为缓存状态 · 更新延迟 ${(state.scheduler.snapshot_age_ms / 1000).toFixed(1)} 秒` : '来自调度器的实时状态 · 每 5 秒刷新') : '调度器当前不可达，无法确认客户端是否在线。';
  $('client-list').innerHTML = clients.length ? [...machines].map(([physical, workers]) => `<article class="client-card"><div class="panel-head"><h2>${escapeHtml(physical)}</h2><span class="chip ${workers.some((item) => item.status === 'busy') ? 'blue' : 'green'}">${workers.length} 个插件进程</span></div>${workers.map((client) => {
    const tasks = client.tasks || [];
    const heartbeat = Number.isFinite(Number(client.last_seen_age_ms)) ? `${(Number(client.last_seen_age_ms) / 1000).toFixed(1)} 秒前` : '--';
    return `<section class="client-plugin"><div class="panel-head"><h3>${escapeHtml(client.plugin_key || client.plugin_type)}</h3><span class="chip ${client.registered ? (client.status === 'busy' ? 'blue' : 'green') : 'amber'}">${client.registered ? (client.status === 'busy' ? '处理中' : '空闲') : '尚未注册'}</span></div><div class="client-facts"><span>插件 <b>${escapeHtml(client.plugin_type)}</b></span><span>心跳 <b>${heartbeat}</b></span><span>地址 <b>${escapeHtml(client.ip || client.peer_address || '--')}</b></span><span>内存 <b>${formatBytes(client.memory_bytes)}</b></span><span>可用磁盘 <b>${formatBytes(client.disk_free_bytes)}</b></span><span>CPU <b>${client.cpu_percent === null || client.cpu_percent === undefined ? '--' : `${Number(client.cpu_percent).toFixed(0)}%`}</b></span></div><p class="client-info">${escapeHtml(client.worker_id)} · ${escapeHtml(client.plugin_info || '')}</p>${tasks.length ? `<div class="table-wrap"><table><thead><tr><th>当前任务</th><th>运行</th><th>尝试</th><th>状态</th></tr></thead><tbody>${tasks.map((task) => `<tr><td>${escapeHtml(task.task_id)}</td><td>${escapeHtml(task.run_id)}</td><td>${escapeHtml(task.attempt_id)}</td><td>${task.acked ? '客户端已确认' : '等待确认'}</td></tr>`).join('')}</tbody></table></div>` : '<p class="management-status">当前没有分配的任务</p>'}</section>`;
  }).join('')}</article>`).join('') : `<div class="empty-panel"><h2>${online ? '暂无客户端连接' : '调度器离线'}</h2><p>${online ? '启动客户端后，这里会显示连接、心跳和任务。' : '恢复调度服务后会自动更新。'}</p></div>`;
}

function agentUnavailableReason(agent) {
  if (state.agentsError) return '无法确认在线状态';
  if (!agent?.online) return '离线';
  if (agent.busy || (agent.workers || []).some((worker) => worker.status === 'busy' || (worker.tasks || []).length)) return '正在执行任务';
  if (agent.state === 'updating' || agent.reservation_id) return '正在更新';
  if (agent.configurable === false || !['ready', 'unconfigured', 'starting', 'error'].includes(agent.state)) return '管理进程尚未就绪';
  return '';
}

function renderAgentClients() {
  const agents = state.agents || [];
  const labels = { unconfigured: '待配置', ready: '已就绪', starting: '正在启动', updating: '正在更新', error: '需要处理' };
  $('client-count').textContent = `${agents.length} 台`;
  $('clients-status').textContent = state.agentsError ? `客户端状态读取失败：${state.agentsError}。状态恢复后可配置。` : '客户端 Agent 独立上线；仅整机空闲时可以配置。每 5 秒刷新。';
  $('client-list').innerHTML = agents.map((agent) => {
    const reason = agentUnavailableReason(agent);
    const workers = agent.workers || [];
    const status = !agent.online ? '离线' : agent.busy || workers.some((item) => item.status === 'busy' || item.tasks?.length) ? '处理中' : agent.reservation_id ? '等待更新' : labels[agent.state] || agent.state;
    const deployment = state.deployments.find((item) => item.client_id === agent.client_id);
    const configHint = reason ? `${reason}，暂不能配置。` : agent.state === 'starting' ? '插件正在启动或等待注册，当前整机空闲，可修改配置以修正程序版本等问题。' : agent.state === 'unconfigured' ? 'Agent 已连接，选择插件、程序和数据后即可开始执行。' : '';
    return `<article class="client-card ${state.selectedAgentIds.includes(agent.client_id) ? 'selected' : ''}"><div class="panel-head"><label class="client-select"><input type="checkbox" data-agent-select="${escapeHtml(agent.client_id)}" aria-label="选择 ${escapeHtml(agent.client_id)}" ${state.selectedAgentIds.includes(agent.client_id) ? 'checked' : ''} ${(reason && !state.selectedAgentIds.includes(agent.client_id)) || state.deploymentBusy ? 'disabled' : ''}><span><strong>${escapeHtml(agent.hostname || agent.client_id)}</strong><small>${escapeHtml(agent.client_id)}</small></span></label><span class="chip ${!agent.online || ['error', 'starting'].includes(agent.state) ? 'amber' : reason ? 'blue' : 'green'}">${escapeHtml(status)}</span><button type="button" class="button" data-configure-agent="${escapeHtml(agent.client_id)}" ${reason || state.deploymentBusy ? 'disabled' : ''}>配置</button></div><div class="client-facts"><span>执行插件 <b>${escapeHtml((agent.plugin_keys || []).join('、') || '尚未配置')}</b></span><span>心跳 <b>${Number.isFinite(Number(agent.last_seen_age_ms)) ? `${(Number(agent.last_seen_age_ms) / 1000).toFixed(1)} 秒前` : '--'}</b></span><span>Agent 版本 <b>${escapeHtml(agent.agent_version || '--')}</b></span><span>更新时间窗 <b>${agent.update_window_open === false ? '尚未开放' : '允许更新'}</b></span></div>${configHint ? `<p class="management-status">${escapeHtml(configHint)}</p>` : ''}${deployment ? `<p class="management-status">部署：${escapeHtml(({ current: '已生效', pending: '等待应用', applying: '正在应用', failed: '应用失败' })[deployment.state] || deployment.state)}${deployment.reported?.message ? ` · ${escapeHtml(deployment.reported.message)}` : ''}</p>` : ''}${workers.length ? `<details class="client-workers" ${reason === '正在执行任务' ? 'open' : ''}><summary>${workers.length} 个插件进程与任务详情</summary>${workers.map((worker) => `<section class="client-plugin"><div class="panel-head"><h3>${escapeHtml(worker.plugin_key || worker.plugin_type)}</h3><span class="chip ${worker.status === 'busy' ? 'blue' : 'green'}">${worker.registered ? worker.status === 'busy' ? '处理中' : '空闲' : '尚未注册'}</span></div><p class="client-info">${escapeHtml(worker.worker_id)} · ${escapeHtml(worker.plugin_info || '')}</p><div class="client-facts"><span>内存 <b>${formatBytes(worker.memory_bytes)}</b></span><span>可用磁盘 <b>${formatBytes(worker.disk_free_bytes)}</b></span><span>CPU <b>${worker.cpu_percent == null ? '--' : `${Number(worker.cpu_percent).toFixed(0)}%`}</b></span></div>${worker.tasks?.length ? `<div class="table-wrap"><table><thead><tr><th>当前任务</th><th>运行</th><th>状态</th></tr></thead><tbody>${worker.tasks.map((task) => `<tr><td>${escapeHtml(task.task_id)}</td><td>${escapeHtml(task.run_id)}</td><td>${task.acked ? '客户端已确认' : '等待确认'}</td></tr>`).join('')}</tbody></table></div>` : '<p class="management-status">当前没有分配的任务</p>'}</section>`).join('')}</details>` : ''}</article>`;
  }).join('') || '<div class="empty-panel"><h2>还没有客户端 Agent</h2><p>在客户端包填写服务器地址并启动 Agent。即使尚未启用插件，也会在这里显示。</p></div>';
  const available = agents.filter((agent) => !agentUnavailableReason(agent));
  const invalid = state.selectedAgentIds.some((id) => !agents.some((agent) => agent.client_id === id && !agentUnavailableReason(agent)));
  const waiting = agents.filter((agent) => state.selectedAgentIds.includes(agent.client_id) && agent.update_window_open === false).length;
  $('clients-selected-count').textContent = state.selectedAgentIds.length ? `已选 ${state.selectedAgentIds.length} 台${invalid ? ' · 部分机器当前不可用' : ''}${waiting ? ` · ${waiting} 台将等待更新时间窗，期间暂停领取任务` : ''}` : `可配置 ${available.length} 台`;
  $('clients-configure').disabled = state.deploymentBusy || !state.selectedAgentIds.length || invalid;
  $('clients-select-all').checked = available.length > 0 && available.every((agent) => state.selectedAgentIds.includes(agent.client_id));
  $('clients-select-all').indeterminate = state.selectedAgentIds.length > 0 && (!available.length || !available.every((agent) => state.selectedAgentIds.includes(agent.client_id)) || invalid);
  $('clients-select-all').disabled = state.deploymentBusy || (!available.length && !state.selectedAgentIds.length);
  if (state.deploymentBatchTargets) renderBatchTargets();
}

function loadAgents() {
  if (state.agentsLoad) return state.agentsLoad;
  state.agentsBusy = true;
  state.agentsLoad = (async () => {
    try {
      const result = await jsonFetch('/api/agents');
      state.agents = result.agents || []; state.agentsError = '';
    } catch (error) { state.agentsError = error.message; if (state.agents === null) state.agents = []; }
    finally { state.agentsBusy = false; state.agentsLoad = null; renderClients(); }
  })();
  return state.agentsLoad;
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
  const payload = { action, plugin_type: plugin };
  if (action === 'run') {
    payload.dataset_version = $('run-dataset').value.trim();
    payload.params = $('run-params').value;
    if (!$('run-all-clients').checked) {
      payload.worker_ids = state.selectedWorkerIds.filter((id) => (state.scheduler.clients || []).some((item) => item.worker_id === id && item.plugin_type === plugin && item.registered));
      if (!payload.worker_ids.length) { $('scheduler-command-status').textContent = '请选择执行客户端，或勾选全部匹配客户端。'; return; }
    }
    payload.force_rerun = $('run-force').checked;
    if (payload.force_rerun) payload.force_rerun_reason = $('run-force-reason').value.trim();
    if (!payload.dataset_version) { $('scheduler-command-status').textContent = '请输入数据版本。'; return; }
    if (payload.force_rerun && !payload.force_rerun_reason) { $('scheduler-command-status').textContent = '强制重跑需要填写原因。'; return; }
  }
  if (action === 'stop') {
    const active = (state.scheduler.active_runs || []).find((run) => run.run_id === runId && run.plugin_type === plugin);
    const queued = (state.scheduler.queued_runs || []).find((run) => run.run_id === runId && run.plugin_type === plugin);
    if ((!active && !queued) || active?.stopping) return;
    payload.run_id = runId;
  }
  state.commandBusy = true;
  renderScheduler();
  $('scheduler-command-status').textContent = '正在提交命令…';
  try {
    const result = await jsonFetch('/api/scheduler/commands', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
    $('scheduler-command-status').textContent = action === 'run' ? `${result.status === 'queued' ? '已加入队列' : '已启动运行'} ${result.run_id || ''}` : action === 'stop' ? `${result.status === 'cancelled' ? '已取消排队' : '已请求停止'} ${runId}` : `已唤醒 ${plugin} 客户端`;
    await loadScheduler();
  } catch (error) {
    const duplicate = error.data;
    const duplicateState = { finished: '相同任务已全部完成', running: '相同任务正在执行', queued: '相同任务已在排队', planning: '相同任务正在规划', stopped: '相同任务已停止，仍有待恢复的进度' };
    $('scheduler-command-status').textContent = duplicate?.duplicate === true
      ? `${duplicateState[duplicate.status] || `已存在相同任务（${statusLabel(duplicate.status)}）`}。已有运行：${duplicate.run_id || '--'}。${duplicate.force_rerun_available === true ? '确需重新执行时，请勾选“强制重跑”并填写原因。' : ''}`
      : `命令失败：${error.message}`;
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

function commonConfigFields(config = state.configs[state.configKind]) {
  const common = /^(host|port|scheduler_management_url|db_conn|logs_dir|server\.(router_endpoint|management_endpoint|client_timeout_ms|task_lease_timeout_ms)|scheduler\.endpoint|client\.(worker_id|task_threads)|plugins\.[^.]+\.(enabled|task_size|stp_manifest|expected_dataset_version|expected_program_version|expected_baseline_program_version|expected_candidate_program_version)|providers\.dsh\.(enabled|command|cwd|timeout_seconds))$/;
  const labels = { enabled: '启用状态', stp_manifest: 'STP 清单', expected_dataset_version: '预期数据版本', expected_program_version: '预期程序版本', expected_baseline_program_version: '预期老程序版本（baseline）', expected_candidate_program_version: '预期新程序版本（candidate）', command: '启动命令', cwd: '工作目录', timeout_seconds: '超时时间（秒）' };
  return (config?.fields || []).filter((field) => common.test(field.path) && ['string', 'number', 'boolean'].includes(field.type)).map((field) => {
    const key = field.path.split('.').pop();
    return {
      ...field,
      label: !field.label || field.label === key ? (labels[key] || field.label || key) : field.label,
      description: key === 'stp_manifest' ? '服务端调度器使用的 STP 清单路径。请与客户端数据集及服务端预期数据版本保持一致；上传数据制品不会自动替换此清单。' : field.description,
    };
  });
}

function renderConfigForm() {
  const container = $('config-form');
  if (!container) return;
  state.configFormInvalid = false;
  state.configFormErrors = {};
  let draft;
  try { draft = JSON.parse($('config-json').value || '{}'); }
  catch (_) { container.innerHTML = '<p class="config-help">高级 JSON 尚未完整，请修正后继续使用常用配置。</p>'; return; }
  const fields = commonConfigFields();
  container.innerHTML = fields.length ? fields.map((field, index) => {
    const value = deploymentGet(draft, field.path);
    const id = `config-common-${index}`;
    const group = field.path.startsWith('plugins.') ? field.path.split('.')[1] : '';
    const input = field.type === 'boolean'
      ? `<select id="${id}" data-config-field="${escapeHtml(field.path)}" data-config-type="boolean"><option value="true" ${value === true ? 'selected' : ''}>启用</option><option value="false" ${value !== true ? 'selected' : ''}>禁用</option></select>`
      : `<input id="${id}" type="${field.type === 'number' ? 'number' : 'text'}" data-config-field="${escapeHtml(field.path)}" data-config-type="${field.type}" value="${escapeHtml(value ?? '')}" autocomplete="off">`;
    return `<div class="management-field config-common-field"><label for="${id}">${group ? `${escapeHtml(group)} · ` : ''}${escapeHtml(field.label)}</label>${input}<small>${escapeHtml(field.description)}</small></div>`;
  }).join('') : '<p class="config-help">此配置暂无常用字段，可展开高级配置查看和编辑全部内容。</p>';
  for (const [index, field] of fields.entries()) {
    const input = $(`config-common-${index}`);
    input.value = String(deploymentGet(draft, field.path) ?? '');
    input.disabled = state.configBusy || !state.configs[state.configKind]?.available;
  }
  if ($('config-form-status')) $('config-form-status').textContent = '';
}

function editCommonConfig(event) {
  const input = event.target.closest('[data-config-field]');
  if (!input || state.configBusy || !state.configs[state.configKind]?.available) return;
  const field = commonConfigFields().find((item) => item.path === input.dataset.configField);
  if (!field) return;
  try {
    const draft = JSON.parse($('config-json').value || '{}');
    if (!deploymentObject(draft)) throw new Error('高级配置必须是 JSON 对象');
    let value = input.value;
    if (field.type === 'boolean') value = value === 'true';
    if (field.type === 'number') {
      if (!value.trim() || !Number.isFinite(Number(value))) throw new Error(`${field.label}需要填写数字`);
      value = Number(value);
    }
    deploymentSet(draft, field.path, value);
    $('config-json').value = JSON.stringify(draft, null, 2);
    delete state.configFormErrors[field.path];
    state.configFormInvalid = Object.keys(state.configFormErrors).length > 0;
    $('config-form-status').textContent = state.configFormInvalid ? Object.values(state.configFormErrors).join('；') : '修改已加入草稿，保存后按提示重启服务。';
    rememberConfigDraft();
  } catch (error) {
    state.configFormInvalid = true;
    state.configFormErrors[field.path] = error.message;
    $('config-form-status').textContent = error.message;
    updateConfigControls();
  }
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
  $('config-save').disabled = !available || !dirty || state.configBusy || state.configFormInvalid;
  $('config-json').disabled = !available || state.configBusy;
  $('config-kind').disabled = state.configBusy;
  $('config-reload').disabled = state.configBusy || dirty;
  $('config-discard').hidden = !dirty;
  $('config-discard').disabled = state.configBusy;
  $('config-dirty').hidden = !dirty;
  commonConfigFields().forEach((_, index) => { const input = $(`config-common-${index}`); if (input) input.disabled = !available || state.configBusy; });
}

function renderConfig() {
  const config = state.configs[state.configKind];
  $('config-path').textContent = config?.path || '';
  $('config-notice').textContent = [config?.notice, ...(config?.warnings || []), config?.restart_required ? '配置已更新，需重启相应服务生效。' : ''].filter(Boolean).join(' ');
  $('config-notice').classList.toggle('warning', Boolean(config?.warnings?.length));
  $('config-json').value = state.configDrafts[state.configKind] ?? (config?.available ? JSON.stringify(config.config, null, 2) : '');
  $('config-fields').innerHTML = config?.fields?.length ? `<div class="table-wrap"><table><thead><tr><th>配置字段</th><th>类型</th><th>说明</th></tr></thead><tbody>${config.fields.map((field) => `<tr><td><strong>${escapeHtml(field.label)}</strong><br><code>${escapeHtml(field.path)}</code></td><td>${escapeHtml(field.type)}</td><td>${escapeHtml(field.description)}</td></tr>`).join('')}</tbody></table></div>` : '';
  $('config-effective').textContent = config?.effective_values ? JSON.stringify(config.effective_values, null, 2) : '';
  renderConfigForm();
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
  if (state.configBusy || state.configFormInvalid || !state.configs[state.configKind]?.available) return;
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
  $('ai-design-view').disabled = !view;
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

function setWorkspaceTab(tab, focus = false) {
  if (!['plugins', 'artifacts', 'deployments', 'settings'].includes(tab)) return;
  state.workspaceTab = tab;
  document.querySelectorAll('[data-workspace-tab]').forEach((button) => {
    const selected = button.dataset.workspaceTab === tab;
    button.setAttribute('aria-selected', String(selected));
    button.tabIndex = selected ? 0 : -1;
    button.classList.toggle('active', selected);
    if (selected && focus) button.focus?.();
  });
  document.querySelectorAll('[data-workspace-panel]').forEach((panel) => { panel.hidden = panel.dataset.workspacePanel !== tab; });
  if (tab === 'deployments') navigate('clients');
}

function openWorkspaceDrawer(title, content, trigger = document.activeElement) {
  const drawer = $('workspace-drawer');
  if (!drawer) return;
  state.workspaceDrawerTrigger = trigger;
  $('workspace-drawer-title').textContent = title;
  $('workspace-drawer-content').innerHTML = content;
  drawer.hidden = false;
  if (!drawer.open) drawer.showModal?.();
  $('workspace-drawer-close').focus?.();
}

function closeWorkspaceDrawer() {
  $('workspace-drawer').close?.();
  $('workspace-drawer').hidden = true;
  state.workspaceDrawerTrigger?.focus?.();
}

function openPluginDetails(name, trigger) {
  const plugin = state.plugins.find((item) => item.name === name);
  const enabled = executionPluginNames().includes(name);
  openWorkspaceDrawer(`${name} · 插件详情`, `<p class="drawer-intro">${escapeHtml(plugin?.view?.description || '由调度器加载的任务执行插件。')}</p><dl class="detail-list"><div><dt>任务执行</dt><dd>${state.scheduler?.online ? enabled ? '调度器已启用' : '调度器未启用' : '调度器离线，待确认'}</dd></div><div><dt>数据库分析</dt><dd>${plugin ? plugin.configured ? '分析适配器已配置' : '分析适配器待配置' : '未提供分析适配器'}</dd></div><div><dt>结果展示</dt><dd>${plugin?.view_editable ? '支持编辑展示定义和 Python 报告' : '由插件提供展示能力'}</dd></div></dl><p class="config-help">执行插件负责结果入库；结果分析读取数据库，可独立修改和运行。</p><div class="drawer-actions"><button type="button" class="button primary" data-workspace-go="deployments">配置客户端部署</button><button type="button" class="button" data-workspace-go="settings">查看服务配置</button></div>`, trigger);
}

function renderPlugins(plugins) {
  const execution = executionPluginNames();
  const names = [...new Set([...plugins.map((plugin) => plugin.name), ...execution])];
  $('plugin-grid').innerHTML = names.map((name) => {
    const plugin = plugins.find((item) => item.name === name);
    const executionState = state.scheduler?.online ? (execution.includes(name) ? '已启用' : '未启用') : '调度器离线，待确认';
    return `<article class="workspace-plugin-card"><div class="workspace-plugin-title"><span class="workspace-plugin-icon">${name === 'coverage' ? 'C' : name === 'stpcompare' ? 'S' : escapeHtml(name.slice(0, 1).toUpperCase())}</span><div><h3>${escapeHtml(name)}</h3><span>${name === 'coverage' ? '覆盖率采集' : name === 'stpcompare' ? '新老程序结果采集' : '任务执行插件'}</span></div><span class="chip ${state.scheduler?.online && execution.includes(name) ? 'green' : 'amber'}">${executionState}</span></div><p class="workspace-plugin-description">${escapeHtml(plugin?.view?.description || '此插件由调度器加载执行')}</p><div class="workspace-plugin-capabilities"><span>分析适配器 <b>${plugin ? (plugin.configured ? '已配置' : '待配置') : '未提供'}</b></span><span>结果展示 <b>${plugin?.view_editable ? '可编辑' : '未提供'}</b></span></div><div class="workspace-plugin-footer"><span>执行与分析独立维护</span><button type="button" class="button" data-plugin-detail="${escapeHtml(name)}">查看详情</button></div></article>`;
  }).join('') || '<div class="workspace-empty"><strong>还没有加载插件</strong><p>安装服务端插件包并重启服务后，可在这里查看插件状态。</p></div>';
}

const deploymentGroups = {
  shared: [
    { id: 'plugin', kind: 'plugin', title: '插件程序', fields: [['dll', '插件 DLL 路径', 'path']] },
    { id: 'dataset', kind: 'dataset', title: 'STP 数据', fields: [['stp_root', '数据目录（包内根目录可留空）', 'root']] },
  ],
  coverage: [
    { id: 'coverage', kind: 'program', title: '覆盖率业务程序', fields: [['program_dir', '程序目录（包内根目录可留空）', 'root'], ['target_exe', '被执行程序路径', 'path'], ['engine_dlls', '采集 DLL（每行一个；绑定程序时留空表示不限制模块）', 'paths']] },
    { id: 'collector', kind: 'program', title: '覆盖率采集工具', fields: [['opencppcoverage', 'OpenCppCoverage 程序路径', 'path']] },
  ],
  stpcompare: ['baseline', 'candidate'].map((role) => ({ id: role, kind: 'program', title: role === 'baseline' ? '老程序 · baseline' : '新程序 · candidate', fields: [[`programs.${role}.executable`, '可执行程序路径', 'path'], [`programs.${role}.version_file`, '版本文件路径（留空使用程序旁 version.txt）', 'optionalPath'], [`programs.${role}.version`, '注册版本（留空读取版本文件）', 'version'], [`programs.${role}.arguments`, '参数模板（每行一个参数，需包含 {stp} 和 {outdir}）', 'arguments']] })),
};
function wizardGroups(plugin) { return [...deploymentGroups.shared, ...(deploymentGroups[plugin] || [])]; }
function deploymentObject(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }
function deploymentGet(value, path) { return path.split('.').reduce((node, key) => node?.[key], value); }
function deploymentSet(value, path, next) {
  const parts = path.split('.'); const last = parts.pop();
  for (const key of parts) {
    if (value[key] !== undefined && !deploymentObject(value[key])) throw new Error(`${path} 的父字段必须为对象，请使用高级 JSON 修正`);
    if (!value[key]) value[key] = {};
    value = value[key];
  }
  value[last] = next;
}
function deploymentUnset(value, path) {
  const parts = path.split('.'); const last = parts.pop();
  for (const key of parts) { value = value?.[key]; if (!deploymentObject(value)) return; }
  delete value[last];
}
function deploymentReference(value) { return typeof value === 'string' ? value.match(/^\{artifact:([^}]+)\}(?:[\\/](.*))?$/) : null; }
function deploymentFieldId(group, path) { return `deployment-field-${group}-${path.replaceAll('.', '-')}`; }
function deploymentComponents() { return Object.entries(state.deploymentSelected).map(([slot, artifact_id]) => ({ slot, artifact_id })); }

// These transformations do not mutate the saved patch. Unknown plugin and client
// options survive editing; only fields represented by the chosen form change.
function readDeploymentWizard(patch, components, requestedPlugin, incremental = false) {
  const enabled = Object.entries(patch.plugins || {}).find(([, item]) => item?.enabled === true)?.[0];
  const plugin = requestedPlugin || (['coverage', 'stpcompare'].includes(enabled) ? enabled : 'coverage');
  const config = patch.plugins?.[plugin] || {};
  const groups = {};
  for (const group of wizardGroups(plugin)) {
    const reference = group.fields.map(([path]) => deploymentReference(deploymentGet(config, path))).find(Boolean);
    const slot = reference?.[1] || `${group.kind}.${group.id === 'plugin' ? plugin : group.id === 'dataset' ? 'stp' : group.id === 'coverage' ? 'coverage-tool' : group.id === 'collector' ? 'opencppcoverage' : `stp-${group.id}`}`;
    const values = {};
    for (const [path, , type] of group.fields) {
      let value = deploymentGet(config, path);
      const strip = (item) => { const ref = deploymentReference(item); return ref && ref[1] === slot ? (ref[2] || '') : item; };
      if (type === 'arguments') value = (value || (incremental ? [] : ['--input', '{stp}', '--output-dir', '{outdir}'])).join('\n');
      else if (type === 'paths') value = (value || []).map(strip).join('\n');
      else value = strip(value);
      values[path] = value ?? '';
    }
    groups[group.id] = { slot, artifactId: reference ? (components.find((item) => item.slot === slot)?.artifact_id || '') : '', values };
  }
  const windows = {};
  for (const kind of ['execution', 'update']) {
    const value = patch.client?.[`${kind}_windows`];
    windows[kind] = { mode: value === undefined ? 'inherit' : value.length ? 'custom' : 'all', items: JSON.parse(JSON.stringify(value || [])) };
  }
  return { plugin, groups, windows, incremental, threads: patch.client?.task_threads ?? '', dryRun: config.dry_run ?? null, storeCov: config.store_cov ?? null };
}

function buildDeploymentWizard(patch, components, model, artifacts, knownPlugins = []) {
  if (!deploymentObject(patch)) throw new Error('配置覆盖必须是 JSON 对象');
  if (Array.isArray(model.enabledPlugins)) {
    if (!model.enabledPlugins.length) throw new Error('请至少启用一个执行插件');
    let result = { config_patch: patch, components };
    const slots = new Map();
    // Keep bindings used outside the fields this form will replace. In
    // particular, editing coverage must not retarget a custom/disabled plugin
    // that still references the same dataset or program slot.
    const preserved = JSON.parse(JSON.stringify(patch));
    for (const name of model.enabledPlugins) {
      if (!['coverage', 'stpcompare'].includes(name) || !deploymentObject(preserved.plugins?.[name])) continue;
      for (const group of wizardGroups(name)) {
        for (const [path] of group.fields) deploymentUnset(preserved.plugins?.[name], path);
      }
    }
    const originalSlots = new Map(components.map((item) => [item.slot, item.artifact_id]));
    const reserveReferences = (value) => {
      if (value && typeof value === 'object') Object.values(value).forEach(reserveReferences);
      else {
        const reference = deploymentReference(value);
        if (reference && originalSlots.has(reference[1])) slots.set(reference[1], originalSlots.get(reference[1]));
      }
    };
    reserveReferences(preserved);
    for (const name of model.enabledPlugins) {
      if (!['coverage', 'stpcompare'].includes(name)) continue;
      const draft = JSON.parse(JSON.stringify(name === model.plugin ? model : model.pluginModels?.[name] || readDeploymentWizard(patch, components, name, model.incremental)));
      delete draft.enabledPlugins; delete draft.pluginModels;
      draft.threads = model.threads; draft.windows = model.windows;
      for (const input of Object.values(draft.groups)) {
        if (!input.artifactId) continue;
        if (slots.has(input.slot) && slots.get(input.slot) !== input.artifactId) input.slot = `${input.slot}.${name}`;
        if (slots.has(input.slot) && slots.get(input.slot) !== input.artifactId) throw new Error(`槽位 ${input.slot} 引用了不同制品，请使用高级 JSON 分配独立槽位`);
        slots.set(input.slot, input.artifactId);
      }
      try { result = buildDeploymentWizard(result.config_patch, result.components, draft, artifacts, knownPlugins); }
      catch (error) { throw new Error(`${name}：${error.message}`); }
    }
    const next = JSON.parse(JSON.stringify(result.config_patch));
    for (const name of new Set(['demo', 'coverage', 'stpcompare', ...knownPlugins, ...Object.keys(next.plugins || {}), ...model.enabledPlugins])) deploymentSet(next, `plugins.${name}.enabled`, model.enabledPlugins.includes(name));
    applyDeploymentCommon(next, model);
    return { ...result, config_patch: next };
  }
  const next = JSON.parse(JSON.stringify(patch));
  const plan = new Map(components.map((item) => [item.slot, item.artifact_id]));
  if (!['coverage', 'stpcompare'].includes(model.plugin)) throw new Error('请选择 coverage 或 stpcompare');
  for (const name of new Set(['demo', 'coverage', 'stpcompare', ...knownPlugins, ...Object.keys(next.plugins || {})])) deploymentSet(next, `plugins.${name}.enabled`, name === model.plugin);
  const hasProgramArtifact = wizardGroups(model.plugin).some((group) => group.kind === 'program' && model.groups[group.id].artifactId);
  if (hasProgramArtifact && typeof model.dryRun !== 'boolean') throw new Error('分发程序制品时，请明确选择真实执行或模拟执行，以便核对程序身份');
  if (typeof model.dryRun === 'boolean') deploymentSet(next, `plugins.${model.plugin}.dry_run`, model.dryRun);
  const config = next.plugins[model.plugin];
  const previousConfig = patch.plugins?.[model.plugin] || {};
  if (model.dryRun === null) delete config.dry_run;
  if (model.plugin === 'coverage' && typeof model.storeCov === 'boolean') config.store_cov = model.storeCov;
  else if (model.plugin === 'coverage') delete config.store_cov;
  const formSlots = new Map();
  const detachedSlots = new Set();
  for (const group of wizardGroups(model.plugin)) {
    const input = model.groups[group.id];
    const artifact = input.artifactId ? artifacts.find((item) => item.artifact_id === input.artifactId) : null;
    if (input.artifactId && (!artifact || artifact.kind !== group.kind)) throw new Error(`${group.title}：请选择有效的${group.kind}制品`);
    if (artifact) {
      if (formSlots.has(input.slot) && formSlots.get(input.slot) !== artifact.artifact_id) throw new Error(`槽位 ${input.slot} 被两个不同制品占用，请在高级 JSON 中分配独立槽位`);
      formSlots.set(input.slot, artifact.artifact_id); plan.set(input.slot, artifact.artifact_id);
    } else if (group.fields.some(([path]) => deploymentReference(deploymentGet(previousConfig, path))?.[1] === input.slot)) {
      detachedSlots.add(input.slot);
    }
    const resolve = (value, root = false) => {
      const path = value.trim();
      if (!artifact || deploymentReference(path)) return path;
      const relative = path.replaceAll('\\', '/').replace(/^\.\//, '');
      if ((!relative && !root) || relative.startsWith('/') || relative.includes(':') || relative.split('/').includes('..')) throw new Error(`${group.title}：请填写有效的包内相对路径`);
      return `{artifact:${input.slot}}${relative ? '/' + relative : ''}`;
    };
    for (const [path, , type] of group.fields) {
      const value = String(input.values[path] ?? '');
      if (model.incremental && !value.trim() && ['arguments', 'version', 'optionalPath'].includes(type)) continue;
      if (type === 'arguments') {
        const args = value.split(/\r?\n/).filter((item) => item.trim());
        if (!args.some((item) => item.includes('{stp}')) || !args.some((item) => item.includes('{outdir}'))) throw new Error(`${group.title}参数必须包含 {stp} 和 {outdir}`);
        deploymentSet(config, path, args);
      } else if (type === 'version') deploymentSet(config, path, value.trim());
      else if (type === 'paths') { if (artifact || value.trim() || detachedSlots.has(input.slot)) deploymentSet(config, path, value.split(/\r?\n/).filter((item) => item.trim()).map((item) => resolve(item))); }
      else if (type === 'optionalPath') { if (value.trim()) deploymentSet(config, path, resolve(value)); else deploymentSet(config, path, ''); }
      else if (artifact || value.trim()) deploymentSet(config, path, resolve(value, type === 'root'));
      else if (detachedSlots.has(input.slot) && deploymentReference(deploymentGet(previousConfig, path))) deploymentUnset(config, path);
    }
  }
  // Leaving a managed executable must also leave its generated identity. Local
  // programs return to version.txt unless the user explicitly supplies a version.
  const versions = model.plugin === 'coverage'
    ? [['target_exe', 'program_version']]
    : ['baseline', 'candidate'].map((role) => [`programs.${role}.executable`, `programs.${role}.version`]);
  for (const [executable, version] of versions) {
    const previous = deploymentGet(previousConfig, version);
    if (deploymentReference(deploymentGet(previousConfig, executable)) && !deploymentReference(deploymentGet(config, executable))
        && typeof previous === 'string' && previous.startsWith('sha256:') && deploymentGet(config, version) === previous) deploymentSet(config, version, '');
  }
  const referencedSlots = new Set();
  const collectSlots = (value) => {
    if (value && typeof value === 'object') Object.values(value).forEach(collectSlots);
    else { const reference = deploymentReference(value); if (reference) referencedSlots.add(reference[1]); }
  };
  collectSlots(next);
  for (const slot of detachedSlots) if (!formSlots.has(slot) && !referencedSlots.has(slot)) plan.delete(slot);
  if (model.plugin === 'coverage' && model.groups.coverage.artifactId && !config.opencppcoverage) throw new Error('分发覆盖率程序时，请填写覆盖率采集工具路径或选择其制品');
  applyDeploymentCommon(next, model);
  return { config_patch: next, components: [...plan].map(([slot, artifact_id]) => ({ slot, artifact_id })) };
}

function applyDeploymentCommon(next, model) {
  if (String(model.threads).trim()) {
    const count = Number(model.threads);
    if (!Number.isInteger(count) || count < 1) throw new Error('任务并发数必须为正整数');
    deploymentSet(next, 'client.task_threads', count);
  } else if (next.client) delete next.client.task_threads;
  for (const kind of ['execution', 'update']) {
    const window = model.windows[kind];
    if (window.mode === 'inherit') { if (next.client) delete next.client[`${kind}_windows`]; continue; }
    if (window.mode === 'custom' && !window.items.length) throw new Error('指定时间窗至少需要一个时段，全天运行请选择“全天”');
    const items = window.mode === 'all' ? [] : window.items.map((item) => {
      if (!/^([01]\d|2[0-3]):[0-5]\d$/.test(item.start) || !/^([01]\d|2[0-3]):[0-5]\d$/.test(item.end)) throw new Error('时间窗需要有效的开始和结束时间');
      const days = item.days || [];
      if (!Array.isArray(days) || days.some((day) => !Number.isInteger(day) || day < 1 || day > 7)) throw new Error('星期应为 1 至 7 的数字，逗号分隔');
      return item.days === undefined ? { ...item } : { ...item, days: [...new Set(days)] };
    });
    deploymentSet(next, `client.${kind}_windows`, items);
  }
}

function setDeploymentBusy(busy) {
  state.deploymentBusy = busy;
  $('deployment-form')?.querySelectorAll?.('input, select, textarea, button').forEach((input) => {
    if (busy) { input.dataset.busyDisabled = String(input.disabled); input.disabled = true; }
    else if (input.dataset.busyDisabled !== undefined) { input.disabled = input.dataset.busyDisabled === 'true'; delete input.dataset.busyDisabled; }
  });
  for (const id of ['deployment-client', 'deployment-load', 'deployment-new', 'deployment-close', 'deployment-reset', 'deployment-next', 'deployment-back', 'deployment-preview', 'deployment-editor-mode', 'deployment-save', 'deployment-rollback-button']) {
    if ($(id)) $(id).disabled = busy;
  }
  if (!busy) {
    if ($('deployment-bind-scheduler')) $('deployment-bind-scheduler').disabled = !state.deploymentBinding?.program_bindings?.length || Boolean(state.deploymentBatchTargets && (!state.deploymentBatchReview || state.deploymentWizardDirty || !Object.keys(state.deploymentBinding?.scheduler_plugins || {}).length));
    updateDeploymentMode();
  }
  if (state.deploymentBatchTargets) $('deployment-rollback-button').disabled = busy || state.deploymentBatchTargets.length !== 1 || !state.deploymentBatchHistory?.history?.length;
  if (state.agents !== null) renderClients();
}

function showDeploymentStep(step, focus = false) {
  state.deploymentStep = Math.max(0, Math.min(4, Number(step) || 0));
  const wizard = $('deployment-editor-mode').value === 'wizard';
  document.querySelectorAll('[data-deployment-stage]').forEach((section) => { section.hidden = Number(section.dataset.deploymentStage) !== state.deploymentStep || Boolean(state.deploymentBatchTargets && section.dataset.deploymentResource !== undefined && !batchScopes().resources); });
  document.querySelectorAll('[data-deployment-step]').forEach((button) => {
    const index = Number(button.dataset.deploymentStep);
    button.setAttribute('aria-current', index === state.deploymentStep ? 'step' : 'false');
    button.classList.toggle('active', index === state.deploymentStep);
    button.classList.toggle('complete', index < state.deploymentStep);
    button.disabled = state.deploymentBusy || index > state.deploymentStep || (!wizard && index > 0 && index < 4);
  });
  if ($('deployment-back')) $('deployment-back').hidden = state.deploymentStep === 0;
  if ($('deployment-next')) {
    $('deployment-next').hidden = state.deploymentStep === 4;
    $('deployment-next').textContent = state.deploymentStep === 3 || (!wizard && state.deploymentStep === 0) ? '核对部署' : '下一步';
  }
  $('deployment-save').hidden = state.deploymentEditorOpen && state.deploymentStep !== 4;
  $('deployment-preview').hidden = !wizard || (state.deploymentEditorOpen && state.deploymentStep !== 4);
  const keepSchedule = Boolean(state.deploymentBatchTargets && !batchScopes().schedule);
  $('deployment-schedule-fields').disabled = state.deploymentBusy || keepSchedule;
  $('deployment-schedule-inherit').hidden = !keepSchedule;
  $('deployment-rollback-section').hidden = Boolean(state.deploymentBatchTargets && state.deploymentBatchTargets.length !== 1);
  $('deployment-reset').hidden = !state.deploymentBatchTargets;
  $('deployment-plugin-scope-help').textContent = state.deploymentBatchTargets && !batchScopes().plugins
    ? '勾选本次需要修改程序或数据的插件。各机器当前的插件启用状态保持原值。'
    : '勾选部署后需要启用的插件；未勾选的已知插件将停用。程序和数据按本次修改范围配置。';
  if (state.deploymentStep === 4) renderDeploymentSummary();
  if (focus) {
    const firstResource = state.deploymentWizard ? wizardGroups(state.deploymentWizard.plugin).find((group) => group.id !== 'plugin') : null;
    const target = [ state.deploymentBatchTargets ? 'batch-scope-plugins' : 'deployment-client', 'deployment-plugin', firstResource ? `deployment-artifact-${firstResource.id}` : 'deployment-dry-run', keepSchedule ? 'deployment-next' : 'deployment-threads', 'deployment-summary' ][state.deploymentStep];
    $(target)?.focus?.({ preventScroll: true });
    $('deployment-editor')?.scrollIntoView?.({ block: 'start' });
  }
}

function renderDeploymentSummary() {
  if (!$('deployment-summary')) return;
  if (state.deploymentBatchTargets) { renderBatchReview(); return; }
  let patch;
  try { patch = readDeploymentPatch(); } catch (_) { $('deployment-summary').textContent = '配置 JSON 尚未完整，请修正后核对。'; return; }
  const enabled = Object.entries(patch.plugins || {}).filter(([, value]) => value?.enabled).map(([name]) => name).join(', ') || '继承客户端配置';
  const windows = (kind) => {
    const items = patch.client?.[`${kind}_windows`];
    if (items === undefined) return '继承客户端基础配置';
    if (!items.length) return '全天允许';
    return items.map((item) => `${item.days?.length ? `星期 ${item.days.join('、')}` : '每天'} ${item.start}–${item.end}`).join('；');
  };
  const components = deploymentComponents();
  $('deployment-summary').innerHTML = `<div class="deployment-review-head"><strong>${escapeHtml(state.deploymentClient || $('deployment-client').value)}</strong><span class="chip blue">${escapeHtml(enabled)}</span></div><dl class="deployment-review-grid"><div><dt>程序与数据</dt><dd>${components.length} 个制品</dd></div><div><dt>每插件并发</dt><dd>${escapeHtml(patch.client?.task_threads ?? '继承基础配置')}</dd></div><div><dt>执行时间</dt><dd>${escapeHtml(windows('execution'))}</dd></div><div><dt>更新时间</dt><dd>${escapeHtml(windows('update'))}</dd></div></dl>${components.length ? `<ul class="deployment-review-artifacts">${components.map((component) => { const item = state.artifacts.find((artifact) => artifact.artifact_id === component.artifact_id); return `<li><span>${escapeHtml(item?.name || component.slot)}</span><b>${escapeHtml(item?.version || '未知版本')}</b></li>`; }).join('')}</ul>` : '<p class="config-help">使用客户端现有程序和数据，不分发新的制品。</p>'}`;
}

async function openDeploymentEditor(client = '') {
  if (state.deploymentBusy) return;
  state.deploymentBatchTargets = null; state.deploymentBatchReview = null; state.deploymentBatchHistory = null;
  $('deployment-batch-targets').hidden = true; $('deployment-batch-scopes').hidden = true;
  $('deployment-batch-review').hidden = true; $('deployment-single-target').hidden = false;
  setWorkspaceTab('deployments');
  state.deploymentEditorOpen = true;
  $('deployment-editor').hidden = false;
  const changeClient = Boolean(client && client !== state.deploymentClient);
  if (changeClient) await loadDeployment(client);
  if (!client && !state.deploymentDraftLoaded) {
    $('deployment-editor-mode').value = 'wizard';
    if (!$('deployment-patch').value) $('deployment-patch').value = '{}';
    importDeploymentWizard();
  }
  if (changeClient && state.deploymentDraftLoaded && state.deploymentClient === client) {
    const enabled = Object.entries(readDeploymentPatch().plugins || {}).find(([, item]) => item?.enabled)?.[0];
    if (!enabled || ['coverage', 'stpcompare'].includes(enabled)) {
      $('deployment-editor-mode').value = 'wizard';
      if ($('deployment-advanced')) $('deployment-advanced').open = false;
    }
    updateDeploymentMode();
  }
  showDeploymentStep(0, true);
}

function batchScopes() {
  return { plugins: Boolean($('batch-scope-plugins').checked), resources: Boolean($('batch-scope-resources').checked), schedule: Boolean($('batch-scope-schedule').checked) };
}

function batchWindowNotice() {
  const waiting = (state.deploymentBatchTargets || []).filter((id) => (state.agents || []).find((agent) => agent.client_id === id)?.update_window_open === false);
  return waiting.length ? `${waiting.join('、')} 的更新时间窗尚未开放。发布后会预占这些机器，等待窗口期间不再领取任务。` : '';
}

function renderBatchTargets() {
  const ids = state.deploymentBatchTargets;
  if (!ids) return;
  const notice = batchWindowNotice();
  $('deployment-batch-targets').innerHTML = `<strong>已选 ${ids.length} 台客户端</strong><div>${ids.map((id) => `<span class="chip">${escapeHtml(id)}</span>`).join('')}</div><p class="config-help">使用同一份增量配置，分别合并到每台机器。只提交本次明确修改的字段。</p>${notice ? `<p class="management-status warning">${escapeHtml(notice)}</p>` : ''}`;
}

function validateBatchTargets() {
  if (!state.deploymentBatchTargets?.length) throw new Error('请先选择客户端机器。');
  const blocked = state.deploymentBatchTargets.map((id) => {
    const agent = (state.agents || []).find((item) => item.client_id === id);
    const reason = agentUnavailableReason(agent);
    return reason ? `${id}：${reason}` : '';
  }).filter(Boolean);
  if (blocked.length) throw new Error(`所选客户端暂不可配置：${blocked.join('；')}。草稿已保留。`);
}

async function openBatchDeployment(clientIds = state.selectedAgentIds, reset = false) {
  if (state.deploymentBusy) return;
  const ids = [...new Set(clientIds)];
  navigate('clients');
  await loadAgents();
  const blocked = ids.filter((id) => !(state.agents || []).some((agent) => agent.client_id === id && !agentUnavailableReason(agent)));
  if (!ids.length || blocked.length) { $('deployment-status').textContent = blocked.length ? `客户端 ${blocked.join('、')} 当前不可配置。请刷新状态或等待其空闲。` : '请先从客户端列表选择在线空闲机器。'; return false; }
  if (!reset && JSON.stringify(state.deploymentBatchTargets) === JSON.stringify(ids)) {
    state.deploymentEditorOpen = true; $('deployment-editor').hidden = false;
    showDeploymentStep(state.deploymentStep, true); return true;
  }
  if (!reset && state.deploymentBatchTargets) {
    state.selectedAgentIds = ids; state.deploymentBatchTargets = ids;
    state.deploymentBatchReview = null; state.deploymentBinding = null; state.deploymentBatchHistory = null;
    state.deploymentPreview = ''; state.deploymentWizardDirty = true;
    $('deployment-bindings').hidden = true; $('deployment-batch-review').hidden = true;
    $('deployment-rollback').innerHTML = '<option value="">选择历史版本</option>';
    $('deployment-history-status').textContent = ''; $('deployment-rollback-button').disabled = true;
    state.deploymentEditorOpen = true; $('deployment-editor').hidden = false;
    renderClients(); showDeploymentStep(0, true);
    $('deployment-status').textContent = '目标客户端已更新，现有增量草稿已保留。请重新核对全部目标；待配置机器需明确启用插件。';
    if (ids.length === 1) await loadBatchDeploymentHistory();
    return true;
  }
  await loadArtifacts();
  state.selectedAgentIds = ids;
  state.deploymentBatchTargets = ids; state.deploymentBatchReview = null; state.deploymentBatchHistory = null;
  state.deploymentClient = ''; state.deploymentRevision = null; state.deploymentDraftLoaded = true;
  state.deploymentSelected = {}; state.deploymentBinding = null; state.deploymentPreview = '';
  $('deployment-patch').value = '{}'; $('deployment-reason').value = '';
  $('deployment-editor-mode').value = 'wizard'; $('deployment-advanced').open = false;
  $('deployment-batch-targets').hidden = false; $('deployment-single-target').hidden = true;
  $('deployment-batch-scopes').hidden = false;
  renderBatchTargets();
  $('batch-scope-plugins').checked = ids.some((id) => (state.agents || []).find((agent) => agent.client_id === id)?.state === 'unconfigured');
  $('batch-scope-resources').checked = true; $('batch-scope-schedule').checked = false;
  $('deployment-bindings').hidden = true; $('deployment-batch-review').hidden = true;
  $('deployment-rollback').innerHTML = '<option value="">选择历史版本</option>';
  $('deployment-history-status').textContent = '';
  $('deployment-rollback-button').disabled = true;
  state.deploymentEditorOpen = true; $('deployment-editor').hidden = false;
  importDeploymentWizard(); updateDeploymentMode(); showDeploymentStep(0, true);
  $('deployment-status').textContent = '先核对本次修改范围，再配置所选插件的程序与数据。';
  renderClients();
  if (ids.length === 1) await loadBatchDeploymentHistory();
  return true;
}

async function loadBatchDeploymentHistory() {
  if (state.deploymentBusy || state.deploymentBatchTargets?.length !== 1) return;
  const client = state.deploymentBatchTargets[0];
  setDeploymentBusy(true);
  try {
    const result = await jsonFetch(`/api/deployments/${encodeURIComponent(client)}`);
    state.deploymentBatchHistory = { client_id: client, revision: result.desired?.revision || null, history: result.history || [] };
    renderDeploymentHistory(result);
    $('deployment-history-status').textContent = result.history?.length ? '历史记录已读取；恢复时会核对当前部署版本和整机空闲状态。' : '此客户端还没有可恢复的历史部署。';
  } catch (error) { $('deployment-history-status').textContent = `历史读取失败：${error.message}。当前配置草稿已保留。`; }
  finally { setDeploymentBusy(false); }
}

function buildBatchDeploymentRequest() {
  if ($('deployment-editor-mode').value !== 'wizard') {
    const patch = readDeploymentPatch();
    if (!Object.keys(patch).length && !deploymentComponents().length) throw new Error('请填写本次配置变更或选择制品。');
    return { client_ids: [...state.deploymentBatchTargets], components: deploymentComponents(), config_patch: patch };
  }
  const scopes = batchScopes();
  if (!Object.values(scopes).some(Boolean)) throw new Error('请至少选择一项修改范围。');
  const model = collectDeploymentWizard();
  let patch = {}, components = [];
  if (scopes.resources && model.enabledPlugins.length) {
    const built = buildDeploymentWizard({}, [], { ...model, threads: '', windows: { execution: { mode: 'inherit' }, update: { mode: 'inherit' } } }, state.artifacts, executionPluginNames());
    patch = built.config_patch; components = built.components;
    for (const [name, config] of Object.entries(patch.plugins || {})) {
      delete config.enabled;
      if (!Object.keys(config).length) delete patch.plugins[name];
    }
    if (patch.plugins && !Object.keys(patch.plugins).length) delete patch.plugins;
    if (patch.client && !Object.keys(patch.client).length) delete patch.client;
  }
  if (scopes.plugins) {
    const names = new Set(['coverage', 'stpcompare', ...executionPluginNames(), ...model.enabledPlugins]);
    for (const name of names) deploymentSet(patch, `plugins.${name}.enabled`, model.enabledPlugins.includes(name));
  }
  if (scopes.schedule) applyDeploymentCommon(patch, model);
  if (!Object.keys(patch).length && !components.length) throw new Error('当前范围没有填写变更；继承现有运行安排不会产生新的配置。');
  return { client_ids: [...state.deploymentBatchTargets], components, config_patch: patch };
}

function batchPreviewSnapshot() {
  return JSON.stringify({ targets: state.deploymentBatchTargets, scopes: batchScopes(), mode: $('deployment-editor-mode').value, model: collectDeploymentWizard(), json: $('deployment-patch').value, components: deploymentComponents() });
}

function renderBatchReview() {
  const review = state.deploymentBatchReview;
  const notice = batchWindowNotice();
  $('deployment-summary').innerHTML = `<div class="deployment-review-head"><strong>${state.deploymentBatchTargets.length} 台客户端</strong><span class="chip blue">增量合并</span></div><p class="config-help">${$('deployment-editor-mode').value === 'wizard' ? Object.entries(batchScopes()).filter(([, value]) => value).map(([name]) => ({ plugins: '插件启用状态', resources: '插件、程序与数据', schedule: '运行安排' })[name]).join(' · ') : '高级 JSON 中明确指定的配置'}。发布前将再次核对每台机器的空闲状态及配置版本。</p>${notice ? `<p class="management-status warning">${escapeHtml(notice)}</p>` : ''}`;
  $('deployment-batch-review').hidden = !review;
  if (!review) return;
  $('deployment-batch-review').innerHTML = `<h3>逐台配置预览</h3>${review.clients.map((client) => `<details class="batch-review-client"><summary><strong>${escapeHtml(client.client_id)}</strong><span>${client.revision ? '更新现有部署' : '首次部署'}</span></summary>${client.changes ? `<p class="config-help">${escapeHtml(typeof client.changes === 'string' ? client.changes : JSON.stringify(client.changes))}</p>` : ''}<p class="config-help">以下为该机器合并后的配置，仅供核对。发布发送本次增量变更。</p><pre>${escapeHtml(JSON.stringify({ components: client.components, config_patch: client.config_patch }, null, 2))}</pre></details>`).join('')}`;
}

async function previewBatchDeployment() {
  if (state.deploymentBusy) return false;
  try {
    validateBatchTargets();
    const payload = buildBatchDeploymentRequest();
    const snapshot = batchPreviewSnapshot();
    setDeploymentBusy(true); $('deployment-preview-status').textContent = '正在核对所选机器和逐台合并后的配置…';
    const result = await jsonFetch('/api/deployments/batch/preview', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
    if (snapshot !== batchPreviewSnapshot()) throw new Error('预览期间草稿已变化，请重新核对。');
    if (!Array.isArray(result.clients) || result.clients.length !== payload.client_ids.length || payload.client_ids.some((id) => !result.clients.some((client) => client.client_id === id && Object.hasOwn(client, 'revision')))) throw new Error('预览未返回全部目标客户端，请重新读取。');
    // Per-client configurations may contain masked secrets. Keep them for
    // review only; publication must use the exact original incremental patch.
    state.deploymentBatchReview = { ...result, payload };
    $('deployment-patch').value = JSON.stringify(payload.config_patch, null, 2);
    state.deploymentSelected = Object.fromEntries(payload.components.map((item) => [item.slot, item.artifact_id]));
    state.deploymentBinding = result; state.deploymentWizardDirty = false;
    state.deploymentPreview = batchPreviewSnapshot();
    renderDeploymentBindings(result); renderBatchReview();
    $('deployment-preview-status').textContent = `已核对 ${result.clients.length} 台客户端，请展开逐台配置后发布。`;
    return true;
  } catch (error) { $('deployment-preview-status').textContent = `无法生成预览：${error.message}`; return false; }
  finally { setDeploymentBusy(false); }
}

async function publishBatchDeployment() {
  if (state.deploymentBusy) return;
  try {
    validateBatchTargets();
    if (!state.deploymentBatchReview || state.deploymentWizardDirty || state.deploymentPreview !== batchPreviewSnapshot()) throw new Error('请先核对当前增量配置和全部目标机器，再发布。');
    const review = state.deploymentBatchReview;
    setDeploymentBusy(true); $('deployment-status').textContent = '正在预占客户端并发布部署…';
    const payload = { targets: review.clients.map(({ client_id, revision }) => ({ client_id, revision })), components: review.payload.components, config_patch: review.payload.config_patch, reason: $('deployment-reason').value.trim(), operator_id: state.operatorId };
    const result = await jsonFetch('/api/deployments/batch', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
    for (const client of result.clients || []) state.deployments = [...state.deployments.filter((item) => item.client_id !== client.client_id), client];
    renderDeployments(); renderClients();
    state.deploymentBatchReview = null; state.deploymentPreview = ''; renderBatchReview();
    $('deployment-status').textContent = `已发布 ${result.clients?.length || payload.targets.length} 台客户端${result.batch_id ? ` · 批次 ${result.batch_id}` : ''}。请查看下方逐台应用状态。`;
    await Promise.all([loadAgents(), loadArtifacts()]);
  } catch (error) { state.deploymentPreview = ''; state.deploymentWizardDirty = true; $('deployment-status').textContent = `发布失败：${error.message}。草稿已保留，请重新核对后再发布。`; }
  finally { setDeploymentBusy(false); }
}

function closeDeploymentEditor() {
  if (state.deploymentBusy) return;
  state.deploymentEditorOpen = false;
  $('deployment-editor').hidden = true;
  $('deployment-new')?.focus?.();
  renderArtifacts();
}

async function advanceDeploymentStep() {
  if (state.deploymentBusy) return false;
  try {
    if (state.deploymentBatchTargets) {
      validateBatchTargets();
      if (state.deploymentStep === 0) {
        if ($('deployment-editor-mode').value !== 'wizard') {
          if (!(await previewBatchDeployment())) return false;
          showDeploymentStep(4, true); return true;
        }
        const scopes = batchScopes();
        if (!Object.values(scopes).some(Boolean)) throw new Error('请至少选择一项修改范围。');
        showDeploymentStep(scopes.plugins || scopes.resources ? 1 : 3, true); return true;
      }
      if (state.deploymentStep === 1) {
        if (!state.deploymentEnabledPlugins.length && !batchScopes().plugins) throw new Error('请至少选择一个要配置的执行插件。');
        showDeploymentStep(batchScopes().resources && state.deploymentEnabledPlugins.length ? 2 : 3, true); return true;
      }
      if (state.deploymentStep === 2) { buildBatchDeploymentRequest(); showDeploymentStep(3, true); return true; }
      if (state.deploymentStep === 3) {
        if (!(await previewBatchDeployment())) { $('deployment-status').textContent = $('deployment-preview-status').textContent; return false; }
        showDeploymentStep(4, true); return true;
      }
      return true;
    }
    if (state.deploymentStep === 0) {
      const client = $('deployment-client').value.trim();
      if (!client) throw new Error('请选择或填写目标客户端标识。');
      if (!state.deploymentDraftLoaded || state.deploymentClient !== client) {
        const preferredMode = $('deployment-editor-mode').value;
        const advancedOpen = Boolean($('deployment-advanced')?.open);
        const loaded = await loadDeployment(client);
        if (!loaded) return false;
        const enabled = Object.entries(readDeploymentPatch().plugins || {}).find(([, item]) => item?.enabled)?.[0];
        if (preferredMode === 'wizard' && (!enabled || ['coverage', 'stpcompare'].includes(enabled))) {
          $('deployment-editor-mode').value = 'wizard';
          if ($('deployment-advanced')) $('deployment-advanced').open = advancedOpen;
        }
        else $('deployment-editor-mode').value = 'json';
        updateDeploymentMode();
      }
      if ($('deployment-editor-mode').value !== 'wizard') { readDeploymentPatch(); showDeploymentStep(4, true); return true; }
    }
    if (state.deploymentStep === 1 && !state.deploymentEnabledPlugins.length) throw new Error('请至少启用一个客户端执行插件。');
    if (state.deploymentStep === 1 && !['coverage', 'stpcompare'].includes($('deployment-plugin').value)) throw new Error('请选择正在配置的执行插件。');
    if (state.deploymentStep === 2) {
      const model = collectDeploymentWizard();
      buildDeploymentWizard(readDeploymentPatch(), deploymentComponents(), { ...model, threads: '', windows: { execution: { mode: 'inherit' }, update: { mode: 'inherit' } } }, state.artifacts, executionPluginNames());
    }
    if (state.deploymentStep === 3 && !(await previewDeploymentWizard())) {
      $('deployment-status').textContent = $('deployment-preview-status').textContent;
      return false;
    }
    if (state.deploymentStep < 4) { $('deployment-status').textContent = ''; showDeploymentStep(state.deploymentStep + 1, true); }
    return true;
  } catch (error) { $('deployment-status').textContent = error.message; return false; }
}

function readDeploymentPatch() {
  const patch = JSON.parse($('deployment-patch').value || '{}');
  if (!deploymentObject(patch)) throw new Error('配置覆盖必须是 JSON 对象');
  return patch;
}
function wizardArtifactOptions(kind, selected) {
  return '<option value="">使用客户端现有路径 / 继承配置</option>' + state.artifacts.filter((item) => item.kind === kind).map((item) => `<option value="${escapeHtml(item.artifact_id)}" ${item.artifact_id === selected ? 'selected' : ''}>${escapeHtml(item.name)} @ ${escapeHtml(item.version)}</option>`).join('');
}
function renderDeploymentWizard() {
  const model = state.deploymentWizard;
  if (!model) return;
  $('deployment-plugin').value = model.plugin;
  $('deployment-resource-plugin').value = model.plugin;
  const names = [...new Set(['coverage', 'stpcompare', ...executionPluginNames(), ...Object.keys(readDeploymentPatch().plugins || {})])];
  $('deployment-enabled-plugins').innerHTML = names.map((name) => `<label class="check-field"><input type="checkbox" data-deployment-enable="${escapeHtml(name)}" ${state.deploymentEnabledPlugins.includes(name) ? 'checked' : ''}>${escapeHtml(name)}</label>`).join('');
  $('deployment-fields').innerHTML = wizardGroups(model.plugin).map((group) => `<section class="deployment-section" data-deployment-resource data-deployment-stage="${group.id === 'plugin' ? 1 : 2}"><h3>${group.title}</h3><div class="management-form"><div class="management-field"><label for="deployment-artifact-${group.id}">已上传制品</label><select id="deployment-artifact-${group.id}">${wizardArtifactOptions(group.kind, model.groups[group.id].artifactId)}</select></div>${group.fields.map(([path, label, type]) => `<div class="management-field grow-field"><label for="${deploymentFieldId(group.id, path)}">${escapeHtml(label)}</label>${['arguments', 'paths'].includes(type) ? `<textarea id="${deploymentFieldId(group.id, path)}" spellcheck="false">${escapeHtml(model.groups[group.id].values[path])}</textarea>` : `<input id="${deploymentFieldId(group.id, path)}" value="${escapeHtml(model.groups[group.id].values[path])}" autocomplete="off">`}</div>`).join('')}</div></section>`).join('');
  // Assign properties as well as markup so the form and tests share one source.
  for (const group of wizardGroups(model.plugin)) {
    $(`deployment-artifact-${group.id}`).value = model.groups[group.id].artifactId;
    for (const [path] of group.fields) $(deploymentFieldId(group.id, path)).value = model.groups[group.id].values[path];
  }
  $('deployment-threads').value = model.threads;
  $('deployment-dry-run').value = model.dryRun === null ? 'inherit' : model.dryRun ? 'simulate' : 'real';
  $('deployment-store-cov').value = model.storeCov === null ? 'inherit' : model.storeCov ? 'keep' : 'discard';
  $('deployment-store-cov-field').hidden = model.plugin !== 'coverage';
  for (const kind of ['execution', 'update']) renderDeploymentWindows(kind);
  $('deployment-preview-status').textContent = model.plugin === 'coverage' ? '预览时核对所选程序制品的内容身份；数据注册版本仍需与数据目录的 version.txt 一致。业务程序参数由插件传入 STP 文件路径。' : '两个程序分别使用独立输出目录，并各生成 info.txt 和 info_new.txt；预览时核对程序内容与注册版本。';
  showDeploymentStep(state.deploymentStep);
}
function renderDeploymentWindows(kind) {
  const window = state.deploymentWizard.windows[kind];
  $(`deployment-${kind}-windows`).innerHTML = `<label class="config-help" for="deployment-${kind}-mode">时间安排</label><select id="deployment-${kind}-mode" data-window-mode="${kind}"><option value="inherit">继承客户端基础配置</option><option value="all">全天允许</option><option value="custom">指定时段</option></select>` + window.items.map((item, index) => `<div class="deployment-window-row" ${window.mode === 'custom' ? '' : 'hidden'}><label class="deployment-days">星期（1–7，逗号分隔；空为每天）<input data-window-kind="${kind}" data-window-index="${index}" data-window-field="days" value="${escapeHtml((item.days || []).join(','))}"></label><label>开始<input type="time" data-window-kind="${kind}" data-window-index="${index}" data-window-field="start" value="${escapeHtml(item.start)}"></label><label>结束<input type="time" data-window-kind="${kind}" data-window-index="${index}" data-window-field="end" value="${escapeHtml(item.end)}"></label><button type="button" class="button" data-window-remove="${kind}" data-window-index="${index}">移除此时段</button></div>`).join('');
  $(`deployment-${kind}-mode`).value = window.mode;
}
function collectDeploymentWizard() {
  if (!state.deploymentWizard) throw new Error('请先读取部署配置并回填表单');
  const model = JSON.parse(JSON.stringify(state.deploymentWizard));
  for (const group of wizardGroups(model.plugin)) {
    model.groups[group.id].artifactId = $(`deployment-artifact-${group.id}`).value;
    for (const [path] of group.fields) model.groups[group.id].values[path] = $(deploymentFieldId(group.id, path)).value;
  }
  model.threads = $('deployment-threads').value;
  model.dryRun = $('deployment-dry-run').value === 'inherit' ? null : $('deployment-dry-run').value === 'simulate';
  model.storeCov = $('deployment-store-cov').value === 'inherit' ? null : $('deployment-store-cov').value === 'keep';
  model.enabledPlugins = [...state.deploymentEnabledPlugins];
  model.pluginModels = Object.fromEntries(Object.entries(state.deploymentWizardDrafts).map(([name, draft]) => {
    const copy = { ...draft }; delete copy.enabledPlugins; delete copy.pluginModels; return [name, copy];
  }));
  return model;
}
function importDeploymentWizard(plugin) {
  state.deploymentWizard = null;
  state.deploymentWizardDrafts = {};
  try {
    const patch = readDeploymentPatch();
    state.deploymentWizard = readDeploymentWizard(patch, deploymentComponents(), plugin, Boolean(state.deploymentBatchTargets));
    state.deploymentEnabledPlugins = Object.entries(patch.plugins || {}).filter(([, item]) => item?.enabled === true).map(([name]) => name);
    if (!state.deploymentEnabledPlugins.length) state.deploymentEnabledPlugins = [state.deploymentWizard.plugin];
    state.deploymentWizardSource = JSON.stringify({ patch: $('deployment-patch').value, components: deploymentComponents() });
    state.deploymentWizardDirty = false; state.deploymentPreview = '';
    renderDeploymentWizard();
  } catch (error) { $('deployment-editor-mode').value = 'json'; updateDeploymentMode(); $('deployment-preview-status').textContent = `回填失败：${error.message}。可继续使用高级 JSON 编辑。`; }
}
function updateDeploymentMode() {
  const wizard = $('deployment-editor-mode').value === 'wizard';
  $('deployment-wizard').hidden = !wizard;
  $('deployment-plugin').disabled = !wizard;
  $('deployment-preview').hidden = !wizard;
  $('deployment-import').hidden = !wizard;
  $('deployment-patch').readOnly = wizard;
  showDeploymentStep(wizard ? state.deploymentStep : state.deploymentStep > 0 ? 4 : 0);
  if (!wizard && $('deployment-advanced')) $('deployment-advanced').open = true;
  if (state.deploymentBatchTargets) {
    $('deployment-batch-scopes').hidden = !wizard;
    $('deployment-preview').hidden = state.deploymentStep !== 4;
  }
  renderArtifacts();
}
function deploymentPreviewSnapshot(model = collectDeploymentWizard()) {
  return JSON.stringify({ model, patch: $('deployment-patch').value, components: deploymentComponents() });
}
async function previewDeploymentWizard() {
  if (state.deploymentBatchTargets) return previewBatchDeployment();
  if (state.deploymentBusy) return false;
  try {
    if (!state.deploymentDraftLoaded || $('deployment-client').value.trim() !== state.deploymentClient) throw new Error('请先读取目标客户端的当前部署');
    const model = collectDeploymentWizard();
    const result = buildDeploymentWizard(readDeploymentPatch(), deploymentComponents(), model, state.artifacts, executionPluginNames());
    const snapshot = deploymentPreviewSnapshot(model); const client = state.deploymentClient;
    setDeploymentBusy(true);
    $('deployment-preview-status').textContent = '正在核对程序内容和服务端匹配配置…';
    const verified = await jsonFetch('/api/deployments/preview', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(result) });
    if (snapshot !== deploymentPreviewSnapshot() || $('deployment-client').value.trim() !== client) {
      $('deployment-preview-status').textContent = '预览期间草稿已修改，请重新生成预览。'; return false;
    }
    $('deployment-patch').value = JSON.stringify(verified.config_patch, null, 2);
    state.deploymentSelected = Object.fromEntries(verified.components.map((item) => [item.slot, item.artifact_id]));
    importDeploymentWizard(model.plugin);
    state.deploymentPreview = deploymentPreviewSnapshot();
    state.deploymentBinding = verified;
    renderDeploymentBindings(verified);
    renderArtifacts();
    $('deployment-preview-status').textContent = `预览已生成：${state.deploymentClient} · ${state.deploymentEnabledPlugins.join('、')} · ${result.components.length} 个制品。请核对部署摘要后发布。`;
    return true;
  } catch (error) { $('deployment-preview-status').textContent = `无法生成预览：${error.message}`; return false; }
  finally { setDeploymentBusy(false); }
}
function renderDeploymentBindings(result) {
  $('deployment-bindings').hidden = false;
  const bindings = result.program_bindings || [];
  $('deployment-binding-list').innerHTML = bindings.map((item) => `<details class="binding-item"><summary><strong>${escapeHtml(item.plugin)} · ${escapeHtml(item.role === 'baseline' ? '老程序' : item.role === 'candidate' ? '新程序' : item.role === 'program' ? '业务程序' : item.role)}</strong><span class="chip green">内容已核对</span></summary><p class="config-help">程序路径：${escapeHtml(item.executable)}<br>注册版本：<code>${escapeHtml(item.version)}</code></p></details>`).join('') || '<p class="config-help">当前路径没有可绑定的程序制品，需自行核对服务端预期版本与客户端注册版本。</p>';
  $('deployment-binding-status').textContent = (result.warnings || []).join('\n');
  $('deployment-bind-scheduler').disabled = !bindings.length || Boolean(state.deploymentBatchTargets && !Object.keys(result.scheduler_plugins || {}).length);
}

async function bindDeploymentScheduler() {
  if (state.deploymentBusy) return;
  const previewValid = state.deploymentBatchTargets ? state.deploymentBatchReview && state.deploymentPreview === batchPreviewSnapshot() : state.deploymentPreview === deploymentPreviewSnapshot() && $('deployment-client').value.trim() === state.deploymentClient;
  if (!state.deploymentBinding || state.deploymentWizardDirty || !previewValid) {
    $('deployment-binding-status').textContent = '草稿已变化，请重新生成预览后保存匹配配置。'; return;
  }
  setDeploymentBusy(true); $('deployment-bind-scheduler').disabled = true;
  try {
    if (state.deploymentBatchTargets && !Object.keys(state.deploymentBatchReview.scheduler_plugins || {}).length) throw new Error('所选机器没有一致的程序身份，请按相同程序版本分批配置。');
    // The backend merges machine defaults and resolves identities again. The
    // displayed per-machine previews may contain masked secrets; never send them.
    const plan = state.deploymentBatchTargets ? state.deploymentBatchReview.payload : { components: deploymentComponents(), config_patch: readDeploymentPatch() };
    const result = await jsonFetch('/api/deployments/bind-scheduler', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ...plan, scheduler_revision: state.deploymentBinding.scheduler_revision, operator_id: state.operatorId }) });
    state.deploymentBinding.scheduler_revision = result.scheduler_revision || result.revision;
    if (state.deploymentBatchTargets) {
      state.deploymentPreview = ''; state.deploymentBatchReview = null; state.deploymentWizardDirty = true; renderBatchReview();
      $('deployment-preview-status').textContent = '服务端匹配配置已更新，请重新生成逐台配置预览后发布客户端部署。';
    }
    $('deployment-binding-status').textContent = '服务端匹配配置已保存。需重启调度器后生效；请重新核对客户端部署后发布。';
  } catch (error) { $('deployment-binding-status').textContent = `保存失败：${error.message}。当前草稿已保留。`; }
  finally { setDeploymentBusy(false); }
}
function deploymentWizardChanged(event) {
  state.deploymentWizardDirty = true;
  const target = event.target;
  if (target.dataset.windowMode) {
    const window = state.deploymentWizard.windows[target.dataset.windowMode];
    window.mode = target.value;
    renderDeploymentWindows(target.dataset.windowMode);
  }
  if (target.dataset.windowKind) {
    const item = state.deploymentWizard.windows[target.dataset.windowKind].items[Number(target.dataset.windowIndex)];
    item[target.dataset.windowField] = target.dataset.windowField === 'days' ? target.value.split(/[,，\s]+/).filter(Boolean).map(Number) : target.value;
  }
  $('deployment-preview-status').textContent = '表单已修改，请重新生成配置预览后发布。';
}

function renderArtifacts() {
  const kinds = { plugin: '插件', program: '程序', dataset: '数据集' };
  const query = state.artifactSearch.trim().toLowerCase();
  const filtered = state.artifacts.filter((artifact) => (!state.artifactFilter || state.artifactFilter === 'all' || artifact.kind === state.artifactFilter) && (!query || [artifact.name, artifact.version, artifact.filename, artifact.sha256].some((value) => String(value || '').toLowerCase().includes(query))));
  const advanced = state.deploymentEditorOpen && $('deployment-editor-mode').value === 'json';
  const rows = filtered.map((artifact) => `<tr><td><div class="artifact-name-cell">${advanced ? `<input type="checkbox" data-artifact-id="${escapeHtml(artifact.artifact_id)}" ${Object.values(state.deploymentSelected).includes(artifact.artifact_id) ? 'checked' : ''} ${state.deploymentBusy ? 'disabled' : ''} aria-label="选择 ${escapeHtml(artifact.name)} ${escapeHtml(artifact.version)}">` : ''}<button type="button" class="workspace-table-link" data-artifact-detail="${escapeHtml(artifact.artifact_id)}">${escapeHtml(artifact.name)}</button></div><small class="workspace-table-sub">${escapeHtml(artifact.filename || '')}</small></td><td><span class="chip">${escapeHtml(kinds[artifact.kind] || artifact.kind)}</span></td><td>${escapeHtml(artifact.version)}</td><td>${formatBytes(artifact.size_bytes)}</td><td>${escapeHtml(artifact.created_at ? timeLabel(artifact.created_at) : '--')}</td><td><button type="button" class="button compact" data-artifact-detail="${escapeHtml(artifact.artifact_id)}">详情</button></td></tr>`);
  $('artifact-list').innerHTML = rows.length ? rows.join('') : `<tr><td colspan="6" class="empty-row">${state.artifacts.length ? '没有匹配的制品，请调整搜索或类型筛选。' : '还没有制品。上传插件包、业务程序或数据集后即可部署。'}</td></tr>`;
  if ($('artifact-count')) $('artifact-count').textContent = `${filtered.length} / ${state.artifacts.length} 个制品`;
  const pluginSelection = $('server-plugin-artifact').value;
  $('server-plugin-artifact').innerHTML = '<option value="">选择插件包</option>' + state.artifacts.filter((item) => item.kind === 'plugin').map((item) => `<option value="${escapeHtml(item.artifact_id)}">${escapeHtml(item.name)} @ ${escapeHtml(item.version)}</option>`).join('');
  $('server-plugin-artifact').value = pluginSelection;
}

function openArtifactDetails(artifactId, trigger) {
  const artifact = state.artifacts.find((item) => item.artifact_id === artifactId);
  if (!artifact) return;
  const kinds = { plugin: '插件', program: '业务程序', dataset: '数据集' };
  openWorkspaceDrawer(artifact.name, `<p class="drawer-intro">${escapeHtml(kinds[artifact.kind] || artifact.kind)} · ${escapeHtml(artifact.version)}</p><dl class="detail-list"><div><dt>文件名</dt><dd>${escapeHtml(artifact.filename || '--')}</dd></div><div><dt>文件大小</dt><dd>${formatBytes(artifact.size_bytes)}</dd></div><div><dt>上传时间</dt><dd>${escapeHtml(artifact.created_at ? timeLabel(artifact.created_at) : '--')}</dd></div><div><dt>内容 SHA-256</dt><dd><code>${escapeHtml(artifact.sha256 || '--')}</code><button type="button" class="button compact" data-copy-value="${escapeHtml(artifact.sha256 || '')}">复制摘要</button></dd></div><div><dt>制品标识</dt><dd><code>${escapeHtml(artifact.artifact_id)}</code></dd></div></dl><p class="config-help">制品保持原内容不变。更新时上传新版本，再从客户端部署向导选择。</p><div class="drawer-actions"><button type="button" class="button primary" data-workspace-go="deployments">前往客户端部署</button>${artifact.kind === 'plugin' ? `<button type="button" class="button" data-install-artifact="${escapeHtml(artifact.artifact_id)}">预览服务端安装</button>` : ''}</div><p id="workspace-copy-status" class="management-status" role="status"></p>`, trigger);
}

async function copyWorkspaceValue(value) {
  try {
    if (globalThis.navigator?.clipboard?.writeText) await globalThis.navigator.clipboard.writeText(value);
    else {
      const field = document.createElement('textarea');
      field.value = value;
      field.setAttribute('aria-label', '复制内容');
      $('workspace-drawer-content').appendChild(field);
      field.select?.();
      const copied = document.execCommand?.('copy');
      field.remove?.();
      if (!copied) throw new Error('此浏览器不支持自动复制，请选中上面的完整摘要复制。');
    }
    $('workspace-copy-status').textContent = '摘要已复制。';
  } catch (error) { $('workspace-copy-status').textContent = error.message; }
}

async function previewServerPlugin() {
  const artifactId = $('server-plugin-artifact').value;
  if (!artifactId || state.serverPluginBusy) return;
  state.serverPluginBusy = true; state.serverPluginPreview = null; $('server-plugin-install').disabled = true;
  $('server-plugin-status').textContent = '正在验证服务端插件包…';
  try {
    const result = await jsonFetch('/api/server-plugins/preview', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ artifact_id: artifactId }) });
    if ($('server-plugin-artifact').value !== artifactId) { $('server-plugin-status').textContent = '所选插件包已变更，请重新预览。'; return; }
    state.serverPluginPreview = { artifactId, ...result };
    if ($('server-plugin-summary')) $('server-plugin-summary').innerHTML = `<div class="server-install-summary"><strong>${escapeHtml(result.plugin)} <span>${escapeHtml(result.version)}</span></strong><p>${result.action === 'upgrade' ? '更新已有插件，保留业务配置与用户分析脚本。' : '安装服务端插件与配套分析资源。新插件安装后默认不启用。'}</p><span class="chip amber">安装后需重启调度器和结果服务</span>${result.warnings?.length ? `<p>${escapeHtml(result.warnings.join(' '))}</p>` : ''}</div>`;
    $('server-plugin-plan').textContent = JSON.stringify({ plugin: result.plugin, version: result.version, action: result.action, enabled_after_install: result.enabled_after_install, preserves_existing_configuration: result.preserves_existing_configuration, scheduler_config_path: result.scheduler_config_path, result_service_config_path: result.result_service_config_path, files: result.files, warnings: result.warnings }, null, 2);
    $('server-plugin-status').textContent = '预览完成。核对以上文件与配置改动后可安装；安装后需要重启服务。';
    $('server-plugin-install').disabled = false;
  } catch (error) { $('server-plugin-status').textContent = `预览失败：${error.message}`; }
  finally { state.serverPluginBusy = false; }
}
async function installServerPlugin() {
  const preview = state.serverPluginPreview;
  if (state.serverPluginBusy || !preview || $('server-plugin-artifact').value !== preview.artifactId) return;
  state.serverPluginBusy = true; $('server-plugin-install').disabled = true;
  try {
    await jsonFetch('/api/server-plugins/install', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ artifact_id: preview.artifactId, scheduler_revision: preview.scheduler_revision, result_service_revision: preview.result_service_revision, operator_id: state.operatorId }) });
    state.serverPluginPreview = null;
    $('server-plugin-status').textContent = '服务端插件已安装。新插件需在调度器配置中启用；请重启调度器和结果服务后核对加载状态。刷新网页不会代替进程重启。';
  } catch (error) { $('server-plugin-status').textContent = `安装失败：${error.message}。安装预览已保留，请重新预览核对后再试。`; }
  finally { state.serverPluginBusy = false; }
}

function renderDeployments() {
  const labels = { current: '已生效', pending: '等待客户端', applying: '应用中', failed: '更新失败', unconfigured: '未配置', staged: '已下载', waiting_window: '等待更新时间窗', waiting_idle: '等待当前任务完成' };
  const rows = state.deployments.map((deployment) => {
    const summary = (value) => (value?.components || []).map((item) => `${item.artifact?.name || item.slot} @ ${item.artifact?.version || '--'}`).join('、') || (value ? '客户端配置' : '尚未回报');
    const applied = deployment.reported?.state === 'ready' ? summary(deployment.reported) : '尚未确认生效';
    return `<tr><td><button type="button" class="workspace-table-link" data-deployment-detail="${escapeHtml(deployment.client_id)}">${escapeHtml(deployment.client_id)}</button><small class="workspace-table-sub">${escapeHtml(deployment.reported?.message || '')}</small></td><td><span class="chip ${deployment.state === 'current' ? 'green' : deployment.state === 'failed' ? 'red' : 'amber'}">${escapeHtml(labels[deployment.state] || deployment.state)}</span></td><td class="deployment-version-cell">${escapeHtml(applied)}</td><td class="deployment-version-cell">${escapeHtml(deployment.desired ? summary(deployment.desired) : '未配置')}</td><td><div class="workspace-row-actions"><button type="button" class="button compact" data-deployment-detail="${escapeHtml(deployment.client_id)}">详情</button><button type="button" class="button compact" data-edit-deployment="${escapeHtml(deployment.client_id)}" ${state.deploymentBusy ? 'disabled' : ''}>编辑</button></div></td></tr>`;
  });
  $('deployment-list').innerHTML = rows.length ? rows.join('') : '<tr><td colspan="5" class="empty-row">还没有客户端部署。请从上方客户端列表选择在线空闲机器开始配置。</td></tr>';
  const clientIds = new Set(state.deployments.map((item) => item.client_id));
  for (const worker of state.scheduler?.clients || []) {
    clientIds.add(worker.physical_client_id || worker.worker_id);
  }
  $('deployment-client-options').innerHTML = [...clientIds].sort().map((id) => `<option value="${escapeHtml(id)}"></option>`).join('');
}

function openDeploymentDetails(client, trigger) {
  const deployment = state.deployments.find((item) => item.client_id === client);
  if (!deployment) return;
  const labels = { current: '已生效', pending: '等待客户端', applying: '应用中', failed: '更新失败', unconfigured: '未配置', staged: '已下载', waiting_window: '等待更新时间窗', waiting_idle: '等待当前任务完成' };
  const components = deployment.desired?.components || [];
  const reported = deployment.reported;
  const reportSummary = reported?.message || (reported ? ({
    ready: '客户端已确认回报版本就绪。', failed: '客户端回报更新失败，未附具体说明。',
    waiting_window: '客户端正在等待更新时间窗。', waiting_idle: '客户端正在等待当前任务完成。',
    applying: '客户端正在应用回报版本。', staged: '客户端已下载并准备好回报版本。',
  })[reported.state] || '已收到客户端回报，未附具体说明。' : '尚未收到客户端回报');
  const guidance = ({
    current: '目标部署已生效，客户端已确认此版本。',
    failed: '此版本更新失败，不会自动重试。请修正配置后重新发布，或选择历史版本恢复。',
    unconfigured: '尚未发布部署，请先选择插件、程序和数据。',
    waiting_window: '客户端已预占，将在更新时间窗开放后应用；等待期间不再领取任务。',
    waiting_idle: '客户端将在当前任务完成且更新时间窗开放后应用。',
    applying: '客户端正在切换配置和启动插件，请等待应用结果。',
    staged: '所需资源已准备，等待客户端应用并确认。',
  })[deployment.state] || '目标部署尚未确认生效，客户端将在更新时间窗开放且整机空闲时应用。';
  openWorkspaceDrawer(`${client} · 部署详情`, `<p class="drawer-intro"><span class="chip ${deployment.state === 'current' ? 'green' : deployment.state === 'failed' ? 'red' : 'amber'}">${escapeHtml(labels[deployment.state] || deployment.state)}</span></p><dl class="detail-list"><div><dt>客户端回报</dt><dd>${escapeHtml(reportSummary)}</dd></div><div><dt>目标版本</dt><dd><code>${escapeHtml(deployment.desired?.revision || '--')}</code></dd></div><div><dt>回报版本</dt><dd><code>${escapeHtml(deployment.reported?.desired_revision || '--')}</code></dd></div><div><dt>变更说明</dt><dd>${escapeHtml(deployment.desired?.reason || '未填写')}</dd></div></dl><h3>目标制品</h3>${components.length ? `<ul class="deployment-review-artifacts">${components.map((item) => `<li><span>${escapeHtml(item.artifact?.name || item.slot)}</span><b>${escapeHtml(item.artifact?.version || '--')}</b></li>`).join('')}</ul>` : '<p class="config-help">此部署没有分发制品。</p>'}<p class="config-help">${escapeHtml(guidance)} 单台配置时可展开“恢复历史部署”选择已有版本。</p><div class="drawer-actions"><button type="button" class="button primary" data-edit-deployment="${escapeHtml(client)}" ${state.deploymentBusy ? 'disabled' : ''}>编辑或恢复历史部署</button></div>`, trigger);
}

async function loadArtifacts() {
  try {
    const [artifacts, deployments] = await Promise.all([jsonFetch('/api/artifacts'), jsonFetch('/api/deployments')]);
    state.artifacts = artifacts.artifacts || [];
    state.deployments = deployments.clients || [];
    renderArtifacts(); renderDeployments();
    if (state.deploymentWizard) for (const group of wizardGroups(state.deploymentWizard.plugin)) {
      const input = $(`deployment-artifact-${group.id}`); const selected = input.value;
      input.innerHTML = wizardArtifactOptions(group.kind, selected); input.value = selected;
    }
  } catch (error) {
    $('artifact-status').textContent = `制品读取失败：${error.message}`;
  }
}

async function uploadArtifact(event) {
  event.preventDefault();
  if (state.artifactBusy) return;
  const file = $('artifact-file').files?.[0];
  if (!file) return;
  const params = new URLSearchParams({ kind: $('artifact-kind').value, name: $('artifact-name').value.trim(), version: $('artifact-version').value.trim(), filename: file.name });
  state.artifactBusy = true; $('artifact-upload').disabled = true; $('artifact-status').textContent = '正在上传并校验 SHA-256…';
  try {
    // The service hashes the stream while receiving it. This also works over
    // plain LAN HTTP, where browser SubtleCrypto is unavailable.
    const artifact = await jsonFetch(`/api/artifacts/upload?${params}`, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: file });
    $('artifact-status').textContent = `制品上传成功，SHA-256：${artifact.sha256}`;
    $('artifact-file').value = ''; await loadArtifacts();
  } catch (error) { $('artifact-status').textContent = `上传失败：${error.message}`; }
  finally { state.artifactBusy = false; $('artifact-upload').disabled = false; }
}

async function loadDeployment(client = $('deployment-client').value.trim()) {
  if (!client || state.deploymentBusy) return false;
  setDeploymentBusy(true);
  try {
    const result = await jsonFetch(`/api/deployments/${encodeURIComponent(client)}`);
    state.deploymentClient = client; state.deploymentRevision = result.desired?.revision || null;
    state.deploymentDraftLoaded = true;
    state.deploymentSelected = Object.fromEntries((result.desired?.components || []).map((item) => [item.slot, item.artifact_id]));
    $('deployment-client').value = client;
    $('deployment-patch').value = JSON.stringify(result.desired?.config_patch || {}, null, 2);
    $('deployment-editor-mode').value = result.desired ? 'json' : 'wizard';
    state.deploymentBinding = null; $('deployment-bindings').hidden = true;
    importDeploymentWizard(); updateDeploymentMode();
    renderDeploymentHistory(result);
    $('deployment-status').textContent = result.desired ? '已读取当前部署；修改并发布后生效。' : '此客户端尚未配置部署。';
    renderArtifacts();
    return true;
  } catch (error) { $('deployment-status').textContent = `读取失败：${error.message}`; return false; }
  finally { setDeploymentBusy(false); }
}

function renderDeploymentHistory(result) {
  $('deployment-rollback').innerHTML = '<option value="">选择历史版本</option>' + (result.history || []).map((item) => `<option value="${escapeHtml(item.revision)}">${escapeHtml(timeLabel(item.updated_at))} · ${escapeHtml(item.reason || item.revision.slice(0, 8))}</option>`).join('');
}

function selectArtifact(event) {
  if (state.deploymentBusy) return;
  const input = event.target.closest('[data-artifact-id]');
  if (!input) return;
  const artifact = state.artifacts.find((item) => item.artifact_id === input.dataset.artifactId);
  if (!artifact) return;
  if (input.checked) {
    const slot = `${artifact.kind}.${artifact.name}`;
    state.deploymentSelected[slot] = artifact.artifact_id;
  } else for (const [slot, id] of Object.entries(state.deploymentSelected)) { if (id === artifact.artifact_id) delete state.deploymentSelected[slot]; }
  state.deploymentWizardDirty = true;
  renderArtifacts();
}

async function saveDeployment(event) {
  event.preventDefault();
  if (state.deploymentBusy) return;
  if (state.deploymentEditorOpen && state.deploymentStep !== 4) return advanceDeploymentStep();
  if (state.deploymentBatchTargets) return publishBatchDeployment();
  const client = $('deployment-client').value.trim();
  const components = deploymentComponents();
  if (!client || !state.deploymentDraftLoaded || state.deploymentClient !== client) { $('deployment-status').textContent = '请先读取目标客户端的当前部署。'; return; }
  if ($('deployment-editor-mode').value === 'wizard' && (state.deploymentWizardDirty || state.deploymentPreview !== JSON.stringify({ model: collectDeploymentWizard(), patch: $('deployment-patch').value, components }))) {
    $('deployment-status').textContent = '请先生成配置预览，确认后再发布。'; return;
  }
  let patch;
  try {
    patch = JSON.parse($('deployment-patch').value || '{}');
    if (!patch || typeof patch !== 'object' || Array.isArray(patch)) throw new Error('配置覆盖必须是 JSON 对象');
  } catch (error) { $('deployment-status').textContent = `配置格式错误：${error.message}`; return; }
  setDeploymentBusy(true); $('deployment-status').textContent = '正在发布部署期望…';
  try {
    const result = await jsonFetch(`/api/deployments/${encodeURIComponent(client)}/desired`, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ revision: state.deploymentRevision, components, config_patch: patch, reason: $('deployment-reason').value.trim(), operator_id: state.operatorId }) });
    state.deploymentRevision = result.desired.revision;
    $('deployment-patch').value = JSON.stringify(result.desired.config_patch || patch, null, 2);
    if ($('deployment-editor-mode').value === 'wizard') {
      importDeploymentWizard();
      state.deploymentPreview = JSON.stringify({ model: collectDeploymentWizard(), patch: $('deployment-patch').value, components: deploymentComponents() });
    }
    renderDeploymentHistory(result);
    $('deployment-status').textContent = '部署期望已发布，客户端将在更新时间窗内应用。'; await loadArtifacts();
  } catch (error) { $('deployment-status').textContent = `发布失败：${error.message}`; }
  finally { setDeploymentBusy(false); }
}

async function rollbackDeployment() {
  const target = $('deployment-rollback').value;
  if (!target || !state.deploymentDraftLoaded || state.deploymentBusy) return;
  if (state.deploymentBatchTargets) {
    try {
      validateBatchTargets();
      const history = state.deploymentBatchHistory;
      if (state.deploymentBatchTargets.length !== 1 || history?.client_id !== state.deploymentBatchTargets[0] || !history.history.some((item) => item.revision === target)) throw new Error('请先读取此客户端的历史部署。');
      setDeploymentBusy(true);
      await jsonFetch(`/api/deployments/${encodeURIComponent(history.client_id)}/desired`, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ revision: history.revision, action: 'rollback', target_revision: target, reason: $('deployment-reason').value.trim(), operator_id: state.operatorId }) });
      state.deploymentPreview = ''; state.deploymentBatchReview = null; state.deploymentBatchHistory = null;
      await Promise.all([loadArtifacts(), loadAgents()]);
      setDeploymentBusy(false); await loadBatchDeploymentHistory();
      $('deployment-status').textContent = '回滚期望已发布，客户端将在更新时间窗内应用。未发布的编辑草稿已保留，使用前请重新核对。';
    } catch (error) { $('deployment-status').textContent = `回滚失败：${error.message}。编辑草稿已保留；如版本冲突，请刷新历史后重试。`; }
    finally { setDeploymentBusy(false); }
    return;
  }
  if ($('deployment-client').value.trim() !== state.deploymentClient) { $('deployment-status').textContent = '请先读取目标客户端的当前部署。'; return; }
  setDeploymentBusy(true);
  try {
    await jsonFetch(`/api/deployments/${encodeURIComponent(state.deploymentClient)}/desired`, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ revision: state.deploymentRevision, action: 'rollback', target_revision: target, reason: $('deployment-reason').value.trim(), operator_id: state.operatorId }) });
    await loadArtifacts();
    setDeploymentBusy(false);
    await loadDeployment(state.deploymentClient);
    $('deployment-status').textContent = '回滚期望已发布，客户端将在更新时间窗内应用。';
  } catch (error) { $('deployment-status').textContent = `回滚失败：${error.message}`; }
  finally { setDeploymentBusy(false); }
}

async function loadAll() {
  const requestId = ++state.loadRequest;
  // Execution controls do not depend on database readiness or report loading.
  const schedulerRefresh = loadScheduler();
  state.apiHealthy = false;
  const preferredPlugin = $('result-type').value;
  const preferredRun = $('result-run').value;
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
    if (state.page === 'plugins') await loadArtifacts();
    if (state.page === 'clients') await Promise.all([loadAgents(), loadArtifacts()]);
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

function updateReportControls() {
  const plugin = $('result-type').value;
  const canRun = Boolean(state.selected?.run_id && state.readiness?.configured && state.reportScripts[plugin] !== undefined);
  $('run-report').disabled = !canRun || state.reportBusy;
  $('run-report').textContent = state.reportBusy ? '正在生成…' : (state.report ? '重新运行报告' : '运行报告');
  $('edit-report-script').disabled = !plugin;
  const editorBusy = state.reportEditorSaving || state.reportEditorLoading;
  $('save-report-editor').disabled = editorBusy || !state.reportEditorRevision;
  $('save-run-report-editor').disabled = !canRun || state.reportBusy || state.reportEditorPlugin !== plugin || editorBusy || !state.reportEditorRevision;
  $('report-editor-text').disabled = editorBusy;
  $('report-load-latest').hidden = !state.reportEditorConflicted;
  $('report-load-latest').disabled = editorBusy;
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
  if (state.reportScriptLoads[plugin] === requestId) {
    state.reportScripts[plugin] = result.source;
    state.reportScriptRevisions[plugin] = result.revision;
  }
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

async function saveReportScript(plugin, source, revision) {
  const saved = await jsonFetch(`/api/plugins/${encodeURIComponent(plugin)}/report-script`, {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ source, revision, operator_id: state.operatorId }),
  });
  state.reportLoads[plugin] = (state.reportLoads[plugin] || 0) + 1;
  state.reportScriptLoads[plugin] = (state.reportScriptLoads[plugin] || 0) + 1;
  state.reportScripts[plugin] = saved.source;
  state.reportScriptRevisions[plugin] = saved.revision;
  if (state.reportDrafts[plugin] === source) {
    delete state.reportDrafts[plugin];
    delete state.reportDraftBases[plugin];
  }
  updateReportControls();
  return saved;
}

async function openReportEditor() {
  const plugin = $('result-type').value;
  if (!plugin || state.reportEditorSaving || state.reportEditorLoading) return;
  if ($('report-editor-dialog').open) rememberReportDraft();
  const requestId = ++state.reportEditorRequest;
  state.reportEditorPlugin = plugin;
  state.reportEditorOriginal = state.reportDraftBases[plugin]?.source ?? state.reportScripts[plugin] ?? '';
  state.reportEditorRevision = state.reportDraftBases[plugin]?.revision ?? state.reportScriptRevisions[plugin] ?? '';
  state.reportEditorConflicted = false;
  $('report-editor-title').textContent = '编辑 Python 报告脚本';
  $('report-editor-plugin').textContent = plugin;
  $('report-editor-text').value = state.reportDrafts[plugin] ?? state.reportScripts[plugin] ?? '';
  $('report-conflict-section').hidden = state.reportConflictDrafts[plugin] === undefined;
  $('report-conflict-draft').value = state.reportConflictDrafts[plugin] ?? '';
  $('report-editor-status').textContent = '保存后可用于当前插件的其他运行。';
  const dialog = $('report-editor-dialog');
  if (typeof dialog.showModal === 'function') dialog.showModal();
  else dialog.open = true;
  updateReportControls();
  $('report-editor-status').textContent = '正在检查最新脚本…';
  try {
    await refreshReportScript(plugin);
    if (requestId !== state.reportEditorRequest) return;
    // With no cached script, the first completed read supplies the initial
    // baseline for text typed while the editor was loading.
    if (state.reportDrafts[plugin] !== undefined && !state.reportDraftBases[plugin]?.revision) {
      state.reportDraftBases[plugin] = { source: state.reportScripts[plugin] ?? '', revision: state.reportScriptRevisions[plugin] ?? '' };
    }
    // A refreshed cache must not rebase an existing draft: its source and
    // revision stay paired until a save succeeds or the draft is discarded.
    state.reportEditorOriginal = state.reportDraftBases[plugin]?.source ?? state.reportScripts[plugin] ?? '';
    state.reportEditorRevision = state.reportDraftBases[plugin]?.revision ?? state.reportScriptRevisions[plugin] ?? '';
    state.reportEditorConflicted = state.reportEditorRevision !== state.reportScriptRevisions[plugin];
    $('report-editor-text').value = state.reportDrafts[plugin] ?? state.reportEditorOriginal;
    $('report-editor-status').textContent = state.reportDrafts[plugin] !== undefined
      ? (state.reportEditorConflicted ? '已保留未保存的草稿。服务器脚本已有新版本，可载入最新版本后参考原草稿合并。' : '已保留未保存的草稿。保存将替换当前插件脚本。')
      : '已加载最新脚本。保存后可用于当前插件的其他运行。';
    updateReportControls();
  } catch (error) {
    if (requestId === state.reportEditorRequest) $('report-editor-status').textContent = `最新脚本读取失败：${error.message}`;
  }
}

function rememberReportDraft() {
  const plugin = state.reportEditorPlugin;
  const source = $('report-editor-text').value;
  if (source !== state.reportEditorOriginal) {
    state.reportDrafts[plugin] = source;
    state.reportDraftBases[plugin] = { source: state.reportEditorOriginal, revision: state.reportEditorRevision };
  } else {
    delete state.reportDrafts[plugin];
    delete state.reportDraftBases[plugin];
  }
}

function closeReportEditor() {
  rememberReportDraft();
  state.reportEditorRequest += 1;
  const dialog = $('report-editor-dialog');
  if (typeof dialog.close === 'function') dialog.close();
  else dialog.open = false;
}

async function loadLatestReportEditor() {
  const plugin = state.reportEditorPlugin;
  if (!plugin || state.reportEditorSaving || state.reportEditorLoading) return;
  const source = $('report-editor-text').value;
  rememberReportDraft();
  const requestId = ++state.reportEditorRequest;
  state.reportEditorLoading = true;
  updateReportControls();
  $('report-editor-status').textContent = '正在读取最新版本，原草稿将保留供合并…';
  try {
    const latest = await refreshReportScript(plugin);
    if (requestId !== state.reportEditorRequest) return;
    state.reportConflictDrafts[plugin] = source;
    state.reportEditorOriginal = latest.source;
    state.reportEditorRevision = latest.revision;
    state.reportEditorConflicted = false;
    delete state.reportDrafts[plugin];
    delete state.reportDraftBases[plugin];
    $('report-editor-text').value = latest.source;
    $('report-conflict-draft').value = source;
    $('report-conflict-section').hidden = false;
    $('report-conflict-section').open = true;
    $('report-editor-status').textContent = '已载入最新版本，原草稿已单独保留。请参考原草稿合并修改后保存。';
  } catch (error) {
    if (requestId === state.reportEditorRequest) $('report-editor-status').textContent = `最新版本读取失败：${error.message}。原草稿未改动。`;
  } finally {
    state.reportEditorLoading = false;
    updateReportControls();
  }
}

async function saveReportEditor(andRun = false) {
  const plugin = state.reportEditorPlugin;
  if (!plugin || state.reportEditorSaving || state.reportEditorLoading || !state.reportEditorRevision) return;
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
    const saved = await saveReportScript(plugin, source, state.reportEditorRevision);
    if (editorRequest !== state.reportEditorRequest) return;
    state.reportEditorOriginal = saved.source;
    state.reportEditorRevision = saved.revision;
    state.reportEditorConflicted = false;
    $('report-editor-status').textContent = '脚本已保存。';
    if (andRun && selectionGeneration === state.reportSelectionGeneration) {
      closeReportEditor();
      await runReport(plugin, runId);
    }
  } catch (error) {
    if (editorRequest === state.reportEditorRequest) {
      state.reportEditorConflicted = error.status === 409 || state.reportEditorConflicted;
      $('report-editor-status').textContent = error.status === 409
        ? '服务器脚本已有新版本，原草稿已保留。请载入最新版本，参考原草稿合并后再保存。'
        : `无法保存：${error.message}`;
    }
  } finally {
    state.reportEditorSaving = false;
    updateReportControls();
  }
}

function pluginViewDefinition(view) {
  const { revision, ...definition } = view || {};
  return definition;
}

async function savePluginView(plugin, definition, revision) {
  const saved = await jsonFetch(`/api/plugins/${encodeURIComponent(plugin)}/view`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ...pluginViewDefinition(definition), revision }),
  });
  state.pluginViews[plugin] = saved;
  const metadata = state.plugins.find((item) => item.name === plugin);
  if (metadata) metadata.view = saved;
  if ($('result-type').value === plugin) renderAnalysis(plugin, state.selected || {});
  renderPlugins(state.plugins);
  return saved;
}

function prepareDshContext(purpose = 'current') {
  const plugin = $('result-type').value;
  const context = {
    plugin,
    operator_id: state.operatorId,
    run: state.selected,
    // DSH has its own conversation history. The user explicitly copies this
    // snapshot when they want to attach it to a native DSH conversation.
    snapshot_at: new Date().toISOString(),
  };
  let instruction = '请结合以下结果上下文回答我的问题。先说明数据库中哪些结果已入库、是否完整，以及当前可以进行哪些分析。未选择运行时，可先帮助了解系统状态和排查配置。上下文是当前页面的数据快照，必要时通过 TestTracking 工具重新读取。';
  if (purpose === 'report') {
    context.current_script = state.reportScripts[plugin] || '';
    context.script_revision = state.reportScriptRevisions[plugin] || '';
    context.script_contract = state.analysisCatalogs[plugin]?.script_contract;
    context.analysis_catalog = state.analysisCatalogs[plugin] || null;
    instruction = `请为 ${plugin} 插件生成并应用可替换的 Python 交互报告。先通过 TestTracking 工具读取插件说明、数据库结构和结果完整性，分析只依赖数据库。实现 render(connection, context) 并返回独立 HTML 字符串，connection 为只读数据库连接，行是字典，context['run_id'] 为当前运行。图表和表格的样式、脚本均内联，不依赖外网。保存共享脚本前先读取最新 source 和 revision，携带该 revision 和当前 operator_id 保存；出现版本冲突时保留草稿并与最新脚本合并。保存成功后为当前运行生成报告，尚未选择运行时先保存脚本即可。页面会自动读取最新报告，打开“编辑 Python”可读取最新脚本。也可以给出完整源码供我在“编辑 Python”中检查和保存。`;
  } else if (purpose === 'view') {
    context.current_view = pluginViewDefinition(state.pluginViews[plugin]);
    context.view_revision = state.pluginViews[plugin]?.revision || '';
    context.view_schema = state.pluginViewSchema;
    instruction = `请为 ${plugin} 插件设计结果展示定义，突出最重要的分析指标。遵循上下文中的 view_schema，仅使用允许的数据路径，plugin 必须为 ${plugin}。先给出完整 JSON 供我在“编辑展示”中检查和保存，不要覆盖其他用户的配置。`;
  }
  $('dsh-context-prompt').value = `${instruction}\n\n结果上下文（JSON）：\n${JSON.stringify(context, null, 2)}`;
  $('dsh-context-section').open = true;
  $('dsh-context-status').textContent = `已准备 ${plugin || '系统'} / ${state.selected?.run_id || '未选择运行'} 的上下文。复制后粘贴到 DSH，补充要求并发送。`;
  $('dsh-context-copy').disabled = false;
  $('console').dataset.aiOpen = 'true';
  ensureDshWeb();
}

async function copyDshContext() {
  const field = $('dsh-context-prompt');
  if (!field.value) return;
  try {
    if (globalThis.navigator?.clipboard?.writeText) await globalThis.navigator.clipboard.writeText(field.value);
    else {
      field.focus?.();
      field.select?.();
      if (!document.execCommand?.('copy')) throw new Error('请选中上面的提示内容，手动复制后粘贴到 DSH。');
    }
    $('dsh-context-status').textContent = '提示已复制。请粘贴到 DSH 原生输入框，补充要求后发送。';
  } catch (_) {
    field.focus?.();
    field.select?.();
    $('dsh-context-status').textContent = '浏览器未允许自动复制，请手动复制已选中的提示内容。';
  }
}

function designPluginView() {
  if (!state.pluginViews[$('result-type').value]) return;
  prepareDshContext('view');
}

function designPythonReport() {
  if (!$('result-type').value) return;
  prepareDshContext('report');
}

function openViewEditor() {
  const plugin = $('result-type').value;
  const definition = state.pluginViews[plugin];
  if (!definition) return;
  state.viewEditorPlugin = plugin;
  state.viewEditorRevision = definition.revision;
  $('view-editor-plugin').textContent = plugin;
  $('view-editor-text').value = JSON.stringify(pluginViewDefinition(definition), null, 2);
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
  const plugin = state.viewEditorPlugin;
  const revision = state.viewEditorRevision;
  $('save-view-editor').disabled = true;
  try {
    const definition = JSON.parse($('view-editor-text').value);
    await savePluginView(plugin, definition, revision);
    closeViewEditor();
    $('footer-status').textContent = `${plugin} 的结果展示已保存并刷新。`;
  } catch (error) {
    $('view-editor-status').textContent = `无法保存：${error.message}`;
  } finally {
    $('save-view-editor').disabled = false;
  }
}

function ensureDshWeb() {
  if ($('console').dataset.aiOpen === 'true' && !state.dshWebAttempted
      && state.providers.some((item) => item.name === 'dsh' && item.available)) {
    void openDshWeb();
  }
}

async function openDshWeb() {
  if (state.dshWebLoading) return;
  state.dshWebAttempted = true;
  state.dshWebLoading = true;
  $('dsh-native-open').disabled = true;
  $('dsh-native-open').textContent = '正在启动';
  $('dsh-native-status').textContent = '正在启动服务器 DSH Web...';
  try {
    const result = await jsonFetch('/api/ai/dsh-web', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ operator_id: state.operatorId }),
    });
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
$('overview-filter').addEventListener('click', (event) => { const button = event.target.closest('button'); if (!button) return; state.filter = button.dataset.filter; document.querySelectorAll('#overview-filter button').forEach((item) => item.setAttribute('aria-pressed', String(item === button))); renderRunCards(); });
$('run-start-form').addEventListener('submit', async (event) => { event.preventDefault(); await schedulerCommand('run', $('run-plugin').value); });
$('run-force').addEventListener('change', () => renderScheduler());
$('run-plugin').addEventListener('change', () => { state.selectedWorkerIds = []; renderScheduler(); });
$('run-all-clients').addEventListener('change', renderScheduler);
$('run-clients').addEventListener('change', () => { state.selectedWorkerIds = Array.from($('run-clients').selectedOptions).map((item) => item.value); });
$('run-wakeup').addEventListener('click', () => schedulerCommand('wakeup', $('run-plugin').value));
$('active-runs').addEventListener('click', (event) => {
  const button = event.target.closest('[data-stop-run]');
  if (button && !button.disabled) return schedulerCommand('stop', button.dataset.stopPlugin, button.dataset.stopRun);
});
$('clients-refresh').addEventListener('click', () => Promise.all([loadScheduler(), loadAgents(), loadArtifacts()]));
$('clients-select-all').addEventListener('change', () => {
  if (state.deploymentBusy) return;
  state.selectedAgentIds = $('clients-select-all').checked ? (state.agents || []).filter((agent) => !agentUnavailableReason(agent)).map((agent) => agent.client_id) : [];
  renderClients();
});
$('clients-configure').addEventListener('click', () => openBatchDeployment());
$('client-list').addEventListener('change', (event) => {
  const id = event.target.dataset.agentSelect;
  if (!id || state.deploymentBusy) return;
  const agent = (state.agents || []).find((item) => item.client_id === id);
  if (event.target.checked && agentUnavailableReason(agent)) return;
  state.selectedAgentIds = state.selectedAgentIds.filter((value) => value !== id);
  if (event.target.checked) state.selectedAgentIds.push(id);
  renderClients();
});
$('client-list').addEventListener('click', (event) => {
  const button = event.target.closest('[data-configure-agent]');
  if (button && !button.disabled) return openBatchDeployment([button.dataset.configureAgent]);
});
$('deployment-go-clients').addEventListener('click', () => navigate('clients'));
for (const kind of ['plugins', 'resources', 'schedule']) $('batch-scope-' + kind).addEventListener('change', () => {
  state.deploymentWizardDirty = true;
  $('deployment-preview-status').textContent = '修改范围已变化，请重新核对全部客户端。';
  showDeploymentStep(state.deploymentStep);
});
$('logs-form').addEventListener('submit', async (event) => { event.preventDefault(); await loadLogs(); });
$('logs-file').addEventListener('change', loadLogs);
$('logs-level').addEventListener('change', loadLogs);
$('logs-limit').addEventListener('change', loadLogs);
document.querySelectorAll('[data-workspace-tab]').forEach((button) => {
  button.addEventListener('click', () => setWorkspaceTab(button.dataset.workspaceTab));
  button.addEventListener('keydown', (event) => {
    const tabs = ['plugins', 'artifacts', 'deployments', 'settings'];
    const index = tabs.indexOf(button.dataset.workspaceTab);
    const next = event.key === 'ArrowRight' ? (index + 1) % tabs.length : event.key === 'ArrowLeft' ? (index + tabs.length - 1) % tabs.length : event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : -1;
    if (next >= 0) { event.preventDefault(); setWorkspaceTab(tabs[next], true); }
  });
});
$('workspace-drawer-close')?.addEventListener('click', closeWorkspaceDrawer);
$('workspace-drawer')?.addEventListener('cancel', (event) => { event.preventDefault(); closeWorkspaceDrawer(); });
$('workspace-drawer-content')?.addEventListener('click', async (event) => {
  const copy = event.target.closest('[data-copy-value]');
  if (copy) { await copyWorkspaceValue(copy.dataset.copyValue); return; }
  const link = event.target.closest('[data-workspace-go]');
  if (link) { closeWorkspaceDrawer(); setWorkspaceTab(link.dataset.workspaceGo, true); return; }
  const edit = event.target.closest('[data-edit-deployment]');
  if (edit && !state.deploymentBusy) { closeWorkspaceDrawer(); await openBatchDeployment([edit.dataset.editDeployment]); return; }
  const install = event.target.closest('[data-install-artifact]');
  if (install && !state.serverPluginBusy) {
    closeWorkspaceDrawer(); setWorkspaceTab('plugins');
    $('server-plugin-section').open = true;
    $('server-plugin-artifact').value = install.dataset.installArtifact;
    await previewServerPlugin();
  }
});
$('plugin-grid').addEventListener('click', (event) => { const button = event.target.closest('[data-plugin-detail]'); if (button) openPluginDetails(button.dataset.pluginDetail, button); });
$('plugin-install-open')?.addEventListener('click', () => { $('server-plugin-section').open = true; $('server-plugin-artifact').focus?.(); });
$('artifact-upload-open')?.addEventListener('click', () => { $('artifact-upload-section').open = true; $('artifact-kind').focus?.(); });
$('artifact-search')?.addEventListener('input', () => { state.artifactSearch = $('artifact-search').value; renderArtifacts(); });
$('artifact-filter')?.addEventListener('change', () => { state.artifactFilter = $('artifact-filter').value; renderArtifacts(); });
$('config-kind').addEventListener('change', () => loadConfig($('config-kind').value));
$('config-json').addEventListener('input', () => { rememberConfigDraft(); renderConfigForm(); updateConfigControls(); });
$('config-form')?.addEventListener('input', editCommonConfig);
$('config-form')?.addEventListener('change', editCommonConfig);
$('config-reload').addEventListener('click', () => { if (!configDirty()) return loadConfig(state.configKind, true); });
$('config-discard').addEventListener('click', () => loadConfig(state.configKind, true));
$('config-save').addEventListener('click', saveConfig);
$('artifact-upload-form').addEventListener('submit', uploadArtifact);
$('server-plugin-preview').addEventListener('click', previewServerPlugin);
$('server-plugin-install').addEventListener('click', installServerPlugin);
$('server-plugin-artifact').addEventListener('change', () => { state.serverPluginPreview = null; $('server-plugin-install').disabled = true; $('server-plugin-plan').textContent = ''; if ($('server-plugin-summary')) $('server-plugin-summary').textContent = ''; });
$('artifacts-refresh').addEventListener('click', loadArtifacts);
$('deployment-form').addEventListener('submit', saveDeployment);
$('deployment-load').addEventListener('click', async () => {
  const guided = state.deploymentEditorOpen && $('deployment-editor-mode').value === 'wizard';
  const advancedOpen = Boolean($('deployment-advanced')?.open);
  const loaded = await loadDeployment();
  if (!loaded || !guided) return loaded;
  const enabled = Object.entries(readDeploymentPatch().plugins || {}).find(([, item]) => item?.enabled)?.[0];
  if (!enabled || ['coverage', 'stpcompare'].includes(enabled)) {
    $('deployment-editor-mode').value = 'wizard';
    if ($('deployment-advanced')) $('deployment-advanced').open = advancedOpen;
    updateDeploymentMode();
  }
  return loaded;
});
$('deployment-new')?.addEventListener('click', () => {
  if (state.deploymentBusy) return;
  if (state.selectedAgentIds.length) return openBatchDeployment();
  navigate('clients'); $('client-list').scrollIntoView?.({ block: 'start' });
  $('deployment-status').textContent = '请先选择需要配置的在线空闲客户端。';
});
$('deployment-close')?.addEventListener('click', closeDeploymentEditor);
$('deployment-reset').addEventListener('click', () => state.deploymentBatchTargets && openBatchDeployment(state.deploymentBatchTargets, true));
$('deployment-history-refresh').addEventListener('click', () => state.deploymentBatchTargets ? loadBatchDeploymentHistory() : loadDeployment(state.deploymentClient));
$('deployment-next')?.addEventListener('click', advanceDeploymentStep);
$('deployment-back')?.addEventListener('click', () => { if (!state.deploymentBusy) showDeploymentStep($('deployment-editor-mode').value === 'json' ? 0 : state.deploymentStep - 1, true); });
document.querySelectorAll('[data-deployment-step]').forEach((button) => button.addEventListener('click', () => {
  const step = Number(button.dataset.deploymentStep);
  if (!state.deploymentBusy && step <= state.deploymentStep) showDeploymentStep(step, true);
}));
$('deployment-editor-mode').addEventListener('change', () => {
  if (state.deploymentBusy) return;
  if ($('deployment-editor-mode').value === 'wizard') {
    const source = JSON.stringify({ patch: $('deployment-patch').value, components: deploymentComponents() });
    if (state.deploymentWizard && state.deploymentWizardSource === source) renderDeploymentWizard();
    else importDeploymentWizard();
  } else if (state.deploymentWizard) {
    try {
      const model = collectDeploymentWizard();
      const desiredMode = $('deployment-editor-mode').value;
      if (state.deploymentBatchTargets) $('deployment-editor-mode').value = 'wizard';
      let draft;
      try { draft = state.deploymentBatchTargets ? buildBatchDeploymentRequest() : buildDeploymentWizard(readDeploymentPatch(), deploymentComponents(), model, state.artifacts, executionPluginNames()); }
      finally { $('deployment-editor-mode').value = desiredMode; }
      $('deployment-patch').value = JSON.stringify(draft.config_patch, null, 2);
      state.deploymentSelected = Object.fromEntries(draft.components.map((item) => [item.slot, item.artifact_id]));
      state.deploymentWizard = model;
      state.deploymentWizardSource = JSON.stringify({ patch: $('deployment-patch').value, components: deploymentComponents() });
      state.deploymentPreview = '';
      state.deploymentBinding = null;
      $('deployment-bindings').hidden = true;
      $('deployment-preview-status').textContent = '表单修改已同步到高级 JSON，发布时将使用当前配置和制品。';
    } catch (error) {
      $('deployment-editor-mode').value = 'wizard';
      $('deployment-preview-status').textContent = `无法切换到高级 JSON：${error.message}。请先修正表单。`;
      $('deployment-status').textContent = $('deployment-preview-status').textContent;
    }
  }
  updateDeploymentMode();
});
function switchDeploymentPlugin(plugin) {
  if (state.deploymentBusy) return;
  const current = state.deploymentWizard ? collectDeploymentWizard() : null;
  if (current) {
    delete current.enabledPlugins; delete current.pluginModels;
    state.deploymentWizardDrafts[current.plugin] = current;
  }
  state.deploymentWizard = state.deploymentWizardDrafts[plugin] || readDeploymentWizard(readDeploymentPatch(), deploymentComponents(), plugin, Boolean(state.deploymentBatchTargets));
  if (current) { state.deploymentWizard.threads = current.threads; state.deploymentWizard.windows = current.windows; }
  renderDeploymentWizard(); state.deploymentWizardDirty = true;
}
$('deployment-plugin').addEventListener('change', () => switchDeploymentPlugin($('deployment-plugin').value));
$('deployment-resource-plugin').addEventListener('change', () => switchDeploymentPlugin($('deployment-resource-plugin').value));
$('deployment-enabled-plugins').addEventListener('change', (event) => {
  const name = event.target.dataset.deploymentEnable;
  if (!name || state.deploymentBusy) return;
  state.deploymentEnabledPlugins = state.deploymentEnabledPlugins.filter((item) => item !== name);
  if (event.target.checked) state.deploymentEnabledPlugins.push(name);
  state.deploymentWizardDirty = true;
  $('deployment-preview-status').textContent = '启用插件已修改，请重新核对部署后发布。';
});
$('deployment-import').addEventListener('click', () => importDeploymentWizard());
$('deployment-preview').addEventListener('click', previewDeploymentWizard);
$('deployment-bind-scheduler').addEventListener('click', bindDeploymentScheduler);
$('deployment-wizard').addEventListener('input', deploymentWizardChanged);
$('deployment-wizard').addEventListener('change', deploymentWizardChanged);
for (const kind of ['execution', 'update']) $('deployment-' + kind + '-add').addEventListener('click', () => {
  const window = state.deploymentWizard?.windows[kind]; if (!window) return;
  window.mode = 'custom'; window.items.push({ days: [1, 2, 3, 4, 5, 6, 7], start: '09:00', end: '18:00' });
  state.deploymentWizardDirty = true; renderDeploymentWindows(kind);
});
$('deployment-wizard').addEventListener('click', (event) => {
  const button = event.target.closest('[data-window-remove]'); if (!button || !state.deploymentWizard) return;
  const kind = button.dataset.windowRemove;
  state.deploymentWizard.windows[kind].items.splice(Number(button.dataset.windowIndex), 1);
  state.deploymentWizardDirty = true; renderDeploymentWindows(kind);
});
$('artifact-list').addEventListener('change', selectArtifact);
$('artifact-list').addEventListener('click', (event) => { const button = event.target.closest('[data-artifact-detail]'); if (button) openArtifactDetails(button.dataset.artifactDetail, button); });
$('deployment-list').addEventListener('click', (event) => {
  const edit = event.target.closest('[data-edit-deployment]');
  if (edit) return openBatchDeployment([edit.dataset.editDeployment]);
  const detail = event.target.closest('[data-deployment-detail]');
  if (detail) openDeploymentDetails(detail.dataset.deploymentDetail, detail);
});
$('deployment-rollback-button').addEventListener('click', rollbackDeployment);
setWorkspaceTab(state.workspaceTab);
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
$('result-type').addEventListener('change', () => switchPlugin($('result-type').value).catch((error) => { $('report-state').textContent = error.message; }));
$('result-run').addEventListener('change', () => selectRun($('result-run').value, $('result-type').value).catch((error) => { $('report-state').textContent = error.message; }));
$('refresh').addEventListener('click', () => loadAll().catch(showLoadError));
$('ai-toggle').addEventListener('click', () => { $('console').dataset.aiOpen = String($('console').dataset.aiOpen !== 'true'); ensureDshWeb(); });
$('ai-close').addEventListener('click', () => { $('console').dataset.aiOpen = 'false'; });
$('ai-expand').addEventListener('click', toggleAiExpanded);
$('text-smaller').addEventListener('click', () => setUiScale(state.uiScale - 0.1));
$('text-larger').addEventListener('click', () => setUiScale(state.uiScale + 0.1));
$('edit-plugin-view').addEventListener('click', openViewEditor);
$('close-view-editor').addEventListener('click', closeViewEditor);
$('save-view-editor').addEventListener('click', saveViewEditor);
$('ai-design-view').addEventListener('click', designPluginView);
$('ai-design-report').addEventListener('click', designPythonReport);
$('run-report').addEventListener('click', () => runReport());
$('edit-report-script').addEventListener('click', openReportEditor);
$('close-report-editor').addEventListener('click', closeReportEditor);
$('report-editor-text').addEventListener('input', rememberReportDraft);
$('report-editor-dialog').addEventListener('cancel', () => { rememberReportDraft(); state.reportEditorRequest += 1; });
$('save-report-editor').addEventListener('click', () => saveReportEditor());
$('save-run-report-editor').addEventListener('click', () => saveReportEditor(true));
$('report-load-latest').addEventListener('click', loadLatestReportEditor);
$('dsh-native-open').addEventListener('click', openDshWeb);
$('dsh-context-current').addEventListener('click', () => prepareDshContext());
$('dsh-context-copy').addEventListener('click', copyDshContext);
$('dsh-native-frame').addEventListener('load', () => { if (state.dshWebUrl) $('dsh-native-status').textContent = 'DSH 原生界面已连接'; });
$('dsh-native-frame').addEventListener('error', () => { if (state.dshWebUrl) $('dsh-native-status').textContent = 'DSH 界面加载失败，请重新连接。'; });

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
  $('report-state').textContent = `加载失败：${error.message}`;
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
    if (state.page === 'clients') await loadAgents();
    if (['plugins', 'clients'].includes(state.page) && !state.deploymentBusy && !state.artifactBusy) await loadArtifacts();
    if (state.page === 'logs' && $('logs-auto').checked && !state.logsBusy) await loadLogs();
  }, 5000);
}
