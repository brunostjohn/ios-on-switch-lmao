import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Core Animation's framebuffer-fetching fill (fixed_frag_lph_cph with fc_framebuffer_fetch: PBGRAXm_A2Xhf, BGRA8
/// beside the memoryless colour 1) over busy content, in the draws whose raster order the raw path keeps differently:
/// three instances of one quad in the middle of three render encoders of one command buffer, a strip of six vertices
/// that folds back over itself, and two overlapping quads in one draw over a depth attachment and into four samples
/// (states the corpus lacks). Each pass reports its command buffer's route (path_<pass>) and its fetching draws'
/// (fetch_<pass>); raw and m2v must draw the same picture (hash_<pass>), and none may be black. Where the instanced
/// draw was refused as it was recorded, the two encoders around it must stay raw.
struct CAFetchRouteScene: GPUScene {
    let id = "gpu.ca.fetch_routes"
    let name = "CA framebuffer-fetching draws: instanced, folded strip, depth and multisample routes and pixels"

    static let size = 64
    static let line =
        "R ok=1 label=com.apple.coreanimation.PBGRAXm_A2Xhf samples=1 topo=0 ds=0/0 "
        + "c0=80:15:0:1:0:0:1:0:0 c1=80:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 "
        + "a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v fixed_vert_lph_spc fc_generic:53:00 "
        + "fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 fc_vertex_layout:49:00 | f fixed_frag_lph_cph "
        + "fc_attachment_count:49:02 fc_blend_function:49:3a fc_clamp_color:53:00 fc_color_inputs0:49:00 "
        + "fc_color_inputs1:49:00 fc_color_opaque0:53:00 fc_color_opaque1:53:00 "
        + "fc_color_opcodes0:52:00000000 fc_color_opcodes1:52:00000000 fc_coord_function0:49:00 "
        + "fc_coord_function1:49:00 fc_debug_xr:53:00 fc_dest_count:49:00 fc_dest_index:49:00 "
        + "fc_destination_function:49:00 fc_extended_range:53:00 fc_filter0:49:00 fc_filter1:49:00 "
        + "fc_framebuffer_fetch:53:01 fc_generic:53:00 fc_has_asg_77_weights:53:00 "
        + "fc_has_attachment_0:53:00 fc_has_attachment_1:53:00 fc_has_attachment_2:53:00 "
        + "fc_has_color_cube0:53:00 fc_has_color_cube1:53:00 fc_has_color_params:53:00 "
        + "fc_has_color_trc0:53:00 fc_has_color_trc1:53:00 fc_has_edr_factor:53:00 fc_has_gamma_luts:53:00 "
        + "fc_has_hdr_scale:53:00 fc_has_image_0A:53:00 fc_has_image_0B:53:00 fc_has_image_0C:53:00 "
        + "fc_has_image_1A:53:00 fc_has_image_1B:53:00 fc_has_image_1C:53:00 fc_has_lod_bias:53:00 "
        + "fc_has_lut_tex:53:00 fc_has_noise_scale:53:00 fc_has_noise_tex:53:00 fc_has_premultiply:53:00 "
        + "fc_has_ycbcr_matrix:53:00 fc_image_count:49:00 fc_image_function0:49:00 fc_image_function1:49:00 "
        + "fc_memoryless_index0:49:00 fc_memoryless_index1:49:00 fc_noise_width:49:00 fc_plane_count0:49:00 "
        + "fc_plane_count1:49:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 "
        + "fc_texture_function:49:00 fc_unbounded_range:53:00 fc_vertex_layout:49:00 |"
    static let fill = SIMD4<Float16>(0.2, 0.6, 0.9, 1)

    enum Pass: String, CaseIterable {
        case instanced, strip, depth, msaa
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        guard let library = try? gpu.device.makeLibrary(URL: CAState.library) else {
            result.notes.append("no QuartzCore library here")
            return result
        }
        for pass in Pass.allCases {
            let text = Self.line.replacingOccurrences(of: "samples=1 topo=0 ds=0/0", with: Self.shape(pass))
            guard let line = PipelineLine(text) else {
                result.expect(false, "\(pass.rawValue): the state line does not parse")
                continue
            }
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            } catch {
                result.notes.append("\(pass.rawValue): no state (\(GPU.describe(error)))")
                continue
            }
            do {
                try await draw(pass, pipeline, samples: line.samples, gpu, &result)
            } catch {
                result.notes.append("\(pass.rawValue): \(GPU.describe(error))")
            }
        }
        return result
    }

    /// The pass's samples and depth/stencil formats as its state line has them (252: Depth32Float)
    static func shape(_ pass: Pass) -> String {
        switch pass {
        case .depth: return "samples=1 topo=0 ds=252/0"
        case .msaa: return "samples=4 topo=0 ds=0/0"
        case .instanced, .strip: return "samples=1 topo=0 ds=0/0"
        }
    }

    func draw(
        _ pass: Pass, _ pipeline: MTLRenderPipelineState, samples: Int, _ gpu: GPU, _ result: inout SceneResult
    ) async throws {
        let size = Self.size
        let content = Patterns.bytes(
            Patterns.opaque(Patterns.busy(width: size, height: size, channels: 4, seed: 5)), .bgra8Unorm)
        let resolved = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
        let target =
            try samples > 1 ? gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget], samples: samples) : resolved
        if samples == 1 { try await gpu.upload(content, to: resolved, bytesPerRow: size * 4) }
        let colour1 = try gpu.texture(
            .bgra8Unorm, size, size, usage: [.renderTarget], samples: samples, storage: .memoryless)
        let depth: MTLTexture? =
            try pass == .depth ? gpu.texture(.depth32Float, size, size, usage: [.renderTarget]) : nil
        let depthState = depth.flatMap { _ -> MTLDepthStencilState? in
            let descriptor = MTLDepthStencilDescriptor()
            descriptor.depthCompareFunction = .lessEqual
            descriptor.isDepthWriteEnabled = true
            return gpu.device.makeDepthStencilState(descriptor: descriptor)
        }
        let before = RawFacade.counters(of: gpu.device)
        let timing = try await gpu.run(tolerateError: true) { commands in
            func encoder() -> MTLRenderCommandEncoder {
                let descriptor = MTLRenderPassDescriptor()
                descriptor.colorAttachments[0].texture = target
                descriptor.colorAttachments[0].loadAction = samples > 1 ? .clear : .load
                descriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0.5, blue: 0.5, alpha: 1)
                descriptor.colorAttachments[0].storeAction = samples > 1 ? .multisampleResolve : .store
                if samples > 1 { descriptor.colorAttachments[0].resolveTexture = resolved }
                descriptor.colorAttachments[1].texture = colour1
                descriptor.colorAttachments[1].loadAction = .clear
                descriptor.colorAttachments[1].storeAction = .dontCare
                if let depth {
                    descriptor.depthAttachment.texture = depth
                    descriptor.depthAttachment.loadAction = .clear
                    descriptor.depthAttachment.clearDepth = 1
                    descriptor.depthAttachment.storeAction = .dontCare
                }
                let made = commands.makeRenderCommandEncoder(descriptor: descriptor)!
                made.setRenderPipelineState(pipeline)
                if let depthState { made.setDepthStencilState(depthState) }
                return made
            }
            switch pass {
            case .instanced:
                let spans: [(Float, Float, Int)] = [(-0.9, -0.5, 1), (-0.3, 0.3, 3), (0.5, 0.9, 1)]
                for (x0, x1, instances) in spans {
                    let e = encoder()
                    CAState.bind(
                        e, vertices: CAState.rectangle(x0: x0, y0: -0.8, x1: x1, y1: 0.8, color: Self.fill), source: nil
                    )
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: instances)
                    e.endEncoding()
                }
            case .strip:
                let e = encoder()
                CAState.bind(e, vertices: Self.foldedStrip(), source: nil)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 6)
                e.endEncoding()
            case .depth, .msaa:
                let e = encoder()
                CAState.bind(e, vertices: Self.overlappingQuads(), source: nil)
                e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 12)
                e.endEncoding()
            }
        }
        let after = RawFacade.counters(of: gpu.device)
        let name = pass.rawValue
        result.metrics.set("path_" + name, timing.path)
        result.metrics.set("fetch_" + name, RawFacade.fetchRoutes(before: before, after: after))
        result.expect(!timing.path.hasPrefix("error:"), "\(name): \(timing.path)")
        let picture = try await gpu.raster(resolved, .bgra8Unorm)
        result.metrics.set("hash_" + name, Self.coarse(picture))
        result.expect(!Self.black(picture), "\(name): the fetching draws left the target black")
        if samples == 1 {
            let reference = TexelFormat.bgra8Unorm.raster(content, width: size, height: size)
            result.expect(
                RasterDiff.compare(picture, reference, tolerance: 2.0 / 255).wrong > 0,
                "\(name): the fetching draws left the content unchanged")
        }
        let refusedInstances = (after["fetch_record.instances"] ?? 0) > (before["fetch_record.instances"] ?? 0)
        if pass == .instanced, RawFacade.kind(of: gpu.device) == "raw", refusedInstances {
            result.expect(
                timing.path.hasPrefix("split:2+1:"),
                "the instanced draw's encoder went to m2v, and its neighbours with it: \(timing.path)")
        }
    }

    /// Six vertices whose last two triangles fold back over the first two
    static func foldedStrip() -> [CAState.Vertex] {
        let corners: [(Float, Float)] = [(-0.8, -0.8), (0.8, -0.8), (-0.8, 0.8), (0.8, 0.8), (-0.6, -0.4), (0.6, -0.4)]
        return corners.map { corner in CAState.Vertex(corner.0, corner.1, u: 0, v: 0, color: Self.fill) }
    }

    /// Two quads as a triangle list, the second over half of the first
    static func overlappingQuads() -> [CAState.Vertex] {
        let spans: [(Float, Float)] = [(-0.8, 0.4), (-0.4, 0.8)]
        return spans.flatMap { span -> [CAState.Vertex] in
            let corners = CAState.rectangle(x0: span.0, y0: -0.6, x1: span.1, y1: 0.6, color: Self.fill)
            return [corners[0], corners[1], corners[2], corners[2], corners[1], corners[3]]
        }
    }

    static func coarse(_ picture: Raster) -> String {
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for value in picture.boxDownsampled(by: 8).samples {
            let level = value.isFinite ? max(0, min(15, Int(value * 16))) : 0
            hash = (hash ^ UInt64(level)) &* 0x100_0000_01b3
        }
        return String(hash, radix: 16)
    }

    static func black(_ picture: Raster) -> Bool {
        !picture.samples.enumerated().contains { $0.offset % picture.channels != 3 && $0.element > 1.0 / 255 }
    }
}
