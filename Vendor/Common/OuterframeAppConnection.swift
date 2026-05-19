import AppKit

/// Minimal protocol for plugin communication back to the host.
///
/// This protocol has a single purpose: allowing the plugin to register its root CALayer,
/// which creates the CAContext and notifies the browser that the plugin is ready.
///
/// All other data (URLs, proxy configuration, appearance) is passed in the
/// `createContentController` parameters. Plugins should store this data at initialization.
///
/// For runtime communication (cursor updates, input mode, display link, etc.), plugins
/// use the socket directly via the `socketFD` parameter passed to `createContentController`.
@MainActor
@objc(OuterframeAppConnection) public protocol OuterframeAppConnection: AnyObject {

    /// Announce the plugin's root CALayer to the host.
    /// This creates the CAContext and sends the pluginLoaded message to the browser
    /// on the infrastructure socket.
    /// - Parameter layer: The plugin's root CALayer
    @objc optional func registerLayer(_ layer: CALayer)
}
