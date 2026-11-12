const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const source = fs.readFileSync(path.join(__dirname, "../Resources/OuterShellWeb/app.js"), "utf8");
const state = { hostPlatform: "windows", providers: [], safeSpaces: [], groupOrder: [], endpointOrder: {} };
const elements = { overview: { innerHTML: "" } };
const context = {
  state, elements, document: { createElement: () => ({ set innerHTML(value) { this.content = value; } }) }, window: { outerLoop: { sessionContext: { username: "test-user" } } },
  renderInitialLoading: () => false, escapeHTML: String,
  groupPinKeys: () => [], overviewEndpoint: entry => entry.key,
  overviewShortcut: entry => entry.key, containerCommandsHTML: () => "",
  containerPreparing: () => false, safeSpaceState: () => "running",
  safeSpaceIsRunning: () => true, managedContainer: () => true
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf("  function renderOverview(entries)"), source.indexOf("  function saveGroupOrder(")), context);
context.updateOverviewChildren = (parent, markup) => { parent.innerHTML = markup; };
const entries = [
  { key: "ordinary-app", item: { backend: { serviceScope: "user" } } },
  { key: "custom-privileged-app", item: { backend: { serviceScope: "system" } } }
];
context.renderOverview(entries);
assert.match(elements.overview.innerHTML, /data-group-id="user"/);
assert.match(elements.overview.innerHTML, /data-group-id="root"/);
assert.match(elements.overview.innerHTML, /data-action="open-add" data-scope="system"/);
assert.match(elements.overview.innerHTML, /<h2>test-user<\/h2><p><svg[^>]*>.*Administrator<\/p>/);
const userCard = elements.overview.innerHTML.slice(0, elements.overview.innerHTML.indexOf('data-group-id="root"'));
assert.match(userCard, /ordinary-app/);
assert.doesNotMatch(userCard, /custom-privileged-app/);
assert.match(elements.overview.innerHTML, /custom-privileged-app/);
state.safeSpaces = [{ id: "ubuntu", name: "Ubuntu", runtimeKind: "wsl", userName: "linux-user" }];
context.renderOverview(entries);
assert.match(elements.overview.innerHTML, /data-group-id="container:ubuntu:root"/);
assert.match(elements.overview.innerHTML, /data-action="open-wsl-add"[^>]*data-scope="system"/);
state.safeSpaces = [];
for (const platform of ["linux", "macos"]) {
  state.hostPlatform = platform;
  context.renderOverview(entries);
  assert.match(elements.overview.innerHTML, /data-group-id="root"/);
  assert.match(elements.overview.innerHTML, /data-scope="system"/);
}
console.log("PASS: Windows has a shielded Administrator card for system backends, Linux/macOS and WSL retain root cards");
