import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// The 8-bit H.264 720p clip decoded once, and the decoder's own count of how each picture reached the client
/// (SwitchPictureRoutes, debug builds of the AVD plugin): exported as the destination surface's native NVDEC picture,
/// placed by VIC, or copied by the CPU. LIMITS V1: every 720p, 1080p and 4K session exported none. claim_verdict is
/// PASS when every picture went out natively; INCONCLUSIVE when the decoder does not say.
struct NativeExportTest: HWTest {
    let id = "decode.native_export"
    let name = "H.264 720p: pictures exported natively against VIC copies, by the decoder's count"
    let subsystem = Subsystem.decode
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 720 }) else {
            return .skip("no H.264 720p clip")
        }
        let demuxed = try await Demux.load(clip, from: context.media)
        let result = try Decompressor.decode(
            demuxed.samples, format: demuxed.format, pattern: clip.pattern, bitDepth: clip.codec.bitDepth,
            fps: clip.fps)
        var metrics = Metrics()
        metrics.set("frames", result.frames)
        metrics.set("matched", result.matched)
        let finding: ProbeFinding
        if let routes = result.routes {
            for key in ["exported", "vic", "cpu", "errors"] { metrics.set("decoder_" + key, routes[key] ?? 0) }
            let notNative = (routes["vic"] ?? 0) + (routes["cpu"] ?? 0)
            finding = ProbeFinding(
                notNative == 0 && (routes["exported"] ?? 0) > 0 ? .pass : .fail, metrics,
                note: notNative == 0 ? "" : "\(notNative) pictures were copied, not exported")
        } else {
            finding = ProbeFinding(.inconclusive, metrics, note: "the decoder does not answer SwitchPictureRoutes")
        }
        var base = Metrics()
        base.set("hardware", result.usingHardware)
        return finding.outcome(probe: "V1", base: base, proof: "")
    }
}
