#!/usr/bin/env python3
import copy
import pathlib
import runpy
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

provider = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))

class RuntimeTests(unittest.TestCase):
    def test_defaults_and_invalid_settings(self):
        settings = provider['runtime_settings']
        self.assertEqual(settings({}), {'networkMode': 'bridge', 'useInit': True, 'restartPolicy': 'no'})
        for bad in [{'networkMode': 'none'}, {'useInit': 'false'}, {'restartPolicy': 'always'},
                    {'networkMode': 'host', 'publishedPorts': [{'hostPort': 8123, 'containerPort': 8123}]}]:
            with self.assertRaises(ValueError): settings(bad)

    def test_create_arguments_and_volume_identity(self):
        create = provider['create_runtime_container']
        item = {'id': 'data-id', 'guestPath': '/config'}
        recipe = {'networkMode': 'host', 'useInit': False, 'restartPolicy': 'unless-stopped', 'persistentData': [item]}
        calls = []
        class Records:
            def __enter__(self): self.values = [{'id': 'test', 'cpus': 2, 'memoryInMB': 2048}]; return self
            def __exit__(self, *args): pass
        with patch.dict(create.__globals__, load_recipe=lambda _: recipe, Records=Records,
                        find_record=lambda rows, _: rows[0], container_name=lambda _: 'container',
                        image_name=lambda _: 'image', recipe_directory=lambda _: '/recipe',
                        prepare_persistent_volume=lambda *args: None,
                        persistent_mount=lambda cid, iid: 'type=volume,source=' + iid,
                        docker=lambda args, **kwargs: calls.append(args)):
            create('test')
            self.assertNotIn('--init', calls[-1])
            self.assertEqual(calls[-1][calls[-1].index('--network')+1], 'host')
            self.assertEqual(calls[-1][calls[-1].index('--restart')+1], 'unless-stopped')
            self.assertIn('type=volume,source=data-id,target=/config', calls[-1])
            recipe.clear(); create('test')
            self.assertIn('--init', calls[-1])
            self.assertEqual(calls[-1][calls[-1].index('--network')+1], 'bridge')

    def test_configuration_save_preserves_omitted_settings(self):
        update = provider['operation_update_configuration']
        recipe = {'networkMode': 'host', 'useInit': False, 'restartPolicy': 'unless-stopped'}
        saved = []
        class Records:
            def __enter__(self): self.values = []; return self
            def __exit__(self, *args): pass
        def save(_, value):
            provider['runtime_settings'](value)
            saved.append(copy.deepcopy(value))
        with patch.dict(update.__globals__, Records=Records, find_record=lambda *args: {},
                        require_managed_container=lambda _: None, load_recipe=lambda _: copy.deepcopy(recipe), save_recipe=save):
            update({'workspaceID': 'test', 'environment': [{'name': 'TZ', 'value': 'America/Los_Angeles'}]})
            self.assertEqual(provider['runtime_settings'](saved[-1]), recipe)
            with self.assertRaises(ValueError):
                update({'workspaceID': 'test', 'publishedPorts': [{'hostPort': 8123, 'containerPort': 8123}]})
            self.assertEqual(len(saved), 1)

    def test_rebuild_detection_and_transfer_encoding(self):
        snapshot = provider['recipe_dictionary']
        recipe = provider['default_recipe']()
        recipe.update(realizedDockerfile='FROM test', realizedMounts=[], realizedEnvironment=[], realizedPublishedPorts=[])
        with tempfile.TemporaryDirectory() as directory:
            file = pathlib.Path(directory) / 'Dockerfile'; file.write_text('FROM test')
            with patch.dict(snapshot.__globals__, dockerfile_path=lambda _: file, recipe_directory=lambda _: pathlib.Path(directory),
                            docker_path=lambda: '/usr/bin/docker', image_name=lambda _: 'image'):
                self.assertFalse(snapshot({'id': 'test'}, recipe)['needsRebuild'])
                recipe.update(networkMode='host', useInit=False, restartPolicy='unless-stopped')
                self.assertTrue(snapshot({'id': 'test'}, recipe)['needsRebuild'])
                recipe['realizedRuntimeSettings'] = provider['runtime_settings'](recipe)
                self.assertFalse(snapshot({'id': 'test'}, recipe)['needsRebuild'])
                restored = provider['offset_archive_decode'](provider['offset_archive_encode'](recipe))
                self.assertEqual(provider['runtime_settings'](restored), recipe['realizedRuntimeSettings'])
                self.assertTrue(snapshot({'id': 'test'}, restored)['supportsRuntimeSettings'])

if __name__ == '__main__': unittest.main()
