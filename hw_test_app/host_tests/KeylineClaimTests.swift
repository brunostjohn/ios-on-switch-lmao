import Foundation
import HWTestCore
import XCTest

final class KeylineClaimTests: XCTestCase {
    func evidence(
        line: [Int], backdrop: [Int]? = [20, 20, 21], framebuffer: [Int]? = nil, replica: String? = ""
    ) -> KeylineClaim.Evidence {
        KeylineClaim.Evidence(
            line: line, backdrop: backdrop, framebuffer: framebuffer, replicaWrong: replica, alpha: 0.1)
    }

    func testTheKeylinesWhiteOverTheBackdropIsSourceOverOfTheHalfAlpha() {
        XCTAssertEqual(KeylineClaim.half(0.1), 0.0999755859375)
        XCTAssertEqual(KeylineClaim.expected(over: [20, 20, 21], alpha: 0.1), [43, 43, 44])
        XCTAssertEqual(KeylineClaim.expected(over: [255, 0, 128], alpha: 0.1), [255, 25, 141])
    }

    func testTheDevicesRowIsTheFreshTexelAndBNamesItWhenTheReplicaIsExact() {
        let (claim, why) = KeylineClaim.verdict(evidence(line: [255, 0, 255], framebuffer: [255, 0, 255]))
        XCTAssertEqual(claim, "b")
        XCTAssertTrue(why.contains("fresh"))
        XCTAssertTrue(KeylineClaim.fresh([254, 0, 254]))
        XCTAssertFalse(KeylineClaim.fresh([255, 25, 255]))
    }

    func testAWrongReplicaIsTheDrawsOwn() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [255, 0, 255], replica: "c0.g")).claim, "a")
    }

    func testAPurpleRowWithoutAReplicaDecidesNeither() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [250, 10, 240], replica: nil)).claim, "a_or_b")
    }

    func testCleanRenderAndFramebufferPutTheLineAfterTheFramebuffer() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 43, 44], framebuffer: [43, 43, 44])).claim, "d")
    }

    func testAChannelOffByTheROPsRoundingIsC() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 42, 44])).claim, "c")
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 42, 44], framebuffer: [43, 42, 44])).claim, "c")
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 43, 44])).claim, "none")
    }

    func testAPurpleFramebufferUnderACleanSnapshotIsB() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 43, 44], framebuffer: [255, 0, 255])).claim, "b")
    }
}
