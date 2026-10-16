"""Run from a Mac/Linux client against a Windows test host with both Linux runtimes.

Requires outershell/windows-test:1 in both runtimes. Creates and removes disposable containers.
"""
import argparse
import json
import os
import pathlib
import selectors
import struct
import subprocess
import time
import sys
import tempfile
parser = argparse.ArgumentParser()
parser.add_argument('host')
parser.add_argument('bridge')
parser.add_argument('shell_pipe')
parser.add_argument('--identity')
options = parser.parse_args()
ssh = ['ssh', '-T', '-o', 'ConnectTimeout=10']
if options.identity:
    ssh += ['-i', options.identity, '-o', 'IdentitiesOnly=yes']
ssh += [options.host]
shell = options.shell_pipe

class Bridge:
    def __init__(self, pipe):
        command = f'"{options.bridge}" bridge --socket {pipe}'
        self.process = subprocess.Popen(ssh + [command], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = bytearray()
        self.stream = 0
        assert self.frame()[0] == 1
    def read(self, length):
        deadline = time.monotonic() + 45
        while len(self.buffer) < length:
            ready = self.selector.select(max(0, deadline - time.monotonic()))
            if not ready: raise TimeoutError('Bridge read timed out')
            block = os.read(self.process.stdout.fileno(), 65536)
            if not block:
                self.process.kill()
                _, error = self.process.communicate(timeout=5)
                raise RuntimeError('Bridge closed: ' + error.decode(errors='replace'))
            self.buffer += block
        result = bytes(self.buffer[:length]); del self.buffer[:length]; return result
    def frame(self):
        magic, version, kind, stream, length = struct.unpack('<IHHII', self.read(16))
        assert magic == 0x3142524f and version == 1 and length <= 1024 * 1024
        return kind, stream, self.read(length)
    def send(self, kind, body=b''):
        self.process.stdin.write(struct.pack('<IHHII', 0x3142524f, 1, kind, self.stream, len(body)) + body)
        self.process.stdin.flush()
    def request(self, route, body=b'', method='GET', native=False, headers=None):
        self.stream += 1
        self.send(2)
        while True:
            kind, stream, data = self.frame()
            if stream == self.stream: break
        assert kind == 3, (kind, data)
        accept = 'Outerframe-Accept: application/vnd.outerframe\r\n' if native else ''
        extra = ''.join(f'{key}: {value}\r\n' for key,value in (headers or {}).items())
        request = f'{method} {route} HTTP/1.1\r\nHost: localhost\r\n{accept}{extra}Accept: application/vnd.outerframe, text/html\r\nConnection: close\r\nContent-Length: {len(body)}\r\n\r\n'.encode() + body
        for offset in range(0, len(request), 32768): self.send(5, request[offset:offset + 32768])
        self.send(6)
        result = bytearray()
        while True:
            kind, stream, data = self.frame()
            if stream != self.stream: continue
            if kind == 5: result += data
            elif kind in (6, 7): break
        self.send(7)
        if not result: return 0, b''
        header, body = bytes(result).split(b'\r\n\r\n', 1)
        status = int(header.split(b' ')[1])
        fields = {k.lower(): v.strip() for k, v in (line.split(b':', 1) for line in header.split(b'\r\n')[1:])}
        self.response_headers = fields
        if method != 'HEAD' and b'content-length' in fields: assert len(body) == int(fields[b'content-length'])
        return status, body
    def close(self):
        self.process.stdin.close()
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.selector.close()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()

import uuid,io,tarfile,runpy
codec=runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Resources' / 'outershell-container-provider'))
def check_mac_archive(archive):
 with tarfile.open(fileobj=io.BytesIO(archive),mode='r:gz') as tar:
  recipe=tar.extractfile('Recipe/recipe.orwv').read();manifest=tar.extractfile('manifest.orwv').read()
 assert {'version','baseImage','steps','realizedStepIDs'} <= codec['offset_archive_decode'](recipe).keys()
 if sys.platform != 'darwin':return
 source=(pathlib.Path(__file__).resolve().parents[1]/'Agent'/'SafeSpaceManager.swift').read_text()
 records=source[source.index('private struct SafeSpaceRecipe: Codable'):source.index('private struct SafeSpaceRecipeCatalogItem')]
 decoder=source[source.index('private enum OffsetArchiveError:'):source.index('private enum RegistryResourceStore')]
 data=source[source.index('private extension Data {'):source.index('    func safeSpaceReferencedData')]+"}\n"
 main="""do {
 let recipe = try OffsetArchive.decode(SafeSpaceRecipe.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))
 let manifest = try OffsetArchive.decode(SafeSpaceTransferManifest.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[2])))
 guard recipe.version == 1 && manifest.version == 1 else { exit(2) }
 print("PASS actual macOS recipe/manifest decoder")
} catch { print(error); exit(1) }
"""
 with tempfile.TemporaryDirectory(prefix='outershell-archive-test-') as directory:
  root=pathlib.Path(directory)
  (root/'Check.swift').write_text('import Foundation\nimport CoreFoundation\n'+records+decoder+data+main)
  (root/'recipe.orwv').write_bytes(recipe);(root/'manifest.orwv').write_bytes(manifest)
  subprocess.run(['swiftc','-module-cache-path',str(root/'cache'),str(root/'Check.swift'),'-o',str(root/'check')],check=True)
  subprocess.run([str(root/'check'),str(root/'recipe.orwv'),str(root/'manifest.orwv')],check=True)

b=Bridge(shell)
ids=[]
def op(operation,**values):
 values.update(operation=operation,requestID=str(uuid.uuid4()))
 status,body=b.request('/api/safe-spaces',json.dumps(values).encode(),'POST')
 assert status==200,(status,body[:500]);return json.loads(body)
def snapshot():
 status,raw=b.request('/api/container-snapshot');assert status==200;return json.loads(raw)
def wait(id):
 for _ in range(150):
  w=next((w for w in snapshot()['workspaces'] if w['id']==id),None)
  if w and w.get('buildProgress',{}).get('phase')=='failed':raise AssertionError(w['buildProgress'])
  if w and w['state']=='running' and not w['recipe']['needsRebuild']:return w
  time.sleep(1)
 raise TimeoutError(id)
def binary(t,operation,offset=0,chunk=b'',length=1000000):
 data=struct.pack('<4sHH16sQQII',b'OSCT',1,operation,uuid.UUID(t).bytes,offset,len(chunk) if operation==2 else length,48 if chunk else 0,len(chunk))+chunk
 status,response=b.request('/api/safe-spaces',data,'POST');assert status==200
 header=struct.unpack_from('<4sHHQQIIII',response)
 assert header[2]==0,response[header[7]:header[7]+header[8]]
 return header,response[header[5]:header[5]+header[6]]
try:
 name='Sharing test '+uuid.uuid4().hex
 op('create',runtimeProviderID='docker',name=name,baseImage='outershell/windows-test:1',cpus=2,memoryInGB=2)
 for _ in range(30):
  w=next((w for w in snapshot()['workspaces'] if w['name']==name),None)
  if w:break
  time.sleep(1)
 assert w;ids.append(w['id'])
 op('updateDockerfile',workspaceID=w['id'],dockerfile='FROM outershell/windows-test:1\nRUN echo sharing > /share-test\n')
 share=op('prepareShare',workspaceID=w['id'],includeMountedFolders=False,includePersistentData=False)
 h,archive=binary(share['transferID'],1)
 assert len(archive)==share['byteCount']
 with tarfile.open(fileobj=io.BytesIO(archive),mode='r:gz') as tar:
  manifest=codec['offset_archive_decode'](tar.extractfile('manifest.orwv').read())
  recipe=codec['offset_archive_decode'](tar.extractfile('Recipe/recipe.orwv').read())
  assert manifest['name']==name and recipe['mounts']==[]
 check_mac_archive(archive)
 print('PASS Windows export decoded by Linux/macOS codec',flush=True)
 upload=op('beginImport');binary(upload['transferID'],2,chunk=archive)
 result=op('finishImport',transferID=upload['transferID'],runtimeProviderID='wsl.container');ids.append(result['importedWorkspaceID'])
 w=wait(ids[-1]);assert w['runtime']['providerID']=='wsl.container'
 print('PASS Docker -> shared archive -> WSL container',flush=True)
 # Generate the existing Unix archive format independently.
 memory=io.BytesIO()
 with tarfile.open(fileobj=memory,mode='w:gz') as tar:
  values={'manifest.orwv':codec['offset_archive_encode']({'version':1,'name':'Unix import '+uuid.uuid4().hex,'cpus':2,'memoryInGB':2,'includedMounts':[],'omittedMounts':[]}),
          'Recipe/recipe.orwv':codec['offset_archive_encode']({'baseImage':'outershell/windows-test:1','mounts':[],'environment':[{'name':'FROM_UNIX','value':'yes'}],'publishedPorts':[]}),
          'Recipe/Dockerfile':b'FROM outershell/windows-test:1\nRUN echo unix > /from-unix\n'}
  for name,value in values.items():
   info=tarfile.TarInfo(name);info.size=len(value);tar.addfile(info,io.BytesIO(value))
 upload=op('beginImport')
 endpoint='/api/container-transfers/'+upload['transferID']
 protocol={'Upload-Protocol':'outershell-resumable-v1'}
 status,_=b.request(endpoint,method='HEAD',headers=protocol)
 assert status==200 and b.response_headers[b'upload-offset']==b'0'
 data=memory.getvalue()
 status,_=b.request(endpoint,data,'PATCH',headers=dict(protocol,**{'Upload-Offset':'0'}))
 assert status==200 and int(b.response_headers[b'upload-offset'])==len(data)
 status,_=b.request(endpoint,b'x','PATCH',headers=dict(protocol,**{'Upload-Offset':'0'}))
 assert status==409
 result=op('finishImport',transferID=upload['transferID'],runtimeProviderID='docker');ids.append(result['importedWorkspaceID']);wait(ids[-1])
 print('PASS Unix-format import -> Docker',flush=True)
 upload=op('beginImport');endpoint='/api/container-transfers/'+upload['transferID']
 status,_=b.request(endpoint,b'x','PATCH',headers={'Upload-Protocol':'outershell-resumable-v1','Upload-Offset':'0','Origin':'http://untrusted.invalid'})
 assert status==403
 segment=bytes(64*1024*1024)
 status,_=b.request(endpoint,segment,'PATCH',headers={'Upload-Protocol':'outershell-resumable-v1','Upload-Offset':'0'})
 assert status==200 and int(b.response_headers[b'upload-offset'])==len(segment)
 status,_=b.request(endpoint,method='HEAD',headers=protocol)
 assert status==200 and int(b.response_headers[b'upload-offset'])==len(segment)
 op('cancelImport',transferID=upload['transferID'])
 del segment
 print('PASS native resumable HTTP upload, 64 MiB segments, offset conflicts, and origin checks',flush=True)
 # Included mounts and persistent data must survive a cross-platform round trip.
 mount_id=str(uuid.uuid4()); data_id=str(uuid.uuid4()); transfer_name='Mounted import '+uuid.uuid4().hex
 memory=io.BytesIO()
 mount={'id':mount_id,'name':'Source files','hostPath':'/source/on/unix','guestPath':'/shared-source','isReadOnly':True}
 recipe={'baseImage':'outershell/windows-test:1','mounts':[mount],'environment':[{'name':'UNICODE','value':'café 日本語'}],'publishedPorts':[],
         'persistentData':[{'id':data_id,'name':'Saved data','guestPath':'/saved','isDeclared':True}],'createdAt':123.5}
 with tarfile.open(fileobj=memory,mode='w:gz') as tar:
  values={'manifest.orwv':codec['offset_archive_encode']({'version':1,'name':transfer_name,'cpus':2,'memoryInGB':2,'includedMounts':[mount],'omittedMounts':[]}),
          'Recipe/recipe.orwv':codec['offset_archive_encode'](recipe), 'Recipe/Dockerfile':b'FROM outershell/windows-test:1\n',
          f'Mounted Folders/{mount_id}/hello.txt':b'mounted contents',f'Persistent Data/{data_id}/saved.txt':b'persistent contents'}
  for name,value in values.items():
   info=tarfile.TarInfo(name);info.size=len(value);tar.addfile(info,io.BytesIO(value))
 upload=op('beginImport');binary(upload['transferID'],2,chunk=memory.getvalue())
 prompt=op('finishImport',transferID=upload['transferID'],runtimeProviderID='docker')
 assert prompt['needsMountDestination'] and prompt['importMounts'][0]['id']==mount_id
 result=op('finishImport',transferID=upload['transferID'],runtimeProviderID='docker',mountDestinationRoot=prompt['suggestedMountRoot'])
 ids.append(result['importedWorkspaceID']);mounted=wait(ids[-1])
 assert mounted['mounts'][0]['isReadOnly'] and len(mounted['persistentData'])==1
 share=op('prepareShare',workspaceID=ids[-1],includeMountedFolders=True,includePersistentData=True)
 header,archive=binary(share['transferID'],1)
 with tarfile.open(fileobj=io.BytesIO(archive),mode='r:gz') as tar:
  assert tar.extractfile(f'Mounted Folders/{mount_id}/hello.txt').read()==b'mounted contents'
  assert tar.extractfile(f'Persistent Data/{data_id}/saved.txt').read()==b'persistent contents'
 print('PASS included folders, persistent data, Unicode, and Unix binary archive round trip',flush=True)
 for unsafe in ('../escape.txt','Recipe/linked','Recipe/hardlinked','Recipe/CON'):
  memory=io.BytesIO()
  with tarfile.open(fileobj=memory,mode='w:gz') as tar:
   safe_values={'manifest.orwv':codec['offset_archive_encode']({'version':1,'name':'Unsafe archive test','cpus':2,'memoryInGB':2,'includedMounts':[],'omittedMounts':[]}),
                'Recipe/recipe.orwv':codec['offset_archive_encode']({'mounts':[],'environment':[],'publishedPorts':[]}),
                'Recipe/Dockerfile':b'FROM outershell/windows-test:1\n','Recipe/source.txt':b'safe'}
   for safe_name,value in safe_values.items():
    safe_info=tarfile.TarInfo(safe_name);safe_info.size=len(value);tar.addfile(safe_info,io.BytesIO(value))
   info=tarfile.TarInfo(unsafe)
   if unsafe.endswith('hardlinked'):info.type=tarfile.LNKTYPE;info.linkname='Recipe/source.txt'
   elif unsafe.endswith('linked'): info.type=tarfile.SYMTYPE;info.linkname='../../outside'
   else: info.size=1
   tar.addfile(info,io.BytesIO(b'x'))
  upload=op('beginImport');binary(upload['transferID'],2,chunk=memory.getvalue())
  payload={'operation':'finishImport','requestID':str(uuid.uuid4()),'transferID':upload['transferID'],'runtimeProviderID':'docker'}
  status,response=b.request('/api/safe-spaces',json.dumps(payload).encode(),'POST')
  assert status==400,(unsafe,status,response)
  op('cancelImport',transferID=upload['transferID'])
 print('PASS archive traversal, symbolic/hard links, and Windows device-name rejection',flush=True)
finally:
 for id in ids:
  try:op('delete',workspaceID=id)
  except Exception as e:print('cleanup',e)
 b.close()
