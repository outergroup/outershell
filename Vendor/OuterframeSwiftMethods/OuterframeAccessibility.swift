import Foundation
import CoreGraphics

/// Represents the role of an accessibility element exposed by outerframe content.
public enum OuterframeAccessibilityRole: UInt8, Sendable {
    case container = 0
    case staticText = 1
    case button = 2
    case image = 3
    case table = 4
    case row = 5
    case cell = 6
    case textField = 7
    case scrollArea = 8
    case textArea = 9
    case dialog = 10
    case outline = 11
    case slider = 12
}

/// Actions are handled by content against the current node identifier, never stale screen coordinates.
public enum OuterframeAccessibilityAction: UInt8, Sendable {
    case press = 0, showMenu, focus, setValue, scrollUp, scrollDown, moveEarlier, moveLater, setSelectedTextRange, increment, decrement, expand, collapse
}

public struct OuterframeAccessibilityActions: OptionSet, Sendable {
    public let rawValue: UInt16
    public init(rawValue: UInt16) { self.rawValue = rawValue }
    public static let press = Self(rawValue: 1 << 0)
    public static let showMenu = Self(rawValue: 1 << 1)
    public static let focus = Self(rawValue: 1 << 2)
    public static let setValue = Self(rawValue: 1 << 3)
    public static let scrollUp = Self(rawValue: 1 << 4)
    public static let scrollDown = Self(rawValue: 1 << 5)
    public static let moveEarlier = Self(rawValue: 1 << 6)
    public static let moveLater = Self(rawValue: 1 << 7)
    public static let setSelectedTextRange = Self(rawValue: 1 << 8)
    public static let increment = Self(rawValue: 1 << 9)
    public static let decrement = Self(rawValue: 1 << 10)
    public static let expand = Self(rawValue: 1 << 11)
    public static let collapse = Self(rawValue: 1 << 12)
    public func supports(_ action: OuterframeAccessibilityAction) -> Bool {
        rawValue & (1 << action.rawValue) != 0
    }
}

public enum OuterframeAccessibilitySortDirection: UInt8, Sendable {
    case none = 0, ascending, descending
}

/// Represents notifications that the outerframe content can request the host to post.
public struct OuterframeAccessibilityNotification: OptionSet, Sendable {
    public let rawValue: UInt8

    public init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    public static let layoutChanged = OuterframeAccessibilityNotification(rawValue: 1 << 0)
    public static let selectedChildrenChanged = OuterframeAccessibilityNotification(rawValue: 1 << 1)
    public static let focusedElementChanged = OuterframeAccessibilityNotification(rawValue: 1 << 2)
}

/// A node within the content-provided accessibility tree.
public struct OuterframeAccessibilityNode: Sendable {
    public var identifier: UInt32
    public var role: OuterframeAccessibilityRole
    public var frame: CGRect
    public var label: String?
    public var value: String?
    public var hint: String?
    public var children: [OuterframeAccessibilityNode]
    /// For table roles: the total number of rows (may exceed children count for virtualized tables)
    public var rowCount: Int?
    /// For table roles: the number of columns
    public var columnCount: Int?
    /// Whether the element is enabled (default true)
    public var actions: OuterframeAccessibilityActions
    public var isFocused: Bool
    public var isSelected: Bool
    public var rowIndex: Int?
    public var columnIndex: Int?
    public var sortDirection: OuterframeAccessibilitySortDirection
    public var isEnabled: Bool
    public var selectedTextRange: NSRange?
    public var supportsActionSnapshot: Bool
    public var supportsTextGeometry: Bool
    public var isExpanded: Bool?
    public var disclosureLevel: Int
    public var minimumValue: Double?
    public var maximumValue: Double?

    public init(identifier: UInt32,
                role: OuterframeAccessibilityRole,
                frame: CGRect,
                label: String? = nil,
                value: String? = nil,
                hint: String? = nil,
                children: [OuterframeAccessibilityNode] = [],
                rowCount: Int? = nil,
                columnCount: Int? = nil,
                isEnabled: Bool = true,
                actions: OuterframeAccessibilityActions = [],
                isFocused: Bool = false,
                isSelected: Bool = false,
                rowIndex: Int? = nil,
                columnIndex: Int? = nil,
                sortDirection: OuterframeAccessibilitySortDirection = .none,
                selectedTextRange: NSRange? = nil,
                supportsTextGeometry: Bool = false, supportsActionSnapshot: Bool = false, isExpanded: Bool? = nil, disclosureLevel: Int = 0,
                minimumValue: Double? = nil, maximumValue: Double? = nil) {
        self.supportsActionSnapshot = supportsActionSnapshot
        self.supportsTextGeometry = supportsTextGeometry
        self.isExpanded = isExpanded
        self.disclosureLevel = disclosureLevel
        self.minimumValue = minimumValue
        self.maximumValue = maximumValue
        self.selectedTextRange = selectedTextRange
        self.identifier = identifier
        self.role = role
        self.frame = frame
        self.label = label
        self.value = value
        self.hint = hint
        self.children = children
        self.rowCount = rowCount
        self.columnCount = columnCount
        self.isEnabled = isEnabled
        self.actions = actions
        self.isFocused = isFocused
        self.isSelected = isSelected
        self.rowIndex = rowIndex
        self.columnIndex = columnIndex
        self.sortDirection = sortDirection
    }
}

/// A snapshot describing all accessibility elements exposed by the outerframe content.
public struct OuterframeAccessibilitySnapshot: Sendable {
    public var rootNodes: [OuterframeAccessibilityNode]

    public init(rootNodes: [OuterframeAccessibilityNode]) {
        self.rootNodes = rootNodes
    }

    /// Convenience helper for content that has not implemented accessibility yet.
    public static func notImplementedSnapshot(message: String = "Accessibility not implemented") -> OuterframeAccessibilitySnapshot {
        let child = OuterframeAccessibilityNode(identifier: 1,
                                                role: .staticText,
                                                frame: .zero,
                                                label: message)
        let root = OuterframeAccessibilityNode(identifier: 0,
                                               role: .container,
                                               frame: .zero,
                                               children: [child])
        return OuterframeAccessibilitySnapshot(rootNodes: [root])
    }

    /// Serialises the snapshot to the binary format shared with the host.
    public func serializedData() -> Data {
        let flattenedNodes = flattenedNodes()
        let nodeRecordsOffset = Self.headerSize
        let nodeRecordsSize = flattenedNodes.count * Self.nodeRecordSize
        let variableDataOffset = nodeRecordsOffset + nodeRecordsSize

        var stringData = Data()
        var nodeRecords = Data(capacity: nodeRecordsSize)
        for flattenedNode in flattenedNodes {
            encode(flattenedNode: flattenedNode,
                   variableDataOffset: variableDataOffset,
                   stringData: &stringData,
                   nodeRecords: &nodeRecords)
        }

        var data = Data(capacity: Self.headerSize + nodeRecords.count + stringData.count)
        data.appendUInt32(Self.formatVersion)
        data.appendUInt32(UInt32(Self.nodeRecordSize))
        data.appendUInt32(UInt32(nodeRecordsOffset))
        data.appendUInt32(UInt32(nodeRecords.count))
        data.append(nodeRecords)
        data.append(stringData)
        return data
    }

    /// Deserialises a snapshot from binary payload produced by `serializedData`.
    public static func deserialize(from data: Data) -> OuterframeAccessibilitySnapshot? {
        guard data.count >= headerSize,
              let version = data.readUInt32(at: 0),
              version == formatVersion,
              let nodeRecordSizeRaw = data.readUInt32(at: 4),
              let nodeRecordsOffsetRaw = data.readUInt32(at: 8),
              let nodeRecordsLengthRaw = data.readUInt32(at: 12),
              nodeRecordSizeRaw >= minimumNodeRecordSize,
              let nodeRecordSize = Int(exactly: nodeRecordSizeRaw),
              let nodeRecordsOffset = Int(exactly: nodeRecordsOffsetRaw),
              let nodeRecordsLength = Int(exactly: nodeRecordsLengthRaw),
              nodeRecordsLength.isMultiple(of: nodeRecordSize) else {
            return nil
        }
        let nodeCount = nodeRecordsLength / nodeRecordSize
        guard nodeCount <= maximumNodeCount else {
            return nil
        }

        let nodeRecordsEnd = nodeRecordsOffset.addingReportingOverflow(nodeRecordsLength)
        guard !nodeRecordsEnd.overflow,
              nodeRecordsOffset >= headerSize,
              nodeRecordsEnd.partialValue <= data.count else {
            return nil
        }

        var nodes: [OuterframeAccessibilityNode] = []
        var parentIndices: [UInt32] = []
        var identifiers: Set<UInt32> = []
        nodes.reserveCapacity(nodeCount)
        parentIndices.reserveCapacity(nodeCount)

        for index in 0..<nodeCount {
            let offset = nodeRecordsOffset + index * nodeRecordSize
            guard let decoded = decodeNodeRecord(from: data,
                                                 offset: offset,
                                                 recordSize: nodeRecordSize,
                                                 variableDataOffset: nodeRecordsEnd.partialValue) else {
                return nil
            }
            guard identifiers.insert(decoded.node.identifier).inserted else { return nil }
            if decoded.parentIndex != UInt32.max && decoded.parentIndex >= UInt32(index) {
                return nil
            }
            nodes.append(decoded.node)
            parentIndices.append(decoded.parentIndex)
        }

        var childIndices = Array(repeating: [Int](), count: nodeCount)
        var rootIndices: [Int] = []
        for (index, parentIndex) in parentIndices.enumerated() {
            if parentIndex == UInt32.max {
                rootIndices.append(index)
            } else {
                childIndices[Int(parentIndex)].append(index)
            }
        }

        if nodeCount > 0 {
            for index in stride(from: nodeCount - 1, through: 0, by: -1) {
                nodes[index].children = childIndices[index].map { nodes[$0] }
            }
        }

        return OuterframeAccessibilitySnapshot(rootNodes: rootIndices.map { nodes[$0] })
    }
}

// MARK: - Binary helpers

private struct FlattenedAccessibilityNode {
    let node: OuterframeAccessibilityNode
    let parentIndex: UInt32
}

private extension OuterframeAccessibilitySnapshot {
    static let formatVersion: UInt32 = 1
    static let headerSize = 16
    static let nodeRecordSize = 114
    static let minimumNodeRecordSize: UInt32 = 74
    static let maximumNodeCount = 100_000

    static let stringLabelFlag: UInt8 = 1 << 0
    static let stringValueFlag: UInt8 = 1 << 1
    static let stringHintFlag: UInt8 = 1 << 2
    static let rowCountFlag: UInt8 = 1 << 3
    static let columnCountFlag: UInt8 = 1 << 4
    static let enabledFlag: UInt8 = 1 << 5

    struct DecodedNodeRecord {
        let node: OuterframeAccessibilityNode
        let parentIndex: UInt32
    }

    func flattenedNodes() -> [FlattenedAccessibilityNode] {
        var result: [FlattenedAccessibilityNode] = []
        var stack: [(node: OuterframeAccessibilityNode, parentIndex: UInt32)] = rootNodes.reversed().map {
            (node: $0, parentIndex: UInt32.max)
        }

        while let (node, parentIndex) = stack.popLast() {
            if result.count >= Self.maximumNodeCount { break }
            let currentIndex = UInt32(result.count)
            result.append(FlattenedAccessibilityNode(node: node, parentIndex: parentIndex))
            for child in node.children.reversed() {
                stack.append((node: child, parentIndex: currentIndex))
            }
        }

        return result
    }

    func encode(flattenedNode: FlattenedAccessibilityNode,
                variableDataOffset: Int,
                stringData: inout Data,
                nodeRecords: inout Data) {
        var flags: UInt8 = flattenedNode.node.isEnabled ? Self.enabledFlag : 0
        let labelReference = appendStringReference(flattenedNode.node.label,
                                                   variableDataOffset: variableDataOffset,
                                                   stringData: &stringData)
        let valueReference = appendStringReference(flattenedNode.node.value,
                                                   variableDataOffset: variableDataOffset,
                                                   stringData: &stringData)
        let hintReference = appendStringReference(flattenedNode.node.hint,
                                                  variableDataOffset: variableDataOffset,
                                                  stringData: &stringData)
        if flattenedNode.node.isSelected { flags |= 1 << 7 }
        if flattenedNode.node.isFocused { flags |= 1 << 6 }
        if labelReference != nil { flags |= Self.stringLabelFlag }
        if valueReference != nil { flags |= Self.stringValueFlag }
        if hintReference != nil { flags |= Self.stringHintFlag }
        if flattenedNode.node.rowCount != nil { flags |= Self.rowCountFlag }
        if flattenedNode.node.columnCount != nil { flags |= Self.columnCountFlag }

        nodeRecords.appendUInt32(flattenedNode.node.identifier)
        nodeRecords.appendUInt32(flattenedNode.parentIndex)
        nodeRecords.appendFloat64(flattenedNode.node.frame.origin.x)
        nodeRecords.appendFloat64(flattenedNode.node.frame.origin.y)
        nodeRecords.appendFloat64(flattenedNode.node.frame.size.width)
        nodeRecords.appendFloat64(flattenedNode.node.frame.size.height)
        nodeRecords.appendStringReference(labelReference)
        nodeRecords.appendStringReference(valueReference)
        nodeRecords.appendStringReference(hintReference)
        nodeRecords.appendInt32(Int32(clamping: flattenedNode.node.rowCount ?? 0))
        nodeRecords.appendInt32(Int32(clamping: flattenedNode.node.columnCount ?? 0))
        nodeRecords.appendUInt8(flattenedNode.node.role.rawValue)
        nodeRecords.appendUInt8(flags)
        nodeRecords.appendUInt8(UInt8(truncatingIfNeeded: flattenedNode.node.actions.rawValue))
        nodeRecords.appendInt32(Int32(clamping: flattenedNode.node.rowIndex ?? -1))
        nodeRecords.appendInt32(Int32(clamping: flattenedNode.node.columnIndex ?? -1))
        nodeRecords.appendUInt8(flattenedNode.node.sortDirection.rawValue)
        nodeRecords.appendUInt8(UInt8(truncatingIfNeeded: flattenedNode.node.actions.rawValue >> 8))
        nodeRecords.appendUInt32(flattenedNode.node.selectedTextRange.map { UInt32(clamping: $0.location) } ?? UInt32.max)
        nodeRecords.appendUInt32(flattenedNode.node.selectedTextRange.map { UInt32(clamping: $0.length) } ?? 0)
        let node = flattenedNode.node
        var extraFlags: UInt8 = node.supportsTextGeometry ? 1 : 0
        if node.supportsActionSnapshot { extraFlags |= 32 }
        if node.isExpanded != nil { extraFlags |= 2 }
        if node.isExpanded == true { extraFlags |= 4 }
        if node.minimumValue != nil { extraFlags |= 8 }
        if node.maximumValue != nil { extraFlags |= 16 }
        nodeRecords.appendUInt8(extraFlags)
        nodeRecords.appendInt32(Int32(clamping: node.disclosureLevel))
        nodeRecords.appendFloat64(node.minimumValue ?? 0)
        nodeRecords.appendFloat64(node.maximumValue ?? 0)
    }

    func appendStringReference(_ string: String?,
                               variableDataOffset: Int,
                               stringData: inout Data) -> (offset: UInt32, length: UInt32)? {
        guard let string else { return nil }
        let encoded = Data(string.utf8)
        let offset = variableDataOffset + stringData.count
        guard let offset = UInt32(exactly: offset),
              let length = UInt32(exactly: encoded.count) else {
            preconditionFailure("Accessibility snapshot is too large to serialize")
        }
        stringData.append(encoded)
        return (offset, length)
    }

    static func decodeNodeRecord(from data: Data,
                                 offset: Int,
                                 recordSize: Int,
                                 variableDataOffset: Int) -> DecodedNodeRecord? {
        guard let identifier = data.readUInt32(at: offset),
              let parentIndex = data.readUInt32(at: offset + 4),
              let originX = data.readFloat64(at: offset + 8),
              let originY = data.readFloat64(at: offset + 16),
              let width = data.readFloat64(at: offset + 24),
              let height = data.readFloat64(at: offset + 32),
              let labelOffset = data.readUInt32(at: offset + 40),
              let labelLength = data.readUInt32(at: offset + 44),
              let valueOffset = data.readUInt32(at: offset + 48),
              let valueLength = data.readUInt32(at: offset + 52),
              let hintOffset = data.readUInt32(at: offset + 56),
              let hintLength = data.readUInt32(at: offset + 60),
              let rowCountRaw = data.readInt32(at: offset + 64),
              let columnCountRaw = data.readInt32(at: offset + 68),
              let roleRaw = data.readUInt8(at: offset + 72),
              let flags = data.readUInt8(at: offset + 73),
              let role = OuterframeAccessibilityRole(rawValue: roleRaw) else {
            return nil
        }

        if flags & stringLabelFlag == 0 && (labelOffset != 0 || labelLength != 0) { return nil }
        if flags & stringValueFlag == 0 && (valueOffset != 0 || valueLength != 0) { return nil }
        if flags & stringHintFlag == 0 && (hintOffset != 0 || hintLength != 0) { return nil }
        if flags & rowCountFlag == 0 && rowCountRaw != 0 { return nil }
        if flags & columnCountFlag == 0 && columnCountRaw != 0 { return nil }

        let label = flags & stringLabelFlag == 0 ? nil : readString(from: data,
                                                                    offset: labelOffset,
                                                                    length: labelLength,
                                                                    variableDataOffset: variableDataOffset)
        let value = flags & stringValueFlag == 0 ? nil : readString(from: data,
                                                                    offset: valueOffset,
                                                                    length: valueLength,
                                                                    variableDataOffset: variableDataOffset)
        let hint = flags & stringHintFlag == 0 ? nil : readString(from: data,
                                                                  offset: hintOffset,
                                                                  length: hintLength,
                                                                  variableDataOffset: variableDataOffset)

        if flags & stringLabelFlag != 0 && label == nil { return nil }
        if flags & stringValueFlag != 0 && value == nil { return nil }
        if flags & stringHintFlag != 0 && hint == nil { return nil }

        let rowIndex = recordSize >= 79 ? Int(data.readInt32(at: offset + 75) ?? -1) : -1
        let columnIndex = recordSize >= 83 ? Int(data.readInt32(at: offset + 79) ?? -1) : -1
        guard rowIndex >= -1, columnIndex >= -1,
              let sortDirection = OuterframeAccessibilitySortDirection(rawValue: recordSize >= 84 ? (data.readUInt8(at: offset + 83) ?? 0) : 0) else { return nil }
        var selectedTextRange: NSRange?
        if recordSize >= 93, let location = data.readUInt32(at: offset + 85), let length = data.readUInt32(at: offset + 89) {
            if location != UInt32.max {
                let textLength = (value ?? "").utf16.count
                guard Int(location) <= textLength, Int(length) <= textLength - Int(location) else { return nil }
                let range = NSRange(location: Int(location), length: Int(length))
                guard OuterframeAccessibilityTextRange.isValid(range, in: value ?? "") else { return nil }
                selectedTextRange = range
            } else if length != 0 { return nil }
        }
        let actionMask = UInt16(recordSize > 74 ? (data.readUInt8(at: offset + 74) ?? 0) : 0)
            | (UInt16(recordSize > 84 ? (data.readUInt8(at: offset + 84) ?? 0) : 0) << 8)
        let extraFlags = recordSize >= 114 ? (data.readUInt8(at: offset + 93) ?? 0) : 0
        let disclosureLevel = recordSize >= 114 ? Int(data.readInt32(at: offset + 94) ?? 0) : 0
        let minimumValue = extraFlags & 8 != 0 ? data.readFloat64(at: offset + 98) : nil
        let maximumValue = extraFlags & 16 != 0 ? data.readFloat64(at: offset + 106) : nil
        guard disclosureLevel >= 0, minimumValue?.isFinite != false, maximumValue?.isFinite != false else { return nil }
        if let minimumValue, let maximumValue, minimumValue > maximumValue { return nil }
        let node = OuterframeAccessibilityNode(identifier: identifier,
                                               role: role,
                                               frame: CGRect(x: originX, y: originY, width: width, height: height),
                                               label: label,
                                               value: value,
                                               hint: hint,
                                               rowCount: flags & rowCountFlag == 0 ? nil : Int(rowCountRaw),
                                               columnCount: flags & columnCountFlag == 0 ? nil : Int(columnCountRaw),
                                               isEnabled: flags & enabledFlag != 0,
                                               actions: .init(rawValue: actionMask),
                                               isFocused: flags & (1 << 6) != 0,
                                               isSelected: flags & (1 << 7) != 0,
                                               rowIndex: rowIndex < 0 ? nil : rowIndex,
                                               columnIndex: columnIndex < 0 ? nil : columnIndex,
                                               sortDirection: sortDirection, selectedTextRange: selectedTextRange,
                                               supportsTextGeometry: extraFlags & 1 != 0, supportsActionSnapshot: extraFlags & 32 != 0,
                                               isExpanded: extraFlags & 2 == 0 ? nil : extraFlags & 4 != 0,
                                               disclosureLevel: disclosureLevel, minimumValue: minimumValue.map(Double.init), maximumValue: maximumValue.map(Double.init))
        return DecodedNodeRecord(node: node, parentIndex: parentIndex)
    }

    static func readString(from data: Data,
                           offset: UInt32,
                           length: UInt32,
                           variableDataOffset: Int) -> String? {
        let start = Int(offset)
        let byteCount = Int(length)
        guard start >= variableDataOffset,
              start <= data.count,
              byteCount <= data.count - start else {
            return nil
        }
        return String(data: data[start..<(start + byteCount)], encoding: .utf8)
    }
}

private extension Data {
    mutating func appendUInt8(_ value: UInt8) {
        append(value)
    }

    mutating func appendUInt32(_ value: UInt32) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }

    mutating func appendUInt64(_ value: UInt64) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }

    mutating func appendFloat64(_ value: CGFloat) {
        appendUInt64(Double(value).bitPattern)
    }

    mutating func appendInt32(_ value: Int32) {
        appendUInt32(UInt32(bitPattern: value))
    }

    mutating func appendStringReference(_ reference: (offset: UInt32, length: UInt32)?) {
        appendUInt32(reference?.offset ?? 0)
        appendUInt32(reference?.length ?? 0)
    }

    func readUInt8(at offset: Int) -> UInt8? {
        guard offset >= 0, offset < count else { return nil }
        return self[offset]
    }

    func readUInt32(at offset: Int) -> UInt32? {
        guard offset >= 0, offset + 4 <= count else { return nil }
        let b0 = UInt32(self[offset])
        let b1 = UInt32(self[offset + 1])
        let b2 = UInt32(self[offset + 2])
        let b3 = UInt32(self[offset + 3])
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
    }

    func readUInt64(at offset: Int) -> UInt64? {
        guard offset >= 0, offset + 8 <= count else { return nil }
        let b0 = UInt64(self[offset])
        let b1 = UInt64(self[offset + 1])
        let b2 = UInt64(self[offset + 2])
        let b3 = UInt64(self[offset + 3])
        let b4 = UInt64(self[offset + 4])
        let b5 = UInt64(self[offset + 5])
        let b6 = UInt64(self[offset + 6])
        let b7 = UInt64(self[offset + 7])
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24) | (b4 << 32) | (b5 << 40) | (b6 << 48) | (b7 << 56)
    }

    func readInt32(at offset: Int) -> Int32? {
        guard let value = readUInt32(at: offset) else { return nil }
        return Int32(bitPattern: value)
    }

    func readFloat64(at offset: Int) -> CGFloat? {
        guard let bits = readUInt64(at: offset) else { return nil }
        return CGFloat(Double(bitPattern: bits))
    }
}

/// Geometry is queried on demand in content-root coordinates. Text offsets use UTF-16.
public enum OuterframeAccessibilityTextQuery: UInt8, Sendable {
    case frameForRange = 0, rangeForPosition, lineForIndex, rangeForLine, visibleRange
}

public struct OuterframeAccessibilityTextResult: Sendable {
    public var range: NSRange
    public var frame: CGRect
    public var index: Int
    public init(range: NSRange = NSRange(location: 0, length: 0), frame: CGRect = .zero, index: Int = 0) {
        self.range = range
        self.frame = frame
        self.index = index
    }
}

public enum OuterframeAccessibilityTextRange {
    public static func isValid(_ range: NSRange, in text: String) -> Bool {
        let value = text as NSString
        guard range.location >= 0, range.length >= 0, range.location <= value.length,
              range.length <= value.length - range.location else { return false }
        func isBoundary(_ index: Int) -> Bool {
            guard index > 0, index < value.length else { return true }
            return !(0xDC00...0xDFFF).contains(value.character(at: index))
                || !(0xD800...0xDBFF).contains(value.character(at: index - 1))
        }
        return isBoundary(range.location) && isBoundary(NSMaxRange(range))
    }
}
