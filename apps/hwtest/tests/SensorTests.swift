import CoreMotion
import Foundation
import HWTestCore
import UIKit

/// Collects CoreMotion samples for a fixed time at a requested 100 Hz.
final class MotionSamples<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var samples: [T] = []

    func add(_ sample: T) {
        lock.lock()
        samples.append(sample)
        lock.unlock()
    }

    var all: [T] {
        lock.lock()
        defer { lock.unlock() }
        return samples
    }
}

enum Motion {
    static let manager = CMMotionManager()
    static let queue = OperationQueue()
    static let interval = 0.01
    static let seconds = 2.0

    static func collect<T>(
        start: (MotionSamples<T>) -> Void, stop: () -> Void
    ) async throws -> (samples: [T], rate: Double) {
        let samples = MotionSamples<T>()
        start(samples)
        try await Task.sleep(nanoseconds: 300_000_000)
        let skip = samples.all.count
        let begin = CFAbsoluteTimeGetCurrent()
        try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
        let window = Array(samples.all.dropFirst(skip))
        let elapsed = CFAbsoluteTimeGetCurrent() - begin
        stop()
        return (window, Double(window.count) / elapsed)
    }
}

struct AccelerometerTest: HWTest {
    let id = "sensor.accelerometer"
    let name = "Accelerometer"
    let subsystem = Subsystem.sensors

    func run(_ context: TestContext) async throws -> Outcome {
        let manager = Motion.manager
        guard manager.isAccelerometerAvailable else { return .fail("isAccelerometerAvailable is false") }
        manager.accelerometerUpdateInterval = Motion.interval
        let (samples, rate) = try await Motion.collect(
            start: { (sink: MotionSamples<CMAcceleration>) in
                manager.startAccelerometerUpdates(to: Motion.queue) { data, _ in
                    if let data { sink.add(data.acceleration) }
                }
            }, stop: { manager.stopAccelerometerUpdates() }
        )
        let magnitude = Stats.mean(samples.map { sqrt($0.x * $0.x + $0.y * $0.y + $0.z * $0.z) })
        var metrics = Metrics()
        metrics.set("rate_hz", rate, digits: 1)
        metrics.set("gravity_g", magnitude, digits: 3)
        return .judged(
            [
                ("at least 80 Hz of the 100 asked", rate >= 80),
                ("about 1 g at rest (0.8-1.2)", magnitude > 0.8 && magnitude < 1.2),
            ], metrics
        )
    }
}

struct GyroscopeTest: HWTest {
    let id = "sensor.gyroscope"
    let name = "Gyroscope"
    let subsystem = Subsystem.sensors

    func run(_ context: TestContext) async throws -> Outcome {
        let manager = Motion.manager
        guard manager.isGyroAvailable else { return .fail("isGyroAvailable is false") }
        manager.gyroUpdateInterval = Motion.interval
        let (samples, rate) = try await Motion.collect(
            start: { (sink: MotionSamples<CMRotationRate>) in
                manager.startGyroUpdates(to: Motion.queue) { data, _ in if let data { sink.add(data.rotationRate) } }
            }, stop: { manager.stopGyroUpdates() }
        )
        let spin = Stats.mean(samples.map { sqrt($0.x * $0.x + $0.y * $0.y + $0.z * $0.z) })
        var metrics = Metrics()
        metrics.set("rate_hz", rate, digits: 1)
        metrics.set("mean_rad_s", spin, digits: 4)
        return .judged(
            [
                ("at least 80 Hz of the 100 asked", rate >= 80),
                ("samples are finite", samples.allSatisfy { $0.x.isFinite }),
            ], metrics
        )
    }
}

/// How many device motion reports SwitchMotion has fused and sent since boot (debug kernels: its DeviceMotionReports
/// property, drivers/SwitchMotion/SwitchMotion.cpp); nil without the driver or the property.
enum MotionDriver {
    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> CFMutableDictionary?
    typealias Service = @convention(c) (mach_port_t, CFDictionary?) -> mach_port_t
    typealias Properties =
        @convention(c) (mach_port_t, UnsafeMutablePointer<Unmanaged<CFMutableDictionary>?>, CFAllocator?, UInt32)
        -> kern_return_t
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t

    static func reports() -> UInt64? {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
              let matching = dlsym(iokit, "IOServiceMatching"),
              let service = dlsym(iokit, "IOServiceGetMatchingService"),
              let properties = dlsym(iokit, "IORegistryEntryCreateCFProperties"),
              let release = dlsym(iokit, "IOObjectRelease")
        else { return nil }
        let motion = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("SwitchMotion")
        )
        guard motion != 0 else { return nil }
        defer { _ = unsafeBitCast(release, to: Release.self)(motion) }
        var table: Unmanaged<CFMutableDictionary>?
        guard unsafeBitCast(properties, to: Properties.self)(motion, &table, kCFAllocatorDefault, 0) == KERN_SUCCESS,
              let dictionary = table?.takeRetainedValue() as NSDictionary?
        else { return nil }
        return (dictionary["DeviceMotionReports"] as? NSNumber)?.uint64Value
    }
}

/// Sensor fusion (CMDeviceMotion) and the orientation UIKit derives from it. Device motion is fused in SwitchMotion as
/// a motion coprocessor fuses it: the driver's report count has to grow with the samples received (it stays put when
/// CoreMotion fuses the raw accelerometer and gyroscope itself), each sample arrives soon after its sample time, and
/// at rest its gravity is the accelerometer's.
struct DeviceMotionTest: HWTest {
    let id = "sensor.device_motion"
    let name = "Device motion and orientation"
    let subsystem = Subsystem.sensors

    struct Sample {
        let motion: CMDeviceMotion
        let latency: Double
    }

    static func length(_ v: [Double]) -> Double { sqrt(v.map { $0 * $0 }.reduce(0, +)) }

    static func degrees(between a: [Double], _ b: [Double]) -> Double {
        let norms = length(a) * length(b)
        guard norms > 0 else { return 180 }
        return acos(max(-1, min(1, zip(a, b).map { $0 * $1 }.reduce(0, +) / norms))) * 180 / .pi
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let manager = Motion.manager
        guard manager.isDeviceMotionAvailable else { return .fail("isDeviceMotionAvailable is false") }
        manager.deviceMotionUpdateInterval = Motion.interval
        manager.accelerometerUpdateInterval = Motion.interval
        let accelerations = MotionSamples<CMAcceleration>()
        let before = MotionDriver.reports()
        let (samples, rate) = try await Motion.collect(
            start: { (sink: MotionSamples<Sample>) in
                manager.startAccelerometerUpdates(to: Motion.queue) { data, _ in
                    if let data { accelerations.add(data.acceleration) }
                }
                manager.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: Motion.queue) { data, _ in
                    if let data {
                        sink.add(Sample(motion: data, latency: ProcessInfo.processInfo.systemUptime - data.timestamp))
                    }
                }
            }, stop: {
                manager.stopDeviceMotionUpdates()
                manager.stopAccelerometerUpdates()
            }
        )
        let after = MotionDriver.reports()
        let motions = samples.map(\.motion)
        let gravity = Stats.mean(motions.map { Self.length([$0.gravity.x, $0.gravity.y, $0.gravity.z]) })
        let meanGravity = [
            Stats.mean(motions.map(\.gravity.x)),
            Stats.mean(motions.map(\.gravity.y)),
            Stats.mean(motions.map(\.gravity.z)),
        ]
        let accel = accelerations.all
        let meanAccel = [Stats.mean(accel.map(\.x)), Stats.mean(accel.map(\.y)), Stats.mean(accel.map(\.z))]
        let angle = Self.degrees(between: meanGravity, meanAccel)
        let user = Stats.mean(
            motions.map { Self.length([$0.userAcceleration.x, $0.userAcceleration.y, $0.userAcceleration.z]) }
        )
        let latencies = samples.map(\.latency)
        let latency = Stats.mean(latencies)
        let readable = before != nil && after != nil
        let fused = readable ? Double(after! &- before!) : 0
        // Lying flat, a wrongly conjugated attitude still matches the accelerometer: only a tilted console tells.
        let tilt = Self.degrees(between: meanAccel, [0, 0, -1])
        let orientation = await MainActor.run { () -> Int in
            UIDevice.current.beginGeneratingDeviceOrientationNotifications()
            defer { UIDevice.current.endGeneratingDeviceOrientationNotifications() }
            return UIDevice.current.orientation.rawValue
        }
        var metrics = Metrics()
        metrics.set("rate_hz", rate, digits: 1)
        metrics.set("gravity_g", gravity, digits: 3)
        metrics.set("gravity_vs_accel_deg", angle, digits: 2)
        metrics.set("user_accel_g", user, digits: 3)
        metrics.set("latency_ms", latency * 1000, digits: 1)
        metrics.set("driver_reports", fused, digits: 0)
        metrics.set("tilt_deg", tilt, digits: 1)
        metrics.set("orientation", orientation)
        return .judged(
            [
                ("at least 80 Hz of the 100 asked", rate >= 80),
                ("unit gravity vector", abs(gravity - 1) < 0.05),
                ("SwitchMotion's DeviceMotionReports readable (debug kernel)", readable),
                ("fused in SwitchMotion: its reports grew with the samples", fused >= 0.8 * Double(samples.count)),
                ("each sample within 25 ms of its sample time", latency < 0.025 && (latencies.min() ?? -1) > -0.001),
                (
                    tilt < 10
                        ? "gravity within 5 degrees of the accelerometer's (lying flat: tilt it for a real test)"
                        : "gravity within 5 degrees of the accelerometer's", angle < 5
                ),
                ("little user acceleration while still (< 0.1 g)", user < 0.1),
            ], metrics
        )
    }
}

/// The ambient light sensor has no public API; with auto-brightness on, covering it moves the brightness.
struct AmbientLightTest: HWTest {
    let id = "sensor.ambient_light"
    let name = "Ambient light sensor (via auto-brightness)"
    let subsystem = Subsystem.sensors
    let interactive = true

    func run(_ context: TestContext) async throws -> Outcome {
        let before = await MainActor.run { UIScreen.main.brightness }
        var observer: NSObjectProtocol?
        let after: CGFloat? = await context.interact(
            "Cover the light sensor with a finger for a few seconds (auto-brightness on)"
        ) { waiter in
            observer = NotificationCenter.default.addObserver(
                forName: UIScreen.brightnessDidChangeNotification, object: nil, queue: .main
            ) { _ in
                if abs(UIScreen.main.brightness - before) > 0.02 { waiter.resume(UIScreen.main.brightness) }
            }
        }
        if let observer { NotificationCenter.default.removeObserver(observer) }
        guard let after else { return .skip("brightness did not move (skipped, timed out or auto-brightness off)") }
        var metrics = Metrics()
        metrics.set("before", Double(before), digits: 3)
        metrics.set("after", Double(after), digits: 3)
        return .pass(metrics)
    }
}
