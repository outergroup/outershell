import Foundation
struct OuterframeEditCommandSet: OptionSet {
    let rawValue: UInt32
    static let copy = Self(rawValue: 1)
    static let cut = Self(rawValue: 2)
    static let paste = Self(rawValue: 4)
    static let selectAll = Self(rawValue: 8)
    static let undo = Self(rawValue: 128)
    static let redo = Self(rawValue: 256)
}
