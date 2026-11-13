import CoreGraphics
import CoreImage
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal
import SwiftUI
import SwitchTexturePattern

/// The home screen's Maps widget on boot 21 showed a 260x260 BGRA snapshot with the GOB signature: block-linear
/// texels read as rows. Its render ran on WidgetRenderer's own raw device (states with a 260x260 target) and no IOSurface
/// of that size was ever block-linear, so the rows came back through one of the raw path's own readbacks. This draws an
/// opaque pattern whose every byte of a GOB differs into a 260x260 BGRA target with the 3D engine, then reads it back by
/// each route raw offers (heap textures among them, as RenderBox places its own), and by SwiftUI's ImageRenderer
/// (RenderBox, as WidgetRenderer draws): `<route>_wrong` (bytes off by more than the route's tolerance) and
/// `<route>_gob` (its first row is the pattern laid out in GOBs, whatever pitch the misread took). `surface_bare` reads
/// a block-linear surface with no lock, which only a lock can show as rows (TegraGM20BViews.cpp): reported with the
/// surface's layout, not judged.
struct RawReadbackRoutesTest: HWTest {
    let id = "gpu.raw.readback_routes"
    let name = "A 260x260 BGRA render reads back as rows by every readback route"
    let subsystem = Subsystem.metal

    static let side = 260
    static let row = side * 4

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else { return .skip("the default device is not the raw facade") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        func judge(_ route: String, _ got: [UInt8]?, judged: Bool = true, tolerance: Int = 0) {
            guard let got else {
                checks.append(("\(route): read back", false))
                return
            }
            let wrong = Self.wrong(got, tolerance: tolerance)
            let gob = Self.readAsGobs(got, tolerance: tolerance)
            metrics.set("\(route)_wrong", wrong)
            metrics.set("\(route)_gob", gob ? 1 : 0)
            if judged {
                checks.append(("\(route): rows (\(wrong) bytes differ, gob \(gob))", wrong == 0 && !gob))
            }
        }
        let source = try await Self.source(gpu)
        let pipeline = try gpu.render(fragment: "readback_copy_fragment", format: .bgra8Unorm)
        let draw = { (target: MTLTexture) in try await Self.draw(gpu, pipeline, source, into: target) }

        let privateTarget = try gpu.texture(.bgra8Unorm, Self.side, Self.side, storage: .private)
        try await draw(privateTarget)
        judge("private_blit", try await gpu.read(privateTarget, bytesPerPixel: 4))

        let sharedTarget = try gpu.texture(.bgra8Unorm, Self.side, Self.side, storage: .shared)
        try await draw(sharedTarget)
        judge("shared_getbytes", Self.getBytes(sharedTarget))
        judge("shared_blit", try await gpu.read(sharedTarget, bytesPerPixel: 4))
        judge("ci_texture", Self.ciRows(CIImage(mtlTexture: sharedTarget)?.oriented(.downMirrored), gpu.device))

        for storage in [MTLStorageMode.private, .shared] {
            let label = storage == .private ? "heap_private" : "heap_shared"
            let target = try Self.heapTexture(gpu, storage)
            try await draw(target)
            judge("\(label)_blit", try await gpu.read(target, bytesPerPixel: 4))
            if storage == .shared { judge("\(label)_getbytes", Self.getBytes(target)) }
        }

        let (bufferTexture, backing, pitch) = try Self.bufferTexture(gpu)
        try await draw(bufferTexture)
        judge("buffer_texture", Self.rows(backing.contents(), pitch: pitch))

        let surface = try Self.surfaceTexture(gpu)
        try await draw(surface.texture)
        if let connection = GPUConnection() {
            let (kr, layout) = connection.surfaceLayout(surface.surface)
            metrics.set("surface_layout", kr == KERN_SUCCESS ? String(layout) : GPUConnection.hex(kr))
        }
        surface.surface.lock(options: .readOnly, seed: nil)
        let locked = Self.rows(surface.surface.baseAddress, pitch: surface.surface.bytesPerRow)
        surface.surface.unlock(options: .readOnly, seed: nil)
        judge("surface_lock", locked)
        judge("surface_bare", Self.rows(surface.surface.baseAddress, pitch: surface.surface.bytesPerRow), judged: false)
        judge(
            "ci_surface",
            Self.ciRows(CIImage(ioSurface: unsafeBitCast(surface.surface, to: IOSurfaceRef.self)), gpu.device))

        judge("swiftui", await Self.swiftUIRows(), tolerance: 2)
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func byte(_ x: Int, _ y: Int, _ c: Int) -> UInt8 {
        c == 3 ? 255 : switch_texpeer_surface_byte(UInt32(x), UInt32(y), UInt32(c))
    }

    static func pattern() -> [UInt8] {
        var out = [UInt8](repeating: 0, count: row * side)
        for y in 0..<side { for x in 0..<side { for c in 0..<4 { out[y * row + x * 4 + c] = byte(x, y, c) } } }
        return out
    }

    /// The pattern in a texture the draws read, put there by the blit engine from a buffer
    static func source(_ gpu: GPU) async throws -> MTLTexture {
        let texture = try gpu.texture(.bgra8Unorm, side, side, usage: [.shaderRead], storage: .private)
        let from = try gpu.buffer(pattern())
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: from, sourceOffset: 0, sourceBytesPerRow: row, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: side, height: side, depth: 1), to: texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
        }
        return texture
    }

    static func draw(
        _ gpu: GPU, _ pipeline: MTLRenderPipelineState, _ source: MTLTexture, into target: MTLTexture
    )
        async throws
    {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentTexture(source, index: 0)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
    }

    static func getBytes(_ texture: MTLTexture) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: row * side)
        out.withUnsafeMutableBytes {
            texture.getBytes(
                $0.baseAddress!, bytesPerRow: row, from: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0)
        }
        return out
    }

    /// A target placed in a heap of `storage` (RenderBox's ResourceHeap places its textures so)
    static func heapTexture(_ gpu: GPU, _ storage: MTLStorageMode) throws -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = storage
        let h = MTLHeapDescriptor()
        h.storageMode = storage
        h.size = gpu.device.heapTextureSizeAndAlign(descriptor: d).size
        guard let heap = gpu.device.makeHeap(descriptor: h), let texture = heap.makeTexture(descriptor: d) else {
            throw TestFailure("no \(storage == .private ? "private" : "shared") heap texture")
        }
        return texture
    }

    /// A linear texture over a shared buffer, whose bytes a CGImage can wrap with no copy
    static func bufferTexture(_ gpu: GPU) throws -> (MTLTexture, MTLBuffer, Int) {
        let alignment = max(1, gpu.device.minimumLinearTextureAlignment(for: .bgra8Unorm))
        let pitch = (row + alignment - 1) / alignment * alignment
        let backing = try gpu.buffer(pitch * side)
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .shared
        guard let texture = backing.makeTexture(descriptor: d, offset: 0, bytesPerRow: pitch) else {
            throw TestFailure("no texture over a \(pitch * side)-byte buffer")
        }
        return (texture, backing, pitch)
    }

    static func surfaceTexture(_ gpu: GPU) throws -> (surface: IOSurface, texture: MTLTexture) {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: side, .height: side, .bytesPerElement: 4, .pixelFormat: UInt32(0x4247_5241),
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface not made") }
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: d, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("no texture over the IOSurface") }
        return (surface, texture)
    }

    static func rows(_ base: UnsafeRawPointer, pitch: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: row * side)
        out.withUnsafeMutableBytes {
            for y in 0..<side {
                $0.baseAddress!.advanced(by: y * row).copyMemory(from: base + y * pitch, byteCount: row)
            }
        }
        return out
    }

    /// A CGImage of `image` by Core Image on the raw device, drawn into BGRA rows (opaque: alpha 255)
    static func ciRows(_ image: CIImage?, _ device: MTLDevice) -> [UInt8]? {
        guard let image else { return nil }
        let context = CIContext(
            mtlDevice: device, options: [.workingColorSpace: NSNull(), .outputColorSpace: NSNull()])
        return context.createCGImage(image, from: CGRect(x: 0, y: 0, width: side, height: side)).map(cgRows)
    }

    /// SwiftUI's ImageRenderer (RenderBox, as a widget's snapshot is made) drawing the pattern at one point a pixel
    static func swiftUIRows() async -> [UInt8]? {
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        var bytes = pattern()
        guard
            let image = bytes.withUnsafeMutableBytes({
                CGContext(
                    data: $0.baseAddress, width: side, height: side, bitsPerComponent: 8, bytesPerRow: row,
                    space: space, bitmapInfo: bgraInfo)?.makeImage()
            })
        else { return nil }
        return await MainActor.run {
            let renderer = ImageRenderer(
                content: Image(decorative: image, scale: 1).interpolation(.none).frame(
                    width: CGFloat(side), height: CGFloat(side)))
            renderer.scale = 1
            renderer.isOpaque = true
            return renderer.cgImage.map(cgRows)
        }
    }

    static let bgraInfo = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue

    static func cgRows(_ image: CGImage) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: row * side)
        out.withUnsafeMutableBytes {
            let context = CGContext(
                data: $0.baseAddress, width: side, height: side, bitsPerComponent: 8, bytesPerRow: row,
                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: bgraInfo)
            context?.setBlendMode(.copy)
            context?.draw(image, in: CGRect(x: 0, y: 0, width: side, height: side))
        }
        return out
    }

    /// Colour bytes (the alpha byte is the route's own) further than `tolerance` from the pattern's
    static func wrong(_ got: [UInt8], tolerance: Int) -> Int {
        var wrong = 0
        for y in 0..<side {
            for x in 0..<side {
                for c in 0..<3 where abs(Int(got[y * row + x * 4 + c]) - Int(byte(x, y, c))) > tolerance { wrong += 1 }
            }
        }
        return wrong
    }

    /// Whether the first row is the pattern laid out block-linear at NIL's block height: its bytes are the image's
    /// first bytes, whatever pitch a misread used
    static func readAsGobs(_ got: [UInt8], tolerance: Int) -> Bool {
        let yLog2 = SurfaceCrossProcessTest.blockHeightLog2(side)
        let rowBytes = (row + 63) & ~63
        var tiled = [UInt8](repeating: 0, count: rowBytes * side + 64 * 8 * 16)
        for y in 0..<side {
            for xB in 0..<row {
                let at = SurfaceCrossProcessTest.gobOffset(rowBytes, yLog2, xB, y)
                if at < tiled.count { tiled[at] = byte(xB / 4, y, xB % 4) }
            }
        }
        for xB in 0..<row where xB % 4 < 3 && abs(Int(got[xB]) - Int(tiled[xB])) > tolerance { return false }
        return true
    }
}
