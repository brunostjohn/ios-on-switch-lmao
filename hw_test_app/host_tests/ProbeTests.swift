import Foundation
import HWTestCore
import XCTest

final class ProbeTests: XCTestCase {
    func line(_ finding: ProbeFinding) -> String {
        var base = Metrics()
        base.set("paths", "raw/1")
        let outcome = finding.outcome(probe: "d23", base: base, proof: "GM20B")
        return LogLine.format(id: "gpu.x1.rop_conversion", outcome: outcome, milliseconds: 1)
    }

    func testARunProbeIsAPassingLineCarryingTheClaimsVerdict() {
        let finding = ProbeFinding.counted(["unorm16_wrong": 3, "srgb8_wrong": 0], paths: ["raw"])
        XCTAssertEqual(finding.verdict, .fail)
        XCTAssertEqual(
            line(finding),
            "HWTEST gpu.x1.rop_conversion PASS probe=d23 claim_verdict=FAIL paths=raw/1 srgb8_wrong=0 unorm16_wrong=3 "
                + "hw=GM20B ms=1.0 why=wrong:_unorm16_wrong")
    }

    func testNoWrongCountPasses() {
        XCTAssertEqual(ProbeFinding.counted(["a_wrong": 0], paths: ["raw", "raw"]).verdict, .pass)
    }

    func testWorkOffTheRawPathDecidesNothing() {
        let finding = ProbeFinding.counted(["a_wrong": 0], paths: ["raw", "m2v:pipeline:compile"])
        XCTAssertEqual(finding.verdict, .inconclusive)
        XCTAssertEqual(finding.note, "not on the raw path: m2v:pipeline:compile")
    }
}
