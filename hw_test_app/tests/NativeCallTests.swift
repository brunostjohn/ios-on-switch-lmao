import HWTestCore

/// Metal calls the raw facade answers itself where they were answered zero before: each runs on the raw path, or its
/// feature answers NO wherever Metal lets a caller ask.
enum NativeCallTests {
    static let all: [any HWTest] = [
        NativeUnsupportedTest(), NativeVertexSamplerTest(), NativeParallelEncoderTest(), NativeIndirectCommandsTest(),
        NativeSharedTextureTest(),
        NativeSharedTexturePeerTest(),
        NativeComputeCommandsTest(),
        NativeCountersTest(),
        NativeCounterObjectsTest(),
        NativeTargetPartsTest(),
        NativeLayeredClearTest(),
        NativeLayeredMemorylessTest(),
        NativeLayeredVertexTest(),
        NativeViewportsTest(),
        NativeViewportIndexTest(),
        NativeBlockTexturesTest(),
    ]
}
