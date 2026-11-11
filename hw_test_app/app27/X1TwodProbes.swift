import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The 2D engine (FERMI_TWOD_A) in place of a 3D pass, per (format, operation): a cell is exact where the engine's
/// bytes equal the 3D route's, the route raw takes today, so the planner can select it there without changing a texel.
/// Each probe runs the 2D work through the facade's probe channel (raw_probe_ops.h) at an open blit encoder.
enum X1TwodProbes {
    /// Metal formats with the 2D engine's name for each (raw_probe_ops.h probeTwodFormat)
    static let formats: [(String, MTLPixelFormat, Int)] = [
        ("bgra8", .bgra8Unorm, 4), ("rgba8", .rgba8Unorm, 4), ("bgra8srgb", .bgra8Unorm_srgb, 4),
        ("rgba8srgb", .rgba8Unorm_srgb, 4), ("rgba16f", .rgba16Float, 8), ("rgb10a2", .rgb10a2Unorm, 4),
        ("rg11b10f", .rg11b10Float, 4), ("r8", .r8Unorm, 1), ("rg8", .rg8Unorm, 2), ("rgba32f", .rgba32Float, 16),
    ]

    /// Per cell: texels that differ, texels more than one code apart (any byte by more than one), the first
    struct Compared {
        var wrong = 0, beyondOne = 0
        var first = ""

        /// Texels that changed where nothing should have written
        init(changed: Int) {
            wrong = changed
            beyondOne = changed
        }

        init(_ got: [UInt8], _ want: [UInt8], width: Int, bytesPerTexel: Int) {
            for t in 0..<min(got.count, want.count) / bytesPerTexel {
                let range = t * bytesPerTexel..<(t + 1) * bytesPerTexel
                guard got[range] != want[range] else { continue }
                wrong += 1
                if zip(got[range], want[range]).contains(where: { abs(Int($0) - Int($1)) > 1 }) { beyondOne += 1 }
                if first.isEmpty {
                    first = "\(t % width),\(t / width):" + got[range].map { String(format: "%02x", $0) }.joined() + ">"
                        + want[range].map { String(format: "%02x", $0) }.joined()
                }
            }
        }
    }

    /// The cells' fields: <cell>=<exact|near|wrong>:<texels wrong> and the counts, with the rule; PASS when every cell
    /// is exact, FAIL when none is, INCONCLUSIVE between (the planner selects per cell either way)
    static func finding(_ rule: String, _ cells: [(String, Compared)], ops: Int, paths: [String]) -> ProbeFinding {
        var metrics = Metrics()
        metrics.set("rule", rule)
        metrics.set("ops", ops)
        let text = cells.map { c in "\(c.0):\(c.1.wrong == 0 ? "exact" : c.1.beyondOne == 0 ? "near" : "wrong"):\(c.1.wrong)" }
        metrics.set("cells", text.joined(separator: ","))
        metrics.set("exact_cells", cells.filter { $0.1.wrong == 0 }.count)
        metrics.set("near_cells", cells.filter { $0.1.wrong > 0 && $0.1.beyondOne == 0 }.count)
        metrics.set("wrong_cells", cells.filter { $0.1.beyondOne > 0 }.count)
        if let firstWrong = cells.first(where: { $0.1.wrong > 0 }) { metrics.set("first", "\(firstWrong.0)@\(firstWrong.1.first)") }
        let offPath = paths.filter { $0 != "raw" }
        if !offPath.isEmpty { return ProbeFinding(.inconclusive, metrics, note: "not on the raw path: \(offPath)") }
        if ops == 0 { return ProbeFinding(.inconclusive, metrics, note: "the 2D work never reached a blit encoder") }
        let exact = cells.filter { $0.1.wrong == 0 }.count
        let verdict: ClaimVerdict = exact == cells.count ? .pass : exact == 0 ? .fail : .inconclusive
        return ProbeFinding(verdict, metrics, note: verdict == .inconclusive ? "exact in some cells: see cells" : "")
    }

    /// A source of `format` holding a hash pattern with a gradient under it (every code of a channel appears)
    static func source(_ gpu: GPU, _ format: MTLPixelFormat, _ bpp: Int, _ w: Int, _ h: Int) async throws -> MTLTexture {
        let t = try gpu.texture(format, w, h, usage: [.renderTarget, .shaderRead])
        var bytes = [UInt8](repeating: 0, count: w * h * bpp)
        for i in 0..<w * h {
            let v = probeHash(UInt32(i % w), UInt32(i / w), 7)
            for b in 0..<bpp {
                bytes[i * bpp + b] = b % 2 == 0 ? UInt8(truncatingIfNeeded: v >> (8 * (b % 4))) : UInt8((i % w) * 255 / max(1, w - 1))
            }
            if format == .rgba16Float || format == .rgba32Float {
                for c in 0..<4 {  // finite values in [0, 1]: halves 0x0000-0x3bff, floats from the hash
                    if format == .rgba16Float {
                        let half = UInt16(truncatingIfNeeded: v >> (c * 4)) % 0x3c00
                        bytes[i * 8 + c * 2] = UInt8(half & 0xff)
                        bytes[i * 8 + c * 2 + 1] = UInt8(half >> 8)
                    } else {
                        let f = Float(UInt16(truncatingIfNeeded: v >> (c * 4))) / 65535
                        withUnsafeBytes(of: f.bitPattern.littleEndian) { bytes.replaceSubrange(i * 16 + c * 4..<i * 16 + c * 4 + 4, with: $0) }
                    }
                }
            }
        }
        try await gpu.upload(bytes, to: t, bytesPerRow: w * bpp)
        return t
    }

    /// gpu.x1.twod_scale (copy.twod, METALMAP d4/d18): a scaled copy by the 2D engine, point and bilinear, corner origin at
    /// half a step (the mip chain's sampling, fx::kGm20bSampling), against the 3D route's nearest or linear sample at
    /// each destination pixel's centre. Ratios 1:1, 2:1, 3:7 (up) and 3:2 (down).
    static let scale = X1Probe(
        id: "gpu.x1.twod_scale", name: "2D-engine scaled copies against the 3D route's, per format, ratio and filter",
        probe: "copy.twod", risky: true, timeout: 120
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_scale")
        var cells: [(String, Compared)] = []
        var ops = 0
        let ratios = [("1to1", 64, 64), ("2to1", 128, 64), ("3to7", 30, 70), ("3to2", 96, 64)]
        for (name, format, bpp) in formats {
            journal.starting(name)
            let pipeline = try gpu.render(fragment: "x1_scale_fragment", format: format)
            for (ratio, from, to) in ratios {
                let src = try await source(gpu, format, bpp, from, from)
                for bilinear in [false, true] {
                    let sampler = gpu.device.makeSamplerState(descriptor: {
                        let d = MTLSamplerDescriptor()
                        d.minFilter = bilinear ? .linear : .nearest
                        d.magFilter = d.minFilter
                        return d
                    }())!
                    let viaTwod = try gpu.texture(format, to, to, usage: [.renderTarget, .shaderRead])
                    let via3d = try gpu.texture(format, to, to, usage: [.renderTarget, .shaderRead])
                    let du = (UInt64(from) << 32) / UInt64(to)
                    let batch = try await gpu.run(forms: "") { commands in
                        let blit = commands.makeBlitCommandEncoder()!
                        try RawProbe.engine(commands, [
                            "kind": "scale", "read": src, "written": viaTwod, "du": du, "dv": du, "sx": du / 2,
                            "sy": du / 2, "bilinear": bilinear ? 1 : 0, "corner": 1,
                        ])
                        blit.endEncoding()
                        let encoder = commands.makeRenderCommandEncoder(descriptor: BarrierKit.pass(via3d))!
                        encoder.setRenderPipelineState(pipeline)
                        encoder.setFragmentTexture(src, index: 0)
                        encoder.setFragmentSamplerState(sampler, index: 0)
                        var size = SIMD2<Float>(Float(to), Float(to))
                        encoder.setFragmentBytes(&size, length: 8, index: 0)
                        var corners = GPU.fullScreen
                        encoder.setVertexBytes(&corners, length: 64, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                    ops += batch.taken["ops"] ?? 0
                    let got = try await gpu.read(viaTwod, bytesPerPixel: bpp), want = try await gpu.read(via3d, bytesPerPixel: bpp)
                    cells.append(("\(name)_\(ratio)_\(bilinear ? "linear" : "point")", Compared(got, want, width: to, bytesPerTexel: bpp)))
                }
            }
        }
        return finding("copy.twod", cells, ops: ops, paths: gpu.paths.paths.keys.map { $0 })
    }

    /// gpu.x1.twod_resolve (resolve.twod, METALMAP d5): a multisampled pass's resolve by the 2D engine, its sample grid read
    /// as one image at 2:1 (corner origin at half a step: the box of each pixel's samples), against the resolve pass raw
    /// draws today (the pack's resolve state). Samples carry values whose sums land on half codes.
    static let resolve = X1Probe(
        id: "gpu.x1.twod_resolve", name: "2D-engine MSAA resolves against the resolve pass, per format and sample count",
        probe: "resolve.twod", risky: true, timeout: 120
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_resolve")
        var cells: [(String, Compared)] = []
        var ops = 0
        let resolvable = formats.filter { ["bgra8", "rgba8", "bgra8srgb", "rgba16f", "rgb10a2", "rg11b10f"].contains($0.0) }
        var colors: [SIMD4<Float>] = []
        for set in 0..<4 {
            for s in 0..<4 {
                let base = Float(set * 37 + 20) / 255, step = Float(s % 2) / 255
                colors.append(SIMD4(base + step, 1 - base - step, Float(set) / 4 + step * Float(s), Float(s + 1) / 4))
            }
        }
        for (name, format, bpp) in resolvable {
            for samples in [2, 4] where gpu.device.supportsTextureSampleCount(samples) {
                journal.starting("\(name)_\(samples)x")
                let pipeline = try gpu.render(fragment: "x1_sample_fragment", format: format, samples: samples)
                let (w, h) = (96, 64)
                var results: [[UInt8]] = []
                for forms in ["resolve=draw", "resolve=twod"] {
                    let ms = try gpu.texture(format, w, h, usage: [.renderTarget], samples: samples, storage: .memoryless)
                    let into = try gpu.texture(format, w, h, usage: [.renderTarget, .shaderRead])
                    let batch = try await gpu.run(forms: forms) { commands in
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = ms
                        pass.colorAttachments[0].resolveTexture = into
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].storeAction = .multisampleResolve
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(pipeline)
                        encoder.setFragmentBytes(&colors, length: colors.count * 16, index: 0)
                        var corners = GPU.fullScreen
                        encoder.setVertexBytes(&corners, length: 64, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                    if forms == "resolve=twod" { ops += batch.taken["resolves"] ?? 0 }
                    results.append(try await gpu.read(into, bytesPerPixel: bpp))
                }
                cells.append(("\(name)_\(samples)x", Compared(results[1], results[0], width: w, bytesPerTexel: bpp)))
            }
        }
        return finding("resolve.twod", cells, ops: ops, paths: gpu.paths.paths.keys.map { $0 })
    }

    /// gpu.x1.twod_clear (clear.twod, LIMITS 2D-4): a 2D-engine solid rectangle against a 3D clear of the same target, the
    /// colour given as four floats (RF32_GF32_BF32_AF32), including sRGB and 10-bit targets; the rectangle covers the
    /// target but a one-texel border, which keeps the colour the target held (cells _edge count texels changed there).
    static let clear = X1Probe(
        id: "gpu.x1.twod_clear", name: "2D-engine solid fills against 3D clears, per format and colour",
        probe: "clear.twod", risky: true, timeout: 120
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_clear")
        var cells: [(String, Compared)] = []
        var ops = 0
        let colors: [SIMD4<Float>] = [
            SIMD4(0, 0, 0, 1), SIMD4(1, 1, 1, 1), SIMD4(0.5, 0.25, 0.75, 1), SIMD4(0.5 / 255, 1.5 / 255, 127.5 / 255, 0.5),
            SIMD4(0.2, 0.4, 0.6, 0.8), SIMD4(1.0 / 3, 2.0 / 3, 0.1, 0.9),
        ]
        let (w, h) = (40, 24)
        for (name, format, bpp) in formats {
            journal.starting(name)
            for (index, color) in colors.enumerated() {
                let viaTwod = try gpu.texture(format, w, h, usage: [.renderTarget, .shaderRead])
                let via3d = try gpu.texture(format, w, h, usage: [.renderTarget, .shaderRead])
                let clear = MTLClearColor(red: Double(color.x), green: Double(color.y), blue: Double(color.z), alpha: Double(color.w))
                let border = MTLClearColor(red: 0.125, green: 0.375, blue: 0.625, alpha: 0.875)
                let batch = try await gpu.run(forms: "") { commands in
                    for t in [viaTwod, via3d] {
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = t
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].clearColor = t === via3d ? clear : border
                        pass.colorAttachments[0].storeAction = .store
                        commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                    }
                    let blit = commands.makeBlitCommandEncoder()!
                    try RawProbe.engine(commands, [
                        "kind": "fill", "written": viaTwod, "dx": 1, "dy": 1, "dw": w - 2, "dh": h - 2, "colorFormat": 0xc0,
                        "color0": color.x.bitPattern, "color1": color.y.bitPattern, "color2": color.z.bitPattern,
                        "color3": color.w.bitPattern,
                    ])
                    blit.endEncoding()
                }
                ops += batch.taken["ops"] ?? 0
                let got = try await gpu.read(viaTwod, bytesPerPixel: bpp), want = try await gpu.read(via3d, bytesPerPixel: bpp)
                let inside = (1..<h - 1).flatMap { y in got[(y * w + 1) * bpp..<(y * w + w - 1) * bpp] }
                let wantInside = (1..<h - 1).flatMap { y in want[(y * w + 1) * bpp..<(y * w + w - 1) * bpp] }
                cells.append(("\(name)_c\(index)", Compared(Array(inside), Array(wantInside), width: w - 2, bytesPerTexel: bpp)))
                if index == 0 {
                    let first = Array(got[0..<bpp])
                    let edge = (0..<w * h).filter { t in
                        (t % w == 0 || t % w == w - 1 || t / w == 0 || t / w == h - 1) && Array(got[t * bpp..<(t + 1) * bpp]) != first
                    }.count
                    cells.append(("\(name)_edge", Compared(changed: edge)))
                }
            }
        }
        return finding("clear.twod", cells, ops: ops, paths: gpu.paths.paths.keys.map { $0 })
    }
}
