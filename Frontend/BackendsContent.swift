import AppKit
import CoreText
import Foundation
import QuartzCore

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

private struct BackendsResponse: Decodable {
    let error: String
    let backends: [BackendRecord]
}

private struct BackendRecord: Decodable {
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
    let iconSymbolName: String?
    let launchdPlistPath: String
    let ownsLaunchdPlist: Bool
    let frontends: [FrontendRecord]
    let logFiles: [LogFileRecord]

    var isBundledPlaceholder: Bool {
        (isBundled ?? false) && !(isInstalled ?? true)
    }

    var canUninstallBackend: Bool {
        canUninstall ?? false
    }

    var isBackendsSelf: Bool {
        serviceID == "org.outershell.OuterShell" || serviceID == "dev.outergroup.Navigator" || serviceID == "dev.outergroup.Backends"
    }

    var isMigrationAction: Bool {
        isMigration ?? false
    }

    var pathText: String {
        if let serviceUnitPath,
           !serviceUnitPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return serviceUnitPath
        }
        if !launchdPlistPath.isEmpty {
            return launchdPlistPath
        }
        if !serviceUnit.isEmpty {
            return serviceUnit
        }
        return "--"
    }
}

private struct FrontendRecord: Decodable {
    let id: String
    let name: String
    let url: String
    let port: Int
    let socketPath: String
    let iconPath: String?
    let iconData: Data?
    let list: String?
    let isRunning: Bool

    var hasEndpoint: Bool {
        if !socketPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || port > 0 {
            return true
        }
        let trimmedURL = url.trimmingCharacters(in: .whitespacesAndNewlines)
        return URL(string: trimmedURL)?.scheme != nil
    }

    var iconImage: NSImage? {
        guard let iconData,
              !iconData.isEmpty else {
            return nil
        }
        return NSImage(data: iconData)
    }

    var listName: String {
        list?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

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
}

private struct EventResponse {
    let backendsChanged: Bool
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
                             iconSymbolName: emptyToNil(try reader.stringRef(at: 48)),
                             launchdPlistPath: try reader.stringRef(at: 56),
                             ownsLaunchdPlist: (flags & 0x20) != 0,
                             frontends: try reader.child(at: 68).payloadArray().map(FrontendRecord.decodeBinary),
                             logFiles: try reader.child(at: 76).payloadArray().map(LogFileRecord.decodeBinary))
    }
}

private extension FrontendRecord {
    static func decodeBinary(_ reader: BinaryPayloadReader) throws -> FrontendRecord {
        let iconData = try reader.dataRef(at: 32)
        let id = reader.data.count >= 60 ? try reader.stringRef(at: 52) : ""
        let flags = reader.data.count >= 64 ? try reader.uint32(at: 60) : 1
        return FrontendRecord(id: id,
                              name: try reader.stringRef(at: 0),
                              url: try reader.stringRef(at: 8),
                              port: Int(try reader.uint32(at: 48)),
                              socketPath: try reader.stringRef(at: 16),
                              iconPath: emptyToNil(try reader.stringRef(at: 24)),
                              iconData: iconData.isEmpty ? nil : iconData,
                              list: emptyToNil(try reader.stringRef(at: 40)),
                              isRunning: (flags & 0x01) != 0)
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
                              needsPassword: (flags & 0x02) != 0)
    }
}

private extension EventResponse {
    static func decodeBinary(_ data: Data) throws -> EventResponse {
        let reader = BinaryPayloadReader(data: data)
        let flags = try reader.uint32(at: 0)
        return EventResponse(backendsChanged: (flags & 0x01) != 0,
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
        FilePickerEntryRecord(name: try reader.stringRef(at: 0),
                              path: try reader.stringRef(at: 8),
                              isDirectory: (try reader.uint32(at: 16) & 0x01) != 0,
                              size: try reader.uint64(at: 20),
                              modified: try reader.double(at: 28))
    }
}

private func emptyToNil(_ value: String) -> String? {
    value.isEmpty ? nil : value
}

private struct LogSelection: Equatable {
    let serviceID: String
    let logIndex: Int
}

private enum BackendsViewMode {
    case apps
    case create
}

private struct AppLauncherEndpoint {
    let backend: BackendRecord
    let frontend: FrontendRecord
    let frontendIndex: Int
}

private struct AppLauncherItem {
    let identityKey: String
    let primaryEndpoint: AppLauncherEndpoint
    let userEndpoint: AppLauncherEndpoint?
    let rootEndpoint: AppLauncherEndpoint?

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

    var iconImage: NSImage? {
        frontend.iconImage
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

private struct BackendListRow {
    let backend: BackendRecord
    let frontend: FrontendRecord?
    let frontendIndex: Int?
    let isFrontendChild: Bool

    var serviceID: String { backend.serviceID }

    var iconKey: String? {
        guard let frontendIndex,
              let frontend else { return nil }
        return frontendIdentityKey(backend: backend, frontend: frontend, frontendIndex: frontendIndex)
    }

    var rowID: String {
        if let frontendIndex {
            return "\(backendIdentityKey(backend)):frontend:\(frontendIndex)"
        }
        return "\(backendIdentityKey(backend)):backend"
    }
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
    let operation: String
    let displayName: String
}

private struct IconMatchState {
    let frame: CGRect
    let image: NSImage?
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
}

private struct PendingCreateTextDrag {
    let startPoint: CGPoint
    let cursorIndex: Int
    let selectedText: String
}

private enum AppDropTarget: Equatable {
    case unlisted
    case list(String)

    var listName: String {
        switch self {
        case .unlisted:
            return ""
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

private enum FilePickerMode {
    case saveFile
    case chooseDirectory
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
    let recipeID: String?
    let targetFieldKey: String?
    let fileExtension: String
    var directory: String
    var parent: String
    var filename: String
    var entries: [FilePickerEntryRecord]
    var isLoading: Bool
    var error: String
}

@MainActor
private final class BackendsHandler: NSObject, OuterframeHostDelegate, SingleLineTextInputControllerDelegate, ScrollbarControllerDelegate {
    private let outerframeHost: OuterframeHost
    private let appConnection: OuterframeAppConnection
    private var retainedSelf: BackendsHandler?
    private var appearance: NSAppearance?
    private var currentSize = CGSize(width: 900, height: 620)
    private var urlSession: URLSession?
    private var backendsEndpoint: URL?
    private var logsEndpoint: URL?
    private var controlEndpoint: URL?
    private var createEndpoint: URL?
    private var recipesEndpoint: URL?
    private var filePickerEndpoint: URL?
    private var eventsEndpoint: URL?
    private var eventWatchTask: URLSessionDataTask?
    private var eventWatchGeneration = 0
    private var eventWatchRetryDelay: TimeInterval = 1
    private var backendsEventVersion: UInt64 = 0
    private var logEventVersion: UInt64 = 0
    private var didRegisterLayer = false

    private var backends: [BackendRecord] = []
    private var backendError = ""
    private var selectedServiceID: String?
    private var selectedLog: LogSelection?
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
    private var backendScroll: CGFloat = 0
    private var logScroll: CGFloat = 0
    private var shouldScrollLogToBottomOnNextLayout = false
    private var logRenderedText = ""
    private var logAttributedText = NSAttributedString(string: "")
    private var renderedLogHeaderDetailText = ""
    private var logHeaderDetailFrame = CGRect.zero
    private var logDismissFrame = CGRect.zero
    private var logHeaderDetailSelectionRange: NSRange?
    private var logHeaderDetailDragAnchorOffset: Int?
    private var logTextSelectionRange: NSRange?
    private var logDragAnchorOffset: Int?
    private var lastLogDragTextPoint: CGPoint?
    private var logTextSelectionLayers: [CALayer] = []
    private var logTextFragmentLayers: [ObjectIdentifier: LogTextFragmentLayer] = [:]
    private var logTextLayoutWidth: CGFloat = 0
    private var logTextContentGeneration = 0
    private var logEstimatedContentHeightCache: (generation: Int, textWidth: CGFloat, height: CGFloat)?
    private var logVisualLineCache: (generation: Int, textWidth: CGFloat, metrics: LogVisualLineMetrics)?
    private var logTextFragmentCoverage: (generation: Int, textWidth: CGFloat, contentHeight: CGFloat, rect: CGRect)?
    private var logTextSelectionCoverage: (generation: Int, textWidth: CGFloat, contentHeight: CGFloat, range: NSRange, rect: CGRect)?
    private var logScrollbarController: ScrollbarController<BackendsHandler>?
    private let logContentStorage = NSTextContentStorage()
    private let logTextLayoutManager = NSTextLayoutManager()
    private let logTextContainer = NSTextContainer(size: CGSize(width: 320, height: 1_000_000))
    private var appsScroll: CGFloat = 0
    private var createScroll: CGFloat = 0
    private var createContentBottom: CGFloat = 0
    private var viewHasFocus = true
    private var windowIsActive = true
    private var isSynchronizingCreateInput = false
    private var isSynchronizingPasswordInput = false
    private var pendingCreateTextDrag: PendingCreateTextDrag?
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
        controller.delegate = self
        controller.onSubmit = { [weak self] in
            Task { @MainActor in self?.submitPasswordPrompt() }
        }
        return controller
    }()
    private var sudoPasswordInput = ""
    private var sudoPasswordMessage = ""
    private var pendingFilePicker: PendingFilePicker?
    private static let filePickerFilenameKey = "__filePickerFilename"

    private let rootLayer = CALayer()
    private let toolbarLayer = CALayer()
    private let titleLayer = CATextLayer()
    private let statusLayer = CATextLayer()
    private let outerShellActionLayer = SymbolButtonLayer(symbolName: "ellipsis.circle", accessibilityTitle: "Outer Shell Actions")
    private let contentLayer = CALayer()
    private let appsLayer = CALayer()
    private let appsScrollContentLayer = CALayer()
    private let appsOverlayLayer = CALayer()
    private let tableHeaderLayer = CALayer()
    private let rowsClipLayer = CALayer()
    private let backendRowsContentLayer = CALayer()
    private let logHeaderLayer = CALayer()
    private let logRowsClipLayer = CALayer()
    private let logTextContentLayer = CALayer()
    private let logTextSelectionLayer = CALayer()
    private let dividerLayer = CALayer()
    private let createLayer = CALayer()
    private let iconTransitionLayer = CALayer()
    private let installOverlayLayer = CALayer()
    private let passwordOverlayLayer = CALayer()
    private let filePickerOverlayLayer = CALayer()

    private var newBackendRowFrame = CGRect.zero
    private var appCardFrames: [(frame: CGRect, item: AppLauncherItem)] = []
    private var appBadgeFrames: [AppLauncherBadgeTarget] = []
    private var appListDropFrames: [(frame: CGRect, listName: String)] = []
    private var appUnlistedDropFrames: [CGRect] = []
    private var addAppFrame = CGRect.zero
    private var outerShellActionFrame = CGRect.zero
    private var appsContentBottom: CGFloat = 0
    private var pendingAppDrag: PendingAppDrag?
    private var backendRowFrames: [(frame: CGRect, row: BackendListRow)] = []
    private var backendActionFrames: [(frame: CGRect, row: BackendListRow, operation: String)] = []
    private var iconMatchStates: [String: IconMatchState] = [:]
    private var iconMatchLayers: [String: CALayer] = [:]
    private var textMatchStates: [String: TextMatchState] = [:]
    private var textMatchLayers: [String: CATextLayer] = [:]
    private var pendingMenuActions: [UUID: (serviceID: String, operationByItemID: [String: String])] = [:]
    private var pendingAppMenuActions: [UUID: (item: AppLauncherItem, operationByItemID: [String: String])] = [:]
    private var pendingLogMenuSelections: [UUID: (serviceID: String, logIndexByItemID: [String: Int])] = [:]
    private var logSelectorFrame = CGRect.zero
    private var recipeFrames: [(frame: CGRect, recipeID: String)] = []
    private var bundledAppInstallFrames: [(frame: CGRect, backend: BackendRecord)] = []
    private var bundledAppMenuFrames: [(frame: CGRect, backend: BackendRecord)] = []
    private var createFieldFrames: [(frame: CGRect, key: String)] = []
    private var createFieldLayouts: [String: CreateFieldLayout] = [:]
    private var createChoiceFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createSuggestionFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createDirectorySelectFrames: [(frame: CGRect, key: String)] = []
    private var createButtonFrame = CGRect.zero
    private var cancelCreateFrame = CGRect.zero
    private var passwordFieldFrame = CGRect.zero
    private var passwordTextFrame = CGRect.zero
    private var passwordSubmitFrame = CGRect.zero
    private var passwordCancelFrame = CGRect.zero
    private var passwordPanelFrame = CGRect.zero
    private var pendingInstallBackend: BackendRecord?
    private var pendingInstallOperation: String = "run"
    private var installPanelFrame = CGRect.zero
    private var installConfirmFrame = CGRect.zero
    private var installRootConfirmFrame = CGRect.zero
    private var installCancelFrame = CGRect.zero
    private var filePickerPanelFrame = CGRect.zero
    private var filePickerEntryFrames: [(frame: CGRect, entry: FilePickerEntryRecord)] = []
    private var filePickerFilenameFrame = CGRect.zero
    private var filePickerSaveFrame = CGRect.zero
    private var filePickerCancelFrame = CGRect.zero
    private var filePickerListFrame = CGRect.zero
    private var filePickerContentHeight: CGFloat = 0
    private var filePickerScroll: CGFloat = 0

    private let toolbarHeight: CGFloat = 48
    private let tableHeaderHeight: CGFloat = 30
    private let backendRowHeight: CGFloat = 44
    private let logHeaderHeight: CGFloat = 62
    private let logTextInsetX: CGFloat = 12
    private let logTextInsetY: CGFloat = 10
    private let logScrollLineHeight: CGFloat = 18
    private let horizontalInset: CGFloat = 18
    private let createBottomInset: CGFloat = 18
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
            outerframeHost.configure(url: arguments.url ?? "",
                                     bundleUrl: arguments.bundleUrl ?? "",
                                     proxyHost: arguments.proxy?.host,
                                     proxyPort: arguments.proxy?.port ?? 0,
                                     proxyUsername: arguments.proxy?.username,
                                     proxyPassword: arguments.proxy?.password)
            mode = modeFromURL(arguments.url)
            appearance = arguments.appearance ?? NSAppearance.currentDrawing()
            currentSize = arguments.contentSize ?? currentSize
            configureNetworking()
            configureLayersIfNeeded()
            updateLayout()
            updateColors()
            registerRootLayerIfNeeded()
            updateInputMode()
            updateEditingAndPasteboardState()
            outerframeHost.setPasteboardDropBehaviorHitTest(acceptedTypes: Self.createFieldPasteboardTypes)
            fetchBackends()
            fetchRecipes()
            startEventWatch()

        case .resizeContent(let size):
            currentSize = size
            clampScrollOffsets()
            updateLayout()

        case .systemAppearanceUpdate(let appearance):
            self.appearance = appearance
            updateColors()

        case .scrollWheelEvent(let point, let delta, _, _, _, let hasPreciseScrollingDeltas):
            handleScroll(at: point, delta: delta, precise: hasPreciseScrollingDeltas)

        case .mouseDown(let point, let modifierFlags, let clickCount):
            handleMouseDown(at: point, modifierFlags: modifierFlags, clickCount: clickCount)

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
            if pendingPasswordAction != nil {
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
            }
            updateEditingAndPasteboardState()

        case .windowActiveUpdate(let isActive):
            windowIsActive = isActive
            updateLayout()

        case .selectionToPasteboardCopyRequest(let requestID):
            outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID,
                                                              items: pasteboardItemsForCopy())

        case .selectionToPasteboardCutRequest(let requestID):
            outerframeHost.sendCopySelectedPasteboardResponse(requestID: requestID,
                                                              items: pasteboardItemsForCut())

        case .pasteboardContentPasted(let items):
            handlePasteboardItemsForPaste(items)

        case .pasteboardDropHitTestRequest(let requestID, let point, let pasteboardTypes, let operationMask, _):
            let accepted = createFieldAcceptsTextDrop(at: point,
                                                      pasteboardTypes: pasteboardTypes,
                                                      operationMask: operationMask) ||
                           passwordFieldAcceptsTextDrop(at: point,
                                                        pasteboardTypes: pasteboardTypes,
                                                        operationMask: operationMask)
            outerframeHost.sendPasteboardDropHitTestResponse(requestID: requestID,
                                                             operationMask: accepted ? .copy : [])

        case .pasteboardContentDropped(let point, let items):
            handlePasteboardItemsForDrop(at: point, items: items)

        case .historyTraversal(_, let url):
            blurCreateField()
            applyMode(modeFromURL(url))

        case .accessibilitySnapshotRequest(let requestID):
            outerframeHost.sendAccessibilitySnapshotResponse(requestID: requestID,
                                                             snapshot: OuterframeAccessibilitySnapshot.notImplementedSnapshot())

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
            backendsEndpoint = URL(string: "/api/backends", relativeTo: base)?.absoluteURL
            logsEndpoint = URL(string: "/api/logs", relativeTo: base)?.absoluteURL
            controlEndpoint = URL(string: "/api/control", relativeTo: base)?.absoluteURL
            createEndpoint = URL(string: "/api/create", relativeTo: base)?.absoluteURL
            recipesEndpoint = URL(string: "/api/recipes", relativeTo: base)?.absoluteURL
            filePickerEndpoint = URL(string: "/api/file-picker", relativeTo: base)?.absoluteURL
            eventsEndpoint = URL(string: "/api/events", relativeTo: base)?.absoluteURL
        }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 40
        configuration.timeoutIntervalForResource = 45
        configuration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        outerframeHost.applyProxy(to: configuration)
        urlSession = URLSession(configuration: configuration)
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

    private func applyMode(_ nextMode: BackendsViewMode) {
        if mode == .create && nextMode != .create {
            blurCreateField()
        }
        if nextMode != .create {
            setCursorIfNeeded(.arrow)
        }
        mode = nextMode
        if mode == .create || mode == .apps {
            clampScrollOffsets()
        }
        updateColors()
    }

    private func returnToAppsFromCreate() {
        if outerframeHost.canGoBackInHistory() {
            outerframeHost.goBackInHistory()
        } else {
            navigateToMode(.apps, pushHistory: false)
        }
    }

    private func startEventWatch(resetVersions: Bool = false) {
        guard eventWatchTask == nil, let eventsEndpoint, let urlSession else { return }
        if resetVersions {
            backendsEventVersion = 0
            logEventVersion = 0
        }
        let generation = eventWatchGeneration
        var components = URLComponents(url: eventsEndpoint, resolvingAgainstBaseURL: false)
        var items = [
            URLQueryItem(name: "sinceBackends", value: String(backendsEventVersion)),
            URLQueryItem(name: "sinceLog", value: String(logEventVersion))
        ]
        if let selectedLog {
            items.append(URLQueryItem(name: "serviceID", value: selectedLog.serviceID))
            items.append(URLQueryItem(name: "logIndex", value: String(selectedLog.logIndex)))
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
                    if event.backendsChanged {
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
        rootLayer.addSublayer(installOverlayLayer)
        rootLayer.addSublayer(passwordOverlayLayer)
        toolbarLayer.addSublayer(titleLayer)
        toolbarLayer.addSublayer(statusLayer)
        toolbarLayer.addSublayer(outerShellActionLayer)
        contentLayer.addSublayer(appsLayer)
        appsLayer.addSublayer(appsScrollContentLayer)
        appsLayer.addSublayer(appsOverlayLayer)
        contentLayer.addSublayer(tableHeaderLayer)
        contentLayer.addSublayer(rowsClipLayer)
        rowsClipLayer.addSublayer(backendRowsContentLayer)
        contentLayer.addSublayer(dividerLayer)
        contentLayer.addSublayer(logHeaderLayer)
        contentLayer.addSublayer(logRowsClipLayer)
        logRowsClipLayer.addSublayer(logTextContentLayer)
        logTextContentLayer.addSublayer(logTextSelectionLayer)
        let scrollbar = ScrollbarController<BackendsHandler>(appConnection: outerframeHost,
                                                             viewportLayer: logRowsClipLayer,
                                                             appearance: appearance ?? NSAppearance.currentDrawing(),
                                                             scrollOffsetOrigin: .bottom)
        scrollbar.delegate = self
        logScrollbarController = scrollbar
        contentLayer.addSublayer(createLayer)
        createLayer.addSublayer(filePickerOverlayLayer)

        titleLayer.string = ""
        outerShellActionLayer.isHidden = true
        installOverlayLayer.isHidden = true
        passwordOverlayLayer.isHidden = true
        filePickerOverlayLayer.isHidden = true
        appsLayer.masksToBounds = true
        rowsClipLayer.masksToBounds = true
        logRowsClipLayer.masksToBounds = true

        for layer in [titleLayer, statusLayer] {
            layer.contentsScale = 2
            layer.truncationMode = .end
            layer.alignmentMode = .center
        }
        titleLayer.alignmentMode = .left
        statusLayer.alignmentMode = .right
    }

    private func registerRootLayerIfNeeded() {
        guard !didRegisterLayer, let registerLayer = appConnection.registerLayer else { return }
        registerLayer(rootLayer)
        didRegisterLayer = true
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

    private func updateLayout() {
        withEffectiveAppearance {
            withoutImplicitAnimations {
                let width = max(currentSize.width, 1)
                let height = max(currentSize.height, 1)
                rootLayer.frame = CGRect(origin: .zero, size: CGSize(width: width, height: height))
                toolbarLayer.frame = CGRect(x: 0, y: max(height - toolbarHeight, 0), width: width, height: toolbarHeight)
                contentLayer.frame = CGRect(x: 0, y: 0, width: width, height: max(height - toolbarHeight, 0))
                iconTransitionLayer.frame = rootLayer.bounds

                titleLayer.frame = .zero
                outerShellActionFrame = mode == .apps && outerShellBackend() != nil
                    ? CGRect(x: max(width - horizontalInset - 28, horizontalInset),
                             y: 10,
                             width: 28,
                             height: 28)
                    : .zero
                outerShellActionLayer.frame = outerShellActionFrame
                outerShellActionLayer.isHidden = outerShellActionFrame.isEmpty
                outerShellActionLayer.opacity = outerShellActionFrame.isEmpty ? 0 : 1
                statusLayer.frame = CGRect(x: horizontalInset, y: 14, width: max(width - horizontalInset * 2 - 36, 1), height: 18)

                let contentHeight = contentLayer.bounds.height
                if mode == .apps {
                    outerframeHost.sendTextCursorUpdate(cursors: [])
                    appsLayer.isHidden = false
                    tableHeaderLayer.isHidden = true
                    rowsClipLayer.isHidden = true
                    dividerLayer.isHidden = selectedServiceID == nil
                    logHeaderLayer.isHidden = selectedServiceID == nil
                    logRowsClipLayer.isHidden = selectedServiceID == nil
                    createLayer.isHidden = true
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
                } else {
                    appsLayer.isHidden = true
                    tableHeaderLayer.isHidden = true
                    rowsClipLayer.isHidden = true
                    dividerLayer.isHidden = true
                    logHeaderLayer.isHidden = true
                    logRowsClipLayer.isHidden = true
                    createLayer.isHidden = false
                    createLayer.frame = CGRect(x: 0, y: 0, width: width, height: contentHeight)
                    hideAllMatchedLayers()
                    renderCreateForm()
                }
                updateStatusText()
                renderInstallPromptIfNeeded(width: width, height: height)
                renderPasswordPromptIfNeeded(width: width, height: height)
            }
        }
    }

    private func updateColors() {
        withEffectiveAppearance {
            withoutImplicitAnimations {
                rootLayer.backgroundColor = resolvedCGColor(.windowBackgroundColor)
                toolbarLayer.backgroundColor = resolvedCGColor(.controlBackgroundColor)
                contentLayer.backgroundColor = resolvedCGColor(.windowBackgroundColor)
                tableHeaderLayer.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.9))
                logHeaderLayer.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.9))
                dividerLayer.backgroundColor = resolvedCGColor(.separatorColor)

                titleLayer.foregroundColor = resolvedCGColor(.labelColor)
                statusLayer.foregroundColor = resolvedCGColor(.secondaryLabelColor)
                outerShellActionLayer.applyStyle(tintCGColor: resolvedCGColor(.secondaryLabelColor),
                                                 backgroundCGColor: resolvedCGColor(.clear))
                updateLogTextContentIfNeeded(text: currentLogText(), force: true)
                updateLogTextViewport()
                updateLogTextSelectionLayers(force: true)
                logScrollbarController?.updateAppearance(appearance ?? NSAppearance.currentDrawing())

                titleLayer.font = NSFont.systemFont(ofSize: 15, weight: .semibold)
                titleLayer.fontSize = 15
                for layer in [statusLayer] {
                    layer.font = NSFont.systemFont(ofSize: 12, weight: .medium)
                    layer.fontSize = 12
                }
                updateLayout()
            }
        }
    }

    private func renderBackendsHeader() {
        tableHeaderLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        let columns = backendColumns(width: tableHeaderLayer.bounds.width)
        for column in columns {
            let layer = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
            layer.string = column.title
            layer.frame = CGRect(x: column.x, y: 8, width: column.width, height: 16)
            tableHeaderLayer.addSublayer(layer)
        }
    }

    private func updateAppsScrollLayerFrames() {
        appsScrollContentLayer.frame = CGRect(x: 0,
                                              y: appsScroll,
                                              width: appsLayer.bounds.width,
                                              height: appsLayer.bounds.height)
        appsOverlayLayer.frame = appsLayer.bounds
    }

    private func updateBackendRowsScrollLayerFrame() {
        backendRowsContentLayer.frame = CGRect(x: 0,
                                               y: backendScroll,
                                               width: rowsClipLayer.bounds.width,
                                               height: rowsClipLayer.bounds.height)
    }

    private func updateMatchedLayerVisibility() {
        let clipLayer: CALayer?
        switch mode {
        case .apps:
            clipLayer = appsLayer
        case .create:
            clipLayer = nil
        }
        guard let clipLayer else {
            hideAllMatchedLayers()
            return
        }
        let clipFrame = rootLayer.convert(clipLayer.bounds, from: clipLayer).insetBy(dx: -1, dy: -1)
        for (key, state) in iconMatchStates {
            iconMatchLayers[key]?.isHidden = !state.frame.intersects(clipFrame)
        }
        for (key, state) in textMatchStates {
            textMatchLayers[key]?.isHidden = !state.frame.intersects(clipFrame)
        }
    }

    private func offsetMatchedLayers(deltaY: CGFloat) {
        guard abs(deltaY) > 0.001 else {
            updateMatchedLayerVisibility()
            return
        }
        for (key, state) in iconMatchStates {
            let frame = state.frame.offsetBy(dx: 0, dy: deltaY)
            iconMatchStates[key] = IconMatchState(frame: frame,
                                                  image: state.image,
                                                  title: state.title)
            iconMatchLayers[key]?.frame = frame
        }
        for (key, state) in textMatchStates {
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
                offsetMatchedLayers(deltaY: deltaY)
            case .create:
                break
            }
        }
    }

    private func renderBackendsRows() {
        for layer in rowsClipLayer.sublayers ?? [] where layer !== backendRowsContentLayer {
            layer.removeFromSuperlayer()
        }
        if backendRowsContentLayer.superlayer == nil {
            rowsClipLayer.addSublayer(backendRowsContentLayer)
        }
        backendRowsContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        backendRowFrames.removeAll()
        backendActionFrames.removeAll()
        newBackendRowFrame = .zero
        iconMatchStates.removeAll()
        textMatchStates.removeAll()
        var visibleIconKeys = Set<String>()
        var visibleTextKeys = Set<String>()

        let rows = backendListRows()
        if rows.isEmpty {
            let empty = makeTextLayer(size: 13, weight: .regular, color: .tertiaryLabelColor, alignment: .center)
            empty.string = isLoadingBackends ? "Loading backends..." : (backendError.isEmpty ? "No registered backends." : backendError)
            empty.frame = CGRect(x: horizontalInset, y: max(rowsClipLayer.bounds.midY - 10, 0), width: max(rowsClipLayer.bounds.width - horizontalInset * 2, 1), height: 20)
            rowsClipLayer.addSublayer(empty)
        }

        let totalRows = rows.count + 1
        let columns = backendColumns(width: rowsClipLayer.bounds.width)
        let selectedBackground = resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.14))
        let alternating = alternatingRowColors()

        for index in 0..<totalRows {
            let y = rowsClipLayer.bounds.height - CGFloat(index + 1) * backendRowHeight
            let rowFrame = CGRect(x: 0, y: y, width: rowsClipLayer.bounds.width, height: backendRowHeight)
            if index == rows.count {
                renderNewBackendRow(rowFrame: rowFrame,
                                    columns: columns,
                                    backgroundColor: index.isMultiple(of: 2) ? alternating.even : alternating.odd)
                continue
            }

            let row = rows[index]
            let backend = row.backend
            backendRowFrames.append((rowFrame, row))

            let rowLayer = CALayer()
            rowLayer.frame = rowFrame
            if backend.serviceID == selectedServiceID {
                rowLayer.backgroundColor = selectedBackground
            } else if index.isMultiple(of: 2) {
                rowLayer.backgroundColor = alternating.even
            } else {
                rowLayer.backgroundColor = alternating.odd
            }
            backendRowsContentLayer.addSublayer(rowLayer)

            let italic = backend.isBundledPlaceholder
            let indent: CGFloat
            if row.isFrontendChild {
                indent = addFrontendIcon(for: row,
                                         rowLayer: rowLayer,
                                         column: columns[0],
                                         visibleIconKeys: &visibleIconKeys)
            } else {
                indent = 0
            }
            let visibleName = rowName(for: row)
            if !recordBackendNameMatches(for: row,
                                          visibleName: visibleName,
                                          rowLayer: rowLayer,
                                          column: columns[0],
                                          y: 14,
                                          xOffset: indent,
                                          italic: italic,
                                          visibleTextKeys: &visibleTextKeys) {
                addCell(to: rowLayer, text: visibleName, column: columns[0], y: 14, xOffset: indent, weight: row.isFrontendChild ? .regular : .medium, italic: italic, emptyPlaceholder: "")
            }
            addCell(to: rowLayer, text: rowStatus(for: row), column: columns[1], y: 14, color: row.isFrontendChild ? .secondaryLabelColor : statusColor(backend.status), italic: italic, emptyPlaceholder: "")
            addCell(to: rowLayer, text: rowPathText(for: row), column: columns[2], y: 14, color: .secondaryLabelColor, italic: italic)

            let actions = rowActions(for: row)
            if !actions.isEmpty {
                var x = columns[3].x
                for action in actions {
                    let width = actionButtonWidth(for: action.operation)
                    let actionFrame = CGRect(x: x, y: 8, width: min(width, max(columns[3].x + columns[3].width - x - 6, 1)), height: 28)
                    backendActionFrames.append((CGRect(x: actionFrame.minX,
                                                       y: rowFrame.minY + actionFrame.minY,
                                                       width: actionFrame.width,
                                                       height: actionFrame.height),
                                                row,
                                                action.operation))
                    let button = makeActionButtonLayer(title: action.title, operation: action.operation)
                    button.frame = actionFrame
                    rowLayer.addSublayer(button)
                    x += width + 6
                    if x >= columns[3].x + columns[3].width - 6 { break }
                }
            } else {
                addCell(to: rowLayer, text: "--", column: columns[3], y: 14, color: .tertiaryLabelColor, alignment: .center)
            }
        }
        hideUnrenderedMatchedLayers(visibleIconKeys: visibleIconKeys, visibleTextKeys: visibleTextKeys)
        updateMatchedLayerVisibility()
    }

    private func renderNewBackendRow(rowFrame: CGRect,
                                     columns: [(title: String, x: CGFloat, width: CGFloat)],
                                     backgroundColor: CGColor) {
        newBackendRowFrame = rowFrame

        let rowLayer = CALayer()
        rowLayer.frame = rowFrame
        rowLayer.backgroundColor = backgroundColor
        backendRowsContentLayer.addSublayer(rowLayer)

        let iconSize: CGFloat = 18
        let icon = CALayer()
        icon.frame = CGRect(x: columns[0].x,
                            y: floor((backendRowHeight - iconSize) / 2),
                            width: iconSize,
                            height: iconSize)
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: "plus.circle", pointSize: iconSize)
        rowLayer.addSublayer(icon)

        addCell(to: rowLayer,
                text: "New",
                column: columns[0],
                y: 14,
                xOffset: iconSize + 8,
                color: .controlAccentColor,
                weight: .medium,
                emptyPlaceholder: "")
        addCell(to: rowLayer,
                text: "Create",
                column: columns[1],
                y: 14,
                color: .secondaryLabelColor,
                emptyPlaceholder: "")
        addCell(to: rowLayer,
                text: "Add a backend",
                column: columns[2],
                y: 14,
                color: .secondaryLabelColor,
                emptyPlaceholder: "")
    }

    private func renderAppsPage() {
        for layer in appsLayer.sublayers ?? [] where layer !== appsScrollContentLayer && layer !== appsOverlayLayer {
            layer.removeFromSuperlayer()
        }
        if appsScrollContentLayer.superlayer == nil {
            appsLayer.addSublayer(appsScrollContentLayer)
        }
        if appsOverlayLayer.superlayer == nil {
            appsLayer.addSublayer(appsOverlayLayer)
        }
        appsScrollContentLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        appsOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        appCardFrames.removeAll()
        appBadgeFrames.removeAll()
        appListDropFrames.removeAll()
        appUnlistedDropFrames.removeAll()
        addAppFrame = .zero
        iconMatchStates.removeAll()
        textMatchStates.removeAll()
        var visibleIconKeys = Set<String>()
        var visibleTextKeys = Set<String>()

        let items = appLauncherItems()
        let iconItems = items.filter { $0.frontend.listName.isEmpty }
        let listGroups = Dictionary(grouping: items.filter { !$0.frontend.listName.isEmpty },
                                    by: { $0.frontend.listName })
            .map { (name: $0.key, items: $0.value.sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }) }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        let contentWidth = max(appsLayer.bounds.width - horizontalInset * 2, 1)
        let left = horizontalInset
        let top = max(appsLayer.bounds.height - 28, 0)

        let splitGap: CGFloat = 34
        let usesSplitLayout = contentWidth >= 760 && !listGroups.isEmpty
        let appArea: CGRect
        let listArea: CGRect
        if usesSplitLayout {
            let columnWidth = floor((contentWidth - splitGap) / 2)
            appArea = CGRect(x: left, y: 0, width: columnWidth, height: 0)
            listArea = CGRect(x: left + columnWidth + splitGap, y: 0, width: columnWidth, height: 0)
            let appBottom = renderAppIconGrid(items: iconItems,
                                              area: appArea,
                                              top: top,
                                              includesAddTile: true,
                                              visibleIconKeys: &visibleIconKeys,
                                              visibleTextKeys: &visibleTextKeys)
            let listBottom = renderAppListGroups(groups: listGroups,
                                                 area: listArea,
                                                 top: top,
                                                 visibleIconKeys: &visibleIconKeys,
                                                 visibleTextKeys: &visibleTextKeys)
            appsContentBottom = min(appBottom, listBottom)
        } else {
            let fullArea = CGRect(x: left, y: 0, width: contentWidth, height: 0)
            let appBottom = renderAppIconGrid(items: iconItems,
                                              area: fullArea,
                                              top: top,
                                              includesAddTile: true,
                                              visibleIconKeys: &visibleIconKeys,
                                              visibleTextKeys: &visibleTextKeys)
            let listTop = listGroups.isEmpty ? appBottom : appBottom - 28
            let listBottom = renderAppListGroups(groups: listGroups,
                                                 area: fullArea,
                                                 top: listTop,
                                                 visibleIconKeys: &visibleIconKeys,
                                                 visibleTextKeys: &visibleTextKeys)
            appsContentBottom = min(appBottom, listBottom)
        }
        renderAppDragOverlayIfNeeded()
        hideUnrenderedMatchedLayers(visibleIconKeys: visibleIconKeys, visibleTextKeys: visibleTextKeys)
        updateMatchedLayerVisibility()
        if clampAppsScrollUsingRenderedContent() {
            updateAppsScrollLayerFrames()
            renderAppsPage()
        }
    }

    private func renderAppIconGrid(items: [AppLauncherItem],
                                   area: CGRect,
                                   top: CGFloat,
                                   includesAddTile: Bool,
                                   visibleIconKeys: inout Set<String>,
                                   visibleTextKeys: inout Set<String>) -> CGFloat {
        guard !items.isEmpty || includesAddTile else { return top }

        let itemWidth = max(min(floor(area.width / 4) - 10, 112), 86)
        let itemHeight: CGFloat = 116
        let iconSize: CGFloat = 46
        let horizontalGap: CGFloat = 10
        let verticalGap: CGFloat = 10
        let columns = max(Int((area.width + horizontalGap) / (itemWidth + horizontalGap)), 1)
        let usedWidth = CGFloat(columns) * itemWidth + CGFloat(max(columns - 1, 0)) * horizontalGap
        let startX = area.minX + max(floor((area.width - usedWidth) / 2), 0)
        var x = startX
        var y = top - itemHeight

        for (index, item) in items.enumerated() {
            if index > 0 && index.isMultiple(of: columns) {
                x = startX
                y -= itemHeight + verticalGap
            }

            let frame = CGRect(x: x, y: y, width: itemWidth, height: itemHeight)
            appCardFrames.append((frame, item))
            let iconFrame = CGRect(x: frame.minX + floor((itemWidth - iconSize) / 2),
                                   y: frame.maxY - iconSize - 10,
                                   width: iconSize,
                                   height: iconSize)

            recordMatchedIcon(key: item.iconKey,
                              frame: rootLayer.convert(iconFrame, from: appsScrollContentLayer),
                              image: item.iconImage,
                              title: item.displayName)
            visibleIconKeys.insert(item.iconKey)
            let titleFrame = CGRect(x: frame.minX, y: frame.minY + 26, width: itemWidth, height: 28)
            recordMatchedText(key: item.iconKey,
                              frame: rootLayer.convert(titleFrame, from: appsScrollContentLayer),
                              title: item.displayName,
                              fontSize: 12,
                              weight: .medium,
                              alignment: .center,
                              isWrapped: true)
            visibleTextKeys.insert(item.iconKey)
            renderRunningBadges(for: item,
                                leftX: iconFrame.maxX + 6,
                                centerY: iconFrame.midY,
                                pointSize: 10,
                                circleDiameter: 18,
                                gap: 3)

            x += itemWidth + horizontalGap
        }

        if includesAddTile {
            let index = items.count
            if index > 0 && index.isMultiple(of: columns) {
                x = startX
                y -= itemHeight + verticalGap
            }
            let frame = CGRect(x: x, y: y, width: itemWidth, height: itemHeight)
            addAppFrame = frame
            renderAddAppTile(frame: frame, iconSize: iconSize)
        }

        appUnlistedDropFrames.append(CGRect(x: area.minX,
                                            y: min(y, top),
                                            width: area.width,
                                            height: max(top - min(y, top), itemHeight)))
        return y
    }

    private func renderAddAppTile(frame: CGRect, iconSize: CGFloat) {
        let iconFrame = CGRect(x: frame.minX + floor((frame.width - iconSize) / 2),
                               y: frame.maxY - iconSize - 10,
                               width: iconSize,
                               height: iconSize)
        let icon = CALayer()
        icon.frame = iconFrame
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: "plus.square", pointSize: iconSize)
        appsScrollContentLayer.addSublayer(icon)

        let title = makeTextLayer(size: 12, weight: .medium, color: .labelColor, alignment: .center, italic: true)
        title.string = "Add app"
        title.frame = CGRect(x: frame.minX, y: frame.minY + 26, width: frame.width, height: 28)
        appsScrollContentLayer.addSublayer(title)
    }

    private func renderAddableAppsSection(apps: [BackendRecord],
                                          left: CGFloat,
                                          top: CGFloat,
                                          width: CGFloat) -> CGFloat {
        let title = makeTextLayer(size: 13, weight: .semibold, color: .secondaryLabelColor)
        title.string = "App Catalog"
        title.frame = CGRect(x: left, y: top, width: width, height: 18)
        createLayer.addSublayer(title)

        let itemHeight: CGFloat = 94
        let itemGap: CGFloat = 12
        let itemWidth: CGFloat = 112
        let columns = max(Int((width + itemGap) / (itemWidth + itemGap)), 1)
        let usedWidth = CGFloat(columns) * itemWidth + CGFloat(max(columns - 1, 0)) * itemGap
        let startX = left + max(floor((width - usedWidth) / 2), 0)
        var x = left
        var y = top - itemHeight - 22

        for (index, app) in apps.enumerated() {
            if index == 0 {
                x = startX
            } else if index.isMultiple(of: columns) {
                x = startX
                y -= itemHeight + itemGap
            }
            let frame = CGRect(x: x, y: y, width: itemWidth, height: itemHeight)
            renderAddableAppTile(app, frame: frame)
            x += itemWidth + itemGap
        }

        return y - 34
    }

    private func renderAddableAppTile(_ backend: BackendRecord, frame: CGRect) {
        let iconSize: CGFloat = 46
        let iconFrame = CGRect(x: frame.minX + floor((frame.width - iconSize) / 2),
                               y: frame.maxY - iconSize - 8,
                               width: iconSize,
                               height: iconSize)
        let icon = CALayer()
        icon.frame = iconFrame
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        if let symbolName = backend.iconSymbolName,
           !symbolName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            icon.contents = symbolCGImage(named: symbolName, pointSize: iconSize)
        } else {
            icon.contents = letterIconCGImage(for: backend.displayName)
        }
        createLayer.addSublayer(icon)

        let name = makeTextLayer(size: 12, weight: .medium, color: .labelColor, alignment: .center)
        name.string = backend.displayName
        name.isWrapped = true
        name.truncationMode = .none
        name.frame = CGRect(x: frame.minX, y: frame.minY + 6, width: frame.width, height: 32)
        createLayer.addSublayer(name)

        bundledAppInstallFrames.append((frame, backend))
    }

    private func renderAppListGroups(groups: [(name: String, items: [AppLauncherItem])],
                                     area: CGRect,
                                     top: CGFloat,
                                     visibleIconKeys: inout Set<String>,
                                     visibleTextKeys: inout Set<String>) -> CGFloat {
        guard !groups.isEmpty else { return top }

        let rowHeight: CGFloat = 52
        let labelHeight: CGFloat = 20
        let widgetTopPadding: CGFloat = 10
        let widgetBottomPadding: CGFloat = 10
        var y = top

        for group in groups {
            let rowCount = max(group.items.count, 1)
            let widgetHeight = widgetTopPadding + CGFloat(rowCount) * rowHeight + widgetBottomPadding
            y -= widgetHeight
            let widgetFrame = CGRect(x: area.minX, y: y, width: area.width, height: widgetHeight)
            appListDropFrames.append((widgetFrame, group.name))
            renderAppListWidget(name: group.name,
                                items: group.items,
                                frame: widgetFrame,
                                rowHeight: rowHeight,
                                visibleIconKeys: &visibleIconKeys,
                                visibleTextKeys: &visibleTextKeys)

            let label = makeTextLayer(size: 12, weight: .medium, color: .secondaryLabelColor, alignment: .center)
            label.string = group.name
            label.frame = CGRect(x: widgetFrame.minX, y: widgetFrame.minY - labelHeight, width: widgetFrame.width, height: 16)
            appsScrollContentLayer.addSublayer(label)
            y -= labelHeight + 22
        }

        return y
    }

    private func renderAppListWidget(name: String,
                                     items: [AppLauncherItem],
                                     frame: CGRect,
                                     rowHeight: CGFloat,
                                     visibleIconKeys: inout Set<String>,
                                     visibleTextKeys: inout Set<String>) {
        let background = CALayer()
        background.frame = frame
        background.cornerRadius = 14
        background.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.86))
        background.borderWidth = 0.5
        background.borderColor = resolvedCGColor(NSColor.separatorColor.withAlphaComponent(0.55))
        appsScrollContentLayer.addSublayer(background)

        let iconSize: CGFloat = 30
        let rowInset: CGFloat = 12
        var y = frame.maxY - 10 - rowHeight
        for (index, item) in items.enumerated() {
            let rowFrame = CGRect(x: frame.minX + 8, y: y, width: frame.width - 16, height: rowHeight)
            appCardFrames.append((rowFrame, item))

            if index > 0 {
                let separator = CALayer()
                separator.backgroundColor = resolvedCGColor(.separatorColor)
                separator.frame = CGRect(x: rowFrame.minX + rowInset + iconSize + 10,
                                         y: rowFrame.maxY - 0.5,
                                         width: max(rowFrame.width - rowInset - iconSize - 22, 1),
                                         height: 0.5)
                appsScrollContentLayer.addSublayer(separator)
            }

            let iconFrame = CGRect(x: rowFrame.minX + rowInset,
                                   y: rowFrame.midY - iconSize / 2,
                                   width: iconSize,
                                   height: iconSize)
            recordMatchedIcon(key: item.iconKey,
                              frame: rootLayer.convert(iconFrame, from: appsScrollContentLayer),
                              image: item.iconImage,
                              title: item.displayName)
            visibleIconKeys.insert(item.iconKey)
            let hasRunningBadges = !runningEndpoints(for: item).isEmpty
            let textLeft = iconFrame.maxX + (hasRunningBadges ? 28 : 12)
            let textFrame = CGRect(x: textLeft,
                                   y: rowFrame.midY - 10,
                                   width: max(rowFrame.maxX - textLeft - 16, 1),
                                   height: 20)
            recordMatchedText(key: item.iconKey,
                              frame: rootLayer.convert(textFrame, from: appsScrollContentLayer),
                              title: item.displayName,
                              fontSize: 13,
                              weight: .medium,
                              alignment: .left,
                              isWrapped: false)
            visibleTextKeys.insert(item.iconKey)
            renderRunningBadges(for: item,
                                leftX: iconFrame.maxX + 6,
                                centerY: iconFrame.midY,
                                pointSize: 8,
                                circleDiameter: 14,
                                gap: 2)
            y -= rowHeight
        }
    }

    private func renderAppDragOverlayIfNeeded() {
        guard let drag = pendingAppDrag, drag.isDragging else { return }
        let appsPoint = appsLayer.convert(drag.currentPoint, from: rootLayer)
        let appsContentPoint = appsScrollContentLayer.convert(drag.currentPoint, from: rootLayer)
        if let target = appDropTarget(at: appsContentPoint, for: drag.item),
           target.listName != drag.item.frontend.listName,
           let highlightFrame = appDropHighlightFrame(for: target) {
            let highlight = CALayer()
            highlight.frame = highlightFrame.insetBy(dx: -4, dy: -4)
            highlight.cornerRadius = target == .unlisted ? 18 : 16
            highlight.borderWidth = 2
            highlight.borderColor = resolvedCGColor(.controlAccentColor)
            highlight.backgroundColor = resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.08))
            appsScrollContentLayer.addSublayer(highlight)
        }

        let iconSize: CGFloat = 46
        let iconFrame = CGRect(x: appsPoint.x - iconSize / 2,
                               y: appsPoint.y - iconSize / 2 + 12,
                               width: iconSize,
                               height: iconSize)
        let icon = makeLauncherIconLayer(image: drag.item.iconImage,
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

    private func appDropTarget(at appsPoint: CGPoint, for _: AppLauncherItem) -> AppDropTarget? {
        if let frame = appListDropFrames.first(where: { $0.frame.contains(appsPoint) }) {
            return .list(frame.listName)
        }
        if appUnlistedDropFrames.contains(where: { $0.contains(appsPoint) }) {
            return .unlisted
        }
        return nil
    }

    private func appDropHighlightFrame(for target: AppDropTarget) -> CGRect? {
        switch target {
        case .unlisted:
            return appUnlistedDropFrames.reduce(nil) { partial, frame in
                guard let partial else { return frame }
                return partial.union(frame)
            }
        case .list(let name):
            return appListDropFrames.first(where: { $0.listName == name })?.frame
        }
    }

    private func addFrontendIcon(for row: BackendListRow,
                                 rowLayer: CALayer,
                                 column: (title: String, x: CGFloat, width: CGFloat),
                                 visibleIconKeys: inout Set<String>) -> CGFloat {
        guard row.isFrontendChild else { return 0 }
        let iconSize: CGFloat = 22
        let iconX = column.x + 18
        let title = frontendIconTitle(for: row)
        if let key = row.iconKey {
            recordMatchedIcon(key: key,
                              frame: rootLayer.convert(CGRect(x: iconX,
                                                              y: floor((backendRowHeight - iconSize) / 2),
                                                              width: iconSize,
                                                              height: iconSize),
                                                       from: rowLayer),
                              image: row.frontend?.iconImage,
                              title: title)
            visibleIconKeys.insert(key)
        }
        return 48
    }

    private func makeLauncherIconLayer(image: NSImage?, title: String, iconSize: CGFloat) -> CALayer {
        let icon = CALayer()
        icon.cornerRadius = iconCornerRadius(for: iconSize)
        icon.masksToBounds = true
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        configureLauncherIconLayer(icon, image: image, symbolName: nil, title: title, iconSize: iconSize)
        return icon
    }

    private func configureLauncherIconLayer(_ icon: CALayer,
                                            image: NSImage?,
                                            symbolName: String?,
                                            title: String,
                                            iconSize: CGFloat) {
        icon.cornerRadius = iconCornerRadius(for: iconSize)
        if let image,
           let cgImage = cgImage(for: image) {
            icon.contents = cgImage
            icon.backgroundColor = resolvedCGColor(.clear)
        } else if let symbolName,
                  !symbolName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  let cgImage = symbolAppIconCGImage(named: symbolName) {
            icon.contents = cgImage
            icon.backgroundColor = resolvedCGColor(.clear)
        } else {
            icon.contents = letterIconCGImage(for: title)
            icon.backgroundColor = resolvedCGColor(.clear)
        }
    }

    private func frontendIconTitle(for row: BackendListRow) -> String {
        let frontendName = row.frontend?.name.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if !frontendName.isEmpty {
            return frontendName
        }
        let backendName = row.backend.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        return backendName.isEmpty ? "Frontend" : backendName
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

    private func symbolAppIconCGImage(named symbolName: String) -> CGImage? {
        let imageSize: CGFloat = 96
        let image = NSImage(size: NSSize(width: imageSize, height: imageSize))
        image.lockFocus()
        withEffectiveAppearance {
            NSColor.controlAccentColor.withAlphaComponent(0.18).setFill()
            NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: imageSize, height: imageSize),
                         xRadius: 22,
                         yRadius: 22).fill()

            if let symbol = NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)?
                .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: 42, weight: .regular)
                    .applying(NSImage.SymbolConfiguration(hierarchicalColor: .controlAccentColor))) {
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
        return cgImage(for: image)
    }

    private func iconCornerRadius(for iconSize: CGFloat) -> CGFloat {
        iconSize >= 44 ? 13 : 5
    }

    private func recordMatchedIcon(key: String, frame: CGRect, image: NSImage?, title: String) {
        let existingLayer = iconMatchLayers[key]
        let layer = existingLayer ?? makeLauncherIconLayer(image: image, title: title, iconSize: frame.width)
        if layer.superlayer == nil {
            iconTransitionLayer.addSublayer(layer)
        }
        configureLauncherIconLayer(layer, image: image, symbolName: nil, title: title, iconSize: frame.width)
        layer.frame = frame
        layer.isHidden = false
        layer.opacity = 1
        iconMatchStates[key] = IconMatchState(frame: frame,
                                              image: image,
                                              title: title)
        iconMatchLayers[key] = layer
    }

    private func recordBackendNameMatches(for row: BackendListRow,
                                          visibleName: String,
                                          rowLayer: CALayer,
                                          column: (title: String, x: CGFloat, width: CGFloat),
                                          y: CGFloat,
                                          xOffset: CGFloat,
                                          italic: Bool,
                                          visibleTextKeys: inout Set<String>) -> Bool {
        if row.isFrontendChild {
            guard !visibleName.isEmpty,
                  let key = row.iconKey else {
                return false
            }
            let frame = rootLayer.convert(cellFrame(column: column, y: y, xOffset: xOffset), from: rowLayer)
            recordMatchedText(key: key,
                              frame: frame,
                              title: visibleName,
                              fontSize: 12,
                              weight: .regular,
                              alignment: .left,
                              isWrapped: false)
            visibleTextKeys.insert(key)
            return true
        }

        let backendName = row.backend.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let match = row.backend.frontends.enumerated().first(where: { _, frontend in
            let frontendName = frontend.name.trimmingCharacters(in: .whitespacesAndNewlines)
            return frontendName.isEmpty || frontendName == backendName
        }) else {
            return false
        }
        let key = frontendIdentityKey(backend: row.backend, frontend: match.element, frontendIndex: match.offset)
        let frame = rootLayer.convert(cellFrame(column: column, y: y, xOffset: xOffset), from: rowLayer)
        recordMatchedText(key: key,
                          frame: frame,
                          title: backendName.isEmpty ? row.backend.serviceID : backendName,
                          fontSize: 12,
                          weight: .medium,
                          alignment: .left,
                          isWrapped: false,
                          italic: italic)
        visibleTextKeys.insert(key)
        return true
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

        let hasRootChoice = (backend.supportsRoot ?? false) && !(backend.rootOnly ?? false)
        let isRootOnly = backend.rootOnly ?? false
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
            let rootButton = makeButtonLayer(title: hasRootChoice ? "Enable for user and root" : "Enable for root", emphasized: true)
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
        sendPasswordFieldCursorUpdate()
    }

    private func renderFilePickerIfNeeded(width: CGFloat, height: CGFloat) {
        filePickerOverlayLayer.removeFromSuperlayer()
        createLayer.addSublayer(filePickerOverlayLayer)
        filePickerOverlayLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        filePickerEntryFrames.removeAll()
        guard let picker = pendingFilePicker else {
            filePickerOverlayLayer.isHidden = true
            filePickerPanelFrame = .zero
            filePickerFilenameFrame = .zero
            filePickerSaveFrame = .zero
            filePickerCancelFrame = .zero
            filePickerListFrame = .zero
            filePickerContentHeight = 0
            return
        }

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

        let isSaveFile = picker.mode == .saveFile
        let bottomControlsHeight: CGFloat = isSaveFile ? 86 : 52
        let buttonTitle = isSaveFile ? "Save" : "Choose"
        let panel = CALayer()
        panel.frame = panelFrame
        panel.cornerRadius = 8
        panel.borderWidth = 1
        panel.backgroundColor = resolvedCGColor(.windowBackgroundColor)
        panel.borderColor = resolvedCGColor(.separatorColor)
        filePickerOverlayLayer.addSublayer(panel)

        let title = makeTextLayer(size: 15, weight: .semibold, color: .labelColor)
        title.string = isSaveFile ? "Save Script" : "Choose Folder"
        title.frame = CGRect(x: 18, y: panelHeight - 38, width: panelWidth - 36, height: 20)
        panel.addSublayer(title)

        let path = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor, monospaced: true)
        path.string = picker.directory
        path.frame = CGRect(x: 18, y: panelHeight - 62, width: panelWidth - 36, height: 17)
        panel.addSublayer(path)

        let listFrame = CGRect(x: 18, y: bottomControlsHeight, width: panelWidth - 36, height: max(panelHeight - bottomControlsHeight - 84, 80))
        filePickerListFrame = listFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        let list = CALayer()
        list.frame = listFrame
        list.cornerRadius = 6
        list.borderWidth = 1
        list.borderColor = resolvedCGColor(.separatorColor)
        list.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.35))
        list.masksToBounds = true
        panel.addSublayer(list)

        let rowHeight: CGFloat = 28
        let parentEntry = FilePickerEntryRecord(name: "..", path: picker.parent, isDirectory: true, size: 0, modified: 0)
        let allEntries = [parentEntry] + picker.entries
        filePickerContentHeight = CGFloat(allEntries.count) * rowHeight
        let maxPickerScroll = max(filePickerContentHeight - listFrame.height, 0)
        filePickerScroll = min(max(filePickerScroll, 0), maxPickerScroll)

        if picker.isLoading {
            let loading = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor, alignment: .center)
            loading.string = "Loading..."
            loading.frame = CGRect(x: 0, y: max((listFrame.height - 18) / 2, 0), width: listFrame.width, height: 18)
            list.addSublayer(loading)
        } else if !picker.error.isEmpty {
            let error = makeTextLayer(size: 12, weight: .regular, color: .systemRed, alignment: .center)
            error.string = picker.error
            error.frame = CGRect(x: 10, y: max((listFrame.height - 18) / 2, 0), width: listFrame.width - 20, height: 18)
            list.addSublayer(error)
        } else {
            let visibleStart = max(Int(floor(filePickerScroll / rowHeight)), 0)
            let visibleEnd = min(allEntries.count, visibleStart + Int(ceil(listFrame.height / rowHeight)) + 2)
            for index in visibleStart..<visibleEnd {
                let entry = allEntries[index]
                let rowY = listFrame.height - CGFloat(index) * rowHeight + filePickerScroll - rowHeight
                renderFilePickerRow(entry, in: list, localFrame: CGRect(x: 0, y: rowY, width: listFrame.width, height: rowHeight), panelFrame: panelFrame, listFrame: listFrame)
            }
        }

        if isSaveFile {
            let filenameLabel = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
            filenameLabel.string = "Filename"
            filenameLabel.frame = CGRect(x: 18, y: 62, width: panelWidth - 36, height: 14)
            panel.addSublayer(filenameLabel)

            let fieldFrame = CGRect(x: 18, y: 28, width: max(panelWidth - 18 - 18 - 156, 120), height: 30)
            filePickerFilenameFrame = fieldFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
            createFieldFrames.append((filePickerFilenameFrame, Self.filePickerFilenameKey))
            let textFrame = CGRect(x: filePickerFilenameFrame.minX + 9,
                                   y: filePickerFilenameFrame.minY + 7,
                                   width: max(filePickerFilenameFrame.width - 18, 1),
                                   height: 16)
            createFieldLayouts[Self.filePickerFilenameKey] = CreateFieldLayout(fieldFrame: filePickerFilenameFrame,
                                                                               textFrame: textFrame,
                                                                               key: Self.filePickerFilenameKey,
                                                                               monospaced: true)

            let field = CALayer()
            field.frame = fieldFrame
            field.cornerRadius = 5
            field.masksToBounds = true
            let focused = createInputController.isFocused && activeCreateFieldKey == Self.filePickerFilenameKey
            field.borderWidth = focused ? 1.5 : 1
            field.borderColor = focused ? resolvedCGColor(.keyboardFocusIndicatorColor) : resolvedCGColor(.separatorColor)
            field.backgroundColor = resolvedCGColor(.textBackgroundColor)
            panel.addSublayer(field)

            let filenameValue = picker.filename
            if focused,
               let selectionRange = createInputController.selectionRange,
               !filenameValue.isEmpty {
                let line = makeCreateFieldLine(for: filenameValue, monospaced: true)
                let offsets = selectionOffsets(line: line,
                                               text: filenameValue,
                                               range: selectionRange,
                                               maxWidth: max(fieldFrame.width - 18, 1))
                let selectionWidth = max(0, offsets.end - offsets.start)
                if selectionWidth > 0.5 {
                    let selection = CALayer()
                    selection.frame = CGRect(x: 9 + offsets.start,
                                             y: 6,
                                             width: selectionWidth,
                                             height: 18)
                    selection.backgroundColor = resolvedCGColor(windowIsActive ? .selectedTextBackgroundColor : .unemphasizedSelectedTextBackgroundColor)
                    field.addSublayer(selection)
                }
            }

            let filenameText = makeTextLayer(size: 12, weight: .regular, color: picker.filename.isEmpty ? .tertiaryLabelColor : .labelColor, monospaced: true)
            filenameText.string = picker.filename.isEmpty ? defaultScriptFilename(for: picker.recipeID ?? "", extension: picker.fileExtension) : picker.filename
            filenameText.frame = CGRect(x: 9, y: 7, width: max(fieldFrame.width - 18, 1), height: 16)
            field.addSublayer(filenameText)
        } else {
            filePickerFilenameFrame = .zero
        }

        let saveLocal = CGRect(x: panelWidth - 18 - 66, y: 28, width: 66, height: 30)
        let cancelLocal = CGRect(x: saveLocal.minX - 78, y: 28, width: 70, height: 30)
        filePickerSaveFrame = saveLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        filePickerCancelFrame = cancelLocal.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)

        let cancel = makeButtonLayer(title: "Cancel", emphasized: false)
        cancel.frame = cancelLocal
        panel.addSublayer(cancel)
        let save = makeButtonLayer(title: buttonTitle, emphasized: true)
        save.frame = saveLocal
        panel.addSublayer(save)
        sendCreateFieldCursorUpdate()
    }

    private func renderFilePickerRow(_ entry: FilePickerEntryRecord,
                                     in list: CALayer,
                                     localFrame: CGRect,
                                     panelFrame: CGRect,
                                     listFrame: CGRect) {
        filePickerEntryFrames.append((localFrame.offsetBy(dx: panelFrame.minX + listFrame.minX,
                                                          dy: panelFrame.minY + listFrame.minY),
                                      entry))
        let row = CALayer()
        row.frame = localFrame
        list.addSublayer(row)

        let icon = CALayer()
        icon.frame = CGRect(x: 22, y: 6, width: 16, height: 16)
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: entry.isDirectory ? "folder" : "doc", pointSize: 15)
        row.addSublayer(icon)

        let name = makeTextLayer(size: 12, weight: .regular, color: .labelColor)
        name.string = entry.name
        name.frame = CGRect(x: 58, y: 6, width: max(localFrame.width - 70, 1), height: 16)
        row.addSublayer(name)
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
        logTextContainer.size = CGSize(width: textWidth, height: max(logTextContainer.size.height, logRowsClipLayer.bounds.height))
        updateLogTextContentIfNeeded(text: currentLogText())
        logTextContainer.size = CGSize(width: textWidth, height: max(estimatedLogContentHeight(textWidth: textWidth) - logTextInsetY * 2, logRowsClipLayer.bounds.height))
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
        logEstimatedContentHeightCache = nil
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
                                    .foregroundColor: color,
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
        let metrics = logVisualLineMetrics(textWidth: textWidth)
        guard !metrics.lines.isEmpty else {
            return
        }

        let firstLine = max(Int(floor(max(selectionRect.minY, 0) / metrics.lineHeight)), 0)
        let lastLine = min(Int(floor(max(selectionRect.maxY - 0.001, 0) / metrics.lineHeight)), metrics.lines.count - 1)
        if firstLine <= lastLine {
            for lineIndex in firstLine...lastLine {
                let line = metrics.lines[lineIndex]
                let intersection = NSIntersectionRange(selectionRange, line.range)
                let selectsEmptyLine = line.range.length == 0 &&
                                       selectionRange.location <= line.range.location &&
                                       selectionRange.location + selectionRange.length > line.range.location
                guard intersection.length > 0 || selectsEmptyLine else {
                    continue
                }

                let startColumn: Int
                let selectedColumnCount: Int
                if selectsEmptyLine {
                    startColumn = 0
                    selectedColumnCount = 1
                } else {
                    startColumn = max(intersection.location - line.range.location, 0)
                    selectedColumnCount = max(intersection.length, 1)
                }

                let x = min(CGFloat(startColumn) * metrics.charWidth, textWidth)
                let width = max(min(CGFloat(selectedColumnCount) * metrics.charWidth, textWidth - x), 1)
                let topDownY = CGFloat(lineIndex) * metrics.lineHeight
                let rect = CGRect(x: x,
                                  y: topDownY,
                                  width: width,
                                  height: metrics.lineHeight)
                guard rect.intersects(selectionRect) else {
                    continue
                }

                let highlight = CALayer()
                highlight.frame = CGRect(x: rect.minX,
                                         y: logTextSelectionLayer.bounds.height - rect.maxY,
                                         width: rect.width,
                                         height: rect.height)
                highlight.backgroundColor = selectionColor
                highlight.cornerRadius = 2
                self.logTextSelectionLayer.addSublayer(highlight)
                self.logTextSelectionLayers.append(highlight)
            }
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

    private func renderCreateForm() {
        createLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        recipeFrames.removeAll()
        bundledAppInstallFrames.removeAll()
        bundledAppMenuFrames.removeAll()
        createFieldFrames.removeAll()
        createFieldLayouts.removeAll()
        createChoiceFrames.removeAll()
        createSuggestionFrames.removeAll()
        createDirectorySelectFrames.removeAll()

        let availableWidth = max(createLayer.bounds.width - horizontalInset * 2, 1)
        let pageWidth = min(availableWidth, 980)
        let left = horizontalInset + floor((availableWidth - pageWidth) / 2)
        let top = max(createLayer.bounds.height - 58, 0) + createScroll

        let pageTitle = makeTextLayer(size: 22, weight: .semibold, color: .labelColor)
        pageTitle.string = "Add Apps"
        pageTitle.frame = CGRect(x: left, y: top, width: pageWidth, height: 28)
        createLayer.addSublayer(pageTitle)

        var contentTop = top - 50
        let addableApps = bundledPlaceholderBackends()
        if !addableApps.isEmpty {
            contentTop = renderAddableAppsSection(apps: addableApps,
                                                  left: left,
                                                  top: contentTop,
                                                  width: pageWidth)
        }

        let title = makeTextLayer(size: 13, weight: .semibold, color: .secondaryLabelColor)
        title.string = "Command Recipes"
        title.frame = CGRect(x: left, y: contentTop, width: pageWidth, height: 18)
        createLayer.addSublayer(title)

        let subtitle = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        subtitle.string = "Run something that is already on this device."
        subtitle.frame = CGRect(x: left, y: contentTop - 24, width: pageWidth, height: 18)
        createLayer.addSublayer(subtitle)

        if recipes.isEmpty {
            let empty = makeTextLayer(size: 13, weight: .regular, color: .secondaryLabelColor)
            empty.string = isLoadingRecipes ? "Loading recipes..." : "No recipes loaded."
            empty.frame = CGRect(x: left, y: contentTop - 58, width: pageWidth, height: 20)
            createLayer.addSublayer(empty)
            createContentBottom = contentTop - 58
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
        let listTop = contentTop - 58
        var cardY = listTop - cardHeight
        let cardWidth = selectorWidth
        for recipe in recipes {
            let frame = CGRect(x: left, y: cardY, width: cardWidth, height: cardHeight)
            recipeFrames.append((frame, recipe.identifier))
            let selected = recipe.identifier == selectedRecipeID
            let card = CALayer()
            card.frame = frame
            card.cornerRadius = 7
            card.borderWidth = selected ? 1.5 : 1
            card.borderColor = selected ? resolvedCGColor(.controlAccentColor) : resolvedCGColor(.separatorColor)
            card.backgroundColor = selected ? resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.12)) : resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.45))
            createLayer.addSublayer(card)

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

        guard let recipe = selectedRecipe() else { return }
        let summary = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        summary.string = recipe.summary
        let formTop = usesTwoPaneLayout ? listTop - 18 : recipeListBottom - 34
        summary.frame = CGRect(x: detailLeft, y: formTop, width: detailWidth, height: 18)
        createLayer.addSublayer(summary)

        var y = formTop - 58
        for field in visibleCreateFields(for: recipe) {
            if field.fieldType == "choice" {
                addCreateChoiceField(field, frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 50))
                y -= 62
            } else {
                addCreateField(field,
                               value: createValue(for: field),
                               frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: field.suggestions.isEmpty ? 46 : 68),
                               monospaced: field.key == "command" || field.key == "executablePath" || field.key == "python")
                y -= field.suggestions.isEmpty ? 62 : 84
            }
        }

        createButtonFrame = CGRect(x: detailLeft, y: y + 14, width: 96, height: 30)
        cancelCreateFrame = CGRect(x: detailLeft + 106, y: y + 14, width: 78, height: 30)
        let createButton = makeButtonLayer(title: isPerformingAction ? "Creating..." : "Create", emphasized: true)
        createButton.frame = createButtonFrame
        createLayer.addSublayer(createButton)
        let cancelButton = makeButtonLayer(title: "Cancel", emphasized: false)
        cancelButton.frame = cancelCreateFrame
        createLayer.addSublayer(cancelButton)

        if !createMessage.isEmpty {
            let message = makeTextLayer(size: 12, weight: .regular, color: createMessage.hasPrefix("Created") ? .secondaryLabelColor : .systemRed)
            message.string = createMessage
            message.frame = CGRect(x: detailLeft, y: y - 26, width: detailWidth, height: 18)
            createLayer.addSublayer(message)
        }
        let formBottom = !createMessage.isEmpty ? y - 26 : y + 14
        createContentBottom = min(recipeListBottom, formBottom)
        sendCreateFieldCursorUpdate()
        if clampCreateScrollUsingRenderedContent() {
            renderCreateForm()
        }
        renderFilePickerIfNeeded(width: createLayer.bounds.width, height: createLayer.bounds.height)
    }

    private func addCreateField(_ field: RecipeFieldRecord,
                                value: String,
                                frame: CGRect,
                                monospaced: Bool = false) {
        let labelLayer = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
        labelLayer.string = field.label
        labelLayer.frame = CGRect(x: frame.minX, y: frame.maxY - 16, width: frame.width, height: 14)
        createLayer.addSublayer(labelLayer)

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
                                                          monospaced: monospaced)
        let box = CALayer()
        box.frame = boxFrame
        box.masksToBounds = true
        box.cornerRadius = 5
        let focused = createInputController.isFocused && activeCreateFieldKey == field.key
        box.borderWidth = focused ? 1.5 : 1
        box.borderColor = focused ? resolvedCGColor(.keyboardFocusIndicatorColor) : resolvedCGColor(.separatorColor)
        box.backgroundColor = resolvedCGColor(.textBackgroundColor)
        createLayer.addSublayer(box)

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

        if hasDirectoryPicker {
            let selectFrame = CGRect(x: boxFrame.maxX + selectGap,
                                     y: frame.minY,
                                     width: min(selectButtonWidth, max(frame.maxX - boxFrame.maxX - selectGap, 1)),
                                     height: 30)
            createDirectorySelectFrames.append((selectFrame, field.key))
            let selectButton = makeButtonLayer(title: "Select", emphasized: false)
            selectButton.frame = selectFrame
            createLayer.addSublayer(selectButton)
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
                createLayer.addSublayer(chip)
                chipX += chipWidth + 8
            }
        }
    }

    private func addCreateChoiceField(_ field: RecipeFieldRecord, frame: CGRect) {
        let labelLayer = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
        labelLayer.string = field.label
        labelLayer.frame = CGRect(x: frame.minX, y: frame.maxY - 16, width: frame.width, height: 14)
        createLayer.addSublayer(labelLayer)

        var x = frame.minX
        let value = createValue(for: field)
        for choice in field.choices {
            let width = max(CGFloat(choice.title.count) * 8 + 24, 72)
            let choiceFrame = CGRect(x: x, y: frame.minY, width: width, height: 30)
            createChoiceFrames.append((choiceFrame, field.key, choice.value))
            let selected = value == choice.value
            let button = makeButtonLayer(title: choice.title, emphasized: selected)
            button.frame = choiceFrame
            createLayer.addSublayer(button)
            x += width + 8
        }
    }

    private func fetchBackends(quiet: Bool = false) {
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
                        self.updateLayout()
                    }
                    return
                }
                if let http = response as? HTTPURLResponse, http.statusCode >= 400 {
                    if !quiet || self.backends.isEmpty {
                        self.backendError = "Backends API returned HTTP \(http.statusCode)."
                        self.updateLayout()
                    }
                    return
                }
                guard let data else {
                    if !quiet || self.backends.isEmpty {
                        self.backendError = "Backends API returned no data."
                        self.updateLayout()
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
                    if let selectedServiceID = self.selectedServiceID,
                       !self.backends.contains(where: { $0.serviceID == selectedServiceID }) {
                        self.clearLogSelection()
                        self.restartEventWatch(resetVersions: true)
                    }
                    if self.selectedServiceID != nil {
                        self.ensureLogSelection()
                    }
                    self.clampScrollOffsets()
                    if !(quiet && self.mode == .apps && previousAppsSignature == nextAppsSignature) {
                        self.updateLayout()
                    }
                    if self.selectedLog != nil {
                        self.fetchSelectedLog()
                    }
                } catch {
                    self.backendError = "Could not decode Backends API response."
                    self.updateLayout()
                }
            }
        }.resume()
    }

    private func fetchRecipes() {
        guard !isLoadingRecipes, let recipesEndpoint, let urlSession else { return }
        isLoadingRecipes = true
        urlSession.dataTask(with: recipesEndpoint) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isLoadingRecipes = false
                if error != nil {
                    self.updateLayout()
                    return
                }
                guard let data else {
                    self.updateLayout()
                    return
                }
                do {
                    let response = try RecipesResponse.decodeBinary(data)
                    self.recipes = response.recipes
                    if !self.recipes.contains(where: { $0.identifier == self.selectedRecipeID }) {
                        self.selectedRecipeID = self.recipes.first?.identifier ?? "command-port"
                    }
                    self.applyRecipeDefaults(overwrite: false)
                    self.updateLayout()
                } catch {
                    self.updateLayout()
                }
            }
        }.resume()
    }

    private func fetchSelectedLog(quiet: Bool = false, scrollToBottom: Bool = false) {
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
        components?.queryItems = [
            URLQueryItem(name: "serviceID", value: selection.serviceID),
            URLQueryItem(name: "logIndex", value: String(selection.logIndex)),
            URLQueryItem(name: "bytes", value: "262144")
        ]
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

    private func performControlAction(for backend: BackendRecord, operation: String, sudoPassword: String? = nil) {
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
        urlSession.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                var actionCompleted = false
                if let error {
                    self.backendError = error.localizedDescription
                } else if let data,
                          let response = try? ActionResponse.decodeBinary(data) {
                    if response.needsPassword == true {
                        self.showPasswordPrompt(for: backend, operation: operation, message: response.message)
                        self.backendError = ""
                    } else {
                        self.backendError = response.ok ? "" : response.message
                        actionCompleted = response.ok
                    }
                } else {
                    self.backendError = "Control request failed."
                }
                if actionCompleted {
                    self.applyOptimisticStatus(for: backend.serviceID, operation: operation)
                    self.scheduleBackendsRefreshes()
                }
                self.fetchBackends()
            }
        }.resume()
    }

    private func setFrontendList(for item: AppLauncherItem, listName: String) {
        guard !isPerformingAction, let controlEndpoint, let urlSession else { return }
        isPerformingAction = true
        backendsRefreshGeneration += 1
        backendError = listName.isEmpty ? "Moving \(item.displayName) to Apps..." : "Moving \(item.displayName) to \(listName)..."
        updateLayout()

        var components = URLComponents(url: controlEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "serviceID", value: item.backend.serviceID),
            URLQueryItem(name: "operation", value: "setFrontendList")
        ]
        guard let url = components?.url else {
            isPerformingAction = false
            backendError = "Could not build list update request."
            updateLayout()
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/x-www-form-urlencoded; charset=utf-8", forHTTPHeaderField: "Content-Type")
        request.httpBody = formEncodedBody([
            "frontendID": item.frontend.id,
            "frontendURL": item.frontend.url,
            "list": listName
        ])
        urlSession.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                if let error {
                    self.backendError = error.localizedDescription
                } else if let data,
                          let response = try? ActionResponse.decodeBinary(data) {
                    self.backendError = response.ok ? "" : response.message
                    if response.ok {
                        self.applyOptimisticFrontendList(serviceID: item.backend.serviceID,
                                                         frontendURL: item.frontend.url,
                                                         listName: listName)
                    } else {
                        self.updateLayout()
                    }
                } else {
                    self.backendError = "List update request failed."
                    self.updateLayout()
                }
            }
        }.resume()
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

    private func applyOptimisticStatus(for serviceID: String, operation: String) {
        let status: String
        switch operation {
        case "stop":
            status = "stopped"
        case "start", "restart", "run", "install", "runUser", "installUser", "runRoot", "installRoot", "addRootSupport":
            status = "running"
        default:
            return
        }
        backends = backends.map { backend in
            guard backend.serviceID == serviceID else { return backend }
            return BackendRecord(serviceID: backend.serviceID,
                                 displayName: backend.displayName,
                                 serviceUnit: backend.serviceUnit,
                                 serviceUnitPath: backend.serviceUnitPath,
                                 serviceScope: backend.serviceScope,
                                 status: status,
                                 canControl: backend.canControl,
                                 canUninstall: backend.canUninstall,
                                 isBundled: backend.isBundled,
                                 isInstalled: operation == "run" || operation == "install" || operation == "runUser" || operation == "installUser" || operation == "runRoot" || operation == "installRoot" ? true : backend.isInstalled,
                                 isMigration: backend.isMigration,
                                 supportsRoot: backend.supportsRoot,
                                 rootOnly: backend.rootOnly,
                                 hasRootSupport: operation == "runRoot" || operation == "installRoot" || operation == "addRootSupport" ? true : (operation == "removeRootSupport" ? false : backend.hasRootSupport),
                                 iconSymbolName: backend.iconSymbolName,
                                 launchdPlistPath: backend.launchdPlistPath,
                                 ownsLaunchdPlist: backend.ownsLaunchdPlist,
                                 frontends: backend.frontends,
                                 logFiles: backend.logFiles)
        }
        updateLayout()
    }

    private func applyOptimisticFrontendList(serviceID: String, frontendURL: String, listName: String) {
        backends = backends.map { backend in
            guard backend.serviceID == serviceID else { return backend }
            let frontends = backend.frontends.map { frontend in
                guard frontend.url == frontendURL else { return frontend }
                return FrontendRecord(id: frontend.id,
                                      name: frontend.name,
                                      url: frontend.url,
                                      port: frontend.port,
                                      socketPath: frontend.socketPath,
                                      iconPath: frontend.iconPath,
                                      iconData: frontend.iconData,
                                      list: listName.isEmpty ? nil : listName,
                                      isRunning: frontend.isRunning)
            }
            return BackendRecord(serviceID: backend.serviceID,
                                 displayName: backend.displayName,
                                 serviceUnit: backend.serviceUnit,
                                 serviceUnitPath: backend.serviceUnitPath,
                                 serviceScope: backend.serviceScope,
                                 status: backend.status,
                                 canControl: backend.canControl,
                                 canUninstall: backend.canUninstall,
                                 isBundled: backend.isBundled,
                                 isInstalled: backend.isInstalled,
                                 isMigration: backend.isMigration,
                                 supportsRoot: backend.supportsRoot,
                                 rootOnly: backend.rootOnly,
                                 hasRootSupport: backend.hasRootSupport,
                                 iconSymbolName: backend.iconSymbolName,
                                 launchdPlistPath: backend.launchdPlistPath,
                                 ownsLaunchdPlist: backend.ownsLaunchdPlist,
                                 frontends: frontends,
                                 logFiles: backend.logFiles)
        }
        updateLayout()
    }

    private func openFrontend(for row: BackendListRow, opensInNewTab: Bool) {
        guard let frontend = row.frontend,
              let url = frontendNavigationURL(frontend) else {
            backendError = "Could not build frontend URL."
            updateLayout()
            return
        }

        let displayName = frontend.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            ? row.backend.displayName
            : frontend.name
        if opensInNewTab {
            outerframeHost.openNewTab(with: url, displayString: displayName)
        } else {
            outerframeHost.navigate(to: url)
        }
    }

    private func openLauncherItem(_ item: AppLauncherItem, opensInNewTab: Bool) {
        openLauncherEndpoint(item.primaryEndpoint, displayName: item.displayName, opensInNewTab: opensInNewTab)
    }

    private func canOpenStoppedSocketActivatedFrontend(_ endpoint: AppLauncherEndpoint) -> Bool {
        !endpoint.frontend.socketPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        endpoint.backend.status == "available"
    }

    private func openLauncherEndpoint(_ endpoint: AppLauncherEndpoint, displayName: String, opensInNewTab: Bool) {
        if !endpoint.frontend.isRunning,
           !canOpenStoppedSocketActivatedFrontend(endpoint) {
            startAndOpenLauncherEndpoint(endpoint, displayName: displayName, opensInNewTab: opensInNewTab)
            return
        }
        guard let url = frontendNavigationURL(endpoint.frontend) else {
            startAndOpenLauncherEndpoint(endpoint, displayName: displayName, opensInNewTab: opensInNewTab)
            return
        }

        if opensInNewTab {
            outerframeHost.openNewTab(with: url, displayString: displayName)
        } else {
            outerframeHost.navigate(to: url)
        }
    }

    private func startAndOpenLauncherEndpoint(_ endpoint: AppLauncherEndpoint, displayName: String, opensInNewTab: Bool) {
        guard !isPerformingAction, let controlEndpoint, let urlSession else { return }
        isPerformingAction = true
        backendError = "Starting \(displayName)..."
        updateLayout()

        var components = URLComponents(url: controlEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "serviceID", value: endpoint.backend.serviceID),
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
                                             attempt: 0)
            }
        }.resume()
    }

    private func waitForLauncherEndpoint(_ endpoint: AppLauncherEndpoint,
                                         displayName: String,
                                         opensInNewTab: Bool,
                                         attempt: Int) {
        guard let backendsEndpoint, let urlSession else { return }
        backendError = "Waiting for \(displayName)..."
        updateLayout()
        urlSession.dataTask(with: backendsEndpoint) { [weak self] data, _, _ in
            Task { @MainActor in
                guard let self else { return }
                if let data,
                   let response = try? BackendsResponse.decodeBinary(data) {
                    self.backends = response.backends
                    if let nextEndpoint = self.findLauncherEndpoint(serviceID: endpoint.backend.serviceID,
                                                                    frontendID: endpoint.frontend.id),
                       let url = self.frontendNavigationURL(nextEndpoint.frontend),
                       nextEndpoint.frontend.isRunning || self.canOpenStoppedSocketActivatedFrontend(nextEndpoint) {
                        self.backendError = ""
                        self.updateLayout()
                        if opensInNewTab {
                            self.outerframeHost.openNewTab(with: url, displayString: displayName)
                        } else {
                            self.outerframeHost.navigate(to: url)
                        }
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
                                                 attempt: attempt + 1)
                }
            }
        }.resume()
    }

    private func findLauncherEndpoint(serviceID: String, frontendID: String) -> AppLauncherEndpoint? {
        for backend in backends where backend.serviceID == serviceID {
            for (index, frontend) in backend.frontends.enumerated() where frontend.id == frontendID {
                return AppLauncherEndpoint(backend: backend, frontend: frontend, frontendIndex: index)
            }
        }
        return nil
    }

    private func performAppMenuAction(_ item: AppLauncherItem, operation: String) {
        if operation.hasPrefix("showLogs:") {
            let serviceID = String(operation.dropFirst("showLogs:".count))
            guard let backend = backends.first(where: { $0.serviceID == serviceID }) else { return }
            showLogs(for: backend)
            return
        }

        switch operation {
        case "run":
            if let userEndpoint = item.userEndpoint {
                openLauncherEndpoint(userEndpoint, displayName: item.displayName, opensInNewTab: false)
            } else {
                openLauncherItem(item, opensInNewTab: false)
            }
        case "runRoot":
            if let rootEndpoint = item.rootEndpoint {
                openLauncherEndpoint(rootEndpoint, displayName: item.displayName, opensInNewTab: false)
            } else {
                performControlAction(for: item.backend, operation: "runRoot")
            }
        default:
            performControlAction(for: item.backend, operation: operation)
        }
    }

    private func submitCreateForm() {
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
        if needsScriptPathPicker(for: recipe),
           createValues["scriptPath", default: ""].trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            showFilePicker(for: recipe)
            return
        }
        performCreateRequest(recipe: recipe, createEndpoint: createEndpoint, urlSession: urlSession)
    }

    private func performCreateRequest(recipe: RecipeRecord, createEndpoint: URL, urlSession: URLSession) {
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
        if recipe.fields.contains(where: { $0.key == "scriptPath" }) {
            queryItems.append(URLQueryItem(name: "scriptPath", value: createValues["scriptPath", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)))
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

    private func showFilePicker(for recipe: RecipeRecord) {
        blurCreateField()
        let defaultPath = recipe.fields.first(where: { $0.key == "scriptPath" })?.placeholder ?? ""
        let fileExtension = scriptFileExtension(for: recipe)
        var split = splitScriptPath(defaultPath.isEmpty ? "~/dev/run-service\(fileExtension)" : defaultPath)
        let typedIdentifier = createValues["identifier", default: ""].trimmingCharacters(in: .whitespacesAndNewlines)
        split.filename = defaultScriptFilename(for: typedIdentifier.isEmpty ? recipe.identifier : typedIdentifier,
                                               extension: fileExtension)
        let preferredDirectory = createValues["projectDir"] ?? createValues["workdir"] ?? ""
        if !preferredDirectory.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            split.directory = preferredDirectory
        }
        filePickerScroll = 0
        pendingFilePicker = PendingFilePicker(mode: .saveFile,
                                              recipeID: recipe.identifier,
                                              targetFieldKey: nil,
                                              fileExtension: fileExtension,
                                              directory: split.directory,
                                              parent: split.directory,
                                              filename: split.filename,
                                              entries: [],
                                              isLoading: true,
                                              error: "")
        createValues[Self.filePickerFilenameKey] = split.filename
        focusCreateField(Self.filePickerFilenameKey)
        fetchFilePickerDirectory(path: split.directory)
        updateLayout()
    }

    private func showDirectoryPicker(for key: String) {
        blurCreateField()
        let currentDirectory = createValues[key] ?? selectedRecipe()?.fields.first(where: { $0.key == key })?.defaultValue ?? "~"
        let directory = currentDirectory.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? "~" : currentDirectory
        filePickerScroll = 0
        pendingFilePicker = PendingFilePicker(mode: .chooseDirectory,
                                              recipeID: nil,
                                              targetFieldKey: key,
                                              fileExtension: "",
                                              directory: directory,
                                              parent: directory,
                                              filename: "",
                                              entries: [],
                                              isLoading: true,
                                              error: "")
        fetchFilePickerDirectory(path: directory)
        updateLayout()
    }

    private func fetchFilePickerDirectory(path: String) {
        guard let filePickerEndpoint, let urlSession else { return }
        if pendingFilePicker != nil {
            pendingFilePicker?.directory = path
            pendingFilePicker?.entries = []
            pendingFilePicker?.isLoading = true
            pendingFilePicker?.error = ""
            filePickerScroll = 0
            updateLayout()
        }
        var components = URLComponents(url: filePickerEndpoint, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "path", value: path),
            URLQueryItem(name: "extension", value: pendingFilePicker?.fileExtension ?? ""),
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
                } catch {
                    self.pendingFilePicker?.error = "Could not decode directory."
                }
                self.updateLayout()
            }
        }.resume()
    }

    private func confirmFilePickerSave() {
        guard var picker = pendingFilePicker else { return }
        if picker.mode == .chooseDirectory {
            guard let targetFieldKey = picker.targetFieldKey else { return }
            createValues[targetFieldKey] = picker.directory
            createValues.removeValue(forKey: Self.filePickerFilenameKey)
            pendingFilePicker = nil
            filePickerScroll = 0
            focusCreateField(targetFieldKey, cursorPosition: picker.directory.count)
            createMessage = ""
            updateLayout()
            return
        }
        guard let recipeID = picker.recipeID,
              let recipe = recipes.first(where: { $0.identifier == recipeID }),
              let createEndpoint,
              let urlSession else { return }
        var filename = picker.filename.trimmingCharacters(in: .whitespacesAndNewlines)
        if filename.isEmpty {
            pendingFilePicker?.error = "Enter a filename."
            updateLayout()
            return
        }
        if !filename.lowercased().hasSuffix(picker.fileExtension.lowercased()) {
            if filename.contains(".") {
                pendingFilePicker?.error = "Use a \(picker.fileExtension) filename."
                updateLayout()
                return
            }
            filename += picker.fileExtension
        }
        picker.filename = filename
        createValues["scriptPath"] = joinPath(directory: picker.directory, filename: filename)
        createValues.removeValue(forKey: Self.filePickerFilenameKey)
        pendingFilePicker = nil
        blurCreateField()
        performCreateRequest(recipe: recipe, createEndpoint: createEndpoint, urlSession: urlSession)
    }

    private func dismissFilePicker() {
        pendingFilePicker = nil
        createValues.removeValue(forKey: Self.filePickerFilenameKey)
        filePickerScroll = 0
        blurCreateField()
        updateLayout()
    }

    func textInputControllerDidChangeState() {
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
            sendFocusedTextCursorUpdate()
            return
        }

        let oldNameSuggestion = suggestedIdentifier(from: createValues["name", default: ""])
        createValues[key] = createInputController.text
        if key == Self.filePickerFilenameKey {
            pendingFilePicker?.filename = createInputController.text
            updateInputMode()
            updateEditingAndPasteboardState()
            updateLayout()
            return
        }
        if key == "name" {
            let identifier = createValues["identifier", default: ""]
            if identifier.isEmpty || identifier == oldNameSuggestion {
                createValues["identifier"] = suggestedIdentifier(from: createValues["name", default: ""])
            }
        }
        createMessage = ""
        updateInputMode()
        updateEditingAndPasteboardState()
        updateLayout()
    }

    private func handleCreateKeyDown(keyCode: UInt16, characters: String?) {
        if pendingFilePicker != nil {
            handleFilePickerKeyDown(keyCode: keyCode, characters: characters)
            return
        }
        if createInputController.isFocused {
            if keyCode == 48 {
                advanceCreateField()
            }
            return
        }
        switch keyCode {
        case 48:
            advanceCreateField()
        case 51, 117:
            focusActiveCreateField()
            createInputController.deleteBackward()
        case 36, 76:
            submitCreateForm()
        case 53:
            returnToAppsFromCreate()
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
            confirmFilePickerSave()
        case 51, 117:
            focusCreateField(Self.filePickerFilenameKey)
            createInputController.deleteBackward()
        case 53:
            dismissFilePicker()
        default:
            if let characters, !characters.isEmpty {
                insertCreateText(characters)
            }
        }
    }

    private func advanceCreateField() {
        if pendingFilePicker != nil {
            focusCreateField(Self.filePickerFilenameKey)
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
        let cleaned = cleanSingleLineText(text)
        guard !cleaned.isEmpty else { return }
        createInputController.insertText(cleaned)
    }

    private func cleanSingleLineText(_ text: String) -> String {
        text.filter { character in
            !character.isNewline && character.unicodeScalars.allSatisfy { $0.value >= 0x20 && $0.value != 0x7f }
        }
    }

    private func focusActiveCreateField(selectAll: Bool = false) {
        let firstVisibleKey = selectedRecipe().flatMap { recipe in
            visibleCreateFields(for: recipe).first(where: { $0.fieldType != "choice" })?.key
        }
        let key = pendingFilePicker != nil ? Self.filePickerFilenameKey : activeCreateFieldKey ?? firstVisibleKey
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
        sendPasswordFieldCursorUpdate()
    }

    private func blurPasswordField() {
        passwordInputController.blur()
        updateInputMode()
        updateEditingAndPasteboardState()
        if !createInputController.isFocused {
            outerframeHost.sendTextCursorUpdate(cursors: [])
        }
    }

    private func focusCreateField(_ key: String, selectAll: Bool = false, cursorPosition: Int? = nil) {
        blurPasswordField()
        activeCreateFieldKey = key
        let value = createValues[key] ?? (key == Self.filePickerFilenameKey ? pendingFilePicker?.filename : nil) ?? selectedRecipe()?.fields.first(where: { $0.key == key })?.defaultValue ?? ""
        isSynchronizingCreateInput = true
        createInputController.setText(value)
        createInputController.focus(selectAll: selectAll)
        if let cursorPosition {
            createInputController.setCursorPosition(cursorPosition, modifySelection: false)
        }
        isSynchronizingCreateInput = false
        updateInputMode()
        updateEditingAndPasteboardState()
        sendCreateFieldCursorUpdate()
    }

    private func blurCreateField() {
        createInputController.blur()
        updateInputMode()
        updateEditingAndPasteboardState()
        outerframeHost.sendTextCursorUpdate(cursors: [])
    }

    private func handleTextCommand(_ command: String) {
        if pendingPasswordAction != nil, passwordInputController.isFocused {
            if command == "cancelOperation" {
                dismissPasswordPrompt()
                return
            }
            passwordInputController.performCommand(command)
            return
        }

        guard mode == .create, createInputController.isFocused else { return }
        if command == "insertTab" || command == "insertBacktab" {
            advanceCreateField()
            return
        }
        if command == "cancelOperation" {
            if pendingFilePicker != nil {
                dismissFilePicker()
            } else {
                returnToAppsFromCreate()
            }
            return
        }
        createInputController.performCommand(command)
    }

    private func handleTextInputFocus(fieldID: UUID, hasFocus: Bool) {
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
        outerframeHost.setInputMode((createInputController.isFocused || passwordInputController.isFocused) ? .textInput : .rawKeys)
    }

    private func updateEditingAndPasteboardState() {
        if passwordInputController.isFocused {
            outerframeHost.setEditingCapabilities(canCopy: false, canCut: false)
            outerframeHost.setAcceptedPasteboardPasteTypes(Self.passwordFieldPasteboardTypes)
            return
        }

        if createInputController.isFocused {
            let capabilities = createInputController.currentEditingCapabilities()
            outerframeHost.setEditingCapabilities(canCopy: capabilities.canCopy, canCut: capabilities.canCut)
            outerframeHost.setAcceptedPasteboardPasteTypes(createInputController.currentAcceptedPasteboardTypeIdentifiers())
            return
        }

        outerframeHost.setEditingCapabilities(canCopy: (logHeaderDetailSelectionRange?.length ?? 0) > 0 ||
                                              (logTextSelectionRange?.length ?? 0) > 0,
                                              canCut: false)
        outerframeHost.setAcceptedPasteboardPasteTypes([])
    }

    private func createInputFont(monospaced: Bool) -> NSFont {
        monospaced ? NSFont.monospacedSystemFont(ofSize: 12, weight: .regular) : NSFont.systemFont(ofSize: 12, weight: .regular)
    }

    private func makeCreateFieldLine(for text: String, monospaced: Bool) -> CTLine {
        let attributed = NSAttributedString(string: text, attributes: [.font: createInputFont(monospaced: monospaced)])
        return CTLineCreateWithAttributedString(attributed)
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

    private func characterIndexForCreateField(key: String, xPosition: CGFloat) -> Int {
        guard let layout = createFieldLayouts[key] else { return 0 }
        let text = createValues[key] ?? selectedRecipe()?.fields.first(where: { $0.key == key })?.defaultValue ?? ""
        let localX = max(0, min(xPosition - layout.textFrame.minX, layout.textFrame.width))
        guard !text.isEmpty, layout.textFrame.width > 0 else { return 0 }
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

    private func sendCreateFieldCursorUpdate() {
        guard mode == .create,
              createInputController.isFocused,
              !createInputController.hasSelection,
              let key = activeCreateFieldKey,
              let layout = createFieldLayouts[key] else {
            outerframeHost.sendTextCursorUpdate(cursors: [])
            return
        }
        let line = createInputController.text.isEmpty ? nil : makeCreateFieldLine(for: createInputController.text, monospaced: layout.monospaced)
        let cursorFrame = createFieldCursorRect(layout: layout, cachedLine: line)
        let rootPosition = createLayer.convert(cursorFrame.origin, to: rootLayer)
        let topLeftY = rootLayer.bounds.height - rootPosition.y - cursorFrame.height
        let cursor = OuterframeContentTextCursorSnapshot(fieldID: Self.createFieldInputID,
                                                         rect: CGRect(x: rootPosition.x,
                                                                      y: topLeftY,
                                                                      width: cursorFrame.width,
                                                                      height: cursorFrame.height),
                                                         visible: true)
        outerframeHost.sendTextCursorUpdate(cursors: [cursor])
    }

    private func sendPasswordFieldCursorUpdate() {
        let bulletString = String(repeating: "\u{2022}", count: passwordInputController.text.count)
        guard pendingPasswordAction != nil,
              passwordInputController.isFocused,
              !passwordInputController.hasSelection,
              passwordFieldFrame != .zero,
              passwordTextFrame != .zero else {
            if !createInputController.isFocused {
                outerframeHost.sendTextCursorUpdate(cursors: [])
            }
            return
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
        let cursorFrame = CGRect(x: max(passwordTextFrame.minX, min(proposedX, maxCursorX)),
                                 y: passwordTextFrame.minY - 1,
                                 width: cursorWidth,
                                 height: passwordTextFrame.height + 2)
        let topLeftY = rootLayer.bounds.height - cursorFrame.minY - cursorFrame.height
        let cursor = OuterframeContentTextCursorSnapshot(fieldID: Self.passwordFieldInputID,
                                                         rect: CGRect(x: cursorFrame.minX,
                                                                      y: topLeftY,
                                                                      width: cursorFrame.width,
                                                                      height: cursorFrame.height),
                                                         visible: true)
        outerframeHost.sendTextCursorUpdate(cursors: [cursor])
    }

    private func sendFocusedTextCursorUpdate() {
        if passwordInputController.isFocused {
            sendPasswordFieldCursorUpdate()
        } else {
            sendCreateFieldCursorUpdate()
        }
    }

    private func pasteboardItemsForCopy() -> [OuterframeContentPasteboardItem] {
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

        if let selectedText = selectedLogHeaderDetailAttributedText(),
           selectedText.length > 0 {
            return pasteboardItems(for: selectedText)
        }

        guard let selectedText = selectedLogAttributedText(),
              selectedText.length > 0 else {
            return []
        }

        return pasteboardItems(for: selectedText)
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
        if pendingPasswordAction != nil, passwordInputController.isFocused {
            _ = insertPasteboardItemsIntoPasswordField(items)
            return
        }
        guard mode == .create, createInputController.isFocused else { return }
        _ = insertPasteboardItemsIntoCreateField(items)
    }

    @discardableResult
    private func insertPasteboardItemsIntoCreateField(_ items: [OuterframeContentPasteboardItem]) -> Bool {
        for item in items {
            if let representation = item.representations.first(where: { $0.typeIdentifier == NSPasteboard.PasteboardType.string.rawValue }),
               let stringValue = String(data: representation.data, encoding: .utf8) {
                createInputController.insertText(cleanSingleLineText(stringValue))
                return true
            }
            if let representation = item.representations.first(where: { $0.typeIdentifier == NSPasteboard.PasteboardType.rtf.rawValue }),
               let attributed = try? NSAttributedString(data: representation.data,
                                                        options: [.documentType: NSAttributedString.DocumentType.rtf],
                                                        documentAttributes: nil) {
                createInputController.insertText(cleanSingleLineText(attributed.string))
                return true
            }
        }
        return false
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
        if pendingFilePicker != nil {
            guard filePickerFilenameFrame.contains(createPoint) else { return nil }
            return (Self.filePickerFilenameKey, createPoint)
        }
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

    private func handlePasteboardItemsForDrop(at point: CGPoint, items: [OuterframeContentPasteboardItem]) {
        if passwordFieldFrame.contains(point), pendingPasswordAction != nil {
            let index = characterIndexForPasswordField(xPosition: point.x)
            focusPasswordField(cursorPosition: index)
            _ = insertPasteboardItemsIntoPasswordField(items)
            return
        }

        guard let drop = createFieldDropPoint(point) else { return }
        let index = characterIndexForCreateField(key: drop.key, xPosition: drop.point.x)
        focusCreateField(drop.key, cursorPosition: index)
        _ = insertPasteboardItemsIntoCreateField(items)
    }

    @discardableResult
    private func handleLogHeaderMouseDown(at point: CGPoint,
                                          modifierFlags: NSEvent.ModifierFlags,
                                          clickCount: Int) -> Bool {
        _ = modifierFlags
        if isPointInLogDismissButton(point) {
            dismissLogViewer()
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

    private func handleMouseDragged(to point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        _ = modifierFlags
        if logScrollbarController?.handleMouseDragged(to: rootLayer.convert(point, to: logRowsClipLayer)) == true {
            return
        }
        if handleLogHeaderMouseDragged(to: point) {
            return
        }
        if handleLogMouseDragged(to: point) {
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
                updateLayout()
            }
            return
        }
        guard let pendingCreateTextDrag else { return }
        let dx = point.x - pendingCreateTextDrag.startPoint.x
        let dy = point.y - pendingCreateTextDrag.startPoint.y
        guard hypot(dx, dy) >= 3 else { return }
        self.pendingCreateTextDrag = nil
        beginDraggingSelectedCreateText(pendingCreateTextDrag.selectedText)
    }

    private func handleMouseUp(at point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        logDragAnchorOffset = nil
        logHeaderDetailDragAnchorOffset = nil
        lastLogDragTextPoint = nil
        _ = logScrollbarController?.handleMouseUp(at: rootLayer.convert(point, to: logRowsClipLayer))
        if let pendingAppDrag {
            self.pendingAppDrag = nil
            if pendingAppDrag.isDragging {
                let contentPoint = contentLayer.convert(point, from: rootLayer)
                let appsPoint = appsScrollContentLayer.convert(contentPoint, from: contentLayer)
                if let target = appDropTarget(at: appsPoint, for: pendingAppDrag.item),
                   target.listName != pendingAppDrag.item.frontend.listName {
                    setFrontendList(for: pendingAppDrag.item, listName: target.listName)
                } else {
                    updateLayout()
                }
            } else {
                openLauncherItem(pendingAppDrag.item, opensInNewTab: modifierFlags.contains(.command))
            }
            setCursorIfNeeded(.arrow)
            return
        }
        guard let pendingCreateTextDrag else { return }
        self.pendingCreateTextDrag = nil
        focusActiveCreateField()
        createInputController.setCursorPosition(pendingCreateTextDrag.cursorIndex, modifySelection: false)
    }

    private func handleMouseMoved(to point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        _ = modifierFlags
        if pendingInstallBackend != nil {
            setCursorIfNeeded((installConfirmFrame.contains(point) || installRootConfirmFrame.contains(point) || installCancelFrame.contains(point)) ? .pointingHand : .arrow)
            return
        }
        if pendingFilePicker != nil, mode == .create {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            if filePickerFilenameFrame.contains(createPoint) {
                setCursorIfNeeded(.iBeam)
            } else if filePickerSaveFrame.contains(createPoint) ||
                        filePickerCancelFrame.contains(createPoint) ||
                        filePickerEntryFrames.contains(where: { $0.frame.contains(createPoint) }) {
                setCursorIfNeeded(.pointingHand)
            } else {
                setCursorIfNeeded(.arrow)
            }
            return
        }
        let isOverCreateField = pendingPasswordAction == nil && createFieldDropPoint(point) != nil
        let isOverPasswordField = pendingPasswordAction != nil && passwordFieldFrame.contains(point)
        var isOverBundledApp = false
        var isOverDirectorySelect = false
        var isOverAppTile = false
        if pendingPasswordAction == nil, mode == .create {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            isOverBundledApp = bundledAppInstallFrames.contains { $0.frame.contains(createPoint) }
            isOverDirectorySelect = createDirectorySelectFrames.contains { $0.frame.contains(createPoint) }
        } else if pendingPasswordAction == nil, mode == .apps {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let appsPoint = appsScrollContentLayer.convert(contentPoint, from: contentLayer)
            let toolbarPoint = toolbarLayer.convert(point, from: rootLayer)
            isOverAppTile = appCardFrames.contains { $0.frame.contains(appsPoint) } ||
                            appBadgeFrames.contains { $0.frame.contains(appsPoint) } ||
                            addAppFrame.contains(appsPoint) ||
                            outerShellActionFrame.contains(toolbarPoint)
        }
        if isOverCreateField || isOverPasswordField {
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
            outerframeHost.showContextMenu(for: NSAttributedString(string: ""), at: point)
            return
        }

        if pendingFilePicker != nil, mode == .create {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            guard filePickerFilenameFrame.contains(createPoint) else { return }
            let index = characterIndexForCreateField(key: Self.filePickerFilenameKey, xPosition: createPoint.x)
            focusCreateField(Self.filePickerFilenameKey)
            if !createInputController.hasSelection {
                createInputController.setCursorPosition(index, modifySelection: false)
            }
            updateEditingAndPasteboardState()
            let selectedText = createInputController.selectedTextContent() ?? ""
            let attributedText = NSAttributedString(string: selectedText,
                                                    attributes: [.font: createInputFont(monospaced: true)])
            outerframeHost.showContextMenu(for: attributedText, at: point)
            return
        }

        let contentPoint = contentLayer.convert(point, from: rootLayer)
        if mode == .apps {
            let toolbarPoint = toolbarLayer.convert(point, from: rootLayer)
            if outerShellActionFrame.contains(toolbarPoint),
               let backend = outerShellBackend() {
                showBackendActionsMenu(for: backend, at: point)
                return
            }
            let appsPoint = appsScrollContentLayer.convert(contentPoint, from: contentLayer)
            if let card = appCardFrames.first(where: { $0.frame.contains(appsPoint) }) {
                showAppActionsMenu(for: card.item, at: point)
            }
            return
        }

        guard pendingPasswordAction == nil, mode == .create else { return }
        let createPoint = createLayer.convert(contentPoint, from: contentLayer)
        guard let key = createFieldFrames.first(where: { $0.frame.contains(createPoint) })?.key else { return }
        let index = characterIndexForCreateField(key: key, xPosition: createPoint.x)
        focusCreateField(key)
        if !createInputController.hasSelection {
            createInputController.setCursorPosition(index, modifySelection: false)
        }
        updateEditingAndPasteboardState()
        let selectedText = createInputController.selectedTextContent() ?? ""
        let attributedText = NSAttributedString(string: selectedText,
                                                attributes: [.font: createInputFont(monospaced: createFieldLayouts[key]?.monospaced ?? false)])
        outerframeHost.showContextMenu(for: attributedText, at: point)
    }

    private func handleScroll(at point: CGPoint, delta: CGPoint, precise: Bool) {
        let multiplier: CGFloat = precise ? 1 : backendRowHeight
        let previousAppsScroll = appsScroll
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
            } else {
                appsScroll -= delta.y * multiplier
            }
        } else if mode == .create {
            if pendingFilePicker != nil {
                let contentPoint = contentLayer.convert(point, from: rootLayer)
                let createPoint = createLayer.convert(contentPoint, from: contentLayer)
                if filePickerListFrame.contains(createPoint) {
                    filePickerScroll -= delta.y * multiplier
                    let maxPickerScroll = max(filePickerContentHeight - filePickerListFrame.height, 0)
                    filePickerScroll = min(max(filePickerScroll, 0), maxPickerScroll)
                }
            } else {
                createScroll -= delta.y * multiplier
            }
        }
        clampScrollOffsets()
        if mode == .apps {
            scrollCurrentModeWithoutRerender(deltaY: appsScroll - previousAppsScroll)
        } else {
            updateLayout()
        }
    }

    private func handleMouseDown(at point: CGPoint,
                                 modifierFlags: NSEvent.ModifierFlags = [],
                                 clickCount: Int = 1) {
        pendingCreateTextDrag = nil
        if pendingInstallBackend != nil {
            if installConfirmFrame.contains(point) {
                pendingInstallOperation = "run"
                confirmPendingInstall()
            } else if installRootConfirmFrame.contains(point) {
                pendingInstallOperation = "runRoot"
                confirmPendingInstall()
            } else if installCancelFrame.contains(point) || !installPanelFrame.contains(point) {
                dismissInstallPrompt()
            }
            return
        }
        if pendingPasswordAction != nil {
            if passwordSubmitFrame.contains(point) {
                submitPasswordPrompt()
            } else if passwordFieldFrame.contains(point) {
                let wasFocused = passwordInputController.isFocused
                let index = characterIndexForPasswordField(xPosition: point.x)
                focusPasswordField(selectAll: clickCount >= 3)
                switch clickCount {
                case 3...:
                    passwordInputController.selectAll()
                case 2:
                    passwordInputController.selectWord(at: index)
                default:
                    passwordInputController.setCursorPosition(index,
                                                              modifySelection: modifierFlags.contains(.shift) && wasFocused)
                }
                updateLayout()
            } else if passwordCancelFrame.contains(point) || !passwordPanelFrame.contains(point) {
                dismissPasswordPrompt()
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
           let backend = outerShellBackend() {
            showBackendActionsMenu(for: backend, at: point)
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
            let appsPoint = appsScrollContentLayer.convert(contentPoint, from: contentLayer)
            if let badge = appBadgeFrames.first(where: { $0.frame.contains(appsPoint) }) {
                openLauncherEndpoint(badge.endpoint,
                                     displayName: badge.displayName,
                                     opensInNewTab: modifierFlags.contains(.command))
                return
            }
            if addAppFrame.contains(appsPoint) {
                navigateToMode(.create, pushHistory: true)
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
            if let menu = bundledAppMenuFrames.first(where: { $0.frame.contains(createPoint) }) {
                showBackendActionsMenu(for: menu.backend, at: point)
                return
            }
            if let install = bundledAppInstallFrames.first(where: { $0.frame.contains(createPoint) }) {
                showInstallPrompt(for: install.backend)
                return
            }
            if let select = createDirectorySelectFrames.first(where: { $0.frame.contains(createPoint) }) {
                showDirectoryPicker(for: select.key)
                return
            }
            if let recipeFrame = recipeFrames.first(where: { $0.frame.contains(createPoint) }) {
                blurCreateField()
                selectedRecipeID = recipeFrame.recipeID
                applyRecipeDefaults(overwrite: true)
                createMessage = ""
                updateLayout()
                return
            }
            if createButtonFrame.contains(createPoint) {
                submitCreateForm()
                return
            }
            if cancelCreateFrame.contains(createPoint) {
                returnToAppsFromCreate()
                return
            }
            if let choice = createChoiceFrames.first(where: { $0.frame.contains(createPoint) }) {
                createValues[choice.key] = choice.value
                if createInputController.isFocused, activeCreateFieldKey == choice.key {
                    focusCreateField(choice.key)
                }
                createMessage = ""
                updateLayout()
                return
            }
            if let suggestion = createSuggestionFrames.first(where: { $0.frame.contains(createPoint) }) {
                createValues[suggestion.key] = suggestion.value
                if createInputController.isFocused, activeCreateFieldKey == suggestion.key {
                    focusCreateField(suggestion.key)
                }
                createMessage = ""
                updateLayout()
                return
            }
            if let field = createFieldFrames.first(where: { $0.frame.contains(createPoint) })?.key {
                let wasFocused = createInputController.isFocused && activeCreateFieldKey == field
                let index = characterIndexForCreateField(key: field, xPosition: createPoint.x)
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
            confirmFilePickerSave()
            return
        }
        if filePickerCancelFrame.contains(createPoint) {
            dismissFilePicker()
            return
        }
        if filePickerFilenameFrame.contains(createPoint) {
            let wasFocused = createInputController.isFocused && activeCreateFieldKey == Self.filePickerFilenameKey
            let index = characterIndexForCreateField(key: Self.filePickerFilenameKey, xPosition: createPoint.x)
            focusCreateField(Self.filePickerFilenameKey, selectAll: clickCount >= 3)
            switch clickCount {
            case 3...:
                createInputController.selectAll()
            case 2:
                createInputController.selectWord(at: index)
            default:
                createInputController.setCursorPosition(index,
                                                        modifySelection: modifierFlags.contains(.shift) && wasFocused)
            }
            updateLayout()
            return
        }
        if let hit = filePickerEntryFrames.first(where: { $0.frame.contains(createPoint) }) {
            blurCreateField()
            if hit.entry.isDirectory {
                fetchFilePickerDirectory(path: hit.entry.path)
            } else {
                pendingFilePicker?.filename = hit.entry.name
                createValues[Self.filePickerFilenameKey] = hit.entry.name
                focusCreateField(Self.filePickerFilenameKey, cursorPosition: hit.entry.name.count)
                updateLayout()
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
        _ = charactersIgnoringModifiers
        _ = modifierFlags
        _ = isARepeat
        if pendingInstallBackend != nil {
            handleInstallPromptKeyDown(keyCode: keyCode)
            return
        }
        if pendingPasswordAction != nil {
            handlePasswordKeyDown(keyCode: keyCode, characters: characters)
            return
        }
        if mode == .create {
            handleCreateKeyDown(keyCode: keyCode, characters: characters)
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
        pendingInstallOperation = (backend.rootOnly ?? false) ? "runRoot" : "run"
        updateLayout()
    }

    private func dismissInstallPrompt() {
        pendingInstallBackend = nil
        pendingInstallOperation = "run"
        updateLayout()
    }

    private func confirmPendingInstall() {
        guard let backend = pendingInstallBackend else { return }
        let operation = (backend.rootOnly ?? false) ? "runRoot" : pendingInstallOperation
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

    private func showPasswordPrompt(for backend: BackendRecord, operation: String, message: String) {
        pendingPasswordAction = PendingPasswordAction(serviceID: backend.serviceID,
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
              let backend = backends.first(where: { $0.serviceID == pendingPasswordAction.serviceID }) else {
            dismissPasswordPrompt()
            return
        }
        let password = passwordInputController.isFocused ? passwordInputController.text : sudoPasswordInput
        performControlAction(for: backend, operation: pendingPasswordAction.operation, sudoPassword: password)
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
            return LogSelection(serviceID: backend.serviceID, logIndex: 0)
        }
        guard !selections.isEmpty else { return }
        let currentIndex = selectedLog.flatMap { selections.firstIndex(of: $0) } ?? 0
        let nextIndex = min(max(currentIndex + delta, 0), selections.count - 1)
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
        guard let selectedServiceID,
              let preferred = backends.first(where: { $0.serviceID == selectedServiceID }) else {
            clearLogSelection()
            return
        }
        if let selectedLog,
           selectedLog.serviceID == selectedServiceID,
           preferred.logFiles.indices.contains(selectedLog.logIndex) {
            return
        }
        if !preferred.logFiles.isEmpty {
            selectedLog = LogSelection(serviceID: preferred.serviceID, logIndex: 0)
        } else {
            selectedLog = nil
            logSnapshot = nil
            shouldScrollLogToBottomOnNextLayout = false
        }
    }

    private func clearLogSelection() {
        selectedServiceID = nil
        selectedLog = nil
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

    private func selectedRecipe() -> RecipeRecord? {
        recipes.first { $0.identifier == selectedRecipeID } ?? recipes.first
    }

    private func visibleCreateFields(for recipe: RecipeRecord) -> [RecipeFieldRecord] {
        recipe.fields.filter { !isCreateFieldHidden($0, in: recipe) }
    }

    private func isCreateFieldHidden(_ field: RecipeFieldRecord, in recipe: RecipeRecord) -> Bool {
        if field.key == "scriptPath" {
            return true
        }
        if field.key == "port",
           recipe.fields.contains(where: { $0.key == "frontendTransport" }),
           createValues["frontendTransport", default: "port"] == "unixSocket" {
            return true
        }
        return false
    }

    private func needsScriptPathPicker(for recipe: RecipeRecord) -> Bool {
        recipe.fields.contains { $0.key == "scriptPath" }
    }

    private func scriptFileExtension(for recipe: RecipeRecord) -> String {
        switch recipe.identifier {
        case "jupyter", "jupyter-uv":
            return ".py"
        default:
            return ".sh"
        }
    }

    private func defaultScriptFilename(for recipeIdentifier: String, extension fileExtension: String) -> String {
        let normalizedExtension = fileExtension.hasPrefix(".") ? String(fileExtension.dropFirst()) : fileExtension
        let fallbackIdentifier = recipeIdentifier.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? "script" : recipeIdentifier
        return normalizedExtension.isEmpty ? fallbackIdentifier : "\(fallbackIdentifier).\(normalizedExtension)"
    }

    private func splitScriptPath(_ path: String) -> (directory: String, filename: String) {
        let trimmed = path.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return ("~", "run-service.sh") }
        let ns = trimmed as NSString
        let filename = ns.lastPathComponent
        let directory = ns.deletingLastPathComponent
        if directory.isEmpty || directory == "." {
            return ("~", filename.isEmpty ? "run-service.sh" : filename)
        }
        return (directory, filename.isEmpty ? "run-service.sh" : filename)
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
        return backends.first { $0.serviceID == selectedServiceID }
    }

    private func outerShellBackend() -> BackendRecord? {
        backends.first { $0.isBackendsSelf }
    }

    private func currentLogFile(for backend: BackendRecord) -> LogFileRecord? {
        guard let selectedLog,
              selectedLog.serviceID == backend.serviceID,
              backend.logFiles.indices.contains(selectedLog.logIndex) else {
            return backend.logFiles.first
        }
        return backend.logFiles[selectedLog.logIndex]
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

    private func selectLog(serviceID: String, logIndex: Int) {
        guard let backend = backends.first(where: { $0.serviceID == serviceID }),
              backend.logFiles.indices.contains(logIndex) else { return }
        let nextSelection = LogSelection(serviceID: serviceID, logIndex: logIndex)
        guard selectedLog != nextSelection else { return }
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
        selectedServiceID = backend.serviceID
        ensureLogSelection()
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

    private func bundledPlaceholderBackends() -> [BackendRecord] {
        backends
            .filter { $0.isBundledPlaceholder }
            .sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    private func appLauncherItems() -> [AppLauncherItem] {
        appLauncherItems(from: backends)
    }

    private func runningEndpoints(for item: AppLauncherItem) -> [(endpoint: AppLauncherEndpoint, symbolName: String)] {
        var endpoints: [(endpoint: AppLauncherEndpoint, symbolName: String)] = []
        if let userEndpoint = item.userEndpoint,
           userEndpoint.frontend.isRunning {
            endpoints.append((userEndpoint, "person.fill"))
        }
        if let rootEndpoint = item.rootEndpoint,
           rootEndpoint.frontend.isRunning {
            endpoints.append((rootEndpoint, "checkmark.shield.fill"))
        }
        return endpoints
    }

    private func renderRunningBadges(for item: AppLauncherItem,
                                     leftX: CGFloat,
                                     centerY: CGFloat,
                                     pointSize: CGFloat,
                                     circleDiameter: CGFloat,
                                     gap: CGFloat) {
        let badges = runningEndpoints(for: item).compactMap { badge -> (endpoint: AppLauncherEndpoint, image: CGImage, size: CGSize)? in
            guard let symbol = naturalSymbolCGImage(named: badge.symbolName, pointSize: pointSize) else { return nil }
            return (badge.endpoint, symbol.image, symbol.size)
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
            appBadgeFrames.append(AppLauncherBadgeTarget(frame: chipFrame.insetBy(dx: -3, dy: -3),
                                                         endpoint: chip.badge.endpoint,
                                                         displayName: item.displayName))

            let chipLayer = CALayer()
            chipLayer.frame = chipFrame
            chipLayer.cornerRadius = floor(chipFrame.height / 2)
            chipLayer.backgroundColor = resolvedCGColor(NSColor.systemGreen.withAlphaComponent(0.95))
            chipLayer.borderWidth = 0.5
            chipLayer.borderColor = resolvedCGColor(NSColor.white.withAlphaComponent(0.8))
            chipLayer.shadowColor = resolvedCGColor(NSColor.systemGreen.withAlphaComponent(0.4))
            chipLayer.shadowOpacity = 0.22
            chipLayer.shadowRadius = 3
            chipLayer.shadowOffset = CGSize(width: 0, height: 1)

            let symbolLayer = CALayer()
            symbolLayer.frame = CGRect(x: floor((chipFrame.width - chip.badge.size.width) / 2),
                                       y: floor((chipFrame.height - chip.badge.size.height) / 2),
                                       width: chip.badge.size.width,
                                       height: chip.badge.size.height)
            symbolLayer.contentsGravity = .resizeAspect
            symbolLayer.contentsScale = 2
            symbolLayer.contents = chip.badge.image
            chipLayer.addSublayer(symbolLayer)

            appsScrollContentLayer.addSublayer(chipLayer)
            y -= gap
        }
    }

    private func appLauncherItems(from records: [BackendRecord]) -> [AppLauncherItem] {
        let endpoints = records.flatMap { backend -> [AppLauncherEndpoint] in
            guard !backend.isBackendsSelf else { return [] }
            return backend.frontends.enumerated().compactMap { index, frontend in
                return AppLauncherEndpoint(backend: backend, frontend: frontend, frontendIndex: index)
            }
        }

        let grouped = Dictionary(grouping: endpoints) { endpoint in
            appLauncherGroupingKey(backend: endpoint.backend, frontend: endpoint.frontend)
        }
        return grouped.map { key, endpoints in
            let sorted = endpoints.sorted { lhs, rhs in
                if lhs.backend.serviceScope == "system", rhs.backend.serviceScope != "system" { return false }
                if lhs.backend.serviceScope != "system", rhs.backend.serviceScope == "system" { return true }
                return frontendIdentityKey(backend: lhs.backend, frontend: lhs.frontend, frontendIndex: lhs.frontendIndex)
                    .localizedStandardCompare(frontendIdentityKey(backend: rhs.backend, frontend: rhs.frontend, frontendIndex: rhs.frontendIndex)) == .orderedAscending
            }
            let userEndpoint = sorted.first { $0.backend.serviceScope != "system" }
            let rootEndpoint = sorted.first { $0.backend.serviceScope == "system" }
            let primaryEndpoint = userEndpoint ?? rootEndpoint ?? sorted[0]
            return AppLauncherItem(identityKey: key,
                                   primaryEndpoint: primaryEndpoint,
                                   userEndpoint: userEndpoint,
                                   rootEndpoint: rootEndpoint)
        }
        .sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    private func appLauncherGroupingKey(backend: BackendRecord, frontend: FrontendRecord) -> String {
        let frontendName = frontend.name.trimmingCharacters(in: .whitespacesAndNewlines)
        let displayName = frontendName.isEmpty ? backend.displayName.trimmingCharacters(in: .whitespacesAndNewlines) : frontendName
        let endpointPath: String
        if backend.isBundled ?? false {
            endpointPath = pathAndQuery(fromFrontendURL: frontend.url, socketPath: frontend.socketPath)
        } else if !frontend.id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            endpointPath = frontend.id
        } else if !frontend.socketPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            endpointPath = pathAndQuery(fromFrontendURL: frontend.url, socketPath: frontend.socketPath)
        } else if frontend.port > 0 {
            endpointPath = pathAndQuery(fromFrontendURL: frontend.url, socketPath: nil)
        } else if let parsed = URL(string: frontend.url), parsed.scheme != nil {
            endpointPath = normalizedPathAndQuery((parsed.path.isEmpty ? "/" : parsed.path) + (parsed.query.map { "?\($0)" } ?? ""))
        } else {
            endpointPath = normalizedPathAndQuery(frontend.url)
        }
        return [
            "app",
            backend.serviceID,
            displayName,
            endpointPath
        ].joined(separator: "\u{1f}")
    }

    private func appLauncherSignature(for backends: [BackendRecord]) -> String {
        appLauncherItems(from: backends).map { item in
            let iconBytes = item.frontend.iconData?.count ?? 0
            return [
                item.identityKey,
                item.backend.serviceID,
                item.backend.serviceScope,
                item.backend.displayName,
                item.frontend.url,
                item.frontend.id,
                item.frontend.name,
                item.frontend.socketPath,
                String(item.frontend.port),
                item.frontend.isRunning ? "running" : "stopped",
                item.frontend.iconPath ?? "",
                String(iconBytes),
                item.frontend.listName,
                item.userEndpoint.map { frontendIdentityKey(backend: $0.backend, frontend: $0.frontend, frontendIndex: $0.frontendIndex) } ?? "",
                item.userEndpoint?.backend.status ?? "",
                item.rootEndpoint.map { frontendIdentityKey(backend: $0.backend, frontend: $0.frontend, frontendIndex: $0.frontendIndex) } ?? "",
                item.rootEndpoint?.backend.status ?? ""
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

    private func backendListRows() -> [BackendListRow] {
        backends.flatMap { backend -> [BackendListRow] in
            guard !backend.isBundledPlaceholder else { return [] }
            let parent = BackendListRow(backend: backend,
                                        frontend: nil,
                                        frontendIndex: nil,
                                        isFrontendChild: false)
            guard !backend.frontends.isEmpty else {
                return [parent]
            }
            let children = backend.frontends.enumerated().map { index, frontend in
                BackendListRow(backend: backend,
                               frontend: frontend,
                               frontendIndex: index,
                               isFrontendChild: true)
            }
            return [parent] + children
        }
    }

    private func clampScrollOffsets() {
        _ = clampAppsScrollUsingRenderedContent()
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
        return max(estimatedLogContentHeight(textWidth: textWidth), logRowsClipLayer.bounds.height)
    }

    private func estimatedLogContentHeight(textWidth: CGFloat) -> CGFloat {
        if let cache = logEstimatedContentHeightCache,
           cache.generation == logTextContentGeneration,
           abs(cache.textWidth - textWidth) <= 0.5 {
            return cache.height
        }

        let font = logTextFont()
        let charWidth = max(("0" as NSString).size(withAttributes: [.font: font]).width, 1)
        let lineHeight = ceil(font.ascender - font.descender + font.leading + 2)
        let charactersPerLine = max(Int(floor(textWidth / charWidth)), 1)
        var visualLineCount = 0
        var currentLineLength = 0
        for character in logRenderedText.utf8 {
            if character == 10 {
                visualLineCount += max(Int(ceil(Double(currentLineLength) / Double(charactersPerLine))), 1)
                currentLineLength = 0
            } else {
                currentLineLength += 1
            }
        }
        visualLineCount += max(Int(ceil(Double(currentLineLength) / Double(charactersPerLine))), 1)

        let height = CGFloat(max(visualLineCount, 1)) * lineHeight + logTextInsetY * 2
        logEstimatedContentHeightCache = (generation: logTextContentGeneration, textWidth: textWidth, height: height)
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
        let metrics = logVisualLineMetrics()
        guard !metrics.lines.isEmpty else { return 0 }

        let lineIndex = min(max(Int(floor(max(textPoint.y, 0) / metrics.lineHeight)), 0), metrics.lines.count - 1)
        let line = metrics.lines[lineIndex]
        let column = min(max(Int(floor(max(textPoint.x, 0) / metrics.charWidth)), 0), line.range.length)
        return min(line.range.location + column, logAttributedText.length)
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

    private func clampCreateScrollUsingRenderedContent() -> Bool {
        let maxScroll = max(createScroll + createBottomInset - createContentBottom, 0)
        let clamped = min(max(createScroll, 0), maxScroll)
        if abs(clamped - createScroll) > 0.5 {
            createScroll = clamped
            return true
        }
        return false
    }

    private func updateStatusText() {
        if isPerformingAction {
            statusLayer.string = mode == .create ? "Creating..." : backendError
        } else if isLoadingBackends && backends.isEmpty {
            statusLayer.string = "Loading..."
        } else if !backendError.isEmpty {
            statusLayer.string = backendError
        } else {
            statusLayer.string = ""
        }
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

    private func backendColumns(width: CGFloat) -> [(title: String, x: CGFloat, width: CGFloat)] {
        let contentWidth = max(width - horizontalInset * 2, 1)
        let name = floor(contentWidth * 0.24)
        let status: CGFloat = 78
        let action: CGFloat = 112
        let path = max(contentWidth - name - status - action, 1)
        let x = horizontalInset
        return [
            (title: "Name", x: x, width: name),
            (title: "Status", x: x + name, width: status),
            (title: "Path", x: x + name + status, width: path),
            (title: "Action", x: x + name + status + path, width: action)
        ]
    }

    private func statusColor(_ status: String) -> NSColor {
        switch status {
        case "running":
            return .systemGreen
        case "stopped":
            return .secondaryLabelColor
        case "available":
            return .secondaryLabelColor
        default:
            return .systemOrange
        }
    }

    private func rowName(for row: BackendListRow) -> String {
        if row.isFrontendChild {
            let name = row.frontend?.name.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            if name == row.backend.displayName.trimmingCharacters(in: .whitespacesAndNewlines) {
                return ""
            }
            return name.isEmpty ? "Frontend" : name
        }
        return row.backend.displayName
    }

    private func rowStatus(for row: BackendListRow) -> String {
        row.isFrontendChild ? "" : row.backend.status.capitalized
    }

    private func rowPathText(for row: BackendListRow) -> String {
        if row.isFrontendChild {
            return frontendPathText(row.frontend)
        }
        return row.backend.pathText
    }

    private func frontendPathText(_ frontend: FrontendRecord?) -> String {
        guard let frontend else { return "--" }
        if !frontend.socketPath.isEmpty {
            let path = pathAndQuery(fromFrontendURL: frontend.url, socketPath: frontend.socketPath)
            return path == "/" ? frontend.socketPath : "\(frontend.socketPath)\(path)"
        }
        if frontend.port > 0 {
            return frontend.url.isEmpty ? "127.0.0.1:\(frontend.port)/" : frontend.url
        }
        return frontend.url.isEmpty ? "--" : frontend.url
    }

    private func rowActions(for row: BackendListRow) -> [(title: String, operation: String)] {
        let backend = row.backend
        if row.isFrontendChild {
            return row.frontend?.hasEndpoint == true ? [("Open", "open")] : []
        }
        if backend.isBackendsSelf {
            var actions: [(title: String, operation: String)] = []
            if row.frontend?.hasEndpoint == true {
                actions.append(("Open", "open"))
            }
            actions.append(("Actions", "menu"))
            return actions
        }
        if backend.isMigrationAction {
            return [("Migrate", "migrateRoot")]
        }
        if backend.isBundledPlaceholder {
            let operation = (backend.rootOnly ?? false) ? "runRoot" : "run"
            let title = (backend.rootOnly ?? false) ? "Run as root" : "Run"
            return [(title, operation), ("Actions", "menu")]
        }
        var actions: [(title: String, operation: String)] = []
        if row.frontend?.hasEndpoint == true {
            actions.append(("Open", "open"))
        }
        if backend.canControl {
            actions.append((backend.status == "running" ? "Stop" : "Start",
                            backend.status == "running" ? "stop" : "start"))
        }
        if backend.canUninstallBackend || (backend.isBundled ?? false) {
            actions.append(("Actions", "menu"))
        }
        return actions
    }

    private func actionButtonWidth(for operation: String) -> CGFloat {
        switch operation {
        case "start", "stop", "menu":
            return 28
        default:
            return 62
        }
    }

    private func backendManagementMenuItems(for backend: BackendRecord,
                                            includePlaceholderRunActions: Bool) -> (operationByItemID: [String: String], items: [OuterframeContextMenuItem]) {
        var operationByItemID: [String: String] = [:]
        var items: [OuterframeContextMenuItem] = []
        if backend.isBackendsSelf {
            operationByItemID["checkUpdate"] = "checkUpdate"
            items.append(OuterframeContextMenuItem(id: "checkUpdate",
                                                   title: "Check for Updates",
                                                   isEnabled: true))
            operationByItemID["update"] = "update"
            items.append(OuterframeContextMenuItem(id: "update",
                                                   title: "Update Outer Shell",
                                                   isEnabled: true))
            operationByItemID["uninstall"] = "uninstallOuterShell"
            items.append(OuterframeContextMenuItem(id: "uninstall",
                                                   title: "Uninstall Outer Shell",
                                                   isEnabled: true))
            return (operationByItemID, items)
        }
        if backend.isBundled ?? false {
            if backend.isBundledPlaceholder && includePlaceholderRunActions {
                operationByItemID["run"] = (backend.rootOnly ?? false) ? "runRoot" : "run"
                items.append(OuterframeContextMenuItem(id: "run",
                                                       title: (backend.rootOnly ?? false) ? "Run as root" : "Run",
                                                       isEnabled: true))
                if (backend.supportsRoot ?? false) && !(backend.rootOnly ?? false) {
                    operationByItemID["runRoot"] = "runRoot"
                    items.append(OuterframeContextMenuItem(id: "runRoot",
                                                           title: "Run as root",
                                                           isEnabled: true))
                }
            } else if (backend.supportsRoot ?? false) {
                let hasRootSupport = backend.hasRootSupport ?? (backend.serviceScope == "system")
                operationByItemID["rootSupport"] = hasRootSupport ? "removeRootSupport" : "addRootSupport"
                items.append(OuterframeContextMenuItem(id: "rootSupport",
                                                       title: hasRootSupport ? "Remove Root Support" : "Add Root Support",
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
        pendingMenuActions[menuID] = (backend.serviceID, menu.operationByItemID)
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: menu.items,
                                       at: point)
    }

    private func showAppActionsMenu(for item: AppLauncherItem, at point: CGPoint) {
        let menuID = UUID()
        var operationByItemID: [String: String] = [:]
        var items: [OuterframeContextMenuItem] = []

        if item.backend.rootOnly ?? false {
            operationByItemID["runRoot"] = "runRoot"
            items.append(OuterframeContextMenuItem(id: "runRoot",
                                                   title: "Run as root",
                                                   isEnabled: true))
        } else {
            if item.userEndpoint != nil || item.rootEndpoint == nil {
                operationByItemID["run"] = "run"
                items.append(OuterframeContextMenuItem(id: "run",
                                                       title: "Run",
                                                       isEnabled: true))
            }
        }
        if ((item.backend.supportsRoot ?? false) || item.rootEndpoint != nil) && !(item.backend.rootOnly ?? false) {
            operationByItemID["runRoot"] = "runRoot"
            items.append(OuterframeContextMenuItem(id: "runRoot",
                                                   title: "Run as root",
                                                   isEnabled: true))
        }
        for logEndpoint in appLogEndpoints(for: item) {
            operationByItemID[logEndpoint.itemID] = "showLogs:\(logEndpoint.endpoint.backend.serviceID)"
            items.append(OuterframeContextMenuItem(id: logEndpoint.itemID,
                                                   title: logEndpoint.title,
                                                   isEnabled: true))
        }
        let management = backendManagementMenuItems(for: item.backend, includePlaceholderRunActions: false)
        for menuItem in management.items where operationByItemID[menuItem.id] == nil {
            guard let operation = management.operationByItemID[menuItem.id] else { continue }
            operationByItemID[menuItem.id] = operation
            items.append(menuItem)
        }
        guard !items.isEmpty else { return }

        pendingAppMenuActions[menuID] = (item, operationByItemID)
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: items,
                                       at: point)
    }

    private func appLogEndpoints(for item: AppLauncherItem) -> [(itemID: String, title: String, endpoint: AppLauncherEndpoint)] {
        let endpoints = [item.userEndpoint, item.rootEndpoint].compactMap { $0 }
        guard !endpoints.isEmpty else { return [] }

        let hasMultipleInstalls = endpoints.count > 1
        return endpoints.map { endpoint in
            let isRoot = endpoint.backend.serviceScope == "system"
            let prefix = hasMultipleInstalls && isRoot ? "root/" : ""
            let title = "Show logs for \(prefix)\(item.displayName)"
            let itemID = isRoot ? "showRootLogs" : "showLogs"
            return (itemID: itemID, title: title, endpoint: endpoint)
        }
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
        pendingLogMenuSelections[menuID] = (backend.serviceID, logIndexByItemID)
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: items,
                                       at: point)
    }

    private func handleContextMenuSelection(menuID: UUID, itemID: String) {
        if let menuSelection = pendingLogMenuSelections.removeValue(forKey: menuID),
           let logIndex = menuSelection.logIndexByItemID[itemID] {
            selectLog(serviceID: menuSelection.serviceID, logIndex: logIndex)
            return
        }

        if let appAction = pendingAppMenuActions.removeValue(forKey: menuID),
           let operation = appAction.operationByItemID[itemID] {
            performAppMenuAction(appAction.item, operation: operation)
            return
        }

        guard let menuAction = pendingMenuActions.removeValue(forKey: menuID),
              let operation = menuAction.operationByItemID[itemID],
              let backend = backends.first(where: { $0.serviceID == menuAction.serviceID }) else {
            return
        }
        performControlAction(for: backend, operation: operation)
    }

    private func frontendNavigationURL(_ frontend: FrontendRecord) -> URL? {
        let socketPath = frontend.socketPath.trimmingCharacters(in: .whitespacesAndNewlines)
        if !socketPath.isEmpty {
            let path = pathAndQuery(fromFrontendURL: frontend.url, socketPath: socketPath)
            return URL(string: "http+unix://\(percentEncodedSocketPath(socketPath))\(path)")
        }

        let rawURL = frontend.url.trimmingCharacters(in: .whitespacesAndNewlines)
        if let parsed = URL(string: rawURL), parsed.scheme != nil {
            return parsed
        }
        if frontend.port > 0 {
            let path = pathAndQuery(fromFrontendURL: rawURL, socketPath: nil)
            return URL(string: "http://127.0.0.1:\(frontend.port)\(path)")
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
            return "Checking \(backend.displayName) for updates..."
        case "update":
            return "Updating \(backend.displayName)..."
        case "migrateRoot":
            return "Migrating \(backend.displayName)..."
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

    @discardableResult
    private func addCell(to rowLayer: CALayer,
                         text: String,
                         column: (title: String, x: CGFloat, width: CGFloat),
                         y: CGFloat,
                         xOffset: CGFloat = 0,
                         color: NSColor = .labelColor,
                         weight: NSFont.Weight = .regular,
                         alignment: CATextLayerAlignmentMode = .left,
                         italic: Bool = false,
                         emptyPlaceholder: String = "--") -> CATextLayer {
        let layer = makeTextLayer(size: 12, weight: weight, color: color, alignment: alignment, italic: italic)
        layer.string = text.isEmpty ? emptyPlaceholder : text
        layer.frame = cellFrame(column: column, y: y, xOffset: xOffset)
        rowLayer.addSublayer(layer)
        return layer
    }

    private func cellFrame(column: (title: String, x: CGFloat, width: CGFloat),
                           y: CGFloat,
                           xOffset: CGFloat = 0) -> CGRect {
        CGRect(x: column.x + xOffset, y: y, width: max(column.width - 10 - xOffset, 1), height: 16)
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

    private func makeActionButtonLayer(title: String, operation: String) -> CALayer {
        switch operation {
        case "start":
            return makeSymbolButtonLayer(symbolName: "play.fill", accessibilityTitle: title)
        case "stop":
            return makeSymbolButtonLayer(symbolName: "stop.fill", accessibilityTitle: title)
        case "menu":
            return makeSymbolButtonLayer(symbolName: "ellipsis.circle", accessibilityTitle: title)
        default:
            return makeButtonLayer(title: title, emphasized: false)
        }
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

    private func symbolCGImage(named symbolName: String, pointSize: CGFloat) -> CGImage? {
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
            NSColor.controlAccentColor.setFill()
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
