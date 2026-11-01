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
    let diskCache = EndpointIconDiskCache(directory: directory)
    let cacheFiles = try! FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
    let bitmapFile = cacheFiles.first { $0.lastPathComponent.hasPrefix("96px-srgb-bgra-v3-") }!
    let raw = try! Data(contentsOf: bitmapFile)
    precondition(raw.count == 12 + 96 * 48 * 4)
    let diskStart = CFAbsoluteTimeGetCurrent()
    for _ in 0..<100 { precondition(diskCache.image(for: url)?.width == 96) }
    let diskTime = (CFAbsoluteTimeGetCurrent() - diskStart) / 100
    var badMagic = raw; badMagic[0] = 0
    var badVersion = raw; badVersion[4] = 255
    var zeroWidth = raw; zeroWidth[8] = 0
    var largeHeight = raw; largeHeight[11] = 255
    for invalid in [Data(raw.prefix(8)), Data(raw.dropLast()), raw + Data([0]), badMagic, badVersion, zeroWidth, largeHeight] {
        try! invalid.write(to: bitmapFile)
        precondition(diskCache.image(for: url) == nil)
    }
    diskCache.store(data, for: url)
    precondition(diskCache.image(for: url)?.width == 96)
    precondition(try! Data(contentsOf: bitmapFile) == raw)
    print(String(format: "Raw disk cache hit average %.3f ms", diskTime * 1000))
    let svg = try! Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[2]))
    let icon = decodedIconCGImage(svg)!
    precondition(icon.width == 96 && icon.height == 96)
    precondition(icon.colorSpace?.name == CGColorSpace.sRGB)
    precondition(decodedIconCGImage(svg) === icon)
    precondition(decodedIconCGImage(Data()) == nil)
    precondition(decodedIconCGImage(Data("invalid image".utf8)) == nil)
    print("PASS resized P3/alpha PNG, raw bitmap roundtrip and corruption recovery, identical memory-cache hits, SVG, invalid input")
    print(String(format: "PNG cold %.3f ms; warm average %.3f ms", cold * 1000, warm * 1000))
}
'''
with tempfile.TemporaryDirectory(prefix="outershell-icon-test-") as temporary:
    work = pathlib.Path(temporary)
    (work / "main.swift").write_text(test)
    (work / "icon.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 96 96"><rect width="96" height="96" rx="20" fill="#0088ff"/></svg>')
    subprocess.run(["xcrun", "swiftc", "-O", "-module-cache-path", str(work / "modules"), str(work / "main.swift"), "-o", str(work / "test")], check=True)
    subprocess.run([str(work / "test"), str(work / "icons"), str(work / "icon.svg")], check=True)
