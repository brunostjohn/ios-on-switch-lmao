import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Weather's moon card drew a flat pale square (25 Nov, IMG_0026): the frame NanoUniverse's Aegir renderer draws it with
/// (gpu/AegirMoon.swift), run as a fresh queue's first frame, and its bytes against Metal's, which an M4's GPU draws
/// (host_gpu_tests/AegirMoonTests.swift): the 'A008' mask 255 outside the disc and 0 inside, bloom level 2 0 away from it,
/// the BGR10_XR_sRGB drawable XR 0.0 (code 384) outside and the disc's grey (code 846) inside. `<check>_off`: texels unlike Metal's
/// (AegirMoon.misses); `<target>_centre`, `<target>_corner`: what the centre and a corner texel hold; path.
struct AegirMoonTest: HWTest {
    let id = "gpu.gm20b.aegir_moon"
    let name = "Weather's moon frame: mask, bloom and extended-range drawable hold Metal's bytes"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let reading = try await AegirMoon.run(gpu)
        var metrics = Metrics()
        var wrong: [String] = []
        for (check, off) in AegirMoon.misses(reading) {
            metrics.set("\(check)_off", off)
            if off != 0 { wrong.append(check) }
        }
        for (key, value) in AegirMoon.samples(reading) { metrics.set(key, value) }
        metrics.set("path", reading.path)
        if reading.path != "gm20b" { wrong.append("path") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
