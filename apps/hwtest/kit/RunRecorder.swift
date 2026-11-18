import Foundation
import HWTestCore
import os

/// The run on the card (RunStore, RunJournal), written from one serial queue: the runner's lines and the app's
/// interruptions land in the order they happen. The main thread waits on it only for an interruption's line (no flush)
/// and a person's run choice, never once a test; the queue runs at the runner's priority because the runner waits for
/// it between tests.
final class RunRecorder: @unchecked Sendable {
    private let store: RunStore
    private let queue = DispatchQueue(label: "com.brunostjohn.switch.hwtest.journal", qos: .userInitiated)
    /// Only touched on `queue`.
    private var journal: RunJournal?

    init(store: RunStore) {
        self.store = store
    }

    /// A batch's start: `report`'s journal, unless the journal holds it already (a run just continued).
    func start(_ report: Report) {
        queue.async { [self] in
            guard journal?.report != report else { return }
            attempt("\(report.fileName): journal not started") { journal = try store.open(report) }
        }
    }

    /// `finished`'s result and `next`'s begin on the card; the milliseconds that took.
    func advance(_ finished: TestRecord?, begin next: String?) async -> Double {
        await withCheckedContinuation { continuation in
            queue.async { [self] in
                let start = CFAbsoluteTimeGetCurrent()
                attempt("journal line not written") { try journal?.advance(finished, begin: next) }
                continuation.resume(returning: (CFAbsoluteTimeGetCurrent() - start) * 1000)
            }
        }
    }

    /// Waits for the line: the app may be terminating, and the next launch tells a closed app from a crash by it.
    func interrupt(_ reason: String?) {
        queue.sync {
            attempt("interruption not written") { try journal?.interrupt(reason) }
        }
    }

    /// A batch's end: a run with nothing left is finished and closed (written whole, its journal removed). The run as
    /// it stands; nil when its journal never started.
    func finish(at date: Date) async -> Report? {
        await withCheckedContinuation { continuation in
            queue.async { [self] in
                guard let open = journal else { return continuation.resume(returning: nil) }
                if open.report.remaining.isEmpty && open.report.run.current == nil {
                    attempt("\(open.report.fileName): run not closed") {
                        try open.finish(at: date)
                        try store.close(open.report)
                    }
                    journal = nil
                    store.prune()
                }
                continuation.resume(returning: open.report)
            }
        }
    }

    /// A person's choice of an unfinished run, on the card before the run goes on: its journal from `continued`.
    func checkpoint(_ continued: Report) throws {
        try queue.sync { journal = try store.open(continued) }
    }

    /// A person's choice to end a run (cancelled, or continued with nothing left): written whole.
    func close(_ ended: Report) throws {
        try queue.sync {
            journal = nil
            try store.close(ended)
            store.prune()
        }
    }

    private func attempt(_ what: String, _ work: () throws -> Void) {
        do {
            try work()
        } catch {
            Suite.results.error("\(what, privacy: .public): \(String(describing: error), privacy: .public)")
        }
    }
}
