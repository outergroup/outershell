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
    enum FrontendLanguage: String {
        case swift
        case objc
    }

    enum BackendLanguage: String {
        case go
        case c
    }

    let appName: String
    let appID: String
    let xcodeScheme: String
    let projectFolderName: String
    let socketFilename: String
    let frontendLanguage: FrontendLanguage
    let backendLanguage: BackendLanguage
    let sshCommandArguments: [String]

    var backendExecutableName: String {
        "\(xcodeScheme)Backend"
    }
}

struct GeneratedNativeAppProject {
    let projectURL: URL
    let folderName: String
}

enum NativeAppProjectGeneratorError: LocalizedError {
    case missingTemplate
    case invalidTemplate
    case missingStagingDirectory
    case invalidName

    var errorDescription: String? {
        switch self {
        case .missingTemplate:
            return "Native app template is unavailable from the Outer Shell backend."
        case .invalidTemplate:
            return "Native app template is invalid."
        case .missingStagingDirectory:
            return "Outerframe staging directory is unavailable."
        case .invalidName:
            return "Project name is invalid."
        }
    }
}

enum NativeAppProjectGenerator {
    static func generate(configuration: NativeAppProjectConfiguration,
                         stagingDirectory: URL,
                         templateArchiveData: Data) throws -> GeneratedNativeAppProject {
        let folderName = safePathComponent(configuration.projectFolderName)
        guard !folderName.isEmpty else {
            throw NativeAppProjectGeneratorError.invalidName
        }

        let generationRoot = stagingDirectory
            .appendingPathComponent("native-app-projects", isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let projectURL = generationRoot.appendingPathComponent(folderName, isDirectory: true)

        try createWritableDirectory(at: generationRoot)
        try materializeTemplateArchive(templateArchiveData, to: projectURL)

        try materializeFrontendTemplate(projectURL: projectURL,
                                        frontendLanguage: configuration.frontendLanguage)
        try materializeBackendTemplate(projectURL: projectURL,
                                       backendLanguage: configuration.backendLanguage)
        try renameTemplateFiles(projectURL: projectURL, scheme: configuration.xcodeScheme)
        try patchTemplateFiles(projectURL: projectURL, configuration: configuration)
        try writeGeneratedIcon(projectURL: projectURL, configuration: configuration)
        try writeTargetEnvironment(projectURL: projectURL, sshCommandArguments: configuration.sshCommandArguments)
        try makeProjectWritable(generationRoot)

        return GeneratedNativeAppProject(projectURL: projectURL, folderName: folderName)
    }

    private static func materializeTemplateArchive(_ data: Data, to projectURL: URL) throws {
        try createWritableDirectory(at: projectURL, withIntermediateDirectories: false)
        try makeItemWritable(projectURL)

        var reader = TemplateArchiveReader(data: data)
        guard reader.readMagic() else {
            throw NativeAppProjectGeneratorError.invalidTemplate
        }

        while let entry = try reader.readEntry() {
            guard isSafeTemplateRelativePath(entry.path) else {
                throw NativeAppProjectGeneratorError.invalidTemplate
            }
            let destinationURL = projectURL.appendingPathComponent(entry.path)
            try createWritableDirectory(at: destinationURL.deletingLastPathComponent())
            try writeFile(entry.contents,
                          to: destinationURL,
                          permissions: Int(entry.permissions | 0o644))
        }
    }

    private static func createWritableDirectory(at url: URL, withIntermediateDirectories: Bool = true) throws {
        try FileManager.default.createDirectory(at: url,
                                                withIntermediateDirectories: withIntermediateDirectories,
                                                attributes: [.posixPermissions: 0o755])
        try makeItemWritable(url)
    }

    private static func writeFile(_ data: Data, to url: URL, permissions: Int) throws {
        let fileManager = FileManager.default
        if fileManager.fileExists(atPath: url.path) {
            try fileManager.removeItem(at: url)
        }
        let fd = open(url.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode_t(permissions & 0o777))
        guard fd >= 0 else {
            throw NativeAppProjectFileError(action: "open", path: url.path, errnoValue: errno)
        }
        do {
            try data.withUnsafeBytes { rawBuffer in
                guard let baseAddress = rawBuffer.baseAddress else { return }
                var remaining = rawBuffer.count
                var offset = 0
                while remaining > 0 {
                    let written = Darwin.write(fd, baseAddress.advanced(by: offset), remaining)
                    if written < 0 {
                        if errno == EINTR {
                            continue
                        }
                        throw NativeAppProjectFileError(action: "write", path: url.path, errnoValue: errno)
                    }
                    if written == 0 {
                        throw NativeAppProjectFileError(action: "write", path: url.path, errnoValue: EIO)
                    }
                    remaining -= written
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

    private static func materializeBackendTemplate(projectURL: URL,
                                                   backendLanguage: NativeAppProjectConfiguration.BackendLanguage) throws {
        let fileManager = FileManager.default
        let goURL = projectURL.appendingPathComponent("backend-go", isDirectory: true)
        let cURL = projectURL.appendingPathComponent("backend-c", isDirectory: true)
        let backendURL = projectURL.appendingPathComponent("backend", isDirectory: true)

        switch backendLanguage {
        case .go:
            if fileManager.fileExists(atPath: goURL.path) {
                try fileManager.moveItem(at: goURL, to: backendURL)
            }
            if fileManager.fileExists(atPath: cURL.path) {
                try fileManager.removeItem(at: cURL)
            }
        case .c:
            if fileManager.fileExists(atPath: cURL.path) {
                try fileManager.moveItem(at: cURL, to: backendURL)
            }
            if fileManager.fileExists(atPath: goURL.path) {
                try fileManager.removeItem(at: goURL)
            }
        }
    }

    private static func materializeFrontendTemplate(projectURL: URL,
                                                    frontendLanguage: NativeAppProjectConfiguration.FrontendLanguage) throws {
        let fileManager = FileManager.default
        let swiftURL = projectURL.appendingPathComponent("frontend-swift", isDirectory: true)
        let objcURL = projectURL.appendingPathComponent("frontend-objc", isDirectory: true)
        let frontendURL = projectURL.appendingPathComponent("frontend", isDirectory: true)

        switch frontendLanguage {
        case .swift:
            if fileManager.fileExists(atPath: swiftURL.path) {
                try fileManager.moveItem(at: swiftURL, to: frontendURL)
            }
            if fileManager.fileExists(atPath: objcURL.path) {
                try fileManager.removeItem(at: objcURL)
            }
        case .objc:
            if fileManager.fileExists(atPath: objcURL.path) {
                try fileManager.moveItem(at: objcURL, to: frontendURL)
            }
            if fileManager.fileExists(atPath: swiftURL.path) {
                try fileManager.removeItem(at: swiftURL)
            }
        }
    }

    static func makeProjectWritable(_ projectURL: URL) throws {
        let fileManager = FileManager.default
        try makeItemWritable(projectURL)

        guard let enumerator = fileManager.enumerator(at: projectURL,
                                                      includingPropertiesForKeys: [.isDirectoryKey],
                                                      options: []) else {
            return
        }

        for case let itemURL as URL in enumerator {
            try makeItemWritable(itemURL)
        }
    }

    private static func makeItemWritable(_ url: URL) throws {
        let fileManager = FileManager.default
        let attributes = try fileManager.attributesOfItem(atPath: url.path)
        let permissions = (attributes[.posixPermissions] as? NSNumber)?.intValue ?? 0o644
        let isDirectory = (attributes[.type] as? FileAttributeType) == .typeDirectory
        let writablePermissions = permissions | (isDirectory ? 0o755 : 0o644)
        try fileManager.setAttributes([.posixPermissions: writablePermissions], ofItemAtPath: url.path)
    }

    private static func renameTemplateFiles(projectURL: URL, scheme: String) throws {
        let fileManager = FileManager.default
        let frontendURL = projectURL.appendingPathComponent("frontend", isDirectory: true)

        let oldProject = frontendURL.appendingPathComponent("HelloFullstack.xcodeproj", isDirectory: true)
        let newProject = frontendURL.appendingPathComponent("\(scheme).xcodeproj", isDirectory: true)
        if fileManager.fileExists(atPath: oldProject.path) {
            try fileManager.moveItem(at: oldProject, to: newProject)
        }

        let oldSwift = frontendURL.appendingPathComponent("Frontend/HelloFullstackContent.swift")
        let newSwift = frontendURL.appendingPathComponent("Frontend/\(scheme)Content.swift")
        if fileManager.fileExists(atPath: oldSwift.path) {
            try fileManager.moveItem(at: oldSwift, to: newSwift)
        }

        let oldObjCHeader = frontendURL.appendingPathComponent("Frontend/HelloFullstackContent.h")
        let newObjCHeader = frontendURL.appendingPathComponent("Frontend/\(scheme)Content.h")
        if fileManager.fileExists(atPath: oldObjCHeader.path) {
            try fileManager.moveItem(at: oldObjCHeader, to: newObjCHeader)
        }

        let oldObjCImplementation = frontendURL.appendingPathComponent("Frontend/HelloFullstackContent.m")
        let newObjCImplementation = frontendURL.appendingPathComponent("Frontend/\(scheme)Content.m")
        if fileManager.fileExists(atPath: oldObjCImplementation.path) {
            try fileManager.moveItem(at: oldObjCImplementation, to: newObjCImplementation)
        }

        let oldScheme = newProject.appendingPathComponent("xcshareddata/xcschemes/HelloFullstack.xcscheme")
        let newScheme = newProject.appendingPathComponent("xcshareddata/xcschemes/\(scheme).xcscheme")
        if fileManager.fileExists(atPath: oldScheme.path) {
            try fileManager.moveItem(at: oldScheme, to: newScheme)
        }
    }

    private static func patchTemplateFiles(projectURL: URL,
                                           configuration: NativeAppProjectConfiguration) throws {
        let replacements: [(String, String)] = [
            ("com.example.HelloFullstack.sock", configuration.socketFilename),
            ("com.example.HelloFullstack", configuration.appID),
            ("HelloFullstackContent", "\(configuration.xcodeScheme)Content"),
            ("HelloFullstackHandler", "\(configuration.xcodeScheme)Handler"),
            ("HelloResponse", "\(configuration.xcodeScheme)HelloResponse"),
            ("HelloFullstackBackend", configuration.backendExecutableName),
            ("HelloFullstack", configuration.xcodeScheme),
            ("Hello World", configuration.appName),
            ("Hello world", configuration.appName),
            ("hellofullstack/backend", "\(modulePathComponent(configuration.projectFolderName))/backend")
        ]

        let textExtensions: Set<String> = ["", "swift", "go", "mod", "c", "h", "m", "mk", "md", "env", "in", "sh", "py", "plist", "pbxproj", "xcscheme", "xcworkspacedata", "gitignore", "Dockerfile", "Makefile"]
        let resourceKeys: Set<URLResourceKey> = [.isDirectoryKey, .isRegularFileKey]
        guard let enumerator = FileManager.default.enumerator(at: projectURL,
                                                              includingPropertiesForKeys: Array(resourceKeys),
                                                              options: [.skipsHiddenFiles]) else {
            return
        }

        for case let fileURL as URL in enumerator {
            let values = try fileURL.resourceValues(forKeys: resourceKeys)
            guard values.isRegularFile == true else { continue }
            let last = fileURL.lastPathComponent
            let ext = fileURL.pathExtension
            guard textExtensions.contains(ext) || textExtensions.contains(last) else { continue }

            var contents = try String(contentsOf: fileURL, encoding: .utf8)
            for (old, new) in replacements {
                contents = contents.replacingOccurrences(of: old, with: new)
            }
            contents = contents.replacingOccurrences(of: "BACKEND_LANGUAGE=\"go\"",
                                                     with: "BACKEND_LANGUAGE=\"\(configuration.backendLanguage.rawValue)\"")
            contents = contents.replacingOccurrences(of: "FRONTEND_LANGUAGE=\"swift\"",
                                                     with: "FRONTEND_LANGUAGE=\"\(configuration.frontendLanguage.rawValue)\"")
            contents = contents.replacingOccurrences(of: "BACKEND_EXECUTABLE_NAME=\"HelloFullstackBackend\"",
                                                     with: "BACKEND_EXECUTABLE_NAME=\"\(configuration.backendExecutableName)\"")
            try writeFile(Data(contents.utf8),
                          to: fileURL,
                          permissions: try existingPermissions(at: fileURL) | 0o644)
        }
    }

    private static func existingPermissions(at url: URL) throws -> Int {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        return (attributes[.posixPermissions] as? NSNumber)?.intValue ?? 0o644
    }

    private static func writeTargetEnvironment(projectURL: URL, sshCommandArguments: [String]) throws {
        let body: String
        if sshCommandArguments.isEmpty {
            body = """
            # Generated by Outer Shell for a local Outer Loop session.
            OUTER_TARGET_KIND=local
            OUTER_TARGET_SSH=()
            """
        } else {
            body = """
            # Generated by Outer Shell from Outer Loop's SSH connection arguments.
            OUTER_TARGET_KIND=ssh
            OUTER_TARGET_SSH=(
            \(sshCommandArguments.map { "    \(shellSingleQuotedValue($0))" }.joined(separator: "\n"))
            )
            """
        }
        try writeFile(Data(body.utf8),
                      to: projectURL.appendingPathComponent("target.env"),
                      permissions: 0o644)
    }

    private static func writeGeneratedIcon(projectURL: URL,
                                           configuration: NativeAppProjectConfiguration) throws {
        let data = try generatedIconPNGData(appName: configuration.appName, appID: configuration.appID)
        let rootIconURL = projectURL.appendingPathComponent("app-icon.png")
        try writeFile(data, to: rootIconURL, permissions: 0o644)

        let frontendIconURL = projectURL
            .appendingPathComponent("frontend", isDirectory: true)
            .appendingPathComponent("app-icon.png")
        try writeFile(data, to: frontendIconURL, permissions: 0o644)
    }

    private static func generatedIconPNGData(appName: String, appID: String) throws -> Data {
        let points = 1024
        let scale: CGFloat = 1
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
        context.cgContext.scaleBy(x: scale, y: scale)
        defer {
            NSGraphicsContext.restoreGraphicsState()
        }

        let bounds = NSRect(origin: .zero, size: size)
        NSColor.clear.setFill()
        NSBezierPath(rect: bounds).fill()

        let backgroundPath = NSBezierPath(roundedRect: bounds.insetBy(dx: 72, dy: 72),
                                          xRadius: 210,
                                          yRadius: 210)
        backgroundPath.addClip()

        let gradient = NSGradient(colors: [palette.backgroundA, palette.backgroundB])!
        gradient.draw(in: bounds, angle: CGFloat(generator.nextInt(0..<360)))

        for _ in 0..<18 {
            let diameter = CGFloat(generator.nextInt(90..<340))
            let x = CGFloat(generator.nextInt(-90..<980))
            let y = CGFloat(generator.nextInt(-90..<980))
            let shape = NSBezierPath(ovalIn: NSRect(x: x, y: y, width: diameter, height: diameter))
            palette.accent(generator.nextInt(0..<palette.accents.count))
                .withAlphaComponent(CGFloat(generator.nextInt(10..<24)) / 100)
                .setFill()
            shape.fill()
        }

        for _ in 0..<7 {
            let width = CGFloat(generator.nextInt(130..<360))
            let height = CGFloat(generator.nextInt(34..<92))
            let x = CGFloat(generator.nextInt(-80..<850))
            let y = CGFloat(generator.nextInt(80..<850))
            let rect = NSRect(x: x, y: y, width: width, height: height)
            let path = NSBezierPath(roundedRect: rect, xRadius: height / 2, yRadius: height / 2)
            palette.accent(generator.nextInt(0..<palette.accents.count))
                .withAlphaComponent(CGFloat(generator.nextInt(26..<58)) / 100)
                .setFill()
            path.fill()
        }

        let lineColor = palette.foreground.withAlphaComponent(0.18)
        lineColor.setStroke()
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
        let words = appName
            .split { !$0.isLetter && !$0.isNumber }
            .map(String.init)
        let letters = words
            .prefix(2)
            .compactMap { $0.first }
            .map { String($0).uppercased() }
            .joined()
        if !letters.isEmpty {
            return letters
        }
        return "A"
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
            self.state = seed == 0 ? 0x9e3779b97f4a7c15 : seed
        }

        mutating func next() -> UInt64 {
            state &+= 0x9e3779b97f4a7c15
            var value = state
            value = (value ^ (value >> 30)) &* 0xbf58476d1ce4e5b9
            value = (value ^ (value >> 27)) &* 0x94d049bb133111eb
            return value ^ (value >> 31)
        }

        mutating func nextInt(_ range: Range<Int>) -> Int {
            let width = UInt64(range.upperBound - range.lowerBound)
            return range.lowerBound + Int(next() % width)
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

    private static func modulePathComponent(_ value: String) -> String {
        let lower = safePathComponent(value.lowercased())
        return lower.isEmpty ? "outerframe-app" : lower
    }

    private static func shellSingleQuotedValue(_ value: String) -> String {
        return "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
    }

    private static func isSafeTemplateRelativePath(_ path: String) -> Bool {
        if path.isEmpty || path.hasPrefix("/") || path.contains("\0") {
            return false
        }
        let components = path.split(separator: "/", omittingEmptySubsequences: false)
        return !components.contains { $0 == ".." || $0.isEmpty }
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
            let magic = data[offset..<offset + 8]
            offset += 8
            return Array(magic) == [0x4f, 0x53, 0x4e, 0x54, 0x50, 0x4c, 0x31, 0x00]
        }

        mutating func readEntry() throws -> TemplateArchiveEntry? {
            guard let pathLength = readUInt16() else {
                throw NativeAppProjectGeneratorError.invalidTemplate
            }
            if pathLength == 0 {
                return nil
            }
            guard let permissions = readUInt32(),
                  let contentLength = readUInt64(),
                  contentLength <= UInt64(Int.max),
                  let pathData = readData(Int(pathLength)),
                  let contents = readData(Int(contentLength)),
                  let path = String(data: pathData, encoding: .utf8) else {
                throw NativeAppProjectGeneratorError.invalidTemplate
            }
            return TemplateArchiveEntry(path: path, permissions: Int(permissions), contents: contents)
        }

        private mutating func readUInt16() -> UInt16? {
            guard offset + 2 <= data.count else { return nil }
            let value = UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
            offset += 2
            return value
        }

        private mutating func readUInt32() -> UInt32? {
            guard offset + 4 <= data.count else { return nil }
            var value: UInt32 = 0
            for shift in 0..<4 {
                value |= UInt32(data[offset + shift]) << UInt32(shift * 8)
            }
            offset += 4
            return value
        }

        private mutating func readUInt64() -> UInt64? {
            guard offset + 8 <= data.count else { return nil }
            var value: UInt64 = 0
            for shift in 0..<8 {
                value |= UInt64(data[offset + shift]) << UInt64(shift * 8)
            }
            offset += 8
            return value
        }

        private mutating func readData(_ length: Int) -> Data? {
            guard length >= 0, offset + length <= data.count else { return nil }
            let result = data[offset..<offset + length]
            offset += length
            return Data(result)
        }
    }
}
