import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// metal.native.argument_encoder's compute kernel: an argument buffer the kernel's own encoder wrote, a colour behind its
/// pointer and a texture by its ID, read by one thread of argument_kernel
extension NativeArgumentEncoderTest {
    /// Where the dispatch went, and whether the kernel wrote the colour and the texel it reached
    func dispatchThroughArguments(_ gpu: GPU) async throws -> String {
        guard let function = gpu.library.makeFunction(name: "argument_kernel") else { return "no_function" }
        let encoder = function.makeArgumentEncoder(bufferIndex: 0)
        let colour = try gpu.buffer([SIMD4<Float>(1, 0, 1, 1)])
        let texture = try gpu.texture(.rgba8Unorm, 1, 1, usage: [.shaderRead], storage: .shared)
        let texel: [UInt8] = [0, 255, 0, 255]
        texture.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: texel, bytesPerRow: 4)
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16))
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setTexture(texture, index: 0)
        encoder.setBuffer(colour, offset: 0, index: 1)
        let pipeline = try gpu.compute("argument_kernel")
        let out = try gpu.buffer(32)
        let ran = try await gpu.run { commands in
            let compute = commands.makeComputeCommandEncoder()!
            compute.setComputePipelineState(pipeline)
            compute.useResource(colour, usage: .read)
            compute.useResource(texture, usage: .read)
            compute.setBuffer(arguments, offset: 0, index: 0)
            compute.setBuffer(out, offset: 0, index: 1)
            compute.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                         threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
            compute.endEncoding()
        }
        let words = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: 2)
        let exact = words[0] == SIMD4<Float>(1, 0, 1, 1) && words[1] == SIMD4<Float>(0, 1, 0, 1)
        return "\(ran.path)/\(exact ? "exact" : "wrong")"
    }
}
