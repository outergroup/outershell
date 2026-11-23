#!/usr/bin/env python3
"""Check that the web, native, and Linux provider container messages agree."""
import json
import pathlib
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
provider = runpy.run_path(str(ROOT / "Resources" / "outershell-container-provider"))
globals_ = provider["handle_container_message"].__globals__
ARGUMENTS = {"dockerfile": "FROM debian:bookworm\n", "mounts": [{"id": "m", "isReadOnly": True}], "cpus": 4}
WORKSPACE = "54a0a9ae-71f2-4dfa-a69e-3b875ca20a0f"

NODE = r"""
const fs = require("fs");
const vm = require("vm");
const source = fs.readFileSync(process.argv[2], "utf8");
function slice(start, end) {
  const from = source.indexOf(start);
  return source.slice(from, source.indexOf(end, from) + end.length);
}
const code = [
  slice("  class PayloadReader {", "\n  }\n"),
  slice("  const Orwv = (() => {", "  })();\n"),
  slice("  const containerOperations = [", "\n  ];\n"),
  slice("  function encodeContainerRequest(", "\n  }\n"),
  slice("  function decodeContainerResponse(", "\n  }\n"),
].join("\n");
const context = { TextEncoder, TextDecoder, DataView, Uint8Array, ArrayBuffer, BigInt, Date, Number, Object, Math, Array, Error, decoder: new TextDecoder() };
vm.createContext(context);
vm.runInContext(code + "\nthis.encodeContainerRequest = encodeContainerRequest; this.decodeContainerResponse = decodeContainerResponse;", context);
const arguments_ = JSON.parse(process.argv[3]);
fs.writeFileSync(process.argv[4], context.encodeContainerRequest("updateDockerfile", { workspaceID: process.argv[5], ...arguments_ }));
const reply = context.decodeContainerResponse(new Uint8Array(fs.readFileSync(process.argv[6])));
fs.writeFileSync(process.argv[7], JSON.stringify(reply));
try {
  context.decodeContainerResponse(new Uint8Array(fs.readFileSync(process.argv[8])));
  console.log("FAIL error reply did not throw");
  process.exit(1);
} catch (error) {
  if (error.message !== "No such container.") { console.log("FAIL wrong error " + error.message); process.exit(1); }
}
"""

SWIFT = r"""
let arguments = CommandLine.arguments
let message = try ContainerMessage.request(operation: "updateDockerfile",
                                           workspaceID: arguments[1],
                                           arguments: ["dockerfile": "FROM debian:bookworm\n",
                                                       "mounts": [["id": "m", "isReadOnly": true]],
                                                       "cpus": 4])
try message.write(to: URL(fileURLWithPath: arguments[2]))
struct Reply: Decodable {
    struct Workspace: Decodable { let id: String; let name: String }
    let workspaces: [Workspace]
    let transferID: String?
    let requestID: String
    let error: String?
}
let reply = try ContainerMessage.decode(Reply.self, from: Data(contentsOf: URL(fileURLWithPath: arguments[3])),
                                        defaults: ["requestID": "r"])
guard reply.workspaces.first?.name == "Science", reply.transferID == "t", reply.requestID == "r", reply.error == nil else {
    print("FAIL swift reply", reply); exit(1)
}
let failure = try ContainerMessage.decode(Reply.self, from: Data(contentsOf: URL(fileURLWithPath: arguments[4])),
                                          defaults: ["requestID": "r", "workspaces": [Any]()])
guard failure.error == "No such container." else { print("FAIL swift error", failure); exit(1) }
"""


def decoded_request(message):
    seen = {}

    def handle(request):
        seen.update(request)
        return {"requestID": "x", "providers": [], "workspaces": [{"id": WORKSPACE, "name": "Science"}], "transferID": "t"}

    with patch.dict(globals_, handle_request=handle):
        reply = provider["handle_container_message"](message)
    return seen, reply


def main():
    failures = 0
    message = provider["api_message"](
        struct.pack("<HHI", 35, provider["CONTAINER_OPERATIONS"].index("updateDockerfile"), 0),
        [WORKSPACE, provider["offset_archive_encode"](ARGUMENTS)],
    )
    seen, reply = decoded_request(message)
    expected = dict(ARGUMENTS, operation="updateDockerfile", workspaceID=WORKSPACE)
    if seen != expected:
        print("FAIL python request", seen)
        failures += 1
    error_reply = provider["container_response_message"](error="No such container.")
    with tempfile.TemporaryDirectory(prefix="container-messages-") as temporary:
        directory = pathlib.Path(temporary)
        (directory / "reply.bin").write_bytes(reply)
        (directory / "error.bin").write_bytes(error_reply)
        (directory / "harness.cjs").write_text(NODE)
        shutil.copy(ROOT / "Resources" / "OuterShellWeb" / "app.js", directory / "app.js")
        node_arguments = ["harness.cjs", "app.js", json.dumps(ARGUMENTS), "web-request.bin", WORKSPACE,
                          "reply.bin", "web-reply.json", "error.bin"]
        if shutil.which("node"):
            command = ["node", *node_arguments]
        elif shutil.which("docker"):
            command = ["docker", "run", "--rm", "-v", f"{directory}:/work", "-w", "/work", "node:22-alpine",
                       "node", *node_arguments]
        else:
            command = None
        if command:
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            if result.returncode:
                print("FAIL javascript", result.stdout, result.stderr[-800:])
                failures += 1
            else:
                seen, _ = decoded_request((directory / "web-request.bin").read_bytes())
                if seen != expected:
                    print("FAIL javascript request", seen)
                    failures += 1
                merged = json.loads((directory / "web-reply.json").read_text())
                if merged.get("transferID") != "t" or merged["workspaces"][0]["name"] != "Science":
                    print("FAIL javascript reply", merged)
                    failures += 1
            print("checked JavaScript messages")
        else:
            print("SKIP JavaScript messages: no node or docker")
        if shutil.which("swiftc") and sys.platform == "darwin":
            source = ((ROOT / "SharedSwift" / "OffsetArchive.swift").read_text() +
                      (ROOT / "Frontend" / "ContainerMessages.swift").read_text().replace("import Foundation\n", "", 1) +
                      SWIFT)
            (directory / "main.swift").write_text(source)
            subprocess.run(["swiftc", "-module-cache-path", str(directory / "cache"), str(directory / "main.swift"),
                            "-o", str(directory / "swift-check")], check=True, capture_output=True)
            result = subprocess.run([str(directory / "swift-check"), WORKSPACE, str(directory / "swift-request.bin"),
                                     str(directory / "reply.bin"), str(directory / "error.bin")],
                                    capture_output=True, text=True)
            if result.returncode:
                print("FAIL swift", result.stdout, result.stderr[-800:])
                failures += 1
            else:
                seen, _ = decoded_request((directory / "swift-request.bin").read_bytes())
                if seen != expected:
                    print("FAIL swift request", seen)
                    failures += 1
            print("checked Swift messages")
        else:
            print("SKIP Swift messages: requires swiftc on macOS")
    if failures:
        print(f"FAIL {failures} container message check(s)")
        return 1
    print("PASS container requests and replies agree across the web UI, the native UI, and the Linux provider")
    return 0


if __name__ == "__main__":
    sys.exit(main())
