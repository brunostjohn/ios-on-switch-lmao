// MC_CLIENT_HOTRESET_CTRL (0x200) and CTRL_1 (0x970) hold the hot-reset bits of clients different drivers power: CTRL
// VIC 18, NVENC 11, HC 6, DCB 3; CTRL_1 GPU 2, NVDEC 5, NVJPG 8 (Tegra X1 TRM 18.11.1.56, 18.11.1.114). Two drivers
// powering their engines at once must not lose each other's bit, so tegra_mc_hot_reset is their one writer.
//
// The model runs the real TegraSMMU.cpp and TegraDisplayB.cpp on a register file. A second driver's power step
// (tegra_mc_hot_reset for VIC) lands at the first read of CTRL by anyone else: at once if that read is outside the
// MC's lock, or, as a second CPU spinning on the lock would, right after the lock is released.
#include "drivers/SwitchDisplayPort/TegraDisplayB.h"
#include "drivers/TegraSMMU/TegraSMMU.h"
#include "kits/SwitchCAR/SwitchCAR.h"
#include "kits/SwitchCAR/SwitchPMC.h"
#include "mc_hot_reset_stubs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {
constexpr uintptr_t kMc = 0x70019000;
constexpr uint32_t kCtrl = 0x200;
constexpr uint32_t kVic = 1u << 18, kDcb = 1u << 3;

std::map<uintptr_t, uint32_t> gRegisters;
int gLocksHeld;
std::vector<std::function<void()>> gSpinning;
std::function<void()> gSecond;
bool gSecondRan;

uint32_t &mc(uint32_t offset)
{
    return gRegisters[kMc + offset];
}

void landsAtTheNextRead(std::function<void()> step)
{
    gSecond = std::move(step);
    gSecondRan = false;
}

class McHotReset : public testing::Test {
protected:
    void SetUp() override
    {
        gRegisters.clear();
        gSpinning.clear();
        gSecond = nullptr;
        gSecondRan = false;
    }

    TegraDisplayB displayB;
    char why[96] = "";
};

TEST_F(McHotReset, ADpPowerUpKeepsTheVicFlushThatLandsInsideIt)
{
    mc(kCtrl) = kDcb;
    landsAtTheNextRead([] {
        tegra_mc_hot_reset(kCtrl, kVic, true);
    });
    ASSERT_TRUE(displayB.powerUp(why, sizeof(why))) << why;
    EXPECT_TRUE(gSecondRan);
    EXPECT_EQ(mc(kCtrl), kVic) << "VIC flushed while display B powered up: its client must stay blocked";
}

TEST_F(McHotReset, ADpPowerUpKeepsTheVicReleaseThatLandsInsideIt)
{
    mc(kCtrl) = kDcb | kVic;
    landsAtTheNextRead([] {
        tegra_mc_hot_reset(kCtrl, kVic, false);
    });
    ASSERT_TRUE(displayB.powerUp(why, sizeof(why))) << why;
    EXPECT_TRUE(gSecondRan);
    EXPECT_EQ(mc(kCtrl), 0u) << "VIC powered on while display B powered up: its client must not stay in hot reset";
}

// Every other driver and kit names the hot-reset registers only to hand them to tegra_mc_hot_reset. TegraGM20B's
// platform.c still writes CTRL_1 itself for the GPU's flush and rail-down.
TEST(McHotResetSource, OnlyTheOwnerWritesTheHotResetRegisters)
{
    const std::regex write(R"((write32\w*|rmw32|\bw32|nvkm_wr32|nvkm_mask)\s*\()");
    const std::regex named(R"(hot_?reset)", std::regex::icase);
    const std::regex offset(R"(\b(g?[mM]c|mc_|mcRegs)\b[^;]*\b0x(200|970)\b)");
    unsigned units = 0;
    std::vector<std::string> writers;
    for (const char *tree : {"../../", "../../../kits/"}) {
        const std::filesystem::path root = switch_ios::test::PackagePath(tree);
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            const std::filesystem::path &path = entry.path();
            const std::string ext = path.extension().string();
            const std::string name = path.generic_string();
            if ((ext != ".c" && ext != ".cpp" && ext != ".h") || name.find("/test/") != std::string::npos ||
                name.ends_with("TegraSMMU/TegraSMMU.cpp") || name.ends_with("TegraGM20B/platform.c"))
                continue;
            ++units;
            std::ifstream file(path, std::ios::binary);
            std::string line;
            for (unsigned number = 1; std::getline(file, line); ++number) {
                if (line.find("write32") == std::string::npos && line.find("rmw32") == std::string::npos &&
                    line.find("w32") == std::string::npos && line.find("nvkm_mask") == std::string::npos)
                    continue;
                if (std::regex_search(line, write) &&
                    (std::regex_search(line, named) || std::regex_search(line, offset)))
                    writers.push_back(name + ":" + std::to_string(number) + ": " + line);
            }
        }
    }
    EXPECT_GT(units, 300u) << "the drivers' and kits' sources are not in the test's data";
    EXPECT_THAT(writers, testing::IsEmpty());
}

TEST(McHotResetSource, TheDisplayAndHost1xHandTheirBitsToTheOwner)
{
    const std::string displayB = switch_ios::test::SourceText("../../SwitchDisplayPort/TegraDisplayB.cpp");
    const std::string host1x = switch_ios::test::SourceText("../../TegraHost1x/TegraHost1x.cpp");
    EXPECT_NE(displayB.find("tegra_mc_hot_reset(kMcHotResetControl, kMcHotResetDcb, false)"), std::string::npos);
    EXPECT_NE(host1x.find("tegra_mc_hot_reset(MC_HOTRESET_CTRL, MC_HOTRESET_HC, false)"), std::string::npos);
}
}  // namespace

IOSimpleLock *IOSimpleLockAlloc()
{
    return new IOSimpleLock{};
}

void IOSimpleLockFree(IOSimpleLock *lock)
{
    delete lock;
}

void IOSimpleLockLock(IOSimpleLock *lock)
{
    EXPECT_FALSE(lock->held) << "a step that found the MC's lock held ran before its release";
    lock->held = true;
    ++gLocksHeld;
}

void IOSimpleLockUnlock(IOSimpleLock *lock)
{
    lock->held = false;
    --gLocksHeld;
    std::vector<std::function<void()>> spinning;
    spinning.swap(gSpinning);
    for (const auto &step : spinning)
        step();
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    const uint32_t value = gRegisters[address];
    if (address == kMc + kCtrl && gSecond) {
        std::function<void()> second = std::move(gSecond);
        gSecond = nullptr;
        std::function<void()> run = [second] {
            second();
            gSecondRan = true;
        };
        if (gLocksHeld)
            gSpinning.push_back(run);
        else
            run();
    }
    return value;
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gRegisters[address] = value;
}

uint16_t switch_mmio_model_read16(uintptr_t address)
{
    return static_cast<uint16_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write16(uintptr_t address, uint16_t value)
{
    switch_mmio_model_write32(address, value);
}

uint8_t switch_mmio_model_read8(uintptr_t address)
{
    return static_cast<uint8_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write8(uintptr_t address, uint8_t value)
{
    switch_mmio_model_write32(address, value);
}

void IOLog(const char *, ...)
{}

uint64_t mach_absolute_time(void)
{
    return 0;
}

void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *result)
{
    *result = 0;
}

uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}

bool switch_pmc_power_up(unsigned, unsigned, unsigned)
{
    return true;
}

bool switch_pmc_partition_off(unsigned)
{
    return true;
}

void switch_car_rmw(uint32_t, uint32_t, uint32_t)
{}

bool switch_car_clock_enabled(unsigned)
{
    return true;
}

void switch_car_clock_disable(unsigned)
{}

void switch_car_reset_assert(unsigned)
{}
}
