#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTERLOOP_ROOT="${OUTERLOOP_ROOT:-${ROOT}/../outerloop}"
TEST_DIR="$(mktemp -d /private/tmp/outershell-icon-cache.XXXXXX)"
trap 'rm -rf "$TEST_DIR"' EXIT
python3 - "$ROOT" "$TEST_DIR" <<'PYTHON'
from pathlib import Path
import sys
root, destination = map(Path, sys.argv[1:])
source = (root / "Frontend/BackendsContent.swift").read_text()
cache = source[source.index("private struct EndpointIconDiskCache:"):source.index("@MainActor")]
(destination / "main.swift").write_text("import Foundation\nimport CryptoKit\nimport Darwin\nimport ImageIO\n" + cache + r'''
let directory = CommandLine.arguments[2]
try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
setenv("TMPDIR", directory + "/", 1)
let result = OuterSandbox.apply(bundleId: "dev.outergroup.OuterLoop",
    runtimeDirectoryPath: directory, stagedFileDirectoryPath: directory)
precondition(result.success, result.error ?? "Sandbox setup failed")
private let cache = EndpointIconDiskCache(directory: URL(fileURLWithPath: directory).appendingPathComponent("cache"))
let url = URL(string: "http://server.invalid/api/icons/version-1.png")!
let bytes = Data("icon bytes".utf8)
let imageURL = URL(string: "http://server.invalid/api/icons/large.png")!
let smallURL = URL(string: "http://server.invalid/api/icons/small.png")!
func fixture(width: Int, height: Int) -> Data {
    let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
        bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 0.5))
    context.fill(CGRect(x: 0, y: 0, width: width, height: height))
    let encoded = NSMutableData()
    let destination = CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, context.makeImage()!, nil)
    precondition(CGImageDestinationFinalize(destination))
    return encoded as Data
}
if CommandLine.arguments[1] == "write" {
    cache.store(fixture(width: 1024, height: 512), for: imageURL)
    let large = cache.image(for: imageURL)!
    precondition(large.width == 96 && large.height == 48)
    precondition(large.alphaInfo != .none && large.alphaInfo != .noneSkipLast && large.alphaInfo != .noneSkipFirst)
    let small = cache.storeImage(fixture(width: 32, height: 16), for: smallURL)!
    precondition(small.width == 32 && small.height == 16)
    precondition(cache.storeImage(Data("invalid".utf8), for: URL(string: "http://server.invalid/bad")!) == nil)
    cache.store(Data("old bytes".utf8), for: url)
    cache.store(bytes, for: url)
    precondition(cache.data(for: url) == bytes)
} else {
    precondition(cache.data(for: imageURL, rendition: "96px-v1-") != nil)
    precondition(cache.data(for: smallURL, rendition: "96px-v1-") != nil)
    let large = cache.image(for: imageURL)!
    precondition(large.width == 96 && large.height == 48)
    let small = cache.image(for: smallURL)!
    precondition(small.width == 32 && small.height == 16)
    precondition(cache.data(for: url) == bytes)
    precondition(cache.data(for: URL(string: "http://server.invalid/api/icons/version-2.png")!) == nil)
    precondition(cache.data(for: URL(string: "http://other.invalid/api/icons/version-1.png")!) == nil)
    print("Sandboxed cache writes, resized cross-process reads, aspect ratio, alpha, small images, invalid images, and URL isolation passed.")
}
''')
PYTHON
swiftc -module-cache-path "$TEST_DIR/modules" \
    "$OUTERLOOP_ROOT/Outerframe/LibOuterframeContent/OuterSandbox.swift" \
    "$TEST_DIR/main.swift" -o "$TEST_DIR/test"
"$TEST_DIR/test" write "$TEST_DIR/runtime"
"$TEST_DIR/test" read "$TEST_DIR/runtime"
