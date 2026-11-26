import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.perf.rates' arms at small sizes on this Mac's GPU: each runs, is timed and gives a finite rate above 0, so a
/// broken arm fails here and not on the Switch.
final class GPURatesTests: XCTestCase {
    func testEveryArmGivesARate() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "GPURates.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        let result = try await GPURates.run(GPU(device: device, queue: queue, library: library), .host)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        let rates = [
            "alu_gflops", "lane_mhz", "copy_gbps", "read_gbps_private", "read_gbps_private_pages", "read_gbps_shared",
            "read_gbps_shared_pages", "fill_gpix_s", "blend_gpix_s", "fill_shared_gpix_s", "fill4x_gpix_s",
            "depth_gpix_s", "texel_gtexel_s", "tri_mtri_s", "clipped_mtri_s", "hidden_us_per_layer",
            "shaded_us_per_layer", "atomic_ns_cached", "fetch_mtri_s_cached", "table_us_cached",
            "jobs_us_cached",
        ]
        let line = result.metrics.ordered.map { "\($0)=\($1.logText)" }.joined(separator: " ")
        print("GPURates \(HostFacts.fields(device)) \(line)")
        for key in rates {
            guard case .number(let value, _) = result.metrics[key] else {
                XCTFail("\(key) missing")
                continue
            }
            XCTAssertTrue(value.isFinite && value > 0, "\(key)=\(value)")
        }
    }

    /// gpu.perf.knobs at small sizes: with no probes on this Mac, the default knob and no promotion, each arm timed
    func testTheKnobArmsRunWithoutTheProbes() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "GPURates.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        let result = try await GPURates.knobs(GPU(device: device, queue: queue, library: library), .host)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.notes.count, 2, "both probes are absent here")
        for key in [
            "subtile_one_default_us", "subtile_four_default_us", "subtile_depth_default_us", "subtile_fill_default_us",
            "promo_minified_0_us", "promo_one_0_us", "promo_scatter_0_us",
        ] {
            guard case .number(let value, _) = result.metrics[key] else {
                XCTFail("\(key) missing")
                continue
            }
            XCTAssertTrue(value.isFinite && value > 0, "\(key)=\(value)")
        }
    }
}
