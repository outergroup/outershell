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
            moveToBeginning()
        case "moveDown":
            moveToEnd()
        case "moveWordLeft":
            moveWordLeft()
        case "moveWordRight":
            moveWordRight()
        case "moveToBeginningOfLine", "moveToBeginningOfDocument", "moveToBeginningOfParagraph", "moveToLeftEndOfLine":
            moveToBeginning()
        case "moveToEndOfLine", "moveToEndOfDocument", "moveToEndOfParagraph", "moveToRightEndOfLine":
            moveToEnd()
        case "moveLeftAndModifySelection":
            moveLeftAndModifySelection()
        case "moveRightAndModifySelection":
            moveRightAndModifySelection()
        case "moveWordLeftAndModifySelection":
            moveWordLeftAndModifySelection()
        case "moveWordRightAndModifySelection":
            moveWordRightAndModifySelection()
        case "moveToBeginningOfLineAndModifySelection", "moveToBeginningOfDocumentAndModifySelection", "moveToBeginningOfParagraphAndModifySelection", "moveToLeftEndOfLineAndModifySelection":
            moveToBeginningAndModifySelection()
        case "moveToEndOfLineAndModifySelection", "moveToEndOfDocumentAndModifySelection", "moveToEndOfParagraphAndModifySelection", "moveToRightEndOfLineAndModifySelection":
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
        case "deleteToBeginningOfLine", "deleteToBeginningOfParagraph":
            deleteToBeginning()
        case "deleteToEndOfLine", "deleteToEndOfParagraph":
            deleteToEnd()
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

    private func deleteToBeginning() {
        if hasSelection {
            deleteSelection()
            return
        }
        guard cursorPosition > 0 else { return }
        clearMarkedTextState()
        let endIndex = stringIndex(forCharacterIndex: cursorPosition)
        text.removeSubrange(text.startIndex..<endIndex)
        cursorPosition = 0
        selectionAnchor = nil
        notifyStateChanged()
    }

    private func deleteToEnd() {
        if hasSelection {
            deleteSelection()
            return
        }
        guard cursorPosition < text.count else { return }
        clearMarkedTextState()
        let startIndex = stringIndex(forCharacterIndex: cursorPosition)
        text.removeSubrange(startIndex..<text.endIndex)
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
