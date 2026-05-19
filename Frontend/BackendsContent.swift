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
    let databasePath: String
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
        serviceID == "dev.outergroup.Backends"
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

    var hasEndpoint: Bool {
        !socketPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || port > 0 || !url.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
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
    case backends
    case create
}

private struct BackendListRow {
    let backend: BackendRecord
    let frontend: FrontendRecord?
    let frontendIndex: Int?
    let isFrontendChild: Bool

    var serviceID: String { backend.serviceID }

    var rowID: String {
        if let frontendIndex {
            return "\(backend.serviceID):frontend:\(frontendIndex)"
        }
        return "\(backend.serviceID):backend"
    }
}

@MainActor
private final class BackendsHandler: NSObject, OuterframeHostDelegate {
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

    private var databasePath = ""
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
    private var mode: BackendsViewMode = .backends
    private var recipes: [RecipeRecord] = []
    private var selectedRecipeID = "command-port"
    private var createValues: [String: String] = [:]
    private var activeCreateFieldKey: String?
    private var createMessage = ""
    private var backendScroll: CGFloat = 0
    private var logScroll: CGFloat = 0
    private var createScroll: CGFloat = 0
    private var createContentBottom: CGFloat = 0

    private let rootLayer = CALayer()
    private let toolbarLayer = CALayer()
    private let titleLayer = CATextLayer()
    private let statusLayer = CATextLayer()
    private let refreshLayer = CenteredButtonLayer(title: "Refresh")
    private let contentLayer = CALayer()
    private let bottomBarLayer = CALayer()
    private let newButtonLayer = CenteredButtonLayer(title: "New")
    private let tableHeaderLayer = CALayer()
    private let rowsClipLayer = CALayer()
    private let logHeaderLayer = CALayer()
    private let logRowsClipLayer = CALayer()
    private let dividerLayer = CALayer()
    private let createLayer = CALayer()

    private var newButtonFrame = CGRect.zero
    private var refreshFrame = CGRect.zero
    private var backendRowFrames: [(frame: CGRect, row: BackendListRow)] = []
    private var backendActionFrames: [(frame: CGRect, row: BackendListRow, operation: String)] = []
    private var pendingMenuActions: [UUID: (serviceID: String, operationByItemID: [String: String])] = [:]
    private var recipeFrames: [(frame: CGRect, recipeID: String)] = []
    private var createFieldFrames: [(frame: CGRect, key: String)] = []
    private var createChoiceFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createSuggestionFrames: [(frame: CGRect, key: String, value: String)] = []
    private var createButtonFrame = CGRect.zero
    private var cancelCreateFrame = CGRect.zero

    private let toolbarHeight: CGFloat = 48
    private let bottomBarHeight: CGFloat = 54
    private let tableHeaderHeight: CGFloat = 30
    private let backendRowHeight: CGFloat = 44
    private let logHeaderHeight: CGFloat = 62
    private let logLineHeight: CGFloat = 18
    private let horizontalInset: CGFloat = 18
    private let createBottomInset: CGFloat = 18

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
            outerframeHost.setInputMode(.rawKeys)
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

        case .mouseDown(let point, let modifierFlags, _):
            handleMouseDown(at: point, modifierFlags: modifierFlags)

        case .contextMenuItemSelected(let menuID, let itemID):
            handleContextMenuSelection(menuID: menuID, itemID: itemID)

        case .keyDown(let keyCode, let characters, _, _, _):
            handleKeyDown(keyCode: keyCode, characters: characters)

        case .textInput(let text, _, _, _):
            if mode == .create {
                insertCreateText(text)
            }

        case .historyTraversal(_, let url):
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
        configuration.timeoutIntervalForRequest = 5
        configuration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        outerframeHost.applyProxy(to: configuration)
        urlSession = URLSession(configuration: configuration)
    }

    private func modeFromURL(_ urlString: String?) -> BackendsViewMode {
        guard let urlString,
              let components = URLComponents(string: urlString),
              components.queryItems?.first(where: { $0.name == "view" })?.value == "new" else {
            return .backends
        }
        return .create
    }

    private func urlForMode(_ mode: BackendsViewMode) -> URL? {
        guard let url = outerframeHost.pluginURL(),
              var components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        var queryItems = components.queryItems ?? []
        queryItems.removeAll { $0.name == "view" }
        if mode == .create {
            queryItems.append(URLQueryItem(name: "view", value: "new"))
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
        mode = nextMode
        if mode == .create {
            clampScrollOffsets()
        }
        updateColors()
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
                guard let self, self.selectedLog != nil else { return }
                self.fetchSelectedLog(quiet: true)
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
        toolbarLayer.addSublayer(titleLayer)
        toolbarLayer.addSublayer(statusLayer)
        toolbarLayer.addSublayer(refreshLayer)
        contentLayer.addSublayer(bottomBarLayer)
        bottomBarLayer.addSublayer(newButtonLayer)
        contentLayer.addSublayer(tableHeaderLayer)
        contentLayer.addSublayer(rowsClipLayer)
        contentLayer.addSublayer(dividerLayer)
        contentLayer.addSublayer(logHeaderLayer)
        contentLayer.addSublayer(logRowsClipLayer)
        contentLayer.addSublayer(createLayer)

        titleLayer.string = "Backends"

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

                titleLayer.frame = CGRect(x: horizontalInset, y: 14, width: 120, height: 22)
                refreshFrame = CGRect(x: max(width - 96, 8), y: 10, width: 78, height: 28)
                refreshLayer.frame = refreshFrame
                statusLayer.frame = CGRect(x: 152, y: 14, width: max(width - 264, 1), height: 18)

                let contentHeight = contentLayer.bounds.height
                if mode == .backends {
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
                    bottomBarLayer.isHidden = true
                    tableHeaderLayer.isHidden = true
                    rowsClipLayer.isHidden = true
                    dividerLayer.isHidden = true
                    logHeaderLayer.isHidden = true
                    logRowsClipLayer.isHidden = true
                    createLayer.isHidden = false
                    createLayer.frame = CGRect(x: 0, y: 0, width: width, height: contentHeight)
                    renderCreateForm()
                }
                updateStatusText()
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
                refreshLayer.applyStyle(textCGColor: resolvedCGColor(.controlAccentColor),
                                        backgroundCGColor: resolvedCGColor(NSColor.controlAccentColor.withAlphaComponent(0.12)),
                                        font: NSFont.systemFont(ofSize: 12, weight: .medium))
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

        let rows = backendListRows()
        if rows.isEmpty {
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

            let italic = backend.isBundledPlaceholder
            let indent: CGFloat = row.isFrontendChild ? 18 : 0
            addCell(to: rowLayer, text: rowName(for: row), column: columns[0], y: 14, xOffset: indent, weight: row.isFrontendChild ? .regular : .medium, italic: italic, emptyPlaceholder: "")
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
            rowsClipLayer.addSublayer(rowLayer)
        }
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
        createFieldFrames.removeAll()
        createChoiceFrames.removeAll()
        createSuggestionFrames.removeAll()

        let formWidth = min(max(createLayer.bounds.width - horizontalInset * 2, 1), 920)
        let left = horizontalInset
        let top = max(createLayer.bounds.height - 54, 0) + createScroll

        let title = makeTextLayer(size: 18, weight: .semibold, color: .labelColor)
        title.string = "New Backend"
        title.frame = CGRect(x: left, y: top, width: formWidth, height: 24)
        createLayer.addSublayer(title)

        if recipes.isEmpty {
            let empty = makeTextLayer(size: 13, weight: .regular, color: .secondaryLabelColor)
            empty.string = isLoadingRecipes ? "Loading recipes..." : "No recipes loaded."
            empty.frame = CGRect(x: left, y: top - 40, width: formWidth, height: 20)
            createLayer.addSublayer(empty)
            createContentBottom = top - 40
            if clampCreateScrollUsingRenderedContent() {
                renderCreateForm()
            }
            return
        }

        let cardWidth = min((formWidth - 16) / 2, 310)
        let cardHeight: CGFloat = 54
        var cardX = left
        var cardY = top - 74
        for recipe in recipes {
            if cardX + cardWidth > left + formWidth {
                cardX = left
                cardY -= cardHeight + 10
            }
            let frame = CGRect(x: cardX, y: cardY, width: cardWidth, height: cardHeight)
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
            name.frame = CGRect(x: 10, y: 30, width: cardWidth - 20, height: 16)
            card.addSublayer(name)
            let summary = makeTextLayer(size: 10, weight: .regular, color: .secondaryLabelColor)
            summary.string = recipe.summary
            summary.frame = CGRect(x: 10, y: 10, width: cardWidth - 20, height: 14)
            card.addSublayer(summary)
            cardX += cardWidth + 16
        }

        guard let recipe = selectedRecipe() else { return }
        let summary = makeTextLayer(size: 12, weight: .regular, color: .secondaryLabelColor)
        summary.string = recipe.summary
        summary.frame = CGRect(x: left, y: cardY - 34, width: formWidth, height: 18)
        createLayer.addSublayer(summary)

        var y = cardY - 92
        for field in recipe.fields {
            if field.fieldType == "choice" {
                addCreateChoiceField(field, frame: CGRect(x: left, y: y, width: formWidth, height: 50))
                y -= 62
            } else {
                addCreateField(field,
                               value: createValue(for: field),
                               frame: CGRect(x: left, y: y, width: formWidth, height: field.suggestions.isEmpty ? 46 : 68),
                               monospaced: field.key == "command" || field.key == "executablePath" || field.key == "python")
                y -= field.suggestions.isEmpty ? 62 : 84
            }
        }

        createButtonFrame = CGRect(x: left, y: y + 14, width: 96, height: 30)
        cancelCreateFrame = CGRect(x: left + 106, y: y + 14, width: 78, height: 30)
        let createButton = makeButtonLayer(title: isPerformingAction ? "Creating..." : "Create", emphasized: true)
        createButton.frame = createButtonFrame
        createLayer.addSublayer(createButton)
        let cancelButton = makeButtonLayer(title: "Cancel", emphasized: false)
        cancelButton.frame = cancelCreateFrame
        createLayer.addSublayer(cancelButton)

        if !createMessage.isEmpty {
            let message = makeTextLayer(size: 12, weight: .regular, color: createMessage.hasPrefix("Created") ? .secondaryLabelColor : .systemRed)
            message.string = createMessage
            message.frame = CGRect(x: left, y: y - 26, width: formWidth, height: 18)
            createLayer.addSublayer(message)
        }
        createContentBottom = !createMessage.isEmpty ? y - 26 : y + 14
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
        let box = CALayer()
        box.frame = boxFrame
        box.cornerRadius = 5
        box.borderWidth = activeCreateFieldKey == field.key ? 1.5 : 1
        box.borderColor = activeCreateFieldKey == field.key ? resolvedCGColor(.controlAccentColor) : resolvedCGColor(.separatorColor)
        box.backgroundColor = resolvedCGColor(.textBackgroundColor)
        createLayer.addSublayer(box)

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

    private func fetchBackends() {
        guard !isLoadingBackends, let backendsEndpoint, let urlSession else { return }
        isLoadingBackends = true
        backendError = ""
        updateStatusText()
        renderBackendsRows()
        urlSession.dataTask(with: backendsEndpoint) { [weak self] data, response, error in
            Task { @MainActor in
                guard let self else { return }
                self.isLoadingBackends = false
                if let error {
                    self.backendError = error.localizedDescription
                    self.updateLayout()
                    return
                }
                if let http = response as? HTTPURLResponse, http.statusCode >= 400 {
                    self.backendError = "Backends API returned HTTP \(http.statusCode)."
                    self.updateLayout()
                    return
                }
                guard let data else {
                    self.backendError = "Backends API returned no data."
                    self.updateLayout()
                    return
                }
                do {
                    let response = try JSONDecoder().decode(BackendsResponse.self, from: data)
                    self.databasePath = response.databasePath
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

    private func performControlAction(for backend: BackendRecord, operation: String) {
        guard !isPerformingAction, let controlEndpoint, let urlSession else { return }
        isPerformingAction = true
        backendError = actionProgressText(operation: operation, backend: backend)
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
        urlSession.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor in
                guard let self else { return }
                self.isPerformingAction = false
                if let error {
                    self.backendError = error.localizedDescription
                } else if let data,
                          let response = try? JSONDecoder().decode(ActionResponse.self, from: data) {
                    self.backendError = response.ok ? "" : response.message
                } else {
                    self.backendError = "Control request failed."
                }
                self.fetchBackends()
            }
        }.resume()
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

    private func handleCreateKeyDown(keyCode: UInt16, characters: String?) {
        switch keyCode {
        case 48:
            advanceCreateField()
        case 51, 117:
            deleteBackwardInCreateField()
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
        activeCreateFieldKey = fields[(index + 1) % fields.count].key
        updateLayout()
    }

    private func insertCreateText(_ text: String) {
        guard !text.isEmpty else { return }
        let cleaned = text.filter { character in
            !character.isNewline && character.unicodeScalars.allSatisfy { $0.value >= 0x20 && $0.value != 0x7f }
        }
        guard !cleaned.isEmpty else { return }
        let key = activeCreateFieldKey ?? selectedRecipe()?.fields.first(where: { $0.fieldType != "choice" })?.key
        guard let key else { return }
        createValues[key, default: ""] += cleaned
        if key == "name" {
            let identifier = createValues["identifier", default: ""]
            if identifier.isEmpty {
                createValues["identifier"] = suggestedIdentifier(from: createValues["name", default: ""])
            }
        }
        createMessage = ""
        updateLayout()
    }

    private func deleteBackwardInCreateField() {
        guard let key = activeCreateFieldKey else { return }
        if key == "name" {
            let oldSuggestion = suggestedIdentifier(from: createValues["name", default: ""])
            if !(createValues[key] ?? "").isEmpty { createValues[key]?.removeLast() }
            let identifier = createValues["identifier", default: ""]
            if identifier == oldSuggestion || identifier.isEmpty {
                createValues["identifier"] = suggestedIdentifier(from: createValues["name", default: ""])
            }
        } else if !(createValues[key] ?? "").isEmpty {
            createValues[key]?.removeLast()
        }
        updateLayout()
    }

    private func handleScroll(at point: CGPoint, delta: CGPoint, precise: Bool) {
        let multiplier: CGFloat = precise ? 1 : backendRowHeight
        if mode == .backends {
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

    private func handleMouseDown(at point: CGPoint, modifierFlags: NSEvent.ModifierFlags = []) {
        let toolbarPoint = toolbarLayer.convert(point, from: rootLayer)
        if refreshFrame.contains(toolbarPoint) {
            fetchBackends()
            if selectedLog != nil {
                fetchSelectedLog()
            }
            return
        }

        let contentPoint = contentLayer.convert(point, from: rootLayer)
        if mode == .backends {
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
            if let recipeFrame = recipeFrames.first(where: { $0.frame.contains(createPoint) }) {
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
                createMessage = ""
                updateLayout()
                return
            }
            if let suggestion = createSuggestionFrames.first(where: { $0.frame.contains(createPoint) }) {
                createValues[suggestion.key] = suggestion.value
                createMessage = ""
                updateLayout()
                return
            }
            if let field = createFieldFrames.first(where: { $0.frame.contains(createPoint) })?.key {
                activeCreateFieldKey = field
                updateLayout()
            }
        }
    }

    private func handleKeyDown(keyCode: UInt16, characters: String?) {
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
                mode = .backends
                updateColors()
            }
        case 125:
            moveSelection(delta: 1)
        case 126:
            moveSelection(delta: -1)
        default:
            break
        }
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
    }

    private func selectedBackend() -> BackendRecord? {
        guard let selectedServiceID else { return nil }
        return backends.first { $0.serviceID == selectedServiceID }
    }

    private func backendListRows() -> [BackendListRow] {
        backends.flatMap { backend -> [BackendListRow] in
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
        backendScroll = clampedScroll(backendScroll, contentRows: backendListRows().count, rowHeight: backendRowHeight, viewportHeight: rowsClipLayer.bounds.height)
        logScroll = clampedScroll(logScroll, contentRows: logLines(from: currentLogText()).count, rowHeight: logLineHeight, viewportHeight: logRowsClipLayer.bounds.height)
        _ = clampCreateScrollUsingRenderedContent()
    }

    private func clampedScroll(_ value: CGFloat, contentRows: Int, rowHeight: CGFloat, viewportHeight: CGFloat) -> CGFloat {
        let contentHeight = CGFloat(contentRows) * rowHeight
        let maxScroll = max(contentHeight - viewportHeight, 0)
        return min(max(value, 0), maxScroll)
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
        } else if isLoadingBackends {
            statusLayer.string = "Loading..."
        } else if !backendError.isEmpty {
            statusLayer.string = backendError
        } else if databasePath.isEmpty {
            statusLayer.string = ""
        } else {
            statusLayer.string = databasePath
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
            return [("Run", "run")]
        }
        var actions: [(title: String, operation: String)] = []
        if row.frontend?.hasEndpoint == true {
            actions.append(("Open", "open"))
        }
        if backend.canControl {
            actions.append((backend.status == "running" ? "Stop" : "Start",
                            backend.status == "running" ? "stop" : "start"))
        }
        if backend.canUninstallBackend {
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
        guard backend.canUninstallBackend else { return }
        let menuID = UUID()
        pendingMenuActions[menuID] = (backend.serviceID, ["uninstall": "uninstall"])
        outerframeHost.showContextMenu(menuID: menuID,
                                       items: [
                                           OuterframeContextMenuItem(id: "uninstall",
                                                                     title: "Uninstall",
                                                                     isEnabled: true)
                                       ],
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
        case "uninstall":
            return "Uninstalling \(backend.displayName)..."
        default:
            return "\(operation.capitalized)ing \(backend.displayName)..."
        }
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

    private func addCell(to rowLayer: CALayer,
                         text: String,
                         column: (title: String, x: CGFloat, width: CGFloat),
                         y: CGFloat,
                         xOffset: CGFloat = 0,
                         color: NSColor = .labelColor,
                         weight: NSFont.Weight = .regular,
                         alignment: CATextLayerAlignmentMode = .left,
                         italic: Bool = false,
                         emptyPlaceholder: String = "--") {
        let layer = makeTextLayer(size: 12, weight: weight, color: color, alignment: alignment, italic: italic)
        layer.string = text.isEmpty ? emptyPlaceholder : text
        layer.frame = CGRect(x: column.x + xOffset, y: y, width: max(column.width - 10 - xOffset, 1), height: 16)
        rowLayer.addSublayer(layer)
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
