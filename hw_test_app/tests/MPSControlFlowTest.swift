import Foundation
import HWTestCore
import HWTestGPU
import Metal
import MetalPerformanceShaders
import MetalPerformanceShadersGraph

/// The callers of the predication SPI on the raw path (drivers/TegraGM20BMetal/raw/runtime/raw_predication.h), with
/// supportsCommandBufferJump YES: MPSGraph's while (a count past one laying of its copies, so the submission lays it
/// again: loop_continuations), for and if, encoded into one MPSCommandBuffer each, and MPSImageCanny's hysteresis on a
/// square. PASS: every command buffer completes on the raw path with MPSGraph's answers and Canny's edges; SKIP while the
/// device answers supportsCommandBufferJump NO.
struct MPSControlFlowTest: HWTest {
    let id = "gpu.raw.mps_control_flow"
    let name = "MPSGraph while/for/if and MPSImageCanny on the raw path's predicated loops"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let whileCount: Int32 = 3000, forCount: Int32 = 37, side = 64

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard MPSSupportsMTLDevice(gpu.device) else { return .fail("MPSSupportsMTLDevice is false") }
        let jump = (gpu.device as AnyObject).value(forKey: "supportsCommandBufferJump") as? Bool ?? false
        guard jump else {
            return .skip("supportsCommandBufferJump answers NO: MPSGraph and MPSImageCanny take their CPU paths")
        }
        var metrics = Metrics()
        var wrong: [String] = []
        let before = RawFacade.counters(of: gpu.device)

        let (counted, whilePath) = try runGraph(gpu, start: 0) { graph, start in
            let one = graph.constant(1, shape: [1], dataType: .int32)
            let limit = graph.constant(Double(Self.whileCount), shape: [1], dataType: .int32)
            return graph.while(
                initialInputs: [start],
                before: { inputs, results in
                    results.add(inputs[0])
                    return graph.lessThan(inputs[0], limit, name: nil)
                },
                after: { args in [graph.addition(args[0], one, name: nil)] }, name: nil)[0]
        }
        metrics.set("while_result", Int(counted))
        metrics.set("while_path", whilePath)
        if counted != Self.whileCount { wrong.append("while") }

        let (summed, forPath) = try runGraph(gpu, start: Self.forCount) { graph, start in
            graph.for(
                numberOfIterations: start,
                initialBodyArguments: [graph.constant(0, shape: [1], dataType: .int32)],
                body: { _, args in
                    [graph.addition(args[0], graph.constant(2, shape: [1], dataType: .int32), name: nil)]
                }, name: nil)[0]
        }
        metrics.set("for_result", Int(summed))
        metrics.set("for_path", forPath)
        if summed != 2 * Self.forCount { wrong.append("for") }

        let (chosen, ifPath) = try runGraph(gpu, start: 3) { graph, start in
            let pick = graph.lessThan(start, graph.constant(5, shape: [1], dataType: .int32), name: nil)
            return graph.if(
                pick, then: { [graph.constant(11, shape: [1], dataType: .int32)] },
                else: { [graph.constant(22, shape: [1], dataType: .int32)] }, name: nil)[0]
        }
        metrics.set("if_result", Int(chosen))
        metrics.set("if_path", ifPath)
        if chosen != 11 { wrong.append("if") }

        let canny = try await cannyEdges(gpu)
        metrics.set("canny_edge_pixels", canny.edges)
        metrics.set("canny_inside_pixels", canny.inside)
        metrics.set("canny_path", canny.path)
        if canny.edges == 0 || canny.inside != 0 { wrong.append("canny") }

        let after = RawFacade.counters(of: gpu.device)
        metrics.set("loop_continuations", (after["loop_continuations"] ?? 0) - (before["loop_continuations"] ?? 0))
        for path in [whilePath, forPath, ifPath, canny.path] where path != "raw" { wrong.append("path:\(path)") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// The graph's one int32 output, encoded into one MPSCommandBuffer, and that command buffer's path. `start` comes
    /// in as a fed placeholder so MPSGraph cannot fold the control flow into a constant on the CPU.
    func runGraph(_ gpu: GPU, start: Int32, _ build: (MPSGraph, MPSGraphTensor) -> MPSGraphTensor) throws -> (
        Int32, String
    ) {
        let graph = MPSGraph()
        let placeholder = graph.placeholder(shape: [1], dataType: .int32, name: nil)
        let out = build(graph, placeholder)
        let fed = MPSGraphTensorData(try gpu.buffer([start]), shape: [1], dataType: .int32)
        let commands = MPSCommandBuffer(from: gpu.queue)
        let results = graph.encode(
            to: commands, feeds: [placeholder: fed], targetTensors: [out], targetOperations: nil,
            executionDescriptor: nil)
        commands.commit()
        commands.waitUntilCompleted()
        let root = commands.rootCommandBuffer
        guard root.status == .completed, let data = results[out] else {
            return (-1, "error:" + (root.error.map { "\($0)" } ?? "no result"))
        }
        var value: Int32 = 0
        data.mpsndarray().readBytes(&value, strideBytes: nil)
        return (value, RawFacade.path(of: root))
    }

    /// A white square on black through MPSImageCanny: edge pixels on the square's border, none well inside it
    func cannyEdges(_ gpu: GPU) async throws -> (edges: Int, inside: Int, path: String) {
        let side = Self.side
        var bytes = [UInt8](repeating: 0, count: side * side * 4)
        for y in 16..<48 {
            for x in 16..<48 {
                let at = (y * side + x) * 4
                bytes.replaceSubrange(at..<at + 4, with: [255, 255, 255, 255])
            }
        }
        let source = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead])
        let destination = try gpu.texture(.r8Unorm, side, side, usage: [.shaderRead, .shaderWrite])
        try await gpu.upload(bytes, to: source, bytesPerRow: side * 4)
        let canny = MPSImageCanny(device: gpu.device)
        let timing = try await gpu.run(tolerateError: true) { commands in
            canny.encode(commandBuffer: commands, sourceTexture: source, destinationTexture: destination)
        }
        let out = try await gpu.read(destination, bytesPerPixel: 1)
        var edges = 0, inside = 0
        for y in 0..<side {
            for x in 0..<side where out[y * side + x] > 0 {
                if (20..<44).contains(x) && (20..<44).contains(y) { inside += 1 } else { edges += 1 }
            }
        }
        return (edges, inside, timing.path)
    }
}
