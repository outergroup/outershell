#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'Frontend/BackendsContent.swift').read_text()
start = source.index('    private func scheduleDockerfileMagnification(')
end = source.index('    private func createInputFont(', start)
methods = source[start:end].replace('private ', '')
program = '''import Foundation
class Harness {
    var pendingDockerfileTextScale: CGFloat?
    var dockerfileZoomUpdateScheduled = false
    var dockerfileTextScale: CGFloat = 1
    var containerConfigurationHorizontalScroll: CGFloat = 0
    var containerConfigurationTextScroll: CGFloat = 100
    var containerConfigurationTextFragmentsText = "cached"
    var isContainerConfigurationEditorVisible = true
    enum Tab { case dockerfile, mounts }
    var containerConfigurationTab = Tab.dockerfile
    var currentSize = CGSize(width: 900, height: 620)
    var renders = 0
    func withoutImplicitAnimations(_ body: () -> Void) { body() }
    func renderWorkspacePanelIfNeeded(width: CGFloat, height: CGFloat) { renders += 1 }
''' + methods + '''
}
func drain() { RunLoop.main.run(until: Date().addingTimeInterval(0.06)) }
let h = Harness()
for _ in 0..<500 { h.scheduleDockerfileMagnification(0.001) }
assert(h.renders == 0)
drain()
assert(h.renders == 1)
assert(abs(h.dockerfileTextScale - pow(1.001, 500)) < 0.000001)
assert(abs(h.containerConfigurationTextScroll - 100 * h.dockerfileTextScale) < 0.000001)
drain()
assert(h.renders == 1, "Gesture left a render backlog")
h.scheduleDockerfileMagnification(10)
drain()
assert(h.dockerfileTextScale == 3)
h.scheduleDockerfileMagnification(1)
drain()
assert(h.renders == 2, "Clamped zoom should not render")
h.scheduleDockerfileMagnification(-0.5)
h.containerConfigurationTab = .mounts
drain()
assert(h.renders == 2)
h.containerConfigurationTab = .dockerfile
h.scheduleDockerfileMagnification(-0.5)
h.resizeDockerfileText(1)
drain()
assert(h.dockerfileTextScale == 1, "Old gesture overwrote explicit zoom")
print("PASS: 500 gesture events coalesced, no backlog, bounds, scroll scaling, tab changes, explicit zoom")
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'main.swift'
    path.write_text(program)
    subprocess.run(['swift', str(path)], check=True)
