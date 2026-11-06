#!/usr/bin/env python3
"""Verify endpoint rendering shares the scroll hierarchy and survives layer reuse."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Frontend/BackendsContent.swift').read_text()
methods = []
for name in ['recordMatchedIcon', 'recordMatchedText', 'addAppsSublayer']:
    match = re.search(r'    private func ' + name + r'\(.*?^    }', source, re.M | re.S)
    if not match:
        raise RuntimeError('Missing production method: ' + name)
    methods.append(match.group().replace('private func', 'func', 1))
fixture = r'''
import AppKit
import QuartzCore
final class Fixture {
    var activeAppsContentLayer = CALayer()
    var iconMatchLayers: [String: CALayer] = [:]
    var textMatchLayers: [String: CATextLayer] = [:]
    func makeLauncherIconLayer(image: CGImage?, symbolName: String?, symbolColor: NSColor, title: String, iconSize: CGFloat) -> CALayer { CALayer() }
    func configureLauncherIconLayer(_ layer: CALayer, image: CGImage?, symbolName: String?, symbolColor: NSColor, title: String, iconSize: CGFloat) {}
    func makeTextLayer(size: CGFloat, weight: NSFont.Weight, color: NSColor, alignment: CATextLayerAlignmentMode, italic: Bool) -> CATextLayer { CATextLayer() }
    func configureTextLayer(_ layer: CATextLayer, title: String, fontSize: CGFloat, weight: NSFont.Weight, color: NSColor, alignment: CATextLayerAlignmentMode, isWrapped: Bool, italic: Bool) {}
    METHODS
}
let f = Fixture()
let root = CALayer()
root.frame = CGRect(x: 0, y: 0, width: 800, height: 600)
let clip = CALayer()
clip.frame = CGRect(x: 20, y: 30, width: 700, height: 500)
clip.masksToBounds = true
root.addSublayer(clip)
let content = f.activeAppsContentLayer
content.frame = clip.bounds
clip.addSublayer(content)
let row = CALayer()
row.frame = CGRect(x: 10, y: 100, width: 200, height: 40)
content.addSublayer(row)
func render() {
    f.recordMatchedIcon(key: "endpoint", frame: CGRect(x: 10, y: 100, width: 24, height: 24), image: nil, symbolName: nil, title: "Endpoint")
    f.recordMatchedText(key: "endpoint", frame: CGRect(x: 44, y: 100, width: 150, height: 20), title: "Endpoint", fontSize: 13, weight: .regular, alignment: .left, isWrapped: false)
}
render()
guard let icon = f.iconMatchLayers["endpoint"], let title = f.textMatchLayers["endpoint"] else { fatalError("Missing endpoint layers") }
func verifyAlignment() {
    precondition(icon.superlayer === content && title.superlayer === content)
    for layer in [icon, title] {
        precondition(abs(root.convert(.zero, from: layer).y - root.convert(.zero, from: row).y) < 0.001)
    }
}
verifyAlignment()
content.frame.origin.y = 120
verifyAlignment()
for stretch: CGFloat in [-70, 55, 0] {
    content.sublayerTransform = CATransform3DMakeTranslation(0, stretch, 0)
    verifyAlignment()
    render()
    verifyAlignment()
}
content.sublayers?.forEach { $0.removeFromSuperlayer() }
render()
precondition(icon.superlayer === content && title.superlayer === content)
let workspaceContent = CALayer()
clip.addSublayer(workspaceContent)
f.activeAppsContentLayer = workspaceContent
render()
precondition(icon.superlayer === workspaceContent && title.superlayer === workspaceContent)
precondition(f.iconMatchLayers["endpoint"] === icon && f.textMatchLayers["endpoint"] === title)
print("PASS endpoint layers: normal scroll, both elastic edges, rerender, reuse, and pane changes")
'''.replace('METHODS', '\n'.join(methods))
with tempfile.TemporaryDirectory(prefix='endpoint-scroll-') as directory:
    directory = Path(directory)
    swift = directory / 'main.swift'
    swift.write_text(fixture)
    binary = directory / 'test'
    subprocess.run(['swiftc', '-module-cache-path', str(directory / 'modules'), str(swift), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
