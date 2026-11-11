import Foundation

/// Metal's store of a render pass into a two-plane YCbCr attachment over plane 0 of a CoreVideo surface, as
/// drivers/TegraGM20BMetal/raw/runtime/raw_ycbcr_store.h states it (an M4 Max's bytes): code formats (540, 541, 543,
/// 544) hold each texel as a half and store each texel's green code and each chroma block's rounded mean code; the
/// converted 8-bit ones (500, 502) are m2v's rule over the same halves.
public enum YcbcrStore {
    public enum Source: Sendable {
        case drawnFloat, drawnHalf, clear
    }

    static func held(_ v: Float, towardZero: Bool) -> Double {
        let step = Double(sign: .plus, exponent: max(Int(v.exponent), -14) - 10, significand: 1)
        let q = Double(v) / step
        return (towardZero ? q.rounded(.down) : q.rounded(.toNearestOrEven)) * step
    }

    /// A held channel's code of `max` (255 or 1023): 10-bit codes take a drawn float toward zero.
    public static func code(_ v: Float, _ source: Source, max: UInt32) -> UInt32 {
        if v.isNaN || v <= 0 { return 0 }
        if v >= 1 { return max }
        let h = source == .drawnHalf ? Double(v) : held(v, towardZero: max == 1023 && source == .drawnFloat)
        return UInt32((h * Double(max) + 0.5).rounded(.down))
    }

    /// Luma codes, then Cb and Cr of each chroma block (2 x 2 texels, or 2 x 1 where not `halvedDown`), row by row.
    public static func codes(
        _ texels: [SIMD4<Float>], width: Int, height: Int, max: UInt32, halvedDown: Bool, source: Source
    ) -> (luma: [UInt16], chroma: [UInt16]) {
        let luma = texels.map { UInt16(code($0.y, source, max: max)) }
        let down = halvedDown ? 2 : 1
        var chroma: [UInt16] = []
        for y in 0..<(height / down) {
            for x in 0..<(width / 2) {
                for channel in [2, 0] {
                    var sum: UInt32 = 0
                    for dy in 0..<down {
                        for dx in 0..<2 {
                            sum += code(texels[(y * down + dy) * width + 2 * x + dx][channel], source, max: max)
                        }
                    }
                    chroma.append(UInt16((sum + UInt32(down)) / UInt32(2 * down)))
                }
            }
        }
        return (luma, chroma)
    }

    static func gridStep(_ v: Float) -> Int64 {
        if v.isNaN || v <= 0 { return 0 }
        if v >= 1 { return 2048 }
        return Int64((held(v, towardZero: false) * 2048).rounded(.toNearestOrEven))
    }

    /// YCBCR8_420_2P (500) or YCBCR8_422_2P (502) over a surface without a matrix (BT.601), video or full range.
    public static func converted(
        _ texels: [SIMD4<Float>], width: Int, height: Int, full: Bool, halvedDown: Bool
    ) -> (luma: [UInt16], chroma: [UInt16]) {
        let q15: [[Int64]] = [[9797, 19235, 3736], [-5537, -10846, 16383], [16384, -13730, -2654]]
        let video: [([Int64], Int64)] = [
            ([8381, 16455, 3196], 18), ([-4845, -9491, 14336], 18), ([7168, -6007, -1161], 17),
        ]
        let rows: [[Int64]] = (0..<3).map { code in
            full
                ? q15[code].map { $0 * (code == 0 ? 255 : 256) }
                : video[code].0.map { $0 * (1 << (26 - video[code].1)) }
        }
        let offsets: [Int64] = [full ? 0 : 16, 128, 128]
        let steps = texels.map { [gridStep($0.x), gridStep($0.y), gridStep($0.z)] }
        let term = { (code: Int, texel: Int) -> Int64 in
            (0..<3).reduce(Int64(0)) { $0 + rows[code][$1] * steps[texel][$1] } >> 20
        }
        let encode = { (code: Int, sum: Int64, k: Int64) -> UInt16 in
            UInt16(Swift.min(Swift.max(offsets[code] + ((sum + (1 << (5 + k))) >> (6 + k)), 0), 255))
        }
        let luma = (0..<texels.count).map { encode(0, term(0, $0), 0) }
        let down = halvedDown ? 2 : 1
        var chroma: [UInt16] = []
        for y in 0..<(height / down) {
            for x in 0..<(width / 2) {
                for code in 1...2 {
                    var sum: Int64 = 0
                    for dy in 0..<down {
                        for dx in 0..<2 { sum += term(code, (y * down + dy) * width + 2 * x + dx) }
                    }
                    chroma.append(encode(code, sum, halvedDown ? 2 : 1))
                }
            }
        }
        return (luma, chroma)
    }
}
