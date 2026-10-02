#!/usr/bin/env python3
"""Exercise the production CoreText geometry against Unicode, wrapping, and scrolling."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "Frontend/BackendsContent.swift").read_text()
names = ["accessibilityTextResult", "createTextAreaLineFragments", "wrappedCreateTextRanges",
         "measuredCreateTextWidth", "makeCreateFieldLine", "createInputFont",
         "makeWorkspaceNameFieldLine", "workspaceNameInputFont", "utf16Offset"]
methods = []
for name in names:
    match = re.search(r"    private func " + name + r"\(.*?^    }", source, re.M | re.S)
    if not match:
        raise RuntimeError("Missing production method: " + name)
    methods.append(match.group().replace("private func", "func", 1))
structs = "\n".join(re.search(r"private struct " + name + r" \{.*?^}", source, re.M | re.S).group().replace("private struct", "struct", 1)
                    for name in ["CreateFieldLayout", "CreateTextLineFragment"])
fixture = r''' 
import AppKit
import QuartzCore
import CoreText
STRUCTS
final class Fixture {
    let rootLayer = CALayer()
    let createLayer = CALayer()
    var accessibilityTextTargets: [UInt32: String] = [1: "workspace"]
    var accessibilityCurrentNodes: [UInt32: OuterframeAccessibilityNode] = [:]
    var workspaceRenameFieldFrame = CGRect(x: 10, y: 20, width: 100, height: 48)
    var workspaceRenameTextFrame = CGRect(x: 10, y: 20, width: 100, height: 48)
    var isDockerfileFragmentPrompt = true
    var createFieldLayouts: [String: CreateFieldLayout] = [:]
    METHODS
}
let f = Fixture()
let text = "a😀e\u{301}z\nsecond line\n"
f.accessibilityCurrentNodes[1] = OuterframeAccessibilityNode(identifier: 1, role: .textArea, frame: f.workspaceRenameFieldFrame, value: text)
func query(_ kind: OuterframeAccessibilityTextQuery, _ range: NSRange = NSRange(location: 0, length: 0), _ point: CGPoint = .zero) -> OuterframeAccessibilityTextResult? {
    f.accessibilityTextResult(identifier: 1, query: kind, range: range, point: point)
}
precondition(query(.frameForRange, NSRange(location: 2, length: 1)) == nil)
let emoji = query(.frameForRange, NSRange(location: 1, length: 2))!
precondition(emoji.frame.width > 0 && emoji.frame.height == 16)
let hit = query(.rangeForPosition, pointRange, CGPoint(x: emoji.frame.midX, y: emoji.frame.midY))!
precondition(hit.range == NSRange(location: 1, length: 2))
precondition(query(.rangeForLine, NSRange(location: 0, length: 0))?.range == NSRange(location: 0, length: 7))
precondition(query(.lineForIndex, NSRange(location: 7, length: 0))?.index == 1)
precondition(query(.frameForRange, NSRange(location: 6, length: 1)) != nil)
precondition(query(.frameForRange, NSRange(location: text.utf16.count, length: 0)) != nil)
f.workspaceRenameTextFrame = f.workspaceRenameTextFrame.offsetBy(dx: 0, dy: 16)
precondition(query(.visibleRange)!.range.location >= 7)
f.workspaceRenameTextFrame.size.width = 24
precondition(query(.lineForIndex, NSRange(location: 7, length: 0))!.index > 1)
precondition(query(.rangeForLine, NSRange(location: 999, length: 0)) == nil)
f.accessibilityCurrentNodes[1] = OuterframeAccessibilityNode(identifier: 1, role: .textField, frame: f.workspaceRenameFieldFrame, value: "")
f.isDockerfileFragmentPrompt = false
precondition(query(.frameForRange)?.frame.width == 1)
precondition(query(.rangeForLine)?.range == NSRange(location: 0, length: 0))
f.rootLayer.addSublayer(f.createLayer)
f.createLayer.frame = CGRect(x: 30, y: 50, width: 300, height: 300)
f.createFieldLayouts["draft"] = CreateFieldLayout(fieldFrame: f.workspaceRenameFieldFrame, textFrame: f.workspaceRenameTextFrame, key: "draft", monospaced: true, multiline: false)
f.accessibilityTextTargets[1] = "draft"
let translated = query(.frameForRange)!.frame
precondition(translated.minX == f.workspaceRenameTextFrame.minX + 30)
precondition(translated.minY == f.workspaceRenameTextFrame.minY + 50)
print("PASS CoreText accessibility geometry: Unicode, caret, hit testing, visual lines, wrapping, scrolling")
'''.replace("pointRange", "NSRange(location: 0, length: 0)").replace("STRUCTS", structs).replace("METHODS", "\n".join(methods))
with tempfile.TemporaryDirectory(prefix="outershell-ax-geometry-") as directory:
    main = Path(directory) / "main.swift"
    binary = Path(directory) / "test"
    main.write_text(fixture)
    subprocess.run(["swiftc", str(root / "Vendor/OuterframeSwiftMethods/OuterframeAccessibility.swift"), str(main), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
