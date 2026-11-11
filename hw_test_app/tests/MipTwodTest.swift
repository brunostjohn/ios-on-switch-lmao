import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Rule mip.twod (drivers/TegraGM20BMetal/raw/fx/fx_mip_route.h): generateMipmaps on the 2D engine, step by step against
/// Metal's definition computed from the level above as the GPU left it (MipReference). For BGRA8, RGBA8, RGBA16Float, R8,
/// RG8 and both 8-bit sRGB formats: `<fmt>_wrong` (codes more than one apart) at the proven steps must be 0; `<fmt>_odd_*`
/// are the steps no probe covered (776x24's 97 -> 48, 255x129's 255 -> 127 and 129 -> 64); `<fmt>_3d_*` the same chains drawn
/// by 3D passes into one-level textures (the control, `paths_3d`); `<fmt>_l1_*` level 1 of the two routes compared. GPU time
/// of a 1024x1024 chain each way: `<fmt>_twod_gpu_ms`, `<fmt>_3d_gpu_ms`. `rounding` names how the 2D engine rounds 8-bit
/// 2:1 steps (MipRounding). Probe cells, debug builds: the formats the 3D engine makes (the pack's mip states) recorded
/// on the 2D engine (switchRawProbeMipCandidates:) against their own 3D route, `candidates=<fmt>:<exact|near|wrong>`.
struct MipTwodTest: HWTest {
    let id = "gpu.raw.mip_twod"
    let name = "generateMipmaps on the 2D engine within one code of Metal's step, and its GPU time against 3D passes"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 240 }

    struct Format {
        let name: String
        let pixel: MTLPixelFormat
        let layout: MipLayout
    }

    static let twod = [
        Format(name: "bgra8", pixel: .bgra8Unorm, layout: .unorm([8, 8, 8, 8])),
        Format(name: "rgba8", pixel: .rgba8Unorm, layout: .unorm([8, 8, 8, 8])),
        Format(name: "rgba16f", pixel: .rgba16Float, layout: .floats([16, 16, 16, 16])),
        Format(name: "r8", pixel: .r8Unorm, layout: .unorm([8])),
        Format(name: "rg8", pixel: .rg8Unorm, layout: .unorm([8, 8])),
        Format(name: "bgra8srgb", pixel: .bgra8Unorm_srgb, layout: .unorm([8, 8, 8, 8], srgb: true)),
        Format(name: "rgba8srgb", pixel: .rgba8Unorm_srgb, layout: .unorm([8, 8, 8, 8], srgb: true)),
    ]

    static let candidates = [
        Format(name: "a8", pixel: .a8Unorm, layout: .unorm([8])),
        Format(name: "r16", pixel: .r16Unorm, layout: .unorm([16])),
        Format(name: "r16f", pixel: .r16Float, layout: .floats([16])),
        Format(name: "b5g6r5", pixel: .b5g6r5Unorm, layout: .unorm([5, 6, 5])),
        Format(name: "bgr5a1", pixel: .bgr5A1Unorm, layout: .unorm([1, 5, 5, 5])),
        Format(name: "r32f", pixel: .r32Float, layout: .floats([32])),
        Format(name: "rg16", pixel: .rg16Unorm, layout: .unorm([16, 16])),
        Format(name: "rg16f", pixel: .rg16Float, layout: .floats([16, 16])),
        Format(name: "rgb10a2", pixel: .rgb10a2Unorm, layout: .unorm([10, 10, 10, 2])),
        Format(name: "rg11b10f", pixel: .rg11b10Float, layout: .floats([11, 11, 10])),
        Format(name: "bgr10a2", pixel: .bgr10a2Unorm, layout: .unorm([10, 10, 10, 2])),
        Format(name: "rg32f", pixel: .rg32Float, layout: .floats([32, 32])),
        Format(name: "rgba16", pixel: .rgba16Unorm, layout: .unorm([16, 16, 16, 16])),
        Format(name: "rgba32f", pixel: .rgba32Float, layout: .floats([32, 32, 32, 32])),
    ]

    static let sizes = [(512, 512), (776, 24), (255, 129)]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: Set<String> = [], paths3d: Set<String> = []
        let before = RawFacade.counters(of: gpu.device)
        let rounding = MipRounding()
        for f in Self.twod {
            let chains = MipChains(gpu: gpu, format: f)
            var proven = MipTally(), odd = MipTally(), control = MipTally(), level1 = MipTally()
            for (w, h) in Self.sizes {
                let (twod, path) = try await chains.generated(w, h)
                paths.insert(path)
                chains.compare(twod, w, h, proven: &proven, odd: &odd, rounding: rounding)
                let (drawn, path3d) = try await chains.drawn(twod[0], w, h)
                paths3d.insert(path3d)
                var unused = MipTally()
                chains.compare(drawn, w, h, proven: &control, odd: &unused, rounding: nil)
                level1.add(twod[1], drawn[1], texels: max(1, w / 2) * max(1, h / 2), f.layout)
            }
            Self.report(&metrics, f.name, proven)
            Self.report(&metrics, f.name + "_odd", odd)
            Self.report(&metrics, f.name + "_3d", control)
            Self.report(&metrics, f.name + "_l1", level1)
            if proven.wrong > 0 { wrong.append("\(f.name)_wrong") }
            let (twodMs, drawnMs) = try await chains.times(1024)
            metrics.set("\(f.name)_twod_gpu_ms", twodMs, digits: 3)
            metrics.set("\(f.name)_3d_gpu_ms", drawnMs, digits: 3)
        }
        let after = RawFacade.counters(of: gpu.device)
        for key in ["mip_twod_levels", "mip_twod_unproven_levels", "mip_3d_levels"] {
            metrics.set(key, (after[key] ?? 0) - (before[key] ?? 0))
        }
        metrics.set("round_q1_low", rounding.low[0])
        metrics.set("round_q2_low", rounding.low[1])
        metrics.set("round_q3_low", rounding.low[2])
        metrics.set("round_q1", rounding.seen[0])
        metrics.set("round_q2", rounding.seen[1])
        metrics.set("round_q3", rounding.seen[2])
        metrics.set("rounding", rounding.rule)
        metrics.set("candidates", try await Self.candidateCells(gpu, &metrics))
        metrics.set("paths", paths.sorted().joined(separator: ","))
        metrics.set("paths_3d", paths3d.sorted().joined(separator: ","))
        if (after["mip_twod_levels"] ?? 0) == (before["mip_twod_levels"] ?? 0) { wrong.append("no_twod_levels") }
        if paths != ["raw"] { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    static func report(_ metrics: inout Metrics, _ prefix: String, _ t: MipTally) {
        metrics.set("\(prefix)_exact", t.exact)
        metrics.set("\(prefix)_within_one", t.withinOne)
        metrics.set("\(prefix)_wrong", t.wrong)
        metrics.set("\(prefix)_max_diff", Int(t.maxDiff))
    }

    /// Each 3D-engine format's chain recorded on the 2D engine against the same chain by its 3D passes; "-" where the
    /// facade has no probe switch (a release build)
    static func candidateCells(_ gpu: GPU, _ metrics: inout Metrics) async throws -> String {
        guard MipChains.probe(gpu.device, true) != nil else { return "-" }
        defer { _ = MipChains.probe(gpu.device, false) }
        var cells: [String] = []
        for f in candidates {
            let chains = MipChains(gpu: gpu, format: f)
            var proven = MipTally(), versus = MipTally(), unused = MipTally()
            do {
                for (w, h) in sizes {
                    _ = MipChains.probe(gpu.device, false)
                    let (passes, _) = try await chains.generated(w, h)
                    _ = MipChains.probe(gpu.device, true)
                    let (twod, path) = try await chains.generated(w, h, seed: 0)
                    guard path == "raw" else { throw TestFailure("\(f.name): \(path)") }
                    chains.compare(twod, w, h, proven: &proven, odd: &unused, rounding: nil)
                    versus.add(twod[1], passes[1], texels: twod[1].count / f.layout.bytes, f.layout)
                }
            } catch {
                cells.append("\(f.name):error")
                continue
            }
            report(&metrics, "cand_" + f.name, proven)
            report(&metrics, "cand_\(f.name)_l1", versus)
            cells.append("\(f.name):\(proven.wrong > 0 ? "wrong" : proven.withinOne > 0 ? "near" : "exact")")
        }
        return cells.joined(separator: ",")
    }
}
