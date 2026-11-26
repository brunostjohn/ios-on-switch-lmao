import Foundation
import HWTestCore
import Metal

/// gpu.perf.knobs: two fixed-function settings A/B'd through the facade's probes.
///
/// - SET_SUBTILING_PERF_KNOB_A: our 0x20164010 (NVK's desktop value, GM20B's golden one) against uam's 0x087F6080 and
///   0x20806080 (3 or more colour outputs) and its commented-out 0x06164010. Each runs on a fragment-heavy draw into one
///   target, into four, with a program-written depth, and on a plain fill. The `_us_768` fields scale a time by
///   `lane_mhz` / 768, which holds for these clock-bound draws.
/// - SECTOR_PROMOTION of block-linear texture headers: none, to 2V (deko3d's), to 2H and to 4, on a 2048x2048 RGBA8
///   texture sampled minified, sampled at 1:1, and read at scattered texels. These are bound by DRAM traffic, not the
///   clock, so they keep their measured times.
extension GPURates {
    static let knobValues: [UInt32] = [0x2016_4010, 0x087F_6080, 0x2080_6080, 0x0616_4010]
    static let golden: UInt32 = 0x2016_4010

    public static func knobs(_ gpu: GPU, _ s: Sizes = .device) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        try await b.alu()
        var notes: [String] = []
        if b.probe("switchMetalProbeSubtilingKnob:", 0) == nil {
            notes.append("no subtiling arms beside the default: the device has no switchMetalProbeSubtilingKnob: probe")
            try await b.subtiling(nil)
        } else {
            do {
                for knob in knobValues { try await b.subtiling(knob) }
                try await b.subtiling(golden)
                b.probe("switchMetalProbeSubtilingKnob:", 0)
            } catch {
                // left at the golden value, the next pass puts the channel's knob back for the tests after this one
                b.probe("switchMetalProbeSubtilingKnob:", golden)
                throw error
            }
        }
        let promotions: [UInt32] = b.probe("switchMetalProbeSectorPromotion:", 0) == nil ? [0] : [0, 1, 2, 3]
        if promotions.count == 1 {
            notes.append("no promotion arms beside none: the device has no switchMetalProbeSectorPromotion: probe")
        }
        try await b.promotion(promotions)
        var r = SceneResult()
        r.metrics = b.metrics
        r.notes = notes
        for p in b.problems { r.expect(false, p) }
        return r
    }

    /// The probe's answer after setting it to `value`; nil where the device has no such probe
    @discardableResult
    func probe(_ name: String, _ value: UInt32) -> String? {
        let selector = NSSelectorFromString(name)
        guard let object = gpu.device as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector, with: NSNumber(value: value))?.takeUnretainedValue() as? String ?? ""
    }

    /// The four draws under `knob` (nil: whatever the channel holds, for a device without the probe)
    func subtiling(_ knob: UInt32?) async throws {
        if let knob { probe("switchMetalProbeSubtilingKnob:", knob) }
        let tag = knob.map { String($0, radix: 16) } ?? "default"
        let one = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm)
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let four = try gpu.render(fragment: "rates_heavy_mrt_fragment", format: .bgra8Unorm) { d in
            for j in 1 ..< 4 { d.colorAttachments[j].pixelFormat = .bgra8Unorm }
        }
        let depth = try gpu.render(fragment: "rates_heavy_depth_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let always = try depthState(.always)
        let targets = try (0 ..< 4).map { _ in try gpu.texture(.bgra8Unorm, s.width, s.height) }
        let zeta = try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        let cases: [(String, MTLRenderPipelineState, Int, Bool)] = [
            ("one", one, 1, false), ("four", four, 4, false), ("depth", depth, 1, true), ("fill", solid, 1, false),
        ]
        for (name, pipeline, colours, writesDepth) in cases {
            let t = try await fastest("subtile_\(name)_\(tag)") { c in
                let pass = MTLRenderPassDescriptor()
                for j in 0 ..< colours {
                    pass.colorAttachments[j].texture = targets[j]
                    pass.colorAttachments[j].loadAction = .clear
                    pass.colorAttachments[j].storeAction = .store
                }
                if writesDepth {
                    pass.depthAttachment.texture = zeta
                    pass.depthAttachment.loadAction = .clear
                    pass.depthAttachment.storeAction = .dontCare
                }
                let e = c.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(pipeline)
                if writesDepth { e.setDepthStencilState(always) }
                var corners = GPU.fullScreen
                var n = self.s.heavyIterations
                var colour = SIMD4<Float>(0.2, 0.4, 0.6, 1)
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                if pipeline === solid {
                    e.setFragmentBytes(&colour, length: 16, index: 0)
                } else {
                    e.setFragmentBytes(&n, length: 4, index: 0)
                }
                for _ in 0 ..< self.s.layers { e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
                e.endEncoding()
            }
            metrics.set("subtile_\(name)_\(tag)_us", t * 1000, digits: 1)
            if laneMhz > 0 { metrics.set("subtile_\(name)_\(tag)_us_768", t * 1000 * laneMhz / 768, digits: 1) }
        }
    }

    /// A 2048x2048 texture per mode, made under it and copied from one source, sampled three ways. The source stays
    /// unpromoted; only `image`'s header is under the mode.
    func promotion(_ modes: [UInt32]) async throws {
        let side = 2048
        let source = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead, .renderTarget])
        let bytes = (0 ..< side * side * 4).map { UInt8(truncatingIfNeeded: ($0 &* 40503) >> 9) }
        try await gpu.upload(bytes, to: source, bytesPerRow: side * 4)
        let sample = try gpu.render(fragment: "rates_sample_fragment", format: .bgra8Unorm)
        let scatter = try gpu.render(fragment: "rates_scatter_fragment", format: .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, s.width, s.height)
        for mode in modes {
            // held through the mode's arms: a header written later (a move to compressed memory) takes it too
            probe("switchMetalProbeSectorPromotion:", mode)
            defer { probe("switchMetalProbeSectorPromotion:", 0) }
            let image = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead, .renderTarget])
            try await gpu.run { c in
                let e = c.makeBlitCommandEncoder()!
                e.copy(from: source, to: image)
                e.endEncoding()
            }
            let cases: [(String, MTLRenderPipelineState, Float)] = [
                ("minified", sample, 1.6 / Float(s.width)), ("one", sample, 1 / Float(side)), ("scatter", scatter, 0),
            ]
            for (name, pipeline, scale) in cases {
                let t = try await fastest("promo_\(name)_\(mode)") { c in
                    let e = self.pass(target, nil, c)
                    e.setRenderPipelineState(pipeline)
                    e.setFragmentTexture(image, index: 0)
                    var corners = GPU.fullScreen
                    var step = scale
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    e.setFragmentBytes(&step, length: 4, index: 0)
                    for _ in 0 ..< self.s.layers {
                        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                    e.endEncoding()
                }
                metrics.set("promo_\(name)_\(mode)_us", t * 1000, digits: 1)
            }
        }
    }
}
