//
//  OuterframeHost.swift
//  OuterframeSwiftMethods
//
//  Method-based API for browser communication, wrapping the socket protocol.
//

import AppKit
import Foundation
import Network
import QuartzCore
import UniformTypeIdentifiers

enum OuterframeHostError: LocalizedError {
    case apiFailure(String)

    var errorDescription: String? {
        switch self {
        case .apiFailure(let methodName):
            return "Missing method, the API may have changed: \(methodName)"
        }
    }
}

typealias OuterframeHostMessageHandler = @MainActor (OuterframeHost, BrowserToContentMessage) -> Void
typealias OuterframeHostDisconnectHandler = @MainActor (OuterframeHost) -> Void

private enum OuterframeHostPasteboardPayload {
    static let filePromiseTypeIdentifier = "org.outerframe.file-promise"

    private static let version: UInt32 = 1
    private static let missingSize = UInt64.max

    static func encodeFilePromise(id: UUID,
                                  name: String,
                                  fileSize: UInt64?,
                                  fileType: String?) -> Data? {
        var builder = BinaryPayloadBuilder(referenceBaseOffset: 0)
        builder.append(uint32: version)
        builder.append(uint32: 0)
        builder.append(uuid: id)
        builder.append(uint64: fileSize ?? missingSize)
        guard builder.append(stringReference: name),
              builder.append(stringReference: fileType ?? "") else {
            return nil
        }
        return builder.finalize()
    }
}

private struct BinaryPayloadBuilder {
    private struct Reference {
        let patchOffset: Int
        let variableOffset: Int
        let length: Int
    }

    private var fixed = Data()
    private var variable = Data()
    private var references: [Reference] = []
    private let referenceBaseOffset: Int

    init(referenceBaseOffset: Int) {
        self.referenceBaseOffset = referenceBaseOffset
    }

    mutating func append(uint32 value: UInt32) {
        fixed.appendLittleEndian(value)
    }

    mutating func append(uint64 value: UInt64) {
        fixed.appendLittleEndian(value)
    }

    mutating func append(uuid value: UUID) {
        fixed.append(uuid: value)
    }

    mutating func append(stringReference string: String) -> Bool {
        guard let data = string.data(using: .utf8),
              data.count <= Int(UInt32.max) else {
            return false
        }
        let patchOffset = fixed.count
        fixed.appendLittleEndian(UInt32(0))
        fixed.appendLittleEndian(UInt32(data.count))
        references.append(Reference(patchOffset: patchOffset,
                                    variableOffset: variable.count,
                                    length: data.count))
        variable.append(data)
        return true
    }

    mutating func finalize() -> Data? {
        guard fixed.count <= Int(UInt32.max),
              variable.count <= Int(UInt32.max),
              variable.count <= Int(UInt32.max) - fixed.count else {
            return nil
        }

        for reference in references {
            let offset = referenceBaseOffset + fixed.count + reference.variableOffset
            guard offset <= Int(UInt32.max),
                  reference.length <= Int(UInt32.max) else {
                return nil
            }
            fixed.replaceLittleEndianUInt32(at: reference.patchOffset, with: UInt32(offset))
            fixed.replaceLittleEndianUInt32(at: reference.patchOffset + 4, with: UInt32(reference.length))
        }

        var payload = Data(capacity: fixed.count + variable.count)
        payload.append(fixed)
        payload.append(variable)
        return payload
    }
}

private extension Data {
    mutating func appendLittleEndian(_ value: UInt32) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) { append(contentsOf: $0) }
    }

    mutating func appendLittleEndian(_ value: UInt64) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) { append(contentsOf: $0) }
    }

    mutating func append(uuid value: UUID) {
        var uuidValue = value.uuid
        Swift.withUnsafeBytes(of: &uuidValue) { append(contentsOf: $0) }
    }

    mutating func replaceLittleEndianUInt32(at offset: Int, with value: UInt32) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) {
            replaceSubrange(offset..<(offset + 4), with: $0)
        }
    }
}

/// Delegate for receiving decoded messages from the browser.
@MainActor
protocol OuterframeHostDelegate: AnyObject {
    /// Called when a message is received from the browser.
    /// Note: displayLinkFired and displayLinkCallbackRegistered
    /// are handled internally by OuterframeHost and will not be forwarded to this delegate.
    func outerframeHost(_ host: OuterframeHost, didReceiveMessage message: BrowserToContentMessage)

    /// Called when the connection to the browser is closed.
    func outerframeHostDidDisconnect(_ host: OuterframeHost)
}

/// Helper class providing method-based API for browser communication
@MainActor
final class OuterframeHost: SocketToBrowserDelegate {
    let socket: SocketToBrowser
    var stagedFileDirectoryURL: URL? {
        guard let path = ProcessInfo.processInfo.environment["OUTERFRAME_STAGING_DIR"],
              !path.isEmpty else {
            return nil
        }
        return URL(fileURLWithPath: path, isDirectory: true)
    }

    /// Delegate for receiving decoded messages from the browser.
    weak var delegate: OuterframeHostDelegate?
    private var messageHandler: OuterframeHostMessageHandler?
    private var disconnectHandler: OuterframeHostDisconnectHandler?

    /// The URL that was navigated to (e.g., "https://example.com/apps/top.outer?host=server1")
    private var _url: String?

    /// The URL where the plugin bundle was downloaded from
    private var _bundleUrl: String?

    private var _currentHistoryEntryID: UUID?
    private var _historyLength: UInt32 = 0
    private var _canGoBack = false
    private var _canGoForward = false

    // Display link callback management
    private var displayLinkCallbacks: [UUID: @MainActor @Sendable (CFTimeInterval) -> Void] = [:]
    private var pendingDisplayLinkCallbacks: [UUID: @MainActor @Sendable (CFTimeInterval) -> Void] = [:]
    private var callbackIDToBrowserID: [UUID: UUID] = [:]
    private var browserIDToCallbackID: [UUID: UUID] = [:]
    private var pendingPasteboardAccessRequests: [UUID: @MainActor (Bool, [OuterContentPasteboardItem]) -> Void] = [:]

    /// Creates an OuterframeHost and starts the socket.
    /// Call `configure()` after receiving the initializeContent message to set context and appearance.
    init(socketFD: Int32,
         messageHandler: OuterframeHostMessageHandler? = nil,
         disconnectHandler: OuterframeHostDisconnectHandler? = nil) {
        let socket = SocketToBrowser()
        self.socket = socket
        self._url = nil
        self._bundleUrl = nil
        self.messageHandler = messageHandler
        self.disconnectHandler = disconnectHandler

        // Set ourselves as the socket delegate to decode messages
        socket.delegate = self

        // Start the socket for plugin communication
        Task {
            await socket.start(withFileDescriptor: socketFD)
        }
    }

    // MARK: - SocketToBrowserDelegate

    nonisolated func socketToBrowser(_ socket: SocketToBrowser, didReceiveMessage message: Data) {
        Task { @MainActor in
            handleRawMessage(messageData: message)
        }
    }

    nonisolated func socketToBrowserDidClose(_ socket: SocketToBrowser) {
        Task { @MainActor in
            if let disconnectHandler {
                disconnectHandler(self)
            } else {
                delegate?.outerframeHostDidDisconnect(self)
            }
        }
    }

    func setMessageHandler(_ handler: OuterframeHostMessageHandler?) {
        messageHandler = handler
    }

    func setDisconnectHandler(_ handler: OuterframeHostDisconnectHandler?) {
        disconnectHandler = handler
    }

    private func handleRawMessage(messageData: Data) {
        let message: BrowserToContentMessage
        do {
            message = try BrowserToContentMessage.decode(message: messageData)
        } catch {
            print("OuterframeHost: Failed to decode message: \(error)")
            return
        }

        // Handle internal messages that OuterframeHost manages
        switch message {
        case .displayLinkFired(_, let targetTimestamp):
            handleDisplayLinkFired(targetTimestamp: targetTimestamp)
            return

        case .displayLinkCallbackRegistered(let callbackID, let browserCallbackID):
            handleDisplayLinkCallbackRegistered(callbackID: callbackID, browserCallbackID: browserCallbackID)
            return

        case .pasteboardAccessResponse(let requestID, let granted, let items):
            if let completion = pendingPasteboardAccessRequests.removeValue(forKey: requestID) {
                completion(granted, items)
            }
            return

        case .initializeContent(let arguments):
            _currentHistoryEntryID = arguments.historyEntryID

        case .historyEntryAccepted(let entryID, let url),
             .historyTraversal(let entryID, let url):
            _currentHistoryEntryID = entryID
            _url = url

        case .historyContextUpdate(let currentEntryID, let url, let length, let canGoBack, let canGoForward):
            _currentHistoryEntryID = currentEntryID
            _url = url
            _historyLength = length
            _canGoBack = canGoBack
            _canGoForward = canGoForward

        default:
            break
        }

        if let messageHandler {
            messageHandler(self, message)
        } else {
            delegate?.outerframeHost(self, didReceiveMessage: message)
        }
    }

    /// Configures the host with data from the initializeContent message.
    func configure(url: String,
                   bundleUrl: String,
                   proxyHost: String?,
                   proxyPort: UInt16,
                   proxyUsername: String?,
                   proxyPassword: String?) {
        self._url = url
        self._bundleUrl = bundleUrl
        self._networkProxyHost = proxyHost
        self._networkProxyPort = proxyPort
        self._networkProxyUsername = proxyUsername
        self._networkProxyPassword = proxyPassword
    }

    // MARK: - Cursor

    func setCursor(_ cursorType: PluginCursorType) {
        Task {
            try? await socket.send(ContentToBrowserMessage.cursorUpdate(cursorType: UInt8(cursorType.rawValue)).encode())
        }
    }

    // MARK: - Input Mode

    func setInputMode(_ inputMode: OuterframeContentInputMode) {
        Task {
            try? await socket.send(ContentToBrowserMessage.inputModeUpdate(inputMode: inputMode.rawValue).encode())
        }
    }

    // MARK: - Pasteboard

    func sendEditCommandValidationResponse(requestID: UUID, enabledCommands: OuterframeEditCommandSet) {
        Task {
            try? await socket.send(ContentToBrowserMessage.editCommandValidationResponse(
                requestID: requestID,
                enabledCommands: enabledCommands
            ).encode())
        }
    }

    func setPasteboardDropBehaviorUniform(_ pasteboardTypeIdentifiers: [String]) {
        Task {
            try? await socket.send(ContentToBrowserMessage.setPasteboardDropBehaviorUniform(
                pasteboardTypeIdentifiers
            ).encode())
        }
    }

    func setAcceptedPasteboardPasteTypes(_ pasteboardTypeIdentifiers: [String]) {
        Task {
            try? await socket.send(ContentToBrowserMessage.setAcceptedPasteboardPasteTypes(
                pasteboardTypeIdentifiers
            ).encode())
        }
    }

    func setPasteboardDropBehaviorHitTest() {
        Task {
            try? await socket.send(ContentToBrowserMessage.setPasteboardDropBehaviorHitTest.encode())
        }
    }

    func setPasteboardDropBehaviorHitTest(acceptedTypes pasteboardTypeIdentifiers: [String]) {
        Task {
            try? await socket.send(ContentToBrowserMessage.setPasteboardDropBehaviorUniform(
                pasteboardTypeIdentifiers
            ).encode())
            try? await socket.send(ContentToBrowserMessage.setPasteboardDropBehaviorHitTest.encode())
        }
    }

    func sendPasteboardDropHitTestResponse(requestID: UUID, operationMask: NSDragOperation) {
        Task {
            try? await socket.send(ContentToBrowserMessage.pasteboardDropHitTestResponse(
                requestID: requestID,
                operationMask: UInt32(truncatingIfNeeded: operationMask.rawValue)
            ).encode())
        }
    }

    // MARK: - Display Link

    func registerDisplayLinkCallback(_ callback: @MainActor @Sendable @escaping (CFTimeInterval) -> Void) -> UUID {
        let callbackID = UUID()
        pendingDisplayLinkCallbacks[callbackID] = callback

        Task {
            try? await socket.send(ContentToBrowserMessage.startDisplayLink(callbackID: callbackID).encode())
        }

        return callbackID
    }

    func stopDisplayLinkCallback(_ callbackID: UUID) {
        pendingDisplayLinkCallbacks.removeValue(forKey: callbackID)
        displayLinkCallbacks.removeValue(forKey: callbackID)

        if let browserID = callbackIDToBrowserID.removeValue(forKey: callbackID) {
            browserIDToCallbackID.removeValue(forKey: browserID)
            Task {
                try? await socket.send(ContentToBrowserMessage.stopDisplayLink(browserCallbackID: browserID).encode())
            }
        }
    }

    private func handleDisplayLinkCallbackRegistered(callbackID: UUID, browserCallbackID: UUID) {
        callbackIDToBrowserID[callbackID] = browserCallbackID
        browserIDToCallbackID[browserCallbackID] = callbackID

        if let callback = pendingDisplayLinkCallbacks.removeValue(forKey: callbackID) {
            displayLinkCallbacks[callbackID] = callback
        }
    }

    private func handleDisplayLinkFired(targetTimestamp: Double) {
        for callback in displayLinkCallbacks.values {
            callback(targetTimestamp)
        }
    }

    // MARK: - Text Input Geometry

    func sendTextInputGeometryUpdate(_ geometry: OuterContentTextInputGeometry?) {
        Task {
            try? await socket.send(ContentToBrowserMessage.textInputGeometryUpdate(geometry: geometry).encode())
        }
    }

    // MARK: - Navigation

    func openNewWindow(with url: URL, displayString: String?, preferredSize: CGSize?) {
        Task {
            try? await socket.send(ContentToBrowserMessage.openNewWindow(
                url: url.absoluteString,
                displayString: displayString,
                preferredSize: preferredSize
            ).encode())
        }
    }

    func navigate(to url: URL) {
        Task {
            try? await socket.send(ContentToBrowserMessage.navigate(url: url.absoluteString).encode())
        }
    }

    func openNewTab(with url: URL, displayString: String?) {
        Task {
            try? await socket.send(ContentToBrowserMessage.openNewTab(
                url: url.absoluteString,
                displayString: displayString
            ).encode())
        }
    }

    @discardableResult
    func pushHistoryEntry(url: URL?) -> UUID {
        let entryID = UUID()
        Task {
            try? await socket.send(ContentToBrowserMessage.historyPushEntry(
                entryID: entryID,
                url: url?.absoluteString
            ).encode())
        }
        return entryID
    }

    @discardableResult
    func replaceHistoryEntry(url: URL?) -> UUID {
        let entryID = UUID()
        Task {
            try? await socket.send(ContentToBrowserMessage.historyReplaceEntry(
                entryID: entryID,
                url: url?.absoluteString
            ).encode())
        }
        return entryID
    }

    func goInHistory(by delta: Int32) {
        Task {
            try? await socket.send(ContentToBrowserMessage.historyGo(delta: delta).encode())
        }
    }

    func goBackInHistory() {
        goInHistory(by: -1)
    }

    func goForwardInHistory() {
        goInHistory(by: 1)
    }

    func showContextMenu(for attributedText: NSAttributedString, at location: CGPoint) {
        guard let data = try? attributedText.data(from: NSRange(location: 0, length: attributedText.length),
                                                  documentAttributes: [.documentType: NSAttributedString.DocumentType.rtf]) else {
            return
        }
        Task {
            try? await socket.send(ContentToBrowserMessage.showContextMenu(
                attributedTextData: data,
                locationX: location.x,
                locationY: location.y
            ).encode())
        }
    }

    func showContextMenu(menuID: UUID,
                         items: [OuterframeContextMenuItem],
                         at location: CGPoint,
                         attributedText: NSAttributedString? = nil) {
        let attributedTextData = attributedText.flatMap {
            try? $0.data(from: NSRange(location: 0, length: $0.length),
                         documentAttributes: [.documentType: NSAttributedString.DocumentType.rtf])
        }
        Task {
            try? await socket.send(ContentToBrowserMessage.showContextMenuItems(
                menuID: menuID,
                locationX: location.x,
                locationY: location.y,
                attributedTextData: attributedTextData,
                items: items
            ).encode())
        }
    }

    func showDefinition(for attributedText: NSAttributedString, at location: CGPoint) {
        guard let data = try? attributedText.data(from: NSRange(location: 0, length: attributedText.length),
                                                  documentAttributes: [.documentType: NSAttributedString.DocumentType.rtf]) else {
            return
        }
        Task {
            try? await socket.send(ContentToBrowserMessage.showDefinition(
                attributedTextData: data,
                locationX: location.x,
                locationY: location.y
            ).encode())
        }
    }

    // MARK: - Haptic Feedback

    func performHapticFeedback(_ style: OuterframeHapticFeedbackStyle) {
        Task {
            try? await socket.send(ContentToBrowserMessage.hapticFeedback(style: UInt8(style.rawValue)).encode())
        }
    }

    func sendAccessibilitySnapshotResponse(requestID: UUID, snapshotData: Data?) {
        do {
            try socket.sendBlocking(ContentToBrowserMessage.accessibilitySnapshotResponse(
                requestID: requestID,
                snapshotData: snapshotData
            ).encode())
        } catch {
            print("OuterframeHost: Failed to send accessibility snapshot response: \(error)")
        }
    }

    func sendAccessibilitySnapshotResponse(requestID: UUID, snapshot: OuterframeAccessibilitySnapshot?) {
        sendAccessibilitySnapshotResponse(
            requestID: requestID,
            snapshotData: (snapshot ?? OuterframeAccessibilitySnapshot.notImplementedSnapshot()).serializedData()
        )
    }

    func notifyAccessibilityTreeChanged(_ notifications: OuterframeAccessibilityNotification = .layoutChanged) {
        Task {
            try? await socket.send(ContentToBrowserMessage.accessibilityTreeChanged(
                notificationMask: notifications.rawValue
            ).encode())
        }
    }

    // MARK: - Pasteboard

    /// Sends a copy selected pasteboard response to the browser.
    func sendCopySelectedPasteboardResponse(requestID: UUID, items: [OuterContentPasteboardItem]) {
        Task {
            try? await socket.send(ContentToBrowserMessage.selectionToPasteboardResponse(
                requestID: requestID,
                items: items
            ).encode())
        }
    }

    func requestPasteboardWrite(items: [OuterContentPasteboardItem],
                                completion: (@MainActor (Bool) -> Void)? = nil) {
        let requestID = UUID()
        pendingPasteboardAccessRequests[requestID] = { granted, _ in
            completion?(granted)
        }
        Task {
            do {
                try await socket.send(ContentToBrowserMessage.pasteboardAccessRequest(
                    requestID: requestID,
                    operation: .write,
                    pasteboardTypes: [],
                    items: items
                ).encode())
            } catch {
                await MainActor.run {
                    if let pending = self.pendingPasteboardAccessRequests.removeValue(forKey: requestID) {
                        pending(false, [])
                    }
                }
            }
        }
    }

    func requestPasteboardRead(typeIdentifiers: [String],
                               completion: @escaping @MainActor (Bool, [OuterContentPasteboardItem]) -> Void) {
        let requestID = UUID()
        pendingPasteboardAccessRequests[requestID] = completion
        Task {
            do {
                try await socket.send(ContentToBrowserMessage.pasteboardAccessRequest(
                    requestID: requestID,
                    operation: .read,
                    pasteboardTypes: typeIdentifiers,
                    items: []
                ).encode())
            } catch {
                await MainActor.run {
                    if let pending = self.pendingPasteboardAccessRequests.removeValue(forKey: requestID) {
                        pending(false, [])
                    }
                }
            }
        }
    }

    func beginDraggingPasteboardItems(_ items: [OuterContentPasteboardItem],
                                      operationMask: NSDragOperation = .copy) {
        let draggingItems = items.map { OuterContentDraggingItem(pasteboardItem: $0) }
        beginDraggingPasteboardItems(draggingItems, operationMask: operationMask)
    }

    func beginDraggingPasteboardItem(_ item: OuterContentPasteboardItem,
                                     operationMask: NSDragOperation = .copy,
                                     previewPNGData: Data?,
                                     previewSize: CGSize?,
                                     previewFrameOrigin: CGPoint? = nil) {
        beginDraggingPasteboardItems(
            [
                OuterContentDraggingItem(
                    pasteboardItem: item,
                    previewImageData: previewPNGData,
                    previewSize: previewSize,
                    previewFrameOrigin: previewFrameOrigin
                )
            ],
            operationMask: operationMask
        )
    }

    func beginDraggingPasteboardItems(_ items: [OuterContentDraggingItem],
                                      operationMask: NSDragOperation = .copy) {
        Task {
            try? await socket.send(ContentToBrowserMessage.beginDraggingPasteboardItems(
                items: items,
                operationMask: UInt32(truncatingIfNeeded: operationMask.rawValue)
            ).encode())
        }
    }

    func filePromisePasteboardItem(promiseID: UUID,
                                   name: String,
                                   fileSize: UInt64? = nil,
                                   fileType: String? = nil) -> OuterContentPasteboardItem? {
        guard !name.isEmpty,
              let payload = OuterframeHostPasteboardPayload.encodeFilePromise(
                id: promiseID,
                name: name,
                fileSize: fileSize,
                fileType: fileType
              ) else {
            return nil
        }

        return OuterContentPasteboardItem(representations: [
            OuterContentPasteboardRepresentation(typeIdentifier: OuterframeHostPasteboardPayload.filePromiseTypeIdentifier,
                                                 data: payload)
        ])
    }

    func beginDraggingFilePromise(promiseID: UUID,
                                  name: String,
                                  fileSize: UInt64? = nil,
                                  fileType: String? = nil,
                                  operationMask: NSDragOperation = .copy,
                                  previewPNGData: Data? = nil,
                                  previewSize: CGSize? = nil) {
        guard let pasteboardItem = filePromisePasteboardItem(promiseID: promiseID,
                                                             name: name,
                                                             fileSize: fileSize,
                                                             fileType: fileType) else {
            return
        }
        beginDraggingPasteboardItem(pasteboardItem,
                                    operationMask: operationMask,
                                    previewPNGData: previewPNGData,
                                    previewSize: previewSize)
    }

    func releaseDroppedFileAccess(_ accessID: UUID) {
        Task {
            try? await socket.send(ContentToBrowserMessage.releaseDroppedFileAccess(accessID: accessID).encode())
        }
    }

    func sendFilePromiseWriteResponse(requestID: UUID,
                                      promiseID: UUID,
                                      localPath: String,
                                      deleteWhenDone: Bool = true) {
        Task {
            try? await socket.send(ContentToBrowserMessage.filePromiseWriteResponse(
                requestID: requestID,
                promiseID: promiseID,
                success: true,
                localPath: localPath,
                deleteWhenDone: deleteWhenDone,
                errorMessage: nil
            ).encode())
        }
    }

    func sendFilePromiseWriteFailure(requestID: UUID,
                                     promiseID: UUID,
                                     errorMessage: String) {
        Task {
            try? await socket.send(ContentToBrowserMessage.filePromiseWriteResponse(
                requestID: requestID,
                promiseID: promiseID,
                success: false,
                localPath: nil,
                deleteWhenDone: false,
                errorMessage: errorMessage
            ).encode())
        }
    }

    // MARK: - Context URLs

    /// The full URL that was navigated to.
    func pluginURL() -> URL? {
        guard let urlString = _url else { return nil }
        return URL(string: urlString)
    }

    /// The security origin (scheme + host + port).
    func pluginOriginURL() -> URL? {
        guard let url = pluginURL(),
              var components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        components.path = ""
        components.query = nil
        components.fragment = nil
        return components.url
    }

    /// The directory containing the .outer file.
    func pluginBaseURL() -> URL? {
        pluginURL()?.deletingLastPathComponent()
    }

    /// The URL where the plugin bundle was downloaded from.
    func pluginBundleURL() -> URL? {
        guard let urlString = _bundleUrl else { return nil }
        return URL(string: urlString)
    }

    func currentHistoryEntryID() -> UUID? {
        _currentHistoryEntryID
    }

    func historyLength() -> UInt32 {
        _historyLength
    }

    func canGoBackInHistory() -> Bool {
        _canGoBack
    }

    func canGoForwardInHistory() -> Bool {
        _canGoForward
    }

    // MARK: - Network Proxy (stored separately, set by host before passing to plugin)

    private var _networkProxyHost: String?
    private var _networkProxyPort: UInt16 = 0
    private var _networkProxyUsername: String?
    private var _networkProxyPassword: String?

    func networkProxyConfiguration() -> (host: String, port: UInt16, username: String, password: String)? {
        guard let host = _networkProxyHost,
              let username = _networkProxyUsername,
              let password = _networkProxyPassword else {
            return nil
        }
        return (host, _networkProxyPort, username, password)
    }

    func applyProxy(to configuration: URLSessionConfiguration) {
        guard let proxy = networkProxyConfiguration(),
              !proxy.host.isEmpty,
              proxy.port != 0,
              let endpointPort = NWEndpoint.Port(rawValue: proxy.port) else {
            return
        }

        var socksProxy = ProxyConfiguration(socksv5Proxy: .hostPort(host: NWEndpoint.Host(proxy.host),
                                                                    port: endpointPort))
        socksProxy.applyCredential(username: proxy.username, password: proxy.password)
        socksProxy.allowFailover = false
        socksProxy.excludedDomains = []
        socksProxy.matchDomains = [""]
        configuration.proxyConfigurations = [socksProxy]
    }
}

/// Cursor types that plugins can request
enum PluginCursorType: Int {
    case arrow = 0
    case iBeam = 1
    case crosshair = 2
    case openHand = 3
    case closedHand = 4
    case pointingHand = 5
    case resizeLeft = 6
    case resizeRight = 7
    case resizeLeftRight = 8
    case resizeUp = 9
    case resizeDown = 10
    case resizeUpDown = 11
}

enum OuterframeHapticFeedbackStyle: Int {
    case generic = 0
    case alignment = 1
    case levelChange = 2
}

/// Input modes that plugins can request. Represented as a bitmask so modes can be combined.
struct OuterframeContentInputMode: OptionSet, Sendable {
    let rawValue: UInt8

    init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    static let textInput = OuterframeContentInputMode(rawValue: 1 << 0)   // Keyboard events interpreted as text
    static let rawKeys = OuterframeContentInputMode(rawValue: 1 << 1)     // Raw key events forwarded to the plugin
    static let none: OuterframeContentInputMode = []

    var allowsTextInput: Bool { contains(.textInput) }
    var allowsRawKeys: Bool { contains(.rawKeys) }
}
