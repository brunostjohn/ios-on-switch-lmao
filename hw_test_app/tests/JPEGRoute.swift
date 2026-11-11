import Darwin
import Foundation

/// How many JPEG decodes NVJPG finished since boot: AppleJPEGDriver's TegraNVJPGDecodes (debug kernels), which the
/// service makes when read. nil when the service or the property is missing (a release kernel, no NVJPG).
enum JPEGRoute {
    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> CFMutableDictionary?
    typealias Service = @convention(c) (mach_port_t, CFDictionary?) -> mach_port_t
    typealias Property = @convention(c) (mach_port_t, CFString, CFAllocator?, UInt32) -> Unmanaged<CFTypeRef>?
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t

    static func decodes() -> UInt64? {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let property = dlsym(iokit, "IORegistryEntryCreateCFProperty"), let release = dlsym(iokit, "IOObjectRelease")
        else { return nil }
        let jpeg = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("AppleJPEGDriver"))
        guard jpeg != 0 else { return nil }
        defer { _ = unsafeBitCast(release, to: Release.self)(jpeg) }
        let value = unsafeBitCast(property, to: Property.self)(jpeg, "TegraNVJPGDecodes" as CFString, nil, 0)?
            .takeRetainedValue()
        return (value as? NSNumber)?.uint64Value
    }
}
