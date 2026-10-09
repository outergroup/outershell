"""Windows command-app lifecycle test; run with no same-user Outer Shell active."""
from pathlib import Path
support = Path(__file__).with_name('test_host.py').read_text().split('with tempfile.TemporaryDirectory')[0]
exec(compile(support, 'test_host_support', 'exec'))
from urllib.parse import urlencode
import ctypes
import uuid
import socket
import urllib.request
watchdog.cancel()
watchdog = threading.Timer(240, fail_timeout); watchdog.daemon = True; watchdog.start()

fixture_source = r'''
import ctypes, os, subprocess, sys, time
from ctypes import wintypes
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.CreateNamedPipeW.restype = wintypes.HANDLE
k.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
k.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
k.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
k.WriteFile.argtypes = k.ReadFile.argtypes
k.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
k.CloseHandle.argtypes = [wintypes.HANDLE]
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])
print('command fixture started', flush=True)
while True:
 h = k.CreateNamedPipeW(sys.argv[1], 3, 0, 1, 65536, 65536, 0, None)
 assert h != ctypes.c_void_p(-1).value
 k.ConnectNamedPipe(h, None)
 data = ctypes.create_string_buffer(65536); count = wintypes.DWORD()
 k.ReadFile(h, data, 65536, ctypes.byref(count), None)
 body = (str(child.pid)+' '+sys.argv[2]).encode('utf-8')
 reply = b'HTTP/1.1 200 OK\r\nContent-Length: '+str(len(body)).encode()+b'\r\nConnection: close\r\n\r\n'+body
 k.WriteFile(h, reply, len(reply), ctypes.byref(count), None)
 time.sleep(.1)
 k.DisconnectNamedPipe(h); k.CloseHandle(h)
'''

def app_control(id, operation):
 status, data = request(pipe, '/api/control?' + urlencode(dict(serviceID=id, operation=operation)), method='POST')
 assert status == 200 and struct.unpack_from('<I', data)[0] & 1, (status, data)

def app_status(id):
 data = request(pipe, '/api/backends')[1]
 for i in range(struct.unpack_from('<I', data, 8)[0]):
  row = ref(data, 12+i*8)
  if ref(row, 0).decode() == id: return ref(row,40).decode()
 return None

with tempfile.TemporaryDirectory(prefix='outer-command-雪-') as temporary:
 state = Path(temporary)/'state'
 fixture = Path(temporary)/'pipe fixture.py'; fixture.write_text(fixture_source, encoding='utf-8')
 allow = Path(os.environ['LOCALAPPDATA'])/'OuterSSH/http-pipe.allow'
 original = allow.read_bytes() if allow.exists() else None
 activation = Path(os.environ['LOCALAPPDATA'])/'org.outerssh/activations.tsv'
 original_activation = activation.read_bytes() if activation.exists() else None
 name = 'test-'+uuid.uuid4().hex
 endpoint = '\\\\.\\pipe\\'+name
 service = 'org.outershell.command.'+name
 command = f'echo Unicode 雪\n"{sys.executable}" "{fixture}" "{endpoint}" "hello 雪"'
 values = dict(recipe='command-port', identifier=name, name='Command test 雪', command=command, frontendTransport='namedPipe', socketPath=endpoint, scope='user')
 def create(**overrides):
  return request(pipe, '/api/create', urlencode(values | overrides).encode(), 'POST', b'Content-Type: application/x-www-form-urlencoded\r\n')
 def launch():
  p = subprocess.Popen([str(exe), '--state', str(state)], stdout=subprocess.PIPE, stderr=subprocess.PIPE); children.append(p)
  assert request(pipe, '/api/backends')[0] == 200
  return p
 process = launch()
 try:
  assert request(pipe, '/api/create')[0] == 405
  for invalid in (dict(scope='system'), dict(identifier='../bad'), dict(socketPath='\\\\remote\\pipe\\test'), dict(socketPath='\\\\.\\pipe\\bad\nentry')):
   assert struct.unpack_from('<I', create(**invalid)[1])[0] == 0
  status, data = create(); assert status == 200 and struct.unpack_from('<I',data)[0] == 1, (status,data)
  assert struct.unpack_from('<I', create()[1])[0] == 0
  assert activation.read_bytes() == original_activation, 'Commands must not install activation mappings'
  status, body = request(endpoint, '/'); assert status == 200 and body.endswith('hello 雪'.encode()), body
  assert app_status(service) == 'running'
  assert endpoint in allow.read_text()
  assert 'Unicode 雪'.encode() in ref(request(pipe, '/api/logs?'+urlencode(dict(serviceID=service)))[1],16)
  child_id = int(body.split()[0])
  k=ctypes.WinDLL('kernel32',use_last_error=True); k.OpenProcess.restype=ctypes.c_void_p
  k.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_ulong]; k.CloseHandle.argtypes=[ctypes.c_void_p]
  handle=k.OpenProcess(0x100000,False,child_id); assert handle
  app_control(service,'stop'); assert app_status(service)=='stopped'
  assert k.WaitForSingleObject(handle,5000)==0; k.CloseHandle(handle)
  process.kill(); process.wait(); time.sleep(.3); process=launch()
  assert app_status(service)=='stopped'
  app_control(service,'start'); assert request(endpoint,'/')[0]==200
  old_reply = request(endpoint,'/')[1]
  process.kill(); process.wait(); time.sleep(.3)
  assert request(endpoint,'/')[1] == old_reply, 'Command app did not survive Outer Shell exit'
  process=launch()
  assert request(endpoint,'/')[1] == old_reply
  app_control(service,'restart'); assert request(endpoint,'/')[0]==200
  app_control(service,'uninstall'); assert app_status(service) is None
  assert endpoint not in allow.read_text()
  assert not (state/'commands'/service).exists()
  with allow.open('a') as f: f.write(endpoint+'\n')
  assert struct.unpack_from('<I',create()[1])[0]==1
  app_control(service,'uninstall'); assert endpoint in allow.read_text()
  assert struct.unpack_from('<I',create(command='echo Exiting normally\nexit /b 0')[1])[0]==1
  deadline=time.monotonic()+10
  while app_status(service) != 'stopped' and time.monotonic()<deadline: time.sleep(.2)
  assert app_status(service)=='stopped'
  app_control(service,'uninstall'); assert app_status(service) is None
  assert not (state/'commands'/service).exists()
  with socket.socket() as reservation:
   reservation.bind(('127.0.0.1',0)); port=reservation.getsockname()[1]
  for invalid_port in ('', '0', '65536', '-1', '6006x', ' 6006'):
   assert struct.unpack_from('<I',create(frontendTransport='port',port=invalid_port)[1])[0]==0
  port_command=f'"{sys.executable}" -m http.server {port} --bind 127.0.0.1 --directory "{temporary}"'
  assert struct.unpack_from('<I',create(frontendTransport='port',port=str(port),command=port_command)[1])[0]==1
  def fetch_port():
   deadline=time.monotonic()+10
   while True:
    try:
     with urllib.request.urlopen(f'http://127.0.0.1:{port}/',timeout=1) as response: return response.read()
    except OSError:
     if time.monotonic()>=deadline: raise
     time.sleep(.1)
  assert b'Directory listing' in fetch_port()
  data=request(pipe,'/api/backends')[1]
  row=next(ref(data,12+i*8) for i in range(struct.unpack_from('<I',data,8)[0]) if ref(ref(data,12+i*8),0).decode()==service)
  assert f'127.0.0.1:{port}'.encode() in data
  assert activation.read_bytes()==original_activation
  app_control(service,'stop'); assert app_status(service)=='stopped'
  with socket.socket() as probe: assert probe.connect_ex(('127.0.0.1',port))!=0
  app_control(service,'start'); assert b'Directory listing' in fetch_port()
  app_control(service,'restart'); assert b'Directory listing' in fetch_port()
  app_control(service,'uninstall'); assert app_status(service) is None
  assert activation.read_bytes()==original_activation
  print('PASS manual TCP command: port validation, HTTP, stop/start/restart/uninstall, no activation mapping',flush=True)
  print('PASS uninstall after normal command exit',flush=True)
  print('PASS command app: UTF-8 batch and HTTP pipe, validation, duplicate rejection, logs, process-tree stop, enabled persistence, independence from Outer Shell, restart, uninstall, allowlist ownership',flush=True)
 finally:
  sid = subprocess.check_output(['whoami','/user','/fo','csv','/nh'],text=True).strip().split(',')[-1].strip('"')
  task_name = 'OuterShell-App-'+sid+'-'+service
  subprocess.run(['schtasks','/End','/TN',task_name],capture_output=True)
  subprocess.run(['schtasks','/Delete','/TN',task_name,'/F'],capture_output=True)
  if process.poll() is None: process.kill()
  process.wait(); time.sleep(.3)
  if original_activation is None: activation.unlink(missing_ok=True)
  else: activation.write_bytes(original_activation)
  if original is None: allow.unlink(missing_ok=True)
  else: allow.write_bytes(original)
watchdog.cancel()
