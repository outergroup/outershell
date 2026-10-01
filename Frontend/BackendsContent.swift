import AppKit
import CoreText
import CryptoKit
import Darwin
import Foundation
import ImageIO
import QuartzCore

private struct EndpointIconDiskCache: Sendable {
    let directory: URL

    private func fileURL(for url: URL, rendition: String) -> URL {
        let digest = SHA256.hash(data: Data(url.absoluteString.utf8))
        return directory.appendingPathComponent(rendition + digest.map { String(format: "%02x", $0) }.joined())
    }

    func data(for url: URL, rendition: String = "") -> Data? {
        let file = fileURL(for: url, rendition: rendition)
        do {
            return try Data(contentsOf: file)
        } catch CocoaError.fileReadNoSuchFile {
            return nil
        } catch {
            print("Outer Shell: Cannot read cached icon: \(error)")
            return nil
        }
    }

    private static let rendition = "96px-v1-"

    func image(for url: URL) -> CGImage? {
        if let data = data(for: url, rendition: Self.rendition),
           let source = CGImageSourceCreateWithData(data as CFData, nil),
           let image = CGImageSourceCreateImageAtIndex(source, 0,
                [kCGImageSourceShouldCacheImmediately: true] as CFDictionary) {
            return image
        }
        guard let original = data(for: url) else { return nil }
        return storeImage(original, for: url)
    }

    func storeImage(_ data: Data, for url: URL) -> CGImage? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? Int,
              let height = properties[kCGImagePropertyPixelHeight] as? Int else { return nil }
        if max(width, height) <= 96 {
            guard let image = CGImageSourceCreateImageAtIndex(source, 0,
                [kCGImageSourceShouldCacheImmediately: true] as CFDictionary) else { return nil }
            store(data, for: url, rendition: Self.rendition)
            return image
        }
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 96,
            kCGImageSourceShouldCacheImmediately: true
        ] as CFDictionary) else { return nil }
        let encoded = NSMutableData()
        if let destination = CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil) {
            CGImageDestinationAddImage(destination, image, nil)
            if CGImageDestinationFinalize(destination) {
                store(encoded as Data, for: url, rendition: Self.rendition)
            } else {
                print("Outer Shell: Cannot encode resized icon.")
            }
        }
        return image
    }

    func store(_ data: Data, for url: URL, rendition: String = "") {
        do {
            let manager = FileManager.default
            try manager.createDirectory(at: directory, withIntermediateDirectories: true)
            let target = fileURL(for: url, rendition: rendition)
            let temporary = directory.appendingPathComponent("." + UUID().uuidString)
            defer { try? manager.removeItem(at: temporary) }
            try data.write(to: temporary)
            guard rename(temporary.path, target.path) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            let files = try manager.contentsOfDirectory(at: directory,
                includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey], options: [.skipsHiddenFiles])
            let entries = try files.map { file in
                (file, try file.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey]))
            }
            var bytes = entries.reduce(0) { $0 + ($1.1.fileSize ?? 0) }
            for (file, values) in entries.sorted(by: {
                ($0.1.contentModificationDate ?? .distantPast) < ($1.1.contentModificationDate ?? .distantPast)
            }) where bytes > 128 * 1024 * 1024 {
                try manager.removeItem(at: file)
                bytes -= values.fileSize ?? 0
            }
        } catch {
            print("Outer Shell: Cannot store cached icon: \(error)")
        }
    }
}

@MainActor
@objc public final class BackendsContent: NSObject, OuterframeContentLibrary {
    @objc public static func start(
        socketFD: Int32,
        appConnection: OuterframeAppConnection
    ) -> Int32 {
        let outerframeHost = OuterframeHost(socketFD: socketFD)
        let handler = BackendsHandler(outerframeHost: outerframeHost, appConnection: appConnection)
        outerframeHost.delegate = handler
        return 0
    }
}

private struct BackendsResponse {
    let error: String
    let backends: [BackendRecord]
}

private struct BackendRecord {
    let serviceID: String
    let displayName: String
    let serviceUnit: String
    let serviceUnitPath: String?
    let serviceScope: String
    let status: String
    let canControl: Bool
    let canUninstall: Bool?
    let isBundled: Bool?
    let isInstalled: Bool?
    let isMigration: Bool?
    let supportsRoot: Bool?
    let rootOnly: Bool?
    let hasRootSupport: Bool?
    let installedVersion: String?
    let availableVersion: String?
    let scriptPath: String?
    let publicBaseURL: String?
    let iconSymbolName: String?
    let launchdPlistPath: String
    let ownsLaunchdPlist: Bool
    let menuBarVisibilityEnabled: Bool?
    let menuBarVisibilityAvailable: Bool
    let frontends: [FrontendRecord]
    let logFiles: [LogFileRecord]

    var isBundledPlaceholder: Bool {
        (isBundled ?? false) && !(isInstalled ?? true)
    }

    var isBundledCatalogEntry: Bool {
        isBundled ?? false
    }

    var canUninstallBackend: Bool {
        canUninstall ?? false
    }

    var isBackendsSelf: Bool {
        serviceID == "org.outershell.OuterShell"
    }

    var isMigrationAction: Bool {
        isMigration ?? false
    }

}

private struct BundledCatalogEntry {
    let backend: BackendRecord
    let isInstalled: Bool
}

private struct FrontendRecord {
    let id: String
    let name: String
    let url: String
    let port: Int
    let socketPath: String
    let iconPath: String?
    let iconByteCount: Int
    let iconCGImage: CGImage?
    let iconObservationToken: String
    let list: String?
    let isRunning: Bool

    var hasEndpoint: Bool {
        if !socketPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || port > 0 {
            return true
        }
        let trimmedURL = url.trimmingCharacters(in: .whitespacesAndNewlines)
        return URL(string: trimmedURL)?.scheme != nil
    }

    var listName: String {
        list?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    var iconURL: String? = nil

}

private func decodedIconCGImage(_ data: Data) -> CGImage? {
    guard !data.isEmpty,
          let source = CGImageSourceCreateWithData(data as CFData, nil) else {
        return nil
    }
    let options = [kCGImageSourceShouldCacheImmediately: true] as CFDictionary
    return CGImageSourceCreateImageAtIndex(source, 0, options)
}

private struct LogFileRecord: Decodable {
    let identifier: String
    let displayName: String
    let path: String
    let size: UInt64
    let modified: Double
    let readable: Bool
}

private struct LogResponse: Decodable {
    let serviceID: String
    let path: String
    let contents: String
    let isTruncated: Bool
    let fileSize: UInt64
    let modified: Double
    let error: String
}

private struct ActionResponse: Decodable {
    let ok: Bool
    let message: String
    let needsPassword: Bool?
    let updateAvailable: Bool
    let installedVersion: String
    let availableVersion: String
}

private struct EventResponse {
    let backendsChanged: Bool
    let overviewChanged: Bool
    let overviewVersion: UInt64
    let logChanged: Bool
    let timedOut: Bool
    let backendsVersion: UInt64
    let logVersion: UInt64
}

private struct RecipesResponse: Decodable {
    let pythonSuggestions: [String]
    let recipes: [RecipeRecord]
}

private struct RecipeRecord: Decodable {
    let identifier: String
    let displayName: String
    let summary: String
    let fields: [RecipeFieldRecord]
}

private struct RecipeFieldRecord: Decodable {
    let key: String
    let label: String
    let defaultValue: String
    let fieldType: String
    let placeholder: String
    let suggestions: [String]
    let choices: [RecipeChoiceRecord]
}

private struct RecipeChoiceRecord: Decodable {
    let title: String
    let value: String
}

private struct FilePickerResponse: Decodable {
    let path: String
    let parent: String
    let entries: [FilePickerEntryRecord]
}

private struct FilePickerEntryRecord: Decodable {
    let name: String
    let path: String
    let isDirectory: Bool
    let willCreate: Bool
    let size: UInt64
    let modified: Double
}

private enum BinaryPayloadError: Error {
    case outOfBounds
    case invalidString
}

private struct BinaryPayloadReader {
    let data: Data

    func uint32(at offset: Int) throws -> UInt32 {
        guard offset >= 0, offset + 4 <= data.count else { throw BinaryPayloadError.outOfBounds }
        return UInt32(data[offset]) |
               (UInt32(data[offset + 1]) << 8) |
               (UInt32(data[offset + 2]) << 16) |
               (UInt32(data[offset + 3]) << 24)
    }

    func uint64(at offset: Int) throws -> UInt64 {
        guard offset >= 0, offset + 8 <= data.count else { throw BinaryPayloadError.outOfBounds }
        var value: UInt64 = 0
        for index in stride(from: 7, through: 0, by: -1) {
            value = (value << 8) | UInt64(data[offset + index])
        }
        return value
    }

    func double(at offset: Int) throws -> Double {
        Double(bitPattern: try uint64(at: offset))
    }

    func dataRef(at offset: Int) throws -> Data {
        let start = Int(try uint32(at: offset))
        let length = Int(try uint32(at: offset + 4))
        guard start >= 0, length >= 0, start <= data.count, length <= data.count - start else {
            throw BinaryPayloadError.outOfBounds
        }
        return Data(data[start..<(start + length)])
    }

    func stringRef(at offset: Int) throws -> String {
        let bytes = try dataRef(at: offset)
        guard !bytes.isEmpty else { return "" }
        guard let string = String(data: bytes, encoding: .utf8) else {
            throw BinaryPayloadError.invalidString
        }
        return string
    }

    func child(at offset: Int) throws -> BinaryPayloadReader {
        BinaryPayloadReader(data: try dataRef(at: offset))
    }

    func payloadArray() throws -> [BinaryPayloadReader] {
        let count = Int(try uint32(at: 0))
        var children: [BinaryPayloadReader] = []
        children.reserveCapacity(count)
        for index in 0..<count {
            children.append(try child(at: 4 + index * 8))
        }
        return children
    }

    func stringArray() throws -> [String] {
        let count = Int(try uint32(at: 0))
        var strings: [String] = []
        strings.reserveCapacity(count)
        for index in 0..<count {
            strings.append(try stringRef(at: 4 + index * 8))
        }
        return strings
    }
}

private extension BackendsResponse {
    static func decodeBinary(_ data: Data) throws -> BackendsResponse {
        let reader = BinaryPayloadReader(data: data)
        let count = Int(try reader.uint32(at: 8))
        var backends: [BackendRecord] = []
        backends.reserveCapacity(count)
        for index in 0..<count {
            backends.append(try BackendRecord.decodeBinary(try reader.child(at: 12 + index * 8)))
        }
        return BackendsResponse(error: try reader.stringRef(at: 0), backends: backends)
    }
}

private extension BackendRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> BackendRecord {
        let flags = try reader.uint32(at: 64)
        let serviceUnitPath = try reader.stringRef(at: 24)
        return BackendRecord(serviceID: try reader.stringRef(at: 0),
                             displayName: try reader.stringRef(at: 8),
                             serviceUnit: try reader.stringRef(at: 16),
                             serviceUnitPath: serviceUnitPath.isEmpty ? nil : serviceUnitPath,
                             serviceScope: try reader.stringRef(at: 32),
                             status: try reader.stringRef(at: 40),
                             canControl: (flags & 0x01) != 0,
                             canUninstall: (flags & 0x02) != 0,
                             isBundled: (flags & 0x04) != 0,
                             isInstalled: (flags & 0x08) != 0,
                             isMigration: (flags & 0x10) != 0,
                             supportsRoot: (flags & 0x40) != 0,
                             rootOnly: (flags & 0x80) != 0,
                             hasRootSupport: (flags & 0x100) != 0,
                             installedVersion: reader.data.count >= 92 ? emptyToNil(try reader.stringRef(at: 84)) : nil,
                             availableVersion: reader.data.count >= 100 ? emptyToNil(try reader.stringRef(at: 92)) : nil,
                             scriptPath: reader.data.count >= 108 ? emptyToNil(try reader.stringRef(at: 100)) : nil,
                             publicBaseURL: reader.data.count >= 116 ? emptyToNil(try reader.stringRef(at: 108)) : nil,
                             iconSymbolName: emptyToNil(try reader.stringRef(at: 48)),
                             launchdPlistPath: try reader.stringRef(at: 56),
                             ownsLaunchdPlist: (flags & 0x20) != 0,
                             menuBarVisibilityEnabled: (flags & 0x200) != 0,
                             menuBarVisibilityAvailable: (flags & 0x400) != 0,
                             frontends: try reader.child(at: 68).payloadArray().map(FrontendRecord.decodeBinary),
                             logFiles: try reader.child(at: 76).payloadArray().map(LogFileRecord.decodeBinary))
    }
}

private extension FrontendRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> FrontendRecord {
        let id = reader.data.count >= 60 ? try reader.stringRef(at: 52) : ""
        let flags = reader.data.count >= 64 ? try reader.uint32(at: 60) : 1
        let iconData = (flags & 2) == 0 ? try reader.dataRef(at: 32) : Data()
        return FrontendRecord(id: id,
                              name: try reader.stringRef(at: 0),
                              url: try reader.stringRef(at: 8),
                              port: Int(try reader.uint32(at: 48)),
                              socketPath: try reader.stringRef(at: 16),
                              iconPath: emptyToNil(try reader.stringRef(at: 24)),
                              iconByteCount: iconData.count,
                              iconCGImage: decodedIconCGImage(iconData),
                              iconObservationToken: reader.data.count >= 72 ? try reader.stringRef(at: 64) : "",
                              list: emptyToNil(try reader.stringRef(at: 40)),
                              isRunning: (flags & 0x01) != 0,
                              iconURL: (flags & 2) != 0 ? try reader.stringRef(at: 32) : nil)
    }
}

private extension LogFileRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> LogFileRecord {
        LogFileRecord(identifier: try reader.stringRef(at: 0),
                      displayName: try reader.stringRef(at: 8),
                      path: try reader.stringRef(at: 16),
                      size: try reader.uint64(at: 24),
                      modified: try reader.double(at: 32),
                      readable: (try reader.uint32(at: 40) & 0x01) != 0)
    }
}

private extension LogResponse {
    static func decodeBinary(_ data: Data) throws -> LogResponse {
        let reader = BinaryPayloadReader(data: data)
        return LogResponse(serviceID: try reader.stringRef(at: 0),
                           path: try reader.stringRef(at: 8),
                           contents: try reader.stringRef(at: 16),
                           isTruncated: (try reader.uint32(at: 24) & 0x01) != 0,
                           fileSize: try reader.uint64(at: 28),
                           modified: try reader.double(at: 36),
                           error: try reader.stringRef(at: 44))
    }
}

private extension ActionResponse {
    static func decodeBinary(_ data: Data) throws -> ActionResponse {
        let reader = BinaryPayloadReader(data: data)
        let flags = try reader.uint32(at: 0)
        return ActionResponse(ok: (flags & 0x01) != 0,
                              message: try reader.stringRef(at: 4),
                              needsPassword: (flags & 0x02) != 0,
                              updateAvailable: (flags & 0x04) != 0,
                              installedVersion: data.count >= 20 ? try reader.stringRef(at: 12) : "",
                              availableVersion: data.count >= 28 ? try reader.stringRef(at: 20) : "")
    }
}

private extension EventResponse {
    static func decodeBinary(_ data: Data) throws -> EventResponse {
        let reader = BinaryPayloadReader(data: data)
        let flags = try reader.uint32(at: 0)
        return EventResponse(backendsChanged: (flags & 0x01) != 0,
                             overviewChanged: (flags & 0x08) != 0,
                             overviewVersion: try reader.uint64(at: 24),
                             logChanged: (flags & 0x02) != 0,
                             timedOut: (flags & 0x04) != 0,
                             backendsVersion: try reader.uint64(at: 8),
                             logVersion: try reader.uint64(at: 16))
    }
}

private extension RecipesResponse {
    static func decodeBinary(_ data: Data) throws -> RecipesResponse {
        let reader = BinaryPayloadReader(data: data)
        return RecipesResponse(pythonSuggestions: try reader.child(at: 0).stringArray(),
                               recipes: try reader.child(at: 8).payloadArray().map(RecipeRecord.decodeBinary))
    }
}

private extension RecipeRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> RecipeRecord {
        RecipeRecord(identifier: try reader.stringRef(at: 0),
                     displayName: try reader.stringRef(at: 8),
                     summary: try reader.stringRef(at: 16),
                     fields: try reader.child(at: 24).payloadArray().map(RecipeFieldRecord.decodeBinary))
    }
}

private extension RecipeFieldRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> RecipeFieldRecord {
        RecipeFieldRecord(key: try reader.stringRef(at: 0),
                          label: try reader.stringRef(at: 8),
                          defaultValue: try reader.stringRef(at: 16),
                          fieldType: try reader.stringRef(at: 24),
                          placeholder: try reader.stringRef(at: 32),
                          suggestions: try reader.child(at: 40).stringArray(),
                          choices: try reader.child(at: 48).payloadArray().map(RecipeChoiceRecord.decodeBinary))
    }
}

private extension RecipeChoiceRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> RecipeChoiceRecord {
        RecipeChoiceRecord(title: try reader.stringRef(at: 0),
                           value: try reader.stringRef(at: 8))
    }
}

private extension FilePickerResponse {
    static func decodeBinary(_ data: Data) throws -> FilePickerResponse {
        let reader = BinaryPayloadReader(data: data)
        let count = Int(try reader.uint32(at: 16))
        var entries: [FilePickerEntryRecord] = []
        entries.reserveCapacity(count)
        for index in 0..<count {
            entries.append(try FilePickerEntryRecord.decodeBinary(try reader.child(at: 20 + index * 8)))
        }
        return FilePickerResponse(path: try reader.stringRef(at: 0),
                                  parent: try reader.stringRef(at: 8),
                                  entries: entries)
    }
}

private extension FilePickerEntryRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> FilePickerEntryRecord {
        let flags = try reader.uint32(at: 16)
        return FilePickerEntryRecord(name: try reader.stringRef(at: 0),
                                     path: try reader.stringRef(at: 8),
                                     isDirectory: (flags & 0x01) != 0,
                                     willCreate: (flags & 0x02) != 0,
                                     size: try reader.uint64(at: 20),
                                     modified: try reader.double(at: 28))
    }
}

private func emptyToNil(_ value: String) -> String? {
    value.isEmpty ? nil : value
}

private struct LogSelection: Equatable {
    let serviceID: String
    let serviceScope: String
    let logIndex: Int
}

private enum BackendsViewMode {
    case apps
    case create
}

private struct LocalWorkspaceAppRecord: Decodable, Equatable {
    let frontendID: String
    let serviceID: String
    let displayName: String
    let socketPath: String
    let url: String
    let iconPath: String
    let iconData: Data?
    let iconObservationToken: String
    let listName: String
    let isRunning: Bool
    let publishedPort: Int
    var iconURL: String? = nil

}

private struct LocalWorkspaceCommandRecord: Decodable, Equatable {
    let id: String
    let displayName: String
    let shellCommand: String
    let containerCommand: String
    let internalCommand: String
    let iconPath: String
    let iconData: Data?
    var iconURL: String? = nil

}

private struct LocalWorkspaceRecord: Decodable, Equatable {
    struct BuildProgress: Decodable, Equatable {
        let phase: String
        let detail: String
        let instruction: String?
        let currentStep: Int?
        let totalSteps: Int?
        let sourceStartLine: Int?
        let sourceEndLine: Int?
        let log: String
    }

    struct OuterShellSupport: Decodable, Equatable {
        let status: String
        let detail: String
    }

    struct Mount: Decodable, Equatable {
        let id: UUID
        let name: String
        let hostPath: String
        let guestPath: String
        let isReadOnly: Bool
        let isInfrastructure: Bool?
        let isRecipeMount: Bool?

        var isInfrastructureMount: Bool {
            isInfrastructure == true
        }

        var isRecipeInfrastructureMount: Bool {
            isRecipeMount == true
        }
    }

    struct PersistentData: Decodable, Equatable {
        let id: UUID
        let hostPath: String
        let guestPath: String
    }

    struct Runtime: Decodable, Equatable {
        let providerID: String
        let providerName: String
        let isolationKind: String
        let isolationName: String
        let operatingSystemName: String
        let operatingSystemVersion: String
        let architecture: String
    }

    struct Capabilities: Decodable, Equatable {
        let supportsApps: Bool
        let supportsShell: Bool
        let supportsLiveMounts: Bool
        let supportsMounts: Bool?
        let supportsRecipes: Bool?
    }

    struct Recipe: Decodable, Equatable {
        struct User: Decodable, Equatable {
            let id: String
            let name: String
            let homeDirectory: String
            let workingDirectory: String
            let isRoot: Bool
        }

        struct Step: Decodable, Equatable {
            let id: UUID
            let command: String
            let createdAt: Double
            let isApplied: Bool
            let catalogItemID: String?
            let displayName: String?
            let dockerfileFragment: String
            let isEditable: Bool
        }

        struct Fragment: Decodable, Equatable {
            let id: String
            let stepID: String
            let displayName: String
            let contents: String
            let isEditable: Bool
            let isRemovable: Bool
            let isApplied: Bool
        }

        struct ScriptFile: Decodable, Equatable {
            let relativePath: String
            let scope: String
            let userID: String
            let fileName: String
            let hostPath: String
            let guestPath: String
            let isApplied: Bool
        }

        struct CatalogItem: Decodable, Equatable {
            let id: String
            let displayName: String
            let summary: String
            let kind: String
            let serviceID: String
            let isInstalled: Bool
        }

        struct Launcher: Decodable, Equatable {
            let id: UUID
            let kind: String
            let displayName: String
            let workingDirectory: String
            let isApplied: Bool
        }

        struct TransferItem: Decodable, Equatable {
            let name: String
            let detail: String
            let status: String
        }

        struct EnvironmentVariable: Decodable, Equatable {
            let name: String
            let value: String
        }

        struct PublishedPort: Decodable, Equatable {
            let hostPort: Int
            let containerPort: Int
        }

        let baseImage: String
        let installsOuterShellSupport: Bool
        let outerShellSupportSnippet: String
        let steps: [Step]
        let fragments: [Fragment]
        let catalog: [CatalogItem]
        let launchers: [Launcher]
        let users: [User]
        let scriptFiles: [ScriptFile]
        let containerfile: String
        let dockerfileHostPath: String
        let dockerfileGuestPath: String
        let buildEngineAvailable: Bool
        let definitionDirectory: String
        let transferItems: [TransferItem]
        let needsRebuild: Bool
        let hasUntrackedChanges: Bool
        let workingDirectory: String
        let environment: [EnvironmentVariable]
        let publishedPorts: [PublishedPort]
    }

    let id: UUID
    let name: String
    let state: String
    let cpus: Int
    let memoryInGB: Int
    let runtimeKind: String?
    let managementKind: String?
    let ownsContainer: Bool?
    let runtimeName: String?
    let supportsLiveMounts: Bool?
    let runtime: Runtime?
    let capabilities: Capabilities?
    let shellCommand: String
    let apps: [LocalWorkspaceAppRecord]
    let commands: [LocalWorkspaceCommandRecord]?
    let mounts: [Mount]?
    let persistentData: [PersistentData]?
    let recipe: Recipe?
    let outerShellSupport: OuterShellSupport?
    let buildProgress: BuildProgress?

    var visibleMounts: [Mount] {
        mounts ?? []
    }

    var commandLaunchers: [LocalWorkspaceCommandRecord] {
        commands ?? []
    }

    var persistentDataDirectories: [PersistentData] {
        persistentData ?? []
    }

    var overviewMounts: [Mount] {
        visibleMounts.filter { !$0.isRecipeInfrastructureMount }
    }

    var appWorkingDirectoryMounts: [Mount] {
        visibleMounts.filter { !$0.isRecipeInfrastructureMount }
    }

    var runtimeDescription: String {
        guard let runtime else {
            switch runtimeKind {
            case "appleContainer":
                return "Apple container"
            default:
                return "Container"
            }
        }
        let operatingSystem = [
            runtime.operatingSystemName,
            runtime.operatingSystemVersion
        ]
        .filter { !$0.isEmpty }
        .joined(separator: " ")
        let description = [
            runtime.providerName,
            runtime.isolationName,
            operatingSystem,
            runtime.architecture
        ]
        .filter { !$0.isEmpty }
        .joined(separator: " · ")
        return isManagedContainer ? description : "Attached · \(description)"
    }

    var isManagedContainer: Bool {
        ownsContainer ?? (managementKind != "attached")
    }

    var canMountFoldersLive: Bool {
        capabilities?.supportsLiveMounts ?? supportsLiveMounts ?? false
    }

    var canConfigureFolders: Bool {
        capabilities?.supportsMounts ?? canMountFoldersLive
    }

    var outerShellSupportIssue: String? {
        guard let outerShellSupport else { return nil }
        switch outerShellSupport.status {
        case "missing", "inactive":
            return outerShellSupport.detail
        default:
            return nil
        }
    }
}

private struct LocalSafeSpaceProviderRecord: Decodable, Equatable {
    let id: String
    let name: String
    let detail: String
    let defaultBaseImage: String?
    let isolationName: String
    let isAvailable: Bool?
    let capabilities: LocalWorkspaceRecord.Capabilities

    var canCreate: Bool { isAvailable ?? true }
}

private struct LocalWorkspaceHostRequest: Encodable {
    let requestID: UUID
    let operation: String
    let workspaceID: UUID?
    let name: String?
    let frontendID: String?
    let serviceID: String?
    let listName: String?
    let cpus: Int?
    let memoryInGB: Int?
    let runtimeKind: String?
    let runtimeProviderID: String?
    let mountID: UUID?
    let readOnly: Bool?
    let recipeStepID: UUID?
    let catalogItemID: String?
    let command: String?
    let baseImage: String?
    let installsOuterShellSupport: Bool?
    let rebuild: Bool?
    let launcherKind: String?
    let workingDirectory: String?
    let recipeUserID: UUID?
    let recipeScriptPath: String?
    let dockerfile: String?
    let mounts: [ContainerConfigurationMountRequest]?
    let environment: [ContainerConfigurationEnvironmentRequest]?
    let publishedPorts: [ContainerConfigurationPublishedPortRequest]?
}

private struct ContainerConfigurationMountRequest: Encodable {
    let id: UUID
    let name: String
    let hostPath: String
    let guestPath: String
    let isReadOnly: Bool
}

private struct ContainerConfigurationEnvironmentRequest: Encodable {
    let name: String
    let value: String
}

private struct ContainerConfigurationPublishedPortRequest: Encodable {
    let hostPort: Int
    let containerPort: Int
}

private struct SafeSpaceSocketAPIRequest: Encodable {
    let requestID: UUID
    let operation: String
    let workspaceID: UUID?
    let socketPath: String
}

private struct LocalWorkspaceAppLogRecord: Decodable {
    let path: String
    let contents: String
    let isTruncated: Bool
    let fileSize: UInt64
    let modified: Double
    let error: String
}

private struct LocalWorkspaceHostResponse: Decodable {
    let requestID: UUID
    let providers: [LocalSafeSpaceProviderRecord]?
    let workspaces: [LocalWorkspaceRecord]
    let appLog: LocalWorkspaceAppLogRecord?
    let recipeCommandOutput: String?
    let recipeCommandApplied: Bool?
    let selectedFolderPath: String?
    let transferID: String?
    let fileName: String?
    let byteCount: Int?
    let stagedDirectly: Bool?
    let includedMountCount: Int?
    let omittedMountCount: Int?
    let importedWorkspaceID: String?
    let needsMountDestination: Bool?
    let importName: String?
    let suggestedMountRoot: String?
    let importMounts: [SharedContainerImportMount]?
    let error: String?
}

private extension LocalWorkspaceHostResponse {
    static func decode(_ data: Data, snapshotRequestID: UUID? = nil) throws -> LocalWorkspaceHostResponse {
        let payload: Data
        if let snapshotRequestID {
            guard var object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw URLError(.cannotParseResponse)
            }
            object["requestID"] = snapshotRequestID.uuidString
            payload = try JSONSerialization.data(withJSONObject: object)
        } else {
            payload = data
        }
        return try JSONDecoder().decode(LocalWorkspaceHostResponse.self, from: payload)
    }
}

private struct SharedContainerImportMount: Decodable {
    let id: UUID
    let name: String
    let sourceHostPath: String
    let guestPath: String
    let isReadOnly: Bool
    let directoryName: String
}

private struct PendingSharedContainerImport {
    let transferID: String
    let runtimeProviderID: String
    let name: String
    let mounts: [SharedContainerImportMount]
}

private struct ContainerTransferBinaryResponse {
    let nextOffset: UInt64
    let totalLength: UInt64
    let data: Data
}

private enum ContainerTransferBinaryCodec {
    private static let magic = Data([0x4f, 0x53, 0x43, 0x54])
    private static let version: UInt16 = 1
    private static let requestHeaderSize = 48
    private static let responseHeaderSize = 40
    static let chunkSize = 4_000_000
    static let requestTimeout: TimeInterval = 90
    static let maximumRetryCount = 8

    static func request(operation: UInt16,
                        transferID: UUID,
                        offset: UInt64,
                        length: UInt64,
                        data: Data = Data()) -> Data {
        var result = Data()
        result.append(magic)
        result.appendLittleEndian(version)
        result.appendLittleEndian(operation)
        var uuid = transferID.uuid
        withUnsafeBytes(of: &uuid) { result.append(contentsOf: $0) }
        result.appendLittleEndian(offset)
        result.appendLittleEndian(length)
        result.appendLittleEndian(data.isEmpty ? UInt32(0) : UInt32(requestHeaderSize))
        result.appendLittleEndian(UInt32(data.count))
        result.append(data)
        return result
    }

    static func response(from value: Data) throws -> ContainerTransferBinaryResponse {
        guard value.count >= responseHeaderSize,
              value.prefix(4) == magic,
              value.littleEndianUInt16(at: 4) == version,
              let status = value.littleEndianUInt16(at: 6),
              let nextOffset = value.littleEndianUInt64(at: 8),
              let totalLength = value.littleEndianUInt64(at: 16),
              let dataOffset = value.littleEndianUInt32(at: 24),
              let dataLength = value.littleEndianUInt32(at: 28),
              let errorOffset = value.littleEndianUInt32(at: 32),
              let errorLength = value.littleEndianUInt32(at: 36) else {
            throw ContainerConfigurationInputError(
                message: "The container service returned an invalid transfer response."
            )
        }
        let payload = try value.referencedData(offset: dataOffset, length: dataLength)
        let errorData = try value.referencedData(offset: errorOffset, length: errorLength)
        if status != 0 {
            let message = String(data: errorData, encoding: .utf8) ??
                "The container transfer failed."
            throw ContainerConfigurationInputError(message: message)
        }
        return ContainerTransferBinaryResponse(nextOffset: nextOffset,
                                               totalLength: totalLength,
                                               data: payload)
    }
}

private enum ContainerResumableUpload {
    static let segmentSize = 64 * 1024 * 1024
    static let requestTimeout: TimeInterval = 600
    static let maximumRetryCount = 8
}

private extension Data {
    mutating func appendLittleEndian<T: FixedWidthInteger>(_ value: T) {
        var encoded = value.littleEndian
        Swift.withUnsafeBytes(of: &encoded) { append(contentsOf: $0) }
    }

    func littleEndianUInt16(at offset: Int) -> UInt16? {
        littleEndianInteger(at: offset, as: UInt16.self)
    }

    func littleEndianUInt32(at offset: Int) -> UInt32? {
        littleEndianInteger(at: offset, as: UInt32.self)
    }

    func littleEndianUInt64(at offset: Int) -> UInt64? {
        littleEndianInteger(at: offset, as: UInt64.self)
    }

    private func littleEndianInteger<T: FixedWidthInteger>(at offset: Int,
                                                            as type: T.Type) -> T? {
        guard offset >= 0, offset <= count - MemoryLayout<T>.size else { return nil }
        return self[offset..<(offset + MemoryLayout<T>.size)].enumerated().reduce(T.zero) {
            $0 | (T($1.element) << T($1.offset * 8))
        }
    }

    func referencedData(offset: UInt32, length: UInt32) throws -> Data {
        if length == 0 { return Data() }
        let start = Int(offset)
        let count = Int(length)
        guard start >= 0, start <= self.count, count <= self.count - start else {
            throw ContainerConfigurationInputError(
                message: "The container service returned an invalid data reference."
            )
        }
        return subdata(in: start..<(start + count))
    }
}

private enum BaseImageTemplate {
    case outerShell
    case customWithSupport
    case customAsIs
}

private enum ContainerConfigurationTab {
    case dockerfile
    case mounts
    case environment
    case ports
    case runtime
}

private struct ContainerConfigurationMountDraft: Equatable {
    let id: UUID
    let name: String
    let hostPath: String
    let guestPath: String
    var isReadOnly: Bool
    let isInfrastructure: Bool
}

private struct ContainerConfigurationInputError: LocalizedError {
    let message: String

    var errorDescription: String? { message }
}

private struct PublishWorkspaceSocketHostResponse: Decodable {
    let requestID: UUID
    let publishedSocketPath: String?
    let error: String?
}

private struct AppLauncherEndpoint {
    let backend: BackendRecord
    let frontend: FrontendRecord
    let frontendIndex: Int
}

private enum AppLauncherScope: Hashable, Sendable {
    case server
    case container(UUID)
}

private let alwaysShownAppListName = "__outershell_always_shown__"
private let moreAppsListName = "__outershell_more_apps__"

private struct ContainerAppLauncherContext {
    let container: LocalWorkspaceRecord
    let app: LocalWorkspaceAppRecord
}

private struct AppLauncherItem {
    let identityKey: String
    let primaryEndpoint: AppLauncherEndpoint
    let userEndpoint: AppLauncherEndpoint?
    let rootEndpoint: AppLauncherEndpoint?
    let scope: AppLauncherScope
    let containerContext: ContainerAppLauncherContext?

    var backend: BackendRecord {
        primaryEndpoint.backend
    }

    var frontend: FrontendRecord {
        primaryEndpoint.frontend
    }

    var frontendIndex: Int {
        primaryEndpoint.frontendIndex
    }

    var displayName: String {
        let frontendName = frontend.name.trimmingCharacters(in: .whitespacesAndNewlines)
        if !frontendName.isEmpty {
            return frontendName
        }
        let backendName = backend.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        return backendName.isEmpty ? "App" : backendName
    }

    var subtitle: String {
        let backendName = backend.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        return backendName.isEmpty || backendName == displayName ? backend.serviceID : backendName
    }

    var iconCGImage: CGImage? {
        frontend.iconCGImage
    }

    var iconKey: String {
        identityKey
    }
}

private struct AppLauncherBadgeTarget {
    let frame: CGRect
    let endpoint: AppLauncherEndpoint
    let displayName: String
}

private func backendIdentityKey(_ backend: BackendRecord) -> String {
    let path = backend.serviceUnitPath ?? backend.serviceUnit
    return "\(backend.serviceID):\(backend.serviceScope):\(path)"
}

private func frontendIdentityKey(backend: BackendRecord, frontend: FrontendRecord, frontendIndex: Int) -> String {
    if !frontend.id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
        return "\(backendIdentityKey(backend)):frontend:\(frontend.id)"
    }
    let endpoint: String
    if !frontend.socketPath.isEmpty {
        endpoint = frontend.socketPath
    } else if frontend.port > 0 {
        endpoint = "port:\(frontend.port)"
    } else {
        endpoint = frontend.url
    }
    return "\(backendIdentityKey(backend)):frontend:\(frontendIndex):\(endpoint)"
}

private struct LogVisualLine {
    let range: NSRange
}

private struct LogVisualLineMetrics {
    let textWidth: CGFloat
    let charWidth: CGFloat
    let lineHeight: CGFloat
    let charactersPerLine: Int
    let lines: [LogVisualLine]
}

private struct PendingPasswordAction {
    let serviceID: String
    let serviceScope: String
    let operation: String
    let displayName: String
}

private struct PendingOuterShellUpdate {
    let backend: BackendRecord
    let installedVersion: String
    let availableVersion: String
    let message: String
}

private struct IconMatchState {
    let frame: CGRect
    let image: CGImage?
    let symbolName: String?
    let title: String
}

private struct TextMatchState {
    let frame: CGRect
    let title: String
    let fontSize: CGFloat
    let weight: NSFont.Weight
    let alignment: CATextLayerAlignmentMode
    let isWrapped: Bool
}

private struct CreateFieldLayout {
    let fieldFrame: CGRect
    let textFrame: CGRect
    let key: String
    let monospaced: Bool
    let multiline: Bool
}

private struct CreateTextLineFragment {
    let text: String
    let start: Int
    let end: Int
    let y: CGFloat
}

private struct PendingCreateTextDrag {
    let startPoint: CGPoint
    let cursorIndex: Int
    let selectedText: String
}

private struct DockerfileTextLine {
    let text: String
    let range: NSRange
    let frame: CGRect
}

private enum AppsTextContentSpace: Equatable {
    case apps
    case workspace
    case workspacePanel
}

private struct DockerfileTextBlock {
    let fragmentID: String
    let text: String
    let frame: CGRect
    let lines: [DockerfileTextLine]
    let font: NSFont
    let contentSpace: AppsTextContentSpace
    let selectionLayer: CALayer
}

private enum TextSelectionDragTarget {
    case createMessage
    case createField(String)
    case password
    case workspaceName
}

private struct PendingTextSelectionDrag {
    let target: TextSelectionDragTarget
}

private enum CreateSection: String {
    case appCatalog
    case bashCommands
    case nativeApp
    case otherRecipes
}

private enum PendingButtonAction {
    case addApp
    case appBadge(AppLauncherEndpoint, displayName: String, opensInNewTab: Bool)
    case bundledInstall(BackendRecord)
    case createCancel
    case createChoice(key: String, value: String)
    case createSubmit
    case createSuggestion(key: String, value: String)
    case chooseBashIcon
    case directorySelect(String)
    case filePickerCancel
    case filePickerEntry(FilePickerEntryRecord)
    case filePickerSave
    case installCancel
    case installConfirm(operation: String)
    case logDismiss
    case updateCancel
    case updateConfirm
    case aboutDismiss
    case passwordCancel
    case passwordSubmit
    case recipe(String)
    case createSection(CreateSection)
    case perform(() -> Void)
    case performAtPoint((CGPoint) -> Void)
}

private struct PendingButtonClick {
    let frame: CGRect
    let action: PendingButtonAction
}

private struct CopyConfirmation {
    let id: UUID
    let anchor: CGPoint
}

private enum AppDropTarget: Equatable {
    case pinned
    case moreApps
    case list(String)

    var listName: String {
        switch self {
        case .pinned:
            return alwaysShownAppListName
        case .moreApps:
            return moreAppsListName
        case .list(let name):
            return name
        }
    }
}

private struct PendingAppDrag {
    let item: AppLauncherItem
    let startPoint: CGPoint
    var currentPoint: CGPoint
    var isDragging: Bool
}

private struct PendingNativeProjectDrag {
    let project: GeneratedNativeAppProject
    let startPoint: CGPoint
}

private struct NativeProjectDragPreview {
    let pngData: Data
    let size: CGSize
    let frameOrigin: CGPoint
}

private enum NativeProjectSelectionState {
    case none
    case selected
}

private enum FilePickerMode {
    case chooseDirectory
    case chooseFile
}

private final class LogTextFragmentLayer: CALayer {
    var textLayoutFragment: NSTextLayoutFragment? {
        didSet {
            if textLayoutFragment !== oldValue {
                setNeedsDisplay()
            }
        }
    }
    var renderingSurfaceOffset: CGPoint = .zero {
        didSet {
            if renderingSurfaceOffset != oldValue {
                setNeedsDisplay()
            }
        }
    }

    override init() {
        super.init()
        contentsScale = NSScreen.main?.backingScaleFactor ?? 2
    }

    override init(layer: Any) {
        super.init(layer: layer)
        if let layer = layer as? LogTextFragmentLayer {
            textLayoutFragment = layer.textLayoutFragment
            renderingSurfaceOffset = layer.renderingSurfaceOffset
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func draw(in context: CGContext) {
        guard let textLayoutFragment else { return }

        context.saveGState()
        context.translateBy(x: 0, y: bounds.height)
        context.scaleBy(x: 1, y: -1)
        textLayoutFragment.draw(at: CGPoint(x: -renderingSurfaceOffset.x,
                                            y: -renderingSurfaceOffset.y),
                                in: context)
        context.restoreGState()
    }
}

private struct PendingFilePicker {
    let mode: FilePickerMode
    let targetFieldKey: String?
    var directory: String
    var parent: String
    var entries: [FilePickerEntryRecord]
    var isLoading: Bool
    var error: String
}

private struct OverviewLayout: Codable {
    var version = 1
    var pins: [String: [String]] = [:]
    var order: [String: [String]] = [:]
    var groups: [String] = []
    var names: [String: String] = [:]

    init() {}

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        version = try values.decodeIfPresent(Int.self, forKey: .version) ?? 1
        pins = try values.decodeIfPresent([String: [String]].self, forKey: .pins) ?? [:]
        order = try values.decodeIfPresent([String: [String]].self, forKey: .order) ?? [:]
        groups = try values.decodeIfPresent([String].self, forKey: .groups) ?? []
        names = try values.decodeIfPresent([String: String].self, forKey: .names) ?? [:]
    }

    static func decode(_ data: Data) throws -> (OverviewLayout, UInt64) {
        guard data.count >= 18, data.prefix(8) == Data("OSLAY001".utf8) else { throw URLError(.cannotParseResponse) }
        let revision = data[8..<16].enumerated().reduce(UInt64(0)) { $0 | UInt64($1.element) << ($1.offset * 8) }
        let layout = try JSONDecoder().decode(OverviewLayout.self, from: data.dropFirst(16))
        guard layout.version == 1 else { throw URLError(.cannotParseResponse) }
        return (layout, revision)
    }

    func encode(revision: UInt64) throws -> Data {
        var data = Data("OSLAY001".utf8)
        var littleEndianRevision = revision.littleEndian
        withUnsafeBytes(of: &littleEndianRevision) { data.append(contentsOf: $0) }
        data.append(try JSONEncoder().encode(self))
        return data
    }
}

private final class BackendsHandler: NSObject, OuterframeHostDelegate, SingleLineTextInputControllerDelegate, ScrollbarControllerDelegate {
    private let outerframeHost: OuterframeHost
    private let appConnection: OuterframeAppConnection
    private var retainedSelf: BackendsHandler?
    private var appearance: NSAppearance?
    private var didReceiveSystemAppearanceUpdate = false
    private var currentSize = CGSize(width: 900, height: 620)
    private var layoutUpdateScheduled = false
    private var scheduledLayoutNeedsScrollClamping = false
    private var iconSession: URLSession?
    private var endpointIconDiskCache: EndpointIconDiskCache?
    private var endpointIcons: [String: CGImage] = [:]
    private var terminalSymbolImages: [URL: CGImage] = [:]
    private var pendingIconURLs: Set<String> = []
    private var urlSession: URLSession?
    private var backendsEndpoint: URL?
    private var logsEndpoint: URL?
    private var controlEndpoint: URL?
    private var createEndpoint: URL?
    private var recipesEndpoint: URL?
    private var filePickerEndpoint: URL?
    private var eventsEndpoint: URL?
    private var nativeAppProjectEndpoint: URL?
    private var safeSpacesEndpoint: URL?
    private var nativeAppInstallSession: URLSession?
    private var safeSpaceOperationSession: URLSession?
    private var eventWatchTask: URLSessionDataTask?
    private var eventWatchGeneration = 0
    private var eventWatchRetryDelay: TimeInterval = 1
    private var backendsEventVersion: UInt64 = 0
    private var logEventVersion: UInt64 = 0
    private var overviewEventVersion: UInt64 = 0
    private var didRegisterLayer = false

    private var backends: [BackendRecord] = []
    private var backendError = ""
    private var selectedServiceID: String?
    private var selectedLog: LogSelection?
    private var selectedContainerLogContext: ContainerAppLauncherContext?
    private var logSnapshot: LogResponse?
    private var logError = ""
    private var isLoadingBackends = false
    private var isLoadingLog = false
    private var isLoadingRecipes = false
    private var isPerformingAction = false
    private var lastBackendsResponseData: Data?
    private var backendsRefreshGeneration = 0
    private var mode: BackendsViewMode = .apps
    private var recipes: [RecipeRecord] = []
    private var selectedRecipeID = "command-port"
    private var selectedCreateSection: CreateSection = .appCatalog
    private var createValues: [String: String] = [:]
    private var activeCreateFieldKey: String?
    private static let createFieldInputID = UUID()
    private static let createFieldPasteboardTypes = [
        NSPasteboard.PasteboardType.string.rawValue,
        NSPasteboard.PasteboardType.rtf.rawValue
    ]
    private lazy var createInputController: SingleLineTextInputController<BackendsHandler> = {
        let controller = SingleLineTextInputController<BackendsHandler>(
            identifier: Self.createFieldInputID,
            acceptedPasteboardTypeIdentifiers: Self.createFieldPasteboardTypes
        )
        controller.delegate = self
        controller.onSubmit = { [weak self] in
            Task { @MainActor in
                if self?.pendingFilePicker != nil {
                    self?.confirmFilePickerSave()
                } else {
                    self?.submitCreateForm()
                }
            }
        }
        return controller
    }()
    private var createMessage = ""
    private var createMessageFrame = CGRect.zero
    private var createMessageSelectionRange: NSRange?
    private var createMessageDragAnchorOffset: Int?
    private var logScroll: CGFloat = 0
    private var shouldScrollLogToBottomOnNextLayout = false
    private var logRenderedText = ""
    private var logAttributedText = NSAttributedString(string: "")
    private var renderedLogHeaderDetailText = ""
    private var logHeaderDetailFrame = CGRect.zero
    private var logDismissFrame = CGRect.zero
    private var logHeaderDetailSelectionRange: NSRange?
    private var logHeaderDetailDragAnchorOffset: Int?
    private var renderedStatusText = ""
    private var statusSelectionRange: NSRange?
    private var statusDragAnchorOffset: Int?
    private var logTextSelectionRange: NSRange?
    private var logDragAnchorOffset: Int?
    private var lastLogDragTextPoint: CGPoint?
    private var logTextSelectionLayers: [CALayer] = []
    private var logTextFragmentLayers: [ObjectIdentifier: LogTextFragmentLayer] = [:]
    private var logTextLayoutWidth: CGFloat = 0
    private var logTextContentGeneration = 0
    private var logContentHeightCache: (generation: Int, textWidth: CGFloat, height: CGFloat)?
    private var logVisualLineCache: (generation: Int, textWidth: CGFloat, metrics: LogVisualLineMetrics)?
    private var logTextFragmentCoverage: (generation: Int, textWidth: CGFloat, contentHeight: CGFloat, rect: CGRect)?
    private var logTextSelectionCoverage: (generation: Int, textWidth: CGFloat, contentHeight: CGFloat, range: NSRange, rect: CGRect)?
    private var logScrollbarController: ScrollbarController<BackendsHandler>?
    private lazy var filePickerScrollbarDelegate = FilePickerScrollbarDelegate(owner: self)
    private var filePickerScrollbarController: ScrollbarController<FilePickerScrollbarDelegate>?
    private let logContentStorage = NSTextContentStorage()
    private let logTextLayoutManager = NSTextLayoutManager()
    private let logTextContainer = NSTextContainer(size: CGSize(width: 320, height: 1_000_000))
    private var appsScroll: CGFloat = 0
    private var workspaceScroll: CGFloat = 0
    private var createScroll: CGFloat = 0
    private var createContentBottom: CGFloat = 0
    private var viewHasFocus = true
    private var windowIsActive = true
    private var isSynchronizingCreateInput = false
    private var isSynchronizingPasswordInput = false
    private var pendingCreateTextDrag: PendingCreateTextDrag?
    private var pendingTextSelectionDrag: PendingTextSelectionDrag?
    private var currentCursor: PluginCursorType = .arrow
    private var pendingPasswordAction: PendingPasswordAction?
    private static let passwordFieldInputID = UUID()
    private static let passwordFieldPasteboardTypes = [
        NSPasteboard.PasteboardType.string.rawValue,
        NSPasteboard.PasteboardType.rtf.rawValue
    ]
    private lazy var passwordInputController: SingleLineTextInputController<BackendsHandler> = {
        let controller = SingleLineTextInputController<BackendsHandler>(
            identifier: Self.passwordFieldInputID,
            acceptedPasteboardTypeIdentifiers: Self.passwordFieldPasteboardTypes
        )
        controller.masksWordBoundaries = true
        controller.delegate = self
        controller.onSubmit = { [weak self] in
            Task { @MainActor in self?.submitPasswordPrompt() }
        }
        return controller
    }()
    private var sudoPasswordInput = ""
    private var sudoPasswordMessage = ""
    private var pendingFilePicker: PendingFilePicker?
    private var accessibilityNotificationScheduled = false
    private var isShowingWorkspacePanel = false
    private var localWorkspaces: [LocalWorkspaceRecord] = []
    private var availableSafeSpaceProviders: [LocalSafeSpaceProviderRecord] = []
    private var workspacePanelMessage = ""
    private var copyConfirmation: CopyConfirmation?
    private var isPerformingWorkspaceOperation = false
    private var addWorkspaceMessage = ""
    private var isRefreshingWorkspaces = false
    private var pendingWorkspaceOperations: [UUID: String] = [:]
    private var pendingWorkspaceOperationWorkspaceIDs: [UUID: UUID] = [:]
    private var pendingWorkspaceCreationNames: [UUID: String] = [:]
    private var pendingWorkspaceSocketPublications: [UUID: (String?, String?) -> Void] = [:]
    private var workspaceContextID: UUID?
    private var workspaceOverviewAvailable = false
    private var workspaceRefreshGeneration = 0
    private var containerBuildProgressRefreshGeneration = 0
    private var containerConfigurationRebuildWorkspaceID: UUID?
    private static let workspaceRenameInputID = UUID()
    private static let workspaceRenamePasteboardTypes = [
        NSPasteboard.PasteboardType.string.rawValue,
        NSPasteboard.PasteboardType.rtf.rawValue
    ]
    private lazy var workspaceRenameInputController: SingleLineTextInputController<BackendsHandler> = {
        let controller = SingleLineTextInputController<BackendsHandler>(
            identifier: Self.workspaceRenameInputID,
            acceptedPasteboardTypeIdentifiers: Self.workspaceRenamePasteboardTypes
        )
        controller.delegate = self
        controller.onSubmit = { [weak self] in
            Task { @MainActor in self?.submitWorkspaceRename() }
        }
        return controller
    }()
    private var pendingWorkspaceRename: LocalWorkspaceRecord?
    private var pendingWorkspaceDeletion: LocalWorkspaceRecord?
    private var isCreatingWorkspace = false
    private var selectedSafeSpaceProviderID = "apple.container"
    private var creationBaseImage = "debian:bookworm"
    private var baseImageTemplate = BaseImageTemplate.outerShell
    private var pendingCreationContainerName = ""
    private var isEditingCreationBaseImage = false
    private var workspaceNamePromptDismissesPanel = false
    private var workspaceRenameName = ""
    private var isSynchronizingWorkspaceRenameInput = false
    private var pendingSharedContainerImport: PendingSharedContainerImport?
    private var selectedRecipeSafeSpaceID: UUID?
    private var pendingRecipeCommandWorkspace: LocalWorkspaceRecord?
    private var pendingDockerfileWorkspace: LocalWorkspaceRecord?
    private var pendingRecipeBaseImageWorkspace: LocalWorkspaceRecord?
    private var pendingRecipeUserWorkspace: LocalWorkspaceRecord?
    private var pendingRecipeEditWorkspace: LocalWorkspaceRecord?
    private var pendingRecipeEditStep: LocalWorkspaceRecord.Recipe.Step?
    private var pendingRecipeScriptWorkspace: LocalWorkspaceRecord?
    private var pendingRecipeScriptUserID: UUID?
    private var pendingRecipeScriptRename: LocalWorkspaceRecord.Recipe.ScriptFile?
    private var pendingSafeSpaceAppWorkspace: LocalWorkspaceRecord?
    private var pendingSafeSpaceAppKind: String?
    private var safeSpaceRecipeMessage = ""
    private var containerConfigurationTab = ContainerConfigurationTab.dockerfile
    private var containerConfigurationDockerfile = ""
    private var containerConfigurationSavedDockerfile = ""
    private var containerConfigurationEnvironment = ""
    private var containerConfigurationSavedEnvironment = ""
    private var containerConfigurationPorts = ""
    private var containerConfigurationSavedPorts = ""
    private var containerConfigurationMounts: [ContainerConfigurationMountDraft] = []
    private var containerConfigurationSavedMounts: [ContainerConfigurationMountDraft] = []
    private var containerConfigurationRequiresRebuild = false
    private var isConfirmingContainerConfigurationRebuild = false
    private var isConfirmingContainerConfigurationDismissal = false
    private var containerConfigurationRebuildAfterDockerfileSave = false
    private var containerConfigurationDismissAfterDockerfileSave = false
    private var containerConfigurationEnvironmentSaveGeneration = 0
    private var pendingContainerConfigurationDockerfileSave: String?
    private var pendingContainerConfigurationPreviousSavedDockerfile: String?
    private var pendingContainerConfigurationPreviousRequiresRebuild: Bool?
    private var pendingContainerConfigurationRuntimeSave: (
        environment: String,
        ports: String,
        mounts: [ContainerConfigurationMountDraft]
    )?
    private var containerConfigurationTextScroll: CGFloat = 0
    private var containerConfigurationMountScroll: CGFloat = 0
    private var containerConfigurationRenderedTextScroll: CGFloat = 0
    private var containerConfigurationTextViewportLayer: CALayer?
    private var containerConfigurationTextSelectionLayer: CALayer?
    private var containerConfigurationTextFragments: [CreateTextLineFragment] = []
    private var containerConfigurationTextFragmentsText = ""
    private var containerConfigurationTextFragmentsWidth: CGFloat = 0
    private var containerConfigurationBuildError: String?
    private var containerConfigurationBuildErrorScroll: CGFloat = 0
    private var containerConfigurationBuildErrorRenderedScroll: CGFloat = 0
    private var containerConfigurationBuildErrorContentHeight: CGFloat = 0
    private var containerConfigurationBuildErrorViewportLayer: CALayer?
    private var containerConfigurationBuildErrorCopyConfirmationID: UUID?

    private var isWorkspaceNamePromptVisible: Bool {
        isCreatingWorkspace || pendingWorkspaceRename != nil ||
            pendingRecipeCommandWorkspace != nil || pendingRecipeUserWorkspace != nil ||
            pendingDockerfileWorkspace != nil ||
            pendingRecipeBaseImageWorkspace != nil ||
            pendingRecipeEditStep != nil ||
            pendingRecipeScriptWorkspace != nil ||
            pendingSafeSpaceAppWorkspace != nil ||
            pendingSharedContainerImport != nil
    }

    private var isRenamingContainerConfiguration: Bool {
        guard let pendingWorkspaceRename,
              let pendingDockerfileWorkspace else {
            return false
        }
        return pendingWorkspaceRename.id == pendingDockerfileWorkspace.id
    }

    private var isDockerfileFragmentPrompt: Bool {
        if isRenamingContainerConfiguration {
            return false
        }
        return pendingRecipeCommandWorkspace != nil || pendingRecipeEditStep != nil ||
            pendingDockerfileWorkspace != nil
    }

    private var isBaseImageChoicePrompt: Bool {
        pendingRecipeBaseImageWorkspace != nil ||
            (isCreatingWorkspace && isEditingCreationBaseImage)
    }

    private let rootLayer = CALayer()
    private let toolbarLayer = CALayer()
    private let titleLayer = CATextLayer()
    private let statusSelectionLayer = CALayer()
    private let statusLayer = CATextLayer()
    private let privilegedAppsHeaderLayer = CATextLayer()
    private let safeSpacesHeaderLayer = CATextLayer()
    private let outerShellActionLayer = SymbolButtonLayer(symbolName: "ellipsis.circle", accessibilityTitle: "Outer Shell Actions")
    private let contentLayer = CALayer()
    private let appsLayer = CALayer()
    private let appsScrollContentLayer = CALayer()
    private let workspacePaneClipLayer = CALayer()
    private let workspaceScrollContentLayer = CALayer()
    private let appsOverlayLayer = CALayer()
    private let logHeaderLayer = CALayer()
    private let logRowsClipLayer = CALayer()
    private let logTextContentLayer = CALayer()
    private let logTextSelectionLayer = CALayer()
    private let dividerLayer = CALayer()
    private let createLayer = CALayer()
    private let createFormContentLayer = CALayer()
    private let iconTransitionLayer = CALayer()
    private let installOverlayLayer = CALayer()
    private let updateOverlayLayer = CALayer()
    private let aboutOverlayLayer = CALayer()
    private let aboutSelectionLayer = CALayer()
    private let passwordOverlayLayer = CALayer()
    private let workspaceOverlayLayer = CALayer()
    private let workspacePanelLayer = CALayer()
    private let copyConfirmationLayer = CALayer()
    private let filePickerOverlayLayer = CALayer()
    private let filePickerListLayer = CALayer()
    private let filePickerRowsContentLayer = CALayer()
    private let filePickerStatusLayer = CALayer()
    private var filePickerVisibleRowLayers: [Int: CALayer] = [:]
    private var filePickerReusableRowLayers: [CALayer] = []

    private var appCardFrames: [(frame: CGRect, item: AppLauncherItem)] = []
    private var appBadgeFrames: [AppLauncherBadgeTarget] = []
    private var appListDropFrames: [(frame: CGRect, listName: String, scope: AppLauncherScope)] = []
    private var appUnlistedDropFrames: [(
        frame: CGRect,
        scope: AppLauncherScope,
        target: AppDropTarget
    )] = []
    private var appOverflowFrames: [(frame: CGRect, scope: AppLauncherScope)] = []
    private var expandedAppOverflowScopes = Set<AppLauncherScope>()
    private var appOverflowAnimationProgress: [AppLauncherScope: CGFloat] = [:]
    private var appOverflowAnimationTargets: [AppLauncherScope: CGFloat] = [:]
    private var appOverflowAnimationGenerations: [AppLauncherScope: Int] = [:]
    private var addAppFrame = CGRect.zero
    private var outerShellActionFrame = CGRect.zero
    private var appsContentBottom: CGFloat = 0
    private var workspaceContentBottom: CGFloat = 0
    private var workspacePaneFrame = CGRect.zero
    private var usesWorkspaceSplitLayout = false
    private var isRenderingWorkspacePane = false
    private var workspaceVisibleIconKeys = Set<String>()
    private var workspaceVisibleTextKeys = Set<String>()
    private var appsRenderTargetOverride: CALayer?
    private var pendingAppDrag: PendingAppDrag?
    private var pendingNativeProjectDrag: PendingNativeProjectDrag?
    private var generatedNativeProject: GeneratedNativeAppProject?
    private var generatedNativeProjectWasExported = false
    private var nativeProjectDragFrame = CGRect.zero
    private var nativeProjectSelectionState: NativeProjectSelectionState = .none
    private var nativeFilePromiseURLs: [UUID: URL] = [:]
    private var pendingButtonClick: PendingButtonClick?
    private var pendingOuterShellUpdate: PendingOuterShellUpdate?
    private var didShowAutomaticOuterShellUpdatePrompt = false
    private var pendingAboutBackend: BackendRecord?
    private var iconMatchStates: [String: IconMatchState] = [:]
    private var iconMatchLayers: [String: CALayer] = [:]
    private struct RunningBadgeSymbolKey: Hashable {
        let symbolName: String
        let pointSize: CGFloat
        let isRoot: Bool
    }
    private var runningBadgeSymbolImages: [RunningBadgeSymbolKey: (image: CGImage, size: CGSize)] = [:]
    private var textMatchStates: [String: TextMatchState] = [:]
    private var textMatchLayers: [String: CATextLayer] = [:]
    private var pendingMenuActions: [UUID: (serviceID: String, serviceScope: String, operationByItemID: [String: String])] = [:]
    private var pendingAppMenuActions: [UUID: (item: AppLauncherItem, operationByItemID: [String: String])] = [:]
    private var pendingWorkspaceOverviewMenuActions: [UUID: (
        workspace: LocalWorkspaceRecord,
        operationByItemID: [String: String],
        anchor: CGPoint
    )] = [:]
    private var pendingContainerCommandMenuActions: [UUID: (
        workspace: LocalWorkspaceRecord,
        command: LocalWorkspaceCommandRecord?,
        commandByItemID: [String: String],
        anchor: CGPoint
    )] = [:]
    private var pendingShareScopeMenuActions: [UUID: (
        workspace: LocalWorkspaceRecord,
        options: [String: (includePersistentData: Bool, includeMountedFolders: Bool)]
    )] = [:]
    private var pendingImportRuntimeMenuActions: [UUID: (url: URL, providerByItemID: [String: String])] = [:]
    private var sharedContainerFile: (url: URL, name: String, byteCount: Int)?
    private var sharedContainerDragFrame = CGRect.zero
    private var sharedContainerCloseFrame = CGRect.zero
    private var pendingSharedContainerDrag = false
    private var pendingSafeSpaceProviderMenuSelections: [UUID: [String: String]] = [:]
    private var pendingContainerPathMenuSelections: [UUID: [String: String]] = [:]
    private var pendingSafeSpaceAppMenuSelections: [UUID: (
        workspace: LocalWorkspaceRecord,
        kindByItemID: [String: String]
    )] = [:]
    private var pendingRecipeStepMenuSelections: [UUID: (
        workspaceID: UUID,
        stepID: UUID,
        rebuildByItemID: [String: Bool]
    )] = [:]
    private var pendingLogMenuSelections: [UUID: (serviceID: String, serviceScope: String, logIndexByItemID: [String: Int])] = [:]
    private var logSelectorFrame = CGRect.zero
    private var createSectionFrames: [(frame: CGRect, section: CreateSection)] = []
    private var recipeFrames: [(frame: CGRect, recipeID: String)] = []
    private var bundledAppInstallFrames: [(frame: CGRect, backend: BackendRecord)] = []
    private var createFieldFrames: [(frame: CGRect, key: String)] = []
    private var createFieldLayouts: [String: CreateFieldLayout] = [:]
    private var createChoiceFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createSuggestionFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createDirectorySelectFrames: [(frame: CGRect, key: String)] = []
    private var createContentClipFrame = CGRect.zero
    private var createDismissFrame = CGRect.zero
    private var createButtonFrame = CGRect.zero
    private var cancelCreateFrame = CGRect.zero
    private var bashIconSelectFrame = CGRect.zero
    private var passwordFieldFrame = CGRect.zero
    private var passwordTextFrame = CGRect.zero
    private var passwordSubmitFrame = CGRect.zero
    private var passwordCancelFrame = CGRect.zero
    private var passwordPanelFrame = CGRect.zero
    private var updatePanelFrame = CGRect.zero
    private var updateCancelFrame = CGRect.zero
    private var updateConfirmFrame = CGRect.zero
    private var aboutPanelFrame = CGRect.zero
    private var aboutDoneFrame = CGRect.zero
    private var aboutTextFrame = CGRect.zero
    private var aboutSelectionRange: NSRange?
    private var aboutDragAnchorOffset: Int?
    private var renderedAboutText = ""
    private var pendingInstallBackend: BackendRecord?
    private var pendingInstallOperation: String = "run"
    private var installPanelFrame = CGRect.zero
    private var installConfirmFrame = CGRect.zero
    private var installRootConfirmFrame = CGRect.zero
    private var installCancelFrame = CGRect.zero
    private var filePickerPanelFrame = CGRect.zero
    private var filePickerEntryFrames: [(frame: CGRect, entry: FilePickerEntryRecord, index: Int)] = []
    private var filePickerBreadcrumbFrame = CGRect.zero
    private var filePickerBreadcrumbSegmentFrames: [(frame: CGRect, path: String)] = []
    private var filePickerSelectedIndex: Int?
    private var filePickerTypeaheadPrefix = ""
    private var filePickerTypeaheadLastUpdated: Date?
    private var filePickerSaveFrame = CGRect.zero
    private var filePickerCancelFrame = CGRect.zero
    private var filePickerListFrame = CGRect.zero
    private var filePickerContentHeight: CGFloat = 0
    private var filePickerScroll: CGFloat = 0
    private let filePickerRowHeight: CGFloat = 28
    private var workspacePanelFrame = CGRect.zero
    private var workspaceCloseFrame = CGRect.zero
    private var workspaceOverviewRowFrames: [(frame: CGRect, workspace: LocalWorkspaceRecord)] = []
    private var workspaceOverviewActionFrames: [(frame: CGRect, workspace: LocalWorkspaceRecord, operation: String)] = []
    private var workspaceOverviewAppFrames: [(frame: CGRect, workspace: LocalWorkspaceRecord, app: LocalWorkspaceAppRecord)] = []
    private var overviewMenuFrames: [(frame: CGRect, item: AppLauncherItem)] = []
    private var overviewAddFrames: [CGRect] = []
    private var overviewDropFrames: [(frame: CGRect, group: String, target: AppDropTarget)] = []
    private var overviewGroupFrames: [(frame: CGRect, header: CGRect, id: String)] = []
    private let overviewDragFeedbackLayer = CALayer()
    private var overviewDragPreview: CALayer?
    private var overviewDragPreviewKey: String?
    private var overviewDragFrameScheduled = false
    private var pendingOverviewGroupDrag: (id: String, start: CGPoint, current: CGPoint, active: Bool)?
    private var overviewUsername = "Your user"
    private var overviewLayout = OverviewLayout()
    private var overviewLayoutRevision: UInt64 = 0
    private var overviewLayoutReady = false
    private var overviewLayoutSaving = false
    private var overviewLayoutRequestPending = false
    private var workspaceOverviewCreateFrame = CGRect.zero
    private var safeSpaceDetailBackFrame = CGRect.zero
    private var safeSpaceDetailAddAppFrames: [CGRect] = []
    private var safeSpaceDetailAddStepFrame = CGRect.zero
    private var safeSpaceDetailAddUserFrame = CGRect.zero
    private var safeSpaceDetailRebuildFrame = CGRect.zero
    private var safeSpaceDetailCopyContainerfileFrame = CGRect.zero
    private var safeSpaceDetailEditDockerfileFrame = CGRect.zero
    private var safeSpaceDetailOpenDockerfileFrame = CGRect.zero
    private var safeSpaceDetailCopySupportSnippetFrame = CGRect.zero
    private var safeSpaceDetailCopyRecipeMessageFrame = CGRect.zero
    private var safeSpaceDetailEditBaseImageFrame = CGRect.zero
    private var safeSpaceDetailStepFrames: [(frame: CGRect, step: LocalWorkspaceRecord.Recipe.Step)] = []
    private var safeSpaceDetailEditStepFrames: [(frame: CGRect, step: LocalWorkspaceRecord.Recipe.Step)] = []
    private var safeSpaceDetailAddScriptFrames: [(frame: CGRect, userID: UUID?)] = []
    private var safeSpaceDetailEditScriptFrames: [(frame: CGRect, script: LocalWorkspaceRecord.Recipe.ScriptFile)] = []
    private var safeSpaceDetailRenameScriptFrames: [(frame: CGRect, script: LocalWorkspaceRecord.Recipe.ScriptFile)] = []
    private var safeSpaceDetailCatalogFrames: [(frame: CGRect, item: LocalWorkspaceRecord.Recipe.CatalogItem)] = []
    private var safeSpaceDockerfileTextBlocks: [DockerfileTextBlock] = []
    private var selectedDockerfileFragmentID: String?
    private var dockerfileSelectionRange: NSRange?
    private var dockerfileDragAnchorOffset: Int?
    private var workspaceRenamePanelFrame = CGRect.zero
    private var workspaceRenameFieldFrame = CGRect.zero
    private var workspaceRenameTextFrame = CGRect.zero
    private var workspaceRenameCancelFrame = CGRect.zero
    private var workspaceRenameConfirmFrame = CGRect.zero
    private var workspaceCreationBaseImageFrame = CGRect.zero
    private var workspaceOuterShellBaseImageFrame = CGRect.zero
    private var workspaceCustomBaseImageFrame = CGRect.zero
    private var workspaceCustomAsIsFrame = CGRect.zero
    private var workspaceDeletePanelFrame = CGRect.zero
    private var workspaceDeleteCancelFrame = CGRect.zero
    private var workspaceDeleteConfirmFrame = CGRect.zero
    private var containerConfigurationDockerfileTabFrame = CGRect.zero
    private var containerConfigurationMountsTabFrame = CGRect.zero
    private var containerConfigurationEnvironmentTabFrame = CGRect.zero
    private var containerConfigurationPortsTabFrame = CGRect.zero
    private var containerConfigurationRuntimeTabFrame = CGRect.zero
    private var containerConfigurationChangeRuntimeFrame = CGRect.zero
    private var containerConfigurationEditorToolbarFrame = CGRect.zero
    private var containerConfigurationCookbookFrame = CGRect.zero
    private var containerConfigurationCopyPathFrame = CGRect.zero
    private var containerConfigurationTextVisibleFrame = CGRect.zero
    private var containerConfigurationAddMountFrame = CGRect.zero
    private var containerConfigurationContentFrame = CGRect.zero
    private var containerConfigurationMountActionFrames: [(frame: CGRect, id: UUID, action: String)] = []
    private var containerConfigurationSaveFrame = CGRect.zero
    private var containerConfigurationDiscardFrame = CGRect.zero
    private var containerConfigurationRebuildFrame = CGRect.zero
    private var containerConfigurationRebuildPromptFrame = CGRect.zero
    private var containerConfigurationRebuildSaveFrame = CGRect.zero
    private var containerConfigurationRebuildWithoutSavingFrame = CGRect.zero
    private var containerConfigurationRebuildCancelFrame = CGRect.zero
    private var containerConfigurationBuildErrorTextFrame = CGRect.zero
    private var containerConfigurationBuildErrorCopyFrame = CGRect.zero
    private var containerConfigurationBuildErrorDismissFrame = CGRect.zero
    private var containerConfigurationDismissPromptFrame = CGRect.zero
    private var containerConfigurationDismissSaveFrame = CGRect.zero
    private var containerConfigurationDismissWithoutSavingFrame = CGRect.zero
    private var containerConfigurationDismissCancelFrame = CGRect.zero
    private var containerConfigurationRenameFrame = CGRect.zero

    private var isContainerConfigurationEditorVisible: Bool {
        isShowingWorkspacePanel && pendingDockerfileWorkspace != nil &&
            selectedRecipeSafeSpaceID == pendingDockerfileWorkspace?.id
    }

    private let toolbarHeight: CGFloat = 48
    private let wheelScrollLineHeight: CGFloat = 44
    private let logHeaderHeight: CGFloat = 62
    private let logTextInsetX: CGFloat = 12
    private let logTextInsetY: CGFloat = 10
    private let logTextMeasurementHeight: CGFloat = 10_000_000
    private let logScrollLineHeight: CGFloat = 18
    private let horizontalInset: CGFloat = 18
    private let createBottomInset: CGFloat = 18
    private let textCaretBlinkAnimationKey = "textCaretBlink"
    init(outerframeHost: OuterframeHost, appConnection: OuterframeAppConnection) {
        self.outerframeHost = outerframeHost
        self.appConnection = appConnection
        super.init()
        logTextContainer.lineFragmentPadding = 0
        logTextLayoutManager.textContainer = logTextContainer
        logTextLayoutManager.usesFontLeading = true
        logContentStorage.addTextLayoutManager(logTextLayoutManager)
        logContentStorage.attributedString = logAttributedText
        retainedSelf = self
    }

    func outerframeHost(_ host: OuterframeHost, didReceiveMessage message: BrowserToContentMessage) {
        switch message {
        case .initializeContent(let arguments):
            let initialURL = arguments.url ?? ""
            outerframeHost.configure(url: initialURL,
                                     bundleUrl: arguments.bundleUrl ?? "",
                                     proxyHost: arguments.proxy?.host,
                                     proxyPort: arguments.proxy?.port ?? 0,
                                     proxyUsername: arguments.proxy?.username,
                                     proxyPassword: arguments.proxy?.password)
            outerframeHost.setTitle("Outer Shell")
            outerframeHost.setIcon(.bundleResource(path: "Contents/Resources/app-icon.png"))
            mode = modeFromURL(initialURL)
            workspaceContextID = workspaceIDFromURL(initialURL)
            selectedRecipeSafeSpaceID = recipeSafeSpaceIDFromURL(initialURL)
            didReceiveSystemAppearanceUpdate = false
            appearance = arguments.appearance ?? NSAppearance.currentDrawing()
            currentSize = arguments.contentSize ?? currentSize
            configureNetworking()
            configureLayersIfNeeded()
            updateColors()
            registerRootLayerIfNeeded()
            updateInputMode()
            updateEditingAndPasteboardState()
            outerframeHost.setPasteboardDropBehaviorHitTest(
                acceptedTypes: Self.createFieldPasteboardTypes + [
                    NSPasteboard.PasteboardType.fileURL.rawValue
                ]
            )
            loadOverviewLayout()
            outerframeHost.requestOuterLoopSSHCommandArguments { [weak self] arguments in
                guard let self, let arguments else { return }
                if let index = arguments.firstIndex(of: "-l"), arguments.indices.contains(index + 1) {
                    self.overviewUsername = arguments[index + 1]
                } else if let destination = arguments.last, let at = destination.firstIndex(of: "@") {
                    self.overviewUsername = String(destination[..<at])
                }
                self.updateLayout()
            }
            fetchBackends()
            if mode == .create { fetchRecipes() }
            startEventWatch()
            if workspaceContextID == nil {
                sendWorkspaceRequest(operation: "list")
            }

        case .resizeContent(let size):
            currentSize = size
            scheduleResizeLayoutUpdate()

        case .systemAppearanceUpdate(let appearance):
            let repeatsInitialAppearance = !didReceiveSystemAppearanceUpdate && self.appearance?.name == appearance.name
            didReceiveSystemAppearanceUpdate = true
            self.appearance = appearance
            if !repeatsInitialAppearance {
                updateColors()
            }

        case .scrollWheelEvent(let point, let delta, _, _, _, let hasPreciseScrollingDeltas):
            handleScroll(at: point, delta: delta, precise: hasPreciseScrollingDeltas)

        case .mouseDown(let point, let modifierFlags, let clickCount):
            if modifierFlags.contains(.control) {
                handleRightMouseDown(at: point, modifierFlags: modifierFlags, clickCount: clickCount)
            } else {
                handleMouseDown(at: point, modifierFlags: modifierFlags, clickCount: clickCount)
            }

        case .mouseDragged(let point, let modifierFlags):
            handleMouseDragged(to: point, modifierFlags: modifierFlags)

        case .mouseUp(let point, let modifierFlags):
            handleMouseUp(at: point, modifierFlags: modifierFlags)

        case .mouseMoved(let point, let modifierFlags):
            handleMouseMoved(to: point, modifierFlags: modifierFlags)

        case .rightMouseDown(let point, let modifierFlags, let clickCount):
            handleRightMouseDown(at: point, modifierFlags: modifierFlags, clickCount: clickCount)

        case .contextMenuItemSelected(let menuID, let itemID):
            handleContextMenuSelection(menuID: menuID, itemID: itemID)

        case .keyDown(let keyCode, let characters, let charactersIgnoringModifiers, let modifierFlags, let isARepeat):
            handleKeyDown(keyCode: keyCode,
                          characters: characters,
                          charactersIgnoringModifiers: charactersIgnoringModifiers,
                          modifierFlags: modifierFlags,
                          isARepeat: isARepeat)

        case .textInput(let text, let hasReplacementRange, let replacementLocation, let replacementLength):
            if isWorkspaceNamePromptVisible {
                insertWorkspaceRenameText(text,
                                          hasReplacementRange: hasReplacementRange,
                                          replacementLocation: replacementLocation,
                                          replacementLength: replacementLength)
            } else if pendingPasswordAction != nil {
                insertPasswordText(text,
                                   hasReplacementRange: hasReplacementRange,
                                   replacementLocation: replacementLocation,
                                   replacementLength: replacementLength)
            } else if mode == .create {
                insertCreateText(text,
                                 hasReplacementRange: hasReplacementRange,
                                 replacementLocation: replacementLocation,
                                 replacementLength: replacementLength)
            }

        case .setMarkedText(let text, let selectedLocation, let selectedLength, let hasReplacementRange, let replacementLocation, let replacementLength):
            handleSetMarkedText(text,
                                selectedLocation: Int(selectedLocation),
                                selectedLength: Int(selectedLength),
                                hasReplacementRange: hasReplacementRange,
                                replacementLocation: Int(replacementLocation),
                                replacementLength: Int(replacementLength))

        case .unmarkText:
            handleUnmarkText()

        case .textCommand(let command):
            handleTextCommand(command)

        case .textInputFocus(let fieldID, let hasFocus):
            handleTextInputFocus(fieldID: fieldID, hasFocus: hasFocus)

        case .setCursorPosition(let fieldID, let position, let modifySelection):
            handleSetCursorPosition(fieldID: fieldID, position: Int(position), modifySelection: modifySelection)

        case .viewFocusChanged(let isFocused):
            viewHasFocus = isFocused
            if !isFocused {
                blurCreateField()
                blurPasswordField()
                blurWorkspaceRenameField()
            }
            updateEditingAndPasteboardState()

        case .windowActiveUpdate(let isActive):
            guard windowIsActive != isActive else { return }
            windowIsActive = isActive
            updateWindowActiveAppearance()

        case .selectionToPasteboardCopyRequest(let requestID):
            outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID,
                                                              items: pasteboardItemsForCopy())

        case .selectionToPasteboardCutRequest(let requestID):
            outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID,
                                                              items: pasteboardItemsForCut())

        case .editCommandValidationRequest(let requestID, let commands):
            outerframeHost.sendEditCommandValidationResponse(
                requestID: requestID,
                enabledCommands: enabledEditCommands(in: commands)
            )

        case .pasteboardContentPasted(let items):
            handlePasteboardItemsForPaste(items)

        case .pasteboardDropHitTestRequest(let requestID, let point, let pasteboardTypes, let operationMask, _):
            let accepted = createFieldAcceptsTextDrop(at: point,
                                                      pasteboardTypes: pasteboardTypes,
                                                      operationMask: operationMask) ||
                           passwordFieldAcceptsTextDrop(at: point,
                                                        pasteboardTypes: pasteboardTypes,
                                                        operationMask: operationMask) ||
                           sharedContainerFileAcceptsDrop(pasteboardTypes: pasteboardTypes,
                                                          operationMask: operationMask)
            outerframeHost.sendPasteboardDropHitTestResponse(requestID: requestID,
                                                             operationMask: accepted ? .copy : [])

        case .pasteboardContentDropped(let point, let items):
            handlePasteboardItemsForDrop(at: point, items: items)

        case .hostSpecificMessage:
            break

        case .hostSpecificMessageUnrecognized:
            break

        case .filePromiseWriteRequest(let requestID, let promiseID):
            handleFilePromiseWriteRequest(requestID: requestID, promiseID: promiseID)

        case .historyTraversal(_, let url):
            blurCreateField()
            applyNavigation(url)

        case .accessibilitySnapshotRequest(let requestID):
            outerframeHost.sendAccessibilitySnapshotResponse(requestID: requestID,
                                                             snapshot: buildAccessibilitySnapshot())

        case .shutdown:
            stopEventWatch()
            retainedSelf = nil

        default:
            break
        }
    }

    func outerframeHostDidDisconnect(_ host: OuterframeHost) {
        stopEventWatch()
        retainedSelf = nil
    }

    private func configureNetworking() {
        if let base = outerframeHost.pluginBaseURL() {
            backendsEndpoint = URL(string: "/api/backends?web=1", relativeTo: base)?.absoluteURL
            logsEndpoint = URL(string: "/api/logs", relativeTo: base)?.absoluteURL
            controlEndpoint = URL(string: "/api/control", relativeTo: base)?.absoluteURL
            createEndpoint = URL(string: "/api/create", relativeTo: base)?.absoluteURL
            recipesEndpoint = URL(string: "/api/recipes", relativeTo: base)?.absoluteURL
            filePickerEndpoint = URL(string: "/api/file-picker", relativeTo: base)?.absoluteURL
            eventsEndpoint = URL(string: "/api/events", relativeTo: base)?.absoluteURL
            nativeAppProjectEndpoint = URL(string: "/api/native-app-projects", relativeTo: base)?.absoluteURL
            safeSpacesEndpoint = URL(string: "/api/safe-spaces", relativeTo: base)?.absoluteURL
        }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 40
        configuration.timeoutIntervalForResource = 45
        configuration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        outerframeHost.applyProxy(to: configuration)
        urlSession = URLSession(configuration: configuration)
        endpointIconDiskCache = outerframeHost.stagedFileDirectoryURL.map {
            EndpointIconDiskCache(directory: $0.appendingPathComponent("cache/OuterShellIcons", isDirectory: true))
        }
        let iconConfiguration = URLSessionConfiguration.ephemeral
        iconConfiguration.urlCache = nil
        iconConfiguration.timeoutIntervalForRequest = 20
        outerframeHost.applyProxy(to: iconConfiguration)
        iconSession = URLSession(configuration: iconConfiguration)

        let installConfiguration = URLSessionConfiguration.ephemeral
        installConfiguration.timeoutIntervalForRequest = 600
        installConfiguration.timeoutIntervalForResource = 600
        installConfiguration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        outerframeHost.applyProxy(to: installConfiguration)
        nativeAppInstallSession = URLSession(configuration: installConfiguration)

        let safeSpaceConfiguration = URLSessionConfiguration.ephemeral
        safeSpaceConfiguration.timeoutIntervalForRequest = 3600
        safeSpaceConfiguration.timeoutIntervalForResource = 3600
        safeSpaceConfiguration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        outerframeHost.applyProxy(to: safeSpaceConfiguration)
        safeSpaceOperationSession = URLSession(configuration: safeSpaceConfiguration)
    }

    private func recoverNetworkingIfNeeded(after error: Error) -> Bool {
        let nsError = error as NSError
        guard nsError.domain == NSURLErrorDomain else { return false }
        switch nsError.code {
        case NSURLErrorCannotConnectToHost,
             NSURLErrorNetworkConnectionLost,
             NSURLErrorNotConnectedToInternet,
             NSURLErrorCannotFindHost,
             NSURLErrorDNSLookupFailed,
             NSURLErrorTimedOut:
            eventWatchGeneration += 1
            eventWatchTask?.cancel()
            eventWatchTask = nil
            eventWatchRetryDelay = 1
            urlSession?.invalidateAndCancel()
            nativeAppInstallSession?.invalidateAndCancel()
            safeSpaceOperationSession?.invalidateAndCancel()
            configureNetworking()
            startEventWatch()
            return true
        default:
            return false
        }
    }

    private func modeFromURL(_ urlString: String?) -> BackendsViewMode {
        guard let urlString,
              let components = URLComponents(string: urlString) else {
            return .apps
        }

        let normalizedPath = components.path.trimmingCharacters(in: CharacterSet(charactersIn: "/")).lowercased()
        switch normalizedPath {
        case "backends":
            return .apps
        case "new":
            return .create
        case "", "apps":
            return .apps
        default:
            break
        }

        switch components.queryItems?.first(where: { $0.name == "view" })?.value {
        case "backends":
            return .apps
        case "new":
            return .create
        default:
            return .apps
        }
    }

    private func workspaceIDFromURL(_ urlString: String?) -> UUID? {
        guard let urlString,
              let components = URLComponents(string: urlString),
              let value = components.queryItems?.first(where: {
                  $0.name == "workspace"
              })?.value else {
            return nil
        }
        return UUID(uuidString: value)
    }

    private func recipeSafeSpaceIDFromURL(_ urlString: String?) -> UUID? {
        guard let urlString,
              let components = URLComponents(string: urlString) else {
            return nil
        }
        let pathComponents = components.path.split(separator: "/")
        guard pathComponents.count == 2,
              pathComponents[0].lowercased() == "safe-spaces" else {
            return nil
        }
        return UUID(uuidString: String(pathComponents[1]))
    }

    private func urlForMode(_ mode: BackendsViewMode) -> URL? {
        guard let url = outerframeHost.pluginURL(),
              var components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        var queryItems = components.queryItems ?? []
        queryItems.removeAll { $0.name == "view" }
        switch mode {
        case .apps:
            components.path = "/"
        case .create:
            components.path = "/new"
        }
        components.queryItems = queryItems.isEmpty ? nil : queryItems
        return components.url
    }

    private func urlForRecipeSafeSpace(_ workspaceID: UUID?) -> URL? {
        guard let url = outerframeHost.pluginURL(),
              var components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        var queryItems = components.queryItems ?? []
        queryItems.removeAll { $0.name == "view" }
        if let workspaceID {
            components.path = "/safe-spaces/\(workspaceID.uuidString.lowercased())"
        } else {
            components.path = "/"
        }
        components.queryItems = queryItems.isEmpty ? nil : queryItems
        return components.url
    }

    private func navigateToMode(_ nextMode: BackendsViewMode, pushHistory: Bool) {
        guard nextMode != mode else { return }
        if let url = urlForMode(nextMode) {
            if pushHistory {
                outerframeHost.pushHistoryEntry(url: url)
            } else {
                outerframeHost.replaceHistoryEntry(url: url)
            }
        }
        applyMode(nextMode)
    }

    private func navigateToRecipeSafeSpace(_ workspaceID: UUID?, pushHistory: Bool) {
        guard selectedRecipeSafeSpaceID != workspaceID || mode != .apps else { return }
        if let url = urlForRecipeSafeSpace(workspaceID) {
            if pushHistory {
                outerframeHost.pushHistoryEntry(url: url)
            } else {
                outerframeHost.replaceHistoryEntry(url: url)
            }
        }
        selectedRecipeSafeSpaceID = workspaceID
        safeSpaceRecipeMessage = ""
        appsScroll = 0
        if let workspaceID,
           let workspace = localWorkspaces.first(where: { $0.id == workspaceID }) {
            beginContainerConfigurationEditor(for: workspace)
        } else if workspaceID == nil {
            endContainerConfigurationEditor()
        }
        applyMode(.apps)
    }

    private func applyNavigation(_ urlString: String) {
        let nextMode = modeFromURL(urlString)
        selectedRecipeSafeSpaceID = nextMode == .apps
            ? recipeSafeSpaceIDFromURL(urlString)
            : nil
        safeSpaceRecipeMessage = ""
        appsScroll = 0
        if let selectedRecipeSafeSpaceID,
           let workspace = localWorkspaces.first(where: {
               $0.id == selectedRecipeSafeSpaceID
           }) {
            if pendingDockerfileWorkspace?.id != selectedRecipeSafeSpaceID {
                beginContainerConfigurationEditor(for: workspace)
            }
        } else {
            endContainerConfigurationEditor()
        }
        applyMode(nextMode)
    }

    private func returnFromRecipeSafeSpace() {
        if outerframeHost.canGoBackInHistory() {
            selectedRecipeSafeSpaceID = nil
            endContainerConfigurationEditor()
            outerframeHost.goBackInHistory()
        } else {
            navigateToRecipeSafeSpace(nil, pushHistory: false)
        }
    }

    private func applyMode(_ nextMode: BackendsViewMode) {
        if nextMode == .create && recipes.isEmpty { fetchRecipes() }
        let previousMode = mode
        if mode == .create && nextMode != .create {
            blurCreateField()
            discardGeneratedNativeProject()
        }
        if nextMode != .create {
            setCursorIfNeeded(.arrow)
        }
        mode = nextMode
        if mode == .create || mode == .apps {
            clampScrollOffsets()
        }
        let shouldAnimateCreateIn = previousMode != .create && nextMode == .create
        if shouldAnimateCreateIn {
            createLayer.removeAnimation(forKey: "create-overlay-fade-out")
            withoutImplicitAnimations {
                createLayer.opacity = 1
            }
        }
        updateColors()
        if shouldAnimateCreateIn {
            let animation = CABasicAnimation(keyPath: "opacity")
            animation.fromValue = 0
            animation.toValue = 1
            animation.duration = 0.14
            animation.timingFunction = CAMediaTimingFunction(name: .easeOut)
            createLayer.add(animation, forKey: "create-overlay-fade-in")
        }
    }

    private func returnToAppsFromCreate() {
        if outerframeHost.canGoBackInHistory() {
            outerframeHost.goBackInHistory()
        } else {
            navigateToMode(.apps, pushHistory: false)
        }
    }

    private func dismissCreateOverlay(removeGeneratedProjectStaging: Bool = true) {
        if removeGeneratedProjectStaging {
            discardGeneratedNativeProject()
        } else {
            clearGeneratedNativeProjectState()
        }
        let currentOpacity = createLayer.presentation()?.opacity ?? createLayer.opacity
        createLayer.removeAnimation(forKey: "create-overlay-fade-in")
        withoutImplicitAnimations {
            createLayer.opacity = 0
        }
        let animation = CABasicAnimation(keyPath: "opacity")
        animation.fromValue = currentOpacity
        animation.toValue = 0
        animation.duration = 0.12
        animation.timingFunction = CAMediaTimingFunction(name: .easeIn)
        createLayer.add(animation, forKey: "create-overlay-fade-out")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.13) { [weak self] in
            self?.returnToAppsFromCreate()
        }
    }

    private func discardGeneratedNativeProject() {
        let stagingRootURL = generatedNativeProject?.stagingRootURL
        clearGeneratedNativeProjectState()
        if let stagingRootURL {
            try? FileManager.default.removeItem(at: stagingRootURL)
        }
    }

    private func clearGeneratedNativeProjectState() {
        nativeFilePromiseURLs.removeAll()
        pendingNativeProjectDrag = nil
        nativeProjectSelectionState = .none
        nativeProjectDragFrame = .zero
        generatedNativeProject = nil
        generatedNativeProjectWasExported = false
    }

    private func startEventWatch(resetVersions: Bool = false) {
        guard eventWatchTask == nil, let eventsEndpoint, let urlSession else { return }
        if resetVersions {
            backendsEventVersion = 0
            logEventVersion = 0
            overviewEventVersion = 0
        }
        let generation = eventWatchGeneration
        var components = URLComponents(url: eventsEndpoint, resolvingAgainstBaseURL: false)
        var items = [
            URLQueryItem(name: "sinceBackends", value: String(backendsEventVersion)),
            URLQueryItem(name: "sinceLog", value: String(logEventVersion)),
            URLQueryItem(name: "sinceOverview", value: String(overviewEventVersion))
        ]
        if selectedContainerLogContext == nil, let selectedLog {
            items.append(URLQueryItem(name: "serviceID", value: selectedLog.serviceID))
            if let logFile = logFile(for: selectedLog), !logFile.path.isEmpty {
                items.append(URLQueryItem(name: "path", value: logFile.path))
            } else {
                items.append(URLQueryItem(name: "logIndex", value: String(selectedLog.logIndex)))
            }
        }
        components?.queryItems = items
        guard let url = components?.url else { return }
        eventWatchTask = urlSession.dataTask(with: url) { [weak self] data, response, error in
            Task { @MainActor in
                guard let self, self.eventWatchGeneration == generation else { return }
                self.eventWatchTask = nil
                let statusCode = (response as? HTTPURLResponse)?.statusCode ?? 0
                if error == nil,
                   statusCode < 400,
                   let data,
                   let event = try? EventResponse.decodeBinary(data) {
                    self.eventWatchRetryDelay = 1
                    self.backendsEventVersion = event.backendsVersion
                    self.logEventVersion = event.logVersion
                    if event.overviewChanged || !self.overviewLayoutReady {
                        self.overviewEventVersion = event.overviewVersion
                        self.loadOverviewLayout()
                        if !self.isRefreshingWorkspaces && self.workspaceContextID == nil {
                            self.sendWorkspaceRequest(operation: "list")
                        }
                    }
                    if event.backendsChanged {
                        self.loadOverviewLayout()
                        self.fetchBackends(quiet: true)
                    }
                    if event.logChanged, self.selectedLog != nil {
                        self.fetchSelectedLog(quiet: true)
                    }
                    self.startEventWatch()
                    return
                }
                self.scheduleEventWatchRetry()
            }
        }
        eventWatchTask?.resume()
    }

    private func scheduleEventWatchRetry() {
        let generation = eventWatchGeneration
        let delay = eventWatchRetryDelay
        eventWatchRetryDelay = min(eventWatchRetryDelay * 2, 30)
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            Task { @MainActor in
                guard let self,
                      self.eventWatchGeneration == generation,
                      self.eventWatchTask == nil else {
                    return
                }
                self.startEventWatch()
            }
        }
    }

    private func restartEventWatch(resetVersions: Bool = false) {
        eventWatchGeneration += 1
        eventWatchTask?.cancel()
        eventWatchTask = nil
        eventWatchRetryDelay = 1
        startEventWatch(resetVersions: resetVersions)
    }

    private func stopEventWatch() {
        eventWatchGeneration += 1
        eventWatchTask?.cancel()
        eventWatchTask = nil
        eventWatchRetryDelay = 1
    }

    private func configureLayersIfNeeded() {
        guard toolbarLayer.superlayer == nil else { return }
        rootLayer.masksToBounds = true
        rootLayer.addSublayer(toolbarLayer)
        rootLayer.addSublayer(contentLayer)
        rootLayer.addSublayer(iconTransitionLayer)
        rootLayer.addSublayer(overviewDragFeedbackLayer)
        rootLayer.addSublayer(createLayer)
        rootLayer.addSublayer(installOverlayLayer)
        rootLayer.addSublayer(updateOverlayLayer)
        rootLayer.addSublayer(aboutOverlayLayer)
        rootLayer.addSublayer(passwordOverlayLayer)
        rootLayer.addSublayer(workspaceOverlayLayer)
        workspaceOverlayLayer.addSublayer(workspacePanelLayer)
        rootLayer.addSublayer(copyConfirmationLayer)
        toolbarLayer.addSublayer(titleLayer)
        toolbarLayer.addSublayer(statusSelectionLayer)
        toolbarLayer.addSublayer(statusLayer)
        toolbarLayer.addSublayer(privilegedAppsHeaderLayer)
        toolbarLayer.addSublayer(safeSpacesHeaderLayer)
        toolbarLayer.addSublayer(outerShellActionLayer)
        contentLayer.addSublayer(appsLayer)
        appsLayer.addSublayer(appsScrollContentLayer)
        appsLayer.addSublayer(workspacePaneClipLayer)
        workspacePaneClipLayer.addSublayer(workspaceScrollContentLayer)
        workspacePaneClipLayer.masksToBounds = true
        appsLayer.addSublayer(appsOverlayLayer)
        contentLayer.addSublayer(dividerLayer)
        contentLayer.addSublayer(logHeaderLayer)
        contentLayer.addSublayer(logRowsClipLayer)
        logRowsClipLayer.addSublayer(logTextContentLayer)
        logTextContentLayer.addSublayer(logTextSelectionLayer)
        let scrollbarColors = ScrollbarColorConfiguration(appearance: appearance ?? NSAppearance.currentDrawing())
        let scrollbar = ScrollbarController<BackendsHandler>(appConnection: outerframeHost,
                                                             viewportLayer: logRowsClipLayer,
                                                             colorConfiguration: scrollbarColors,
                                                             scrollOffsetOrigin: .bottom)
        scrollbar.delegate = self
        logScrollbarController = scrollbar
        let filePickerScrollbar = ScrollbarController<FilePickerScrollbarDelegate>(appConnection: outerframeHost,
                                                                                  viewportLayer: filePickerListLayer,
                                                                                  colorConfiguration: scrollbarColors,
                                                                                  width: 10,
                                                                                  inset: 4,
                                                                                  scrollOffsetOrigin: .bottom)
        filePickerScrollbar.delegate = filePickerScrollbarDelegate
        filePickerScrollbarController = filePickerScrollbar
        createLayer.addSublayer(filePickerOverlayLayer)

        titleLayer.string = ""
        privilegedAppsHeaderLayer.string = "PRIVILEGED APPS"
        safeSpacesHeaderLayer.string = "SAFE SPACES"
        privilegedAppsHeaderLayer.isHidden = true
        safeSpacesHeaderLayer.isHidden = true
        outerShellActionLayer.isHidden = true
        installOverlayLayer.isHidden = true
        updateOverlayLayer.isHidden = true
        aboutOverlayLayer.isHidden = true
        passwordOverlayLayer.isHidden = true
        workspaceOverlayLayer.isHidden = true
        copyConfirmationLayer.isHidden = true
        filePickerOverlayLayer.isHidden = true
        appsLayer.masksToBounds = true
        logRowsClipLayer.masksToBounds = true
        statusSelectionLayer.masksToBounds = true

        for layer in [titleLayer, statusLayer,
                      privilegedAppsHeaderLayer, safeSpacesHeaderLayer] {
            layer.contentsScale = 2
            layer.truncationMode = .end
            layer.alignmentMode = .center
        }
        titleLayer.alignmentMode = .left
        statusLayer.alignmentMode = .right
        privilegedAppsHeaderLayer.alignmentMode = .left
        safeSpacesHeaderLayer.alignmentMode = .left
    }

    private func registerRootLayerIfNeeded() {
        guard !didRegisterLayer, let registerLayer = appConnection.registerLayer else { return }
        registerLayer(rootLayer)
        didRegisterLayer = true
        notifyAccessibilityLayoutChanged()
    }

    private func withEffectiveAppearance(_ body: () -> Void) {
        if let appearance {
            appearance.performAsCurrentDrawingAppearance(body)
        } else {
            body()
        }
    }

    private func resolvedCGColor(_ color: NSColor) -> CGColor {
        var resolved = CGColor(gray: 0, alpha: 1)
        withEffectiveAppearance {
            resolved = color.cgColor
        }
        return resolved
    }

    private func resolvedColor(_ color: NSColor) -> NSColor {
        NSColor(cgColor: resolvedCGColor(color)) ?? color
    }

    private func pageBackgroundColor() -> NSColor {
        .windowBackgroundColor
    }

    private func updateLayout(inCurrentTransaction: Bool = false) {
        withEffectiveAppearance {
            let update = { [self] in
                let width = max(currentSize.width, 1)
                let height = max(currentSize.height, 1)
                rootLayer.frame = CGRect(origin: .zero, size: CGSize(width: width, height: height))
                let visibleToolbarHeight: CGFloat = mode == .apps && backendError.isEmpty ? 0 : toolbarHeight
                toolbarLayer.frame = CGRect(x: 0, y: max(height - visibleToolbarHeight, 0), width: width, height: visibleToolbarHeight)
                toolbarLayer.isHidden = visibleToolbarHeight == 0
                contentLayer.frame = CGRect(x: 0, y: 0, width: width, height: max(height - visibleToolbarHeight, 0))
                iconTransitionLayer.frame = rootLayer.bounds
                overviewDragFeedbackLayer.frame = rootLayer.bounds
                overviewDragFeedbackLayer.isHidden = mode != .apps
                createLayer.frame = rootLayer.bounds
                workspaceOverlayLayer.frame = rootLayer.bounds
                copyConfirmationLayer.frame = rootLayer.bounds

                titleLayer.frame = .zero
                outerShellActionFrame = mode == .create && outerShellActionsBackend() != nil
                    ? CGRect(x: max(width - horizontalInset - 28, horizontalInset),
                             y: 10,
                             width: 28,
                             height: 28)
                    : .zero
                outerShellActionLayer.frame = outerShellActionFrame
                outerShellActionLayer.isHidden = outerShellActionFrame.isEmpty
                outerShellActionLayer.opacity = outerShellActionFrame.isEmpty ? 0 : 1
                statusLayer.frame = CGRect(x: horizontalInset, y: 14, width: max(width - horizontalInset * 2 - 36, 1), height: 18)
                statusSelectionLayer.frame = statusLayer.frame

                let contentHeight = contentLayer.bounds.height
                if mode == .apps || mode == .create {
                    outerframeHost.sendTextInputGeometryUpdate(nil)
                    appsLayer.isHidden = false
                    dividerLayer.isHidden = selectedServiceID == nil
                    logHeaderLayer.isHidden = selectedServiceID == nil
                    logRowsClipLayer.isHidden = selectedServiceID == nil
                    createLayer.isHidden = mode != .create
                    let appWidth = selectedServiceID == nil ? width : max(floor(width * 0.42), 320)
                    appsLayer.frame = CGRect(x: 0, y: 0, width: appWidth, height: contentHeight)
                    updateAppsScrollLayerFrames()
                    if selectedServiceID != nil {
                        dividerLayer.frame = CGRect(x: appWidth, y: 0, width: 1, height: contentHeight)
                        let logX = appWidth + 1
                        let logWidth = max(width - logX, 1)
                        logHeaderLayer.frame = CGRect(x: logX, y: max(contentHeight - logHeaderHeight, 0), width: logWidth, height: logHeaderHeight)
                        logRowsClipLayer.frame = CGRect(x: logX, y: 0, width: logWidth, height: max(contentHeight - logHeaderHeight, 0))
                        renderLogHeader()
                        renderLogRows()
                    }
                    renderAppsPage()
                    if mode == .create {
                        renderCreateForm()
                    }
                } else {
                    appsLayer.isHidden = true
                    dividerLayer.isHidden = true
                    logHeaderLayer.isHidden = true
                    logRowsClipLayer.isHidden = true
                    createLayer.isHidden = false
                    createLayer.frame = CGRect(x: 0, y: 0, width: width, height: contentHeight)
                    renderCreateForm()
                }
                if mode == .create {
                    renderFilePickerIfNeeded(width: createLayer.bounds.width,
                                             height: createLayer.bounds.height)
                }
                updateStatusText()
                renderInstallPromptIfNeeded(width: width, height: height)
                renderUpdatePromptIfNeeded(width: width, height: height)
                renderAboutPromptIfNeeded(width: width, height: height)
                renderPasswordPromptIfNeeded(width: width, height: height)
                renderWorkspacePanelIfNeeded(width: width, height: height)
                renderCopyConfirmationIfNeeded(width: width, height: height)
                notifyAccessibilityLayoutChanged()
            }
            if inCurrentTransaction {
                update()
            } else {
                withoutImplicitAnimations(update)
            }
        }
    }

    private func scheduleLayoutUpdate(needsScrollClamping: Bool = false) {
        scheduledLayoutNeedsScrollClamping = scheduledLayoutNeedsScrollClamping || needsScrollClamping
        guard !layoutUpdateScheduled else { return }
        layoutUpdateScheduled = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            let needsScrollClamping = scheduledLayoutNeedsScrollClamping
            layoutUpdateScheduled = false
            scheduledLayoutNeedsScrollClamping = false
            if needsScrollClamping {
                clampScrollOffsets()
            }
            updateLayout()
        }
    }

    private func scheduleResizeLayoutUpdate() {
        scheduleLayoutUpdate(needsScrollClamping: true)
    }

    private func scheduleCreateLayoutUpdate() {
        scheduleLayoutUpdate(needsScrollClamping: true)
    }

    private func updateWindowActiveAppearance() {
        let createContentNeedsLayout = mode == .create &&
            (createInputController.isFocused || normalizedCreateMessageSelectionRange() != nil)
        let passwordPromptNeedsLayout = pendingPasswordAction != nil && passwordInputController.isFocused
        if createContentNeedsLayout || passwordPromptNeedsLayout {
            scheduleLayoutUpdate()
            return
        }

        if logHeaderDetailSelectionRange != nil {
            withoutImplicitAnimations {
                renderLogHeader()
            }
        }
        if statusSelectionRange != nil {
            renderStatusSelection()
        }
        if logTextSelectionRange != nil {
            updateLogTextSelectionLayers(force: true)
        }
        if aboutSelectionRange != nil {
            updateAboutSelectionLayers()
        }
    }

    private func updateColors() {
        runningBadgeSymbolImages.removeAll(keepingCapacity: true)
        withEffectiveAppearance {
            withoutImplicitAnimations {
                let pageBackground = pageBackgroundColor()
                rootLayer.backgroundColor = resolvedCGColor(pageBackground)
                toolbarLayer.backgroundColor = resolvedCGColor(.controlBackgroundColor)
                contentLayer.backgroundColor = resolvedCGColor(pageBackground)
                logHeaderLayer.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.9))
                dividerLayer.backgroundColor = resolvedCGColor(.separatorColor)

                titleLayer.foregroundColor = resolvedCGColor(.labelColor)
                statusLayer.foregroundColor = resolvedCGColor(.secondaryLabelColor)
                renderStatusSelection()
                privilegedAppsHeaderLayer.foregroundColor = resolvedCGColor(.secondaryLabelColor)
                safeSpacesHeaderLayer.foregroundColor = resolvedCGColor(.secondaryLabelColor)
                outerShellActionLayer.applyStyle(tintCGColor: resolvedCGColor(.secondaryLabelColor),
                                                 backgroundCGColor: resolvedCGColor(.clear))
                updateLogTextContentIfNeeded(text: currentLogText(), force: true)
                updateLogTextViewport()
                updateLogTextSelectionLayers(force: true)
                let scrollbarColors = ScrollbarColorConfiguration(appearance: appearance ?? NSAppearance.currentDrawing())
                logScrollbarController?.updateColorConfiguration(scrollbarColors)
                filePickerScrollbarController?.updateColorConfiguration(scrollbarColors)
                updateFilePickerVisibleRows(rebuild: true)
                renderWorkspacePanelIfNeeded(width: rootLayer.bounds.width,
                                             height: rootLayer.bounds.height)

                titleLayer.font = NSFont.systemFont(ofSize: 15, weight: .semibold)
                titleLayer.fontSize = 15
                for layer in [privilegedAppsHeaderLayer, safeSpacesHeaderLayer] {
                    layer.font = NSFont.systemFont(ofSize: 13, weight: .semibold)
                    layer.fontSize = 13
                }
                for layer in [statusLayer] {
                    layer.font = NSFont.systemFont(ofSize: 12, weight: .medium)
                    layer.fontSize = 12
                }
                updateLayout(inCurrentTransaction: true)
            }
        }
    }

    private func updateAppsScrollLayerFrames() {
        appsScrollContentLayer.frame = CGRect(x: 0,
                                              y: appsScroll,
                                              width: appsLayer.bounds.width,
                                              height: appsLayer.bounds.height)
        workspacePaneClipLayer.isHidden = !usesWorkspaceSplitLayout
        workspacePaneClipLayer.frame = workspacePaneFrame
        workspaceScrollContentLayer.frame = CGRect(
            x: -workspacePaneFrame.minX,
            y: workspaceScroll,
            width: appsLayer.bounds.width,
            height: appsLayer.bounds.height
        )
        appsOverlayLayer.frame = appsLayer.bounds
    }

    private var activeAppsContentLayer: CALayer {
        if let appsRenderTargetOverride {
            return appsRenderTargetOverride
        }
        return isRenderingWorkspacePane ? workspaceScrollContentLayer : appsScrollContentLayer
    }

    private func addAppsSublayer(_ layer: CALayer) {
        activeAppsContentLayer.addSublayer(layer)
    }

    private func appsContentLayer(for scope: AppLauncherScope) -> CALayer {
        if usesWorkspaceSplitLayout, case .container = scope {
            return workspaceScrollContentLayer
        }
        return appsScrollContentLayer
    }

    private var workspaceOverviewContentLayer: CALayer {
        usesWorkspaceSplitLayout ? workspaceScrollContentLayer : appsScrollContentLayer
    }

    private func appsContentPoint(for contentPoint: CGPoint) -> CGPoint {
        let pointInAppsLayer = appsLayer.convert(contentPoint, from: contentLayer)
        if usesWorkspaceSplitLayout, workspacePaneFrame.contains(pointInAppsLayer) {
            return workspaceScrollContentLayer.convert(contentPoint, from: contentLayer)
        }
        return appsScrollContentLayer.convert(contentPoint, from: contentLayer)
    }

    private func appsTextContentSpace(for contentPoint: CGPoint) -> AppsTextContentSpace {
        let pointInAppsLayer = appsLayer.convert(contentPoint, from: contentLayer)
        return usesWorkspaceSplitLayout && workspacePaneFrame.contains(pointInAppsLayer)
            ? .workspace
            : .apps
    }

    private func updateMatchedLayerVisibility() {
        let appsClipFrame = rootLayer.convert(appsLayer.bounds, from: appsLayer).insetBy(dx: -1, dy: -1)
        let workspaceClipFrame = rootLayer.convert(
            workspacePaneClipLayer.bounds,
            from: workspacePaneClipLayer
        ).insetBy(dx: -1, dy: -1)
        for (key, state) in iconMatchStates {
            let clipFrame = usesWorkspaceSplitLayout && workspaceVisibleIconKeys.contains(key)
                ? workspaceClipFrame
                : appsClipFrame
            iconMatchLayers[key]?.isHidden = !state.frame.intersects(clipFrame)
        }
        for (key, state) in textMatchStates {
            let clipFrame = usesWorkspaceSplitLayout && workspaceVisibleTextKeys.contains(key)
                ? workspaceClipFrame
                : appsClipFrame
            textMatchLayers[key]?.isHidden = !state.frame.intersects(clipFrame)
        }
    }

    private func offsetMatchedLayers(deltaY: CGFloat, workspaceOnly: Bool? = nil) {
        guard abs(deltaY) > 0.001 else {
            updateMatchedLayerVisibility()
            return
        }
        for (key, state) in iconMatchStates {
            if let workspaceOnly,
               workspaceVisibleIconKeys.contains(key) != workspaceOnly {
                continue
            }
            let frame = state.frame.offsetBy(dx: 0, dy: deltaY)
            iconMatchStates[key] = IconMatchState(frame: frame,
                                                  image: state.image,
                                                  symbolName: state.symbolName,
                                                  title: state.title)
            iconMatchLayers[key]?.frame = frame
        }
        for (key, state) in textMatchStates {
            if let workspaceOnly,
               workspaceVisibleTextKeys.contains(key) != workspaceOnly {
                continue
            }
            let frame = state.frame.offsetBy(dx: 0, dy: deltaY)
            textMatchStates[key] = TextMatchState(frame: frame,
                                                  title: state.title,
                                                  fontSize: state.fontSize,
                                                  weight: state.weight,
                                                  alignment: state.alignment,
                                                  isWrapped: state.isWrapped)
            textMatchLayers[key]?.frame = frame
        }
        updateMatchedLayerVisibility()
    }

    private func scrollCurrentModeWithoutRerender(deltaY: CGFloat) {
        withoutImplicitAnimations {
            switch mode {
            case .apps:
                updateAppsScrollLayerFrames()
                offsetMatchedLayers(
                    deltaY: deltaY,
                    workspaceOnly: usesWorkspaceSplitLayout ? false : nil
                )
            case .create:
                offsetCreateFormWithoutRerender(deltaY: deltaY)
            }
        }
    }

    private func scrollWorkspaceWithoutRerender(deltaY: CGFloat) {
        withoutImplicitAnimations {
            updateAppsScrollLayerFrames()
            offsetMatchedLayers(deltaY: deltaY, workspaceOnly: true)
        }
    }

    private func offsetCreateFormWithoutRerender(deltaY: CGFloat) {
        guard abs(deltaY) > 0.001 else { return }

        createFormContentLayer.frame = createFormContentLayer.frame.offsetBy(dx: 0, dy: deltaY)

        createSectionFrames = createSectionFrames.map { ($0.frame.offsetBy(dx: 0, dy: deltaY), $0.section) }
        recipeFrames = recipeFrames.map { ($0.frame.offsetBy(dx: 0, dy: deltaY), $0.recipeID) }
        bundledAppInstallFrames = bundledAppInstallFrames.map { ($0.frame.offsetBy(dx: 0, dy: deltaY), $0.backend) }
        createFieldFrames = createFieldFrames.map { ($0.frame.offsetBy(dx: 0, dy: deltaY), $0.key) }
        createFieldLayouts = Dictionary(uniqueKeysWithValues: createFieldLayouts.map { key, layout in
            (key, CreateFieldLayout(fieldFrame: layout.fieldFrame.offsetBy(dx: 0, dy: deltaY),
                                    textFrame: layout.textFrame.offsetBy(dx: 0, dy: deltaY),
                                    key: layout.key,
                                    monospaced: layout.monospaced,
                                    multiline: layout.multiline))
        })
        createChoiceFrames = createChoiceFrames.map { ($0.frame.offsetBy(dx: 0, dy: deltaY), $0.key, $0.value) }
        createSuggestionFrames = createSuggestionFrames.map { ($0.frame.offsetBy(dx: 0, dy: deltaY), $0.key, $0.value) }
        createDirectorySelectFrames = createDirectorySelectFrames.map { ($0.frame.offsetBy(dx: 0, dy: deltaY), $0.key) }
        createButtonFrame = createButtonFrame.offsetBy(dx: 0, dy: deltaY)
        cancelCreateFrame = cancelCreateFrame.offsetBy(dx: 0, dy: deltaY)
        bashIconSelectFrame = bashIconSelectFrame.offsetBy(dx: 0, dy: deltaY)
        nativeProjectDragFrame = nativeProjectDragFrame.offsetBy(dx: 0, dy: deltaY)
        createMessageFrame = createMessageFrame.offsetBy(dx: 0, dy: deltaY)
        createContentBottom += deltaY
        sendCreateFieldTextInputGeometryUpdate()
    }

    private func renderAppsPage() {
        privilegedAppsHeaderLayer.isHidden = true
        safeSpacesHeaderLayer.isHidden = true
        for layer in appsLayer.sublayers ?? []
        where layer !== appsScrollContentLayer &&
            layer !== workspacePaneClipLayer &&
            layer !== appsOverlayLayer {
            layer.removeFromSuperlayer()
        }
        if appsScrollContentLayer.superlayer == nil {
            appsLayer.addSublayer(appsScrollContentLayer)
        }
        if workspacePaneClipLayer.superlayer == nil {
            appsLayer.addSublayer(workspacePaneClipLayer)
        }
        if workspaceScrollContentLayer.superlayer == nil {
            workspacePaneClipLayer.addSublayer(workspaceScrollContentLayer)
        }
        if appsOverlayLayer.superlayer == nil {
            appsLayer.addSublayer(appsOverlayLayer)
        }
        appsScrollContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        workspaceScrollContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        appsOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        appCardFrames.removeAll()
        overviewMenuFrames.removeAll()
        overviewAddFrames.removeAll()
        overviewDropFrames.removeAll()
        overviewGroupFrames.removeAll()
        appBadgeFrames.removeAll()
        appListDropFrames.removeAll()
        appUnlistedDropFrames.removeAll()
        appOverflowFrames.removeAll()
        workspaceOverviewRowFrames.removeAll()
        workspaceOverviewActionFrames.removeAll()
        workspaceOverviewAppFrames.removeAll()
        workspaceOverviewCreateFrame = .zero
        safeSpaceDetailBackFrame = .zero
        safeSpaceDetailAddAppFrames.removeAll()
        safeSpaceDetailAddStepFrame = .zero
        safeSpaceDetailAddUserFrame = .zero
        safeSpaceDetailRebuildFrame = .zero
        safeSpaceDetailCopyContainerfileFrame = .zero
        safeSpaceDetailEditDockerfileFrame = .zero
        safeSpaceDetailOpenDockerfileFrame = .zero
        safeSpaceDetailCopySupportSnippetFrame = .zero
        safeSpaceDetailCopyRecipeMessageFrame = .zero
        safeSpaceDetailEditBaseImageFrame = .zero
        safeSpaceDetailStepFrames.removeAll()
        safeSpaceDetailEditStepFrames.removeAll()
        safeSpaceDetailAddScriptFrames.removeAll()
        safeSpaceDetailEditScriptFrames.removeAll()
        safeSpaceDetailRenameScriptFrames.removeAll()
        safeSpaceDetailCatalogFrames.removeAll()
        safeSpaceDockerfileTextBlocks.removeAll()
        addAppFrame = .zero
        iconMatchStates.removeAll()
        textMatchStates.removeAll()
        workspaceVisibleIconKeys.removeAll()
        workspaceVisibleTextKeys.removeAll()
        var visibleIconKeys = Set<String>()
        var visibleTextKeys = Set<String>()

        usesWorkspaceSplitLayout = false
        workspacePaneFrame = .zero
        workspaceScroll = 0
        updateAppsScrollLayerFrames()
        appsContentBottom = renderOverviewCards(
            visibleIconKeys: &visibleIconKeys,
            visibleTextKeys: &visibleTextKeys
        )
        workspaceContentBottom = 0
        renderOverviewDragFrame()
        hideUnrenderedMatchedLayers(visibleIconKeys: visibleIconKeys, visibleTextKeys: visibleTextKeys)
        updateMatchedLayerVisibility()
        let didClampAppsScroll = clampAppsScrollUsingRenderedContent()
        let didClampWorkspaceScroll = clampWorkspaceScrollUsingRenderedContent()
        if didClampAppsScroll || didClampWorkspaceScroll {
            updateAppsScrollLayerFrames()
            renderAppsPage()
        }
    }

    private func renderSafeSpaceDockerfileFragments(
        _ recipe: LocalWorkspaceRecord.Recipe,
        area: CGRect,
        top: CGFloat
    ) -> CGFloat {
        var cursor = top
        let footerFragments = recipe.fragments.filter { $0.id == "footer" }
        let users = recipe.users.filter { !$0.isRoot }
        let userFragmentIDs = Set(users.map { "user-\($0.id)" })
        let userSetupPrefixes = users.map { "\($0.name) setup · " }
        let rootFragments = recipe.fragments.filter { fragment in
            fragment.id != "footer" &&
                !fragment.id.hasPrefix("recipe-file-") &&
                !userFragmentIDs.contains(fragment.id) &&
                !userSetupPrefixes.contains(where: {
                    fragment.displayName.hasPrefix($0)
                })
        }

        cursor = renderDockerfileSectionHeading(
            title: "ROOT",
            detail: "/root · administrative user",
            area: area,
            top: cursor
        )
        cursor = renderDockerfileFragmentBlocks(rootFragments,
                                                recipe: recipe,
                                                area: area,
                                                top: cursor)
        cursor = renderRecipeScriptSection(recipe,
                                           userID: nil,
                                           area: area,
                                           top: cursor)
        cursor = renderInlineDockerfileAddApp(area: area,
                                              top: cursor)
        cursor = renderInlineDockerfileAddFragment(area: area, top: cursor)

        for user in users {
            cursor -= 14
            cursor = renderDockerfileSectionHeading(
                title: user.name.uppercased(),
                detail: "\(user.homeDirectory) · \(user.workingDirectory)",
                area: area,
                top: cursor
            )
            let userFragments = recipe.fragments.filter {
                $0.id == "user-\(user.id)"
            }
            cursor = renderDockerfileFragmentBlocks(userFragments,
                                                    recipe: recipe,
                                                    area: area,
                                                    top: cursor)
            cursor = renderRecipeScriptSection(recipe,
                                               userID: UUID(uuidString: user.id),
                                               area: area,
                                               top: cursor)
            cursor = renderInlineDockerfileAddApp(area: area,
                                                  top: cursor)
        }

        cursor = renderInlineDockerfileAddUser(area: area, top: cursor)

        if !footerFragments.isEmpty {
            cursor -= 14
            cursor = renderDockerfileSectionHeading(
                title: "CONTAINER STARTUP",
                detail: "Default process and execution user",
                area: area,
                top: cursor
            )
            cursor = renderDockerfileFragmentBlocks(footerFragments,
                                                    recipe: recipe,
                                                    area: area,
                                                    top: cursor)
        }
        return cursor - 8
    }

    private func renderRecipeScriptSection(
        _ recipe: LocalWorkspaceRecord.Recipe,
        userID: UUID?,
        area: CGRect,
        top: CGFloat
    ) -> CGFloat {
        let scripts = recipe.scriptFiles.filter { script in
            if let userID {
                return script.userID == userID.uuidString
            }
            return script.userID.isEmpty
        }.sorted { $0.fileName.localizedStandardCompare($1.fileName) == .orderedAscending }
        let scriptIDs = Set(scripts.map { "recipe-file-\($0.relativePath)" })
        let fragments = recipe.fragments.filter { scriptIDs.contains($0.id) }

        var cursor = top - 2
        let heading = makeTextLayer(size: 9,
                                    weight: .semibold,
                                    color: .secondaryLabelColor)
        heading.string = "SETUP SCRIPTS"
        heading.frame = CGRect(x: area.minX,
                               y: cursor - 17,
                               width: 110,
                               height: 13)
        addAppsSublayer(heading)

        let location = makeTextLayer(size: 9,
                                     weight: .regular,
                                     color: .tertiaryLabelColor,
                                     alignment: .right)
        let directory = userID.flatMap { identifier in
            recipe.users.first(where: { $0.id == identifier.uuidString })
        }.map { "steps/users/\($0.name)" } ?? "steps/root"
        location.string = "\(directory) · runs alphabetically"
        location.truncationMode = .middle
        location.frame = CGRect(x: area.minX + 112,
                                y: cursor - 17,
                                width: max(area.width - 112, 1),
                                height: 13)
        addAppsSublayer(location)
        cursor -= 28

        if scripts.isEmpty {
            let empty = makeTextLayer(size: 10,
                                      weight: .regular,
                                      color: .tertiaryLabelColor)
            empty.string = "No setup scripts. These mounted files can be changed inside the container or with a host text editor."
            empty.isWrapped = true
            empty.frame = CGRect(x: area.minX,
                                 y: cursor - 32,
                                 width: area.width,
                                 height: 30)
            addAppsSublayer(empty)
            cursor -= 38
        } else {
            cursor = renderDockerfileFragmentBlocks(fragments,
                                                    recipe: recipe,
                                                    area: area,
                                                    top: cursor)
        }

        let addFrame = CGRect(x: area.minX,
                              y: cursor - 36,
                              width: area.width,
                              height: 32)
        safeSpaceDetailAddScriptFrames.append((addFrame, userID))
        renderInlineDockerfileAction(frame: addFrame,
                                     symbolName: "doc.badge.plus",
                                     title: "Add setup script…")
        return cursor - 44
    }

    private func renderDockerfileSectionHeading(
        title: String,
        detail: String,
        area: CGRect,
        top: CGFloat
    ) -> CGFloat {
        let divider = CALayer()
        divider.frame = CGRect(x: area.minX,
                               y: top - 2,
                               width: area.width,
                               height: 0.5)
        divider.backgroundColor = resolvedCGColor(.separatorColor)
        addAppsSublayer(divider)

        let titleLayer = makeTextLayer(size: 11,
                                       weight: .semibold,
                                       color: .secondaryLabelColor)
        titleLayer.string = title
        titleLayer.frame = CGRect(x: area.minX,
                                  y: top - 28,
                                  width: max(area.width * 0.34, 100),
                                  height: 16)
        addAppsSublayer(titleLayer)

        let detailLayer = makeTextLayer(size: 10,
                                        weight: .regular,
                                        color: .tertiaryLabelColor,
                                        alignment: .right)
        detailLayer.string = detail
        detailLayer.truncationMode = .middle
        detailLayer.frame = CGRect(x: area.minX + area.width * 0.34,
                                   y: top - 28,
                                   width: area.width * 0.66,
                                   height: 16)
        addAppsSublayer(detailLayer)
        return top - 40
    }

    private func renderDockerfileFragmentBlocks(
        _ fragments: [LocalWorkspaceRecord.Recipe.Fragment],
        recipe: LocalWorkspaceRecord.Recipe,
        area: CGRect,
        top: CGFloat
    ) -> CGFloat {
        var cursor = top
        for fragment in fragments {
            let script = recipe.scriptFiles.first {
                fragment.id == "recipe-file-\($0.relativePath)"
            }
            let textWidth = max(area.width - 18, 1)
            let wrappedLines = dockerfileWrappedLines(fragment.contents, width: textWidth)
            let codeHeight = max(CGFloat(wrappedLines.count) * dockerfileLineHeight() + 16, 46)
            let headerHeight: CGFloat = script == nil ? 34 : 62
            let blockHeight = codeHeight + headerHeight
            let blockFrame = CGRect(x: area.minX,
                                    y: cursor - blockHeight,
                                    width: area.width,
                                    height: blockHeight)

            let title = makeTextLayer(size: 10,
                                      weight: .medium,
                                      color: .secondaryLabelColor)
            title.string = script?.fileName ?? fragment.displayName
            title.truncationMode = .end
            title.frame = CGRect(x: blockFrame.minX,
                                 y: blockFrame.maxY - 18,
                                 width: max(blockFrame.width - (script == nil ? 72 : 112), 1),
                                 height: 14)
            addAppsSublayer(title)

            if let script {
                let inside = makeTextLayer(size: 8.5,
                                           weight: .regular,
                                           color: .tertiaryLabelColor)
                inside.string = "Inside  \(script.guestPath)"
                inside.truncationMode = .middle
                inside.frame = CGRect(x: blockFrame.minX,
                                      y: blockFrame.maxY - 34,
                                      width: max(blockFrame.width - 112, 1),
                                      height: 12)
                addAppsSublayer(inside)

                let outside = makeTextLayer(size: 8.5,
                                            weight: .regular,
                                            color: .tertiaryLabelColor)
                outside.string = "Outside  \(script.hostPath)"
                outside.truncationMode = .middle
                outside.frame = CGRect(x: blockFrame.minX,
                                       y: blockFrame.maxY - 47,
                                       width: max(blockFrame.width - 112, 1),
                                       height: 12)
                addAppsSublayer(outside)
            }

            var actionX = blockFrame.maxX - 25
            if let script {
                let renameFrame = CGRect(x: actionX,
                                         y: blockFrame.maxY - 24,
                                         width: 22,
                                         height: 22)
                let rename = makeSymbolButtonLayer(
                    symbolName: "character.cursor.ibeam",
                    accessibilityTitle: "Rename \(script.fileName)"
                )
                rename.frame = renameFrame
                addAppsSublayer(rename)
                safeSpaceDetailRenameScriptFrames.append((renameFrame, script))
                actionX -= 27

                let editFrame = CGRect(x: actionX,
                                       y: blockFrame.maxY - 24,
                                       width: 22,
                                       height: 22)
                let edit = makeSymbolButtonLayer(
                    symbolName: "square.and.pencil",
                    accessibilityTitle: "Edit \(script.fileName) in Plaintext"
                )
                edit.frame = editFrame
                addAppsSublayer(edit)
                safeSpaceDetailEditScriptFrames.append((editFrame, script))
            } else if fragment.isRemovable,
               let stepID = UUID(uuidString: fragment.stepID),
               let step = recipe.steps.first(where: { $0.id == stepID }) {
                let removeFrame = CGRect(x: actionX,
                                         y: blockFrame.maxY - 24,
                                         width: 22,
                                         height: 22)
                let remove = makeSymbolButtonLayer(
                    symbolName: "minus.circle",
                    accessibilityTitle: "Remove \(fragment.displayName)"
                )
                remove.frame = removeFrame
                addAppsSublayer(remove)
                safeSpaceDetailStepFrames.append((removeFrame, step))
                actionX -= 27
                if fragment.isEditable {
                    let editFrame = CGRect(x: actionX,
                                           y: blockFrame.maxY - 24,
                                           width: 22,
                                           height: 22)
                    let edit = makeSymbolButtonLayer(
                        symbolName: "pencil",
                        accessibilityTitle: "Edit \(fragment.displayName)"
                    )
                    edit.frame = editFrame
                    addAppsSublayer(edit)
                    safeSpaceDetailEditStepFrames.append((editFrame, step))
                }
            } else if fragment.id == "header", fragment.isEditable {
                let editFrame = CGRect(x: actionX,
                                       y: blockFrame.maxY - 24,
                                       width: 22,
                                       height: 22)
                let edit = makeSymbolButtonLayer(
                    symbolName: "pencil",
                    accessibilityTitle: "Change base image"
                )
                edit.frame = editFrame
                addAppsSublayer(edit)
                safeSpaceDetailEditBaseImageFrame = editFrame
            } else if fragment.id != "dockerfile" {
                let lock = CALayer()
                lock.frame = CGRect(x: actionX,
                                    y: blockFrame.maxY - 22,
                                    width: 16,
                                    height: 16)
                lock.contentsGravity = .resizeAspect
                lock.contents = symbolCGImage(named: "lock.fill", pointSize: 10)
                lock.opacity = 0.45
                addAppsSublayer(lock)
            }

            let codeFrame = CGRect(x: blockFrame.minX,
                                   y: blockFrame.minY,
                                   width: blockFrame.width,
                                   height: codeHeight)
            let background = CALayer()
            background.frame = codeFrame
            background.cornerRadius = 6
            background.backgroundColor = resolvedCGColor(
                NSColor.textBackgroundColor.withAlphaComponent(0.62)
            )
            background.borderWidth = fragment.isEditable ? 1 : 0.5
            background.borderColor = resolvedCGColor(
                fragment.isEditable ? .controlAccentColor : .separatorColor
            )
            addAppsSublayer(background)

            let selectionLayer = CALayer()
            selectionLayer.frame = background.bounds
            background.addSublayer(selectionLayer)

            let lineHeight = dockerfileLineHeight()
            var renderedLines: [DockerfileTextLine] = []
            renderedLines.reserveCapacity(wrappedLines.count)
            for (index, wrappedLine) in wrappedLines.enumerated() {
                let localFrame = CGRect(
                    x: 9,
                    y: codeHeight - 8 - CGFloat(index + 1) * lineHeight,
                    width: textWidth,
                    height: lineHeight
                )
                let code = makeTextLayer(size: 10,
                                         weight: .regular,
                                         color: .labelColor,
                                         monospaced: true)
                code.string = wrappedLine.text
                code.frame = localFrame
                background.addSublayer(code)
                renderedLines.append(
                    DockerfileTextLine(
                        text: wrappedLine.text,
                        range: wrappedLine.range,
                        frame: localFrame.offsetBy(dx: codeFrame.minX, dy: codeFrame.minY)
                    )
                )
            }
            let textBlock = DockerfileTextBlock(
                fragmentID: fragment.id,
                text: fragment.contents,
                frame: codeFrame,
                lines: renderedLines,
                font: dockerfileFont(),
                contentSpace: .apps,
                selectionLayer: selectionLayer
            )
            safeSpaceDockerfileTextBlocks.append(textBlock)
            renderDockerfileSelection(in: textBlock)
            cursor -= blockHeight + 10
        }
        return cursor
    }

    private func renderInlineDockerfileAddFragment(
        area: CGRect,
        top: CGFloat
    ) -> CGFloat {
        safeSpaceDetailAddStepFrame = CGRect(x: area.minX,
                                             y: top - 36,
                                             width: area.width,
                                             height: 32)
        renderInlineDockerfileAction(
            frame: safeSpaceDetailAddStepFrame,
            symbolName: "plus.square",
            title: "Add root Dockerfile fragment…"
        )
        return top - 44
    }

    private func renderInlineDockerfileAddApp(
        area: CGRect,
        top: CGFloat
    ) -> CGFloat {
        let frame = CGRect(x: area.minX,
                           y: top - 36,
                           width: area.width,
                           height: 32)
        safeSpaceDetailAddAppFrames.append(frame)
        renderInlineDockerfileAction(
            frame: frame,
            symbolName: "app.badge.plus",
            title: "Add app…"
        )
        return top - 44
    }

    private func renderInlineDockerfileAddUser(
        area: CGRect,
        top: CGFloat
    ) -> CGFloat {
        safeSpaceDetailAddUserFrame = CGRect(x: area.minX,
                                             y: top - 36,
                                             width: area.width,
                                             height: 32)
        renderInlineDockerfileAction(
            frame: safeSpaceDetailAddUserFrame,
            symbolName: "person.crop.circle.badge.plus",
            title: "Add user…"
        )
        return top - 44
    }

    private func renderInlineDockerfileAction(
        frame: CGRect,
        symbolName: String,
        title: String
    ) {
        let background = CALayer()
        background.frame = frame
        background.cornerRadius = 6
        background.borderWidth = 0.5
        background.borderColor = resolvedCGColor(.separatorColor)
        background.backgroundColor = resolvedCGColor(
            NSColor.controlBackgroundColor.withAlphaComponent(0.32)
        )
        addAppsSublayer(background)

        let icon = CALayer()
        icon.frame = CGRect(x: frame.minX + 10,
                            y: frame.minY + 7,
                            width: 18,
                            height: 18)
        icon.contentsGravity = .resizeAspect
        icon.contents = symbolCGImage(named: symbolName, pointSize: 13)
        addAppsSublayer(icon)

        let titleLayer = makeTextLayer(size: 11,
                                       weight: .medium,
                                       color: .controlAccentColor)
        titleLayer.string = title
        titleLayer.frame = CGRect(x: frame.minX + 37,
                                  y: frame.minY + 7,
                                  width: max(frame.width - 47, 1),
                                  height: 18)
        addAppsSublayer(titleLayer)
    }

    private func dockerfileFont() -> NSFont {
        NSFont.monospacedSystemFont(ofSize: 10, weight: .regular)
    }

    private func dockerfileLineHeight() -> CGFloat {
        14
    }

    private func renderSelectableMountedFolderPath(
        _ path: String,
        identifier: String,
        font: NSFont,
        color: NSColor,
        localFrame: CGRect,
        contentFrame: CGRect,
        contentSpace: AppsTextContentSpace,
        in parentLayer: CALayer
    ) {
        let selectionLayer = CALayer()
        selectionLayer.frame = localFrame
        selectionLayer.masksToBounds = true
        parentLayer.addSublayer(selectionLayer)

        let text = makeTextLayer(size: font.pointSize,
                                 weight: .regular,
                                 color: color)
        text.string = NSAttributedString(
            string: path,
            attributes: [
                .font: font,
                .foregroundColor: color
            ]
        )
        text.truncationMode = .middle
        text.frame = localFrame
        parentLayer.addSublayer(text)

        let line = DockerfileTextLine(
            text: path,
            range: NSRange(location: 0, length: (path as NSString).length),
            frame: contentFrame
        )
        let block = DockerfileTextBlock(
            fragmentID: identifier,
            text: path,
            frame: contentFrame,
            lines: [line],
            font: font,
            contentSpace: contentSpace,
            selectionLayer: selectionLayer
        )
        safeSpaceDockerfileTextBlocks.append(block)
        renderDockerfileSelection(in: block)
    }

    private func dockerfileWrappedLines(
        _ text: String,
        width: CGFloat
    ) -> [(text: String, range: NSRange)] {
        let source = text as NSString
        let length = source.length
        guard length > 0 else {
            return [("", NSRange(location: 0, length: 0))]
        }

        let attributed = NSAttributedString(string: text, attributes: [.font: dockerfileFont()])
        let typesetter = CTTypesetterCreateWithAttributedString(attributed)
        var lines: [(text: String, range: NSRange)] = []
        var location = 0
        while location < length {
            let remainingRange = NSRange(location: location, length: length - location)
            let newlineRange = source.range(of: "\n", options: [], range: remainingRange)
            let lineEnd = newlineRange.location == NSNotFound ? length : newlineRange.location
            if location == lineEnd {
                lines.append(("", NSRange(location: location, length: 0)))
            } else {
                while location < lineEnd {
                    let suggestedLength = CTTypesetterSuggestLineBreak(
                        typesetter,
                        location,
                        Double(max(width, 1))
                    )
                    let lineLength = min(max(suggestedLength, 1), lineEnd - location)
                    let range = NSRange(location: location, length: lineLength)
                    lines.append((source.substring(with: range), range))
                    location += lineLength
                }
            }
            guard newlineRange.location != NSNotFound else { break }
            location = lineEnd + 1
            if location == length {
                lines.append(("", NSRange(location: location, length: 0)))
            }
        }
        return lines
    }

    private func normalizedDockerfileSelectionRange(
        _ range: NSRange?,
        in block: DockerfileTextBlock
    ) -> NSRange? {
        guard let range else { return nil }
        let length = (block.text as NSString).length
        let lower = max(min(range.location, length), 0)
        let upper = max(min(range.location + range.length, length), lower)
        guard upper > lower else { return nil }
        return NSRange(location: lower, length: upper - lower)
    }

    private func renderDockerfileSelection(in block: DockerfileTextBlock) {
        block.selectionLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        guard selectedDockerfileFragmentID == block.fragmentID,
              let selection = normalizedDockerfileSelectionRange(
                  dockerfileSelectionRange,
                  in: block
              ) else {
            return
        }
        let selectionEnd = selection.location + selection.length
        for line in block.lines {
            let lineEnd = line.range.location + line.range.length
            let lower = max(selection.location, line.range.location)
            let upper = min(selectionEnd, lineEnd)
            guard upper > lower else { continue }
            let coreTextLine = CTLineCreateWithAttributedString(
                NSAttributedString(string: line.text, attributes: [.font: block.font])
            )
            let startX = CTLineGetOffsetForStringIndex(
                coreTextLine,
                lower - line.range.location,
                nil
            )
            let endX = CTLineGetOffsetForStringIndex(
                coreTextLine,
                upper - line.range.location,
                nil
            )
            let highlight = CALayer()
            highlight.frame = CGRect(
                x: line.frame.minX - block.frame.minX + startX,
                y: line.frame.minY - block.frame.minY + 1,
                width: max(endX - startX, 1),
                height: max(line.frame.height - 2, 1)
            )
            highlight.cornerRadius = 1
            highlight.backgroundColor = resolvedCGColor(
                NSColor.selectedTextBackgroundColor.withAlphaComponent(0.55)
            )
            block.selectionLayer.addSublayer(highlight)
        }
    }

    private func updateDockerfileSelectionLayers() {
        withoutImplicitAnimations {
            for block in safeSpaceDockerfileTextBlocks {
                renderDockerfileSelection(in: block)
            }
        }
    }

    private func setDockerfileSelection(
        fragmentID: String?,
        range: NSRange?
    ) {
        selectedDockerfileFragmentID = fragmentID
        dockerfileSelectionRange = range
        updateDockerfileSelectionLayers()
        updateEditingAndPasteboardState()
    }

    private func dockerfileTextBlock(
        at point: CGPoint,
        contentSpace: AppsTextContentSpace
    ) -> DockerfileTextBlock? {
        safeSpaceDockerfileTextBlocks.first {
            $0.contentSpace == contentSpace && $0.frame.contains(point)
        }
    }

    private func dockerfileTextOffset(
        at point: CGPoint,
        in block: DockerfileTextBlock
    ) -> Int {
        guard !block.lines.isEmpty else { return 0 }
        let line = block.lines.min { lhs, rhs in
            abs(lhs.frame.midY - point.y) < abs(rhs.frame.midY - point.y)
        } ?? block.lines[0]
        let coreTextLine = CTLineCreateWithAttributedString(
            NSAttributedString(string: line.text, attributes: [.font: block.font])
        )
        let relativeIndex = CTLineGetStringIndexForPosition(
            coreTextLine,
            CGPoint(x: max(point.x - line.frame.minX, 0), y: 0)
        )
        if relativeIndex == kCFNotFound {
            return line.range.location + line.range.length
        }
        return min(
            max(line.range.location + relativeIndex, line.range.location),
            line.range.location + line.range.length
        )
    }

    private func dockerfileWordRange(
        containing offset: Int,
        in block: DockerfileTextBlock
    ) -> NSRange? {
        let string = block.text as NSString
        let length = string.length
        guard length > 0 else { return nil }
        var location = min(max(offset, 0), length - 1)
        if location > 0, !aboutCharacterIsWordLike(string.character(at: location)) {
            location -= 1
        }
        guard aboutCharacterIsWordLike(string.character(at: location)) else {
            return nil
        }
        var start = location
        while start > 0, aboutCharacterIsWordLike(string.character(at: start - 1)) {
            start -= 1
        }
        var end = location + 1
        while end < length, aboutCharacterIsWordLike(string.character(at: end)) {
            end += 1
        }
        return NSRange(location: start, length: end - start)
    }

    private func selectedDockerfileAttributedText() -> NSAttributedString? {
        guard let fragmentID = selectedDockerfileFragmentID,
              let block = safeSpaceDockerfileTextBlocks.first(where: {
                  $0.fragmentID == fragmentID
              }),
              let selection = normalizedDockerfileSelectionRange(
                  dockerfileSelectionRange,
                  in: block
              ) else {
            return nil
        }
        let attributed = NSAttributedString(
            string: block.text,
            attributes: [.font: block.font, .foregroundColor: NSColor.labelColor]
        )
        return attributed.attributedSubstring(from: selection)
    }

    private func handleDockerfileMouseDragged(to point: CGPoint) -> Bool {
        guard let fragmentID = selectedDockerfileFragmentID,
              let anchor = dockerfileDragAnchorOffset,
              let block = safeSpaceDockerfileTextBlocks.first(where: {
                  $0.fragmentID == fragmentID
              }) else {
            return false
        }
        let textPoint: CGPoint
        switch block.contentSpace {
        case .apps, .workspace:
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            textPoint = appsContentPoint(for: contentPoint)
        case .workspacePanel:
            textPoint = workspacePanelLayer.convert(point, from: rootLayer)
        }
        let offset = dockerfileTextOffset(at: textPoint, in: block)
        setDockerfileSelection(
            fragmentID: fragmentID,
            range: NSRange(location: min(anchor, offset), length: abs(offset - anchor))
        )
        return true
    }

    private func handleDockerfileRightMouseDown(
        at appsPoint: CGPoint,
        contentSpace: AppsTextContentSpace,
        rootPoint: CGPoint
    ) -> Bool {
        guard let block = dockerfileTextBlock(
            at: appsPoint,
            contentSpace: contentSpace
        ) else {
            return false
        }
        let offset = dockerfileTextOffset(at: appsPoint, in: block)
        if selectedDockerfileFragmentID != block.fragmentID ||
            normalizedDockerfileSelectionRange(dockerfileSelectionRange, in: block)?.contains(offset) != true {
            if let wordRange = dockerfileWordRange(containing: offset, in: block) {
                setDockerfileSelection(fragmentID: block.fragmentID, range: wordRange)
            } else {
                setDockerfileSelection(
                    fragmentID: block.fragmentID,
                    range: NSRange(location: 0, length: (block.text as NSString).length)
                )
            }
        }
        if let selectedText = selectedDockerfileAttributedText() {
            outerframeHost.showContextMenu(for: selectedText, at: rootPoint)
        }
        return true
    }

    private func overviewGroupID(for item: AppLauncherItem) -> String {
        if let context = item.containerContext { return "container:\(context.container.id.uuidString.lowercased())" }
        return item.backend.serviceScope == "system" ? "root" : "user"
    }

    private func overviewTitle(for item: AppLauncherItem) -> String {
        overviewLayout.names[overviewKey(for: item)] ?? item.displayName
    }

    private func renderOverviewCards(visibleIconKeys: inout Set<String>,
                                     visibleTextKeys: inout Set<String>) -> CGFloat {
        guard overviewLayoutReady, !backends.isEmpty || !isLoadingBackends,
              workspaceContextID != nil || workspaceOverviewAvailable else { return appsLayer.bounds.height - 18 }
        let width = max(appsLayer.bounds.width - horizontalInset * 2, 1)
        let gap: CGFloat = 18
        let columns = max(1, Int((width + gap) / 340))
        let cardWidth = (width - CGFloat(columns - 1) * gap) / CGFloat(columns)
        var tops = Array(repeating: appsLayer.bounds.height - 18, count: columns)
        let hostItems = appLauncherItems().filter { $0.backend.isInstalled ?? true }
        var groups: [(id: String, name: String, note: String, items: [AppLauncherItem], workspace: LocalWorkspaceRecord?)] = [
            ("user", overviewUsername, "User", hostItems.filter { $0.backend.serviceScope != "system" }, nil),
            ("root", "root", "Administrator", hostItems.filter { $0.backend.serviceScope == "system" }, nil)
        ]
        if workspaceContextID == nil {
            groups += localWorkspaces.map {
                ("container:\($0.id.uuidString.lowercased())", $0.name, "Container · \(displayedWorkspaceState(for: $0))", appLauncherItems(in: $0), $0)
            }
        }
        groups.sort {
            (overviewLayout.groups.firstIndex(of: $0.id) ?? Int.max) < (overviewLayout.groups.firstIndex(of: $1.id) ?? Int.max)
        }
        for (index, group) in groups.enumerated() {
            let column = index % columns
            let innerWidth = max(cardWidth - 32, 1)
            let pinColumns = max(1, Int(innerWidth / 96))
            let pins = overviewOrderedItems(group.items.filter(isAppProminent), group: group.id, pinned: true)
            let listed = overviewOrderedItems(group.items.filter { !isAppProminent($0) }, group: group.id, pinned: false)
            let pinHeight = pins.isEmpty ? 0 : CGFloat((pins.count + pinColumns - 1) / pinColumns) * 100 + 12
            let commands = group.workspace?.commandLaunchers ?? []
            let hasTerminal = !(group.workspace?.shellCommand.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ?? true)
            let commandCount = commands.count + (hasTerminal ? 1 : 0)
            let commandsHeight: CGFloat = commandCount == 0 ? 0 : 38 + CGFloat(commandCount) * 38
            let listHeight = max(CGFloat(listed.count) * 38, 28)
            let cardHeight = 80 + pinHeight + listHeight + commandsHeight + 62
            let frame = CGRect(x: horizontalInset + CGFloat(column) * (cardWidth + gap),
                               y: tops[column] - cardHeight, width: cardWidth, height: cardHeight)
            let card = CALayer()
            card.frame = frame
            card.cornerRadius = 14
            card.cornerCurve = .continuous
            card.backgroundColor = resolvedCGColor(.controlBackgroundColor)
            card.borderWidth = 0.5
            card.borderColor = resolvedCGColor(.separatorColor)
            addAppsSublayer(card)
            overviewGroupFrames.append((frame, CGRect(x: frame.minX, y: frame.maxY - 70, width: frame.width, height: 70), group.id))
            let titleWidth = max(innerWidth - (group.workspace == nil ? 0 : 88), 1)
            let heading = makeTextLayer(size: 18, weight: .semibold, color: .labelColor)
            heading.string = group.name
            heading.frame = CGRect(x: frame.minX + 16, y: frame.maxY - 41, width: titleWidth, height: 24)
            addAppsSublayer(heading)
            let note = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
            note.string = group.note
            note.frame = CGRect(x: frame.minX + 16, y: frame.maxY - 60, width: titleWidth, height: 18)
            addAppsSublayer(note)
            if let workspace = group.workspace {
                workspaceOverviewRowFrames.append((frame, workspace))
                let manage = CGRect(x: frame.maxX - 96, y: frame.maxY - 51, width: 80, height: 28)
                renderOverviewButton("Manage…", frame: manage)
                workspaceOverviewActionFrames.append((manage, workspace, "menu"))
            }
            var top = frame.maxY - 80
            let pinTop = top
            for (pinIndex, item) in pins.enumerated() {
                let tileWidth = innerWidth / CGFloat(pinColumns)
                let tile = CGRect(x: frame.minX + 16 + CGFloat(pinIndex % pinColumns) * tileWidth,
                                  y: top - CGFloat(pinIndex / pinColumns + 1) * 100, width: tileWidth, height: 100)
                renderOverviewEndpoint(item, frame: tile, pinned: true, separator: false,
                                       visibleIconKeys: &visibleIconKeys, visibleTextKeys: &visibleTextKeys)
            }
            top -= pinHeight
            overviewDropFrames.append((CGRect(x: frame.minX + 8, y: top, width: cardWidth - 16, height: max(pinTop - top, 20)), group.id, .pinned))
            let listTop = top
            for (row, item) in listed.enumerated() {
                let rowFrame = CGRect(x: frame.minX + 16, y: top - 38, width: innerWidth, height: 38)
                renderOverviewEndpoint(item, frame: rowFrame, pinned: false, separator: row > 0,
                                       visibleIconKeys: &visibleIconKeys, visibleTextKeys: &visibleTextKeys)
                top -= 38
            }
            if listed.isEmpty {
                if pins.isEmpty {
                    let empty = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
                    empty.string = isLoadingBackends ? "Loading…" : "No registered endpoints."
                    empty.frame = CGRect(x: frame.minX + 16, y: top - 22, width: innerWidth, height: 18)
                    addAppsSublayer(empty)
                }
                top -= 28
            }
            overviewDropFrames.append((CGRect(x: frame.minX + 8, y: top, width: cardWidth - 16, height: listTop - top), group.id, .moreApps))
            if let workspace = group.workspace, commandCount > 0 {
                let label = makeTextLayer(size: 12, weight: .medium, color: .secondaryLabelColor)
                label.string = "Command line tools"
                label.frame = CGRect(x: frame.minX + 16, y: top - 32, width: innerWidth, height: 18)
                addAppsSublayer(label)
                top -= 38
                if hasTerminal {
                    renderContainerTerminalRow(workspace: workspace,
                        frame: CGRect(x: frame.minX + 16, y: top - 38, width: innerWidth, height: 38),
                        showsSeparator: false, visibleIconKeys: &visibleIconKeys, visibleTextKeys: &visibleTextKeys)
                    top -= 38
                }
                for (commandIndex, command) in commands.enumerated() {
                    renderContainerCommandRow(workspace: workspace, command: command,
                        frame: CGRect(x: frame.minX + 16, y: top - 38, width: innerWidth, height: 38),
                        showsSeparator: hasTerminal || commandIndex > 0,
                        visibleIconKeys: &visibleIconKeys, visibleTextKeys: &visibleTextKeys)
                    top -= 38
                }
            }
            let add = CGRect(x: frame.minX + 16, y: frame.minY + 16,
                             width: min(innerWidth, group.workspace == nil ? 106 : 204), height: 28)
            renderOverviewButton(group.workspace == nil ? "Add more…" : "Add more to Dockerfile…", frame: add)
            if let workspace = group.workspace {
                workspaceOverviewActionFrames.append((add, workspace, "editContainer"))
            } else {
                overviewAddFrames.append(add)
            }
            tops[column] = frame.minY - gap
        }
        if workspaceContextID == nil && workspaceOverviewAvailable {
            let column = groups.count % columns
            let frame = CGRect(x: horizontalInset + CGFloat(column) * (cardWidth + gap), y: tops[column] - 90, width: cardWidth, height: 90)
            workspaceOverviewCreateFrame = frame
            renderAddContainerCard(frame: frame)
            tops[column] = frame.minY - gap
        }
        return (tops.min() ?? 0) - 8
    }

    private func scheduleOverviewDragFrame() {
        guard !overviewDragFrameScheduled else { return }
        overviewDragFrameScheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.overviewDragFrameScheduled = false
            self.withEffectiveAppearance {
                withoutImplicitAnimations { self.renderOverviewDragFrame() }
            }
        }
    }

    private func overviewGroupDrop(at point: CGPoint, source: String) -> (id: String, after: Bool, marker: CGRect)? {
        let candidates = overviewGroupFrames.filter { $0.id != source }
        guard let target = candidates.min(by: {
            func distance(_ frame: CGRect) -> CGFloat {
                let dx = max(frame.minX - point.x, 0, point.x - frame.maxX)
                let dy = max(frame.minY - point.y, 0, point.y - frame.maxY)
                return dx * dx + dy * dy
            }
            return distance($0.frame) < distance($1.frame)
        }) else { return nil }
        let multipleColumns = Set(overviewGroupFrames.map { $0.frame.minX }).count > 1
        let after = multipleColumns ? point.x > target.frame.midX : point.y < target.frame.midY
        let marker = multipleColumns
            ? CGRect(x: (after ? target.frame.maxX : target.frame.minX) + (after ? 7 : -9), y: target.frame.minY, width: 2, height: target.frame.height)
            : CGRect(x: target.frame.minX, y: (after ? target.frame.minY - 9 : target.frame.maxY + 7), width: target.frame.width, height: 2)
        return (target.id, after, marker)
    }

    private func overviewEndpointDrop(at point: CGPoint, for item: AppLauncherItem)
        -> (target: AppDropTarget, before: AppLauncherItem?, marker: CGRect, zone: CGRect)? {
        guard let zone = overviewDropFrames.first(where: {
            $0.group == overviewGroupID(for: item) && $0.frame.contains(point)
        }) else { return nil }
        let pinned = zone.target == .pinned
        let candidates = appCardFrames.filter {
            overviewGroupID(for: $0.item) == zone.group &&
            isAppProminent($0.item) == pinned && $0.item.identityKey != item.identityKey
        }
        let next = candidates.first {
            pinned
                ? point.y > $0.frame.maxY || (point.y >= $0.frame.minY && point.x < $0.frame.midX)
                : point.y > $0.frame.midY
        }
        let marker: CGRect
        if let next {
            marker = pinned
                ? CGRect(x: next.frame.minX, y: next.frame.minY + 8, width: 2, height: next.frame.height - 16)
                : CGRect(x: next.frame.minX, y: next.frame.maxY - 1, width: next.frame.width, height: 2)
        } else if let last = candidates.last {
            marker = pinned
                ? CGRect(x: last.frame.maxX - 2, y: last.frame.minY + 8, width: 2, height: last.frame.height - 16)
                : CGRect(x: last.frame.minX, y: last.frame.minY - 1, width: last.frame.width, height: 2)
        } else {
            marker = pinned
                ? CGRect(x: zone.frame.minX + 8, y: zone.frame.minY + 4, width: 2, height: max(zone.frame.height - 8, 12))
                : CGRect(x: zone.frame.minX + 8, y: zone.frame.maxY - 2, width: zone.frame.width - 16, height: 2)
        }
        return (zone.target, next?.item, marker, zone.frame)
    }

    private func renderOverviewDragFrame() {
        overviewDragFeedbackLayer.sublayers = nil
        overviewDragFeedbackLayer.removeAllAnimations()
        overviewDragFeedbackLayer.frame = rootLayer.bounds
        func outline(_ frame: CGRect, fill: Bool = false) {
            let layer = CALayer()
            layer.frame = overviewDragFeedbackLayer.convert(frame, from: appsScrollContentLayer)
            layer.cornerRadius = fill ? 8 : 1
            layer.backgroundColor = resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(fill ? 0.08 : 1))
            overviewDragFeedbackLayer.addSublayer(layer)
        }
        let key: String
        let point: CGPoint
        let previewSize: CGSize
        if let drag = pendingOverviewGroupDrag, drag.active,
           let source = overviewGroupFrames.first(where: { $0.id == drag.id }) {
            key = drag.id
            point = drag.current
            previewSize = CGSize(width: min(source.header.width, 280), height: 52)
            outline(source.header, fill: true)
            let contentPoint = appsScrollContentLayer.convert(point, from: rootLayer)
            if let target = overviewGroupDrop(at: contentPoint, source: drag.id) { outline(target.marker) }
            if overviewDragPreviewKey != key {
                let preview = CALayer()
                preview.backgroundColor = resolvedCGColor(.controlBackgroundColor)
                preview.borderColor = resolvedCGColor(.controlAccentColor)
                preview.borderWidth = 1
                preview.cornerRadius = 10
                preview.shadowOpacity = 0.18
                preview.shadowRadius = 8
                preview.shadowOffset = CGSize(width: 0, height: -3)
                let text = makeTextLayer(size: 16, weight: .semibold, color: .labelColor)
                text.string = drag.id == "user" ? overviewUsername : drag.id == "root" ? "root" : localWorkspaces.first { "container:\($0.id.uuidString.lowercased())" == drag.id }?.name ?? "Container"
                text.frame = CGRect(x: 16, y: 14, width: previewSize.width - 32, height: 24)
                preview.addSublayer(text)
                overviewDragPreview = preview
                overviewDragPreviewKey = key
            }
        } else if let drag = pendingAppDrag, drag.isDragging {
            key = drag.item.identityKey
            point = drag.currentPoint
            previewSize = CGSize(width: 144, height: 88)
            let contentPoint = appsScrollContentLayer.convert(point, from: rootLayer)
            if let drop = overviewEndpointDrop(at: contentPoint, for: drag.item) {
                outline(drop.zone, fill: true)
                outline(drop.marker)
            }
            if overviewDragPreviewKey != key {
                let preview = CALayer()
                let icon = makeLauncherIconLayer(image: launcherIconImage(for: drag.item),
                    symbolName: launcherIconSymbolName(for: drag.item), symbolColor: appIconTintColor(for: drag.item.backend),
                    title: overviewTitle(for: drag.item), iconSize: 44)
                icon.frame = CGRect(x: 50, y: 38, width: 44, height: 44)
                preview.addSublayer(icon)
                let text = makeTextLayer(size: 12, weight: .medium, color: .labelColor, alignment: .center)
                text.string = overviewTitle(for: drag.item)
                text.isWrapped = true
                text.frame = CGRect(x: 2, y: 2, width: 140, height: 32)
                preview.addSublayer(text)
                overviewDragPreview = preview
                overviewDragPreviewKey = key
            }
        } else {
            overviewDragPreview?.removeFromSuperlayer()
            overviewDragPreview = nil
            overviewDragPreviewKey = nil
            return
        }
        if let preview = overviewDragPreview {
            overviewDragFeedbackLayer.addSublayer(preview)
            let location = overviewDragFeedbackLayer.convert(point, from: rootLayer)
            preview.removeAllAnimations()
            preview.frame = CGRect(x: location.x - previewSize.width / 2, y: location.y - previewSize.height + 22,
                                   width: previewSize.width, height: previewSize.height)
        }
    }

    private func renderOverviewButton(_ title: String, frame: CGRect) {
        let background = CALayer()
        background.frame = frame
        background.cornerRadius = 7
        background.cornerCurve = .continuous
        background.backgroundColor = resolvedCGColor(.controlColor)
        background.borderWidth = 0.5
        background.borderColor = resolvedCGColor(.separatorColor)
        addAppsSublayer(background)
        let text = makeTextLayer(size: 12, weight: .regular, color: .labelColor, alignment: .center)
        text.string = title
        text.frame = frame.insetBy(dx: 6, dy: 5)
        addAppsSublayer(text)
    }

    private func renderOverviewEndpoint(_ item: AppLauncherItem, frame: CGRect, pinned: Bool,
                                         separator: Bool, visibleIconKeys: inout Set<String>,
                                         visibleTextKeys: inout Set<String>) {
        appCardFrames.append((frame, item))
        let title = overviewTitle(for: item)
        let size: CGFloat = pinned ? 44 : 24
        let iconFrame = CGRect(x: pinned ? frame.midX - size / 2 : frame.minX + 14,
                               y: pinned ? frame.maxY - size - 8 : frame.midY - size / 2,
                               width: size, height: size)
        recordMatchedIcon(key: item.iconKey, frame: rootLayer.convert(iconFrame, from: activeAppsContentLayer),
                          image: launcherIconImage(for: item), symbolName: launcherIconSymbolName(for: item),
                          symbolColor: appIconTintColor(for: item.backend), title: title)
        visibleIconKeys.insert(item.iconKey)
        let isThisPage = item.backend.isBackendsSelf && item.backend.serviceScope == "user"
        let textFrame = pinned
            ? CGRect(x: frame.minX + 12, y: frame.minY + 8, width: frame.width - 24, height: 32)
            : CGRect(x: iconFrame.maxX + 10, y: frame.midY - 9, width: max(frame.maxX - iconFrame.maxX - (isThisPage ? 98 : 42), 1), height: 18)
        recordMatchedText(key: item.iconKey, frame: rootLayer.convert(textFrame, from: activeAppsContentLayer),
                          title: title, fontSize: 13, weight: .regular,
                          alignment: pinned ? .center : .left, isWrapped: pinned)
        visibleTextKeys.insert(item.iconKey)
        if endpointIsRunning(item.primaryEndpoint) {
            let dot = CALayer()
            dot.backgroundColor = resolvedCGColor(.systemGreen)
            dot.cornerRadius = 2.5
            let titleWidth = (title as NSString).size(withAttributes: [.font: NSFont.systemFont(ofSize: 13)]).width
            let x = pinned ? max(frame.minX + 2, frame.midX - min(titleWidth, textFrame.width) / 2 - 11) : frame.minX
            dot.frame = CGRect(x: x, y: pinned ? textFrame.maxY - 11 : frame.midY - 2.5, width: 5, height: 5)
            addAppsSublayer(dot)
        }
        if !pinned {
            let menuFrame = CGRect(x: frame.maxX - 26, y: frame.minY, width: 26, height: frame.height)
            let menu = makeTextLayer(size: 16, weight: .medium, color: .secondaryLabelColor, alignment: .center)
            menu.string = "⋯"
            menu.frame = menuFrame.insetBy(dx: 0, dy: 8)
            addAppsSublayer(menu)
            overviewMenuFrames.append((menuFrame, item))
            if isThisPage {
                let note = makeTextLayer(size: 10, weight: .regular, color: .secondaryLabelColor, alignment: .right)
                note.string = "This page"
                note.frame = CGRect(x: menuFrame.minX - 58, y: frame.midY - 7, width: 54, height: 16)
                addAppsSublayer(note)
            }
            if separator {
                let line = CALayer()
                line.backgroundColor = resolvedCGColor(.separatorColor)
                line.frame = CGRect(x: frame.minX, y: frame.maxY, width: frame.width, height: 0.5)
                addAppsSublayer(line)
            }
        }
    }

    private func overviewKey(for item: AppLauncherItem) -> String {
        func json(_ values: [Any]) -> String {
            guard let data = try? JSONSerialization.data(withJSONObject: values, options: [.fragmentsAllowed, .withoutEscapingSlashes]),
                  let value = String(data: data, encoding: .utf8) else { return "" }
            return value
        }
        if let context = item.containerContext {
            let app = context.app
            return json(["container", context.container.id.uuidString.lowercased(), app.serviceID,
                         json([app.frontendID, app.socketPath, app.url, 0])])
        }
        return json(["host", item.backend.serviceScope, item.backend.serviceID,
                     item.frontend.id.isEmpty ? (frontendNavigationURL(item.frontend)?.absoluteString ?? item.frontend.url) : item.frontend.id])
    }

    private func overviewOrderedItems(_ items: [AppLauncherItem], group: String, pinned: Bool) -> [AppLauncherItem] {
        let order = (pinned ? overviewLayout.pins[group] : overviewLayout.order[group]) ?? []
        return items.sorted {
            let left = order.firstIndex(of: overviewKey(for: $0)) ?? Int.max
            let right = order.firstIndex(of: overviewKey(for: $1)) ?? Int.max
            return left == right ? overviewTitle(for: $0).localizedStandardCompare(overviewTitle(for: $1)) == .orderedAscending : left < right
        }
    }

    private func loadOverviewLayout() {
        guard !overviewLayoutRequestPending, !overviewLayoutSaving, let urlSession,
              let base = outerframeHost.pluginBaseURL(),
              let url = URL(string: "/api/layout", relativeTo: base)?.absoluteURL else { return }
        overviewLayoutRequestPending = true
        urlSession.dataTask(with: url) { [weak self] data, response, error in
            Task { @MainActor in
                guard let self else { return }
                self.overviewLayoutRequestPending = false
                do {
                    if let error { throw error }
                    guard (response as? HTTPURLResponse)?.statusCode == 200, let data else { throw URLError(.badServerResponse) }
                    let (layout, revision) = try OverviewLayout.decode(data)
                    guard !self.overviewLayoutSaving,
                          !self.overviewLayoutReady || revision > self.overviewLayoutRevision else { return }
                    self.overviewLayout = layout
                    self.overviewLayoutRevision = revision
                    self.overviewLayoutReady = true
                    self.scheduleLayoutUpdate()
                } catch {
                    self.backendError = "Could not load the layout: \(error.localizedDescription)"
                    self.scheduleLayoutUpdate()
                }
            }
        }.resume()
    }

    private func saveOverviewLayout(_ layout: OverviewLayout) {
        guard overviewLayoutReady, !overviewLayoutSaving, let urlSession,
              let base = outerframeHost.pluginBaseURL(),
              let url = URL(string: "/api/layout", relativeTo: base)?.absoluteURL else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        do { request.httpBody = try layout.encode(revision: overviewLayoutRevision) }
        catch {
            backendError = "Could not save the layout: \(error.localizedDescription)"
            updateLayout()
            return
        }
        let previous = overviewLayout
        overviewLayout = layout
        overviewLayoutSaving = true
        updateLayout()
        urlSession.dataTask(with: request) { [weak self] data, response, error in
            Task { @MainActor in
                guard let self else { return }
                self.overviewLayoutSaving = false
                do {
                    if let error { throw error }
                    let status = (response as? HTTPURLResponse)?.statusCode
                    guard status == 200, let data else { throw URLError(.badServerResponse) }
                    let (saved, revision) = try OverviewLayout.decode(data)
                    self.overviewLayout = saved
                    self.overviewLayoutRevision = revision
                } catch {
                    self.overviewLayout = previous
                    self.backendError = (response as? HTTPURLResponse)?.statusCode == 409
                        ? "The layout changed in another window. Try again after it reloads."
                        : "Could not save the layout: \(error.localizedDescription)"
                    self.loadOverviewLayout()
                }
                self.updateLayout()
            }
        }.resume()
    }

    private func moveOverviewItem(_ item: AppLauncherItem, pinned: Bool, before: AppLauncherItem? = nil) {
        let group = overviewGroupID(for: item)
        let items = item.containerContext.map { appLauncherItems(in: $0.container) } ?? appLauncherItems().filter { overviewGroupID(for: $0) == group }
        let key = overviewKey(for: item)
        var pins = overviewOrderedItems(items.filter(isAppProminent), group: group, pinned: true).map(overviewKey).filter { $0 != key }
        var list = overviewOrderedItems(items.filter { !isAppProminent($0) }, group: group, pinned: false).map(overviewKey).filter { $0 != key }
        let beforeKey = before.map(overviewKey)
        if pinned { pins.insert(key, at: beforeKey.flatMap { pins.firstIndex(of: $0) } ?? pins.count) }
        else { list.insert(key, at: beforeKey.flatMap { list.firstIndex(of: $0) } ?? list.count) }
        var layout = overviewLayout
        layout.pins[group] = pins
        layout.order[group] = list
        saveOverviewLayout(layout)
    }

    private func renderAddContainerCard(frame: CGRect) {
        let card = CALayer()
        card.frame = frame
        styleWorkspaceOverviewCard(card)
        addAppsSublayer(card)

        let iconSize: CGFloat = 26
        let iconFrame = CGRect(x: 18,
                               y: floor((frame.height - iconSize) / 2),
                               width: iconSize,
                               height: iconSize)
        let icon = CALayer()
        icon.frame = iconFrame
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: "plus.square", pointSize: iconSize)
        card.addSublayer(icon)

        let title = makeTextLayer(size: 13,
                                  weight: .medium,
                                  color: .labelColor,
                                  italic: true)
        title.string = "Add container"
        title.frame = CGRect(x: iconFrame.maxX + 12,
                             y: addWorkspaceMessage.isEmpty ? floor((frame.height - 20) / 2) : 51,
                             width: max(frame.width - iconFrame.maxX - 28, 1),
                             height: 20)
        card.addSublayer(title)

        if !addWorkspaceMessage.isEmpty {
            let detail = makeTextLayer(
                size: 11,
                weight: .regular,
                color: .systemRed
            )
            detail.string = addWorkspaceMessage
            detail.truncationMode = .end
            detail.frame = CGRect(x: iconFrame.maxX + 12,
                                  y: 27,
                                  width: max(frame.width - iconFrame.maxX - 28, 1),
                                  height: 18)
            card.addSublayer(detail)
        }
    }

    private func styleWorkspaceOverviewCard(_ card: CALayer) {
        let cornerRadius: CGFloat = 12
        card.cornerRadius = cornerRadius
        card.backgroundColor = resolvedCGColor(pageBackgroundColor())
        card.borderWidth = 2
        card.borderColor = resolvedCGColor(.separatorColor)
        card.shadowOpacity = 0
    }

    private func displayedWorkspaceState(for workspace: LocalWorkspaceRecord) -> String {
        let normalizedName = workspace.name.lowercased()
        if pendingWorkspaceCreationNames.values.contains(where: {
            $0.lowercased() == normalizedName
        }) {
            return "creating"
        }
        return workspace.state
    }

    private func showWorkspaceShellCommandMenu(_ workspace: LocalWorkspaceRecord,
                                               at point: CGPoint) {
        let containerCommand = workspace.shellCommand.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !containerCommand.isEmpty else {
            workspacePanelMessage = "The shell command for \(workspace.name) is not available yet."
            updateLayout()
            return
        }
        showContainerCommandMenu(workspace: workspace,
                                 command: nil,
                                 containerCommand: containerCommand,
                                 internalCommand: nil,
                                 at: point)
    }

    private func showWorkspaceCommandMenu(_ command: LocalWorkspaceCommandRecord,
                                          in workspace: LocalWorkspaceRecord,
                                          at point: CGPoint) {
        let containerCommand = command.containerCommand.trimmingCharacters(in: .whitespacesAndNewlines)
        let internalCommand = command.internalCommand.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !containerCommand.isEmpty, !internalCommand.isEmpty else {
            workspacePanelMessage = "The \(command.displayName) command for \(workspace.name) is not available yet."
            updateLayout()
            return
        }
        showContainerCommandMenu(workspace: workspace,
                                 command: command,
                                 containerCommand: containerCommand,
                                 internalCommand: internalCommand,
                                 at: point)
    }

    private func copyWorkspaceShellCommand(_ workspace: LocalWorkspaceRecord,
                                           at point: CGPoint) {
        let command = workspace.shellCommand.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !command.isEmpty else {
            workspacePanelMessage = "The shell command for \(workspace.name) is not available yet."
            updateLayout()
            return
        }
        copyCommandForCurrentServer(command) { [weak self] resolvedCommand in
            guard let self else { return }
            self.copyTextToPasteboard(resolvedCommand)
            self.workspacePanelMessage = ""
            self.showCommandCopiedConfirmation(at: point)
        }
    }

    private func copyWorkspaceCommand(_ command: LocalWorkspaceCommandRecord,
                                      in workspace: LocalWorkspaceRecord,
                                      at point: CGPoint) {
        let containerCommand = command.containerCommand.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        guard !containerCommand.isEmpty else {
            workspacePanelMessage = "The \(command.displayName) command for \(workspace.name) is not available yet."
            updateLayout()
            return
        }
        copyCommandForCurrentServer(containerCommand) { [weak self] resolvedCommand in
            guard let self else { return }
            self.copyTextToPasteboard(resolvedCommand)
            self.workspacePanelMessage = ""
            self.showCommandCopiedConfirmation(at: point)
        }
    }

    private func showWorkspaceOverviewCommandMenu(
        for operation: String,
        in workspace: LocalWorkspaceRecord,
        at point: CGPoint
    ) -> Bool {
        if operation == "copyShell" {
            showWorkspaceShellCommandMenu(workspace, at: point)
            return true
        }
        guard operation.hasPrefix("copyCommand:"),
              let command = workspace.commandLaunchers.first(where: {
                  $0.id == String(operation.dropFirst("copyCommand:".count))
              }) else {
            return false
        }
        showWorkspaceCommandMenu(command, in: workspace, at: point)
        return true
    }

    private func showContainerCommandMenu(workspace: LocalWorkspaceRecord,
                                          command: LocalWorkspaceCommandRecord?,
                                          containerCommand: String,
                                          internalCommand: String?,
                                          at point: CGPoint) {
        outerframeHost.requestOuterLoopSSHCommandArguments { [weak self] arguments in
            guard let self else { return }
            var commandByItemID: [String: String] = ["container": containerCommand]
            var items: [OuterframeContextMenuItem] = []
            if let arguments, !arguments.isEmpty {
                let sshCommand = self.commandForSSHArguments(arguments, command: containerCommand)
                commandByItemID["ssh"] = sshCommand
                items.append(OuterframeContextMenuItem(
                    id: "ssh",
                    title: internalCommand == nil
                        ? "SSH command, container command"
                        : "SSH command, container command, and internal command",
                    isEnabled: true,
                    systemImageName: "network"
                ))
            }
            items.append(OuterframeContextMenuItem(
                id: "container",
                title: internalCommand == nil
                    ? "Container command"
                    : "Container command and internal command",
                isEnabled: true,
                systemImageName: "shippingbox"
            ))
            if let internalCommand {
                commandByItemID["internal"] = internalCommand
                items.append(OuterframeContextMenuItem(
                    id: "internal",
                    title: "Internal command",
                    isEnabled: true,
                    systemImageName: "terminal"
                ))
            }
            let menuID = UUID()
            self.pendingContainerCommandMenuActions[menuID] = (
                workspace,
                command,
                commandByItemID,
                point
            )
            self.outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
        }
    }

    private func copyCommandForCurrentServer(_ command: String,
                                             completion: @escaping @MainActor (String) -> Void) {
        outerframeHost.requestOuterLoopSSHCommandArguments { arguments in
            guard let arguments, !arguments.isEmpty else {
                completion(command)
                return
            }
            completion(self.commandForSSHArguments(arguments, command: command))
        }
    }

    private func commandForSSHArguments(_ sourceArguments: [String], command: String) -> String {
        var arguments = sourceArguments
        if !arguments.contains("-t") && !arguments.contains("-tt") {
            arguments.insert("-t", at: 1)
        }
        arguments.append(command)
        return arguments.map(Self.shellQuotedArgument).joined(separator: " ")
    }

    private static func shellQuotedArgument(_ argument: String) -> String {
        let safeCharacters = CharacterSet.alphanumerics.union(
            CharacterSet(charactersIn: "@%_+=:,./-")
        )
        if !argument.isEmpty && argument.unicodeScalars.allSatisfy(safeCharacters.contains) {
            return argument
        }
        return "'\(argument.replacingOccurrences(of: "'", with: "'\\''"))'"
    }

    private func showCommandCopiedConfirmation(at anchor: CGPoint) {
        let confirmation = CopyConfirmation(id: UUID(), anchor: anchor)
        copyConfirmation = confirmation
        copyConfirmationLayer.removeAnimation(forKey: "copy-confirmation-fade-out")
        withoutImplicitAnimations {
            copyConfirmationLayer.opacity = 1
        }
        updateLayout()

        let animation = CABasicAnimation(keyPath: "opacity")
        animation.fromValue = 0
        animation.toValue = 1
        animation.duration = 0.12
        animation.timingFunction = CAMediaTimingFunction(name: .easeOut)
        copyConfirmationLayer.add(animation, forKey: "copy-confirmation-fade-in")

        DispatchQueue.main.asyncAfter(deadline: .now() + 2.6) { [weak self] in
            self?.dismissCopyConfirmation(id: confirmation.id)
        }
    }

    private func dismissCopyConfirmation(id: UUID) {
        guard copyConfirmation?.id == id else { return }
        let currentOpacity = copyConfirmationLayer.presentation()?.opacity ?? copyConfirmationLayer.opacity
        withoutImplicitAnimations {
            copyConfirmationLayer.opacity = 0
        }
        let animation = CABasicAnimation(keyPath: "opacity")
        animation.fromValue = currentOpacity
        animation.toValue = 0
        animation.duration = 0.14
        animation.timingFunction = CAMediaTimingFunction(name: .easeIn)
        copyConfirmationLayer.add(animation, forKey: "copy-confirmation-fade-out")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            guard let self, copyConfirmation?.id == id else { return }
            copyConfirmation = nil
            withoutImplicitAnimations {
                copyConfirmationLayer.isHidden = true
                copyConfirmationLayer.sublayers = nil
                copyConfirmationLayer.opacity = 1
            }
        }
    }

    private func renderCopyConfirmationIfNeeded(width: CGFloat, height: CGFloat) {
        copyConfirmationLayer.sublayers = nil
        guard let copyConfirmation else {
            copyConfirmationLayer.isHidden = true
            return
        }
        copyConfirmationLayer.isHidden = false

        let horizontalInset: CGFloat = 12
        let bubbleWidth = max(min(360, width - horizontalInset * 2), 1)
        let bubbleHeight: CGFloat = 62
        let arrowHeight: CGFloat = 8
        let anchor = CGPoint(x: min(max(copyConfirmation.anchor.x, horizontalInset), width - horizontalInset),
                             y: min(max(copyConfirmation.anchor.y, horizontalInset), height - horizontalInset))
        let fitsBelow = anchor.y - bubbleHeight - arrowHeight - 8 >= horizontalInset
        let bubbleY = fitsBelow
            ? anchor.y - bubbleHeight - arrowHeight - 6
            : min(anchor.y + arrowHeight + 6, height - bubbleHeight - horizontalInset)
        let bubbleX = min(max(anchor.x - bubbleWidth / 2, horizontalInset),
                          max(width - bubbleWidth - horizontalInset, horizontalInset))
        let bubbleFrame = CGRect(x: bubbleX,
                                 y: max(bubbleY, horizontalInset),
                                 width: bubbleWidth,
                                 height: bubbleHeight)
        let backgroundColor = NSColor.windowBackgroundColor

        let bubble = CALayer()
        bubble.frame = bubbleFrame
        bubble.cornerRadius = 10
        bubble.backgroundColor = resolvedCGColor(backgroundColor)
        bubble.borderWidth = 0.5
        bubble.borderColor = resolvedCGColor(NSColor.separatorColor)
        bubble.shadowColor = resolvedCGColor(NSColor.black)
        bubble.shadowOpacity = 0.18
        bubble.shadowRadius = 9
        bubble.shadowOffset = CGSize(width: 0, height: -2)
        copyConfirmationLayer.addSublayer(bubble)

        let arrowCenterX = min(max(anchor.x, bubbleFrame.minX + 20), bubbleFrame.maxX - 20)
        let arrow = CAShapeLayer()
        let path = CGMutablePath()
        if fitsBelow {
            path.move(to: CGPoint(x: arrowCenterX - 8, y: bubbleFrame.maxY - 0.5))
            path.addLine(to: CGPoint(x: arrowCenterX + 8, y: bubbleFrame.maxY - 0.5))
            path.addLine(to: CGPoint(x: anchor.x, y: min(anchor.y - 2, bubbleFrame.maxY + arrowHeight)))
        } else {
            path.move(to: CGPoint(x: arrowCenterX - 8, y: bubbleFrame.minY + 0.5))
            path.addLine(to: CGPoint(x: arrowCenterX + 8, y: bubbleFrame.minY + 0.5))
            path.addLine(to: CGPoint(x: anchor.x, y: max(anchor.y + 2, bubbleFrame.minY - arrowHeight)))
        }
        path.closeSubpath()
        arrow.path = path
        arrow.fillColor = resolvedCGColor(backgroundColor)
        copyConfirmationLayer.insertSublayer(arrow, below: bubble)

        let icon = CALayer()
        icon.frame = CGRect(x: 14, y: 19, width: 24, height: 24)
        icon.contents = symbolCGImage(named: "checkmark.circle.fill",
                                      pointSize: 12,
                                      color: .systemGreen)
        icon.contentsGravity = .resizeAspect
        bubble.addSublayer(icon)

        let title = makeTextLayer(size: 12,
                                  weight: .semibold,
                                  color: .labelColor)
        title.string = "Command copied."
        title.frame = CGRect(x: 48, y: 32, width: max(bubbleWidth - 62, 1), height: 17)
        bubble.addSublayer(title)

        let detail = makeTextLayer(size: 11,
                                   weight: .regular,
                                   color: .secondaryLabelColor)
        detail.string = "Paste into your favorite terminal app."
        detail.frame = CGRect(x: 48, y: 13, width: max(bubbleWidth - 62, 1), height: 16)
        bubble.addSublayer(detail)
    }

    private func performWorkspaceOverviewAction(operation: String,
                                                workspace: LocalWorkspaceRecord,
                                                at point: CGPoint) {
        if operation == "menu" {
            showWorkspaceOverviewActionsMenu(for: workspace, at: point)
        } else if operation == "editContainer" {
            navigateToRecipeSafeSpace(workspace.id, pushHistory: true)
        } else if operation == "copyShell" {
            showWorkspaceShellCommandMenu(workspace, at: point)
        } else if operation.hasPrefix("copyCommand:"),
                  let command = workspace.commandLaunchers.first(where: {
                      $0.id == String(operation.dropFirst("copyCommand:".count))
                  }) {
            showWorkspaceCommandMenu(command, in: workspace, at: point)
        } else if operation.hasPrefix("unmountFolder:") {
            let identifier = String(operation.dropFirst("unmountFolder:".count))
            guard let mountID = UUID(uuidString: identifier) else { return }
            sendWorkspaceRequest(operation: "unmountFolder",
                                 workspaceID: workspace.id,
                                 mountID: mountID)
        } else {
            sendWorkspaceRequest(operation: operation,
                                 workspaceID: workspace.id)
        }
    }

    private func isAppProminent(_ item: AppLauncherItem) -> Bool {
        if let keys = overviewLayout.pins[overviewGroupID(for: item)] {
            return keys.contains(overviewKey(for: item))
        }
        return ["org.outershell.files", "org.outershell.top"].contains(item.backend.serviceID.lowercased())
    }

    private func toggleAppOverflow(_ scope: AppLauncherScope) {
        let current = appOverflowAnimationProgress[scope] ??
            (expandedAppOverflowScopes.contains(scope) ? 1 : 0)
        let previousTarget = appOverflowAnimationTargets[scope] ??
            (expandedAppOverflowScopes.contains(scope) ? 1 : 0)
        let target: CGFloat = previousTarget > 0.5 ? 0 : 1
        if target > 0.5 {
            expandedAppOverflowScopes.insert(scope)
        }
        appOverflowAnimationTargets[scope] = target
        let generation = (appOverflowAnimationGenerations[scope] ?? 0) + 1
        appOverflowAnimationGenerations[scope] = generation
        let startTime = CACurrentMediaTime()
        let duration = max(0.1, 0.22 * abs(target - current))
        advanceAppOverflowAnimation(scope: scope,
                                    generation: generation,
                                    from: current,
                                    to: target,
                                    startTime: startTime,
                                    duration: duration)
    }

    private func advanceAppOverflowAnimation(scope: AppLauncherScope,
                                             generation: Int,
                                             from start: CGFloat,
                                             to target: CGFloat,
                                             startTime: CFTimeInterval,
                                             duration: CFTimeInterval) {
        guard appOverflowAnimationGenerations[scope] == generation else {
            return
        }
        let elapsed = CACurrentMediaTime() - startTime
        let linearProgress = min(max(elapsed / duration, 0), 1)
        let easedProgress = 1 - pow(1 - linearProgress, 3)
        appOverflowAnimationProgress[scope] =
            start + (target - start) * CGFloat(easedProgress)
        updateLayout()
        guard linearProgress < 1 else {
            appOverflowAnimationProgress.removeValue(forKey: scope)
            appOverflowAnimationTargets.removeValue(forKey: scope)
            if target < 0.5 {
                expandedAppOverflowScopes.remove(scope)
            }
            updateLayout()
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0 / 60.0) { [weak self] in
            self?.advanceAppOverflowAnimation(scope: scope,
                                              generation: generation,
                                              from: start,
                                              to: target,
                                              startTime: startTime,
                                              duration: duration)
        }
    }

    private func flatAppColumnCount(for width: CGFloat) -> Int {
        if width >= 1_080 {
            return 3
        }
        if width >= 700 {
            return 2
        }
        return 1
    }

    private func flatAppRowsHeight(itemCount: Int,
                                   includesAddApp: Bool,
                                   width: CGFloat) -> CGFloat {
        let totalCount = itemCount + (includesAddApp ? 1 : 0)
        guard totalCount > 0 else { return 0 }
        let columns = min(flatAppColumnCount(for: width), totalCount)
        let rows = Int(ceil(Double(totalCount) / Double(columns)))
        return 16 + CGFloat(rows) * 42
    }

    private func renderFlatAppRows(items: [AppLauncherItem],
                                   frame: CGRect,
                                   includesAddApp: Bool,
                                   visibleIconKeys: inout Set<String>,
                                   visibleTextKeys: inout Set<String>) {
        let totalCount = items.count + (includesAddApp ? 1 : 0)
        guard totalCount > 0 else { return }
        let rowHeight: CGFloat = 42
        let columnGap: CGFloat = 18
        let columns = min(flatAppColumnCount(for: frame.width), totalCount)
        let rows = Int(ceil(Double(totalCount) / Double(columns)))
        let columnWidth = floor(
            (frame.width - CGFloat(columns - 1) * columnGap) / CGFloat(columns)
        )

        for column in 0..<columns {
            let startIndex = column * rows
            let endIndex = min(startIndex + rows, items.count)
            let columnItems = startIndex < endIndex
                ? Array(items[startIndex..<endIndex])
                : []
            let columnFrame = CGRect(
                x: frame.minX + CGFloat(column) * (columnWidth + columnGap),
                y: frame.minY,
                width: column == columns - 1
                    ? frame.maxX - (frame.minX + CGFloat(column) * (columnWidth + columnGap))
                    : columnWidth,
                height: frame.height
            )
            renderAppListWidget(name: "",
                                items: columnItems,
                                frame: columnFrame,
                                rowHeight: rowHeight,
                                drawsBackground: false,
                                visibleIconKeys: &visibleIconKeys,
                                visibleTextKeys: &visibleTextKeys)
        }

        if includesAddApp {
            let addIndex = items.count
            let column = addIndex / rows
            let row = addIndex % rows
            let columnX = frame.minX + CGFloat(column) * (columnWidth + columnGap)
            let actualColumnWidth = column == columns - 1
                ? frame.maxX - columnX
                : columnWidth
            let rowFrame = CGRect(x: columnX + 8,
                                  y: frame.maxY - 8 - CGFloat(row + 1) * rowHeight,
                                  width: actualColumnWidth - 16,
                                  height: rowHeight)
            addAppFrame = rowFrame
            renderAddAppRow(frame: rowFrame, showsSeparator: row > 0)
        }
    }

    private func renderAppIconGrid(items: [AppLauncherItem],
                                   area: CGRect,
                                   top: CGFloat,
                                   includesAddTile: Bool,
                                   scope: AppLauncherScope = .server,
                                   visibleIconKeys: inout Set<String>,
                                   visibleTextKeys: inout Set<String>) -> CGFloat {
        guard !items.isEmpty || includesAddTile else { return top }

        let height = flatAppRowsHeight(itemCount: items.count,
                                       includesAddApp: includesAddTile,
                                       width: area.width)
        let frame = CGRect(x: area.minX, y: top - height, width: area.width, height: height)
        renderFlatAppRows(items: items,
                          frame: frame,
                          includesAddApp: includesAddTile,
                          visibleIconKeys: &visibleIconKeys,
                          visibleTextKeys: &visibleTextKeys)

        appUnlistedDropFrames.append((frame, scope, .pinned))
        return frame.minY
    }

    private func renderContainerTerminalRow(
        workspace: LocalWorkspaceRecord,
        frame: CGRect,
        showsSeparator: Bool,
        visibleIconKeys: inout Set<String>,
        visibleTextKeys: inout Set<String>
    ) {
        renderContainerCopyCommandRow(
            workspace: workspace,
            keySuffix: "terminal-command",
            title: "Terminal",
            symbolName: "apple.terminal",
            operation: "copyShell",
            frame: frame,
            showsSeparator: showsSeparator,
            visibleIconKeys: &visibleIconKeys,
            visibleTextKeys: &visibleTextKeys
        )
    }

    private func renderContainerCommandRow(
        workspace: LocalWorkspaceRecord,
        command: LocalWorkspaceCommandRecord,
        frame: CGRect,
        showsSeparator: Bool,
        visibleIconKeys: inout Set<String>,
        visibleTextKeys: inout Set<String>
    ) {
        renderContainerCopyCommandRow(
            workspace: workspace,
            keySuffix: "command:\(command.id)",
            title: command.displayName,
            image: command.iconURL.flatMap { endpointIcons[$0] } ?? command.iconData.flatMap(decodedIconCGImage),
            symbolName: "apple.terminal",
            operation: "copyCommand:\(command.id)",
            frame: frame,
            showsSeparator: showsSeparator,
            visibleIconKeys: &visibleIconKeys,
            visibleTextKeys: &visibleTextKeys
        )
    }

    private func renderContainerCopyCommandRow(
        workspace: LocalWorkspaceRecord,
        keySuffix: String,
        title: String,
        image: CGImage? = nil,
        symbolName: String,
        operation: String,
        frame: CGRect,
        showsSeparator: Bool,
        visibleIconKeys: inout Set<String>,
        visibleTextKeys: inout Set<String>
    ) {
        let iconSize: CGFloat = 24
        let rowInset: CGFloat = 14
        let key = "container:\(workspace.id.uuidString):\(keySuffix)"

        if showsSeparator {
            let separator = CALayer()
            separator.backgroundColor = resolvedCGColor(.separatorColor)
            separator.frame = CGRect(
                x: frame.minX + rowInset + iconSize + 10,
                y: frame.maxY - 0.5,
                width: max(frame.width - rowInset - iconSize - 22, 1),
                height: 0.5
            )
            addAppsSublayer(separator)
        }

        let iconFrame = CGRect(
            x: frame.minX + rowInset,
            y: frame.midY - iconSize / 2,
            width: iconSize,
            height: iconSize
        )
        recordMatchedIcon(
            key: key,
            frame: rootLayer.convert(iconFrame, from: activeAppsContentLayer),
            image: image,
            symbolName: symbolName,
            symbolColor: .controlAccentColor,
            title: title
        )
        visibleIconKeys.insert(key)

        let textFrame = CGRect(
            x: iconFrame.maxX + 10,
            y: frame.midY - 9,
            width: max(frame.maxX - iconFrame.maxX - 42, 1),
            height: 18
        )
        recordMatchedText(
            key: key,
            frame: rootLayer.convert(textFrame, from: activeAppsContentLayer),
            title: title,
            fontSize: 13,
            weight: .regular,
            alignment: .left,
            isWrapped: false
        )
        visibleTextKeys.insert(key)
        let copy = CALayer()
        copy.frame = CGRect(x: frame.maxX - 20, y: frame.midY - 7, width: 14, height: 14)
        copy.contentsGravity = .resizeAspect
        copy.contents = symbolCGImage(named: "doc.on.doc", pointSize: 14)
        copy.opacity = 0.65
        addAppsSublayer(copy)
        workspaceOverviewActionFrames.append((frame, workspace, operation))
    }

    private func renderAddAppRow(frame: CGRect, showsSeparator: Bool) {
        let iconSize: CGFloat = 26
        if showsSeparator {
            let separator = CALayer()
            separator.backgroundColor = resolvedCGColor(.separatorColor)
            separator.frame = CGRect(x: frame.minX + 48,
                                     y: frame.maxY - 0.5,
                                     width: max(frame.width - 60, 1),
                                     height: 0.5)
            addAppsSublayer(separator)
        }

        let iconFrame = CGRect(x: frame.minX + 10,
                               y: frame.midY - iconSize / 2,
                               width: iconSize,
                               height: iconSize)
        let icon = CALayer()
        icon.frame = iconFrame
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: "plus.square", pointSize: iconSize)
        addAppsSublayer(icon)

        let title = makeTextLayer(size: 13, weight: .medium, color: .labelColor, italic: true)
        title.string = "Add app"
        title.frame = CGRect(x: iconFrame.maxX + 12,
                             y: frame.midY - 10,
                             width: max(frame.maxX - iconFrame.maxX - 28, 1),
                             height: 20)
        addAppsSublayer(title)
    }

    private func addCreateFormSublayer(_ layer: CALayer) {
        createFormContentLayer.addSublayer(layer)
    }

    private func renderCreateSectionCards(left: CGFloat,
                                          top: CGFloat,
                                          width: CGFloat) -> CGFloat {
        let gap: CGFloat = 12
        let cardHeight: CGFloat = 72
        let columns: CGFloat = width >= 820 ? 4 : (width >= 520 ? 2 : 1)
        let cardWidth = floor((width - gap * (columns - 1)) / columns)
        let sections: [(CreateSection, String, String)] = [
            (.appCatalog, "Outer Shell app catalog", "square.grid.2x2"),
            (.bashCommands, "Run bash commands", "terminal"),
            (.nativeApp, "New App", "hammer"),
            (.otherRecipes, "Other recipes", "list.bullet.rectangle")
        ]

        var x = left
        var y = top - cardHeight
        for (index, section) in sections.enumerated() {
            if columns == 1 {
                if index > 0 {
                    y -= cardHeight + gap
                }
                x = left
            } else {
                let column = CGFloat(index).truncatingRemainder(dividingBy: columns)
                if index > 0 && column == 0 {
                    y -= cardHeight + gap
                }
                x = left + column * (cardWidth + gap)
            }
            let frame = CGRect(x: x, y: y, width: cardWidth, height: cardHeight)
            renderCreateSectionCard(section: section.0,
                                    title: section.1,
                                    symbolName: section.2,
                                    frame: frame)
        }

        return y - 34
    }

    private func renderCreateSectionCard(section: CreateSection,
                                         title: String,
                                         symbolName: String,
                                         frame: CGRect) {
        createSectionFrames.append((frame, section))
        let selected = selectedCreateSection == section
        let card = CALayer()
        card.frame = frame
        card.cornerRadius = 8
        card.borderWidth = selected ? 1.5 : 1
        card.borderColor = selected ? resolvedCGColor(.controlAccentColor) : resolvedCGColor(.separatorColor)
        card.backgroundColor = selected ? resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.12)) : resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.45))
        addCreateFormSublayer(card)

        let iconSize: CGFloat = 24
        let icon = CALayer()
        icon.frame = CGRect(x: 14, y: floor((frame.height - iconSize) / 2), width: iconSize, height: iconSize)
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: symbolName, pointSize: iconSize)
        card.addSublayer(icon)

        let titleLayer = makeTextLayer(size: 12, weight: .semibold, color: .labelColor)
        titleLayer.string = title
        titleLayer.frame = CGRect(x: 48, y: floor((frame.height - 16) / 2), width: max(frame.width - 62, 1), height: 16)
        card.addSublayer(titleLayer)
    }

    private func renderCreateEmptyMessage(_ message: String,
                                          left: CGFloat,
                                          top: CGFloat,
                                          width: CGFloat) {
        let empty = makeTextLayer(size: 13, weight: .regular, color: .secondaryLabelColor)
        empty.string = message
        empty.frame = CGRect(x: left, y: top - 24, width: width, height: 20)
        addCreateFormSublayer(empty)
    }

    private func renderBashCommandsSection(left: CGFloat,
                                           top: CGFloat,
                                           width: CGFloat) -> CGFloat {
        ensureBashDefaults()

        let detailWidth = min(width, 680)
        let detailLeft = left
        let transportWidth: CGFloat = min(260, detailWidth)
        var y = top - 18

        let help = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        help.string = "Run a script that launches a web app on some port or socket"
        help.frame = CGRect(x: detailLeft, y: y, width: detailWidth, height: 18)
        addCreateFormSublayer(help)
        y -= 58

        addCreateField(RecipeFieldRecord(key: "bashDisplayName",
                                         label: "Display Name",
                                         defaultValue: "",
                                         fieldType: "text",
                                         placeholder: "My App",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["bashDisplayName", default: ""],
                       frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46))
        y -= 26

        addCreateTextArea(key: "bashCommands",
                          label: "Bash commands",
                          placeholder: "Paste commands here",
                          frame: CGRect(x: detailLeft, y: y - 132, width: detailWidth, height: 132))
        y -= 194

        let transportField = RecipeFieldRecord(key: "bashFrontendTransport",
                                                label: "Connection",
                                                defaultValue: "port",
                                                fieldType: "choice",
                                                placeholder: "",
                                                suggestions: [],
                                               choices: [
                                                    RecipeChoiceRecord(title: "Port", value: "port"),
                                                    RecipeChoiceRecord(title: "Unix Socket", value: "unixSocket")
                                                ])
        addCreateChoiceField(transportField, frame: CGRect(x: detailLeft, y: y, width: transportWidth, height: 50))
        let endpointFieldX = detailLeft + transportWidth + 18
        let endpointFieldWidth = max(detailWidth - transportWidth - 18, 180)

        if createValues["bashFrontendTransport", default: "port"] == "unixSocket" {
            addCreateField(RecipeFieldRecord(key: "bashSocketPath",
                                             label: "Socket Path",
                                             defaultValue: "",
                                             fieldType: "text",
                                             placeholder: "/tmp/my-service.sock",
                                             suggestions: [],
                                             choices: []),
                           value: createValues["bashSocketPath", default: ""],
                           frame: CGRect(x: endpointFieldX, y: y, width: endpointFieldWidth, height: 46),
                           monospaced: true)
        } else {
            addCreateField(RecipeFieldRecord(key: "bashPort",
                                             label: "Port",
                                             defaultValue: "",
                                             fieldType: "text",
                                             placeholder: "4000",
                                             suggestions: [],
                                             choices: []),
                           value: createValues["bashPort", default: ""],
                           frame: CGRect(x: endpointFieldX, y: y, width: min(endpointFieldWidth, 180), height: 46),
                           monospaced: true)
        }
        y -= 72

        let iconButtonWidth: CGFloat = 70
        addCreateField(RecipeFieldRecord(key: "bashIconPath",
                                         label: "Icon Path",
                                         defaultValue: "",
                                         fieldType: "text",
                                         placeholder: "Optional",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["bashIconPath", default: ""],
                       frame: CGRect(x: detailLeft, y: y, width: max(detailWidth - iconButtonWidth - 10, 160), height: 46),
                       monospaced: true)
        bashIconSelectFrame = CGRect(x: detailLeft + max(detailWidth - iconButtonWidth, 160),
                                     y: y,
                                     width: iconButtonWidth,
                                     height: 30)
        let iconSelect = makeButtonLayer(title: "Select", emphasized: false)
        iconSelect.frame = bashIconSelectFrame
        addCreateFormSublayer(iconSelect)
        y -= 72

        addCreateField(RecipeFieldRecord(key: "bashIdentifier",
                                         label: "ID",
                                         defaultValue: "",
                                         fieldType: "text",
                                         placeholder: "my-app",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["bashIdentifier", default: ""],
                       frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46),
                       monospaced: true)
        y -= 58

        createButtonFrame = CGRect(x: detailLeft, y: y, width: 76, height: 30)
        let save = makeButtonLayer(title: isPerformingAction ? "Saving..." : "Save", emphasized: true)
        save.frame = createButtonFrame
        addCreateFormSublayer(save)

        if !createMessage.isEmpty {
            addCreateMessageLayer(frame: CGRect(x: detailLeft + 88,
                                                y: y + 6,
                                                width: max(detailWidth - 88, 1),
                                                height: 18),
                                  color: createMessage.hasPrefix("Created") ? .secondaryLabelColor : .systemRed)
        }

        return y - 26
    }

    private func renderNativeAppSection(left: CGFloat,
                                        top: CGFloat,
                                        width: CGFloat) -> CGFloat {
        ensureNativeAppDefaults()

        let detailWidth = min(width, 680)
        let detailLeft = left
        var y = top - 18

        let help = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        help.string = "Create user-owned source on this server with separate implementations per platform"
        help.frame = CGRect(x: detailLeft, y: y, width: detailWidth, height: 18)
        addCreateFormSublayer(help)
        y -= 58

        addCreateField(RecipeFieldRecord(key: "nativeAppName",
                                         label: "Name",
                                         defaultValue: "",
                                         fieldType: "text",
                                         placeholder: "Hello World",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["nativeAppName", default: ""],
                       frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46))
        y -= 72

        let targetsLabel = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
        targetsLabel.string = "Platforms"
        targetsLabel.frame = CGRect(x: detailLeft, y: y + 34, width: detailWidth, height: 14)
        addCreateFormSublayer(targetsLabel)
        let targetWidth = min(floor((detailWidth - 10) / 2), 210)
        addCreateCheckbox(key: "nativeTargetHTML",
                          title: "HTML",
                          subtitle: "Browsers and web views",
                          frame: CGRect(x: detailLeft, y: y - 8, width: targetWidth, height: 36))
        addCreateCheckbox(key: "nativeTargetMacOS",
                          title: "macOS",
                          subtitle: "Compiled outerframe bundle",
                          frame: CGRect(x: detailLeft + targetWidth + 10, y: y - 8, width: targetWidth, height: 36))
        y -= 72

        if createValues["nativeTargetMacOS", default: "true"] == "true" {
            let frontendLanguageField = RecipeFieldRecord(key: "nativeFrontendLanguage",
                                                          label: "macOS Language",
                                                          defaultValue: "swift",
                                                          fieldType: "choice",
                                                          placeholder: "",
                                                          suggestions: [],
                                                          choices: [
                                                            RecipeChoiceRecord(title: "Swift", value: "swift"),
                                                            RecipeChoiceRecord(title: "Objective-C", value: "objc")
                                                          ])
            addCreateChoiceField(frontendLanguageField, frame: CGRect(x: detailLeft,
                                                                      y: y,
                                                                      width: min(detailWidth, 300),
                                                                      height: 50))
            y -= 72
        }

        let backendLanguageField = RecipeFieldRecord(key: "nativeBackendLanguage",
                                                     label: "Backend",
                                                     defaultValue: "go",
                                                     fieldType: "choice",
                                                     placeholder: "",
                                                     suggestions: [],
                                                     choices: [
                                                        RecipeChoiceRecord(title: "Go", value: "go"),
                                                        RecipeChoiceRecord(title: "C", value: "c")
                                                     ])
        addCreateChoiceField(backendLanguageField, frame: CGRect(x: detailLeft,
                                                                 y: y,
                                                                 width: min(detailWidth, 220),
                                                                 height: 50))
        y -= 72

        let isolationField = RecipeFieldRecord(key: "nativeIsolationMode",
                                               label: "Server Isolation",
                                               defaultValue: "container",
                                               fieldType: "choice",
                                               placeholder: "",
                                               suggestions: [],
                                               choices: [
                                                RecipeChoiceRecord(title: "Containerized", value: "container"),
                                                RecipeChoiceRecord(title: "Full Host Access", value: "host")
                                               ])
        addCreateChoiceField(isolationField, frame: CGRect(x: detailLeft,
                                                           y: y,
                                                           width: min(detailWidth, 360),
                                                           height: 50))
        y -= 72

        addCreateField(RecipeFieldRecord(key: "nativeProjectRoot",
                                         label: "Project Location",
                                         defaultValue: "~/outerframe-apps",
                                         fieldType: "directory",
                                         placeholder: "~/outerframe-apps",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["nativeProjectRoot", default: "~/outerframe-apps"],
                       frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46),
                       monospaced: true)
        y -= 72

        addCreateField(RecipeFieldRecord(key: "nativeProjectFolder",
                                         label: "Project Folder",
                                         defaultValue: "",
                                         fieldType: "text",
                                         placeholder: "hello-world",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["nativeProjectFolder", default: ""],
                       frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46),
                       monospaced: true)
        y -= 72

        addCreateField(RecipeFieldRecord(key: "nativeAppID",
                                         label: "App ID",
                                         defaultValue: "",
                                         fieldType: "text",
                                         placeholder: "org.example.HelloWorld",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["nativeAppID", default: ""],
                       frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46),
                       monospaced: true)
        y -= 72

        addCreateField(RecipeFieldRecord(key: "nativeSocketFilename",
                                         label: "Socket Filename",
                                         defaultValue: "",
                                         fieldType: "text",
                                         placeholder: "org.example.HelloWorld",
                                         suggestions: [],
                                         choices: []),
                       value: createValues["nativeSocketFilename", default: ""],
                       frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46),
                       monospaced: true)
        y -= 58

        createButtonFrame = CGRect(x: detailLeft, y: y, width: 96, height: 30)
        let generate = makeButtonLayer(title: isPerformingAction ? "Generating..." : "Generate", emphasized: true)
        generate.frame = createButtonFrame
        addCreateFormSublayer(generate)

        if !createMessage.isEmpty {
            addCreateMessageLayer(frame: CGRect(x: detailLeft + 108,
                                                y: y + 6,
                                                width: max(detailWidth - 108, 1),
                                                height: 18),
                                  color: createMessage.hasPrefix("Installed") ? .secondaryLabelColor : .systemRed)
        }
        return y - 56
    }

    private func renderGeneratedNativeProjectView(_ project: GeneratedNativeAppProject,
                                                  panelFrame: CGRect,
                                                  width: CGFloat) -> CGFloat {
        let centerX = panelFrame.midX
        let top = max(panelFrame.maxY - 102, 0) + createScroll

        let title = makeTextLayer(size: 18, weight: .semibold, color: .labelColor, alignment: .center)
        if !project.hasPlatformWorkspace {
            title.string = "Installed on the server at \(project.remoteProjectPath)."
        } else if generatedNativeProjectWasExported {
            title.string = "The canonical project stays at \(project.remoteProjectPath). Run \"./platform publish\" in the Mac builder to compile and publish only macOS."
        } else {
            title.string = "Installed on the server at \(project.remoteProjectPath). Drag this macOS builder to your Mac."
        }
        let titleHeight: CGFloat = 58
        title.frame = CGRect(x: panelFrame.minX + 42,
                             y: (!project.hasPlatformWorkspace || generatedNativeProjectWasExported) ? panelFrame.midY - titleHeight / 2 : top,
                             width: max(panelFrame.width - 84, 1),
                             height: titleHeight)
        title.isWrapped = true
        addCreateFormSublayer(title)

        if !project.hasPlatformWorkspace || generatedNativeProjectWasExported {
            nativeProjectDragFrame = .zero
            return top - 84
        }
        guard let projectURL = project.projectURL else { return top - 84 }

        let iconSize: CGFloat = 96
        let iconPlateSize: CGFloat = 116
        let labelHeight: CGFloat = 24
        let nameFont = NSFont.systemFont(ofSize: 13, weight: .medium)
        let measuredNameWidth = (project.folderName as NSString).size(withAttributes: [.font: nameFont]).width
        let labelWidth = min(max(ceil(measuredNameWidth) + 18, 36), min(width - 96, 280))
        let tileWidth = max(iconPlateSize, labelWidth)
        let tileHeight = iconPlateSize + 6 + labelHeight
        let tileTop = top - 70
        nativeProjectDragFrame = CGRect(x: centerX - tileWidth / 2,
                                        y: tileTop - tileHeight,
                                        width: tileWidth,
                                        height: tileHeight)

        let iconPlateFrame = CGRect(x: nativeProjectDragFrame.midX - iconPlateSize / 2,
                                    y: tileTop - iconPlateSize,
                                    width: iconPlateSize,
                                    height: iconPlateSize)
        if nativeProjectSelectionState == .selected {
            let iconPlate = CALayer()
            iconPlate.frame = iconPlateFrame
            iconPlate.cornerRadius = 10
            iconPlate.backgroundColor = resolvedCGColor(.unemphasizedSelectedContentBackgroundColor)
            addCreateFormSublayer(iconPlate)
        }

        let icon = CALayer()
        icon.frame = CGRect(x: iconPlateFrame.midX - iconSize / 2,
                            y: iconPlateFrame.midY - iconSize / 2,
                            width: iconSize,
                            height: iconSize)
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = max(NSScreen.main?.backingScaleFactor ?? 2, 1)
        icon.contents = folderIconCGImage(for: projectURL, pointSize: iconSize)
        addCreateFormSublayer(icon)

        let labelFrame = CGRect(x: nativeProjectDragFrame.midX - labelWidth / 2,
                                y: nativeProjectDragFrame.minY,
                                width: labelWidth,
                                height: labelHeight)
        if nativeProjectSelectionState == .selected {
            let highlight = CALayer()
            highlight.frame = labelFrame.insetBy(dx: 0, dy: 2)
            highlight.cornerRadius = 6
            highlight.backgroundColor = resolvedCGColor(.controlAccentColor)
            addCreateFormSublayer(highlight)
        }

        let name = makeTextLayer(size: 13,
                                 weight: .medium,
                                 color: nativeProjectSelectionState == .selected ? .alternateSelectedControlTextColor : .labelColor,
                                 alignment: .center)
        name.string = project.folderName
        name.isWrapped = true
        name.truncationMode = CATextLayerTruncationMode.end
        name.frame = labelFrame.insetBy(dx: 0, dy: 3).offsetBy(dx: 0, dy: -1)
        addCreateFormSublayer(name)

        return nativeProjectDragFrame.minY - 28
    }

    private func addCreateMessageLayer(frame: CGRect, color: NSColor) {
        createMessageFrame = frame
        let selectionRange = normalizedCreateMessageSelectionRange()
        if let selectionRange, selectionRange.length > 0 {
            let line = createMessageLine()
            var startSecondary: CGFloat = 0
            var endSecondary: CGFloat = 0
            let start = CGFloat(CTLineGetOffsetForStringIndex(line, selectionRange.location, &startSecondary))
            let end = CGFloat(CTLineGetOffsetForStringIndex(line, selectionRange.location + selectionRange.length, &endSecondary))
            let selection = CALayer()
            selection.frame = CGRect(x: frame.minX + min(start, end),
                                     y: frame.minY + 1,
                                     width: max(abs(end - start), 1),
                                     height: frame.height - 2)
            selection.cornerRadius = 2
            selection.backgroundColor = resolvedCGColor(windowIsActive ? NSColor.selectedTextBackgroundColor : NSColor.unemphasizedSelectedTextBackgroundColor)
            addCreateFormSublayer(selection)
        }

        let message = makeTextLayer(size: 12, weight: .regular, color: color)
        message.string = createMessage
        message.truncationMode = .end
        message.frame = frame
        addCreateFormSublayer(message)
    }

    private func createMessageFont() -> NSFont {
        NSFont.systemFont(ofSize: 12, weight: .regular)
    }

    private func createMessageLine() -> CTLine {
        CTLineCreateWithAttributedString(NSAttributedString(string: createMessage,
                                                            attributes: [.font: createMessageFont()]))
    }

    private func createMessageCharacterIndex(at point: CGPoint) -> Int {
        let length = (createMessage as NSString).length
        guard length > 0 else { return 0 }
        let x = max(point.x - createMessageFrame.minX, 0)
        if x <= 0 { return 0 }
        let index = CTLineGetStringIndexForPosition(createMessageLine(), CGPoint(x: x, y: 0))
        if index == kCFNotFound { return length }
        return min(max(index, 0), length)
    }

    private func normalizedCreateMessageSelectionRange() -> NSRange? {
        let length = (createMessage as NSString).length
        guard length > 0, let range = createMessageSelectionRange else { return nil }
        let location = min(max(range.location, 0), length)
        let end = min(max(range.location + range.length, location), length)
        guard end > location else { return nil }
        return NSRange(location: location, length: end - location)
    }

    private func setCreateMessageSelectionRange(_ range: NSRange?) {
        createMessageSelectionRange = range
        updateEditingAndPasteboardState()
        updateLayout()
    }

    private func createMessageWordRange(containing offset: Int) -> NSRange? {
        let string = createMessage as NSString
        let length = string.length
        guard length > 0 else { return nil }
        var location = min(max(offset, 0), length - 1)
        if location > 0, !createMessageCharacterIsWordLike(string.character(at: location)) {
            location -= 1
        }
        guard createMessageCharacterIsWordLike(string.character(at: location)) else {
            return NSRange(location: min(max(offset, 0), length), length: 0)
        }

        var start = location
        while start > 0, createMessageCharacterIsWordLike(string.character(at: start - 1)) {
            start -= 1
        }
        var end = location + 1
        while end < length, createMessageCharacterIsWordLike(string.character(at: end)) {
            end += 1
        }
        return NSRange(location: start, length: end - start)
    }

    private func createMessageCharacterIsWordLike(_ character: unichar) -> Bool {
        if character >= 48 && character <= 57 { return true }
        if character >= 65 && character <= 90 { return true }
        if character >= 97 && character <= 122 { return true }
        return character == 45 || character == 46 || character == 47 || character == 95 || character == 126
    }

    private func selectedCreateMessageAttributedText() -> NSAttributedString? {
        guard let range = normalizedCreateMessageSelectionRange() else { return nil }
        return NSAttributedString(string: createMessage,
                                  attributes: [.font: createMessageFont()])
            .attributedSubstring(from: range)
    }

    private func renderAddableAppsSection(apps: [BundledCatalogEntry],
                                          left: CGFloat,
                                          top: CGFloat,
                                          width: CGFloat) -> CGFloat {
        let itemHeight: CGFloat = 108
        let itemGap: CGFloat = 12
        let itemWidth: CGFloat = 112
        let columns = max(Int((width + itemGap) / (itemWidth + itemGap)), 1)
        let usedWidth = CGFloat(columns) * itemWidth + CGFloat(max(columns - 1, 0)) * itemGap
        let startX = left + max(floor((width - usedWidth) / 2), 0)
        var x = left
        var y = top - itemHeight

        for (index, app) in apps.enumerated() {
            if index == 0 {
                x = startX
            } else if index.isMultiple(of: columns) {
                x = startX
                y -= itemHeight + itemGap
            }
            let frame = CGRect(x: x, y: y, width: itemWidth, height: itemHeight)
            renderAddableAppTile(app.backend, isInstalled: app.isInstalled, frame: frame)
            x += itemWidth + itemGap
        }

        return y - 34
    }

    private func renderAddableAppTile(_ backend: BackendRecord,
                                      isInstalled installed: Bool,
                                      frame: CGRect) {
        let iconSize: CGFloat = 46
        let symbolPointSize: CGFloat = 40
        let iconFrame = CGRect(x: frame.minX + floor((frame.width - iconSize) / 2),
                               y: frame.maxY - iconSize - 10,
                               width: iconSize,
                               height: iconSize)
        let icon = CALayer()
        icon.frame = iconFrame
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        if let symbolName = appIconSymbolName(for: backend),
           !symbolName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            icon.contents = symbolCGImage(named: symbolName,
                                          pointSize: symbolPointSize,
                                          color: appIconTintColor(for: backend))
        } else {
            icon.contents = letterIconCGImage(for: backend.displayName)
        }
        icon.opacity = installed ? 0.62 : 1
        addCreateFormSublayer(icon)

        let name = makeTextLayer(size: 12, weight: .medium, color: installed ? .secondaryLabelColor : .labelColor, alignment: .center)
        name.string = backend.displayName
        name.isWrapped = true
        name.truncationMode = .none
        name.frame = CGRect(x: frame.minX, y: frame.minY + 20, width: frame.width, height: 28)
        addCreateFormSublayer(name)

        if installed {
            let installedLayer = makeTextLayer(size: 10, weight: .medium, color: .secondaryLabelColor, alignment: .center)
            installedLayer.string = "Installed"
            installedLayer.frame = CGRect(x: frame.minX, y: frame.minY + 3, width: frame.width, height: 14)
            addCreateFormSublayer(installedLayer)
        } else {
            bundledAppInstallFrames.append((frame, backend))
        }
    }

    private func renderAppListWidget(name: String,
                                     items: [AppLauncherItem],
                                     frame: CGRect,
                                     rowHeight: CGFloat,
                                     drawsBackground: Bool = true,
                                     visibleIconKeys: inout Set<String>,
                                     visibleTextKeys: inout Set<String>) {
        if drawsBackground {
            let background = CALayer()
            background.frame = frame
            background.cornerRadius = 10
            background.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.86))
            background.borderWidth = 0.5
            background.borderColor = resolvedCGColor(NSColor.separatorColor.withAlphaComponent(0.55))
            addAppsSublayer(background)
        }

        var y = frame.maxY - 8 - rowHeight
        for (index, item) in items.enumerated() {
            let rowFrame = CGRect(x: frame.minX + 8, y: y, width: frame.width - 16, height: rowHeight)
            renderAppListRow(
                item: item,
                frame: rowFrame,
                showsSeparator: index > 0,
                visibleIconKeys: &visibleIconKeys,
                visibleTextKeys: &visibleTextKeys
            )
            y -= rowHeight
        }
    }

    private func renderAppListRow(
        item: AppLauncherItem,
        frame: CGRect,
        showsSeparator: Bool,
        visibleIconKeys: inout Set<String>,
        visibleTextKeys: inout Set<String>
    ) {
        let iconSize: CGFloat = 26
        let rowInset: CGFloat = 10
        appCardFrames.append((frame, item))

        if showsSeparator {
            let separator = CALayer()
            separator.backgroundColor = resolvedCGColor(.separatorColor)
            separator.frame = CGRect(
                x: frame.minX + rowInset + iconSize + 10,
                y: frame.maxY - 0.5,
                width: max(frame.width - rowInset - iconSize - 22, 1),
                height: 0.5
            )
            addAppsSublayer(separator)
        }

        let iconFrame = CGRect(
            x: frame.minX + rowInset,
            y: frame.midY - iconSize / 2,
            width: iconSize,
            height: iconSize
        )
        recordMatchedIcon(
            key: item.iconKey,
            frame: rootLayer.convert(iconFrame, from: activeAppsContentLayer),
            image: launcherIconImage(for: item),
            symbolName: launcherIconSymbolName(for: item),
            symbolColor: appIconTintColor(for: item.backend),
            title: item.displayName
        )
        visibleIconKeys.insert(item.iconKey)
        let hasRunningBadges = !runningEndpoints(for: item).isEmpty
        let textLeft = iconFrame.maxX + (hasRunningBadges ? 28 : 12)
        let textFrame = CGRect(
            x: textLeft,
            y: frame.midY - 10,
            width: max(frame.maxX - textLeft - 16, 1),
            height: 20
        )
        recordMatchedText(
            key: item.iconKey,
            frame: rootLayer.convert(textFrame, from: activeAppsContentLayer),
            title: item.displayName,
            fontSize: 13,
            weight: .medium,
            alignment: .left,
            isWrapped: false
        )
        visibleTextKeys.insert(item.iconKey)
        renderRunningBadges(
            for: item,
            leftX: iconFrame.maxX + 6,
            centerY: iconFrame.midY,
            pointSize: 8,
            circleDiameter: 14,
            gap: 2
        )
    }

    private func renderAppDragOverlayIfNeeded() {
        guard let drag = pendingAppDrag, drag.isDragging else { return }
        let appsPoint = appsLayer.convert(drag.currentPoint, from: rootLayer)
        let appsContentPoint = appsContentLayer(for: drag.item.scope).convert(
            drag.currentPoint,
            from: rootLayer
        )
        if let target = appDropTarget(at: appsContentPoint, for: drag.item),
           !appItem(drag.item, isIn: target),
           let highlightFrame = appDropHighlightFrame(for: target, scope: drag.item.scope) {
            let highlight = CALayer()
            highlight.frame = highlightFrame.insetBy(dx: -4, dy: -4)
            switch target {
            case .list:
                highlight.cornerRadius = 16
            case .pinned, .moreApps:
                highlight.cornerRadius = 4
            }
            highlight.borderWidth = 2
            highlight.borderColor = resolvedCGColor(.controlAccentColor)
            highlight.backgroundColor = resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.08))
            appsContentLayer(for: drag.item.scope).addSublayer(highlight)
        }

        let iconSize: CGFloat = 46
        let iconFrame = CGRect(x: appsPoint.x - iconSize / 2,
                               y: appsPoint.y - iconSize / 2 + 12,
                               width: iconSize,
                               height: iconSize)
        let icon = makeLauncherIconLayer(image: launcherIconImage(for: drag.item),
                                         symbolName: launcherIconSymbolName(for: drag.item),
                                         symbolColor: appIconTintColor(for: drag.item.backend),
                                         title: drag.item.displayName,
                                         iconSize: iconSize)
        icon.frame = iconFrame
        icon.opacity = 0.82
        appsOverlayLayer.addSublayer(icon)

        let title = makeTextLayer(size: 12, weight: .medium, color: .labelColor, alignment: .center)
        title.string = drag.item.displayName
        title.isWrapped = true
        title.truncationMode = .none
        title.frame = CGRect(x: appsPoint.x - 70, y: iconFrame.minY - 32, width: 140, height: 30)
        title.opacity = 0.82
        appsOverlayLayer.addSublayer(title)
    }

    private func appDropTarget(at appsPoint: CGPoint, for item: AppLauncherItem) -> AppDropTarget? {
        if let frame = overviewDropFrames.first(where: {
            $0.group == overviewGroupID(for: item) && $0.frame.contains(appsPoint)
        }) { return frame.target }
        if let frame = appListDropFrames.first(where: {
            $0.scope == item.scope && $0.frame.contains(appsPoint)
        }) {
            return .list(frame.listName)
        }
        if let frame = appUnlistedDropFrames.first(where: {
            $0.scope == item.scope && $0.frame.contains(appsPoint)
        }) {
            return frame.target
        }
        return nil
    }

    private func appItem(_ item: AppLauncherItem, isIn target: AppDropTarget) -> Bool {
        switch target {
        case .pinned:
            return isAppProminent(item)
        case .moreApps:
            return !isAppProminent(item)
        case .list(let name):
            return item.frontend.listName == name
        }
    }

    private func appDropHighlightFrame(for target: AppDropTarget,
                                       scope: AppLauncherScope) -> CGRect? {
        switch target {
        case .pinned, .moreApps:
            return appUnlistedDropFrames
                .filter { $0.scope == scope && $0.target == target }
                .reduce(nil) { partial, item in
                    guard let partial else { return item.frame }
                    return partial.union(item.frame)
                }
        case .list(let name):
            return appListDropFrames.first(where: {
                $0.scope == scope && $0.listName == name
            })?.frame
        }
    }

    private func makeLauncherIconLayer(image: CGImage?,
                                       symbolName: String?,
                                       symbolColor: NSColor = .controlAccentColor,
                                       title: String,
                                       iconSize: CGFloat) -> CALayer {
        let icon = CALayer()
        icon.cornerRadius = iconCornerRadius(for: iconSize)
        icon.masksToBounds = true
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        configureLauncherIconLayer(icon,
                                   image: image,
                                   symbolName: symbolName,
                                   symbolColor: symbolColor,
                                   title: title,
                                   iconSize: iconSize)
        return icon
    }

    private func configureLauncherIconLayer(_ icon: CALayer,
                                            image: CGImage?,
                                            symbolName: String?,
                                            symbolColor: NSColor = .controlAccentColor,
                                            title: String,
                                            iconSize: CGFloat) {
        icon.cornerRadius = iconCornerRadius(for: iconSize)
        if let image {
            if icon.contents.map({ ($0 as AnyObject) !== image }) ?? true {
                icon.contents = image
            }
            icon.backgroundColor = resolvedCGColor(.clear)
        } else if let symbolName,
                  !symbolName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  let cgImage = symbolAppIconCGImage(named: symbolName, color: symbolColor) {
            icon.contents = cgImage
            icon.backgroundColor = resolvedCGColor(.clear)
        } else {
            icon.contents = letterIconCGImage(for: title)
            icon.backgroundColor = resolvedCGColor(.clear)
        }
    }

    private func letterIconCGImage(for title: String) -> CGImage? {
        let imageSize: CGFloat = 96
        let image = NSImage(size: NSSize(width: imageSize, height: imageSize))
        image.lockFocus()
        withEffectiveAppearance {
            NSColor.controlAccentColor.withAlphaComponent(0.18).setFill()
            NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: imageSize, height: imageSize),
                         xRadius: 22,
                         yRadius: 22).fill()

            let font = NSFont.systemFont(ofSize: 40, weight: .semibold)
            let paragraph = NSMutableParagraphStyle()
            paragraph.alignment = .center
            let attributes: [NSAttributedString.Key: Any] = [
                .font: font,
                .foregroundColor: NSColor.controlAccentColor,
                .paragraphStyle: paragraph
            ]
            let text = appInitial(for: title) as NSString
            let textHeight = font.ascender - font.descender
            let textRect = NSRect(x: 0,
                                  y: floor((imageSize - textHeight) / 2) - 2,
                                  width: imageSize,
                                  height: textHeight + 6)
            text.draw(in: textRect, withAttributes: attributes)
        }
        image.unlockFocus()
        return cgImage(for: image)
    }

    private func symbolAppIconCGImage(named symbolName: String, color: NSColor = .controlAccentColor) -> CGImage? {
        var cacheURL: URL?
        if symbolName == "apple.terminal", let rgb = resolvedColor(color).usingColorSpace(.sRGB) {
            var components = URLComponents()
            components.scheme = "outershell-symbol"
            components.host = "local"
            components.path = "/terminal-96-v1"
            components.queryItems = [
                URLQueryItem(name: "os", value: ProcessInfo.processInfo.operatingSystemVersionString),
                URLQueryItem(name: "appearance", value: appearance?.name.rawValue ?? NSAppearance.currentDrawing().name.rawValue),
                URLQueryItem(name: "color", value: "\(rgb.redComponent),\(rgb.greenComponent),\(rgb.blueComponent),\(rgb.alphaComponent)")
            ]
            cacheURL = components.url
            if let cacheURL {
                if let cached = terminalSymbolImages[cacheURL] { return cached }
                if let cached = endpointIconDiskCache?.image(for: cacheURL) {
                    terminalSymbolImages[cacheURL] = cached
                    return cached
                }
            }
        }
        let imageSize: CGFloat = 96
        let image = NSImage(size: NSSize(width: imageSize, height: imageSize))
        image.lockFocus()
        withEffectiveAppearance {
            if let symbol = NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)?
                .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: 84, weight: .regular)
                    .applying(NSImage.SymbolConfiguration(hierarchicalColor: color))) {
                let drawSize = symbol.size
                symbol.draw(in: NSRect(x: floor((imageSize - drawSize.width) / 2),
                                       y: floor((imageSize - drawSize.height) / 2),
                                       width: drawSize.width,
                                       height: drawSize.height),
                            from: .zero,
                            operation: .sourceOver,
                            fraction: 1)
            }
        }
        image.unlockFocus()
        guard let rendered = cgImage(for: image) else { return nil }
        if let cacheURL {
            let encoded = NSMutableData()
            if let destination = CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil) {
                CGImageDestinationAddImage(destination, rendered, nil)
                if CGImageDestinationFinalize(destination),
                   let cached = endpointIconDiskCache?.storeImage(encoded as Data, for: cacheURL) {
                    terminalSymbolImages[cacheURL] = cached
                    return cached
                }
            }
            terminalSymbolImages[cacheURL] = rendered
        }
        return rendered
    }

    private func iconCornerRadius(for iconSize: CGFloat) -> CGFloat {
        iconSize >= 44 ? 13 : 5
    }

    private func recordMatchedIcon(key: String,
                                   frame: CGRect,
                                   image: CGImage?,
                                   symbolName: String?,
                                   symbolColor: NSColor = .controlAccentColor,
                                   title: String) {
        let layer: CALayer
        if let existingLayer = iconMatchLayers[key] {
            layer = existingLayer
            configureLauncherIconLayer(layer,
                                       image: image,
                                       symbolName: symbolName,
                                       symbolColor: symbolColor,
                                       title: title,
                                       iconSize: frame.width)
        } else {
            layer = makeLauncherIconLayer(image: image,
                                          symbolName: symbolName,
                                          symbolColor: symbolColor,
                                          title: title,
                                          iconSize: frame.width)
        }
        if layer.superlayer == nil {
            iconTransitionLayer.addSublayer(layer)
        }
        layer.frame = frame
        layer.isHidden = false
        layer.opacity = 1
        iconMatchStates[key] = IconMatchState(frame: frame,
                                              image: image,
                                              symbolName: symbolName,
                                              title: title)
        iconMatchLayers[key] = layer
    }

    private func recordMatchedText(key: String,
                                   frame: CGRect,
                                   title: String,
                                   fontSize: CGFloat,
                                   weight: NSFont.Weight,
                                   alignment: CATextLayerAlignmentMode,
                                   isWrapped: Bool,
                                   italic: Bool = false) {
        let existingLayer = textMatchLayers[key]
        let layer = existingLayer ?? makeTextLayer(size: fontSize,
                                                   weight: weight,
                                                   color: .labelColor,
                                                   alignment: alignment,
                                                   italic: italic)
        if layer.superlayer == nil {
            iconTransitionLayer.addSublayer(layer)
        }
        configureTextLayer(layer,
                           title: title,
                           fontSize: fontSize,
                           weight: weight,
                           color: .labelColor,
                           alignment: alignment,
                           isWrapped: isWrapped,
                           italic: italic)
        layer.frame = frame
        layer.isHidden = false
        layer.opacity = 1
        textMatchStates[key] = TextMatchState(frame: frame,
                                              title: title,
                                              fontSize: fontSize,
                                              weight: weight,
                                              alignment: alignment,
                                              isWrapped: isWrapped)
        textMatchLayers[key] = layer
    }

    private func hideUnrenderedMatchedLayers(visibleIconKeys: Set<String>, visibleTextKeys: Set<String>) {
        for (key, layer) in iconMatchLayers {
            if !visibleIconKeys.contains(key) {
                layer.isHidden = true
            }
        }
        for (key, layer) in textMatchLayers {
            if !visibleTextKeys.contains(key) {
                layer.isHidden = true
            }
        }
    }

    private func hideAllMatchedLayers() {
        hideUnrenderedMatchedLayers(visibleIconKeys: [], visibleTextKeys: [])
        iconMatchStates.removeAll()
        textMatchStates.removeAll()
    }

    private func installsBundledPlaceholderAsSystemOnly(_ backend: BackendRecord) -> Bool {
        backend.isBundledPlaceholder && backend.serviceScope == "system" && (backend.supportsRoot ?? false)
    }

    private var isDirectRootSession: Bool {
        backends.contains { $0.isBackendsSelf && $0.serviceScope == "system" }
    }

    private func renderInstallPromptIfNeeded(width: CGFloat, height: CGFloat) {
        installOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        guard let backend = pendingInstallBackend else {
            installOverlayLayer.isHidden = true
            installPanelFrame = .zero
            installConfirmFrame = .zero
            installRootConfirmFrame = .zero
            installCancelFrame = .zero
            return
        }

        installOverlayLayer.isHidden = false
        installOverlayLayer.frame = CGRect(x: 0, y: 0, width: width, height: height)
        installOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.18))

        let systemOnlyPlaceholder = installsBundledPlaceholderAsSystemOnly(backend)
        let hasRootChoice = !systemOnlyPlaceholder && (backend.supportsRoot ?? false) && !(backend.rootOnly ?? false)
        let isRootOnly = systemOnlyPlaceholder || (backend.rootOnly ?? false)
        let preferredPanelWidth: CGFloat = hasRootChoice ? 430 : 360
        let panelWidth = min(max(width - 48, 280), preferredPanelWidth)
        let stacksButtons = hasRootChoice && panelWidth < 398
        let panelHeight: CGFloat = stacksButtons ? 208 : 160
        let panelFrame = CGRect(x: floor((width - panelWidth) / 2),
                                y: floor((height - panelHeight) / 2),
                                width: panelWidth,
                                height: panelHeight)
        installPanelFrame = panelFrame

        let panel = CALayer()
        panel.frame = panelFrame
        panel.cornerRadius = 8
        panel.borderWidth = 1
        panel.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        panel.borderColor = resolvedCGColor(.separatorColor)
        installOverlayLayer.addSublayer(panel)

        let iconSize: CGFloat = 42
        let icon = CALayer()
        icon.frame = CGRect(x: 18, y: panelHeight - iconSize - 22, width: iconSize, height: iconSize)
        icon.cornerRadius = iconCornerRadius(for: iconSize)
        icon.masksToBounds = true
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        configureLauncherIconLayer(icon,
                                   image: nil,
                                   symbolName: backend.iconSymbolName,
                                   title: backend.displayName,
                                   iconSize: iconSize)
        panel.addSublayer(icon)

        let title = makeTextLayer(size: 15, weight: .semibold, color: .labelColor)
        title.string = "Install \(backend.displayName)?"
        title.frame = CGRect(x: 72, y: panelHeight - 42, width: panelWidth - 90, height: 20)
        panel.addSublayer(title)

        let message = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        message.string = "Outer Shell will download this app."
        message.isWrapped = true
        message.frame = CGRect(x: 72, y: panelHeight - 74, width: panelWidth - 90, height: 20)
        panel.addSublayer(message)

        let buttonY: CGFloat = 18
        let rootButtonWidth: CGFloat = hasRootChoice ? 156 : 112
        let installButtonWidth: CGFloat = hasRootChoice ? 118 : (isRootOnly ? 112 : 118)
        let cancelButtonWidth: CGFloat = 70
        let rootLocalFrame: CGRect
        let installLocalFrame: CGRect
        let cancelLocalFrame: CGRect
        if stacksButtons {
            let buttonWidth = panelWidth - 36
            rootLocalFrame = CGRect(x: 18, y: buttonY, width: buttonWidth, height: 30)
            installLocalFrame = CGRect(x: 18, y: buttonY + 38, width: buttonWidth, height: 30)
            cancelLocalFrame = CGRect(x: 18, y: buttonY + 76, width: buttonWidth, height: 30)
        } else {
            rootLocalFrame = (hasRootChoice || isRootOnly)
                ? CGRect(x: panelWidth - rootButtonWidth - 18,
                         y: buttonY,
                         width: rootButtonWidth,
                         height: 30)
                : .zero
            installLocalFrame = isRootOnly
                ? .zero
                : CGRect(x: (hasRootChoice ? rootLocalFrame.minX : panelWidth) - installButtonWidth - 8,
                         y: buttonY,
                         width: installButtonWidth,
                         height: 30)
            let cancelAnchorX = isRootOnly ? rootLocalFrame.minX : installLocalFrame.minX
            cancelLocalFrame = CGRect(x: cancelAnchorX - cancelButtonWidth - 8,
                                      y: buttonY,
                                      width: cancelButtonWidth,
                                      height: 30)
        }
        installConfirmFrame = installLocalFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        installRootConfirmFrame = (hasRootChoice || isRootOnly) ? rootLocalFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY) : .zero
        installCancelFrame = cancelLocalFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)

        let cancelButton = makeButtonLayer(title: "Cancel", emphasized: false)
        cancelButton.frame = cancelLocalFrame
        panel.addSublayer(cancelButton)

        if !isRootOnly {
            let installButton = makeButtonLayer(title: "Enable for user", emphasized: true)
            installButton.frame = installLocalFrame
            panel.addSublayer(installButton)
        }
        if hasRootChoice || isRootOnly {
            let rootButtonTitle = systemOnlyPlaceholder ? "Enable" : (hasRootChoice ? "Enable for user and root" : "Enable for root")
            let rootButton = makeButtonLayer(title: rootButtonTitle, emphasized: true)
            rootButton.frame = rootLocalFrame
            panel.addSublayer(rootButton)
        }
    }

    private func renderPasswordPromptIfNeeded(width: CGFloat, height: CGFloat) {
        passwordOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        guard let action = pendingPasswordAction else {
            passwordOverlayLayer.isHidden = true
            passwordPanelFrame = .zero
            passwordFieldFrame = .zero
            passwordTextFrame = .zero
            passwordSubmitFrame = .zero
            passwordCancelFrame = .zero
            return
        }

        passwordOverlayLayer.isHidden = false
        passwordOverlayLayer.frame = CGRect(x: 0, y: 0, width: width, height: height)
        passwordOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.22))

        let panelWidth = min(max(width - 48, 280), 390)
        let panelHeight: CGFloat = 178
        let panelFrame = CGRect(x: floor((width - panelWidth) / 2),
                                y: floor((height - panelHeight) / 2),
                                width: panelWidth,
                                height: panelHeight)
        passwordPanelFrame = panelFrame

        let panel = CALayer()
        panel.frame = panelFrame
        panel.cornerRadius = 8
        panel.borderWidth = 1
        panel.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        panel.borderColor = resolvedCGColor(.separatorColor)
        passwordOverlayLayer.addSublayer(panel)

        let title = makeTextLayer(size: 15, weight: .semibold, color: .labelColor)
        title.string = "Administrator Password"
        title.frame = CGRect(x: 18, y: panelHeight - 38, width: panelWidth - 36, height: 20)
        panel.addSublayer(title)

        let message = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        message.string = "\(action.displayName): \(sudoPasswordMessage)"
        message.frame = CGRect(x: 18, y: panelHeight - 62, width: panelWidth - 36, height: 17)
        panel.addSublayer(message)

        let field = CALayer()
        let localFieldFrame = CGRect(x: 18, y: 70, width: panelWidth - 36, height: 32)
        field.frame = localFieldFrame
        field.masksToBounds = true
        field.cornerRadius = 5
        field.borderWidth = passwordInputController.isFocused ? 1.5 : 1
        field.backgroundColor = resolvedCGColor(.textBackgroundColor)
        field.borderColor = passwordInputController.isFocused ? resolvedCGColor(.keyboardFocusIndicatorColor) : resolvedCGColor(.separatorColor)
        panel.addSublayer(field)
        passwordFieldFrame = localFieldFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        passwordTextFrame = CGRect(x: passwordFieldFrame.minX + 10,
                                   y: passwordFieldFrame.minY + 8,
                                   width: max(localFieldFrame.width - 20, 1),
                                   height: 18)

        let bulletString = String(repeating: "\u{2022}", count: sudoPasswordInput.count)
        if passwordInputController.isFocused,
           let selectionRange = passwordInputController.selectionRange,
           !bulletString.isEmpty {
            let line = makePasswordFieldLine(for: bulletString)
            let offsets = selectionOffsets(line: line,
                                           text: bulletString,
                                           range: selectionRange,
                                           maxWidth: passwordTextFrame.width)
            let selectionWidth = max(0, offsets.end - offsets.start)
            if selectionWidth > 0.5 {
                let selection = CALayer()
                selection.frame = CGRect(x: 10 + offsets.start,
                                         y: 7,
                                         width: selectionWidth,
                                         height: 18)
                selection.backgroundColor = resolvedCGColor(windowIsActive ? .selectedTextBackgroundColor : .unemphasizedSelectedTextBackgroundColor)
                field.addSublayer(selection)
            }
        }

        let bullets = makeTextLayer(size: 14, weight: .regular, color: .labelColor)
        bullets.string = bulletString
        bullets.frame = CGRect(x: 10, y: 8, width: max(localFieldFrame.width - 20, 1), height: 18)
        field.addSublayer(bullets)
        if let cursorFrame = passwordFieldCursorRect(), windowIsActive {
            addBlinkingTextCaret(to: field,
                                 frame: cursorFrame.offsetBy(dx: -passwordFieldFrame.minX,
                                                             dy: -passwordFieldFrame.minY))
        }

        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        let submit = makeButtonLayer(title: "Continue", emphasized: true)
        let buttonY: CGFloat = 20
        let buttonWidth: CGFloat = 86
        let submitLocal = CGRect(x: panelWidth - 18 - buttonWidth, y: buttonY, width: buttonWidth, height: 30)
        let cancelLocal = CGRect(x: submitLocal.minX - 10 - 76, y: buttonY, width: 76, height: 30)
        cancel.frame = cancelLocal
        submit.frame = submitLocal
        panel.addSublayer(cancel)
        panel.addSublayer(submit)
        passwordCancelFrame = cancelLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        passwordSubmitFrame = submitLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        sendPasswordFieldTextInputGeometryUpdate()
    }

    private func renderUpdatePromptIfNeeded(width: CGFloat, height: CGFloat) {
        updateOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        guard let update = pendingOuterShellUpdate else {
            updateOverlayLayer.isHidden = true
            updatePanelFrame = .zero
            updateCancelFrame = .zero
            updateConfirmFrame = .zero
            return
        }

        updateOverlayLayer.isHidden = false
        updateOverlayLayer.frame = CGRect(x: 0, y: 0, width: width, height: height)
        updateOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.22))

        let panelWidth = min(max(width - 48, 320), 430)
        let panelHeight: CGFloat = 166
        let panelFrame = CGRect(x: floor((width - panelWidth) / 2),
                                y: floor((height - panelHeight) / 2),
                                width: panelWidth,
                                height: panelHeight)
        updatePanelFrame = panelFrame

        let panel = CALayer()
        panel.frame = panelFrame
        panel.cornerRadius = 8
        panel.borderWidth = 1
        panel.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        panel.borderColor = resolvedCGColor(.separatorColor)
        updateOverlayLayer.addSublayer(panel)

        let title = makeTextLayer(size: 15, weight: .semibold, color: .labelColor)
        title.string = "Outer Shell Update Available"
        title.frame = CGRect(x: 18, y: panelHeight - 38, width: panelWidth - 36, height: 20)
        panel.addSublayer(title)

        let available = update.availableVersion.isEmpty ? "the latest version" : "Outer Shell \(update.availableVersion)"
        let installed = update.installedVersion.isEmpty ? "unknown" : update.installedVersion
        let message = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        message.string = "\(available) is available. Installed version: \(installed)."
        message.isWrapped = true
        message.truncationMode = .none
        message.frame = CGRect(x: 18, y: 64, width: panelWidth - 36, height: 46)
        panel.addSublayer(message)

        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        let updateButton = makeButtonLayer(title: isPerformingAction ? "Updating..." : "Update", emphasized: true)
        let buttonY: CGFloat = 20
        let buttonWidth: CGFloat = 86
        let updateLocal = CGRect(x: panelWidth - 18 - buttonWidth, y: buttonY, width: buttonWidth, height: 30)
        let cancelLocal = CGRect(x: updateLocal.minX - 10 - 76, y: buttonY, width: 76, height: 30)
        cancel.frame = cancelLocal
        updateButton.frame = updateLocal
        panel.addSublayer(cancel)
        panel.addSublayer(updateButton)
        updateCancelFrame = cancelLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        updateConfirmFrame = updateLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
    }

    private func renderAboutPromptIfNeeded(width: CGFloat, height: CGFloat) {
        aboutOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        guard let backend = pendingAboutBackend else {
            aboutOverlayLayer.isHidden = true
            aboutSelectionLayer.removeFromSuperlayer()
            aboutSelectionLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
            aboutPanelFrame = .zero
            aboutDoneFrame = .zero
            aboutTextFrame = .zero
            renderedAboutText = ""
            aboutSelectionRange = nil
            return
        }

        aboutOverlayLayer.isHidden = false
        aboutOverlayLayer.frame = CGRect(x: 0, y: 0, width: width, height: height)
        aboutOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.22))

        let panelWidth = min(max(width - 48, 340), 500)
        let panelHeight: CGFloat = 244
        let panelFrame = CGRect(x: floor((width - panelWidth) / 2),
                                y: floor((height - panelHeight) / 2),
                                width: panelWidth,
                                height: panelHeight)
        aboutPanelFrame = panelFrame

        let panel = CALayer()
        panel.frame = panelFrame
        panel.cornerRadius = 8
        panel.borderWidth = 1
        panel.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        panel.borderColor = resolvedCGColor(.separatorColor)
        aboutOverlayLayer.addSublayer(panel)

        let title = makeTextLayer(size: 15, weight: .semibold, color: .labelColor)
        title.string = "About Outer Shell"
        title.frame = CGRect(x: 18, y: panelHeight - 38, width: panelWidth - 36, height: 20)
        panel.addSublayer(title)

        renderedAboutText = aboutDialogText(for: backend)
        let textLocalFrame = CGRect(x: 18, y: 58, width: panelWidth - 36, height: 132)
        aboutTextFrame = textLocalFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)

        let textBackground = CALayer()
        textBackground.frame = textLocalFrame
        textBackground.cornerRadius = 6
        textBackground.borderWidth = 1
        textBackground.backgroundColor = resolvedCGColor(.textBackgroundColor)
        textBackground.borderColor = resolvedCGColor(.separatorColor)
        panel.addSublayer(textBackground)

        aboutSelectionLayer.frame = textBackground.bounds
        textBackground.addSublayer(aboutSelectionLayer)
        updateAboutSelectionLayers()
        renderAboutTextLines(in: textBackground)

        let done = makeButtonLayer(title: "Done", emphasized: true)
        let doneLocal = CGRect(x: panelWidth - 18 - 76, y: 18, width: 76, height: 30)
        done.frame = doneLocal
        panel.addSublayer(done)
        aboutDoneFrame = doneLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
    }

    private func renderAboutTextLines(in layer: CALayer) {
        let font = aboutTextFont()
        let lineHeight = aboutTextLineHeight()
        for fragment in aboutLineFragments() {
            let text = makeTextLayer(size: 12, weight: .regular, color: .labelColor, monospaced: true)
            text.string = fragment.text
            text.frame = CGRect(x: 10,
                                y: fragment.y,
                                width: max(layer.bounds.width - 20, 1),
                                height: lineHeight)
            text.font = font
            layer.addSublayer(text)
        }
    }

    private func updateAboutSelectionLayers() {
        withoutImplicitAnimations {
            aboutSelectionLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
            renderAboutSelection(in: aboutSelectionLayer)
        }
    }

    private func renderAboutSelection(in layer: CALayer) {
        guard let selectionRange = normalizedAboutSelectionRange(aboutSelectionRange) else { return }
        let color = resolvedCGColor(windowIsActive ? NSColor.selectedTextBackgroundColor : NSColor.unemphasizedSelectedTextBackgroundColor)
        for fragment in aboutLineFragments() {
            let lower = max(selectionRange.location, fragment.range.location)
            let upper = min(selectionRange.location + selectionRange.length, fragment.range.location + fragment.range.length)
            guard upper > lower else { continue }
            let line = aboutTextLine(for: fragment.text)
            var startSecondary: CGFloat = 0
            var endSecondary: CGFloat = 0
            let start = CGFloat(CTLineGetOffsetForStringIndex(line, lower - fragment.range.location, &startSecondary))
            let end = CGFloat(CTLineGetOffsetForStringIndex(line, upper - fragment.range.location, &endSecondary))
            let x = min(start, end)
            let width = max(abs(end - start), 1)
            let selection = CALayer()
            selection.frame = CGRect(x: 10 + x,
                                     y: fragment.y + 1,
                                     width: width,
                                     height: aboutTextLineHeight() - 2)
            selection.backgroundColor = color
            selection.cornerRadius = 2
            layer.addSublayer(selection)
        }
    }

    private func renderFilePickerIfNeeded(width: CGFloat, height: CGFloat) {
        filePickerOverlayLayer.removeFromSuperlayer()
        createLayer.addSublayer(filePickerOverlayLayer)
        filePickerOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        filePickerEntryFrames.removeAll()
        filePickerBreadcrumbSegmentFrames.removeAll()
        guard let picker = pendingFilePicker else {
            filePickerOverlayLayer.isHidden = true
            filePickerPanelFrame = .zero
            filePickerBreadcrumbFrame = .zero
            filePickerSelectedIndex = nil
            resetFilePickerTypeahead()
            filePickerSaveFrame = .zero
            filePickerCancelFrame = .zero
            filePickerListFrame = .zero
            filePickerContentHeight = 0
            resetFilePickerRowLayers()
            filePickerScrollbarController?.updateLayout(metrics: filePickerScrollbarMetrics())
            return
        }
        clampFilePickerSelection()

        filePickerOverlayLayer.isHidden = false
        filePickerOverlayLayer.frame = CGRect(x: 0, y: 0, width: width, height: height)
        filePickerOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.18))

        let panelWidth = min(max(width - 48, 460), 760)
        let panelHeight = min(max(height - 48, 340), 540)
        let panelFrame = CGRect(x: floor((width - panelWidth) / 2),
                                y: floor((height - panelHeight) / 2),
                                width: panelWidth,
                                height: panelHeight)
        filePickerPanelFrame = panelFrame

        let isChooseFile = picker.mode == .chooseFile
        let bottomControlsHeight: CGFloat = 72
        let panel = CALayer()
        panel.frame = panelFrame
        panel.cornerRadius = 8
        panel.borderWidth = 1
        panel.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        panel.borderColor = resolvedCGColor(.separatorColor)
        filePickerOverlayLayer.addSublayer(panel)

        let title = makeTextLayer(size: 15, weight: .semibold, color: .labelColor)
        if isChooseFile {
            title.string = "Choose Icon"
        } else if picker.targetFieldKey == "nativeProjectRoot" {
            title.string = "Choose Project Location"
        } else {
            title.string = "Choose Folder"
        }
        title.frame = CGRect(x: 18, y: panelHeight - 38, width: panelWidth - 36, height: 20)
        panel.addSublayer(title)

        let breadcrumbFrame = CGRect(x: 18, y: panelHeight - 78, width: panelWidth - 36, height: FilePickerBreadcrumbBar.height)
        filePickerBreadcrumbFrame = breadcrumbFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        let breadcrumb = CALayer()
        breadcrumb.frame = breadcrumbFrame
        breadcrumb.masksToBounds = true
        FilePickerBreadcrumbBar.render(path: picker.directory,
                                       in: breadcrumb,
                                       segmentFrames: &filePickerBreadcrumbSegmentFrames,
                                       appearance: appearance ?? NSAppearance.currentDrawing())
        panel.addSublayer(breadcrumb)

        let listFrame = CGRect(x: 18, y: bottomControlsHeight, width: panelWidth - 36, height: max(breadcrumbFrame.minY - bottomControlsHeight - 10, 80))
        filePickerListFrame = listFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        filePickerListLayer.frame = listFrame
        filePickerListLayer.cornerRadius = 6
        filePickerListLayer.borderWidth = 1
        filePickerListLayer.borderColor = resolvedCGColor(.separatorColor)
        filePickerListLayer.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.35))
        filePickerListLayer.masksToBounds = true
        panel.addSublayer(filePickerListLayer)

        if filePickerRowsContentLayer.superlayer !== filePickerListLayer {
            filePickerListLayer.addSublayer(filePickerRowsContentLayer)
        }
        if filePickerStatusLayer.superlayer !== filePickerListLayer {
            filePickerListLayer.addSublayer(filePickerStatusLayer)
        }
        filePickerStatusLayer.frame = filePickerListLayer.bounds
        filePickerStatusLayer.sublayers?.forEach { $0.removeFromSuperlayer() }

        let allEntries = picker.entries
        filePickerContentHeight = CGFloat(allEntries.count) * filePickerRowHeight
        let maxPickerScroll = max(filePickerContentHeight - listFrame.height, 0)
        filePickerScroll = min(max(filePickerScroll, 0), maxPickerScroll)

        if picker.isLoading {
            resetFilePickerRowLayers()
            let loading = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor, alignment: .center)
            loading.string = "Loading..."
            loading.frame = CGRect(x: 0, y: max((listFrame.height - 18) / 2, 0), width: listFrame.width, height: 18)
            filePickerStatusLayer.addSublayer(loading)
        } else if !picker.error.isEmpty {
            resetFilePickerRowLayers()
            let message = makeTextLayer(size: 12,
                                        weight: .regular,
                                        color: .systemRed,
                                        alignment: .center)
            message.string = picker.error
            message.frame = CGRect(x: 10, y: max((listFrame.height - 18) / 2, 0), width: listFrame.width - 20, height: 18)
            filePickerStatusLayer.addSublayer(message)
        } else {
            updateFilePickerVisibleRows(rebuild: true)
        }
        updateFilePickerScrollbarLayout()
        let saveLocal = CGRect(x: panelWidth - 18 - 66, y: 28, width: 66, height: 30)
        let cancelLocal = CGRect(x: saveLocal.minX - 78, y: 28, width: 70, height: 30)
        filePickerSaveFrame = saveLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        filePickerCancelFrame = cancelLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)

        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        cancel.frame = cancelLocal
        panel.addSublayer(cancel)
        let save = makeButtonLayer(title: "Choose", emphasized: true)
        save.frame = saveLocal
        panel.addSublayer(save)
        if let proposed = picker.entries.first(where: \.willCreate) {
            let note = makeTextLayer(size: 11, weight: .regular, color: .secondaryLabelColor)
            note.string = "\(proposed.name) will be created if chosen."
            note.frame = CGRect(x: 18,
                                y: 34,
                                width: max(cancelLocal.minX - 30, 1),
                                height: 16)
            panel.addSublayer(note)
        }
        sendCreateFieldTextInputGeometryUpdate()
    }

    private func makeFilePickerRowLayer() -> CALayer {
        let row = CALayer()

        let icon = CALayer()
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        row.addSublayer(icon)

        let name = makeTextLayer(size: 12, weight: .regular, color: .labelColor)
        row.addSublayer(name)
        let detail = makeTextLayer(size: 11, weight: .regular, color: .secondaryLabelColor, alignment: .right)
        row.addSublayer(detail)
        return row
    }

    private func configureFilePickerRowLayer(_ row: CALayer,
                                             entry: FilePickerEntryRecord,
                                             index: Int,
                                             frame: CGRect) {
        row.frame = frame
        let isSelected = filePickerSelectedIndex == index
        row.backgroundColor = isSelected ? resolvedCGColor(.selectedContentBackgroundColor) : nil

        if row.sublayers?.count != 3 {
            row.sublayers?.forEach { $0.removeFromSuperlayer() }
            let icon = CALayer()
            icon.contentsGravity = .resizeAspect
            icon.contentsScale = 2
            row.addSublayer(icon)
            row.addSublayer(makeTextLayer(size: 12, weight: .regular, color: .labelColor))
            row.addSublayer(makeTextLayer(size: 11,
                                          weight: .regular,
                                          color: .secondaryLabelColor,
                                          alignment: .right))
        }

        let icon = row.sublayers?[0]
        icon?.frame = CGRect(x: 22, y: 6, width: 16, height: 16)
        icon?.contents = symbolCGImage(named: entry.isDirectory ? "folder" : "doc",
                                       pointSize: 15,
                                       color: isSelected ? .white : .secondaryLabelColor)

        if let name = row.sublayers?[1] as? CATextLayer {
            name.string = entry.name
            name.foregroundColor = resolvedCGColor(isSelected ? .white : .labelColor)
            name.frame = CGRect(x: 58,
                                y: 6,
                                width: max(frame.width - (entry.willCreate ? 170 : 70), 1),
                                height: 16)
        }
        if let detail = row.sublayers?[2] as? CATextLayer {
            detail.string = entry.willCreate ? "New Folder" : ""
            detail.foregroundColor = resolvedCGColor(isSelected ? .white : .secondaryLabelColor)
            detail.frame = CGRect(x: max(frame.width - 108, 58),
                                  y: 6,
                                  width: min(92, max(frame.width - 120, 1)),
                                  height: 16)
        }
    }

    private func recycleFilePickerRowLayer(_ layer: CALayer) {
        layer.removeFromSuperlayer()
        filePickerReusableRowLayers.append(layer)
    }

    private func resetFilePickerRowLayers() {
        withoutImplicitAnimations {
            filePickerRowsContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
            filePickerVisibleRowLayers.removeAll()
            filePickerReusableRowLayers.removeAll()
            filePickerEntryFrames.removeAll()
            filePickerRowsContentLayer.frame = CGRect(origin: .zero,
                                                      size: CGSize(width: filePickerListLayer.bounds.width,
                                                                   height: 0))
        }
    }

    private func currentFilePickerEntries() -> [FilePickerEntryRecord] {
        guard let picker = pendingFilePicker,
              !picker.isLoading else { return [] }
        if !picker.error.isEmpty {
            return []
        }
        return picker.entries
    }

    fileprivate func setFilePickerScroll(_ value: CGFloat) {
        let maxPickerScroll = max(filePickerContentHeight - filePickerListFrame.height, 0)
        let clamped = min(max(value, 0), maxPickerScroll)
        guard abs(clamped - filePickerScroll) > 0.001 else {
            updateFilePickerScrollbarLayout()
            return
        }
        filePickerScroll = clamped
        updateFilePickerVisibleRows(rebuild: false)
    }

    private func updateFilePickerVisibleRows(rebuild: Bool) {
        guard pendingFilePicker != nil else {
            resetFilePickerRowLayers()
            return
        }

        let entries = currentFilePickerEntries()
        let viewportHeight = max(filePickerListLayer.bounds.height, 0)
        let viewportWidth = max(filePickerListLayer.bounds.width, 1)
        filePickerContentHeight = CGFloat(entries.count) * filePickerRowHeight
        let maxPickerScroll = max(filePickerContentHeight - viewportHeight, 0)
        filePickerScroll = min(max(filePickerScroll, 0), maxPickerScroll)

        withoutImplicitAnimations {
            filePickerEntryFrames.removeAll()

            let contentY = viewportHeight - filePickerContentHeight + filePickerScroll
            filePickerRowsContentLayer.frame = CGRect(x: 0,
                                                      y: contentY,
                                                      width: viewportWidth,
                                                      height: max(filePickerContentHeight, 0))

            guard !entries.isEmpty, viewportHeight > 0 else {
                filePickerRowsContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
                filePickerVisibleRowLayers.removeAll()
                filePickerReusableRowLayers.removeAll()
                filePickerEntryFrames.removeAll()
                filePickerRowsContentLayer.frame = CGRect(origin: .zero,
                                                          size: CGSize(width: viewportWidth, height: 0))
                return
            }

            let visibleStart = max(Int(floor(filePickerScroll / filePickerRowHeight)), 0)
            let visibleEnd = min(entries.count, visibleStart + Int(ceil(viewportHeight / filePickerRowHeight)) + 2)
            let visibleRange = visibleStart..<visibleEnd

            let staleIndices = filePickerVisibleRowLayers.keys.filter { !visibleRange.contains($0) }
            for index in staleIndices {
                if let layer = filePickerVisibleRowLayers[index] {
                    recycleFilePickerRowLayer(layer)
                }
                filePickerVisibleRowLayers.removeValue(forKey: index)
            }

            for index in visibleRange {
                let entry = entries[index]
                let rowFrame = CGRect(x: 0,
                                      y: filePickerContentHeight - CGFloat(index + 1) * filePickerRowHeight,
                                      width: viewportWidth,
                                      height: filePickerRowHeight)
                let row: CALayer
                if let existing = filePickerVisibleRowLayers[index] {
                    row = existing
                } else if let reusable = filePickerReusableRowLayers.popLast() {
                    row = reusable
                    filePickerRowsContentLayer.addSublayer(row)
                    filePickerVisibleRowLayers[index] = row
                } else {
                    row = makeFilePickerRowLayer()
                    filePickerRowsContentLayer.addSublayer(row)
                    filePickerVisibleRowLayers[index] = row
                }
                configureFilePickerRowLayer(row, entry: entry, index: index, frame: rowFrame)

                let visibleFrame = rowFrame.offsetBy(dx: filePickerRowsContentLayer.frame.minX,
                                                     dy: filePickerRowsContentLayer.frame.minY)
                filePickerEntryFrames.append((visibleFrame.offsetBy(dx: filePickerListFrame.minX,
                                                                    dy: filePickerListFrame.minY),
                                              entry,
                                              index))
            }
        }

        updateFilePickerScrollbarLayout()
    }

    private func filePickerScrollbarMetrics() -> ScrollbarController<FilePickerScrollbarDelegate>.Metrics {
        ScrollbarController.Metrics(viewportSize: filePickerListLayer.bounds.size,
                                    contentHeight: filePickerContentHeight,
                                    scrollOffset: filePickerScroll)
    }

    private func updateFilePickerScrollbarLayout() {
        filePickerScrollbarController?.updateLayout(metrics: filePickerScrollbarMetrics())
    }

    private func renderLogHeader() {
        logHeaderLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        let backend = selectedBackend()
        logSelectorFrame = .zero
        logDismissFrame = CGRect(x: max(logHeaderLayer.bounds.width - horizontalInset - 24, horizontalInset),
                                 y: 32,
                                 width: 24,
                                 height: 24)
        let dismiss = makeSymbolButtonLayer(symbolName: "x.circle", accessibilityTitle: "Dismiss logs")
        dismiss.frame = logDismissFrame
        logHeaderLayer.addSublayer(dismiss)

        if let backend,
           backend.logFiles.count > 1,
           logHeaderLayer.bounds.width > horizontalInset * 2 + 80 {
            let availableWidth = max(logHeaderLayer.bounds.width - horizontalInset * 2, 1)
            let selectorMaxX = logDismissFrame.minX - 8
            let selectorAvailableWidth = max(selectorMaxX - horizontalInset, 1)
            let selectorWidth = min(max(availableWidth * 0.34, 120), min(220, selectorAvailableWidth))
            logSelectorFrame = CGRect(x: selectorMaxX - selectorWidth,
                                      y: 31,
                                      width: selectorWidth,
                                      height: 24)
            let currentLog = currentLogFile(for: backend)
            let selector = makeButtonLayer(title: "Log: \(logSelectorTitle(for: currentLog, index: selectedLog?.logIndex ?? 0))",
                                           emphasized: false)
            selector.frame = logSelectorFrame
            logHeaderLayer.addSublayer(selector)
        }

        let title = makeTextLayer(size: 16, weight: .semibold, color: .labelColor)
        title.string = backend?.displayName ?? "Logs"
        let titleMaxX = logSelectorFrame.isNull || logSelectorFrame.isEmpty ? logDismissFrame.minX - 8 : logSelectorFrame.minX - 8
        title.frame = CGRect(x: horizontalInset,
                             y: 34,
                             width: max(titleMaxX - horizontalInset, 1),
                             height: 20)
        logHeaderLayer.addSublayer(title)

        let detailText = logHeaderDetailText()
        if renderedLogHeaderDetailText != detailText {
            renderedLogHeaderDetailText = detailText
            if logHeaderDetailSelectionRange != nil {
                logHeaderDetailSelectionRange = nil
                updateEditingAndPasteboardState()
            }
        }

        logHeaderDetailFrame = CGRect(x: horizontalInset,
                                      y: 14,
                                      width: max(logHeaderLayer.bounds.width - horizontalInset * 2, 1),
                                      height: 16)
        renderLogHeaderDetailSelection()

        let detail = makeTextLayer(size: 11, weight: .regular, color: logHeaderDetailColor())
        detail.string = detailText
        detail.frame = logHeaderDetailFrame
        logHeaderLayer.addSublayer(detail)
    }

    private func renderLogHeaderDetailSelection() {
        guard let selectionRange = normalizedLogHeaderDetailSelectionRange(logHeaderDetailSelectionRange) else {
            return
        }

        let line = logHeaderDetailLine()
        var startSecondaryOffset: CGFloat = 0
        var endSecondaryOffset: CGFloat = 0
        let startX = CGFloat(CTLineGetOffsetForStringIndex(line, selectionRange.location, &startSecondaryOffset))
        let endX = CGFloat(CTLineGetOffsetForStringIndex(line, selectionRange.location + selectionRange.length, &endSecondaryOffset))
        let minX = min(startX, endX)
        let maxX = max(startX, endX)
        let x = max(minX, 0)
        let width = max(min(maxX, logHeaderDetailFrame.width) - x, 1)

        let selection = CALayer()
        selection.frame = CGRect(x: logHeaderDetailFrame.minX + x,
                                 y: logHeaderDetailFrame.minY,
                                 width: width,
                                 height: logHeaderDetailFrame.height)
        selection.backgroundColor = resolvedCGColor(windowIsActive ? NSColor.selectedTextBackgroundColor : NSColor.unemphasizedSelectedTextBackgroundColor)
        selection.cornerRadius = 2
        logHeaderLayer.addSublayer(selection)
    }

    private func renderLogRows() {
        let textWidth = max(logRowsClipLayer.bounds.width - logTextInsetX * 2, 1)
        if abs(textWidth - logTextLayoutWidth) > 0.5 {
            logTextLayoutWidth = textWidth
            clearLogTextFragmentLayers()
        }
        updateLogTextContentIfNeeded(text: currentLogText())
        logTextContainer.size = CGSize(width: textWidth, height: max(logContentHeight(textWidth: textWidth) - logTextInsetY * 2, logRowsClipLayer.bounds.height))
        if shouldScrollLogToBottomOnNextLayout && (logSnapshot != nil || !logError.isEmpty) {
            logScroll = clampedLogScroll(.greatestFiniteMagnitude)
            shouldScrollLogToBottomOnNextLayout = false
        } else {
            logScroll = clampedLogScroll(logScroll)
        }
        updateLogTextViewport()
        updateLogTextSelectionLayers()
    }

    private func updateLogTextViewport() {
        withoutImplicitAnimations {
            updateLogTextViewportWithoutAnimations()
        }
    }

    private func updateLogTextViewportWithoutAnimations() {
        guard logRowsClipLayer.bounds.width > 0, logRowsClipLayer.bounds.height > 0 else {
            clearLogTextFragmentLayers()
            return
        }

        let textWidth = max(logRowsClipLayer.bounds.width - logTextInsetX * 2, 1)
        let contentHeight = max(logContentHeight() - logTextInsetY * 2, logRowsClipLayer.bounds.height)
        logTextContentLayer.frame = CGRect(x: logTextInsetX,
                                           y: logRowsClipLayer.bounds.height - logTextInsetY - contentHeight + logScroll,
                                           width: textWidth,
                                           height: contentHeight)
        logTextSelectionLayer.frame = CGRect(x: 0, y: 0, width: textWidth, height: contentHeight)
        updateLogScrollbarLayout()

        let visibleTextRect = visibleLogTextContentRect()
        if let coverage = logTextFragmentCoverage,
           coverage.generation == logTextContentGeneration,
           abs(coverage.textWidth - textWidth) <= 0.5,
           abs(coverage.contentHeight - contentHeight) <= 0.5,
           coverage.rect.contains(visibleTextRect) {
            return
        }

        let layoutRect = expandedLogTextContentRect(containing: visibleTextRect,
                                                    contentHeight: contentHeight)
        logTextLayoutManager.ensureLayout(for: layoutRect)
        let startLocation = logTextLayoutManager.textLayoutFragment(for: CGPoint(x: 0, y: max(layoutRect.minY, 0)))?.rangeInElement.location
            ?? logTextLayoutManager.documentRange.location

        var visibleFragmentIDs = Set<ObjectIdentifier>()
        logTextLayoutManager.enumerateTextLayoutFragments(from: startLocation,
                                                          options: [.ensuresLayout]) { fragment in
            let frame = fragment.layoutFragmentFrame
            if frame.minY > layoutRect.maxY {
                return false
            }
            guard frame.maxY >= layoutRect.minY else {
                return true
            }

            let id = ObjectIdentifier(fragment)
            visibleFragmentIDs.insert(id)
            let layer = self.logTextFragmentLayers[id] ?? {
                let layer = LogTextFragmentLayer()
                layer.contentsScale = NSScreen.main?.backingScaleFactor ?? 2
                self.logTextContentLayer.addSublayer(layer)
                self.logTextFragmentLayers[id] = layer
                return layer
            }()
            layer.textLayoutFragment = fragment
            let surface = fragment.renderingSurfaceBounds
            let topDownFrame = CGRect(x: frame.minX + surface.minX,
                                      y: frame.minY + surface.minY,
                                      width: surface.width,
                                      height: surface.height)
            layer.renderingSurfaceOffset = surface.origin
            layer.frame = CGRect(x: topDownFrame.minX,
                                 y: contentHeight - topDownFrame.maxY,
                                 width: topDownFrame.width,
                                 height: topDownFrame.height)
            return true
        }

        logTextFragmentCoverage = (generation: logTextContentGeneration,
                                   textWidth: textWidth,
                                   contentHeight: contentHeight,
                                   rect: layoutRect)
        let staleFragmentIDs = logTextFragmentLayers.keys.filter { !visibleFragmentIDs.contains($0) }
        for id in staleFragmentIDs {
            logTextFragmentLayers[id]?.removeFromSuperlayer()
            logTextFragmentLayers[id] = nil
        }
    }

    private func clearLogTextFragmentLayers() {
        for layer in logTextFragmentLayers.values {
            layer.removeFromSuperlayer()
        }
        logTextFragmentLayers = [:]
        logTextFragmentCoverage = nil
        logTextSelectionCoverage = nil
    }

    private func visibleLogTextContentRect() -> CGRect {
        CGRect(x: 0,
               y: max(logScroll - logTextInsetY, 0),
               width: max(logRowsClipLayer.bounds.width - logTextInsetX * 2, 1),
               height: logRowsClipLayer.bounds.height)
    }

    private func expandedLogTextContentRect(containing rect: CGRect, contentHeight: CGFloat) -> CGRect {
        let overscan = max(logRowsClipLayer.bounds.height * 1.5, 600)
        let minY = max(rect.minY - overscan, 0)
        let maxY = min(max(rect.maxY + overscan, minY + rect.height), contentHeight)
        return CGRect(x: rect.minX,
                      y: minY,
                      width: rect.width,
                      height: max(maxY - minY, rect.height))
    }

    private func logScrollbarMetrics() -> ScrollbarController<BackendsHandler>.Metrics {
        ScrollbarController.Metrics(viewportSize: logRowsClipLayer.bounds.size,
                                    contentHeight: logContentHeight(),
                                    scrollOffset: logScroll)
    }

    private func updateLogScrollbarLayout() {
        logScrollbarController?.updateLayout(metrics: logScrollbarMetrics())
    }

    private func updateLogTextContentIfNeeded(text: String, force: Bool = false) {
        let displayText = text.isEmpty ? " " : text
        guard force || displayText != logRenderedText else { return }

        let shouldPreserveSelection = force && displayText == logRenderedText
        let previousRange = logTextSelectionRange
        logRenderedText = displayText
        logTextContentGeneration += 1
        logContentHeightCache = nil
        logVisualLineCache = nil
        logAttributedText = makeLogAttributedText(displayText)
        logContentStorage.attributedString = logAttributedText
        clearLogTextFragmentLayers()

        if shouldPreserveSelection,
           let previousRange,
           previousRange.location + previousRange.length <= logAttributedText.length,
           let textRange = logTextRange(for: previousRange) {
            let selection = NSTextSelection([textRange], affinity: .downstream, granularity: .character)
            logTextLayoutManager.textSelections = [selection]
            logTextSelectionRange = previousRange
        } else {
            setLogTextSelection(nil, notify: false)
        }
    }

    private func makeLogAttributedText(_ text: String) -> NSAttributedString {
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byCharWrapping
        paragraph.lineSpacing = 2

        let color: NSColor
        if !logError.isEmpty {
            color = .systemRed
        } else if logSnapshot == nil || text == "No logs yet." || text == "No registered log file." || text == "Loading logs..." {
            color = .secondaryLabelColor
        } else {
            color = .labelColor
        }

        return NSAttributedString(string: text,
                                  attributes: [
                                    .font: logTextFont(),
                                    .foregroundColor: resolvedColor(color),
                                    .paragraphStyle: paragraph
                                  ])
    }

    private func normalizedLogSelectionRange(_ range: NSRange?) -> NSRange? {
        guard let range else { return nil }
        let lower = max(min(range.location, logAttributedText.length), 0)
        let upper = max(min(range.location + range.length, logAttributedText.length), lower)
        guard upper > lower else { return nil }
        return NSRange(location: lower, length: upper - lower)
    }

    private func setLogTextSelectionRange(_ range: NSRange?, notify: Bool = true) {
        _ = notify
        let nextRange = normalizedLogSelectionRange(range)
        if nextRange == logTextSelectionRange {
            return
        }
        if nextRange != nil, logHeaderDetailSelectionRange != nil {
            logHeaderDetailSelectionRange = nil
            renderLogHeader()
        }
        logTextLayoutManager.textSelections = []
        logTextSelectionRange = nextRange
        updateLogTextSelectionLayers()
        updateEditingAndPasteboardState()
    }

    private func setLogTextSelection(_ selection: NSTextSelection?, notify: Bool = true) {
        _ = notify
        let nextRange = normalizedLogSelectionRange(logTextRangeOffsets(for: selection))
        if nextRange == logTextSelectionRange {
            return
        }
        if nextRange != nil, logHeaderDetailSelectionRange != nil {
            logHeaderDetailSelectionRange = nil
            renderLogHeader()
        }
        logTextLayoutManager.textSelections = selection.map { [$0] } ?? []
        logTextSelectionRange = nextRange
        updateLogTextSelectionLayers()
        updateEditingAndPasteboardState()
    }

    private func updateLogTextSelectionLayers(force: Bool = false) {
        withoutImplicitAnimations {
            updateLogTextSelectionLayersWithoutAnimations(force: force)
        }
    }

    private func updateLogTextSelectionLayersWithoutAnimations(force: Bool) {
        guard let selectionRange = logTextSelectionRange,
              selectionRange.length > 0 else {
            if !logTextSelectionLayers.isEmpty {
                for layer in logTextSelectionLayers {
                    layer.removeFromSuperlayer()
                }
                logTextSelectionLayers = []
            }
            logTextSelectionCoverage = nil
            return
        }

        let textWidth = max(logRowsClipLayer.bounds.width - logTextInsetX * 2, 1)
        let contentHeight = max(logContentHeight() - logTextInsetY * 2, logRowsClipLayer.bounds.height)
        let visibleTextRect = visibleLogTextContentRect()
        if !force,
           let coverage = logTextSelectionCoverage,
           coverage.generation == logTextContentGeneration,
           abs(coverage.textWidth - textWidth) <= 0.5,
           abs(coverage.contentHeight - contentHeight) <= 0.5,
           coverage.range == selectionRange,
           coverage.rect.contains(visibleTextRect) {
            return
        }

        for layer in logTextSelectionLayers {
            layer.removeFromSuperlayer()
        }
        logTextSelectionLayers = []

        let selectionColor = resolvedCGColor(windowIsActive ? NSColor.selectedTextBackgroundColor.withAlphaComponent(0.78) : NSColor.unemphasizedSelectedTextBackgroundColor.withAlphaComponent(0.78))
        let selectionRect = expandedLogTextContentRect(containing: visibleTextRect,
                                                       contentHeight: contentHeight)
        for rect in logTextSegmentRects(for: selectionRange, type: .selection) {
            guard rect.intersects(selectionRect) else {
                continue
            }

            let normalizedRect = CGRect(x: rect.minX,
                                        y: rect.minY,
                                        width: max(rect.width, 1),
                                        height: max(rect.height, 1))
            let highlight = CALayer()
            highlight.frame = CGRect(x: normalizedRect.minX,
                                     y: logTextSelectionLayer.bounds.height - normalizedRect.maxY,
                                     width: normalizedRect.width,
                                     height: normalizedRect.height)
            highlight.backgroundColor = selectionColor
            highlight.cornerRadius = 2
            self.logTextSelectionLayer.addSublayer(highlight)
            self.logTextSelectionLayers.append(highlight)
        }
        logTextSelectionCoverage = (generation: logTextContentGeneration,
                                    textWidth: textWidth,
                                    contentHeight: contentHeight,
                                    range: selectionRange,
                                    rect: selectionRect)
    }

    private func logHeaderDetailFont() -> NSFont {
        NSFont.systemFont(ofSize: 11, weight: .regular)
    }

    private func logHeaderDetailAttributedString() -> NSAttributedString {
        NSAttributedString(string: renderedLogHeaderDetailText,
                           attributes: [
                               .font: logHeaderDetailFont(),
                               .foregroundColor: logHeaderDetailColor()
                           ])
    }

    private func logHeaderDetailLine() -> CTLine {
        CTLineCreateWithAttributedString(logHeaderDetailAttributedString())
    }

    private func normalizedLogHeaderDetailSelectionRange(_ range: NSRange?) -> NSRange? {
        guard let range else { return nil }
        let length = (renderedLogHeaderDetailText as NSString).length
        let lower = max(min(range.location, length), 0)
        let upper = max(min(range.location + range.length, length), lower)
        guard upper > lower else { return nil }
        return NSRange(location: lower, length: upper - lower)
    }

    private func setLogHeaderDetailSelectionRange(_ range: NSRange?) {
        let nextRange = normalizedLogHeaderDetailSelectionRange(range)
        if nextRange == logHeaderDetailSelectionRange {
            return
        }
        logHeaderDetailSelectionRange = nextRange
        if nextRange != nil {
            setLogTextSelectionRange(nil, notify: false)
        }
        renderLogHeader()
        updateEditingAndPasteboardState()
    }

    private func selectedLogHeaderDetailAttributedText() -> NSAttributedString? {
        guard let selectionRange = normalizedLogHeaderDetailSelectionRange(logHeaderDetailSelectionRange) else {
            return nil
        }
        return logHeaderDetailAttributedString().attributedSubstring(from: selectionRange)
    }

    private func clearLogHeaderDetailSelection() {
        guard logHeaderDetailSelectionRange != nil else { return }
        logHeaderDetailSelectionRange = nil
        renderLogHeader()
        updateEditingAndPasteboardState()
    }

    private func selectedLogAttributedText() -> NSAttributedString? {
        guard let selectionRange = logTextSelectionRange,
              selectionRange.length > 0,
              selectionRange.location >= 0,
              selectionRange.location + selectionRange.length <= logAttributedText.length else {
            return nil
        }
        return logAttributedText.attributedSubstring(from: selectionRange)
    }

    private func logAttributedText(for selection: NSTextSelection?) -> NSAttributedString? {
        guard let range = logTextRangeOffsets(for: selection),
              range.location >= 0,
              range.location + range.length <= logAttributedText.length else {
            return nil
        }
        return logAttributedText.attributedSubstring(from: range)
    }

    private func logTextRangeOffsets(for selection: NSTextSelection?) -> NSRange? {
        guard let textRange = selection?.textRanges.first else { return nil }
        let documentStart = logTextLayoutManager.documentRange.location
        let start = logContentStorage.offset(from: documentStart, to: textRange.location)
        let end = logContentStorage.offset(from: documentStart, to: textRange.endLocation)
        guard start != NSNotFound, end != NSNotFound else { return nil }
        let location = max(0, min(start, end))
        let length = min(logAttributedText.length - location, abs(end - start))
        guard length > 0 else { return nil }
        return NSRange(location: location, length: length)
    }

    private func logTextRange(for range: NSRange) -> NSTextRange? {
        let documentStart = logTextLayoutManager.documentRange.location
        guard let start = logContentStorage.location(documentStart, offsetBy: range.location),
              let end = logContentStorage.location(documentStart, offsetBy: range.location + range.length) else {
            return nil
        }
        return NSTextRange(location: start, end: end)
    }

    private func logTextLocation(for offset: Int) -> (any NSTextLocation)? {
        logContentStorage.location(logTextLayoutManager.documentRange.location,
                                   offsetBy: min(max(offset, 0), logAttributedText.length))
    }

    private func logTextOffset(for location: any NSTextLocation) -> Int {
        min(max(logContentStorage.offset(from: logTextLayoutManager.documentRange.location, to: location), 0), logAttributedText.length)
    }

    private func logTextSegmentRects(for range: NSRange, type: NSTextLayoutManager.SegmentType) -> [CGRect] {
        guard let textRange = logTextRange(for: range) else { return [] }
        logTextLayoutManager.ensureLayout(for: textRange)
        var rects: [CGRect] = []
        logTextLayoutManager.enumerateTextSegments(in: textRange, type: type, options: []) { _, rect, _, _ in
            rects.append(rect)
            return true
        }
        return rects
    }

    private func renderCreateForm() {
        createLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        createFormContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        createSectionFrames.removeAll()
        recipeFrames.removeAll()
        bundledAppInstallFrames.removeAll()
        createFieldFrames.removeAll()
        createFieldLayouts.removeAll()
        createChoiceFrames.removeAll()
        createSuggestionFrames.removeAll()
        createDirectorySelectFrames.removeAll()
        createContentClipFrame = .zero
        createDismissFrame = .zero
        createButtonFrame = .zero
        cancelCreateFrame = .zero
        bashIconSelectFrame = .zero
        nativeProjectDragFrame = .zero
        createMessageFrame = .zero
        if createMessage.isEmpty {
            createMessageSelectionRange = nil
            createMessageDragAnchorOffset = nil
        }

        let availableWidth = max(createLayer.bounds.width - horizontalInset * 2, 1)
        let pageWidth = min(availableWidth, 980)
        let left = horizontalInset + floor((availableWidth - pageWidth) / 2)
        let overlay = CALayer()
        overlay.frame = createLayer.bounds
        overlay.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.18))
        createLayer.addSublayer(overlay)

        let panelFrame = CGRect(x: max(left - 24, 12),
                                y: 20,
                                width: min(pageWidth + 48, createLayer.bounds.width - 24),
                                height: max(createLayer.bounds.height - 44, 80))
        let panel = CALayer()
        panel.frame = panelFrame
        panel.cornerRadius = 12
        panel.borderWidth = 1
        panel.borderColor = resolvedCGColor(.separatorColor)
        panel.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        createLayer.addSublayer(panel)

        let contentClipFrame = panelFrame.insetBy(dx: 1, dy: 1)
        createContentClipFrame = contentClipFrame
        let contentClipLayer = CALayer()
        contentClipLayer.frame = contentClipFrame
        contentClipLayer.masksToBounds = true
        createLayer.addSublayer(contentClipLayer)
        createFormContentLayer.frame = CGRect(x: -contentClipFrame.minX,
                                              y: -contentClipFrame.minY,
                                              width: createLayer.bounds.width,
                                              height: createLayer.bounds.height)
        contentClipLayer.addSublayer(createFormContentLayer)

        createDismissFrame = CGRect(x: panelFrame.maxX - 38,
                                    y: panelFrame.maxY - 38,
                                    width: 28,
                                    height: 28)
        let dismiss = makeSymbolButtonLayer(symbolName: "x.circle", accessibilityTitle: "Close Add Apps")
        dismiss.frame = createDismissFrame
        createLayer.addSublayer(dismiss)

        if let generatedNativeProject, !isPerformingAction {
            createContentBottom = renderGeneratedNativeProjectView(generatedNativeProject,
                                                                   panelFrame: panelFrame,
                                                                   width: pageWidth)
            if clampCreateScrollUsingRenderedContent() {
                renderCreateForm()
            }
            return
        }

        let top = max(panelFrame.maxY - 62, 0) + createScroll

        let pageTitle = makeTextLayer(size: 22, weight: .semibold, color: .labelColor)
        pageTitle.string = "Add Apps"
        pageTitle.frame = CGRect(x: left, y: top, width: max(pageWidth - 48, 1), height: 28)
        addCreateFormSublayer(pageTitle)

        let contentTop = renderCreateSectionCards(left: left,
                                                  top: top - 50,
                                                  width: pageWidth)

        switch selectedCreateSection {
        case .appCatalog:
            let addableApps = bundledCatalogBackends()
            if addableApps.isEmpty {
                renderCreateEmptyMessage("No apps are currently available.",
                                         left: left,
                                         top: contentTop,
                                         width: pageWidth)
                createContentBottom = contentTop - 20
            } else {
                createContentBottom = renderAddableAppsSection(apps: addableApps,
                                                               left: left,
                                                               top: contentTop,
                                                               width: pageWidth)
            }
            if clampCreateScrollUsingRenderedContent() {
                renderCreateForm()
            }
            return
        case .bashCommands:
            createContentBottom = renderBashCommandsSection(left: left,
                                                            top: contentTop,
                                                            width: pageWidth)
            if clampCreateScrollUsingRenderedContent() {
                renderCreateForm()
            }
            sendCreateFieldTextInputGeometryUpdate()
            return
        case .nativeApp:
            createContentBottom = renderNativeAppSection(left: left,
                                                         top: contentTop,
                                                         width: pageWidth)
            if clampCreateScrollUsingRenderedContent() {
                renderCreateForm()
            }
            sendCreateFieldTextInputGeometryUpdate()
            return
        case .otherRecipes:
            break
        }

        let visibleRecipes = otherRecipeRecords()
        if visibleRecipes.isEmpty {
            let empty = makeTextLayer(size: 13, weight: .regular, color: .secondaryLabelColor)
            empty.string = isLoadingRecipes ? "Loading recipes..." : "No recipes loaded."
            empty.frame = CGRect(x: left, y: contentTop, width: pageWidth, height: 20)
            addCreateFormSublayer(empty)
            createContentBottom = contentTop
            if clampCreateScrollUsingRenderedContent() {
                renderCreateForm()
            }
            return
        }

        let usesTwoPaneLayout = pageWidth >= 760
        let paneGap: CGFloat = 22
        let selectorWidth: CGFloat = usesTwoPaneLayout ? min(300, floor(pageWidth * 0.34)) : pageWidth
        let detailLeft = usesTwoPaneLayout ? left + selectorWidth + paneGap : left
        let detailWidth = usesTwoPaneLayout ? pageWidth - selectorWidth - paneGap : pageWidth

        let cardHeight: CGFloat = 58
        let listTop = contentTop
        var cardY = listTop - cardHeight
        let cardWidth = selectorWidth
        let selectedVisibleRecipe = selectedRecipe()
        for recipe in visibleRecipes {
            let frame = CGRect(x: left, y: cardY, width: cardWidth, height: cardHeight)
            recipeFrames.append((frame, recipe.identifier))
            let selected = recipe.identifier == selectedVisibleRecipe?.identifier
            let card = CALayer()
            card.frame = frame
            card.cornerRadius = 7
            card.borderWidth = selected ? 1.5 : 1
            card.borderColor = selected ? resolvedCGColor(.controlAccentColor) : resolvedCGColor(.separatorColor)
            card.backgroundColor = selected ? resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.12)) : resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.45))
            addCreateFormSublayer(card)

            let name = makeTextLayer(size: 12, weight: .semibold, color: .labelColor)
            name.string = recipe.displayName
            name.frame = CGRect(x: 11, y: 33, width: cardWidth - 22, height: 16)
            card.addSublayer(name)
            let summary = makeTextLayer(size: 10, weight: .regular, color: .secondaryLabelColor)
            summary.string = recipe.summary
            summary.frame = CGRect(x: 11, y: 12, width: cardWidth - 22, height: 14)
            card.addSublayer(summary)
            cardY -= cardHeight + 10
        }
        let recipeListBottom = cardY + cardHeight + 10

        guard let recipe = selectedVisibleRecipe else { return }
        let summary = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        summary.string = recipe.summary
        let formTop = usesTwoPaneLayout ? listTop - 18 : recipeListBottom - 34
        summary.frame = CGRect(x: detailLeft, y: formTop, width: detailWidth, height: 18)
        addCreateFormSublayer(summary)

        var y = formTop - 58
        for field in visibleCreateFields(for: recipe) {
            if field.fieldType == "choice" {
                addCreateChoiceField(field, frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 50))
                y -= 62
            } else {
                addCreateField(field,
                               value: createValue(for: field),
                               frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 46),
                               monospaced: field.key == "command" || field.key == "executablePath" || field.key == "python")
                y -= field.suggestions.isEmpty ? 62 : 84
            }
        }

        createButtonFrame = CGRect(x: detailLeft, y: y + 14, width: 96, height: 30)
        cancelCreateFrame = .zero
        let createButton = makeButtonLayer(title: isPerformingAction ? "Creating..." : "Create", emphasized: true)
        createButton.frame = createButtonFrame
        addCreateFormSublayer(createButton)

        if !createMessage.isEmpty {
            addCreateMessageLayer(frame: CGRect(x: detailLeft,
                                                y: y - 26,
                                                width: detailWidth,
                                                height: 18),
                                  color: createMessage.hasPrefix("Created") ? .secondaryLabelColor : .systemRed)
        }
        let formBottom = !createMessage.isEmpty ? y - 26 : y + 14
        createContentBottom = min(recipeListBottom, formBottom)
        sendCreateFieldTextInputGeometryUpdate()
        if clampCreateScrollUsingRenderedContent() {
            renderCreateForm()
        }
    }

    private func addCreateField(_ field: RecipeFieldRecord,
                                value: String,
                                frame: CGRect,
                                monospaced: Bool = false) {
        let labelLayer = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
        labelLayer.string = field.label
        labelLayer.frame = CGRect(x: frame.minX, y: frame.maxY - 16, width: frame.width, height: 14)
        addCreateFormSublayer(labelLayer)

        let hasDirectoryPicker = field.fieldType == "directory"
        let selectButtonWidth: CGFloat = 68
        let selectGap: CGFloat = 8
        let boxWidth = hasDirectoryPicker ? max(frame.width - selectButtonWidth - selectGap, 120) : frame.width
        let boxFrame = CGRect(x: frame.minX, y: frame.minY, width: boxWidth, height: 30)
        createFieldFrames.append((boxFrame, field.key))
        let textFrame = CGRect(x: boxFrame.minX + 9, y: boxFrame.minY + 7, width: max(boxFrame.width - 18, 1), height: 16)
        createFieldLayouts[field.key] = CreateFieldLayout(fieldFrame: boxFrame,
                                                          textFrame: textFrame,
                                                          key: field.key,
                                                          monospaced: monospaced,
                                                          multiline: false)
        let box = CALayer()
        box.frame = boxFrame
        box.masksToBounds = true
        box.cornerRadius = 5
        let focused = createInputController.isFocused && activeCreateFieldKey == field.key
        box.borderWidth = focused ? 1.5 : 1
        box.borderColor = focused ? resolvedCGColor(.keyboardFocusIndicatorColor) : resolvedCGColor(.separatorColor)
        box.backgroundColor = resolvedCGColor(.textBackgroundColor)
        addCreateFormSublayer(box)

        if focused,
           let selectionRange = createInputController.selectionRange,
           !value.isEmpty {
            let line = makeCreateFieldLine(for: value, monospaced: monospaced)
            let offsets = selectionOffsets(line: line,
                                           text: value,
                                           range: selectionRange,
                                           maxWidth: textFrame.width)
            let selectionWidth = max(0, offsets.end - offsets.start)
            if selectionWidth > 0.5 {
                let selection = CALayer()
                selection.frame = CGRect(x: 9 + offsets.start,
                                         y: 6,
                                         width: selectionWidth,
                                         height: 18)
                selection.backgroundColor = resolvedCGColor(windowIsActive ? .selectedTextBackgroundColor : .unemphasizedSelectedTextBackgroundColor)
                box.addSublayer(selection)
            }
        }

        let text = makeTextLayer(size: 12, weight: .regular, color: value.isEmpty ? .tertiaryLabelColor : .labelColor, monospaced: monospaced)
        text.string = value.isEmpty ? field.placeholder : value
        text.frame = CGRect(x: 9, y: 7, width: max(boxFrame.width - 18, 1), height: 16)
        box.addSublayer(text)
        if focused,
           windowIsActive,
           !createInputController.hasSelection,
           let layout = createFieldLayouts[field.key] {
            let line = value.isEmpty ? nil : makeCreateFieldLine(for: value, monospaced: monospaced)
            let cursorFrame = createFieldCursorRect(layout: layout, cachedLine: line)
            addBlinkingTextCaret(to: box,
                                 frame: cursorFrame.offsetBy(dx: -layout.fieldFrame.minX,
                                                             dy: -layout.fieldFrame.minY))
        }

        if hasDirectoryPicker {
            let selectFrame = CGRect(x: boxFrame.maxX + selectGap,
                                     y: frame.minY,
                                     width: min(selectButtonWidth, max(frame.maxX - boxFrame.maxX - selectGap, 1)),
                                     height: 30)
            createDirectorySelectFrames.append((selectFrame, field.key))
            let selectButton = makeButtonLayer(title: "Select", emphasized: false)
            selectButton.frame = selectFrame
            addCreateFormSublayer(selectButton)
        }

        if !field.suggestions.isEmpty {
            var chipX = frame.minX
            let chipY = frame.minY - 28
            for suggestion in field.suggestions.prefix(3) {
                let chipWidth = min(max(CGFloat(suggestion.count) * 6.5 + 18, 84), min(frame.width, 260))
                if chipX + chipWidth > frame.maxX { break }
                let chipFrame = CGRect(x: chipX, y: chipY, width: chipWidth, height: 22)
                createSuggestionFrames.append((chipFrame, field.key, suggestion))
                let chip = makeButtonLayer(title: suggestion, emphasized: false)
                chip.applyStyle(textCGColor: resolvedCGColor(.controlAccentColor),
                                backgroundCGColor: resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.12)),
                                font: NSFont.systemFont(ofSize: 10, weight: .medium))
                chip.frame = chipFrame
                addCreateFormSublayer(chip)
                chipX += chipWidth + 8
            }
        }
    }

    private func addCreateTextArea(key: String,
                                   label: String,
                                   placeholder: String,
                                   frame: CGRect) {
        let labelLayer = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
        labelLayer.string = label
        labelLayer.frame = CGRect(x: frame.minX, y: frame.maxY - 16, width: frame.width, height: 14)
        addCreateFormSublayer(labelLayer)

        let boxFrame = CGRect(x: frame.minX, y: frame.minY, width: frame.width, height: max(frame.height - 20, 80))
        createFieldFrames.append((boxFrame, key))
        let textFrame = CGRect(x: boxFrame.minX + 10, y: boxFrame.minY + 9, width: max(boxFrame.width - 20, 1), height: max(boxFrame.height - 18, 1))
        createFieldLayouts[key] = CreateFieldLayout(fieldFrame: boxFrame,
                                                    textFrame: textFrame,
                                                    key: key,
                                                    monospaced: true,
                                                    multiline: true)

        let box = CALayer()
        box.frame = boxFrame
        box.masksToBounds = true
        box.cornerRadius = 6
        let focused = createInputController.isFocused && activeCreateFieldKey == key
        box.borderWidth = focused ? 1.5 : 1
        box.borderColor = focused ? resolvedCGColor(.keyboardFocusIndicatorColor) : resolvedCGColor(.separatorColor)
        box.backgroundColor = resolvedCGColor(.textBackgroundColor)
        addCreateFormSublayer(box)

        let value = createValues[key, default: ""]
        let textOriginX = textFrame.minX - boxFrame.minX
        if value.isEmpty {
            let text = makeTextLayer(size: 12, weight: .regular, color: .tertiaryLabelColor, monospaced: true)
            text.string = placeholder
            text.frame = CGRect(x: textOriginX,
                                y: textFrame.maxY - boxFrame.minY - 16,
                                width: max(boxFrame.width - 20, 1),
                                height: 16)
            box.addSublayer(text)
            if focused,
               windowIsActive,
               !createInputController.hasSelection,
               let layout = createFieldLayouts[key] {
                let cursorFrame = createFieldCursorRect(layout: layout, cachedLine: nil)
                addBlinkingTextCaret(to: box,
                                     frame: cursorFrame.offsetBy(dx: -layout.fieldFrame.minX,
                                                                 dy: -layout.fieldFrame.minY))
            }
            return
        }

        guard let layout = createFieldLayouts[key] else { return }
        let fragments = createTextAreaLineFragments(text: value, layout: layout)
        if focused,
           let selectionRange = createInputController.selectionRange {
            for fragment in fragments {
                let lower = max(selectionRange.lowerBound, fragment.start)
                let upper = min(selectionRange.upperBound, fragment.end)
                guard upper > lower else { continue }
                let line = makeCreateFieldLine(for: fragment.text, monospaced: true)
                let offsets = selectionOffsets(line: line,
                                               text: fragment.text,
                                               range: (lower - fragment.start)..<(upper - fragment.start),
                                               maxWidth: textFrame.width)
                let selectionWidth = max(0, offsets.end - offsets.start)
                guard selectionWidth > 0.5 else { continue }
                let selection = CALayer()
                selection.frame = CGRect(x: textOriginX + offsets.start,
                                         y: fragment.y - boxFrame.minY - 1,
                                         width: selectionWidth,
                                         height: 18)
                selection.backgroundColor = resolvedCGColor(windowIsActive ? .selectedTextBackgroundColor : .unemphasizedSelectedTextBackgroundColor)
                box.addSublayer(selection)
            }
        }

        for fragment in fragments {
            let lineBottom = fragment.y
            if lineBottom + 16 < textFrame.minY || lineBottom > textFrame.maxY {
                continue
            }
            let text = makeTextLayer(size: 12, weight: .regular, color: .labelColor, monospaced: true)
            text.string = fragment.text
            text.frame = CGRect(x: textOriginX,
                                y: lineBottom - boxFrame.minY,
                                width: max(boxFrame.width - 20, 1),
                                height: 16)
            box.addSublayer(text)
        }
        if focused,
           windowIsActive,
           !createInputController.hasSelection {
            let cursorFrame = createFieldCursorRect(layout: layout, cachedLine: nil)
            addBlinkingTextCaret(to: box,
                                 frame: cursorFrame.offsetBy(dx: -layout.fieldFrame.minX,
                                                             dy: -layout.fieldFrame.minY))
        }
    }

    private func addCreateChoiceField(_ field: RecipeFieldRecord, frame: CGRect) {
        let labelLayer = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
        labelLayer.string = field.label
        labelLayer.frame = CGRect(x: frame.minX, y: frame.maxY - 16, width: frame.width, height: 14)
        addCreateFormSublayer(labelLayer)

        var x = frame.minX
        let value = createValue(for: field)
        for choice in field.choices {
            let width = max(CGFloat(choice.title.count) * 8 + 24, 72)
            let choiceFrame = CGRect(x: x, y: frame.minY, width: width, height: 30)
            createChoiceFrames.append((choiceFrame, field.key, choice.value))
            let selected = value == choice.value
            let button = makeButtonLayer(title: choice.title, emphasized: selected)
            button.frame = choiceFrame
            addCreateFormSublayer(button)
            x += width + 8
        }
    }

    private func addCreateCheckbox(key: String,
                                   title: String,
                                   subtitle: String,
                                   frame: CGRect) {
        let selected = createValues[key, default: "true"] == "true"
        createChoiceFrames.append((frame, key, "true"))

        let background = CALayer()
        background.frame = frame
        background.cornerRadius = 7
        background.borderWidth = selected ? 1.5 : 1
        background.borderColor = resolvedCGColor(selected ? .controlAccentColor : .separatorColor)
        background.backgroundColor = resolvedCGColor(selected
            ? NSColor.controlAccentColor.withAlphaComponent(0.1)
            : NSColor.controlBackgroundColor.withAlphaComponent(0.4))
        addCreateFormSublayer(background)

        let icon = CALayer()
        icon.frame = CGRect(x: 10, y: 9, width: 18, height: 18)
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: selected ? "checkmark.square.fill" : "square",
                                      pointSize: 18,
                                      color: selected ? .controlAccentColor : .secondaryLabelColor)
        background.addSublayer(icon)

        let titleLayer = makeTextLayer(size: 12, weight: .semibold, color: .labelColor)
        titleLayer.string = title
        titleLayer.frame = CGRect(x: 36, y: 18, width: max(frame.width - 44, 1), height: 15)
        background.addSublayer(titleLayer)

        let subtitleLayer = makeTextLayer(size: 9, weight: .regular, color: .secondaryLabelColor)
        subtitleLayer.string = subtitle
        subtitleLayer.frame = CGRect(x: 36, y: 5, width: max(frame.width - 44, 1), height: 12)
        background.addSublayer(subtitleLayer)
    }

    private func buildAccessibilitySnapshot() -> OuterframeAccessibilitySnapshot {
        var nextIdentifier: UInt32 = 1
        var children: [OuterframeAccessibilityNode] = []

        if pendingInstallBackend != nil {
            children.append(contentsOf: buildInstallPromptAccessibilityNodes(nextIdentifier: &nextIdentifier))
        } else if pendingPasswordAction != nil {
            children.append(contentsOf: buildPasswordPromptAccessibilityNodes(nextIdentifier: &nextIdentifier))
        } else if pendingFilePicker != nil {
            children.append(contentsOf: buildFilePickerAccessibilityNodes(nextIdentifier: &nextIdentifier))
        } else {
            children.append(contentsOf: buildToolbarAccessibilityNodes(nextIdentifier: &nextIdentifier))
            switch mode {
            case .apps:
                children.append(contentsOf: buildAppsAccessibilityNodes(nextIdentifier: &nextIdentifier))
                children.append(contentsOf: buildLogAccessibilityNodes(nextIdentifier: &nextIdentifier))
            case .create:
                children.append(contentsOf: buildCreateAccessibilityNodes(nextIdentifier: &nextIdentifier))
            }
        }

        let root = OuterframeAccessibilityNode(identifier: 0,
                                               role: .container,
                                               frame: rootLayer.bounds,
                                               label: "Outer Shell",
                                               children: children)
        return OuterframeAccessibilitySnapshot(rootNodes: [root])
    }

    private func accessibilityNode(nextIdentifier: inout UInt32,
                                   role: OuterframeAccessibilityRole,
                                   frame: CGRect,
                                   label: String? = nil,
                                   value: String? = nil,
                                   hint: String? = nil,
                                   children: [OuterframeAccessibilityNode] = [],
                                   rowCount: Int? = nil,
                                   columnCount: Int? = nil,
                                   isEnabled: Bool = true) -> OuterframeAccessibilityNode {
        let identifier = nextIdentifier
        nextIdentifier = nextIdentifier == UInt32.max ? 1 : nextIdentifier + 1
        return OuterframeAccessibilityNode(identifier: identifier,
                                           role: role,
                                           frame: frame,
                                           label: label,
                                           value: value,
                                           hint: hint,
                                           children: children,
                                           rowCount: rowCount,
                                           columnCount: columnCount,
                                           isEnabled: isEnabled)
    }

    private func accessibilityFrame(_ frame: CGRect,
                                    from layer: CALayer,
                                    clippedBy clipFrame: CGRect? = nil) -> CGRect? {
        var rootFrame = rootLayer.convert(frame, from: layer)
        if let clipFrame {
            rootFrame = rootFrame.intersection(clipFrame)
        }
        guard !rootFrame.isNull,
              rootFrame.width > 0.5,
              rootFrame.height > 0.5 else {
            return nil
        }
        return rootFrame
    }

    private func accessibilityFrame(_ frame: CGRect,
                                    clippedBy clipFrame: CGRect? = nil) -> CGRect? {
        accessibilityFrame(frame, from: rootLayer, clippedBy: clipFrame)
    }

    private func accessibilityPreview(_ text: String, maximumLength: Int = 4096) -> String {
        if text.count <= maximumLength {
            return text
        }
        let end = text.index(text.startIndex, offsetBy: maximumLength)
        return String(text[..<end])
    }

    private func buildToolbarAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        var nodes: [OuterframeAccessibilityNode] = []
        if let status = statusLayer.string as? String,
           !status.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
           let frame = accessibilityFrame(statusLayer.frame, from: toolbarLayer) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .staticText,
                                           frame: frame,
                                           label: status))
        }
        if !outerShellActionLayer.isHidden,
           let frame = accessibilityFrame(outerShellActionFrame, from: toolbarLayer) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Outer Shell Actions"))
        }
        return nodes
    }

    private func buildAppsAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        var nodes: [OuterframeAccessibilityNode] = []
        let clipFrame = rootLayer.convert(appsLayer.bounds, from: appsLayer)
        let workspaceClipFrame = rootLayer.convert(
            workspacePaneClipLayer.bounds,
            from: workspacePaneClipLayer
        )

        for card in appCardFrames {
            let contentLayer = appsContentLayer(for: card.item.scope)
            let cardClipFrame = usesWorkspaceSplitLayout && card.item.containerContext != nil
                ? workspaceClipFrame
                : clipFrame
            guard let frame = accessibilityFrame(
                card.frame,
                from: contentLayer,
                clippedBy: cardClipFrame
            ) else { continue }
            let status = endpointIsRunning(card.item.primaryEndpoint) ? "Running" : "Not running"
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Open \(overviewTitle(for: card.item))",
                                           value: status,
                                           hint: card.item.subtitle))
        }

        for badge in appBadgeFrames {
            guard let frame = accessibilityFrame(badge.frame, from: appsScrollContentLayer, clippedBy: clipFrame) else { continue }
            let scope = badge.endpoint.backend.serviceScope == "system" ? " as root" : ""
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Open \(badge.displayName)\(scope)",
                                           value: "Running"))
        }

        for target in overviewMenuFrames {
            if let frame = accessibilityFrame(target.frame, from: appsScrollContentLayer, clippedBy: clipFrame) {
                nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier, role: .button, frame: frame, label: "Actions for \(overviewTitle(for: target.item))"))
            }
        }
        for target in overviewAddFrames {
            if let frame = accessibilityFrame(target, from: appsScrollContentLayer, clippedBy: clipFrame) {
                nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier, role: .button, frame: frame, label: "Add more…"))
            }
        }
        for target in workspaceOverviewActionFrames {
            if let frame = accessibilityFrame(target.frame, from: appsScrollContentLayer, clippedBy: clipFrame) {
                let label = target.operation == "menu" ? "Manage \(target.workspace.name)" : target.operation == "editContainer" ? "Edit \(target.workspace.name)" : "Copy command"
                nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier, role: .button, frame: frame, label: label))
            }
        }
        if let frame = accessibilityFrame(workspaceOverviewCreateFrame, from: appsScrollContentLayer, clippedBy: clipFrame) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier, role: .button, frame: frame, label: "Add container…"))
        }
        if let frame = accessibilityFrame(addAppFrame, from: appsScrollContentLayer, clippedBy: clipFrame) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Add app"))
        }

        if nodes.isEmpty,
           let frame = accessibilityFrame(appsLayer.bounds, from: appsLayer) {
            let message = isLoadingBackends ? "Loading apps" : (backendError.isEmpty ? "No apps available" : backendError)
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .staticText,
                                           frame: frame,
                                           label: message))
        }
        return nodes
    }

    private func buildLogAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        guard mode == .apps,
              selectedServiceID != nil,
              !logHeaderLayer.isHidden else {
            return []
        }

        var nodes: [OuterframeAccessibilityNode] = []
        if let frame = accessibilityFrame(logHeaderDetailFrame, from: logHeaderLayer) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .staticText,
                                           frame: frame,
                                           label: logHeaderDetailText()))
        }
        if let frame = accessibilityFrame(logSelectorFrame, from: logHeaderLayer),
           let backend = selectedBackend(),
           backend.logFiles.count > 1 {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Choose log"))
        }
        if let frame = accessibilityFrame(logDismissFrame, from: logHeaderLayer) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Close logs"))
        }

        let logText = currentLogText()
        if !logText.isEmpty,
           let frame = accessibilityFrame(logRowsClipLayer.bounds, from: logRowsClipLayer) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .staticText,
                                           frame: frame,
                                           label: "Log output",
                                           value: accessibilityPreview(logText)))
        }
        return nodes
    }

    private func buildCreateAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        var nodes: [OuterframeAccessibilityNode] = []
        let clipFrame = rootLayer.convert(createContentClipFrame, from: createLayer)

        if let frame = accessibilityFrame(createDismissFrame, from: createLayer) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Close Add Apps"))
        }

        for section in createSectionFrames {
            guard let frame = accessibilityFrame(section.frame, from: createLayer, clippedBy: clipFrame) else { continue }
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: createSectionAccessibilityLabel(section.section),
                                           value: selectedCreateSection == section.section ? "Selected" : nil))
        }

        for app in bundledAppInstallFrames {
            guard let frame = accessibilityFrame(app.frame, from: createLayer, clippedBy: clipFrame) else { continue }
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Install \(app.backend.displayName)"))
        }

        for recipe in recipeFrames {
            guard let frame = accessibilityFrame(recipe.frame, from: createLayer, clippedBy: clipFrame),
                  let record = recipes.first(where: { $0.identifier == recipe.recipeID }) else { continue }
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: record.displayName,
                                           value: selectedRecipeID == recipe.recipeID ? "Selected" : nil,
                                           hint: record.summary))
        }

        for field in createFieldFrames {
            guard let frame = accessibilityFrame(field.frame, from: createLayer, clippedBy: clipFrame) else { continue }
            let description = createFieldAccessibilityDescription(key: field.key)
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .textField,
                                           frame: frame,
                                           label: description.label,
                                           value: description.value,
                                           hint: description.hint))
        }

        for choice in createChoiceFrames {
            guard let frame = accessibilityFrame(choice.frame, from: createLayer, clippedBy: clipFrame) else { continue }
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: createChoiceAccessibilityTitle(key: choice.key, value: choice.value),
                                           value: createValue(forCreateKey: choice.key) == choice.value ? "Selected" : nil))
        }

        for suggestion in createSuggestionFrames {
            guard let frame = accessibilityFrame(suggestion.frame, from: createLayer, clippedBy: clipFrame) else { continue }
            let field = createFieldAccessibilityDescription(key: suggestion.key)
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Use \(suggestion.value)",
                                           hint: field.label))
        }

        for select in createDirectorySelectFrames {
            guard let frame = accessibilityFrame(select.frame, from: createLayer, clippedBy: clipFrame) else { continue }
            let field = createFieldAccessibilityDescription(key: select.key)
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Select \(field.label)"))
        }

        if let generatedNativeProject,
           generatedNativeProject.hasPlatformWorkspace,
           let frame = accessibilityFrame(nativeProjectDragFrame, from: createLayer, clippedBy: clipFrame) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "macOS platform builder folder",
                                           value: generatedNativeProject.folderName,
                                           hint: "Drag this builder to your Mac; the canonical project remains on the server."))
        }

        if let frame = accessibilityFrame(bashIconSelectFrame, from: createLayer, clippedBy: clipFrame) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: "Select icon"))
        }

        if let frame = accessibilityFrame(createButtonFrame, from: createLayer, clippedBy: clipFrame) {
            let title: String
            switch selectedCreateSection {
            case .bashCommands:
                title = "Save"
            case .nativeApp:
                title = "Generate"
            default:
                title = "Create"
            }
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .button,
                                           frame: frame,
                                           label: isPerformingAction ? "\(title) in progress" : title,
                                           isEnabled: !isPerformingAction))
        }

        if !createMessage.isEmpty,
           let frame = accessibilityFrame(CGRect(x: horizontalInset,
                                                 y: createContentBottom,
                                                 width: max(createLayer.bounds.width - horizontalInset * 2, 1),
                                                 height: 22),
                                          from: createLayer,
                                          clippedBy: clipFrame) {
            nodes.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                           role: .staticText,
                                           frame: frame,
                                           label: createMessage))
        }

        return nodes
    }

    private func buildInstallPromptAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        guard let backend = pendingInstallBackend else { return [] }
        var children: [OuterframeAccessibilityNode] = []
        if let frame = accessibilityFrame(installConfirmFrame) {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: "Enable for user"))
        }
        if let frame = accessibilityFrame(installRootConfirmFrame) {
            let systemOnlyPlaceholder = installsBundledPlaceholderAsSystemOnly(backend)
            let label = systemOnlyPlaceholder
                ? "Enable"
                : ((backend.supportsRoot ?? false) && !(backend.rootOnly ?? false)
                    ? "Enable for user and root"
                    : "Enable for root")
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: label))
        }
        if let frame = accessibilityFrame(installCancelFrame) {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: "Cancel"))
        }
        guard let frame = accessibilityFrame(installPanelFrame) else { return children }
        return [
            accessibilityNode(nextIdentifier: &nextIdentifier,
                              role: .container,
                              frame: frame,
                              label: "Install \(backend.displayName)",
                              value: "Outer Shell will download this app.",
                              children: children)
        ]
    }

    private func buildPasswordPromptAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        guard let action = pendingPasswordAction else { return [] }
        var children: [OuterframeAccessibilityNode] = []
        if let frame = accessibilityFrame(passwordFieldFrame) {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .textField,
                                              frame: frame,
                                              label: "Administrator password",
                                              hint: "\(action.displayName): \(sudoPasswordMessage)"))
        }
        if let frame = accessibilityFrame(passwordCancelFrame) {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: "Cancel"))
        }
        if let frame = accessibilityFrame(passwordSubmitFrame) {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: "Continue"))
        }
        guard let frame = accessibilityFrame(passwordPanelFrame) else { return children }
        return [
            accessibilityNode(nextIdentifier: &nextIdentifier,
                              role: .container,
                              frame: frame,
                              label: "Administrator Password",
                              value: "\(action.displayName): \(sudoPasswordMessage)",
                              children: children)
        ]
    }

    private func buildFilePickerAccessibilityNodes(nextIdentifier: inout UInt32) -> [OuterframeAccessibilityNode] {
        guard let picker = pendingFilePicker else { return [] }
        var children: [OuterframeAccessibilityNode] = []

        for segment in filePickerBreadcrumbSegmentFrames {
            let localFrame = FilePickerBreadcrumbBar.rootFrame(for: segment.frame, in: filePickerBreadcrumbFrame)
            guard let frame = accessibilityFrame(localFrame, from: createLayer) else { continue }
            let title = FilePickerBreadcrumbBar.segments(for: picker.directory).first(where: { $0.path == segment.path })?.title ?? segment.path
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: title == "/" ? "Root folder" : "\(title) folder",
                                              value: segment.path))
        }

        for entry in filePickerEntryFrames {
            guard let frame = accessibilityFrame(entry.frame, from: createLayer) else { continue }
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: entry.entry.willCreate
                                                ? "\(entry.entry.name) folder, will be created"
                                                : (entry.entry.isDirectory ? "\(entry.entry.name) folder" : entry.entry.name),
                                              value: entry.entry.path))
        }

        if let frame = accessibilityFrame(filePickerCancelFrame, from: createLayer) {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: "Cancel"))
        }
        if let frame = accessibilityFrame(filePickerSaveFrame, from: createLayer) {
            children.append(accessibilityNode(nextIdentifier: &nextIdentifier,
                                              role: .button,
                                              frame: frame,
                                              label: "Choose"))
        }

        guard let frame = accessibilityFrame(filePickerPanelFrame, from: createLayer) else { return children }
        let title: String
        switch picker.mode {
        case .chooseFile:
            title = "Choose Icon"
        case .chooseDirectory:
            title = picker.targetFieldKey == "nativeProjectRoot" ? "Choose Project Location" : "Choose Folder"
        }
        return [
            accessibilityNode(nextIdentifier: &nextIdentifier,
                              role: .container,
                              frame: frame,
                              label: title,
                              value: picker.error.isEmpty ? picker.directory : picker.error,
                              children: children)
        ]
    }

    private func createSectionAccessibilityLabel(_ section: CreateSection) -> String {
        switch section {
        case .appCatalog:
            return "Outer Shell app catalog"
        case .bashCommands:
            return "Run bash commands"
        case .nativeApp:
            return "New App"
        case .otherRecipes:
            return "Other recipes"
        }
    }

    private func createFieldAccessibilityDescription(key: String) -> (label: String, value: String?, hint: String?) {
        let bashLabels: [String: (String, String)] = [
            "bashDisplayName": ("Display Name", "My App"),
            "bashCommands": ("Bash commands", "Paste commands here"),
            "bashFrontendTransport": ("Connection", ""),
            "bashSocketPath": ("Socket Path", "/tmp/my-service.sock"),
            "bashPort": ("Port", "4000"),
            "bashIconPath": ("Icon Path", "Optional"),
            "bashIdentifier": ("ID", "my-app")
        ]
        if let description = bashLabels[key] {
            return (description.0, createValues[key], description.1.isEmpty ? nil : description.1)
        }
        let nativeLabels: [String: (String, String)] = [
            "nativeAppName": ("Name", "Hello World"),
            "nativeTargetHTML": ("HTML platform", "Enabled"),
            "nativeTargetMacOS": ("macOS platform", "Enabled"),
            "nativeFrontendLanguage": ("macOS Language", "Swift"),
            "nativeBackendLanguage": ("Backend", "Go"),
            "nativeIsolationMode": ("Server Isolation", "Containerized"),
            "nativeProjectRoot": ("Project Location", "~/outerframe-apps"),
            "nativeProjectFolder": ("Project Folder", "hello-world"),
            "nativeAppID": ("App ID", "org.example.HelloWorld"),
            "nativeSocketFilename": ("Socket Filename", "org.example.HelloWorld")
        ]
        if let description = nativeLabels[key] {
            return (description.0, createValues[key], description.1.isEmpty ? nil : description.1)
        }
        if let field = selectedRecipe()?.fields.first(where: { $0.key == key }) {
            return (field.label, createValue(for: field), field.placeholder.isEmpty ? nil : field.placeholder)
        }
        return (key, createValues[key], nil)
    }

    private func createChoiceAccessibilityTitle(key: String, value: String) -> String {
        if key == "bashFrontendTransport" {
            switch value {
            case "port":
                return "Port"
            case "unixSocket":
                return "Unix Socket"
            default:
                return value
            }
        }
        if key == "nativeBackendLanguage" {
            switch value {
            case "go":
                return "Go"
            case "c":
                return "C"
            default:
                return value
            }
        }
        if key == "nativeFrontendLanguage" {
            switch value {
            case "swift":
                return "Swift"
            case "objc":
                return "Objective-C"
            default:
                return value
            }
        }
        if key == "nativeIsolationMode" {
            switch value {
            case "container":
                return "Containerized"
            case "host":
                return "Full Host Access"
            default:
                return value
            }
        }
        if key == "nativeTargetHTML" {
            return "HTML"
        }
        if key == "nativeTargetMacOS" {
            return "macOS"
        }
        return selectedRecipe()?.fields.first(where: { $0.key == key })?.choices.first(where: { $0.value == value })?.title ?? value
    }

    private func createValue(forCreateKey key: String) -> String {
        if key == "bashFrontendTransport" {
            return createValues[key, default: "port"]
        }
        if key == "nativeBackendLanguage" {
            return createValues[key, default: "go"]
        }
        if key == "nativeFrontendLanguage" {
            return createValues[key, default: "swift"]
        }
        if key == "nativeIsolationMode" {
            return createValues[key, default: "container"]
        }
        if key == "nativeTargetHTML" || key == "nativeTargetMacOS" {
            return createValues[key, default: "true"]
        }
        if let field = selectedRecipe()?.fields.first(where: { $0.key == key }) {
            return createValue(for: field)
        }
        return createValues[key, default: ""]
    }

    private func notifyAccessibilityLayoutChanged() {
        guard didRegisterLayer, !accessibilityNotificationScheduled else { return }
        accessibilityNotificationScheduled = true
        Task { @MainActor in
            accessibilityNotificationScheduled = false
            outerframeHost.notifyAccessibilityTreeChanged(.layoutChanged)
        }
    }

    private func fetchBackends(quiet: Bool = false, didRecoverNetworking: Bool = false) {
        guard !isLoadingBackends, let backendsEndpoint, let urlSession else { return }
        isLoadingBackends = true
        if !quiet {
            backendError = ""
            updateStatusText()
        }
        urlSession.dataTask(with: backendsEndpoint) { [weak self] data, response, error in
            Task { @MainActor in
                guard let self else { return }
                self.isLoadingBackends = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                        self.fetchBackends(quiet: quiet, didRecoverNetworking: true)
                        return
                    }
                    let nsError = error as NSError
                    if nsError.domain == NSURLErrorDomain,
                       nsError.code == NSURLErrorTimedOut,
                       !self.backends.isEmpty {
                        self.backendError = ""
                        self.scheduleBackendsRefreshes()
                    } else {
                        if !quiet || self.backends.isEmpty {
                            self.backendError = error.localizedDescription
                        }
                    }
                    if !quiet || self.backends.isEmpty {
                        self.scheduleLayoutUpdate()
                    }
                    return
                }
                if let http = response as? HTTPURLResponse, http.statusCode >= 400 {
                    if !quiet || self.backends.isEmpty {
                        self.backendError = "Outer Shell API returned HTTP \(http.statusCode)."
                        self.scheduleLayoutUpdate()
                    }
                    return
                }
                guard let data else {
                    if !quiet || self.backends.isEmpty {
                        self.backendError = "Outer Shell API returned no data."
                        self.scheduleLayoutUpdate()
                    }
                    return
                }
                if quiet, self.lastBackendsResponseData == data {
                    return
                }
                do {
                    let response = try BackendsResponse.decodeBinary(data)
                    let previousAppsSignature = self.appLauncherSignature(for: self.backends)
                    let nextAppsSignature = self.appLauncherSignature(for: response.backends)
                    self.lastBackendsResponseData = data
                    self.backendError = response.error
                    self.backends = response.backends
                    self.fetchEndpointIcons()
                    if self.overviewUsername == "Your user",
                       let path = self.backends.first(where: { $0.serviceScope == "user" && $0.serviceUnitPath != nil })?.serviceUnitPath {
                        let components = path.split(separator: "/")
                        if components.count > 1 && ["home", "Users"].contains(String(components[0])) {
                            self.overviewUsername = String(components[1])
                        }
                    }
                    if self.selectedContainerLogContext == nil,
                       let selectedServiceID = self.selectedServiceID,
                       !self.backends.contains(where: { $0.serviceID == selectedServiceID }) {
                        self.clearLogSelection()
                        self.restartEventWatch(resetVersions: true)
                    }
                    if self.selectedServiceID != nil {
                        self.ensureLogSelection()
                    }
                    self.clampScrollOffsets()
                    if !(quiet && self.mode == .apps && previousAppsSignature == nextAppsSignature) {
                        self.scheduleLayoutUpdate()
                    }
                    self.showAutomaticOuterShellUpdatePromptIfNeeded()
                    if self.selectedLog != nil {
                        self.fetchSelectedLog()
                    }
                } catch {
                    self.backendError = "Could not decode Outer Shell API response."
                    self.scheduleLayoutUpdate()
                }
            }
        }.resume()
    }

    private func showAutomaticOuterShellUpdatePromptIfNeeded() {
        guard !didShowAutomaticOuterShellUpdatePrompt,
              pendingOuterShellUpdate == nil,
              pendingInstallBackend == nil,
              pendingPasswordAction == nil,
              pendingAboutBackend == nil,
              let backend = outerShellBackend(),
              backend.status == "update available" else {
            return
        }
        let availableVersion = backend.availableVersion?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard !availableVersion.isEmpty else { return }
        didShowAutomaticOuterShellUpdatePrompt = true
        pendingOuterShellUpdate = PendingOuterShellUpdate(backend: backend,
                                                          installedVersion: backend.installedVersion ?? "",
                                                          availableVersion: availableVersion,
                                                          message: "Outer Shell \(availableVersion) is available.")
        backendError = ""
        scheduleLayoutUpdate()
    }

    private func fetchRecipes(didRecoverNetworking: Bool = false) {
        guard !isLoadingRecipes, let recipesEndpoint, let urlSession else { return }
        isLoadingRecipes = true
        urlSession.dataTask(with: recipesEndpoint) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isLoadingRecipes = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                        self.fetchRecipes(didRecoverNetworking: true)
                        return
                    }
                    if self.mode == .create {
                        self.scheduleLayoutUpdate()
                    }
                    return
                }
                guard let data else {
                    if self.mode == .create {
                        self.scheduleLayoutUpdate()
                    }
                    return
                }
                do {
                    let response = try RecipesResponse.decodeBinary(data)
                    self.recipes = response.recipes
                    if !self.recipes.contains(where: { $0.identifier == self.selectedRecipeID }) {
                        self.selectedRecipeID = self.recipes.first?.identifier ?? "command-port"
                    }
                    self.applyRecipeDefaults(overwrite: false)
                    if self.mode == .create {
                        self.scheduleLayoutUpdate()
                    }
                } catch {
                    if self.mode == .create {
                        self.scheduleLayoutUpdate()
                    }
                }
            }
        }.resume()
    }

    private func fetchSelectedLog(quiet: Bool = false, scrollToBottom: Bool = false, didRecoverNetworking: Bool = false) {
        if let context = selectedContainerLogContext {
            guard !isLoadingLog, !isPerformingWorkspaceOperation else { return }
            isLoadingLog = true
            if !quiet {
                logError = ""
                logScroll = 0
                renderLogHeader()
                renderLogRows()
            }
            sendWorkspaceRequest(operation: "appLogs",
                                 workspaceID: context.container.id,
                                 serviceID: context.app.serviceID)
            return
        }
        guard let selection = selectedLog, let logsEndpoint, let urlSession else { return }
        if isLoadingLog { return }
        let shouldFollowLogTail = quiet && isLogScrolledNearBottom()
        isLoadingLog = true
        if !quiet {
            logError = ""
            logScroll = 0
            renderLogHeader()
            renderLogRows()
        }
        var components = URLComponents(url: logsEndpoint, resolvingAgainstBaseURL: false)
        var items = [
            URLQueryItem(name: "serviceID", value: selection.serviceID),
            URLQueryItem(name: "bytes", value: "262144")
        ]
        if let logFile = logFile(for: selection), !logFile.path.isEmpty {
            items.append(URLQueryItem(name: "path", value: logFile.path))
        } else {
            items.append(URLQueryItem(name: "logIndex", value: String(selection.logIndex)))
        }
        components?.queryItems = items
        guard let url = components?.url else {
            isLoadingLog = false
            logError = "Could not build log request."
            updateLayout()
            return
        }
        urlSession.dataTask(with: url) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                guard self.selectedLog == selection else { return }
                self.isLoadingLog = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                        self.fetchSelectedLog(quiet: quiet,
                                              scrollToBottom: scrollToBottom,
                                              didRecoverNetworking: true)
                        return
                    }
                    self.logError = error.localizedDescription
                    self.updateLayout()
                    return
                }
                guard let data else {
                    self.logError = "Logs API returned no data."
                    self.updateLayout()
                    return
                }
                do {
                    let snapshot = try LogResponse.decodeBinary(data)
                    self.logSnapshot = snapshot
                    self.logError = snapshot.error
                    if scrollToBottom || shouldFollowLogTail {
                        self.shouldScrollLogToBottomOnNextLayout = true
                    }
                    self.clampScrollOffsets()
                    self.updateLayout()
                } catch {
                    self.logError = "Could not decode log response."
                    self.updateLayout()
                }
            }
        }.resume()
    }

    private func performControlAction(for backend: BackendRecord,
                                      operation: String,
                                      sudoPassword: String? = nil,
                                      didRecoverNetworking: Bool = false) {
        guard !isPerformingAction, let controlEndpoint, let urlSession else { return }
        isPerformingAction = true
        backendError = actionProgressText(operation: operation, backend: backend)
        if sudoPassword != nil {
            blurPasswordField()
            pendingPasswordAction = nil
            sudoPasswordInput = ""
            sudoPasswordMessage = ""
        }
        updateLayout()

        var components = URLComponents(url: controlEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "serviceID", value: backend.serviceID),
            URLQueryItem(name: "scope", value: backend.serviceScope),
            URLQueryItem(name: "operation", value: operation)
        ]
        guard let url = components?.url else {
            isPerformingAction = false
            backendError = "Could not build control request."
            updateLayout()
            return
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        if let sudoPassword {
            request.setValue("application/x-www-form-urlencoded; charset=utf-8", forHTTPHeaderField: "Content-Type")
            request.httpBody = formEncodedBody(["sudoPassword": sudoPassword])
        }
        urlSession.dataTask(with: request) { [weak self] data, urlResponse, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                var actionCompleted = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                        self.performControlAction(for: backend,
                                                  operation: operation,
                                                  sudoPassword: sudoPassword,
                                                  didRecoverNetworking: true)
                        return
                    }
                    self.backendError = error.localizedDescription
                } else if let data,
                          let response = try? ActionResponse.decodeBinary(data) {
                    if response.needsPassword == true {
                        self.showPasswordPrompt(for: backend, operation: operation, message: response.message)
                        self.backendError = ""
                    } else if operation == "checkUpdate", response.ok, response.updateAvailable {
                        self.pendingOuterShellUpdate = PendingOuterShellUpdate(backend: backend,
                                                                               installedVersion: response.installedVersion,
                                                                               availableVersion: response.availableVersion,
                                                                               message: response.message)
                        self.backendError = ""
                    } else {
                        self.backendError = response.ok ? (operation == "checkUpdate" ? response.message : "") : response.message
                        actionCompleted = response.ok
                    }
                } else if let data,
                          let message = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines),
                          !message.isEmpty {
                    self.backendError = message
                } else {
                    let statusCode = (urlResponse as? HTTPURLResponse)?.statusCode
                    self.backendError = statusCode.map { "Control request failed with HTTP \($0)." } ?? "Control request failed."
                }
                if actionCompleted {
                    self.applyOptimisticStatus(for: backend.serviceID,
                                               serviceScope: backend.serviceScope,
                                               operation: operation)
                    self.scheduleBackendsRefreshes()
                    if operation == "uninstallOuterShell",
                       let url = self.outerframeHost.pluginURL() {
                        self.outerframeHost.navigate(to: url)
                        return
                    }
                    if operation == "update" {
                        self.reloadOuterShellAfterUpdate()
                        return
                    }
                }
                self.fetchBackends()
            }
        }.resume()
    }

    private func reloadOuterShellAfterUpdate() {
        let url = outerShellReloadURL() ?? outerframeHost.pluginURL()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.25) { [weak self] in
            guard let self, let url else { return }
            self.outerframeHost.navigate(to: url)
        }
    }

    private func outerShellReloadURL() -> URL? {
        guard let currentURL = outerframeHost.pluginURL(),
              var components = URLComponents(url: currentURL, resolvingAgainstBaseURL: false) else {
            return outerframeHost.pluginURL()
        }
        var queryItems = components.queryItems ?? []
        queryItems.removeAll { $0.name == "_outerShellReload" }
        queryItems.append(URLQueryItem(name: "_outerShellReload", value: String(Int(Date().timeIntervalSince1970 * 1000))))
        components.queryItems = queryItems
        return components.url ?? currentURL
    }

    private func controlContainerApp(_ context: ContainerAppLauncherContext,
                                     operation: String) {
        let action: String
        switch operation {
        case "startApp":
            action = "Starting"
        case "stopApp":
            action = "Stopping"
        case "restartApp":
            action = "Restarting"
        default:
            return
        }
        workspacePanelMessage = "\(action) \(context.app.displayName)…"
        sendWorkspaceRequest(operation: operation,
                             workspaceID: context.container.id,
                             serviceID: context.app.serviceID)
    }

    private func scheduleBackendsRefreshes() {
        backendsRefreshGeneration += 1
        let generation = backendsRefreshGeneration
        for delay in [0.6, 1.8, 4.0, 8.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                Task { @MainActor in
                    guard let self, self.backendsRefreshGeneration == generation else { return }
                    self.fetchBackends(quiet: true)
                }
            }
        }
    }

    private func applyOptimisticStatus(for serviceID: String, serviceScope: String, operation: String) {
        let status: String?
        switch operation {
        case "stop":
            status = "stopped"
        case "start", "restart", "run", "install", "runUser", "installUser", "runRoot", "installRoot", "addRootSupport":
            status = "running"
        case "removeRootSupport":
            status = nil
        default:
            return
        }
        backends = backends.map { backend in
            guard backend.serviceID == serviceID,
                  backend.serviceScope == serviceScope else { return backend }
            return BackendRecord(serviceID: backend.serviceID,
                                 displayName: backend.displayName,
                                 serviceUnit: backend.serviceUnit,
                                 serviceUnitPath: backend.serviceUnitPath,
                                 serviceScope: backend.serviceScope,
                                 status: status ?? backend.status,
                                 canControl: backend.canControl,
                                 canUninstall: backend.canUninstall,
                                 isBundled: backend.isBundled,
                                 isInstalled: operation == "run" || operation == "install" || operation == "runUser" || operation == "installUser" || operation == "runRoot" || operation == "installRoot" ? true : backend.isInstalled,
                                 isMigration: backend.isMigration,
                                 supportsRoot: backend.supportsRoot,
                                 rootOnly: backend.rootOnly,
                                 hasRootSupport: operation == "runRoot" || operation == "installRoot" || operation == "addRootSupport" ? true : (operation == "removeRootSupport" ? false : backend.hasRootSupport),
                                 installedVersion: backend.installedVersion,
                                 availableVersion: backend.availableVersion,
                                 scriptPath: backend.scriptPath,
                                 publicBaseURL: backend.publicBaseURL,
                                 iconSymbolName: backend.iconSymbolName,
                                 launchdPlistPath: backend.launchdPlistPath,
                                 ownsLaunchdPlist: backend.ownsLaunchdPlist,
                                 menuBarVisibilityEnabled: backend.menuBarVisibilityEnabled,
                                 menuBarVisibilityAvailable: backend.menuBarVisibilityAvailable,
                                 frontends: backend.frontends,
                                 logFiles: backend.logFiles)
        }
        updateLayout()
    }

    private func openLauncherItem(_ item: AppLauncherItem, opensInNewTab: Bool) {
        if let context = item.containerContext {
            openWorkspaceApp(context.app,
                             in: context.container,
                             opensInNewTab: opensInNewTab)
            return
        }
        openLauncherEndpoint(item.primaryEndpoint, displayName: item.displayName, opensInNewTab: opensInNewTab)
    }

    private func openLauncherEndpoint(_ endpoint: AppLauncherEndpoint,
                                      displayName: String,
                                      opensInNewTab: Bool,
                                      opensInNewWindow: Bool = false) {
        if !endpointIsReadyToOpen(endpoint) {
            startAndOpenLauncherEndpoint(endpoint,
                                         displayName: displayName,
                                         opensInNewTab: opensInNewTab,
                                         opensInNewWindow: opensInNewWindow)
            return
        }
        guard launcherNavigationURL(endpoint) != nil else {
            startAndOpenLauncherEndpoint(endpoint,
                                         displayName: displayName,
                                         opensInNewTab: opensInNewTab,
                                         opensInNewWindow: opensInNewWindow)
            return
        }

        navigateToLauncherEndpoint(endpoint,
                                   displayName: displayName,
                                   opensInNewTab: opensInNewTab,
                                   opensInNewWindow: opensInNewWindow)
        scheduleEndpointActivationStateInvalidationIfNeeded(for: endpoint)
    }

    private func navigateToLauncherEndpoint(_ endpoint: AppLauncherEndpoint,
                                            displayName: String,
                                            opensInNewTab: Bool,
                                            opensInNewWindow: Bool,
                                            publishedSocketPath: String? = nil) {
        let navigate: (String?) -> Void = { [weak self] publishedSocketPath in
            guard let self,
                  let url = self.launcherNavigationURL(
                    endpoint,
                    socketPathOverride: publishedSocketPath
                  ) else {
                return
            }
            if opensInNewWindow {
                self.outerframeHost.openNewWindow(with: url,
                                                  displayString: displayName,
                                                  preferredSize: nil)
            } else if opensInNewTab {
                self.outerframeHost.openNewTab(with: url, displayString: displayName)
            } else {
                self.outerframeHost.navigate(to: url)
            }
        }

        if let publishedSocketPath {
            navigate(publishedSocketPath)
            return
        }
        let socketPath = endpoint.frontend.socketPath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !socketPath.isEmpty else {
            navigate(nil)
            return
        }
        navigate(nil)
    }

    private func requestPublishedWorkspaceSocket(socketPath: String,
                                                 workspaceID: UUID? = nil,
                                                 completion: @escaping (String?, String?) -> Void) {
        let requestID = UUID()
        pendingWorkspaceSocketPublications[requestID] = completion
        guard let safeSpacesEndpoint, let urlSession else {
            pendingWorkspaceSocketPublications.removeValue(forKey: requestID)
            completion(nil, "Outer Shell's container service is unavailable.")
            return
        }
        let apiRequest = SafeSpaceSocketAPIRequest(requestID: requestID,
                                                   operation: "publishSocket",
                                                   workspaceID: workspaceID,
                                                   socketPath: socketPath)
        guard let body = try? JSONEncoder().encode(apiRequest) else {
            pendingWorkspaceSocketPublications.removeValue(forKey: requestID)
            completion(nil, "Outer Shell could not encode the container socket request.")
            return
        }
        var urlRequest = URLRequest(url: safeSpacesEndpoint)
        urlRequest.httpMethod = "POST"
        urlRequest.httpBody = body
        urlRequest.setValue("application/json", forHTTPHeaderField: "Content-Type")
        urlSession.dataTask(with: urlRequest) { [weak self] data, response, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if let error {
                    let callback = self.pendingWorkspaceSocketPublications
                        .removeValue(forKey: requestID)
                    callback?(nil, error.localizedDescription)
                    return
                }
                if
                    (response as? HTTPURLResponse).map({ !(200..<300).contains($0.statusCode) }) == true ||
                    data == nil {
                    let callback = self.pendingWorkspaceSocketPublications
                        .removeValue(forKey: requestID)
                    callback?(nil, "Outer Shell's container service returned an invalid response.")
                    return
                }
                self.handlePublishedWorkspaceSocketResponse(data ?? Data())
            }
        }.resume()
    }

    private func openWorkspaceApp(_ app: LocalWorkspaceAppRecord,
                                  in workspace: LocalWorkspaceRecord,
                                  opensInNewTab: Bool,
                                  opensInNewWindow: Bool = false) {
        guard !isPerformingAction else { return }
        let displayName = app.displayName.isEmpty ? app.serviceID : app.displayName
        let endpointUserName = app.socketPath.hasPrefix("/run/user/0/")
            ? "root"
            : "workspace"
        let endpointDisplayName = "\(workspace.name) / \(endpointUserName) / \(displayName)"
        isPerformingAction = true
        backendError = displayedWorkspaceState(for: workspace) == "running"
            ? "Opening \(displayName)…"
            : "Starting container \(workspace.name) and opening \(displayName)…"
        updateLayout()
        if app.publishedPort > 0 {
            let path = pathAndQuery(fromFrontendURL: app.url,
                                    socketPath: app.socketPath)
            guard let url = URL(string: "http://127.0.0.1:\(app.publishedPort)\(path)") else {
                isPerformingAction = false
                backendError = "The app returned an invalid published port address."
                updateLayout()
                return
            }
            isPerformingAction = false
            backendError = ""
            updateLayout()
            navigateToWorkspaceApp(workspaceAppNavigationURL(url, app: app),
                                   displayName: endpointDisplayName,
                                   opensInNewTab: opensInNewTab,
                                   opensInNewWindow: opensInNewWindow)
            return
        }
        requestPublishedWorkspaceSocket(socketPath: app.socketPath,
                                        workspaceID: workspace.id,
                                        completion: { [weak self] publishedSocketPath, publicationError in
            guard let self else { return }
            self.isPerformingAction = false
            guard let publishedSocketPath else {
                self.backendError = publicationError ??
                    "Could not open \(displayName) in \(workspace.name)."
                self.updateLayout()
                return
            }
            let path = self.pathAndQuery(fromFrontendURL: app.url,
                                         socketPath: app.socketPath)
            guard let url = URL(string:
                "http+unix://\(self.percentEncodedSocketPath(publishedSocketPath))\(path)"
            ) else {
                self.backendError = "The app returned an invalid container address."
                self.updateLayout()
                return
            }
            self.backendError = ""
            self.updateLayout()
            let navigationURL = self.workspaceAppNavigationURL(url, app: app)
            self.navigateToWorkspaceApp(navigationURL,
                                        displayName: endpointDisplayName,
                                        opensInNewTab: opensInNewTab,
                                        opensInNewWindow: opensInNewWindow)
        })
    }

    private func navigateToWorkspaceApp(_ url: URL,
                                        displayName: String,
                                        opensInNewTab: Bool,
                                        opensInNewWindow: Bool) {
        if opensInNewWindow {
            outerframeHost.openNewWindow(with: url,
                                         displayString: displayName,
                                         preferredSize: nil)
        } else if opensInNewTab {
            outerframeHost.openNewTab(with: url,
                                      displayString: displayName)
        } else {
            outerframeHost.navigate(to: url,
                                    displayString: displayName)
        }
    }

    private func workspaceAppNavigationURL(_ targetURL: URL,
                                           app: LocalWorkspaceAppRecord) -> URL {
        if app.iconURL != nil || app.iconData?.isEmpty == false {
            return targetURL
        }
        guard !app.iconObservationToken.isEmpty else {
            return targetURL
        }
        guard let backendsEndpoint,
              var callbackComponents = URLComponents(
                url: backendsEndpoint,
                resolvingAgainstBaseURL: false
              ) else {
            return targetURL
        }
        callbackComponents.path = "/api/icon-observation"
        callbackComponents.queryItems = [
            URLQueryItem(name: "token", value: app.iconObservationToken)
        ]
        guard let callbackURL = callbackComponents.url,
              let callbackScheme = callbackURL.scheme?.lowercased(),
              ["http", "https"].contains(callbackScheme) else {
            return targetURL
        }

        var observationComponents = URLComponents()
        observationComponents.scheme = "outerloop"
        observationComponents.host = "observe-page-icon"
        observationComponents.queryItems = [
            URLQueryItem(name: "url", value: targetURL.absoluteString),
            URLQueryItem(name: "callback", value: callbackURL.absoluteString)
        ]
        return observationComponents.url ?? targetURL
    }

    private func handlePublishedWorkspaceSocketResponse(_ payload: Data) {
        guard let response = try? JSONDecoder().decode(
            PublishWorkspaceSocketHostResponse.self,
            from: payload
        ), let completion = pendingWorkspaceSocketPublications.removeValue(forKey: response.requestID) else {
            return
        }
        completion(response.publishedSocketPath, response.error)
    }

    private func scheduleEndpointActivationStateInvalidationIfNeeded(for endpoint: AppLauncherEndpoint) {
        let socketPath = endpoint.frontend.socketPath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !socketPath.isEmpty || endpoint.frontend.port > 0 else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            self?.invalidateBackendStateAfterEndpointActivation()
        }
    }

    private func invalidateBackendStateAfterEndpointActivation(didRecoverNetworking: Bool = false) {
        guard let controlEndpoint, let urlSession else { return }
        var components = URLComponents(url: controlEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "serviceID", value: "org.outershell.OuterShell"),
            URLQueryItem(name: "operation", value: "invalidateBackendState")
        ]
        guard let url = components?.url else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        urlSession.dataTask(with: request) { [weak self] _, _, error in
            guard let self, let error, !didRecoverNetworking else { return }
            Task { @MainActor in
                if self.recoverNetworkingIfNeeded(after: error) {
                    self.invalidateBackendStateAfterEndpointActivation(didRecoverNetworking: true)
                }
            }
        }.resume()
    }

    private func startAndOpenLauncherEndpoint(_ endpoint: AppLauncherEndpoint,
                                              displayName: String,
                                              opensInNewTab: Bool,
                                              opensInNewWindow: Bool,
                                              didRecoverNetworking: Bool = false) {
        guard !isPerformingAction, let controlEndpoint, let urlSession else { return }
        isPerformingAction = true
        backendError = "Starting \(displayName)..."
        updateLayout()

        var components = URLComponents(url: controlEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "serviceID", value: endpoint.backend.serviceID),
            URLQueryItem(name: "scope", value: endpoint.backend.serviceScope),
            URLQueryItem(name: "operation", value: "start")
        ]
        guard let url = components?.url else {
            isPerformingAction = false
            backendError = "Could not build frontend URL."
            updateLayout()
            return
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        urlSession.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                        self.startAndOpenLauncherEndpoint(endpoint,
                                                          displayName: displayName,
                                                          opensInNewTab: opensInNewTab,
                                                          opensInNewWindow: opensInNewWindow,
                                                          didRecoverNetworking: true)
                        return
                    }
                    self.backendError = error.localizedDescription
                    self.updateLayout()
                    return
                }
                if let data,
                   let response = try? ActionResponse.decodeBinary(data),
                   !response.ok {
                    self.backendError = response.message
                    self.updateLayout()
                    return
                }
                self.waitForLauncherEndpoint(endpoint,
                                             displayName: displayName,
                                             opensInNewTab: opensInNewTab,
                                             opensInNewWindow: opensInNewWindow,
                                             attempt: 0)
            }
        }.resume()
    }

    private func waitForLauncherEndpoint(_ endpoint: AppLauncherEndpoint,
                                         displayName: String,
                                         opensInNewTab: Bool,
                                         opensInNewWindow: Bool,
                                         attempt: Int,
                                         didRecoverNetworking: Bool = false) {
        guard let backendsEndpoint, let urlSession else { return }
        backendError = "Waiting for \(displayName)..."
        updateLayout()
        urlSession.dataTask(with: backendsEndpoint) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                if let error, !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                    self.waitForLauncherEndpoint(endpoint,
                                                 displayName: displayName,
                                                 opensInNewTab: opensInNewTab,
                                                 opensInNewWindow: opensInNewWindow,
                                                 attempt: attempt,
                                                 didRecoverNetworking: true)
                    return
                }
                if let data,
                   let response = try? BackendsResponse.decodeBinary(data) {
                    self.backends = response.backends
                    if let nextEndpoint = self.findLauncherEndpoint(serviceID: endpoint.backend.serviceID,
                                                                    serviceScope: endpoint.backend.serviceScope,
                                                                    frontendID: endpoint.frontend.id),
                       self.launcherNavigationURL(nextEndpoint) != nil,
                       self.endpointIsReadyToOpen(nextEndpoint) {
                        self.backendError = ""
                        self.updateLayout()
                        self.navigateToLauncherEndpoint(nextEndpoint,
                                                        displayName: displayName,
                                                        opensInNewTab: opensInNewTab,
                                                        opensInNewWindow: opensInNewWindow)
                        return
                    }
                }
                guard attempt < 30 else {
                    self.backendError = "Timed out waiting for \(displayName)."
                    self.updateLayout()
                    return
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                    self.waitForLauncherEndpoint(endpoint,
                                                 displayName: displayName,
                                                 opensInNewTab: opensInNewTab,
                                                 opensInNewWindow: opensInNewWindow,
                                                 attempt: attempt + 1)
                }
            }
        }.resume()
    }

    private func findLauncherEndpoint(serviceID: String, serviceScope: String, frontendID: String) -> AppLauncherEndpoint? {
        for backend in backends where backend.serviceID == serviceID && backend.serviceScope == serviceScope {
            for (index, frontend) in backend.frontends.enumerated() where frontend.id == frontendID {
                return AppLauncherEndpoint(backend: backend, frontend: frontend, frontendIndex: index)
            }
        }
        return nil
    }

    private func plaintextEndpoint(for serviceScope: String) -> AppLauncherEndpoint? {
        guard let item = appLauncherItems(from: backends).first(where: { $0.backend.serviceID == "org.outershell.Plaintext" && $0.backend.serviceScope == serviceScope }) else {
            return nil
        }
        if serviceScope == "system" {
            return item.rootEndpoint
        }
        return item.userEndpoint
    }

    private func endpointForScriptOperation(_ item: AppLauncherItem, serviceScope: String) -> AppLauncherEndpoint? {
        serviceScope == "system" ? item.rootEndpoint : item.userEndpoint
    }

    private func copyTextToPasteboard(_ text: String) {
        let item = OuterframeContentPasteboardItem(representations: [
            OuterframeContentPasteboardRepresentation(typeIdentifier: NSPasteboard.PasteboardType.string.rawValue,
                                                      data: Data(text.utf8))
        ])
        outerframeHost.requestPasteboardWrite(items: [item])
    }

    private func plaintextURL(for endpoint: AppLauncherEndpoint, filePath: String) -> URL? {
        guard var components = frontendNavigationURL(endpoint).flatMap({ URLComponents(url: $0, resolvingAgainstBaseURL: false) }) else {
            return nil
        }
        var queryItems = components.queryItems ?? []
        queryItems.removeAll { $0.name == "file" }
        queryItems.append(URLQueryItem(name: "file", value: filePath))
        components.queryItems = queryItems
        return components.url
    }

    private func editScript(_ path: String, serviceScope: String) {
        guard let endpoint = plaintextEndpoint(for: serviceScope),
              let url = plaintextURL(for: endpoint, filePath: path) else {
            backendError = serviceScope == "system"
                ? "Install root Plaintext to edit this script."
                : "Install Plaintext to edit this script."
            updateLayout()
            return
        }
        outerframeHost.openNewTab(with: url, displayString: "Plaintext")
    }

    private func backendForShowLogsOperation(_ operation: String) -> BackendRecord? {
        let value = String(operation.dropFirst("showLogs:".count))
        let pieces = value.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)
        if pieces.count == 2 {
            let serviceID = String(pieces[0])
            let serviceScope = String(pieces[1])
            return backends.first { $0.serviceID == serviceID && $0.serviceScope == serviceScope }
        }
        return backends.first { $0.serviceID == value }
    }

    private func performAppMenuAction(_ item: AppLauncherItem, operation: String) {
        if operation == "alwaysShow" {
            moveOverviewItem(item, pinned: true)
            return
        }
        if operation == "moveToMoreApps" {
            moveOverviewItem(item, pinned: false)
            return
        }
        if let context = item.containerContext {
            switch operation {
            case "containerOpen":
                openWorkspaceApp(context.app,
                                 in: context.container,
                                 opensInNewTab: false)
            case "containerOpenNewTab":
                openWorkspaceApp(context.app,
                                 in: context.container,
                                 opensInNewTab: true)
            case "containerOpenNewWindow":
                openWorkspaceApp(context.app,
                                 in: context.container,
                                 opensInNewTab: false,
                                 opensInNewWindow: true)
            case "containerStart":
                controlContainerApp(context, operation: "startApp")
            case "containerStop":
                controlContainerApp(context, operation: "stopApp")
            case "containerRestart":
                controlContainerApp(context, operation: "restartApp")
            case "containerLogs":
                showContainerLogs(context)
            default:
                break
            }
            return
        }
        if operation.hasPrefix("showLogs:") {
            guard let backend = backendForShowLogsOperation(operation) else { return }
            showLogs(for: backend)
            return
        }
        if operation.hasPrefix("start:") || operation.hasPrefix("stop:") {
            let controlOperation = operation.hasPrefix("start:") ? "start" : "stop"
            let prefix = "\(controlOperation):"
            let scope = String(operation.dropFirst(prefix.count))
            let endpoint = scope == "system" ? item.rootEndpoint : item.userEndpoint
            guard let endpoint else { return }
            performControlAction(for: endpoint.backend, operation: controlOperation)
            return
        }
        if operation.hasPrefix("editScript:") {
            let scope = String(operation.dropFirst("editScript:".count))
            guard let endpoint = endpointForScriptOperation(item, serviceScope: scope),
                  let scriptPath = endpoint.backend.scriptPath?.trimmingCharacters(in: .whitespacesAndNewlines),
                  !scriptPath.isEmpty else { return }
            editScript(scriptPath, serviceScope: scope)
            return
        }
        if operation.hasPrefix("copyScriptPath:") {
            let scope = String(operation.dropFirst("copyScriptPath:".count))
            guard let endpoint = endpointForScriptOperation(item, serviceScope: scope),
                  let scriptPath = endpoint.backend.scriptPath?.trimmingCharacters(in: .whitespacesAndNewlines),
                  !scriptPath.isEmpty else { return }
            copyTextToPasteboard(scriptPath)
            return
        }

        switch operation {
        case "run":
            if let userEndpoint = item.userEndpoint {
                openLauncherEndpoint(userEndpoint, displayName: item.displayName, opensInNewTab: false)
            } else {
                performControlAction(for: item.backend, operation: "run")
            }
        case "runNewTab":
            if let userEndpoint = item.userEndpoint {
                openLauncherEndpoint(userEndpoint, displayName: item.displayName, opensInNewTab: true)
            } else {
                performControlAction(for: item.backend, operation: "run")
            }
        case "runNewWindow":
            if let userEndpoint = item.userEndpoint {
                openLauncherEndpoint(userEndpoint,
                                     displayName: item.displayName,
                                     opensInNewTab: false,
                                     opensInNewWindow: true)
            } else {
                performControlAction(for: item.backend, operation: "run")
            }
        case "runRoot":
            if let rootEndpoint = item.rootEndpoint {
                openLauncherEndpoint(rootEndpoint, displayName: item.displayName, opensInNewTab: false)
            } else {
                performControlAction(for: item.backend, operation: "runRoot")
            }
        case "runRootNewTab":
            if let rootEndpoint = item.rootEndpoint {
                openLauncherEndpoint(rootEndpoint, displayName: item.displayName, opensInNewTab: true)
            } else {
                performControlAction(for: item.backend, operation: "runRoot")
            }
        case "runRootNewWindow":
            if let rootEndpoint = item.rootEndpoint {
                openLauncherEndpoint(rootEndpoint,
                                     displayName: item.displayName,
                                     opensInNewTab: false,
                                     opensInNewWindow: true)
            } else {
                performControlAction(for: item.backend, operation: "runRoot")
            }
        default:
            performControlAction(for: item.backend, operation: operation)
        }
    }

    private func submitCreateForm() {
        if selectedCreateSection == .bashCommands {
            submitBashCommandsForm()
            return
        }
        if selectedCreateSection == .nativeApp {
            submitNativeAppForm()
            return
        }
        guard !isPerformingAction, let createEndpoint, let urlSession else { return }
        guard let recipe = selectedRecipe() else {
            createMessage = "Choose a recipe."
            updateLayout()
            return
        }
        let missing = visibleCreateFields(for: recipe).first { field in
            let value = createValue(for: field).trimmingCharacters(in: .whitespacesAndNewlines)
            if field.key == "port" { return false }
            return value.isEmpty && field.defaultValue.isEmpty
        }
        if let missing {
            createMessage = "\(missing.label) is required."
            updateLayout()
            return
        }
        performCreateRequest(recipe: recipe, createEndpoint: createEndpoint, urlSession: urlSession)
    }

    private func submitBashCommandsForm() {
        ensureBashDefaults()
        let displayName = createValues["bashDisplayName", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        if displayName.isEmpty {
            createMessage = "Display Name is required."
            updateLayout()
            return
        }
        let commands = createValues["bashCommands", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        if commands.isEmpty {
            createMessage = "Bash commands are required."
            updateLayout()
            return
        }
        let transport = createValues["bashFrontendTransport", default: "port"]
        if transport == "unixSocket" {
            let socketPath = createValues["bashSocketPath", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
            if socketPath.isEmpty {
                createMessage = "Socket Path is required."
                updateLayout()
                return
            }
        } else {
            let port = createValues["bashPort", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
            if port.isEmpty {
                createMessage = "Port is required."
                updateLayout()
                return
            }
        }
        performBashCreateRequest()
    }

    private func submitNativeAppForm() {
        guard !isPerformingAction else { return }
        ensureNativeAppDefaults()

        let appName = createValues["nativeAppName", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        let socketFilename = NativeAppProjectGenerator.safePathComponent(createValues["nativeSocketFilename", default: ""].trimmingCharacters(in: .whitespacesAndNewlines))
        let projectRoot = createValues["nativeProjectRoot", default: "~/outerframe-apps"].trimmingCharacters(in: .whitespacesAndNewlines)
        let projectFolder = NativeAppProjectGenerator.safePathComponent(createValues["nativeProjectFolder", default: ""].trimmingCharacters(in: .whitespacesAndNewlines))
        let appID = createValues["nativeAppID", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        let frontendLanguage = NativeAppProjectConfiguration.FrontendLanguage(rawValue: createValues["nativeFrontendLanguage", default: "swift"]) ?? .swift
        let backendLanguage = NativeAppProjectConfiguration.BackendLanguage(rawValue: createValues["nativeBackendLanguage", default: "go"]) ?? .go
        let isolationMode = NativeAppProjectConfiguration.IsolationMode(rawValue: createValues["nativeIsolationMode", default: "container"]) ?? .container
        var platformTargets: Set<NativeAppProjectConfiguration.PlatformTarget> = []
        if createValues["nativeTargetHTML", default: "true"] == "true" {
            platformTargets.insert(.html)
        }
        if createValues["nativeTargetMacOS", default: "true"] == "true" {
            platformTargets.insert(.macos)
        }
        let scheme = suggestedSwiftIdentifier(from: appName)

        if appName.isEmpty {
            createMessage = "Name is required."
            updateLayout()
            return
        }
        if appName.rangeOfCharacter(from: CharacterSet(charactersIn: "\"\\$`\n\r")) != nil {
            createMessage = "Name cannot contain quotes, backslashes, dollar signs, or backticks."
            updateLayout()
            return
        }
        if socketFilename.isEmpty {
            createMessage = "Socket Filename is required."
            updateLayout()
            return
        }
        if projectRoot.isEmpty {
            createMessage = "Project Location is required."
            updateLayout()
            return
        }
        if projectRoot != "~" && !projectRoot.hasPrefix("~/") && !projectRoot.hasPrefix("/") {
            createMessage = "Project Location must be an absolute path or start with ~/."
            updateLayout()
            return
        }
        if projectRoot.rangeOfCharacter(from: CharacterSet(charactersIn: "\"\\$`\n\r")) != nil {
            createMessage = "Project Location cannot contain quotes, backslashes, dollar signs, or backticks."
            updateLayout()
            return
        }
        if projectFolder.isEmpty {
            createMessage = "Project Folder is required."
            updateLayout()
            return
        }
        if appID.isEmpty {
            createMessage = "App ID is required."
            updateLayout()
            return
        }
        if platformTargets.isEmpty {
            createMessage = "Choose at least one platform."
            updateLayout()
            return
        }
        guard let stagingDirectory = outerframeHost.stagedFileDirectoryURL else {
            createMessage = NativeAppProjectGeneratorError.missingStagingDirectory.localizedDescription
            updateLayout()
            return
        }

        let configuration = NativeAppProjectConfiguration(appName: appName,
                                                          appID: appID,
                                                          xcodeScheme: scheme,
                                                          projectRootPath: projectRoot,
                                                          projectFolderName: projectFolder,
                                                          socketFilename: socketFilename,
                                                          platformTargets: platformTargets,
                                                          frontendLanguage: frontendLanguage,
                                                          backendLanguage: backendLanguage,
                                                          isolationMode: isolationMode)
        let iconPNGData: Data
        do {
            iconPNGData = try NativeAppProjectGenerator.generatedIconPNGData(appName: appName, appID: appID)
        } catch {
            createMessage = error.localizedDescription
            updateLayout()
            return
        }

        isPerformingAction = true
        createMessage = "Installing on server..."
        updateLayout()
        outerframeHost.requestOuterLoopSSHCommandArguments { [weak self] arguments in
            self?.installGeneratedNativeProject(configuration: configuration,
                                                iconPNGData: iconPNGData,
                                                stagingDirectory: stagingDirectory,
                                                sshCommandArguments: arguments ?? [])
        }
    }

    private func installGeneratedNativeProject(configuration: NativeAppProjectConfiguration,
                                               iconPNGData: Data,
                                               stagingDirectory: URL,
                                               sshCommandArguments: [String]) {
        guard let nativeAppProjectEndpoint, let nativeAppInstallSession else {
            isPerformingAction = false
            createMessage = "The Outer Shell backend cannot create server projects."
            updateLayout()
            return
        }

        let targets = NativeAppProjectConfiguration.PlatformTarget.allCases
            .filter { configuration.platformTargets.contains($0) }
            .map(\.rawValue)
            .joined(separator: " ")
        var form = URLComponents()
        form.queryItems = [
            URLQueryItem(name: "name", value: configuration.appName),
            URLQueryItem(name: "appID", value: configuration.appID),
            URLQueryItem(name: "scheme", value: configuration.xcodeScheme),
            URLQueryItem(name: "sourceRoot", value: configuration.projectRootPath),
            URLQueryItem(name: "folder", value: configuration.projectFolderName),
            URLQueryItem(name: "socket", value: configuration.socketFilename),
            URLQueryItem(name: "targets", value: targets),
            URLQueryItem(name: "macOSLanguage", value: configuration.frontendLanguage.rawValue),
            URLQueryItem(name: "backendLanguage", value: configuration.backendLanguage.rawValue),
            URLQueryItem(name: "isolation", value: configuration.isolationMode.rawValue)
        ]
        var request = URLRequest(url: nativeAppProjectEndpoint)
        request.httpMethod = "POST"
        request.timeoutInterval = 600
        request.setValue("application/vnd.outershell.native-app-project", forHTTPHeaderField: "Content-Type")
        guard let formData = form.percentEncodedQuery?.data(using: .utf8),
              formData.count <= Int(UInt32.max),
              iconPNGData.count <= Int(UInt32.max) else {
            isPerformingAction = false
            createMessage = "Native app project request is too large."
            updateLayout()
            return
        }
        var requestBody = Data([0x4f, 0x53, 0x4e, 0x52, 0x45, 0x51, 0x31, 0x00])
        var formLength = UInt32(formData.count).littleEndian
        var iconLength = UInt32(iconPNGData.count).littleEndian
        withUnsafeBytes(of: &formLength) { requestBody.append(contentsOf: $0) }
        withUnsafeBytes(of: &iconLength) { requestBody.append(contentsOf: $0) }
        requestBody.append(formData)
        requestBody.append(iconPNGData)
        request.httpBody = requestBody

        nativeAppInstallSession.dataTask(with: request) { [weak self] data, response, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                if let error {
                    self.createMessage = error.localizedDescription
                } else if let httpResponse = response as? HTTPURLResponse,
                          (200..<300).contains(httpResponse.statusCode) {
                    do {
                        guard let data, !data.isEmpty else {
                            throw NativeAppProjectGeneratorError.invalidResponse
                        }
                        self.generatedNativeProject = try NativeAppProjectGenerator.materializeProjectResponse(
                            data,
                            configuration: configuration,
                            stagingDirectory: stagingDirectory,
                            sshCommandArguments: sshCommandArguments
                        )
                        self.nativeProjectSelectionState = .none
                        self.generatedNativeProjectWasExported = false
                        self.createScroll = 0
                        self.createMessage = "Installed \(configuration.projectFolderName) on the server."
                        self.fetchBackends()
                    } catch {
                        self.discardGeneratedNativeProject()
                        self.createMessage = error.localizedDescription
                    }
                } else {
                    let message = data.flatMap { String(data: $0, encoding: .utf8) }?
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    self.createMessage = (message?.isEmpty == false) ? message! : "Server installation failed."
                }
                self.updateLayout()
            }
        }.resume()
    }

    private func beginDraggingGeneratedNativeProject(_ project: GeneratedNativeAppProject) {
        guard let projectURL = project.projectURL else { return }
        let promiseID = UUID()
        nativeFilePromiseURLs[promiseID] = projectURL
        let dragPreview = nativeProjectDragPreview(for: project)
        guard let pasteboardItem = outerframeHost.filePromisePasteboardItem(promiseID: promiseID,
                                                                            name: project.folderName,
                                                                            fileType: "public.folder") else {
            nativeFilePromiseURLs.removeValue(forKey: promiseID)
            return
        }
        outerframeHost.beginDraggingPasteboardItem(pasteboardItem,
                                                   operationMask: .copy,
                                                   previewPNGData: dragPreview?.pngData,
                                                   previewSize: dragPreview?.size,
                                                   previewFrameOrigin: dragPreview?.frameOrigin)
    }

    private func beginDraggingSharedContainer() {
        guard let file = sharedContainerFile else { return }
        guard let urlData = file.url.absoluteString.data(using: .utf8) else { return }
        let pasteboardItem = OuterContentPasteboardItem(representations: [
            OuterContentPasteboardRepresentation(
                typeIdentifier: NSPasteboard.PasteboardType.fileURL.rawValue,
                data: urlData
            )
        ])
        let dragPreview = sharedContainerDragPreview(for: file)
        outerframeHost.beginDraggingPasteboardItem(
            pasteboardItem,
            operationMask: .copy,
            previewPNGData: dragPreview?.pngData,
            previewSize: dragPreview?.size,
            previewFrameOrigin: dragPreview?.frameOrigin
        )
    }

    private func dismissSharedContainerPanel() {
        if let url = sharedContainerFile?.url {
            try? FileManager.default.removeItem(at: url.deletingLastPathComponent())
        }
        sharedContainerFile = nil
        sharedContainerDragFrame = .zero
        sharedContainerCloseFrame = .zero
        pendingSharedContainerDrag = false
        isShowingWorkspacePanel = false
        updateLayout()
    }

    private func handleFilePromiseWriteRequest(requestID: UUID, promiseID: UUID) {
        guard let url = nativeFilePromiseURLs.removeValue(forKey: promiseID) else {
            outerframeHost.sendFilePromiseWriteFailure(requestID: requestID,
                                                       promiseID: promiseID,
                                                       errorMessage: "Unknown file promise.")
            return
        }
        do {
            try NativeAppProjectGenerator.makeProjectWritable(url.deletingLastPathComponent())
            try NativeAppProjectGenerator.makeProjectWritable(url)
            outerframeHost.sendFilePromiseWriteResponse(requestID: requestID,
                                                        promiseID: promiseID,
                                                        localPath: url.path,
                                                        deleteWhenDone: true)
            generatedNativeProjectWasExported = true
            nativeProjectSelectionState = .none
            nativeProjectDragFrame = .zero
            updateLayout()
        } catch {
            outerframeHost.sendFilePromiseWriteFailure(requestID: requestID,
                                                       promiseID: promiseID,
                                                       errorMessage: error.localizedDescription)
        }
    }

    private func performCreateRequest(recipe: RecipeRecord,
                                      createEndpoint: URL,
                                      urlSession: URLSession,
                                      didRecoverNetworking: Bool = false) {
        isPerformingAction = true
        createMessage = "Creating..."
        updateLayout()
        var components = URLComponents(url: createEndpoint, resolvingAgainstBaseURL: false)
        var queryItems = [URLQueryItem(name: "recipe", value: recipe.identifier)]
        for field in recipe.fields {
            if isCreateFieldHidden(field, in: recipe) {
                continue
            }
            queryItems.append(URLQueryItem(name: field.key, value: createValue(for: field).trimmingCharacters(in: .whitespacesAndNewlines)))
        }
        components?.queryItems = queryItems
        guard let url = components?.url else {
            isPerformingAction = false
            createMessage = "Could not build create request."
            updateLayout()
            return
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        urlSession.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error),
                       let urlSession = self.urlSession {
                        self.performCreateRequest(recipe: recipe,
                                                  createEndpoint: createEndpoint,
                                                  urlSession: urlSession,
                                                  didRecoverNetworking: true)
                        return
                    }
                    self.createMessage = error.localizedDescription
                } else if let data,
                          let response = try? ActionResponse.decodeBinary(data) {
                        self.createMessage = response.message
                        if response.ok {
                            self.createValues.removeAll()
                            self.applyRecipeDefaults(overwrite: true)
                            self.pendingFilePicker = nil
                            self.navigateToMode(.apps, pushHistory: false)
                            self.fetchBackends()
                        }
                } else {
                    self.createMessage = "Create request failed."
                }
                self.updateColors()
            }
        }.resume()
    }

    private func performBashCreateRequest(didRecoverNetworking: Bool = false) {
        guard !isPerformingAction, let createEndpoint, let urlSession else { return }
        ensureBashDefaults()
        isPerformingAction = true
        createMessage = "Creating..."
        updateLayout()

        let identifier = normalizedBashIdentifier()
        let transport = createValues["bashFrontendTransport", default: "port"]
        var queryItems: [URLQueryItem] = [
            URLQueryItem(name: "recipe", value: "command-port"),
            URLQueryItem(name: "command", value: createValues["bashCommands", default: ""]),
            URLQueryItem(name: "workdir", value: "~"),
            URLQueryItem(name: "frontendTransport", value: transport),
            URLQueryItem(name: "name", value: createValues["bashDisplayName", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)),
            URLQueryItem(name: "identifier", value: identifier)
        ]
        if transport == "unixSocket" {
            queryItems.append(URLQueryItem(name: "socketPath", value: createValues["bashSocketPath", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)))
        } else {
            queryItems.append(URLQueryItem(name: "port", value: createValues["bashPort", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)))
        }
        let iconPath = createValues["bashIconPath", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        if !iconPath.isEmpty {
            queryItems.append(URLQueryItem(name: "iconPath", value: iconPath))
        }

        var components = URLComponents(url: createEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = queryItems
        guard let url = components?.url else {
            isPerformingAction = false
            createMessage = "Could not build create request."
            updateLayout()
            return
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        urlSession.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                        self.performBashCreateRequest(didRecoverNetworking: true)
                        return
                    }
                    self.createMessage = error.localizedDescription
                } else if let data,
                          let response = try? ActionResponse.decodeBinary(data) {
                    self.createMessage = response.message
                    if response.ok {
                        self.createValues.removeAll()
                        self.ensureBashDefaults()
                        self.pendingFilePicker = nil
                        self.navigateToMode(.apps, pushHistory: false)
                        self.fetchBackends()
                    }
                } else {
                    self.createMessage = "Create request failed."
                }
                self.updateColors()
            }
        }.resume()
    }

    private func showBashIconFilePicker() {
        blurCreateField()
        let current = createValues["bashIconPath", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        let directory = parentDirectoryForPath(current.isEmpty ? "~" : current)
        filePickerScroll = 0
        filePickerSelectedIndex = nil
        resetFilePickerTypeahead()
        pendingFilePicker = PendingFilePicker(mode: .chooseFile,
                                              targetFieldKey: "bashIconPath",
                                              directory: directory,
                                              parent: directory,
                                              entries: [],
                                              isLoading: true,
                                              error: "")
        fetchFilePickerDirectory(path: directory)
        updateLayout()
    }

    private func showDirectoryPicker(for key: String) {
        blurCreateField()
        let currentDirectory = createValues[key] ?? selectedRecipe()?.fields.first(where: { $0.key == key })?.defaultValue ?? "~"
        let directory = currentDirectory.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? "~" : currentDirectory
        filePickerScroll = 0
        filePickerSelectedIndex = nil
        resetFilePickerTypeahead()
        pendingFilePicker = PendingFilePicker(mode: .chooseDirectory,
                                              targetFieldKey: key,
                                              directory: directory,
                                              parent: directory,
                                              entries: [],
                                              isLoading: true,
                                              error: "")
        fetchFilePickerDirectory(path: directory)
        updateLayout()
    }

    private func fetchFilePickerDirectory(path: String, didRecoverNetworking: Bool = false) {
        guard let filePickerEndpoint, let urlSession else { return }
        if pendingFilePicker != nil {
            pendingFilePicker?.directory = path
            pendingFilePicker?.entries = []
            pendingFilePicker?.isLoading = true
            pendingFilePicker?.error = ""
            filePickerScroll = 0
            filePickerSelectedIndex = nil
            resetFilePickerTypeahead()
            updateLayout()
        }
        var components = URLComponents(url: filePickerEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "path", value: path),
            URLQueryItem(name: "extension", value: ""),
            URLQueryItem(name: "directoriesOnly", value: pendingFilePicker?.mode == .chooseDirectory ? "true" : "false")
        ]
        guard let url = components?.url else {
            pendingFilePicker?.isLoading = false
            pendingFilePicker?.error = "Could not build file request."
            updateLayout()
            return
        }
        urlSession.dataTask(with: url) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self, self.pendingFilePicker != nil else { return }
                self.pendingFilePicker?.isLoading = false
                if let error {
                    if !didRecoverNetworking, self.recoverNetworkingIfNeeded(after: error) {
                        self.fetchFilePickerDirectory(path: path, didRecoverNetworking: true)
                        return
                    }
                    self.pendingFilePicker?.error = error.localizedDescription
                    self.updateLayout()
                    return
                }
                guard let data else {
                    self.pendingFilePicker?.error = "No directory response."
                    self.updateLayout()
                    return
                }
                do {
                    let response = try FilePickerResponse.decodeBinary(data)
                    self.pendingFilePicker?.directory = response.path
                    self.pendingFilePicker?.parent = response.parent
                    self.pendingFilePicker?.entries = response.entries
                    self.filePickerSelectedIndex = response.entries.firstIndex(where: \.willCreate)
                    self.resetFilePickerTypeahead()
                } catch {
                    self.pendingFilePicker?.error = "Could not decode directory."
                }
                self.updateLayout()
            }
        }.resume()
    }

    private func confirmFilePickerSave() {
        guard let picker = pendingFilePicker,
              let targetFieldKey = picker.targetFieldKey else { return }
        if picker.mode == .chooseFile {
            guard let selectedIndex = filePickerSelectedIndex,
                  picker.entries.indices.contains(selectedIndex),
                  !picker.entries[selectedIndex].isDirectory else {
                pendingFilePicker?.error = "Choose a file."
                updateLayout()
                return
            }
            createValues[targetFieldKey] = picker.entries[selectedIndex].path
        } else {
            if let selectedIndex = filePickerSelectedIndex,
               picker.entries.indices.contains(selectedIndex),
               picker.entries[selectedIndex].isDirectory {
                createValues[targetFieldKey] = picker.entries[selectedIndex].path
            } else {
                createValues[targetFieldKey] = picker.directory
            }
        }
        pendingFilePicker = nil
        filePickerScroll = 0
        filePickerSelectedIndex = nil
        resetFilePickerTypeahead()
        focusCreateField(targetFieldKey, cursorPosition: createValues[targetFieldKey, default: ""].count)
        createMessage = ""
        updateLayout()
    }

    private func dismissFilePicker() {
        pendingFilePicker = nil
        filePickerScroll = 0
        filePickerSelectedIndex = nil
        resetFilePickerTypeahead()
        blurCreateField()
        updateLayout()
    }

    private func activateSelectedFilePickerEntry() -> Bool {
        guard let selectedIndex = filePickerSelectedIndex,
              let entries = pendingFilePicker?.entries,
              entries.indices.contains(selectedIndex) else {
            return false
        }
        activateFilePickerEntry(entries[selectedIndex])
        return true
    }

    private func activateFilePickerEntry(_ entry: FilePickerEntryRecord) {
        blurCreateField()
        if entry.willCreate,
           let targetFieldKey = pendingFilePicker?.targetFieldKey {
            createValues[targetFieldKey] = entry.path
            pendingFilePicker = nil
            filePickerScroll = 0
            filePickerSelectedIndex = nil
            resetFilePickerTypeahead()
            focusCreateField(targetFieldKey, cursorPosition: entry.path.count)
            createMessage = ""
            updateLayout()
        } else if entry.isDirectory {
            fetchFilePickerDirectory(path: entry.path)
        } else if pendingFilePicker?.mode == .chooseFile,
                  let targetFieldKey = pendingFilePicker?.targetFieldKey {
            createValues[targetFieldKey] = entry.path
            pendingFilePicker = nil
            filePickerScroll = 0
            filePickerSelectedIndex = nil
            resetFilePickerTypeahead()
            focusCreateField(targetFieldKey, cursorPosition: entry.path.count)
            createMessage = ""
            updateLayout()
        }
    }

    private func moveFilePickerSelection(delta: Int) {
        guard let entries = pendingFilePicker?.entries,
              !entries.isEmpty else { return }
        let nextIndex = min(max((filePickerSelectedIndex ?? (delta > 0 ? -1 : entries.count)) + delta, 0), entries.count - 1)
        selectFilePickerIndex(nextIndex)
    }

    private func selectFilePickerIndex(_ index: Int) {
        guard let entries = pendingFilePicker?.entries,
              entries.indices.contains(index) else { return }
        filePickerSelectedIndex = index
        ensureFilePickerSelectionVisible()
        updateFilePickerVisibleRows(rebuild: true)
    }

    private func ensureFilePickerSelectionVisible() {
        guard let selectedIndex = filePickerSelectedIndex else { return }
        let rowTop = CGFloat(selectedIndex) * filePickerRowHeight
        let viewportHeight = max(filePickerListFrame.height, 1)
        if rowTop < filePickerScroll {
            filePickerScroll = rowTop
        } else if rowTop + filePickerRowHeight > filePickerScroll + viewportHeight {
            filePickerScroll = rowTop + filePickerRowHeight - viewportHeight
        }
        let maxPickerScroll = max(filePickerContentHeight - filePickerListFrame.height, 0)
        filePickerScroll = min(max(filePickerScroll, 0), maxPickerScroll)
    }

    private func clampFilePickerSelection() {
        guard let count = pendingFilePicker?.entries.count,
              count > 0 else {
            filePickerSelectedIndex = nil
            return
        }
        if let selectedIndex = filePickerSelectedIndex,
           selectedIndex >= count {
            filePickerSelectedIndex = count - 1
        }
    }

    private func resetFilePickerTypeahead() {
        filePickerTypeaheadPrefix = ""
        filePickerTypeaheadLastUpdated = nil
    }

    private func handleFilePickerTypeahead(_ text: String) {
        guard let entries = pendingFilePicker?.entries,
              !entries.isEmpty else { return }
        let now = Date()
        if let last = filePickerTypeaheadLastUpdated,
           now.timeIntervalSince(last) > 1.0 {
            filePickerTypeaheadPrefix = ""
        }
        filePickerTypeaheadLastUpdated = now
        filePickerTypeaheadPrefix += text.lowercased()
        if selectFilePickerEntry(matchingPrefix: filePickerTypeaheadPrefix, in: entries, startingAfterSelection: false) {
            return
        }
        filePickerTypeaheadPrefix = text.lowercased()
        _ = selectFilePickerEntry(matchingPrefix: filePickerTypeaheadPrefix, in: entries, startingAfterSelection: true)
    }

    private func selectFilePickerEntry(matchingPrefix prefix: String, in entries: [FilePickerEntryRecord], startingAfterSelection: Bool) -> Bool {
        guard !prefix.isEmpty else { return false }
        let start = startingAfterSelection ? (filePickerSelectedIndex ?? -1) + 1 : 0
        for offset in 0..<entries.count {
            let index = (start + offset) % entries.count
            if entries[index].name.lowercased().hasPrefix(prefix) {
                selectFilePickerIndex(index)
                return true
            }
        }
        return false
    }

    private func filePickerTypeaheadText(from characters: String) -> String? {
        guard !characters.isEmpty,
              characters.unicodeScalars.allSatisfy({
                  !CharacterSet.controlCharacters.contains($0) &&
                  !CharacterSet.newlines.contains($0)
              }) else {
            return nil
        }
        return characters
    }

    func textInputControllerDidChangeState() {
        if isWorkspaceNamePromptVisible,
           workspaceRenameInputController.isFocused,
           !isSynchronizingWorkspaceRenameInput {
            workspaceRenameName = workspaceRenameInputController.text
            if isContainerConfigurationEditorVisible &&
                !isRenamingContainerConfiguration {
                switch containerConfigurationTab {
                case .dockerfile:
                    containerConfigurationDockerfile = workspaceRenameInputController.text
                case .environment:
                    containerConfigurationEnvironment = workspaceRenameInputController.text
                    scheduleContainerConfigurationEnvironmentSave()
                case .ports:
                    containerConfigurationPorts = workspaceRenameInputController.text
                    scheduleContainerConfigurationEnvironmentSave()
                case .mounts, .runtime:
                    break
                }
            }
            workspacePanelMessage = ""
            updateInputMode()
            updateEditingAndPasteboardState()
            updateLayout()
            return
        }

        if pendingPasswordAction != nil,
           passwordInputController.isFocused,
           !isSynchronizingPasswordInput {
            sudoPasswordInput = passwordInputController.text
            sudoPasswordMessage = ""
            updateInputMode()
            updateEditingAndPasteboardState()
            updateLayout()
            return
        }

        guard mode == .create,
              !isSynchronizingCreateInput,
              let key = activeCreateFieldKey else {
            updateInputMode()
            updateEditingAndPasteboardState()
            sendFocusedTextInputGeometryUpdate()
            return
        }

        let oldNameSuggestion = suggestedIdentifier(from: createValues["name", default: ""])
        let oldNativeName = createValues["nativeAppName", default: ""]
        let oldNativeFolder = suggestedProjectFolderName(from: oldNativeName.isEmpty ? "Hello World" : oldNativeName)
        let oldNativeScheme = suggestedSwiftIdentifier(from: oldNativeName.isEmpty ? "Hello World" : oldNativeName)
        let oldNativeAppID = createValues["nativeAppID", default: ""]
        createValues[key] = createInputController.text
        if key == "name" {
            let identifier = createValues["identifier", default: ""]
            if identifier.isEmpty || identifier == oldNameSuggestion {
                createValues["identifier"] = suggestedIdentifier(from: createValues["name", default: ""])
            }
        }
        if key == "bashDisplayName" {
            createValues["bashIdentifier"] = uniqueBashIdentifier(from: createValues["bashDisplayName", default: ""])
        } else if key == "bashIdentifier" {
            let sanitized = suggestedIdentifier(from: createInputController.text)
            if sanitized != createInputController.text {
                createValues["bashIdentifier"] = sanitized
            }
        } else if key == "nativeAppName" {
            let newFolder = suggestedProjectFolderName(from: createInputController.text)
            let newScheme = suggestedSwiftIdentifier(from: createInputController.text)
            let oldSuggestedAppID = "org.example.\(oldNativeScheme)"
            let newAppID = "org.example.\(newScheme)"
            let folder = createValues["nativeProjectFolder", default: ""]
            if folder.isEmpty || folder == oldNativeFolder {
                createValues["nativeProjectFolder"] = newFolder
            }
            let appID = createValues["nativeAppID", default: ""]
            let socket = createValues["nativeSocketFilename", default: ""]
            if socket.isEmpty ||
                socket == suggestedSocketFilename(fromAppID: appID.isEmpty ? oldSuggestedAppID : appID) ||
                socket == "\(oldNativeFolder).sock" ||
                socket == oldNativeFolder {
                createValues["nativeSocketFilename"] = suggestedSocketFilename(fromAppID: newAppID)
            }
            if appID.isEmpty || appID == oldSuggestedAppID {
                createValues["nativeAppID"] = newAppID
            }
        } else if key == "nativeAppID" {
            let appID = createInputController.text.trimmingCharacters(in: .whitespacesAndNewlines)
            let oldSocket = suggestedSocketFilename(fromAppID: oldNativeAppID.isEmpty ? "org.example.\(oldNativeScheme)" : oldNativeAppID)
            let socket = createValues["nativeSocketFilename", default: ""]
            if socket.isEmpty || socket == oldSocket {
                createValues["nativeSocketFilename"] = suggestedSocketFilename(fromAppID: appID)
            }
        } else if key == "nativeProjectFolder" || key == "nativeSocketFilename" {
            createValues[key] = NativeAppProjectGenerator.safePathComponent(createInputController.text)
        }
        createMessage = ""
        updateInputMode()
        updateEditingAndPasteboardState()
        updateLayout()
    }

    private func handleCreateKeyDown(keyCode: UInt16, characters: String?, modifierFlags: NSEvent.ModifierFlags) {
        if pendingFilePicker != nil {
            handleFilePickerKeyDown(keyCode: keyCode, characters: characters)
            return
        }
        if createInputController.isFocused {
            if keyCode == 48 {
                if modifierFlags.contains(.shift) {
                    retreatCreateField()
                } else {
                    advanceCreateField()
                }
            }
            return
        }
        switch keyCode {
        case 48:
            if modifierFlags.contains(.shift) {
                retreatCreateField()
            } else {
                advanceCreateField()
            }
        case 51, 117:
            focusActiveCreateField()
            createInputController.deleteBackward()
        case 36, 76:
            submitCreateForm()
        case 53:
            dismissCreateOverlay()
        default:
            if let characters, !characters.isEmpty {
                insertCreateText(characters)
            }
        }
    }

    private func handleFilePickerKeyDown(keyCode: UInt16, characters: String?) {
        if createInputController.isFocused {
            if keyCode == 53 {
                dismissFilePicker()
            }
            return
        }
        switch keyCode {
        case 36, 76:
            if !activateSelectedFilePickerEntry() {
                confirmFilePickerSave()
            }
        case 126:
            moveFilePickerSelection(delta: -1)
        case 125:
            moveFilePickerSelection(delta: 1)
        case 53:
            dismissFilePicker()
        default:
            if let characters,
               let text = filePickerTypeaheadText(from: characters) {
                handleFilePickerTypeahead(text)
            }
        }
    }

    private func advanceCreateField() {
        guard pendingFilePicker == nil else { return }
        if selectedCreateSection == .bashCommands || selectedCreateSection == .nativeApp {
            let fields = visibleLocalCreateFieldKeys()
            guard !fields.isEmpty else { return }
            let currentKey = activeCreateFieldKey ?? fields[0]
            let index = fields.firstIndex(of: currentKey) ?? 0
            focusCreateField(fields[(index + 1) % fields.count])
            updateLayout()
            return
        }
        let fields = selectedRecipe().map { visibleCreateFields(for: $0).filter { $0.fieldType != "choice" } } ?? []
        guard !fields.isEmpty else { return }
        let currentKey = activeCreateFieldKey ?? fields[0].key
        let index = fields.firstIndex(where: { $0.key == currentKey }) ?? 0
        focusCreateField(fields[(index + 1) % fields.count].key)
        updateLayout()
    }

    private func retreatCreateField() {
        guard pendingFilePicker == nil else { return }
        if selectedCreateSection == .bashCommands || selectedCreateSection == .nativeApp {
            let fields = visibleLocalCreateFieldKeys()
            guard !fields.isEmpty else { return }
            let currentKey = activeCreateFieldKey ?? fields[0]
            let index = fields.firstIndex(of: currentKey) ?? 0
            focusCreateField(fields[(index + fields.count - 1) % fields.count])
            updateLayout()
            return
        }
        let fields = selectedRecipe().map { visibleCreateFields(for: $0).filter { $0.fieldType != "choice" } } ?? []
        guard !fields.isEmpty else { return }
        let currentKey = activeCreateFieldKey ?? fields[0].key
        let index = fields.firstIndex(where: { $0.key == currentKey }) ?? 0
        focusCreateField(fields[(index + fields.count - 1) % fields.count].key)
        updateLayout()
    }

    private func insertCreateText(_ text: String,
                                  hasReplacementRange: Bool = false,
                                  replacementLocation: UInt64 = 0,
                                  replacementLength: UInt64 = 0) {
        guard mode == .create, !text.isEmpty else { return }
        if !createInputController.isFocused {
            focusActiveCreateField()
        }
        if hasReplacementRange {
            createInputController.setCursorPosition(Int(replacementLocation), modifySelection: false)
            let end = Int(replacementLocation + replacementLength)
            createInputController.setCursorPosition(end, modifySelection: true)
        }
        let cleaned = activeCreateFieldKey == "bashCommands" ? cleanBashCommandText(text) : cleanSingleLineText(text)
        guard !cleaned.isEmpty else { return }
        createInputController.insertText(cleaned)
    }

    private func insertWorkspaceRenameText(_ text: String,
                                           hasReplacementRange: Bool = false,
                                           replacementLocation: UInt64 = 0,
                                           replacementLength: UInt64 = 0) {
        guard isWorkspaceNamePromptVisible, !text.isEmpty else { return }
        if !workspaceRenameInputController.isFocused {
            focusWorkspaceRenameField()
        }
        if hasReplacementRange {
            workspaceRenameInputController.setCursorPosition(Int(replacementLocation),
                                                             modifySelection: false)
            let end = Int(replacementLocation + replacementLength)
            workspaceRenameInputController.setCursorPosition(end, modifySelection: true)
        }
        let cleaned = isDockerfileFragmentPrompt
            ? cleanBashCommandText(text)
            : cleanSingleLineText(text)
        guard !cleaned.isEmpty else { return }
        workspaceRenameInputController.insertText(cleaned)
    }

    private func workspaceRenameReplacementRange(hasReplacementRange: Bool,
                                                 replacementLocation: Int,
                                                 replacementLength: Int) -> Range<Int>? {
        guard hasReplacementRange else { return nil }
        let start = min(max(replacementLocation, 0), workspaceRenameInputController.text.count)
        let end = min(max(start + replacementLength, start),
                      workspaceRenameInputController.text.count)
        return start..<end
    }

    private func activeCreateReplacementRange(hasReplacementRange: Bool,
                                              replacementLocation: Int,
                                              replacementLength: Int) -> Range<Int>? {
        guard hasReplacementRange else { return nil }
        let start = min(max(replacementLocation, 0), createInputController.text.count)
        let end = min(max(start + replacementLength, start), createInputController.text.count)
        return start..<end
    }

    private func passwordReplacementRange(hasReplacementRange: Bool,
                                          replacementLocation: Int,
                                          replacementLength: Int) -> Range<Int>? {
        guard hasReplacementRange else { return nil }
        let start = min(max(replacementLocation, 0), passwordInputController.text.count)
        let end = min(max(start + replacementLength, start), passwordInputController.text.count)
        return start..<end
    }

    private func handleSetMarkedText(_ text: String,
                                     selectedLocation: Int,
                                     selectedLength: Int,
                                     hasReplacementRange: Bool,
                                     replacementLocation: Int,
                                     replacementLength: Int) {
        if isWorkspaceNamePromptVisible {
            if !workspaceRenameInputController.isFocused {
                focusWorkspaceRenameField()
            }
            let cleaned = isDockerfileFragmentPrompt
                ? cleanBashCommandText(text)
                : cleanSingleLineText(text)
            workspaceRenameInputController.setMarkedText(
                cleaned,
                selectedLocation: min(selectedLocation, cleaned.count),
                selectedLength: selectedLength,
                replacementRange: workspaceRenameReplacementRange(
                    hasReplacementRange: hasReplacementRange,
                    replacementLocation: replacementLocation,
                    replacementLength: replacementLength
                )
            )
            return
        }

        if pendingPasswordAction != nil {
            if !passwordInputController.isFocused {
                focusPasswordField()
            }
            passwordInputController.setMarkedText(cleanSingleLineText(text),
                                                  selectedLocation: selectedLocation,
                                                  selectedLength: selectedLength,
                                                  replacementRange: passwordReplacementRange(hasReplacementRange: hasReplacementRange,
                                                                                            replacementLocation: replacementLocation,
                                                                                            replacementLength: replacementLength))
            return
        }

        guard mode == .create else { return }
        if !createInputController.isFocused {
            focusActiveCreateField()
        }
        let cleaned = cleanTextForActiveCreateField(text)
        createInputController.setMarkedText(cleaned,
                                            selectedLocation: min(selectedLocation, cleaned.count),
                                            selectedLength: selectedLength,
                                            replacementRange: activeCreateReplacementRange(hasReplacementRange: hasReplacementRange,
                                                                                          replacementLocation: replacementLocation,
                                                                                          replacementLength: replacementLength))
    }

    private func handleUnmarkText() {
        if isWorkspaceNamePromptVisible, workspaceRenameInputController.isFocused {
            workspaceRenameInputController.unmarkText()
            return
        }
        if pendingPasswordAction != nil, passwordInputController.isFocused {
            passwordInputController.unmarkText()
            return
        }
        guard mode == .create, createInputController.isFocused else { return }
        createInputController.unmarkText()
    }

    private func cleanSingleLineText(_ text: String) -> String {
        text.filter { character in
            !character.isNewline && character.unicodeScalars.allSatisfy { $0.value >= 0x20 && $0.value != 0x7f }
        }
    }

    private func cleanBashCommandText(_ text: String) -> String {
        var normalized = text.replacingOccurrences(of: "\r\n", with: "\n")
        normalized = normalized.replacingOccurrences(of: "\r", with: "\n")
        return normalized.filter { character in
            character == "\n" || character == "\t" || character.unicodeScalars.allSatisfy { $0.value >= 0x20 && $0.value != 0x7f }
        }
    }

    private func focusWorkspaceRenameField(selectAll: Bool = false,
                                           cursorPosition: Int? = nil) {
        guard isWorkspaceNamePromptVisible else { return }
        blurCreateField()
        blurPasswordField()
        isSynchronizingWorkspaceRenameInput = true
        if workspaceRenameInputController.text != workspaceRenameName {
            workspaceRenameInputController.setText(workspaceRenameName)
        }
        workspaceRenameInputController.allowsNewlines =
            pendingRecipeCommandWorkspace != nil || pendingRecipeEditStep != nil ||
            (pendingDockerfileWorkspace != nil && !isRenamingContainerConfiguration)
        workspaceRenameInputController.visualLineWidth =
            workspaceRenameInputController.allowsNewlines
                ? max(workspaceRenameTextFrame.width, 280)
                : nil
        workspaceRenameInputController.focus(selectAll: selectAll)
        if let cursorPosition {
            workspaceRenameInputController.setCursorPosition(cursorPosition,
                                                             modifySelection: false)
        }
        isSynchronizingWorkspaceRenameInput = false
        updateInputMode()
        updateEditingAndPasteboardState()
        sendWorkspaceRenameTextInputGeometryUpdate()
    }

    private func blurWorkspaceRenameField() {
        workspaceRenameInputController.blur()
        updateInputMode()
        updateEditingAndPasteboardState()
        if !createInputController.isFocused && !passwordInputController.isFocused {
            outerframeHost.sendTextInputGeometryUpdate(nil)
        }
    }

    private func focusActiveCreateField(selectAll: Bool = false) {
        guard pendingFilePicker == nil else { return }
        let firstVisibleKey = selectedCreateSection == .bashCommands || selectedCreateSection == .nativeApp
            ? visibleLocalCreateFieldKeys().first
            : selectedRecipe().flatMap { recipe in
                visibleCreateFields(for: recipe).first(where: { $0.fieldType != "choice" })?.key
            }
        let key = activeCreateFieldKey ?? firstVisibleKey
        guard let key else { return }
        focusCreateField(key, selectAll: selectAll)
    }

    private func focusPasswordField(selectAll: Bool = false, cursorPosition: Int? = nil) {
        guard pendingPasswordAction != nil else { return }
        blurCreateField()
        isSynchronizingPasswordInput = true
        passwordInputController.setText(sudoPasswordInput)
        passwordInputController.focus(selectAll: selectAll)
        if let cursorPosition {
            passwordInputController.setCursorPosition(cursorPosition, modifySelection: false)
        }
        isSynchronizingPasswordInput = false
        updateInputMode()
        updateEditingAndPasteboardState()
        sendPasswordFieldTextInputGeometryUpdate()
    }

    private func blurPasswordField() {
        passwordInputController.blur()
        updateInputMode()
        updateEditingAndPasteboardState()
        if !createInputController.isFocused {
            outerframeHost.sendTextInputGeometryUpdate(nil)
        }
    }

    private func focusCreateField(_ key: String, selectAll: Bool = false, cursorPosition: Int? = nil) {
        blurPasswordField()
        activeCreateFieldKey = key
        let value = createValues[key] ?? selectedRecipe()?.fields.first(where: { $0.key == key })?.defaultValue ?? ""
        isSynchronizingCreateInput = true
        createInputController.allowsNewlines = key == "bashCommands"
        createInputController.visualLineWidth = createFieldLayouts[key]?.multiline == true ? createFieldLayouts[key]?.textFrame.width : nil
        createInputController.setText(value)
        createInputController.focus(selectAll: selectAll)
        if let cursorPosition {
            createInputController.setCursorPosition(cursorPosition, modifySelection: false)
        }
        isSynchronizingCreateInput = false
        updateInputMode()
        updateEditingAndPasteboardState()
        sendCreateFieldTextInputGeometryUpdate()
    }

    private func blurCreateField() {
        createInputController.allowsNewlines = false
        createInputController.visualLineWidth = nil
        createInputController.blur()
        updateInputMode()
        updateEditingAndPasteboardState()
        outerframeHost.sendTextInputGeometryUpdate(nil)
    }

    private func handleTextCommand(_ command: String) {
        if isRenamingContainerConfiguration {
            if command == "cancelOperation" {
                dismissWorkspaceRename()
            } else {
                workspaceRenameInputController.performCommand(command)
            }
            return
        }
        if isConfirmingContainerConfigurationDismissal {
            if command == "cancelOperation" {
                dismissContainerConfigurationDismissalPrompt()
            }
            return
        }
        if isConfirmingContainerConfigurationRebuild {
            if command == "cancelOperation" {
                dismissContainerConfigurationRebuildPrompt()
            }
            return
        }
        if isContainerConfigurationEditorVisible {
            if containerConfigurationBuildError != nil {
                if command == "cancelOperation" {
                    dismissContainerConfigurationBuildError()
                } else if command == "selectAll",
                          let block = safeSpaceDockerfileTextBlocks.first(where: {
                              $0.contentSpace == .workspacePanel
                          }) {
                    setDockerfileSelection(
                        fragmentID: block.fragmentID,
                        range: NSRange(
                            location: 0,
                            length: (block.text as NSString).length
                        )
                    )
                }
                return
            }
            if command == "saveDocument" {
                saveContainerConfigurationDockerfile()
                return
            }
            if command == "cancelOperation" {
                requestContainerConfigurationDismissal()
                return
            }
        }
        if isWorkspaceNamePromptVisible, workspaceRenameInputController.isFocused {
            if command == "cancelOperation" {
                dismissWorkspaceRename()
                return
            }
            if command == "insertTab", isDockerfileFragmentPrompt {
                workspaceRenameInputController.insertText("    ")
                return
            }
            workspaceRenameInputController.performCommand(command)
            return
        }

        if pendingPasswordAction != nil, passwordInputController.isFocused {
            if command == "cancelOperation" {
                dismissPasswordPrompt()
                return
            }
            passwordInputController.performCommand(command)
            return
        }

        if command == "selectAll",
           let fragmentID = selectedDockerfileFragmentID,
           let block = safeSpaceDockerfileTextBlocks.first(where: {
               $0.fragmentID == fragmentID
           }) {
            setDockerfileSelection(
                fragmentID: fragmentID,
                range: NSRange(location: 0, length: (block.text as NSString).length)
            )
            return
        }

        guard mode == .create, createInputController.isFocused else { return }
        if command == "insertTab" {
            if activeCreateFieldKey == "bashCommands" {
                createInputController.insertText("\t")
                return
            }
            advanceCreateField()
            return
        }
        if command == "insertBacktab" {
            retreatCreateField()
            return
        }
        if command == "cancelOperation" {
            if pendingFilePicker != nil {
                dismissFilePicker()
            } else {
                dismissCreateOverlay()
            }
            return
        }
        if let key = activeCreateFieldKey,
           let layout = createFieldLayouts[key],
           layout.multiline {
            createInputController.visualLineWidth = layout.textFrame.width
        } else {
            createInputController.visualLineWidth = nil
        }
        createInputController.performCommand(command)
    }

    private func handleTextInputFocus(fieldID: UUID, hasFocus: Bool) {
        if fieldID == Self.workspaceRenameInputID {
            if hasFocus {
                focusWorkspaceRenameField()
            } else {
                blurWorkspaceRenameField()
                updateLayout()
            }
            return
        }

        if fieldID == Self.passwordFieldInputID {
            if hasFocus {
                focusPasswordField()
            } else {
                blurPasswordField()
                updateLayout()
            }
            return
        }

        guard fieldID == Self.createFieldInputID else { return }
        if hasFocus {
            focusActiveCreateField()
        } else {
            blurCreateField()
            updateLayout()
        }
    }

    private func handleSetCursorPosition(fieldID: UUID, position: Int, modifySelection: Bool) {
        if fieldID == Self.workspaceRenameInputID {
            focusWorkspaceRenameField()
            workspaceRenameInputController.setCursorPosition(
                position,
                modifySelection: modifySelection
            )
            return
        }

        if fieldID == Self.passwordFieldInputID {
            focusPasswordField()
            passwordInputController.setCursorPosition(position, modifySelection: modifySelection)
            return
        }

        guard fieldID == Self.createFieldInputID else { return }
        focusActiveCreateField()
        createInputController.setCursorPosition(position, modifySelection: modifySelection)
    }

    private func updateInputMode() {
        let textInputIsFocused = createInputController.isFocused ||
            passwordInputController.isFocused ||
            workspaceRenameInputController.isFocused
        if textInputIsFocused, isContainerConfigurationEditorVisible {
            outerframeHost.setInputMode([.textInput, .rawKeys])
        } else {
            outerframeHost.setInputMode(textInputIsFocused ? .textInput : .rawKeys)
        }
    }

    private func updateEditingAndPasteboardState() {
        if workspaceRenameInputController.isFocused {
            outerframeHost.setAcceptedPasteboardPasteTypes(Self.workspaceRenamePasteboardTypes)
            return
        }

        if passwordInputController.isFocused {
            outerframeHost.setAcceptedPasteboardPasteTypes(Self.passwordFieldPasteboardTypes)
            return
        }

        if createInputController.isFocused {
            outerframeHost.setAcceptedPasteboardPasteTypes(createInputController.currentAcceptedPasteboardTypeIdentifiers())
            return
        }

        outerframeHost.setAcceptedPasteboardPasteTypes([])
    }

    private func enabledEditCommands(in requestedCommands: OuterframeEditCommandSet) -> OuterframeEditCommandSet {
        if workspaceRenameInputController.isFocused {
            return workspaceRenameInputController.enabledEditCommands(in: requestedCommands)
        }

        if passwordInputController.isFocused {
            var enabledCommands: OuterframeEditCommandSet = []
            if requestedCommands.contains(.paste) {
                enabledCommands.insert(.paste)
            }
            if requestedCommands.contains(.selectAll), !passwordInputController.text.isEmpty {
                enabledCommands.insert(.selectAll)
            }
            return enabledCommands
        }

        if createInputController.isFocused {
            return createInputController.enabledEditCommands(in: requestedCommands)
        }

        var enabledCommands: OuterframeEditCommandSet = []
        if requestedCommands.contains(.copy),
           ((aboutSelectionRange?.length ?? 0) > 0 ||
            (statusSelectionRange?.length ?? 0) > 0 ||
            (logHeaderDetailSelectionRange?.length ?? 0) > 0 ||
            (logTextSelectionRange?.length ?? 0) > 0 ||
            (selectedDockerfileAttributedText()?.length ?? 0) > 0 ||
            (normalizedCreateMessageSelectionRange()?.length ?? 0) > 0) {
            enabledCommands.insert(.copy)
        }
        if requestedCommands.contains(.selectAll),
           selectedDockerfileFragmentID != nil {
            enabledCommands.insert(.selectAll)
        }
        return enabledCommands
    }

    private func createInputFont(monospaced: Bool) -> NSFont {
        monospaced ? NSFont.monospacedSystemFont(ofSize: 12, weight: .regular) : NSFont.systemFont(ofSize: 12, weight: .regular)
    }

    private func makeCreateFieldLine(for text: String, monospaced: Bool) -> CTLine {
        let attributed = NSAttributedString(string: text, attributes: [.font: createInputFont(monospaced: monospaced)])
        return CTLineCreateWithAttributedString(attributed)
    }

    private func workspaceNameInputFont() -> NSFont {
        isDockerfileFragmentPrompt
            ? NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
            : NSFont.systemFont(ofSize: 13, weight: .regular)
    }

    private func makeWorkspaceNameFieldLine(for text: String) -> CTLine {
        let attributed = NSAttributedString(
            string: text,
            attributes: [.font: workspaceNameInputFont()]
        )
        return CTLineCreateWithAttributedString(attributed)
    }

    private func createTextAreaLineFragments(text: String, layout: CreateFieldLayout) -> [CreateTextLineFragment] {
        let lineHeight: CGFloat = 16
        var fragments: [CreateTextLineFragment] = []

        var start = 0
        var visualLineIndex = 0
        let parts = text.split(separator: "\n", omittingEmptySubsequences: false)
        if parts.isEmpty {
            return [CreateTextLineFragment(text: "", start: 0, end: 0, y: layout.textFrame.maxY - lineHeight)]
        }
        for part in parts {
            let lineText = String(part)
            let end = start + lineText.count
            let wrappedRanges = wrappedCreateTextRanges(for: lineText,
                                                        maxWidth: layout.textFrame.width,
                                                        monospaced: layout.monospaced)
            for range in wrappedRanges {
                let fragmentStart = start + range.lowerBound
                let fragmentEnd = start + range.upperBound
                let lower = lineText.index(lineText.startIndex, offsetBy: range.lowerBound)
                let upper = lineText.index(lineText.startIndex, offsetBy: range.upperBound)
                fragments.append(CreateTextLineFragment(text: String(lineText[lower..<upper]),
                                                        start: fragmentStart,
                                                        end: fragmentEnd,
                                                        y: layout.textFrame.maxY - CGFloat(visualLineIndex + 1) * lineHeight))
                visualLineIndex += 1
            }
            start = end + 1
        }
        return fragments
    }

    private func wrappedCreateTextRanges(for text: String,
                                         maxWidth: CGFloat,
                                         monospaced: Bool) -> [Range<Int>] {
        let count = text.count
        guard count > 0 else { return [0..<0] }
        guard maxWidth > 1 else { return (0..<count).map { $0..<($0 + 1) } }

        var ranges: [Range<Int>] = []
        var start = 0
        while start < count {
            var low = start + 1
            var high = count
            var best = start + 1
            while low <= high {
                let mid = (low + high) / 2
                if measuredCreateTextWidth(text, range: start..<mid, monospaced: monospaced) <= maxWidth {
                    best = mid
                    low = mid + 1
                } else {
                    high = mid - 1
                }
            }
            ranges.append(start..<max(best, start + 1))
            start = max(best, start + 1)
        }
        return ranges
    }

    private func measuredCreateTextWidth(_ text: String,
                                         range: Range<Int>,
                                         monospaced: Bool) -> CGFloat {
        let lower = text.index(text.startIndex, offsetBy: range.lowerBound)
        let upper = text.index(text.startIndex, offsetBy: range.upperBound)
        let line = makeCreateFieldLine(for: String(text[lower..<upper]), monospaced: monospaced)
        return CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
    }

    private func makePasswordFieldLine(for text: String) -> CTLine {
        let attributed = NSAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: 14, weight: .regular)])
        return CTLineCreateWithAttributedString(attributed)
    }

    private func offsetForCreateFieldCharacter(line: CTLine, text: String, index: Int, maxWidth: CGFloat) -> CGFloat {
        let utf16Index = utf16Offset(forCharacterIndex: index, in: text)
        var secondaryOffset: CGFloat = 0
        let primary = CTLineGetOffsetForStringIndex(line, utf16Index, &secondaryOffset)
        let offset = max(primary, secondaryOffset)
        return offset.isFinite ? min(max(offset, 0), maxWidth) : 0
    }

    private func selectionOffsets(line: CTLine, text: String, range: Range<Int>, maxWidth: CGFloat) -> (start: CGFloat, end: CGFloat) {
        let start = offsetForCreateFieldCharacter(line: line, text: text, index: range.lowerBound, maxWidth: maxWidth)
        let end = offsetForCreateFieldCharacter(line: line, text: text, index: range.upperBound, maxWidth: maxWidth)
        return (min(start, maxWidth), min(max(end, start), maxWidth))
    }

    private func characterIndexForCreateField(key: String, at point: CGPoint) -> Int {
        guard let layout = createFieldLayouts[key] else { return 0 }
        let text = createValues[key] ?? selectedRecipe()?.fields.first(where: { $0.key == key })?.defaultValue ?? ""
        guard !text.isEmpty, layout.textFrame.width > 0 else { return 0 }

        if layout.multiline {
            let fragments = createTextAreaLineFragments(text: text, layout: layout)
            let lineHeight: CGFloat = 16
            let row = max(0, min(Int(floor((layout.textFrame.maxY - point.y) / lineHeight)), fragments.count - 1))
            let fragment = fragments[row]
            let localX = max(0, min(point.x - layout.textFrame.minX, layout.textFrame.width))
            guard !fragment.text.isEmpty else { return fragment.start }
            let line = makeCreateFieldLine(for: fragment.text, monospaced: layout.monospaced)
            let utf16Index = CTLineGetStringIndexForPosition(line, CGPoint(x: localX, y: 0))
            if utf16Index == kCFNotFound { return fragment.end }
            return fragment.start + characterIndex(forUTF16: utf16Index, in: fragment.text)
        }

        let localX = max(0, min(point.x - layout.textFrame.minX, layout.textFrame.width))
        let line = makeCreateFieldLine(for: text, monospaced: layout.monospaced)
        let utf16Index = CTLineGetStringIndexForPosition(line, CGPoint(x: localX, y: 0))
        if utf16Index == kCFNotFound { return text.count }
        return characterIndex(forUTF16: utf16Index, in: text)
    }

    private func characterIndexForPasswordField(xPosition: CGFloat) -> Int {
        let bulletString = String(repeating: "\u{2022}", count: sudoPasswordInput.count)
        let localX = max(0, min(xPosition - passwordTextFrame.minX, passwordTextFrame.width))
        guard !bulletString.isEmpty, passwordTextFrame.width > 0 else { return 0 }
        let line = makePasswordFieldLine(for: bulletString)
        let utf16Index = CTLineGetStringIndexForPosition(line, CGPoint(x: localX, y: 0))
        if utf16Index == kCFNotFound { return bulletString.count }
        return characterIndex(forUTF16: utf16Index, in: bulletString)
    }

    private func characterIndexForWorkspaceRenameField(at point: CGPoint) -> Int {
        let text = workspaceRenameInputController.text
        if isDockerfileFragmentPrompt {
            let layout = CreateFieldLayout(fieldFrame: workspaceRenameFieldFrame,
                                           textFrame: workspaceRenameTextFrame,
                                           key: "dockerfileFragment",
                                           monospaced: true,
                                           multiline: true)
            let canUseContainerFragments = isContainerConfigurationEditorVisible &&
                containerConfigurationTextFragmentsText == text &&
                abs(containerConfigurationTextFragmentsWidth - layout.textFrame.width) < 0.5
            let fragments = canUseContainerFragments
                ? containerConfigurationTextFragments
                : createTextAreaLineFragments(text: text, layout: layout)
            guard !fragments.isEmpty else { return 0 }
            let lineHeight: CGFloat = 16
            let row = max(0, min(
                Int(floor((workspaceRenameTextFrame.maxY - point.y) / lineHeight)),
                fragments.count - 1
            ))
            let fragment = fragments[row]
            if point.y > workspaceRenameTextFrame.maxY { return 0 }
            let textBottom = workspaceRenameTextFrame.maxY -
                CGFloat(fragments.count) * lineHeight
            if point.y < textBottom { return text.count }
            let localX = max(0, min(point.x - workspaceRenameTextFrame.minX,
                                    workspaceRenameTextFrame.width))
            guard !fragment.text.isEmpty else { return fragment.start }
            let line = makeCreateFieldLine(for: fragment.text, monospaced: true)
            let utf16Index = CTLineGetStringIndexForPosition(
                line, CGPoint(x: localX, y: 0)
            )
            if utf16Index == kCFNotFound { return fragment.end }
            return fragment.start + characterIndex(forUTF16: utf16Index,
                                                     in: fragment.text)
        }
        let localX = max(0, min(point.x - workspaceRenameTextFrame.minX,
                                workspaceRenameTextFrame.width))
        guard !text.isEmpty, workspaceRenameTextFrame.width > 0 else { return 0 }
        let line = makeWorkspaceNameFieldLine(for: text)
        let utf16Index = CTLineGetStringIndexForPosition(line, CGPoint(x: localX, y: 0))
        if utf16Index == kCFNotFound { return text.count }
        return characterIndex(forUTF16: utf16Index, in: text)
    }

    private func utf16Offset(forCharacterIndex index: Int, in text: String) -> Int {
        let clamped = max(0, min(index, text.count))
        let stringIndex = text.index(text.startIndex, offsetBy: clamped)
        return text[text.startIndex..<stringIndex].utf16.count
    }

    private func characterIndex(forUTF16 offset: Int, in text: String) -> Int {
        let clamped = max(0, min(offset, text.utf16.count))
        let stringIndex = String.Index(utf16Offset: clamped, in: text)
        return text.distance(from: text.startIndex, to: stringIndex)
    }

    private func createFieldCursorRect(layout: CreateFieldLayout, cachedLine: CTLine?) -> CGRect {
        let text = createInputController.text
        let cursorWidth: CGFloat = 1
        let maxWidth = max(layout.textFrame.width, 0)

        if layout.multiline {
            let fragments = createTextAreaLineFragments(text: text, layout: layout)
            let cursorPosition = createInputController.cursorPosition
            let fragment = fragments.first { cursorPosition >= $0.start && cursorPosition <= $0.end } ?? fragments.last
            guard let fragment else {
                return CGRect(x: layout.textFrame.minX,
                              y: layout.textFrame.maxY - 16,
                              width: cursorWidth,
                              height: 16)
            }
            let offset: CGFloat
            if !fragment.text.isEmpty && maxWidth > 0 {
                let line = makeCreateFieldLine(for: fragment.text, monospaced: layout.monospaced)
                offset = offsetForCreateFieldCharacter(line: line,
                                                       text: fragment.text,
                                                       index: cursorPosition - fragment.start,
                                                       maxWidth: maxWidth)
            } else {
                offset = 0
            }
            let proposedX = min(max(layout.textFrame.minX + offset, layout.textFrame.minX), layout.textFrame.maxX)
            let maxCursorX = layout.fieldFrame.maxX - 2 - cursorWidth
            return CGRect(x: max(layout.textFrame.minX, min(proposedX, maxCursorX)),
                          y: max(layout.textFrame.minY, min(fragment.y - 1, layout.textFrame.maxY - 16)),
                          width: cursorWidth,
                          height: 18)
        }

        let offset: CGFloat
        if let cachedLine, !text.isEmpty && maxWidth > 0 {
            offset = offsetForCreateFieldCharacter(line: cachedLine,
                                                   text: text,
                                                   index: createInputController.cursorPosition,
                                                   maxWidth: maxWidth)
        } else {
            offset = 0
        }
        let proposedX = min(max(layout.textFrame.minX + offset, layout.textFrame.minX), layout.textFrame.maxX)
        let maxCursorX = layout.fieldFrame.maxX - 2 - cursorWidth
        return CGRect(x: max(layout.textFrame.minX, min(proposedX, maxCursorX)),
                      y: layout.textFrame.minY - 1,
                      width: cursorWidth,
                      height: layout.textFrame.height + 2)
    }

    private func addBlinkingTextCaret(to layer: CALayer, frame: CGRect) {
        let caret = CALayer()
        caret.frame = frame
        caret.backgroundColor = resolvedCGColor(.textColor)
        caret.opacity = 1
        let animation = CABasicAnimation(keyPath: "opacity")
        animation.fromValue = 1
        animation.toValue = 0
        animation.duration = 0.55
        animation.beginTime = CACurrentMediaTime() + 0.55
        animation.autoreverses = true
        animation.repeatCount = .infinity
        animation.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
        caret.add(animation, forKey: textCaretBlinkAnimationKey)
        layer.addSublayer(caret)
    }

    private func sendCreateFieldTextInputGeometryUpdate() {
        guard mode == .create,
              windowIsActive,
              createInputController.isFocused,
              !createInputController.hasSelection,
              let key = activeCreateFieldKey,
              let layout = createFieldLayouts[key] else {
            outerframeHost.sendTextInputGeometryUpdate(nil)
            return
        }
        let line = createInputController.text.isEmpty ? nil : makeCreateFieldLine(for: createInputController.text, monospaced: layout.monospaced)
        let cursorFrame = createFieldCursorRect(layout: layout, cachedLine: line)
        let rootPosition = createLayer.convert(cursorFrame.origin, to: rootLayer)
        let topLeftY = rootLayer.bounds.height - rootPosition.y - cursorFrame.height
        let geometry = OuterframeContentTextInputGeometry(fieldID: Self.createFieldInputID,
                                                          rect: CGRect(x: rootPosition.x,
                                                                       y: topLeftY,
                                                                       width: cursorFrame.width,
                                                                       height: cursorFrame.height))
        outerframeHost.sendTextInputGeometryUpdate(geometry)
    }

    private func passwordFieldCursorRect() -> CGRect? {
        let bulletString = String(repeating: "\u{2022}", count: passwordInputController.text.count)
        guard pendingPasswordAction != nil,
              passwordInputController.isFocused,
              !passwordInputController.hasSelection,
              passwordFieldFrame != .zero,
              passwordTextFrame != .zero else {
            return nil
        }

        let cursorWidth: CGFloat = 1
        let maxWidth = max(passwordTextFrame.width, 0)
        let offset: CGFloat
        if !bulletString.isEmpty && maxWidth > 0 {
            let line = makePasswordFieldLine(for: bulletString)
            offset = offsetForCreateFieldCharacter(line: line,
                                                   text: bulletString,
                                                   index: passwordInputController.cursorPosition,
                                                   maxWidth: maxWidth)
        } else {
            offset = 0
        }

        let proposedX = min(max(passwordTextFrame.minX + offset, passwordTextFrame.minX), passwordTextFrame.maxX)
        let maxCursorX = passwordFieldFrame.maxX - 2 - cursorWidth
        return CGRect(x: max(passwordTextFrame.minX, min(proposedX, maxCursorX)),
                      y: passwordTextFrame.minY - 1,
                      width: cursorWidth,
                      height: passwordTextFrame.height + 2)
    }

    private func sendPasswordFieldTextInputGeometryUpdate() {
        guard windowIsActive,
              let cursorFrame = passwordFieldCursorRect() else {
            if !createInputController.isFocused {
                outerframeHost.sendTextInputGeometryUpdate(nil)
            }
            return
        }
        let topLeftY = rootLayer.bounds.height - cursorFrame.minY - cursorFrame.height
        let geometry = OuterframeContentTextInputGeometry(fieldID: Self.passwordFieldInputID,
                                                          rect: CGRect(x: cursorFrame.minX,
                                                                       y: topLeftY,
                                                                       width: cursorFrame.width,
                                                                       height: cursorFrame.height))
        outerframeHost.sendTextInputGeometryUpdate(geometry)
    }

    private func workspaceRenameFieldCursorRect() -> CGRect? {
        guard isWorkspaceNamePromptVisible,
              workspaceRenameInputController.isFocused,
              !workspaceRenameInputController.hasSelection,
              !workspaceRenameFieldFrame.isEmpty,
              !workspaceRenameTextFrame.isEmpty else {
            return nil
        }

        let text = workspaceRenameInputController.text
        if isDockerfileFragmentPrompt {
            let layout = CreateFieldLayout(fieldFrame: workspaceRenameFieldFrame,
                                           textFrame: workspaceRenameTextFrame,
                                           key: "dockerfileFragment",
                                           monospaced: true,
                                           multiline: true)
            let fragments = createTextAreaLineFragments(text: text, layout: layout)
            let cursorPosition = workspaceRenameInputController.cursorPosition
            guard let fragment = fragments.first(where: {
                cursorPosition >= $0.start && cursorPosition <= $0.end
            }) ?? fragments.last else {
                return CGRect(x: workspaceRenameTextFrame.minX,
                              y: workspaceRenameTextFrame.maxY - 16,
                              width: 1,
                              height: 18)
            }
            let offset: CGFloat
            if fragment.text.isEmpty {
                offset = 0
            } else {
                let line = makeCreateFieldLine(for: fragment.text, monospaced: true)
                offset = offsetForCreateFieldCharacter(
                    line: line,
                    text: fragment.text,
                    index: cursorPosition - fragment.start,
                    maxWidth: workspaceRenameTextFrame.width
                )
            }
            return CGRect(x: workspaceRenameTextFrame.minX + offset,
                          y: fragment.y - 1,
                          width: 1,
                          height: 18)
        }
        let maxWidth = max(workspaceRenameTextFrame.width, 0)
        let offset: CGFloat
        if !text.isEmpty && maxWidth > 0 {
            let line = makeWorkspaceNameFieldLine(for: text)
            offset = offsetForCreateFieldCharacter(
                line: line,
                text: text,
                index: workspaceRenameInputController.cursorPosition,
                maxWidth: maxWidth
            )
        } else {
            offset = 0
        }
        let cursorWidth: CGFloat = 1
        let proposedX = min(max(workspaceRenameTextFrame.minX + offset,
                                workspaceRenameTextFrame.minX),
                            workspaceRenameTextFrame.maxX)
        let maxCursorX = workspaceRenameFieldFrame.maxX - 2 - cursorWidth
        return CGRect(
            x: max(workspaceRenameTextFrame.minX, min(proposedX, maxCursorX)),
            y: workspaceRenameTextFrame.minY - 1,
            width: cursorWidth,
            height: workspaceRenameTextFrame.height + 2
        )
    }

    private func sendWorkspaceRenameTextInputGeometryUpdate() {
        guard windowIsActive,
              let cursorFrame = workspaceRenameFieldCursorRect() else {
            if !createInputController.isFocused && !passwordInputController.isFocused {
                outerframeHost.sendTextInputGeometryUpdate(nil)
            }
            return
        }
        let topLeftY = rootLayer.bounds.height - cursorFrame.minY - cursorFrame.height
        let geometry = OuterframeContentTextInputGeometry(
            fieldID: Self.workspaceRenameInputID,
            rect: CGRect(x: cursorFrame.minX,
                         y: topLeftY,
                         width: cursorFrame.width,
                         height: cursorFrame.height)
        )
        outerframeHost.sendTextInputGeometryUpdate(geometry)
    }

    private func sendFocusedTextInputGeometryUpdate() {
        if workspaceRenameInputController.isFocused {
            sendWorkspaceRenameTextInputGeometryUpdate()
        } else if passwordInputController.isFocused {
            sendPasswordFieldTextInputGeometryUpdate()
        } else {
            sendCreateFieldTextInputGeometryUpdate()
        }
    }

    private func pasteboardItemsForCopy() -> [OuterframeContentPasteboardItem] {
        if workspaceRenameInputController.isFocused,
           let selectedText = workspaceRenameInputController.selectedTextContent(),
           !selectedText.isEmpty {
            return [
                OuterframeContentPasteboardItem(representations: [
                    OuterframeContentPasteboardRepresentation(
                        typeIdentifier: NSPasteboard.PasteboardType.string.rawValue,
                        data: Data(selectedText.utf8)
                    )
                ])
            ]
        }

        if createInputController.isFocused,
           let selectedText = createInputController.selectedTextContent(),
           !selectedText.isEmpty {
            return [
                OuterframeContentPasteboardItem(representations: [
                    OuterframeContentPasteboardRepresentation(typeIdentifier: NSPasteboard.PasteboardType.string.rawValue,
                                                              data: Data(selectedText.utf8))
                ])
            ]
        }

        if let selectedText = selectedCreateMessageAttributedText(),
           selectedText.length > 0 {
            return pasteboardItems(for: selectedText)
        }

        if let selectedText = selectedStatusAttributedText(),
           selectedText.length > 0 {
            return pasteboardItems(for: selectedText)
        }

        if let selectedText = selectedDockerfileAttributedText(),
           selectedText.length > 0 {
            return pasteboardItems(for: selectedText)
        }

        if let selectedText = selectedLogHeaderDetailAttributedText(),
           selectedText.length > 0 {
            return pasteboardItems(for: selectedText)
        }

        if let selectedText = selectedAboutAttributedText(),
           selectedText.length > 0 {
            return pasteboardItems(for: selectedText)
        }

        guard let selectedText = selectedLogAttributedText(),
              selectedText.length > 0 else {
            return []
        }

        return pasteboardItems(for: selectedText)
    }

    private func aboutDialogText(for backend: BackendRecord) -> String {
        let version = backend.installedVersion?.trimmingCharacters(in: .whitespacesAndNewlines)
        let publicBaseURL = backend.publicBaseURL?.trimmingCharacters(in: .whitespacesAndNewlines)
        let updateSource = (publicBaseURL?.isEmpty == false) ? publicBaseURL! : "unknown"
        return [
            "Outer Shell",
            "Version: \((version?.isEmpty == false) ? version! : "unknown")",
            "Service ID: \(backend.serviceID)",
            "Scope: \(backend.serviceScope)",
            "Status: \(backend.status)",
            "Update source: \(updateSource)"
        ].joined(separator: "\n")
    }

    private func aboutTextFont() -> NSFont {
        NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    }

    private func aboutTextLineHeight() -> CGFloat {
        18
    }

    private func aboutTextLine(for text: String) -> CTLine {
        CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: [.font: aboutTextFont()]))
    }

    private func aboutLineFragments() -> [(text: String, range: NSRange, y: CGFloat)] {
        let lines = renderedAboutText.components(separatedBy: "\n")
        let lineHeight = aboutTextLineHeight()
        var location = 0
        var fragments: [(text: String, range: NSRange, y: CGFloat)] = []
        fragments.reserveCapacity(lines.count)
        for (index, line) in lines.enumerated() {
            let length = (line as NSString).length
            fragments.append((text: line,
                              range: NSRange(location: location, length: length),
                              y: aboutTextFrame.height - CGFloat(index + 1) * lineHeight - 4))
            location += length
            if index < lines.count - 1 {
                location += 1
            }
        }
        return fragments
    }

    private func normalizedAboutSelectionRange(_ range: NSRange?) -> NSRange? {
        guard let range else { return nil }
        let length = (renderedAboutText as NSString).length
        let lower = max(min(range.location, length), 0)
        let upper = max(min(range.location + range.length, length), lower)
        guard upper > lower else { return nil }
        return NSRange(location: lower, length: upper - lower)
    }

    private func setAboutSelectionRange(_ range: NSRange?) {
        let nextRange = normalizedAboutSelectionRange(range)
        guard nextRange != aboutSelectionRange else { return }
        aboutSelectionRange = nextRange
        updateAboutSelectionLayers()
        updateEditingAndPasteboardState()
    }

    private func selectedAboutAttributedText() -> NSAttributedString? {
        guard let selectionRange = normalizedAboutSelectionRange(aboutSelectionRange) else {
            return nil
        }
        let attributed = NSAttributedString(string: renderedAboutText,
                                            attributes: [.font: aboutTextFont(),
                                                         .foregroundColor: NSColor.labelColor])
        return attributed.attributedSubstring(from: selectionRange)
    }

    private func aboutTextOffset(at point: CGPoint) -> Int {
        let length = (renderedAboutText as NSString).length
        guard length > 0 else { return 0 }
        let x = max(point.x - aboutTextFrame.minX - 10, 0)
        let y = point.y - aboutTextFrame.minY
        let lineHeight = aboutTextLineHeight()
        let fragments = aboutLineFragments()
        guard !fragments.isEmpty else { return 0 }
        let rawIndex = Int(floor((aboutTextFrame.height - y - 4) / lineHeight))
        let lineIndex = min(max(rawIndex, 0), fragments.count - 1)
        let fragment = fragments[lineIndex]
        if x <= 0 {
            return fragment.range.location
        }
        let line = aboutTextLine(for: fragment.text)
        let index = CTLineGetStringIndexForPosition(line, CGPoint(x: x, y: 0))
        if index == kCFNotFound {
            return fragment.range.location + fragment.range.length
        }
        return min(max(fragment.range.location + index, fragment.range.location),
                   fragment.range.location + fragment.range.length)
    }

    private func aboutWordRange(containing offset: Int) -> NSRange? {
        let string = renderedAboutText as NSString
        let length = string.length
        guard length > 0 else { return nil }
        var location = min(max(offset, 0), length - 1)
        if location > 0, !aboutCharacterIsWordLike(string.character(at: location)) {
            location -= 1
        }
        guard aboutCharacterIsWordLike(string.character(at: location)) else {
            return NSRange(location: min(max(offset, 0), length), length: 0)
        }
        var start = location
        while start > 0, aboutCharacterIsWordLike(string.character(at: start - 1)) {
            start -= 1
        }
        var end = location + 1
        while end < length, aboutCharacterIsWordLike(string.character(at: end)) {
            end += 1
        }
        return NSRange(location: start, length: end - start)
    }

    private func aboutCharacterIsWordLike(_ character: unichar) -> Bool {
        if character >= 48 && character <= 57 { return true }
        if character >= 65 && character <= 90 { return true }
        if character >= 97 && character <= 122 { return true }
        return character == 45 || character == 46 || character == 47 || character == 58 || character == 95
    }

    private func handleAboutMouseDragged(to point: CGPoint) -> Bool {
        guard let aboutDragAnchorOffset else { return false }
        let offset = aboutTextOffset(at: point)
        let location = min(aboutDragAnchorOffset, offset)
        let length = abs(offset - aboutDragAnchorOffset)
        setAboutSelectionRange(NSRange(location: location, length: length))
        return true
    }

    private func handleAboutRightMouseDown(at point: CGPoint) {
        guard aboutTextFrame.contains(point) else { return }
        let offset = aboutTextOffset(at: point)
        if let selectionRange = normalizedAboutSelectionRange(aboutSelectionRange),
           offset >= selectionRange.location,
           offset <= selectionRange.location + selectionRange.length,
           let selectedText = selectedAboutAttributedText() {
            outerframeHost.showContextMenu(for: selectedText, at: point)
            return
        }
        if let wordRange = aboutWordRange(containing: offset),
           wordRange.length > 0 {
            setAboutSelectionRange(wordRange)
        } else {
            setAboutSelectionRange(NSRange(location: 0, length: (renderedAboutText as NSString).length))
        }
        if let selectedText = selectedAboutAttributedText() {
            outerframeHost.showContextMenu(for: selectedText, at: point)
        }
    }

    private func pasteboardItems(for selectedText: NSAttributedString) -> [OuterframeContentPasteboardItem] {
        var representations = [
            OuterframeContentPasteboardRepresentation(typeIdentifier: NSPasteboard.PasteboardType.string.rawValue,
                                                      data: Data(selectedText.string.utf8))
        ]
        if let rtfData = try? selectedText.data(from: NSRange(location: 0, length: selectedText.length),
                                                documentAttributes: [.documentType: NSAttributedString.DocumentType.rtf]) {
            representations.append(OuterframeContentPasteboardRepresentation(typeIdentifier: NSPasteboard.PasteboardType.rtf.rawValue,
                                                                             data: rtfData))
        }
        return [OuterframeContentPasteboardItem(representations: representations)]
    }

    private func pasteboardItemsForCut() -> [OuterframeContentPasteboardItem] {
        if workspaceRenameInputController.isFocused,
           let selectedText = workspaceRenameInputController.cutSelectedTextContent(),
           !selectedText.isEmpty {
            return [
                OuterframeContentPasteboardItem(representations: [
                    OuterframeContentPasteboardRepresentation(
                        typeIdentifier: NSPasteboard.PasteboardType.string.rawValue,
                        data: Data(selectedText.utf8)
                    )
                ])
            ]
        }

        guard createInputController.isFocused,
              let selectedText = createInputController.cutSelectedTextContent(),
              !selectedText.isEmpty else {
            return []
        }
        return [
            OuterframeContentPasteboardItem(representations: [
                OuterframeContentPasteboardRepresentation(typeIdentifier: NSPasteboard.PasteboardType.string.rawValue,
                                                          data: Data(selectedText.utf8))
            ])
        ]
    }

    private func handlePasteboardItemsForPaste(_ items: [OuterframeContentPasteboardItem]) {
        if isWorkspaceNamePromptVisible, workspaceRenameInputController.isFocused {
            _ = insertPasteboardItemsIntoWorkspaceRenameField(items)
            return
        }
        if pendingPasswordAction != nil, passwordInputController.isFocused {
            _ = insertPasteboardItemsIntoPasswordField(items)
            return
        }
        guard mode == .create, createInputController.isFocused else { return }
        _ = insertPasteboardItemsIntoCreateField(items)
    }

    @discardableResult
    private func insertPasteboardItemsIntoWorkspaceRenameField(
        _ items: [OuterframeContentPasteboardItem]
    ) -> Bool {
        for item in items {
            if let representation = item.representations.first(where: {
                $0.typeIdentifier == NSPasteboard.PasteboardType.string.rawValue
            }),
               let stringValue = String(data: representation.data, encoding: .utf8) {
                workspaceRenameInputController.insertText(
                    isDockerfileFragmentPrompt
                        ? cleanBashCommandText(stringValue)
                        : cleanSingleLineText(stringValue)
                )
                return true
            }
            if let representation = item.representations.first(where: {
                $0.typeIdentifier == NSPasteboard.PasteboardType.rtf.rawValue
            }),
               let attributed = try? NSAttributedString(
                   data: representation.data,
                   options: [.documentType: NSAttributedString.DocumentType.rtf],
                   documentAttributes: nil
               ) {
                workspaceRenameInputController.insertText(
                    isDockerfileFragmentPrompt
                        ? cleanBashCommandText(attributed.string)
                        : cleanSingleLineText(attributed.string)
                )
                return true
            }
        }
        return false
    }

    @discardableResult
    private func insertPasteboardItemsIntoCreateField(_ items: [OuterframeContentPasteboardItem]) -> Bool {
        for item in items {
            if let representation = item.representations.first(where: { $0.typeIdentifier == NSPasteboard.PasteboardType.string.rawValue }),
               let stringValue = String(data: representation.data, encoding: .utf8) {
                createInputController.insertText(cleanTextForActiveCreateField(stringValue))
                return true
            }
            if let representation = item.representations.first(where: { $0.typeIdentifier == NSPasteboard.PasteboardType.rtf.rawValue }),
               let attributed = try? NSAttributedString(data: representation.data,
                                                        options: [.documentType: NSAttributedString.DocumentType.rtf],
                                                        documentAttributes: nil) {
                createInputController.insertText(cleanTextForActiveCreateField(attributed.string))
                return true
            }
        }
        return false
    }

    private func cleanTextForActiveCreateField(_ text: String) -> String {
        activeCreateFieldKey == "bashCommands" ? cleanBashCommandText(text) : cleanSingleLineText(text)
    }

    @discardableResult
    private func insertPasteboardItemsIntoPasswordField(_ items: [OuterframeContentPasteboardItem]) -> Bool {
        for item in items {
            if let representation = item.representations.first(where: { $0.typeIdentifier == NSPasteboard.PasteboardType.string.rawValue }),
               let stringValue = String(data: representation.data, encoding: .utf8) {
                passwordInputController.insertText(cleanSingleLineText(stringValue))
                return true
            }
            if let representation = item.representations.first(where: { $0.typeIdentifier == NSPasteboard.PasteboardType.rtf.rawValue }),
               let attributed = try? NSAttributedString(data: representation.data,
                                                        options: [.documentType: NSAttributedString.DocumentType.rtf],
                                                        documentAttributes: nil) {
                passwordInputController.insertText(cleanSingleLineText(attributed.string))
                return true
            }
        }
        return false
    }

    private func createFieldDropPoint(_ point: CGPoint) -> (key: String, point: CGPoint)? {
        guard mode == .create else { return nil }
        let contentPoint = contentLayer.convert(point, from: rootLayer)
        let createPoint = createLayer.convert(contentPoint, from: contentLayer)
        guard pendingFilePicker == nil else { return nil }
        guard createContentClipFrame.contains(createPoint) else { return nil }
        guard let key = createFieldFrames.first(where: { $0.frame.contains(createPoint) })?.key else { return nil }
        return (key, createPoint)
    }

    private func createFieldAcceptsTextDrop(at point: CGPoint,
                                            pasteboardTypes: [String],
                                            operationMask: UInt32) -> Bool {
        let operations = NSDragOperation(rawValue: UInt(operationMask))
        let types = Set(pasteboardTypes)
        return operations.contains(.copy) &&
               Self.createFieldPasteboardTypes.contains { types.contains($0) } &&
               createFieldDropPoint(point) != nil
    }

    private func passwordFieldAcceptsTextDrop(at point: CGPoint,
                                              pasteboardTypes: [String],
                                              operationMask: UInt32) -> Bool {
        let operations = NSDragOperation(rawValue: UInt(operationMask))
        let types = Set(pasteboardTypes)
        return operations.contains(.copy) &&
               pendingPasswordAction != nil &&
               passwordFieldFrame.contains(point) &&
               Self.passwordFieldPasteboardTypes.contains { types.contains($0) }
    }

    private func sharedContainerFileAcceptsDrop(pasteboardTypes: [String],
                                                operationMask: UInt32) -> Bool {
        let operations = NSDragOperation(rawValue: UInt(operationMask))
        return mode == .apps &&
            workspaceContextID == nil &&
            operations.contains(.copy) &&
            pasteboardTypes.contains(NSPasteboard.PasteboardType.fileURL.rawValue)
    }

    private func sharedContainerURL(in items: [OuterframeContentPasteboardItem]) -> URL? {
        for item in items {
            guard let representation = item.representations.first(where: {
                $0.typeIdentifier == NSPasteboard.PasteboardType.fileURL.rawValue
            }),
                  let text = String(data: representation.data, encoding: .utf8),
                  let url = URL(string: text),
                  url.isFileURL,
                  url.pathExtension == "outershell-container" else {
                continue
            }
            return url
        }
        return nil
    }

    private func handlePasteboardItemsForDrop(at point: CGPoint, items: [OuterframeContentPasteboardItem]) {
        if let url = sharedContainerURL(in: items) {
            showSharedContainerImportRuntimeMenu(for: url, at: point)
            return
        }
        if passwordFieldFrame.contains(point), pendingPasswordAction != nil {
            let index = characterIndexForPasswordField(xPosition: point.x)
            focusPasswordField(cursorPosition: index)
            _ = insertPasteboardItemsIntoPasswordField(items)
            return
        }

        guard let drop = createFieldDropPoint(point) else { return }
        let index = characterIndexForCreateField(key: drop.key, at: drop.point)
        focusCreateField(drop.key, cursorPosition: index)
        _ = insertPasteboardItemsIntoCreateField(items)
    }

    @discardableResult
    private func handleLogHeaderMouseDown(at point: CGPoint,
                                          modifierFlags: NSEvent.ModifierFlags,
                                          clickCount: Int) -> Bool {
        _ = modifierFlags
        if isPointInLogDismissButton(point) {
            let frame = rootFrame(logDismissFrame.insetBy(dx: -2, dy: -2), from: logHeaderLayer)
            armButtonClick(frame: frame, action: .logDismiss)
            return true
        }
        if isPointInLogSelector(point) {
            blurCreateField()
            blurPasswordField()
            showLogSelectorMenu(at: point)
            return true
        }
        guard isPointInLogHeaderDetail(point) else { return false }
        blurCreateField()
        blurPasswordField()

        let offset = logHeaderDetailCharacterIndex(atRootPoint: point)
        logHeaderDetailDragAnchorOffset = offset
        if clickCount >= 3 {
            setLogHeaderDetailSelectionRange(NSRange(location: 0, length: (renderedLogHeaderDetailText as NSString).length))
        } else if clickCount == 2 {
            setLogHeaderDetailSelectionRange(logHeaderDetailWordRange(containing: offset))
        } else {
            setLogHeaderDetailSelectionRange(nil)
        }
        return true
    }

    @discardableResult
    private func handleLogHeaderRightMouseDown(at point: CGPoint) -> Bool {
        if isPointInLogDismissButton(point) {
            return true
        }
        if isPointInLogSelector(point) {
            blurCreateField()
            blurPasswordField()
            showLogSelectorMenu(at: point)
            return true
        }
        guard isPointInLogHeaderDetail(point) else { return false }
        blurCreateField()
        blurPasswordField()

        let offset = logHeaderDetailCharacterIndex(atRootPoint: point)
        if let selectionRange = normalizedLogHeaderDetailSelectionRange(logHeaderDetailSelectionRange),
           offset >= selectionRange.location,
           offset <= selectionRange.location + selectionRange.length,
           let selectedText = selectedLogHeaderDetailAttributedText() {
            outerframeHost.showContextMenu(for: selectedText, at: point)
            return true
        }

        let fullRange = NSRange(location: 0, length: (renderedLogHeaderDetailText as NSString).length)
        setLogHeaderDetailSelectionRange(fullRange)
        outerframeHost.showContextMenu(for: selectedLogHeaderDetailAttributedText() ?? logHeaderDetailAttributedString(), at: point)
        return true
    }

    private func handleLogHeaderMouseDragged(to point: CGPoint) -> Bool {
        guard let logHeaderDetailDragAnchorOffset else { return false }
        let offset = logHeaderDetailCharacterIndex(atRootPoint: point)
        let location = min(logHeaderDetailDragAnchorOffset, offset)
        let length = abs(offset - logHeaderDetailDragAnchorOffset)
        setLogHeaderDetailSelectionRange(NSRange(location: location, length: length))
        return true
    }

    private func logHeaderDetailPoint(fromRootPoint point: CGPoint) -> CGPoint {
        let contentPoint = contentLayer.convert(point, from: rootLayer)
        return logHeaderLayer.convert(contentPoint, from: contentLayer)
    }

    private func isPointInLogDismissButton(_ point: CGPoint) -> Bool {
        guard mode == .apps,
              selectedServiceID != nil,
              !logHeaderLayer.isHidden else { return false }
        let localPoint = logHeaderDetailPoint(fromRootPoint: point)
        return logDismissFrame.insetBy(dx: -2, dy: -2).contains(localPoint)
    }

    private func isPointInLogHeaderDetail(_ point: CGPoint) -> Bool {
        guard mode == .apps,
              selectedServiceID != nil,
              !logHeaderLayer.isHidden else { return false }
        let localPoint = logHeaderDetailPoint(fromRootPoint: point)
        return logHeaderDetailFrame.insetBy(dx: 0, dy: -3).contains(localPoint)
    }

    private func isPointInLogSelector(_ point: CGPoint) -> Bool {
        guard mode == .apps,
              selectedServiceID != nil,
              !logHeaderLayer.isHidden,
              !logSelectorFrame.isEmpty else { return false }
        let localPoint = logHeaderDetailPoint(fromRootPoint: point)
        return logSelectorFrame.insetBy(dx: -2, dy: -2).contains(localPoint)
    }

    private func logHeaderDetailCharacterIndex(atRootPoint point: CGPoint) -> Int {
        let length = (renderedLogHeaderDetailText as NSString).length
        guard length > 0 else { return 0 }

        let localPoint = logHeaderDetailPoint(fromRootPoint: point)
        let x = max(localPoint.x - logHeaderDetailFrame.minX, 0)
        if x <= 0 {
            return 0
        }

        let line = logHeaderDetailLine()
        let index = CTLineGetStringIndexForPosition(line, CGPoint(x: x, y: 0))
        if index == kCFNotFound {
            return length
        }
        return min(max(index, 0), length)
    }

    private func logHeaderDetailWordRange(containing offset: Int) -> NSRange? {
        let string = renderedLogHeaderDetailText as NSString
        let length = string.length
        guard length > 0 else { return nil }

        var location = min(max(offset, 0), length - 1)
        if location > 0, !logHeaderDetailCharacterIsWordLike(string.character(at: location)) {
            location -= 1
        }
        guard logHeaderDetailCharacterIsWordLike(string.character(at: location)) else {
            return NSRange(location: min(max(offset, 0), length), length: 0)
        }

        var start = location
        while start > 0, logHeaderDetailCharacterIsWordLike(string.character(at: start - 1)) {
            start -= 1
        }

        var end = location + 1
        while end < length, logHeaderDetailCharacterIsWordLike(string.character(at: end)) {
            end += 1
        }
        return NSRange(location: start, length: end - start)
    }

    private func logHeaderDetailCharacterIsWordLike(_ character: unichar) -> Bool {
        if character >= 48 && character <= 57 { return true }
        if character >= 65 && character <= 90 { return true }
        if character >= 97 && character <= 122 { return true }
        return character == 45 || character == 46 || character == 47 || character == 95 || character == 126
    }

    @discardableResult
    private func handleLogMouseDown(at point: CGPoint,
                                    modifierFlags: NSEvent.ModifierFlags,
                                    clickCount: Int) -> Bool {
        guard isPointInLogTextRegion(point) else { return false }
        blurCreateField()
        blurPasswordField()
        clearLogHeaderDetailSelection()

        let textPoint = logTextContainerPoint(fromRootPoint: point)
        let anchorOffset = logTextOffset(atTextPoint: textPoint)
        logDragAnchorOffset = anchorOffset
        lastLogDragTextPoint = nil

        if clickCount >= 3 {
            let fragmentSelection = logTextLayoutFragmentSelection(at: textPoint)
            setLogTextSelection(fragmentSelection)
        } else if clickCount == 2, let selection = logTextSelection(at: point) {
            let wordSelection = logTextLayoutManager.textSelectionNavigation.textSelection(for: .word,
                                                                                          enclosing: selection)
            setLogTextSelection(wordSelection)
        } else if !modifierFlags.contains(.shift) {
            setLogTextSelectionRange(nil)
        }
        return true
    }

    @discardableResult
    private func handleLogRightMouseDown(at point: CGPoint) -> Bool {
        guard isPointInLogTextRegion(point) else { return false }

        if let location = logTextLocationOffset(at: point),
           let selectionRange = logTextSelectionRange,
           NSLocationInRange(location, selectionRange),
           let selectedText = selectedLogAttributedText() {
            outerframeHost.showContextMenu(for: selectedText, at: point)
            return true
        }

        guard let selection = logTextSelection(at: point) else { return true }
        let wordSelection = logTextLayoutManager.textSelectionNavigation.textSelection(for: .word,
                                                                                      enclosing: selection)
        guard let selectedText = logAttributedText(for: wordSelection),
              !selectedText.string.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return true
        }

        setLogTextSelection(wordSelection)
        outerframeHost.showContextMenu(for: selectedText, at: point)
        return true
    }

    private func handleLogMouseDragged(to point: CGPoint) -> Bool {
        guard let logDragAnchorOffset else { return false }
        let textPoint = logTextContainerPoint(fromRootPoint: point)
        if let lastLogDragTextPoint,
           abs(lastLogDragTextPoint.x - textPoint.x) < 0.5,
           abs(lastLogDragTextPoint.y - textPoint.y) < 0.5 {
            return true
        }
        lastLogDragTextPoint = textPoint
        let offset = logTextOffset(atTextPoint: textPoint)
        let location = min(logDragAnchorOffset, offset)
        let length = abs(offset - logDragAnchorOffset)
        setLogTextSelectionRange(NSRange(location: location, length: length))
        return true
    }

    private func logTextSelection(at point: CGPoint) -> NSTextSelection? {
        let textPoint = logTextContainerPoint(fromRootPoint: point)
        return logTextLayoutManager.textSelectionNavigation.textSelections(interactingAt: textPoint,
                                                                           inContainerAt: logTextLayoutManager.documentRange.location,
                                                                           anchors: [],
                                                                           modifiers: [],
                                                                           selecting: false,
                                                                           bounds: logTextInteractionBounds()).first
    }

    private func logTextLayoutFragmentSelection(at textPoint: CGPoint) -> NSTextSelection? {
        var matchingFragment: NSTextLayoutFragment?
        logTextLayoutManager.enumerateTextLayoutFragments(from: logTextLayoutManager.documentRange.location,
                                                          options: [.ensuresLayout]) { fragment in
            let frame = fragment.layoutFragmentFrame
            if frame.minY > textPoint.y {
                return false
            }
            let paragraphHitFrame = CGRect(x: 0,
                                           y: frame.minY,
                                           width: logTextContainer.size.width,
                                           height: max(frame.height, 1))
            if paragraphHitFrame.insetBy(dx: 0, dy: -2).contains(textPoint) {
                matchingFragment = fragment
                return false
            }
            return true
        }

        guard let range = matchingFragment?.rangeInElement else { return nil }
        return NSTextSelection([range], affinity: .downstream, granularity: .paragraph)
    }

    private func logTextLocationOffset(at point: CGPoint) -> Int? {
        guard isPointInLogTextRegion(point) else { return nil }
        return logTextOffset(atTextPoint: logTextContainerPoint(fromRootPoint: point))
    }

    private func isPointInLogTextRegion(_ point: CGPoint) -> Bool {
        guard mode == .apps,
              selectedServiceID != nil,
              pendingInstallBackend == nil,
              pendingPasswordAction == nil,
              pendingFilePicker == nil else { return false }
        let contentPoint = contentLayer.convert(point, from: rootLayer)
        return logRowsClipLayer.frame.contains(contentPoint)
    }

    private func isPointOverLogText(_ point: CGPoint) -> Bool {
        guard isPointInLogTextRegion(point) else { return false }
        let textPoint = logTextContainerPoint(fromRootPoint: point)
        let textWidth = max(logRowsClipLayer.bounds.width - logTextInsetX * 2, 1)
        return textPoint.x >= -1 &&
               textPoint.x <= textWidth + 1 &&
               textPoint.y >= -2 &&
               textPoint.y <= logScroll + logRowsClipLayer.bounds.height + 2
    }

    private func logTextContainerPoint(fromRootPoint point: CGPoint) -> CGPoint {
        let contentPoint = contentLayer.convert(point, from: rootLayer)
        let localPoint = logRowsClipLayer.convert(contentPoint, from: contentLayer)
        let topDownPoint = CGPoint(x: localPoint.x,
                                   y: logRowsClipLayer.bounds.height - localPoint.y)
        return CGPoint(x: topDownPoint.x - logTextInsetX,
                       y: topDownPoint.y + logScroll - logTextInsetY)
    }

    private func logTextInteractionBounds() -> CGRect {
        CGRect(x: 0,
               y: 0,
               width: logTextContainer.size.width,
               height: max(logContentHeight() - logTextInsetY * 2, logTextContainer.size.height))
    }

    private func beginDraggingSelectedCreateText(_ text: String) {
        guard !text.isEmpty else { return }
        let item = OuterframeContentPasteboardItem(representations: [
            OuterframeContentPasteboardRepresentation(typeIdentifier: NSPasteboard.PasteboardType.string.rawValue,
                                                      data: Data(text.utf8))
        ])
        outerframeHost.beginDraggingPasteboardItem(item,
                                                   operationMask: .copy,
                                                   previewPNGData: nil,
                                                   previewSize: nil)
    }

    private func armButtonClick(frame: CGRect, action: PendingButtonAction) {
        pendingButtonClick = PendingButtonClick(frame: frame, action: action)
    }

    private func armButtonClick(frame: CGRect,
                                perform action: @escaping () -> Void) {
        armButtonClick(frame: frame, action: .perform(action))
    }

    private func armButtonClick(frame: CGRect,
                                performAtPoint action: @escaping (CGPoint) -> Void) {
        armButtonClick(frame: frame, action: .performAtPoint(action))
    }

    private func rootFrame(_ frame: CGRect, from layer: CALayer) -> CGRect {
        rootLayer.convert(frame, from: layer)
    }

    @discardableResult
    private func handlePendingButtonMouseUp(at point: CGPoint) -> Bool {
        guard let pendingButtonClick else {
            return false
        }
        self.pendingButtonClick = nil
        guard pendingButtonClick.frame.contains(point) else {
            return true
        }
        performPendingButtonAction(pendingButtonClick.action, at: point)
        return true
    }

    private func performPendingButtonAction(_ action: PendingButtonAction,
                                            at point: CGPoint) {
        switch action {
        case .addApp:
            navigateToMode(.create, pushHistory: true)
        case .appBadge(let endpoint, let displayName, let opensInNewTab):
            openLauncherEndpoint(endpoint,
                                 displayName: displayName,
                                 opensInNewTab: opensInNewTab)
        case .bundledInstall(let backend):
            showInstallPrompt(for: backend)
        case .createCancel:
            dismissCreateOverlay()
        case .createChoice(let key, let value):
            if key == "nativeTargetHTML" || key == "nativeTargetMacOS" {
                createValues[key] = createValues[key, default: "true"] == "true" ? "false" : "true"
            } else {
                createValues[key] = value
            }
            if createInputController.isFocused, activeCreateFieldKey == key {
                focusCreateField(key)
            }
            createMessage = ""
            updateLayout()
        case .createSubmit:
            submitCreateForm()
        case .createSection(let section):
            blurCreateField()
            selectedCreateSection = section
            if section == .bashCommands {
                ensureBashDefaults()
            } else if section == .nativeApp {
                ensureNativeAppDefaults()
            }
            createMessage = ""
            updateLayout()
        case .createSuggestion(let key, let value):
            createValues[key] = value
            if createInputController.isFocused, activeCreateFieldKey == key {
                focusCreateField(key)
            }
            createMessage = ""
            updateLayout()
        case .chooseBashIcon:
            showBashIconFilePicker()
        case .directorySelect(let key):
            showDirectoryPicker(for: key)
        case .filePickerCancel:
            dismissFilePicker()
        case .filePickerEntry(let entry):
            activateFilePickerEntry(entry)
        case .filePickerSave:
            confirmFilePickerSave()
        case .installCancel:
            dismissInstallPrompt()
        case .installConfirm(let operation):
            pendingInstallOperation = operation
            confirmPendingInstall()
        case .logDismiss:
            dismissLogViewer()
        case .updateCancel:
            pendingOuterShellUpdate = nil
            updateLayout()
        case .updateConfirm:
            confirmOuterShellUpdate()
        case .aboutDismiss:
            dismissAboutPrompt()
        case .passwordCancel:
            dismissPasswordPrompt()
        case .passwordSubmit:
            submitPasswordPrompt()
        case .recipe(let recipeID):
            blurCreateField()
            selectedRecipeID = recipeID
            applyRecipeDefaults(overwrite: true)
            createMessage = ""
            updateLayout()
        case .perform(let action):
            action()
        case .performAtPoint(let action):
            action(point)
        }
    }

    private func handleMouseDragged(to point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        _ = modifierFlags
        if var drag = pendingOverviewGroupDrag {
            drag.current = point
            if hypot(point.x - drag.start.x, point.y - drag.start.y) > 5 { drag.active = true }
            pendingOverviewGroupDrag = drag
            if drag.active { setCursorIfNeeded(.closedHand) }
            scheduleOverviewDragFrame()
            return
        }
        if pendingSharedContainerDrag {
            pendingSharedContainerDrag = false
            beginDraggingSharedContainer()
            return
        }
        if handleStatusMouseDragged(to: point) {
            return
        }
        if pendingFilePicker != nil,
           filePickerScrollbarController?.handleMouseDragged(to: rootLayer.convert(point, to: filePickerListLayer)) == true {
            return
        }
        if logScrollbarController?.handleMouseDragged(to: rootLayer.convert(point, to: logRowsClipLayer)) == true {
            return
        }
        if handleAboutMouseDragged(to: point) {
            return
        }
        if handleLogHeaderMouseDragged(to: point) {
            return
        }
        if handleLogMouseDragged(to: point) {
            return
        }
        if handleTextSelectionMouseDragged(to: point) {
            return
        }
        if handleDockerfileMouseDragged(to: point) {
            return
        }
        if var pendingAppDrag {
            pendingAppDrag.currentPoint = point
            let dx = point.x - pendingAppDrag.startPoint.x
            let dy = point.y - pendingAppDrag.startPoint.y
            if hypot(dx, dy) >= 4 {
                pendingAppDrag.isDragging = true
                setCursorIfNeeded(.closedHand)
            }
            self.pendingAppDrag = pendingAppDrag
            if pendingAppDrag.isDragging {
                scheduleOverviewDragFrame()
            }
            return
        }
        if let pendingNativeProjectDrag {
            let dx = point.x - pendingNativeProjectDrag.startPoint.x
            let dy = point.y - pendingNativeProjectDrag.startPoint.y
            guard hypot(dx, dy) >= 3 else { return }
            self.pendingNativeProjectDrag = nil
            beginDraggingGeneratedNativeProject(pendingNativeProjectDrag.project)
            return
        }
        guard let pendingCreateTextDrag else { return }
        let dx = point.x - pendingCreateTextDrag.startPoint.x
        let dy = point.y - pendingCreateTextDrag.startPoint.y
        guard hypot(dx, dy) >= 3 else { return }
        self.pendingCreateTextDrag = nil
        beginDraggingSelectedCreateText(pendingCreateTextDrag.selectedText)
    }

    private func handleTextSelectionMouseDragged(to point: CGPoint) -> Bool {
        guard let pendingTextSelectionDrag else {
            return false
        }
        switch pendingTextSelectionDrag.target {
        case .workspaceName:
            guard isWorkspaceNamePromptVisible,
                  workspaceRenameInputController.isFocused else {
                self.pendingTextSelectionDrag = nil
                return false
            }
            let index = characterIndexForWorkspaceRenameField(at: point)
            if isContainerConfigurationEditorVisible &&
                !isRenamingContainerConfiguration {
                let hadSelection = workspaceRenameInputController.hasSelection
                workspaceRenameInputController.setCursorPosition(
                    index,
                    modifySelection: true,
                    notifyDelegate: false
                )
                updateContainerConfigurationSelectionLayers()
                if hadSelection != workspaceRenameInputController.hasSelection {
                    sendWorkspaceRenameTextInputGeometryUpdate()
                    updateEditingAndPasteboardState()
                }
            } else {
                workspaceRenameInputController.setCursorPosition(index, modifySelection: true)
                sendWorkspaceRenameTextInputGeometryUpdate()
                updateLayout()
            }
            return true
        case .createMessage:
            guard mode == .create,
                  !createMessageFrame.isEmpty,
                  let anchor = createMessageDragAnchorOffset else {
                self.pendingTextSelectionDrag = nil
                return false
            }
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            let offset = createMessageCharacterIndex(at: createPoint)
            let location = min(anchor, offset)
            let length = abs(offset - anchor)
            setCreateMessageSelectionRange(NSRange(location: location, length: length))
            return true
        case .password:
            guard pendingPasswordAction != nil,
                  passwordInputController.isFocused else {
                self.pendingTextSelectionDrag = nil
                return false
            }
            let index = characterIndexForPasswordField(xPosition: point.x)
            passwordInputController.setCursorPosition(index, modifySelection: true)
            sendPasswordFieldTextInputGeometryUpdate()
            updateLayout()
            return true
        case .createField(let key):
            guard mode == .create,
                  createInputController.isFocused,
                  activeCreateFieldKey == key else {
                self.pendingTextSelectionDrag = nil
                return false
            }
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            let index = characterIndexForCreateField(key: key, at: createPoint)
            createInputController.setCursorPosition(index, modifySelection: true)
            sendCreateFieldTextInputGeometryUpdate()
            updateLayout()
            return true
        }
    }

    private func handleMouseUp(at point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        if let drag = pendingOverviewGroupDrag {
            pendingOverviewGroupDrag = nil
            let targetPoint = appsScrollContentLayer.convert(point, from: rootLayer)
            if drag.active, let target = overviewGroupDrop(at: targetPoint, source: drag.id) {
                var layout = overviewLayout
                var groups = overviewGroupFrames.map(\.id).filter { $0 != drag.id }
                if let destination = groups.firstIndex(of: target.id) {
                    groups.insert(drag.id, at: destination + (target.after ? 1 : 0))
                    if groups != overviewGroupFrames.map(\.id) {
                        layout.groups = groups
                        saveOverviewLayout(layout)
                    }
                }
            }
            setCursorIfNeeded(.arrow)
            updateLayout()
            return
        }
        statusDragAnchorOffset = nil
        aboutDragAnchorOffset = nil
        logDragAnchorOffset = nil
        logHeaderDetailDragAnchorOffset = nil
        createMessageDragAnchorOffset = nil
        dockerfileDragAnchorOffset = nil
        lastLogDragTextPoint = nil
        let finishedContainerTextSelection: Bool
        if isContainerConfigurationEditorVisible &&
            !isRenamingContainerConfiguration,
           let pendingTextSelectionDrag,
           case .workspaceName = pendingTextSelectionDrag.target {
            finishedContainerTextSelection = true
        } else {
            finishedContainerTextSelection = false
        }
        pendingTextSelectionDrag = nil
        if finishedContainerTextSelection {
            updateLayout()
        }
        if pendingSharedContainerDrag {
            pendingSharedContainerDrag = false
            return
        }
        if pendingFilePicker != nil {
            _ = filePickerScrollbarController?.handleMouseUp(at: rootLayer.convert(point, to: filePickerListLayer))
        }
        _ = logScrollbarController?.handleMouseUp(at: rootLayer.convert(point, to: logRowsClipLayer))
        if handlePendingButtonMouseUp(at: point) {
            return
        }
        if let pendingAppDrag {
            self.pendingAppDrag = nil
            if pendingAppDrag.isDragging {
                let contentPoint = contentLayer.convert(point, from: rootLayer)
                let appsPoint = appsContentPoint(for: contentPoint)
                if let drop = overviewEndpointDrop(at: appsPoint, for: pendingAppDrag.item) {
                    moveOverviewItem(pendingAppDrag.item, pinned: drop.target == .pinned, before: drop.before)
                } else if let target = appDropTarget(at: appsPoint, for: pendingAppDrag.item) {
                    let before = appCardFrames.first {
                        $0.item.identityKey != pendingAppDrag.item.identityKey &&
                        overviewGroupID(for: $0.item) == overviewGroupID(for: pendingAppDrag.item) &&
                        $0.frame.contains(appsPoint)
                    }?.item
                    moveOverviewItem(pendingAppDrag.item, pinned: target == .pinned, before: before)
                } else {
                    updateLayout()
                }
            } else {
                openLauncherItem(pendingAppDrag.item, opensInNewTab: modifierFlags.contains(.command))
            }
            setCursorIfNeeded(.arrow)
            return
        }
        if pendingNativeProjectDrag != nil {
            pendingNativeProjectDrag = nil
            return
        }
        guard let pendingCreateTextDrag else { return }
        self.pendingCreateTextDrag = nil
        focusActiveCreateField()
        createInputController.setCursorPosition(pendingCreateTextDrag.cursorIndex, modifySelection: false)
    }

    private func handleMouseMoved(to point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        _ = modifierFlags
        if isShowingWorkspacePanel {
            if isContainerConfigurationEditorVisible {
                if containerConfigurationBuildError != nil {
                    if containerConfigurationBuildErrorTextFrame.contains(point) {
                        setCursorIfNeeded(.iBeam)
                    } else if containerConfigurationBuildErrorCopyFrame.contains(point) ||
                                containerConfigurationBuildErrorDismissFrame.contains(point) {
                        setCursorIfNeeded(.pointingHand)
                    } else {
                        setCursorIfNeeded(.arrow)
                    }
                } else if isRenamingContainerConfiguration {
                    if workspaceRenameFieldFrame.contains(point) {
                        setCursorIfNeeded(.iBeam)
                    } else {
                        let isInteractive = workspaceRenameCancelFrame.contains(point) ||
                            workspaceRenameConfirmFrame.contains(point)
                        setCursorIfNeeded(isInteractive ? .pointingHand : .arrow)
                    }
                } else if isConfirmingContainerConfigurationDismissal {
                    let isInteractive = containerConfigurationDismissSaveFrame.contains(point) ||
                        containerConfigurationDismissWithoutSavingFrame.contains(point) ||
                        containerConfigurationDismissCancelFrame.contains(point)
                    setCursorIfNeeded(isInteractive ? .pointingHand : .arrow)
                } else if isConfirmingContainerConfigurationRebuild {
                    let isInteractive = containerConfigurationRebuildSaveFrame.contains(point) ||
                        containerConfigurationRebuildWithoutSavingFrame.contains(point) ||
                        containerConfigurationRebuildCancelFrame.contains(point)
                    setCursorIfNeeded(isInteractive ? .pointingHand : .arrow)
                } else if workspaceCloseFrame.contains(point) ||
                    containerConfigurationRenameFrame.contains(point) ||
                    containerConfigurationCookbookFrame.contains(point) ||
                    containerConfigurationCopyPathFrame.contains(point) {
                    setCursorIfNeeded(.pointingHand)
                } else if workspaceRenameFieldFrame.contains(point) &&
                            !containerConfigurationEditorToolbarFrame.contains(point) {
                    setCursorIfNeeded(.iBeam)
                } else {
                    let panelPoint = workspacePanelLayer.convert(point, from: rootLayer)
                    if dockerfileTextBlock(
                        at: panelPoint,
                        contentSpace: .workspacePanel
                    ) != nil {
                        setCursorIfNeeded(.iBeam)
                        return
                    }
                    let isInteractive =
                        containerConfigurationDockerfileTabFrame.contains(point) ||
                        containerConfigurationMountsTabFrame.contains(point) ||
                        containerConfigurationEnvironmentTabFrame.contains(point) ||
                        containerConfigurationPortsTabFrame.contains(point) ||
                        containerConfigurationRuntimeTabFrame.contains(point) ||
                        containerConfigurationChangeRuntimeFrame.contains(point) ||
                        containerConfigurationCookbookFrame.contains(point) ||
                        containerConfigurationCopyPathFrame.contains(point) ||
                        containerConfigurationAddMountFrame.contains(point) ||
                        containerConfigurationSaveFrame.contains(point) ||
                        containerConfigurationDiscardFrame.contains(point) ||
                        containerConfigurationRebuildFrame.contains(point) ||
                        containerConfigurationMountActionFrames.contains {
                            $0.frame.contains(point)
                        }
                    setCursorIfNeeded(isInteractive ? .pointingHand : .arrow)
                }
                return
            }
            if pendingWorkspaceDeletion != nil {
                let isInteractive = workspaceDeleteCancelFrame.contains(point) ||
                    workspaceDeleteConfirmFrame.contains(point)
                setCursorIfNeeded(isInteractive ? .pointingHand : .arrow)
                return
            }
            if isWorkspaceNamePromptVisible {
                let isInteractive = workspaceRenameFieldFrame.contains(point) ||
                    workspaceCreationBaseImageFrame.contains(point) ||
                    workspaceOuterShellBaseImageFrame.contains(point) ||
                    workspaceCustomBaseImageFrame.contains(point) ||
                    workspaceCustomAsIsFrame.contains(point) ||
                    workspaceRenameCancelFrame.contains(point) ||
                    workspaceRenameConfirmFrame.contains(point)
                setCursorIfNeeded(
                    workspaceRenameFieldFrame.contains(point) &&
                        (!isBaseImageChoicePrompt || baseImageTemplate != .outerShell)
                        ? .iBeam
                        : (isInteractive ? .pointingHand : .arrow)
                )
                return
            }
            let isInteractive = workspaceCloseFrame.contains(point)
            setCursorIfNeeded(isInteractive ? .pointingHand : .arrow)
            return
        }
        if pendingInstallBackend != nil {
            setCursorIfNeeded((installConfirmFrame.contains(point) || installRootConfirmFrame.contains(point) || installCancelFrame.contains(point)) ? .pointingHand : .arrow)
            return
        }
        if pendingAboutBackend != nil {
            if aboutDoneFrame.contains(point) {
                setCursorIfNeeded(.pointingHand)
            } else if aboutTextFrame.contains(point) {
                setCursorIfNeeded(.iBeam)
            } else {
                setCursorIfNeeded(.arrow)
            }
            return
        }
        if pendingFilePicker != nil, mode == .create {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            if filePickerSaveFrame.contains(createPoint) ||
               filePickerCancelFrame.contains(createPoint) ||
               FilePickerBreadcrumbBar.hitPath(at: createPoint,
                                               breadcrumbFrame: filePickerBreadcrumbFrame,
                                               segmentFrames: filePickerBreadcrumbSegmentFrames) != nil ||
               (filePickerListFrame.contains(createPoint) &&
                filePickerEntryFrames.contains(where: { $0.frame.contains(createPoint) })) {
                setCursorIfNeeded(.pointingHand)
            } else {
                setCursorIfNeeded(.arrow)
            }
            return
        }
        let isOverCreateField = pendingPasswordAction == nil && createFieldDropPoint(point) != nil
        let isOverPasswordField = pendingPasswordAction != nil && passwordFieldFrame.contains(point)
        var isOverCreateMessage = false
        var isOverBundledApp = false
        var isOverDirectorySelect = false
        var isOverAppTile = false
        if pendingPasswordAction == nil, mode == .create {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            let isInCreateContent = createContentClipFrame.contains(createPoint)
            isOverCreateMessage = isInCreateContent && !createMessage.isEmpty && createMessageFrame.insetBy(dx: 0, dy: -3).contains(createPoint)
            isOverBundledApp = isInCreateContent && bundledAppInstallFrames.contains { $0.frame.contains(createPoint) }
            isOverDirectorySelect = isInCreateContent && createDirectorySelectFrames.contains { $0.frame.contains(createPoint) }
            isOverAppTile = createDismissFrame.contains(createPoint) ||
                            (isInCreateContent && (createSectionFrames.contains { $0.frame.contains(createPoint) } ||
                                                   recipeFrames.contains { $0.frame.contains(createPoint) } ||
                                                   createButtonFrame.contains(createPoint) ||
                                                   bashIconSelectFrame.contains(createPoint) ||
                                                   createChoiceFrames.contains { $0.frame.contains(createPoint) } ||
                                                   createSuggestionFrames.contains { $0.frame.contains(createPoint) }))
        } else if pendingPasswordAction == nil, mode == .apps {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let appsPoint = appsContentPoint(for: contentPoint)
            let toolbarPoint = toolbarLayer.convert(point, from: rootLayer)
            if dockerfileTextBlock(
                at: appsPoint,
                contentSpace: appsTextContentSpace(for: contentPoint)
            ) != nil {
                setCursorIfNeeded(.iBeam)
                return
            }
            isOverAppTile = appCardFrames.contains { $0.frame.contains(appsPoint) } ||
                            appBadgeFrames.contains { $0.frame.contains(appsPoint) } ||
                            appOverflowFrames.contains { $0.frame.contains(appsPoint) } ||
                            workspaceOverviewCreateFrame.contains(appsPoint) ||
                            workspaceOverviewActionFrames.contains { $0.frame.contains(appsPoint) } ||
                            workspaceOverviewAppFrames.contains { $0.frame.contains(appsPoint) } ||
                            safeSpaceDetailBackFrame.contains(appsPoint) ||
                            safeSpaceDetailAddUserFrame.contains(appsPoint) ||
                            safeSpaceDetailAddStepFrame.contains(appsPoint) ||
                            safeSpaceDetailRebuildFrame.contains(appsPoint) ||
                            safeSpaceDetailCopyContainerfileFrame.contains(appsPoint) ||
                            safeSpaceDetailCopySupportSnippetFrame.contains(appsPoint) ||
                            safeSpaceDetailCopyRecipeMessageFrame.contains(appsPoint) ||
                            safeSpaceDetailEditStepFrames.contains { $0.frame.contains(appsPoint) } ||
                            safeSpaceDetailStepFrames.contains { $0.frame.contains(appsPoint) } ||
                            overviewAddFrames.contains { $0.contains(appsPoint) } ||
                            addAppFrame.contains(appsPoint) ||
                            outerShellActionFrame.contains(toolbarPoint)
        }
        if isOverCreateField || isOverPasswordField || isOverCreateMessage {
            setCursorIfNeeded(.iBeam)
        } else if isPointInLogDismissButton(point) || isPointInLogSelector(point) {
            setCursorIfNeeded(.pointingHand)
        } else if isPointInLogHeaderDetail(point) {
            setCursorIfNeeded(.iBeam)
        } else if isPointOverLogText(point) {
            setCursorIfNeeded(.iBeam)
        } else if isOverBundledApp || isOverDirectorySelect || isOverAppTile {
            setCursorIfNeeded(.pointingHand)
        } else {
            setCursorIfNeeded(.arrow)
        }
    }

    private func setCursorIfNeeded(_ cursor: PluginCursorType) {
        guard currentCursor != cursor else { return }
        currentCursor = cursor
        outerframeHost.setCursor(cursor)
    }

    private func handleRightMouseDown(at point: CGPoint,
                                      modifierFlags: NSEvent.ModifierFlags,
                                      clickCount: Int) {
        _ = modifierFlags
        _ = clickCount
        if handleStatusRightMouseDown(at: point) {
            return
        }
        if isShowingWorkspacePanel {
            if containerConfigurationBuildError != nil {
                let panelPoint = workspacePanelLayer.convert(point, from: rootLayer)
                _ = handleDockerfileRightMouseDown(
                    at: panelPoint,
                    contentSpace: .workspacePanel,
                    rootPoint: point
                )
                return
            }
            if isContainerConfigurationEditorVisible {
                let panelPoint = workspacePanelLayer.convert(point, from: rootLayer)
                if handleDockerfileRightMouseDown(
                    at: panelPoint,
                    contentSpace: .workspacePanel,
                    rootPoint: point
                ) {
                    return
                }
            }
            if isContainerConfigurationEditorVisible,
               containerConfigurationCopyPathFrame.contains(point) {
                showContainerPathMenu(at: point)
                return
            }
            guard isWorkspaceNamePromptVisible,
                  workspaceRenameFieldFrame.contains(point),
                  !containerConfigurationEditorToolbarFrame.contains(point) else {
                return
            }
            let index = characterIndexForWorkspaceRenameField(at: point)
            focusWorkspaceRenameField()
            if workspaceRenameInputController.selectionRange?.contains(index) != true {
                workspaceRenameInputController.setCursorPosition(index, modifySelection: false)
            }
            updateEditingAndPasteboardState()
            showWorkspaceNameFieldContextMenu(at: point)
            return
        }
        if pendingAboutBackend != nil {
            handleAboutRightMouseDown(at: point)
            return
        }
        if handleLogHeaderRightMouseDown(at: point) {
            return
        }
        if handleLogRightMouseDown(at: point) {
            return
        }
        if pendingPasswordAction != nil {
            guard passwordFieldFrame.contains(point) else { return }
            let index = characterIndexForPasswordField(xPosition: point.x)
            focusPasswordField(cursorPosition: index)
            updateEditingAndPasteboardState()
            showPasswordFieldContextMenu(at: point)
            return
        }

        if pendingFilePicker != nil, mode == .create { return }

        let contentPoint = contentLayer.convert(point, from: rootLayer)
        if mode == .apps {
            let toolbarPoint = toolbarLayer.convert(point, from: rootLayer)
            if outerShellActionFrame.contains(toolbarPoint),
               let backend = outerShellActionsBackend() {
                showBackendActionsMenu(for: backend, at: point)
                return
            }
            let appsPoint = appsContentPoint(for: contentPoint)
            if handleDockerfileRightMouseDown(
                at: appsPoint,
                contentSpace: appsTextContentSpace(for: contentPoint),
                rootPoint: point
            ) {
                return
            }
            if let action = workspaceOverviewActionFrames.first(where: {
                $0.frame.contains(appsPoint)
            }), showWorkspaceOverviewCommandMenu(
                for: action.operation,
                in: action.workspace,
                at: point
            ) {
                return
            }
            if let card = appCardFrames.first(where: { $0.frame.contains(appsPoint) }) {
                showAppActionsMenu(for: card.item, at: point)
            }
            return
        }

        guard pendingPasswordAction == nil, mode == .create else { return }
        let createPoint = createLayer.convert(contentPoint, from: contentLayer)
        if !createMessage.isEmpty,
           createMessageFrame.insetBy(dx: 0, dy: -3).contains(createPoint) {
            blurCreateField()
            blurPasswordField()
            let offset = createMessageCharacterIndex(at: createPoint)
            if let selectionRange = normalizedCreateMessageSelectionRange(),
               offset >= selectionRange.location,
               offset <= selectionRange.location + selectionRange.length,
               let selectedText = selectedCreateMessageAttributedText() {
                outerframeHost.showContextMenu(for: selectedText, at: point)
                return
            }
            if let wordRange = createMessageWordRange(containing: offset),
               wordRange.length > 0 {
                setCreateMessageSelectionRange(wordRange)
            } else {
                setCreateMessageSelectionRange(NSRange(location: 0, length: (createMessage as NSString).length))
            }
            if let selectedText = selectedCreateMessageAttributedText() {
                outerframeHost.showContextMenu(for: selectedText, at: point)
            }
            return
        }
        guard let key = createFieldFrames.first(where: { $0.frame.contains(createPoint) })?.key else { return }
        let index = characterIndexForCreateField(key: key, at: createPoint)
        focusCreateField(key)
        if !createInputController.hasSelection {
            createInputController.setCursorPosition(index, modifySelection: false)
        }
        updateEditingAndPasteboardState()
        showCreateFieldContextMenu(at: point, monospaced: createFieldLayouts[key]?.monospaced ?? false)
    }

    private func showPasswordFieldContextMenu(at point: CGPoint) {
        let items = [
            OuterframeContextMenuItem(id: "paste",
                                      title: "Paste",
                                      action: .standardPaste,
                                      isEnabled: passwordInputController.isFocused)
        ]

        outerframeHost.showContextMenu(menuID: UUID(),
                                       items: items,
                                       at: point)
    }

    private func showWorkspaceNameFieldContextMenu(at point: CGPoint) {
        let selectedText = workspaceRenameInputController.selectedTextContent() ?? ""
        let hasSelection = !selectedText.isEmpty
        let items = [
            OuterframeContextMenuItem(id: "cut",
                                      title: "Cut",
                                      action: .standardCut,
                                      isEnabled: hasSelection),
            OuterframeContextMenuItem(id: "copy",
                                      title: "Copy",
                                      action: .standardCopy,
                                      isEnabled: hasSelection),
            OuterframeContextMenuItem(id: "paste",
                                      title: "Paste",
                                      action: .standardPaste,
                                      isEnabled: workspaceRenameInputController.isFocused),
            OuterframeContextMenuItem(id: "select-all",
                                      title: "Select All",
                                      action: .standardSelectAll,
                                      isEnabled: !workspaceRenameInputController.text.isEmpty)
        ]
        let attributedText = hasSelection
            ? NSAttributedString(
                string: selectedText,
                attributes: [.font: workspaceNameInputFont()]
            )
            : nil
        outerframeHost.showContextMenu(menuID: UUID(),
                                       items: items,
                                       at: point,
                                       attributedText: attributedText)
    }

    private func showCreateFieldContextMenu(at point: CGPoint, monospaced: Bool) {
        let selectedText = createInputController.selectedTextContent() ?? ""
        let hasSelection = !selectedText.isEmpty
        let items = [
            OuterframeContextMenuItem(id: "cut",
                                      title: "Cut",
                                      action: .standardCut,
                                      isEnabled: hasSelection),
            OuterframeContextMenuItem(id: "copy",
                                      title: "Copy",
                                      action: .standardCopy,
                                      isEnabled: hasSelection),
            OuterframeContextMenuItem(id: "paste",
                                      title: "Paste",
                                      action: .standardPaste,
                                      isEnabled: createInputController.isFocused)
        ]

        let attributedText = hasSelection
            ? NSAttributedString(string: selectedText,
                                 attributes: [.font: createInputFont(monospaced: monospaced)])
            : nil
        outerframeHost.showContextMenu(menuID: UUID(),
                                       items: items,
                                       at: point,
                                       attributedText: attributedText)
    }

    private func handleScroll(at point: CGPoint, delta: CGPoint, precise: Bool) {
        let multiplier: CGFloat = precise ? 1 : wheelScrollLineHeight
        if containerConfigurationBuildError != nil,
           containerConfigurationBuildErrorTextFrame.contains(point) {
            let maximum = max(
                containerConfigurationBuildErrorContentHeight -
                    containerConfigurationBuildErrorTextFrame.height,
                0
            )
            let nextScroll = min(
                max(containerConfigurationBuildErrorScroll - delta.y * multiplier, 0),
                maximum
            )
            scrollContainerConfigurationBuildError(to: nextScroll)
            return
        }
        if isContainerConfigurationEditorVisible,
           containerConfigurationTab == .mounts,
           containerConfigurationContentFrame.contains(point) {
            let listHeight = max(containerConfigurationContentFrame.height - 76, 0)
            let contentHeight = containerConfigurationMountContentHeight
            let maximum = max(contentHeight - listHeight, 0)
            containerConfigurationMountScroll = min(
                max(containerConfigurationMountScroll - delta.y * multiplier, 0),
                maximum
            )
            updateLayout()
            return
        }
        if isContainerConfigurationEditorVisible,
           workspaceRenameFieldFrame.contains(point),
           containerConfigurationTab == .dockerfile ||
            containerConfigurationTab == .environment ||
            containerConfigurationTab == .ports {
            let visibleFrame = containerConfigurationTextVisibleFrame
            let text = workspaceRenameInputController.isFocused
                ? workspaceRenameInputController.text
                : workspaceRenameName
            let measurementLayout = CreateFieldLayout(
                fieldFrame: workspaceRenameFieldFrame,
                textFrame: visibleFrame,
                key: "containerConfiguration",
                monospaced: true,
                multiline: true
            )
            let lineCount = createTextAreaLineFragments(
                text: text,
                layout: measurementLayout
            ).count
            let contentHeight = CGFloat(lineCount) * 16 + 8
            let maximum = max(contentHeight - visibleFrame.height, 0)
            let nextScroll = min(
                max(containerConfigurationTextScroll - delta.y * multiplier, 0),
                maximum
            )
            scrollContainerConfigurationText(to: nextScroll)
            return
        }
        let previousAppsScroll = appsScroll
        let previousWorkspaceScroll = workspaceScroll
        let previousCreateScroll = createScroll
        var didScrollCreateForm = false
        if mode == .apps {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            if selectedServiceID != nil && logRowsClipLayer.frame.contains(contentPoint) {
                logScrollbarController?.cancelAnimation()
                shouldScrollLogToBottomOnNextLayout = false
                logScroll -= delta.y * (precise ? 1 : logScrollLineHeight)
                logScroll = clampedLogScroll(logScroll)
                updateLogTextViewport()
                updateLogTextSelectionLayers()
                return
            } else if usesWorkspaceSplitLayout,
                      workspacePaneFrame.contains(
                        appsLayer.convert(contentPoint, from: contentLayer)
                      ) {
                workspaceScroll -= delta.y * multiplier
            } else {
                appsScroll -= delta.y * multiplier
            }
        } else if mode == .create {
            if pendingFilePicker != nil {
                let contentPoint = contentLayer.convert(point, from: rootLayer)
                let createPoint = createLayer.convert(contentPoint, from: contentLayer)
                if filePickerListFrame.contains(createPoint) {
                    setFilePickerScroll(filePickerScroll - delta.y * multiplier)
                    return
                }
            } else {
                createScroll -= delta.y * multiplier
                didScrollCreateForm = true
            }
        }
        clampScrollOffsets()
        if mode == .apps {
            let workspaceDelta = workspaceScroll - previousWorkspaceScroll
            if abs(workspaceDelta) > 0.1 {
                scrollWorkspaceWithoutRerender(deltaY: workspaceDelta)
            } else {
                scrollCurrentModeWithoutRerender(deltaY: appsScroll - previousAppsScroll)
            }
        } else if didScrollCreateForm {
            let scrollDelta = createScroll - previousCreateScroll
            if abs(scrollDelta) > 0.1 {
                scrollCurrentModeWithoutRerender(deltaY: scrollDelta)
            }
        } else {
            updateLayout()
        }
    }

    private func scrollContainerConfigurationText(to nextScroll: CGFloat) {
        let delta = nextScroll - containerConfigurationTextScroll
        guard abs(delta) > 0.001 else { return }
        containerConfigurationTextScroll = nextScroll
        workspaceRenameTextFrame = workspaceRenameTextFrame.offsetBy(dx: 0,
                                                                      dy: delta)
        withoutImplicitAnimations {
            if let layer = containerConfigurationTextViewportLayer {
                layer.frame = layer.frame.offsetBy(dx: 0, dy: delta)
            }
        }

        let refreshDistance = max(workspaceRenameFieldFrame.height * 0.65, 120)
        if abs(nextScroll - containerConfigurationRenderedTextScroll) >= refreshDistance {
            updateLayout()
        }
    }

    private func scrollContainerConfigurationBuildError(to nextScroll: CGFloat) {
        let delta = nextScroll - containerConfigurationBuildErrorScroll
        guard abs(delta) > 0.001 else { return }
        containerConfigurationBuildErrorScroll = nextScroll
        withoutImplicitAnimations {
            if let layer = containerConfigurationBuildErrorViewportLayer {
                layer.frame = layer.frame.offsetBy(dx: 0, dy: delta)
            }
        }

        if let index = safeSpaceDockerfileTextBlocks.firstIndex(where: {
            $0.fragmentID == "container-build-error"
        }) {
            let block = safeSpaceDockerfileTextBlocks[index]
            safeSpaceDockerfileTextBlocks[index] = DockerfileTextBlock(
                fragmentID: block.fragmentID,
                text: block.text,
                frame: block.frame,
                lines: block.lines.map {
                    DockerfileTextLine(
                        text: $0.text,
                        range: $0.range,
                        frame: $0.frame.offsetBy(dx: 0, dy: delta)
                    )
                },
                font: block.font,
                contentSpace: block.contentSpace,
                selectionLayer: block.selectionLayer
            )
            updateDockerfileSelectionLayers()
        }

        let refreshDistance = max(
            containerConfigurationBuildErrorTextFrame.height * 0.65,
            120
        )
        if abs(nextScroll - containerConfigurationBuildErrorRenderedScroll) >= refreshDistance {
            updateLayout()
        }
    }

    private func handleMouseDown(at point: CGPoint,
                                 modifierFlags: NSEvent.ModifierFlags = [],
                                 clickCount: Int = 1) {
        pendingCreateTextDrag = nil
        pendingTextSelectionDrag = nil
        pendingButtonClick = nil
        if handleStatusMouseDown(at: point, clickCount: clickCount) {
            return
        }
        if sharedContainerFile != nil {
            if sharedContainerCloseFrame.contains(point) {
                armButtonClick(frame: sharedContainerCloseFrame) { [weak self] in
                    self?.dismissSharedContainerPanel()
                }
            } else if sharedContainerDragFrame.contains(point) {
                pendingSharedContainerDrag = true
            } else if !workspacePanelFrame.contains(point) {
                dismissSharedContainerPanel()
            }
            return
        }
        if isContainerConfigurationEditorVisible,
           containerConfigurationBuildError == nil,
           !isRenamingContainerConfiguration,
           !isConfirmingContainerConfigurationDismissal,
           !isConfirmingContainerConfigurationRebuild,
           containerConfigurationTab == .mounts {
            let panelPoint = workspacePanelLayer.convert(point, from: rootLayer)
            if let block = dockerfileTextBlock(
                at: panelPoint,
                contentSpace: .workspacePanel
            ) {
                let offset = dockerfileTextOffset(at: panelPoint, in: block)
                dockerfileDragAnchorOffset = offset
                switch clickCount {
                case 3...:
                    setDockerfileSelection(
                        fragmentID: block.fragmentID,
                        range: NSRange(
                            location: 0,
                            length: (block.text as NSString).length
                        )
                    )
                case 2:
                    setDockerfileSelection(
                        fragmentID: block.fragmentID,
                        range: dockerfileWordRange(containing: offset, in: block)
                    )
                default:
                    setDockerfileSelection(fragmentID: block.fragmentID, range: nil)
                }
                return
            }
        }
        if isContainerConfigurationEditorVisible {
            if containerConfigurationBuildError != nil {
                if containerConfigurationBuildErrorCopyFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationBuildErrorCopyFrame) { [weak self] in
                        guard let self,
                              let error = self.containerConfigurationBuildError else {
                            return
                        }
                        self.copyTextToPasteboard(self.normalizedContainerBuildError(error))
                        self.showContainerBuildErrorCopiedConfirmation()
                    }
                } else if containerConfigurationBuildErrorDismissFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationBuildErrorDismissFrame) { [weak self] in
                        self?.dismissContainerConfigurationBuildError()
                    }
                } else {
                    let panelPoint = workspacePanelLayer.convert(point, from: rootLayer)
                    if let block = dockerfileTextBlock(
                        at: panelPoint,
                        contentSpace: .workspacePanel
                    ) {
                        let offset = dockerfileTextOffset(at: panelPoint, in: block)
                        dockerfileDragAnchorOffset = offset
                        switch clickCount {
                        case 3...:
                            setDockerfileSelection(
                                fragmentID: block.fragmentID,
                                range: NSRange(
                                    location: 0,
                                    length: (block.text as NSString).length
                                )
                            )
                        case 2:
                            setDockerfileSelection(
                                fragmentID: block.fragmentID,
                                range: dockerfileWordRange(containing: offset, in: block)
                            )
                        default:
                            setDockerfileSelection(fragmentID: block.fragmentID, range: nil)
                        }
                    }
                }
            } else if isRenamingContainerConfiguration {
                if workspaceRenameFieldFrame.contains(point) {
                    let wasFocused = workspaceRenameInputController.isFocused
                    let index = characterIndexForWorkspaceRenameField(at: point)
                    focusWorkspaceRenameField(selectAll: clickCount >= 3)
                    switch clickCount {
                    case 3...:
                        workspaceRenameInputController.selectAll()
                    case 2:
                        workspaceRenameInputController.selectWord(at: index)
                    default:
                        workspaceRenameInputController.setCursorPosition(
                            index,
                            modifySelection: modifierFlags.contains(.shift) && wasFocused
                        )
                        pendingTextSelectionDrag = PendingTextSelectionDrag(
                            target: .workspaceName
                        )
                    }
                    updateLayout()
                } else if workspaceRenameConfirmFrame.contains(point) {
                    armButtonClick(frame: workspaceRenameConfirmFrame) { [weak self] in
                        self?.submitWorkspaceRename()
                    }
                } else if workspaceRenameCancelFrame.contains(point) {
                    armButtonClick(frame: workspaceRenameCancelFrame) { [weak self] in
                        self?.dismissWorkspaceRename()
                    }
                } else if !workspaceRenamePanelFrame.contains(point) {
                    dismissWorkspaceRename()
                }
            } else if isConfirmingContainerConfigurationDismissal {
                if containerConfigurationDismissSaveFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationDismissSaveFrame) { [weak self] in
                        self?.saveContainerConfigurationDockerfileAndDismiss()
                    }
                } else if containerConfigurationDismissWithoutSavingFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationDismissWithoutSavingFrame) { [weak self] in
                        self?.dismissContainerConfigurationWithoutSavingDockerfile()
                    }
                } else if containerConfigurationDismissCancelFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationDismissCancelFrame) { [weak self] in
                        self?.dismissContainerConfigurationDismissalPrompt()
                    }
                }
            } else if isConfirmingContainerConfigurationRebuild {
                if containerConfigurationRebuildSaveFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationRebuildSaveFrame) { [weak self] in
                        self?.saveContainerConfigurationDockerfileAndRebuild()
                    }
                } else if containerConfigurationRebuildWithoutSavingFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationRebuildWithoutSavingFrame) { [weak self] in
                        self?.rebuildContainerConfigurationWithoutSavingDockerfile()
                    }
                } else if containerConfigurationRebuildCancelFrame.contains(point) {
                    armButtonClick(frame: containerConfigurationRebuildCancelFrame) { [weak self] in
                        self?.dismissContainerConfigurationRebuildPrompt()
                    }
                }
            } else if workspaceCloseFrame.contains(point) {
                armButtonClick(frame: workspaceCloseFrame) { [weak self] in
                    self?.requestContainerConfigurationDismissal()
                }
            } else if containerConfigurationRenameFrame.contains(point),
                      let workspace = pendingDockerfileWorkspace {
                armButtonClick(frame: containerConfigurationRenameFrame) { [weak self] in
                    self?.showContainerConfigurationRename(workspace)
                }
            } else if containerConfigurationDockerfileTabFrame.contains(point) {
                armButtonClick(frame: containerConfigurationDockerfileTabFrame) { [weak self] in
                    self?.selectContainerConfigurationTab(.dockerfile)
                }
            } else if containerConfigurationMountsTabFrame.contains(point) {
                armButtonClick(frame: containerConfigurationMountsTabFrame) { [weak self] in
                    self?.selectContainerConfigurationTab(.mounts)
                }
            } else if containerConfigurationEnvironmentTabFrame.contains(point) {
                armButtonClick(frame: containerConfigurationEnvironmentTabFrame) { [weak self] in
                    self?.selectContainerConfigurationTab(.environment)
                }
            } else if containerConfigurationPortsTabFrame.contains(point) {
                armButtonClick(frame: containerConfigurationPortsTabFrame) { [weak self] in
                    self?.selectContainerConfigurationTab(.ports)
                }
            } else if containerConfigurationRuntimeTabFrame.contains(point) {
                armButtonClick(frame: containerConfigurationRuntimeTabFrame) { [weak self] in
                    self?.selectContainerConfigurationTab(.runtime)
                }
            } else if containerConfigurationChangeRuntimeFrame.contains(point),
                      containerConfigurationRebuildWorkspaceID == nil,
                      let workspace = pendingDockerfileWorkspace,
                      let destination = availableSafeSpaceProviders.first(where: {
                          $0.id != workspace.runtime?.providerID && $0.canCreate
                      }) {
                armButtonClick(frame: containerConfigurationChangeRuntimeFrame) { [weak self] in
                    guard let self else { return }
                    self.selectedSafeSpaceProviderID = destination.id
                    self.containerConfigurationBuildError = nil
                    self.containerConfigurationRebuildWorkspaceID = workspace.id
                    self.sendWorkspaceRequest(
                        operation: "changeRuntime",
                        workspaceID: workspace.id
                    )
                }
            } else if containerConfigurationCookbookFrame.contains(point) {
                armButtonClick(frame: containerConfigurationCookbookFrame) { [weak self] in
                    guard let self,
                          let url = URL(string: "https://outershell.org/cookbook") else {
                        return
                    }
                    self.outerframeHost.openURLExternally(url)
                }
            } else if containerConfigurationCopyPathFrame.contains(point) {
                armButtonClick(frame: containerConfigurationCopyPathFrame) { [weak self] in
                    self?.showContainerPathMenu(at: point)
                }
            } else if containerConfigurationSaveFrame.contains(point) {
                armButtonClick(frame: containerConfigurationSaveFrame) { [weak self] in
                    self?.saveContainerConfigurationDockerfile()
                }
            } else if containerConfigurationDiscardFrame.contains(point) {
                armButtonClick(frame: containerConfigurationDiscardFrame) { [weak self] in
                    self?.discardContainerConfigurationDockerfileChanges()
                }
            } else if containerConfigurationEditorToolbarFrame.contains(point) {
                return
            } else if workspaceRenameFieldFrame.contains(point) {
                let wasFocused = workspaceRenameInputController.isFocused
                let index = characterIndexForWorkspaceRenameField(at: point)
                focusWorkspaceRenameField(selectAll: clickCount >= 3)
                switch clickCount {
                case 3...:
                    workspaceRenameInputController.selectAll()
                case 2:
                    workspaceRenameInputController.selectWord(at: index)
                default:
                    workspaceRenameInputController.setCursorPosition(
                        index,
                        modifySelection: modifierFlags.contains(.shift) && wasFocused
                    )
                    pendingTextSelectionDrag = PendingTextSelectionDrag(
                        target: .workspaceName
                    )
                }
                updateLayout()
            } else if containerConfigurationAddMountFrame.contains(point),
                      let workspace = pendingDockerfileWorkspace {
                armButtonClick(frame: containerConfigurationAddMountFrame) { [weak self] in
                    self?.sendWorkspaceRequest(operation: "chooseFolder",
                                               workspaceID: workspace.id)
                }
            } else if let action = containerConfigurationMountActionFrames.first(where: {
                $0.frame.contains(point)
            }) {
                armButtonClick(frame: action.frame) { [weak self] in
                    guard let self,
                          let index = self.containerConfigurationMounts.firstIndex(where: {
                              $0.id == action.id
                          }) else {
                        return
                    }
                    if action.action == "remove" {
                        self.containerConfigurationMounts.remove(at: index)
                    } else if action.action == "toggleReadOnly" {
                        self.containerConfigurationMounts[index].isReadOnly.toggle()
                    }
                    self.persistContainerRuntimeConfiguration()
                }
            } else if containerConfigurationRebuildFrame.contains(point) {
                armButtonClick(frame: containerConfigurationRebuildFrame) { [weak self] in
                    self?.rebuildContainerConfiguration()
                }
            } else if !workspacePanelFrame.contains(point) {
                requestContainerConfigurationDismissal()
            }
            return
        }
        if pendingWorkspaceDeletion != nil {
            if workspaceDeleteConfirmFrame.contains(point) {
                armButtonClick(frame: workspaceDeleteConfirmFrame) { [weak self] in
                    self?.submitWorkspaceDeletion()
                }
            } else if workspaceDeleteCancelFrame.contains(point) {
                armButtonClick(frame: workspaceDeleteCancelFrame) { [weak self] in
                    self?.dismissWorkspaceDeletion()
                }
            } else if !workspaceDeletePanelFrame.contains(point) {
                dismissWorkspaceDeletion()
            }
            return
        }
        if isWorkspaceNamePromptVisible {
            if workspaceOuterShellBaseImageFrame.contains(point) {
                armButtonClick(frame: workspaceOuterShellBaseImageFrame) { [weak self] in
                    self?.baseImageTemplate = .outerShell
                    self?.blurWorkspaceRenameField()
                    self?.updateLayout()
                }
            } else if workspaceCustomBaseImageFrame.contains(point) {
                armButtonClick(frame: workspaceCustomBaseImageFrame) { [weak self] in
                    guard let self else { return }
                    self.baseImageTemplate = .customWithSupport
                    self.workspaceRenameName = self.creationBaseImage
                    self.focusWorkspaceRenameField(selectAll: true)
                    self.updateLayout()
                }
            } else if workspaceCustomAsIsFrame.contains(point) {
                armButtonClick(frame: workspaceCustomAsIsFrame) { [weak self] in
                    guard let self else { return }
                    self.baseImageTemplate = .customAsIs
                    self.workspaceRenameName = self.creationBaseImage
                    self.focusWorkspaceRenameField(selectAll: true)
                    self.updateLayout()
                }
            } else if workspaceCreationBaseImageFrame.contains(point) {
                armButtonClick(frame: workspaceCreationBaseImageFrame) { [weak self] in
                    guard let self else { return }
                    self.pendingCreationContainerName = self.workspaceRenameInputController.text
                    self.isEditingCreationBaseImage = true
                    self.workspaceRenameName = self.creationBaseImage
                    self.workspacePanelMessage = ""
                    if self.baseImageTemplate == .outerShell {
                        self.blurWorkspaceRenameField()
                    } else {
                        self.focusWorkspaceRenameField(selectAll: true)
                    }
                    self.updateLayout()
                }
            } else if workspaceRenameFieldFrame.contains(point),
                      !isBaseImageChoicePrompt || baseImageTemplate != .outerShell {
                let wasFocused = workspaceRenameInputController.isFocused
                let index = characterIndexForWorkspaceRenameField(at: point)
                focusWorkspaceRenameField(selectAll: clickCount >= 3)
                switch clickCount {
                case 3...:
                    workspaceRenameInputController.selectAll()
                case 2:
                    workspaceRenameInputController.selectWord(at: index)
                default:
                    workspaceRenameInputController.setCursorPosition(
                        index,
                        modifySelection: modifierFlags.contains(.shift) && wasFocused
                    )
                    pendingTextSelectionDrag = PendingTextSelectionDrag(target: .workspaceName)
                }
                updateLayout()
            } else if workspaceRenameConfirmFrame.contains(point) {
                armButtonClick(frame: workspaceRenameConfirmFrame) { [weak self] in
                    self?.submitWorkspaceRename()
                }
            } else if workspaceRenameCancelFrame.contains(point) {
                armButtonClick(frame: workspaceRenameCancelFrame) { [weak self] in
                    self?.dismissWorkspaceRename()
                }
            } else if !workspaceRenamePanelFrame.contains(point) {
                dismissWorkspaceRename()
            }
            return
        }
        if isShowingWorkspacePanel {
            if workspaceCloseFrame.contains(point) {
                armButtonClick(frame: workspaceCloseFrame) { [weak self] in
                    self?.dismissWorkspacePanel()
                }
                return
            }
            if !workspacePanelFrame.contains(point) {
                dismissWorkspacePanel()
                return
            }
            return
        }

        if pendingInstallBackend != nil {
            if installConfirmFrame.contains(point) {
                armButtonClick(frame: installConfirmFrame, action: .installConfirm(operation: "run"))
            } else if installRootConfirmFrame.contains(point) {
                armButtonClick(frame: installRootConfirmFrame, action: .installConfirm(operation: "runRoot"))
            } else if installCancelFrame.contains(point) || !installPanelFrame.contains(point) {
                if installCancelFrame.contains(point) {
                    armButtonClick(frame: installCancelFrame, action: .installCancel)
                } else {
                    dismissInstallPrompt()
                }
            }
            return
        }
        if pendingOuterShellUpdate != nil {
            if updateConfirmFrame.contains(point) {
                armButtonClick(frame: updateConfirmFrame, action: .updateConfirm)
            } else if updateCancelFrame.contains(point) || !updatePanelFrame.contains(point) {
                if updateCancelFrame.contains(point) {
                    armButtonClick(frame: updateCancelFrame, action: .updateCancel)
                } else {
                    pendingOuterShellUpdate = nil
                    updateLayout()
                }
            }
            return
        }
        if pendingAboutBackend != nil {
            if aboutDoneFrame.contains(point) {
                armButtonClick(frame: aboutDoneFrame, action: .aboutDismiss)
            } else if aboutTextFrame.contains(point) {
                let offset = aboutTextOffset(at: point)
                aboutDragAnchorOffset = offset
                switch clickCount {
                case 3...:
                    setAboutSelectionRange(NSRange(location: 0, length: (renderedAboutText as NSString).length))
                case 2:
                    setAboutSelectionRange(aboutWordRange(containing: offset))
                default:
                    setAboutSelectionRange(nil)
                }
            } else if !aboutPanelFrame.contains(point) {
                dismissAboutPrompt()
            }
            return
        }
        if pendingPasswordAction != nil {
            if passwordSubmitFrame.contains(point) {
                armButtonClick(frame: passwordSubmitFrame, action: .passwordSubmit)
            } else if passwordFieldFrame.contains(point) {
                let wasFocused = passwordInputController.isFocused
                let index = characterIndexForPasswordField(xPosition: point.x)
                focusPasswordField(selectAll: clickCount >= 2)
                switch clickCount {
                case 2...:
                    // Word selection would reveal word boundaries in the masked value.
                    passwordInputController.selectAll()
                default:
                    passwordInputController.setCursorPosition(index,
                                                              modifySelection: modifierFlags.contains(.shift) && wasFocused)
                    pendingTextSelectionDrag = PendingTextSelectionDrag(target: .password)
                }
                updateLayout()
            } else if passwordCancelFrame.contains(point) || !passwordPanelFrame.contains(point) {
                if passwordCancelFrame.contains(point) {
                    armButtonClick(frame: passwordCancelFrame, action: .passwordCancel)
                } else {
                    dismissPasswordPrompt()
                }
            }
            return
        }

        if pendingFilePicker != nil, mode == .create {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            handleFilePickerMouseDown(at: createPoint, modifierFlags: modifierFlags, clickCount: clickCount)
            return
        }

        if mode == .apps,
           selectedServiceID != nil,
           logScrollbarController?.handleMouseDown(at: rootLayer.convert(point, to: logRowsClipLayer)) == true {
            return
        }

        let toolbarPoint = toolbarLayer.convert(point, from: rootLayer)
        if mode == .apps,
           outerShellActionFrame.contains(toolbarPoint),
           let backend = outerShellActionsBackend() {
            armButtonClick(frame: rootFrame(outerShellActionFrame, from: toolbarLayer),
                           performAtPoint: { [weak self] releasePoint in
                self?.showBackendActionsMenu(for: backend, at: releasePoint)
            })
            return
        }
        let contentPoint = contentLayer.convert(point, from: rootLayer)
        if mode == .apps {
            if handleLogHeaderMouseDown(at: point, modifierFlags: modifierFlags, clickCount: clickCount) {
                return
            }
            if handleLogMouseDown(at: point, modifierFlags: modifierFlags, clickCount: clickCount) {
                return
            }
            let appsPoint = appsContentPoint(for: contentPoint)
            if let block = dockerfileTextBlock(
                at: appsPoint,
                contentSpace: appsTextContentSpace(for: contentPoint)
            ) {
                let offset = dockerfileTextOffset(at: appsPoint, in: block)
                dockerfileDragAnchorOffset = offset
                switch clickCount {
                case 3...:
                    setDockerfileSelection(
                        fragmentID: block.fragmentID,
                        range: NSRange(
                            location: 0,
                            length: (block.text as NSString).length
                        )
                    )
                case 2:
                    setDockerfileSelection(
                        fragmentID: block.fragmentID,
                        range: dockerfileWordRange(containing: offset, in: block)
                    )
                default:
                    setDockerfileSelection(fragmentID: block.fragmentID, range: nil)
                }
                return
            }
            if let selectedRecipeSafeSpaceID,
               let workspace = localWorkspaces.first(where: {
                   $0.id == selectedRecipeSafeSpaceID
                }) {
                if safeSpaceDetailBackFrame.contains(appsPoint) {
                    armButtonClick(frame: rootFrame(safeSpaceDetailBackFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.returnFromRecipeSafeSpace()
                    }
                } else if safeSpaceDetailEditDockerfileFrame.contains(appsPoint) {
                    armButtonClick(frame: rootFrame(safeSpaceDetailEditDockerfileFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.showDockerfileEditor(for: workspace)
                    }
                } else if safeSpaceDetailOpenDockerfileFrame.contains(appsPoint) {
                    armButtonClick(frame: rootFrame(safeSpaceDetailOpenDockerfileFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.openDockerfileInTextEditor(for: workspace)
                    }
                } else if safeSpaceDetailEditBaseImageFrame.contains(appsPoint) {
                    armButtonClick(frame: rootFrame(safeSpaceDetailEditBaseImageFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.showRecipeBaseImagePrompt(for: workspace)
                    }
                } else if let addAppFrame = safeSpaceDetailAddAppFrames.first(where: {
                    $0.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(addAppFrame,
                                                    from: appsScrollContentLayer),
                                   performAtPoint: { [weak self] releasePoint in
                        self?.showSafeSpaceAddAppMenu(for: workspace, at: releasePoint)
                    })
                } else if safeSpaceDetailAddUserFrame.contains(appsPoint) {
                    armButtonClick(frame: rootFrame(safeSpaceDetailAddUserFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.showRecipeUserPrompt(for: workspace)
                    }
                } else if let target = safeSpaceDetailAddScriptFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(target.frame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.showRecipeScriptCreationPrompt(for: workspace,
                                                             userID: target.userID)
                    }
                } else if let target = safeSpaceDetailEditScriptFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(target.frame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.editRecipeScript(target.script)
                    }
                } else if let target = safeSpaceDetailRenameScriptFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(target.frame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.showRecipeScriptRenamePrompt(for: workspace,
                                                           script: target.script)
                    }
                } else if let action = workspaceOverviewActionFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(action.frame,
                                                    from: appsScrollContentLayer),
                                   performAtPoint: { [weak self] releasePoint in
                        self?.performWorkspaceOverviewAction(operation: action.operation,
                                                             workspace: action.workspace,
                                                             at: releasePoint)
                    })
                } else if let catalogItem = safeSpaceDetailCatalogFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(catalogItem.frame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.sendWorkspaceRequest(operation: "installRecipeCatalogItem",
                                                   workspaceID: workspace.id,
                                                   catalogItemID: catalogItem.item.id)
                    }
                } else if safeSpaceDetailAddStepFrame.contains(appsPoint) {
                    armButtonClick(frame: rootFrame(safeSpaceDetailAddStepFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.showRecipeCommandPrompt(for: workspace)
                    }
                } else if safeSpaceDetailRebuildFrame.contains(appsPoint) {
                    armButtonClick(frame: rootFrame(safeSpaceDetailRebuildFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.sendWorkspaceRequest(operation: "rebuildRecipe",
                                                   workspaceID: workspace.id)
                    }
                } else if safeSpaceDetailCopyContainerfileFrame.contains(appsPoint),
                          let containerfile = workspace.recipe?.containerfile {
                    armButtonClick(frame: rootFrame(safeSpaceDetailCopyContainerfileFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.copyTextToPasteboard(containerfile)
                        self?.safeSpaceRecipeMessage = "Copied Dockerfile."
                        self?.updateLayout()
                    }
                } else if safeSpaceDetailCopySupportSnippetFrame.contains(appsPoint),
                          let snippet = workspace.recipe?.outerShellSupportSnippet {
                    armButtonClick(frame: rootFrame(safeSpaceDetailCopySupportSnippetFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.copyTextToPasteboard(snippet)
                        self?.safeSpaceRecipeMessage = "Copied the Outer Shell support snippet."
                        self?.updateLayout()
                    }
                } else if safeSpaceDetailCopyRecipeMessageFrame.contains(appsPoint),
                          !safeSpaceRecipeMessage.isEmpty {
                    let message = safeSpaceRecipeMessage
                    armButtonClick(frame: rootFrame(safeSpaceDetailCopyRecipeMessageFrame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.copyTextToPasteboard(message)
                    }
                } else if let target = safeSpaceDetailEditStepFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(target.frame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.showRecipeFragmentEditor(for: workspace, step: target.step)
                    }
                } else if let target = safeSpaceDetailStepFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(target.frame,
                                                    from: appsScrollContentLayer),
                                   performAtPoint: { [weak self] releasePoint in
                        self?.showRecipeStepRemovalMenu(workspaceID: workspace.id,
                                                        step: target.step,
                                                        at: releasePoint)
                    })
                } else if let overflow = appOverflowFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(frame: rootFrame(overflow.frame,
                                                    from: appsScrollContentLayer)) { [weak self] in
                        self?.toggleAppOverflow(overflow.scope)
                    }
                } else if let badge = appBadgeFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    armButtonClick(
                        frame: rootFrame(badge.frame, from: appsScrollContentLayer),
                        action: .appBadge(
                            badge.endpoint,
                            displayName: badge.displayName,
                            opensInNewTab: modifierFlags.contains(.command)
                        )
                    )
                } else if let card = appCardFrames.first(where: {
                    $0.frame.contains(appsPoint)
                }) {
                    if modifierFlags.contains(.control) {
                        showAppActionsMenu(for: card.item, at: point)
                    } else {
                        pendingAppDrag = PendingAppDrag(item: card.item,
                                                        startPoint: point,
                                                        currentPoint: point,
                                                        isDragging: false)
                    }
                }
                return
            }
            if let overflow = appOverflowFrames.first(where: {
                $0.frame.contains(appsPoint)
            }) {
                armButtonClick(frame: rootFrame(overflow.frame,
                                                from: appsContentLayer(for: overflow.scope))) { [weak self] in
                    self?.toggleAppOverflow(overflow.scope)
                }
                return
            }
            if let menu = overviewMenuFrames.first(where: { $0.frame.contains(appsPoint) }) {
                armButtonClick(frame: rootFrame(menu.frame, from: appsScrollContentLayer),
                               performAtPoint: { [weak self] point in
                    self?.showAppActionsMenu(for: menu.item, at: point)
                })
                return
            }
            if let frame = overviewAddFrames.first(where: { $0.contains(appsPoint) }) {
                armButtonClick(frame: rootFrame(frame, from: appsScrollContentLayer), action: .addApp)
                return
            }
            if workspaceOverviewCreateFrame.contains(appsPoint) {
                armButtonClick(frame: rootFrame(workspaceOverviewCreateFrame,
                                                from: workspaceOverviewContentLayer),
                               performAtPoint: { [weak self] releasePoint in
                    self?.showSafeSpaceProviderMenu(at: releasePoint)
                })
                return
            }
            if let action = workspaceOverviewActionFrames.first(where: {
                $0.frame.contains(appsPoint)
            }) {
                armButtonClick(frame: rootFrame(action.frame,
                                                from: workspaceOverviewContentLayer),
                               performAtPoint: { [weak self] releasePoint in
                    self?.performWorkspaceOverviewAction(operation: action.operation,
                                                         workspace: action.workspace,
                                                         at: releasePoint)
                })
                return
            }
            if let group = overviewGroupFrames.first(where: { $0.header.contains(appsPoint) }) {
                pendingOverviewGroupDrag = (group.id, point, point, false)
                return
            }
            if let target = workspaceOverviewAppFrames.first(where: {
                $0.frame.contains(appsPoint)
            }) {
                armButtonClick(frame: rootFrame(target.frame,
                                                from: workspaceOverviewContentLayer)) { [weak self] in
                    self?.openWorkspaceApp(target.app,
                                           in: target.workspace,
                                           opensInNewTab: modifierFlags.contains(.command))
                }
                return
            }
            if let badge = appBadgeFrames.first(where: { $0.frame.contains(appsPoint) }) {
                armButtonClick(frame: rootFrame(badge.frame, from: appsScrollContentLayer),
                               action: .appBadge(badge.endpoint,
                                                 displayName: badge.displayName,
                                                 opensInNewTab: modifierFlags.contains(.command)))
                return
            }
            if addAppFrame.contains(appsPoint) {
                armButtonClick(frame: rootFrame(addAppFrame, from: appsScrollContentLayer),
                               action: .addApp)
                return
            }
            if let card = appCardFrames.first(where: { $0.frame.contains(appsPoint) }) {
                if modifierFlags.contains(.control) {
                    showAppActionsMenu(for: card.item, at: point)
                    return
                }
                pendingAppDrag = PendingAppDrag(item: card.item,
                                                startPoint: point,
                                                currentPoint: point,
                                                isDragging: false)
                return
            }
        } else if mode == .create {
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            if createDismissFrame.contains(createPoint) {
                armButtonClick(frame: rootFrame(createDismissFrame, from: createLayer),
                               action: .createCancel)
                return
            }
            guard createContentClipFrame.contains(createPoint) else {
                if createInputController.isFocused {
                    blurCreateField()
                    updateLayout()
                }
                return
            }
            if let sectionFrame = createSectionFrames.first(where: { $0.frame.contains(createPoint) }) {
                armButtonClick(frame: rootFrame(sectionFrame.frame, from: createLayer),
                               action: .createSection(sectionFrame.section))
                return
            }
            if let install = bundledAppInstallFrames.first(where: { $0.frame.contains(createPoint) }) {
                armButtonClick(frame: rootFrame(install.frame, from: createLayer),
                               action: .bundledInstall(install.backend))
                return
            }
            if let select = createDirectorySelectFrames.first(where: { $0.frame.contains(createPoint) }) {
                armButtonClick(frame: rootFrame(select.frame, from: createLayer),
                               action: .directorySelect(select.key))
                return
            }
            if let recipeFrame = recipeFrames.first(where: { $0.frame.contains(createPoint) }) {
                armButtonClick(frame: rootFrame(recipeFrame.frame, from: createLayer),
                               action: .recipe(recipeFrame.recipeID))
                return
            }
            if createButtonFrame.contains(createPoint) {
                armButtonClick(frame: rootFrame(createButtonFrame, from: createLayer),
                               action: .createSubmit)
                return
            }
            if bashIconSelectFrame.contains(createPoint) {
                armButtonClick(frame: rootFrame(bashIconSelectFrame, from: createLayer),
                               action: .chooseBashIcon)
                return
            }
            if cancelCreateFrame.contains(createPoint) {
                armButtonClick(frame: rootFrame(cancelCreateFrame, from: createLayer),
                               action: .createCancel)
                return
            }
            if let choice = createChoiceFrames.first(where: { $0.frame.contains(createPoint) }) {
                armButtonClick(frame: rootFrame(choice.frame, from: createLayer),
                               action: .createChoice(key: choice.key, value: choice.value))
                return
            }
            if let suggestion = createSuggestionFrames.first(where: { $0.frame.contains(createPoint) }) {
                armButtonClick(frame: rootFrame(suggestion.frame, from: createLayer),
                               action: .createSuggestion(key: suggestion.key, value: suggestion.value))
                return
            }
            if !createMessage.isEmpty,
               createMessageFrame.insetBy(dx: 0, dy: -3).contains(createPoint) {
                blurCreateField()
                blurPasswordField()
                let offset = createMessageCharacterIndex(at: createPoint)
                createMessageDragAnchorOffset = offset
                switch clickCount {
                case 3...:
                    setCreateMessageSelectionRange(NSRange(location: 0, length: (createMessage as NSString).length))
                case 2:
                    setCreateMessageSelectionRange(createMessageWordRange(containing: offset))
                default:
                    setCreateMessageSelectionRange(nil)
                    pendingTextSelectionDrag = PendingTextSelectionDrag(target: .createMessage)
                }
                return
            }
            if selectedCreateSection == .nativeApp,
               nativeProjectDragFrame.contains(createPoint),
               let generatedNativeProject {
                nativeProjectSelectionState = .selected
                pendingNativeProjectDrag = PendingNativeProjectDrag(project: generatedNativeProject,
                                                                    startPoint: point)
                updateLayout()
                return
            }
            if generatedNativeProject != nil,
               nativeProjectSelectionState == .selected {
                nativeProjectSelectionState = .none
                updateLayout()
                return
            }
            if createMessageSelectionRange != nil {
                createMessageSelectionRange = nil
            }
            if let field = createFieldFrames.first(where: { $0.frame.contains(createPoint) })?.key {
                let wasFocused = createInputController.isFocused && activeCreateFieldKey == field
                let index = characterIndexForCreateField(key: field, at: createPoint)
                if wasFocused,
                   clickCount == 1,
                   !modifierFlags.contains(.shift),
                   let selection = createInputController.selectionRange,
                   selection.contains(index),
                   let selectedText = createInputController.selectedTextContent(),
                   !selectedText.isEmpty {
                    pendingCreateTextDrag = PendingCreateTextDrag(startPoint: point,
                                                                  cursorIndex: index,
                                                                  selectedText: selectedText)
                    return
                }
                focusCreateField(field, selectAll: clickCount >= 3)
                switch clickCount {
                case 3...:
                    createInputController.selectAll()
                case 2:
                    createInputController.selectWord(at: index)
                default:
                    createInputController.setCursorPosition(index,
                                                            modifySelection: modifierFlags.contains(.shift) && wasFocused)
                    pendingTextSelectionDrag = PendingTextSelectionDrag(target: .createField(field))
                }
                updateLayout()
                return
            } else if createInputController.isFocused {
                blurCreateField()
                updateLayout()
            }
        }
    }

    private func handleFilePickerMouseDown(at createPoint: CGPoint,
                                           modifierFlags: NSEvent.ModifierFlags,
                                           clickCount: Int) {
        if filePickerSaveFrame.contains(createPoint) {
            armButtonClick(frame: rootFrame(filePickerSaveFrame, from: createLayer),
                           action: .filePickerSave)
            return
        }
        if filePickerCancelFrame.contains(createPoint) {
            armButtonClick(frame: rootFrame(filePickerCancelFrame, from: createLayer),
                           action: .filePickerCancel)
            return
        }
        if let path = FilePickerBreadcrumbBar.hitPath(at: createPoint,
                                                      breadcrumbFrame: filePickerBreadcrumbFrame,
                                                      segmentFrames: filePickerBreadcrumbSegmentFrames) {
            blurCreateField()
            fetchFilePickerDirectory(path: path)
            return
        }
        if filePickerListFrame.contains(createPoint),
           filePickerScrollbarController?.handleMouseDown(at: createLayer.convert(createPoint, to: filePickerListLayer)) == true {
            return
        }
        if filePickerListFrame.contains(createPoint),
           let hit = filePickerEntryFrames.first(where: { $0.frame.contains(createPoint) }) {
            blurCreateField()
            filePickerSelectedIndex = hit.index
            resetFilePickerTypeahead()
            updateFilePickerVisibleRows(rebuild: true)
            if clickCount >= 2 {
                activateFilePickerEntry(hit.entry)
            }
            return
        }
        if !filePickerPanelFrame.contains(createPoint), createInputController.isFocused {
            blurCreateField()
            updateLayout()
        }
    }

    private func handleKeyDown(keyCode: UInt16,
                               characters: String?,
                               charactersIgnoringModifiers: String?,
                               modifierFlags: NSEvent.ModifierFlags,
                               isARepeat: Bool) {
        _ = isARepeat
        if isContainerConfigurationEditorVisible {
            if isRenamingContainerConfiguration {
                if keyCode == 53 {
                    dismissWorkspaceRename()
                }
                return
            }
            if modifierFlags.contains(.command),
               !isConfirmingContainerConfigurationDismissal,
               !isConfirmingContainerConfigurationRebuild,
               charactersIgnoringModifiers?.lowercased() == "s" {
                saveContainerConfigurationDockerfile()
                return
            }
            if keyCode == 53 {
                guard !workspaceRenameInputController.isFocused else { return }
                if isConfirmingContainerConfigurationDismissal {
                    dismissContainerConfigurationDismissalPrompt()
                } else if isConfirmingContainerConfigurationRebuild {
                    dismissContainerConfigurationRebuildPrompt()
                } else {
                    requestContainerConfigurationDismissal()
                }
                return
            }
        }
        if pendingWorkspaceDeletion != nil {
            switch keyCode {
            case 36, 76:
                submitWorkspaceDeletion()
            case 53:
                dismissWorkspaceDeletion()
            default:
                break
            }
            return
        }
        if isWorkspaceNamePromptVisible {
            if workspaceRenameInputController.isFocused {
                if keyCode == 53 {
                    dismissWorkspaceRename()
                }
                return
            }
            switch keyCode {
            case 36, 76:
                submitWorkspaceRename()
            case 53:
                dismissWorkspaceRename()
            default:
                if !workspaceRenameInputController.isFocused,
                   let characters,
                   !characters.isEmpty {
                    focusWorkspaceRenameField()
                    workspaceRenameInputController.insertText(cleanSingleLineText(characters))
                }
            }
            return
        }
        if pendingInstallBackend != nil {
            handleInstallPromptKeyDown(keyCode: keyCode)
            return
        }
        if pendingOuterShellUpdate != nil {
            handleUpdatePromptKeyDown(keyCode: keyCode)
            return
        }
        if pendingAboutBackend != nil {
            handleAboutPromptKeyDown(keyCode: keyCode)
            return
        }
        if pendingPasswordAction != nil {
            handlePasswordKeyDown(keyCode: keyCode, characters: characters)
            return
        }
        if mode == .create {
            handleCreateKeyDown(keyCode: keyCode, characters: characters, modifierFlags: modifierFlags)
            return
        }
        switch keyCode {
        case 53:
            if selectedServiceID != nil {
                clearLogSelection()
                restartEventWatch(resetVersions: true)
                updateLayout()
            }
        default:
            break
        }
    }

    private func showInstallPrompt(for backend: BackendRecord) {
        blurCreateField()
        pendingInstallBackend = backend
        pendingInstallOperation = ((backend.rootOnly ?? false) || installsBundledPlaceholderAsSystemOnly(backend)) ? "runRoot" : "run"
        updateLayout()
    }

    private func dismissInstallPrompt() {
        pendingInstallBackend = nil
        pendingInstallOperation = "run"
        updateLayout()
    }

    private func confirmPendingInstall() {
        guard let backend = pendingInstallBackend else { return }
        let operation = ((backend.rootOnly ?? false) || installsBundledPlaceholderAsSystemOnly(backend)) ? "runRoot" : pendingInstallOperation
        pendingInstallBackend = nil
        pendingInstallOperation = "run"
        performControlAction(for: backend, operation: operation)
    }

    private func handleInstallPromptKeyDown(keyCode: UInt16) {
        switch keyCode {
        case 36, 76:
            confirmPendingInstall()
        case 53:
            dismissInstallPrompt()
        default:
            break
        }
    }

    private func handleUpdatePromptKeyDown(keyCode: UInt16) {
        switch keyCode {
        case 36, 76:
            confirmOuterShellUpdate()
        case 53:
            pendingOuterShellUpdate = nil
            updateLayout()
        default:
            break
        }
    }

    private func handleAboutPromptKeyDown(keyCode: UInt16) {
        switch keyCode {
        case 36, 53, 76:
            dismissAboutPrompt()
        default:
            break
        }
    }

    private func showAboutPrompt(for backend: BackendRecord) {
        blurCreateField()
        blurPasswordField()
        pendingAboutBackend = backend
        aboutSelectionRange = nil
        aboutDragAnchorOffset = nil
        updateLayout()
    }

    private func dismissAboutPrompt() {
        pendingAboutBackend = nil
        aboutSelectionRange = nil
        aboutDragAnchorOffset = nil
        updateEditingAndPasteboardState()
        updateLayout()
    }

    private func showPasswordPrompt(for backend: BackendRecord, operation: String, message: String) {
        pendingPasswordAction = PendingPasswordAction(serviceID: backend.serviceID,
                                                      serviceScope: backend.serviceScope,
                                                      operation: operation,
                                                      displayName: backend.displayName)
        sudoPasswordInput = ""
        sudoPasswordMessage = message.isEmpty ? "Administrator password required." : message
        focusPasswordField()
        updateLayout()
    }

    private func dismissPasswordPrompt() {
        blurPasswordField()
        pendingPasswordAction = nil
        sudoPasswordInput = ""
        sudoPasswordMessage = ""
        updateLayout()
    }

    private func submitPasswordPrompt() {
        guard let pendingPasswordAction,
              let backend = backends.first(where: {
                  $0.serviceID == pendingPasswordAction.serviceID &&
                      $0.serviceScope == pendingPasswordAction.serviceScope
              }) else {
            dismissPasswordPrompt()
            return
        }
        let password = passwordInputController.isFocused ? passwordInputController.text : sudoPasswordInput
        performControlAction(for: backend, operation: pendingPasswordAction.operation, sudoPassword: password)
    }

    private func confirmOuterShellUpdate() {
        guard let update = pendingOuterShellUpdate else { return }
        pendingOuterShellUpdate = nil
        performControlAction(for: update.backend, operation: "update")
    }

    private func handlePasswordKeyDown(keyCode: UInt16, characters: String?) {
        if passwordInputController.isFocused {
            if keyCode == 53 {
                dismissPasswordPrompt()
            }
            return
        }
        switch keyCode {
        case 36, 76:
            submitPasswordPrompt()
        case 51, 117:
            focusPasswordField()
            passwordInputController.deleteBackward()
        case 53:
            dismissPasswordPrompt()
        default:
            if let characters, !characters.isEmpty {
                insertPasswordText(characters)
            }
        }
    }

    private func insertPasswordText(_ text: String,
                                    hasReplacementRange: Bool = false,
                                    replacementLocation: UInt64 = 0,
                                    replacementLength: UInt64 = 0) {
        guard pendingPasswordAction != nil, !text.isEmpty else { return }
        if !passwordInputController.isFocused {
            focusPasswordField()
        }
        if hasReplacementRange {
            passwordInputController.setCursorPosition(Int(replacementLocation), modifySelection: false)
            let end = Int(replacementLocation + replacementLength)
            passwordInputController.setCursorPosition(end, modifySelection: true)
        }
        let cleaned = cleanSingleLineText(text)
        guard !cleaned.isEmpty else { return }
        passwordInputController.insertText(cleaned)
    }

    private func moveSelection(delta: Int) {
        guard mode == .apps else { return }
        let selections = backends.compactMap { backend -> LogSelection? in
            guard !backend.logFiles.isEmpty else { return nil }
            return LogSelection(serviceID: backend.serviceID, serviceScope: backend.serviceScope, logIndex: 0)
        }
        guard !selections.isEmpty else { return }
        let currentIndex = selectedLog.flatMap { selections.firstIndex(of: $0) } ?? 0
        let nextIndex = min(max(currentIndex + delta, 0), selections.count - 1)
        selectedContainerLogContext = nil
        selectedLog = selections[nextIndex]
        selectedServiceID = selectedLog?.serviceID
        logSnapshot = nil
        logScroll = 0
        setLogTextSelection(nil)
        fetchSelectedLog(scrollToBottom: true)
        restartEventWatch(resetVersions: true)
        updateLayout()
    }

    private func ensureLogSelection() {
        guard let selectedServiceID else {
            clearLogSelection()
            return
        }
        if selectedContainerLogContext != nil {
            return
        }
        let preferred = selectedLog.flatMap { selection in
            backends.first { $0.serviceID == selection.serviceID && $0.serviceScope == selection.serviceScope }
        } ?? backends.first { $0.serviceID == selectedServiceID }
        guard let preferred else {
            clearLogSelection()
            return
        }
        if let selectedLog,
           selectedLog.serviceID == selectedServiceID,
           selectedLog.serviceScope == preferred.serviceScope,
           preferred.logFiles.indices.contains(selectedLog.logIndex) {
            return
        }
        if !preferred.logFiles.isEmpty {
            selectedLog = LogSelection(serviceID: preferred.serviceID, serviceScope: preferred.serviceScope, logIndex: 0)
        } else {
            selectedLog = nil
            logSnapshot = nil
            shouldScrollLogToBottomOnNextLayout = false
        }
    }

    private func clearLogSelection() {
        selectedServiceID = nil
        selectedLog = nil
        selectedContainerLogContext = nil
        logSnapshot = nil
        logError = ""
        logScroll = 0
        logDragAnchorOffset = nil
        logHeaderDetailDragAnchorOffset = nil
        logHeaderDetailSelectionRange = nil
        lastLogDragTextPoint = nil
        shouldScrollLogToBottomOnNextLayout = false
        setLogTextSelection(nil)
    }

    private func dismissLogViewer() {
        clearLogSelection()
        restartEventWatch(resetVersions: true)
        updateLayout()
    }

    private func otherRecipeRecords() -> [RecipeRecord] {
        recipes.filter { $0.identifier == "jupyter" || $0.identifier == "jupyter-uv" }
    }

    private func selectedRecipe() -> RecipeRecord? {
        let visibleRecipes = selectedCreateSection == .otherRecipes ? otherRecipeRecords() : recipes
        return visibleRecipes.first { $0.identifier == selectedRecipeID } ?? visibleRecipes.first
    }

    private func visibleCreateFields(for recipe: RecipeRecord) -> [RecipeFieldRecord] {
        recipe.fields.filter { !isCreateFieldHidden($0, in: recipe) }
    }

    private func visibleBashCreateFieldKeys() -> [String] {
        var keys = ["bashDisplayName", "bashCommands"]
        if createValues["bashFrontendTransport", default: "port"] == "unixSocket" {
            keys.append("bashSocketPath")
        } else {
            keys.append("bashPort")
        }
        keys.append("bashIconPath")
        keys.append("bashIdentifier")
        return keys
    }

    private func visibleNativeCreateFieldKeys() -> [String] {
        ["nativeAppName", "nativeProjectRoot", "nativeProjectFolder", "nativeAppID", "nativeSocketFilename"]
    }

    private func visibleLocalCreateFieldKeys() -> [String] {
        selectedCreateSection == .nativeApp ? visibleNativeCreateFieldKeys() : visibleBashCreateFieldKeys()
    }

    private func isCreateFieldHidden(_ field: RecipeFieldRecord, in recipe: RecipeRecord) -> Bool {
        if field.key == "port",
           recipe.fields.contains(where: { $0.key == "frontendTransport" }),
           createValues["frontendTransport", default: "port"] == "unixSocket" {
            return true
        }
        if field.key == "socketPath",
           recipe.fields.contains(where: { $0.key == "frontendTransport" }),
           createValues["frontendTransport", default: "port"] != "unixSocket" {
            return true
        }
        return false
    }

    private func ensureBashDefaults() {
        if createValues["bashFrontendTransport", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["bashFrontendTransport"] = "port"
        }
        let displayName = createValues["bashDisplayName", default: ""]
        if !displayName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
           createValues["bashIdentifier", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["bashIdentifier"] = uniqueBashIdentifier(from: displayName)
        }
    }

    private func ensureNativeAppDefaults() {
        let name = createValues["nativeAppName", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty {
            createValues["nativeAppName"] = "Hello World"
        }
        if createValues["nativeBackendLanguage", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["nativeBackendLanguage"] = "go"
        }
        if createValues["nativeIsolationMode", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["nativeIsolationMode"] = "container"
        }
        if createValues["nativeTargetHTML"] == nil {
            createValues["nativeTargetHTML"] = "true"
        }
        if createValues["nativeTargetMacOS"] == nil {
            createValues["nativeTargetMacOS"] = "true"
        }
        if createValues["nativeFrontendLanguage", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["nativeFrontendLanguage"] = "swift"
        }
        if createValues["nativeProjectRoot", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["nativeProjectRoot"] = "~/outerframe-apps"
        }
        let effectiveName = createValues["nativeAppName", default: "Hello World"].trimmingCharacters(in: .whitespacesAndNewlines)
        let folder = suggestedProjectFolderName(from: effectiveName)
        let scheme = suggestedSwiftIdentifier(from: effectiveName)
        if createValues["nativeProjectFolder", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["nativeProjectFolder"] = folder
        }
        if createValues["nativeAppID", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["nativeAppID"] = "org.example.\(scheme)"
        }
        if createValues["nativeSocketFilename", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            createValues["nativeSocketFilename"] = suggestedSocketFilename(fromAppID: createValues["nativeAppID", default: "org.example.\(scheme)"])
        }
    }

    private func suggestedSocketFilename(fromAppID appID: String) -> String {
        let component = NativeAppProjectGenerator.safePathComponent(appID.trimmingCharacters(in: .whitespacesAndNewlines))
        return component.isEmpty ? "org.example.OuterframeApp" : component
    }

    private func suggestedProjectFolderName(from name: String) -> String {
        let lower = name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        var result = ""
        for scalar in lower.unicodeScalars {
            if allowed.contains(scalar) {
                result.unicodeScalars.append(scalar)
            } else if scalar.properties.isWhitespace || scalar.value == 45 || scalar.value == 95 {
                result.append("-")
            }
        }
        while result.contains("--") {
            result = result.replacingOccurrences(of: "--", with: "-")
        }
        let trimmed = result.trimmingCharacters(in: CharacterSet(charactersIn: "-_"))
        return trimmed.isEmpty ? "outerframe-app" : trimmed
    }

    private func suggestedSwiftIdentifier(from name: String) -> String {
        var words: [String] = []
        var current = ""
        for scalar in name.unicodeScalars {
            if CharacterSet.alphanumerics.contains(scalar) {
                current.unicodeScalars.append(scalar)
            } else if !current.isEmpty {
                words.append(current)
                current = ""
            }
        }
        if !current.isEmpty {
            words.append(current)
        }
        let identifier = words.map { word -> String in
            guard let first = word.first else { return "" }
            return String(first).uppercased() + String(word.dropFirst())
        }.joined()
        if identifier.isEmpty {
            return "OuterframeApp"
        }
        if let first = identifier.unicodeScalars.first,
           CharacterSet.decimalDigits.contains(first) {
            return "App\(identifier)"
        }
        return identifier
    }

    private func normalizedBashIdentifier() -> String {
        let explicit = createValues["bashIdentifier", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        if !explicit.isEmpty {
            let sanitized = suggestedIdentifier(from: explicit)
            return uniqueBashIdentifier(base: sanitized.isEmpty ? "bash-app" : sanitized)
        }
        return uniqueBashIdentifier(from: createValues["bashDisplayName", default: ""])
    }

    private func uniqueBashIdentifier(from displayName: String) -> String {
        let base = suggestedIdentifier(from: displayName)
        return uniqueBashIdentifier(base: base.isEmpty ? "bash-app" : base)
    }

    private func uniqueBashIdentifier(base: String) -> String {
        let existing = Set(backends.map { suggestedIdentifier(from: $0.serviceID) })
        if !existing.contains(base) {
            return base
        }
        var index = 2
        while existing.contains("\(base)-\(index)") {
            index += 1
        }
        return "\(base)-\(index)"
    }

    private func parentDirectoryForPath(_ path: String) -> String {
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return "~" }
        let ns = trimmed as NSString
        let directory = ns.deletingLastPathComponent
        if directory.isEmpty || directory == "." {
            return "~"
        }
        return directory
    }

    private func joinPath(directory: String, filename: String) -> String {
        var trimmedDirectory = directory.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmedDirectory.isEmpty {
            return filename
        }
        if trimmedDirectory == "/" {
            return "/" + filename
        }
        while trimmedDirectory.count > 1 && trimmedDirectory.hasSuffix("/") {
            trimmedDirectory.removeLast()
        }
        return "\(trimmedDirectory)/\(filename)"
    }

    private func createValue(for field: RecipeFieldRecord) -> String {
        createValues[field.key] ?? field.defaultValue
    }

    private func applyRecipeDefaults(overwrite: Bool) {
        guard let recipe = selectedRecipe() else { return }
        for field in recipe.fields {
            if overwrite || createValues[field.key] == nil {
                createValues[field.key] = field.defaultValue
            }
        }
        let visibleFields = visibleCreateFields(for: recipe)
        if overwrite || activeCreateFieldKey == nil || !visibleFields.contains(where: { $0.key == activeCreateFieldKey }) {
            activeCreateFieldKey = visibleFields.first(where: { $0.fieldType != "choice" })?.key
        }
        if createInputController.isFocused, let activeCreateFieldKey {
            focusCreateField(activeCreateFieldKey)
        }
    }

    private func selectedBackend() -> BackendRecord? {
        guard let selectedServiceID else { return nil }
        if let context = selectedContainerLogContext,
           context.app.serviceID == selectedServiceID {
            return appLauncherItems(in: context.container).first(where: {
                $0.backend.serviceID == selectedServiceID
            })?.backend
        }
        if let selectedLog,
           let backend = backends.first(where: { $0.serviceID == selectedLog.serviceID && $0.serviceScope == selectedLog.serviceScope }) {
            return backend
        }
        return backends.first { $0.serviceID == selectedServiceID }
    }

    private func outerShellBackend() -> BackendRecord? {
        backends.first { $0.isBackendsSelf }
    }

    private func outerShellActionsBackend() -> BackendRecord? {
        if let backend = outerShellBackend() {
            return backend
        }
        guard controlEndpoint != nil else { return nil }
        return BackendRecord(serviceID: "org.outershell.OuterShell",
                             displayName: "Outer Shell",
                             serviceUnit: "",
                             serviceUnitPath: nil,
                             serviceScope: "user",
                             status: backendError.isEmpty ? "" : "error",
                             canControl: true,
                             canUninstall: true,
                             isBundled: false,
                             isInstalled: true,
                             isMigration: false,
                             supportsRoot: false,
                             rootOnly: false,
                             hasRootSupport: false,
                             installedVersion: nil,
                             availableVersion: nil,
                             scriptPath: nil,
                             publicBaseURL: nil,
                             iconSymbolName: nil,
                             launchdPlistPath: "",
                             ownsLaunchdPlist: true,
                             menuBarVisibilityEnabled: nil,
                             menuBarVisibilityAvailable: false,
                             frontends: [],
                             logFiles: [])
    }

    private func currentLogFile(for backend: BackendRecord) -> LogFileRecord? {
        guard let selectedLog,
              selectedLog.serviceID == backend.serviceID,
              selectedLog.serviceScope == backend.serviceScope,
              backend.logFiles.indices.contains(selectedLog.logIndex) else {
            return backend.logFiles.first
        }
        return backend.logFiles[selectedLog.logIndex]
    }

    private func logFile(for selection: LogSelection) -> LogFileRecord? {
        guard let backend = backends.first(where: { $0.serviceID == selection.serviceID && $0.serviceScope == selection.serviceScope }),
              backend.logFiles.indices.contains(selection.logIndex) else {
            return nil
        }
        return backend.logFiles[selection.logIndex]
    }

    private func logSelectorTitle(for logFile: LogFileRecord?, index: Int) -> String {
        guard let logFile else { return "Log" }
        let name = logFile.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        return name.isEmpty ? "Log \(index + 1)" : name
    }

    private func logMenuTitle(for logFile: LogFileRecord, index: Int, in logFiles: [LogFileRecord]) -> String {
        let baseTitle = logSelectorTitle(for: logFile, index: index)
        let duplicateCount = logFiles.filter { $0.displayName == logFile.displayName }.count
        guard duplicateCount > 1 else { return baseTitle }
        return "\(baseTitle) - \(logFile.path)"
    }

    private func selectLog(serviceID: String, serviceScope: String, logIndex: Int) {
        guard let backend = backends.first(where: { $0.serviceID == serviceID && $0.serviceScope == serviceScope }),
              backend.logFiles.indices.contains(logIndex) else { return }
        let nextSelection = LogSelection(serviceID: serviceID, serviceScope: serviceScope, logIndex: logIndex)
        guard selectedLog != nextSelection else { return }
        selectedContainerLogContext = nil
        selectedServiceID = serviceID
        selectedLog = nextSelection
        logSnapshot = nil
        logError = ""
        logScroll = 0
        logHeaderDetailSelectionRange = nil
        logHeaderDetailDragAnchorOffset = nil
        isLoadingLog = false
        setLogTextSelection(nil)
        fetchSelectedLog(scrollToBottom: true)
        restartEventWatch(resetVersions: true)
        updateLayout()
    }

    private func showLogs(for backend: BackendRecord) {
        selectedContainerLogContext = nil
        selectedServiceID = backend.serviceID
        selectedLog = backend.logFiles.isEmpty
            ? nil
            : LogSelection(serviceID: backend.serviceID, serviceScope: backend.serviceScope, logIndex: 0)
        logSnapshot = nil
        logError = ""
        logScroll = 0
        logHeaderDetailSelectionRange = nil
        logHeaderDetailDragAnchorOffset = nil
        isLoadingLog = false
        setLogTextSelection(nil)
        if selectedLog != nil {
            fetchSelectedLog(scrollToBottom: true)
        }
        restartEventWatch(resetVersions: true)
        updateLayout()
    }

    private func showContainerLogs(_ context: ContainerAppLauncherContext) {
        selectedContainerLogContext = context
        selectedServiceID = context.app.serviceID
        selectedLog = LogSelection(serviceID: context.app.serviceID,
                                   serviceScope: "container:\(context.container.id.uuidString)",
                                   logIndex: 0)
        logSnapshot = nil
        logError = ""
        logScroll = 0
        logHeaderDetailSelectionRange = nil
        logHeaderDetailDragAnchorOffset = nil
        isLoadingLog = false
        setLogTextSelection(nil)
        fetchSelectedLog(scrollToBottom: true)
        restartEventWatch(resetVersions: true)
        updateLayout()
    }

    private func bundledCatalogBackends() -> [BundledCatalogEntry] {
        let grouped = Dictionary(grouping: backends.filter { $0.isBundledCatalogEntry }, by: \.serviceID)
        return grouped.values.compactMap { records in
            guard let displayRecord = records.first(where: { $0.isBundledPlaceholder }) ??
                    records.first(where: { ($0.iconSymbolName ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false }) ??
                    records.first else {
                return nil
            }
            return BundledCatalogEntry(backend: displayRecord,
                                       isInstalled: records.contains { $0.isInstalled ?? false })
        }
        .sorted { $0.backend.displayName.localizedCaseInsensitiveCompare($1.backend.displayName) == .orderedAscending }
    }

    private func appIconSymbolName(for backend: BackendRecord) -> String? {
        if let symbolName = backend.iconSymbolName,
           !symbolName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return symbolName
        }
        switch backend.serviceID {
        case "org.outershell.Files":
            return "folder"
        case "org.outershell.Firehose":
            return "text.line.last.and.arrowtriangle.forward"
        case "org.outershell.Plaintext":
            return "doc.plaintext"
        case "org.outershell.Top":
            return "chart.bar.xaxis"
        default:
            return nil
        }
    }

    private func appIconTintColor(for backend: BackendRecord) -> NSColor {
        switch backend.serviceID {
        case "org.outershell.Firehose":
            return NSColor(calibratedRed: 1.0, green: 0.53, blue: 0.13, alpha: 1.0)
        default:
            return .controlAccentColor
        }
    }

    private func launcherIconSymbolName(for item: AppLauncherItem) -> String? {
        appIconSymbolName(for: item.backend)
    }

    private func fetchEndpointIcons() {
        guard let iconSession, let base = outerframeHost.pluginBaseURL() else { return }
        let paths = backends.flatMap { $0.frontends.compactMap(\.iconURL) } +
            localWorkspaces.flatMap { $0.apps.compactMap(\.iconURL) + $0.commandLaunchers.compactMap(\.iconURL) }
        for path in Set(paths) where endpointIcons[path] == nil && !pendingIconURLs.contains(path) {
            guard let url = URL(string: path, relativeTo: base)?.absoluteURL else { continue }
            let diskCache = endpointIconDiskCache
            if let image = diskCache?.image(for: url) {
                endpointIcons[path] = image
                continue
            }
            pendingIconURLs.insert(path)
            iconSession.dataTask(with: url) { [weak self] data, response, error in
                let image: CGImage?
                if error == nil, (response as? HTTPURLResponse)?.statusCode == 200, let data {
                    image = diskCache.map { $0.storeImage(data, for: url) } ?? decodedIconCGImage(data)
                } else {
                    image = nil
                }
                Task { @MainActor in
                    guard let self else { return }
                    self.pendingIconURLs.remove(path)
                    if let image {
                        self.endpointIcons[path] = image
                        self.scheduleLayoutUpdate()
                    }
                }
            }.resume()
        }
    }

    private func launcherIconImage(for item: AppLauncherItem) -> CGImage? {
        if let path = item.containerContext?.app.iconURL ?? item.frontend.iconURL,
           let image = endpointIcons[path] { return image }
        if let image = item.iconCGImage {
            return image
        }
        if item.containerContext != nil,
           let matchingServerItem = appLauncherItems(from: backends).first(where: {
               $0.backend.serviceID == item.backend.serviceID
           }) {
            return launcherIconImage(for: matchingServerItem)
        }
        switch item.backend.serviceID {
        case "org.outershell.Top", "org.outershell.Plaintext":
            return nil
        default:
            return item.iconCGImage
        }
    }

    private func appLauncherItems() -> [AppLauncherItem] {
        appLauncherItems(from: backends)
    }

    private func appLauncherItems(in container: LocalWorkspaceRecord) -> [AppLauncherItem] {
        container.apps.map { app in
            let frontend = FrontendRecord(
                id: app.frontendID,
                name: app.displayName,
                url: app.url,
                port: 0,
                socketPath: app.socketPath,
                iconPath: app.iconPath,
                iconByteCount: app.iconData?.count ?? 0,
                iconCGImage: app.iconData.flatMap(decodedIconCGImage),
                iconObservationToken: app.iconObservationToken,
                list: app.listName,
                isRunning: app.isRunning
            )
            let backend = BackendRecord(
                serviceID: app.serviceID,
                displayName: app.displayName,
                serviceUnit: app.serviceID,
                serviceUnitPath: nil,
                serviceScope: "container",
                status: app.isRunning ? "running" : "available",
                canControl: true,
                canUninstall: false,
                isBundled: false,
                isInstalled: true,
                isMigration: false,
                supportsRoot: false,
                rootOnly: false,
                hasRootSupport: false,
                installedVersion: nil,
                availableVersion: nil,
                scriptPath: nil,
                publicBaseURL: nil,
                iconSymbolName: nil,
                launchdPlistPath: "",
                ownsLaunchdPlist: false,
                menuBarVisibilityEnabled: nil,
                menuBarVisibilityAvailable: false,
                frontends: [frontend],
                logFiles: [
                    LogFileRecord(
                        identifier: "container:\(container.id.uuidString):\(app.serviceID):0",
                        displayName: "Backend",
                        path: "",
                        size: 0,
                        modified: 0,
                        readable: true
                    )
                ]
            )
            let endpoint = AppLauncherEndpoint(backend: backend,
                                               frontend: frontend,
                                               frontendIndex: 0)
            return AppLauncherItem(
                identityKey: "container:\(container.id.uuidString):\(app.frontendID)",
                primaryEndpoint: endpoint,
                userEndpoint: endpoint,
                rootEndpoint: nil,
                scope: .container(container.id),
                containerContext: ContainerAppLauncherContext(container: container, app: app)
            )
        }
        .sorted {
            $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
        }
    }

    private func runningEndpoints(for item: AppLauncherItem) -> [(endpoint: AppLauncherEndpoint, symbolName: String, isRoot: Bool)] {
        var endpoints: [(endpoint: AppLauncherEndpoint, symbolName: String, isRoot: Bool)] = []
        if let userEndpoint = item.userEndpoint,
           endpointIsRunning(userEndpoint) {
            endpoints.append((userEndpoint, "person.fill", false))
        }
        if let rootEndpoint = item.rootEndpoint,
           endpointIsRunning(rootEndpoint) {
            endpoints.append((rootEndpoint, "checkmark.shield.fill", true))
        }
        return endpoints
    }

    private func endpointIsRunning(_ endpoint: AppLauncherEndpoint) -> Bool {
        if endpoint.frontend.hasEndpoint {
            return endpoint.frontend.isRunning
        }
        return endpoint.frontend.isRunning || endpoint.backend.status == "running"
    }

    private func endpointIsReadyToOpen(_ endpoint: AppLauncherEndpoint) -> Bool {
        endpointIsRunning(endpoint) ||
            ((endpoint.backend.status == "available" || endpoint.backend.status == "awaiting") && endpoint.frontend.hasEndpoint)
    }

    private func renderRunningBadges(for item: AppLauncherItem,
                                     leftX: CGFloat,
                                     centerY: CGFloat,
                                     pointSize: CGFloat,
                                     circleDiameter: CGFloat,
                                     gap: CGFloat) {
        let rootBadgeColor = NSColor.systemGreen
        let badges = runningEndpoints(for: item).compactMap { badge -> (endpoint: AppLauncherEndpoint, image: CGImage, size: CGSize, backgroundColor: NSColor?, shadowColor: NSColor)? in
            if badge.isRoot {
                guard let symbol = runningBadgeSymbol(named: badge.symbolName,
                                                      pointSize: circleDiameter,
                                                      isRoot: true) else { return nil }
                return (badge.endpoint, symbol.image, symbol.size, nil, rootBadgeColor)
            }
            guard let symbol = runningBadgeSymbol(named: badge.symbolName,
                                                  pointSize: pointSize,
                                                  isRoot: false) else { return nil }
            return (badge.endpoint, symbol.image, symbol.size, .systemGreen, .systemGreen)
        }
        guard !badges.isEmpty else { return }

        let circleSize = CGSize(width: circleDiameter, height: circleDiameter)
        let chips = badges.map { (badge: $0, size: circleSize) }
        let totalHeight = chips.reduce(CGFloat(0)) { $0 + $1.size.height } + CGFloat(max(chips.count - 1, 0)) * gap
        let x = floor(leftX)
        var y = floor(centerY + totalHeight / 2)
        for chip in chips {
            y -= chip.size.height
            let chipFrame = CGRect(x: x,
                                   y: floor(y),
                                   width: chip.size.width,
                                   height: chip.size.height)
            if item.containerContext == nil {
                appBadgeFrames.append(AppLauncherBadgeTarget(
                    frame: chipFrame.insetBy(dx: -3, dy: -3),
                    endpoint: chip.badge.endpoint,
                    displayName: item.displayName
                ))
            }

            let chipLayer = CALayer()
            chipLayer.frame = chipFrame
            chipLayer.cornerRadius = floor(chipFrame.height / 2)
            if let backgroundColor = chip.badge.backgroundColor {
                chipLayer.backgroundColor = resolvedCGColor(backgroundColor.withAlphaComponent(0.95))
                chipLayer.borderWidth = 0.5
                chipLayer.borderColor = resolvedCGColor(NSColor.white.withAlphaComponent(0.8))
            } else {
                chipLayer.backgroundColor = resolvedCGColor(.clear)
                chipLayer.borderWidth = 0
            }
            chipLayer.shadowColor = resolvedCGColor(chip.badge.shadowColor.withAlphaComponent(0.4))
            chipLayer.shadowOpacity = 0.22
            chipLayer.shadowRadius = 3
            chipLayer.shadowOffset = CGSize(width: 0, height: 1)

            let symbolLayer = CALayer()
            symbolLayer.frame = CGRect(x: (chipFrame.width - chip.badge.size.width) / 2,
                                       y: (chipFrame.height - chip.badge.size.height) / 2,
                                       width: chip.badge.size.width,
                                       height: chip.badge.size.height)
            symbolLayer.contentsGravity = .resizeAspect
            symbolLayer.contentsScale = 2
            symbolLayer.contents = chip.badge.image
            chipLayer.addSublayer(symbolLayer)

            addAppsSublayer(chipLayer)
            y -= gap
        }
    }

    private func runningBadgeSymbol(named symbolName: String,
                                    pointSize: CGFloat,
                                    isRoot: Bool) -> (image: CGImage, size: CGSize)? {
        let key = RunningBadgeSymbolKey(symbolName: symbolName,
                                        pointSize: pointSize,
                                        isRoot: isRoot)
        if let image = runningBadgeSymbolImages[key] {
            return image
        }
        guard let image = naturalSymbolCGImage(named: symbolName,
                                               pointSize: pointSize,
                                               color: isRoot ? .systemGreen : .white) else {
            return nil
        }
        runningBadgeSymbolImages[key] = image
        return image
    }

    private func appLauncherItems(from records: [BackendRecord]) -> [AppLauncherItem] {
        records.flatMap { backend in
            backend.frontends.enumerated().map { index, frontend in
                let endpoint = AppLauncherEndpoint(backend: backend, frontend: frontend, frontendIndex: index)
                return AppLauncherItem(
                    identityKey: frontendIdentityKey(backend: backend, frontend: frontend, frontendIndex: index),
                    primaryEndpoint: endpoint,
                    userEndpoint: backend.serviceScope == "system" ? nil : endpoint,
                    rootEndpoint: backend.serviceScope == "system" ? endpoint : nil,
                    scope: .server,
                    containerContext: nil
                )
            }
        }.sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    private func appLauncherSignature(for backends: [BackendRecord]) -> String {
        appLauncherItems(from: backends).map { item in
            let iconBytes = item.frontend.iconByteCount
            return [
                item.identityKey,
                item.backend.serviceID,
                item.backend.serviceScope,
                item.backend.displayName,
                item.backend.scriptPath ?? "",
                item.backend.iconSymbolName ?? "",
                item.frontend.url,
                item.frontend.id,
                item.frontend.name,
                item.frontend.socketPath,
                String(item.frontend.port),
                item.frontend.isRunning ? "running" : "stopped",
                item.frontend.iconPath ?? "",
                String(iconBytes),
                item.frontend.iconObservationToken,
                item.frontend.iconURL ?? "",
                item.frontend.listName,
                item.userEndpoint.map { frontendIdentityKey(backend: $0.backend, frontend: $0.frontend, frontendIndex: $0.frontendIndex) } ?? "",
                item.userEndpoint?.backend.status ?? "",
                item.userEndpoint?.backend.scriptPath ?? "",
                item.rootEndpoint.map { frontendIdentityKey(backend: $0.backend, frontend: $0.frontend, frontendIndex: $0.frontendIndex) } ?? "",
                item.rootEndpoint?.backend.status ?? "",
                item.rootEndpoint?.backend.scriptPath ?? ""
            ].joined(separator: "\u{1f}")
        }
        .sorted()
        .joined(separator: "\u{1e}")
    }

    private func appInitial(for name: String) -> String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let first = trimmed.first else { return "A" }
        return String(first).uppercased()
    }

    private func clampScrollOffsets() {
        _ = clampAppsScrollUsingRenderedContent()
        _ = clampWorkspaceScrollUsingRenderedContent()
        logScroll = clampedLogScroll(logScroll)
        _ = clampCreateScrollUsingRenderedContent()
    }

    private func clampedLogScroll(_ value: CGFloat) -> CGFloat {
        let maxScroll = max(logContentHeight() - logRowsClipLayer.bounds.height, 0)
        return min(max(value, 0), maxScroll)
    }

    private func isLogScrolledNearBottom(tolerance: CGFloat = 2) -> Bool {
        let maxScroll = max(logContentHeight() - logRowsClipLayer.bounds.height, 0)
        return maxScroll - logScroll <= tolerance
    }

    func scrollbarDidChangeScrollOffset(_ offset: CGFloat) {
        shouldScrollLogToBottomOnNextLayout = false
        logScroll = clampedLogScroll(offset)
        updateLogTextViewport()
        updateLogTextSelectionLayers()
    }

    private func logContentHeight() -> CGFloat {
        guard logRowsClipLayer.bounds.width > 0, logRowsClipLayer.bounds.height > 0 else { return 0 }
        let textWidth = max(logRowsClipLayer.bounds.width - logTextInsetX * 2, 1)
        return logContentHeight(textWidth: textWidth)
    }

    private func logContentHeight(textWidth: CGFloat) -> CGFloat {
        if let cache = logContentHeightCache,
           cache.generation == logTextContentGeneration,
           abs(cache.textWidth - textWidth) <= 0.5 {
            return cache.height
        }

        logTextContainer.size = CGSize(width: textWidth,
                                       height: max(logTextMeasurementHeight, logRowsClipLayer.bounds.height))
        logTextLayoutManager.ensureLayout(for: logTextLayoutManager.documentRange)
        let height = max(logTextLayoutManager.usageBoundsForTextContainer.maxY + logTextInsetY * 2,
                         logRowsClipLayer.bounds.height)
        logContentHeightCache = (generation: logTextContentGeneration, textWidth: textWidth, height: height)
        return height
    }

    private func logTextFont() -> NSFont {
        NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    }

    private func logVisualLineMetrics(textWidth: CGFloat? = nil) -> LogVisualLineMetrics {
        let resolvedTextWidth = max(textWidth ?? (logRowsClipLayer.bounds.width - logTextInsetX * 2), 1)
        if let cache = logVisualLineCache,
           cache.generation == logTextContentGeneration,
           abs(cache.textWidth - resolvedTextWidth) <= 0.5 {
            return cache.metrics
        }

        let font = logTextFont()
        let charWidth = max(("0" as NSString).size(withAttributes: [.font: font]).width, 1)
        let lineHeight = ceil(font.ascender - font.descender + font.leading + 2)
        let charactersPerLine = max(Int(floor(resolvedTextWidth / charWidth)), 1)
        let nsString = logRenderedText as NSString
        let length = nsString.length
        var lines: [LogVisualLine] = []
        var lineStart = 0

        while lineStart < length {
            let lineRange = nsString.lineRange(for: NSRange(location: lineStart, length: 0))
            var contentLength = lineRange.length
            while contentLength > 0 {
                let character = nsString.character(at: lineRange.location + contentLength - 1)
                if character == 10 || character == 13 {
                    contentLength -= 1
                } else {
                    break
                }
            }

            let visualLineCount = max(Int(ceil(Double(contentLength) / Double(charactersPerLine))), 1)
            for visualLineIndex in 0..<visualLineCount {
                let offset = visualLineIndex * charactersPerLine
                let location = min(lineRange.location + offset, length)
                let remaining = max(contentLength - offset, 0)
                let visualLength = min(remaining, charactersPerLine)
                lines.append(LogVisualLine(range: NSRange(location: location, length: visualLength)))
            }

            let nextLineStart = NSMaxRange(lineRange)
            if nextLineStart <= lineStart {
                break
            }
            lineStart = nextLineStart
        }

        if length == 0 {
            lines.append(LogVisualLine(range: NSRange(location: 0, length: 0)))
        } else {
            let lastCharacter = nsString.character(at: length - 1)
            if lastCharacter == 10 || lastCharacter == 13 {
                lines.append(LogVisualLine(range: NSRange(location: length, length: 0)))
            }
        }

        let metrics = LogVisualLineMetrics(textWidth: resolvedTextWidth,
                                           charWidth: charWidth,
                                           lineHeight: lineHeight,
                                           charactersPerLine: charactersPerLine,
                                           lines: lines)
        logVisualLineCache = (generation: logTextContentGeneration,
                              textWidth: resolvedTextWidth,
                              metrics: metrics)
        return metrics
    }

    private func logTextOffset(atTextPoint textPoint: CGPoint) -> Int {
        let point = CGPoint(x: max(textPoint.x, 0), y: max(textPoint.y, 0))
        logTextLayoutManager.ensureLayout(for: logTextLayoutManager.documentRange)
        guard let fragment = logTextLayoutManager.textLayoutFragment(for: point) else {
            return point.y <= 0 ? 0 : logAttributedText.length
        }
        let fragmentFrame = fragment.layoutFragmentFrame
        let localY = point.y - fragmentFrame.minY
        guard let line = fragment.textLineFragment(forVerticalOffset: localY, requiresExactMatch: false) else {
            return logTextOffset(for: fragment.rangeInElement.location)
        }

        let fragmentStart = logTextOffset(for: fragment.rangeInElement.location)
        let linePoint = CGPoint(x: point.x - fragmentFrame.minX - line.typographicBounds.minX,
                                y: localY - line.typographicBounds.minY)
        let localIndex = line.characterIndex(for: linePoint)
        let lineLower = line.characterRange.location
        let lineUpper = line.characterRange.location + line.characterRange.length
        let clampedLocalIndex = min(max(localIndex, lineLower), lineUpper)
        return min(max(fragmentStart + clampedLocalIndex, 0), logAttributedText.length)
    }

    private func clampAppsScrollUsingRenderedContent() -> Bool {
        let maxScroll = max(createBottomInset - appsContentBottom, 0)
        let clamped = min(max(appsScroll, 0), maxScroll)
        if abs(clamped - appsScroll) > 0.5 {
            appsScroll = clamped
            return true
        }
        return false
    }

    private func clampWorkspaceScrollUsingRenderedContent() -> Bool {
        guard usesWorkspaceSplitLayout else {
            if workspaceScroll != 0 {
                workspaceScroll = 0
                return true
            }
            return false
        }
        let maxScroll = max(createBottomInset - workspaceContentBottom, 0)
        let clamped = min(max(workspaceScroll, 0), maxScroll)
        if abs(clamped - workspaceScroll) > 0.5 {
            workspaceScroll = clamped
            return true
        }
        return false
    }

    private func clampCreateScrollUsingRenderedContent() -> Bool {
        let visibleBottom = createContentClipFrame.isEmpty ? createBottomInset : createContentClipFrame.minY + createBottomInset
        let maxScroll = max(createScroll + visibleBottom - createContentBottom, 0)
        let clamped = min(max(createScroll, 0), maxScroll)
        if abs(clamped - createScroll) > 0.5 {
            createScroll = clamped
            return true
        }
        return false
    }

    private func dismissWorkspacePanel() {
        pendingWorkspaceDeletion = nil
        if isWorkspaceNamePromptVisible {
            blurWorkspaceRenameField()
            pendingWorkspaceRename = nil
            pendingRecipeCommandWorkspace = nil
            pendingDockerfileWorkspace = nil
            pendingRecipeBaseImageWorkspace = nil
            pendingRecipeUserWorkspace = nil
            pendingRecipeEditWorkspace = nil
            pendingRecipeEditStep = nil
            pendingRecipeScriptWorkspace = nil
            pendingRecipeScriptUserID = nil
            pendingRecipeScriptRename = nil
            pendingSafeSpaceAppWorkspace = nil
            pendingSafeSpaceAppKind = nil
            isCreatingWorkspace = false
            isEditingCreationBaseImage = false
            pendingCreationContainerName = ""
            workspaceNamePromptDismissesPanel = false
            workspaceRenameName = ""
        }
        isShowingWorkspacePanel = false
        workspacePanelMessage = ""
        updateLayout()
    }

    private func sendWorkspaceRequest(operation: String,
                                      workspaceID: UUID? = nil,
                                      name: String? = nil,
                                      frontendID: String? = nil,
                                      serviceID: String? = nil,
                                      listName: String? = nil,
                                      mountID: UUID? = nil,
                                      readOnly: Bool? = nil,
                                      recipeStepID: UUID? = nil,
                                      catalogItemID: String? = nil,
                                      command: String? = nil,
                                      baseImage: String? = nil,
                                      installsOuterShellSupport: Bool? = nil,
                                      rebuild: Bool? = nil,
                                      launcherKind: String? = nil,
                                      workingDirectory: String? = nil,
                                      recipeUserID: UUID? = nil,
                                      recipeScriptPath: String? = nil,
                                      dockerfile: String? = nil,
                                      mounts: [ContainerConfigurationMountRequest]? = nil,
                                      environment: [ContainerConfigurationEnvironmentRequest]? = nil,
                                      publishedPorts: [ContainerConfigurationPublishedPortRequest]? = nil,
                                      includeDetails: Bool = false) {
        if operation == "list" {
            guard !isRefreshingWorkspaces else { return }
        } else if operation != "create" {
            guard !isPerformingWorkspaceOperation else { return }
        }
        let requestID = UUID()
        let request = LocalWorkspaceHostRequest(requestID: requestID,
                                                operation: operation,
                                                workspaceID: workspaceID,
                                                name: name,
                                                frontendID: frontendID,
                                                serviceID: serviceID,
                                                listName: listName,
                                                cpus: operation == "create" ? 4 : nil,
                                                memoryInGB: operation == "create" ? 8 : nil,
                                                runtimeKind: operation == "create"
                                                    ? "appleContainer"
                                                    : nil,
                                                runtimeProviderID: operation == "create" ||
                                                    operation == "duplicate" ||
                                                    operation == "changeRuntime"
                                                    ? selectedSafeSpaceProviderID
                                                    : nil,
                                                mountID: mountID,
                                                readOnly: readOnly,
                                                recipeStepID: recipeStepID,
                                                catalogItemID: catalogItemID,
                                                command: command,
                                                baseImage: baseImage,
                                                installsOuterShellSupport: installsOuterShellSupport,
                                                rebuild: rebuild,
                                                launcherKind: launcherKind,
                                                workingDirectory: workingDirectory,
                                                recipeUserID: recipeUserID,
                                                recipeScriptPath: recipeScriptPath,
                                                dockerfile: dockerfile,
                                                mounts: mounts,
                                                environment: environment,
                                                publishedPorts: publishedPorts)
        guard let payload = try? JSONEncoder().encode(request) else {
            if operation == "rebuildRecipe" || operation == "changeRuntime" {
                containerConfigurationRebuildWorkspaceID = nil
            }
            workspacePanelMessage = "Could not prepare the container request."
            updateLayout()
            return
        }
        if operation == "list" {
            isRefreshingWorkspaces = true
        } else if operation == "create" {
            addWorkspaceMessage = ""
            if let name {
                pendingWorkspaceCreationNames[requestID] = name
            }
        } else {
            isPerformingWorkspaceOperation = true
        }
        pendingWorkspaceOperations[requestID] = operation
        if let workspaceID {
            pendingWorkspaceOperationWorkspaceIDs[requestID] = workspaceID
        }
        if operation != "list", operation != "create" {
            switch operation {
            case "start":
                workspacePanelMessage = "Starting container…"
            case "startApp":
                workspacePanelMessage = "Starting app…"
            case "stopApp":
                workspacePanelMessage = "Stopping app…"
            case "restartApp":
                workspacePanelMessage = "Restarting app…"
            case "mountFolder":
                workspacePanelMessage = "Mounting folder…"
            case "unmountFolder":
                workspacePanelMessage = "Unmounting folder…"
            case "chooseFolder":
                workspacePanelMessage = "Choosing folder…"
            case "delete":
                workspacePanelMessage = "Deleting container…"
            case "addRecipeStep":
                safeSpaceRecipeMessage = "Adding Dockerfile fragment…"
            case "updateDockerfile":
                safeSpaceRecipeMessage = "Saving Dockerfile…"
            case "updateContainerConfiguration":
                safeSpaceRecipeMessage = "Saving container configuration…"
            case "updateRecipeBaseImage":
                safeSpaceRecipeMessage = "Updating base image…"
            case "addRecipeUser":
                safeSpaceRecipeMessage = "Adding container user…"
            case "createRecipeScript":
                safeSpaceRecipeMessage = "Adding setup script…"
            case "renameRecipeScript":
                safeSpaceRecipeMessage = "Renaming setup script…"
            case "updateRecipeStep":
                safeSpaceRecipeMessage = "Saving Dockerfile fragment…"
            case "installRecipeCatalogItem":
                safeSpaceRecipeMessage = "Installing software…"
            case "addRecipeApp":
                safeSpaceRecipeMessage = "Adding app launcher…"
            case "deleteRecipeStep":
                safeSpaceRecipeMessage = rebuild == true
                    ? "Removing step and rebuilding…"
                    : "Removing build step…"
            case "rebuildRecipe":
                safeSpaceRecipeMessage = "Applying changes… This can take several minutes."
            case "changeRuntime":
                safeSpaceRecipeMessage = "Changing container runtime…"
            default:
                workspacePanelMessage = "Working…"
            }
        }
        let operationSession = operation == "start" ||
            operation == "rebuildRecipe" ||
            operation == "changeRuntime" ||
            (operation == "deleteRecipeStep" && rebuild == true)
            ? safeSpaceOperationSession
            : urlSession
        guard let safeSpacesEndpoint, let operationSession else {
            pendingWorkspaceOperations.removeValue(forKey: requestID)
            pendingWorkspaceOperationWorkspaceIDs.removeValue(forKey: requestID)
            if operation == "rebuildRecipe" || operation == "changeRuntime" {
                containerConfigurationRebuildWorkspaceID = nil
            }
            isPerformingWorkspaceOperation = false
            isRefreshingWorkspaces = false
            workspacePanelMessage = "Outer Shell's container service is unavailable."
            updateLayout()
            return
        }
        let usesSnapshot = operation == "list" && !includeDetails &&
            !isShowingWorkspacePanel && selectedRecipeSafeSpaceID == nil
        let endpoint = usesSnapshot
            ? safeSpacesEndpoint.deletingLastPathComponent().appendingPathComponent("container-snapshot")
            : safeSpacesEndpoint
        var urlRequest = URLRequest(url: endpoint)
        if !usesSnapshot {
            urlRequest.httpMethod = "POST"
            urlRequest.httpBody = payload
            urlRequest.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        operationSession.dataTask(with: urlRequest) { [weak self] data, response, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                guard error == nil,
                      (response as? HTTPURLResponse).map({
                          (200..<300).contains($0.statusCode)
                      }) != false,
                      let data else {
                    if operation == "updateDockerfile" {
                        self.restoreContainerConfigurationDockerfileSave()
                    }
                    if operation == "rebuildRecipe" || operation == "changeRuntime" {
                        self.containerConfigurationRebuildWorkspaceID = nil
                    }
                    self.pendingWorkspaceOperations.removeValue(forKey: requestID)
                    self.pendingWorkspaceOperationWorkspaceIDs.removeValue(forKey: requestID)
                    self.pendingWorkspaceCreationNames.removeValue(forKey: requestID)
                    self.isPerformingWorkspaceOperation = false
                    self.isRefreshingWorkspaces = false
                    let message = error?.localizedDescription
                        ?? "Outer Shell could not reach its container service."
                    if operation == "rebuildRecipe" || operation == "changeRuntime",
                       self.isContainerConfigurationEditorVisible {
                        self.containerConfigurationBuildError = message
                        self.containerConfigurationBuildErrorScroll = 0
                        self.containerConfigurationBuildErrorRenderedScroll = 0
                        self.containerConfigurationBuildErrorCopyConfirmationID = nil
                        self.safeSpaceRecipeMessage = ""
                        self.workspacePanelMessage = ""
                        self.blurWorkspaceRenameField()
                    } else {
                        self.workspacePanelMessage = message
                    }
                    self.scheduleLayoutUpdate()
                    return
                }
                self.handleWorkspaceResponse(data, requestID: requestID, fromSnapshot: usesSnapshot)
            }
        }.resume()
        if operation == "rebuildRecipe" || operation == "changeRuntime" {
            scheduleContainerBuildProgressRefresh()
        }
        if operation != "list" {
            scheduleLayoutUpdate()
        }
    }

    private func handleWorkspaceResponse(_ payload: Data, requestID: UUID? = nil, fromSnapshot: Bool = false) {
        guard let response = try? LocalWorkspaceHostResponse.decode(payload,
            snapshotRequestID: fromSnapshot ? requestID : nil) else {
            if pendingWorkspaceOperations.values.contains("rebuildRecipe") ||
                pendingWorkspaceOperations.values.contains("changeRuntime") {
                containerConfigurationRebuildWorkspaceID = nil
            }
            isPerformingWorkspaceOperation = false
            isRefreshingWorkspaces = false
            workspacePanelMessage = "Outer Shell returned an invalid container response."
            scheduleLayoutUpdate()
            return
        }
        let effectiveRequestID = requestID ?? response.requestID
        let operation = pendingWorkspaceOperations.removeValue(forKey: effectiveRequestID)
        let operationWorkspaceID = pendingWorkspaceOperationWorkspaceIDs.removeValue(
            forKey: effectiveRequestID
        )
        if operation == "list" { isRefreshingWorkspaces = false }
        let workspacesChanged = localWorkspaces != response.workspaces
        let previousOverviewAvailability = workspaceOverviewAvailable
        let previousPanelMessage = workspacePanelMessage
        if response.error == nil || !response.workspaces.isEmpty {
            localWorkspaces = response.workspaces
            fetchEndpointIcons()
        }
        if let pendingID = pendingDockerfileWorkspace?.id,
           let updatedWorkspace = localWorkspaces.first(where: { $0.id == pendingID }) {
            pendingDockerfileWorkspace = updatedWorkspace
        }
        if let selectedRecipeSafeSpaceID,
           !localWorkspaces.contains(where: { $0.id == selectedRecipeSafeSpaceID }) {
            navigateToRecipeSafeSpace(nil, pushHistory: false)
        } else if let selectedRecipeSafeSpaceID,
                  pendingDockerfileWorkspace == nil,
                  let workspace = localWorkspaces.first(where: {
                      $0.id == selectedRecipeSafeSpaceID
                  }) {
            if workspace.recipe != nil || fromSnapshot {
                beginContainerConfigurationEditor(for: workspace)
            }
        }
        if let providers = response.providers {
            availableSafeSpaceProviders = providers
        }
        if let selected = selectedContainerLogContext,
           let container = localWorkspaces.first(where: { $0.id == selected.container.id }),
           let app = container.apps.first(where: { $0.serviceID == selected.app.serviceID }) {
            selectedContainerLogContext = ContainerAppLauncherContext(container: container, app: app)
        }
        workspaceOverviewAvailable = workspaceContextID == nil
        if operation != "list", operation != "create" {
            isPerformingWorkspaceOperation = false
        }
        var shouldDismissContainerConfiguration = false
        var didBeginRebuildAfterDockerfileSave = false
        if operation == "updateDockerfile" {
            let shouldRebuild = containerConfigurationRebuildAfterDockerfileSave
            let shouldDismiss = containerConfigurationDismissAfterDockerfileSave
            containerConfigurationRebuildAfterDockerfileSave = false
            containerConfigurationDismissAfterDockerfileSave = false
            if response.error?.isEmpty == false,
               let previousDockerfile = pendingContainerConfigurationPreviousSavedDockerfile {
                containerConfigurationSavedDockerfile = previousDockerfile
                containerConfigurationRequiresRebuild =
                    pendingContainerConfigurationPreviousRequiresRebuild ?? false
            }
            pendingContainerConfigurationDockerfileSave = nil
            pendingContainerConfigurationPreviousSavedDockerfile = nil
            pendingContainerConfigurationPreviousRequiresRebuild = nil
            if shouldRebuild, response.error?.isEmpty != false {
                didBeginRebuildAfterDockerfileSave = true
                rebuildContainerConfiguration()
            }
            shouldDismissContainerConfiguration = shouldDismiss &&
                response.error?.isEmpty != false
        } else if operation == "updateContainerConfiguration" {
            if response.error?.isEmpty != false,
               let savedConfiguration = pendingContainerConfigurationRuntimeSave {
                containerConfigurationSavedEnvironment = savedConfiguration.environment
                containerConfigurationSavedPorts = savedConfiguration.ports
                containerConfigurationSavedMounts = savedConfiguration.mounts
                containerConfigurationRequiresRebuild = true
            }
            pendingContainerConfigurationRuntimeSave = nil
            if containerConfigurationEnvironment != containerConfigurationSavedEnvironment ||
                containerConfigurationPorts != containerConfigurationSavedPorts ||
                containerConfigurationMounts != containerConfigurationSavedMounts {
                scheduleContainerConfigurationEnvironmentSave(after: 0.2)
            }
        }
        if operation == "appLogs" {
            isLoadingLog = false
            if let error = response.error, !error.isEmpty {
                logError = error
            } else if let log = response.appLog {
                logSnapshot = LogResponse(serviceID: selectedServiceID ?? "",
                                          path: log.path,
                                          contents: log.contents,
                                          isTruncated: log.isTruncated,
                                          fileSize: log.fileSize,
                                          modified: log.modified,
                                          error: log.error)
                logError = log.error
                shouldScrollLogToBottomOnNextLayout = true
            } else {
                logError = "Outer Shell returned no container app logs."
            }
            scheduleLayoutUpdate()
            return
        }
        if operation == "create" {
            pendingWorkspaceCreationNames.removeValue(forKey: effectiveRequestID)
            addWorkspaceMessage = response.error ?? ""
        } else if operation == "chooseFolder" {
            if let error = response.error, !error.isEmpty {
                workspacePanelMessage = error
            } else if let path = response.selectedFolderPath, !path.isEmpty {
                appendSelectedContainerConfigurationFolder(path)
                workspacePanelMessage = ""
                persistContainerRuntimeConfiguration()
            }
        } else if operation == "addRecipeStep" ||
                    operation == "updateDockerfile" ||
                    operation == "updateContainerConfiguration" ||
                    operation == "updateRecipeBaseImage" ||
                    operation == "addRecipeUser" ||
                    operation == "createRecipeScript" ||
                    operation == "renameRecipeScript" ||
                    operation == "updateRecipeStep" ||
                    operation == "installRecipeCatalogItem" ||
                    operation == "addRecipeApp" ||
                    operation == "deleteRecipeStep" ||
                    operation == "rebuildRecipe" ||
                    operation == "changeRuntime" {
            if let error = response.error, !error.isEmpty {
                if operation == "rebuildRecipe" || operation == "changeRuntime",
                   isContainerConfigurationEditorVisible {
                    containerConfigurationBuildError = error
                    containerConfigurationBuildErrorScroll = 0
                    containerConfigurationBuildErrorRenderedScroll = 0
                    containerConfigurationBuildErrorCopyConfirmationID = nil
                    safeSpaceRecipeMessage = ""
                    workspacePanelMessage = ""
                    blurWorkspaceRenameField()
                } else {
                    safeSpaceRecipeMessage = error
                }
            } else if operation == "addRecipeStep" ||
                        operation == "updateDockerfile" ||
                        operation == "updateContainerConfiguration" ||
                        operation == "updateRecipeBaseImage" ||
                        operation == "addRecipeUser" ||
                        operation == "createRecipeScript" ||
                        operation == "renameRecipeScript" ||
                        operation == "updateRecipeStep" ||
                        operation == "installRecipeCatalogItem" ||
                        operation == "addRecipeApp" {
                if response.recipeCommandApplied == true {
                    if operation == "installRecipeCatalogItem" {
                        safeSpaceRecipeMessage = "Software installed and added to the recipe."
                    } else if operation == "addRecipeApp" {
                        safeSpaceRecipeMessage = "App launcher added."
                    } else {
                        safeSpaceRecipeMessage = "Dockerfile fragment applied to the running container."
                    }
                } else if let output = response.recipeCommandOutput,
                          !output.isEmpty {
                    safeSpaceRecipeMessage = "Build step saved, but live application failed: \(output)"
                } else {
                    if operation == "updateContainerConfiguration" {
                        safeSpaceRecipeMessage = "Container configuration saved. Rebuild to apply it."
                    } else if operation == "updateDockerfile" {
                        if !didBeginRebuildAfterDockerfileSave {
                            safeSpaceRecipeMessage = "Dockerfile saved. Rebuild when you want to recreate the container."
                        }
                    } else if operation == "updateRecipeBaseImage" {
                        safeSpaceRecipeMessage = "Base image updated. Rebuild to use it."
                    } else if operation == "addRecipeApp" {
                        safeSpaceRecipeMessage = "App launcher added. Rebuild the container to make it available."
                    } else if operation == "addRecipeUser" {
                        safeSpaceRecipeMessage = "Container user added. Rebuild to create it."
                    } else if operation == "createRecipeScript" {
                        safeSpaceRecipeMessage = "Setup script added. Edit it in Plaintext, then rebuild when ready."
                    } else if operation == "renameRecipeScript" {
                        safeSpaceRecipeMessage = "Setup script renamed. Its alphabetical run order has been updated."
                    } else {
                        safeSpaceRecipeMessage = "Dockerfile fragment saved. Rebuild to update the running container."
                    }
                }
            } else if operation == "deleteRecipeStep" {
                safeSpaceRecipeMessage = response.recipeCommandApplied == true
                    ? "Build step removed and the container rebuilt."
                    : "Build step removed. Rebuild to update the container."
            } else if operation != "rebuildRecipe" && operation != "changeRuntime" {
                containerConfigurationBuildError = nil
                containerConfigurationBuildErrorScroll = 0
                safeSpaceRecipeMessage = "Container rebuilt. Apps are ready to use."
            }
        } else if let error = response.error, !error.isEmpty {
            if operation == "start",
               let operationWorkspaceID,
               let workspace = localWorkspaces.first(where: {
                   $0.id == operationWorkspaceID
               }) {
                navigateToRecipeSafeSpace(workspace.id, pushHistory: true)
                containerConfigurationBuildError = error
                containerConfigurationBuildErrorScroll = 0
                containerConfigurationBuildErrorRenderedScroll = 0
                containerConfigurationBuildErrorCopyConfirmationID = nil
                workspacePanelMessage = ""
                blurWorkspaceRenameField()
            } else {
                workspacePanelMessage = error
            }
        } else {
            workspacePanelMessage = ""
        }
        if shouldDismissContainerConfiguration {
            returnFromRecipeSafeSpace()
        }
        if operation != "list" ||
            workspacesChanged ||
            previousOverviewAvailability != workspaceOverviewAvailable ||
            previousPanelMessage != workspacePanelMessage {
            scheduleLayoutUpdate()
        }
        if operation == "list", selectedContainerLogContext != nil {
            DispatchQueue.main.async { [weak self] in
                self?.fetchSelectedLog(quiet: true)
            }
        }
        updateContainerBuildMonitoring(operation: operation, responseError: response.error)
        scheduleContainerBuildProgressRefresh()
    }

    private func scheduleContainerBuildProgressRefresh() {
        guard containerConfigurationRebuildWorkspaceID != nil,
              isContainerConfigurationEditorVisible else {
            containerBuildProgressRefreshGeneration += 1
            return
        }
        containerBuildProgressRefreshGeneration += 1
        let generation = containerBuildProgressRefreshGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            guard let self,
                  self.containerBuildProgressRefreshGeneration == generation,
                  self.containerConfigurationRebuildWorkspaceID != nil,
                  self.isContainerConfigurationEditorVisible else {
                return
            }
            if self.isRefreshingWorkspaces {
                self.scheduleContainerBuildProgressRefresh()
            } else {
                self.sendWorkspaceRequest(operation: "list")
            }
        }
    }

    private func isActiveContainerBuildProgress(
        _ progress: LocalWorkspaceRecord.BuildProgress?
    ) -> Bool {
        guard let progress else { return false }
        return progress.phase != "complete" && progress.phase != "failed"
    }

    private func updateContainerBuildMonitoring(operation: String?,
                                                responseError: String?) {
        guard let workspaceID = containerConfigurationRebuildWorkspaceID else { return }
        guard operation == "rebuildRecipe" ||
                operation == "changeRuntime" ||
                operation == "list" else { return }
        if operation == "rebuildRecipe" || operation == "changeRuntime",
           let responseError,
           !responseError.isEmpty {
            containerConfigurationRebuildWorkspaceID = nil
            return
        }
        guard let workspace = localWorkspaces.first(where: { $0.id == workspaceID }) else {
            return
        }
        guard let progress = workspace.buildProgress else {
            if workspace.state != "rebuilding" {
                containerConfigurationRebuildWorkspaceID = nil
                containerConfigurationRequiresRebuild = false
                safeSpaceRecipeMessage = "Container rebuilt. Apps are ready to use."
            }
            return
        }
        switch progress.phase {
        case "complete":
            containerConfigurationRebuildWorkspaceID = nil
            containerConfigurationRequiresRebuild = false
            safeSpaceRecipeMessage = progress.detail
            updateLayout()
        case "failed":
            containerConfigurationRebuildWorkspaceID = nil
            let log = progress.log.trimmingCharacters(in: .whitespacesAndNewlines)
            containerConfigurationBuildError = log.isEmpty
                ? progress.detail
                : log
            containerConfigurationBuildErrorScroll = 0
            containerConfigurationBuildErrorRenderedScroll = 0
            containerConfigurationBuildErrorCopyConfirmationID = nil
            safeSpaceRecipeMessage = ""
            workspacePanelMessage = ""
            blurWorkspaceRenameField()
            updateLayout()
        default:
            break
        }
    }

    private func scheduleWorkspaceRefresh() {
        workspaceRefreshGeneration += 1
        let generation = workspaceRefreshGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            guard let self,
                  self.workspaceRefreshGeneration == generation,
                  self.workspaceOverviewAvailable,
                  self.workspaceContextID == nil,
                  !self.isPerformingWorkspaceOperation,
                  !self.isRefreshingWorkspaces else {
                return
            }
            self.sendWorkspaceRequest(operation: "list")
        }
    }

    private func performContainerTransferRequest(
        operation: String,
        values: [String: Any] = [:],
        completion: @escaping @MainActor (Result<LocalWorkspaceHostResponse, Error>) -> Void
    ) {
        guard let safeSpacesEndpoint, let safeSpaceOperationSession else {
            completion(.failure(NSError(
                domain: "org.outershell.transfer",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "Outer Shell's container service is unavailable."]
            )))
            return
        }
        var requestValue = values
        requestValue["requestID"] = UUID().uuidString.lowercased()
        requestValue["operation"] = operation
        let payload: Data
        do {
            payload = try JSONSerialization.data(withJSONObject: requestValue)
        } catch {
            completion(.failure(error))
            return
        }
        var request = URLRequest(url: safeSpacesEndpoint)
        request.httpMethod = "POST"
        request.httpBody = payload
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        safeSpaceOperationSession.dataTask(with: request) { data, response, error in
            Task { @MainActor in
                if let error {
                    completion(.failure(error))
                    return
                }
                guard let data else {
                    completion(.failure(NSError(
                        domain: "org.outershell.transfer",
                        code: 2,
                        userInfo: [NSLocalizedDescriptionKey: "The container service returned no response."]
                    )))
                    return
                }
                let decoded = try? JSONDecoder().decode(LocalWorkspaceHostResponse.self, from: data)
                if let message = decoded?.error, !message.isEmpty {
                    completion(.failure(NSError(
                        domain: "org.outershell.transfer",
                        code: 3,
                        userInfo: [NSLocalizedDescriptionKey: message]
                    )))
                    return
                }
                if let response = response as? HTTPURLResponse,
                   !(200..<300).contains(response.statusCode) {
                    let body = String(data: data, encoding: .utf8)?
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    let detail = body.flatMap { $0.isEmpty ? nil : $0 }
                        ?? "The container service returned HTTP \(response.statusCode)."
                    completion(.failure(NSError(
                        domain: "org.outershell.transfer",
                        code: 2,
                        userInfo: [NSLocalizedDescriptionKey: detail]
                    )))
                    return
                }
                do {
                    let value = try decoded ?? JSONDecoder().decode(
                        LocalWorkspaceHostResponse.self,
                        from: data
                    )
                    completion(.success(value))
                } catch {
                    completion(.failure(error))
                }
            }
        }.resume()
    }

    private func performContainerBinaryTransferRequest(
        _ payload: Data,
        completion: @escaping @MainActor (Result<ContainerTransferBinaryResponse, Error>) -> Void
    ) {
        guard let safeSpacesEndpoint, let safeSpaceOperationSession else {
            completion(.failure(ContainerConfigurationInputError(
                message: "Outer Shell's container service is unavailable."
            )))
            return
        }
        var request = URLRequest(url: safeSpacesEndpoint)
        request.httpMethod = "POST"
        request.httpBody = payload
        request.timeoutInterval = ContainerTransferBinaryCodec.requestTimeout
        request.setValue("application/vnd.outershell.container-transfer",
                         forHTTPHeaderField: "Content-Type")
        safeSpaceOperationSession.dataTask(with: request) { data, response, error in
            Task { @MainActor in
                if let error {
                    completion(.failure(error))
                    return
                }
                guard let data else {
                    completion(.failure(ContainerConfigurationInputError(
                        message: "The container service returned no transfer response."
                    )))
                    return
                }
                if let response = response as? HTTPURLResponse,
                   !(200..<300).contains(response.statusCode) {
                    let detail = String(data: data, encoding: .utf8)?
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    let message: String
                    if let detail, !detail.isEmpty {
                        message = detail
                    } else {
                        message = "The container service returned HTTP \(response.statusCode)."
                    }
                    completion(.failure(ContainerConfigurationInputError(
                        message: message
                    )))
                    return
                }
                do {
                    completion(.success(try ContainerTransferBinaryCodec.response(from: data)))
                } catch {
                    completion(.failure(error))
                }
            }
        }.resume()
    }

    private func showShareContainerOptions(for workspace: LocalWorkspaceRecord,
                                           at point: CGPoint) {
        let menuID = UUID()
        var options: [String: (includePersistentData: Bool, includeMountedFolders: Bool)] = [
            "project": (false, false),
            "persistent": (true, false)
        ]
        var items = [
            OuterframeContextMenuItem(
                id: "project",
                title: "Container project only (recommended)",
                isEnabled: true,
                systemImageName: "doc.zipper"
            ),
            OuterframeContextMenuItem(
                id: "persistent",
                title: "Include persistent data (may contain credentials)",
                isEnabled: true,
                systemImageName: "externaldrive"
            )
        ]
        if !workspace.overviewMounts.isEmpty {
            options["complete"] = (true, true)
            items.append(OuterframeContextMenuItem(
                id: "complete",
                title: "Include persistent data and mounted folders…",
                isEnabled: true,
                systemImageName: "folder.badge.plus"
            ))
        }
        pendingShareScopeMenuActions[menuID] = (workspace, options)
        outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
    }

    private func prepareSharedContainer(_ workspace: LocalWorkspaceRecord,
                                        includePersistentData: Bool,
                                        includeMountedFolders: Bool) {
        guard let stagingDirectory = outerframeHost.stagedFileDirectoryURL else {
            workspacePanelMessage = "Outer Loop did not provide a place to stage the shared container."
            updateLayout()
            return
        }
        let fileName = safeSharedContainerFileName(workspace.name)
        let directory = stagingDirectory.appendingPathComponent(
            UUID().uuidString.lowercased(), isDirectory: true
        )
        let destination = directory.appendingPathComponent(fileName)
        do {
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
        } catch {
            workspacePanelMessage = error.localizedDescription
            updateLayout()
            return
        }
        isPerformingWorkspaceOperation = true
        workspacePanelMessage = "Preparing shared container…"
        updateLayout()
        performContainerTransferRequest(
            operation: "prepareShare",
            values: [
                "workspaceID": workspace.id.uuidString.lowercased(),
                "includePersistentData": includePersistentData,
                "includeMountedFolders": includeMountedFolders,
                "stagingPath": destination.path
            ]
        ) { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let error):
                try? FileManager.default.removeItem(at: directory)
                self.isPerformingWorkspaceOperation = false
                self.workspacePanelMessage = error.localizedDescription
                self.updateLayout()
            case .success(let response):
                guard let transferID = response.transferID,
                      let responseFileName = response.fileName,
                      let byteCount = response.byteCount else {
                    try? FileManager.default.removeItem(at: directory)
                    self.isPerformingWorkspaceOperation = false
                    self.workspacePanelMessage = "Outer Shell did not return a usable shared container."
                    self.updateLayout()
                    return
                }
                if response.stagedDirectly == true {
                    self.isPerformingWorkspaceOperation = false
                    self.workspacePanelMessage = ""
                    self.sharedContainerFile = (destination, responseFileName, byteCount)
                    self.isShowingWorkspacePanel = true
                    self.updateLayout()
                    return
                }
                do {
                    try Data().write(to: destination)
                    self.downloadSharedContainer(
                        transferID: transferID,
                        destination: destination,
                        fileName: responseFileName,
                        byteCount: byteCount,
                        offset: 0
                    )
                } catch {
                    try? FileManager.default.removeItem(at: directory)
                    self.isPerformingWorkspaceOperation = false
                    self.workspacePanelMessage = error.localizedDescription
                    self.updateLayout()
                }
            }
        }
    }

    private func safeSharedContainerFileName(_ name: String) -> String {
        let characters = name.map { character -> Character in
            character.isLetter || character.isNumber || character == "-" || character == "_"
                ? character
                : "-"
        }
        let stem = String(characters)
        return "\(stem.isEmpty ? "Container" : stem).outershell-container"
    }

    private func downloadSharedContainer(transferID: String,
                                         destination: URL,
                                         fileName: String,
                                         byteCount: Int,
                                         offset: Int) {
        guard let transferUUID = UUID(uuidString: transferID) else {
            finishSharedContainerImport(message: "The shared container transfer identifier is invalid.")
            return
        }
        let request = ContainerTransferBinaryCodec.request(
            operation: 1,
            transferID: transferUUID,
            offset: UInt64(offset),
            length: UInt64(ContainerTransferBinaryCodec.chunkSize)
        )
        performContainerBinaryTransferRequest(request) { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let error):
                self.isPerformingWorkspaceOperation = false
                self.workspacePanelMessage = error.localizedDescription
                self.updateLayout()
            case .success(let response):
                let data = response.data
                let nextOffset = Int(response.nextOffset)
                do {
                    let handle = try FileHandle(forWritingTo: destination)
                    defer { try? handle.close() }
                    try handle.seekToEnd()
                    try handle.write(contentsOf: data)
                } catch {
                    self.isPerformingWorkspaceOperation = false
                    self.workspacePanelMessage = error.localizedDescription
                    self.updateLayout()
                    return
                }
                if response.nextOffset >= response.totalLength {
                    self.isPerformingWorkspaceOperation = false
                    self.workspacePanelMessage = ""
                    self.sharedContainerFile = (destination, fileName, byteCount)
                    self.isShowingWorkspacePanel = true
                    self.updateLayout()
                } else {
                    let percent = byteCount > 0
                        ? min(Int((Double(nextOffset) / Double(byteCount)) * 100), 99)
                        : 0
                    self.workspacePanelMessage = "Preparing shared container… \(percent)%"
                    self.updateLayout()
                    self.downloadSharedContainer(
                        transferID: transferID,
                        destination: destination,
                        fileName: fileName,
                        byteCount: byteCount,
                        offset: nextOffset
                    )
                }
            }
        }
    }

    private func showSharedContainerImportRuntimeMenu(for url: URL, at point: CGPoint) {
        let providers = availableSafeSpaceProviders.filter(\.canCreate)
        guard !providers.isEmpty else {
            workspacePanelMessage = "No container runtime is available on this server."
            updateLayout()
            return
        }
        let menuID = UUID()
        pendingImportRuntimeMenuActions[menuID] = (
            url,
            Dictionary(uniqueKeysWithValues: providers.map { ($0.id, $0.id) })
        )
        let items = providers.map { provider in
            OuterframeContextMenuItem(
                id: provider.id,
                title: "Import using \(provider.name)",
                isEnabled: true,
                systemImageName: provider.capabilities.supportsLiveMounts
                    ? "server.rack"
                    : "shippingbox"
            )
        }
        outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
    }

    private func importSharedContainer(at url: URL, runtimeProviderID: String) {
        isPerformingWorkspaceOperation = true
        workspacePanelMessage = "Importing shared container…"
        updateLayout()
        performContainerTransferRequest(operation: "beginImport") { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let error):
                self.finishSharedContainerImport(error: error)
            case .success(let response):
                guard let transferID = response.transferID else {
                    self.finishSharedContainerImport(message: "The server could not begin the import.")
                    return
                }
                do {
                    let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
                    let handle = try FileHandle(forReadingFrom: url)
                    self.resumeSharedContainerUpload(
                        transferID: transferID,
                        runtimeProviderID: runtimeProviderID,
                        handle: handle,
                        byteCount: size,
                        retryCount: 0
                    )
                } catch {
                    self.finishSharedContainerImport(error: error)
                }
            }
        }
    }

    private func containerUploadEndpoint(transferID: String) -> URL? {
        guard let safeSpacesEndpoint,
              var components = URLComponents(
                  url: safeSpacesEndpoint,
                  resolvingAgainstBaseURL: true
              ) else {
            return nil
        }
        components.path = "/api/container-transfers/\(transferID.lowercased())"
        components.query = nil
        components.fragment = nil
        return components.url
    }

    private func uploadOffset(from response: URLResponse?) -> Int? {
        guard let response = response as? HTTPURLResponse,
              let text = response.value(forHTTPHeaderField: "Upload-Offset"),
              let value = UInt64(text),
              value <= UInt64(Int.max) else {
            return nil
        }
        return Int(value)
    }

    private func resumeSharedContainerUpload(transferID: String,
                                             runtimeProviderID: String,
                                             handle: FileHandle,
                                             byteCount: Int,
                                             retryCount: Int) {
        guard let endpoint = containerUploadEndpoint(transferID: transferID),
              let safeSpaceOperationSession else {
            try? handle.close()
            finishSharedContainerImport(
                message: "Outer Shell's container transfer service is unavailable."
            )
            return
        }
        var request = URLRequest(url: endpoint)
        request.httpMethod = "HEAD"
        request.timeoutInterval = ContainerResumableUpload.requestTimeout
        request.setValue("outershell-resumable-v1", forHTTPHeaderField: "Upload-Protocol")
        safeSpaceOperationSession.dataTask(with: request) { [weak self] _, response, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if let error {
                    self.retrySharedContainerUpload(
                        transferID: transferID,
                        runtimeProviderID: runtimeProviderID,
                        handle: handle,
                        byteCount: byteCount,
                        sentByteCount: 0,
                        retryCount: retryCount,
                        error: error
                    )
                    return
                }
                guard let httpResponse = response as? HTTPURLResponse,
                      (200..<300).contains(httpResponse.statusCode),
                      let offset = self.uploadOffset(from: response),
                      offset <= byteCount else {
                    try? handle.close()
                    self.finishSharedContainerImport(
                        message: "The destination returned an invalid container transfer offset."
                    )
                    return
                }
                self.uploadSharedContainerSegment(
                    transferID: transferID,
                    runtimeProviderID: runtimeProviderID,
                    handle: handle,
                    byteCount: byteCount,
                    sentByteCount: offset,
                    retryCount: retryCount
                )
            }
        }.resume()
    }

    private func uploadSharedContainerSegment(transferID: String,
                                              runtimeProviderID: String,
                                              handle: FileHandle,
                                              byteCount: Int,
                                              sentByteCount: Int,
                                              retryCount: Int) {
        let data: Data
        do {
            try handle.seek(toOffset: UInt64(sentByteCount))
            data = try handle.read(upToCount: ContainerResumableUpload.segmentSize) ?? Data()
        } catch {
            try? handle.close()
            finishSharedContainerImport(error: error)
            return
        }
        if data.isEmpty {
            try? handle.close()
            finishUploadedSharedContainer(
                transferID: transferID,
                runtimeProviderID: runtimeProviderID
            )
            return
        }
        guard let endpoint = containerUploadEndpoint(transferID: transferID),
              let safeSpaceOperationSession else {
            try? handle.close()
            finishSharedContainerImport(
                message: "Outer Shell's container transfer service is unavailable."
            )
            return
        }
        var request = URLRequest(url: endpoint)
        request.httpMethod = "PATCH"
        request.timeoutInterval = ContainerResumableUpload.requestTimeout
        request.setValue("application/offset+octet-stream", forHTTPHeaderField: "Content-Type")
        request.setValue("outershell-resumable-v1", forHTTPHeaderField: "Upload-Protocol")
        request.setValue(String(sentByteCount), forHTTPHeaderField: "Upload-Offset")
        request.setValue(String(data.count), forHTTPHeaderField: "Content-Length")
        safeSpaceOperationSession.uploadTask(with: request, from: data) { [weak self] _, response, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if let error {
                    self.retrySharedContainerUpload(
                        transferID: transferID,
                        runtimeProviderID: runtimeProviderID,
                        handle: handle,
                        byteCount: byteCount,
                        sentByteCount: sentByteCount,
                        retryCount: retryCount,
                        error: error
                    )
                    return
                }
                guard let httpResponse = response as? HTTPURLResponse else {
                    try? handle.close()
                    self.finishSharedContainerImport(
                        message: "The destination returned no container transfer response."
                    )
                    return
                }
                if httpResponse.statusCode == 409 {
                    self.resumeSharedContainerUpload(
                        transferID: transferID,
                        runtimeProviderID: runtimeProviderID,
                        handle: handle,
                        byteCount: byteCount,
                        retryCount: retryCount
                    )
                    return
                }
                guard (200..<300).contains(httpResponse.statusCode),
                      let total = self.uploadOffset(from: response),
                      total > sentByteCount,
                      total <= byteCount else {
                    try? handle.close()
                    self.finishSharedContainerImport(
                        message: "The destination did not accept the container transfer segment."
                    )
                    return
                }
                let percent = byteCount > 0
                    ? min(Int((Double(total) / Double(byteCount)) * 100), 99)
                    : 0
                self.workspacePanelMessage = "Importing shared container… \(percent)%"
                self.updateLayout()
                self.uploadSharedContainerSegment(
                    transferID: transferID,
                    runtimeProviderID: runtimeProviderID,
                    handle: handle,
                    byteCount: byteCount,
                    sentByteCount: total,
                    retryCount: 0
                )
            }
        }.resume()
    }

    private func finishUploadedSharedContainer(transferID: String,
                                               runtimeProviderID: String,
                                               mountDestinationRoot: String? = nil) {
        var values = [
            "transferID": transferID,
            "runtimeProviderID": runtimeProviderID
        ]
        if let mountDestinationRoot {
            values["mountDestinationRoot"] = mountDestinationRoot
        }
        performContainerTransferRequest(
            operation: "finishImport",
            values: values
        ) { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let error):
                if self.pendingSharedContainerImport != nil {
                    self.isPerformingWorkspaceOperation = false
                    self.workspacePanelMessage = error.localizedDescription
                    self.focusWorkspaceRenameField(selectAll: false)
                    self.updateLayout()
                } else {
                    self.finishSharedContainerImport(error: error)
                }
            case .success(let response):
                if response.needsMountDestination == true,
                   let suggestedMountRoot = response.suggestedMountRoot,
                   let mounts = response.importMounts,
                   !mounts.isEmpty {
                    self.pendingSharedContainerImport = PendingSharedContainerImport(
                        transferID: transferID,
                        runtimeProviderID: runtimeProviderID,
                        name: response.importName ?? "Imported Container",
                        mounts: mounts
                    )
                    self.isPerformingWorkspaceOperation = false
                    self.workspaceNamePromptDismissesPanel = true
                    self.workspaceRenameName = suggestedMountRoot
                    self.workspacePanelMessage = ""
                    self.isShowingWorkspacePanel = true
                    self.focusWorkspaceRenameField(selectAll: false)
                    self.updateLayout()
                    return
                }
                self.pendingSharedContainerImport = nil
                if let providers = response.providers {
                    self.availableSafeSpaceProviders = providers
                }
                self.localWorkspaces = response.workspaces
                let omitted = response.omittedMountCount ?? 0
                let message = omitted > 0
                    ? "Imported the container. \(omitted) folder mount(s) need to be reconnected."
                    : "Imported the container. Its image is being built."
                self.blurWorkspaceRenameField()
                self.workspaceNamePromptDismissesPanel = false
                self.workspaceRenameName = ""
                self.isShowingWorkspacePanel = false
                self.finishSharedContainerImport(message: message)
                self.scheduleWorkspaceRefresh()
            }
        }
    }

    private func retrySharedContainerUpload(transferID: String,
                                            runtimeProviderID: String,
                                            handle: FileHandle,
                                            byteCount: Int,
                                            sentByteCount: Int,
                                            retryCount: Int,
                                            error: Error) {
        guard retryCount < ContainerResumableUpload.maximumRetryCount,
              canRetryContainerTransfer(after: error) else {
            try? handle.close()
            finishSharedContainerImport(error: error)
            return
        }
        _ = recoverNetworkingIfNeeded(after: error)
        let percent = byteCount > 0
            ? min(Int((Double(sentByteCount) / Double(byteCount)) * 100), 99)
            : 0
        workspacePanelMessage = "Importing shared container… \(percent)% · Reconnecting…"
        updateLayout()
        let delay = min(pow(2.0, Double(retryCount)), 8.0)
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            self?.resumeSharedContainerUpload(
                transferID: transferID,
                runtimeProviderID: runtimeProviderID,
                handle: handle,
                byteCount: byteCount,
                retryCount: retryCount + 1
            )
        }
    }

    private func canRetryContainerTransfer(after error: Error) -> Bool {
        let value = error as NSError
        if value.domain == NSURLErrorDomain {
            switch value.code {
            case NSURLErrorCannotConnectToHost,
                 NSURLErrorNetworkConnectionLost,
                 NSURLErrorNotConnectedToInternet,
                 NSURLErrorCannotFindHost,
                 NSURLErrorDNSLookupFailed,
                 NSURLErrorTimedOut:
                return true
            default:
                break
            }
        }
        let message = error.localizedDescription.lowercased()
        return message.contains("connection") ||
            message.contains("timed out") ||
            message.contains("failed to send request") ||
            message.contains("temporarily unavailable")
    }

    private func finishSharedContainerImport(error: Error) {
        finishSharedContainerImport(message: error.localizedDescription)
    }

    private func finishSharedContainerImport(message: String) {
        isPerformingWorkspaceOperation = false
        workspacePanelMessage = message
        updateLayout()
    }

    private func showSafeSpaceProviderMenu(at point: CGPoint) {
        let providers: [LocalSafeSpaceProviderRecord]
        if availableSafeSpaceProviders.isEmpty {
            providers = [
                LocalSafeSpaceProviderRecord(
                    id: "apple.container",
                    name: "Apple container",
                    detail: "Portable OCI container",
                    defaultBaseImage: "outershell/container-base:6",
                    isolationName: "",
                    isAvailable: true,
                    capabilities: LocalWorkspaceRecord.Capabilities(
                        supportsApps: true,
                        supportsShell: true,
                        supportsLiveMounts: false,
                        supportsMounts: true,
                        supportsRecipes: true
                    )
                )
            ]
        } else {
            providers = availableSafeSpaceProviders
        }
        let availableProviders = providers.filter(\.canCreate)
        if availableProviders.count == 1, let provider = availableProviders.first {
            createWorkspace(providerID: provider.id)
            return
        }
        let menuID = UUID()
        pendingSafeSpaceProviderMenuSelections[menuID] = Dictionary(
            uniqueKeysWithValues: providers.map { ($0.id, $0.id) }
        )
        let items = providers.map { provider in
            OuterframeContextMenuItem(
                id: provider.id,
                title: "\(provider.name) — \(provider.detail)",
                isEnabled: provider.canCreate,
                systemImageName: provider.capabilities.supportsLiveMounts
                    ? "server.rack"
                    : "shippingbox"
            )
        }
        outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
    }

    private func showContainerPathMenu(at point: CGPoint) {
        guard let recipe = pendingDockerfileWorkspace?.recipe else { return }
        let menuID = UUID()
        pendingContainerPathMenuSelections[menuID] = [
            "server": recipe.dockerfileHostPath,
            "container": recipe.dockerfileGuestPath
        ]
        let items = [
            OuterframeContextMenuItem(id: "server",
                                      title: "Path on Server",
                                      isEnabled: true,
                                      systemImageName: "server.rack"),
            OuterframeContextMenuItem(id: "container",
                                      title: "Path Inside Container",
                                      isEnabled: true,
                                      systemImageName: "shippingbox")
        ]
        outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
    }

    private func createWorkspace(providerID: String) {
        let existingNames = Set(
            localWorkspaces.map { $0.name.lowercased() } +
                pendingWorkspaceCreationNames.values.map { $0.lowercased() }
        )
        var index = existingNames.count + 1
        while existingNames.contains("container \(index)") {
            index += 1
        }
        workspaceNamePromptDismissesPanel = !isShowingWorkspacePanel
        isShowingWorkspacePanel = true
        isCreatingWorkspace = true
        selectedSafeSpaceProviderID = providerID
        creationBaseImage = "debian:bookworm"
        baseImageTemplate = .outerShell
        pendingCreationContainerName = "Container \(index)"
        isEditingCreationBaseImage = false
        pendingWorkspaceRename = nil
        pendingRecipeBaseImageWorkspace = nil
        workspaceRenameName = pendingCreationContainerName
        workspacePanelMessage = ""
        focusWorkspaceRenameField(selectAll: true)
        updateLayout()
    }

    private func showRecipeCommandPrompt(for workspace: LocalWorkspaceRecord) {
        workspaceNamePromptDismissesPanel = true
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        pendingWorkspaceRename = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeCommandWorkspace = workspace
        workspaceRenameName = """
        RUN apt-get update \\
            && apt-get install -y --no-install-recommends package-name \\
            && rm -rf /var/lib/apt/lists/*
        """
        workspacePanelMessage = ""
        focusWorkspaceRenameField(selectAll: true)
        updateLayout()
    }

    private func showDockerfileEditor(for workspace: LocalWorkspaceRecord) {
        beginContainerConfigurationEditor(for: workspace)
        updateLayout()
    }

    private func beginContainerConfigurationEditor(
        for workspace: LocalWorkspaceRecord
    ) {
        guard let recipe = workspace.recipe else {
            selectedRecipeSafeSpaceID = workspace.id
            sendWorkspaceRequest(operation: "list", includeDetails: true)
            return
        }
        workspaceNamePromptDismissesPanel = true
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        pendingWorkspaceRename = nil
        pendingRecipeCommandWorkspace = nil
        pendingDockerfileWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        pendingSafeSpaceAppWorkspace = nil
        pendingSafeSpaceAppKind = nil
        pendingDockerfileWorkspace = workspace
        containerConfigurationTab = .dockerfile
        containerConfigurationDockerfile = recipe.containerfile
        containerConfigurationSavedDockerfile = recipe.containerfile
        containerConfigurationEnvironment = recipe.environment.map {
            "\($0.name)=\($0.value)"
        }.joined(separator: "\n")
        containerConfigurationSavedEnvironment = containerConfigurationEnvironment
        containerConfigurationPorts = recipe.publishedPorts.map {
            "\($0.hostPort):\($0.containerPort)"
        }.joined(separator: "\n")
        containerConfigurationSavedPorts = containerConfigurationPorts
        containerConfigurationMounts = workspace.visibleMounts.map {
            ContainerConfigurationMountDraft(
                id: $0.id,
                name: $0.name,
                hostPath: $0.hostPath,
                guestPath: $0.guestPath,
                isReadOnly: $0.isReadOnly,
                isInfrastructure: $0.isInfrastructureMount
            )
        }
        containerConfigurationSavedMounts = containerConfigurationMounts
        containerConfigurationRequiresRebuild = recipe.needsRebuild
        containerConfigurationRebuildWorkspaceID = isActiveContainerBuildProgress(
            workspace.buildProgress
        ) ? workspace.id : nil
        isConfirmingContainerConfigurationRebuild = false
        isConfirmingContainerConfigurationDismissal = false
        containerConfigurationRebuildAfterDockerfileSave = false
        containerConfigurationDismissAfterDockerfileSave = false
        containerConfigurationEnvironmentSaveGeneration += 1
        pendingContainerConfigurationDockerfileSave = nil
        pendingContainerConfigurationPreviousSavedDockerfile = nil
        pendingContainerConfigurationPreviousRequiresRebuild = nil
        pendingContainerConfigurationRuntimeSave = nil
        containerConfigurationTextScroll = 0
        containerConfigurationMountScroll = 0
        containerConfigurationBuildError = nil
        containerConfigurationBuildErrorScroll = 0
        containerConfigurationBuildErrorRenderedScroll = 0
        containerConfigurationBuildErrorContentHeight = 0
        containerConfigurationBuildErrorViewportLayer = nil
        containerConfigurationBuildErrorCopyConfirmationID = nil
        workspaceRenameName = containerConfigurationDockerfile
        workspacePanelMessage = ""
        focusWorkspaceRenameField()
        scheduleContainerBuildProgressRefresh()
    }

    private func endContainerConfigurationEditor() {
        guard pendingDockerfileWorkspace != nil else { return }
        blurWorkspaceRenameField()
        pendingDockerfileWorkspace = nil
        isShowingWorkspacePanel = false
        workspaceNamePromptDismissesPanel = false
        workspaceRenameName = ""
        containerConfigurationDockerfile = ""
        containerConfigurationSavedDockerfile = ""
        containerConfigurationEnvironment = ""
        containerConfigurationSavedEnvironment = ""
        containerConfigurationPorts = ""
        containerConfigurationSavedPorts = ""
        containerConfigurationMounts = []
        containerConfigurationSavedMounts = []
        containerConfigurationRequiresRebuild = false
        containerConfigurationRebuildWorkspaceID = nil
        isConfirmingContainerConfigurationRebuild = false
        isConfirmingContainerConfigurationDismissal = false
        containerConfigurationRebuildAfterDockerfileSave = false
        containerConfigurationDismissAfterDockerfileSave = false
        containerConfigurationEnvironmentSaveGeneration += 1
        pendingContainerConfigurationDockerfileSave = nil
        pendingContainerConfigurationPreviousSavedDockerfile = nil
        pendingContainerConfigurationPreviousRequiresRebuild = nil
        pendingContainerConfigurationRuntimeSave = nil
        containerConfigurationTextScroll = 0
        containerConfigurationMountScroll = 0
        containerConfigurationBuildError = nil
        containerConfigurationBuildErrorScroll = 0
        containerConfigurationBuildErrorRenderedScroll = 0
        containerConfigurationBuildErrorContentHeight = 0
        containerConfigurationBuildErrorViewportLayer = nil
        containerConfigurationBuildErrorCopyConfirmationID = nil
    }

    private func selectContainerConfigurationTab(_ tab: ContainerConfigurationTab) {
        guard isContainerConfigurationEditorVisible,
              containerConfigurationTab != tab else {
            return
        }
        if workspaceRenameInputController.isFocused {
            switch containerConfigurationTab {
            case .dockerfile:
                containerConfigurationDockerfile = workspaceRenameInputController.text
            case .environment:
                containerConfigurationEnvironment = workspaceRenameInputController.text
                scheduleContainerConfigurationEnvironmentSave(after: 0)
            case .ports:
                containerConfigurationPorts = workspaceRenameInputController.text
                scheduleContainerConfigurationEnvironmentSave(after: 0)
            case .mounts, .runtime:
                break
            }
        }
        containerConfigurationTab = tab
        containerConfigurationTextScroll = 0
        if tab == .mounts {
            containerConfigurationMountScroll = 0
        }
        switch tab {
        case .dockerfile:
            workspaceRenameName = containerConfigurationDockerfile
            focusWorkspaceRenameField()
        case .environment:
            workspaceRenameName = containerConfigurationEnvironment
            focusWorkspaceRenameField()
        case .ports:
            workspaceRenameName = containerConfigurationPorts
            focusWorkspaceRenameField()
        case .mounts, .runtime:
            blurWorkspaceRenameField()
        }
        updateLayout()
    }

    private var hasUnsavedContainerConfigurationDockerfile: Bool {
        containerConfigurationDockerfile != containerConfigurationSavedDockerfile
    }

    private var currentContainerConfigurationText: String {
        switch containerConfigurationTab {
        case .dockerfile:
            return containerConfigurationDockerfile
        case .environment:
            return containerConfigurationEnvironment
        case .ports:
            return containerConfigurationPorts
        case .mounts, .runtime:
            return ""
        }
    }

    private var isContainerConfigurationTextTab: Bool {
        containerConfigurationTab == .dockerfile ||
            containerConfigurationTab == .environment ||
            containerConfigurationTab == .ports
    }

    @discardableResult
    private func saveContainerConfigurationDockerfile() -> Bool {
        guard let workspace = pendingDockerfileWorkspace,
              !isPerformingWorkspaceOperation else {
            return false
        }
        if containerConfigurationTab == .dockerfile,
           workspaceRenameInputController.isFocused {
            containerConfigurationDockerfile = workspaceRenameInputController.text
        }
        let dockerfile = containerConfigurationDockerfile
        guard !dockerfile.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            workspacePanelMessage = "The Dockerfile cannot be empty."
            selectContainerConfigurationTab(.dockerfile)
            updateLayout()
            return false
        }
        guard dockerfile != containerConfigurationSavedDockerfile else { return false }
        pendingContainerConfigurationPreviousSavedDockerfile =
            containerConfigurationSavedDockerfile
        pendingContainerConfigurationPreviousRequiresRebuild =
            containerConfigurationRequiresRebuild
        pendingContainerConfigurationDockerfileSave = dockerfile
        containerConfigurationSavedDockerfile = dockerfile
        containerConfigurationRequiresRebuild = true
        sendWorkspaceRequest(operation: "updateDockerfile",
                             workspaceID: workspace.id,
                             command: dockerfile)
        return true
    }

    private func discardContainerConfigurationDockerfileChanges() {
        guard containerConfigurationTab == .dockerfile,
              !isPerformingWorkspaceOperation else { return }
        containerConfigurationDockerfile = containerConfigurationSavedDockerfile
        workspaceRenameName = containerConfigurationSavedDockerfile
        isSynchronizingWorkspaceRenameInput = true
        workspaceRenameInputController.setText(containerConfigurationSavedDockerfile)
        isSynchronizingWorkspaceRenameInput = false
        workspacePanelMessage = ""
        updateLayout()
    }

    private func restoreContainerConfigurationDockerfileSave() {
        if let previousDockerfile = pendingContainerConfigurationPreviousSavedDockerfile {
            containerConfigurationSavedDockerfile = previousDockerfile
            containerConfigurationRequiresRebuild =
                pendingContainerConfigurationPreviousRequiresRebuild ?? false
        }
        pendingContainerConfigurationDockerfileSave = nil
        pendingContainerConfigurationPreviousSavedDockerfile = nil
        pendingContainerConfigurationPreviousRequiresRebuild = nil
    }

    private func scheduleContainerConfigurationEnvironmentSave(
        after delay: TimeInterval = 0.7
    ) {
        containerConfigurationEnvironmentSaveGeneration += 1
        let generation = containerConfigurationEnvironmentSaveGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self,
                  self.containerConfigurationEnvironmentSaveGeneration == generation,
                  self.isContainerConfigurationEditorVisible else {
                return
            }
            if self.isPerformingWorkspaceOperation {
                self.scheduleContainerConfigurationEnvironmentSave(after: 0.35)
                return
            }
            self.persistContainerRuntimeConfiguration(reportErrors: false)
        }
    }

    private func persistContainerRuntimeConfiguration(reportErrors: Bool = true) {
        guard let workspace = pendingDockerfileWorkspace,
              !isPerformingWorkspaceOperation else {
            return
        }
        if containerConfigurationTab == .environment,
           workspaceRenameInputController.isFocused {
            containerConfigurationEnvironment = workspaceRenameInputController.text
        }
        if containerConfigurationTab == .ports,
           workspaceRenameInputController.isFocused {
            containerConfigurationPorts = workspaceRenameInputController.text
        }
        let environment: [ContainerConfigurationEnvironmentRequest]
        let publishedPorts: [ContainerConfigurationPublishedPortRequest]
        do {
            environment = try parsedContainerConfigurationEnvironment()
            publishedPorts = try parsedContainerConfigurationPublishedPorts()
        } catch {
            if reportErrors {
                workspacePanelMessage = error.localizedDescription
                updateLayout()
            }
            return
        }
        guard containerConfigurationEnvironment != containerConfigurationSavedEnvironment ||
                containerConfigurationPorts != containerConfigurationSavedPorts ||
                containerConfigurationMounts != containerConfigurationSavedMounts else {
            return
        }
        let mounts = containerConfigurationMounts.compactMap { mount in
            mount.isInfrastructure ? nil : ContainerConfigurationMountRequest(
                id: mount.id,
                name: mount.name,
                hostPath: mount.hostPath,
                guestPath: mount.guestPath,
                isReadOnly: mount.isReadOnly
            )
        }
        pendingContainerConfigurationRuntimeSave = (
            environment: containerConfigurationEnvironment,
            ports: containerConfigurationPorts,
            mounts: containerConfigurationMounts
        )
        sendWorkspaceRequest(operation: "updateContainerConfiguration",
                             workspaceID: workspace.id,
                             dockerfile: containerConfigurationSavedDockerfile,
                             mounts: mounts,
                             environment: environment,
                             publishedPorts: publishedPorts)
    }

    private func rebuildContainerConfiguration() {
        guard let workspace = pendingDockerfileWorkspace,
              !isPerformingWorkspaceOperation,
              containerConfigurationRebuildWorkspaceID == nil else {
            return
        }
        if hasUnsavedContainerConfigurationDockerfile {
            isConfirmingContainerConfigurationRebuild = true
            blurWorkspaceRenameField()
            updateLayout()
            return
        }
        containerConfigurationBuildError = nil
        containerConfigurationBuildErrorScroll = 0
        containerConfigurationBuildErrorRenderedScroll = 0
        containerConfigurationBuildErrorViewportLayer = nil
        containerConfigurationBuildErrorCopyConfirmationID = nil
        safeSpaceRecipeMessage = ""
        workspacePanelMessage = ""
        setDockerfileSelection(fragmentID: nil, range: nil)
        containerConfigurationRebuildWorkspaceID = workspace.id
        sendWorkspaceRequest(operation: "rebuildRecipe",
                             workspaceID: workspace.id)
    }

    private func dismissContainerConfigurationBuildError() {
        containerConfigurationBuildError = nil
        containerConfigurationBuildErrorScroll = 0
        containerConfigurationBuildErrorRenderedScroll = 0
        containerConfigurationBuildErrorContentHeight = 0
        containerConfigurationBuildErrorViewportLayer = nil
        containerConfigurationBuildErrorCopyConfirmationID = nil
        setDockerfileSelection(fragmentID: nil, range: nil)
        if isContainerConfigurationTextTab {
            workspaceRenameName = currentContainerConfigurationText
            focusWorkspaceRenameField()
        }
        updateLayout()
    }

    private func showContainerBuildErrorCopiedConfirmation() {
        let confirmationID = UUID()
        containerConfigurationBuildErrorCopyConfirmationID = confirmationID
        updateLayout()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self,
                  self.containerConfigurationBuildErrorCopyConfirmationID == confirmationID else {
                return
            }
            self.containerConfigurationBuildErrorCopyConfirmationID = nil
            self.updateLayout()
        }
    }

    private func dismissContainerConfigurationRebuildPrompt() {
        isConfirmingContainerConfigurationRebuild = false
        containerConfigurationRebuildAfterDockerfileSave = false
        if isContainerConfigurationTextTab {
            workspaceRenameName = currentContainerConfigurationText
            focusWorkspaceRenameField()
        }
        updateLayout()
    }

    private func saveContainerConfigurationDockerfileAndRebuild() {
        guard isConfirmingContainerConfigurationRebuild else { return }
        isConfirmingContainerConfigurationRebuild = false
        containerConfigurationRebuildAfterDockerfileSave = true
        workspaceRenameName = containerConfigurationDockerfile
        focusWorkspaceRenameField()
        saveContainerConfigurationDockerfile()
    }

    private func rebuildContainerConfigurationWithoutSavingDockerfile() {
        guard isConfirmingContainerConfigurationRebuild else { return }
        isConfirmingContainerConfigurationRebuild = false
        containerConfigurationRebuildAfterDockerfileSave = false
        containerConfigurationDockerfile = containerConfigurationSavedDockerfile
        workspaceRenameName = containerConfigurationSavedDockerfile
        isSynchronizingWorkspaceRenameInput = true
        workspaceRenameInputController.setText(containerConfigurationSavedDockerfile)
        isSynchronizingWorkspaceRenameInput = false
        rebuildContainerConfiguration()
    }

    private func requestContainerConfigurationDismissal() {
        guard isContainerConfigurationEditorVisible else { return }
        if containerConfigurationTab == .dockerfile,
           workspaceRenameInputController.isFocused {
            containerConfigurationDockerfile = workspaceRenameInputController.text
        }
        if pendingContainerConfigurationDockerfileSave != nil {
            containerConfigurationDismissAfterDockerfileSave = true
            blurWorkspaceRenameField()
            updateLayout()
            return
        }
        guard hasUnsavedContainerConfigurationDockerfile else {
            returnFromRecipeSafeSpace()
            return
        }
        isConfirmingContainerConfigurationDismissal = true
        blurWorkspaceRenameField()
        updateLayout()
    }

    private func dismissContainerConfigurationDismissalPrompt() {
        guard isConfirmingContainerConfigurationDismissal else { return }
        isConfirmingContainerConfigurationDismissal = false
        if isContainerConfigurationTextTab {
            workspaceRenameName = currentContainerConfigurationText
            focusWorkspaceRenameField()
        }
        updateLayout()
    }

    private func saveContainerConfigurationDockerfileAndDismiss() {
        guard isConfirmingContainerConfigurationDismissal else { return }
        isConfirmingContainerConfigurationDismissal = false
        containerConfigurationDismissAfterDockerfileSave = true
        if !saveContainerConfigurationDockerfile() {
            containerConfigurationDismissAfterDockerfileSave = false
            if !hasUnsavedContainerConfigurationDockerfile {
                returnFromRecipeSafeSpace()
            }
        }
    }

    private func dismissContainerConfigurationWithoutSavingDockerfile() {
        guard isConfirmingContainerConfigurationDismissal else { return }
        isConfirmingContainerConfigurationDismissal = false
        containerConfigurationDismissAfterDockerfileSave = false
        containerConfigurationDockerfile = containerConfigurationSavedDockerfile
        returnFromRecipeSafeSpace()
    }

    private func parsedContainerConfigurationEnvironment() throws
        -> [ContainerConfigurationEnvironmentRequest] {
        var values: [ContainerConfigurationEnvironmentRequest] = []
        var names = Set<String>()
        for (index, rawLine) in containerConfigurationEnvironment
            .components(separatedBy: .newlines).enumerated() {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty || rawLine.isEmpty else { continue }
            if line.isEmpty { continue }
            guard let separator = line.firstIndex(of: "=") else {
                throw ContainerConfigurationInputError(
                    message: "Environment line \(index + 1) needs NAME=value."
                )
            }
            let name = String(line[..<separator]).trimmingCharacters(in: .whitespaces)
            let value = String(line[line.index(after: separator)...])
            let validStart = name.unicodeScalars.first.map {
                CharacterSet.letters.union(CharacterSet(charactersIn: "_"))
                    .contains($0)
            } == true
            let validRest = name.unicodeScalars.dropFirst().allSatisfy {
                CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "_"))
                    .contains($0)
            }
            guard validStart, validRest, names.insert(name).inserted else {
                throw ContainerConfigurationInputError(
                    message: "Environment line \(index + 1) needs a unique shell variable name."
                )
            }
            values.append(ContainerConfigurationEnvironmentRequest(name: name,
                                                                    value: value))
        }
        return values
    }

    private func parsedContainerConfigurationPublishedPorts() throws
        -> [ContainerConfigurationPublishedPortRequest] {
        var values: [ContainerConfigurationPublishedPortRequest] = []
        var hostPorts = Set<Int>()
        for (index, rawLine) in containerConfigurationPorts
            .components(separatedBy: .newlines).enumerated() {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }
            let parts = line.split(separator: ":", omittingEmptySubsequences: false)
            guard parts.count == 2,
                  let hostPort = Int(parts[0].trimmingCharacters(in: .whitespaces)),
                  let containerPort = Int(parts[1].trimmingCharacters(in: .whitespaces)),
                  (1...65535).contains(hostPort),
                  (1...65535).contains(containerPort),
                  hostPorts.insert(hostPort).inserted else {
                throw ContainerConfigurationInputError(
                    message: "Published port line \(index + 1) needs a unique HOST:CONTAINER mapping, such as 4000:4000."
                )
            }
            values.append(ContainerConfigurationPublishedPortRequest(
                hostPort: hostPort,
                containerPort: containerPort
            ))
        }
        return values
    }

    private func appendSelectedContainerConfigurationFolder(_ path: String) {
        guard !path.isEmpty else { return }
        let url = URL(fileURLWithPath: path, isDirectory: true)
        let name = url.lastPathComponent.isEmpty ? "Folder" : url.lastPathComponent
        let base = name.lowercased()
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        let preferred = base.isEmpty ? "folder" : base
        let existing = Set(containerConfigurationMounts.map {
            URL(fileURLWithPath: $0.guestPath).lastPathComponent
        })
        var component = preferred
        var suffix = 2
        while existing.contains(component) {
            component = "\(preferred)-\(suffix)"
            suffix += 1
        }
        containerConfigurationMounts.append(
            ContainerConfigurationMountDraft(
                id: UUID(),
                name: name,
                hostPath: url.standardizedFileURL.path,
                guestPath: "/workspaces/mounts/\(component)",
                isReadOnly: false,
                isInfrastructure: false
            )
        )
    }

    private func openDockerfileInTextEditor(for workspace: LocalWorkspaceRecord) {
        guard let recipe = workspace.recipe else { return }
        let endpoint = plaintextEndpoint(for: "user") ?? plaintextEndpoint(for: "system")
        guard let endpoint,
              let url = plaintextURL(for: endpoint, filePath: recipe.dockerfileHostPath) else {
            backendError = "Install Plaintext to edit this Dockerfile in a separate tab."
            updateLayout()
            return
        }
        outerframeHost.openNewTab(with: url, displayString: "\(workspace.name) Dockerfile")
    }

    private func showRecipeBaseImagePrompt(for workspace: LocalWorkspaceRecord) {
        guard let recipe = workspace.recipe else { return }
        workspaceNamePromptDismissesPanel = true
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        pendingWorkspaceRename = nil
        pendingRecipeCommandWorkspace = nil
        pendingDockerfileWorkspace = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        pendingSafeSpaceAppWorkspace = nil
        pendingSafeSpaceAppKind = nil
        pendingRecipeBaseImageWorkspace = workspace
        let defaultBaseImage = outerShellBaseImage(for: workspace.runtime?.providerID)
        if recipe.baseImage == defaultBaseImage {
            baseImageTemplate = .outerShell
            creationBaseImage = "debian:bookworm"
        } else {
            baseImageTemplate = recipe.installsOuterShellSupport
                ? .customWithSupport
                : .customAsIs
            creationBaseImage = recipe.baseImage
        }
        workspaceRenameName = creationBaseImage
        workspacePanelMessage = ""
        if baseImageTemplate == .outerShell {
            blurWorkspaceRenameField()
        } else {
            focusWorkspaceRenameField(selectAll: true)
        }
        updateLayout()
    }

    private func outerShellBaseImage(for providerID: String? = nil) -> String {
        let selectedProviderID = providerID ?? selectedSafeSpaceProviderID
        return availableSafeSpaceProviders.first(where: {
            $0.id == selectedProviderID
        })?.defaultBaseImage ?? "outershell/container-base:6"
    }

    private var selectedBaseImage: String {
        baseImageTemplate == .outerShell ? outerShellBaseImage() : creationBaseImage
    }

    private func showRecipeUserPrompt(for workspace: LocalWorkspaceRecord) {
        workspaceNamePromptDismissesPanel = true
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        pendingWorkspaceRename = nil
        pendingRecipeCommandWorkspace = nil
        pendingDockerfileWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        pendingSafeSpaceAppWorkspace = nil
        pendingSafeSpaceAppKind = nil
        pendingRecipeUserWorkspace = workspace
        workspaceRenameName = ""
        workspacePanelMessage = ""
        focusWorkspaceRenameField()
        updateLayout()
    }

    private func showRecipeFragmentEditor(
        for workspace: LocalWorkspaceRecord,
        step: LocalWorkspaceRecord.Recipe.Step
    ) {
        guard step.isEditable else { return }
        workspaceNamePromptDismissesPanel = true
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        pendingWorkspaceRename = nil
        pendingRecipeCommandWorkspace = nil
        pendingDockerfileWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeEditWorkspace = workspace
        pendingRecipeEditStep = step
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        pendingSafeSpaceAppWorkspace = nil
        pendingSafeSpaceAppKind = nil
        workspaceRenameName = step.dockerfileFragment
        workspacePanelMessage = ""
        focusWorkspaceRenameField()
        updateLayout()
    }

    private func showRecipeScriptCreationPrompt(
        for workspace: LocalWorkspaceRecord,
        userID: UUID?
    ) {
        workspaceNamePromptDismissesPanel = true
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        pendingWorkspaceRename = nil
        pendingRecipeCommandWorkspace = nil
        pendingDockerfileWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        pendingSafeSpaceAppWorkspace = nil
        pendingSafeSpaceAppKind = nil
        pendingRecipeScriptWorkspace = workspace
        pendingRecipeScriptUserID = userID
        pendingRecipeScriptRename = nil
        workspaceRenameName = nextRecipeScriptName(
            in: workspace.recipe?.scriptFiles ?? [],
            userID: userID
        )
        workspacePanelMessage = ""
        focusWorkspaceRenameField(selectAll: true)
        updateLayout()
    }

    private func showRecipeScriptRenamePrompt(
        for workspace: LocalWorkspaceRecord,
        script: LocalWorkspaceRecord.Recipe.ScriptFile
    ) {
        showRecipeScriptCreationPrompt(
            for: workspace,
            userID: UUID(uuidString: script.userID)
        )
        pendingRecipeScriptRename = script
        workspaceRenameName = script.fileName
        focusWorkspaceRenameField(selectAll: true)
        updateLayout()
    }

    private func nextRecipeScriptName(
        in scripts: [LocalWorkspaceRecord.Recipe.ScriptFile],
        userID: UUID?
    ) -> String {
        let matching = scripts.filter { script in
            userID.map { $0.uuidString == script.userID } ?? script.userID.isEmpty
        }
        let prefixes = matching.compactMap { script -> Int? in
            let prefix = script.fileName.prefix { $0.isNumber }
            return Int(prefix)
        }
        let next = ((prefixes.max() ?? 0) / 10 + 1) * 10
        return String(format: "%03d-setup.sh", max(next, 10))
    }

    private func editRecipeScript(_ script: LocalWorkspaceRecord.Recipe.ScriptFile) {
        let endpoint = plaintextEndpoint(for: "user") ?? plaintextEndpoint(for: "system")
        guard let endpoint,
              let url = plaintextURL(for: endpoint, filePath: script.hostPath) else {
            backendError = "Install Plaintext to edit setup scripts outside the container."
            updateLayout()
            return
        }
        outerframeHost.openNewTab(with: url, displayString: script.fileName)
    }

    private func showSafeSpaceAppPrompt(for workspace: LocalWorkspaceRecord,
                                        kind: String) {
        workspaceNamePromptDismissesPanel = true
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        pendingWorkspaceRename = nil
        pendingRecipeCommandWorkspace = nil
        pendingDockerfileWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingSafeSpaceAppWorkspace = nil
        pendingSafeSpaceAppKind = nil
        pendingSafeSpaceAppWorkspace = workspace
        pendingSafeSpaceAppKind = kind
        workspaceRenameName = "/home/workspace/Workspace"
        workspacePanelMessage = ""
        focusWorkspaceRenameField(selectAll: true)
        updateLayout()
    }

    private func addSafeSpaceApp(to workspace: LocalWorkspaceRecord,
                                 kind: String,
                                 workingDirectory: String) {
        sendWorkspaceRequest(operation: "addRecipeApp",
                             workspaceID: workspace.id,
                             launcherKind: kind,
                             workingDirectory: workingDirectory)
    }

    private func showWorkspaceRename(_ workspace: LocalWorkspaceRecord) {
        isShowingWorkspacePanel = true
        isCreatingWorkspace = false
        workspaceNamePromptDismissesPanel = false
        pendingWorkspaceRename = workspace
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        workspaceRenameName = workspace.name
        workspacePanelMessage = ""
        focusWorkspaceRenameField(selectAll: true)
        updateLayout()
    }

    private func showContainerConfigurationRename(
        _ workspace: LocalWorkspaceRecord
    ) {
        guard isContainerConfigurationEditorVisible else { return }
        if workspaceRenameInputController.isFocused {
            switch containerConfigurationTab {
            case .dockerfile:
                containerConfigurationDockerfile = workspaceRenameInputController.text
            case .environment:
                containerConfigurationEnvironment = workspaceRenameInputController.text
            case .ports:
                containerConfigurationPorts = workspaceRenameInputController.text
            case .mounts, .runtime:
                break
            }
        }
        blurWorkspaceRenameField()
        workspaceNamePromptDismissesPanel = false
        pendingWorkspaceRename = workspace
        workspaceRenameName = workspace.name
        workspacePanelMessage = ""
        focusWorkspaceRenameField(selectAll: true)
        updateLayout()
    }

    private func restoreContainerConfigurationInputAfterRename() {
        pendingWorkspaceRename = nil
        workspaceNamePromptDismissesPanel = true
        workspacePanelMessage = ""
        containerConfigurationTextScroll = 0
        switch containerConfigurationTab {
        case .dockerfile:
            workspaceRenameName = containerConfigurationDockerfile
            focusWorkspaceRenameField()
        case .environment:
            workspaceRenameName = containerConfigurationEnvironment
            focusWorkspaceRenameField()
        case .ports:
            workspaceRenameName = containerConfigurationPorts
            focusWorkspaceRenameField()
        case .mounts, .runtime:
            workspaceRenameName = ""
            blurWorkspaceRenameField()
        }
    }

    private func showWorkspaceDeletionConfirmation(_ workspace: LocalWorkspaceRecord) {
        pendingWorkspaceDeletion = workspace
        isShowingWorkspacePanel = true
        workspacePanelMessage = ""
        updateLayout()
    }

    private func dismissWorkspaceDeletion() {
        pendingWorkspaceDeletion = nil
        isShowingWorkspacePanel = false
        workspacePanelMessage = ""
        updateLayout()
    }

    private func submitWorkspaceDeletion() {
        guard let workspace = pendingWorkspaceDeletion,
              !isPerformingWorkspaceOperation else {
            return
        }
        pendingWorkspaceDeletion = nil
        isShowingWorkspacePanel = false
        sendWorkspaceRequest(operation: "delete", workspaceID: workspace.id)
    }

    private func dismissWorkspaceRename() {
        if let pendingImport = pendingSharedContainerImport {
            guard !isPerformingWorkspaceOperation else { return }
            pendingSharedContainerImport = nil
            blurWorkspaceRenameField()
            workspaceNamePromptDismissesPanel = false
            workspaceRenameName = ""
            workspacePanelMessage = ""
            isShowingWorkspacePanel = false
            performContainerTransferRequest(
                operation: "cancelImport",
                values: ["transferID": pendingImport.transferID]
            ) { _ in }
            updateLayout()
            return
        }
        if isRenamingContainerConfiguration {
            blurWorkspaceRenameField()
            restoreContainerConfigurationInputAfterRename()
            updateLayout()
            return
        }
        if isCreatingWorkspace && isEditingCreationBaseImage {
            blurWorkspaceRenameField()
            isEditingCreationBaseImage = false
            workspaceRenameName = pendingCreationContainerName
            workspacePanelMessage = ""
            focusWorkspaceRenameField(selectAll: true)
            updateLayout()
            return
        }
        let dismissesPanel = workspaceNamePromptDismissesPanel
        blurWorkspaceRenameField()
        pendingWorkspaceRename = nil
        pendingRecipeCommandWorkspace = nil
        pendingDockerfileWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        isCreatingWorkspace = false
        workspaceNamePromptDismissesPanel = false
        workspaceRenameName = ""
        pendingCreationContainerName = ""
        isEditingCreationBaseImage = false
        if dismissesPanel {
            isShowingWorkspacePanel = false
        }
        updateLayout()
    }

    private func submitWorkspaceRename() {
        guard isWorkspaceNamePromptVisible else { return }
        guard !isPerformingWorkspaceOperation else {
            workspacePanelMessage = "Another container operation is still finishing. Your changes have not been discarded."
            updateLayout()
            return
        }
        let submittedText = workspaceRenameInputController.text
        let name = submittedText.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        guard !name.isEmpty else {
            if pendingSharedContainerImport != nil {
                workspacePanelMessage = "Choose a destination folder on this server."
            } else if pendingWorkspaceRename != nil {
                workspacePanelMessage = "Enter a name for the container."
            } else if pendingSafeSpaceAppWorkspace != nil {
                workspacePanelMessage = "Enter a folder path inside the container."
            } else if pendingDockerfileWorkspace != nil {
                workspacePanelMessage = "The Dockerfile cannot be empty."
            } else if pendingRecipeCommandWorkspace != nil {
                workspacePanelMessage = "Enter a Dockerfile fragment."
            } else if pendingRecipeBaseImageWorkspace != nil {
                workspacePanelMessage = "Enter an OCI base-image reference."
            } else if pendingRecipeUserWorkspace != nil {
                workspacePanelMessage = "Enter a Linux username."
            } else if pendingRecipeScriptWorkspace != nil {
                workspacePanelMessage = "Enter a setup-script filename."
            } else if pendingRecipeEditStep != nil {
                workspacePanelMessage = "A Dockerfile fragment cannot be empty."
            } else {
                workspacePanelMessage = "Enter a name for the container."
            }
            updateLayout()
            return
        }
        if let pendingImport = pendingSharedContainerImport {
            isPerformingWorkspaceOperation = true
            workspacePanelMessage = "Copying mounted folders…"
            blurWorkspaceRenameField()
            updateLayout()
            finishUploadedSharedContainer(
                transferID: pendingImport.transferID,
                runtimeProviderID: pendingImport.runtimeProviderID,
                mountDestinationRoot: name
            )
            return
        }
        if isCreatingWorkspace && isEditingCreationBaseImage {
            if baseImageTemplate != .outerShell {
                creationBaseImage = name
            }
            isEditingCreationBaseImage = false
            workspaceRenameName = pendingCreationContainerName
            workspacePanelMessage = ""
            focusWorkspaceRenameField(selectAll: true)
            updateLayout()
            return
        }
        let workspace = pendingWorkspaceRename
        let dockerfileWorkspace = pendingDockerfileWorkspace
        let recipeWorkspace = pendingRecipeCommandWorkspace
        let recipeBaseImageWorkspace = pendingRecipeBaseImageWorkspace
        let recipeUserWorkspace = pendingRecipeUserWorkspace
        let recipeEditWorkspace = pendingRecipeEditWorkspace
        let recipeEditStep = pendingRecipeEditStep
        let recipeScriptWorkspace = pendingRecipeScriptWorkspace
        let recipeScriptUserID = pendingRecipeScriptUserID
        let recipeScriptRename = pendingRecipeScriptRename
        let appWorkspace = pendingSafeSpaceAppWorkspace
        let appKind = pendingSafeSpaceAppKind
        let createsWorkspace = isCreatingWorkspace
        let dismissesPanel = workspaceNamePromptDismissesPanel
        let renamesContainerConfiguration = isRenamingContainerConfiguration
        blurWorkspaceRenameField()
        pendingWorkspaceRename = nil
        if !renamesContainerConfiguration {
            pendingDockerfileWorkspace = nil
        }
        pendingRecipeCommandWorkspace = nil
        pendingRecipeBaseImageWorkspace = nil
        pendingRecipeUserWorkspace = nil
        pendingRecipeEditWorkspace = nil
        pendingRecipeEditStep = nil
        pendingRecipeScriptWorkspace = nil
        pendingRecipeScriptUserID = nil
        pendingRecipeScriptRename = nil
        pendingSafeSpaceAppWorkspace = nil
        pendingSafeSpaceAppKind = nil
        isCreatingWorkspace = false
        workspaceNamePromptDismissesPanel = false
        workspaceRenameName = ""
        pendingCreationContainerName = ""
        isEditingCreationBaseImage = false
        if dismissesPanel && !renamesContainerConfiguration {
            isShowingWorkspacePanel = false
        }
        if renamesContainerConfiguration, let workspace {
            restoreContainerConfigurationInputAfterRename()
            sendWorkspaceRequest(operation: "rename",
                                 workspaceID: workspace.id,
                                 name: name)
        } else if let dockerfileWorkspace {
            sendWorkspaceRequest(operation: "updateDockerfile",
                                 workspaceID: dockerfileWorkspace.id,
                                 command: submittedText)
        } else if let appWorkspace, let appKind {
            addSafeSpaceApp(to: appWorkspace,
                            kind: appKind,
                            workingDirectory: name)
        } else if let recipeWorkspace {
            sendWorkspaceRequest(operation: "addRecipeStep",
                                 workspaceID: recipeWorkspace.id,
                                 command: name)
        } else if let recipeBaseImageWorkspace {
            sendWorkspaceRequest(operation: "updateRecipeBaseImage",
                                 workspaceID: recipeBaseImageWorkspace.id,
                                 baseImage: baseImageTemplate == .outerShell
                                    ? outerShellBaseImage(
                                        for: recipeBaseImageWorkspace.runtime?.providerID
                                    )
                                    : name,
                                 installsOuterShellSupport: baseImageTemplate == .customWithSupport)
        } else if let recipeUserWorkspace {
            sendWorkspaceRequest(operation: "addRecipeUser",
                                 workspaceID: recipeUserWorkspace.id,
                                 name: name)
        } else if let recipeEditWorkspace, let recipeEditStep {
            sendWorkspaceRequest(operation: "updateRecipeStep",
                                 workspaceID: recipeEditWorkspace.id,
                                 recipeStepID: recipeEditStep.id,
                                 command: name)
        } else if let recipeScriptWorkspace, let recipeScriptRename {
            sendWorkspaceRequest(operation: "renameRecipeScript",
                                 workspaceID: recipeScriptWorkspace.id,
                                 name: name,
                                 recipeScriptPath: recipeScriptRename.relativePath)
        } else if let recipeScriptWorkspace {
            sendWorkspaceRequest(operation: "createRecipeScript",
                                 workspaceID: recipeScriptWorkspace.id,
                                 name: name,
                                 recipeUserID: recipeScriptUserID)
        } else if createsWorkspace {
            sendWorkspaceRequest(operation: "create",
                                 name: name,
                                 baseImage: selectedBaseImage,
                                 installsOuterShellSupport: baseImageTemplate == .customWithSupport)
        } else if let workspace {
            sendWorkspaceRequest(operation: "rename",
                                 workspaceID: workspace.id,
                                 name: name)
        }
    }

    private func renderSharedContainerPanel(_ file: (url: URL, name: String, byteCount: Int),
                                            width: CGFloat,
                                            height: CGFloat) {
        workspaceOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.24))
        let panelWidth = min(max(width - 64, 420), 560)
        let panelHeight: CGFloat = 240
        workspacePanelFrame = CGRect(
            x: floor((width - panelWidth) / 2),
            y: floor((height - panelHeight) / 2),
            width: panelWidth,
            height: panelHeight
        )
        workspacePanelLayer.frame = workspacePanelFrame
        workspacePanelLayer.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        workspacePanelLayer.cornerRadius = 14
        workspacePanelLayer.borderWidth = 0.5
        workspacePanelLayer.borderColor = resolvedCGColor(.separatorColor)
        workspacePanelLayer.shadowColor = resolvedCGColor(.black)
        workspacePanelLayer.shadowOpacity = 0.18
        workspacePanelLayer.shadowRadius = 18
        workspacePanelLayer.shadowOffset = CGSize(width: 0, height: -4)
        workspacePanelLayer.sublayers = nil

        let title = makeTextLayer(size: 20, weight: .semibold, color: .labelColor)
        title.string = "Share Container"
        title.frame = CGRect(x: 24, y: panelHeight - 48, width: panelWidth - 76, height: 26)
        workspacePanelLayer.addSublayer(title)

        let close = makeSymbolButtonLayer(
            symbolName: "xmark.circle.fill",
            accessibilityTitle: "Close Share Container"
        )
        let closeFrame = CGRect(x: panelWidth - 42, y: panelHeight - 43, width: 24, height: 24)
        close.frame = closeFrame
        sharedContainerCloseFrame = closeFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )
        workspacePanelLayer.addSublayer(close)

        let explanation = makeTextLayer(size: 13, weight: .regular, color: .secondaryLabelColor)
        explanation.string = "Drag this file into another Outer Shell window to create a portable copy."
        explanation.frame = CGRect(x: 24, y: panelHeight - 76, width: panelWidth - 48, height: 20)
        workspacePanelLayer.addSublayer(explanation)

        let localFileFrame = CGRect(x: 24, y: 28, width: panelWidth - 48, height: 102)
        let card = CALayer()
        card.frame = localFileFrame
        card.backgroundColor = resolvedCGColor(.controlBackgroundColor)
        card.cornerRadius = 10
        card.borderWidth = 1
        card.borderColor = resolvedCGColor(.separatorColor)
        workspacePanelLayer.addSublayer(card)

        let icon = makeSymbolButtonLayer(
            symbolName: "shippingbox.and.arrow.backward",
            accessibilityTitle: "Shared container file"
        )
        icon.applyStyle(
            tintCGColor: resolvedCGColor(.controlAccentColor),
            backgroundCGColor: resolvedCGColor(.clear)
        )
        icon.frame = CGRect(x: 20, y: 28, width: 42, height: 42)
        card.addSublayer(icon)

        let name = makeTextLayer(size: 14, weight: .medium, color: .labelColor)
        name.string = file.name
        name.frame = CGRect(x: 76, y: 52, width: localFileFrame.width - 96, height: 22)
        card.addSublayer(name)

        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        let detail = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        detail.string = "\(formatter.string(fromByteCount: Int64(file.byteCount))) · Drag to share"
        detail.frame = CGRect(x: 76, y: 28, width: localFileFrame.width - 96, height: 20)
        card.addSublayer(detail)

        sharedContainerDragFrame = localFileFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )
        workspaceCloseFrame = .zero
    }

    private func renderWorkspacePanelIfNeeded(width: CGFloat, height: CGFloat) {
        workspaceOverlayLayer.isHidden = !isShowingWorkspacePanel
        guard isShowingWorkspacePanel else {
            workspacePanelLayer.sublayers = nil
            workspacePanelFrame = .zero
            workspaceCloseFrame = .zero
            workspaceRenamePanelFrame = .zero
            workspaceRenameFieldFrame = .zero
            workspaceRenameTextFrame = .zero
            workspaceRenameCancelFrame = .zero
            workspaceRenameConfirmFrame = .zero
            workspaceCreationBaseImageFrame = .zero
            workspaceOuterShellBaseImageFrame = .zero
            workspaceCustomBaseImageFrame = .zero
            workspaceCustomAsIsFrame = .zero
            workspaceDeletePanelFrame = .zero
            workspaceDeleteCancelFrame = .zero
            workspaceDeleteConfirmFrame = .zero
            sharedContainerDragFrame = .zero
            sharedContainerCloseFrame = .zero
            return
        }

        if let sharedContainerFile {
            renderSharedContainerPanel(sharedContainerFile, width: width, height: height)
            return
        }

        if isContainerConfigurationEditorVisible {
            renderContainerConfigurationPanel(width: width, height: height)
            if isRenamingContainerConfiguration {
                renderWorkspaceRenamePromptIfNeeded(
                    panelWidth: workspacePanelFrame.width,
                    panelHeight: workspacePanelFrame.height
                )
            }
            return
        }

        workspaceOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.24))
        let showsStandaloneNamePrompt = isWorkspaceNamePromptVisible
        let showsDeletePrompt = pendingWorkspaceDeletion != nil
        guard showsStandaloneNamePrompt || showsDeletePrompt else {
            isShowingWorkspacePanel = false
            workspaceOverlayLayer.isHidden = true
            workspacePanelLayer.sublayers = nil
            workspacePanelFrame = .zero
            return
        }
        let panelWidth = pendingSharedContainerImport != nil
                ? min(max(width - 72, 460), 680)
                : (isDockerfileFragmentPrompt
                ? min(max(width - 72, 520), 760)
                : min(max(width - 72, 320), 460))
        let panelHeight = pendingSharedContainerImport != nil
                ? min(
                    CGFloat(224 + min(pendingSharedContainerImport?.mounts.count ?? 0, 5) * 50),
                    max(height - 72, 274)
                )
                : (isDockerfileFragmentPrompt
                ? min(max(height - 72, 360), 600)
                : CGFloat(showsDeletePrompt
                    ? 184
                    : (isBaseImageChoicePrompt
                        ? 310
                        : (isCreatingWorkspace && !isEditingCreationBaseImage ? 224 : 174))))
        workspacePanelFrame = CGRect(x: floor((width - panelWidth) / 2),
                                     y: floor((height - panelHeight) / 2),
                                     width: panelWidth,
                                     height: panelHeight)
        workspacePanelLayer.frame = workspacePanelFrame
        workspacePanelLayer.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        workspacePanelLayer.cornerRadius = 14
        workspacePanelLayer.borderWidth = 0.5
        workspacePanelLayer.borderColor = resolvedCGColor(.separatorColor)
        workspacePanelLayer.shadowColor = resolvedCGColor(.black)
        workspacePanelLayer.shadowOpacity = 0.18
        workspacePanelLayer.shadowRadius = 18
        workspacePanelLayer.shadowOffset = CGSize(width: 0, height: -4)
        workspacePanelLayer.sublayers = nil

        if showsDeletePrompt {
            renderWorkspaceDeletionPrompt(panelWidth: panelWidth,
                                          panelHeight: panelHeight)
            return
        }

        if showsStandaloneNamePrompt {
            renderWorkspaceRenamePromptIfNeeded(panelWidth: panelWidth,
                                                panelHeight: panelHeight)
            return
        }

    }

    private func renderContainerConfigurationPanel(width: CGFloat, height: CGFloat) {
        guard let workspace = pendingDockerfileWorkspace else { return }
        workspaceOverlayLayer.backgroundColor = resolvedCGColor(
            NSColor.black.withAlphaComponent(0.28)
        )
        let panelWidth = max(width - 48, 620)
        let panelHeight = max(height - 48, 420)
        workspacePanelFrame = CGRect(x: floor((width - panelWidth) / 2),
                                     y: floor((height - panelHeight) / 2),
                                     width: panelWidth,
                                     height: panelHeight)
        workspaceRenamePanelFrame = workspacePanelFrame
        workspacePanelLayer.frame = workspacePanelFrame
        workspacePanelLayer.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        workspacePanelLayer.cornerRadius = 14
        workspacePanelLayer.borderWidth = 0.5
        workspacePanelLayer.borderColor = resolvedCGColor(.separatorColor)
        workspacePanelLayer.shadowColor = resolvedCGColor(.black)
        workspacePanelLayer.shadowOpacity = 0.2
        workspacePanelLayer.shadowRadius = 20
        workspacePanelLayer.shadowOffset = CGSize(width: 0, height: -4)
        workspacePanelLayer.sublayers = nil
        containerConfigurationTextViewportLayer = nil
        containerConfigurationBuildErrorViewportLayer = nil
        containerConfigurationTextSelectionLayer = nil
        containerConfigurationTextFragments = []
        containerConfigurationTextFragmentsText = ""
        containerConfigurationTextFragmentsWidth = 0
        containerConfigurationMountActionFrames.removeAll()
        containerConfigurationSaveFrame = .zero
        containerConfigurationDiscardFrame = .zero
        containerConfigurationRebuildFrame = .zero
        containerConfigurationRebuildPromptFrame = .zero
        containerConfigurationRebuildSaveFrame = .zero
        containerConfigurationRebuildWithoutSavingFrame = .zero
        containerConfigurationRebuildCancelFrame = .zero
        containerConfigurationBuildErrorTextFrame = .zero
        containerConfigurationBuildErrorCopyFrame = .zero
        containerConfigurationBuildErrorDismissFrame = .zero
        containerConfigurationDismissPromptFrame = .zero
        containerConfigurationDismissSaveFrame = .zero
        containerConfigurationDismissWithoutSavingFrame = .zero
        containerConfigurationDismissCancelFrame = .zero
        containerConfigurationRenameFrame = .zero
        containerConfigurationPortsTabFrame = .zero
        containerConfigurationRuntimeTabFrame = .zero
        containerConfigurationChangeRuntimeFrame = .zero
        workspaceRenameConfirmFrame = .zero
        workspaceRenameCancelFrame = .zero

        let title = makeTextLayer(size: 20, weight: .semibold, color: .labelColor)
        title.string = workspace.name
        title.frame = CGRect(x: 24,
                             y: panelHeight - 46,
                             width: panelWidth - 170,
                             height: 26)
        workspacePanelLayer.addSublayer(title)

        let rename = makeButtonLayer(title: "Rename", emphasized: false)
        let localRenameFrame = CGRect(x: panelWidth - 128,
                                      y: panelHeight - 45,
                                      width: 76,
                                      height: 28)
        rename.frame = localRenameFrame
        workspacePanelLayer.addSublayer(rename)
        containerConfigurationRenameFrame = localRenameFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )

        let close = makeSymbolButtonLayer(
            symbolName: "x.circle",
            accessibilityTitle: "Close container editor"
        )
        let localCloseFrame = CGRect(x: panelWidth - 42,
                                     y: panelHeight - 43,
                                     width: 24,
                                     height: 24)
        close.frame = localCloseFrame
        workspacePanelLayer.addSublayer(close)
        workspaceCloseFrame = localCloseFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )

        let tabs: [(ContainerConfigurationTab, String)] = [
            (.dockerfile, "Dockerfile"),
            (.mounts, "Folder Mounts"),
            (.environment, "Environment"),
            (.ports, "Ports"),
            (.runtime, "Runtime")
        ]
        var tabX: CGFloat = 24
        for (tab, label) in tabs {
            let tabWidth: CGFloat = label == "Folder Mounts" ? 126 : 108
            let localFrame = CGRect(x: tabX,
                                    y: panelHeight - 94,
                                    width: tabWidth,
                                    height: 32)
            let tabLayer = makeButtonLayer(
                title: label,
                emphasized: containerConfigurationTab == tab
            )
            tabLayer.frame = localFrame
            workspacePanelLayer.addSublayer(tabLayer)
            let rootFrame = localFrame.offsetBy(dx: workspacePanelFrame.minX,
                                                 dy: workspacePanelFrame.minY)
            switch tab {
            case .dockerfile:
                containerConfigurationDockerfileTabFrame = rootFrame
            case .mounts:
                containerConfigurationMountsTabFrame = rootFrame
            case .environment:
                containerConfigurationEnvironmentTabFrame = rootFrame
            case .ports:
                containerConfigurationPortsTabFrame = rootFrame
            case .runtime:
                containerConfigurationRuntimeTabFrame = rootFrame
            }
            tabX += tabWidth + 8
        }

        containerConfigurationEditorToolbarFrame = .zero
        containerConfigurationCookbookFrame = .zero
        containerConfigurationCopyPathFrame = .zero
        let contentTop = panelHeight - 108
        let contentFrame = CGRect(x: 24,
                                  y: 76,
                                  width: panelWidth - 48,
                                  height: max(contentTop - 76, 150))
        containerConfigurationContentFrame = contentFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )
        switch containerConfigurationTab {
        case .dockerfile:
            let activeBuildProgress = isActiveContainerBuildProgress(workspace.buildProgress)
                ? workspace.buildProgress
                : nil
            let progressHeight: CGFloat = activeBuildProgress == nil ? 0 : 126
            let editorFrame = CGRect(
                x: contentFrame.minX,
                y: contentFrame.minY + progressHeight,
                width: contentFrame.width,
                height: max(contentFrame.height - progressHeight, 100)
            )
            renderContainerConfigurationTextEditor(
                in: editorFrame,
                placeholder: "FROM debian:bookworm"
            )
            if let progress = activeBuildProgress {
                renderContainerBuildProgress(
                    progress,
                    in: CGRect(
                        x: contentFrame.minX,
                        y: contentFrame.minY,
                        width: contentFrame.width,
                        height: progressHeight - 8
                    )
                )
            }
        case .mounts:
            renderContainerConfigurationMounts(in: contentFrame)
        case .environment:
            renderContainerConfigurationTextEditor(
                in: contentFrame,
                placeholder: "EXAMPLE=value"
            )
        case .ports:
            renderContainerConfigurationTextEditor(
                in: contentFrame,
                placeholder: "4000:4000"
            )
        case .runtime:
            renderContainerConfigurationRuntime(workspace, in: contentFrame)
        }

        let divider = CALayer()
        divider.frame = CGRect(x: 0, y: 62, width: panelWidth, height: 0.5)
        divider.backgroundColor = resolvedCGColor(.separatorColor)
        workspacePanelLayer.addSublayer(divider)

        var buttonX = panelWidth - 24
        let rebuildWidth: CGFloat = 88
        buttonX -= rebuildWidth
        let localRebuildFrame = CGRect(x: buttonX,
                                       y: 17,
                                       width: rebuildWidth,
                                       height: 32)
        let rebuild = makeButtonLayer(
            title: "Rebuild",
            emphasized: containerConfigurationRequiresRebuild ||
                containerConfigurationEnvironment != containerConfigurationSavedEnvironment ||
                containerConfigurationPorts != containerConfigurationSavedPorts ||
                containerConfigurationMounts != containerConfigurationSavedMounts
        )
        rebuild.frame = localRebuildFrame
        rebuild.opacity = isPerformingWorkspaceOperation ? 0.55 : 1
        workspacePanelLayer.addSublayer(rebuild)
        containerConfigurationRebuildFrame = localRebuildFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )

        if isActiveContainerBuildProgress(workspace.buildProgress) ||
            !workspacePanelMessage.isEmpty || !safeSpaceRecipeMessage.isEmpty {
            let value = workspace.buildProgress?.detail ?? (
                workspacePanelMessage.isEmpty
                    ? safeSpaceRecipeMessage
                    : workspacePanelMessage
            )
            let message = makeTextLayer(
                size: 10,
                weight: .medium,
                color: value.lowercased().contains("error") ||
                    value.lowercased().contains("failed")
                    ? .systemRed
                    : .secondaryLabelColor
            )
            message.string = value
            message.truncationMode = .end
            message.frame = CGRect(x: 24,
                                   y: 25,
                                   width: max(buttonX - 40, 1),
                                   height: 16)
            workspacePanelLayer.addSublayer(message)
        }
        if containerConfigurationBuildError != nil {
            renderContainerConfigurationBuildError(panelWidth: panelWidth,
                                                   panelHeight: panelHeight)
        } else if isConfirmingContainerConfigurationDismissal {
            renderContainerConfigurationDismissalPrompt(panelWidth: panelWidth,
                                                         panelHeight: panelHeight)
        } else if isConfirmingContainerConfigurationRebuild {
            renderContainerConfigurationRebuildPrompt(panelWidth: panelWidth,
                                                       panelHeight: panelHeight)
        }
        sendWorkspaceRenameTextInputGeometryUpdate()
    }

    private func renderContainerConfigurationTextEditor(
        in contentFrame: CGRect,
        placeholder: String
    ) {
        let toolbarHeight: CGFloat = containerConfigurationTab == .dockerfile ? 36 : 0
        let localFieldFrame = CGRect(x: contentFrame.minX,
                                     y: contentFrame.minY,
                                     width: contentFrame.width,
                                     height: max(contentFrame.height, 100))
        let field = CALayer()
        field.frame = localFieldFrame
        field.cornerRadius = 7
        let isEditingField = workspaceRenameInputController.isFocused &&
            !isRenamingContainerConfiguration
        field.borderWidth = isEditingField ? 1.5 : 1
        field.borderColor = resolvedCGColor(
            isEditingField
                ? .controlAccentColor
                : .separatorColor
        )
        field.backgroundColor = resolvedCGColor(.textBackgroundColor)
        field.masksToBounds = true
        workspacePanelLayer.addSublayer(field)

        let textClip = CALayer()
        textClip.frame = CGRect(x: 0,
                                y: 0,
                                width: field.bounds.width,
                                height: max(field.bounds.height - toolbarHeight, 1))
        textClip.masksToBounds = true
        field.addSublayer(textClip)

        let viewport = CALayer()
        viewport.frame = textClip.bounds
        textClip.addSublayer(viewport)
        containerConfigurationTextViewportLayer = viewport
        containerConfigurationRenderedTextScroll = containerConfigurationTextScroll

        workspaceRenameFieldFrame = localFieldFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )
        let visibleTextFrame = CGRect(
            x: workspaceRenameFieldFrame.minX + 12,
            y: workspaceRenameFieldFrame.minY + 10,
            width: max(workspaceRenameFieldFrame.width - 24, 1),
            height: max(workspaceRenameFieldFrame.height - 20 - toolbarHeight, 1)
        )
        containerConfigurationTextVisibleFrame = visibleTextFrame
        workspaceRenameTextFrame = visibleTextFrame.offsetBy(
            dx: 0,
            dy: containerConfigurationTextScroll
        )
        workspaceRenameInputController.visualLineWidth = visibleTextFrame.width

        let text: String
        if isRenamingContainerConfiguration {
            text = currentContainerConfigurationText
        } else {
            text = workspaceRenameInputController.isFocused
                ? workspaceRenameInputController.text
                : workspaceRenameName
        }
        let layout = CreateFieldLayout(fieldFrame: workspaceRenameFieldFrame,
                                       textFrame: workspaceRenameTextFrame,
                                       key: "containerConfiguration",
                                       monospaced: true,
                                       multiline: true)
        let fragments = createTextAreaLineFragments(text: text, layout: layout)
        containerConfigurationTextFragments = fragments
        containerConfigurationTextFragmentsText = text
        containerConfigurationTextFragmentsWidth = visibleTextFrame.width
        if text.isEmpty {
            let placeholderLayer = makeTextLayer(size: 12,
                                                 weight: .regular,
                                                 color: .placeholderTextColor,
                                                 monospaced: true)
            placeholderLayer.string = placeholder
            placeholderLayer.frame = CGRect(x: 12,
                                             y: visibleTextFrame.maxY -
                                                workspaceRenameFieldFrame.minY - 17,
                                             width: localFieldFrame.width - 24,
                                             height: 16)
            viewport.addSublayer(placeholderLayer)
        }
        let selectionLayer = CALayer()
        selectionLayer.frame = viewport.bounds
        viewport.addSublayer(selectionLayer)
        containerConfigurationTextSelectionLayer = selectionLayer
        updateContainerConfigurationSelectionLayers()
        let buildSourceRange = pendingDockerfileWorkspace?.buildProgress.flatMap {
            dockerfileSourceCharacterRange(for: $0, in: text)
        }
        for fragment in fragments {
            let localY = fragment.y - workspaceRenameFieldFrame.minY
            guard localY > -localFieldFrame.height,
                  localY < localFieldFrame.height * 2 else {
                continue
            }
            let line = makeTextLayer(size: 12,
                                     weight: .regular,
                                     color: .textColor,
                                     monospaced: true)
            if let buildSourceRange,
               fragment.end > buildSourceRange.lowerBound,
               fragment.start < buildSourceRange.upperBound {
                let highlight = CALayer()
                highlight.frame = CGRect(
                    x: 7,
                    y: localY - 1,
                    width: max(visibleTextFrame.width + 10, 1),
                    height: 18
                )
                highlight.backgroundColor = resolvedCGColor(
                    NSColor.controlAccentColor.withAlphaComponent(0.13)
                )
                highlight.cornerRadius = 3
                viewport.insertSublayer(highlight, below: selectionLayer)
            }
            line.string = fragment.text
            line.frame = CGRect(x: 12,
                                y: localY,
                                width: visibleTextFrame.width,
                                height: 16)
            viewport.addSublayer(line)
        }
        if !isRenamingContainerConfiguration,
           let cursorFrame = workspaceRenameFieldCursorRect() {
            addBlinkingTextCaret(
                to: viewport,
                frame: cursorFrame.offsetBy(dx: -workspaceRenameFieldFrame.minX,
                                            dy: -workspaceRenameFieldFrame.minY)
            )
        }

        if toolbarHeight > 0 {
            let toolbar = CALayer()
            toolbar.frame = CGRect(x: 0,
                                   y: field.bounds.height - toolbarHeight,
                                   width: field.bounds.width,
                                   height: toolbarHeight)
            toolbar.backgroundColor = resolvedCGColor(.controlBackgroundColor)
            field.addSublayer(toolbar)

            let divider = CALayer()
            divider.frame = CGRect(x: 0,
                                   y: 0,
                                   width: toolbar.bounds.width,
                                   height: 0.5)
            divider.backgroundColor = resolvedCGColor(.separatorColor)
            toolbar.addSublayer(divider)

            if hasUnsavedContainerConfigurationDockerfile {
                var fileActionX: CGFloat = 6
                let saveActionWidth: CGFloat = 62
                let saveIcon = makeSymbolButtonLayer(
                    symbolName: "checkmark",
                    accessibilityTitle: "Save Dockerfile"
                )
                saveIcon.applyStyle(tintCGColor: resolvedCGColor(.controlAccentColor),
                                    backgroundCGColor: resolvedCGColor(.clear))
                saveIcon.frame = CGRect(x: fileActionX, y: 5, width: 26, height: 26)
                toolbar.addSublayer(saveIcon)

                let saveLabel = makeTextLayer(size: 11,
                                              weight: .medium,
                                              color: .controlAccentColor)
                saveLabel.string = "Save"
                saveLabel.frame = CGRect(x: fileActionX + 27, y: 10,
                                         width: 34, height: 16)
                toolbar.addSublayer(saveLabel)
                let localSaveFrame = CGRect(x: fileActionX, y: 2,
                                            width: saveActionWidth, height: 32)
                containerConfigurationSaveFrame = localSaveFrame.offsetBy(
                    dx: workspaceRenameFieldFrame.minX,
                    dy: workspaceRenameFieldFrame.minY + toolbar.frame.minY
                )

                fileActionX += saveActionWidth + 6
                let discardActionWidth: CGFloat = 138
                let discardIcon = makeSymbolButtonLayer(
                    symbolName: "arrow.uturn.backward",
                    accessibilityTitle: "Discard Dockerfile changes"
                )
                discardIcon.applyStyle(tintCGColor: resolvedCGColor(.controlAccentColor),
                                       backgroundCGColor: resolvedCGColor(.clear))
                discardIcon.frame = CGRect(x: fileActionX, y: 5, width: 26, height: 26)
                toolbar.addSublayer(discardIcon)

                let discardLabel = makeTextLayer(size: 11,
                                                 weight: .medium,
                                                 color: .controlAccentColor)
                discardLabel.string = "Discard Changes"
                discardLabel.frame = CGRect(x: fileActionX + 27, y: 10,
                                            width: 108, height: 16)
                toolbar.addSublayer(discardLabel)
                let localDiscardFrame = CGRect(x: fileActionX, y: 2,
                                               width: discardActionWidth, height: 32)
                containerConfigurationDiscardFrame = localDiscardFrame.offsetBy(
                    dx: workspaceRenameFieldFrame.minX,
                    dy: workspaceRenameFieldFrame.minY + toolbar.frame.minY
                )
            }

            let copyActionWidth: CGFloat = 190
            let cookbookActionWidth: CGFloat = 130
            let copyActionX = max(toolbar.bounds.width - copyActionWidth - 6, 6)
            let cookbookActionX = max(copyActionX - cookbookActionWidth - 6, 6)

            let cookbookIcon = makeSymbolButtonLayer(
                symbolName: "book.closed",
                accessibilityTitle: "View cookbook"
            )
            cookbookIcon.applyStyle(tintCGColor: resolvedCGColor(.controlAccentColor),
                                    backgroundCGColor: resolvedCGColor(.clear))
            cookbookIcon.frame = CGRect(x: cookbookActionX + 2, y: 5, width: 26, height: 26)
            toolbar.addSublayer(cookbookIcon)

            let cookbookLabel = makeTextLayer(size: 11,
                                              weight: .medium,
                                              color: .controlAccentColor)
            cookbookLabel.string = "View cookbook"
            cookbookLabel.frame = CGRect(x: cookbookActionX + 30, y: 10, width: 100, height: 16)
            toolbar.addSublayer(cookbookLabel)

            let icon = makeSymbolButtonLayer(
                symbolName: "doc.on.doc",
                accessibilityTitle: "Copy path to Dockerfile"
            )
            icon.applyStyle(tintCGColor: resolvedCGColor(.controlAccentColor),
                            backgroundCGColor: resolvedCGColor(.clear))
            icon.frame = CGRect(x: copyActionX + 2, y: 5, width: 26, height: 26)
            toolbar.addSublayer(icon)

            let label = makeTextLayer(size: 11,
                                      weight: .medium,
                                      color: .controlAccentColor)
            label.string = "Copy path to Dockerfile"
            label.frame = CGRect(x: copyActionX + 30, y: 10, width: 160, height: 16)
            toolbar.addSublayer(label)

            let cookbookActionFrame = CGRect(x: cookbookActionX, y: 2,
                                             width: cookbookActionWidth, height: 32)
            let copyActionFrame = CGRect(x: copyActionX, y: 2,
                                         width: copyActionWidth, height: 32)
            containerConfigurationEditorToolbarFrame = toolbar.frame
                .offsetBy(dx: workspaceRenameFieldFrame.minX,
                          dy: workspaceRenameFieldFrame.minY)
            containerConfigurationCookbookFrame = cookbookActionFrame
                .offsetBy(dx: containerConfigurationEditorToolbarFrame.minX,
                          dy: containerConfigurationEditorToolbarFrame.minY)
            containerConfigurationCopyPathFrame = copyActionFrame
                .offsetBy(dx: containerConfigurationEditorToolbarFrame.minX,
                          dy: containerConfigurationEditorToolbarFrame.minY)
        }
    }

    private func dockerfileSourceCharacterRange(
        for progress: LocalWorkspaceRecord.BuildProgress,
        in dockerfile: String
    ) -> Range<Int>? {
        guard progress.phase == "building",
              let startLine = progress.sourceStartLine,
              let endLine = progress.sourceEndLine,
              startLine > 0,
              endLine >= startLine else {
            return nil
        }
        let lines = dockerfile.components(separatedBy: "\n")
        guard startLine <= lines.count else { return nil }
        var offset = 0
        for index in 0..<(startLine - 1) {
            offset += (lines[index] as NSString).length + 1
        }
        let start = offset
        for index in (startLine - 1)..<min(endLine, lines.count) {
            offset += (lines[index] as NSString).length
            if index + 1 < lines.count {
                offset += 1
            }
        }
        return start..<offset
    }

    private func renderContainerBuildProgress(
        _ progress: LocalWorkspaceRecord.BuildProgress,
        in frame: CGRect
    ) {
        guard frame.height > 0 else { return }
        let card = CALayer()
        card.frame = frame
        card.cornerRadius = 7
        card.backgroundColor = resolvedCGColor(.controlBackgroundColor)
        card.borderWidth = 0.5
        card.borderColor = resolvedCGColor(.separatorColor)
        card.masksToBounds = true
        workspacePanelLayer.addSublayer(card)

        let status = makeTextLayer(size: 11, weight: .semibold, color: .labelColor)
        status.string = progress.detail
        status.truncationMode = .end
        status.frame = CGRect(x: 12,
                              y: frame.height - 25,
                              width: frame.width - 24,
                              height: 16)
        card.addSublayer(status)

        if let current = progress.currentStep,
           let total = progress.totalSteps,
           total > 0 {
            let track = CALayer()
            track.frame = CGRect(x: 12,
                                 y: frame.height - 34,
                                 width: frame.width - 24,
                                 height: 2)
            track.backgroundColor = resolvedCGColor(.separatorColor)
            track.cornerRadius = 1
            card.addSublayer(track)

            let fill = CALayer()
            fill.frame = CGRect(x: 0,
                                y: 0,
                                width: track.bounds.width * min(
                                    max(CGFloat(current) / CGFloat(total), 0),
                                    1
                                ),
                                height: 2)
            fill.backgroundColor = resolvedCGColor(.controlAccentColor)
            fill.cornerRadius = 1
            track.addSublayer(fill)
        }

        let logLines = normalizedContainerBuildError(progress.log)
            .components(separatedBy: "\n")
            .filter { !$0.isEmpty }
        let visibleLog = logLines.suffix(5).joined(separator: "\n")
        guard !visibleLog.isEmpty else { return }
        let logFrame = CGRect(x: 12,
                              y: 8,
                              width: frame.width - 24,
                              height: max(frame.height - 48, 1))
        let logContainer = CALayer()
        logContainer.frame = logFrame
        logContainer.masksToBounds = true
        card.addSublayer(logContainer)
        let wrappedLines = dockerfileWrappedLines(visibleLog, width: logContainer.bounds.width)
        let lineHeight: CGFloat = 13
        let maximumLines = max(Int(floor(logFrame.height / lineHeight)), 1)
        let shownLines = Array(wrappedLines.suffix(maximumLines))
        let selectionLayer = CALayer()
        selectionLayer.frame = logContainer.bounds
        logContainer.addSublayer(selectionLayer)
        var renderedLines: [DockerfileTextLine] = []
        for (index, wrappedLine) in shownLines.enumerated() {
            let lineFrame = CGRect(
                x: 0,
                y: logContainer.bounds.height - CGFloat(index + 1) * lineHeight,
                width: logContainer.bounds.width,
                height: lineHeight
            )
            let line = makeTextLayer(size: 9,
                                     weight: .regular,
                                     color: .secondaryLabelColor,
                                     monospaced: true)
            line.string = wrappedLine.text
            line.frame = lineFrame
            logContainer.addSublayer(line)
            renderedLines.append(
                DockerfileTextLine(
                    text: wrappedLine.text,
                    range: wrappedLine.range,
                    frame: lineFrame.offsetBy(
                        dx: frame.minX + logFrame.minX,
                        dy: frame.minY + logFrame.minY
                    )
                )
            )
        }
        let block = DockerfileTextBlock(
            fragmentID: "container-build-progress",
            text: visibleLog,
            frame: logFrame.offsetBy(dx: frame.minX, dy: frame.minY),
            lines: renderedLines,
            font: NSFont.monospacedSystemFont(ofSize: 9, weight: .regular),
            contentSpace: .workspacePanel,
            selectionLayer: selectionLayer
        )
        safeSpaceDockerfileTextBlocks.append(block)
        renderDockerfileSelection(in: block)
    }

    private func updateContainerConfigurationSelectionLayers() {
        guard let selectionLayer = containerConfigurationTextSelectionLayer else { return }
        withoutImplicitAnimations {
            selectionLayer.sublayers = nil
            guard workspaceRenameInputController.isFocused,
                  let selection = workspaceRenameInputController.selectionRange else {
                return
            }
            let fieldHeight = workspaceRenameFieldFrame.height
            for fragment in containerConfigurationTextFragments {
                let lower = max(selection.lowerBound, fragment.start)
                let upper = min(selection.upperBound, fragment.end)
                guard upper > lower else { continue }
                let localY = fragment.y - workspaceRenameFieldFrame.minY - 1
                guard localY > -fieldHeight,
                      localY < fieldHeight * 2 else {
                    continue
                }
                let line = makeCreateFieldLine(for: fragment.text, monospaced: true)
                let offsets = selectionOffsets(
                    line: line,
                    text: fragment.text,
                    range: (lower - fragment.start)..<(upper - fragment.start),
                    maxWidth: containerConfigurationTextVisibleFrame.width
                )
                let highlight = CALayer()
                highlight.frame = CGRect(
                    x: 12 + offsets.start,
                    y: localY,
                    width: max(offsets.end - offsets.start, 1),
                    height: 18
                )
                highlight.backgroundColor = resolvedCGColor(
                    NSColor.selectedTextBackgroundColor.withAlphaComponent(0.65)
                )
                selectionLayer.addSublayer(highlight)
            }
        }
    }

    private func normalizedContainerBuildError(_ value: String) -> String {
        let normalizedNewlines = value
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
        guard let expression = try? NSRegularExpression(
            pattern: "\u{001B}\\[[0-?]*[ -/]*[@-~]"
        ) else {
            return normalizedNewlines
        }
        return expression.stringByReplacingMatches(
            in: normalizedNewlines,
            range: NSRange(location: 0, length: (normalizedNewlines as NSString).length),
            withTemplate: ""
        )
    }

    private func renderContainerConfigurationBuildError(panelWidth: CGFloat,
                                                        panelHeight: CGFloat) {
        guard let rawError = containerConfigurationBuildError else { return }
        let error = normalizedContainerBuildError(rawError)

        let dim = CALayer()
        dim.frame = workspacePanelLayer.bounds
        dim.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.2))
        dim.cornerRadius = workspacePanelLayer.cornerRadius
        workspacePanelLayer.addSublayer(dim)

        let promptWidth = min(max(panelWidth - 80, 520), 900)
        let promptHeight = min(max(panelHeight - 96, 340), 640)
        let promptFrame = CGRect(
            x: floor((panelWidth - promptWidth) / 2),
            y: floor((panelHeight - promptHeight) / 2),
            width: promptWidth,
            height: promptHeight
        )
        let prompt = CALayer()
        prompt.frame = promptFrame
        prompt.cornerRadius = 12
        prompt.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        prompt.borderWidth = 0.5
        prompt.borderColor = resolvedCGColor(.separatorColor)
        prompt.shadowColor = resolvedCGColor(.black)
        prompt.shadowOpacity = 0.2
        prompt.shadowRadius = 16
        prompt.shadowOffset = CGSize(width: 0, height: -3)
        workspacePanelLayer.addSublayer(prompt)

        let title = makeTextLayer(size: 17, weight: .semibold, color: .labelColor)
        title.string = "Container Rebuild Failed"
        title.frame = CGRect(x: 20,
                             y: promptHeight - 42,
                             width: promptWidth - 180,
                             height: 22)
        prompt.addSublayer(title)

        let detail = makeTextLayer(size: 11,
                                   weight: .regular,
                                   color: .secondaryLabelColor)
        detail.string = "The existing container was left in place. Review or copy the complete build output below."
        detail.isWrapped = true
        detail.frame = CGRect(x: 20,
                              y: promptHeight - 72,
                              width: promptWidth - 40,
                              height: 30)
        prompt.addSublayer(detail)

        let textFrame = CGRect(x: 20,
                               y: 62,
                               width: promptWidth - 40,
                               height: max(promptHeight - 146, 150))
        let textBackground = CALayer()
        textBackground.frame = textFrame
        textBackground.cornerRadius = 7
        textBackground.backgroundColor = resolvedCGColor(.textBackgroundColor)
        textBackground.borderWidth = 0.5
        textBackground.borderColor = resolvedCGColor(.separatorColor)
        textBackground.masksToBounds = true
        prompt.addSublayer(textBackground)

        let textWidth = max(textFrame.width - 24, 1)
        let wrappedLines = dockerfileWrappedLines(error, width: textWidth)
        let lineHeight: CGFloat = 15
        containerConfigurationBuildErrorContentHeight =
            CGFloat(wrappedLines.count) * lineHeight + 24
        let maximumScroll = max(
            containerConfigurationBuildErrorContentHeight - textFrame.height,
            0
        )
        containerConfigurationBuildErrorScroll = min(
            max(containerConfigurationBuildErrorScroll, 0),
            maximumScroll
        )

        let selectionLayer = CALayer()
        selectionLayer.frame = textBackground.bounds
        textBackground.addSublayer(selectionLayer)

        let viewport = CALayer()
        viewport.frame = textBackground.bounds
        textBackground.addSublayer(viewport)
        containerConfigurationBuildErrorViewportLayer = viewport
        containerConfigurationBuildErrorRenderedScroll =
            containerConfigurationBuildErrorScroll

        var renderedLines: [DockerfileTextLine] = []
        renderedLines.reserveCapacity(wrappedLines.count)
        for (index, wrappedLine) in wrappedLines.enumerated() {
            let localFrame = CGRect(
                x: 12,
                y: textFrame.height - 12 - CGFloat(index + 1) * lineHeight +
                    containerConfigurationBuildErrorScroll,
                width: textWidth,
                height: lineHeight
            )
            let absoluteFrame = localFrame.offsetBy(dx: textFrame.minX,
                                                     dy: textFrame.minY)
            renderedLines.append(
                DockerfileTextLine(
                    text: wrappedLine.text,
                    range: wrappedLine.range,
                    frame: absoluteFrame
                )
            )
            guard localFrame.maxY >= -textFrame.height,
                  localFrame.minY <= textFrame.height * 2 else {
                continue
            }
            let line = makeTextLayer(size: 10,
                                     weight: .regular,
                                     color: .systemRed,
                                     monospaced: true)
            line.string = wrappedLine.text
            line.frame = localFrame
            viewport.addSublayer(line)
        }

        let textBlock = DockerfileTextBlock(
            fragmentID: "container-build-error",
            text: error,
            frame: textFrame.offsetBy(dx: promptFrame.minX, dy: promptFrame.minY),
            lines: renderedLines.map { line in
                DockerfileTextLine(
                    text: line.text,
                    range: line.range,
                    frame: line.frame.offsetBy(dx: promptFrame.minX, dy: promptFrame.minY)
                )
            },
            font: dockerfileFont(),
            contentSpace: .workspacePanel,
            selectionLayer: selectionLayer
        )
        safeSpaceDockerfileTextBlocks.append(textBlock)
        renderDockerfileSelection(in: textBlock)

        let dismissWidth: CGFloat = 72
        let copyWidth: CGFloat = 96
        let dismissFrame = CGRect(x: promptWidth - 20 - dismissWidth,
                                  y: 18,
                                  width: dismissWidth,
                                  height: 30)
        let dismiss = makeButtonLayer(title: "Done", emphasized: true)
        dismiss.frame = dismissFrame
        prompt.addSublayer(dismiss)

        let copyFrame = CGRect(x: dismissFrame.minX - 10 - copyWidth,
                               y: 18,
                               width: copyWidth,
                               height: 30)
        let copyTitle = containerConfigurationBuildErrorCopyConfirmationID == nil
            ? "Copy Error"
            : "Copied"
        let copy = makeButtonLayer(title: copyTitle, emphasized: false)
        copy.frame = copyFrame
        prompt.addSublayer(copy)

        containerConfigurationBuildErrorTextFrame = textBlock.frame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )
        containerConfigurationBuildErrorCopyFrame = copyFrame
            .offsetBy(dx: promptFrame.minX + workspacePanelFrame.minX,
                      dy: promptFrame.minY + workspacePanelFrame.minY)
        containerConfigurationBuildErrorDismissFrame = dismissFrame
            .offsetBy(dx: promptFrame.minX + workspacePanelFrame.minX,
                      dy: promptFrame.minY + workspacePanelFrame.minY)
    }

    private func renderContainerConfigurationRebuildPrompt(panelWidth: CGFloat,
                                                           panelHeight: CGFloat) {
        let dim = CALayer()
        dim.frame = workspacePanelLayer.bounds
        dim.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.18))
        dim.cornerRadius = workspacePanelLayer.cornerRadius
        workspacePanelLayer.addSublayer(dim)

        let promptWidth = min(max(panelWidth - 72, 420), 560)
        let promptHeight: CGFloat = 188
        let localPromptFrame = CGRect(
            x: floor((panelWidth - promptWidth) / 2),
            y: floor((panelHeight - promptHeight) / 2),
            width: promptWidth,
            height: promptHeight
        )
        let prompt = CALayer()
        prompt.frame = localPromptFrame
        prompt.cornerRadius = 10
        prompt.borderWidth = 0.5
        prompt.borderColor = resolvedCGColor(.separatorColor)
        prompt.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        prompt.shadowColor = resolvedCGColor(.black)
        prompt.shadowOpacity = 0.18
        prompt.shadowRadius = 14
        prompt.shadowOffset = CGSize(width: 0, height: -3)
        workspacePanelLayer.addSublayer(prompt)
        containerConfigurationRebuildPromptFrame = localPromptFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )

        let title = makeTextLayer(size: 17, weight: .semibold, color: .labelColor)
        title.string = "Save Dockerfile changes?"
        title.frame = CGRect(x: 22, y: promptHeight - 47,
                             width: promptWidth - 44, height: 23)
        prompt.addSublayer(title)

        let detail = makeTextLayer(size: 12, weight: .regular,
                                   color: .secondaryLabelColor)
        detail.string = "The Dockerfile has unsaved changes. Save them before rebuilding the container?"
        detail.isWrapped = true
        detail.frame = CGRect(x: 22, y: 75, width: promptWidth - 44, height: 42)
        prompt.addSublayer(detail)

        let saveWidth: CGFloat = 126
        let withoutSavingWidth: CGFloat = 96
        let cancelWidth: CGFloat = 72
        let gap: CGFloat = 8
        var buttonX = promptWidth - 20 - saveWidth
        let localSaveFrame = CGRect(x: buttonX, y: 20, width: saveWidth, height: 32)
        let save = makeButtonLayer(title: "Save and Rebuild", emphasized: true)
        save.frame = localSaveFrame
        prompt.addSublayer(save)

        buttonX -= gap + withoutSavingWidth
        let localWithoutSavingFrame = CGRect(x: buttonX, y: 20,
                                             width: withoutSavingWidth, height: 32)
        let withoutSaving = makeButtonLayer(title: "Don’t Save", emphasized: false)
        withoutSaving.frame = localWithoutSavingFrame
        prompt.addSublayer(withoutSaving)

        buttonX -= gap + cancelWidth
        let localCancelFrame = CGRect(x: buttonX, y: 20,
                                      width: cancelWidth, height: 32)
        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        cancel.frame = localCancelFrame
        prompt.addSublayer(cancel)

        let rootOffsetX = workspacePanelFrame.minX + localPromptFrame.minX
        let rootOffsetY = workspacePanelFrame.minY + localPromptFrame.minY
        containerConfigurationRebuildSaveFrame = localSaveFrame.offsetBy(
            dx: rootOffsetX, dy: rootOffsetY
        )
        containerConfigurationRebuildWithoutSavingFrame = localWithoutSavingFrame.offsetBy(
            dx: rootOffsetX, dy: rootOffsetY
        )
        containerConfigurationRebuildCancelFrame = localCancelFrame.offsetBy(
            dx: rootOffsetX, dy: rootOffsetY
        )
    }

    private func renderContainerConfigurationDismissalPrompt(panelWidth: CGFloat,
                                                             panelHeight: CGFloat) {
        let dim = CALayer()
        dim.frame = workspacePanelLayer.bounds
        dim.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.18))
        dim.cornerRadius = workspacePanelLayer.cornerRadius
        workspacePanelLayer.addSublayer(dim)

        let promptWidth = min(max(panelWidth - 72, 420), 560)
        let promptHeight: CGFloat = 188
        let localPromptFrame = CGRect(
            x: floor((panelWidth - promptWidth) / 2),
            y: floor((panelHeight - promptHeight) / 2),
            width: promptWidth,
            height: promptHeight
        )
        let prompt = CALayer()
        prompt.frame = localPromptFrame
        prompt.cornerRadius = 10
        prompt.borderWidth = 0.5
        prompt.borderColor = resolvedCGColor(.separatorColor)
        prompt.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        prompt.shadowColor = resolvedCGColor(.black)
        prompt.shadowOpacity = 0.18
        prompt.shadowRadius = 14
        prompt.shadowOffset = CGSize(width: 0, height: -3)
        workspacePanelLayer.addSublayer(prompt)
        containerConfigurationDismissPromptFrame = localPromptFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )

        let title = makeTextLayer(size: 17, weight: .semibold, color: .labelColor)
        title.string = "Save Dockerfile changes?"
        title.frame = CGRect(x: 22, y: promptHeight - 47,
                             width: promptWidth - 44, height: 23)
        prompt.addSublayer(title)

        let detail = makeTextLayer(size: 12, weight: .regular,
                                   color: .secondaryLabelColor)
        detail.string = "The Dockerfile has unsaved changes. Save them before closing the container editor?"
        detail.isWrapped = true
        detail.frame = CGRect(x: 22, y: 75, width: promptWidth - 44, height: 42)
        prompt.addSublayer(detail)

        let saveWidth: CGFloat = 120
        let withoutSavingWidth: CGFloat = 96
        let cancelWidth: CGFloat = 72
        let gap: CGFloat = 8
        var buttonX = promptWidth - 20 - saveWidth
        let localSaveFrame = CGRect(x: buttonX, y: 20, width: saveWidth, height: 32)
        let save = makeButtonLayer(title: "Save and Close", emphasized: true)
        save.frame = localSaveFrame
        prompt.addSublayer(save)

        buttonX -= gap + withoutSavingWidth
        let localWithoutSavingFrame = CGRect(x: buttonX, y: 20,
                                             width: withoutSavingWidth, height: 32)
        let withoutSaving = makeButtonLayer(title: "Don’t Save", emphasized: false)
        withoutSaving.frame = localWithoutSavingFrame
        prompt.addSublayer(withoutSaving)

        buttonX -= gap + cancelWidth
        let localCancelFrame = CGRect(x: buttonX, y: 20,
                                      width: cancelWidth, height: 32)
        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        cancel.frame = localCancelFrame
        prompt.addSublayer(cancel)

        let rootOffsetX = workspacePanelFrame.minX + localPromptFrame.minX
        let rootOffsetY = workspacePanelFrame.minY + localPromptFrame.minY
        containerConfigurationDismissSaveFrame = localSaveFrame.offsetBy(
            dx: rootOffsetX, dy: rootOffsetY
        )
        containerConfigurationDismissWithoutSavingFrame = localWithoutSavingFrame.offsetBy(
            dx: rootOffsetX, dy: rootOffsetY
        )
        containerConfigurationDismissCancelFrame = localCancelFrame.offsetBy(
            dx: rootOffsetX, dy: rootOffsetY
        )
    }

    private var containerConfigurationPersistentData: [LocalWorkspaceRecord.PersistentData] {
        pendingDockerfileWorkspace?.persistentDataDirectories ?? []
    }

    private var containerConfigurationMountContentHeight: CGFloat {
        let folderHeight = max(CGFloat(containerConfigurationMounts.count) * 60, 28)
        let persistentHeight = max(CGFloat(containerConfigurationPersistentData.count) * 60, 28)
        return 24 + folderHeight + 12 + 24 + persistentHeight
    }

    private func renderContainerConfigurationRuntime(
        _ workspace: LocalWorkspaceRecord,
        in contentFrame: CGRect
    ) {
        let currentProviderID = workspace.runtime?.providerID ?? "apple.container"
        let currentProvider = availableSafeSpaceProviders.first {
            $0.id == currentProviderID
        }
        let destination = availableSafeSpaceProviders.first {
            $0.id != currentProviderID && $0.canCreate
        }

        let heading = makeTextLayer(size: 13, weight: .semibold, color: .labelColor)
        heading.string = currentProvider?.name ?? workspace.runtimeDescription
        heading.frame = CGRect(x: contentFrame.minX,
                               y: contentFrame.maxY - 34,
                               width: contentFrame.width,
                               height: 20)
        workspacePanelLayer.addSublayer(heading)

        let detail = makeTextLayer(size: 11, weight: .regular, color: .secondaryLabelColor)
        detail.string = currentProvider?.detail ??
            "The OCI runtime currently executing this container."
        detail.frame = CGRect(x: contentFrame.minX,
                              y: contentFrame.maxY - 58,
                              width: contentFrame.width,
                              height: 18)
        workspacePanelLayer.addSublayer(detail)

        let capability = makeTextLayer(size: 11, weight: .regular, color: .secondaryLabelColor)
        capability.string = workspace.canMountFoldersLive
            ? "Host file changes are delivered to file watchers inside this container."
            : "Host file changes may not notify file watchers inside this container."
        capability.frame = CGRect(x: contentFrame.minX,
                                  y: contentFrame.maxY - 88,
                                  width: contentFrame.width,
                                  height: 18)
        workspacePanelLayer.addSublayer(capability)

        guard let destination else {
            let unavailable = makeTextLayer(size: 11,
                                            weight: .regular,
                                            color: .secondaryLabelColor)
            unavailable.string = "Install and start another supported runtime to convert this container."
            unavailable.frame = CGRect(x: contentFrame.minX,
                                       y: contentFrame.maxY - 132,
                                       width: contentFrame.width,
                                       height: 18)
            workspacePanelLayer.addSublayer(unavailable)
            return
        }

        let changeButton = makeButtonLayer(
            title: "Change runtime to \(destination.name)",
            emphasized: false
        )
        let changeWidth = min(max(CGFloat(destination.name.count * 8 + 154), 220),
                              contentFrame.width)
        let changeFrame = CGRect(x: contentFrame.minX,
                                 y: contentFrame.maxY - 146,
                                 width: changeWidth,
                                 height: 32)
        changeButton.frame = changeFrame
        workspacePanelLayer.addSublayer(changeButton)
        containerConfigurationChangeRuntimeFrame = changeFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )

    }

    private func renderContainerConfigurationMounts(in contentFrame: CGRect) {
        let detail = makeTextLayer(size: 11,
                                   weight: .regular,
                                   color: .secondaryLabelColor)
        detail.string = "Folder mounts and persistent data are attached when the container starts."
        detail.frame = CGRect(x: contentFrame.minX,
                              y: contentFrame.maxY - 22,
                              width: contentFrame.width,
                              height: 17)
        workspacePanelLayer.addSublayer(detail)

        let listFrame = CGRect(x: contentFrame.minX,
                               y: contentFrame.minY + 42,
                               width: contentFrame.width,
                               height: max(contentFrame.height - 76, 0))
        var cursor = listFrame.maxY + containerConfigurationMountScroll

        func addSectionTitle(_ value: String) {
            let frame = CGRect(x: contentFrame.minX + 2,
                               y: cursor - 18,
                               width: contentFrame.width - 4,
                               height: 15)
            cursor -= 24
            guard frame.minY >= listFrame.minY,
                  frame.maxY <= listFrame.maxY else {
                return
            }
            let title = makeTextLayer(size: 10,
                                      weight: .semibold,
                                      color: .secondaryLabelColor)
            title.string = value.uppercased()
            title.frame = frame
            workspacePanelLayer.addSublayer(title)
        }

        func addEmptyMessage(_ value: String) {
            let frame = CGRect(x: contentFrame.minX + 12,
                               y: cursor - 20,
                               width: contentFrame.width - 24,
                               height: 16)
            cursor -= 28
            guard frame.minY >= listFrame.minY,
                  frame.maxY <= listFrame.maxY else {
                return
            }
            let message = makeTextLayer(size: 10,
                                        weight: .regular,
                                        color: .tertiaryLabelColor)
            message.string = value
            message.frame = frame
            workspacePanelLayer.addSublayer(message)
        }

        addSectionTitle("Folder Mounts")
        if containerConfigurationMounts.isEmpty {
            addEmptyMessage("No folders are mounted.")
        }
        for mount in containerConfigurationMounts {
            let rowFrame = CGRect(x: contentFrame.minX,
                                  y: cursor - 54,
                                  width: contentFrame.width,
                                  height: 52)
            cursor -= 60
            guard rowFrame.minY >= listFrame.minY,
                  rowFrame.maxY <= listFrame.maxY else {
                continue
            }
            let row = CALayer()
            row.frame = rowFrame
            row.cornerRadius = 7
            row.backgroundColor = resolvedCGColor(
                NSColor.controlBackgroundColor.withAlphaComponent(0.55)
            )
            workspacePanelLayer.addSublayer(row)

            let name = makeTextLayer(size: 11, weight: .semibold, color: .labelColor)
            name.string = mount.name
            name.frame = CGRect(x: 12,
                                y: 35,
                                width: max(row.bounds.width - 210, 1),
                                height: 14)
            row.addSublayer(name)
            let pathWidth = max(row.bounds.width - 210, 1)
            let hostPathFrame = CGRect(x: 12, y: 19, width: pathWidth, height: 14)
            renderSelectableMountedFolderPath(
                mount.hostPath,
                identifier: "configuration-mount:\(mount.id):host",
                font: NSFont.systemFont(ofSize: 9, weight: .regular),
                color: .secondaryLabelColor,
                localFrame: hostPathFrame,
                contentFrame: hostPathFrame.offsetBy(dx: rowFrame.minX, dy: rowFrame.minY),
                contentSpace: .workspacePanel,
                in: row
            )
            let guestPathFrame = CGRect(x: 12, y: 4, width: pathWidth, height: 14)
            renderSelectableMountedFolderPath(
                mount.guestPath,
                identifier: "configuration-mount:\(mount.id):guest",
                font: NSFont.systemFont(ofSize: 9, weight: .regular),
                color: .secondaryLabelColor,
                localFrame: guestPathFrame,
                contentFrame: guestPathFrame.offsetBy(dx: rowFrame.minX, dy: rowFrame.minY),
                contentSpace: .workspacePanel,
                in: row
            )

            let rootRowFrame = row.frame.offsetBy(dx: workspacePanelFrame.minX,
                                                   dy: workspacePanelFrame.minY)
            if mount.isInfrastructure {
                let managed = makeTextLayer(size: 9,
                                            weight: .medium,
                                            color: .tertiaryLabelColor,
                                            alignment: .right)
                managed.string = "Managed by Outer Shell"
                managed.frame = CGRect(x: row.bounds.width - 172,
                                       y: 19,
                                       width: 158,
                                       height: 14)
                row.addSublayer(managed)
            } else {
                let readOnlyFrame = CGRect(x: row.bounds.width - 180,
                                           y: 11,
                                           width: 108,
                                           height: 30)
                let readOnly = makeButtonLayer(
                    title: mount.isReadOnly ? "Read only" : "Read & write",
                    emphasized: mount.isReadOnly
                )
                readOnly.frame = readOnlyFrame
                row.addSublayer(readOnly)
                containerConfigurationMountActionFrames.append((
                    readOnlyFrame.offsetBy(dx: rootRowFrame.minX,
                                           dy: rootRowFrame.minY),
                    mount.id,
                    "toggleReadOnly"
                ))

                let removeFrame = CGRect(x: row.bounds.width - 58,
                                         y: 11,
                                         width: 44,
                                         height: 30)
                let remove = makeSymbolButtonLayer(
                    symbolName: "minus.circle",
                    accessibilityTitle: "Remove \(mount.name)"
                )
                remove.frame = removeFrame
                row.addSublayer(remove)
                containerConfigurationMountActionFrames.append((
                    removeFrame.offsetBy(dx: rootRowFrame.minX,
                                         dy: rootRowFrame.minY),
                    mount.id,
                    "remove"
                ))
            }
        }

        cursor -= 12
        addSectionTitle("Persistent Data")
        let persistentData = containerConfigurationPersistentData
        if persistentData.isEmpty {
            addEmptyMessage("No persistent data is declared with VOLUME.")
        }
        for item in persistentData {
            let rowFrame = CGRect(x: contentFrame.minX,
                                  y: cursor - 54,
                                  width: contentFrame.width,
                                  height: 52)
            cursor -= 60
            guard rowFrame.minY >= listFrame.minY,
                  rowFrame.maxY <= listFrame.maxY else {
                continue
            }
            let row = CALayer()
            row.frame = rowFrame
            row.cornerRadius = 7
            row.backgroundColor = resolvedCGColor(
                NSColor.controlBackgroundColor.withAlphaComponent(0.55)
            )
            workspacePanelLayer.addSublayer(row)

            let pathWidth = max(row.bounds.width - 210, 1)
            let destinationFrame = CGRect(x: 12, y: 29, width: pathWidth, height: 16)
            renderSelectableMountedFolderPath(
                item.guestPath,
                identifier: "configuration-persistent-data:\(item.id):guest",
                font: NSFont.systemFont(ofSize: 11, weight: .semibold),
                color: .labelColor,
                localFrame: destinationFrame,
                contentFrame: destinationFrame.offsetBy(dx: rowFrame.minX, dy: rowFrame.minY),
                contentSpace: .workspacePanel,
                in: row
            )

            let storageFrame = CGRect(x: 12, y: 8, width: pathWidth, height: 14)
            renderSelectableMountedFolderPath(
                item.hostPath,
                identifier: "configuration-persistent-data:\(item.id):host",
                font: NSFont.systemFont(ofSize: 9, weight: .regular),
                color: .secondaryLabelColor,
                localFrame: storageFrame,
                contentFrame: storageFrame.offsetBy(dx: rowFrame.minX, dy: rowFrame.minY),
                contentSpace: .workspacePanel,
                in: row
            )

            let source = makeTextLayer(size: 9,
                                       weight: .medium,
                                       color: .tertiaryLabelColor,
                                       alignment: .right)
            source.string = "Dockerfile VOLUME"
            source.frame = CGRect(x: row.bounds.width - 172,
                                  y: 19,
                                  width: 158,
                                  height: 14)
            row.addSublayer(source)
        }

        let localAddFrame = CGRect(x: contentFrame.minX,
                                   y: contentFrame.minY,
                                   width: 154,
                                   height: 34)
        let add = makeButtonLayer(title: "Mount Folder…", emphasized: false)
        add.frame = localAddFrame
        workspacePanelLayer.addSublayer(add)
        containerConfigurationAddMountFrame = localAddFrame.offsetBy(
            dx: workspacePanelFrame.minX,
            dy: workspacePanelFrame.minY
        )
        workspaceRenameFieldFrame = .zero
        workspaceRenameTextFrame = .zero
    }

    private func renderWorkspaceDeletionPrompt(panelWidth: CGFloat,
                                               panelHeight: CGFloat) {
        guard let workspace = pendingWorkspaceDeletion else {
            workspaceDeletePanelFrame = .zero
            workspaceDeleteCancelFrame = .zero
            workspaceDeleteConfirmFrame = .zero
            return
        }

        workspaceDeletePanelFrame = workspacePanelFrame

        let title = makeTextLayer(size: 17, weight: .semibold, color: .labelColor)
        title.string = workspace.isManagedContainer
            ? "Delete \(workspace.name)?"
            : "Remove \(workspace.name) from Outer Shell?"
        title.frame = CGRect(x: 20, y: panelHeight - 46, width: panelWidth - 40, height: 23)
        workspacePanelLayer.addSublayer(title)

        let detail = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        detail.string = workspace.isManagedContainer
            ? "The container and its Outer Shell configuration will be permanently deleted. Mounted folders and their contents will remain on the server. This cannot be undone."
            : "Outer Shell will forget this registration. The externally managed container and all of its data will remain unchanged."
        detail.isWrapped = true
        detail.frame = CGRect(x: 20, y: 65, width: panelWidth - 40, height: 47)
        workspacePanelLayer.addSublayer(detail)

        let confirm = makeButtonLayer(
            title: workspace.isManagedContainer ? "Delete" : "Remove",
            emphasized: true
        )
        confirm.applyStyle(
            textCGColor: resolvedCGColor(.white),
            backgroundCGColor: resolvedCGColor(.systemRed),
            font: NSFont.systemFont(ofSize: 12, weight: .medium)
        )
        let localConfirmFrame = CGRect(x: panelWidth - 98, y: 18, width: 78, height: 30)
        confirm.frame = localConfirmFrame
        workspacePanelLayer.addSublayer(confirm)
        workspaceDeleteConfirmFrame = localConfirmFrame.offsetBy(dx: workspacePanelFrame.minX,
                                                                  dy: workspacePanelFrame.minY)

        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        let localCancelFrame = CGRect(x: localConfirmFrame.minX - 82,
                                      y: 18,
                                      width: 72,
                                      height: 30)
        cancel.frame = localCancelFrame
        workspacePanelLayer.addSublayer(cancel)
        workspaceDeleteCancelFrame = localCancelFrame.offsetBy(dx: workspacePanelFrame.minX,
                                                                dy: workspacePanelFrame.minY)
    }

    private func renderWorkspaceRenamePromptIfNeeded(panelWidth: CGFloat,
                                                     panelHeight: CGFloat) {
        guard isWorkspaceNamePromptVisible else {
            workspaceRenamePanelFrame = .zero
            workspaceRenameFieldFrame = .zero
            workspaceRenameTextFrame = .zero
            workspaceRenameCancelFrame = .zero
            workspaceRenameConfirmFrame = .zero
            workspaceCreationBaseImageFrame = .zero
            workspaceOuterShellBaseImageFrame = .zero
            workspaceCustomBaseImageFrame = .zero
            workspaceCustomAsIsFrame = .zero
            return
        }

        let prompt: CALayer
        let promptWidth: CGFloat
        let promptHeight: CGFloat
        let wrapsPromptDetail = pendingSharedContainerImport != nil ||
            isBaseImageChoicePrompt ||
            (pendingDockerfileWorkspace != nil && !isRenamingContainerConfiguration)
        if workspaceNamePromptDismissesPanel {
            prompt = workspacePanelLayer
            promptWidth = panelWidth
            promptHeight = panelHeight
            workspaceRenamePanelFrame = workspacePanelFrame
        } else {
            let dim = CALayer()
            dim.frame = workspacePanelLayer.bounds
            dim.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.18))
            dim.cornerRadius = workspacePanelLayer.cornerRadius
            workspacePanelLayer.addSublayer(dim)

            if pendingDockerfileWorkspace != nil &&
                !isRenamingContainerConfiguration {
                promptWidth = min(max(panelWidth - 72, 480), 900)
            } else {
                promptWidth = min(max(panelWidth - 72, 320), 460)
            }
            if pendingDockerfileWorkspace != nil &&
                !isRenamingContainerConfiguration {
                promptHeight = min(max(panelHeight - 96, 420), 760)
            } else if isBaseImageChoicePrompt {
                promptHeight = 310
            } else if isCreatingWorkspace && !isEditingCreationBaseImage {
                promptHeight = 224
            } else {
                promptHeight = 174
            }
            let localPromptFrame = CGRect(
                x: floor((panelWidth - promptWidth) / 2),
                y: floor((panelHeight - promptHeight) / 2),
                width: promptWidth,
                height: promptHeight
            )
            let nestedPrompt = CALayer()
            nestedPrompt.frame = localPromptFrame
            nestedPrompt.backgroundColor = resolvedCGColor(.windowBackgroundColor)
            nestedPrompt.cornerRadius = 12
            nestedPrompt.borderWidth = 0.5
            nestedPrompt.borderColor = resolvedCGColor(.separatorColor)
            nestedPrompt.shadowColor = resolvedCGColor(.black)
            nestedPrompt.shadowOpacity = 0.15
            nestedPrompt.shadowRadius = 14
            nestedPrompt.shadowOffset = CGSize(width: 0, height: -3)
            workspacePanelLayer.addSublayer(nestedPrompt)
            workspaceRenamePanelFrame = localPromptFrame.offsetBy(
                dx: workspacePanelFrame.minX,
                dy: workspacePanelFrame.minY
            )
            prompt = nestedPrompt
        }

        let title = makeTextLayer(size: 17, weight: .semibold, color: .labelColor)
        if pendingSharedContainerImport != nil {
            title.string = "Copy Mounted Folders"
        } else if isCreatingWorkspace && isEditingCreationBaseImage {
            title.string = "Choose Base Image"
        } else if pendingSafeSpaceAppWorkspace != nil {
            title.string = "Add JupyterLab App"
        } else if pendingRecipeBaseImageWorkspace != nil {
            title.string = "Change Base Image"
        } else if pendingRecipeUserWorkspace != nil {
            title.string = "Add Container User"
        } else if pendingRecipeScriptRename != nil {
            title.string = "Rename Setup Script"
        } else if pendingRecipeScriptWorkspace != nil {
            title.string = "Add Setup Script"
        } else if pendingDockerfileWorkspace != nil &&
                    !isRenamingContainerConfiguration {
            title.string = "Edit Dockerfile"
        } else if pendingRecipeEditStep != nil {
            title.string = "Edit Dockerfile Fragment"
        } else if pendingRecipeCommandWorkspace != nil {
            title.string = "Add Dockerfile Fragment"
        } else {
            title.string = isCreatingWorkspace ? "Add Container" : "Rename Container"
        }
        title.frame = CGRect(x: 18, y: promptHeight - 44, width: promptWidth - 36, height: 23)
        prompt.addSublayer(title)

        let detail = makeTextLayer(size: 11, weight: .regular, color: .secondaryLabelColor)
        if !workspacePanelMessage.isEmpty {
            detail.string = workspacePanelMessage
        } else if let pendingImport = pendingSharedContainerImport {
            let count = pendingImport.mounts.count
            detail.string = "Choose where \(count) included folder\(count == 1 ? "" : "s") " +
                "will be copied on this server. Paths inside the container will stay the same."
        } else if isCreatingWorkspace && isEditingCreationBaseImage {
            detail.string = "Start with Outer Shell's image, add visible support steps to another image, or use it unchanged."
        } else if let workspace = pendingSafeSpaceAppWorkspace {
            detail.string = "Choose the folder JupyterLab opens in \(workspace.name)."
        } else if let workspace = pendingWorkspaceRename {
            detail.string = "Choose a new name for \(workspace.name)."
        } else if let workspace = pendingDockerfileWorkspace {
            detail.string = "Edit \(workspace.name)'s complete Dockerfile. This file is the container's source of truth."
        } else if let workspace = pendingRecipeEditWorkspace {
            detail.string = "Edit this fragment of \(workspace.name)'s Dockerfile."
        } else if let workspace = pendingRecipeCommandWorkspace {
            detail.string = "Add editable Dockerfile instructions to \(workspace.name)."
        } else if let workspace = pendingRecipeBaseImageWorkspace {
            detail.string = "Choose the image in \(workspace.name)'s FROM instruction. The resulting Dockerfile stays explicit."
        } else if let workspace = pendingRecipeUserWorkspace {
            detail.string = "Create a Linux user and a private setup-script folder in \(workspace.name)."
        } else if pendingRecipeScriptRename != nil {
            detail.string = "Rename this mounted script. Its alphabetical position controls when it runs."
        } else if pendingRecipeScriptWorkspace != nil {
            detail.string = "Add a mounted .sh file. It can be edited inside the container or with Plaintext outside it."
        } else if isCreatingWorkspace {
            if let provider = availableSafeSpaceProviders.first(where: {
                $0.id == selectedSafeSpaceProviderID
            }) {
                detail.string = "\(provider.name) · \(provider.detail)"
            } else {
                detail.string = "Apple container · portable OCI container"
            }
        }
        detail.foregroundColor = resolvedCGColor(
            workspacePanelMessage.isEmpty || isPerformingWorkspaceOperation
                ? .secondaryLabelColor
                : .systemRed
        )
        detail.isWrapped = wrapsPromptDetail
        detail.frame = CGRect(x: 18,
                              y: promptHeight - (wrapsPromptDetail ? 84 : 64),
                              width: promptWidth - 36,
                              height: wrapsPromptDetail ? 36 : 17)
        prompt.addSublayer(detail)

        if let pendingImport = pendingSharedContainerImport {
            let shownMounts = Array(pendingImport.mounts.prefix(5))
            var rowY = promptHeight - 145
            for mount in shownMounts {
                let source = makeTextLayer(size: 10,
                                           weight: .medium,
                                           color: .labelColor)
                source.string = mount.sourceHostPath
                source.truncationMode = .middle
                source.frame = CGRect(x: 24,
                                      y: rowY + 22,
                                      width: promptWidth - 48,
                                      height: 15)
                prompt.addSublayer(source)

                let destination = makeTextLayer(size: 9,
                                                weight: .regular,
                                                color: .secondaryLabelColor)
                let root = workspaceRenameInputController.text.isEmpty
                    ? workspaceRenameName
                    : workspaceRenameInputController.text
                let serverPath = NSString(string: root)
                    .appendingPathComponent(mount.directoryName)
                destination.string = "→ \(serverPath)   ·   mounted at \(mount.guestPath)"
                destination.truncationMode = .middle
                destination.frame = CGRect(x: 24,
                                           y: rowY + 4,
                                           width: promptWidth - 48,
                                           height: 14)
                prompt.addSublayer(destination)
                rowY -= 50
            }
            if pendingImport.mounts.count > shownMounts.count {
                let more = makeTextLayer(size: 9,
                                         weight: .regular,
                                         color: .secondaryLabelColor)
                more.string = "And \(pendingImport.mounts.count - shownMounts.count) more…"
                more.frame = CGRect(x: 24, y: 105, width: promptWidth - 48, height: 14)
                prompt.addSublayer(more)
            }
            let fieldLabel = makeTextLayer(size: 9,
                                           weight: .semibold,
                                           color: .secondaryLabelColor)
            fieldLabel.string = "DESTINATION FOLDER ON SERVER"
            fieldLabel.frame = CGRect(x: 18, y: 100, width: promptWidth - 36, height: 13)
            prompt.addSublayer(fieldLabel)
        }

        let showsCreationOptions = isCreatingWorkspace && !isEditingCreationBaseImage
        let localFieldFrame = CGRect(x: 18,
                                     y: isBaseImageChoicePrompt
                                        ? 80
                                        : (showsCreationOptions ? 112 : 62),
                                     width: promptWidth - 36,
                                     height: isDockerfileFragmentPrompt
                                        ? max(promptHeight - 142, 160)
                                        : 32)
        let field = CALayer()
        field.frame = localFieldFrame
        field.cornerRadius = 6
        field.borderWidth = workspaceRenameInputController.isFocused ? 1.5 : 1
        field.borderColor = resolvedCGColor(
            workspaceRenameInputController.isFocused ? .controlAccentColor : .separatorColor
        )
        field.backgroundColor = resolvedCGColor(.textBackgroundColor)
        field.masksToBounds = true
        field.opacity = isBaseImageChoicePrompt && baseImageTemplate == .outerShell ? 0.42 : 1
        prompt.addSublayer(field)

        let promptRootOrigin = workspaceRenamePanelFrame.origin
        workspaceCreationBaseImageFrame = .zero
        workspaceOuterShellBaseImageFrame = .zero
        workspaceCustomBaseImageFrame = .zero
        workspaceCustomAsIsFrame = .zero
        if isBaseImageChoicePrompt {
            let localOuterShellFrame = CGRect(x: 18,
                                              y: 184,
                                              width: promptWidth - 36,
                                              height: 30)
            let outerShellOption = CALayer()
            outerShellOption.frame = localOuterShellFrame
            prompt.addSublayer(outerShellOption)
            let outerShellSelection = CALayer()
            outerShellSelection.frame = CGRect(x: 0, y: 6, width: 18, height: 18)
            outerShellSelection.contentsGravity = .resizeAspect
            outerShellSelection.contents = symbolCGImage(
                named: baseImageTemplate == .outerShell
                    ? "checkmark.circle.fill"
                    : "circle",
                pointSize: 13
            )
            outerShellOption.addSublayer(outerShellSelection)
            let outerShellTitle = makeTextLayer(size: 12,
                                                weight: .medium,
                                                color: .labelColor)
            outerShellTitle.string = "Outer Shell base image"
            outerShellTitle.frame = CGRect(x: 26, y: 8,
                                           width: localOuterShellFrame.width - 180,
                                           height: 17)
            outerShellOption.addSublayer(outerShellTitle)
            let outerShellDetail = makeTextLayer(size: 10,
                                                 weight: .regular,
                                                 color: .secondaryLabelColor,
                                                 alignment: .right)
            outerShellDetail.string = "Recommended · managed"
            outerShellDetail.frame = CGRect(x: localOuterShellFrame.width - 160,
                                            y: 8,
                                            width: 160,
                                            height: 15)
            outerShellOption.addSublayer(outerShellDetail)
            workspaceOuterShellBaseImageFrame = localOuterShellFrame.offsetBy(
                dx: promptRootOrigin.x,
                dy: promptRootOrigin.y
            )

            let localCustomFrame = CGRect(x: 18,
                                          y: 150,
                                          width: promptWidth - 36,
                                          height: 30)
            let customOption = CALayer()
            customOption.frame = localCustomFrame
            prompt.addSublayer(customOption)
            let customSelection = CALayer()
            customSelection.frame = CGRect(x: 0, y: 6, width: 18, height: 18)
            customSelection.contentsGravity = .resizeAspect
            customSelection.contents = symbolCGImage(
                named: baseImageTemplate == .customWithSupport
                    ? "checkmark.circle.fill"
                    : "circle",
                pointSize: 13
            )
            customOption.addSublayer(customSelection)
            let customTitle = makeTextLayer(size: 12,
                                            weight: .medium,
                                            color: .labelColor)
            customTitle.string = "Custom image + support"
            customTitle.frame = CGRect(x: 26, y: 8,
                                       width: localCustomFrame.width - 136,
                                       height: 17)
            customOption.addSublayer(customTitle)
            let customDetail = makeTextLayer(size: 10,
                                             weight: .regular,
                                             color: .secondaryLabelColor,
                                             alignment: .right)
            customDetail.string = "Visible install steps"
            customDetail.frame = CGRect(x: localCustomFrame.width - 126,
                                        y: 8,
                                        width: 126,
                                        height: 15)
            customOption.addSublayer(customDetail)
            workspaceCustomBaseImageFrame = localCustomFrame.offsetBy(
                dx: promptRootOrigin.x,
                dy: promptRootOrigin.y
            )

            let localAsIsFrame = CGRect(x: 18,
                                        y: 116,
                                        width: promptWidth - 36,
                                        height: 30)
            let asIsOption = CALayer()
            asIsOption.frame = localAsIsFrame
            prompt.addSublayer(asIsOption)
            let asIsSelection = CALayer()
            asIsSelection.frame = CGRect(x: 0, y: 6, width: 18, height: 18)
            asIsSelection.contentsGravity = .resizeAspect
            asIsSelection.contents = symbolCGImage(
                named: baseImageTemplate == .customAsIs
                    ? "checkmark.circle.fill"
                    : "circle",
                pointSize: 13
            )
            asIsOption.addSublayer(asIsSelection)
            let asIsTitle = makeTextLayer(size: 12,
                                          weight: .medium,
                                          color: .labelColor)
            asIsTitle.string = "Custom image as-is"
            asIsTitle.frame = CGRect(x: 26, y: 8,
                                     width: localAsIsFrame.width - 170,
                                     height: 17)
            asIsOption.addSublayer(asIsTitle)
            let asIsDetail = makeTextLayer(size: 10,
                                           weight: .regular,
                                           color: .secondaryLabelColor,
                                           alignment: .right)
            asIsDetail.string = "Advanced · no assumptions"
            asIsDetail.frame = CGRect(x: localAsIsFrame.width - 160,
                                      y: 8,
                                      width: 160,
                                      height: 15)
            asIsOption.addSublayer(asIsDetail)
            workspaceCustomAsIsFrame = localAsIsFrame.offsetBy(
                dx: promptRootOrigin.x,
                dy: promptRootOrigin.y
            )
        }
        if showsCreationOptions {
            let localBaseImageFrame = CGRect(x: 18,
                                             y: 62,
                                             width: promptWidth - 36,
                                             height: 36)
            let baseImageButton = CALayer()
            baseImageButton.frame = localBaseImageFrame
            baseImageButton.cornerRadius = 6
            baseImageButton.borderWidth = 0.5
            baseImageButton.borderColor = resolvedCGColor(.separatorColor)
            baseImageButton.backgroundColor = resolvedCGColor(
                NSColor.controlBackgroundColor.withAlphaComponent(0.45)
            )
            prompt.addSublayer(baseImageButton)
            let baseImageLabel = makeTextLayer(size: 9,
                                               weight: .medium,
                                               color: .secondaryLabelColor)
            baseImageLabel.string = "BASE IMAGE"
            baseImageLabel.frame = CGRect(x: 9, y: 19,
                                          width: localBaseImageFrame.width - 34,
                                          height: 12)
            baseImageButton.addSublayer(baseImageLabel)
            let baseImageValue = makeTextLayer(size: 11,
                                               weight: .regular,
                                               color: .labelColor,
                                               monospaced: true)
            baseImageValue.string = baseImageTemplate == .outerShell
                ? "Outer Shell base image"
                : creationBaseImage
            baseImageValue.truncationMode = .middle
            baseImageValue.frame = CGRect(x: 9, y: 4,
                                          width: localBaseImageFrame.width - 34,
                                          height: 15)
            baseImageButton.addSublayer(baseImageValue)
            let edit = CALayer()
            edit.frame = CGRect(x: localBaseImageFrame.width - 25,
                                y: 9,
                                width: 16,
                                height: 16)
            edit.contentsGravity = .resizeAspect
            edit.contents = symbolCGImage(named: "pencil", pointSize: 11)
            edit.opacity = 0.65
            baseImageButton.addSublayer(edit)
            workspaceCreationBaseImageFrame = localBaseImageFrame.offsetBy(
                dx: promptRootOrigin.x,
                dy: promptRootOrigin.y
            )
        }

        workspaceRenameFieldFrame = localFieldFrame.offsetBy(dx: promptRootOrigin.x,
                                                              dy: promptRootOrigin.y)
        workspaceRenameTextFrame = CGRect(x: workspaceRenameFieldFrame.minX + 9,
                                          y: workspaceRenameFieldFrame.minY + 7,
                                          width: max(workspaceRenameFieldFrame.width - 18, 1),
                                          height: max(workspaceRenameFieldFrame.height - 14, 18))

        if isDockerfileFragmentPrompt {
            workspaceRenameInputController.visualLineWidth = workspaceRenameTextFrame.width
        }

        let text = workspaceRenameInputController.isFocused
            ? workspaceRenameInputController.text
            : workspaceRenameName
        if isDockerfileFragmentPrompt {
            let layout = CreateFieldLayout(fieldFrame: workspaceRenameFieldFrame,
                                           textFrame: workspaceRenameTextFrame,
                                           key: "dockerfileFragment",
                                           monospaced: true,
                                           multiline: true)
            let fragments = createTextAreaLineFragments(text: text, layout: layout)
            if workspaceRenameInputController.isFocused,
               let selection = workspaceRenameInputController.selectionRange {
                for fragment in fragments {
                    let lower = max(selection.lowerBound, fragment.start)
                    let upper = min(selection.upperBound, fragment.end)
                    guard upper > lower else { continue }
                    let line = makeCreateFieldLine(for: fragment.text,
                                                   monospaced: true)
                    let offsets = selectionOffsets(
                        line: line,
                        text: fragment.text,
                        range: (lower - fragment.start)..<(upper - fragment.start),
                        maxWidth: workspaceRenameTextFrame.width
                    )
                    let selectionLayer = CALayer()
                    selectionLayer.frame = CGRect(
                        x: workspaceRenameTextFrame.minX - workspaceRenameFieldFrame.minX + offsets.start,
                        y: fragment.y - workspaceRenameFieldFrame.minY - 1,
                        width: max(offsets.end - offsets.start, 1),
                        height: 18
                    )
                    selectionLayer.backgroundColor = resolvedCGColor(
                        NSColor.selectedTextBackgroundColor.withAlphaComponent(0.65)
                    )
                    field.addSublayer(selectionLayer)
                }
            }
            for fragment in fragments {
                let line = makeTextLayer(size: 12,
                                         weight: .regular,
                                         color: .textColor,
                                         monospaced: true)
                line.string = fragment.text
                line.frame = CGRect(
                    x: workspaceRenameTextFrame.minX - workspaceRenameFieldFrame.minX,
                    y: fragment.y - workspaceRenameFieldFrame.minY,
                    width: workspaceRenameTextFrame.width,
                    height: 16
                )
                field.addSublayer(line)
            }
        } else if workspaceRenameInputController.isFocused,
                  let selection = workspaceRenameInputController.selectionRange,
                  !text.isEmpty {
            let line = makeWorkspaceNameFieldLine(for: text)
            let offsets = selectionOffsets(line: line,
                                           text: text,
                                           range: selection,
                                           maxWidth: workspaceRenameTextFrame.width)
            let selectionLayer = CALayer()
            selectionLayer.frame = CGRect(
                x: workspaceRenameTextFrame.minX - workspaceRenameFieldFrame.minX + offsets.start,
                y: workspaceRenameTextFrame.minY - workspaceRenameFieldFrame.minY,
                width: max(offsets.end - offsets.start, 1),
                height: workspaceRenameTextFrame.height
            )
            selectionLayer.backgroundColor = resolvedCGColor(
                NSColor.selectedTextBackgroundColor.withAlphaComponent(0.65)
            )
            field.addSublayer(selectionLayer)
        }

        if !isDockerfileFragmentPrompt {
            let value = makeTextLayer(size: 13, weight: .regular, color: .textColor)
            value.string = text.isEmpty
                ? (pendingSharedContainerImport != nil
                    ? "/path/on/this/server"
                    : (pendingSafeSpaceAppWorkspace != nil
                    ? "/home/workspace/Workspace"
                    : (pendingRecipeBaseImageWorkspace != nil ||
                       (isCreatingWorkspace && isEditingCreationBaseImage)
                        ? "debian:bookworm"
                    : (pendingRecipeUserWorkspace != nil
                        ? "Linux username"
                        : "Container name"))))
                : text
            value.foregroundColor = resolvedCGColor(
                text.isEmpty ? .placeholderTextColor : .textColor
            )
            value.truncationMode = .end
            value.frame = CGRect(x: 9,
                                 y: 7,
                                 width: max(localFieldFrame.width - 18, 1),
                                 height: 18)
            field.addSublayer(value)
        }

        if let cursorFrame = workspaceRenameFieldCursorRect() {
            addBlinkingTextCaret(
                to: field,
                frame: cursorFrame.offsetBy(dx: -workspaceRenameFieldFrame.minX,
                                            dy: -workspaceRenameFieldFrame.minY)
            )
        }

        let confirm = makeButtonLayer(
            title: pendingSharedContainerImport != nil
                ? "Import"
                : (isCreatingWorkspace && isEditingCreationBaseImage
                ? "Done"
                : (pendingSafeSpaceAppWorkspace != nil
                ? "Add App"
                : (pendingRecipeBaseImageWorkspace != nil
                    ? "Use Image"
                : (pendingRecipeUserWorkspace != nil
                    ? "Add User"
                : (pendingRecipeScriptWorkspace != nil
                    ? (pendingRecipeScriptRename == nil ? "Add" : "Rename")
                : (isDockerfileFragmentPrompt
                    ? (pendingDockerfileWorkspace != nil || pendingRecipeEditStep != nil
                        ? "Save"
                        : "Add")
                    : (isCreatingWorkspace ? "Create" : "Rename"))))))),
            emphasized: true
        )
        let localConfirmFrame = CGRect(x: promptWidth - 96, y: 18, width: 78, height: 30)
        confirm.frame = localConfirmFrame
        confirm.opacity = isPerformingWorkspaceOperation ? 0.55 : 1
        prompt.addSublayer(confirm)
        workspaceRenameConfirmFrame = localConfirmFrame.offsetBy(dx: promptRootOrigin.x,
                                                                  dy: promptRootOrigin.y)

        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        let localCancelFrame = CGRect(x: localConfirmFrame.minX - 82,
                                      y: 18,
                                      width: 72,
                                      height: 30)
        cancel.frame = localCancelFrame
        prompt.addSublayer(cancel)
        workspaceRenameCancelFrame = localCancelFrame.offsetBy(dx: promptRootOrigin.x,
                                                                dy: promptRootOrigin.y)
        sendWorkspaceRenameTextInputGeometryUpdate()
    }

    private func statusAttributedString() -> NSAttributedString {
        NSAttributedString(
            string: renderedStatusText,
            attributes: [
                .font: NSFont.systemFont(ofSize: 12, weight: .medium),
                .foregroundColor: NSColor.secondaryLabelColor
            ]
        )
    }

    private func statusLine() -> CTLine {
        CTLineCreateWithAttributedString(statusAttributedString())
    }

    private func normalizedStatusSelectionRange(_ range: NSRange?) -> NSRange? {
        guard let range else { return nil }
        let length = (renderedStatusText as NSString).length
        let lower = max(min(range.location, length), 0)
        let upper = max(min(range.location + range.length, length), lower)
        guard upper > lower else { return nil }
        return NSRange(location: lower, length: upper - lower)
    }

    private func setStatusSelectionRange(_ range: NSRange?) {
        let nextRange = normalizedStatusSelectionRange(range)
        guard nextRange != statusSelectionRange else { return }
        statusSelectionRange = nextRange
        renderStatusSelection()
        updateEditingAndPasteboardState()
    }

    private func selectedStatusAttributedText() -> NSAttributedString? {
        guard let range = normalizedStatusSelectionRange(statusSelectionRange) else {
            return nil
        }
        return statusAttributedString().attributedSubstring(from: range)
    }

    private func renderStatusSelection() {
        statusSelectionLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        guard let range = normalizedStatusSelectionRange(statusSelectionRange),
              statusSelectionLayer.bounds.width > 0 else {
            return
        }
        let line = statusLine()
        let lineWidth = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
        let originX = max(statusSelectionLayer.bounds.width - lineWidth, 0)
        let startX = originX + CGFloat(CTLineGetOffsetForStringIndex(line, range.location, nil))
        let endX = originX + CGFloat(CTLineGetOffsetForStringIndex(
            line,
            range.location + range.length,
            nil
        ))
        let selection = CALayer()
        selection.frame = CGRect(
            x: min(startX, endX),
            y: 0,
            width: max(abs(endX - startX), 1),
            height: statusSelectionLayer.bounds.height
        )
        selection.backgroundColor = resolvedCGColor(
            windowIsActive
                ? NSColor.selectedTextBackgroundColor
                : NSColor.unemphasizedSelectedTextBackgroundColor
        )
        selection.cornerRadius = 2
        statusSelectionLayer.addSublayer(selection)
    }

    private func isPointInStatus(_ point: CGPoint) -> Bool {
        guard !renderedStatusText.isEmpty,
              !isShowingWorkspacePanel else {
            return false
        }
        return rootFrame(statusLayer.frame, from: toolbarLayer)
            .insetBy(dx: 0, dy: -3)
            .contains(point)
    }

    private func statusCharacterIndex(at point: CGPoint) -> Int {
        let length = (renderedStatusText as NSString).length
        guard length > 0 else { return 0 }
        let localPoint = toolbarLayer.convert(point, from: rootLayer)
        let line = statusLine()
        let lineWidth = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
        let originX = max(statusLayer.bounds.width - lineWidth, 0)
        let x = max(localPoint.x - statusLayer.frame.minX - originX, 0)
        let index = CTLineGetStringIndexForPosition(line, CGPoint(x: x, y: 0))
        return index == kCFNotFound ? length : min(max(index, 0), length)
    }

    private func statusWordRange(containing offset: Int) -> NSRange? {
        let string = renderedStatusText as NSString
        let length = string.length
        guard length > 0 else { return nil }
        var location = min(max(offset, 0), length - 1)
        if location > 0, !logHeaderDetailCharacterIsWordLike(string.character(at: location)) {
            location -= 1
        }
        guard logHeaderDetailCharacterIsWordLike(string.character(at: location)) else {
            return NSRange(location: min(max(offset, 0), length), length: 0)
        }
        var start = location
        while start > 0, logHeaderDetailCharacterIsWordLike(string.character(at: start - 1)) {
            start -= 1
        }
        var end = location + 1
        while end < length, logHeaderDetailCharacterIsWordLike(string.character(at: end)) {
            end += 1
        }
        return NSRange(location: start, length: end - start)
    }

    private func handleStatusMouseDown(at point: CGPoint, clickCount: Int) -> Bool {
        guard isPointInStatus(point) else {
            if statusSelectionRange != nil {
                setStatusSelectionRange(nil)
            }
            return false
        }
        let offset = statusCharacterIndex(at: point)
        statusDragAnchorOffset = offset
        if clickCount >= 3 {
            setStatusSelectionRange(NSRange(
                location: 0,
                length: (renderedStatusText as NSString).length
            ))
        } else if clickCount == 2 {
            setStatusSelectionRange(statusWordRange(containing: offset))
        } else {
            setStatusSelectionRange(nil)
        }
        return true
    }

    private func handleStatusMouseDragged(to point: CGPoint) -> Bool {
        guard let anchor = statusDragAnchorOffset else { return false }
        let offset = statusCharacterIndex(at: point)
        setStatusSelectionRange(NSRange(
            location: min(anchor, offset),
            length: abs(offset - anchor)
        ))
        return true
    }

    private func handleStatusRightMouseDown(at point: CGPoint) -> Bool {
        guard isPointInStatus(point) else { return false }
        let offset = statusCharacterIndex(at: point)
        if let range = normalizedStatusSelectionRange(statusSelectionRange),
           offset >= range.location,
           offset <= range.location + range.length,
           let selected = selectedStatusAttributedText() {
            outerframeHost.showContextMenu(for: selected, at: point)
            return true
        }
        setStatusSelectionRange(NSRange(
            location: 0,
            length: (renderedStatusText as NSString).length
        ))
        outerframeHost.showContextMenu(
            for: selectedStatusAttributedText() ?? statusAttributedString(),
            at: point
        )
        return true
    }

    private func updateStatusText() {
        let text: String
        if isPerformingAction {
            text = mode == .create ? "Creating..." : backendError
        } else if isLoadingBackends && backends.isEmpty {
            text = "Loading..."
        } else if !backendError.isEmpty {
            text = backendError
        } else {
            text = ""
        }
        if text != renderedStatusText {
            renderedStatusText = text
            statusSelectionRange = nil
            statusDragAnchorOffset = nil
            updateEditingAndPasteboardState()
        }
        statusLayer.string = text
        renderStatusSelection()
    }

    private func logHeaderDetailText() -> String {
        if isLoadingLog && logSnapshot == nil {
            return "Loading..."
        }
        if let snapshot = logSnapshot {
            if !snapshot.error.isEmpty {
                return snapshot.error
            }
            let sizeText = ByteCountFormatter.string(fromByteCount: Int64(snapshot.fileSize), countStyle: .file)
            let prefix = snapshot.isTruncated ? "Showing tail of " : "Showing "
            return "\(prefix)\(snapshot.path) (\(sizeText))"
        }
        if selectedLog == nil {
            return selectedServiceID == nil ? "Select a backend to view logs." : "No registered log file."
        }
        return "No log loaded."
    }

    private func logHeaderDetailColor() -> NSColor {
        if let snapshot = logSnapshot, !snapshot.error.isEmpty {
            return .systemRed
        }
        return .secondaryLabelColor
    }

    private func currentLogText() -> String {
        if isLoadingLog && logSnapshot == nil { return "Loading logs..." }
        if !logError.isEmpty { return logError }
        if let contents = logSnapshot?.contents, !contents.isEmpty { return contents }
        if selectedServiceID != nil, selectedLog == nil { return "No registered log file." }
        if selectedServiceID == nil { return "" }
        return "No logs yet."
    }

    private func backendManagementMenuItems(for backend: BackendRecord,
                                            includePlaceholderRunActions: Bool) -> (operationByItemID: [String: String], items: [OuterframeContextMenuItem]) {
        var operationByItemID: [String: String] = [:]
        var items: [OuterframeContextMenuItem] = []
        if backend.isBackendsSelf {
            operationByItemID["about"] = "aboutOuterShell"
            items.append(OuterframeContextMenuItem(id: "about",
                                                   title: "About Outer Shell",
                                                   isEnabled: true))
            operationByItemID["showLogs"] = "showLogs:\(backend.serviceID):\(backend.serviceScope)"
            items.append(OuterframeContextMenuItem(id: "showLogs",
                                                   title: "Show logs",
                                                   isEnabled: true,
                                                   systemImageName: "doc.plaintext"))
            if backend.menuBarVisibilityAvailable {
                operationByItemID["menuBarVisibility"] = "toggleMenuBarVisibility"
                items.append(OuterframeContextMenuItem(id: "menuBarVisibility",
                                                       title: "Show in macOS menu bar when backends are running",
                                                       isEnabled: true,
                                                       state: (backend.menuBarVisibilityEnabled ?? true) ? .on : .off))
            }
            operationByItemID["checkUpdate"] = "checkUpdate"
            items.append(OuterframeContextMenuItem(id: "checkUpdate",
                                                   title: "Check for Updates",
                                                   isEnabled: true))
            operationByItemID["uninstall"] = "uninstallOuterShell"
            items.append(OuterframeContextMenuItem(id: "uninstall",
                                                   title: "Uninstall Outer Shell",
                                                   isEnabled: true))
            return (operationByItemID, items)
        }
        if backend.isBundled ?? false {
            if backend.isBundledPlaceholder && includePlaceholderRunActions {
                let systemOnlyPlaceholder = installsBundledPlaceholderAsSystemOnly(backend)
                operationByItemID["run"] = ((backend.rootOnly ?? false) || systemOnlyPlaceholder) ? "runRoot" : "run"
                items.append(OuterframeContextMenuItem(id: "run",
                                                       title: (backend.rootOnly ?? false) && !systemOnlyPlaceholder ? "Run as root" : "Run",
                                                       isEnabled: true))
                if !systemOnlyPlaceholder && (backend.supportsRoot ?? false) && !(backend.rootOnly ?? false) {
                    operationByItemID["runRoot"] = "runRoot"
                    items.append(OuterframeContextMenuItem(id: "runRoot",
                                                           title: "Run as root",
                                                           isEnabled: true))
                }
            } else if (backend.supportsRoot ?? false) && !(backend.rootOnly ?? false) && !isDirectRootSession {
                let hasRootSupport = backend.hasRootSupport ?? (backend.serviceScope == "system")
                operationByItemID["rootSupport"] = hasRootSupport ? "removeRootSupport" : "addRootSupport"
                items.append(OuterframeContextMenuItem(id: "rootSupport",
                                                       title: hasRootSupport ? "Reinstall as user-only" : "Reinstall with root support",
                                                       isEnabled: true))
            }
        }
        if backend.canUninstallBackend {
            operationByItemID["uninstall"] = "uninstall"
            items.append(OuterframeContextMenuItem(id: "uninstall",
                                                   title: "Uninstall",
                                                   isEnabled: true))
        }
        return (operationByItemID, items)
    }

    private func showBackendActionsMenu(for backend: BackendRecord, at point: CGPoint) {
        let menu = backendManagementMenuItems(for: backend, includePlaceholderRunActions: true)
        guard !menu.items.isEmpty else { return }
        let menuID = UUID()
        pendingMenuActions[menuID] = (backend.serviceID, backend.serviceScope, menu.operationByItemID)
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: menu.items,
                                       at: point)
    }

    private func showWorkspaceOverviewActionsMenu(for workspace: LocalWorkspaceRecord,
                                                  at point: CGPoint) {
        let displayedState = displayedWorkspaceState(for: workspace)
        let stateOperation = displayedState == "running" ? "stop" : "start"
        let canChangeState = displayedState != "creating" && displayedState != "starting"
        let menuID = UUID()
        var operations = [
            "state": stateOperation,
            "delete": "delete"
        ]
        var items: [OuterframeContextMenuItem] = []
        items.append(
            OuterframeContextMenuItem(id: "state",
                                      title: canChangeState
                                        ? (stateOperation == "stop" ? "Stop" : "Start")
                                        : "Preparing…",
                                      isEnabled: canChangeState && !isPerformingWorkspaceOperation,
                                      systemImageName: stateOperation == "stop"
                                        ? "stop.fill"
                                        : "play.fill")
        )
        if workspace.isManagedContainer {
            operations["editContainer"] = "editContainer"
            items.append(OuterframeContextMenuItem(id: "editContainer", title: "Edit Container…", isEnabled: !isPerformingWorkspaceOperation, systemImageName: "square.and.pencil"))
            operations["share"] = "share"
            items.append(
                OuterframeContextMenuItem(id: "share",
                                          title: "Share Container…",
                                          isEnabled: !isPerformingWorkspaceOperation,
                                          systemImageName: "square.and.arrow.up")
            )
        }
        items.append(contentsOf: [
            OuterframeContextMenuItem(id: "delete-separator",
                                      title: "",
                                      kind: .separator,
                                      isEnabled: false),
            OuterframeContextMenuItem(id: "delete",
                                      title: workspace.isManagedContainer
                                        ? "Delete Container…"
                                        : "Remove from Outer Shell…",
                                      isEnabled: !isPerformingWorkspaceOperation,
                                      systemImageName: workspace.isManagedContainer
                                        ? "trash"
                                        : "minus.circle")
        ])
        pendingWorkspaceOverviewMenuActions[menuID] = (workspace, operations, point)
        outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
    }

    private func showRecipeStepRemovalMenu(
        workspaceID: UUID,
        step: LocalWorkspaceRecord.Recipe.Step,
        at point: CGPoint
    ) {
        let menuID = UUID()
        pendingRecipeStepMenuSelections[menuID] = (
            workspaceID,
            step.id,
            ["remove": false, "remove-rebuild": true]
        )
        let items = [
            OuterframeContextMenuItem(
                id: "remove",
                title: "Remove from Recipe",
                isEnabled: !isPerformingWorkspaceOperation,
                systemImageName: "minus.circle"
            ),
            OuterframeContextMenuItem(
                id: "remove-rebuild",
                title: "Remove and Rebuild Container",
                isEnabled: !isPerformingWorkspaceOperation,
                systemImageName: "arrow.triangle.2.circlepath"
            )
        ]
        outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
    }

    private func showSafeSpaceAddAppMenu(for workspace: LocalWorkspaceRecord,
                                         at point: CGPoint) {
        guard let catalog = workspace.recipe?.catalog else { return }
        let menuID = UUID()
        var actions: [String: String] = [:]
        var items: [OuterframeContextMenuItem] = []

        let availableCatalogItems = catalog.filter { !$0.isInstalled }
        if !availableCatalogItems.isEmpty {
            items.append(OuterframeContextMenuItem(
                id: "software-heading",
                title: "Install an App",
                isEnabled: false
            ))
            for item in availableCatalogItems {
                let itemID = "install-\(item.id)"
                actions[itemID] = "installCatalog:\(item.id)"
                items.append(OuterframeContextMenuItem(
                    id: itemID,
                    title: item.displayName,
                    isEnabled: !isPerformingWorkspaceOperation,
                    systemImageName: "shippingbox"
                ))
            }
        }
        pendingSafeSpaceAppMenuSelections[menuID] = (workspace, actions)
        outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
    }

    private func showAppActionsMenu(for item: AppLauncherItem, at point: CGPoint) {
        if item.backend.isBackendsSelf {
            showBackendActionsMenu(for: item.backend, at: point)
            return
        }
        let menuID = UUID()
        if let context = item.containerContext {
            var operations = [
                "open": "containerOpen",
                "open-new-tab": "containerOpenNewTab",
                "open-new-window": "containerOpenNewWindow"
            ]
            var items = [
                OuterframeContextMenuItem(id: "open",
                                          title: "Open",
                                          isEnabled: true,
                                          systemImageName: "arrow.up.forward"),
                OuterframeContextMenuItem(id: "open-new-tab",
                                          title: "Open in New Tab",
                                          isEnabled: true,
                                          systemImageName: "plus.square.on.square"),
                OuterframeContextMenuItem(id: "open-new-window",
                                          title: "Open in New Window",
                                          isEnabled: true,
                                          systemImageName: "macwindow.badge.plus")
            ]
            items.append(OuterframeContextMenuItem(id: "container-separator",
                                                   title: "",
                                                   kind: .separator,
                                                   isEnabled: false))
            let controlItemID = context.app.isRunning ? "stop" : "start"
            operations[controlItemID] = context.app.isRunning
                ? "containerStop"
                : "containerStart"
            items.append(OuterframeContextMenuItem(
                id: controlItemID,
                title: context.app.isRunning ? "Stop" : "Start",
                isEnabled: true,
                systemImageName: context.app.isRunning ? "stop.fill" : "play.fill"
            ))
            if context.app.isRunning {
                operations["restart"] = "containerRestart"
                items.append(OuterframeContextMenuItem(id: "restart",
                                                       title: "Restart",
                                                       isEnabled: true,
                                                       systemImageName: "arrow.clockwise"))
            }
            operations["logs"] = "containerLogs"
            items.append(OuterframeContextMenuItem(id: "logs",
                                                   title: "Show Logs",
                                                   isEnabled: true,
                                                   systemImageName: "doc.plaintext"))
            items.append(OuterframeContextMenuItem(id: "visibility-separator",
                                                   title: "",
                                                   kind: .separator,
                                                   isEnabled: false))
            let isProminent = isAppProminent(item)
            let visibilityItemID = isProminent ? "move-to-more-apps" : "always-show"
            operations[visibilityItemID] = isProminent ? "moveToMoreApps" : "alwaysShow"
            items.append(OuterframeContextMenuItem(
                id: visibilityItemID,
                title: isProminent ? "Unpin" : "Pin to Top",
                isEnabled: true,
                systemImageName: isProminent ? "ellipsis" : "pin"
            ))

            pendingAppMenuActions[menuID] = (item, operations)
            outerframeHost.showContextMenu(menuID: menuID, items: items, at: point)
            return
        }
        var operationByItemID: [String: String] = [:]
        var items: [OuterframeContextMenuItem] = []
        let sectionLabelStyle = OuterframeContextMenuItemStyle(height: 23,
                                                               topInset: 4,
                                                               leftInset: 16,
                                                               bottomInset: 4,
                                                               rightInset: 8,
                                                               fontSize: 11,
                                                               fontWeight: Float32(NSFont.Weight.semibold.rawValue),
                                                               textColorRGBA: 0,
                                                               alignment: .left)

        func appendSeparatorIfNeeded() {
            guard !items.isEmpty, items.last?.kind != .separator else { return }
            items.append(OuterframeContextMenuItem(id: "separator-\(items.count)",
                                                   title: "",
                                                   kind: .separator,
                                                   isEnabled: false))
        }

        func appendEndpointSection(title: String,
                                   idPrefix: String,
                                   endpoint: AppLauncherEndpoint?,
                                   openOperation: String,
                                   openNewTabOperation: String,
                                   openNewWindowOperation: String) {
            guard let endpoint else { return }
            appendSeparatorIfNeeded()
            items.append(OuterframeContextMenuItem(id: "\(idPrefix)-heading",
                                                   title: title,
                                                   kind: .label,
                                                   isEnabled: false,
                                                   style: sectionLabelStyle))

            let openItemID = "\(idPrefix)-open"
            operationByItemID[openItemID] = openOperation
            items.append(OuterframeContextMenuItem(id: openItemID,
                                                   title: "Open",
                                                   isEnabled: true,
                                                   systemImageName: "arrow.up.forward"))

            let openNewTabItemID = "\(idPrefix)-open-new-tab"
            operationByItemID[openNewTabItemID] = openNewTabOperation
            items.append(OuterframeContextMenuItem(id: openNewTabItemID,
                                                   title: "Open in New Tab",
                                                   isEnabled: true,
                                                   systemImageName: "plus.square.on.square"))

            let openNewWindowItemID = "\(idPrefix)-open-new-window"
            operationByItemID[openNewWindowItemID] = openNewWindowOperation
            items.append(OuterframeContextMenuItem(id: openNewWindowItemID,
                                                   title: "Open in New Window",
                                                   isEnabled: true,
                                                   systemImageName: "macwindow.badge.plus"))

            if endpoint.backend.canControl && (endpointIsRunning(endpoint) || !endpointIsReadyToOpen(endpoint)) {
                let isRunning = endpointIsRunning(endpoint)
                let controlOperation = isRunning ? "stop" : "start"
                let controlItemID = "\(idPrefix)-\(controlOperation)"
                operationByItemID[controlItemID] = "\(controlOperation):\(endpoint.backend.serviceScope)"
                items.append(OuterframeContextMenuItem(id: controlItemID,
                                                       title: isRunning ? "Stop" : "Start",
                                                       isEnabled: true,
                                                       systemImageName: isRunning ? "stop.fill" : "play.fill"))
            }

            let logsItemID = "\(idPrefix)-logs"
            operationByItemID[logsItemID] = "showLogs:\(endpoint.backend.serviceID):\(endpoint.backend.serviceScope)"
            items.append(OuterframeContextMenuItem(id: logsItemID,
                                                   title: "Show logs",
                                                   isEnabled: true,
                                                   systemImageName: "doc.plaintext"))
        }

        func appendScriptItems() {
            var candidates: [(prefix: String, titlePrefix: String, endpoint: AppLauncherEndpoint)] = []
            if let userEndpoint = item.userEndpoint,
               userEndpoint.backend.scriptPath?.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false {
                candidates.append(("user", "User", userEndpoint))
            }
            if let rootEndpoint = item.rootEndpoint,
               rootEndpoint.backend.scriptPath?.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false {
                candidates.append(("root", "Root", rootEndpoint))
            }
            guard !candidates.isEmpty else { return }
            appendSeparatorIfNeeded()
            let needsScopeLabel = candidates.count > 1
            for candidate in candidates {
                let scope = candidate.endpoint.backend.serviceScope
                let editItemID = "\(candidate.prefix)-edit-script"
                if plaintextEndpoint(for: scope) != nil {
                    operationByItemID[editItemID] = "editScript:\(scope)"
                    items.append(OuterframeContextMenuItem(id: editItemID,
                                                           title: needsScopeLabel ? "Edit \(candidate.titlePrefix) Script" : "Edit Script",
                                                           isEnabled: true,
                                                           systemImageName: "square.and.pencil"))
                }

                let copyItemID = "\(candidate.prefix)-copy-script-path"
                operationByItemID[copyItemID] = "copyScriptPath:\(scope)"
                items.append(OuterframeContextMenuItem(id: copyItemID,
                                                       title: needsScopeLabel ? "Copy \(candidate.titlePrefix) Script Path" : "Copy Script Path",
                                                       isEnabled: true,
                                                       systemImageName: "doc.on.doc"))
            }
        }

        if item.backend.rootOnly ?? false {
            appendEndpointSection(title: "Root",
                                  idPrefix: "root",
                                  endpoint: item.rootEndpoint,
                                  openOperation: "runRoot",
                                  openNewTabOperation: "runRootNewTab",
                                  openNewWindowOperation: "runRootNewWindow")
        } else {
            appendEndpointSection(title: "User",
                                  idPrefix: "user",
                                  endpoint: item.userEndpoint,
                                  openOperation: "run",
                                  openNewTabOperation: "runNewTab",
                                  openNewWindowOperation: "runNewWindow")
            appendEndpointSection(title: "Root",
                                  idPrefix: "root",
                                  endpoint: item.rootEndpoint,
                                  openOperation: "runRoot",
                                  openNewTabOperation: "runRootNewTab",
                                  openNewWindowOperation: "runRootNewWindow")
        }

        appendScriptItems()

        appendSeparatorIfNeeded()
        let isProminent = isAppProminent(item)
        let visibilityItemID = isProminent ? "move-to-more-apps" : "always-show"
        operationByItemID[visibilityItemID] = isProminent ? "moveToMoreApps" : "alwaysShow"
        items.append(OuterframeContextMenuItem(
            id: visibilityItemID,
            title: isProminent ? "Unpin" : "Pin to Top",
            isEnabled: true,
            systemImageName: isProminent ? "ellipsis" : "pin"
        ))

        let management = backendManagementMenuItems(for: item.backend, includePlaceholderRunActions: false)
        var managementOperationByItemID = management.operationByItemID
        var managementItems = management.items
        if (item.backend.isBundled ?? false),
           (item.backend.supportsRoot ?? false),
           !(item.backend.rootOnly ?? false),
           !isDirectRootSession {
            let hasRootSupport = item.rootEndpoint != nil || (item.backend.hasRootSupport ?? false)
            managementOperationByItemID["rootSupport"] = hasRootSupport ? "removeRootSupport" : "addRootSupport"
            managementItems.removeAll { $0.id == "rootSupport" }
            managementItems.append(OuterframeContextMenuItem(id: "rootSupport",
                                                             title: hasRootSupport ? "Reinstall as user-only" : "Reinstall with root support",
                                                             isEnabled: true))
        }
        managementItems = managementItems.filter { $0.id == "uninstall" } + managementItems.filter { $0.id != "uninstall" }
        if !managementItems.isEmpty {
            appendSeparatorIfNeeded()
        }
        for menuItem in managementItems where operationByItemID[menuItem.id] == nil {
            guard let operation = managementOperationByItemID[menuItem.id] else { continue }
            operationByItemID[menuItem.id] = operation
            items.append(menuItem)
        }
        guard !items.isEmpty else { return }

        pendingAppMenuActions[menuID] = (item, operationByItemID)
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: items,
                                       at: point)
    }

    private func showLogSelectorMenu(at point: CGPoint) {
        guard let backend = selectedBackend(),
              backend.logFiles.count > 1 else { return }
        let menuID = UUID()
        var logIndexByItemID: [String: Int] = [:]
        let items = backend.logFiles.enumerated().map { index, logFile in
            let itemID = "log-\(index)"
            logIndexByItemID[itemID] = index
            let title = index == selectedLog?.logIndex
                ? "Current: \(logMenuTitle(for: logFile, index: index, in: backend.logFiles))"
                : logMenuTitle(for: logFile, index: index, in: backend.logFiles)
            return OuterframeContextMenuItem(id: itemID, title: title, isEnabled: true)
        }
        pendingLogMenuSelections[menuID] = (backend.serviceID, backend.serviceScope, logIndexByItemID)
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: items,
                                       at: point)
    }

    private func handleContextMenuSelection(menuID: UUID, itemID: String) {
        if let selection = pendingContainerCommandMenuActions.removeValue(forKey: menuID),
           let command = selection.commandByItemID[itemID] {
            copyTextToPasteboard(command)
            workspacePanelMessage = ""
            showCommandCopiedConfirmation(at: selection.anchor)
            return
        }
        if let selection = pendingShareScopeMenuActions.removeValue(forKey: menuID),
           let option = selection.options[itemID] {
            prepareSharedContainer(selection.workspace,
                                   includePersistentData: option.includePersistentData,
                                   includeMountedFolders: option.includeMountedFolders)
            return
        }
        if let selection = pendingImportRuntimeMenuActions.removeValue(forKey: menuID),
           let providerID = selection.providerByItemID[itemID] {
            importSharedContainer(at: selection.url, runtimeProviderID: providerID)
            return
        }
        if let paths = pendingContainerPathMenuSelections.removeValue(forKey: menuID),
           let path = paths[itemID] {
            copyTextToPasteboard(path)
            workspacePanelMessage = itemID == "server"
                ? "Copied the path on the server."
                : "Copied the path inside the container."
            updateLayout()
            return
        }
        if let selection = pendingSafeSpaceAppMenuSelections.removeValue(forKey: menuID),
           let kind = selection.kindByItemID[itemID] {
            let catalogPrefix = "installCatalog:"
            if kind.hasPrefix(catalogPrefix) {
                sendWorkspaceRequest(
                    operation: "installRecipeCatalogItem",
                    workspaceID: selection.workspace.id,
                    catalogItemID: String(kind.dropFirst(catalogPrefix.count))
                )
            }
            return
        }
        if let selection = pendingRecipeStepMenuSelections.removeValue(forKey: menuID),
           let rebuild = selection.rebuildByItemID[itemID] {
            sendWorkspaceRequest(operation: "deleteRecipeStep",
                                 workspaceID: selection.workspaceID,
                                 recipeStepID: selection.stepID,
                                 rebuild: rebuild)
            return
        }
        if let providers = pendingSafeSpaceProviderMenuSelections.removeValue(forKey: menuID),
           let providerID = providers[itemID] {
            createWorkspace(providerID: providerID)
            return
        }
        if let action = pendingWorkspaceOverviewMenuActions.removeValue(forKey: menuID),
           let operation = action.operationByItemID[itemID] {
            switch operation {
            case "delete":
                showWorkspaceDeletionConfirmation(action.workspace)
            case "editContainer":
                navigateToRecipeSafeSpace(action.workspace.id, pushHistory: true)
            case "share":
                showShareContainerOptions(for: action.workspace, at: action.anchor)
            case let value where value.hasPrefix("unmountFolder:"):
                let identifier = String(value.dropFirst("unmountFolder:".count))
                guard let mountID = UUID(uuidString: identifier) else {
                    return
                }
                sendWorkspaceRequest(operation: "unmountFolder",
                                     workspaceID: action.workspace.id,
                                     mountID: mountID)
            default:
                sendWorkspaceRequest(operation: operation,
                                     workspaceID: action.workspace.id)
            }
            return
        }

        if let menuSelection = pendingLogMenuSelections.removeValue(forKey: menuID),
           let logIndex = menuSelection.logIndexByItemID[itemID] {
            selectLog(serviceID: menuSelection.serviceID, serviceScope: menuSelection.serviceScope, logIndex: logIndex)
            return
        }

        if let appAction = pendingAppMenuActions.removeValue(forKey: menuID),
           let operation = appAction.operationByItemID[itemID] {
            performAppMenuAction(appAction.item, operation: operation)
            return
        }

        guard let menuAction = pendingMenuActions.removeValue(forKey: menuID),
              let operation = menuAction.operationByItemID[itemID] else {
            return
        }
        guard let backend = backends.first(where: {
            $0.serviceID == menuAction.serviceID && $0.serviceScope == menuAction.serviceScope
        }) else { return }
        if operation.hasPrefix("showLogs:") {
            guard let backend = backendForShowLogsOperation(operation) else { return }
            showLogs(for: backend)
            return
        }
        if operation == "toggleMenuBarVisibility" {
            let enabled = !(backend.menuBarVisibilityEnabled ?? true)
            performControlAction(for: backend, operation: enabled ? "showMenuBarWhenRunning" : "hideMenuBarWhenRunning")
            return
        }
        if operation == "aboutOuterShell" {
            showAboutPrompt(for: backend)
            return
        }
        performControlAction(for: backend, operation: operation)
    }

    private func frontendNavigationURL(_ endpoint: AppLauncherEndpoint) -> URL? {
        frontendNavigationURL(endpoint.frontend)
    }

    private func launcherNavigationURL(_ endpoint: AppLauncherEndpoint,
                                       socketPathOverride: String? = nil) -> URL? {
        guard let targetURL = frontendNavigationURL(
            endpoint.frontend,
            socketPathOverride: socketPathOverride
        ) else { return nil }
        let token = endpoint.frontend.iconObservationToken
        guard endpoint.frontend.iconCGImage == nil, endpoint.frontend.iconURL == nil,
              !token.isEmpty,
              let backendsEndpoint,
              var callbackComponents = URLComponents(url: backendsEndpoint, resolvingAgainstBaseURL: false) else {
            return targetURL
        }
        callbackComponents.path = "/api/icon-observation"
        callbackComponents.queryItems = [URLQueryItem(name: "token", value: token)]
        guard let callbackURL = callbackComponents.url else { return targetURL }

        var observationComponents = URLComponents()
        observationComponents.scheme = "outerloop"
        observationComponents.host = "observe-page-icon"
        observationComponents.queryItems = [
            URLQueryItem(name: "url", value: targetURL.absoluteString),
            URLQueryItem(name: "callback", value: callbackURL.absoluteString)
        ]
        return observationComponents.url ?? targetURL
    }

    private func frontendNavigationURL(_ frontend: FrontendRecord) -> URL? {
        frontendNavigationURL(frontend, socketPathOverride: nil)
    }

    private func frontendNavigationURL(_ frontend: FrontendRecord,
                                       socketPathOverride: String?) -> URL? {
        let socketPath = (socketPathOverride ?? frontend.socketPath)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if !socketPath.isEmpty {
            let path = pathAndQuery(fromFrontendURL: frontend.url, socketPath: socketPath)
            return URL(string: "http+unix://\(percentEncodedSocketPath(socketPath))\(path)")
        }

        let rawURL = frontend.url.trimmingCharacters(in: .whitespacesAndNewlines)
        if frontend.port > 0 {
            let path = pathAndQuery(fromFrontendURL: rawURL, socketPath: nil)
            return URL(string: "http://127.0.0.1:\(frontend.port)\(path)")
        }
        if let parsed = URL(string: rawURL), parsed.scheme != nil {
            return parsed
        }
        return URL(string: rawURL)
    }

    private func pathAndQuery(fromFrontendURL rawURL: String, socketPath: String?) -> String {
        let trimmed = rawURL.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return "/" }

        if let socketPath = socketPath?.trimmingCharacters(in: .whitespacesAndNewlines),
           !socketPath.isEmpty {
            if trimmed == socketPath { return "/" }
            if trimmed.hasPrefix(socketPath) {
                let suffix = String(trimmed.dropFirst(socketPath.count))
                return normalizedPathAndQuery(suffix)
            }
        }

        if trimmed.lowercased().hasPrefix("http+unix://") {
            let prefixLength = "http+unix://".count
            let startIndex = trimmed.index(trimmed.startIndex, offsetBy: prefixLength)
            let authorityAndSuffix = String(trimmed[startIndex...])
            let suffixStart = authorityAndSuffix.firstIndex(where: { $0 == "/" || $0 == "?" || $0 == "#" }) ?? authorityAndSuffix.endIndex
            return normalizedPathAndQuery(String(authorityAndSuffix[suffixStart...]))
        }

        if let components = URLComponents(string: trimmed), components.scheme != nil {
            var path = components.path.isEmpty ? "/" : components.path
            if let query = components.query, !query.isEmpty {
                path += "?\(query)"
            }
            return normalizedPathAndQuery(path)
        }

        guard let slashIndex = trimmed.firstIndex(of: "/") else { return "/" }
        return normalizedPathAndQuery(String(trimmed[slashIndex...]))
    }

    private func normalizedPathAndQuery(_ value: String) -> String {
        if value.isEmpty { return "/" }
        if value.hasPrefix("/") { return value }
        if value.hasPrefix("?") || value.hasPrefix("#") { return "/\(value)" }
        return "/\(value)"
    }

    private func percentEncodedSocketPath(_ socketPath: String) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return socketPath.addingPercentEncoding(withAllowedCharacters: allowed) ?? socketPath
    }

    private func actionProgressText(operation: String, backend: BackendRecord) -> String {
        switch operation {
        case "run":
            return "Installing \(backend.displayName)..."
        case "runRoot", "installRoot", "addRootSupport":
            return "Adding root support for \(backend.displayName)..."
        case "runUser", "installUser":
            return "Installing \(backend.displayName)..."
        case "removeRootSupport":
            return "Removing root support from \(backend.displayName)..."
        case "uninstall":
            return "Uninstalling \(backend.displayName)..."
        case "uninstallOuterShell":
            return "Uninstalling \(backend.displayName)..."
        case "checkUpdate":
            return "Checking for \(backend.displayName) updates..."
        case "update":
            return "Updating \(backend.displayName)..."
        case "showMenuBarWhenRunning":
            return "Showing \(backend.displayName) in the menu bar..."
        case "hideMenuBarWhenRunning":
            return "Hiding \(backend.displayName) from the menu bar..."
        case "start":
            return "Starting \(backend.displayName)..."
        case "stop":
            return "Stopping \(backend.displayName)..."
        default:
            return "\(operation.capitalized)ing \(backend.displayName)..."
        }
    }

    private func formEncodedBody(_ values: [String: String]) -> Data {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-._* "))
        let pairs = values.map { key, value -> String in
            let encodedKey = key.addingPercentEncoding(withAllowedCharacters: allowed)?.replacingOccurrences(of: " ", with: "+") ?? key
            let encodedValue = value.addingPercentEncoding(withAllowedCharacters: allowed)?.replacingOccurrences(of: " ", with: "+") ?? value
            return "\(encodedKey)=\(encodedValue)"
        }
        return pairs.joined(separator: "&").data(using: .utf8) ?? Data()
    }

    private func suggestedIdentifier(from value: String) -> String {
        let scalars = value.lowercased().unicodeScalars
        var result = ""
        var lastWasDash = false
        for scalar in scalars {
            let character = Character(scalar)
            if character.isLetter || character.isNumber || scalar == "." || scalar == "_" {
                result.append(character)
                lastWasDash = false
            } else if !lastWasDash && !result.isEmpty {
                result.append("-")
                lastWasDash = true
            }
        }
        while result.last == "-" {
            result.removeLast()
        }
        return result
    }

    private func makeTextLayer(size: CGFloat,
                               weight: NSFont.Weight,
                               color: NSColor,
                               alignment: CATextLayerAlignmentMode = .left,
                               monospaced: Bool = false,
                               italic: Bool = false) -> CATextLayer {
        let layer = CATextLayer()
        layer.contentsScale = 2
        var font = monospaced ? NSFont.monospacedSystemFont(ofSize: size, weight: weight) : NSFont.systemFont(ofSize: size, weight: weight)
        if italic {
            let descriptor = font.fontDescriptor.withSymbolicTraits(.italic)
            if let italicFont = NSFont(descriptor: descriptor, size: size) {
                font = italicFont
            }
        }
        layer.font = font
        layer.fontSize = size
        layer.foregroundColor = resolvedCGColor(color)
        layer.truncationMode = .end
        layer.alignmentMode = alignment
        return layer
    }

    private func configureTextLayer(_ layer: CATextLayer,
                                    title: String,
                                    fontSize: CGFloat,
                                    weight: NSFont.Weight,
                                    color: NSColor,
                                    alignment: CATextLayerAlignmentMode,
                                    isWrapped: Bool,
                                    italic: Bool = false,
                                    monospaced: Bool = false) {
        var font = monospaced ? NSFont.monospacedSystemFont(ofSize: fontSize, weight: weight) : NSFont.systemFont(ofSize: fontSize, weight: weight)
        if italic {
            let descriptor = font.fontDescriptor.withSymbolicTraits(.italic)
            if let italicFont = NSFont(descriptor: descriptor, size: fontSize) {
                font = italicFont
            }
        }
        layer.string = title
        layer.font = font
        layer.fontSize = fontSize
        layer.foregroundColor = resolvedCGColor(color)
        layer.alignmentMode = alignment
        layer.isWrapped = isWrapped
        layer.truncationMode = isWrapped ? .none : .end
    }

    private func makeButtonLayer(title: String, emphasized: Bool) -> CenteredButtonLayer {
        let layer = CenteredButtonLayer(title: title)
        layer.applyStyle(textCGColor: resolvedCGColor(emphasized ? .white : .controlAccentColor),
                         backgroundCGColor: resolvedCGColor(emphasized ? .controlAccentColor : NSColor.controlAccentColor.withAlphaComponent(0.12)),
                         font: NSFont.systemFont(ofSize: 12, weight: .medium))
        return layer
    }

    private func makeSymbolButtonLayer(symbolName: String, accessibilityTitle: String) -> SymbolButtonLayer {
        let layer = SymbolButtonLayer(symbolName: symbolName, accessibilityTitle: accessibilityTitle)
        layer.applyStyle(tintCGColor: resolvedCGColor(.secondaryLabelColor),
                         backgroundCGColor: resolvedCGColor(.clear))
        return layer
    }

    private func cgImage(for image: NSImage) -> CGImage? {
        var rect = NSRect(origin: .zero, size: image.size)
        return image.cgImage(forProposedRect: &rect, context: nil, hints: nil)
    }

    private func folderIconCGImage(for url: URL, pointSize: CGFloat) -> CGImage? {
        var output: CGImage?
        withEffectiveAppearance {
            let scale = max(NSScreen.main?.backingScaleFactor ?? 2, 1)
            let pixelSize = pointSize * scale
            let icon = NSWorkspace.shared.icon(forFile: url.path)
            let canvas = NSImage(size: NSSize(width: pixelSize, height: pixelSize))
            canvas.lockFocus()
            icon.draw(in: NSRect(origin: .zero, size: canvas.size),
                      from: .zero,
                      operation: .sourceOver,
                      fraction: 1)
            canvas.unlockFocus()
            output = cgImage(for: canvas)
        }
        return output
    }

    private func nativeProjectDragPreview(for project: GeneratedNativeAppProject) -> NativeProjectDragPreview? {
        guard let projectURL = project.projectURL else { return nil }
        let scale = max(NSScreen.main?.backingScaleFactor ?? 2, 1)
        let iconSize: CGFloat = 96
        let labelHeight: CGFloat = 24
        let iconLabelGap: CGFloat = 12
        let font = NSFont.systemFont(ofSize: 13, weight: .medium)
        let measuredNameWidth = ceil((project.folderName as NSString).size(withAttributes: [.font: font]).width)
        let labelWidth = min(max(measuredNameWidth + 18, 36), 280)
        let width = max(iconSize, labelWidth)
        let height = iconSize + iconLabelGap + labelHeight

        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil,
                                            pixelsWide: max(Int(ceil(width * scale)), 1),
                                            pixelsHigh: max(Int(ceil(height * scale)), 1),
                                            bitsPerSample: 8,
                                            samplesPerPixel: 4,
                                            hasAlpha: true,
                                            isPlanar: false,
                                            colorSpaceName: .deviceRGB,
                                            bytesPerRow: 0,
                                            bitsPerPixel: 0),
              let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
            return nil
        }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        context.cgContext.scaleBy(x: scale, y: scale)
        defer {
            NSGraphicsContext.restoreGraphicsState()
        }

        withEffectiveAppearance {
            NSColor.clear.setFill()
            NSBezierPath(rect: NSRect(x: 0, y: 0, width: width, height: height)).fill()

            let icon = NSWorkspace.shared.icon(forFile: projectURL.path)
            icon.draw(in: NSRect(x: (width - iconSize) / 2,
                                 y: labelHeight + iconLabelGap,
                                 width: iconSize,
                                 height: iconSize),
                      from: .zero,
                      operation: .sourceOver,
                      fraction: 1)

            let labelFrame = NSRect(x: (width - labelWidth) / 2,
                                    y: 0,
                                    width: labelWidth,
                                    height: labelHeight)
            NSColor.controlAccentColor.setFill()
            NSBezierPath(roundedRect: labelFrame.insetBy(dx: 0, dy: 2),
                         xRadius: 6,
                         yRadius: 6).fill()

            let paragraph = NSMutableParagraphStyle()
            paragraph.alignment = .center
            paragraph.lineBreakMode = .byTruncatingTail
            let attributes: [NSAttributedString.Key: Any] = [
                .font: font,
                .foregroundColor: NSColor.alternateSelectedControlTextColor,
                .paragraphStyle: paragraph
            ]
            (project.folderName as NSString).draw(with: labelFrame.insetBy(dx: 9, dy: 4),
                                                  options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine],
                                                  attributes: attributes)
        }

        guard let pngData = bitmap.representation(using: .png, properties: [:]) else {
            return nil
        }
        return NativeProjectDragPreview(
            pngData: pngData,
            size: CGSize(width: width, height: height),
            frameOrigin: CGPoint(x: nativeProjectDragFrame.midX - width / 2,
                                 y: nativeProjectDragFrame.minY)
        )
    }

    private func sharedContainerDragPreview(
        for file: (url: URL, name: String, byteCount: Int)
    ) -> NativeProjectDragPreview? {
        let scale = max(NSScreen.main?.backingScaleFactor ?? 2, 1)
        let iconSize: CGFloat = 72
        let labelHeight: CGFloat = 24
        let iconLabelGap: CGFloat = 8
        let font = NSFont.systemFont(ofSize: 13, weight: .medium)
        let measuredNameWidth = ceil((file.name as NSString).size(withAttributes: [.font: font]).width)
        let labelWidth = min(max(measuredNameWidth + 18, 80), 320)
        let width = max(iconSize, labelWidth)
        let height = iconSize + iconLabelGap + labelHeight

        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil,
                                            pixelsWide: max(Int(ceil(width * scale)), 1),
                                            pixelsHigh: max(Int(ceil(height * scale)), 1),
                                            bitsPerSample: 8,
                                            samplesPerPixel: 4,
                                            hasAlpha: true,
                                            isPlanar: false,
                                            colorSpaceName: .deviceRGB,
                                            bytesPerRow: 0,
                                            bitsPerPixel: 0),
              let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
            return nil
        }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        context.cgContext.scaleBy(x: scale, y: scale)
        defer { NSGraphicsContext.restoreGraphicsState() }

        withEffectiveAppearance {
            NSColor.clear.setFill()
            NSBezierPath(rect: NSRect(x: 0, y: 0, width: width, height: height)).fill()

            let symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 58, weight: .regular)
                .applying(NSImage.SymbolConfiguration(hierarchicalColor: .controlAccentColor))
            if let icon = NSImage(systemSymbolName: "shippingbox.fill", accessibilityDescription: nil)?
                .withSymbolConfiguration(symbolConfiguration) {
                icon.draw(in: NSRect(x: (width - iconSize) / 2,
                                     y: labelHeight + iconLabelGap,
                                     width: iconSize,
                                     height: iconSize),
                          from: .zero,
                          operation: .sourceOver,
                          fraction: 1)
            }

            let labelFrame = NSRect(x: (width - labelWidth) / 2,
                                    y: 0,
                                    width: labelWidth,
                                    height: labelHeight)
            NSColor.controlAccentColor.setFill()
            NSBezierPath(roundedRect: labelFrame.insetBy(dx: 0, dy: 2),
                         xRadius: 6,
                         yRadius: 6).fill()

            let paragraph = NSMutableParagraphStyle()
            paragraph.alignment = .center
            paragraph.lineBreakMode = .byTruncatingTail
            let attributes: [NSAttributedString.Key: Any] = [
                .font: font,
                .foregroundColor: NSColor.alternateSelectedControlTextColor,
                .paragraphStyle: paragraph
            ]
            (file.name as NSString).draw(with: labelFrame.insetBy(dx: 9, dy: 4),
                                         options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine],
                                         attributes: attributes)
        }

        guard let pngData = bitmap.representation(using: .png, properties: [:]) else {
            return nil
        }
        return NativeProjectDragPreview(
            pngData: pngData,
            size: CGSize(width: width, height: height),
            frameOrigin: CGPoint(x: sharedContainerDragFrame.midX - width / 2,
                                 y: sharedContainerDragFrame.minY)
        )
    }

    private func symbolCGImage(named symbolName: String,
                               pointSize: CGFloat,
                               color: NSColor = .controlAccentColor) -> CGImage? {
        var output: CGImage?
        withEffectiveAppearance {
            let scale: CGFloat = 2
            let pixelSize = pointSize * scale
            guard let image = NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)?
                    .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: pixelSize, weight: .regular)) else {
                return
            }
            let canvas = NSImage(size: NSSize(width: pixelSize, height: pixelSize))
            canvas.lockFocus()
            color.setFill()
            NSRect(origin: .zero, size: canvas.size).fill()
            image.draw(in: NSRect(origin: .zero, size: canvas.size),
                       from: .zero,
                       operation: .destinationIn,
                       fraction: 1)
            canvas.unlockFocus()
            output = cgImage(for: canvas)
        }
        return output
    }

    private func naturalSymbolCGImage(named symbolName: String,
                                      pointSize: CGFloat,
                                      color: NSColor = .white) -> (image: CGImage, size: CGSize)? {
        var output: CGImage?
        var displaySize = CGSize.zero
        withEffectiveAppearance {
            let scale: CGFloat = 2
            guard let image = NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)?
                    .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: pointSize * scale, weight: .regular)) else {
                return
            }

            let canvasSize = NSSize(width: ceil(image.size.width), height: ceil(image.size.height))
            guard canvasSize.width > 0, canvasSize.height > 0 else { return }

            let canvas = NSImage(size: canvasSize)
            canvas.lockFocus()
            color.setFill()
            NSRect(origin: .zero, size: canvasSize).fill()
            image.draw(in: NSRect(origin: .zero, size: canvasSize),
                       from: .zero,
                       operation: .destinationIn,
                       fraction: 1)
            canvas.unlockFocus()

            output = cgImage(for: canvas)
            displaySize = CGSize(width: ceil(canvasSize.width / scale),
                                 height: ceil(canvasSize.height / scale))
        }
        guard let output else { return nil }
        return (image: output, size: displaySize)
    }

    private func alternatingRowColors() -> (even: CGColor, odd: CGColor) {
        let even = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.26))
        let odd = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.08))
        return (even, odd)
    }
}

private final class SymbolButtonLayer: CALayer {
    private let symbolName: String
    private let accessibilityTitle: String
    private var tintCGColor = CGColor(gray: 0.5, alpha: 1)

    init(symbolName: String, accessibilityTitle: String) {
        self.symbolName = symbolName
        self.accessibilityTitle = accessibilityTitle
        super.init()
        cornerRadius = 5
        masksToBounds = true
        contentsScale = 2
        needsDisplayOnBoundsChange = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func applyStyle(tintCGColor: CGColor, backgroundCGColor: CGColor) {
        self.tintCGColor = tintCGColor
        self.backgroundColor = backgroundCGColor
        setNeedsDisplay()
    }

    override func draw(in context: CGContext) {
        guard bounds.width > 2, bounds.height > 2 else { return }
        let pointSize: CGFloat = 13
        let configuration = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .medium)
        let image = NSImage(systemSymbolName: symbolName, accessibilityDescription: accessibilityTitle)?
            .withSymbolConfiguration(configuration)
        guard let image else { return }

        let imageSize = image.size
        let drawSize = CGSize(width: min(imageSize.width, bounds.width - 8),
                              height: min(imageSize.height, bounds.height - 8))
        let drawRect = CGRect(x: floor((bounds.width - drawSize.width) / 2),
                              y: floor((bounds.height - drawSize.height) / 2),
                              width: drawSize.width,
                              height: drawSize.height)

        guard let cgImage = Self.symbolMaskCGImage(named: symbolName,
                                                   accessibilityTitle: accessibilityTitle,
                                                   pointSize: pointSize,
                                                   drawSize: drawSize,
                                                   scale: max(contentsScale, 1)) else { return }

        context.saveGState()
        context.clip(to: drawRect, mask: cgImage)
        context.setFillColor(tintCGColor)
        context.fill(drawRect)
        context.restoreGState()
    }

    private static func symbolMaskCGImage(named symbolName: String,
                                          accessibilityTitle: String,
                                          pointSize: CGFloat,
                                          drawSize: CGSize,
                                          scale: CGFloat) -> CGImage? {
        let pixelWidth = max(Int(ceil(drawSize.width * scale)), 1)
        let pixelHeight = max(Int(ceil(drawSize.height * scale)), 1)
        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil,
                                            pixelsWide: pixelWidth,
                                            pixelsHigh: pixelHeight,
                                            bitsPerSample: 8,
                                            samplesPerPixel: 4,
                                            hasAlpha: true,
                                            isPlanar: false,
                                            colorSpaceName: .deviceRGB,
                                            bytesPerRow: 0,
                                            bitsPerPixel: 0) else {
            return nil
        }
        bitmap.size = NSSize(width: drawSize.width, height: drawSize.height)

        guard let graphicsContext = NSGraphicsContext(bitmapImageRep: bitmap) else { return nil }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = graphicsContext
        graphicsContext.imageInterpolation = .high
        defer {
            NSGraphicsContext.restoreGraphicsState()
        }

        guard let image = NSImage(systemSymbolName: symbolName, accessibilityDescription: accessibilityTitle)?
            .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: pointSize, weight: .medium)) else {
            return nil
        }

        NSColor.white.setFill()
        NSRect(origin: .zero, size: drawSize).fill()
        image.draw(in: NSRect(origin: .zero, size: drawSize),
                   from: .zero,
                   operation: .destinationIn,
                   fraction: 1)

        return bitmap.cgImage
    }
}

@MainActor
private final class FilePickerScrollbarDelegate: ScrollbarControllerDelegate {
    private weak var owner: BackendsHandler?

    init(owner: BackendsHandler) {
        self.owner = owner
    }

    func scrollbarDidChangeScrollOffset(_ offset: CGFloat) {
        owner?.setFilePickerScroll(offset)
    }
}

private final class CenteredButtonLayer: CALayer {
    private var textCGColor = CGColor(gray: 0, alpha: 1)
    private var font = NSFont.systemFont(ofSize: 12, weight: .medium)

    var title: String {
        didSet {
            setNeedsDisplay()
        }
    }

    init(title: String) {
        self.title = title
        super.init()
        cornerRadius = 5
        masksToBounds = true
        contentsScale = 2
        needsDisplayOnBoundsChange = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func applyStyle(textCGColor: CGColor, backgroundCGColor: CGColor, font: NSFont) {
        self.backgroundColor = backgroundCGColor
        self.textCGColor = textCGColor
        self.font = font
        setNeedsDisplay()
    }

    override func draw(in context: CGContext) {
        guard bounds.width > 2, bounds.height > 2 else { return }

        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = .center
        let attributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: NSColor(cgColor: textCGColor) ?? NSColor.labelColor,
            .paragraphStyle: paragraph
        ]
        let attributedTitle = NSAttributedString(string: title, attributes: attributes)
        let originalLine = CTLineCreateWithAttributedString(attributedTitle)
        let token = CTLineCreateWithAttributedString(NSAttributedString(string: "...", attributes: attributes))
        let availableWidth = max(bounds.width - 14, 1)
        let line = CTLineCreateTruncatedLine(originalLine, Double(availableWidth), .end, token) ?? originalLine

        var ascent: CGFloat = 0
        var descent: CGFloat = 0
        var leading: CGFloat = 0
        let lineWidth = CGFloat(CTLineGetTypographicBounds(line, &ascent, &descent, &leading))
        let x = floor((bounds.width - min(lineWidth, availableWidth)) / 2)
        let baselineY = floor((bounds.height - ascent - descent) / 2 + descent)

        context.saveGState()
        context.textMatrix = .identity
        context.textPosition = CGPoint(x: x, y: baselineY)
        CTLineDraw(line, context)
        context.restoreGState()
    }
}

private func withoutImplicitAnimations(_ body: () -> Void) {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    body()
    CATransaction.commit()
}
