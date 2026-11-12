import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The states this device's QuartzCore asks for where Apple's offline compilation manifest names another colour
/// precision (ca_asked.log: raw/pack/ca/deepcolour.py), each made on the raw facade from the pack: none of them may be a
/// state the pack lacks (the facade's pack_misses), which would cost Core Animation a generic draw and a compile at an
/// app's launch. QuartzCore names them by CADeviceUseCIF10, which this process reads as QuartzCore does
/// (MobileGestalt's HasExtendedColorDisplay, the CAEnableDeepFramebuffer preference over it): it has to be the answer
/// the pack was made for.
struct CAAskedFormsTest: HWTest {
    let id = "gpu.ca.asked_forms"
    let name = "Core Animation's states in this device's colour precision come from the pack"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    typealias BoolAnswer = @convention(c) (CFString) -> Bool

    /// CADeviceUseCIF10 as QuartzCore's block computes it (24B119 0x184409e94, 21H461 0x18957be58), nil where
    /// MobileGestalt cannot be asked
    static func cif10() -> Bool? {
        guard let gestalt = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW),
            let symbol = dlsym(gestalt, "MGGetBoolAnswer")
        else { return nil }
        guard unsafeBitCast(symbol, to: BoolAnswer.self)("HasExtendedColorDisplay" as CFString) else { return false }
        var exists: DarwinBoolean = false
        let preference = CFPreferencesGetAppBooleanValue(
            "CAEnableDeepFramebuffer" as CFString, kCFPreferencesCurrentApplication, &exists)
        return exists.boolValue ? preference : true
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let url = Bundle.main.url(forResource: "ca_asked", withExtension: "log"),
            let text = try? String(contentsOf: url, encoding: .utf8)
        else { return .fail("ca_asked.log is not in the app's bundle") }
        let header = text.prefix { $0 != "\n" }
        let lines = text.split(separator: "\n").compactMap { PipelineLine(String($0)) }
        var metrics = Metrics()
        metrics.set("states", lines.count)
        let device = Self.cif10().map { String($0) } ?? "unknown"
        metrics.set("cif10", device)
        metrics.set("pack_made_for", String(header.dropFirst("# cif10=".count)))
        guard header == "# cif10=\(device)" else {
            return .fail("the device's CADeviceUseCIF10 is \(device), the pack was made for \(header)", metrics)
        }
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var missed: [String] = []
        var failed: [String] = []
        var slowest = 0.0
        let started = DispatchTime.now().uptimeNanoseconds
        for line in lines {
            let before = RawFacade.counters(of: gpu.device)["pack_misses"] ?? 0
            let made = DispatchTime.now().uptimeNanoseconds
            do {
                _ = try CAState.pipeline(line, library: library, device: gpu.device)
            } catch {
                failed.append(line.shortLabel)
                continue
            }
            slowest = max(slowest, Double(DispatchTime.now().uptimeNanoseconds - made) / 1e6)
            if (RawFacade.counters(of: gpu.device)["pack_misses"] ?? 0) != before { missed.append(line.shortLabel) }
        }
        metrics.set("ms", Double(DispatchTime.now().uptimeNanoseconds - started) / 1e6)
        metrics.set("slowest_ms", slowest)
        metrics.set("pack_misses", missed.count)
        metrics.set("failed", failed.count)
        if !missed.isEmpty { metrics.set("missed_states", missed.prefix(12).joined(separator: ",")) }
        if !failed.isEmpty { metrics.set("failed_states", failed.prefix(12).joined(separator: ",")) }
        guard RawFacade.counters(of: gpu.device)["pack_misses"] != nil else {
            return .fail("the facade counts no pack misses (switchRawCounters: a debug build's)", metrics)
        }
        if !missed.isEmpty || !failed.isEmpty {
            return .fail("\(missed.count) states the pack lacks, \(failed.count) not made", metrics)
        }
        return .pass(metrics)
    }
}
