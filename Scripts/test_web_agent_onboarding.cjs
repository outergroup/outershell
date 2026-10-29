const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const root = process.argv[2] || process.cwd();
const source = fs.readFileSync(path.join(root, "Resources/OuterShellWeb/app.js"), "utf8");
const functions = source.slice(source.indexOf("  const agentExamples ="), source.indexOf("  function renderOverview(entries)"));
const quote = source.slice(source.indexOf("  function shellQuotedArgument("), source.indexOf("  function containerCommandVariants("));
const context = { state: {}, window: { location: { hostname: "localhost" } }, launcherItems: () => [], TextEncoder, btoa };
vm.createContext(context); vm.runInContext(quote + functions, context);
context.window.outerLoop = {sessionContext: {sshCommandArguments: ["ssh", "-p", "2222", "-i", "/keys/my key", "me@host"]}};
assert.equal(context.agentConnection().command, "ssh -p 2222 -i '/keys/my key' me@host");
const prompt = context.agentPrompt("Install Jupyter.");
assert.match(prompt, /ssh -p 2222/); assert.match(prompt, /https:\/\/outershell.org\/api\//); assert.match(prompt, /Install Jupyter/);
vm.runInContext(source.slice(source.indexOf("  function containerCommands("), source.indexOf("  function containerCommandsHTML(")) + source.slice(source.indexOf("  function containerCommandVariants("), source.indexOf("  function openContainerCommand(")), context);
context.state.addWorkspace = {id: "test", name: "Experiment container", runtimeKind: "docker", shellCommand: "docker exec -it 'my container' /bin/sh"};
const containerPrompt = context.agentPrompt("Install Jupyter.");
assert.equal(context.agentConnection().command, "ssh -p 2222 -i '/keys/my key' me@host");
assert.match(containerPrompt, /docker exec 'my container' \/bin\/sh -lc '<command>'/);
assert.doesNotMatch(containerPrompt, /ssh -t|exec -it|COLORTERM/);
assert.match(containerPrompt, /Run commands inside the “Experiment container” container/);
assert.match(containerPrompt, /ssh -p 2222/);
assert.match(containerPrompt, /Install and register apps inside this container/);
assert.match(containerPrompt, /https:\/\/outershell\.org\/api\/containers\//);
context.window.outerLoop.sessionContext.sshCommandArguments = [];
assert.match(context.agentPrompt("Do work."), /Run commands inside the “Experiment container” container/);
assert.match(context.agentPrompt("Do work."), /docker exec/);
context.state.addWorkspace = null;
assert.match(context.agentPrompt("Do work."), /^Read https:/);
context.window.outerLoop = undefined;
assert.equal(context.agentConnection().local, false);
for (const hostname of ["localhost", "127.0.0.1", "[::1]", "app.unix.outerssh.localhost", "app.outerloop.localhost"]) {
  context.window.location.hostname = hostname;
  assert.match(context.agentPrompt("Do work."), /^Read https:/);
}
context.window.location.hostname = "remote.example.com";
assert.match(context.agentPrompt("Do work."), /^Read https:/);
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
assert.doesNotMatch(context.html, /data-tab="catalog"/);
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
for (const runtimeKind of ["docker", "appleContainer", "wslContainer"]) {
  context.state.addWorkspace = {id: "container", name: "Test", runtimeKind};
  context.openAddDialog("catalog");
  assert.equal(context.state.addTab, "agent");
  assert.doesNotMatch(context.html, /data-tab="catalog"|data-catalog-id=|data-action="install"/);
  assert.match(context.html, /Set up OpenCode/);
  assert.match(context.html, /data-tab="dockerfile"/);
  context.state.addWorkspace.ownsContainer = false;
  context.openAddDialog("catalog");
  assert.doesNotMatch(context.html, /data-tab="catalog"|data-tab="dockerfile"/);
}

for (const runtime of ["docker", "container"]) {
  const shellCommand = 'TERM=${TERM:-xterm-256color} COLORTERM=${COLORTERM:-truecolor} COLORFGBG=${COLORFGBG:-} '
    + runtime + " --host unix:///run/docker.sock exec -it --env TERM --env COLORTERM --env COLORFGBG 'my container' /bin/bash";
  const workspace = { name: "My Websites", runtimeKind: runtime, shellCommand };
  assert.equal(context.agentContainerCommand(workspace), runtime + " --host unix:///run/docker.sock exec 'my container' /bin/bash -lc '<command>'");
  assert.equal(context.containerCommands(workspace)[0].text, shellCommand);
}
assert.equal(context.agentContainerCommand({ runtimeKind: "wsl", shellCommand: "wsl -d Ubuntu" }), "wsl -d Ubuntu");
assert.equal(context.agentContainerCommand({ runtimeKind: "docker" }), "");
console.log("PASS: agent commands preserve runtime options and shell without terminal settings; terminal commands unchanged");
