#!/usr/bin/env python3
"""Verify provider overview assets and immutable HTTP icon delivery."""
import base64
import copy
import hashlib
import http.client
import os
from pathlib import Path
import runpy
import socket
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from test_layout_storage import request

with tempfile.TemporaryDirectory(prefix='shell-icons-') as directory:
    root = Path(directory)
    (root/'services').mkdir()
    provider = runpy.run_path(str(Path(__file__).resolve().parents[1]/'Resources/outershell-container-provider'))
    globals_ = provider['handle_request'].__globals__
    png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aVZ0AAAAASUVORK5CYII=')
    workspace = {'id':'test', 'apps':[{'iconData':base64.b64encode(png).decode()}], 'commands':['unused'], 'recipe':{'containerfile':'FROM alpine'}, 'mounts':[], 'persistentData':[]}
    globals_['outer_shell_home'] = lambda: root
    globals_['provider_dictionary'] = lambda: {}
    globals_['list_workspaces'] = lambda overview=False: [copy.deepcopy(workspace)]
    result = provider['handle_request']({'operation':'list', 'overview':True})
    overview = result['workspaces'][0]
    assert 'commands' not in overview and 'recipe' not in overview
    icon_url = overview['apps'][0]['iconURL']
    assert 'iconData' not in overview['apps'][0]
    assert icon_url == '/api/icon?key=' + hashlib.sha256(png).hexdigest()
    details = provider['handle_request']({'operation':'list'})
    assert details['workspaces'][0] == workspace
    if len(sys.argv) < 3:
        print('PASS: overview omits detail payloads and exports stable icon URLs; details remain available')
        sys.exit(0)
    api, web_path = root/'api', root/'web'
    env = dict(os.environ, OUTERSHELL_HOME=str(root))
    with (root/'log').open('wb') as log:
        daemon = subprocess.Popen([sys.argv[1], '--api-socket-path',str(api),'--service-manager','internal','--services-dir',str(root/'services'),'--stay-alive'],env=env,stdout=log,stderr=log)
        web = None
        try:
            for _ in range(100):
                try:
                    request(api,11)
                    break
                except (OSError,RuntimeError): time.sleep(.02)
            web = subprocess.Popen([sys.argv[2],'--socket-path',str(web_path),'--api-socket-path',str(api),'--web-root',str(root)],env=env,stdout=log,stderr=log)
            for _ in range(100):
                if web_path.exists():break
                time.sleep(.02)
            def fetch(url):
                c=http.client.HTTPConnection('localhost',timeout=5)
                c.sock=socket.socket(socket.AF_UNIX)
                c.sock.settimeout(5)
                c.sock.connect(str(web_path))
                c.request('GET',url)
                response=c.getresponse()
                result=response.status,dict(response.getheaders()),response.read()
                c.close()
                return result
            status,headers,body=fetch(icon_url)
            assert status==200 and body==png
            assert headers['Content-Type']=='image/png'
            assert 'immutable' in headers['Cache-Control']
            assert fetch('/api/icon?key=../../version')[0]==404
            assert fetch('/api/icon?key='+'0'*64)[0]==404
            print('PASS: overview assets, full details, immutable icon HTTP response, missing/invalid keys')
        finally:
            if web:
                web.terminate();web.wait(timeout=5)
            daemon.terminate();daemon.wait(timeout=5)
