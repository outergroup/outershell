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
        serviceID == "dev.outergroup.HomeScreen" || serviceID == "dev.outergroup.Navigator" || serviceID == "dev.outergroup.Backends"
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
    let name: String
    let url: String
    let port: Int
    let socketPath: String
    let iconPath: String?
    let iconData: String?
    let list: String?

    var hasEndpoint: Bool {
        !socketPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || port > 0 || !url.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var iconImage: NSImage? {
        guard let iconData,
              let data = Data(base64Encoded: base64IconPayload(iconData)),
              !data.isEmpty else {
            return nil
        }
        return NSImage(data: data)
    }

    var listName: String {
        list?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    private func base64IconPayload(_ value: String) -> String {
        guard value.hasPrefix("data:"),
              let commaIndex = value.firstIndex(of: ",") else {
            return value
        }
        return String(value[value.index(after: commaIndex)...])
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

private struct LogSelection: Equatable {
    let serviceID: String
    let logIndex: Int
}

private enum BackendsViewMode {
    case apps
    case backends
    case create
}

private struct AppLauncherItem {
    let backend: BackendRecord
    let frontend: FrontendRecord
    let frontendIndex: Int

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
        "\(backend.serviceID):frontend:\(frontendIndex)"
    }
}

private struct BackendListRow {
    let backend: BackendRecord
    let frontend: FrontendRecord?
    let frontendIndex: Int?
    let isFrontendChild: Bool

    var serviceID: String { backend.serviceID }

    var iconKey: String? {
        guard let frontendIndex else { return nil }
        return "\(backend.serviceID):frontend:\(frontendIndex)"
    }

    var rowID: String {
        if let frontendIndex {
            return "\(backend.serviceID):frontend:\(frontendIndex)"
        }
        return "\(backend.serviceID):backend"
    }
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

@MainActor
private final class BackendsHandler: NSObject, OuterframeHostDelegate, SingleLineTextInputControllerDelegate {
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
    private var pollTimer: Timer?
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
    private var mode: BackendsViewMode = .backends
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
            Task { @MainActor in self?.submitCreateForm() }
        }
        return controller
    }()
    private var createMessage = ""
    private var backendScroll: CGFloat = 0
    private var logScroll: CGFloat = 0
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

    private let rootLayer = CALayer()
    private let toolbarLayer = CALayer()
    private let titleLayer = CATextLayer()
    private let statusLayer = CATextLayer()
    private let appsToggleLayer = CenteredButtonLayer(title: "Apps")
    private let backendsToggleLayer = CenteredButtonLayer(title: "Backends")
    private let contentLayer = CALayer()
    private let appsLayer = CALayer()
    private let bottomBarLayer = CALayer()
    private let newButtonLayer = CenteredButtonLayer(title: "New")
    private let tableHeaderLayer = CALayer()
    private let rowsClipLayer = CALayer()
    private let logHeaderLayer = CALayer()
    private let logRowsClipLayer = CALayer()
    private let dividerLayer = CALayer()
    private let createLayer = CALayer()
    private let iconTransitionLayer = CALayer()
    private let installOverlayLayer = CALayer()
    private let passwordOverlayLayer = CALayer()

    private var newButtonFrame = CGRect.zero
    private var appsToggleFrame = CGRect.zero
    private var backendsToggleFrame = CGRect.zero
    private var appCardFrames: [(frame: CGRect, item: AppLauncherItem)] = []
    private var addAppFrame = CGRect.zero
    private var appsContentBottom: CGFloat = 0
    private var backendRowFrames: [(frame: CGRect, row: BackendListRow)] = []
    private var backendActionFrames: [(frame: CGRect, row: BackendListRow, operation: String)] = []
    private var iconMatchStates: [String: IconMatchState] = [:]
    private var iconMatchLayers: [String: CALayer] = [:]
    private var textMatchStates: [String: TextMatchState] = [:]
    private var textMatchLayers: [String: CATextLayer] = [:]
    private var isRecordingTransitionTargets = false
    private var pendingMenuActions: [UUID: (serviceID: String, operationByItemID: [String: String])] = [:]
    private var recipeFrames: [(frame: CGRect, recipeID: String)] = []
    private var bundledAppInstallFrames: [(frame: CGRect, backend: BackendRecord)] = []
    private var bundledAppMenuFrames: [(frame: CGRect, backend: BackendRecord)] = []
    private var createFieldFrames: [(frame: CGRect, key: String)] = []
    private var createFieldLayouts: [String: CreateFieldLayout] = [:]
    private var createChoiceFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createSuggestionFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createButtonFrame = CGRect.zero
    private var cancelCreateFrame = CGRect.zero
    private var passwordFieldFrame = CGRect.zero
    private var passwordTextFrame = CGRect.zero
    private var passwordSubmitFrame = CGRect.zero
    private var passwordCancelFrame = CGRect.zero
    private var passwordPanelFrame = CGRect.zero
    private var pendingInstallBackend: BackendRecord?
    private var installPanelFrame = CGRect.zero
    private var installConfirmFrame = CGRect.zero
    private var installCancelFrame = CGRect.zero

    private let toolbarHeight: CGFloat = 48
    private let bottomBarHeight: CGFloat = 54
    private let tableHeaderHeight: CGFloat = 30
    private let backendRowHeight: CGFloat = 44
    private let logHeaderHeight: CGFloat = 62
    private let logLineHeight: CGFloat = 18
    private let horizontalInset: CGFloat = 18
    private let createBottomInset: CGFloat = 18
    private let pageTransitionDuration: CFTimeInterval = 0.28

    init(outerframeHost: OuterframeHost, appConnection: OuterframeAppConnection) {
        self.outerframeHost = outerframeHost
        self.appConnection = appConnection
        super.init()
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
            startPolling()

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
            stopPolling()
            retainedSelf = nil

        default:
            break
        }
    }

    func outerframeHostDidDisconnect(_ host: OuterframeHost) {
        stopPolling()
        retainedSelf = nil
    }

    private func configureNetworking() {
        if let base = outerframeHost.pluginBaseURL() {
            backendsEndpoint = URL(string: "/api/backends", relativeTo: base)?.absoluteURL
            logsEndpoint = URL(string: "/api/logs", relativeTo: base)?.absoluteURL
            controlEndpoint = URL(string: "/api/control", relativeTo: base)?.absoluteURL
            createEndpoint = URL(string: "/api/create", relativeTo: base)?.absoluteURL
            recipesEndpoint = URL(string: "/api/recipes", relativeTo: base)?.absoluteURL
        }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 20
        configuration.timeoutIntervalForResource = 30
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
            return .backends
        case "new":
            return .create
        case "", "apps":
            return .apps
        default:
            break
        }

        switch components.queryItems?.first(where: { $0.name == "view" })?.value {
        case "backends":
            return .backends
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
        case .backends:
            components.path = "/backends"
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
        if (mode == .apps && nextMode == .backends) || (mode == .backends && nextMode == .apps) {
            animateIconTransition(to: nextMode)
            return
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

    private func animateIconTransition(to nextMode: BackendsViewMode) {
        let startStates = iconMatchStates
        let startTextStates = textMatchStates
        let startIconLayers = iconMatchLayers
        let startTextLayers = textMatchLayers
        let previousMode = mode
        let outgoingLayers = visibleModeLayers(for: previousMode)
        let wasRecordingTransitionTargets = isRecordingTransitionTargets
        isRecordingTransitionTargets = true
        defer {
            isRecordingTransitionTargets = wasRecordingTransitionTargets
        }
        applyMode(nextMode)
        isRecordingTransitionTargets = wasRecordingTransitionTargets
        let endStates = iconMatchStates
        let endTextStates = textMatchStates
        let incomingLayers = visibleModeLayers(for: nextMode)
        let sharedKeys = Set(startStates.keys).intersection(endStates.keys)
        let sharedTextKeys = Set(startTextStates.keys).intersection(endTextStates.keys)
        let incomingIconKeys = Set(endStates.keys).subtracting(startStates.keys)
        let outgoingIconKeys = Set(startStates.keys).subtracting(endStates.keys)
        let incomingTextKeys = Set(endTextStates.keys).subtracting(startTextStates.keys)
        let outgoingTextKeys = Set(startTextStates.keys).subtracting(endTextStates.keys)
        guard !sharedKeys.isEmpty || !sharedTextKeys.isEmpty || !outgoingLayers.isEmpty || !incomingLayers.isEmpty else { return }

        prepareCrossfade(from: outgoingLayers, to: incomingLayers)
        prepareMatchedLayerFades(incomingIconKeys: incomingIconKeys,
                                 outgoingIconKeys: outgoingIconKeys,
                                 incomingTextKeys: incomingTextKeys,
                                 outgoingTextKeys: outgoingTextKeys,
                                 startIconLayers: startIconLayers,
                                 startTextLayers: startTextLayers)

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        animateCrossfade(from: outgoingLayers, to: incomingLayers)
        animateMatchedLayerFades(incomingIconKeys: incomingIconKeys,
                                 outgoingIconKeys: outgoingIconKeys,
                                 incomingTextKeys: incomingTextKeys,
                                 outgoingTextKeys: outgoingTextKeys,
                                 startIconLayers: startIconLayers,
                                 startTextLayers: startTextLayers)
        for key in sharedKeys {
            guard let start = startStates[key],
                  let end = endStates[key],
                  let layer = iconMatchLayers[key] else {
                continue
            }

            let positionAnimation = CABasicAnimation(keyPath: "position")
            positionAnimation.fromValue = CGPoint(x: start.frame.midX, y: start.frame.midY)
            positionAnimation.toValue = CGPoint(x: end.frame.midX, y: end.frame.midY)

            let boundsAnimation = CABasicAnimation(keyPath: "bounds")
            boundsAnimation.fromValue = CGRect(origin: .zero, size: start.frame.size)
            boundsAnimation.toValue = CGRect(origin: .zero, size: end.frame.size)

            let radiusAnimation = CABasicAnimation(keyPath: "cornerRadius")
            radiusAnimation.fromValue = layer.cornerRadius
            radiusAnimation.toValue = iconCornerRadius(for: end.frame.width)

            let group = CAAnimationGroup()
            group.animations = [positionAnimation, boundsAnimation, radiusAnimation]
            group.duration = pageTransitionDuration
            group.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            layer.position = CGPoint(x: end.frame.midX, y: end.frame.midY)
            layer.bounds = CGRect(origin: .zero, size: end.frame.size)
            configureLauncherIconLayer(layer, image: end.image, symbolName: nil, title: end.title, iconSize: end.frame.width)
            layer.cornerRadius = iconCornerRadius(for: end.frame.width)
            layer.add(group, forKey: "matchedIconTransition")
        }
        for key in sharedTextKeys {
            guard let start = startTextStates[key],
                  let end = endTextStates[key],
                  let layer = textMatchLayers[key] else {
                continue
            }
            let startFlightFrame = flightFrame(for: start)
            let endFlightFrame = flightFrame(for: end)
            configureTextLayer(layer,
                               title: start.title,
                               fontSize: start.fontSize,
                               weight: start.weight,
                               color: .labelColor,
                               alignment: .left,
                               isWrapped: false)
            layer.frame = startFlightFrame

            let positionAnimation = CABasicAnimation(keyPath: "position")
            positionAnimation.fromValue = CGPoint(x: startFlightFrame.midX, y: startFlightFrame.midY)
            positionAnimation.toValue = CGPoint(x: endFlightFrame.midX, y: endFlightFrame.midY)

            let boundsAnimation = CABasicAnimation(keyPath: "bounds")
            boundsAnimation.fromValue = CGRect(origin: .zero, size: startFlightFrame.size)
            boundsAnimation.toValue = CGRect(origin: .zero, size: endFlightFrame.size)

            let fontAnimation = CABasicAnimation(keyPath: "fontSize")
            fontAnimation.fromValue = start.fontSize
            fontAnimation.toValue = end.fontSize

            let group = CAAnimationGroup()
            group.animations = [positionAnimation, boundsAnimation, fontAnimation]
            group.duration = pageTransitionDuration
            group.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            layer.position = CGPoint(x: endFlightFrame.midX, y: endFlightFrame.midY)
            layer.bounds = CGRect(origin: .zero, size: endFlightFrame.size)
            layer.fontSize = end.fontSize
            layer.add(group, forKey: "matchedTextTransition")
        }
        CATransaction.commit()

        DispatchQueue.main.asyncAfter(deadline: .now() + pageTransitionDuration + 0.01) { [weak self] in
            guard let self else { return }
            self.finishCrossfade(from: outgoingLayers, to: incomingLayers, currentMode: nextMode)
            self.finishMatchedTextTransition(endTextStates: endTextStates, sharedTextKeys: sharedTextKeys)
            self.finishMatchedLayerFades(incomingIconKeys: incomingIconKeys,
                                         outgoingIconKeys: outgoingIconKeys,
                                         incomingTextKeys: incomingTextKeys,
                                         outgoingTextKeys: outgoingTextKeys,
                                         startIconLayers: startIconLayers,
                                         startTextLayers: startTextLayers,
                                         currentMode: nextMode)
        }
    }

    private func prepareMatchedLayerFades(incomingIconKeys: Set<String>,
                                          outgoingIconKeys: Set<String>,
                                          incomingTextKeys: Set<String>,
                                          outgoingTextKeys: Set<String>,
                                          startIconLayers: [String: CALayer],
                                          startTextLayers: [String: CATextLayer]) {
        withoutImplicitAnimations {
            for key in incomingIconKeys {
                guard let layer = iconMatchLayers[key] else { continue }
                layer.isHidden = false
                layer.opacity = 0
            }
            for key in incomingTextKeys {
                guard let layer = textMatchLayers[key] else { continue }
                layer.isHidden = false
                layer.opacity = 0
            }
            for key in outgoingIconKeys {
                guard let layer = startIconLayers[key] else { continue }
                layer.isHidden = false
                layer.opacity = 1
            }
            for key in outgoingTextKeys {
                guard let layer = startTextLayers[key] else { continue }
                layer.isHidden = false
                layer.opacity = 1
            }
        }
    }

    private func animateMatchedLayerFades(incomingIconKeys: Set<String>,
                                          outgoingIconKeys: Set<String>,
                                          incomingTextKeys: Set<String>,
                                          outgoingTextKeys: Set<String>,
                                          startIconLayers: [String: CALayer],
                                          startTextLayers: [String: CATextLayer]) {
        for key in incomingIconKeys {
            guard let layer = iconMatchLayers[key] else { continue }
            addOpacityAnimation(to: layer, from: 0, to: 1, key: "matchedIconAppear")
            layer.opacity = 1
        }
        for key in incomingTextKeys {
            guard let layer = textMatchLayers[key] else { continue }
            addOpacityAnimation(to: layer, from: 0, to: 1, key: "matchedTextAppear")
            layer.opacity = 1
        }
        for key in outgoingIconKeys {
            guard let layer = startIconLayers[key] else { continue }
            addOpacityAnimation(to: layer, from: 1, to: 0, key: "matchedIconDisappear")
            layer.opacity = 0
        }
        for key in outgoingTextKeys {
            guard let layer = startTextLayers[key] else { continue }
            addOpacityAnimation(to: layer, from: 1, to: 0, key: "matchedTextDisappear")
            layer.opacity = 0
        }
    }

    private func finishMatchedLayerFades(incomingIconKeys: Set<String>,
                                         outgoingIconKeys: Set<String>,
                                         incomingTextKeys: Set<String>,
                                         outgoingTextKeys: Set<String>,
                                         startIconLayers: [String: CALayer],
                                         startTextLayers: [String: CATextLayer],
                                         currentMode: BackendsViewMode) {
        withoutImplicitAnimations {
            for key in incomingIconKeys {
                guard let layer = iconMatchLayers[key] else { continue }
                layer.removeAnimation(forKey: "matchedIconAppear")
                layer.opacity = 1
                layer.isHidden = mode != currentMode
            }
            for key in incomingTextKeys {
                guard let layer = textMatchLayers[key] else { continue }
                layer.removeAnimation(forKey: "matchedTextAppear")
                layer.opacity = 1
                layer.isHidden = mode != currentMode
            }
            for key in outgoingIconKeys {
                guard let layer = startIconLayers[key] else { continue }
                layer.removeAnimation(forKey: "matchedIconDisappear")
                layer.opacity = 1
                layer.isHidden = true
            }
            for key in outgoingTextKeys {
                guard let layer = startTextLayers[key] else { continue }
                layer.removeAnimation(forKey: "matchedTextDisappear")
                layer.opacity = 1
                layer.isHidden = true
            }
        }
    }

    private func flightFrame(for state: TextMatchState) -> CGRect {
        let width = max(measuredTextWidth(for: state), 1)
        return CGRect(x: glyphOriginX(for: state),
                      y: state.frame.minY,
                      width: width,
                      height: state.frame.height)
    }

    private func glyphOriginX(for state: TextMatchState) -> CGFloat {
        let textWidth = measuredTextWidth(for: state)
        switch state.alignment {
        case .center:
            return state.frame.minX + max((state.frame.width - textWidth) / 2, 0)
        case .right:
            return state.frame.minX + max(state.frame.width - textWidth, 0)
        default:
            return state.frame.minX
        }
    }

    private func measuredTextWidth(for state: TextMatchState) -> CGFloat {
        let font = NSFont.systemFont(ofSize: state.fontSize, weight: state.weight)
        let attributes: [NSAttributedString.Key: Any] = [.font: font]
        let width = (state.title as NSString).size(withAttributes: attributes).width
        return min(ceil(width), state.frame.width)
    }

    private func finishMatchedTextTransition(endTextStates: [String: TextMatchState], sharedTextKeys: Set<String>) {
        withoutImplicitAnimations {
            for key in sharedTextKeys {
                guard let state = endTextStates[key],
                      let layer = textMatchLayers[key] else {
                    continue
                }
                configureTextLayer(layer,
                                   title: state.title,
                                   fontSize: state.fontSize,
                                   weight: state.weight,
                                   color: .labelColor,
                                   alignment: state.alignment,
                                   isWrapped: state.isWrapped)
                layer.frame = state.frame
            }
        }
    }

    private func visibleModeLayers(for mode: BackendsViewMode) -> [CALayer] {
        let layers: [CALayer]
        switch mode {
        case .apps:
            layers = [appsLayer]
        case .backends:
            layers = [
                bottomBarLayer,
                tableHeaderLayer,
                rowsClipLayer,
                dividerLayer,
                logHeaderLayer,
                logRowsClipLayer
            ]
        case .create:
            layers = [createLayer]
        }
        return layers.filter { !$0.isHidden && $0.opacity > 0 }
    }

    private func prepareCrossfade(from outgoingLayers: [CALayer], to incomingLayers: [CALayer]) {
        withoutImplicitAnimations {
            for layer in outgoingLayers {
                layer.isHidden = false
                layer.opacity = 1
            }
            for layer in incomingLayers {
                layer.isHidden = false
                layer.opacity = 0
            }
        }
    }

    private func animateCrossfade(from outgoingLayers: [CALayer], to incomingLayers: [CALayer]) {
        for layer in outgoingLayers {
            addOpacityAnimation(to: layer, from: 1, to: 0, key: "outerframeDisappear")
            layer.opacity = 0
        }
        for layer in incomingLayers {
            addOpacityAnimation(to: layer, from: 0, to: 1, key: "outerframeAppear")
            layer.opacity = 1
        }
    }

    private func addOpacityAnimation(to layer: CALayer, from startOpacity: Float, to endOpacity: Float, key: String) {
        let animation = CABasicAnimation(keyPath: "opacity")
        animation.fromValue = startOpacity
        animation.toValue = endOpacity
        animation.duration = pageTransitionDuration
        animation.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
        layer.add(animation, forKey: key)
    }

    private func finishCrossfade(from outgoingLayers: [CALayer],
                                 to incomingLayers: [CALayer],
                                 currentMode: BackendsViewMode) {
        withoutImplicitAnimations {
            for layer in outgoingLayers {
                layer.removeAnimation(forKey: "outerframeDisappear")
                layer.opacity = 1
                if mode == currentMode {
                    layer.isHidden = true
                }
            }
            for layer in incomingLayers {
                layer.removeAnimation(forKey: "outerframeAppear")
                layer.opacity = 1
                if mode == currentMode {
                    layer.isHidden = false
                }
            }
            if mode != currentMode {
                for layer in visibleModeLayers(for: mode) {
                    layer.isHidden = false
                    layer.opacity = 1
                }
            }
        }
    }

    private func returnToBackendsFromCreate() {
        if outerframeHost.canGoBackInHistory() {
            outerframeHost.goBackInHistory()
        } else {
            navigateToMode(.backends, pushHistory: false)
        }
    }

    private func startPolling() {
        stopPolling()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.fetchBackends(quiet: true)
                if self.selectedLog != nil {
                    self.fetchSelectedLog(quiet: true)
                }
            }
        }
    }

    private func stopPolling() {
        pollTimer?.invalidate()
        pollTimer = nil
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
        toolbarLayer.addSublayer(appsToggleLayer)
        toolbarLayer.addSublayer(backendsToggleLayer)
        contentLayer.addSublayer(appsLayer)
        contentLayer.addSublayer(bottomBarLayer)
        bottomBarLayer.addSublayer(newButtonLayer)
        contentLayer.addSublayer(tableHeaderLayer)
        contentLayer.addSublayer(rowsClipLayer)
        contentLayer.addSublayer(dividerLayer)
        contentLayer.addSublayer(logHeaderLayer)
        contentLayer.addSublayer(logRowsClipLayer)
        contentLayer.addSublayer(createLayer)

        titleLayer.string = ""
        installOverlayLayer.isHidden = true
        passwordOverlayLayer.isHidden = true

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
                let toggleWidth: CGFloat = 184
                let toggleHeight: CGFloat = 30
                let toggleX = floor((width - toggleWidth) / 2)
                let showsModeToggle = mode != .create
                appsToggleFrame = showsModeToggle ? CGRect(x: toggleX, y: 9, width: 84, height: toggleHeight) : .zero
                backendsToggleFrame = showsModeToggle ? CGRect(x: toggleX + 86, y: 9, width: 98, height: toggleHeight) : .zero
                appsToggleLayer.frame = appsToggleFrame
                backendsToggleLayer.frame = backendsToggleFrame
                appsToggleLayer.isHidden = !showsModeToggle
                backendsToggleLayer.isHidden = !showsModeToggle
                appsToggleLayer.opacity = showsModeToggle ? 1 : 0
                backendsToggleLayer.opacity = showsModeToggle ? 1 : 0
                statusLayer.frame = CGRect(x: 152, y: 14, width: max(toggleX - 170, 1), height: 18)

                let contentHeight = contentLayer.bounds.height
                if mode == .apps {
                    outerframeHost.sendTextCursorUpdate(cursors: [])
                    appsLayer.isHidden = false
                    bottomBarLayer.isHidden = true
                    tableHeaderLayer.isHidden = true
                    rowsClipLayer.isHidden = true
                    dividerLayer.isHidden = true
                    logHeaderLayer.isHidden = true
                    logRowsClipLayer.isHidden = true
                    createLayer.isHidden = true
                    appsLayer.frame = CGRect(x: 0, y: 0, width: width, height: contentHeight)
                    renderAppsPage()
                } else if mode == .backends {
                    outerframeHost.sendTextCursorUpdate(cursors: [])
                    appsLayer.isHidden = true
                    bottomBarLayer.isHidden = false
                    tableHeaderLayer.isHidden = false
                    rowsClipLayer.isHidden = false
                    dividerLayer.isHidden = selectedServiceID == nil
                    logHeaderLayer.isHidden = selectedServiceID == nil
                    logRowsClipLayer.isHidden = selectedServiceID == nil
                    createLayer.isHidden = true
                    bottomBarLayer.frame = CGRect(x: 0, y: 0, width: width, height: min(bottomBarHeight, contentHeight))
                    newButtonFrame = CGRect(x: horizontalInset, y: 12, width: 76, height: 30)
                    newButtonLayer.frame = newButtonFrame
                    let tableBottom = bottomBarLayer.frame.maxY
                    let tableAreaHeight = max(contentHeight - tableBottom, 0)
                    let tableWidth = selectedServiceID == nil ? width : max(floor(width * 0.42), 320)
                    tableHeaderLayer.frame = CGRect(x: 0, y: tableBottom + max(tableAreaHeight - tableHeaderHeight, 0), width: tableWidth, height: tableHeaderHeight)
                    rowsClipLayer.frame = CGRect(x: 0, y: tableBottom, width: tableWidth, height: max(tableAreaHeight - tableHeaderHeight, 0))
                    if selectedServiceID != nil {
                        dividerLayer.frame = CGRect(x: tableWidth, y: tableBottom, width: 1, height: tableAreaHeight)
                        let logX = tableWidth + 1
                        let logWidth = max(width - logX, 1)
                        logHeaderLayer.frame = CGRect(x: logX, y: tableBottom + max(tableAreaHeight - logHeaderHeight, 0), width: logWidth, height: logHeaderHeight)
                        logRowsClipLayer.frame = CGRect(x: logX, y: tableBottom, width: logWidth, height: max(tableAreaHeight - logHeaderHeight, 0))
                        renderLogHeader()
                        renderLogRows()
                    }
                    renderBackendsHeader()
                    renderBackendsRows()
                } else {
                    appsLayer.isHidden = true
                    bottomBarLayer.isHidden = true
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
                bottomBarLayer.backgroundColor = resolvedCGColor(.controlBackgroundColor)
                tableHeaderLayer.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.9))
                logHeaderLayer.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.9))
                dividerLayer.backgroundColor = resolvedCGColor(.separatorColor)

                titleLayer.foregroundColor = resolvedCGColor(.labelColor)
                statusLayer.foregroundColor = resolvedCGColor(.secondaryLabelColor)
                if mode != .create {
                    appsToggleLayer.applyStyle(textCGColor: resolvedCGColor(mode == .apps ? .white : .controlAccentColor),
                                               backgroundCGColor: resolvedCGColor(mode == .apps ? .controlAccentColor : NSColor.controlAccentColor.withAlphaComponent(0.12)),
                                               font: NSFont.systemFont(ofSize: 12, weight: .medium))
                    backendsToggleLayer.applyStyle(textCGColor: resolvedCGColor(mode == .backends ? .white : .controlAccentColor),
                                                   backgroundCGColor: resolvedCGColor(mode == .backends ? .controlAccentColor : NSColor.controlAccentColor.withAlphaComponent(0.12)),
                                                   font: NSFont.systemFont(ofSize: 12, weight: .medium))
                }
                newButtonLayer.applyStyle(textCGColor: resolvedCGColor(.white),
                                          backgroundCGColor: resolvedCGColor(.controlAccentColor),
                                          font: NSFont.systemFont(ofSize: 12, weight: .medium))

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

    private func renderBackendsRows() {
        rowsClipLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        backendRowFrames.removeAll()
        backendActionFrames.removeAll()
        iconMatchStates.removeAll()
        textMatchStates.removeAll()
        var visibleIconKeys = Set<String>()
        var visibleTextKeys = Set<String>()

        let rows = backendListRows()
        if rows.isEmpty {
            hideUnrenderedMatchedLayers(visibleIconKeys: visibleIconKeys, visibleTextKeys: visibleTextKeys)
            let empty = makeTextLayer(size: 13, weight: .regular, color: .tertiaryLabelColor, alignment: .center)
            empty.string = isLoadingBackends ? "Loading backends..." : (backendError.isEmpty ? "No registered backends." : backendError)
            empty.frame = CGRect(x: horizontalInset, y: max(rowsClipLayer.bounds.midY - 10, 0), width: max(rowsClipLayer.bounds.width - horizontalInset * 2, 1), height: 20)
            rowsClipLayer.addSublayer(empty)
            return
        }

        let visibleStart = max(Int(floor(backendScroll / backendRowHeight)), 0)
        let visibleCount = Int(ceil(rowsClipLayer.bounds.height / backendRowHeight)) + 2
        let visibleEnd = min(rows.count, visibleStart + visibleCount)
        let columns = backendColumns(width: rowsClipLayer.bounds.width)
        let selectedBackground = resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.14))
        let alternating = alternatingRowColors()

        for index in visibleStart..<visibleEnd {
            let row = rows[index]
            let backend = row.backend
            let y = rowsClipLayer.bounds.height - CGFloat(index) * backendRowHeight + backendScroll - backendRowHeight
            let rowFrame = CGRect(x: 0, y: y, width: rowsClipLayer.bounds.width, height: backendRowHeight)
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
            rowsClipLayer.addSublayer(rowLayer)

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
    }

    private func renderAppsPage() {
        appsLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        appCardFrames.removeAll()
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
        let top = max(appsLayer.bounds.height - 28, 0) + appsScroll

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
        hideUnrenderedMatchedLayers(visibleIconKeys: visibleIconKeys, visibleTextKeys: visibleTextKeys)
        if clampAppsScrollUsingRenderedContent() {
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
        let itemHeight: CGFloat = 104
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

            recordMatchedIcon(key: item.iconKey,
                              frame: rootLayer.convert(CGRect(x: frame.minX + floor((itemWidth - iconSize) / 2),
                                                              y: frame.maxY - iconSize - 12,
                                                              width: iconSize,
                                                              height: iconSize),
                                                       from: appsLayer),
                              image: item.iconImage,
                              title: item.displayName)
            visibleIconKeys.insert(item.iconKey)

            recordMatchedText(key: item.iconKey,
                              frame: rootLayer.convert(CGRect(x: frame.minX, y: frame.minY + 10, width: itemWidth, height: 28),
                                                       from: appsLayer),
                              title: item.displayName,
                              fontSize: 12,
                              weight: .medium,
                              alignment: .center,
                              isWrapped: true)
            visibleTextKeys.insert(item.iconKey)

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

        return y
    }

    private func renderAddAppTile(frame: CGRect, iconSize: CGFloat) {
        let iconFrame = CGRect(x: frame.minX + floor((frame.width - iconSize) / 2),
                               y: frame.maxY - iconSize - 12,
                               width: iconSize,
                               height: iconSize)
        let icon = CALayer()
        icon.frame = iconFrame
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        icon.contents = symbolCGImage(named: "plus.square", pointSize: iconSize)
        appsLayer.addSublayer(icon)

        let title = makeTextLayer(size: 12, weight: .medium, color: .labelColor, alignment: .center, italic: true)
        title.string = "Add app"
        title.frame = CGRect(x: frame.minX, y: frame.minY + 10, width: frame.width, height: 28)
        appsLayer.addSublayer(title)
    }

    private func renderAddableAppsSection(apps: [BackendRecord],
                                          left: CGFloat,
                                          top: CGFloat,
                                          width: CGFloat) -> CGFloat {
        let title = makeTextLayer(size: 13, weight: .semibold, color: .secondaryLabelColor)
        title.string = "App Catalog"
        title.frame = CGRect(x: left, y: top, width: width, height: 18)
        createLayer.addSublayer(title)

        let itemHeight: CGFloat = 104
        let itemGap: CGFloat = 12
        let itemWidth: CGFloat = 112
        let columns = max(Int((width + itemGap) / (itemWidth + itemGap)), 1)
        let usedWidth = CGFloat(columns) * itemWidth + CGFloat(max(columns - 1, 0)) * itemGap
        let startX = left + max(floor((width - usedWidth) / 2), 0)
        var x = left
        var y = top - itemHeight - 32

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

        return y - 54
    }

    private func renderAddableAppTile(_ backend: BackendRecord, frame: CGRect) {
        let iconSize: CGFloat = 50
        let iconFrame = CGRect(x: frame.minX + floor((frame.width - iconSize) / 2),
                               y: frame.maxY - iconSize - 12,
                               width: iconSize,
                               height: iconSize)
        let icon = CALayer()
        icon.frame = iconFrame
        icon.cornerRadius = iconCornerRadius(for: iconSize)
        icon.masksToBounds = true
        icon.contentsGravity = .resizeAspect
        icon.contentsScale = 2
        configureLauncherIconLayer(icon,
                                   image: nil,
                                   symbolName: backend.iconSymbolName,
                                   title: backend.displayName,
                                   iconSize: iconSize)
        createLayer.addSublayer(icon)

        let name = makeTextLayer(size: 12, weight: .medium, color: .labelColor, alignment: .center)
        name.string = backend.displayName
        name.isWrapped = true
        name.truncationMode = .none
        name.frame = CGRect(x: frame.minX, y: frame.minY + 10, width: frame.width, height: 34)
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
            renderAppListWidget(name: group.name,
                                items: group.items,
                                frame: widgetFrame,
                                rowHeight: rowHeight,
                                visibleIconKeys: &visibleIconKeys,
                                visibleTextKeys: &visibleTextKeys)

            let label = makeTextLayer(size: 12, weight: .medium, color: .secondaryLabelColor, alignment: .center)
            label.string = group.name
            label.frame = CGRect(x: widgetFrame.minX, y: widgetFrame.minY - labelHeight, width: widgetFrame.width, height: 16)
            appsLayer.addSublayer(label)
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
        appsLayer.addSublayer(background)

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
                appsLayer.addSublayer(separator)
            }

            let iconFrame = CGRect(x: rowFrame.minX + rowInset,
                                   y: rowFrame.midY - iconSize / 2,
                                   width: iconSize,
                                   height: iconSize)
            recordMatchedIcon(key: item.iconKey,
                              frame: rootLayer.convert(iconFrame, from: appsLayer),
                              image: item.iconImage,
                              title: item.displayName)
            visibleIconKeys.insert(item.iconKey)

            let textFrame = CGRect(x: iconFrame.maxX + 12,
                                   y: rowFrame.midY - 10,
                                   width: max(rowFrame.maxX - iconFrame.maxX - 28, 1),
                                   height: 20)
            recordMatchedText(key: item.iconKey,
                              frame: rootLayer.convert(textFrame, from: appsLayer),
                              title: item.displayName,
                              fontSize: 13,
                              weight: .medium,
                              alignment: .left,
                              isWrapped: false)
            visibleTextKeys.insert(item.iconKey)
            y -= rowHeight
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
        if !isRecordingTransitionTargets || existingLayer == nil {
            configureLauncherIconLayer(layer, image: image, symbolName: nil, title: title, iconSize: frame.width)
            layer.frame = frame
        }
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
        let key = "\(row.backend.serviceID):frontend:\(match.offset)"
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
        if !isRecordingTransitionTargets || existingLayer == nil {
            configureTextLayer(layer,
                               title: title,
                               fontSize: fontSize,
                               weight: weight,
                               color: .labelColor,
                               alignment: alignment,
                               isWrapped: isWrapped,
                               italic: italic)
            layer.frame = frame
        }
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
            installCancelFrame = .zero
            return
        }

        installOverlayLayer.isHidden = false
        installOverlayLayer.frame = CGRect(x: 0, y: 0, width: width, height: height)
        installOverlayLayer.backgroundColor = resolvedCGColor(NSColor.black.withAlphaComponent(0.18))

        let panelWidth = min(max(width - 48, 280), 390)
        let panelHeight: CGFloat = 170
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
        message.string = "Home Screen will download, install, and start this app."
        message.isWrapped = true
        message.frame = CGRect(x: 72, y: panelHeight - 82, width: panelWidth - 90, height: 34)
        panel.addSublayer(message)

        let buttonY: CGFloat = 18
        let installButtonWidth: CGFloat = 74
        let cancelButtonWidth: CGFloat = 70
        let installLocalFrame = CGRect(x: panelWidth - installButtonWidth - 18,
                                       y: buttonY,
                                       width: installButtonWidth,
                                       height: 30)
        let cancelLocalFrame = CGRect(x: installLocalFrame.minX - cancelButtonWidth - 8,
                                      y: buttonY,
                                      width: cancelButtonWidth,
                                      height: 30)
        installConfirmFrame = installLocalFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)
        installCancelFrame = cancelLocalFrame.offsetBy(dx: panelFrame.minX, dy: panelFrame.minY)

        let cancelButton = makeButtonLayer(title: "Cancel", emphasized: false)
        cancelButton.frame = cancelLocalFrame
        panel.addSublayer(cancelButton)

        let installButton = makeButtonLayer(title: "Install", emphasized: true)
        installButton.frame = installLocalFrame
        panel.addSublayer(installButton)
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

    private func renderLogHeader() {
        logHeaderLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        let backend = selectedBackend()
        let title = makeTextLayer(size: 16, weight: .semibold, color: .labelColor)
        title.string = backend?.displayName ?? "Logs"
        title.frame = CGRect(x: horizontalInset, y: 34, width: max(logHeaderLayer.bounds.width - horizontalInset * 2, 1), height: 20)
        logHeaderLayer.addSublayer(title)

        let detail = makeTextLayer(size: 11, weight: .regular, color: logHeaderDetailColor())
        detail.string = logHeaderDetailText()
        detail.frame = CGRect(x: horizontalInset, y: 14, width: max(logHeaderLayer.bounds.width - horizontalInset * 2, 1), height: 16)
        logHeaderLayer.addSublayer(detail)
    }

    private func renderLogRows() {
        logRowsClipLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
        let text: String
        if isLoadingLog && logSnapshot == nil {
            text = "Loading logs..."
        } else if !logError.isEmpty {
            text = logError
        } else if let contents = logSnapshot?.contents, !contents.isEmpty {
            text = contents
        } else if selectedServiceID != nil, selectedLog == nil {
            text = "No registered log file."
        } else if selectedServiceID != nil {
            text = "No logs yet."
        } else {
            text = ""
        }

        let lines = logLines(from: text)
        let visibleStart = max(Int(floor(logScroll / logLineHeight)), 0)
        let visibleCount = Int(ceil(logRowsClipLayer.bounds.height / logLineHeight)) + 2
        let visibleEnd = min(lines.count, visibleStart + visibleCount)
        let lineColor: NSColor = logError.isEmpty ? .labelColor : .systemRed

        for index in visibleStart..<visibleEnd {
            let y = logRowsClipLayer.bounds.height - CGFloat(index) * logLineHeight + logScroll - logLineHeight
            let rowLayer = CALayer()
            rowLayer.frame = CGRect(x: 0, y: y, width: logRowsClipLayer.bounds.width, height: logLineHeight)
            if index.isMultiple(of: 2) {
                rowLayer.backgroundColor = resolvedCGColor(NSColor.controlBackgroundColor.withAlphaComponent(0.20))
            }

            let number = makeTextLayer(size: 11, weight: .regular, color: .tertiaryLabelColor, alignment: .right, monospaced: true)
            number.string = String(index + 1)
            number.frame = CGRect(x: 8, y: 2, width: 48, height: 14)
            rowLayer.addSublayer(number)

            let line = makeTextLayer(size: 12, weight: .regular, color: lineColor, monospaced: true)
            line.string = lines[index].isEmpty ? " " : lines[index]
            line.frame = CGRect(x: 66, y: 2, width: max(rowLayer.bounds.width - 78, 1), height: 15)
            rowLayer.addSublayer(line)
            logRowsClipLayer.addSublayer(rowLayer)
        }
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
        for field in recipe.fields {
            if field.fieldType == "choice" {
                addCreateChoiceField(field, frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: 50))
                y -= 62
            } else {
                addCreateField(field,
                               value: createValue(for: field),
                               frame: CGRect(x: detailLeft, y: y, width: detailWidth, height: field.suggestions.isEmpty ? 46 : 68),
                               monospaced: field.key == "command" || field.key == "executablePath" || field.key == "python" || field.key == "scriptPath")
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
    }

    private func addCreateField(_ field: RecipeFieldRecord,
                                value: String,
                                frame: CGRect,
                                monospaced: Bool = false) {
        let labelLayer = makeTextLayer(size: 11, weight: .medium, color: .secondaryLabelColor)
        labelLayer.string = field.label
        labelLayer.frame = CGRect(x: frame.minX, y: frame.maxY - 16, width: frame.width, height: 14)
        createLayer.addSublayer(labelLayer)

        let boxFrame = CGRect(x: frame.minX, y: frame.minY, width: frame.width, height: 30)
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
            renderBackendsRows()
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
                    let response = try JSONDecoder().decode(BackendsResponse.self, from: data)
                    self.lastBackendsResponseData = data
                    self.backendError = response.error
                    self.backends = response.backends
                    if let selectedServiceID = self.selectedServiceID,
                       !self.backends.contains(where: { $0.serviceID == selectedServiceID }) {
                        self.clearLogSelection()
                    }
                    if self.selectedServiceID != nil {
                        self.ensureLogSelection()
                    }
                    self.clampScrollOffsets()
                    self.updateLayout()
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
                    let response = try JSONDecoder().decode(RecipesResponse.self, from: data)
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

    private func fetchSelectedLog(quiet: Bool = false) {
        guard let selection = selectedLog, let logsEndpoint, let urlSession else { return }
        if isLoadingLog { return }
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
                    let snapshot = try JSONDecoder().decode(LogResponse.self, from: data)
                    self.logSnapshot = snapshot
                    self.logError = snapshot.error
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
                          let response = try? JSONDecoder().decode(ActionResponse.self, from: data) {
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

    private func scheduleBackendsRefreshes() {
        backendsRefreshGeneration += 1
        let generation = backendsRefreshGeneration
        for delay in [0.6, 1.8, 4.0, 8.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                Task { @MainActor in
                    guard let self, self.backendsRefreshGeneration == generation else { return }
                    self.fetchBackends()
                }
            }
        }
    }

    private func applyOptimisticStatus(for serviceID: String, operation: String) {
        let status: String
        switch operation {
        case "stop":
            status = "stopped"
        case "start", "restart", "run", "install", "runUser", "installUser", "runRoot", "installRoot":
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
                                 iconSymbolName: backend.iconSymbolName,
                                 launchdPlistPath: backend.launchdPlistPath,
                                 ownsLaunchdPlist: backend.ownsLaunchdPlist,
                                 frontends: backend.frontends,
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
        guard let url = frontendNavigationURL(item.frontend) else {
            backendError = "Could not build frontend URL."
            updateLayout()
            return
        }

        if opensInNewTab {
            outerframeHost.openNewTab(with: url, displayString: item.displayName)
        } else {
            outerframeHost.navigate(to: url)
        }
    }

    private func submitCreateForm() {
        guard !isPerformingAction, let createEndpoint, let urlSession else { return }
        guard let recipe = selectedRecipe() else {
            createMessage = "Choose a recipe."
            updateLayout()
            return
        }
        let missing = recipe.fields.first { field in
            let value = createValue(for: field).trimmingCharacters(in: .whitespacesAndNewlines)
            if field.key == "port" { return false }
            return value.isEmpty && field.defaultValue.isEmpty
        }
        if let missing {
            createMessage = "\(missing.label) is required."
            updateLayout()
            return
        }

        isPerformingAction = true
        createMessage = "Creating..."
        updateLayout()
        var components = URLComponents(url: createEndpoint, resolvingAgainstBaseURL: false)
        var queryItems = [URLQueryItem(name: "recipe", value: recipe.identifier)]
        for field in recipe.fields {
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
                    self.createMessage = error.localizedDescription
                } else if let data,
                          let response = try? JSONDecoder().decode(ActionResponse.self, from: data) {
                        self.createMessage = response.message
                        if response.ok {
                            self.createValues.removeAll()
                            self.applyRecipeDefaults(overwrite: true)
                            self.navigateToMode(.backends, pushHistory: false)
                            self.fetchBackends()
                        }
                } else {
                    self.createMessage = "Create request failed."
                }
                self.updateColors()
            }
        }.resume()
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
            returnToBackendsFromCreate()
        default:
            if let characters, !characters.isEmpty {
                insertCreateText(characters)
            }
        }
    }

    private func advanceCreateField() {
        let fields = selectedRecipe()?.fields.filter { $0.fieldType != "choice" } ?? []
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
        let key = activeCreateFieldKey ?? selectedRecipe()?.fields.first(where: { $0.fieldType != "choice" })?.key
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
        let value = createValues[key] ?? selectedRecipe()?.fields.first(where: { $0.key == key })?.defaultValue ?? ""
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
            returnToBackendsFromCreate()
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

        let capabilities = createInputController.currentEditingCapabilities()
        outerframeHost.setEditingCapabilities(canCopy: capabilities.canCopy, canCut: capabilities.canCut)
        outerframeHost.setAcceptedPasteboardPasteTypes(createInputController.currentAcceptedPasteboardTypeIdentifiers())
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
        guard createInputController.isFocused,
              let selectedText = createInputController.selectedTextContent(),
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
        guard let pendingCreateTextDrag else { return }
        let dx = point.x - pendingCreateTextDrag.startPoint.x
        let dy = point.y - pendingCreateTextDrag.startPoint.y
        guard hypot(dx, dy) >= 3 else { return }
        self.pendingCreateTextDrag = nil
        beginDraggingSelectedCreateText(pendingCreateTextDrag.selectedText)
    }

    private func handleMouseUp(at point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        _ = point
        _ = modifierFlags
        guard let pendingCreateTextDrag else { return }
        self.pendingCreateTextDrag = nil
        focusActiveCreateField()
        createInputController.setCursorPosition(pendingCreateTextDrag.cursorIndex, modifySelection: false)
    }

    private func handleMouseMoved(to point: CGPoint, modifierFlags: NSEvent.ModifierFlags) {
        _ = modifierFlags
        if pendingInstallBackend != nil {
            setCursorIfNeeded((installConfirmFrame.contains(point) || installCancelFrame.contains(point)) ? .pointingHand : .arrow)
            return
        }
        let isOverCreateField = pendingPasswordAction == nil && createFieldDropPoint(point) != nil
        let isOverPasswordField = pendingPasswordAction != nil && passwordFieldFrame.contains(point)
        var isOverBundledApp = false
        if pendingPasswordAction == nil, mode == .create {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            let createPoint = createLayer.convert(contentPoint, from: contentLayer)
            isOverBundledApp = bundledAppInstallFrames.contains { $0.frame.contains(createPoint) }
        }
        if isOverCreateField || isOverPasswordField {
            setCursorIfNeeded(.iBeam)
        } else if isOverBundledApp {
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
        if pendingPasswordAction != nil {
            guard passwordFieldFrame.contains(point) else { return }
            let index = characterIndexForPasswordField(xPosition: point.x)
            focusPasswordField(cursorPosition: index)
            updateEditingAndPasteboardState()
            outerframeHost.showContextMenu(for: NSAttributedString(string: ""), at: point)
            return
        }

        guard pendingPasswordAction == nil, mode == .create else { return }
        let contentPoint = contentLayer.convert(point, from: rootLayer)
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
        if mode == .apps {
            appsScroll -= delta.y * multiplier
        } else if mode == .backends {
            let contentPoint = contentLayer.convert(point, from: rootLayer)
            if selectedServiceID != nil && logRowsClipLayer.frame.contains(contentPoint) {
                logScroll -= delta.y * (precise ? 1 : logLineHeight)
            } else {
                backendScroll -= delta.y * multiplier
            }
        } else if mode == .create {
            createScroll -= delta.y * multiplier
        }
        clampScrollOffsets()
        updateLayout()
    }

    private func handleMouseDown(at point: CGPoint,
                                 modifierFlags: NSEvent.ModifierFlags = [],
                                 clickCount: Int = 1) {
        pendingCreateTextDrag = nil
        if pendingInstallBackend != nil {
            if installConfirmFrame.contains(point) {
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

        let toolbarPoint = toolbarLayer.convert(point, from: rootLayer)
        if mode != .create, appsToggleFrame.contains(toolbarPoint) {
            navigateToMode(.apps, pushHistory: true)
            return
        }
        if mode != .create, backendsToggleFrame.contains(toolbarPoint) {
            navigateToMode(.backends, pushHistory: true)
            return
        }

        let contentPoint = contentLayer.convert(point, from: rootLayer)
        if mode == .apps {
            let appsPoint = appsLayer.convert(contentPoint, from: contentLayer)
            if addAppFrame.contains(appsPoint) {
                navigateToMode(.create, pushHistory: true)
                return
            }
            if let card = appCardFrames.first(where: { $0.frame.contains(appsPoint) }) {
                openLauncherItem(card.item, opensInNewTab: modifierFlags.contains(.command))
                return
            }
        } else if mode == .backends {
            if bottomBarLayer.frame.contains(contentPoint) {
                let bottomPoint = bottomBarLayer.convert(contentPoint, from: contentLayer)
                if newButtonFrame.contains(bottomPoint) {
                    navigateToMode(.create, pushHistory: true)
                }
                return
            }
            let rowsPoint = rowsClipLayer.convert(contentPoint, from: contentLayer)
            if let action = backendActionFrames.first(where: { $0.frame.contains(rowsPoint) }) {
                if action.operation == "open" {
                    openFrontend(for: action.row, opensInNewTab: modifierFlags.contains(.command))
                } else if action.operation == "menu" {
                    showBackendActionsMenu(for: action.row.backend, at: point)
                } else {
                    performControlAction(for: action.row.backend, operation: action.operation)
                }
                return
            }
            if let row = backendRowFrames.first(where: { $0.frame.contains(rowsPoint) }) {
                if row.row.serviceID == selectedServiceID {
                    clearLogSelection()
                    updateLayout()
                } else {
                    selectedServiceID = row.row.serviceID
                    ensureLogSelection()
                    logSnapshot = nil
                    logScroll = 0
                    fetchSelectedLog()
                    updateLayout()
                }
                return
            }
            if selectedServiceID != nil,
               (rowsClipLayer.frame.contains(contentPoint) || tableHeaderLayer.frame.contains(contentPoint)) {
                clearLogSelection()
                updateLayout()
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
            if let recipeFrame = recipeFrames.first(where: { $0.frame.contains(createPoint) }) {
                blurCreateField()
                selectedRecipeID = recipeFrame.recipeID
                applyRecipeDefaults(overwrite: true)
                createMessage = ""
                createScroll = 0
                updateLayout()
                return
            }
            if createButtonFrame.contains(createPoint) {
                submitCreateForm()
                return
            }
            if cancelCreateFrame.contains(createPoint) {
                returnToBackendsFromCreate()
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
            if mode == .backends, selectedServiceID != nil {
                clearLogSelection()
                updateLayout()
            } else {
                navigateToMode(.apps, pushHistory: true)
            }
        case 125:
            moveSelection(delta: 1)
        case 126:
            moveSelection(delta: -1)
        default:
            break
        }
    }

    private func showInstallPrompt(for backend: BackendRecord) {
        blurCreateField()
        pendingInstallBackend = backend
        updateLayout()
    }

    private func dismissInstallPrompt() {
        pendingInstallBackend = nil
        updateLayout()
    }

    private func confirmPendingInstall() {
        guard let backend = pendingInstallBackend else { return }
        pendingInstallBackend = nil
        performControlAction(for: backend, operation: "run")
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
        guard mode == .backends else { return }
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
        fetchSelectedLog()
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
        }
    }

    private func clearLogSelection() {
        selectedServiceID = nil
        selectedLog = nil
        logSnapshot = nil
        logError = ""
        logScroll = 0
    }

    private func selectedRecipe() -> RecipeRecord? {
        recipes.first { $0.identifier == selectedRecipeID } ?? recipes.first
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
        if overwrite || activeCreateFieldKey == nil || !recipe.fields.contains(where: { $0.key == activeCreateFieldKey }) {
            activeCreateFieldKey = recipe.fields.first(where: { $0.fieldType != "choice" })?.key
        }
        if createInputController.isFocused, let activeCreateFieldKey {
            focusCreateField(activeCreateFieldKey)
        }
    }

    private func selectedBackend() -> BackendRecord? {
        guard let selectedServiceID else { return nil }
        return backends.first { $0.serviceID == selectedServiceID }
    }

    private func bundledPlaceholderBackends() -> [BackendRecord] {
        backends
            .filter { $0.isBundledPlaceholder }
            .sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
    }

    private func appLauncherItems() -> [AppLauncherItem] {
        backends.flatMap { backend -> [AppLauncherItem] in
            guard !backend.isBackendsSelf else { return [] }
            return backend.frontends.enumerated().compactMap { index, frontend in
                guard frontend.hasEndpoint else { return nil }
                return AppLauncherItem(backend: backend, frontend: frontend, frontendIndex: index)
            }
        }
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
        backendScroll = clampedScroll(backendScroll, contentRows: backendListRows().count, rowHeight: backendRowHeight, viewportHeight: rowsClipLayer.bounds.height)
        logScroll = clampedScroll(logScroll, contentRows: logLines(from: currentLogText()).count, rowHeight: logLineHeight, viewportHeight: logRowsClipLayer.bounds.height)
        _ = clampCreateScrollUsingRenderedContent()
    }

    private func clampedScroll(_ value: CGFloat, contentRows: Int, rowHeight: CGFloat, viewportHeight: CGFloat) -> CGFloat {
        let contentHeight = CGFloat(contentRows) * rowHeight
        let maxScroll = max(contentHeight - viewportHeight, 0)
        return min(max(value, 0), maxScroll)
    }

    private func clampAppsScrollUsingRenderedContent() -> Bool {
        let maxScroll = max(appsScroll + createBottomInset - appsContentBottom, 0)
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
        if !logError.isEmpty { return logError }
        if let contents = logSnapshot?.contents, !contents.isEmpty { return contents }
        return "No logs yet."
    }

    private func logLines(from text: String) -> [String] {
        let lines = text.components(separatedBy: .newlines)
        return lines.isEmpty ? [""] : lines
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
            return row.frontend?.hasEndpoint == true ? [("Open", "open")] : []
        }
        if backend.isBundledPlaceholder {
            return [("Run", "run"), ("Actions", "menu")]
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

    private func showBackendActionsMenu(for backend: BackendRecord, at point: CGPoint) {
        guard backend.canUninstallBackend || (backend.isBundled ?? false) else { return }
        let menuID = UUID()
        var operationByItemID: [String: String] = [:]
        var items: [OuterframeContextMenuItem] = []
        if backend.isBundled ?? false {
            if backend.serviceScope == "system" {
                operationByItemID["runUser"] = "runUser"
                items.append(OuterframeContextMenuItem(id: "runUser",
                                                       title: "Reinstall as User",
                                                       isEnabled: true))
            } else {
                operationByItemID["runRoot"] = "runRoot"
                items.append(OuterframeContextMenuItem(id: "runRoot",
                                                       title: backend.isBundledPlaceholder ? "Run as Root" : "Reinstall as Root",
                                                       isEnabled: true))
            }
        }
        if backend.canUninstallBackend {
            operationByItemID["uninstall"] = "uninstall"
            items.append(OuterframeContextMenuItem(id: "uninstall",
                                                   title: "Uninstall",
                                                   isEnabled: true))
        }
        guard !items.isEmpty else { return }
        pendingMenuActions[menuID] = (backend.serviceID, operationByItemID)
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: items,
                                       at: point)
    }

    private func handleContextMenuSelection(menuID: UUID, itemID: String) {
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
        case "runRoot", "installRoot":
            return "Installing \(backend.displayName) as root..."
        case "runUser", "installUser":
            return "Installing \(backend.displayName) as user..."
        case "uninstall":
            return "Uninstalling \(backend.displayName)..."
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
        let configuration = NSImage.SymbolConfiguration(pointSize: 13, weight: .medium)
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

        var proposedRect = CGRect(origin: .zero, size: image.size)
        guard let cgImage = image.cgImage(forProposedRect: &proposedRect, context: nil, hints: nil) else { return }

        context.saveGState()
        context.clip(to: drawRect, mask: cgImage)
        context.setFillColor(tintCGColor)
        context.fill(drawRect)
        context.restoreGState()
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
