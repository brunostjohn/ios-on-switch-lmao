import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Render passes into two-plane YCbCr attachments over CoreVideo surfaces, as cameracaptured renders its frames
/// (RGB8_420_2P over '420v'/'420f'): a clear, a draw of floats and a draw of halves into each, every byte of both planes
/// against Metal's store (YcbcrStore, an M4 Max's bytes) and every command buffer on the raw path. The raw path
/// renders the 8-bit code formats (device log: "raw: YCbCr code target stored on the GPU"); the converted 500 and the
/// 10-bit 543 need the pack's store states, which come with a stack key change, so their checks fail until then.
struct YcbcrTargetTest: HWTest {
    let id = "gpu.ycbcr_target"
    let name = "Render passes into two-plane YCbCr surfaces"
    let subsystem = Subsystem.metal

    struct Target {
        let format: UInt
        let fourcc: OSType
        let pending: String?

        var label: String {
            let code = String(bytes: withUnsafeBytes(of: fourcc.bigEndian) { Array($0) }, encoding: .ascii) ?? "?"
            return "\(format) over '\(code)'"
        }
    }

    static let targets: [Target] = [
        Target(format: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, pending: nil),
        Target(format: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, pending: nil),
        Target(format: 541, fourcc: kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, pending: nil),
        Target(
            format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            pending: "needs the pack's converted store state (stack key change)"),
        Target(
            format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            pending: "needs the pack's converted store state (stack key change)"),
        Target(
            format: 543, fourcc: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
            pending: "needs the pack's 10-bit code store state (stack key change)"),
    ]
    static let width = 128, height = 64
    static let clear = MTLClearColor(red: 0.70196, green: 0.501960784, blue: 0.17058829, alpha: 1)

    func run(_ context: TestContext) async throws -> Outcome {
        let probe = try GPU.make()
        guard RawFacade.kind(of: probe.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: probe.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for target in Self.targets {
            let gpu = try GPU.make()
            let wrong: Int
            let note: String?
            do {
                (wrong, note) = try await exercise(gpu, target)
            } catch {
                (wrong, note) = (0, "failed: \(GPU.describe(error))")
            }
            let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
            let key = target.label.filter { $0.isLetter || $0.isNumber }
            metrics.set("paths_" + key, gpu.paths.isEmpty ? "-" : gpu.paths.text)
            metrics.set("wrong_" + key, wrong)
            var check = "\(target.label): every byte Metal's, every command buffer raw"
            if let note { check += " (\(note))" }
            if let pending = target.pending { check += " [\(pending)]" }
            checks.append((check, note == nil && wrong == 0 && raw))
        }
        return .judged(checks, metrics, proof: probe.proof)
    }

    /// The bytes off Metal's store over the target's passes, and why the passes could not run.
    func exercise(_ gpu: GPU, _ target: Target) async throws -> (Int, String?) {
        let (w, h) = (Self.width, Self.height)
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, w, h, target.fourcc, attributes, &made) == kCVReturnSuccess, let buffer = made,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let format = MTLPixelFormat(rawValue: target.format)
        else { return (0, "no surface") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return (0, "no texture")
        }
        let floats: MTLRenderPipelineState, halves: MTLRenderPipelineState
        do {
            floats = try gpu.render(fragment: "texel_values", format: format)
            halves = try gpu.render(fragment: "texel_halves", format: format)
        } catch {
            return (0, "no state: \(GPU.describe(error))")
        }
        let values = Self.values(w * h)
        let halved = values.map { SIMD4<Float>(Float16($0.x).float, Float16($0.y).float, Float16($0.z).float, 1) }
        let clear = SIMD4<Float>(Float(Self.clear.red), Float(Self.clear.green), Float(Self.clear.blue), 1)
        let passes: [(MTLRenderPipelineState?, [SIMD4<Float>], YcbcrStore.Source)] = [
            (nil, Array(repeating: clear, count: w * h), .clear), (floats, values, .drawnFloat),
            (halves, halved, .drawnHalf),
        ]
        var wrong = 0
        for (index, (state, texels, source)) in passes.enumerated() {
            Self.scribble(buffer, seed: UInt8(index))
            let data = try gpu.buffer(state == nil ? [clear] : values)
            var width = UInt32(w)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = state == nil ? .clear : .dontCare
                pass.colorAttachments[0].clearColor = Self.clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                if let state {
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBuffer(data, offset: 0, index: 0)
                    encoder.setFragmentBytes(&width, length: 4, index: 1)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            wrong += Self.mismatches(buffer, Self.expected(target, texels, source), wide: target.format == 543)
        }
        return (wrong, nil)
    }

    /// The planes Metal stores: plane 0's codes, then plane 1's Cb, Cr pairs.
    static func expected(_ target: Target, _ texels: [SIMD4<Float>], _ source: YcbcrStore.Source) -> [[UInt16]] {
        let down = target.fourcc != kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange
        let full = target.fourcc == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
        let (luma, chroma) =
            target.format == 500
            ? YcbcrStore.converted(texels, width: width, height: height, full: full, halvedDown: down)
            : YcbcrStore.codes(
                texels, width: width, height: height, max: target.format == 543 ? 1023 : 255, halvedDown: down,
                source: source)
        let shift: UInt16 = target.format == 543 ? 6 : 0
        return [luma.map { $0 << shift }, chroma.map { $0 << shift }]
    }

    static func mismatches(_ buffer: CVPixelBuffer, _ planes: [[UInt16]], wide: Bool) -> Int {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        var wrong = 0
        for (plane, codes) in planes.enumerated() {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { return codes.count }
            let row = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            let rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
            let perRow = codes.count / rows
            for y in 0..<rows {
                let line = base.advanced(by: y * row)
                for x in 0..<perRow {
                    let stored =
                        wide
                        ? UInt16(line.load(fromByteOffset: 2 * x, as: UInt8.self))
                            | UInt16(line.load(fromByteOffset: 2 * x + 1, as: UInt8.self)) << 8
                        : UInt16(line.load(fromByteOffset: x, as: UInt8.self))
                    wrong += stored == codes[y * perRow + x] ? 0 : 1
                }
            }
        }
        return wrong
    }

    /// Bytes no pass wrote, so a store that missed a texel shows.
    static func scribble(_ buffer: CVPixelBuffer, seed: UInt8) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        for plane in 0..<CVPixelBufferGetPlaneCount(buffer) {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { continue }
            let bytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) * CVPixelBufferGetHeightOfPlane(buffer, plane)
            let out = base.bindMemory(to: UInt8.self, capacity: bytes)
            for i in 0..<bytes { out[i] = UInt8(truncatingIfNeeded: i &* 37) &+ seed }
        }
    }

    /// Floats beside the half midpoints and the 1/255 and 1/1023 ties, uniform ones, out of range and not numbers.
    static func values(_ count: Int) -> [SIMD4<Float>] {
        var state: UInt64 = 540
        func next() -> UInt64 {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return state >> 33
        }
        func beside(_ x: Float, _ spread: UInt64) -> Float {
            Float(bitPattern: x.bitPattern &+ UInt32(next() % (2 * spread + 1)) &- UInt32(spread))
        }
        func value() -> Float {
            switch next() % 8 {
            case 0:
                let h = UInt16(next() % 0x3c00)
                return beside((Float16(bitPattern: h).float + Float16(bitPattern: h + 1).float) / 2, 1)
            case 1: return beside((Float(next() % 255) + 0.5) / 255, 3)
            case 2: return beside((Float(next() % 1023) + 0.5) / 1023, 3)
            case 3: return [Float.nan, .infinity, -.infinity, -0.0, -0.25, 1.5, 0, 1][Int(next() % 8)]
            default: return Float(next() % 1_000_001) / 1_000_000 * 1.1 - 0.05
            }
        }
        return (0..<count).map { _ in SIMD4(value(), value(), value(), 1) }
    }
}

extension Float16 {
    fileprivate var float: Float { Float(self) }
}
