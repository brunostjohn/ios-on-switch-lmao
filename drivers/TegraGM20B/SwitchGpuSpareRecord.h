// SPDX-License-Identifier: MIT
// One boot's spare demand, kept for the next boot to seed its depths from: each slot that boot asked, by kind and class
// size, with the depth its asks left it. The pool keeps it in NVRAM (TegraGM20BUserClientSpareRecord.cpp), so it reaches
// the card when SwitchNVRAM writes the bank at sleep or shutdown.
#pragma once
#include "SwitchGpuSpareBooks.h"

constexpr uint8_t SWITCH_GPU_SPARE_RECORD_VERSION = 1;
constexpr uint32_t SWITCH_GPU_SPARE_RECORD_HEADER = 2;  // version, entries
constexpr uint32_t SWITCH_GPU_SPARE_RECORD_ENTRY =
	6;  // kind, depth, class KiB (32 bits, little-endian)
constexpr uint32_t SWITCH_GPU_SPARE_RECORD_ENTRIES = 32;
constexpr uint32_t SWITCH_GPU_SPARE_RECORD_BYTES =
	SWITCH_GPU_SPARE_RECORD_HEADER +
	SWITCH_GPU_SPARE_RECORD_ENTRIES * SWITCH_GPU_SPARE_RECORD_ENTRY;

// This boot's record into `out` (SWITCH_GPU_SPARE_RECORD_BYTES): its length. A slot this boot did not ask is left out,
// so a seed it no longer needs ends after one boot.
inline uint32_t switch_gpu_spare_record_encode(const switch_gpu_spare_books &b, uint8_t *out)
{
	uint32_t n = 0;
	for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS && n < SWITCH_GPU_SPARE_RECORD_ENTRIES;
	     s++) {
		if (b.used[s] <= SWITCH_GPU_SPARE_SEEDED || !b.depth[s])
			continue;
		uint8_t *e =
			out + SWITCH_GPU_SPARE_RECORD_HEADER + n * SWITCH_GPU_SPARE_RECORD_ENTRY;
		const uint32_t kib = uint32_t(switch_gpu_spare_slot_bytes(s) >> 10);
		e[0] = uint8_t(switch_gpu_spare_slot_kind(s));
		e[1] = uint8_t(b.depth[s]);
		for (uint32_t i = 0; i < 4; i++)
			e[2 + i] = uint8_t(kib >> (8 * i));
		n++;
	}
	out[0] = SWITCH_GPU_SPARE_RECORD_VERSION;
	out[1] = uint8_t(n);
	return SWITCH_GPU_SPARE_RECORD_HEADER + n * SWITCH_GPU_SPARE_RECORD_ENTRY;
}

// The slot of a record's entry, or SWITCH_GPU_SPARE_NONE when it names no kind or class of this build
inline uint32_t switch_gpu_spare_record_slot(const uint8_t *e)
{
	uint32_t kib = 0;
	for (uint32_t i = 0; i < 4; i++)
		kib |= uint32_t(e[2 + i]) << (8 * i);
	const uint64_t bytes = uint64_t(kib) << 10;
	const uint32_t c = switch_gpu_spare_class_up(bytes);
	if (e[0] >= SWITCH_GPU_SPARE_KINDS || c >= SWITCH_GPU_SPARE_CLASSES ||
	    switch_gpu_spare_class_bytes(c) != bytes)
		return SWITCH_GPU_SPARE_NONE;
	return switch_gpu_spare_slot(e[0], c);
}

// Seeds the books from a record: the number of slots seeded, 0 for a record of another version or length
inline uint32_t switch_gpu_spare_record_seed(switch_gpu_spare_books &b, const uint8_t *record,
					     uint32_t length, uint64_t cap)
{
	if (length < SWITCH_GPU_SPARE_RECORD_HEADER ||
	    record[0] != SWITCH_GPU_SPARE_RECORD_VERSION ||
	    record[1] > SWITCH_GPU_SPARE_RECORD_ENTRIES ||
	    length != SWITCH_GPU_SPARE_RECORD_HEADER + record[1] * SWITCH_GPU_SPARE_RECORD_ENTRY)
		return 0;
	uint32_t seeded = 0;
	for (uint32_t n = 0; n < record[1]; n++) {
		const uint8_t *e =
			record + SWITCH_GPU_SPARE_RECORD_HEADER + n * SWITCH_GPU_SPARE_RECORD_ENTRY;
		const uint32_t s = switch_gpu_spare_record_slot(e);
		if (s == SWITCH_GPU_SPARE_NONE || !e[1] || e[1] > SWITCH_GPU_SPARE_DEPTH_MAX)
			continue;
		switch_gpu_spare_books_seed(b, s, e[1], cap);
		seeded++;
	}
	return seeded;
}
