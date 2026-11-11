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
/// It starts by disarming what a probe that timed out left armed (stale_emitted= draws carried those methods).
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
        let stale = risky ? RawMethods.disarm(gpu.device) : 0
        let before = RawFacade.counters(of: gpu.device)
        var finding: ProbeFinding?
        var failure: String?
        do {
            finding = try await body(gpu)
        } catch {
            failure = String(describing: error)
        }
        let after = RawFacade.counters(of: gpu.device)
        var base = Metrics()
        base.set("dev", RawFacade.kind(of: gpu.device))
        base.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        base.set("reopens", (after["reopens"] ?? 0) - (before["reopens"] ?? 0))
        if risky {
            base.set("gpu_after", await GPUProbe.answers())
            base.set("stale_emitted", stale)
        }
        guard let finding else {
            var metrics = Metrics()
            metrics.set("probe", probe)
            metrics.merge(base)
            return .fail(failure ?? "no finding", metrics, proof: gpu.proof)
        }
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

    /// The draws that emitted the methods since they were armed.
    static func disarm(_ device: MTLDevice) -> Int {
        guard let object = device as? NSObject, object.responds(to: selector) else { return 0 }
        let emitted = object.perform(selector, with: nil, with: nil)?.takeUnretainedValue() as? String
        return emitted.flatMap { Int($0) } ?? 0
    }

    static func data(_ pairs: [(UInt32, UInt32)]) -> NSData {
        let words = pairs.flatMap { [$0.0, $0.1] }
        return words.withUnsafeBytes { NSData(bytes: $0.baseAddress, length: $0.count) }
    }

    /// One command buffer drawn with the methods armed: its path on the facade, and the draws that emitted them
    /// (0: the methods never reached the GPU, whatever the picture shows).
    static func draw(
        _ gpu: GPU, before: [(UInt32, UInt32)], after: [(UInt32, UInt32)], _ encode: (MTLCommandBuffer) throws -> Void
    ) async throws -> (path: String, emitted: Int) {
        if let refusal = arm(gpu.device, before: before, after: after) { throw TestFailure(refusal) }
        do {
            let path = try await gpu.run(encode).path
            return (path, disarm(gpu.device))
        } catch {
            _ = disarm(gpu.device)
            throw error
        }
    }
}

extension ProbeFinding {
    /// emitted= on the line; INCONCLUSIVE when the armed methods reached no draw, whatever the pictures show.
    func emitting(_ draws: Int) -> ProbeFinding {
        var finding = self
        finding.metrics.set("emitted", draws)
        if draws == 0 {
            finding.verdict = .inconclusive
            finding.note = "the probe's methods reached no draw"
        }
        return finding
    }
}

/// The probes in the order they run: the ones that only use Metal first (their bodies in gpu/x1, which the host GPU
/// test runs on a Mac), then the risky ones one at a time.
enum X1Probes {
    static let all: [any HWTest] = [
        X1Probe(
            id: "gpu.x1.buffer_zero", name: "New MTLBuffers read zero after freed ones held 0xAB", probe: "d1/F11",
            body: X1MemoryProbes.bufferZero),
        X1Probe(
            id: "gpu.x1.linear_align", name: "Textures over buffers at offsets and rows 16 mod 32", probe: "d24",
            body: X1MemoryProbes.linearAlign),
        X1Probe(
            id: "gpu.x1.rop_conversion", name: "ROP float to unorm, sRGB, 10-bit and half: all 65,536 halves",
            probe: "E3/d23", body: X1ConversionProbes.rop),
        X1Probe(
            id: "gpu.x1.sampler_conversion", name: "Sampler unorm8 and unorm16 texels as half: every code",
            probe: "E3b", body: X1ConversionProbes.sampler),
        X1Probe(
            id: "gpu.x1.blend_exact", name: "ROP blend on BGRA8 against Metal's half arithmetic, 8.4 M cases a pair",
            probe: "E4", timeout: 300, body: X1BlendProbes.exact),
        X1Probe(
            id: "gpu.x1.f32_filter", name: "RGBA32F linear filtering: {0, 1e6} sampled a quarter of the way",
            probe: "d7", body: X1FilterProbes.f32Filter),
        X1Probe(
            id: "gpu.x1.mip_filter", name: "generateMipmaps' first level against an exact 2x2 box, five formats",
            probe: "G13/d4", body: X1FilterProbes.mipFilter),
        X1Probe(
            id: "gpu.x1.visibility", name: "Visibility results: counting within and across passes, boolean",
            probe: "d17", body: X1PassProbes.visibility),
        X1PassBarrierProbe.probe, X1JobBoundaryProbe.probe,
        X1RawProbes.blendKills, X1RawProbes.viewportSwizzle, X1IteratedBlendProbe.probe,
        X1VICPitchProbe.probe,
    ]
}
