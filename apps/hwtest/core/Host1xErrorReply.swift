import Foundation

/// The illegal-opcode probe's reply (drivers/TegraHost1x/SwitchHost1xErrorProbe.h, little-endian, 40 bytes): spare
/// channel 13 fed opcode 15, host1x's general interrupt to deliver it, then the channel restarted and fed a stream that
/// only increments a syncpoint.
public struct Host1xErrorReply: Sendable, Equatable {
    public static let magic: UInt32 = 0x4849_4F50 // 'HIOP'
    public static let size = 40

    public let status: Int32
    public let attached: Bool
    public let channel: UInt32
    public let opcode: UInt32
    public let taken: Int
    public let statBefore: UInt32
    public let statStopped: UInt32
    public let statRestarted: UInt32
    public let recovered: Bool

    /// The request: magic, 0.
    public static let request: [UInt8] = [magic, 0].flatMap { word in
        (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
    }

    public init?(_ bytes: [UInt8]) {
        guard bytes.count == Self.size, Self.word(bytes, 0) == Self.magic else { return nil }
        status = Int32(bitPattern: Self.word(bytes, 4))
        attached = Self.word(bytes, 8) != 0
        channel = Self.word(bytes, 12)
        opcode = Self.word(bytes, 16)
        taken = Int(Self.word(bytes, 20))
        statBefore = Self.word(bytes, 24)
        statStopped = Self.word(bytes, 28)
        statRestarted = Self.word(bytes, 32)
        recovered = Self.word(bytes, 36) != 0
    }

    /// Why the interrupt is not proven to deliver and the channel to come back, or nil.
    public var failure: String? {
        if !attached { return "host1x's general interrupt is not attached" }
        if status != 0 { return "the illegal opcode could not be submitted (\(status))" }
        if taken == 0 { return "nothing latched: opcode \(opcode) is not illegal on this host1x" }
        if !recovered { return "channel \(channel) ran no stream after its restart" }
        return nil
    }

    static func word(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        (0 ..< 4).reduce(0) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
    }
}
