const AI_CONVERSATIONS_KEY = 'testtracking.ai.conversations.v1';
const AI_PANEL_WIDTH_KEY = 'testtracking.ai.panel-width.v1';

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

const state = {
  runs: [], selected: null, page: 'results', filter: '', readiness: null,
  providers: [], selectionRequest: 0, loadRequest: 0, apiHealthy: false,
  conversationIds: readStoredJson(AI_CONVERSATIONS_KEY, {}), aiBusy: false,
  panelWidth: readStoredNumber(AI_PANEL_WIDTH_KEY, 400), dshWebUrl: '', dshWebLoading: false,
};
const $ = (id) => document.getElementById(id);

const pages = {
  overview: ['运行总览', '查看持久化运行和文件结果状态。'],
  results: ['结果分析', '查看已完成运行的业务结果与差异。'],
  clients: ['客户端', '查看注册状态、资源使用与最近心跳。'],
  history: ['历史运行', '查看长期保存的运行记录。'],
  plugins: ['插件与配置', '查看已加载的业务结果适配器。'],
  logs: ['运行日志', '查看服务端与结果服务运行记录。']
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
function statusLabel(value) { return ({ finished: '已完成', running: '运行中', planned: '已计划', stopped: '已停止' })[value] || value || '--'; }
function timeLabel(value) { return value ? new Date(value).toLocaleString('zh-CN', { hour12: false }) : '--'; }
function executionChipClass(value) { return value === 'running' ? 'blue' : (value === 'stopped' ? 'amber' : ''); }

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
  // A local provider can answer general questions without a selected run.
  // Selecting a run only enriches the context sent with the request.
  const enabled = hasProvider && !state.aiBusy;
  $('question').disabled = !enabled;
  $('send-button').disabled = !enabled;
  $('new-conversation').disabled = state.aiBusy || !hasProvider;
  document.querySelectorAll('#quick-actions button').forEach((button) => { button.disabled = !enabled; });
  $('question').placeholder = hasProvider ? (hasRun ? '询问当前结果...' : '询问 Codex 或 DSH...') : '当前 AI 服务不可用';
  $('compose-hint').textContent = hasProvider
    ? (hasRun ? '当前运行的结构化结果' : '未选择运行，使用通用上下文')
    : 'AI 服务不可用';
  const providerStatus = $('provider-availability');
  providerStatus.className = hasProvider ? 'available' : 'unavailable';
  providerStatus.textContent = provider ? `${provider.name} ${hasProvider ? '可用' : '不可用'}` : '没有可用的 AI 服务';
  const conversationId = state.conversationIds[$('provider').value];
  $('conversation-status').textContent = conversationId ? `会话 ${conversationId.slice(0, 8)}` : '新会话';

  const dshAvailable = state.providers.some((item) => item.name === 'dsh' && item.available);
  $('dsh-native-open').disabled = !dshAvailable || state.dshWebLoading;
  if (!dshAvailable && !state.dshWebUrl) $('dsh-native-status').textContent = '本机 DSH 命令不可用。';
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
}

function renderMetrics() {
  const finished = state.runs.filter((run) => run.status === 'finished').length;
  const success = state.runs.reduce((sum, run) => sum + Number(run.success_files || 0), 0);
  const failed = state.runs.reduce((sum, run) => sum + Number(run.failed_files || 0), 0);
  $('metric-runs').innerHTML = `${number(state.runs.length)}<small>个运行</small>`;
  $('metric-finished').innerHTML = `${number(finished)}<small>个运行</small>`;
  $('metric-success').innerHTML = `${number(success)}<small>个</small>`;
  $('metric-failed').innerHTML = `${number(failed)}<small>个</small>`;
}

function renderRunCards() {
  const runs = state.runs.filter((run) => !state.filter || run.plugin_type === state.filter);
  const [emptyTitle, emptyDetail] = emptyRunState();
  $('overview-count').textContent = `${runs.length} 条`;
  $('run-cards').innerHTML = runs.length ? runs.slice(0, 8).map((run) => `
    <article class="run-card">
      <div class="run-card-top"><div class="run-title"><span class="plugin-badge">${escapeHtml(run.plugin_type.slice(0, 1))}</span><div><h3>${escapeHtml(run.plugin_type)} <span class="chip ${executionChipClass(run.status)}">${escapeHtml(statusLabel(run.status))}</span></h3><div class="run-id">${escapeHtml(run.run_id)} · 数据集 ${escapeHtml(run.dataset_version || '--')}</div></div></div><button type="button" class="button" data-open-run="${escapeHtml(run.run_id)}" data-open-plugin="${escapeHtml(run.plugin_type)}">查看结果</button></div>
      <div class="run-meta"><span>预期文件 <b>${number(run.expected_files)}</b></span><span>成功 <b>${number(run.success_files)}</b></span><span>失败 <b>${number(run.failed_files)}</b></span><span>创建时间 <b>${escapeHtml(timeLabel(run.created_at))}</b></span></div>
    </article>`).join('') : `<div class="empty-panel"><span>◇</span><h2>${emptyTitle}</h2><p>${emptyDetail}</p></div>`;
}

function renderHistory() {
  const [emptyTitle] = emptyRunState();
  $('history-count').textContent = `${state.runs.length} 条`;
  $('history-table').innerHTML = state.runs.length ? state.runs.map((run) => `<tr data-open-run="${escapeHtml(run.run_id)}" data-open-plugin="${escapeHtml(run.plugin_type)}"><td>${escapeHtml(run.run_id)}</td><td>${escapeHtml(run.plugin_type)}</td><td><span class="chip ${executionChipClass(run.status)}">${escapeHtml(statusLabel(run.status))}</span></td><td>${number(run.success_files)} 成功 / ${number(run.failed_files)} 失败</td><td>${escapeHtml(timeLabel(run.finished_at))}</td></tr>`).join('') : `<tr><td colspan="5" class="empty-row">${emptyTitle}</td></tr>`;
}

function fillRunSelect(plugin) {
  const runs = state.runs.filter((run) => run.plugin_type === plugin);
  $('result-run').innerHTML = runs.length ? runs.map((run) => `<option value="${escapeHtml(run.run_id)}">${escapeHtml(run.run_id)} · ${escapeHtml(run.dataset_version || '--')}</option>`).join('') : '<option value="">暂无运行记录</option>';
  return runs;
}

function resetResult(plugin) {
  state.selected = null;
  $('execution-status').textContent = '--';
  $('completeness').innerHTML = '--<small> / --</small>';
  $('analysis-label').textContent = plugin === 'coverage' ? '行覆盖率' : '存在差异';
  $('analysis-value').textContent = '--';
  $('analysis-state').textContent = '等待选择运行';
  $('ai-context').textContent = `上下文：${plugin} / 尚未选择运行`;
  renderAnalysis(plugin, {});
}

function renderAnalysis(plugin, run) {
  const hasRun = Boolean(run.run_id);
  const completeness = run.result_completeness || {};
  const outcomes = run.outcomes || {};
  const analysis = run.analysis || {};
  const expected = Number(completeness.expected_files ?? run.expected_files ?? 0);
  const success = Number(completeness.successful_files ?? outcomes.success ?? 0);
  const failed = Number(completeness.failed_files ?? outcomes.failed ?? 0);
  const pending = Math.max(0, expected - success - failed);
  $('execution-status').textContent = statusLabel(run.status);
  $('completeness').innerHTML = hasRun ? `${number(success + failed)}<small> / ${number(expected)}</small>` : '--<small> / --</small>';
  const analysisStatus = run.analysis_status || (Object.keys(analysis).length ? 'ready' : (hasRun ? 'pending' : 'not_selected'));
  const analysisError = analysisStatus === 'error';
  const analysisLabels = { ready: '分析结果已发布', error: '后处理结果读取失败', not_configured: '后处理未配置', pending: '等待后处理结果', not_selected: '等待选择运行' };
  setAnalysisState(analysisStatus, analysisLabels[analysisStatus] || '等待后处理结果');
  $('ai-context').textContent = `上下文：${plugin} / ${run.run_id || '尚未选择运行'} / 当前结果`;
  $('donut-total').textContent = number(expected);
  $('file-total-chip').textContent = `${number(expected)} 个文件`;
  $('legend-success').textContent = number(success);
  $('legend-pending').textContent = number(pending);
  $('legend-failed').textContent = number(failed);
  const base = Math.max(expected, 1);
  const successEnd = success / base * 100;
  const pendingEnd = successEnd + pending / base * 100;
  $('result-donut').style.background = expected ? `conic-gradient(var(--green) 0 ${successEnd}%, var(--amber) ${successEnd}% ${pendingEnd}%, var(--red) ${pendingEnd}% 100%)` : 'var(--line)';
  $('coverage-view').hidden = plugin !== 'coverage';
  $('compare-view').hidden = plugin !== 'stpcompare';
  if (plugin === 'coverage') {
    const percentRaw = Number(analysis.percent || 0);
    const percent = percentRaw <= 1 ? percentRaw * 100 : percentRaw;
    $('analysis-label').textContent = '行覆盖率';
    $('analysis-value').textContent = Object.keys(analysis).length ? `${percent.toFixed(1)}%` : '--';
    const bars = [['覆盖行', analysis.covered_lines, analysis.total_lines], ['精简用例', analysis.selected_files, analysis.total_files]];
    $('coverage-bars').innerHTML = bars.map(([label, value, total]) => { const ratio = Number(total) > 0 ? Math.min(100, Number(value || 0) / Number(total) * 100) : 0; return `<div class="bar-row"><span>${label}</span><div class="bar-track"><i style="width:${ratio}%"></i></div><strong>${number(value)}</strong></div>`; }).join('');
    $('coverage-table').innerHTML = Object.keys(analysis).length ? [['总文件', analysis.total_files, '已结算'], ['精简用例', analysis.selected_files, '已生成'], ['累计覆盖点', analysis.cumulative_points, '已统计'], ['覆盖行 / 总行', `${number(analysis.covered_lines)} / ${number(analysis.total_lines)}`, '已统计']].map(([label, value, status]) => `<tr><td>${label}</td><td>${value ?? '--'}</td><td><span class="chip green">${status}</span></td></tr>`).join('') : `<tr><td colspan="3" class="empty-row">${analysisError ? 'coverage 后处理结果读取失败，请检查结果服务日志' : '当前运行尚无 coverage 派生结果'}</td></tr>`;
  } else {
    const different = Number(analysis.different || 0);
    $('analysis-label').textContent = '存在差异';
    $('analysis-value').textContent = Object.keys(analysis).length ? `${number(different)} 个文件` : '--';
    const emptyAnalysis = analysisStatus === 'not_configured' ? 'stpcompare 后处理表尚未配置' : (analysisError ? 'stpcompare 后处理结果读取失败，请检查结果服务日志' : '当前运行尚无 stpcompare 分析结果');
    $('compare-table').innerHTML = Object.keys(analysis).length ? [['相同', analysis.same, 'green'], ['不同', analysis.different, 'amber'], ['不完整', analysis.incomplete, 'amber']].map(([label, value, color]) => `<tr><td>${label}</td><td>${number(value)}</td><td><span class="chip ${color}">${label}</span></td></tr>`).join('') : `<tr><td colspan="3" class="empty-row">${emptyAnalysis}</td></tr>`;
  }
  updateAiControls();
}

async function selectRun(runId, plugin) {
  const requestId = ++state.selectionRequest;
  if (!runId) {
    state.selected = null;
    resetResult(plugin);
    return;
  }
  state.selected = null;
  renderAnalysis(plugin, {});
  setAnalysisState('pending', '正在加载运行');
  $('ai-context').textContent = `上下文：${plugin} / ${runId} / 加载中`;
  let selected;
  try {
    selected = await jsonFetch(`/api/runs/${encodeURIComponent(runId)}?plugin=${encodeURIComponent(plugin)}`);
  } catch (error) {
    if (requestId === state.selectionRequest) {
      resetResult(plugin);
      setAnalysisState('error', '运行加载失败');
    }
    throw error;
  }
  if (requestId !== state.selectionRequest) return;
  state.selected = selected;
  renderAnalysis(plugin, state.selected);
}

async function switchPlugin(plugin, preferredRun = '') {
  $('result-type').value = plugin;
  const runs = fillRunSelect(plugin);
  const runId = runs.some((run) => run.run_id === preferredRun) ? preferredRun : (runs[0]?.run_id || '');
  $('result-run').value = runId;
  await selectRun(runId, plugin);
}

function renderPlugins(plugins) {
  $('plugin-grid').innerHTML = plugins.map((plugin) => `<article class="plugin-card"><div class="panel-head"><h2>${escapeHtml(plugin.name)}</h2><span class="chip green">已注册</span></div><p>${plugin.name === 'coverage' ? '覆盖率原始结果与派生摘要' : '文本比较结果与完整性分类'}</p></article>`).join('');
}

async function loadAll() {
  const requestId = ++state.loadRequest;
  state.apiHealthy = false;
  const preferredPlugin = $('result-type').value;
  const preferredRun = $('result-run').value;
  const preferredProvider = $('provider').value;
  try {
    const [health, readiness] = await Promise.all([jsonFetch('/healthz'), statusFetch('/readyz')]);
    if (requestId !== state.loadRequest) return;
    if (health.status !== 'ok') throw new Error('结果服务健康检查失败');
    state.apiHealthy = true;

    const [plugins, providers] = await Promise.all([jsonFetch('/api/plugins'), jsonFetch('/api/ai/providers')]);
    if (requestId !== state.loadRequest) return;
    // Provider availability is independent of database queries. Publish it
    // before loading runs so an unavailable database cannot disable local AI.
    state.providers = providers;
    renderServiceState(readiness, providers);
    const wantedProvider = $('provider').value || preferredProvider;
    $('provider').innerHTML = providers.map((provider) => `<option value="${escapeHtml(provider.name)}" ${provider.available ? '' : 'disabled'}>${escapeHtml(provider.name)}${provider.available ? '' : ' · 不可用'}</option>`).join('');
    const selectedProvider = providers.find((provider) => provider.name === wantedProvider && provider.available)
      || providers.find((provider) => provider.available);
    if (selectedProvider) $('provider').value = selectedProvider.name;
    updateAiControls();
    const runs = readiness.ok ? await jsonFetch('/api/runs?limit=100') : [];
    if (requestId !== state.loadRequest) return;

    const currentPlugin = $('result-type').value;
    const currentRun = $('result-run').value;
    const selectionChanged = currentPlugin !== preferredPlugin || currentRun !== preferredRun;

    state.runs = runs;
    renderPlugins(plugins); renderMetrics(); renderRunCards(); renderHistory();
    await switchPlugin(selectionChanged ? currentPlugin : preferredPlugin, selectionChanged ? currentRun : preferredRun);
    if (requestId !== state.loadRequest) return;
    $('snapshot-time').textContent = new Date().toLocaleString('zh-CN', { hour12: false });
    updateAiControls();
  } catch (error) {
    if (requestId !== state.loadRequest) return;
    throw error;
  }
}

function addMessage(role, text) {
  const node = document.createElement('div');
  node.className = `message ${role === 'user' ? 'user-message' : 'assistant-message'}`;
  node.textContent = text;
  $('chat').appendChild(node);
  $('chat').scrollTop = $('chat').scrollHeight;
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

async function askAi(message) {
  if (!message.trim()) return;
  const provider = $('provider').value;
  if (!state.providers.some((item) => item.name === provider && item.available)) { addMessage('assistant', '当前 AI 服务不可用。'); return; }
  state.aiBusy = true;
  updateAiControls();
  addMessage('user', message);
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
        context: { run: state.selected, result_type: $('result-type').value },
      }),
    });
    if (result.conversation_id) {
      state.conversationIds[provider] = result.conversation_id;
      persistConversationIds();
    }
    waiting.textContent = result.text;
  } catch (error) { waiting.textContent = `请求失败：${error.message}`; }
  finally { state.aiBusy = false; updateAiControls(); }
  $('chat').scrollTop = $('chat').scrollHeight;
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
    $('dsh-native-status').textContent = '本机 DSH 命令不可用。';
    return;
  }
  state.dshWebLoading = true;
  $('dsh-native-open').disabled = true;
  $('dsh-native-open').textContent = '正在启动';
  $('dsh-native-status').textContent = '正在启动本机 DSH Web...';
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

function setPanelWidth(width, persist = true) {
  const viewport = Number(window.innerWidth || 1440);
  state.panelWidth = Math.max(320, Math.min(Number(width) || 400, Math.max(320, viewport - 360)));
  if (typeof $('console').style.setProperty === 'function') $('console').style.setProperty('--assistant-width', `${state.panelWidth}px`);
  $('ai-resize').setAttribute('aria-valuenow', String(state.panelWidth));
  $('ai-resize').setAttribute('aria-valuemin', '320');
  $('ai-resize').setAttribute('aria-valuemax', String(Math.max(320, viewport - 360)));
  if (persist) {
    try { if (typeof localStorage !== 'undefined') localStorage.setItem(AI_PANEL_WIDTH_KEY, String(state.panelWidth)); } catch (_) { /* Keep the current in-memory width. */ }
  }
}

function toggleAiExpanded() {
  const expanded = $('console').dataset.aiExpanded === 'true';
  $('console').dataset.aiExpanded = String(!expanded);
  $('ai-expand').textContent = expanded ? '↗' : '↙';
  $('ai-expand').title = expanded ? '展开 AI 助手' : '恢复 AI 助手宽度';
  $('ai-expand').setAttribute('aria-label', $('ai-expand').title);
}

document.querySelectorAll('.nav-item').forEach((button) => button.addEventListener('click', () => navigate(button.dataset.page)));
document.querySelectorAll('[data-prompt]').forEach((button) => button.addEventListener('click', () => askAi(button.dataset.prompt)));
$('overview-filter').addEventListener('click', (event) => { const button = event.target.closest('button'); if (!button) return; state.filter = button.dataset.filter; document.querySelectorAll('#overview-filter button').forEach((item) => item.setAttribute('aria-pressed', String(item === button))); renderRunCards(); });
$('run-cards').addEventListener('click', async (event) => { const button = event.target.closest('[data-open-run]'); if (!button) return; navigate('results'); await switchPlugin(button.dataset.openPlugin, button.dataset.openRun); });
$('history-table').addEventListener('click', async (event) => { const row = event.target.closest('[data-open-run]'); if (!row) return; navigate('results'); await switchPlugin(row.dataset.openPlugin, row.dataset.openRun); });
$('result-type').addEventListener('change', () => switchPlugin($('result-type').value).catch((error) => addMessage('assistant', error.message)));
$('result-run').addEventListener('change', () => selectRun($('result-run').value, $('result-type').value).catch((error) => addMessage('assistant', error.message)));
$('provider').addEventListener('change', updateAiControls);
$('refresh').addEventListener('click', () => loadAll().catch(showLoadError));
$('ai-toggle').addEventListener('click', () => { $('console').dataset.aiOpen = String($('console').dataset.aiOpen !== 'true'); });
$('ai-close').addEventListener('click', () => { $('console').dataset.aiOpen = 'false'; });
$('ai-expand').addEventListener('click', toggleAiExpanded);
$('new-conversation').addEventListener('click', newAiConversation);
$('dsh-native-open').addEventListener('click', openDshWeb);
$('dsh-native-frame').addEventListener('load', () => { $('dsh-native-status').textContent = 'DSH 原生界面已连接'; });
$('dsh-native-frame').addEventListener('error', () => { $('dsh-native-status').textContent = 'DSH 界面加载失败，请重新连接。'; });
document.querySelectorAll('[data-ai-tab]').forEach((button) => button.addEventListener('click', () => selectAiTab(button.dataset.aiTab)));
$('chat-form').addEventListener('submit', async (event) => { event.preventDefault(); const message = $('question').value; $('question').value = ''; await askAi(message); });

let resizeStart = null;
$('ai-resize').addEventListener('pointerdown', (event) => {
  resizeStart = { x: event.clientX, width: state.panelWidth };
  document.body.classList.toggle('resizing-ai', true);
  event.preventDefault();
});
$('ai-resize').addEventListener('keydown', (event) => {
  if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
  setPanelWidth(state.panelWidth + (event.key === 'ArrowLeft' ? 24 : -24));
  event.preventDefault();
});
if (typeof window.addEventListener === 'function') {
  window.addEventListener('pointermove', (event) => { if (resizeStart) setPanelWidth(resizeStart.width + resizeStart.x - event.clientX, false); });
  window.addEventListener('pointerup', () => {
    if (!resizeStart) return;
    resizeStart = null;
    document.body.classList.toggle('resizing-ai', false);
    setPanelWidth(state.panelWidth);
  });
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
if (window.matchMedia('(max-width: 1280px)').matches) $('console').dataset.aiOpen = 'false';
navigate('results');
loadAll().catch(showLoadError);
