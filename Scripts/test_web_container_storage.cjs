const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('Resources/OuterShellWeb/app.js', 'utf8');
let cacheClick, runtimeChange, submit, selected = [], calls = [], fail = false, pending;
const button = { disabled: true }, message = {}, list = {};
const cacheButton = {disabled:true, addEventListener: (_, callback) => {cacheClick=callback;}}, section = {};
const runtime = {value:"docker", addEventListener: (_, callback) => {runtimeChange=callback;}};
const form = {
  elements: {runtimeProviderID:runtime},
  querySelector: selector => selector === '[type="submit"]' ? button : selector === '[data-clean-build-cache]' ? cacheButton : selector === '[data-build-cache]' ? section : selector === '[data-storage-images]' ? list : message,
  querySelectorAll: () => selected,
  addEventListener: (_, callback) => { submit = callback; }
};
const dialog = {isConnected:true, querySelector: () => form};
const storage = {images:[{reference:'outershell/container-base:1', snapshotBytes:1073741824}], orphanSnapshotBytes:2147483648, deleted:[], failures:[], supportsBuildCacheCleanup:true};
const context = {Array, Number, String, state:{providers:[{id:"docker",name:"Docker",capabilities:{supportsStorageCleanup:true}}]}, containerDialogHeader: String, escapeHTML: s => s.replaceAll('<','&lt;'),
  openDialog: () => { button.disabled = true; return dialog; },
  safeSpaceRequest: async (operation, request) => {
    calls.push([operation, request]);
    if (pending) await pending;
    if (fail) throw Error('Image references changed. Refresh storage before cleaning up.');
    return {storage};
  }
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('  async function openContainerStorage('), source.indexOf('  function openDeleteContainer(')), context);
(async () => {
  await context.openContainerStorage();
  assert.match(list.innerHTML, /2.00 GiB/);
  assert.match(list.innerHTML, /1.00 GiB/);
  assert(!list.innerHTML.includes('checked'));
  assert.equal(button.disabled, false);
  await submit({preventDefault(){}});
  assert.equal(calls[1][0], 'cleanContainerStorage');
  assert.equal(calls[1][1].runtimeProviderID,'docker');
  assert.equal(calls[1][1].images.length, 0);
  selected = [{value:'0'}];
  let resolve;
  pending = new Promise(done => { resolve = done; });
  const first = submit({preventDefault(){}});
  await submit({preventDefault(){}});
  assert.equal(calls.length, 3);
  assert.equal(calls[2][1].images[0], 'outershell/container-base:1');
  resolve(); await first; pending = null;
  fail = true;
  await submit({preventDefault(){}});
  assert.match(message.textContent, /references changed/);
  assert.equal(button.disabled, false);
  await context.openContainerStorage();
  assert.equal(button.disabled, true);
  fail = false;
  storage.failures = ['image is now in use'];
  await context.openContainerStorage();
  await submit({preventDefault(){}});
  assert.equal(message.textContent, 'image is now in use');
  storage.failures=[];
  await cacheClick();
  assert.equal(calls.at(-1)[0],'cleanContainerBuildCache');
  assert.equal(calls.at(-1)[1].images.length,0);
  storage.supportsBuildCacheCleanup=false; runtime.value='wsl.container';
  await runtimeChange();
  assert.equal(calls.at(-1)[1].runtimeProviderID,'wsl.container');
  assert.equal(cacheButton.disabled,true); assert.equal(section.hidden,true);
  console.log('PASS: storage estimates, unchecked image selection, cache-only cleanup, scoped selection, duplicate suppression, stale references, inventory failure, and partial failure reporting');
})().catch(error => { console.error(error); process.exitCode = 1; });
