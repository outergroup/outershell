const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const root = process.argv[2] || process.cwd();
const source = fs.readFileSync(path.join(root, "Resources/OuterShellWeb/app.js"), "utf8");
const functions = source.slice(source.indexOf("  const agentExamples ="), source.indexOf("  function renderOverview(entries)"));
const quote = source.slice(source.indexOf("  function shellQuotedArgument("), source.indexOf("  function containerCommandVariants("));
const context = { window: {}, launcherItems: () => [], TextEncoder, btoa };
vm.createContext(context); vm.runInContext(quote + functions, context);
context.window.outerLoop = {sessionContext: {sshCommandArguments: ["ssh", "-p", "2222", "-i", "/keys/my key", "me@host"]}};
assert.equal(context.agentConnection().command, "ssh -p 2222 -i '/keys/my key' me@host");
const prompt = context.agentPrompt("Install Jupyter.");
assert.match(prompt, /ssh -p 2222/); assert.match(prompt, /https:\/\/outershell.org\/api\//); assert.match(prompt, /Install Jupyter/);
context.window.outerLoop.sessionContext.sshCommandArguments = [];
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
