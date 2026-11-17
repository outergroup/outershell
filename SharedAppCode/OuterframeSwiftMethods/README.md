# outerframe Swift helpers

Swift outerframe apps compile a copy of these helpers in
`Vendor/OuterframeSwiftMethods` (Top uses `Frontend/Vendor/OuterframeSwiftMethods`),
plus outerframe's shared files in `Vendor/Common`. This directory holds the
originals of the app-side helpers:

- `OuterframeHost.swift`: the method-based API apps use to talk to their host:
  input modes, cursors, pasteboard and drag, display links, history and
  navigation, accessibility, edit commands, and host-specific messages.
  `dataDirectoryURL` exposes `OUTERFRAME_DATA_DIR` for persistent storage.
- `SocketToBrowser.swift`: the socket connection to the host.

The protocol and accessibility types come from the outerframe repository, which
owns them: `macOS/Common/OuterframeContentSocketMessage.swift` (vendored as
`OuterContentSocketMessage.swift`), `macOS/Common/OuterframeAccessibility.swift`,
and the `Vendor/Common` files. They are not copied here, so there is one original
of each.

Refresh apps, including the native app template at
`Resources/NativeAppTemplate/frontend-swift`, with:

```sh
python3 sync.py /path/to/App ... [--outerframe /path/to/outerframe]
```

`--outerframe` defaults to an `outerframe` checkout next to this repository.

These helpers stay generic. Behavior that only one host or app needs belongs in
that app. Outer Shell's requests that only Outer Loop answers, for example, live
in `Frontend/OuterLoopHostRequests.swift` and use `sendHostSpecificMessage`;
`OuterframeHost` passes host-specific replies on to the app's message handler.
