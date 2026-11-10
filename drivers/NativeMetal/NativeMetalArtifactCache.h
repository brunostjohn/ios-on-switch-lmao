// SPDX-License-Identifier: MIT
#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace native_artifact_cache {
using Key = std::array<uint8_t, 32>;
constexpr size_t kHeaderBytes = 88, kMaxPayloadBytes = 64 * 1024 * 1024;
constexpr std::array<uint8_t, 8> kMagic = {'S', 'W', 'N', 'C', 'A', 'R', 'T', 0};

inline std::string name(const Key &key)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < key.size(); ++i) {
        result[2 * i] = digits[key[i] >> 4];
        result[2 * i + 1] = digits[key[i] & 15];
    }
    return result + ".nca";
}

inline uint64_t little(const uint8_t *bytes, unsigned count)
{
    uint64_t result = 0;
    for (unsigned i = 0; i < count; ++i)
        result |= uint64_t(bytes[i]) << (8 * i);
    return result;
}

inline void put(uint8_t *bytes, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        bytes[i] = uint8_t(value >> (8 * i));
}

class Mapping {
    void *mapping_ = nullptr;
    size_t length_ = 0;

public:
    Mapping() = default;
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    Mapping(Mapping &&other) noexcept { swap(other); }
    Mapping &operator=(Mapping &&other) noexcept
    {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }
    ~Mapping() { reset(); }

    void reset()
    {
        if (mapping_)
            munmap(mapping_, length_);
        mapping_ = nullptr;
        length_ = 0;
    }

    void swap(Mapping &other) noexcept
    {
        std::swap(mapping_, other.mapping_);
        std::swap(length_, other.length_);
    }

    const void *data() const
    {
        return mapping_ ? static_cast<const uint8_t *>(mapping_) + kHeaderBytes : nullptr;
    }
    size_t size() const { return mapping_ ? length_ - kHeaderBytes : 0; }

    bool load(const std::string &directory, const Key &key)
    {
        reset();
        if (directory.empty())
            return false;
        const int fd = open((directory + "/" + name(key)).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            return false;
        struct stat info = {};
        const bool valid = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size > off_t(kHeaderBytes) &&
                           info.st_size <= off_t(kHeaderBytes + kMaxPayloadBytes);
        void *mapped = valid ? mmap(nullptr, size_t(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        close(fd);
        if (mapped == MAP_FAILED)
            return false;
        const auto *header = static_cast<const uint8_t *>(mapped);
        Key digest = {};
        const size_t bytes = size_t(info.st_size) - kHeaderBytes;
        bool matches = !memcmp(header, kMagic.data(), kMagic.size()) && little(header + 8, 4) == 1 &&
                       little(header + 12, 4) == kHeaderBytes && little(header + 16, 8) == bytes &&
                       !memcmp(header + 24, key.data(), key.size());
        if (matches) {
            CC_SHA256(header + kHeaderBytes, CC_LONG(bytes), digest.data());
            matches = !memcmp(header + 56, digest.data(), digest.size());
        }
        if (!matches) {
            munmap(mapped, size_t(info.st_size));
            return false;
        }
        mapping_ = mapped;
        length_ = size_t(info.st_size);
        return true;
    }
};

inline bool writeAll(int fd, const void *data, size_t length)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    while (length) {
        const ssize_t count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes += count;
        length -= size_t(count);
    }
    return true;
}

inline bool store(const std::string &directory, const Key &key, const void *payload, size_t bytes)
{
    if (directory.empty() || !payload || !bytes || bytes > kMaxPayloadBytes)
        return false;
    if (mkdir(directory.c_str(), 0700) && errno != EEXIST)
        return false;
    std::array<uint8_t, kHeaderBytes> header = {};
    memcpy(header.data(), kMagic.data(), kMagic.size());
    put(header.data() + 8, 1, 4);
    put(header.data() + 12, kHeaderBytes, 4);
    put(header.data() + 16, bytes, 8);
    memcpy(header.data() + 24, key.data(), key.size());
    CC_SHA256(payload, CC_LONG(bytes), header.data() + 56);
    static std::atomic<uint64_t> serial{0};
    char suffix[64];
    snprintf(suffix, sizeof suffix, ".tmp.%d.%llu", getpid(),
             static_cast<unsigned long long>(serial.fetch_add(1, std::memory_order_relaxed)));
    const std::string final = directory + "/" + name(key), temporary = final + suffix;
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    bool ok = writeAll(fd, header.data(), header.size()) && writeAll(fd, payload, bytes);
    if (close(fd))
        ok = false;
    if (ok)
        ok = !rename(temporary.c_str(), final.c_str());
    if (!ok)
        unlink(temporary.c_str());
    return ok;
}
} // namespace native_artifact_cache
