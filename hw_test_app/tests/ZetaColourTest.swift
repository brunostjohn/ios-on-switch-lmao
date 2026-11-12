import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.zeta_colour decides drivers/TegraGM20BMetal/raw/runtime/raw_chip_facts.h kColourRopWritesZeta: whether the
/// colour ROP writes a zeta's memory (PTE kinds ZF32_X24S8 and S8) as a colour target of its texel, RG32Uint and R8Uint,
/// and the zeta's depth and stencil tests and its copies by aspect then read what it wrote (Gallium nvc0's route,
/// raw_zeta_colour.h). ZetaColourCells writes each zeta a pattern, every stencil value and a depth of its own a texel:
///  - resolve: a 4x zeta resolved by sample 0 into a one-sample one holding another pattern, its depth by the depth
///    resolve, its stencil by the route (Depth32Float_Stencil8: word G alone, the depth kept);
///  - turn: a zeta beside a drawable of rotation 1, moved into the pass's plane-shaped stand-in and back by the route's
///    turn copies (the whole texel), its stencil tested and inverted in between;
/// Depth32Float_Stencil8 and Stencil8 at 90x60, 96x64 and 1280x720, each zeta then read back by aspect and tested as a
/// zeta (EQUAL against each stencil value and against the pattern's depth). The arms: bits (the build's route) and
/// colour (switchRawProbeZetaColour: on); each also runs gpu.raw.stored_msaa (every stencil resolve pick) and
/// gpu.x1.rotated_target's keptStencil at rotations 1 to 4. <arm>_depth, _stencil and _tested count texels off the
/// pattern, _msaa the scene's failures, _kept the drawable's pixels off Metal's store, _bit_draws and _rop_draws the
/// stencil plane draws of each route. claim_verdict=PASS with colour_rop_draws above 0 and reopens=0, and no
/// "fault", "ROP", "LTC" or "TRAP" kernel line in the run, sets kColourRopWritesZeta; off the facade the arm is the
/// device's own (the test's references held to Metal).
struct ZetaColourTest: HWTest {
    let id = "gpu.raw.zeta_colour"
    let name = "Stencil planes by the colour ROP over the zeta's memory, read back and tested as a zeta"
    var subsystem: Subsystem { .metal }
    var timeout: TimeInterval { 300 }
    var runsWhenGPUWedged: Bool { true }

    static let sizes = [(90, 60), (96, 64), (1280, 720)]
    static let formats: [MTLPixelFormat] = [.depth32Float_stencil8, .stencil8]

    enum Arm: String {
        case bits, colour, device
    }

    static func probe(_ device: MTLDevice, _ on: Bool) -> Bool {
        let selector = NSSelectorFromString("switchRawProbeZetaColour:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return false }
        _ = object.perform(selector, with: NSNumber(value: on))
        return true
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        let raw = kind == "raw"
        if raw && !Self.probe(gpu.device, false) {
            return .skip("the facade has no zeta colour probe (a release build)")
        }
        defer { _ = Self.probe(gpu.device, false) }
        let cells = ZetaColourCells(gpu: gpu, rotated: try RotatedStates(gpu))
        let before = RawFacade.counters(of: gpu.device)
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var first: String?
        var colourDraws = 0
        for arm in raw ? [Arm.bits, .colour] : [.device] {
            _ = Self.probe(gpu.device, arm == .colour)
            let start = RawFacade.counters(of: gpu.device)
            let (count, msaa, kept, turned) = try await armRun(gpu, cells, seed: arm == .colour ? 40 : 10)
            let end = RawFacade.counters(of: gpu.device)
            let delta = { (key: String) in (end[key] ?? 0) - (start[key] ?? 0) }
            wrong["\(arm)_depth"] = count.depth
            wrong["\(arm)_stencil"] = count.stencil
            wrong["\(arm)_tested"] = count.tested
            wrong["\(arm)_msaa"] = msaa
            wrong["\(arm)_kept"] = kept
            metrics.set("\(arm)_bit_draws", delta("stencil_bit_draws"))
            metrics.set("\(arm)_rop_draws", delta("zeta_colour_draws"))
            metrics.set("\(arm)_turn_cells", turned)
            if arm == .colour { colourDraws = delta("zeta_colour_draws") }
            if first == nil, let f = count.first { first = "\(arm) \(f)" }
        }
        var finding = ProbeFinding.counted(wrong, paths: Array(gpu.paths.paths.keys), metrics: metrics)
        if raw && finding.verdict == .pass && colourDraws == 0 {
            finding.verdict = .inconclusive
            finding.note = "the colour arm drew no stencil plane by the colour ROP"
        }
        if let first { finding.note = finding.note.isEmpty ? first : finding.note + "; " + first }
        var base = Metrics()
        base.set("dev", kind)
        base.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        base.set("reopens", (RawFacade.counters(of: gpu.device)["reopens"] ?? 0) - (before["reopens"] ?? 0))
        base.set("gpu_after", await GPUProbe.answers())
        return finding.outcome(probe: "rop.zeta_colour", base: base, proof: gpu.proof)
    }

    /// One arm: every cell, gpu.raw.stored_msaa's failures, keptStencil's pixels off Metal's store at each rotation, and
    /// the turn cells that ran (none where the device has no rotation SPI)
    private func armRun(
        _ gpu: GPU, _ cells: ZetaColourCells, seed: UInt32
    ) async throws -> (ZetaColourCells.Count, Int, Int, Int) {
        var count = ZetaColourCells.Count()
        var turned = 0
        var n: UInt32 = 0
        for format in Self.formats {
            for (w, h) in Self.sizes {
                n += 1
                count.add(try await cells.resolve(format, w, h, seed: seed + n))
                if let t = try await cells.turn(format, w, h, seed: seed + n + 100) {
                    count.add(t)
                    turned += 1
                }
            }
        }
        var msaa = 0
        if let scene = Scenes.all.first(where: { $0.id == "gpu.raw.stored_msaa" }) {
            let result = try await scene.run(gpu)
            msaa = result.failed.count + result.problems.count
        }
        var kept = 0
        for turn in 1...4 {
            let rotated = RotatedCells(
                gpu: gpu, states: cells.rotated, turn: turn, width: 96, height: 64, aligned: true)
            if let plane = try rotated.makePlane() {
                kept += try await rotated.run(.keptStencil, into: plane).wrong
            }
        }
        return (count, msaa, kept, turned)
    }
}
