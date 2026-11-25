// Guards for what switch_motion_test's models cannot see: the sample and FIFO drain paths never wait or poll (the
// models' clock does not run while code spins), the motion services' HID descriptor (SwitchHIDDevice.cpp, a stand-in in
// the scenarios) carries the Sensor-page usages 21H461's IOHIDEventDriver knows, the orientation device serves the
// descriptor orientation_pipeline_test checks, SwitchBacklight follows the panel's power into the orientation
// detector, and the device motion service's event driver takes CoreMotion's fast path (SwitchHIDFastPath.cpp).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchMotionSource, SamplePathsNeverWait)
{
    const std::string source = SourceText("../SwitchMotion.cpp");
    for (const char *signature :
         {"void SwitchMotion::sample(", "void SwitchMotion::drainFifo(", "void SwitchMotion::fuse("}) {
        const std::string body = Body(source, signature);
        ASSERT_FALSE(body.empty()) << signature;
        for (const char *wait : {"IOSleep", "IODelay", "while (", "for (;;)"})
            EXPECT_THAT(body, Not(HasSubstr(wait))) << signature << " " << wait;
    }
}

// Report Interval 0x30e and Report Latency 0x31b; 0x316 is Reporting State, which no element takes (BatchInterval never
// reached the driver and FIFO batching never ran before r17).
TEST(SwitchMotionSource, SensorPropertyUsages)
{
    const std::string motion = Between(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"), "#define MOTION_DESCRIPTOR",
                                       "#undef MOTION_DESCRIPTOR");
    EXPECT_THAT(motion, HasSubstr("0x0a, 0x0e, 0x03,"));
    EXPECT_THAT(motion, HasSubstr("0x0a, 0x1b, 0x03,"));
    EXPECT_THAT(motion, Not(HasSubstr("0x0a, 0x16, 0x03")));
}

// CoreMotion's 6-axis device motion: AppleVendorMotion 0xff0c / 5, the payload as one element of the build's length
// inside an AppleVendor Message collection, then the same Report Interval and Report Latency features.
TEST(SwitchMotionSource, DeviceMotionDescriptor)
{
    const std::string hid = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    const std::string descriptor = Between(hid, "const uint8_t kDeviceMotionDescriptor[] = {", "};");
    EXPECT_THAT(descriptor, HasSubstr("0x06, 0x0c, 0xff, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,"));
    EXPECT_THAT(descriptor, HasSubstr("0x06, 0x00, 0xff, 0x09, 0x23, 0xa1, 0x00,"));
    EXPECT_THAT(descriptor, HasSubstr("0x95, kDeviceMotionBytes, 0x81, 0x02, 0xc0,"));
    EXPECT_THAT(descriptor, HasSubstr("0x0a, 0x0e, 0x03,"));
    EXPECT_THAT(descriptor, HasSubstr("0x0a, 0x1b, 0x03,"));
    EXPECT_THAT(
        hid, HasSubstr("{kDeviceMotionDescriptor, sizeof(kDeviceMotionDescriptor), sizeof(SwitchDeviceMotionReport),"));
}

// orientation_pipeline_test checks kSwitchOrientationDescriptor against the report; the device must serve that one.
TEST(SwitchMotionSource, OrientationDeviceServesTheCheckedDescriptor)
{
    EXPECT_THAT(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"),
                HasSubstr("{kSwitchOrientationDescriptor, sizeof(kSwitchOrientationDescriptor), "
                          "sizeof(SwitchOrientationReport),"));
}

TEST(SwitchMotionSource, BacklightFollowsPanelPower)
{
    // The weak default, off and on.
    EXPECT_EQ(Count(SourceText("../../SwitchBrightness/SwitchBacklight.cpp"), "switch_motion_display_power("), 3u);
}

// CoreMotion's orientation notifier opens the orientation device with AppleSPULib's AppleSPUHIDDevice plug-in and asks
// LastOrientation through its 'ashd' user client: a 10-byte reply of the echo, the orientation and the sample time.
TEST(SwitchMotionSource, OrientationDeviceAnswersTheAopPlugIn)
{
    const std::string hid = SourceText("../../SwitchTouch/SwitchHIDDevice.cpp");
    EXPECT_THAT(hid, HasSubstr("types->setObject(\"7ACF5332-1A35-4893-87CB-BA64E1887FAE\", plugIn)"));
    EXPECT_THAT(hid, HasSubstr("return switch_motion_orientation_client(this, task, security, type, handler);"));
    EXPECT_THAT(hid, HasSubstr("return IOHIDDevice::newUserClient(task, security, type, properties, handler);"));
    const std::string client = SourceText("../SwitchOrientationClient.cpp");
    EXPECT_THAT(client, HasSubstr("kSelectorReset = 0, kSelectorOpen = 4, kSelectorClose = 5, kSelectorCommand = 6"));
    EXPECT_THAT(client, HasSubstr("constexpr uint32_t kLastSize = 10"));
    EXPECT_THAT(client, HasSubstr("switch_motion_last_orientation(&orientation, &stamp);"));
    const std::string motion = SourceText("../SwitchMotion.cpp");
    EXPECT_THAT(Body(motion, "void SwitchMotion::announce("), HasSubstr("__atomic_store_n(&gLastOrientation,"));
}

// The slots both builds' IOHIDEventServiceFastPathUserClient calls (SwitchHIDFastPath.cpp's file comment), only for the
// builds they were read from, the plug-in type IOHIDServiceClientFastPathInit asks for, and the hook installed from the
// first publish of the device motion device's driver, before it starts.
TEST(SwitchMotionSource, DeviceMotionTakesTheFastPath)
{
    const std::string fast = SourceText("../../SwitchTouch/SwitchHIDFastPath.cpp");
    for (const char *slot : {"kDispatch = 198,", "kPrimaryUsagePage = 199,", "kPrimaryUsage = 200,",
                             "kCopyEventForClient = 205,", "kOpenForClient = 208,", "kCloseForClient = 209,"})
        EXPECT_THAT(fast, HasSubstr(slot));
    EXPECT_THAT(fast, HasSubstr("\"0B842ADD-C395-4352-95F6-6585EB515623\""));
    EXPECT_THAT(fast, HasSubstr("\"IOHIDFamily.kext/PlugIns/IOHIDLib.plugin\""));
    EXPECT_THAT(fast, HasSubstr("#if SWITCH_IPSW_KIND_24B119 || SWITCH_IPSW_KIND_21H461"));
    EXPECT_THAT(fast, HasSubstr("gIOFirstPublishNotification"));
    const std::string create =
        Body(SourceText("../../SwitchTouch/SwitchHIDDevice.cpp"), "IOService *switch_hid_create(");
    EXPECT_LT(create.find("switch_hid_fastpath_watch(device)"), create.find("device->start(resources)"));
}
