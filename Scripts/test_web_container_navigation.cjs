const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const s = fs.readFileSync(require('node:path').join(__dirname, '../Resources/OuterShellWeb/app.js'), 'utf8');
const c = {URL, URLSearchParams, window: {location: {href:'http://localhost/'}, outerLoopPageIconObservationSupported:true},
  safeSpaceAppURL: () => 'http+unix://%2Frun%2Fcontainer.sock/lab?x=1#work'};
vm.createContext(c);
vm.runInContext(s.slice(s.indexOf('  function safeSpaceAppNavigationURL('), s.indexOf('  function safeSpaceAppIsReady(')), c);
const workspace = {name:'Projects'}, app = {displayName:'Untrusted name', iconObservationToken:'token'};
const link = new URL(c.safeSpaceAppNavigationURL(workspace, app));
assert.equal(link.host, 'observe-page-icon');
assert.equal(link.searchParams.get('display'), null);
assert.equal(link.searchParams.get('url'), c.safeSpaceAppURL());
assert.equal(new URL(link.searchParams.get('callback')).searchParams.get('token'), 'token');
assert.equal(c.safeSpaceAppNavigationURL(workspace, {...app, iconURL:'/icon.png'}), c.safeSpaceAppURL());
c.window.outerLoopPageIconObservationSupported = false;
assert.equal(c.safeSpaceAppNavigationURL(workspace, app), c.safeSpaceAppURL());
console.log('PASS container links contain destinations and icon callbacks, without address labels');
