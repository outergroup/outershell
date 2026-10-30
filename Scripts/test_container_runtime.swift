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
        let docker = check(output: "2 4109926400\n").resources
        precondition(docker == ContainerResourceLimits(cpus: 2, memoryBytes: 4_109_926_400))
        precondition(docker?.maxMemoryInMB == 3919 && docker?.defaultCPUs == 2 && docker?.defaultMemoryInMB == 3919)
        precondition(check(output: "unexpected").resources == nil && check(output: "unexpected").isAvailable)
        let large = ContainerResourceLimits(cpus: 16, memoryBytes: 68_719_476_736)
        precondition(large.defaultCPUs == 4 && large.defaultMemoryInMB == 8192)
        if case .success(let value) = docker!.validated(cpus: nil, memoryInMB: nil) {
            precondition(value.cpus == 2 && value.memoryInMB == 3919)
        } else { preconditionFailure() }
        if case .success = docker!.validated(cpus: 4, memoryInMB: 2048) { preconditionFailure() }
        if case .success = docker!.validated(cpus: 1, memoryInMB: 8192) { preconditionFailure() }
        if case .success = docker!.validated(cpus: 1, memoryInMB: 64) { preconditionFailure() }
        if case .failure = docker!.validated(cpus: 1, memoryInMB: 1536) { preconditionFailure() }
        if case .success = docker!.validated(cpus: "2", memoryInMB: 2048) { preconditionFailure() }
        precondition(docker!.runnable(cpus: 4, memoryInMB: 8192) == (2, 3919))
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
        print("PASS: missing, unsupported, stopped, ready, failed, resource limits, install-after-launch discovery, and bounded runtime probes")
    }
}
