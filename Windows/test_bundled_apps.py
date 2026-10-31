import os,sys,subprocess,tempfile,time,struct
from pathlib import Path
root=Path(sys.argv[1]); children=[]
base_pipe=subprocess.check_output([str(root/'OuterShell.exe'),'--pipe'],text=True).strip()
suffix=base_pipe.split('org.outershell.outershell.',1)[1]
try:
 with open(base_pipe,'r+b',buffering=0):pass
except OSError as error:
 if getattr(error,'winerror',None) not in (None,2,3):raise
else:raise RuntimeError('Stop the existing same-user Outer Shell before running this isolated test.')
u32=lambda b,o:struct.unpack_from('<I',b,o)[0]
def field(b,o):
 a,n=struct.unpack_from('<II',b,o);assert a<=len(b) and n<=len(b)-a;return b[a:a+n]
def call(app,path,body=None,accept="*/*"):
 pipe='\\\\.\\pipe\\org.outershell.'+app.lower()+'.'+suffix
 end=time.monotonic()+10
 while True:
  try:f=open(pipe,'r+b',buffering=0);break
  except OSError:
   if time.monotonic()>end:raise
   time.sleep(.05)
 with f:
  content=body or b'';method='POST' if body is not None else 'GET'
  f.write(f'{method} {path} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(content)}\r\nContent-Type: application/octet-stream\r\nOuterframe-Accept: {accept}\r\nX-Environment-Request: 1\r\n\r\n'.encode()+content)
  h=b''
  while not h.endswith(b'\r\n\r\n'):
   b=f.read(1);assert b,h;h+=b
  fields=dict(line.lower().split(b': ',1) for line in h.split(b'\r\n')[1:-2]);n=int(fields[b'content-length']);result=b''
  while len(result)<n:
   b=f.read(n-len(result));assert b;result+=b
  assert h.split()[1]==b'200',(h,result)
  return result

def env(app,op,path='',target='host',destination='',revision='',text=''):
 body=bytearray(72);struct.pack_into('<IIII',body,0,0x51434c46 if app=='Files' else 0x51434c50,1,0,op)
 for i,s in enumerate([target,'',path,destination,revision,text]):
  b=s.encode();struct.pack_into('<II',body,24+i*8,len(body),len(b));body+=b
 struct.pack_into('<I',body,8,len(body));reply=call(app,'/api/environments',body)
 assert u32(reply,8)==len(reply);return reply

def ok(b):
 assert u32(b,12)==0,field(b,32);return field(b,16),field(b,24)
try:
 with tempfile.TemporaryDirectory() as work:
  children.append(subprocess.Popen([root/'OuterShell.exe','--state',str(Path(work)/'shell')]))
  for app in ['Files','Plaintext','Top','Firehose','Profile']:children.append(subprocess.Popen([root/(app+'Backend.exe')]))
  for app in ['Files','Plaintext','Top']:
   descriptor=call(app,'/',accept='application/vnd.outerframe');assert descriptor[:4]==b'OUTR'
   offset,length=struct.unpack_from('<QQ',descriptor,8);url=descriptor[offset:offset+length].decode()
   assert b'macos-arm' in call(app,url)
   assert len(call(app,url+'/macos-arm'))>10000
  p=Path(work)/'Folder 雪 & +';p.mkdir();file=p/'hello.txt';file.write_text('hello',encoding='utf8')
  ok(env('Files',7));m,b=ok(env('Files',1,str(p)));assert u32(m,20)==1
  m,b=ok(env('Files',8,str(file)));assert b==b'hello'
  m,b=ok(env('Files',10));assert u32(m,20)>=1
  ok(env('Files',11,str(p)));ok(env('Files',11,str(p)))
  m,b=ok(env('Files',10));assert sum(field(m,64+i*48+8).decode().replace('\\','/').lower()==str(p).replace('\\','/').lower() for i in range(u32(m,20)))==1
  m,b=ok(env('Plaintext',2,str(file)));assert b==b'hello';revision=field(m,56).decode()
  ok(env('Plaintext',3,str(file),revision=revision,text='new contents'))
  assert file.read_text()=='new contents'
  assert u32(env('Plaintext',3,str(file),revision=revision,text='stale'),12)==1
  assert file.read_text()=='new contents'
  m,b=ok(env('Plaintext',11,str(file)));token=field(env('Plaintext',11,str(file)),32).decode().split(':')[0];revision=field(m,56).decode()
  ok(env('Plaintext',12,str(file),destination=token,revision=revision,text='draft contents'))
  m,b=ok(env('Plaintext',11,str(file)));assert b==b'draft contents'
  assert file.read_text()=='new contents'
  assert u32(env('Files',1,str(p),target='docker:missing'),12)==1
  top=call('Top','/api/environments?operation=processes');assert top[:4]==b'OETP'
  for app in ['Firehose','Profile']:assert b'Choose a container' in call(app,'/')
  print('PASS native Files list/preview/places, Plaintext read/save/conflicts/shared drafts, container rejection, Top discovery, tracing pickers',flush=True)
finally:
 for p in children:
  if p.poll() is None:p.terminate()
 for p in children:p.wait(timeout=10)
