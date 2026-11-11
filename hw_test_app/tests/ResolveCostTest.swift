import HWTestCore
import HWTestGPU
import Metal

/// What a 720x1280 multisampled resolve costs the GPU: a cleared pass stored with and without its resolve, the
/// difference the resolve pass. The lock screen's Celosia poster resolves a 4x BGRA8 memoryless target into its
/// drawable every frame, and that pass took 11.5-12.1 ms on device (12.7 ns a pixel, as MapKit's 4x resolves did)
/// where reading 14.7 MB and writing 3.7 MB takes under 1 ms. The variants split the cost between the unorm8 resolve
/// program (rgba8: a loop over the sample count) and the multisampled fetch (rgba16f: four reads in line), and
/// between memoryless and stored targets.
struct ResolveCostTest: HWTest {
    let id = "gpu.msaa.resolve_cost"
    let name = "A 720x1280 4x resolve costs the GPU what its bytes do"
    let subsystem = Subsystem.metal

    static let width = 720
    static let height = 1280
    static let runs = 6
    static let budgetMs = 2.0

    struct Variant {
        let label: String
        let format: MTLPixelFormat
        let samples: Int
        let storage: MTLStorageMode
    }

    static let variants = [
        Variant(label: "rgba8_4x_memoryless", format: .rgba8Unorm, samples: 4, storage: .memoryless),
        Variant(label: "rgba8_4x_private", format: .rgba8Unorm, samples: 4, storage: .private),
        Variant(label: "rgba8_2x_memoryless", format: .rgba8Unorm, samples: 2, storage: .memoryless),
        Variant(label: "rgba16f_4x_memoryless", format: .rgba16Float, samples: 4, storage: .memoryless),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard gpu.device.supportsTextureSampleCount(4) else { return .fail("4x MSAA unsupported") }
        var metrics = Metrics()
        var poster: Double?
        for variant in Self.variants {
            let resolved = try gpu.texture(variant.format, Self.width, Self.height)
            let target = try gpu.texture(
                variant.format, Self.width, Self.height, usage: [.renderTarget], samples: variant.samples,
                storage: variant.storage)
            let with = try await median(gpu, target, resolve: resolved)
            let without = try await median(gpu, target, resolve: nil)
            let resolveMs = max(0, with - without)
            metrics.set("\(variant.label)_pass_ms", with, digits: 3)
            metrics.set("\(variant.label)_bare_ms", without, digits: 3)
            metrics.set("\(variant.label)_ms", resolveMs, digits: 3)
            metrics.set(
                "\(variant.label)_ns_px", resolveMs * 1e6 / Double(Self.width * Self.height), digits: 2)
            if variant.label == "rgba8_4x_memoryless" { poster = resolveMs }
        }
        metrics.set("paths", gpu.paths.text)
        guard let poster, gpu.paths.paths.keys.contains("raw") else {
            return .skip("the resolves did not run on the raw path (\(gpu.paths.text))", metrics)
        }
        return .judged(
            [("the poster's 720x1280 4x resolve within \(Self.budgetMs) ms", poster <= Self.budgetMs)], metrics,
            proof: gpu.proof)
    }

    /// The median GPU time of a cleared pass over `target`, resolved into `resolve` or not stored.
    func median(_ gpu: GPU, _ target: MTLTexture, resolve: MTLTexture?) async throws -> Double {
        var times: [Double] = []
        for _ in 0..<Self.runs {
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].resolveTexture = resolve
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
                pass.colorAttachments[0].storeAction = resolve == nil ? .dontCare : .multisampleResolve
                commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
            }
            times.append(timing.gpuMilliseconds)
        }
        return times.sorted()[times.count / 2]
    }
}
