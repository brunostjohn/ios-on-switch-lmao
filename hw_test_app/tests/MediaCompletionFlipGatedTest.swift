import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import UIKit

/// TegraVIC's gate (drivers/TegraVIC/SwitchVICGateABI.h, debug kernels), little-endian: ARM queues a VIC copy of one of
/// the app's IOSurfaces into another behind a syncpoint the test holds, RELEASE lets it run, QUERY reports.
enum VICGate {
    static let magic: UInt32 = 0x5647_4154
    static let arm: UInt32 = 1
    static let release: UInt32 = 2
    static let query: UInt32 = 3
    static let held: UInt32 = 1
    static let done: UInt32 = 3
    static let replyBytes = 24

    struct Reply {
        var state: UInt32 = 0
        var error: Int32 = 0
        var status: Int32 = 0
    }

    static func call(
        _ session: VICKernelTest.Session, _ op: UInt32, source: UInt32 = 0, destination: UInt32 = 0
    )
        -> Result<Reply, TestFailure>
    {
        let request = [magic, op, source, destination].flatMap { word in
            (0..<4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
        }
        return session.call(request, replyBytes: replyBytes).flatMap { bytes in
            let word = { (at: Int) in (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) } }
            guard word(0) == magic else { return .failure(TestFailure("malformed gate reply")) }
            return .success(
                Reply(state: word(4), error: Int32(bitPattern: word(8)), status: Int32(bitPattern: word(12))))
        }
    }
}

/// Three 1280x720 '420v' IOSurface buffers on an AVSampleBufferDisplayLayer, which Core Animation detaches onto window
/// B (display.plane_in_use): `gated` (VIC's destination), `shown` and `source`, on 256-byte rows so VIC addresses them as
/// they are.
@MainActor
final class GatedRun {
    let layer = AVSampleBufferDisplayLayer()
    private(set) var buffers: [CVPixelBuffer] = []
    private var format: CMVideoFormatDescription?
    nonisolated static let gated = 0
    nonisolated static let shown = 1
    nonisolated static let source = 2
    nonisolated static let luma: [Int32] = [160, 64, 235]

    init() {
        let attributes: [CFString: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
            kCVPixelBufferBytesPerRowAlignmentKey: 256, kCVPixelBufferPlaneAlignmentKey: 256,
        ]
        for luma in Self.luma {
            var buffer: CVPixelBuffer?
            CVPixelBufferCreate(
                nil, 1280, 720, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes as CFDictionary, &buffer)
            guard let buffer else { continue }
            CVBufferSetAttachment(
                buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
            CVPixelBufferLockBaseAddress(buffer, [])
            for plane in 0..<2 {
                guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { continue }
                let bytes =
                    CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) * CVPixelBufferGetHeightOfPlane(buffer, plane)
                memset(base, plane == 0 ? luma : 128, bytes)
            }
            CVPixelBufferUnlockBaseAddress(buffer, [])
            buffers.append(buffer)
        }
        if let first = buffers.first {
            CMVideoFormatDescriptionCreateForImageBuffer(
                allocator: nil, imageBuffer: first, formatDescriptionOut: &format)
        }
        layer.videoGravity = .resizeAspect
    }

    var surfaceIDs: [UInt32] {
        buffers.compactMap { CVPixelBufferGetIOSurface($0).map { IOSurfaceGetID($0.takeUnretainedValue()) } }
    }

    func enqueue(_ index: Int, frame: Int) {
        guard let format, index < buffers.count else { return }
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: 30),
            presentationTimeStamp: CMTime(value: Int64(frame), timescale: 30), decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        CMSampleBufferCreateReadyWithImageBuffer(
            allocator: nil, imageBuffer: buffers[index], formatDescription: format, sampleTiming: &timing,
            sampleBufferOut: &sample)
        guard let sample else { return }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
            CFArrayGetCount(attachments) > 0
        {
            let dictionary = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(
                dictionary, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        layer.sampleBufferRenderer.enqueue(sample)
    }
}

/// A video set that waits for its producer at its swap, written by that producer's completion
/// (drivers/NativeMobileDisplay/SwitchMediaCompletionFlipDisplay.h). Nothing iOS presents waits so: the decoder emits a
/// picture once NVDEC is done and Core Animation's VIC turns finish before it presents them (display.media_completion_flip
/// skips with d_armed 0). The test makes one with TegraVIC's gate: `gated` then `shown` go on window B, so both are
/// mapped and neither is scanned; a VIC copy of `source` into `gated` is queued behind the gate and `gated` presented;
/// once the display has armed that set (kern.switch_media_flips_armed) the gate is released. The copy's completion has to
/// write the set and windows B and C latch it as written (d_completion, refused 0).
struct MediaCompletionFlipGatedTest: HWTest {
    let id = "display.media_completion_flip_gated"
    let name = "A video set waiting for its VIC copy is written by the copy's completion"
    let subsystem = Subsystem.display

    func run(_ context: TestContext) async throws -> Outcome {
        let (first, unread) = MediaCompletionFlipTest.totals()
        guard !first.isEmpty else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_media_flips_*", unread), metrics)
        }
        let session: VICKernelTest.Session
        switch VICKernelTest.Session.open() {
        case .failure(let failure): return .skip("no TegraVIC kernel-test connection: \(failure)")
        case .success(let opened): session = opened
        }
        defer { session.close() }
        let run = await context.onStage(fullScreen: true) { stage -> GatedRun in
            let run = GatedRun()
            run.layer.frame = stage.bounds
            stage.layer.addSublayer(run.layer)
            return run
        }
        let ids = await MainActor.run { run.surfaceIDs }
        guard ids.count == 3 else { return .fail("three IOSurface buffers (\(ids.count) made)") }
        var frame = 0
        for index in [GatedRun.gated, GatedRun.shown] {
            for _ in 0..<10 {
                let at = frame
                await MainActor.run { run.enqueue(index, frame: at) }
                frame += 1
                try await Task.sleep(nanoseconds: 33_000_000)
            }
        }
        try await Task.sleep(nanoseconds: 300_000_000)

        let before = MediaCompletionFlipTest.totals().values
        let armed = VICGate.call(session, VICGate.arm, source: ids[GatedRun.source], destination: ids[GatedRun.gated])
        guard case .success(let held) = armed else { return .fail("no gate reply") }
        var metrics = Metrics()
        metrics.set("arm_error", Int(held.error))
        guard held.error == 0, held.state == VICGate.held else {
            return .fail("the gate did not hold the copy (errno \(held.error))", metrics)
        }
        let presented = DispatchTime.now()
        let last = frame
        await MainActor.run { run.enqueue(GatedRun.gated, frame: last) }
        var armedAt: DispatchTime?
        for _ in 0..<100 {
            try await Task.sleep(nanoseconds: 10_000_000)
            if (MediaCompletionFlipTest.totals().values["armed"] ?? 0) > (before["armed"] ?? 0) {
                armedAt = DispatchTime.now()
                break
            }
        }
        let released = VICGate.call(session, VICGate.release)
        var final = VICGate.Reply()
        for _ in 0..<100 {
            guard case .success(let reply) = VICGate.call(session, VICGate.query) else { break }
            final = reply
            if reply.state == VICGate.done { break }
            try await Task.sleep(nanoseconds: 10_000_000)
        }
        try await Task.sleep(nanoseconds: 200_000_000)
        let after = MediaCompletionFlipTest.totals().values
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }

        for name in MediaCompletionFlipTest.counters { metrics.set("d_" + name, delta(name)) }
        metrics.set("refused", after["refused"] ?? 0)
        let waited = armedAt.map { Double($0.uptimeNanoseconds - presented.uptimeNanoseconds) / 1e6 } ?? -1
        metrics.set("arm_wait_ms", waited, digits: 1)
        var releaseOK = false
        if case .success(let reply) = released {
            metrics.set("release_error", Int(reply.error))
            releaseOK = reply.error == 0
        }
        metrics.set("copy_state", Int(final.state))
        metrics.set("copy_status", Int(final.status))
        return .judged(
            [
                ("the gate released the copy (release_error 0)", releaseOK),
                ("the set waited for VIC at its swap (d_fenced > 0)", delta("fenced") > 0),
                ("the display armed it for its producer's completion (d_armed > 0)", delta("armed") > 0),
                ("the copy's completion wrote it (d_completion > 0)", delta("completion") > 0),
                ("windows B and C latched it as written (d_refused 0)", delta("refused") == 0),
                ("the copy completed (copy_state 3, copy_status 0)", final.state == VICGate.done && final.status == 0),
            ], metrics)
    }
}
