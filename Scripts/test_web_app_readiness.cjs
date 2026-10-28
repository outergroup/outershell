const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../Resources/OuterShellWeb/app.js'), 'utf8');
async function scenario(readyAt, unavailable = false) {
  let now = 0, requests = 0, cleared = 0;
  const context = {
    performance: { now: () => now }, AbortController,
    window: { clearTimeout: () => { cleared++; } },
    startRefreshTimeout: () => 1,
    state: { safeSpacesRefreshing: true, safeSpaces: [] },
    refreshSafeSpaces: () => { throw Error('Must not rely on a skipped background refresh'); },
    safeSpaceRequest: async () => {
      requests++; now += 25;
      return { workspaces: [{ id: 'w', state: unavailable ? 'unavailable' : 'running', apps: [{ serviceID: 'a', isRunning: now >= readyAt }] }] };
    },
    findSafeSpaceApp: (w, id) => w?.apps.find(a => a.serviceID === id),
    safeSpaceAppIsReady: (w, a) => w.state === 'running' && a.isRunning,
    safeSpaceState: w => w.state,
    delay: async ms => { now += ms; }
  };
  vm.createContext(context);
  vm.runInContext(source.slice(source.indexOf('  async function waitForSafeSpaceApp('), source.indexOf('  async function startSafeSpace(')), context);
  if (unavailable) {
    await assert.rejects(context.waitForSafeSpaceApp('w', 'a', ''), /container is unavailable/);
    assert.equal(requests, 1);
  } else if (Number.isFinite(readyAt)) {
    const result = await context.waitForSafeSpaceApp('w', 'a', '');
    assert.equal(result.app.isRunning, true);
    assert.ok(now >= readyAt);
  } else {
    await assert.rejects(context.waitForSafeSpaceApp('w', 'a', ''), /two minutes/);
    assert.equal(now, 120000);
  }
  assert.equal(cleared, requests);
}
(async () => {
  await scenario(20000);
  await scenario(90000);
  await scenario(Infinity);
  await scenario(Infinity, true);
  console.log('PASS: slow starts, concurrent refresh, elapsed deadline, unavailable container, and timer cleanup');
})().catch(error => { console.error(error); process.exitCode = 1; });
