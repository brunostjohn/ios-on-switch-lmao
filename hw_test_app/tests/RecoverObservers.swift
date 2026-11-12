import Foundation
import HWTestCore
import Metal
import QuartzCore
import UIKit

/// Another raw client drawing while gpu.recover.kill_one kills this process's channel: the bench's raw device, over a
/// connection and channel of its own, runs the app's known-answer draw back to back on a thread of its own and keeps its
/// failures and the longest wait between two finished runs.
final class RecoverBystander: @unchecked Sendable {
    private let runner: BenchRunner
    private let experiment: BenchExperiment
    private let lock = NSLock()
    private let ended = DispatchSemaphore(value: 0)
    private var stopping = false, lastNs: UInt64 = 0
    private(set) var runs = 0, errors = 0, longestNs: UInt64 = 0
    private(set) var firstError = ""

    init(_ runner: BenchRunner) throws {
        self.runner = runner
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "bystander",
            "experiments": [
                [
                    "id": "bystander", "kind": "known", "readback": "none", "repeats": 4,
                    "target": ["format": "rgba32float", "width": 64, "height": 64],
                ]
            ],
        ]
        let data = try JSONSerialization.data(withJSONObject: batch)
        guard case .experiment(let e) = try BenchBatch.decode(data).entries.first else {
            throw TestFailure("the bystander's experiment does not decode")
        }
        experiment = e
    }

    func start() {
        lastNs = DispatchTime.now().uptimeNanoseconds
        Thread { [self] in
            while !self.locked({ self.stopping }) {
                let outcome = self.runner.run(self.experiment)
                let now = DispatchTime.now().uptimeNanoseconds
                self.locked {
                    self.runs += 1
                    if outcome.status != "ok" {
                        self.errors += 1
                        if self.firstError.isEmpty { self.firstError = "\(outcome.status):\(outcome.why)" }
                    }
                    self.longestNs = max(self.longestNs, now - self.lastNs)
                    self.lastNs = now
                }
            }
            self.ended.signal()
        }.start()
    }

    /// From now on: the runs, failures and waits before (the first, cold run among them) are not counted.
    func measure() {
        locked {
            runs = 0
            errors = 0
            longestNs = 0
            firstError = ""
            lastNs = DispatchTime.now().uptimeNanoseconds
        }
    }

    /// False: a run still had not returned after `seconds`.
    func stop(seconds: Double) -> Bool {
        locked { stopping = true }
        return ended.wait(timeout: .now() + seconds) == .success
    }

    private func locked<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }
}

/// The compositor's side: a probe layer cleared every display-link frame on the m2v device (NVK's channel, not the raw
/// channel being killed), with the times the display showed each drawable (ShownTimes, as the pacing tests take them).
@MainActor
final class RecoverCompositorProbe: NSObject {
    let probe = CAMetalLayer()
    private let queue: MTLCommandQueue
    private let shown = ShownTimes()
    private var link: CADisplayLink?
    private var frame = 0
    private var measuring = false

    init(device: MTLDevice, queue: MTLCommandQueue) {
        self.queue = queue
        probe.device = device
        probe.pixelFormat = .bgra8Unorm
        probe.framebufferOnly = true
    }

    var presented: [Double] { shown.measured }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    /// Presented times count from now on.
    func measure() { measuring = true }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func tick(_: CADisplayLink) {
        frame += 1
        guard let drawable = probe.nextDrawable(), let commands = queue.makeCommandBuffer() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(
            red: Double(frame % 60) / 60, green: 0.5, blue: 0.2, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        let shown = shown
        let submitted = CACurrentMediaTime()
        let measured = measuring
        drawable.addPresentedHandler { shown.add($0.presentedTime, submitted: submitted, measured: measured) }
        commands.present(drawable)
        commands.commit()
    }
}
