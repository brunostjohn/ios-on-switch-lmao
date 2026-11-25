import HWTestCore
import XCTest

final class ControllerSkipTests: XCTestCase {
    /// 25 Nov 19:31: both Joy-Cons on the rails (input.rail rails=L,R), skipped as "no game controller connected"
    func testRailJoyConsNameThePressThatPublishesThem() {
        let why = ControllerSkip.reason(railsRunning: ["R", "L"])
        XCTAssertTrue(why.hasPrefix("the Joy-Cons on rails L,R publish a game controller only after a gamepad press"), why)
        XCTAssertTrue(why.hasSuffix("Run All presses nothing"), why)
    }

    func testNoRailSaysNothingIsConnected() {
        XCTAssertEqual(
            ControllerSkip.reason(railsRunning: []), "no Joy-Con on a rail and no other game controller connected"
        )
        XCTAssertEqual(ControllerSkip.reason(railsRunning: nil), ControllerSkip.reason(railsRunning: []))
    }
}
