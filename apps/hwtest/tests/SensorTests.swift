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

/// What SwitchMotion reports of itself on debug kernels (drivers/SwitchMotion/SwitchMotion.cpp serializeProperties):
/// the device motion reports it has fused and sent since boot, the fast path copies that answered one
/// (SwitchHIDFastPath.cpp), and the raw gyroscope's report interval, 0 while no client streams it. nil without the
/// driver or the properties (a release kernel).
enum MotionDriver {
    struct State {
        let reports: UInt64
        let copies: UInt64
        let gyroUS: UInt32
    }

    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> CFMutableDictionary?
    typealias Service = @convention(c) (mach_port_t, CFDictionary?) -> mach_port_t
    typealias Properties =
        @convention(c) (mach_port_t, UnsafeMutablePointer<Unmanaged<CFMutableDictionary>?>, CFAllocator?, UInt32)
        -> kern_return_t
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t

    static func state() -> State? {
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
        guard let reports = dictionary["DeviceMotionReports"] as? NSNumber,
              let copies = dictionary["DeviceMotionFastPathCopies"] as? NSNumber,
              let gyro = dictionary["GyroReportInterval"] as? NSNumber
        else { return nil }
        return State(reports: reports.uint64Value, copies: copies.uint64Value, gyroUS: gyro.uint32Value)
    }
}

/// Sensor fusion (CMDeviceMotion) and the orientation UIKit derives from it. Device motion is fused in SwitchMotion as
/// a motion coprocessor fuses it: the driver's report count has to grow with the samples received, CoreMotion has to
/// read reports through the fast path, and the raw gyroscope has to stay unstreamed meanwhile, since CoreMotion fusing
/// on the CPU (here or in another process) needs it. Each sample arrives soon after its sample time, and at rest its gravity is the accelerometer's. Lying flat, a
/// conjugated attitude matches the accelerometer as well as the right one does, so a flat console never passes: with
/// every other check holding, the verdict is SKIP until the console is tilted (docked, handheld, propped up).
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

    /// Where the samples were fused, from SwitchMotion's counters across the run; the detail when they are absent.
    /// 24B119 opens {0xff0c, 5} for this frame
    /// (CLDeviceMotion::enableSensorFusionWithMode, mode 3, caps bit 4) and reads the latest report through the fast path
    /// (CLSensorFusionServiceSPU); which 21H461 mode .xArbitraryZVertical takes was not read, and there SpringBoard's
    /// parallax streams the raw gyroscope itself, so the fusion rows are 24B119's only.
    static func driverChecks(
        _ before: MotionDriver.State?, _ during: MotionDriver.State?, samples: Int, _ checks: inout [(String, Bool)],
        _ metrics: inout Metrics
    ) -> String {
        guard let before, let during else {
            return "SwitchMotion's counters are not published (release kernel): where it was fused is not checked"
        }
        let fused = Double(during.reports &- before.reports)
        let copied = during.copies &- before.copies
        metrics.set("driver_reports", fused, digits: 0)
        metrics.set("fastpath_copies", Int(copied))
        metrics.set("raw_gyro_us", Int(during.gyroUS))
        guard ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 else { return "" }
        checks += [
            ("fused in SwitchMotion: its reports grew with the samples", fused >= 0.8 * Double(samples)),
            ("CoreMotion read reports through the fast path", copied > 0),
            ("nobody streamed the raw gyroscope (CPU fusion needs it)", during.gyroUS == 0),
        ]
        return ""
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let manager = Motion.manager
        guard manager.isDeviceMotionAvailable else { return .fail("isDeviceMotionAvailable is false") }
        manager.deviceMotionUpdateInterval = Motion.interval
        manager.accelerometerUpdateInterval = Motion.interval
        let accelerations = MotionSamples<CMAcceleration>()
        let before = MotionDriver.state()
        var during: MotionDriver.State?
        let (samples, rate) = try await Motion.collect(
            start: { (sink: MotionSamples<Sample>) in
                manager.startAccelerometerUpdates(to: Motion.queue) { data, _ in
                    if let data { accelerations.add(data.acceleration) }
                }
                manager.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: Motion.queue) { data, _ in
                    if let data {
                        sink.add(Sample(motion: data, latency: ProcessInfo.processInfo.systemUptime - data.timestamp))
                    }
                    // A pull as well as the push, in case CoreMotion takes the fast path only for one of them.
                    _ = manager.deviceMotion
                }
            }, stop: {
                during = MotionDriver.state()
                manager.stopDeviceMotionUpdates()
                manager.stopAccelerometerUpdates()
            }
        )
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
        metrics.set("tilt_deg", tilt, digits: 1)
        metrics.set("orientation", orientation)
        var checks = [
            ("at least 80 Hz of the 100 asked", rate >= 80),
            ("unit gravity vector", abs(gravity - 1) < 0.05),
            ("each sample within 25 ms of its sample time", latency < 0.025 && (latencies.min() ?? -1) > -0.001),
            ("gravity within 5 degrees of the accelerometer's", angle < 5),
            ("little user acceleration while still (< 0.1 g)", user < 0.1),
        ]
        let detail = Self.driverChecks(before, during, samples: samples.count, &checks, &metrics)
        let outcome = Outcome.judged(checks, metrics, detail: detail)
        guard outcome.verdict == .pass, tilt < 10 else { return outcome }
        return .skip(
            "lying flat (\(String(format: "%.1f", tilt)) degrees): a conjugated attitude passes too; tilt the console "
                + "10 degrees or more",
            metrics
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
