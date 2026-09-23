'use strict';

// Reusable business choices live separately from the advanced deployment editor.
const DeploymentProfileUI = (() => {
  const clone = (value) => JSON.parse(JSON.stringify(value));
  const esc = (value) => String(value ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const roles = (plugin) => plugin === 'coverage'
    ? [['plugin', '客户端插件', 'plugin'], ['program', '业务程序', 'program'], ['collector', '覆盖率采集工具', 'program'], ['dataset', 'STP 数据', 'dataset']]
    : [['plugin', '客户端插件', 'plugin'], ['baseline', '老程序', 'program'], ['candidate', '新程序', 'program'], ['dataset', 'STP 数据', 'dataset']];
  const defaultPlugin = (name) => name === 'coverage'
    ? { resources: {}, dry_run: false, store_cov: false, engine_dlls: [] }
    : { resources: {}, dry_run: false, arguments: { baseline: ['--input', '{stp}', '--output-dir', '{outdir}'], candidate: ['--input', '{stp}', '--output-dir', '{outdir}'] } };
  const newProfile = () => ({ revision: null, name: '', description: '', plugins: { coverage: defaultPlugin('coverage') }, schedule: { task_threads: 1, execution_windows: [], update_windows: [] } });
  const allDays = [1, 2, 3, 4, 5, 6, 7];
  function normalizeSavedSchedule(draft) {
    draft.schedule = { task_threads: 1, execution_windows: [], update_windows: [], ...draft.schedule };
    for (const kind of ['execution_windows', 'update_windows']) for (const row of draft.schedule[kind]) {
      // The wire format historically treats omitted/empty days as every day.
      if (!row.days?.length) row.days = [...allDays];
    }
  }
  function entryChoices(resource, role) {
    if (!resource) return [];
    if (role === 'dataset') return [...new Set(resource.dataset_roots || [])];
    return [...new Set(role === 'plugin' ? resource.dlls || [] : resource.executables || [])];
  }
  function autoEntry(resource, role, plugin) {
    const candidates = entryChoices(resource, role);
    if (role === 'dataset') return (resource?.dataset_roots || []).length === 1 ? resource.dataset_roots[0] : '';
    if (candidates.length === 1) return candidates[0];
    const preferred = candidates.filter((path) => {
      const file = path.replace(/\\/g, '/').split('/').pop().toLowerCase();
      return role === 'plugin' ? new RegExp(`^(?:lib)?${plugin}(?:[_-](?:client|plugin))?\\.dll$`).test(file)
        : role === 'collector' && file === 'opencppcoverage.exe';
    });
    return preferred.length === 1 ? preferred[0] : '';
  }
  function validateProfile(profile, inventories) {
    if (!profile.name.trim()) throw new Error('请填写方案名称。');
    const enabled = Object.keys(profile.plugins);
    if (!enabled.length) throw new Error('请至少选择一个插件。');
    for (const name of enabled) {
      if (!['coverage', 'stpcompare'].includes(name)) continue;
      for (const [role, label] of roles(name)) {
        const resource = profile.plugins[name].resources?.[role];
        if (!resource?.artifact_id) throw new Error(`${name}：请选择${label}。`);
        const files = inventories[resource.artifact_id];
        if (!files) throw new Error(`${name}：${label}的内容尚未读取，请重新选择或稍后保存。`);
        const entry = role === 'dataset' ? resource.root ?? '' : resource.entry;
        if (!entryChoices(files, role).includes(entry)) throw new Error(`${name}：请在“资源入口与运行设置”中选择${label}入口。`);
      }
      if (name === 'coverage') {
        const available = inventories[profile.plugins[name].resources.program.artifact_id].dlls || [];
        if ((profile.plugins[name].engine_dlls || []).some((path) => !available.includes(path))) throw new Error('coverage：请从当前业务程序的模块列表选择采集模块。');
      } else if (name === 'stpcompare') {
        for (const role of ['baseline', 'candidate']) {
          const args = profile.plugins[name].arguments?.[role] || defaultPlugin(name).arguments[role];
          if (!args.length || !['{stp}', '{outdir}'].every((token) => args.some((arg) => arg.includes(token)))) throw new Error(`stpcompare：${role === 'baseline' ? '老程序' : '新程序'}参数需包含 {stp} 和 {outdir}。`);
        }
      }
    }
    if (!Number.isInteger(profile.schedule.task_threads) || profile.schedule.task_threads < 1 || profile.schedule.task_threads > 1024) throw new Error('每插件任务并发数必须是 1 到 1024 的整数。');
    for (const kind of ['execution_windows', 'update_windows']) for (const row of profile.schedule[kind] || []) {
      if (!Array.isArray(row.days) || !row.days.length) throw new Error('每个时间窗请至少选择一天；不需要此时段时可移除。');
      if (!/^([01]\d|2[0-3]):[0-5]\d$/.test(row.start) || !/^([01]\d|2[0-3]):[0-5]\d$/.test(row.end) || row.days?.some((day) => !Number.isInteger(day) || day < 1 || day > 7)) throw new Error('请补全时间窗的日期和起止时间。');
    }
    return profile;
  }
  function summary(profile, artifacts) {
    const byId = new Map(artifacts.map((item) => [item.artifact_id, item]));
    return Object.entries(profile.plugins || {}).map(([name, config]) => ({ name, resources: roles(name).map(([role, label]) => {
      const artifact = byId.get(config.resources?.[role]?.artifact_id);
      return { label, value: artifact ? `${artifact.name} · ${artifact.version}` : config.resources?.[role]?.artifact_id ? '已选择资源' : '未选择' };
    }) }));
  }

  function createController(bridge) {
    const doc = bridge.document, $ = (id) => doc.getElementById(id), request = bridge.request;
    const model = { profiles: [], artifacts: [], inventories: {}, draft: null, drafts: {}, dirtyProfiles: new Set(), pluginDrafts: {}, generation: 0, previewGeneration: 0, loading: null, editorBusy: false, resourcePending: 0, targets: [], selected: '', mode: 'update', preview: null, requestId: '', applyBusy: false, conflictDraft: null, rollouts: [], rolloutTargets: {}, rolloutLoading: null, rolloutBusy: '', rolloutError: '', rolloutGeneration: 0, replacement: null, aiReview: null, aiReviewBusy: false, aiReviewRequest: 0 };
    const post = (url, body, method = 'POST') => request(url, { method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    const message = (id, text) => { $(id).textContent = text; };
    const profile = () => model.profiles.find((item) => item.profile_id === model.selected);
    function draftChanged() {
      if (!model.draft?.profile_id) return;
      model.dirtyProfiles.add(model.draft.profile_id);
      if (model.selected === model.draft.profile_id) invalidate('此方案有未保存修改，请先保存方案，再重新预览。');
    }
    function summaryHtml(record) {
      return summary(record, model.artifacts).map((plugin) => `<section class="profile-summary-plugin"><strong>${esc(plugin.name)}</strong><dl>${plugin.resources.map((row) => `<dt>${esc(row.label)}</dt><dd>${esc(row.value)}</dd>`).join('')}</dl></section>`).join('');
    }
    function resourceSummaryHtml(resources) {
      return (resources || []).map((plugin) => `<section class="profile-summary-plugin"><strong>${esc(plugin.plugin)}</strong><dl>${(plugin.resources || []).map((resource) => `<dt>${esc(roles(plugin.plugin).find(([role]) => role === resource.label)?.[1] || resource.label)}</dt><dd>${esc(resource.name)}${resource.version ? ` · ${esc(resource.version)}` : ''}</dd>`).join('')}</dl></section>`).join('');
    }
    function renderList() {
      $('profiles-list').innerHTML = model.profiles.length ? model.profiles.map((item) => `<article class="workspace-surface profile-card"><div class="panel-head"><h3>${esc(item.name)}</h3><button type="button" class="button" data-profile-edit="${esc(item.profile_id)}">编辑方案</button></div>${item.description ? `<p class="workspace-help">${esc(item.description)}</p>` : ''}${summaryHtml(item)}<p class="config-help">发布到所有已启用这些插件的机器；保留每台机器的并发和时间窗。</p><div class="profile-card-actions"><small class="workspace-help">${item.updated_at ? `更新于 ${esc(new Date(item.updated_at).toLocaleString('zh-CN'))}` : '资源组合可重复使用'}</small><button type="button" class="button primary" data-profile-update="${esc(item.profile_id)}" ${model.applyBusy ? 'disabled' : ''}>发布更新</button></div></article>`).join('') : '<div class="empty-panel"><h3>先创建一个部署方案</h3><p>选择插件、程序与数据版本，保存草稿后统一发布。</p></div>';
      $('profile-apply-select').innerHTML = '<option value="">选择部署方案</option>' + model.profiles.map((item) => `<option value="${esc(item.profile_id)}">${esc(item.name)}</option>`).join('');
      $('profile-apply-select').value = model.selected;
    }
    async function load() {
      if (model.loading) return model.loading;
      model.loading = (async () => {
        try {
          const [profiles, artifacts] = await Promise.all([request('/api/deployment-profiles'), request('/api/artifacts')]);
          const before = profile()?.revision;
          model.profiles = profiles.profiles || []; model.artifacts = artifacts.artifacts || [];
          if (model.preview && before !== profile()?.revision) invalidate('方案已更新，请重新预览。');
          renderList(); if (model.draft) renderEditor(); message('profiles-status', '');
        } catch (error) { message('profiles-status', `方案读取失败：${error.message}。已有草稿保留。`); throw error; }
        finally { model.loading = null; }
      })();
      return model.loading;
    }
    function renderEditor() {
      const draft = model.draft;
      $('profile-editor').hidden = false;
      $('profile-editor-title').textContent = draft.profile_id ? '编辑部署方案' : '创建部署方案';
      $('profile-name').value = draft.name; $('profile-description').value = draft.description || '';
      $('profile-threads').value = draft.schedule.task_threads;
      $('profile-replacement-notice').hidden = model.replacement?.profile_id !== draft.profile_id;
      doc.querySelectorAll('[data-profile-plugin]').forEach((input) => { input.checked = Boolean(draft.plugins[input.dataset.profilePlugin]); });
      $('profile-resources').innerHTML = Object.keys(draft.plugins).filter((name) => ['coverage', 'stpcompare'].includes(name)).map((name) => `<section class="profile-resource-group"><h3>${esc(name)}</h3><div class="profile-fields">${roles(name).map(([role, label, kind]) => {
        const selected = draft.plugins[name].resources?.[role]?.artifact_id || '';
        return `<div class="management-field"><label for="profile-resource-${name}-${role}">${esc(label)}</label><select id="profile-resource-${name}-${role}" data-profile-resource="${name}" data-role="${role}"><option value="">选择已上传的版本</option>${model.artifacts.filter((item) => item.artifact_id === selected || item.kind === kind && !item.metadata?.managed).map((item) => `<option value="${esc(item.artifact_id)}" ${item.artifact_id === selected ? 'selected' : ''}>${esc(item.name)} · ${esc(item.version)}</option>`).join('')}</select></div>`;
      }).join('')}</div></section>`).join('');
      renderEntries(); renderWindows(); editorControls();
    }
    function renderEntries() {
      $('profile-resource-entries').innerHTML = Object.entries(model.draft.plugins).filter(([name]) => ['coverage', 'stpcompare'].includes(name)).map(([name, config]) => `<section class="profile-resource-group"><h3>${esc(name)}</h3><div class="profile-fields">${roles(name).map(([role, label]) => {
        const value = config.resources?.[role], inventory = model.inventories[value?.artifact_id];
        const choices = entryChoices(inventory, role); const selected = role === 'dataset' ? value?.root ?? '' : value?.entry || '';
        return `<div class="management-field"><label for="profile-entry-${name}-${role}">${esc(label)}入口</label><select id="profile-entry-${name}-${role}" data-profile-entry="${name}" data-role="${role}" ${!inventory ? 'disabled' : ''}>${role !== 'dataset' ? '<option value="">请选择入口</option>' : '<option value="__choose__">请选择数据目录</option>'}${choices.map((path) => `<option value="${esc(path)}" ${selected === path ? 'selected' : ''}>${esc(path || '数据包根目录')}</option>`).join('')}</select></div>`;
      }).join('')}${programSettings(name, config)}<div class="management-field"><label for="profile-mode-${name}">执行模式</label><select id="profile-mode-${name}" data-profile-mode="${name}"><option value="real" ${!config.dry_run ? 'selected' : ''}>真实执行</option><option value="simulate" ${config.dry_run ? 'selected' : ''}>模拟执行</option></select></div>${name === 'coverage' ? `<label class="check-field"><input type="checkbox" data-profile-cov ${config.store_cov ? 'checked' : ''}>保留原始 cov 文件</label>` : ''}</div></section>`).join('');
      for (const [name, config] of Object.entries(model.draft.plugins)) {
        const unresolved = Object.entries(config.resources || {}).find(([role, resource]) => {
          const inventory = model.inventories[resource.artifact_id];
          return inventory && !entryChoices(inventory, role).includes(resource[role === 'dataset' ? 'root' : 'entry']);
        });
        if (unresolved) { revealEntry(name, unresolved[0]); break; }
      }
    }
    function programSettings(name, config) {
      if (name === 'coverage') {
        const modules = model.inventories[config.resources?.program?.artifact_id]?.dlls || [];
        return `<div class="management-field"><label for="profile-engine-dlls">采集模块（不选表示全部）</label><select id="profile-engine-dlls" data-profile-engine-dlls multiple size="${Math.max(2, Math.min(5, modules.length))}" ${!modules.length ? 'disabled' : ''}>${modules.map((path) => `<option value="${esc(path)}" ${(config.engine_dlls || []).includes(path) ? 'selected' : ''}>${esc(path)}</option>`).join('') || '<option disabled>所选业务程序包没有 DLL 模块</option>'}</select><small>可多选业务程序包中的 DLL；留空采集全部模块。</small></div>`;
      }
      return ['baseline', 'candidate'].map((role) => `<div class="management-field"><label for="profile-arguments-${role}">${role === 'baseline' ? '老程序' : '新程序'}参数（每行一个）</label><textarea id="profile-arguments-${role}" data-profile-arguments="${role}" rows="4" spellcheck="false">${esc((config.arguments?.[role] || defaultPlugin('stpcompare').arguments[role]).join('\n'))}</textarea><small>保留 {stp} 和 {outdir} 占位符；每行是一个完整参数，无需额外加引号。</small></div>`).join('');
    }
    function revealEntry(name, role) {
      $('profile-settings').open = true;
      const entry = $(`profile-entry-${name}-${role}`);
      entry?.focus?.(); entry?.scrollIntoView?.({ block: 'nearest' });
    }
    function renderWindows() {
      const days = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'];
      $('profile-windows').innerHTML = [['execution_windows', '任务执行时间窗'], ['update_windows', '部署更新时间窗']].map(([kind, label]) => `<section class="deployment-window"><h3>${label}</h3>${(model.draft.schedule[kind] || []).map((row, index) => `<div class="profile-window-row"><div class="profile-days">${days.map((day, n) => `<label><input type="checkbox" data-profile-window="${kind}" data-index="${index}" data-day="${n + 1}" ${(row.days || [1, 2, 3, 4, 5, 6, 7]).includes(n + 1) ? 'checked' : ''}>${day}</label>`).join('')}</div><div class="head-actions"><input type="time" value="${esc(row.start)}" aria-label="${label}开始时间" data-profile-window="${kind}" data-index="${index}" data-time="start"><span>至</span><input type="time" value="${esc(row.end)}" aria-label="${label}结束时间" data-profile-window="${kind}" data-index="${index}" data-time="end"><button type="button" class="button compact" data-profile-remove-window="${kind}" data-index="${index}">移除</button></div></div>`).join('') || '<p class="workspace-help">全天</p>'}<button type="button" class="button" data-profile-add-window="${kind}">＋ 添加时段</button></section>`).join('');
    }
    function editorControls() {
      $('profile-form').querySelectorAll('input, select, textarea, button').forEach((element) => { element.disabled = model.editorBusy; });
      $('profile-save').disabled = model.editorBusy || model.resourcePending > 0;
    }
    async function inventoryFor(name, role, preserve = false) {
      const generation = model.generation, value = model.draft.plugins[name]?.resources?.[role];
      if (!value?.artifact_id) return;
      const id = value.artifact_id; model.resourcePending++; editorControls();
      try {
        const inventory = model.inventories[id] || await request(`/api/artifacts/${encodeURIComponent(id)}/resources`);
        model.inventories[id] = inventory;
        if (generation !== model.generation || model.draft.plugins[name]?.resources?.[role]?.artifact_id !== id) return;
        const key = role === 'dataset' ? 'root' : 'entry';
        if (!preserve || value[key] === undefined) value[key] = autoEntry(inventory, role, name);
        if (!preserve && name === 'coverage' && role === 'program') {
          const config = model.draft.plugins.coverage;
          config.engine_dlls = (config.engine_dlls || []).filter((path) => (inventory.dlls || []).includes(path));
        }
        renderEntries();
        if (!entryChoices(inventory, role).includes(value[key])) {
          message('profile-editor-status', `${name} 的${roles(name).find((row) => row[0] === role)[1]}${entryChoices(inventory, role).length ? '有多个入口，请在已展开的入口设置中选择。' : '没有可用入口，请重新选择资源包。'}`);
          revealEntry(name, role);
        }
      } catch (error) { if (generation === model.generation) message('profile-editor-status', `资源读取失败：${error.message}。请重新选择资源后保存。`); }
      finally { model.resourcePending--; editorControls(); }
    }
    async function edit(id) {
      if (model.editorBusy) return;
      if (model.replacement && model.replacement.profile_id !== id) model.replacement = null;
      if (model.draft && (model.draft.profile_id || '') === (id || '')) { $('profile-editor').hidden = false; return; }
      if (model.draft) model.drafts[model.draft.profile_id || '__new__'] = clone(model.draft);
      const source = model.drafts[id || '__new__'] || (id ? model.profiles.find((item) => item.profile_id === id) : newProfile());
      if (!source) return;
      model.draft = clone(source);
      if (!model.drafts[id || '__new__']) normalizeSavedSchedule(model.draft);
      model.pluginDrafts = {}; model.generation++;
      $('profile-settings').open = false;
      renderEditor(); message('profile-editor-status', '方案保存后可反复应用到客户端。');
      $('profile-load-latest').hidden = true; $('profile-conflict-copy').hidden = true;
      await Promise.all(Object.entries(model.draft.plugins).flatMap(([name, config]) => Object.keys(config.resources || {}).map((role) => inventoryFor(name, role, true))));
      if (!$('profile-settings').open) $('profile-name').focus?.();
    }
    async function save() {
      if (!model.draft || model.editorBusy || model.resourcePending) return false;
      syncVisibleSchedule();
      try { validateProfile(model.draft, model.inventories); }
      catch (error) { message('profile-editor-status', error.message); return false; }
      const draft = clone(model.draft); model.editorBusy = true; editorControls();
      try {
        const saved = await post(draft.profile_id ? `/api/deployment-profiles/${encodeURIComponent(draft.profile_id)}` : '/api/deployment-profiles', draft, draft.profile_id ? 'PUT' : 'POST');
        const record = saved.profile || saved;
        delete model.drafts[draft.profile_id || '__new__'];
        model.dirtyProfiles.delete(record.profile_id);
        model.draft = clone(record); model.profiles = [...model.profiles.filter((item) => item.profile_id !== record.profile_id), record];
        if (model.selected === record.profile_id) invalidate('方案已保存，请重新预览。');
        renderList(); message('profile-editor-status', '草稿已保存，尚未更新客户端。点击“发布更新”核对范围后提交。');
        $('profile-load-latest').hidden = true; $('profile-conflict-copy').hidden = true;
        return true;
      } catch (error) {
        message('profile-editor-status', error.status === 409 ? '另一位使用者已更新此方案。当前草稿与原版本已保留，请载入最新版本后合并再保存。' : `保存失败：${error.message}。草稿已保留。`);
        if (error.status === 409) { model.conflictDraft = clone(model.draft); $('profile-load-latest').hidden = false; $('profile-conflict-copy').hidden = false; }
        return false;
      } finally { model.editorBusy = false; editorControls(); }
    }
    function invalidate(text = '') {
      model.previewGeneration++;
      model.aiReviewRequest++;
      model.requestId = '';
      model.preview = null; model.aiReview = null; model.aiReviewBusy = false; $('profile-apply-confirm').checked = false;
      $('profile-apply-summary').innerHTML = profile() ? summaryHtml(profile()) : '';
      if ($('profile-apply-ai-status')) $('profile-apply-ai-status').textContent = '';
      bridge.clearDshDeploymentContext?.();
      if (text) message('profile-apply-status', text);
      applyControls();
    }
    function applyControls() {
      const blocked = model.applyBusy, review = model.preview;
      for (const id of ['profile-apply-select', 'profile-apply-preview', 'profile-apply-manage', 'profile-apply-close', 'profile-apply-confirm']) $(id).disabled = blocked;
      $('profile-apply-select').disabled = blocked || model.mode === 'update';
      $('profile-apply-preview').disabled = blocked || !profile() || model.mode === 'assign' && !model.targets.length || model.dirtyProfiles.has(model.selected);
      $('profile-apply-bind').hidden = true;
      $('profile-apply-bind').disabled = blocked;
      const ready = Boolean(review?.plan_fingerprint && !model.dirtyProfiles.has(model.selected));
      $('profile-apply-confirm-row').hidden = !ready; $('profile-apply-publish').hidden = !ready;
      $('profile-apply-publish').disabled = blocked || !ready || Boolean(review?.publish_blocked);
      $('profile-apply-ai-review').hidden = !ready;
      $('profile-apply-ai-review').disabled = blocked || model.aiReviewBusy || !ready;
      $('profile-apply-ai-review').textContent = model.aiReviewBusy ? '正在准备 DSH 提示…' : model.aiReview ? '重新让 DSH 分析' : '让 DSH 分析本次发布';
      $('profile-apply-publish').textContent = model.mode === 'assign' ? '分配并部署' : '确认发布';
      $('profile-apply-confirm-row').hidden = true;
    }
    function applyBusy(value) { model.applyBusy = value; bridge.setBusy?.(value); applyControls(); }
    const replacementFields = () => model.replacement?.profile_id === model.selected ? { replaces_rollout_id: model.replacement.rollout_id, replaces_revision: model.replacement.revision } : {};
    const selectionKey = () => JSON.stringify({ mode: model.mode, targets: model.mode === 'assign' ? model.targets : [], id: model.selected, revision: profile()?.revision, ...replacementFields() });
    function selectionChanged(ids) {
      if (model.mode !== 'assign' || $('profile-apply').hidden || model.applyBusy || JSON.stringify(ids) === JSON.stringify(model.targets)) return;
      model.targets = [...ids]; renderTargets(); invalidate('目标客户端已变化，请重新预览。');
    }
    function renderTargets() { $('profile-apply-targets').innerHTML = model.mode === 'update' ? '<strong>统一更新已启用方案插件的全部机器</strong><p class="config-help">范围由服务端核对，包括离线机器；不会自动启用其他机器上的插件。</p>' : `<strong>为 ${model.targets.length} 台机器分配方案</strong><div>${model.targets.map((id) => `<span class="chip">${esc(id)}</span>`).join('')}</div>`; }
    let applyReturnFocus = null;
    function rememberApplyFocus() { const element = doc.activeElement; applyReturnFocus = { element, id: element?.id, profileUpdate: element?.dataset?.profileUpdate }; }
    function showApply() {
      const dialog = $('profile-apply'); dialog.hidden = false;
      if (!dialog.open) { if (dialog.showModal) dialog.showModal(); else dialog.open = true; }
      $('profile-apply-title').focus?.({ preventScroll: true });
    }
    function closeApply(restoreFocus = true) {
      const dialog = $('profile-apply'); if (dialog.open && dialog.close) dialog.close(); dialog.open = false; dialog.hidden = true;
      if (restoreFocus) {
        const previous = applyReturnFocus;
        const target = previous?.element?.isConnected ? previous.element : previous?.id ? $(previous.id) : [...doc.querySelectorAll('[data-profile-update]')].find((button) => button.dataset.profileUpdate === previous?.profileUpdate);
        target?.focus?.({ preventScroll: true });
      }
    }
    async function openApply(ids) {
      if (model.applyBusy) return;
      rememberApplyFocus();
      model.mode = 'assign';
      model.replacement = null;
      $('profile-apply-title').textContent = '为机器分配方案';
      model.targets = [...new Set(ids)]; bridge.setSelected?.(model.targets);
      bridge.navigate('clients'); $('deployment-editor').hidden = true;
      renderTargets(); invalidate(); showApply();
      try {
        await load();
        if (!model.selected && model.profiles.length === 1) model.selected = model.profiles[0].profile_id;
        renderList(); invalidate();
        message('profile-apply-status', model.dirtyProfiles.has(model.selected) ? '此方案有未保存修改，请先保存方案，再重新预览。' : model.profiles.length ? '选择方案，机器将自动跟随该插件的统一发布版本。' : '还没有部署方案，请先点击“维护方案”创建。');
        if (profile() && !model.dirtyProfiles.has(model.selected)) await preview();
      } catch (error) { message('profile-apply-status', `无法读取方案：${error.message}`); }
    }
    async function preview() {
      if (model.applyBusy || !profile() || model.mode === 'assign' && !model.targets.length || model.dirtyProfiles.has(model.selected)) return false;
      const selected = clone(profile()), key = selectionKey(); applyBusy(true); invalidate();
      const generation = model.previewGeneration;
      message('profile-apply-status', '正在核对方案与目标客户端…');
      try {
        const result = await post(`/api/deployment-profiles/${encodeURIComponent(selected.profile_id)}/preview`, { revision: selected.revision, mode: model.mode, ...replacementFields(), ...(model.mode === 'assign' ? { client_ids: model.targets } : {}) });
        if (generation !== model.previewGeneration || model.dirtyProfiles.has(model.selected) || key !== selectionKey()) return false;
        if (!result.plan_fingerprint) throw new Error('服务端未返回完整预览标识，请刷新后重试');
        model.preview = result; model.aiReview = null;
        model.requestId = bridge.uuid?.() || globalThis.crypto?.randomUUID?.() || 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (character) => { const value = Math.floor(Math.random() * 16); return (character === 'x' ? value : value & 3 | 8).toString(16); });
        const clients = result.clients || [], offline = clients.filter((client) => client.online === false).length;
        $('profile-apply-summary').innerHTML = `<h3>${esc(result.profile_name || selected.name)}</h3>` + (result.resource_summary ? resourceSummaryHtml(result.resource_summary) : summaryHtml(selected)) + `<div class="profile-review-notice"><strong>${clients.length} 台机器 · ${offline} 台离线待补齐</strong><p>${model.mode === 'assign' ? '统一目标可运行后可为新机器分配方案；已有离线机器会在上线后自动补齐。' : '当前批次与提交前已排队批次先按旧版完成，再自动同步服务端并更新客户端。'}</p><p>已有机器的并发数、执行时间窗、更新时间窗及方案外插件配置保持原值；首次配置使用方案默认安排。离线机器上线后补更新，不阻挡已就绪机器执行。</p>${result.published_target ? '<p>分配当前已发布版本；方案草稿中尚未发布的变更不参与本次分配。</p>' : ''}${model.replacement ? '<p>这是失败更新的修复发布，将接替原请求继续推进；不会临时放开旧版本执行。</p>' : ''}${(result.warnings || []).map((warning) => `<p>${esc(warning)}</p>`).join('')}</div>${waitingRunsHtml(result.waiting_runs || [])}<details class="rollout-client-details"><summary>查看全部 ${clients.length} 台目标机器</summary>${clients.map((client) => `<p>${esc(client.client_id)} · ${client.online === false ? '离线，上线后补齐' : client.busy ? '正在执行，等待旧批次完成' : '在线'}</p>`).join('')}</details>`;
        message('profile-apply-status', result.publish_blocked ? `暂不能发布：${(result.unsupported_clients || []).join('、') || '部分目标机器'}需要先升级 Agent，以支持统一更新。升级后请重新预览。` : '请核对目标资源与影响范围。确认后请求会持久保存，关闭页面不影响推进。');
        return true;
      } catch (error) { invalidate(); message('profile-apply-status', `预览失败：${error.message}。请刷新方案或客户端状态后重试。`); return false; }
      finally { applyBusy(false); }
    }
    async function reviewWithDsh() {
      if (model.applyBusy || model.aiReviewBusy || model.dirtyProfiles.has(model.selected) || !model.preview?.plan_fingerprint) return false;
      const selected = clone(profile()), review = model.preview, generation = model.previewGeneration;
      const requestId = ++model.aiReviewRequest;
      model.aiReviewBusy = true; applyControls();
      if ($('profile-apply-ai-status')) $('profile-apply-ai-status').textContent = '正在读取本次预览的脱敏审查上下文…';
      try {
        const result = await post('/api/deployment-ai/review-context', {
          profile_id: selected.profile_id,
          revision: selected.revision,
          mode: model.mode,
          ...replacementFields(),
          ...(model.mode === 'assign' ? { client_ids: model.targets } : {}),
          plan_fingerprint: review.plan_fingerprint,
          operator_id: bridge.operatorId,
        });
        if (requestId !== model.aiReviewRequest || generation !== model.previewGeneration || model.preview?.plan_fingerprint !== review.plan_fingerprint) return false;
        if (!result || typeof result.context !== 'object' || result.plan_fingerprint !== review.plan_fingerprint) {
          throw new Error('服务端返回的 AI 审查上下文与当前预览不一致');
        }
        model.aiReview = result;
        // A modal dialog makes its siblings inert. Keep the valid preview in
        // memory, then reveal DSH with an explicit route back to this dialog.
        closeApply(false);
        bridge.prepareDshDeploymentContext?.({
          purpose: 'deployment-review',
          profile_name: result.profile_name || review.profile_name || selected.name,
          plan_fingerprint: review.plan_fingerprint,
          context_fingerprint: result.context_fingerprint,
          context: result.context,
        }, () => {
          if (generation !== model.previewGeneration || model.preview?.plan_fingerprint !== review.plan_fingerprint) return false;
          showApply(); $('profile-apply-ai-review').focus?.({ preventScroll: true }); return true;
        });
        if ($('profile-apply-ai-status')) $('profile-apply-ai-status').textContent = '已准备 DSH 审查提示。AI 只提供建议；发布仍需在此确认。';
        return true;
      } catch (error) {
        if (requestId !== model.aiReviewRequest) return false;
        if (error.status === 409) invalidate('部署预览已变化，请重新核对范围后再发布或交给 DSH。');
        if ($('profile-apply-ai-status')) $('profile-apply-ai-status').textContent = `AI 审查准备失败：${error.message}`;
        return false;
      } finally { if (requestId === model.aiReviewRequest) { model.aiReviewBusy = false; applyControls(); } }
    }
    async function publish() {
      if (model.applyBusy || model.dirtyProfiles.has(model.selected) || !model.preview?.plan_fingerprint || model.preview.publish_blocked || !model.requestId) return false;
      const selected = clone(profile()), review = model.preview; applyBusy(true);
      try {
        const result = await post(`/api/deployment-profiles/${encodeURIComponent(selected.profile_id)}/publish`, { revision: selected.revision, mode: model.mode, ...replacementFields(), client_ids: model.mode === 'assign' ? model.targets : [], plan_fingerprint: review.plan_fingerprint, request_id: model.requestId, operator_id: bridge.operatorId, reason: `${model.mode === 'assign' ? '分配方案' : '统一发布更新'}：${review.profile_name || selected.name}` });
        rememberRollout(result); model.replacement = null; invalidate(); closeApply(false);
        message('profile-apply-status', '更新已提交，进度会自动保存。'); openRollout(result.rollout_id);
        await bridge.refresh?.(); return true;
      } catch (error) {
        if (error.status >= 400 && error.status < 500) invalidate();
        message('profile-apply-status', error.status >= 400 && error.status < 500 ? `提交失败：${error.message}。方案保留，请重新预览。` : `尚未确认提交结果：${error.message}。可再次确认，使用同一请求标识避免重复提交。`);
        loadRollouts().catch(() => {}); return false;
      }
      finally { applyBusy(false); }
    }
    async function openUpdate(id, saveDraft = false) {
      if (model.applyBusy || model.editorBusy) return false;
      rememberApplyFocus();
      if (saveDraft && !(await save())) return false;
      model.selected = id || model.draft?.profile_id || '';
      if (model.replacement?.profile_id === model.selected) {
        const latest = model.rollouts.find((record) => record.rollout_id === model.replacement.rollout_id);
        if (latest) model.replacement.revision = latest.revision;
      }
      model.mode = 'update'; model.targets = [];
      $('deployment-editor').hidden = true;
      renderList(); renderTargets(); invalidate();
      $('profile-apply-title').textContent = '确认发布更新';
      $('profile-apply-select').disabled = true;
      showApply();
      if (model.dirtyProfiles.has(model.selected)) { message('profile-apply-status', '此方案有未保存修改，请先保存草稿再发布。'); return false; }
      return preview();
    }
    const runStatus = (status) => ({ running: '运行中', queued: '已排队', planning: '规划中', planned: '已计划', stopped: '已停止，等待处理', finished: '已完成', cancelled: '已放弃', result_pending: '等待任务结果确认', client_busy: '等待机器任务与结果确认' })[status] || status;
    function waitingRunsHtml(runs, rollout = null) {
      if (!runs.length) return '<p class="config-help">没有需要先完成的旧批次。</p>';
      const grouped = new Map(), machines = new Map();
      for (const run of runs) {
        if (!run.run_id) { const key = run.client_id || run.worker_id || '客户端'; if (!machines.has(key)) machines.set(key, run); continue; }
        const key = run.plugin_type + '/' + run.run_id;
        if (!grouped.has(key)) grouped.set(key, { ...run, statuses: [], pendingTasks: new Set() });
        const group = grouped.get(key); group.statuses.push(run.status);
        if (run.status === 'result_pending') group.pendingTasks.add(run.task_id || group.pendingTasks.size);
      }
      const rows = [...grouped.values()].map((run) => {
        const status = ['stopped', 'running', 'queued', 'planning', 'planned'].find((value) => run.statuses.includes(value)) || run.status;
        const resultWait = run.pendingTasks.size ? ' · ' + run.pendingTasks.size + ' 个任务结果待确认' : '';
        return '<div class="rollout-run" data-run-row="' + esc(run.run_id) + '"><div><strong>' + esc(run.plugin_type) + ' · ' + esc(run.run_id) + '</strong><span>' + esc(runStatus(status)) + resultWait + (Number.isFinite(run.total_tasks) ? ' · ' + Number(run.completed_tasks || 0) + ' / ' + run.total_tasks + ' 个任务' : '') + '</span></div><div class="head-actions"><button type="button" class="button compact" data-rollout-view-run="' + esc(run.run_id) + '" data-plugin="' + esc(run.plugin_type) + '">查看批次</button>' + (rollout && status === 'stopped' ? '<button type="button" class="button compact" data-rollout-action="resume" data-rollout-id="' + esc(rollout.rollout_id) + '" data-run-id="' + esc(run.run_id) + '" data-plugin="' + esc(run.plugin_type) + '" ' + (model.rolloutBusy ? 'disabled' : '') + '>恢复旧批次</button><button type="button" class="button compact" data-rollout-abandon="' + esc(run.run_id) + '" data-rollout-id="' + esc(rollout.rollout_id) + '" data-plugin="' + esc(run.plugin_type) + '">处理剩余任务</button>' : '') + '</div></div>';
      }).join('');
      return '<section class="rollout-runs"><h4>先完成的旧版本批次</h4>' + rows + [...machines].map(([client]) => '<p class="config-help">' + esc(client) + ' · 等待机器任务与结果确认后再更新。</p>').join('') + '</section>';
    }
    const rolloutLabels = { pending: '更新已提交', waiting_runs: '等待旧版本批次完成', switching: '正在准备并同步服务端', deploying: '正在更新客户端', ready: '新版可运行，部分机器待补齐', completed: '全部目标机器已就绪', cancelled: '更新已取消', blocked: '更新需要处理', superseded: '已由修复发布接替' };
    const clientLabels = { ready: '已就绪', offline: '离线待补齐', waiting_window: '等待更新时间窗', waiting_idle: '等待机器任务与结果确认', downloading: '正在下载', applying: '正在切换', failed: '更新失败', pending: '等待更新', superseded: '已由后续目标接替' };
    function rolloutHtml(rollout) {
      const clients = rollout.clients || [], ready = clients.filter((client) => client.status === 'ready').length;
      const phase = rollout.phase || rollout.status;
      const stage = ['cancelled', 'superseded'].includes(rollout.status) ? 3 : ['pending', 'waiting_runs', 'blocked'].includes(phase) ? 0 : phase === 'switching' ? 1 : phase === 'deploying' ? 2 : 3;
      const waiting = rollout.waiting_runs || [], stopped = waiting.some((run) => run.status === 'stopped');
      const plugins = rollout.plugins || [];
      const pendingSuccessor = model.rollouts.some((item) => item.rollout_id !== rollout.rollout_id && item.mode !== 'assign' && ['pending', 'waiting_runs', 'switching', 'deploying', 'blocked'].includes(item.phase || item.status) && (item.plugins || []).some((plugin) => plugins.includes(plugin)));
      const changedTarget = plugins.some((plugin) => model.rolloutTargets[plugin]?.target_id && rollout.target_ids?.[plugin] && model.rolloutTargets[plugin].target_id !== rollout.target_ids[plugin]);
      const newerPublication = model.rollouts.some((item) => item.rollout_id !== rollout.rollout_id && item.mode !== 'assign' && ['ready', 'completed'].includes(item.status) && item.created_at && rollout.created_at && item.created_at > rollout.created_at && (item.plugins || []).some((plugin) => plugins.includes(plugin)));
      const historical = ['cancelled', 'superseded'].includes(rollout.status) || ['ready', 'completed'].includes(rollout.status) && (pendingSuccessor || changedTarget || newerPublication);
      const runnable = !historical && ['ready', 'completed'].includes(rollout.status);
      const label = historical && ['ready', 'completed'].includes(rollout.status) ? '历史发布 · ' + (rollout.status === 'completed' ? '已完成' : '部分机器已就绪') : rolloutLabels[rollout.status] || rollout.status;
      const waitingHtml = !historical && ['pending', 'waiting_runs', 'blocked'].includes(phase) ? waitingRunsHtml(waiting, rollout) : '';
      const repairable = ['switching', 'deploying'].includes(rollout.phase || rollout.status) && !ready && (rollout.error || clients.some((client) => client.status === 'failed'));
      const resources = resourceSummaryHtml(rollout.resource_summary);
      return `<article class="workspace-surface rollout-card" data-rollout-row="${esc(rollout.rollout_id)}" data-rollout-current="${!historical}" tabindex="-1"><div class="panel-head"><div><h3>${esc(rollout.profile_name || rollout.profile_id)}</h3><p class="workspace-help">${esc((rollout.plugins || []).join('、'))} · ${esc(rollout.created_at ? new Date(rollout.created_at).toLocaleString('zh-CN') : '')}</p></div><span class="chip ${runnable ? 'green' : rollout.status === 'blocked' ? 'red' : 'amber'}">${esc(label)}</span></div><ol class="rollout-steps">${['等待旧批次', '准备资源', '应用更新', '更新结果'].map((label, index) => `<li class="${index < stage ? 'done' : index === stage ? 'current' : ''}">${label}</li>`).join('')}</ol>${rollout.error ? `<p class="rollout-notice error">${esc(rollout.error)}</p>` : ''}${stopped && waitingHtml ? '<p class="rollout-notice error">有旧批次被停止。恢复并完成，或明确说明原因放弃剩余任务后，更新才能继续。</p>' : ''}${historical && ['ready', 'completed'].includes(rollout.status) ? `<p class="config-help">本次发布记录：${ready} / ${clients.length} 台机器当时已就绪。当前目标已更新，请查看最新更新记录。</p>` : runnable ? `<p class="rollout-notice success">${ready} / ${clients.length} 台机器已就绪，可执行新版任务。未就绪机器不会领取新版任务。</p>` : rollout.status === 'superseded' ? '<p class="config-help">此请求已由修复发布接替，后续进度请查看新的更新记录。</p>' : rollout.status === 'cancelled' ? '<p class="config-help">旧版本运行与排队批次继续，不改变已生效资源。</p>' : '<p class="rollout-notice">请求已持久保存，关闭页面不影响推进。旧版批次完成后自动同步与更新；新版可运行前暂停提交这些插件的新批次。</p>'}${resources ? `<details class="rollout-client-details"><summary>本次发布的资源版本</summary>${resources}</details>` : ''}${waitingHtml}<details class="rollout-client-details"><summary>逐台进度 · ${ready} 台就绪 / ${clients.length} 台目标</summary><div class="rollout-machines">${clients.map((client) => `<div class="rollout-machine" data-rollout-client="${esc(client.client_id)}"><strong>${esc(client.client_id)}</strong><div><span>${esc(clientLabels[client.status] || client.status)}</span>${client.error || client.message ? `<small>${esc(client.error || client.message)}</small>` : ''}</div>${client.status === 'failed' && rollout.can_retry ? `<button type="button" class="button compact" data-rollout-action="retry" data-rollout-id="${esc(rollout.rollout_id)}" data-client-id="${esc(client.client_id)}" ${model.rolloutBusy ? 'disabled' : ''}>重试此机器</button>` : ''}</div>`).join('')}</div></details><div class="rollout-footer"><small class="workspace-help">${rollout.updated_at ? `状态更新 ${esc(new Date(rollout.updated_at).toLocaleString('zh-CN'))}` : '状态自动刷新'}</small><div class="head-actions">${rollout.can_cancel ? `<button type="button" class="button" data-rollout-action="cancel" data-rollout-id="${esc(rollout.rollout_id)}" ${model.rolloutBusy ? 'disabled' : ''}>取消本次更新</button>` : ''}${rollout.can_retry && rollout.status === 'blocked' ? `<button type="button" class="button" data-rollout-action="retry" data-rollout-id="${esc(rollout.rollout_id)}" ${model.rolloutBusy ? 'disabled' : ''}>重试推进</button>` : ''}${repairable ? `<button type="button" class="button primary" data-rollout-repair="${esc(rollout.rollout_id)}">修正方案并重新发布</button>` : ''}${runnable ? '<button type="button" class="button primary" data-rollout-runs>进入运行总览</button>' : ''}</div></div><p class="management-status" data-rollout-message="${esc(rollout.rollout_id)}"></p></article>`;
    }
    function renderRollouts() {
      const html = model.rollouts.length ? model.rollouts.map(rolloutHtml).join('') : '<p class="config-help">还没有统一更新记录。发布请求保存在服务端，可随时返回查看。</p>';
      for (const id of ['profile-rollouts', 'client-rollouts']) {
        if (bridge.patchRegion) bridge.patchRegion($(id), html);
        else if ($(id).innerHTML !== html) $(id).innerHTML = html;
      }
      for (const id of ['profile-rollouts-status', 'client-rollouts-status']) message(id, model.rolloutError);
      bridge.rolloutsChanged?.(model.rollouts);
    }
    function rememberRollout(record) {
      if (!record?.rollout_id) return;
      model.rolloutGeneration++;
      model.rollouts = [record, ...model.rollouts.filter((item) => item.rollout_id !== record.rollout_id)];
      renderRollouts();
    }
    function loadRollouts() {
      if (model.rolloutLoading) return model.rolloutLoading;
      const generation = model.rolloutGeneration;
      model.rolloutLoading = (async () => {
        try { const result = await request('/api/deployment-rollouts'); if (generation !== model.rolloutGeneration) return; model.rollouts = result.rollouts || []; model.rolloutTargets = result.targets || {}; model.rolloutError = ''; renderRollouts(); }
        catch (error) { if (generation !== model.rolloutGeneration) return; model.rolloutError = `更新状态读取失败：${error.message}。已显示记录保留，请刷新确认。`; renderRollouts(); throw error; }
        finally { model.rolloutLoading = null; }
      })();
      return model.rolloutLoading;
    }
    function openRollout(id) {
      if (!$('profile-apply').hidden) closeApply(false);
      bridge.navigate('clients');
      const container = $('client-rollouts');
      const card = [...container.querySelectorAll('[data-rollout-row]')].find((item) => item.dataset.rolloutRow === id);
      const target = card || container; target.tabIndex = -1;
      target.scrollIntoView?.({ block: 'start' }); target.focus?.({ preventScroll: true });
    }
    async function rolloutAction(action, id, extra = {}) {
      const record = model.rollouts.find((item) => item.rollout_id === id);
      if (!record || model.rolloutBusy || action === 'cancel' && !record.can_cancel || action === 'retry' && !record.can_retry) return false;
      model.rolloutBusy = id; renderRollouts();
      let failure = '';
      try { const updated = await post(`/api/deployment-rollouts/${encodeURIComponent(id)}/${action}`, { revision: record.revision, ...extra }); rememberRollout(updated); await bridge.refresh?.(); return true; }
      catch (error) { failure = `操作未完成：${error.message}。请核对最新状态后重试。`; return false; }
      finally { model.rolloutBusy = ''; await loadRollouts().catch(() => {}); if (failure) model.rolloutError = failure; renderRollouts(); }
    }
    async function repairRollout(id) {
      if (model.applyBusy || model.editorBusy) return false;
      const record = model.rollouts.find((item) => item.rollout_id === id);
      if (!record || !['switching', 'deploying'].includes(record.phase || record.status)) return false;
      await edit(record.profile_id);
      if (model.draft?.profile_id !== record.profile_id) return false;
      model.replacement = { rollout_id: id, revision: record.revision, profile_id: record.profile_id };
      renderEditor(); manage();
      message('profile-editor-status', '请选择修复后的资源，保存并发布。新请求会接替失败更新，原请求记录保留。');
      return true;
    }
    function rolloutClick(event) {
      const button = event.target.closest('button'); if (!button || button.disabled) return;
      const data = button.dataset;
      if (data.rolloutRepair) return repairRollout(data.rolloutRepair);
      if (data.rolloutViewRun) { closeApply(false); return bridge.viewRun?.(data.rolloutViewRun, data.plugin); }
      if ('rolloutRuns' in data) return bridge.navigate('overview');
      if (data.rolloutAbandon) {
        bridge.navigate('clients');
        $('rollout-abandon-panel').hidden = false;
        $('rollout-abandon-panel').dataset.rolloutId = data.rolloutId;
        $('rollout-abandon-panel').dataset.runId = data.rolloutAbandon;
        $('rollout-abandon-panel').dataset.plugin = data.plugin;
        $('rollout-abandon-run').textContent = data.rolloutAbandon;
        $('rollout-abandon-reason').value = ''; $('rollout-abandon-reason').focus?.();
        return;
      }
      if (data.rolloutAction) return rolloutAction(data.rolloutAction, data.rolloutId, data.clientId ? { client_ids: [data.clientId] } : data.runId ? { run_id: data.runId, plugin_type: data.plugin } : {});
    }
    function manage() { bridge.navigate('plugins'); bridge.tab('profiles'); }
    function syncVisibleSchedule() {
      const threads = Number($('profile-threads').value);
      let changed = threads !== model.draft.schedule.task_threads;
      model.draft.schedule.task_threads = threads;
      $('profile-windows').querySelectorAll('[data-profile-window][data-time]').forEach((input) => {
        const row = model.draft.schedule[input.dataset.profileWindow]?.[Number(input.dataset.index)];
        if (!row) return;
        changed ||= row[input.dataset.time] !== input.value;
        row[input.dataset.time] = input.value;
      });
      const visibleDays = new Map();
      $('profile-windows').querySelectorAll('[data-profile-window][data-day]').forEach((input) => {
        if (input.dataset.day === undefined) return;
        const key = `${input.dataset.profileWindow}:${input.dataset.index}`;
        if (!visibleDays.has(key)) visibleDays.set(key, []);
        if (input.checked) visibleDays.get(key).push(Number(input.dataset.day));
      });
      for (const [key, days] of visibleDays) {
        const [kind, index] = key.split(':'), row = model.draft.schedule[kind]?.[Number(index)];
        if (!row) continue;
        changed ||= JSON.stringify(row.days) !== JSON.stringify(days);
        row.days = days;
      }
      if (changed) draftChanged();
    }
    function handleEditorChange(event) {
      if (!model.draft || model.editorBusy) return;
      const input = event.target, data = input.dataset;
      draftChanged();
      if (input.id === 'profile-name') model.draft.name = input.value;
      else if (input.id === 'profile-description') model.draft.description = input.value;
      else if (input.id === 'profile-threads') model.draft.schedule.task_threads = Number(input.value);
      else if (data.profilePlugin) {
        const name = data.profilePlugin;
        if (input.checked) model.draft.plugins[name] = model.pluginDrafts[name] || defaultPlugin(name);
        else { model.pluginDrafts[name] = model.draft.plugins[name]; delete model.draft.plugins[name]; }
        renderEditor();
      } else if (data.profileResource) {
        const config = model.draft.plugins[data.profileResource];
        config.resources ||= {}; config.resources[data.role] = { artifact_id: input.value };
        renderEntries(); inventoryFor(data.profileResource, data.role);
      } else if (data.profileEntry) model.draft.plugins[data.profileEntry].resources[data.role][data.role === 'dataset' ? 'root' : 'entry'] = input.value;
      else if ('profileEngineDlls' in data) model.draft.plugins.coverage.engine_dlls = Array.from(input.selectedOptions).map((option) => option.value);
      else if (data.profileArguments) {
        model.draft.plugins.stpcompare.arguments ||= clone(defaultPlugin('stpcompare').arguments);
        model.draft.plugins.stpcompare.arguments[data.profileArguments] = input.value.split(/\r?\n/).filter((line) => line.trim().length);
      }
      else if (data.profileMode) model.draft.plugins[data.profileMode].dry_run = input.value === 'simulate';
      else if ('profileCov' in data) model.draft.plugins.coverage.store_cov = input.checked;
      else if (data.profileWindow) {
        const row = model.draft.schedule[data.profileWindow][Number(data.index)];
        if (data.time) row[data.time] = input.value;
        else if (data.day !== undefined) row.days = input.checked ? [...new Set([...(row.days || [1, 2, 3, 4, 5, 6, 7]), Number(data.day)])].sort() : (row.days || [1, 2, 3, 4, 5, 6, 7]).filter((day) => day !== Number(data.day));
      }
    }
    function init() {
      $('profiles-create').addEventListener('click', () => edit());
      $('profiles-refresh').addEventListener('click', () => load().catch(() => {}));
      $('profiles-list').addEventListener('click', (event) => { const button = event.target.closest('button'); if (button?.dataset.profileEdit) edit(button.dataset.profileEdit); else if (button?.dataset.profileUpdate) openUpdate(button.dataset.profileUpdate); });
      $('profile-replacement-cancel').addEventListener('click', () => { if (!model.editorBusy && !model.applyBusy) { model.replacement = null; invalidate(); renderEditor(); } });
      $('profile-save-publish').addEventListener('click', () => openUpdate('', true));
      $('profile-form').addEventListener('submit', (event) => { event.preventDefault(); save(); });
      $('profile-form').addEventListener('change', handleEditorChange);
      $('profile-form').addEventListener('input', (event) => { if (event.target.id === 'profile-threads' || event.target.dataset.profileWindow && event.target.dataset.time || event.target.dataset.profileArguments) handleEditorChange(event); });
      $('profile-name').addEventListener('input', handleEditorChange); $('profile-description').addEventListener('input', handleEditorChange);
      $('profile-editor-close').addEventListener('click', () => { if (!model.editorBusy) $('profile-editor').hidden = true; });
      $('profile-go-artifacts').addEventListener('click', () => { bridge.tab('artifacts'); bridge.loadArtifacts?.(); });
      $('profile-windows').addEventListener('click', (event) => {
        if (model.editorBusy) return;
        const add = event.target.closest('[data-profile-add-window]'), remove = event.target.closest('[data-profile-remove-window]');
        if (add) model.draft.schedule[add.dataset.profileAddWindow].push({ days: [1, 2, 3, 4, 5, 6, 7], start: '09:00', end: '18:00' });
        if (remove) model.draft.schedule[remove.dataset.profileRemoveWindow].splice(Number(remove.dataset.index), 1);
        if (add || remove) { draftChanged(); renderWindows(); }
      });
      $('profile-load-latest').addEventListener('click', async () => {
        if (!model.conflictDraft || model.editorBusy) return;
        const old = clone(model.draft), id = old.profile_id;
        model.editorBusy = true; editorControls();
        try {
          await load(); const latest = model.profiles.find((item) => item.profile_id === id); if (!latest) throw new Error('方案不存在');
          model.draft = clone(latest); normalizeSavedSchedule(model.draft);
          delete model.drafts[id]; model.dirtyProfiles.delete(id); model.pluginDrafts = {}; model.generation++;
          renderEditor();
          await Promise.all(Object.entries(model.draft.plugins).flatMap(([name, config]) => Object.keys(config.resources || {}).map((role) => inventoryFor(name, role, true))));
          model.conflictDraft = old; invalidate('已载入最新方案，请重新预览。');
          $('profile-conflict').hidden = false; $('profile-conflict-draft').value = JSON.stringify(old, null, 2);
          $('profile-load-latest').hidden = true;
          message('profile-editor-status', '已载入最新版本。原草稿保留在下方，请核对并合并需要的修改。');
        } catch (error) { model.draft = old; renderEditor(); message('profile-editor-status', `读取失败：${error.message}。草稿与原版本保留。`); }
        finally { model.editorBusy = false; editorControls(); }
      });
      $('profile-conflict-copy').addEventListener('click', async () => { try { await bridge.clipboard?.(JSON.stringify(model.draft, null, 2)); message('profile-editor-status', '草稿已复制。'); } catch (_) { $('profile-conflict').hidden = false; $('profile-conflict-draft').value = JSON.stringify(model.draft, null, 2); } });
      $('profile-apply-select').addEventListener('change', () => { if (!model.applyBusy) { model.selected = $('profile-apply-select').value; invalidate(model.dirtyProfiles.has(model.selected) ? '此方案有未保存修改，请先保存方案，再重新预览。' : '正在核对所选方案。'); preview(); } });
      $('profile-apply-preview').addEventListener('click', preview); $('profile-apply-ai-review').addEventListener('click', reviewWithDsh); $('profile-apply-publish').addEventListener('click', publish);
      $('profile-apply-confirm').addEventListener('change', applyControls);
      $('profile-apply-close').addEventListener('click', () => { if (!model.applyBusy) closeApply(); });
      $('profile-apply').addEventListener('cancel', (event) => { event.preventDefault(); if (!model.applyBusy) closeApply(); });
      $('profile-apply-manage').addEventListener('click', () => { closeApply(false); manage(); $('profiles-create')?.focus?.(); });
      $('profiles-advanced-open').addEventListener('click', () => { if (!model.applyBusy) { closeApply(false); bridge.advanced(); } });
      for (const id of ['profile-rollouts', 'client-rollouts', 'profile-apply-summary']) $(id).addEventListener('click', rolloutClick);
      for (const id of ['profile-rollouts-refresh', 'client-rollouts-refresh']) $(id).addEventListener('click', () => loadRollouts().catch(() => {}));
      $('rollout-abandon-close').addEventListener('click', () => { if (!model.rolloutBusy) $('rollout-abandon-panel').hidden = true; });
      $('rollout-abandon-form').addEventListener('submit', async (event) => {
        event.preventDefault(); const reason = $('rollout-abandon-reason').value.trim(); if (!reason) { message('rollout-abandon-status', '请填写放弃剩余任务的原因。'); return; }
        const data = $('rollout-abandon-panel').dataset;
        $('rollout-abandon-confirm').disabled = true;
        const ok = await rolloutAction('abandon', data.rolloutId, { run_id: data.runId, plugin_type: data.plugin, reason });
        if (ok) $('rollout-abandon-panel').hidden = true; else message('rollout-abandon-status', model.rolloutError);
        $('rollout-abandon-confirm').disabled = false;
      });
      load().catch(() => {});
      loadRollouts().catch(() => {});
    }
    return { init, load, edit, save, openApply, openUpdate, preview, publish, reviewWithDsh, selectionChanged, handleEditorChange, loadRollouts, openRollout, rolloutAction, repairRollout, model };
  }
  return { newProfile, defaultPlugin, entryChoices, autoEntry, validateProfile, summary, createController };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = DeploymentProfileUI;
if (typeof document !== 'undefined' && typeof window !== 'undefined' && typeof state !== 'undefined') {
  window.deploymentProfiles = DeploymentProfileUI.createController({
    document, request: jsonFetch, navigate, tab: setWorkspaceTab, operatorId: state.operatorId,
    setBusy: (busy) => { setDeploymentBusy(busy); renderClients(); },
    setSelected: (ids) => { state.selectedAgentIds = ids; renderClients(); },
    advanced: () => openBatchDeployment(), loadArtifacts,
    refresh: () => Promise.all([loadAgents(), loadArtifacts(), loadScheduler()]),
    clipboard: (value) => navigator.clipboard.writeText(value),
    uuid: createUuid,
    patchRegion: (element, html) => patchClientList(html, element),
    rolloutsChanged: (rollouts) => { state.rollouts = rollouts; renderScheduler(); },
    viewRun: async (run, plugin) => { navigate('results'); await switchPlugin(plugin, run); },
    prepareDshDeploymentContext, clearDshDeploymentContext,
  });
  window.deploymentProfiles.init();
}
