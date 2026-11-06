import AppKit
import QuartzCore

@MainActor
final class ElasticScrollController {
    private let host: OuterframeHost
    private var state = ElasticScrollState()
    private var surface: String?
    private var callbackID: UUID?
    private var contentLayers: (() -> [CALayer])?
    private var shiftedLayers: [CALayer] = []
    private var direction: CGFloat = 1

    init(host: OuterframeHost) {
        self.host = host
    }

    deinit {
        let host = host
        let callbackID = callbackID
        Task { @MainActor in
            if let callbackID { host.stopDisplayLinkCallback(callbackID) }
        }
    }

    func isActive(_ surface: String) -> Bool {
        self.surface == surface && (state.isTracking || state.displacement != 0)
    }

    func scroll(_ surface: String, by delta: CGFloat, offset: CGFloat, maximum: CGFloat,
                viewport: CGFloat, phase: NSEvent.Phase, momentumPhase: NSEvent.Phase,
                precise: Bool, direction: CGFloat = 1, layers: @escaping () -> [CALayer]) -> CGFloat {
        if self.surface != surface { reset() }
        self.surface = surface
        self.direction = direction
        contentLayers = layers
        let result = state.scroll(by: delta, offset: offset, maximum: maximum, viewport: viewport,
                                  phase: phase, momentumPhase: momentumPhase, precise: precise,
                                  time: CACurrentMediaTime())
        present()
        if state.displacement != 0 && callbackID == nil {
            callbackID = host.registerDisplayLinkCallback { [weak self] _ in
                guard let self else { return }
                state.advance(to: CACurrentMediaTime())
                present()
                if state.displacement == 0 { stopAnimation() }
            }
        } else if state.displacement == 0 {
            stopAnimation()
        }
        return result
    }

    func present() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for layer in shiftedLayers { layer.sublayerTransform = CATransform3DIdentity }
        shiftedLayers = contentLayers?() ?? []
        for layer in shiftedLayers {
            layer.sublayerTransform = CATransform3DMakeTranslation(0, direction * state.displacement, 0)
        }
        CATransaction.commit()
    }

    func finish(at point: CGPoint, in root: CALayer) -> CGPoint {
        var adjusted = point
        if state.displacement != 0, let layer = shiftedLayers.first {
            var ancestor: CALayer? = layer
            var inside = true
            while let candidate = ancestor, candidate !== root {
                if candidate.masksToBounds && !candidate.bounds.contains(candidate.convert(point, from: root)) {
                    inside = false
                    break
                }
                ancestor = candidate.superlayer
            }
            if inside {
                var local = layer.convert(point, from: root)
                local.y -= direction * state.displacement
                adjusted = root.convert(local, from: layer)
            }
        }
        reset()
        return adjusted
    }

    func reset() {
        stopAnimation()
        state.reset()
        surface = nil
        contentLayers = nil
        present()
    }

    private func stopAnimation() {
        if let callbackID { host.stopDisplayLinkCallback(callbackID) }
        callbackID = nil
    }
}
