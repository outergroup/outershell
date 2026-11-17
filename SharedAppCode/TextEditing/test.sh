#!/bin/bash
# Checks that each app's vendored engine matches this one, then runs the undo tests
# against the app's own SingleLineTextInputController. Controllers that support
# newlines (allowsNewlines) also run the multi-line tests.
set -euo pipefail
if [[ $# -eq 0 ]]; then
    echo "Usage: $0 APP_DIRECTORY..." >&2
    exit 1
fi
SOURCE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
for APP in "$@"; do
    ENGINE="$APP/Vendor/TextEditing/TextUndoHistory.swift"
    [[ -f "$ENGINE" ]] || ENGINE="$APP/Frontend/Vendor/TextEditing/TextUndoHistory.swift"
    CONTROLLER="$APP/Frontend/SingleLineTextInputController.swift"
    [[ -f "$CONTROLLER" ]] || CONTROLLER="$APP/Frontend/Common/SingleLineTextInputController.swift"
    cmp "$SOURCE/TextUndoHistory.swift" "$ENGINE"
    OPTIONS=()
    if grep -q "var allowsNewlines" "$CONTROLLER"; then OPTIONS=(-D MULTILINE); fi
    NAME="$(basename "$(cd "$APP" && pwd)")"
    swiftc ${OPTIONS[@]+"${OPTIONS[@]}"} -parse-as-library "$SOURCE/Tests/EditCommandStub.swift" "$ENGINE" \
        "$CONTROLLER" "$SOURCE/Tests/TextInputUndoTests.swift" -o "$WORK/$NAME"
    echo "$NAME:"
    "$WORK/$NAME"
done
