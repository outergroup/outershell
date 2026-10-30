#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'Frontend/BackendsContent.swift').read_text()

def block(start, end):
    return source[source.index(start):source.index(end, source.index(start))].replace('private ', '')

program = 'import Foundation\nstruct ContainerConfigurationInputError: Error { let message: String }\nclass Harness {\n'
program += block('    private struct OpenCodeRelease:', '    private var openCodePrompt:')
program += 'var openCodePrompt: String?\n'
program += block('    private func prepareOpenCodeTemplate(', '    private func openCodeText(')
program += block('    private func openCodeNavigationURL(', '    private func checkOpenCodeUpdates(')
program += r'''
}
let h = Harness()
let hash = String(repeating: "a", count: 64)
let release = Harness.OpenCodeRelease(tag_name: "v1.2.3", draft: false, prerelease: false,
    assets: [.init(name: "opencode-linux-arm64.tar.gz", digest: "sha256:" + hash)])
let output = try h.prepareOpenCodeTemplate("v=__OPENCODE_VERSION__; sha=__OPENCODE_SHA256_opencode-linux-arm64.tar.gz__", release: release)
assert(output == "v=1.2.3; sha=" + hash)
do {
    _ = try h.prepareOpenCodeTemplate("__OPENCODE_SHA256_missing.zip__", release: release)
    fatalError("Missing verified asset accepted")
} catch is ContainerConfigurationInputError {}
let invalid = Harness.OpenCodeRelease(tag_name: "v1.2.3", draft: false, prerelease: false,
    assets: [.init(name: "opencode-linux-arm64.tar.gz", digest: "sha256:invalid")])
do {
    _ = try h.prepareOpenCodeTemplate("__OPENCODE_SHA256_opencode-linux-arm64.tar.gz__", release: invalid)
    fatalError("Invalid digest accepted")
} catch is ContainerConfigurationInputError {}
let url = URL(string: "http://localhost:4096/work/session/ses_old?token=test&prompt=old#fragment")!
h.openCodePrompt = "Edit Dockerfile\nkeep changes & data"
let result = h.openCodeNavigationURL(url, serviceID: "org.outershell.OpenCode")
let parts = URLComponents(url: result, resolvingAgainstBaseURL: false)!
assert(parts.path == "/work/session")
assert(parts.fragment == nil)
assert(parts.queryItems == [URLQueryItem(name: "token", value: "test"), URLQueryItem(name: "prompt", value: "Edit Dockerfile\nkeep changes & data")])
assert(h.openCodePrompt == nil)
h.openCodePrompt = "Keep this pending"
assert(h.openCodeNavigationURL(url, serviceID: "other") == url)
assert(h.openCodePrompt == "Keep this pending")
let unusual = URL(string: "http://localhost/session-history/file")!
assert(h.openCodeNavigationURL(unusual, serviceID: "opencode") == unusual)
print("PASS: verified OpenCode templates, session reset, prompt encoding, and authentication preservation")
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'main.swift'
    path.write_text(program)
    subprocess.run(['swift', str(path)], check=True)
