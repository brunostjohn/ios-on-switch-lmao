import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// RenderBox's icon and widget quads on the raw path (boot 21 iconservicesagent: enc.geometry a=4 b=0x100000004 for
/// filter_color and uber, then the command buffer failed): a state that fetches the colour it writes, drawn as an indexed
/// strip of four over a private target on compressed memory, its corners set as vertex bytes before the encoder's first
/// draw. Its barrier form cannot order an indexed strip's triangles, so the target moves to plain memory and the
/// storage form draws it inside the pixel interlock. FixedBlendRule.metal rule_fetch_fragment blends source-over its
/// destination four times; a target that never compresses (ShaderWrite) is the reference. PASS: both raw, bytes alike,
/// no compressed fallback counted, and (debug builds, which answer a texture's facts) the target compressed before.
struct StripOverCompressedTest: HWTest {
    let id = "gpu.raw.strip_over_compressed"
    let name = "A fetching indexed strip over a compressed private target takes the storage form on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")
    // A shape no earlier test renders: a storage form that needed one plain makes every later target of it plain
    static let width = 232, height = 88

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let facade = gpu.device as? NSObject
        let state = try gpu.render(fragment: "rule_fetch_fragment", format: .bgra8Unorm)
        let indices: [UInt16] = [0, 1, 2, 3]
        guard
            let strip = gpu.device.makeBuffer(
                bytes: indices, length: MemoryLayout<UInt16>.stride * indices.count, options: .storageModeShared)
        else { throw TestFailure("no index buffer") }
        var target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        for _ in 0..<20 where comptags(facade, target) == 0 {
            try await Task.sleep(nanoseconds: 50_000_000)
            target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        }
        let reference = try gpu.texture(
            .bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead, .shaderWrite])
        var metrics = Metrics()
        metrics.set("compressed_before", comptags(facade, target))
        let before = RawFacade.counters(of: gpu.device)["fetch_fallback.compressed"] ?? 0
        var paths: [String] = []
        for texture in [target, reference] {
            paths.append(
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = texture
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
                    pass.colorAttachments[0].storeAction = .store
                    let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                    e.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    for layer in 0..<4 {
                        var which = SIMD2<UInt32>(UInt32(layer), 1)
                        e.setFragmentBytes(&which, length: 8, index: 0)
                        e.drawIndexedPrimitives(
                            type: .triangleStrip, indexCount: 4, indexType: .uint16, indexBuffer: strip,
                            indexBufferOffset: 0)
                    }
                    e.endEncoding()
                }.path)
        }
        metrics.set("paths", paths.joined(separator: ","))
        metrics.set("compressed_after", comptags(facade, target))
        metrics.set(
            "compressed_fallbacks", (RawFacade.counters(of: gpu.device)["fetch_fallback.compressed"] ?? 0) - before)
        let got = try await gpu.read(target, bytesPerPixel: 4)
        let want = try await gpu.read(reference, bytesPerPixel: 4)
        let differ = zip(got, want).filter { $0 != $1 }.count
        metrics.set("bytes_differ", differ)
        var wrong: [String] = []
        if paths.contains(where: { $0 != "raw" }) { wrong.append("paths") }
        if differ > 0 { wrong.append("bytes_differ") }
        if metrics["compressed_fallbacks"] != .integer(0) { wrong.append("compressed_fallback") }
        if facade?.responds(to: Self.factsSelector) == true && metrics["compressed_before"] != .integer(1) {
            wrong.append("target_not_compressed")
        }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }

    func comptags(_ facade: NSObject?, _ texture: MTLTexture) -> Int {
        guard let facade, facade.responds(to: Self.factsSelector) else { return 0 }
        let line = facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }
}
