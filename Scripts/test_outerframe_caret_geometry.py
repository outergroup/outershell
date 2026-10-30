#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'Frontend/BackendsContent.swift').read_text()
a = source.index('private final class DockerfileTextLayout')
b = source.index('private final class DockerfileTextTileLayer', a)
layout = source[a:b].replace('private final class', 'final class')
a = source.index('    private var textInputGeometryUpdateScheduled')
b = source.index('    private func sendFocusedTextInputGeometryUpdate()', a)
queue = source[a:b].replace('private ', '')
program = '''import AppKit
import CoreText
''' + layout + '''
struct OuterframeContentTextInputGeometry {
    let fieldID: UUID
    let rect: CGRect
}
class Host {
    var messages: [OuterframeContentTextInputGeometry?] = []
    func sendTextInputGeometryUpdate(_ value: OuterframeContentTextInputGeometry?) { messages.append(value) }
}
class Harness {
    let outerframeHost = Host()
''' + queue + '''
}
let text = "RUN echo café 🐚é\\n\\nCMD bash\\n"
let layout = DockerfileTextLayout(text: text)
for (row, line) in layout.lines.enumerated() {
    for character in 0...(line.end - line.start) {
        let result = layout.caret(at: line.start + character)
        assert(result.row == row)
        let index = line.text.index(line.text.startIndex, offsetBy: character)
        var secondary: CGFloat = 0
        let expected = CTLineGetOffsetForStringIndex(line.shaped, line.text[..<index].utf16.count, &secondary)
        assert(abs(result.offset - max(expected, secondary)) < 0.001)
    }
}
let big = DockerfileTextLayout(text: Array(repeating: "RUN echo hello", count: 10000).joined(separator: "\\n"))
let position = big.lines[9000].start + 7
let caret = big.caret(at: position)
for _ in 0..<10000 {
    let cached = big.caret(at: position)
    assert(cached.row == 9000 && cached.offset == caret.offset)
}
let changed = DockerfileTextLayout(text: "a\\nb")
assert(changed.caret(at: 2).row == 1)
assert(DockerfileTextLayout(text: "").caret(at: 0).offset == 0)
func drain() { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
let h = Harness()
let id = UUID()
for index in 0..<500 {
    h.queueTextInputGeometryUpdate(.init(fieldID: id, rect: CGRect(x: index, y: 20, width: 1, height: 18)))
}
assert(h.outerframeHost.messages.isEmpty)
drain()
assert(h.outerframeHost.messages.count == 1)
assert(h.outerframeHost.messages.last??.rect.minX == 499)
let last = h.outerframeHost.messages.last!
h.queueTextInputGeometryUpdate(nil)
h.queueTextInputGeometryUpdate(last)
drain()
assert(h.outerframeHost.messages.count == 1, "Sent unchanged geometry or intermediate clear")
h.queueTextInputGeometryUpdate(.init(fieldID: UUID(), rect: last!.rect))
drain()
assert(h.outerframeHost.messages.count == 2, "Lost focus change")
h.queueTextInputGeometryUpdate(nil)
drain()
assert(h.outerframeHost.messages.count == 3 && h.outerframeHost.messages.last! == nil)
h.queueTextInputGeometryUpdate(nil)
drain()
assert(h.outerframeHost.messages.count == 3)
print("PASS: cached caret offsets, Unicode/newlines/empty text, large-document lookup, 500-event coalescing, deduplication, focus changes and clears")
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'main.swift'
    path.write_text(program)
    subprocess.run(['swift', str(path)], check=True)
