const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync(require('node:path').join(__dirname, '../Resources/OuterShellWeb/app.js'),'utf8');
const code=source.slice(source.indexOf('  async function uploadContainerArchive('),source.indexOf('  function openContainerImport('));
const id='12345678-1234-1234-1234-123456789abc';
(async()=>{
 for(const scenario of ['normal','lost-response','conflict','stalled','cancelled']){
  let offset=0,patches=0,heads=0;const controller=new AbortController();const file=new Blob([new Uint8Array(9*1024*1024+17)]);
  const context={fetch:async(url,req)=>{
   assert(url.endsWith(id));assert.equal(req.headers['Upload-Protocol'],'outershell-resumable-v1');
   if(req.method==='HEAD'){heads++;return new Response(null,{headers:{'Upload-Offset':String(offset)}})}
   patches++;assert(req.body.size<=4*1024*1024);assert.equal(Number(req.headers['Upload-Offset']),offset);
   if(scenario==='conflict'&&patches===1)return new Response(null,{status:409});
   if(scenario!=='stalled')offset+=req.body.size;
   if(scenario==='lost-response'&&patches===1)throw new TypeError('connection lost after write');
   return new Response(null,{headers:{'Upload-Offset':String(offset)}});
  }};vm.createContext(context);vm.runInContext(code,context);
  if(scenario==='cancelled')controller.abort();
  const promise=context.uploadContainerArchive(file,id,()=>{},controller.signal);
  if(['stalled','cancelled'].includes(scenario))await assert.rejects(promise);
  else{await promise;assert.equal(offset,file.size)}
  console.log('PASS:',scenario, 'PATCH requests:',patches);
 }
})().catch(e=>{console.error(e);process.exitCode=1});
