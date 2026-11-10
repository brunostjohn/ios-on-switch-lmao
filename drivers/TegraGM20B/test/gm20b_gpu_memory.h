// SPDX-License-Identifier: MIT
#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

// GPU memory as the model sees it: a page table of 4 KiB GPU pages, each the CPU's view of the bytes behind it and
// whether its PTE is read-only. Only the stubbed vm-map entry points (tgm_vm_map_ordered, switch_gpu_vm_map,
// switch_gpu_vm_map_volatile, tgm_vm_kmap) fill it, at the addresses the driver asks for: a read of an unmapped page and a
// write through a read-only PTE are faults, as the GMMU raises them.
struct Memory {
	static constexpr uint64_t kPage = 4096;

	// A page of a test's buffer (followed through the vector, which may reallocate) or of host memory.
	struct Pte {
		std::vector<uint8_t> *buffer;
		uint8_t *bytes;
		uint64_t offset, size;
		bool readOnly;

		uint8_t *base() const
		{
			return buffer ? buffer->data() + offset : bytes;
		}

		uint64_t valid() const
		{
			return buffer ? (buffer->size() > offset
						 ? std::min(kPage, buffer->size() - offset)
						 : 0)
				      : size;
		}
	};

	std::map<uint64_t, Pte> ptes;  // by GPU page number
	uint64_t lastPage = ~0ull;
	const Pte *last = nullptr;

	void map(uint64_t va, uint8_t *bytes, uint64_t size, bool readOnly)
	{
		EXPECT_EQ(va % kPage, 0u) << "a GPU mapping starts on a page";
		last = nullptr;
		for (uint64_t at = 0; at < size; at += kPage)
			ptes[(va + at) / kPage] = {nullptr, bytes + at, 0,
						   std::min(kPage, size - at), readOnly};
	}

	void map(uint64_t va, std::vector<uint8_t> &buffer, bool readOnly = false)
	{
		EXPECT_EQ(va % kPage, 0u) << "a GPU mapping starts on a page";
		last = nullptr;
		for (uint64_t at = 0; at < buffer.size(); at += kPage)
			ptes[(va + at) / kPage] = {&buffer, nullptr, at, 0, readOnly};
	}

	void unmap(uint64_t va, uint64_t size)
	{
		last = nullptr;
		for (uint64_t at = 0; at < size; at += kPage)
			ptes.erase((va + at) / kPage);
	}

	bool mapped(uint64_t va) const
	{
		return ptes.count(va / kPage) != 0;
	}

	bool readOnly(uint64_t va) const
	{
		const auto it = ptes.find(va / kPage);
		return it != ptes.end() && it->second.readOnly;
	}

	// nullptr: a fault (no PTE, past the buffer, or a write through a read-only PTE)
	uint8_t *at(uint64_t va, bool write = false)
	{
		const uint64_t page = va / kPage, offset = va % kPage;
		if (page != lastPage || !last) {
			const auto it = ptes.find(page);
			if (it == ptes.end())
				return nullptr;
			lastPage = page;
			last = &it->second;
		}
		if (offset >= last->valid() || (write && last->readOnly))
			return nullptr;
		return last->base() + offset;
	}

	// A pushbuffer as the GPU's host fetches it: through its GPU address. false: a fault.
	bool fetch(uint64_t va, uint32_t dwords, std::vector<uint32_t> &words)
	{
		words.resize(dwords);
		for (uint32_t i = 0; i < dwords; ++i) {
			const uint8_t *word = at(va + uint64_t(i) * 4);
			if (!word || (va + uint64_t(i) * 4) % kPage > kPage - 4)
				return false;
			__builtin_memcpy(&words[i], word, 4);
		}
		return true;
	}
};
