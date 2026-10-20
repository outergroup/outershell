const fs = require("node:fs"), vm = require("node:vm"), assert = require("node:assert/strict");
const source = fs.readFileSync(require("node:path").join(__dirname, "../Resources/OuterShellWeb/app.js"), "utf8");
const code = source.slice(source.indexOf("  function openShareContainer("), source.indexOf("  function openContainerMenu("));
(async () => {
  let submit, link, clicked = false;
  const message = {}, button = {}, output = { replaceChildren(value) { link = value; } };
  const form = {
    elements: { persistent: { checked: true }, mounts: { checked: false } },
    querySelector(selector) { return selector === '[type="submit"]' ? button : selector === '.container-message' ? message : output; },
    addEventListener(event, handler) { assert.equal(event, "submit"); submit = handler; }
  };
  const context = {
    findSafeSpace: () => ({ name: "Experiment" }), managedContainer: () => true,
    escapeHTML: value => value, containerDialogHeader: value => value,
    openDialog(html) { assert(!html.includes("browser memory")); return { querySelector: () => form }; },
    containerOperation: async (operation, request) => {
      assert.equal(operation, "prepareShare"); assert.equal(request.includePersistentData, true);
      return { transferID: "12345678-1234-1234-1234-123456789abc", fileName: "Experiment.outershell-container", byteCount: 8 * 1024 ** 3 };
    },
    document: { createElement(tag) { assert.equal(tag, "a"); return { click() { clicked = true; } }; } },
    fetch() { throw new Error("Page must not fetch archive bytes"); }
  };
  vm.createContext(context); vm.runInContext(code, context);
  context.openShareContainer("workspace");
  await submit({ preventDefault() {} });
  assert(clicked); assert.equal(button.disabled, false);
  assert.equal(link.href, "/api/container-downloads/12345678-1234-1234-1234-123456789abc/Experiment.outershell-container");
  assert.equal(link.download, "Experiment.outershell-container");
  assert.equal(message.textContent, "Archive ready.");
  console.log("PASS: large archives use a browser download link without fetching or buffering bytes in the page");
})().catch(error => { console.error(error); process.exitCode = 1; });
