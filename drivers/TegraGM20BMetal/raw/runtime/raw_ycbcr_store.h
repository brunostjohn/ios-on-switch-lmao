// SPDX-License-Identifier: MIT
// Metal's store of a render pass into a two-plane YCbCr code attachment over plane 0 of a CoreVideo surface: RGB8_420_2P (540)
// and RGB8_422_2P (541) over '420v'/'420f'/'422v'/'422f', RGB10_420_2P (543) and RGB10_422_2P (544) over 'x420'/'xf20'/'x422'/
// 'xf22'. Measured on an M4 Max under macOS 27.0.1 (raw/test/ycbcrstoreoracle.mm: clears, float and half draws, every byte of
// both planes; the surface's range changes nothing):
//
//  - each texel is held as a half: a drawn float rounded to nearest even (8-bit codes) or toward zero (10-bit), a half a
//    fragment returns kept, a clear's colour rounded to nearest even;
//  - a held channel's code is round(clamp(h, 0, 1) * M), M = 255 or 1023, a NaN 0;
//  - plane 0 holds each texel's green code; plane 1 each chroma block's blue then red code: the block's (2 x 2 texels at 4:2:0,
//    2 x 1 at 4:2:2) texels' codes summed and rounded half up, (sum + n / 2) / n;
//  - a 10-bit code is a little-endian 16-bit word's high ten bits, its low six 0.
//
// A Load reloads the chroma between blocks interpolated (a block reloaded and stored again moves), and a DontCare leaves
// undrawn texels undefined (of a partial draw's, the M4 Max kept 77% of the bytes and stored zeros over the rest): neither
// has a rule here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace raw {

struct YcbcrCodeTarget {
    uint32_t max = 0;  // 255 or 1023
    bool halvedDown = false;
};

inline bool ycbcrCodeTarget(uint32_t pixelFormat, YcbcrCodeTarget &t)
{
    switch (pixelFormat) {
    case 540:
        t = {255, true};
        return true;
    case 541:
        t = {255, false};
        return true;
    case 543:
        t = {1023, true};
        return true;
    case 544:
        t = {1023, false};
        return true;
    default:
        return false;
    }
}

// The format a pass of the raw path renders such an attachment's texels into, its pipelines made for it in the attachment's
// place: RGBA32Float, the fragment's floats as they are, which the store states read as halves rounded as Metal holds them. An
// RGBA16Float target would hold them rounded toward zero on the GM20B (HW Test metal.rgba16f_rounding: 0.5 - 2^-13 stored as
// 0x37ff). A pass's stand-in is a memoryless attachment: its backing is the pass's, read by the store passes after it.
constexpr uint32_t kYcbcrTargetStandIn = 125;

// A two-plane YCbCr format whose passes the raw path renders through its stand-in, or refuses (ycbcrTargetRefusal): all but
// RGB8_444_2P, which the frontend renders through its own stand-in
inline bool ycbcrRenderedFormat(uint32_t pixelFormat)
{
    return pixelFormat >= 500 && pixelFormat <= 548 && pixelFormat != 542;
}

// Null where the raw path renders passes into a two-plane YCbCr attachment of `pixelFormat` (its stand-in, then the pack's
// store states into the planes: pack/ycbcr_store.metal), else why it cannot
inline const char *ycbcrTargetRefusal(uint32_t pixelFormat)
{
    switch (pixelFormat) {
    case 500:
    case 502:
    case 540:
    case 541:
    case 543:
    case 544:
    case 545:
        return nullptr;
    case 503:
        return "a 4:4:4 converted target: m2v's store rule covers the 8-bit 4:2:0 and 4:2:2 ones";
    case 505:
    case 506:
    case 507:
        return "10-bit converted codes: m2v's store rule covers the 8-bit ones";
    case 508:
    case 509:
    case 510:
    case 528:
    case 529:
    case 530:
    case 546:
    case 547:
    case 548:
        return "packed 10-bit codes (p420 and its kin): three codes a word, no unorm target holds one";
    default:
        if (pixelFormat >= 520 && pixelFormat <= 527)
            return "an sRGB converted target: its store's encode is not m2v's rule";
        return "not a two-plane YCbCr format the raw path renders";
    }
}

enum class YcbcrTexelSource : uint8_t {
    DrawnFloat,
    DrawnHalf,
    Clear
};

// The half Metal holds of `v` in (0, 1), as a double (exact)
inline double ycbcrHeldHalf(float v, bool towardZero)
{
    int e = 0;
    std::frexp(double(v), &e);
    const double step = std::ldexp(1.0, std::max(e - 1, -14) - 10);
    const double q = double(v) / step;
    return (towardZero ? std::floor(q) : std::nearbyint(q)) * step;
}

// A held channel's code
inline uint32_t ycbcrTexelCode(float v, YcbcrTexelSource source, uint32_t max)
{
    if (std::isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return max;
    const bool towardZero = max == 1023 && source == YcbcrTexelSource::DrawnFloat;
    const double h = source == YcbcrTexelSource::DrawnHalf ? double(v) : ycbcrHeldHalf(v, towardZero);
    return uint32_t(std::floor(h * max + 0.5));
}

// A chroma code of its block's `n` texel codes (2 or 4)
inline uint32_t ycbcrBlockCode(uint32_t sum, uint32_t n)
{
    return (sum + n / 2) / n;
}

// Metal's codes of a pass's store of `rgba` (width x height texels, four floats each, row by row): `luma` width x height,
// `chroma` (width / 2) x (height / 2 or height) pairs, Cb then Cr. False for a size the chroma blocks do not tile.
inline bool ycbcrStore(const YcbcrCodeTarget &t, const float *rgba, uint32_t width, uint32_t height,
                       YcbcrTexelSource source, uint16_t *luma, uint16_t *chroma)
{
    const uint32_t down = t.halvedDown ? 2 : 1;
    if (!width || !height || width % 2 || height % down)
        return false;
    for (uint32_t i = 0; i < width * height; i++)
        luma[i] = uint16_t(ycbcrTexelCode(rgba[4 * i + 1], source, t.max));
    const uint32_t cw = width / 2, ch = height / down;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++)
            for (uint32_t c = 0; c < 2; c++) {
                const uint32_t channel = c ? 0 : 2;
                uint32_t sum = 0;
                for (uint32_t dy = 0; dy < down; dy++)
                    for (uint32_t dx = 0; dx < 2; dx++)
                        sum +=
                            ycbcrTexelCode(rgba[4 * ((y * down + dy) * width + 2 * x + dx) + channel], source, t.max);
                chroma[2 * (y * cw + x) + c] = uint16_t(ycbcrBlockCode(sum, 2 * down));
            }
    return true;
}

// The converted 8-bit formats' store (YCBCR8_420_2P 500, YCBCR8_422_2P 502), m2v's (bind/ycbcr_write.rs `ycbcr_store`), the
// same bytes on this Mac: a held half's grid step x = rint(clamp(h, 0, 1) * 2048), each texel's term of a code's row c (red,
// green, blue, counted at 2^26) truncated at 2^-6 of a code, t = (c . x) >> 20, and the code o + ((sum t + 2^(5 + k)) >> (6 + k))
// over its block of 2^k texels, clamped to [0, 255]
enum class YcbcrMatrix : uint8_t {
    Bt601,
    Bt709,
    Bt2020
};

inline void ycbcrConvertedRows(bool full, YcbcrMatrix m, int64_t rows[3][3], int64_t offsets[3])
{
    static constexpr int32_t kQ15[3][3][3] = {{{9797, 19235, 3736}, {-5537, -10846, 16383}, {16384, -13730, -2654}},
                                              {{6967, 23436, 2365}, {-3756, -12629, 16384}, {16384, -14884, -1501}},
                                              {{8608, 22217, 1943}, {-4574, -11810, 16384}, {16384, -15067, -1318}}};
    static constexpr int32_t kVideo[3][3][4] = {
        {{8381, 16455, 3196, 18}, {-4845, -9491, 14336, 18}, {7168, -6007, -1161, 17}},
        {{5960, 20049, 2023, 18}, {-1643, -5525, 7168, 17}, {14336, -13023, -1313, 18}},
        {{3682, 9503, 831, 17}, {-4003, -10334, 14336, 18}, {14336, -13183, -1153, 18}}};
    const int i = int(m);
    for (int code = 0; code < 3; code++) {
        offsets[code] = code ? 128 : full ? 0 : 16;
        for (int c = 0; c < 3; c++)
            rows[code][c] = full ? int64_t(kQ15[i][code][c]) * (code ? 256 : 255)
                                 : int64_t(kVideo[i][code][c]) * (int64_t(1) << (26 - kVideo[i][code][3]));
    }
}

inline int64_t ycbcrGridStep(float v)
{
    if (std::isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 2048;
    return int64_t(std::nearbyint(ycbcrHeldHalf(v, false) * 2048.0));
}

// Every texel source rounds to the nearest half here (a half a fragment returns is its own nearest)
inline bool ycbcrStoreConverted(bool full, YcbcrMatrix m, bool halvedDown, const float *rgba, uint32_t width,
                                uint32_t height, uint16_t *luma, uint16_t *chroma)
{
    const uint32_t down = halvedDown ? 2 : 1;
    if (!width || !height || width % 2 || height % down)
        return false;
    int64_t rows[3][3], offsets[3];
    ycbcrConvertedRows(full, m, rows, offsets);
    auto term = [&](int code, uint32_t texel) {
        int64_t t = 0;
        for (int c = 0; c < 3; c++)
            t += rows[code][c] * ycbcrGridStep(rgba[4 * texel + c]);
        return t >> 20;
    };
    auto encode = [&](int code, int64_t sum, int k) {
        const int shift = 6 + k;
        return uint16_t(std::clamp<int64_t>(offsets[code] + ((sum + (int64_t(1) << (shift - 1))) >> shift), 0, 255));
    };
    for (uint32_t i = 0; i < width * height; i++)
        luma[i] = encode(0, term(0, i), 0);
    const uint32_t cw = width / 2, ch = height / down;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++)
            for (int code = 1; code < 3; code++) {
                int64_t sum = 0;
                for (uint32_t dy = 0; dy < down; dy++)
                    for (uint32_t dx = 0; dx < 2; dx++)
                        sum += term(code, (y * down + dy) * width + 2 * x + dx);
                chroma[2 * (y * cw + x) + code - 1] = encode(code, sum, halvedDown ? 2 : 1);
            }
    return true;
}

// The pack's store states' fragment buffer 0 (pack/ycbcr_store.metal StoreShape)
struct YcbcrStoreShape {
    uint32_t max = 0, shift = 0, towardZero = 0, across = 0, down = 0, converted = 0;
    int32_t offset[3] = {};
    int32_t row[3][3] = {};
    uint32_t mul[3] = {};
};
static_assert(sizeof(YcbcrStoreShape) == 84, "pack/ycbcr_store.metal StoreShape");

// A code target of `bits`-bit codes: 10-bit codes in a 16-bit word's high bits, a drawn float held toward zero
inline YcbcrStoreShape ycbcrCodesShape(uint32_t bits, bool across, bool down)
{
    YcbcrStoreShape s;
    s.max = (1u << bits) - 1;
    s.shift = bits == 10 ? 6 : 0;
    s.towardZero = bits == 10;
    s.across = across;
    s.down = down;
    return s;
}

// A converted 8-bit target: ycbcrConvertedRows' rows as a coefficient and its scale over 2^20
inline YcbcrStoreShape ycbcrConvertedShape(bool full, YcbcrMatrix m, bool across, bool down)
{
    static constexpr int32_t kQ15[3][3][3] = {{{9797, 19235, 3736}, {-5537, -10846, 16383}, {16384, -13730, -2654}},
                                              {{6967, 23436, 2365}, {-3756, -12629, 16384}, {16384, -14884, -1501}},
                                              {{8608, 22217, 1943}, {-4574, -11810, 16384}, {16384, -15067, -1318}}};
    static constexpr int32_t kVideo[3][3][4] = {
        {{8381, 16455, 3196, 18}, {-4845, -9491, 14336, 18}, {7168, -6007, -1161, 17}},
        {{5960, 20049, 2023, 18}, {-1643, -5525, 7168, 17}, {14336, -13023, -1313, 18}},
        {{3682, 9503, 831, 17}, {-4003, -10334, 14336, 18}, {14336, -13183, -1153, 18}}};
    YcbcrStoreShape s;
    s.max = 255;
    s.across = across;
    s.down = down;
    s.converted = 1;
    const int i = int(m);
    for (int code = 0; code < 3; code++) {
        s.offset[code] = code ? 128 : full ? 0 : 16;
        for (int c = 0; c < 3; c++)
            s.row[code][c] = full ? kQ15[i][code][c] : kVideo[i][code][c];
        s.mul[code] = full ? (code ? 256u : 255u) : 1u << (26 - kVideo[i][code][3]);
    }
    return s;
}

// floor(sum * mul / 2^20) as the store state takes it in 32-bit steps: sum = 4096 q + r, q mul = 256 s + u
inline int32_t ycbcrScaled(int32_t sum, uint32_t mul)
{
    const int32_t q = sum >> 12, r = sum & 4095, qm = q * int32_t(mul);
    return (qm >> 8) + (((qm & 255) * 4096 + r * int32_t(mul)) >> 20);
}

// The store states' codes of a pass's stand-in (`rgba`, width x height texels): the luma plane's and each chroma block's Cb, Cr
// as the pack's ycbcr_store.metal computes them, step for step
inline bool ycbcrStoreDrawn(const YcbcrStoreShape &s, const float *rgba, uint32_t width, uint32_t height, uint16_t *luma,
                            uint16_t *chroma)
{
    const uint32_t w = s.across ? 2 : 1, h = s.down ? 2 : 1, n = w * h, k = (s.across ? 1 : 0) + (s.down ? 1 : 0);
    if (!width || !height || width % w || height % h)
        return false;
    const auto code = [&](float v) {
        if (std::isnan(v) || v <= 0.0f)
            return 0u;
        if (v >= 1.0f)
            return s.max;
        return uint32_t(std::floor(ycbcrHeldHalf(v, s.towardZero != 0) * s.max + 0.5));
    };
    const auto term = [&](uint32_t c, uint32_t texel) {
        int32_t sum = 0;
        for (uint32_t ch = 0; ch < 3; ch++)
            sum += s.row[c][ch] * int32_t(ycbcrGridStep(rgba[4 * texel + ch]));
        return ycbcrScaled(sum, s.mul[c]);
    };
    const auto encoded = [&](uint32_t c, int32_t sum, uint32_t blockLog2) {
        const uint32_t shift = 6 + blockLog2;
        return uint16_t(std::clamp(s.offset[c] + ((sum + (1 << (shift - 1))) >> shift), 0, 255));
    };
    for (uint32_t i = 0; i < width * height; i++)
        luma[i] = s.converted ? encoded(0, term(0, i), 0) : uint16_t(code(rgba[4 * i + 1]));
    const uint32_t cw = width / w, ch = height / h;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            int32_t cb = 0, cr = 0;
            for (uint32_t dy = 0; dy < h; dy++)
                for (uint32_t dx = 0; dx < w; dx++) {
                    const uint32_t texel = (y * h + dy) * width + x * w + dx;
                    cb += s.converted ? term(1, texel) : int32_t(code(rgba[4 * texel + 2]));
                    cr += s.converted ? term(2, texel) : int32_t(code(rgba[4 * texel]));
                }
            uint16_t *out = &chroma[2 * (y * cw + x)];
            out[0] = s.converted ? encoded(1, cb, k) : uint16_t((uint32_t(cb) + n / 2) / n);
            out[1] = s.converted ? encoded(2, cr, k) : uint16_t((uint32_t(cr) + n / 2) / n);
        }
    return true;
}

}  // namespace raw
