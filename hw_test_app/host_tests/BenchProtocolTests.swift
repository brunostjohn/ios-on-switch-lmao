import Foundation
import HWTestCore
import XCTest

/// The bench loop's decoder against tools/benchloop's golden batch, and the result files the Mac parses.
final class BenchProtocolTests: XCTestCase {
    func golden() throws -> Data {
        let environment = ProcessInfo.processInfo.environment
        let path = try XCTUnwrap(environment["BENCH_GOLDEN"])
        let root = environment["TEST_SRCDIR"].map { $0 + "/" } ?? ""
        return try Data(contentsOf: URL(fileURLWithPath: root + path))
    }

    func testEveryKindOfTheGoldenBatchDecodes() throws {
        let batch = try BenchBatch.decode(try golden())
        XCTAssertEqual(batch.name, "golden")
        let experiments = batch.entries.compactMap { entry -> BenchExperiment? in
            if case .experiment(let e) = entry { return e }
            XCTFail("\(entry.id) did not decode: \(entry)")
            return nil
        }
        XCTAssertEqual(experiments.map(\.kind), [.noop, .known, .fragment, .compute, .readback])
        let fragment = experiments[2]
        XCTAssertEqual(fragment.formatRaw, 125)
        XCTAssertEqual(fragment.rect, [0, 0, 8, 4])
        XCTAssertEqual(fragment.draws, [1, 2, 4])
        XCTAssertEqual(fragment.program?.sph.count, 80)
        XCTAssertEqual(fragment.program?.gprs, 4)
        XCTAssertEqual(fragment.commandBuffers, 21)
        let readback = experiments[4]
        XCTAssertEqual(readback.methods, [0xDD4, 1, 0xDF0, 0, 0xDF4, 0x302A_0000, 0xDD0, 3])
        XCTAssertEqual(readback.restore, [0xDD0, 0, 0xDD4, 0])
        XCTAssertEqual(readback.fill?.count, 4 * 2 * 4)
        XCTAssertEqual(experiments[3].threads, [32, 1, 1])
    }

    func testAnExperimentTheRawPathMayNotTakeIsKeptAsInvalid() throws {
        let batch: [String: Any] = [
            "protocol": "benchloop/1", "batch": "b",
            "experiments": [
                [
                    "id": "owned", "kind": "readback", "target": ["format": "bgra8unorm", "width": 4, "height": 4],
                    "program": [
                        "sph": Data(count: 80).base64EncodedString(), "code": Data(count: 32).base64EncodedString(),
                        "gprs": 4,
                    ], "methods": [[0x0200, 1]],
                ],
                ["id": "big", "kind": "noop", "target": ["format": "rgba16float", "width": 4096, "height": 4]],
                ["id": "ok", "kind": "noop", "target": ["format": "rgba16float", "width": 64, "height": 4]],
            ],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
        guard case .invalid(_, _, let why) = decoded.entries[0] else { return XCTFail("owned method accepted") }
        XCTAssertTrue(why.contains("not allowed"), why)
        guard case .invalid = decoded.entries[1] else { return XCTFail("4096 wide accepted") }
        guard case .experiment = decoded.entries[2] else { return XCTFail("a plain noop refused") }
    }

    func testAReadbackMayBindColourZeroAlone() throws {
        let program: [String: Any] = [
            "sph": Data(count: 80).base64EncodedString(), "code": Data(count: 32).base64EncodedString(), "gprs": 4,
        ]
        let target: [String: Any] = ["format": "bgra8unorm_srgb", "width": 4, "height": 4]
        let batch: [String: Any] = [
            "protocol": "benchloop/1", "batch": "b",
            "experiments": [
                [
                    "id": "one", "kind": "readback", "target": target, "program": program, "targets": 1,
                    "template": "1b1ae4ebb482e4d1",
                ],
                ["id": "two", "kind": "readback", "target": target, "program": program, "targets": 2],
                ["id": "frag", "kind": "fragment", "target": target, "program": program, "targets": 1],
            ],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
        guard case .experiment(let one) = decoded.entries[0] else { return XCTFail("targets 1 refused") }
        XCTAssertTrue(one.singleTarget)
        XCTAssertEqual(one.formatRaw, 81)
        XCTAssertEqual(one.templateKey, 0x1b1a_e4eb_b482_e4d1)
        guard case .invalid = decoded.entries[1] else { return XCTFail("targets 2 accepted") }
        guard case .invalid = decoded.entries[2] else { return XCTFail("a fragment experiment chose its targets") }
    }

    func testATwodExperimentCarriesItsOperation() throws {
        let batch: [String: Any] = [
            "protocol": "benchloop/1", "batch": "b",
            "experiments": [
                [
                    "id": "fill", "kind": "twod", "target": ["format": "bgra8unorm", "width": 8, "height": 8],
                    "rect": [1, 1, 7, 7],
                    "twod": ["op": "fill", "colorFormat": 0xCF, "color": [0xFF00_80FF, 0, 0, 0]],
                ],
                [
                    "id": "half", "kind": "twod", "target": ["format": "bgra8unorm", "width": 8, "height": 8],
                    "twod": ["op": "scale", "du": 1 << 33, "dv": 1 << 33, "bilinear": true],
                ],
                ["id": "none", "kind": "twod", "target": ["format": "bgra8unorm", "width": 8, "height": 8]],
            ],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
        guard case .experiment(let fill) = decoded.entries[0], let f = fill.twod else { return XCTFail("fill refused") }
        XCTAssertEqual([f.colorFormat, f.color[0]], [0xCF, 0xFF00_80FF])
        XCTAssertFalse(f.scale)
        guard case .experiment(let half) = decoded.entries[1], let h = half.twod else {
            return XCTFail("scale refused")
        }
        XCTAssertEqual(h.du, 1 << 33)
        XCTAssertTrue(h.scale && h.bilinear)
        guard case .invalid = decoded.entries[2] else { return XCTFail("a twod experiment without its operation ran") }
    }

    func testResultFileCarriesRunsAndTexels() throws {
        let batch = try BenchBatch.decode(try golden())
        var outcome = BenchOutcome()
        outcome.runs = [(1, [100, 120], [5000, 5100])]
        outcome.texels = Data([1, 2, 3, 4, 1, 2, 3, 4, 9, 9, 9, 9, 1, 2, 3, 4]) + Data(count: 4 * 2 * 4 - 16)
        let bytes = BenchResult.encode(batch: "golden", entry: batch.entries[4], outcome: outcome)
        let top = try XCTUnwrap(JSONSerialization.jsonObject(with: bytes) as? [String: Any])
        XCTAssertEqual(top["protocol"] as? String, "benchloop/1")
        XCTAssertEqual(top["status"] as? String, "ok")
        let texels = try XCTUnwrap(top["texels"] as? [String: Any])
        let distinct = try XCTUnwrap(texels["distinct"] as? [[String: Any]])
        XCTAssertEqual(distinct.map { $0["value"] as? String }, ["01020304", "09090909", "00000000"])
        XCTAssertEqual(distinct.map { $0["count"] as? Int }, [3, 1, 4])
        XCTAssertEqual(distinct[1]["first"] as? [Int], [2, 0])
        XCTAssertNotNil(texels["data"])
        let line = BenchResult.logLine(batch: "golden", id: "readback", outcome: outcome)
        XCTAssertTrue(line.hasPrefix("BENCH golden readback ok runs=1:120 crc="), line)
    }

    func testCrcIsZlibs() {
        XCTAssertEqual(BenchResult.crc32(Data("123456789".utf8)), 0xCBF4_3926)
    }

    func testJournalNamesWhatWasRunningWhenTheProcessEnded() {
        let text = "starting a\ndone a ok\nstarting b\n"
        XCTAssertEqual(BenchJournal.crashed(text), ["b"])
        XCTAssertEqual(BenchJournal.finished(text), ["a"])
    }
}
