import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Rule resolve.twod (drivers/TegraGM20BMetal/raw/runtime/raw_fetch_rungs.h twodResolveAcceptable): a 720x1280 BGRA8 4x
/// pass, every sample shaded apart, resolved into a private texture by the 2D engine (the rule's route) and by the pack's
/// resolve pass (switchRawProbeResolveDraw:, Metal's exact rule), each three times. PASS: both on the raw path, the 2D
/// route taken (resolve_twod moved) and every byte within one code of the resolve pass's. The line gives the bytes one
/// code apart, which channel they fall in, and each route's fastest GPU time for the pass and its resolve. 2x is run too.
struct ResolveTwodTest: HWTest {
    let id = "gpu.raw.resolve_twod"
    let name = "MSAA resolves by the 2D engine: within one code of the resolve pass, and the GPU time each way"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let selector = NSSelectorFromString("switchRawProbeResolveDraw:")
    static let (width, height) = (720, 1280)

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.selector) else {
            return .skip("a release facade: no switch to send the reference through the resolve pass")
        }
        var metrics = Metrics()
        var wrong: [String] = []
        for samples in [4, 2] where gpu.device.supportsTextureSampleCount(samples) {
            let state = try gpu.render(fragment: "resolve_twod_fragment", format: .bgra8Unorm, samples: samples)
            let before = RawFacade.counters(of: gpu.device)["resolve_twod"] ?? 0
            let (twod, twodMs, twodPath) = try await resolved(gpu, state, samples: samples)
            let taken = (RawFacade.counters(of: gpu.device)["resolve_twod"] ?? 0) - before
            _ = facade.perform(Self.selector, with: NSNumber(value: true))
            defer { _ = facade.perform(Self.selector, with: NSNumber(value: false)) }
            let (pass, passMs, passPath) = try await resolved(gpu, state, samples: samples)
            var oneOff = [0, 0, 0, 0], beyond = 0
            for i in 0..<min(twod.count, pass.count) where twod[i] != pass[i] {
                if abs(Int(twod[i]) - Int(pass[i])) > 1 { beyond += 1 } else { oneOff[i % 4] += 1 }
            }
            let tag = "\(samples)x"
            metrics.set("\(tag)_beyond_one", beyond)
            metrics.set("\(tag)_one_off_bgra", oneOff.map(String.init).joined(separator: "/"))
            metrics.set("\(tag)_twod_gpu_ms", twodMs, digits: 3)
            metrics.set("\(tag)_pass_gpu_ms", passMs, digits: 3)
            metrics.set("\(tag)_twod_taken", taken)
            metrics.set("\(tag)_paths", "\(twodPath)/\(passPath)")
            if beyond > 0 { wrong.append("\(tag)_beyond_one") }
            if taken == 0 { wrong.append("\(tag)_not_taken") }
            if twodPath != "raw" || passPath != "raw" { wrong.append("\(tag)_paths") }
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// The resolve texture's bytes, the fastest of three GPU times of the pass and its resolve, and the path
    func resolved(_ gpu: GPU, _ state: MTLRenderPipelineState, samples: Int) async throws -> ([UInt8], Double, String) {
        let ms = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], samples: samples)
        let into = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        var best = Double.infinity
        var path = ""
        for _ in 0..<3 {
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = ms
                pass.colorAttachments[0].resolveTexture = into
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .multisampleResolve
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }
            best = min(best, timing.gpuMilliseconds)
            path = timing.path
        }
        return (try await gpu.read(into, bytesPerPixel: 4), best, path)
    }
}
