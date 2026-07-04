import AppKit
import QuartzCore

@MainActor
@objc public final class HelloFullstackContent: NSObject, OuterframeContentLibrary {
    @objc public static func start(
        socketFD: Int32,
        appConnection: OuterframeAppConnection
    ) -> Int32 {
        let outerframeHost = OuterframeHost(socketFD: socketFD)
        let handler = HelloFullstackHandler(outerframeHost: outerframeHost, appConnection: appConnection)
        outerframeHost.delegate = handler
        return 0
    }
}

@MainActor
private final class HelloFullstackHandler: NSObject, OuterframeHostDelegate {
    private let outerframeHost: OuterframeHost
    private let appConnection: OuterframeAppConnection
    private var retainedSelf: HelloFullstackHandler?

    private var appearance: NSAppearance?
    private let rootLayer = CALayer()
    private let titleLayer = CATextLayer()
    private let subtitleLayer = CATextLayer()
    private var didRegisterLayer = false
    private var didStartBackendFetch = false
    private var currentSize = CGSize(width: 800, height: 600)

    init(outerframeHost: OuterframeHost, appConnection: OuterframeAppConnection) {
        self.outerframeHost = outerframeHost
        self.appConnection = appConnection
        super.init()
        retainedSelf = self
    }

    func outerframeHost(_ host: OuterframeHost, didReceiveMessage message: BrowserToContentMessage) {
        switch message {
        case .initializeContent(let arguments):
            outerframeHost.configure(url: arguments.url ?? "",
                                     bundleUrl: arguments.bundleUrl ?? "",
                                     proxyHost: arguments.proxy?.host,
                                     proxyPort: arguments.proxy?.port ?? 0,
                                     proxyUsername: arguments.proxy?.username,
                                     proxyPassword: arguments.proxy?.password)
            outerframeHost.setTitle("Hello World")
            outerframeHost.setIcon(.bundleResource(path: "Contents/Resources/app-icon.png"))
            appearance = arguments.appearance ?? NSAppearance.currentDrawing()

            currentSize = arguments.contentSize ?? CGSize(width: 800, height: 600)
            configureLayersIfNeeded()
            updateLayout()
            updateColors()
            registerRootLayerIfNeeded()
            fetchBackendGreetingIfNeeded()

        case .resizeContent(let size):
            currentSize = size
            updateLayout()

        case .systemAppearanceUpdate(let appearance):
            self.appearance = appearance
            updateColors()

        case .accessibilitySnapshotRequest(let requestID):
            outerframeHost.sendAccessibilitySnapshotResponse(requestID: requestID,
                                                             snapshot: accessibilitySnapshot())

        case .shutdown:
            retainedSelf = nil

        default:
            break
        }
    }

    func outerframeHostDidDisconnect(_ host: OuterframeHost) {
        retainedSelf = nil
    }

    // MARK: - Backend API

    /// The response shape served by `backend/main.go` at `/api/hello`.
    private struct HelloResponse: Decodable {
        let message: String
        let hostname: String
        let os: String
    }

    /// Fetches a greeting from this app's own backend. The request goes to the
    /// same origin the `.outer` file was served from, tunneled through Outer
    /// Loop's SOCKS proxy (i.e. over the SSH connection to the server).
    private func fetchBackendGreetingIfNeeded() {
        guard !didStartBackendFetch else { return }
        didStartBackendFetch = true

        guard let origin = outerframeHost.pluginOriginURL() else {
            subtitleLayer.string = "No origin URL available."
            return
        }
        let apiURL = origin.appendingPathComponent("api/hello")

        let configuration = URLSessionConfiguration.ephemeral
        outerframeHost.applyProxy(to: configuration)
        let session = URLSession(configuration: configuration)

        subtitleLayer.string = "Asking the backend for a greeting…"

        Task {
            do {
                let (data, _) = try await session.data(from: apiURL)
                let hello = try JSONDecoder().decode(HelloResponse.self, from: data)
                self.subtitleLayer.string = "\(hello.message) — \(hello.os) on \(hello.hostname)"
            } catch {
                self.subtitleLayer.string = "Backend request failed: \(error.localizedDescription)"
            }
        }
    }

    // MARK: - Accessibility

    private func accessibilitySnapshot() -> OuterframeAccessibilitySnapshot? {
        let titleNode = OuterframeAccessibilityNode(identifier: 1,
                                                    role: .staticText,
                                                    frame: titleLayer.frame,
                                                    label: titleLayer.string as? String ?? "Hello, world!")
        let subtitleNode = OuterframeAccessibilityNode(identifier: 2,
                                                       role: .staticText,
                                                       frame: subtitleLayer.frame,
                                                       label: subtitleLayer.string as? String ?? "")
        let rootNode = OuterframeAccessibilityNode(identifier: 0,
                                                   role: .container,
                                                   frame: rootLayer.frame,
                                                   label: "Hello world outerframe app",
                                                   children: [titleNode, subtitleNode])
        return OuterframeAccessibilitySnapshot(rootNodes: [rootNode])
    }

    // MARK: - Layers

    private func configureLayersIfNeeded() {
        guard titleLayer.superlayer == nil else { return }

        titleLayer.string = "Hello, world!"
        titleLayer.font = NSFont.systemFont(ofSize: 34, weight: .semibold)
        titleLayer.fontSize = 34
        titleLayer.alignmentMode = .center
        titleLayer.contentsScale = 2.0
        titleLayer.isWrapped = true

        subtitleLayer.string = "Starting…"
        subtitleLayer.font = NSFont.systemFont(ofSize: 15, weight: .regular)
        subtitleLayer.fontSize = 15
        subtitleLayer.alignmentMode = .center
        subtitleLayer.contentsScale = 2.0
        subtitleLayer.isWrapped = true

        rootLayer.addSublayer(titleLayer)
        rootLayer.addSublayer(subtitleLayer)
    }

    private func updateLayout() {
        let width = max(currentSize.width, 1)
        let height = max(currentSize.height, 1)

        CATransaction.begin()
        CATransaction.setDisableActions(true)

        rootLayer.frame = CGRect(x: 0, y: 0, width: width, height: height)

        let horizontalPadding = min(max(width * 0.1, 24), 80)
        titleLayer.frame = CGRect(x: horizontalPadding,
                                  y: height * 0.5,
                                  width: width - (horizontalPadding * 2),
                                  height: 44)
        subtitleLayer.frame = CGRect(x: horizontalPadding,
                                     y: max(titleLayer.frame.minY - 48, 24),
                                     width: width - (horizontalPadding * 2),
                                     height: 40)

        CATransaction.commit()
    }

    private func updateColors() {
        appearance?.performAsCurrentDrawingAppearance {
            rootLayer.backgroundColor = NSColor.windowBackgroundColor.cgColor
            titleLayer.foregroundColor = NSColor.labelColor.cgColor
            subtitleLayer.foregroundColor = NSColor.secondaryLabelColor.cgColor
        }
    }

    private func registerRootLayerIfNeeded() {
        guard !didRegisterLayer else { return }
        guard let registerLayer = appConnection.registerLayer else { return }
        registerLayer(rootLayer)
        didRegisterLayer = true
    }
}
