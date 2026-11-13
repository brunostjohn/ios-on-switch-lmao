import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.compressed_targets' ZBC arms (drivers/TegraGM20BMetal/raw/runtime/raw_zbc.h zbcColorEntry, zbcDepthEntry): a
/// one-sample memoryless target of each format (compressed memory once the facade has made some) cleared again and
/// again to a value the boot table lacks, its texels read back by a framebuffer-fetching draw into a private
/// RGBA32Float target; for Depth16, an EQUAL-tested quad at the clear depth over a private BGRA8 target. The first
/// round's clear writes texels (the facade asks for the entry at the third), so every later round must read back the
/// first round's bits: an entry's L2 texel is what the ROP writes for the value. `zbc_<arm>=<first round texels off the
/// value>:<last round texels off the first>:<ZBC hits>`; FAIL on either count or on no hit.
enum CompressedZbcArms {
    static let width = 256, height = 128, rounds = 12

    struct Arm {
        let name: String
        let format: MTLPixelFormat
        let clear: [Double]
        let want: [Float]
        let tolerance: Float
    }

    static func linear(_ code: Double) -> Double { X1Channel.linear(code / 255) }

    static func unorm(_ codes: [Double], _ max: [Double]) -> [Double] { zip(codes, max).map { $0 / $1 } }

    static let srgb = [linear(51), linear(102), linear(204), 1]

    static let arms: [Arm] = [
        Arm(name: "bgra8srgb", format: .bgra8Unorm_srgb, clear: srgb, want: srgb.map(Float.init), tolerance: 2e-3),
        Arm(name: "rgba8srgb", format: .rgba8Unorm_srgb, clear: srgb, want: srgb.map(Float.init), tolerance: 2e-3),
        Arm(
            name: "rgb10a2", format: .rgb10a2Unorm, clear: unorm([300, 600, 900, 2], [1023, 1023, 1023, 3]),
            want: unorm([300, 600, 900, 2], [1023, 1023, 1023, 3]).map(Float.init), tolerance: 1e-4),
        Arm(
            name: "rgba16unorm", format: .rgba16Unorm,
            clear: unorm([10000, 20000, 40000, 65535], [65535, 65535, 65535, 65535]),
            want: unorm([10000, 20000, 40000, 65535], [65535, 65535, 65535, 65535]).map(Float.init), tolerance: 1e-6),
        Arm(
            name: "rgba32f", format: .rgba32Float, clear: [0.3, -1.5, 7.25, 0.5], want: [0.3, -1.5, 7.25, 0.5],
            tolerance: 0),
    ]

    static let depth16 = Float(39321) / 65535

    static func run(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        for arm in arms {
            let before = RawFacade.compression(of: gpu.device)
            let (first, last) = try await colourRounds(gpu, arm)
            let hits = (RawFacade.compression(of: gpu.device)["zbc_hits"] ?? 0) - (before["zbc_hits"] ?? 0)
            var firstOff = 0, lastOff = 0
            first.withUnsafeBytes { f in
                last.withUnsafeBytes { l in
                    let a = f.bindMemory(to: Float.self), b = l.bindMemory(to: Float.self)
                    for t in 0..<width * height {
                        if (0..<4).contains(where: { abs(a[t * 4 + $0] - arm.want[$0]) > arm.tolerance }) {
                            firstOff += 1
                        }
                        if (0..<4).contains(where: { a[t * 4 + $0].bitPattern != b[t * 4 + $0].bitPattern }) {
                            lastOff += 1
                        }
                    }
                }
            }
            judge(arm.name, firstOff, lastOff, hits, &metrics, &wrong)
        }
        let before = RawFacade.compression(of: gpu.device)
        let (first, last) = try await depthRounds(gpu)
        let hits = (RawFacade.compression(of: gpu.device)["zbc_hits"] ?? 0) - (before["zbc_hits"] ?? 0)
        judge("depth16", unwritten(first), unwritten(last), hits, &metrics, &wrong)
    }

    static func judge(
        _ name: String, _ firstOff: Int, _ lastOff: Int, _ hits: Int, _ metrics: inout Metrics,
        _ wrong: inout [String]
    ) {
        metrics.set("zbc_\(name)", "\(firstOff):\(lastOff):\(hits)")
        if firstOff > 0 { wrong.append("zbc_\(name)_first") }
        if lastOff > 0 { wrong.append("zbc_\(name)_held_texel") }
        if hits == 0 { wrong.append("zbc_\(name)_no_hit") }
    }

    /// The fetched copy after the first round and after the last
    static func colourRounds(_ gpu: GPU, _ arm: Arm) async throws -> ([UInt8], [UInt8]) {
        let target = try gpu.texture(arm.format, width, height, usage: [.renderTarget], storage: .memoryless)
        let copy = try gpu.texture(.rgba32Float, width, height, usage: [.renderTarget, .shaderRead])
        let state = try gpu.render(fragment: "zbc_fetch_copy_fragment", format: arm.format) { d in
            d.colorAttachments[0].writeMask = []
            d.colorAttachments[1].pixelFormat = .rgba32Float
        }
        var first: [UInt8] = []
        for round in 0..<rounds {
            _ = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(
                    red: arm.clear[0], green: arm.clear[1], blue: arm.clear[2], alpha: arm.clear[3])
                pass.colorAttachments[0].storeAction = .dontCare
                pass.colorAttachments[1].texture = copy
                pass.colorAttachments[1].loadAction = .dontCare
                pass.colorAttachments[1].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }
            if round == 0 { first = try await gpu.read(copy, bytesPerPixel: 16) }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        return (first, try await gpu.read(copy, bytesPerPixel: 16))
    }

    /// The BGRA8 target after the first round and after the last: white where the quad passed EQUAL at the clear depth
    static func depthRounds(_ gpu: GPU) async throws -> ([UInt8], [UInt8]) {
        let colour = try gpu.texture(.bgra8Unorm, width, height, usage: [.renderTarget, .shaderRead])
        let depth = try gpu.texture(.depth16Unorm, width, height, usage: [.renderTarget], storage: .memoryless)
        let state = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth16Unorm)
        let equal = gpu.device.makeDepthStencilState(
            descriptor: {
                let d = MTLDepthStencilDescriptor()
                d.depthCompareFunction = .equal
                d.isDepthWriteEnabled = false
                return d
            }())!
        var first: [UInt8] = []
        for round in 0..<rounds {
            _ = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = colour
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = Double(depth16)
                pass.depthAttachment.storeAction = .dontCare
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(equal)
                GPU.draw(
                    e, corners: GPU.pixelQuad(0, 0, width, height, width: width, height: height, z: depth16),
                    color: SIMD4<Float>(1, 1, 1, 1))
                e.endEncoding()
            }
            if round == 0 { first = try await gpu.read(colour, bytesPerPixel: 4) }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        return (first, try await gpu.read(colour, bytesPerPixel: 4))
    }

    static func unwritten(_ bytes: [UInt8]) -> Int {
        stride(from: 0, to: bytes.count, by: 4).filter { bytes[$0..<$0 + 4] != [255, 255, 255, 255] }.count
    }
}
