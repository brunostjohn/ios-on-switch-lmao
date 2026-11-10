// SPDX-License-Identifier: MIT
// A task's CPU mapping of an IOSurface, as IOSurface.kext makes it (TegraGM20BViews.h). IOSurfaceRootUserClient keeps an
// IOSurfaceClient per surface ID in a table under its own lock; the client maps the surface's descriptor into the task
// once (createMappingInTask, kIOMapAnywhere and the surface's cache mode) and hands userspace that address, which
// userspace keeps for the surface's life. A view replaces that range by another descriptor's mapping at the same address
// (kIOMapOverwrite | kIOMapUnique: IOKit enters it fixed over the old one and the task's other threads fault onto it;
// without kIOMapUnique IOKit would hand back the existing map), and later puts the surface's own back the same way.
// The new IOMemoryMap is released at once: an overwrite mapping's release leaves the range mapped. Offsets are the
// IPSW's (kits/SwitchABI: IOSurfaceRootUserClient, IOSurfaceClient).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <kern/locks.h>
#include <mach/vm_param.h>
#include <stdint.h>
#include "OriginalIOSurface.h"
#include "OriginalIOSurfaceClient.h"
#include "OriginalIOSurfaceRootUserClient.h"
#include "TegraGM20BViews.h"
#include "SwitchGpuSurfacePages.h"
#include "SwitchGpuSurfaceGeometry.h"
#include "SwitchGpuBacking.h"

// IOSurfaceRoot::lookupSurface(id, task), by address (kits/SwitchABI IOSurface, bound through TegraVIC's bindings)
extern "C" OSObject *tegra_gm20b_ios_lookup(IOService *root, uint32_t id, task_t task);
extern "C" IOMemoryDescriptor *tegra_vic_ios_memory(OSObject *surface);
extern "C" uint64_t tegra_vic_ios_alloc_size(OSObject *surface);
extern "C" uint64_t tegra_vic_ios_width(OSObject *surface);
extern "C" uint64_t tegra_vic_ios_height(OSObject *surface);
extern "C" uint64_t tegra_vic_ios_bytes_per_row(OSObject *surface);
extern "C" uint32_t tegra_vic_ios_plane_count(OSObject *surface);
extern "C" uint32_t tegra_gm20b_ios_bpe(OSObject *surface);
extern "C" uint32_t tegra_gm20b_ios_element_width(OSObject *surface);
extern "C" uint32_t tegra_gm20b_ios_element_height(OSObject *surface);

namespace
{
template <class T> T at(const void *object, unsigned long offset)
{
	T value;
	// NOLINTNEXTLINE(bugprone-sizeof-expression): T may be a pointer; the field is that pointer
	memcpy(&value, static_cast<const uint8_t *>(object) + offset, sizeof(value));
	return value;
}

// The user client's IOSurfaceClient of surface `id`, retained (under the mutex the table's lock pointer names), or null
OSObject *surfaceClient(const void *client, uint32_t id)
{
	using UC = OriginalIOSurfaceRootUserClient;
	auto *lock = at<lck_mtx_t *>(client, UC::clientTableLock);
	OSObject *c = nullptr;
	lck_mtx_lock(lock);
	if (id < at<uint32_t>(client, UC::clientCapacity)) {
		auto *const *table = at<OSObject **>(client, UC::clientTable);
		c = table ? table[id] : nullptr;
		if (c && at<void *>(c, OriginalIOSurfaceClient::sharedClient) !=
				 at<void *>(client, UC::sharedClient))
			c = nullptr;
		if (c)
			c->retain();
	}
	lck_mtx_unlock(lock);
	return c;
}
}  // namespace

extern "C" int tgm_cpu_view_of(void *client, uint32_t id, struct tgm_cpu_view *out)
{
	const OSObject *c = surfaceClient(client, id);
	if (!c)
		return 0;
	IOMemoryMap *map = at<IOMemoryMap *>(c, OriginalIOSurfaceClient::map);
	const IOMemoryDescriptor *md = map ? map->getMemoryDescriptor() : nullptr;
	int mapped = map ? 1 : 0;
	// IOSurface maps the whole descriptor (createMappingInTask with no offset or length): the view puts back the same
	if (map && (!md || map->getLength() != md->getLength())) {
		mapped = -1;
	} else if (map) {
		*out = {client, map->getAddressTask(), map->getAddress(), map->getLength(),
			0,      map->getMapOptions()};
	}
	c->release();
	return mapped;
}

// A prefaulted mapping enters every page's entries in the call (the scratch is prepared): a task's first touch of each of
// its 16 KiB pages would otherwise take a soft fault, an estimated 1 ms for a 720x1280 surface
extern "C" int tgm_cpu_view_remap(const struct tgm_cpu_view *view, void *memory, uint64_t offset,
				  int prefault)
{
	auto *md = static_cast<IOMemoryDescriptor *>(memory);
	if (!md || md->getLength() < offset + view->length)
		return -22;
	const IOOptionBits options = (view->options & ~uint32_t(kIOMapAnywhere)) | kIOMapOverwrite |
				     kIOMapUnique | (prefault ? kIOMapPrefault : 0);
	const IOMemoryMap *map = md->createMappingInTask(
		static_cast<task_t>(view->task), view->address, options, offset, view->length);
	if (!map)
		return -12;
	map->release();
	return 0;
}

// NOLINTNEXTLINE(misc-const-correctness): TegraGM20BGpu.h's declaration, which C calls
extern "C" void *tgm_surface_memory(void *client, uint32_t id)
{
	const OSObject *c = surfaceClient(client, id);
	if (!c)
		return nullptr;
	IOMemoryMap *map = at<IOMemoryMap *>(c, OriginalIOSurfaceClient::map);
	IOMemoryDescriptor *md = map ? map->getMemoryDescriptor() : nullptr;
	if (md)
		md->retain();
	c->release();
	return md;
}

extern "C" OSObject *tgm_surface_lookup(uint32_t id, void *task)
{
	static IOService *root;  // kept
	if (!__atomic_load_n(&root, __ATOMIC_ACQUIRE)) {
		OSDictionary *match = IOService::nameMatching("IOCoreSurfaceRoot");
		// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap below stores it
		IOService *found = match ? IOService::copyMatchingService(match) : nullptr;
		OSSafeReleaseNULL(match);
		// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap writes the current value back into it
		IOService *expected = nullptr;
		if (found && !__atomic_compare_exchange_n(&root, &expected, found, false,
							  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			found->release();
	}
	// NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): root keeps the matched service for good
	IOService *r = __atomic_load_n(&root, __ATOMIC_ACQUIRE);
	return r ? tegra_gm20b_ios_lookup(r, id, static_cast<task_t>(task)) : nullptr;
}

// A surface IOSurface made over its creator's own memory: that task reads and writes the pages without IOSurfaceLock, so
// they hold rows for good (TegraGM20BLayout.h). One this task cannot look up is taken as one too: rows are always right.
extern "C" int tgm_surface_client_memory(uint32_t id, void *task)
{
	const OSObject *surface = tgm_surface_lookup(id, task);
	if (!surface)
		return 1;
	const int client =
		static_cast<int>(at<uint64_t>(surface, OriginalIOSurface::clientMemory) != 0);
	surface->release();
	return client;
}

namespace {
SwitchGpuSurfaceMatch surfacePages(OSObject *surface, const uint64_t *pages, uint32_t count)
{
	IOMemoryDescriptor *memory = tegra_vic_ios_memory(surface);
	const uint64_t base = at<uint64_t>(surface, OriginalIOSurface::baseOffset);
	const uint64_t bytes = tegra_vic_ios_alloc_size(surface);
	if (!memory || memory->prepare() != kIOReturnSuccess)
		return SwitchGpuSurfaceMatch::Physical;
	const auto result = switch_gpu_surface_page_vector(memory->getLength(), base, bytes, PAGE_SIZE, pages, count,
		[&](uint64_t offset, uint64_t &length) {
			IOByteCount span = 0;
			const addr64_t pa = memory->getPhysicalSegment(offset, &span, kIOMemoryMapperNone);
			length = span;
			return uint64_t(pa);
		});
	memory->complete();
	return result;
}
}

extern "C" OSObject *switch_gpu_surface_hold(uint32_t id, void *task, const uint64_t *pages, uint32_t count)
{
	OSObject *surface = tgm_surface_lookup(id, task);
	if (!surface)
		return nullptr;
	const auto match = surfacePages(surface, pages, count);
	if (match != SwitchGpuSurfaceMatch::Match) {
		auto *memory = tegra_vic_ios_memory(surface);
		IOLog("SwitchGPU: IOSurface %u import rejected (%u): %u pages, allocation %llu at %llu, descriptor %llu bytes\n",
			id, unsigned(match), count, tegra_vic_ios_alloc_size(surface),
			at<uint64_t>(surface, OriginalIOSurface::baseOffset), memory ? memory->getLength() : 0);
	}
	if (match != SwitchGpuSurfaceMatch::Match) {
		surface->release();
		return nullptr;
	}
	return surface;
}

extern "C" int switch_gpu_surface_pages_match(uint32_t id, void *task, const uint64_t *pages, uint32_t count)
{
	auto *surface = switch_gpu_surface_hold(id, task, pages, count);
	if (!surface)
		return 0;
	surface->release();
	return 1;
}

extern "C" int switch_gpu_surface_geometry(uint32_t id, void *task, const uint64_t *pages, uint32_t count,
	SwitchGpuSurfaceGeometry *geometry)
{
	OSObject *surface = tgm_surface_lookup(id, task);
	if (!surface)
		return -22;
	const auto match = surfacePages(surface, pages, count);
	if (match == SwitchGpuSurfaceMatch::Match)
		*geometry = {tegra_vic_ios_width(surface), tegra_vic_ios_height(surface),
			tegra_vic_ios_bytes_per_row(surface), tegra_vic_ios_alloc_size(surface),
			tegra_gm20b_ios_bpe(surface), tegra_gm20b_ios_element_width(surface),
			tegra_gm20b_ios_element_height(surface), tegra_vic_ios_plane_count(surface),
			at<uint64_t>(surface, OriginalIOSurface::clientMemory) != 0};
	surface->release();
	return match == SwitchGpuSurfaceMatch::Match ? 0 : -22;
}

extern "C" int switch_gpu_surface_backing(uint32_t id, void *task, switch_gpu_backing **out)
{
	*out = nullptr;
	// A numeric ID can already belong to a new IOSurface while an older allocation retires.
	auto *hint = switch_gpu_backing_find_surface(id);
	if (!hint)
		return 0;
	switch_gpu_backing_release(hint);
	OSObject *surface = tgm_surface_lookup(id, task);
	if (!surface)
		return 0;
	auto *memory = tegra_vic_ios_memory(surface);
	const uint64_t base = at<uint64_t>(surface, OriginalIOSurface::baseOffset);
	const bool prepared = memory && base < memory->getLength() && memory->prepare() == kIOReturnSuccess;
	int result = prepared ? 0 : -22;
	if (prepared) {
		IOByteCount length = 0;
		const uint64_t pa = memory->getPhysicalSegment(base, &length, kIOMemoryMapperNone);
		auto *backing = pa && length ? switch_gpu_backing_find_pa(pa) : nullptr;
		if (backing) {
			switch_gpu_backing_info info = {};
			switch_gpu_backing_describe(backing, &info);
			if (surfacePages(surface, info.pages, info.page_count) == SwitchGpuSurfaceMatch::Match)
				*out = backing;
			else {
				switch_gpu_backing_release(backing);
				result = -22;
			}
		}
		memory->complete();
	}
	surface->release();
	return result;
}

extern "C" void tgm_memory_release(void *memory)
{
	if (memory)
		static_cast<OSObject *>(memory)->release();
}

extern "C" int tgm_cpu_view_live(const struct tgm_cpu_view *view)
{
	return static_cast<int>(!static_cast<IOService *>(view->client)->isInactive());
}

// NOLINTNEXTLINE(misc-const-correctness): the host tests' stubs and the C callers share this signature
extern "C" void tgm_cpu_view_hold(struct tgm_cpu_view *view, int hold)
{
	if (hold) {
		static_cast<OSObject *>(view->client)->retain();
	} else {
		static_cast<OSObject *>(view->client)->release();
	}
}
