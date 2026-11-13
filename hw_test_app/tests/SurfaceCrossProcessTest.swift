import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal
import SwitchTexturePattern

/// One process renders into an IOSurface, another reads it: the texture peer (peer/texpeer.mm) renders a pattern whose
/// every GOB byte differs into a BGRA surface on the GPU, and the app reads it through a CPU lock and through a GPU
/// import of its own. Every reader must see rows the way the writer wrote them, whatever layout the kernel decided
/// (drivers/TegraGM20B/TegraGM20BTiled.cpp). Four orders: the peer makes and renders the surface and keeps its texture
/// while the app reads (block-linear, a held import); the same with the peer's texture gone first (the deferred untile);
/// the app samples a surface of its own first, while nothing has written it (block-linear), before the peer renders it;
/// and the same sampled surface written by the app's CPU under a write lock instead, whose lock and unlock move its
/// texels on the copy engine (`cpu<size>_write_us`; `<case>_read_us` times each read lock); and a surface the app's CPU
/// wrote with no lock (its seed still 0) before it sampled it first, whose rows must survive its tiling. The app-made
/// surfaces must read block-linear (`<case>_layout=1`). Two shapes: 256x256 and
/// the 260x260 of the home screen's large widget. A read that matches the pattern laid out in GOBs reports
/// `<case>_gob=1`: block-linear texels seen as rows (the Maps widget's garbage on boot 21).
struct SurfaceCrossProcessTest: HWTest {
    let id = "gpu.iosurface.cross_process"
    let name = "An IOSurface one process renders reads as rows in another, by CPU lock and by GPU import"
    let subsystem = Subsystem.metal

    static let sizes = [256, 260]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else { return .skip("the default device is not the raw facade") }
        let acceptor = TexturePeerAcceptor()
        let listener = NSXPCListener.anonymous()
        listener.delegate = acceptor
        listener.resume()
        defer {
            acceptor.connection?.invalidate()
            listener.invalidate()
        }
        let endpoint = listener.endpoint
        if let why = await NativeSharedTexturePeerTest.offThread({ NativeSharedTexturePeerTest.startPeer(endpoint) }) {
            throw TestFailure("no texture peer: \(why)")
        }
        let connected = await NativeSharedTexturePeerTest.offThread {
            acceptor.accepted.wait(timeout: .now() + 20) == .success
        }
        guard connected, let connection = acceptor.connection else {
            throw TestFailure("the texture peer never connected")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for size in Self.sizes {
            for order in ["held", "dropped", "sampled", "cpu", "unlocked"] {
                let label = "\(order)\(size)"
                guard let (surface, sampler) = try await Self.rendered(gpu, connection, order, size, &metrics)
                else {
                    checks.append(("\(label): the peer rendered the surface", false))
                    continue
                }
                if let gpuConnection = GPUConnection() {
                    let (kr, layout) = gpuConnection.surfaceLayout(surface)
                    metrics.set("\(label)_layout", kr == KERN_SUCCESS ? String(layout) : GPUConnection.hex(kr))
                    if order != "dropped" {
                        checks.append(("\(label): block-linear", kr == KERN_SUCCESS && layout == 1))
                    }
                }
                let (cpu, readUs) = Self.cpuRows(surface)
                metrics.set("\(label)_read_us", readUs)
                let (gpuRows, _) = try await Self.gpuRows(gpu, surface)
                withExtendedLifetime(sampler) {}
                for (reader, got) in [("cpu", cpu), ("gpu", gpuRows)] {
                    let (wrong, first) = Self.wrong(got, surface)
                    metrics.set("\(label)_\(reader)_wrong", wrong)
                    metrics.set("\(label)_\(reader)_first", first)
                    metrics.set("\(label)_\(reader)_gob", Self.readAsGobs(got, surface) ? 1 : 0)
                    checks.append(
                        ("\(label): the \(reader) read is the peer's rows (\(wrong) bytes differ)", wrong == 0))
                }
                if order == "held" { _ = await Self.ask(connection) { $0.dropSurfaces($1) } }
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The surface, rendered by the peer in `order`, with the texture through which the app sampled it first ("sampled":
    /// held until the reads, so the pitch decision stands), or nil when the peer did not answer ok
    static func rendered(
        _ gpu: GPU, _ connection: NSXPCConnection, _ order: String, _ size: Int, _ metrics: inout Metrics
    ) async throws
        -> (IOSurface, MTLTexture?)?
    {
        if order == "held" || order == "dropped" {
            let made = HandlerValue<IOSurface?>(nil)
            let answer = await ask(connection) { peer, reply in
                peer.makeSurfaceWidth(size, height: size, keep: order == "held") {
                    made.value = $0
                    reply($1)
                }
            }
            guard answer?.hasPrefix("ok ") == true, let surface = made.value else { return nil }
            return (surface, nil)
        }
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: size, .height: size, .bytesPerElement: 4, .pixelFormat: UInt32(0x4247_5241),
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface \(size) not made") }
        if order == "unlocked" { write(surface) }
        let (_, sampler) = try await gpuRows(gpu, surface)
        if order == "unlocked" { return (surface, sampler) }
        if order == "cpu" {
            metrics.set("cpu\(size)_write_us", cpuWrite(surface))
            return (surface, sampler)
        }
        let answer = await ask(connection) { $0.renderSurface(surface, keep: false, reply: $1) }
        return answer?.hasPrefix("ok ") == true ? (surface, sampler) : nil
    }

    static func ask(
        _ connection: NSXPCConnection, _ call: @escaping (SharedTexturePeer, @escaping (String) -> Void) -> Void
    ) async -> String? {
        let answer = HandlerValue<String?>(nil)
        let done = DispatchSemaphore(value: 0)
        guard let peer = connection.remoteObjectProxyWithErrorHandler({ _ in done.signal() }) as? SharedTexturePeer
        else { return nil }
        call(peer) {
            answer.value = $0
            done.signal()
        }
        let arrived = await NativeSharedTexturePeerTest.offThread { done.wait(timeout: .now() + 20) == .success }
        return arrived ? answer.value : nil
    }

    /// Its texels as rows, through a read-only CPU lock, and the microseconds from the lock to the unlock's return
    static func cpuRows(_ surface: IOSurface) -> ([UInt8], Int) {
        let start = DispatchTime.now().uptimeNanoseconds
        surface.lock(options: .readOnly, seed: nil)
        var out = [UInt8](repeating: 0, count: surface.width * surface.height * 4)
        for y in 0..<surface.height {
            out.withUnsafeMutableBytes {
                $0.baseAddress!.advanced(by: y * surface.width * 4).copyMemory(
                    from: surface.baseAddress + y * surface.bytesPerRow, byteCount: surface.width * 4)
            }
        }
        surface.unlock(options: .readOnly, seed: nil)
        return (out, Int((DispatchTime.now().uptimeNanoseconds - start) / 1000))
    }

    /// The pattern into every texel through a CPU write lock; the microseconds from the lock to the unlock's return
    static func cpuWrite(_ surface: IOSurface) -> Int {
        let start = DispatchTime.now().uptimeNanoseconds
        surface.lock(options: [], seed: nil)
        write(surface)
        surface.unlock(options: [], seed: nil)
        return Int((DispatchTime.now().uptimeNanoseconds - start) / 1000)
    }

    /// The pattern into every texel through the surface's own mapping, with or without a lock around it
    static func write(_ surface: IOSurface) {
        for y in 0..<surface.height {
            let row = surface.baseAddress + y * surface.bytesPerRow
            for x in 0..<surface.width {
                for c in 0..<4 { row.storeBytes(of: byte(x, y, c), toByteOffset: x * 4 + c, as: UInt8.self) }
            }
        }
    }

    /// Its texels as rows, through this process's own GPU import of it (a blit into a buffer), and that texture
    static func gpuRows(_ gpu: GPU, _ surface: IOSurface) async throws -> ([UInt8], MTLTexture) {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: surface.width, height: surface.height, mipmapped: false)
        d.usage = .shaderRead
        d.storageMode = .shared
        guard
            let t = gpu.device.makeTexture(
                descriptor: d, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("no texture over IOSurface \(surface.width)") }
        let row = surface.width * 4
        let out = try gpu.buffer(row * surface.height)
        _ = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: t, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: surface.width, height: surface.height, depth: 1), to: out,
                destinationOffset: 0, destinationBytesPerRow: row, destinationBytesPerImage: 0)
            blit.endEncoding()
        }
        return ([UInt8](UnsafeRawBufferPointer(start: out.contents(), count: row * surface.height)), t)
    }

    /// Bytes that differ from the pattern, and the first one's offset in the rows (-1: none)
    static func wrong(_ got: [UInt8], _ surface: IOSurface) -> (Int, Int) {
        var wrong = 0, first = -1
        for y in 0..<surface.height {
            for x in 0..<surface.width {
                for c in 0..<4 where got[(y * surface.width + x) * 4 + c] != byte(x, y, c) {
                    wrong += 1
                    if first < 0 { first = (y * surface.width + x) * 4 + c }
                }
            }
        }
        return (wrong, first)
    }

    static func byte(_ x: Int, _ y: Int, _ c: Int) -> UInt8 {
        switch_texpeer_surface_byte(UInt32(x), UInt32(y), UInt32(c))
    }

    /// Whether the first rows read are the pattern laid out block-linear (drivers/TegraGM20B/TegraGM20BLayout.h at
    /// NIL's block height) and read back as rows of the surface's pitch
    static func readAsGobs(_ got: [UInt8], _ surface: IOSurface) -> Bool {
        let rowBytes = (surface.width * 4 + 63) & ~63
        let yLog2 = blockHeightLog2(surface.height)
        let pitch = surface.bytesPerRow
        var tiled = [UInt8](repeating: 0, count: pitch * surface.height + 64 * 8 * 16)
        for y in 0..<surface.height {
            for xB in 0..<(surface.width * 4) {
                let at = gobOffset(rowBytes, yLog2, xB, y)
                if at < tiled.count { tiled[at] = byte(xB / 4, y, xB % 4) }
            }
        }
        let rows = min(8, surface.height)
        for y in 0..<rows {
            for xB in 0..<(surface.width * 4) where got[y * surface.width * 4 + xB] != tiled[y * pitch + xB] {
                return false
            }
        }
        return true
    }

    static func blockHeightLog2(_ height: Int) -> Int {
        let y = height >= 86 ? 4 : height >= 43 ? 3 : height >= 22 ? 2 : height >= 11 ? 1 : 0
        var clamp = 0
        while (1 << clamp) < (height + 7) / 8 { clamp += 1 }
        return min(y, clamp)
    }

    static func gobOffset(_ rowBytes: Int, _ yLog2: Int, _ xB: Int, _ y: Int) -> Int {
        let blockRows = 8 << yLog2
        let block = (y / blockRows) * (rowBytes / 64) + xB / 64
        let inGob =
            ((xB % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xB % 32) / 16) * 32 + (y % 2) * 16 + (xB % 16)
        return block * (512 << yLog2) + ((y % blockRows) / 8) * 512 + inGob
    }
}
