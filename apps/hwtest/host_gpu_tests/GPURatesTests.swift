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
            "shaded_us_per_layer",
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
}
