// SPDX-License-Identifier: MIT
// See raw_kernel.h. The transport (direct call, or tunnel {magic, method} + record through the selector that answers the tunnel
// probe) is nvkmd_darwin's (out/gpu/nvk/nvk-darwin.patch, MIT), so whatever the kext admits for NVK it admits here.
#include "raw_kernel.h"
#include "raw_encode_profile.h"

#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace raw {
namespace {
constexpr uint64_t kVaGranule = 0x10000;
constexpr uint64_t kBigPageAlignment = 0x20000;
constexpr char kNvk[] = "libvulkan_nouveau.dylib";

// NVK's image: the one this process loaded already, else the one beside this image (the plugin's bundle, which the m2v device's
// open loads NVK from: the same image then). Null: none (a host tool of the raw path alone).
void *nvkImage()
{
    const size_t suffix = strlen(kNvk);
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        const size_t length = name ? strlen(name) : 0;
        if (length > suffix && name[length - suffix - 1] == '/' && !strcmp(name + length - suffix, kNvk))
            return dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    }
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&nvkImage), &info) || !info.dli_fname)
        return nullptr;
    std::string path(info.dli_fname);
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return nullptr;
    path.replace(slash + 1, std::string::npos, kNvk);
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
}  // namespace

Kernel::~Kernel()
{
    if (events_)
        events_->shutdown();
    if (release_)
        release_();
    else if (conn_)
        IOServiceClose(conn_);
}

std::shared_ptr<SharedEvents> Kernel::sharedEvents()
{
    if (!exactJobs())
        return nullptr;
    std::lock_guard<std::mutex> lock(eventsLock_);
    if (!events_)
        events_ = SharedEvents::create(conn_, tunnel_, selector_);
    return events_;
}

static std::string failed(const char *what, int kr)
{
    char line[64];
    snprintf(line, sizeof line, "%s failed (0x%x)", what, unsigned(kr));
    return line;
}

int Kernel::call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
{
    RAW_ENCODE_SPAN(Kernel);
    size_t outBytes = outSize;
    if (!tunnel_)
        return IOConnectCallStructMethod(conn_, method, in, inSize, outSize ? out : nullptr,
                                         outSize ? &outBytes : nullptr);
    uint32_t buffer[1024];
    if (inSize > sizeof buffer - 8)
        return kIOReturnBadArgument;
    buffer[0] = TegraGM20BTunnelMagic;
    buffer[1] = method;
    if (inSize)
        memcpy(&buffer[2], in, inSize);
    return IOConnectCallStructMethod(conn_, selector_, buffer, 8 + inSize, outSize ? out : nullptr,
                                     outSize ? &outBytes : nullptr);
}

bool Kernel::open(std::string &err)
{
    using Connection = bool (*)(uint32_t *, bool *, uint32_t *, uint64_t *, uint64_t *);
#ifdef SWITCH_RAW_OWN_CONNECTION
    // HW Test's bench loop (debug images only): a second raw device in a process whose facade already holds NVK's VA
    // window, so it needs a GPU VM of its own; its experiments' channel kills stay away from the app's Metal device too
    void *nvk = nullptr;
#else
    void *nvk = nvkImage();
#endif
    if (!nvk)
        return openOwn(err);
    const auto connection = reinterpret_cast<Connection>(dlsym(nvk, "switch_nvk_connection"));
    const auto release = reinterpret_cast<void (*)()>(dlsym(nvk, "switch_nvk_connection_release"));
    if (!connection || !release) {
        // No fallback to a connection of our own: beside NVK's it would split the process's GPU VM in two
        err = "the loaded NVK has no switch_nvk_connection, so it is older than this tree (rebuild //gpu/nvk:host)";
        return false;
    }
    // [vaStart, vaEnd) is the whole window NVK leaves out, handed to every caller: one Kernel per process may take it (the
    // render server's one raw device), or two free lists would hand out the same VAs.
    uint64_t vaStart = 0, vaEnd = 0;
    if (!connection(&conn_, &tunnel_, &selector_, &vaStart, &vaEnd)) {
        err = "NVK's connection did not open";
        return false;
    }
    release_ = release;
    shared_ = true;
    if (call(TegraGM20BMethodGetInfo, nullptr, 0, &info_, sizeof info_) != KERN_SUCCESS ||
        info_.abi_version != TegraGM20BAbiVersion) {
        err = "GetInfo failed";
        return false;
    }
    vaStart = (vaStart + kVaGranule - 1) & ~(kVaGranule - 1);
    vaFree_[vaStart] = (vaEnd & ~(kVaGranule - 1)) - vaStart;
    return true;
}

bool Kernel::openOwn(std::string &err)
{
    const char *name = getenv("NVK_DARWIN_SERVICE");
    if (!name || !*name)
        name = "IOAcceleratorES";
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
    if (!service) {
        err = std::string("no ") + name;
        return false;
    }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), TegraGM20BOpenType, &c);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        err = "IOServiceOpen failed";
        return false;
    }
    conn_ = c;
    kr = call(TegraGM20BMethodGetInfo, nullptr, 0, &info_, sizeof info_);
    if (kr != KERN_SUCCESS) {
        const uint32_t probe[2] = {TegraGM20BTunnelMagic, TegraGM20BTunnelProbe};
        for (uint32_t s = 0; s < 256 && !tunnel_; s++)
            if (IOConnectCallStructMethod(conn_, s, probe, sizeof probe, nullptr, nullptr) == KERN_SUCCESS) {
                tunnel_ = true;
                selector_ = s;
            }
        if (tunnel_)
            kr = call(TegraGM20BMethodGetInfo, nullptr, 0, &info_, sizeof info_);
    }
    if (kr != KERN_SUCCESS || info_.abi_version != TegraGM20BAbiVersion) {
        err = "GetInfo failed";
        return false;
    }
    // the top quarter of the client range
    const uint64_t start =
        (info_.va_start + (info_.va_end - info_.va_start) / 4 * 3 + kVaGranule - 1) & ~(kVaGranule - 1);
    vaFree_[start] = (info_.va_end & ~(kVaGranule - 1)) - start;
    return true;
}

uint64_t Kernel::allocVa(uint64_t bytes, uint64_t align)
{
    std::lock_guard<std::mutex> lock(vaLock_);
    bytes = (bytes + kVaGranule - 1) & ~(kVaGranule - 1);
    const uint64_t minimumAlignment = bytes >= kBigPageAlignment ? kBigPageAlignment : kVaGranule;
    if (align < minimumAlignment)
        align = minimumAlignment;
    for (auto it = vaFree_.begin(); it != vaFree_.end(); ++it) {
        const uint64_t at = (it->first + align - 1) & ~(align - 1), end = it->first + it->second;
        if (at + bytes > end)
            continue;
        const uint64_t start = it->first;
        vaFree_.erase(it);
        if (at > start)
            vaFree_[start] = at - start;
        if (at + bytes < end)
            vaFree_[at + bytes] = end - at - bytes;
        vaUsed_ += bytes;
        return at;
    }
    return 0;
}

void Kernel::freeVa(uint64_t va, uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(vaLock_);
    bytes = (bytes + kVaGranule - 1) & ~(kVaGranule - 1);
    if (!va || !bytes)
        return;
    vaUsed_ -= bytes;
    auto next = vaFree_.lower_bound(va);
    if (next != vaFree_.end() && va + bytes == next->first) {  // merge with the range after
        bytes += next->second;
        next = vaFree_.erase(next);
    }
    if (next != vaFree_.begin()) {  // and with the one before
        auto prev = std::prev(next);
        if (prev->first + prev->second == va) {
            prev->second += bytes;
            return;
        }
    }
    vaFree_[va] = bytes;
}

bool Kernel::bind(uint64_t va, uint64_t handle, uint64_t range, uint32_t kind, bool gpuVolatile)
{
    TegraGM20BAbiVmBind b = {};
    b.count = 1;
    b.ops[0] = {gpuVolatile && exactJobs() ? TegraGM20BBindMapVolatile : TegraGM20BBindMap,
                kind, handle, 0, va, range};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + sizeof(b.ops[0]);
    if (call(TegraGM20BMethodVmBind, &b, in, nullptr, 0) != KERN_SUCCESS) {
        setError("VmBind failed");
        return false;
    }
    return true;
}

bool Kernel::alloc(uint64_t bytes, bool coherent, Bo &bo, uint32_t kind, uint64_t align, bool gpuVolatile, bool control)
{
    bytes = (bytes + info_.page_size - 1) & ~uint64_t(info_.page_size - 1);
    TegraGM20BAbiBoAlloc a = {bytes, 12, coherent ? uint32_t(TegraGM20BBoCoherent) : 0u};
    if (control && coherent && !kind)
        a.flags |= TegraGM20BBoControl;
    TegraGM20BAbiBoAllocReply r = {};
    bo = {};
    if (call(TegraGM20BMethodBoAlloc, &a, sizeof a, &r, sizeof r) != KERN_SUCCESS) {
        setError("BoAlloc failed");
        return false;
    }
    bo.handle = r.handle;
    bo.token = r.map_token;
    bo.bytes = bytes;
    uint64_t addr = 0, size = 0;
    if (!map(r.map_token, false, addr, size) || size < bytes) {
        setError("BO map failed");
        free(bo);
        return false;
    }
    bo.cpu = reinterpret_cast<uint8_t *>(uintptr_t(addr));
    bo.mapped = size;
    bo.va = bo.vaBase = allocVa(bytes, align);
    if (!bo.va) {
        setError("out of GPU VA");
        free(bo);
        return false;
    }
    bo.gpuVolatile = gpuVolatile && exactJobs();
    if (!bind(bo.va, bo.handle, bytes, kind, bo.gpuVolatile)) {
        free(bo);
        return false;
    }
    return true;
}

bool Kernel::import(void *addr, uint64_t bytes, Bo &bo, uint32_t kind, uint32_t surfaceId)
{
    if (!importPages(addr, bytes, bo, surfaceId)) {
        setError("BoImport failed");
        return false;
    }
    return bindImport(addr, bo, kind);
}

bool Kernel::importPages(void *addr, uint64_t bytes, Bo &bo, uint32_t surfaceId, bool gpuReads)
{
    const uint64_t page = info_.page_size;
    const uintptr_t base = uintptr_t(addr) & ~uintptr_t(page - 1);
    const uint64_t span = ((uintptr_t(addr) + bytes + page - 1) & ~uintptr_t(page - 1)) - base;
    gpuReads = gpuReads || (surfaceId && !importsWritten_.load(std::memory_order_relaxed));
    TegraGM20BAbiBoImport in = {base, span, surfaceId, gpuReads ? uint32_t(TegraGM20BImportGpuReads) : 0};
    TegraGM20BAbiBoAllocReply r = {};
    bo = {};
    if (call(TegraGM20BMethodBoImport, &in, sizeof in, &r, sizeof r) != KERN_SUCCESS)
        return false;
    bo.handle = r.handle;
    bo.bytes = span;
    return true;
}

bool Kernel::surfaceGpuWrites(uint32_t surfaceId)
{
    const TegraGM20BAbiSurfaceWrites in = {surfaceId, 0};
    if (call(TegraGM20BMethodSurfaceWrites, &in, sizeof in, nullptr, 0) == KERN_SUCCESS)
        return true;
    importsWritten_.store(true, std::memory_order_relaxed);
    return false;
}

bool Kernel::priority(uint32_t priority, uint32_t background)
{
    if (!(info_.flags & TegraGM20BInfoPriority))
        return false;
    const TegraGM20BAbiPriority in = {priority, background};
    return call(TegraGM20BMethodPriority, &in, sizeof in, nullptr, 0) == KERN_SUCCESS;
}

uint32_t Kernel::surfaceLayout(const Bo &bo, uint32_t width, uint32_t height, uint32_t bpp, uint32_t bytesPerRow,
                               bool writes, tgm_layout *canonical)
{
    const TegraGM20BAbiSurfaceLayout in = {
        bo.handle, width, height, bpp, bytesPerRow, writes ? uint32_t(TegraGM20BLayoutWrites) : 0, 0};
    if (canonical && (info_.flags & TegraGM20BInfoCanonicalSurfaceLayout)) {
        TegraGM20BAbiSurfaceLayoutInfo out = {};
        if (call(TegraGM20BMethodSurfaceLayoutInfo, &in, sizeof in, &out, sizeof out) != KERN_SUCCESS)
            return TegraGM20BLayoutOtherShape;
        if (out.layout == TegraGM20BLayoutTiled)
            *canonical = {out.width, out.height,          out.bpp,   out.pitch,
                          out.rows,  uint8_t(out.y_log2), out.align, out.bytes};
        return out.layout;
    }
    if (!(info_.flags & TegraGM20BInfoSurfaceLayout))
        return 0;
    TegraGM20BAbiSurfaceLayoutReply out = {};
    if (call(TegraGM20BMethodSurfaceLayout, &in, sizeof in, &out, sizeof out) != KERN_SUCCESS)
        return TegraGM20BLayoutOtherShape;
    if (canonical && out.layout == TegraGM20BLayoutTiled && !tgm_layout_tiled(width, height, bpp, canonical))
        return TegraGM20BLayoutOtherShape;
    return out.layout;
}

int Kernel::nativeSurfaceView(const Bo &source, Bo &view, TegraGM20BAbiNativeSurfaceViewReply &layout)
{
    layout = {};
    view = {};
    if (!exactJobs())
        return KERN_SUCCESS;
    const TegraGM20BAbiNativeSurfaceView in = {source.handle, 0, 0};
    const int result = call(TegraGM20BMethodNativeSurfaceView, &in, sizeof in, &layout, sizeof layout);
    if (result != KERN_SUCCESS || !layout.handle)
        return result;
    view.handle = layout.handle;
    view.bytes = layout.bytes;
    if (!bindImport(nullptr, view, layout.kind))
        return kIOReturnNoResources;
    return KERN_SUCCESS;
}

bool Kernel::bindImport(void *addr, Bo &bo, uint32_t kind)
{
    const uintptr_t base = uintptr_t(addr) & ~uintptr_t(info_.page_size - 1);
    bo.vaBase = allocVa(bo.bytes, kVaGranule);
    if (!bo.vaBase) {
        setError("out of GPU VA");
        free(bo);
        return false;
    }
    if (!bind(bo.vaBase, bo.handle, bo.bytes, kind)) {
        free(bo);
        return false;
    }
    bo.va = bo.vaBase + (uintptr_t(addr) - base);
    bo.cpu = static_cast<uint8_t *>(addr);
    return true;
}

uint64_t Kernel::reserveVa(uint64_t bytes)
{
    return allocVa(bytes, kVaGranule);
}

void Kernel::releaseVa(uint64_t va, uint64_t bytes)
{
    freeVa(va, bytes);
}

bool Kernel::bindImportAt(void *addr, Bo &bo, uint64_t va, uint32_t kind)
{
    if (!bind(va, bo.handle, bo.bytes, kind)) {
        free(bo);
        return false;
    }
    bo.vaBase = va;
    bo.vaReserved = true;
    bo.va = va + (uintptr_t(addr) & (info_.page_size - 1));
    bo.cpu = static_cast<uint8_t *>(addr);
    return true;
}

bool Kernel::bindAlias(const Bo &bo, uint64_t va, uint32_t kind)
{
    return bind(va, bo.handle, bo.bytes, kind);
}

void Kernel::free(Bo &bo)
{
    if (bo.token && bo.cpu)
        unmap(bo.token, uint64_t(uintptr_t(bo.cpu)));
    if (bo.handle) {
        TegraGM20BAbiBoFree f = {bo.handle};
        call(TegraGM20BMethodBoFree, &f, sizeof f, nullptr, 0);
    }
    if (bo.vaBase && !bo.vaReserved)
        freeVa(bo.vaBase, bo.bytes);
    bo = {};
}

bool Kernel::channel(uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence)
{
    std::lock_guard<std::mutex> l(lostLock_);
    if (engines == TegraGM20BEngineCopy)
        engines |= TegraGM20BEngineCopyOnly;
    TegraGM20BAbiChannelCreate in = {engines, 0};
    TegraGM20BAbiChannelCreateReply r = {};
    if (const int kr = call(TegraGM20BMethodChannelCreate, &in, sizeof in, &r, sizeof r); kr != KERN_SUCCESS) {
        setError(failed("ChannelCreate", kr));
        return false;
    }
    if (!fenceMap_ && !map(r.fence_token, true, fenceMap_, fenceMapBytes_)) {
        fenceMap_ = 0;
        fenceMapBytes_ = 0;
        closeChannel(r.channel);
        setError("fence map failed");
        return false;
    }
    const uint64_t recordBytes =
        sizeof(TegraGM20BAbiFence) + sizeof(TegraGM20BAbiFenceSemantic) + sizeof(TegraGM20BAbiFenceIdentity);
    if ((r.fence_offset & 7) || r.fence_offset > fenceMapBytes_ || recordBytes > fenceMapBytes_ - r.fence_offset) {
        closeChannel(r.channel);
        setError("fence record outside mapped range");
        return false;
    }
    channel = r.channel;
    fence = reinterpret_cast<const volatile uint32_t *>(uintptr_t(fenceMap_) + r.fence_offset);
    opened_.fetch_add(1, std::memory_order_release);
    uint32_t reused = r.channel;
    lostChannel_.compare_exchange_strong(reused, 0, std::memory_order_relaxed);
    return true;
}

void Kernel::closeChannel(uint32_t channel)
{
    const TegraGM20BAbiChannel in = {channel, 0};
    call(TegraGM20BMethodChannelDestroy, &in, sizeof in, nullptr, 0);
}

// A clientMemoryForType token mapped into this task: through the kext's own Map (method 12) where it has one, as NVK's
// nvkmd_darwin does then (a sandbox may admit io_connect_method and not io_connect_map_memory_into_task), else IOKit's.
bool Kernel::map(uint32_t token, bool readOnly, uint64_t &address, uint64_t &bytes)
{
    if (info_.flags & TegraGM20BInfoKernelMap) {
        const TegraGM20BAbiMap in = {token, readOnly ? uint32_t(TegraGM20BMapReadOnly) : 0u, 0};
        TegraGM20BAbiMapReply r = {};
        if (call(TegraGM20BMethodMap, &in, sizeof in, &r, sizeof r) != KERN_SUCCESS)
            return false;
        address = r.address;
        bytes = r.bytes;
        return true;
    }
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(conn_, token, mach_task_self(), &addr, &size,
                             kIOMapAnywhere | (readOnly ? kIOMapReadOnly : 0)) != KERN_SUCCESS)
        return false;
    address = addr;
    bytes = size;
    return true;
}

void Kernel::unmap(uint32_t token, uint64_t address)
{
    if (info_.flags & TegraGM20BInfoKernelMap) {
        const TegraGM20BAbiUnmap in = {token, 0, address};
        call(TegraGM20BMethodUnmap, &in, sizeof in, nullptr, 0);
        return;
    }
    IOConnectUnmapMemory64(conn_, token, mach_task_self(), mach_vm_address_t(address));
}

int Kernel::order(uint32_t channel, uint32_t flags, uint32_t seqno, uint32_t &waits)
{
    const TegraGM20BAbiOrder in = {channel, flags, seqno, 0};
    TegraGM20BAbiOrderReply r = {};
    const uint32_t at = opened();
    const int kr = call(TegraGM20BMethodOrder, &in, sizeof in, &r, sizeof r);
    if (kr == kIOReturnNotResponding)
        markLost(channel, at);
    waits = kr == KERN_SUCCESS ? r.waits : 0;
    return kr;
}

bool Kernel::cpuAccess(const Bo &bo, uint64_t offset, uint64_t bytes, uint32_t access, bool acquire)
{
    if (!(info_.flags & TegraGM20BInfoCpuAccess) || !bytes || offset > bo.bytes || bytes > bo.bytes - offset) {
        setError("CPU ownership unavailable or invalid range");
        return false;
    }
    const TegraGM20BAbiCpuAccess in = {bo.handle, offset, bytes, access, 0};
    const int kr = call(acquire ? TegraGM20BMethodCpuAcquire : TegraGM20BMethodCpuRelease, &in, sizeof in, nullptr, 0);
    if (kr != KERN_SUCCESS)
        setError(failed(acquire ? "CpuAcquire" : "CpuRelease", kr));
    return kr == KERN_SUCCESS;
}

CpuCompletionResult Kernel::completeCpu(const CpuCompletion &completion)
{
    return completeCpuRanges(completion, [&](const auto &request, auto &reply) {
        const int kr = call(TegraGM20BMethodCpuComplete, &request, sizeof request, &reply, sizeof reply);
        if (kr != KERN_SUCCESS)
            setError(failed("CpuComplete", kr));
        return kr == KERN_SUCCESS;
    });
}

bool Kernel::exec(uint32_t channel, const TegraGM20BAbiPush *pushes, uint32_t count, uint32_t last, uint32_t &seqno,
                  SurfaceList *surfaces, int *result, JobVisibility visibility, const JobEvents &events,
                  const JobAdmission &admission)
{
    if (result)
        *result = KERN_SUCCESS;
    if (count > maxPushes() || (surfaces && !surfaces->valid())) {
        setError(failed("Submit resource capacity", kIOReturnBadArgument));
        return false;
    }
    // Initial context packets retain the legacy path until their control BOs are named.
    const bool addressed = exactJobs() && surfaces;
    for (uint32_t i = 0; i < count; ++i)
        if ((pushes[i].flags & TegraGM20BPushFence) && (!addressed || !maxFences())) {
            if (result)
                *result = kIOReturnBadArgument;
            setError("The kernel cannot fence inside a job");
            return false;
        }
    constexpr uint32_t admissionFlags = TegraGM20BSubmitAdmissionBegin | TegraGM20BSubmitAdmissionContinue |
        TegraGM20BSubmitAdmissionEnd | TegraGM20BSubmitAdmissionCancel;
    if (!admission.empty() && (!addressed || !admission.cookie ||
        !(admission.flags & admissionFlags) || (admission.flags & ~admissionFlags))) {
        if (result)
            *result = kIOReturnBadArgument;
        setError("The kernel cannot retain this job's resource admission");
        return false;
    }
    if (!events.empty() && (!addressed ||
        events.waitCount > TegraGM20BMaxJobEvents || events.signalCount > TegraGM20BMaxJobEvents - events.waitCount ||
        (events.waitCount && !events.waits) || (events.signalCount && !events.signals))) {
        if (result)
            *result = kIOReturnBadArgument;
        setError("The kernel cannot represent this job's shared events");
        return false;
    }
    const bool explicitVisibility = addressed && visibility != JobVisibility::Legacy;
    TegraGM20BAbiExec legacyExec = {};
    size_t listBytes = 0;
    const TegraGM20BAbiExecSurfaces *list = nullptr;
    if (!addressed && surfaces && (info_.flags & TegraGM20BInfoExecSurfaces)) {
        list = surfaces->legacy(channel, listBytes);
        if (!list) {
            setError("The kernel cannot represent every resource dependency in this job");
            return false;
        }
    }
    const uint32_t nuses = addressed ? uint32_t(surfaces->uses().size()) : list ? list->count : 0;
    const size_t admissionOffset =
        sizeof(TegraGM20BAbiSubmit) + count * sizeof(pushes[0]) + nuses * sizeof(TegraGM20BAbiSurfaceUse);
    const size_t eventOffset = admissionOffset + (admission.empty() ? 0 : sizeof(TegraGM20BAbiSubmitAdmission));
    const size_t submitBytes = eventOffset + (events.empty() ? 0 : sizeof(TegraGM20BAbiSubmitEvents) +
        (events.waitCount + events.signalCount) * sizeof(TegraGM20BAbiEventUse));
    const bool atomic = addressed || ((info_.flags & TegraGM20BInfoSubmit) && submitBytes <= TegraGM20BMaxSubmitBytes);
    // Reused per submitting thread; no shared scratch lock is held across a channel's fence wait.
    thread_local std::vector<uint64_t> storage;
    if (atomic) {
        storage.resize((submitBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        uint32_t flags = addressed || list ? 0u : uint32_t(TegraGM20BExecSurfacesAll);
        flags |= admission.flags;
        if (explicitVisibility)
            flags |= TegraGM20BSubmitVisibilityExplicit |
                     (visibility == JobVisibility::External ? uint32_t(TegraGM20BSubmitExternalVisibility) : 0u);
        if (!events.empty())
            flags |= TegraGM20BSubmitHasEvents;
        const TegraGM20BAbiSubmit header = {channel, count, nuses, flags};
        auto *data = reinterpret_cast<uint8_t *>(storage.data());
        memcpy(data, &header, sizeof header);
        if (count)
            memcpy(data + sizeof header, pushes, count * sizeof pushes[0]);
        if (nuses)
            memcpy(data + sizeof header + count * sizeof pushes[0], addressed ? surfaces->uses().data() : list->uses,
                   nuses * sizeof(TegraGM20BAbiSurfaceUse));
        if (!explicitVisibility) {
            auto *uses = reinterpret_cast<TegraGM20BAbiSurfaceUse *>(data + sizeof header + count * sizeof pushes[0]);
            for (uint32_t i = 0; i < nuses; ++i)
                uses[i].access &= ~uint32_t(TegraGM20BSurfaceHostWritten);
        }
        if (count && !addressed && importsWritten_.load(std::memory_order_relaxed))
            reinterpret_cast<TegraGM20BAbiPush *>(data + sizeof header)[0].flags |= TegraGM20BPushImportsWritten;
        if (!admission.empty()) {
            const TegraGM20BAbiSubmitAdmission retained{admission.cookie, 0};
            memcpy(data + admissionOffset, &retained, sizeof retained);
        }
        if (!events.empty()) {
            const TegraGM20BAbiSubmitEvents eventHeader{events.waitCount, events.signalCount, 0};
            auto *tail = data + eventOffset;
            memcpy(tail, &eventHeader, sizeof eventHeader);
            tail += sizeof eventHeader;
            if (events.waitCount)
                memcpy(tail, events.waits, events.waitCount * sizeof events.waits[0]);
            tail += events.waitCount * sizeof events.waits[0];
            if (events.signalCount)
                memcpy(tail, events.signals, events.signalCount * sizeof events.signals[0]);
        }
    } else {
        if (count > TegraGM20BMaxPushes) {
            setError(failed("Exec", kIOReturnBadArgument));
            return false;
        }
        legacyExec.channel = channel;
        legacyExec.count = count;
        memcpy(legacyExec.pushes, pushes, count * sizeof pushes[0]);
        if (count && importsWritten_.load(std::memory_order_relaxed))
            legacyExec.pushes[0].flags |= TegraGM20BPushImportsWritten;
    }
    const TegraGM20BAbiSubmitAddress address = {uint64_t(reinterpret_cast<uintptr_t>(storage.data())),
                                                uint32_t(submitBytes), 0};
    TegraGM20BAbiExecReply reply = {};
    const uint32_t at = opened();
    for (;;) {
        int kr;
        if (addressed)
            kr = call(TegraGM20BMethodSubmitAddress, &address, sizeof address, &reply, sizeof reply);
        else if (atomic)
            kr = call(TegraGM20BMethodSubmit, storage.data(), submitBytes, &reply, sizeof reply);
        else {
            if (list) {
                kr = call(TegraGM20BMethodExecSurfaces, list, listBytes, nullptr, 0);
                if (kr != KERN_SUCCESS) {
                    setError(failed("ExecSurfaces", kr));
                    return false;
                }
            }
            kr = call(TegraGM20BMethodExec, &legacyExec, offsetof(TegraGM20BAbiExec, pushes) + count * sizeof pushes[0],
                      &reply, sizeof reply);
        }
        if (result)
            *result = kr;
        if (kr == KERN_SUCCESS) {
            seqno = reply.seqno;
            return true;
        }
        if (kr == kIOReturnNotResponding)
            markLost(channel, at);
        if (kr != kIOReturnBusy || !last) {
            setError(failed(addressed ? "SubmitAddress" : atomic ? "Submit" : "Exec", kr));
            return false;
        }
        if (!wait(channel, last))
            return false;
    }
}

// The kernel keeps a killed channel, and its cause, until ChannelDestroy (reopen), so GetStatus still answers for it here
bool Kernel::status(uint32_t channel, TegraGM20BAbiStatus &st)
{
    const TegraGM20BAbiChannel in = {channel, 0};
    st = {};
    if (call(TegraGM20BMethodGetStatus, &in, sizeof in, &st, sizeof st) == KERN_SUCCESS)
        return true;
    st = {};
    return false;
}

void Kernel::markLost(uint32_t channel, uint32_t opened)
{
    std::lock_guard<std::mutex> l(lostLock_);
    if (opened != opened_.load(std::memory_order_relaxed))
        return;
    TegraGM20BAbiStatus st;
    status(channel, st);  // none: the channel already destroyed (another thread's reopen), the cause unknown
    lostFaultVa_.store(st.fault_va, std::memory_order_relaxed);
    lostError_.store(st.error, std::memory_order_relaxed);
    lostChannel_.store(channel, std::memory_order_relaxed);
}

bool Kernel::wait(uint32_t channel, uint32_t seqno, uint64_t timeoutNs, int *result)
{
    TegraGM20BAbiWaitFence w = {channel, seqno, timeoutNs};
    const uint32_t at = opened();
    const int kr = call(TegraGM20BMethodWaitFence, &w, sizeof w, nullptr, 0);
    if (result)
        *result = kr;
    if (kr == kIOReturnNotResponding)
        markLost(channel, at);
    if (kr != KERN_SUCCESS) {
        setError("WaitFence failed");
        return false;
    }
    return true;
}

}  // namespace raw
