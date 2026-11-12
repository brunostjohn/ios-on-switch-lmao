import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Compute stage-in: a kernel whose [[stage_in]] float2 comes from buffer 1 per thread position, over the dispatch's
/// whole grid as its stage-in region, writes x + y of each element. A static layout of stride 16 runs on both kinds; on
/// 24B119's A13 a layout of MTLBufferLayoutStrideDynamic runs at the bind's attributeStride, 16 and then 24 (21H461's
/// A10X answers supportsDynamicAttributeStride NO and refuses that state). Each result is compared with the CPU's;
/// every command buffer stays raw.
struct NativeStageInTest: NativeObjectTest {
    let id = "metal.native.stage_in"
    let name = "Compute stage-in from static and per-bind strides on the raw path"

    static let count = 64

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct In { float2 value [[attribute(0)]]; };
        kernel void stage_sum(In in [[stage_in]], device float *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
            out[i] = in.value.x + in.value.y;
        }
        """

    /// Element k's (k, 2k + 1) `stride` bytes apart, junk between
    static func elements(_ stride: Int) -> [Float] {
        let floats = stride / 4
        var out = [Float](repeating: -1e6, count: floats * (count + 1))
        for k in 0...count {
            out[k * floats] = Float(k)
            out[k * floats + 1] = Float(2 * k + 1)
        }
        return out
    }

    static func descriptor(_ function: MTLFunction, stride: Int) -> MTLComputePipelineDescriptor {
        let d = MTLComputePipelineDescriptor()
        d.computeFunction = function
        let input = MTLStageInputOutputDescriptor()
        input.attributes[0].format = .float2
        input.attributes[0].bufferIndex = 1
        input.layouts[1].stride = stride
        input.layouts[1].stepFunction = .threadPositionInGridX
        d.stageInputDescriptor = input
        return d
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        guard let function = library.makeFunction(name: "stage_sum") else { throw TestFailure("no stage_sum") }
        let make = { (d: MTLComputePipelineDescriptor) throws -> MTLComputePipelineState in
            try device.makeComputePipelineState(descriptor: d, options: []).0
        }
        let dynamicStride = Int(bitPattern: UInt.max)  // MTLBufferLayoutStrideDynamic
        let dynamic =
            (device as AnyObject as! NSObject).value(forKey: "supportsDynamicAttributeStride") as? Bool ?? false
        metrics.set("dynamic_stride", dynamic ? "yes" : "no")
        var cases = [("static16", try make(Self.descriptor(function, stride: 16)), 16, false)]
        if dynamic {
            let state = try make(Self.descriptor(function, stride: dynamicStride))
            cases += [("dynamic16", state, 16, true), ("dynamic24", state, 24, true)]
        } else if (try? make(Self.descriptor(function, stride: dynamicStride))) != nil {
            work.failures.append("a dynamic-stride stage-in state was made where the device answers it NO")
        }
        for (label, state, stride, perBind) in cases {
            work.objects.append(state)
            let input = try gpu.buffer(Self.elements(stride))
            let output = try gpu.buffer([Float](repeating: 0, count: Self.count + 1))
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(state)
                encoder.setBuffer(output, offset: 0, index: 0)
                if perBind {
                    encoder.setBuffer(input, offset: 0, attributeStride: stride, index: 1)
                } else {
                    encoder.setBuffer(input, offset: 0, index: 1)
                }
                encoder.setStageInRegion(MTLRegionMake1D(0, Self.count + 1))
                encoder.dispatchThreadgroups(
                    MTLSize(width: Self.count + 1, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
            let results = output.contents().bindMemory(to: Float.self, capacity: Self.count + 1)
            let wrong = (0...Self.count).filter { results[$0] != Float(3 * $0 + 1) }.count
            metrics.set("\(label)_wrong", wrong)
            if wrong != 0 { work.failures.append("\(label): \(wrong) of \(Self.count + 1) elements wrong") }
        }
        return work
    }
}
