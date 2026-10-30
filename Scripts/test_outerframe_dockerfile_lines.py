#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'Frontend/BackendsContent.swift').read_text()
a = source.index('    private func createTextAreaLineFragments(')
b = source.index('    private func wrappedCreateTextRanges(', a)
method = source[a:b].replace('private ', '')
classes = source[source.index('private final class DockerfileTextLayout'):source.index('private final class DockerfileTextTileLayer')].replace('private final class', 'final class')
program = '''import AppKit
import CoreText
''' + classes + '''
struct CreateFieldLayout { var textFrame: CGRect; var key: String; var monospaced = true }
struct CreateTextLineFragment { var text: String; var start: Int; var end: Int; var y: CGFloat }
class Harness {
    var editorLineHeight: CGFloat = 16
    var usesUnwrappedDockerfile = true
    var wrapCalls = 0
    func cachedDockerfileLayout(for text: String) -> DockerfileTextLayout { DockerfileTextLayout(text: text) }
    func wrappedCreateTextRanges(for text: String, maxWidth: CGFloat, monospaced: Bool) -> [Range<Int>] {
        wrapCalls += 1
        return [0..<text.count]
    }
''' + method + '''
}
let h = Harness()
let long = String(repeating: "RUN echo 🐚 café; ", count: 1000)
let text = long + "\\n\\nCMD bash\\n"
let narrow = CreateFieldLayout(textFrame: CGRect(x: 12, y: 10, width: 100, height: 400), key: "containerConfiguration")
let lines = h.createTextAreaLineFragments(text: text, layout: narrow)
assert(lines.count == 4)
assert(lines[0].text == long && lines[0].end == long.count)
assert(lines[1].text.isEmpty && lines[1].start == long.count + 1)
assert(lines[2].start == long.count + 2)
assert(lines[3].text.isEmpty)
assert(h.wrapCalls == 0)
assert(lines[1].y == lines[0].y - 16)
let cursorLayout = CreateFieldLayout(textFrame: narrow.textFrame, key: "dockerfileFragment")
assert(h.createTextAreaLineFragments(text: text, layout: cursorLayout).count == 4)
assert(h.wrapCalls == 0)
h.usesUnwrappedDockerfile = false
_ = h.createTextAreaLineFragments(text: text, layout: narrow)
assert(h.wrapCalls == 4)
print("PASS: long Unicode lines, blank/trailing lines, character offsets, caret layout, wrapping isolation")
'''
a = source.index('    private func navigateToRecipeSafeSpace(')
b = source.index('    private func applyNavigation(', a)
assert 'pushHistoryEntry' not in source[a:b]
a = source.index('    private func returnFromRecipeSafeSpace()')
b = source.index('    private func applyMode(', a)
assert 'goBackInHistory' not in source[a:b]
assert 'urlForRecipeSafeSpace' not in source
assert 'components.path = "/safe-spaces/' not in source
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'main.swift'
    path.write_text(program)
    subprocess.run(['swift', str(path)], check=True)
print('PASS: container editor does not create a safe-spaces URL')
