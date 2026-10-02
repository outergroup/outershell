import Foundation

@main
struct RuntimeTests {
    static func main() throws {
        let executable = URL(fileURLWithPath: "/bin/sh")
        func check(apple: Bool = false, installed: Bool = true, supported: Bool = true,
                   code: Int32 = 0, output: String = "1") -> ContainerRuntimeReadiness {
            ContainerRuntime.readiness(apple: apple, executable: installed ? executable : nil,
                                       appleSupported: supported) { _, _ in (code, output) }
        }
        precondition(check(installed: false).status == "notInstalled")
        precondition(check(apple: true, supported: false).status == "unsupported")
        precondition(check().isAvailable)
        precondition(check(code: 1, output: "Cannot connect to the Docker daemon").status == "stopped")
        precondition(check(code: 1, output: "permission denied").status == "failed")
        precondition(check(apple: true, code: 1, output: "container system start").status == "stopped")
        precondition(check(code: 124, output: "did not respond").status == "failed")
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("runtime-test-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let cli = root.appendingPathComponent("test-runtime")
        precondition(ContainerRuntime.executable(named: "test-runtime", environment: ["PATH": root.path]) == nil)
        try "#!/bin/sh\nprintf ready\n".write(to: cli, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: cli.path)
        precondition(ContainerRuntime.executable(named: "test-runtime", environment: ["PATH": root.path]) == cli)
        let ready = try ContainerRuntime.probe(cli, arguments: [])
        precondition(ready.0 == 0 && ready.1 == "ready")
        try "#!/bin/sh\nexec /bin/sleep 10\n".write(to: cli, atomically: true, encoding: .utf8)
        let started = Date()
        let timeout = try ContainerRuntime.probe(cli, arguments: [], timeout: 0.1)
        precondition(timeout.0 == 124 && Date().timeIntervalSince(started) < 2)
        print("PASS: missing, unsupported, stopped, ready, failed, install-after-launch discovery, and bounded runtime probes")
    }
}
