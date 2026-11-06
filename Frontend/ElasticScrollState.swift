import AppKit

struct ElasticScrollState {
    private(set) var displacement: CGFloat = 0
    private(set) var isTracking = false
    private var ignoresMomentum = false
    private var lastInputTime: TimeInterval = 0
    private var returnStartTime: TimeInterval?
    private var returnDisplacement: CGFloat = 0

    mutating func reset() {
        self = ElasticScrollState()
    }

    mutating func scroll(by delta: CGFloat, offset: CGFloat, maximum: CGFloat, viewport: CGFloat,
                         phase: NSEvent.Phase, momentumPhase: NSEvent.Phase, precise: Bool,
                         time: TimeInterval) -> CGFloat {
        let maximum = max(maximum, 0)
        guard precise else {
            reset()
            return min(max(offset + delta, 0), maximum)
        }
        let isMomentum = !momentumPhase.isEmpty
        if !isMomentum && (phase.contains(.began) || phase.contains(.changed) || (phase.isEmpty && delta != 0)) {
            ignoresMomentum = false
            returnStartTime = nil
        }
        if phase.contains(.began) || phase.contains(.changed) { isTracking = true }
        if phase.contains(.ended) || phase.contains(.cancelled) { isTracking = false }
        if isMomentum && ignoresMomentum { return min(max(offset, 0), maximum) }

        lastInputTime = time
        let limit = max(min(viewport * 0.35, 180), 1)
        let magnitude = min(abs(displacement), limit - 0.001)
        let pull = magnitude / (0.55 * (1 - magnitude / limit)) * (displacement < 0 ? -1 : 1)
        let proposed = offset + pull + delta
        let bounded = min(max(proposed, 0), maximum)
        let excess = proposed - bounded
        displacement = excess / (1 + abs(excess) * 0.55 / limit) * 0.55

        if phase.contains(.ended) || phase.contains(.cancelled) || isMomentum {
            isTracking = false
            if displacement != 0 {
                ignoresMomentum = true
                beginReturn(at: time)
            }
        }
        return bounded
    }

    mutating func advance(to time: TimeInterval) {
        guard displacement != 0 else { return }
        if returnStartTime == nil && !isTracking && time - lastInputTime >= 0.12 {
            beginReturn(at: lastInputTime + 0.12)
        }
        guard let start = returnStartTime else { return }
        let elapsed = max(time - start, 0)
        let decay = 18 * elapsed
        displacement = returnDisplacement * CGFloat((1 + decay) * exp(-decay))
        if abs(displacement) < 0.1 {
            displacement = 0
            returnStartTime = nil
        }
    }

    private mutating func beginReturn(at time: TimeInterval) {
        guard returnStartTime == nil else { return }
        returnDisplacement = displacement
        returnStartTime = time
    }
}
