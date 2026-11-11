import Foundation
import HWTestCore
import HWTestGPU
import Metal

extension X1TwodProbes {
    /// One reading of how the engine filters: an origin shift in source texels, the weights' fractional bits (0: exact)
    /// and, for sRGB, whether it filters linear light (Metal's sampler) or codes
    struct Filter: Hashable {
        let shift: Double
        let weightBits: Int
        let linearLight: Bool

        var name: String { "o\(shift == 0 ? "0" : "m")_w\(weightBits)\(linearLight ? "_lin" : "")" }

        static func all(point: Bool, srgb: Bool) -> [Filter] {
            [0.0, -0.5].flatMap { shift in
                (point ? [0] : [0, 8, 6, 4]).flatMap { bits in
                    (srgb && !point ? [false, true] : [false]).map {
                        Filter(shift: shift, weightBits: bits, linearLight: $0)
                    }
                }
            }
        }

        /// Metal's: no shift, exact weights, sRGB in linear light
        static func metal(srgb: Bool) -> Filter { Filter(shift: 0, weightBits: 0, linearLight: srgb) }
    }

    /// Source texel taps and weights along one axis at 32.32 position `at` (in source texels from the corner)
    static func taps(_ at: Double, _ size: Int, point: Bool, _ f: Filter) -> [(Int, Double)] {
        let p = at + f.shift
        if point { return [(min(max(Int(p.rounded(.down)), 0), size - 1), 1)] }
        let c = p - 0.5
        let i = Int(c.rounded(.down))
        var w = c - Double(i)
        if f.weightBits > 0 { w = (w * Double(1 << f.weightBits)).rounded(.down) / Double(1 << f.weightBits) }
        return [(min(max(i, 0), size - 1), 1 - w), (min(max(i + 1, 0), size - 1), w)]
    }

    static func filtered(
        _ codes: [[UInt32]], _ width: Int, _ height: Int, _ format: X1Format, x: Double, y: Double,
        point: Bool, _ f: Filter
    ) -> [Double] {
        let tx = taps(x, width, point: point, f), ty = taps(y, height, point: point, f)
        return format.channels.enumerated().map { c, channel in
            let linear = channel == .srgb8 && f.linearLight
            var sum = 0.0
            for (j, wy) in ty {
                for (i, wx) in tx where wx * wy != 0 {
                    let v = channel.value(codes[j * width + i][c])
                    sum += wx * wy * (linear ? X1Channel.linear(v) : v)
                }
            }
            return linear ? X1Channel.encoded(sum) : sum
        }
    }

    /// gpu.x1.twod_scale (copy.twod, METALMAP d4/d18): scaled copies by the 2D engine, point and bilinear, corner origin
    /// at half a step (the mip chain's sampling, fx::kGm20bSampling), at 1:1, 2:1, 3:7 (up) and 3:2 (down), against
    /// the CPU's filter of the source's known codes under each reading (Filter) and rounding. A cell grades against
    /// Metal's reading and names the reading and rules that fit it with the fewest misses.
    static let scale = X1Probe(
        id: "gpu.x1.twod_scale", name: "2D-engine scaled copies against the CPU's filter, per format, ratio and filter",
        probe: "copy.twod", risky: true, timeout: 240
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_scale")
        var cells: [(String, X1RuleTally)] = []
        var readings: [String] = []
        var ops = 0
        let ratios = [("1to1", 64, 64), ("2to1", 128, 64), ("3to7", 30, 70), ("3to2", 96, 64)]
        for format in X1Format.all {
            journal.starting(format.name)
            let srgb = format.channels.contains(.srgb8)
            for (ratio, from, to) in ratios {
                let (src, codes) = try await source(gpu, format, from, from)
                for point in [true, false] {
                    let written = try gpu.texture(format.pixelFormat, to, to, usage: [.renderTarget, .shaderRead])
                    let du = (UInt64(from) << 32) / UInt64(to)
                    let batch = try await gpu.run(forms: "") { commands in
                        let blit = commands.makeBlitCommandEncoder()!
                        try RawProbe.engine(
                            commands,
                            [
                                "kind": "scale", "read": src, "written": written, "du": du, "dv": du, "sx": du / 2,
                                "sy": du / 2, "bilinear": point ? 0 : 1, "corner": 1,
                            ])
                        blit.endEncoding()
                    }
                    ops += batch.taken["ops"] ?? 0
                    let bytes = try await gpu.read(written, bytesPerPixel: format.bytesPerTexel)
                    let filters = Filter.all(point: point, srgb: srgb)
                    var tallies = [X1RuleTally](repeating: X1RuleTally(), count: filters.count)
                    let metal = filters.firstIndex(of: .metal(srgb: srgb && !point)) ?? 0
                    for t in 0..<to * to {
                        let (x, y) = (t % to, t / to)
                        let px = Double(UInt64(x) * du + du / 2) / 4_294_967_296
                        let py = Double(UInt64(y) * du + du / 2) / 4_294_967_296
                        let got = format.codes(bytes, t)
                        for (k, f) in filters.enumerated() {
                            let exact = filtered(codes, from, from, format, x: px, y: py, point: point, f)
                            tallies[k].add(
                                got, exact: exact, channels: format.channels, metal: format.metalRounding, at: t)
                        }
                    }
                    let name = "\(format.name)_\(ratio)_\(point ? "point" : "linear")"
                    cells.append((name, tallies[metal]))
                    let best = tallies.indices.min { tallies[$0].fewestMisses < tallies[$1].fewestMisses } ?? metal
                    readings.append("\(name):\(filters[best].name):\(tallies[best].text)")
                }
            }
        }
        var finding = finding("copy.twod", cells, ops: ops, paths: gpu.paths.paths.keys.map { $0 })
        finding.metrics.set("best_readings", readings.joined(separator: ","))
        return finding
    }
}
