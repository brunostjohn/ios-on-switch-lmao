import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// metal.native.argument_encoder's draws of functions whose bind offsets m2v places past their stage's push window, in the
/// stage's argument table the raw path fills (raw_spill_table.h): an argument buffer beside fourteen fragment buffers, or
/// beside thirteen vertex buffers, each bound off its base alignment so each offset's residual is its own
extension NativeArgumentEncoderTest {
    static let spillStride = 528

    /// Each slot's float4 at `slot * spillStride`, twice it 16 bytes past slot 30's, `elsewhere` everywhere else: a
    /// residual lost reads `elsewhere`
    static func spillValues(_ gpu: GPU, _ value: SIMD4<Float>, _ elsewhere: SIMD4<Float>) throws -> MTLBuffer {
        var values = [SIMD4<Float>](repeating: elsewhere, count: 32 * spillStride / 16)
        for slot in 1..<32 { values[slot * spillStride / 16] = value }
        values[30 * spillStride / 16 + 1] = value * 2
        return try gpu.buffer(values)
    }

    /// Where two draws of a spilling stage went, the second after slot 14 moved to a float4 twice the others (its table
    /// uploaded again, nothing else), and whether each read every buffer at its own offset: the fragment stage's
    /// argument_spill_fragment, or the vertex stage's spill_vertex (`vertex`)
    func drawSpillThroughArguments(_ gpu: GPU, vertex: Bool) async throws -> String {
        guard let encoder = gpu.device.makeArgumentEncoder(arguments: [Self.argument(.pointer, 0)]) else {
            return "no_encoder"
        }
        let colour = try gpu.buffer([SIMD4<Float>(0, 0, 1, 1)]), arguments = try gpu.buffer(16)
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setBuffer(colour, offset: 0, index: 0)
        let one = vertex ? SIMD4<Float>(0, 1.0 / 32, 0, 0) : SIMD4<Float>(1.0 / 32, 0, 0, 0)
        let values = try Self.spillValues(gpu, one, vertex ? SIMD4<Float>(0, 0, 0, -1) : SIMD4<Float>(0, 1, 0, 0))
        let pipeline = try gpu.render(
            vertex: vertex ? "spill_vertex" : "quad_vertex",
            fragment: vertex ? "tint_fragment" : "argument_spill_fragment", format: .rgba8Unorm)
        let slots = vertex ? 2...14 : 1...14
        let targets = try (0..<2).map { _ in try gpu.texture(.rgba8Unorm, 2, 1) }
        let drawn = try await gpu.run { commands in
            for (k, target) in targets.enumerated() {
                let render = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
                render.useResource(colour, usage: .read, stages: vertex ? .vertex : .fragment)
                render.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                render.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
                for slot in slots {
                    if vertex {
                        render.setVertexBuffer(values, offset: slot * Self.spillStride, index: slot)
                    } else {
                        render.setFragmentBuffer(values, offset: slot * Self.spillStride, index: slot)
                    }
                }
                if vertex {
                    render.setVertexBuffer(arguments, offset: 0, index: 1)
                } else {
                    render.setFragmentBuffer(arguments, offset: 0, index: 0)
                }
                render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                if k == 1 {
                    if vertex {
                        render.setVertexBufferOffset(30 * Self.spillStride + 16, index: 14)
                    } else {
                        render.setFragmentBufferOffset(30 * Self.spillStride + 16, index: 14)
                    }
                    render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                render.endEncoding()
            }
        }
        // the spilled channel: 13 or 14 thirty-seconds, then one more
        let first: UInt8 = vertex ? 104 : 112, second: UInt8 = vertex ? 112 : 120
        var exact = true
        for (k, sum) in [(0, first), (1, second)] {
            let pixels = try await gpu.read(targets[k], bytesPerPixel: 4)
            let want: [UInt8] = vertex ? [0, sum, 255, 255] : [sum, 0, 255, 255]
            exact = exact && (0..<2).allSatisfy { Array(pixels.pixel($0, 0, width: 2)) == want }
        }
        return "\(drawn.path)/\(exact ? "exact" : "wrong")"
    }
}
