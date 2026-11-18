import Foundation

/// What Continue makes of an unfinished run: the run as it goes on and the tests left, in the order they run.
public struct Continuation {
    public enum Cause: Equatable {
        /// The run stopped between tests.
        case none
        /// The device restarted while the test ran: it runs last.
        case restarted(String)
        /// The app was closed or sent to the background (`reason`) while the test ran: it runs again.
        case interrupted(String, reason: String)
        /// The app's process ended while the test ran: it is recorded as a crash.
        case crashed(String)
    }

    public let report: Report
    /// The tests left, in run order; empty when the run is finished.
    public let order: [String]
    public let cause: Cause
    /// The crash's record, when the cause is a crash.
    public let crash: TestRecord?
}

extension Report {
    /// The run continued on boot `boot`. `available`: the tests this build has (a test it lacks is recorded as a
    /// SKIP); `record` makes a test's record with an outcome the continuation gives it.
    public func continued(
        boot: String, at date: Date, identity: [String: String], available: Set<String>,
        record: (String, Outcome) -> TestRecord
    ) -> Continuation {
        var report = self
        var cause = Continuation.Cause.none
        var crash: TestRecord?
        if let current = report.run.current {
            if report.latestBootUUID != boot {
                report.requeueInterrupted()
                cause = .restarted(current)
            } else if let reason = report.run.interruption {
                report.requeueInterrupted()
                cause = .interrupted(current, reason: reason)
            } else {
                let made = record(current, Report.crashOutcome(current))
                report.record(made)
                crash = made
                cause = .crashed(current)
            }
        }
        report.interrupt(nil)
        report.resume(bootUUID: boot, at: date, identity: identity)
        for id in report.remaining where !available.contains(id) {
            report.record(record(id, .skip("test is not available in this build")))
        }
        var order = report.remaining
        if case .restarted(let current) = cause, let index = order.firstIndex(of: current) {
            order.append(order.remove(at: index))
        }
        if order.isEmpty { report.finish(at: date) }
        return Continuation(report: report, order: order, cause: cause, crash: crash)
    }
}
