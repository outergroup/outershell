#!/usr/bin/env python3
import os
import pathlib
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
codec = runpy.run_path(str(ROOT / "Resources" / "outershell-container-provider"))
encode = codec["offset_archive_encode"]
decode = codec["offset_archive_decode"]

VECTORS = [
    ("empty dictionary", {}, True),
    ("empty array", [], True),
    ("empty string", "", True),
    ("scalars", {"yes": True, "no": False, "count": 42, "negative": -7, "ratio": 1.5, "tiny": -0.25}, True),
    ("integer limits", {"max": 2**63 - 1, "min": -2**63}, False),
    ("nested", {"workspaces": [{"id": "a", "apps": [], "mounts": [{"hostPath": "/x", "isReadOnly": True}]}],
                "providers": [{"id": "docker", "capabilities": {"supportsApps": True}}]}, True),
    ("unicode keys", {"z": 1, "é": 2, "a": 3, "😀": 4, "￿": 5, "B": 6}, True),
    ("data", {"payload": bytes(range(256)), "empty": b""}, True),
    ("unicode strings", ["naïve", "日本語", "emoji 😀", "nul\u0000inside"], True),
    ("large double", [1e300, -2.5e-300], True),
]


def corrupt_vectors():
    good = encode({"key": "value"})
    vectors = {
        "truncated": good[:-1],
        "bad magic": b"XRWV" + good[4:],
        "bad version": good[:4] + struct.pack("<I", 2) + good[8:],
        "root out of range": good[:8] + struct.pack("<Q", len(good)) + good[16:],
        "root inside header": good[:8] + struct.pack("<Q", 0) + good[16:],
    }
    header = struct.Struct("<4sIQ")
    node = struct.Struct("<B7xQQ")

    def single(node_type, payload, count, extra=b""):
        body = extra
        root = header.size + len(body)
        return header.pack(b"ORWV", 1, root) + body + node.pack(node_type, payload, count)

    vectors["unknown type"] = single(1, 0, 0)
    vectors["null type"] = single(0, 0, 0)
    vectors["boolean two"] = single(2, 2, 0)
    vectors["invalid utf-8"] = single(5, header.size, 2, b"\xc3\x28")
    vectors["string out of range"] = single(5, header.size, 100, b"ab")
    vectors["array count overflow"] = single(7, header.size, 2**61)
    key = node.pack(5, header.size, 1)
    duplicate = b"k" + key
    table = struct.pack("<QQQQ", header.size + 1, header.size + 1, header.size + 1, header.size + 1)
    body = duplicate + table
    vectors["duplicate keys"] = header.pack(b"ORWV", 1, header.size + len(body)) + body + node.pack(
        8, header.size + len(duplicate), 2)
    offsets = []
    data = bytearray(header.pack(b"ORWV", 1, 0))
    previous = None
    for _ in range(200):
        if previous is None:
            payload = len(data)
            offsets.append(len(data))
            data.extend(node.pack(7, payload, 0))
        else:
            table_offset = len(data)
            data.extend(struct.pack("<Q", previous))
            offsets.append(len(data))
            data.extend(node.pack(7, table_offset, 1))
        previous = offsets[-1]
    struct.pack_into("<Q", data, 8, previous)
    vectors["too deep"] = bytes(data)
    return vectors


C_HARNESS = r"""
#include <stdio.h>
#include "OffsetArchive.inc"
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 2;
    unsigned char buffer[1 << 20];
    size_t size = fread(buffer, 1, sizeof(buffer), input);
    fclose(input);
    OrwvValue *value = orwv_decode(buffer, size);
    if (!value) return 3;
    unsigned char *bytes;
    size_t length;
    if (!orwv_encode(value, &bytes, &length)) return 4;
    orwv_free(value);
    FILE *output = fopen(argv[2], "wb");
    fwrite(bytes, 1, length, output);
    fclose(output);
    free(bytes);
    return 0;
}
"""

SWIFT_MAIN = r"""
let input = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
do {
    let value = try OffsetArchive.decodePropertyList(input)
    try OffsetArchive.encodePropertyList(value).write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
} catch {
    exit(3)
}
"""

NODE_HARNESS = r"""
const fs = require("fs");
const vm = require("vm");
const source = fs.readFileSync(process.argv[2], "utf8");
const start = source.indexOf("  const Orwv = (() => {");
const end = source.indexOf("  })();\n", start) + "  })();\n".length;
const context = { TextEncoder, TextDecoder, DataView, Uint8Array, ArrayBuffer, BigInt, Date, Number, Object, Math, Array, Error };
vm.createContext(context);
vm.runInContext(source.slice(start, end) + "\nthis.Orwv = Orwv;", context);
let failed = false;
for (const name of fs.readdirSync(process.argv[3]).sort()) {
  const input = fs.readFileSync(`${process.argv[3]}/${name}`);
  const corrupt = name.startsWith("corrupt-");
  try {
    const output = context.Orwv.encode(context.Orwv.decode(new Uint8Array(input)));
    if (corrupt) { console.log(`FAIL node accepted ${name}`); failed = true; }
    else if (Buffer.compare(Buffer.from(output), input) !== 0) { console.log(`FAIL node bytes differ for ${name}`); failed = true; }
  } catch (error) {
    if (!corrupt) { console.log(`FAIL node rejected ${name}: ${error.message}`); failed = true; }
  }
}
process.exit(failed ? 1 : 0);
"""


def check_native(label, command, directory, names):
    failures = 0
    for name in names:
        source = directory / name
        output = directory / (name + "." + label)
        result = subprocess.run([*command, str(source), str(output)], capture_output=True)
        corrupt = name.startswith("corrupt-")
        if corrupt:
            if result.returncode == 0:
                print(f"FAIL {label} accepted {name}")
                failures += 1
        elif result.returncode != 0:
            print(f"FAIL {label} rejected {name}: {result.returncode} {result.stderr.decode(errors='replace')[-300:]}")
            failures += 1
        elif output.read_bytes() != source.read_bytes():
            print(f"FAIL {label} bytes differ for {name}")
            failures += 1
    return failures


def main():
    failures = 0
    with tempfile.TemporaryDirectory(prefix="orwv-vectors-") as temporary:
        directory = pathlib.Path(temporary)
        vectors = directory / "vectors"
        portable = directory / "portable"
        vectors.mkdir()
        portable.mkdir()
        for index, (name, value, javascript) in enumerate(VECTORS):
            encoded = encode(value)
            if decode(encoded) != value:
                print(f"FAIL python round trip for {name}")
                failures += 1
            filename = f"vector-{index:02d}.orwv"
            (vectors / filename).write_bytes(encoded)
            if javascript:
                (portable / filename).write_bytes(encoded)
        for name, value in corrupt_vectors().items():
            filename = "corrupt-" + name.replace(" ", "-") + ".orwv"
            (vectors / filename).write_bytes(value)
            (portable / filename).write_bytes(value)
            try:
                decode(value)
                print(f"FAIL python accepted {filename}")
                failures += 1
            except (ValueError, UnicodeDecodeError, struct.error, RecursionError):
                pass
        names = sorted(path.name for path in vectors.iterdir())

        compiler = shutil.which("cc")
        if compiler:
            (directory / "harness.c").write_text(C_HARNESS)
            subprocess.run([compiler, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            "-I", str(ROOT / "Backend"), str(directory / "harness.c"), "-o", str(directory / "c-check")],
                           check=True)
            failures += check_native("c", [str(directory / "c-check")], vectors, names)
            print("checked C codec")
        else:
            print("SKIP C codec: no cc")

        if shutil.which("swiftc") and sys.platform == "darwin":
            source = (ROOT / "SharedSwift" / "OffsetArchive.swift").read_text()
            (directory / "main.swift").write_text(source + SWIFT_MAIN)
            subprocess.run(["swiftc", "-O", "-module-cache-path", str(directory / "cache"),
                            str(directory / "main.swift"), "-o", str(directory / "swift-check")],
                           check=True, capture_output=True)
            failures += check_native("swift", [str(directory / "swift-check")], vectors, names)
            print("checked Swift codec")
        else:
            print("SKIP Swift codec: requires swiftc on macOS")

        (directory / "harness.cjs").write_text(NODE_HARNESS)
        app = ROOT / "Resources" / "OuterShellWeb" / "app.js"
        if shutil.which("node"):
            command = ["node", str(directory / "harness.cjs"), str(app), str(portable)]
        elif shutil.which("docker"):
            shutil.copy(app, directory / "app.js")
            command = ["docker", "run", "--rm", "-v", f"{directory}:/work", "-w", "/work",
                       os.environ.get("OUTERSHELL_TEST_NODE_IMAGE", "node:22-alpine"),
                       "node", "harness.cjs", "app.js", "portable"]
        else:
            command = None
        if command:
            result = subprocess.run(command, capture_output=True, text=True)
            sys.stdout.write(result.stdout)
            if result.returncode != 0:
                failures += 1
                sys.stdout.write(result.stderr[-1000:])
            print("checked JavaScript codec")
        else:
            print("SKIP JavaScript codec: no node or docker")

    if failures:
        print(f"FAIL {failures} offset archive vector check(s)")
        return 1
    print("PASS offset archive vectors match across Python, C, Swift and JavaScript")
    return 0


if __name__ == "__main__":
    sys.exit(main())
