import base64
import hashlib
import os
import pathlib
import runpy
import tempfile


provider = runpy.run_path(str(
    pathlib.Path(__file__).resolve().parents[1] / "Resources" / "outershell-container-provider"
))
icon_data = provider["endpoint_icon_data"]
with_icon = provider["with_endpoint_icon"]
update = provider["update_cached_apps"]
png = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII="
)
encoded = base64.b64encode(png).decode()

with tempfile.TemporaryDirectory() as root:
    os.environ["XDG_CONFIG_HOME"] = root
    cache = pathlib.Path(root) / "outerssh" / "endpoint-icons"
    socket_app = {"serviceID": "app", "frontendID": "web", "externalSocketPath": "/run/outershell/relay.sock",
                  "publishedPort": 0, "iconPath": ""}
    port_app = dict(socket_app, frontendID="port", publishedPort=6006)
    assert icon_data(socket_app) is None
    assert with_icon(socket_app) is socket_app

    cache.mkdir(parents=True, mode=0o700)
    socket_icon = cache / (hashlib.sha256(b"unix /run/outershell/relay.sock").hexdigest() + ".png")
    port_icon = cache / "57cadb02e51f395b58595fe590195d2f5b1a921b18f35ee60ea5fbfbb4cb19b5.png"
    socket_icon.write_bytes(png)
    socket_icon.chmod(0o600)
    port_icon.write_bytes(png)
    port_icon.chmod(0o600)
    assert icon_data(socket_app) == encoded
    assert icon_data(port_app) == encoded
    assert with_icon(socket_app)["iconData"] == encoded and "iconData" not in socket_app
    declared = dict(socket_app, iconData="declared")
    assert with_icon(declared)["iconData"] == "declared"
    assert icon_data(dict(socket_app, externalSocketPath="relative.sock", publishedPort=0)) is None
    assert icon_data(dict(socket_app, externalSocketPath="", publishedPort=0)) is None

    socket_icon.chmod(0o666)
    assert icon_data(socket_app) is None
    socket_icon.chmod(0o600)
    cache.chmod(0o777)
    assert icon_data(socket_app) is None
    cache.chmod(0o700)
    socket_icon.write_bytes(b"not a png")
    assert icon_data(socket_app) is None
    socket_icon.unlink()
    socket_icon.symlink_to(port_icon)
    assert icon_data(socket_app) is None
    socket_icon.unlink()
    socket_icon.write_bytes(png + b"x" * (64 * 1024))
    assert icon_data(socket_app) is None

record = {"cachedApps": [dict(socket_app, iconData="old")]}
update(record, [socket_app])
assert record["cachedApps"] == [socket_app]
print("PASS: container app icons come from the endpoint icon cache by published port or relay socket, "
      "declared icons take precedence, and unsafe or invalid cache entries are ignored")
