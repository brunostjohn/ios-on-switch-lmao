// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/offline_metal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include "drivers/TegraGM20BMetal/utf8_path.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace switch_seed {
namespace {

void releaseHeldStorage(const void *owner) noexcept
{
    delete static_cast<const std::shared_ptr<MappedBytes> *>(owner);
}

bool isWinReservedName(std::string_view seg)
{
    const size_t dot = seg.find('.');
    std::string stem(dot == std::string_view::npos ? seg : seg.substr(0, dot));
    for (char &ch : stem)
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return true;
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' &&
        stem[3] <= '9')
        return true;
    return false;
}

}  // namespace

bool isSafeSha256Hex(std::string_view text)
{
    if (text.size() != 64)
        return false;
    for (char ch : text) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return true;
}

bool resolveContainedPath(const std::string &root, std::string_view guestPath, std::string &out)
{
    out.clear();
    if (root.empty() || guestPath.empty())
        return false;
    if (guestPath.front() == '/')
        guestPath.remove_prefix(1);
    if (guestPath.empty() || guestPath.back() == '/')
        return false;
    std::string rel;
    size_t pos = 0;
    while (pos <= guestPath.size()) {
        const size_t slash = guestPath.find('/', pos);
        const std::string_view seg =
            guestPath.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        if (seg.back() == '.' || seg.back() == ' ')
            return false;
        if (isWinReservedName(seg))
            return false;
        for (unsigned char c : seg) {
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|')
                return false;
        }
        if (!rel.empty())
            rel.push_back('/');
        rel.append(seg);
        if (slash == std::string_view::npos)
            break;
        pos = slash + 1;
    }
    out = root;
    if (out.back() != '/' && out.back() != '\\')
        out.push_back('/');
    out.append(rel);
    return true;
}

MappedBytes::~MappedBytes()
{
    if (!mapped_ || !data_)
        return;
#ifdef _WIN32
    UnmapViewOfFile(data_);
#else
    munmap(const_cast<uint8_t *>(data_), size_);
#endif
}

std::shared_ptr<MappedBytes> MappedBytes::openFile(const std::string &path, std::string &err)
{
#ifdef _WIN32
    HANDLE file = gm20b::createFileReadUtf8(path);
    if (file == INVALID_HANDLE_VALUE) {
        err = "cannot open " + path;
        return nullptr;
    }
    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0) {
        CloseHandle(file);
        err = "empty or unreadable file " + path;
        return nullptr;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) {
        err = "CreateFileMapping failed for " + path;
        return nullptr;
    }
    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (!view) {
        err = "MapViewOfFile failed for " + path;
        return nullptr;
    }
    auto out = std::make_shared<MappedBytes>();
    out->data_ = static_cast<const uint8_t *>(view);
    out->size_ = size_t(fileSize.QuadPart);
    out->mapped_ = true;
    return out;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = "cannot open " + path;
        return nullptr;
    }
    struct stat st;
    void *mapped = fstat(fd, &st) == 0 && st.st_size > 0
                       ? mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0)
                       : MAP_FAILED;
    ::close(fd);
    if (mapped == MAP_FAILED) {
        err = "cannot map " + path;
        return nullptr;
    }
    auto out = std::make_shared<MappedBytes>();
    out->data_ = static_cast<const uint8_t *>(mapped);
    out->size_ = size_t(st.st_size);
    out->mapped_ = true;
    return out;
#endif
}

std::shared_ptr<MappedBytes> MappedBytes::copyBytes(const uint8_t *data, size_t length)
{
    auto out = std::make_shared<MappedBytes>();
    out->owned_.assign(data, data + length);
    out->data_ = out->owned_.data();
    out->size_ = out->owned_.size();
    return out;
}

ConstantSet parseConstants(const std::vector<std::string> &items)
{
    std::map<uint64_t, ConstantValue> indexed;
    std::map<std::string, ConstantValue> named;
    for (const std::string &item : items) {
        const size_t a = item.rfind(':');
        const size_t b = (a == std::string::npos || a == 0) ? std::string::npos : item.rfind(':', a - 1);
        if (a == std::string::npos || b == std::string::npos || b == 0)
            continue;
        const std::string name = item.substr(0, b);
        const std::string typeText = item.substr(b + 1, a - b - 1);
        const std::string hex = item.substr(a + 1);
        ConstantValue c;
        c.type = std::strtoull(typeText.c_str(), nullptr, 10);
        c.length = gm20b::constantBytes(c.type);
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < c.bytes.size(); i += 2)
            c.bytes[i / 2] = uint8_t(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        if (name[0] == '#') {
            c.index = std::strtoull(name.c_str() + 1, nullptr, 10);
            indexed[c.index] = c;
        } else {
            c.name = name;
            c.isNamed = true;
            named[c.name] = c;
        }
    }
    ConstantSet set;
    for (const auto &[_, v] : indexed)
        set.values.push_back(v);
    for (const auto &[_, v] : named)
        set.values.push_back(v);
    return set;
}

ConstantSet parseConstantsLine(const std::string &text)
{
    std::vector<std::string> items;
    std::istringstream in(text);
    std::string item;
    while (in >> item)
        items.push_back(item);
    return parseConstants(items);
}

std::string ConstantSet::text() const
{
    std::vector<NmbConstant> nmb;
    nmb.reserve(values.size());
    for (const ConstantValue &v : values) {
        const uint64_t bytes = gm20b::constantBytes(v.type);
        nmb.push_back({v.type, v.isNamed ? 0 : v.index, v.isNamed ? v.name.c_str() : nullptr, v.bytes.data(), bytes});
    }
    return gm20b::constantsText(nmb.data(), nmb.size());
}

bool ConstantSet::toNmb(std::vector<NmbConstant> &out, std::string &why) const
{
    out.clear();
    out.reserve(values.size());
    for (const ConstantValue &v : values) {
        const uint64_t bytes = gm20b::constantBytes(v.type);
        if (!bytes) {
            why = "constant type " + std::to_string(v.type) + " is not qualified";
            return false;
        }
        out.push_back({v.type, v.isNamed ? 0 : v.index, v.isNamed ? v.name.c_str() : nullptr, v.bytes.data(), bytes});
    }
    return true;
}

OfflineLibrary::~OfflineLibrary()
{
    if (handle_)
        nmb_offline_library_release(handle_);
}

std::shared_ptr<OfflineLibrary> OfflineLibrary::create(std::shared_ptr<MappedBytes> storage,
                                                       const switch_metal::AirSliceRule &rule, uint64_t scope,
                                                       std::string &err)
{
    size_t offset = 0, size = 0;
    if (!storage || !switch_metal::airSlice(storage->data(), storage->size(), offset, size, rule)) {
        err = "This library does not have a compatible AIR slice for this device";
        return nullptr;
    }
    auto lib = std::make_shared<OfflineLibrary>();
    auto *owner = new std::shared_ptr<MappedBytes>(storage);
    NmbOfflineLibrary *handle =
        nmb_offline_library_new_held(storage->data() + offset, size, scope, owner, releaseHeldStorage);
    if (!handle) {
        err = nmb_last_error();
        return nullptr;
    }
    lib->handle_ = handle;
    lib->storage_ = std::move(storage);
    lib->sliceBytes_ = lib->storage_->data() + offset;
    lib->sliceLength_ = size;
    return lib;
}

const std::set<std::string> &OfflineLibrary::functionNames() const
{
    if (!namesLoaded_) {
        namesLoaded_ = true;
        const bool whole =
            switch_metal::forEachMetallibEntry(sliceBytes_, sliceLength_, [&](const switch_metal::MetallibEntry &e) {
                if (e.type != switch_metal::kMetallibExternType)
                    names_.insert(e.name);
                return true;
            });
        if (!whole)
            names_.clear();
    }
    return names_;
}

bool OfflineLibrary::airHash(const std::string &entry, gm20b::AirHash &out) const
{
    if (!hashesLoaded_) {
        hashesLoaded_ = true;
        hashesOk_ = gm20b::metallibAirHashes(sliceBytes_, sliceLength_, hashes_);
    }
    if (!hashesOk_)
        return false;
    const auto found = hashes_.find(entry);
    if (found == hashes_.end())
        return false;
    out = found->second;
    return true;
}

OfflineFunction::OfflineFunction(NmbOfflineFunction *handle, std::shared_ptr<OfflineLibrary> library, std::string name,
                                 std::string libraryName)
    : handle_(handle), library_(std::move(library)), name_(std::move(name)), libraryName_(std::move(libraryName)),
      functionType_(handle ? nmb_offline_function_type(handle) : 0)
{}

OfflineFunction::~OfflineFunction()
{
    if (handle_)
        nmb_offline_function_release(handle_);
}

switch_metal::FunctionTess OfflineFunction::tess() const
{
    switch_metal::FunctionTess t;
    if (library_) {
        const std::string &entry = libraryName_.empty() ? name_ : libraryName_;
        switch_metal::metallibFunctionTess(library_->sliceBytes(), library_->sliceLength(), entry.c_str(), t);
    }
    return t;
}

uint64_t OfflineFunction::patchType() const
{
    return tess().patchType;
}

bool OfflineFunction::airHash(gm20b::AirHash &out) const
{
    return library_ && library_->airHash(libraryName_, out);
}

std::shared_ptr<OfflineLibrary> LibraryStore::loadFile(const std::string &path, std::string &err)
{
    const auto it = cache_.find(path);
    if (it != cache_.end())
        return it->second;
    auto mapped = MappedBytes::openFile(path, err);
    if (!mapped)
        return nullptr;
    const uint64_t scope = sharedScope_ ? sharedScope_ : nmb_offline_scope_new();
    auto lib = OfflineLibrary::create(std::move(mapped), rule_, scope, err);
    if (lib)
        cache_[path] = lib;
    return lib;
}

std::shared_ptr<OfflineLibrary> LibraryStore::loadMemory(const uint8_t *bytes, size_t length, std::string &err)
{
    auto storage = MappedBytes::copyBytes(bytes, length);
    const uint64_t scope = sharedScope_ ? sharedScope_ : nmb_offline_scope_new();
    return OfflineLibrary::create(std::move(storage), rule_, scope, err);
}

std::shared_ptr<OfflineFunction> LibraryStore::makeFunction(const std::shared_ptr<OfflineLibrary> &lib,
                                                            const std::string &name, const std::string &specializedName,
                                                            const ConstantSet &constants, bool specialized,
                                                            std::string &err)
{
    if (!lib || !lib->handle() || name.empty()) {
        err = "a function name is required";
        return nullptr;
    }
    NmbOfflineFunction *handle = nullptr;
    if (specialized || !constants.empty() || !specializedName.empty()) {
        std::vector<NmbConstant> nmb;
        if (!constants.toNmb(nmb, err))
            return nullptr;
        const char *spec = specializedName.empty() ? nullptr : specializedName.c_str();
        handle =
            nmb_offline_function_new_specialized(lib->handle(), name.c_str(), spec, nmb.data(), uint32_t(nmb.size()));
        if (!handle) {
            err = std::string("specialized function: ") + nmb_last_error();
            return nullptr;
        }
    } else {
        handle = nmb_offline_function_new(lib->handle(), name.c_str());
        if (!handle) {
            err = std::string("function ") + name + ": " + nmb_last_error();
            return nullptr;
        }
    }
    return std::make_shared<OfflineFunction>(handle, lib, specializedName.empty() ? name : specializedName, name);
}

bool readWholeFile(const std::string &path, std::string &out, std::string &err)
{
    const auto wp = gm20b::fsPathUtf8(path);
    if (wp.empty()) {
        err = "invalid UTF-8 path " + path;
        return false;
    }
    std::ifstream in(wp, std::ios::binary | std::ios::ate);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        err = "cannot size " + path;
        return false;
    }
    out.resize(size_t(size));
    in.seekg(0);
    if (size > 0 && !in.read(&out[0], size)) {
        err = "cannot read " + path;
        return false;
    }
    return true;
}

}  // namespace switch_seed
