const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('Resources/OuterShellWeb/app.js', 'utf8');
const timers = new Map();
let timerID = 0;
let reply = {appLog: {path: '/custom/output.log', contents: '<script>untrusted</script>', fileSize: 900000, isTruncated: true}};
let requests = [];
const context = {state: {}, AbortController, Uint8Array, atob,
  window: {setTimeout(f) {timers.set(++timerID, f); return timerID;}, clearTimeout(id) {timers.delete(id);}},
  startRefreshTimeout: () => 0,
  escapeHTML: s => String(s).replaceAll('<', '&lt;'), formatBytes: n => `${n} B`,
  safeSpaceRequest: async (op, args) => {requests.push({op, args}); return reply;},
  decodeBackends: () => ({backends: [{serviceID: 'app', logFiles: [{path: '/custom/a.log'}, {path: '/custom/b.log'}]}]}),
  decodeLog: () => ({path: '/custom/b.log', contents: 'second log', fileSize: 10})};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('  function decodeContainerLog('), source.indexOf('  function openLogs(')), context);
function selection() {
  const output = {textContent: '', scrollHeight: 100, scrollTop: 0, clientHeight: 100};
  const meta = {textContent: ''};
  const select = {innerHTML: '', value: '', disabled: false};
  const dialog = {isConnected: true, querySelector: id => ({'#log-output': output, '#log-meta': meta, '#log-select': select})[id]};
  return {workspaceID: 'lab', serviceID: 'app', index: 0, dialog, output, meta, select};
}
(async () => {
  const s = context.state.logSelection = selection();
  await context.fetchContainerLog(s);
  assert.equal(s.output.textContent, '<script>untrusted</script>');
  assert.match(s.meta.textContent, /latest output/);
  assert.equal(requests[0].op, 'appLogs');
  assert.equal(timers.size, 1);
  s.index = 1;
  reply = {appLog: {backendsPayload: 'AA==', logPayload: 'AA=='}};
  await context.fetchContainerLog(s);
  assert.equal(s.output.textContent, 'second log');
  assert.equal(s.select.value, '1');
  assert.equal(requests.at(-1).args.logIndex, 1);
  reply = {appLog: {path: '', contents: '', error: 'missing'}};
  await context.fetchContainerLog(s);
  assert.equal(s.output.textContent, 'No registered log file.');
  assert.equal(s.select.disabled, true);
  let complete;
  context.safeSpaceRequest = () => new Promise(resolve => {complete = resolve;});
  const pending = context.fetchContainerLog(s);
  const before = s.output.textContent;
  context.state.logSelection = null;
  s.dialog.isConnected = false;
  complete({appLog: {path: '/x', contents: 'stale'}});
  await pending;
  assert.equal(s.output.textContent, before);
  assert.equal(timers.size, 0);
  assert.equal(s.loading, false);
  console.log('PASS: log selection, polling, truncation, literal text, empty logs, and stale-response cleanup');
})().catch(error => {console.error(error); process.exitCode = 1;});
