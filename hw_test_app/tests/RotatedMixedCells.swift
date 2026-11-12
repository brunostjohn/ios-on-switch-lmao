import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// gpu.x1.rotated_target's passes whose attachments beside the turned target are turned another way, of another frame, or
/// not turned and of another size. Metal accepts each (measured on an M-series Mac). Of one frame, every attachment lands by
/// its own store map. Of other extents, its area and default viewport are not the attachments' intersection, which these
/// cells assume: those two run as informational.
extension RotatedCells {
    /// The second plane's turn: never the target's (rotation 4 stores as rotation 1)
    var secondTurn: Int { turn == 2 ? 3 : 2 }

    /// A second plane of another turn, of the target's frame or a larger one, beside the target (in the plain run, a texture
    /// of that frame), cleared and drawn, then loaded and drawn again; the plane must hold the texture's bytes by its own
    /// store map
    func mixedTurns(_ commands: MTLCommandBuffer, _ target: MTLTexture, larger: Bool) {
        let w = width + (larger ? 32 : 0), h = height + (larger ? 16 : 0)
        let second: MTLTexture
        if beside.turnedRun {
            let cells = RotatedCells(
                gpu: gpu, states: states, turn: secondTurn, width: w, height: h, aligned: aligned)
            guard let plane = try? cells.makePlane() else { return }
            fillSurface(plane, 0x5a)
            beside.second = (plane, cells)
            second = plane.texture
        } else {
            guard let texture = try? gpu.texture(.bgra8Unorm, w, h, storage: .shared) else { return }
            let bytes = [UInt8](repeating: 0x5a, count: w * h * 4)
            texture.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w * 4)
            beside.plainSecond = texture
            second = texture
        }
        pair(commands, target, second, load: .clear) { e in
            shapes(e, rect(4, 2, 60, 40), [1, 0, 0, 1], states.pair)
        }
        // the target cleared again: a rotation-4 target's Load is the load cell's, which reads it transposed
        pair(commands, target, second, load: .load, targetLoad: .clear) { e in
            shapes(e, rect(30, 20, 90, 60), [0, 1, 0, 1], states.pair)
        }
    }

    /// Private textures larger and smaller than the frame beside the target, each cleared and drawn, then loaded and drawn
    /// again: both runs' textures must agree byte for byte
    func mixedSizes(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        for (w, h) in [(width + 32, height + 16), (width - 32, height - 24)] {
            guard let texture = try? gpu.texture(.bgra8Unorm, w, h, storage: .shared) else { return }
            let bytes = [UInt8](repeating: 0x33, count: w * h * 4)
            texture.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w * 4)
            beside.textures.append(texture)
            pair(commands, target, texture, load: .clear) { e in
                shapes(e, rect(4, 2, 40, 30), [1, 0, 0, 1], states.pair)
            }
            pair(commands, target, texture, load: .load) { e in
                shapes(e, rect(20, 10, 70, 50), [0, 0, 1, 1], states.pair)
            }
        }
    }

    /// The second plane's frame texels that are not the plain texture's (each through the second plane's store map)
    func secondPlaneWrong() async throws -> Int {
        guard let (plane, cells) = beside.second, let plain = beside.plainSecond else { return 0 }
        let expected = try await gpu.read(plain, bytesPerPixel: 4)
        guard plane.surface.lock(options: .readOnly, seed: nil) == 0 else {
            throw TestFailure("IOSurfaceLock for the second plane's readback failed")
        }
        defer { plane.surface.unlock(options: .readOnly, seed: nil) }
        var wrong = 0
        for y in 0..<cells.height {
            for x in 0..<cells.width {
                let (px, py) = cells.stored(x, y)
                let got = UnsafeRawBufferPointer(
                    start: plane.surface.baseAddress + py * plane.rowBytes + px * 4, count: 4)
                if !zip(got, expected.pixel(x, y, width: cells.width)).allSatisfy({ $0 == $1 }) { wrong += 1 }
            }
        }
        return wrong
    }

    private func fillSurface(_ plane: Plane, _ byte: UInt8) {
        guard plane.surface.lock(options: [], seed: nil) == 0 else { return }
        memset(plane.surface.baseAddress, Int32(byte), plane.rowBytes * plane.height)
        plane.surface.unlock(options: [], seed: nil)
    }

    private func pair(
        _ commands: MTLCommandBuffer, _ target: MTLTexture, _ second: MTLTexture, load: MTLLoadAction,
        targetLoad: MTLLoadAction? = nil, _ draw: (MTLRenderCommandEncoder) -> Void
    ) {
        let pass = MTLRenderPassDescriptor()
        for (i, texture) in [target, second].enumerated() {
            pass.colorAttachments[i].texture = texture
            pass.colorAttachments[i].loadAction = i == 0 ? targetLoad ?? load : load
            pass.colorAttachments[i].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[i].storeAction = .store
        }
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        draw(e)
        e.endEncoding()
    }
}
