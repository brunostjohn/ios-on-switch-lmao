import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Boot 21's close frames: Core Animation made a new 1280x720 IOSurface each frame and its first GPU import cleaned the
/// surface's 3600 KiB on the render thread (`clean 3600 KiB 1.6 ms`). The kernel's IOSurface prefault now cleans the
/// zero-filled pages and keeps them wired for that import (drivers/TegraGM20B/TegraGM20BSurfacePrefault.cpp), which
/// cleans nothing. Fresh surfaces made as Core Animation makes them, given the prefault's moment, each imported (a
/// texture over it), cleared whole by the GPU and read back under IOSurfaceLock: a dirty zero line written back over the
/// GPU's pixels would read black. The imports' times are the metrics (import_us_mean, import_us_max: under the 1.6 ms
/// clean when it is skipped); the debug kernel's `mem2` line counts the skipped cleans.
struct FreshSurfaceImportPathTest: GPUPathTest {
    let id = "gpu.path.fresh_surface_import"
    let name = "A fresh IOSurface's first import needs no CPU clean and the GPU's pixels read back whole"
    let counters: [String] = []
    static let width = 1280, height = 720, rowBytes = 1280 * 4, surfaces = 6

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: Self.width, .height: Self.height, .bytesPerElement: 4, .bytesPerRow: Self.rowBytes,
            .allocSize: Self.rowBytes * Self.height, .pixelFormat: 0x4247_5241,
        ]
        var surfaces: [IOSurface] = []
        for _ in 0..<Self.surfaces {
            guard let surface = IOSurface(properties: properties) else {
                throw TestFailure("IOSurface creation failed")
            }
            surfaces.append(surface)
        }
        try await Task.sleep(nanoseconds: 100_000_000)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.width, height: Self.height, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        var textures: [MTLTexture] = []
        var importNs: [UInt64] = []
        for surface in surfaces {
            let start = DispatchTime.now().uptimeNanoseconds
            guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
                throw TestFailure("no texture over the IOSurface")
            }
            importNs.append(DispatchTime.now().uptimeNanoseconds - start)
            textures.append(texture)
        }
        metrics.set("import_us_mean", Int(importNs.reduce(0, +) / UInt64(importNs.count) / 1000))
        metrics.set("import_us_max", Int((importNs.max() ?? 0) / 1000))
        try await gpu.run { commands in
            for texture in textures {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
        }
        var wrong = 0
        for surface in surfaces {
            guard surface.lock(options: .readOnly, seed: nil) == 0 else {
                throw TestFailure("IOSurfaceLock for the readback failed")
            }
            for y in 0..<Self.height {
                let row = UnsafeRawBufferPointer(start: surface.baseAddress + y * Self.rowBytes, count: Self.width * 4)
                for x in 0..<Self.width
                where row[x * 4] != 0 || row[x * 4 + 1] != 255 || row[x * 4 + 2] != 0 || row[x * 4 + 3] != 255 {
                    wrong += 1
                }
            }
            surface.unlock(options: .readOnly, seed: nil)
        }
        metrics.set("wrong_pixels", wrong)
        return wrong == 0 ? [] : ["\(wrong) pixels of \(Self.surfaces) fresh surfaces miss the GPU's clear"]
    }
}
