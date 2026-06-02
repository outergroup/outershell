import Foundation
import AppKit

let OuterframeContentSocketHeaderLength = MemoryLayout<UInt32>.size
let OuterframeContentSocketMessageTypeLength = MemoryLayout<UInt16>.size

// MARK: - Content Messages (Browser ↔ Content)

struct InitializeContentProxy {
    var host: String
    var port: UInt16
    var username: String?
    var password: String?
}

struct InitializeContentArguments {
    var data: Data?
    var contentSize: CGSize?
    var appearance: NSAppearance?
    var proxy: InitializeContentProxy?
    var url: String?
    var bundleUrl: String?
    var windowIsActive: Bool?
    var historyEntryID: UUID?

    init(data: Data? = nil,
         contentSize: CGSize? = nil,
         appearance: NSAppearance? = nil,
         proxy: InitializeContentProxy? = nil,
         url: String? = nil,
         bundleUrl: String? = nil,
         windowIsActive: Bool? = nil,
         historyEntryID: UUID? = nil) {
        self.data = data
        self.contentSize = contentSize
        self.appearance = appearance
        self.proxy = proxy
        self.url = url
        self.bundleUrl = bundleUrl
        self.windowIsActive = windowIsActive
        self.historyEntryID = historyEntryID
    }
}

fileprivate enum InitArgKind: UInt8 {
    case data = 1
    case contentSize = 2
    case appearance = 3
    case proxy = 4
    case proxyAuth = 5
    case url = 6
    case bundleUrl = 7
    case windowIsActive = 8
    case historyEntryID = 9
}

/// Messages from Browser to Content on the content socket
enum BrowserToContentMessage {
    case initializeContent(args: InitializeContentArguments)
    case displayLinkFired(frameNumber: UInt64, targetTimestamp: Double)
    case displayLinkCallbackRegistered(callbackID: UUID, browserCallbackID: UUID)
    case resizeContent(size: CGSize)
    case mouseDown(point: CGPoint, modifierFlags: NSEvent.ModifierFlags, clickCount: Int)
    case mouseDragged(point: CGPoint, modifierFlags: NSEvent.ModifierFlags)
    case mouseUp(point: CGPoint, modifierFlags: NSEvent.ModifierFlags)
    case mouseMoved(point: CGPoint, modifierFlags: NSEvent.ModifierFlags)
    case rightMouseDown(point: CGPoint, modifierFlags: NSEvent.ModifierFlags, clickCount: Int)
    case rightMouseUp(point: CGPoint, modifierFlags: NSEvent.ModifierFlags)
    case scrollWheelEvent(point: CGPoint,
                          delta: CGPoint,
                          modifierFlags: NSEvent.ModifierFlags,
                          phase: NSEvent.Phase,
                          momentumPhase: NSEvent.Phase,
                          hasPreciseScrollingDeltas: Bool)
    case keyDown(keyCode: UInt16,
                 characters: String,
                 charactersIgnoringModifiers: String,
                 modifierFlags: NSEvent.ModifierFlags,
                 isARepeat: Bool)
    case keyUp(keyCode: UInt16,
               characters: String,
               charactersIgnoringModifiers: String,
               modifierFlags: NSEvent.ModifierFlags,
               isARepeat: Bool)
    case magnification(surfaceID: UInt32, magnification: CGFloat, location: CGPoint, scrollOffset: CGPoint)
    case magnificationEnded(surfaceID: UInt32, magnification: CGFloat, location: CGPoint, scrollOffset: CGPoint)
    case quickLook(point: CGPoint)
    case textInput(text: String,
                   hasReplacementRange: Bool,
                   replacementLocation: UInt64,
                   replacementLength: UInt64)
    case setMarkedText(text: String,
                       selectedLocation: UInt64,
                       selectedLength: UInt64,
                       hasReplacementRange: Bool,
                       replacementLocation: UInt64,
                       replacementLength: UInt64)
    case unmarkText
    case textInputFocus(fieldID: UUID, hasFocus: Bool)
    case textCommand(command: String)
    case setCursorPosition(fieldID: UUID, position: UInt64, modifySelection: Bool)
    case systemAppearanceUpdate(appearance: NSAppearance)
    case windowActiveUpdate(isActive: Bool)
    case viewFocusChanged(isFocused: Bool)
    case selectionToPasteboardCopyRequest(requestID: UUID)
    case selectionToPasteboardCutRequest(requestID: UUID)
    case pasteboardContentPasted(items: [OuterframeContentPasteboardItem])
    case pasteboardContentDropped(point: CGPoint, items: [OuterframeContentPasteboardItem])
    case pasteboardDropHitTestRequest(requestID: UUID,
                                      point: CGPoint,
                                      pasteboardTypes: [String],
                                      operationMask: UInt32,
                                      modifierFlags: UInt64)
    case pasteboardAccessResponse(requestID: UUID, granted: Bool, items: [OuterframeContentPasteboardItem])
    case filePromiseWriteRequest(requestID: UUID, promiseID: UUID)
    case accessibilitySnapshotRequest(requestID: UUID)
    case historyEntryAccepted(entryID: UUID, url: String)
    case historyEntryRejected(entryID: UUID, errorMessage: String)
    case historyTraversal(entryID: UUID, url: String)
    case historyContextUpdate(currentEntryID: UUID, url: String, length: UInt32, canGoBack: Bool, canGoForward: Bool)
    case contextMenuItemSelected(menuID: UUID, itemID: String)
    case shutdown

    func encode() throws -> Data {
        switch self {
        case .initializeContent(let arguments):
            var encodedArguments: [Data] = []

            if let data = arguments.data {
                var argPayload = OffsetPayloadBuilder(referenceBaseOffset: 0)
                argPayload.append(uint8: InitArgKind.data.rawValue)
                try argPayload.append(dataReference: data)
                encodedArguments.append(try argPayload.finalize())
            }

            if let contentSize = arguments.contentSize {
                var argPayload = Data(capacity: 1 + 16)
                argPayload.append(uint8: InitArgKind.contentSize.rawValue)
                argPayload.append(float64: contentSize.width)
                argPayload.append(float64: contentSize.height)
                encodedArguments.append(argPayload)
            }

            if let appearance = arguments.appearance {
                var argPayload = OffsetPayloadBuilder(referenceBaseOffset: 0)
                argPayload.append(uint8: InitArgKind.appearance.rawValue)
                let appearanceData = try NSKeyedArchiver.archivedData(withRootObject: appearance, requiringSecureCoding: true)
                try argPayload.append(dataReference: appearanceData)
                encodedArguments.append(try argPayload.finalize())
            }

            if let proxy = arguments.proxy {
                var argPayload = OffsetPayloadBuilder(referenceBaseOffset: 0)
                argPayload.append(uint8: InitArgKind.proxy.rawValue)
                try argPayload.append(stringReference: proxy.host)
                argPayload.append(uint16: proxy.port)
                encodedArguments.append(try argPayload.finalize())

                if proxy.username != nil || proxy.password != nil {
                    var authPayload = OffsetPayloadBuilder(referenceBaseOffset: 0)
                    authPayload.append(uint8: InitArgKind.proxyAuth.rawValue)
                    var flags: UInt8 = 0
                    if proxy.username != nil {
                        flags |= 1 << 0
                    }
                    if proxy.password != nil {
                        flags |= 1 << 1
                    }
                    authPayload.append(uint8: flags)
                    try authPayload.append(stringReference: proxy.username ?? "")
                    try authPayload.append(stringReference: proxy.password ?? "")
                    encodedArguments.append(try authPayload.finalize())
                }
            }

            if let url = arguments.url {
                var argPayload = OffsetPayloadBuilder(referenceBaseOffset: 0)
                argPayload.append(uint8: InitArgKind.url.rawValue)
                try argPayload.append(stringReference: url)
                encodedArguments.append(try argPayload.finalize())
            }

            if let bundleUrl = arguments.bundleUrl {
                var argPayload = OffsetPayloadBuilder(referenceBaseOffset: 0)
                argPayload.append(uint8: InitArgKind.bundleUrl.rawValue)
                try argPayload.append(stringReference: bundleUrl)
                encodedArguments.append(try argPayload.finalize())
            }

            if let windowIsActive = arguments.windowIsActive {
                var argPayload = Data(capacity: 2)
                argPayload.append(uint8: InitArgKind.windowIsActive.rawValue)
                argPayload.append(uint8: windowIsActive ? 1 << 0 : 0)
                encodedArguments.append(argPayload)
            }

            if let historyEntryID = arguments.historyEntryID {
                var argPayload = Data(capacity: 1 + 16)
                argPayload.append(uint8: InitArgKind.historyEntryID.rawValue)
                argPayload.append(uuid: historyEntryID)
                encodedArguments.append(argPayload)
            }

            var payload = OffsetPayloadBuilder()
            payload.append(uint16: UInt16(min(encodedArguments.count, Int(UInt16.max))))

            for encodedArgument in encodedArguments {
                try payload.append(dataReference: encodedArgument)
            }

            return makeBrowserToContentFrame(type: .initializeContent, payload: try payload.finalize())

        case .displayLinkFired(let frameNumber, let targetTimestamp):
            var payload = Data(capacity: 16)
            payload.append(uint64: frameNumber)
            payload.append(float64: targetTimestamp)
            return makeBrowserToContentFrame(type: .displayLinkFired, payload: payload)

        case .displayLinkCallbackRegistered(let callbackID, let browserCallbackID):
            var payload = Data(capacity: 16 * 2)
            payload.append(uuid: callbackID)
            payload.append(uuid: browserCallbackID)
            return makeBrowserToContentFrame(type: .displayLinkCallbackRegistered, payload: payload)

        case .resizeContent(let size):
            var payload = Data(capacity: 8 + 8)
            payload.append(float64: size.width)
            payload.append(float64: size.height)
            return makeBrowserToContentFrame(type: .resizeContent, payload: payload)

        case .mouseDown(let point, let modifierFlags, let clickCount):
            return makeMouseEventFrame(type: .mouseDown, point: point,
                                       modifierFlags: modifierFlags, clickCount: clickCount)

        case .mouseDragged(let point, let modifierFlags):
            return makeMouseEventFrame(type: .mouseDragged, point: point, modifierFlags: modifierFlags)

        case .mouseUp(let point, let modifierFlags):
            return makeMouseEventFrame(type: .mouseUp, point: point, modifierFlags: modifierFlags)

        case .mouseMoved(let point, let modifierFlags):
            return makeMouseEventFrame(type: .mouseMoved, point: point, modifierFlags: modifierFlags)

        case .rightMouseDown(let point, let modifierFlags, let clickCount):
            return makeMouseEventFrame(type: .rightMouseDown, point: point,
                                       modifierFlags: modifierFlags, clickCount: clickCount)

        case .rightMouseUp(let point, let modifierFlags):
            return makeMouseEventFrame(type: .rightMouseUp, point: point, modifierFlags: modifierFlags)

        case .scrollWheelEvent(let point,
                               let delta,
                               let modifierFlags,
                               let phase,
                               let momentumPhase,
                               let hasPreciseScrollingDeltas):
            var payload = Data(capacity: 8 * 4 + 8 + 4 + 4 + 1)
            payload.append(float64: point.x)
            payload.append(float64: point.y)
            payload.append(float64: delta.x)
            payload.append(float64: delta.y)
            payload.append(uint64: UInt64(modifierFlags.rawValue))
            payload.append(uint32: UInt32(truncatingIfNeeded: phase.rawValue))
            payload.append(uint32: UInt32(truncatingIfNeeded: momentumPhase.rawValue))
            var flags: UInt8 = 0
            if hasPreciseScrollingDeltas { flags |= 1 << 0 }
            payload.append(uint8: flags)
            return makeBrowserToContentFrame(type: .scrollWheelEvent, payload: payload)

        case .keyDown(let keyCode, let characters, let charactersIgnoringModifiers, let modifierFlags, let isARepeat):
            var payload = OffsetPayloadBuilder()
            payload.append(uint16: keyCode)
            try payload.append(stringReference: characters)
            try payload.append(stringReference: charactersIgnoringModifiers)
            payload.append(uint64: UInt64(modifierFlags.rawValue))
            payload.append(uint8: isARepeat ? 1 << 0 : 0)
            return makeBrowserToContentFrame(type: .keyDown, payload: try payload.finalize())

        case .keyUp(let keyCode, let characters, let charactersIgnoringModifiers, let modifierFlags, let isARepeat):
            var payload = OffsetPayloadBuilder()
            payload.append(uint16: keyCode)
            try payload.append(stringReference: characters)
            try payload.append(stringReference: charactersIgnoringModifiers)
            payload.append(uint64: UInt64(modifierFlags.rawValue))
            payload.append(uint8: isARepeat ? 1 << 0 : 0)
            return makeBrowserToContentFrame(type: .keyUp, payload: try payload.finalize())

        case .magnification(let surfaceID, let magnification, let location, let scrollOffset):
            var payload = Data()
            payload.append(uint32: surfaceID)
            payload.append(float64: magnification)
            payload.append(float64: location.x)
            payload.append(float64: location.y)
            payload.append(float64: scrollOffset.x)
            payload.append(float64: scrollOffset.y)
            return makeBrowserToContentFrame(type: .magnification, payload: payload)

        case .magnificationEnded(let surfaceID, let magnification, let location, let scrollOffset):
            var payload = Data()
            payload.append(uint32: surfaceID)
            payload.append(float64: magnification)
            payload.append(float64: location.x)
            payload.append(float64: location.y)
            payload.append(float64: scrollOffset.x)
            payload.append(float64: scrollOffset.y)
            return makeBrowserToContentFrame(type: .magnificationEnded, payload: payload)

        case .quickLook(let point):
            var payload = Data(capacity: 8 + 8)
            payload.append(float64: point.x)
            payload.append(float64: point.y)
            return makeBrowserToContentFrame(type: .quickLook, payload: payload)

        case .textInput(let text, let hasReplacementRange, let replacementLocation, let replacementLength):
            var payload = OffsetPayloadBuilder()
            try payload.append(stringReference: text)
            payload.append(uint8: hasReplacementRange ? 1 << 0 : 0)
            payload.append(uint64: replacementLocation)
            payload.append(uint64: replacementLength)
            return makeBrowserToContentFrame(type: .textInput, payload: try payload.finalize())

        case .setMarkedText(let text, let selectedLocation, let selectedLength, let hasReplacementRange, let replacementLocation, let replacementLength):
            var payload = OffsetPayloadBuilder()
            try payload.append(stringReference: text)
            payload.append(uint64: selectedLocation)
            payload.append(uint64: selectedLength)
            payload.append(uint8: hasReplacementRange ? 1 << 0 : 0)
            payload.append(uint64: replacementLocation)
            payload.append(uint64: replacementLength)
            return makeBrowserToContentFrame(type: .setMarkedText, payload: try payload.finalize())

        case .unmarkText:
            return makeBrowserToContentFrame(type: .unmarkText, payload: Data())

        case .textInputFocus(let fieldID, let hasFocus):
            var payload = Data()
            payload.append(uuid: fieldID)
            payload.append(uint8: hasFocus ? 1 << 0 : 0)
            return makeBrowserToContentFrame(type: .textInputFocus, payload: payload)

        case .textCommand(let command):
            var payload = OffsetPayloadBuilder()
            try payload.append(stringReference: command)
            return makeBrowserToContentFrame(type: .textCommand, payload: try payload.finalize())

        case .setCursorPosition(let fieldID, let position, let modifySelection):
            var payload = Data()
            payload.append(uuid: fieldID)
            payload.append(uint64: position)
            payload.append(uint8: modifySelection ? 1 << 0 : 0)
            return makeBrowserToContentFrame(type: .setCursorPosition, payload: payload)

        case .systemAppearanceUpdate(let appearance):
            var payload = OffsetPayloadBuilder()
            let appearanceData = try NSKeyedArchiver.archivedData(withRootObject: appearance, requiringSecureCoding: true)
            try payload.append(dataReference: appearanceData)
            return makeBrowserToContentFrame(type: .systemAppearanceUpdate, payload: try payload.finalize())

        case .windowActiveUpdate(let isActive):
            var payload = Data(capacity: 1)
            payload.append(uint8: isActive ? 1 << 0 : 0)
            return makeBrowserToContentFrame(type: .windowActiveUpdate, payload: payload)

        case .viewFocusChanged(let isFocused):
            var payload = Data(capacity: 1)
            payload.append(uint8: isFocused ? 1 << 0 : 0)
            return makeBrowserToContentFrame(type: .viewFocusChanged, payload: payload)

        case .selectionToPasteboardCopyRequest(let requestID):
            var payload = Data(capacity: 16)
            payload.append(uuid: requestID)
            return makeBrowserToContentFrame(type: .selectionToPasteboardCopyRequest, payload: payload)

        case .selectionToPasteboardCutRequest(let requestID):
            var payload = Data(capacity: 16)
            payload.append(uuid: requestID)
            return makeBrowserToContentFrame(type: .selectionToPasteboardCutRequest, payload: payload)

        case .pasteboardContentPasted(let items):
            let payload = try encodePasteboardItems(items)
            return makeBrowserToContentFrame(type: .pasteboardContentPasted, payload: payload)

        case .pasteboardContentDropped(let point, let items):
            var payload = OffsetPayloadBuilder()
            payload.append(float64: point.x)
            payload.append(float64: point.y)
            try appendPasteboardItems(items, to: &payload)
            return makeBrowserToContentFrame(type: .pasteboardContentDropped, payload: try payload.finalize())

        case .pasteboardDropHitTestRequest(let requestID, let point, let pasteboardTypes, let operationMask, let modifierFlags):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: requestID)
            payload.append(float64: point.x)
            payload.append(float64: point.y)
            payload.append(uint32: operationMask)
            payload.append(uint64: modifierFlags)
            let clampedCount = UInt16(min(pasteboardTypes.count, Int(UInt16.max)))
            payload.append(uint16: clampedCount)
            for identifier in pasteboardTypes.prefix(Int(clampedCount)) {
                try payload.append(stringReference: identifier)
            }
            return makeBrowserToContentFrame(type: .pasteboardDropHitTestRequest, payload: try payload.finalize())

        case .pasteboardAccessResponse(let requestID, let granted, let items):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: requestID)
            payload.append(uint8: granted ? 1 << 0 : 0)
            try appendPasteboardItems(items, to: &payload)
            return makeBrowserToContentFrame(type: .pasteboardAccessResponse, payload: try payload.finalize())

        case .filePromiseWriteRequest(let requestID, let promiseID):
            var payload = Data(capacity: 32)
            payload.append(uuid: requestID)
            payload.append(uuid: promiseID)
            return makeBrowserToContentFrame(type: .filePromiseWriteRequest, payload: payload)

        case .accessibilitySnapshotRequest(let requestID):
            var payload = Data(capacity: 16)
            payload.append(uuid: requestID)
            return makeBrowserToContentFrame(type: .accessibilitySnapshotRequest, payload: payload)

        case .historyEntryAccepted(let entryID, let url):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: entryID)
            try payload.append(stringReference: url)
            return makeBrowserToContentFrame(type: .historyEntryAccepted, payload: try payload.finalize())

        case .historyEntryRejected(let entryID, let errorMessage):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: entryID)
            try payload.append(stringReference: errorMessage)
            return makeBrowserToContentFrame(type: .historyEntryRejected, payload: try payload.finalize())

        case .historyTraversal(let entryID, let url):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: entryID)
            try payload.append(stringReference: url)
            return makeBrowserToContentFrame(type: .historyTraversal, payload: try payload.finalize())

        case .historyContextUpdate(let currentEntryID, let url, let length, let canGoBack, let canGoForward):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: currentEntryID)
            try payload.append(stringReference: url)
            payload.append(uint32: length)
            var flags: UInt8 = 0
            if canGoBack { flags |= 1 << 0 }
            if canGoForward { flags |= 1 << 1 }
            payload.append(uint8: flags)
            return makeBrowserToContentFrame(type: .historyContextUpdate, payload: try payload.finalize())

        case .contextMenuItemSelected(let menuID, let itemID):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: menuID)
            try payload.append(stringReference: itemID)
            return makeBrowserToContentFrame(type: .contextMenuItemSelected, payload: try payload.finalize())

        case .shutdown:
            return makeBrowserToContentFrame(type: .shutdown, payload: Data())
        }
    }

    static func decode(message: Data) throws -> BrowserToContentMessage {
        var cursor = DataCursor(message)
        guard let typeRaw = cursor.readUInt16() else {
            throw OuterframeContentSocketMessageError.truncatedPayload
        }
        guard let type = BrowserToContentMessageKind(rawValue: typeRaw) else {
            throw OuterframeContentSocketMessageError.unknownType(typeRaw)
        }

        switch type {
        case .initializeContent:
            guard let argCount = cursor.readUInt16() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }

            var arguments = InitializeContentArguments()
            var proxyUsername: String?
            var proxyPassword: String?

            for _ in 0..<argCount {
                guard let argData = cursor.readDataReference() else {
                    throw OuterframeContentSocketMessageError.truncatedPayload
                }

                var argCursor = DataCursor(argData)
                guard let kindRaw = argCursor.readUInt8() else {
                    throw OuterframeContentSocketMessageError.truncatedPayload
                }

                guard let kind = InitArgKind(rawValue: kindRaw) else {
                    continue
                }

                switch kind {
                case .data:
                    guard let data = argCursor.readDataReference() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.data = data

                case .contentSize:
                    guard let width = argCursor.readFloat64(),
                          let height = argCursor.readFloat64() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.contentSize = CGSize(width: width, height: height)

                case .appearance:
                    guard let appearanceData = argCursor.readDataReference(),
                          let decoded = try? NSKeyedUnarchiver.unarchivedObject(ofClass: NSAppearance.self, from: appearanceData) else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.appearance = decoded

                case .proxy:
                    guard let proxyHost = argCursor.readStringReference(),
                          let proxyPort = argCursor.readUInt16() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.proxy = InitializeContentProxy(host: proxyHost,
                                                             port: proxyPort,
                                                             username: proxyUsername,
                                                             password: proxyPassword)

                case .proxyAuth:
                    guard let flags = argCursor.readUInt8(),
                          let username = argCursor.readStringReference(),
                          let password = argCursor.readStringReference() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    if flags & (1 << 0) != 0 {
                        proxyUsername = username
                    } else {
                        proxyUsername = nil
                    }

                    if flags & (1 << 1) != 0 {
                        proxyPassword = password
                    } else {
                        proxyPassword = nil
                    }

                    if var proxy = arguments.proxy {
                        proxy.username = proxyUsername
                        proxy.password = proxyPassword
                        arguments.proxy = proxy
                    }

                case .url:
                    guard let url = argCursor.readStringReference() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.url = url

                case .bundleUrl:
                    guard let bundleUrl = argCursor.readStringReference() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.bundleUrl = bundleUrl

                case .windowIsActive:
                    guard let windowIsActiveRaw = argCursor.readUInt8() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.windowIsActive = windowIsActiveRaw & (1 << 0) != 0

                case .historyEntryID:
                    guard let historyEntryID = argCursor.readUUID() else {
                        throw OuterframeContentSocketMessageError.truncatedPayload
                    }
                    arguments.historyEntryID = historyEntryID
                }
            }

            return .initializeContent(args: arguments)

        case .displayLinkFired:
            guard let frameNumber = cursor.readUInt64(),
                  let timestampBits = cursor.readUInt64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let timestamp = Double(bitPattern: timestampBits)
            return .displayLinkFired(frameNumber: frameNumber, targetTimestamp: timestamp)

        case .displayLinkCallbackRegistered:
            guard let callbackID = cursor.readUUID(),
                  let browserCallbackID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .displayLinkCallbackRegistered(callbackID: callbackID, browserCallbackID: browserCallbackID)

        case .resizeContent:
            guard let width = cursor.readFloat64(),
                  let height = cursor.readFloat64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .resizeContent(size: CGSize(width: width, height: height))

        case .mouseDown:
            let event = try readMouseEvent(cursor: &cursor, includesClickCount: true)
            return .mouseDown(point: event.point,
                              modifierFlags: event.modifierFlags, clickCount: event.clickCount)

        case .mouseDragged:
            let event = try readMouseEvent(cursor: &cursor, includesClickCount: false)
            return .mouseDragged(point: event.point, modifierFlags: event.modifierFlags)

        case .mouseUp:
            let event = try readMouseEvent(cursor: &cursor, includesClickCount: false)
            return .mouseUp(point: event.point, modifierFlags: event.modifierFlags)

        case .mouseMoved:
            let event = try readMouseEvent(cursor: &cursor, includesClickCount: false)
            return .mouseMoved(point: event.point, modifierFlags: event.modifierFlags)

        case .rightMouseDown:
            let event = try readMouseEvent(cursor: &cursor, includesClickCount: true)
            return .rightMouseDown(point: event.point,
                                   modifierFlags: event.modifierFlags, clickCount: event.clickCount)

        case .rightMouseUp:
            let event = try readMouseEvent(cursor: &cursor, includesClickCount: false)
            return .rightMouseUp(point: event.point, modifierFlags: event.modifierFlags)

        case .scrollWheelEvent:
            guard let x = cursor.readFloat64(),
                  let y = cursor.readFloat64(),
                  let deltaX = cursor.readFloat64(),
                  let deltaY = cursor.readFloat64(),
                  let modifierFlags = cursor.readUInt64(),
                  let phaseRaw = cursor.readUInt32(),
                  let momentumPhaseRaw = cursor.readUInt32(),
                  let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .scrollWheelEvent(point: CGPoint(x: x, y: y),
                                     delta: CGPoint(x: deltaX, y: deltaY),
                                     modifierFlags: NSEvent.ModifierFlags(rawValue: UInt(modifierFlags)),
                                     phase: NSEvent.Phase(rawValue: UInt(phaseRaw)),
                                     momentumPhase: NSEvent.Phase(rawValue: UInt(momentumPhaseRaw)),
                                     hasPreciseScrollingDeltas: flags & (1 << 0) != 0)

        case .keyDown:
            guard let keyCode = cursor.readUInt16(),
                  let characters = cursor.readStringReference(),
                  let charactersIgnoringModifiers = cursor.readStringReference(),
                  let modifierFlags = cursor.readUInt64(),
                  let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .keyDown(keyCode: keyCode, characters: characters,
                            charactersIgnoringModifiers: charactersIgnoringModifiers,
                            modifierFlags: NSEvent.ModifierFlags(rawValue: UInt(modifierFlags)),
                            isARepeat: flags & (1 << 0) != 0)

        case .keyUp:
            guard let keyCode = cursor.readUInt16(),
                  let characters = cursor.readStringReference(),
                  let charactersIgnoringModifiers = cursor.readStringReference(),
                  let modifierFlags = cursor.readUInt64(),
                  let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .keyUp(keyCode: keyCode, characters: characters,
                          charactersIgnoringModifiers: charactersIgnoringModifiers,
                          modifierFlags: NSEvent.ModifierFlags(rawValue: UInt(modifierFlags)),
                          isARepeat: flags & (1 << 0) != 0)

        case .magnification:
            guard let surfaceID = cursor.readUInt32(),
                  let magnification = cursor.readFloat64(),
                  let x = cursor.readFloat64(),
                  let y = cursor.readFloat64(),
                  let scrollX = cursor.readFloat64(),
                  let scrollY = cursor.readFloat64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .magnification(surfaceID: surfaceID, magnification: magnification,
                                  location: CGPoint(x: x, y: y),
                                  scrollOffset: CGPoint(x: scrollX, y: scrollY))

        case .magnificationEnded:
            guard let surfaceID = cursor.readUInt32(),
                  let magnification = cursor.readFloat64(),
                  let x = cursor.readFloat64(),
                  let y = cursor.readFloat64(),
                  let scrollX = cursor.readFloat64(),
                  let scrollY = cursor.readFloat64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .magnificationEnded(surfaceID: surfaceID, magnification: magnification,
                                       location: CGPoint(x: x, y: y),
                                       scrollOffset: CGPoint(x: scrollX, y: scrollY))

        case .quickLook:
            guard let x = cursor.readFloat64(),
                  let y = cursor.readFloat64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .quickLook(point: CGPoint(x: x, y: y))

        case .textInput:
            guard let text = cursor.readStringReference(),
                  let flags = cursor.readUInt8(),
                  let replacementLocation = cursor.readUInt64(),
                  let replacementLength = cursor.readUInt64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .textInput(text: text, hasReplacementRange: flags & (1 << 0) != 0,
                              replacementLocation: replacementLocation,
                              replacementLength: replacementLength)

        case .setMarkedText:
            guard let text = cursor.readStringReference(),
                  let selectedLocation = cursor.readUInt64(),
                  let selectedLength = cursor.readUInt64(),
                  let flags = cursor.readUInt8(),
                  let replacementLocation = cursor.readUInt64(),
                  let replacementLength = cursor.readUInt64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .setMarkedText(text: text, selectedLocation: selectedLocation,
                                  selectedLength: selectedLength,
                                  hasReplacementRange: flags & (1 << 0) != 0,
                                  replacementLocation: replacementLocation,
                                  replacementLength: replacementLength)

        case .unmarkText:
            return .unmarkText

        case .textInputFocus:
            guard let fieldID = cursor.readUUID(),
                  let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .textInputFocus(fieldID: fieldID, hasFocus: flags & (1 << 0) != 0)

        case .textCommand:
            guard let command = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .textCommand(command: command)

        case .setCursorPosition:
            guard let fieldID = cursor.readUUID(),
                  let position = cursor.readUInt64(),
                  let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .setCursorPosition(fieldID: fieldID, position: position,
                                      modifySelection: flags & (1 << 0) != 0)

        case .systemAppearanceUpdate:
            guard let appearanceData = cursor.readDataReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let appearance = (try? NSKeyedUnarchiver.unarchivedObject(ofClass: NSAppearance.self, from: appearanceData))
                ?? NSAppearance.currentDrawing()
            return .systemAppearanceUpdate(appearance: appearance)

        case .windowActiveUpdate:
            guard let raw = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .windowActiveUpdate(isActive: raw & (1 << 0) != 0)

        case .viewFocusChanged:
            guard let raw = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .viewFocusChanged(isFocused: raw & (1 << 0) != 0)

        case .selectionToPasteboardCopyRequest:
            guard let requestID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .selectionToPasteboardCopyRequest(requestID: requestID)

        case .selectionToPasteboardCutRequest:
            guard let requestID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .selectionToPasteboardCutRequest(requestID: requestID)

        case .pasteboardContentPasted:
            let items = try readPasteboardItems(cursor: &cursor)
            return .pasteboardContentPasted(items: items)

        case .pasteboardContentDropped:
            guard let x = cursor.readFloat64(),
                  let y = cursor.readFloat64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let items = try readPasteboardItems(cursor: &cursor)
            return .pasteboardContentDropped(point: CGPoint(x: x, y: y), items: items)

        case .pasteboardDropHitTestRequest:
            guard let requestID = cursor.readUUID(),
                  let x = cursor.readFloat64(),
                  let y = cursor.readFloat64(),
                  let operationMask = cursor.readUInt32(),
                  let modifierFlags = cursor.readUInt64(),
                  let typeCount = cursor.readUInt16() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            var pasteboardTypes: [String] = []
            pasteboardTypes.reserveCapacity(Int(typeCount))
            for _ in 0..<typeCount {
                guard let identifier = cursor.readStringReference() else {
                    throw OuterframeContentSocketMessageError.truncatedPayload
                }
                pasteboardTypes.append(identifier)
            }
            return .pasteboardDropHitTestRequest(requestID: requestID,
                                                 point: CGPoint(x: x, y: y),
                                                 pasteboardTypes: pasteboardTypes,
                                                 operationMask: operationMask,
                                                 modifierFlags: modifierFlags)

        case .pasteboardAccessResponse:
            guard let requestID = cursor.readUUID(),
                  let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let items = try readPasteboardItems(cursor: &cursor)
            return .pasteboardAccessResponse(requestID: requestID,
                                             granted: flags & (1 << 0) != 0,
                                             items: items)

        case .filePromiseWriteRequest:
            guard let requestID = cursor.readUUID(),
                  let promiseID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .filePromiseWriteRequest(requestID: requestID, promiseID: promiseID)

        case .accessibilitySnapshotRequest:
            guard let requestID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .accessibilitySnapshotRequest(requestID: requestID)

        case .historyEntryAccepted:
            guard let entryID = cursor.readUUID(),
                  let url = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .historyEntryAccepted(entryID: entryID, url: url)

        case .historyEntryRejected:
            guard let entryID = cursor.readUUID(),
                  let errorMessage = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .historyEntryRejected(entryID: entryID, errorMessage: errorMessage)

        case .historyTraversal:
            guard let entryID = cursor.readUUID(),
                  let url = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .historyTraversal(entryID: entryID, url: url)

        case .historyContextUpdate:
            guard let currentEntryID = cursor.readUUID(),
                  let url = cursor.readStringReference(),
                  let length = cursor.readUInt32(),
                  let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .historyContextUpdate(currentEntryID: currentEntryID,
                                         url: url,
                                         length: length,
                                         canGoBack: flags & (1 << 0) != 0,
                                         canGoForward: flags & (1 << 1) != 0)

        case .contextMenuItemSelected:
            guard let menuID = cursor.readUUID(),
                  let itemID = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .contextMenuItemSelected(menuID: menuID, itemID: itemID)

        case .shutdown:
            return .shutdown
        }
    }
}

/// Messages from Content to Browser on the content socket
enum ContentToBrowserMessage {
    case startDisplayLink(callbackID: UUID)
    case stopDisplayLink(browserCallbackID: UUID)
    case cursorUpdate(cursorType: UInt8)
    case inputModeUpdate(inputMode: UInt8)
    case showContextMenu(attributedTextData: Data, locationX: CGFloat, locationY: CGFloat)
    case showContextMenuItems(menuID: UUID,
                              locationX: CGFloat,
                              locationY: CGFloat,
                              attributedTextData: Data?,
                              items: [OuterframeContextMenuItem])
    case showDefinition(attributedTextData: Data, locationX: CGFloat, locationY: CGFloat)
    case textCursorUpdate(cursors: [OuterframeContentTextCursorSnapshot])
    case selectionToPasteboardResponse(requestID: UUID, items: [OuterframeContentPasteboardItem])
    case pasteboardAccessRequest(requestID: UUID,
                                 operation: OuterframePasteboardAccessOperation,
                                 pasteboardTypes: [String],
                                 items: [OuterframeContentPasteboardItem])
    case beginDraggingPasteboardItems(items: [OuterframeContentDraggingItem], operationMask: UInt32)
    case releaseDroppedFileAccess(accessID: UUID)
    case filePromiseWriteResponse(requestID: UUID,
                                  promiseID: UUID,
                                  success: Bool,
                                  localPath: String?,
                                  deleteWhenDone: Bool,
                                  errorMessage: String?)
    case openNewWindow(url: String, displayString: String?, preferredSize: CGSize?)
    case navigate(url: String)
    case openNewTab(url: String, displayString: String?)
    case setEditingCapabilities(canCopy: Bool, canCut: Bool)
    case setPasteboardDropBehaviorUniform([String])
    case setAcceptedPasteboardPasteTypes([String])
    case setPasteboardDropBehaviorHitTest
    case pasteboardDropHitTestResponse(requestID: UUID, operationMask: UInt32)
    case accessibilitySnapshotResponse(requestID: UUID, snapshotData: Data?)
    case accessibilityTreeChanged(notificationMask: UInt8)
    case hapticFeedback(style: UInt8)
    case historyPushEntry(entryID: UUID, url: String?)
    case historyReplaceEntry(entryID: UUID, url: String?)
    case historyGo(delta: Int32)

    func encode() throws -> Data {
        switch self {
        case .startDisplayLink(let callbackID):
            var payload = Data(capacity: 16)
            payload.append(uuid: callbackID)
            return makeContentToBrowserFrame(type: .startDisplayLink, payload: payload)

        case .stopDisplayLink(let browserCallbackID):
            var payload = Data(capacity: 16)
            payload.append(uuid: browserCallbackID)
            return makeContentToBrowserFrame(type: .stopDisplayLink, payload: payload)

        case .cursorUpdate(let cursorType):
            var payload = Data(capacity: 1)
            payload.append(uint8: cursorType)
            return makeContentToBrowserFrame(type: .cursorUpdate, payload: payload)

        case .inputModeUpdate(let inputMode):
            var payload = Data(capacity: 1)
            payload.append(uint8: inputMode)
            return makeContentToBrowserFrame(type: .inputModeUpdate, payload: payload)

        case .showContextMenu(let attributedTextData, let locationX, let locationY):
            var payload = OffsetPayloadBuilder()
            payload.append(float64: locationX)
            payload.append(float64: locationY)
            try payload.append(dataReference: attributedTextData)
            return makeContentToBrowserFrame(type: .showContextMenu, payload: try payload.finalize())

        case .showContextMenuItems(let menuID, let locationX, let locationY, let attributedTextData, let items):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: menuID)
            payload.append(float64: locationX)
            payload.append(float64: locationY)
            var flags: UInt8 = 0
            if attributedTextData != nil { flags |= 1 << 0 }
            payload.append(uint8: flags)
            let clampedCount = UInt16(min(items.count, Int(UInt16.max)))
            payload.append(uint16: clampedCount)
            try payload.append(dataReference: attributedTextData ?? Data())
            for item in items.prefix(Int(clampedCount)) {
                try appendContextMenuItem(item, to: &payload)
            }
            return makeContentToBrowserFrame(type: .showContextMenuItems, payload: try payload.finalize())

        case .showDefinition(let attributedTextData, let locationX, let locationY):
            var payload = OffsetPayloadBuilder()
            payload.append(float64: locationX)
            payload.append(float64: locationY)
            try payload.append(dataReference: attributedTextData)
            return makeContentToBrowserFrame(type: .showDefinition, payload: try payload.finalize())

        case .textCursorUpdate(let cursors):
            var payload = Data()
            let countValue = UInt32(max(0, min(cursors.count, Int(UInt32.max))))
            payload.append(uint32: countValue)
            for cursor in cursors {
                payload.append(uuid: cursor.fieldID)
                payload.append(float64: cursor.rect.origin.x)
                payload.append(float64: cursor.rect.origin.y)
                payload.append(float64: cursor.rect.size.width)
                payload.append(float64: cursor.rect.size.height)
                payload.append(uint8: cursor.visible ? 1 << 0 : 0)
            }
            return makeContentToBrowserFrame(type: .textCursorUpdate, payload: payload)

        case .selectionToPasteboardResponse(let requestID, let items):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: requestID)
            try appendPasteboardItems(items, to: &payload)
            return makeContentToBrowserFrame(type: .selectionToPasteboardResponse, payload: try payload.finalize())

        case .pasteboardAccessRequest(let requestID, let operation, let pasteboardTypes, let items):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: requestID)
            payload.append(uint8: operation.rawValue)
            let clampedTypeCount = UInt16(min(pasteboardTypes.count, Int(UInt16.max)))
            let clampedItemCount = UInt16(min(items.count, Int(UInt16.max)))
            payload.append(uint16: clampedTypeCount)
            payload.append(uint16: clampedItemCount)
            for identifier in pasteboardTypes.prefix(Int(clampedTypeCount)) {
                try payload.append(stringReference: identifier)
            }
            try appendPasteboardItems(items.prefix(Int(clampedItemCount)), to: &payload, includeCount: false)
            return makeContentToBrowserFrame(type: .pasteboardAccessRequest, payload: try payload.finalize())

        case .beginDraggingPasteboardItems(let items, let operationMask):
            var payload = OffsetPayloadBuilder()
            payload.append(uint32: operationMask)
            try appendDraggingItems(items, to: &payload)
            return makeContentToBrowserFrame(type: .beginDraggingPasteboardItems, payload: try payload.finalize())

        case .releaseDroppedFileAccess(let accessID):
            var payload = Data(capacity: 16)
            payload.append(uuid: accessID)
            return makeContentToBrowserFrame(type: .releaseDroppedFileAccess, payload: payload)

        case .filePromiseWriteResponse(let requestID, let promiseID, let success, let localPath, let deleteWhenDone, let errorMessage):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: requestID)
            payload.append(uuid: promiseID)
            var flags: UInt8 = 0
            if success { flags |= 1 << 0 }
            if deleteWhenDone { flags |= 1 << 1 }
            payload.append(uint8: flags)
            try payload.append(stringReference: localPath ?? "")
            try payload.append(stringReference: errorMessage ?? "")
            return makeContentToBrowserFrame(type: .filePromiseWriteResponse, payload: try payload.finalize())

        case .openNewWindow(let url, let displayString, let preferredSize):
            var payload = OffsetPayloadBuilder()
            try payload.append(stringReference: url)
            var flags: UInt8 = 0
            if displayString != nil { flags |= 1 << 0 }
            if preferredSize != nil { flags |= 1 << 1 }
            payload.append(uint8: flags)
            try payload.append(stringReference: displayString ?? "")
            payload.append(float64: preferredSize.map { Float64($0.width) } ?? 0)
            payload.append(float64: preferredSize.map { Float64($0.height) } ?? 0)
            return makeContentToBrowserFrame(type: .openNewWindow, payload: try payload.finalize())

        case .navigate(let url):
            var payload = OffsetPayloadBuilder()
            try payload.append(stringReference: url)
            return makeContentToBrowserFrame(type: .navigate, payload: try payload.finalize())

        case .openNewTab(let url, let displayString):
            var payload = OffsetPayloadBuilder()
            try payload.append(stringReference: url)
            var flags: UInt8 = 0
            if displayString != nil { flags |= 1 << 0 }
            payload.append(uint8: flags)
            try payload.append(stringReference: displayString ?? "")
            return makeContentToBrowserFrame(type: .openNewTab, payload: try payload.finalize())

        case .setEditingCapabilities(let canCopy, let canCut):
            var payload = Data(capacity: 1)
            var flags: UInt8 = 0
            if canCopy { flags |= 1 << 0 }
            if canCut { flags |= 1 << 1 }
            payload.append(uint8: flags)
            return makeContentToBrowserFrame(type: .setEditingCapabilities, payload: payload)

        case .setPasteboardDropBehaviorUniform(let pasteboardTypes):
            var payload = OffsetPayloadBuilder()
            let clampedCount = UInt16(min(pasteboardTypes.count, Int(UInt16.max)))
            payload.append(uint16: clampedCount)
            for identifier in pasteboardTypes.prefix(Int(clampedCount)) {
                try payload.append(stringReference: identifier)
            }
            return makeContentToBrowserFrame(type: .setPasteboardDropBehaviorUniform, payload: try payload.finalize())

        case .setAcceptedPasteboardPasteTypes(let pasteboardTypes):
            var payload = OffsetPayloadBuilder()
            let clampedCount = UInt16(min(pasteboardTypes.count, Int(UInt16.max)))
            payload.append(uint16: clampedCount)
            for identifier in pasteboardTypes.prefix(Int(clampedCount)) {
                try payload.append(stringReference: identifier)
            }
            return makeContentToBrowserFrame(type: .setAcceptedPasteboardPasteTypes, payload: try payload.finalize())

        case .setPasteboardDropBehaviorHitTest:
            return makeContentToBrowserFrame(type: .setPasteboardDropBehaviorHitTest, payload: Data())

        case .pasteboardDropHitTestResponse(let requestID, let operationMask):
            var payload = Data(capacity: 20)
            payload.append(uuid: requestID)
            payload.append(uint32: operationMask)
            return makeContentToBrowserFrame(type: .pasteboardDropHitTestResponse, payload: payload)

        case .accessibilitySnapshotResponse(let requestID, let snapshotData):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: requestID)
            payload.append(uint8: snapshotData != nil ? 1 << 0 : 0)
            try payload.append(dataReference: snapshotData ?? Data())
            return makeContentToBrowserFrame(type: .accessibilitySnapshotResponse, payload: try payload.finalize())

        case .accessibilityTreeChanged(let notificationMask):
            var payload = Data(capacity: 1)
            payload.append(uint8: notificationMask)
            return makeContentToBrowserFrame(type: .accessibilityTreeChanged, payload: payload)

        case .hapticFeedback(let style):
            var payload = Data(capacity: 1)
            payload.append(uint8: style)
            return makeContentToBrowserFrame(type: .hapticFeedback, payload: payload)

        case .historyPushEntry(let entryID, let url):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: entryID)
            payload.append(uint8: url != nil ? 1 << 0 : 0)
            try payload.append(stringReference: url ?? "")
            return makeContentToBrowserFrame(type: .historyPushEntry, payload: try payload.finalize())

        case .historyReplaceEntry(let entryID, let url):
            var payload = OffsetPayloadBuilder()
            payload.append(uuid: entryID)
            payload.append(uint8: url != nil ? 1 << 0 : 0)
            try payload.append(stringReference: url ?? "")
            return makeContentToBrowserFrame(type: .historyReplaceEntry, payload: try payload.finalize())

        case .historyGo(let delta):
            var payload = Data(capacity: 4)
            payload.append(int32: delta)
            return makeContentToBrowserFrame(type: .historyGo, payload: payload)
        }
    }

    static func decode(message: Data) throws -> ContentToBrowserMessage {
        var cursor = DataCursor(message)
        guard let typeRaw = cursor.readUInt16() else {
            throw OuterframeContentSocketMessageError.truncatedPayload
        }
        guard let type = ContentToBrowserMessageKind(rawValue: typeRaw) else {
            throw OuterframeContentSocketMessageError.unknownType(typeRaw)
        }

        switch type {
        case .startDisplayLink:
            guard let callbackID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .startDisplayLink(callbackID: callbackID)

        case .stopDisplayLink:
            guard let browserCallbackID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .stopDisplayLink(browserCallbackID: browserCallbackID)

        case .cursorUpdate:
            guard let cursorType = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .cursorUpdate(cursorType: cursorType)

        case .inputModeUpdate:
            guard let inputMode = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .inputModeUpdate(inputMode: inputMode)

        case .showContextMenu:
            guard let locationX = cursor.readFloat64(),
                  let locationY = cursor.readFloat64(),
                  let attributedTextData = cursor.readDataReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .showContextMenu(attributedTextData: attributedTextData,
                                    locationX: locationX, locationY: locationY)

        case .showContextMenuItems:
            guard let menuID = cursor.readUUID(),
                  let locationX = cursor.readFloat64(),
                  let locationY = cursor.readFloat64(),
                  let flags = cursor.readUInt8(),
                  let count = cursor.readUInt16(),
                  let attributedTextData = cursor.readDataReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            var items: [OuterframeContextMenuItem] = []
            items.reserveCapacity(Int(count))
            for _ in 0..<count {
                items.append(try readContextMenuItem(from: &cursor))
            }
            return .showContextMenuItems(menuID: menuID,
                                         locationX: locationX,
                                         locationY: locationY,
                                         attributedTextData: flags & (1 << 0) != 0 ? attributedTextData : nil,
                                         items: items)

        case .showDefinition:
            guard let locationX = cursor.readFloat64(),
                  let locationY = cursor.readFloat64(),
                  let attributedTextData = cursor.readDataReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .showDefinition(attributedTextData: attributedTextData,
                                   locationX: locationX, locationY: locationY)

        case .textCursorUpdate:
            guard let cursorCount = cursor.readUInt32() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            var entries: [OuterframeContentTextCursorSnapshot] = []
            entries.reserveCapacity(Int(cursorCount))
            for _ in 0..<cursorCount {
                guard let fieldID = cursor.readUUID(),
                      let rectX = cursor.readFloat64(),
                      let rectY = cursor.readFloat64(),
                      let rectWidth = cursor.readFloat64(),
                      let rectHeight = cursor.readFloat64(),
                      let flags = cursor.readUInt8() else {
                    throw OuterframeContentSocketMessageError.truncatedPayload
                }
                entries.append(OuterframeContentTextCursorSnapshot(fieldID: fieldID,
                                                                   rect: CGRect(x: rectX,
                                                                                y: rectY,
                                                                                width: rectWidth,
                                                                                height: rectHeight),
                                                                   visible: flags & (1 << 0) != 0))
            }
            return .textCursorUpdate(cursors: entries)

        case .selectionToPasteboardResponse:
            guard let requestID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let items = try readPasteboardItems(cursor: &cursor)
            return .selectionToPasteboardResponse(requestID: requestID, items: items)

        case .pasteboardAccessRequest:
            guard let requestID = cursor.readUUID(),
                  let operationRaw = cursor.readUInt8(),
                  let typeCount = cursor.readUInt16(),
                  let itemCount = cursor.readUInt16(),
                  let operation = OuterframePasteboardAccessOperation(rawValue: operationRaw) else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            var pasteboardTypes: [String] = []
            pasteboardTypes.reserveCapacity(Int(typeCount))
            for _ in 0..<typeCount {
                guard let identifier = cursor.readStringReference() else {
                    throw OuterframeContentSocketMessageError.truncatedPayload
                }
                pasteboardTypes.append(identifier)
            }
            let items = try readPasteboardItems(cursor: &cursor, count: itemCount)
            return .pasteboardAccessRequest(requestID: requestID,
                                            operation: operation,
                                            pasteboardTypes: pasteboardTypes,
                                            items: items)

        case .beginDraggingPasteboardItems:
            guard let operationMask = cursor.readUInt32() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let items = try readDraggingItems(cursor: &cursor)
            return .beginDraggingPasteboardItems(items: items, operationMask: operationMask)

        case .releaseDroppedFileAccess:
            guard let accessID = cursor.readUUID() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .releaseDroppedFileAccess(accessID: accessID)

        case .filePromiseWriteResponse:
            guard let requestID = cursor.readUUID(),
                  let promiseID = cursor.readUUID(),
                  let flags = cursor.readUInt8(),
                  let localPath = cursor.readStringReference(),
                  let errorMessage = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let success = flags & (1 << 0) != 0
            return .filePromiseWriteResponse(requestID: requestID,
                                             promiseID: promiseID,
                                             success: success,
                                             localPath: localPath.isEmpty ? nil : localPath,
                                             deleteWhenDone: flags & (1 << 1) != 0,
                                             errorMessage: errorMessage.isEmpty ? nil : errorMessage)

        case .setEditingCapabilities:
            guard let flags = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .setEditingCapabilities(canCopy: flags & (1 << 0) != 0,
                                           canCut: flags & (1 << 1) != 0)

        case .setPasteboardDropBehaviorUniform:
            guard let count = cursor.readUInt16() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            var identifiers: [String] = []
            identifiers.reserveCapacity(Int(count))
            for _ in 0..<count {
                guard let identifier = cursor.readStringReference() else {
                    throw OuterframeContentSocketMessageError.truncatedPayload
                }
                identifiers.append(identifier)
            }
            return .setPasteboardDropBehaviorUniform(identifiers)

        case .setAcceptedPasteboardPasteTypes:
            guard let count = cursor.readUInt16() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            var identifiers: [String] = []
            identifiers.reserveCapacity(Int(count))
            for _ in 0..<count {
                guard let identifier = cursor.readStringReference() else {
                    throw OuterframeContentSocketMessageError.truncatedPayload
                }
                identifiers.append(identifier)
            }
            return .setAcceptedPasteboardPasteTypes(identifiers)

        case .setPasteboardDropBehaviorHitTest:
            return .setPasteboardDropBehaviorHitTest

        case .pasteboardDropHitTestResponse:
            guard let requestID = cursor.readUUID(),
                  let operationMask = cursor.readUInt32() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .pasteboardDropHitTestResponse(requestID: requestID, operationMask: operationMask)

        case .accessibilitySnapshotResponse:
            guard let requestID = cursor.readUUID(),
                  let flags = cursor.readUInt8(),
                  let payload = cursor.readDataReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let snapshotData = flags & (1 << 0) != 0 ? payload : nil
            return .accessibilitySnapshotResponse(requestID: requestID, snapshotData: snapshotData)

        case .accessibilityTreeChanged:
            guard let mask = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .accessibilityTreeChanged(notificationMask: mask)

        case .hapticFeedback:
            guard let style = cursor.readUInt8() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .hapticFeedback(style: style)

        case .historyPushEntry:
            guard let entryID = cursor.readUUID(),
                  let flags = cursor.readUInt8(),
                  let urlReference = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let url = flags & (1 << 0) != 0 ? urlReference : nil
            return .historyPushEntry(entryID: entryID, url: url)

        case .historyReplaceEntry:
            guard let entryID = cursor.readUUID(),
                  let flags = cursor.readUInt8(),
                  let urlReference = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let url = flags & (1 << 0) != 0 ? urlReference : nil
            return .historyReplaceEntry(entryID: entryID, url: url)

        case .historyGo:
            guard let delta = cursor.readInt32() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .historyGo(delta: delta)

        case .openNewWindow:
            guard let url = cursor.readStringReference(),
                  let flags = cursor.readUInt8(),
                  let displayStringReference = cursor.readStringReference(),
                  let width = cursor.readFloat64(),
                  let height = cursor.readFloat64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let displayString = flags & (1 << 0) != 0 ? displayStringReference : nil
            let preferredSize = flags & (1 << 1) != 0 ? CGSize(width: width, height: height) : nil
            return .openNewWindow(url: url, displayString: displayString,
                                  preferredSize: preferredSize)

        case .navigate:
            guard let url = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            return .navigate(url: url)

        case .openNewTab:
            guard let url = cursor.readStringReference(),
                  let flags = cursor.readUInt8(),
                  let displayStringReference = cursor.readStringReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            let displayString = flags & (1 << 0) != 0 ? displayStringReference : nil
            return .openNewTab(url: url, displayString: displayString)
        }
    }
}

// MARK: - Supporting Types

struct OuterframeContentTextCursorSnapshot: Sendable {
    let fieldID: UUID
    let rect: CGRect
    let visible: Bool
}

struct OuterframeContentPasteboardRepresentation: Sendable {
    let typeIdentifier: String
    let data: Data

    init(typeIdentifier: String, data: Data) {
        self.typeIdentifier = typeIdentifier
        self.data = data
    }
}

struct OuterframeContentPasteboardItem: Sendable {
    let representations: [OuterframeContentPasteboardRepresentation]

    init(representations: [OuterframeContentPasteboardRepresentation]) {
        self.representations = representations
    }
}

struct OuterframeContentDraggingItem: Sendable {
    let pasteboardItem: OuterframeContentPasteboardItem
    let previewImageData: Data?
    let previewSize: CGSize?
    let previewFrameOrigin: CGPoint?

    init(pasteboardItem: OuterframeContentPasteboardItem,
         previewImageData: Data? = nil,
         previewSize: CGSize? = nil,
         previewFrameOrigin: CGPoint? = nil) {
        self.pasteboardItem = pasteboardItem
        self.previewImageData = previewImageData
        self.previewSize = previewSize
        self.previewFrameOrigin = previewFrameOrigin
    }
}

typealias OuterContentPasteboardItem = OuterframeContentPasteboardItem
typealias OuterContentPasteboardRepresentation = OuterframeContentPasteboardRepresentation
typealias OuterContentDraggingItem = OuterframeContentDraggingItem
typealias OuterContentTextCursorSnapshot = OuterframeContentTextCursorSnapshot

public enum OuterframePasteboardAccessOperation: UInt8, Sendable {
    case read = 0
    case write = 1
}

public enum OuterframeContextMenuItemAction: UInt8, Sendable {
    case contentCommand = 0
    case standardCopy = 1
    case standardPaste = 2
    case standardCut = 3
    case standardSelectAll = 4
    case standardLookUp = 5
    case standardServices = 6
}

public enum OuterframeContextMenuItemKind: UInt8, Sendable {
    case command = 0
    case separator = 1
    case submenu = 2
    case label = 3
}

public enum OuterframeContextMenuItemState: UInt8, Sendable {
    case off = 0
    case on = 1
    case mixed = 2
}

public enum OuterframeContextMenuTextAlignment: UInt8, Sendable {
    case natural = 0
    case left = 1
    case center = 2
    case right = 3
}

public struct OuterframeContextMenuItemStyle: Sendable {
    public var height: Float32
    public var topInset: Float32
    public var leftInset: Float32
    public var bottomInset: Float32
    public var rightInset: Float32
    public var fontSize: Float32
    public var fontWeight: Float32
    public var textColorRGBA: UInt32
    public var alignment: OuterframeContextMenuTextAlignment

    public init(height: Float32 = 0,
                topInset: Float32 = 0,
                leftInset: Float32 = 0,
                bottomInset: Float32 = 0,
                rightInset: Float32 = 0,
                fontSize: Float32 = 0,
                fontWeight: Float32 = 0,
                textColorRGBA: UInt32 = 0,
                alignment: OuterframeContextMenuTextAlignment = .natural) {
        self.height = height
        self.topInset = topInset
        self.leftInset = leftInset
        self.bottomInset = bottomInset
        self.rightInset = rightInset
        self.fontSize = fontSize
        self.fontWeight = fontWeight
        self.textColorRGBA = textColorRGBA
        self.alignment = alignment
    }
}

public struct OuterframeContextMenuItem: Sendable {
    public let id: String
    public let title: String
    public let kind: OuterframeContextMenuItemKind
    public let action: OuterframeContextMenuItemAction
    public let isEnabled: Bool
    public let state: OuterframeContextMenuItemState
    public let indentationLevel: UInt16
    public let keyEquivalent: String
    public let keyEquivalentModifierMask: UInt32
    public let systemImageName: String
    public let style: OuterframeContextMenuItemStyle
    public let children: [OuterframeContextMenuItem]

    public init(id: String,
                title: String,
                kind: OuterframeContextMenuItemKind = .command,
                action: OuterframeContextMenuItemAction = .contentCommand,
                isEnabled: Bool = true,
                state: OuterframeContextMenuItemState = .off,
                indentationLevel: UInt16 = 0,
                keyEquivalent: String = "",
                keyEquivalentModifierMask: UInt32 = 0,
                systemImageName: String = "",
                style: OuterframeContextMenuItemStyle = OuterframeContextMenuItemStyle(),
                children: [OuterframeContextMenuItem] = []) {
        self.id = id
        self.title = title
        self.kind = kind
        self.action = action
        self.isEnabled = isEnabled
        self.state = state
        self.indentationLevel = indentationLevel
        self.keyEquivalent = keyEquivalent
        self.keyEquivalentModifierMask = keyEquivalentModifierMask
        self.systemImageName = systemImageName
        self.style = style
        self.children = children
    }
}

enum OuterframeContentSocketMessageError: Error {
    case unknownType(UInt16)
    case truncatedPayload
    case encodingFailure(String)
}

// MARK: - Message Kind Enums

private enum BrowserToContentMessageKind: UInt16 {
    case initializeContent = 1000
    case resizeContent = 1001
    case shutdown = 1002
    case displayLinkFired = 1003
    case displayLinkCallbackRegistered = 1004
    case systemAppearanceUpdate = 1005
    case windowActiveUpdate = 1006
    case viewFocusChanged = 1007
    case mouseDown = 1008
    case mouseDragged = 1009
    case mouseUp = 1010
    case mouseMoved = 1011
    case rightMouseDown = 1012
    case rightMouseUp = 1013
    case scrollWheelEvent = 1014
    case keyDown = 1015
    case keyUp = 1016
    case magnification = 1017
    case magnificationEnded = 1018
    case quickLook = 1019
    case textInput = 1020
    case setMarkedText = 1021
    case unmarkText = 1022
    case textInputFocus = 1023
    case textCommand = 1024
    case setCursorPosition = 1025
    case selectionToPasteboardCopyRequest = 1026
    case pasteboardContentPasted = 1027
    case accessibilitySnapshotRequest = 1028
    case historyEntryAccepted = 1029
    case historyEntryRejected = 1030
    case historyTraversal = 1031
    case historyContextUpdate = 1032
    case contextMenuItemSelected = 1033
    case pasteboardAccessResponse = 1034
    case pasteboardContentDropped = 1035
    case selectionToPasteboardCutRequest = 1037
    case pasteboardDropHitTestRequest = 1038
    case filePromiseWriteRequest = 1039

    // Assign new indices in contiguous blocks to make the switch statement more efficient
}

private enum ContentToBrowserMessageKind: UInt16 {
    case startDisplayLink = 2000
    case stopDisplayLink = 2001
    case cursorUpdate = 2002
    case inputModeUpdate = 2003
    case textCursorUpdate = 2004
    case showContextMenu = 2005
    case showDefinition = 2006
    case hapticFeedback = 2007
    case selectionToPasteboardResponse = 2008
    case setEditingCapabilities = 2009
    case accessibilitySnapshotResponse = 2010
    case accessibilityTreeChanged = 2011
    case openNewWindow = 2012
    case historyPushEntry = 2013
    case historyReplaceEntry = 2014
    case historyGo = 2015
    case showContextMenuItems = 2016
    case pasteboardAccessRequest = 2017
    case beginDraggingPasteboardItems = 2018
    case setPasteboardDropBehaviorUniform = 2021
    case setAcceptedPasteboardPasteTypes = 2022
    case pasteboardDropHitTestResponse = 2023
    case setPasteboardDropBehaviorHitTest = 2024
    case releaseDroppedFileAccess = 2026
    case filePromiseWriteResponse = 2027
    case navigate = 2028
    case openNewTab = 2029

    // Assign new indices in contiguous blocks to make the switch statement more efficient
}

// MARK: - Frame Helpers

private func makeBrowserToContentFrame(type: BrowserToContentMessageKind, payload: Data) -> Data {
    let messageLength = OuterframeContentSocketMessageTypeLength + payload.count
    var frame = Data(capacity: OuterframeContentSocketHeaderLength + messageLength)
    frame.append(uint32: UInt32(messageLength))
    frame.append(uint16: type.rawValue)
    frame.append(payload)
    return frame
}

private func makeContentToBrowserFrame(type: ContentToBrowserMessageKind, payload: Data) -> Data {
    let messageLength = OuterframeContentSocketMessageTypeLength + payload.count
    var frame = Data(capacity: OuterframeContentSocketHeaderLength + messageLength)
    frame.append(uint32: UInt32(messageLength))
    frame.append(uint16: type.rawValue)
    frame.append(payload)
    return frame
}

private func appendContextMenuItem(_ item: OuterframeContextMenuItem,
                                   to payload: inout OffsetPayloadBuilder) throws {
    payload.append(uint8: item.kind.rawValue)
    payload.append(uint8: item.action.rawValue)
    payload.append(uint8: item.isEnabled ? 1 : 0)
    payload.append(uint8: item.state.rawValue)
    payload.append(uint16: item.indentationLevel)
    payload.append(uint16: UInt16(min(item.children.count, Int(UInt16.max))))
    payload.append(uint32: item.keyEquivalentModifierMask)
    payload.append(float32: item.style.height)
    payload.append(float32: item.style.topInset)
    payload.append(float32: item.style.leftInset)
    payload.append(float32: item.style.bottomInset)
    payload.append(float32: item.style.rightInset)
    payload.append(float32: item.style.fontSize)
    payload.append(float32: item.style.fontWeight)
    payload.append(uint32: item.style.textColorRGBA)
    payload.append(uint8: item.style.alignment.rawValue)
    payload.append(uint8: 0)
    payload.append(uint8: 0)
    payload.append(uint8: 0)
    try payload.append(stringReference: item.id)
    try payload.append(stringReference: item.title)
    try payload.append(stringReference: item.keyEquivalent)
    try payload.append(stringReference: item.systemImageName)
    for child in item.children.prefix(Int(UInt16.max)) {
        try appendContextMenuItem(child, to: &payload)
    }
}

private func readContextMenuItem(from cursor: inout DataCursor) throws -> OuterframeContextMenuItem {
    guard let kindRawValue = cursor.readUInt8(),
          let actionRawValue = cursor.readUInt8(),
          let enabledRawValue = cursor.readUInt8(),
          let stateRawValue = cursor.readUInt8(),
          let indentationLevel = cursor.readUInt16(),
          let childCount = cursor.readUInt16(),
          let keyEquivalentModifierMask = cursor.readUInt32(),
          let height = cursor.readFloat32(),
          let topInset = cursor.readFloat32(),
          let leftInset = cursor.readFloat32(),
          let bottomInset = cursor.readFloat32(),
          let rightInset = cursor.readFloat32(),
          let fontSize = cursor.readFloat32(),
          let fontWeight = cursor.readFloat32(),
          let textColorRGBA = cursor.readUInt32(),
          let alignmentRawValue = cursor.readUInt8(),
          cursor.readUInt8() != nil,
          cursor.readUInt8() != nil,
          cursor.readUInt8() != nil,
          let id = cursor.readStringReference(),
          let title = cursor.readStringReference(),
          let keyEquivalent = cursor.readStringReference(),
          let systemImageName = cursor.readStringReference() else {
        throw OuterframeContentSocketMessageError.truncatedPayload
    }

    var children: [OuterframeContextMenuItem] = []
    children.reserveCapacity(Int(childCount))
    for _ in 0..<childCount {
        children.append(try readContextMenuItem(from: &cursor))
    }

    let style = OuterframeContextMenuItemStyle(height: height,
                                               topInset: topInset,
                                               leftInset: leftInset,
                                               bottomInset: bottomInset,
                                               rightInset: rightInset,
                                               fontSize: fontSize,
                                               fontWeight: fontWeight,
                                               textColorRGBA: textColorRGBA,
                                               alignment: OuterframeContextMenuTextAlignment(rawValue: alignmentRawValue) ?? .natural)
    return OuterframeContextMenuItem(id: id,
                                     title: title,
                                     kind: OuterframeContextMenuItemKind(rawValue: kindRawValue) ?? .command,
                                     action: OuterframeContextMenuItemAction(rawValue: actionRawValue) ?? .contentCommand,
                                     isEnabled: enabledRawValue != 0,
                                     state: OuterframeContextMenuItemState(rawValue: stateRawValue) ?? .off,
                                     indentationLevel: indentationLevel,
                                     keyEquivalent: keyEquivalent,
                                     keyEquivalentModifierMask: keyEquivalentModifierMask,
                                     systemImageName: systemImageName,
                                     style: style,
                                     children: children)
}

private func appendPasteboardItems<S: Sequence>(_ items: S,
                                                to payload: inout OffsetPayloadBuilder,
                                                includeCount: Bool = true) throws where S.Element == OuterframeContentPasteboardItem {
    let items = Array(items)
    let clampedCount = UInt16(min(items.count, Int(UInt16.max)))
    if includeCount {
        payload.append(uint16: clampedCount)
    }
    for item in items.prefix(Int(clampedCount)) {
        let clampedRepresentationCount = UInt16(min(item.representations.count, Int(UInt16.max)))
        payload.append(uint16: clampedRepresentationCount)
        for representation in item.representations.prefix(Int(clampedRepresentationCount)) {
            try payload.append(stringReference: representation.typeIdentifier)
            try payload.append(dataReference: representation.data)
        }
    }
}

private func encodePasteboardItems(_ items: [OuterframeContentPasteboardItem]) throws -> Data {
    var payload = OffsetPayloadBuilder()
    try appendPasteboardItems(items, to: &payload)
    return try payload.finalize()
}

private func appendDraggingItems(_ items: [OuterframeContentDraggingItem],
                                 to payload: inout OffsetPayloadBuilder) throws {
    let clampedCount = UInt16(min(items.count, Int(UInt16.max)))
    payload.append(uint16: clampedCount)
    for item in items.prefix(Int(clampedCount)) {
        let clampedRepresentationCount = UInt16(min(item.pasteboardItem.representations.count, Int(UInt16.max)))
        payload.append(uint16: clampedRepresentationCount)
        for representation in item.pasteboardItem.representations.prefix(Int(clampedRepresentationCount)) {
            try payload.append(stringReference: representation.typeIdentifier)
            try payload.append(dataReference: representation.data)
        }
        let hasPreview = item.previewImageData != nil
        let hasPreviewFrameOrigin = item.previewFrameOrigin != nil
        payload.append(uint8: (hasPreview ? 1 << 0 : 0) | (hasPreviewFrameOrigin ? 1 << 1 : 0))
        try payload.append(dataReference: item.previewImageData ?? Data())
        payload.append(float64: item.previewSize.map { Double($0.width) } ?? 0)
        payload.append(float64: item.previewSize.map { Double($0.height) } ?? 0)
        if let previewFrameOrigin = item.previewFrameOrigin {
            payload.append(float64: Double(previewFrameOrigin.x))
            payload.append(float64: Double(previewFrameOrigin.y))
        }
    }
}

private func readPasteboardItems(cursor: inout DataCursor,
                                 count providedCount: UInt16? = nil) throws -> [OuterframeContentPasteboardItem] {
    let count: UInt16
    if let providedCount {
        count = providedCount
    } else {
        guard let readCount = cursor.readUInt16() else {
            throw OuterframeContentSocketMessageError.truncatedPayload
        }
        count = readCount
    }
    var items: [OuterframeContentPasteboardItem] = []
    items.reserveCapacity(Int(count))
    for _ in 0..<count {
        guard let representationCount = cursor.readUInt16() else {
            throw OuterframeContentSocketMessageError.truncatedPayload
        }
        var representations: [OuterframeContentPasteboardRepresentation] = []
        representations.reserveCapacity(Int(representationCount))
        for _ in 0..<representationCount {
            guard let identifier = cursor.readStringReference(),
                  let data = cursor.readDataReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            representations.append(OuterframeContentPasteboardRepresentation(typeIdentifier: identifier, data: data))
        }
        items.append(OuterframeContentPasteboardItem(representations: representations))
    }
    return items
}

private func readDraggingItems(cursor: inout DataCursor) throws -> [OuterframeContentDraggingItem] {
    guard let count = cursor.readUInt16() else {
        throw OuterframeContentSocketMessageError.truncatedPayload
    }
    var items: [OuterframeContentDraggingItem] = []
    items.reserveCapacity(Int(count))
    for _ in 0..<count {
        guard let representationCount = cursor.readUInt16() else {
            throw OuterframeContentSocketMessageError.truncatedPayload
        }
        var representations: [OuterframeContentPasteboardRepresentation] = []
        representations.reserveCapacity(Int(representationCount))
        for _ in 0..<representationCount {
            guard let identifier = cursor.readStringReference(),
                  let data = cursor.readDataReference() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            representations.append(OuterframeContentPasteboardRepresentation(typeIdentifier: identifier, data: data))
        }
        guard
              let flags = cursor.readUInt8(),
              let previewImageData = cursor.readDataReference(),
              let previewWidth = cursor.readFloat64(),
              let previewHeight = cursor.readFloat64() else {
            throw OuterframeContentSocketMessageError.truncatedPayload
        }
        let hasPreview = flags & (1 << 0) != 0
        let hasPreviewFrameOrigin = flags & (1 << 1) != 0
        let previewSize: CGSize?
        if hasPreview, previewWidth > 0, previewHeight > 0 {
            previewSize = CGSize(width: previewWidth, height: previewHeight)
        } else {
            previewSize = nil
        }
        let previewFrameOrigin: CGPoint?
        if hasPreviewFrameOrigin {
            guard let previewFrameOriginX = cursor.readFloat64(),
                  let previewFrameOriginY = cursor.readFloat64() else {
                throw OuterframeContentSocketMessageError.truncatedPayload
            }
            previewFrameOrigin = CGPoint(x: previewFrameOriginX, y: previewFrameOriginY)
        } else {
            previewFrameOrigin = nil
        }
        items.append(OuterframeContentDraggingItem(
            pasteboardItem: OuterframeContentPasteboardItem(representations: representations),
            previewImageData: hasPreview ? previewImageData : nil,
            previewSize: previewSize,
            previewFrameOrigin: previewFrameOrigin
        ))
    }
    return items
}

private func makeMouseEventFrame(type: BrowserToContentMessageKind,
                                 point: CGPoint,
                                 modifierFlags: NSEvent.ModifierFlags,
                                 clickCount: Int? = nil) -> Data {
    var payload = Data(capacity: clickCount == nil ? 24 : 28)
    payload.append(float64: point.x)
    payload.append(float64: point.y)
    payload.append(uint64: UInt64(modifierFlags.rawValue))
    if let clickCount {
        payload.append(uint32: UInt32(truncatingIfNeeded: clickCount))
    }
    return makeBrowserToContentFrame(type: type, payload: payload)
}

private func readMouseEvent(cursor: inout DataCursor,
                            includesClickCount: Bool) throws -> (point: CGPoint, modifierFlags: NSEvent.ModifierFlags, clickCount: Int) {
    guard let x = cursor.readFloat64(),
          let y = cursor.readFloat64(),
          let modifierFlags = cursor.readUInt64() else {
        throw OuterframeContentSocketMessageError.truncatedPayload
    }
    if includesClickCount {
        guard let clickCount = cursor.readUInt32() else {
            throw OuterframeContentSocketMessageError.truncatedPayload
        }
        return (CGPoint(x: x, y: y), NSEvent.ModifierFlags(rawValue: UInt(modifierFlags)), Int(clickCount))
    }
    return (CGPoint(x: x, y: y), NSEvent.ModifierFlags(rawValue: UInt(modifierFlags)), 0)
}

// MARK: - Data Cursor

private struct OffsetPayloadBuilder {
    private struct Reference {
        let patchOffset: Int
        let variableOffset: Int
        let length: Int
    }

    private var fixed = Data()
    private var variable = Data()
    private var references: [Reference] = []
    private let referenceBaseOffset: Int

    init(referenceBaseOffset: Int = OuterframeContentSocketMessageTypeLength) {
        self.referenceBaseOffset = referenceBaseOffset
    }

    mutating func append(uint32 value: UInt32) {
        fixed.append(uint32: value)
    }

    mutating func append(int32 value: Int32) {
        fixed.append(int32: value)
    }

    mutating func append(uint16 value: UInt16) {
        fixed.append(uint16: value)
    }

    mutating func append(uint8 value: UInt8) {
        fixed.append(uint8: value)
    }

    mutating func append(uint64 value: UInt64) {
        fixed.append(uint64: value)
    }

    mutating func append(float32 value: Float32) {
        fixed.append(float32: value)
    }

    mutating func append(float64 value: Double) {
        fixed.append(float64: value)
    }

    mutating func append(uuid: UUID) {
        fixed.append(uuid: uuid)
    }

    mutating func append(stringReference string: String) throws {
        guard let data = string.data(using: .utf8) else {
            throw OuterframeContentSocketMessageError.encodingFailure("Invalid UTF-8 string")
        }
        try append(dataReference: data)
    }

    mutating func append(dataReference data: Data) throws {
        guard data.count <= UInt32.max else {
            throw OuterframeContentSocketMessageError.encodingFailure("Data too long")
        }
        let patchOffset = fixed.count
        fixed.append(uint32: 0)
        fixed.append(uint32: UInt32(data.count))
        references.append(Reference(patchOffset: patchOffset,
                                    variableOffset: variable.count,
                                    length: data.count))
        variable.append(data)
    }

    mutating func finalize() throws -> Data {
        guard fixed.count <= UInt32.max,
              variable.count <= UInt32.max,
              variable.count <= Int(UInt32.max) - fixed.count else {
            throw OuterframeContentSocketMessageError.encodingFailure("Payload too long")
        }

        for reference in references {
            let offset = referenceBaseOffset + fixed.count + reference.variableOffset
            guard offset <= UInt32.max,
                  reference.length <= UInt32.max else {
                throw OuterframeContentSocketMessageError.encodingFailure("Payload too long")
            }
            fixed.replaceUInt32(at: reference.patchOffset, with: UInt32(offset))
            fixed.replaceUInt32(at: reference.patchOffset + 4, with: UInt32(reference.length))
        }

        var payload = Data(capacity: fixed.count + variable.count)
        payload.append(fixed)
        payload.append(variable)
        return payload
    }
}

private struct DataCursor {
    private let data: Data
    private var offset: Int = 0

    init(_ data: Data) {
        self.data = data
    }

    mutating func readUInt32() -> UInt32? {
        guard offset + 4 <= data.count else { return nil }
        let value = data[offset..<(offset + 4)].enumerated().reduce(UInt32(0)) {
            $0 | (UInt32($1.element) << (8 * $1.offset))
        }
        offset += 4
        return value
    }

    mutating func readInt32() -> Int32? {
        guard let value = readUInt32() else { return nil }
        return Int32(bitPattern: value)
    }

    mutating func readUInt16() -> UInt16? {
        guard offset + 2 <= data.count else { return nil }
        let value = data[offset..<(offset + 2)].enumerated().reduce(UInt16(0)) {
            $0 | (UInt16($1.element) << (8 * $1.offset))
        }
        offset += 2
        return value
    }

    mutating func readUInt8() -> UInt8? {
        guard offset + 1 <= data.count else { return nil }
        let value = data[offset]
        offset += 1
        return value
    }

    mutating func readUInt64() -> UInt64? {
        guard offset + 8 <= data.count else { return nil }
        let value = data[offset..<(offset + 8)].enumerated().reduce(UInt64(0)) {
            $0 | (UInt64($1.element) << (8 * $1.offset))
        }
        offset += 8
        return value
    }

    mutating func readFloat32() -> Float32? {
        guard let bits = readUInt32() else { return nil }
        return Float32(bitPattern: bits)
    }

    mutating func readFloat64() -> Float64? {
        guard let bits = readUInt64() else { return nil }
        return Float64(bitPattern: bits)
    }

    mutating func readData(_ length: Int) -> Data? {
        guard length >= 0,
              length <= data.count - offset else { return nil }
        let range = offset..<(offset + length)
        offset += length
        return data.subdata(in: range)
    }

    mutating func readDataReference() -> Data? {
        guard let offsetValue = readUInt32(),
              let lengthValue = readUInt32() else {
            return nil
        }
        let start = Int(offsetValue)
        let length = Int(lengthValue)
        guard start <= data.count,
              length <= data.count - start else {
            return nil
        }
        return data.subdata(in: start..<(start + length))
    }

    mutating func readStringReference() -> String? {
        guard let data = readDataReference() else { return nil }
        return String(data: data, encoding: .utf8)
    }

    mutating func readUUID() -> UUID? {
        guard let bytes = readData(16) else { return nil }
        return bytes.withUnsafeBytes { raw -> UUID? in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return nil }
            return NSUUID(uuidBytes: base) as UUID
        }
    }
}

// MARK: - Data Extensions

fileprivate extension Data {
    mutating func append(uint32 value: UInt32) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }

    mutating func append(int32 value: Int32) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }

    mutating func append(uint16 value: UInt16) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }

    mutating func append(uint8 value: UInt8) {
        append(value)
    }

    mutating func append(uint64 value: UInt64) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }

    mutating func append(float64 value: Double) {
        append(uint64: value.bitPattern)
    }

    mutating func append(float32 value: Float32) {
        append(int32: Int32(bitPattern: value.bitPattern))
    }

    mutating func append(uuid: UUID) {
        var uuidValue = uuid.uuid
        Swift.withUnsafeBytes(of: &uuidValue) { append(contentsOf: $0) }
    }

    mutating func replaceUInt32(at offset: Int, with value: UInt32) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) {
            replaceSubrange(offset..<(offset + 4), with: $0)
        }
    }

}
