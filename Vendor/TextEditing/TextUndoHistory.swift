import Foundation

struct TextEditSelection: Equatable {
    var caret: Int
    var anchor: Int?

    var isInsertionPoint: Bool { anchor == nil || anchor == caret }
}

final class TextUndoHistory {
    enum Kind: Equatable {
        case typing, backwardDelete, forwardDelete, delete, paste, cut, replacement, composition

        var name: String {
            switch self {
            case .typing: return "Typing"
            case .backwardDelete, .forwardDelete, .delete: return "Delete"
            case .paste: return "Paste"
            case .cut: return "Cut"
            case .replacement: return "Replace"
            case .composition: return "Typing"
            }
        }
    }

    struct Result {
        var text: String
        var selection: TextEditSelection
    }

    private struct Entry {
        var location: Int
        var removed: String
        var inserted: String
        var before: TextEditSelection
        var after: TextEditSelection
        var kind: Kind
        var time: TimeInterval
        var cost: Int { removed.utf8.count + inserted.utf8.count }
    }

    private var undoEntries: [Entry] = []
    private var redoEntries: [Entry] = []
    private var coalescing = false
    private var composition: Result?
    private var retainedBytes = 0
    let idleInterval: TimeInterval
    let maximumGroups: Int
    let maximumBytes: Int

    init(idleInterval: TimeInterval = 2, maximumGroups: Int = 200, maximumBytes: Int = 16 * 1024 * 1024) {
        self.idleInterval = idleInterval
        self.maximumGroups = max(1, maximumGroups)
        self.maximumBytes = max(0, maximumBytes)
    }

    var canUndo: Bool { !undoEntries.isEmpty }
    var canRedo: Bool { !redoEntries.isEmpty }
    var undoName: String? { undoEntries.last?.kind.name }
    var redoName: String? { redoEntries.last?.kind.name }
    var isComposing: Bool { composition != nil }

    func breakCoalescing() { coalescing = false }

    func reset() {
        undoEntries.removeAll(); redoEntries.removeAll()
        composition = nil; coalescing = false; retainedBytes = 0
    }

    func beginComposition(text: String, selection: TextEditSelection) {
        guard composition == nil else { return }
        breakCoalescing()
        composition = Result(text: text, selection: selection)
    }

    func finishComposition(text: String, selection: TextEditSelection, time: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        guard let start = composition else { return }
        composition = nil
        var oldStart = start.text.startIndex, newStart = text.startIndex
        while oldStart < start.text.endIndex && newStart < text.endIndex,
              String(start.text[oldStart]).utf16.elementsEqual(String(text[newStart]).utf16) {
            oldStart = start.text.index(after: oldStart); newStart = text.index(after: newStart)
        }
        var oldEnd = start.text.endIndex, newEnd = text.endIndex
        while oldEnd > oldStart && newEnd > newStart {
            let oldPrevious = start.text.index(before: oldEnd), newPrevious = text.index(before: newEnd)
            guard String(start.text[oldPrevious]).utf16.elementsEqual(String(text[newPrevious]).utf16) else { break }
            oldEnd = oldPrevious; newEnd = newPrevious
        }
        record(range: NSRange(oldStart..<oldEnd, in: start.text), removed: String(start.text[oldStart..<oldEnd]),
               inserted: String(text[newStart..<newEnd]), before: start.selection, after: selection,
               kind: .composition, time: time)
        breakCoalescing()
    }

    func record(range: NSRange, removed: String, inserted: String, before: TextEditSelection, after: TextEditSelection,
                kind: Kind, time: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        guard composition == nil, !removed.utf16.elementsEqual(inserted.utf16) else { return }
        for entry in redoEntries { retainedBytes -= entry.cost }
        redoEntries.removeAll()
        let entry = Entry(location: range.location, removed: removed, inserted: inserted,
                          before: before, after: after, kind: kind, time: time)
        if coalescing, var last = undoEntries.last, last.kind == kind, last.after == before,
           before.isInsertionPoint, after.isInsertionPoint, time >= last.time, time - last.time <= idleInterval {
            let previousCost = last.cost
            var merged = false
            switch kind {
            case .typing where removed.isEmpty && inserted.count == 1 && !inserted.contains(where: \.isNewline):
                let beginsWord = last.inserted.last?.isWhitespace == true && inserted.first?.isWhitespace == false
                if !beginsWord && range.location == last.location + last.inserted.utf16.count {
                    last.inserted += inserted; merged = true
                }
            case .backwardDelete where inserted.isEmpty && last.inserted.isEmpty:
                if range.location + removed.utf16.count == last.location && !removed.contains(where: \.isNewline) {
                    last.location = range.location; last.removed = removed + last.removed; merged = true
                }
            case .forwardDelete where inserted.isEmpty && last.inserted.isEmpty:
                if range.location == last.location && !removed.contains(where: \.isNewline) {
                    last.removed += removed; merged = true
                }
            default: break
            }
            if merged {
                last.after = after; last.time = time
                undoEntries[undoEntries.count - 1] = last
                retainedBytes += last.cost - previousCost
                prune()
                return
            }
        }
        undoEntries.append(entry); retainedBytes += entry.cost
        coalescing = after.isInsertionPoint &&
            ((kind == .typing && inserted.count == 1 && !inserted.contains(where: \.isNewline)) ||
             (before.isInsertionPoint && (kind == .backwardDelete || kind == .forwardDelete) && inserted.isEmpty && !removed.contains(where: \.isNewline)))
        prune()
    }

    private func prune() {
        while undoEntries.count > maximumGroups || (retainedBytes > maximumBytes && undoEntries.count > 1) {
            retainedBytes -= undoEntries.removeFirst().cost
        }
    }

    func undo(text: String) -> Result? {
        breakCoalescing()
        guard composition == nil, let entry = undoEntries.last,
              let result = apply(entry, to: text, undo: true) else { return nil }
        undoEntries.removeLast(); redoEntries.append(entry)
        return result
    }

    func redo(text: String) -> Result? {
        breakCoalescing()
        guard composition == nil, let entry = redoEntries.last,
              let result = apply(entry, to: text, undo: false) else { return nil }
        redoEntries.removeLast(); undoEntries.append(entry)
        return result
    }

    private func apply(_ entry: Entry, to text: String, undo: Bool) -> Result? {
        let expected = undo ? entry.inserted : entry.removed
        let replacement = undo ? entry.removed : entry.inserted
        let range = NSRange(location: entry.location, length: expected.utf16.count)
        guard let indices = Range(range, in: text), text[indices].utf16.elementsEqual(expected.utf16) else { return nil }
        var updated = text
        updated.replaceSubrange(indices, with: replacement)
        return Result(text: updated, selection: undo ? entry.before : entry.after)
    }
}
