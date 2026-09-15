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
    style: {}, dataset: {}, children: [], listeners: {}, attributes: {},
    classList: { toggle() {} },
    querySelector() { return element(); },
    addEventListener(name, callback) { this.listeners[name] = callback; },
    setAttribute(name, value) { this.attributes[name] = String(value); },
    appendChild(child) { this.children.push(child); },
    replaceChildren(...children) { this.children = children; },
  };
}

async function createConsole({ available = 'codex', databaseReady = false, runsError = false, chatConversationId = null, chatGate = null } = {}) {
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
  const storage = new Map();
  get('result-type').value = 'coverage';
  const context = vm.createContext({
    document: {
      body: element(),
      getElementById: get,
      createElement: element,
      querySelectorAll: (selector) => {
        if (selector === '#quick-actions button') return quickActions;
        if (selector === '[data-ai-tab]') return aiTabs;
        return [];
      },
    },
    localStorage: {
      getItem: (key) => storage.get(key) ?? null,
      setItem: (key, value) => storage.set(key, String(value)),
    },
    window: { innerWidth: 1440, matchMedia: () => ({ matches: false }), addEventListener() {} },
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
          if (chatGate) await chatGate;
          data = { text: 'Local AI response', conversation_id: chatConversationId };
          break;
        case '/api/ai/dsh-web':
          assert.equal(options.method, 'POST');
          posts.push({ dshWeb: true });
          data = { url: 'http://127.0.0.1:56335/' };
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
  return { get, quickActions, aiTabs, posts, storage };
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
  assert.equal(ui.get('dsh-native-frame').src, 'http://127.0.0.1:56335/');
  assert.deepEqual(ui.posts.at(-1), { dshWeb: true });
});
