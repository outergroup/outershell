const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('Resources/OuterShellWeb/app.js','utf8');
let backdrop=null,underneath=null,calls=[];
const target=(identity,inside=false)=>({identity,closest:selector=>selector==='.context-menu'&&inside?{}:null});
const context={Date,state:{suppressLaunchUntil:0},elements:{dialogLayer:{querySelector:()=>backdrop,childElementCount:1}},document:{elementFromPoint:()=>{assert.equal(backdrop.style.visibility,'hidden');return underneath;}},itemIdentityFromTarget:node=>node?.identity||'',menuContext:()=> 'shortcut',closeDialog:()=>{calls.push('close');backdrop=null;},cancelLongPress(){},finishEndpointDrag(){},clearPressFeedback(){},openAppMenu:(id,point,kind)=>calls.push({id,point,kind})};
vm.createContext(context);vm.runInContext(source.slice(source.indexOf('  function handleAppContextMenu('),source.indexOf('  document.body.addEventListener("contextmenu", handleAppContextMenu)')),context);
const click = node => {const event={target:node,clientX:125,clientY:240,preventDefault(){this.prevented=true;}};context.handleAppContextMenu(event);return event;};
assert(click(target('first')).prevented);assert.equal(calls[0].id,'first');
backdrop={style:{visibility:''}};underneath=target('second');calls=[];context.state.suppressLaunchUntil=Date.now()+10000;
assert(click(target('')).prevented);assert.equal(calls[0],'close');assert.equal(calls[1].id,'second');assert.equal(calls[1].kind,'shortcut');
backdrop={style:{visibility:''}};calls=[];assert(click(target('first',true)).prevented);assert.equal(calls.length,0);
underneath=null;assert(click(target('')).prevented);assert.deepEqual(calls,['close']);
calls=[];assert(!click(target('')).prevented);assert.equal(calls.length,0);
context.escapeHTML=String;context.endpointRunning=()=>true;context.endpointReady=()=>true;context.backendKey=()=> 'backend';context.navigationURL=()=> '/';context.contextMenuGlyph=()=> '';
vm.runInContext(source.slice(source.indexOf('  function endpointContextMenuHTML('),source.indexOf('  function showContextMenu(')),context);
for(const serviceScope of ['user','system']) {
 const html=context.endpointContextMenuHTML({identity:'app',primary:{backend:{canControl:true,serviceScope,logFiles:['log'],scriptPath:'/script'},frontend:{}}},'primary');
 assert(!html.includes('<h3>'));assert(!html.includes('<section'));assert(html.includes('View Logs'));assert(html.includes('Stop'));assert(html.includes('Copy Script Path'));
}
console.log('PASS: first menu, retargeting, backdrop hit testing, click inside menu, dismissal on blank space, flat app actions');
