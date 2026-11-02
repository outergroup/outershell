import Foundation
import AppKit
import CoreText

@MainActor
protocol SingleLineTextInputControllerDelegate: AnyObject {
    func textInputControllerDidChangeState()
}

@MainActor
final class SingleLineTextInputController<DelegateClass: SingleLineTextInputControllerDelegate> {
    let identifier: UUID
    weak var delegate: DelegateClass?
    var onSubmit: (() -> Void)?
    var allowsNewlines = false
    var visualLineWidth: CGFloat?
    /// Makes word-based editing follow a masked display value instead of the underlying text.
    var masksWordBoundaries = false

    private var undoHistory = TextUndoHistory()
    private var savedHistories: [String: (String, TextUndoHistory)] = [:]
    private var historyKey: String?

    var markedRange: Range<Int>? { markedTextRange }

    private var undoSelection: TextEditSelection {
        TextEditSelection(caret: stringIndex(forCharacterIndex: cursorPosition).utf16Offset(in: text),
                          anchor: selectionAnchor.map { stringIndex(forCharacterIndex: $0).utf16Offset(in: text) })
    }

    func breakUndoCoalescing() {
        undoHistory.finishComposition(text: text, selection: undoSelection)
        undoHistory.breakCoalescing()
        markedTextRange = nil
        pendingMarkedTextRange = nil
    }

    func resetUndoHistory() {
        undoHistory.reset()
        savedHistories.removeAll()
        historyKey = nil
    }

    func setText(_ newText: String, historyKey key: String) {
        breakUndoCoalescing()
        if let historyKey { savedHistories[historyKey] = (text, undoHistory) }
        let history = savedHistories[key]
        setText(newText)
        undoHistory = history.flatMap { $0.0.utf16.elementsEqual(newText.utf16) ? $0.1 : nil } ?? TextUndoHistory()
        historyKey = key
    }

    func pasteText(_ value: String) {
        guard isFocused else { return }
        breakUndoCoalescing()
        replaceText(range: selectionRange ?? cursorPosition..<cursorPosition, with: value, kind: .paste)
        breakUndoCoalescing()
    }

    private func applyUndo(redo: Bool) {
        breakUndoCoalescing()
        guard let result = redo ? undoHistory.redo(text: text) : undoHistory.undo(text: text) else { return }
        text = result.text
        cursorPosition = characterIndexForUTF16(result.selection.caret)
        selectionAnchor = result.selection.anchor.map { characterIndexForUTF16($0) }
        notifyStateChanged()
    }

    private(set) var text: String
    private(set) var cursorPosition: Int
    private var selectionAnchor: Int?
    private(set) var isFocused: Bool
    private var markedTextRange: Range<Int>?
    private var pendingMarkedTextRange: Range<Int>?
    private let acceptedPasteboardTypeIdentifiers: [String]

    init(identifier: UUID,
         initialText: String = "",
         acceptedPasteboardTypeIdentifiers: [String] = [NSPasteboard.PasteboardType.string.rawValue]) {
        self.identifier = identifier
        self.text = initialText
        self.cursorPosition = initialText.count
        self.selectionAnchor = nil
        self.isFocused = false
        self.acceptedPasteboardTypeIdentifiers = acceptedPasteboardTypeIdentifiers
    }

    var selectionRange: Range<Int>? {
        guard let anchor = selectionAnchor, anchor != cursorPosition else { return nil }
        if anchor < cursorPosition {
            return anchor..<cursorPosition
        } else {
            return cursorPosition..<anchor
        }
    }

    var hasSelection: Bool {
        selectionRange != nil
    }

    func focus(selectAll: Bool = false) {
        guard !isFocused else {
            if selectAll {
                self.selectAll()
            }
            return
        }
        isFocused = true
        if selectAll {
            selectionAnchor = 0
            cursorPosition = text.count
        } else {
            selectionAnchor = nil
        }
        notifyStateChanged()
    }

    func blur() {
        guard isFocused else { return }
        breakUndoCoalescing()
        isFocused = false
        selectionAnchor = nil
        markedTextRange = nil
        pendingMarkedTextRange = nil
        notifyStateChanged()
    }

    func setText(_ newText: String) {
        undoHistory = TextUndoHistory()
        historyKey = nil
        text = newText
        cursorPosition = min(cursorPosition, text.count)
        selectionAnchor = nil
        markedTextRange = nil
        pendingMarkedTextRange = nil
        notifyStateChanged()
    }

    func insertText(_ value: String, replacementRange: Range<Int>? = nil) {
        guard isFocused else { return }
        if let replacementRange {
            replaceText(range: replacementRange, with: value)
            return
        }
        switch value {
        case "\u{8}": deleteBackward(); return
        case "\u{7f}": deleteForward(); return
        case "\u{2190}": moveCursorLeft(); return
        case "\u{2192}": moveCursorRight(); return
        case "\n" where !allowsNewlines, "\r" where !allowsNewlines:
            breakUndoCoalescing(); onSubmit?(); return
        default: break
        }
        let range = markedTextRange ?? pendingMarkedTextRange ?? selectionRange ?? cursorPosition..<cursorPosition
        if value == "\t" { breakUndoCoalescing() }
        replaceText(range: range, with: value, kind: .typing)
        if value == "\t" { breakUndoCoalescing() }
    }

    func replaceText(range: Range<Int>, with replacement: String, clearsMarkedText: Bool = true, kind: TextUndoHistory.Kind = .replacement) {
        guard isFocused else { return }
        let lower = min(max(range.lowerBound, 0), text.count)
        let upper = min(max(range.upperBound, lower), text.count)
        let startIndex = stringIndex(forCharacterIndex: lower)
        let endIndex = stringIndex(forCharacterIndex: upper)
        let utf16Range = NSRange(startIndex..<endIndex, in: text)
        let removed = String(text[startIndex..<endIndex])
        let before = undoSelection
        let composing = undoHistory.isComposing
        text.replaceSubrange(startIndex..<endIndex, with: replacement)
        cursorPosition = characterIndexForUTF16(utf16Range.location + replacement.utf16.count)
        selectionAnchor = nil
        if clearsMarkedText {
            markedTextRange = nil
            pendingMarkedTextRange = nil
        }
        if composing {
            if clearsMarkedText { undoHistory.finishComposition(text: text, selection: undoSelection) }
        } else {
            undoHistory.record(range: utf16Range, removed: removed, inserted: replacement,
                               before: before, after: undoSelection, kind: kind)
        }
        notifyStateChanged()
    }

    func setMarkedText(_ markedText: String,
                       selectedLocation: Int,
                       selectedLength: Int,
                       replacementRange: Range<Int>?) {
        guard isFocused else { return }
        undoHistory.beginComposition(text: text, selection: undoSelection)
        let range = normalizedRange(replacementRange)
            ?? markedTextRange
            ?? selectionRange
            ?? cursorPosition..<cursorPosition
        let lower = min(max(range.lowerBound, 0), text.count)
        let newMarkedRange = lower..<(lower + markedText.count)
        markedTextRange = markedText.isEmpty ? nil : newMarkedRange
        pendingMarkedTextRange = nil
        replaceText(range: range, with: markedText, clearsMarkedText: false)

        let selectedLower = min(max(selectedLocation, 0), markedText.count)
        let selectedUpper = min(max(selectedLower + selectedLength, selectedLower), markedText.count)
        if selectedUpper > selectedLower {
            selectionAnchor = lower + selectedLower
            cursorPosition = lower + selectedUpper
        } else {
            selectionAnchor = nil
            cursorPosition = lower + selectedLower
        }
        notifyStateChanged()
    }

    func unmarkText() {
        guard isFocused else { return }
        pendingMarkedTextRange = markedTextRange
        markedTextRange = nil
        notifyStateChanged()
    }

    func deleteBackward() {
        guard isFocused else { return }
        if hasSelection { deleteSelection(); return }
        guard cursorPosition > 0 else { return }
        undoHistory.finishComposition(text: text, selection: undoSelection)
        replaceText(range: (cursorPosition - 1)..<cursorPosition, with: "", kind: .backwardDelete)
    }

    func deleteForward() {
        guard isFocused else { return }
        if hasSelection { deleteSelection(); return }
        guard cursorPosition < text.count else { return }
        undoHistory.finishComposition(text: text, selection: undoSelection)
        replaceText(range: cursorPosition..<(cursorPosition + 1), with: "", kind: .forwardDelete)
    }

    func performCommand(_ command: String) {
        guard isFocused else { return }

        switch command {
        case "undo": applyUndo(redo: false)
        case "redo": applyUndo(redo: true)
        case "moveLeft":
            moveCursorLeft()
        case "moveRight":
            moveCursorRight()
        case "moveUp":
            moveVertically(delta: -1, modifySelection: false)
        case "moveDown":
            moveVertically(delta: 1, modifySelection: false)
        case "moveWordLeft":
            moveWordLeft()
        case "moveWordRight":
            moveWordRight()
        case "moveToBeginningOfLine", "moveToLeftEndOfLine":
            moveToVisualLineStart()
        case "moveToEndOfLine", "moveToRightEndOfLine":
            moveToVisualLineEnd()
        case "moveToBeginningOfParagraph":
            moveToParagraphStart()
        case "moveToEndOfParagraph":
            moveToParagraphEnd()
        case "moveToBeginningOfDocument":
            moveToBeginning()
        case "moveToEndOfDocument":
            moveToEnd()
        case "moveLeftAndModifySelection":
            moveLeftAndModifySelection()
        case "moveRightAndModifySelection":
            moveRightAndModifySelection()
        case "moveUpAndModifySelection":
            moveVertically(delta: -1, modifySelection: true)
        case "moveDownAndModifySelection":
            moveVertically(delta: 1, modifySelection: true)
        case "moveWordLeftAndModifySelection":
            moveWordLeftAndModifySelection()
        case "moveWordRightAndModifySelection":
            moveWordRightAndModifySelection()
        case "moveToBeginningOfLineAndModifySelection", "moveToLeftEndOfLineAndModifySelection":
            moveToVisualLineStartAndModifySelection()
        case "moveToEndOfLineAndModifySelection", "moveToRightEndOfLineAndModifySelection":
            moveToVisualLineEndAndModifySelection()
        case "moveToBeginningOfParagraphAndModifySelection":
            moveToParagraphStartAndModifySelection()
        case "moveToEndOfParagraphAndModifySelection":
            moveToParagraphEndAndModifySelection()
        case "moveToBeginningOfDocumentAndModifySelection":
            moveToBeginningAndModifySelection()
        case "moveToEndOfDocumentAndModifySelection":
            moveToEndAndModifySelection()
        case "selectAll":
            selectAll()
        case "deleteBackward":
            deleteBackward()
        case "deleteForward":
            deleteForward()
        case "deleteWordBackward":
            deleteWordBackward()
        case "deleteWordForward":
            deleteWordForward()
        case "deleteToBeginningOfLine":
            deleteToVisualLineStart()
        case "deleteToEndOfLine":
            deleteToVisualLineEnd()
        case "deleteToBeginningOfParagraph":
            deleteToParagraphStart()
        case "deleteToEndOfParagraph":
            deleteToParagraphEnd()
        case "deleteToMark":
            deleteToBeginning()
        case "insertNewline":
            if allowsNewlines {
                insertText("\n")
            } else {
                breakUndoCoalescing(); onSubmit?()
            }
        default:
            break
        }
    }

    func setCursorPosition(_ position: Int,
                           modifySelection: Bool,
                           notifyDelegate: Bool = true) {
        guard isFocused else { return }
        clearMarkedTextState()
        let clamped = clamp(position)
        if modifySelection {
            if selectionAnchor == nil {
                selectionAnchor = cursorPosition
            }
            cursorPosition = clamped
        } else {
            cursorPosition = clamped
            selectionAnchor = nil
        }
        if notifyDelegate {
            notifyStateChanged()
        }
    }

    func selectWord(at position: Int) {
        guard isFocused else { return }
        clearMarkedTextState()
        if masksWordBoundaries { selectAll(); return }
        guard !text.isEmpty else { return }
        let offset = stringIndex(forCharacterIndex: clamp(position)).utf16Offset(in: text)
        let range = NSAttributedString(string: text).doubleClick(at: min(offset, text.utf16.count - 1))
        selectionAnchor = characterIndexForUTF16(range.location)
        cursorPosition = characterIndexForUTF16(NSMaxRange(range))
        notifyStateChanged()
    }

    func selectAll() {
        guard isFocused else { return }
        clearMarkedTextState()
        selectionAnchor = 0
        cursorPosition = text.count
        notifyStateChanged()
    }

    func selectedTextContent() -> String? {
        guard let range = selectionRange else { return nil }
        let lower = text.index(text.startIndex, offsetBy: range.lowerBound)
        let upper = text.index(text.startIndex, offsetBy: range.upperBound)
        if lower == upper { return nil }
        return String(text[lower..<upper])
    }

    func cutSelectedTextContent() -> String? {
        guard isFocused, let range = selectionRange, let selected = selectedTextContent() else { return nil }
        breakUndoCoalescing()
        replaceText(range: range, with: "", kind: .cut)
        breakUndoCoalescing()
        return selected
    }

    // MARK: - Private helpers

    private func moveCursorLeft() {
        clearMarkedTextState()
        if hasSelection {
            cursorPosition = min(selectionRange!.lowerBound, selectionRange!.upperBound)
            selectionAnchor = nil
            notifyStateChanged()
            return
        }
        guard cursorPosition > 0 else { return }
        cursorPosition -= 1
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveCursorRight() {
        clearMarkedTextState()
        if hasSelection {
            cursorPosition = max(selectionRange!.lowerBound, selectionRange!.upperBound)
            selectionAnchor = nil
            notifyStateChanged()
            return
        }
        guard cursorPosition < text.count else { return }
        cursorPosition += 1
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveToBeginning() {
        clearMarkedTextState()
        cursorPosition = 0
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveToEnd() {
        clearMarkedTextState()
        cursorPosition = text.count
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveToVisualLineStart() {
        clearMarkedTextState()
        cursorPosition = visualLineRange(for: cursorPosition).lowerBound
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveToVisualLineEnd() {
        clearMarkedTextState()
        cursorPosition = visualLineRange(for: cursorPosition).upperBound
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveToParagraphStart() {
        clearMarkedTextState()
        cursorPosition = paragraphStart(for: cursorPosition)
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveToParagraphEnd() {
        clearMarkedTextState()
        cursorPosition = paragraphEnd(for: cursorPosition)
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveVertically(delta: Int, modifySelection: Bool) {
        guard delta != 0 else { return }
        guard allowsNewlines,
              let target = visualLineOffset(from: cursorPosition, delta: delta) else {
            return
        }
        clearMarkedTextState()
        if modifySelection {
            extendSelection(to: target)
        } else {
            cursorPosition = target
            selectionAnchor = nil
            notifyStateChanged()
        }
    }

    private func moveWordLeft() {
        clearMarkedTextState()
        if hasSelection {
            cursorPosition = min(selectionRange!.lowerBound, selectionRange!.upperBound)
            selectionAnchor = nil
            notifyStateChanged()
            return
        }
        cursorPosition = findPreviousWordBoundary(from: cursorPosition)
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveWordRight() {
        clearMarkedTextState()
        if hasSelection {
            cursorPosition = max(selectionRange!.lowerBound, selectionRange!.upperBound)
            selectionAnchor = nil
            notifyStateChanged()
            return
        }
        cursorPosition = findNextWordBoundary(from: cursorPosition)
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func moveLeftAndModifySelection() {
        clearMarkedTextState()
        let target = max(0, cursorPosition - 1)
        extendSelection(to: target)
    }

    private func moveRightAndModifySelection() {
        clearMarkedTextState()
        let target = min(text.count, cursorPosition + 1)
        extendSelection(to: target)
    }

    private func moveWordLeftAndModifySelection() {
        clearMarkedTextState()
        let target = findPreviousWordBoundary(from: cursorPosition)
        extendSelection(to: target)
    }

    private func moveWordRightAndModifySelection() {
        clearMarkedTextState()
        let target = findNextWordBoundary(from: cursorPosition)
        extendSelection(to: target)
    }

    private func moveToBeginningAndModifySelection() {
        clearMarkedTextState()
        extendSelection(to: 0)
    }

    private func moveToEndAndModifySelection() {
        clearMarkedTextState()
        extendSelection(to: text.count)
    }

    private func moveToVisualLineStartAndModifySelection() {
        clearMarkedTextState()
        extendSelection(to: visualLineRange(for: cursorPosition).lowerBound)
    }

    private func moveToVisualLineEndAndModifySelection() {
        clearMarkedTextState()
        extendSelection(to: visualLineRange(for: cursorPosition).upperBound)
    }

    private func moveToParagraphStartAndModifySelection() {
        clearMarkedTextState()
        extendSelection(to: paragraphStart(for: cursorPosition))
    }

    private func moveToParagraphEndAndModifySelection() {
        clearMarkedTextState()
        extendSelection(to: paragraphEnd(for: cursorPosition))
    }

    private func deleteSelection() {
        guard let range = selectionRange else { return }
        breakUndoCoalescing()
        replaceText(range: range, with: "", kind: .delete)
    }

    private func deleteWordBackward() {
        guard isFocused else { return }
        if hasSelection { deleteSelection(); return }
        guard cursorPosition > 0 else { return }
        undoHistory.finishComposition(text: text, selection: undoSelection)
        replaceText(range: findPreviousWordBoundary(from: cursorPosition)..<cursorPosition, with: "", kind: .delete)
    }

    private func deleteWordForward() {
        guard isFocused else { return }
        if hasSelection { deleteSelection(); return }
        guard cursorPosition < text.count else { return }
        undoHistory.finishComposition(text: text, selection: undoSelection)
        replaceText(range: cursorPosition..<findNextWordBoundary(from: cursorPosition), with: "", kind: .delete)
    }

    private func deleteToVisualLineStart() {
        deleteBackward(to: visualLineRange(for: cursorPosition).lowerBound)
    }

    private func deleteToVisualLineEnd() {
        deleteForward(to: visualLineRange(for: cursorPosition).upperBound)
    }

    private func deleteToParagraphStart() {
        deleteBackward(to: paragraphStart(for: cursorPosition))
    }

    private func deleteToParagraphEnd() {
        deleteForward(to: paragraphEnd(for: cursorPosition))
    }

    private func deleteToBeginning() {
        deleteBackward(to: 0)
    }

    private func deleteBackward(to target: Int) {
        if hasSelection {
            deleteSelection()
            return
        }
        let clampedTarget = clamp(target)
        guard clampedTarget < cursorPosition else { return }
        clearMarkedTextState()
        let startIndex = stringIndex(forCharacterIndex: clampedTarget)
        let endIndex = stringIndex(forCharacterIndex: cursorPosition)
        replaceText(range: text.distance(from: text.startIndex, to: startIndex)..<text.distance(from: text.startIndex, to: endIndex), with: "", kind: .delete)
    }

    private func deleteToEnd() {
        deleteForward(to: text.count)
    }

    private func deleteForward(to target: Int) {
        if hasSelection {
            deleteSelection()
            return
        }
        let clampedTarget = clamp(target)
        guard clampedTarget > cursorPosition else { return }
        clearMarkedTextState()
        let startIndex = stringIndex(forCharacterIndex: cursorPosition)
        let endIndex = stringIndex(forCharacterIndex: clampedTarget)
        replaceText(range: text.distance(from: text.startIndex, to: startIndex)..<text.distance(from: text.startIndex, to: endIndex), with: "", kind: .delete)
    }

    private func extendSelection(to position: Int) {
        let clamped = clamp(position)
        if selectionAnchor == nil {
            selectionAnchor = cursorPosition
        }
        cursorPosition = clamped
        notifyStateChanged()
    }

    private func clamp(_ value: Int) -> Int {
        return min(max(0, value), text.count)
    }

    private func findPreviousWordBoundary(from position: Int) -> Int {
        if masksWordBoundaries { return 0 }
        let offset = stringIndex(forCharacterIndex: clamp(position)).utf16Offset(in: text)
        return characterIndexForUTF16(NSAttributedString(string: text).nextWord(from: offset, forward: false))
    }

    private func findNextWordBoundary(from position: Int) -> Int {
        if masksWordBoundaries { return text.count }
        let offset = stringIndex(forCharacterIndex: clamp(position)).utf16Offset(in: text)
        return characterIndexForUTF16(NSAttributedString(string: text).nextWord(from: offset, forward: true))
    }

    private func stringIndex(forCharacterIndex index: Int) -> String.Index {
        text.index(text.startIndex, offsetBy: index)
    }

    private func normalizedRange(_ range: Range<Int>?) -> Range<Int>? {
        guard let range else { return nil }
        let lower = min(max(range.lowerBound, 0), text.count)
        let upper = min(max(range.upperBound, lower), text.count)
        return lower..<upper
    }

    private func clearMarkedTextState() {
        breakUndoCoalescing()
    }

    private func characterIndexForUTF16(_ utf16Index: Int) -> Int {
        let offset = max(0, min(utf16Index, text.utf16.count))
        let stringIndex = String.Index(utf16Offset: offset, in: text)
        return text.distance(from: text.startIndex, to: stringIndex)
    }

    private func paragraphStart(for position: Int) -> Int {
        let clamped = clamp(position)
        let index = stringIndex(forCharacterIndex: clamped)
        guard let newline = text[..<index].lastIndex(of: "\n") else { return 0 }
        return text.distance(from: text.startIndex, to: text.index(after: newline))
    }

    private func paragraphEnd(for position: Int) -> Int {
        let clamped = clamp(position)
        let index = stringIndex(forCharacterIndex: clamped)
        guard let newline = text[index...].firstIndex(of: "\n") else { return text.count }
        return text.distance(from: text.startIndex, to: newline)
    }

    private func visualLineRange(for position: Int) -> Range<Int> {
        let clamped = clamp(position)
        guard allowsNewlines,
              let visualLineWidth,
              visualLineWidth > 1 else {
            return paragraphStart(for: clamped)..<paragraphEnd(for: clamped)
        }

        let fragments = visualLineFragments(maxWidth: visualLineWidth)
        if let containing = fragments.first(where: { fragment in
            if fragment.lowerBound == fragment.upperBound {
                return clamped == fragment.lowerBound
            }
            return clamped >= fragment.lowerBound && clamped < fragment.upperBound
        }) {
            return containing
        }
        return fragments.first(where: {
            $0.lowerBound != $0.upperBound && clamped == $0.upperBound
        }) ?? fragments.last ?? 0..<0
    }

    private func visualLineOffset(from position: Int, delta: Int) -> Int? {
        guard let visualLineWidth, visualLineWidth > 1 else { return nil }
        let fragments = visualLineFragments(maxWidth: visualLineWidth)
        guard !fragments.isEmpty else { return nil }
        let clamped = clamp(position)
        let currentIndex = visualLineIndex(for: clamped, in: fragments)
        let targetIndex = min(max(currentIndex + delta, 0), fragments.count - 1)
        guard targetIndex != currentIndex else { return nil }

        let current = fragments[currentIndex]
        let target = fragments[targetIndex]
        let column = max(clamped - current.lowerBound, 0)
        return min(target.lowerBound + column, target.upperBound)
    }

    private func visualLineIndex(for position: Int, in fragments: [Range<Int>]) -> Int {
        let clamped = clamp(position)
        if let exact = fragments.firstIndex(where: { fragment in
            if fragment.lowerBound == fragment.upperBound {
                return clamped == fragment.lowerBound
            }
            return clamped >= fragment.lowerBound && clamped < fragment.upperBound
        }) {
            return exact
        }
        if let ending = fragments.firstIndex(where: {
            $0.lowerBound != $0.upperBound && clamped == $0.upperBound
        }) {
            return ending
        }
        if clamped >= fragments.last?.upperBound ?? 0 {
            return max(fragments.count - 1, 0)
        }
        return 0
    }

    private func visualLineFragments(maxWidth: CGFloat) -> [Range<Int>] {
        guard !text.isEmpty else { return [0..<0] }

        var fragments: [Range<Int>] = []
        var paragraphStart = 0
        let parts = text.split(separator: "\n", omittingEmptySubsequences: false)
        if parts.isEmpty {
            return [0..<0]
        }

        for part in parts {
            let paragraph = String(part)
            let paragraphLength = paragraph.count
            if paragraphLength == 0 {
                fragments.append(paragraphStart..<paragraphStart)
            } else {
                for relativeRange in wrappedRanges(for: paragraph, maxWidth: maxWidth) {
                    fragments.append((paragraphStart + relativeRange.lowerBound)..<(paragraphStart + relativeRange.upperBound))
                }
            }
            paragraphStart += paragraphLength + 1
        }

        return fragments
    }

    private func wrappedRanges(for value: String, maxWidth: CGFloat) -> [Range<Int>] {
        let count = value.count
        guard count > 0 else { return [0..<0] }
        var ranges: [Range<Int>] = []
        var start = 0
        while start < count {
            var low = start + 1
            var high = count
            var best = start + 1
            while low <= high {
                let mid = (low + high) / 2
                if measuredWidth(of: value, range: start..<mid) <= maxWidth {
                    best = mid
                    low = mid + 1
                } else {
                    high = mid - 1
                }
            }
            ranges.append(start..<max(best, start + 1))
            start = max(best, start + 1)
        }
        return ranges
    }

    private func measuredWidth(of value: String, range: Range<Int>) -> CGFloat {
        let lower = value.index(value.startIndex, offsetBy: range.lowerBound)
        let upper = value.index(value.startIndex, offsetBy: range.upperBound)
        let attributed = NSAttributedString(string: String(value[lower..<upper]),
                                            attributes: [.font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)])
        let line = CTLineCreateWithAttributedString(attributed)
        return CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
    }

    private func notifyStateChanged() {
        delegate?.textInputControllerDidChangeState()
    }

    func enabledEditCommands(in requestedCommands: OuterframeEditCommandSet) -> OuterframeEditCommandSet {
        guard isFocused else { return [] }

        var enabledCommands: OuterframeEditCommandSet = []
        if requestedCommands.contains(.undo), undoHistory.canUndo || undoHistory.isComposing { enabledCommands.insert(.undo) }
        if requestedCommands.contains(.redo), undoHistory.canRedo && !undoHistory.isComposing { enabledCommands.insert(.redo) }
        if hasSelection {
            if requestedCommands.contains(.copy) {
                enabledCommands.insert(.copy)
            }
            if requestedCommands.contains(.cut) {
                enabledCommands.insert(.cut)
            }
        }
        if requestedCommands.contains(.paste) {
            enabledCommands.insert(.paste)
        }
        if requestedCommands.contains(.selectAll), !text.isEmpty {
            enabledCommands.insert(.selectAll)
        }
        return enabledCommands
    }

    func currentAcceptedPasteboardTypeIdentifiers() -> [String] {
        isFocused ? acceptedPasteboardTypeIdentifiers : []
    }
}
