import Foundation

/// Every purgeable mark any process asked APFS for since boot (kern.switch_fsctl_marks, debug kernels;
/// kernel/switch/vfs/fsctl_diag.c), by outcome. SplashBoard keeps an app's launch snapshot only once APFS marks it, and
/// an app's sandbox denies it the mark itself, so the system's own marks are what system.snapshot_purgeable judges.
public struct PurgeableMarks: Equatable, Sendable {
    public let ok: Int
    /// ENOTSUP: the Data volume lacks the purgeable feature bits, as on a card imaged before 34c147a35
    public let unsupported: Int
    /// Refused on a decmpfs-compressed file: SplashBoard's compressed snapshots
    public let compressed: Int
    public let other: Int

    public init(ok: Int, unsupported: Int, compressed: Int, other: Int) {
        self.ok = ok
        self.unsupported = unsupported
        self.compressed = compressed
        self.other = other
    }

    public var asked: Int { ok + unsupported + compressed + other }

    /// "ok=N unsupported=N compressed=N other=N"; nil for anything else.
    public static func parse(_ line: String) -> PurgeableMarks? {
        var values: [String: Int] = [:]
        for field in line.split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1)
            if pair.count == 2, let value = Int(pair[1]) { values[String(pair[0])] = value }
        }
        guard let ok = values["ok"], let unsupported = values["unsupported"], let compressed = values["compressed"],
              let other = values["other"]
        else { return nil }
        return PurgeableMarks(ok: ok, unsupported: unsupported, compressed: compressed, other: other)
    }

    /// One check per cause, so a run shows which of them still holds.
    public var checks: [(String, Bool)] {
        [
            (
                "no mark refused for the Data volume's missing purgeable features (\(unsupported); a card imaged "
                    + "before 34c147a35 needs imaging again)", unsupported == 0
            ),
            ("no mark refused on a compressed snapshot (\(compressed))", compressed == 0),
            ("no mark refused otherwise (\(other))", other == 0),
            ("APFS marked a file purgeable (\(ok))", ok > 0),
        ]
    }
}
