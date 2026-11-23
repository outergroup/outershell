#!/usr/bin/env python3
import pathlib
import runpy
import tempfile
from unittest.mock import patch

provider = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))
create = provider['operation_create']
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    definition = root / 'Dockerfile'
    events = []
    class Records:
        def __enter__(self):
            self.values = []
            return self
        def save(self):
            events.append(('record', definition.read_text()))
        def __exit__(self, *args):
            pass
    def initial(*args):
        definition.write_text('FROM default\n')
    def spawn(*args):
        events.append(('build', definition.read_text()))
    with patch.dict(create.__globals__, docker_readiness=lambda:('ready',''), container_directory=lambda _:root,
                    write_initial_definition=initial, dockerfile_path=lambda _:definition, Records=Records, spawn_worker=spawn):
        custom = 'FROM debian:bookworm\nRUN echo configured\n'
        create({'dockerfile':custom})
        assert events == [('record',custom),('build',custom)]
        events.clear()
        create({})
        assert events == [('record','FROM default\n'),('build','FROM default\n')]
        for invalid in ['', ' ', 42, 'x'*1_000_001]:
            try:
                create({'dockerfile':invalid})
            except RuntimeError:
                pass
            else:
                raise AssertionError('Invalid Dockerfile was accepted')
print('PASS: initial Dockerfile is validated and saved before publishing or building a container')

import copy
import json
from types import SimpleNamespace
reconcile = provider['reconcile_image_volumes']
configuration = {'mounts': [], 'persistentData': []}
volumes = {'/work': {}, '/var/lib/codex': {}}
def save_configuration(_, value):
    configuration.clear()
    configuration.update(copy.deepcopy(value))
with patch.dict(reconcile.__globals__,
                docker=lambda *args, **kwargs: SimpleNamespace(stdout=json.dumps([{'Config': {'Volumes': volumes}}])),
                load_configuration=lambda _: copy.deepcopy(configuration), save_configuration=save_configuration):
    reconcile('test')
    identities = {item['guestPath']: item['id'] for item in configuration['persistentData']}
    assert set(identities) == {'/work', '/var/lib/codex'}
    reconcile('test')
    assert {item['guestPath']: item['id'] for item in configuration['persistentData']} == identities
    del volumes['/work']
    reconcile('test')
    assert next(item for item in configuration['persistentData'] if item['guestPath'] == '/work')['isDeclared'] is False
    volumes['/work'] = {}
    reconcile('test')
    assert {item['guestPath']: item['id'] for item in configuration['persistentData']} == identities
print('PASS: declared image volumes retain their storage identities across rebuilds and configuration changes')
