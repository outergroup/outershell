"""Verify installed user tasks exit after five idle minutes and reactivate."""
from pathlib import Path
support = Path(__file__).with_name('test_task_activation.py').read_text().split('try:\n    stop(shell_task)')[0]
exec(compile(support, 'task_activation_support', 'exec'))
watchdog.cancel()
watchdog = threading.Timer(420, lambda: os._exit(124)); watchdog.daemon=True; watchdog.start()
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.WaitNamedPipeW.argtypes = [w.LPCWSTR,w.DWORD]
def exists(pipe):
    if kernel.WaitNamedPipeW(pipe, 1): return True
    error=ctypes.get_last_error()
    assert error in (2,121,231),error
    return error != 2
try:
    request(shell_pipe,'/api/backends');request(files_pipe,'/api/files?path=~')
    assert exists(shell_pipe) and exists(files_pipe)
    print('Both apps running; waiting for their five-minute idle shutdown.',flush=True)
    started=time.monotonic()
    while exists(shell_pipe) or exists(files_pipe):
        assert time.monotonic()-started<360,'Idle tasks did not release their pipes.'
        time.sleep(1)
    print('PASS idle shutdown after',round(time.monotonic()-started,1),'seconds',flush=True)
    request(shell_pipe,'/api/backends');request(files_pipe,'/api/files?path=~')
    print('PASS both idle tasks reactivate through the bridge',flush=True)
finally:
    stop(shell_task);stop(files_task);watchdog.cancel()
