import Darwin
import Foundation
import AppKit

private let currentSafeSpaceRecipeVersion = 13
private let rootContainerBaseImage = "outershell/container-base:10"
private let bundledAppOCIImageVersion = "2"

private struct SafeSpaceRecord: Codable {
    var id: UUID
    var name: String
    var createdAt: Date
    var cpus: Int
    var memoryInGB: Int
    var runtimeProviderID: String?
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

private struct SafeSpaceRecipe: Codable {
    var version: Int
    var baseImage: String
    var realizedBaseImage: String?
    var installsOuterShellSupport: Bool?
    var realizedInstallsOuterShellSupport: Bool?
    var steps: [SafeSpaceRecipeStep]
    var realizedStepIDs: [UUID]
    var hasUntrackedChanges: Bool?
    var mounts: [SafeSpaceMount]?
    var launchers: [SafeSpaceRecipeLauncher]?
    var realizedLauncherIDs: [UUID]?
    var realizedEditableStepContents: [String: String]?
    var users: [SafeSpaceRecipeUser]?
    var realizedDockerfileContents: String?
    var environment: [SafeSpaceEnvironmentVariable]?
    var realizedMounts: [SafeSpaceMount]?
    var realizedEnvironment: [SafeSpaceEnvironmentVariable]?
    var publishedPorts: [SafeSpacePublishedPort]?
    var realizedPublishedPorts: [SafeSpacePublishedPort]?
    var persistentData: [SafeSpacePersistentData]?
}

private struct SafeSpaceEnvironmentVariable: Codable, Equatable {
    var name: String
    var value: String
}

private struct SafeSpacePublishedPort: Codable, Equatable {
    var hostPort: Int
    var containerPort: Int
}

private struct SafeSpaceRecipeUser: Codable, Equatable {
    var id: UUID
    var name: String
    var homeDirectory: String
    var workingDirectory: String
}

private struct SafeSpaceRecipeLauncher: Codable {
    var id: UUID
    var kind: String
    var displayName: String
    var workingDirectory: String
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

private struct SafeSpaceRecipeStep: Codable, Equatable {
    var id: UUID
    var command: String
    var createdAt: Date
    var catalogItemID: String?
    var displayName: String?
    var dockerfileFragment: String?
    var isEditable: Bool?
}

private struct SafeSpaceRecipeCatalogItem {
    enum Kind: String {
        case bundledApp
    }

    let id: String
    let displayName: String
    let summary: String
    let kind: Kind
    let serviceID: String
    let command: String
    let liveCommand: String?
    let dockerfileFragment: String?
    let isEditable: Bool
    let bundledPayloadName: String?
    let ociImageReference: String?
    let ociImageBuildCommand: String?
}

private struct SafeSpaceEditableRecipeStep {
    let relativePath: String
    let scope: String
    let user: SafeSpaceRecipeUser?
    let fileName: String
    let contents: String
}

private struct SafeSpaceCachedApp: Codable {
    let frontendID: String
    let serviceID: String
    let displayName: String
    let socketPath: String
    let url: String
    let iconPath: String
    var iconData: Data?
    let iconObservationToken: String?
    var listName: String
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
    let listName: String
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
    case safeSpaceAppNotFound
    case unsupportedProvider
    case commandFailed(String)

    var errorDescription: String? {
        switch self {
        case .invalidRequest:
            return "The container request is invalid."
        case .safeSpaceNotFound:
            return "The selected container no longer exists."
        case .safeSpaceAppNotFound:
            return "The selected container app no longer exists."
        case .unsupportedProvider:
            return "The selected container provider is unavailable."
        case .commandFailed(let message):
            return message
        }
    }
}

final class SafeSpaceManager: @unchecked Sendable {
    static let shared = SafeSpaceManager()

    private let lock = NSLock()
    private let containerSystemRecoveryLock = NSLock()
    private let containerBaseImageLock = NSLock()
    private var records: [SafeSpaceRecord] = []
    private var cachedApps: [UUID: [SafeSpaceCachedApp]] = [:]
    private var publishedForwards: [String: SafeSpaceSocketRelayForward] = [:]
    private var transientStates: [UUID: String] = [:]
    private var buildProgress: [UUID: SafeSpaceBuildProgress] = [:]
    private var appIconFetchesInProgress: Set<String> = []
    private var appEventMonitors: [UUID: SafeSpaceAppEventMonitor] = [:]
    private var cachedCommandIcons: [UUID: [String: SafeSpaceCachedCommandIcon]] = [:]

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
        do {
            records = try loadRecords()
            cachedApps = try loadCachedApps()
        } catch {
            NSLog("Container catalog load failed: %@", error.localizedDescription)
        }
    }

    func handle(_ data: Data) async -> (status: Int, data: Data) {
        let requestID: String
        do {
            guard let request = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let operation = request["operation"] as? String else {
                throw SafeSpaceManagerError.invalidRequest
            }
            requestID = request["requestID"] as? String ?? UUID().uuidString
            let extra = try await perform(operation: operation, request: request)
            if operationChangesMenuBarState(operation) {
                notifyOuterShellSafeSpacesChanged()
            }
            return (200, try await response(requestID: requestID, extra: extra))
        } catch {
            let fallbackRequestID = (try? JSONSerialization.jsonObject(with: data))
                .flatMap { $0 as? [String: Any] }?["requestID"] as? String ?? UUID().uuidString
            return (200, errorResponse(requestID: fallbackRequestID, error: error))
        }
    }

    private func operationChangesMenuBarState(_ operation: String) -> Bool {
        switch operation {
        case "create", "duplicate", "changeRuntime", "rename", "start", "stop", "delete",
             "startApp", "stopApp", "restartApp", "rebuildRecipe":
            return true
        default:
            return false
        }
    }

    private func errorResponse(requestID: String, error: Error) -> Data {
        let body: [String: Any] = [
            "requestID": requestID,
            "providers": providerDictionaries(),
            "workspaces": [],
            "error": error.localizedDescription
        ]
        return (try? JSONSerialization.data(withJSONObject: body)) ?? Data()
    }

    func storeDiscoveredIcon(token: String, data: Data) throws {
        guard !data.isEmpty,
              data.count <= 48 * 1024,
              let image = NSBitmapImageRep(data: data),
              image.pixelsWide > 0,
              image.pixelsHigh > 0,
              image.pixelsWide <= 4096,
              image.pixelsHigh <= 4096 else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let found = lock.withSafeSpaceLock { () -> Bool in
            for safeSpaceID in cachedApps.keys {
                guard let index = cachedApps[safeSpaceID]?.firstIndex(where: {
                    $0.iconObservationToken == token
                }) else {
                    continue
                }
                cachedApps[safeSpaceID]?[index].iconData = data
                return true
            }
            return false
        }
        guard found else {
            throw SafeSpaceManagerError.safeSpaceAppNotFound
        }
        try saveCachedApps()
    }

    func menuBarContainers() async -> [SafeSpaceMenuBarContainer] {
        let currentRecords = lock.withSafeSpaceLock { records }
            .sorted { $0.createdAt < $1.createdAt }
        var containers: [SafeSpaceMenuBarContainer] = []
        for record in currentRecords {
            guard (try? runtimeState(record)) == "running" else { continue }
            let snapshots: [SafeSpaceAppSnapshot]
            do {
                snapshots = try await appSnapshots(for: record)
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
        case "create":
            try create(request)
        case "duplicate":
            try duplicate(request)
        case "changeRuntime":
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
            try await mountFolder(request)
        case "unmountFolder":
            try await unmountFolder(request)
        case "chooseFolder":
            return ["selectedFolderPath": await chooseFolder() ?? ""]
        case "setAppList":
            try setAppList(request)
        case "startApp", "stopApp", "restartApp":
            try await controlApp(request, operation: operation)
        case "appLogs":
            return ["appLog": try await appLog(request)]
        case "addRecipeStep":
            return try addRecipeStep(request)
        case "updateDockerfile":
            return try updateDockerfile(request)
        case "updateContainerConfiguration":
            return try updateContainerConfiguration(request)
        case "updateRecipeBaseImage":
            return try updateRecipeBaseImage(request)
        case "addRecipeUser":
            return try addRecipeUser(request)
        case "createRecipeScript":
            return try createRecipeScript(request)
        case "renameRecipeScript":
            return try renameRecipeScript(request)
        case "updateRecipeStep":
            return try updateRecipeStep(request)
        case "installRecipeCatalogItem":
            return try installRecipeCatalogItem(request)
        case "deleteRecipeStep":
            return try await deleteRecipeStep(request)
        case "rebuildRecipe":
            return try beginRebuildRecipe(request)
        case "publishSocket":
            let record = try record(from: request)
            guard let socketPath = request["socketPath"] as? String else {
                throw SafeSpaceManagerError.invalidRequest
            }
            return ["publishedSocketPath": try await publishSocket(in: record,
                                                                   socketPath: socketPath)]
        default:
            throw SafeSpaceManagerError.invalidRequest
        }
        return [:]
    }

    private func response(requestID: String,
                          extra: [String: Any] = [:]) async throws -> Data {
        var body: [String: Any] = [
            "requestID": requestID,
            "providers": providerDictionaries(),
            "workspaces": try await safeSpaceDictionaries()
        ]
        for (key, value) in extra {
            body[key] = value
        }
        return try JSONSerialization.data(withJSONObject: body)
    }

    private func providerDictionaries() -> [[String: Any]] {
        return [
            [
                "id": "apple.container",
                "name": "Apple container",
                "detail": "A portable OCI container running locally in its own lightweight virtual machine.",
                "defaultBaseImage": rootContainerBaseImage,
                "isolationName": "",
                "isAvailable": runtimeExecutableURL(for: .appleContainer) != nil,
                "capabilities": [
                    "supportsApps": true,
                    "supportsShell": true,
                    "supportsLiveMounts": false,
                    "supportsMounts": true,
                    "supportsRecipes": true
                ]
            ],
            [
                "id": "docker",
                "name": "Docker",
                "detail": "A broadly compatible OCI runtime with live host file notifications.",
                "defaultBaseImage": rootContainerBaseImage,
                "isolationName": "",
                "isAvailable": dockerExecutableURL() != nil,
                "capabilities": [
                    "supportsApps": true,
                    "supportsShell": true,
                    "supportsLiveMounts": true,
                    "supportsMounts": true,
                    "supportsRecipes": true
                ]
            ]
        ]
    }

    private func safeSpaceDictionaries() async throws -> [[String: Any]] {
        let currentRecords = lock.withSafeSpaceLock { records }
        var values: [[String: Any]] = []
        for record in currentRecords.sorted(by: {
            $0.createdAt < $1.createdAt
        }) {
            let transientState = lock.withSafeSpaceLock { transientStates[record.id] }
            let state: String
            if let transientState {
                state = transientState
            } else {
                do {
                    state = try runtimeState(record)
                } catch SafeSpaceManagerError.unsupportedProvider {
                    state = "unavailable"
                }
            }
            let recipe = try recipe(for: record)
            let outerShellSupport = outerShellSupportDictionary(
                for: record,
                state: state,
                recipe: recipe
            )
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
                    appDictionary($0, running: state == "running" && supportsRunningApps)
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
            let base = try safeSpaceDirectory(record.id)
            var mounts: [[String: Any]] = [[
                "id": UUID(uuid: (0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 2)).uuidString,
                "name": "Container project",
                "hostPath": try recipeDirectory(record.id).path,
                "guestPath": "/var/lib/outershell/project",
                "isReadOnly": false,
                "isInfrastructure": true,
                "isRecipeMount": true
            ]]
            for user in recipe.users ?? [] {
                if user.name == "workspace" {
                    mounts.append([
                        "id": UUID(uuid: (0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 1)).uuidString,
                        "name": "Workspace",
                        "hostPath": base.appendingPathComponent("Workspace").path,
                        "guestPath": user.workingDirectory,
                        "isReadOnly": false,
                        "isInfrastructure": true,
                        "isRecipeMount": false
                    ])
                }
            }
            mounts.append(contentsOf: (recipe.mounts ?? []).map {
                [
                    "id": $0.id.uuidString,
                    "name": $0.name,
                    "hostPath": $0.hostPath,
                    "guestPath": $0.guestPath,
                    "isReadOnly": $0.isReadOnly,
                    "isInfrastructure": false,
                    "isRecipeMount": false
                ] as [String: Any]
            })
            let persistentData = try (recipe.persistentData ?? [])
                .filter(\.isDeclared)
                .sorted { $0.guestPath.localizedStandardCompare($1.guestPath) == .orderedAscending }
                .map { item in
                    [
                        "id": item.id.uuidString,
                        "guestPath": item.guestPath,
                        "hostPath": try persistentDataDirectory(record, item: item).path
                    ] as [String: Any]
                }
            var value: [String: Any] = [
                "id": record.id.uuidString,
                "name": record.name,
                "state": state,
                "cpus": record.cpus,
                "memoryInGB": record.memoryInGB,
                "runtimeKind": runtimeProvider(for: record).runtimeKind,
                "supportsLiveMounts": runtimeProvider(for: record).supportsLiveMounts,
                "runtime": try runtimeDictionary(for: record),
                "capabilities": capabilityDictionary(for: record),
                "outerShellSupport": outerShellSupport,
                "shellCommand": try shellCommand(for: record),
                "apps": apps,
                "commands": commands,
                "mounts": mounts,
                "persistentData": persistentData
            ]
            value["recipe"] = try recipeDictionary(for: record)
            if let progress = lock.withSafeSpaceLock({ buildProgress[record.id] }) {
                value["buildProgress"] = progress.dictionary
            }
            values.append(value)
        }
        try saveCachedApps()
        return values
    }

    private func runtimeDictionary(for record: SafeSpaceRecord) throws -> [String: Any] {
        let provider = runtimeProvider(for: record)
        let baseImage = try recipe(for: record).baseImage
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

    private func shellCommand(for record: SafeSpaceRecord) throws -> String {
        let shell = usesBuiltInOuterShellImage(try recipe(for: record).baseImage)
            ? "/bin/bash"
            : "/bin/sh"
        let executable = runtimeProvider(for: record) == .docker ? "docker" : "container"
        return "\(executable) exec -it \(containerName(record.id)) \(shell)"
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
            runtimeExecutable, "exec", "-it", "--user", command.user,
            containerName(record.id), "/bin/sh", "-lc", invocation
        ]
        var value: [String: Any] = [
            "id": command.id,
            "displayName": command.displayName,
            "shellCommand": hostArguments.map(shellCommandArgument).joined(separator: " "),
            "iconPath": command.iconPath
        ]
        if let iconData = commandIconData(command, for: record) {
            value["iconData"] = iconData.base64EncodedString()
        }
        return value
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
        if usesBuiltInOuterShellImage(baseImage) || lowercased.contains("debian") {
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
        return [
            "supportsApps": true,
            "supportsShell": true,
            "supportsLiveMounts": provider.supportsLiveMounts,
            "supportsMounts": true,
            "supportsRecipes": true
        ]
    }

    private func outerShellSupportDictionary(
        for record: SafeSpaceRecord,
        state: String,
        recipe: SafeSpaceRecipe
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
        let requestedBaseImage = try (request["baseImage"] as? String).map {
            try validatedBaseImage($0)
        }
        let requestedSupportInstall = request["installsOuterShellSupport"] as? Bool
        let record = SafeSpaceRecord(id: UUID(),
                                     name: name,
                                     createdAt: Date(),
                                     cpus: max(request["cpus"] as? Int ?? 4, 1),
                                     memoryInGB: max(request["memoryInGB"] as? Int ?? 8, 1),
                                     runtimeProviderID: provider.rawValue)
        try createManagedDirectories(for: record)
        lock.withSafeSpaceLock {
            records.append(record)
            transientStates[record.id] = "creating"
        }
        try saveRecords()
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                if let requestedBaseImage {
                    try self.saveRecipe(
                        self.newRecipe(baseImage: requestedBaseImage,
                                       installsOuterShellSupport: requestedSupportInstall ??
                                           !self.usesBuiltInOuterShellImage(requestedBaseImage),
                                       hasUntrackedChanges: false),
                        for: record
                    )
                } else {
                    _ = try self.recipe(for: record)
                }
                let imageReference = self.recipeImageReference(record.id)
                _ = try self.runContainerBuild(record,
                                               imageReference: imageReference)
                try self.createRuntimeContainer(record, imageReference: imageReference)
                try self.markRecipeRealized(for: record)
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
        let duplicate = SafeSpaceRecord(
            id: UUID(),
            name: name,
            createdAt: Date(),
            cpus: source.cpus,
            memoryInGB: source.memoryInGB,
            runtimeProviderID: provider.rawValue
        )
        try createManagedDirectories(for: duplicate)
        let sourceRecipe = try recipeDirectory(source.id)
        let destinationRecipe = try recipeDirectory(duplicate.id)
        if FileManager.default.fileExists(atPath: destinationRecipe.path) {
            try FileManager.default.removeItem(at: destinationRecipe)
        }
        try FileManager.default.copyItem(at: sourceRecipe, to: destinationRecipe)
        var duplicatedRecipe = try recipe(for: source)
        duplicatedRecipe.realizedBaseImage = nil
        duplicatedRecipe.realizedInstallsOuterShellSupport = nil
        duplicatedRecipe.realizedStepIDs = []
        duplicatedRecipe.realizedLauncherIDs = []
        duplicatedRecipe.realizedEditableStepContents = nil
        duplicatedRecipe.realizedDockerfileContents = nil
        duplicatedRecipe.realizedMounts = nil
        duplicatedRecipe.realizedEnvironment = nil
        duplicatedRecipe.realizedPublishedPorts = nil
        duplicatedRecipe.hasUntrackedChanges = false
        try saveRecipe(duplicatedRecipe, for: duplicate)
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
                for item in duplicatedRecipe.persistentData ?? [] where item.isDeclared {
                    let sourceDirectory = try self.persistentDataDirectory(source, item: item)
                    guard FileManager.default.fileExists(atPath: sourceDirectory.path) else {
                        continue
                    }
                    let destinationDirectory = try self.persistentDataDirectory(duplicate,
                                                                                 item: item)
                    if FileManager.default.fileExists(atPath: destinationDirectory.path) {
                        try FileManager.default.removeItem(at: destinationDirectory)
                    }
                    try FileManager.default.copyItem(at: sourceDirectory,
                                                     to: destinationDirectory)
                }
                let imageReference = self.recipeImageReference(duplicate.id)
                _ = try self.runContainerBuild(duplicate, imageReference: imageReference)
                try self.createRuntimeContainer(duplicate, imageReference: imageReference)
                try self.markRecipeRealized(for: duplicate)
                self.setTransientState(nil, for: duplicate.id)
            } catch {
                NSLog("Container duplication failed: %@", error.localizedDescription)
                self.setTransientState("error", for: duplicate.id)
            }
        }
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

        do {
            let staleTarget = try removeRuntimeContainer(target, force: true)
            if staleTarget.status != 0 && !runtimeObjectIsMissing(staleTarget) {
                try requireSuccess(staleTarget, action: "remove a previous target container")
            }

            let imageReference = recipeImageReference(source.id)
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
            try markRecipeRealized(for: target)
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
            if targetWasCreated {
                _ = try? removeRuntimeContainer(target, force: true)
            }
            if sourceWasRunning && sourceWasStopped {
                _ = try? runRuntime(source, ["start", containerName(source.id)])
            }
            throw error
        }
    }

    private func createRuntimeContainer(_ record: SafeSpaceRecord,
                                        imageReference: String) throws {
        switch runtimeProvider(for: record) {
        case .appleContainer:
            try createAppleContainer(record, imageReference: imageReference)
        case .docker:
            try createDockerContainer(record, imageReference: imageReference)
        }
    }

    private func createAppleContainer(_ record: SafeSpaceRecord,
                                      imageReference: String) throws {
        try createManagedDirectories(for: record)
        let base = try safeSpaceDirectory(record.id)
        let recipe = try recipe(for: record)
        var arguments = [
            "create",
            "--name", containerName(record.id),
            "--cpus", String(record.cpus),
            "--memory", "\(record.memoryInGB)G",
            "--init",
            "--label", "org.outershell.safe-space=\(record.id.uuidString)",
            "--label", "dev.outergroup.outerloop.workspace=\(record.id.uuidString)",
            "--volume", "\(base.appendingPathComponent("Runtime/PiAgent").path):/var/lib/outershell/pi-agent",
            "--volume", "\(try recipeDirectory(record.id).path):/var/lib/outershell/project"
        ]
        for user in recipe.users ?? [] {
            if user.name == "workspace" {
                try createLegacyWorkspaceDirectory(for: record)
                arguments.append(contentsOf: [
                    "--volume", "\(base.appendingPathComponent("Workspace").path):\(user.workingDirectory)"
                ])
            }
        }
        for mount in recipe.mounts ?? [] {
            let suffix = mount.isReadOnly ? ":ro" : ""
            arguments.append(contentsOf: [
                "--volume", "\(mount.hostPath):\(mount.guestPath)\(suffix)"
            ])
        }
        let persistentData = try reconcilePersistentData(
            for: record,
            imageReference: imageReference
        )
        var occupiedGuestPaths: Set<String> = [
            "/var/lib/outershell/pi-agent",
            "/var/lib/outershell/project"
        ]
        occupiedGuestPaths.formUnion((recipe.mounts ?? []).map(\.guestPath))
        occupiedGuestPaths.formUnion((recipe.users ?? []).compactMap { user in
            user.name == "workspace" ? user.workingDirectory : nil
        })
        for item in persistentData where !occupiedGuestPaths.contains(item.guestPath) {
            let directory = try persistentDataDirectory(record, item: item)
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            arguments.append(contentsOf: [
                "--volume", "\(directory.path):\(item.guestPath)"
            ])
        }
        for variable in recipe.environment ?? [] {
            arguments.append(contentsOf: [
                "--env", "\(variable.name)=\(variable.value)"
            ])
        }
        for port in recipe.publishedPorts ?? [] {
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
        let base = try safeSpaceDirectory(record.id)
        let recipe = try recipe(for: record)
        var arguments = [
            "create",
            "--name", containerName(record.id),
            "--cpus", String(record.cpus),
            "--memory", "\(record.memoryInGB)g",
            "--init",
            "--label", "org.outershell.safe-space=\(record.id.uuidString)",
            "--label", "dev.outergroup.outerloop.workspace=\(record.id.uuidString)",
            "--volume", "\(base.appendingPathComponent("Runtime/PiAgent").path):/var/lib/outershell/pi-agent",
            "--volume", "\(try recipeDirectory(record.id).path):/var/lib/outershell/project"
        ]
        for user in recipe.users ?? [] where user.name == "workspace" {
            try createLegacyWorkspaceDirectory(for: record)
            arguments.append(contentsOf: [
                "--volume", "\(base.appendingPathComponent("Workspace").path):\(user.workingDirectory)"
            ])
        }
        for mount in recipe.mounts ?? [] {
            arguments.append(contentsOf: [
                "--volume", "\(mount.hostPath):\(mount.guestPath)\(mount.isReadOnly ? ":ro" : "")"
            ])
        }
        let persistentData = try reconcilePersistentData(
            for: record,
            imageReference: imageReference
        )
        var occupiedGuestPaths: Set<String> = [
            "/var/lib/outershell/pi-agent", "/var/lib/outershell/project"
        ]
        occupiedGuestPaths.formUnion((recipe.mounts ?? []).map(\.guestPath))
        occupiedGuestPaths.formUnion((recipe.users ?? []).compactMap {
            $0.name == "workspace" ? $0.workingDirectory : nil
        })
        for item in persistentData where !occupiedGuestPaths.contains(item.guestPath) {
            let directory = try persistentDataDirectory(record, item: item)
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            arguments.append(contentsOf: ["--volume", "\(directory.path):\(item.guestPath)"])
        }
        for variable in recipe.environment ?? [] {
            arguments.append(contentsOf: ["--env", "\(variable.name)=\(variable.value)"])
        }
        for port in recipe.publishedPorts ?? [] {
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
        let value = normalizedRecipe(try recipe(for: record))
        guard value.version == currentSafeSpaceRecipeVersion,
              value.realizedDockerfileContents == (try? dockerfileContents(for: record)) else {
            return value.baseImage
        }
        return recipeImageReference(record.id)
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
            let imageReference = recipeImageReference(record.id)
            _ = try runContainerBuild(record, imageReference: imageReference)
            try createRuntimeContainer(record, imageReference: imageReference)
            try markRecipeRealized(for: record)
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

    private func delete(_ record: SafeSpaceRecord) async throws {
        cancelAppEventMonitor(for: record.id)
        closePublishedForwards(record.id)
        let result = try removeRuntimeContainer(record, force: true)
        if result.status != 0 && !runtimeObjectIsMissing(result) {
            try requireSuccess(result, action: "delete the container")
        }
        lock.withSafeSpaceLock {
            records.removeAll { $0.id == record.id }
            cachedApps.removeValue(forKey: record.id)
            cachedCommandIcons.removeValue(forKey: record.id)
            transientStates.removeValue(forKey: record.id)
        }
        try saveRecords()
        try saveCachedApps()
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
        var value = try recipe(for: selected)
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
        try saveRecipe(value, for: selected)
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
        var value = try recipe(for: selected)
        if value.realizedMounts == nil {
            value.realizedMounts = value.mounts ?? []
        }
        value.mounts = (value.mounts ?? []).filter { $0.id != mountID }
        try saveRecipe(value, for: selected)
    }

    private func setAppList(_ request: [String: Any]) throws {
        let selected = try record(from: request)
        guard let frontendID = request["frontendID"] as? String,
              let listName = request["listName"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        lock.withSafeSpaceLock {
            guard var apps = cachedApps[selected.id],
                  let index = apps.firstIndex(where: { $0.frontendID == frontendID }) else {
                return
            }
            apps[index].listName = listName
            cachedApps[selected.id] = apps
        }
        try saveCachedApps()
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
        guard let serviceID = request["serviceID"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let identity = try runtimeIdentity(for: selected)
        let command = "/bin/cat \(identity.appsDirectory)/\(shellQuoted(serviceID))/backend.log 2>/dev/null || true"
        let result = try runRuntime(selected, [
            "exec", "--user", identity.user, containerName(selected.id),
            "/bin/sh", "-c", command
        ])
        try requireSuccess(result, action: "read the container app log")
        let contents = result.stdout
        return [
            "path": "\(serviceID) backend log",
            "contents": contents,
            "isTruncated": false,
            "fileSize": contents.utf8.count,
            "modified": Date().timeIntervalSince1970,
            "error": ""
        ]
    }

    private func addRecipeStep(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let rawCommand = request["command"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let command = rawCommand.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !command.isEmpty else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var value = try recipe(for: selected)
        let step = SafeSpaceRecipeStep(id: UUID(),
                                       command: "",
                                       createdAt: Date(),
                                       catalogItemID: nil,
                                       displayName: "Dockerfile fragment",
                                       dockerfileFragment: command,
                                       isEditable: true)
        value.steps.append(step)
        try appendDockerfileInstructions(command, for: selected)
        try saveRecipe(value, for: selected)
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
    }

    private func updateDockerfile(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let contents = request["command"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        try validateDockerfile(contents)
        try contents.write(to: try dockerfileURL(selected.id),
                           atomically: true,
                           encoding: .utf8)
        try saveRecipe(try recipe(for: selected), for: selected)
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
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
        var value = try recipe(for: selected)
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
        try saveRecipe(value, for: selected)
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
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

    private func updateRecipeBaseImage(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let rawBaseImage = request["baseImage"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let baseImage = try validatedBaseImage(rawBaseImage)
        guard let installsOuterShellSupport = request["installsOuterShellSupport"] as? Bool else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var value = try recipe(for: selected)
        value.baseImage = baseImage
        value.installsOuterShellSupport = usesBuiltInOuterShellImage(baseImage)
            ? false
            : installsOuterShellSupport
        value.realizedStepIDs = []
        value.realizedLauncherIDs = []
        value.realizedEditableStepContents = [:]
        try saveRecipe(value, for: selected)
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
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

    private func addRecipeUser(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let rawName = request["name"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let characters = Array(name)
        guard !characters.isEmpty,
              characters.count <= 32,
              (characters.first?.isLowercase == true || characters.first == "_"),
              characters.dropFirst().allSatisfy({
                  $0.isLowercase || $0.isNumber || $0 == "_" || $0 == "-"
              }),
              name != "root" else {
            throw SafeSpaceManagerError.commandFailed(
                "Use a Linux username containing lowercase letters, numbers, hyphens, or underscores."
            )
        }
        var value = try recipe(for: selected)
        guard !(value.users ?? []).contains(where: { $0.name == name }) else {
            throw SafeSpaceManagerError.commandFailed(
                "The container recipe already includes the user \(name)."
            )
        }
        let homeDirectory = "/home/\(name)"
        value.users = (value.users ?? []) + [
            SafeSpaceRecipeUser(
                id: UUID(),
                name: name,
                homeDirectory: homeDirectory,
                workingDirectory: homeDirectory
            )
        ]
        try saveRecipe(value, for: selected)
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
    }

    private func createRecipeScript(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        let value = try recipe(for: selected)
        let fileName = try validatedRecipeScriptName(request["name"] as? String)
        let directory: URL
        if let userIDText = request["recipeUserID"] as? String,
           !userIDText.isEmpty {
            guard let userID = UUID(uuidString: userIDText),
                  let user = (value.users ?? []).first(where: { $0.id == userID }) else {
                throw SafeSpaceManagerError.invalidRequest
            }
            directory = try userRecipeStepDirectory(for: selected, user: user)
        } else {
            directory = try rootRecipeStepDirectory(for: selected)
        }
        try FileManager.default.createDirectory(
            at: directory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        let destination = directory.appendingPathComponent(fileName)
        guard !FileManager.default.fileExists(atPath: destination.path) else {
            throw SafeSpaceManagerError.commandFailed(
                "A setup script named \(fileName) already exists in this section."
            )
        }
        try "#!/bin/sh\nset -eu\n\n".write(
            to: destination,
            atomically: true,
            encoding: .utf8
        )
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: destination.path
        )
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
    }

    private func renameRecipeScript(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let relativePath = request["recipeScriptPath"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let value = try recipe(for: selected)
        guard let script = try editableRecipeSteps(
            for: selected,
            users: value.users ?? []
        ).first(where: { $0.relativePath == relativePath }) else {
            throw SafeSpaceManagerError.commandFailed(
                "The setup script no longer exists."
            )
        }
        let fileName = try validatedRecipeScriptName(request["name"] as? String)
        guard fileName != script.fileName else {
            return ["recipeCommandOutput": "", "recipeCommandApplied": false]
        }
        let directory: URL
        if let user = script.user {
            directory = try userRecipeStepDirectory(for: selected, user: user)
        } else {
            directory = try rootRecipeStepDirectory(for: selected)
        }
        let source = directory.appendingPathComponent(script.fileName)
        let destination = directory.appendingPathComponent(fileName)
        guard !FileManager.default.fileExists(atPath: destination.path) else {
            throw SafeSpaceManagerError.commandFailed(
                "A setup script named \(fileName) already exists in this section."
            )
        }
        try FileManager.default.moveItem(at: source, to: destination)
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
    }

    private func validatedRecipeScriptName(_ rawName: String?) throws -> String {
        guard var fileName = rawName?.trimmingCharacters(in: .whitespacesAndNewlines),
              !fileName.isEmpty else {
            throw SafeSpaceManagerError.invalidRequest
        }
        if !fileName.hasSuffix(".sh") {
            fileName += ".sh"
        }
        guard fileName.count <= 128,
              fileName != ".sh",
              fileName.unicodeScalars.allSatisfy({
                  CharacterSet.alphanumerics.contains($0) ||
                      "._-".unicodeScalars.contains($0)
              }) else {
            throw SafeSpaceManagerError.commandFailed(
                "Setup-script names may contain only letters, numbers, dots, hyphens, and underscores."
            )
        }
        return fileName
    }

    private func updateRecipeStep(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let stepText = request["recipeStepID"] as? String,
              let stepID = UUID(uuidString: stepText),
              let rawFragment = request["command"] as? String else {
            throw SafeSpaceManagerError.invalidRequest
        }
        let fragment = rawFragment.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !fragment.isEmpty else {
            throw SafeSpaceManagerError.commandFailed(
                "A Dockerfile fragment cannot be empty. Remove it instead."
            )
        }
        var value = try recipe(for: selected)
        guard let index = value.steps.firstIndex(where: { $0.id == stepID }),
              value.steps[index].isEditable == true else {
            throw SafeSpaceManagerError.commandFailed(
                "This Dockerfile fragment is managed by Outer Shell."
            )
        }
        value.steps[index].dockerfileFragment = fragment
        value.realizedStepIDs.removeAll { $0 == stepID }
        try saveRecipe(value, for: selected)
        return ["recipeCommandOutput": "", "recipeCommandApplied": false]
    }

    private func installRecipeCatalogItem(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        guard let catalogItemID = request["catalogItemID"] as? String,
              let item = recipeCatalog().first(where: { $0.id == catalogItemID }) else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var value = try recipe(for: selected)
        let dockerfile = try dockerfileContents(for: selected)
        let fragment = item.dockerfileFragment?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard fragment.isEmpty || !dockerfile.contains(fragment) else {
            throw SafeSpaceManagerError.commandFailed(
                "\(item.displayName) is already in this Dockerfile."
            )
        }
        let wasUpToDate = value.realizedDockerfileContents ==
            dockerfile
        let step = SafeSpaceRecipeStep(id: UUID(),
                                       command: item.command,
                                       createdAt: Date(),
                                       catalogItemID: item.id,
                                       displayName: "Install \(item.displayName)",
                                       dockerfileFragment: item.dockerfileFragment,
                                       isEditable: item.isEditable)
        if !value.steps.contains(where: { $0.catalogItemID == item.id }) {
            value.steps.append(step)
        }
        var instructions = item.dockerfileFragment ?? ""
        if !item.command.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            let stepIndex = value.steps.firstIndex(where: { $0.catalogItemID == item.id }) ?? 0
            let fileName = recipeStepFileName(index: stepIndex, step: step)
            instructions += "\n" + recipeStepInstructions(fileName: fileName)
                .joined(separator: "\n")
        }
        try appendDockerfileInstructions(instructions, for: selected)
        try saveRecipe(value, for: selected)

        return try applyRecipeStepIfPossible(step, to: selected, recipe: &value,
                                             wasUpToDate: wasUpToDate,
                                             liveCommand: item.liveCommand)
    }

    private func applyRecipeStepIfPossible(_ step: SafeSpaceRecipeStep,
                                           to selected: SafeSpaceRecord,
                                           recipe value: inout SafeSpaceRecipe,
                                           wasUpToDate: Bool,
                                           liveCommand: String? = nil) throws -> [String: Any] {
        var output = ""
        var appliedDynamically = false
        if let liveCommand, wasUpToDate, try runtimeState(selected) == "running" {
            let result = try runRuntime(selected, [
                "exec", "--user", "root", containerName(selected.id),
                "/bin/sh", "-lc", "cd / && \(liveCommand)"
            ])
            if result.status == 0 {
                value.realizedStepIDs = value.steps.map(\.id)
                value.realizedDockerfileContents = try dockerfileContents(for: selected)
                try saveRecipe(value, for: selected)
                appliedDynamically = true
                output = result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
            } else {
                output = [result.stderr, result.stdout]
                    .joined(separator: "\n")
                    .trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }
        return [
            "recipeCommandOutput": output,
            "recipeCommandApplied": appliedDynamically
        ]
    }

    private func deleteRecipeStep(_ request: [String: Any]) async throws -> [String: Any] {
        let selected = try record(from: request)
        guard let text = request["recipeStepID"] as? String,
              let stepID = UUID(uuidString: text) else {
            throw SafeSpaceManagerError.invalidRequest
        }
        var value = try recipe(for: selected)
        guard value.steps.contains(where: { $0.id == stepID }) else {
            throw SafeSpaceManagerError.invalidRequest
        }
        value.steps.removeAll { $0.id == stepID }
        try saveRecipe(value, for: selected)
        if request["rebuild"] as? Bool == true {
            return try await rebuild(selected, recipe: value)
        }
        return [:]
    }

    private func beginRebuildRecipe(_ request: [String: Any]) throws -> [String: Any] {
        let selected = try record(from: request)
        let value = try recipe(for: selected)
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
                _ = try await self.rebuild(selected, recipe: value)
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
                         recipe value: SafeSpaceRecipe) async throws -> [String: Any] {
        try saveRecipe(value, for: selected)
        let imageReference = recipeImageReference(selected.id)
        let result = try runContainerBuild(selected, imageReference: imageReference)
        try requireSuccess(result, action: "build the container recipe")

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
        try markRecipeRealized(for: selected)
        return [
            "recipeCommandOutput": result.stdout.trimmingCharacters(
                in: .whitespacesAndNewlines
            ),
            "recipeCommandApplied": true
        ]
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
        if let existing = lock.withSafeSpaceLock({ publishedForwards[key] }),
           !existing.isClosed,
           let path = existing.publishedSocketPath {
            return path
        }
        let socketUser = try socketOwner(in: record, socketPath: normalized) == 0 ? "root" : "workspace"
        try ensureContainerSocketBridge(record)
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
            "/bin/test", "-x", "/usr/local/bin/outer-socket-bridge"
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
            libc=glibc
            for loader in /lib/ld-musl-*.so.1 /lib/libc.musl-*.so.1; do
                if [ -e "${loader}" ]; then libc=musl; break; fi
            done
            /usr/bin/printf '%s/%s' "${libc}" "${architecture}"
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

        let temporaryPath = "/tmp/outershell-outer-socket-bridge"
        let copy = try runRuntime(record, [
            "cp", source.path, "\(containerName(record.id)):\(temporaryPath)"
        ])
        try requireSuccess(copy, action: "copy the container socket bridge")
        let install = try runRuntime(record, [
            "exec", "--user", "root", containerName(record.id),
            "/bin/sh", "-c",
            "/bin/mkdir -p /usr/local/bin && /bin/cp \(temporaryPath) /usr/local/bin/outer-socket-bridge && /bin/chmod 0755 /usr/local/bin/outer-socket-bridge && /bin/rm -f \(temporaryPath)"
        ])
        try requireSuccess(install, action: "install the container socket bridge")
    }

    private func containerSocketBridgeURL(platform: String) -> URL? {
        let candidates = [
            Bundle.main.resourceURL?
                .appendingPathComponent("container-bootstrap", isDirectory: true)
                .appendingPathComponent("bin", isDirectory: true)
                .appendingPathComponent(platform, isDirectory: true)
                .appendingPathComponent("outer-socket-bridge"),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath,
                isDirectory: true)
                .appendingPathComponent("build/run/container-bootstrap", isDirectory: true)
                .appendingPathComponent("bin", isDirectory: true)
                .appendingPathComponent(platform, isDirectory: true)
                .appendingPathComponent("outer-socket-bridge")
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
            ? "/root/.config/outerloop"
            : "/home/workspace/.config/outerloop"
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
        let publishedPorts = Dictionary(uniqueKeysWithValues: (try recipe(for: record).publishedPorts ?? []).map {
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
        var result: [SafeSpaceAppSnapshot] = []
        result.reserveCapacity(snapshots.count)
        for snapshot in snapshots {
            if snapshot.publishedPort != nil {
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
                result.append(SafeSpaceAppSnapshot(
                    frontendID: snapshot.frontendID,
                    serviceID: snapshot.serviceID,
                    displayName: snapshot.displayName,
                    socketPath: snapshot.socketPath,
                    externalSocketPath: publishedPath,
                    url: externalURL,
                    iconPath: snapshot.iconPath,
                    listName: snapshot.listName,
                    isRunning: snapshot.isRunning,
                    publishedPort: snapshot.publishedPort
                ))
            } catch {
                NSLog("Could not publish app endpoint %@ in container %@: %@",
                      snapshot.serviceID,
                      record.name,
                      error.localizedDescription)
                result.append(snapshot)
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
        if !snapshot.listName.isEmpty {
            arguments.append(contentsOf: ["--list", snapshot.listName])
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
                listName: field("list"),
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
            cachedApps[safeSpaceID] = snapshots.map {
                SafeSpaceCachedApp(frontendID: $0.frontendID,
                                   serviceID: $0.serviceID,
                                   displayName: $0.displayName,
                                   socketPath: $0.socketPath,
                                   url: $0.url,
                                   iconPath: $0.iconPath,
                                   iconData: old[$0.frontendID]?.iconData,
                                   iconObservationToken: old[$0.frontendID]?.iconObservationToken
                                       ?? UUID().uuidString,
                                   listName: old[$0.frontendID]?.listName ?? $0.listName,
                                   isRunning: $0.isRunning,
                                   publishedPort: $0.publishedPort)
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
            if [ -n "${size}" ] && [ "${size}" -le 49152 ]; then
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
        data.count <= 48 * 1024,
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
                               running: Bool) -> [String: Any] {
        var value: [String: Any] = [
            "frontendID": app.frontendID,
            "serviceID": app.serviceID,
            "displayName": app.displayName,
            "socketPath": app.socketPath,
            "url": app.url,
            "iconPath": app.iconPath,
            "iconObservationToken": app.iconObservationToken ?? "",
            "listName": app.listName,
            "isRunning": running && (app.isRunning ?? false),
            "publishedPort": app.publishedPort ?? 0
        ]
        if let iconData = app.iconData {
            value["iconData"] = iconData.base64EncodedString()
        }
        return value
    }

    private func runtimeState(_ record: SafeSpaceRecord) throws -> String {
        switch runtimeProvider(for: record) {
        case .appleContainer:
            let result = try runContainer(["list", "--all", "--format", "json"])
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
        let current = try catalogURL()
        if FileManager.default.fileExists(atPath: current.path) {
            return try decodeContainerRecords(Data(contentsOf: current))
        }
        let legacy = try legacyDirectory().appendingPathComponent("workspaces.json")
        guard FileManager.default.fileExists(atPath: legacy.path) else {
            return []
        }
        let imported = try decodeContainerRecords(Data(contentsOf: legacy))
        let encoder = JSONEncoder()
        try encoder.encode(imported).write(to: current, options: .atomic)
        return imported
    }

    private func decodeContainerRecords(_ data: Data) throws -> [SafeSpaceRecord] {
        guard let values = try JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell's container catalog is invalid."
            )
        }
        let containers = values.filter {
            if let providerID = $0["runtimeProviderID"] as? String {
                return SafeSpaceRuntimeProviderID(rawValue: providerID) != nil
            }
            return ($0["runtimeKind"] as? String ?? "appleContainer") == "appleContainer"
        }
        return try JSONDecoder().decode(
            [SafeSpaceRecord].self,
            from: JSONSerialization.data(withJSONObject: containers)
        )
    }

    private func loadCachedApps() throws -> [UUID: [SafeSpaceCachedApp]] {
        let current = try cachedAppsURL()
        let source: URL
        if FileManager.default.fileExists(atPath: current.path) {
            source = current
        } else {
            source = try legacyDirectory().appendingPathComponent("workspace-apps.json")
        }
        guard FileManager.default.fileExists(atPath: source.path) else {
            return [:]
        }
        let decoded = try JSONDecoder().decode([String: [SafeSpaceCachedApp]].self,
                                               from: Data(contentsOf: source))
        return Dictionary(uniqueKeysWithValues: decoded.compactMap { key, value in
            UUID(uuidString: key).map { ($0, value) }
        })
    }

    private func saveRecords() throws {
        let values = lock.withSafeSpaceLock { records }
        try JSONEncoder().encode(values).write(to: catalogURL(), options: .atomic)
    }

    private func saveCachedApps() throws {
        let values = lock.withSafeSpaceLock {
            Dictionary(uniqueKeysWithValues: cachedApps.map {
                ($0.key.uuidString.lowercased(), $0.value)
            })
        }
        try JSONEncoder().encode(values).write(to: cachedAppsURL(), options: .atomic)
    }

    private var legacyWorkspaceRecipeUser: SafeSpaceRecipeUser {
        SafeSpaceRecipeUser(
            id: UUID(uuid: (0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 1, 1)),
            name: "workspace",
            homeDirectory: "/home/workspace",
            workingDirectory: "/home/workspace/Workspace"
        )
    }

    private func recipe(for record: SafeSpaceRecord) throws -> SafeSpaceRecipe {
        let url = try recipeURL(record.id)
        if FileManager.default.fileExists(atPath: url.path) {
            var value = try JSONDecoder().decode(SafeSpaceRecipe.self,
                                                 from: Data(contentsOf: url))
            let storedVersion = value.version
            var needsSave = false
            if value.version < currentSafeSpaceRecipeVersion {
                value.version = currentSafeSpaceRecipeVersion
                value.realizedDockerfileContents = nil
                needsSave = true
            }
            if value.users == nil {
                value.users = storedVersion < 11
                    ? [legacyWorkspaceRecipeUser]
                    : []
                if value.users?.contains(where: { $0.name == "workspace" }) == true {
                    try createLegacyWorkspaceDirectory(for: record)
                }
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
                try saveRecipe(value, for: record)
            }
            return value
        }
        let value = newRecipe(
            baseImage: try rootContainerBaseReference(
                for: runtimeProvider(for: record)
            ),
            installsOuterShellSupport: false,
            hasUntrackedChanges: (try? runtimeState(record)) != "absent"
        )
        try saveRecipe(value, for: record)
        return value
    }

    private func newRecipe(baseImage: String,
                           installsOuterShellSupport: Bool,
                           hasUntrackedChanges: Bool) -> SafeSpaceRecipe {
        SafeSpaceRecipe(version: currentSafeSpaceRecipeVersion,
                        baseImage: baseImage,
                        realizedBaseImage: nil,
                        installsOuterShellSupport: installsOuterShellSupport,
                        realizedInstallsOuterShellSupport: nil,
                        steps: [],
                        realizedStepIDs: [],
                        hasUntrackedChanges: hasUntrackedChanges,
                        mounts: [],
                        launchers: [],
                        realizedLauncherIDs: [],
                        realizedEditableStepContents: [:],
                        users: [],
                        realizedDockerfileContents: nil,
                        environment: [],
                        realizedMounts: nil,
                        realizedEnvironment: nil,
                        publishedPorts: [],
                        realizedPublishedPorts: nil,
                        persistentData: [])
    }

    private func saveRecipe(_ value: SafeSpaceRecipe,
                            for record: SafeSpaceRecord) throws {
        let directory = try recipeDirectory(record.id)
        let normalizedValue = normalizedRecipe(value)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        try createEditableRecipeStepDirectories(
            in: directory,
            users: normalizedValue.users ?? []
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(normalizedValue).write(to: try recipeURL(record.id),
                                                  options: .atomic)
        try writeRecipeBuildContext(normalizedValue, to: directory)
        let dockerfile = try dockerfileURL(record.id)
        if !FileManager.default.fileExists(atPath: dockerfile.path) {
            try generatedDockerfile(for: normalizedValue, record: record).write(
                to: dockerfile,
                atomically: true,
                encoding: .utf8
            )
        }
        try writeContainerProjectGuide(to: directory)
        let legacyContainerfile = directory.appendingPathComponent("Containerfile")
        if FileManager.default.fileExists(atPath: legacyContainerfile.path) {
            try FileManager.default.removeItem(at: legacyContainerfile)
        }
    }

    private func normalizedRecipe(_ value: SafeSpaceRecipe) -> SafeSpaceRecipe {
        let catalog = Dictionary(uniqueKeysWithValues: recipeCatalog().map { ($0.id, $0) })
        var result = value
        if result.baseImage.hasPrefix("outershell/container-base:") {
            result.baseImage = rootContainerBaseImage
        }
        if usesBuiltInOuterShellImage(result.baseImage) {
            result.installsOuterShellSupport = false
        } else if result.installsOuterShellSupport == nil {
            result.installsOuterShellSupport = true
        }
        if result.users == nil {
            result.users = value.version < currentSafeSpaceRecipeVersion
                ? [legacyWorkspaceRecipeUser]
                : []
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
        var changedStepIDs = Set<UUID>()
        result.steps = value.steps.map { step in
            guard let catalogItemID = step.catalogItemID,
                  let item = catalog[catalogItemID] else {
                return step
            }
            var normalizedStep = step
            normalizedStep.command = item.command
            normalizedStep.displayName = "Install \(item.displayName)"
            normalizedStep.dockerfileFragment = item.dockerfileFragment
            normalizedStep.isEditable = item.isEditable
            if normalizedStep != step {
                changedStepIDs.insert(step.id)
            }
            return normalizedStep
        }
        result.realizedStepIDs.removeAll { changedStepIDs.contains($0) }
        return result
    }

    private func markRecipeRealized(for record: SafeSpaceRecord) throws {
        var value = normalizedRecipe(try recipe(for: record))
        value.version = currentSafeSpaceRecipeVersion
        value.realizedBaseImage = value.baseImage
        value.realizedInstallsOuterShellSupport = value.installsOuterShellSupport
        value.realizedStepIDs = value.steps.map(\.id)
        value.realizedLauncherIDs = (value.launchers ?? []).map(\.id)
        value.realizedEditableStepContents = try editableRecipeStepContents(
            for: record,
            users: value.users ?? []
        )
        value.realizedDockerfileContents = try dockerfileContents(for: record)
        value.realizedMounts = value.mounts ?? []
        value.realizedEnvironment = value.environment ?? []
        value.realizedPublishedPorts = value.publishedPorts ?? []
        value.hasUntrackedChanges = false
        try saveRecipe(value, for: record)
    }

    private func recipeDictionary(for record: SafeSpaceRecord) throws -> [String: Any] {
        let value = normalizedRecipe(try recipe(for: record))
        let realizedIDs = Set(value.realizedStepIDs)
        let editableSteps = try editableRecipeSteps(
            for: record,
            users: value.users ?? []
        )
        let dockerfile = try dockerfileContents(for: record)
        let needsRebuild = value.version != currentSafeSpaceRecipeVersion ||
            value.realizedDockerfileContents != dockerfile ||
            value.realizedMounts.map { $0 != (value.mounts ?? []) } == true ||
            value.realizedEnvironment.map { $0 != (value.environment ?? []) } == true ||
            value.realizedPublishedPorts.map { $0 != (value.publishedPorts ?? []) } == true
        return [
            "baseImage": value.baseImage,
            "installsOuterShellSupport": value.installsOuterShellSupport ?? false,
            "outerShellSupportSnippet": outerShellSupportSnippet(),
            "steps": value.steps.enumerated().map { index, step in
                [
                    "id": step.id.uuidString,
                    "command": step.command,
                    "createdAt": step.createdAt.timeIntervalSince1970,
                    "isApplied": realizedIDs.contains(step.id),
                    "catalogItemID": step.catalogItemID ?? "",
                    "displayName": step.displayName ?? "",
                    "dockerfileFragment": dockerfileFragment(for: step, index: index),
                    "isEditable": step.isEditable ?? false
                ]
            },
            "catalog": recipeCatalog().map { item in
                [
                    "id": item.id,
                    "displayName": item.displayName,
                    "summary": item.summary,
                    "kind": item.kind.rawValue,
                    "serviceID": item.serviceID,
                    "isInstalled": item.dockerfileFragment.map {
                        dockerfile.contains(
                            $0.trimmingCharacters(in: .whitespacesAndNewlines)
                        )
                    } ?? false
                ]
            },
            "launchers": (value.launchers ?? []).map {
                [
                    "id": $0.id.uuidString,
                    "kind": $0.kind,
                    "displayName": $0.displayName,
                    "workingDirectory": $0.workingDirectory,
                    "isApplied": Set(value.realizedLauncherIDs ?? []).contains($0.id)
                ] as [String: Any]
            },
            "users": [[
                "id": "root",
                "name": "root",
                "homeDirectory": "/root",
                "workingDirectory": "/",
                "isRoot": true
            ]] + (value.users ?? []).map {
                [
                    "id": $0.id.uuidString,
                    "name": $0.name,
                    "homeDirectory": $0.homeDirectory,
                    "workingDirectory": $0.workingDirectory,
                    "isRoot": false
                ] as [String: Any]
            },
            "scriptFiles": try editableSteps.map { step in
                let hostDirectory: URL
                let guestPath: String
                let userID: String
                if let user = step.user {
                    hostDirectory = try userRecipeStepDirectory(for: record, user: user)
                    guestPath = "/var/lib/outershell/recipe/users/\(user.name)/steps/\(step.fileName)"
                    userID = user.id.uuidString
                } else {
                    hostDirectory = try rootRecipeStepDirectory(for: record)
                    guestPath = "/var/lib/outershell/recipe/root/\(step.fileName)"
                    userID = ""
                }
                return [
                    "relativePath": step.relativePath,
                    "scope": step.scope,
                    "userID": userID,
                    "fileName": step.fileName,
                    "hostPath": hostDirectory.appendingPathComponent(step.fileName).path,
                    "guestPath": guestPath,
                    "isApplied": (value.realizedEditableStepContents ?? [:])[step.relativePath] == step.contents
                ] as [String: Any]
            },
            "fragments": try dockerfileFragmentDictionaries(for: value, record: record),
            "containerfile": dockerfile,
            "dockerfileHostPath": try dockerfileURL(record.id).path,
            "dockerfileGuestPath": "/var/lib/outershell/project/Dockerfile",
            "buildEngineAvailable": runtimeExecutableURL(
                for: runtimeProvider(for: record)
            ) != nil,
            "definitionDirectory": try recipeDirectory(record.id).path,
            "transferItems": transferItems(for: record, recipe: value),
            "needsRebuild": needsRebuild,
            "hasUntrackedChanges": value.hasUntrackedChanges ?? true,
            "workingDirectory": value.users?.first?.workingDirectory ?? "/",
            "environment": (value.environment ?? []).map {
                ["name": $0.name, "value": $0.value]
            },
            "publishedPorts": (value.publishedPorts ?? []).map {
                ["hostPort": $0.hostPort, "containerPort": $0.containerPort]
            }
        ]
    }

    private func transferItems(for record: SafeSpaceRecord,
                               recipe value: SafeSpaceRecipe) -> [[String: Any]] {
        var items = [
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
                "detail": recipeImageReference(record.id),
                "status": "Rebuild or push to a registry"
            ]
        ]
        if value.users?.contains(where: { $0.name == "workspace" }) == true {
            let workspacePath = (try? safeSpaceDirectory(record.id)
                .appendingPathComponent("Workspace", isDirectory: true).path) ?? ""
            items.insert([
                "name": "Workspace files",
                "detail": workspacePath,
                "status": "Transfer separately"
            ], at: 1)
        }
        return items
    }

    private func recipeCatalog() -> [SafeSpaceRecipeCatalogItem] {
        let bundledApps = [
            ("files", "Files", "Browse and manage files in this container.",
             "org.outershell.Files", "Files", "FilesBackend"),
            ("plaintext", "Plaintext", "Read and edit text files.",
             "org.outershell.Plaintext", "Plaintext", "PlaintextBackend"),
            ("top", "Top", "Inspect processes running in this container.",
             "org.outershell.Top", "Top", "TopBackend"),
            ("profile", "Profile", "Profile programs running in this container.",
             "org.outershell.Profile", "Profile", "ProfileBackend"),
            ("firehose", "Firehose", "Inspect streaming system activity.",
             "org.outershell.Firehose", "Firehose", "FirehoseBackend")
        ].map { id, displayName, summary, serviceID, archiveName, binaryName in
            let installCommand = bundledAppRecipeCommand(
                displayName: displayName,
                serviceID: serviceID,
                archiveName: archiveName,
                binaryName: binaryName
            )
            let imageReference = "ghcr.io/outergroup/outershell-app-\(id):\(bundledAppOCIImageVersion)"
            return SafeSpaceRecipeCatalogItem(
                id: "outershell.\(id)",
                displayName: displayName,
                summary: summary,
                kind: .bundledApp,
                serviceID: serviceID,
                command: "",
                liveCommand: recipeAppLiveCommand(
                    command: installCommand,
                    registrationID: serviceID
                ),
                dockerfileFragment: "COPY --from=\(imageReference) /outershell-rootfs/ /",
                isEditable: false,
                bundledPayloadName: nil,
                ociImageReference: imageReference,
                ociImageBuildCommand: installCommand
            )
        }
        return bundledApps + [
            SafeSpaceRecipeCatalogItem(
                id: "outershell.agentdiy",
                displayName: "Container Agent",
                summary: "A root-capable Pi assistant that can install software and build this container recipe.",
                kind: .bundledApp,
                serviceID: "org.outershell.AgentDIY",
                command: "",
                liveCommand: nil,
                dockerfileFragment: "COPY --from=ghcr.io/outergroup/outershell-app-container-agent:\(bundledAppOCIImageVersion) /outershell-rootfs/ /",
                isEditable: false,
                bundledPayloadName: "AgentDIY",
                ociImageReference: "ghcr.io/outergroup/outershell-app-container-agent:\(bundledAppOCIImageVersion)",
                ociImageBuildCommand: agentDIYRecipeCommand()
            )
        ]
    }

    private func recipeAppLiveCommand(command: String,
                                      registrationID: String) -> String {
        """
        \(recipeAppBootstrapCommand())
        \(command)
        \(recipeAppActivationCommand(registrationID: registrationID))
        """
    }

    private func recipeAppBootstrapCommand() -> String {
        """
        /bin/mkdir -p /etc/outershell/apps.d
        /bin/mkdir -p /var/lib/outershell/services /var/lib/outershell/apps
        /bin/mkdir -p /root/.local/share/jupyter /root/.cache
        """
    }

    private func agentDIYRecipeCommand() -> String {
        let serviceID = "org.outershell.AgentDIY"
        let appDirectory = "/opt/outershell/apps/org.outershell.AgentDIY"
        let socketPath = "/run/user/0/org.outershell.agentdiy"
        let serviceFile = "/var/lib/outershell/services/\(serviceID).outerservice"
        let logDirectory = "/var/lib/outershell/apps/\(serviceID)"
        let registration = recipeAppRegistrationCommand(
            displayName: "Container Agent",
            serviceID: serviceID,
            socketPath: socketPath,
            iconPath: "\(appDirectory)/app-icon.png"
        )
        return """
        set -eu
        /usr/bin/apt-get update
        /usr/bin/apt-get install -y --no-install-recommends ca-certificates curl sudo xz-utils
        /bin/rm -rf /var/lib/apt/lists/*
        node_version=22.23.2
        case "$(/usr/bin/dpkg --print-architecture)" in
            arm64)
                node_arch=arm64
                node_sha256=fff4078c5def658577f92c88db7db3bc0072924bfb93fe52c1e744a54e94abb8
                ;;
            amd64)
                node_arch=x64
                node_sha256=d60acfe00a2932254bb0ad20e01b0d74397a0875595de719654b214f4b03f307
                ;;
            *)
                /usr/bin/printf 'Unsupported architecture for Container Agent.\n' >&2
                exit 1
                ;;
        esac
        node_archive="node-v${node_version}-linux-${node_arch}.tar.xz"
        /usr/bin/curl --fail --location --silent --show-error \
            "https://nodejs.org/dist/v${node_version}/${node_archive}" \
            --output "/tmp/${node_archive}"
        /usr/bin/printf '%s  %s\n' "${node_sha256}" "/tmp/${node_archive}" | \
            /usr/bin/sha256sum --check --status
        /bin/tar -xJf "/tmp/${node_archive}" --strip-components=1 -C /usr/local
        /bin/rm -f "/tmp/${node_archive}"
        /usr/local/bin/npm install --global --ignore-scripts \
            @earendil-works/pi-coding-agent@0.84.1

        /bin/cat >/usr/local/bin/outershell-app <<'OUTERSHELL_APP_HELPER'
        #!/bin/sh
        set -eu

        operation=${1:-}
        case "${operation}" in
            publish|register-live) shift ;;
            *)
                /usr/bin/printf '%s\n' \
                    'Usage: outershell-app publish --id ID --name NAME --service-file PATH --socket PATH [--url PATH] [--log PATH] [--icon PATH] [--list NAME]' >&2
                exit 64
                ;;
        esac

        service_id=
        display_name=
        service_file=
        socket_path=
        frontend_url=/
        log_path=
        icon_path=
        list_name=
        while [ "$#" -gt 0 ]; do
            option=$1
            [ "$#" -ge 2 ] || {
                /usr/bin/printf 'Missing value for %s\n' "${option}" >&2
                exit 64
            }
            value=$2
            shift 2
            case "${option}" in
                --id) service_id=${value} ;;
                --name) display_name=${value} ;;
                --service-file) service_file=${value} ;;
                --socket) socket_path=${value} ;;
                --url) frontend_url=${value} ;;
                --log) log_path=${value} ;;
                --icon) icon_path=${value} ;;
                --list) list_name=${value} ;;
                *)
                    /usr/bin/printf 'Unknown option: %s\n' "${option}" >&2
                    exit 64
                    ;;
            esac
        done

        case "${service_id}" in
            ''|*[!A-Za-z0-9._-]*)
                /usr/bin/printf 'The app identifier is invalid.\n' >&2
                exit 64
                ;;
        esac
        [ -n "${display_name}" ] && [ -n "${service_file}" ] && [ -n "${socket_path}" ] || {
            /usr/bin/printf 'The app name, service file, and socket are required.\n' >&2
            exit 64
        }
        case "${service_file}:${socket_path}" in
            *[![:print:]]*|*'\t'*|*'\n'*)
                /usr/bin/printf 'App paths must be single-line printable text.\n' >&2
                exit 64
                ;;
        esac
        case "${service_file}" in /*) ;; *) exit 64 ;; esac
        case "${socket_path}" in /*) ;; *) exit 64 ;; esac

        append_argument() {
            escaped=$(/usr/bin/printf '%s' "$1" | /usr/bin/sed "s/'/'\\\\''/g")
            /usr/bin/printf " '%s'" "${escaped}"
        }

        if [ "${operation}" = publish ]; then
            /bin/mkdir -p /etc/outershell/apps.d
            registration=/etc/outershell/apps.d/${service_id}.sh
            temporary=${registration}.tmp.$$
            {
                /usr/bin/printf '%s\n%s' '#!/bin/sh' \
                    'exec /usr/local/bin/outershell-app register-live'
                append_argument --id
                append_argument "${service_id}"
                append_argument --name
                append_argument "${display_name}"
                append_argument --service-file
                append_argument "${service_file}"
                append_argument --socket
                append_argument "${socket_path}"
                append_argument --url
                append_argument "${frontend_url}"
                if [ -n "${log_path}" ]; then
                    append_argument --log
                    append_argument "${log_path}"
                fi
                if [ -n "${icon_path}" ]; then
                    append_argument --icon
                    append_argument "${icon_path}"
                fi
                if [ -n "${list_name}" ]; then
                    append_argument --list
                    append_argument "${list_name}"
                fi
                /usr/bin/printf '\n'
            } >"${temporary}"
            /bin/chmod 0755 "${temporary}"
            /bin/mv -f "${temporary}" "${registration}"
            [ -S /run/user/0/outershelld-api ] || exit 0
        fi

        export HOME=/root
        export USER=root
        export LOGNAME=root
        export XDG_RUNTIME_DIR=/run/user/0
        export OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
        outerctl=/usr/local/bin/outerctl
        "${outerctl}" backend upsert \
            --backend "${service_id}" \
            --name "${display_name}" \
            --service-file "${service_file}" \
            --outershell-owns true
        set -- app upsert \
            --backend "${service_id}" \
            --frontend-id "${service_id}:main" \
            --socket-path "${socket_path}" \
            --name "${display_name}" \
            --url "${frontend_url}"
        if [ -n "${icon_path}" ]; then set -- "$@" --icon-path "${icon_path}"; fi
        if [ -n "${list_name}" ]; then set -- "$@" --list "${list_name}"; fi
        "${outerctl}" "$@"
        if [ -n "${log_path}" ]; then
            "${outerctl}" log add --backend "${service_id}" --path "${log_path}"
        fi
        OUTERSHELL_APP_HELPER
        /bin/chmod 0755 /usr/local/bin/outershell-app

        /bin/chmod 0755 \(appDirectory)/AgentDIYBackend
        /bin/mkdir -p \(logDirectory) "$(/usr/bin/dirname \(serviceFile))"
        /bin/cat >\(appDirectory)/AGENTS.md <<'OUTERSHELL_PI_CONTEXT'
        This container is reproducibly built from the Dockerfile at
        /var/lib/outershell/project/Dockerfile. The containing directory is the OCI
        build context and is mounted read-write from the host for root processes.

        When changing the environment, make the change in the running container and
        add the equivalent portable, idempotent Dockerfile instructions. Edit the
        Dockerfile directly; do not introduce a parallel recipe or setup-script
        system. Do not add host mounts, because those remain host configuration.

        Apps in this container are portable services owned by its outershelld. To add
        an app, create its .outerservice and log from Dockerfile instructions, then
        arrange for `outershell-app publish` during the image build. That command records a startup
        announcement, registers the app immediately through the inner outershelld API,
        and starts services whose .outerservice says `Start=eager`. Run the saved root
        equivalent commands once in the live container so the change appears immediately.
        Never restart or kill outershelld to make it discover an app.

        A JupyterLab launcher should use a distinct stable identifier and Unix socket,
        disable token/password authentication, and set its requested root directory.
        Its recipe step should end with commands equivalent to:

            /usr/local/bin/outershell-app publish \\
                --id org.outershell.JupyterLab.hello \\
                --name 'JupyterLab: hello' \\
                --service-file /var/lib/outershell/services/org.outershell.JupyterLab.hello.outerservice \\
                --socket /run/user/0/org.outershell.JupyterLab.hello \\
                --url /lab \\
                --log /var/lib/outershell/apps/org.outershell.JupyterLab.hello/backend.log

        The container service manager runs as root. Do not create users or switch a
        service to another account unless the user explicitly asks for that design.
        OUTERSHELL_PI_CONTEXT
        /bin/cat >\(appDirectory)/run-as-root <<'OUTERSHELL_AGENTDIY_RUNNER'
        #!/bin/sh
        set -eu
        umask 077
        export HOME=/root
        export USER=root
        export LOGNAME=root
        export PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin
        export PI_CODING_AGENT_DIR=/var/lib/outershell/pi-agent
        /bin/mkdir -p "${PI_CODING_AGENT_DIR}"
        /bin/cp /opt/outershell/apps/org.outershell.AgentDIY/AGENTS.md \
            "${PI_CODING_AGENT_DIR}/AGENTS.md"
        exec /opt/outershell/apps/org.outershell.AgentDIY/AgentDIYBackend \
            --socket /run/user/0/org.outershell.agentdiy \
            --root /opt/outershell/apps/org.outershell.AgentDIY \
            --workspace /root
        OUTERSHELL_AGENTDIY_RUNNER
        /bin/chmod 0755 \(appDirectory)/run-as-root
        /bin/touch \(logDirectory)/backend.log
        /bin/chmod 0644 \(logDirectory)/backend.log
        /bin/cat >\(serviceFile) <<'OUTERSHELL_SERVICE'
        [Service]
        Format=1
        Name=Container Agent
        Executable=/opt/outershell/apps/org.outershell.AgentDIY/run-as-root
        WorkingDirectory=/root
        EnvironmentPolicy=clean
        Start=manual
        Restart=on-failure
        RestartDelayMilliseconds=1000
        StopTimeoutMilliseconds=10000
        LogPath=\(logDirectory)/backend.log
        OUTERSHELL_SERVICE
        /bin/cat >/etc/outershell/apps.d/\(serviceID).sh <<'OUTERSHELL_REGISTRATION'
        \(registration)
        OUTERSHELL_REGISTRATION
        /bin/chmod 0755 /etc/outershell/apps.d/\(serviceID).sh
        """
    }

    private func recipeAppActivationCommand(registrationID: String) -> String {
        """
        if [ -S /run/user/0/outershelld-api ]; then
            /bin/sh /etc/outershell/apps.d/\(registrationID).sh
        fi
        """
    }

    private func recipeAppRegistrationCommand(displayName: String,
                                              serviceID: String,
                                              socketPath: String,
                                              frontendURL: String = "/",
                                              iconPath: String?) -> String {
        let iconOption = iconPath.map {
            " \\" + "\n            --icon-path \($0)"
        } ?? ""
        return """
        #!/bin/sh
        set -eu
        export HOME=/root
        export USER=root
        export LOGNAME=root
        export XDG_RUNTIME_DIR=/run/user/0
        export OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
        outerctl=/usr/local/bin/outerctl
        service=/var/lib/outershell/services/\(serviceID).outerservice
        log=/var/lib/outershell/apps/\(serviceID)/backend.log
        "${outerctl}" backend upsert \
            --backend \(serviceID) \
            --name "\(displayName)" \
            --service-file "${service}" \
            --outershell-owns true
        "${outerctl}" app upsert \
            --backend \(serviceID) \
            --frontend-id \(serviceID):main \
            --socket-path \(socketPath) \
            --name "\(displayName)" \
            --url \(frontendURL)\(iconOption)
        "${outerctl}" log add \
            --backend \(serviceID) \
            --path "${log}"
        """
    }

    private func bundledAppRecipeCommand(displayName: String,
                                         serviceID: String,
                                         archiveName: String,
                                         binaryName: String) -> String {
        let registrationID = serviceID
        let appDirectory = "/var/lib/outershell/apps/\(serviceID)"
        let serviceFile = "/var/lib/outershell/services/\(serviceID).outerservice"
        var registration = recipeAppRegistrationCommand(
            displayName: displayName,
            serviceID: serviceID,
            socketPath: "/run/user/0/\(serviceID)",
            iconPath: "\(appDirectory)/app-icon.png"
        )
        if serviceID == "org.outershell.Plaintext" {
            registration += """

            "${outerctl}" opener upsert \
                --backend org.outershell.Plaintext \
                --content-type public.text \
                --url-template '?file={file}' \
                --rank 0 \
                --capabilities view,edit
            """
        }
        return """
        set -eu
        export DEBIAN_FRONTEND=noninteractive
        /usr/bin/apt-get update
        /usr/bin/apt-get install -y --no-install-recommends ca-certificates curl
        case "$(/usr/bin/uname -m)" in
            aarch64|arm64) platform=linux-aarch64 ; architecture=aarch64 ;;
            x86_64|amd64) platform=linux-x86_64 ; architecture=x86_64 ;;
            *) /usr/bin/printf 'Unsupported architecture.\n' >&2; exit 1 ;;
        esac
        archive=/tmp/\(archiveName).tar.gz
        /usr/bin/curl --fail --location --retry 4 --retry-delay 2 \
            --output "${archive}" \
            "https://outershell.org/outer-shell/dev/apps/\(archiveName)/${platform}.tar.gz"
        /bin/rm -rf \(appDirectory)
        /bin/mkdir -p \(appDirectory) "$(/usr/bin/dirname \(serviceFile))"
        /bin/tar -xzf "${archive}" --strip-components=1 -C \(appDirectory)
        /bin/chmod 0755 \(appDirectory)/RemoteLinuxBinaries/${architecture}/\(binaryName)
        /usr/bin/touch \(appDirectory)/backend.log
        /bin/cat >\(serviceFile) <<OUTERSHELL_SERVICE
        [Service]
        Format=1
        Name=\(displayName)
        Executable=\(appDirectory)/RemoteLinuxBinaries/${architecture}/\(binaryName)
        Argument=--label
        Argument=\(serviceID)
        Argument=--socket-path
        Argument=/run/user/0/\(serviceID)
        Argument=--bundles-dir
        Argument=\(appDirectory)/bundles
        Argument=--icon-file
        Argument=\(appDirectory)/app-icon.png
        WorkingDirectory=\(appDirectory)
        Environment=HOME=/root
        Environment=USER=root
        Environment=LOGNAME=root
        Environment=OUTERSHELL_SERVICE_STATE_DIR=\(appDirectory)
        Environment=OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
        EnvironmentPolicy=clean
        Start=socket
        Restart=on-failure
        LogPath=\(appDirectory)/backend.log

        [Socket.http]
        Type=unix
        Path=/run/user/0/\(serviceID)
        Mode=0600
        Backlog=64
        OUTERSHELL_SERVICE
        /bin/cat >/etc/outershell/apps.d/\(registrationID).sh <<'OUTERSHELL_REGISTRATION'
        \(registration)
        OUTERSHELL_REGISTRATION
        /bin/chmod 0755 /etc/outershell/apps.d/\(registrationID).sh
        /bin/rm -f "${archive}"
        /bin/rm -rf /var/lib/apt/lists/*
        """
    }

    private func jupyterLabDockerfileFragment() -> String {
        """
        RUN apt-get update \\
            && apt-get install -y --no-install-recommends python3 python3-venv \\
            && python3 -m venv /opt/outershell/jupyter \\
            && /opt/outershell/jupyter/bin/pip install --no-cache-dir \\
                jupyterlab \\
            && rm -rf /var/lib/apt/lists/*
        """
    }

    private func jupyterLabInstallCommand() -> String {
        """
        set -eu
        export DEBIAN_FRONTEND=noninteractive
        /usr/bin/apt-get update
        /usr/bin/apt-get install -y --no-install-recommends python3 python3-venv
        /usr/bin/python3 -m venv /opt/outershell/jupyter
        /opt/outershell/jupyter/bin/pip install --no-cache-dir jupyterlab
        /bin/rm -rf /var/lib/apt/lists/*
        """
    }

    private func jupyterLabRecipeCommand() -> String {
        let serviceID = "org.outershell.JupyterLab"
        let appDirectory = "/var/lib/outershell/apps/\(serviceID)"
        let serviceFile = "/var/lib/outershell/services/\(serviceID).outerservice"
        let registration = recipeAppRegistrationCommand(
            displayName: "JupyterLab",
            serviceID: serviceID,
            socketPath: "/run/user/0/\(serviceID)",
            frontendURL: "/lab",
            iconPath: nil
        )
        return """
        set -eu
        /bin/mkdir -p \(appDirectory) "$(/usr/bin/dirname \(serviceFile))"
        /usr/bin/touch \(appDirectory)/backend.log
        /bin/cat >\(serviceFile) <<'OUTERSHELL_SERVICE'
        [Service]
        Format=1
        Name=JupyterLab
        Executable=/opt/outershell/jupyter/bin/jupyter-lab
        Argument=--no-browser
        Argument=--ServerApp.sock=/run/user/0/\(serviceID)
        Argument=--ServerApp.sock_mode=0600
        Argument=--IdentityProvider.token=
        Argument=--ServerApp.password=
        Argument=--ServerApp.root_dir=/root
        WorkingDirectory=/root
        Environment=HOME=/root
        Environment=USER=root
        Environment=LOGNAME=root
        Environment=XDG_RUNTIME_DIR=/run/user/0
        EnvironmentPolicy=clean
        Start=eager
        Restart=on-failure
        RestartDelayMilliseconds=1000
        LogPath=\(appDirectory)/backend.log
        OUTERSHELL_SERVICE
        /bin/cat >/etc/outershell/apps.d/\(serviceID).sh <<'OUTERSHELL_REGISTRATION'
        \(registration)
        OUTERSHELL_REGISTRATION
        /bin/chmod 0755 /etc/outershell/apps.d/\(serviceID).sh
        """
    }

    private func rStudioServerDockerfileFragment() -> String {
        """
        RUN set -eux; \
            apt-get update; \
            apt-get install -y --no-install-recommends ca-certificates curl r-base socat; \
            case "$(dpkg --print-architecture)" in \
                arm64) package_url='https://s3.amazonaws.com/rstudio-ide-build/server/jammy/arm64/rstudio-server-2026.07.0-139-arm64.deb' ;; \
                amd64) package_url='https://download2.rstudio.org/server/jammy/amd64/rstudio-server-2026.07.1-147-amd64.deb' ;; \
                *) echo 'Unsupported architecture' >&2; exit 1 ;; \
            esac; \
            curl --fail --location --retry 4 --retry-delay 2 \
                --output /tmp/rstudio-server.deb "${package_url}"; \
            apt-get install -y --no-install-recommends /tmp/rstudio-server.deb; \
            rm -f /tmp/rstudio-server.deb; \
            rm -rf /var/lib/apt/lists/*
        """
    }

    private func rStudioServerInstallCommand() -> String {
        """
        set -eu
        export DEBIAN_FRONTEND=noninteractive
        /usr/bin/apt-get update
        /usr/bin/apt-get install -y --no-install-recommends ca-certificates curl r-base socat
        case "$(/usr/bin/dpkg --print-architecture)" in
            arm64)
                package_url=https://s3.amazonaws.com/rstudio-ide-build/server/jammy/arm64/rstudio-server-2026.07.0-139-arm64.deb
                ;;
            amd64)
                package_url=https://download2.rstudio.org/server/jammy/amd64/rstudio-server-2026.07.1-147-amd64.deb
                ;;
            *) /usr/bin/printf 'Unsupported architecture.\n' >&2; exit 1 ;;
        esac
        /usr/bin/curl --fail --location --retry 4 --retry-delay 2 \
            --output /tmp/rstudio-server.deb "${package_url}"
        /usr/bin/apt-get install -y --no-install-recommends /tmp/rstudio-server.deb
        /bin/rm -f /tmp/rstudio-server.deb
        /bin/rm -rf /var/lib/apt/lists/*
        """
    }

    private func rStudioServerRecipeCommand() -> String {
        let serviceID = "org.outershell.RStudio"
        let appDirectory = "/var/lib/outershell/apps/\(serviceID)"
        let rootAppDirectory = "/opt/outershell/apps/\(serviceID)"
        let serviceFile = "/var/lib/outershell/services/\(serviceID).outerservice"
        let registration = recipeAppRegistrationCommand(
            displayName: "RStudio Server",
            serviceID: serviceID,
            socketPath: "/run/user/0/\(serviceID)",
            iconPath: nil
        )
        return """
        set -eu
        /usr/sbin/rstudio-server stop >/dev/null 2>&1 || true
        /bin/mkdir -p \(appDirectory) \(rootAppDirectory) "$(/usr/bin/dirname \(serviceFile))"
        /usr/bin/touch \(appDirectory)/backend.log
        /bin/cat >\(rootAppDirectory)/run-rserver-as-root <<'OUTERSHELL_ROOT_RUNNER'
        #!/bin/bash
        set -eu
        exec /usr/bin/env \
            HOME=/root \
            USER=root \
            LOGNAME=root \
            XDG_RUNTIME_DIR=/run/user/0 \
            /usr/lib/rstudio-server/bin/rserver \
            --server-user=root \
            --server-daemonize=0 \
            --server-working-dir=/root \
            --server-data-dir=/var/lib/outershell/apps/\(serviceID)/rstudio-server \
            --server-pid-file=\(appDirectory)/rserver.pid \
            --secure-cookie-key-file=\(appDirectory)/secure-cookie-key \
            --database-config-file=\(appDirectory)/database.conf \
            --www-address=127.0.0.1 \
            --www-port=8787 \
            --www-thread-pool-size=2 \
            --auth-none=1 \
            --auth-minimum-user-id=0
        OUTERSHELL_ROOT_RUNNER
        /bin/chown root:root \(rootAppDirectory)/run-rserver-as-root
        /bin/chmod 0755 \(rootAppDirectory)/run-rserver-as-root
        /bin/cat >\(appDirectory)/start-rstudio-server <<'OUTERSHELL_RUNNER'
        #!/bin/bash
        set -eu
        state_directory=\(appDirectory)
        socket_path=/run/user/0/\(serviceID)
        data_directory=/var/lib/outershell/apps/\(serviceID)/rstudio-server
        database_config="${state_directory}/database.conf"
        database_directory="${state_directory}/database"
        /bin/mkdir -p "${data_directory}" "${database_directory}"
        /bin/chmod 0700 "${data_directory}" "${database_directory}"
        /usr/bin/touch "${database_config}"
        /bin/chmod 0600 "${database_config}"
        /bin/rm -f "${socket_path}"
        rserver_pid=
        proxy_pid=
        cleanup() {
            trap - EXIT INT TERM HUP
            if [ -n "${proxy_pid}" ]; then
                /bin/kill "${proxy_pid}" 2>/dev/null || true
                wait "${proxy_pid}" 2>/dev/null || true
            fi
            if [ -n "${rserver_pid}" ]; then
                /bin/kill "${rserver_pid}" 2>/dev/null || true
                wait "${rserver_pid}" 2>/dev/null || true
            fi
            /bin/rm -f "${socket_path}"
        }
        trap cleanup EXIT
        trap 'exit 0' INT TERM HUP
        \(rootAppDirectory)/run-rserver-as-root &
        rserver_pid=$!
        attempts=0
        until /usr/bin/curl --silent --fail --max-time 1 \
            --output /dev/null http://127.0.0.1:8787/; do
            if ! /bin/kill -0 "${rserver_pid}" 2>/dev/null; then
                wait "${rserver_pid}"
                exit $?
            fi
            attempts=$((attempts + 1))
            [ "${attempts}" -lt 120 ] || exit 1
            /bin/sleep 0.1
        done
        /usr/bin/socat \
            UNIX-LISTEN:"${socket_path}",fork,mode=0600 \
            TCP:127.0.0.1:8787 &
        proxy_pid=$!
        wait -n "${rserver_pid}" "${proxy_pid}"
        OUTERSHELL_RUNNER
        /bin/chmod 0755 \(appDirectory)/start-rstudio-server
        /bin/cat >\(serviceFile) <<'OUTERSHELL_SERVICE'
        [Service]
        Format=1
        Name=RStudio Server
        Executable=\(appDirectory)/start-rstudio-server
        WorkingDirectory=/root
        Environment=HOME=/root
        Environment=USER=root
        Environment=LOGNAME=root
        Environment=XDG_RUNTIME_DIR=/run/user/0
        Environment=OUTERSHELL_SERVICE_STATE_DIR=\(appDirectory)
        EnvironmentPolicy=clean
        Start=eager
        Restart=on-failure
        RestartDelayMilliseconds=1000
        StopTimeoutMilliseconds=10000
        LogPath=\(appDirectory)/backend.log
        OUTERSHELL_SERVICE
        /bin/cat >/etc/outershell/apps.d/\(serviceID).sh <<'OUTERSHELL_REGISTRATION'
        \(registration)
        OUTERSHELL_REGISTRATION
        /bin/chmod 0755 /etc/outershell/apps.d/\(serviceID).sh
        """
    }

    private func jupyterLabLauncherRecipeCommand(
        _ launcher: SafeSpaceRecipeLauncher
    ) -> String {
        let serviceID = jupyterLabLauncherServiceID(launcher)
        let appDirectory = "/var/lib/outershell/apps/\(serviceID)"
        let serviceFile = "/var/lib/outershell/services/\(serviceID).outerservice"
        let displayName = launcher.displayName
            .replacingOccurrences(of: "\"", with: "'")
            .replacingOccurrences(of: "\n", with: " ")
        let registration = recipeAppRegistrationCommand(
            displayName: displayName,
            serviceID: serviceID,
            socketPath: "/run/user/0/\(serviceID)",
            frontendURL: "/lab",
            iconPath: nil
        )
        return """
        set -eu
        /bin/mkdir -p \(appDirectory) "$(/usr/bin/dirname \(serviceFile))"
        /usr/bin/touch \(appDirectory)/backend.log
        /bin/cat >\(serviceFile) <<'OUTERSHELL_SERVICE'
        [Service]
        Format=1
        Name=\(displayName)
        Executable=/opt/outershell/jupyter/bin/jupyter-lab
        Argument=--no-browser
        Argument=--ServerApp.sock=/run/user/0/\(serviceID)
        Argument=--ServerApp.sock_mode=0600
        Argument=--IdentityProvider.token=
        Argument=--ServerApp.password=
        Argument=--ServerApp.root_dir=\(launcher.workingDirectory)
        WorkingDirectory=\(launcher.workingDirectory)
        Environment=HOME=/root
        Environment=USER=root
        Environment=LOGNAME=root
        Environment=XDG_RUNTIME_DIR=/run/user/0
        EnvironmentPolicy=clean
        Start=eager
        Restart=on-failure
        RestartDelayMilliseconds=1000
        LogPath=\(appDirectory)/backend.log
        OUTERSHELL_SERVICE
        /bin/cat >/etc/outershell/apps.d/\(serviceID).sh <<'OUTERSHELL_REGISTRATION'
        \(registration)
        OUTERSHELL_REGISTRATION
        /bin/chmod 0755 /etc/outershell/apps.d/\(serviceID).sh
        """
    }

    private func jupyterLabLauncherServiceID(
        _ launcher: SafeSpaceRecipeLauncher
    ) -> String {
        let identifier = launcher.id.uuidString
            .replacingOccurrences(of: "-", with: "")
            .lowercased()
        return "org.outershell.JupyterLab.\(identifier)"
    }

    private func generatedDockerfile(for value: SafeSpaceRecipe,
                                     record: SafeSpaceRecord) throws -> String {
        let value = normalizedRecipe(value)
        let hasManagedOuterShellSupport = usesManagedOuterShellSupport(value)
        let editableSteps = try editableRecipeSteps(
            for: record,
            users: value.users ?? []
        )
        var lines = [
            "# syntax=docker/dockerfile:1",
            "FROM \(value.baseImage)"
        ]
        if value.installsOuterShellSupport == true {
            lines.append("")
            lines.append(contentsOf: outerShellBootstrapInstructions())
        }
        let hasGeneratedRecipeContent = !value.steps.isEmpty ||
            !(value.launchers ?? []).isEmpty ||
            !editableSteps.isEmpty ||
            !(value.users ?? []).isEmpty
        if hasManagedOuterShellSupport || hasGeneratedRecipeContent {
            lines.append(contentsOf: [
                "USER root",
                "WORKDIR /"
            ])
        }
        if !editableSteps.isEmpty || !(value.users ?? []).isEmpty {
            lines.append("RUN /usr/bin/install -d -m 0700 -o root -g root /var/lib/outershell/recipe && /usr/bin/install -d -m 0700 -o root -g root /var/lib/outershell/recipe/root")
        }
        for (index, step) in value.steps.enumerated() {
            if let fragment = step.dockerfileFragment?.trimmingCharacters(
                in: .whitespacesAndNewlines
            ), !fragment.isEmpty {
                lines.append("")
                lines.append(fragment)
                if step.catalogItemID != nil,
                   !step.command.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                    lines.append(contentsOf: recipeStepInstructions(
                        fileName: recipeStepFileName(index: index, step: step)
                    ))
                }
            } else {
                lines.append(contentsOf: recipeStepInstructions(
                    fileName: recipeStepFileName(index: index, step: step)
                ))
            }
        }
        for (index, launcher) in (value.launchers ?? []).enumerated() {
            lines.append(contentsOf: recipeStepInstructions(
                fileName: recipeLauncherFileName(index: index, launcher: launcher)
            ))
        }
        for step in editableSteps where step.scope == "root" {
            lines.append(contentsOf: editableRecipeStepInstructions(step))
        }
        for user in value.users ?? [] {
            let userRoot = "/var/lib/outershell/recipe/users/\(user.name)"
            lines.append(contentsOf: [
                "",
                "USER root",
                "RUN /usr/bin/id -u \(user.name) >/dev/null 2>&1 || /usr/sbin/useradd --create-home --user-group --shell /bin/bash \(user.name)",
                "RUN /usr/bin/install -d -m 0700 -o \(user.name) -g \(user.name) \(userRoot) && /usr/bin/install -d -m 0700 -o \(user.name) -g \(user.name) \(userRoot)/steps",
                "ENV HOME=\(user.homeDirectory) USER=\(user.name) LOGNAME=\(user.name)",
                "USER \(user.name)",
                "WORKDIR \(user.workingDirectory)"
            ])
            for step in editableSteps where step.user?.id == user.id {
                lines.append(contentsOf: editableRecipeStepInstructions(step))
            }
        }
        if hasManagedOuterShellSupport {
            lines.append("")
            lines.append(contentsOf: outerShellRuntimeInstructions())
        }
        lines.append("")
        return lines.joined(separator: "\n")
    }

    private func dockerfileFragment(for step: SafeSpaceRecipeStep,
                                    index: Int) -> String {
        if let fragment = step.dockerfileFragment?.trimmingCharacters(
            in: .whitespacesAndNewlines
        ), !fragment.isEmpty {
            return fragment
        }
        return recipeStepInstructions(
            fileName: recipeStepFileName(index: index, step: step)
        ).joined(separator: "\n")
    }

    private func dockerfileFragmentDictionaries(
        for value: SafeSpaceRecipe,
        record: SafeSpaceRecord
    ) throws -> [[String: Any]] {
        var fragments: [[String: Any]] = [[
            "id": "header",
            "stepID": "",
            "displayName": "Base image",
            "contents": [
                "# syntax=docker/dockerfile:1",
                "FROM \(value.baseImage)"
            ].joined(separator: "\n"),
            "isEditable": true,
            "isRemovable": false,
            "isApplied": value.realizedBaseImage == value.baseImage
        ]]
        if value.installsOuterShellSupport == true {
            fragments.append([
                "id": "outershell-bootstrap",
                "stepID": "",
                "displayName": "Install Outer Shell support",
                "contents": outerShellBootstrapInstructions().joined(separator: "\n"),
                "isEditable": false,
                "isRemovable": false,
                "isApplied": value.realizedInstallsOuterShellSupport == true
            ])
        }
        let hasManagedOuterShellSupport = usesManagedOuterShellSupport(value)
        if hasManagedOuterShellSupport || !value.steps.isEmpty ||
            !(value.launchers ?? []).isEmpty || !(value.users ?? []).isEmpty {
            fragments.append([
                "id": "root-environment",
                "stepID": "",
                "displayName": "Root environment",
                "contents": [
                    "USER root",
                    "WORKDIR /"
                ].joined(separator: "\n"),
                "isEditable": false,
                "isRemovable": false,
                "isApplied": true
            ])
        }
        let realizedIDs = Set(value.realizedStepIDs)
        for (index, step) in value.steps.enumerated() {
            let rawFragment = step.dockerfileFragment?.trimmingCharacters(
                in: .whitespacesAndNewlines
            )
            fragments.append([
                "id": step.id.uuidString,
                "stepID": step.id.uuidString,
                "displayName": step.displayName ?? "Dockerfile fragment",
                "contents": dockerfileFragment(for: step, index: index),
                "isEditable": step.isEditable ?? false,
                "isRemovable": true,
                "isApplied": realizedIDs.contains(step.id)
            ])
            if rawFragment?.isEmpty == false,
               step.catalogItemID != nil,
               !step.command.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                fragments.append([
                    "id": "\(step.id.uuidString)-integration",
                    "stepID": "",
                    "displayName": "Connect \(step.displayName ?? "app") to Outer Shell",
                    "contents": recipeStepInstructions(
                        fileName: recipeStepFileName(index: index, step: step)
                    ).joined(separator: "\n"),
                    "isEditable": false,
                    "isRemovable": false,
                    "isApplied": realizedIDs.contains(step.id)
                ])
            }
        }
        for (index, launcher) in (value.launchers ?? []).enumerated() {
            fragments.append([
                "id": launcher.id.uuidString,
                "stepID": "",
                "displayName": "Add \(launcher.displayName) app",
                "contents": recipeStepInstructions(
                    fileName: recipeLauncherFileName(index: index, launcher: launcher)
                ).joined(separator: "\n"),
                "isEditable": false,
                "isRemovable": false,
                "isApplied": Set(value.realizedLauncherIDs ?? []).contains(launcher.id)
            ])
        }
        let editableSteps = try editableRecipeSteps(
            for: record,
            users: value.users ?? []
        )
        let realizedContents = value.realizedEditableStepContents ?? [:]
        for step in editableSteps where step.scope == "root" {
            fragments.append([
                "id": "recipe-file-\(step.relativePath)",
                "stepID": "",
                "displayName": "Root setup · \(step.fileName)",
                "contents": editableRecipeStepInstructions(step).joined(separator: "\n"),
                "isEditable": false,
                "isRemovable": false,
                "isApplied": realizedContents[step.relativePath] == step.contents
            ])
        }
        for user in value.users ?? [] {
            let userRoot = "/var/lib/outershell/recipe/users/\(user.name)"
            fragments.append([
                "id": "user-\(user.id.uuidString)",
                "stepID": "",
                "displayName": "User · \(user.name)",
                "contents": [
                    "USER root",
                    "RUN /usr/bin/id -u \(user.name) >/dev/null 2>&1 || /usr/sbin/useradd --create-home --user-group --shell /bin/bash \(user.name)",
                    "RUN /usr/bin/install -d -m 0700 -o \(user.name) -g \(user.name) \(userRoot) && /usr/bin/install -d -m 0700 -o \(user.name) -g \(user.name) \(userRoot)/steps",
                    "ENV HOME=\(user.homeDirectory) USER=\(user.name) LOGNAME=\(user.name)",
                    "USER \(user.name)",
                    "WORKDIR \(user.workingDirectory)"
                ].joined(separator: "\n"),
                "isEditable": false,
                "isRemovable": false,
                "isApplied": true
            ])
            for step in editableSteps where step.user?.id == user.id {
                fragments.append([
                    "id": "recipe-file-\(step.relativePath)",
                    "stepID": "",
                    "displayName": "\(user.name) setup · \(step.fileName)",
                    "contents": editableRecipeStepInstructions(step).joined(separator: "\n"),
                    "isEditable": false,
                    "isRemovable": false,
                    "isApplied": realizedContents[step.relativePath] == step.contents
                ])
            }
        }
        if hasManagedOuterShellSupport {
            fragments.append([
                "id": "footer",
                "stepID": "",
                "displayName": "Container default · root",
                "contents": outerShellRuntimeInstructions().joined(separator: "\n"),
                "isEditable": false,
                "isRemovable": false,
                "isApplied": true
            ])
        }
        return fragments
    }

    private func recipeStepFileName(index: Int,
                                    step: SafeSpaceRecipeStep) -> String {
        let name = step.displayName ?? "Run command"
        let slug = name.lowercased()
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        return String(format: "%03d-%@.sh", index + 1,
                      slug.isEmpty ? "run-command" : slug)
    }

    private func recipeLauncherFileName(index: Int,
                                        launcher: SafeSpaceRecipeLauncher) -> String {
        let slug = launcher.displayName.lowercased()
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        return String(format: "%03d-add-%@.sh", index + 501,
                      slug.isEmpty ? "app" : slug)
    }

    private func recipeStepInstructions(fileName: String) -> [String] {
        let source = "managed-steps/\(fileName)"
        let destination = "/tmp/outershell-recipe/\(fileName)"
        return [
            "COPY --chmod=0755 \(source) \(destination)",
            "RUN \(destination) && rm -f \(destination)"
        ]
    }

    private func usesBuiltInOuterShellImage(_ image: String) -> Bool {
        image == rootContainerBaseImage
    }

    private func usesManagedOuterShellSupport(_ value: SafeSpaceRecipe) -> Bool {
        usesBuiltInOuterShellImage(value.baseImage) ||
            value.installsOuterShellSupport == true
    }

    private func outerShellBootstrapInstructions() -> [String] {
        [
            "COPY --chmod=0755 .outershell/bootstrap/ /tmp/outershell-bootstrap/",
            "RUN /tmp/outershell-bootstrap/install.sh && rm -rf /tmp/outershell-bootstrap"
        ]
    }

    private func outerShellRuntimeInstructions() -> [String] {
        [
            "ENV HOME=/root USER=root LOGNAME=root XDG_RUNTIME_DIR=/run/user/0 OUTERSHELL_HOME=/var/lib/outershell OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api",
            "USER root",
            "WORKDIR /root",
            "CMD [\"/usr/local/bin/outershell-container-init\"]"
        ]
    }

    private func outerShellSupportSnippet() -> String {
        (outerShellBootstrapInstructions() + [""] + outerShellRuntimeInstructions())
            .joined(separator: "\n")
    }

    private func editableRecipeStepInstructions(
        _ step: SafeSpaceEditableRecipeStep
    ) -> [String] {
        if let user = step.user {
            let source = "steps/users/\(user.name)/\(step.fileName)"
            let destination = "/var/lib/outershell/recipe/users/\(user.name)/steps/\(step.fileName)"
            return [
                "COPY --chown=\(user.name):\(user.name) --chmod=0755 \(source) \(destination)",
                "RUN \(destination)"
            ]
        }
        let source = "steps/root/\(step.fileName)"
        let destination = "/var/lib/outershell/recipe/root/\(step.fileName)"
        return [
            "COPY --chmod=0755 \(source) \(destination)",
            "RUN \(destination)"
        ]
    }

    private func editableRecipeSteps(
        for record: SafeSpaceRecord,
        users: [SafeSpaceRecipeUser]
    ) throws -> [SafeSpaceEditableRecipeStep] {
        try createEditableRecipeStepDirectories(
            in: recipeDirectory(record.id),
            users: users
        )
        var values: [SafeSpaceEditableRecipeStep] = []
        var identities: [(scope: String, user: SafeSpaceRecipeUser?, directory: URL)] = [
            ("root", nil, try rootRecipeStepDirectory(for: record))
        ]
        for user in users {
            identities.append((
                "user:\(user.name)",
                user,
                try userRecipeStepDirectory(for: record, user: user)
            ))
        }
        for identity in identities {
            let urls = try FileManager.default.contentsOfDirectory(
                at: identity.directory,
                includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey],
                options: [.skipsHiddenFiles]
            ).sorted { $0.lastPathComponent < $1.lastPathComponent }
            for url in urls where url.pathExtension == "sh" {
                let name = url.lastPathComponent
                guard name.unicodeScalars.allSatisfy({
                    CharacterSet.alphanumerics.contains($0) || "._-".unicodeScalars.contains($0)
                }) else {
                    throw SafeSpaceManagerError.commandFailed(
                        "Recipe step names may contain only letters, numbers, dots, hyphens, and underscores."
                    )
                }
                let resources = try url.resourceValues(
                    forKeys: [.isRegularFileKey, .isSymbolicLinkKey]
                )
                guard resources.isRegularFile == true,
                      resources.isSymbolicLink != true else {
                    throw SafeSpaceManagerError.commandFailed(
                        "Recipe steps must be regular shell-script files."
                    )
                }
                values.append(SafeSpaceEditableRecipeStep(
                    relativePath: "\(identity.scope)/\(name)",
                    scope: identity.scope,
                    user: identity.user,
                    fileName: name,
                    contents: try String(contentsOf: url, encoding: .utf8)
                ))
            }
        }
        return values
    }

    private func editableRecipeStepContents(
        for record: SafeSpaceRecord,
        users: [SafeSpaceRecipeUser]
    ) throws -> [String: String] {
        Dictionary(uniqueKeysWithValues: try editableRecipeSteps(
            for: record,
            users: users
        ).map {
            ($0.relativePath, $0.contents)
        })
    }

    private func writeRecipeBuildContext(_ value: SafeSpaceRecipe,
                                         to directory: URL) throws {
        try writeOuterShellBootstrap(to: directory, isRequired: true)
        let legacyAssets = directory.appendingPathComponent("assets", isDirectory: true)
        if FileManager.default.fileExists(atPath: legacyAssets.path) {
            try FileManager.default.removeItem(at: legacyAssets)
        }
        let stepsDirectory = directory.appendingPathComponent("managed-steps", isDirectory: true)
        if FileManager.default.fileExists(atPath: stepsDirectory.path) {
            try FileManager.default.removeItem(at: stepsDirectory)
        }
        try FileManager.default.createDirectory(at: stepsDirectory,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        for (index, step) in value.steps.enumerated() {
            if step.dockerfileFragment == nil ||
                (step.catalogItemID != nil &&
                 !step.command.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty) {
                try writeRecipeScript(step.command,
                                      named: recipeStepFileName(index: index, step: step),
                                      to: stepsDirectory)
            }
        }
        for (index, launcher) in (value.launchers ?? []).enumerated() {
            guard launcher.kind == "jupyterLab" else { continue }
            try writeRecipeScript(
                jupyterLabLauncherRecipeCommand(launcher),
                named: recipeLauncherFileName(index: index, launcher: launcher),
                to: stepsDirectory
            )
        }
    }

    private func writeOuterShellBootstrap(to directory: URL,
                                          isRequired: Bool) throws {
        let outerShellDirectory = directory.appendingPathComponent(
            ".outershell",
            isDirectory: true
        )
        let destination = outerShellDirectory.appendingPathComponent(
            "bootstrap",
            isDirectory: true
        )
        if FileManager.default.fileExists(atPath: destination.path) {
            try FileManager.default.removeItem(at: destination)
        }
        guard isRequired else { return }
        guard let source = containerBootstrapPayloadURL() else {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell's portable container bootstrap is missing."
            )
        }
        try FileManager.default.createDirectory(
            at: outerShellDirectory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        try FileManager.default.copyItem(at: source, to: destination)
        try outerShellContainerBootstrapInstallScript().write(
            to: destination.appendingPathComponent("install.sh"),
            atomically: true,
            encoding: .utf8
        )
        try outerShellContainerInitScript().write(
            to: destination.appendingPathComponent("outershell-container-init"),
            atomically: true,
            encoding: .utf8
        )
    }

    private func writeContainerProjectGuide(to directory: URL) throws {
        let guide = """
        # Outer Shell container project

        This directory is an ordinary OCI build context. Its `Dockerfile` can be built with Apple `container`, Docker, Podman, or another compatible builder.

        ```sh
        container build --tag my-container .
        # or: docker build --tag my-container .
        ```

        The `FROM` instruction is your chosen base image. For a custom image, Outer Shell can use `.outershell/bootstrap` to install the small service-management tools that let apps announce themselves. You can omit that step when the base image already provides compatible Outer Shell support.

        Bundled Outer Shell apps are ordinary OCI image inputs. Their Dockerfile fragments use `COPY --from` to copy the app's `/outershell-rootfs` overlay into this image. Outer Shell pulls those source images when they are available and can prepare the same tagged images locally from its bundled app releases.

        Outer Shell treats `Dockerfile` as the source of truth. The same project directory is mounted at `/var/lib/outershell/project` while the container runs, so a root process in the container can edit the Dockerfile and its build-context files directly. Rebuilding recreates the image from those files.

        Mounted folders are runtime configuration. They intentionally do not appear in the Dockerfile or become part of the image. Recreate those mounts separately when moving the container to another host.

        The portable bootstrap expects a Linux image with `/bin/sh` and standard file utilities. Software snippets may have narrower requirements; for example, the supplied JupyterLab and RStudio Server snippets target Debian-family images.
        """
        try guide.write(
            to: directory.appendingPathComponent("README.md"),
            atomically: true,
            encoding: .utf8
        )
        let dockerIgnore = """
        README.md
        recipe.json
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

    private func outerShellContainerBootstrapInstallScript() -> String {
        """
        #!/bin/sh
        set -eu
        architecture=$(/bin/uname -m)
        case "${architecture}" in
            aarch64|arm64) architecture=aarch64 ;;
            x86_64|amd64) architecture=x86_64 ;;
            *) /bin/echo "Unsupported architecture: ${architecture}" >&2; exit 1 ;;
        esac
        libc=glibc
        for loader in /lib/ld-musl-*.so.1 /lib/libc.musl-*.so.1; do
            if [ -e "${loader}" ]; then libc=musl; break; fi
        done
        source_directory=/tmp/outershell-bootstrap/bin/${libc}/${architecture}
        test -x "${source_directory}/outershelld"
        test -x "${source_directory}/outerctl"
        test -x "${source_directory}/outer-socket-bridge"
        /bin/mkdir -p /usr/local/bin /run/user/0 /var/lib/outershell/services \
            /var/lib/outershell/apps /etc/outershell/apps.d /root/.config/outerloop \
            /root/.local/share/jupyter /root/.cache
        /bin/cp "${source_directory}/outershelld" /usr/local/bin/outershelld
        /bin/cp "${source_directory}/outerctl" /usr/local/bin/outerctl
        /bin/cp "${source_directory}/outer-socket-bridge" \
            /usr/local/bin/outer-socket-bridge
        /bin/cp /tmp/outershell-bootstrap/outershell-container-init \
            /usr/local/bin/outershell-container-init
        /bin/chmod 0755 /usr/local/bin/outershelld /usr/local/bin/outerctl \
            /usr/local/bin/outer-socket-bridge \
            /usr/local/bin/outershell-container-init
        /bin/chmod 0700 /run/user/0 /root/.config/outerloop
        """
    }

    private func outerShellContainerInitScript() -> String {
        """
        #!/bin/sh
        set -eu
        umask 022
        /bin/mkdir -p /run/user/0 /root/.config/outerloop /var/lib/outershell/services \
            /var/lib/outershell/apps /etc/outershell/apps.d /root/.local/share/jupyter \
            /root/.cache
        /bin/chmod 0700 /run/user/0 /root/.config/outerloop
        /usr/bin/printf '%s\n' '%t/outershelld-api' \
            >/root/.config/outerloop/http-unix.allow
        /bin/chmod 0600 /root/.config/outerloop/http-unix.allow
        export HOME=/root USER=root LOGNAME=root XDG_RUNTIME_DIR=/run/user/0
        export OUTERSHELL_HOME=/var/lib/outershell
        export OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
        /usr/local/bin/outershelld --service-manager internal \
            --services-dir /var/lib/outershell/services \
            --api-socket-path /run/user/0/outershelld-api --stay-alive &
        daemon_pid=$!
        /usr/bin/printf '%s\n' "${daemon_pid}" >/run/user/0/outershelld.pid
        cleanup() {
            /bin/kill "${daemon_pid}" 2>/dev/null || true
            wait "${daemon_pid}" 2>/dev/null || true
        }
        trap cleanup EXIT INT TERM HUP
        attempts=0
        while [ ! -S /run/user/0/outershelld-api ] && [ "${attempts}" -lt 120 ]; do
            attempts=$((attempts + 1))
            /bin/sleep 0.1
        done
        for registration in /etc/outershell/apps.d/*.sh; do
            [ -f "${registration}" ] || continue
            /bin/sh "${registration}" || \
                /usr/bin/printf 'App registration failed: %s\n' "${registration}" >&2
        done
        wait "${daemon_pid}"
        """
    }

    private func writeRecipeScript(_ command: String,
                                   named fileName: String,
                                   to directory: URL) throws {
        let contents = """
        #!/bin/sh
        set -eu
        cd /
        \(command)

        """
        let url = directory.appendingPathComponent(fileName)
        try contents.write(to: url, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700],
                                              ofItemAtPath: url.path)
    }

    private func recipeDirectory(_ id: UUID) throws -> URL {
        try safeSpaceDirectory(id).appendingPathComponent("Recipe", isDirectory: true)
    }

    private func dockerfileURL(_ id: UUID) throws -> URL {
        try recipeDirectory(id).appendingPathComponent("Dockerfile")
    }

    private func dockerfileContents(for record: SafeSpaceRecord) throws -> String {
        try String(contentsOf: dockerfileURL(record.id), encoding: .utf8)
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

    private func appendDockerfileInstructions(_ instructions: String,
                                              for record: SafeSpaceRecord) throws {
        let addition = instructions.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !addition.isEmpty else { return }
        var contents = try dockerfileContents(for: record)
        if !contents.hasSuffix("\n") {
            contents += "\n"
        }
        contents += "\n\(addition)\n"
        try contents.write(to: dockerfileURL(record.id),
                           atomically: true,
                           encoding: .utf8)
    }

    private func rootRecipeStepDirectory(for record: SafeSpaceRecord) throws -> URL {
        return try recipeDirectory(record.id)
            .appendingPathComponent("steps", isDirectory: true)
            .appendingPathComponent("root", isDirectory: true)
    }

    private func userRecipeStepDirectory(for record: SafeSpaceRecord,
                                         user: SafeSpaceRecipeUser) throws -> URL {
        try recipeDirectory(record.id)
            .appendingPathComponent("steps", isDirectory: true)
            .appendingPathComponent("users", isDirectory: true)
            .appendingPathComponent(user.name, isDirectory: true)
    }

    private func createEditableRecipeStepDirectories(
        in recipeDirectory: URL,
        users: [SafeSpaceRecipeUser]
    ) throws {
        let root = recipeDirectory.appendingPathComponent("steps", isDirectory: true)
        try FileManager.default.createDirectory(at: root,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        try FileManager.default.createDirectory(
            at: root.appendingPathComponent("root", isDirectory: true),
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        let userRoot = root.appendingPathComponent("users", isDirectory: true)
        try FileManager.default.createDirectory(at: userRoot,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        for user in users {
            let destination = userRoot.appendingPathComponent(user.name, isDirectory: true)
            try FileManager.default.createDirectory(
                at: destination,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            if user.name == "workspace" {
                try migrateLegacyWorkspaceSteps(
                    from: root.appendingPathComponent("workspace", isDirectory: true),
                    to: destination
                )
            }
        }
    }

    private func migrateLegacyWorkspaceSteps(from source: URL, to destination: URL) throws {
        guard FileManager.default.fileExists(atPath: source.path) else { return }
        for sourceFile in try FileManager.default.contentsOfDirectory(
            at: source,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) where sourceFile.pathExtension == "sh" {
            let destinationFile = destination.appendingPathComponent(sourceFile.lastPathComponent)
            guard !FileManager.default.fileExists(atPath: destinationFile.path) else { continue }
            try FileManager.default.copyItem(at: sourceFile, to: destinationFile)
        }
    }

    private func bundledRecipePayloadURL(named name: String) -> URL? {
        let architecture = cpuArchitecture() == "arm64" ? "linux-aarch64" : "linux-x86_64"
        let candidates = [
            Bundle.main.resourceURL?
                .appendingPathComponent("bundled-apps", isDirectory: true)
                .appendingPathComponent(name, isDirectory: true)
                .appendingPathComponent(architecture, isDirectory: true),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath,
                isDirectory: true)
                .appendingPathComponent("build/run/bundled-apps", isDirectory: true)
                .appendingPathComponent(name, isDirectory: true)
                .appendingPathComponent(architecture, isDirectory: true)
        ].compactMap { $0 }
        return candidates.first {
            FileManager.default.fileExists(atPath: $0.path)
        }
    }

    private func recipeURL(_ id: UUID) throws -> URL {
        try recipeDirectory(id).appendingPathComponent("recipe.json")
    }

    private func recipeImageReference(_ id: UUID) -> String {
        "outershell/safe-space-\(id.uuidString.lowercased()):recipe"
    }

    private func createManagedDirectories(for record: SafeSpaceRecord) throws {
        let base = try safeSpaceDirectory(record.id)
        for name in ["Runtime/PiAgent", "Persistent Data"] {
            try FileManager.default.createDirectory(
                at: base.appendingPathComponent(name, isDirectory: true),
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
        }
    }

    private func persistentDataDirectory(
        _ record: SafeSpaceRecord,
        item: SafeSpacePersistentData
    ) throws -> URL {
        try safeSpaceDirectory(record.id)
            .appendingPathComponent("Persistent Data", isDirectory: true)
            .appendingPathComponent(item.id.uuidString.lowercased(), isDirectory: true)
    }

    private func createLegacyWorkspaceDirectory(for record: SafeSpaceRecord) throws {
        try FileManager.default.createDirectory(
            at: try safeSpaceDirectory(record.id)
                .appendingPathComponent("Workspace", isDirectory: true),
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
    }

    private func catalogURL() throws -> URL {
        try applicationDirectory().appendingPathComponent("safe-spaces.json")
    }

    private func cachedAppsURL() throws -> URL {
        try applicationDirectory().appendingPathComponent("safe-space-apps.json")
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

    private func safeSpaceDirectory(_ id: UUID) throws -> URL {
        try applicationDirectory()
            .appendingPathComponent("Safe Spaces", isDirectory: true)
            .appendingPathComponent(id.uuidString.lowercased(), isDirectory: true)
    }

    private func legacyDirectory() throws -> URL {
        try FileManager.default.url(for: .applicationSupportDirectory,
                                    in: .userDomainMask,
                                    appropriateFor: nil,
                                    create: false)
            .appendingPathComponent("Outer Loop/Workspaces", isDirectory: true)
    }

    private func publishedSocketPath(_ id: UUID, socketPath: String) throws -> String {
        let digest = socketPath.utf8.reduce(UInt64(14_695_981_039_346_656_037)) {
            ($0 ^ UInt64($1)) &* 1_099_511_628_211
        }
        let directory = URL(fileURLWithPath: "/private/tmp/outershell-\(getuid())",
                            isDirectory: true)
            .appendingPathComponent("safe-space-relays", isDirectory: true)
            .appendingPathComponent(String(id.uuidString.lowercased().prefix(8)),
                                    isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        return directory.appendingPathComponent(
            String(format: "%016llx.sock", digest)
        ).path
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
            .appendingPathComponent("dev.outergroup.OuterLoop", isDirectory: true)
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

    private func containerName(_ id: UUID) -> String {
        "outerloop-workspace-\(id.uuidString.lowercased())"
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

    private func rootContainerBaseReference(
        for provider: SafeSpaceRuntimeProviderID
    ) throws -> String {
        containerBaseImageLock.lock()
        defer { containerBaseImageLock.unlock() }
        let images = try containerImageReferences(provider: provider)
        if images.contains(rootContainerBaseImage) {
            return rootContainerBaseImage
        }
        if provider == .docker {
            try buildRootContainerBase(
                from: "debian:bookworm",
                provider: provider
            )
            return rootContainerBaseImage
        }
        let prefix = "outerloop/workspace-\(getuid()):"
        let references = images.compactMap { reference -> (version: Int, reference: String)? in
            guard reference.hasPrefix(prefix),
                  let version = Int(reference.dropFirst(prefix.count)) else {
                return nil
            }
            return (version, reference)
        }
        guard let selected = references.max(by: { $0.version < $1.version }) else {
            throw SafeSpaceManagerError.commandFailed(
                "Outer Shell's container OCI image has not been installed."
            )
        }
        try buildRootContainerBase(
            from: selected.reference,
            provider: provider
        )
        return rootContainerBaseImage
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
        var value = try recipe(for: record)
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
            try saveRecipe(value, for: record)
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

    private func buildRootContainerBase(
        from sourceImage: String,
        provider: SafeSpaceRuntimeProviderID
    ) throws {
        let buildContextsDirectory = try applicationDirectory()
            .appendingPathComponent("Build Contexts", isDirectory: true)
        try FileManager.default.createDirectory(
            at: buildContextsDirectory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        let directory = buildContextsDirectory
            .appendingPathComponent("outershell-container-base-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: directory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        defer { try? FileManager.default.removeItem(at: directory) }
        try writeOuterShellBootstrap(to: directory, isRequired: true)
        let containerfile = """
        FROM \(sourceImage)
        USER root
        COPY --chmod=0755 .outershell/bootstrap/ /tmp/outershell-bootstrap/
        RUN /tmp/outershell-bootstrap/install.sh \\
            && /bin/rm -rf /tmp/outershell-bootstrap \\
            && /bin/rm -rf /home/workspace /etc/sudoers.d/workspace \\
            && (/usr/sbin/userdel workspace 2>/dev/null || true) \\
            && (/usr/sbin/groupdel workspace 2>/dev/null || true) \\
            && /bin/rm -f /usr/local/bin/outerloop-workspace-init /usr/local/bin/outerloop-workspace-status /usr/local/bin/outerloop-workspace-control
        ENV HOME=/root USER=root LOGNAME=root XDG_RUNTIME_DIR=/run/user/0 OUTERSHELL_HOME=/var/lib/outershell OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
        USER root
        WORKDIR /root
        CMD ["/usr/local/bin/outershell-container-init"]
        """
        try containerfile.write(
            to: directory.appendingPathComponent("Containerfile"),
            atomically: true,
            encoding: .utf8
        )
        let arguments = [
            "build", "--file", "Containerfile",
            "--tag", rootContainerBaseImage, "--progress", "plain", "."
        ]
        let result: (status: Int32, stdout: String, stderr: String)
        switch provider {
        case .appleContainer:
            result = try runContainer(arguments, currentDirectoryURL: directory)
        case .docker:
            result = try runDocker(arguments, currentDirectoryURL: directory)
        }
        try requireSuccess(result, action: "prepare Outer Shell's root container image")
    }

    private func runContainerBuild(_ record: SafeSpaceRecord,
                                   imageReference: String)
        throws -> (status: Int32, stdout: String, stderr: String) {
        updateBuildProgressPhase(
            "preparing",
            detail: "Preparing the build context",
            for: record.id
        )
        let value = normalizedRecipe(try recipe(for: record))
        if value.baseImage == rootContainerBaseImage {
            _ = try rootContainerBaseReference(
                for: runtimeProvider(for: record)
            )
        }
        try prepareBundledAppOCIImages(
            for: record,
            recipe: value,
            dockerfile: try dockerfileContents(for: record)
        )
        try saveRecipe(value, for: record)
        let directory = try recipeDirectory(record.id)
        updateBuildProgressPhase(
            "building",
            detail: "Building the container image",
            for: record.id
        )
        let result = try runRuntime(record, [
            "build",
            "--file", "Dockerfile",
            "--tag", imageReference,
            "--progress", "plain",
            "."
        ], currentDirectoryURL: directory) { [weak self] output in
            self?.appendBuildOutput(output, for: record)
        }
        try requireSuccess(result, action: "build the container image")
        return result
    }

    private func prepareBundledAppOCIImages(
        for record: SafeSpaceRecord,
        recipe: SafeSpaceRecipe,
        dockerfile: String
    ) throws {
        let catalogItems = recipeCatalog()
        var requiredItemIDs = Set(recipe.steps.compactMap(\.catalogItemID))
        for item in catalogItems {
            guard let imageReference = item.ociImageReference,
                  dockerfile.contains(imageReference) else {
                continue
            }
            requiredItemIDs.insert(item.id)
        }
        let requiredItems = catalogItems.filter {
            requiredItemIDs.contains($0.id) &&
                $0.kind == .bundledApp &&
                $0.ociImageReference != nil
        }
        guard !requiredItems.isEmpty else { return }

        var availableImages = try containerImageReferences(
            provider: runtimeProvider(for: record)
        )
        for item in requiredItems {
            guard let imageReference = item.ociImageReference,
                  !availableImages.contains(imageReference) else {
                continue
            }
            let pull = try runRuntime(record, ["image", "pull", imageReference])
            if pull.status != 0 {
                try buildBundledAppOCIImage(item, for: record)
            }
            availableImages.insert(imageReference)
        }
    }

    private func buildBundledAppOCIImage(_ item: SafeSpaceRecipeCatalogItem,
                                         for record: SafeSpaceRecord) throws {
        guard let imageReference = item.ociImageReference,
              let installCommand = item.ociImageBuildCommand else {
            throw SafeSpaceManagerError.commandFailed(
                "The \(item.displayName) OCI app image is not configured."
            )
        }
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("outershell-app-image-\(UUID().uuidString)",
                                    isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }

        try "#!/bin/sh\n\(recipeAppBootstrapCommand())\n\(installCommand)\n".write(
            to: directory.appendingPathComponent("install.sh"),
            atomically: true,
            encoding: .utf8
        )
        var installerInstructions = [
            "FROM debian:bookworm AS installer"
        ]
        if let payloadName = item.bundledPayloadName {
            guard let source = bundledRecipePayloadURL(named: payloadName) else {
                throw SafeSpaceManagerError.commandFailed(
                    "Outer Shell does not include the \(payloadName) container payload."
                )
            }
            let assets = directory.appendingPathComponent("assets", isDirectory: true)
            try FileManager.default.createDirectory(at: assets,
                                                    withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
            try FileManager.default.copyItem(
                at: source,
                to: assets.appendingPathComponent(payloadName, isDirectory: true)
            )
            installerInstructions.append(
                "COPY --chmod=0755 assets/\(payloadName) /opt/outershell/apps/\(item.serviceID)"
            )
        }
        installerInstructions.append(contentsOf: [
            "COPY --chmod=0755 install.sh /tmp/outershell-install-app.sh",
            "RUN /tmp/outershell-install-app.sh && rm -f /tmp/outershell-install-app.sh",
            "FROM scratch"
        ])
        for path in bundledAppOCIPayloadPaths(item) {
            installerInstructions.append(
                "COPY --from=installer \(path) /outershell-rootfs\(path)"
            )
        }
        let dockerfile = (["# syntax=docker/dockerfile:1"] + installerInstructions)
            .joined(separator: "\n") + "\n"
        let dockerfileURL = directory.appendingPathComponent("Dockerfile")
        try dockerfile.write(to: dockerfileURL,
                             atomically: true,
                             encoding: .utf8)
        let result = try runRuntime(record, [
            "build",
            "--file", dockerfileURL.path,
            "--tag", imageReference,
            "--progress", "plain",
            directory.path
        ])
        try requireSuccess(result, action: "prepare the \(item.displayName) OCI app image")
    }

    private func bundledAppOCIPayloadPaths(
        _ item: SafeSpaceRecipeCatalogItem
    ) -> [String] {
        var paths = [
            "/var/lib/outershell/apps/\(item.serviceID)",
            "/var/lib/outershell/services/\(item.serviceID).outerservice",
            "/etc/outershell/apps.d/\(item.serviceID).sh"
        ]
        if item.id == "outershell.agentdiy" {
            paths.append(contentsOf: [
                "/opt/outershell/apps/\(item.serviceID)",
                "/usr/local/bin/node",
                "/usr/local/bin/npm",
                "/usr/local/bin/npx",
                "/usr/local/bin/pi",
                "/usr/local/bin/outershell-app",
                "/usr/local/lib/node_modules"
            ])
        }
        return paths
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
            let path = "/usr/local/bin/container"
            return FileManager.default.isExecutableFile(atPath: path)
                ? URL(fileURLWithPath: path)
                : nil
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
        let candidates = [
            "/usr/local/bin/docker",
            "/opt/homebrew/bin/docker",
            "/Applications/Docker.app/Contents/Resources/bin/docker"
        ]
        return candidates.first(where: {
            FileManager.default.isExecutableFile(atPath: $0)
        }).map(URL.init(fileURLWithPath:))
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
                              arguments: arguments,
                              environment: try dockerEnvironment(executable: executable),
                              currentDirectoryURL: currentDirectoryURL,
                              progress: progress)
    }

    private func dockerEnvironment(executable: URL) throws -> [String: String] {
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
        let configurationDirectory = try applicationDirectory()
            .appendingPathComponent("Docker", isDirectory: true)
        try FileManager.default.createDirectory(
            at: configurationDirectory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        try installDockerCLIPlugins(in: configurationDirectory)
        return [
            "PATH": path,
            "DOCKER_CONFIG": configurationDirectory.path,
            "DOCKER_HOST": "unix:///var/run/docker.sock"
        ]
    }

    private func installDockerCLIPlugins(in configurationDirectory: URL) throws {
        let sourceCandidates = [
            "/Applications/Docker.app/Contents/Resources/cli-plugins/docker-buildx",
            "/usr/local/lib/docker/cli-plugins/docker-buildx",
            "/opt/homebrew/lib/docker/cli-plugins/docker-buildx"
        ]
        guard let sourcePath = sourceCandidates.first(where: {
            FileManager.default.isExecutableFile(atPath: $0)
        }) else {
            return
        }
        let directory = configurationDirectory
            .appendingPathComponent("cli-plugins", isDirectory: true)
        try FileManager.default.createDirectory(
            at: directory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
        let destination = directory.appendingPathComponent("docker-buildx")
        if FileManager.default.fileExists(atPath: destination.path) {
            let currentTarget = try? FileManager.default.destinationOfSymbolicLink(
                atPath: destination.path
            )
            guard currentTarget != sourcePath else { return }
            try FileManager.default.removeItem(at: destination)
        }
        try FileManager.default.createSymbolicLink(
            at: destination,
            withDestinationURL: URL(fileURLWithPath: sourcePath)
        )
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
        let executable = "/usr/local/bin/container"
        guard FileManager.default.isExecutableFile(atPath: executable) else {
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
                                       arguments: ["system", "start"])
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
                            progress: ((String) -> Void)? = nil) throws
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
        process.standardOutput = stdout
        process.standardError = stderr
        do {
            try process.run()
        } catch {
            try? stdout.close()
            try? stderr.close()
            throw error
        }
        if let progress {
            let stdoutReader = try FileHandle(forReadingFrom: stdoutURL)
            let stderrReader = try FileHandle(forReadingFrom: stderrURL)
            defer {
                try? stdoutReader.close()
                try? stderrReader.close()
            }
            while process.isRunning {
                Thread.sleep(forTimeInterval: 0.1)
                if let data = try stdoutReader.readToEnd(), !data.isEmpty,
                   let text = String(data: data, encoding: .utf8) {
                    progress(text)
                }
                if let data = try stderrReader.readToEnd(), !data.isEmpty,
                   let text = String(data: data, encoding: .utf8) {
                    progress(text)
                }
            }
            process.waitUntilExit()
            if let data = try stdoutReader.readToEnd(), !data.isEmpty,
               let text = String(data: data, encoding: .utf8) {
                progress(text)
            }
            if let data = try stderrReader.readToEnd(), !data.isEmpty,
               let text = String(data: data, encoding: .utf8) {
                progress(text)
            }
        } else {
            process.waitUntilExit()
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
