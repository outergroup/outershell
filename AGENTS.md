This is an outerframe app for macOS. The outerframe is open-source: https://github.com/outergroup/outerframe 

An outerframe app is built on the CALayer. Everything you know about CALayers is relevant here. Most of the platform is simply macOS. This app runs in a background sandboxed non-UI process, and it uses Unix socket messages to send and receive various events. "Vendor/Common/OuterframeSwiftMethods" takes these events and surfaces them as APIs and delegate methods.
