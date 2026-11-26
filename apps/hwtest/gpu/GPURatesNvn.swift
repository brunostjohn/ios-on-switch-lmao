import Foundation
import HWTestCore
import Metal

/// gpu.perf.nvn: state NVN writes and the driver does not, A/B'd through the facade's probes (nvn_probes.h,
/// learn/deko3d.md). Each arm times the same work with the probe off (`_off`) and on (`_on`). Where the state could
/// change pixels, it also counts the pixels that differ from the expected ones (`_wrong`).
/// - zcull_bounds: a program writes a depth farther than its triangle's, then 16 layers of a 64-FMA program sit between
///   the two depths and must be drawn. A second form has the written depth nearer, which hides the layers. The probe
///   opens zcull's bounds for that program in place of invalidating the region.
/// - stencil_zcull: 16 layers whose stencil test fails everywhere, under SET_ZCULL with stencil on and NVN's NOTEQUAL
///   criterion.
/// - zcull_4x2, zcull_fp: the hidden-layer pass of gpu.perf.rates under the Z_4X2 region format, and under ZFORMAT FP for
///   its clear of 1.0.
/// - discard: a loaded target stored DontCare, redrawn whole. `discard_kept_px` counts pixels that still hold the
///   texels from before the pass. More than 0 means the write-back was skipped.
/// - cbr: a 4x BGRA8 and a 4x RGBA16F target on NVN's 2CBR kinds against 2CRA, filled and resolved. The resolved
///   pixels must be equal.
/// - tiled_flush: a pass whose target the next pass reads, with and without the tiled-cache flush at its end
///   (switchMetalProbeForms: switch=nowfi_noflush).
extension GPURates {
    public static func nvn(_ gpu: GPU, _ s: Sizes = .device) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        var notes: [String] = []
        // without the probes (off the facade) every arm still runs, its on form the same as its off one
        if b.probe("switchMetalProbeNvn:", 0) == nil {
            notes.append("the device has no switchMetalProbeNvn: probe: each arm's on form is its off form")
        }
        do {
            try await b.zcullBounds()
            try await b.stencilZcull()
            try await b.zcullFormats()
            try await b.discard()
            try await b.cbr()
            try await b.tiledFlush()
        } catch {
            b.probe("switchMetalProbeNvn:", 0)
            b.probe("switchMetalProbeZcullCriterion:", 0)
            b.probe("switchMetalProbeMs4Cbr:", 0)
            throw error
        }
        var r = SceneResult()
        r.metrics = b.metrics
        r.notes = notes
        for p in b.problems { r.expect(false, p) }
        return r
    }

    static let nvnZcullBounds: UInt32 = 1, nvnStencilZcull: UInt32 = 2, nvnZcull4x2: UInt32 = 4
    static let nvnZcullFp: UInt32 = 8, nvnDiscard: UInt32 = 16
    static let notEqualCriterion: UInt32 = 5 | 0xff << 16

    /// `encode` timed with the probe off and on, each its fastest run, the target read back after the last
    func ab(_ arm: String, bits: UInt32, read: MTLTexture? = nil, want: [UInt8]? = nil,
            _ encode: (MTLCommandBuffer) throws -> Void) async throws {
        for (state, on) in [("off", false), ("on", true)] {
            probe("switchMetalProbeNvn:", on ? bits : 0)
            let t = try await fastest("\(arm)_\(state)", encode)
            probe("switchMetalProbeNvn:", 0)
            metrics.set("\(arm)_\(state)_us", t * 1000, digits: 1)
            if let read, let want {
                let got = try await gpu.read(read, bytesPerPixel: 4)
                metrics.set("\(arm)_\(state)_wrong", zip(got, want).filter { $0 != $1 }.count / 4)
            }
        }
    }

    func expected(_ rgba: SIMD4<Float>) -> [UInt8] {
        let bgra = [rgba.z, rgba.y, rgba.x, rgba.w].map { UInt8(($0 * 255).rounded()) }
        return [UInt8]([[UInt8]](repeating: bgra, count: s.width * s.height).joined())
    }

    func quad(_ e: MTLRenderCommandEncoder, z: Float) {
        var corners = GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: z)
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    /// The depth-writing program's draw, its triangle at 0.1, then the heavy layers at 0.3: drawn where it wrote 0.6
    /// (`seen`), hidden where it wrote 0.2
    func zcullBounds() async throws {
        let writes = try gpu.render(fragment: "rates_set_depth_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let depth = try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        let less = try depthState(.less)
        for (form, written) in [("seen", Float(0.6)), ("hidden", Float(0.2))] {
            let behind = SIMD4<Float>(0, 0, 1, written)
            var want: [UInt8]?
            if form == "hidden" { want = expected(SIMD4(0, 0, 1, 1)) }
            let checked = want == nil ? nil : colour
            try await ab("zcull_bounds_\(form)", bits: Self.nvnZcullBounds, read: checked, want: want) { c in
                let e = self.pass(colour, depth, c)
                e.setDepthStencilState(less)
                e.setRenderPipelineState(writes)
                var w = behind
                e.setFragmentBytes(&w, length: 16, index: 0)
                self.quad(e, z: 0.1)
                e.setRenderPipelineState(heavy)
                var n = self.s.heavyIterations
                e.setFragmentBytes(&n, length: 4, index: 0)
                for _ in 0 ..< self.s.layers { self.quad(e, z: 0.3) }
                e.endEncoding()
            }
            if form == "seen" {
                let got = try await gpu.read(colour, bytesPerPixel: 4)
                let still = expected(SIMD4(0, 0, 1, 1))
                metrics.set("zcull_bounds_seen_culled_px", zip(got, still).filter { $0 == $1 }.count / 4)
            }
        }
    }

    /// A stencil of 1 everywhere, then 16 heavy layers whose stencil test (not equal to 1) fails everywhere
    func stencilZcull() async throws {
        let format = MTLPixelFormat.depth32Float_stencil8
        let flat = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm, depth: format, stencil: format)
        let heavy = try gpu.render(
            fragment: "rates_heavy_fragment",
            format: .bgra8Unorm,
            depth: format,
            stencil: format
        )
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let zeta = try gpu.texture(format, s.width, s.height, usage: [.renderTarget])
        let mark = MTLDepthStencilDescriptor(), test = MTLDepthStencilDescriptor()
        mark.depthCompareFunction = .always
        mark.frontFaceStencil.depthStencilPassOperation = .replace
        mark.backFaceStencil = mark.frontFaceStencil
        test.depthCompareFunction = .always
        test.frontFaceStencil.stencilCompareFunction = .notEqual
        test.backFaceStencil = test.frontFaceStencil
        guard let marks = gpu.device.makeDepthStencilState(descriptor: mark),
              let tests = gpu.device.makeDepthStencilState(descriptor: test)
        else { throw TestFailure("makeDepthStencilState returned nil") }
        probe("switchMetalProbeZcullCriterion:", Self.notEqualCriterion)
        defer { probe("switchMetalProbeZcullCriterion:", 0) }
        let flatColour = SIMD4<Float>(0, 1, 0, 1)
        try await ab("stencil_zcull", bits: Self.nvnStencilZcull, read: colour, want: expected(flatColour)) { c in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .dontCare
            pass.stencilAttachment.texture = zeta
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.storeAction = .dontCare
            let e = c.makeRenderCommandEncoder(descriptor: pass)!
            e.setStencilReferenceValue(1)
            e.setDepthStencilState(marks)
            e.setRenderPipelineState(flat)
            var k = flatColour
            e.setFragmentBytes(&k, length: 16, index: 0)
            self.quad(e, z: 0.5)
            e.setDepthStencilState(tests)
            e.setRenderPipelineState(heavy)
            var n = self.s.heavyIterations
            e.setFragmentBytes(&n, length: 4, index: 0)
            for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
            e.endEncoding()
        }
    }

    /// gpu.perf.rates' hidden layers under each zcull format probe
    func zcullFormats() async throws {
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let flat = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let depth = try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        let lequal = try depthState(.lessEqual)
        let front = SIMD4<Float>(1, 0, 0, 1)
        for (arm, bits) in [("zcull_4x2", Self.nvnZcull4x2), ("zcull_fp", Self.nvnZcullFp)] {
            try await ab(arm, bits: bits, read: colour, want: expected(front)) { c in
                let e = self.pass(colour, depth, c)
                e.setDepthStencilState(lequal)
                e.setRenderPipelineState(flat)
                var k = front
                e.setFragmentBytes(&k, length: 16, index: 0)
                self.quad(e, z: 0.1)
                e.setRenderPipelineState(heavy)
                var n = self.s.heavyIterations
                e.setFragmentBytes(&n, length: 4, index: 0)
                for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
                e.endEncoding()
            }
        }
    }

    /// A target filled with one colour, then a pass that loads it, draws another over all of it and stores DontCare
    func discard() async throws {
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let before = SIMD4<Float>(1, 1, 0, 1), drawn = SIMD4<Float>(0, 1, 1, 1)
        let fill = { (c: MTLCommandBuffer) in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 1, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            c.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let redraw = { (c: MTLCommandBuffer) in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .dontCare
            let e = c.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(solid)
            var k = drawn
            e.setFragmentBytes(&k, length: 16, index: 0)
            for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
            e.endEncoding()
        }
        let kept = expected(before)
        for (state, on) in [("off", false), ("on", true)] {
            try await gpu.run(fill)
            probe("switchMetalProbeNvn:", on ? Self.nvnDiscard : 0)
            let t = try await gpu.run(redraw)
            probe("switchMetalProbeNvn:", 0)
            metrics.set("discard_\(state)_us", t.gpuMilliseconds * 1000, digits: 1)
            let got = try await gpu.read(target, bytesPerPixel: 4)
            metrics.set("discard_\(state)_kept_px", zip(got, kept).filter { $0 == $1 }.count / 4)
        }
    }

    /// 4x targets on 2CRA and on 2CBR: 16 layers of fill, resolved into a one-sample texture, the resolves compared
    func cbr() async throws {
        for (name, format, bytes) in [("bgra8", MTLPixelFormat.bgra8Unorm, 4), ("rgba16f", .rgba16Float, 8)] {
            let solid = try gpu.render(fragment: "solid_fragment", format: format, samples: 4)
            var resolved: [[UInt8]] = []
            for (state, on) in [("off", false), ("on", true)] {
                probe("switchMetalProbeMs4Cbr:", on ? 1 : 0)
                let ms = try gpu.texture(format, s.width, s.height, usage: [.renderTarget], samples: 4)
                probe("switchMetalProbeMs4Cbr:", 0)
                let into = try gpu.texture(format, s.width, s.height)
                let t = try await fastest("cbr_\(name)_\(state)") { c in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = ms
                    pass.colorAttachments[0].resolveTexture = into
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].storeAction = .multisampleResolve
                    let e = c.makeRenderCommandEncoder(descriptor: pass)!
                    e.setRenderPipelineState(solid)
                    for i in 0 ..< self.s.layers {
                        var k = SIMD4<Float>(Float(i) / Float(self.s.layers), 0.5, 0.25, 1)
                        e.setFragmentBytes(&k, length: 16, index: 0)
                        self.quad(e, z: 0.5)
                    }
                    e.endEncoding()
                }
                metrics.set("cbr_\(name)_\(state)_us", t * 1000, digits: 1)
                resolved.append(try await gpu.read(into, bytesPerPixel: bytes))
            }
            metrics.set("cbr_\(name)_differ_bytes", zip(resolved[0], resolved[1]).filter { $0 != $1 }.count)
        }
    }

    /// Pass 1 draws 16 heavy layers into a target; pass 2, in the same command buffer, copies it into another
    func tiledFlush() async throws {
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm)
        let copy = try gpu.render(fragment: "rates_copy_fragment", format: .bgra8Unorm)
        let first = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let second = try gpu.texture(.bgra8Unorm, s.width, s.height)
        var outputs: [[UInt8]] = []
        let probed = (gpu.device.makeCommandQueue()?.makeCommandBuffer() as? NSObject)?
            .responds(to: NSSelectorFromString("switchMetalProbeForms:")) ?? false
        for (state, forms) in [("off", ""), ("on", probed ? "switch=nowfi_noflush" : "")] {
            let t = try await fastest("tiled_flush_\(state)") { c in
                if !forms.isEmpty { try self.forms(c, forms) }
                let e = self.pass(first, nil, c)
                e.setRenderPipelineState(heavy)
                var n = self.s.heavyIterations
                e.setFragmentBytes(&n, length: 4, index: 0)
                for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
                e.endEncoding()
                let f = self.pass(second, nil, c)
                f.setRenderPipelineState(copy)
                f.setFragmentTexture(first, index: 0)
                self.quad(f, z: 0.5)
                f.endEncoding()
            }
            metrics.set("tiled_flush_\(state)_us", t * 1000, digits: 1)
            outputs.append(try await gpu.read(second, bytesPerPixel: 4))
        }
        metrics.set("tiled_flush_wrong", zip(outputs[0], outputs[1]).filter { $0 != $1 }.count / 4)
    }

    func forms(_ c: MTLCommandBuffer, _ forms: String) throws {
        let selector = NSSelectorFromString("switchMetalProbeForms:")
        guard let object = c as? NSObject, object.responds(to: selector) else {
            throw TestFailure("the facade has no switchMetalProbeForms:")
        }
        if let refusal = object.perform(selector, with: forms)?.takeUnretainedValue() as? String {
            throw TestFailure(refusal)
        }
    }
}
