import Foundation
import HWTestCore
import Metal
import SwitchBench

/// The bench's raw device (hw_test_app/bench/switch_bench.h): one experiment at a time, on its caller's thread.
final class BenchRunner {
    private let bench: OpaquePointer

    private init(_ bench: OpaquePointer) { self.bench = bench }

    deinit { switch_bench_close(bench) }

    /// The Metal device is made first: it loads the GPU plugin, beside which the bench finds the raw pack.
    static func open() throws -> BenchRunner {
        _ = MTLCreateSystemDefaultDevice()
        var why = [CChar](repeating: 0, count: 256)
        guard let bench = switch_bench_open(&why, why.count) else { throw BenchInvalid(String(cString: why)) }
        return BenchRunner(bench)
    }

    var summary: String {
        var line = [CChar](repeating: 0, count: 512)
        switch_bench_describe(bench, &line, line.count)
        return String(cString: line)
    }

    static var knownData: [UInt32] {
        var words = [UInt32](repeating: 0, count: 4)
        switch_bench_known_data(&words)
        return words
    }

    static var knownKernel: Data {
        var bytes = [UInt8](repeating: 0, count: 256)
        return Data(bytes.prefix(switch_bench_known_kernel(&bytes, bytes.count)))
    }

    static let kinds: [BenchKind: switch_bench_kind] = [
        .noop: SWITCH_BENCH_NOOP, .known: SWITCH_BENCH_KNOWN, .fragment: SWITCH_BENCH_FRAGMENT,
        .compute: SWITCH_BENCH_COMPUTE, .readback: SWITCH_BENCH_READBACK, .twod: SWITCH_BENCH_TWOD,
    ]
    static let statuses = ["ok", "invalid", "refused", "killed", "error"]

    func run(_ e: BenchExperiment) -> BenchOutcome {
        let started = DispatchTime.now().uptimeNanoseconds
        let pinned = Pinned()
        let program = e.program ?? BenchProgram(sph: Data(), code: Data(), data: Data(), gprs: 0)
        var x = switch_bench_experiment()
        x.kind = Self.kinds[e.kind]!.rawValue
        (x.format, x.width, x.height) = (e.formatRaw, e.width, e.height)
        x.rect = (e.rect[0], e.rect[1], e.rect[2], e.rect[3])
        x.clear = (e.clear[0], e.clear[1], e.clear[2], e.clear[3])
        x.fill = e.fill.flatMap(pinned.bytes)
        x.program = switch_bench_program(
            sph: pinned.bytes(program.sph), code: pinned.bytes(program.code), code_bytes: program.code.count,
            data: pinned.bytes(program.data), data_bytes: program.data.count, gprs: program.gprs)
        x.threads = (e.threads[0], e.threads[1], e.threads[2])
        x.groups = (e.groups[0], e.groups[1], e.groups[2])
        (x.draws, x.draw_counts) = (pinned.words(e.draws), UInt32(e.draws.count))
        (x.repeats, x.warmup) = (e.repeats, e.warmup)
        (x.methods, x.method_pairs) = (pinned.words(e.methods), UInt32(e.methods.count / 2))
        (x.restore, x.restore_pairs) = (pinned.words(e.restore), UInt32(e.restore.count / 2))
        x.single_target = e.singleTarget ? 1 : 0
        x.template_key = e.templateKey
        if let t = e.twod {
            x.twod.op = (t.scale ? SWITCH_BENCH_TWOD_SCALE : SWITCH_BENCH_TWOD_FILL).rawValue
            (x.twod.fmt2d, x.twod.color_format) = (t.fmt2d, t.colorFormat)
            x.twod.color = (t.color[0], t.color[1], t.color[2], t.color[3])
            (x.twod.du, x.twod.dv, x.twod.sx, x.twod.sy) = (t.du, t.dv, t.sx, t.sy)
            (x.twod.bilinear, x.twod.corner) = (t.bilinear ? 1 : 0, t.corner ? 1 : 0)
        }
        let timed = e.draws.count * Int(e.repeats)
        let texelBytes = e.readback == .none ? 0 : Int(e.width * e.height) * e.texelBytes
        var r = switch_bench_result()
        r.gpu_ns = pinned.zeroed(UInt64.self, timed)
        r.wall_ns = pinned.zeroed(UInt64.self, timed)
        r.texels = texelBytes > 0 ? pinned.zeroed(UInt8.self, texelBytes) : nil
        r.texel_bytes = texelBytes
        switch_bench_run(bench, &x, &r)
        var outcome = BenchOutcome(status: Self.statuses[Int(r.status)], why: Self.text(&r.why))
        let gpu = Array(UnsafeBufferPointer(start: r.gpu_ns, count: timed))
        let wall = Array(UnsafeBufferPointer(start: r.wall_ns, count: timed))
        let repeats = Int(e.repeats)
        if outcome.status == "ok" {
            outcome.runs = e.draws.enumerated().map { index, draws in
                let slice = (index * repeats)..<((index + 1) * repeats)
                return (draws, Array(gpu[slice]), Array(wall[slice]))
            }
            if let texels = r.texels { outcome.texels = Data(bytes: texels, count: texelBytes) }
        }
        (outcome.lostError, outcome.faultVA, outcome.templateKey) = (r.lost_error, r.fault_va, r.template_key)
        outcome.milliseconds = Double(DispatchTime.now().uptimeNanoseconds - started) / 1e6
        return outcome
    }

    private static func text<T>(_ tuple: inout T) -> String {
        withUnsafeBytes(of: &tuple) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
}

/// Copies of the inputs at addresses that stay put for the C call, freed with it.
private final class Pinned {
    private var buffers: [UnsafeMutableRawBufferPointer] = []

    deinit { buffers.forEach { $0.deallocate() } }

    func bytes(_ data: Data) -> UnsafePointer<UInt8>? {
        guard !data.isEmpty else { return nil }
        let buffer = allocate(data.count)
        buffer.copyBytes(from: data)
        return UnsafePointer(buffer.baseAddress!.assumingMemoryBound(to: UInt8.self))
    }

    func words(_ values: [UInt32]) -> UnsafePointer<UInt32>? {
        guard !values.isEmpty else { return nil }
        let buffer = allocate(values.count * 4)
        values.withUnsafeBytes { buffer.copyMemory(from: $0) }
        return UnsafePointer(buffer.baseAddress!.assumingMemoryBound(to: UInt32.self))
    }

    func zeroed<T>(_: T.Type, _ count: Int) -> UnsafeMutablePointer<T> {
        let buffer = allocate(max(1, count) * MemoryLayout<T>.stride)
        buffer.initializeMemory(as: UInt8.self, repeating: 0)
        return buffer.baseAddress!.assumingMemoryBound(to: T.self)
    }

    private func allocate(_ count: Int) -> UnsafeMutableRawBufferPointer {
        let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: count, alignment: 16)
        buffers.append(buffer)
        return buffer
    }
}
