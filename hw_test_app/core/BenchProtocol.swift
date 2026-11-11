import CoreFoundation
import Foundation

/// tools/benchloop/protocol.py's batch as the bench loop decodes it: every limit and allowed method range is the
/// same (tools/benchloop/agreement_test.py compares them), so the host's and the app's checks agree.
public enum BenchProtocol {
    public static let version = "benchloop/1"
    public static let maxExperiments = 256
    public static let maxSide = 2048
    public static let maxDrawCounts = 16
    public static let maxDraws = 256
    public static let maxRepeats = 64
    public static let maxWarmup = 16
    public static let maxCommandBuffers = 1024
    public static let maxCode = 64 * 1024
    public static let maxData = 64 * 1024
    public static let maxMethodPairs = 1024
    public static let maxGroups = 65535
    public static let maxThreads = 1024
    public static let sphBytes = 80
    public static let maxCommandBufferMs = 50.0
    static let clockHz = 768e6
    static let lanesPerClock = 256.0

    /// MTLPixelFormat raw value and bytes per texel of each target the raw path renders.
    public static let formats: [String: (raw: UInt32, bytes: Int)] = [
        "r32float": (55, 4),
        "rgba8unorm": (70, 4),
        "bgra8unorm": (80, 4),
        "rgb10a2unorm": (90, 4),
        "rg32float": (105, 8),
        "rgba16float": (115, 8),
        "rgba32float": (125, 16),
        "r8unorm": (10, 1),
        "r16unorm": (20, 2),
        "r16float": (25, 2),
        "rg8unorm": (30, 2),
        "b5g6r5unorm": (40, 2),
        "bgr5a1unorm": (43, 2),
        "rg16unorm": (60, 4),
        "rg16float": (65, 4),
        "rgba8unorm_srgb": (71, 4),
        "bgra8unorm_srgb": (81, 4),
        "rg11b10float": (92, 4),
        "bgr10a2unorm": (94, 4),
        "rgba16unorm": (110, 8),
    ]

    public static let methodRanges: [(UInt32, UInt32)] = [
        (0x02E0, 0x02E4), (0x02F4, 0x02F8), (0x0754, 0x0758), (0x0D9C, 0x0DA0), (0x0DD0, 0x0DD8), (0x0DF0, 0x0DF8),
        (0x0F40, 0x0F54), (0x0F90, 0x0F94), (0x0FAC, 0x0FB0), (0x0FDC, 0x0FE0), (0x10CC, 0x10D0), (0x10E0, 0x10F4),
        (0x113C, 0x1140), (0x1140, 0x1144), (0x1220, 0x1224), (0x12E0, 0x12E8), (0x131C, 0x132C), (0x133C, 0x1360),
        (0x1360, 0x1380), (0x13A8, 0x13AC), (0x1480, 0x1500), (0x15B8, 0x15BC), (0x19C0, 0x19C4), (0x1A00, 0x1A20),
        (0x1E00, 0x1F00), (0x2600, 0x2604),
    ]

    public static func methodAllowed(_ method: UInt32) -> Bool {
        method % 4 == 0 && methodRanges.contains { $0.0 <= method && method < $0.1 }
    }

    public static func validName(_ name: String) -> Bool {
        guard let first = name.unicodeScalars.first, name.unicodeScalars.count <= 64 else { return false }
        let lower = { (c: Unicode.Scalar) in ("a"..."z").contains(c) || ("0"..."9").contains(c) }
        return lower(first) && name.unicodeScalars.allSatisfy { lower($0) || $0 == "_" || $0 == "." || $0 == "-" }
    }
}

public enum BenchKind: String, Sendable {
    case noop, known, fragment, compute, readback, twod
}

/// One 2D-engine operation (hw_test_app/bench/switch_bench.h switch_bench_twod)
public struct BenchTwod: Equatable, Sendable {
    public let scale: Bool
    public let fmt2d: UInt32
    public let colorFormat: UInt32
    public let color: [UInt32]
    /// 32.32: the source step per destination texel (du, dv) and the source origin (sx, sy)
    public let du: UInt64, dv: UInt64, sx: UInt64, sy: UInt64
    public let bilinear: Bool, corner: Bool
}

public enum BenchReadback: String, Sendable {
    case none, summary, full
}

public struct BenchInvalid: Error, Equatable {
    public let why: String

    public init(_ why: String) { self.why = why }
}

public struct BenchProgram: Equatable, Sendable {
    public let sph: Data
    public let code: Data
    public let data: Data
    public let gprs: UInt32

    public init(sph: Data, code: Data, data: Data, gprs: UInt32) {
        (self.sph, self.code, self.data, self.gprs) = (sph, code, data, gprs)
    }
}

public struct BenchExperiment: Equatable, Sendable {
    public let id: String
    public let kind: BenchKind
    public let format: String
    public let formatRaw: UInt32
    public let texelBytes: Int
    public let width: UInt32
    public let height: UInt32
    public let rect: [UInt32]
    public let clear: [Float]
    public let fill: Data?
    public let program: BenchProgram?
    public let threads: [UInt32]
    public let groups: [UInt32]
    public let draws: [UInt32]
    public let repeats: UInt32
    public let warmup: UInt32
    /// (method, value) pairs, flat
    public let methods: [UInt32]
    public let restore: [UInt32]
    public let readback: BenchReadback
    /// Colour 0 bound alone, without the template's other targets (the iterated blend unit takes one target)
    public let singleTarget: Bool
    /// The pack state to draw with, by key hash; 0: the bench's own choice
    public let templateKey: UInt64
    public let twod: BenchTwod?

    public var commandBuffers: Int { draws.count * Int(repeats + warmup) }
}

/// A batch's experiments in order; one that fails its checks is kept with why, so its result says so.
public struct BenchBatch: Sendable {
    public enum Entry: Sendable {
        case experiment(BenchExperiment)
        case invalid(id: String, kind: String, why: String)

        public var id: String {
            switch self {
            case .experiment(let e): return e.id
            case .invalid(let id, _, _): return id
            }
        }
    }

    public let name: String
    public let entries: [Entry]

    public static func decode(_ bytes: Data) throws -> BenchBatch {
        guard let top = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any] else {
            throw BenchInvalid("not a JSON object")
        }
        guard top["protocol"] as? String == BenchProtocol.version else {
            throw BenchInvalid("protocol is not benchloop/1")
        }
        guard let name = top["batch"] as? String, BenchProtocol.validName(name) else {
            throw BenchInvalid("batch name")
        }
        guard let list = top["experiments"] as? [Any], (1...BenchProtocol.maxExperiments).contains(list.count) else {
            throw BenchInvalid("1...\(BenchProtocol.maxExperiments) experiments a batch")
        }
        var seen = Set<String>()
        let entries = list.enumerated().map { index, item -> Entry in
            let fields = item as? [String: Any] ?? [:]
            var id = fields["id"] as? String ?? ""
            if !BenchProtocol.validName(id) || seen.contains(id) { id = "invalid-\(index)" }
            seen.insert(id)
            do {
                return .experiment(try BenchFields(fields).experiment(id: id))
            } catch let invalid as BenchInvalid {
                return .invalid(id: id, kind: fields["kind"] as? String ?? "-", why: invalid.why)
            } catch {
                return .invalid(id: id, kind: fields["kind"] as? String ?? "-", why: "\(error)")
            }
        }
        return BenchBatch(name: name, entries: entries)
    }
}
