"""A client that stops reading must not stall other requests: python3 Scripts/test_backend_slow_client.py BACKEND_BINARY."""
import hashlib
import http.client
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="outershell-slow-client-") as temporary:
    root = pathlib.Path(temporary)
    bundle = root / "OuterShell.bundle.macos-arm.aar"
    block = bytes(range(256)) * 256
    expected = hashlib.sha256()
    with bundle.open("wb") as output:
        for _ in range(512):
            output.write(block)
            expected.update(block)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen([sys.argv[1], "--port", str(port), "--bundles-dir", str(root),
                                "--api-socket-path", str(root / "missing-api.sock"), "--stay-alive"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def request(path):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        connection.request("GET", path)
        return connection, connection.getresponse()

    try:
        for _ in range(100):
            try:
                connection, response = request("/missing")
                response.read()
                connection.close()
                break
            except ConnectionRefusedError:
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                time.sleep(.05)

        stalled = socket.create_connection(("127.0.0.1", port))
        stalled.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        stalled.sendall(b"GET /bundles/OuterShell/macos-arm HTTP/1.1\r\nHost: localhost\r\n\r\n")
        time.sleep(.3)

        start = time.monotonic()
        connection, response = request("/missing")
        assert response.status == 404
        response.read()
        connection.close()
        assert time.monotonic() - start < 2, "a stalled client blocked another request"

        stalled.settimeout(5)
        received = b""
        while b"\r\n\r\n" not in received:
            received += stalled.recv(65536)
        header, body = received.split(b"\r\n\r\n", 1)
        assert header.startswith(b"HTTP/1.1 200 ")
        actual = hashlib.sha256(body)
        length = len(body)
        while chunk := stalled.recv(1 << 20):
            actual.update(chunk)
            length += len(chunk)
        stalled.close()
        assert length == bundle.stat().st_size, length
        assert actual.digest() == expected.digest()

        abandoned = socket.create_connection(("127.0.0.1", port))
        abandoned.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        abandoned.sendall(b"GET /bundles/OuterShell/macos-arm HTTP/1.1\r\nHost: localhost\r\n\r\n")
        time.sleep(20)
        abandoned.settimeout(5)
        drained = 0
        try:
            while chunk := abandoned.recv(1 << 20):
                drained += len(chunk)
        except (ConnectionResetError, socket.timeout):
            pass
        abandoned.close()
        assert drained < bundle.stat().st_size, "an idle client was never dropped"

        connection, response = request("/missing")
        assert response.status == 404
        connection.close()
        print("PASS: stalled client isolated, buffered response delivered intact, idle client dropped")
    finally:
        process.terminate()
        process.wait(timeout=5)
