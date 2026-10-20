"""Exercise the real HTTP backend: python3 Scripts/test_container_download.py BACKEND_BINARY."""
import hashlib
import http.client
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import uuid

with tempfile.TemporaryDirectory(prefix="outershell-download-") as temporary:
    root = pathlib.Path(temporary)
    identifier = str(uuid.uuid4())
    transfer = root / identifier
    transfer.mkdir()
    archive = transfer / "container.outershell-container"
    block = bytes(range(256)) * 256
    expected = hashlib.sha256()
    with archive.open("wb") as output:
        for _ in range(1024):
            output.write(block)
            expected.update(block)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen([sys.argv[1], "--port", str(port), "--container-transfers-dir", str(root), "--stay-alive"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    def request(method, path):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        connection.request(method, path)
        return connection, connection.getresponse()
    route = f"/api/container-downloads/{identifier}/Experiment.outershell-container"
    try:
        for _ in range(100):
            try:
                connection, response = request("HEAD", route)
                break
            except ConnectionRefusedError:
                if process.poll() is not None: raise RuntimeError(process.stderr.read().decode())
                time.sleep(.05)
        assert response.status == 200
        assert int(response.getheader("Content-Length")) == archive.stat().st_size
        assert response.getheader("Content-Disposition") == 'attachment; filename="Experiment.outershell-container"'
        assert response.read() == b""
        connection.close()
        slow, slow_response = request("GET", route)
        assert slow_response.status == 200
        time.sleep(.2)
        start = time.monotonic()
        connection, response = request("HEAD", route)
        assert response.status == 200 and time.monotonic() - start < 2
        connection.close()
        slow.close()
        slow_response.close()
        connection, response = request("GET", route)
        actual = hashlib.sha256()
        while chunk := response.read(65536): actual.update(chunk)
        assert actual.digest() == expected.digest()
        connection.close()
        print("PASS: 64 MiB streamed exactly; HEAD metadata, slow-client isolation, and cancellation")
        for invalid in ("../outside", f"{identifier}/bad%0d%0aname", f"{uuid.uuid4()}/missing", f"{identifier}/../escape"):
            connection, response = request("GET", "/api/container-downloads/" + invalid)
            assert response.status == 404
            response.read(); connection.close()
        connection, response = request("POST", route)
        assert response.status == 405
        response.read(); connection.close()
        archive.unlink()
        archive.symlink_to(root / "secret")
        (root / "secret").write_text("must not be downloaded")
        connection, response = request("GET", route)
        assert response.status == 404
        response.read(); connection.close()
        print("PASS: invalid paths, header injection, missing files, wrong methods, and symlinks rejected")
    finally:
        process.terminate()
        process.wait(timeout=5)
