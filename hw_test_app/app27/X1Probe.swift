import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// One hardware probe of the X1 research (out/agent-scratch/x1-probe/PROBES.md names each claim, its source and what
/// each outcome means), on the process's default device. Its line carries probe=, claim_verdict=, the probe's numbers,
/// paths= (each command buffer's path on the facade) and reopens= (GPU channels the kernel killed while it ran and the
/// facade opened again). A risky probe programs methods no driver has run on this chip: it runs even after an earlier
/// test left the GPU unanswering, and ends with a fill on a fresh queue, so gpu_after=0 names the probe that wedged it.
struct X1Probe: HWTest {
    let id: String
    let name: String
    let probe: String
    var risky = false
    var timeout: TimeInterval = 120
    let body: @Sendable (GPU) async throws -> ProbeFinding

    var subsystem: Subsystem { .metal }
    var runsWhenGPUWedged: Bool { risky }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let before = RawFacade.counters(of: gpu.device)
        let finding = try await body(gpu)
        let after = RawFacade.counters(of: gpu.device)
        var base = Metrics()
        base.set("dev", RawFacade.kind(of: gpu.device))
        base.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        base.set("reopens", (after["reopens"] ?? 0) - (before["reopens"] ?? 0))
        if risky { base.set("gpu_after", await GPUProbe.answers()) }
        return finding.outcome(probe: probe, base: base, proof: gpu.proof)
    }
}

/// GM20B methods Metal cannot reach (drivers/TegraGM20BMetal/raw/runtime/RawMetalProbe.mm, debug builds of the
/// facade): while armed, every draw of one instance this process encodes emits `before` after its state and `after`
/// once it ended. Pairs are (3D-class method, value).
enum RawMethods {
    static let selector = NSSelectorFromString("switchRawProbeBefore:after:")

    /// nil once armed, else why not.
    static func arm(_ device: MTLDevice, before: [(UInt32, UInt32)], after: [(UInt32, UInt32)]) -> String? {
        guard let object = device as? NSObject, object.responds(to: selector) else {
            return "the device has no raw probe channel"
        }
        let refusal = object.perform(selector, with: data(before), with: data(after))
        return refusal?.takeUnretainedValue() as? String
    }

    static func disarm(_ device: MTLDevice) {
        guard let object = device as? NSObject, object.responds(to: selector) else { return }
        _ = object.perform(selector, with: nil, with: nil)
    }

    static func data(_ pairs: [(UInt32, UInt32)]) -> NSData {
        let words = pairs.flatMap { [$0.0, $0.1] }
        return words.withUnsafeBytes { NSData(bytes: $0.baseAddress, length: $0.count) }
    }

    /// One command buffer drawn with the methods armed; its path on the facade.
    static func draw(
        _ gpu: GPU, before: [(UInt32, UInt32)], after: [(UInt32, UInt32)], _ encode: (MTLCommandBuffer) throws -> Void
    ) async throws -> String {
        if let refusal = arm(gpu.device, before: before, after: after) { throw TestFailure(refusal) }
        defer { disarm(gpu.device) }
        return try await gpu.run(encode).path
    }
}

/// The probes in the order they run: the ones that only use Metal first, then the risky ones one at a time.
enum X1Probes {
    static let all: [any HWTest] = [
        X1MemoryProbes.bufferZero, X1MemoryProbes.linearAlign, X1ConversionProbes.rop, X1ConversionProbes.sampler,
        X1BlendProbes.exact, X1FilterProbes.f32Filter, X1FilterProbes.mipFilter, X1PassProbes.visibility,
        X1RawProbes.blendKills, X1RawProbes.viewportSwizzle, X1IteratedBlendProbe.probe,
    ]
}
