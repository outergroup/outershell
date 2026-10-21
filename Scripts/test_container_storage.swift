import Foundation

@main
struct StorageTests {
    static func main() throws {
        typealias Image = ContainerImageStorage.Image
        let id = "11111111-1111-1111-1111-111111111111"
        let recipe = "outershell/safe-space-\(id):recipe"
        func image(_ name: String, _ digest: String? = nil) -> Image {
            Image(reference: name, digests: [digest ?? name], snapshotDigests: [])
        }
        let images = [image("outershell/container-base:1"), image("outershell/container-base:12"), image("outershell/container-base:18"),
                      image("outerloop/workspace-501:2"), image("outerloop/workspace-501:9"),
                      image(recipe), image("ubuntu:24.04"), image("outershell/safe-space-not-a-uuid:recipe"),
                      image("outerloop/workspace-502:1"), image("outershell/container-base:custom")]
        func candidates(_ protected: Set<String> = [], _ files: [String] = [], _ input: [Image]? = nil) -> [String] {
            ContainerImageStorage.candidates(images: input ?? images, protected: protected,
                dockerfiles: files, currentBase: "outershell/container-base:12",
                workspacePrefix: "outerloop/workspace-501:").map(\.reference)
        }
        precondition(Set(candidates()) == ["outershell/container-base:1", "outerloop/workspace-501:2", recipe])
        precondition(!candidates([recipe]).contains(recipe))
        precondition(!candidates([], ["FROM outershell/container-base:1\nRUN echo $HOME"]).contains("outershell/container-base:1"))
        precondition(candidates([], ["ARG BASE\nFROM ${BASE}"]).isEmpty)
        precondition(candidates([], ["ARG BASE\nFROM\t${BASE}"]).isEmpty)
        precondition(candidates([], ["FROM \\\n ${BASE}"]).isEmpty)
        precondition(candidates([], ["COPY --chown=root --from=${SOURCE} /bin /bin"]).isEmpty)
        let shared = [image(recipe, "sha256:shared"), image("another:tag", "sha256:shared")]
        precondition(candidates(["another:tag"], [], shared).isEmpty)
        precondition(candidates(["sha256:shared"], [], shared).isEmpty)
        precondition(candidates([], ["FROM example@sha256:shared"], shared).isEmpty)
        let data = Data("""
        [{"configuration":{"image":{"reference":"another:tag","descriptor":{"digest":"sha256:shared"}}}}]
        """.utf8)
        let references = try ContainerImageStorage.containerReferences(from: data)
        precondition(references == ["another:tag", "sha256:shared"])
        for invalid in ["{}", "[{}]", "[{\"configuration\":{\"image\":{\"reference\":\"x\"}}}]"] {
            do { _ = try ContainerImageStorage.containerReferences(from: Data(invalid.utf8)); preconditionFailure("Accepted invalid inventory") }
            catch {}
        }
        var deleted: [String] = []
        do {
            _ = try ContainerImageStorage.deleteSelected(["ubuntu:24.04"], candidates: { images.filter { $0.reference == recipe } }, delete: { deleted.append($0) })
            preconditionFailure("Accepted an unreviewed image")
        } catch {}
        precondition(deleted.isEmpty)
        var reads = 0
        let changed = try ContainerImageStorage.deleteSelected([recipe], candidates: {
            reads += 1
            return reads == 1 ? [image(recipe)] : []
        }, delete: { deleted.append($0) })
        precondition(deleted.isEmpty && changed.failures.count == 1)
        let failed = try ContainerImageStorage.deleteSelected([recipe], candidates: { [image(recipe)] }, delete: { _ in throw CocoaError(.fileWriteNoPermission) })
        precondition(failed.deleted.isEmpty && failed.failures.count == 1)
        let success = try ContainerImageStorage.deleteSelected([recipe, recipe], candidates: { [image(recipe)] }, delete: { deleted.append($0) })
        precondition(deleted == [recipe] && success.deleted == [recipe])
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("storage-test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let indexDigest = String(repeating: "a", count: 64)
        let retainedDigest = String(repeating: "b", count: 64)
        let orphanDigest = String(repeating: "c", count: 64)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("content/blobs/sha256"), withIntermediateDirectories: true)
        try JSONSerialization.data(withJSONObject: ["hidden-system-image": ["digest": "sha256:" + indexDigest]])
            .write(to: root.appendingPathComponent("state.json"))
        try JSONSerialization.data(withJSONObject: ["manifests": [["digest": "sha256:" + retainedDigest]]])
            .write(to: root.appendingPathComponent("content/blobs/sha256/\(indexDigest)"))
        for digest in [retainedDigest, orphanDigest] {
            let directory = root.appendingPathComponent("snapshots/\(digest)")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try Data(repeating: 1, count: 65536).write(to: directory.appendingPathComponent("snapshot"))
        }
        let orphanImage = Image(reference: "test", digests: [], snapshotDigests: ["sha256:" + orphanDigest])
        let expected = try ContainerImageStorage.allocatedSnapshotBytes(for: orphanImage, root: root)
        let actual = try ContainerImageStorage.orphanSnapshotBytes(root: root)
        precondition(actual == expected && expected > 0)
        let sample = Data("[{\"configuration\":{\"name\":\"test:1\",\"descriptor\":{\"digest\":\"sha256:index\"}},\"variants\":[{\"digest\":\"sha256:variant\"}]}]".utf8)
        let parsed = try ContainerImageStorage.images(from: sample)
        precondition(parsed[0].digests == ["sha256:index", "sha256:variant"])
        do { _ = try ContainerImageStorage.images(from: Data("[{}]".utf8)); preconditionFailure("Accepted malformed image inventory") }
        catch {}
        let dockerData = Data("""
        [{"Id":"sha256:docker","RepoTags":[],"RepoDigests":["original@sha256:manifest"],"Config":{"Labels":{"org.outershell.managed":"true"}},"Size":123}]
        """.utf8)
        let dockerImages = try ContainerImageStorage.dockerImages(from: dockerData)
        precondition(dockerImages[0].isOwnedDangling && dockerImages[0].sizeInBytes == 123)
        precondition(candidates([], [], dockerImages) == ["sha256:docker"])
        precondition(candidates(["sha256:docker"], [], dockerImages).isEmpty)
        precondition(candidates([], ["FROM alias@sha256:manifest"], dockerImages).isEmpty)
        let unlabelled = try ContainerImageStorage.dockerImages(from: Data("[{\"Id\":\"sha256:other\",\"Config\":{}}]".utf8))
        precondition(candidates([], [], unlabelled).isEmpty)
        print("PASS: current bases, workspace fallback, owned namespaces, recipes, variable references, digest aliases, malformed inventories, stale selections, deletion failures, and deduplication")
    }
}
