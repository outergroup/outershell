import runpy,tempfile,pathlib,subprocess
from unittest.mock import patch,Mock
p=runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))
f=p['ensure_container_socket_bridge']
with tempfile.TemporaryDirectory() as d:
 root=pathlib.Path(d);src=root/'bin/glibc/aarch64/outer-socket-bridge';src.parent.mkdir(parents=True);src.write_bytes(b'bridge')
 calls=[]
 def execute(cid,args,**kw):
  calls.append((args,kw))
  if len(calls)==1:return subprocess.CompletedProcess(args,127,'','missing loader')
  if len(calls)==2:return subprocess.CompletedProcess(args,0,'glibc/aarch64','')
  return subprocess.CompletedProcess(args,0,'outer-socket-bridge 1','')
 copy=Mock()
 with patch.dict(f.__globals__,container_exec=execute,provider_resources=lambda:root,docker=copy,container_name=lambda _: 'test'):
  f('test')
 assert calls[0][0]==['/usr/local/bin/outer-socket-bridge','--version']
 assert calls[-1]==(['/usr/local/bin/outer-socket-bridge','--version'],{'check':True})
 assert copy.call_count==1
 print('PASS: unusable executable is replaced and the replacement is executed')
 good=Mock(return_value=subprocess.CompletedProcess([],0,'outer-socket-bridge 1',''));copy.reset_mock()
 with patch.dict(f.__globals__,container_exec=good,docker=copy):f('test')
 assert good.call_count==1 and copy.call_count==0
 print('PASS: working bridge is preserved')
obj=object.__new__(p['ContainerSocketBridge']);obj.stopped=Mock();obj.stopped.wait.side_effect=[False,False,True];obj.send=Mock();obj.keep_alive()
assert obj.send.call_count==2 and all(c.args==(p['FRAME_PING'],0) for c in obj.send.call_args_list)
print('PASS: idle bridge sends periodic keepalives and stops when cancelled')
