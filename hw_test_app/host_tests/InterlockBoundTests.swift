import HWTestCore
import XCTest

/// InterlockBound against gpu.ca.hot_states' two interlocked states on device: probe-v1 within the bound,
/// stack-S-v2 (the ticket buffer bypassing the GPU's L2) past it.
final class InterlockBoundTests: XCTestCase {
    func run(rawBvcm: Double, rawGlass: Double) -> (Metrics, [String]) {
        var metrics = Metrics()
        metrics.set("raw_us_timgbvcm", rawBvcm, digits: 1)
        metrics.set("m2v_us_timgbvcm", 1812.5, digits: 1)
        metrics.set("raw_ilk_timgbvcm", 40)
        metrics.set("raw_us_glass", rawGlass, digits: 1)
        metrics.set("m2v_us_glass", 1718.4, digits: 1)
        metrics.set("raw_ilk_glass", 40)
        metrics.set("raw_us_tn27", 9000.0, digits: 1)
        metrics.set("m2v_us_tn27", 2200.0, digits: 1)
        metrics.set("raw_ilk_tn27", 0)
        let reasons = InterlockBound.judge(&metrics)
        return (metrics, reasons)
    }

    func testProbeBuildIsWithinTheBound() {
        let (metrics, reasons) = run(rawBvcm: 2380, rawGlass: 2650)
        XCTAssertEqual(reasons, [])
        XCTAssertEqual(metrics["raw_ilk_x_timgbvcm"], .number(2380 / (1812.5 * 1.6276), digits: 2))
        XCTAssertNil(metrics["raw_ilk_x_tn27"])
    }

    func testUncachedTicketsFailTheBound() {
        let (_, reasons) = run(rawBvcm: 26500, rawGlass: 19600)
        XCTAssertEqual(reasons.count, 2)
        XCTAssertTrue(reasons[0].hasPrefix("timgbvcm: the raw interlocked draw takes 9.0x m2v's"), reasons[0])
    }
}
