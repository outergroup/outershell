import Foundation
import Darwin

struct ContainerRuntimeReadiness {
    let status: String
    let detail: String
    let setupURL: String
    var resources: ContainerResourceLimits? = nil
    var isAvailable: Bool { status == "ready" }
}

struct ContainerResourceLimits: Equatable {
    static let preferredCPUs = 4
    static let preferredMemoryInMB = 8192
    static let minimumMemoryInMB = 128

    let maxCPUs: Int
    let maxMemoryInMB: Int

    init(cpus: Int, memoryBytes: Int64) {
        maxCPUs = max(cpus, 1)
        maxMemoryInMB = max(Int(memoryBytes / 1_048_576), Self.minimumMemoryInMB)
    }

    init?(dockerInfo: String) {
        let fields = dockerInfo.split(whereSeparator: \.isWhitespace)
        guard fields.count == 2, let cpus = Int(fields[0]), let memory = Int64(fields[1]) else { return nil }
        self.init(cpus: cpus, memoryBytes: memory)
    }

    static var host: ContainerResourceLimits {
        .init(cpus: ProcessInfo.processInfo.activeProcessorCount,
              memoryBytes: Int64(clamping: ProcessInfo.processInfo.physicalMemory))
    }

    var defaultCPUs: Int { min(Self.preferredCPUs, maxCPUs) }
    var defaultMemoryInMB: Int { min(Self.preferredMemoryInMB, maxMemoryInMB) }

    var dictionary: [String: Int] {
        ["maxCPUs": maxCPUs, "maxMemoryInMB": maxMemoryInMB,
         "defaultCPUs": defaultCPUs, "defaultMemoryInMB": defaultMemoryInMB]
    }

    func validated(cpus: Any?, memoryInMB: Any?) -> Result<(cpus: Int, memoryInMB: Int), ContainerResourceError> {
        let cpus = cpus.map { $0 as? Int } ?? defaultCPUs
        let memory = memoryInMB.map { $0 as? Int } ?? defaultMemoryInMB
        guard let cpus, (1...maxCPUs).contains(cpus) else {
            return .failure(.init(message: "Choose between 1 and \(maxCPUs) CPUs. This runtime has \(maxCPUs)."))
        }
        guard let memory, (Self.minimumMemoryInMB...maxMemoryInMB).contains(memory) else {
            return .failure(.init(message: "Choose between \(Self.minimumMemoryInMB) MB and \(maxMemoryInMB) MB of memory. This runtime has \(maxMemoryInMB) MB."))
        }
        return .success((cpus, memory))
    }

    func runnable(cpus: Int, memoryInMB: Int) -> (cpus: Int, memoryInMB: Int) {
        (min(max(cpus, 1), maxCPUs), min(max(memoryInMB, Self.minimumMemoryInMB), maxMemoryInMB))
    }
}

struct ContainerResourceError: Error {
    let message: String
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
            let result = try probe(executable, apple ? ["system", "status"] : ["info", "--format", "{{.NCPU}} {{.MemTotal}}"])
            if result.0 == 0 {
                return .init(status: "ready", detail: apple ? "Apple container is ready on this server." : "Docker is ready using this server user's configured Docker endpoint.", setupURL: url,
                             resources: apple ? .host : ContainerResourceLimits(dockerInfo: result.1))
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

struct ContainerImageStorage {
    struct Image {
        let reference: String
        let digests: Set<String>
        let snapshotDigests: Set<String>
        var isOwnedDangling = false
        var sizeInBytes: Int64? = nil
    }

    static func dockerImages(from data: Data) throws -> [Image] {
        guard let values = try JSONSerialization.jsonObject(with: data) as? [[String: Any]], !values.isEmpty else {
            throw CocoaError(.fileReadCorruptFile)
        }
        return try values.flatMap { value -> [Image] in
            guard let id = value["Id"] as? String, let config = value["Config"] as? [String: Any] else {
                throw CocoaError(.fileReadCorruptFile)
            }
            let tags = (value["RepoTags"] as? [String] ?? []).filter { $0 != "<none>:<none>" }
            let labels = config["Labels"] as? [String: String] ?? [:]
            let repositoryDigests = value["RepoDigests"] as? [String] ?? []
            let digests = Set(repositoryDigests + repositoryDigests.compactMap { $0.split(separator: "@").last.map(String.init) } + [id])
            return (tags.isEmpty ? [id] : tags).map { reference in
                Image(reference: reference, digests: digests, snapshotDigests: [],
                      isOwnedDangling: tags.isEmpty && labels["org.outershell.managed"] == "true",
                      sizeInBytes: (value["Size"] as? NSNumber)?.int64Value)
            }
        }
    }

    static func images(from data: Data) throws -> [Image] {
        guard let values = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            throw CocoaError(.fileReadCorruptFile)
        }
        return try values.map { value in
            guard let configuration = value["configuration"] as? [String: Any],
                  let reference = configuration["name"] as? String,
                  let descriptor = configuration["descriptor"] as? [String: Any],
                  let digest = descriptor["digest"] as? String,
                  let variants = value["variants"] as? [[String: Any]] else {
                throw CocoaError(.fileReadCorruptFile)
            }
            let snapshots = try Set(variants.map { variant -> String in
                guard let digest = variant["digest"] as? String else {
                    throw CocoaError(.fileReadCorruptFile)
                }
                return digest
            })
            return Image(reference: reference, digests: snapshots.union([digest]), snapshotDigests: snapshots)
        }
    }

    static func containerReferences(from data: Data) throws -> Set<String> {
        guard let containers = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            throw CocoaError(.fileReadCorruptFile)
        }
        var references = Set<String>()
        for container in containers {
            guard let configuration = container["configuration"] as? [String: Any],
                  let image = configuration["image"] as? [String: Any],
                  let reference = image["reference"] as? String,
                  let descriptor = image["descriptor"] as? [String: Any],
                  let digest = descriptor["digest"] as? String else {
                throw CocoaError(.fileReadCorruptFile)
            }
            references.formUnion([reference, digest])
        }
        return references
    }

    static func candidates(images: [Image], protected: Set<String>, dockerfiles: [String],
                           currentBase: String, workspacePrefix: String) -> [Image] {
        let newestWorkspace = images.compactMap { image -> (Int, String)? in
            guard image.reference.hasPrefix(workspacePrefix),
                  let version = Int(image.reference.dropFirst(workspacePrefix.count)) else { return nil }
            return (version, image.reference)
        }.max { $0.0 < $1.0 }?.1
        var keep = protected.union([currentBase])
        if let newestWorkspace { keep.insert(newestWorkspace) }
        if dockerfiles.contains(where: { text in
            text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\\\n", with: " ").split(separator: "\n").contains { line in
                let instruction = line.trimmingCharacters(in: .whitespaces).uppercased()
                let command = instruction.split(whereSeparator: \.isWhitespace).first
                return (command == "FROM" || (command == "COPY" && instruction.contains("--FROM=")) ||
                    (instruction.contains("--MOUNT=") && instruction.contains("FROM=")) ||
                    instruction.contains("ESCAPE=`")) && text.contains("$") &&
                    (line.contains("$") || instruction.contains("ESCAPE=`"))
            }
        }) { return [] }
        for image in images where dockerfiles.contains(where: { text in
            text.contains(image.reference) || image.digests.contains(where: { text.contains($0) })
        }) {
            keep.insert(image.reference)
        }
        for image in images where keep.contains(image.reference) || !keep.isDisjoint(with: image.digests) {
            keep.formUnion(image.digests)
        }
        return images.filter { image in
            guard !keep.contains(image.reference), keep.isDisjoint(with: image.digests) else { return false }
            if image.isOwnedDangling { return true }
            if image.reference.hasPrefix(workspacePrefix) {
                return Int(image.reference.dropFirst(workspacePrefix.count)) != nil
            }
            if image.reference.hasPrefix("outershell/container-base:") {
                guard let version = Int(image.reference.dropFirst("outershell/container-base:".count)),
                      let current = currentBase.split(separator: ":").last.flatMap({ Int($0) }) else { return false }
                return version < current
            }
            let prefix = "outershell/safe-space-"
            guard image.reference.hasPrefix(prefix), image.reference.hasSuffix(":recipe") else { return false }
            return UUID(uuidString: String(image.reference.dropFirst(prefix.count).dropLast(":recipe".count))) != nil
        }.sorted { $0.reference < $1.reference }
    }

    enum CleanupError: LocalizedError {
        case referencesChanged
        var errorDescription: String? {
            "Image references changed. Refresh storage before cleaning up."
        }
    }

    static func deleteSelected(_ references: [String], candidates: () throws -> [Image],
                               delete: (String) throws -> Void) throws -> (deleted: [String], failures: [String]) {
        guard Set(references).isSubset(of: Set(try candidates().map(\.reference))) else {
            throw CleanupError.referencesChanged
        }
        var deleted: [String] = []
        var failures: [String] = []
        for reference in Set(references).sorted() {
            do {
                guard try candidates().contains(where: { $0.reference == reference }) else {
                    failures.append("\(reference): now in use")
                    continue
                }
                try delete(reference)
                deleted.append(reference)
            } catch {
                failures.append("\(reference): \(error.localizedDescription)")
            }
        }
        return (deleted, failures)
    }

    static func orphanSnapshotBytes(root: URL) throws -> Int64 {
        let state = try Data(contentsOf: root.appendingPathComponent("state.json"))
        guard let images = try JSONSerialization.jsonObject(with: state) as? [String: [String: Any]] else {
            throw CocoaError(.fileReadCorruptFile)
        }
        var retained = Set<String>()
        for descriptor in images.values {
            guard let digest = descriptor["digest"] as? String else { throw CocoaError(.fileReadCorruptFile) }
            let name = digest.replacingOccurrences(of: "sha256:", with: "")
            guard name.count == 64, name.allSatisfy({ $0.isHexDigit }) else { throw CocoaError(.fileReadCorruptFile) }
            let data = try Data(contentsOf: root.appendingPathComponent("content/blobs/sha256/\(name)"))
            guard let index = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw CocoaError(.fileReadCorruptFile)
            }
            let manifests = index["manifests"] as? [[String: Any]] ?? [descriptor]
            for manifest in manifests {
                guard let digest = manifest["digest"] as? String else { throw CocoaError(.fileReadCorruptFile) }
                retained.insert(digest.replacingOccurrences(of: "sha256:", with: ""))
            }
        }
        let directories = try FileManager.default.contentsOfDirectory(
            at: root.appendingPathComponent("snapshots"), includingPropertiesForKeys: nil)
        return try directories.reduce(0) { total, directory in
            guard directory.lastPathComponent.count == 64, !retained.contains(directory.lastPathComponent) else { return total }
            let file = directory.appendingPathComponent("snapshot")
            guard FileManager.default.fileExists(atPath: file.path) else { return total }
            let values = try file.resourceValues(forKeys: [.totalFileAllocatedSizeKey, .fileAllocatedSizeKey])
            return total + Int64(values.totalFileAllocatedSize ?? values.fileAllocatedSize ?? 0)
        }
    }

    static func allocatedSnapshotBytes(for image: Image, root: URL) throws -> Int64 {
        try image.snapshotDigests.reduce(0) { total, digest in
            let name = digest.replacingOccurrences(of: "sha256:", with: "")
            guard name.count == 64, name.allSatisfy({ $0.isHexDigit }) else {
                throw CocoaError(.fileReadCorruptFile)
            }
            let file = root.appendingPathComponent("snapshots/\(name)/snapshot")
            guard FileManager.default.fileExists(atPath: file.path) else { return total }
            let values = try file.resourceValues(forKeys: [.totalFileAllocatedSizeKey, .fileAllocatedSizeKey])
            return total + Int64(values.totalFileAllocatedSize ?? values.fileAllocatedSize ?? 0)
        }
    }
}
