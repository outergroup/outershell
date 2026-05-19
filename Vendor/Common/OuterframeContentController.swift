//
//  PluginProtocol.swift
//

import Foundation
import AppKit
import QuartzCore

@MainActor
@objc(OuterframeContentLibrary) public protocol OuterframeContentLibrary {
    /// Starts the content, creating a layer and monitoring the socket.
    /// The plugin receives configuration via a `initializeContent` message on the socket.
    /// The plugin must call `registerLayer` to register its CALayer.
    /// - Parameters:
    ///   - socketFD: The socket file descriptor for browser communication.
    ///   - appConnection: Connection for registering the CALayer.
    /// - Returns: 0 on success, non-zero error code on failure.
    @objc optional static func start(
        socketFD: Int32,
        appConnection: OuterframeAppConnection
    ) -> Int32
}
