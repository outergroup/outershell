import Foundation

@MainActor
final class Delegate: SingleLineTextInputControllerDelegate {
    var notifications = 0
    func textInputControllerDidChangeState() { notifications += 1 }
}

@main
struct TextInputUndoTests {
    @MainActor static func main() {
        let delegate = Delegate()
        func input(_ text: String = "") -> SingleLineTextInputController<Delegate> {
            let field = SingleLineTextInputController<Delegate>(identifier: UUID(), initialText: text)
            field.delegate = delegate
            field.focus()
            return field
        }
        let field = input()
        for c in "hello world" { field.insertText(String(c)) }
        field.performCommand("undo")
        precondition(field.text == "hello ")
        field.performCommand("redo")
        precondition(field.text == "hello world")
        field.selectAll()
        for c in "replacement" { field.insertText(String(c)) }
        field.performCommand("undo")
        precondition(field.text == "hello world" && field.selectionRange == 0..<11)
        field.pasteText("👨‍👩‍👧‍👦e\u{301}")
        field.performCommand("deleteBackward")
        field.performCommand("deleteBackward")
        precondition(field.text.isEmpty)
        field.performCommand("undo")
        precondition(field.text == "👨‍👩‍👧‍👦e\u{301}")
        field.performCommand("undo")
        precondition(field.text == "hello world" && field.hasSelection)
        _ = field.cutSelectedTextContent()
        precondition(field.text.isEmpty)
        field.performCommand("undo")
        precondition(field.text == "hello world")
        field.blur()
        precondition(field.enabledEditCommands(in: [.undo, .redo]).isEmpty)
        field.focus()
        field.performCommand("redo")
        precondition(field.text.isEmpty)
        field.insertText("x")
        precondition(!field.enabledEditCommands(in: [.redo]).contains(.redo))
        field.setText("server")
        precondition(field.enabledEditCommands(in: [.undo, .redo]).isEmpty)

        let composed = input("prefix ")
        composed.setMarkedText("n", selectedLocation: 1, selectedLength: 0, replacementRange: nil)
        composed.setMarkedText("ni", selectedLocation: 2, selectedLength: 0, replacementRange: nil)
        composed.unmarkText()
        composed.insertText("你")
        precondition(composed.text == "prefix 你")
        composed.performCommand("undo")
        precondition(composed.text == "prefix ")
        composed.performCommand("redo")
        precondition(composed.text == "prefix 你")
        composed.setMarkedText("a", selectedLocation: 1, selectedLength: 0, replacementRange: nil)
        composed.insertText("")
        composed.performCommand("undo")
        precondition(composed.text == "prefix ")
        composed.setMarkedText("a", selectedLocation: 1, selectedLength: 0, replacementRange: nil)
        composed.insertText("")
        precondition(composed.enabledEditCommands(in: [.redo]).contains(.redo))
        composed.performCommand("redo")
        precondition(composed.text == "prefix 你")

        let shared = input()
        shared.setText("one", historyKey: "first")
        shared.performCommand("moveToEndOfDocument")
        shared.insertText("!")
        shared.setText("two", historyKey: "second")
        shared.performCommand("moveToEndOfDocument")
        shared.insertText("?")
        shared.setText("one!", historyKey: "first")
        shared.performCommand("undo")
        precondition(shared.text == "one")
        shared.setText("two?", historyKey: "second")
        shared.performCommand("undo")
        precondition(shared.text == "two")
        shared.setText("external", historyKey: "first")
        precondition(shared.enabledEditCommands(in: [.undo, .redo]).isEmpty)
        shared.resetUndoHistory()
        shared.setText("two", historyKey: "second")
        precondition(shared.enabledEditCommands(in: [.undo, .redo]).isEmpty)
        let boundary = input()
        boundary.insertText("a")
        boundary.breakUndoCoalescing()
        boundary.insertText("b")
        boundary.performCommand("undo")
        precondition(boundary.text == "a")
        boundary.performCommand("undo")
        precondition(boundary.text.isEmpty)
#if MULTILINE
        let multiline = input("FROM alpine")
        multiline.allowsNewlines = true
        multiline.performCommand("insertNewline")
        for c in "RUN true" { multiline.insertText(String(c)) }
        multiline.performCommand("undo")
        precondition(multiline.text == "FROM alpine\nRUN ")
        multiline.performCommand("undo")
        precondition(multiline.text == "FROM alpine\n")
        multiline.performCommand("undo")
        precondition(multiline.text == "FROM alpine")
#endif
        precondition(delegate.notifications > 20)
        print("PASS field undo: grouping, selection, paste/cut, Unicode, composition, focus, keyed histories, resets, notifications")
    }
}
