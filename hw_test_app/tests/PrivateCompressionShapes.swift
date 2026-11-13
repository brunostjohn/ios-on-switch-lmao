import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.private_compression's shape arms (raw_private_comp.h privateShapeCompresses): a private target of each shape
/// the device lays out on compressed memory, made until it binds some, drawn and read whole, against the same work with
/// compression held off. `<arm>_made` is its comptags when made, `<arm>_compressed` after the work, `<arm>_paths` the
/// command buffers' paths; it fails where the bytes differ, the target was not compressed or the work left the raw path.
extension PrivateCompressionTest {
    struct ShapeRun {
        var bytes: [UInt8] = []
        var made = 0, compressed = 0
        var paths: [String] = []
    }

    static let shapeArms = ["mips"]

    func shapeArms(_ gpu: GPU, _ facade: NSObject, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        for arm in Self.shapeArms {
            let run = try await shapeArm(arm, gpu, facade, plain: false)
            _ = facade.perform(Self.plainSelector, with: NSNumber(value: true))
            let reference: ShapeRun
            do {
                defer { _ = facade.perform(Self.plainSelector, with: NSNumber(value: false)) }
                reference = try await shapeArm(arm, gpu, facade, plain: true)
            }
            let differ =
                zip(run.bytes, reference.bytes).filter { $0 != $1 }.count + abs(run.bytes.count - reference.bytes.count)
            metrics.set("\(arm)_bytes_differ", differ)
            metrics.set("\(arm)_made", run.made)
            metrics.set("\(arm)_compressed", run.compressed)
            metrics.set("\(arm)_paths", run.paths.joined(separator: "/"))
            if differ > 0 { wrong.append("\(arm)_bytes_differ") }
            if run.made != 1 || run.compressed != 1 { wrong.append("\(arm)_not_compressed") }
            if reference.made == 1 { wrong.append("\(arm)_reference_compressed") }
            if run.paths.contains(where: { $0 != "raw" }) { wrong.append("\(arm)_path") }
        }
    }

    /// `make` until what it made binds compressed memory (20 tries), or once where `plain`
    func made(_ facade: NSObject, plain: Bool, _ make: () throws -> MTLTexture) async throws -> MTLTexture {
        var made = try make()
        for _ in 0..<20 where !plain && comptags(facade, made) == 0 {
            try await Task.sleep(nanoseconds: 50_000_000)
            made = try make()
        }
        return made
    }

    func shapeArm(_ arm: String, _ gpu: GPU, _ facade: NSObject, plain: Bool) async throws -> ShapeRun {
        var run = ShapeRun()
        let pattern = try gpu.render(fragment: "comp_pattern_fragment", format: .bgra8Unorm)
        func draw(_ target: MTLTexture, slice: Int = 0) -> (MTLCommandBuffer) -> Void {
            { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = target
                p.colorAttachments[0].slice = slice
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                p.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(pattern)
                var corners = GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }
        }
        switch arm {
        case "mips":
            let target = try await made(facade, plain: plain) { try gpu.texture(.bgra8Unorm, 256, 256, mipmaps: 5) }
            run.made = comptags(facade, target)
            run.paths.append(try await gpu.run(draw(target)).path)
            run.paths.append(
                try await gpu.run { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.generateMipmaps(for: target)
                    blit.endEncoding()
                }.path)
            for level in 0..<target.mipmapLevelCount {
                run.bytes += try await gpu.read(target, bytesPerPixel: 4, level: level)
            }
            run.compressed = comptags(facade, target)
        default:
            throw TestFailure("no shape arm \(arm)")
        }
        return run
    }
}
