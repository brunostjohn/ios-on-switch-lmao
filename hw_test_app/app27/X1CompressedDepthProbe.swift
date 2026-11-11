import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// copy.ce_compressed for zetas (drivers/TegraGM20BMetal/raw/runtime/raw_comp.h): the copy engine copying a compressed
/// Depth32Float or multisampled Depth32Float_Stencil8 zeta (kinds ZF32_2CZ, ZF32_MS4_2CZ, ZF32_X24S8_MS2/MS4_2CSZV) into the
/// shader's depth-test images and back, as the facade does around framebuffer-fetching draws that test depth in the
/// shader. A debug build's switch (switchRawProbeZetaKinds:) gives those memoryless zetas their compressible kinds; the
/// same pass drawn with it off is the reference. PASS: every round's picture with compressed zetas is the reference's
/// byte for byte and zeta_ce_copies moved; INCONCLUSIVE: no switch, the work left the raw path, or no copy went through
/// compressed memory (the facade had none made yet).
enum X1CompressedDepthProbe {
    static let probe = X1Probe(
        id: "gpu.x1.ce_compressed_depth",
        name: "The copy engine copies compressed shader-tested zetas in and out as plain texels",
        probe: "copy.ce_compressed_depth", risky: true, timeout: 180
    ) { gpu in try await run(gpu) }

    static let size = 64, rounds = 16
    static let cases: [(String, MTLPixelFormat, Int)] = [
        ("d32f_1x", .depth32Float, 1), ("d32f_4x", .depth32Float, 4), ("d32s8_2x", .depth32Float_stencil8, 2),
        ("d32s8_4x", .depth32Float_stencil8, 4),
    ]

    static func zetaKinds(_ device: MTLDevice, _ on: Bool) -> Bool {
        let selector = NSSelectorFromString("switchRawProbeZetaKinds:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return false }
        _ = object.perform(selector, with: NSNumber(value: on))
        return true
    }

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        var metrics = Metrics()
        metrics.set("rule", "copy.ce_compressed_depth")
        guard zetaKinds(gpu.device, false) else {
            return ProbeFinding(.inconclusive, metrics, note: "the facade has no zeta probe switch (a release build)")
        }
        defer { _ = zetaKinds(gpu.device, false) }
        var wrong: [String: Int] = [:]
        var engaged = 0
        for (name, format, samples) in cases where gpu.device.supportsTextureSampleCount(samples) {
            let scene = try Scene(gpu: gpu, format: format, samples: samples)
            _ = zetaKinds(gpu.device, false)
            let reference = try await scene.draw()
            _ = zetaKinds(gpu.device, true)
            var bad = 0, compressed = 0
            for _ in 0..<rounds {
                let before = RawFacade.compression(of: gpu.device)["zeta_ce_copies"] ?? 0
                let picture = try await scene.draw()
                guard (RawFacade.compression(of: gpu.device)["zeta_ce_copies"] ?? 0) > before else { continue }
                compressed += 1
                if picture != reference { bad += 1 }
                try await Task.sleep(nanoseconds: 10_000_000)
            }
            wrong["\(name)"] = bad
            metrics.set("\(name)_compressed_rounds", compressed)
            engaged += compressed
        }
        metrics.set("compressed_rounds", engaged)
        let paths = gpu.paths.paths.keys.map { $0 }
        if engaged == 0 {
            var finding = ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
            finding.verdict = .inconclusive
            finding.note = "no shader-test copy went through a compressed zeta"
            return finding
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }

    /// FramebufferFetchDepthScene's draws over a memoryless zeta: fetching draws test depth in the shader, the plain one
    /// between them on the fixed-function test, so the zeta goes through the copy engine both ways
    struct Scene {
        let gpu: GPU
        let target: MTLTexture, resolved: MTLTexture?, depth: MTLTexture
        let fetch: MTLRenderPipelineState, plain: MTLRenderPipelineState, less: MTLDepthStencilState
        let stencil: Bool

        init(gpu: GPU, format: MTLPixelFormat, samples: Int) throws {
            self.gpu = gpu
            stencil = format == .depth32Float_stencil8
            depth = try gpu.texture(format, size, size, usage: [.renderTarget], samples: samples, storage: .memoryless)
            target = try gpu.texture(.rgba8Unorm, size, size, samples: samples)
            resolved = samples > 1 ? try gpu.texture(.rgba8Unorm, size, size) : nil
            fetch = try gpu.render(
                vertex: "fetch_depth_vertex", fragment: "fetch_add_fragment", format: .rgba8Unorm, samples: samples,
                depth: format, stencil: stencil ? format : .invalid)
            plain = try gpu.render(
                vertex: "fetch_depth_vertex", fragment: "solid_fragment", format: .rgba8Unorm, samples: samples,
                depth: format, stencil: stencil ? format : .invalid)
            let descriptor = MTLDepthStencilDescriptor()
            descriptor.depthCompareFunction = .less
            descriptor.isDepthWriteEnabled = true
            less = gpu.device.makeDepthStencilState(descriptor: descriptor)!
        }

        func draw() async throws -> [UInt8] {
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = resolved == nil ? .store : .multisampleResolve
                pass.colorAttachments[0].resolveTexture = resolved
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .dontCare
                if stencil {
                    pass.stencilAttachment.texture = depth
                    pass.stencilAttachment.loadAction = .clear
                    pass.stencilAttachment.storeAction = .dontCare
                }
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setDepthStencilState(less)
                for (state, z, colour) in [
                    (fetch, Float(0.75), SIMD4<Float>(0.125, 0, 0, 1)), (plain, 0.5, SIMD4(0.25, 0.25, 0.25, 1)),
                    (fetch, 0.625, SIMD4(0, 0.5, 0, 1)), (fetch, 0.25, SIMD4(0, 0, 0.125, 1)),
                    (plain, 0.375, SIMD4(0.5, 0.5, 0.5, 1)), (fetch, 0.125, SIMD4(0, 0.125, 0, 1)),
                ] {
                    encoder.setRenderPipelineState(state)
                    var depth = z
                    var colour = colour
                    encoder.setVertexBytes(&depth, length: 4, index: 0)
                    encoder.setFragmentBytes(&colour, length: 16, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                }
                encoder.endEncoding()
            }
            return try await gpu.read(resolved ?? target, bytesPerPixel: 4)
        }
    }
}
