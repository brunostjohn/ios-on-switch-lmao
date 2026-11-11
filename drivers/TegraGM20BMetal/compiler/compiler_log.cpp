// SPDX-License-Identifier: MIT
// See compiler_log.h.
#include "compiler_log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"

namespace switch_compiler {
namespace {

bool writeAll(int fd, const void *bytes, size_t size)
{
    for (size_t at = 0; at < size;) {
        const ssize_t wrote = write(fd, static_cast<const uint8_t *>(bytes) + at, size - at);
        if (wrote <= 0)
            return false;
        at += size_t(wrote);
    }
    return true;
}

std::vector<std::string> readRequests(int fd, uint64_t &good)
{
    std::vector<std::string> out;
    good = 0;
    struct stat info = {};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode))
        return out;
    std::string all(size_t(std::min<uint64_t>(uint64_t(info.st_size), RequestLog::kLogBytes)), '\0');
    if (pread(fd, all.data(), all.size(), 0) != ssize_t(all.size()))
        return out;
    for (size_t at = 0; at + 8 <= all.size();) {
        uint32_t length, sum;
        memcpy(&length, all.data() + at, 4);
        memcpy(&sum, all.data() + at + 4, 4);
        if (length > RequestLog::kRequestBytes || length > all.size() - at - 8 ||
            switch_seed_pack::fnv32(reinterpret_cast<const uint8_t *>(all.data()) + at + 8, length) != sum)
            break;
        out.push_back(all.substr(at + 8, length));
        at += 8 + length;
        good = at;
    }
    return out;
}

}  // namespace

bool RequestLog::append(int fd, const std::string &request)
{
    const uint32_t head[2] = {
        uint32_t(request.size()),
        switch_seed_pack::fnv32(reinterpret_cast<const uint8_t *>(request.data()), request.size())};
    return writeAll(fd, head, sizeof head) && writeAll(fd, request.data(), request.size());
}

bool RequestLog::open(const std::string &path)
{
    std::lock_guard<std::mutex> l(lock_);
    path_ = path;
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd_ < 0)
        return false;
    read_ = readRequests(fd_, bytes_);
    if (ftruncate(fd_, off_t(bytes_)) || lseek(fd_, 0, SEEK_END) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

// lock held: the log written again without its oldest requests, so `room` more bytes fit in three quarters of it
bool RequestLog::trimLocked(uint64_t room)
{
    uint64_t good = 0;
    std::vector<std::string> kept = readRequests(fd_, good);
    uint64_t total = 0;
    for (const std::string &r : kept)
        total += 8 + r.size();
    size_t first = 0;
    while (first < kept.size() && total + room > kLogBytes / 4 * 3)
        total -= 8 + kept[first++].size();
    const std::string fresh = path_ + ".new";
    const int fd = ::open(fresh.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    bool written = fd >= 0;
    for (size_t i = first; written && i < kept.size(); ++i)
        written = append(fd, kept[i]);
    if (!written || rename(fresh.c_str(), path_.c_str())) {
        if (fd >= 0)
            close(fd);
        unlink(fresh.c_str());
        return false;
    }
    close(fd_);
    fd_ = fd;
    bytes_ = total;
    return true;
}

void RequestLog::add(const raw::Digest &name, const std::string &request)
{
    std::lock_guard<std::mutex> l(lock_);
    if (fd_ < 0 || names_.count(name) || request.size() > kRequestBytes)
        return;
    if (bytes_ + 8 + request.size() > kLogBytes && !trimLocked(8 + request.size()))
        return;
    if (!append(fd_, request))
        return;
    bytes_ += 8 + request.size();
    names_.insert(name);
    added_.emplace_back(name, request);
}

void RequestLog::replay(const std::function<bool(const std::string &request, raw::Digest &name)> &keep)
{
    std::vector<std::string> read;
    {
        std::lock_guard<std::mutex> l(lock_);
        read.swap(read_);
    }
    std::vector<std::pair<raw::Digest, std::string>> kept;
    std::set<raw::Digest> seen;
    for (const std::string &request : read) {
        raw::Digest name{};
        if (keep(request, name) && seen.insert(name).second)
            kept.emplace_back(name, request);
    }
    std::lock_guard<std::mutex> l(lock_);
    if (fd_ < 0)
        return;
    for (auto &entry : added_)
        if (seen.insert(entry.first).second)
            kept.push_back(std::move(entry));
    added_.clear();
    const std::string fresh = path_ + ".new";
    const int fd = ::open(fresh.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    uint64_t bytes = 0;
    bool written = fd >= 0;
    for (const auto &[name, request] : kept)
        if (written && (written = append(fd, request)))
            bytes += 8 + request.size();
    if (!written || rename(fresh.c_str(), path_.c_str())) {
        if (fd >= 0)
            close(fd);
        unlink(fresh.c_str());
        return;
    }
    close(fd_);
    fd_ = fd;
    bytes_ = bytes;
    names_ = std::move(seen);
}

}  // namespace switch_compiler
