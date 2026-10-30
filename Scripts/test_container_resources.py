#!/usr/bin/env python3
import pathlib
import runpy
import subprocess
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))
g = provider['operation_update_resources'].__globals__
LIMITS = {'maxCPUs': 2, 'maxMemoryInMB': 3919, 'defaultCPUs': 2, 'defaultMemoryInMB': 3919}


class ResourceUpdateTests(unittest.TestCase):
    def update(self, request, state='running', code=0):
        record = {'id': 'test', 'cpus': 2, 'memoryInMB': 3919, 'ownsContainer': True}
        recipe = {}
        calls = []

        class Records:
            def __enter__(self): self.values = [record]; return self
            def __exit__(self, *args): pass

        def docker(arguments, **kwargs):
            calls.append(arguments)
            return subprocess.CompletedProcess(arguments, code, '', 'cgroup error' if code else '')

        with patch.dict(g, Records=Records, find_record=lambda rows, _: rows[0],
                        resource_limits=lambda: LIMITS, load_recipe=lambda _: recipe,
                        save_recipe=lambda _, value: recipe.update(value),
                        update_record=lambda _, **changes: record.update(changes),
                        docker_state=lambda _: state, docker=docker,
                        container_name=lambda _: 'container'):
            result = g['operation_update_resources']({'workspaceID': 'test', **request})
        return result, record, recipe, calls

    def test_running_container_is_updated_in_place(self):
        result, record, recipe, calls = self.update({'cpus': 1, 'memoryInMB': 1536})
        self.assertEqual(result, {'resourcesApplied': True})
        self.assertEqual((record['cpus'], record['memoryInMB']), (1, 1536))
        self.assertEqual(calls[-1], ['update', '--cpus', '1', '--memory', '1536m', '--memory-swap', '3072m', 'container'])
        self.assertEqual(recipe['realizedResources'], {'cpus': 1, 'memoryInMB': 1536})

    def test_failed_live_update_leaves_rebuild_pending(self):
        result, record, recipe, _ = self.update({'cpus': 1, 'memoryInMB': 1536}, code=1)
        self.assertFalse(result['resourcesApplied'])
        self.assertIn('cgroup error', result['resourcesDetail'])
        self.assertEqual(record['cpus'], 1)
        self.assertEqual(recipe['realizedResources'], {'cpus': 2, 'memoryInMB': 3919})

    def test_missing_container_uses_limits_on_next_build(self):
        result, record, _, calls = self.update({'cpus': 1, 'memoryInMB': 512}, state='absent')
        self.assertEqual(result, {'resourcesApplied': False})
        self.assertEqual(record['memoryInMB'], 512)
        self.assertEqual(calls, [])

    def test_out_of_range_values_are_rejected_before_saving(self):
        with self.assertRaisesRegex(ValueError, 'between 1 and 2 CPUs'):
            self.update({'cpus': 4, 'memoryInMB': 2048})
        with self.assertRaisesRegex(ValueError, 'between 128 MB and 3919 MB'):
            self.update({'cpus': 1, 'memoryInMB': 64})


if __name__ == '__main__':
    unittest.main()
