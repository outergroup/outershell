const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../Resources/OuterShellWeb/app.js'), 'utf8');
let now = 0, requests = 0;
const state = { endpointControls: new Map(), safeSpacesRevision: 0, safeSpaces: [] };
const context = { state, performance: { now: () => now }, AbortController,
  startRefreshTimeout: () => 1, window: { clearTimeout() {} }, closeDialog() {}, renderSafeSpaces() {},
  toast: message => { throw Error(message); },
  findSafeSpaceApp: w => w?.apps[0],
  safeSpaceRequest: async () => {
    assert.equal(state.endpointControls.get('app'), 'Stopping');
    requests++;
    return { workspaces: [{ id: 'w', apps: [{ isRunning: requests < 4 }] }] };
  }, delay: async ms => { now += ms; }
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('  async function waitForStoppedContainerApp('), source.indexOf('  async function performControl(')), context);
(async () => {
  await context.performEndpointControl('app', 'stopApp', () => context.waitForStoppedContainerApp('w', 'service', 'frontend'));
  assert.equal(requests, 4);
  assert.equal(state.safeSpaces[0].apps[0].isRunning, false);
  assert.equal(state.safeSpacesRevision, 1);
  assert.equal(state.endpointControls.size, 0);
  console.log('PASS: stopping indicator survives stale running snapshots and clears only with confirmed stopped state');
})().catch(error => { console.error(error); process.exitCode = 1; });
