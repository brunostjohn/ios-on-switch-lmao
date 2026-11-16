// Host test of the charging path: the real drivers/SwitchBattery/SwitchBattery.cpp and drivers/SwitchUSB/SwitchPD.cpp,
// each its own translation unit over switch_charging_stubs.h (BUILD.bazel), behind register-level models of the three
// I2C1 chips; plus the pure policy (drivers/SwitchBattery/SwitchChargePolicy.h) checked directly. One scenario per
// process (the drivers keep globals): each TEST runs its scenario in a fresh process; Policy runs the policy checks.
//   BQ24193: TI SLUSBG7A reset values (REG00 30, 01 1b, 02 60, 03 11, 04 b2, 05 9a, 06 03, 07 4b, 0a 2f), default mode
//            after reset (REG09 bit 7), host mode from the first write, the I2C watchdog (REG05, kicked by REG01 bit
//            6) resetting every register when it expires, input current detection on plug-in (8.3.1.3), REG09's
//            latched-then-current double read, read-only REG08-0A, multi-read only over REG00-REG08, an INT pulse per
//            event. Every write is checked against the limits: ICHG <= 2048 mA, VREG <= 4208 mV, an IINLIM the driver
//            raises <= 2000 mA, and charge mode only at 4 to 51 C battery temperature.
//   MAX17050: words, ALRT low while CONFIG.Aen and a STATUS alert flag (TALRT_Th/SALRT_Th windows) is set.
//   BM92T36: ALERT low while ALERT_STATUS (clear on read) is not 0; STATUS1/2, CONFIG1, READ_PDOS_SRC, CURRENT_PDO/RDO,
//            SET_RDO + SEND_RDO (accepted when the RDO is valid for the PDO, or refused, or never answered as the
//            scenario says) and PS_RDY; for the alternate-mode engine DP_STATUS, a data role swap (accepted: DFP and
//            CMD_DONE) and VDMs a partner never answers.
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <functional>
#include <typeinfo>
#include <gtest/gtest.h>
#include "bazel/testing/fresh_process.h"
#include "../SwitchBattery.h"
#include "../SwitchBatteryPack.h"
#include "../SwitchChargePolicy.h"
#include "../../SwitchDisplayPort/SwitchDisplayPort.h"
#include "../../SwitchSuspend/SwitchWakeSources.h"
#include "../../SwitchUSB/SwitchPD.h"
#include "../../SwitchUSB/XusbLanes.h"
#include "../../TegraGPIO/TegraGPIO.h"
#include "../../TegraI2C/TegraI2C.h"

extern "C" int switch_battery_start(void);
extern "C" int switch_battery_temperature(int *milli_c, unsigned long long *at);
static unsigned gSensorNotes = 0;

extern "C" void switch_sensors_note(void)
{
    ++gSensorNotes;
}  // SwitchSensors (SwitchPower.h)

static std::vector<unsigned> gWakeEvents;

// AppleKeyStore's per-console digest (AKSDeviceKey.cpp): a fixed one, so the serial is checked against a known value.
extern "C" bool native_aks_device_digest(const char *label, uint8_t out[32])
{
    for (unsigned i = 0; i < 32; ++i)
        out[i] = static_cast<uint8_t>(i * 37 + strlen(label));
    return true;
}

extern "C" void switch_wake_event(unsigned source)
{
    gWakeEvents.push_back(source);
}  // SwitchWake.h

static switch_usb3_lanes gAltLanes = SWITCH_USB3_LANES_FREE;
static bool gAltLanesTaken = false;
static bool gDisplayPortConfigured = false;

void switch_usb_set_alt_lanes(enum switch_usb3_lanes lanes, bool)
{
    gAltLanes = lanes;
    gAltLanesTaken |= lanes == SWITCH_USB3_LANES_TAKEN;
}  // SwitchUSB.cpp (XusbLanes.h)

extern "C" void switch_displayport_alt_mode(unsigned lanes, bool)
{
    gDisplayPortConfigured |= lanes != 0;
}

// Every check is a GoogleTest failure at its call site; the count lets a failed scenario dump the logs it ran on.
static unsigned gFailures = 0;

static void checkAt(bool ok, const std::string &what, const char *file, int line)
{
    if (ok)
        return;
    ++gFailures;
    ADD_FAILURE_AT(file, line) << what;
}

#define check(ok, what) checkAt((ok), (what), __FILE__, __LINE__)

static std::string hex(unsigned v)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%02x", v);
    return text;
}

static bool logHas(const std::string &needle, size_t from = 0)
{
    for (size_t i = from; i < gLog.size(); ++i)
        if (gLog[i].find(needle) != std::string::npos)
            return true;
    return false;
}

static size_t logCount(const std::string &needle)
{
    size_t n = 0;
    for (const auto &line : gLog)
        n += line.find(needle) != std::string::npos;
    return n;
}

struct Gauge {
    std::map<uint8_t, uint16_t> reg;
    bool line = true, edge = false;  // ALRT (high = released), a falling edge not yet delivered

    Gauge()
    {
        reg[0x21] = 0x00ac;    // DevName
        reg[0x00] = 0x0002;    // Status: POR
        reg[0x1d] = 0x2210;    // Config
        reg[0x01] = 0xff00;    // VAlrtTh
        reg[0x02] = 0x7f80;    // TAlrtTh
        reg[0x03] = 0xff00;    // SAlrtTh
        reg[0x05] = 2586 * 2;  // RepCap 2586 mAh
        reg[0x06] = 60 << 8;   // RepSOC 60 %
        reg[0x09] = (uint16_t)((3900 * 1000 / 625) << 3);
        reg[0x0a] = (uint16_t)(1800 * 100000 / 15625);  // Current 1800 mA
        reg[0x0b] = reg[0x0a];
        reg[0x10] = 4310 * 2;
        reg[0x17] = 400;
        reg[0x18] = 4310 * 2;
        setTemp(3000);
    }

    void setTemp(int centi)
    {
        reg[0x08] = (uint16_t)(int16_t)(centi * 256 / 100);
    }

    int centi() const
    {
        return (int16_t)reg.at(0x08) * 100 / 256;
    }

    void update()
    {
        const int t = (int16_t)reg[0x08];  // 1/256 C
        const int tmax = (int8_t)(reg[0x02] >> 8), tmin = (int8_t)(reg[0x02] & 0xff);
        const int soc = reg[0x06] >> 8, smax = reg[0x03] >> 8, smin = reg[0x03] & 0xff;
        if (t > tmax * 256)
            reg[0x00] |= 1u << 13;
        if (t < tmin * 256)
            reg[0x00] |= 1u << 9;
        if (soc > smax)
            reg[0x00] |= 1u << 14;
        if (soc < smin)
            reg[0x00] |= 1u << 10;
        const bool low = (reg[0x1d] & (1u << 2)) && (reg[0x00] & 0x7700);
        if (line && low)
            edge = true;
        line = !low;
    }
};

struct Charger {
    uint8_t reg[11] = {};
    bool defaultMode = true, input = false, intPending = false;
    uint8_t latched = 0;
    uint64_t lastKick = 0;
    unsigned detectMA = 500, vbusStat = 2, writes = 0, iinlimRaises = 0;
    bool refuseReg05 = false;
    const Gauge *gauge = nullptr;
    std::vector<std::string> ops;

    Charger()
    {
        reset();
    }

    uint8_t current() const
    {
        return defaultMode ? 0x80 : 0;
    }

    void reset()
    {
        const uint8_t r[11] = {0x30, 0x1b, 0x60, 0x11, 0xb2, 0x9a, 0x03, 0x4b, 0x00, 0x00, 0x2f};
        std::memcpy(reg, r, sizeof(reg));
        defaultMode = true;
        latched |= 0x80;
        status();
        if (input)
            detect();
    }

    void detect()
    {
        reg[0] = (uint8_t)((reg[0] & ~7) | charge::bq::encodeInputLimit(detectMA));
    }

    void status()
    {
        const unsigned mode = (reg[1] >> 4) & 3;
        if (mode >= 2) {
            reg[8] = (uint8_t)(3u << 6 | 0x04);
        } else if (input) {
            reg[8] = (uint8_t)(vbusStat << 6 | (mode == 1 ? 0x20 : 0) | 0x04);
        } else {
            reg[8] = 0;
        }
    }

    void plug(unsigned ma, unsigned stat)
    {
        input = true;
        detectMA = ma;
        vbusStat = stat;
        if (!(reg[0] & 0x80) || defaultMode)
            detect();  // [TI] 8.3.1.3: unless in Hi-Z in host mode
        status();
        intPending = true;
    }

    void unplug()
    {
        input = false;
        status();
        intPending = true;
    }

    void tick()
    {
        const unsigned wd = charge::bq::watchdogSeconds(reg[5]);
        if (!defaultMode && wd && gNow - lastKick >= wd * 1000000000ull) {
            ops.push_back("watchdog expired");
            reset();
            intPending = true;
        }
    }

    int read(uint8_t r, uint8_t *buf, size_t len)
    {
        if (len > 1 && r + len > 9)
            return TEGRA_I2C_EIO;  // multi-read only over REG00-REG08
        if (len == 1 && r == 9) {
            buf[0] = latched;
            latched = current();
            return 0;
        }
        if (r + len > 11)
            return TEGRA_I2C_EINVAL;
        std::memcpy(buf, reg + r, len);
        return 0;
    }

    int write(uint8_t r, uint8_t v)
    {
        ++writes;
        ops.push_back("W " + hex(r) + " " + hex(v));
        if (r >= 8) {
            check(false, "write to read-only BQ24193 REG" + hex(r));
            return TEGRA_I2C_EINVAL;
        }
        if (defaultMode) {
            defaultMode = false;
            lastKick = gNow;
        }
        if (r == 1 && (v & 0x80)) {
            reset();
            return 0;
        }
        if (r == 1 && (v & 0x40))
            lastKick = gNow;
        if (r == 5 && refuseReg05)
            return 0;
        if (r == 0 && (v & 7) > (reg[0] & 7)) {
            ++iinlimRaises;
            check(charge::bq::decodeInputLimit(v) <= 2000,
                  "IINLIM raised to " + std::to_string(charge::bq::decodeInputLimit(v)));
        }
        reg[r] = r == 1 ? (uint8_t)(v & 0x3f) : v;
        if (r == 2)
            check(charge::bq::decodeChargeCurrent(v) <= 2048, "ICHG " + hex(v));
        if (r == 4)
            check(charge::bq::decodeChargeVoltage(v) <= 4208, "VREG " + hex(v));
        if (r == 1 && ((v >> 4) & 3) == 1 && gauge) {
            const int t = gauge->centi();
            check(t >= 400 && t < 5100, "charge mode at " + std::to_string(t) + " centi-C");
        }
        status();
        return 0;
    }
};

static std::map<unsigned, int> gOutputs;

struct PdChip {
    uint16_t alert = 0, status1 = 0, status2 = 0, config1 = 0xc0e0, vendor = 0;
    uint16_t config2 = 0;                         // DR_SWAP field 3:2 reject in both roles
    uint8_t alertEnable[4] = {0xff, 0xfb, 0, 0};  // every ALERT but bit 10 (DR_SWAP)
    uint32_t idHeader = 0;                        // the ID Header VDO it ACKs Discover Identity with; 0: never answers
    std::vector<uint8_t> vdmReply, incomingVdm;
    std::vector<uint32_t> pdos;
    uint32_t curPdo = 0, curRdo = 0, pendingRdo = 0;

    enum Reply {
        Accept,
        Refuse,
        Silent,
        AbortOnce
    } rdoReply = Accept;

    bool psRdyReply = true;
    std::vector<std::string> commands;
    unsigned vdmsSent = 0;
    unsigned unpoweredSourceArms = 0;

    static uint32_t fixed(unsigned mv, unsigned ma)
    {
        return (uint32_t)(mv / 50) << 10 | (ma / 10);
    }

    void sink(const std::vector<uint32_t> &source, unsigned autoIndex, uint16_t bits)
    {
        pdos = source;
        status1 = 0x4580;  // SPDSNK, VSAFE, UFP, INSERT
        status2 = 0x1;
        if (!pdos.empty()) {
            curPdo = pdos[autoIndex];
            curRdo = (uint32_t)(autoIndex + 1) << 28 | (pdos[autoIndex] & 0x3ff) << 10 | (pdos[autoIndex] & 0x3ff);
        } else {
            curPdo = curRdo = 0;
        }
        alert |= bits;
    }

    void unplug()
    {
        status1 = status2 = 0;
        pdos.clear();
        curPdo = curRdo = 0;
        alert |= 0x8;
    }

    int read(uint8_t r, uint8_t *buf, size_t len)
    {
        std::memset(buf, 0, len);
        auto put16 = [&](uint16_t v) {
            buf[0] = (uint8_t)v;
            if (len > 1)
                buf[1] = (uint8_t)(v >> 8);
        };
        auto put32 = [&](uint32_t v) {
            if (len < 5)
                return;
            buf[0] = 4;
            for (int i = 0; i < 4; ++i)
                buf[1 + i] = (uint8_t)(v >> (8 * i));
        };
        switch (r) {
        case 0x02:
            put16(alert);
            alert = 0;
            break;
        case 0x03:
            put16(status1);
            break;
        case 0x04:
            put16(status2);
            break;
        case 0x06:
            put16(config1);
            break;
        case 0x1a:
            put16(vendor);
            break;
        case 0x17:
            put16(config2);
            break;
        case 0x2e:
            check(len == 4, "ALERT_ENABLE length " + std::to_string(len));
            std::memcpy(buf, alertEnable, 4);
            break;
        case 0x50:
            std::memcpy(buf, incomingVdm.data(), std::min(len, incomingVdm.size()));
            break;
        case 0x4b:
            put16(0x603);
            break;
        case 0x4c:
            put16(0xd7c);
            break;
        case 0x28:
            put32(curPdo);
            break;
        case 0x2b:
            put32(curRdo);
            break;
        case 0x18:  // DP_STATUS: no DisplayPort configured
            put16(0);
            break;
        case 0x08:
            check(len == 29, "READ_PDOS_SRC length " + std::to_string(len));
            buf[0] = (uint8_t)(pdos.size() * 4);
            for (size_t i = 0; i < pdos.size() && 1 + 4 * i + 3 < len; ++i)
                for (int b = 0; b < 4; ++b)
                    buf[1 + 4 * i + b] = (uint8_t)(pdos[i] >> (8 * b));
            break;
        default:
            check(false, "BM92T36 read of " + hex(r));
            return TEGRA_I2C_ENACK;
        }
        return 0;
    }

    int write(uint8_t r, const uint8_t *data, size_t len)
    {
        const uint16_t v16 = len >= 2 ? (uint16_t)(data[0] | data[1] << 8) : 0;
        switch (r) {
        case 0x06:
            if ((config1 >> 14) != 0 && (v16 >> 14) == 0 &&
                (!gOutputs[TEGRA_GPIO(X, 3)] || !gOutputs[TEGRA_GPIO(K, 5)]))
                ++unpoweredSourceArms;
            config1 = v16;
            break;
        case 0x1a:
            vendor = v16;
            break;
        case 0x17:
            config2 = v16;
            break;
        case 0x2e:
            check(len == 4, "ALERT_ENABLE write length " + std::to_string(len));
            std::memcpy(alertEnable, data, 4);
            break;
        case 0x60:  // OUTGOING_VDM: count, then the message
            check(len >= 5 && data[0] == len - 1, "OUTGOING_VDM layout");
            break;
        case 0x30:
            check(len == 5 && data[0] == 4, "SET_RDO layout");
            pendingRdo = (uint32_t)data[1] | (uint32_t)data[2] << 8 | (uint32_t)data[3] << 16 | (uint32_t)data[4] << 24;
            break;
        case 0x05:
            commands.push_back(hex(v16 >> 8) + hex(v16 & 0xff));
            if (v16 == 0x0707) {
                const unsigned pos = charge::rdoPosition(pendingRdo);
                const bool valid = pos >= 1 && pos <= pdos.size() && charge::pdoType(pdos[pos - 1]) == 0 &&
                                   charge::rdoOperatingMilliamps(pendingRdo) <= charge::pdoMilliamps(pdos[pos - 1]) &&
                                   charge::rdoMaxMilliamps(pendingRdo) <= charge::pdoMilliamps(pdos[pos - 1]) &&
                                   !(pendingRdo & charge::kRdoMismatch);
                check(valid, "RDO " + hex(pendingRdo) + " valid for the source");
                if (rdoReply == Silent)
                    break;
                if (rdoReply == AbortOnce) {  // a MacBook's port: the first request at attach is aborted
                    rdoReply = Accept;
                    status1 = (uint16_t)((status1 & ~0x70) | 2u << 4);
                    alert |= 0x4;
                    break;
                }
                status1 = (uint16_t)((status1 & ~0x70) | (rdoReply == Accept && valid ? 0 : 6u << 4));
                if (rdoReply == Accept && valid) {
                    curRdo = pendingRdo;
                    curPdo = pdos[pos - 1];
                    alert |= 0x1000 | 0x4;
                } else {
                    alert |= 0x4;
                }
            } else if (v16 == 0x0505) {
                if (psRdyReply)
                    alert |= 0x4;
            } else if (v16 == 0x1818) {  // DR_SWAP, accepted: we are DFP
                status1 = (uint16_t)((status1 & ~(0x70 | 3u << 8)) | 2u << 8);
                alert |= 0x4;
            } else if (v16 == 0x1111) {  // SEND_VDM: Discover Identity, ACKed with idHeader or never answered
                ++vdmsSent;
                if (idHeader) {
                    vdmReply = {20, 0x41, 0x80, 0x00, 0xFF};
                    for (const uint32_t object : {idHeader, 0u, 0x12340001u, 0x01000000u})
                        for (int b = 0; b < 4; ++b)
                            vdmReply.push_back((uint8_t)(object >> (8 * b)));
                    alert |= 0x4 | 0x800;
                }
            } else if (v16 == 0x1616) {  // ACCEPT_VDM: the reply readable in INCOMING_VDM
                incomingVdm = vdmReply;
                alert |= 0x4;
            } else if (v16 == 0x0D0D) {  // SYS_RESET: faults and the last command cleared, OCP and CONFIG2 to defaults
                status1 = (uint16_t)(status1 & ~(0x3 | 0x70));
                vendor |= 0x4;
                config2 = 0;
            } else if (v16 == 0x0808) {  // PD_HARD_RST
                alert |= 0x4;
            } else {
                check(false, "unexpected BM92T36 command " + hex(v16));
            }
            break;
        default:
            check(false, "BM92T36 write of " + hex(r));
            return TEGRA_I2C_ENACK;
        }
        return 0;
    }
};

static Gauge gGauge;
static Charger gCharger;
static PdChip gPd;
static std::map<unsigned, IOService *> gNubs;

extern "C" int tegra_i2c_init(unsigned)
{
    return 0;
}

extern "C" int tegra_i2c_transfer(unsigned, uint8_t, const void *, size_t, void *, size_t)
{
    return TEGRA_I2C_EINVAL;
}

extern "C" int tegra_i2c_read(unsigned bus, uint8_t addr, uint8_t reg, void *buf, size_t len)
{
    check(bus == 1, "I2C bus " + std::to_string(bus));
    auto *b = static_cast<uint8_t *>(buf);
    if (addr == 0x6b)
        return gCharger.read(reg, b, len);
    if (addr == 0x18)
        return gPd.read(reg, b, len);
    if (addr == 0x36) {
        check(len == 2, "gauge read length");
        const uint16_t v = gGauge.reg.count(reg) ? gGauge.reg[reg] : 0;
        b[0] = (uint8_t)v;
        b[1] = (uint8_t)(v >> 8);
        return 0;
    }
    return TEGRA_I2C_ENACK;
}

extern "C" int tegra_i2c_write(unsigned bus, uint8_t addr, uint8_t reg, const void *buf, size_t len)
{
    check(bus == 1, "I2C bus " + std::to_string(bus));
    auto *b = static_cast<const uint8_t *>(buf);
    if (addr == 0x6b) {
        check(len == 1, "charger write length");
        return gCharger.write(reg, b[0]);
    }
    if (addr == 0x18)
        return gPd.write(reg, b, len);
    if (addr == 0x36) {
        check(len == 2, "gauge write length");
        uint16_t v = (uint16_t)(b[0] | b[1] << 8);
        if (reg == 0x00)
            v = (uint16_t)(gGauge.reg[0] & v);  // alert flags clear by writing 0
        gGauge.reg[reg] = v;
        gGauge.update();
        return 0;
    }
    return TEGRA_I2C_ENACK;
}

extern "C" IOService *tegra_gpio_interrupt_nub(unsigned gpio, unsigned)
{
    if (!gNubs[gpio])
        gNubs[gpio] = new IOService;
    return gNubs[gpio];
}

extern "C" void tegra_gpio_describe(unsigned gpio, char *buffer, size_t size)
{
    std::snprintf(buffer, size, "GPIO %u", gpio);
}

extern "C" int tegra_gpio_config(unsigned gpio)  // SwitchPD's report: an output at its last value
{
    return 1 | 2 | (gOutputs[gpio] ? 4 : 0);
}

extern "C" int tegra_gpio_output(unsigned gpio, int value)
{
    if (value && (gpio == TEGRA_GPIO(X, 3) || gpio == TEGRA_GPIO(K, 5)))
        check((gPd.config1 >> 14) == 3u, "source supply raised before the connector switches opened");
    gOutputs[gpio] = value;
    return 0;
}

static IOInterruptEventSource *sourceFor(unsigned gpio)
{
    for (auto *s : IOInterruptEventSource::all)
        if (s->provider == gNubs[gpio])
            return s;
    return nullptr;
}

static void deliver()
{
    for (int round = 0; round < 200; ++round) {
        gGauge.update();
        bool any = false;
        if (gPd.alert) {
            if (auto *s = sourceFor(TEGRA_GPIO_PK4); s && s->enabled) {
                any = true;
                s->interruptOccurred();
            }
        }
        if (gCharger.intPending) {
            gCharger.intPending = false;
            if (auto *s = sourceFor(TEGRA_GPIO_PZ0)) {
                any = true;
                s->interruptOccurred();
            }
        }
        if (gGauge.edge) {
            gGauge.edge = false;
            if (auto *s = sourceFor(TEGRA_GPIO_PY0)) {
                any = true;
                s->interruptOccurred();
            }
        }
        if (!any)
            return;
    }
    check(false, "interrupts never settle");
}

static void advance(uint64_t ms)
{
    const uint64_t target = gNow + ms * 1000000ull;
    for (;;) {
        deliver();
        IOTimerEventSource *next = nullptr;
        for (auto *t : IOTimerEventSource::all)
            if (t->armed && t->deadline <= target && (!next || t->deadline < next->deadline))
                next = t;
        if (!next)
            break;
        if (next->deadline > gNow)
            gNow = next->deadline;
        gCharger.tick();
        next->fire();
    }
    if (target > gNow)
        gNow = target;
    gCharger.tick();
    deliver();
}

static IOPMPowerSource *battery()
{
    for (auto *s : IOInterruptEventSource::all)
        if (auto *ps = dynamic_cast<IOPMPowerSource *>(s->owner))
            return ps;
    return nullptr;
}

const uint32_t kNintendo5V = PdChip::fixed(5000, 1500), kNintendo15V = PdChip::fixed(15000, 2600);

static void bootWithNintendoCharger(unsigned detectMA = 500)
{
    gCharger.plug(detectMA, 2);
    gCharger.intPending = false;                  // before boot
    gPd.sink({kNintendo5V, kNintendo15V}, 0, 0);  // the controller's own contract: 5 V
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(10);
}

static unsigned iinlim()
{
    return charge::bq::decodeInputLimit(gCharger.reg[0]);
}

static void nintendoBoot()
{
    bootWithNintendoCharger();
    check(logHas("BQ24193 as found: REG00-09 32 1b 60 11 b2 9a 03 4b a4 80/80: input limit 500 mA") &&
              logHas("default mode"),
          "as-found line");
    check(logHas("charger set: VINDPM 4360 mV, ICHG 2048 mA, VREG 4208 mV, pre-charge/termination 128/128 mA, "
                 "watchdog off, safety timer off, battery 30.00 C (normal)"),
          "setup line");
    check((gCharger.reg[5] & 0x38) == 0 && (gCharger.reg[5] & 0x80), "REG05 watchdog/timer off, EN_TERM kept");
    check(gCharger.reg[2] == 0x60 && gCharger.reg[3] == 0x00 && (gCharger.reg[4] & 0xfc) == 0xb0 &&
              gCharger.reg[6] == 0x03 && (gCharger.reg[0] & 0x78) == 0x30,
          "REG02-06 and VINDPM");
    check((gCharger.reg[1] & 0x3e) == 0x10, "REG01 charge, SYS_MIN 3000 mV: " + hex(gCharger.reg[1]));
    check(gPd.commands.size() == 2 && gPd.commands[0] == "0707" && gPd.commands[1] == "0505", "SEND_RDO, PS_RDY");
    check(gPd.curRdo == (2u << 28 | 3u << 24 | 120u << 10 | 260u),
          "RDO: PDO 2, 1.2 A op, 2.6 A max, USB comms, no suspend");
    check(!logHas("SwitchPD: contract"), "not published before 2 s");
    check(iinlim() == 500, "input limit as detected until the contract is published");
    const size_t mark = gLog.size();
    const uint64_t slept = gSleptMS, delayed = gDelayedUS;
    advance(2300);
    check(logHas("IOLog: SwitchPD: contract 15 V 1.2 A (5 V 1.5 A, *15 V 2.6 A)", mark), "contract line");
    check(iinlim() == 1200, "input limit 1200 mA: " + std::to_string(iinlim()));
    check(gSleptMS - slept >= 1 && gDelayedUS == delayed, "raised in steps 1 ms apart, asleep between them");
    check(
        logHas("IOLog: SwitchBattery: input limit 1200 mA (was 500 mA): USB-PD contract 15000 mV; read back REG00 0x34 "
               "(IINLIM 1200 mA, VINDPM 4360 mV) REG02 0x60 (ICHG 2048 mA)",
               mark),
        "input limit line with the read-back");
    check(logHas("os_log: SwitchBattery: stats: in USB-PD 15000 mV, limit 1200 mA | charge 1800 mA", mark), "stats");
    IOPMPowerSource *ps = battery();
    check(ps && ps->adapter.numbers["FamilyCode"] == kIOPSFamilyCodeUSBCPD &&
              ps->adapter.numbers["AdapterVoltage"] == 15000 && ps->adapter.numbers["Current"] == 1200 &&
              ps->adapter.numbers["Watts"] == 18,
          "adapter details 15 V 1.2 A 18 W");
    {
        // 17 characters of Apple's serial alphabet from the digest's first 17 bytes, digit or letter but I and O.
        static const char kAlphabet[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
        std::string expected;
        for (unsigned i = 0; i < 17; ++i)
            expected += kAlphabet[static_cast<uint8_t>(i * 37 + strlen("Switch battery serial")) % 34];
        check(ps && ps->strings[kIOPMPSSerialKey] == expected, "battery serial " + (ps ? ps->strings["Serial"] : ""));
    }
    check(gGauge.reg[0x02] == (31u << 8 | 29u),
          "temperature window [29, 31] (1 C around, within [17, 51]): " + hex(gGauge.reg[0x02]));
    int milliC = 0;
    unsigned long long at = 0;
    check(switch_battery_temperature(&milliC, &at) == 1 && milliC == 30000 && at && gSensorNotes == 1,
          "temperature for thermalmonitord: " + std::to_string(milliC));
    // No watchdog: the settings stay for hours; a stats line every 4 minutes.
    const size_t before = logCount("SwitchBattery: stats:");
    advance(3600 * 1000);
    check(iinlim() == 1200 && (gCharger.reg[5] & 0x30) == 0, "still set after an hour");
    check(std::count(gCharger.ops.begin(), gCharger.ops.end(), std::string("watchdog expired")) == 0, "no expiry");
    const size_t lines = logCount("SwitchBattery: stats:") - before;
    check(lines >= 14 && lines <= 16, "stats every 4 minutes: " + std::to_string(lines));
    check(!logHas("settings lost"), "no lost settings");
}

static void jeita()
{
    bootWithNintendoCharger();
    advance(2300);
    auto setTemp = [](int centi) {
        gGauge.setTemp(centi);
        advance(300);
    };
    setTemp(5200);
    check(logHas("battery 52.00 C: normal -> hot, charging off"), "hot at 52 C");
    check(((gCharger.reg[1] >> 4) & 3) == 0, "charge off when hot");
    check(gGauge.reg[0x02] == (53u << 8 | 51u), "hot: [51, 53] within [50, 127]: " + hex(gGauge.reg[0x02]));
    setTemp(5050);
    advance(120000);
    check(((gCharger.reg[1] >> 4) & 3) == 0, "still off at 50.5 C (1 C margin)");
    setTemp(4950);
    check(logHas("battery 49.50 C: hot -> normal, charging"), "normal again below 50 C");
    check(((gCharger.reg[1] >> 4) & 3) == 1 && gCharger.reg[2] == 0x60, "charging 2048 mA");
    setTemp(1650);
    check(logHas("normal -> cool, charging (ICHG 768 mA"), "cool below 17 C");
    check(gCharger.reg[2] == 0x10, "ICHG 768 mA: " + hex(gCharger.reg[2]));
    check(gGauge.reg[0x02] == (18u << 8 | 16u), "cool: [16, 18] within [4, 18]: " + hex(gGauge.reg[0x02]));
    setTemp(1750);
    advance(120000);
    check(gCharger.reg[2] == 0x10, "still cool at 17.5 C");
    setTemp(1850);
    check(gCharger.reg[2] == 0x60, "normal at 18.5 C");
    setTemp(350);
    check(((gCharger.reg[1] >> 4) & 3) == 0 && logHas("-> cold, charging off"), "cold below 4 C");
    setTemp(450);
    advance(120000);
    check(((gCharger.reg[1] >> 4) & 3) == 0, "still off at 4.5 C");
    setTemp(-1000);
    check(gGauge.reg[0x02] == (0xf7u << 8 | 0xf5u), "cold: [-11, -9] within [-128, 5]: " + hex(gGauge.reg[0x02]));
    setTemp(550);
    check(((gCharger.reg[1] >> 4) & 3) == 1 && gCharger.reg[2] == 0x10, "cool again at 5.5 C");
    // The OTG boost and the zone: boost off goes back to the zone's mode, not to charge.
    setTemp(5500);
    check(((gCharger.reg[1] >> 4) & 3) == 0, "hot");
    gCharger.unplug();
    gPd.unplug();
    advance(300);
    check(switch_battery_set_otg_boost(true, false) == 0 && ((gCharger.reg[1] >> 4) & 3) == 2 && (gCharger.reg[1] & 1),
          "boost on: OTG, 1.3 A");
    check(switch_battery_set_otg_boost(false, false) == 0 && ((gCharger.reg[1] >> 4) & 3) == 0,
          "boost off while hot: off");
}

static void hotplug()
{
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(10);
    check(logHas("charger set:"), "managed with no cable");
    // Nintendo's charger: the controller's own 5 V contract first.
    gCharger.plug(500, 2);
    gPd.sink({kNintendo5V, kNintendo15V}, 0, 0x8 | 0x1000 | 0x4000);
    advance(3000);
    check(iinlim() == 1200 && logHas("SwitchPD: contract 15 V 1.2 A"), "hot-plugged charger: 1200 mA");
    // Unplug, then a plain (non-PD) Type-C charger the BQ24193 detects as 3 A: the chip's value stays.
    gCharger.unplug();
    gPd.unplug();
    advance(500);
    gCharger.plug(3000, 2);
    gPd.sink({}, 0, 0x8);
    advance(500);
    check(logHas("no USB-PD source capabilities"), "not USB-PD line");
    check(iinlim() == 3000, "non-PD: the chip's own detection stays: " + std::to_string(iinlim()));
    // A USB host port detected at 100 mA: raised to 500 mA (Horizon boot), Hi-Z cleared.
    gCharger.unplug();
    gPd.unplug();
    advance(500);
    gCharger.plug(100, 1);
    gCharger.reg[0] |= 0x80;  // the chip went to Hi-Z on the 100 mA source ([TI] 8.3.1.3.5)
    gPd.sink({}, 0, 0x8);
    advance(500);
    check(iinlim() == 500 && !(gCharger.reg[0] & 0x80), "USB host: 500 mA, Hi-Z off: " + hex(gCharger.reg[0]));
    check(battery()->adapter.numbers["FamilyCode"] == kIOPSFamilyCodeUSBHost, "family USB host");
    // A Mac: PD 5 V 3 A.
    gCharger.unplug();
    gPd.unplug();
    advance(500);
    gCharger.plug(500, 1);
    gPd.sink({PdChip::fixed(5000, 3000)}, 0, 0x8 | 0x1000 | 0x4000);
    advance(3000);
    check(logHas("SwitchPD: contract 5 V 2 A (*5 V 3 A)"), "Mac: 5 V 2 A");
    check(iinlim() == 2000, "Mac: 2000 mA");
}

static void refused()
{
    gPd.rdoReply = PdChip::Refuse;
    bootWithNintendoCharger(3000);  // the chip's own detection: 3 A
    advance(3000);
    check(logHas("SwitchPD: no USB-PD request: the request was refused (last command status 6); the controller's own "
                 "contract stays (source: 5 V 1.5 A, 15 V 2.6 A)"),
          "refusal line");
    check(iinlim() == 500 && gCharger.iinlimRaises == 0, "PD source without our contract: 500 mA");
    check(logHas("input limit 500 mA (was 3000 mA): USB-PD source without our contract"), "500 mA line");
    check(!logHas("SwitchPD: contract"), "no contract published");
    // Three unsolicited contracts: at most three failed requests per session.
    for (int i = 0; i < 4; ++i) {
        gPd.alert |= 0x1000 | 0x4000;
        advance(2000);
    }
    check(logCount("SwitchPD: no USB-PD request") == 3 && logHas("no more requests until unplugged"), "three at most");
    check(std::count(gPd.commands.begin(), gPd.commands.end(), std::string("0707")) == 3, "three requests");
}

static void silent()
{
    gPd.rdoReply = PdChip::Silent;
    bootWithNintendoCharger();
    advance(1400);
    check(!logHas("no USB-PD request"), "waiting");
    advance(200);
    check(logHas("no USB-PD request: no contract within 1.5 s of the request"), "timeout line");
    check(iinlim() == 500, "input limit untouched");
    gPd.rdoReply = PdChip::Accept;
    gPd.psRdyReply = false;
    gPd.alert |= 0x1000 | 0x4000;  // the source re-advertises: start over; PS_RDY never completes
    advance(2000);
    check(logHas("PS_RDY was not completed within 1.5 s"), "PS_RDY timeout line");
    check(iinlim() == 500 && !logHas("SwitchPD: contract"), "nothing published");
}

static void rebalance()
{
    bootWithNintendoCharger();
    advance(2300);
    check(iinlim() == 1200, "1200 mA");
    // A multi-port charger re-balances: new PDOs, its own new contract.
    gPd.sink({PdChip::fixed(5000, 3000), PdChip::fixed(9000, 2000), PdChip::fixed(15000, 1000)}, 0, 0x1000 | 0x4000);
    advance(300);
    check(iinlim() == 500, "withdrawn: 500 mA while the new request runs: " + std::to_string(iinlim()));
    advance(2300);
    check(logHas("SwitchPD: contract 9 V 2 A (5 V 3 A, *9 V 2 A, 15 V 1 A)"), "new contract 9 V 2 A");
    check(iinlim() == 2000, "2000 mA");
}

static void oursAlready()
{
    const charge::PdChoice c = charge::choosePdo(std::vector<uint32_t>{kNintendo5V, kNintendo15V}.data(), 2);
    gCharger.plug(500, 2);
    gCharger.intPending = false;
    gPd.sink({kNintendo5V, kNintendo15V}, 1, 0);
    gPd.curRdo = charge::encodeRdo(c);
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(400);
    check(gPd.commands.empty(), "no request: the contract is ours already");
    check(logHas("SwitchPD: contract 15 V 1.2 A") && iinlim() == 1200, "published at once");
}

static void settingsLost()
{
    bootWithNintendoCharger();
    advance(2300);
    gCharger.reset();  // a power-on reset: defaults, default mode
    gCharger.intPending = true;
    advance(300);
    check(logHas("charger settings lost (REG00-06 32 1b 60 11 b2 9a 03"), "lost line");
    check((gCharger.reg[5] & 0x38) == 0 && gCharger.reg[3] == 0 && iinlim() == 1200 && (gCharger.reg[1] & 0x0e) == 0,
          "set again: " + hex(gCharger.reg[5]) + " " + hex(gCharger.reg[0]));
}

static void lostWhileHot()
{
    bootWithNintendoCharger();
    advance(2300);
    gGauge.setTemp(5500);
    advance(300);
    check(((gCharger.reg[1] >> 4) & 3) == 0, "hot: off");
    gCharger.reset();  // a power-on reset while hot: REG01 back to charge
    gCharger.intPending = true;
    advance(300);
    check(logHas("charger settings lost") && ((gCharger.reg[1] >> 4) & 3) == 0, "set again, still off while hot");
    // A chip that keeps losing them: left alone, but still no charging while hot, and charging again when cool.
    for (int i = 0; i < 4; ++i) {
        gCharger.reset();
        gCharger.intPending = true;
        advance(300);
    }
    check(logHas("charger keeps losing its settings") && ((gCharger.reg[1] >> 4) & 3) == 0, "unmanaged, off while hot");
    const unsigned writes = gCharger.writes;
    gCharger.reset();
    gCharger.intPending = true;
    advance(300);
    check(((gCharger.reg[1] >> 4) & 3) == 0 && gCharger.writes == writes + 2 && (gCharger.reg[5] & 0x30) == 0,
          "only the watchdog off and REG01 off: " + hex(gCharger.reg[1]) + " " + hex(gCharger.reg[5]));
    gGauge.setTemp(3000);
    advance(300);
    check(((gCharger.reg[1] >> 4) & 3) == 1 && logHas("hot -> normal, charging (charger otherwise as found)"),
          "charging again at 30 C");
    check(gCharger.reg[2] == 0x60 && gCharger.reg[5] == 0x9a, "REG02 and REG05 as the chip has them (watchdog back)");
}

static void notTaken()
{
    gCharger.refuseReg05 = true;
    bootWithNintendoCharger();
    check(logHas("charger setup did not take"), "fallback line");
    check(gCharger.reg[2] == 0x60 && gCharger.reg[3] == 0x11 && gCharger.reg[4] == 0xb2 && gCharger.reg[6] == 0x03 &&
              (gCharger.reg[1] & 0x0e) == 0x0a,
          "put back as found");
    const unsigned writes = gCharger.writes;
    advance(5000);
    check(gCharger.writes == writes, "read only afterwards (" + std::to_string(gCharger.writes - writes) + " writes)");
    check(logHas("SwitchPD: contract 15 V 1.2 A") && iinlim() == 500, "contract made, input limit not touched");
    // Not managed, yet a boost must survive the chip's I2C watchdog (switchroot: no OTG watchdog).
    gCharger.refuseReg05 = false;
    gCharger.unplug();
    gPd.unplug();
    advance(300);
    check(switch_battery_set_otg_boost(true, false) == 0 && ((gCharger.reg[1] >> 4) & 3) == 2, "boost on");
    check((gCharger.reg[5] & 0x30) == 0, "watchdog off for the boost: " + hex(gCharger.reg[5]));
    advance(200000);
    check(((gCharger.reg[1] >> 4) & 3) == 2 &&
              std::count(gCharger.ops.begin(), gCharger.ops.end(), std::string("watchdog expired")) == 0,
          "boost still on after 200 s");
    check(battery()->external == false, "our own VBUS is not external power");
    check(switch_battery_set_otg_boost(false, false) == 0 && (gCharger.reg[5] & 0x30) == 0x10,
          "boost off: the watchdog as it was found (40 s): " + hex(gCharger.reg[5]));
    check(switch_battery_set_otg_boost(true, false) == 0 && (gCharger.reg[5] & 0x30) == 0, "boost on again");
#if DEVELOPMENT || DEBUG
    // REG09 read by the USB role report: the latched fault still reaches the next reading.
    gCharger.latched |= 0x40;
    char line[512];
    check(switch_battery_describe_otg(line, sizeof(line)) == 0 && std::strstr(line, "boost fault 1/0"), line);
    gCharger.intPending = true;
    advance(300);
    check(logHas("REG09 0x40"), "the fault the report read is in the next stats line");
#endif
}

static void linuxLeft()
{
    gCharger.reg[5] = 0x02;   // switchroot's bq2419x_charger_init: termination off, watchdog off, safety timer off
    gCharger.reg[7] |= 0x10;  // and JEITA_VSET
    gCharger.defaultMode = false;
    gCharger.latched = 0;
    bootWithNintendoCharger();
    check(logHas("watchdog 0 s, safety timer off") && logHas("host mode"), "as found from Linux");
    check(gCharger.reg[5] == 0x82, "termination on again: " + hex(gCharger.reg[5]));
    check(gCharger.reg[7] == 0x5b, "REG07 left alone: " + hex(gCharger.reg[7]));
}

static void badTemp()
{
    gGauge.reg[0x08] = 0;
    bootWithNintendoCharger();
    check(logHas("battery temperature 0.00 C (TEMP 0x0000) is not plausible: charger left as found, read only"),
          "line");
    check(gCharger.writes == 0, "no charger writes: " + std::to_string(gCharger.writes));
    advance(5000);
    check(gCharger.writes == 0 && iinlim() == 500, "read only afterwards");
}

// iPadOS's requests and what it reads back (SwitchChargePolicy.h [IOS-*]).
static IOReturn setProperty(const char *key, bool value)
{
    OSDictionary dict;
    dict.setObject(key, value ? kOSBooleanTrue : kOSBooleanFalse);
    return battery()->setProperties(&dict);
}

static IOReturn setProperty(const char *key, unsigned value)
{
    OSDictionary dict;
    OSNumber *n = OSNumber::withNumber(value, 32);
    dict.setObject(key, n);
    const IOReturn r = battery()->setProperties(&dict);
    n->release();
    return r;
}

static unsigned chgConfig()
{
    return (gCharger.reg[1] >> 4) & 3;
}

static void iosRequests()
{
    bootWithNintendoCharger();
    advance(2300);
    IOPMPowerSource *ps = battery();
    check(chgConfig() == 1 && ps->charging && ps->ps["ChargingOverride"] == 0, "charging");
    // Optimized Battery Charging (PowerUI): hold, then charge.
    check(setProperty("PredictiveChargingInhibit", true) == kIOReturnSuccess, "OBC request taken");
    advance(300);
    check(chgConfig() == 0 && !ps->charging, "OBC: held");
    check(ps->ps["ChargingOverride"] == 2, "OBC: ChargingOverride bit 1 (Optimized Battery Charging Engaged)");
    check(ps->ps["NotChargingReason"] == 0 && ps->external, "OBC: not a temperature hold, still on AC");
    check(logHas("PredictiveChargingInhibit on: charging held"), "OBC line");
    setProperty("PredictiveChargingInhibit", false);
    advance(300);
    check(chgConfig() == 1 && ps->ps["ChargingOverride"] == 0, "OBC released");
    // powerd's ChargeInhibit assertion: IsCharging = false / true.
    setProperty("IsCharging", false);
    advance(300);
    check(chgConfig() == 0 && logHas("ChargeInhibit assertion held"), "ChargeInhibit: off");
    setProperty("IsCharging", true);
    advance(300);
    check(chgConfig() == 1, "ChargeInhibit released: charging");
    // powerd's DisableInflow assertion: ExternalConnected = false: input off (Hi-Z), reported as no AC.
    setProperty("ExternalConnected", false);
    advance(300);
    check((gCharger.reg[0] & 0x80) && chgConfig() == 0, "DisableInflow: Hi-Z, charging off: " + hex(gCharger.reg[0]));
    check(!ps->external && ps->ps["AppleRawExternalConnected"] == 1, "DisableInflow: no AC, raw still connected");
    setProperty("ExternalConnected", true);
    advance(300);
    check(!(gCharger.reg[0] & 0x80) && iinlim() == 1200 && chgConfig() == 1, "inflow back: contract limit again");
    // thermalmonitord's charge rate limit index: 70 % / 50 % / 30 % of 2048 mA.
    setProperty("AppleChargeRateLimitIndex", 1u);
    advance(300);
    check(charge::bq::decodeChargeCurrent(gCharger.reg[2]) == 1408, "index 1: 1408 mA: " + hex(gCharger.reg[2]));
    setProperty("AppleChargeRateLimitIndex", 3u);
    advance(300);
    check(charge::bq::decodeChargeCurrent(gCharger.reg[2]) == 576, "index 3: 576 mA");
    check(ps->ps["AppleChargeRateLimitIndex"] == 3, "index read back");
    setProperty("AppleChargeRateLimitIndex", 0u);
    advance(300);
    check(gCharger.reg[2] == 0x60, "index 0: 2048 mA again");
    check(setProperty("SomethingElse", true) == kIOReturnUnsupported, "unknown keys refused");
    check(ps->dicts["ChargerData"].numbers["ChargingCurrent"] == 2048 &&
              ps->dicts["ChargerData"].numbers["ChargingVoltage"] == 4208 &&
              ps->dicts["ChargerData"].numbers["InputCurrentLimit"] == 1200,
          "ChargerData");
    check(ps->adapter.numbers["PMUConfiguration"] == 1200 && ps->adapter.numbers["IsWireless"] == 0,
          "AdapterDetails for thermalmonitord");
    // While we source VBUS: ChargeInhibit and DisableInflow never take REG01 or the input from the boost.
    gCharger.unplug();
    gPd.unplug();
    advance(300);
    check(switch_battery_set_otg_boost(true, false) == 0 && chgConfig() == 2, "boost on");
    setProperty("IsCharging", false);
    setProperty("ExternalConnected", false);
    advance(300);
    check(chgConfig() == 2 && !(gCharger.reg[0] & 0x80), "boost untouched: OTG, no Hi-Z: " + hex(gCharger.reg[0]));
    check(switch_battery_set_otg_boost(false, false) == 0 && chgConfig() == 0, "boost off: charging off (inhibited)");
    setProperty("IsCharging", true);
    setProperty("ExternalConnected", true);
    advance(300);
    check(chgConfig() == 1 && !(gCharger.reg[0] & 0x80), "released: charge mode, input on");
}

static void temperatureHoldPublished()
{
    bootWithNintendoCharger();
    advance(2300);
    gGauge.setTemp(5500);
    advance(300);
    IOPMPowerSource *ps = battery();
    check(chgConfig() == 0 && !ps->charging && ps->external, "hot: on AC, not charging");
    check((ps->ps["NotChargingReason"] & charge::kTlcMask) &&
              (ps->dicts["ChargerData"].numbers["NotChargingReason"] & charge::kTlcMask),
          "TLC: NotChargingReason in PowerUI's 0x11e mask (Charging On Hold: iPad Temperature)");
    check((ps->ps["NotChargingReason"] & 0x180) != 0x100, "not PowerLog's penalty box");
    // An OBC hold on top: both shown; cooling down leaves only OBC.
    setProperty("PredictiveChargingInhibit", true);
    gGauge.setTemp(3000);
    advance(300);
    check(chgConfig() == 0 && ps->ps["NotChargingReason"] == 0 && ps->ps["ChargingOverride"] == 2, "cool, OBC holds");
    setProperty("PredictiveChargingInhibit", false);
    advance(300);
    check(chgConfig() == 1 && ps->charging, "charging");
}

static void powerInWake()
{
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(10);
    check(gWakeEvents.empty(), "no event at boot");
    gCharger.plug(500, 2);
    gPd.sink({kNintendo5V, kNintendo15V}, 0, 0x8 | 0x1000 | 0x4000);
    advance(300);
    check(gWakeEvents.size() == 1 && gWakeEvents[0] == SWITCH_WAKE_POWER_IN && battery()->external,
          "plug: power-in wake event, ExternalConnected at once");
    advance(3000);
    check(gWakeEvents.size() == 1, "one per attach");
    gCharger.unplug();
    gPd.unplug();
    advance(300);
    check(gWakeEvents.size() == 1 && !battery()->external, "unplug: none");
    gCharger.plug(3000, 2);
    gPd.sink({}, 0, 0x8);
    advance(300);
    check(gWakeEvents.size() == 2, "a plain charger too");
    gCharger.unplug();
    gPd.unplug();
    advance(300);
    check(switch_battery_set_otg_boost(true, false) == 0, "boost");
    advance(300);
    check(gWakeEvents.size() == 2, "our own boost is not power in");
}

static void sleepWindows()
{
    bootWithNintendoCharger();
    advance(2300);
    check(gGauge.reg[0x02] == (31u << 8 | 29u) && gGauge.reg[0x03] != 0xff00, "awake: 1 C and SOC windows");
    switch_battery_suspend();
    check(gGauge.reg[0x02] == (51u << 8 | 17u) && gGauge.reg[0x03] == 0xff00 && gGauge.line,
          "asleep: zone edges only, SOC off, ALRT released: " + hex(gGauge.reg[0x02]));
    const unsigned reads = (unsigned)gLog.size();
    advance(600000);  // no re-read while asleep (the timer is off); a zone edge would still alert
    check(!logHas("stats:", reads), "no stats while asleep");
    gGauge.setTemp(5200);
    gGauge.update();
    check(gGauge.edge, "a zone edge raises ALRT (PMC wake 27)");
    switch_battery_resume();
    advance(300);
    check(chgConfig() == 0 && gGauge.reg[0x03] != 0xff00, "after the wake: hot, charging off, normal windows");
}

static void roleSwap()
{
    gCharger.plug(500, 1);
    gCharger.intPending = false;
    gPd.sink({PdChip::fixed(5000, 3000)}, 0, 0);  // a Mac
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(2400);
    check(iinlim() == 2000 && logHas("SwitchPD: contract 5 V 2 A"), "Mac: 5 V 2 A");
    // The Mac asks for a power-role swap: we come up as the source (no boost here: the partner keeps VBUS for now).
    gPd.status1 = (uint16_t)(gPd.status1 | 0x1000);
    gPd.alert |= 0x1000;
    advance(300);
    check(iinlim() == 500, "swapped: contract withdrawn, 500 mA: " + std::to_string(iinlim()));
    gPd.status1 = (uint16_t)(gPd.status1 & ~0x1000);
    gPd.alert |= 0x1000 | 0x4000;
    advance(3000);
    check(iinlim() == 2000, "swapped back: the contract again");
    // The Nintendo dock: its two-PDO signature.
    gCharger.unplug();
    gPd.unplug();
    advance(500);
    gCharger.plug(500, 2);
    gPd.sink({PdChip::fixed(5000, 500), PdChip::fixed(15000, 2600)}, 0, 0x8 | 0x1000 | 0x4000);
    advance(3000);
    check(logHas("SwitchPD: contract 15 V 1.2 A (dock: 5 V 0.5 A, *15 V 2.6 A)") && iinlim() == 1200, "dock");
}

// The dock's alternate-mode session runs on the contract we made: the data role swap (only for the dock's power
// signature, never the Mac's or Nintendo's charger), then its VDMs, which this dock never answers. The engine gives up
// after 1.5 s; the contract, the input limit and charging are untouched throughout and an hour later, and nothing but
// the swap and VDMs went to COMMAND after SEND_RDO and PS_RDY.
static void dockAltMode()
{
    bootWithNintendoCharger();
    advance(2300);
    check(gPd.commands == (std::vector<std::string>{"0707", "0505"}) && gPd.vdmsSent == 0,
          "Nintendo's charger: no data role swap, no VDM");
    gCharger.unplug();
    gPd.unplug();
    advance(500);
    gCharger.plug(500, 2);
    gPd.commands.clear();
    gPd.sink({PdChip::fixed(5000, 500), PdChip::fixed(15000, 2600)}, 0, 0x8 | 0x1000 | 0x4000);
    advance(3000);
    check(gPd.commands.size() >= 3 && gPd.commands[0] == "0707" && gPd.commands[1] == "0505" &&
              gPd.commands[2] == "1818",
          "SEND_RDO, PS_RDY, then the data role swap");
    check(((gPd.status1 >> 8) & 3) == 2 && gPd.vdmsSent == 1, "DFP after the swap, Discover Identity sent");
    check(logHas("SwitchPD: contract 15 V 1.2 A (dock: 5 V 0.5 A, *15 V 2.6 A)") && iinlim() == 1200,
          "dock contract published");
    check(logHas("the partner did not answer within 1.5 s"), "the engine gave up on the silent dock");
    check(gAltLanes == SWITCH_USB3_LANES_FREE && !gAltLanesTaken && !gDisplayPortConfigured,
          "silent dock: SuperSpeed pair released without a DisplayPort takeover");
    advance(3600 * 1000);
    check(iinlim() == 1200 && ((gCharger.reg[1] >> 4) & 3) == 1, "still charging at 1200 mA an hour later");
    check(gPd.vdmsSent == 1, "no retries after the engine gave up");
}

static void oledDockSourceStartup()
{
    check(switch_battery_start() == 0, "switch_battery_start");
    advance(10);
    check(switch_pd_allow_source(true) == 0, "host mode available");
    check(gPd.unpoweredSourceArms == 0 && gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] &&
              chgConfig() == 1 && (gPd.config1 >> 14) == 0,
          "fixed supplies ready before the first source switch arm, without BQ boost");
    SwitchPDState state{};
    check(switch_pd_add_listener(
              [](void *context, const SwitchPDState *current) { *static_cast<SwitchPDState *>(context) = *current; },
              &state) == 0,
          "power-role listener");
    const auto classifyDock = [&state] {
        gPd.status1 = 0x1280;
        gPd.status2 = 0x2000;
        gPd.alert |= 0x8;
        advance(10);
        check(!state.settled && chgConfig() != 2 && gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] &&
                  (gPd.config1 >> 14) == 3,
              "OLED attach: fixed supplies remain ready while boost and connector switches stay off");
        gPd.unplug();
        advance(100);
    };
    classifyDock();
    advance(1000);
    check(!state.settled && state.role != SWITCH_PD_ROLE_SOURCE && chgConfig() == 2 &&
              gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 0,
          "BQ24193 boost and rails power the OLED reattach wait before a source role is published");
    advance(1100);
    check(state.settled && state.role == SWITCH_PD_ROLE_NONE && chgConfig() == 1 &&
              gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)],
          "reattach timeout releases boost and leaves fixed supplies ready for the empty port");

    classifyDock();
    advance(1000);
    check(chgConfig() == 2, "second reattach powers VBUS");
    gCharger.plug(500, 2);
    gPd.sink({PdChip::fixed(5000, 1500)}, 0, 0x8 | 0x1000 | 0x4000);
    advance(10);
    check(state.settled && state.role == SWITCH_PD_ROLE_SINK && chgConfig() == 1 &&
              gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 3,
          "external source during reattach keeps fixed supplies with boost and source switches off");

    gCharger.unplug();
    gPd.unplug();
    advance(500);
    classifyDock();
    gCharger.plug(500, 2);
    advance(1000);
    check(logHas("not sourcing: the charger sees external VBUS") && state.settled &&
              state.role == SWITCH_PD_ROLE_SINK && chgConfig() == 1 && gOutputs[TEGRA_GPIO(X, 3)] &&
              gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 3,
          "external VBUS at the classify deadline keeps the sink refusal latched");
    gCharger.unplug();
    gPd.unplug();
    advance(10);
    classifyDock();
    advance(1000);
    switch_pd_sleep(true);
    advance(1100);
    check(state.settled && state.role == SWITCH_PD_ROLE_NONE && chgConfig() == 1 &&
              !gOutputs[TEGRA_GPIO(X, 3)] && !gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 3,
          "sleep cancels the powered reattach wait without rearming it");
    switch_pd_sleep(false);
    check(chgConfig() == 1 && gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] &&
              (gPd.config1 >> 14) == 0,
          "resume restores fixed supplies without restarting the canceled boost");
}

static void sourceSupplyLifecycle()
{
    check(switch_battery_start() == 0, "switch_battery_start");
    advance(10);
    gPd.status1 = 0x0580;
    gPd.status2 = 0;
    gPd.alert |= 0x8;
    advance(10);
    check(switch_pd_allow_source(true) == 0 && gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] &&
              chgConfig() != 2 && (gPd.config1 >> 14) == 3,
          "host capability arriving after a cold sink attach prepares fixed supplies without sourcing VBUS");
    check(switch_pd_allow_source(false) == 0 && !gOutputs[TEGRA_GPIO(X, 3)] &&
              !gOutputs[TEGRA_GPIO(K, 5)] && chgConfig() != 2 && (gPd.config1 >> 14) == 3,
          "host capability removal in Sink releases fixed supplies with source switches and boost off");
    gPd.unplug();
    advance(10);
    check(switch_pd_allow_source(true) == 0, "host mode available");
    gPd.status1 = 0x9e80;
    gPd.status2 = 0x2000;
    gPd.alert |= 0x8;
    advance(10);
    check(logHas("classifying:") && chgConfig() == 1 && gOutputs[TEGRA_GPIO(X, 3)] &&
              gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 3,
          "our VSAFE still classifies with fixed supplies on and connector switches off");
    advance(422);
    gCharger.plug(500, 1);
    gPd.status1 = 0x0d80;
    gPd.status2 = 0;
    gPd.alert |= 0x8;
    advance(10);
    check(logHas("dual-role partner:") && chgConfig() == 1 && gOutputs[TEGRA_GPIO(X, 3)] &&
              gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 3,
          "Mac takes the source role with fixed supplies on and BQ boost and source switches off");
    gCharger.unplug();
    gPd.unplug();
    advance(10);
    check(gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)], "empty port supplies restored");
    switch_pd_sleep(true);
    check(!gOutputs[TEGRA_GPIO(X, 3)] && !gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 3,
          "empty-port sleep opens connector switches and removes fixed supplies");
    switch_pd_sleep(false);
    check(gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 0,
          "resume prepares fixed supplies before rearming connector switches");
    check(switch_pd_allow_source(false) == 0 && !gOutputs[TEGRA_GPIO(X, 3)] &&
              !gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 3,
          "host disable releases empty-port supplies");
    check(switch_pd_allow_source(true) == 0 && gPd.unpoweredSourceArms == 0,
          "host enable never arms an unpowered source");
    gPd.status1 = 0x1280;
    gPd.status2 = 0x2000;
    gPd.alert |= 0x8;
    advance(10);
    switch_pd_sleep(true);
    check(chgConfig() == 1 && !gOutputs[TEGRA_GPIO(X, 3)] && !gOutputs[TEGRA_GPIO(K, 5)] &&
              (gPd.config1 >> 14) == 3,
          "sleep cancels classification with all source power off");
    gPd.unplug();
    advance(10);
    switch_pd_sleep(false);
    check(gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] && (gPd.config1 >> 14) == 0,
          "canceled classification resumes to a prepared empty port");
    gPd.status1 = 0x1280;
    gPd.status2 = 0x2000;
    gPd.alert |= 0x8;
    advance(10);
    gPd.alert |= 0x2;
    advance(10);
    check(chgConfig() == 1 && !gOutputs[TEGRA_GPIO(X, 3)] && !gOutputs[TEGRA_GPIO(K, 5)],
          "a source fault releases fixed supplies while held");
    stubSystemPowerMessage(kIOMessageSystemWillRestart);
    check(chgConfig() == 1 && !gOutputs[TEGRA_GPIO(X, 3)] && !gOutputs[TEGRA_GPIO(K, 5)],
          "shutdown leaves source supplies off");
}

static void macPort()
{
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(10);
    check(switch_pd_allow_source(true) == 0, "host mode available");
    SwitchPDState state{};
    check(switch_pd_add_listener(
              [](void *context, const SwitchPDState *current) { *static_cast<SwitchPDState *>(context) = *current; },
              &state) == 0,
          "power-role listener");
    gPd.rdoReply = PdChip::AbortOnce;
    gCharger.plug(500, 1);
    gPd.sink({PdChip::fixed(5000, 1500)}, 0, 0x8 | 0x1000 | 0x4000);
    gPd.curRdo = 1u << 28 | 50u << 10 | 150u;  // the controller's own contract: 0.5 A of 1.5 A
    advance(10);
    check(state.role == SWITCH_PD_ROLE_SINK && gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] &&
              chgConfig() != 2 && (gPd.config1 >> 14) == 3,
          "cold source attach keeps fixed supplies while source switches and boost stay off");
    advance(3190);
    check(logHas("request aborted at attach") && !logHas("no USB-PD request"), "aborted once, asked again");
    check(logHas("SwitchPD: contract 5 V 1.5 A (*5 V 1.5 A)") && iinlim() == 1500, "5 V 1.5 A: 1500 mA (r8a: 0.9 A)");
    check(std::count(gPd.commands.begin(), gPd.commands.end(), std::string("0707")) == 2 &&
              std::count(gPd.commands.begin(), gPd.commands.end(), std::string("0505")) == 1 &&
              state.role == SWITCH_PD_ROLE_SINK && !state.dataHost && ((gPd.status1 >> 8) & 3) == 1 &&
              gOutputs[TEGRA_GPIO(X, 3)] && gOutputs[TEGRA_GPIO(K, 5)] && chgConfig() != 2 && (gPd.config1 >> 14) == 3,
          "RDO and PS_RDY complete with fixed supplies retained and the Mac still the USB host");
}

// bm92t_system_reset_auto(false): a controller an earlier boot left plugged as DFP is reset first, then over-current
// protection goes on.
static void resetAtStart()
{
    gPd.status1 = 0x0280;  // INSERT, DFP
    gPd.vendor = 0x4;      // OCP off
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(10);
    check(!gPd.commands.empty() && gPd.commands.front() == "0d0d", "SYS_RESET first");
    check(logHas("SwitchPD: BM92T36 SYS_RESET: found plugged as DFP"), "one line");
    check((gPd.vendor & 0x4) == 0, "OCP on after the reset");
}

// bm92t_event_handler: an internal fault with a partner plugged resets the controller; the next event waits, turns
// over-current protection back on and puts the switches back as the phase has them (a sink: off).
static void faultReset()
{
    bootWithNintendoCharger();
    gPd.commands.clear();
    gPd.status1 |= 0x1;
    gPd.alert |= 0x8;
    advance(10);
    check(gPd.commands.size() == 1 && gPd.commands.front() == "0d0d", "SYS_RESET on the internal fault");
    check((gPd.vendor & 0x4) == 0x4, "the reset's ALERT read, nothing more yet");
    gPd.alert |= 0x8;  // the controller attaches the partner again
    advance(10);
    check((gPd.vendor & 0x4) == 0, "OCP back on at the next event");
    check(((gPd.config1 >> 14) & 3u) == 3u, "a sink session's switches off again");
    check((gPd.config2 & 0x4) != 0, "incoming data role swaps accepted as UFP again after the reset");
}

// A dual-role source other than the dock (its 5 V PDO has Dual-Role Data and USB Communications) leaves the Switch
// UFP. Discover Identity decides: a Mac stays the USB host, a powered hub is swapped and the Switch becomes its host.
// One the controller let swap us itself (CONFIG2 accept as UFP, ALERT bit 10) reaches SwitchUSB the same way.
static bool gDataHost;

static void watchDataRole()
{
    switch_pd_add_listener(
        [](void *, const SwitchPDState *state) {
            gDataHost = state->dataHost;
        },
        nullptr);
}

constexpr uint32_t kDualRoleData = 1u << 25 | 1u << 26;

static void dualRoleSource(uint32_t idHeader)
{
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(10);
    check((gPd.config2 & 0xc) == 0x4 && (gPd.alertEnable[1] & 0x4) != 0,
          "CONFIG2 accepts DR_Swap as UFP, ALERT_ENABLE has DR_SWAP");
    check(logHas("SwitchPD: incoming data role swaps accepted as UFP (CONFIG2 0 +accept as UFP, ALERT_ENABLE 0xfbff "
                 "+DR_SWAP)"),
          "one line with what was found");
    watchDataRole();
    gPd.idHeader = idHeader;
    gCharger.plug(500, 2);
    gPd.sink({PdChip::fixed(5000, 3000) | kDualRoleData, PdChip::fixed(9000, 3000), PdChip::fixed(15000, 3000)}, 0,
             0x8 | 0x1000 | 0x4000);
    advance(3000);
}

static void macSourceStaysHost()
{
    dualRoleSource(0xD14005ACu);
    check(gPd.vdmsSent == 1 && std::count(gPd.commands.begin(), gPd.commands.end(), std::string("1818")) == 0,
          "Discover Identity, no data role swap");
    check(((gPd.status1 >> 8) & 3) == 1 && !gDataHost, "UFP: the Mac stays the USB host");
    check(logHas("alternate mode (done): no data role swap: the source is a USB host"), "the decision's line");
}

static void dataDiscoveryWithoutCharging()
{
    check(switch_battery_start() == 0, "switch_battery_start");
    advance(10);
    gPd.commands.clear();
    gPd.sink({PdChip::fixed(5000, 0) | kDualRoleData}, 0, 0x1000 | 0x8);
    gPd.status1 = 0x0d80;
    gPd.status2 = 0;
    const uint32_t contract = gPd.curRdo;
    const uint8_t input = gCharger.reg[0];
    advance(499);
    check(gPd.commands.empty() && gPd.vdmsSent == 0, "discovery waits for the PDO timer");
    advance(2);
    check(gPd.commands == (std::vector<std::string>{"1111"}) && gPd.vdmsSent == 1,
          "PDO timer starts one identity discovery without SEND_RDO or PS_RDY");
    check(logHas("no fixed 5/9/12/15 V PDO up to 3 A that gives 500 mA or more"), "charging policy still refuses zero");
    check(gPd.curRdo == contract && !gCharger.input && gCharger.reg[0] == input,
          "controller contract and absent charger input remain unchanged");
    advance(2000);
    check(gPd.commands == (std::vector<std::string>{"1111"}) && gPd.vdmsSent == 1,
          "silent identity discovery is not repeated");
}

static void hubSourceIsSwapped()
{
    dualRoleSource(0x48602109u);
    const auto swap = std::find(gPd.commands.begin(), gPd.commands.end(), std::string("1818"));
    check(swap != gPd.commands.end() && std::find(gPd.commands.begin(), swap, std::string("1111")) != swap,
          "Discover Identity, then the data role swap");
    check(((gPd.status1 >> 8) & 3) == 2 && gDataHost, "DFP: the Switch is the hub's USB host");
    check(logHas("the source is a USB hub: data role swap"), "the decision's line");
    check(iinlim() == 1200 && logHas("SwitchPD: contract 15 V 1.2 A"),
          "the contract kept: " + std::to_string(iinlim()));
}

static void sourceSwapsUs()
{
    dualRoleSource(0);
    check(logHas("no data role swap: the source did not answer Discover Identity within 1.5 s") && !gDataHost,
          "a silent source leaves us UFP");
    gPd.status1 = (uint16_t)((gPd.status1 & ~(3u << 8)) | 2u << 8);
    gPd.alert |= 0x400;
    advance(10);
    check(gDataHost && logHas("the partner swapped the data roles: Discover Identity as DFP"),
          "its own swap makes us host");
    check(std::count(gPd.commands.begin(), gPd.commands.end(), std::string("1818")) == 0, "no swap of ours");
}

// A source fault gets a PD hard reset (PD_HARD_RST_CMD).
static void sourceFault()
{
    bootWithNintendoCharger();
    gPd.commands.clear();
    gPd.alert |= 0x2;
    advance(10);
    check(!gPd.commands.empty() && gPd.commands.front() == "0808", "PD hard reset on the source fault");
    check(logHas("SwitchPD: source fault: PD hard reset"), "one line");
}

static void otgLeftOn()
{
    gCharger.reg[1] = 0x21;  // an earlier boot rebooted with its boost on
    gCharger.defaultMode = false;
    if (switch_battery_start())
        check(false, "switch_battery_start");
    advance(10);
    check(logHas("charger was left with CHG_CONFIG 2 (REG01 0x21), now charge (REG01 0x11)"), "OTG left on: charge");
}

static void policy()
{
    using namespace charge;
    // Encodings: Atmosphere's static_asserts and the datasheet's reset values.
    for (unsigned ma : {512u, 576u, 768u, 2048u})
        check(bq::decodeChargeCurrent(bq::encodeChargeCurrent(ma)) == ma, "ICHG " + std::to_string(ma));
    check(bq::decodeChargeCurrent(bq::encodeChargeCurrent(4544)) == 2048, "ICHG capped at 2048");
    for (unsigned mv : {3504u, 3520u, 3952u, 4208u})
        check(bq::decodeChargeVoltage(bq::encodeChargeVoltage(mv)) == mv, "VREG " + std::to_string(mv));
    check(bq::decodeChargeVoltage(bq::encodeChargeVoltage(4400)) == 4208, "VREG capped at 4208");
    for (unsigned i = 0; i < 8; ++i)
        check(bq::decodeInputLimit(bq::encodeInputLimit(kInputSteps[i])) == kInputSteps[i], "IINLIM step");
    check(bq::encodeInputLimit(0) == 0 && bq::encodeInputLimit(149) == 0 && bq::encodeInputLimit(151) == 1 &&
              bq::encodeInputLimit(9999) == 7 && bq::encodeInputLimit(1199) == 3,
          "IINLIM rounds down");
    check(bq::decodeInputVoltage(0x30) == 4360 && bq::encodeInputVoltage(4360) == 0x30, "VINDPM default 4360");
    check(bq::decodeChargeCurrent(0x60) == 2048 && bq::decodeChargeVoltage(0xb2) == 4208, "reset values");
    check(bq::watchdogSeconds(0x9a) == 40 && bq::watchdogSeconds(0xaa) == 80 && bq::watchdogSeconds(0xba) == 160 &&
              bq::watchdogSeconds(0x8a) == 0,
          "watchdog decode");
    check(bq::encodePreChargeTermination(128, 128) == 0x00 && bq::encodePreChargeTermination(256, 256) == 0x11,
          "REG03");
    check(bq::encodeSystemMinimum(3000) == 0x00 && bq::encodeSystemMinimum(3500) == 0x0a, "SYS_MIN");

    // PDO choice (bm92t_check_pdo) and the input limit (bm92t_calculate_current_limit, DTB caps).
    struct Case {
        const char *what;
        std::vector<uint32_t> pdos;
        unsigned position, millivolts, limit;
    };

    auto F = PdChip::fixed;
    const Case cases[] = {
        {"Nintendo AC adapter", {F(5000, 1500), F(15000, 2600)}, 2, 15000, 1200},
        {"Nintendo dock signature", {F(5000, 500), F(15000, 2600)}, 2, 15000, 1200},
        {"dock signature, too little", {F(5000, 500), F(15000, 1500)}, 0, 0, 0},
        {"20 V laptop brick", {F(5000, 3000), F(9000, 3000), F(15000, 3000), F(20000, 3250)}, 3, 15000, 1200},
        {"9 V 3 A ties 15 V 1.8 A", {F(5000, 3000), F(9000, 3000), F(15000, 1800)}, 3, 15000, 1200},
        {"18 W phone charger", {F(5000, 3000), F(9000, 2000), F(12000, 1500)}, 3, 12000, 1500},
        {"20 W charger", {F(5000, 3000), F(9000, 2220)}, 2, 9000, 2000},
        {"Mac 5 V 3 A", {F(5000, 3000)}, 1, 5000, 2000},
        {"USB 5 V 0.9 A", {F(5000, 900)}, 1, 5000, 900},
        {"USB 5 V 0.4 A", {F(5000, 400)}, 0, 0, 0},
        {"MacBook port 5 V 1.5 A (r8a)", {F(5000, 1500)}, 1, 5000, 1500},
        {"over 3 A", {F(5000, 3000), F(15000, 5000)}, 1, 5000, 2000},
        {"odd voltage", {F(5000, 3000), F(14800, 3000)}, 1, 5000, 2000},
        {"variable and PPS only beyond 5 V",
         {F(5000, 3000), (2u << 30) | F(15000, 3000), (3u << 30) | 0x1234},
         1,
         5000,
         2000},
        {"no PDOs", {}, 0, 0, 0},
    };
    for (const Case &c : cases) {
        const PdChoice p = choosePdo(c.pdos.data(), (unsigned)c.pdos.size());
        check(p.position == c.position && (!p.position || (p.millivolts == c.millivolts && p.inputLimit == c.limit)),
              std::string(c.what) + ": PDO " + std::to_string(p.position) + " " + std::to_string(p.millivolts) +
                  " mV " + std::to_string(p.inputLimit) + " mA");
        if (p.position) {
            const uint32_t rdo = encodeRdo(p);
            check(rdoPosition(rdo) == p.position && rdoOperatingMilliamps(rdo) == p.inputLimit &&
                      rdoMaxMilliamps(rdo) == p.milliamps && !(rdo & kRdoMismatch) && (rdo & kRdoUsbCommunications) &&
                      (rdo & kRdoNoUsbSuspend) && !(rdo >> 31),
                  std::string(c.what) + ": RDO " + hex(rdo));
            check(contractMatches(p, p.pdo, rdo) && !contractMatches(p, p.pdo, rdo ^ (1u << 28)), "contract check");
        }
    }
    // Every PDO a source can offer: within 15 V, the PDO's current, the per-voltage caps, and a BQ24193 step.
    unsigned combos = 0;
    for (unsigned mv = 50; mv <= 21000; mv += 50)
        for (unsigned ma = 0; ma <= 5000; ma += 10) {
            const unsigned l = pdInputLimit(mv, ma);
            if (!l)
                continue;
            ++combos;
            const unsigned cap = mv == 5000 ? 2000 : mv == 9000 ? 2000 : mv == 12000 ? 1500 : 1200;
            check(l <= ma && l <= cap && l >= 100 && kInputSteps[bq::encodeInputLimit(l)] == l,
                  "limit " + std::to_string(mv) + "/" + std::to_string(ma));
        }
    check(combos > 1000, "limit sweep ran");
    for (unsigned a = 0; a < 512; ++a) {  // random-ish PDO sets
        uint32_t pdos[7];
        unsigned seed = a * 2654435761u;
        for (uint32_t &p : pdos) {
            seed = seed * 1103515245u + 12345u;
            p = (seed >> 3) & 0xffffffffu;
        }
        const PdChoice p = choosePdo(pdos, 1 + a % 7);
        if (p.position) {
            check(p.millivolts <= 15000 && pdoType(p.pdo) == 0 && p.milliamps <= 3000 && p.inputLimit <= p.milliamps,
                  "random set " + std::to_string(a));
        }
    }

    // Zones: limits at every temperature, hysteresis only toward less charge, a window that holds the zone.
    for (int t = -4000; t <= 9000; ++t) {
        const Zone raw = rawZone(t);
        const bool charges = t >= 400 && t < 5100;
        check(zoneCharges(raw) == charges, "raw zone at " + std::to_string(t));
        check(zoneChargeCurrent(raw) <= 2048 && zoneChargeVoltage(raw) <= 4208, "zone limits");
        if (t >= 400 && t < 1700)
            check(zoneChargeCurrent(raw) == 768, "cool 768 mA");
        for (Zone from : {Zone::Cold, Zone::Cool, Zone::Normal, Zone::Hot}) {
            const Zone next = nextZone(from, t);
            check(zoneRank(next) <= zoneRank(raw), "never more than the raw zone at " + std::to_string(t));
            check(!zoneCharges(next) || charges, "charging only from 4 to 51 C");
            if (zoneRank(next) > zoneRank(from))
                check(zoneRank(rawZone(t - 100)) >= zoneRank(next) && zoneRank(rawZone(t + 100)) >= zoneRank(next),
                      "more charge only 1 C inside at " + std::to_string(t));
            const uint16_t w = temperatureWindow(next, t);
            const int hi = (int8_t)(w >> 8), lo = (int8_t)(w & 0xff);
            check(t >= lo * 100 && t <= hi * 100, "window holds " + std::to_string(t) + ": " + hex(w));
        }
    }
    // Combined with a +-1 C window around the reading (switch_max17050_talrt's shape: nearest degree +-1).
    for (int t = -2000; t <= 8000; t += 7) {
        const int whole = t >= 0 ? (t + 50) / 100 : -((-t + 50) / 100);
        const uint16_t around = (uint16_t)((uint8_t)(int8_t)(whole + 1) << 8 | (uint8_t)(int8_t)(whole - 1));
        for (Zone z : {Zone::Cold, Zone::Cool, Zone::Normal, Zone::Hot}) {
            if (nextZone(z, t) != z)
                continue;
            const uint16_t w = intersectWindows(temperatureWindow(z, t), around);
            const int hi = (int8_t)(w >> 8), lo = (int8_t)(w & 0xff);
            const uint16_t zw = temperatureWindow(z, t);
            check(t >= lo * 100 && t <= hi * 100 && hi <= (int8_t)(zw >> 8) && lo >= (int8_t)(zw & 0xff),
                  "combined window at " + std::to_string(t) + ": " + hex(w));
        }
    }
    check(intersectWindows(0x3311, 0x201e) == 0x201e && intersectWindows(0x3311, 0x3432) == 0x3332, "intersections");
    check(temperatureWindow(Zone::Normal, 3000) == (51u << 8 | 17u), "normal window");
    check(temperatureWindow(Zone::Hot, 6000) == (127u << 8 | 50u), "hot window");
    check(chargerSetting(Zone::Cool).reg02 == 0x10 && chargerSetting(Zone::Hot).reg02 == 0x00, "zone ICHG");
}

// iPadOS 27 reads the battery through an AppleSmartBatteryPack beside the power source (SwitchBatteryPack.h); 21H461
// reads none, so its kernel publishes none.
static void batteryPack()
{
    bootWithNintendoCharger();
    advance(2300);
    AppleSmartBatteryPack *pack = nullptr;
    for (IOService *s : IOService::published())
        if (auto *p = dynamic_cast<AppleSmartBatteryPack *>(s))
            pack = p;
#if SWITCH_IPSW_KIND_24B119
    check(pack && pack->provider == battery(), "pack published under the battery");
    if (!pack)
        return;
    auto *id = dynamic_cast<OSNumber *>(pack->getProperty("ID"));
    check(id && id->value == 0, "pack ID 0, the one powerd looks for");
    auto data = [pack] {
        auto *d = dynamic_cast<OSDictionary *>(pack->getProperty("BatteryData"));
        return d ? *d : OSDictionary();
    };
    OSDictionary d = data();
    check(d.numbers["VirtualTemperature"] == 3000,
          "VirtualTemperature 30.00 C: " + std::to_string(d.numbers["VirtualTemperature"]));
    check(d.numbers["RemainingCapacity"] == 2586 && d.numbers["MaxCapacity"] == 4310 &&
              d.numbers["NominalChargeCapacity"] == 4310 && d.numbers["DesignCapacity"] == 4310,
          "capacities in mAh");
    check(d.numbers["Voltage"] == 3900 && d.numbers["AverageCurrent"] == 1800 && d.numbers["StateOfCharge"] == 60,
          "voltage, average current, state of charge");
    check(d.numbers.count("FullyCharged") && d.numbers["FullyCharged"] == 0 && !d.booleans.count("FullyCharged"),
          "FullyCharged is a number: powerd reads it as SInt32");
    check(d.strings["Serial"] == battery()->strings[kIOPMPSSerialKey] && d.strings["Serial"].size() == 17,
          "the battery's serial");
    gGauge.setTemp(4600);
    advance(300);
    check(data().numbers["VirtualTemperature"] == 4600,
          "VirtualTemperature follows the gauge's alert: " + std::to_string(data().numbers["VirtualTemperature"]));
#else
    check(!pack, "no pack on 21H461");
#endif
}

// One process per scenario: the drivers keep globals.
static void run(void (*scenario)())
{
    switch_ios::test::ExpectPassesInFreshProcess([scenario] {
        gCharger.gauge = &gGauge;
        try {
            scenario();
        } catch (const Panic &p) {
            check(false, std::string("panic: ") + p.what());
        }
        if (gFailures) {
            for (const auto &line : gLog)
                std::fprintf(stderr, "  %s\n", line.c_str());
            for (const auto &op : gCharger.ops)
                std::fprintf(stderr, "  BQ24193 %s\n", op.c_str());
        }
    });
}

TEST(SwitchCharging, Policy)
{
    run(policy);
}

TEST(SwitchCharging, NintendoBoot)
{
    run(nintendoBoot);
}

TEST(SwitchCharging, Jeita)
{
    run(jeita);
}

TEST(SwitchCharging, Hotplug)
{
    run(hotplug);
}

TEST(SwitchCharging, Refused)
{
    run(refused);
}

TEST(SwitchCharging, Silent)
{
    run(silent);
}

TEST(SwitchCharging, Rebalance)
{
    run(rebalance);
}

TEST(SwitchCharging, OursAlready)
{
    run(oursAlready);
}

TEST(SwitchCharging, BatteryPack)
{
    run(batteryPack);
}

TEST(SwitchCharging, SettingsLost)
{
    run(settingsLost);
}

TEST(SwitchCharging, NotTaken)
{
    run(notTaken);
}

TEST(SwitchCharging, OtgLeftOn)
{
    run(otgLeftOn);
}

TEST(SwitchCharging, LinuxLeft)
{
    run(linuxLeft);
}

TEST(SwitchCharging, BadTemp)
{
    run(badTemp);
}

TEST(SwitchCharging, LostWhileHot)
{
    run(lostWhileHot);
}

TEST(SwitchCharging, IosRequests)
{
    run(iosRequests);
}

TEST(SwitchCharging, TemperatureHold)
{
    run(temperatureHoldPublished);
}

TEST(SwitchCharging, PowerInWake)
{
    run(powerInWake);
}

TEST(SwitchCharging, SleepWindows)
{
    run(sleepWindows);
}

TEST(SwitchCharging, RoleSwap)
{
    run(roleSwap);
}

TEST(SwitchCharging, MacPort)
{
    run(macPort);
}

TEST(SwitchCharging, DockAltMode)
{
    run(dockAltMode);
}

TEST(SwitchCharging, OledDockSourceStartup)
{
    run(oledDockSourceStartup);
}

TEST(SwitchCharging, SourceSupplyLifecycle)
{
    run(sourceSupplyLifecycle);
}

TEST(SwitchCharging, ResetAtStart)
{
    run(resetAtStart);
}

TEST(SwitchCharging, FaultReset)
{
    run(faultReset);
}

TEST(SwitchCharging, SourceFault)
{
    run(sourceFault);
}

// Shut Down and Restart put the BM92T36 back as this boot found it: it stays powered across both, so the next OS
// (Horizon, L4T, this one) would otherwise inherit what this boot set. Sleep leaves it alone.
static void exitAsFound()
{
    gPd.config1 = 0x00e0;  // source switches on, as an earlier boot left them
    gPd.vendor = 0x4;      // over-current protection off, as after a SYS_RESET
    bootWithNintendoCharger();
    advance(3000);
    check(((gPd.config1 >> 14) & 3u) == 3u && (gPd.config2 & 0x4) && (gPd.alertEnable[1] & 0x4) && !(gPd.vendor & 0x4),
          "this boot's settings: switches off, swaps accepted as UFP, the DR_SWAP alert, OCP on");
    stubSystemPowerMessage(kIOMessageSystemWillSleep);
    check(gPd.config2 & 0x4, "sleep puts nothing back");
    stubSystemPowerMessage(kIOMessageSystemWillRestart);
    check(gPd.config1 == 0x00e0 && gPd.config2 == 0 && gPd.alertEnable[1] == 0xfb && gPd.vendor == 0x4,
          "every setting as found: CONFIG1 " + hex(gPd.config1) + " CONFIG2 " + hex(gPd.config2) + " VENDOR_CONFIG " +
              hex(gPd.vendor));
    check(logHas("SwitchPD: BM92T36 settings back as found"), "one line");
    const size_t commands = gPd.commands.size();
    gPd.alert |= 0x1000 | 0x4000;  // a late contract: nothing answers it any more
    advance(3000);
    check(gPd.commands.size() == commands && gPd.config2 == 0 && gPd.vendor == 0x4, "nothing written after");
}

// A fault's SYS_RESET reloads the controller's defaults and this boot sets its own again; Shut Down still returns the
// values found at start.
static void exitAfterFaultReset()
{
    bootWithNintendoCharger();
    gPd.status1 |= 0x1;
    gPd.alert |= 0x8;
    advance(10);
    gPd.alert |= 0x8;
    advance(10);
    check((gPd.config2 & 0x4) != 0, "accept as UFP set again after the reset");
    stubSystemPowerMessage(kIOMessageSystemWillPowerOff);
    check(gPd.config2 == 0 && (gPd.vendor & 0x4) == 0 && ((gPd.config1 >> 14) & 3u) == 3u, "as found at start");
}

TEST(SwitchCharging, ExitAsFound)
{
    run(exitAsFound);
}

TEST(SwitchCharging, ExitAfterFaultReset)
{
    run(exitAfterFaultReset);
}

TEST(SwitchCharging, MacSourceStaysHost)
{
    run(macSourceStaysHost);
}

TEST(SwitchCharging, DataDiscoveryWithoutCharging)
{
    run(dataDiscoveryWithoutCharging);
}

TEST(SwitchCharging, HubSourceIsSwapped)
{
    run(hubSourceIsSwapped);
}

TEST(SwitchCharging, SourceSwapsUs)
{
    run(sourceSwapsUs);
}
