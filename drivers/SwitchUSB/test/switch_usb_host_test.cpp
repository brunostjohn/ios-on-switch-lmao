// Host mode on the USB-C port: SwitchPD's power-role decisions and SwitchUSB's data role (drivers/SwitchUSB/
// SwitchPDPolicy.h) and the host role's register arithmetic (drivers/SwitchUSB/XusbPorts.h), compiled from the same
// headers the kernel uses (switch_usb_host_harness.c).
//
// The power role runs the BM92T36 traces r7w recorded on hardware (11-05 12:47: a keyboard, mouse and trackpad through
// a USB-C adapter, then the Mac) through a model of SwitchPD.cpp's apply/enter, and checks: the fixed source supplies
// are ready before the first switch arm and through classification; boost is owned only by Reattach and Source;
// a re-attach whose VSAFE is our own
// (SPDSRC1/2 closed, as with the rails up first) sources, a VSAFE nobody of ours drives does not; and over a fuzzed walk
// nothing but a classified re-attach ever sources, and nothing sources without host mode.
//
// The routing the host interrupts rely on (INTID 71 xHCI, 72 falcon mailbox: DT GIC_SPI 39/40 level-high through the
// LIC, IPFS as tegra_xusb_config) is checked against the OLED DT and the driver sources in
// switch_usb_host_source_test.cpp.
//
// The host role: the USB 2.0 ports come from the xHCI Supported Protocol capabilities (pad 0 = the first one; a broken
// list ends the walk), PORTSC writes never set a write-1 bit (PED, PR, the change bits) and only move PP, and the pad
// controller values for each role (ID grounded with the VBUS override dropped first, floating for the XUDC; the
// charger detect pull overrides and sources off, port 0's over-current pin off) match xusb-tegra210.c.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "bazel/testing/require.h"
#include "switch_usb_host_harness.h"

namespace {

enum Phase {
    EMPTY,
    SINK,
    CLASSIFY,
    REATTACH,
    SOURCE,
    HELD
};

enum Act {
    NONE,
    SAW_DETACH,
    ENTER,
    START,
    END
};

// BM92T36 bits (bm92txx.c).
constexpr uint16_t SRC_FAULT = 1 << 1, PLUGPULL = 1 << 3, CONTRACT = 1 << 12;
constexpr unsigned SPDSRC2 = 1u << 3, INSERT = 1u << 7, VSAFE = 1u << 10, CSIDE = 1u << 11, SRC_MODE = 1u << 12,
                   SPDSNK = 1u << 14, SPDSRC1 = 1u << 15;
constexpr unsigned DFP = 2u << 8, UFP = 1u << 8;
constexpr unsigned OTG_INSERT = 1u << 13;

using Status = std::pair<uint16_t, uint16_t>;  // STATUS1, STATUS2
// r7w (live-1105-1249, kernel log): STATUS1/STATUS2 as SwitchPD logged them.
constexpr Status R7W_SINK_ATTACH = {0x1280, 0x2000};      // SRC_MODE | DFP | INSERT, OTG_INSERT: the keyboard's adapter
constexpr Status R7W_SINK_ATTACH_CC2 = {0x1A80, 0x2000};  // the same on CC2
constexpr Status R7W_DETACH_SOURCING = {0x8400, 0};       // SPDSRC1 | VSAFE, no INSERT: unplugged while we sourced
constexpr Status R7W_MAC_SOURCES = {0x0D80, 0};           // the Mac took the source role with our switches off
constexpr Status R7W_CHARGER_AT_BOOT = {0x4580, 0x0001};  // SPDSNK | VSAFE | UFP | INSERT
// what the re-attach shows once our switch closes onto v_vdd5v3
constexpr Status REATTACH_RAILS_UP = {0x1280 | SPDSRC1 | VSAFE, 0x2000};
// The traces, bit by bit as their comments read them.
static_assert(R7W_SINK_ATTACH.first == (SRC_MODE | DFP | INSERT) && R7W_SINK_ATTACH.second == OTG_INSERT);
static_assert(R7W_SINK_ATTACH_CC2.first == (SRC_MODE | CSIDE | DFP | INSERT));
static_assert(R7W_DETACH_SOURCING.first == (SPDSRC1 | VSAFE));
static_assert(R7W_MAC_SOURCES.first == (CSIDE | VSAFE | UFP | INSERT));
static_assert(R7W_CHARGER_AT_BOOT.first == (SPDSNK | VSAFE | UFP | INSERT));

struct Decision {
    int act, next;
    bool cancel;
    std::string why;
};

Decision decide(int phase, uint16_t alert, uint16_t status1, uint16_t status2, bool allow = true)
{
    int next = 0;
    int cancel = 0;
    char why[256];
    const int act = pd_decide(phase, alert, status1, status2, allow ? 1 : 0, &next, &cancel, why, sizeof(why));
    return {act, next, cancel != 0, why};
}

Decision decide(int phase, uint16_t alert, Status status, bool allow = true)
{
    return decide(phase, alert, status.first, status.second, allow);
}

Decision windowEnd(int phase, bool saw, uint16_t status1)
{
    int next = 0;
    char why[256];
    const int act = pd_window_end(phase, saw ? 1 : 0, status1, &next, why, sizeof(why));
    return {act, next, false, why};
}

// A model invariant (the Python model's assert): a failure stops the test at the first violation.
void invariant(bool ok, const std::string &what)
{
    if (!ok) {
        ADD_FAILURE() << "model invariant: " << what;
        // NOLINTNEXTLINE(bugprone-std-exception-baseclass): not a std::exception on purpose (require.h)
        throw switch_ios::test::RequireFailed();
    }
}

using Event = std::pair<std::string, bool>;

// SwitchPD.cpp's apply/enter/startSourcing/endSourcing/windowFired, with the hardware as recorded outputs.
struct Port {
    explicit Port(bool allow = true, int boostResult = 0) : allow(allow), boostResult(boostResult)
    {}

    int phase = EMPTY;
    bool allow, saw = false, window = false;
    bool switches = false, rails = false, boost = false;
    bool railFailure = false;
    int boostResult;  // 0 ok, -2 the charger sees VBUS that is not ours, -1 I2C
    std::vector<Event> events;
    std::vector<std::string> lines;
    uint16_t status1 = 0;

    void setSwitches(bool on)
    {
        if (on != switches) {
            switches = on;
            events.emplace_back("switches", on);
        }
    }

    bool setRails(bool on)
    {
        if (on != rails) {
            setSwitches(false);
            events.emplace_back("rails", on);
            if (on && railFailure)
                return false;
            rails = on;
        }
        return true;
    }

    int setBoost(bool on)
    {
        events.emplace_back("boost", on);
        const int result = !on || (boostResult == -2 && rails && switches && pd_vbus_ours(status1)) ? 0 : boostResult;
        if (!result)
            boost = on;
        return result;
    }

    void enter(int next, const std::string &why)
    {
        const bool boosting = pd_boost_on(phase) != 0;
        phase = next;
        saw = false;
        window = false;
        const bool on = pd_switches_on(next, allow ? 1 : 0) != 0;
        const bool railsOn = allow && pd_rails_on(next) != 0;
        const bool boostOn = pd_boost_on(next) != 0;
        if (!on || (boosting && !boostOn))
            setSwitches(false);
        if (boosting && !boostOn)
            (void)setBoost(false);
        if (boostOn && !boosting) {
            const int result = setBoost(true);
            if (result) {
                enter(result == -2 ? SINK : HELD, "not sourcing: boost failed");
                return;
            }
        }
        if (!setRails(railsOn)) {
            enter(HELD, "not sourcing: source rails failed");
            return;
        }
        lines.push_back(why);
        if (on)
            setSwitches(true);
        if (next == CLASSIFY || next == REATTACH)
            window = true;
    }

    void start()
    {
        phase = SOURCE;
        lines.emplace_back("sourcing");
    }

    void end(int next, const std::string &why)
    {
        enter(next, why);
    }

    void apply(const Decision &d)
    {
        if (d.cancel)
            window = false;
        if (d.act == SAW_DETACH) {
            saw = true;
        } else if (d.act == ENTER) {
            enter(d.next, d.why);
        } else if (d.act == START) {
            start();
        } else if (d.act == END) {
            end(d.next, d.why);
        }
    }

    void alert(Status status, uint16_t alertBits = PLUGPULL)
    {
        status1 = status.first;
        apply(decide(phase, alertBits, status.first, status.second, allow && rails && switches));
        check();
    }

    void windowEnds(Status status)
    {
        if (!window)
            return;
        window = false;
        const Decision d = windowEnd(phase, saw, status.first);
        if (d.act == NONE)
            return;
        status1 = status.first;
        apply({d.act, d.next, false, d.why});
        if (phase == d.next)
            apply(decide(phase, 0, status.first, status.second, allow && rails && switches));
        check();
    }

    void check() const
    {
        const std::string at = "phase " + std::to_string(phase);
        invariant(rails == (allow && pd_rails_on(phase)),
                  at + ": rails " + std::to_string(static_cast<int>(rails)));
        invariant(boost == (pd_boost_on(phase) != 0),
                  at + ": boost " + std::to_string(static_cast<int>(boost)));
        if (!allow) {
            invariant(phase != CLASSIFY && phase != REATTACH && phase != SOURCE && !rails && !switches,
                      at + " without host mode");
        }
    }
};

// --- Power role ------------------------------------------------------------------------------------------------------

TEST(SwitchUsbHostPowerRole, OutputsPerPhase)
{
    for (int phase = 0; phase < 6; ++phase) {
        EXPECT_EQ(pd_rails_on(phase) != 0, phase != HELD) << phase;
        EXPECT_EQ(pd_boost_on(phase) != 0, phase == REATTACH || phase == SOURCE) << phase;
        EXPECT_FALSE(pd_switches_on(phase, 0)) << phase;
        EXPECT_EQ(pd_switches_on(phase, 1) != 0, phase != SINK && phase != CLASSIFY) << phase;
    }
    EXPECT_STREQ(pd_phase_name(REATTACH), "reattach");
}

// r7w's plug-in, with the re-attach as the rails-first order makes it: sourced, rails before switches.
TEST(SwitchUsbHostPowerRole, KeyboardThroughAnAdapter)
{
    for (const Status attach : {R7W_SINK_ATTACH, R7W_SINK_ATTACH_CC2}) {
        SCOPED_TRACE(attach.first);
        Port port;
        port.enter(EMPTY, "start");
        EXPECT_TRUE(port.switches);
        EXPECT_TRUE(port.rails);
        EXPECT_FALSE(port.boost);
        EXPECT_EQ(port.events, (std::vector<Event>{{"rails", true}, {"switches", true}}));
        port.events.clear();
        port.alert(attach);
        EXPECT_EQ(port.phase, CLASSIFY);
        EXPECT_EQ(port.events, (std::vector<Event>{{"switches", false}}));
        port.alert({0, 0});  // it left with our switches off
        EXPECT_TRUE(port.saw);
        port.events.clear();
        port.windowEnds({0, 0});
        EXPECT_EQ(port.phase, REATTACH);
        EXPECT_EQ(port.events, (std::vector<Event>{{"boost", true}, {"switches", true}}));
        EXPECT_TRUE(port.boost);
        port.events.clear();
        port.alert(REATTACH_RAILS_UP);
        EXPECT_EQ(port.phase, SOURCE);
        EXPECT_TRUE(port.boost && port.rails && port.switches);
        EXPECT_TRUE(port.events.empty());
        port.events.clear();
        port.alert(R7W_DETACH_SOURCING);
        EXPECT_EQ(port.phase, EMPTY);
        EXPECT_EQ(port.events,
                  (std::vector<Event>{{"switches", false}, {"boost", false}, {"switches", true}}));
        EXPECT_TRUE(port.switches && port.rails);
        ASSERT_FALSE(port.lines.empty());
        EXPECT_THAT(port.lines.back(), testing::HasSubstr("detached"));
    }
}

// r7w's own re-attach status (no VSAFE yet) sources too.
TEST(SwitchUsbHostPowerRole, ReattachBeforeOurSwitchCloses)
{
    const Decision d = decide(REATTACH, PLUGPULL, R7W_SINK_ATTACH);
    EXPECT_EQ(std::make_tuple(d.act, d.next, d.cancel), std::make_tuple((int)START, (int)SOURCE, true));
}

TEST(SwitchUsbHostPowerRole, VsafeThatIsNotOurs)
{
    Decision d = decide(REATTACH, PLUGPULL, SRC_MODE | VSAFE | DFP | INSERT, OTG_INSERT);
    EXPECT_EQ(std::make_tuple(d.act, d.next, d.cancel), std::make_tuple((int)ENTER, (int)SINK, true));
    EXPECT_THAT(d.why, testing::HasSubstr("not ours"));
    EXPECT_TRUE(pd_vbus_ours(SPDSRC2 | VSAFE));
    EXPECT_FALSE(pd_vbus_ours(VSAFE | INSERT | SRC_MODE));
    d = decide(EMPTY, PLUGPULL, REATTACH_RAILS_UP);
    EXPECT_EQ(std::make_pair(d.act, d.next), std::make_pair((int)ENTER, (int)CLASSIFY));
    d = decide(EMPTY, PLUGPULL, REATTACH_RAILS_UP, false);
    EXPECT_EQ(std::make_pair(d.act, d.next), std::make_pair((int)ENTER, (int)SINK));
    d = decide(EMPTY, PLUGPULL, SRC_MODE | VSAFE | DFP | INSERT, OTG_INSERT);
    EXPECT_EQ(std::make_pair(d.act, d.next), std::make_pair((int)ENTER, (int)SINK));
}

TEST(SwitchUsbHostPowerRole, ChargerSeesVbusThatIsNotOurs)
{
    Port port(true, -2);
    port.enter(REATTACH, "proven");
    EXPECT_EQ(port.phase, SINK);
    EXPECT_TRUE(port.rails);
    EXPECT_FALSE(port.switches || port.boost);
    EXPECT_EQ(port.events, (std::vector<Event>{{"boost", true}, {"boost", false}, {"rails", true}}));
    Port unpowered(true, -2);
    unpowered.status1 = REATTACH_RAILS_UP.first;
    unpowered.enter(REATTACH, "proven");
    EXPECT_EQ(unpowered.phase, SINK);
    EXPECT_TRUE(unpowered.rails);
    EXPECT_FALSE(unpowered.boost || unpowered.switches);
    Port closed(true, -2);
    closed.enter(EMPTY, "start");
    closed.status1 = REATTACH_RAILS_UP.first;
    closed.enter(REATTACH, "proven");
    closed.alert(REATTACH_RAILS_UP);  // our switch closed: what it sees is v_vdd5v3
    EXPECT_EQ(closed.phase, SOURCE);
}

TEST(SwitchUsbHostPowerRole, BoostFailureLeavesRailsUnpowered)
{
    Port port(true, -1);
    port.enter(REATTACH, "proven");
    EXPECT_EQ(port.phase, HELD);
    EXPECT_FALSE(port.rails || port.boost || port.window);
    EXPECT_EQ(port.events, (std::vector<Event>{{"boost", true}, {"boost", false}, {"switches", true}}));
}

TEST(SwitchUsbHostPowerRole, ClassifyWindowKeepsBoostRefusalLatched)
{
    for (const int result : {-2, -1}) {
        Port port(true, result);
        port.enter(EMPTY, "start");
        port.alert(R7W_SINK_ATTACH);
        port.alert({0, 0});
        port.events.clear();
        port.windowEnds({0, 0});
        EXPECT_EQ(port.phase, result == -2 ? SINK : HELD);
        EXPECT_EQ(port.rails, result == -2);
        EXPECT_FALSE(port.boost || port.window);
        std::vector<Event> expected = {{"boost", true}, {"boost", false}};
        if (result == -1) {
            expected.emplace_back("rails", false);
            expected.emplace_back("switches", true);
        }
        EXPECT_EQ(port.events, expected);
    }
}

TEST(SwitchUsbHostPowerRole, RailFailureReleasesPreparedBoost)
{
    Port port;
    port.railFailure = true;
    port.enter(REATTACH, "proven");
    EXPECT_EQ(port.phase, HELD);
    EXPECT_FALSE(port.rails || port.boost || port.window);
    EXPECT_EQ(port.events,
              (std::vector<Event>{{"boost", true}, {"rails", true}, {"boost", false}, {"switches", true}}));
}

TEST(SwitchUsbHostPowerRole, OledDockBoostIsReadyWhileWaitingForReattach)
{
    Port port;
    port.enter(EMPTY, "start");
    port.alert({0x1280, 0x2000});
    port.alert({0, 0});
    port.events.clear();
    port.windowEnds({0, 0});
    EXPECT_EQ(port.phase, REATTACH);
    EXPECT_TRUE(port.window && port.boost && port.rails && port.switches);
    EXPECT_EQ(port.events, (std::vector<Event>{{"boost", true}, {"switches", true}}));
    port.events.clear();
    port.windowEnds({0, 0});
    EXPECT_EQ(port.phase, EMPTY);
    EXPECT_FALSE(port.boost || port.window);
    EXPECT_TRUE(port.rails && port.switches);
    EXPECT_EQ(port.events,
              (std::vector<Event>{{"switches", false}, {"boost", false}, {"switches", true}}));
}

TEST(SwitchUsbHostPowerRole, ReattachPowerIsReleasedOnRoleChangeAndStop)
{
    Port port;
    port.enter(REATTACH, "proven");
    port.events.clear();
    port.alert(R7W_MAC_SOURCES);
    EXPECT_EQ(port.phase, SINK);
    EXPECT_TRUE(port.rails);
    EXPECT_FALSE(port.boost || port.switches || port.window);
    EXPECT_EQ(port.events, (std::vector<Event>{{"switches", false}, {"boost", false}}));
    for (int next : {EMPTY, SINK, HELD}) {
        Port stopped;
        stopped.enter(REATTACH, "proven");
        stopped.allow = false;
        stopped.enter(next, "stop");
        EXPECT_FALSE(stopped.boost || stopped.rails || stopped.switches || stopped.window);
        stopped.check();
    }
}

TEST(SwitchUsbHostPowerRole, ReattachFaultReleasesPreparedPower)
{
    for (const uint16_t fault : {uint16_t{SRC_FAULT}, uint16_t{0}}) {
        Port port;
        port.enter(REATTACH, "proven");
        port.events.clear();
        const Status status = {static_cast<uint16_t>(R7W_SINK_ATTACH.first | (fault ? 0 : 1)), OTG_INSERT};
        const Decision d = decide(REATTACH, fault, status);
        EXPECT_EQ(std::make_tuple(d.act, d.next, d.cancel), std::make_tuple((int)END, (int)HELD, true));
        port.alert(status, fault);
        EXPECT_EQ(port.phase, HELD);
        EXPECT_FALSE(port.boost || port.rails || port.window);
        EXPECT_EQ(port.events,
                  (std::vector<Event>{{"switches", false}, {"boost", false}, {"rails", false}, {"switches", true}}));
    }
}

TEST(SwitchUsbHostPowerRole, MacIsDualRole)
{
    Port port;
    port.enter(EMPTY, "start");
    port.events.clear();
    port.alert(R7W_SINK_ATTACH_CC2);
    port.alert(R7W_MAC_SOURCES);
    EXPECT_EQ(port.phase, SINK);
    EXPECT_TRUE(port.rails);
    EXPECT_FALSE(port.window || port.boost || port.switches);
    for (const Event &e : port.events) {
        EXPECT_NE(e, Event("rails", true));
        EXPECT_NE(e, Event("boost", true));
    }
    ASSERT_FALSE(port.lines.empty());
    EXPECT_THAT(port.lines.back(), testing::HasSubstr("dual-role"));
}

TEST(SwitchUsbHostPowerRole, PreparedSuppliesReleaseOnFault)
{
    for (int phase : {EMPTY, CLASSIFY}) {
        Port port;
        port.enter(EMPTY, "start");
        if (phase == CLASSIFY)
            port.alert(R7W_SINK_ATTACH);
        port.events.clear();
        port.alert(R7W_SINK_ATTACH, SRC_FAULT);
        EXPECT_EQ(port.phase, HELD);
        EXPECT_FALSE(port.rails || port.boost || port.window);
        EXPECT_TRUE(port.switches);
        EXPECT_THAT(port.events, testing::Contains(Event("rails", false)));
        EXPECT_THAT(port.events, testing::Not(testing::Contains(Event("boost", true))));
    }
}

// SwitchUSB's data role: host for the partner we source and for a source that left us DFP (the dock after its data role
// swap, a hub with power in); never for a partner still being classified, which presents Rd while we are DFP by
// default (r7w's attach, before the Mac turned out to be dual-role), nor for the Mac or a charger, which leave us UFP.
TEST(SwitchUsbHostDataRole, HostForOurSinkOrASourceThatMadeUsDfp)
{
    EXPECT_FALSE(pd_usb_host(0, R7W_SINK_ATTACH.first)) << "classifying the keyboard's adapter";
    EXPECT_FALSE(pd_usb_host(0, R7W_SINK_ATTACH_CC2.first)) << "classifying the Mac";
    EXPECT_TRUE(pd_usb_host(1, REATTACH_RAILS_UP.first)) << "sourcing";
    EXPECT_FALSE(pd_usb_host(0, R7W_MAC_SOURCES.first)) << "the Mac is our host";
    EXPECT_FALSE(pd_usb_host(0, R7W_CHARGER_AT_BOOT.first)) << "a charger leaves us UFP";
    constexpr uint16_t kDockAfterSwap = SPDSNK | VSAFE | DFP | INSERT;
    EXPECT_TRUE(pd_usb_host(0, kDockAfterSwap));
    EXPECT_TRUE(pd_usb_host(0, kDockAfterSwap | CSIDE));
    EXPECT_FALSE(pd_usb_host(0, kDockAfterSwap & ~VSAFE)) << "no VBUS from it";
    EXPECT_FALSE(pd_usb_host(0, kDockAfterSwap & ~INSERT)) << "unplugged";
}

// The device controller's VBUS through a host session, its detach and the device role with nothing attached: off
// throughout, so the XUDC sees no VBUS and reports no connect; on for the Mac as our host and for a charger; off for
// the dock that made us DFP.
TEST(SwitchUsbHostDataRole, DeviceVbusOnlyFromAHostPort)
{
    constexpr uint16_t kNothing = 0;
    const std::vector<std::pair<int, uint16_t>> hostSessionThenNoPartner = {
        {0, R7W_SINK_ATTACH.first}, {1, REATTACH_RAILS_UP.first}, {1, kNothing}, {0, kNothing}};
    for (const auto &[sourcing, status1] : hostSessionThenNoPartner)
        EXPECT_FALSE(pd_usb_device_vbus(sourcing, status1)) << std::hex << status1;
    EXPECT_TRUE(pd_usb_device_vbus(0, R7W_MAC_SOURCES.first)) << "the Mac is our host";
    EXPECT_TRUE(pd_usb_device_vbus(0, R7W_CHARGER_AT_BOOT.first)) << "a charger's VBUS is real";
    constexpr uint16_t kDockAfterSwap = SPDSNK | VSAFE | DFP | INSERT;
    EXPECT_FALSE(pd_usb_device_vbus(0, kDockAfterSwap)) << "we are its host";
    EXPECT_FALSE(pd_usb_device_vbus(0, R7W_MAC_SOURCES.first & ~INSERT)) << "unplugged";
}

TEST(SwitchUsbHostPowerRole, ChargerAtBootAndNoHostMode)
{
    Port port;
    port.alert(R7W_CHARGER_AT_BOOT, 0);
    EXPECT_EQ(port.phase, SINK);
    Port off(false);
    off.alert(R7W_SINK_ATTACH);
    EXPECT_EQ(off.phase, SINK);
    ASSERT_FALSE(off.lines.empty());
    EXPECT_THAT(off.lines.back(), testing::HasSubstr("host mode off"));
}

TEST(SwitchUsbHostPowerRole, ClassifyWindows)
{
    const auto actNext = [](const Decision &d) {
        return std::make_pair(d.act, d.next);
    };
    // no detach: no proof
    EXPECT_EQ(actNext(windowEnd(CLASSIFY, false, R7W_SINK_ATTACH.first)), std::make_pair((int)ENTER, (int)SINK));
    // back already
    EXPECT_EQ(actNext(windowEnd(CLASSIFY, true, R7W_SINK_ATTACH.first)), std::make_pair((int)ENTER, (int)SINK));
    Decision d = windowEnd(CLASSIFY, true, 0);
    EXPECT_EQ(actNext(d), std::make_pair((int)ENTER, (int)REATTACH));
    EXPECT_THAT(d.why, testing::HasSubstr("boost"));
    d = windowEnd(REATTACH, false, 0);
    EXPECT_EQ(actNext(d), std::make_pair((int)ENTER, (int)EMPTY));
    EXPECT_THAT(d.why, testing::HasSubstr("boost off"));
    for (const int phase : {EMPTY, SINK, SOURCE, HELD})
        EXPECT_EQ(windowEnd(phase, true, 0).act, NONE) << phase;
}

TEST(SwitchUsbHostPowerRole, LeavingSource)
{
    const auto actNext = [](const Decision &d) {
        return std::make_pair(d.act, d.next);
    };
    EXPECT_EQ(decide(SOURCE, CONTRACT, R7W_SINK_ATTACH).act, NONE);
    EXPECT_EQ(actNext(decide(SOURCE, SRC_FAULT, R7W_SINK_ATTACH)), std::make_pair((int)END, (int)HELD));
    EXPECT_EQ(actNext(decide(SOURCE, 0, R7W_SINK_ATTACH.first | 1, 0x2000)), std::make_pair((int)END, (int)HELD));
    EXPECT_EQ(actNext(decide(SOURCE, PLUGPULL, R7W_MAC_SOURCES)), std::make_pair((int)END, (int)SINK));
    EXPECT_EQ(decide(SOURCE, 0, REATTACH_RAILS_UP).act, NONE);
    EXPECT_EQ(actNext(decide(HELD, PLUGPULL, 0, 0)), std::make_pair((int)ENTER, (int)EMPTY));
    Port port;
    port.enter(REATTACH, "proven");
    port.alert(REATTACH_RAILS_UP);
    port.events.clear();
    port.alert(REATTACH_RAILS_UP, CONTRACT);
    EXPECT_EQ(port.phase, SOURCE);
    EXPECT_TRUE(port.boost && port.rails && port.switches);
    EXPECT_TRUE(port.events.empty());
}

TEST(SwitchUsbHostPowerRole, FuzzedWalk)
{
    std::mt19937_64 random(1105);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    const auto below = [&](size_t n) {
        return static_cast<size_t>(std::uniform_int_distribution<uint64_t>(0, n - 1)(random));
    };
    const std::vector<Status> statuses = {{0, 0},
                                          R7W_SINK_ATTACH,
                                          R7W_SINK_ATTACH_CC2,
                                          R7W_DETACH_SOURCING,
                                          R7W_MAC_SOURCES,
                                          R7W_CHARGER_AT_BOOT,
                                          REATTACH_RAILS_UP,
                                          {SRC_MODE | VSAFE | DFP | INSERT, OTG_INSERT},
                                          {SRC_MODE | DFP | INSERT, 0},
                                          {INSERT | UFP, 0}};
    const std::vector<uint16_t> alerts = {0, PLUGPULL, PLUGPULL, CONTRACT, SRC_FAULT, PLUGPULL | CONTRACT};
    for (const bool allow : {true, false}) {
        for (const int boost : {0, -1, -2}) {
            SCOPED_TRACE(std::string(allow ? "host mode" : "no host mode") + ", boost " + std::to_string(boost));
            Port port(allow, boost);
            port.enter(EMPTY, "start");
            for (int step = 0; step < 4000; ++step) {
                const Status status = statuses[below(statuses.size())];
                const int before = port.phase;
                if (std::uniform_real_distribution<double>(0, 1)(random) < 0.25) {
                    port.windowEnds(status);
                } else {
                    port.alert(status, alerts[below(alerts.size())]);
                    if (port.phase == SOURCE && before != SOURCE) {
                        const auto [s1, s2] = status;
                        ASSERT_EQ(before, REATTACH) << "step " << step;
                        ASSERT_TRUE((s1 & INSERT) && (s1 & SRC_MODE) && (s2 & OTG_INSERT)) << "step " << step;
                        ASSERT_TRUE(!(s1 & VSAFE) || (s1 & (SPDSRC1 | SPDSRC2))) << "step " << step;
                    }
                    if (before == SOURCE && !(status.first & INSERT))
                        ASSERT_NE(port.phase, SOURCE) << "step " << step;
                }
                if (port.phase == SOURCE)
                    ASSERT_TRUE(port.rails && port.switches) << "step " << step;
            }
        }
    }
}

// --- Host role -------------------------------------------------------------------------------------------------------

struct Cap {
    uint32_t id, next, major, first, count;
};

class SwitchUsbHostRole : public testing::Test {
protected:
    void SetUp() override
    {
        fake_reset();
    }

    // entries laid out from xecp; an entry after one whose next is 0 still gets its own place (unreachable).
    static void caps(const std::vector<Cap> &entries, uint32_t xecp = 0x500)
    {
        fake_write(0x10, (xecp >> 2) << 16 | 0x1);  // HCCPARAMS1: xECP, AC64
        uint32_t offset = xecp;
        for (const Cap &c : entries) {
            fake_write(offset, c.major << 24 | c.next << 8 | c.id);
            fake_write(offset + 4, 0x20425355);  // "USB "
            fake_write(offset + 8, c.count << 8 | c.first);
            offset += (c.next ? c.next : 4) * 4;
        }
    }

    static std::pair<std::pair<uint32_t, uint32_t>, unsigned> find(unsigned major, uint32_t window = 0x2000)
    {
        unsigned reads = 0;
        const uint32_t value = find_ports(window, major, &reads);
        return {{value >> 16, value & 0xffff}, reads};
    }
};

using Range = std::pair<uint32_t, uint32_t>;

// T210 XUSB: legacy support, USB 3 ports 1-4, USB 2 ports 5-9 (pads 0-3, HSIC).
TEST_F(SwitchUsbHostRole, TegraLayout)
{
    caps({{1, 4, 0, 0, 0}, {2, 5, 3, 1, 4}, {2, 0, 2, 5, 5}});
    EXPECT_EQ(find(2).first, Range(5, 5));
    EXPECT_EQ(find(3).first, Range(1, 4));
    EXPECT_EQ(portsc_offset(0x20, 5), 0x20u + 0x400 + 0x40);
}

// HLC is bit 19 of the USB 2.0 capability's third dword, whichever capability comes first.
TEST_F(SwitchUsbHostRole, Usb2HardwareLpmCapability)
{
    caps({{1, 4, 0, 0, 0}, {2, 5, 3, 1, 4}, {2, 0, 2, 5, 5}});
    EXPECT_EQ(usb2_hlc(0x2000), 0);
    const uint32_t usb2 = 0x500 + 4 * 4 + 5 * 4;  // xECP, then after the legacy and USB 3 capabilities
    fake_write(usb2 + 8, 1u << 19 | 5u << 8 | 5u);
    EXPECT_EQ(usb2_hlc(0x2000), 1);
    EXPECT_EQ(find(2).first, Range(5, 5));
}

TEST_F(SwitchUsbHostRole, BrokenLists)
{
    caps({{1, 0, 0, 0, 0}, {2, 0, 2, 5, 5}});  // the list ends before the USB 2 capability
    EXPECT_EQ(find(2).first, Range(0, 0));
    caps({{2, 0x80, 3, 1, 4}}, 0x1f00);  // next points outside the window
    EXPECT_EQ(find(2, 0x2000).first, Range(0, 0));
    fake_reset();
    EXPECT_EQ(find(2).first, Range(0, 0));  // no xECP at all
    fake_reset();
    fake_write(0x10, (0x100u >> 2) << 16);
    for (uint32_t k = 0; k < 200; ++k)
        fake_write(0x100 + 4 * k, 1u << 8 | 0xc0u);  // a long chain of other capabilities:
    const auto [result, reads] = find(2);            // the walk stops after 64
    EXPECT_EQ(result, Range(0, 0));
    EXPECT_LE(reads, 1u + 64);
}

// The role line's words for a port (PORTSC CCS 0, PLS 8:5, PP 9, speed 13:10).
TEST(SwitchUsbHostPortWords, LinkAndSpeed)
{
    constexpr uint32_t kPp = 1u << 9;
    constexpr uint32_t kCcs = 1u;
    EXPECT_STREQ(portsc_link_name(kPp | kCcs | 4u << 10), "U0");
    EXPECT_STREQ(portsc_speed_name(kPp | kCcs | 4u << 10), "SuperSpeed 5 Gb/s");
    EXPECT_STREQ(portsc_speed_name(kPp | kCcs | 3u << 10), "high speed");
    EXPECT_STREQ(portsc_speed_name(kPp | 4u << 10), "nothing") << "no device connected";
    EXPECT_STREQ(portsc_link_name(kPp | 5u << 5), "RxDetect");
    EXPECT_STREQ(portsc_link_name(kPp | kCcs | 3u << 5), "U3");
    EXPECT_STREQ(portsc_link_name(4u << 5), "Disabled");
    EXPECT_STREQ(portsc_link_name(15u << 5), "Resume");
}

TEST_F(SwitchUsbHostRole, PortscPowerWrites)
{
    // PED, PR, LWS, the change bits, WPR
    const uint32_t write1 = (1u << 1) | (1u << 4) | (1u << 16) | (0x7fu << 17) | (1u << 31);
    std::mt19937 random(9);  // NOLINT(bugprone-random-generator-seed): a fixed seed, so a failure reproduces
    for (int i = 0; i < 2000; ++i) {
        const uint32_t v = random();
        for (const int on : {0, 1}) {
            const uint32_t out = portsc_power(v, on);
            ASSERT_EQ(out & write1, 0u) << std::hex << v;
            ASSERT_EQ((out & (1u << 9)) != 0, on != 0) << std::hex << v;
            ASSERT_EQ(out & ~(1u << 9), v & portsc_keep() & ~(1u << 9)) << std::hex << v;
        }
    }
    // A powered SuperSpeed port in RxDetect with a connect change pending, as after reset: the change bit is not cleared.
    EXPECT_EQ(portsc_power(0x000202a0, 0), 0x000000a0u);
}

TEST_F(SwitchUsbHostRole, VbusId)
{
    const uint32_t xudcOnBus =
        (1u << 12) | (1u << 14) | (1u << 16) | (8u << 18);  // XudcHardware connect(): VBUS on, ID
    EXPECT_EQ(vbus_id_drop_vbus(xudcOnBus), (1u << 12) | (1u << 16) | (8u << 18));  // floating
    const uint32_t host = vbus_id_host(xudcOnBus);
    EXPECT_EQ(host, (1u << 12) | (1u << 16));
    EXPECT_STREQ(id_name(host), "grounded");
    const uint32_t device = vbus_id_device(host);
    EXPECT_EQ(device, (1u << 12) | (1u << 16) | (8u << 18));
    EXPECT_STREQ(id_name(device), "floating");
    EXPECT_EQ(vbus_id_host(0xffffffff) & (0xfu << 18), 0u);
    EXPECT_EQ(vbus_id_host(0xffffffff) & (1u << 14), 0u);
}

TEST_F(SwitchUsbHostRole, ChargerDetectAndOc)
{
    // utmi_pad_charger_detect_on leaves: pull overrides enabled with value 0, PD_CHG clear, sources off.
    const uint32_t detectOnCtl1 = (1u << 16) | (1u << 18) | (1u << 20) | (1u << 22) | 0x80;
    EXPECT_EQ(chrg_ctl1_host(detectOnCtl1), 0x80u);
    EXPECT_EQ(chrg_ctl0_host(0xffffffff), 0xffffffffu & ~(0x1fu << 9));  // OP_I_SRC_EN too (Tegra X1 TRM 22.8.11)
    EXPECT_EQ(chrg_ctl1_host(1u << 4), 0u);                              // DIV_DET_EN
    EXPECT_EQ(chrg_ctl0_host(1), 1u);                                    // PD_CHG as found
    EXPECT_EQ(oc_map_host(0xfff0), 0xffffu);
    EXPECT_EQ(oc_map_host(0xffff), 0xffffu);
}

}  // namespace

// bm92t_system_reset_auto(false): only a plugged controller, and only one left as DFP, in DisplayPort mode or after a
// command that did not complete.
TEST(SwitchPdPolicy, ResetAtStartAsBm92txx)
{
    constexpr unsigned kInsert = 1u << 7;
    constexpr unsigned kDfp = 2u << 8;
    constexpr unsigned kUfp = 1u << 8;
    EXPECT_FALSE(pd_reset_at_start(kDfp, 1u << 15));
    EXPECT_FALSE(pd_reset_at_start(kInsert | kUfp, 0));
    EXPECT_TRUE(pd_reset_at_start(kInsert | kDfp, 0));
    EXPECT_TRUE(pd_reset_at_start(kInsert | kUfp, 1u << 15));
    EXPECT_TRUE(pd_reset_at_start(kInsert | kUfp | 2u << 4, 0));  // last command aborted
}

// bm92t_event_handler: an internal fault resets when plugged, on a sink fault or with no alert; a source fault without
// an internal one gets a hard reset instead.
TEST(SwitchPdPolicy, FaultResetsAsBm92txx)
{
    constexpr uint16_t kInsert = 1u << 7;
    constexpr uint16_t kFault = 1u;
    constexpr uint16_t kSnkFault = 1u;
    constexpr uint16_t kSrcFault = 2u;
    constexpr uint16_t kPlugPull = 8u;
    EXPECT_TRUE(pd_reset_on_fault(kPlugPull, kInsert | kFault));
    EXPECT_TRUE(pd_reset_on_fault(kSnkFault, kFault));
    EXPECT_TRUE(pd_reset_on_fault(0, kFault));
    EXPECT_FALSE(pd_reset_on_fault(kPlugPull, kFault));
    EXPECT_FALSE(pd_reset_on_fault(kPlugPull, kInsert));
    EXPECT_TRUE(pd_hard_reset(kSrcFault, kInsert));
    EXPECT_FALSE(pd_hard_reset(kSrcFault, kInsert | kFault));
    EXPECT_FALSE(pd_hard_reset(kPlugPull, kInsert));
}
