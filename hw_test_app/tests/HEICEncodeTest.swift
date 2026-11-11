import CoreGraphics
import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import ImageIO
import UniformTypeIdentifiers
import VideoToolbox

/// HEIC written on the device and read back: ImageIO's encode (CMPhoto, a tile session per image over the 'hvc1'
/// encoder) decoded and compared with the pattern. The same tile session SPI, with CMPhoto's specification and
/// properties, then runs here on the pattern's NV12 picture so its encoder can be asked how it coded the tiles
/// (SwitchEncoderRoutes, debug builds of the AVE plug-in): every 512x512 tile one NVENC picture, each decoded back on
/// the hardware decoder against its region of the source. hevc_encoders is how many 'hvc1' encoders VideoToolbox lists.
struct HEICEncodeTest: HWTest {
    let id = "image.heic_encode"
    let name = "HEIC encode on NVENC (tile sessions) and decode on the device"
    let subsystem = Subsystem.images

    func run(_ context: TestContext) async throws -> Outcome {
        let pattern = Pattern(width: 1024, height: 768, clip: 0x5E1C)
        let tiled = try TileCompressor.encode(
            try PatternPixels.make(pattern, frame: 0, bitDepth: 8, pool: nil), tile: 512, quality: 0.9)
        var metrics = Metrics()
        metrics.set("hevc_encoders", encoderIDs(kCMVideoCodecType_HEVC).count)
        metrics.set("tiles", tiled.tiles)
        metrics.set("tiles_failed", tiled.failed)
        metrics.set("tile_hardware", tiled.usingHardware)
        metrics.set("nvenc_tiles", tiled.routes?["tiles"] ?? -1)
        metrics.set("nvenc_errors", tiled.routes?["errors"] ?? -1)
        metrics.set("tiles_decoded", tiled.decoded)
        metrics.set("tile_error", tiled.worstLumaError, digits: 1)
        metrics.set("tile_bytes", tiled.bytes)
        metrics.set("tile_ms", tiled.seconds * 1000, digits: 1)

        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.heic.identifier as CFString, 1, nil)
        else {
            return .fail("ImageIO offers no HEIC destination", metrics)
        }
        let start = CFAbsoluteTimeGetCurrent()
        CGImageDestinationAddImage(
            destination, Pixels.patternImage(pattern), [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary
        )
        guard CGImageDestinationFinalize(destination) else {
            return .fail("CGImageDestinationFinalize failed", metrics)
        }
        metrics.set("encode_ms", (CFAbsoluteTimeGetCurrent() - start) * 1000, digits: 1)
        metrics.set("bytes", data.length)
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
            let decoded = CGImageSourceCreateImageAtIndex(source, 0, nil)
        else { return .fail("the HEIC written does not decode", metrics) }
        let result = Pixels.check(decoded, pattern: pattern)
        metrics.set("error", result.lumaError, digits: 1)
        let nvencTiles = tiled.routes?["tiles"]
        return .judged(
            [
                ("decodes back to the pattern", result.matches(frame: 0, tolerance: 12)),
                (
                    "the tile session's encoder is the AVE plug-in on hardware",
                    tiled.routes != nil && tiled.usingHardware
                ),
                (
                    "NVENC coded every tile",
                    tiled.tiles == tiled.expected && tiled.failed == 0 && nvencTiles == tiled.expected
                        && tiled.routes?["errors"] == 0
                ),
                ("every tile decodes back to its region", tiled.decoded == tiled.expected && tiled.worstLumaError <= 4),
            ],
            metrics, proof: "SwitchEncoderRoutes tiles=\(nvencTiles.map(String.init) ?? "-") of \(tiled.expected)")
    }

    func encoderIDs(_ codec: CMVideoCodecType) -> [String] {
        var list: CFArray?
        guard VTCopyVideoEncoderList(nil, &list) == noErr, let entries = list as? [[String: Any]] else { return [] }
        return entries.filter { ($0[kVTVideoEncoderList_CodecType as String] as? NSNumber)?.uint32Value == codec }
            .compactMap { $0[kVTVideoEncoderList_EncoderID as String] as? String }
    }
}
