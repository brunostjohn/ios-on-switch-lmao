import HWTestCore
import XCTest

/// InterlockBound on gpu.ca.hot_states' two interlocked states: probe-v1's cached ticket buffer within the factor,
/// stack-S-v2's (the ticket buffer bypassing the GPU's L2) past it, each against the same state's uncontended bands.
final class InterlockBoundTests: XCTestCase {
    static let bvcm = "pbgraxm_timgbvcma2xhfn_isrc", glass = "pb3a8bsovxmw_tgsaa3xhf_isrc"

    func run(bvcm: Double, glass: Double, apart: Double = 2000) -> (Metrics, [String]) {
        var metrics = Metrics()
        for (label, time) in [(Self.bvcm, bvcm), (Self.glass, glass)] {
            metrics.set("raw_us_" + label, time, digits: 1)
            metrics.set("raw_apart_us_" + label, apart, digits: 1)
            metrics.set("raw_ilk_" + label, 40)
        }
        metrics.set("raw_us_pbgraxm_tn27a2xhf_isrc", 9000.0, digits: 1)
        metrics.set("raw_ilk_pbgraxm_tn27a2xhf_isrc", 0)
        let reasons = InterlockBound.judge(&metrics)
        return (metrics, reasons)
    }

    func testCachedTicketsAreWithinTheFactor() {
        let (metrics, reasons) = run(bvcm: 2380, glass: 2650)
        XCTAssertEqual(reasons, [])
        XCTAssertEqual(metrics["raw_ilk_x_" + Self.bvcm], .number(2380.0 / 2000, digits: 2))
        XCTAssertEqual(metrics["raw_ilk_ms_" + Self.bvcm], .number(2.38, digits: 3))
        XCTAssertEqual(metrics["raw_apart_ms_" + Self.bvcm], .number(2.0, digits: 3))
        XCTAssertNil(metrics["raw_ilk_x_pbgraxm_tn27a2xhf_isrc"])
    }

    func testUncachedTicketsFailTheFactor() {
        let (_, reasons) = run(bvcm: 26600, glass: 19600)
        XCTAssertEqual(reasons.count, 2)
        XCTAssertTrue(reasons[1].hasPrefix(Self.bvcm + ": the interlocked draw takes 13.3x"), reasons[1])
    }

    func testAHostWindowIsNoGpuTime() {
        var metrics = run(bvcm: 2380, glass: 2650).0
        metrics.set("raw_gpu_clock_" + Self.bvcm, "host")
        let reasons = InterlockBound.judge(&metrics)
        XCTAssertEqual(reasons, [Self.bvcm + ": the raw interlocked draw's time is the host's window, not the GPU's"])
    }

    func testAnInterlockedStateNeedsItsUncontendedTimeAndAFactor() {
        var metrics = Metrics()
        metrics.set("raw_us_pbgraxm_tn27a2xhf_isrc", 900.0, digits: 1)
        metrics.set("raw_ilk_pbgraxm_tn27a2xhf_isrc", 8)
        XCTAssertEqual(
            InterlockBound.judge(&metrics),
            ["pbgraxm_tn27a2xhf_isrc: interlocked, with no uncontended time to hold it to"])
        metrics.set("raw_apart_us_pbgraxm_tn27a2xhf_isrc", 800.0, digits: 1)
        XCTAssertEqual(
            InterlockBound.judge(&metrics), ["pbgraxm_tn27a2xhf_isrc: interlocked, and its class has no stated factor"])
    }
}
