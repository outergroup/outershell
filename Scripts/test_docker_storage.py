import contextlib
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))
g = provider['storage_inventory'].__globals__
ID = '11111111-1111-1111-1111-111111111111'
REFERENCE = 'outershell/container-' + ID + ':recipe'

class Records:
    values = []
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def save(self): pass

class StorageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.images = [{'Id':'sha256:one','RepoTags':[REFERENCE], 'Config':{},'Size':123}]
        self.containers = []
        self.calls = []
        self.fail_remove = False
        Records.values = []
        self.patch = patch.dict(g, docker=self.docker, Records=Records, provider_root=lambda:self.root,
                               dockerfile_path=lambda _: self.root/'Dockerfile', load_recipe=lambda _: {'baseImage':'debian:bookworm'})
        self.patch.start(); self.addCleanup(self.patch.stop)
        (self.root/'Dockerfile').write_text('FROM debian:bookworm\nRUN echo $HOME\n')
    def docker(self, args, **kwargs):
        self.calls.append(args)
        if args[:3] == ['image','ls','--all']: output='\n'.join(image['Id'] for image in self.images)
        elif args[:2] == ['image','inspect']: output=json.dumps([image for image in self.images if image['Id']==args[2]])
        elif args[0]=='ps': output='\n'.join(str(i) for i in range(len(self.containers)))
        elif args[0]=='inspect': output=json.dumps([self.containers[int(args[1])]])
        elif args[:2]==['image','rm']:
            self.assertTrue(kwargs.get('check'))
            self.assertNotIn('--force', args)
            if self.fail_remove: raise RuntimeError('image in use')
            self.images=[]; output=''
        elif args[:2]==['builder','prune']: output='cleaned'
        else: self.fail('Unexpected command '+str(args))
        return subprocess.CompletedProcess(args, 0, output, '')
    def candidates(self): return [item['reference'] for item in g['storage_inventory']()]
    def test_unused_and_runtime_digest(self):
        self.assertEqual(self.candidates(), [REFERENCE])
        self.containers=[{'Image':'sha256:one'}]
        self.assertEqual(self.candidates(), [])
    def test_saved_recipe_and_alias(self):
        Records.values=[{'id':ID,'ownsContainer':True}]
        self.assertEqual(self.candidates(), [])
        Records.values=[{'id':'22222222-2222-2222-2222-222222222222','ownsContainer':True}]
        self.images[0]['RepoTags'].append('debian:bookworm')
        self.assertEqual(self.candidates(), [])
    def test_digest_reference_through_another_repository(self):
        Records.values=[{'id':'22222222-2222-2222-2222-222222222222','ownsContainer':True}]
        self.images[0]['RepoDigests']=['original@sha256:manifest']
        (self.root/'Dockerfile').write_text('FROM alias@sha256:manifest')
        self.assertEqual(self.candidates(), [])
    def test_dynamic_and_current_base(self):
        Records.values=[{'id':ID,'ownsContainer':True}]
        (self.root/'Dockerfile').write_text('ARG BASE\nFROM ${BASE}')
        self.assertEqual(self.candidates(), [])
        Records.values=[]
        self.images[0]['RepoTags']=['outershell/container-base:20']
        self.assertEqual(self.candidates(), ['outershell/container-base:20'])
        self.images[0]['RepoTags']=['outershell/container-base:21']
        self.assertEqual(self.candidates(), [])
        self.images[0]['RepoTags']=['outershell/container-base:22']
        self.assertEqual(self.candidates(), [])
    def test_dangling_requires_label(self):
        self.images[0]['RepoTags']=[]
        self.assertEqual(self.candidates(), [])
        self.images[0]['Config']['Labels']={'org.outershell.managed':'true'}
        self.assertEqual(self.candidates(), ['sha256:one'])
    def test_rejected_selection_never_removes(self):
        with self.assertRaisesRegex(ValueError,'references changed'):
            g['operation_storage']({'operation':'cleanContainerStorage','images':['ubuntu:latest']})
        self.assertFalse(any(call[:2]==['image','rm'] for call in self.calls))
    def test_partial_failure_and_cache_separation(self):
        self.fail_remove=True
        result=g['operation_storage']({'operation':'cleanContainerStorage','images':[REFERENCE]})
        self.assertEqual(len(result['storage']['failures']),1)
        self.assertFalse(any(call[0]=='builder' for call in self.calls))
        g['operation_storage']({'operation':'cleanContainerBuildCache'})
        self.assertIn(['builder','prune','--force','--filter','until=168h','--keep-storage','10GB'],self.calls)
    def test_delete_failure_retains_registration(self):
        Records.values=[{'id':ID,'ownsContainer':True}]
        removed=[]
        self.fail_remove=True
        with patch.dict(g, remove_runtime_container=lambda _:None, registry_container_remove=removed.append,
                        registry_resource_remove=lambda _:None):
            with self.assertRaisesRegex(RuntimeError,'image in use'):
                g['operation_delete']({'workspaceID':ID})
        self.assertEqual(removed,[])
    def test_malformed_inventory_fails_closed(self):
        self.images=[{}]
        with self.assertRaises((KeyError,ValueError)):
            self.candidates()

if __name__=='__main__': unittest.main()
