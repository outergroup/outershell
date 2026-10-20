const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('Resources/OuterShellWeb/app.js', 'utf8');
let workspace, markup, submit, calls = [], fail = false, pending;
const button = {disabled:false}, message = {};
const form = {querySelector: selector => selector === '[type="submit"]' ? button : message,
  addEventListener: (_, callback) => { submit = callback; }};
const dialog = {isConnected:true, querySelector: selector => selector === 'form' ? form : {focus(){}}};
const context = {state:{hostPlatform:'windows'}, findSafeSpace: () => workspace,
  containerPreparing: value => value.busy, managedContainer: value => value.ownsContainer !== false,
  escapeHTML: String, containerDialogHeader: String,
  openDialog: html => {markup = html; button.disabled = false; return dialog;},
  containerOperation: async (operation, values) => {calls.push([operation,values.workspaceID]); if(pending) await pending; if(fail) throw Error('Runtime unavailable');},
  closeDialog: () => calls.push('close'), toast(){}, refreshSafeSpaces: async () => calls.push('refresh')};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('  function openDeleteContainer('),source.indexOf('  function openContainerMenu(')), context);
(async () => {
  workspace = {id:'one',name:'Experiment',runtimeKind:'wslContainer'};
  context.openDeleteContainer('one');
  assert.match(markup,/runtime-managed persistent data/);
  assert.match(markup,/Saved Dockerfiles and mounted folders will remain/);
  assert.deepEqual(calls,[]);
  let resolve;
  pending = new Promise(done => {resolve = done;});
  const first = submit({preventDefault(){}});
  await submit({preventDefault(){}});
  assert.deepEqual(calls,[['delete','one']]);
  resolve(); await first; pending = null;
  assert.deepEqual(calls,[['delete','one'],'close','refresh']);
  calls = []; fail = true;
  context.openDeleteContainer('one'); await submit({preventDefault(){}});
  assert.equal(message.textContent,'Runtime unavailable');
  assert.equal(button.disabled,false); assert(!calls.includes('close'));
  workspace.ownsContainer = false;
  context.openDeleteContainer('one');
  assert.match(markup,/externally managed container and all of its data will remain unchanged/);
  assert.match(markup,/Remove registration/);
  markup = null; workspace.runtimeKind = 'wsl'; context.openDeleteContainer('one'); assert.equal(markup,null);
  workspace.runtimeKind = 'docker'; workspace.busy = true; context.openDeleteContainer('one'); assert.equal(markup,null);
  console.log('PASS: deletion confirmation, scoped request, duplicate prevention, error recovery, attached containers, WSL and busy guards');
})().catch(error => {console.error(error); process.exitCode = 1;});
