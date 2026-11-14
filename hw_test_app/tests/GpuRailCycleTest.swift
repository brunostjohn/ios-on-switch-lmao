import Darwin
import HWTestCore

/// The GPU rail-gated and brought up again four times (debug.switch_gpu_rail_cycle, debug kernels): the gate and ungate
/// are the Tegra X1 TRM's GM20b sequence (12.2.7.17), and after each rail-up the kernel runs the GPU's first job (one
/// fence on a kernel channel) and reads its GPCPLL, which must be enabled, locked and on the idle clock's VCO at the PL
/// the idle clock last set. The display stays on, so the rail worker brings nothing down meanwhile.
struct GpuRailCycleTest: HWTest {
    let id = "gpu.rail_cycle"
    let name = "GPU rail-gate and ungate 4x: first job and GPCPLL after each"
    let subsystem = Subsystem.metal

    static let sysctl = "debug.switch_gpu_rail_cycle"
    static let cycles: Int32 = 4

    func run(_ context: TestContext) async throws -> Outcome {
        var cycles = Self.cycles
        guard sysctlbyname(Self.sysctl, nil, nil, &cycles, MemoryLayout<Int32>.size) == 0 else {
            let error = errno
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(error))
            return .skip(KernelSysctl.unreadable(Self.sysctl, error), metrics)
        }
        guard case .value(let line) = KernelSysctl.string(Self.sysctl) else {
            return .skip(KernelSysctl.unreadable(Self.sysctl, errno), Metrics())
        }
        let fields = DcHost1xClassTest.fields(line)
        var metrics = Metrics()
        let keys = [
            "cycles", "downs", "fences", "plls", "fence_us_max", "coeff", "cfg", "pl", "expect_pl", "khz", "why",
        ]
        for key in keys {
            if let value = fields[key] { metrics.set(key, value) }
        }
        let wanted = String(Self.cycles)
        return .judged(
            [
                ("every cycle rail-gated the GPU and brought it up (downs = cycles)", fields["downs"] == wanted),
                ("the first job after each rail-up completed (fences = cycles)", fields["fences"] == wanted),
                ("the GPCPLL was locked at the idle clock's PL after each (plls = cycles)", fields["plls"] == wanted),
            ], metrics)
    }
}
