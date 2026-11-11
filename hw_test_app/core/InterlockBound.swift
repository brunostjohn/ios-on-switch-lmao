import Foundation

/// gpu.ca.hot_states' check that the raw path's pixel interlock costs about what the m2v device's does for the same
/// Core Animation state: both run NAK's ticket lock over a ticket buffer, so the raw draw is held to `limit` times
/// m2v's. The raw path's GPU times are its stamps' nanoseconds; m2v's are PTIMER units read as nanoseconds
/// (CAHotStatesScene). The bound comes from the kernel's own per-job GPU times of the 8-draw command buffers
/// (`tegra-gm20b: jobs SwitchHWTest: ... slowest seqno`): TimgBvcm took 2.4 ms a draw with the ticket buffer cached
/// (probe-v1) and 26.1-34.1 ms with it bypassing the GPU's L2 (stack-S-v2), against m2v's 1812.5 PTIMER units, 2.95 ms:
/// 0.8 and 9 times m2v's. A raw time that is the host's window (no GPU stamp) is no GPU time and fails the check.
public enum InterlockBound {
    public static let limit = 2.0
    public static let ptimerNanoseconds = 1.6276

    /// Per state that interlocked on the raw device (`raw_ilk_<label>` > 0) and was timed on both devices: its raw time
    /// over m2v's, as `raw_ilk_x_<label>`, and a reason for each past `limit`.
    public static func judge(_ metrics: inout Metrics) -> [String] {
        var reasons: [String] = []
        for key in metrics.keys where key.hasPrefix("raw_ilk_") && !key.hasPrefix("raw_ilk_x_") {
            let label = String(key.dropFirst("raw_ilk_".count))
            guard let draws = value(metrics[key]), draws > 0, let raw = value(metrics["raw_us_" + label]),
                let m2v = value(metrics["m2v_us_" + label]), m2v > 0
            else { continue }
            if metrics["raw_gpu_clock_" + label] == .text("host") {
                reasons.append("\(label): the raw interlocked draw's time is the host's window, not the GPU's")
                continue
            }
            let ratio = raw / (m2v * ptimerNanoseconds)
            metrics.set("raw_ilk_x_" + label, ratio, digits: 2)
            if ratio > limit {
                let shown = MetricValue.format(ratio, digits: 1)
                reasons.append("\(label): the raw interlocked draw takes \(shown)x m2v's (limit \(limit)x)")
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
