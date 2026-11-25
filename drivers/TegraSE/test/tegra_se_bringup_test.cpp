// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE's bring-up against a register model of both engines: an operation the model never ends turns its engine off
// for the boot, and nothing after it waits for that engine. Each scenario runs in a process of its own (the driver's
// state is boot-wide) and is given 5 s; one still waiting then is let go (callers refused as at system sleep) and fails.
#include <gtest/gtest.h>

#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../TegraSE.cpp"
#include "bazel/testing/fresh_process.h"

namespace se_model {
constexpr uintptr_t kBase[kEngines] = {0x70012000, 0x70412000};

struct Engine {
    std::map<uint32_t, uint32_t> regs;
    unsigned started = 0;
    unsigned hangAt = 0;  // the operation (counted from 1) that never ends; 0: every one ends
    std::vector<uint32_t> crypto;
};

Engine engines[kEngines];
std::vector<std::string> logged;
uint32_t nextPa = 0x80000000;
IOFilterInterruptEventSource sources[kEngines];
unsigned sourcesMade;
IOService nub;
IOWorkLoop loop;

int engineAt(uintptr_t address)
{
    for (unsigned i = 0; i < kEngines; ++i) {
        if (address >= kBase[i] && address < kBase[i] + 0x2000)
            return static_cast<int>(i);
    }
    return -1;
}

uint32_t read(Engine &engine, uint32_t offset)
{
    switch (offset) {
    case SECURITY:
        return kSoftNonsecure;
    case PERKEY:
        return 0x3ffe;
    case STATUS:
    case ERR_STATUS:
        return 0;
    default:
        break;
    }
    if (offset > KEY_ACCESS && offset <= KEY_ACCESS + 4 * kLastSlot)
        return 0xff;
    return engine.regs[offset];
}

void write(unsigned index, uint32_t offset, uint32_t value)
{
    Engine &engine = engines[index];
    if (offset == INT_STATUS) {
        engine.regs[INT_STATUS] &= ~value;
        return;
    }
    if (offset == CRYPTO_CONFIG)
        engine.crypto.push_back(value);
    if (offset == OPERATION && (value & 1u)) {
        if (++engine.started == engine.hangAt)
            return;
        engine.regs[INT_STATUS] |= kIntDone;
        gEngines[index].source->fire();
        return;
    }
    engine.regs[offset] = value;
}

// Runs `body` on a thread of its own, as seThread runs; false if it was still running after 5 s.
template <class Body> bool finishes(Body body)
{
    std::promise<void> done;
    auto finished = done.get_future();
    std::thread worker([&] {
        body();
        done.set_value();
    });
    const bool ended = finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!ended)
        gSleeping = true;
    worker.join();
    return ended;
}

std::string log()
{
    std::string all;
    for (const auto &line : logged)
        all += line;
    return all;
}

uint32_t initial[16] = {1, 2, 3, 4, 5, 6, 7, 8};

void compress(ccdigest_state_t state, size_t nblocks, const void *)
{
    reinterpret_cast<uint32_t *>(state)[0] += static_cast<uint32_t>(nblocks);
}

const ccdigest_info kSha256 = {32, 32, 64, 0, nullptr, initial, compress};
const ccdigest_info kSha1 = {20, 20, 64, 0, nullptr, initial, compress};
}  // namespace se_model

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    const int index = se_model::engineAt(address);
    return index < 0 ? 0 : se_model::read(se_model::engines[index], static_cast<uint32_t>(address & 0x1fff));
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const int index = se_model::engineAt(address);
    if (index >= 0)
        se_model::write(static_cast<unsigned>(index), static_cast<uint32_t>(address & 0x1fff), value);
}

uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    return pa;
}

boolean_t ml_at_interrupt_context(void)
{
    return 0;
}

boolean_t ml_get_interrupts_enabled(void)
{
    return 1;
}

vm_offset_t ml_vtophys(vm_offset_t va)
{
    return va;
}

int get_preemption_level(void)
{
    return 0;
}

int write_random(void *, u_int)
{
    return 0;
}

kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *)
{
    return KERN_FAILURE;
}

void thread_deallocate(thread_t)
{}

int (*volatile switch_digest_compress_hook)(const struct ccdigest_info *, ccdigest_state_t, size_t, const void *);
const struct ccdigest_info *switch_digest_originals[4];

bool switch_car_clock_enabled(unsigned)
{
    return true;
}

bool switch_car_reset_asserted(unsigned)
{
    return false;
}

uint32_t switch_car_read(uint32_t)
{
    return 0x100;
}

void switch_refusal(const char *, const char *, const char *, ...)
{}
}

namespace {
thread_local int tSelf;
thread_local event_t tWaitingOn;
thread_local bool tWoken;
}  // namespace

thread_t current_thread()
{
    return reinterpret_cast<thread_t>(&tSelf);
}

void thread_terminate(thread_t)
{}

wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t, uint64_t)
{
    tWaitingOn = event;
    tWoken = false;
    return THREAD_AWAKENED;
}

void thread_wakeup(event_t event)
{
    tWoken = tWoken || event == tWaitingOn;
}

wait_result_t thread_block(thread_continue_t)
{
    const bool woken = tWoken;
    tWaitingOn = nullptr;
    tWoken = false;
    return woken ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}

void panic(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}

void IOLog(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    se_model::logged.emplace_back(line);
}

void IOSleep(unsigned ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

IOFilterInterruptEventSource *IOFilterInterruptEventSource::filterInterruptEventSource(OSObject *owner, Action,
                                                                                       Filter filter,
                                                                                       IOService *provider, int)
{
    if (se_model::sourcesMade == kEngines)
        return nullptr;
    auto *source = &se_model::sources[se_model::sourcesMade++];
    source->owner = owner;
    source->filter = filter;
    source->provider = provider;
    return source;
}

IOWorkLoop *IOWorkLoop::workLoop()
{
    return &se_model::loop;
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    return &se_model::nub;
}

uint32_t switch_interrupt_count(uint32_t)
{
    return 0;
}

void switch_interrupt_describe(uint32_t intid, char *buffer, size_t size)
{
    snprintf(buffer, size, "INTID %u", intid);
}

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    const size_t rounded = (size + 4095) & ~static_cast<size_t>(4095);
    *out = {};
    out->va = static_cast<uint8_t *>(aligned_alloc(4096, rounded));
    if (!out->va)
        return false;
    memset(out->va, 0, rounded);
    out->pa = se_model::nextPa;
    out->size = size;
    se_model::nextPa += static_cast<uint32_t>(rounded);
    return true;
}

void switch_dma_free(switch_dma_buffer *buffer)
{
    free(buffer->va);
    *buffer = {};
}

namespace {
void bringUpWithSe2LostOnItsSecondOperation()
{
    se_model::engines[1].hangAt = 2;
    const bool ended = se_model::finishes([] { seThread(nullptr, THREAD_AWAKENED); });
    EXPECT_TRUE(ended) << "bring-up kept waiting for SE2 after it went off:\n" << se_model::log();
    EXPECT_TRUE(gReady);
    EXPECT_TRUE(gEngines[0].up);
    EXPECT_FALSE(gEngines[1].up);
    EXPECT_EQ(se_model::engines[1].started, 2u) << "an operation reached SE2 after it was off";
    EXPECT_TRUE(gEngines[0].proven) << "callers reach SE1 from here";
    EXPECT_NE(se_model::log().find("SE2 off for this boot: operation never ended"), std::string::npos)
        << se_model::log();
    ASSERT_FALSE(se_model::engines[0].crypto.empty());
    ASSERT_FALSE(se_model::engines[1].crypto.empty());
    for (const uint32_t config : se_model::engines[0].crypto)
        EXPECT_NE(config & kCryptoMccif, 0u) << "SE1 moves its data through its memory clients";
    for (const uint32_t config : se_model::engines[1].crypto)
        EXPECT_EQ(config & kCryptoMccif, 0u) << "SE2 stays on the AHB";
}

void stateLoadAfterItsEngineWentOff()
{
    gAhb = switch_mmio_map(kAhbArbPA, 0x1000);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gLoop = IOWorkLoop::workLoop();
    gQueue = IOLockAlloc();
    ASSERT_TRUE(engineUp(gEngines[0]));
    ASSERT_TRUE(engineUp(gEngines[1]));
    se_model::engines[0].hangAt = 1;
    bool sha256 = true;
    bool sha1 = true;
    const bool ended = se_model::finishes([&] {
        sha256 = stateLoadWorks(gEngines[0], SHA256, &se_model::kSha256);
        sha1 = stateLoadWorks(gEngines[0], SHA1, &se_model::kSha1);
    });
    EXPECT_TRUE(ended) << "the SHA-1 state load kept waiting for SE1 after it went off:\n" << se_model::log();
    EXPECT_FALSE(sha256);
    EXPECT_FALSE(sha1);
    EXPECT_FALSE(gEngines[0].up);
    EXPECT_TRUE(gEngines[1].up);
    EXPECT_EQ(se_model::engines[0].started, 1u);
    EXPECT_EQ(se_model::engines[1].started, 0u) << "SE2 was not the engine under test";
}
}  // namespace

TEST(TegraSEBringUp, AnEngineThatNeverEndsAnOperationIsOffAndBringUpFinishesWithoutIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(bringUpWithSe2LostOnItsSecondOperation);
}

TEST(TegraSEBringUp, AStateLoadCheckOfAnEngineThatWentOffEndsAtOnce)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateLoadAfterItsEngineWentOff);
}
