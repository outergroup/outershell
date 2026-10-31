const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('Resources/OuterShellWeb/app.js','utf8');
let now=1000000000, running=true, installed=true, healthCalls=0, releaseCalls=0, version='1.19.3', fail=false;
const item={identity:'opencode',primary:{frontend:{socketPath:'/run/user/1000/outershell-opencode/opencode.sock'}}};
const storage=new Map();
const context={Date:{now:()=>now},document:{hidden:false},state:{},openCodeItem:()=>installed?item:undefined,
  endpointRunning:()=>running,render:()=>{},escapeHTML:s=>s,URL,AbortSignal,delay:async()=>{},
  localStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,v)},
  fetch:async path=>{
    if(path.includes('health')){healthCalls++;if(fail)throw Error('offline');return {ok:true,json:async()=>({healthy:true,version})};}
    releaseCalls++;return {ok:true,json:async()=>[{tag_name:'v1.19.5'}]};
  }};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('  const openCodeUpdates ='),source.indexOf('  function prepareOpenCodeRelease(')),context);
(async()=>{
  assert(!context.newerOpenCodeVersion('1.19.5','1.19.5'));
  assert(context.newerOpenCodeVersion('1.20.0','1.19.9'));
  assert(!context.newerOpenCodeVersion('2.0.0','1.19.5'));
  await context.checkOpenCodeUpdates();assert.equal(healthCalls,1);assert.equal(releaseCalls,1);
  assert.equal(context.openCodeUpdateVersion(item),'1.19.5');assert.match(context.openCodeUpdateBadge(item),/Update available/);
  await context.checkOpenCodeUpdates();assert.equal(healthCalls,1);
  now+=300001;version='1.19.5';await context.checkOpenCodeUpdates();
  assert.equal(healthCalls,2);assert.equal(releaseCalls,1);assert.equal(context.openCodeUpdateVersion(item),'');
  now+=86400001;await context.checkOpenCodeUpdates();assert.equal(releaseCalls,2);
  context.document.hidden=true;now+=86400001;await context.checkOpenCodeUpdates();assert.equal(releaseCalls,2);
  context.document.hidden=false;running=false;await context.checkOpenCodeUpdates();assert.equal(releaseCalls,2);
  running=true;fail=true;await context.checkOpenCodeUpdates();const failedCount=healthCalls;
  await context.checkOpenCodeUpdates();assert.equal(healthCalls,failedCount);assert.equal(context.openCodeUpdateVersion(item),'');
  now+=3600001;fail=false;await context.checkOpenCodeUpdates();assert.equal(healthCalls,failedCount+1);
  installed=false;await context.checkOpenCodeUpdates();assert.equal(context.openCodeUpdateVersion(item),'');
  assert(!/openCodeItem\(\) \? '<button[^']+Update OpenCode/.test(source));
  assert.match(source,/window.addEventListener\("focus"/);
  assert.match(source,/window.setInterval\(\(\) => \{ void checkOpenCodeUpdates\(\); \}, 60000\)/);
  console.log('PASS: automatic checks, daily release caching, health refresh, upgrade badge removal, hidden/stopped apps, silent retry and major-version filtering');
})().catch(error=>{console.error(error);process.exitCode=1});
