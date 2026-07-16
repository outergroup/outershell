This is a multi-platform outerframe app. It is deliberately multi-platform,
not cross-platform: each platform has its own first-class implementation while
the server, APIs, identity, and outerframe negotiation protocol are shared.

`html/` contains the web implementation. `macos/` contains a CALayer-based
Swift or Objective-C bundle that runs in a sandboxed background process;
everything you know about CALayers applies. Do not edit `macos/Vendor/` unless
asked. App code belongs in `macos/Frontend/`.

`server/` is generated as Go or C and serves the HTML target, the `.outer`
descriptor and compiled platform archives, and the app's API. At `/`, normal
browsers receive HTML while clients sending `Outerframe-Accept:
application/vnd.outerframe` receive the compiled outerframe path when one is
available.

Prefer small explicit little-endian binary records for shared APIs. Avoid JSON
unless a human-editable or ecosystem-facing format is part of the product.

App identity, enabled platforms, isolation, and language choices live in
`app.env`. Build and deployment tasks live in `./app` and `deploy/`. Compiled
platform outputs live in `artifacts/`.

Containerized open-web deployment is provider-neutral. `./app deploy-web
<provider>` dispatches to an executable Bash adapter in
`deploy/web-providers/`, the user's outerframe configuration, or `PATH`.
Adapters may invoke provider-native CLIs, but must not move provider-specific
requirements into the container contract or make Outer Shell/`outerctl` part of
the deployment path.

This directory on the Linux server is the user-owned canonical project. New App
defaults to `~/outerframe-apps`, but the user may choose another source root. It
is suitable for a normal Git repository.
Edit HTML, server, and platform source here (including through coding agents
over SSH). The deployed service never reads this source tree directly:
`./app deploy` builds and installs a private runtime snapshot, so source edits
do not become live until deployment.

Do not turn a platform build workspace into a second copy of the project. A Mac
workspace may sync `macos/` from here, but it publishes only `app.outer` and the
compiled `macos-arm`/`macos-x86` archives back through `./app accept-platform
macos`. Those generated files live under the gitignored `artifacts/` directory,
and the publish command redeploys the resulting snapshot or container image.
Never make lasting edits in `~/.local/share/outershell-apps`; it contains only
disposable deployed output.
