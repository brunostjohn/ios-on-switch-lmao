import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import IOSurface
import Metal

/// NVJPG into a BGRA surface GM20B laid out block-linear (a GPU sampler asked first), 256x256 (16-GOB blocks), on a
/// debug kernel. route: the decode takes the copy engine into the blocks (TegraNVJPGRoute.h: the T210's NVJPG writes
/// rows), route_taken= the route counts' change, and the CPU read is the picture (route_wrong= bytes past
/// JPEGSurfaceDecode.tolerance). The tile probes, reported and not judged: NVJPG told tile_mode 1 or 2 and block height
/// codes 2 to 4 (16-GOB blocks are code 4, or code 3 if the decoder codes as the encoder does) writes the
/// destination's blocks itself; m<mode>_g<code>= the layout its bytes match
/// (JPEGSurfaceDecode.explained: rows, y<n> GM20B's Tegra GOBs, fermi_y<n> NIL's Fermi GOBs, - none, exact when
/// m<mode>_g<code>_wrong=0), which settles whether any tiling setup writes GM20B's layout natively.
struct NVJPGTileProbeTest: HWTest {
    let id = "image.nvjpg_tile_probe"
    let name = "NVJPG into a block-linear surface goes through the copy engine; what each tiling setup writes"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 60 }

    static let size = 256
    static let probes: [(mode: UInt32, gob: UInt32)] = [(1, 2), (1, 3), (1, 4), (2, 2), (2, 3), (2, 4)]

    func run(_ context: TestContext) async throws -> Outcome {
        guard case .success(let first) = JPEGRoute.decodes() else {
            return .skip("no route counts: a release kernel")
        }
        guard let file = JPEGSurfaceDecode.file(Self.size) else { throw TestFailure("no JPEG of \(Self.size)") }
        let gpu = try GPU.make()
        var metrics = Metrics()
        var checks: [(String, Bool)] = []

        let (surface, sampler) = try await Self.blockLinear(gpu)
        let failure = JPEGSurfaceDecode.decode(file, into: surface)
        let taken = (try? JPEGRoute.decodes().get()).map { JPEGRoute.routes(from: first, to: $0) } ?? "-"
        let (wrong, _) = JPEGSurfaceDecode.wrong(SurfaceCrossProcessTest.cpuRows(surface).0, surface)
        metrics.set("route_decode", failure?.description ?? "ok")
        metrics.set("route_taken", taken)
        metrics.set("route_wrong", wrong)
        checks.append(
            ("the decode goes through the copy engine into the blocks (\(taken))", taken == "copy_blocks:1")
        )
        checks.append(("the CPU reads the picture (\(wrong) bytes differ)", failure == nil && wrong == 0))
        withExtendedLifetime(sampler) {}

        for probe in Self.probes {
            let key = "m\(probe.mode)_g\(probe.gob)"
            let (target, held) = try await Self.blockLinear(gpu)
            if let failure = JPEGSurfaceDecode.decode(file, into: target, tile: probe) {
                metrics.set(key, "failed:" + String(failure.description.prefix(40)))
                continue
            }
            let got = SurfaceCrossProcessTest.cpuRows(target).0
            metrics.set(key, JPEGSurfaceDecode.explained(got, target))
            metrics.set(key + "_wrong", JPEGSurfaceDecode.wrong(got, target).0)
            withExtendedLifetime(held) {}
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// A BGRA surface the GPU samples first, so GM20B lays it out block-linear, and the texture that sampled it (held
    /// until the reads, as gpu.iosurface.cross_process holds its sampler)
    static func blockLinear(_ gpu: GPU) async throws -> (IOSurface, MTLTexture) {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: size, .height: size, .bytesPerElement: 4, .pixelFormat: UInt32(0x4247_5241),
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface \(size) not made") }
        return (surface, try await SurfaceCrossProcessTest.gpuRows(gpu, surface).1)
    }
}
