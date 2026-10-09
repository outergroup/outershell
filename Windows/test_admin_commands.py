import os,struct,subprocess,json,time
from pathlib import Path
from urllib.parse import urlencode
exe=Path(os.environ['LOCALAPPDATA'])/'OuterShell/OuterShell.exe'
pipe=subprocess.check_output([str(exe),'--pipe'],text=True).strip()
def request(route,method='GET',body=b''):
 with open(pipe,'r+b',buffering=0) as f:
  f.write(f'{method} {route} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(body)}\r\n\r\n'.encode()+body)
  h=b''
  while not h.endswith(b'\r\n\r\n'): h+=f.read(1)
  size=int(next(x.split(b':',1)[1] for x in h.split(b'\r\n') if x.lower().startswith(b'content-length:')))
  data=b''
  while len(data)<size:data+=f.read(size-len(data))
  return int(h.split()[1]),data
def ref(data,o):
 a,n=struct.unpack_from('<II',data,o);return data[a:a+n]
sid=pipe.rsplit('.s-',1)[1];sid='S-'+sid
result=subprocess.run([str(exe),'--apply-admin-command',r'\\.\pipe\org.outershell.admin-apply-test',sid])
assert result.returncode==5,result.returncode
print('PASS apply helper requires elevation',flush=True)
root=Path(os.environ['ProgramW6432'])/'OuterShell Commands'/sid/'commands/org.outershell.command.admin-regression-fixture'
for name in ['run.cmd','runner.exe']:
 try:
  with (root/name).open('ab'):pass
 except PermissionError:pass
 else:raise AssertionError('unelevated write allowed: '+name)
print('PASS protected script and runner deny unelevated writes',flush=True)
ps = r"""$ErrorActionPreference='Stop'
$service=New-Object -ComObject Schedule.Service
$service.Connect()
$root=$service.GetFolder('\')
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$name='OuterShell-Admin-App-'+$sid+'-org.outershell.command.admin-regression-fixture'
$task=$root.GetTask($name)
try { $root.RegisterTask($name,$task.Xml,4,$null,$null,3,$null) | Out-Null; exit 1 }
catch { if ($_.Exception.HResult -eq -2147024891 -or $_.Exception.Message -match 'denied|0x80070005') { exit 0 }; throw }
"""
result=subprocess.run(['powershell.exe','-NoProfile','-Command',ps],capture_output=True)
assert result.returncode==0,(result.stdout,result.stderr)
print('PASS unelevated task-definition changes denied',flush=True)
status,data=request('/api/admin-commands');assert status==200 and json.loads(data)['localSetup']
assert request('/api/admin-commands','POST',b'')[0]==400
print('PASS local capability and rejection of empty setup requests',flush=True)
status,data=request('/api/backends');assert status==200
id='org.outershell.command.admin-regression-fixture'
rows=[ref(data,12+i*8) for i in range(struct.unpack_from('<I',data,8)[0])]
row=next(r for r in rows if ref(r,0).decode()==id and ref(r,32)==b'system')
print('PASS administrator card registration',flush=True)
for op in ['stop','start','restart','stop']:
 status,data=request('/api/control?'+urlencode(dict(scope='system',serviceID=id,operation=op)),'POST')
 assert status==200 and struct.unpack_from('<I',data)[0]==1,(op,status,data)
 time.sleep(.5)
print('PASS unelevated control of approved administrator task',flush=True)
status,data=request('/api/logs?'+urlencode(dict(scope='system',serviceID=id)))
assert status==200 and b'ADMIN_FIXTURE_READY' in ref(data,16),(status,data)
assert b'S-1-16-12288' in ref(data,16),ref(data,16)
print('PASS administrator output logs and high-integrity execution',flush=True)
