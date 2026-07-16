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
    let sshCommandArguments: [String]

    var backendExecutableName: String {
        "\(xcodeScheme)Backend"
    }
}

struct GeneratedNativeAppProject {
    let canonicalProjectURL: URL
    let projectURL: URL
    let folderName: String
    let remoteProjectPath: String
    let hasPlatformWorkspace: Bool
}

enum NativeAppProjectGeneratorError: LocalizedError {
    case missingTemplate
    case invalidTemplate
    case missingStagingDirectory
    case invalidName

    var errorDescription: String? {
        switch self {
        case .missingTemplate:
            return "App template is unavailable from the Outer Shell backend."
        case .invalidTemplate:
            return "App template is invalid."
        case .missingStagingDirectory:
            return "Outerframe staging directory is unavailable."
        case .invalidName:
            return "Project name is invalid."
        }
    }
}

enum NativeAppProjectGenerator {
    private struct RemoteProjectLocation {
        let displayPath: String
        let scriptPath: String
        let isHomeRelative: Bool
    }

    private static func remoteProjectLocation(rootPath: String,
                                              folderName: String) -> RemoteProjectLocation {
        var root = rootPath.trimmingCharacters(in: .whitespacesAndNewlines)
        if root.isEmpty { root = "~/outerframe-apps" }
        while root.count > 1 && root.hasSuffix("/") {
            root.removeLast()
        }
        if root == "~" {
            return RemoteProjectLocation(displayPath: "~/\(folderName)",
                                         scriptPath: folderName,
                                         isHomeRelative: true)
        }
        if root.hasPrefix("~/") {
            return RemoteProjectLocation(displayPath: "\(root)/\(folderName)",
                                         scriptPath: "\(root.dropFirst(2))/\(folderName)",
                                         isHomeRelative: true)
        }
        let separator = root == "/" ? "" : "/"
        let path = "\(root)\(separator)\(folderName)"
        return RemoteProjectLocation(displayPath: path,
                                     scriptPath: path,
                                     isHomeRelative: false)
    }

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

        try materializePlatformTemplates(projectURL: projectURL,
                                         configuration: configuration)
        try materializeBackendTemplate(projectURL: projectURL,
                                       backendLanguage: configuration.backendLanguage)
        try materializeContainerTemplate(projectURL: projectURL,
                                         configuration: configuration)
        try renameTemplateFiles(projectURL: projectURL, scheme: configuration.xcodeScheme)
        try patchTemplateFiles(projectURL: projectURL, configuration: configuration)
        try writeGeneratedIcon(projectURL: projectURL, configuration: configuration)
        let workspaceURL = try materializeMacOSWorkspace(projectURL: projectURL,
                                                         generationRoot: generationRoot,
                                                         folderName: folderName,
                                                         configuration: configuration)
        try makeProjectWritable(generationRoot)

        let remoteProject = remoteProjectLocation(rootPath: configuration.projectRootPath,
                                                  folderName: folderName)
        return GeneratedNativeAppProject(canonicalProjectURL: projectURL,
                                         projectURL: workspaceURL ?? projectURL,
                                         folderName: workspaceURL?.lastPathComponent ?? folderName,
                                         remoteProjectPath: remoteProject.displayPath,
                                         hasPlatformWorkspace: workspaceURL != nil)
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
        let backendURL = projectURL.appendingPathComponent("server", isDirectory: true)

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

    private static func materializePlatformTemplates(projectURL: URL,
                                                     configuration: NativeAppProjectConfiguration) throws {
        let fileManager = FileManager.default
        let swiftURL = projectURL.appendingPathComponent("frontend-swift", isDirectory: true)
        let objcURL = projectURL.appendingPathComponent("frontend-objc", isDirectory: true)
        let macOSURL = projectURL.appendingPathComponent("macos", isDirectory: true)
        let htmlURL = projectURL.appendingPathComponent("html", isDirectory: true)

        if configuration.platformTargets.contains(.macos) {
            switch configuration.frontendLanguage {
            case .swift:
                if fileManager.fileExists(atPath: swiftURL.path) {
                    try fileManager.moveItem(at: swiftURL, to: macOSURL)
                }
                if fileManager.fileExists(atPath: objcURL.path) {
                    try fileManager.removeItem(at: objcURL)
                }
            case .objc:
                if fileManager.fileExists(atPath: objcURL.path) {
                    try fileManager.moveItem(at: objcURL, to: macOSURL)
                }
                if fileManager.fileExists(atPath: swiftURL.path) {
                    try fileManager.removeItem(at: swiftURL)
                }
            }
        } else {
            if fileManager.fileExists(atPath: swiftURL.path) {
                try fileManager.removeItem(at: swiftURL)
            }
            if fileManager.fileExists(atPath: objcURL.path) {
                try fileManager.removeItem(at: objcURL)
            }
        }

        if !configuration.platformTargets.contains(.html),
           fileManager.fileExists(atPath: htmlURL.path) {
            try fileManager.removeItem(at: htmlURL)
        }
    }

    private static func materializeContainerTemplate(projectURL: URL,
                                                     configuration: NativeAppProjectConfiguration) throws {
        let fileManager = FileManager.default
        let goURL = projectURL.appendingPathComponent("Dockerfile-go")
        let cURL = projectURL.appendingPathComponent("Dockerfile-c")
        let dockerfileURL = projectURL.appendingPathComponent("Dockerfile")

        guard configuration.isolationMode == .container else {
            if fileManager.fileExists(atPath: goURL.path) {
                try fileManager.removeItem(at: goURL)
            }
            if fileManager.fileExists(atPath: cURL.path) {
                try fileManager.removeItem(at: cURL)
            }
            let entrypointURL = projectURL.appendingPathComponent("deploy/container-entrypoint.sh")
            if fileManager.fileExists(atPath: entrypointURL.path) {
                try fileManager.removeItem(at: entrypointURL)
            }
            let serviceURL = projectURL.appendingPathComponent("deploy/app-container.service.in")
            if fileManager.fileExists(atPath: serviceURL.path) {
                try fileManager.removeItem(at: serviceURL)
            }
            let runnerURL = projectURL.appendingPathComponent("deploy/run-container.sh")
            if fileManager.fileExists(atPath: runnerURL.path) {
                try fileManager.removeItem(at: runnerURL)
            }
            let webProvidersURL = projectURL.appendingPathComponent("deploy/web-providers", isDirectory: true)
            if fileManager.fileExists(atPath: webProvidersURL.path) {
                try fileManager.removeItem(at: webProvidersURL)
            }
            return
        }

        let selectedURL = configuration.backendLanguage == .go ? goURL : cURL
        let unselectedURL = configuration.backendLanguage == .go ? cURL : goURL
        if fileManager.fileExists(atPath: selectedURL.path) {
            try fileManager.moveItem(at: selectedURL, to: dockerfileURL)
        }
        if fileManager.fileExists(atPath: unselectedURL.path) {
            try fileManager.removeItem(at: unselectedURL)
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
        let frontendURL = projectURL.appendingPathComponent("macos", isDirectory: true)

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
            ("com.example.HelloFullstackSocket", configuration.socketFilename),
            ("com.example.HelloFullstack", configuration.appID),
            ("HelloFullstackContent", "\(configuration.xcodeScheme)Content"),
            ("HelloFullstackHandler", "\(configuration.xcodeScheme)Handler"),
            ("HelloResponse", "\(configuration.xcodeScheme)HelloResponse"),
            ("HelloFullstackBackend", configuration.backendExecutableName),
            ("HelloFullstack", configuration.xcodeScheme),
            ("Hello World", configuration.appName),
            ("Hello world", configuration.appName),
            ("hellofullstack/server", "\(modulePathComponent(configuration.projectFolderName))/server")
        ]

        let textExtensions: Set<String> = ["", "swift", "go", "mod", "c", "h", "m", "mk", "md", "env", "in", "sh", "py", "plist", "pbxproj", "xcscheme", "xcworkspacedata", "gitignore", "html", "css", "js", "Dockerfile", "Makefile"]
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
            let targets = NativeAppProjectConfiguration.PlatformTarget.allCases
                .filter { configuration.platformTargets.contains($0) }
                .map(\.rawValue)
                .joined(separator: " ")
            contents = contents.replacingOccurrences(of: "APP_TARGETS=\"html macos\"",
                                                     with: "APP_TARGETS=\"\(targets)\"")
            contents = contents.replacingOccurrences(of: "ISOLATION_MODE=\"container\"",
                                                     with: "ISOLATION_MODE=\"\(configuration.isolationMode.rawValue)\"")
            contents = contents.replacingOccurrences(of: "BACKEND_LANGUAGE=\"go\"",
                                                     with: "BACKEND_LANGUAGE=\"\(configuration.backendLanguage.rawValue)\"")
            contents = contents.replacingOccurrences(of: "MACOS_LANGUAGE=\"swift\"",
                                                     with: "MACOS_LANGUAGE=\"\(configuration.frontendLanguage.rawValue)\"")
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

    private static func writeConnectionEnvironment(workspaceURL: URL, sshCommandArguments: [String]) throws {
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

    private static func materializeMacOSWorkspace(projectURL: URL,
                                                  generationRoot: URL,
                                                  folderName: String,
                                                  configuration: NativeAppProjectConfiguration) throws -> URL? {
        guard configuration.platformTargets.contains(.macos) else { return nil }

        let workspaceURL = generationRoot.appendingPathComponent("\(folderName)-macOS", isDirectory: true)
        try createWritableDirectory(at: workspaceURL, withIntermediateDirectories: false)
        let fileManager = FileManager.default
        for name in ["macos", "app.env", "app-icon.png"] {
            let source = projectURL.appendingPathComponent(name)
            let destination = workspaceURL.appendingPathComponent(name)
            if fileManager.fileExists(atPath: source.path) {
                try fileManager.copyItem(at: source, to: destination)
            }
        }

        try writeConnectionEnvironment(workspaceURL: workspaceURL,
                                       sshCommandArguments: configuration.sshCommandArguments)
        let remoteProject = remoteProjectLocation(rootPath: configuration.projectRootPath,
                                                  folderName: folderName)
        try writeFile(Data(macOSPlatformScript(configuration: configuration,
                                              remoteProjectPath: remoteProject.scriptPath,
                                              remoteProjectIsHomeRelative: remoteProject.isHomeRelative).utf8),
                      to: workspaceURL.appendingPathComponent("platform"),
                      permissions: 0o755)
        try writeFile(Data(macOSWorkspaceREADME(configuration: configuration,
                                                remoteProjectPath: remoteProject.displayPath).utf8),
                      to: workspaceURL.appendingPathComponent("README.md"),
                      permissions: 0o644)
        return workspaceURL
    }

    private static func macOSPlatformScript(configuration: NativeAppProjectConfiguration,
                                            remoteProjectPath: String,
                                            remoteProjectIsHomeRelative: Bool) -> String {
        let remoteProjectCommandPath = remoteProjectIsHomeRelative
            ? "\\$HOME/${REMOTE_PROJECT}"
            : "${REMOTE_PROJECT}"
        return """
        #!/bin/bash
        # macOS platform builder for \(configuration.appName). The canonical project
        # stays on the server; this workspace only compiles and publishes macOS.

        set -euo pipefail
        ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        source "${ROOT}/app.env"
        source "${ROOT}/connection.env"

        REMOTE_PROJECT="\(remoteProjectPath)"
        ARTIFACTS_DIR="${ROOT}/artifacts"
        DERIVED_DATA_DIR="${ROOT}/build/DerivedData"
        SSH_BASE=()

        load_connection() {
            if [[ "${OUTER_SERVER_KIND:-ssh}" == local ]]; then
                echo "error: publishing from a local-server workspace is not implemented" >&2
                exit 1
            fi
            if [[ "$(declare -p OUTER_SERVER_SSH 2>/dev/null)" != declare\\ -a* || "${#OUTER_SERVER_SSH[@]}" -eq 0 ]]; then
                echo "error: connection.env does not contain an SSH command" >&2
                exit 1
            fi
            SSH_BASE=("${OUTER_SERVER_SSH[@]}")
        }

        run_ssh() {
            load_connection
            "${SSH_BASE[@]}" "$@"
        }

        require_tool() {
            command -v "$1" >/dev/null 2>&1 || { echo "error: required tool '$1' was not found" >&2; exit 1; }
        }

        bundle_executable_name() {
            local bundle_path="$1" executable_name=""
            if [[ -f "${bundle_path}/Contents/Info.plist" ]]; then
                executable_name="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "${bundle_path}/Contents/Info.plist" 2>/dev/null || true)"
            fi
            [[ -n "${executable_name}" ]] || executable_name="$(basename "${bundle_path}" .bundle)"
            printf '%s\\n' "${executable_name}"
        }

        archive_bundle() {
            local source_bundle="$1" platform="$2" arch="$3"
            local temp_root temp_bundle executable_name executable_path executable_archs
            temp_root="$(mktemp -d)"
            temp_bundle="${temp_root}/$(basename "${source_bundle}")"
            cp -R "${source_bundle}" "${temp_bundle}"
            executable_name="$(bundle_executable_name "${temp_bundle}")"
            executable_path="${temp_bundle}/Contents/MacOS/${executable_name}"
            executable_archs="$(lipo -archs "${executable_path}")"
            [[ " ${executable_archs} " == *" ${arch} "* ]] || { echo "error: missing ${arch} slice" >&2; exit 1; }
            if [[ "${executable_archs}" != "${arch}" ]]; then
                lipo "${executable_path}" -thin "${arch}" -output "${temp_root}/thin"
                mv "${temp_root}/thin" "${executable_path}"
                chmod +x "${executable_path}"
            fi
            aa archive -d "${temp_root}" -subdir "$(basename "${temp_bundle}")" -o "${ARTIFACTS_DIR}/frontends/${platform}" -a lzfse
            rm -rf "${temp_root}"
        }

        cmd_sync() {
            echo "==> Syncing macOS source from the canonical server project"
            rm -rf "${ROOT}/macos.incoming"
            mkdir -p "${ROOT}/macos.incoming"
            run_ssh "set -e; project=\\\"\(remoteProjectCommandPath)\\\"; test -d \\${project}/macos; tar czf - -C \\${project} macos app.env app-icon.png" \
                | tar xzf - -C "${ROOT}/macos.incoming"
            rm -rf "${ROOT}/macos"
            mv "${ROOT}/macos.incoming/macos" "${ROOT}/macos"
            mv "${ROOT}/macos.incoming/app.env" "${ROOT}/app.env"
            [[ ! -f "${ROOT}/macos.incoming/app-icon.png" ]] || mv "${ROOT}/macos.incoming/app-icon.png" "${ROOT}/app-icon.png"
            rmdir "${ROOT}/macos.incoming"
            echo "Synced. Local changes under macos/ were replaced by the server copy."
        }

        cmd_build() {
            require_tool /usr/bin/xcodebuild
            require_tool aa
            require_tool lipo
            require_tool /usr/bin/python3
            rm -rf "${ARTIFACTS_DIR}" "${DERIVED_DATA_DIR}"
            mkdir -p "${ARTIFACTS_DIR}/frontends"
            echo "==> Building ${XCODE_SCHEME}.bundle"
            /usr/bin/xcodebuild -project "${ROOT}/macos/${XCODE_SCHEME}.xcodeproj" -scheme "${XCODE_SCHEME}" \
                -configuration Release -derivedDataPath "${DERIVED_DATA_DIR}" ARCHS="arm64 x86_64" \
                ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
            local bundle="${DERIVED_DATA_DIR}/Build/Products/Release/${XCODE_SCHEME}.bundle"
            [[ -d "${bundle}" ]] || { echo "error: expected bundle at ${bundle}" >&2; exit 1; }
            archive_bundle "${bundle}" macos-arm arm64
            archive_bundle "${bundle}" macos-x86 x86_64
            /usr/bin/python3 "${ROOT}/macos/Scripts/generate_outer.py" --bundle-url "${FRONTEND_PATH}" --output "${ARTIFACTS_DIR}/app.outer"
            echo "==> macOS artifacts ready"
        }

        cmd_publish() {
            cmd_build
            echo "==> Publishing only the compiled macOS implementation"
            COPYFILE_DISABLE=1 tar czf - -C "${ARTIFACTS_DIR}" app.outer frontends | run_ssh "
                set -e
                project=\\\"\(remoteProjectCommandPath)\\\"
                incoming=\\\"\\$(mktemp -d)\\\"
                tar xzf - -C \\${incoming}
                cd \\${project}
                ./app accept-platform macos \\${incoming}
                rm -rf \\${incoming}
            "
            echo "Published. Reload \(configuration.appName) in Outer Loop."
        }

        case "${1:-help}" in
            sync)    cmd_sync ;;
            build)   cmd_build ;;
            publish) cmd_publish ;;
            help|--help|-h)
                echo "usage: ./platform sync | build | publish"
                echo "  sync     replace macos/ with the canonical server source"
                echo "  build    compile macOS artifacts locally"
                echo "  publish  build and send only compiled artifacts to the server"
                ;;
            *) echo "error: unknown command '$1'" >&2; exit 1 ;;
        esac
        """
    }

    private static func macOSWorkspaceREADME(configuration: NativeAppProjectConfiguration,
                                             remoteProjectPath: String) -> String {
        """
        # \(configuration.appName) — macOS builder

        This is a platform build workspace, not the app's main project. The
        canonical project lives on the server at `\(remoteProjectPath)`.

        - `./platform sync` replaces `macos/` with the source from the server.
        - `./platform build` compiles universal macOS artifacts locally.
        - `./platform publish` builds and sends only those compiled artifacts
          back to the canonical project, which redeploys the app.

        Edit the canonical source over SSH (directly or with a coding agent),
        sync it here, then publish it from this Mac.
        """
    }

    private static func writeGeneratedIcon(projectURL: URL,
                                           configuration: NativeAppProjectConfiguration) throws {
        let data = try generatedIconPNGData(appName: configuration.appName, appID: configuration.appID)
        let rootIconURL = projectURL.appendingPathComponent("app-icon.png")
        try writeFile(data, to: rootIconURL, permissions: 0o644)

        let frontendIconURL = projectURL
            .appendingPathComponent("macos", isDirectory: true)
            .appendingPathComponent("app-icon.png")
        if FileManager.default.fileExists(atPath: frontendIconURL.deletingLastPathComponent().path) {
            try writeFile(data, to: frontendIconURL, permissions: 0o644)
        }
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
