import CoreFoundation
import Foundation

/// One experiment's JSON fields, read and checked as tools/benchloop/protocol.py validate_experiment checks them.
struct BenchFields {
    let fields: [String: Any]

    init(_ fields: [String: Any]) { self.fields = fields }

    func experiment(id: String) throws -> BenchExperiment {
        guard let kind = BenchKind(rawValue: fields["kind"] as? String ?? "") else { throw BenchInvalid("kind") }
        try Self.keys(fields, BenchProtocol.experimentKeys, "experiment")
        let target = fields["target"] as? [String: Any] ?? [:]
        try Self.keys(target, BenchProtocol.targetKeys, "target")
        guard let format = target["format"] as? String, let info = BenchProtocol.formats[format] else {
            throw BenchInvalid("target format is not one the raw path renders")
        }
        let width = try count(target["width"], 1...BenchProtocol.maxSide, "width")
        let height = try count(target["height"], 1...BenchProtocol.maxSide, "height")
        let rect = try counts(fields["rect"] ?? ([0, 0, width, height] as [Any]), 4, 0...BenchProtocol.maxSide, "rect")
        guard rect[0] < rect[2], rect[2] <= width, rect[1] < rect[3], rect[3] <= height else {
            throw BenchInvalid("rect outside the target")
        }
        let draws = try counts(
            fields["draws"] ?? ([1] as [Any]), nil, (kind == .noop ? 0 : 1)...BenchProtocol.maxDraws, "draws")
        guard (1...BenchProtocol.maxDrawCounts).contains(draws.count) else { throw BenchInvalid("draws: 1..16 counts") }
        let repeats = try count(fields["repeats"] ?? (1 as Any), 1...BenchProtocol.maxRepeats, "repeats")
        let warmup = try count(fields["warmup"] ?? (0 as Any), 0...BenchProtocol.maxWarmup, "warmup")
        guard draws.count * (repeats + warmup) <= BenchProtocol.maxCommandBuffers else {
            throw BenchInvalid("too many command buffers")
        }
        guard let readback = BenchReadback(rawValue: fields["readback"] as? String ?? "summary") else {
            throw BenchInvalid("readback is none, summary or full")
        }
        let clear = (fields["clear"] as? [Any] ?? [0, 0, 0, 0]).compactMap { ($0 as? NSNumber)?.floatValue }
        guard clear.count == 4 else { throw BenchInvalid("clear is four floats") }
        var fill: Data?
        if let text = fields["fill"] {
            fill = try bytes(text, "fill")
            guard fill?.count == width * height * info.bytes else {
                throw BenchInvalid("fill is the whole target, packed")
            }
        }
        var program: BenchProgram?
        var threads = [1, 1, 1], groups = [1, 1, 1]
        var outputBytes = 0
        switch kind {
        case .fragment, .readback:
            program = try self.program(fields["program"], "program", sph: true)
        case .compute:
            let compute = fields["compute"] as? [String: Any] ?? [:]
            program = try self.program(compute, "compute", sph: false)
            threads = try counts(
                compute["threads"] ?? ([] as [Any]), 3, 1...BenchProtocol.maxThreads, "compute.threads")
            groups = try counts(compute["groups"] ?? ([] as [Any]), 3, 1...BenchProtocol.maxGroups, "compute.groups")
            guard threads.reduce(1, *) <= BenchProtocol.maxThreads else { throw BenchInvalid("compute.threads > 1024") }
            outputBytes = try output(compute, program!)
        case .noop, .known, .twod:
            break
        }
        let forms = try self.forms(kind, format: format, size: (width, height), rect: rect)
        if forms.compare == "program" { program = try self.program(fields["program"], "program", sph: true) }
        let methods = kind == .readback ? try pairs(fields["methods"] ?? ([] as [Any]), "methods") : []
        let restore = kind == .readback ? try pairs(fields["restore"] ?? ([] as [Any]), "restore") : []
        if kind != .readback, fields["methods"] != nil || fields["restore"] != nil {
            throw BenchInvalid("only readback experiments set methods")
        }
        let targets = try count(fields["targets"] ?? (0 as Any), 0...1, "targets")
        if kind != .readback, fields["targets"] != nil {
            throw BenchInvalid("only readback experiments choose their targets")
        }
        var templateKey: UInt64 = 0
        if let text = fields["template"] {
            guard let hex = text as? String, hex.count == 16, let key = UInt64(hex, radix: 16), key != 0 else {
                throw BenchInvalid("template is a pack state's key hash, 16 hex digits")
            }
            templateKey = key
        }
        let experiment = BenchExperiment(
            id: id, kind: kind, format: format, formatRaw: info.raw, texelBytes: info.bytes, width: UInt32(width),
            height: UInt32(height), rect: rect.map(UInt32.init), clear: clear, fill: fill, program: program,
            threads: threads.map(UInt32.init), groups: groups.map(UInt32.init), draws: draws.map(UInt32.init),
            repeats: UInt32(repeats), warmup: UInt32(warmup), methods: methods, restore: restore, readback: readback,
            singleTarget: targets == 1, templateKey: templateKey, twod: forms.twod, source: forms.source,
            compare: forms.compare, compressed: forms.compressed, outputBytes: outputBytes)
        guard Self.estimatedMs(experiment) <= BenchProtocol.maxCommandBufferMs else {
            throw BenchInvalid("a command buffer would run past \(BenchProtocol.maxCommandBufferMs) ms")
        }
        return experiment
    }

    /// protocol.py estimated_ms: every lane runs every instruction at the SMs' full rate, a warp at the least.
    static func estimatedMs(_ e: BenchExperiment) -> Double {
        guard let program = e.program else { return 0 }
        let instructions = Double(program.code.count / 32 * 3)
        let lanes =
            e.kind == .compute
            ? Double(e.threads.reduce(1, *) * e.groups.reduce(1, *))
            : max(32, Double((e.rect[2] - e.rect[0]) * (e.rect[3] - e.rect[1])))
        let work = lanes * instructions * Double(e.draws.max() ?? 1)
        return work / BenchProtocol.lanesPerClock / BenchProtocol.clockHz * 1e3
    }

    func program(_ value: Any?, _ field: String, sph: Bool) throws -> BenchProgram {
        guard let p = value as? [String: Any] else { throw BenchInvalid("\(field) is an object") }
        try Self.keys(p, sph ? BenchProtocol.programKeys : BenchProtocol.computeKeys, field)
        let code = try bytes(p["code"] ?? ("" as Any), "\(field).code")
        guard !code.isEmpty, code.count <= BenchProtocol.maxCode, code.count % 32 == 0 else {
            throw BenchInvalid("\(field).code: whole 32-byte bundles, at most 64 KiB")
        }
        let data = try bytes(p["data"] ?? ("" as Any), "\(field).data")
        guard data.count <= BenchProtocol.maxData, data.count % 16 == 0 else {
            throw BenchInvalid("\(field).data: 16-byte multiples, at most 64 KiB")
        }
        let header = sph ? try bytes(p["sph"] ?? ("" as Any), "\(field).sph") : Data()
        guard !sph || header.count == BenchProtocol.sphBytes else { throw BenchInvalid("program.sph is 80 bytes") }
        let gprs = try count(p["gprs"], 1...255, "\(field).gprs")
        return BenchProgram(sph: header, code: code, data: data, gprs: UInt32(gprs))
    }

    private func pairs(_ value: Any, _ field: String) throws -> [UInt32] {
        guard let list = value as? [Any], list.count <= BenchProtocol.maxMethodPairs else {
            throw BenchInvalid("\(field): at most \(BenchProtocol.maxMethodPairs) pairs")
        }
        var flat: [UInt32] = []
        for pair in list {
            let words = try counts(pair, 2, 0...Int(UInt32.max), field)
            guard BenchProtocol.methodAllowed(UInt32(words[0])) else {
                throw BenchInvalid("\(field): method \(String(words[0], radix: 16)) is not allowed")
            }
            flat += words.map(UInt32.init)
        }
        return flat
    }

    private func output(_ compute: [String: Any], _ program: BenchProgram) throws -> Int {
        guard let value = compute["output"] else { return 0 }
        let bytes = try count(value, 16...BenchProtocol.maxOutput, "compute.output")
        guard bytes % 16 == 0 else { throw BenchInvalid("compute.output: 16-byte multiples") }
        guard program.data.count >= 16 else {
            throw BenchInvalid("compute.output: its address goes in the shader data's first 8 bytes")
        }
        return bytes
    }

    static func keys(_ object: [String: Any], _ allowed: Set<String>, _ field: String) throws {
        let unknown = Set(object.keys).subtracting(allowed).sorted()
        guard unknown.isEmpty else { throw BenchInvalid("\(field): unknown field \(unknown.joined(separator: ", "))") }
    }

    func bytes(_ value: Any, _ field: String) throws -> Data {
        guard let text = value as? String, let data = Data(base64Encoded: text) else {
            throw BenchInvalid("\(field) is not base64")
        }
        return data
    }

    func count(_ value: Any?, _ range: ClosedRange<Int>, _ field: String) throws -> Int {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
            let n = Int(exactly: number.doubleValue), range.contains(n)
        else { throw BenchInvalid("\(field) not in \(range.lowerBound)..\(range.upperBound)") }
        return n
    }

    func counts(_ value: Any, _ length: Int?, _ range: ClosedRange<Int>, _ field: String) throws -> [Int] {
        guard let list = value as? [Any], length == nil || list.count == length else {
            throw BenchInvalid("\(field) is a list of \(length.map(String.init) ?? "some") numbers")
        }
        return try list.map { try count($0, range, field) }
    }
}
