#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

source = (pathlib.Path(__file__).resolve().parents[1] / "Agent/SafeSpaceManager.swift").read_text()
policy = source.split("private struct SafeSpaceEndpointPublicationFailures {", 1)[1].split("final class SafeSpaceManager:", 1)[0]
program = "import Foundation\nprivate struct SafeSpaceEndpointPublicationFailures {" + policy + r"""
private var failures = SafeSpaceEndpointPublicationFailures()
assert(!failures.record("missing", for: "a/socket", now: 0))
assert(!failures.record("missing", for: "a/socket", now: 29))
assert(failures.record("missing", for: "a/socket", now: 30))
assert(!failures.record("missing", for: "a/socket", now: 60))
assert(failures.record("denied", for: "a/socket", now: 61))
assert(!failures.record("denied", for: "a/socket", now: 62))
assert(failures.recover("a/socket"))
assert(!failures.recover("a/socket"))
assert(!failures.record("missing", for: "a/socket", now: 70))
assert(!failures.recover("a/socket"))
assert(!failures.record("missing", for: "a/socket", now: 100))
assert(!failures.record("missing", for: "b/socket", now: 100))
failures.retain([], prefix: "a/")
assert(!failures.record("missing", for: "a/socket", now: 140))
assert(failures.record("missing", for: "b/socket", now: 140))
failures.retain(["b/socket"], prefix: "b/")
assert(!failures.record("missing", for: "b/socket", now: 150))
print("Container endpoint publication failure checks passed")
"""
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory) / "main.swift"
    path.write_text(program)
    subprocess.run(["swift", str(path)], check=True)

publication = source.split("private func publishExternalAppEndpoints(", 1)[1].split("private func updateExternalAppEndpoint", 1)[0]
assert publication.index("if !snapshot.isRunning || snapshot.publishedPort != nil") < publication.index("try await publishSocket")
