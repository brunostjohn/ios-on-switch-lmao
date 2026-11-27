// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/native_seed_io.h"
#include "install/seed/cpp/switch_seed_cpp.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include "drivers/TegraGM20BMetal/cache_io.h"
#include "drivers/TegraGM20BMetal/program_pages.h"
#include "drivers/TegraGM20BMetal/utf8_path.h"
#include "install/seed/cpp/offline_metal.h"

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace switch_seed {

void setErrBuf(char *buf, size_t cap, const std::string &msg)
{
    if (!buf || !cap)
        return;
    const size_t n = msg.size() < cap - 1 ? msg.size() : cap - 1;
    std::memcpy(buf, msg.data(), n);
    buf[n] = '\0';
}

bool reportSeedError(const char *step, const std::string &path, int err)
{
    if (err)
        std::fprintf(stderr, "native-seed: %s %s: %s (errno %d)\n", step, path.c_str(), std::strerror(err), err);
    else
        std::fprintf(stderr, "native-seed: %s %s\n", step, path.c_str());
    return false;
}

bool setFileMode(const std::string &path, unsigned mode)
{
#ifdef _WIN32
    (void)path;
    (void)mode;
    return true;
#else
    struct stat st;
    if (stat(path.c_str(), &st))
        return reportSeedError("stat", path, errno);
    if ((st.st_mode & 07777u) == mode || !chmod(path.c_str(), mode_t(mode)))
        return true;
    return reportSeedError("chmod", path, errno);
#endif
}

bool writeFileAtomic(const std::string &path, const void *data, size_t size, unsigned mode)
{
    static std::atomic<uint32_t> serial{0};
#ifdef _WIN32
    const long pid = _getpid();
#else
    const long pid = long(getpid());
#endif
    const std::string tmp = path + "." + std::to_string(pid) + "-" + std::to_string(serial++) + ".tmp";
    const auto wtmp = gm20b::fsPathUtf8(tmp);
    if (wtmp.empty())
        return reportSeedError("write", path);
    {
        std::ofstream out(wtmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return reportSeedError("write", path, errno);
        if (size)
            out.write(static_cast<const char *>(data), std::streamsize(size));
        out.flush();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(wtmp, ec);
            return reportSeedError("write", path, errno);
        }
    }
#ifdef _WIN32
    if (!gm20b::moveFileReplaceUtf8(tmp, path)) {
        std::error_code ec;
        std::filesystem::remove(wtmp, ec);
        return reportSeedError("write", path);
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        const int e = errno;
        unlink(tmp.c_str());
        return reportSeedError("write", path, e);
    }
#endif
    return setFileMode(path, mode);
}

std::string hex16(uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::string airHex(const uint8_t *air)
{
    char text[65];
    for (unsigned i = 0; i < 32; ++i)
        std::snprintf(text + 2 * i, 3, "%02x", air[i]);
    return text;
}

bool parseHexBytes(const std::string &text, uint8_t *out, size_t bytes)
{
    if (text.size() != bytes * 2)
        return false;
    const auto digit = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (size_t i = 0; i < bytes; ++i) {
        const int a = digit(text[2 * i]), b = digit(text[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = uint8_t(a * 16 + b);
    }
    return true;
}

bool parseHexStack(const std::string &text, uint64_t &out)
{
    uint8_t bytes[8];
    if (!parseHexBytes(text, bytes, sizeof bytes))
        return false;
    out = 0;
    for (uint8_t byte : bytes)
        out = (out << 8) | byte;
    return true;
}

void identifyManifestRow(JsonValue &row, const gm20b::CacheKey &key)
{
    row.object["key"] = JsonValue::makeString(key.key);
    row.object["name"] = JsonValue::makeString(hex16(key.name()));
    row.object["vertexAir"] = JsonValue::makeString(airHex(key.vertexAir));
    row.object["fragmentAir"] = JsonValue::makeString(airHex(key.fragmentAir));
}

bool openSeedOutput(const std::string &output, uint64_t stack, gm20b::RecordCache &cache)
{
    const auto wout = gm20b::fsPathUtf8(output);
    if (wout.empty())
        return reportSeedError("create the output directory", output);
    std::error_code ec;
    std::filesystem::create_directories(wout, ec);
    if (ec)
        return reportSeedError("create the output directory", output, ec.value());
    if (!setFileMode(output, 0755))
        return false;
    errno = 0;
    if (!cache.attach(output, stack, 0))
        return reportSeedError("attach the record cache in", output, errno);
    return setFileMode(output + "/rec-" + hex16(stack), 0755);
}

bool writeSeedManifest(const std::string &output, uint64_t stack, std::vector<JsonValue> rows)
{
    JsonValue manifest = JsonValue::makeObject();
    manifest.object["schema"] = JsonValue::makeUint(1);
    manifest.object["stack"] = JsonValue::makeString(hex16(stack));
    manifest.object["packVersion"] = JsonValue::makeUint(gm20b::kPackVersion);
    manifest.object["cacheVersion"] = JsonValue::makeUint(gm20b::kEntryFormat);
    manifest.object["records"] = JsonValue::makeArray(std::move(rows));
    const std::string json = jsonSerialize(manifest);
    return writeFileAtomic(output + "/manifest.json", json.data(), json.size(), 0644);
}

bool writeRecordSlots(const gm20b::RuntimeRecord &record, uint64_t stack, const std::string &output,
                      std::vector<JsonValue> &slots, bool &hasSlots)
{
    slots.clear();
    hasSlots = false;
    for (const gm20b::CapturedProgram *program : gm20b::slotPrograms(record)) {
        if (program->image.empty())
            continue;
        gm20b::PackProgram info;
        std::vector<uint8_t> slot = gm20b::programSlot(*program, info);
        if (slot.size() < gm20b::kProgramSlotPage)
            continue;
        const uint64_t name = gm20b::seedSlotName(slot.data(), slot.size());
        slot.resize(gm20b::programSlotEntryBytes(slot.size()));
        const std::string relative = "rec-" + hex16(stack) + "/" + hex16(name) + ".slot";
        if (!writeFileAtomic(output + "/" + relative, slot.data(), slot.size(), 0644))
            return false;
        JsonValue entry = JsonValue::makeObject();
        entry.object["name"] = JsonValue::makeString(hex16(name));
        entry.object["path"] = JsonValue::makeString(relative);
        slots.push_back(std::move(entry));
    }
    hasSlots = !slots.empty();
    return true;
}

namespace {

bool recordIdentity(const std::string &bytes, gm20b::CacheKey &key, gm20b::RuntimeRecord &record,
                    gm20b::ComputeRecord &compute, bool &kernel)
{
    gm20b::Reader reader(bytes);
    char magic[sizeof gm20b::kEntryMagic];
    reader.bytes(magic, sizeof magic);
    const auto version = reader.value<uint32_t>(), kind = reader.value<uint32_t>();
    key.stack = reader.value<uint64_t>();
    reader.bytes(key.vertexAir, sizeof key.vertexAir);
    reader.bytes(key.fragmentAir, sizeof key.fragmentAir);
    key.key = reader.text();
    std::string refusal;
    if (!reader.ok() || std::memcmp(magic, gm20b::kEntryMagic, sizeof magic) || version != gm20b::kEntryFormat)
        return false;
    kernel = kind == gm20b::kKindKernel;
    if (kernel)
        return gm20b::decodeKernelEntry(bytes, key, compute, refusal) == gm20b::RecordCache::Found::Record &&
               gm20b::encodeKernelEntry(key, &compute, "") == bytes;
    return kind == gm20b::kKindRecord &&
           gm20b::decodeEntry(bytes, key, record, refusal) == gm20b::RecordCache::Found::Record &&
           gm20b::encodeEntry(key, &record, "") == bytes;
}

bool restampRow(JsonValue &row, const std::string &input, uint64_t oldStack, uint64_t stack, const std::string &output,
                gm20b::RecordCache &cache, std::map<uint64_t, std::string> &written)
{
    const JsonValue *statusField = row.field("status");
    if (!statusField || !statusField->isString())
        return false;
    const bool made = statusField->text == "made";
    const JsonValue *keyField = row.field("key");
    if (!keyField)
        return !made && !row.field("name") && !row.field("artifact");
    if (!keyField->isString())
        return false;
    const JsonValue *vAirField = row.field("vertexAir");
    const JsonValue *fAirField = row.field("fragmentAir");
    const JsonValue *nameField = row.field("name");
    if (!vAirField || !vAirField->isString() || !fAirField || !fAirField->isString() || !nameField ||
        !nameField->isString())
        return false;
    gm20b::CacheKey key;
    key.stack = oldStack;
    key.key = keyField->text;
    if (!parseHexBytes(vAirField->text, key.vertexAir, sizeof key.vertexAir) ||
        !parseHexBytes(fAirField->text, key.fragmentAir, sizeof key.fragmentAir) ||
        nameField->text != hex16(key.name()))
        return false;
    const std::string original = "rec-" + hex16(oldStack) + "/" + hex16(key.name()) + ".rec";
    if (!made) {
        if (row.field("artifact"))
            return false;
        key.stack = stack;
        identifyManifestRow(row, key);
        return true;
    }
    const JsonValue *artifactField = row.field("artifact");
    if (!artifactField || !artifactField->isString() || artifactField->text != original)
        return false;
    std::string bytes, readErr;
    if (!readWholeFile(input + "/" + original, bytes, readErr) || bytes.size() > (1u << 24))
        return false;
    gm20b::CacheKey decoded;
    gm20b::RuntimeRecord record;
    gm20b::ComputeRecord compute;
    bool kernel = false;
    if (!recordIdentity(bytes, decoded, record, compute, kernel) || decoded.stack != key.stack ||
        decoded.key != key.key || std::memcmp(decoded.vertexAir, key.vertexAir, sizeof key.vertexAir) ||
        std::memcmp(decoded.fragmentAir, key.fragmentAir, sizeof key.fragmentAir))
        return false;
    const JsonValue *kindField = row.field("kind");
    if ((kindField && (!kindField->isString() || kindField->text != (kernel ? "compute" : "render"))) ||
        (kernel && key.key.rfind("kernel | ", 0) != 0))
        return false;
    key.stack = stack;
    const std::string encoded =
        kernel ? gm20b::encodeKernelEntry(key, &compute, "") : gm20b::encodeEntry(key, &record, "");
    const auto [at, inserted] = written.try_emplace(key.name(), encoded);
    if (!inserted && at->second != encoded)
        return false;
    const std::string relative = "rec-" + hex16(stack) + "/" + hex16(key.name()) + ".rec";
    if (inserted) {
        gm20b::RuntimeRecord loaded;
        gm20b::ComputeRecord loadedCompute;
        std::string refusal;
        const bool stored =
            kernel ? cache.storeKernel(key, &compute, "") &&
                         cache.loadKernel(key, loadedCompute, refusal) == gm20b::RecordCache::Found::Record &&
                         gm20b::encodeKernelEntry(key, &loadedCompute, "") == encoded
                   : cache.store(key, &record, "") &&
                         cache.load(key, loaded, refusal) == gm20b::RecordCache::Found::Record &&
                         gm20b::encodeEntry(key, &loaded, "") == encoded;
        if (!stored || !setFileMode(output + "/" + relative, 0644))
            return false;
    }
    std::vector<JsonValue> slots;
    bool hasSlots = false;
    if (!kernel && !writeRecordSlots(record, stack, output, slots, hasSlots))
        return false;
    identifyManifestRow(row, key);
    row.object["artifact"] = JsonValue::makeString(relative);
    if (hasSlots)
        row.object["slots"] = JsonValue::makeArray(std::move(slots));
    else
        row.object.erase("slots");
    return true;
}

}  // namespace

}  // namespace switch_seed

extern "C" switch_seed_status_t switch_seed_native_restamp(uint64_t stack_key, const char *output_dir,
                                                           const char *const *input_dirs, size_t input_count,
                                                           char *err_buf, size_t err_cap)
{
    using namespace switch_seed;
    try {
        if (!output_dir || !input_dirs || input_count == 0) {
            setErrBuf(err_buf, err_cap, "missing required restamp arguments");
            return SWITCH_SEED_ERR_INVALID_ARG;
        }
        const std::string output = output_dir;
        gm20b::RecordCache cache;
        if (!openSeedOutput(output, stack_key, cache)) {
            setErrBuf(err_buf, err_cap, "failed to open output directory " + output);
            return SWITCH_SEED_ERR_IO;
        }
        std::vector<JsonValue> rows;
        std::map<uint64_t, std::string> written;
        for (size_t i = 0; i < input_count; ++i) {
            if (!input_dirs[i]) {
                setErrBuf(err_buf, err_cap, "null input directory");
                return SWITCH_SEED_ERR_INVALID_ARG;
            }
            const std::string input = input_dirs[i];
            std::string text, readErr;
            JsonValue manifest;
            if (!readWholeFile(input + "/manifest.json", text, readErr) || !jsonParse(text, manifest)) {
                reportSeedError("read the manifest of another build or none in", input);
                setErrBuf(err_buf, err_cap, "invalid manifest in " + input);
                return SWITCH_SEED_ERR_IO;
            }
            const JsonValue *schema = manifest.field("schema");
            const JsonValue *packVersion = manifest.field("packVersion");
            const JsonValue *cacheVersion = manifest.field("cacheVersion");
            const JsonValue *oldStackVal = manifest.field("stack");
            const JsonValue *records = manifest.field("records");
            uint64_t oldStack = 0;
            if (!manifest.isObject() || !schema || !schema->equalsUint(1) || !packVersion ||
                !packVersion->equalsUint(gm20b::kPackVersion) || !cacheVersion ||
                !cacheVersion->equalsUint(gm20b::kEntryFormat) || !oldStackVal || !oldStackVal->isString() ||
                !parseHexStack(oldStackVal->text, oldStack) || !records || !records->isArray()) {
                reportSeedError("read the manifest of another build or none in", input);
                setErrBuf(err_buf, err_cap, "incompatible manifest in " + input);
                return SWITCH_SEED_ERR_IO;
            }
            for (const JsonValue &source : records->array) {
                if (!source.isObject()) {
                    reportSeedError("read a record row of", input + "/manifest.json");
                    setErrBuf(err_buf, err_cap, "invalid row in " + input);
                    return SWITCH_SEED_ERR_IO;
                }
                JsonValue row = source;
                if (!restampRow(row, input, oldStack, stack_key, output, cache, written)) {
                    std::fprintf(stderr, "invalid or conflicting native record in %s\n", input.c_str());
                    setErrBuf(err_buf, err_cap, "invalid or conflicting native record in " + input);
                    return SWITCH_SEED_ERR_COMPILE;
                }
                rows.push_back(std::move(row));
            }
        }
        if (!writeSeedManifest(output, stack_key, std::move(rows))) {
            setErrBuf(err_buf, err_cap, "failed to write manifest.json");
            return SWITCH_SEED_ERR_IO;
        }
        return SWITCH_SEED_OK;
    } catch (const std::exception &e) {
        setErrBuf(err_buf, err_cap, std::string("restamp exception: ") + e.what());
        return SWITCH_SEED_ERR_COMPILE;
    } catch (...) {
        setErrBuf(err_buf, err_cap, "restamp: unknown C++ exception");
        return SWITCH_SEED_ERR_COMPILE;
    }
}
