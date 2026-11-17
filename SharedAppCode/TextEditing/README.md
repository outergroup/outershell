# Shared text undo

`TextUndoHistory.swift` is a Foundation-only source dependency shared by outerframe
apps. Apps compile a copy in `Vendor/TextEditing` (Top uses
`Frontend/Vendor/TextEditing`), and new Swift apps from Outer Shell's native app
template start with one. This directory holds the original. It has no network or
host dependencies and can later move into a static library. Refresh copies
explicitly with `python3 sync.py /path/to/App ...`; the template is
`Resources/NativeAppTemplate/frontend-swift`.

The history stores UTF-16 edit ranges and directed selections. Adjacent typing
coalesces into word-sized runs; same-direction deletion coalesces; a two-second
pause ends a run. Paste, cut, replacement, and IME composition are atomic. Call
`breakCoalescing` for navigation, focus changes, and Save. Use
`beginComposition`/`finishComposition` around provisional marked text. No-op
compositions preserve redo. A new edit clears redo. History is in-memory and
bounded to 200 groups / 16 MiB of edit text, retaining the newest operation.

The existing `SingleLineTextInputController` variants adapt their character
positions to UTF-16 and use the same engine. Route normal input to `insertText`,
clipboard text to `pasteText`, and native commands to `performCommand`. Route cut
to `cutSelectedTextContent`. Query `enabledEditCommands` for host menu validation.
Delegate notifications fire after undo/redo just as they do after other edits.

`setText` starts a fresh history for programmatic content replacement. For a
controller reused across fields, use `setText(_:historyKey:)` with stable field
identities. Returning to a key restores its history only if its text still
matches; externally replaced text starts fresh. `resetUndoHistory` clears the
current and cached histories at the end of an editing session. Blurring ends the
current group without deleting history. Secret fields may reset on blur.

Outer Shell uses keyed histories for create fields and container configuration
tabs; Profile, Top, and Plaintext's Find/file-picker controls use the same
engine. The host forwards `textCommand("undo")`/`textCommand("redo")` and validates
availability using edit-command bits 7/8. No additional wire protocol is needed.

Run `bash test.sh /path/to/App ...` to check apps' controller integrations and
verify their engine copies match. Controllers with `allowsNewlines` also run the
multi-line tests. `EditCommandStub.swift` supplies only the protocol option set for
these isolated tests; full app builds validate the real vendored protocol.
Plaintext's own `Scripts/test_native_documents.sh` also tests the history engine
and document/save integration.
