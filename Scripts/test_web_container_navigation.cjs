const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const s = fs.readFileSync(require('node:path').join(__dirname, '../Resources/OuterShellWeb/app.js'), 'utf8');
assert.ok(!s.includes('outerloop:'), 'the launcher navigates directly to apps');
const c = {encodeURIComponent, pathAndQuery: () => '/lab?x=1#work'};
vm.createContext(c);
vm.runInContext(s.slice(s.indexOf('  function safeSpacePathAndQuery('), s.indexOf('  function safeSpaceAppIsReady(')), c);
assert.equal(c.safeSpaceAppURL({externalSocketPath: '/run/container.sock', displayName: 'Untrusted name'}),
  'http+unix://%2Frun%2Fcontainer.sock/lab?x=1#work');
assert.equal(c.safeSpaceAppURL({externalSocketPath: '\\\\.\\pipe\\container', publishedPort: 0}),
  'http+pipe://%5C%5C.%5Cpipe%5Ccontainer/lab?x=1#work');
assert.equal(c.safeSpaceAppURL({externalSocketPath: '/run/container.sock', publishedPort: 8080}),
  'http://127.0.0.1:8080/lab?x=1#work');
assert.equal(c.safeSpaceAppURL({}), '#');
console.log('PASS container links navigate directly to their destinations, without address labels');
