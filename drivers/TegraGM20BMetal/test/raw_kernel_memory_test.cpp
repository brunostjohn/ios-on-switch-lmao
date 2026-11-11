// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_control_memory.h"
#include "../raw/runtime/raw_kernel.h"
#include "bazel/testing/source_text.h"
#include <IOKit/IOKitLib.h>
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace {
struct Model {
    uint64_t nextHandle = 1;
    std::map<uint32_t, std::vector<uint8_t>> pages;
    std::vector<uint32_t> allocFlags;
    std::vector<TegraGM20BAbiBindOp> binds;
    std::vector<TegraGM20BAbiCpuAccess> acquires;
} model;

constexpr uint32_t kInfoFlags = TegraGM20BInfoKernelMap | TegraGM20BInfoSubmitAddress | TegraGM20BInfoCpuAccess;
}  // namespace

extern "C" kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t method, const void *input, size_t,
                                                   void *output, size_t *)
{
    switch (method) {
    case TegraGM20BMethodGetInfo: {
        auto &info = *static_cast<TegraGM20BAbiInfo *>(output);
        info = {};
        info.abi_version = TegraGM20BAbiVersion;
        info.flags = kInfoFlags;
        info.va_start = 0x1000000;
        info.va_end = 0x100000000;
        info.page_size = 4096;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        const auto &in = *static_cast<const TegraGM20BAbiBoAlloc *>(input);
        model.allocFlags.push_back(in.flags);
        const uint32_t token = uint32_t(model.nextHandle);
        model.pages[token].assign(in.bytes, 0);
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, token, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoImport:
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, 0, 0};
        return KERN_SUCCESS;
    case TegraGM20BMethodMap: {
        const auto &in = *static_cast<const TegraGM20BAbiMap *>(input);
        auto &bytes = model.pages[in.token];
        *static_cast<TegraGM20BAbiMapReply *>(output) = {uint64_t(uintptr_t(bytes.data())), bytes.size()};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodVmBind:
        model.binds.push_back(static_cast<const TegraGM20BAbiVmBind *>(input)->ops[0]);
        return KERN_SUCCESS;
    case TegraGM20BMethodCpuAcquire:
        model.acquires.push_back(*static_cast<const TegraGM20BAbiCpuAccess *>(input));
        return KERN_SUCCESS;
    case TegraGM20BMethodCpuRelease:
    case TegraGM20BMethodBoFree:
    case TegraGM20BMethodUnmap:
        return KERN_SUCCESS;
    default:
        std::abort();
    }
}

extern "C" io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef matching)
{
    CFRelease(matching);
    return 111;
}

extern "C" kern_return_t IOServiceOpen(io_service_t, task_port_t, uint32_t, io_connect_t *connection)
{
    *connection = 42;
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOObjectRelease(io_object_t)
{
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOServiceClose(io_connect_t)
{
    return KERN_SUCCESS;
}

class RawKernelMemory : public testing::Test {
protected:
    raw::Kernel kernel;

    void SetUp() override
    {
        model = {};
        std::string err;
        ASSERT_TRUE(kernel.open(err)) << err;
        ASSERT_TRUE(kernel.exactJobs());
    }
};

TEST(RawControlMemory, MemoryTheGpuAloneTouchesPerPixelOrDrawIsCached)
{
    using raw::ControlUse;
    for (ControlUse use :
         {ControlUse::InterlockTickets, ControlUse::RootTable, ControlUse::ZeroPage, ControlUse::ColorTable})
        EXPECT_EQ(raw::controlMapping(use), raw::ControlMapping::Cached) << int(use);
    for (ControlUse use : {ControlUse::TextureHeaders, ControlUse::SamplerHeaders, ControlUse::Stamps, ControlUse::Ring,
                           ControlUse::Bytes, ControlUse::TransferSlot})
        EXPECT_EQ(raw::controlMapping(use), raw::ControlMapping::Volatile) << int(use);
}

// Device::init's allocation of each of its fixed control BOs names the use that maps it
TEST(RawControlMemory, DeviceAllocatesEachFixedBufferForItsUse)
{
    const std::string source = switch_ios::test::SourceText("../raw/runtime/raw_gpu.cpp");
    const std::pair<const char *, const char *> wanted[] = {
        {"tickets_", "InterlockTickets"},
        {"root_", "RootTable"},
        {"zero_", "ZeroPage"},
        {"colorTable_", "ColorTable"},
        {"tic_", "TextureHeaders"},
        {"tsc_", "SamplerHeaders"},
        {"stamps_", "Stamps"},
    };
    for (const auto &[member, use] : wanted) {
        const std::regex call(std::string("allocControl\\([^;()]*(\\([^()]*\\))?[^;()]*,\\s*") + member +
                              ",\\s*ControlUse::(\\w+)\\)");
        std::smatch match;
        ASSERT_TRUE(std::regex_search(source, match, call)) << member << " is not allocated for a named use";
        EXPECT_EQ(match[2].str(), use) << member;
    }
}

TEST_F(RawKernelMemory, CachedControlMemoryBindsThroughTheL2FromTheControlReserve)
{
    raw::Bo tickets, ring;
    ASSERT_TRUE(kernel.allocControl(0x1000, tickets, raw::ControlUse::InterlockTickets));
    ASSERT_TRUE(kernel.allocControl(0x10000, ring, raw::ControlUse::Ring));
    ASSERT_EQ(model.binds.size(), 2u);
    EXPECT_EQ(model.binds[0].op, uint32_t(TegraGM20BBindMap));
    EXPECT_EQ(model.binds[1].op, uint32_t(TegraGM20BBindMapVolatile));
    EXPECT_FALSE(tickets.gpuVolatile);
    EXPECT_TRUE(ring.gpuVolatile);
    ASSERT_EQ(model.allocFlags.size(), 2u);
    for (uint32_t flags : model.allocFlags)
        EXPECT_EQ(flags, uint32_t(TegraGM20BBoCoherent | TegraGM20BBoControl));
    kernel.free(tickets);
    kernel.free(ring);
}
