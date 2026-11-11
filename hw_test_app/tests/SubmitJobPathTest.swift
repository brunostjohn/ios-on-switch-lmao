import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// SubmitJob (drivers/TegraGM20B/TegraGM20BJobABI.h): every raw command buffer reaches the kernel as jobs that name the
/// device's resource set at its current generation, so its control memory and rings are never uses of a job. Frames of a
/// clear and a setFragmentBytes draw, each into its own target, read back byte for byte; in the window the kernel ran no
/// SubmitAddress job and refused few jobs for an ended set generation. The counts are the process's, so a copy-channel
/// job (no set) or an NVK submission (SubmitAddress) from another thread of the app shows in them too.
struct SubmitJobPathTest: GPUPathTest {
    let id = "gpu.path.submit_job"
    let name = "Raw command buffers go to the kernel as jobs over the device's resource set"
    let counters = ["submit_jobs", "set_jobs"]
    static let frames = 12, width = 16, height = 16, staleBound = 2

    static func clear(_ frame: Int) -> [UInt8] { [UInt8(frame * 20), UInt8(255 - frame * 20), 0x40, 255] }
    static func drawn(_ frame: Int) -> [UInt8] { [UInt8(17 * frame % 256), 0x80, UInt8(255 - 9 * frame), 255] }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let connection = GPUConnection()
        guard case .counts(let before) = GPUPaths.read(connection) else { throw TestFailure("no path counters") }
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        var committed: [MTLCommandBuffer] = []
        var targets: [MTLTexture] = []
        for frame in 0..<Self.frames {
            let target = try gpu.texture(.rgba8Unorm, Self.width, Self.height)
            targets.append(target)
            committed.append(try gpu.commit { commands in
                let pass = MTLRenderPassDescriptor()
                let c = Self.clear(frame).map { Double($0) / 255 }
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: c[0], green: c[1], blue: c[2], alpha: c[3])
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var colour = Self.drawn(frame).map { Float($0) / 255 }
                var corners = GPU.pixelQuad(0, 0, Self.width / 2, Self.height, width: Self.width, height: Self.height)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBytes(&colour, length: MemoryLayout<Float>.stride * 4, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            })
        }
        try gpu.finish(committed)
        var wrong = 0
        for (frame, target) in targets.enumerated() {
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            for y in 0..<Self.height {
                for x in 0..<Self.width {
                    let at = (y * Self.width + x) * 4
                    let want = x < Self.width / 2 ? Self.drawn(frame) : Self.clear(frame)
                    wrong += Self.wrong(Array(bytes[at..<(at + 4)]), want)
                }
            }
        }
        guard case .counts(let after) = GPUPaths.read(connection) else { throw TestFailure("no path counters after") }
        let jobs = after.taken("submit_jobs", since: before), setJobs = after.taken("set_jobs", since: before)
        let stale = after.taken("stale_sets", since: before), address = after.taken("address_jobs", since: before)
        metrics.set("frames", Self.frames)
        metrics.set("wrong_bytes", wrong)
        metrics.set("stale_sets", stale)
        metrics.set("address_jobs", address)
        metrics.set("set_share", jobs > 0 ? String(format: "%.2f", Double(setJobs) / Double(jobs)) : "-")
        var failures: [String] = []
        if wrong != 0 { failures.append("\(wrong) bytes differ from the clears and draws") }
        if address != 0 { failures.append("\(address) SubmitAddress jobs while it ran") }
        if stale > Self.staleBound {
            failures.append("\(stale) jobs refused for an ended set generation (at most \(Self.staleBound))")
        }
        return failures
    }
}
