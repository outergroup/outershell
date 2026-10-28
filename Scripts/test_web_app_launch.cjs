const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../Resources/OuterShellWeb/app.js'), 'utf8');
async function scenario(fail) {
  const workspace = { id: 'w' }, app = { serviceID: 'a', isRunning: false };
  const state = { appLaunches: new Map(), endpointControls: new Map(), safeSpaceBusy: new Set() };
  let timer, delay, cleared = false, starts = 0, navigations = 0, resolve;
  const pending = new Promise(r => { resolve = r; });
  const context = { state,
    window: { setTimeout: (f, ms) => { timer = f; delay = ms; return 1; }, clearTimeout: () => { cleared = true; }, location: { assign: () => { navigations++; } } },
    findSafeSpace: () => workspace, findSafeSpaceApp: () => app,
    safeSpaceAppIsReady: () => false, safeSpaceIsRunning: () => true,
    renderSafeSpaces() {}, escapeHTML: String, containerFrontendKey: () => 'main',
    safeSpaceRequest: async () => { starts++; await pending; if (fail) throw Error('startup failed'); },
    waitForSafeSpaceApp: async () => ({ workspace, app }), safeSpaceAppNavigationURL: () => '/ready'
  };
  vm.createContext(context);
  vm.runInContext(source.slice(source.indexOf('  function appLaunchFeedback('), source.indexOf('  function overviewEndpoint(')) + source.slice(source.indexOf('  async function launchSafeSpaceApp('), source.indexOf('  async function refreshBackends(')), context);
  const entry = { workspace, app };
  const launch = context.launchSafeSpaceApp('w', 'a', 'main');
  assert.equal(delay, 1000);
  assert.match(context.overviewStatus(entry), /address-status app-launch-spinner/);
  assert.equal(context.appLaunchFeedback(entry), "");
  assert.doesNotMatch(context.appLaunchFeedback(entry), /View logs/);
  await context.launchSafeSpaceApp('w', 'a', 'main');
  assert.equal(starts, 1);
  timer();
  assert.match(context.appLaunchFeedback(entry), /data-action="container-logs"/);
  assert.match(context.appLaunchFeedback(entry), /View logs/);
  resolve();
  if (fail) await assert.rejects(launch, /startup failed/); else await launch;
  assert.equal(navigations, fail ? 0 : 1);
  assert.equal(state.appLaunches.size, 0);
  assert.equal(state.safeSpaceBusy.size, 0);
  assert.equal(cleared, true);
  assert.equal(context.appLaunchFeedback(entry), '');
}
(async () => {
  await scenario(false);
  await scenario(true);
  console.log('PASS: immediate spinner, delayed logs, duplicate-click protection, and success/failure cleanup');
})().catch(error => { console.error(error); process.exitCode = 1; });
