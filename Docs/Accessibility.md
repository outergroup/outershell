# Native accessibility

The outerframe frontend publishes semantic snapshots from `BackendsContent.swift`.
Node identities are stable within each screen and modal context. Modal overlays
publish only their own controls and use the dialog role, including nested container
prompts. Button focus is separate from activation.

Interactive nodes opt into `accessibilityActionAndSnapshot`: the frontend applies
the action and returns a snapshot in the same main-actor handler. This acknowledges
the immediate UI change; asynchronous server operations still publish subsequent
updates when they finish.

Container configuration and Add Apps text fields expose UTF-16 selection ranges
and text geometry. Geometry uses the same CoreText lines, wrapping, and scroll
positions as rendering. Input controllers use Swift character offsets, so selection
updates validate and convert UTF-16 boundaries before modifying their selection.
Password fields do not expose their contents or text geometry.

This consumes the framework's existing accessibility messages (1044, 1045, 2035);
it introduces no new wire messages. The canonical protocol specification is
`Outerframe/outerframe-socket-messages.md` in the Outer Loop repository.

Run `python3 Scripts/test_accessibility_text_geometry.py` on macOS to check the
production layout/query methods for Unicode, hit testing, caret bounds, wrapping,
visual lines, and scrolling. Also run Outer Loop's
`OuterLoop/Scripts/test_outerframe_accessibility.sh` with this repository's
`Vendor/OuterframeSwiftMethods/OuterContentSocketMessage.swift` as its argument.
