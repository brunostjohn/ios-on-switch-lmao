import CoreGraphics
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The App Library's icon corners. `rb<side>_<storage>_*`: RenderBox in this process draws the icon mask at the small
/// pod icons' 54 pixels and the large ones' 136 (27 and 68 points at scale 2) on the default device, into a private and
/// a shared target, against its own CPU drawing (HWTestGPU RenderBoxCorner: `beyond`, `max_diff`, `partial` and
/// `ref_partial`), with the command buffers that went to m2v and the fetch routes of those draws.
/// `strip_<storage>_*` and `strip_restart_<storage>_*`: RenderBox's draw shape alone (HWTestGPU FetchStrip).
/// `icon<pt>_*`: iconservicesagent draws Settings' icon at 27 and 68 points, scale 2, uncached (-[ISImageDescriptor
/// setIgnoreCache:]); the small one's corners against the large one reduced to its size (`icon27_corner_beyond`), and
/// the cached 27-point image the App Library shows (`icon27_cached_corner_beyond`, reported). FAIL: a RenderBox or
/// strip arm off its reference or off the raw path, or the uncached small icon's corners beyond
/// RenderBoxCorner.tolerance of the reduced large one.
struct IconCornersTest: HWTest {
    let id = "gpu.raw.icon_corners"
    let name = "Icon corners: RenderBox's mask and IconServices' icons at the App Library's sizes against references"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let iconRadius = 0.2237

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()
        var wrong: [String] = []
        metrics.set("dev", RawFacade.kind(of: gpu.device))
        for cell in try await FetchStrip.cells(gpu) {
            metrics.set("\(cell.name)_path", cell.path)
            metrics.set("\(cell.name)_routes", cell.routes)
            metrics.set("\(cell.name)_wrong", cell.wrong)
            if cell.wrong > 0 || (cell.path != "raw" && cell.path != "device") { wrong.append(cell.name) }
        }
        if RenderBoxCorner.available {
            for side in [54, 136] {
                for storage in [MTLStorageMode.private, .shared] {
                    let before = RawFacade.counters(of: gpu.device)
                    let cell = try await RenderBoxCorner.cell(
                        gpu, side: side, radius: Double(side) * Self.iconRadius, storage: storage)
                    let after = RawFacade.counters(of: gpu.device)
                    let name = "rb\(side)_\(cell.storage)"
                    metrics.set("\(name)_beyond", cell.beyond)
                    metrics.set("\(name)_max_diff", cell.maxDiff)
                    metrics.set("\(name)_partial", cell.partial)
                    metrics.set("\(name)_ref_partial", cell.referencePartial)
                    let m2v = (after["m2v"] ?? 0) - (before["m2v"] ?? 0)
                    metrics.set("\(name)_m2v", m2v)
                    metrics.set("\(name)_routes", RawFacade.fetchRoutes(before: before, after: after))
                    if cell.beyond > 0 || m2v > 0 { wrong.append(name) }
                }
            }
        } else {
            metrics.set("rb", "absent")
        }
        icons(&metrics, &wrong)
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    typealias InitSizeScale = @convention(c) (AnyObject, Selector, CGSize, CGFloat) -> AnyObject
    typealias SetBool = @convention(c) (AnyObject, Selector, Bool) -> Void
    typealias ImageFor = @convention(c) (AnyObject, Selector, AnyObject) -> Unmanaged<CGImage>?

    /// Settings' icon as iconservicesagent draws it at `points` and scale 2: its alpha and width, nil where
    /// IconServices lacks what this asks
    static func icon(points: CGFloat, ignoreCache: Bool) -> (alpha: [Double], side: Int)? {
        guard let iconType = NSClassFromString("ISIcon") as? NSObject.Type,
            let descriptorType = NSClassFromString("ISImageDescriptor") as? NSObject.Type
        else { return nil }
        let initSize = NSSelectorFromString("initWithSize:scale:"), ignore = NSSelectorFromString("setIgnoreCache:")
        let prepare = NSSelectorFromString("prepareImageForDescriptor:")
        let cgImage = NSSelectorFromString("CGImageForDescriptor:")
        guard descriptorType.instancesRespond(to: initSize), descriptorType.instancesRespond(to: ignore),
            iconType.instancesRespond(to: prepare), iconType.instancesRespond(to: cgImage),
            let made = iconType.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() as? NSObject,
            let icon = made.perform(
                NSSelectorFromString("initWithBundleIdentifier:"), with: "com.apple.Preferences")?
                .takeUnretainedValue() as? NSObject,
            let alloc = descriptorType.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue()
        else { return nil }
        let descriptor = unsafeBitCast(descriptorType.instanceMethod(for: initSize), to: InitSizeScale.self)(
            alloc, initSize, CGSize(width: points, height: points), 2)
        unsafeBitCast(descriptorType.instanceMethod(for: ignore), to: SetBool.self)(descriptor, ignore, ignoreCache)
        _ = icon.perform(prepare, with: descriptor)
        guard
            let image = unsafeBitCast(iconType.instanceMethod(for: cgImage), to: ImageFor.self)(
                icon, cgImage, descriptor)?
                .takeUnretainedValue(), image.width == image.height
        else { return nil }
        return (RenderBoxCorner.alpha(of: image), image.width)
    }

    func icons(_ metrics: inout Metrics, _ wrong: inout [String]) {
        guard let large = Self.icon(points: 68, ignoreCache: true) else {
            metrics.set("icon_api", "absent")
            return
        }
        metrics.set("icon68_side", large.side)
        for (name, ignoreCache) in [("icon27", true), ("icon27_cached", false)] {
            guard let small = Self.icon(points: 27, ignoreCache: ignoreCache) else {
                metrics.set("\(name)_side", 0)
                continue
            }
            metrics.set("\(name)_side", small.side)
            let reference = RenderBoxCorner.reduce(large.alpha, from: large.side, to: small.side)
            let corner = Int((Double(small.side) * Self.iconRadius * 1.5).rounded(.up)) + 1
            var beyond = 0, maxDiff = 0, partial = 0, referencePartial = 0
            for (y, x) in RenderBoxCorner.cornerPixels(side: small.side, corner: corner) {
                let got = Int((small.alpha[y * small.side + x] * 255).rounded())
                let want = Int((reference[y * small.side + x] * 255).rounded())
                maxDiff = max(maxDiff, abs(got - want))
                if abs(got - want) > RenderBoxCorner.tolerance { beyond += 1 }
                if (8...247).contains(got) { partial += 1 }
                if (8...247).contains(want) { referencePartial += 1 }
            }
            metrics.set("\(name)_corner_beyond", beyond)
            metrics.set("\(name)_corner_max_diff", maxDiff)
            metrics.set("\(name)_partial", partial)
            metrics.set("\(name)_ref_partial", referencePartial)
            if name == "icon27", beyond > 0 { wrong.append(name) }
        }
    }
}
