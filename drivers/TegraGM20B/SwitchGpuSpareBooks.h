// SPDX-License-Identifier: MIT
// What the spare pool (TegraGM20BUserClientSpares.cpp) holds of each slot (a kind and a class), and every decision it
// makes over that: the slot the refill makes next, which ready spares go back to make room, whether a dirty spare is
// kept, which ready spare a BoAlloc takes and what a miss teaches. The kernel keeps the lists and calls these under the
// pool's lock.
#pragma once
#include "SwitchGpuSpares.h"

constexpr uint32_t SWITCH_GPU_SPARE_SLOTS = SWITCH_GPU_SPARE_CLASSES * SWITCH_GPU_SPARE_KINDS;

constexpr uint32_t switch_gpu_spare_slot(uint32_t kind, uint32_t c)
{
	return kind * SWITCH_GPU_SPARE_CLASSES + c;
}

constexpr uint32_t switch_gpu_spare_slot_class(uint32_t slot)
{
	return slot % SWITCH_GPU_SPARE_CLASSES;
}

constexpr uint32_t switch_gpu_spare_slot_kind(uint32_t slot)
{
	return slot / SWITCH_GPU_SPARE_CLASSES;
}

constexpr uint64_t switch_gpu_spare_slot_bytes(uint32_t slot)
{
	return switch_gpu_spare_class_bytes(switch_gpu_spare_slot_class(slot));
}

struct switch_gpu_spare_books {
	uint16_t ready[SWITCH_GPU_SPARE_SLOTS];    // zeroed and clean
	uint16_t zeroing[SWITCH_GPU_SPARE_SLOTS];  // on the copy engine
	uint16_t dirty[SWITCH_GPU_SPARE_SLOTS];    // freed or newly made, waiting for the copy engine
	uint16_t depth[SWITCH_GPU_SPARE_SLOTS];    // kept this deep
	uint16_t floor[SWITCH_GPU_SPARE_SLOTS];    // the warm table's count: no miss lowers the depth below it
	bool reserved[SWITCH_GPU_SPARE_SLOTS];
	// Bytes of every spare above, in any state, plus one the refill is making: the cap bounds all of them, so a copy
	// engine too busy to zero cannot let the refill wire past it
	uint64_t held;
};

constexpr uint32_t SWITCH_GPU_SPARE_NONE = SWITCH_GPU_SPARE_SLOTS;

inline void switch_gpu_spare_books_init(switch_gpu_spare_books &b)
{
	b = {};
	for (const switch_gpu_spare_warm &w : SWITCH_GPU_SPARE_WARM) {
		const uint32_t s =
			switch_gpu_spare_slot(SWITCH_GPU_SPARE_CACHED, switch_gpu_spare_class_up(w.bytes));
		b.depth[s] = b.floor[s] = w.ready;
		b.reserved[s] = w.reserved;
	}
}

// The class a spare of `bytes` serves: the largest class not past it
constexpr uint32_t switch_gpu_spare_class_of_spare(uint64_t bytes)
{
	const uint32_t c = switch_gpu_spare_class_up(bytes);
	return c < SWITCH_GPU_SPARE_CLASSES && switch_gpu_spare_class_bytes(c) == bytes ? c : c - 1;
}

inline bool switch_gpu_spare_books_short(const switch_gpu_spare_books &b, uint32_t s)
{
	return switch_gpu_spare_short(b.ready[s], b.zeroing[s], b.dirty[s], b.depth[s]);
}

// A warm slot below its warm count: its refill comes first, and may take room from spares any slot keeps past its own
// warm count (learned depths of either kind included), so learned depths cannot starve the warm table or the
// compositor's reserve
inline bool switch_gpu_spare_books_floor_short(const switch_gpu_spare_books &b, uint32_t s)
{
	return switch_gpu_spare_short(b.ready[s], b.zeroing[s], b.dirty[s], b.floor[s]);
}

// The line a slot's ready spares above can go back from: its depth, or for a warm slot's refill its warm count
inline uint16_t switch_gpu_spare_books_line(const switch_gpu_spare_books &b, uint32_t s,
					    bool floors)
{
	return floors ? b.floor[s] : b.depth[s];
}

// Bytes of ready spares kept past their line: what can go back to make room
inline uint64_t switch_gpu_spare_books_over(const switch_gpu_spare_books &b, bool floors = false)
{
	uint64_t over = 0;
	for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS; s++) {
		const uint16_t line = switch_gpu_spare_books_line(b, s, floors);
		if (b.ready[s] > line)
			over += uint64_t(b.ready[s] - line) * switch_gpu_spare_slot_bytes(s);
	}
	return over;
}

// The ready spare to give back next for room: the largest class past its line, of either kind, or SWITCH_GPU_SPARE_NONE
inline uint32_t switch_gpu_spare_books_give_back(const switch_gpu_spare_books &b,
						 bool floors = false)
{
	for (uint32_t c = SWITCH_GPU_SPARE_CLASSES; c-- > 0;) {
		for (uint32_t k = 0; k < SWITCH_GPU_SPARE_KINDS; k++) {
			const uint32_t s = switch_gpu_spare_slot(k, c);
			if (b.ready[s] > switch_gpu_spare_books_line(b, s, floors))
				return s;
		}
	}
	return SWITCH_GPU_SPARE_NONE;
}

// Whether `need` more bytes fit under `cap` once every spare past its line has gone back
inline bool switch_gpu_spare_books_fit(const switch_gpu_spare_books &b, uint64_t need, uint64_t cap,
				       bool floors = false)
{
	return b.held - switch_gpu_spare_books_over(b, floors) + need <= cap;
}

// The slot the refill makes a spare of next, and in `floors` whether its room comes from past the warm counts: first
// the smallest warm class below its warm count, then the smallest class of either kind below its learned depth, each
// only where it fits under the cap; SWITCH_GPU_SPARE_NONE when none does
inline uint32_t switch_gpu_spare_books_refill(const switch_gpu_spare_books &b, uint64_t cap,
					      bool *floors = nullptr)
{
	for (int pass = 0; pass < 2; pass++) {
		const bool warm = pass == 0;
		for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++) {
			for (uint32_t k = 0; k < SWITCH_GPU_SPARE_KINDS; k++) {
				const uint32_t s = switch_gpu_spare_slot(k, c);
				const bool wanted = warm ? switch_gpu_spare_books_floor_short(b, s)
							 : switch_gpu_spare_books_short(b, s);
				if (wanted && switch_gpu_spare_books_fit(b, switch_gpu_spare_slot_bytes(s),
									 cap, warm)) {
					if (floors)
						*floors = warm;
					return s;
				}
			}
		}
	}
	return SWITCH_GPU_SPARE_NONE;
}

// Whether the refill has a spare to make: after a take, the class taken from may be short while the asked one has no
// depth (backboardd's 17,280 KiB BOs, a 20 MiB class, are served by the 24 MiB class's spares)
inline bool switch_gpu_spare_books_wants_refill(const switch_gpu_spare_books &b, uint64_t cap)
{
	return switch_gpu_spare_books_refill(b, cap) != SWITCH_GPU_SPARE_NONE;
}

// Whether the copy engine zeroes a dirty spare of slot `s` (already off the dirty count, its bytes still held) rather
// than it going back to the VM: under the cap as it is, or, for a slot below its depth, once the spares past theirs have
// gone back. Freed BOs of slots nobody keeps a depth of are kept only while they fit.
inline bool switch_gpu_spare_books_keep(const switch_gpu_spare_books &b, uint32_t s, uint64_t cap)
{
	return b.held <= cap ||
	       (switch_gpu_spare_books_floor_short(b, s) &&
		switch_gpu_spare_books_fit(b, 0, cap, true)) ||
	       (switch_gpu_spare_books_short(b, s) && switch_gpu_spare_books_fit(b, 0, cap));
}

// The end of `slot`'s kind: a take or a miss looks no further, so the kinds never serve each other
constexpr uint32_t switch_gpu_spare_kind_end(uint32_t slot)
{
	return (switch_gpu_spare_slot_kind(slot) + 1) * SWITCH_GPU_SPARE_CLASSES;
}

// The slot a BoAlloc of slot `asked` takes a ready spare from, or SWITCH_GPU_SPARE_NONE. A client that is not the
// compositor leaves a reserved class's last ready spare.
inline uint32_t switch_gpu_spare_books_take(const switch_gpu_spare_books &b, uint32_t asked,
					    bool compositor)
{
	for (uint32_t d = asked;
	     d < switch_gpu_spare_kind_end(asked) &&
	     switch_gpu_spare_serves(switch_gpu_spare_slot_class(asked), switch_gpu_spare_slot_class(d));
	     d++) {
		if (!b.ready[d] || (!compositor && b.reserved[d] && b.ready[d] <= 1))
			continue;
		return d;
	}
	return SWITCH_GPU_SPARE_NONE;
}

// Why a BoAlloc of slot `asked` found no spare to take
inline switch_gpu_spare_miss switch_gpu_spare_books_why(const switch_gpu_spare_books &b,
							uint32_t asked, bool compositor,
							bool pressure)
{
	if (!pressure && !compositor) {
		for (uint32_t d = asked; d < switch_gpu_spare_kind_end(asked) &&
					 switch_gpu_spare_serves(switch_gpu_spare_slot_class(asked),
								 switch_gpu_spare_slot_class(d));
		     d++) {
			if (b.ready[d])
				return SWITCH_GPU_SPARE_RESERVED;
		}
	}
	return switch_gpu_spare_miss_of(b.depth[asked], uint16_t(b.zeroing[asked] + b.dirty[asked]),
					pressure);
}

// A miss of slot `asked` teaches it a deeper depth, within its cap and never below its warm count
inline void switch_gpu_spare_books_learn(switch_gpu_spare_books &b, uint32_t asked,
					 switch_gpu_spare_miss why, uint64_t learn_max_bytes,
					 uint64_t cap)
{
	const uint64_t bytes = switch_gpu_spare_slot_bytes(asked);
	const uint16_t learnt =
		switch_gpu_spare_depth_after_miss(b.depth[asked], bytes <= learn_max_bytes, why);
	const uint16_t most = switch_gpu_spare_depth_cap(bytes, cap, b.floor[asked]);
	b.depth[asked] = learnt < most ? learnt : most;
	if (b.depth[asked] < b.floor[asked])
		b.depth[asked] = b.floor[asked];
}
