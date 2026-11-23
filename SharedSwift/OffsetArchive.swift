import Foundation

enum OffsetArchiveError: Error, LocalizedError {
    case invalidFormat
    case unsupportedValue

    var errorDescription: String? {
        switch self {
        case .invalidFormat:
            return "The binary catalog resource is invalid."
        case .unsupportedValue:
            return "The binary catalog contains an unsupported value."
        }
    }
}

enum OffsetArchive {
    private static let headerSize = 16
    private static let nodeSize = 24
    private static let maximumDepth = 128

    static func encode<Value: Encodable>(_ value: Value) throws -> Data {
        let encoder = PropertyListEncoder()
        encoder.outputFormat = .binary
        let propertyListData = try encoder.encode(value)
        let propertyList = try PropertyListSerialization.propertyList(
            from: propertyListData,
            options: [],
            format: nil
        )
        return try encodePropertyList(propertyList)
    }

    static func encodePropertyList(_ propertyList: Any) throws -> Data {
        var writer = Writer()
        let rootOffset = try writer.append(propertyList, depth: 0)
        writer.data.replaceSubrange(8..<16, with: littleEndianBytes(rootOffset))
        return writer.data
    }

    static func decode<Value: Decodable>(_ type: Value.Type, from data: Data) throws -> Value {
        let propertyList = try decodePropertyList(data)
        let propertyListData = try PropertyListSerialization.data(
            fromPropertyList: propertyList,
            format: .binary,
            options: 0
        )
        return try PropertyListDecoder().decode(type, from: propertyListData)
    }

    static func decodePropertyList(_ data: Data) throws -> Any {
        try Reader(data: Data(data)).root()
    }

    private static func littleEndianBytes<Integer: FixedWidthInteger>(_ value: Integer) -> [UInt8] {
        withUnsafeBytes(of: value.littleEndian) { Array($0) }
    }

    private struct Writer {
        var data: Data = {
            var value = Data("ORWV".utf8)
            value.append(contentsOf: OffsetArchive.littleEndianBytes(UInt32(1)))
            value.append(contentsOf: OffsetArchive.littleEndianBytes(UInt64(0)))
            return value
        }()

        mutating func append(_ value: Any, depth: Int) throws -> UInt64 {
            guard depth < OffsetArchive.maximumDepth else {
                throw OffsetArchiveError.unsupportedValue
            }
            if let dictionary = value as? [String: Any] {
                var entries: [(UInt64, UInt64)] = []
                let keys = dictionary.keys.sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
                for key in keys {
                    guard let item = dictionary[key] else {
                        throw OffsetArchiveError.invalidFormat
                    }
                    entries.append((try append(key, depth: depth + 1), try append(item, depth: depth + 1)))
                }
                let payloadOffset = UInt64(data.count)
                for entry in entries {
                    appendInteger(entry.0)
                    appendInteger(entry.1)
                }
                return appendNode(type: 8, value: payloadOffset, count: UInt64(entries.count))
            }
            if let array = value as? [Any] {
                let offsets = try array.map { try append($0, depth: depth + 1) }
                let payloadOffset = UInt64(data.count)
                for offset in offsets {
                    appendInteger(offset)
                }
                return appendNode(type: 7, value: payloadOffset, count: UInt64(offsets.count))
            }
            if let string = value as? String {
                let bytes = Data(string.utf8)
                let payloadOffset = UInt64(data.count)
                data.append(bytes)
                return appendNode(type: 5, value: payloadOffset, count: UInt64(bytes.count))
            }
            if let bytes = value as? Data {
                let payloadOffset = UInt64(data.count)
                data.append(bytes)
                return appendNode(type: 6, value: payloadOffset, count: UInt64(bytes.count))
            }
            if let date = value as? Date {
                return appendNode(type: 9,
                                  value: date.timeIntervalSince1970.bitPattern,
                                  count: 0)
            }
            if let number = value as? NSNumber {
                if CFGetTypeID(number) == CFBooleanGetTypeID() {
                    return appendNode(type: 2, value: number.boolValue ? 1 : 0, count: 0)
                }
                let encoding = String(cString: number.objCType)
                if "csilqCSILQ".contains(encoding) {
                    return appendNode(type: 3,
                                      value: UInt64(bitPattern: number.int64Value),
                                      count: 0)
                }
                return appendNode(type: 4, value: number.doubleValue.bitPattern, count: 0)
            }
            throw OffsetArchiveError.unsupportedValue
        }

        private mutating func appendInteger(_ value: UInt64) {
            data.append(contentsOf: OffsetArchive.littleEndianBytes(value))
        }

        private mutating func appendNode(type: UInt8,
                                         value: UInt64,
                                         count: UInt64) -> UInt64 {
            let offset = UInt64(data.count)
            data.append(type)
            data.append(contentsOf: [UInt8](repeating: 0, count: 7))
            appendInteger(value)
            appendInteger(count)
            return offset
        }
    }

    private struct Reader {
        let data: Data

        func root() throws -> Any {
            guard data.count >= OffsetArchive.headerSize,
                  data.prefix(8) == Data([0x4f, 0x52, 0x57, 0x56, 1, 0, 0, 0]) else {
                throw OffsetArchiveError.invalidFormat
            }
            return try value(at: integer(at: 8), depth: 0)
        }

        private func integer(at offset: Int) -> UInt64 {
            var value: UInt64 = 0
            for index in 0..<8 {
                value |= UInt64(data[offset + index]) << (UInt64(index) * 8)
            }
            return value
        }

        private func value(at rawOffset: UInt64, depth: Int) throws -> Any {
            guard depth < OffsetArchive.maximumDepth,
                  rawOffset <= UInt64(Int.max) else {
                throw OffsetArchiveError.invalidFormat
            }
            let offset = Int(rawOffset)
            guard offset >= OffsetArchive.headerSize,
                  offset <= data.count,
                  OffsetArchive.nodeSize <= data.count - offset else {
                throw OffsetArchiveError.invalidFormat
            }
            let type = data[offset]
            let payload = integer(at: offset + 8)
            let count = integer(at: offset + 16)
            switch type {
            case 2:
                guard payload <= 1 else {
                    throw OffsetArchiveError.invalidFormat
                }
                return payload != 0
            case 3:
                return NSNumber(value: Int64(bitPattern: payload))
            case 4:
                return NSNumber(value: Double(bitPattern: payload))
            case 5:
                let bytes = try referencedData(offset: payload, length: count)
                guard let text = String(data: bytes, encoding: .utf8) else {
                    throw OffsetArchiveError.invalidFormat
                }
                return text
            case 6:
                return try referencedData(offset: payload, length: count)
            case 7:
                let (arrayOffset, arrayCount) = try table(offset: payload, count: count, stride: 8)
                return try (0..<arrayCount).map {
                    try value(at: integer(at: arrayOffset + $0 * 8), depth: depth + 1)
                }
            case 8:
                let (dictionaryOffset, dictionaryCount) = try table(offset: payload, count: count, stride: 16)
                var dictionary: [String: Any] = [:]
                for index in 0..<dictionaryCount {
                    let entryOffset = dictionaryOffset + index * 16
                    guard let key = try value(at: integer(at: entryOffset), depth: depth + 1) as? String,
                          dictionary[key] == nil else {
                        throw OffsetArchiveError.invalidFormat
                    }
                    dictionary[key] = try value(at: integer(at: entryOffset + 8), depth: depth + 1)
                }
                return dictionary
            case 9:
                return Date(timeIntervalSince1970: Double(bitPattern: payload))
            default:
                throw OffsetArchiveError.invalidFormat
            }
        }

        private func table(offset: UInt64, count: UInt64, stride: Int) throws -> (Int, Int) {
            guard offset <= UInt64(data.count), count <= UInt64(Int.max) else {
                throw OffsetArchiveError.invalidFormat
            }
            let start = Int(offset)
            let entries = Int(count)
            guard entries <= (data.count - start) / stride else {
                throw OffsetArchiveError.invalidFormat
            }
            return (start, entries)
        }

        private func referencedData(offset: UInt64, length: UInt64) throws -> Data {
            guard offset <= UInt64(data.count), length <= UInt64(data.count) else {
                throw OffsetArchiveError.invalidFormat
            }
            let start = Int(offset)
            let count = Int(length)
            guard count <= data.count - start else {
                throw OffsetArchiveError.invalidFormat
            }
            return data.subdata(in: start..<(start + count))
        }
    }
}
