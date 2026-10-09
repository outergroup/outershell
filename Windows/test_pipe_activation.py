"""Verify local named-pipe SCM activation in an isolated Windows test VM.

Run as SYSTEM with the packaged OuterShell.exe path. No system Outer Shell
may already be running. The temporary service is always removed.
"""
import ctypes
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

advapi = ctypes.WinDLL('advapi32', use_last_error=True)
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
advapi.OpenSCManagerW.argtypes = [w.LPCWSTR, w.LPCWSTR, w.DWORD]
advapi.OpenSCManagerW.restype = w.HANDLE
advapi.OpenServiceW.argtypes = [w.HANDLE, w.LPCWSTR, w.DWORD]
advapi.OpenServiceW.restype = w.HANDLE
advapi.QueryServiceStatus.argtypes = [w.HANDLE, ctypes.c_void_p]
advapi.CloseServiceHandle.argtypes = [w.HANDLE]
kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
kernel.CreateFileW.restype = w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.WaitNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD]

exe = Path(sys.argv[1]).resolve()
pipe = subprocess.check_output([str(exe), '--pipe'], text=True).strip()
assert pipe.endswith('.s-1-5-18'), 'Run this isolated test as SYSTEM.'
if kernel.WaitNamedPipeW(pipe, 1) or ctypes.get_last_error() != 2:
    raise RuntimeError('A system Outer Shell or pipe activation trigger already exists.')

name = 'OuterShellActivationTest-' + uuid.uuid4().hex
manager = advapi.OpenSCManagerW(None, None, 1)
assert manager, ctypes.get_last_error()
service = None
created = False

def sc(*args):
    subprocess.run(['sc.exe', *args], check=True, capture_output=True, text=True)

def state():
    status = (w.DWORD * 7)()
    assert advapi.QueryServiceStatus(service, ctypes.byref(status)), ctypes.get_last_error()
    return status[1]

def wait_stopped():
    deadline = time.monotonic() + 15
    while state() != 1:
        assert time.monotonic() < deadline, 'Service did not stop.'
        time.sleep(.05)

with tempfile.TemporaryDirectory(prefix='OuterShell-activation-') as directory:
    try:
        binary = f'"{exe}" --service --service-name {name} --state "{directory}"'
        sc('create', name, 'binPath=', binary, 'start=', 'demand')
        created = True
        service = advapi.OpenServiceW(manager, name, 4)
        assert service, ctypes.get_last_error()
        sc('triggerinfo', name, 'start/namedpipe/' + pipe[9:])
        for trial in range(3):
            assert state() == 1, 'Service must begin stopped.'
            started = time.monotonic()
            errors = []
            while True:
                handle = kernel.CreateFileW(pipe, 0xc0000000, 0, None, 3, 0, None)
                if handle != ctypes.c_void_p(-1).value:
                    kernel.CloseHandle(handle)
                    break
                error = ctypes.get_last_error()
                assert error in (2, 231), error
                errors.append(error)
                assert time.monotonic() - started < 5, 'Pipe activation timed out.'
                time.sleep(.025)
            assert state() == 4, 'Pipe opened without a running service.'
            print(f'Trial {trial + 1}: activated in {time.monotonic() - started:.3f}s; startup errors {errors}', flush=True)
            sc('stop', name)
            wait_stopped()
    finally:
        if service:
            if state() != 1:
                sc('stop', name)
                wait_stopped()
            advapi.CloseServiceHandle(service)
        if created:
            sc('delete', name)
        advapi.CloseServiceHandle(manager)
