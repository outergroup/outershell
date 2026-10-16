const assert = require("node:assert/strict"), fs = require("node:fs"), vm = require("node:vm");
const root = process.argv[2] || process.cwd();
const source = fs.readFileSync(`${root}/Resources/OuterShellWeb/app.js`, "utf8");
const css = fs.readFileSync(`${root}/Resources/OuterShellWeb/style.css`, "utf8");
let dialog = "", operation = "", success = false, closed = 0, refreshes = 0;
const backend = {displayName: "Top <test>", serviceScope: "system"};
const message = "Root Outer Shell support could not start.\nsystemd could not execute the daemon (203/EXEC).\nPermission denied <script>alert(1)</script>";
const context = {
  findBackend: () => backend,
  escapeHTML: value => String(value).replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll('"', "&quot;"),
  openDialog: html => { dialog = html; },
  closeDialog: () => { closed++; },
  control: async (unused, action) => { operation = action; return {ok: success, message}; },
  toast: () => { assert(success, "Failures must remain visible in a dialog"); },
  refreshBackends: async () => { refreshes++; }, refreshSafeSpaces: async () => {},
  state: {hostPlatform: "linux"}
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf("  function showOperationFailure("), source.indexOf("  async function uninstallBackend(")), context);
(async () => {
  const button = {disabled: false, textContent: "Install as root", querySelector: () => null};
  await context.installBackend("top", button);
  assert.equal(operation, "runRoot");
  assert.match(dialog, /Could not install Top &lt;test>/);
  assert.match(dialog, /203\/EXEC/); assert.match(dialog, /Permission denied &lt;script>/);
  assert(!dialog.includes("<script>"));
  assert.match(dialog, /<pre class="operation-error" role="alert">/);
  assert.match(dialog, /data-action="close-dialog"/);
  assert.equal(button.disabled, false); assert.equal(button.textContent, "Install as root");
  assert.equal(closed, 0); assert.equal(refreshes, 0);
  await context.performControl("top", "uninstall", button);
  assert.equal(operation, "uninstall"); assert.match(dialog, /Could not complete the action/);
  assert.equal(button.disabled, false);
  success = true; dialog = "";
  await context.installBackend("top", button);
  assert.equal(dialog, ""); assert.equal(closed, 1); assert.equal(refreshes, 1);
  assert.match(css, /\.operation-error[^}]*white-space: pre-wrap/);
  assert.match(css, /\.operation-error[^}]*overflow: auto;[^}]*user-select: text/);
  console.log("PASS: persistent escaped startup details, selectable scrollable text, restored actions, normal success flow");
})().catch(error => { console.error(error); process.exitCode = 1; });
