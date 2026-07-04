This is a full-stack outerframe app. The frontend (frontend/) is a macOS CALayer-based bundle; everything you know about CALayers applies. It runs in a sandboxed background process and communicates with the Outer Loop browser over Unix socket messages, surfaced as APIs and delegate methods by frontend/Vendor/. The outerframe is open source: https://github.com/outergroup/outerframe

The backend (backend/) is a small HTTP server, generated as either Go or C, that runs locally or on a Linux server and serves the .outer descriptor, the platform bundle archives, and the app's binary API. Swift frontends reach it via OuterframeHost.pluginOriginURL() with a URLSession configured by OuterframeHost.applyProxy(to:), which tunnels requests over the user's SSH connection when needed.

Prefer small explicit little-endian binary records for app APIs. Avoid JSON unless the user specifically asks for them; the default should stay easy to parse from C with simple bounds checks.

App identity is defined once in app.env. Build and deploy tasks are in ./app (a plain bash script) and deploy/. Run ./app help for the command list. Do not edit files under frontend/Vendor/ unless asked; app code belongs in frontend/Frontend/ and backend/.
