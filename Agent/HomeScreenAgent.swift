import AppKit
import Darwin
import Foundation
import SQLite3

@_silgen_name("HomeScreenBackendMain")
private func HomeScreenBackendMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32

@_silgen_name("HomeScreenBackendRequestShutdown")
private func HomeScreenBackendRequestShutdown()

private struct HostedFrontend: Equatable {
    let serviceID: String
    let name: String
    let port: Int
    let socketPath: String
    let url: String
}

private struct ManagedBackend: Equatable {
    enum State: Equatable {
        case running(pid: Int32?)
        case stopped
        case registered
        case missing
        case unknown

        var title: String {
            switch self {
            case .running:
                return "Running"
            case .stopped:
                return "Stopped"
            case .registered:
                return "Registered"
            case .missing:
                return "Missing"
            case .unknown:
                return "Unknown"
            }
        }

        var isRunning: Bool {
            if case .running = self { return true }
            return false
        }
    }

    let serviceID: String
    let displayName: String
    let plistPath: String
    let logFiles: [String]
    let frontends: [HostedFrontend]
    let state: State

    var launchdDomain: String {
        plistPath.hasPrefix("/Library/LaunchDaemons/") ? "system" : "gui/\(getuid())"
    }

    var isSystemService: Bool {
        plistPath.hasPrefix("/Library/LaunchDaemons/")
    }

    var canStart: Bool {
        !plistPath.isEmpty && !isSystemService
    }

    var canStop: Bool {
        !plistPath.isEmpty && !isSystemService
    }
}

private final class BackendMenuItemPayload: NSObject {
    let service: ManagedBackend

    init(service: ManagedBackend) {
        self.service = service
    }
}

private final class FrontendMenuItemPayload: NSObject {
    let frontend: HostedFrontend

    init(frontend: HostedFrontend) {
        self.frontend = frontend
    }
}

private struct PendingLifecycleAction: Equatable {
    let desiredState: ManagedBackend.State
    let expiresAt: Date
}

private enum RefreshMode {
    case prominent
    case background
}

private enum HomeScreenRegistry {
    static func loadBackends() throws -> [ManagedBackend] {
        var merged: [String: PartialBackend] = [:]
        for path in registryPaths() where FileManager.default.isReadableFile(atPath: path) {
            try loadRegistry(at: path, into: &merged)
        }
        return merged.values
            .filter { !isHomeScreenServiceID($0.serviceID) }
            .map { partial in
                let state = launchdState(serviceID: partial.serviceID, plistPath: partial.plistPath)
                return ManagedBackend(serviceID: partial.serviceID,
                                      displayName: partial.displayName.isEmpty ? partial.serviceID : partial.displayName,
                                      plistPath: partial.plistPath,
                                      logFiles: partial.logFiles.sorted(),
                                      frontends: partial.frontends.sorted { lhs, rhs in
                                          lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
                                      },
                                      state: state)
            }
            .sorted { lhs, rhs in
                let order = lhs.displayName.localizedCaseInsensitiveCompare(rhs.displayName)
                if order == .orderedSame {
                    return lhs.serviceID.localizedCaseInsensitiveCompare(rhs.serviceID) == .orderedAscending
                }
                return order == .orderedAscending
            }
    }

    private struct PartialBackend {
        let serviceID: String
        var displayName: String = ""
        var plistPath: String = ""
        var logFiles: Set<String> = []
        var frontends: [HostedFrontend] = []
    }

    private static func registryPaths() -> [String] {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        var paths: [String] = []
        if let override = ProcessInfo.processInfo.environment["OUTERWEBAPPS_REGISTRY"], !override.isEmpty {
            paths.append((override as NSString).expandingTildeInPath)
        } else {
            paths.append((home as NSString).appendingPathComponent("Library/Application Support/outerwebapps/registry.sqlite3"))
        }
        if let override = ProcessInfo.processInfo.environment["OUTERWEBAPPS_SYSTEM_REGISTRY"], !override.isEmpty {
            paths.append((override as NSString).expandingTildeInPath)
        } else {
            paths.append("/Library/Application Support/outerwebapps/registry.sqlite3")
        }
        return Array(Set(paths))
    }

    private static func isHomeScreenServiceID(_ serviceID: String) -> Bool {
        let value = serviceID.trimmingCharacters(in: .whitespacesAndNewlines)
        return value == "dev.outergroup.HomeScreen" ||
            value == "dev.outergroup.Navigator" ||
            value == "dev.outergroup.Backends"
    }

    private static func loadRegistry(at path: String, into merged: inout [String: PartialBackend]) throws {
        var database: OpaquePointer?
        let flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI
        let uri = "file:" + path.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed)! + "?mode=ro&immutable=1"
        guard sqlite3_open_v2(uri, &database, flags, nil) == SQLITE_OK, let database else {
            return
        }
        defer { sqlite3_close(database) }

        try query(database,
                  """
                  SELECT b.service_id,
                         COALESCE(b.display_name, ''),
                         COALESCE(l.plist_path, ''),
                         COALESCE(f.path, '')
                  FROM backends b
                  LEFT JOIN launchd_backends l ON l.service_id = b.service_id
                  LEFT JOIN log_files f ON f.service_id = b.service_id;
                  """) { statement in
            let serviceID = columnText(statement, 0)
            guard !serviceID.isEmpty else { return }
            var partial = merged[serviceID] ?? PartialBackend(serviceID: serviceID)
            let displayName = columnText(statement, 1)
            let plistPath = columnText(statement, 2)
            let logPath = columnText(statement, 3)
            if !displayName.isEmpty { partial.displayName = displayName }
            if !plistPath.isEmpty { partial.plistPath = plistPath }
            if !logPath.isEmpty { partial.logFiles.insert(logPath) }
            merged[serviceID] = partial
        }

        try query(database,
                  """
                  SELECT COALESCE(service_id, ''),
                         COALESCE(name, ''),
                         COALESCE(port, 0),
                         COALESCE(socket_path, ''),
                         COALESCE(url, '')
                  FROM frontends;
                  """) { statement in
            let serviceID = columnText(statement, 0)
            guard !serviceID.isEmpty else { return }
            var partial = merged[serviceID] ?? PartialBackend(serviceID: serviceID)
            let frontend = HostedFrontend(serviceID: serviceID,
                                          name: columnText(statement, 1),
                                          port: Int(sqlite3_column_int(statement, 2)),
                                          socketPath: columnText(statement, 3),
                                          url: columnText(statement, 4))
            if !partial.frontends.contains(frontend) {
                partial.frontends.append(frontend)
            }
            merged[serviceID] = partial
        }
    }

    private static func query(_ database: OpaquePointer,
                              _ sql: String,
                              row: (OpaquePointer) -> Void) throws {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(database, sql, -1, &statement, nil) == SQLITE_OK, let statement else {
            return
        }
        defer { sqlite3_finalize(statement) }
        while sqlite3_step(statement) == SQLITE_ROW {
            row(statement)
        }
    }

    private static func columnText(_ statement: OpaquePointer, _ index: Int32) -> String {
        guard let text = sqlite3_column_text(statement, index) else { return "" }
        return String(cString: text)
    }

    private static func launchdState(serviceID: String, plistPath: String) -> ManagedBackend.State {
        if plistPath.isEmpty {
            return .registered
        }
        guard FileManager.default.fileExists(atPath: plistPath) else {
            return .missing
        }
        let domain = plistPath.hasPrefix("/Library/LaunchDaemons/") ? "system" : "gui/\(getuid())"
        guard let result = runProcess("/bin/launchctl", ["print", "\(domain)/\(serviceID)"]),
              result.status == 0 else {
            return .stopped
        }
        var pid: Int32?
        var running = false
        for rawLine in (result.stdout + "\n" + result.stderr).split(whereSeparator: \.isNewline) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line == "state = running" {
                running = true
            } else if line.hasPrefix("pid = "),
                      let parsed = Int32(line.dropFirst("pid = ".count)),
                      parsed > 0 {
                running = true
                pid = parsed
            }
        }
        return running ? .running(pid: pid) : .stopped
    }

    static func runProcess(_ executable: String, _ arguments: [String]) -> (status: Int32, stdout: String, stderr: String)? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let stdout = Pipe()
        let stderr = Pipe()
        process.standardOutput = stdout
        process.standardError = stderr
        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return nil
        }
        return (process.terminationStatus,
                String(data: stdout.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? "",
                String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? "")
    }
}

private final class MenuIconButton: NSButton {
    private var trackingArea: NSTrackingArea?
    private var isHovered = false {
        didSet {
            needsDisplay = true
        }
    }

    override var image: NSImage? {
        didSet {
            needsDisplay = true
        }
    }

    override var contentTintColor: NSColor? {
        didSet {
            needsDisplay = true
        }
    }

    override var isEnabled: Bool {
        didSet {
            needsDisplay = true
            window?.invalidateCursorRects(for: self)
        }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        isBordered = false
        imagePosition = .imageOnly
        imageScaling = .scaleProportionallyDown
        setButtonType(.momentaryChange)
        focusRingType = .none
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()

        if let trackingArea {
            removeTrackingArea(trackingArea)
        }

        let trackingArea = NSTrackingArea(rect: bounds,
                                          options: [.activeAlways, .inVisibleRect, .mouseEnteredAndExited],
                                          owner: self,
                                          userInfo: nil)
        addTrackingArea(trackingArea)
        self.trackingArea = trackingArea
    }

    override func draw(_ dirtyRect: NSRect) {
        if isHovered, isEnabled {
            let highlightRect = bounds.insetBy(dx: 1, dy: 1)
            let highlightPath = NSBezierPath(roundedRect: highlightRect, xRadius: 6, yRadius: 6)
            effectiveAppearance.performAsCurrentDrawingAppearance {
                NSColor.unemphasizedSelectedContentBackgroundColor.withAlphaComponent(0.9).setFill()
                highlightPath.fill()
            }
        }

        guard let image else { return }

        let tintColor = contentTintColor ?? (isEnabled ? NSColor.secondaryLabelColor : NSColor.disabledControlTextColor)
        let imageSize = scaledImageSize(for: image)
        let imageRect = NSRect(x: bounds.midX - imageSize.width / 2,
                               y: bounds.midY - imageSize.height / 2,
                               width: imageSize.width,
                               height: imageSize.height)
        tintedImage(from: image, color: tintColor).draw(in: imageRect,
                                                        from: NSRect(origin: .zero, size: image.size),
                                                        operation: .sourceOver,
                                                        fraction: isEnabled ? 1.0 : 0.55,
                                                        respectFlipped: true,
                                                        hints: nil)
    }

    override func mouseEntered(with event: NSEvent) {
        guard isEnabled else { return }
        isHovered = true
    }

    override func mouseExited(with event: NSEvent) {
        isHovered = false
    }

    override func resetCursorRects() {
        guard isEnabled else { return }
        addCursorRect(bounds, cursor: .pointingHand)
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }

    private func scaledImageSize(for image: NSImage) -> NSSize {
        let maximumSide = max(min(bounds.width, bounds.height) - 8, 1)
        let originalSize = image.size
        guard originalSize.width > 0, originalSize.height > 0 else {
            return NSSize(width: maximumSide, height: maximumSide)
        }

        let scale = min(maximumSide / originalSize.width, maximumSide / originalSize.height, 1)
        return NSSize(width: originalSize.width * scale, height: originalSize.height * scale)
    }

    private func tintedImage(from image: NSImage, color: NSColor) -> NSImage {
        let tintedImage = NSImage(size: image.size)
        tintedImage.lockFocus()
        effectiveAppearance.performAsCurrentDrawingAppearance {
            let imageRect = NSRect(origin: .zero, size: image.size)
            image.draw(in: imageRect,
                       from: NSRect(origin: .zero, size: image.size),
                       operation: .sourceOver,
                       fraction: 1)
            color.setFill()
            imageRect.fill(using: .sourceIn)
        }
        tintedImage.unlockFocus()
        tintedImage.isTemplate = false
        return tintedImage
    }
}

private final class BackendHeadingMenuItemView: NSView {
    private weak var menuItem: NSMenuItem?

    init(menuItem: NSMenuItem,
         title: String,
         statusSymbolName: String,
         statusColor: NSColor,
         showsProgress: Bool) {
        self.menuItem = menuItem

        let label = NSTextField(labelWithString: title)
        label.font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
        label.textColor = .labelColor
        label.translatesAutoresizingMaskIntoConstraints = false

        super.init(frame: .zero)
        autoresizingMask = [.width]

        addSubview(label)

        let statusView: NSView
        if showsProgress {
            let progressIndicator = NSProgressIndicator()
            progressIndicator.style = .spinning
            progressIndicator.controlSize = .small
            progressIndicator.translatesAutoresizingMaskIntoConstraints = false
            progressIndicator.startAnimation(nil)
            statusView = progressIndicator
        } else {
            let statusButton = MenuIconButton()
            statusButton.translatesAutoresizingMaskIntoConstraints = false
            if let image = NSImage(systemSymbolName: statusSymbolName, accessibilityDescription: title) {
                image.isTemplate = true
                statusButton.image = image
            }
            statusButton.contentTintColor = statusColor
            statusButton.isEnabled = menuItem.isEnabled
            statusButton.target = self
            statusButton.action = #selector(statusButtonClicked(_:))
            statusView = statusButton
        }

        addSubview(statusView)

        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            label.trailingAnchor.constraint(lessThanOrEqualTo: statusView.leadingAnchor, constant: -8),
            statusView.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            statusView.centerYAnchor.constraint(equalTo: centerYAnchor),
            statusView.widthAnchor.constraint(equalToConstant: 24),
            statusView.heightAnchor.constraint(equalToConstant: 24),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 5),
            label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4)
        ])

        let contentSize = fittingSize
        frame.size = NSSize(width: 260, height: contentSize.height)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layout() {
        super.layout()

        guard let superview else { return }
        let targetWidth = max(frame.width, superview.bounds.width)
        if frame.width != targetWidth {
            frame.size.width = targetWidth
        }
    }

    @objc private func statusButtonClicked(_ sender: NSButton) {
        guard sender.isEnabled else { return }
        guard let menuItem, let menu = menuItem.menu else { return }
        menu.performActionForItem(at: menu.index(of: menuItem))
    }
}

private final class ServiceActionMenuItemView: NSView {
    private weak var menuItem: NSMenuItem?
    private let iconView = NSImageView()
    private let label: NSTextField
    private var trackingArea: NSTrackingArea?
    private var isHovered = false {
        didSet {
            updateAppearance()
            needsDisplay = true
        }
    }

    init(menuItem: NSMenuItem, symbolName: String?) {
        self.menuItem = menuItem
        self.label = NSTextField(labelWithString: menuItem.title)

        super.init(frame: .zero)

        wantsLayer = true
        autoresizingMask = [.width]

        if let symbolName,
           let image = NSImage(systemSymbolName: symbolName, accessibilityDescription: menuItem.title) {
            image.isTemplate = true
            iconView.image = image
        }
        iconView.translatesAutoresizingMaskIntoConstraints = false

        label.translatesAutoresizingMaskIntoConstraints = false

        addSubview(iconView)
        addSubview(label)

        NSLayoutConstraint.activate([
            iconView.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            iconView.centerYAnchor.constraint(equalTo: centerYAnchor),
            iconView.widthAnchor.constraint(equalToConstant: 12),
            iconView.heightAnchor.constraint(equalToConstant: 12),
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 34),
            label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 3),
            label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -3)
        ])

        updateAppearance()
        let contentSize = fittingSize
        frame.size = NSSize(width: 260, height: contentSize.height)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()

        if let trackingArea {
            removeTrackingArea(trackingArea)
        }

        let trackingArea = NSTrackingArea(rect: bounds,
                                          options: [.activeAlways, .inVisibleRect, .mouseEnteredAndExited],
                                          owner: self,
                                          userInfo: nil)
        addTrackingArea(trackingArea)
        self.trackingArea = trackingArea
    }

    override func draw(_ dirtyRect: NSRect) {
        if isHovered, menuItem?.isEnabled == true {
            let highlightRect = bounds.insetBy(dx: 6, dy: 1)
            let highlightPath = NSBezierPath(roundedRect: highlightRect, xRadius: 6, yRadius: 6)
            effectiveAppearance.performAsCurrentDrawingAppearance {
                NSColor.unemphasizedSelectedContentBackgroundColor.withAlphaComponent(0.8).setFill()
                highlightPath.fill()
            }
        }
        super.draw(dirtyRect)
    }

    override func layout() {
        super.layout()

        guard let superview else { return }
        let targetWidth = max(frame.width, superview.bounds.width)
        if frame.width != targetWidth {
            frame.size.width = targetWidth
        }
    }

    override func mouseEntered(with event: NSEvent) {
        guard menuItem?.isEnabled == true else { return }
        isHovered = true
    }

    override func mouseExited(with event: NSEvent) {
        isHovered = false
    }

    override func mouseUp(with event: NSEvent) {
        guard menuItem?.isEnabled == true else { return }
        guard bounds.contains(convert(event.locationInWindow, from: nil)) else { return }
        guard let menuItem, let menu = menuItem.menu else { return }
        let index = menu.index(of: menuItem)
        guard index >= 0 else { return }

        isHovered = false
        menu.cancelTracking()
        DispatchQueue.main.async {
            menu.performActionForItem(at: index)
        }
    }

    private func updateAppearance() {
        let enabled = menuItem?.isEnabled == true
        label.textColor = enabled ? .labelColor : .disabledControlTextColor
        iconView.contentTintColor = enabled ? .secondaryLabelColor : .disabledControlTextColor
    }
}

@MainActor
private final class HomeScreenAgentDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem?
    private let menu = NSMenu()
    private var services: [ManagedBackend] = []
    private var lastError: String?
    private var refreshTimer: Timer?
    private var followUpRefreshTask: Task<Void, Never>?
    private var pendingLifecycleActions: [String: PendingLifecycleAction] = [:]
    private var serviceExitMonitors: [String: (pid: pid_t, source: DispatchSourceProcess)] = [:]
    private var backendThread: Thread?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApplication.shared.setActivationPolicy(.accessory)
        startBackend()
        configureStatusItem()
        refresh()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 15, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.refresh()
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        refreshTimer?.invalidate()
        followUpRefreshTask?.cancel()
        cancelAllServiceExitMonitors()
        HomeScreenBackendRequestShutdown()
    }

    func menuWillOpen(_ menu: NSMenu) {
        refresh()
    }

    private func startBackend() {
        let arguments = backendArguments()
        backendThread = Thread {
            _ = runBackend(arguments: arguments)
        }
        backendThread?.name = "HomeScreenBackend"
        backendThread?.start()
    }

    private func backendArguments() -> [String] {
        var args = CommandLine.arguments
        if !args.contains("--stay-alive") {
            args.append("--stay-alive")
        }
        if !containsOption(args, "--port") && !containsOption(args, "--socket-path") {
            args.append("--socket-path")
            args.append(defaultSocketPath())
        }
        if !containsOption(args, "--launchd-socket-name") {
            args.append("--launchd-socket-name")
            args.append("Listener")
        }
        if !containsOption(args, "--bundles-dir") {
            args.append("--bundles-dir")
            args.append(defaultBundlesPath())
        }
        if !containsOption(args, "--bundled-apps-dir") && !containsOption(args, "--app-base-url") {
            args.append("--bundled-apps-dir")
            args.append(defaultBundledAppsPath())
        }
        return args
    }

    private func configureStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.menu = menu
        statusItem = item
        if let button = item.button {
            let image = NSImage(systemSymbolName: "server.rack", accessibilityDescription: "Home Screen")
            image?.isTemplate = true
            button.image = image
            button.imagePosition = .imageLeading
        }
        menu.delegate = self
        rebuildMenu()
    }

    private func refresh() {
        let previousServices = services
        let previousError = lastError
        let previousPendingActions = pendingLifecycleActions
        do {
            services = mergePendingLifecycleActions(into: try HomeScreenRegistry.loadBackends())
            lastError = nil
            reconcileServiceExitMonitors()
        } catch {
            lastError = error.localizedDescription
        }
        guard previousServices != services ||
                previousError != lastError ||
                previousPendingActions != pendingLifecycleActions else {
            return
        }
        updateStatusItem()
        rebuildMenu()
    }

    private func updateStatusItem() {
        guard let button = statusItem?.button else { return }
        let running = services.filter(\.state.isRunning).count
        button.title = running > 0 ? " \(running)" : ""
        button.toolTip = lastError ?? "\(running) running out of \(services.count) localhost backends"
    }

    private func rebuildMenu() {
        menu.removeAllItems()
        if let lastError {
            let item = NSMenuItem(title: "Error: \(lastError)", action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
            menu.addItem(.separator())
        }
        if services.isEmpty {
            let item = NSMenuItem(title: "No tracked localhost backends", action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
        } else {
            for (index, service) in services.enumerated() {
                append(service, isLast: index == services.index(before: services.endIndex))
            }
        }
        menu.addItem(.separator())
        let manage = NSMenuItem(title: "Open Home Screen in Outer Loop",
                                action: #selector(openHomeScreen(_:)),
                                keyEquivalent: "")
        manage.target = self
        menu.addItem(manage)
        let refresh = NSMenuItem(title: "Refresh",
                                 action: #selector(refreshMenuItem(_:)),
                                 keyEquivalent: "")
        refresh.target = self
        menu.addItem(refresh)
        let quit = NSMenuItem(title: "Stop showing this in menu bar",
                              action: #selector(quit(_:)),
                              keyEquivalent: "")
        quit.target = self
        menu.addItem(quit)
    }

    private func append(_ service: ManagedBackend, isLast: Bool) {
        let isLifecyclePending = pendingLifecycleActions[service.serviceID] != nil
        let heading = NSMenuItem()
        heading.target = self
        heading.action = #selector(serviceLifecycleMenuItemClicked(_:))
        heading.representedObject = BackendMenuItemPayload(service: service)
        heading.isEnabled = lifecycleItemEnabled(for: service) && !isLifecyclePending
        heading.view = BackendHeadingMenuItemView(menuItem: heading,
                                                  title: service.displayName,
                                                  statusSymbolName: lifecycleSymbolName(for: service),
                                                  statusColor: lifecycleSymbolColor(for: service),
                                                  showsProgress: isLifecyclePending)
        menu.addItem(heading)

        if service.state.isRunning {
            if service.frontends.isEmpty && !isLifecyclePending {
                let emptyItem = NSMenuItem(title: "No frontends", action: nil, keyEquivalent: "")
                emptyItem.isEnabled = false
                emptyItem.indentationLevel = 1
                menu.addItem(emptyItem)
            } else {
                for frontend in service.frontends {
                    let item = NSMenuItem(title: openTitle(for: frontend, service: service),
                                          action: #selector(openFrontendMenuItemClicked(_:)),
                                          keyEquivalent: "")
                    item.target = self
                    item.representedObject = FrontendMenuItemPayload(frontend: frontend)
                    applyCustomChildItemView(to: item, symbolName: "arrow.up.forward")
                    menu.addItem(item)
                }
            }
        }

        let logs = NSMenuItem(title: "View logs",
                              action: #selector(openLogsMenuItemClicked(_:)),
                              keyEquivalent: "")
        logs.target = self
        logs.representedObject = BackendMenuItemPayload(service: service)
        applyCustomChildItemView(to: logs, symbolName: "doc.plaintext")
        menu.addItem(logs)

        if !isLast {
            menu.addItem(.separator())
        }
    }

    @objc private func refreshMenuItem(_ sender: Any?) {
        refresh()
    }

    @objc private func quit(_ sender: Any?) {
        NSApplication.shared.terminate(sender)
    }

    @objc private func serviceLifecycleMenuItemClicked(_ sender: NSMenuItem) {
        guard let payload = sender.representedObject as? BackendMenuItemPayload else {
            return
        }
        performLifecycleAction(for: payload.service)
    }

    @objc private func openFrontendMenuItemClicked(_ sender: NSMenuItem) {
        guard let payload = sender.representedObject as? FrontendMenuItemPayload else {
            return
        }
        openFrontend(payload.frontend)
    }

    @objc private func openLogsMenuItemClicked(_ sender: NSMenuItem) {
        guard let payload = sender.representedObject as? BackendMenuItemPayload else {
            return
        }
        openLogs(payload.service)
    }

    private func openFrontend(_ frontend: HostedFrontend) {
        guard let url = outerLoopHostedAppURL(frontend) else {
            return
        }
        NSWorkspace.shared.open(url)
    }

    private func openLogs(_ service: ManagedBackend) {
        guard var components = URLComponents(string: "outerloop://open-backend-logs") else { return }
        components.queryItems = [
            URLQueryItem(name: "server", value: "localhost"),
            URLQueryItem(name: "identifier", value: service.serviceID),
            URLQueryItem(name: "name", value: service.displayName),
        ]
        if let url = components.url {
            NSWorkspace.shared.open(url)
        }
    }

    private func performLifecycleAction(for service: ManagedBackend) {
        let pendingAction = PendingLifecycleAction(desiredState: nextLifecycleState(for: service),
                                                   expiresAt: Date().addingTimeInterval(5))
        pendingLifecycleActions[service.serviceID] = pendingAction
        applyOptimisticLifecycleState(for: service, desiredState: pendingAction.desiredState)
        lastError = nil
        updateStatusItem()
        rebuildMenu()

        Task { [service] in
            let success = runLifecycleCommand(for: service)
            await MainActor.run {
                if !success {
                    self.lastError = "Could not \(service.state.isRunning ? "stop" : "start") \(service.displayName)."
                    self.pendingLifecycleActions.removeValue(forKey: service.serviceID)
                }
                self.scheduleFollowUpRefreshes()
                self.refresh()
            }
        }
    }

    private func applyOptimisticLifecycleState(for service: ManagedBackend, desiredState: ManagedBackend.State) {
        services = services
            .map { current in
                guard current.serviceID == service.serviceID else { return current }
                return ManagedBackend(serviceID: current.serviceID,
                                      displayName: current.displayName,
                                      plistPath: current.plistPath,
                                      logFiles: current.logFiles,
                                      frontends: desiredState.isRunning ? current.frontends : [],
                                      state: desiredState)
            }
            .sorted(by: sortServicesForDisplay)
    }

    private func mergePendingLifecycleActions(into fetchedServices: [ManagedBackend]) -> [ManagedBackend] {
        let now = Date()
        pendingLifecycleActions = pendingLifecycleActions.filter { $0.value.expiresAt > now }

        let currentServicesByID = Dictionary(uniqueKeysWithValues: services.map { ($0.serviceID, $0) })
        let fetchedIDs = Set(fetchedServices.map(\.serviceID))
        var mergedServices = fetchedServices.map { service in
            guard let pendingAction = pendingLifecycleActions[service.serviceID] else {
                return service
            }
            if shouldClearPendingLifecycleAction(pendingAction, for: service) {
                pendingLifecycleActions.removeValue(forKey: service.serviceID)
                return service
            }
            let fallbackFrontends = currentServicesByID[service.serviceID]?.frontends ?? service.frontends
            return ManagedBackend(serviceID: service.serviceID,
                                  displayName: service.displayName,
                                  plistPath: service.plistPath,
                                  logFiles: service.logFiles,
                                  frontends: pendingAction.desiredState.isRunning ? fallbackFrontends : [],
                                  state: pendingAction.desiredState)
        }

        for (serviceID, pendingAction) in pendingLifecycleActions where !fetchedIDs.contains(serviceID) {
            guard let current = currentServicesByID[serviceID] else { continue }
            mergedServices.append(ManagedBackend(serviceID: current.serviceID,
                                                 displayName: current.displayName,
                                                 plistPath: current.plistPath,
                                                 logFiles: current.logFiles,
                                                 frontends: pendingAction.desiredState.isRunning ? current.frontends : [],
                                                 state: pendingAction.desiredState))
        }

        return mergedServices.sorted(by: sortServicesForDisplay)
    }

    private func shouldClearPendingLifecycleAction(_ pendingAction: PendingLifecycleAction,
                                                   for service: ManagedBackend) -> Bool {
        switch pendingAction.desiredState {
        case .running:
            return service.state.isRunning
        case .stopped:
            return !service.state.isRunning
        case .registered, .missing, .unknown:
            return service.state == pendingAction.desiredState
        }
    }

    private func scheduleFollowUpRefreshes() {
        followUpRefreshTask?.cancel()
        followUpRefreshTask = Task { @MainActor [weak self] in
            let delays: [TimeInterval] = [0.35, 1.0, 2.0]
            for delay in delays {
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                guard !Task.isCancelled else { return }
                self?.refresh()
            }
        }
    }

    private func reconcileServiceExitMonitors() {
        let desiredPIDs = Dictionary(uniqueKeysWithValues: services.compactMap { service -> (String, pid_t)? in
            guard case let .running(pid) = service.state,
                  let pid,
                  pid > 0 else {
                return nil
            }
            return (service.serviceID, pid_t(pid))
        })

        let identifiersToRemove = serviceExitMonitors.compactMap { serviceID, existing in
            desiredPIDs[serviceID] == existing.pid ? nil : serviceID
        }
        for serviceID in identifiersToRemove {
            serviceExitMonitors[serviceID]?.source.cancel()
            serviceExitMonitors.removeValue(forKey: serviceID)
        }

        for (serviceID, pid) in desiredPIDs {
            if serviceExitMonitors[serviceID]?.pid == pid {
                continue
            }

            let source = DispatchSource.makeProcessSource(identifier: pid, eventMask: .exit, queue: .main)
            source.setEventHandler { [weak self] in
                self?.handleServiceExit(serviceID: serviceID, pid: pid)
            }
            source.resume()
            serviceExitMonitors[serviceID] = (pid: pid, source: source)
        }
    }

    private func handleServiceExit(serviceID: String, pid: pid_t) {
        guard let monitor = serviceExitMonitors[serviceID],
              monitor.pid == pid else {
            return
        }

        monitor.source.cancel()
        serviceExitMonitors.removeValue(forKey: serviceID)
        refresh()
    }

    private func cancelAllServiceExitMonitors() {
        for (_, monitor) in serviceExitMonitors {
            monitor.source.cancel()
        }
        serviceExitMonitors.removeAll()
    }

    private func lifecycleItemEnabled(for service: ManagedBackend) -> Bool {
        service.state.isRunning ? service.canStop : service.canStart
    }

    private func lifecycleSymbolName(for service: ManagedBackend) -> String {
        service.state.isRunning ? "stop.fill" : "play.fill"
    }

    private func lifecycleSymbolColor(for service: ManagedBackend) -> NSColor {
        lifecycleItemEnabled(for: service) ? .secondaryLabelColor : .disabledControlTextColor
    }

    private func nextLifecycleState(for service: ManagedBackend) -> ManagedBackend.State {
        service.state.isRunning ? .stopped : .running(pid: nil)
    }

    private func applyCustomChildItemView(to item: NSMenuItem, symbolName: String) {
        item.view = ServiceActionMenuItemView(menuItem: item, symbolName: symbolName)
    }

    private func openTitle(for frontend: HostedFrontend, service: ManagedBackend) -> String {
        if service.frontends.count == 1 {
            return "Open"
        }
        return "Open \(frontend.name.isEmpty ? service.displayName : frontend.name)"
    }

    private func sortServicesForDisplay(lhs: ManagedBackend, rhs: ManagedBackend) -> Bool {
        let displayNameOrder = lhs.displayName.localizedCaseInsensitiveCompare(rhs.displayName)
        if displayNameOrder == .orderedSame {
            return lhs.serviceID.localizedCaseInsensitiveCompare(rhs.serviceID) == .orderedAscending
        }
        return displayNameOrder == .orderedAscending
    }

    @objc private func openHomeScreen(_ sender: Any?) {
        var frontend = HostedFrontend(serviceID: "dev.outergroup.HomeScreen",
                                      name: "Home Screen",
                                      port: 0,
                                      socketPath: defaultSocketPath(),
                                      url: "/")
        if let registered = try? HomeScreenRegistry.loadBackends()
            .flatMap(\.frontends)
            .first(where: { $0.serviceID == "dev.outergroup.HomeScreen" }) {
            frontend = registered
        }
        if let url = outerLoopHostedAppURL(frontend) {
            NSWorkspace.shared.open(url)
        }
    }

    private func outerLoopHostedAppURL(_ frontend: HostedFrontend) -> URL? {
        var components = URLComponents()
        components.scheme = "outerloop"
        components.host = "open-hosted-app"
        components.queryItems = [
            URLQueryItem(name: "server", value: "localhost"),
            URLQueryItem(name: "backend", value: frontend.serviceID),
            URLQueryItem(name: "appURL", value: frontend.url.isEmpty ? "/" : frontend.url),
            URLQueryItem(name: "name", value: frontend.name.isEmpty ? frontend.serviceID : frontend.name),
        ]
        if !frontend.socketPath.isEmpty {
            components.queryItems?.append(URLQueryItem(name: "socketPath", value: frontend.socketPath))
        } else if frontend.port > 0 {
            components.queryItems?.append(URLQueryItem(name: "port", value: String(frontend.port)))
        }
        return components.url
    }
}

private func runBackend(arguments: [String]) -> Int32 {
    var cStrings = arguments.map { strdup($0) }
    cStrings.append(nil)
    defer {
        for pointer in cStrings where pointer != nil {
            free(pointer)
        }
    }
    return cStrings.withUnsafeMutableBufferPointer { buffer in
        HomeScreenBackendMain(Int32(arguments.count), buffer.baseAddress)
    }
}

private func runLifecycleCommand(for service: ManagedBackend) -> Bool {
    if service.isSystemService || service.plistPath.isEmpty {
        return false
    }
    if service.state.isRunning {
        return HomeScreenRegistry.runProcess("/bin/launchctl", ["bootout", "\(service.launchdDomain)/\(service.serviceID)"])?.status == 0
    }

    _ = HomeScreenRegistry.runProcess("/bin/launchctl", ["bootout", "\(service.launchdDomain)/\(service.serviceID)"])
    _ = HomeScreenRegistry.runProcess("/bin/launchctl", ["remove", service.serviceID])
    guard HomeScreenRegistry.runProcess("/bin/launchctl", ["bootstrap", "gui/\(getuid())", service.plistPath])?.status == 0 else {
        return false
    }
    return HomeScreenRegistry.runProcess("/bin/launchctl", ["kickstart", "-k", "\(service.launchdDomain)/\(service.serviceID)"])?.status == 0
}

private func containsOption(_ args: [String], _ option: String) -> Bool {
    args.contains(option)
}

private func defaultSocketPath() -> String {
    var buffer = [CChar](repeating: 0, count: Int(PATH_MAX))
    let result = confstr(_CS_DARWIN_USER_TEMP_DIR, &buffer, buffer.count)
    let directory: String
    if result > 0, let terminator = buffer.firstIndex(of: 0) {
        directory = String(decoding: buffer[..<terminator].map { UInt8(bitPattern: $0) }, as: UTF8.self)
    } else {
        directory = NSTemporaryDirectory()
    }
    return (directory as NSString).appendingPathComponent("dev.outergroup.HomeScreen")
}

private func defaultBundlesPath() -> String {
    if let resourcePath = Bundle.main.resourceURL?.appendingPathComponent("bundles").path,
       FileManager.default.fileExists(atPath: resourcePath) {
        return resourcePath
    }
    return (FileManager.default.currentDirectoryPath as NSString).appendingPathComponent("build/run/bundles")
}

private func defaultBundledAppsPath() -> String {
    if let resourcePath = Bundle.main.resourceURL?.appendingPathComponent("bundled-apps").path,
       FileManager.default.fileExists(atPath: resourcePath) {
        return resourcePath
    }
    return (FileManager.default.currentDirectoryPath as NSString).appendingPathComponent("build/run/bundled-apps")
}

@main
private enum HomeScreenAgentMain {
    @MainActor
    static func main() {
        let app = NSApplication.shared
        let delegate = HomeScreenAgentDelegate()
        app.delegate = delegate
        app.run()
    }
}
