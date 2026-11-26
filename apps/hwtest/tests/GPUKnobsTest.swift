import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// GPURates.knobs on the default device with the facade's band budget at 0: the subtiling knob and texture sector
/// promotion A/B (gpu/GPURatesKnobs.swift). It passes once every arm ran with GPU timestamps; on the facade both probes
/// must be there. The numbers are for comparing runs, not judged here.
struct GPUKnobsTest: HWTest {
    let id = "gpu.perf.knobs"
    let name = "Subtiling perf knob and texture sector promotion, A/B per draw shape"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let production = DrawBandsTest.budget(gpu.device, 0)
        defer { production.map { _ = DrawBandsTest.budget(gpu.device, $0) } }
        let result = try await GPURates.knobs(gpu)
        var metrics = result.metrics
        metrics.set("dev", GM20BFacade.kind(of: gpu.device))
        if !result.passed { return .fail(result.reasons(limit: 4).joined(separator: "; "), metrics) }
        if GM20BFacade.kind(of: gpu.device) == "gm20b" && !result.notes.isEmpty {
            return .fail(result.notes.joined(separator: "; "), metrics)
        }
        return .pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
