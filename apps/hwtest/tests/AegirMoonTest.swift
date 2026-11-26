import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal
import QuartzCore

/// The pass structure NanoUniverse's Aegir renderer draws Weather's moon card with (gpu/AegirMoon.swift), run as a fresh
/// queue's first frame, and its bytes against Metal's, which an M4's GPU draws (host_gpu_tests/AegirMoonTests.swift):
/// the 'A008' mask 255 outside the disc and 0 inside, bloom level 2 0 away from it, the BGR10_XR_sRGB drawable XR 0.0
/// (code 384) outside and the disc's grey (code 846) inside. Arm `layer` draws into a CAMetalLayer's drawable, which the
/// kernel lays out as Weather's (block-linear, 64-row blocks: `layer_drawable_facts`, `layer_drawable_layout` where it
/// is not); arm `surface` into a texture over a 'w30r' IOSurface. Green says the passes, their loads, clears and blends
/// and the drawable's layout hold Metal's bytes; luna's program and Core Animation's composite are not covered.
/// `<arm>_<check>_off`: texels unlike Metal's (AegirMoon.misses); `<arm>_<target>_centre`, `_corner`: what the centre
/// and a corner texel hold; `<arm>_<target>_facts`: the driver's layout of each target; path.
struct AegirMoonTest: HWTest {
    let id = "gpu.gm20b.aegir_moon"
    let name = "Weather's moon passes: mask, bloom and extended-range drawable hold Metal's bytes"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let layer = CAMetalLayer()
        layer.device = gpu.device
        layer.pixelFormat = .bgr10_xr_srgb
        layer.framebufferOnly = false
        layer.drawableSize = CGSize(width: AegirMoon.side, height: AegirMoon.side)
        let drawable = layer.nextDrawable()
        let facts = { (texture: MTLTexture) in
            SameBitsCases.facts(gpu, texture).sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }
                .joined(separator: ",")
        }
        var metrics = Metrics()
        var wrong: [String] = drawable == nil ? ["layer_no_drawable"] : []
        var arms: [(String, MTLTexture?)] = [("surface", nil)]
        if let drawable { arms.insert(("layer", drawable.texture), at: 0) }
        for (arm, given) in arms {
            let reading: AegirMoon.Reading
            do {
                reading = try await AegirMoon.run(gpu, drawable: given, facts: facts)
            } catch {
                metrics.set("\(arm)_error", String(describing: error).prefix(80).description)
                wrong.append("\(arm)_error")
                continue
            }
            for (check, off) in AegirMoon.misses(reading) {
                metrics.set("\(arm)_\(check)_off", off)
                if off != 0 { wrong.append("\(arm)_\(check)") }
            }
            for (key, value) in AegirMoon.samples(reading) { metrics.set("\(arm)_\(key)", value) }
            metrics.set("\(arm)_drawable_facts", reading.drawableFacts)
            metrics.set("\(arm)_mask_facts", reading.maskFacts)
            metrics.set("\(arm)_path", reading.path)
            if reading.path != "gm20b" { wrong.append("\(arm)_path") }
        }
        let layout = drawable.map { SameBitsCases.facts(gpu, $0.texture) } ?? [:]
        if !layout.isEmpty && (layout["block_linear"] != 1 || layout["block_rows"] != 64) {
            wrong.append("layer_drawable_layout")
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
