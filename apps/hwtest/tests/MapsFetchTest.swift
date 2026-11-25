import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Maps' map view as GM20B draws it: a 1280x720 BGRA8Unorm_sRGB target at 4 samples, resolved into the drawable,
/// under VectorKit's framebuffer-fetch states (shaders/MapsFetch.metal: Hillshade's blend modes read the fetched colour
/// in every mode, GlowAlpha keeps the colour and takes the alpha's minimum). Each fetch arm runs beside the same
/// arithmetic by the ROP (hillshade's multiply as destination times a colour, GlowAlpha as an alpha minimum with the
/// colour masked), `draws` full-screen draws a pass, the median of `runs`:
///   us_<arm>          one draw's GPU time (the pass less a pass of no draw)
///   fetch_<arm>       the facade's fetch route counters over the arm's passes
///   worst_<shape>     the largest code difference between the fetch arm's resolved image and the ROP arm's
///   resolve_us        the 4x sRGB resolve (a pass of no draw resolved, less the same pass stored)
///   plain_us          a draw of a colour with no blend: the floor a draw of this target costs
/// PASS where every arm ran on the driver with GPU stamps and each fetch arm's image is within `bound` codes of its
/// ROP arm's (GlowAlpha exactly).
struct MapsFetchTest: HWTest {
    let id = "gpu.gm20b.maps_fetch"
    let name = "Maps' 4x sRGB framebuffer-fetch states and their resolve against the ROP"
    let subsystem = Subsystem.metal

    static let width = 1280
    static let height = 720
    static let samples = 4
    static let draws = 4
    static let runs = 5
    static let bound = 2
    static let clear = MTLClearColor(red: 0.3, green: 0.55, blue: 0.4, alpha: 1)

    struct Style {
        var low = SIMD4<Float>(0.55, 0.5, 0.45, 1)
        var high = SIMD4<Float>(1, 1, 1, 1)
        var opacity: Float16 = 0.6
        var mode: Int32 = 1
        var pad: (Int32, Int32) = (0, 0)
    }

    struct Arm {
        let label: String
        let pipeline: MTLRenderPipelineState
    }

    func pipeline(_ gpu: GPU, _ fragment: String, _ blend: (MTLRenderPipelineColorAttachmentDescriptor) -> Void)
        throws -> MTLRenderPipelineState
    {
        try gpu.render(
            vertex: "maps_fetch_vertex", fragment: fragment, format: .bgra8Unorm_srgb, samples: Self.samples
        ) { blend($0.colorAttachments[0]) }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let facade = GM20BFacade.kind(of: gpu.device) == "gm20b"
        guard gpu.device.supportsTextureSampleCount(Self.samples) else { return .fail("4x MSAA unsupported") }
        var metrics = Metrics()
        metrics.set("draws", Self.draws)
        let arms: [Arm]
        do {
            arms = [
                Arm(label: "plain", pipeline: try pipeline(gpu, "maps_plain_fragment") { _ in }),
                Arm(label: "hillshade_fetch", pipeline: try pipeline(gpu, "maps_hillshade_fragment") { _ in }),
                Arm(label: "hillshade_rop", pipeline: try pipeline(gpu, "maps_hillshade_rop_fragment") { c in
                    c.isBlendingEnabled = true
                    c.sourceRGBBlendFactor = .zero
                    c.destinationRGBBlendFactor = .sourceColor
                    c.sourceAlphaBlendFactor = .one
                    c.destinationAlphaBlendFactor = .zero
                }),
                Arm(label: "glow_fetch", pipeline: try pipeline(gpu, "maps_glow_fragment") { _ in }),
                Arm(label: "glow_rop", pipeline: try pipeline(gpu, "maps_glow_rop_fragment") { c in
                    c.writeMask = .alpha
                    c.isBlendingEnabled = true
                    c.alphaBlendOperation = .min
                }),
            ]
        } catch {
            return .fail("no pipeline: \(GPU.describe(error))", metrics)
        }
        let target = try gpu.texture(
            .bgra8Unorm_srgb, Self.width, Self.height, usage: [.renderTarget], samples: Self.samples
        )
        let resolved = try gpu.texture(.bgra8Unorm_srgb, Self.width, Self.height)
        let map = try gpu.texture(.rgba8Unorm, 256, 256, usage: [.shaderRead])
        try await gpu.upload(
            Patterns.bytes(Patterns.busy(width: 256, height: 256, channels: 4), .rgba8Unorm), to: map, bytesPerRow: 1024
        )
        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        guard let sampler = gpu.device.makeSamplerState(descriptor: samplerDescriptor) else {
            return .fail("no sampler", metrics)
        }
        var checks: [(String, Bool)] = []
        let pass = { (arm: Arm?, draws: Int, resolve: Bool) -> (MTLCommandBuffer) -> Void in
            { commands in
                let descriptor = MTLRenderPassDescriptor()
                descriptor.colorAttachments[0].texture = target
                descriptor.colorAttachments[0].resolveTexture = resolve ? resolved : nil
                descriptor.colorAttachments[0].loadAction = .clear
                descriptor.colorAttachments[0].clearColor = Self.clear
                descriptor.colorAttachments[0].storeAction = resolve ? .multisampleResolve : .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
                if let arm {
                    encoder.setRenderPipelineState(arm.pipeline)
                    var style = Style()
                    encoder.setFragmentBytes(&style, length: MemoryLayout<Style>.stride, index: 0)
                    encoder.setFragmentTexture(map, index: 0)
                    encoder.setFragmentSamplerState(sampler, index: 0)
                    for _ in 0 ..< draws { encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3) }
                }
                encoder.endEncoding()
            }
        }
        let median = { (encode: (MTLCommandBuffer) -> Void) async throws -> (Double, Set<String>) in
            var times: [Double] = []
            var paths = Set<String>()
            for _ in 0 ..< Self.runs {
                let timing = try await gpu.run(encode)
                checks.append(("GPU stamps", timing.gpuTimed))
                times.append(timing.gpuMilliseconds)
                paths.insert(timing.path)
            }
            return (Stats.percentile(times, 0.5), paths)
        }
        let (bare, barePaths) = try await median(pass(nil, 0, false))
        let (bareResolved, _) = try await median(pass(nil, 0, true))
        metrics.set("resolve_us", max(0, bareResolved - bare) * 1000, digits: 1)
        metrics.set("bare_us", bare * 1000, digits: 1)
        var images: [String: [UInt8]] = [:]
        var perDraw: [String: Double] = [:]
        for arm in arms {
            _ = try await gpu.run(tolerateError: true, pass(arm, 1, true))
            let before = GM20BFacade.counters(of: gpu.device)
            let (ms, paths) = try await median(pass(arm, Self.draws, false))
            perDraw[arm.label] = max(0, ms - bare) * 1000 / Double(Self.draws)
            metrics.set("us_" + arm.label, perDraw[arm.label]!, digits: 1)
            if facade {
                metrics.set(
                    "fetch_" + arm.label,
                    GM20BFacade.fetchRoutes(before: before, after: GM20BFacade.counters(of: gpu.device))
                )
                checks.append(("\(arm.label) on the driver", paths.union(barePaths) == ["gm20b"]))
            }
            _ = try await gpu.run(pass(arm, Self.draws, true))
            images[arm.label] = try await gpu.read(resolved, bytesPerPixel: 4)
        }
        for (shape, bound) in [("hillshade", Self.bound), ("glow", 0)] {
            guard let fetched = images[shape + "_fetch"], let blended = images[shape + "_rop"] else { continue }
            let worst = zip(fetched, blended).map { abs(Int($0) - Int($1)) }.max() ?? 0
            metrics.set("worst_" + shape, worst)
            checks.append(("\(shape): the fetch within \(bound) codes of the ROP", worst <= bound))
        }
        if let fetch = perDraw["hillshade_fetch"], let rop = perDraw["hillshade_rop"], rop > 0 {
            metrics.set("hillshade_fetch_over_rop", fetch / rop, digits: 2)
        }
        let failed = Set(checks.filter { !$0.1 }.map(\.0)).sorted()
        if failed.isEmpty { return .pass(metrics, proof: gpu.proof) }
        return .fail(failed.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
