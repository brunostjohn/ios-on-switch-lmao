// SPDX-License-Identifier: MIT
// What the system shader compiler holds as it serves, each run a process of its own over the mock GM20B: its start
// (compiler_service.mm open: the pack, the store, the request log, the take buffer, the pool's workers), the compiler
// opened at the first compile, then boot 12's requests (compiler_boot12.log) made through the pool and the pool idle.
// Once with the m2v device the compiler opened before its compile context (its libraries cached as compiler_engine.mm
// cached them), once with the engine. Each step's line: the heap in use and allocated (the device's footprint line's
// figures), phys_footprint and its peak, and what grew, by image and VM tag (footprint_breakdown.h). Run by hand with
// MallocStackLogging=1 and COMPILER_FOOTPRINT_SITES=<dir>, each step also leaves malloc_history's allocations by size there.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <malloc/malloc.h>
#include <pthread/qos.h>
#include "NativeBridgeMetalFrontend.h"
#include "bazel/testing/fresh_process.h"
#include "compiler_requests.h"
#include "footprint_breakdown.h"
#include "../compiler/compiler_engine.h"
#include "../compiler/compiler_log.h"
#include "../compiler/compiler_pool.h"
#include "../compiler/compiler_store.h"
#include "../raw/runtime/raw_pack.h"

#include <gtest/gtest.h>
#include <fstream>
#include <mutex>

namespace {

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

std::string loader()
{
    char path[PATH_MAX];
    const char *nvk = getenv("SWITCH_NVK");
    return nvk && realpath(nvk, path) ? path : "";
}

std::string root()
{
    return compiler_test::systemRoot(getenv("SWITCH_QUARTZCORE"));
}

// What the device's line reports (compiler_service.mm logFootprint on x1-jetsam), and what grew since the last step
struct Steps {
    const char *mode;
    std::map<std::string, int64_t> owners = footprint::dirtyByOwner();

    void step(const char *what)
    {
        task_vm_info_data_t vm = {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count);
        malloc_statistics_t heap = {};
        malloc_zone_statistics(nullptr, &heap);
        const std::map<std::string, int64_t> now = footprint::dirtyByOwner();
        char line[256];
        snprintf(line, sizeof line,
                 "%s %s: heap %llu KiB in use of %llu KiB; footprint %llu KiB, peak %llu KiB; grew:", mode, what,
                 (unsigned long long)(heap.size_in_use >> 10), (unsigned long long)(heap.size_allocated >> 10),
                 (unsigned long long)(vm.phys_footprint >> 10),
                 (unsigned long long)(vm.ledger_phys_footprint_peak >> 10));
        std::ofstream(dir() + "/" + mode + ".txt", std::ios::app) << line << footprint::growth(owners, now) << "\n";
        owners = now;
        if (const char *sites = getenv("COMPILER_FOOTPRINT_SITES")) {
            const std::string out = std::string(sites) + "/" + mode + "-" + what + ".txt";
            const std::string command =
                "/usr/bin/malloc_history " + std::to_string(getpid()) + " -allBySize > '" + out + "' 2>&1";
            system(command.c_str());
        }
    }
};

void show(const char *mode)
{
    std::ifstream in(dir() + "/" + mode + ".txt");
    for (std::string line; std::getline(in, line);)
        printf("%s\n", line.c_str());
}

struct Asked : switch_compiler::Work {
    switch_compiler::Prepared prepared;
};

// The compiler's own start: compiler_service.mm Service::open's state, and its pool, whose run is `compile`
struct Serving {
    switch_compiler::Store store;
    switch_compiler::RequestLog log;
    switch_compiler::SystemLibraries system{root()};
    std::vector<uint8_t> buffer = std::vector<uint8_t>(1u << 20);
    std::unique_ptr<switch_compiler::Pool> pool;

    template <typename Compile> bool open(const raw::Digest &rules, Compile compile)
    {
        std::string why;
        if (!store.open(dir() + "/shaders", 0, rules, why) || !log.open(dir() + "/shaders/requests"))
            return ADD_FAILURE() << why, false;
        switch_compiler::Pool::Hooks hooks;
        hooks.run = [compile](switch_compiler::Work &work, const std::function<void(int32_t)> &primary) {
            std::string entry, failed;
            const int32_t result = compile(static_cast<Asked &>(work).prepared, entry, failed);
            primary(result);
            return result;
        };
        hooks.complete = [](const switch_compiler::Waiter &, int32_t) {};
        hooks.setBand = [](switch_compiler::Band band) {
            pthread_set_qos_class_self_np(qos_class_t(switch_compiler::qosOf(band)), 0);
        };
        hooks.raise = [](pthread_t, switch_compiler::Band) -> void * {
            return nullptr;
        };
        hooks.lower = [](void *) {};
        pool = std::make_unique<switch_compiler::Pool>(std::move(hooks));
        return true;
    }

    // Boot 12's requests in its order: the 13 its request log replayed at the background class, then the 2 asked
    void replay(Steps &steps)
    {
        compiler_test::Requests made(
            root(), {{"/Applications/SwitchHWTest.app/default.metallib", getenv("SWITCH_HWTEST_METALLIB") ?: ""}});
        const std::vector<raw::CompileRequest> asked = made.of(compiler_test::corpusLines(getenv("SWITCH_BOOT12")));
        bool first = true;
        for (size_t i = 0; i < asked.size(); ++i) {
            auto work = std::make_unique<Asked>();
            std::string why;
            if (!switch_compiler::prepare(raw::encodeRequest(asked[i]), 0, system, work->prepared, why)) {
                ADD_FAILURE() << why;
                continue;
            }
            const switch_compiler::Name name = work->prepared.name;
            const bool background = i + 2 < asked.size();
            pool->submit(
                name, std::move(work),
                {i + 1, 1, background ? switch_compiler::Band::Background : switch_compiler::Band::Interactive, false});
            if (first) {
                pool->drain();
                steps.step("first compile");
                first = false;
            }
        }
        pool->drain();
        steps.step(("replayed " + std::to_string(asked.size())).c_str());
        malloc_zone_pressure_relief(nullptr, 0);
        steps.step("idle");
    }
};

void environment()
{
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    setenv("NMB_SHADOW_LOG", "0", 0);
}

void withTheDevice()
{
    environment();
    Steps steps{"device"};
    raw::Pack pack;
    std::string why;
    ASSERT_TRUE(pack.load(compiler_test::emptyPack(dir()).c_str(), 0, why)) << why;
    std::mutex opening, caching;
    id<MTLDevice> device = nil;
    std::unique_ptr<raw::Direct> direct;
    std::map<std::string, id<MTLLibrary>> libraries;
    Serving serving;
    const auto compile = [&](const switch_compiler::Prepared &p, std::string &entry, std::string &failed) {
        {
            std::lock_guard<std::mutex> l(opening);
            if (!device) {
                @autoreleasepool {
                    device = native_bridge_metal_open(loader().c_str(), nullptr, 0, nullptr);
                }
                direct = std::make_unique<raw::Direct>(pack);
            }
        }
        const switch_compiler::Libraries made = compiler_test::deviceLibraries(device, root());
        const switch_compiler::Libraries cached = [&](const raw::RequestLibrary &l, std::string &missed) {
            const raw::Digest digest = raw::sha256(l.bytes.data(), l.bytes.size());
            const std::string name = l.system ? l.path : std::string(digest.begin(), digest.end());
            std::lock_guard<std::mutex> g(caching);
            __strong id<MTLLibrary> &library = libraries[name];
            if (!library)
                library = made(l, missed);
            return library;
        };
        return switch_compiler::makeEntry(p, cached, *direct, 0, entry, failed);
    };
    ASSERT_TRUE(serving.open(raw::sha256("", 0), compile));
    steps.step("serving");
    serving.replay(steps);
}

void withTheEngine()
{
    environment();
    Steps steps{"context"};
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {compiler_test::emptyPack(dir()), loader(), root()}, why)) << why;
    Serving serving;
    ASSERT_TRUE(
        serving.open(engine.rules(), [&](const switch_compiler::Prepared &p, std::string &entry, std::string &failed) {
            return engine.compile(p, entry, failed);
        }));
    steps.step("serving");
    serving.replay(steps);
}

}  // namespace

TEST(CompilerFootprint, AtLaunch)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        Steps steps{"launch"};
        steps.owners = {};
        steps.step("launch");
    });
    show("launch");
}

TEST(CompilerFootprint, ServingBoot12WithTheM2VDevice)
{
    switch_ios::test::ExpectPassesInFreshProcess(withTheDevice);
    show("device");
}

TEST(CompilerFootprint, ServingBoot12WithTheCompileContext)
{
    switch_ios::test::ExpectPassesInFreshProcess(withTheEngine);
    show("context");
}
