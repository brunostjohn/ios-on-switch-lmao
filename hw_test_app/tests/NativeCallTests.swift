import HWTestCore

/// Metal calls the raw facade answers itself where they were answered zero before: each runs on the raw path, or its
/// feature answers NO wherever Metal lets a caller ask.
enum NativeCallTests {
    static let all: [any HWTest] = [
        NativeUnsupportedTest(), NativeVertexSamplerTest(), NativeParallelEncoderTest(), NativeIndirectCommandsTest(),
        NativeComputeCommandsTest(), NativeSharedTextureTest(), NativeSharedTexturePeerTest(), NativeCountersTest(),
        NativeCounterObjectsTest(), NativeTessellationTest(), NativeAmplificationTest(), NativeAmplificationCountTest(),
        NativeDynamicStrideTest(), NativeStageInTest(), NativeVertexStreamsTest(), NativeM2vMissingTest(),
        NativeTargetPartsTest(), NativeLayeredClearTest(), NativeLayeredMemorylessTest(), NativeLayeredVertexTest(),
        NativeViewportsTest(), NativeViewportIndexTest(), NativeBlockTexturesTest(), NativeDepthSlicesTest(),
    ]
}
