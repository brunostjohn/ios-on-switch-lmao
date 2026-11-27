// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct TaggedItem {
    int key;
    int order;
};

uint32_t fnv1a_32(const std::string &text)
{
    uint32_t hash = 0x811c9dc5U;
    for (const unsigned char ch : text) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 0x01000193U;
    }
    return hash;
}

std::string build_summary()
{
    static std::mutex lock_mutex;
    const std::lock_guard<std::mutex> guard(lock_mutex);

    const std::map<std::string, int> counts{
        {"beta", 20},
        {"alpha", 10},
        {"gamma", 30},
    };

    std::vector<int> values{9, 3, 7, 1, 5};
    std::sort(values.begin(), values.end());

    std::vector<TaggedItem> items{{2, 1}, {1, 2}, {2, 3}, {1, 4}};
    std::stable_sort(items.begin(), items.end(), [](const TaggedItem &lhs, const TaggedItem &rhs) {
        return lhs.key < rhs.key;
    });

    const std::filesystem::path artifact = std::filesystem::path("shaders") / "pack.bin";

    std::ostringstream out;
    out << artifact.filename().string() << '|';
    for (const auto &entry : counts) {
        out << entry.first << ':' << entry.second << ',';
    }
    out << '|';
    for (const int value : values) {
        out << value << ',';
    }
    out << '|';
    for (const TaggedItem &item : items) {
        out << item.key << ':' << item.order << ',';
    }
    return out.str();
}

}  // namespace

extern "C" int switch_hello_cpp_check(void);

extern "C" int switch_hello_cpp_check(void)
{
    try {
        throw std::runtime_error(build_summary());
    } catch (const std::runtime_error &err) {
        return static_cast<int>(fnv1a_32(err.what()) & 0x7fffffffU);
    } catch (...) {
        return -1;
    }
}
