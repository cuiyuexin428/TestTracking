const state = { runs: [], selected: null, page: 'results', filter: '' };
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

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[char]));
}

function number(value) { return Number(value || 0).toLocaleString('zh-CN'); }
function statusLabel(value) { return ({ finished: '已完成', running: '运行中', planned: '已计划', stopped: '已停止' })[value] || value || '--'; }
function timeLabel(value) { return value ? new Date(value).toLocaleString('zh-CN', { hour12: false }) : '--'; }

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
  $('overview-count').textContent = `${runs.length} 条`;
  $('run-cards').innerHTML = runs.length ? runs.slice(0, 8).map((run) => `
    <article class="run-card">
      <div class="run-card-top"><div class="run-title"><span class="plugin-badge">${escapeHtml(run.plugin_type.slice(0, 1))}</span><div><h3>${escapeHtml(run.plugin_type)} <span class="chip ${run.status === 'finished' ? 'green' : 'blue'}">${escapeHtml(statusLabel(run.status))}</span></h3><div class="run-id">${escapeHtml(run.run_id)} · 数据集 ${escapeHtml(run.dataset_version || '--')}</div></div></div><button type="button" class="button" data-open-run="${escapeHtml(run.run_id)}" data-open-plugin="${escapeHtml(run.plugin_type)}">查看结果</button></div>
      <div class="run-meta"><span>预期文件 <b>${number(run.expected_files)}</b></span><span>成功 <b>${number(run.success_files)}</b></span><span>失败 <b>${number(run.failed_files)}</b></span><span>创建时间 <b>${escapeHtml(timeLabel(run.created_at))}</b></span></div>
    </article>`).join('') : '<div class="empty-panel"><span>◇</span><h2>暂无运行记录</h2><p>配置只读数据库连接后，这里会显示长期运行清单。</p></div>';
}

function renderHistory() {
  $('history-count').textContent = `${state.runs.length} 条`;
  $('history-table').innerHTML = state.runs.length ? state.runs.map((run) => `<tr data-open-run="${escapeHtml(run.run_id)}" data-open-plugin="${escapeHtml(run.plugin_type)}"><td>${escapeHtml(run.run_id)}</td><td>${escapeHtml(run.plugin_type)}</td><td><span class="chip ${run.status === 'finished' ? 'green' : 'blue'}">${escapeHtml(statusLabel(run.status))}</span></td><td>${number(run.success_files)} 成功 / ${number(run.failed_files)} 失败</td><td>${escapeHtml(timeLabel(run.finished_at))}</td></tr>`).join('') : '<tr><td colspan="5" class="empty-row">暂无运行记录</td></tr>';
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
  const completeness = run.result_completeness || {};
  const outcomes = run.outcomes || {};
  const analysis = run.analysis || {};
  const expected = Number(completeness.expected_files ?? run.expected_files ?? 0);
  const success = Number(completeness.successful_files ?? outcomes.success ?? 0);
  const failed = Number(completeness.failed_files ?? outcomes.failed ?? 0);
  const pending = Math.max(0, expected - success - failed);
  $('execution-status').textContent = statusLabel(run.status);
  $('completeness').innerHTML = `${number(success + failed)}<small> / ${number(expected)}</small>`;
  $('analysis-state').textContent = Object.keys(analysis).length ? '分析结果已发布' : (run.run_id ? '等待后处理结果' : '等待选择运行');
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
    const bars = [['覆盖行', analysis.covered_lines, analysis.total_lines], ['累计覆盖点', analysis.cumulative_points, analysis.cumulative_points], ['精简用例', analysis.selected_files, analysis.total_files]];
    $('coverage-bars').innerHTML = bars.map(([label, value, total]) => { const ratio = Number(total) > 0 ? Math.min(100, Number(value || 0) / Number(total) * 100) : 0; return `<div class="bar-row"><span>${label}</span><div class="bar-track"><i style="width:${ratio}%"></i></div><strong>${number(value)}</strong></div>`; }).join('');
    $('coverage-table').innerHTML = Object.keys(analysis).length ? [['总文件', analysis.total_files, '已结算'], ['精简用例', analysis.selected_files, '已生成'], ['累计覆盖点', analysis.cumulative_points, '已统计'], ['覆盖行 / 总行', `${number(analysis.covered_lines)} / ${number(analysis.total_lines)}`, '已统计']].map(([label, value, status]) => `<tr><td>${label}</td><td>${value ?? '--'}</td><td><span class="chip green">${status}</span></td></tr>`).join('') : '<tr><td colspan="3" class="empty-row">当前运行尚无 coverage 派生结果</td></tr>';
  } else {
    const different = Number(analysis.different || 0);
    $('analysis-label').textContent = '存在差异';
    $('analysis-value').textContent = Object.keys(analysis).length ? `${number(different)} 个文件` : '--';
    $('compare-table').innerHTML = Object.keys(analysis).length ? [['相同', analysis.same, 'green'], ['不同', analysis.different, 'amber'], ['不完整', analysis.incomplete, 'amber']].map(([label, value, color]) => `<tr><td>${label}</td><td>${number(value)}</td><td><span class="chip ${color}">${label}</span></td></tr>`).join('') : '<tr><td colspan="3" class="empty-row">当前运行尚无 stpcompare 分析结果</td></tr>';
  }
}

async function selectRun(runId, plugin) {
  if (!runId) { resetResult(plugin); return; }
  state.selected = await jsonFetch(`/api/runs/${encodeURIComponent(runId)}?plugin=${encodeURIComponent(plugin)}`);
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
  const [plugins, providers, health] = await Promise.all([jsonFetch('/api/plugins'), jsonFetch('/api/ai/providers'), jsonFetch('/healthz')]);
  state.runs = await jsonFetch('/api/runs?limit=100');
  $('health').textContent = health.status === 'ok' ? '结果服务正常' : health.status;
  $('footer-status').textContent = '结果服务运行中';
  $('health-dot').classList.remove('offline');
  $('provider').innerHTML = providers.map((provider) => `<option value="${escapeHtml(provider.name)}" ${provider.available ? '' : 'disabled'}>${escapeHtml(provider.name)}${provider.available ? '' : ' · 不可用'}</option>`).join('');
  renderPlugins(plugins); renderMetrics(); renderRunCards(); renderHistory();
  await switchPlugin($('result-type').value);
}

function addMessage(role, text) {
  const node = document.createElement('div');
  node.className = `message ${role === 'user' ? 'user-message' : 'assistant-message'}`;
  node.textContent = text;
  $('chat').appendChild(node);
  $('chat').scrollTop = $('chat').scrollHeight;
}

async function askAi(message) {
  if (!message.trim()) return;
  const provider = $('provider').value;
  addMessage('user', message);
  const waiting = document.createElement('div');
  waiting.className = 'message assistant-message'; waiting.textContent = `${provider} 正在分析...`; $('chat').appendChild(waiting);
  try {
    const result = await jsonFetch('/api/ai/chat', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ provider, message, context: { run: state.selected, result_type: $('result-type').value } }) });
    waiting.textContent = result.text;
  } catch (error) { waiting.textContent = `请求失败：${error.message}`; }
  $('chat').scrollTop = $('chat').scrollHeight;
}

document.querySelectorAll('.nav-item').forEach((button) => button.addEventListener('click', () => navigate(button.dataset.page)));
document.querySelectorAll('[data-prompt]').forEach((button) => button.addEventListener('click', () => askAi(button.dataset.prompt)));
$('overview-filter').addEventListener('click', (event) => { const button = event.target.closest('button'); if (!button) return; state.filter = button.dataset.filter; document.querySelectorAll('#overview-filter button').forEach((item) => item.setAttribute('aria-pressed', String(item === button))); renderRunCards(); });
$('run-cards').addEventListener('click', async (event) => { const button = event.target.closest('[data-open-run]'); if (!button) return; navigate('results'); await switchPlugin(button.dataset.openPlugin, button.dataset.openRun); });
$('history-table').addEventListener('click', async (event) => { const row = event.target.closest('[data-open-run]'); if (!row) return; navigate('results'); await switchPlugin(row.dataset.openPlugin, row.dataset.openRun); });
$('result-type').addEventListener('change', () => switchPlugin($('result-type').value).catch((error) => addMessage('assistant', error.message)));
$('result-run').addEventListener('change', () => selectRun($('result-run').value, $('result-type').value).catch((error) => addMessage('assistant', error.message)));
$('refresh').addEventListener('click', () => loadAll().catch(showLoadError));
$('ai-toggle').addEventListener('click', () => { $('console').dataset.aiOpen = String($('console').dataset.aiOpen !== 'true'); });
$('ai-close').addEventListener('click', () => { $('console').dataset.aiOpen = 'false'; });
$('chat-form').addEventListener('submit', async (event) => { event.preventDefault(); const message = $('question').value; $('question').value = ''; await askAi(message); });

function showLoadError(error) {
  $('health').textContent = '结果服务异常'; $('footer-status').textContent = error.message; $('health-dot').classList.add('offline'); addMessage('assistant', `加载失败：${error.message}`);
}

$('snapshot-time').textContent = new Date().toLocaleString('zh-CN', { hour12: false });
navigate('results');
loadAll().catch(showLoadError);
