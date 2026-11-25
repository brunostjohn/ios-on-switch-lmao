// SwitchGpuSpareBooks.h, driven the way TegraGM20BUserClientSpares.cpp drives it: the refill, the recycle worker, the
// copy engine's completions and BoAlloc's take, over a model of the pool's lists. The 25 Nov device run's patterns are
// replayed through it, and through the decisions the pool made before (the cap check without dirty spares, no room for a
// class below its depth, no reserve, a refill that could make no spare of 512 KiB or more), to count the misses each
// leaves on the allocating thread.
#include <gtest/gtest.h>

#include <vector>

#include "drivers/TegraGM20B/SwitchGpuSpareBooks.h"

namespace
{
constexpr uint64_t kCap = SWITCH_GPU_SPARE_CAP_BYTES, kLearnMax = 32ULL << 20;
constexpr uint64_t kMiB = 1ULL << 20, kKiB = 1ULL << 10;

uint32_t classOf(uint64_t bytes)
{
	return switch_gpu_spare_class_up(bytes);
}

uint64_t bytesOf(uint32_t c)
{
	return switch_gpu_spare_class_bytes(c);
}

struct Pool {
	switch_gpu_spare_books b{};
	std::vector<uint64_t> dirty;  // spare bytes, as the kernel's dirty list
	bool copyEngineBusy = false;  // switch_gpu_cached_zero answers -16
	bool unitsRefused = false;    // the refill before the fix: no spare of 512 KiB or more
	unsigned made = 0, refused = 0, misses = 0;
	uint64_t peak = 0;

	Pool()
	{
		switch_gpu_spare_books_init(b);
	}

	void track()
	{
		peak = b.held > peak ? b.held : peak;
	}

	void giveBack(uint64_t need)
	{
		while (b.held + need > kCap) {
			const uint32_t d = switch_gpu_spare_books_give_back(b);
			if (d == SWITCH_GPU_SPARE_NONE)
				break;
			--b.ready[d];
			b.held -= bytesOf(d);
		}
	}

	void refill()
	{
		for (uint32_t c; (c = switch_gpu_spare_books_refill(b, kCap)) != SWITCH_GPU_SPARE_NONE;) {
			giveBack(bytesOf(c));
			b.held += bytesOf(c);
			++b.dirty[c];
			track();
			if (unitsRefused && bytesOf(c) >= SWITCH_GPU_SPARE_UNITS_FROM) {
				--b.dirty[c];
				b.held -= bytesOf(c);
				++refused;
				return;
			}
			++made;
			dirty.push_back(bytesOf(c));
			recycle();
		}
	}

	void recycle()
	{
		while (!dirty.empty()) {
			const uint64_t bytes = dirty.back();
			const uint32_t c = switch_gpu_spare_class_of_spare(bytes);
			dirty.pop_back();
			--b.dirty[c];
			if (!switch_gpu_spare_books_keep(b, c, kCap)) {
				b.held -= bytes;
				continue;
			}
			giveBack(0);
			if (copyEngineBusy) {
				dirty.insert(dirty.begin(), bytes);
				++b.dirty[c];
				return;
			}
			++b.ready[c];
		}
	}

	bool take(uint64_t bytes, bool compositor)
	{
		const uint32_t c = classOf(bytes);
		const uint32_t d = switch_gpu_spare_books_take(b, c, compositor);
		if (d != SWITCH_GPU_SPARE_NONE) {
			--b.ready[d];
			b.held -= bytesOf(d);
			return true;
		}
		++misses;
		switch_gpu_spare_books_learn(b, c, switch_gpu_spare_books_why(b, c, compositor, false), kLearnMax,
					     kCap);
		return false;
	}

	// A freed BO of class `bytes` back to the spares (spareKeep), then the workers run
	void free(uint64_t bytes)
	{
		const uint64_t backing = bytesOf(classOf(bytes));
		b.held += backing;
		++b.dirty[switch_gpu_spare_class_of_spare(backing)];
		dirty.push_back(backing);
		recycle();
		refill();
		EXPECT_LE(b.held, kCap) << "once the freed BO has been kept or given back";
	}

	uint64_t readyBytes() const
	{
		uint64_t n = 0;
		for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++)
			n += uint64_t(b.ready[c]) * bytesOf(c);
		return n;
	}
};

TEST(SpareBooks, EveryWarmClassIsReadyFromBootUnderTheCap)
{
	Pool p;
	p.refill();
	for (const switch_gpu_spare_warm &w : SWITCH_GPU_SPARE_WARM)
		EXPECT_EQ(p.b.ready[classOf(w.bytes)], w.ready) << (w.bytes >> 10) << " KiB";
	EXPECT_EQ(p.b.held, switch_gpu_spare_warm_bytes());
	EXPECT_LE(p.peak, kCap);
}

// Review KR1: the compositor's 24 MiB class kept two spares, one for it, only by learning its second, which the cap
// had no room for (72.5 + 24 > 96 MiB). From boot the class holds both, and a miss never lowers a warm class's depth.
TEST(SpareBooks, TheCompositorsClassHoldsItsReserveFromBoot)
{
	Pool p;
	p.refill();
	const uint32_t c = classOf(24 * kMiB);
	EXPECT_TRUE(p.b.reserved[c]);
	EXPECT_EQ(p.b.ready[c], 2);
	const uint32_t sixteen = classOf(16 * kMiB);
	p.b.ready[sixteen] = 0;
	p.take(16 * kMiB, false);
	EXPECT_GE(p.b.depth[sixteen], 2) << "a miss in a warm class kept two lowered it to the quarter-cap depth";
}

// 25 Nov: gpu.path.launch_target's 1792x1472 RGBA16F target (21,504 KiB), made twice by an app: the first takes one of
// the class's two warm spares, the compositor's stays, and the refill makes the next before the second.
TEST(SpareBooks, AnAppsLaunchTargetLeavesTheCompositorsSpare)
{
	Pool p;
	p.refill();
	const uint64_t target = 21504 * kKiB;
	EXPECT_TRUE(p.take(target, false));
	EXPECT_EQ(p.b.ready[classOf(24 * kMiB)], 1);
	EXPECT_FALSE(p.take(target, false)) << "the last one is the compositor's";
	EXPECT_EQ(switch_gpu_spare_books_why(p.b, classOf(target), false, false), SWITCH_GPU_SPARE_RESERVED);
	EXPECT_TRUE(p.take(target, true));
	p.refill();
	EXPECT_EQ(p.b.ready[classOf(24 * kMiB)], 2);
}

// The same two rounds on the pool as it ran on 25 Nov: the refill made no spare of 512 KiB or more, so the warm 24 MiB
// class never held one, and the first round zeroed its target on the calling thread ("drained 1", 13,782 us).
TEST(SpareBooks, LaunchTargetReplayBeforeAndAfter)
{
	const auto rounds = [](bool unitsRefused) {
		Pool p;
		p.unitsRefused = unitsRefused;
		p.refill();
		const uint64_t target = 21504 * kKiB;
		for (int round = 0; round < 2; ++round) {
			p.take(target, false);
			p.free(target);
		}
		return p.misses;
	};
	EXPECT_GE(rounds(true), 1u);
	EXPECT_EQ(rounds(false), 0u);
}

// Review KR2: with the copy engine too busy to zero, every spare the refill makes stays dirty; counting only ready and
// zeroing bytes against the cap, the refill wired past it. Every byte in every state is held now.
TEST(SpareBooks, ABusyCopyEngineCannotLetTheRefillPassTheCap)
{
	Pool p;
	p.copyEngineBusy = true;
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++)
		if (bytesOf(c) <= kLearnMax && bytesOf(c) >= kMiB)
			p.b.depth[c] = switch_gpu_spare_depth_cap(bytesOf(c), kCap, p.b.floor[c]);
	p.refill();
	EXPECT_LE(p.peak, kCap);
	EXPECT_GT(p.made, 0u);
}

// 25 Nov 14:22-14:25: the spares held 94-98 MiB of whatever had been freed, no spare of 20 MiB or more among them,
// while backboardd freed and remade 17,280 KiB BOs; each freed one was dropped (no room under the cap), so each remake
// zeroed 17 MiB on its render thread (~16 ms). Its class learns a depth at the first miss, and a freed BO of a class
// below its depth takes its room from spares kept past theirs.
TEST(SpareBooks, AFullPoolKeepsTheCompositorsChurnedTargets)
{
	const auto churn = [](bool roomForShortClasses) {
		Pool p;
		p.refill();
		EXPECT_TRUE(p.take(24 * kMiB, true));
		EXPECT_TRUE(p.take(24 * kMiB, true));
		while (p.b.held + 4 * kMiB <= kCap) {
			p.b.held += 4 * kMiB;
			++p.b.ready[classOf(4 * kMiB)];
		}
		const uint64_t bo = 17280 * kKiB;
		unsigned remade = 0;
		for (int i = 0; i < 10; ++i) {
			if (!p.take(bo, true))
				++remade;
			if (roomForShortClasses) {
				p.free(bo);
			} else if (p.b.held + bytesOf(classOf(bo)) <= kCap) {
				p.b.held += bytesOf(classOf(bo));
				++p.b.ready[classOf(bo)];
			}
		}
		EXPECT_LE(p.peak, kCap);
		return remade;
	};
	EXPECT_EQ(churn(false), 10u);
	EXPECT_EQ(churn(true), 1u);
}

TEST(SpareBooks, AFreedBoOfAClassNobodyKeepsGoesBackWhenThePoolIsFull)
{
	Pool p;
	p.refill();
	while (p.b.held + 4 * kMiB <= kCap) {
		p.b.held += 4 * kMiB;
		++p.b.ready[classOf(4 * kMiB)];
	}
	const uint64_t before = p.readyBytes();
	p.free(12 * kMiB);
	EXPECT_EQ(p.b.ready[classOf(12 * kMiB)], 0);
	EXPECT_EQ(p.readyBytes(), before);
	EXPECT_LE(p.b.held, kCap);
}

TEST(SpareBooks, ASpareServesItsOwnClassAndTheFourAboveIt)
{
	Pool p;
	p.refill();
	EXPECT_EQ(switch_gpu_spare_books_take(p.b, classOf(17280 * kKiB), true), classOf(24 * kMiB));
	EXPECT_EQ(switch_gpu_spare_books_take(p.b, classOf(300 * kKiB), false), SWITCH_GPU_SPARE_NONE);
}

}  // namespace
