// SPDX-License-Identifier: MIT
#pragma once

/* Sysmem is mapped volatile (GPU L2 does not keep its lines): CONTRACT.md §3(b) option one; cacheable with
 * l2_on(). A compressible kind maps with 128 KiB pages and the memory's comptags (TegraGM20BComp.h) or is refused: a map
 * demoted to its uncompressed kind, as nvgpu demotes one, would look compressed to its caller, who would go on clearing
 * and resolving it as compressed. */
static int vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		  uint64_t offset, uint8_t kind, int read_only, int vol, int gpu_ordered,
		  uint8_t *page_shift)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol, .ro = !!read_only, .priv = 0, .kind = kind};
	u8 shift = PAGE_SHIFT;
	int ret = 0;
	int got = 0;
	u32 ordered_scope = 0;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range ||
	    offset > mem->bytes || range > mem->bytes - offset ||
	    !nvif_mmu_kind_valid(&gpu.mmu, kind))
		return -EINVAL;
	if (gpu.mmu.kind[kind] != kind) {
		if (!tgm_comp_bind_ok(va, offset, range, mem->page))
			return -EINVAL;
		shift = TGM_COMP_SHIFT;
	}
	gate_enter();
	mutex_lock(&vm->map_lock);
	if (gpu_ordered)
		ordered_scope = nvxnu_gpu_ordered_begin();
	ret = nvif_vmm_raw_get(&vm->vmm, va, range, shift);
	got = !ret;
	if (got) {
		ret = nvif_vmm_raw_map(&vm->vmm, va, range, shift, &args, sizeof(args), &mem->mem,
				       offset);
	}
	if (gpu_ordered)
		nvxnu_gpu_ordered_end(ordered_scope);
	if (gpu_ordered && !ret)
		tgm_tlb_mapped(&vm->tlb);
	if (ret) {
		/* removals are synchronous (TegraGM20BTlb.h): a failed get may have dropped page tables it made */
		if (got) {
			nvif_vmm_raw_put(&vm->vmm, va, range, shift);
		} else if (gpu_ordered) {
			mmu_invalidate_all();
		}
	}
	mutex_unlock(&vm->map_lock);
	gate_leave();
	if (!ret && page_shift)
		*page_shift = shift;
	return ret;
}

int tgm_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem, uint64_t offset,
	       uint8_t kind, int read_only)
{
	return vm_map(vm, va, range, mem, offset, kind, read_only, !l2_on(), 0, NULL);
}

int tgm_vm_map_ordered(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		       uint64_t offset, uint8_t kind, int read_only, uint8_t *page_shift)
{
	if (!vm->owner)
		return -EINVAL;
	return vm_map(vm, va, range, mem, offset, kind, read_only, !l2_on(), 1, page_shift);
}

int tgm_vm_unmap_pages(struct tgm_vm *vm, uint64_t va, uint64_t range, uint8_t page_shift)
{
	int ret = 0;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range)
		return -EINVAL;
	gate_enter();
	mutex_lock(&vm->map_lock);
	nvkm_ltc_flush(gpu.device->ltc);
	ret = nvif_vmm_raw_unmap(&vm->vmm, va, range, page_shift, false);
	nvif_vmm_raw_put(&vm->vmm, va, range, page_shift);
	mutex_unlock(&vm->map_lock);
	gate_leave();
	return ret;
}

int tgm_vm_unmap(struct tgm_vm *vm, uint64_t va, uint64_t range)
{
	return tgm_vm_unmap_pages(vm, va, range, PAGE_SHIFT);
}

static void mem_prepare_big_views(struct tgm_mem *mem)
{
	u32 at = 0;
	if (mem->big_views_ready)
		return;
	mem->big_views_ready = true;
	if (mem->page >= SWITCH_GPU_BIG_SHIFT)
		return;
	while (at <= mem->npages && SWITCH_GPU_BIG_PAGES <= mem->npages - at) {
		u32 first = at;
		struct switch_gpu_memory_view *view;
		struct nvif_mem_ram_v0 args = {.version = 0};
		int ret;
		if (!switch_gpu_mapping_big_page(mem->pages, mem->npages, at)) {
			++at;
			continue;
		}
		do {
			at += SWITCH_GPU_BIG_PAGES;
		} while (switch_gpu_mapping_big_page(mem->pages, mem->npages, at));
		view = kzalloc(sizeof(*view), GFP_KERNEL);
		if (!view)
			continue;
		view->offset = (u64)first << PAGE_SHIFT;
		view->bytes = (u64)(at - first) << PAGE_SHIFT;
		args.dma = (dma_addr_t *)(mem->pages + first);
		ret = nvif_mem_ctor_type(&gpu.mmu, "big-span", NVIF_CLASS_MEM_GF100, gpu.mem_type,
					 SWITCH_GPU_BIG_SHIFT, view->bytes, &args, sizeof(args),
					 &view->mem);
		if (ret) {
			kfree(view);
			continue;
		}
		view->next = mem->big_views;
		mem->big_views = view;
	}
}

static struct nvif_mem *mem_mapping_view(struct tgm_mem *mem,
					 struct switch_gpu_mapping_entry *entry, u64 *offset)
{
	struct switch_gpu_memory_view *view;
	*offset = entry->offset;
	if (entry->shift == PAGE_SHIFT || mem->page >= entry->shift)
		return &mem->mem;
	for (view = mem->big_views; view; view = view->next) {
		if (entry->offset >= view->offset && entry->offset - view->offset <= view->bytes &&
		    entry->bytes <= view->bytes - (entry->offset - view->offset)) {
			*offset -= view->offset;
			return &view->mem;
		}
	}
	entry->shift = PAGE_SHIFT;
	return &mem->mem;
}

int switch_gpu_vm_map_volatile(struct tgm_vm *vm, u64 va, u64 range, struct tgm_mem *mem, u64 offset,
		      u8 kind, int read_only, int vol, struct switch_gpu_mapping **mapping)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol || !l2_on(), .ro = !!read_only, .kind = kind};
	struct switch_gpu_mapping_run run;
	struct switch_gpu_mapping *made;
	u64 at = 0;
	u32 count = 0, got = 0, i, ordered_scope;
	int ret = 0, failed_get = 0;
	bool compressed;
	if (!mapping || !vm || !mem || !vm->owner || !range ||
	    ((va | range | offset) & (PAGE_SIZE - 1)) || va < TGM_USER_VA_START ||
	    range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range || offset > mem->bytes ||
	    range > mem->bytes - offset || !nvif_mmu_kind_valid(&gpu.mmu, kind))
		return -EINVAL;
	*mapping = NULL;
	compressed = gpu.mmu.kind[kind] != kind;
	if ((vol && kind) || (compressed && !tgm_comp_bind_ok(va, offset, range, mem->page)))
		return -EINVAL;
	while (at < range) {
		if (compressed)
			run = (struct switch_gpu_mapping_run){range, SWITCH_GPU_BIG_SHIFT};
		else if (!switch_gpu_mapping_next(mem->pages, mem->npages, va + at, offset + at,
						  range - at, &run))
			return -EINVAL;
		++count;
		at += run.bytes;
	}
	made = kzalloc(sizeof(*made) + count * sizeof(made->entries[0]), GFP_KERNEL);
	if (!made)
		return -ENOMEM;
	made->count = count;
	at = 0;
	for (i = 0; i < count; ++i) {
		if (compressed)
			run = (struct switch_gpu_mapping_run){range, SWITCH_GPU_BIG_SHIFT};
		else
			switch_gpu_mapping_next(mem->pages, mem->npages, va + at, offset + at,
						range - at, &run);
		made->entries[i] = (struct switch_gpu_mapping_entry){va + at, offset + at,
								     run.bytes, run.shift};
		at += run.bytes;
	}
	gate_enter();
	mutex_lock(&vm->map_lock);
	ordered_scope = nvxnu_gpu_ordered_begin();
	for (i = 0; i < count; ++i) {
		struct switch_gpu_mapping_entry *entry = &made->entries[i];
		u64 memory_offset;
		struct nvif_mem *memory = mem_mapping_view(mem, entry, &memory_offset);
		ret = nvif_vmm_raw_get(&vm->vmm, entry->va, entry->bytes, entry->shift);
		if (ret) {
			failed_get = 1;
			break;
		}
		++got;
		ret = nvif_vmm_raw_map(&vm->vmm, entry->va, entry->bytes, entry->shift, &args,
				       sizeof(args), memory, memory_offset);
		if (ret)
			break;
	}
	nvxnu_gpu_ordered_end(ordered_scope);
	if (ret) {
		for (i = 0; i < got; ++i) {
			const struct switch_gpu_mapping_entry *entry = &made->entries[i];
			nvif_vmm_raw_unmap(&vm->vmm, entry->va, entry->bytes, entry->shift, false);
			nvif_vmm_raw_put(&vm->vmm, entry->va, entry->bytes, entry->shift);
		}
		if (failed_get)
			mmu_invalidate_all();
	} else {
		tgm_tlb_mapped(&vm->tlb);
	}
	mutex_unlock(&vm->map_lock);
	gate_leave();
	if (ret)
		kfree(made);
	else
		*mapping = made;
	return ret;
}

int switch_gpu_vm_map(struct tgm_vm *vm, u64 va, u64 range, struct tgm_mem *mem, u64 offset,
		      u8 kind, int read_only, struct switch_gpu_mapping **mapping)
{
	return switch_gpu_vm_map_volatile(vm, va, range, mem, offset, kind, read_only, 0, mapping);
}

void switch_gpu_vm_unmap(struct tgm_vm *vm, struct switch_gpu_mapping **mapping)
{
	struct switch_gpu_mapping *made = *mapping;
	u32 i;
	if (!made)
		return;
	gate_enter();
	mutex_lock(&vm->map_lock);
	nvkm_ltc_flush(gpu.device->ltc);
	for (i = 0; i < made->count; ++i) {
		const struct switch_gpu_mapping_entry *entry = &made->entries[i];
		nvif_vmm_raw_unmap(&vm->vmm, entry->va, entry->bytes, entry->shift, false);
		nvif_vmm_raw_put(&vm->vmm, entry->va, entry->bytes, entry->shift);
	}
	mutex_unlock(&vm->map_lock);
	gate_leave();
	kfree(made);
	*mapping = NULL;
}

static struct switch_gpu_mapping *mapping_alloc(u32 count)
{
	struct switch_gpu_mapping *made = NULL;
	if (count) {
		made = kzalloc(sizeof(*made) + count * sizeof(made->entries[0]), GFP_KERNEL);
		if (!made)
			return NULL;
		made->count = 0;
	}
	return made;
}

static void mapping_add(struct switch_gpu_mapping *part, u64 va, u64 offset, u64 bytes, u8 shift)
{
	part->entries[part->count++] = (struct switch_gpu_mapping_entry){va, offset, bytes, shift};
}

int switch_gpu_vm_split(struct tgm_vm *vm, struct switch_gpu_mapping **mapping, struct tgm_mem *mem,
			u8 kind, int read_only, int vol, u64 va, u64 range,
			struct switch_gpu_mapping **below, struct switch_gpu_mapping **taken,
			struct switch_gpu_mapping **above)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol || !l2_on(), .ro = !!read_only, .kind = kind};
	struct switch_gpu_mapping *old = mapping ? *mapping : NULL;
	struct switch_gpu_mapping *parts[3] = {NULL, NULL, NULL};
	struct switch_gpu_mapping_entry lost[2];
	u32 counts[3] = {0, 0, 0}, failed[2] = {0, 0}, i, p, ordered_scope;
	const u64 end = va + range;
	bool cut_big = false;
	if (!old || !vm || !mem || !range || ((va | range) & (PAGE_SIZE - 1)) || end < va)
		return -EINVAL;
	for (i = 0; i < old->count; ++i) {
		const struct switch_gpu_mapping_entry *e = &old->entries[i];
		const u64 e_end = e->va + e->bytes;
		const bool cut = e->va < end && va < e_end && (e->va < va || e_end > end);
		if (cut && e->shift != PAGE_SHIFT && gpu.mmu.kind[kind] != kind)
			return -EINVAL;
		cut_big |= cut && e->shift != PAGE_SHIFT;
		counts[0] += e->va < va;
		counts[2] += e_end > end;
		counts[1] += e->va < end && va < e_end && (e->shift == PAGE_SHIFT || !cut);
	}
	for (p = 0; p < 3; ++p) {
		parts[p] = mapping_alloc(counts[p]);
		if (counts[p] && !parts[p]) {
			while (p--)
				kfree(parts[p]);
			return -ENOMEM;
		}
	}
	for (i = 0; i < old->count; ++i) {
		const struct switch_gpu_mapping_entry *e = &old->entries[i];
		const u64 e_end = e->va + e->bytes;
		const bool cut = e->va < end && va < e_end && (e->va < va || e_end > end);
		const u8 kept = cut ? PAGE_SHIFT : e->shift;
		const u64 low = e->va > va ? e->va : va, high = e_end < end ? e_end : end;
		if (e->va < va)
			mapping_add(parts[0], e->va, e->offset, (e_end < va ? e_end : va) - e->va,
				    kept);
		if (e->va < end && va < e_end && (e->shift == PAGE_SHIFT || !cut))
			mapping_add(parts[1], low, e->offset + (low - e->va), high - low, e->shift);
		if (e_end > end) {
			const u64 from = e->va > end ? e->va : end;
			mapping_add(parts[2], from, e->offset + (from - e->va), e_end - from, kept);
		}
	}
	if (cut_big) {
		gate_enter();
		mutex_lock(&vm->map_lock);
		nvkm_ltc_flush(gpu.device->ltc);
		for (i = 0; i < old->count; ++i) {
			const struct switch_gpu_mapping_entry *e = &old->entries[i];
			if (e->shift == PAGE_SHIFT || !(e->va < end && va < e->va + e->bytes) ||
			    (e->va >= va && e->va + e->bytes <= end))
				continue;
			nvif_vmm_raw_unmap(&vm->vmm, e->va, e->bytes, e->shift, false);
			nvif_vmm_raw_put(&vm->vmm, e->va, e->bytes, e->shift);
		}
		ordered_scope = nvxnu_gpu_ordered_begin();
		for (p = 0; p < 3; p += 2) {
			for (i = 0; parts[p] && i < parts[p]->count; ++i) {
				struct switch_gpu_mapping_entry *e = &parts[p]->entries[i];
				const bool edge =
					(p == 0 ? e->va + e->bytes == va : e->va == end) &&
					e->shift == PAGE_SHIFT;
				u32 k;
				for (k = 0; edge && k < old->count; ++k)
					if (old->entries[k].shift != PAGE_SHIFT &&
					    old->entries[k].va <= e->va &&
					    e->va < old->entries[k].va + old->entries[k].bytes)
						break;
				if (!edge || k == old->count)
					continue;
				failed[p / 2] =
					nvif_vmm_raw_get(&vm->vmm, e->va, e->bytes, PAGE_SHIFT) ? 1
												: 0;
				if (!failed[p / 2] &&
				    nvif_vmm_raw_map(&vm->vmm, e->va, e->bytes, PAGE_SHIFT, &args,
						     sizeof(args), &mem->mem, e->offset))
					failed[p / 2] = 2;
				if (failed[p / 2])
					lost[p / 2] = *e;
			}
		}
		nvxnu_gpu_ordered_end(ordered_scope);
		/* a remainder that could not be mapped again stays unmapped; its part no longer covers it */
		for (p = 0; p < 2; ++p) {
			struct switch_gpu_mapping *part = parts[p * 2];
			if (!failed[p])
				continue;
			if (failed[p] == 2) {
				nvif_vmm_raw_unmap(&vm->vmm, lost[p].va, lost[p].bytes, PAGE_SHIFT,
						   false);
				nvif_vmm_raw_put(&vm->vmm, lost[p].va, lost[p].bytes, PAGE_SHIFT);
			}
			for (i = 0; i < part->count; ++i)
				if (part->entries[i].va == lost[p].va) {
					memmove(&part->entries[i], &part->entries[i + 1],
						(part->count - i - 1) * sizeof(part->entries[0]));
					--part->count;
					break;
				}
		}
		tgm_tlb_mapped(&vm->tlb);
		mutex_unlock(&vm->map_lock);
		gate_leave();
	}
	for (p = 0; p < 3; ++p) {
		if (parts[p] && !parts[p]->count) {
			kfree(parts[p]);
			parts[p] = NULL;
		}
	}
	kfree(old);
	*mapping = NULL;
	*below = parts[0];
	*taken = parts[1];
	*above = parts[2];
	return 0;
}

int switch_gpu_mapping_span(const struct switch_gpu_mapping *mapping, u64 *va, u64 *bytes)
{
	if (!mapping || !mapping->count)
		return -EINVAL;
	*va = mapping->entries[0].va;
	*bytes = mapping->entries[mapping->count - 1].va +
		 mapping->entries[mapping->count - 1].bytes - *va;
	return 0;
}

void switch_gpu_mapping_describe(const struct switch_gpu_mapping *mapping, char *out, size_t size)
{
	size_t at = 0;
	u32 i;
	if (!size)
		return;
	out[0] = 0;
	for (i = 0; mapping && i < mapping->count && at + 1 < size; ++i) {
		const struct switch_gpu_mapping_entry *e = &mapping->entries[i];
		const int n =
			snprintf(out + at, size - at, "%s%#llx+%#llx/%u", i ? " " : "",
				 (unsigned long long)e->va, (unsigned long long)e->bytes, e->shift);
		if (n < 0)
			break;
		at += (size_t)n < size - at ? (size_t)n : size - at - 1;
	}
}

int tgm_vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, uint64_t *va)
{
	return vm_kmap(vm, mem, bytes, 0, va);
}

static int vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, int read_only,
		   uint64_t *va)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = 1, .ro = !!read_only, .priv = 0, .kind = 0};
	struct tgm_kmap *k = kzalloc(sizeof(*k), GFP_KERNEL);
	int ret = 0;
	if (!k)
		return -ENOMEM;
	gate_enter();
	mutex_lock(&vm->map_lock);
	ret = nvif_vmm_get(&vm->vmm, ADDR, false, PAGE_SHIFT, 0, bytes, &k->vma);
	if (!ret) {
		ret = nvif_vmm_map(&vm->vmm, k->vma.addr, bytes, &args, sizeof(args), &mem->mem, 0);
		if (ret)
			nvif_vmm_put(&vm->vmm, &k->vma);
	}
	if (!ret)
		list_add(&k->head, &vm->kmaps);
	mutex_unlock(&vm->map_lock);
	gate_leave();
	if (ret) {
		kfree(k);
		return ret;
	}
	*va = k->vma.addr;
	return 0;
}
