import HWTestCore
import HWTestGPU

public enum Registry {
    /// The GPU scenes come first: the first of them reproduces the process's first command buffer.
    static let gpuScenes: [any HWTest] =
        Scenes.all.map { GPUSceneTest(scene: $0) } + [CAStateSweepTest()]

    static let metal: [any HWTest] = [
        MetalDeviceTest(), MetalComputeTest(), MetalColorPassTest(), MetalDepthStencilTest(), MetalMSAATest(),
        MetalPixelFormatTest(), MetalHalfRoundingTest(), MetalCompressedTest(), MetalBlitTest(), MetalTextureViewTest(),
        MetalIOSurfaceTest(), SurfaceTiledDefaultTest(), MPSBlurTest(), MPSMatrixTest(), MetalRuntimeCompileTest(),
        CoreImageLibraryTest(),
        MetalBenchmarkTest(), FetchBlendCostTest(), CompressionChurnTest(),
    ]

    static let video: [any HWTest] =
        VideoDecodeTest.all + [NativeExportTest()] + PlaybackTest.all + VideoEncodeTest.all

    static let images: [any HWTest] =
        ImageDecodeTest.all + [HEICEncodeTest(), CoreImageTest(), PixelTransferTest(), PixelTransfer422Test()]

    static let audio: [any HWTest] = [
        AudioPlaybackTest(rate: 44_100), AudioPlaybackTest(rate: 48_000), AudioLatencyTest(), AudioRouteTest(),
        HeadsetTest(), MicrophoneTest(),
    ]

    static let display: [any HWTest] = [
        DisplayModeTest(), DisplayPacingTest(), BrightnessTest(), ColourPipelineTest(), PlanesTest(),
        PlainSurfacesTest(), DisplaySnapshotTest(), WakeTest(),
    ]

    static let sensorsAndInput: [any HWTest] = [
        AccelerometerTest(), GyroscopeTest(), DeviceMotionTest(), AmbientLightTest(), ControllersTest(), RailTest(),
        JoyConInputTest(), GameControllerTest(), KeyboardTest(), TouchTest(),
    ]

    static let connectivityAndStorage: [any HWTest] = [
        NetworkPathTest(), ReachabilityTest(), BluetoothTest(), USBStorageTest(), SequentialStorageTest(),
        RandomStorageTest(), WiFiLinkTest(),
    ]

    static let cpuAndSystem: [any HWTest] = [
        CPUSingleTest(), CPUMultiTest(), A64XSelfTest(), CryptoKitTest(), CommonCryptoTest(), KeychainTest(),
        LocalAuthenticationTest(), IdentityTest(), PowerStateTest(), ThermalTest(), MemoryTest(), UnionDirectoryTest(),
        OverlayAppsTest(), CPUClockTest(), CPUSustainedTest(), CPUSchedTest(), DispatchSyncTest(),
        DispatchSyncWorkloopTest(), WorkloopSyncTest(), WidgetsRenderTest(),
    ]

    static let compositor: [any HWTest] = UISceneTests.all + PacingTests.all

    public static let all: [any HWTest] =
        [CPUAsyncTimersTest(), RegDumpQuietTest()] + gpuScenes + metal + GPUPathTests.all + video + images + audio
        + display + compositor + sensorsAndInput + connectivityAndStorage
        + cpuAndSystem + [RawShareTest()]
        // Drives an NVDEC output mode nothing else uses: after every shared test.
        + [DisplayBufferTest()]
}
