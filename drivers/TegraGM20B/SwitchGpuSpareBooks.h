// SPDX-License-Identifier: MIT
// What the spare pool (TegraGM20BUserClientSpares.cpp) holds of each class, and every decision it makes over that: the
// class the refill makes next, which ready spares go back to make room, whether a dirty spare is kept, which ready spare
// a BoAlloc takes and what a miss teaches. The kernel keeps the lists and calls these under the pool's lock.
#pragma once
#include "SwitchGpuSpares.h"

struct switch_gpu_spare_books {
	uint16_t ready[SWITCH_GPU_SPARE_CLASSES];    // zeroed and clean
	uint16_t zeroing[SWITCH_GPU_SPARE_CLASSES];  // on the copy engine
	uint16_t dirty
		[SWITCH_GPU_SPARE_CLASSES];  // freed or newly made, waiting for the copy engine
	uint16_t depth[SWITCH_GPU_SPARE_CLASSES];  // kept this deep
	uint16_t floor
		[SWITCH_GPU_SPARE_CLASSES];  // the warm table's count: no miss lowers the depth below it
	bool reserved[SWITCH_GPU_SPARE_CLASSES];
	// Bytes of every spare above, in any state, plus one the refill is making: the cap bounds all of them, so a copy
	// engine too busy to zero cannot let the refill wire past it
	uint64_t held;
};

constexpr uint32_t SWITCH_GPU_SPARE_NONE = SWITCH_GPU_SPARE_CLASSES;

inline void switch_gpu_spare_books_init(switch_gpu_spare_books &b)
{
	b = {};
	for (const switch_gpu_spare_warm &w : SWITCH_GPU_SPARE_WARM) {
		const uint32_t c = switch_gpu_spare_class_up(w.bytes);
		b.depth[c] = b.floor[c] = w.ready;
		b.reserved[c] = w.reserved;
	}
}

// The class a spare of `bytes` serves: the largest class not past it
constexpr uint32_t switch_gpu_spare_class_of_spare(uint64_t bytes)
{
	const uint32_t c = switch_gpu_spare_class_up(bytes);
	return c < SWITCH_GPU_SPARE_CLASSES && switch_gpu_spare_class_bytes(c) == bytes ? c : c - 1;
}

inline bool switch_gpu_spare_books_short(const switch_gpu_spare_books &b, uint32_t c)
{
	return switch_gpu_spare_short(b.ready[c], b.zeroing[c], b.dirty[c], b.depth[c]);
}

// Bytes of ready spares kept past their class's depth: what can go back to make room
inline uint64_t switch_gpu_spare_books_over(const switch_gpu_spare_books &b)
{
	uint64_t over = 0;
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++) {
		if (b.ready[c] > b.depth[c])
			over += uint64_t(b.ready[c] - b.depth[c]) * switch_gpu_spare_class_bytes(c);
	}
	return over;
}

// The ready spare to give back next for room: the largest class past its depth, or SWITCH_GPU_SPARE_NONE
inline uint32_t switch_gpu_spare_books_give_back(const switch_gpu_spare_books &b)
{
	return switch_gpu_spare_over_depth(b.ready, b.depth, SWITCH_GPU_SPARE_CLASSES);
}

// Whether `need` more bytes fit under `cap` once every spare past its depth has gone back
inline bool switch_gpu_spare_books_fit(const switch_gpu_spare_books &b, uint64_t need, uint64_t cap)
{
	return b.held - switch_gpu_spare_books_over(b) + need <= cap;
}

// The class the refill makes a spare of next: the smallest below its depth that fits under the cap, spares past their
// depth given back first; SWITCH_GPU_SPARE_NONE when none does
inline uint32_t switch_gpu_spare_books_refill(const switch_gpu_spare_books &b, uint64_t cap)
{
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++) {
		if (switch_gpu_spare_books_short(b, c) &&
		    switch_gpu_spare_books_fit(b, switch_gpu_spare_class_bytes(c), cap))
			return c;
	}
	return SWITCH_GPU_SPARE_NONE;
}

// Whether the refill has a spare to make: after a take, the class taken from may be short while the asked one has no
// depth (backboardd's 17,280 KiB BOs, a 20 MiB class, are served by the 24 MiB class's spares)
inline bool switch_gpu_spare_books_wants_refill(const switch_gpu_spare_books &b, uint64_t cap)
{
	return switch_gpu_spare_books_refill(b, cap) != SWITCH_GPU_SPARE_NONE;
}

// Whether the copy engine zeroes a dirty spare of class `c` (already off the dirty count, its bytes still held) rather
// than it going back to the VM: under the cap as it is, or, for a class below its depth, once the spares past theirs
// have gone back. Freed BOs of classes nobody keeps a depth of are kept only while they fit.
inline bool switch_gpu_spare_books_keep(const switch_gpu_spare_books &b, uint32_t c, uint64_t cap)
{
	return b.held <= cap ||
	       (switch_gpu_spare_books_short(b, c) && switch_gpu_spare_books_fit(b, 0, cap));
}

// The class a BoAlloc of class `asked` takes a ready spare from, or SWITCH_GPU_SPARE_NONE. A client that is not the
// compositor leaves a reserved class's last ready spare.
inline uint32_t switch_gpu_spare_books_take(const switch_gpu_spare_books &b, uint32_t asked,
					    bool compositor)
{
	for (uint32_t d = asked; d < SWITCH_GPU_SPARE_CLASSES && switch_gpu_spare_serves(asked, d);
	     d++) {
		if (!b.ready[d] || (!compositor && b.reserved[d] && b.ready[d] <= 1))
			continue;
		return d;
	}
	return SWITCH_GPU_SPARE_NONE;
}

// Why a BoAlloc of class `asked` found no spare to take
inline switch_gpu_spare_miss switch_gpu_spare_books_why(const switch_gpu_spare_books &b,
							uint32_t asked, bool compositor,
							bool pressure)
{
	if (!pressure && !compositor) {
		for (uint32_t d = asked;
		     d < SWITCH_GPU_SPARE_CLASSES && switch_gpu_spare_serves(asked, d); d++) {
			if (b.ready[d])
				return SWITCH_GPU_SPARE_RESERVED;
		}
	}
	return switch_gpu_spare_miss_of(b.depth[asked], uint16_t(b.zeroing[asked] + b.dirty[asked]),
					pressure);
}

// A miss of class `asked` teaches it a deeper depth, within its cap and never below its warm count
inline void switch_gpu_spare_books_learn(switch_gpu_spare_books &b, uint32_t asked,
					 switch_gpu_spare_miss why, uint64_t learn_max_bytes,
					 uint64_t cap)
{
	const uint64_t bytes = switch_gpu_spare_class_bytes(asked);
	const uint16_t learnt =
		switch_gpu_spare_depth_after_miss(b.depth[asked], bytes <= learn_max_bytes, why);
	const uint16_t most = switch_gpu_spare_depth_cap(bytes, cap, b.floor[asked]);
	b.depth[asked] = learnt < most ? learnt : most;
	if (b.depth[asked] < b.floor[asked])
		b.depth[asked] = b.floor[asked];
}
