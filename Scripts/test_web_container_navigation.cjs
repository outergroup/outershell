const fs=require('node:fs'), vm=require('node:vm'), assert=require('node:assert/strict');
const s=fs.readFileSync(require('node:path').join(__dirname, '../Resources/OuterShellWeb/app.js'),'utf8');
const c={URL,URLSearchParams,window:{location:{href:'http://localhost/'},outerLoopPageIconObservationSupported:true,outerLoopFriendlyNavigationSupported:true},safeSpaceAppURL:()=> 'http+unix://%2Frun%2Fcontainer.sock/lab?x=1#work'};
vm.createContext(c); vm.runInContext(s.slice(s.indexOf('  function safeSpaceAppNavigationURL('),s.indexOf('  function safeSpaceAppIsReady(')),c);
const w={name:'Projects'}, a={displayName:'JupyterLab: work',socketPath:'/run/user/0/jupyter',iconObservationToken:'token'};
let u=new URL(c.safeSpaceAppNavigationURL(w,a));
assert.equal(u.host,'observe-page-icon'); assert.equal(u.searchParams.get('display'),'Projects / root / JupyterLab: work'); assert.equal(u.searchParams.get('url'),c.safeSpaceAppURL()); assert.equal(new URL(u.searchParams.get('callback')).searchParams.get('token'),'token');
u=new URL(c.safeSpaceAppNavigationURL(w,{...a,iconURL:'/icon.png'})); assert.equal(u.host,'navigate'); assert.equal(u.searchParams.get('display'),'Projects / root / JupyterLab: work');
// Foundation URLComponents percent-decodes query values without treating '+' as a space.
for (const title of ['JupyterLab: work', 'C++ / notes + examples', '雪 notebook']) {
  for (const iconURL of [undefined, '/icon.png']) {
    const link = new URL(c.safeSpaceAppNavigationURL(w, {...a, displayName: title, iconURL}));
    const fields = Object.fromEntries(link.search.slice(1).split('&').map(pair => {
      const split = pair.indexOf('=');
      return [decodeURIComponent(pair.slice(0, split)), decodeURIComponent(pair.slice(split + 1))];
    }));
    assert.equal(fields.display, `Projects / root / ${title}`);
    assert.equal(fields.url, c.safeSpaceAppURL());
  }
}
c.window.outerLoopPageIconObservationSupported=false; c.window.outerLoopFriendlyNavigationSupported=false; assert.equal(c.safeSpaceAppNavigationURL(w,a),c.safeSpaceAppURL());
console.log('PASS container navigation with icon discovery, known icons, and ordinary browser fallback');
