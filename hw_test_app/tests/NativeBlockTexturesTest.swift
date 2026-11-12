import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Block-compressed textures of several levels or layers, and shared ones, in the device's own memory: NIL's image
/// (fx::blockChainLayout). A shared one's CPU writes (replaceRegion) are read back by the copy engine and by getBytes; a
/// private one's level and slice are written and read by the copy engine. ETC2 and EAC are GM20B's own texture formats
/// (NIL's MaxwellA rows), as ASTC is.
struct NativeBlockTexturesTest: NativeObjectTest {
    let id = "metal.native.block_textures"
    let name = "Block-compressed textures of levels and layers, shared or private, every block written and read back"

    struct Shape {
        let name: String
        let format: MTLPixelFormat
        let blockBytes: Int
        let type: MTLTextureType
        let width: Int, height: Int, layers: Int, levels: Int
        let storage: MTLStorageMode
    }

    static let shapes: [Shape] = [
        Shape(
            name: "astc_shared", format: .astc_4x4_ldr, blockBytes: 16, type: .type2D, width: 64, height: 40, layers: 1,
            levels: 5, storage: .shared),
        Shape(
            name: "etc2_shared_array", format: .etc2_rgb8, blockBytes: 8, type: .type2DArray, width: 32, height: 32,
            layers: 3, levels: 1, storage: .shared),
        Shape(
            name: "eac_private", format: .eac_rgba8, blockBytes: 16, type: .type2DArray, width: 32, height: 16,
            layers: 2, levels: 3, storage: .private),
        Shape(
            name: "astc_private_array", format: .astc_4x4_ldr, blockBytes: 16, type: .type2DArray, width: 64,
            height: 64, layers: 2, levels: 4, storage: .private),
    ]

    /// Every byte of level `level`, slice `slice`: distinct across the texture's images
    static func blocks(_ s: Shape, _ level: Int, _ slice: Int) -> (bytes: [UInt8], row: Int, rows: Int) {
        let across = (max(1, s.width >> level) + 3) / 4, down = (max(1, s.height >> level) + 3) / 4
        let row = across * s.blockBytes
        let bytes = (0..<(row * down)).map { UInt8(truncatingIfNeeded: $0 * 7 + level * 31 + slice * 61 + 1) }
        return (bytes, row, down)
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        for s in Self.shapes {
            let d = MTLTextureDescriptor()
            d.textureType = s.type
            d.pixelFormat = s.format
            d.width = s.width
            d.height = s.height
            d.arrayLength = s.layers
            d.mipmapLevelCount = s.levels
            d.usage = .shaderRead
            d.storageMode = s.storage
            guard let t = gpu.device.makeTexture(descriptor: d) else {
                work.failures.append("no \(s.name) texture")
                continue
            }
            work.objects.append(t)
            var wrong = 0
            for level in 0..<s.levels {
                for slice in 0..<s.layers {
                    let (bytes, row, rows) = Self.blocks(s, level, slice)
                    let size = MTLSize(width: max(1, s.width >> level), height: max(1, s.height >> level), depth: 1)
                    if s.storage == .shared {
                        bytes.withUnsafeBytes {
                            t.replace(
                                region: MTLRegion(origin: MTLOrigin(), size: size), mipmapLevel: level, slice: slice,
                                withBytes: $0.baseAddress!, bytesPerRow: row, bytesPerImage: 0)
                        }
                        var back = [UInt8](repeating: 0, count: bytes.count)
                        back.withUnsafeMutableBytes {
                            t.getBytes(
                                $0.baseAddress!, bytesPerRow: row, bytesPerImage: 0,
                                from: MTLRegion(origin: MTLOrigin(), size: size), mipmapLevel: level, slice: slice)
                        }
                        wrong += zip(back, bytes).filter { $0 != $1 }.count
                    } else {
                        let staging = try gpu.buffer(bytes)
                        let timing = try await gpu.run { commands in
                            let blit = commands.makeBlitCommandEncoder()!
                            blit.copy(
                                from: staging, sourceOffset: 0, sourceBytesPerRow: row, sourceBytesPerImage: 0,
                                sourceSize: size, to: t, destinationSlice: slice, destinationLevel: level,
                                destinationOrigin: MTLOrigin())
                            blit.endEncoding()
                        }
                        work.paths.append(timing.path)
                    }
                    let out = try gpu.buffer(row * rows)
                    let timing = try await gpu.run { commands in
                        let blit = commands.makeBlitCommandEncoder()!
                        blit.copy(
                            from: t, sourceSlice: slice, sourceLevel: level, sourceOrigin: MTLOrigin(),
                            sourceSize: size,
                            to: out, destinationOffset: 0, destinationBytesPerRow: row, destinationBytesPerImage: 0)
                        blit.endEncoding()
                    }
                    work.paths.append(timing.path)
                    let got = [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: row * rows))
                    wrong += zip(got, bytes).filter { $0 != $1 }.count
                }
            }
            metrics.set("wrong_\(s.name)", wrong)
            if wrong != 0 { work.failures.append("\(wrong) bytes of the \(s.name) texture") }
        }
        return work
    }
}
