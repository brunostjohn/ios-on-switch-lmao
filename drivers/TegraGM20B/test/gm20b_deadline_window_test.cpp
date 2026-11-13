// SPDX-License-Identifier: MIT
// deadline.c over a display period at a time: the thread calls a frame end schedules (hold_work, expire_work), the
// resident look's register reads, and whether an app channel slow enough to be held is held, by what has work in flight.
#include <gtest/gtest.h>
#include <vector>
#include "drivers/TegraGM20B/test/gm20b_sched_stubs.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BSched.h"
}

extern "C" void tegra_gm20b_compositor_vsync(uint64_t stamp, uint32_t lead_us, uint64_t interval);

namespace
{
constexpr u64 kPeriod = 16666667, kStatusReads = 3;
u64 now;
unsigned schedules, statusReads, dues;
u64 dueVblank;
bool slow = true, appWork;

struct Work {
	void (*fn)(void *);
	void *arg;
	bool pending;
	u64 at;
};

std::vector<Work *> works;

void runDue()
{
	for (bool ran = true; ran;) {
		ran = false;
		for (Work *w : works) {
			if (w->pending && w->at <= now) {
				w->pending = false;
				w->fn(w->arg);
				ran = true;
			}
		}
	}
}
}  // namespace

extern "C" {
void spin_lock_irqsave_stub(spinlock_t *l)
{
	EXPECT_EQ(l->held, 0);
	l->held = 1;
}

void spin_unlock_irqrestore_stub(spinlock_t *l)
{
	l->held = 0;
}

u64 nvxnu_nanotime(void)
{
	return now;
}

u64 nvxnu_abstime_to_ns(u64 t)
{
	return t;
}

void nvxnu_panic(const char *, ...)
{
	ADD_FAILURE() << "panic";
}

void *nvxnu_work_alloc(void (*fn)(void *), void *arg)
{
	works.push_back(new Work{fn, arg, false, 0});
	return works.back();
}

void nvxnu_work_schedule(void *work)
{
	++schedules;
	static_cast<Work *>(work)->pending = true;
	static_cast<Work *>(work)->at = now;
}

void nvxnu_work_schedule_after(void *work, u64 ns)
{
	++schedules;
	static_cast<Work *>(work)->pending = true;
	static_cast<Work *>(work)->at = now + ns;
}

void nvxnu_work_cancel_wait(void *work)
{
	static_cast<Work *>(work)->pending = false;
}

int tgm_sched_resident(nvkm_device *)
{
	statusReads += kStatusReads;
	return -1;
}

int tgm_hold_wanted(int withWork)
{
	return slow && (!withWork || appWork);
}

uint64_t tgm_hold_due(nvkm_device *, const struct tgm_window *w, int, int *preempt)
{
	++dues;
	dueVblank = w->vblank;
	*preempt = -1;
	return 0;
}

int tgm_hold_preempt(nvkm_device *, unsigned int)
{
	return 0;
}

uint64_t tgm_hold_latency(unsigned int)
{
	return 0;
}

void tgm_hold_check(nvkm_device *, unsigned int)
{
}

void tgm_hold_unneed(uint64_t)
{
}

void tgm_hold_release(nvkm_device *, uint64_t)
{
}
}

class DeadlineWindow : public ::testing::Test
{
protected:
	static nvkm_device device;

	static void SetUpTestSuite()
	{
		tgm_deadline_init(&device);
	}

	void SetUp() override
	{
		now += 10 * kPeriod;
		runDue();
		for (Work *w : works)
			w->pending = false;
		schedules = statusReads = dues = 0;
		slow = true;
		appWork = false;
	}

	// A frame end at `now`: the display's stamp for it and the period
	void frameEnd()
	{
		tegra_gm20b_compositor_vsync(now, 0, kPeriod);
	}
};

nvkm_device DeadlineWindow::device;

// Frame ends with an app channel slow enough to be held but no app work in flight: the period costs no thread call and
// no register read.
TEST_F(DeadlineWindow, NoAppWorkSchedulesNothing)
{
	for (int i = 0; i < 4; ++i) {
		frameEnd();
		tgm_deadline_kicked(1, 0);
		now += kPeriod;
		runDue();
	}
	EXPECT_EQ(schedules, 0u) << "hold_work and expire_work scheduled with no app work in flight";
	EXPECT_EQ(statusReads, 0u);
	EXPECT_EQ(dues, 0u);
}

// No app channel slow enough to be held: no window, before and after.
TEST_F(DeadlineWindow, NoSlowChannelNoWindow)
{
	slow = false;
	frameEnd();
	now += kPeriod;
	runDue();
	EXPECT_EQ(schedules, 0u);
	EXPECT_EQ(dues, 0u);
}

// A slow app channel with work in flight at the frame end: the window opens there, as before.
TEST_F(DeadlineWindow, AppWorkAtTheFrameEndOpensTheWindow)
{
	appWork = true;
	const u64 vblank = now + kPeriod;
	frameEnd();
	EXPECT_EQ(schedules, 2u) << "hold_work now, expire_work at the vblank";
	runDue();
	EXPECT_EQ(dues, 1u);
	EXPECT_EQ(dueVblank, vblank);
	EXPECT_EQ(statusReads, kStatusReads);
}

// The app's own kick inside the frame: the window kept at the frame end opens and holds it.
TEST_F(DeadlineWindow, AnAppKickInTheFrameOpensItsWindow)
{
	const u64 vblank = now + kPeriod;
	frameEnd();
	tgm_deadline_kicked(1, 0);
	runDue();
	EXPECT_EQ(schedules, 0u);
	now += kPeriod / 4;
	appWork = true;
	tgm_deadline_app_kicked();
	EXPECT_EQ(schedules, 1u);
	runDue();
	EXPECT_EQ(dues, 1u) << "the app channel's hold";
	EXPECT_EQ(dueVblank, vblank);
	EXPECT_EQ(schedules, 2u) << "and the window's expiry";
}

// A kick once the kept window's vblank has passed: that frame is gone, and the next frame end decides again.
TEST_F(DeadlineWindow, AnAppKickPastTheVblankOpensNothing)
{
	frameEnd();
	now += kPeriod + 1;
	appWork = true;
	tgm_deadline_app_kicked();
	runDue();
	EXPECT_EQ(schedules, 0u);
	EXPECT_EQ(dues, 0u);
}
