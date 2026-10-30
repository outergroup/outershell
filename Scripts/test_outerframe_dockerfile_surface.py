#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'Frontend/BackendsContent.swift').read_text()
start = source.index('private final class DockerfileTextLayout')
end = source.index('private final class LogTextFragmentLayer', start)
classes = source[start:end].replace('private final class', 'final class')
program = '''import AppKit
import CoreText
import QuartzCore
''' + classes + '''
let longLine = String(repeating: "RUN echo café 🐚; ", count: 300)
let document = DockerfileTextLayout(text: Array(repeating: longLine, count: 200).joined(separator: "\\n") + "\\n")
assert(document.lines.count == 201)
assert(document.lines[1].start == longLine.count + 1)
assert(document.width > 640)
assert(document.visibleRows(top: 310, height: 320, scale: 1) == 0..<20)
assert(document.visibleRows(top: 630, height: 320, scale: 1) == 19..<40)
assert(document.visibleRows(top: 100000, height: 320, scale: 1).isEmpty)
let layer = DockerfileTextViewportLayer()
layer.bounds = CGRect(x: 0, y: 0, width: 640, height: 320)
CATransaction.begin()
CATransaction.setDisableActions(false)
for key in ["contents", "bounds", "position", "opacity", "onOrderIn", "onOrderOut"] {
    assert(layer.action(forKey: key) == nil, "Unexpected implicit action: " + key)
}
CATransaction.commit()
layer.textLayout = document
layer.textColor = CGColor(gray: 1, alpha: 1)
func checkBudget(_ layer: DockerfileTextViewportLayer) {
    let rasterArea = layer.tiles.values.reduce(CGFloat(0)) { $0 + $1.bounds.width * $1.bounds.height }
    let limit = (layer.bounds.width + 4 * 512) * (layer.bounds.height + 6 * 16 * layer.textScale)
    assert(rasterArea <= limit, "Tile budget exceeded")
    assert(layer.tiles.values.allSatisfy { $0.bounds.width <= 512 })
}
layer.textOrigin = CGPoint(x: 12, y: 310)
layer.updateVisibleTiles()
for tile in layer.tiles.values { tile.display() }
let initial = layer.tiles
layer.textOrigin.y += 1
layer.updateVisibleTiles()
assert(initial.count == layer.tiles.count)
for (key, tile) in initial {
    assert(layer.tiles[key] === tile, "Scrolling replaced a cached tile")
    assert(!tile.needsDisplay(), "Scrolling invalidated a cached bitmap")
    assert(tile.action(forKey: "contents") == nil)
}
layer.textOrigin.y += 32
layer.updateVisibleTiles()
let added = layer.tiles.filter { initial[$0.key] == nil }
assert(!added.isEmpty && added.count <= 2 * 4, "Redrew more than entering rows")
for (key, tile) in initial where layer.tiles[key] != nil {
    assert(layer.tiles[key] === tile && !tile.needsDisplay())
}
layer.textOrigin.y += 1600
layer.updateVisibleTiles()
assert(initial.keys.allSatisfy { layer.tiles[$0] == nil }, "Retained offscreen rows")
assert(initial.values.allSatisfy { $0.contents == nil }, "Retained offscreen bitmaps")
checkBudget(layer)
let vertical = layer.tiles
layer.textOrigin.x -= 512
layer.updateVisibleTiles()
assert(vertical.contains { layer.tiles[$0.key] === $0.value }, "Horizontal scrolling discarded overlapping tiles")
checkBudget(layer)
for scale: CGFloat in [0.25, 0.5, 1, 2, 3] {
    layer.textScale = scale
    layer.textOrigin = CGPoint(x: -200 * scale, y: 310)
    layer.updateVisibleTiles()
    checkBudget(layer)
    let context = CGContext(data: nil, width: 1280, height: 640, bitsPerComponent: 8,
        bytesPerRow: 1280 * 4, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.scaleBy(x: 2, y: 2)
    layer.render(in: context)
    let bytes = context.data!.assumingMemoryBound(to: UInt8.self)
    assert((0..<(1280 * 640 * 4)).contains { bytes[$0] != 0 }, "No visible text rendered")
    assert(layer.textLayout === document, "Zoom recreated document layout")
}
let beforeColor = layer.tiles
layer.textColor = CGColor(gray: 0, alpha: 1)
layer.updateVisibleTiles()
assert(beforeColor.allSatisfy { layer.tiles[$0.key] !== $0.value }, "Appearance left stale bitmaps")
layer.discardTiles()
assert(layer.tiles.isEmpty && (layer.sublayers ?? []).isEmpty)
print("PASS: pixel scroll reuses bitmaps without redraw; only entering rows rasterize; offscreen eviction; horizontal reuse; bounded tiles at 25–300% zoom; Unicode; no implicit actions")
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'main.swift'
    path.write_text(program)
    subprocess.run(['swift', str(path)], check=True)
