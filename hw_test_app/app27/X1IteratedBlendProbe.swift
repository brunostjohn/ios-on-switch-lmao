import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// METALMAP d14 and x1-glass/blend/REPORT.txt 1.1: the ROP's iterated blend (SET_ITERATED_BLEND 0x0dd0,
/// LOAD_ITERATED_BLEND_INSTRUCTION 0x0df4), which NVN uses for advanced blending and no open driver programs. A
/// premultiplied ramp over a premultiplied destination in BGRA8, with alpha left to the fixed-function source-over:
/// first a one-instruction program that passes the source through (does a program's last result reach the target at
/// all), then KHR multiply in two (Cs*Cd + Cs*(1-Ad) + Cd*(1-As)). Instruction fields are clb197.h's; that a last
/// instruction writing PASS_OUTPUT NONE is the blend's result is inferred from Ryujinx's PBR destination.
enum X1IteratedBlendProbe {
    static let size = 256

    /// One instruction, always executed (TEST_CC TRUE), ADD_PRODUCTS: A * B + C * D into `output` (0-2 a temporary,
    /// 3 the blend result), RGB swizzle and mask.
    static func instruction(a: UInt32, b: UInt32, c: UInt32, d: UInt32, output: UInt32) -> UInt32 {
        1 | a << 9 | b << 12 | c << 16 | d << 19 | output << 28
    }

    enum Operand {
        static let srcRGB: UInt32 = 0, destRGB: UInt32 = 1, temp0AC: UInt32 = 4
        static let zero: UInt32 = 0, one: UInt32 = 1, destRGBBD: UInt32 = 5, oneMinusSrcA: UInt32 = 4
        static let oneMinusDestA: UInt32 = 7
    }

    static let passThrough = [
        instruction(a: Operand.srcRGB, b: Operand.one, c: Operand.srcRGB, d: Operand.zero, output: 3)
    ]
    static let multiply = [
        instruction(a: Operand.srcRGB, b: Operand.destRGBBD, c: Operand.srcRGB, d: Operand.oneMinusDestA, output: 0),
        instruction(a: Operand.destRGB, b: Operand.oneMinusSrcA, c: Operand.temp0AC, d: Operand.one, output: 3),
    ]

    static func words(_ program: [UInt32]) -> [(UInt32, UInt32)] {
        [(X1RawProbes.loadIteratedBlendPointer, 0)] + program.map { (X1RawProbes.loadIteratedBlendInstruction, $0) }
            + [(X1RawProbes.setIteratedBlendPass, UInt32(program.count)), (X1RawProbes.setIteratedBlend, 1)]
    }

    static let restore: [(UInt32, UInt32)] = [(X1RawProbes.setIteratedBlend, 0), (X1RawProbes.setIteratedBlendPass, 0)]

    /// The destination's premultiplied codes at (x, y): (blue, green, red, alpha) in memory order.
    static func destination(_ x: Int, _ y: Int) -> [Int] {
        let (r, g, b) = (y, 255 - y, x)
        return [b, g, r, max(r, g, b)]
    }

    static func source(_ x: Int, _ y: Int) -> [Double] {
        let u = Float(x) / 255, a = Float(y) / 255
        let rgba = [u * a, (1 - u) * a, 0.5 * a, a].map { Double(Float16($0)) }
        return [rgba[2], rgba[1], rgba[0], rgba[3]]
    }

    static let probe = X1Probe(
        id: "gpu.x1.iterated_blend", name: "Iterated blend: pass-through, then multiply as ROP microcode",
        probe: "d14", risky: true
    ) { gpu in
        let initial = (0..<size * size).flatMap { destination($0 % size, $0 / size).map(UInt8.init) }
        let state = try gpu.render(fragment: "x1_ramp_source", format: .bgra8Unorm, configure: X1RawProbes.sourceOver)
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        var emitted = 0
        var results: [String: [UInt8]] = [:]
        for (name, program) in [("plain", [UInt32]()), ("pass", passThrough), ("multiply", multiply)] {
            let target = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
            try await gpu.upload(initial, to: target, bytesPerRow: size * 4)
            let draw = { (commands: MTLCommandBuffer) in
                let encoder = commands.makeRenderCommandEncoder(descriptor: X1RawProbes.pass(target, load: .load))!
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            if program.isEmpty {
                paths.append(try await gpu.run(draw).path)
            } else {
                let drawn = try await RawMethods.draw(gpu, before: words(program), after: restore, draw)
                paths.append(drawn.path)
                emitted += drawn.emitted
            }
            results[name] = try await gpu.read(target, bytesPerPixel: 4)
        }
        for name in ["pass", "multiply"] {
            let got = results[name]!
            var exact = 0
            var beyondOne = 0
            var maxDiff = 0
            for p in 0..<(size * size) {
                let (x, y) = (p % size, p / size)
                let s = source(x, y)
                let d = destination(x, y).map { Double($0) / 255 }
                for c in 0..<4 {
                    let value: Double
                    if c == 3 {
                        value = s[3] + d[3] * (1 - s[3])
                    } else if name == "pass" {
                        value = s[c]
                    } else {
                        value = s[c] * d[c] + s[c] * (1 - d[3]) + d[c] * (1 - s[3])
                    }
                    let diff = abs(Int(got[p * 4 + c]) - X1Reference.unorm(value, top: 255))
                    if diff > 0 { exact += 1 }
                    if diff > 1 { beyondOne += 1 }
                    maxDiff = max(maxDiff, diff)
                }
            }
            wrong[name + "_beyond_one"] = beyondOne
            metrics.set(name + "_inexact", exact)
            metrics.set(name + "_max_diff", maxDiff)
            metrics.set(name + "_same_as_source_over", got == results["plain"]!)
            metrics.set(name + "_first_pixel", got.prefix(4).map { String($0) }.joined(separator: ","))
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics).emitting(emitted)
    }
}
