const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const zlib = require('node:zlib');
const root = process.argv[2] || process.cwd();
const web = path.join(root, 'Resources/OuterShellWeb');
const source = fs.readFileSync(path.join(web, 'app.js'), 'utf8');
const template = fs.readFileSync(path.join(web, 'opencode-container.Dockerfile'), 'utf8');
const assets = [...template.matchAll(/__OPENCODE_SHA256_([\w.-]+)__/g)].map(match => ({name:match[1],digest:'sha256:'+'a'.repeat(64)}));
const release = {tag_name:'v1.99.0',assets};
const context = {Blob, Response, CompressionStream, Uint8Array, btoa, AbortSignal,
  fetch: async url => url.startsWith('/api/') ? {ok:true,json:async()=>[release]} : {ok:true,text:async()=>fs.readFileSync(path.join(web,url.slice(5)), 'utf8')}};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('  const openCodeUpdates ='), source.indexOf('  async function pinInstalledOpenCode(')), context);
(async()=>{
  const fragment = await context.openCodeContainerFragment();
  assert.match(fragment, /version=1.99.0/);
  assert(!fragment.includes('__OPENCODE_'));
  const configOf = text => JSON.parse(text.match(/export OPENCODE_CONFIG_CONTENT='([^']+)'/)[1]);
  assert.deepEqual(configOf(fragment), {instructions: ["/usr/local/share/outershell/container-agent.md"]});
  const allowed = await context.openCodeContainerFragment(true);
  assert.deepEqual(configOf(allowed), {instructions: ["/usr/local/share/outershell/container-agent.md"], permission: "allow"});
  assert(!allowed.includes('__OPENCODE_CONFIG__'));
  assert(!fragment.includes('__CONTAINER_') && !fragment.includes('__OUTERSHELL_'));
  const payloads = [...fragment.matchAll(/printf '%s' '([A-Za-z0-9+/=]+)'/g)].map(match => zlib.gunzipSync(Buffer.from(match[1], 'base64')).toString('utf8'));
  assert.equal(payloads.length,3);
  for (const [index,file] of ['container-agent.md','container-outerctl.md','outer-shell-skill.md'].entries()) {
    assert.equal(payloads[index],fs.readFileSync(path.join(web,file),'utf8'));
  }
  assert(Buffer.byteLength(fragment)<14000, 'Keep the embedded recipe below the Apple builder input limit');
  assert.match(fragment,/VOLUME \["\/work", "\/var\/lib\/opencode"\]/);
  assert.match(fragment,/--socket-path \/run\/user\/0\/org.outershell.image.opencode/);
  console.log('PASS: portable container recipe, full instruction payloads, persistent workspace and agent state, socket launcher');
})().catch(error=>{console.error(error);process.exitCode=1});
