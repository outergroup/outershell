import AppKit
import Darwin
import Foundation

private struct NativeAppProjectFileError: LocalizedError {
    let action: String
    let path: String
    let errnoValue: Int32

    var errorDescription: String? {
        "\(action) failed for \(path): \(String(cString: strerror(errnoValue)))"
    }
}

struct NativeAppProjectConfiguration {
    enum PlatformTarget: String, CaseIterable {
        case html
        case macos
    }

    enum FrontendLanguage: String {
        case swift
        case objc
    }

    enum BackendLanguage: String {
        case go
        case c
    }

    enum IsolationMode: String {
        case container
        case host
    }

    let appName: String
    let appID: String
    let xcodeScheme: String
    let projectRootPath: String
    let projectFolderName: String
    let socketFilename: String
    let platformTargets: Set<PlatformTarget>
    let frontendLanguage: FrontendLanguage
    let backendLanguage: BackendLanguage
    let isolationMode: IsolationMode
}

struct GeneratedNativeAppProject {
    let stagingRootURL: URL
    let projectURL: URL?
    let folderName: String
    let remoteProjectPath: String

    var hasPlatformWorkspace: Bool {
        projectURL != nil
    }
}

enum NativeAppProjectGeneratorError: LocalizedError {
    case missingStagingDirectory
    case invalidName
    case invalidResponse

    var errorDescription: String? {
        switch self {
        case .missingStagingDirectory:
            return "Outerframe staging directory is unavailable."
        case .invalidName:
            return "Project name is invalid."
        case .invalidResponse:
            return "The Outer Shell backend returned an invalid project."
        }
    }
}

enum NativeAppProjectGenerator {
    static func materializeProjectResponse(_ data: Data,
                                           configuration: NativeAppProjectConfiguration,
                                           stagingDirectory: URL,
                                           sshCommandArguments: [String]) throws -> GeneratedNativeAppProject {
        let folderName = safePathComponent(configuration.projectFolderName)
        guard !folderName.isEmpty else { throw NativeAppProjectGeneratorError.invalidName }

        let stagingRoot = stagingDirectory
            .appendingPathComponent("native-app-projects", isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        do {
            try createWritableDirectory(at: stagingRoot)
            try materializeArchive(data, to: stagingRoot)
            let remotePathURL = stagingRoot.appendingPathComponent(".outershell-remote-project-path")
            let remotePath = try String(contentsOf: remotePathURL, encoding: .utf8)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            guard !remotePath.isEmpty else { throw NativeAppProjectGeneratorError.invalidResponse }

            let builderURL = stagingRoot
                .appendingPathComponent("builder", isDirectory: true)
                .appendingPathComponent("\(folderName)-macOS", isDirectory: true)
            let hasBuilder = FileManager.default.fileExists(atPath: builderURL.path)
            if hasBuilder {
                try writeConnectionEnvironment(workspaceURL: builderURL,
                                               sshCommandArguments: sshCommandArguments)
                try makeProjectWritable(builderURL)
            }
            return GeneratedNativeAppProject(stagingRootURL: stagingRoot,
                                             projectURL: hasBuilder ? builderURL : nil,
                                             folderName: hasBuilder ? builderURL.lastPathComponent : folderName,
                                             remoteProjectPath: remotePath)
        } catch {
            try? FileManager.default.removeItem(at: stagingRoot)
            throw error
        }
    }

    static func generatedIconPNGData(appName: String, appID: String) throws -> Data {
        let points = 1024
        let size = NSSize(width: points, height: points)
        let seedText = "\(appID)\n\(appName)"
        var generator = SeededGenerator(seed: fnv1a64(seedText))
        let palette = iconPalette(seed: &generator)
        let monogram = iconMonogram(from: appName)

        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil,
                                            pixelsWide: points,
                                            pixelsHigh: points,
                                            bitsPerSample: 8,
                                            samplesPerPixel: 4,
                                            hasAlpha: true,
                                            isPlanar: false,
                                            colorSpaceName: .deviceRGB,
                                            bytesPerRow: 0,
                                            bitsPerPixel: 0),
              let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
            throw NativeAppProjectGeneratorError.invalidName
        }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        defer { NSGraphicsContext.restoreGraphicsState() }

        let bounds = NSRect(origin: .zero, size: size)
        NSColor.clear.setFill()
        NSBezierPath(rect: bounds).fill()

        let backgroundPath = NSBezierPath(roundedRect: bounds.insetBy(dx: 72, dy: 72),
                                          xRadius: 210,
                                          yRadius: 210)
        backgroundPath.addClip()
        NSGradient(colors: [palette.backgroundA, palette.backgroundB])!
            .draw(in: bounds, angle: CGFloat(generator.nextInt(0..<360)))

        for _ in 0..<18 {
            let diameter = CGFloat(generator.nextInt(90..<340))
            let x = CGFloat(generator.nextInt(-90..<980))
            let y = CGFloat(generator.nextInt(-90..<980))
            palette.accent(generator.nextInt(0..<palette.accents.count))
                .withAlphaComponent(CGFloat(generator.nextInt(10..<24)) / 100)
                .setFill()
            NSBezierPath(ovalIn: NSRect(x: x, y: y, width: diameter, height: diameter)).fill()
        }

        for _ in 0..<7 {
            let width = CGFloat(generator.nextInt(130..<360))
            let height = CGFloat(generator.nextInt(34..<92))
            let x = CGFloat(generator.nextInt(-80..<850))
            let y = CGFloat(generator.nextInt(80..<850))
            palette.accent(generator.nextInt(0..<palette.accents.count))
                .withAlphaComponent(CGFloat(generator.nextInt(26..<58)) / 100)
                .setFill()
            NSBezierPath(roundedRect: NSRect(x: x, y: y, width: width, height: height),
                         xRadius: height / 2,
                         yRadius: height / 2).fill()
        }

        palette.foreground.withAlphaComponent(0.18).setStroke()
        for index in 0..<6 {
            let offset = CGFloat(170 + index * 92 + generator.nextInt(-20..<20))
            let path = NSBezierPath()
            path.lineWidth = CGFloat(generator.nextInt(12..<24))
            path.move(to: NSPoint(x: 120, y: offset))
            path.curve(to: NSPoint(x: 904, y: offset + CGFloat(generator.nextInt(-80..<80))),
                       controlPoint1: NSPoint(x: 330, y: offset + CGFloat(generator.nextInt(-160..<160))),
                       controlPoint2: NSPoint(x: 660, y: offset + CGFloat(generator.nextInt(-160..<160))))
            path.stroke()
        }

        let centralInset = CGFloat(generator.nextInt(215..<270))
        let centralFrame = bounds.insetBy(dx: centralInset, dy: centralInset)
        let centralPath = NSBezierPath(roundedRect: centralFrame, xRadius: 88, yRadius: 88)
        palette.panel.withAlphaComponent(0.74).setFill()
        centralPath.fill()
        palette.foreground.withAlphaComponent(0.14).setStroke()
        centralPath.lineWidth = 9
        centralPath.stroke()

        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = .center
        let fontSize: CGFloat = monogram.count <= 1 ? 300 : 250
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: fontSize, weight: .black),
            .foregroundColor: palette.foreground,
            .paragraphStyle: paragraph,
            .kern: -8
        ]
        let textHeight = fontSize * 1.05
        (monogram as NSString).draw(with: NSRect(x: centralFrame.minX,
                                                 y: centralFrame.midY - textHeight / 2 - 8,
                                                 width: centralFrame.width,
                                                 height: textHeight),
                                    options: [.usesLineFragmentOrigin],
                                    attributes: attributes)

        guard let pngData = bitmap.representation(using: .png, properties: [:]) else {
            throw NativeAppProjectGeneratorError.invalidName
        }
        return pngData
    }

    static func makeProjectWritable(_ projectURL: URL) throws {
        let fileManager = FileManager.default
        try makeItemWritable(projectURL)
        guard let enumerator = fileManager.enumerator(at: projectURL,
                                                      includingPropertiesForKeys: [.isDirectoryKey],
                                                      options: []) else { return }
        for case let itemURL as URL in enumerator {
            try makeItemWritable(itemURL)
        }
    }

    static func safePathComponent(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_."))
        var result = ""
        for scalar in value.unicodeScalars {
            if allowed.contains(scalar) {
                result.unicodeScalars.append(scalar)
            } else if scalar.properties.isWhitespace || scalar.value == 47 || scalar.value == 58 {
                result.append("-")
            }
        }
        while result.contains("--") {
            result = result.replacingOccurrences(of: "--", with: "-")
        }
        return result.trimmingCharacters(in: CharacterSet(charactersIn: "-. "))
    }

    private static func materializeArchive(_ data: Data, to root: URL) throws {
        var reader = TemplateArchiveReader(data: data)
        guard reader.readMagic() else { throw NativeAppProjectGeneratorError.invalidResponse }
        while let entry = try reader.readEntry() {
            guard isSafeRelativePath(entry.path) else {
                throw NativeAppProjectGeneratorError.invalidResponse
            }
            let destination = root.appendingPathComponent(entry.path)
            try createWritableDirectory(at: destination.deletingLastPathComponent())
            try writeFile(entry.contents,
                          to: destination,
                          permissions: entry.permissions | 0o644)
        }
    }

    private static func writeConnectionEnvironment(workspaceURL: URL,
                                                   sshCommandArguments: [String]) throws {
        let body: String
        if sshCommandArguments.isEmpty {
            body = """
            # This workspace was generated from a local Outer Loop session.
            OUTER_SERVER_KIND=local
            OUTER_SERVER_SSH=()
            """
        } else {
            body = """
            # Generated by Outer Shell from Outer Loop's SSH connection arguments.
            OUTER_SERVER_KIND=ssh
            OUTER_SERVER_SSH=(
            \(sshCommandArguments.map { "    \(shellSingleQuotedValue($0))" }.joined(separator: "\n"))
            )
            """
        }
        try writeFile(Data(body.utf8),
                      to: workspaceURL.appendingPathComponent("connection.env"),
                      permissions: 0o644)
    }

    private static func createWritableDirectory(at url: URL) throws {
        try FileManager.default.createDirectory(at: url,
                                                withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o755])
        try makeItemWritable(url)
    }

    private static func writeFile(_ data: Data, to url: URL, permissions: Int) throws {
        let fileManager = FileManager.default
        if fileManager.fileExists(atPath: url.path) {
            try fileManager.removeItem(at: url)
        }
        let fd = open(url.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      mode_t(permissions & 0o777))
        guard fd >= 0 else {
            throw NativeAppProjectFileError(action: "open", path: url.path, errnoValue: errno)
        }
        do {
            try data.withUnsafeBytes { rawBuffer in
                guard let baseAddress = rawBuffer.baseAddress else { return }
                var offset = 0
                while offset < rawBuffer.count {
                    let written = Darwin.write(fd,
                                               baseAddress.advanced(by: offset),
                                               rawBuffer.count - offset)
                    if written < 0 {
                        if errno == EINTR { continue }
                        throw NativeAppProjectFileError(action: "write",
                                                        path: url.path,
                                                        errnoValue: errno)
                    }
                    if written == 0 {
                        throw NativeAppProjectFileError(action: "write",
                                                        path: url.path,
                                                        errnoValue: EIO)
                    }
                    offset += written
                }
            }
            if fchmod(fd, mode_t(permissions & 0o777)) != 0 {
                throw NativeAppProjectFileError(action: "chmod", path: url.path, errnoValue: errno)
            }
        } catch {
            close(fd)
            throw error
        }
        if close(fd) != 0 {
            throw NativeAppProjectFileError(action: "close", path: url.path, errnoValue: errno)
        }
    }

    private static func makeItemWritable(_ url: URL) throws {
        let fileManager = FileManager.default
        let attributes = try fileManager.attributesOfItem(atPath: url.path)
        let permissions = (attributes[.posixPermissions] as? NSNumber)?.intValue ?? 0o644
        let isDirectory = (attributes[.type] as? FileAttributeType) == .typeDirectory
        try fileManager.setAttributes([.posixPermissions: permissions | (isDirectory ? 0o755 : 0o644)],
                                      ofItemAtPath: url.path)
    }

    private static func isSafeRelativePath(_ path: String) -> Bool {
        if path.isEmpty || path.hasPrefix("/") || path.contains("\0") { return false }
        let components = path.split(separator: "/", omittingEmptySubsequences: false)
        return !components.contains { $0 == ".." || $0.isEmpty }
    }

    private static func shellSingleQuotedValue(_ value: String) -> String {
        "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
    }

    private struct IconPalette {
        let backgroundA: NSColor
        let backgroundB: NSColor
        let panel: NSColor
        let foreground: NSColor
        let accents: [NSColor]

        func accent(_ index: Int) -> NSColor {
            accents[index % accents.count]
        }
    }

    private static func iconPalette(seed generator: inout SeededGenerator) -> IconPalette {
        let palettes: [IconPalette] = [
            IconPalette(backgroundA: color(0x4F8BFF), backgroundB: color(0x64D2FF), panel: color(0xF7FBFF), foreground: color(0x12315C), accents: [color(0xFFB84D), color(0x38D878), color(0x7A5CFF)]),
            IconPalette(backgroundA: color(0x26C6A7), backgroundB: color(0xD4E157), panel: color(0xFAFFF4), foreground: color(0x173D35), accents: [color(0xFF6B6B), color(0x227CFF), color(0xFFE066)]),
            IconPalette(backgroundA: color(0xFF7A45), backgroundB: color(0xFFD166), panel: color(0xFFF8ED), foreground: color(0x4B2411), accents: [color(0x2EC4B6), color(0x5E60CE), color(0xFFFFFF)]),
            IconPalette(backgroundA: color(0x7C4DFF), backgroundB: color(0xFF6FD8), panel: color(0xFCF7FF), foreground: color(0x2B164F), accents: [color(0x4DFFB8), color(0xFFD166), color(0x5CC8FF)]),
            IconPalette(backgroundA: color(0xEAF2FF), backgroundB: color(0x95B8FF), panel: color(0xFFFFFF), foreground: color(0x174680), accents: [color(0xFF6B9E), color(0x2BD4A7), color(0x3366FF)])
        ]
        return palettes[generator.nextInt(0..<palettes.count)]
    }

    private static func iconMonogram(from appName: String) -> String {
        let letters = appName
            .split { !$0.isLetter && !$0.isNumber }
            .prefix(2)
            .compactMap(\.first)
            .map { String($0).uppercased() }
            .joined()
        return letters.isEmpty ? "A" : letters
    }

    private static func color(_ rgb: UInt32) -> NSColor {
        NSColor(calibratedRed: CGFloat((rgb >> 16) & 0xff) / 255,
                green: CGFloat((rgb >> 8) & 0xff) / 255,
                blue: CGFloat(rgb & 0xff) / 255,
                alpha: 1)
    }

    private static func fnv1a64(_ string: String) -> UInt64 {
        var hash: UInt64 = 0xcbf29ce484222325
        for byte in string.utf8 {
            hash ^= UInt64(byte)
            hash &*= 0x100000001b3
        }
        return hash
    }

    private struct SeededGenerator {
        var state: UInt64

        init(seed: UInt64) {
            state = seed == 0 ? 0x9e3779b97f4a7c15 : seed
        }

        mutating func next() -> UInt64 {
            state &+= 0x9e3779b97f4a7c15
            var value = state
            value = (value ^ (value >> 30)) &* 0xbf58476d1ce4e5b9
            value = (value ^ (value >> 27)) &* 0x94d049bb133111eb
            return value ^ (value >> 31)
        }

        mutating func nextInt(_ range: Range<Int>) -> Int {
            range.lowerBound + Int(next() % UInt64(range.upperBound - range.lowerBound))
        }
    }

    private struct TemplateArchiveEntry {
        let path: String
        let permissions: Int
        let contents: Data
    }

    private struct TemplateArchiveReader {
        private let data: Data
        private var offset = 0

        init(data: Data) {
            self.data = data
        }

        mutating func readMagic() -> Bool {
            guard data.count >= 8 else { return false }
            defer { offset = 8 }
            return Array(data[0..<8]) == [0x4f, 0x53, 0x4e, 0x54, 0x50, 0x4c, 0x31, 0x00]
        }

        mutating func readEntry() throws -> TemplateArchiveEntry? {
            guard let pathLength = readUInt16() else { throw NativeAppProjectGeneratorError.invalidResponse }
            if pathLength == 0 { return nil }
            guard let permissions = readUInt32(),
                  let contentLength = readUInt64(),
                  contentLength <= UInt64(Int.max),
                  let pathData = readData(Int(pathLength)),
                  let contents = readData(Int(contentLength)),
                  let path = String(data: pathData, encoding: .utf8) else {
                throw NativeAppProjectGeneratorError.invalidResponse
            }
            return TemplateArchiveEntry(path: path,
                                        permissions: Int(permissions),
                                        contents: contents)
        }

        private mutating func readUInt16() -> UInt16? {
            guard offset + 2 <= data.count else { return nil }
            defer { offset += 2 }
            return UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
        }

        private mutating func readUInt32() -> UInt32? {
            guard offset + 4 <= data.count else { return nil }
            var value: UInt32 = 0
            for shift in 0..<4 { value |= UInt32(data[offset + shift]) << UInt32(shift * 8) }
            offset += 4
            return value
        }

        private mutating func readUInt64() -> UInt64? {
            guard offset + 8 <= data.count else { return nil }
            var value: UInt64 = 0
            for shift in 0..<8 { value |= UInt64(data[offset + shift]) << UInt64(shift * 8) }
            offset += 8
            return value
        }

        private mutating func readData(_ length: Int) -> Data? {
            guard length >= 0, offset + length <= data.count else { return nil }
            defer { offset += length }
            return Data(data[offset..<(offset + length)])
        }
    }
}
