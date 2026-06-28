import AppKit
import Darwin
import Foundation

@_silgen_name("OuterShellBackendMain")
private func OuterShellBackendMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32

@_silgen_name("OuterShelldMain")
private func OuterShelldMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32

@_silgen_name("OuterShellBackendRequestShutdown")
private func OuterShellBackendRequestShutdown()

@_silgen_name("OuterShelldRequestShutdown")
private func OuterShelldRequestShutdown()

private typealias MenuBarVisibilityCallback = @convention(c) (Int32) -> Void
private typealias MenuBarVisibilityGetter = @convention(c) () -> Int32
private typealias BackendEventChangedCallback = @convention(c) () -> Void

@_silgen_name("OuterShelldSetMenuBarVisibilityCallbacks")
private func OuterShelldSetMenuBarVisibilityCallbacks(_ callback: MenuBarVisibilityCallback?,
                                                      _ getter: MenuBarVisibilityGetter?)

@_silgen_name("OuterShelldSetBackendEventChangedCallback")
private func OuterShelldSetBackendEventChangedCallback(_ callback: BackendEventChangedCallback?)

@_silgen_name("OuterShelldMarkBackendEventChanged")
private func OuterShelldMarkBackendEventChanged()

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
    let registryScope: String
    let plistPath: String
    let logFiles: [String]
    let frontends: [HostedFrontend]
    let state: State

    var launchdDomain: String {
        plistPath.hasPrefix("/Library/LaunchDaemons/") ? "system" : "gui/\(getuid())"
    }

    var identityKey: String {
        "\(registryScope):\(serviceID)"
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

private enum MenuBarVisibilityPreference {
    static let suiteName = "org.outershell.OuterShell"
    static let key = "ShowMenuBarItemWhenBackendsAreRunning"
    static let changedNotification = Notification.Name("org.outershell.OuterShell.MenuBarVisibilityPreferenceChanged")

    static var isEnabled: Bool {
        let defaults = UserDefaults(suiteName: suiteName) ?? .standard
        defaults.synchronize()
        guard defaults.object(forKey: key) != nil else { return true }
        return defaults.bool(forKey: key)
    }

    static func setEnabled(_ enabled: Bool) {
        let defaults = UserDefaults(suiteName: suiteName) ?? .standard
        defaults.set(enabled, forKey: key)
        defaults.synchronize()
        DistributedNotificationCenter.default().postNotificationName(changedNotification,
                                                                     object: nil,
                                                                     userInfo: ["enabled": enabled],
                                                                     deliverImmediately: true)
    }
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
}

private enum OuterShellBackendAPI {
    private struct BackendRecord {
        let serviceID: String
        let displayName: String
        let serviceScope: String
        let status: String
        let launchdPlistPath: String
        let frontends: [HostedFrontend]
        let logFiles: [String]
    }

    static func loadBackends() throws -> [ManagedBackend] {
        let body = try httpGet(path: "/api/backends", socketPath: defaultSocketPath())
        let reader = BinaryPayloadReader(data: body)
        let error = try reader.stringRef(at: 0)
        if !error.isEmpty {
            throw NSError(domain: "org.outershell.OuterShellAgent",
                          code: 1,
                          userInfo: [NSLocalizedDescriptionKey: error])
        }
        let count = Int(try reader.uint32(at: 8))
        var records: [BackendRecord] = []
        records.reserveCapacity(count)
        for index in 0..<count {
            records.append(try decodeBackend(try reader.child(at: 12 + index * 8)))
        }
        return records
            .filter { $0.serviceID != "org.outershell.OuterShell" }
            .map { record in
                ManagedBackend(serviceID: record.serviceID,
                               displayName: record.displayName.isEmpty ? record.serviceID : record.displayName,
                               registryScope: record.serviceScope,
                               plistPath: record.launchdPlistPath,
                               logFiles: record.logFiles.sorted(),
                               frontends: record.frontends.sorted { lhs, rhs in
                                   lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
                               },
                               state: state(from: record.status))
            }
            .sorted { lhs, rhs in
                let order = lhs.displayName.localizedCaseInsensitiveCompare(rhs.displayName)
                if order == .orderedSame {
                    let serviceIDOrder = lhs.serviceID.localizedCaseInsensitiveCompare(rhs.serviceID)
                    if serviceIDOrder == .orderedSame {
                        return lhs.registryScope.localizedCaseInsensitiveCompare(rhs.registryScope) == .orderedDescending
                    }
                    return serviceIDOrder == .orderedAscending
                }
                return order == .orderedAscending
            }
    }

    private static func state(from status: String) -> ManagedBackend.State {
        switch status {
        case "running":
            return .running(pid: nil)
        case "stopped":
            return .stopped
        case "available", "registered":
            return .registered
        case "missing":
            return .missing
        default:
            return .unknown
        }
    }

    private static func decodeBackend(_ reader: BinaryPayloadReader) throws -> BackendRecord {
        let serviceID = try reader.stringRef(at: 0)
        return BackendRecord(serviceID: serviceID,
                             displayName: try reader.stringRef(at: 8),
                             serviceScope: try reader.stringRef(at: 32),
                             status: try reader.stringRef(at: 40),
                             launchdPlistPath: try reader.stringRef(at: 56),
                             frontends: try reader.child(at: 68).payloadArray().map { try decodeFrontend($0, serviceID: serviceID) },
                             logFiles: try reader.child(at: 76).payloadArray().compactMap { logReader in
                                 let path = try logReader.stringRef(at: 16)
                                 return path.isEmpty ? nil : path
                             })
    }

    private static func decodeFrontend(_ reader: BinaryPayloadReader, serviceID: String) throws -> HostedFrontend {
        HostedFrontend(serviceID: serviceID,
                       name: try reader.stringRef(at: 0),
                       port: Int(try reader.uint32(at: 48)),
                       socketPath: try reader.stringRef(at: 16),
                       url: try reader.stringRef(at: 8))
    }

    private static func httpGet(path: String, socketPath: String) throws -> Data {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        defer { close(fd) }

        let sunPathSize = MemoryLayout.size(ofValue: sockaddr_un().sun_path)
        guard socketPath.utf8.count < sunPathSize else {
            throw NSError(domain: "org.outershell.OuterShellAgent",
                          code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "Socket path is too long: \(socketPath)"])
        }

        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        _ = socketPath.withCString { source in
            withUnsafeMutablePointer(to: &address.sun_path) { pointer in
                pointer.withMemoryRebound(to: CChar.self,
                                          capacity: sunPathSize) { destination in
                    strncpy(destination, source, sunPathSize - 1)
                }
            }
        }

        let connected = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPointer in
                Darwin.connect(fd, sockaddrPointer, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard connected == 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }

        let request = "GET \(path) HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        try request.withCString { cString in
            var remaining = strlen(cString)
            var offset = 0
            while remaining > 0 {
                let wrote = write(fd, cString.advanced(by: offset), remaining)
                if wrote < 0 {
                    if errno == EINTR { continue }
                    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                }
                if wrote == 0 { throw POSIXError(.EPIPE) }
                offset += wrote
                remaining -= wrote
            }
        }

        var response = Data()
        var buffer = [UInt8](repeating: 0, count: 16384)
        while true {
            let got = buffer.withUnsafeMutableBytes { read(fd, $0.baseAddress, $0.count) }
            if got > 0 {
                response.append(contentsOf: buffer.prefix(got))
            } else if got == 0 {
                break
            } else if errno != EINTR {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        }

        guard let headerEnd = response.range(of: Data([13, 10, 13, 10])) else {
            throw NSError(domain: "org.outershell.OuterShellAgent",
                          code: 3,
                          userInfo: [NSLocalizedDescriptionKey: "Invalid HTTP response from Outer Shell backend."])
        }
        let headerData = response[..<headerEnd.lowerBound]
        let header = String(data: Data(headerData), encoding: .utf8) ?? ""
        guard header.hasPrefix("HTTP/1.1 200 ") || header.hasPrefix("HTTP/1.0 200 ") else {
            let statusLine = header.split(separator: "\r\n", maxSplits: 1).first.map(String.init) ?? "HTTP error"
            throw NSError(domain: "org.outershell.OuterShellAgent",
                          code: 4,
                          userInfo: [NSLocalizedDescriptionKey: statusLine])
        }
        return Data(response[headerEnd.upperBound...])
    }
}

@MainActor
private weak var activeOuterShellAgentDelegate: OuterShellAgentDelegate?

private let menuBarVisibilityCallback: MenuBarVisibilityCallback = { enabled in
    MenuBarVisibilityPreference.setEnabled(enabled != 0)
    Task { @MainActor in
        activeOuterShellAgentDelegate?.menuBarVisibilitySettingDidChange()
    }
}

private let menuBarVisibilityGetter: MenuBarVisibilityGetter = {
    MenuBarVisibilityPreference.isEnabled ? 1 : 0
}

private let backendEventChangedCallback: BackendEventChangedCallback = {
    Task { @MainActor in
        activeOuterShellAgentDelegate?.backendEventChanged()
    }
}

private enum OuterShellRegistry {
    static func loadBackends() throws -> [ManagedBackend] {
        return try OuterShellBackendAPI.loadBackends()
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

private final class SectionHeadingMenuItemView: NSView {
    private let label: NSTextField

    init(title: String) {
        self.label = NSTextField(labelWithString: title)

        super.init(frame: NSRect(x: 0, y: 0, width: 260, height: 23))
        autoresizingMask = [.width]

        label.font = NSFont.systemFont(ofSize: 11, weight: .semibold)
        label.textColor = .secondaryLabelColor
        label.translatesAutoresizingMaskIntoConstraints = false

        addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            label.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -8),
            label.centerYAnchor.constraint(equalTo: centerYAnchor, constant: 0),
        ])
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
private final class OuterShellAgentDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem?
    private let menu = NSMenu()
    private var services: [ManagedBackend] = []
    private var lastError: String?
    private var refreshTimer: Timer?
    private var followUpRefreshTask: Task<Void, Never>?
    private var pendingLifecycleActions: [String: PendingLifecycleAction] = [:]
    private var serviceExitMonitors: [String: (pid: pid_t, source: DispatchSourceProcess)] = [:]
    private var brokerThread: Thread?
    private var backendThread: Thread?

    func applicationDidFinishLaunching(_ notification: Notification) {
        activeOuterShellAgentDelegate = self
        OuterShelldSetMenuBarVisibilityCallbacks(menuBarVisibilityCallback, menuBarVisibilityGetter)
        OuterShelldSetBackendEventChangedCallback(backendEventChangedCallback)
        NSApplication.shared.setActivationPolicy(.accessory)
        startBackend()
        configureStatusItem()
        DistributedNotificationCenter.default().addObserver(self,
                                                            selector: #selector(menuBarVisibilityPreferenceChanged(_:)),
                                                            name: MenuBarVisibilityPreference.changedNotification,
                                                            object: nil)
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
        activeOuterShellAgentDelegate = nil
        OuterShelldSetMenuBarVisibilityCallbacks(nil, nil)
        OuterShelldSetBackendEventChangedCallback(nil)
        DistributedNotificationCenter.default().removeObserver(self)
        OuterShellBackendRequestShutdown()
        OuterShelldRequestShutdown()
    }

    func menuWillOpen(_ menu: NSMenu) {
        refresh()
    }

    private func startBackend() {
        let brokerArguments = brokerArguments()
        brokerThread = Thread {
            _ = runBroker(arguments: brokerArguments)
        }
        brokerThread?.name = "outershelld"
        brokerThread?.start()

        let arguments = backendArguments()
        backendThread = Thread {
            _ = runBackend(arguments: arguments)
        }
        backendThread?.name = "OuterShellBackend"
        backendThread?.start()
    }

    private func brokerArguments() -> [String] {
        var args = [CommandLine.arguments.first ?? "outershelld"]
        if !CommandLine.arguments.contains("--stay-alive") {
            args.append("--stay-alive")
        }
        if !containsOption(CommandLine.arguments, "--api-socket-path") {
            args.append("--api-socket-path")
            args.append(defaultApiSocketPath())
        }
        appendOptionIfPresent("--database", from: CommandLine.arguments, to: &args)
        appendOptionIfPresent("--system-database", from: CommandLine.arguments, to: &args)
        appendOptionIfPresent("--bundled-apps-dir", from: CommandLine.arguments, to: &args)
        appendOptionIfPresent("--public-base-url", from: CommandLine.arguments, to: &args)
        if !containsOption(args, "--bundled-apps-dir") && !containsOption(args, "--app-base-url") {
            args.append("--bundled-apps-dir")
            args.append(defaultBundledAppsPath())
        }
        return args
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
        if !containsOption(args, "--api-socket-path") {
            args.append("--api-socket-path")
            args.append(defaultApiSocketPath())
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
        menu.delegate = self
        updateStatusItem()
        rebuildMenu()
    }

    private func refresh() {
        let previousServices = services
        let previousError = lastError
        do {
            services = try OuterShellRegistry.loadBackends().filter(\.state.isRunning)
            lastError = nil
            reconcileServiceExitMonitors()
        } catch {
            lastError = error.localizedDescription
        }
        guard previousServices != services ||
                previousError != lastError else {
            return
        }
        updateStatusItem()
        rebuildMenu()
    }

    private func updateStatusItem() {
        let running = services.filter(\.state.isRunning).count
        let shouldShow = MenuBarVisibilityPreference.isEnabled && running > 0
        if shouldShow {
            ensureStatusItem()
        } else if let statusItem {
            NSStatusBar.system.removeStatusItem(statusItem)
            self.statusItem = nil
        }
        guard let button = statusItem?.button else { return }
        button.title = " \(running)"
        button.toolTip = lastError ?? "\(running) running backend\(running == 1 ? "" : "s")"
    }

    private func ensureStatusItem() {
        guard statusItem == nil else { return }
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.menu = menu
        statusItem = item
        if let button = item.button {
            let image = NSImage(systemSymbolName: "server.rack", accessibilityDescription: "Outer Shell")
            image?.isTemplate = true
            button.image = image
            button.imagePosition = .imageLeading
        }
    }

    private func rebuildMenu() {
        menu.removeAllItems()
        if let lastError {
            let item = NSMenuItem(title: "Error: \(lastError)", action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
            menu.addItem(.separator())
        }
        let duplicateDisplayNameKeys = duplicateMenuDisplayNameKeys(for: services)
        for (index, service) in services.enumerated() {
            let displayName = menuDisplayName(for: service,
                                              showsScope: duplicateDisplayNameKeys.contains(menuDisplayNameKey(for: service)))
            append(service, displayName: displayName, isLast: index == services.index(before: services.endIndex))
        }
        if !menu.items.isEmpty {
            menu.addItem(.separator())
        }
        let quit = NSMenuItem(title: "Stop showing this in menu bar",
                              action: #selector(quit(_:)),
                              keyEquivalent: "")
        quit.target = self
        menu.addItem(quit)
        quit.image = menuImage(systemSymbolName: "eye.slash")
        let manage = NSMenuItem(title: "Manage in Outer Shell",
                                action: #selector(openOuterShell(_:)),
                                keyEquivalent: "")
        manage.target = self
        manage.image = menuImage(systemSymbolName: "arrow.up.forward")
        menu.addItem(manage)
    }

    private func append(_ service: ManagedBackend, displayName: String, isLast: Bool) {
        let heading = NSMenuItem(title: displayName, action: nil, keyEquivalent: "")
        heading.isEnabled = false
        heading.view = SectionHeadingMenuItemView(title: displayName)
        menu.addItem(heading)

        let frontend = preferredFrontend(for: service)
        let open = NSMenuItem(title: "Open",
                              action: #selector(openFrontendMenuItemClicked(_:)),
                              keyEquivalent: "")
        open.target = self
        open.isEnabled = frontend != nil
        if let frontend {
            open.representedObject = FrontendMenuItemPayload(frontend: frontend)
        }
        open.image = menuImage(systemSymbolName: "arrow.up.forward")
        menu.addItem(open)

        if let frontend,
           copyableURL(for: frontend) != nil {
            let copy = NSMenuItem(title: "Copy URL",
                                  action: #selector(copyFrontendURLMenuItemClicked(_:)),
                                  keyEquivalent: "")
            copy.target = self
            copy.representedObject = FrontendMenuItemPayload(frontend: frontend)
            copy.image = menuImage(systemSymbolName: "doc.on.doc")
            menu.addItem(copy)
        }

        if !isLast {
            menu.addItem(.separator())
        }
    }

    private func duplicateMenuDisplayNameKeys(for services: [ManagedBackend]) -> Set<String> {
        let counts = services.reduce(into: [String: Int]()) { result, service in
            result[menuDisplayNameKey(for: service), default: 0] += 1
        }
        return Set(counts.compactMap { key, count in count > 1 ? key : nil })
    }

    private func menuDisplayNameKey(for service: ManagedBackend) -> String {
        let displayName = service.displayName.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return "\(service.serviceID.lowercased())\u{1f}\(displayName)"
    }

    private func menuDisplayName(for service: ManagedBackend, showsScope: Bool) -> String {
        let base = service.displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        let displayName = base.isEmpty ? service.serviceID : base
        guard showsScope else { return displayName }
        return "\(displayName) (\(service.registryScope == "system" ? "root" : "user"))"
    }

    fileprivate func setMenuBarVisibilityEnabled(_ enabled: Bool) {
        MenuBarVisibilityPreference.setEnabled(enabled)
        menuBarVisibilitySettingDidChange()
    }

    fileprivate func menuBarVisibilitySettingDidChange() {
        refresh()
        updateStatusItem()
    }

    fileprivate func backendEventChanged() {
        scheduleFollowUpRefreshes()
        refresh()
    }

    private func menuImage(systemSymbolName: String) -> NSImage? {
        let image = NSImage(systemSymbolName: systemSymbolName, accessibilityDescription: nil)
        image?.isTemplate = true
        return image
    }

    @objc private func refreshMenuItem(_ sender: Any?) {
        refresh()
    }

    @objc private func quit(_ sender: Any?) {
        MenuBarVisibilityPreference.setEnabled(false)
        OuterShelldMarkBackendEventChanged()
        updateStatusItem()
    }

    @objc private func openFrontendMenuItemClicked(_ sender: NSMenuItem) {
        guard let payload = sender.representedObject as? FrontendMenuItemPayload else {
            return
        }
        openFrontend(payload.frontend)
    }

    @objc private func copyFrontendURLMenuItemClicked(_ sender: NSMenuItem) {
        guard let payload = sender.representedObject as? FrontendMenuItemPayload,
              let url = copyableURL(for: payload.frontend) else {
            return
        }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(url.absoluteString, forType: .string)
    }

    @objc private func menuBarVisibilityPreferenceChanged(_ notification: Notification) {
        updateStatusItem()
    }

    private func openFrontend(_ frontend: HostedFrontend) {
        guard let url = outerLoopHostedAppURL(frontend) else {
            return
        }
        openInOuterLoop(url)
    }

    private func preferredFrontend(for service: ManagedBackend) -> HostedFrontend? {
        service.frontends.first { !$0.url.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty } ??
            service.frontends.first
    }

    private func copyableURL(for frontend: HostedFrontend) -> URL? {
        guard frontend.port > 0 else { return nil }
        var components = URLComponents()
        components.scheme = "http"
        components.host = "127.0.0.1"
        components.port = frontend.port
        let pathAndQuery = pathAndQuery(fromFrontendURL: frontend.url)
        if let questionIndex = pathAndQuery.firstIndex(of: "?") {
            components.path = String(pathAndQuery[..<questionIndex])
            components.query = String(pathAndQuery[pathAndQuery.index(after: questionIndex)...])
        } else {
            components.path = pathAndQuery
        }
        return components.url
    }

    private func pathAndQuery(fromFrontendURL rawURL: String) -> String {
        let trimmed = rawURL.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return "/" }
        if let components = URLComponents(string: trimmed), components.scheme != nil {
            var path = components.path.isEmpty ? "/" : components.path
            if let query = components.query, !query.isEmpty {
                path += "?\(query)"
            }
            return path
        }
        guard let slashIndex = trimmed.firstIndex(of: "/") else { return "/" }
        return String(trimmed[slashIndex...])
    }

    private func performLifecycleAction(for service: ManagedBackend) {
        let pendingAction = PendingLifecycleAction(desiredState: nextLifecycleState(for: service),
                                                   expiresAt: Date().addingTimeInterval(5))
        pendingLifecycleActions[service.identityKey] = pendingAction
        applyOptimisticLifecycleState(for: service, desiredState: pendingAction.desiredState)
        lastError = nil
        updateStatusItem()
        rebuildMenu()

        Task { [service] in
            let success = runLifecycleCommand(for: service)
            await MainActor.run {
                if !success {
                    self.lastError = "Could not \(service.state.isRunning ? "stop" : "start") \(service.displayName)."
                    self.pendingLifecycleActions.removeValue(forKey: service.identityKey)
                }
                self.scheduleFollowUpRefreshes()
                self.refresh()
            }
        }
    }

    private func applyOptimisticLifecycleState(for service: ManagedBackend, desiredState: ManagedBackend.State) {
        services = services
            .map { current in
                guard current.identityKey == service.identityKey else { return current }
                return ManagedBackend(serviceID: current.serviceID,
                                      displayName: current.displayName,
                                      registryScope: current.registryScope,
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

        let currentServicesByKey = Dictionary(uniqueKeysWithValues: services.map { ($0.identityKey, $0) })
        let fetchedKeys = Set(fetchedServices.map(\.identityKey))
        var mergedServices = fetchedServices.map { service in
            guard let pendingAction = pendingLifecycleActions[service.identityKey] else {
                return service
            }
            if shouldClearPendingLifecycleAction(pendingAction, for: service) {
                pendingLifecycleActions.removeValue(forKey: service.identityKey)
                return service
            }
            let fallbackFrontends = currentServicesByKey[service.identityKey]?.frontends ?? service.frontends
            return ManagedBackend(serviceID: service.serviceID,
                                  displayName: service.displayName,
                                  registryScope: service.registryScope,
                                  plistPath: service.plistPath,
                                  logFiles: service.logFiles,
                                  frontends: pendingAction.desiredState.isRunning ? fallbackFrontends : [],
                                  state: pendingAction.desiredState)
        }

        for (serviceKey, pendingAction) in pendingLifecycleActions where !fetchedKeys.contains(serviceKey) {
            guard let current = currentServicesByKey[serviceKey] else { continue }
            mergedServices.append(ManagedBackend(serviceID: current.serviceID,
                                                 displayName: current.displayName,
                                                 registryScope: current.registryScope,
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
            return (service.identityKey, pid_t(pid))
        })

        let identifiersToRemove = serviceExitMonitors.compactMap { serviceKey, existing in
            desiredPIDs[serviceKey] == existing.pid ? nil : serviceKey
        }
        for serviceKey in identifiersToRemove {
            serviceExitMonitors[serviceKey]?.source.cancel()
            serviceExitMonitors.removeValue(forKey: serviceKey)
        }

        for (serviceKey, pid) in desiredPIDs {
            if serviceExitMonitors[serviceKey]?.pid == pid {
                continue
            }

            let source = DispatchSource.makeProcessSource(identifier: pid, eventMask: .exit, queue: .main)
            source.setEventHandler { [weak self] in
                self?.handleServiceExit(serviceKey: serviceKey, pid: pid)
            }
            source.resume()
            serviceExitMonitors[serviceKey] = (pid: pid, source: source)
        }
    }

    private func handleServiceExit(serviceKey: String, pid: pid_t) {
        guard let monitor = serviceExitMonitors[serviceKey],
              monitor.pid == pid else {
            return
        }

        monitor.source.cancel()
        serviceExitMonitors.removeValue(forKey: serviceKey)
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
            let serviceIDOrder = lhs.serviceID.localizedCaseInsensitiveCompare(rhs.serviceID)
            if serviceIDOrder == .orderedSame {
                return lhs.registryScope.localizedCaseInsensitiveCompare(rhs.registryScope) == .orderedDescending
            }
            return serviceIDOrder == .orderedAscending
        }
        return displayNameOrder == .orderedAscending
    }

    @objc private func openOuterShell(_ sender: Any?) {
        var frontend = HostedFrontend(serviceID: "org.outershell.OuterShell",
                                      name: "Outer Shell",
                                      port: 0,
                                      socketPath: defaultSocketPath(),
                                      url: "/")
        if let registered = try? OuterShellRegistry.loadBackends()
            .flatMap(\.frontends)
            .first(where: { $0.serviceID == "org.outershell.OuterShell" }) {
            frontend = registered
        }
        if let url = outerLoopHostedAppURL(frontend) {
            openInOuterLoop(url)
        }
    }

    private func openInOuterLoop(_ url: URL) {
        let localhostServerID = "00000000-0000-0000-0000-000000000001"
        launchOuterLoopOpenHelper(url: url, serverID: localhostServerID)
    }

    private func launchOuterLoopOpenHelper(url: URL, serverID: String) {
        guard let helperURL = outerLoopOpenHelperURL() else {
            NSLog("Unable to find outerloop-open helper")
            return
        }

        let process = Process()
        process.executableURL = helperURL
        process.arguments = [
            "--server-id",
            serverID,
            url.absoluteString
        ]
        process.terminationHandler = { process in
            if process.terminationStatus != 0 {
                NSLog("outerloop-open exited with status %d", process.terminationStatus)
            }
        }

        do {
            try process.run()
        } catch {
            NSLog("Failed to launch outerloop-open: %@", error.localizedDescription)
        }
    }

    private func outerLoopOpenHelperURL() -> URL? {
        let fileManager = FileManager.default
        let appURLs = NSWorkspace.shared.urlsForApplications(withBundleIdentifier: "dev.outergroup.OuterLoop")
        let applicationURLs = appURLs + [
            URL(fileURLWithPath: "/Applications/Outer Loop.app", isDirectory: true),
            FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Applications", isDirectory: true)
                .appendingPathComponent("Outer Loop.app", isDirectory: true)
        ]

        for appURL in applicationURLs {
            let helperURL = appURL
                .appendingPathComponent("Contents", isDirectory: true)
                .appendingPathComponent("MacOS", isDirectory: true)
                .appendingPathComponent("outerloop-open")
            if fileManager.isExecutableFile(atPath: helperURL.path) {
                return helperURL
            }
        }

        return nil
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
        OuterShellBackendMain(Int32(arguments.count), buffer.baseAddress)
    }
}

private func runBroker(arguments: [String]) -> Int32 {
    var cStrings = arguments.map { strdup($0) }
    cStrings.append(nil)
    defer {
        for pointer in cStrings where pointer != nil {
            free(pointer)
        }
    }
    return cStrings.withUnsafeMutableBufferPointer { buffer in
        OuterShelldMain(Int32(arguments.count), buffer.baseAddress)
    }
}

private func runLifecycleCommand(for service: ManagedBackend) -> Bool {
    if service.isSystemService || service.plistPath.isEmpty {
        return false
    }
    if service.state.isRunning {
        return OuterShellRegistry.runProcess("/bin/launchctl", ["bootout", "\(service.launchdDomain)/\(service.serviceID)"])?.status == 0
    }

    _ = OuterShellRegistry.runProcess("/bin/launchctl", ["bootout", "\(service.launchdDomain)/\(service.serviceID)"])
    _ = OuterShellRegistry.runProcess("/bin/launchctl", ["remove", service.serviceID])
    guard OuterShellRegistry.runProcess("/bin/launchctl", ["bootstrap", "gui/\(getuid())", service.plistPath])?.status == 0 else {
        return false
    }
    return OuterShellRegistry.runProcess("/bin/launchctl", ["kickstart", "-k", "\(service.launchdDomain)/\(service.serviceID)"])?.status == 0
}

private func containsOption(_ args: [String], _ option: String) -> Bool {
    args.contains(option)
}

private func appendOptionIfPresent(_ option: String, from source: [String], to destination: inout [String]) {
    guard let index = source.firstIndex(of: option), index + 1 < source.count else {
        return
    }
    destination.append(option)
    destination.append(source[index + 1])
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
    return (directory as NSString).appendingPathComponent("org.outershell.OuterShell")
}

private func defaultApiSocketPath() -> String {
    var buffer = [CChar](repeating: 0, count: Int(PATH_MAX))
    let result = confstr(_CS_DARWIN_USER_TEMP_DIR, &buffer, buffer.count)
    let directory: String
    if result > 0, let terminator = buffer.firstIndex(of: 0) {
        directory = String(decoding: buffer[..<terminator].map { UInt8(bitPattern: $0) }, as: UTF8.self)
    } else {
        directory = NSTemporaryDirectory()
    }
    return (directory as NSString).appendingPathComponent("outershelld-api")
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
private enum OuterShellAgentMain {
    @MainActor
    static func main() {
        let app = NSApplication.shared
        let delegate = OuterShellAgentDelegate()
        app.delegate = delegate
        app.run()
    }
}
