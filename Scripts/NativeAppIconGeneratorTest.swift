import Foundation

@main
struct NativeAppIconGeneratorTest {
    static func main() throws {
        let data = try NativeAppProjectGenerator.generatedIconPNGData(appName: "Widget Works",
                                                                      appID: "com.example.WidgetWorks")
        let signature = Data([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
        guard data.count >= 24, data.prefix(8) == signature else {
            throw NativeAppProjectGeneratorError.invalidResponse
        }
        let width = data[16..<20].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        let height = data[20..<24].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        guard width == 1024, height == 1024 else {
            throw NativeAppProjectGeneratorError.invalidResponse
        }
        print("Generated native app icon: \(width)x\(height), \(data.count) bytes")

        let stagingDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("native-app-project-generator-test-\(UUID().uuidString)",
                                    isDirectory: true)
        defer { try? FileManager.default.removeItem(at: stagingDirectory) }
        try FileManager.default.createDirectory(at: stagingDirectory,
                                                withIntermediateDirectories: true)

        var archive = Data([0x4f, 0x53, 0x4e, 0x54, 0x50, 0x4c, 0x31, 0x00])
        appendArchiveEntry(path: ".outershell-remote-project-path",
                           permissions: 0o644,
                           contents: Data("~/outerframe-apps/widget-works".utf8),
                           to: &archive)
        appendArchiveEntry(path: "builder/widget-works-macOS/platform",
                           permissions: 0o755,
                           contents: Data("#!/bin/bash\n".utf8),
                           to: &archive)
        appendInteger(UInt16(0), to: &archive)

        let configuration = NativeAppProjectConfiguration(
            appName: "Widget Works",
            appID: "com.example.WidgetWorks",
            xcodeScheme: "WidgetWorks",
            projectRootPath: "~/outerframe-apps",
            projectFolderName: "widget-works",
            socketFilename: "com.example.WidgetWorksSocket",
            platformTargets: [.html, .macos],
            frontendLanguage: .swift,
            backendLanguage: .go,
            isolationMode: .host
        )
        let project = try NativeAppProjectGenerator.materializeProjectResponse(
            archive,
            configuration: configuration,
            stagingDirectory: stagingDirectory,
            sshCommandArguments: ["ssh", "example.test"]
        )
        guard let projectURL = project.projectURL,
              project.remoteProjectPath == "~/outerframe-apps/widget-works",
              FileManager.default.isExecutableFile(
                atPath: projectURL.appendingPathComponent("platform").path
              ),
              try String(contentsOf: projectURL.appendingPathComponent("connection.env"), encoding: .utf8)
                .contains("'example.test'") else {
            throw NativeAppProjectGeneratorError.invalidResponse
        }
    }

    private static func appendArchiveEntry(path: String,
                                           permissions: UInt32,
                                           contents: Data,
                                           to archive: inout Data) {
        let pathData = Data(path.utf8)
        appendInteger(UInt16(pathData.count), to: &archive)
        appendInteger(permissions, to: &archive)
        appendInteger(UInt64(contents.count), to: &archive)
        archive.append(pathData)
        archive.append(contents)
    }

    private static func appendInteger<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { data.append(contentsOf: $0) }
    }
}
