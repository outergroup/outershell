import Dispatch

// DispatchSources error out if you double-call suspend or resume, so we use this wrapper.
final class SafeDispatchSource {
    var dispatchSource: DispatchSourceProtocol
    private(set) var isResumed: Bool
    private var isCancelled = false

    init(dispatchSource: DispatchSourceProtocol, isResumed: Bool) {
        self.dispatchSource = dispatchSource
        self.isResumed = isResumed
    }

    func safeSuspend() {
        guard isResumed else { return }
        dispatchSource.suspend()
        isResumed = false
    }

    func safeResume() {
        guard !isResumed else { return }
        dispatchSource.resume()
        isResumed = true
    }

    func cancel() {
        guard !isCancelled else { return }
        safeResume()
        dispatchSource.cancel()
        isCancelled = true
    }

    deinit {
        cancel()
    }
}
