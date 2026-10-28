const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('Resources/OuterShellWeb/app.js', 'utf8');
const editor = source.slice(source.indexOf('  async function openContainerConfiguration('), source.indexOf('  function openShareContainer('));
function setup() {
  let resolve, reject;
  const response = new Promise((yes, no) => { resolve = yes; reject = no; });
  const dialogs = [], requests = [];
  const node = () => ({textContent: '', addEventListener() {}, append(child) { this.child = child; }});
  const form = {addEventListener() {}, querySelector: node};
  const context = {
    findSafeSpace: () => ({id: 'selected', name: 'Selected', ownsContainer: true}),
    managedContainer: value => value.ownsContainer !== false,
    containerDialogHeader: title => title,
    containerAppCatalog: [], escapeHTML: String,
    safeSpaceRuntimeDescription: () => 'Apple container', runtimeOptions: () => '',
    document: {createElement: node},
    safeSpaceRequest: (operation, values) => { requests.push({operation, values}); return response; },
    openDialog: markup => {
      dialogs.forEach(dialog => { dialog.isConnected = false; });
      const status = node(), body = node();
      const dialog = {markup, status, body, isConnected: true, querySelector: selector => selector === 'form' ? form : selector === '.dialog-body' ? body : status};
      dialogs.push(dialog);
      return dialog;
    }
  };
  vm.createContext(context);
  vm.runInContext(editor, context);
  return {context, dialogs, requests, resolve, reject};
}
(async () => {
  const result = {workspaces: [{id: 'selected', name: 'Fresh name', ownsContainer: true, recipe: {containerfile: 'FROM fresh'}, mounts: []}]};
  const ready = setup();
  const pending = ready.context.openContainerConfiguration('selected');
  assert.equal(ready.dialogs.length, 1);
  assert.match(ready.dialogs[0].markup, /Loading configuration/);
  assert.equal(ready.requests[0].values.workspaceID, 'selected');
  assert.equal(ready.requests[0].values.configurationOnly, true);
  ready.resolve(result);
  await pending;
  assert.equal(ready.dialogs.length, 2);
  assert.match(ready.dialogs[1].markup, /Fresh name/);
  assert.match(ready.dialogs[1].markup, /FROM fresh/);
  const closed = setup();
  const abandoned = closed.context.openContainerConfiguration('selected');
  closed.dialogs[0].isConnected = false;
  closed.resolve(result);
  await abandoned;
  assert.equal(closed.dialogs.length, 1);
  const failed = setup();
  const failure = failed.context.openContainerConfiguration('selected');
  failed.reject(new Error('Read failed'));
  await failure;
  assert.equal(failed.dialogs[0].status.textContent, 'Read failed');
  assert.equal(failed.dialogs[0].body.child.textContent, 'Retry');
  const missing = setup();
  const absent = missing.context.openContainerConfiguration('selected');
  missing.resolve({workspaces: []});
  await absent;
  assert.match(missing.dialogs[0].status.textContent, /not available/);
  console.log('PASS: editor opens before I/O, requests selected configuration, uses fresh data, ignores dismissed loads, and offers retry on failure');
})().catch(error => { console.error(error); process.exitCode = 1; });
