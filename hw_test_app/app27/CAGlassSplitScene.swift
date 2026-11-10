import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal
import simd

struct CAGlassSplitTest: HWTest {
    let scene = CAGlassSplitScene()
    let subsystem = Subsystem.metal
    var id: String { scene.id }
    var name: String { scene.name }
    var timeout: TimeInterval { scene.timeout * 2 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("requires the raw facade to exercise native whole and split programs")
        }
        guard RawFacade.counters(of: gpu.device)["split_prologues"] != nil else {
            return .skip("requires development split attribution; route coverage is unverified")
        }
        return try await GPUSceneTest(scene: scene).run(context)
    }
}

// Scissoring the same quad keeps every vertex output identical while reducing the split selector's pixel area.
// Cumulative launch counts must prove different native programs; image agreement alone cannot establish coverage.
struct CAGlassSplitScene: GPUScene {
    let id = "gpu.ca.glass_split"
    let name = "CA 27 Liquid Glass over content: one draw on its split twin against tiles on its whole program"

    static let size = 256
    static let tiles = 8
    static let line =
        "R ok=1 label=com.apple.coreanimation.PBGRABsovXmw_TgbhA3Xhf_Isrc_Isrc samples=1 topo=0 ds=0/0"
        + " c0=80:15:1:1:5:0:1:5:0 c1=115:0:0:1:0:0:1:0:0 c2=55:0:0:1:0:0:1:0:0"
        + " a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1"
        + " | v variable_blur_vert_lph  | f glass_background_all_lph  | "

    struct Frame {
        let pipeline: MTLRenderPipelineState
        let content: [UInt8]
        let target: MTLTexture
        let scratch: [MTLTexture]
        let pictures: [MTLTexture]
        let material: [(fragment: Bool, slot: Int, bytes: [UInt8])]
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        guard let parsed = PipelineLine(Self.line),
            let library = try? gpu.device.makeLibrary(URL: CAState.library),
            library.makeFunction(name: parsed.fragment.name) != nil
        else {
            result.expect(false, "the required QuartzCore glass shader is unavailable")
            return result
        }
        let frame = try await make(gpu, parsed, library)
        let onFacade = RawFacade.kind(of: gpu.device) == "raw"
        let whole = try await draw(gpu, frame, tiles: Self.tiles, &result)
        let split = try await draw(gpu, frame, tiles: 1, &result)
        let content = TexelFormat.bgra8Unorm.raster(frame.content, width: Self.size, height: Self.size)
        result.metrics.set("pixels_split", Self.size * Self.size)
        result.metrics.set("pixels_tile", (Self.size / Self.tiles) * (Self.size / Self.tiles))
        for (name, picture) in [("split", split.picture), ("whole", whole.picture), ("content", content)] {
            result.metrics.set("mean_" + name, Self.mean(picture))
        }
        for (name, half) in [("split", split), ("whole", whole)] {
            result.metrics.set("picture_" + name, Self.coarsePicture(half.picture))
            if let twins = half.twins { result.metrics.set("twins_" + name, twins) }
        }
        for (name, half) in [("one draw", split), ("the tiles", whole)] {
            result.expect(
                RasterDiff.compare(half.picture, content, tolerance: 2.0 / 255).wrong > 0,
                "\(name) left the content unchanged; no glass output was demonstrated")
            result.expect(!Self.black(half.picture), "\(name) drew black over the content")
        }
        result.compare("one draw against tiles", split.picture, whole.picture, tolerance: 3.0 / 255)
        if onFacade {
            result.expect(split.twins.map { $0 > 0 } ?? false, "the whole-target split path was not proven")
            result.expect(whole.twins.map { $0 == 0 } ?? false, "the tiled whole-program path was not proven")
            let paths = Set(split.paths + whole.paths)
            result.expect(paths == ["raw"], "the glass drew on \(paths.sorted().joined(separator: ",")), not raw")
        }
        return result
    }

    func make(_ gpu: GPU, _ parsed: PipelineLine, _ library: MTLLibrary) async throws -> Frame {
        let pipeline = try CAState.pipeline(parsed, library: library, device: gpu.device)
        let size = Self.size
        let content = Patterns.bytes(
            Patterns.opaque(Patterns.busy(width: size, height: size, channels: 4, seed: 3)), .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
        var pictures: [MTLTexture] = []
        for seed in [1, 2] {
            let picture = try gpu.texture(.bgra8Unorm, size, size, usage: [.shaderRead])
            let bytes = Patterns.bytes(Patterns.busy(width: size, height: size, channels: 4, seed: seed), .bgra8Unorm)
            try await gpu.upload(bytes, to: picture, bytesPerRow: size * 4)
            pictures.append(picture)
        }
        let scratch = try parsed.colors.dropFirst().map {
            try gpu.texture(
                MTLPixelFormat(rawValue: $0.format) ?? .invalid, size, size, usage: [.renderTarget],
                storage: .memoryless)
        }
        // Initialize every byte the shader's constant windows may read, including the tail after the transform.
        var transform = Self.material(seed: 2)
        withUnsafeBytes(of: matrix_identity_float4x4) { transform.replaceSubrange(0..<64, with: $0) }
        let material =
            [(false, 3), (true, 1), (true, 6), (true, 9)].map { (fragment, slot) in
                (fragment: fragment, slot: slot, bytes: Self.material(seed: slot + (fragment ? 16 : 0)))
            } + [(fragment: false, slot: 2, bytes: transform)]
        return Frame(
            pipeline: pipeline, content: content, target: target, scratch: scratch, pictures: pictures,
            material: material)
    }

    struct Half {
        let picture: Raster
        let twins: Int?
        let paths: [String]
    }

    func draw(_ gpu: GPU, _ frame: Frame, tiles: Int, _ result: inout SceneResult) async throws -> Half {
        try await gpu.upload(frame.content, to: frame.target, bytesPerRow: Self.size * 4)
        let before = RawFacade.counters(of: gpu.device)["split_prologues"]
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = frame.target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            for (index, texture) in frame.scratch.enumerated() {
                pass.colorAttachments[index + 1].texture = texture
                pass.colorAttachments[index + 1].loadAction = .clear
                pass.colorAttachments[index + 1].storeAction = .dontCare
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(frame.pipeline)
            CAState.bind(encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: .one), source: nil)
            for m in frame.material {
                if m.fragment {
                    encoder.setFragmentBytes(m.bytes, length: m.bytes.count, index: m.slot)
                } else {
                    encoder.setVertexBytes(m.bytes, length: m.bytes.count, index: m.slot)
                }
            }
            encoder.setFragmentTexture(frame.pictures[0], index: 3)
            encoder.setFragmentTexture(frame.pictures[1], index: 4)
            let step = Self.size / tiles
            for ty in 0..<tiles {
                for tx in 0..<tiles {
                    encoder.setScissorRect(
                        MTLScissorRect(x: tx * step, y: ty * step, width: step, height: step))
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
            }
            encoder.endEncoding()
        }
        result.expect(!timing.path.hasPrefix("error:"), "\(tiles)x\(tiles) tiles: \(timing.path)")
        let after = RawFacade.counters(of: gpu.device)["split_prologues"]
        let picture = try await gpu.raster(frame.target, .bgra8Unorm)
        let twins: Int?
        if let before, let after, after >= before {
            twins = after - before
        } else {
            twins = nil
        }
        return Half(picture: picture, twins: twins, paths: [timing.path])
    }

    static func material(seed: Int) -> [UInt8] {
        (0..<64).flatMap { i -> [UInt8] in
            let value = Float(16 + (i * 7 + seed * 13) % 33) / 64
            return withUnsafeBytes(of: value.bitPattern.littleEndian) { Array($0) }
        }
    }

    static func mean(_ picture: Raster) -> String {
        let count = Float(picture.width * picture.height)
        return (0..<3).map { c in
            var sum: Float = 0
            for y in 0..<picture.height { for x in 0..<picture.width { sum += picture[x, y, c] } }
            return String(Int((sum / count * 255).rounded()))
        }.joined(separator: ",")
    }

    static func coarsePicture(_ picture: Raster) -> String {
        let blocks = picture.boxDownsampled(by: 16)
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for value in blocks.samples { hash = (hash ^ UInt64(min(15, Int(value * 16)))) &* 0x100_0000_01b3 }
        return String(hash, radix: 16)
    }

    static func black(_ picture: Raster) -> Bool {
        for y in 0..<picture.height {
            for x in 0..<picture.width where (0..<3).contains(where: { picture[x, y, $0] > 1.0 / 255 }) { return false }
        }
        return true
    }
}
