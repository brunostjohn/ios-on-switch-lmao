import Darwin
import HWTestCore

/// Each live client channel's RAMFC timeslice read back against the value the kernel last wrote to it, once the channel
/// has completed a job since (debug.switch_gpu_ramfc, debug kernels). A level change's timeslice is written only after a
/// preempt of the channel has completed, so the host's save of the channel cannot land on top of it; a write that raced
/// that save reads back as the old value. The compositor's channels went high at its first frame, so they are among
/// those checked.
struct RamfcTimesliceTest: HWTest {
    let id = "gpu.ramfc_timeslice"
    let name = "RAMFC timeslice after a level change: read back on every client channel"
    let subsystem = Subsystem.metal

    static let sysctl = "debug.switch_gpu_ramfc"

    func run(_ context: TestContext) async throws -> Outcome {
        let read = KernelSysctl.string(Self.sysctl)
        guard case .value(let line) = read else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(read.error))
            return .skip(KernelSysctl.unreadable(Self.sysctl, read.error), metrics)
        }
        let fields = DcHost1xClassTest.fields(line)
        var metrics = Metrics()
        for key in ["channels", "checked", "high", "mismatched", "first_chid", "first_ramfc", "first_expected"] {
            if let value = fields[key] { metrics.set(key, value) }
        }
        return .judged(
            [
                (
                    "at least one client channel completed a job after its last timeslice write",
                    (Int(fields["checked"] ?? "") ?? 0) > 0
                ),
                (
                    "every checked channel's RAMFC timeslice is the value last written (mismatched = 0)",
                    fields["mismatched"] == "0"
                ),
            ], metrics)
    }
}
