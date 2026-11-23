import Foundation

enum ContainerMessageError: Error, LocalizedError {
    case unsupportedOperation(String)
    case invalidResponse

    var errorDescription: String? {
        switch self {
        case .unsupportedOperation(let operation):
            return "Outer Shell does not support the container operation \(operation)."
        case .invalidResponse:
            return "Outer Shell returned an invalid container response."
        }
    }
}

enum ContainerMessage {
    static let contentType = "application/vnd.outershell.message"
    static let operations = [
        "list", "checkRuntimes", "containerStorage", "cleanContainerStorage", "cleanContainerBuildCache",
        "create", "duplicate", "changeRuntime", "rename", "start", "stop", "restart", "delete",
        "mountFolder", "unmountFolder", "chooseFolder", "updateDockerfile", "updateContainerConfiguration",
        "updateResources", "rebuild", "startApp", "stopApp", "restartApp", "appLogs", "publishSocket",
        "prepareShare", "beginImport", "cancelImport", "finishImport",
    ]

    static func request(operation: String, workspaceID: String?, arguments: [String: Any]) throws -> Data {
        guard let code = operations.firstIndex(of: operation) else {
            throw ContainerMessageError.unsupportedOperation(operation)
        }
        let identifier = Data((workspaceID ?? "").utf8)
        let encodedArguments = arguments.isEmpty ? Data() : try OffsetArchive.encodePropertyList(arguments)
        var message = Data()
        append(UInt16(35), to: &message)
        append(UInt16(code), to: &message)
        append(UInt32(0), to: &message)
        append(UInt32(24), to: &message)
        append(UInt32(identifier.count), to: &message)
        append(UInt32(24 + identifier.count), to: &message)
        append(UInt32(encodedArguments.count), to: &message)
        message.append(identifier)
        message.append(encodedArguments)
        return message
    }

    static func request<Value: Encodable>(_ value: Value) throws -> Data {
        let encoder = PropertyListEncoder()
        encoder.outputFormat = .binary
        guard var arguments = try PropertyListSerialization.propertyList(
            from: try encoder.encode(value),
            options: [],
            format: nil
        ) as? [String: Any], let operation = arguments.removeValue(forKey: "operation") as? String else {
            throw ContainerMessageError.invalidResponse
        }
        arguments.removeValue(forKey: "requestID")
        let workspaceID = arguments.removeValue(forKey: "workspaceID") as? String
        return try request(operation: operation, workspaceID: workspaceID, arguments: arguments)
    }

    static func registration(identifier: String, flags: UInt32, setMask: UInt32, buildContextPath: String) -> Data {
        let strings = [identifier, "", "", "", buildContextPath].map { Data($0.utf8) }
        var message = Data()
        append(UInt16(31), to: &message)
        append(UInt16(0), to: &message)
        append(flags, to: &message)
        append(setMask, to: &message)
        message.append(Data(repeating: 0, count: 16))
        var offset = 68
        for value in strings {
            append(UInt32(offset), to: &message)
            append(UInt32(value.count), to: &message)
            offset += value.count
        }
        for value in strings {
            message.append(value)
        }
        return message
    }

    static func commandError(_ data: Data) -> String? {
        let message = Data(data)
        guard message.count >= 22, integer16(message, at: 0) == 100 else {
            return "Outer Shell returned an invalid registry response."
        }
        guard integer32(message, at: 2) != 0 else { return nil }
        let error = (try? referenced(message, at: 14)).map { String(decoding: $0, as: UTF8.self) } ?? ""
        let trimmed = error.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "The registry change failed." : trimmed
    }

    static func decode<Value: Decodable>(_ type: Value.Type,
                                         from data: Data,
                                         defaults: [String: Any] = [:]) throws -> Value {
        let message = Data(data)
        guard message.count >= 30, integer16(message, at: 0) == 109 else {
            throw ContainerMessageError.invalidResponse
        }
        var merged = defaults
        if let snapshot = try dictionary(message, reference: 22) {
            merged.merge(snapshot) { _, new in new }
        }
        if let result = try dictionary(message, reference: 14) {
            merged.merge(result) { _, new in new }
        }
        if integer32(message, at: 2) != 0 {
            let error = String(decoding: try referenced(message, at: 6), as: UTF8.self)
            merged["error"] = error.isEmpty ? "The container operation failed." : error
        }
        return try decodePropertyList(type, from: merged)
    }

    static func decodeSnapshot<Value: Decodable>(_ type: Value.Type,
                                                 from data: Data,
                                                 defaults: [String: Any] = [:]) throws -> Value {
        guard let snapshot = try OffsetArchive.decodePropertyList(data) as? [String: Any] else {
            throw ContainerMessageError.invalidResponse
        }
        return try decodePropertyList(type, from: defaults.merging(snapshot) { _, new in new })
    }

    private static func decodePropertyList<Value: Decodable>(_ type: Value.Type, from value: [String: Any]) throws -> Value {
        let data = try PropertyListSerialization.data(fromPropertyList: value, format: .binary, options: 0)
        return try PropertyListDecoder().decode(type, from: data)
    }

    private static func dictionary(_ message: Data, reference offset: Int) throws -> [String: Any]? {
        let data = try referenced(message, at: offset)
        if data.isEmpty { return nil }
        guard let value = try OffsetArchive.decodePropertyList(data) as? [String: Any] else {
            throw ContainerMessageError.invalidResponse
        }
        return value
    }

    private static func referenced(_ message: Data, at offset: Int) throws -> Data {
        let start = Int(integer32(message, at: offset))
        let count = Int(integer32(message, at: offset + 4))
        guard start <= message.count, count <= message.count - start else {
            throw ContainerMessageError.invalidResponse
        }
        return message.subdata(in: start..<(start + count))
    }

    private static func append<Integer: FixedWidthInteger>(_ value: Integer, to data: inout Data) {
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    private static func integer16(_ data: Data, at offset: Int) -> UInt16 {
        UInt16(data[offset]) | UInt16(data[offset + 1]) << 8
    }

    private static func integer32(_ data: Data, at offset: Int) -> UInt32 {
        (0..<4).reduce(UInt32(0)) { $0 | UInt32(data[offset + $1]) << (UInt32($1) * 8) }
    }
}
