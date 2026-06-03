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

@_silgen_name("OuterShelldSetMenuBarVisibilityCallbacks")
private func OuterShelldSetMenuBarVisibilityCallbacks(_ callback: MenuBarVisibilityCallback?,
                                                      _ getter: MenuBarVisibilityGetter?)

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

private enum OuterShellRegistry {
    static func loadBackends() throws -> [ManagedBackend] {
        var merged: [String: PartialBackend] = [:]
        for path in registryPaths() where FileManager.default.isReadableFile(atPath: path) {
            try loadRegistry(at: path, into: &merged)
        }
        return merged.values
            .filter { !isOuterShellServiceID($0.serviceID) }
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

    private struct OrwaDescriptor {
        let offset: Int
        let rowCount: Int
        let rowSize: Int
    }

    private static let orwaDescriptorSize = 20
    private static let orwaTableCount = 5
    private static let orwaLegacyFourTableCount = 4
    private static let orwaLegacyThreeTableCount = 3
    private static let orwaHeaderSize = 8 + orwaTableCount * orwaDescriptorSize
    private static let orwaLegacyFourTableHeaderSize = 8 + orwaLegacyFourTableCount * orwaDescriptorSize
    private static let orwaLegacyThreeTableHeaderSize = 8 + orwaLegacyThreeTableCount * orwaDescriptorSize
    private static let orwaBackendsRowSize = 68
    private static let orwaLegacyBackendsRowSize = 84
    private static let orwaLegacyFrontendsRowSize = 97
    private static let orwaFrontendsRowSize = 113
    private static let orwaFrontendsRowSizeWithFlags = 117
    private static let orwaFrontendLayoutsRowSize = 32
    private static let orwaLogFilesRowSize = 32
    private static let orwaFileOpenersRowSize = 88

    private static func registryPaths() -> [String] {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        var paths: [String] = []
        if let override = ProcessInfo.processInfo.environment["OUTERSHELL_REGISTRY"], !override.isEmpty {
            paths.append(orwaPath(for: (override as NSString).expandingTildeInPath))
        } else {
            paths.append((home as NSString).appendingPathComponent("Library/Application Support/outershell/registry.orwa"))
        }
        if let override = ProcessInfo.processInfo.environment["OUTERSHELL_SYSTEM_REGISTRY"], !override.isEmpty {
            paths.append(orwaPath(for: (override as NSString).expandingTildeInPath))
        } else {
            paths.append("/Library/Application Support/outershell/registry.orwa")
        }
        return Array(Set(paths))
    }

    private static func orwaPath(for registryPath: String) -> String {
        let path = registryPath as NSString
        if path.lastPathComponent == "registry.orwa" {
            return registryPath
        }
        return (path.deletingLastPathComponent as NSString).appendingPathComponent("registry.orwa")
    }

    private static func isOuterShellServiceID(_ serviceID: String) -> Bool {
        let value = serviceID.trimmingCharacters(in: .whitespacesAndNewlines)
        return value == "org.outershell.OuterShell" ||
            value == "dev.outergroup.Navigator" ||
            value == "dev.outergroup.Backends"
    }

    private static func loadRegistry(at path: String, into merged: inout [String: PartialBackend]) throws {
        let data = try Data(contentsOf: URL(fileURLWithPath: path), options: [.mappedIfSafe])
        guard data.count >= orwaLegacyThreeTableHeaderSize,
              data.starts(with: [0x4f, 0x52, 0x57, 0x41]),
              readUInt32(data, 4) == 1 else {
            return
        }

        let firstTableOffset = readUInt64(data, 8)
        let tableCount: Int
        if firstTableOffset == UInt64(orwaHeaderSize) {
            tableCount = orwaTableCount
        } else if firstTableOffset == UInt64(orwaLegacyFourTableHeaderSize) {
            tableCount = orwaLegacyFourTableCount
        } else if firstTableOffset == UInt64(orwaLegacyThreeTableHeaderSize) {
            tableCount = orwaLegacyThreeTableCount
        } else {
            return
        }
        guard data.count >= 8 + tableCount * orwaDescriptorSize else { return }

        var descriptors: [OrwaDescriptor] = []
        var variableOffset = 0
        for tableIndex in 0..<tableCount {
            let descriptorOffset = 8 + tableIndex * orwaDescriptorSize
            let offset = Int(readUInt64(data, descriptorOffset))
            let rowCount = Int(readUInt64(data, descriptorOffset + 8))
            let rowSize = Int(readUInt32(data, descriptorOffset + 16))
            guard rowSize > 0,
                  offset >= 0,
                  rowCount >= 0,
                  rowCount <= (Int.max / rowSize),
                  offset <= data.count,
                  rowCount * rowSize <= data.count - offset,
                  rowSizeIsValid(rowSize, tableIndex: tableIndex, tableCount: tableCount) else {
                return
            }
            descriptors.append(OrwaDescriptor(offset: offset, rowCount: rowCount, rowSize: rowSize))
            variableOffset = max(variableOffset, offset + rowCount * rowSize)
        }

        let backends = descriptors[0]
        for row in 0..<backends.rowCount {
            let rowOffset = backends.offset + row * backends.rowSize
            let serviceID = readString(data, variableOffset, rowOffset)
            guard !serviceID.isEmpty else { continue }
            var partial = merged[serviceID] ?? PartialBackend(serviceID: serviceID)
            let displayName = readString(data, variableOffset, rowOffset + 16)
            let plistPath: String
            if backends.rowSize >= orwaLegacyBackendsRowSize {
                plistPath = readString(data, variableOffset, rowOffset + 64)
            } else {
                plistPath = readString(data, variableOffset, rowOffset + 48)
            }
            if !displayName.isEmpty { partial.displayName = displayName }
            if !plistPath.isEmpty { partial.plistPath = plistPath }
            merged[serviceID] = partial
        }

        let frontends = descriptors[1]
        for row in 0..<frontends.rowCount {
            let rowOffset = frontends.offset + row * frontends.rowSize
            let url = readString(data, variableOffset, rowOffset)
            let serviceID = readString(data, variableOffset, rowOffset + 16)
            guard !serviceID.isEmpty else { continue }
            let displayName = readString(data, variableOffset, rowOffset + 32)
            var port = 0
            var socketPath = ""
            if rowOffset + 81 < data.count {
                let endpointKind = data[rowOffset + 80]
                if endpointKind == 1 {
                    port = Int(readUInt32(data, rowOffset + 81))
                } else if endpointKind == 2 {
                    socketPath = readString(data, variableOffset, rowOffset + 81)
                }
            }
            var partial = merged[serviceID] ?? PartialBackend(serviceID: serviceID)
            let frontend = HostedFrontend(serviceID: serviceID,
                                          name: displayName,
                                          port: port,
                                          socketPath: socketPath,
                                          url: url)
            if !partial.frontends.contains(frontend) {
                partial.frontends.append(frontend)
            }
            merged[serviceID] = partial
        }

        let logTableIndex = tableCount == orwaLegacyThreeTableCount ? 2 : 3
        guard logTableIndex < descriptors.count else { return }
        let logs = descriptors[logTableIndex]
        for row in 0..<logs.rowCount {
            let rowOffset = logs.offset + row * logs.rowSize
            let path = readString(data, variableOffset, rowOffset)
            let serviceID = readString(data, variableOffset, rowOffset + 16)
            guard !serviceID.isEmpty, !path.isEmpty else { continue }
            var partial = merged[serviceID] ?? PartialBackend(serviceID: serviceID)
            partial.logFiles.insert(path)
            merged[serviceID] = partial
        }
    }

    private static func rowSizeIsValid(_ rowSize: Int, tableIndex: Int, tableCount: Int) -> Bool {
        switch tableIndex {
        case 0:
            return rowSize == orwaBackendsRowSize || rowSize == orwaLegacyBackendsRowSize
        case 1:
            return rowSize == orwaFrontendsRowSize || rowSize == orwaFrontendsRowSizeWithFlags || rowSize == orwaLegacyFrontendsRowSize
        case 2 where tableCount != orwaLegacyThreeTableCount:
            return rowSize == orwaFrontendLayoutsRowSize
        case 4:
            return rowSize == orwaFileOpenersRowSize
        default:
            return rowSize == orwaLogFilesRowSize
        }
    }

    private static func readString(_ data: Data, _ variableOffset: Int, _ refOffset: Int) -> String {
        guard refOffset >= 0, refOffset + 16 <= data.count else { return "" }
        let offset = Int(readUInt64(data, refOffset))
        let length = Int(readUInt64(data, refOffset + 8))
        guard length > 0,
              offset >= variableOffset,
              offset <= data.count,
              length <= data.count - offset else {
            return ""
        }
        return String(data: data.subdata(in: offset..<(offset + length)), encoding: .utf8) ?? ""
    }

    private static func readUInt32(_ data: Data, _ offset: Int) -> UInt32 {
        guard offset + 4 <= data.count else { return 0 }
        return UInt32(data[offset]) |
            (UInt32(data[offset + 1]) << 8) |
            (UInt32(data[offset + 2]) << 16) |
            (UInt32(data[offset + 3]) << 24)
    }

    private static func readUInt64(_ data: Data, _ offset: Int) -> UInt64 {
        guard offset + 8 <= data.count else { return 0 }
        var value: UInt64 = 0
        for i in 0..<8 {
            value |= UInt64(data[offset + i]) << UInt64(i * 8)
        }
        return value
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
        if !containsOption(args, "--bundled-apps-dir") {
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
        for (index, service) in services.enumerated() {
            append(service, isLast: index == services.index(before: services.endIndex))
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
        let manage = NSMenuItem(title: "View in Outer Shell",
                                action: #selector(openOuterShell(_:)),
                                keyEquivalent: "")
        manage.target = self
        manage.image = menuImage(systemSymbolName: "arrow.up.forward")
        menu.addItem(manage)
    }

    private func append(_ service: ManagedBackend, isLast: Bool) {
        let heading = NSMenuItem(title: service.displayName, action: nil, keyEquivalent: "")
        heading.isEnabled = false
        heading.view = SectionHeadingMenuItemView(title: service.displayName)
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

    fileprivate func setMenuBarVisibilityEnabled(_ enabled: Bool) {
        MenuBarVisibilityPreference.setEnabled(enabled)
        menuBarVisibilitySettingDidChange()
    }

    fileprivate func menuBarVisibilitySettingDidChange() {
        refresh()
        updateStatusItem()
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
