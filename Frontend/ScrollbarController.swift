import AppKit
import QuartzCore

@MainActor
protocol ScrollbarControllerDelegate: AnyObject {
    func scrollbarDidChangeScrollOffset(_ offset: CGFloat)
    func scrollbarDidChangeMagnification(_ magnification: CGFloat)
    func scrollbarDidEndMagnification(_ magnification: CGFloat)
}

extension ScrollbarControllerDelegate {
    func scrollbarDidChangeMagnification(_ magnification: CGFloat) {}
    func scrollbarDidEndMagnification(_ magnification: CGFloat) {}
}

@MainActor
final class ScrollbarController<Delegate: ScrollbarControllerDelegate> {
    struct Metrics {
        let viewportSize: CGSize
        let contentHeight: CGFloat
        let scrollOffset: CGFloat
        let magnification: CGFloat?

        init(viewportSize: CGSize, contentHeight: CGFloat, scrollOffset: CGFloat, magnification: CGFloat? = nil) {
            self.viewportSize = viewportSize
            self.contentHeight = contentHeight
            self.scrollOffset = scrollOffset
            self.magnification = magnification
        }
    }

    struct ColorConfiguration {
        let trackColor: CGColor
        let knobColor: CGColor

        static var fallback: ColorConfiguration {
            let track = NSColor.unemphasizedSelectedTextBackgroundColor.withAlphaComponent(0.4).cgColor
            let knob = NSColor.secondaryLabelColor.withAlphaComponent(0.75).cgColor
            return ColorConfiguration(trackColor: track, knobColor: knob)
        }

        init(trackColor: CGColor, knobColor: CGColor) {
            self.trackColor = trackColor
            self.knobColor = knobColor
        }

        init(appearance: NSAppearance) {
            var resolvedTrack = Self.fallback.trackColor
            var resolvedKnob = Self.fallback.knobColor

            appearance.performAsCurrentDrawingAppearance {
                let controlBackground = NSColor.controlBackgroundColor
                let label = NSColor.labelColor
                let secondaryLabel = NSColor.secondaryLabelColor
                let inactiveSelection = NSColor.unemphasizedSelectedTextBackgroundColor
                let isLightTheme = controlBackground.ol_brightness > 0.6
                let fallbackTrackBase = controlBackground.ol_blended(withFraction: isLightTheme ? 0.25 : 0.45, toward: label)
                let trackBaseColor = inactiveSelection.ol_isApproxEqual(to: controlBackground) ? fallbackTrackBase : inactiveSelection
                let trackAlpha: CGFloat = isLightTheme ? 0.35 : 0.6
                let knobAlpha: CGFloat = isLightTheme ? 0.75 : 0.85
                resolvedTrack = trackBaseColor.ol_withAlpha(trackAlpha).cgColor
                resolvedKnob = secondaryLabel.ol_withAlpha(knobAlpha).cgColor
            }

            trackColor = resolvedTrack
            knobColor = resolvedKnob
        }
    }

    enum ScrollOffsetOrigin {
        case top
        case bottom
    }

    private let appConnection: OuterframeHost
    private weak var viewportLayer: CALayer?
    private let width: CGFloat
    private let inset: CGFloat
    private let minMagnification: CGFloat
    private let maxMagnification: CGFloat
    private let scrollOffsetOrigin: ScrollOffsetOrigin
    private let supportsMagnification: Bool
    private var colorConfiguration: ColorConfiguration
    private var currentMetrics: Metrics?
    var delegate: Delegate!

    private var containerLayer: CALayer
    private var trackLayer: CALayer
    private var knobLayer: CALayer

    private var isDragging = false
    private var dragOffset: CGFloat = 0

    private var animationTargetOffset: CGFloat?
    private var animationStartOffset: CGFloat = 0
    private var animationStartTime: CFTimeInterval = 0
    private let animationDuration: CFTimeInterval = 0.18
    private var displayLinkCallbackID: UUID?

    init(appConnection: OuterframeHost,
         viewportLayer: CALayer,
         appearance: NSAppearance,
         width: CGFloat = 8,
         inset: CGFloat = 4,
         supportsMagnification: Bool = false,
         magnificationRange: ClosedRange<CGFloat> = 0.5...3.0,
         scrollOffsetOrigin: ScrollOffsetOrigin = .top) {
        self.appConnection = appConnection
        self.viewportLayer = viewportLayer
        self.width = width
        self.inset = inset
        self.supportsMagnification = supportsMagnification
        self.scrollOffsetOrigin = scrollOffsetOrigin
        if magnificationRange.isEmpty {
            self.minMagnification = 1.0
            self.maxMagnification = 1.0
        } else {
            self.minMagnification = max(0.05, magnificationRange.lowerBound)
            self.maxMagnification = max(self.minMagnification, magnificationRange.upperBound)
        }

        let containerLayer = CALayer()
        containerLayer.masksToBounds = false
        containerLayer.opacity = 0.9
        containerLayer.zPosition = 200
        viewportLayer.addSublayer(containerLayer)
        self.containerLayer = containerLayer

        let trackLayer = CALayer()
        trackLayer.cornerRadius = width * 0.5
        containerLayer.addSublayer(trackLayer)
        self.trackLayer = trackLayer

        let knobLayer = CALayer()
        knobLayer.cornerRadius = width * 0.5
        knobLayer.masksToBounds = true
        trackLayer.addSublayer(knobLayer)
        self.knobLayer = knobLayer
        self.colorConfiguration = ColorConfiguration(appearance: appearance)
        applyColorConfiguration()
    }

    func updateLayout(metrics: Metrics) {
        currentMetrics = metrics
        layoutScrollbar(scrollOffset: metrics.scrollOffset)
    }

    private func layoutScrollbar(scrollOffset: CGFloat) {
        guard let viewport = viewportLayer,
              let metrics = currentMetrics else {
            hideScrollbar()
            return
        }

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }

        let trackHeight = max(metrics.viewportSize.height - inset * 2, 0)
        if trackHeight <= 0 {
            containerLayer.isHidden = true
            knobLayer.frame = .zero
            return
        }

        let containerY: CGFloat
        if viewport.contentsAreFlipped() {
            containerY = inset
        } else {
            containerY = max(metrics.viewportSize.height - inset - trackHeight, 0)
        }

        containerLayer.frame = CGRect(x: metrics.viewportSize.width - inset - width,
                                      y: containerY,
                                      width: width,
                                      height: trackHeight)
        containerLayer.isHidden = false

        trackLayer.frame = CGRect(origin: .zero, size: containerLayer.bounds.size)

        let maxOffset = max(metrics.contentHeight - metrics.viewportSize.height, 0)
        if maxOffset <= 0.5 {
            containerLayer.isHidden = true
            knobLayer.frame = .zero
            return
        }

        let knobProportion = min(max(metrics.viewportSize.height / metrics.contentHeight, 0.05), 1.0)
        let desiredKnobHeight = trackLayer.bounds.height * knobProportion
        let knobHeight = min(max(desiredKnobHeight, 12), trackLayer.bounds.height)
        let knobRange = max(trackLayer.bounds.height - knobHeight, 0)
        let normalizedOffset = normalizedOffset(forScrollOffset: scrollOffset,
                                                maxOffset: maxOffset)
        let knobY = knobRange * normalizedOffset

        knobLayer.frame = CGRect(x: 0,
                            y: knobY,
                            width: trackLayer.bounds.width,
                            height: knobHeight)
    }

    @discardableResult
    func handleMouseDown(at viewportPoint: CGPoint) -> Bool {
        guard let viewport = viewportLayer,
              let metrics = currentMetrics,
              containerLayer.isHidden == false else {
            return false
        }

        let convertedPoint = trackLayer.convert(viewportPoint, from: viewport)
        let clampedY = min(max(convertedPoint.y, 0), trackLayer.bounds.height)

        let knobFrame = knobLayer.frame
        if knobFrame.contains(CGPoint(x: convertedPoint.x, y: clampedY)) {
            cancelAnimation()
            isDragging = true
            dragOffset = min(max(clampedY - knobFrame.origin.y, 0), knobFrame.height)
            return true
        }

        if trackLayer.bounds.contains(CGPoint(x: convertedPoint.x, y: clampedY)) {
            cancelAnimation()
            let targetOffset = targetOffsetForTrackClick(knobHeight: knobLayer.bounds.height,
                                                         trackHeight: trackLayer.bounds.height,
                                                         clampedY: clampedY,
                                                         metrics: metrics)
            startAnimation(to: targetOffset)
            return true
        }

        return false
    }

    @discardableResult
    func handleMouseDragged(to viewportPoint: CGPoint) -> Bool {
        guard isDragging,
              let viewport = viewportLayer,
              let metrics = currentMetrics else {
            return false
        }

        let convertedPoint = trackLayer.convert(viewportPoint, from: viewport)
        let clampedY = min(max(convertedPoint.y, 0), trackLayer.bounds.height)

        let knobHeight = knobLayer.bounds.height
        let knobRange = max(trackLayer.bounds.height - knobHeight, 0)
        if knobRange <= 0.0001 {
            return false
        }

        var knobY = clampedY - dragOffset
        knobY = constrain(knobY, range: knobRange)
        let normalized = knobRange > 0 ? knobY / knobRange : 0
        let maxOffset = max(metrics.contentHeight - metrics.viewportSize.height, 0)
        let newOffset = constrain(scrollOffset(forNormalized: normalized, maxOffset: maxOffset),
                                  range: maxOffset)

        cancelAnimation()
        delegate.scrollbarDidChangeScrollOffset( newOffset)
        layoutScrollbar(scrollOffset: newOffset)
        return true
    }

    @discardableResult
    func handleMouseUp(at viewportPoint: CGPoint) -> Bool {
        isDragging = false
        dragOffset = 0
        return false
    }

    @discardableResult
    func handleMagnificationChanged(magnification: CGFloat,
                                    at pointInRoot: CGPoint,
                                    rootLayer: CALayer) -> Bool {
        guard supportsMagnification,
              let viewport = viewportLayer,
              let metrics = currentMetrics,
              let currentMagnification = metrics.magnification else {
            return false
        }

        let clampedMagnification = clampMagnification(magnification)
        if abs(clampedMagnification - currentMagnification) < 0.0001 {
            return false
        }

        cancelAnimation()

        let viewportPoint = viewport.convert(pointInRoot, from: rootLayer)
        let topBasedY = topBasedCoordinate(y: viewportPoint.y, in: viewport)
        let locationInViewport = constrain(topBasedY,
                                           lower: 0,
                                           upper: viewport.bounds.height)

        let safeCurrentMagnification = max(currentMagnification, 0.0001)
        let logicalPosition = (metrics.scrollOffset + locationInViewport) / safeCurrentMagnification
        let newScrollOffset = logicalPosition * clampedMagnification - locationInViewport

        let ratio = clampedMagnification / safeCurrentMagnification
        let estimatedContentHeight = max(metrics.contentHeight * ratio, metrics.viewportSize.height)
        let maxOffset = max(estimatedContentHeight - metrics.viewportSize.height, 0)
        let clampedOffset = constrain(newScrollOffset, lower: 0, upper: maxOffset)

        delegate.scrollbarDidChangeMagnification( clampedMagnification)
        delegate.scrollbarDidChangeScrollOffset( clampedOffset)
        layoutScrollbar(scrollOffset: clampedOffset)
        return true
    }

    @discardableResult
    func handleMagnificationEnded(magnification: CGFloat,
                                  at pointInRoot: CGPoint,
                                  rootLayer: CALayer) -> Bool {
        guard supportsMagnification else { return false }
        let handled = handleMagnificationChanged(magnification: magnification,
                                                 at: pointInRoot,
                                                 rootLayer: rootLayer)
        let finalMagnification = clampMagnification(magnification)
        delegate.scrollbarDidEndMagnification( finalMagnification)
        return handled
    }

    func cancelAnimation() {
        animationTargetOffset = nil
        stopDisplayLinkIfNeeded(force: true)
    }

    func cleanup() {
        cancelAnimation()
        containerLayer.removeFromSuperlayer()
    }

    func updateAppearance(_ appearance: NSAppearance) {
        colorConfiguration = ColorConfiguration(appearance: appearance)
        applyColorConfiguration()
    }

    // MARK: - Private helpers

    private func hideScrollbar() {
        containerLayer.isHidden = true
    }

    private func applyColorConfiguration() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        trackLayer.backgroundColor = colorConfiguration.trackColor
        knobLayer.backgroundColor = colorConfiguration.knobColor
        CATransaction.commit()
    }

    private func startAnimation(to targetOffset: CGFloat) {
        guard let metrics = currentMetrics else { return }
        let maxOffset = max(metrics.contentHeight - metrics.viewportSize.height, 0)
        let clampedTarget = constrain(targetOffset, range: maxOffset)
        if abs(clampedTarget - metrics.scrollOffset) < 0.5 {
            cancelAnimation()
            delegate.scrollbarDidChangeScrollOffset( clampedTarget)
            layoutScrollbar(scrollOffset: clampedTarget)
            return
        }

        animationStartOffset = metrics.scrollOffset
        animationTargetOffset = clampedTarget
        animationStartTime = CACurrentMediaTime()
        ensureDisplayLinkActive()
    }

    private func ensureDisplayLinkActive() {
        guard displayLinkCallbackID == nil else { return }
        displayLinkCallbackID = appConnection.registerDisplayLinkCallback { [weak self] timestamp in
            self?.handleDisplayLinkTick(timestamp: timestamp)
        }
    }

    private func stopDisplayLinkIfNeeded(force: Bool = false) {
        guard let callbackID = displayLinkCallbackID else { return }
        if !force && animationTargetOffset != nil { return }
        appConnection.stopDisplayLinkCallback(callbackID)
        displayLinkCallbackID = nil
    }

    private func handleDisplayLinkTick(timestamp: CFTimeInterval) {
        guard let target = animationTargetOffset else {
            stopDisplayLinkIfNeeded(force: true)
            return
        }

        let now = CACurrentMediaTime()
        let duration = max(animationDuration, 0.0001)
        let elapsed = max(0, min(now - animationStartTime, duration))
        let progress = CGFloat(elapsed / duration)
        let eased = progress

        let currentOffset = animationStartOffset + (target - animationStartOffset) * eased
        delegate.scrollbarDidChangeScrollOffset( currentOffset)
        layoutScrollbar(scrollOffset: currentOffset)

        if progress >= 1 - 0.0001 {
            animationTargetOffset = nil
            delegate.scrollbarDidChangeScrollOffset( target)
            layoutScrollbar(scrollOffset: target)
            stopDisplayLinkIfNeeded(force: true)
        }
    }

    private func targetOffsetForTrackClick(knobHeight: CGFloat,
                                           trackHeight: CGFloat,
                                           clampedY: CGFloat,
                                           metrics: Metrics) -> CGFloat {
        let knobRange = max(trackHeight - knobHeight, 0)
        if knobRange <= 0.0001 {
            return 0
        }
        let centeredKnobY = constrain(clampedY - knobHeight * 0.5, range: knobRange)
        let normalized = knobRange > 0 ? centeredKnobY / knobRange : 0
        let maxOffset = max(metrics.contentHeight - metrics.viewportSize.height, 0)
        return scrollOffset(forNormalized: normalized, maxOffset: maxOffset)
    }

    private func constrain(_ value: CGFloat, range: CGFloat) -> CGFloat {
        if range <= 0 { return 0 }
        if value <= 0 { return 0 }
        if value >= range { return range }
        return value
    }

    private func constrain(_ value: CGFloat, lower: CGFloat, upper: CGFloat) -> CGFloat {
        if value <= lower { return lower }
        if value >= upper { return upper }
        return value
    }

    private func clampMagnification(_ value: CGFloat) -> CGFloat {
        if value.isNaN || value.isZero {
            return min(max(1.0, minMagnification), maxMagnification)
        }
        if value <= minMagnification { return minMagnification }
        if value >= maxMagnification { return maxMagnification }
        return value
    }

    private func topBasedCoordinate(y: CGFloat, in layer: CALayer) -> CGFloat {
        if layer.isGeometryFlipped {
            return y
        }
        return layer.bounds.height - y
    }

    private func normalizedOffset(forScrollOffset scrollOffset: CGFloat, maxOffset: CGFloat) -> CGFloat {
        guard maxOffset > 0 else { return 0 }
        let ratio = min(max(scrollOffset / maxOffset, 0), 1)
        switch scrollOffsetOrigin {
        case .top:
            return ratio
        case .bottom:
            return 1 - ratio
        }
    }

    private func scrollOffset(forNormalized normalized: CGFloat, maxOffset: CGFloat) -> CGFloat {
        if maxOffset <= 0 { return 0 }
        let clamped = min(max(normalized, 0), 1)
        let ratio: CGFloat
        switch scrollOffsetOrigin {
        case .top:
            ratio = clamped
        case .bottom:
            ratio = 1 - clamped
        }
        return ratio * maxOffset
    }
}

private extension NSColor {
    var ol_brightness: CGFloat {
        let rgb = usingColorSpace(.deviceRGB) ?? self
        return (rgb.redComponent + rgb.greenComponent + rgb.blueComponent) / 3.0
    }

    func ol_withAlpha(_ alpha: CGFloat) -> NSColor {
        withAlphaComponent(alpha)
    }

    func ol_blended(withFraction fraction: CGFloat, toward other: NSColor) -> NSColor {
        blended(withFraction: fraction, of: other) ?? self
    }

    func ol_isApproxEqual(to other: NSColor) -> Bool {
        guard let lhs = usingColorSpace(.deviceRGB),
              let rhs = other.usingColorSpace(.deviceRGB) else {
            return isEqual(other)
        }
        let epsilon: CGFloat = 0.001
        return abs(lhs.redComponent - rhs.redComponent) < epsilon &&
               abs(lhs.greenComponent - rhs.greenComponent) < epsilon &&
               abs(lhs.blueComponent - rhs.blueComponent) < epsilon &&
               abs(lhs.alphaComponent - rhs.alphaComponent) < epsilon
    }
}
