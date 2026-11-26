import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.blend_kill's arms where the ROP's zero-source kill (drivers/TegraGM20BMetal/blend_kill.h) could reach past
/// the colour it drops:
/// - `late_depth`, `late_discard`: zero-source draws of a program that writes depth, or that can discard, with depth
///   writes on, so the depth test runs after the shader. Metal writes depth 0.5 at every pixel over the clear's 1 and
///   keeps the colour (`late_<...>_wrong=` depth texels other than 0.5, `late_<...>_colour_changed_bytes=`,
///   `late_<...>_kill_binds=` binds the facade made with the kill on).
/// - `visibility`: zero-source draws counted into a visibility result: Metal counts every sample of every draw
///   (`visibility_counted=` of `visibility_wanted=`).
/// - `zero_clear`: a pass that binds a kill state, then in the same command buffer a pass that clears another target to
///   (0, 0, 0, 0) and draws an opaque quad over its left half: the right half is 0 (`zero_clear_wrong=` texels off the
///   clear or the quad), whatever the kill the first pass left bound.
extension BlendKillTest {
    static let reachSide = 128

    static func killBinds(_ gpu: GPU) -> Int {
        GM20BFacade.counters(of: gpu.device)["blend_kill_binds"] ?? 0
    }

    static func zeroDraws(_ encoder: MTLRenderCommandEncoder, count: Int) {
        var corners = GPU.fullScreen
        var value = SIMD4<Float>.zero
        var depth: Float = 0.5
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        encoder.setFragmentBytes(&value, length: 16, index: 0)
        encoder.setFragmentBytes(&depth, length: 4, index: 1)
        for _ in 0 ..< count { encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
    }

    static func clearedColour(_ side: Int) -> [UInt8] {
        [UInt8]((0 ..< side * side).flatMap { _ in [2, 1, 0, 3].map { UInt8((zetaClear[$0] * 255).rounded()) } })
    }

    static func lateArm(_ gpu: GPU, fragment: String) async throws -> (
        wrong: Int, colourChanged: Int, binds: Int, path: String
    ) {
        let side = reachSide
        let state = try gpu.render(
            fragment: fragment, format: .bgra8Unorm, depth: .depth32Float, configure: sourceOver
        )
        let zetaState = MTLDepthStencilDescriptor()
        zetaState.depthCompareFunction = .always
        zetaState.isDepthWriteEnabled = true
        guard let zetaTest = gpu.device.makeDepthStencilState(descriptor: zetaState) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        let colour = try gpu.texture(.bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead])
        let depth = try gpu.texture(.depth32Float, side, side, usage: [.renderTarget])
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(
            red: zetaClear[0], green: zetaClear[1], blue: zetaClear[2], alpha: zetaClear[3]
        )
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 1
        pass.depthAttachment.storeAction = .store
        let binds = killBinds(gpu)
        let drawn = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setDepthStencilState(zetaTest)
            zeroDraws(encoder, count: 4)
            encoder.endEncoding()
        }
        let texels = try await gpu.read(depth, bytesPerPixel: 4)
        var wrong = 0
        texels.withUnsafeBytes { raw in
            for value in raw.bindMemory(to: Float.self) where value != 0.5 { wrong += 1 }
        }
        let bytes = try await gpu.read(colour, bytesPerPixel: 4)
        let changed = zip(bytes, clearedColour(side)).filter { $0 != $1 }.count
        return (wrong, changed, killBinds(gpu) - binds, drawn.path)
    }

    static func visibilityArm(_ gpu: GPU) async throws -> (counted: UInt64, wanted: UInt64, path: String) {
        let side = reachSide
        let draws = 4
        let state = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, configure: sourceOver)
        let colour = try gpu.texture(.bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead])
        let results = try gpu.buffer(16)
        memset(results.contents(), 0, 16)
        let pass = Self.pass(colour, load: .clear)
        pass.visibilityResultBuffer = results
        let drawn = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setVisibilityResultMode(.counting, offset: 0)
            zeroDraws(encoder, count: draws)
            encoder.setVisibilityResultMode(.disabled, offset: 0)
            encoder.endEncoding()
        }
        let counted = results.contents().load(as: UInt64.self)
        return (counted, UInt64(draws * side * side), drawn.path)
    }

    static func zeroClearArm(_ gpu: GPU) async throws -> (wrong: Int, binds: Int, paths: [String]) {
        let side = reachSide
        let kill = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, configure: sourceOver)
        let opaque = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm)
        let first = try gpu.texture(.bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead])
        let cleared = try gpu.texture(.bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead])
        let filled = [UInt8](repeating: 0x80, count: side * side * 4)
        try await gpu.upload(filled, to: cleared, bytesPerRow: side * 4)
        let clear = MTLRenderPassDescriptor()
        clear.colorAttachments[0].texture = cleared
        clear.colorAttachments[0].loadAction = .clear
        clear.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        clear.colorAttachments[0].storeAction = .store
        let binds = killBinds(gpu)
        let drawn = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass(first, load: .clear))!
            encoder.setRenderPipelineState(kill)
            zeroDraws(encoder, count: 2)
            encoder.endEncoding()
            let second = commands.makeRenderCommandEncoder(descriptor: clear)!
            second.setRenderPipelineState(opaque)
            GPU.draw(
                second, corners: GPU.pixelQuad(0, 0, side / 2, side, width: side, height: side, z: 0.5),
                color: SIMD4(1, 0, 0, 1)
            )
            second.endEncoding()
        }
        let bytes = try await gpu.read(cleared, bytesPerPixel: 4)
        var wrong = 0
        for y in 0 ..< side {
            for x in 0 ..< side {
                let want: [UInt8] = x < side / 2 ? [0, 0, 255, 255] : [0, 0, 0, 0]
                let at = (y * side + x) * 4
                if Array(bytes[at ..< at + 4]) != want { wrong += 1 }
            }
        }
        return (wrong, killBinds(gpu) - binds, [drawn.path])
    }

    static func reachArms(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String], _ paths: inout [String])
        async throws
    {
        for (label, fragment) in [("late_depth", "blend_kill_depth_fragment"), ("late_discard", "blend_kill_discard_fragment")] {
            let arm = try await lateArm(gpu, fragment: fragment)
            metrics.set("\(label)_wrong", arm.wrong)
            metrics.set("\(label)_colour_changed_bytes", arm.colourChanged)
            metrics.set("\(label)_kill_binds", arm.binds)
            paths.append(arm.path)
            if arm.wrong != 0 { wrong.append("\(label)_wrong") }
            if arm.colourChanged != 0 { wrong.append("\(label)_colour_changed_bytes") }
        }
        let visibility = try await visibilityArm(gpu)
        metrics.set("visibility_counted", Int(visibility.counted))
        metrics.set("visibility_wanted", Int(visibility.wanted))
        paths.append(visibility.path)
        if visibility.counted != visibility.wanted { wrong.append("visibility_counted") }
        let zero = try await zeroClearArm(gpu)
        metrics.set("zero_clear_wrong", zero.wrong)
        metrics.set("zero_clear_kill_binds", zero.binds)
        paths.append(contentsOf: zero.paths)
        if zero.wrong != 0 { wrong.append("zero_clear_wrong") }
    }
}
