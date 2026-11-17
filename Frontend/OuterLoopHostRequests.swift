import Foundation

private struct OuterLoopSSHCommandArgumentsRequest: Codable {
    let requestID: UUID
}

private struct OuterLoopSSHCommandArgumentsResponse: Codable {
    let requestID: UUID
    let arguments: [String]
}

private struct OuterLoopOpenFileRequest: Codable {
    let requestID: UUID
    let allowedFileExtensions: [String]
}

private struct OuterLoopOpenFileResponse: Codable {
    let requestID: UUID
    let fileURL: String?
}

/// Requests that only Outer Loop answers, sent as host-specific messages. Other
/// hosts report them unrecognized, and every pending request completes with nil.
@MainActor
final class OuterLoopHostRequests {
    private static let sshCommandArgumentsRequest = "sh.outerloop.sshCommandArgumentsRequest"
    private static let sshCommandArgumentsResponse = "sh.outerloop.sshCommandArgumentsResponse"
    private static let openFileRequest = "sh.outerloop.openFileRequest"
    private static let openFileResponse = "sh.outerloop.openFileResponse"

    private let host: OuterframeHost
    private var pendingSSHCommandArguments: [UUID: @MainActor ([String]?) -> Void] = [:]
    private var pendingOpenFiles: [UUID: @MainActor (URL?) -> Void] = [:]

    init(host: OuterframeHost) {
        self.host = host
    }

    /// Asks Outer Loop for the ssh command-line arguments of this session.
    /// Completes with nil after one second without an answer.
    func requestSSHCommandArguments(completion: @escaping @MainActor ([String]?) -> Void) {
        let requestID = UUID()
        guard let payload = try? JSONEncoder().encode(OuterLoopSSHCommandArgumentsRequest(requestID: requestID)) else {
            completion(nil)
            return
        }
        pendingSSHCommandArguments[requestID] = completion
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            self?.pendingSSHCommandArguments.removeValue(forKey: requestID)?(nil)
        }
        host.sendHostSpecificMessage(name: Self.sshCommandArgumentsRequest, payload: payload)
    }

    /// Asks Outer Loop to show an open panel. Completes with nil if canceled.
    func requestOpenFile(allowedFileExtensions: [String], completion: @escaping @MainActor (URL?) -> Void) {
        let requestID = UUID()
        guard let payload = try? JSONEncoder().encode(OuterLoopOpenFileRequest(
            requestID: requestID, allowedFileExtensions: allowedFileExtensions)) else {
            completion(nil)
            return
        }
        pendingOpenFiles[requestID] = completion
        host.sendHostSpecificMessage(name: Self.openFileRequest, payload: payload)
    }

    /// Completes pending requests from host-specific messages; ignores other messages.
    func handle(_ message: BrowserToContentMessage) {
        switch message {
        case .hostSpecificMessage(Self.sshCommandArgumentsResponse, let payload):
            if let response = try? JSONDecoder().decode(OuterLoopSSHCommandArgumentsResponse.self, from: payload) {
                pendingSSHCommandArguments.removeValue(forKey: response.requestID)?(response.arguments)
            }
        case .hostSpecificMessage(Self.openFileResponse, let payload):
            if let response = try? JSONDecoder().decode(OuterLoopOpenFileResponse.self, from: payload) {
                let url = response.fileURL.flatMap(URL.init(string:)).flatMap { $0.isFileURL ? $0 : nil }
                pendingOpenFiles.removeValue(forKey: response.requestID)?(url)
            }
        case .hostSpecificMessageUnrecognized(Self.sshCommandArgumentsRequest):
            let pending = pendingSSHCommandArguments
            pendingSSHCommandArguments.removeAll()
            pending.values.forEach { $0(nil) }
        case .hostSpecificMessageUnrecognized(Self.openFileRequest):
            let pending = pendingOpenFiles
            pendingOpenFiles.removeAll()
            pending.values.forEach { $0(nil) }
        default:
            break
        }
    }
}
