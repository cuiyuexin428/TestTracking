const state = { runs: [], selected: null };
const $ = (id) => document.getElementById(id);

async function jsonFetch(url, options) {
  const response = await fetch(url, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

function renderStats() {
  const finished = state.runs.filter((run) => run.status === 'finished').length;
  const failed = state.runs.reduce((sum, run) => sum + Number(run.failed_files || 0), 0);
  $('stats').innerHTML = [
    ['运行数', state.runs.length], ['已完成', finished], ['失败文件', failed]
  ].map(([label, value]) => `<div class="stat"><span>${label}</span><strong>${value}</strong></div>`).join('');
}

function renderRuns() {
  $('run-count').textContent = `${state.runs.length} 条`;
  $('runs').innerHTML = state.runs.length ? state.runs.map((run) => `
    <tr data-run="${escapeHtml(run.run_id)}">
      <td>${escapeHtml(run.run_id)}</td><td>${escapeHtml(run.plugin_type)}</td>
      <td>${escapeHtml(run.status)}</td><td>${run.success_files || 0} 成功 / ${run.failed_files || 0} 失败</td>
      <td>${escapeHtml(String(run.created_at || ''))}</td>
    </tr>`).join('') : '<tr><td colspan="5" class="muted">暂无运行记录</td></tr>';
  document.querySelectorAll('#runs tr[data-run]').forEach((row) => row.addEventListener('click', () => selectRun(row.dataset.run)));
}

async function loadRuns() {
  const plugin = $('plugin').value;
  state.runs = await jsonFetch(`/api/runs?limit=100${plugin ? `&plugin=${encodeURIComponent(plugin)}` : ''}`);
  renderStats(); renderRuns();
}

async function selectRun(runId) {
  state.selected = await jsonFetch(`/api/runs/${encodeURIComponent(runId)}`);
  $('detail-panel').hidden = false; $('detail').textContent = JSON.stringify(state.selected, null, 2);
  addChat('assistant', `已选择运行 ${runId}，可以询问该运行的结果。`);
}

async function loadConfig() {
  const [plugins, providers, health] = await Promise.all([
    jsonFetch('/api/plugins'), jsonFetch('/api/ai/providers'), jsonFetch('/healthz')
  ]);
  $('plugin').innerHTML = '<option value="">全部</option>' + plugins.map((p) => `<option value="${p.name}">${p.name}</option>`).join('');
  $('provider').innerHTML = providers.map((p) => `<option value="${p.name}" ${p.available ? '' : 'disabled'}>${p.name}${p.available ? '' : ' (不可用)'}</option>`).join('');
  $('health').textContent = health.status === 'ok' ? '结果服务正常' : health.status;
  $('ai-status').classList.toggle('ready', providers.some((p) => p.available));
}

function addChat(role, text) { const node = document.createElement('div'); node.className = `bubble ${role}`; node.textContent = text; $('chat').appendChild(node); $('chat').scrollTop = $('chat').scrollHeight; }
function escapeHtml(value) { return String(value).replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[char])); }

$('refresh').addEventListener('click', () => loadRuns().catch((error) => addChat('assistant', error.message)));
$('plugin').addEventListener('change', () => loadRuns().catch((error) => addChat('assistant', error.message)));
$('close-detail').addEventListener('click', () => { $('detail-panel').hidden = true; state.selected = null; });
$('chat-form').addEventListener('submit', async (event) => {
  event.preventDefault(); const message = $('question').value.trim(); if (!message) return;
  const provider = $('provider').value; addChat('user', message); $('question').value = '';
  try { const result = await jsonFetch('/api/ai/chat', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ provider, message, context: { run: state.selected } }) }); addChat('assistant', result.text); }
  catch (error) { addChat('assistant', `请求失败：${error.message}`); }
});

loadConfig().then(loadRuns).catch((error) => { $('health').textContent = `服务不可用：${error.message}`; });

