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
        isFocused = false
        selectionAnchor = nil
        markedTextRange = nil
        pendingMarkedTextRange = nil
        notifyStateChanged()
    }

    func setText(_ newText: String) {
        text = newText
        cursorPosition = min(cursorPosition, text.count)
        selectionAnchor = nil
        markedTextRange = nil
        pendingMarkedTextRange = nil
        notifyStateChanged()
    }

    func insertText(_ value: String) {
        guard isFocused else { return }

        if value == "\u{8}" {
            deleteBackward()
            return
        } else if value == "\u{7f}" {
            deleteForward()
            return
        } else if value == "\u{2190}" {
            moveCursorLeft()
            return
        } else if value == "\u{2192}" {
            moveCursorRight()
            return
        } else if (value == "\n" || value == "\r") && !allowsNewlines {
            onSubmit?()
            return
        }

        if let range = markedTextRange ?? pendingMarkedTextRange {
            replaceText(range: range, with: value)
            return
        }

        if hasSelection {
            deleteSelection()
        }

        let index = stringIndex(forCharacterIndex: cursorPosition)
        text.insert(contentsOf: value, at: index)
        cursorPosition += value.count
        selectionAnchor = nil
        markedTextRange = nil
        pendingMarkedTextRange = nil
        notifyStateChanged()
    }

    func replaceText(range: Range<Int>, with replacement: String) {
        guard isFocused else { return }
        replaceText(range: range, with: replacement, clearsMarkedText: true)
    }

    private func replaceText(range: Range<Int>, with replacement: String, clearsMarkedText: Bool) {
        let lower = min(max(range.lowerBound, 0), text.count)
        let upper = min(max(range.upperBound, lower), text.count)
        let startIndex = stringIndex(forCharacterIndex: lower)
        let endIndex = stringIndex(forCharacterIndex: upper)
        text.replaceSubrange(startIndex..<endIndex, with: replacement)
        cursorPosition = lower + replacement.count
        selectionAnchor = nil
        if clearsMarkedText {
            markedTextRange = nil
            pendingMarkedTextRange = nil
        }
        notifyStateChanged()
    }

    func setMarkedText(_ markedText: String,
                       selectedLocation: Int,
                       selectedLength: Int,
                       replacementRange: Range<Int>?) {
        guard isFocused else { return }
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
        if hasSelection {
            deleteSelection()
            return
        }
        guard cursorPosition > 0 else { return }
        clearMarkedTextState()
        let removeIndex = text.index(text.startIndex, offsetBy: cursorPosition - 1)
        text.remove(at: removeIndex)
        cursorPosition -= 1
        selectionAnchor = nil
        notifyStateChanged()
    }

    func deleteForward() {
        guard isFocused else { return }
        if hasSelection {
            deleteSelection()
            return
        }
        guard cursorPosition < text.count else { return }
        clearMarkedTextState()
        let removeIndex = stringIndex(forCharacterIndex: cursorPosition)
        text.remove(at: removeIndex)
        selectionAnchor = nil
        notifyStateChanged()
    }

    func performCommand(_ command: String) {
        guard isFocused else { return }

        switch command {
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
                onSubmit?()
            }
        default:
            break
        }
    }

    func setCursorPosition(_ position: Int, modifySelection: Bool) {
        guard isFocused else { return }
        clearMarkedTextState()
        let clamped = clamp(position)
        if modifySelection {
            extendSelection(to: clamped)
        } else {
            cursorPosition = clamped
            selectionAnchor = nil
            notifyStateChanged()
        }
    }

    func selectWord(at position: Int) {
        guard isFocused else { return }
        clearMarkedTextState()
        let clamped = clamp(position)
        let start = findPreviousWordBoundary(from: clamped)
        let end = findNextWordBoundary(from: clamped)
        selectionAnchor = start
        cursorPosition = end
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
        guard let selectedText = selectedTextContent() else { return nil }
        deleteSelection()
        return selectedText
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
        clearMarkedTextState()
        let startIndex = stringIndex(forCharacterIndex: range.lowerBound)
        let endIndex = stringIndex(forCharacterIndex: range.upperBound)
        text.removeSubrange(startIndex..<endIndex)
        cursorPosition = range.lowerBound
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func deleteWordBackward() {
        if hasSelection {
            deleteSelection()
            return
        }
        guard cursorPosition > 0 else { return }
        clearMarkedTextState()
        let boundary = findPreviousWordBoundary(from: cursorPosition)
        let startIndex = stringIndex(forCharacterIndex: boundary)
        let endIndex = stringIndex(forCharacterIndex: cursorPosition)
        text.removeSubrange(startIndex..<endIndex)
        cursorPosition = boundary
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func deleteWordForward() {
        if hasSelection {
            deleteSelection()
            return
        }
        guard cursorPosition < text.count else { return }
        clearMarkedTextState()
        let boundary = findNextWordBoundary(from: cursorPosition)
        let startIndex = stringIndex(forCharacterIndex: cursorPosition)
        let endIndex = stringIndex(forCharacterIndex: boundary)
        text.removeSubrange(startIndex..<endIndex)
        selectionAnchor = nil
        notifyStateChanged()
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
        text.removeSubrange(startIndex..<endIndex)
        cursorPosition = clampedTarget
        selectionAnchor = nil
        notifyStateChanged()
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
        text.removeSubrange(startIndex..<endIndex)
        selectionAnchor = nil
        notifyStateChanged()
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
        guard !text.isEmpty else { return 0 }
        let clamped = clamp(position)
        if clamped == 0 { return 0 }

        let cfString = text as CFString
        let tokenizer = CFStringTokenizerCreate(kCFAllocatorDefault,
                                                cfString,
                                                CFRangeMake(0, text.utf16.count),
                                                kCFStringTokenizerUnitWordBoundary,
                                                nil)

        var previousBoundary = 0
        var tokenType = CFStringTokenizerAdvanceToNextToken(tokenizer)
        while tokenType.rawValue != 0 {
            let range = CFStringTokenizerGetCurrentTokenRange(tokenizer)
            let tokenEnd = range.location + range.length
            let characterIndex = characterIndexForUTF16(tokenEnd)
            if characterIndex >= clamped {
                break
            }
            previousBoundary = characterIndex
            tokenType = CFStringTokenizerAdvanceToNextToken(tokenizer)
        }
        return previousBoundary
    }

    private func findNextWordBoundary(from position: Int) -> Int {
        guard !text.isEmpty else { return 0 }
        let clamped = clamp(position)
        if clamped >= text.count { return text.count }

        let cfString = text as CFString
        let tokenizer = CFStringTokenizerCreate(kCFAllocatorDefault,
                                                cfString,
                                                CFRangeMake(0, text.utf16.count),
                                                kCFStringTokenizerUnitWordBoundary,
                                                nil)

        var tokenType = CFStringTokenizerAdvanceToNextToken(tokenizer)
        while tokenType.rawValue != 0 {
            let range = CFStringTokenizerGetCurrentTokenRange(tokenizer)
            let characterIndex = characterIndexForUTF16(range.location + range.length)
            if characterIndex > clamped {
                return characterIndex
            }
            tokenType = CFStringTokenizerAdvanceToNextToken(tokenizer)
        }
        return text.count
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
        markedTextRange = nil
        pendingMarkedTextRange = nil
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
        return fragments.first { fragment in
            if fragment.lowerBound == fragment.upperBound {
                return clamped == fragment.lowerBound
            }
            return clamped >= fragment.lowerBound && clamped < fragment.upperBound
        }
            ?? fragments.last
            ?? 0..<0
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
