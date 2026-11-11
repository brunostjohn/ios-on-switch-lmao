import Darwin
import Foundation

/// A TegraGM20B connection of the test's own (drivers/TegraGM20B/TegraGM20BABI.h's wire form): IOKit through dlsym,
/// which the iOS SDK has no headers for.
final class GPUConnection {
    struct Mapping {
        let token: UInt32
        let address: UInt64
        var base: UnsafeRawPointer { UnsafeRawPointer(bitPattern: UInt(address))! }
    }

    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> UnsafeMutableRawPointer?
    typealias Service = @convention(c) (mach_port_t, UnsafeMutableRawPointer?) -> mach_port_t
    typealias Open =
        @convention(c) (mach_port_t, mach_port_t, UInt32, UnsafeMutablePointer<mach_port_t>) ->
        kern_return_t
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t
    typealias Call =
        @convention(c) (
            mach_port_t, UInt32, UnsafeRawPointer?, Int, UnsafeMutableRawPointer?, UnsafeMutablePointer<Int>?
        ) -> kern_return_t

    static let openType: UInt32 = 0x4e56
    static let tunnelMagic: UInt32 = 0x4e56_544e
    static let tunnelProbe: UInt32 = 0xffff_ffff
    static let mapMethod: UInt32 = 12
    static let unmapMethod: UInt32 = 13
    static let mapReadOnly: UInt32 = 1

    let connection: mach_port_t
    let callStruct: Call
    let close: Release
    /// The first selector the sandbox admits, which carries every method with the tunnel's prefix; nil: none answered
    /// the probe, and each method goes by its own selector.
    var tunnel: UInt32?

    init?() {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let open = dlsym(iokit, "IOServiceOpen"), let release = dlsym(iokit, "IOObjectRelease"),
            let close = dlsym(iokit, "IOServiceClose"), let call = dlsym(iokit, "IOConnectCallStructMethod")
        else { return nil }
        let accelerator = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("IOAcceleratorES"))
        guard accelerator != 0 else { return nil }
        var connection: mach_port_t = 0
        let kr = unsafeBitCast(open, to: Open.self)(accelerator, mach_task_self_, Self.openType, &connection)
        _ = unsafeBitCast(release, to: Release.self)(accelerator)
        guard kr == KERN_SUCCESS else { return nil }
        self.connection = connection
        callStruct = unsafeBitCast(call, to: Call.self)
        self.close = unsafeBitCast(close, to: Release.self)
        let probe = words([Self.tunnelMagic, Self.tunnelProbe])
        tunnel = (0..<256).map(UInt32.init).first { send($0, probe, 0).0 == KERN_SUCCESS }
    }

    deinit { _ = close(connection) }

    func map(_ token: UInt32, bytes: Int) -> Mapping? {
        let (kr, reply) = call(Self.mapMethod, words([token, Self.mapReadOnly, 0, 0]), outputSize: 16)
        guard kr == KERN_SUCCESS else { return nil }
        let mapping = Mapping(token: token, address: ScanoutReadback.uint64(reply, 0))
        guard Int(ScanoutReadback.uint64(reply, 8)) >= bytes else {
            unmap(mapping)
            return nil
        }
        return mapping
    }

    func unmap(_ mapping: Mapping) {
        let address = mapping.address
        _ = call(
            Self.unmapMethod, words([mapping.token, 0, UInt32(address & 0xffff_ffff), UInt32(address >> 32)]),
            outputSize: 0)
    }

    func call(_ method: UInt32, _ input: [UInt8], outputSize: Int) -> (kern_return_t, [UInt8]) {
        guard let tunnel else { return send(method, input, outputSize) }
        return send(tunnel, words([Self.tunnelMagic, method]) + input, outputSize)
    }

    func send(_ selector: UInt32, _ input: [UInt8], _ outputSize: Int) -> (kern_return_t, [UInt8]) {
        var output = [UInt8](repeating: 0, count: outputSize)
        var size = outputSize
        let kr = input.withUnsafeBytes { inBytes in
            output.withUnsafeMutableBytes { outBytes in
                callStruct(connection, selector, inBytes.baseAddress, inBytes.count, outBytes.baseAddress, &size)
            }
        }
        return (kr, output)
    }

    func words(_ values: [UInt32]) -> [UInt8] { values.flatMap { withUnsafeBytes(of: $0.littleEndian, Array.init) } }
}
