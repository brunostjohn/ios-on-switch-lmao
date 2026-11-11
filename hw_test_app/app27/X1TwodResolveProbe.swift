import Foundation
import HWTestCore
import HWTestGPU
import Metal

extension X1TwodProbes {
    /// Sample codes per pixel class and sample whose averages fall at every quarter of a code: 4x .25, .5 and .75,
    /// 2x .0 and .5, per channel rotated
    static let sampleOffsets: [[UInt32]] = [[0, 0, 0, 1], [0, 1, 1, 1], [0, 0, 1, 1], [0, 1, 0, 0]]

    /// The code channel c of pixel class `set` holds at sample s
    static func sampleCode(_ channel: X1Channel, set: Int, c: Int, s: Int) -> UInt32 {
        let offset = sampleOffsets[(set + c) % 4][s]
        let seed = UInt32(set * 37 + 20 + c * 61)
        switch channel {
        case .unorm, .srgb8:
            let top = UInt32((1 << (channel.codeBits ?? 8)) - 1)
            return 1 + (seed * (top - 2) / 256) % (top - 1) + offset
        case .float(let m): return 14 << UInt32(m) | (seed % ((1 << UInt32(m)) - 2)) + offset
        case .float32: return Float(0.5 + Double(seed) / 1024).bitPattern + offset
        }
    }

    /// gpu.x1.twod_resolve (resolve.twod, METALMAP d5): a multisampled pass's resolve by the 2D engine, its sample
    /// grid read as one image at 2:1 (corner origin at half a step), held to the average of the codes the samples
    /// store, which a one-sample pass of each sample's colour measures first; resolve.draw (the pack's resolve state,
    /// today's route) is judged the same way where the format has one. Every command buffer stays on the raw path:
    /// under resolve=twod the facade needs no resolve state for the format.
    static let resolve = X1Probe(
        id: "gpu.x1.twod_resolve", name: "2D-engine MSAA resolves against the samples' average, per format and count",
        probe: "resolve.twod", risky: true, timeout: 150
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_resolve")
        var cells: [(String, X1RuleTally)] = [], draws: [(String, X1RuleTally)] = [], linear: [String] = []
        var ops = 0
        let (w, h) = (96, 64)
        for format in X1Format.named(["bgra8", "rgba8", "bgra8srgb", "rgba8srgb", "rgba16f", "rgb10a2", "rg11b10f"]) {
            let stored = try await storedSamples(gpu, format)
            for samples in [2, 4] where gpu.device.supportsTextureSampleCount(samples) {
                let name = "\(format.name)_\(samples)x"
                journal.starting(name)
                let colors = try sampleColors(gpu, format, s: nil)
                let pipeline = try gpu.render(
                    fragment: "x1_sample_fragment", format: format.pixelFormat, samples: samples)
                for arm in ["twod", "draw"] where arm == "twod" || format.name != "rgb10a2" {
                    let ms = try gpu.texture(
                        format.pixelFormat, w, h, usage: [.renderTarget], samples: samples,
                        storage: .memoryless)
                    let into = try gpu.texture(format.pixelFormat, w, h, usage: [.renderTarget, .shaderRead])
                    let batch = try await gpu.run(forms: "resolve=\(arm)") { commands in
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = ms
                        pass.colorAttachments[0].resolveTexture = into
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].storeAction = .multisampleResolve
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(pipeline)
                        encoder.setFragmentBuffer(colors, offset: 0, index: 0)
                        var corners = GPU.fullScreen
                        encoder.setVertexBytes(&corners, length: 64, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                    if arm == "twod" { ops += batch.taken["resolves"] ?? 0 }
                    let bytes = try await gpu.read(into, bytesPerPixel: format.bytesPerTexel)
                    var tally = X1RuleTally(), codesTally = X1RuleTally()
                    for t in 0..<w * h {
                        let set = (t % w + 3 * (t / w)) % 4
                        let got = format.codes(bytes, t)
                        tally.add(
                            got, exact: average(format, stored[set], samples, srgbLinear: true),
                            channels: format.channels, metal: format.metalRounding, at: t)
                        codesTally.add(
                            got, exact: average(format, stored[set], samples, srgbLinear: false),
                            channels: format.channels, metal: format.metalRounding, at: t)
                    }
                    if arm == "twod" {
                        cells.append((name, tally))
                        if format.channels.contains(.srgb8) { linear.append("\(name):\(codesTally.text)") }
                    } else {
                        draws.append((name, tally))
                    }
                }
            }
        }
        var finding = finding("resolve.twod", cells, ops: ops, paths: gpu.paths.paths.keys.map { $0 })
        finding.metrics.set("srgb_codes_cells", linear.isEmpty ? "-" : linear.joined(separator: ","))
        finding.metrics.set("draw_rules", ruleSummary(draws))
        finding.metrics.set("draw_cells", draws.map { "\($0.0):\($0.1.text)" }.joined(separator: ","))
        return finding
    }

    /// Per pixel class, per sample, the codes the format stores for that sample's colour (a one-sample pass each)
    static func storedSamples(_ gpu: GPU, _ format: X1Format) async throws -> [[[UInt32]]] {
        let pipeline = try gpu.render(fragment: "x1_sample_fragment", format: format.pixelFormat)
        var out = [[[UInt32]]](repeating: [[UInt32]](repeating: [], count: 4), count: 4)
        for s in 0..<4 {
            let target = try gpu.texture(format.pixelFormat, 4, 1, usage: [.renderTarget, .shaderRead])
            let colors = try sampleColors(gpu, format, s: s)
            try await gpu.run { commands in
                let encoder = commands.makeRenderCommandEncoder(descriptor: X1RawProbes.pass(target, load: .clear))!
                encoder.setRenderPipelineState(pipeline)
                encoder.setFragmentBuffer(colors, offset: 0, index: 0)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: 64, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            let bytes = try await gpu.read(target, bytesPerPixel: format.bytesPerTexel)
            for set in 0..<4 { out[set][s] = format.codes(bytes, set) }
        }
        return out
    }

    /// x1_sample_fragment's colours: class set's sample s at set * 4 + s; with `s`, sample s's colour in every slot
    static func sampleColors(_ gpu: GPU, _ format: X1Format, s only: Int?) throws -> MTLBuffer {
        var colors: [SIMD4<Float>] = []
        for set in 0..<4 {
            for s in 0..<4 {
                var v = SIMD4<Float>(0, 0, 0, 1)
                for (c, channel) in format.channels.enumerated() {
                    let code = sampleCode(channel, set: set, c: c, s: only ?? s)
                    v[c] = Float(channel == .srgb8 ? X1Channel.linear(channel.value(code)) : channel.value(code))
                }
                colors.append(v)
            }
        }
        return try gpu.buffer(colors)
    }

    /// The exact average of the first `samples` samples' values per channel; sRGB colour channels averaged as linear
    /// light and encoded again (Metal's resolve) or as codes
    static func average(_ format: X1Format, _ stored: [[UInt32]], _ samples: Int, srgbLinear: Bool) -> [Double] {
        format.channels.enumerated().map { c, channel in
            let values = (0..<samples).map { channel.value(stored[$0][c]) }
            guard channel == .srgb8 && srgbLinear else { return values.reduce(0, +) / Double(samples) }
            return X1Channel.encoded(values.map(X1Channel.linear).reduce(0, +) / Double(samples))
        }
    }
}
