from pathlib import Path
import subprocess,tempfile,time,socket,http.client,sys
root=Path(__file__).resolve().parents[2]
for repo,name in [('Top','Top'),('Files','Files'),('Plaintext','Plaintext'),('Profile','Profile'),('Trace','Firehose')]:
 p=root/repo; png=(p/'app-icon.png').read_bytes()
 assert png==(root/f'Backends/Resources/OuterShellWeb/icons/org.outershell.{name}.png').read_bytes()
 assert png==(p/f'Resources/{name}Web/app-icon.png').read_bytes()
 with tempfile.TemporaryDirectory(prefix='icon-') as d:
  sock=Path(d)/'s'; log=open(Path(d)/'log','w+')
  proc=subprocess.Popen([str(Path(sys.argv[1])/repo),'--socket-path',str(sock),'--web-root',str(p/f'Resources/{name}Web')],stdout=log,stderr=log,cwd=p)
  try:
   for _ in range(100):
    if sock.exists():break
    if proc.poll() is not None: log.seek(0);raise RuntimeError(log.read())
    time.sleep(.05)
   def fetch(path,method='GET',headers={}):
    c=http.client.HTTPConnection('localhost');c.sock=socket.socket(socket.AF_UNIX);c.sock.settimeout(5);c.sock.connect(str(sock));c.request(method,path,headers=headers);r=c.getresponse();result=(r.status,dict(r.getheaders()),r.read());c.close();return result
   status,headers,html=fetch('/')
   assert status==200 and b'href="/web/app-icon.png"' in html,(repo,status)
   for path in ['/web/app-icon.png','/favicon.ico','/web/app-icon.png?v=1']:
    status,headers,body=fetch(path);assert status==200 and body==png,(repo,path,status,len(body))
    assert headers['Content-Type']=='image/png'
    assert fetch(path,'HEAD')[2]==b''
   print('PASS',name,'HTML favicon, fallback, query string, HEAD, exact canonical PNG',flush=True)
  finally:
   proc.terminate()
   try:proc.wait(timeout=3)
   except subprocess.TimeoutExpired:proc.kill();proc.wait()
