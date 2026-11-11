import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// gpu.x1.rotated_target (METALMAP d26): passes into a texture Metal's rotation SPI turns over an IOSurface plane (a
/// CAMetalLayer drawable on the turned display), rotations 1 to 4, against the same draws into a plain texture turned by
/// Metal's measured store map (metal2vulkan-runtime rotated.rs). The raw path draws straight into the plane through the
/// viewport coordinate swizzle, a state that reads the plane's axes through its turned variant (its position, one axis's
/// derivative), a private depth texture neither loaded nor stored through a backing laid out as the plane, and a 4x
/// resolve into the plane through the resolve state's turned variant: those cells FAIL when they run anywhere else
/// (path= other than raw). Rotation 4's transposed load is reported, not required (residual_paths=, r4_<shape>_load_path=).
/// native_cells / native_raw count the required cells and those that ran raw; tie_wrong counts the pixels the plane's
/// rasteriser decides apart from the frame's where an edge runs through pixel centres (its tie rule turns too).
struct RotatedTargetTest: HWTest {
    let id = "gpu.x1.rotated_target"
    let name = "Rotated drawables: rendered straight into the turned plane, each rotation"
    var subsystem: Subsystem { .metal }
    var timeout: TimeInterval { 240 }

    /// A frame over a plane whose rows are 256-byte multiples (a direct pitch target), one over rows of the plane's
    /// width (a pitch the render hardware takes only through a shadow, as the device's 60x60 and 720x1280 drawables'
    /// were), and the device's 1280x720 drawable for its clear alone
    static let shapes: [(name: String, width: Int, height: Int, aligned: Bool, cells: [RotatedCell])] = [
        ("a", 96, 64, true, RotatedCell.allCases), ("n", 90, 60, false, RotatedCell.allCases),
        ("hd", 1280, 720, false, [.clear]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let raw = RawFacade.kind(of: gpu.device) == "raw"
        let states = try RotatedStates(gpu)
        var metrics = Metrics()
        var failures: [String] = []
        var residual = PathTally()
        var native = 0, nativeRaw = 0, tieWrong = 0
        for turn in 1...4 {
            for shape in Self.shapes {
                let cells = RotatedCells(
                    gpu: gpu, states: states, turn: turn, width: shape.width, height: shape.height,
                    aligned: shape.aligned)
                guard let plane = try cells.makePlane() else {
                    return .fail("the device makes no texture with the rotation SPI", metrics, proof: gpu.proof)
                }
                for cell in shape.cells {
                    let name = "r\(turn)_\(shape.name)_\(cell.rawValue)"
                    if cell == .load && turn == 4 {
                        let result = try await cells.run(.load, into: plane, checked: false)
                        residual.add(result.path)
                        metrics.set("r4_\(shape.name)_load_path", result.path)
                        continue
                    }
                    let result = try await cells.run(cell, into: plane)
                    switch cell.kind {
                    case .native:
                        let required = raw && (cell != .fetch || result.referencePath == "raw")
                        if required {
                            native += 1
                            if result.path == "raw" { nativeRaw += 1 } else { failures.append("\(name) ran \(result.path)") }
                        }
                        if result.wrong != 0 { failures.append("\(name): \(result.wrong) pixels off Metal's store") }
                    case .informational:
                        tieWrong += result.wrong
                    }
                }
            }
        }
        metrics.set("native_cells", native)
        metrics.set("native_raw", nativeRaw)
        metrics.set("tie_wrong", tieWrong)
        metrics.set("residual_paths", residual.isEmpty ? "-" : residual.text)
        metrics.set("paths", gpu.paths.text)
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}

enum RotatedCell: String, CaseIterable {
    case clear, draw, load, points, fetch, position, derivative, fwidth, depth, resolve, tie

    enum Kind { case native, informational }

    var kind: Kind { self == .tie ? .informational : .native }
}

struct RotatedStates {
    let plain, position, derivative, fwidth, fetch, depthTested, multisampled: MTLRenderPipelineState
    let lessEqual: MTLDepthStencilState

    init(_ gpu: GPU) throws {
        let make = { (fragment: String) in
            try gpu.render(vertex: "rotated_vertex", fragment: fragment, format: .bgra8Unorm)
        }
        plain = try make("rotated_fragment")
        position = try make("rotated_position_fragment")
        derivative = try make("rotated_derivative_fragment")
        fwidth = try make("rotated_fwidth_fragment")
        fetch = try make("rotated_fetch_fragment")
        depthTested = try gpu.render(
            vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: .depth32Float)
        multisampled = try gpu.render(
            vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, samples: 4)
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .lessEqual
        d.isDepthWriteEnabled = true
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        lessEqual = state
    }
}

/// One rotation's cells: each encodes its draws into the turned texture and into a plain one of the frame, and compares
/// the plane's bytes with the plain texture's turned by Metal's store map
struct RotatedCells {
    let gpu: GPU
    let states: RotatedStates
    let turn: Int
    let width, height: Int
    let aligned: Bool

    struct Plane {
        let surface: IOSurface
        let texture: MTLTexture
        let width, height, rowBytes: Int
    }

    struct Result {
        let path: String
        let referencePath: String
        let wrong: Int
    }

    var quarter: Bool { turn != 2 }

    /// Metal's store: frame texel (x, y) lands at this plane texel (rotated.rs source_map, Turn::Store)
    func stored(_ x: Int, _ y: Int) -> (Int, Int) {
        let w = width, h = height
        switch turn {
        case 2: return (w - 1 - x, h - 1 - y)
        case 3: return (y, w - 1 - x)
        default: return (h - 1 - y, x)
        }
    }

    func makePlane() throws -> Plane? {
        let w = width, h = height
        let pw = quarter ? h : w, ph = quarter ? w : h
        let rowBytes = aligned ? (pw * 4 + 255) / 256 * 256 : (pw * 4 + 15) / 16 * 16
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: pw, .height: ph, .bytesPerElement: 4, .bytesPerRow: rowBytes, .allocSize: rowBytes * ph,
            .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface creation failed") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        descriptor.usage = .renderTarget
        descriptor.storageMode = .shared
        guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else { return nil }
        descriptor.setValue(NSNumber(value: turn), forKey: "rotation")
        if descriptor.responds(to: NSSelectorFromString("setFramebufferOnly:")) {
            descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        }
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return nil
        }
        return Plane(surface: surface, texture: texture, width: pw, height: ph, rowBytes: rowBytes)
    }

    func run(_ cell: RotatedCell, into plane: Plane, checked: Bool = true) async throws -> Result {
        let w = width, h = height
        let reference = try gpu.texture(.bgra8Unorm, w, h, usage: [.renderTarget, .shaderRead])
        try fill(plane, 0x5a)
        try await gpu.upload([UInt8](repeating: 0x5a, count: w * h * 4), to: reference, bytesPerRow: w * 4)
        var referencePath = ""
        var path = ""
        for pass in passes(cell) {
            let plain = try await gpu.run { pass($0, reference) }.path
            let turned = try await gpu.run { pass($0, plane.texture) }.path
            if referencePath.isEmpty || referencePath == "raw" { referencePath = plain }
            if path.isEmpty || path == "raw" { path = turned }
        }
        guard checked else { return Result(path: path, referencePath: referencePath, wrong: 0) }
        let expected = try await gpu.read(reference, bytesPerPixel: 4)
        guard plane.surface.lock(options: .readOnly, seed: nil) == 0 else {
            throw TestFailure("IOSurfaceLock for the readback failed")
        }
        defer { plane.surface.unlock(options: .readOnly, seed: nil) }
        var wrong = 0
        for y in 0..<h {
            for x in 0..<w {
                let (px, py) = stored(x, y)
                let at = plane.surface.baseAddress + py * plane.rowBytes + px * 4
                let got = UnsafeRawBufferPointer(start: at, count: 4)
                if !zip(got, expected.pixel(x, y, width: w)).allSatisfy({ $0 == $1 }) { wrong += 1 }
            }
        }
        return Result(path: path, referencePath: referencePath, wrong: wrong)
    }

    private func fill(_ plane: Plane, _ byte: UInt8) throws {
        guard plane.surface.lock(options: [], seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        memset(plane.surface.baseAddress, Int32(byte), plane.rowBytes * plane.height)
        plane.surface.unlock(options: [], seed: nil)
    }

    typealias Pass = (MTLCommandBuffer, MTLTexture) -> Void

    private func passes(_ cell: RotatedCell) -> [Pass] {
        switch cell {
        case .clear:
            return [encode(load: .clear) { _ in }]
        case .draw:
            return [encode(load: .clear, drawn)]
        case .load:
            return [
                encode(load: .clear, drawn),
                encode(load: .load) { e in shapes(e, rect(10, 30, 40, 50), [0, 1, 1, 1], states.plain) },
            ]
        case .points:
            return [
                encode(load: .clear) { e in
                    let centres = [(5, 5), (20, 40), (90, 10), (50, 60)]
                    let points = centres.map { c in
                        SIMD4<Float>(
                            (Float(c.0) + 0.5) / Float(width) * 2 - 1,
                            1 - (Float(c.1) + 0.5) / Float(height) * 2, 0.5, 1)
                    }
                    shapes(e, points, [1, 1, 0, 1], states.plain, primitive: .point, perColor: points.count)
                }
            ]
        case .fetch:
            return [
                encode(load: .clear) { e in
                    shapes(e, rect(4, 2, 40, 30), [1, 0, 0, 1], states.plain)
                    shapes(e, rect(20, 10, 70, 50), [0, 0, 1, 1], states.fetch)
                }
            ]
        case .position:
            return [encode(load: .clear) { e in shapes(e, rect(0, 0, 96, 64), [1, 1, 1, 1], states.position) }]
        case .derivative:
            return [encode(load: .clear) { e in shapes(e, rect(0, 0, 96, 64), [1, 1, 1, 1], states.derivative) }]
        case .fwidth:
            return [encode(load: .clear) { e in shapes(e, rect(0, 0, 96, 64), [1, 1, 1, 1], states.fwidth) }]
        case .depth:
            return [depthTested]
        case .resolve:
            return [resolved]
        case .tie:
            return [encode(load: .clear) { e in shapes(e, halfPixelRect(10.5, 5.5, 30.5, 20.5), [1, 1, 1, 1], states.plain) }]
        }
    }

    /// Two rectangles, a scissored quad, and a front-facing and a back-facing rectangle under back-face culling
    private func drawn(_ e: MTLRenderCommandEncoder) {
        shapes(e, rect(4, 2, 20, 10), [1, 0, 0, 1], states.plain)
        shapes(e, rect(30, 40, 90, 60), [0, 1, 0, 1], states.plain)
        e.setScissorRect(MTLScissorRect(x: 50, y: 5, width: 20, height: 20))
        shapes(e, rect(0, 0, 96, 64), [0, 0, 1, 1], states.plain)
        e.setScissorRect(MTLScissorRect(x: 0, y: 0, width: 96, height: 64))
        e.setCullMode(.back)
        shapes(e, rect(72, 2, 92, 18), [1, 1, 0, 1], states.plain)
        shapes(e, rect(72, 22, 92, 36, reversed: true), [1, 0, 1, 1], states.plain)
        e.setCullMode(.none)
    }

    /// Three rectangles over a private depth texture of the frame, cleared and never stored: the nearest one wins where
    /// they overlap, whichever order they come in
    private func depthTested(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let depth = try? gpu.texture(.depth32Float, width, height, usage: .renderTarget) else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 1
        pass.depthAttachment.storeAction = .dontCare
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        e.setDepthStencilState(states.lessEqual)
        let at = { (r: [SIMD4<Float>], z: Float) in r.map { SIMD4<Float>($0.x, $0.y, z, 1) } }
        shapes(e, at(rect(4, 2, 60, 40), 0.7), [1, 0, 0, 1], states.depthTested)
        shapes(e, at(rect(30, 20, 90, 60), 0.3), [0, 0, 1, 1], states.depthTested)
        shapes(e, at(rect(20, 10, 70, 50), 0.5), [0, 1, 0, 1], states.depthTested)
        e.endEncoding()
    }

    /// The draws into a 4x private texture of the frame, resolved into the target
    private func resolved(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let samples = try? gpu.texture(.bgra8Unorm, width, height, usage: .renderTarget, samples: 4) else {
            return
        }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = samples
        pass.colorAttachments[0].resolveTexture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
        pass.colorAttachments[0].storeAction = .multisampleResolve
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        shapes(e, rect(4, 2, 40, 30), [1, 0, 0, 1], states.multisampled)
        shapes(e, halfPixelRect(20.3, 10.55, 70.7, 50.35), [0, 0, 1, 1], states.multisampled)
        e.endEncoding()
    }

    private func encode(load: MTLLoadAction, _ draw: @escaping (MTLRenderCommandEncoder) -> Void) -> Pass {
        { commands, target in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = load
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            draw(encoder)
            encoder.endEncoding()
        }
    }

    private func rect(_ x0: Int, _ y0: Int, _ x1: Int, _ y1: Int, reversed: Bool = false) -> [SIMD4<Float>] {
        let c = GPU.pixelQuad(
            x0, y0, x1, y1, width: width, height: height)
        return reversed ? [c[0], c[2], c[1], c[1], c[2], c[3]] : [c[0], c[1], c[2], c[1], c[3], c[2]]
    }

    /// A rectangle whose edges run through pixel centres, where the rasteriser's tie rule decides
    private func halfPixelRect(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float) -> [SIMD4<Float>] {
        let fx = { (x: Float) in x / Float(width) * 2 - 1 }
        let fy = { (y: Float) in 1 - y / Float(height) * 2 }
        let c = GPU.quad(x0: fx(x0), y0: fy(y0), x1: fx(x1), y1: fy(y1), z: 0.5)
        return [c[0], c[1], c[2], c[1], c[3], c[2]]
    }

    private func shapes(
        _ e: MTLRenderCommandEncoder, _ corners: [SIMD4<Float>], _ color: SIMD4<Float>,
        _ state: MTLRenderPipelineState, primitive: MTLPrimitiveType = .triangle, perColor: Int = 6
    ) {
        var corners = corners
        var colors = [color]
        var per = UInt32(perColor)
        e.setRenderPipelineState(state)
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        e.setVertexBytes(&colors, length: MemoryLayout<SIMD4<Float>>.stride, index: 1)
        e.setVertexBytes(&per, length: 4, index: 2)
        e.drawPrimitives(type: primitive, vertexStart: 0, vertexCount: corners.count)
    }
}
