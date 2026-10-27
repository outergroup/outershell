import base64
import pathlib
import runpy


provider = runpy.run_path(str(
    pathlib.Path(__file__).resolve().parents[1] / "Resources" / "outershell-container-provider"
))
preserve = provider["preserve_observed_icons"]
store = provider["store_observed_icon"]
endpoint = {
    "serviceID": "app",
    "frontendID": "web",
    "socketPath": "/app.sock",
    "url": "",
    "iconPath": "",
}

apps = [endpoint.copy()]
preserve(apps, [])
token = apps[0]["iconObservationToken"]
assert len(token) == 32
apps[0]["observedIconData"] = "learned"
refreshed = [endpoint.copy()]
preserve(refreshed, apps)
assert refreshed[0]["iconData"] == "learned"
assert refreshed[0]["iconObservationToken"] == token

changed = [dict(endpoint, url="/other")]
preserve(changed, refreshed)
assert changed[0]["iconObservationToken"] != token
assert "iconData" not in changed[0]
explicit = [dict(endpoint, iconPath="/declared.png", iconData="declared")]
preserve(explicit, refreshed)
assert explicit[0]["iconData"] == "declared"


class Records:
    values = [{"cachedApps": refreshed}]
    saves = 0

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def save(self):
        Records.saves += 1


store.__globals__["Records"] = Records
png = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII="
)
request = b"OSCI" + token.encode() + png
assert store(request) == 200
assert Records.saves == 1
assert refreshed[0]["iconData"] == base64.b64encode(png).decode()
assert store(b"OSCI" + b"0" * 32 + png) == 403
assert store(b"OSCI" + token.encode() + b"bad") == 400
refreshed = [endpoint.copy()]
preserve(refreshed, Records.values[0]["cachedApps"])
assert refreshed[0]["iconData"] == base64.b64encode(png).decode()
Records.values[0]["cachedApps"][0]["iconPath"] = "/explicit.png"
assert store(request) == 200
assert Records.saves == 1
print("PASS: token identity, refresh persistence, explicit icon precedence, callback storage, invalid token/payload")

update = provider["update_cached_apps"]
encode = provider["offset_archive_encode"]
decode = provider["offset_archive_decode"]
learned = base64.b64encode(png).decode()
record = {"cachedApps": [dict(endpoint, observedIconData=learned)]}
update(record, [])
assert record["cachedApps"] == []
record = decode(encode(record))
update(record, [endpoint.copy()])
assert record["cachedApps"][0]["iconData"] == learned

second = dict(endpoint, serviceID="second", frontendID="second-web")
record["cachedApps"].append(dict(second, observedIconData="second-icon"))
update(record, [endpoint.copy()])
assert len(record["cachedApps"]) == 1
record = decode(encode(record))
update(record, [endpoint.copy(), second.copy()])
assert [app["iconData"] for app in record["cachedApps"]] == [learned, "second-icon"]
update(record, [dict(endpoint, url="/different"), dict(second, iconPath="/declared.png", iconData="declared")])
assert "iconData" not in record["cachedApps"][0]
assert record["cachedApps"][1]["iconData"] == "declared"
other_container = {}
update(other_container, [endpoint.copy()])
assert "iconData" not in other_container["cachedApps"][0]
assert Records.values[0].get("observedIcons")
print("PASS: learned icons survive empty/partial discovery and persistence; removed apps stay absent; endpoint and container isolation; declared icon precedence")
