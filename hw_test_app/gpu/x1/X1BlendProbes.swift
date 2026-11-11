import Foundation
import HWTestCore
import Metal

/// COMPILER.md E4: GM20B's ROP blend on BGRA8 against Metal's half arithmetic, per factor pair, over the domain Core
/// Animation feeds it: premultiplied s <= sa, each one of the 256 halves nearest k / 255 (32,896 pairs), over every
/// destination code. One 4096 x 2056 draw a pair: pixel p is pair p / 256 over code p % 256 (red and alpha; green and
/// blue over two other codes). The reference is the model an M-series Mac matches in every case (X1ProbeTests): each
/// product rounded to half, then their sum; a fused multiply-add misses it for dov, sat and xor.
public enum X1BlendProbes {
    static let width = 4096
    static let height = 2056

    struct Pair {
        let name: String
        let source: (MTLBlendFactor, X1Reference.Factor)
        let destination: (MTLBlendFactor, X1Reference.Factor)
    }

    static let pairs: [Pair] = [
        Pair(name: "sov", source: (.one, .one), destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(name: "dov", source: (.oneMinusDestinationAlpha, .oneMinusDa), destination: (.one, .one)),
        Pair(name: "sin", source: (.destinationAlpha, .da), destination: (.zero, .zero)),
        Pair(name: "din", source: (.zero, .zero), destination: (.sourceAlpha, .sa)),
        Pair(name: "dot", source: (.zero, .zero), destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(name: "sat", source: (.destinationAlpha, .da), destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(
            name: "xor", source: (.oneMinusDestinationAlpha, .oneMinusDa),
            destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(name: "add", source: (.one, .one), destination: (.one, .one)),
    ]

    static let halves: [Float16] = (0..<256).map { Float16(Double($0) / 255) }
    static let cases: [(UInt16, UInt16)] = (0..<256).flatMap { a in (0...a).map { (UInt16($0), UInt16(a)) } }

    /// The destination's codes at pixel p: (blue, green, red, alpha) in memory order.
    static func destination(_ p: Int) -> (Int, Int, Int, Int) {
        let d = p & 255
        return (255 - d, (d * 7 + 3) & 255, d, d)
    }

    public static func exact(_ gpu: GPU) async throws -> ProbeFinding {
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        let pixels = width * height
        var initial = [UInt8](repeating: 0, count: pixels * 4)
        for p in 0..<pixels {
            let (b, g, r, a) = destination(p)
            initial[p * 4] = UInt8(b)
            initial[p * 4 + 1] = UInt8(g)
            initial[p * 4 + 2] = UInt8(r)
            initial[p * 4 + 3] = UInt8(a)
        }
        let staging = try gpu.buffer(initial)
        let caseBuffer = try gpu.buffer(cases.map { SIMD2<UInt16>($0.0, $0.1) })
        let halfBuffer = try gpu.buffer(halves)
        let target = try gpu.texture(.bgra8Unorm, width, height, usage: [.renderTarget, .shaderRead])
        for pair in pairs {
            let state = try gpu.render(fragment: "x1_blend_source", format: .bgra8Unorm) { descriptor in
                let attachment = descriptor.colorAttachments[0]!
                attachment.isBlendingEnabled = true
                attachment.sourceRGBBlendFactor = pair.source.0
                attachment.sourceAlphaBlendFactor = pair.source.0
                attachment.destinationRGBBlendFactor = pair.destination.0
                attachment.destinationAlphaBlendFactor = pair.destination.0
            }
            paths.append(
                try await gpu.run { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.copy(
                        from: staging, sourceOffset: 0, sourceBytesPerRow: width * 4, sourceBytesPerImage: 0,
                        sourceSize: MTLSize(width: width, height: height, depth: 1), to: target, destinationSlice: 0,
                        destinationLevel: 0, destinationOrigin: MTLOrigin())
                    blit.endEncoding()
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .load
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBuffer(caseBuffer, offset: 0, index: 0)
                    encoder.setFragmentBuffer(halfBuffer, offset: 0, index: 1)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                }.path)
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            let counts = compare(pair, bytes)
            wrong[pair.name + "_wrong"] = counts.separate
            metrics.set(pair.name + "_fma_misses", counts.fma)
            metrics.set(pair.name + "_exact_misses", counts.exact)
            metrics.set(pair.name + "_max_diff", counts.maxDiff)
            if let first = counts.first { metrics.set(pair.name + "_first", first) }
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }

    struct Counts {
        var fma = 0, separate = 0, exact = 0, maxDiff = 0
        var first: String?

        mutating func add(_ have: Int, _ s: Double, _ fs: Double, _ d: Double, _ fd: Double, at p: Int, channel: Int) {
            let want = X1Reference.unorm(X1Reference.halfSeparate(s, fs, d, fd), top: 255)
            if have != want {
                separate += 1
                if first == nil { first = "\(p).\(channel):\(have),want_\(want)" }
            }
            if have != X1Reference.unorm(X1Reference.halfFMA(s, fs, d, fd), top: 255) { fma += 1 }
            if have != X1Reference.unorm(X1Reference.exact(s, fs, d, fd), top: 255) { exact += 1 }
            maxDiff = max(maxDiff, abs(have - want))
        }
    }

    /// Every channel of every case against the three models' codes.
    static func compare(_ pair: Pair, _ bytes: [UInt8]) -> Counts {
        var counts = Counts()
        let halfValues = halves.map(Double.init)
        let texel = (0..<256).map { X1Reference.texel($0, top: 255) }
        for p in 0..<(cases.count * 256) {
            let (si, ai) = cases[p >> 8]
            let (s, sa) = (halfValues[Int(si)], halfValues[Int(ai)])
            let (b, g, r, a) = destination(p)
            let da = texel[a]
            let fs = pair.source.1.value(sa: sa, da: da)
            let fd = pair.destination.1.value(sa: sa, da: da)
            counts.add(Int(bytes[p * 4]), s, fs, texel[b], fd, at: p, channel: 0)
            counts.add(Int(bytes[p * 4 + 1]), s, fs, texel[g], fd, at: p, channel: 1)
            counts.add(Int(bytes[p * 4 + 2]), s, fs, texel[r], fd, at: p, channel: 2)
            counts.add(Int(bytes[p * 4 + 3]), sa, fs, da, fd, at: p, channel: 3)
        }
        return counts
    }
}
