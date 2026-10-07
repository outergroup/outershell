const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "../Resources/OuterShellWeb/app.js"), "utf8");
let html = "";
const context = { Uint8Array, TextDecoder, decoder: new TextDecoder(), btoa,
  state: { addTab: "catalog", addScope: "user", backends: [] },
  openDialog: value => { html = value; }
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf("  class PayloadReader"), source.indexOf("  function safeSpaceAppIconHTML")), context);
vm.runInContext(source.slice(source.indexOf("  function openAddDialog("), source.indexOf("  function wireBashForm(")), context);
for (const name of ["Top", "Files", "Plaintext", "Profile", "Firehose"]) {
  const service = "org.outershell." + name;
  const expected = fs.readFileSync(path.join(__dirname, "../Resources/OuterShellWeb/icons", service + ".png"));
  for (const installed of [0, 1]) {
    context.payload = new Uint8Array(fs.readFileSync(path.join(process.argv[2], service + "-" + installed + ".bin")));
    vm.runInContext("state.backends = [decodeBackend(new PayloadReader(payload))]", context);
    const backend = context.state.backends[0];
    assert.equal(backend.isInstalled, Boolean(installed));
    assert.deepEqual(Buffer.from(backend.iconData), expected);
    context.openAddDialog();
    assert.ok(html.includes("data:image/png;base64," + expected.toString("base64")));
    for (const render of [context.iconHTML, context.launcherIconHTML, context.listIconHTML]) {
      const rendered = render({ backend, displayName: name, frontend: { iconURL: "wrong.png" } });
      assert.ok(rendered.includes(expected.toString("base64")));
      assert.ok(!rendered.includes("wrong.png"));
    }
  }
}
console.log("PASS: five apps, before/after install, HTML catalog and both home layouts use canonical icons");
