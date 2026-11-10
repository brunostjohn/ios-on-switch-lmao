// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "SwitchGpuCopyMappings.h"
#include "SwitchGpuBacking.h"

namespace
{
constexpr uint64_t Page = 4096;

void discard(SwitchGpuCopyMapping &entry)
{
	if (!entry.descriptor)
		return;
	switch_gpu_relayout_mapping_put(entry.mapping);
	entry.descriptor->complete();
	entry.descriptor->release();
	entry = {};
}

SwitchGpuCopyMapping *oldest(SwitchGpuCopyMappings *cache)
{
	SwitchGpuCopyMapping *found = nullptr;
	for (auto &entry : cache->descriptors)
		if (entry.descriptor && !entry.users && (!found || entry.lastUse < found->lastUse))
			found = &entry;
	return found;
}

void releaseDescriptor(void *owner)
{
	auto *descriptor = static_cast<IOMemoryDescriptor *>(owner);
	descriptor->complete();
	descriptor->release();
}

bool contains(const SwitchGpuCopyMapping &entry, uint64_t descriptorOffset, uint64_t backingOffset,
	      uint64_t bytes)
{
	if (descriptorOffset < entry.descriptorStart || backingOffset < entry.backingStart)
		return false;
	const uint64_t relative = descriptorOffset - entry.descriptorStart;
	return relative == backingOffset - entry.backingStart && relative <= entry.length &&
	       bytes <= entry.length - relative;
}
}

int switch_gpu_copy_mapping(tgm_vm *vm, SwitchGpuCopyMappings *cache,
			    IOMemoryDescriptor *descriptor, uint64_t descriptorOffset,
			    switch_gpu_backing *backing, uint64_t backingOffset, uint64_t bytes,
			    bool tiled, SwitchGpuCopyMapping **out)
{
	*out = nullptr;
	if (!descriptor || !backing || !bytes || ((descriptorOffset ^ backingOffset) & (Page - 1)))
		return -22;
	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(backing, &info);
	const uint64_t backingBytes = uint64_t(info.page_count) * Page;
	const uint64_t descriptorBytes = descriptor->getLength();
	if (descriptorOffset > descriptorBytes || bytes > descriptorBytes - descriptorOffset ||
	    backingOffset > backingBytes || bytes > backingBytes - backingOffset)
		return -22;
	for (auto &entry : cache->descriptors) {
		auto *map = entry.mapping;
		if (entry.descriptor == descriptor && map && map->backing == backing &&
		    map->allocation == info.allocation_id &&
		    map->generation == info.allocation_generation && (!tiled || map->tiled) &&
		    contains(entry, descriptorOffset, backingOffset, bytes)) {
			++entry.users;
			entry.lastUse = ++cache->clock;
			*out = &entry;
			return 0;
		}
	}
	SwitchGpuCopyMapping *entry = nullptr;
	for (auto &candidate : cache->descriptors)
		if (!candidate.descriptor) {
			entry = &candidate;
			break;
		}
	if (!entry)
		entry = oldest(cache);
	if (!entry)
		return -16;
	discard(*entry);
	if (descriptor->prepare() != kIOReturnSuccess)
		return -22;
	descriptor->retain();
	entry->descriptor = descriptor;
	entry->users = 1;
	entry->descriptorStart =
		descriptorOffset > backingOffset ? descriptorOffset - backingOffset : 0;
	entry->backingStart =
		backingOffset > descriptorOffset ? backingOffset - descriptorOffset : 0;
	entry->length = descriptorBytes - entry->descriptorStart;
	if (entry->length > backingBytes - entry->backingStart)
		entry->length = backingBytes - entry->backingStart;
	for (uint64_t offset = 0; offset < entry->length; offset += Page) {
		const uint64_t physical = descriptor->getPhysicalSegment(
			entry->descriptorStart + offset, nullptr, kIOMemoryMapperNone);
		if (!physical || physical != info.pages[(entry->backingStart + offset) / Page]) {
			discard(*entry);
			return -22;
		}
	}
	int result;
	while ((result = switch_gpu_relayout_mapping_backing(vm, &cache->allocations, backing,
							     tiled, &entry->mapping)) == -16) {
		auto *unused = oldest(cache);
		if (!unused || unused == entry)
			break;
		discard(*unused);
	}
	if (result) {
		discard(*entry);
		return result;
	}
	entry->users = 1;
	entry->lastUse = ++cache->clock;
	*out = entry;
	return 0;
}

void switch_gpu_copy_mapping_put(SwitchGpuCopyMapping *mapping)
{
	if (mapping)
		--mapping->users;
}

void switch_gpu_copy_mappings_clear(tgm_vm *vm, SwitchGpuCopyMappings *cache)
{
	for (auto &entry : cache->descriptors)
		if (!entry.users)
			discard(entry);
	switch_gpu_relayout_cache_clear(vm, &cache->allocations);
}

switch_gpu_backing *switch_gpu_copy_register(IOMemoryDescriptor *descriptor)
{
	const uint64_t length = descriptor ? descriptor->getLength() : 0;
	if (!length || length > SwitchGpuRelayoutCache::MaximumBytes ||
	    descriptor->prepare() != kIOReturnSuccess)
		return nullptr;
	descriptor->retain();
	const uint32_t count = uint32_t((length + Page - 1) / Page);
	auto *pages = IONew(uint64_t, count);
	if (!pages) {
		releaseDescriptor(descriptor);
		return nullptr;
	}
	bool valid = true;
	for (uint32_t i = 0; i < count; ++i) {
		pages[i] = descriptor->getPhysicalSegment(uint64_t(i) * Page, nullptr,
							  kIOMemoryMapperNone);
		valid &= pages[i] && !(pages[i] & (Page - 1));
	}
	auto *backing =
		valid ? switch_gpu_backing_register(pages, count, 0, descriptor, releaseDescriptor)
		      : nullptr;
	IODelete(pages, uint64_t, count);
	if (!backing)
		releaseDescriptor(descriptor);
	return backing;
}
