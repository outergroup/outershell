import Darwin
import Foundation
import os.log

private let safeSpaceSocketRelayLogger = Logger(subsystem: "org.outershell.OuterShell", category: "SafeSpaceSocketRelay")
private let safeSpaceSocketRelayBufferLimit = 2 * 1024 * 1024
private let safeSpaceSocketRelayReadChunkSize = 64 * 1024
private let safeSpaceSocketRelayPingInterval: DispatchTimeInterval = .seconds(15)

private final class SafeDispatchSource {
    var dispatchSource: DispatchSourceProtocol
    private(set) var isResumed: Bool
    private var isCancelled = false

    init(dispatchSource: DispatchSourceProtocol, isResumed: Bool) {
        self.dispatchSource = dispatchSource
        self.isResumed = isResumed
    }

    func safeSuspend() {
        guard isResumed else { return }
        dispatchSource.suspend()
        isResumed = false
    }

    func safeResume() {
        guard !isResumed else { return }
        dispatchSource.resume()
        isResumed = true
    }

    func cancel() {
        guard !isCancelled else { return }
        safeResume()
        dispatchSource.cancel()
        isCancelled = true
    }

    deinit {
        cancel()
    }
}

enum SafeSpaceSocketRelayError: Error, LocalizedError {
    case bridgeExited(String)
    case sudoPasswordRequired
    case missingHelper
    case helperNotExecutable(String)
    case listenFailed(String)
    case sudoValidationFailed(String)

    var errorDescription: String? {
        switch self {
        case .bridgeExited(let stderr):
            if stderr.isEmpty {
                return "The socket bridge exited."
            }
            return "The socket bridge exited: \(stderr)"
        case .sudoPasswordRequired:
            return "A sudo password is required to start the root socket bridge."
        case .missingHelper:
            return "The bundled outer-socket-bridge helper is missing."
        case .helperNotExecutable(let path):
            return "The bundled outer-socket-bridge helper is not executable at \(path)."
        case .listenFailed(let message):
            return "Failed to start the local root socket proxy: \(message)"
        case .sudoValidationFailed(let message):
            return message.isEmpty ? "The sudo password was rejected." : message
        }
    }
}

func safeSpaceSocketRelayNSError(_ error: Error) -> NSError {
    if let localError = error as? SafeSpaceSocketRelayError {
        return NSError(domain: "OuterShell.SafeSpaceSocketRelay",
                       code: localError.nsErrorCode,
                       userInfo: [NSLocalizedDescriptionKey: localError.errorDescription ?? "Local root bridge failed."])
    }
    return error as NSError
}

private extension SafeSpaceSocketRelayError {
    var nsErrorCode: Int {
        switch self {
        case .bridgeExited:
            return 1
        case .sudoPasswordRequired:
            return 2
        case .missingHelper:
            return 3
        case .helperNotExecutable:
            return 4
        case .listenFailed:
            return 5
        case .sudoValidationFailed:
            return 6
        }
    }
}

private enum SafeSpaceSocketRelayProtocol {
    static let magic: UInt32 = 0x3142524f
    static let version: UInt16 = 1
    static let headerLength = 16
    static let maximumPayloadLength = 1024 * 1024

    enum FrameType: UInt16 {
        case hello = 1
        case open = 2
        case openOK = 3
        case openError = 4
        case data = 5
        case eof = 6
        case close = 7
        case ping = 8
        case pong = 9
        case socketState = 10
        case openPath = 11
    }

    struct Frame {
        let type: FrameType
        let streamID: UInt32
        let payload: Data

        init(type: FrameType, streamID: UInt32, payload: Data = Data()) {
            self.type = type
            self.streamID = streamID
            self.payload = payload
        }
    }

    enum ParseError: Error, LocalizedError {
        case invalidMagic(UInt32)
        case unsupportedVersion(UInt16)
        case unknownFrameType(UInt16)
        case payloadTooLarge(UInt32)

        var errorDescription: String? {
            switch self {
            case .invalidMagic(let magic):
                return "Invalid socket bridge frame magic 0x\(String(magic, radix: 16))."
            case .unsupportedVersion(let version):
                return "Unsupported socket bridge frame version \(version)."
            case .unknownFrameType(let type):
                return "Unknown socket bridge frame type \(type)."
            case .payloadTooLarge(let length):
                return "Socket bridge frame payload is too large (\(length) bytes)."
            }
        }
    }

    static func encode(_ frame: Frame) throws -> Data {
        guard frame.payload.count <= maximumPayloadLength else {
            throw ParseError.payloadTooLarge(UInt32(frame.payload.count))
        }
        var data = Data(count: headerLength)
        data.withUnsafeMutableBytes { buffer in
            writeUInt32(buffer.baseAddress!.advanced(by: 0), magic)
            writeUInt16(buffer.baseAddress!.advanced(by: 4), version)
            writeUInt16(buffer.baseAddress!.advanced(by: 6), frame.type.rawValue)
            writeUInt32(buffer.baseAddress!.advanced(by: 8), frame.streamID)
            writeUInt32(buffer.baseAddress!.advanced(by: 12), UInt32(frame.payload.count))
        }
        data.append(frame.payload)
        return data
    }

    private static func writeUInt16(_ pointer: UnsafeMutableRawPointer, _ value: UInt16) {
        let bytes = pointer.assumingMemoryBound(to: UInt8.self)
        bytes[0] = UInt8(value & 0xff)
        bytes[1] = UInt8((value >> 8) & 0xff)
    }

    private static func writeUInt32(_ pointer: UnsafeMutableRawPointer, _ value: UInt32) {
        let bytes = pointer.assumingMemoryBound(to: UInt8.self)
        bytes[0] = UInt8(value & 0xff)
        bytes[1] = UInt8((value >> 8) & 0xff)
        bytes[2] = UInt8((value >> 16) & 0xff)
        bytes[3] = UInt8((value >> 24) & 0xff)
    }

    static func readUInt16(_ data: Data, _ offset: Int) -> UInt16 {
        UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
    }

    static func readUInt32(_ data: Data, _ offset: Int) -> UInt32 {
        UInt32(data[offset]) |
        (UInt32(data[offset + 1]) << 8) |
        (UInt32(data[offset + 2]) << 16) |
        (UInt32(data[offset + 3]) << 24)
    }
}

private final class SafeSpaceSocketRelayFrameParser {
    private var buffer = Data()

    func append(_ data: Data) {
        buffer.append(data)
    }

    func nextFrame() throws -> SafeSpaceSocketRelayProtocol.Frame? {
        guard buffer.count >= SafeSpaceSocketRelayProtocol.headerLength else {
            return nil
        }

        let magic = SafeSpaceSocketRelayProtocol.readUInt32(buffer, 0)
        guard magic == SafeSpaceSocketRelayProtocol.magic else {
            throw SafeSpaceSocketRelayProtocol.ParseError.invalidMagic(magic)
        }

        let version = SafeSpaceSocketRelayProtocol.readUInt16(buffer, 4)
        guard version == SafeSpaceSocketRelayProtocol.version else {
            throw SafeSpaceSocketRelayProtocol.ParseError.unsupportedVersion(version)
        }

        let typeRaw = SafeSpaceSocketRelayProtocol.readUInt16(buffer, 6)
        guard let type = SafeSpaceSocketRelayProtocol.FrameType(rawValue: typeRaw) else {
            throw SafeSpaceSocketRelayProtocol.ParseError.unknownFrameType(typeRaw)
        }

        let streamID = SafeSpaceSocketRelayProtocol.readUInt32(buffer, 8)
        let payloadLength = SafeSpaceSocketRelayProtocol.readUInt32(buffer, 12)
        guard payloadLength <= SafeSpaceSocketRelayProtocol.maximumPayloadLength else {
            throw SafeSpaceSocketRelayProtocol.ParseError.payloadTooLarge(payloadLength)
        }

        let totalLength = SafeSpaceSocketRelayProtocol.headerLength + Int(payloadLength)
        guard buffer.count >= totalLength else {
            return nil
        }

        let payload: Data
        if payloadLength == 0 {
            payload = Data()
        } else {
            payload = buffer.subdata(in: SafeSpaceSocketRelayProtocol.headerLength..<totalLength)
        }
        buffer.removeSubrange(0..<totalLength)
        return SafeSpaceSocketRelayProtocol.Frame(type: type, streamID: streamID, payload: payload)
    }
}

private final class SafeSpaceSocketRelayStream {
    let id: UInt32
    var sock: Int32
    var toLocal = Data()
    var toLocalOffset = 0
    var localReadOpen = true
    var remoteReadOpen = true
    var openedByRemote = false

    init(id: UInt32, sock: Int32) {
        self.id = id
        self.sock = sock
    }
}

final class SafeSpaceSocketRelayConnection: @unchecked Sendable {
    let socketPath: String

    private let process: Process?
    private let inputHandle: FileHandle
    private let outputHandle: FileHandle
    private let errorHandle: FileHandle?
    private let sendsSocketPathWithOpen: Bool
    private let closesAfterSingleStream: Bool
    private let queue = DispatchQueue(label: "org.outershell.OuterShell.safe-space-socket-relay")
    private let cleanup: @Sendable (SafeSpaceSocketRelayConnection) -> Void

    private var parser = SafeSpaceSocketRelayFrameParser()
    private var readyContinuation: CheckedContinuation<SafeSpaceSocketRelayConnection, Error>?
    private var streams: [UInt32: SafeSpaceSocketRelayStream] = [:]
    private var streamSources: [UInt32: (read: SafeDispatchSource, write: SafeDispatchSource)] = [:]
    private var nextStreamID: UInt32 = 1
    private var stderr = Data()
    private var closed = false
    private var ready = false
    private var pingTimer: SafeDispatchSource?

    private init(socketPath: String,
                 process: Process?,
                 inputHandle: FileHandle,
                 outputHandle: FileHandle,
                 errorHandle: FileHandle?,
                 sendsSocketPathWithOpen: Bool,
                 closesAfterSingleStream: Bool,
                 cleanup: @escaping @Sendable (SafeSpaceSocketRelayConnection) -> Void) {
        self.socketPath = socketPath
        self.process = process
        self.inputHandle = inputHandle
        self.outputHandle = outputHandle
        self.errorHandle = errorHandle
        self.sendsSocketPathWithOpen = sendsSocketPathWithOpen
        self.closesAfterSingleStream = closesAfterSingleStream
        self.cleanup = cleanup
    }

    static func start(socketPath: String,
                      sudoPassword: String?,
                      cleanup: @escaping @Sendable (SafeSpaceSocketRelayConnection) -> Void) async throws -> SafeSpaceSocketRelayConnection {
        let helperURL = try helperURL()
        let arguments = [
            sudoPassword == nil ? "-n" : "-S",
            "-p",
            "",
            helperURL.path,
            "bridge",
            "--socket",
            socketPath
        ]
        return try await start(socketPath: socketPath,
                               executableURL: URL(fileURLWithPath: "/usr/bin/sudo"),
                               arguments: arguments,
                               initialInput: sudoPassword.map { Data("\($0)\n".utf8) },
                               closesAfterSingleStream: false,
                               cleanup: cleanup)
    }

    static func startWorkspace(containerName: String,
                               socketPath: String,
                               runtimeExecutablePath: String,
                               user: String = "root",
                               closesAfterSingleStream: Bool = false,
                               cleanup: @escaping @Sendable (SafeSpaceSocketRelayConnection) -> Void) async throws -> SafeSpaceSocketRelayConnection {
        let isRoot = user == "root"
        let homeDirectory = isRoot ? "/root" : "/home/workspace"
        let runtimeUserID = isRoot ? "0" : String(getuid())
        let arguments = [
            "exec",
            "--interactive",
            "--user", user,
            containerName,
            "/usr/bin/env",
            "-i",
            "HOME=\(homeDirectory)",
            "USER=\(user)",
            "LOGNAME=\(user)",
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "XDG_RUNTIME_DIR=/run/user/\(runtimeUserID)",
            "/usr/local/bin/outer-socket-bridge",
            "bridge",
            "--socket",
            socketPath
        ]
        return try await start(socketPath: socketPath,
                               executableURL: URL(fileURLWithPath: runtimeExecutablePath),
                               arguments: arguments,
                               initialInput: nil,
                               closesAfterSingleStream: closesAfterSingleStream,
                               cleanup: cleanup)
    }

    private static func start(socketPath: String,
                              executableURL: URL,
                              arguments: [String],
                              initialInput: Data?,
                              closesAfterSingleStream: Bool,
                              cleanup: @escaping @Sendable (SafeSpaceSocketRelayConnection) -> Void) async throws -> SafeSpaceSocketRelayConnection {
        let process = Process()
        let inputPipe = Pipe()
        let outputPipe = Pipe()
        let errorPipe = Pipe()
        process.executableURL = executableURL
        process.arguments = arguments
        process.standardInput = inputPipe
        process.standardOutput = outputPipe
        process.standardError = errorPipe

        let bridge = SafeSpaceSocketRelayConnection(socketPath: socketPath,
                                               process: process,
                                               inputHandle: inputPipe.fileHandleForWriting,
                                               outputHandle: outputPipe.fileHandleForReading,
                                               errorHandle: errorPipe.fileHandleForReading,
                                               sendsSocketPathWithOpen: false,
                                               closesAfterSingleStream: closesAfterSingleStream,
                                               cleanup: cleanup)
        try process.run()
        if let initialInput {
            inputPipe.fileHandleForWriting.write(initialInput)
        }
        bridge.startReading()
        return try await bridge.waitUntilReady()
    }

    static func validateSudoPassword(_ password: String) async throws {
        let process = Process()
        let inputPipe = Pipe()
        let outputPipe = Pipe()
        let errorPipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/sudo")
        process.arguments = ["-S", "-p", "", "-v"]
        process.standardInput = inputPipe
        process.standardOutput = outputPipe
        process.standardError = errorPipe
        try process.run()
        inputPipe.fileHandleForWriting.write(Data("\(password)\n".utf8))
        try? inputPipe.fileHandleForWriting.close()
        process.waitUntilExit()
        let stderr = errorPipe.fileHandleForReading.readDataToEndOfFile()
        let stdout = outputPipe.fileHandleForReading.readDataToEndOfFile()
        guard process.terminationStatus == 0 else {
            let message = String(data: stderr + stdout, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            throw SafeSpaceSocketRelayError.sudoValidationFailed(message)
        }
    }

    static func authorizeUnixHTTPSocket(socketPath: String) async throws {
        let process = Process()
        let outputPipe = Pipe()
        let errorPipe = Pipe()
        process.executableURL = try helperURL()
        process.arguments = ["authorize", "--socket", socketPath]
        process.standardOutput = outputPipe
        process.standardError = errorPipe
        try process.run()
        process.waitUntilExit()

        guard process.terminationStatus == 0 else {
            let stderr = errorPipe.fileHandleForReading.readDataToEndOfFile()
            let stdout = outputPipe.fileHandleForReading.readDataToEndOfFile()
            let message = String(data: stderr + stdout, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            throw SafeSpaceSocketRelayError.bridgeExited(message.isEmpty ? "Unix socket is not allowed." : message)
        }
    }

    private static func helperURL() throws -> URL {
        let executableDirectory = Bundle.main.executableURL?.deletingLastPathComponent()
        var candidates = [
            executableDirectory?.appendingPathComponent("outer-socket-bridge"),
            Bundle.main.url(forResource: "outer-socket-bridge", withExtension: nil),
            Bundle.main.resourceURL?.appendingPathComponent("outer-socket-bridge")
        ].compactMap { $0 }
        if let executableDirectory,
           executableDirectory.lastPathComponent == "MacOS",
           executableDirectory.deletingLastPathComponent().lastPathComponent == "Contents",
           executableDirectory
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .pathExtension == "xpc" {
            let appMacOSDirectory = executableDirectory
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .appendingPathComponent("MacOS", isDirectory: true)
            candidates.append(appMacOSDirectory.appendingPathComponent("outer-socket-bridge"))
        }

        for url in candidates {
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
                  !isDirectory.boolValue else {
                continue
            }
            guard FileManager.default.isExecutableFile(atPath: url.path) else {
                throw SafeSpaceSocketRelayError.helperNotExecutable(url.path)
            }
            return url
        }

        throw SafeSpaceSocketRelayError.missingHelper
    }

    func waitUntilReady() async throws -> SafeSpaceSocketRelayConnection {
        try await withCheckedThrowingContinuation { continuation in
            queue.async {
                if self.ready {
                    continuation.resume(returning: self)
                    return
                }
                self.readyContinuation = continuation
            }
        }
    }

    func openLocalSocketStream(sock: Int32) {
        queue.async {
            self.openLocalSocketStreamOnQueue(sock: sock)
        }
    }

    var isClosed: Bool {
        queue.sync {
            closed
        }
    }

    func close() {
        queue.async {
            self.fail(error: SafeSpaceSocketRelayError.bridgeExited(""))
        }
    }

    private var stderrText: String {
        String(data: stderr, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    private func startReading() {
        outputHandle.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard let bridge = self else { return }
            bridge.queue.async {
                guard !bridge.closed else { return }
                if data.isEmpty {
                    bridge.fail(error: bridge.exitError())
                } else {
                    bridge.handleStdout(data)
                }
            }
        }
        if let errorHandle {
            errorHandle.readabilityHandler = { [weak self] handle in
                let data = handle.availableData
                guard let bridge = self else { return }
                bridge.queue.async {
                    guard !bridge.closed, !data.isEmpty else { return }
                    bridge.stderr.append(data)
                    if let text = String(data: data, encoding: .utf8), !text.isEmpty {
                        safeSpaceSocketRelayLogger.error("outer-socket-bridge stderr: \(text, privacy: .public)")
                    }
                }
            }
        }
        if let process {
            process.terminationHandler = { [weak self] _ in
                guard let bridge = self else { return }
                bridge.queue.async {
                    bridge.fail(error: bridge.exitError())
                }
            }
        }
    }

    private func exitError() -> Error {
        let lower = stderrText.lowercased()
        if lower.contains("password is required") ||
            lower.contains("a password is required") ||
            lower.contains("no tty present") {
            return SafeSpaceSocketRelayError.sudoPasswordRequired
        }
        return SafeSpaceSocketRelayError.bridgeExited(stderrText)
    }

    private func handleStdout(_ data: Data) {
        parser.append(data)

        do {
            while let frame = try parser.nextFrame() {
                handle(frame)
            }
        } catch {
            safeSpaceSocketRelayLogger.error("Local root bridge protocol error: \(error.localizedDescription, privacy: .public)")
            fail(error: error)
        }
    }

    private func openLocalSocketStreamOnQueue(sock: Int32) {
        guard !closed else {
            Darwin.close(sock)
            return
        }

        let streamID = nextStreamID
        nextStreamID &+= 1
        if nextStreamID == 0 {
            nextStreamID = 1
        }

        setNonBlocking(sock)
        let stream = SafeSpaceSocketRelayStream(id: streamID, sock: sock)
        streams[streamID] = stream
        safeSpaceSocketRelayLogger.debug("Local root bridge opening stream \(streamID, privacy: .public) for \(self.socketPath, privacy: .public)")

        let readSource = SafeDispatchSource(
            dispatchSource: DispatchSource.makeReadSource(fileDescriptor: sock, queue: queue),
            isResumed: false
        )
        readSource.dispatchSource.setEventHandler { [weak self] in
            self?.handleLocalRead(streamID: streamID)
        }

        let writeSource = SafeDispatchSource(
            dispatchSource: DispatchSource.makeWriteSource(fileDescriptor: sock, queue: queue),
            isResumed: false
        )
        writeSource.dispatchSource.setEventHandler { [weak self] in
            self?.handleLocalWrite(streamID: streamID)
        }

        streamSources[streamID] = (readSource, writeSource)
        sendFrame(.init(type: sendsSocketPathWithOpen ? .openPath : .open,
                        streamID: streamID,
                        payload: sendsSocketPathWithOpen ? Data(socketPath.utf8) : Data()))
    }

    private func handle(_ frame: SafeSpaceSocketRelayProtocol.Frame) {
        switch frame.type {
        case .hello:
            safeSpaceSocketRelayLogger.debug("Local root bridge ready for \(self.socketPath, privacy: .public)")
            ready = true
            startPingTimer()
            if let continuation = readyContinuation {
                readyContinuation = nil
                continuation.resume(returning: self)
            }

        case .openOK:
            guard let stream = streams[frame.streamID] else {
                sendFrame(.init(type: .close, streamID: frame.streamID))
                return
            }
            stream.openedByRemote = true
            streamSources[frame.streamID]?.read.safeResume()

        case .openError:
            let message = String(data: frame.payload, encoding: .utf8) ?? "open failed"
            safeSpaceSocketRelayLogger.error("Local root bridge refused stream \(frame.streamID, privacy: .public): \(message, privacy: .public)")
            closeStream(streamID: frame.streamID, notifyRemote: false)

        case .data:
            guard let stream = streams[frame.streamID] else {
                sendFrame(.init(type: .close, streamID: frame.streamID))
                return
            }
            stream.toLocal.append(frame.payload)
            streamSources[frame.streamID]?.write.safeResume()

        case .eof:
            guard let stream = streams[frame.streamID] else {
                sendFrame(.init(type: .close, streamID: frame.streamID))
                return
            }
            stream.remoteReadOpen = false
            if stream.toLocal.isEmpty {
                _ = shutdown(stream.sock, SHUT_WR)
            }
            if !stream.localReadOpen && stream.toLocal.isEmpty {
                closeStream(streamID: frame.streamID, notifyRemote: false)
            }

        case .close:
            guard let stream = streams[frame.streamID] else { return }
            stream.remoteReadOpen = false
            stream.localReadOpen = false
            streamSources[frame.streamID]?.read.safeSuspend()
            if stream.toLocal.isEmpty {
                closeStream(streamID: frame.streamID, notifyRemote: false)
            } else {
                streamSources[frame.streamID]?.write.safeResume()
            }

        case .ping:
            sendFrame(.init(type: .pong, streamID: frame.streamID, payload: frame.payload))

        case .pong, .open, .openPath, .socketState:
            break
        }
    }

    private func startPingTimer() {
        guard pingTimer == nil else { return }
        let timerSource = DispatchSource.makeTimerSource(queue: queue)
        timerSource.schedule(deadline: .now() + safeSpaceSocketRelayPingInterval,
                             repeating: safeSpaceSocketRelayPingInterval,
                             leeway: .seconds(2))
        let timer = SafeDispatchSource(dispatchSource: timerSource, isResumed: false)
        timer.dispatchSource.setEventHandler { [weak self] in
            guard let self, !self.closed else { return }
            self.sendFrame(.init(type: .ping, streamID: 0))
        }
        pingTimer = timer
        timer.safeResume()
    }

    private func cancelPingTimer() {
        pingTimer?.cancel()
        pingTimer = nil
    }

    private func handleLocalRead(streamID: UInt32) {
        guard let stream = streams[streamID], stream.localReadOpen else {
            return
        }

        while pendingBridgeBytes < safeSpaceSocketRelayBufferLimit {
            var bytes = [UInt8](repeating: 0, count: safeSpaceSocketRelayReadChunkSize)
            let count = bytes.withUnsafeMutableBytes { buffer in
                recv(stream.sock, buffer.baseAddress!, buffer.count, 0)
            }

            if count > 0 {
                sendFrame(.init(type: .data,
                                streamID: streamID,
                                payload: Data(bytes.prefix(count))))
                continue
            }

            if count == 0 {
                stream.localReadOpen = false
                streamSources[streamID]?.read.safeSuspend()
                if closesAfterSingleStream {
                    closeStream(streamID: streamID, notifyRemote: true)
                    return
                }
                sendFrame(.init(type: .eof, streamID: streamID))
                if !stream.remoteReadOpen && stream.toLocal.isEmpty {
                    closeStream(streamID: streamID, notifyRemote: true)
                }
                return
            }

            if errno == EAGAIN || errno == EWOULDBLOCK {
                return
            }

            closeStream(streamID: streamID, notifyRemote: true)
            return
        }

        streamSources[streamID]?.read.safeSuspend()
    }

    private var pendingBridgeBytes: Int {
        streams.values.reduce(0) { $0 + max(0, $1.toLocal.count - $1.toLocalOffset) }
    }

    private func handleLocalWrite(streamID: UInt32) {
        guard let stream = streams[streamID] else {
            return
        }

        while stream.toLocalOffset < stream.toLocal.count {
            let remaining = stream.toLocal.count - stream.toLocalOffset
            let written = stream.toLocal.withUnsafeBytes { buffer in
                send(stream.sock,
                     buffer.baseAddress!.advanced(by: stream.toLocalOffset),
                     remaining,
                     0)
            }

            if written > 0 {
                stream.toLocalOffset += written
                continue
            }

            if written == 0 || errno == EAGAIN || errno == EWOULDBLOCK {
                return
            }

            closeStream(streamID: streamID, notifyRemote: true)
            return
        }

        stream.toLocal.removeAll(keepingCapacity: true)
        stream.toLocalOffset = 0
        streamSources[streamID]?.write.safeSuspend()

        if !stream.remoteReadOpen {
            _ = shutdown(stream.sock, SHUT_WR)
        }

        if !stream.remoteReadOpen && !stream.localReadOpen {
            closeStream(streamID: streamID, notifyRemote: true)
        }
    }

    private func sendFrame(_ frame: SafeSpaceSocketRelayProtocol.Frame) {
        do {
            let encoded = try SafeSpaceSocketRelayProtocol.encode(frame)
            inputHandle.write(encoded)
        } catch {
            safeSpaceSocketRelayLogger.error("Failed to write local root bridge frame: \(error.localizedDescription, privacy: .public)")
            fail(error: error)
        }
    }

    private func closeStream(streamID: UInt32, notifyRemote: Bool) {
        guard let stream = streams.removeValue(forKey: streamID) else {
            return
        }

        if notifyRemote {
            sendFrame(.init(type: .close, streamID: streamID))
        }

        if let sources = streamSources.removeValue(forKey: streamID) {
            sources.read.safeSuspend()
            sources.write.safeSuspend()
            sources.read.dispatchSource.cancel()
            sources.write.dispatchSource.cancel()
        }

        if stream.sock >= 0 {
            Darwin.close(stream.sock)
            stream.sock = -1
        }

        if closesAfterSingleStream && streams.isEmpty {
            fail(error: SafeSpaceSocketRelayError.bridgeExited(""))
        }
    }

    private func fail(error: Error) {
        guard !closed else { return }
        closed = true
        outputHandle.readabilityHandler = nil
        errorHandle?.readabilityHandler = nil
        cancelPingTimer()
        if let continuation = readyContinuation {
            readyContinuation = nil
            continuation.resume(throwing: error)
        }
        for streamID in Array(streams.keys) {
            closeStream(streamID: streamID, notifyRemote: false)
        }
        try? inputHandle.close()
        try? outputHandle.close()
        if let process, process.isRunning {
            process.terminate()
        }
        cleanup(self)
    }
}

final class SafeSpaceSocketRelayForward: @unchecked Sendable {
    let port: UInt16
    let publishedSocketPath: String?

    private let listenSock: Int32
    private let acceptSource: SafeDispatchSource
    private let sharedBridge: SafeSpaceSocketRelayConnection?
    private let workspaceContainerName: String?
    private let workspaceSocketPath: String?
    private let workspaceUser: String?
    private let workspaceRuntimeExecutablePath: String?
    private let queue = DispatchQueue(label: "dev.outergroup.OuterLoop.safe-space-socket-relay-forward")
    private var closed = false
    private var availableWorkspaceBridge: SafeSpaceSocketRelayConnection?
    private var workspaceBridges: [ObjectIdentifier: SafeSpaceSocketRelayConnection] = [:]

    private init(listenSock: Int32,
                 port: UInt16,
                 publishedSocketPath: String?,
                 sharedBridge: SafeSpaceSocketRelayConnection?,
                 workspaceContainerName: String? = nil,
                 workspaceSocketPath: String? = nil,
                 workspaceUser: String? = nil,
                 workspaceRuntimeExecutablePath: String? = nil,
                 availableWorkspaceBridge: SafeSpaceSocketRelayConnection? = nil) {
        self.listenSock = listenSock
        self.port = port
        self.publishedSocketPath = publishedSocketPath
        self.sharedBridge = sharedBridge
        self.workspaceContainerName = workspaceContainerName
        self.workspaceSocketPath = workspaceSocketPath
        self.workspaceUser = workspaceUser
        self.workspaceRuntimeExecutablePath = workspaceRuntimeExecutablePath
        self.availableWorkspaceBridge = availableWorkspaceBridge
        let source = DispatchSource.makeReadSource(fileDescriptor: listenSock, queue: queue)
        self.acceptSource = SafeDispatchSource(dispatchSource: source, isResumed: false)
        self.acceptSource.dispatchSource.setEventHandler { [weak self] in
            self?.acceptConnections()
        }
        self.acceptSource.dispatchSource.setCancelHandler {
            Darwin.close(listenSock)
            if let publishedSocketPath {
                unlink(publishedSocketPath)
            }
        }
        self.acceptSource.safeResume()
    }

    static func start(socketPath: String, sudoPassword: String?) async throws -> SafeSpaceSocketRelayForward {
        let bridge = try await SafeSpaceSocketRelayConnection.start(socketPath: socketPath,
                                                               sudoPassword: sudoPassword,
                                                               cleanup: { _ in })

        let (listenSock, port) = try makeListener()
        return SafeSpaceSocketRelayForward(listenSock: listenSock,
                                      port: port,
                                      publishedSocketPath: nil,
                                      sharedBridge: bridge)
    }

    static func startWorkspace(containerName: String,
                               socketPath: String,
                               runtimeExecutablePath: String,
                               user: String = "root") async throws -> SafeSpaceSocketRelayForward {
        let bridge = try await SafeSpaceSocketRelayConnection.startWorkspace(containerName: containerName,
                                                                        socketPath: socketPath,
                                                                        runtimeExecutablePath: runtimeExecutablePath,
                                                                        user: user,
                                                                        cleanup: { _ in })

        let (listenSock, port) = try makeListener()
        return SafeSpaceSocketRelayForward(listenSock: listenSock,
                                      port: port,
                                      publishedSocketPath: nil,
                                      sharedBridge: bridge)
    }

    static func publishWorkspaceSocket(containerName: String,
                                       socketPath: String,
                                       runtimeExecutablePath: String,
                                       user: String = "root",
                                       at publishedSocketPath: String) async throws -> SafeSpaceSocketRelayForward {
        let bridge = try await SafeSpaceSocketRelayConnection.startWorkspace(containerName: containerName,
                                                                        socketPath: socketPath,
                                                                        runtimeExecutablePath: runtimeExecutablePath,
                                                                        user: user,
                                                                        closesAfterSingleStream: true,
                                                                        cleanup: { _ in })
        do {
            let listenSock = try makeUnixListener(path: publishedSocketPath)
            return SafeSpaceSocketRelayForward(listenSock: listenSock,
                                          port: 0,
                                          publishedSocketPath: publishedSocketPath,
                                          sharedBridge: nil,
                                          workspaceContainerName: containerName,
                                          workspaceSocketPath: socketPath,
                                          workspaceUser: user,
                                          workspaceRuntimeExecutablePath: runtimeExecutablePath,
                                          availableWorkspaceBridge: bridge)
        } catch {
            bridge.close()
            throw error
        }
    }

    func close() {
        queue.async {
            guard !self.closed else { return }
            self.closed = true
            self.acceptSource.cancel()
            self.sharedBridge?.close()
            self.availableWorkspaceBridge?.close()
            self.availableWorkspaceBridge = nil
            for bridge in self.workspaceBridges.values {
                bridge.close()
            }
            self.workspaceBridges.removeAll()
        }
    }

    var isClosed: Bool {
        let forwardClosed = queue.sync {
            closed
        }
        guard !forwardClosed else { return true }
        if let sharedBridge {
            return sharedBridge.isClosed
        }
        return false
    }

    private func acceptConnections() {
        guard !closed else { return }
        while true {
            var addr = sockaddr_storage()
            var len = socklen_t(MemoryLayout<sockaddr_storage>.size)
            let client = withUnsafeMutablePointer(to: &addr) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    accept(listenSock, $0, &len)
                }
            }

            if client >= 0 {
                if let sharedBridge {
                    sharedBridge.openLocalSocketStream(sock: client)
                } else {
                    openIsolatedWorkspaceBridge(for: client)
                }
                continue
            }

            if errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR {
                return
            }
            return
        }
    }

    private func openIsolatedWorkspaceBridge(for client: Int32) {
        guard let workspaceContainerName, let workspaceSocketPath, let workspaceUser,
              let workspaceRuntimeExecutablePath else {
            Darwin.close(client)
            return
        }

        workspaceBridges = workspaceBridges.filter { !$0.value.isClosed }
        if let bridge = availableWorkspaceBridge, !bridge.isClosed {
            availableWorkspaceBridge = nil
            workspaceBridges[ObjectIdentifier(bridge)] = bridge
            bridge.openLocalSocketStream(sock: client)
            return
        }
        availableWorkspaceBridge = nil

        Task { [weak self] in
            do {
                let bridge = try await SafeSpaceSocketRelayConnection.startWorkspace(
                    containerName: workspaceContainerName,
                    socketPath: workspaceSocketPath,
                    runtimeExecutablePath: workspaceRuntimeExecutablePath,
                    user: workspaceUser,
                    closesAfterSingleStream: true,
                    cleanup: { _ in }
                )
                guard let self else {
                    bridge.close()
                    Darwin.close(client)
                    return
                }
                self.queue.async {
                    guard !self.closed else {
                        bridge.close()
                        Darwin.close(client)
                        return
                    }
                    self.workspaceBridges[ObjectIdentifier(bridge)] = bridge
                    bridge.openLocalSocketStream(sock: client)
                }
            } catch {
                safeSpaceSocketRelayLogger.error(
                    "Failed to open isolated container socket bridge: \(error.localizedDescription, privacy: .public)"
                )
                Darwin.close(client)
            }
        }
    }

    private static func makeListener() throws -> (Int32, UInt16) {
        let sock = socket(AF_INET, SOCK_STREAM, 0)
        guard sock >= 0 else {
            throw SafeSpaceSocketRelayError.listenFailed(String(cString: strerror(errno)))
        }
        var one: Int32 = 1
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        setNonBlocking(sock)

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(0).bigEndian
        addr.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))

        let bindResult = withUnsafePointer(to: &addr) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else {
            let message = String(cString: strerror(errno))
            Darwin.close(sock)
            throw SafeSpaceSocketRelayError.listenFailed(message)
        }

        guard listen(sock, SOMAXCONN) == 0 else {
            let message = String(cString: strerror(errno))
            Darwin.close(sock)
            throw SafeSpaceSocketRelayError.listenFailed(message)
        }

        var actual = sockaddr_in()
        var actualLength = socklen_t(MemoryLayout<sockaddr_in>.size)
        let nameResult = withUnsafeMutablePointer(to: &actual) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(sock, $0, &actualLength)
            }
        }
        guard nameResult == 0 else {
            let message = String(cString: strerror(errno))
            Darwin.close(sock)
            throw SafeSpaceSocketRelayError.listenFailed(message)
        }

        return (sock, UInt16(bigEndian: actual.sin_port))
    }

    private static func makeUnixListener(path: String) throws -> Int32 {
        let pathBytes = Array(path.utf8CString)
        let maximumPathLength = MemoryLayout.size(ofValue: sockaddr_un().sun_path)
        guard pathBytes.count <= maximumPathLength else {
            throw SafeSpaceSocketRelayError.listenFailed("The published socket path is too long.")
        }

        let parentDirectory = URL(fileURLWithPath: path).deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(at: parentDirectory,
                                                    withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
            try FileManager.default.setAttributes([.posixPermissions: 0o700],
                                                  ofItemAtPath: parentDirectory.path)
        } catch {
            throw SafeSpaceSocketRelayError.listenFailed(error.localizedDescription)
        }

        if unlink(path) != 0 && errno != ENOENT {
            throw SafeSpaceSocketRelayError.listenFailed(String(cString: strerror(errno)))
        }

        let sock = socket(AF_UNIX, SOCK_STREAM, 0)
        guard sock >= 0 else {
            throw SafeSpaceSocketRelayError.listenFailed(String(cString: strerror(errno)))
        }
        setNonBlocking(sock)

        var address = sockaddr_un()
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        address.sun_family = sa_family_t(AF_UNIX)
        withUnsafeMutablePointer(to: &address.sun_path) { pointer in
            pointer.withMemoryRebound(to: CChar.self, capacity: maximumPathLength) { destination in
                for (index, byte) in pathBytes.enumerated() {
                    destination[index] = byte
                }
            }
        }

        let bindResult = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(sock, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard bindResult == 0 else {
            let message = String(cString: strerror(errno))
            Darwin.close(sock)
            throw SafeSpaceSocketRelayError.listenFailed(message)
        }

        guard chmod(path, S_IRUSR | S_IWUSR) == 0 else {
            let message = String(cString: strerror(errno))
            Darwin.close(sock)
            unlink(path)
            throw SafeSpaceSocketRelayError.listenFailed(message)
        }

        guard listen(sock, SOMAXCONN) == 0 else {
            let message = String(cString: strerror(errno))
            Darwin.close(sock)
            unlink(path)
            throw SafeSpaceSocketRelayError.listenFailed(message)
        }
        return sock
    }
}

private func setNonBlocking(_ fd: Int32) {
    let flags = fcntl(fd, F_GETFL, 0)
    if flags >= 0 {
        _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
    }
}
