import Foundation
import HWTestCore

/// The MPEG-4 Part 2 clips, from FFmpeg's encoder (`ffmpeg` on PATH, or $FFMPEG): the Mac's VideoToolbox encodes no
/// MPEG-4 Part 2. The pattern goes in as raw I420 frames; one thread keeps the bytes the same from run to run. The
/// 720p clip is Simple profile (I- and P-VOPs, H.263 quantisation); the 1080p one is Advanced Simple (two B-VOPs
/// between references, quarter-sample motion, MPEG quantisation with both matrices loaded), the streams NVDEC takes
/// that the Mac's and the iPad's software decoders refuse.
enum FFmpegWriter {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    static let intraMatrix = [
        8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29,
        34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29,
        35, 38, 46, 56, 69, 83,
    ]
    static let interMatrix = [
        16, 17, 18, 19, 20, 21, 22, 23, 17, 18, 19, 20, 21, 22, 23, 24, 18, 19, 20, 21, 22, 23, 24, 25, 19, 20, 21, 22, 23,
        24, 26, 27, 20, 21, 22, 23, 25, 26, 27, 28, 21, 22, 23, 24, 26, 27, 28, 30, 22, 23, 24, 26, 27, 28, 30, 31, 23, 24,
        25, 27, 28, 30, 31, 40,
    ]

    static func encoder(_ clip: Clip) throws -> [String] {
        guard clip.codec == .mpeg4 else { throw Failure(description: "\(clip.file): not an FFmpeg clip") }
        let common = ["-c:v", "mpeg4", "-threads", "1", "-g", "30", "-b:v", "\(clip.bitrate)"]
        guard clip.height > 720 else { return common + ["-bf", "0"] }
        let matrix = { (values: [Int]) in values.map(String.init).joined(separator: ",") }
        return common + [
            "-bf", "2", "-flags", "+qpel", "-mpeg_quant", "1", "-intra_matrix", matrix(intraMatrix), "-inter_matrix",
            matrix(interMatrix),
        ]
    }

    static func write(_ clip: Clip, to url: URL) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        let ffmpeg = ProcessInfo.processInfo.environment["FFMPEG"] ?? "ffmpeg"
        process.arguments =
            [
                ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "\(clip.width)x\(clip.height)",
                "-r", "\(clip.fps)", "-i", "-",
            ] + (try encoder(clip)) + ["-f", "mp4", url.path]
        let input = Pipe()
        process.standardInput = input
        try process.run()
        let lumaBytes = clip.width * clip.height
        var frame = Data(count: lumaBytes * 3 / 2)
        let pattern = clip.pattern
        for index in 0..<clip.frames {
            frame.withUnsafeMutableBytes { bytes in
                let base = bytes.baseAddress!
                PatternWriter.writeLuma(pattern, frame: index, into: .init(base: base, rowBytes: clip.width))
                PatternWriter.writeChromaPlanar(
                    pattern, cb: .init(base: base + lumaBytes, rowBytes: clip.width / 2),
                    cr: .init(base: base + lumaBytes * 5 / 4, rowBytes: clip.width / 2))
            }
            try input.fileHandleForWriting.write(contentsOf: frame)
        }
        try input.fileHandleForWriting.close()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw Failure(description: "\(clip.file): \(ffmpeg) exited \(process.terminationStatus)")
        }
    }
}
