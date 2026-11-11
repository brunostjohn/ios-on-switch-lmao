import Foundation

/// Metal's float conversions and blend arithmetic, the references the X1 probes compare the GPU with. Halves are
/// widened to Double, where a product of two halves is exact, and rounded once.
enum X1Reference {
    /// A store into a `levels - 1`-code unorm channel: NaN is 0, the value clamped to [0, 1], its product with the top
    /// code rounded to nearest, ties away from zero.
    static func unorm(_ value: Double, top: Double) -> Int {
        if value.isNaN { return 0 }
        return Int((min(max(value, 0), 1) * top).rounded(.toNearestOrAwayFromZero))
    }

    /// Whether the product with the top code lands exactly halfway between two codes.
    static func tie(_ value: Double, top: Double) -> Bool {
        guard !value.isNaN else { return false }
        let scaled = min(max(value, 0), 1) * top
        return scaled - scaled.rounded(.down) == 0.5
    }

    /// The sRGB encoding of a linear value, as a code of 255.
    static func srgb8(_ value: Double) -> Int {
        if value.isNaN { return 0 }
        let x = min(max(value, 0), 1)
        let encoded = x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1 / 2.4) - 0.055
        return Int((encoded * 255).rounded(.toNearestOrAwayFromZero))
    }

    static func half(_ value: Double) -> Double { Double(Float16(value)) }

    /// The half nearest code / top, as a unorm texel reads in a shader.
    static func texel(_ code: Int, top: Double) -> Double { Double(Float16(Double(code) / top)) }

    /// MTLBlendFactor's value for one channel.
    enum Factor: String, CaseIterable {
        case zero, one, sa, oneMinusSa, da, oneMinusDa

        func value(sa: Double, da: Double) -> Double {
            switch self {
            case .zero: return 0
            case .one: return 1
            case .sa: return sa
            case .oneMinusSa: return half(1 - sa)
            case .da: return da
            case .oneMinusDa: return half(1 - da)
            }
        }
    }

    /// S * Fs + D * Fd as Core Animation's shaders compute it in half: the destination product rounded, then one fused
    /// multiply-add.
    static func halfFMA(_ s: Double, _ fs: Double, _ d: Double, _ fd: Double) -> Double {
        half(s * fs + half(d * fd))
    }

    /// Each product rounded to half, then their sum.
    static func halfSeparate(_ s: Double, _ fs: Double, _ d: Double, _ fd: Double) -> Double {
        half(half(s * fs) + half(d * fd))
    }

    static func exact(_ s: Double, _ fs: Double, _ d: Double, _ fd: Double) -> Double { s * fs + d * fd }
}
