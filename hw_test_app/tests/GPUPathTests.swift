import Darwin
import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import HWTestGPU
import HWTestVideo
import Metal
import VideoToolbox

/// kern.switch_gpu_paths (debug kernels, drivers/TegraGM20B/SwitchGpuPathStats.h): the capability flags the GM20B
/// kernel advertises and how often this process took each path behind them.
struct GPUPaths {
    let flags: UInt32
    let counts: [String: Int]

    static func read() -> GPUPaths? {
        var size = 0
        guard sysctlbyname("kern.switch_gpu_paths", nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var bytes = [CChar](repeating: 0, count: size)
        guard sysctlbyname("kern.switch_gpu_paths", &bytes, &size, nil, 0) == 0 else { return nil }
        var flags: UInt32 = 0
        var counts: [String: Int] = [:]
        for field in String(cString: bytes).split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1).map(String.init)
            guard pair.count == 2 else { continue }
            if pair[0] == "flags" {
                flags = UInt32(pair[1].dropFirst(2), radix: 16) ?? 0
            } else if let count = Int(pair[1]) {
                counts[pair[0]] = count
            }
        }
        return GPUPaths(flags: flags, counts: counts)
    }

    func taken(_ name: String, since before: GPUPaths) -> Int { (counts[name] ?? 0) - (before.counts[name] ?? 0) }
}

/// One GM20B kernel path behind a capability bit, on the raw facade: the kernel advertises the bit, a workload that
/// needs the path gives the right bytes with every command buffer on the raw path, and the kernel counted the path for
/// this process while it ran.
protocol GPUPathTest: HWTest {
    /// The capability bit that still gates the path, if any
    var bit: Int? { get }
    var counters: [String] { get }
    /// The workload's failures, none when it gave the right bytes.
    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String]
}

extension GPUPathTest {
    var subsystem: Subsystem { .metal }
    var bit: Int? { nil }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        guard let before = GPUPaths.read() else {
            return RawFacade.counters(of: gpu.device).isEmpty
                ? .skip("no kern.switch_gpu_paths: not a debug kernel")
                : .fail("a debug runtime on a kernel without kern.switch_gpu_paths")
        }
        var metrics = Metrics()
        metrics.set("flags", String(before.flags, radix: 16))
        var failures: [String] = []
        if let bit, before.flags & (1 << UInt32(bit)) == 0 {
            failures.append("the kernel does not advertise bit \(bit)")
        }
        failures += try await exercise(gpu, context, &metrics)
        let after = GPUPaths.read() ?? before
        for counter in counters {
            let taken = after.taken(counter, since: before)
            metrics.set(counter, taken)
            if taken <= 0 { failures.append("no \(counter) while it ran") }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        if gpu.paths.paths.keys.contains(where: { $0 != "raw" }) {
            failures.append("command buffers went \(gpu.paths.text)")
        }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }

    static func wrong(_ got: [UInt8], _ want: [UInt8]) -> Int {
        zip(got, want).filter { $0 != $1 }.count + abs(got.count - want.count)
    }
}

/// JobVisibility (bit 18): a pass that writes only a private texture ends Internal, with no L2 flush; the blit into a
/// shared buffer that reads it back ends External, and the CPU reads the pass's colour from it.
struct JobVisibilityPathTest: GPUPathTest {
    let id = "gpu.path.job_visibility"
    let name = "Internal jobs skip the L2 flush, External ones put CPU-read bytes in DRAM"
    let counters = ["internal_jobs", "external_jobs"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let target = try gpu.texture(.rgba8Unorm, 64, 64)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = Self.wrong(bytes, Array([UInt8]([51, 102, 153, 255]).repeated(64 * 64)))
        metrics.set("wrong_bytes", wrong)
        return wrong == 0 ? [] : ["\(wrong) bytes of the cleared texture read back wrong"]
    }
}

/// LeanCopyChannel (bit 19): replaceRegion and getBytes of a texture go through the process's copy channel, made
/// copy-only, and the bytes round-trip.
struct LeanCopyPathTest: GPUPathTest {
    let id = "gpu.path.lean_copy"
    let name = "Texture uploads and readbacks run on a copy-only channel"
    let counters = ["lean_copy_jobs"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let width = 160, height = 48, row = width * 4
        let texture = try gpu.texture(.rgba8Unorm, width, height, usage: [.shaderRead], storage: .shared)
        let pattern = (0..<(row * height)).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) }
        let region = MTLRegionMake2D(0, 0, width, height)
        let before = RawFacade.counters(of: gpu.device)
        texture.replace(region: region, mipmapLevel: 0, withBytes: pattern, bytesPerRow: row)
        var back = [UInt8](repeating: 0, count: pattern.count)
        texture.getBytes(&back, bytesPerRow: row, from: region, mipmapLevel: 0)
        let after = RawFacade.counters(of: gpu.device)
        let uploads = (after["texture_uploads"] ?? 0) - (before["texture_uploads"] ?? 0)
        let readbacks = (after["texture_readbacks"] ?? 0) - (before["texture_readbacks"] ?? 0)
        let wrong = Self.wrong(back, pattern)
        metrics.set("uploads", uploads)
        metrics.set("readbacks", readbacks)
        metrics.set("wrong_bytes", wrong)
        var failures: [String] = []
        if uploads <= 0 || readbacks <= 0 { failures.append("the copy channel did not move the texture") }
        if wrong != 0 { failures.append("\(wrong) bytes changed on the round trip") }
        return failures
    }
}

/// ControlReserve (bit 20): one command buffer with more setBytes than any before it makes the runtime allocate new
/// control buffers from the kernel's reserve; every draw's colour comes from its own setBytes.
struct ControlReservePathTest: GPUPathTest {
    let id = "gpu.path.control_reserve"
    let name = "Control buffers come from the reserve and carry every draw's bytes"
    let counters = ["control_bos"]
    static let width = 64, height = 8, bytesPerDraw = 4096

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let width = Self.width, height = Self.height
        let target = try gpu.texture(.rgba8Unorm, width, height)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        var want = [UInt8](repeating: 0, count: width * height * 4)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var payload = [Float](repeating: 0, count: Self.bytesPerDraw / 4)
            for y in 0..<height {
                for x in 0..<width {
                    let value = UInt8((x * 3 + y * 29) & 0xff)
                    payload[0] = Float(value) / 255
                    payload[1] = Float(255 - value) / 255
                    payload[2] = Float(y) / 255
                    payload[3] = 1
                    var corners = GPU.pixelQuad(x, y, x + 1, y + 1, width: width, height: height)
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBytes(&payload, length: Self.bytesPerDraw, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    let at = (y * width + x) * 4
                    want[at..<(at + 4)] = [value, 255 - value, UInt8(y), 255]
                }
            }
            encoder.endEncoding()
        }
        let wrong = Self.wrong(try await gpu.read(target, bytesPerPixel: 4), want)
        metrics.set("draws", width * height)
        metrics.set("wrong_bytes", wrong)
        return wrong == 0 ? [] : ["\(wrong) bytes of the draws' colours wrong"]
    }
}

/// CpuComplete (bit 24): a GPU fill of no-copy cached memory is maintained by the kernel over exactly its range at
/// completion, so the original pointer reads it without another contents call.
struct CpuCompletePathTest: GPUPathTest {
    let id = "gpu.path.cpu_complete"
    let name = "The kernel maintains a GPU write's cached range at completion"
    let counters = ["cpu_completes"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let length = 3 * Int(getpagesize())
        var allocation: UnsafeMutableRawPointer?
        guard posix_memalign(&allocation, Int(getpagesize()), length) == 0, let memory = allocation else {
            throw TestFailure("aligned no-copy allocation failed")
        }
        memory.initializeMemory(as: UInt8.self, repeating: 0x11, count: length)
        guard let buffer = gpu.device.makeBuffer(
            bytesNoCopy: memory, length: length, options: .storageModeShared,
            deallocator: { pointer, _ in free(pointer) })
        else {
            free(memory)
            throw TestFailure("no-copy buffer creation failed")
        }
        let range = 100..<(length - 300)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: buffer, range: range, value: 0x6b)
            blit.endEncoding()
        }
        var wrong = 0
        for offset in 0..<length {
            let want: UInt8 = range.contains(offset) ? 0x6b : 0x11
            if memory.load(fromByteOffset: offset, as: UInt8.self) != want { wrong += 1 }
        }
        metrics.set("wrong_bytes", wrong)
        return wrong == 0 ? [] : ["\(wrong) bytes wrong through the original pointer"]
    }
}

/// UseRanges (bit 25): small buffers share pool chunks, and each command buffer names only its buffer's bytes; the GPU
/// writes each buffer while the CPU rewrites its neighbours, and no buffer loses either side's bytes.
struct UseRangesPathTest: GPUPathTest {
    let id = "gpu.path.use_ranges"
    let name = "Uses name byte ranges, so buffers sharing a chunk keep their own writes"
    let bit: Int? = 25
    let counters = ["ranged_uses"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let length = 768
        let buffers = try (0..<8).map { _ in try gpu.buffer(length) }
        for (index, buffer) in buffers.enumerated() {
            buffer.contents().initializeMemory(as: UInt8.self, repeating: UInt8(index), count: length)
        }
        var committed: [MTLCommandBuffer] = []
        for (index, buffer) in buffers.enumerated() where index % 2 == 0 {
            committed.append(
                try gpu.commit { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.fill(buffer: buffer, range: 0..<length, value: UInt8(0x80 | index))
                    blit.endEncoding()
                })
            buffers[index + 1].contents().initializeMemory(as: UInt8.self, repeating: UInt8(0x40 | index), count: length)
        }
        try gpu.finish(committed)
        var wrong = 0
        for (index, buffer) in buffers.enumerated() {
            let want = UInt8(index % 2 == 0 ? 0x80 | index : 0x40 | (index - 1))
            let bytes = UnsafeRawBufferPointer(start: buffer.contents(), count: length)
            wrong += bytes.filter { $0 != want }.count
        }
        metrics.set("wrong_bytes", wrong)
        return wrong == 0 ? [] : ["\(wrong) bytes wrong across buffers that share chunks"]
    }
}

/// NativeSurfaceView (bit 17): a hardware-decoded frame's planes, read by a compute kernel through CVMetalTextureCache,
/// come from the decoder's own block-linear picture through a native view, and give the bytes a CPU lock of the frame
/// converts to rows. The GPU reads both planes before the CPU's lock prepares the rows; a blit would read the pitch
/// planes instead.
struct NativeSurfaceViewPathTest: GPUPathTest {
    let id = "gpu.path.native_surface_view"
    let name = "Draws sample a decoded picture where the decoder put it, matching its CPU rows"
    let counters = ["native_tiled_views", "native_reads"]

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 720 }) else {
            throw TestFailure("no 720p H.264 clip")
        }
        let frame = try Self.decodeFirst(try await Demux.load(clip, from: context.media), clip)
        var made: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(nil, nil, gpu.device, nil, &made) == kCVReturnSuccess, let cache = made else {
            throw TestFailure("CVMetalTextureCacheCreate failed")
        }
        let planes: [(format: MTLPixelFormat, channels: Int)] = [(.r8Unorm, 1), (.rg8Unorm, 2)]
        var sampled: [[UInt8]] = []
        var held: [CVMetalTexture] = []
        for (plane, shape) in planes.enumerated() {
            var texture: CVMetalTexture?
            guard CVMetalTextureCacheCreateTextureFromImage(
                nil, cache, frame, nil, shape.format, CVPixelBufferGetWidthOfPlane(frame, plane),
                CVPixelBufferGetHeightOfPlane(frame, plane), plane, &texture) == kCVReturnSuccess,
                let texture, let metal = CVMetalTextureGetTexture(texture)
            else { throw TestFailure("no Metal texture of plane \(plane)") }
            held.append(texture)
            let texels = try await gpu.texels(metal)
            sampled.append(texels.flatMap { texel in (0..<shape.channels).map { UInt8((texel[$0] * 255).rounded()) } })
        }
        var failures: [String] = []
        for (plane, shape) in planes.enumerated() {
            let wrong = Self.wrong(sampled[plane], Self.rows(frame, plane: plane, bytesPerPixel: shape.channels))
            metrics.set("plane\(plane)_wrong_bytes", wrong)
            if wrong != 0 { failures.append("plane \(plane): \(wrong) bytes differ from the CPU's rows") }
        }
        metrics.set("textures", held.count)
        return failures
    }

    final class Decoded: @unchecked Sendable {
        var image: CVImageBuffer?
        var status: OSStatus = noErr
    }

    static func decodeFirst(_ demuxed: Demuxed, _ clip: Clip) throws -> CVPixelBuffer {
        let specification: [CFString: Any] = [
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: true
        ]
        let destination = PatternPixels.attributes(
            width: clip.width, height: clip.height, format: PatternPixels.format(bitDepth: 8))
        var made: VTDecompressionSession?
        let created = VTDecompressionSessionCreate(
            allocator: nil, formatDescription: demuxed.format, decoderSpecification: specification as CFDictionary,
            imageBufferAttributes: destination as CFDictionary, outputCallback: nil, decompressionSessionOut: &made)
        guard created == noErr, let session = made, let first = demuxed.samples.first else {
            throw TestFailure("VTDecompressionSessionCreate \(created)")
        }
        defer { VTDecompressionSessionInvalidate(session) }
        let decoded = Decoded()
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: first, flags: [], infoFlagsOut: nil) {
            status, _, image, _, _ in
            decoded.status = status
            decoded.image = image
        }
        _ = VTDecompressionSessionWaitForAsynchronousFrames(session)
        guard status == noErr, decoded.status == noErr, let image = decoded.image else {
            throw TestFailure("decoding the first frame failed (\(status), \(decoded.status))")
        }
        return image
    }

    /// A plane as a CPU lock gives it, rows packed
    static func rows(_ frame: CVPixelBuffer, plane: Int, bytesPerPixel: Int) -> [UInt8] {
        CVPixelBufferLockBaseAddress(frame, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(frame, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(frame, plane) else { return [] }
        let stride = CVPixelBufferGetBytesPerRowOfPlane(frame, plane)
        let rowBytes = CVPixelBufferGetWidthOfPlane(frame, plane) * bytesPerPixel
        return (0..<CVPixelBufferGetHeightOfPlane(frame, plane)).flatMap { y in
            [UInt8](UnsafeRawBufferPointer(start: base + y * stride, count: rowBytes))
        }
    }
}

/// SemanticFence (bit 22): the runtime reads every command buffer's completion through the fence record's semantic half
/// as well as its seqno; 48 command buffers back to back each fill their own slice and complete, and none reads as failed
/// or makes the raw channel again, as a failure misread as a prefix of later work would.
struct SemanticFencePathTest: GPUPathTest {
    let id = "gpu.path.semantic_fence"
    let name = "Completions read the fence record's semantic state, and no failure spills onto later work"
    let bit: Int? = 22
    let counters: [String] = []
    static let count = 48, slice = 256

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let buffer = try gpu.buffer(Self.count * Self.slice)
        let before = RawFacade.counters(of: gpu.device)
        var committed: [MTLCommandBuffer] = []
        for index in 0..<Self.count {
            committed.append(
                try gpu.commit { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.fill(
                        buffer: buffer, range: (index * Self.slice)..<((index + 1) * Self.slice),
                        value: UInt8(index + 1))
                    blit.endEncoding()
                })
        }
        try gpu.finish(committed)
        let reopens = (RawFacade.counters(of: gpu.device)["reopens"] ?? 0) - (before["reopens"] ?? 0)
        let bytes = UnsafeRawBufferPointer(start: buffer.contents(), count: Self.count * Self.slice)
        var wrong = 0
        for (offset, byte) in bytes.enumerated() where byte != UInt8(offset / Self.slice + 1) {
            wrong += 1
        }
        metrics.set("reopens", reopens)
        metrics.set("wrong_bytes", wrong)
        var failures: [String] = []
        if reopens != 0 { failures.append("the raw channel was made again \(reopens) times") }
        if wrong != 0 { failures.append("\(wrong) bytes wrong") }
        return failures
    }
}

/// SharedEvents (bit 21): one queue's command buffer waits for a shared event on the GPU before it copies a buffer,
/// another's fills that buffer and signals the event on the GPU; the copy sees the fill, and both jobs carry the event
/// to the kernel.
struct SharedEventsPathTest: GPUPathTest {
    let id = "gpu.path.shared_events"
    let name = "A GPU wait on a shared event orders a copy after another queue's GPU signal"
    let bit: Int? = 21
    let counters = ["event_jobs"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard let event = gpu.device.makeSharedEvent(), let other = gpu.device.makeCommandQueue(),
            let waiter = other.makeCommandBuffer()
        else { throw TestFailure("no shared event or second queue") }
        let length = 4096
        let source = try gpu.buffer(length), target = try gpu.buffer(length)
        waiter.encodeWaitForEvent(event, value: 1)
        let copy = waiter.makeBlitCommandEncoder()!
        copy.copy(from: source, sourceOffset: 0, to: target, destinationOffset: 0, size: length)
        copy.endEncoding()
        waiter.commit()
        let producer = try gpu.commit { commands in
            let fill = commands.makeBlitCommandEncoder()!
            fill.fill(buffer: source, range: 0..<length, value: 0x3c)
            fill.endEncoding()
            commands.encodeSignalEvent(event, value: 1)
        }
        try gpu.finish([producer])
        waiter.waitUntilCompleted()
        var failures: [String] = []
        let waiterPath = RawFacade.path(of: waiter)
        metrics.set("waiter_path", waiterPath)
        if waiterPath != "raw" && waiterPath != "device" { failures.append("the waiting command buffer went \(waiterPath)") }
        if waiter.status != .completed { failures.append("the waiting command buffer ended \(waiter.status.rawValue)") }
        let wrong = UnsafeRawBufferPointer(start: target.contents(), count: length).filter { $0 != 0x3c }.count
        metrics.set("wrong_bytes", wrong)
        if wrong != 0 { failures.append("\(wrong) copied bytes miss the signalled fill") }
        metrics.set("signaled", Int(event.signaledValue))
        return failures
    }
}

/// ResourceReady (bit 23): a command buffer that renders into an IOSurface while the CPU holds its lock is refused by the
/// kernel and parks on a readiness notification instead of failing; it runs once the lock goes, and its pixels are there.
struct ResourceReadyPathTest: GPUPathTest {
    let id = "gpu.path.resource_ready"
    let name = "A render into a CPU-locked IOSurface waits for the unlock instead of failing"
    let bit: Int? = 23
    let counters = ["ready_arms"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let width = 64, height = 64, rowBytes = 256
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * height, .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface creation failed") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture over the IOSurface")
        }
        guard surface.lock(options: [], seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        let render = try gpu.commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        try await Task.sleep(nanoseconds: 100_000_000)
        let heldBack = render.status != .completed && render.status != .error
        surface.unlock(options: [], seed: nil)
        try gpu.finish([render])
        metrics.set("held_back", heldBack)
        guard surface.lock(options: .readOnly, seed: nil) == 0 else {
            throw TestFailure("IOSurfaceLock for the readback failed")
        }
        var wrong = 0
        for y in 0..<height {
            let row = UnsafeRawBufferPointer(start: surface.baseAddress + y * rowBytes, count: width * 4)
            for x in 0..<width where Array(row[(x * 4)..<(x * 4 + 4)]) != [0, 255, 0, 255] { wrong += 1 }
        }
        surface.unlock(options: .readOnly, seed: nil)
        metrics.set("wrong_pixels", wrong)
        var failures: [String] = []
        if !heldBack { failures.append("the render ran while the CPU held the lock") }
        if wrong != 0 { failures.append("\(wrong) pixels miss the clear") }
        return failures
    }
}

enum GPUPathTests {
    static let all: [any HWTest] = [
        JobVisibilityPathTest(), LeanCopyPathTest(), ControlReservePathTest(), CpuCompletePathTest(),
        UseRangesPathTest(), NativeSurfaceViewPathTest(), SemanticFencePathTest(), SharedEventsPathTest(),
        ResourceReadyPathTest(),
    ]
}

extension Array {
    fileprivate func repeated(_ times: Int) -> [Element] { (0..<times).flatMap { _ in self } }
}
