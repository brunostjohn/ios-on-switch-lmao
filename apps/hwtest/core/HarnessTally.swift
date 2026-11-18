import Foundation

/// The suite's own time between the tests of one batch, logged after it as `HWTEST suite.harness`: each boundary (one
/// test's measured end to the next one's start: its log line, the list's update, the journal's flush), the journal's
/// flush inside it, and how long the main thread took to answer just before the next test started. FAIL when the
/// median boundary or the median answer passes its ceiling: the suite's work is then back between or inside the tests.
public struct HarnessTally {
    public static let id = "suite.harness"
    /// Two main-thread hops and one journal line flushed to the card (APFS's transaction flush: 36 ms median, boot 2).
    public static let boundaryCeilingMs = 100.0
    /// One display period: an idle main thread answers well inside it.
    public static let mainWaitCeilingMs = 16.7

    private var boundaries: [Double] = []
    private var flushes: [Double] = []
    private var mainWaits: [Double] = []

    public init() {}

    public mutating func add(boundaryMs: Double, flushMs: Double, mainWaitMs: Double) {
        boundaries.append(boundaryMs)
        flushes.append(flushMs)
        mainWaits.append(mainWaitMs)
    }

    public var outcome: Outcome {
        let boundary = Stats.percentile(boundaries, 0.5), mainWait = Stats.percentile(mainWaits, 0.5)
        var metrics = Metrics()
        metrics.set("boundaries", boundaries.count)
        metrics.set("boundary_ms_p50", boundary, digits: 1)
        metrics.set("boundary_ms_max", boundaries.max() ?? 0, digits: 1)
        metrics.set("flush_ms_p50", Stats.percentile(flushes, 0.5), digits: 1)
        metrics.set("flush_ms_max", flushes.max() ?? 0, digits: 1)
        metrics.set("main_wait_ms_p50", mainWait, digits: 1)
        metrics.set("main_wait_ms_max", mainWaits.max() ?? 0, digits: 1)
        metrics.set("main_late", mainWaits.filter { $0 > Self.mainWaitCeilingMs }.count)
        var over: [String] = []
        if boundary > Self.boundaryCeilingMs { over.append("median boundary over \(Int(Self.boundaryCeilingMs)) ms") }
        if mainWait > Self.mainWaitCeilingMs { over.append("median main-thread answer over one display period") }
        return over.isEmpty ? .pass(metrics) : .fail(over.joined(separator: "; "), metrics)
    }

    /// The batch's line: ms= is the time the batch spent between its tests.
    public var line: String {
        LogLine.format(id: Self.id, outcome: outcome, milliseconds: boundaries.reduce(0, +))
    }
}
