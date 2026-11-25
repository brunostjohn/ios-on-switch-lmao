import Foundation

/// HWTEST_LONG=1 in the app's environment runs the long forms Run All leaves out: the barrier probes' full trial
/// targets (bound95 about 0.002 a cell) and the timings that are reported, not judged.
public enum LongRuns {
    public static let on = ProcessInfo.processInfo.environment["HWTEST_LONG"] == "1"
}
