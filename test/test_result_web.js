'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const script = fs.readFileSync(path.join(__dirname, '../result_service/web/app.js'), 'utf8');

// The console uses plain DOM properties. Keep this stub limited to the
// properties and event hooks exercised by initialization and the chat form.
function element() {
  return {
    value: '', disabled: false, hidden: false, textContent: '', innerHTML: '',
    style: {}, dataset: {}, children: [], listeners: {},
    classList: { toggle() {} },
    querySelector() { return element(); },
    addEventListener(name, callback) { this.listeners[name] = callback; },
    appendChild(child) { this.children.push(child); },
    replaceChildren(...children) { this.children = children; },
  };
}

async function createConsole({ available = 'codex', databaseReady = false, runsError = false } = {}) {
  const elements = new Map();
  const get = (id) => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  const quickActions = [element(), element()];
  const posts = [];
  get('result-type').value = 'coverage';
  const context = vm.createContext({
    document: {
      getElementById: get,
      createElement: element,
      querySelectorAll: (selector) => selector === '#quick-actions button' ? quickActions : [],
    },
    window: { matchMedia: () => ({ matches: false }) },
    fetch: async (url, options = {}) => {
      let status = 200;
      let data;
      switch (url) {
        case '/healthz': data = { status: 'ok' }; break;
        case '/readyz':
          status = databaseReady ? 200 : 503;
          data = { database_configured: databaseReady };
          break;
        case '/api/plugins': data = [{ name: 'coverage' }]; break;
        case '/api/ai/providers':
          data = ['codex', 'dsh'].map((name) => ({ name, available: name === available }));
          break;
        case '/api/runs?limit=100':
          status = runsError ? 500 : 200;
          data = runsError ? { error: 'temporary query failure' } : [];
          break;
        case '/api/ai/chat':
          assert.equal(options.method, 'POST');
          posts.push(JSON.parse(options.body));
          data = { text: 'Local AI response' };
          break;
        default: throw new Error(`unexpected request: ${url}`);
      }
      return { ok: status === 200, status, json: async () => data };
    },
  });
  vm.runInContext(script, context, { filename: 'app.js' });
  // Fetch responses are immediate promises; let initialization and its error
  // handler finish before inspecting the visible controls.
  await new Promise(setImmediate);
  return { get, quickActions, posts };
}

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
    context: { run: null, result_type: 'coverage' },
  }]);
  assert.equal(ui.get('question').value, '');
  assert.equal(ui.get('chat').children.at(-1).textContent, 'Local AI response');
});
