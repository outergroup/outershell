const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const source = fs.readFileSync(`${process.argv[2] || process.cwd()}/Resources/OuterShellWeb/app.js`, "utf8");
const context = {
  state: { providers: [] },
  escapeHTML: value => String(value).replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll('"', "&quot;"),
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf("  function runtimeCanCreate("), source.indexOf("  async function containerOperation(")), context);
assert.match(context.runtimeGuidance(), /Check again/);
context.state.providers = [
  { id: "docker", name: "Docker", status: "notInstalled", isAvailable: false, detail: "Install on this server <first>", setupURL: "https://docs.docker.com/" },
  { id: "apple.container", name: "Apple", status: "unsupported", isAvailable: false },
];
assert.match(context.runtimeOptions(), /disabled/);
assert.match(context.runtimeGuidance(), /&lt;first>/);
assert.match(context.runtimeGuidance(), /Setup instructions/);
assert(!context.runtimeCanCreate(context.state.providers[0]));
context.state.providers[0].status = "stopped";
assert(!context.runtimeCanCreate(context.state.providers[0]));
context.state.providers[1].status = "stopped";
assert(context.runtimeCanCreate(context.state.providers[1]));
context.state.providers[0].isAvailable = true;
assert(context.runtimeCanCreate(context.state.providers[0]));
context.state.providers[0].isAvailable = false;
context.state.providers[0].setupURL = "javascript:alert(1)";
assert(!context.runtimeGuidance().includes("javascript:"));
console.log("PASS: missing/unsupported/stopped/ready runtime UI, setup guidance, escaping, and safe links");
(async () => {
  const recheck = { disabled: false, addEventListener(_, handler) { this.handler = handler; } };
  const submit = { disabled: true };
  const message = { textContent: "" };
  const guidance = { innerHTML: "" };
  const form = {
    elements: { runtimeProviderID: { value: "", innerHTML: "" }, baseImage: { value: "" }, name: { value: "My project" } },
    querySelector(selector) { return ({ ".runtime-recheck": recheck, ".container-message": message, ".runtime-guidance": guidance, '[type="submit"]': submit })[selector]; },
    addEventListener() {},
  };
  const dialog = { isConnected: true, querySelector: () => form };
  context.openDialog = () => dialog;
  context.containerDialogHeader = () => "";
  context.state.providers = [];
  context.safeSpaceRequest = async operation => {
    assert.equal(operation, "checkRuntimes");
    context.state.providers = [{ id: "docker", name: "Docker", isAvailable: true, status: "ready", detail: "Ready", defaultBaseImage: "debian:bookworm" }];
  };
  vm.runInContext(source.slice(source.indexOf("  function openCreateContainer("), source.indexOf("  function configurationRow(")), context);
  context.openCreateContainer();
  await recheck.handler({ currentTarget: recheck });
  assert.equal(submit.disabled, false);
  assert.equal(recheck.disabled, false);
  assert.equal(form.elements.name.value, "My project");
  assert.equal(form.elements.baseImage.value, "debian:bookworm");
  assert.match(form.elements.runtimeProviderID.innerHTML, /docker/);
  assert.equal(message.textContent, "Runtime check complete.");
  context.safeSpaceRequest = async () => { throw new Error("Server disconnected"); };
  await recheck.handler({ currentTarget: recheck });
  assert.equal(recheck.disabled, false);
  assert.equal(message.textContent, "Server disconnected");
  console.log("PASS: recheck enables creation after installation, preserves form input, and recovers from request failure");
})().catch(error => { console.error(error); process.exitCode = 1; });
