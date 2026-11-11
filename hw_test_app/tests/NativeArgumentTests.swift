import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Argument encoders of argument descriptors: the encoder is the device's own, its length and alignment and every
/// argument's place are Metal's (an M3 Max's and an M4's: a pointer, a texture, a sampler, a float and a float4 take
/// 0, 8, 16, 24 and 32 of 48 bytes aligned to 16), a buffer is written as its GPU address plus the offset, a texture and
/// a sampler as real resource IDs (nonzero, distinct, past every ID m2v hands out), and an array's elements at their own
/// places. A pass whose fragments read a colour through an argument buffer's pointer logs where it was drawn.
struct NativeArgumentEncoderTest: NativeObjectTest {
    let id = "metal.native.argument_encoder"
    let name = "Argument encoders are the device's own and write addresses and real resource IDs where Metal lays them"

    static func argument(_ type: MTLDataType, _ index: Int, count: Int = 0) -> MTLArgumentDescriptor {
        let argument = MTLArgumentDescriptor()
        argument.dataType = type
        argument.index = index
        argument.arrayLength = count
        return argument
    }

    static func word(_ buffer: MTLBuffer, at offset: Int) -> UInt64 {
        buffer.contents().loadUnaligned(fromByteOffset: offset, as: UInt64.self)
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        guard
            let encoder = device.makeArgumentEncoder(arguments: [
                Self.argument(.pointer, 0), Self.argument(.texture, 1), Self.argument(.sampler, 2),
                Self.argument(.float, 3), Self.argument(.float4, 4),
            ]),
            let arrays = device.makeArgumentEncoder(arguments: [
                Self.argument(.texture, 0, count: 3), Self.argument(.pointer, 3, count: 2),
                Self.argument(.float, 5, count: 4), Self.argument(.sampler, 9, count: 2),
            ]),
            let sampler = device.makeSamplerState(descriptor: MTLSamplerDescriptor())
        else { throw TestFailure("no argument encoder or sampler") }
        let textures = try (0..<3).map { _ in try gpu.texture(.rgba8Unorm, 4, 4) }
        var work = NativeWork(objects: [encoder, arrays, sampler, textures[0]])
        metrics.set(
            "encoded", "\(encoder.encodedLength)/\(encoder.alignment),\(arrays.encodedLength)/\(arrays.alignment)")
        if encoder.encodedLength != 48 || encoder.alignment != 16 || arrays.encodedLength != 72 || arrays.alignment != 8
        {
            work.failures.append("an encoded length or alignment is not Metal's")
        }
        let arguments = try gpu.buffer(256), target = try gpu.buffer(1024)
        encoder.setArgumentBuffer(arguments, offset: 64)
        encoder.setBuffer(target, offset: 32, index: 0)
        encoder.setTexture(textures[0], index: 1)
        encoder.setSamplerState(sampler, index: 2)
        let texture = textures[0].gpuResourceID._impl, samplerID = sampler.gpuResourceID._impl
        metrics.set("texture_id", String(texture, radix: 16))
        metrics.set("sampler_id", String(samplerID, radix: 16))
        if Self.word(arguments, at: 64) != target.gpuAddress + 32 {
            work.failures.append("a buffer is not its address")
        }
        if Self.word(arguments, at: 72) != texture || texture < 1 << 32 {
            work.failures.append("a texture is not its real resource ID")
        }
        if Self.word(arguments, at: 80) != samplerID || samplerID < 1 << 32 {
            work.failures.append("a sampler is not its real resource ID")
        }
        if encoder.constantData(at: 4) != arguments.contents() + 64 + 32 {
            work.failures.append("a constant is not at its place")
        }
        if encoder.makeArgumentEncoderForBuffer(atIndex: 0) != nil {
            work.failures.append("a descriptor layout answered a nested encoder")
        }
        let ids = Set(textures.map { $0.gpuResourceID._impl })
        if ids.count != textures.count || ids.contains(0) { work.failures.append("textures share a resource ID") }
        let arrayBuffer = try gpu.buffer(256)
        arrays.setArgumentBuffer(arrayBuffer, offset: 0)
        arrays.setTextures(textures, range: 0..<3)
        arrays.setSamplerStates([sampler, sampler], range: 9..<11)
        let placed = [0, 8, 16, 56, 64].map { Self.word(arrayBuffer, at: $0) }
        if placed != textures.map({ $0.gpuResourceID._impl }) + [samplerID, samplerID] {
            work.failures.append("an array's elements are not at their places")
        }
        do {
            metrics.set("draw_path", try await drawThroughArguments(gpu))
        } catch {
            metrics.set("draw_path", "error")
            metrics.set("draw_error", "\(error)".replacingOccurrences(of: " ", with: "_"))
        }
        return work
    }

    /// Where a pass that reads its colour through an argument buffer's pointer went, and whether it drew that colour
    func drawThroughArguments(_ gpu: GPU) async throws -> String {
        guard let encoder = gpu.device.makeArgumentEncoder(arguments: [Self.argument(.pointer, 0)]) else {
            return "no_encoder"
        }
        let colour = try gpu.buffer([SIMD4<Float>(1, 0, 1, 1)]), arguments = try gpu.buffer(16)
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setBuffer(colour, offset: 0, index: 0)
        let pipeline = try gpu.render(fragment: "argument_colour_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 8, 8)
        let drawn = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            let render = commands.makeRenderCommandEncoder(descriptor: pass)!
            render.setRenderPipelineState(pipeline)
            render.useResource(colour, usage: .read, stages: .fragment)
            var corners = GPU.fullScreen
            render.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            render.setFragmentBuffer(arguments, offset: 0, index: 0)
            render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            render.endEncoding()
        }
        let pixel = Array(try await gpu.read(target, bytesPerPixel: 4).pixel(4, 4, width: 8))
        return "\(drawn.path)/\(pixel == [255, 0, 255, 255] ? "exact" : "wrong")"
    }
}
