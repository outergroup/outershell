"""Run against a local container CLI, e.g. python3 Scripts/test_container_volume_runtime.py container."""
import subprocess,uuid,tempfile,pathlib,sys
runtime=sys.argv[1:]
names=['outershell-test-'+str(uuid.uuid4()) for _ in range(2)]
def run(args):
 r=subprocess.run(runtime+args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 if r.returncode: raise RuntimeError(r.stderr or r.stdout)
 return r.stdout
try:
 with tempfile.TemporaryDirectory(prefix='outershell-volume-') as d:
  for name in names: run(['volume','create',name])
  run(['run','--rm','--mount',f'type=volume,source={names[0]},target=/data','--entrypoint','/bin/sh','alpine:3.22','-ec',"cd /data; printf data > 'case'; printf other > 'CASE'; printf special > 'linux:name'; chmod 751 case; ln -s case symlink; ln case hardlink"])
  run(['run','--rm','--user','0','--mount',f'type=volume,source={names[0]},target=/data,readonly','--mount',f'type=bind,source={d},target=/transfer','--entrypoint','/bin/tar','alpine:3.22','-cpf','/transfer/data.tar','-C','/data','.'])
  run(['run','--rm','--user','0','--mount',f'type=volume,source={names[1]},target=/data','--mount',f'type=bind,source={d},target=/transfer,readonly','--entrypoint','/bin/tar','alpine:3.22','-xpf','/transfer/data.tar','-C','/data'])
  run(['run','--rm','--mount',f'type=volume,source={names[1]},target=/data','--entrypoint','/bin/sh','alpine:3.22','-ec',"cd /data; test \"$(cat case)\" = data; test \"$(cat CASE)\" = other; test \"$(cat linux:name)\" = special; test \"$(readlink symlink)\" = case; test \"$(stat -c %a case)\" = 751; test \"$(stat -c %i case)\" = \"$(stat -c %i hardlink)\""])
 print('PASS: runtime volume tar round-trip preserves symlinks, hardlinks, modes, case, and Linux filenames')
finally:
 for name in names:
  subprocess.run(runtime+['volume','rm',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
