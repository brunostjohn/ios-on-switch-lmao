import AVFoundation
import HWTestCore
import HWTestVideo
import UIKit

/// A decoder's picture on window B with its destination clipped by the screen's edges
/// (drivers/NativeMobileDisplay/SwitchNativePlane.h): scanned in place through ADDR_H_OFFSET and ADDR_V_OFFSET, the
/// route V5a and V5c proved, instead of its pitch representation (a VIC copy per frame). Only an unturned video layer
/// reaches window B as the decoder's own picture, and on the Switch's portrait panel that takes a portrait UI, so the
/// scene is asked for portrait first and turned back afterwards; a scene that stays landscape is a skip. Plays the H.264
/// 1080p clip aspect-fitted into a view twice the stage's width (both side edges off the screen) and reads
/// kern.switch_planes_* around it (debug kernels): window B has to have taken the decoder's own picture
/// (native_share: placements of it per set written) with no detached layer unshown.
struct PlanesCroppedTest: HWTest {
    let id = "display.planes_cropped"
    let name = "A clipped video scanned on window B from the decoder's own picture"
    let subsystem = Subsystem.compositor

    static let counters = ["flips", "latched", "maps", "native", "dropped"]

    func run(_ context: TestContext) async throws -> Outcome {
        let (before, unread) = PlanesTest.totals(Self.counters)
        guard before.count == Self.counters.count else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_planes_*", unread), metrics)
        }
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 1080 }) else {
            return .skip("no H.264 1080p clip")
        }
        let asset = AVURLAsset(url: context.media.appendingPathComponent(clip.file))
        let duration = try await asset.load(.duration)
        nonisolated(unsafe) let source = asset
        let (item, player) = await MainActor.run { () -> (AVPlayerItem, AVPlayer) in
            let item = AVPlayerItem(asset: source)
            return (item, AVPlayer(playerItem: item))
        }
        let turnedFrom = await Self.request(.portrait)
        try? await Task.sleep(nanoseconds: 1_000_000_000)
        let portrait = await context.onStage(fullScreen: true) { stage -> Bool in
            let bounds = stage.bounds
            let view = PlayerView(
                frame: CGRect(x: -bounds.width / 2, y: 0, width: bounds.width * 2, height: bounds.height))
            view.playerLayer.player = player
            view.playerLayer.videoGravity = .resizeAspect
            stage.addSubview(view)
            return bounds.height > bounds.width
        }
        var metrics = Metrics()
        metrics.set("portrait", portrait)
        guard portrait else {
            await context.clearStage()
            _ = await Self.request(turnedFrom)
            return .skip("the scene stayed landscape: no unturned video layer reaches window B", metrics)
        }
        let ended = Waiter<Bool>()
        let observer = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main
        ) { _ in ended.resume(true) }
        await MainActor.run { player.play() }
        let finished = await ended.wait(timeout: duration.seconds + 15)
        await MainActor.run { player.pause() }
        NotificationCenter.default.removeObserver(observer)
        let after = PlanesTest.totals(Self.counters).values
        await context.clearStage()
        _ = await Self.request(turnedFrom)
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
        for name in Self.counters { metrics.set("d_" + name, delta(name)) }
        let share = delta("flips") > 0 ? Double(delta("native")) / Double(delta("flips")) : 0
        metrics.set("native_share", share, digits: 3)
        return .judged(
            [
                ("played to the end", finished == true),
                ("window B flipped (d_flips > 0)", delta("flips") > 0),
                ("the decoder's own picture, clipped, on window B (native_share >= 0.9)", share >= 0.9),
                ("no detached layer went unshown (d_dropped 0)", delta("dropped") == 0),
            ], metrics)
    }

    /// Asks the app's scene for `mask`; the orientations it had.
    @MainActor static func request(_ mask: UIInterfaceOrientationMask) -> UIInterfaceOrientationMask {
        guard let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first else {
            return mask
        }
        let had: UIInterfaceOrientationMask
        switch scene.interfaceOrientation {
        case .portrait: had = .portrait
        case .portraitUpsideDown: had = .portraitUpsideDown
        case .landscapeLeft: had = .landscapeLeft
        default: had = .landscapeRight
        }
        scene.windows.first?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { _ in }
        return had
    }
}
