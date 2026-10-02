import Foundation
import Darwin

struct ContainerRuntimeReadiness {
    let status: String
    let detail: String
    let setupURL: String
    var isAvailable: Bool { status == "ready" }
}

enum ContainerRuntime {
    static let dockerSetupURL = "https://docs.docker.com/desktop/setup/install/mac-install/"
    static let appleSetupURL = "https://github.com/apple/container#readme"

    static func executable(named name: String, environment: [String: String] = ProcessInfo.processInfo.environment) -> URL? {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let directories = (environment["PATH"] ?? "").split(separator: ":").map(String.init) + [
            "\(home)/.docker/bin", "/usr/local/bin", "/opt/homebrew/bin",
            "/Applications/Docker.app/Contents/Resources/bin"
        ]
        return directories.map { URL(fileURLWithPath: $0).appendingPathComponent(name) }
            .first { FileManager.default.isExecutableFile(atPath: $0.path) }
    }

    static func readiness(apple: Bool, executable: URL?, appleSupported: Bool,
                          probe: (URL, [String]) throws -> (Int32, String)) -> ContainerRuntimeReadiness {
        let url = apple ? appleSetupURL : dockerSetupURL
        if apple && !appleSupported {
            return .init(status: "unsupported", detail: "Apple container requires Apple Silicon and macOS 26 or later on this server. Use Docker on other Macs.", setupURL: url)
        }
        guard let executable else {
            return .init(status: "notInstalled", detail: apple
                ? "Install Apple container on this server, then choose Check again."
                : "Install and open Docker Desktop on this server, finish its setup, then choose Check again.", setupURL: url)
        }
        do {
            let result = try probe(executable, apple ? ["system", "status"] : ["info", "--format", "{{.ServerVersion}}"])
            if result.0 == 0 {
                return .init(status: "ready", detail: apple ? "Apple container is ready on this server." : "Docker is ready using this server user's configured Docker endpoint.", setupURL: url)
            }
            let message = result.1.lowercased()
            let stopped = message.contains("cannot connect") || message.contains("connection refused") ||
                message.contains("is not running") || message.contains("not started") ||
                message.contains("container system start") || message.contains("xpc connection error") ||
                message.contains("connection invalid") || message.contains("no such file")
            if apple && stopped {
                return .init(status: "stopped", detail: "Apple container is installed but stopped. Creating a container will start it and download its kernel if needed. You can also run container system start on this server, then choose Check again.", setupURL: url)
            }
            return .init(status: stopped ? "stopped" : "failed", detail: (apple
                ? "Apple container is not ready. Run container system start on this server, then choose Check again."
                : "Docker is not ready. Open Docker Desktop on this server and check that docker info succeeds, then choose Check again.") + " " + String(result.1.trimmingCharacters(in: .whitespacesAndNewlines).prefix(400)), setupURL: url)
        } catch {
            return .init(status: "failed", detail: "Runtime check failed: \(error.localizedDescription) Choose Check again after fixing the runtime on this server.", setupURL: url)
        }
    }

    static func probe(_ executable: URL, arguments: [String], timeout: TimeInterval = 3) throws -> (Int32, String) {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("runtime-check-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let output = directory.appendingPathComponent("output")
        try Data().write(to: output)
        let handle = try FileHandle(forWritingTo: output)
        defer { try? handle.close() }
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = handle
        process.standardError = handle
        try process.run()
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        while process.isRunning && ProcessInfo.processInfo.systemUptime < deadline {
            Thread.sleep(forTimeInterval: 0.02)
        }
        if process.isRunning {
            kill(process.processIdentifier, SIGKILL)
            process.waitUntilExit()
            return (124, "The runtime did not respond within \(Int(timeout)) seconds.")
        }
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: try Data(contentsOf: output), as: UTF8.self))
    }
}
