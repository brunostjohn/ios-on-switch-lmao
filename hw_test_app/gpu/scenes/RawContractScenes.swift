import Foundation
import HWTestCore
import Metal

/// A 64x64 pattern whose neighbouring texels differ in every channel, uploaded to a private texture.
private func pattern(_ gpu: GPU, size: Int) async throws -> (MTLTexture, Raster) {
    var bytes = [UInt8](repeating: 0, count: size * size * 4)
    for y in 0..<size {
        for x in 0..<size {
            let at = (y * size + x) * 4
            bytes[at] = UInt8((x * 4) & 255)
            bytes[at + 1] = UInt8((y * 4) & 255)
            bytes[at + 2] = UInt8(((x + y) * 2) & 255)
            bytes[at + 3] = 255
        }
    }
    let texture = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
    try await gpu.upload(bytes, to: texture, bytesPerRow: size * 4)
    return (texture, Raster(unorm8: bytes, width: size, height: size, channels: 4))
}

/// Storage buffers bound off a multiple of 16: the vertex corners at 0x38e8 (iconservicesagent's RenderBox offset on boot
/// 13, which raw refused as enc.ubo) and the coordinates at 4 past one. Each texel is copied through the nearest
/// constexpr sampler, so the reference is the pattern itself.
struct StorageOffsetScene: GPUScene {
    let id = "gpu.raw.storage_offset"
    let name = "Storage buffers bound at offsets that are not multiples of 16"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let size = 64
        let (source, model) = try await pattern(gpu, size: size)
        let corners: [SIMD2<Float>] = [SIMD2(-1, -1), SIMD2(1, -1), SIMD2(-1, 1), SIMD2(1, 1)]
        let uvs: [Float] = [0, 1, 1, 1, 0, 0, 1, 0]
        let (cornerOffset, uvOffset) = (0x38e8, 0x2004)
        let cornerBuffer = try gpu.buffer(cornerOffset + 64)
        let uvBuffer = try gpu.buffer(uvOffset + 64)
        corners.withUnsafeBytes {
            cornerBuffer.contents().advanced(by: cornerOffset).copyMemory(from: $0.baseAddress!, byteCount: $0.count)
        }
        uvs.withUnsafeBytes {
            uvBuffer.contents().advanced(by: uvOffset).copyMemory(from: $0.baseAddress!, byteCount: $0.count)
        }
        let target = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(
            vertex: "device_textured_vertex", fragment: "nearest_fragment", format: .rgba8Unorm)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBuffer(cornerBuffer, offset: cornerOffset, index: 0)
            encoder.setVertexBuffer(uvBuffer, offset: uvOffset, index: 1)
            encoder.setFragmentTexture(source, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        result.metrics.set("path", timing.path)
        result.compare("copied", try await gpu.raster(target, .rgba8Unorm), model, tolerance: 1.01 / 255)
        return result
    }
}

/// A runtime sampler with normalizedCoordinates NO (RenderBox's and TVWidgetExtension's, which raw refused as call.sampler
/// on boot 13): nearest at texel centres in pixels copies the pattern, and linear half a texel on averages each texel
/// with its right neighbour (the last column clamps to the edge), as the normalized samplers' draws beside them do.
struct PixelSamplerScene: GPUScene {
    let id = "gpu.raw.pixel_sampler"
    let name = "Samplers with pixel coordinates beside normalized ones"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let size = 64
        let (source, model) = try await pattern(gpu, size: size)
        let pipeline = try gpu.render(vertex: "textured_vertex", fragment: "sampled_fragment", format: .rgba8Unorm)
        let averaged = Raster(width: size, height: size, channels: 4) { x, y, c in
            (model[x, y, c] + model[min(x + 1, size - 1), y, c]) / 2
        }
        let s = Float(size)
        let half = Float(0.5)
        let cases: [(String, Bool, MTLSamplerMinMagFilter, [SIMD2<Float>], Raster)] = [
            ("pixel nearest", false, .nearest, [SIMD2(0, s), SIMD2(s, s), SIMD2(0, 0), SIMD2(s, 0)], model),
            ("normalized nearest", true, .nearest, [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)], model),
            (
                "pixel linear", false, .linear,
                [SIMD2(half, s), SIMD2(s + half, s), SIMD2(half, 0), SIMD2(s + half, 0)], averaged
            ),
            (
                "normalized linear", true, .linear,
                [SIMD2(half / s, 1), SIMD2(1 + half / s, 1), SIMD2(half / s, 0), SIMD2(1 + half / s, 0)], averaged
            ),
        ]
        var paths: [String] = []
        for (label, normalized, filter, uvs, want) in cases {
            let descriptor = MTLSamplerDescriptor()
            descriptor.normalizedCoordinates = normalized
            descriptor.minFilter = filter
            descriptor.magFilter = filter
            descriptor.sAddressMode = .clampToEdge
            descriptor.tAddressMode = .clampToEdge
            guard let sampler = gpu.device.makeSamplerState(descriptor: descriptor) else {
                result.expect(false, "\(label): makeSamplerState returned nil")
                continue
            }
            let target = try gpu.texture(.rgba8Unorm, size, size)
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .dontCare
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                var uvs = uvs
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
                encoder.setFragmentTexture(source, index: 0)
                encoder.setFragmentSamplerState(sampler, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            paths.append(timing.path)
            result.compare(label, try await gpu.raster(target, .rgba8Unorm), want, tolerance: 1.51 / 255)
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        return result
    }
}
