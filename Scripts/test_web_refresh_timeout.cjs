const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('Resources/OuterShellWeb/app.js', 'utf8');
const timers = new Map();
let serial = 0;
let hung = true;
const wait = signal => hung ? new Promise((resolve, reject) => signal.addEventListener('abort', () => reject(signal.reason), {once: true})) : Promise.resolve({workspaces: [], buffer: [], layout: {}, revision: 1});
const state = {refreshAbort: {}, refreshErrorTimers: {}};
const context = {state, AbortController, DOMException, document: {hidden: false},
  window: {setTimeout(fn) { timers.set(++serial, fn); return serial; }, clearTimeout(id) { timers.delete(id); }},
  render() {}, renderSafeSpaces() {}, updateStatus() {}, applyLayout() {},
  safeSpaceRequest: (operation, values, signal) => wait(signal),
  requestBuffer: (url, options) => wait(options.signal), fetchLayout: wait,
  decodeBackends: () => ({backends: [], error: ''})};
vm.createContext(context);
for (const [start, end] of [
  ['  async function refreshLayout(', '  function saveLayout('],
  ['  async function refreshSafeSpaces(', '  function findSafeSpace('],
  ['  async function refreshBackends(', '  function suspendRefreshes('],
]) vm.runInContext(source.slice(source.indexOf(start), source.indexOf(end, source.indexOf(start))), context);
(async () => {
  for (const [name, key] of [['refreshLayout', 'layoutError'], ['refreshBackends', 'backendError'], ['refreshSafeSpaces', 'safeSpacesError']]) {
    hung = true;
    const pending = context[name]();
    assert.equal(timers.size, 1);
    [...timers.values()][0]();
    await pending;
    assert.match(state[key], /not responding/);
    assert.equal(timers.size, 0);
    hung = false;
    await context[name]();
    assert.equal(state[key], '');
    assert.equal(timers.size, 0);
  }
  assert.equal(state.safeSpacesRefreshing, false);
  assert.equal(state.layoutFetching, false);
  assert.equal(state.loading, false);
  console.log('PASS: stalled initial reads time out, clear loading flags, report failure, and recover on retry');
})().catch(error => { console.error(error); process.exitCode = 1; });
