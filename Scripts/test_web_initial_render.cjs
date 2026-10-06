const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(process.argv[2] || process.cwd(), 'Resources/OuterShellWeb/app.js'), 'utf8');
const gate = source.slice(source.indexOf('  function renderInitialLoading()'), source.indexOf('  function renderOverview(entries)'));
function page() {
  const state = {layoutReady:false, backendsReady:false, safeSpacesReady:false, overviewReady:false, showInitialLoading:false};
  const elements = {overview:{innerHTML:''},shell:{setAttribute(key, value){this[key]=value;}}};
  const context = {state, elements, window:{clearTimeout(){}}};
  vm.createContext(context); vm.runInContext(gate, context);
  return {...context, render:context.renderInitialLoading};
}
const keys = ['layoutReady', 'backendsReady', 'safeSpacesReady'];
for (const first of keys) for (const second of keys.filter(key => key !== first)) {
  const last = keys.find(key => key !== first && key !== second);
  const p = page();
  assert(p.render()); assert.equal(p.elements.overview.innerHTML, '');
  for (const key of [first, second, last]) {
    p.state[key] = true;
    assert.equal(p.render(), !(p.state.layoutReady && p.state.backendsReady));
    if (p.state.overviewReady) assert.equal(p.elements.shell['aria-busy'], 'false');
  }
}
const p = page(); p.state.showInitialLoading = true;
assert(p.render()); const loading = p.elements.overview.innerHTML;
assert.match(loading, /role="status">Loading…/);
p.state.layoutReady = p.state.backendsReady = true;
assert.equal(p.render(), false);
assert.equal(p.state.safeSpacesReady, false);
p.state.safeSpacesError = 'Container discovery failed';
assert.equal(p.render(), false);
p.state.safeSpacesReady = true; assert.equal(p.render(), false);
p.elements.overview.innerHTML = 'Rendered cards';
p.state.loading = p.state.safeSpacesLoading = true;
assert.equal(p.render(), false); assert.equal(p.elements.overview.innerHTML, 'Rendered cards');
console.log('PASS: all response orders, blank fast loads, one delayed loading state, unavailable container discovery, stable refresh rendering');
