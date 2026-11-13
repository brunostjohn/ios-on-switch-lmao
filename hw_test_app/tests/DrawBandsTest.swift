import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// An app draw longer than a band (TegraGM20BSchedPolicy.h TGM_APP_BAND_NS) drawn in scissor bands
/// (raw/runtime/raw_draw_bands.h). Per case (blended, framebuffer fetch, 4x MSAA resolved) the same two full-screen
/// draws of a ~15 ms fragment program are made banded and with the facade's band budget at 0: the bytes must be
/// identical and only the first arm banded (banded_draws, bands). While a layer animates on screen, so the compositor
/// kicks every frame, banded draws run back to back: the kernel's preemption latencies sampled for this process
/// (debug kernels, TegraGM20BPathsABI.h `preempts`, `preempt_max_us`) must stay within the budget plus a context switch,
/// and the unbanded arm shows what a whole draw costs the compositor. us_per_band is the GPU time a band took.
struct DrawBandsTest: HWTest {
    let id = "gpu.sched.long_draw_bands"
    let name = "Long app draws in scissor bands: compositor wait and pixel identity"
    let subsystem = Subsystem.metal

    static let width = 1280
    static let height = 720
    static let draws = 2
    static let budgetUs = 1000
    static let waitBoundUs = 2000
    static let kickSeconds = 1.5

    struct Case {
        let name: String
        let fragment: String
        let samples: Int
        let blend: Bool
    }

    static let cases = [
        Case(name: "blend", fragment: "bands_blend_fragment", samples: 1, blend: true),
        Case(name: "fetch", fragment: "bands_fetch_fragment", samples: 1, blend: false),
        Case(name: "msaa", fragment: "bands_blend_fragment", samples: 4, blend: true),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        guard let production = Self.budget(gpu.device, Self.budgetUs * 1000) else {
            return .skip("the facade has no band budget (a release build)")
        }
        defer { Self.budget(gpu.device, production) }
        var metrics = Metrics()
        var failures: [String] = []
        for test in Self.cases {
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try gpu.render(
                    vertex: "bands_vertex", fragment: test.fragment, format: .bgra8Unorm, samples: test.samples
                ) { descriptor in
                    let target = descriptor.colorAttachments[0]!
                    target.isBlendingEnabled = test.blend
                    target.sourceRGBBlendFactor = .sourceAlpha
                    target.destinationRGBBlendFactor = .oneMinusSourceAlpha
                }
            } catch {
                return .skip("no \(test.name) pipeline: \(GPU.describe(error))", metrics)
            }
            let banded = try await draw(gpu, pipeline, test, metrics: &metrics, arm: "")
            _ = Self.budget(gpu.device, 0)
            let whole = try await draw(gpu, pipeline, test, metrics: &metrics, arm: "_whole")
            _ = Self.budget(gpu.device, Self.budgetUs * 1000)
            let identical = banded.bytes == whole.bytes
            metrics.set("identical_" + test.name, identical)
            if !identical { failures.append("\(test.name): banded pixels differ from the whole draw's") }
            if banded.bands < 2 * Self.draws { failures.append("\(test.name): \(banded.bands) bands for \(Self.draws) draws") }
            if whole.bands != 0 { failures.append("\(test.name): \(whole.bands) bands with the budget at 0") }
        }
        let blend = Self.cases[0]
        let pipeline = try gpu.render(vertex: "bands_vertex", fragment: blend.fragment, format: .bgra8Unorm) { _ in }
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let layer = await context.onStage { stage -> CALayer in
            let spinner = CALayer()
            spinner.frame = CGRect(x: 20, y: 20, width: 80, height: 80)
            spinner.backgroundColor = UIColor.systemTeal.cgColor
            let spin = CABasicAnimation(keyPath: "transform.rotation.z")
            spin.fromValue = 0
            spin.toValue = 2 * Double.pi
            spin.duration = 1
            spin.repeatCount = .infinity
            spinner.add(spin, forKey: "spin")
            stage.layer.addSublayer(spinner)
            return spinner
        }
        let connection = GPUConnection()
        for (arm, budgetNs) in [("", Self.budgetUs * 1000), ("_whole", 0)] {
            _ = Self.budget(gpu.device, budgetNs)
            _ = GPUPaths.read(connection)
            let until = CFAbsoluteTimeGetCurrent() + Self.kickSeconds
            while CFAbsoluteTimeGetCurrent() < until {
                try await gpu.run { commands in Self.encode(commands, pipeline, target, samples: 1, resolve: nil) }
            }
            guard case .counts(let after) = GPUPaths.read(connection) else {
                failures.append("the kernel's paths line is unreadable: no preemption latencies")
                break
            }
            let samples = after.counts["preempts"] ?? 0
            let maxUs = after.counts["preempt_max_us"] ?? 0
            metrics.set("preempts" + arm, samples)
            metrics.set("preempt_max_us" + arm, maxUs)
            if arm.isEmpty {
                if samples == 0 { failures.append("no compositor kick met a banded draw in \(Self.kickSeconds) s") }
                if maxUs > Self.waitBoundUs { failures.append("the compositor waited \(maxUs) us behind a banded draw") }
            }
        }
        await MainActor.run { layer.removeFromSuperlayer() }
        await context.clearStage()
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }

    struct Drawn {
        let bytes: [UInt8]
        let bands: Int
    }

    func draw(
        _ gpu: GPU, _ pipeline: MTLRenderPipelineState, _ test: Case, metrics: inout Metrics, arm: String
    ) async throws -> Drawn {
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let msaa = test.samples > 1 ? try gpu.texture(.bgra8Unorm, Self.width, Self.height, samples: test.samples) : nil
        let before = RawFacade.counters(of: gpu.device)
        let timing = try await gpu.run { commands in
            Self.encode(commands, pipeline, msaa ?? target, samples: test.samples, resolve: msaa == nil ? nil : target)
        }
        let after = RawFacade.counters(of: gpu.device)
        let drawsBanded = (after["banded_draws"] ?? 0) - (before["banded_draws"] ?? 0)
        let bands = (after["bands"] ?? 0) - (before["bands"] ?? 0)
        let key = test.name + arm
        metrics.set("path_" + key, timing.path)
        metrics.set("banded_draws_" + key, drawsBanded)
        metrics.set("bands_" + key, bands)
        if timing.gpuTimed {
            metrics.set("us_per_draw_" + key, timing.gpuMilliseconds * 1000 / Double(Self.draws), digits: 0)
            if bands > 0 { metrics.set("us_per_band_" + key, timing.gpuMilliseconds * 1000 / Double(bands), digits: 0) }
        }
        return Drawn(bytes: try await gpu.read(target, bytesPerPixel: 4), bands: bands)
    }

    static func encode(
        _ commands: MTLCommandBuffer, _ pipeline: MTLRenderPipelineState, _ target: MTLTexture, samples: Int,
        resolve: MTLTexture?
    ) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
        pass.colorAttachments[0].resolveTexture = resolve
        pass.colorAttachments[0].storeAction = resolve == nil ? .store : .multisampleResolve
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        for _ in 0..<draws { encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3) }
        encoder.endEncoding()
    }

    /// Sets the facade's band budget (debug builds) and answers the one it replaced; nil off a debug facade.
    @discardableResult
    static func budget(_ device: MTLDevice, _ ns: Int) -> Int? {
        let selector = NSSelectorFromString("switchRawBandBudget:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return nil }
        let old = object.perform(selector, with: NSNumber(value: ns))?.takeUnretainedValue() as? String
        return old.flatMap { Int($0) }
    }
}
