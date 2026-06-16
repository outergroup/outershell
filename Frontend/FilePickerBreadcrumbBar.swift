import AppKit
import QuartzCore

struct FilePickerBreadcrumbSegment {
    let title: String
    let path: String
}

enum FilePickerBreadcrumbBar {
    static let height: CGFloat = 34

    static func segments(for path: String) -> [FilePickerBreadcrumbSegment] {
        let normalizedPath = path.isEmpty ? "/" : path
        guard normalizedPath.hasPrefix("/") else {
            return [FilePickerBreadcrumbSegment(title: normalizedPath, path: normalizedPath)]
        }
        if normalizedPath == "/" {
            return [FilePickerBreadcrumbSegment(title: "/", path: "/")]
        }

        var segments = [FilePickerBreadcrumbSegment(title: "/", path: "/")]
        var accumulatedPath = ""
        for component in normalizedPath.split(separator: "/", omittingEmptySubsequences: true) {
            let title = String(component)
            accumulatedPath += "/" + title
            segments.append(FilePickerBreadcrumbSegment(title: title, path: accumulatedPath))
        }
        return segments
    }

    static func render(path: String,
                       in layer: CALayer,
                       segmentFrames: inout [(frame: CGRect, path: String)],
                       appearance: NSAppearance,
                       horizontalInset: CGFloat = 0) {
        layer.sublayers?.forEach { $0.removeFromSuperlayer() }
        segmentFrames.removeAll()

        let scale = NSScreen.main?.backingScaleFactor ?? 2
        appearance.performAsCurrentDrawingAppearance {
            var x = horizontalInset
            let segments = self.segments(for: path)
            let selectedPath = segments.last?.path
            for (index, segment) in segments.enumerated() {
                if index > 0 {
                    let separator = makeTextLayer(size: 12, weight: .regular, color: .tertiaryLabelColor, scale: scale)
                    separator.string = ">"
                    separator.alignmentMode = .center
                    separator.frame = CGRect(x: x, y: 9, width: 14, height: 16)
                    layer.addSublayer(separator)
                    x += 18
                }

                let titleWidth = ceil(textWidth(segment.title, fontSize: 12, weight: .medium))
                let segmentWidth = max(titleWidth + 18, 28)
                let segmentFrame = CGRect(x: x, y: 5, width: segmentWidth, height: 24)
                segmentFrames.append((segmentFrame, segment.path))

                let segmentLayer = CALayer()
                segmentLayer.frame = segmentFrame
                segmentLayer.cornerRadius = 5
                segmentLayer.masksToBounds = true
                if segment.path == selectedPath {
                    segmentLayer.backgroundColor = resolvedColor(NSColor.selectedContentBackgroundColor.withAlphaComponent(0.18))
                }
                layer.addSublayer(segmentLayer)

                let titleLayer = makeTextLayer(size: 12, weight: .medium, color: .secondaryLabelColor, scale: scale)
                titleLayer.string = segment.title
                titleLayer.frame = CGRect(x: 9, y: 4, width: max(segmentWidth - 18, 1), height: 16)
                segmentLayer.addSublayer(titleLayer)

                x += segmentWidth + 4
            }
        }
    }

    static func hitPath(at point: CGPoint,
                        breadcrumbFrame: CGRect,
                        segmentFrames: [(frame: CGRect, path: String)]) -> String? {
        guard breadcrumbFrame.contains(point) else { return nil }
        let localPoint = CGPoint(x: point.x - breadcrumbFrame.minX,
                                 y: point.y - breadcrumbFrame.minY)
        return segmentFrames.first(where: { $0.frame.contains(localPoint) })?.path
    }

    static func rootFrame(for localSegmentFrame: CGRect, in breadcrumbFrame: CGRect) -> CGRect {
        localSegmentFrame.offsetBy(dx: breadcrumbFrame.minX, dy: breadcrumbFrame.minY)
    }

    private static func makeTextLayer(size: CGFloat,
                                      weight: NSFont.Weight,
                                      color: NSColor,
                                      scale: CGFloat) -> CATextLayer {
        let layer = CATextLayer()
        layer.contentsScale = scale
        layer.font = NSFont.systemFont(ofSize: size, weight: weight)
        layer.fontSize = size
        layer.foregroundColor = resolvedColor(color)
        layer.truncationMode = .middle
        layer.alignmentMode = .left
        return layer
    }

    private static func textWidth(_ text: String, fontSize: CGFloat, weight: NSFont.Weight) -> CGFloat {
        let font = NSFont.systemFont(ofSize: fontSize, weight: weight)
        return NSAttributedString(string: text, attributes: [.font: font]).size().width
    }

    private static func resolvedColor(_ color: NSColor) -> CGColor {
        color.usingColorSpace(.deviceRGB)?.cgColor ?? color.cgColor
    }
}
