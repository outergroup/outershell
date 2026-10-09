"""Installed Windows task activation integration test; run as the desktop user.

Arguments: installed OuterShell.exe. Leaves both installed tasks enabled and stopped.
"""
import concurrent.futures
import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time

exe = Path(sys.argv[1]).resolve()
bridge = exe.parent/'outer-socket-bridge.exe'
sid = subprocess.check_output(['whoami','/user','/fo','csv','/nh'],text=True).strip().split(',')[-1].strip('"')
shell_pipe = r'\\.\pipe\org.outershell.outershell.'+sid.lower()
files_pipe = r'\\.\pipe\org.outershell.files.'+sid.lower()
shell_task = 'OuterShell-'+sid
files_task = 'OuterShell-Files-'+sid
mapping = Path(os.environ['LOCALAPPDATA'])/'org.outerssh/activations.tsv'
original = mapping.read_bytes()
allow = Path(os.environ['LOCALAPPDATA'])/'OuterSSH/http-pipe.allow'
original_allow = allow.read_bytes()
watchdog = threading.Timer(180, lambda: os._exit(124)); watchdog.daemon=True; watchdog.start()

def task(action, name):
    subprocess.run(['schtasks',action,'/TN',name],check=True,capture_output=True)

def stop(name):
    task('/End',name)
    time.sleep(2)

def receive(p):
    def exact(n):
        result=b''
        while len(result)<n:
            part=p.stdout.read(n-len(result)); assert part, p.stderr.read().decode(errors='replace')
            result+=part
        return result
    magic,version,kind,stream,size=struct.unpack('<IHHII',exact(16))
    assert magic==0x3142524f and version==1
    return kind,stream,exact(size)

def send(p,kind,data=b''):
    p.stdin.write(struct.pack('<IHHII',0x3142524f,1,kind,1,len(data))+data);p.stdin.flush()

def request(pipe, route='/', expected_error=None):
    with subprocess.Popen([str(bridge),'bridge','--socket',pipe],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE) as p:
        assert receive(p)==(1,0,b'')
        send(p,2)
        result=receive(p)
        if expected_error:
            assert result[0]==4 and expected_error in result[2],result
            p.stdin.close();p.wait(timeout=10);return
        assert result[0]==3,result
        send(p,5,f'GET {route} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'.encode())
        data=b''
        while True:
            kind,stream,payload=receive(p)
            if kind==5:data+=payload
            if kind==6:break
        p.stdin.close();p.wait(timeout=10)
        assert data.startswith(b'HTTP/1.1 200'), data[:200]
        return data

try:
    stop(shell_task); stop(files_task)
    started=time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda _:request(shell_pipe,'/api/backends'),range(4)))
    print('PASS concurrent cold Outer Shell activation', round(time.monotonic()-started,3),'seconds',flush=True)
    request(files_pipe,'/api/files?path=~')
    stop(shell_task)
    request(files_pipe,'/api/files?path=~')
    print('PASS independent Files task survives Outer Shell stop',flush=True)
    stop(files_task)
    request(files_pipe,'/api/files?path=~')
    print('PASS Files cold reactivation',flush=True)
    stop(shell_task)
    mapping.write_bytes(original + shell_pipe.upper().encode()+b'\t\\duplicate\n')
    request(shell_pipe,expected_error=b'Duplicate')
    mapping.write_bytes(original + b'invalid\n')
    request(shell_pipe,expected_error=b'Invalid row')
    mapping.write_bytes(original.replace(('\\'+shell_task).encode(),b'\\MissingOuterShellTestTask'))
    request(shell_pipe,expected_error=b'Could not activate task')
    mapping.write_bytes(original)
    allow.write_bytes(b'')
    p=subprocess.run([str(bridge),'bridge','--socket',shell_pipe],input=b'',capture_output=True,timeout=5)
    assert p.returncode and b'allowlist' in p.stderr,p.stderr
    allow.write_bytes(original_allow)
    request(shell_pipe,'/api/backends')
    print('PASS malformed/duplicate/missing task errors, allowlist enforcement, restored activation',flush=True)
finally:
    mapping.write_bytes(original);allow.write_bytes(original_allow)
    stop(shell_task);stop(files_task)
    watchdog.cancel()
