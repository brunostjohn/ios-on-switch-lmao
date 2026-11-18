import Foundation
import HWTestCore
import XCTest

/// The Mac golden files as the device reads them (CAGoldens): a line it cannot read is skipped there, and its scene or
/// state then reports no golden at all, so every line must name at least one attachment's signature.
final class GoldenFilesTests: XCTestCase {
    func testEveryGoldenLineGivesItsKeyASignature() throws {
        let environment = ProcessInfo.processInfo.environment
        let root = environment["TEST_SRCDIR"].map { $0 + "/" } ?? ""
        let paths = try XCTUnwrap(environment["HWTEST_GOLDEN_FILES"]).split(separator: " ")
        XCTAssertEqual(paths.count, 4)
        for path in paths {
            let text = try String(contentsOf: URL(fileURLWithPath: root + path), encoding: .utf8)
            for line in text.split(separator: "\n") where !line.hasPrefix("#") {
                let key = String(line.prefix { $0 != " " })
                let read = CAGoldens(String(line)).states[key] ?? [:]
                XCTAssertFalse(read.isEmpty, "\(path): \(line.prefix(80))")
            }
        }
    }
}
