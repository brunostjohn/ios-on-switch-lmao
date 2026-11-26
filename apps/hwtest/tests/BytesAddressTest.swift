import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Two setVertexBytes bindings whose Metal addresses the vertex function compares (gpu/BytesAddress.swift), as Weather's
/// VFX reads buffer 5's: the pixel holds slot 5's magenta, as on an M4 (host_gpu_tests/BytesAddressTests.swift). Each
/// draw writes a bytes binding's upload copy into its SlotGuestBase hole (the driver's Pipeline::guestBases); the record
/// carries the holes once the pack build presents such a slot, until then the state is refused (MissingGuestAddress):
/// `pixel=` the bytes read, `path=`.
struct BytesAddressTest: HWTest {
    let id = "gpu.gm20b.bytes_address"
    let name = "A vertex function comparing two setVertexBytes bindings' addresses sees two addresses"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        let ran: (pixel: [UInt8], path: String)
        do {
            ran = try await BytesAddress.run(gpu)
        } catch {
            metrics.set("error", String(describing: error).prefix(100).description)
            return .fail("refused", metrics)
        }
        metrics.set("pixel", ran.pixel.map { String($0) }.joined(separator: "."))
        metrics.set("path", ran.path)
        let right = ran.pixel == [255, 0, 255, 255] && ran.path == "gm20b"
        return right ? .pass(metrics) : .fail("pixel", metrics)
    }
}
