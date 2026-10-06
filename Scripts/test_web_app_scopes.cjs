const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const source = fs.readFileSync(path.join(__dirname, "../Resources/OuterShellWeb/app.js"), "utf8");
let html = "";
const calls = [];
const record = (scope, installed) => ({
  serviceID: "org.outershell.Top", displayName: "Top", serviceScope: scope,
  isBundled: true, isInstalled: installed, supportsRoot: true
});
const state = { addTab: "catalog", addScope: "user", backends: [record("user", false), record("system", true)] };
const context = {
  state, escapeHTML: String, iconHTML: () => "", backendKey: b => b.serviceScope,
  openDialog: value => { html = value; }, closeDialog() {}, toast() {},
  findBackend: scope => state.backends.find(b => b.serviceScope === scope),
  control: async (backend, operation) => { calls.push([backend.serviceScope, operation]); return { ok: true }; },
  refreshBackends: async () => {}
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf("  function openAddDialog("), source.indexOf("  function wireBashForm(")), context);
vm.runInContext(source.slice(source.indexOf("  async function installBackend("), source.indexOf("\n  function ", source.indexOf("  async function installBackend("))), context);
(async () => {
  context.openAddDialog();
  assert.match(html, /data-backend-key="user"/);
  assert.doesNotMatch(html, /data-backend-key="system"/);
  await context.installBackend("user", {});
  state.addScope = "system";
  context.openAddDialog();
  assert.match(html, /There are no additional bundled apps available/);
  state.backends = [record("user", true), record("system", false)];
  context.openAddDialog();
  assert.match(html, /data-backend-key="system"/);
  assert.match(html, /Install as root/);
  assert.doesNotMatch(html, /data-backend-key="user"/);
  await context.installBackend("system", {});
  assert.deepEqual(calls, [["user", "runUser"], ["system", "runRoot"]]);
  console.log("PASS: user/root catalogs and install requests are independent");
})().catch(error => { console.error(error); process.exitCode = 1; });
