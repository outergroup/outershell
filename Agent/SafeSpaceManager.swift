import Darwin
import Foundation
import AppKit
import CoreFoundation
import CryptoKit

@_silgen_name("OuterShellRegistryCopyResource")
private func outerShellRegistryCopyResource(
    _ key: UnsafePointer<CChar>,
    _ data: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>,
    _ length: UnsafeMutablePointer<Int>,
    _ error: UnsafeMutablePointer<CChar>,
    _ errorSize: Int
) -> Int32

@_silgen_name("OuterShellRegistrySetResource")
private func outerShellRegistrySetResource(
    _ key: UnsafePointer<CChar>,
    _ data: UnsafePointer<UInt8>?,
    _ length: Int,
    _ error: UnsafeMutablePointer<CChar>,
    _ errorSize: Int
) -> Int32

@_silgen_name("OuterShellRegistryRemoveResource")
private func outerShellRegistryRemoveResource(
    _ key: UnsafePointer<CChar>,
    _ error: UnsafeMutablePointer<CChar>,
    _ errorSize: Int
) -> Int32

@_silgen_name("OuterShellRegistryFreeResource")
private func outerShellRegistryFreeResource(_ data: UnsafeMutablePointer<UInt8>?)

private struct OuterShellRegistryContainerRecord {
    var identifier: UnsafeMutablePointer<CChar>?
    var displayName: UnsafeMutablePointer<CChar>?
    var providerID: UnsafeMutablePointer<CChar>?
    var runtimeName: UnsafeMutablePointer<CChar>?
    var buildContextPath: UnsafeMutablePointer<CChar>?
    var createdAtMilliseconds: UInt64
    var cpus: UInt32
    var memoryInMB: UInt32
    var flags: UInt32
}

@_silgen_name("OuterShellRegistryCopyContainers")
private func outerShellRegistryCopyContainers(
    _ records: UnsafeMutablePointer<UnsafeMutablePointer<OuterShellRegistryContainerRecord>?>,
    _ count: UnsafeMutablePointer<Int>,
    _ error: UnsafeMutablePointer<CChar>,
    _ errorSize: Int
) -> Int32

@_silgen_name("OuterShellRegistryUpsertContainer")
private func outerShellRegistryUpsertContainer(
    _ record: UnsafePointer<OuterShellRegistryContainerRecord>,
    _ error: UnsafeMutablePointer<CChar>,
    _ errorSize: Int
) -> Int32

@_silgen_name("OuterShellRegistryRemoveContainer")
private func outerShellRegistryRemoveContainer(
    _ identifier: UnsafePointer<CChar>,
    _ error: UnsafeMutablePointer<CChar>,
    _ errorSize: Int
) -> Int32

@_silgen_name("OuterShellRegistryFreeContainers")
private func outerShellRegistryFreeContainers(
    _ records: UnsafeMutablePointer<OuterShellRegistryContainerRecord>?,
    _ count: Int
)

@_silgen_name("OuterShellRegistryContainerRecordSize")
private func outerShellRegistryContainerRecordSize() -> Int

private let outerShellRegistryContainerOwnsRuntimeFlag: UInt32 = 0x01
private let outerShellRegistryContainerOwnsBuildContextFlag: UInt32 = 0x04
private let outerShellRegistryContainerSelfModifyingFlag: UInt32 = 0x08

private let currentSafeSpaceConfigurationVersion = 14
private let defaultContainerBaseImage = "debian:bookworm"
private let containerSupportImage = "outershell/support"
private let containerSupportImageReferences: Set<String> = [
    "outershell/support", "outershell/support:latest", "docker.io/outershell/support:latest"
]
private let containerSupportLine = "COPY --from=outershell/support /outershell-rootfs/ /"
private let legacyContainerBaseImagePrefix = "outershell/container-base:"
private let legacyBootstrapCopyLine = "COPY --chmod=0755 .outershell/bootstrap/ /tmp/outershell-bootstrap/"
private let legacyBootstrapRunLine = "RUN /tmp/outershell-bootstrap/install.sh && rm -rf /tmp/outershell-bootstrap"
private let containerInitPath = "/usr/local/bin/outershell-container-init"
private let containerRuntimeArguments = [
    "--entrypoint", containerInitPath,
    "--user", "0",
    "--env", "XDG_RUNTIME_DIR=/run/user/0",
    "--env", "OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api"
]
private let containerTransferMagic = Data([0x4f, 0x53, 0x43, 0x54])
private let containerTransferVersion: UInt16 = 1
private let containerTransferRead: UInt16 = 1
private let containerTransferAppend: UInt16 = 2
private let containerTransferRequestHeaderSize = 48
private let containerTransferResponseHeaderSize = 40
private let containerTransferMaximumChunkSize = 16_000_000
private let containerRequestMessageType: UInt16 = 35
private let containerResponseMessageType: UInt16 = 109
private let containerRequestOverviewFlag: UInt32 = 0x01
private let containerOperations = [
    "list", "checkRuntimes", "containerStorage", "cleanContainerStorage", "cleanContainerBuildCache",
    "create", "duplicate", "changeRuntime", "rename", "start", "stop", "restart", "delete",
    "mountFolder", "unmountFolder", "chooseFolder", "updateDockerfile", "updateContainerConfiguration",
    "updateResources", "rebuild", "startApp", "stopApp", "restartApp", "appLogs", "publishSocket",
    "prepareShare", "beginImport", "cancelImport", "finishImport",
]

private struct SafeSpaceRecord: Codable {
    var id: UUID
    var name: String
    var createdAt: Date
    var cpus: Int
    var memoryInMB: Int
    var runtimeProviderID: String?
    var runtimeName: String? = nil
    var ownsContainer: Bool? = nil
    var buildContextPath: String? = nil
    var ownsBuildContext: Bool? = nil
    var selfModifying: Bool? = nil
}

private enum SafeSpaceRuntimeProviderID: String {
    case appleContainer = "apple.container"
    case docker

    var displayName: String {
        switch self {
        case .appleContainer:
            return "Apple container"
        case .docker:
            return "Docker"
        }
    }

    var runtimeKind: String {
        switch self {
        case .appleContainer:
            return "appleContainer"
        case .docker:
            return "docker"
        }
    }

    var supportsLiveMounts: Bool {
        self == .docker
    }
}

private struct SafeSpaceConfiguration: Codable {
    var version: Int
    var baseImage: String
    var realizedBaseImage: String?
    var hasUntrackedChanges: Bool?
    var mounts: [SafeSpaceMount]?
    var realizedDockerfileContents: String?
    var environment: [SafeSpaceEnvironmentVariable]?
    var realizedMounts: [SafeSpaceMount]?
    var realizedEnvironment: [SafeSpaceEnvironmentVariable]?
    var publishedPorts: [SafeSpacePublishedPort]?
    var realizedPublishedPorts: [SafeSpacePublishedPort]?
    var persistentData: [SafeSpacePersistentData]?
    var realizedCPUs: Int?
    var realizedMemoryInMB: Int?
    var realizedBuildContextPath: String?
    var realizedSelfModifying: Bool?
}

private struct SafeSpaceEnvironmentVariable: Codable, Equatable {
    var name: String
    var value: String
}

private struct SafeSpacePublishedPort: Codable, Equatable {
    var hostPort: Int
    var containerPort: Int
}

private struct SafeSpaceMount: Codable, Equatable {
    var id: UUID
    var name: String
    var hostPath: String
    var guestPath: String
    var isReadOnly: Bool
}

private struct SafeSpacePersistentData: Codable, Equatable {
    var id: UUID
    var guestPath: String
    var isDeclared: Bool
}

private struct SafeSpaceTransferManifest: Codable {
    var version: Int
    var name: String
    var cpus: Int
    var memoryInMB: Int
    var sourceRuntimeProviderID: String
    var includesPersistentData: Bool
    var includedMounts: [SafeSpaceMount]
    var omittedMounts: [SafeSpaceMount]
}

private struct SafeSpaceCachedApp: Codable {
    let frontendID: String
    let serviceID: String
    let displayName: String
    let socketPath: String
    let externalSocketPath: String?
    let url: String
    let iconPath: String
    var iconData: Data?
    var isRunning: Bool?
    let publishedPort: Int?
}

private struct SafeSpaceAppSnapshot {
    let frontendID: String
    let serviceID: String
    let displayName: String
    let socketPath: String
    let externalSocketPath: String
    let url: String
    let iconPath: String
    let isRunning: Bool
    let publishedPort: Int?
}

struct SafeSpaceMenuBarApp: Equatable, Sendable {
    let workspaceID: UUID
    let serviceID: String
    let displayName: String
    let socketPath: String
    let url: String
    let publishedPort: Int?
}

struct SafeSpaceMenuBarContainer: Equatable, Sendable {
    let id: UUID
    let name: String
    let apps: [SafeSpaceMenuBarApp]
}

private struct SafeSpaceCommandSnapshot {
    let id: String
    let displayName: String
    let workingDirectory: String
    let user: String
    let iconPath: String
    let executable: String
    let arguments: [String]
}

private struct SafeSpaceCachedCommandIcon {
    let path: String
    let data: Data?
}

private struct SafeSpaceUIAPIResponse {
    let status: Int
    let body: Data
}

private final class SafeSpaceAppEventMonitor: @unchecked Sendable {
    let token = UUID()
    private let lock = NSLock()
    private var task: Task<Void, Never>?
    private var isCancelled = false

    func setTask(_ task: Task<Void, Never>) {
        let shouldCancel = lock.withSafeSpaceLock { () -> Bool in
            if isCancelled {
                return true
            }
            self.task = task
            return false
        }
        if shouldCancel {
            task.cancel()
        }
    }

    func cancel() {
        let task = lock.withSafeSpaceLock { () -> Task<Void, Never>? in
            isCancelled = true
            return self.task
        }
        task?.cancel()
    }
}

enum SafeSpaceManagerError: Error, LocalizedError {
    case invalidRequest
    case safeSpaceNotFound
    case unsupportedProvider
    case commandFailed(String)

    var errorDescription: String? {
        switch self {
        case .invalidRequest:
            return "The container request is invalid."
        case .safeSpaceNotFound:
            return "The selected container no longer exists."
        case .unsupportedProvider:
            return "The selected container provider is unavailable."
        case .commandFailed(let message):
            return message
        }
    }
}

private struct SafeSpaceEndpointPublicationFailures {
    private struct Failure {
        let firstObserved: TimeInterval
        var reportedMessage: String?
    }

    private var failures: [String: Failure] = [:]

    mutating func record(_ message: String, for key: String,
                         now: TimeInterval = ProcessInfo.processInfo.systemUptime) -> Bool {
        var failure = failures[key] ?? Failure(firstObserved: now)
        let shouldReport = now - failure.firstObserved >= 30 && failure.reportedMessage != message
        if shouldReport { failure.reportedMessage = message }
        failures[key] = failure
        return shouldReport
    }

    mutating func recover(_ key: String) -> Bool {
        failures.removeValue(forKey: key)?.reportedMessage != nil
    }

    mutating func retain(_ keys: Set<String>, prefix: String) {
        failures = failures.filter { !$0.key.hasPrefix(prefix) || keys.contains($0.key) }
    }
}

final class SafeSpaceManager: @unchecked Sendable {
    static let shared = SafeSpaceManager()

    private let lock = NSLock()
    private let containerSystemRecoveryLock = NSLock()
    private let containerBaseImageLock = NSRecursiveLock()
    private let imageCleanupQueue = DispatchQueue(label: "org.outershell.image-cleanup")
    private var imageCleanupWork: DispatchWorkItem?
    private var records: [SafeSpaceRecord] = [] {
        didSet {
            let values = Dictionary(uniqueKeysWithValues: records.map {
                ($0.id, (path: $0.buildContextPath ?? "", selfModifying: recordIsSelfModifying($0)))
            })
            buildContextLock.lock()
            buildContexts = values
            buildContextLock.unlock()
        }
    }
    private let buildContextLock = NSLock()
    private var buildContexts: [UUID: (path: String, selfModifying: Bool)] = [:]
    private var cachedApps: [UUID: [SafeSpaceCachedApp]] = [:]
    private var publishedForwards: [String: SafeSpaceSocketRelayForward] = [:]
    private let configurationLock = NSRecursiveLock()
    private var endpointPublicationFailures = SafeSpaceEndpointPublicationFailures()
    private var socketPublications: [String: Task<String, Error>] = [:]
    private var transientStates: [UUID: String] = [:]
    private var buildProgress: [UUID: SafeSpaceBuildProgress] = [:]
    private var appIconFetchesInProgress: Set<String> = []
    private var appEventMonitors: [UUID: SafeSpaceAppEventMonitor] = [:]
    private var cachedCommandIcons: [UUID: [String: SafeSpaceCachedCommandIcon]] = [:]
    private var cachedTerminalShells: [UUID: String] = [:]

    private struct RuntimeIdentity {
        let user: String
        let homeDirectory: String
        let runtimeDirectory: String
        let outerShellHome: String
        let outerctlPath: String
        let daemonPIDPath: String

        var servicesDirectory: String { "\(outerShellHome)/services" }
        var appsDirectory: String { "\(outerShellHome)/apps" }
    }

    private struct SafeSpaceBuildProgress {
        let generation = UUID()
        var phase: String
        var detail: String
        var instruction: String? = nil
        var currentStep: Int? = nil
        var totalSteps: Int? = nil
        var sourceStartLine: Int? = nil
        var sourceEndLine: Int? = nil
        var log = ""
        var lineRemainder = ""

        var dictionary: [String: Any] {
            var value: [String: Any] = [
                "phase": phase,
                "detail": detail,
                "log": log
            ]
            value["instruction"] = instruction
            value["currentStep"] = currentStep
            value["totalSteps"] = totalSteps
            value["sourceStartLine"] = sourceStartLine
            value["sourceEndLine"] = sourceEndLine
            return value
        }
    }

    private init() {
        precondition(
            outerShellRegistryContainerRecordSize() == MemoryLayout<OuterShellRegistryContainerRecord>.stride,
            "The container registry record layout differs between outershelld and the agent."
        )
        do {
            try migrateLegacyContainerDirectory()
        } catch {
            NSLog("Container directory migration failed: %@", error.localizedDescription)
        }
        do {
            records = try loadRecords()
            cachedApps = try loadCachedApps()
        } catch {
            NSLog("Container catalog load failed: %@", error.localizedDescription)
        }
        for index in records.indices {
            do {
                try migrateLegacyDefinition(&records[index])
            } catch {
                NSLog("Container definition migration failed for %@: %@",
                      records[index].id.uuidString, error.localizedDescription)
            }
        }
        let loadedRecords = records
        imageCleanupQueue.async { [self] in
            migrateLegacyContainerImages(for: loadedRecords)
        }
    }

    private func migrateLegacyContainerDirectory() throws {
        let fileManager = FileManager.default
        let base = try applicationDirectory()
        let legacy = base.appendingPathComponent("Safe Spaces", isDirectory: true)
        let current = base.appendingPathComponent("Containers", isDirectory: true)
        guard (try? fileManager.destinationOfSymbolicLink(atPath: legacy.path)) == nil else {
            return
        }
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: legacy.path, isDirectory: &isDirectory),
              isDirectory.boolValue else {
            return
        }
        try fileManager.createDirectory(at: current,
                                        withIntermediateDirectories: true,
                                        attributes: [.posixPermissions: 0o700])
        for name in try fileManager.contentsOfDirectory(atPath: legacy.path) {
            let source = legacy.appendingPathComponent(name, isDirectory: true)
            let destination = current.appendingPathComponent(name, isDirectory: true)
            if fileManager.fileExists(atPath: destination.path) {
                let destinationDockerfile = destination
                    .appendingPathComponent("BuildContext", isDirectory: true)
                    .appendingPathComponent("Dockerfile")
                guard !fileManager.fileExists(atPath: destinationDockerfile.path) else {
                    NSLog("Keeping legacy container directory %@ because %@ already has a Dockerfile",
                          source.path, destination.path)
                    continue
                }
                try fileManager.removeItem(at: destination)
            }
            try fileManager.moveItem(at: source, to: destination)
        }
        guard try fileManager.contentsOfDirectory(atPath: legacy.path).isEmpty else {
            return
        }
        try fileManager.removeItem(at: legacy)
        try fileManager.createSymbolicLink(atPath: legacy.path,
                                           withDestinationPath: current.lastPathComponent)
    }

    private func legacyBuildContextDirectory(_ id: UUID) throws -> URL {
        try safeSpaceDirectory(id).appendingPathComponent("Recipe", isDirectory: true)
    }

    private func migrateLegacyDefinition(_ record: inout SafeSpaceRecord) throws {
        let fileManager = FileManager.default
        let legacyDirectory = try legacyBuildContextDirectory(record.id)
        let directory = try ownedBuildContextDirectory(record.id)
        var isDirectory: ObjCBool = false
        if (try? fileManager.destinationOfSymbolicLink(atPath: legacyDirectory.path)) == nil,
           fileManager.fileExists(atPath: legacyDirectory.path, isDirectory: &isDirectory),
           isDirectory.boolValue,
           !fileManager.fileExists(atPath: directory.path) {
            try fileManager.moveItem(at: legacyDirectory, to: directory)
            try fileManager.createSymbolicLink(atPath: legacyDirectory.path,
                                               withDestinationPath: directory.lastPathComponent)
        }
        let legacyKey = "containers/\(record.id.uuidString.lowercased())/recipe"
        let key = configurationResourceKey(record.id)
        if let data = try RegistryResourceStore.data(for: legacyKey) {
            if try RegistryResourceStore.data(for: key) == nil {
                try RegistryResourceStore.set(data, for: key)
            }
            try RegistryResourceStore.remove(legacyKey)
        }
    }

    private func migrateLegacyContainerImages(for records: [SafeSpaceRecord]) {
        for record in records where recordOwnsContainer(record) {
            let legacyReference = "outershell/container-\(record.id.uuidString.lowercased()):recipe"
            let reference = containerImageReference(record.id)
            do {
                containerBaseImageLock.lock()
                defer { containerBaseImageLock.unlock() }
                guard try runRuntime(record, ["image", "inspect", legacyReference]).status == 0 else {
                    continue
                }
                if try runRuntime(record, ["image", "inspect", reference]).status != 0 {
                    try requireSuccess(
                        try runRuntime(record, ["image", "tag", legacyReference, reference]),
                        action: "rename the container image"
                    )
                }
                let removal = runtimeProvider(for: record) == .docker
                    ? ["image", "rm", legacyReference]
                    : ["image", "delete", legacyReference]
                try requireSuccess(try runRuntime(record, removal),
                                   action: "remove the legacy container image name")
            } catch {
                NSLog("Container image migration failed for %@: %@",
                      record.id.uuidString, error.localizedDescription)
            }
        }
    }

    private func environmentDiscoveryResponse() -> Data {
        var rows: [[String]] = []
        var errorMessage = ""
        do {
            try refreshRecords()
            let registered = lock.withSafeSpaceLock { records }
            for record in registered {
                let provider = runtimeProvider(for: record)
                var row = [record.id.uuidString.lowercased(), record.name, provider.rawValue,
                           "", "unavailable", "", "", "", containerName(record.id)]
                do {
                    switch provider {
                    case .docker:
                        let host = ProcessInfo.processInfo.environment["DOCKER_HOST"]
                        if let host, !host.isEmpty,
                           ProcessInfo.processInfo.environment["DOCKER_CONTEXT"]?.isEmpty != false {
                            row[6] = host
                        } else {
                            let context = try runDocker(["context", "inspect", "--format", "{{.Endpoints.docker.Host}}"])
                            try requireSuccess(context, action: "resolve the Docker endpoint")
                            row[6] = context.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
                        }
                        let result = try runDocker(["container", "inspect", "--format",
                                                    "{{.Id}}\t{{.State.Status}}\t{{.Config.Image}}", "--", row[8]])
                        if result.status != 0 && dockerObjectIsMissing(result) { row[4] = "absent" }
                        try requireSuccess(result, action: "inspect the registered container")
                        let fields = result.stdout.trimmingCharacters(in: .newlines).components(separatedBy: "\t")
                        guard fields.count == 3, !fields[0].isEmpty else {
                            throw SafeSpaceManagerError.commandFailed("Docker returned invalid container identity/state.")
                        }
                        row[3] = fields[0]; row[4] = fields[1]; row[5] = fields[2]
                    case .appleContainer:
                        row[4] = try runtimeState(record, recoverRuntime: false)
                        if row[4] != "absent" { row[3] = row[8] }
                    }
                } catch {
                    row[7] = error.localizedDescription
                }
                rows.append(row)
            }
        } catch {
            errorMessage = error.localizedDescription
        }
        func encode(_ rows: [[String]], error: String) -> Data? {
            guard rows.count <= (16 * 1024 * 1024 - 48) / 72 else { return nil }
            let variable = 48 + rows.count * 72
            var result = Data(repeating: 0, count: variable)
            func put(_ value: UInt32, at offset: Int) {
                var littleEndian = value.littleEndian
                withUnsafeBytes(of: &littleEndian) { result.replaceSubrange(offset..<(offset + 4), with: $0) }
            }
            func reference(_ value: String, at offset: Int) -> Bool {
                let bytes = Data(value.utf8)
                guard !bytes.contains(0), result.count + bytes.count <= 16 * 1024 * 1024 else { return false }
                put(UInt32(result.count), at: offset); put(UInt32(bytes.count), at: offset + 4)
                result.append(bytes)
                return true
            }
            put(0x4445534f, at: 0); put(1, at: 4); put(UInt32(rows.count), at: 12)
            put(72, at: 16); put(48, at: 20); put(UInt32(variable), at: 24)
            put(error.isEmpty ? 0 : 1, at: 28)
            guard reference(error, at: 32) else { return nil }
            for (index, row) in rows.enumerated() {
                for (field, value) in row.enumerated() {
                    guard reference(value, at: 48 + index * 72 + field * 8) else { return nil }
                }
            }
            put(UInt32(result.count), at: 8)
            return result
        }
        return encode(rows, error: errorMessage) ?? encode([], error: "Environment response exceeds limits.") ?? Data()
    }

    func handle(_ data: Data) async -> (status: Int, data: Data) {
        if data == Data([0x4f, 0x53, 0x45, 0x44, 1, 0, 0, 0]) {
            return (200, environmentDiscoveryResponse())
        }
        if data.count >= containerTransferMagic.count,
           data.prefix(containerTransferMagic.count) == containerTransferMagic {
            return handleContainerTransfer(data)
        }
        guard data.count >= 24, data.safeSpaceUInt16(at: 0) == containerRequestMessageType else {
            return (200, containerResponseMessage(error: SafeSpaceManagerError.invalidRequest))
        }
        let operationCode = Int(data.safeSpaceUInt16(at: 2))
        let flags = data.safeSpaceUInt32(at: 4)
        do {
            guard operationCode < containerOperations.count else {
                throw SafeSpaceManagerError.commandFailed(
                    "This container operation is not supported on this Mac."
                )
            }
            let operation = containerOperations[operationCode]
            let workspaceData = try data.safeSpaceReferencedData(at: 8)
            let argumentData = try data.safeSpaceReferencedData(at: 16)
            var request: [String: Any] = [:]
            if !argumentData.isEmpty {
                guard let arguments = try OffsetArchive.decodePropertyList(argumentData) as? [String: Any] else {
                    throw SafeSpaceManagerError.invalidRequest
                }
                request = arguments
            }
            request["operation"] = operation
            if !workspaceData.isEmpty {
                request["workspaceID"] = String(decoding: workspaceData, as: UTF8.self)
            }
            let overview = flags & containerRequestOverviewFlag != 0
            if operation == "list", request["configurationOnly"] as? Bool == true {
                try refreshRecords()
                let selected = try record(from: request)
                return (200, containerResponseMessage(
                    snapshot: ["workspaces": [try configurationDictionary(for: selected)]]
                ))
            }
            let extra = try await perform(operation: operation, request: request)
            if operationChangesMenuBarState(operation) {
                notifyOuterShellSafeSpacesChanged()
            }
            if operation == "appLogs", request["includeWorkspaceSnapshot"] as? Bool == false {
                return (200, containerResponseMessage(result: extra))
            }
            return (200, containerResponseMessage(result: extra, snapshot: [
                "providers": providerDictionaries(),
                "workspaces": try await safeSpaceDictionaries(overview: overview)
            ]))
        } catch {
            return (200, containerResponseMessage(error: error))
        }
    }

    private func containerResponseMessage(error: Error? = nil,
                                          result: [String: Any] = [:],
                                          snapshot: [String: Any]? = nil) -> Data {
        var message = Data()
        message.safeSpaceAppend(containerResponseMessageType)
        message.safeSpaceAppend(UInt32(error == nil ? 0 : 1))
        message.append(Data(repeating: 0, count: 24))
        let encodedResult = result.isEmpty
            ? Data()
            : ((try? OffsetArchive.encodePropertyList(propertyListValue(result) ?? [:])) ?? Data())
        let encodedSnapshot = snapshot
            .flatMap { propertyListValue($0) }
            .flatMap { try? OffsetArchive.encodePropertyList($0) } ?? Data()
        message.safeSpaceAppendReference(Data((error?.localizedDescription ?? "").utf8), at: 6)
        message.safeSpaceAppendReference(encodedResult, at: 14)
        message.safeSpaceAppendReference(encodedSnapshot, at: 22)
        return message
    }

    private func propertyListValue(_ value: Any) -> Any? {
        if value is NSNull { return nil }
        if let dictionary = value as? [String: Any] {
            return dictionary.compactMapValues { propertyListValue($0) }
        }
        if let array = value as? [Any] {
            return array.compactMap { propertyListValue($0) }
        }
        if value is String || value is Data || value is Date || value is NSNumber || value is Bool ||
            value is Int || value is Double {
            return value
        }
        if let url = value as? URL {
            return url.path
        }
        return String(describing: value)
    }

    private func handleContainerTransfer(_ data: Data) -> (status: Int, data: Data) {
        do {
            guard data.count >= containerTransferRequestHeaderSize,
                  data.safeSpaceUInt16(at: 4) == containerTransferVersion else {
                throw SafeSpaceManagerError.invalidRequest
            }
            var transferBytes: uuid_t = (0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0)
            _ = withUnsafeMutableBytes(of: &transferBytes) { destination in
                data.copyBytes(to: destination, from: 8..<24)
            }
            let transferID = UUID(uuid: transferBytes)
            let operation = data.safeSpaceUInt16(at: 6)
            let offset = data.safeSpaceUInt64(at: 24)
            let length = data.safeSpaceUInt64(at: 32)
            let dataOffset = Int(data.safeSpaceUInt32(at: 40))
            let dataLength = Int(data.safeSpaceUInt32(at: 44))
            switch operation {
            case containerTransferRead:
                let archive = try transferDirectory()
                    .appendingPathComponent(transferID.uuidString.lowercased(), isDirectory: true)
                    .appendingPathComponent("container.outershell-container")
                let size = UInt64(try archive.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0)
                guard offset <= size else {
                    throw SafeSpaceManagerError.invalidRequest
                }
                let readLength = min(
                    Int(min(length, UInt64(containerTransferMaximumChunkSize))),
                    Int(size - offset)
                )
                let handle = try FileHandle(forReadingFrom: archive)
                defer { try? handle.close() }
                try handle.seek(toOffset: offset)
                let payload = try handle.read(upToCount: readLength) ?? Data()
                let nextOffset = offset + UInt64(payload.count)
                if nextOffset >= size {
                    try? FileManager.default.removeItem(at: archive.deletingLastPathComponent())
                }
                return (200, containerTransferResponse(nextOffset: nextOffset,
                                                       totalLength: size,
                                                       data: payload))
            case containerTransferAppend:
                guard dataLength <= containerTransferMaximumChunkSize,
                      dataOffset >= containerTransferRequestHeaderSize,
                      dataOffset <= data.count,
                      dataLength <= data.count - dataOffset,
                      UInt64(dataLength) == length else {
                    throw SafeSpaceManagerError.invalidRequest
                }
                let archive = try transferDirectory()
                    .appendingPathComponent(transferID.uuidString.lowercased(), isDirectory: true)
                    .appendingPathComponent("upload.outershell-container")
                let handle = try FileHandle(forWritingTo: archive)
                defer { try? handle.close() }
                let currentOffset = try handle.seekToEnd()
                let payload = data.subdata(in: dataOffset..<(dataOffset + dataLength))
                let nextOffset = offset + UInt64(dataLength)
                if currentOffset == nextOffset {
                    let reader = try FileHandle(forReadingFrom: archive)
                    defer { try? reader.close() }
                    try reader.seek(toOffset: offset)
                    let existing = try reader.read(upToCount: dataLength) ?? Data()
                    guard existing == payload else {
                        throw SafeSpaceManagerError.commandFailed(
                            "The repeated container transfer chunk does not match."
                        )
                    }
                    return (200, containerTransferResponse(nextOffset: nextOffset,
                                                           totalLength: nextOffset))
                }
                guard currentOffset == offset else {
                    throw SafeSpaceManagerError.commandFailed(
                        "The shared container transfer resumed at an unexpected offset."
                    )
                }
                try handle.write(contentsOf: payload)
                return (200, containerTransferResponse(nextOffset: nextOffset,
                                                       totalLength: nextOffset))
            default:
                throw SafeSpaceManagerError.invalidRequest
            }
        } catch {
            return (200, containerTransferResponse(error: error.localizedDescription))
        }
    }

    private func containerTransferResponse(nextOffset: UInt64 = 0,
                                           totalLength: UInt64 = 0,
                                           data: Data = Data(),
                                           error: String? = nil) -> Data {
        var result = Data()
        result.append(containerTransferMagic)
        result.safeSpaceAppend(containerTransferVersion)
        result.safeSpaceAppend(UInt16(error == nil ? 0 : 1))
        result.safeSpaceAppend(nextOffset)
        result.safeSpaceAppend(totalLength)
        result.safeSpaceAppend(UInt32(0))
        result.safeSpaceAppend(UInt32(0))
        result.safeSpaceAppend(UInt32(0))
        result.safeSpaceAppend(UInt32(0))
        precondition(result.count == containerTransferResponseHeaderSize)
        if !data.isEmpty {
            result.safeSpaceAppendReference(data, at: 24)
        }
        if let error, let errorData = error.data(using: .utf8) {
            result.safeSpaceAppendReference(errorData, at: 32)
        }
        return result
    }

    private func operationChangesMenuBarState(_ operation: String) -> Bool {
        switch operation {
        case "create", "duplicate", "changeRuntime", "rename", "start", "stop", "delete",
             "startApp", "stopApp", "restartApp", "rebuild", "finishImport":
            return true
        default:
            return false
        }
    }

    func menuBarContainers() async -> [SafeSpaceMenuBarContainer] {
        do {
            try refreshRecords()
        } catch {
            NSLog("Could not refresh registered containers for the menu bar: %@",
                  error.localizedDescription)
        }
        let currentRecords = lock.withSafeSpaceLock { records }
            .sorted { $0.createdAt < $1.createdAt }
        var containers: [SafeSpaceMenuBarContainer] = []
        var listedRecordIDs: Set<UUID> = []
        for record in currentRecords {
            guard (try? runtimeState(record)) == "running" else { continue }
            let snapshots: [SafeSpaceAppSnapshot]
            do {
                snapshots = await publishExternalAppEndpoints(try await appSnapshots(for: record), for: record)
                updateCachedApps(snapshots, for: record.id)
                listedRecordIDs.insert(record.id)
            } catch {
                NSLog("Could not list menu bar apps for container %@: %@",
                      record.name,
                      error.localizedDescription)
                snapshots = []
            }
            let apps = snapshots
                .filter(\.isRunning)
                .map {
                    SafeSpaceMenuBarApp(workspaceID: record.id,
                                        serviceID: $0.serviceID,
                                        displayName: $0.displayName,
                                        socketPath: $0.socketPath,
                                        url: $0.url,
                                        publishedPort: $0.publishedPort)
                }
                .sorted {
                    $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
                }
            containers.append(SafeSpaceMenuBarContainer(id: record.id,
                                                        name: record.name,
                                                        apps: apps))
            ensureAppEventMonitor(for: record)
        }
        do {
            try saveCachedApps()
            let workspaces: [[String: Any]] = lock.withSafeSpaceLock {
                currentRecords.map { record in
                    let running = listedRecordIDs.contains(record.id)
                    return ["id": record.id.uuidString,
                            "name": record.name,
                            "apps": (cachedApps[record.id] ?? []).map { appDictionary($0, running: running, containerID: record.id) }]
                }
            }
            try publishEndpointNames(workspaces)
        } catch {
            NSLog("Could not publish container endpoint names: %@", error.localizedDescription)
        }
        return containers
    }

    func menuBarURL(for app: SafeSpaceMenuBarApp) async throws -> URL {
        guard let record = lock.withSafeSpaceLock({
            records.first(where: { $0.id == app.workspaceID })
        }) else {
            throw SafeSpaceManagerError.safeSpaceNotFound
        }
        let path = appPathAndQuery(app.url, socketPath: app.socketPath)
        var components = URLComponents()
        components.scheme = "outerloop"
        components.host = "open-hosted-app"
        components.queryItems = [
            URLQueryItem(name: "server", value: "localhost"),
            URLQueryItem(name: "backend", value: app.serviceID),
            URLQueryItem(name: "appURL", value: path),
            URLQueryItem(name: "name", value: app.displayName)
        ]
        if let publishedPort = app.publishedPort {
            components.queryItems?.append(
                URLQueryItem(name: "port", value: String(publishedPort))
            )
        } else {
            let publishedPath = try await publishSocket(in: record,
                                                        socketPath: app.socketPath)
            components.queryItems?.append(
                URLQueryItem(name: "socketPath", value: publishedPath)
            )
        }
        guard let url = components.url else {
            throw SafeSpaceManagerError.commandFailed(
                "The container app returned an invalid address."
            )
        }
        return url
    }

    private func perform(operation: String,
                         request: [String: Any]) async throws -> [String: Any] {
        switch operation {
        case "list":
            break
        case "containerStorage", "cleanContainerStorage", "cleanContainerBuildCache":
            let provider = request["runtimeProviderID"] as? String ?? "apple.container"
            if provider == "docker" { return try dockerStorage(request: request, operation: operation) }
            guard provider == "apple.container", operation != "cleanContainerBuildCache" else {
                throw SafeSpaceManagerError.unsupportedProvider
            }
            return try containerStorage(request: request, clean: operation == "cleanContainerStorage")
        case "checkRuntimes":
            _ = runtimeReadiness(refresh: true)
        case "create":
            try create(request)
        case "duplicate":
            try requireManagedContainer(try record(from: request))
            try duplicate(request)
        case "changeRuntime":
            try requireManagedContainer(try record(from: request))
            return try beginRuntimeChange(request)
        case "rename":
            try rename(request)
        case "start":
            try await start(try record(from: request))
        case "stop":
            try await stop(try record(from: request))
        case "delete":
            try await delete(try record(from: request))
        case "mountFolder":
            try requireManagedContainer(try record(from: request))
            try await mountFolder(request)
        case "unmountFolder":
            try requireManagedContainer(try record(from: request))
            try await unmountFolder(request)
        case "chooseFolder":
            return ["selectedFolderPath": await chooseFolder() ?? ""]
        case "startApp", "stopApp", "restartApp":
            try await controlApp(request, operation: operation)
        case "appLogs":
            return ["appLog": try await appLog(request)]
        case "updateDockerfile":
            try requireManagedContainer(try record(from: request))
            return try updateDockerfile(request)
        case "updateContainerConfiguration":
            try requireManagedContainer(try record(from: request))
            return try updateContainerConfiguration(request)
        case "updateResources":
            try requireManagedContainer(try record(from: request))
            return try updateResources(request)
        case "rebuild":
            try requireManagedContainer(try record(from: request))
            return try beginRebuild(request)
        case "publishSocket":
            let record = try record(from: request)
            guard let socketPath = request["socketPath"] as? String else {
                throw SafeSpaceManagerError.invalidRequest
            }
            return ["publishedSocketPath": try await publishSocket(in: record,
                                                                   socketPath: socketPath)]
        case "prepareShare":
            try requireManagedContainer(try record(from: request))
            return try prepareShare(request)
        case "beginImport":
            return try beginImport(request)
        case "cancelImport":
            try cancelImport(request)
        case "finishImport":
            return try finishImport(request)
        default:
            throw SafeSpaceManagerError.invalidRequest
        }
        return [:]
    }

    private let runtimeReadinessLock = NSLock()
    private var runtimeReadinessCache: (date: Date, values: [String: ContainerRuntimeReadiness])?

    private func runtimeReadiness(refresh: Bool = false) -> [String: ContainerRuntimeReadiness] {
        runtimeReadinessLock.withSafeSpaceLock {
            if !refresh, let cached = runtimeReadinessCache, Date().timeIntervalSince(cached.date) < 15 {
                return cached.values
            }
            #if arch(arm64)
            let appleSupported = ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 26
            #else
            let appleSupported = false
            #endif
            var values: [String: ContainerRuntimeReadiness] = [:]
            for provider in [SafeSpaceRuntimeProviderID.appleContainer, .docker] {
                values[provider.rawValue] = ContainerRuntime.readiness(
                    apple: provider == .appleContainer,
                    executable: runtimeExecutableURL(for: provider), appleSupported: appleSupported,
                    probe: { try ContainerRuntime.probe($0, arguments: $1) })
            }
            runtimeReadinessCache = (Date(), values)
            return values
        }
    }

    private func resourceLimits(for provider: SafeSpaceRuntimeProviderID) -> ContainerResourceLimits? {
        switch provider {
        case .appleContainer:
            return .host
        case .docker:
            return runtimeReadiness()[provider.rawValue]?.resources
        }
    }

    private func providerDictionaries() -> [[String: Any]] {
        let readiness = runtimeReadiness()
        return [
            [
                "id": "apple.container",
                "name": "Apple container",
                "detail": readiness["apple.container"]?.detail ?? "Runtime check failed.",
                "defaultBaseImage": defaultContainerBaseImage,
                "resources": resourceLimits(for: .appleContainer).map { $0.dictionary as Any } ?? NSNull(),
                "isolationName": "",
                "isAvailable": readiness["apple.container"]?.isAvailable ?? false,
                "status": readiness["apple.container"]?.status ?? "failed",
                "setupURL": readiness["apple.container"]?.setupURL ?? "",
                "capabilities": [
                    "supportsApps": true,
                    "supportsShell": true,
                    "supportsLiveMounts": false,
                    "supportsMounts": true,
                    "supportsDefinitions": true,
                    "supportsStorageCleanup": true,
                    "supportsResourceUpdates": true
                ]
            ],
            [
                "id": "docker",
                "name": "Docker",
                "detail": readiness["docker"]?.detail ?? "Runtime check failed.",
                "defaultBaseImage": defaultContainerBaseImage,
                "resources": resourceLimits(for: .docker).map { $0.dictionary as Any } ?? NSNull(),
                "isolationName": "",
                "isAvailable": readiness["docker"]?.isAvailable ?? false,
                "status": readiness["docker"]?.status ?? "failed",
                "setupURL": readiness["docker"]?.setupURL ?? "",
                "capabilities": [
                    "supportsApps": true,
                    "supportsShell": true,
                    "supportsLiveMounts": true,
                    "supportsMounts": true,
                    "supportsDefinitions": true,
                    "supportsStorageCleanup": true,
                    "supportsResourceUpdates": true
                ]
            ]
        ]
    }

    private func configurationDictionary(for record: SafeSpaceRecord) throws -> [String: Any] {
        guard recordOwnsContainer(record) else { throw SafeSpaceManagerError.invalidRequest }
        let configuration = try configuration(for: record)
        return [
            "id": record.id.uuidString,
            "name": record.name,
            "cpus": record.cpus,
            "memoryInMB": record.memoryInMB,
            "ownsContainer": true,
            "managementKind": "managed",
            "runtime": try runtimeDictionary(for: record),
            "definition": try definitionDictionary(for: record),
            "mounts": try configurationMounts(for: record, configuration: configuration)
        ]
    }

    private func configurationMounts(for record: SafeSpaceRecord,
                                     configuration: SafeSpaceConfiguration) throws -> [[String: Any]] {
        var mounts: [[String: Any]] = recordIsSelfModifying(record) ? [[
            "id": UUID(uuid: (0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 2)).uuidString,
            "name": "Container project",
            "hostPath": try buildContextDirectory(record.id).path,
            "guestPath": "/var/lib/outershell/project",
            "isReadOnly": false,
            "isInfrastructure": true,
            "isBuildContextMount": true
        ]] : []
        mounts.append(contentsOf: (configuration.mounts ?? []).map {
            [
                "id": $0.id.uuidString,
                "name": $0.name,
                "hostPath": $0.hostPath,
                "guestPath": $0.guestPath,
                "isReadOnly": $0.isReadOnly,
                "isInfrastructure": false,
                "isBuildContextMount": false
            ] as [String: Any]
        })
        return mounts
    }

    private func safeSpaceDictionaries(overview: Bool = false) async throws -> [[String: Any]] {
        try refreshRecords()
        let currentRecords = lock.withSafeSpaceLock { records }
        var values: [[String: Any]] = []
        for record in currentRecords.sorted(by: {
            $0.createdAt < $1.createdAt
        }) {
            let transientState = lock.withSafeSpaceLock { transientStates[record.id] }
            let state: String
            var runtimeError: String?
            if let transientState {
                state = transientState
            } else {
                do {
                    state = try runtimeState(record)
                } catch {
                    state = "unavailable"
                    runtimeError = error.localizedDescription
                }
            }
            let outerShellSupport: [String: Any]
            if let runtimeError {
                outerShellSupport = ["status": "unknown", "detail": runtimeError]
            } else {
                outerShellSupport = outerShellSupportDictionary(for: record, state: state)
            }
            let supportsRunningApps = outerShellSupport["status"] as? String == "available"
            var snapshots: [SafeSpaceAppSnapshot]?
            if state == "running" && supportsRunningApps {
                do {
                    snapshots = try await appSnapshots(for: record)
                } catch {
                    NSLog("Could not list apps for container %@: %@",
                          record.name,
                          error.localizedDescription)
                    snapshots = nil
                }
            } else {
                snapshots = nil
            }
            if let discoveredSnapshots = snapshots {
                let publishedSnapshots = await publishExternalAppEndpoints(
                    discoveredSnapshots,
                    for: record
                )
                snapshots = publishedSnapshots
                updateCachedApps(publishedSnapshots, for: record.id)
                scheduleDeclaredAppIconFetches(for: record)
                ensureAppEventMonitor(for: record)
            } else {
                cancelAppEventMonitor(for: record.id)
            }
            let apps = lock.withSafeSpaceLock {
                (cachedApps[record.id] ?? []).map {
                    appDictionary($0, running: state == "running" && supportsRunningApps, containerID: record.id)
                }
            }
            let commands: [[String: Any]]
            if state == "running" && supportsRunningApps {
                do {
                    commands = try commandSnapshots(for: record).map {
                        commandDictionary($0, for: record)
                    }
                } catch {
                    NSLog("Could not list commands for container %@: %@",
                          record.name,
                          error.localizedDescription)
                    commands = []
                }
            } else {
                commands = []
            }
            let ownsContainer = recordOwnsContainer(record)
            var mounts: [[String: Any]] = []
            var persistentData: [[String: Any]] = []
            if ownsContainer && !overview {
                let configuration = try configuration(for: record)
                mounts = try configurationMounts(for: record, configuration: configuration)
                persistentData = try (configuration.persistentData ?? [])
                    .filter(\.isDeclared)
                    .sorted {
                        $0.guestPath.localizedStandardCompare($1.guestPath) == .orderedAscending
                    }
                    .map { item in
                        [
                            "id": item.id.uuidString,
                            "guestPath": item.guestPath,
                            "hostPath": try persistentStorageLocation(record, item: item)
                        ] as [String: Any]
                    }
            }
            var value: [String: Any] = [
                "id": record.id.uuidString,
                "name": record.name,
                "state": state,
                "cpus": record.cpus,
                "memoryInMB": record.memoryInMB,
                "managementKind": ownsContainer ? "managed" : "attached",
                "ownsContainer": ownsContainer,
                "ownsBuildContext": ownsContainer && recordOwnsBuildContext(record),
                "selfModifying": ownsContainer && recordIsSelfModifying(record),
                "buildContextPath": ownsContainer ? ((try? buildContextDirectory(record.id).path) ?? "") : "",
                "runtimeName": registeredRuntimeName(for: record),
                "runtimeKind": runtimeProvider(for: record).runtimeKind,
                "supportsLiveMounts": runtimeProvider(for: record).supportsLiveMounts,
                "runtime": try runtimeDictionary(for: record),
                "capabilities": capabilityDictionary(for: record),
                "outerShellSupport": outerShellSupport,
                "shellCommand": try shellCommand(for: record, state: state),
                "apps": apps,
                "commands": commands,
                "mounts": mounts,
                "persistentData": persistentData
            ]
            if ownsContainer && !overview {
                value["definition"] = try definitionDictionary(for: record)
            }
            if let progress = lock.withSafeSpaceLock({ buildProgress[record.id] }) {
                value["buildProgress"] = progress.dictionary
            }
            if overview {
                for key in ["definition", "mounts", "persistentData"] { value.removeValue(forKey: key) }
                for key in ["apps", "commands"] {
                    value[key] = try (value[key] as? [[String: Any]] ?? []).map { item in
                        var item = item
                        if let data = item.removeValue(forKey: "iconData") as? Data {
                            item["iconURL"] = try overviewIconURL(data)
                        }
                        return item
                    }
                }
            }
            values.append(value)
        }
        try saveCachedApps()
        do { try publishEndpointNames(values) }
        catch { NSLog("Could not publish container endpoint names: %@", error.localizedDescription) }
        return values
    }

    private func layoutEndpointNames() -> [String: String] {
        let root = ProcessInfo.processInfo.environment["OUTERSHELL_HOME"]
            ?? NSHomeDirectory() + "/Library/Application Support/outershell"
        let registry = ProcessInfo.processInfo.environment["OUTERSHELL_REGISTRY"]
            ?? (root as NSString).appendingPathComponent("registry.orwa")
        let path = (registry as NSString).expandingTildeInPath + ".layout"
        guard FileManager.default.fileExists(atPath: path) else { return [:] }
        do {
            let data = try Data(contentsOf: URL(fileURLWithPath: path))
            guard data.count >= 18, data.prefix(8) == Data("OSLAY001".utf8),
                  let layout = try JSONSerialization.jsonObject(with: data.dropFirst(16)) as? [String: Any] else {
                NSLog("Could not read endpoint names from %@: unrecognized layout record", path)
                return [:]
            }
            return (layout["names"] as? [String: Any] ?? [:]).compactMapValues { value in
                guard let name = value as? String, !name.isEmpty else { return nil }
                return name
            }
        } catch {
            NSLog("Could not read endpoint names from %@: %@", path, error.localizedDescription)
            return [:]
        }
    }

    private func containerEndpointLayoutKey(workspaceID: String, app: [String: Any]) -> String? {
        func encode(_ values: [Any]) -> String? {
            guard let data = try? JSONSerialization.data(withJSONObject: values,
                                                         options: [.fragmentsAllowed, .withoutEscapingSlashes]) else { return nil }
            return String(data: data, encoding: .utf8)
        }
        guard let serviceID = app["serviceID"] as? String,
              let frontend = encode([app["frontendID"] as? String ?? "", app["socketPath"] as? String ?? "",
                                     app["url"] as? String ?? "", 0]) else { return nil }
        return encode(["container", workspaceID.lowercased(), serviceID, frontend])
    }

    private func publishEndpointNames(_ workspaces: [[String: Any]]) throws {
        let names = layoutEndpointNames()
        var lines = ["outerssh-endpoints-v2"]
        for workspace in workspaces {
            guard let container = workspace["name"] as? String,
                  let apps = workspace["apps"] as? [[String: Any]] else { continue }
            let workspaceID = workspace["id"] as? String ?? ""
            for app in apps where app["isRunning"] as? Bool == true {
                let socket = app["externalSocketPath"] as? String ?? ""
                let renamed = containerEndpointLayoutKey(workspaceID: workspaceID, app: app).flatMap { names[$0] }
                let name = renamed ?? app["displayName"] as? String ?? app["serviceID"] as? String ?? ""
                let fields = [container, name]
                guard !name.isEmpty, fields.allSatisfy({ $0.utf8.count <= 256 &&
                    !$0.unicodeScalars.contains(where: CharacterSet.controlCharacters.contains) }) else { continue }
                let endpoints: [String]
                if let port = app["publishedPort"] as? Int, port > 0 && port <= 65535 {
                    endpoints = ["localhost", "127.0.0.1", "::1"].map { "tcp \($0) \(port)" }
                } else if socket.hasPrefix("/"), socket.utf8.count < 4090,
                          !socket.unicodeScalars.contains(where: CharacterSet.controlCharacters.contains) {
                    endpoints = ["unix \(socket)"]
                } else { continue }
                for endpoint in endpoints { lines.append(contentsOf: [endpoint] + fields) }
            }
        }
        let data = Data((lines.joined(separator: "\n") + "\n").utf8)
        guard data.count <= 65536 else {
            throw NSError(domain: "OuterShell.EndpointNames", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Endpoint names exceed the publication limit."])
        }
        let directory = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/org.outerssh/endpoints.d", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                               attributes: [.posixPermissions: 0o700])
        let file = directory.appendingPathComponent("outershell-containers")
        if (try? Data(contentsOf: file)) == data { return }
        try data.write(to: file, options: .atomic)
    }

    private func overviewIconURL(_ data: Data) throws -> String {
        let key = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        let root = ProcessInfo.processInfo.environment["OUTERSHELL_HOME"]
            ?? NSHomeDirectory() + "/Library/Application Support/outershell"
        let directory = URL(fileURLWithPath: (root as NSString).expandingTildeInPath)
            .appendingPathComponent("web-icons", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let file = directory.appendingPathComponent(key + ".png")
        if !FileManager.default.fileExists(atPath: file.path) {
            try data.write(to: file, options: .atomic)
        }
        return "/api/icon?key=" + key
    }

    private func runtimeDictionary(for record: SafeSpaceRecord) throws -> [String: Any] {
        let provider = runtimeProvider(for: record)
        let baseImage = recordOwnsContainer(record) ? try configuration(for: record).baseImage : "Linux"
        let operatingSystem = operatingSystemDescription(for: baseImage)
        return [
            "providerID": provider.rawValue,
            "providerName": provider.displayName,
            "isolationKind": "container",
            "isolationName": "",
            "operatingSystemName": operatingSystem.name,
            "operatingSystemVersion": operatingSystem.version,
            "architecture": cpuArchitecture()
        ]
    }

    private func shellCommand(for record: SafeSpaceRecord, state: String) throws -> String {
        let cachedShell = lock.withSafeSpaceLock { cachedTerminalShells[record.id] }
        let shell: String
        if let cachedShell {
            shell = cachedShell
        } else if state == "running" {
            shell = resolvedTerminalShell(for: record)
            lock.withSafeSpaceLock {
                cachedTerminalShells[record.id] = shell
            }
        } else {
            shell = "/bin/sh"
        }
        let executable = runtimeProvider(for: record) == .docker ? "docker" : "container"
        return interactiveRuntimeCommand([
            executable, "exec", "-it",
            "--env", "TERM",
            "--env", "COLORTERM",
            "--env", "COLORFGBG",
            containerName(record.id), shell
        ])
    }

    private func resolvedTerminalShell(for record: SafeSpaceRecord) -> String {
        let script = #"""
        uid=$(id -u 2>/dev/null || /usr/bin/id -u)
        shell=$(awk -F: -v uid="$uid" '$3 == uid { print $7; exit }' /etc/passwd 2>/dev/null || true)
        case "$shell" in
            ""|*/false|*/nologin) ;;
            *) if [ -x "$shell" ]; then printf '%s\n' "$shell"; exit 0; fi ;;
        esac
        for shell in /bin/bash /usr/bin/bash /bin/zsh /usr/bin/zsh /bin/ash /usr/bin/ash /bin/sh /usr/bin/sh; do
            if [ -x "$shell" ]; then printf '%s\n' "$shell"; exit 0; fi
        done
        printf '%s\n' /bin/sh
        """#
        guard let result = try? runRuntime(record, [
            "exec", containerName(record.id), "/bin/sh", "-c", script
        ]), result.status == 0 else {
            return "/bin/sh"
        }
        let candidate = result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        guard candidate.hasPrefix("/"),
              candidate.count <= 512,
              !candidate.contains(where: { $0.isWhitespace }) else {
            return "/bin/sh"
        }
        return candidate
    }

    private func commandSnapshots(for record: SafeSpaceRecord) throws
        -> [SafeSpaceCommandSnapshot] {
        let result = try runRuntime(record, [
            "exec", "--user", "root", containerName(record.id),
            "/usr/local/bin/outerctl", "image", "list-commands"
        ])
        try requireSuccess(result, action: "list container commands")
        return result.stdout.split(separator: "\n").compactMap { line in
            let encodedFields = line.split(separator: "\t", omittingEmptySubsequences: false)
            guard encodedFields.count >= 7,
                  encodedFields[0] == "1" else {
                return nil
            }
            let fields = encodedFields.dropFirst().compactMap {
                String($0).removingPercentEncoding
            }
            guard fields.count == encodedFields.count - 1 else {
                return nil
            }
            return SafeSpaceCommandSnapshot(
                id: fields[0],
                displayName: fields[1],
                workingDirectory: fields[2],
                user: fields[3],
                iconPath: fields[4],
                executable: fields[5],
                arguments: Array(fields.dropFirst(6))
            )
        }
    }

    private func commandDictionary(_ command: SafeSpaceCommandSnapshot,
                                   for record: SafeSpaceRecord) -> [String: Any] {
        let innerCommand = ([command.executable] + command.arguments)
            .map(shellCommandArgument)
            .joined(separator: " ")
        let invocation = "cd \(shellCommandArgument(command.workingDirectory)) && exec \(innerCommand)"
        let runtimeExecutable = runtimeProvider(for: record) == .docker
            ? "docker"
            : "container"
        let hostArguments = [
            runtimeExecutable, "exec", "-it",
            "--env", "TERM",
            "--env", "COLORTERM",
            "--env", "COLORFGBG",
            "--user", command.user,
            containerName(record.id), "/bin/sh", "-lc", invocation
        ]
        let containerCommand = interactiveRuntimeCommand(hostArguments)
        var value: [String: Any] = [
            "id": command.id,
            "displayName": command.displayName,
            "shellCommand": containerCommand,
            "containerCommand": containerCommand,
            "internalCommand": invocation,
            "iconPath": command.iconPath
        ]
        if let iconData = commandIconData(command, for: record) {
            value["iconData"] = iconData
        }
        return value
    }

    private func interactiveRuntimeCommand(_ arguments: [String]) -> String {
        let environment = "TERM=${TERM:-xterm-256color} "
            + "COLORTERM=${COLORTERM:-truecolor} "
            + "COLORFGBG=${COLORFGBG:-}"
        return environment + " "
            + arguments.map(shellCommandArgument).joined(separator: " ")
    }

    private func commandIconData(_ command: SafeSpaceCommandSnapshot,
                                 for record: SafeSpaceRecord) -> Data? {
        guard command.iconPath.hasPrefix("/") else { return nil }
        if let cached = lock.withSafeSpaceLock({
            cachedCommandIcons[record.id]?[command.id]
        }), cached.path == command.iconPath {
            return cached.data
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
            "outershell-command-icon-\(UUID().uuidString)",
            isDirectory: true
        )
        do {
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            defer { try? FileManager.default.removeItem(at: directory) }
            let destination = directory.appendingPathComponent("icon")
            let source = "\(containerName(record.id)):\(command.iconPath)"
            let result = try copyFromRuntime(record,
                                             source: source,
                                             destination: destination.path)
            let data: Data?
            if result.status == 0,
               let attributes = try? FileManager.default.attributesOfItem(
                atPath: destination.path
               ), let size = attributes[.size] as? NSNumber,
               size.intValue > 0,
               size.intValue <= 512 * 1024,
               let candidate = try? Data(contentsOf: destination),
               let image = NSBitmapImageRep(data: candidate),
               image.pixelsWide > 0,
               image.pixelsHigh > 0,
               image.pixelsWide <= 4096,
               image.pixelsHigh <= 4096 {
                data = candidate
            } else {
                data = nil
            }
            lock.withSafeSpaceLock {
                cachedCommandIcons[record.id, default: [:]][command.id] =
                    SafeSpaceCachedCommandIcon(path: command.iconPath, data: data)
            }
            return data
        } catch {
            NSLog("Could not read command icon %@ from container %@: %@",
                  command.iconPath,
                  record.name,
                  error.localizedDescription)
            lock.withSafeSpaceLock {
                cachedCommandIcons[record.id, default: [:]][command.id] =
                    SafeSpaceCachedCommandIcon(path: command.iconPath, data: nil)
            }
            return nil
        }
    }

    private func operatingSystemDescription(
        for baseImage: String
    ) -> (name: String, version: String) {
        let lowercased = baseImage.lowercased()
        let tag = baseImage.split(separator: "/").last.flatMap { component in
            component.split(separator: ":", maxSplits: 1).dropFirst().first.map(String.init)
        } ?? ""
        if lowercased.contains("debian") {
            let version = tag == "bookworm" || tag.isEmpty ? "12 (bookworm)" : tag
            return ("Debian GNU/Linux", version)
        }
        if lowercased.contains("ubuntu") {
            return ("Ubuntu", tag)
        }
        if lowercased.contains("alpine") {
            return ("Alpine Linux", tag)
        }
        if lowercased.contains("fedora") {
            return ("Fedora Linux", tag)
        }
        return (baseImage, "")
    }

    private func capabilityDictionary(for record: SafeSpaceRecord) -> [String: Any] {
        let provider = runtimeProvider(for: record)
        let ownsContainer = recordOwnsContainer(record)
        return [
            "supportsApps": true,
            "supportsShell": true,
            "supportsLiveMounts": ownsContainer && provider.supportsLiveMounts,
            "supportsMounts": ownsContainer,
            "supportsDefinitions": ownsContainer,
            "canChangeRuntime": ownsContainer,
            "canShare": ownsContainer,
            "canDeleteRuntime": ownsContainer
        ]
    }

    private func outerShellSupportDictionary(
        for record: SafeSpaceRecord,
        state: String
    ) -> [String: Any] {
        guard state == "running" else {
            return [
                "status": "unknown",
                "detail": "Outer Shell support will be verified when the container starts."
            ]
        }
        guard let result = try? runRuntime(record, [
            "exec", "--user", "root", containerName(record.id),
            "/bin/sh", "-c",
            "if [ ! -x /usr/local/bin/outerctl ] || [ ! -x /usr/local/bin/outershelld ]; then printf missing; elif [ -S /run/user/0/outershelld-api ]; then printf available; else printf inactive; fi"
        ]), result.status == 0 else {
            return [
                "status": "unknown",
                "detail": "Outer Shell could not verify support in the running container."
            ]
        }
        switch result.stdout.trimmingCharacters(in: .whitespacesAndNewlines) {
        case "available":
            return [
                "status": "available",
                "detail": "Outer Shell support is active."
            ]
        case "inactive":
            return [
                "status": "inactive",
                "detail": "Outer Shell support is installed, but its container service is not running."
            ]
        default:
            return [
                "status": "missing",
                "detail": "Outer Shell support was not detected. The container can run, but its apps cannot be managed here."
            ]
        }
    }

    private func create(_ request: [String: Any]) throws {
        guard let rawName = request["name"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let providerID = request["runtimeProviderID"] as? String ?? "apple.container"
        guard let provider = SafeSpaceRuntimeProviderID(rawValue: providerID),
              runtimeExecutableURL(for: provider) != nil else {
            throw SafeSpaceManagerError.unsupportedProvider
        }
        if let readiness = runtimeReadiness(refresh: true)[provider.rawValue],
           !readiness.isAvailable && !(provider == .appleContainer && readiness.status == "stopped") {
            throw SafeSpaceManagerError.commandFailed(readiness.detail)
        }
        let requestedBaseImage = try (request["baseImage"] as? String).map {
            try validatedBaseImage($0)
        }
        let requestedDockerfile = request["dockerfile"] as? String
        if let requestedDockerfile { try validateDockerfile(requestedDockerfile) }
        let limits = resourceLimits(for: provider) ?? .host
        let resources: (cpus: Int, memoryInMB: Int)
        switch limits.validated(cpus: request["cpus"], memoryInMB: request["memoryInMB"]) {
        case .success(let value):
            resources = value
        case .failure(let error):
            throw SafeSpaceManagerError.commandFailed(error.message)
        }
        let id = UUID()
        let record = SafeSpaceRecord(id: id,
                                     name: name,
                                     createdAt: Date(),
                                     cpus: resources.cpus,
                                     memoryInMB: resources.memoryInMB,
                                     runtimeProviderID: provider.rawValue,
                                     runtimeName: managedContainerName(id),
                                     ownsContainer: true,
                                     ownsBuildContext: true,
                                     selfModifying: true)
        try createManagedDirectories(for: record)
        if let requestedBaseImage {
            try saveConfiguration(
                newConfiguration(baseImage: requestedBaseImage,
                          hasUntrackedChanges: false),
                for: record
            )
        } else {
            _ = try configuration(for: record)
        }
        if let requestedDockerfile {
            try requestedDockerfile.write(to: dockerfileURL(record.id), atomically: true, encoding: .utf8)
        }
        lock.withSafeSpaceLock {
            records.append(record)
            transientStates[record.id] = "creating"
        }
        try saveRecords()
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                let imageReference = self.containerImageReference(record.id)
                _ = try self.runContainerBuild(record,
                                               imageReference: imageReference)
                try self.createRuntimeContainer(record, imageReference: imageReference)
                try self.markRealized(for: record)
                self.setTransientState(nil, for: record.id)
            } catch {
                NSLog("Container creation failed: %@", error.localizedDescription)
                self.setTransientState("error", for: record.id)
            }
        }
    }

    private func duplicate(_ request: [String: Any]) throws {
        let source = try record(from: request)
        guard let providerText = request["runtimeProviderID"] as? String,
              let provider = SafeSpaceRuntimeProviderID(rawValue: providerText),
              runtimeExecutableURL(for: provider) != nil else {
            throw SafeSpaceManagerError.unsupportedProvider
        }
        let requestedName = (request["name"] as? String)?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let name = requestedName.flatMap { $0.isEmpty ? nil : $0 }
            ?? "\(source.name) (\(provider.displayName))"
        let duplicateID = UUID()
        let duplicateResources = (resourceLimits(for: provider) ?? .host)
            .runnable(cpus: source.cpus, memoryInMB: source.memoryInMB)
        let duplicate = SafeSpaceRecord(
            id: duplicateID,
            name: name,
            createdAt: Date(),
            cpus: duplicateResources.cpus,
            memoryInMB: duplicateResources.memoryInMB,
            runtimeProviderID: provider.rawValue,
            runtimeName: managedContainerName(duplicateID),
            ownsContainer: true,
            ownsBuildContext: true,
            selfModifying: true
        )
        try createManagedDirectories(for: duplicate)
        let sourceBuildContext = try buildContextDirectory(source.id)
        let destinationBuildContext = try buildContextDirectory(duplicate.id)
        if FileManager.default.fileExists(atPath: destinationBuildContext.path) {
            try FileManager.default.removeItem(at: destinationBuildContext)
        }
        try FileManager.default.copyItem(at: sourceBuildContext, to: destinationBuildContext)
        var duplicatedConfiguration = try configuration(for: source)
        duplicatedConfiguration.realizedBaseImage = nil
        duplicatedConfiguration.realizedDockerfileContents = nil
        duplicatedConfiguration.realizedMounts = nil
        duplicatedConfiguration.realizedEnvironment = nil
        duplicatedConfiguration.realizedPublishedPorts = nil
        duplicatedConfiguration.realizedCPUs = nil
        duplicatedConfiguration.realizedMemoryInMB = nil
        duplicatedConfiguration.hasUntrackedChanges = false
        try saveConfiguration(duplicatedConfiguration, for: duplicate)
        lock.withSafeSpaceLock {
            records.append(duplicate)
            transientStates[duplicate.id] = "creating"
        }
        try saveRecords()
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                let sourceWasRunning = try self.runtimeState(source) == "running"
                if sourceWasRunning {
                    try self.requireSuccess(
                        try self.runRuntime(source, [
                            "stop", "--time", "15", self.containerName(source.id)
                        ]),
                        action: "pause the source container"
                    )
                }
                defer {
                    if sourceWasRunning {
                        _ = try? self.runRuntime(
                            source,
                            ["start", self.containerName(source.id)]
                        )
                    }
                }
                for item in duplicatedConfiguration.persistentData ?? [] where item.isDeclared {
                    let archive = try self.volumeArchiveURL(duplicate, item: item)
                    try self.exportPersistentVolume(source, item: item, archive: archive)
                }
                let imageReference = self.containerImageReference(duplicate.id)
                _ = try self.runContainerBuild(duplicate, imageReference: imageReference)
                try self.createRuntimeContainer(duplicate, imageReference: imageReference)
                try self.markRealized(for: duplicate)
                self.setTransientState(nil, for: duplicate.id)
            } catch {
                NSLog("Container duplication failed: %@", error.localizedDescription)
                self.setTransientState("error", for: duplicate.id)
            }
        }
    }

    private func prepareShare(_ request: [String: Any]) throws -> [String: Any] {
        let record = try record(from: request)
        let includePersistentData = request["includePersistentData"] as? Bool ?? false
        let includeMountedFolders = request["includeMountedFolders"] as? Bool ?? false
        let transferID = UUID()
        let transferRoot = try transferDirectory().appendingPathComponent(
            transferID.uuidString.lowercased(), isDirectory: true
        )
        try FileManager.default.createDirectory(at: transferRoot,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        var keepTransfer = false
        defer {
            if !keepTransfer {
                try? FileManager.default.removeItem(at: transferRoot)
            }
        }
        let configuration = try configuration(for: record)
        let persistentData = (configuration.persistentData ?? []).filter(\.isDeclared)
        let sharedBuildContext = transferRoot.appendingPathComponent(
            "BuildContext", isDirectory: true
        )
        try FileManager.default.copyItem(
            at: try buildContextDirectory(record.id),
            to: sharedBuildContext
        )
        try requireUnlinkedTree(sharedBuildContext)
        for name in ["configuration.orwv"] {
            let staleConfiguration = sharedBuildContext.appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: staleConfiguration.path) {
                try FileManager.default.removeItem(at: staleConfiguration)
            }
        }
        var sharedConfiguration = configuration
        sharedConfiguration.persistentData = persistentData
        try OffsetArchive.encode(sharedConfiguration).write(
            to: sharedBuildContext.appendingPathComponent("configuration.orwv"),
            options: .atomic
        )
        var archiveSources: [(source: URL, destination: String)] = [
            (sharedBuildContext, "BuildContext")
        ]
        if includePersistentData {
            for item in persistentData {
                let archive = transferRoot.appendingPathComponent(item.id.uuidString.lowercased() + ".tar")
                do {
                    try exportPersistentVolume(record, item: item, archive: archive)
                } catch {
                    if runtimeProvider(for: record) == .appleContainer,
                       (try? runtimeState(record)) == "running" {
                        throw SafeSpaceManagerError.commandFailed(
                            "Could not export persistent data from the running Apple container. Stop the container and share it again.\n" + error.localizedDescription
                        )
                    }
                    throw error
                }
                archiveSources.append((archive, "Volume Archives/\(archive.lastPathComponent)"))
            }
        }
        let mounts = configuration.mounts ?? []
        var includedMounts: [SafeSpaceMount] = []
        if includeMountedFolders {
            for mount in mounts {
                let source = URL(fileURLWithPath: mount.hostPath, isDirectory: true)
                guard FileManager.default.fileExists(atPath: source.path) else { continue }
                archiveSources.append(
                    (source, "Mounted Folders/\(mount.id.uuidString.lowercased())")
                )
                includedMounts.append(mount)
            }
        }
        let manifest = SafeSpaceTransferManifest(
            version: 3,
            name: record.name,
            cpus: record.cpus,
            memoryInMB: record.memoryInMB,
            sourceRuntimeProviderID: runtimeProvider(for: record).rawValue,
            includesPersistentData: includePersistentData,
            includedMounts: includedMounts,
            omittedMounts: mounts.filter { mount in
                !includedMounts.contains(where: { $0.id == mount.id })
            }
        )
        let manifestURL = transferRoot.appendingPathComponent("manifest.orwv")
        try OffsetArchive.encode(manifest).write(to: manifestURL, options: .atomic)
        archiveSources.append((manifestURL, "manifest.orwv"))
        let stagedArchive = validatedShareStagingURL(request["stagingPath"] as? String)
        let archive = stagedArchive
            ?? transferRoot.appendingPathComponent("container.outershell-container")
        let archiveRoot = transferRoot.appendingPathComponent("Archive Sources", isDirectory: true)
        for item in archiveSources {
            let source = item.source.resolvingSymlinksInPath().standardizedFileURL
            guard source.path != "/" else {
                throw SafeSpaceManagerError.commandFailed(
                    "The root of the server cannot be included in a shared container."
                )
            }
            let entry = archiveRoot.appendingPathComponent(item.destination)
            try FileManager.default.createDirectory(at: entry.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try FileManager.default.createSymbolicLink(at: entry, withDestinationURL: source)
        }
        let arguments = [
            "-czf", archive.path, "-H", "--no-mac-metadata",
            "--options", "gzip:compression-level=1", "-C", archiveRoot.path
        ] + archiveSources.map { $0.destination }
        let result = try runCommand(
            executable: "/usr/bin/tar",
            arguments: arguments
        )
        try? FileManager.default.removeItem(at: manifestURL)
        try requireSuccess(result, action: "prepare the shared container")
        let size = try archive.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        keepTransfer = stagedArchive == nil
        return [
            "transferID": transferID.uuidString.lowercased(),
            "fileName": safeTransferFileName(record.name),
            "byteCount": size,
            "stagedDirectly": stagedArchive != nil,
            "includedMountCount": includedMounts.count,
            "omittedMountCount": manifest.omittedMounts.count
        ]
    }

    private func validatedShareStagingURL(_ path: String?) -> URL? {
        guard let path, !path.isEmpty else { return nil }
        let destination = URL(fileURLWithPath: path).standardizedFileURL
        guard destination.pathExtension == "outershell-container" else { return nil }
        let parent = destination.deletingLastPathComponent().resolvingSymlinksInPath()
        let temporaryRoot = FileManager.default.temporaryDirectory
            .resolvingSymlinksInPath()
            .standardizedFileURL
        let parentPath = parent.standardizedFileURL.path
        let temporaryPath = temporaryRoot.path.hasSuffix("/")
            ? temporaryRoot.path
            : temporaryRoot.path + "/"
        guard parentPath.hasPrefix(temporaryPath),
              FileManager.default.fileExists(atPath: parentPath) else {
            return nil
        }
        return destination
    }

    private func beginImport(_ request: [String: Any]) throws -> [String: Any] {
        _ = request
        let transferID = UUID()
        let root = try transferDirectory().appendingPathComponent(
            transferID.uuidString.lowercased(), isDirectory: true
        )
        try FileManager.default.createDirectory(at: root,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        try Data().write(to: root.appendingPathComponent("upload.outershell-container"))
        return ["transferID": transferID.uuidString.lowercased()]
    }

    private func cancelImport(_ request: [String: Any]) throws {
        guard let transferText = request["transferID"] as? String,
              let transferID = UUID(uuidString: transferText) else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let root = try transferDirectory().appendingPathComponent(
            transferID.uuidString.lowercased(), isDirectory: true
        )
        try? FileManager.default.removeItem(at: root)
    }

    private func finishImport(_ request: [String: Any]) throws -> [String: Any] {
        guard let transferText = request["transferID"] as? String,
              let transferID = UUID(uuidString: transferText) else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let transferRoot = try transferDirectory().appendingPathComponent(
            transferID.uuidString.lowercased(), isDirectory: true
        )
        let archive = transferRoot.appendingPathComponent("upload.outershell-container")
        let extracted = transferRoot.appendingPathComponent("Extracted", isDirectory: true)
        if !FileManager.default.fileExists(atPath: extracted.path) {
            let listing = try runCommand(
                executable: "/usr/bin/tar",
                arguments: ["-tzf", archive.path]
            )
            try requireSuccess(listing, action: "inspect the shared container")
            let entries = listing.stdout.components(separatedBy: .newlines).filter { !$0.isEmpty }
            guard !entries.isEmpty,
                  entries.allSatisfy({ entry in
                      !entry.hasPrefix("/") &&
                          !entry.split(separator: "/", omittingEmptySubsequences: false).contains("..")
                  }) else {
                throw SafeSpaceManagerError.commandFailed(
                    "The shared container contains unsafe paths."
                )
            }
            let verboseListing = try runCommand(
                executable: "/usr/bin/tar",
                arguments: ["-tvzf", archive.path]
            )
            try requireSuccess(verboseListing, action: "inspect the shared container")
            let containsLinks = verboseListing.stdout.components(separatedBy: .newlines)
                .filter { !$0.isEmpty }
                .contains { line in
                    guard let type = line.first else { return true }
                    return type != "-" && type != "d"
                }
            guard !containsLinks else {
                throw SafeSpaceManagerError.commandFailed(
                    "The shared container contains symbolic or hard links, which cannot be imported safely."
                )
            }
            try FileManager.default.createDirectory(
                at: extracted,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            let extraction = try runCommand(
                executable: "/usr/bin/tar",
                arguments: ["-xzf", archive.path, "-C", extracted.path]
            )
            try requireSuccess(extraction, action: "open the shared container")
        }
        let manifestData = try Data(contentsOf: extracted.appendingPathComponent("manifest.orwv"))
        let manifest = try OffsetArchive.decode(
            SafeSpaceTransferManifest.self,
            from: manifestData
        )
        guard manifest.version == 3 else {
            throw SafeSpaceManagerError.commandFailed("This shared container uses an unsupported format.")
        }
        let providerText = request["runtimeProviderID"] as? String
            ?? availableDefaultRuntimeProvider().rawValue
        guard let provider = SafeSpaceRuntimeProviderID(rawValue: providerText),
              runtimeExecutableURL(for: provider) != nil else {
            throw SafeSpaceManagerError.unsupportedProvider
        }
        let requestedName = (request["name"] as? String)?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let importedName = requestedName.flatMap { $0.isEmpty ? nil : $0 } ?? manifest.name
        let mountDirectoryNames = importMountDirectoryNames(manifest.includedMounts)
        let mountDestinationRoot = (request["mountDestinationRoot"] as? String)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if !manifest.includedMounts.isEmpty && mountDestinationRoot.isEmpty {
            let suggestedRoot = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Outer Shell Containers", isDirectory: true)
                .appendingPathComponent(
                    safeImportDirectoryName(importedName, fallback: "Imported Container"),
                    isDirectory: true
                )
            return [
                "needsMountDestination": true,
                "importName": importedName,
                "suggestedMountRoot": suggestedRoot.path,
                "importMounts": manifest.includedMounts.map { mount in
                    [
                        "id": mount.id.uuidString.lowercased(),
                        "name": mount.name,
                        "sourceHostPath": mount.hostPath,
                        "guestPath": mount.guestPath,
                        "isReadOnly": mount.isReadOnly,
                        "directoryName": mountDirectoryNames[mount.id] ?? "Mounted Folder"
                    ] as [String: Any]
                }
            ]
        }
        let destinationRoot: URL?
        if manifest.includedMounts.isEmpty {
            destinationRoot = nil
        } else {
            let expandedPath = NSString(string: mountDestinationRoot).expandingTildeInPath
            guard expandedPath.hasPrefix("/"), expandedPath != "/" else {
                throw SafeSpaceManagerError.commandFailed(
                    "Choose an absolute destination folder on this server."
                )
            }
            let root = URL(fileURLWithPath: expandedPath, isDirectory: true)
            var isDirectory: ObjCBool = false
            if FileManager.default.fileExists(atPath: root.path, isDirectory: &isDirectory),
               !isDirectory.boolValue {
                throw SafeSpaceManagerError.commandFailed(
                    "The mounted-folder destination is not a folder."
                )
            }
            for mount in manifest.includedMounts {
                let destination = root.appendingPathComponent(
                    mountDirectoryNames[mount.id] ?? "Mounted Folder",
                    isDirectory: true
                )
                if FileManager.default.fileExists(atPath: destination.path) {
                    throw SafeSpaceManagerError.commandFailed(
                        "The destination already contains \(destination.lastPathComponent). " +
                            "Choose another folder so imported data is not overwritten."
                    )
                }
            }
            destinationRoot = root
        }
        for mount in manifest.includedMounts {
            let source = extracted.appendingPathComponent("Mounted Folders", isDirectory: true)
                .appendingPathComponent(mount.id.uuidString.lowercased(), isDirectory: true)
            guard FileManager.default.fileExists(atPath: source.path) else {
                throw SafeSpaceManagerError.commandFailed(
                    "The shared container is missing the included folder \(mount.name)."
                )
            }
        }
        let importedID = UUID()
        let importedResources = (resourceLimits(for: provider) ?? .host)
            .runnable(cpus: manifest.cpus, memoryInMB: manifest.memoryInMB)
        let record = SafeSpaceRecord(
            id: importedID, name: importedName, createdAt: Date(),
            cpus: importedResources.cpus, memoryInMB: importedResources.memoryInMB,
            runtimeProviderID: provider.rawValue,
            runtimeName: managedContainerName(importedID),
            ownsContainer: true,
            ownsBuildContext: true,
            selfModifying: true
        )
        try createManagedDirectories(for: record)
        let destinationBuildContext = try buildContextDirectory(record.id)
        if FileManager.default.fileExists(atPath: destinationBuildContext.path) {
            try FileManager.default.removeItem(at: destinationBuildContext)
        }
        try FileManager.default.moveItem(
            at: extracted.appendingPathComponent("BuildContext", isDirectory: true),
            to: destinationBuildContext
        )
        let transferredConfigurationURL = destinationBuildContext.appendingPathComponent("configuration.orwv")
        let transferredConfigurationData = try Data(contentsOf: transferredConfigurationURL)
        try FileManager.default.removeItem(at: transferredConfigurationURL)
        var importedConfiguration = try OffsetArchive.decode(
            SafeSpaceConfiguration.self,
            from: transferredConfigurationData
        )
        let importedDockerfileURL = try dockerfileURL(record.id)
        var importedDockerfile = try String(
            contentsOf: importedDockerfileURL,
            encoding: .utf8
        )
        let upgradedImportedDockerfile = upgradedDockerfile(importedDockerfile)
        if upgradedImportedDockerfile != importedDockerfile {
            importedDockerfile = upgradedImportedDockerfile
            try importedDockerfile.write(
                to: importedDockerfileURL,
                atomically: true,
                encoding: .utf8
            )
        }
        importedConfiguration.baseImage = dockerfileBaseImage(in: importedDockerfile) ?? defaultContainerBaseImage
        var importedMounts: [SafeSpaceMount] = []
        for mount in manifest.includedMounts {
            let source = extracted.appendingPathComponent("Mounted Folders", isDirectory: true)
                .appendingPathComponent(mount.id.uuidString.lowercased(), isDirectory: true)
            guard FileManager.default.fileExists(atPath: source.path) else {
                throw SafeSpaceManagerError.commandFailed(
                    "The shared container is missing the included folder \(mount.name)."
                )
            }
            guard let destinationRoot else {
                throw SafeSpaceManagerError.commandFailed(
                    "Choose where the included mounted folders should be copied."
                )
            }
            try FileManager.default.createDirectory(
                at: destinationRoot,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            let destination = destinationRoot.appendingPathComponent(
                mountDirectoryNames[mount.id] ?? "Mounted Folder",
                isDirectory: true
            )
            do {
                try FileManager.default.moveItem(at: source, to: destination)
            } catch {
                try FileManager.default.copyItem(at: source, to: destination)
                try FileManager.default.removeItem(at: source)
            }
            var imported = mount
            imported.hostPath = destination.path
            importedMounts.append(imported)
        }
        importedConfiguration.mounts = importedMounts
        importedConfiguration.realizedMounts = nil
        importedConfiguration.realizedDockerfileContents = nil
        importedConfiguration.realizedEnvironment = nil
        importedConfiguration.realizedPublishedPorts = nil
        importedConfiguration.realizedCPUs = nil
        importedConfiguration.realizedMemoryInMB = nil
        importedConfiguration.hasUntrackedChanges = false
        try saveConfiguration(importedConfiguration, for: record)
        let persistentSource = extracted.appendingPathComponent("Volume Archives", isDirectory: true)
        let persistentDestination = try safeSpaceDirectory(record.id)
            .appendingPathComponent("Volume Archives", isDirectory: true)
        if FileManager.default.fileExists(atPath: persistentSource.path) {
            if FileManager.default.fileExists(atPath: persistentDestination.path) {
                try FileManager.default.removeItem(at: persistentDestination)
            }
            try FileManager.default.moveItem(at: persistentSource, to: persistentDestination)
        }
        lock.withSafeSpaceLock {
            records.append(record)
            transientStates[record.id] = "creating"
        }
        try saveRecords()
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                let imageReference = self.containerImageReference(record.id)
                _ = try self.runContainerBuild(record, imageReference: imageReference)
                try self.createRuntimeContainer(record, imageReference: imageReference)
                try self.markRealized(for: record)
                self.setTransientState(nil, for: record.id)
            } catch {
                NSLog("Container import failed: %@", error.localizedDescription)
                self.setTransientState("error", for: record.id)
            }
        }
        try? FileManager.default.removeItem(at: transferRoot)
        return [
            "importedWorkspaceID": record.id.uuidString.lowercased(),
            "omittedMountCount": manifest.omittedMounts.count
        ]
    }

    private func availableDefaultRuntimeProvider() -> SafeSpaceRuntimeProviderID {
        runtimeExecutableURL(for: .docker) != nil ? .docker : .appleContainer
    }

    private func safeTransferFileName(_ name: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        let stem = name.unicodeScalars.map { allowed.contains($0) ? String($0) : "-" }.joined()
        return "\(stem.isEmpty ? "Container" : stem).outershell-container"
    }

    private func safeImportDirectoryName(_ value: String, fallback: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_ ."))
        let name = value.unicodeScalars
            .map { allowed.contains($0) ? String($0) : "-" }
            .joined()
            .trimmingCharacters(in: CharacterSet(charactersIn: " ."))
        return name.isEmpty ? fallback : name
    }

    private func importMountDirectoryNames(_ mounts: [SafeSpaceMount]) -> [UUID: String] {
        var names: [UUID: String] = [:]
        var usedNames = Set<String>()
        for mount in mounts {
            let sourceName = URL(fileURLWithPath: mount.hostPath).lastPathComponent
            let baseName = safeImportDirectoryName(
                mount.name.isEmpty ? sourceName : mount.name,
                fallback: "Mounted Folder"
            )
            var name = baseName
            var suffix = 2
            while usedNames.contains(name.lowercased()) {
                name = "\(baseName) \(suffix)"
                suffix += 1
            }
            usedNames.insert(name.lowercased())
            names[mount.id] = name
        }
        return names
    }

    private func beginRuntimeChange(_ request: [String: Any]) throws -> [String: Any] {
        let source = try record(from: request)
        guard let providerText = request["runtimeProviderID"] as? String,
              let provider = SafeSpaceRuntimeProviderID(rawValue: providerText),
              provider != runtimeProvider(for: source),
              runtimeExecutableURL(for: provider) != nil else {
            throw SafeSpaceManagerError.unsupportedProvider
        }
        let isAlreadyBuilding = lock.withSafeSpaceLock {
            guard let progress = buildProgress[source.id] else { return false }
            return progress.phase != "complete" && progress.phase != "failed"
        }
        guard !isAlreadyBuilding else {
            throw SafeSpaceManagerError.commandFailed(
                "The container is already rebuilding."
            )
        }
        setTransientState("rebuilding", for: source.id)
        setBuildProgress(
            SafeSpaceBuildProgress(
                phase: "preparing",
                detail: "Preparing the container for \(provider.displayName)"
            ),
            for: source.id
        )
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                try self.changeRuntime(of: source, to: provider)
                self.finishBuild(
                    phase: "complete",
                    detail: "Container is now running with \(provider.displayName).",
                    for: source.id
                )
            } catch {
                self.finishBuild(
                    phase: "failed",
                    detail: error.localizedDescription,
                    for: source.id
                )
            }
        }
        return [:]
    }

    private func changeRuntime(
        of source: SafeSpaceRecord,
        to provider: SafeSpaceRuntimeProviderID
    ) throws {
        var target = source
        target.runtimeProviderID = provider.rawValue
        let sourceState = try runtimeState(source)
        let sourceWasRunning = sourceState == "running"
        var sourceWasStopped = false
        var targetWasCreated = false
        var providerWasChanged = false
        var transferredItems: [SafeSpacePersistentData] = []

        do {
            let staleTarget = try removeRuntimeContainer(target, force: true)
            if staleTarget.status != 0 && !runtimeObjectIsMissing(staleTarget) {
                try requireSuccess(staleTarget, action: "remove a previous target container")
            }

            let imageReference = containerImageReference(source.id)
            _ = try runContainerBuild(target, imageReference: imageReference)
            updateBuildProgressPhase(
                "replacing",
                detail: "Switching from \(runtimeProvider(for: source).displayName) to \(provider.displayName)",
                for: source.id
            )

            cancelAppEventMonitor(for: source.id)
            closePublishedForwards(source.id)
            if sourceWasRunning {
                try requireSuccess(
                    try runRuntime(source, [
                        "stop", "--time", "15", containerName(source.id)
                    ]),
                    action: "pause the current container"
                )
                sourceWasStopped = true
            }

            for item in try configuration(for: source).persistentData ?? [] where item.isDeclared {
                let legacy = try persistentDataDirectory(source, item: item)
                if FileManager.default.fileExists(atPath: legacy.path) { continue }
                let archive = try volumeArchiveURL(source, item: item)
                transferredItems.append(item)
                try transferPersistentVolume(source, item: item, archive: archive, restore: false)
                let name = persistentVolumeName(target, item: item)
                if try runRuntime(target, ["volume", "inspect", name]).status == 0 {
                    try requireSuccess(try runRuntime(target, ["volume", "rm", name]), action: "replace previous target storage")
                }
            }

            updateBuildProgressPhase(
                "starting",
                detail: "Starting the container with \(provider.displayName)",
                for: source.id
            )
            try createRuntimeContainer(target, imageReference: imageReference)
            targetWasCreated = true
            Thread.sleep(forTimeInterval: 0.25)
            guard try runtimeState(target) == "running" else {
                throw SafeSpaceManagerError.commandFailed(
                    "The converted container did not remain running."
                )
            }
            if !sourceWasRunning {
                try requireSuccess(
                    try runRuntime(target, [
                        "stop", "--time", "10", containerName(target.id)
                    ]),
                    action: "restore the container state"
                )
            }

            updateBuildProgressPhase(
                "finishing",
                detail: "Saving the new container runtime",
                for: source.id
            )
            lock.withSafeSpaceLock {
                guard let index = records.firstIndex(where: { $0.id == source.id }) else {
                    return
                }
                records[index].runtimeProviderID = provider.rawValue
            }
            providerWasChanged = true
            try saveRecords()
            try markRealized(for: target)
            _ = lock.withSafeSpaceLock {
                cachedCommandIcons.removeValue(forKey: source.id)
            }

            let oldRuntime = try removeRuntimeContainer(source, force: true)
            if oldRuntime.status != 0 && !runtimeObjectIsMissing(oldRuntime) {
                NSLog(
                    "Converted container, but could not remove its previous runtime: %@",
                    oldRuntime.stderr
                )
            }
            if oldRuntime.status == 0 || runtimeObjectIsMissing(oldRuntime) {
                for item in transferredItems {
                    if let removal = try? runRuntime(source, ["volume", "rm", persistentVolumeName(source, item: item)]), removal.status != 0 {
                        NSLog("Converted container, but could not remove previous storage: %@", removal.stderr)
                    }
                }
            }
        } catch {
            if providerWasChanged {
                lock.withSafeSpaceLock {
                    guard let index = records.firstIndex(where: { $0.id == source.id }) else {
                        return
                    }
                    records[index].runtimeProviderID = source.runtimeProviderID
                }
                try? saveRecords()
            }
            if targetWasCreated || !transferredItems.isEmpty {
                _ = try? removeRuntimeContainer(target, force: true)
            }
            for item in transferredItems {
                _ = try? runRuntime(target, ["volume", "rm", persistentVolumeName(target, item: item)])
                if let archive = try? volumeArchiveURL(source, item: item) {
                    try? FileManager.default.removeItem(at: archive)
                }
            }
            if sourceWasRunning && sourceWasStopped {
                _ = try? runRuntime(source, ["start", containerName(source.id)])
            }
            throw error
        }
    }

    private func createRuntimeContainer(_ record: SafeSpaceRecord,
                                        imageReference: String) throws {
        containerBaseImageLock.lock()
        defer { containerBaseImageLock.unlock() }
        _ = lock.withSafeSpaceLock {
            cachedTerminalShells.removeValue(forKey: record.id)
        }
        switch runtimeProvider(for: record) {
        case .appleContainer:
            try createAppleContainer(record, imageReference: imageReference)
            scheduleAppleImageCleanup()
        case .docker:
            try createDockerContainer(record, imageReference: imageReference)
        }
    }

    private func runnableResources(_ record: SafeSpaceRecord) -> (cpus: Int, memoryInMB: Int) {
        let provider = record.runtimeProviderID.flatMap(SafeSpaceRuntimeProviderID.init(rawValue:)) ?? .appleContainer
        guard let limits = resourceLimits(for: provider) else { return (record.cpus, record.memoryInMB) }
        return limits.runnable(cpus: record.cpus, memoryInMB: record.memoryInMB)
    }

    private func createAppleContainer(_ record: SafeSpaceRecord,
                                      imageReference: String) throws {
        try createManagedDirectories(for: record)
        let configuration = try configuration(for: record)
        var arguments = [
            "create",
            "--name", containerName(record.id),
            "--cpus", String(runnableResources(record).cpus),
            "--memory", "\(runnableResources(record).memoryInMB)M",
            "--init",
        ] + containerRuntimeArguments + [
            "--label", "org.outershell.container-id=\(record.id.uuidString)"
        ]
        if recordIsSelfModifying(record) {
            arguments.append(contentsOf: [
                "--volume", "\(try checkedBuildContextDirectory(record).path):/var/lib/outershell/project"
            ])
        }
        for mount in configuration.mounts ?? [] {
            let suffix = mount.isReadOnly ? ":ro" : ""
            arguments.append(contentsOf: [
                "--volume", "\(mount.hostPath):\(mount.guestPath)\(suffix)"
            ])
        }
        let persistentData = try reconcilePersistentData(
            for: record,
            imageReference: imageReference
        )
        var occupiedGuestPaths: Set<String> = recordIsSelfModifying(record) ? ["/var/lib/outershell/project"] : []
        occupiedGuestPaths.formUnion((configuration.mounts ?? []).map(\.guestPath))
        for item in persistentData where !occupiedGuestPaths.contains(item.guestPath) {
            try preparePersistentVolume(record, item: item)
            arguments.append(contentsOf: [
                "--mount", try persistentMount(record, item: item) + ",target=" + item.guestPath
            ])
        }
        arguments.append(contentsOf: ["--env", "OUTERSHELL_RELAY_DIRECTORY=\(relayDirectory(record.id).path)"])
        for variable in configuration.environment ?? [] {
            arguments.append(contentsOf: [
                "--env", "\(variable.name)=\(variable.value)"
            ])
        }
        for port in configuration.publishedPorts ?? [] {
            arguments.append(contentsOf: [
                "--publish", "127.0.0.1:\(port.hostPort):\(port.containerPort)/tcp"
            ])
        }
        arguments.append(imageReference)
        let result = try runContainer(arguments)
        try requireSuccess(result, action: "create the container")
        try requireSuccess(try runContainer(["start", containerName(record.id)]),
                           action: "start the container")
    }

    private func createDockerContainer(_ record: SafeSpaceRecord,
                                       imageReference: String) throws {
        try createManagedDirectories(for: record)
        let configuration = try configuration(for: record)
        var arguments = [
            "create",
            "--name", containerName(record.id),
            "--cpus", String(runnableResources(record).cpus),
            "--memory", "\(runnableResources(record).memoryInMB)m",
            "--init",
        ] + containerRuntimeArguments + [
            "--label", "org.outershell.container-id=\(record.id.uuidString)"
        ]
        if recordIsSelfModifying(record) {
            arguments.append(contentsOf: [
                "--volume", "\(try checkedBuildContextDirectory(record).path):/var/lib/outershell/project"
            ])
        }
        for mount in configuration.mounts ?? [] {
            arguments.append(contentsOf: [
                "--volume", "\(mount.hostPath):\(mount.guestPath)\(mount.isReadOnly ? ":ro" : "")"
            ])
        }
        let persistentData = try reconcilePersistentData(
            for: record,
            imageReference: imageReference
        )
        var occupiedGuestPaths: Set<String> = recordIsSelfModifying(record) ? ["/var/lib/outershell/project"] : []
        occupiedGuestPaths.formUnion((configuration.mounts ?? []).map(\.guestPath))
        for item in persistentData where !occupiedGuestPaths.contains(item.guestPath) {
            try preparePersistentVolume(record, item: item)
            arguments.append(contentsOf: [
                "--mount", try persistentMount(record, item: item) + ",target=" + item.guestPath
            ])
        }
        arguments.append(contentsOf: ["--env", "OUTERSHELL_RELAY_DIRECTORY=\(relayDirectory(record.id).path)"])
        for variable in configuration.environment ?? [] {
            arguments.append(contentsOf: ["--env", "\(variable.name)=\(variable.value)"])
        }
        for port in configuration.publishedPorts ?? [] {
            arguments.append(contentsOf: [
                "--publish", "127.0.0.1:\(port.hostPort):\(port.containerPort)/tcp"
            ])
        }
        arguments.append(imageReference)
        try requireSuccess(try runDocker(arguments), action: "create the Docker container")
        try requireSuccess(
            try runDocker(["start", containerName(record.id)]),
            action: "start the Docker container"
        )
    }

    private func recreateContainer(_ record: SafeSpaceRecord,
                                   imageReference: String) throws {
        let previousState = try runtimeState(record)
        cancelAppEventMonitor(for: record.id)
        closePublishedForwards(record.id)
        if previousState != "absent" {
            let deletion = try removeRuntimeContainer(record, force: true)
            if deletion.status != 0 && !runtimeObjectIsMissing(deletion) {
                try requireSuccess(deletion, action: "update the container")
            }
        }
        try createRuntimeContainer(record, imageReference: imageReference)
        if previousState == "stopped" {
            try requireSuccess(
                try runRuntime(record, ["stop", "--time", "10", containerName(record.id)]),
                action: "restore the container state"
            )
        }
    }

    private func realizedImageReference(for record: SafeSpaceRecord) throws -> String {
        let value = normalizedConfiguration(try configuration(for: record))
        guard value.version == currentSafeSpaceConfigurationVersion,
              value.realizedDockerfileContents == (try? dockerfileContents(for: record)) else {
            return value.baseImage
        }
        return containerImageReference(record.id)
    }

    private func sanitizedMountComponent(_ name: String,
                                         existing: [SafeSpaceMount]) -> String {
        let base = name.lowercased()
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        let preferred = base.isEmpty ? "folder" : base
        let existingNames = Set(existing.map {
            URL(fileURLWithPath: $0.guestPath).lastPathComponent
        })
        guard existingNames.contains(preferred) else { return preferred }
        var suffix = 2
        while existingNames.contains("\(preferred)-\(suffix)") {
            suffix += 1
        }
        return "\(preferred)-\(suffix)"
    }

    private func rename(_ request: [String: Any]) throws {
        let selected = try record(from: request)
        guard let rawName = request["name"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            throw SafeSpaceManagerError.invalidRequest
        }
        lock.withSafeSpaceLock {
            guard let index = records.firstIndex(where: { $0.id == selected.id }) else {
                return
            }
            records[index].name = name
        }
        try saveRecords()
    }

    private func start(_ record: SafeSpaceRecord) async throws {
        let requiresBuild = try runtimeState(record) == "absent"
        setTransientState("starting", for: record.id)
        if requiresBuild {
            setBuildProgress(
                SafeSpaceBuildProgress(
                    phase: "preparing",
                    detail: "Preparing the container build"
                ),
                for: record.id
            )
        }
        do {
            try await startNow(record)
            if requiresBuild {
                finishBuild(
                    phase: "complete",
                    detail: "Container built and started. Apps are ready to use.",
                    for: record.id
                )
            }
        } catch {
            if requiresBuild {
                finishBuild(
                    phase: "failed",
                    detail: error.localizedDescription,
                    for: record.id
                )
            } else {
                setTransientState("error", for: record.id)
            }
            throw error
        }
    }

    private func startNow(_ record: SafeSpaceRecord) async throws {
        if try runtimeState(record) == "absent" {
            guard recordOwnsContainer(record) else {
                throw SafeSpaceManagerError.commandFailed(
                    "The attached container is not present in \(runtimeProvider(for: record).displayName). Outer Shell will reconnect when a container named \(registeredRuntimeName(for: record)) appears."
                )
            }
            let imageReference = containerImageReference(record.id)
            _ = try runContainerBuild(record, imageReference: imageReference)
            try createRuntimeContainer(record, imageReference: imageReference)
            try markRealized(for: record)
        } else {
            try requireSuccess(try runRuntime(record, ["start", containerName(record.id)]),
                               action: "start the container")
        }
        setTransientState(nil, for: record.id)
    }

    private func stop(_ record: SafeSpaceRecord) async throws {
        cancelAppEventMonitor(for: record.id)
        closePublishedForwards(record.id)
        try requireSuccess(
            try runRuntime(record, ["stop", "--time", "10", containerName(record.id)]),
            action: "stop the container"
        )
    }

    private func dockerStorageCandidates(excluding id: UUID? = nil) throws -> [ContainerImageStorage.Image] {
        let listing = try runDocker(["image", "ls", "--all", "--quiet", "--no-trunc"])
        try requireSuccess(listing, action: "list Docker images")
        var images: [ContainerImageStorage.Image] = []
        for identifier in Set(listing.stdout.split(whereSeparator: \.isWhitespace).map(String.init)).sorted() {
            let result = try runDocker(["image", "inspect", identifier])
            try requireSuccess(result, action: "inspect Docker images")
            images += try ContainerImageStorage.dockerImages(from: Data(result.stdout.utf8))
        }
        var protected = containerSupportImageReferences
        var files: [String] = []
        for record in try loadRecords() where record.id != id && recordOwnsContainer(record) {
            if runtimeProvider(for: record) == .docker { protected.insert(containerImageReference(record.id)) }
            let value = try configuration(for: record)
            protected.insert(value.baseImage)
            if let realized = value.realizedBaseImage { protected.insert(realized) }
            if let dockerfile = try? dockerfileContents(for: record) { files.append(dockerfile) }
            if let realized = value.realizedDockerfileContents { files.append(realized) }
        }
        let containers = try runDocker(["ps", "--all", "--quiet", "--no-trunc"])
        try requireSuccess(containers, action: "list Docker containers")
        for identifier in containers.stdout.split(whereSeparator: \.isWhitespace) {
            let result = try runDocker(["inspect", String(identifier)])
            try requireSuccess(result, action: "inspect Docker container image references")
            guard let values = try JSONSerialization.jsonObject(with: Data(result.stdout.utf8)) as? [[String: Any]],
                  values.count == 1, let digest = values.first?["Image"] as? String else {
                throw CocoaError(.fileReadCorruptFile)
            }
            protected.insert(digest)
        }
        return ContainerImageStorage.candidates(images: images, protected: protected, dockerfiles: files,
            workspacePrefix: "outerloop/workspace-\(getuid()):")
    }

    private func dockerStorage(request: [String: Any], operation: String) throws -> [String: Any] {
        containerBaseImageLock.lock()
        defer { containerBaseImageLock.unlock() }
        guard !lock.withSafeSpaceLock({ transientStates.values.contains { ["creating", "rebuilding", "starting"].contains($0) } }) else {
            throw SafeSpaceManagerError.commandFailed("Wait for container builds to finish, then try cleanup again.")
        }
        var deleted: [String] = []
        var failures: [String] = []
        if operation == "cleanContainerBuildCache" {
            try requireSuccess(try runDocker(["builder", "prune", "--force", "--filter", "until=168h", "--keep-storage", "10GB"]), action: "clean old Docker build cache")
        } else if operation == "cleanContainerStorage" {
            guard let selected = request["images"] as? [String] else { throw SafeSpaceManagerError.invalidRequest }
            let result = try ContainerImageStorage.deleteSelected(selected, candidates: {
                try self.dockerStorageCandidates()
            }, delete: { reference in
                try self.requireSuccess(try self.runDocker(["image", "rm", reference]), action: "remove an unused Docker image")
            })
            deleted = result.deleted
            failures = result.failures
        }
        return ["storage": ["images": try dockerStorageCandidates().map { image -> [String: Any] in
            var value: [String: Any] = ["reference": image.reference]
            if let bytes = image.sizeInBytes { value["imageBytes"] = bytes }
            return value
        }, "deleted": deleted, "failures": failures, "supportsBuildCacheCleanup": true]]
    }

    private func appleStorageProtection(excluding id: UUID? = nil) throws -> (Set<String>, [String]) {
        var protected = containerSupportImageReferences
        var dockerfiles: [String] = []
        for record in try loadRecords() where record.id != id && recordOwnsContainer(record) {
            if runtimeProvider(for: record) == .appleContainer {
                protected.insert(containerImageReference(record.id))
            }
            let value = try configuration(for: record)
            protected.insert(value.baseImage)
            if let realized = value.realizedBaseImage { protected.insert(realized) }
            if let dockerfile = try? dockerfileContents(for: record) { dockerfiles.append(dockerfile) }
            if let realized = value.realizedDockerfileContents { dockerfiles.append(realized) }
        }
        let containers = try runContainer(["list", "--all", "--format", "json"])
        try requireSuccess(containers, action: "inspect containers before image cleanup")
        protected.formUnion(try ContainerImageStorage.containerReferences(from: Data(containers.stdout.utf8)))
        return (protected, dockerfiles)
    }

    private func appleStorageImages() throws -> [ContainerImageStorage.Image] {
        let result = try runContainer(["image", "list", "--format", "json"])
        try requireSuccess(result, action: "inspect images before cleanup")
        return try ContainerImageStorage.images(from: Data(result.stdout.utf8))
    }

    private func appleStorageCandidates(excluding id: UUID? = nil) throws -> [ContainerImageStorage.Image] {
        let (protected, dockerfiles) = try appleStorageProtection(excluding: id)
        return ContainerImageStorage.candidates(
            images: try appleStorageImages(), protected: protected, dockerfiles: dockerfiles,
            workspacePrefix: "outerloop/workspace-\(getuid()):"
        )
    }

    private func containerStorage(request: [String: Any], clean: Bool) throws -> [String: Any] {
        containerBaseImageLock.lock()
        defer { containerBaseImageLock.unlock() }
        guard !lock.withSafeSpaceLock({ transientStates.values.contains { ["creating", "rebuilding", "starting"].contains($0) } }) else {
            throw SafeSpaceManagerError.commandFailed("Wait for container builds to finish, then try cleanup again.")
        }
        var deleted: [String] = []
        var failures: [String] = []
        if clean {
            guard let requested = request["images"] as? [String] else { throw SafeSpaceManagerError.invalidRequest }
            let result = try ContainerImageStorage.deleteSelected(requested, candidates: {
                try self.appleStorageCandidates()
            }, delete: { reference in
                try self.requireSuccess(try self.runContainer(["image", "delete", reference]), action: "delete an unused image")
            })
            deleted = result.deleted
            failures = result.failures
            do { try pruneAppleImageCache() }
            catch { failures.append(error.localizedDescription) }
        }
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/com.apple.container")
        let candidates = try appleStorageCandidates().map { image -> [String: Any] in
            var value: [String: Any] = ["reference": image.reference]
            if let bytes = try? ContainerImageStorage.allocatedSnapshotBytes(for: image, root: root) {
                value["snapshotBytes"] = bytes
            }
            return value
        }
        var storage: [String: Any] = ["images": candidates, "deleted": deleted, "failures": failures]
        if let bytes = try? ContainerImageStorage.orphanSnapshotBytes(root: root) {
            storage["orphanSnapshotBytes"] = bytes
        }
        return ["storage": storage]
    }

    private func pruneAppleImageCache() throws {
        let images = try appleStorageImages()
        guard images.allSatisfy({ !$0.reference.contains("@") && $0.reference.split(separator: "/").last?.contains(":") == true }) else {
            throw SafeSpaceManagerError.commandFailed("Cache cleanup skipped because untagged images may still be needed. Review them using the container CLI.")
        }
        try requireSuccess(try runContainer(["image", "prune"]), action: "clean unused image snapshots")
    }

    private func scheduleAppleImageCleanup() {
        lock.withSafeSpaceLock {
            imageCleanupWork?.cancel()
            let work = DispatchWorkItem { [weak self] in
                guard let self else { return }
                self.containerBaseImageLock.lock()
                defer { self.containerBaseImageLock.unlock() }
                if self.lock.withSafeSpaceLock({ self.transientStates.values.contains { ["creating", "rebuilding", "starting"].contains($0) } }) {
                    self.scheduleAppleImageCleanup()
                    return
                }
                do { try self.pruneAppleImageCache() }
                catch { NSLog("Container image cache cleanup failed: %@", error.localizedDescription) }
            }
            imageCleanupWork = work
            imageCleanupQueue.asyncAfter(deadline: .now() + 30, execute: work)
        }
    }

    private func removeOwnedContainerImage(_ record: SafeSpaceRecord) throws {
        containerBaseImageLock.lock()
        defer { containerBaseImageLock.unlock() }
        let reference = containerImageReference(record.id)
        if runtimeProvider(for: record) == .docker {
            if try dockerStorageCandidates(excluding: record.id).contains(where: { $0.reference == reference }) {
                try requireSuccess(try runDocker(["image", "rm", reference]), action: "delete the container image")
            }
            return
        }
        if try appleStorageCandidates(excluding: record.id).contains(where: { $0.reference == reference }) {
            try requireSuccess(try runContainer(["image", "delete", reference]), action: "delete the container image")
        }
        scheduleAppleImageCleanup()
    }

    private func delete(_ record: SafeSpaceRecord) async throws {
        cancelAppEventMonitor(for: record.id)
        closePublishedForwards(record.id)
        if recordOwnsContainer(record) {
            let result = try removeRuntimeContainer(record, force: true)
            if result.status != 0 && !runtimeObjectIsMissing(result) {
                try requireSuccess(result, action: "delete the container")
            }
            for item in try configuration(for: record).persistentData ?? [] {
                let name = persistentVolumeName(record, item: item)
                if try runRuntime(record, ["volume", "inspect", name]).status == 0 {
                    try requireSuccess(try runRuntime(record, ["volume", "rm", name]), action: "delete persistent storage")
                }
            }
        }
        if recordOwnsContainer(record) {
            try removeOwnedContainerImage(record)
        }
        lock.withSafeSpaceLock {
            records.removeAll { $0.id == record.id }
            cachedApps.removeValue(forKey: record.id)
            cachedCommandIcons.removeValue(forKey: record.id)
            cachedTerminalShells.removeValue(forKey: record.id)
            transientStates.removeValue(forKey: record.id)
        }
        try removeRecordRegistration(record.id)
        try saveCachedApps()
        if recordOwnsContainer(record) {
            try RegistryResourceStore.remove(configurationResourceKey(record.id))
            do {
                let directory = try safeSpaceDirectory(record.id)
                if FileManager.default.fileExists(atPath: directory.path) {
                    try FileManager.default.removeItem(at: directory)
                }
            } catch {
                NSLog("Container folder removal failed for %@: %@",
                      record.id.uuidString, error.localizedDescription)
            }
        }
    }

    private func mountFolder(_ request: [String: Any]) async throws {
        let selected = try record(from: request)
        let hostPath: String
        if let requestedPath = request["hostPath"] as? String, !requestedPath.isEmpty {
            hostPath = requestedPath
        } else if let selectedPath = await chooseFolder() {
            hostPath = selectedPath
        } else {
            return
        }
        let url = URL(fileURLWithPath: hostPath, isDirectory: true)
        var value = try configuration(for: selected)
        if value.realizedMounts == nil {
            value.realizedMounts = value.mounts ?? []
        }
        let name = request["name"] as? String ?? url.lastPathComponent
        let component = sanitizedMountComponent(name, existing: value.mounts ?? [])
        value.mounts = (value.mounts ?? []) + [
            SafeSpaceMount(id: UUID(),
                           name: name,
                           hostPath: url.standardizedFileURL.path,
                           guestPath: "/workspaces/mounts/\(component)",
                           isReadOnly: request["readOnly"] as? Bool ?? false)
        ]
        try saveConfiguration(value, for: selected)
    }

    @MainActor
    private func chooseFolder() -> String? {
        NSApp.activate(ignoringOtherApps: true)
        let panel = NSOpenPanel()
        panel.title = "Mount Folder in Container"
        panel.message = "Choose a folder to share with this container."
        panel.prompt = "Mount"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        return panel.runModal() == .OK ? panel.url?.path : nil
    }

    private func unmountFolder(_ request: [String: Any]) async throws {
        let selected = try record(from: request)
        guard let text = request["mountID"] as? String,
              let mountID = UUID(uuidString: text) else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var value = try configuration(for: selected)
        if value.realizedMounts == nil {
            value.realizedMounts = value.mounts ?? []
        }
        value.mounts = (value.mounts ?? []).filter { $0.id != mountID }
        try saveConfiguration(value, for: selected)
    }

    private func controlApp(_ request: [String: Any], operation: String) async throws {
        let selected = try record(from: request)
        guard let serviceID = request["serviceID"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let verb: String
        switch operation {
        case "startApp":
            verb = "start"
        case "stopApp":
            verb = "stop"
        default:
            verb = "restart"
        }
        let body = "serviceID=\(formEncoded(serviceID))" +
            "&operation=\(formEncoded(verb))&scope=user"
        let response = try await safeSpaceUIAPIRequest(
            record: selected,
            route: 3,
            query: "",
            body: Data(body.utf8)
        )
        guard response.body.count >= 12 else {
            throw SafeSpaceManagerError.commandFailed(
                "The container app control response was invalid."
            )
        }
        let succeeded = response.body.safeSpaceUInt32(at: 0) & 1 != 0
        let message = try response.body.safeSpaceReferencedString(at: 4)
        guard (200..<300).contains(response.status), succeeded else {
            throw SafeSpaceManagerError.commandFailed(
                message.isEmpty ? "Could not \(verb) the container app." : message
            )
        }
    }

    private func safeSpaceUIAPIRequest(record: SafeSpaceRecord,
                                       route: UInt16,
                                       query: String,
                                       body: Data,
                                       timeoutSeconds: Int = 15) async throws -> SafeSpaceUIAPIResponse {
        let identity = try runtimeIdentity(for: record)
        let runtimeDirectory = identity.runtimeDirectory
        try ensureContainerSocketBridge(record)
        try ensureSafeSpaceControlSocketAllowed(record)
        let forward = try await SafeSpaceSocketRelayForward.startWorkspace(
            containerName: containerName(record.id),
            socketPath: "\(runtimeDirectory)/outershelld-api",
            runtimeExecutablePath: try requiredRuntimeExecutablePath(for: record),
            user: identity.user
        )
        defer {
            forward.close()
        }

        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else {
            throw SafeSpaceManagerError.commandFailed(
                "Could not create the container control connection."
            )
        }
        defer {
            Darwin.close(fd)
        }
        var timeout = timeval(tv_sec: timeoutSeconds, tv_usec: 0)
        setsockopt(fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeout,
                   socklen_t(MemoryLayout<timeval>.size))

        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = in_port_t(forward.port).bigEndian
        address.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))
        let connectResult = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard connectResult == 0 else {
            throw SafeSpaceManagerError.commandFailed(
                "Could not reach the container service manager: \(String(cString: strerror(errno)))"
            )
        }

        var message = Data(repeating: 0, count: 24)
        message.safeSpaceWrite(UInt16(26), at: 0)
        message.safeSpaceWrite(route, at: 2)
        message.safeSpaceWrite(UInt32(0), at: 4)
        message.safeSpaceAppendReference(Data(query.utf8), at: 8)
        message.safeSpaceAppendReference(body, at: 16)
        var frame = Data()
        frame.safeSpaceAppend(UInt32(message.count))
        frame.append(message)
        try writeSafeSpaceSocketData(frame, to: fd)

        let header = try readSafeSpaceSocketData(count: 4, from: fd)
        let responseLength = Int(header.safeSpaceUInt32(at: 0))
        guard responseLength >= 24, responseLength <= 16 * 1024 * 1024 else {
            throw SafeSpaceManagerError.commandFailed(
                "The container service manager returned an invalid response."
            )
        }
        let response = try readSafeSpaceSocketData(count: responseLength, from: fd)
        guard response.safeSpaceUInt16(at: 0) == 107 else {
            throw SafeSpaceManagerError.commandFailed(
                "The container service manager returned an unsupported response."
            )
        }
        return SafeSpaceUIAPIResponse(
            status: Int(response.safeSpaceUInt32(at: 2)),
            body: try response.safeSpaceReferencedData(at: 16)
        )
    }

    private func ensureSafeSpaceControlSocketAllowed(_ record: SafeSpaceRecord) throws {
        let identity = try runtimeIdentity(for: record)
        try ensureSafeSpaceSocketAllowed(
            record,
            socketPath: "\(identity.runtimeDirectory)/outershelld-api",
            user: identity.user
        )
    }

    private func writeSafeSpaceSocketData(_ data: Data, to fd: Int32) throws {
        var offset = 0
        while offset < data.count {
            let written = data.withUnsafeBytes { bytes in
                Darwin.send(fd,
                            bytes.baseAddress?.advanced(by: offset),
                            data.count - offset,
                            0)
            }
            if written > 0 {
                offset += written
            } else if written < 0 && errno == EINTR {
                continue
            } else {
                throw SafeSpaceManagerError.commandFailed(
                    "Could not send the container request: \(String(cString: strerror(errno)))"
                )
            }
        }
    }

    private func readSafeSpaceSocketData(count: Int, from fd: Int32) throws -> Data {
        var data = Data(count: count)
        var offset = 0
        while offset < count {
            let received = data.withUnsafeMutableBytes { bytes in
                Darwin.recv(fd,
                            bytes.baseAddress?.advanced(by: offset),
                            count - offset,
                            0)
            }
            if received > 0 {
                offset += received
            } else if received < 0 && errno == EINTR {
                continue
            } else if received == 0 {
                throw SafeSpaceManagerError.commandFailed(
                    "The container service manager closed the connection."
                )
            } else {
                throw SafeSpaceManagerError.commandFailed(
                    "Could not read the container response: \(String(cString: strerror(errno)))"
                )
            }
        }
        return data
    }

    private func formEncoded(_ value: String) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return value.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
    }

    private func appLog(_ request: [String: Any]) async throws -> [String: Any] {
        let selected = try record(from: request)
        guard let serviceID = request["serviceID"] as? String, !serviceID.isEmpty else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let index = request["logIndex"] as? Int ?? 0
        guard index >= 0 else { throw SafeSpaceManagerError.invalidRequest }
        var result: [String: Any] = [:]
        if request["includeBackends"] as? Bool != false {
            let backends = try await safeSpaceUIAPIRequest(
                record: selected, route: 1, query: "", body: Data()
            )
            guard backends.status == 200 else {
                throw SafeSpaceManagerError.commandFailed("Could not list the container's registered logs.")
            }
            result["backendsPayload"] = backends.body.base64EncodedString()
        }
        let query = "serviceID=\(formEncoded(serviceID))&logIndex=\(index)&bytes=524288"
        let log = try await safeSpaceUIAPIRequest(
            record: selected, route: 2, query: query, body: Data()
        )
        guard log.status == 200 else {
            throw SafeSpaceManagerError.commandFailed("Could not read the container app log.")
        }
        result["logPayload"] = log.body.base64EncodedString()
        return result
    }

    private func updateDockerfile(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let contents = request["dockerfile"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        try validateDockerfile(contents)
        try contents.write(to: try dockerfileURL(selected.id),
                           atomically: true,
                           encoding: .utf8)
        try saveConfiguration(try configuration(for: selected), for: selected)
        return [:]
    }

    private func updateContainerConfiguration(
        _ request: [String: Any]
    ) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let contents = request["dockerfile"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        try validateDockerfile(contents)
        let mounts = try configurationMounts(request["mounts"])
        let environment = try configurationEnvironment(request["environment"])
        let publishedPorts = try configurationPublishedPorts(request["publishedPorts"])
        var value = try configuration(for: selected)
        if value.realizedMounts == nil {
            value.realizedMounts = value.mounts ?? []
        }
        if value.realizedEnvironment == nil {
            value.realizedEnvironment = value.environment ?? []
        }
        if value.realizedPublishedPorts == nil {
            value.realizedPublishedPorts = value.publishedPorts ?? []
        }

        try contents.write(to: try dockerfileURL(selected.id),
                           atomically: true,
                           encoding: .utf8)
        value.mounts = mounts
        value.environment = environment
        value.publishedPorts = publishedPorts
        try saveConfiguration(value, for: selected)
        return [:]
    }

    private func updateResources(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        let provider = runtimeProvider(for: selected)
        let resources: (cpus: Int, memoryInMB: Int)
        switch (resourceLimits(for: provider) ?? .host).validated(cpus: request["cpus"],
                                                                  memoryInMB: request["memoryInMB"]) {
        case .success(let value):
            resources = value
        case .failure(let error):
            throw SafeSpaceManagerError.commandFailed(error.message)
        }
        var value = try configuration(for: selected)
        if value.realizedCPUs == nil || value.realizedMemoryInMB == nil {
            value.realizedCPUs = selected.cpus
            value.realizedMemoryInMB = selected.memoryInMB
            try saveConfiguration(value, for: selected)
        }
        var updated = selected
        updated.cpus = resources.cpus
        updated.memoryInMB = resources.memoryInMB
        lock.withSafeSpaceLock {
            if let index = records.firstIndex(where: { $0.id == selected.id }) {
                records[index] = updated
            }
        }
        try saveRecords()
        guard provider == .docker, try runtimeState(updated) != "absent" else {
            return ["resourcesApplied": false]
        }
        let applied = runnableResources(updated)
        let result = try runDocker([
            "update",
            "--cpus", String(applied.cpus),
            "--memory", "\(applied.memoryInMB)m",
            "--memory-swap", "\(applied.memoryInMB * 2)m",
            containerName(updated.id)
        ])
        guard result.status == 0 else {
            let detail = (result.stderr.isEmpty ? result.stdout : result.stderr)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            return ["resourcesApplied": false, "resourcesDetail": String(detail.prefix(400))]
        }
        value = try configuration(for: updated)
        value.realizedCPUs = updated.cpus
        value.realizedMemoryInMB = updated.memoryInMB
        try saveConfiguration(value, for: updated)
        return ["resourcesApplied": true]
    }

    private func validateDockerfile(_ contents: String) throws {
        guard !contents.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw SafeSpaceManagerError.commandFailed("The Dockerfile cannot be empty.")
        }
        guard contents.utf8.count <= 2 * 1024 * 1024 else {
            throw SafeSpaceManagerError.commandFailed("The Dockerfile is too large to edit here.")
        }
    }

    private func configurationMounts(_ value: Any?) throws -> [SafeSpaceMount] {
        guard let dictionaries = value as? [[String: Any]] else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var mounts: [SafeSpaceMount] = []
        var guestPaths = Set<String>()
        for dictionary in dictionaries {
            guard let identifier = dictionary["id"] as? String,
                  let id = UUID(uuidString: identifier),
                  let name = dictionary["name"] as? String,
                  let hostPath = dictionary["hostPath"] as? String,
                  let guestPath = dictionary["guestPath"] as? String,
                  let isReadOnly = dictionary["isReadOnly"] as? Bool else {
                throw SafeSpaceManagerError.invalidRequest
            }
            let hostURL = URL(fileURLWithPath: hostPath, isDirectory: true)
                .standardizedFileURL
            if !isReadOnly {
                try requireUnprotectedPath(hostURL.path, description: "A writable folder mount")
            }
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: hostURL.path,
                                                 isDirectory: &isDirectory),
                  isDirectory.boolValue,
                  guestPath.hasPrefix("/"),
                  !guestPath.contains(":"),
                  guestPath != "/var/lib/outershell/project",
                  guestPaths.insert(guestPath).inserted else {
                throw SafeSpaceManagerError.commandFailed(
                    "Each mounted folder needs an existing local folder and a unique absolute container path."
                )
            }
            mounts.append(SafeSpaceMount(id: id,
                                         name: name,
                                         hostPath: hostURL.path,
                                         guestPath: guestPath,
                                         isReadOnly: isReadOnly))
        }
        return mounts
    }

    private func configurationEnvironment(
        _ value: Any?
    ) throws -> [SafeSpaceEnvironmentVariable] {
        guard let dictionaries = value as? [[String: Any]] else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var names = Set<String>()
        return try dictionaries.map { dictionary in
            guard let name = dictionary["name"] as? String,
                  let value = dictionary["value"] as? String,
                  isValidEnvironmentName(name),
                  !value.contains("\0"),
                  names.insert(name).inserted else {
                throw SafeSpaceManagerError.commandFailed(
                    "Environment variable names must be unique shell identifiers."
                )
            }
            return SafeSpaceEnvironmentVariable(name: name, value: value)
        }
    }

    private func isValidEnvironmentName(_ name: String) -> Bool {
        guard let first = name.unicodeScalars.first,
              CharacterSet.letters.union(CharacterSet(charactersIn: "_"))
                .contains(first) else {
            return false
        }
        return name.unicodeScalars.dropFirst().allSatisfy {
            CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "_"))
                .contains($0)
        }
    }

    private func configurationPublishedPorts(
        _ value: Any?
    ) throws -> [SafeSpacePublishedPort] {
        guard let dictionaries = value as? [[String: Any]] else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var hostPorts = Set<Int>()
        return try dictionaries.map { dictionary in
            guard let hostPort = dictionary["hostPort"] as? Int,
                  let containerPort = dictionary["containerPort"] as? Int,
                  (1...65535).contains(hostPort),
                  (1...65535).contains(containerPort),
                  hostPorts.insert(hostPort).inserted else {
                throw SafeSpaceManagerError.commandFailed(
                    "Published ports need a unique host port and values from 1 through 65535."
                )
            }
            return SafeSpacePublishedPort(hostPort: hostPort,
                                          containerPort: containerPort)
        }
    }

    private func validatedBaseImage(_ rawBaseImage: String) throws -> String {
        let baseImage = rawBaseImage.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !baseImage.isEmpty,
              baseImage.count <= 512,
              !baseImage.hasPrefix("-"),
              baseImage.unicodeScalars.allSatisfy({
                  !CharacterSet.whitespacesAndNewlines.contains($0) &&
                      !CharacterSet.controlCharacters.contains($0)
              }) else {
            throw SafeSpaceManagerError.commandFailed(
                "Enter one OCI image reference without spaces, such as debian:bookworm."
            )
        }
        return baseImage
    }

    private func beginRebuild(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        let value = try configuration(for: selected)
        let isAlreadyBuilding = lock.withSafeSpaceLock {
            guard let progress = buildProgress[selected.id] else { return false }
            return progress.phase != "complete" && progress.phase != "failed"
        }
        guard !isAlreadyBuilding else {
            throw SafeSpaceManagerError.commandFailed("The container is already rebuilding.")
        }
        setTransientState("rebuilding", for: selected.id)
        setBuildProgress(
            SafeSpaceBuildProgress(
                phase: "preparing",
                detail: "Preparing the container build"
            ),
            for: selected.id
        )
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                _ = try await self.rebuild(selected, configuration: value)
                self.finishBuild(
                    phase: "complete",
                    detail: "Container rebuilt. Apps are ready to use.",
                    for: selected.id
                )
            } catch {
                self.finishBuild(
                    phase: "failed",
                    detail: error.localizedDescription,
                    for: selected.id
                )
            }
        }
        return [:]
    }

    private func rebuild(_ selected: SafeSpaceRecord,
                         configuration value: SafeSpaceConfiguration) async throws -> [String: Any] {
        try saveConfiguration(value, for: selected)
        let imageReference = containerImageReference(selected.id)
        let result = try runContainerBuild(selected, imageReference: imageReference)
        try requireSuccess(result, action: "build the container image")

        updateBuildProgressPhase(
            "replacing",
            detail: "Replacing the previous container",
            for: selected.id
        )
        closePublishedForwards(selected.id)
        try removeRuntimeContainerForReplacement(selected)
        updateBuildProgressPhase(
            "starting",
            detail: "Starting the rebuilt container",
            for: selected.id
        )
        try createRuntimeContainer(selected, imageReference: imageReference)
        _ = lock.withSafeSpaceLock {
            cachedCommandIcons.removeValue(forKey: selected.id)
        }
        updateBuildProgressPhase(
            "finishing",
            detail: "Finishing the rebuild",
            for: selected.id
        )
        try markRealized(for: selected)
        return [:]
    }

    private func removeRuntimeContainerForReplacement(_ record: SafeSpaceRecord) throws {
        let name = containerName(record.id)
        let state = try runtimeState(record)
        guard state != "absent" else { return }

        if state != "stopped" {
            let stop = try runRuntime(record, ["stop", "--time", "15", name])
            if stop.status != 0 {
                let currentState = try runtimeState(record)
                if currentState != "stopped" {
                    let forcedDeletion = try removeRuntimeContainer(record, force: true)
                    try requireSuccess(forcedDeletion,
                                       action: "replace the unresponsive container")
                    return
                }
            }
        }

        let deletion = try removeRuntimeContainer(record, force: false)
        if deletion.status != 0 &&
            !deletion.stderr.localizedCaseInsensitiveContains("not found") {
            try requireSuccess(deletion, action: "replace the container")
        }
    }

    private func publishSocket(in record: SafeSpaceRecord,
                               socketPath: String) async throws -> String {
        let normalized = URL(fileURLWithPath: socketPath).standardizedFileURL.path
        guard normalized.hasPrefix("/run/user/") else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let key = "\(record.id.uuidString.lowercased())\n\(normalized)"
        let publication = lock.withSafeSpaceLock { () -> Task<String, Error> in
            if let existing = socketPublications[key] { return existing }
            let task = Task { [self] in
                defer { lock.withSafeSpaceLock { socketPublications[key] = nil } }
                return try await createPublishedSocket(in: record, socketPath: normalized, key: key)
            }
            socketPublications[key] = task
            return task
        }
        return try await publication.value
    }

    private func createPublishedSocket(in record: SafeSpaceRecord,
                                       socketPath normalized: String,
                                       key: String) async throws -> String {
        try ensureContainerSocketBridge(record)
        let socketUser = try socketOwner(in: record, socketPath: normalized) == 0 ? "root" : "workspace"
        if let existing = lock.withSafeSpaceLock({ publishedForwards[key] }),
           !existing.isClosed,
           let path = existing.publishedSocketPath {
            return path
        }
        try ensureSafeSpaceSocketAllowed(record,
                                         socketPath: normalized,
                                         user: socketUser)
        let published = try publishedSocketPath(record.id, socketPath: normalized)
        let forward = try await SafeSpaceSocketRelayForward.publishWorkspaceSocket(
            containerName: containerName(record.id),
            socketPath: normalized,
            runtimeExecutablePath: try requiredRuntimeExecutablePath(for: record),
            user: socketUser,
            at: published
        )
        do {
            try authorizePublishedSocketPath(published)
        } catch {
            forward.close()
            throw error
        }
        lock.withSafeSpaceLock {
            publishedForwards[key]?.close()
            publishedForwards[key] = forward
        }
        return published
    }

    private func ensureContainerSocketBridge(_ record: SafeSpaceRecord) throws {
        let installed = try runRuntime(record, [
            "exec", "--user", "root", containerName(record.id),
            "/bin/sh", "-c",
            """
            if [ ! -x /usr/local/bin/outershell-socket-bridge ]; then exit 1; fi
            /usr/local/bin/outershell-socket-bridge >/dev/null 2>&1
            status=$?
            [ "$status" -ne 126 ] && [ "$status" -ne 127 ]
            """
        ])
        guard installed.status != 0 else { return }

        let platform = try runRuntime(record, [
            "exec", "--user", "root", containerName(record.id),
            "/bin/sh", "-c",
            """
            architecture=$(/bin/uname -m)
            case "${architecture}" in
                aarch64|arm64) architecture=aarch64 ;;
                x86_64|amd64) architecture=x86_64 ;;
                *) exit 1 ;;
            esac
            /usr/bin/printf '%s' "${architecture}"
            """
        ])
        try requireSuccess(platform, action: "identify the container platform")
        let platformName = platform.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !platformName.isEmpty,
              !platformName.contains(".."),
              let source = containerSocketBridgeURL(platform: platformName) else {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell does not include a socket bridge for this container platform."
            )
        }

        let temporaryPath = "/tmp/outershell-socket-bridge"
        let copy = try runRuntime(record, [
            "cp", source.path, "\(containerName(record.id)):\(temporaryPath)"
        ])
        try requireSuccess(copy, action: "copy the container socket bridge")
        let install = try runRuntime(record, [
            "exec", "--user", "root", containerName(record.id),
            "/bin/sh", "-c",
            "/bin/mkdir -p /usr/local/bin && /bin/cp \(temporaryPath) /usr/local/bin/outershell-socket-bridge && /bin/chmod 0755 /usr/local/bin/outershell-socket-bridge && /bin/rm -f \(temporaryPath)"
        ])
        try requireSuccess(install, action: "install the container socket bridge")
    }

    private func containerSocketBridgeURL(platform: String) -> URL? {
        let candidates = [
            Bundle.main.resourceURL?
                .appendingPathComponent("container-bootstrap", isDirectory: true)
                .appendingPathComponent("bin", isDirectory: true)
                .appendingPathComponent(platform, isDirectory: true)
                .appendingPathComponent("outershell-socket-bridge"),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath,
                isDirectory: true)
                .appendingPathComponent("build/run/container-bootstrap", isDirectory: true)
                .appendingPathComponent("bin", isDirectory: true)
                .appendingPathComponent(platform, isDirectory: true)
                .appendingPathComponent("outershell-socket-bridge")
        ].compactMap { $0 }
        return candidates.first {
            FileManager.default.isExecutableFile(atPath: $0.path)
        }
    }

    private func ensureSafeSpaceSocketAllowed(_ record: SafeSpaceRecord,
                                              socketPath: String,
                                              user: String) throws {
        guard user == "root" || user == "workspace" else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let name = URL(fileURLWithPath: socketPath).lastPathComponent
        guard !name.isEmpty, !name.contains("\n"), !name.contains("\r") else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let directory = user == "root"
            ? "/root/.config/outerssh"
            : "/home/workspace/.config/outerssh"
        let entry = "%t/\(name)"
        let directoryMode = "0700"
        let command = """
        set -eu
        directory=\(directory)
        allowlist="${directory}/http-unix.allow"
        /bin/mkdir -p "${directory}"
        /bin/chmod \(directoryMode) "${directory}"
        /usr/bin/touch "${allowlist}"
        /bin/chmod 0600 "${allowlist}"
        if ! /bin/grep -Fx -- '\(shellQuoted(entry))' "${allowlist}" >/dev/null 2>&1; then
            /usr/bin/printf '%s\n' '\(shellQuoted(entry))' >>"${allowlist}"
        fi
        """
        let result = try runRuntime(record, [
            "exec", "--user", user, containerName(record.id),
            "/bin/sh", "-c", command
        ])
        try requireSuccess(result, action: "authorize the container app socket")
    }

    private func socketOwner(in record: SafeSpaceRecord, socketPath: String) throws -> UInt32 {
        let exists = try runRuntime(record, [
            "exec", "--user", "root",
            containerName(record.id),
            "/usr/bin/test", "-S", socketPath
        ])
        guard exists.status == 0 else {
            throw SafeSpaceManagerError.commandFailed("The app is still starting. Try again in a moment.")
        }
        let result = try runRuntime(record, [
            "exec", "--user", "root",
            containerName(record.id),
            "/usr/bin/stat", "-c", "%u", "--", socketPath
        ])
        try requireSuccess(result, action: "inspect the container socket")
        guard let owner = UInt32(result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)) else {
            throw SafeSpaceManagerError.commandFailed("The container socket owner is invalid.")
        }
        return owner
    }

    private func appSnapshots(for record: SafeSpaceRecord) async throws -> [SafeSpaceAppSnapshot] {
        let identity = try runtimeIdentity(for: record)
        let runtimeDirectory = identity.runtimeDirectory
        let runningServiceIDs = try await runningServiceIDs(for: record)
        let command = """
        \(identity.outerctlPath) app list
        """
        let result = try runRuntime(record, [
            "exec", "--user", identity.user,
            containerName(record.id),
            "/usr/bin/env", "-i",
            "HOME=\(identity.homeDirectory)",
            "USER=\(identity.user)",
            "LOGNAME=\(identity.user)",
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "XDG_RUNTIME_DIR=\(runtimeDirectory)",
            "OUTERSHELLD_API_SOCKET=\(runtimeDirectory)/outershelld-api",
            "/bin/sh", "-c", command
        ])
        try requireSuccess(result, action: "list container apps")
        let declaredPorts = try declaredAppTCPPorts(for: record)
        let publishedPorts = Dictionary(uniqueKeysWithValues: (try configuration(for: record).publishedPorts ?? []).map {
            ($0.containerPort, $0.hostPort)
        })
        return try parseAppSnapshots(result.stdout,
                                     runtimeDirectory: runtimeDirectory,
                                     runningServiceIDs: runningServiceIDs,
                                     declaredPorts: declaredPorts,
                                     publishedPorts: publishedPorts)
    }

    private func declaredAppTCPPorts(for record: SafeSpaceRecord) throws -> [String: Int] {
        let command = """
        for path in /opt/outershell/image-apps/*/start; do
            [ -f "${path}" ] || continue
            service=${path%/start}
            service=${service##*/}
            port=$(/usr/bin/awk -F= '$1 == "port" && $2 ~ /^[0-9]+$/ { print $2; exit }' "${path}")
            [ -n "${port}" ] && /usr/bin/printf '%s\t%s\n' "${service}" "${port}"
        done
        """
        let result = try runRuntime(record, [
            "exec", "--user", "root", containerName(record.id),
            "/bin/sh", "-c", command
        ])
        try requireSuccess(result, action: "inspect container app ports")
        return Dictionary(uniqueKeysWithValues: result.stdout
            .split(separator: "\n")
            .compactMap { line -> (String, Int)? in
                let fields = line.split(separator: "\t", omittingEmptySubsequences: false)
                guard fields.count == 2, let port = Int(fields[1]), (1...65_535).contains(port) else {
                    return nil
                }
                return (String(fields[0]), port)
            })
    }

    private func runningServiceIDs(for record: SafeSpaceRecord) async throws -> Set<String> {
        let response = try await safeSpaceUIAPIRequest(
            record: record,
            route: 1,
            query: "",
            body: Data()
        )
        guard response.status == 200, response.body.count >= 12 else {
            throw SafeSpaceManagerError.commandFailed(
                "The container service manager returned an invalid app-state response."
            )
        }
        let count = Int(response.body.safeSpaceUInt32(at: 8))
        guard count >= 0, count <= 100_000, 12 + count * 8 <= response.body.count else {
            throw SafeSpaceManagerError.commandFailed(
                "The container service manager returned an invalid app count."
            )
        }
        var result: Set<String> = []
        for index in 0..<count {
            let backend = try response.body.safeSpaceReferencedData(at: 12 + index * 8)
            guard backend.count >= 48 else {
                throw SafeSpaceManagerError.commandFailed(
                    "The container service manager returned an invalid app record."
                )
            }
            let status = try backend.safeSpaceReferencedString(at: 40)
            if status == "running" {
                result.insert(try backend.safeSpaceReferencedString(at: 0))
            }
        }
        return result
    }

    private func publishExternalAppEndpoints(
        _ snapshots: [SafeSpaceAppSnapshot],
        for record: SafeSpaceRecord
    ) async -> [SafeSpaceAppSnapshot] {
        let prefix = "\(record.id.uuidString.lowercased())\n"
        let activeKeys = Set(snapshots.filter { $0.isRunning && $0.publishedPort == nil }
            .map { prefix + $0.socketPath })
        lock.withSafeSpaceLock {
            endpointPublicationFailures.retain(activeKeys, prefix: prefix)
        }
        var result: [SafeSpaceAppSnapshot] = []
        result.reserveCapacity(snapshots.count)
        for snapshot in snapshots {
            if !snapshot.isRunning || snapshot.publishedPort != nil {
                result.append(snapshot)
                continue
            }
            do {
                let publishedPath = try await publishSocket(
                    in: record,
                    socketPath: snapshot.socketPath
                )
                let path = appPathAndQuery(snapshot.url, socketPath: snapshot.socketPath)
                let externalURL = "http+unix://\(formEncoded(publishedPath))\(path)"
                if snapshot.externalSocketPath != publishedPath {
                    try updateExternalAppEndpoint(snapshot,
                                                  externalSocketPath: publishedPath,
                                                  in: record)
                }
                let recovered = lock.withSafeSpaceLock {
                    endpointPublicationFailures.recover(prefix + snapshot.socketPath)
                }
                if recovered {
                    NSLog("Published app endpoint %@ in container %@ after recovery",
                          snapshot.serviceID, record.name)
                }
                result.append(SafeSpaceAppSnapshot(
                    frontendID: snapshot.frontendID,
                    serviceID: snapshot.serviceID,
                    displayName: snapshot.displayName,
                    socketPath: snapshot.socketPath,
                    externalSocketPath: publishedPath,
                    url: externalURL,
                    iconPath: snapshot.iconPath,
                    isRunning: snapshot.isRunning,
                    publishedPort: snapshot.publishedPort
                ))
            } catch {
                let shouldReport = lock.withSafeSpaceLock {
                    endpointPublicationFailures.record(error.localizedDescription,
                                                       for: prefix + snapshot.socketPath)
                }
                if shouldReport {
                    NSLog("Could not publish app endpoint %@ in container %@: %@",
                          snapshot.serviceID,
                          record.name,
                          error.localizedDescription)
                }
                result.append(SafeSpaceAppSnapshot(
                    frontendID: snapshot.frontendID,
                    serviceID: snapshot.serviceID,
                    displayName: snapshot.displayName,
                    socketPath: snapshot.socketPath,
                    externalSocketPath: "",
                    url: snapshot.url,
                    iconPath: snapshot.iconPath,
                    isRunning: snapshot.isRunning,
                    publishedPort: snapshot.publishedPort
                ))
            }
        }
        return result
    }

    private func updateExternalAppEndpoint(_ snapshot: SafeSpaceAppSnapshot,
                                           externalSocketPath: String,
                                           in record: SafeSpaceRecord) throws {
        let identity = try runtimeIdentity(for: record)
        var arguments = [
            "exec", "--user", identity.user, containerName(record.id),
            "/usr/bin/env",
            "HOME=\(identity.homeDirectory)",
            "USER=\(identity.user)",
            "LOGNAME=\(identity.user)",
            "XDG_RUNTIME_DIR=\(identity.runtimeDirectory)",
            "OUTERSHELLD_API_SOCKET=\(identity.runtimeDirectory)/outershelld-api",
            identity.outerctlPath,
            "app", "upsert",
            "--backend", snapshot.serviceID,
            "--frontend-id", snapshot.frontendID,
            "--socket-path", snapshot.socketPath,
            "--name", snapshot.displayName,
            "--url", snapshot.url,
            "--external-socket-path", externalSocketPath
        ]
        if !snapshot.iconPath.isEmpty {
            arguments.append(contentsOf: ["--icon-path", snapshot.iconPath])
        }
        let update = try runRuntime(record, arguments)
        try requireSuccess(update, action: "publish the container app endpoint")
    }

    private func appPathAndQuery(_ rawURL: String, socketPath: String) -> String {
        let trimmed = rawURL.trimmingCharacters(in: .whitespacesAndNewlines)
        func normalized(_ value: String) -> String {
            guard !value.isEmpty else { return "/" }
            if value.first == "/" { return value }
            return "/" + value
        }
        if trimmed == socketPath { return "/" }
        if trimmed.hasPrefix(socketPath) {
            return normalized(String(trimmed.dropFirst(socketPath.count)))
        }
        if let components = URLComponents(string: trimmed), components.scheme != nil {
            var value = components.path.isEmpty ? "/" : components.path
            if let query = components.query, !query.isEmpty {
                value += "?\(query)"
            }
            return normalized(value)
        }
        return normalized(trimmed)
    }

    private func ensureAppEventMonitor(for record: SafeSpaceRecord) {
        let monitor = SafeSpaceAppEventMonitor()
        let shouldStart = lock.withSafeSpaceLock { () -> Bool in
            guard appEventMonitors[record.id] == nil else { return false }
            appEventMonitors[record.id] = monitor
            return true
        }
        guard shouldStart else { return }
        let task = Task.detached(priority: .utility) { [weak self] in
            guard let self else { return }
            await self.monitorAppEvents(for: record, token: monitor.token)
        }
        monitor.setTask(task)
    }

    private func cancelAppEventMonitor(for safeSpaceID: UUID) {
        let monitor = lock.withSafeSpaceLock {
            appEventMonitors.removeValue(forKey: safeSpaceID)
        }
        monitor?.cancel()
    }

    private func monitorAppEvents(for record: SafeSpaceRecord,
                                  token: UUID) async {
        defer {
            lock.withSafeSpaceLock {
                if appEventMonitors[record.id]?.token == token {
                    appEventMonitors.removeValue(forKey: record.id)
                }
            }
        }
        var version: UInt64 = 0
        while !Task.isCancelled {
            do {
                let response = try await safeSpaceUIAPIRequest(
                    record: record,
                    route: 7,
                    query: "sinceBackends=\(version)&sinceLog=0",
                    body: Data(),
                    timeoutSeconds: 35
                )
                guard response.status == 200,
                      response.body.count >= 24 else {
                    throw SafeSpaceManagerError.commandFailed(
                        "The container app event response was invalid."
                    )
                }
                let flags = response.body.safeSpaceUInt32(at: 0)
                version = response.body.safeSpaceUInt64(at: 8)
                guard flags & 0x01 != 0 else { continue }
                let snapshots = try await appSnapshots(for: record)
                updateCachedApps(snapshots, for: record.id)
                try saveCachedApps()
                scheduleDeclaredAppIconFetches(for: record)
                notifyOuterShellSafeSpacesChanged()
            } catch {
                if !Task.isCancelled {
                    NSLog("Container app event watch ended: %@", error.localizedDescription)
                }
                return
            }
        }
    }

    private func parseAppSnapshots(_ output: String,
                                   runtimeDirectory: String,
                                   runningServiceIDs: Set<String>,
                                   declaredPorts: [String: Int],
                                   publishedPorts: [Int: Int]) throws -> [SafeSpaceAppSnapshot] {
        let lines = output.components(separatedBy: .newlines)
        let appLines = lines.filter { !$0.isEmpty }
        guard let headerLine = appLines.first else {
            return []
        }
        let headers = headerLine.split(separator: "\t",
                                       omittingEmptySubsequences: false).map(String.init)
        let indexes = Dictionary(uniqueKeysWithValues: headers.enumerated().map {
            ($0.element, $0.offset)
        })
        return appLines.dropFirst().compactMap { line in
            let fields = line.split(separator: "\t",
                                    omittingEmptySubsequences: false).map(String.init)
            func field(_ name: String) -> String {
                guard let index = indexes[name], fields.indices.contains(index) else {
                    return ""
                }
                return fields[index]
            }
            let socketPath = field("socket_path")
            guard socketPath.hasPrefix("\(runtimeDirectory)/") else {
                return nil
            }
            let serviceID = field("service_id")
            let publishedPort = declaredPorts[serviceID].flatMap { publishedPorts[$0] }
            return SafeSpaceAppSnapshot(
                frontendID: field("frontend_id"),
                serviceID: serviceID,
                displayName: field("display_name"),
                socketPath: socketPath,
                externalSocketPath: field("external_socket_path"),
                url: field("url"),
                iconPath: field("icon_path"),
                isRunning: runningServiceIDs.contains(serviceID),
                publishedPort: publishedPort
            )
        }
    }

    private func updateCachedApps(_ snapshots: [SafeSpaceAppSnapshot],
                                  for safeSpaceID: UUID) {
        lock.withSafeSpaceLock {
            let old = Dictionary(uniqueKeysWithValues: (cachedApps[safeSpaceID] ?? []).map {
                ($0.frontendID, $0)
            })
            cachedApps[safeSpaceID] = snapshots.map { snapshot in
                let previous = old[snapshot.frontendID]
                return SafeSpaceCachedApp(frontendID: snapshot.frontendID,
                                          serviceID: snapshot.serviceID,
                                          displayName: snapshot.displayName,
                                          socketPath: snapshot.socketPath,
                                          externalSocketPath: snapshot.externalSocketPath,
                                          url: snapshot.url,
                                          iconPath: snapshot.iconPath,
                                          iconData: previous?.iconPath == snapshot.iconPath ? previous?.iconData : nil,
                                          isRunning: snapshot.isRunning,
                                          publishedPort: snapshot.publishedPort)
            }
        }
    }

    private func scheduleDeclaredAppIconFetches(for record: SafeSpaceRecord) {
        let candidates = lock.withSafeSpaceLock { () -> [SafeSpaceCachedApp] in
            var values: [SafeSpaceCachedApp] = []
            for app in cachedApps[record.id] ?? [] {
                let key = "\(record.id.uuidString.lowercased())\n\(app.frontendID)"
                guard app.iconData == nil,
                      !app.iconPath.isEmpty,
                      !appIconFetchesInProgress.contains(key) else {
                    continue
                }
                appIconFetchesInProgress.insert(key)
                values.append(app)
            }
            return values
        }
        guard !candidates.isEmpty else {
            return
        }
        Task.detached(priority: .utility) { [weak self] in
            guard let self else { return }
            var discovered: [String: Data] = [:]
            for app in candidates {
                do {
                    if let data = try await self.declaredAppIconData(app, in: record) {
                        discovered[app.frontendID] = data
                    }
                } catch {
                    NSLog("Container app icon load failed for %@: %@",
                          app.serviceID,
                          error.localizedDescription)
                }
            }
            let keys = candidates.map {
                "\(record.id.uuidString.lowercased())\n\($0.frontendID)"
            }
            self.lock.withSafeSpaceLock {
                self.appIconFetchesInProgress.subtract(keys)
                guard var apps = self.cachedApps[record.id] else {
                    return
                }
                for index in apps.indices {
                    if let data = discovered[apps[index].frontendID] {
                        apps[index].iconData = data
                    }
                }
                self.cachedApps[record.id] = apps
            }
            if !discovered.isEmpty {
                do {
                    try self.saveCachedApps()
                } catch {
                    NSLog("Container app icon save failed: %@", error.localizedDescription)
                }
            }
        }
    }

    private func declaredAppIconData(_ app: SafeSpaceCachedApp,
                                     in record: SafeSpaceRecord) async throws -> Data? {
        guard app.iconPath.hasPrefix("/") else {
            return nil
        }
        let path = URL(fileURLWithPath: app.iconPath).standardizedFileURL.path
        let command = """
        path=\(shellQuoted(path))
        if [ -f "${path}" ]; then
            size="$(/usr/bin/stat -c '%s' "${path}" 2>/dev/null || true)"
            if [ -n "${size}" ] && [ "${size}" -le 524288 ]; then
                /usr/bin/base64 -w 0 "${path}"
            fi
        fi
        """
        let result = try runRuntime(record, [
            "exec", "--user", try runtimeIdentity(for: record).user,
            containerName(record.id), "/bin/sh", "-c", command
        ])
        try requireSuccess(result, action: "read the container app icon")
        let encoded = result.stdout
        guard let data = Data(
            base64Encoded: encoded.trimmingCharacters(in: .whitespacesAndNewlines)
        ),
        !data.isEmpty,
        data.count <= 512 * 1024,
        let image = NSBitmapImageRep(data: data),
        image.pixelsWide > 0,
        image.pixelsHigh > 0,
        image.pixelsWide <= 4096,
        image.pixelsHigh <= 4096 else {
            return nil
        }
        return data
    }

    private func appDictionary(_ app: SafeSpaceCachedApp,
                               running: Bool,
                               containerID: UUID) -> [String: Any] {
        var value: [String: Any] = [
            "frontendID": app.frontendID,
            "serviceID": app.serviceID,
            "displayName": app.displayName,
            "socketPath": app.socketPath,
            "externalSocketPath": app.externalSocketPath ?? "",
            "url": app.url,
            "iconPath": app.iconPath,
            "isRunning": running && (app.isRunning ?? false),
            "publishedPort": app.publishedPort ?? 0
        ]
        if let iconData = app.iconData ?? endpointIconData(for: app, containerID: containerID) {
            value["iconData"] = iconData
        }
        return value
    }

    /// The icon Outer Loop cached on this Mac for the app's host endpoint, in
    /// OuterSSH's per-user endpoint icon cache. See endpoint-icons.md. A stopped
    /// app has no published socket yet, so its relay path is computed.
    private func endpointIconData(for app: SafeSpaceCachedApp, containerID: UUID) -> Data? {
        let line: String
        let published = app.externalSocketPath.flatMap { $0.isEmpty ? nil : $0 }
            ?? (app.socketPath.hasPrefix("/") ? relaySocketPath(containerID, socketPath: app.socketPath) : nil)
        if let port = app.publishedPort, port > 0, port <= 65535 {
            line = "tcp localhost \(port)"
        } else if let socket = published, socket.hasPrefix("/"), !socket.contains("\n") {
            line = "unix \(socket)"
        } else {
            return nil
        }
        let name = SHA256.hash(data: Data(line.utf8)).map { String(format: "%02x", $0) }.joined() + ".png"
        let directory = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/org.outerssh/endpoint-icons", isDirectory: true)
        let path = directory.appendingPathComponent(name).path
        guard let directoryAttributes = try? FileManager.default.attributesOfItem(atPath: directory.path),
              let attributes = try? FileManager.default.attributesOfItem(atPath: path),
              directoryAttributes[.type] as? FileAttributeType == .typeDirectory,
              attributes[.type] as? FileAttributeType == .typeRegular,
              [directoryAttributes, attributes].allSatisfy({
                  ($0[.ownerAccountID] as? NSNumber)?.uint32Value == getuid() &&
                      (($0[.posixPermissions] as? NSNumber)?.intValue ?? 0o777) & 0o022 == 0
              }),
              let size = (attributes[.size] as? NSNumber)?.intValue,
              size > 8, size <= 64 * 1024,
              let data = FileManager.default.contents(atPath: path),
              data.starts(with: [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]) else {
            return nil
        }
        return data
    }

    private func runtimeState(_ record: SafeSpaceRecord, recoverRuntime: Bool = true) throws -> String {
        switch runtimeProvider(for: record) {
        case .appleContainer:
            let arguments = ["list", "--all", "--format", "json"]
            let result: (status: Int32, stdout: String, stderr: String)
            if recoverRuntime {
                result = try runContainer(arguments)
            } else {
                guard let executable = runtimeExecutableURL(for: .appleContainer) else {
                    throw SafeSpaceManagerError.unsupportedProvider
                }
                result = try runCommand(executable: executable.path, arguments: arguments)
            }
            try requireSuccess(result, action: "inspect containers")
            guard let data = result.stdout.data(using: .utf8),
                  let values = try JSONSerialization.jsonObject(with: data) as? [[String: Any]]
            else {
                throw SafeSpaceManagerError.commandFailed(
                    "Apple container returned invalid container state."
                )
            }
            guard let value = values.first(where: {
                $0["id"] as? String == containerName(record.id)
            }) else {
                return "absent"
            }
            let status = value["status"] as? [String: Any]
            return status?["state"] as? String ?? "stopped"
        case .docker:
            let result = try runDocker([
                "container", "inspect", "--format", "{{.State.Status}}",
                containerName(record.id)
            ])
            if result.status != 0 && dockerObjectIsMissing(result) {
                return "absent"
            }
            try requireSuccess(result, action: "inspect the Docker container")
            switch result.stdout.trimmingCharacters(in: .whitespacesAndNewlines) {
            case "running":
                return "running"
            case "created", "exited", "dead", "paused", "restarting":
                return "stopped"
            default:
                return "stopped"
            }
        }
    }

    private func record(from request: [String: Any]) throws -> SafeSpaceRecord {
        guard let text = request["workspaceID"] as? String,
              let id = UUID(uuidString: text),
              let selected = lock.withSafeSpaceLock({
                  records.first(where: { $0.id == id })
              }) else {
            throw SafeSpaceManagerError.safeSpaceNotFound
        }
        return selected
    }

    private func loadRecords() throws -> [SafeSpaceRecord] {
        var pointer: UnsafeMutablePointer<OuterShellRegistryContainerRecord>?
        var count = 0
        var error = [CChar](repeating: 0, count: 2048)
        guard outerShellRegistryCopyContainers(&pointer, &count, &error, error.count) != 0 else {
            throw SafeSpaceManagerError.commandFailed(
                String(decoding: error.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
            )
        }
        defer { outerShellRegistryFreeContainers(pointer, count) }
        if count == 0,
           let legacyData = try RegistryResourceStore.data(for: "containers/records") {
            var migrated = try OffsetArchive.decode([SafeSpaceRecord].self, from: legacyData)
            for index in migrated.indices {
                migrated[index].runtimeName = managedContainerName(migrated[index].id)
                migrated[index].ownsContainer = true
                migrated[index].ownsBuildContext = true
                migrated[index].selfModifying = true
                try saveRecord(migrated[index])
            }
            try RegistryResourceStore.remove("containers/records")
            return migrated
        }
        guard let pointer else { return [] }
        return (0..<count).compactMap { index in
            let value = pointer[index]
            guard let identifier = value.identifier,
                  let id = UUID(uuidString: String(cString: identifier)) else {
                return nil
            }
            return SafeSpaceRecord(
                id: id,
                name: value.displayName.map { String(cString: $0) } ?? id.uuidString,
                createdAt: Date(timeIntervalSince1970:
                    TimeInterval(value.createdAtMilliseconds) / 1000),
                cpus: max(Int(value.cpus), 1),
                memoryInMB: max(Int(value.memoryInMB), 1),
                runtimeProviderID: value.providerID.map { String(cString: $0) },
                runtimeName: value.runtimeName.map { String(cString: $0) },
                ownsContainer: value.flags & outerShellRegistryContainerOwnsRuntimeFlag != 0,
                buildContextPath: value.buildContextPath.map { String(cString: $0) },
                ownsBuildContext: value.flags & outerShellRegistryContainerOwnsBuildContextFlag != 0,
                selfModifying: value.flags & outerShellRegistryContainerSelfModifyingFlag != 0
            )
        }
    }

    private func loadCachedApps() throws -> [UUID: [SafeSpaceCachedApp]] {
        guard let data = try RegistryResourceStore.data(for: "containers/apps") else {
            return [:]
        }
        let decoded = try OffsetArchive.decode(
            [String: [SafeSpaceCachedApp]].self,
            from: data
        )
        return Dictionary(uniqueKeysWithValues: decoded.compactMap { key, value in
            UUID(uuidString: key).map { ($0, value) }
        })
    }

    private func saveRecords() throws {
        let values = lock.withSafeSpaceLock { records }
        for value in values {
            try saveRecord(value)
        }
    }

    private func refreshRecords() throws {
        let values = try loadRecords()
        lock.withSafeSpaceLock {
            records = values
        }
    }

    private func removeRecordRegistration(_ id: UUID) throws {
        var error = [CChar](repeating: 0, count: 2048)
        let identifier = id.uuidString.lowercased()
        let removed = identifier.withCString {
            outerShellRegistryRemoveContainer($0, &error, error.count)
        }
        guard removed != 0 else {
            throw SafeSpaceManagerError.commandFailed(String(decoding: error.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self))
        }
    }

    private func saveRecord(_ value: SafeSpaceRecord) throws {
        let identifier = value.id.uuidString.lowercased()
        let providerID = value.runtimeProviderID ?? SafeSpaceRuntimeProviderID.appleContainer.rawValue
        let runtimeName = registeredRuntimeName(for: value)
        let buildContextPath = value.buildContextPath ?? ""
        var flags: UInt32 = 0
        if recordOwnsContainer(value) { flags |= outerShellRegistryContainerOwnsRuntimeFlag }
        if recordOwnsBuildContext(value) { flags |= outerShellRegistryContainerOwnsBuildContextFlag }
        if recordIsSelfModifying(value) { flags |= outerShellRegistryContainerSelfModifyingFlag }
        var error = [CChar](repeating: 0, count: 2048)
        let saved = identifier.withCString { identifierPointer in
            value.name.withCString { displayNamePointer in
                providerID.withCString { providerPointer in
                    runtimeName.withCString { runtimeNamePointer in
                        buildContextPath.withCString { buildContextPointer in
                            var record = OuterShellRegistryContainerRecord(
                                identifier: UnsafeMutablePointer(mutating: identifierPointer),
                                displayName: UnsafeMutablePointer(mutating: displayNamePointer),
                                providerID: UnsafeMutablePointer(mutating: providerPointer),
                                runtimeName: UnsafeMutablePointer(mutating: runtimeNamePointer),
                                buildContextPath: UnsafeMutablePointer(mutating: buildContextPointer),
                                createdAtMilliseconds: UInt64(max(
                                    value.createdAt.timeIntervalSince1970 * 1000,
                                    0
                                )),
                                cpus: UInt32(clamping: value.cpus),
                                memoryInMB: UInt32(clamping: value.memoryInMB),
                                flags: flags
                            )
                            return outerShellRegistryUpsertContainer(
                                &record,
                                &error,
                                error.count
                            )
                        }
                    }
                }
            }
        }
        guard saved != 0 else {
            throw SafeSpaceManagerError.commandFailed(String(decoding: error.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self))
        }
    }

    private func saveCachedApps() throws {
        let values = lock.withSafeSpaceLock {
            Dictionary(uniqueKeysWithValues: cachedApps.map {
                ($0.key.uuidString.lowercased(), $0.value)
            })
        }
        try RegistryResourceStore.set(
            OffsetArchive.encode(values),
            for: "containers/apps"
        )
    }

    private func configuration(for record: SafeSpaceRecord) throws -> SafeSpaceConfiguration {
        configurationLock.lock()
        defer { configurationLock.unlock() }
        if let data = try RegistryResourceStore.data(for: configurationResourceKey(record.id)) {
            var value = try OffsetArchive.decode(SafeSpaceConfiguration.self, from: data)
            var needsSave = false
            if value.version < currentSafeSpaceConfigurationVersion {
                value.version = currentSafeSpaceConfigurationVersion
                value.realizedDockerfileContents = nil
                needsSave = true
            }
            if FileManager.default.fileExists(atPath: try dockerfileURL(record.id).path),
               let baseImage = dockerfileBaseImage(
                   in: try dockerfileContents(for: record)
               ), value.baseImage != baseImage {
                value.baseImage = baseImage
                needsSave = true
            }
            if needsSave {
                try saveConfiguration(value, for: record)
            }
            return value
        }
        let value = newConfiguration(
            baseImage: defaultContainerBaseImage,
            hasUntrackedChanges: (try? runtimeState(record)) != "absent"
        )
        try saveConfiguration(value, for: record)
        return value
    }

    private func newConfiguration(baseImage: String,
                           hasUntrackedChanges: Bool) -> SafeSpaceConfiguration {
        SafeSpaceConfiguration(version: currentSafeSpaceConfigurationVersion,
                        baseImage: baseImage,
                        realizedBaseImage: nil,
                        hasUntrackedChanges: hasUntrackedChanges,
                        mounts: [],
                        realizedDockerfileContents: nil,
                        environment: [],
                        realizedMounts: nil,
                        realizedEnvironment: nil,
                        publishedPorts: [],
                        realizedPublishedPorts: nil,
                        persistentData: [])
    }

    private func saveConfiguration(_ value: SafeSpaceConfiguration,
                            for record: SafeSpaceRecord) throws {
        configurationLock.lock()
        defer { configurationLock.unlock() }
        let directory = try buildContextDirectory(record.id)
        let normalizedValue = normalizedConfiguration(value)
        if recordOwnsBuildContext(record) {
            try FileManager.default.createDirectory(at: directory,
                                                    withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
        }
        try RegistryResourceStore.set(
            OffsetArchive.encode(normalizedValue),
            for: configurationResourceKey(record.id)
        )
        let dockerfile = try dockerfileURL(record.id)
        if !FileManager.default.fileExists(atPath: dockerfile.path) {
            try generatedDockerfile(for: normalizedValue).write(
                to: dockerfile,
                atomically: true,
                encoding: .utf8
            )
        }
        if recordOwnsBuildContext(record) {
            try writeContainerProjectGuide(to: directory)
        }
        let legacyContainerfile = directory.appendingPathComponent("Containerfile")
        if FileManager.default.fileExists(atPath: legacyContainerfile.path) {
            try FileManager.default.removeItem(at: legacyContainerfile)
        }
    }

    private func normalizedConfiguration(_ value: SafeSpaceConfiguration) -> SafeSpaceConfiguration {
        var result = value
        if result.baseImage.hasPrefix(legacyContainerBaseImagePrefix) {
            result.baseImage = defaultContainerBaseImage
        }
        if result.environment == nil {
            result.environment = []
        }
        if result.persistentData == nil {
            result.persistentData = []
        }
        if result.publishedPorts == nil {
            result.publishedPorts = []
        }
        return result
    }

    private func markRealized(for record: SafeSpaceRecord) throws {
        var value = normalizedConfiguration(try configuration(for: record))
        value.version = currentSafeSpaceConfigurationVersion
        value.realizedBaseImage = value.baseImage
        value.realizedDockerfileContents = try dockerfileContents(for: record)
        value.realizedMounts = value.mounts ?? []
        value.realizedEnvironment = value.environment ?? []
        value.realizedPublishedPorts = value.publishedPorts ?? []
        value.realizedCPUs = record.cpus
        value.realizedMemoryInMB = record.memoryInMB
        value.realizedBuildContextPath = try buildContextDirectory(record.id).path
        value.realizedSelfModifying = recordIsSelfModifying(record)
        value.hasUntrackedChanges = false
        try saveConfiguration(value, for: record)
        let legacyDirectory = try legacyBuildContextDirectory(record.id)
        if (try? FileManager.default.destinationOfSymbolicLink(atPath: legacyDirectory.path)) != nil {
            try FileManager.default.removeItem(at: legacyDirectory)
        }
    }

    private func definitionDictionary(for record: SafeSpaceRecord) throws -> [String: Any] {
        let value = normalizedConfiguration(try configuration(for: record))
        let dockerfile = try dockerfileContents(for: record)
        let needsRebuild = value.version != currentSafeSpaceConfigurationVersion ||
            value.realizedDockerfileContents != dockerfile ||
            value.realizedMounts.map { $0 != (value.mounts ?? []) } == true ||
            value.realizedEnvironment.map { $0 != (value.environment ?? []) } == true ||
            value.realizedPublishedPorts.map { $0 != (value.publishedPorts ?? []) } == true ||
            value.realizedCPUs.map { $0 != record.cpus } == true ||
            value.realizedMemoryInMB.map { $0 != record.memoryInMB } == true ||
            value.realizedBuildContextPath.map { $0 != (try? buildContextDirectory(record.id).path) } == true ||
            value.realizedSelfModifying.map { $0 != recordIsSelfModifying(record) } == true
        return [
            "baseImage": value.baseImage,
            "containerfile": dockerfile,
            "dockerfileHostPath": try dockerfileURL(record.id).path,
            "dockerfileGuestPath": recordIsSelfModifying(record) ? "/var/lib/outershell/project/Dockerfile" : "",
            "buildEngineAvailable": runtimeExecutableURL(
                for: runtimeProvider(for: record)
            ) != nil,
            "definitionDirectory": try buildContextDirectory(record.id).path,
            "transferItems": transferItems(for: record, configuration: value),
            "needsRebuild": needsRebuild,
            "hasUntrackedChanges": value.hasUntrackedChanges ?? true,
            "environment": (value.environment ?? []).map {
                ["name": $0.name, "value": $0.value]
            },
            "publishedPorts": (value.publishedPorts ?? []).map {
                ["hostPort": $0.hostPort, "containerPort": $0.containerPort]
            }
        ]
    }

    private func transferItems(for record: SafeSpaceRecord,
                               configuration value: SafeSpaceConfiguration) -> [[String: Any]] {
        return [
            [
                "name": "Container definition",
                "detail": "Dockerfile and its build-context files",
                "status": "Included"
            ],
            [
                "name": "Mounted folders",
                "detail": "\((value.mounts ?? []).count) local folder mapping(s)",
                "status": (value.mounts ?? []).isEmpty ? "None" : "Reconnect at destination"
            ],
            [
                "name": "Secrets",
                "detail": "Credentials are intentionally excluded from the definition",
                "status": "Provide at destination"
            ],
            [
                "name": "OCI image",
                "detail": containerImageReference(record.id),
                "status": "Rebuild or push to a registry"
            ]
        ]
    }

    private func generatedDockerfile(for value: SafeSpaceConfiguration) -> String {
        "FROM \(normalizedConfiguration(value).baseImage)\n\(containerSupportLine)\n"
    }

    private func upgradedDockerfile(_ contents: String) -> String {
        var lines: [String] = []
        for line in contents.components(separatedBy: "\n") {
            let stripped = line.trimmingCharacters(in: .whitespacesAndNewlines)
            if stripped == legacyBootstrapCopyLine {
                lines.append(containerSupportLine)
                continue
            }
            if stripped == legacyBootstrapRunLine { continue }
            let fields = stripped.split(whereSeparator: { $0.isWhitespace })
            if fields.first?.uppercased() == "FROM",
               let image = fields.dropFirst().first(where: { !$0.hasPrefix("--") }),
               image.hasPrefix(legacyContainerBaseImagePrefix),
               let range = line.range(of: String(image)) {
                lines.append(line.replacingCharacters(in: range, with: defaultContainerBaseImage))
                lines.append(containerSupportLine)
                continue
            }
            lines.append(line)
        }
        return lines.joined(separator: "\n")
    }

    private func upgradeDefinition(for record: SafeSpaceRecord) throws {
        let url = try dockerfileURL(record.id)
        guard FileManager.default.fileExists(atPath: url.path) else { return }
        let contents = try readRegularTextFile(url)
        let upgraded = upgradedDockerfile(contents)
        if upgraded != contents {
            try upgraded.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    private func supportImageArchitecture(
        for provider: SafeSpaceRuntimeProviderID
    ) throws -> String {
        let architecture: String
        switch provider {
        case .appleContainer:
            architecture = cpuArchitecture()
        case .docker:
            let result = try runDocker(["info", "--format", "{{.Architecture}}"])
            try requireSuccess(result, action: "inspect the Docker engine architecture")
            architecture = result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return ["arm64": "aarch64", "amd64": "x86_64"][architecture] ?? architecture
    }

    private func ensureSupportImage(for provider: SafeSpaceRuntimeProviderID) throws {
        guard let resources = containerBootstrapPayloadURL() else {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell's container support resources are missing."
            )
        }
        let binaries = resources.appendingPathComponent("bin", isDirectory: true)
            .appendingPathComponent(try supportImageArchitecture(for: provider), isDirectory: true)
        let directory = try applicationDirectory()
            .appendingPathComponent("Build Contexts", isDirectory: true)
            .appendingPathComponent("outershell-support-\(UUID().uuidString)", isDirectory: true)
        let bin = directory.appendingPathComponent("bin", isDirectory: true)
        try FileManager.default.createDirectory(at: bin,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        var digestInput = Data()
        for name in ["outershelld", "outerctl", "outershell-socket-bridge"] {
            let destination = bin.appendingPathComponent(name)
            try FileManager.default.copyItem(at: binaries.appendingPathComponent(name), to: destination)
            digestInput.append(Data("bin/\(name)\0".utf8))
            digestInput.append(Data(SHA256.hash(data: try Data(contentsOf: destination))))
        }
        for name in ["Dockerfile", "outershell-container-init",
                     "share/OUTERCTL.md", "share/skills/outer-shell/SKILL.md"] {
            let destination = directory.appendingPathComponent(name)
            try FileManager.default.createDirectory(at: destination.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try FileManager.default.copyItem(at: resources.appendingPathComponent(name), to: destination)
            digestInput.append(Data("\(name)\0".utf8))
            digestInput.append(Data(SHA256.hash(data: try Data(contentsOf: destination))))
        }
        let fingerprint = SHA256.hash(data: digestInput).map { String(format: "%02x", $0) }.joined()
        let marker = try applicationDirectory()
            .appendingPathComponent("support-image-\(provider.rawValue).sha256")
        if (try? String(contentsOf: marker, encoding: .utf8)) == fingerprint,
           !(try containerImageReferences(provider: provider)).isDisjoint(with: containerSupportImageReferences) {
            return
        }
        let dockerfile = directory.appendingPathComponent("Dockerfile")
        try (try String(contentsOf: dockerfile, encoding: .utf8) +
             "LABEL org.outershell.support-context=\(fingerprint)\n")
            .write(to: dockerfile, atomically: true, encoding: .utf8)
        let arguments = [
            "build", "--file", "Dockerfile",
            "--tag", containerSupportImage, "--progress", "plain", "."
        ]
        let result: (status: Int32, stdout: String, stderr: String)
        switch provider {
        case .appleContainer:
            result = try runContainer(arguments, currentDirectoryURL: directory)
        case .docker:
            result = try runDocker(arguments, currentDirectoryURL: directory)
        }
        try requireSuccess(result, action: "prepare Outer Shell's container support image")
        try fingerprint.write(to: marker, atomically: true, encoding: .utf8)
    }

    private func writeContainerProjectGuide(to directory: URL) throws {
        let guide = """
        # Outer Shell container project

        This directory is an ordinary OCI build context. Its `Dockerfile` can be built with Apple `container`, Docker, Podman, or another compatible builder.

        ```sh
        container build --tag my-container .
        # or: docker build --tag my-container .
        ```

        The `FROM` instruction is your chosen base image. The line `COPY --from=outershell/support /outershell-rootfs/ /` adds Outer Shell's small in-container tools, which let apps announce themselves. Outer Shell builds the `outershell/support` image on this machine, so a build elsewhere needs that image too.

        Outer Shell treats `Dockerfile` as the source of truth. The same project directory is mounted at `/var/lib/outershell/project` while the container runs, so a root process in the container can edit the Dockerfile and its build-context files directly. Rebuilding recreates the image from those files.

        Mounted folders are runtime configuration. They intentionally do not appear in the Dockerfile or become part of the image. Recreate those mounts separately when moving the container to another host.

        Outer Shell's tools are static Linux binaries and need only `/bin/sh` and standard file utilities in the image. Software snippets may have narrower requirements, such as a Debian-family image.
        """
        try guide.write(
            to: directory.appendingPathComponent("README.md"),
            atomically: true,
            encoding: .utf8
        )
        let dockerIgnore = """
        README.md
        """
        try dockerIgnore.write(
            to: directory.appendingPathComponent(".dockerignore"),
            atomically: true,
            encoding: .utf8
        )
    }

    private func containerBootstrapPayloadURL() -> URL? {
        let candidates = [
            Bundle.main.resourceURL?.appendingPathComponent(
                "container-bootstrap",
                isDirectory: true
            ),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath,
                isDirectory: true)
                .appendingPathComponent("build/run/container-bootstrap",
                                        isDirectory: true)
        ].compactMap { $0 }
        return candidates.first {
            FileManager.default.fileExists(atPath: $0.path)
        }
    }

    private func ownedBuildContextDirectory(_ id: UUID) throws -> URL {
        try safeSpaceDirectory(id).appendingPathComponent("BuildContext", isDirectory: true)
    }

    private func buildContextDirectory(_ id: UUID) throws -> URL {
        buildContextLock.lock()
        let external = buildContexts[id]?.path ?? ""
        buildContextLock.unlock()
        return external.isEmpty
            ? try ownedBuildContextDirectory(id)
            : URL(fileURLWithPath: external, isDirectory: true)
    }

    private func checkedBuildContextDirectory(_ record: SafeSpaceRecord) throws -> URL {
        let directory = try buildContextDirectory(record.id)
        guard let path = record.buildContextPath, !path.isEmpty else {
            return directory
        }
        var status = stat()
        let resolved = realpath(path, nil).map { pointer -> String in
            defer { free(pointer) }
            return String(cString: pointer)
        }
        guard lstat(path, &status) == 0, (status.st_mode & S_IFMT) == S_IFDIR, resolved == path else {
            throw SafeSpaceManagerError.commandFailed(
                "The registered build context must be an existing folder that is not a link."
            )
        }
        if recordIsSelfModifying(record) {
            try requireUnprotectedPath(path, description: "A self-modifying build context")
        }
        return directory
    }

    private func protectedPaths() -> [String] {
        var paths = [(try? applicationDirectory().path) ?? ""]
        if let home = getpwuid(getuid())?.pointee.pw_dir {
            paths.append(String(cString: home) + "/Library/Application Support/outershell")
        }
        return paths.filter { !$0.isEmpty }.map { path in
            realpath(path, nil).map { pointer -> String in
                defer { free(pointer) }
                return String(cString: pointer)
            } ?? path
        }
    }

    private func requireUnprotectedPath(_ path: String, description: String) throws {
        let resolved = realpath(path, nil).map { pointer -> String in
            defer { free(pointer) }
            return String(cString: pointer)
        } ?? path
        for protected in protectedPaths() where
            resolved == protected || resolved.hasPrefix(protected + "/") || protected.hasPrefix(resolved + "/") ||
            resolved == "/" {
            throw SafeSpaceManagerError.commandFailed(
                "\(description) cannot include or be inside Outer Shell's own folders."
            )
        }
    }

    private func dockerfileURL(_ id: UUID) throws -> URL {
        try buildContextDirectory(id).appendingPathComponent("Dockerfile")
    }

    private func dockerfileContents(for record: SafeSpaceRecord) throws -> String {
        try readRegularTextFile(dockerfileURL(record.id))
    }

    private func readRegularTextFile(_ url: URL) throws -> String {
        let descriptor = open(url.path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)
        guard descriptor >= 0 else {
            let code = errno
            if code == ELOOP {
                throw SafeSpaceManagerError.commandFailed(
                    "\(url.lastPathComponent) must be a regular file, not a symbolic link."
                )
            }
            throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
        }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        var information = stat()
        guard fstat(descriptor, &information) == 0,
              information.st_mode & S_IFMT == S_IFREG else {
            throw SafeSpaceManagerError.commandFailed("\(url.lastPathComponent) must be a regular file.")
        }
        guard let text = String(data: try handle.readToEnd() ?? Data(), encoding: .utf8) else {
            throw CocoaError(.fileReadInapplicableStringEncoding)
        }
        return text
    }

    private func requireUnlinkedTree(_ directory: URL) throws {
        let keys: [URLResourceKey] = [.isSymbolicLinkKey]
        guard let enumerator = FileManager.default.enumerator(
            at: directory,
            includingPropertiesForKeys: keys
        ) else { return }
        for case let url as URL in enumerator {
            if try url.resourceValues(forKeys: Set(keys)).isSymbolicLink == true {
                let path = url.path.dropFirst(directory.path.count + 1)
                throw SafeSpaceManagerError.commandFailed(
                    "Linked files and folders cannot be shared: \(path)"
                )
            }
        }
    }

    private func dockerfileBaseImage(in contents: String) -> String? {
        for line in contents.components(separatedBy: .newlines) {
            let fields = line.split(whereSeparator: { $0.isWhitespace })
            guard fields.first?.uppercased() == "FROM" else { continue }
            for field in fields.dropFirst() where !field.hasPrefix("--") {
                return String(field)
            }
        }
        return nil
    }

    private func configurationResourceKey(_ id: UUID) -> String {
        "containers/\(id.uuidString.lowercased())/configuration"
    }

    private func containerImageReference(_ id: UUID) -> String {
        "outershell/container-\(id.uuidString.lowercased()):latest"
    }

    private func createManagedDirectories(for record: SafeSpaceRecord) throws {
        let base = try safeSpaceDirectory(record.id)
        try FileManager.default.createDirectory(
            at: base.appendingPathComponent("Persistent Data", isDirectory: true),
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
    }

    private func persistentDataDirectory(
        _ record: SafeSpaceRecord,
        item: SafeSpacePersistentData
    ) throws -> URL {
        try safeSpaceDirectory(record.id)
            .appendingPathComponent("Persistent Data", isDirectory: true)
            .appendingPathComponent(item.id.uuidString.lowercased(), isDirectory: true)
    }

    private func persistentVolumeName(_ record: SafeSpaceRecord, item: SafeSpacePersistentData) -> String {
        "outershell-\(record.id.uuidString.lowercased())-\(item.id.uuidString.lowercased())"
    }

    private func persistentStorageLocation(_ record: SafeSpaceRecord, item: SafeSpacePersistentData) throws -> String {
        let legacy = try persistentDataDirectory(record, item: item)
        return FileManager.default.fileExists(atPath: legacy.path) ? legacy.path : persistentVolumeName(record, item: item)
    }

    private func volumeArchiveURL(_ record: SafeSpaceRecord, item: SafeSpacePersistentData) throws -> URL {
        try safeSpaceDirectory(record.id).appendingPathComponent("Volume Archives", isDirectory: true)
            .appendingPathComponent(item.id.uuidString.lowercased() + ".tar")
    }

    private func persistentMount(_ record: SafeSpaceRecord, item: SafeSpacePersistentData) throws -> String {
        let legacy = try persistentDataDirectory(record, item: item)
        if FileManager.default.fileExists(atPath: legacy.path) {
            return "type=bind,source=" + legacy.path
        }
        return "type=volume,source=" + persistentVolumeName(record, item: item)
    }

    private func transferPersistentVolume(_ record: SafeSpaceRecord, item: SafeSpacePersistentData,
                                          archive: URL, restore: Bool) throws {
        try FileManager.default.createDirectory(at: archive.deletingLastPathComponent(),
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        var arguments = ["run", "--rm", "--user", "0"]
        if runtimeProvider(for: record) == .docker {
            arguments += ["--network", "none"]
        }
        arguments += [
            "--mount", try persistentMount(record, item: item) + ",target=/data" + (restore ? "" : ",readonly"),
            "--mount", "type=bind,source=\(archive.deletingLastPathComponent().path),target=/transfer" + (restore ? ",readonly" : ""),
            "--entrypoint", "/bin/tar", "alpine:3.22"
        ]
        arguments += restore
            ? ["-xpf", "/transfer/" + archive.lastPathComponent, "-C", "/data"]
            : ["-cpf", "/transfer/" + archive.lastPathComponent, "-C", "/data", "."]
        try requireSuccess(try runRuntime(record, arguments), action: restore ? "restore persistent data" : "export persistent data")
    }

    private func exportPersistentVolume(_ record: SafeSpaceRecord, item: SafeSpacePersistentData,
                                        archive: URL) throws {
        let pending = try volumeArchiveURL(record, item: item)
        if FileManager.default.fileExists(atPath: pending.path) {
            try FileManager.default.createDirectory(at: archive.deletingLastPathComponent(), withIntermediateDirectories: true)
            try FileManager.default.copyItem(at: pending, to: archive)
        } else {
            try transferPersistentVolume(record, item: item, archive: archive, restore: false)
        }
    }

    private func preparePersistentVolume(_ record: SafeSpaceRecord, item: SafeSpacePersistentData) throws {
        let legacy = try persistentDataDirectory(record, item: item)
        if FileManager.default.fileExists(atPath: legacy.path) { return }
        let name = persistentVolumeName(record, item: item)
        let archive = try volumeArchiveURL(record, item: item)
        let pending = FileManager.default.fileExists(atPath: archive.path)
        let exists = try runRuntime(record, ["volume", "inspect", name]).status == 0
        if exists && pending {
            throw SafeSpaceManagerError.commandFailed("A volume already exists for pending imported data; refusing to overwrite it.")
        }
        if !exists {
            try requireSuccess(try runRuntime(record, ["volume", "create", name]), action: "create persistent storage")
            if pending {
                do {
                    try transferPersistentVolume(record, item: item, archive: archive, restore: true)
                } catch {
                    _ = try? runRuntime(record, ["volume", "rm", name])
                    throw error
                }
                try FileManager.default.removeItem(at: archive)
            }
        }
    }

    private func applicationDirectory() throws -> URL {
        let base = try FileManager.default.url(for: .applicationSupportDirectory,
                                               in: .userDomainMask,
                                               appropriateFor: nil,
                                               create: true)
            .appendingPathComponent("Outer Shell", isDirectory: true)
        try FileManager.default.createDirectory(at: base,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        return base
    }

    private func transferDirectory() throws -> URL {
        let directory = try applicationDirectory()
            .appendingPathComponent("Transfers", isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        return directory
    }

    private func safeSpaceDirectory(_ id: UUID) throws -> URL {
        try applicationDirectory()
            .appendingPathComponent("Containers", isDirectory: true)
            .appendingPathComponent(id.uuidString.lowercased(), isDirectory: true)
    }

    private func relayDirectory(_ id: UUID) -> URL {
        URL(fileURLWithPath: "/private/tmp/outershell-\(getuid())", isDirectory: true)
            .appendingPathComponent("container-relays", isDirectory: true)
            .appendingPathComponent(String(id.uuidString.lowercased().prefix(8)), isDirectory: true)
    }

    private func relaySocketPath(_ id: UUID, socketPath: String) -> String {
        let digest = socketPath.utf8.reduce(UInt64(14_695_981_039_346_656_037)) {
            ($0 ^ UInt64($1)) &* 1_099_511_628_211
        }
        return relayDirectory(id).appendingPathComponent(String(format: "%016llx.sock", digest)).path
    }

    private func publishedSocketPath(_ id: UUID, socketPath: String) throws -> String {
        try FileManager.default.createDirectory(at: relayDirectory(id),
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        return relaySocketPath(id, socketPath: socketPath)
    }

    private func authorizePublishedSocketPath(_ socketPath: String) throws {
        guard let password = getpwuid(getuid()),
              let home = password.pointee.pw_dir else {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell could not locate the local socket allowlist."
            )
        }
        let directory = URL(fileURLWithPath: String(cString: home), isDirectory: true)
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Application Support", isDirectory: true)
            .appendingPathComponent("org.outerssh", isDirectory: true)
        try FileManager.default.createDirectory(
            at: directory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        var directoryStatus = stat()
        guard lstat(directory.path, &directoryStatus) == 0,
              directoryStatus.st_uid == getuid(),
              (directoryStatus.st_mode & S_IFMT) == S_IFDIR,
              (directoryStatus.st_mode & 0o022) == 0 else {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell's local socket allowlist directory is unsafe."
            )
        }

        let allowlist = directory.appendingPathComponent("http-unix.allow")
        var fileStatus = stat()
        let status = lstat(allowlist.path, &fileStatus)
        if status == 0 {
            guard fileStatus.st_uid == getuid(),
                  (fileStatus.st_mode & S_IFMT) == S_IFREG,
                  (fileStatus.st_mode & 0o022) == 0 else {
                throw SafeSpaceManagerError.commandFailed(
                    "Outer Shell's local socket allowlist is unsafe."
                )
            }
        } else if errno != ENOENT {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell could not inspect the local socket allowlist."
            )
        }

        let existing = status == 0
            ? try String(contentsOf: allowlist, encoding: .utf8)
            : ""
        let entries = existing
            .components(separatedBy: .newlines)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        guard !entries.contains(socketPath) else {
            return
        }
        var updated = existing
        if !updated.isEmpty, !updated.hasSuffix("\n") {
            updated += "\n"
        }
        updated += socketPath + "\n"
        try updated.write(to: allowlist, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o600],
            ofItemAtPath: allowlist.path
        )
    }

    private func closePublishedForwards(_ id: UUID) {
        let prefix = "\(id.uuidString.lowercased())\n"
        let removed = lock.withSafeSpaceLock { () -> [SafeSpaceSocketRelayForward] in
            endpointPublicationFailures.retain([], prefix: prefix)
            let values = publishedForwards.compactMap {
                $0.key.hasPrefix(prefix) ? $0.value : nil
            }
            publishedForwards = publishedForwards.filter {
                !$0.key.hasPrefix(prefix)
            }
            return values
        }
        for forward in removed {
            forward.close()
        }
    }

    private func setTransientState(_ state: String?, for id: UUID) {
        lock.withSafeSpaceLock {
            transientStates[id] = state
        }
    }

    private func setBuildProgress(_ progress: SafeSpaceBuildProgress?, for id: UUID) {
        lock.withSafeSpaceLock {
            buildProgress[id] = progress
        }
    }

    private func updateBuildProgressPhase(_ phase: String,
                                          detail: String,
                                          for id: UUID) {
        lock.withSafeSpaceLock {
            guard var progress = buildProgress[id] else { return }
            progress.phase = phase
            progress.detail = detail
            if phase != "building" {
                progress.instruction = nil
                progress.currentStep = nil
                progress.totalSteps = nil
                progress.sourceStartLine = nil
                progress.sourceEndLine = nil
            }
            buildProgress[id] = progress
        }
    }

    private func finishBuild(phase: String, detail: String, for id: UUID) {
        let generation = lock.withSafeSpaceLock { () -> UUID? in
            guard var progress = buildProgress[id] else { return nil }
            progress.phase = phase
            progress.detail = detail
            progress.instruction = nil
            progress.currentStep = nil
            progress.totalSteps = nil
            progress.sourceStartLine = nil
            progress.sourceEndLine = nil
            if phase == "failed" {
                let separator = progress.log.isEmpty || progress.log.hasSuffix("\n") ? "" : "\n"
                progress.log += "\(separator)Error: \(detail)\n"
            }
            buildProgress[id] = progress
            transientStates[id] = nil
            return progress.generation
        }
        guard let generation else { return }
        Task.detached { [weak self] in
            try? await Task.sleep(for: .seconds(15))
            self?.clearFinishedBuildProgress(for: id, generation: generation)
        }
    }

    private func clearFinishedBuildProgress(for id: UUID, generation: UUID) {
        lock.withSafeSpaceLock {
            guard let progress = buildProgress[id],
                  progress.generation == generation,
                  progress.phase == "complete" || progress.phase == "failed" else {
                return
            }
            buildProgress[id] = nil
        }
    }

    private func appendBuildOutput(_ output: String, for record: SafeSpaceRecord) {
        let cleanOutput = normalizedBuildOutput(output)
        guard !cleanOutput.isEmpty else { return }
        var activeInstruction: String?
        lock.withSafeSpaceLock {
            guard var progress = buildProgress[record.id] else { return }
            progress.log += cleanOutput
            if progress.log.count > 65_536 {
                progress.log = String(progress.log.suffix(65_536))
            }
            let combined = progress.lineRemainder + cleanOutput
            var lines = combined.components(separatedBy: "\n")
            progress.lineRemainder = lines.popLast() ?? ""
            for line in lines {
                if let parsed = buildStep(in: line) {
                    progress.phase = "building"
                    progress.instruction = parsed.instruction
                    progress.currentStep = parsed.current
                    progress.totalSteps = parsed.total
                    progress.detail = "Step \(parsed.current) of \(parsed.total): \(buildStepSummary(parsed.instruction))"
                    activeInstruction = parsed.instruction
                }
            }
            buildProgress[record.id] = progress
        }
        guard let activeInstruction,
              let dockerfile = try? dockerfileContents(for: record),
              let lineRange = dockerfileLineRange(
                  matching: activeInstruction,
                  in: dockerfile
              ) else {
            return
        }
        lock.withSafeSpaceLock {
            guard var progress = buildProgress[record.id],
                  progress.instruction == activeInstruction else {
                return
            }
            progress.sourceStartLine = lineRange.lowerBound
            progress.sourceEndLine = lineRange.upperBound
            buildProgress[record.id] = progress
        }
    }

    private func normalizedBuildOutput(_ output: String) -> String {
        let newlines = output
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
        guard let expression = try? NSRegularExpression(
            pattern: "\u{001B}\\[[0-?]*[ -/]*[@-~]"
        ) else {
            return newlines
        }
        return expression.stringByReplacingMatches(
            in: newlines,
            range: NSRange(location: 0, length: (newlines as NSString).length),
            withTemplate: ""
        )
    }

    private func buildStep(in line: String) -> (
        current: Int,
        total: Int,
        instruction: String
    )? {
        guard line.hasPrefix("#"),
              let openingBracket = line.firstIndex(of: "["),
              let closingBracket = line.range(
                  of: "] ",
                  range: openingBracket..<line.endIndex
              ) else {
            return nil
        }
        let descriptor = line[line.index(after: openingBracket)..<closingBracket.lowerBound]
        guard let step = descriptor.split(whereSeparator: { $0.isWhitespace }).last else {
            return nil
        }
        let counts = step.split(separator: "/", maxSplits: 1)
        guard counts.count == 2,
              let current = Int(counts[0]),
              let total = Int(counts[1]) else {
            return nil
        }
        let instruction = String(line[closingBracket.upperBound...])
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !instruction.isEmpty else { return nil }
        return (current, total, instruction)
    }

    private func buildStepSummary(_ instruction: String) -> String {
        let maximumLength = 120
        guard instruction.count > maximumLength else { return instruction }
        return String(instruction.prefix(maximumLength - 1)) + "…"
    }

    private func dockerfileLineRange(matching instruction: String,
                                     in dockerfile: String) -> ClosedRange<Int>? {
        let target = normalizedDockerfileInstruction(instruction)
        let lines = dockerfile.components(separatedBy: "\n")
        var accumulated = ""
        var startLine = 1
        for (index, line) in lines.enumerated() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if accumulated.isEmpty {
                startLine = index + 1
            }
            let continues = trimmed.hasSuffix("\\")
            let component = continues
                ? String(trimmed.dropLast())
                : trimmed
            accumulated += accumulated.isEmpty ? component : " " + component
            if continues {
                continue
            }
            if normalizedDockerfileInstruction(accumulated) == target {
                return startLine...(index + 1)
            }
            accumulated = ""
        }
        return nil
    }

    private func normalizedDockerfileInstruction(_ value: String) -> String {
        value.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }

    private func availableDestination(for source: URL, in directory: URL) -> URL {
        let fileManager = FileManager.default
        let extensionName = source.pathExtension
        let baseName = extensionName.isEmpty
            ? source.lastPathComponent
            : source.deletingPathExtension().lastPathComponent
        var destination = directory.appendingPathComponent(source.lastPathComponent)
        var suffix = 2
        while fileManager.fileExists(atPath: destination.path) {
            let name = extensionName.isEmpty
                ? "\(baseName) \(suffix)"
                : "\(baseName) \(suffix).\(extensionName)"
            destination = directory.appendingPathComponent(name)
            suffix += 1
        }
        return destination
    }

    private func managedContainerName(_ id: UUID) -> String {
        "outershell-container-\(id.uuidString.lowercased())"
    }

    private func registeredRuntimeName(for record: SafeSpaceRecord) -> String {
        let value = record.runtimeName?.trimmingCharacters(in: .whitespacesAndNewlines)
        return value.flatMap { $0.isEmpty ? nil : $0 } ?? managedContainerName(record.id)
    }

    private func containerName(_ id: UUID) -> String {
        lock.withSafeSpaceLock {
            records.first(where: { $0.id == id }).map(registeredRuntimeName)
                ?? managedContainerName(id)
        }
    }

    private func recordOwnsContainer(_ record: SafeSpaceRecord) -> Bool {
        record.ownsContainer ?? true
    }

    private func recordOwnsBuildContext(_ record: SafeSpaceRecord) -> Bool {
        record.ownsBuildContext ?? recordOwnsContainer(record)
    }

    private func recordIsSelfModifying(_ record: SafeSpaceRecord) -> Bool {
        record.selfModifying ?? recordOwnsBuildContext(record)
    }

    private func requireManagedContainer(_ record: SafeSpaceRecord) throws {
        guard recordOwnsContainer(record) else {
            throw SafeSpaceManagerError.commandFailed(
                "This container is managed externally. Outer Shell can operate it and expose its apps, but cannot change its image or project."
            )
        }
    }

    private func runtimeIdentity(for record: SafeSpaceRecord) throws -> RuntimeIdentity {
        return RuntimeIdentity(
            user: "root",
            homeDirectory: "/root",
            runtimeDirectory: "/run/user/0",
            outerShellHome: "/var/lib/outershell",
            outerctlPath: "/usr/local/bin/outerctl",
            daemonPIDPath: "/run/user/0/outershelld.pid"
        )
    }

    private func containerImageReferences(
        provider: SafeSpaceRuntimeProviderID = .appleContainer
    ) throws -> Set<String> {
        switch provider {
        case .appleContainer:
            let result = try runContainer(["image", "list", "--format", "json"])
            try requireSuccess(result, action: "inspect container images")
            guard let data = result.stdout.data(using: .utf8),
                  let values = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
                throw SafeSpaceManagerError.commandFailed(
                    "Apple container returned an invalid image list."
                )
            }
            return Set(values.compactMap { value in
                (value["configuration"] as? [String: Any])?["name"] as? String
            })
        case .docker:
            let result = try runDocker([
                "image", "list", "--format", "{{.Repository}}:{{.Tag}}"
            ])
            try requireSuccess(result, action: "inspect Docker images")
            return Set(result.stdout.split(separator: "\n").map(String.init).filter {
                !$0.hasSuffix(":<none>")
            })
        }
    }

    private func reconcilePersistentData(
        for record: SafeSpaceRecord,
        imageReference: String
    ) throws -> [SafeSpacePersistentData] {
        let declaredPaths = try containerImageVolumePaths(record, imageReference)
        var value = try configuration(for: record)
        var items = value.persistentData ?? []
        for index in items.indices {
            items[index].isDeclared = declaredPaths.contains(items[index].guestPath)
        }
        for path in declaredPaths.sorted() where !items.contains(where: {
            $0.guestPath == path
        }) {
            items.append(
                SafeSpacePersistentData(id: UUID(), guestPath: path, isDeclared: true)
            )
        }
        if items != value.persistentData ?? [] {
            value.persistentData = items
            try saveConfiguration(value, for: record)
        }
        return items.filter(\.isDeclared)
    }

    private func containerImageVolumePaths(_ record: SafeSpaceRecord,
                                           _ imageReference: String) throws -> Set<String> {
        let result = try runRuntime(record, ["image", "inspect", imageReference])
        try requireSuccess(result, action: "inspect the container image")
        guard let data = result.stdout.data(using: .utf8),
              let images = try JSONSerialization.jsonObject(with: data) as? [[String: Any]],
              let image = images.first else {
            throw SafeSpaceManagerError.commandFailed(
                "The container runtime returned invalid image metadata."
            )
        }
        let imageConfig: [String: Any]
        if runtimeProvider(for: record) == .docker {
            imageConfig = image
        } else {
            guard let variants = image["variants"] as? [[String: Any]] else {
                throw SafeSpaceManagerError.commandFailed(
                    "Apple container returned invalid image metadata."
                )
            }
            let variant = variants.first(where: { value in
                guard let platform = value["platform"] as? [String: Any] else { return false }
                return platform["architecture"] as? String == cpuArchitecture() &&
                    platform["os"] as? String == "linux"
            }) ?? variants.first
            guard let variant,
                  let config = variant["config"] as? [String: Any] else {
                throw SafeSpaceManagerError.commandFailed(
                    "The container image has no compatible Linux configuration."
                )
            }
            imageConfig = config
        }

        var paths: Set<String> = []
        if let runtimeConfig = (imageConfig["config"] as? [String: Any]) ??
            (imageConfig["Config"] as? [String: Any]) {
            let volumes = runtimeConfig["Volumes"] as? [String: Any] ??
                runtimeConfig["volumes"] as? [String: Any]
            for path in volumes?.keys ?? Dictionary<String, Any>().keys {
                if let normalized = normalizedPersistentDataGuestPath(path) {
                    paths.insert(normalized)
                }
            }
        }
        if let history = imageConfig["history"] as? [[String: Any]] {
            for item in history {
                guard let instruction = item["created_by"] as? String else { continue }
                paths.formUnion(volumePaths(in: instruction))
            }
        }
        return paths
    }

    private func volumePaths(in instruction: String) -> Set<String> {
        let trimmed = instruction.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let separator = trimmed.firstIndex(where: { $0.isWhitespace }),
              trimmed[..<separator].uppercased() == "VOLUME" else {
            return []
        }
        let arguments = trimmed[separator...]
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !arguments.isEmpty else { return [] }

        var values: [String] = []
        if arguments.hasPrefix("["),
           let data = arguments.data(using: .utf8),
           let decoded = try? JSONSerialization.jsonObject(with: data) as? [String] {
            values = decoded
        } else {
            let unwrapped = arguments.hasPrefix("[") && arguments.hasSuffix("]")
                ? String(arguments.dropFirst().dropLast())
                : arguments
            values = unwrapped.split(whereSeparator: { $0.isWhitespace }).map { value in
                String(value).trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
            }
        }
        return Set(values.compactMap(normalizedPersistentDataGuestPath))
    }

    private func normalizedPersistentDataGuestPath(_ path: String) -> String? {
        guard path.hasPrefix("/"),
              !path.contains("\0"),
              !path.contains("$") else {
            return nil
        }
        let normalized = NSString(string: path).standardizingPath
        guard normalized != "/" else { return nil }
        return normalized
    }

    private func runContainerBuild(_ record: SafeSpaceRecord,
                                   imageReference: String)
        throws -> (status: Int32, stdout: String, stderr: String) {
        updateBuildProgressPhase(
            "preparing",
            detail: "Preparing the build context",
            for: record.id
        )
        containerBaseImageLock.lock()
        defer {
            containerBaseImageLock.unlock()
            if runtimeProvider(for: record) == .appleContainer { scheduleAppleImageCleanup() }
        }
        try upgradeDefinition(for: record)
        let value = normalizedConfiguration(try configuration(for: record))
        if try dockerfileContents(for: record).contains(containerSupportImage) {
            try ensureSupportImage(for: runtimeProvider(for: record))
        }
        try saveConfiguration(value, for: record)
        let directory = try checkedBuildContextDirectory(record)
        updateBuildProgressPhase(
            "building",
            detail: "Building the container image",
            for: record.id
        )
        let dockerfile = try safeSpaceDirectory(record.id)
            .appendingPathComponent("Dockerfile.outershell")
        var contents = try dockerfileContents(for: record)
        var buildArguments = ["build", "--file", dockerfile.path]
        if runtimeProvider(for: record) == .docker,
           let helpers = try containerHelpersBuildContext(for: record) {
            contents = contents.trimmingCharacters(in: .newlines) +
                "\nCOPY --from=outershell-helpers / /usr/local/lib/outershell/helpers/\n"
            buildArguments += ["--build-context", "outershell-helpers=" + helpers.path]
        }
        try contents.write(to: dockerfile, atomically: true, encoding: .utf8)
        let result = try runRuntime(record, buildArguments + [
            "--tag", imageReference,
            "--progress", "plain",
            "."
        ], currentDirectoryURL: directory) { [weak self] output in
            self?.appendBuildOutput(output, for: record)
        }
        try requireSuccess(result, action: "build the container image")
        return result
    }

    /// Lays out the container helpers of apps installed through Outer Shell,
    /// for the Docker engine's architecture, at their content-addressed paths
    /// (see ContainerHelper.h). Returns nil when there are none.
    private func containerHelpersBuildContext(for record: SafeSpaceRecord) throws -> URL? {
        let info = try runDocker(["info", "--format", "{{.Architecture}}"])
        var architecture = info.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        architecture = ["amd64": "x86_64", "arm64": "aarch64"][architecture] ?? architecture
        let root = ProcessInfo.processInfo.environment["OUTERSHELL_HOME"]
            ?? NSHomeDirectory() + "/Library/Application Support/outershell"
        let apps = URL(fileURLWithPath: (root as NSString).expandingTildeInPath)
            .appendingPathComponent("apps", isDirectory: true)
        let fileManager = FileManager.default
        let context = try safeSpaceDirectory(record.id)
            .appendingPathComponent("Helpers", isDirectory: true)
        try? fileManager.removeItem(at: context)
        var found = false
        for app in (try? fileManager.contentsOfDirectory(atPath: apps.path))?.sorted() ?? [] {
            let appURL = apps.appendingPathComponent(app, isDirectory: true)
            let bundles = ((try? fileManager.contentsOfDirectory(atPath: appURL.path)) ?? [])
                .filter { $0.hasSuffix(".app") }.sorted()
            let candidates = [appURL.appendingPathComponent("helpers", isDirectory: true)] +
                bundles.map { appURL.appendingPathComponent($0 + "/Contents/Resources/helpers", isDirectory: true) }
            for helpers in candidates {
                let directory = helpers.appendingPathComponent("linux-" + architecture, isDirectory: true)
                for name in (try? fileManager.contentsOfDirectory(atPath: directory.path))?.sorted() ?? [] {
                    let source = directory.appendingPathComponent(name)
                    let values = try? source.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
                    guard values?.isRegularFile == true, values?.isSymbolicLink != true else { continue }
                    let data = try Data(contentsOf: source)
                    let hash = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
                    let target = context.appendingPathComponent("\(app)/\(hash)/\(name)")
                    guard !fileManager.fileExists(atPath: target.path) else { continue }
                    try fileManager.createDirectory(at: target.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
                    try data.write(to: target)
                    try fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: target.path)
                    found = true
                }
            }
        }
        return found ? context : nil
    }

    private func cpuArchitecture() -> String {
#if arch(arm64)
        return "arm64"
#elseif arch(x86_64)
        return "x86_64"
#else
        return "unknown"
#endif
    }

    private func runtimeProvider(for record: SafeSpaceRecord) -> SafeSpaceRuntimeProviderID {
        SafeSpaceRuntimeProviderID(rawValue: record.runtimeProviderID ?? "") ??
            .appleContainer
    }

    private func runtimeExecutableURL(
        for provider: SafeSpaceRuntimeProviderID
    ) -> URL? {
        switch provider {
        case .appleContainer:
            return ContainerRuntime.executable(named: "container")
        case .docker:
            return dockerExecutableURL()
        }
    }

    private func requiredRuntimeExecutablePath(for record: SafeSpaceRecord) throws -> String {
        guard let executable = runtimeExecutableURL(for: runtimeProvider(for: record)) else {
            throw SafeSpaceManagerError.unsupportedProvider
        }
        return executable.path
    }

    private func dockerExecutableURL() -> URL? {
        ContainerRuntime.executable(named: "docker")
    }

    private func runRuntime(_ record: SafeSpaceRecord,
                            _ arguments: [String],
                            currentDirectoryURL: URL? = nil,
                            progress: ((String) -> Void)? = nil) throws
        -> (status: Int32, stdout: String, stderr: String) {
        switch runtimeProvider(for: record) {
        case .appleContainer:
            return try runContainer(arguments,
                                    currentDirectoryURL: currentDirectoryURL,
                                    progress: progress)
        case .docker:
            return try runDocker(arguments,
                                 currentDirectoryURL: currentDirectoryURL,
                                 progress: progress)
        }
    }

    private func runDocker(_ arguments: [String],
                           currentDirectoryURL: URL? = nil,
                           progress: ((String) -> Void)? = nil) throws
        -> (status: Int32, stdout: String, stderr: String) {
        guard let executable = dockerExecutableURL() else {
            throw SafeSpaceManagerError.unsupportedProvider
        }
        return try runCommand(executable: executable.path,
                              arguments: arguments.first == "build"
                ? ["build", "--label", "org.outershell.managed=true"] + arguments.dropFirst()
                : arguments,
                              environment: dockerEnvironment(executable: executable),
                              currentDirectoryURL: currentDirectoryURL,
                              progress: progress)
    }

    private func dockerEnvironment(executable: URL) -> [String: String] {
        let resolvedExecutable = executable.resolvingSymlinksInPath()
        let inheritedPath = ProcessInfo.processInfo.environment["PATH"] ?? ""
        let candidates = [
            resolvedExecutable.deletingLastPathComponent().path,
            executable.deletingLastPathComponent().path,
            "/Applications/Docker.app/Contents/Resources/bin",
            "/usr/local/bin",
            "/opt/homebrew/bin"
        ] + inheritedPath.split(separator: ":").map(String.init) + [
            "/usr/bin",
            "/bin",
            "/usr/sbin",
            "/sbin"
        ]
        var seen: Set<String> = []
        let path = candidates.filter { !$0.isEmpty && seen.insert($0).inserted }
            .joined(separator: ":")
        return ["PATH": path]
    }

    private func removeRuntimeContainer(
        _ record: SafeSpaceRecord,
        force: Bool
    ) throws -> (status: Int32, stdout: String, stderr: String) {
        switch runtimeProvider(for: record) {
        case .appleContainer:
            return try runContainer(
                ["delete"] + (force ? ["--force"] : []) + [containerName(record.id)]
            )
        case .docker:
            return try runDocker(
                ["rm"] + (force ? ["--force"] : []) + [containerName(record.id)]
            )
        }
    }

    private func copyFromRuntime(
        _ record: SafeSpaceRecord,
        source: String,
        destination: String
    ) throws -> (status: Int32, stdout: String, stderr: String) {
        switch runtimeProvider(for: record) {
        case .appleContainer:
            return try runContainer(["copy", source, destination])
        case .docker:
            return try runDocker(["cp", source, destination])
        }
    }

    private func dockerObjectIsMissing(
        _ result: (status: Int32, stdout: String, stderr: String)
    ) -> Bool {
        let message = result.stderr + "\n" + result.stdout
        return message.localizedCaseInsensitiveContains("no such object") ||
            message.localizedCaseInsensitiveContains("no such container")
    }

    private func runtimeObjectIsMissing(
        _ result: (status: Int32, stdout: String, stderr: String)
    ) -> Bool {
        dockerObjectIsMissing(result) ||
            (result.stderr + result.stdout)
                .localizedCaseInsensitiveContains("not found")
    }

    private func runContainer(_ arguments: [String],
                              currentDirectoryURL: URL? = nil,
                              progress: ((String) -> Void)? = nil) throws
        -> (status: Int32, stdout: String, stderr: String) {
        guard let executable = runtimeExecutableURL(for: .appleContainer)?.path else {
            throw SafeSpaceManagerError.unsupportedProvider
        }
        let result = try runCommand(executable: executable,
                                    arguments: arguments,
                                    currentDirectoryURL: currentDirectoryURL,
                                    progress: progress)
        guard containerSystemIsUnavailable(result),
              arguments != ["system", "start"] else {
            return result
        }
        return try containerSystemRecoveryLock.withSafeSpaceLock {
            let retry = try runCommand(executable: executable,
                                       arguments: arguments,
                                       currentDirectoryURL: currentDirectoryURL,
                                       progress: progress)
            guard containerSystemIsUnavailable(retry) else {
                return retry
            }
            let start = try runCommand(executable: executable,
                                       arguments: ["system", "start", "--enable-kernel-install", "--timeout", "30"],
                                       progress: progress, timeout: 60)
            try requireSuccess(start, action: "start Apple container")
            return try runCommand(executable: executable,
                                  arguments: arguments,
                                  currentDirectoryURL: currentDirectoryURL,
                                  progress: progress)
        }
    }

    private func containerSystemIsUnavailable(
        _ result: (status: Int32, stdout: String, stderr: String)
    ) -> Bool {
        guard result.status != 0 else {
            return false
        }
        let message = result.stderr + "\n" + result.stdout
        return message.contains("container system start") ||
            message.contains("XPC connection error") ||
            message.contains("Connection invalid")
    }

    private func runCommand(executable: String,
                            arguments: [String],
                            environment: [String: String] = [:],
                            currentDirectoryURL: URL? = nil,
                            progress: ((String) -> Void)? = nil,
                            timeout: TimeInterval? = nil) throws
        -> (status: Int32, stdout: String, stderr: String) {
        let outputDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("outershell-container-\(UUID().uuidString)",
                                    isDirectory: true)
        try FileManager.default.createDirectory(at: outputDirectory,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        defer {
            try? FileManager.default.removeItem(at: outputDirectory)
        }
        let stdoutURL = outputDirectory.appendingPathComponent("stdout")
        let stderrURL = outputDirectory.appendingPathComponent("stderr")
        try Data().write(to: stdoutURL)
        try Data().write(to: stderrURL)
        let stdout = try FileHandle(forWritingTo: stdoutURL)
        let stderr = try FileHandle(forWritingTo: stderrURL)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.currentDirectoryURL = currentDirectoryURL
        if !environment.isEmpty {
            process.environment = ProcessInfo.processInfo.environment.merging(
                environment,
                uniquingKeysWith: { _, replacement in replacement }
            )
        }
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = stdout
        process.standardError = stderr
        do {
            try process.run()
        } catch {
            try? stdout.close()
            try? stderr.close()
            throw error
        }
        if progress == nil && timeout == nil {
            process.waitUntilExit()
        } else {
            let stdoutReader = try FileHandle(forReadingFrom: stdoutURL)
            let stderrReader = try FileHandle(forReadingFrom: stderrURL)
            defer {
                try? stdoutReader.close()
                try? stderrReader.close()
            }
            let started = ProcessInfo.processInfo.systemUptime
            while process.isRunning {
                if let timeout, ProcessInfo.processInfo.systemUptime - started >= timeout {
                    kill(process.processIdentifier, SIGKILL)
                    process.waitUntilExit()
                    throw SafeSpaceManagerError.commandFailed("The container runtime did not finish starting within \(Int(timeout)) seconds. Run container system start on this server, then try again.")
                }
                if let progress {
                    if let data = try stdoutReader.readToEnd(), !data.isEmpty,
                       let text = String(data: data, encoding: .utf8) { progress(text) }
                    if let data = try stderrReader.readToEnd(), !data.isEmpty,
                       let text = String(data: data, encoding: .utf8) { progress(text) }
                }
                Thread.sleep(forTimeInterval: 0.1)
            }
            process.waitUntilExit()
            if let progress {
                if let data = try stdoutReader.readToEnd(), !data.isEmpty,
                   let text = String(data: data, encoding: .utf8) { progress(text) }
                if let data = try stderrReader.readToEnd(), !data.isEmpty,
                   let text = String(data: data, encoding: .utf8) { progress(text) }
            }
        }
        try? stdout.close()
        try? stderr.close()
        return (
            process.terminationStatus,
            String(data: try Data(contentsOf: stdoutURL), encoding: .utf8) ?? "",
            String(data: try Data(contentsOf: stderrURL), encoding: .utf8) ?? ""
        )
    }

    private func requireSuccess(
        _ result: (status: Int32, stdout: String, stderr: String),
        action: String
    ) throws {
        guard result.status == 0 else {
            let message = [result.stderr, result.stdout]
                .joined(separator: "\n")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw SafeSpaceManagerError.commandFailed(
                message.isEmpty ? "Could not \(action)." : message
            )
        }
    }
}

private enum RegistryResourceStore {
    private static func message(from buffer: [CChar]) -> String {
        String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    static func data(for key: String) throws -> Data? {
        var pointer: UnsafeMutablePointer<UInt8>?
        var length = 0
        var error = [CChar](repeating: 0, count: 2048)
        let succeeded = key.withCString { keyPointer in
            outerShellRegistryCopyResource(
                keyPointer,
                &pointer,
                &length,
                &error,
                error.count
            )
        }
        guard succeeded != 0 else {
            throw SafeSpaceManagerError.commandFailed(message(from: error))
        }
        guard let pointer else { return nil }
        defer { outerShellRegistryFreeResource(pointer) }
        return Data(bytes: pointer, count: length)
    }

    static func set(_ data: Data, for key: String) throws {
        var error = [CChar](repeating: 0, count: 2048)
        let succeeded = key.withCString { keyPointer in
            data.withUnsafeBytes { bytes in
                outerShellRegistrySetResource(
                    keyPointer,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    data.count,
                    &error,
                    error.count
                )
            }
        }
        guard succeeded != 0 else {
            throw SafeSpaceManagerError.commandFailed(message(from: error))
        }
    }

    static func remove(_ key: String) throws {
        var error = [CChar](repeating: 0, count: 2048)
        let succeeded = key.withCString { keyPointer in
            outerShellRegistryRemoveResource(keyPointer, &error, error.count)
        }
        guard succeeded != 0 else {
            throw SafeSpaceManagerError.commandFailed(message(from: error))
        }
    }
}

private func shellQuoted(_ value: String) -> String {
    value.replacingOccurrences(of: "'", with: "'\\''")
}

private func shellCommandArgument(_ value: String) -> String {
    let unquotedCharacters = CharacterSet.alphanumerics.union(
        CharacterSet(charactersIn: "_@%+=:,./-")
    )
    if !value.isEmpty,
       value.unicodeScalars.allSatisfy({ unquotedCharacters.contains($0) }) {
        return value
    }
    return "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
}

private extension Data {
    mutating func safeSpaceAppend<T: FixedWidthInteger>(_ value: T) {
        var littleEndian = value.littleEndian
        Swift.withUnsafeBytes(of: &littleEndian) { append(contentsOf: $0) }
    }

    mutating func safeSpaceWrite<T: FixedWidthInteger>(_ value: T, at offset: Int) {
        var littleEndian = value.littleEndian
        Swift.withUnsafeBytes(of: &littleEndian) {
            replaceSubrange(offset..<(offset + $0.count), with: $0)
        }
    }

    mutating func safeSpaceAppendReference(_ value: Data, at offset: Int) {
        safeSpaceWrite(UInt32(count), at: offset)
        safeSpaceWrite(UInt32(value.count), at: offset + 4)
        append(value)
    }

    func safeSpaceUInt16(at offset: Int) -> UInt16 {
        UInt16(self[index(startIndex, offsetBy: offset)]) |
            UInt16(self[index(startIndex, offsetBy: offset + 1)]) << 8
    }

    func safeSpaceUInt32(at offset: Int) -> UInt32 {
        UInt32(self[index(startIndex, offsetBy: offset)]) |
            UInt32(self[index(startIndex, offsetBy: offset + 1)]) << 8 |
            UInt32(self[index(startIndex, offsetBy: offset + 2)]) << 16 |
            UInt32(self[index(startIndex, offsetBy: offset + 3)]) << 24
    }

    func safeSpaceUInt64(at offset: Int) -> UInt64 {
        UInt64(safeSpaceUInt32(at: offset)) |
            UInt64(safeSpaceUInt32(at: offset + 4)) << 32
    }

    func safeSpaceReferencedData(at offset: Int) throws -> Data {
        guard offset >= 0, offset + 8 <= count else {
            throw SafeSpaceManagerError.commandFailed(
                "The container response contained an invalid reference."
            )
        }
        let valueOffset = Int(safeSpaceUInt32(at: offset))
        let valueLength = Int(safeSpaceUInt32(at: offset + 4))
        guard valueOffset >= 0,
              valueLength >= 0,
              valueOffset <= count,
              valueLength <= count - valueOffset else {
            throw SafeSpaceManagerError.commandFailed(
                "The container response contained an invalid reference."
            )
        }
        return subdata(in: valueOffset..<(valueOffset + valueLength))
    }

    func safeSpaceReferencedString(at offset: Int) throws -> String {
        let value = try safeSpaceReferencedData(at: offset)
        guard let string = String(data: value, encoding: .utf8) else {
            throw SafeSpaceManagerError.commandFailed(
                "The container response contained invalid text."
            )
        }
        return string
    }
}

private extension NSLock {
    func withSafeSpaceLock<Result>(_ body: () throws -> Result) rethrows -> Result {
        lock()
        defer {
            unlock()
        }
        return try body()
    }
}
