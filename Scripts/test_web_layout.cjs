const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(process.argv[2] || process.cwd(), 'Resources/OuterShellWeb/app.js'), 'utf8');
const functions = source.slice(source.indexOf('  function validateLayout'), source.indexOf('  function updatePage()'));
const flush = () => new Promise(resolve => setImmediate(resolve));
let server = {revision: 0n, layout: {}}, outage = false, conflict = false;
function client() {
  const state = {groupPins:{},endpointOrder:{},groupOrder:[],endpointNames:{},layoutReady:false,layoutRevision:0n};
  const errors = [];
  const context = {state, decoder:new TextDecoder(),TextDecoder,TextEncoder,Uint8Array,DataView,JSON,console,AbortController,

    render(){},updateStatus(){},clearRefreshFailure(){},reportRefreshFailure:(key,error)=>errors.push(error.message),toast:message=>errors.push(message)};
  Object.defineProperty(context, "localStorage", {get(){throw new Error("Browser storage must not be accessed");}});
  vm.createContext(context);vm.runInContext(functions,context);
  context.requestBuffer = async (url,options={}) => {
    if(outage) throw Error('offline');
    if(options.method==='POST') {
      const input = context.decodeLayout(options.body.buffer);
      if(conflict) {server={revision:server.revision+1n,layout:{version:1,names:{a:'Other browser'}}};conflict=false;}
      if(input.revision!==server.revision)return {response:{ok:false,status:409},buffer:new ArrayBuffer(0)};
      server={revision:server.revision+1n,layout:input.layout};
    }
    return {response:{ok:true,status:200},buffer:context.encodeLayout(server.layout,server.revision).buffer};
  };
  return {context,state,errors};
}
(async()=>{
  const first=client();
  await first.context.refreshLayout();assert.equal(server.revision,0n);assert(first.state.layoutReady);assert.equal(Object.keys(first.state.endpointNames).length,0);
  assert(first.context.saveLayout({...first.context.currentLayout(),pins:{user:['a']},order:{user:['b']},groups:['root','user'],names:{a:'My app'}}));await flush();assert.equal(server.revision,1n);
  const second=client();await second.context.refreshLayout();assert.equal(second.state.endpointNames.a,'My app');assert.equal(server.revision,1n);
  assert(first.context.saveLayout({...first.context.currentLayout(),names:{a:'Renamed'}}));await flush();assert.equal(server.layout.names.a,'Renamed');await second.context.refreshLayout();assert.equal(second.state.endpointNames.a,'Renamed');
  conflict=true;first.context.saveLayout({...first.context.currentLayout(),names:{a:'Conflicting'}});await flush();assert.equal(first.state.endpointNames.a,'Other browser');assert(first.errors.some(message=>message.includes('another browser')));
  outage=true;first.context.saveLayout({...first.context.currentLayout(),names:{a:'Unsaved'}});await flush();assert.equal(first.state.endpointNames.a,'Other browser');assert.equal(server.layout.names.a,'Other browser');outage=false;
  const old=first.context.encodeLayout({names:{a:'Old'}},1n).buffer;const original=first.context.requestBuffer;first.context.requestBuffer=async()=>({response:{ok:true},buffer:old});await first.context.refreshLayout();assert.equal(first.state.endpointNames.a,'Other browser');first.context.requestBuffer=original;
  console.log('PASS: empty-server defaults, server authority, shared names, conflict recovery, failed-write rollback, stale-read protection, no browser storage access');
})().catch(error=>{console.error(error);process.exitCode=1});
