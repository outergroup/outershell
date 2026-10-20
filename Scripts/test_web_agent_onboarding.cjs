const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const root = process.argv[2] || process.cwd();
const source = fs.readFileSync(path.join(root, "Resources/OuterShellWeb/app.js"), "utf8");
const functions = source.slice(source.indexOf("  const agentExamples ="), source.indexOf("  function renderOverview(entries)"));
const quote = source.slice(source.indexOf("  function shellQuotedArgument("), source.indexOf("  function containerCommandVariants("));
const context = { state: {}, window: {}, launcherItems: () => [], TextEncoder, btoa };
vm.createContext(context); vm.runInContext(quote + functions, context);
context.window.outerLoop = {sessionContext: {sshCommandArguments: ["ssh", "-p", "2222", "-i", "/keys/my key", "me@host"]}};
assert.equal(context.agentConnection().command, "ssh -p 2222 -i '/keys/my key' me@host");
const prompt = context.agentPrompt("Install Jupyter.");
assert.match(prompt, /ssh -p 2222/); assert.match(prompt, /https:\/\/outershell.org\/api\//); assert.match(prompt, /Install Jupyter/);
vm.runInContext(source.slice(source.indexOf("  function containerCommands("), source.indexOf("  function containerCommandsHTML(")) + source.slice(source.indexOf("  function containerCommandVariants("), source.indexOf("  function openContainerCommand(")), context);
context.state.addWorkspace = {id: "test", name: "Experiment container", runtimeKind: "docker", shellCommand: "docker exec -it 'my container' /bin/sh"};
const containerPrompt = context.agentPrompt("Install Jupyter.");
assert.equal(context.agentConnection().command, context.containerCommandVariants(context.containerCommands(context.state.addWorkspace)[0])[0].text);
assert.match(containerPrompt, /Connect to the container Experiment container/);
assert.match(containerPrompt, /ssh -t -p 2222/);
assert.match(containerPrompt, /Install and register apps inside this container/);
assert.match(containerPrompt, /https:\/\/outershell\.org\/api\/containers\//);
context.window.outerLoop.sessionContext.sshCommandArguments = [];
assert.match(context.agentPrompt("Do work."), /On the server, enter the container/);
assert.match(context.agentPrompt("Do work."), /docker exec/);
context.state.addWorkspace = null;
assert.match(context.agentPrompt("Do work."), /Work on this server/);
context.window.outerLoop = undefined;
assert.equal(context.agentConnection().local, false);
assert.doesNotMatch(context.agentPrompt("Do work."), /\[your SSH command\]/);
assert(!source.includes('id="agent-ssh"'));
context.launcherItems = () => [{identity: "installed", displayName: "My assistant", backend: {serviceID: "org.outershell.OpenCode"}, primary: {backend: {serviceScope: "user"}}}];
assert.equal(context.openCodeItem().identity, "installed");
for (const extension of ["sh", "ps1"]) {
  const script = fs.readFileSync(path.join(root, `Resources/OuterShellWeb/opencode-start.${extension}`), "utf8")
    .replace("__OUTERSHELL_SKILL__", () => fs.readFileSync(path.join(root, "Resources/OuterShellWeb/outer-shell-skill.md"), "utf8").trimEnd())
    .replace("__OUTERSHELL_ADMIN__", () => fs.readFileSync(path.join(root, "Resources/OuterShellWeb/server-admin.md"), "utf8").trimEnd());
  assert(!script.includes("__OUTERSHELL_"));
  if (extension === "sh") assert(Buffer.byteLength(script) < 32768);
  else {
    const original = script + "\n# Unicode: café 🦊; shell: %PATH% & | $() `\n";
    const command = context.windowsOpenCodeCommand(original, 4096);
    assert(Buffer.byteLength(command) < 262144);
    const lines = command.split("\n");
    assert(lines.every(line => line.length < 8192));
    const encoded = lines.filter(line => /^>>? /.test(line)).map(line => line.split(" echo ")[1]).join("");
    assert.equal(Buffer.from(encoded, "base64").toString("utf8"), original);
    assert.match(command, /-Port 4096/);
  }
}
console.log("PASS: connection prompts, OpenCode discovery, installer limits, Windows Unicode and shell-safe encoding");

context.escapeHTML = value => String(value ?? "");
context.managedContainer = workspace => workspace.ownsContainer !== false;
context.state.backends = [];
context.openDialog = html => { context.html = html; return {}; };
context.containerFrontendKey = () => "web";
context.safeSpaceAppNavigationURL = () => "http://container.localhost";
vm.runInContext(source.slice(source.indexOf("  function openAddDialog("), source.indexOf("  async function loadAddRecipes(")), context);
context.state.addWorkspace = {id: "test", name: "Experiment container", runtimeKind: "wslContainer", shellCommand: "container-shell", apps: []};
context.openAddDialog("agent");
assert.match(context.html, /Edit Dockerfile/);
assert.match(context.html, /data-tab="catalog"/);
assert.match(context.html, /data-action="configure-container"/);
assert.doesNotMatch(context.html, /data-action="setup-opencode"|data-tab="bash"/);
context.openAddDialog("dockerfile");
assert.match(context.html, /Edit container…/);
context.state.addWorkspace.apps = [{serviceID: "OpenCode", displayName: "OpenCode"}];
context.openAddDialog("agent");
assert.match(context.html, /href="http:\/\/container.localhost"/);
context.state.addWorkspace.ownsContainer = false;
context.openAddDialog("agent");
assert.doesNotMatch(context.html, /Edit Dockerfile/);
context.state.addWorkspace = null;
context.openAddDialog("dockerfile");
assert.match(context.html, /data-tab="catalog"/);
assert.match(context.html, /data-action="setup-opencode"/);
assert.doesNotMatch(context.html, /data-tab="dockerfile"/);
console.log("PASS: container SSH reuse, local container command, scoped actions, Dockerfile navigation, installed OpenCode link, host navigation");

context.state.addWorkspace = {id: "container", name: "Test", runtimeKind: "docker"};
context.state.containerCatalog = {id: "container", original: "FROM ubuntu:24.04", dockerfile: "FROM ubuntu:24.04", selected: []};
context.openAddDialog("catalog");
for (const id of ["files", "plaintext", "top", "opencode"]) assert(context.html.includes(`data-catalog-id="${id}"`));
assert.match(context.html, /save and rebuild to install/);
assert.doesNotMatch(context.html, /data-action="install"/);
