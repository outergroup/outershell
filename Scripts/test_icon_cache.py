#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "Frontend/BackendsContent.swift").read_text()
cache = source[source.index("private struct EndpointIconDiskCache:"):source.index("@MainActor\n@objc public final class BackendsContent")]
decoder = source[source.index("private func normalizedIconCGImage("):source.index("private struct LogFileRecord")]
test = r'''import AppKit
import CryptoKit
import Foundation
import ImageIO
import QuartzCore

'''+cache+decoder+r'''
autoreleasepool {
    let directory = URL(fileURLWithPath: CommandLine.arguments[1])
    let space = CGColorSpace(name: CGColorSpace.displayP3)!
    let context = CGContext(data: nil, width: 1024, height: 512, bitsPerComponent: 8,
                            bytesPerRow: 4096, space: space,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.setFillColor(CGColor(colorSpace: space, components: [0.8, 0.2, 0.1, 0.5])!)
    context.fill(CGRect(x: 0, y: 0, width: 1024, height: 512))
    let encoded = NSMutableData()
    let destination = CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, context.makeImage()!, nil)
    precondition(CGImageDestinationFinalize(destination))
    let data = encoded as Data
    EmbeddedIconCache.shared.configure(directory: directory)
    let start = CFAbsoluteTimeGetCurrent()
    let first = decodedIconCGImage(data)!
    let cold = CFAbsoluteTimeGetCurrent() - start
    precondition(first.width == 96 && first.height == 48)
    precondition(first.colorSpace?.name == CGColorSpace.sRGB)
    precondition(first.alphaInfo == .premultipliedFirst && first.bitsPerPixel == 32)
    let warmStart = CFAbsoluteTimeGetCurrent()
    for _ in 0..<100 { precondition(decodedIconCGImage(data) === first) }
    let warm = (CFAbsoluteTimeGetCurrent() - warmStart) / 100
    let url = URL(string: "outershell-icon://embedded/" + SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined())!
    let disk = EndpointIconDiskCache(directory: directory).image(for: url)!
    precondition(disk.width == 96 && disk.height == 48 && disk.colorSpace?.name == CGColorSpace.sRGB)
    precondition(disk.dataProvider?.data as Data? == first.dataProvider?.data as Data?)
    let svg = try! Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[2]))
    let icon = decodedIconCGImage(svg)!
    precondition(icon.width == 96 && icon.height == 96)
    precondition(icon.colorSpace?.name == CGColorSpace.sRGB)
    precondition(decodedIconCGImage(svg) === icon)
    precondition(decodedIconCGImage(Data()) == nil)
    precondition(decodedIconCGImage(Data("invalid image".utf8)) == nil)
    print("PASS resized P3/alpha PNG, normalized disk roundtrip, identical memory-cache hits, OpenCode SVG, invalid input")
    print(String(format: "PNG cold %.3f ms; warm average %.3f ms", cold * 1000, warm * 1000))
}
'''
with tempfile.TemporaryDirectory(prefix="outershell-icon-test-") as temporary:
    work = pathlib.Path(temporary)
    (work / "main.swift").write_text(test)
    subprocess.run(["xcrun", "swiftc", "-O", "-module-cache-path", str(work / "modules"), str(work / "main.swift"), "-o", str(work / "test")], check=True)
    subprocess.run([str(work / "test"), str(work / "icons"), str(root / "Resources/OuterShellWeb/icons/opencode.svg")], check=True)
