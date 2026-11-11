import Darwin
import Foundation
import HWTestCore
import HWTestGPU

/// LIMITS V9: VIC source rows on multiples of 256 bytes (kTegraVICTraits.sourcePitchAlign), from a deleted boot
/// self-test where 144- and 192-byte rows lost half the copy. Every public path bounces other source pitches through a
/// GPU copy first, so the probe is TegraVIC's DEVELOPMENT kernel-test selector
/// (drivers/TegraVIC/SwitchVICPitchProbe.h): nine 1:1 BGRA copies with no bounce at 320, 576 and 2880 bytes a row
/// against 256-multiple controls, each compared in the kernel. A VIC job that hangs panics the kernel, so it runs last.
enum X1VICPitchProbe {
    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> CFMutableDictionary?
    typealias Service = @convention(c) (mach_port_t, CFDictionary?) -> mach_port_t
    typealias Open =
        @convention(c) (mach_port_t, mach_port_t, UInt32, UnsafeMutablePointer<mach_port_t>) -> kern_return_t
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t
    typealias Call =
        @convention(c) (
            mach_port_t, UInt32, UnsafeRawPointer?, Int, UnsafeMutableRawPointer?, UnsafeMutablePointer<Int>
        ) -> kern_return_t

    static let selector: UInt32 = 6
    static let replyBytes = 340
    static let cases = 9
    static let firstControl = 6

    /// The reply's words, or why there are none.
    static func call() -> Result<[UInt32], TestFailure> {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let open = dlsym(iokit, "IOServiceOpen"), let release = dlsym(iokit, "IOObjectRelease"),
            let close = dlsym(iokit, "IOServiceClose"), let call = dlsym(iokit, "IOConnectCallStructMethod")
        else { return .failure(TestFailure("no IOKit")) }
        let vic = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("AppleM2ScalerCSCDriver"))
        guard vic != 0 else { return .failure(TestFailure("no AppleM2ScalerCSCDriver service")) }
        var connection: mach_port_t = 0
        let opened = unsafeBitCast(open, to: Open.self)(vic, mach_task_self_, 0, &connection)
        _ = unsafeBitCast(release, to: Release.self)(vic)
        guard opened == KERN_SUCCESS else {
            return .failure(TestFailure("open \(String(UInt32(bitPattern: opened), radix: 16))"))
        }
        defer { _ = unsafeBitCast(close, to: Release.self)(connection) }
        let request: [UInt32] = [0x5650_4954, 0]
        var reply = [UInt32](repeating: 0, count: replyBytes / 4)
        var size = replyBytes
        let kr = request.withUnsafeBytes { input in
            reply.withUnsafeMutableBytes { output in
                unsafeBitCast(call, to: Call.self)(
                    connection, selector, input.baseAddress, input.count, output.baseAddress, &size)
            }
        }
        guard kr == KERN_SUCCESS, size == replyBytes else {
            return .failure(TestFailure("call \(String(UInt32(bitPattern: kr), radix: 16)) size \(size)"))
        }
        return .success(reply)
    }

    static let probe = X1Probe(
        id: "gpu.x1.vic_pitch", name: "VIC rows of 320, 576 and 2880 bytes copied with no bounce", probe: "V9",
        risky: true
    ) { _ in
        var metrics = Metrics()
        let reply: [UInt32]
        switch call() {
        case .failure(let failure): return ProbeFinding(.inconclusive, metrics, note: failure.description)
        case .success(let words): reply = words
        }
        var wrong: [String: Int] = [:]
        for i in 0..<cases {
            let r = Array(reply[(4 + 9 * i)..<(4 + 9 * (i + 1))])
            let name = "p\(r[0])_\(r[1])"
            metrics.set(name + "_status", Int(Int32(bitPattern: r[3])))
            metrics.set(name + "_outside", Int(r[8]))
            if r[4] > 0 { metrics.set(name + "_first", "\(r[5]),\(r[6]):\(String(r[7], radix: 16))") }
            if i < firstControl { wrong[name + "_wrong"] = Int(r[4]) + Int(r[8]) + (r[3] != 0 ? 1 : 0) }
        }
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0 else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "a 256-multiple control did not copy exactly")
        }
        return finding
    }
}
