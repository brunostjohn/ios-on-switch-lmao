import Foundation

/// gpu.ca.hot_states' check that the pixel interlock costs a Core Animation state no more than its class's factor over
/// the same state's draws when no fragment waits on another. Per state that interlocked on the raw device
/// (raw_ilk_<label> > 0): its full-screen draw over the seven before it (raw_us_<label>) against the same pixels drawn in
/// the same run as eight disjoint bands (raw_apart_us_<label>), both timed by the raw path's GPU stamps. A fetching draw
/// runs its state's program inside the interlock either way (raw_fetch_order.h), so the ratio is what the waits cost: the
/// ticket buffer bypassing the GPU's L2 (stack-S-v2) made TimgBvcm's overlapping draw 26.1-34.1 ms against 2.4 ms with it
/// cached (probe-v1). A time that is the host's window (no GPU stamp) is no GPU time and fails the check, and so does an
/// interlocked state with no uncontended time or no stated factor.
public enum InterlockBound {
    /// The factor per class of interlocked state, by the label's texture function: TimgBvcm's colour-matrix blend that
    /// fetches its destination, and the glass's sample (Tgsa) beside its R32Float and RGBA16Float attachments.
    public static let factors: [(texture: String, factor: Double)] = [("timgbvcm", 2.0), ("tgsa", 2.0)]

    public static func factor(_ label: String) -> Double? {
        factors.first { label.contains("_" + $0.texture) }?.factor
    }

    /// Per interlocked state: raw_ilk_x_<label>, its overlapping time over its uncontended one, both as milliseconds
    /// (raw_ilk_ms_<label>, raw_apart_ms_<label>) for comparisons across builds, and a reason for each past its factor or
    /// that cannot be judged.
    public static func judge(_ metrics: inout Metrics) -> [String] {
        var reasons: [String] = []
        let derived = ["raw_ilk_x_", "raw_ilk_ms_"]
        for key in metrics.keys.sorted()
        where key.hasPrefix("raw_ilk_") && !derived.contains(where: { key.hasPrefix($0) }) {
            let label = String(key.dropFirst("raw_ilk_".count))
            guard let draws = value(metrics[key]), draws > 0, let raw = value(metrics["raw_us_" + label]) else {
                continue
            }
            if metrics["raw_gpu_clock_" + label] == .text("host") {
                reasons.append("\(label): the raw interlocked draw's time is the host's window, not the GPU's")
                continue
            }
            guard let apart = value(metrics["raw_apart_us_" + label]), apart > 0 else {
                reasons.append("\(label): interlocked, with no uncontended time to hold it to")
                continue
            }
            guard let factor = factor(label) else {
                reasons.append("\(label): interlocked, and its class has no stated factor")
                continue
            }
            let ratio = raw / apart
            metrics.set("raw_ilk_x_" + label, ratio, digits: 2)
            metrics.set("raw_ilk_ms_" + label, raw / 1000, digits: 3)
            metrics.set("raw_apart_ms_" + label, apart / 1000, digits: 3)
            if ratio > factor {
                let shown = MetricValue.format(ratio, digits: 1)
                reasons.append("\(label): the interlocked draw takes \(shown)x its uncontended time (limit \(factor)x)")
            }
        }
        return reasons
    }

    static func value(_ metric: MetricValue?) -> Double? {
        switch metric {
        case .integer(let value): return Double(value)
        case .number(let value, _): return value
        default: return nil
        }
    }
}
