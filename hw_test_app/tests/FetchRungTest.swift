import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Fetch rungs 2 and 3 (drivers/TegraGM20BMetal/raw/runtime/raw_fetch_free.h) on the chip: Core Animation states that
/// blend by fetching their colour, each drawn by its storage form (rung 4: the fetch and the blend inside the pixel
/// interlock) and by its fetch-free form (rung 2: the ROP's 8-bit blender with the form's factors; rung 3: the iterated
/// blend unit running the form's program, the colour targets narrowed to the blended one), both forced through the
/// facade's probe channel; the colour matrix (vcm) takes its program and constants from the GPU's macro each draw. Per
/// state, keyed by its fragment blend: worst_<blend> the largest difference in codes between the two routes' first
/// attachments (rule fetch.blend allows 2; rung 3's programs are exact in QuartzCore's arithmetic, so 2 too),
/// us_rung4_<blend> and us_rung<N>_<blend> one full-screen draw's GPU time each way. r<N>= the draws the facade's counter
/// says took the fetch-free form. verdict=PASS: every state within the bound and faster on its fetch-free form, with
/// every forced draw counted. passes_ceiling= the most passes of a state within the bound and faster with no state of
/// as many passes or fewer failing either: rung 3's sets raw_chip_facts.h kIteratedBlendPasses. A state this IPSW's
/// QuartzCore lacks is skipped. Rung 3 programs a unit no other driver here drives: a wrong setup kills the channel
/// (class errors 0x59, 0x5c, 0x5d in the kernel log), so it runs last.
struct FetchRungTest: HWTest {
    let id: String
    let rung: Int
    var name: String { "Fetch rung \(rung) against the storage form: codes and GPU time, Core Animation's states" }
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 300 }

    static let bound = 2
    static let draws = 10
    static let runs = 5

    /// QuartzCore 24B119's states with a fetch-free form of each rung (raw/pack/ca/fetchfree.py; corpus lines tagged
    /// os=24B119), each with the key its metrics take (its fragment blend) and its program's passes
    /// (raw/pack/iterated/table.py; vcm's macro at its longest, which CAStateSweep's matrix selects)
    static let states: [Int: [(label: String, key: String, passes: Int)]] = [
        2: [("PBGRAXm_TimgBclrA2Xhf_Isrc", "clr", 0), ("PBGRAXm_TimgBaddA2Xhf_Isrc", "add", 0)],
        3: [
            ("PBGRAXm_TimgBmpyA2Xhf_Isrc", "mpy", 2), ("PBGRAXm_TimgBpldA2Xhf_Isrc", "pld", 5),
            ("PBGRAXm_TimgBlltA2Xhf_Isrc", "llt", 9), ("PBGRAXm_TimgBdsoA2Xhf_Isrc", "dso", 13),
            ("PBGRAXm_TimgBlsoA2Xhf_Isrc", "lso", 14), ("PBGRAXm_TimgBdlcA2Xhf_Isrc", "dlc", 28),
            ("PBGRAXmw_TimgBvcmA3Xhf_Isrc", "vcm", 38), ("PBGRAXm_TimgBlumA2Xhf_Isrc", "lum", 42),
            ("PBGRAXm_TimgBcliA2Xhf_Isrc", "cli", 54), ("PBGRAXm_TimgBsatA2Xhf_Isrc", "sat", 64),
        ],
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else { return .skip("the default device is not the raw facade") }
        guard let statesURL = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
            let text = try? String(contentsOf: statesURL, encoding: .utf8)
        else { return .fail("ca_states.log is not in the app's bundle") }
        let keys = Dictionary(uniqueKeysWithValues: (Self.states[rung] ?? []).map { ($0.label, ($0.key, $0.passes)) })
        let lines = CAStateSweep.states(text, everyState: true).filter { keys[$0.shortLabel] != nil }
        if lines.isEmpty { return .skip("this QuartzCore makes none of the rung's states") }
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var metrics = Metrics()
        var beyond: [String] = []
        var slower: [String] = []
        var outcomes: [(passes: Int, ok: Bool)] = []
        let key = "fetch_rung\(rung)"
        let before = RawFacade.counters(of: gpu.device)[key] ?? 0
        var forcedDraws = 0
        for line in lines {
            let (label, passes) = keys[line.shortLabel]!
            guard
                let storage = try await FetchRungScene.forcing(
                    4, gpu.device,
                    {
                        try await FetchRungScene.bytes(line, library: library, gpu: gpu)
                    }),
                let free = try await FetchRungScene.forcing(
                    rung, gpu.device,
                    {
                        try await FetchRungScene.bytes(line, library: library, gpu: gpu)
                    })
            else { return .skip("the facade has no probe channel (a release build)") }
            forcedDraws += 2
            let (worst, differing) = FetchRungScene.difference(storage.1, free.1)
            metrics.set("worst_\(label)", worst)
            metrics.set("differing_\(label)", differing)
            metrics.set("path_\(label)", "\(storage.0)/\(free.0)")
            let within = worst <= Self.bound && storage.0 == "raw" && free.0 == "raw"
            if !within { beyond.append(line.shortLabel) }
            var us: [Double] = []
            for (forced, suffix) in [(4, "rung4"), (rung, "rung\(rung)")] {
                let timed = try await FetchRungScene.forcing(forced, gpu.device) {
                    try await FetchRungScene.microseconds(
                        line, library: library, gpu: gpu, draws: Self.draws, runs: Self.runs)
                }
                us.append(timed?.0 ?? 0)
                metrics.set("us_\(suffix)_\(label)", us.last!, digits: 1)
            }
            let faster = us[1] > 0 && us[1] < us[0]
            if !faster { slower.append(label) }
            outcomes.append((passes, within && faster))
            forcedDraws += Self.draws * (Self.runs + 1)
        }
        let taken = (RawFacade.counters(of: gpu.device)[key] ?? 0) - before
        metrics.set("r\(rung)", taken)
        metrics.set("forced_draws", forcedDraws)
        metrics.set("states", lines.count)
        let firstFailing = outcomes.filter { !$0.ok }.map(\.passes).min() ?? Int.max
        metrics.set("passes_ceiling", outcomes.filter { $0.ok && $0.passes < firstFailing }.map(\.passes).max() ?? 0)
        let counted = taken >= forcedDraws
        let verdict = beyond.isEmpty && slower.isEmpty && counted ? "PASS" : "FAIL"
        metrics.set("verdict", verdict)
        if !beyond.isEmpty { metrics.set("beyond", beyond.joined(separator: ",")) }
        if !slower.isEmpty { metrics.set("slower", slower.joined(separator: ",")) }
        if verdict == "PASS" { return .pass(metrics, proof: gpu.proof) }
        let why =
            !beyond.isEmpty
            ? "beyond the bound"
            : !counted ? "the facade drew \(taken) of the forced draws on rung \(rung)" : "slower than the storage form"
        return .fail(why, metrics)
    }
}
