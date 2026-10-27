#!/usr/bin/env python3
"""Exercise the real daemon/provider/helper registry path and reactor responsiveness."""
import concurrent.futures, os, pathlib, socket, struct, subprocess, sys, tempfile, time, uuid
root=pathlib.Path(__file__).resolve().parents[1]
daemon,ctl,helper=map(lambda s:str(pathlib.Path(s).resolve()),sys.argv[1:4])
sys.path.insert(0,str(root.parent/'Plaintext/Helper'))
with tempfile.TemporaryDirectory(prefix='registry-') as temporary:
    work=pathlib.Path(temporary);home=work/'home';(home/'services').mkdir(parents=True)
    sock=work/'api'; log=(work/'daemon.log').open('w+')
    provider=work/'provider';provider.write_bytes((root/'Resources/outershell-container-provider').read_bytes());provider.chmod(0o755)
    env=dict(os.environ,OUTERSHELL_HOME=str(home),OUTERSHELLD_API_SOCKET=str(sock),
        OUTERSHELL_OUTERCTL=ctl,OUTER_SHELL_CONTAINER_PROVIDER=str(provider),
        OUTER_SHELL_DOCKER_HOST='unix:///run/user/'+str(os.getuid())+'/docker.sock',
        PATH=str(work)+':'+os.environ.get('PATH',''),TEST_DIRECTORY=str(work),TEST_HELPER=str(pathlib.Path(sys.argv[4]).resolve()))
    (work/'docker').write_text('#!'+sys.executable+'\n'+'''import os,pathlib,sys,time
root=pathlib.Path(os.environ['TEST_DIRECTORY']);args=sys.argv[1:]
if args[:1]==['--host']:args=args[2:]
with (root/'calls').open('a') as f:f.write(repr(args)+'\\n')
if args[:2]==['container','inspect']:
 assert args[-2]=='--' and args[-1]=='registered-name',args
 if (root/'slow').exists():time.sleep(1)
 if (root/'missing').exists():print('No such container',file=sys.stderr);sys.exit(1)
 identity='b'*64 if (root/'replaced').exists() else 'a'*64
 print(identity+'\\trunning\\ttest-image')
elif args[:2]==['exec','-i']:
 assert args[2]=='a'*64,args
 os.execv(os.environ['TEST_HELPER'],[os.environ['TEST_HELPER'],'serve'])
else:raise AssertionError('Unexpected runtime command '+repr(args))
''');(work/'docker').chmod(0o755)
    process=subprocess.Popen([daemon,'--service-manager','internal','--services-dir',str(home/'services'),'--api-socket-path',str(sock),'--stay-alive'],env=env,stdout=log,stderr=log)
    try:
        for _ in range(100):
            if sock.exists():break
            if process.poll() is not None:raise RuntimeError((work/'daemon.log').read_text())
            time.sleep(.05)
        def outer(*args):return subprocess.check_output([ctl,*args],env=env,stderr=subprocess.STDOUT,timeout=5)
        identifier=str(uuid.uuid4())
        outer('container','upsert','--container',identifier,'--name','Friendly café','--provider','docker','--runtime-name','registered-name')
        def query():
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
                connection.settimeout(10);connection.connect(str(sock));connection.sendall(b'\x02\0\0\0\x22\0')
                def exact(n):
                    out=b''
                    while len(out)<n:
                        part=connection.recv(n-len(out));assert part;out+=part
                    return out
                return exact(struct.unpack('<I',exact(4))[0])
        def string(data,field):
            offset,length=struct.unpack_from('<II',data,field);assert offset>=120 and offset+length<=len(data)
            return data[offset:offset+length].decode()
        data=query();assert data[:4]==b'OSED',(data,(work/'daemon.log').read_text());assert struct.unpack_from('<I',data,12)[0]==1
        assert string(data,48)==identifier;assert string(data,56)=='Friendly café';assert string(data,72)=='a'*64
        assert string(data,80)=='running';assert string(data,104)==''
        (work/'slow').touch()
        with concurrent.futures.ThreadPoolExecutor() as pool:
            result=pool.submit(query);time.sleep(.2)
            start=time.monotonic();outer('container','list');assert time.monotonic()-start<.8,'registry blocked behind runtime query'
            assert result.result()[:4]==b'OSED'
        (work/'slow').unlink()
        file=work/'file';file.write_text('registry-backed read')
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "Plaintext/Helper"))
        from client_binary import run_client
        def read(target):
            return run_client(helper,'read',env=env,target=target,path=str(file))
        target=('docker' if os.getuid()==0 else 'docker-user')+':'+'a'*64
        assert read(target)['contents']=='registry-backed read'
        calls=(work/'calls').read_text();assert "'ps'" not in calls and "'list'" not in calls
        (work/'replaced').touch();assert 'replaced' in read(target)['error']
        (work/'missing').touch();data=query();assert string(data,80)=='absent';assert string(data,104)
        outer('container','remove','--container',identifier);data=query();assert struct.unpack_from('<I',data,12)[0]==0
        assert 'registered' in read(target)['error']
        print('PASS: real registry → asynchronous provider → binary helper; friendly names, no runtime enumeration, responsive reactor, missing/rebuilt/unregistered containers')
    finally:
        process.terminate()
        try:process.wait(timeout=10)
        except subprocess.TimeoutExpired:process.kill();process.wait()
        log.close()
