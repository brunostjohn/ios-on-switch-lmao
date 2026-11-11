import HWTestKit

/// What only iOS 27 has: QuartzCore 27's glass states on the raw path, Metal 4, Liquid Glass through UIKit and the
/// render server, True Tone's default, and VideoToolbox's frame processors. The frame processors run after the rest:
/// they may reach hardware J181 has and the Switch lacks. The X1 hardware probes come last, the risky ones at the end.
enum Tests27 {
    static let all: [any HWTest] = [
        GPUSceneTest(scene: CAGlassScene()), CAGlassSplitTest(), GPUSceneTest(scene: CAFetchRouteScene()),
        Metal4Test(), BinaryArchiveTest(), UIGlassTest(), UIGlassMotionTest(), UIGlassFirstFrameTest(),
        PocketStripTest(), PocketKeylineTest(),
        TrueToneDefaultTest(), GlassPacing.over,
        GlassPacing.under, NoCameraTest(), FrameProcessorTest(),
    ] + X1Probes.all
}
