// The kernel's update check (switch_pt_signed_as_cdhash, which pretranslated_updates.c runs at capture) over the real
// translated files the boot package's pins name, from //install:pretranslated_updates: the bytes a deploy puts on the
// card. iOS 27 only.
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "kits/SwitchPretranslated/switch_pretranslated_updates.h"

namespace {

std::string Env(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

// {System path: CDHash hex} of a pins file ({"/usr/libexec/locationd": "020e95fc..."}).
std::map<std::string, std::string> Pins(const std::string &path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    const std::string json = text.str();
    std::map<std::string, std::string> pins;
    const std::regex entry(R"re("(/[^"]+)"\s*:\s*"([0-9a-f]{40})")re");
    for (std::sregex_iterator it(json.begin(), json.end(), entry), end; it != end; ++it)
        pins[(*it)[1].str()] = (*it)[2].str();
    return pins;
}

std::array<uint8_t, SWITCH_PT_CDHASH> Bytes(const std::string &hex)
{
    std::array<uint8_t, SWITCH_PT_CDHASH> out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return out;
}

bool ReadAt(void *context, void *buffer, uint32_t size, uint64_t offset)
{
    auto *file = static_cast<std::FILE *>(context);
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(buffer, 1, size, file) == size;
}

void *Allocate(size_t size)
{
    return std::malloc(size);
}

void Release(void *bytes, size_t)
{
    std::free(bytes);
}

void Sha256(const void *bytes, size_t size, uint8_t digest[SWITCH_PT_SHA256])
{
    CC_SHA256(bytes, static_cast<CC_LONG>(size), digest);
}

}  // namespace

TEST(RealUpdates, EveryPinnedUpdateIsSignedAsItsCDHashWithinTheHeadTheKernelReads)
{
    const std::string tree = Env("SWITCH_UPDATES_TREE");
    const std::map<std::string, std::string> pins = Pins(Env("SWITCH_UPDATES_PINS"));
    ASSERT_FALSE(pins.empty()) << "no updates in " << Env("SWITCH_UPDATES_PINS");
    for (const auto &[path, hex] : pins) {
        const std::string file = tree + path;
        std::FILE *bytes = std::fopen(file.c_str(), "rb");
        ASSERT_NE(bytes, nullptr) << file;
        struct stat st{};
        ASSERT_EQ(fstat(fileno(bytes), &st), 0) << file;
        std::array<uint8_t, 32> header{};
        ASSERT_TRUE(ReadAt(bytes, header.data(), header.size(), 0)) << file;
        uint32_t commands = 0;
        std::memcpy(&commands, header.data() + 20, sizeof(commands));
        EXPECT_LE(32u + commands, SWITCH_PT_UPDATE_HEAD) << path << ": load commands past the head the kernel reads";
        RecordProperty(path + " load command bytes", static_cast<int>(commands));
        const struct switch_pt_reader reader = {bytes, ReadAt, Allocate, Release, Sha256};
        const std::array<uint8_t, SWITCH_PT_CDHASH> cdhash = Bytes(hex);
        EXPECT_TRUE(switch_pt_signed_as_cdhash(&reader, static_cast<uint64_t>(st.st_size), cdhash.data())) << path;
        std::array<uint8_t, SWITCH_PT_CDHASH> other = cdhash;
        other[0] ^= 1;
        EXPECT_FALSE(switch_pt_signed_as_cdhash(&reader, static_cast<uint64_t>(st.st_size), other.data())) << path;
        std::fclose(bytes);
    }
}
