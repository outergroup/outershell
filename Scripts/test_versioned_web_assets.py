#!/usr/bin/env python3
"""Verify immutable asset URLs and HTML invalidation after an asset-only update."""
import http.client
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

with tempfile.TemporaryDirectory(prefix='shell-assets-') as directory:
    root=Path(directory)
    (root/'index.html').write_text('<link href="/web/style.css"><script src="/web/app.js"></script>')
    (root/'style.css').write_text('body { color: black; }')
    (root/'app.js').write_text('console.log(1);')
    sock=root/'http'
    process=subprocess.Popen([sys.argv[1],'--socket-path',str(sock),'--web-root',str(root)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if sock.exists():break
            time.sleep(.02)
        def fetch(path,method='GET',headers=None):
            c=http.client.HTTPConnection('localhost',timeout=5)
            c.sock=socket.socket(socket.AF_UNIX);c.sock.settimeout(5);c.sock.connect(str(sock))
            c.request(method,path,headers=headers or {})
            response=c.getresponse();result=response.status,dict(response.getheaders()),response.read();c.close();return result
        status,headers,html=fetch('/')
        assert status==200
        urls=re.findall(rb'/web/[^"<>]+',html)
        assert len(urls)==2 and all(b'?v=' in u for u in urls),html
        assert 'must-revalidate' in headers['Cache-Control']
        for url in urls:
            status,h,body=fetch(url.decode())
            assert status==200 and 'immutable' in h['Cache-Control']
            assert fetch(url.decode(),'HEAD')[2]==b''
            assert fetch(url.decode(),headers={'If-None-Match':h['ETag']})[0]==304
        assert fetch('/',headers={'If-None-Match':headers['ETag']})[0]==304
        assert 'immutable' not in fetch('/web/app.js')[1]['Cache-Control']
        (root/'app.js').write_text('console.log(2);')
        status,newheaders,newhtml=fetch('/',headers={'If-None-Match':headers['ETag']})
        assert status==200 and html!=newhtml and newheaders['ETag']!=headers['ETag']
        assert urls[0] in newhtml and urls[1] not in newhtml
        assert fetch(urls[1].decode())[0]==404
        newurl=re.findall(rb'/web/[^"<>]+',newhtml)[1].decode()
        assert fetch(newurl)[2]==b'console.log(2);'
        print('PASS: versioned URLs, immutable caching, HEAD/304, asset-only HTML invalidation, stale-version rejection')
    finally:
        process.terminate();process.wait(timeout=5)
