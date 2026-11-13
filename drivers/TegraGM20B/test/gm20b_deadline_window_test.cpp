// SPDX-License-Identifier: MIT
// deadline.c over a display period at a time: the thread calls a frame end schedules (hold_work, expire_work), the
// resident look's register reads, and whether an app channel slow enough to be held is held, by what has work in flight.
// Under dl.lock (a spinlock: XNU panics on a mutex, sleep or allocation there) nothing schedules, allocates or nests.
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
unsigned schedules, statusReads, dues, spinDepth, releases;
int workAsksUntilKick = -1;  // tgm_hold_wanted(1) calls before an app's kick lands (-1: none lands)
u64 dueVblank;
int lastResident;
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
	++spinDepth;
}

void spin_unlock_irqrestore_stub(spinlock_t *l)
{
	l->held = 0;
	--spinDepth;
}

void mayBlock(const char *what)
{
	if (spinDepth)
		ADD_FAILURE() << what << " under dl.lock";
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
	mayBlock("an allocation");
	works.push_back(new Work{fn, arg, false, 0});
	return works.back();
}

void nvxnu_work_schedule(void *work)
{
	mayBlock("a thread call");
	++schedules;
	static_cast<Work *>(work)->pending = true;
	static_cast<Work *>(work)->at = now;
}

void nvxnu_work_schedule_after(void *work, u64 ns)
{
	mayBlock("a thread call");
	++schedules;
	static_cast<Work *>(work)->pending = true;
	static_cast<Work *>(work)->at = now + ns;
}

void nvxnu_work_cancel_wait(void *work)
{
	mayBlock("a wait for a thread call");
	static_cast<Work *>(work)->pending = false;
}

int tgm_sched_resident(nvkm_device *)
{
	mayBlock("the resident look's register reads");
	statusReads += kStatusReads;
	return -1;
}

int tgm_hold_wanted(int withWork)
{
	if (withWork && workAsksUntilKick >= 0 && workAsksUntilKick-- == 0)
		appWork = true;
	return slow && (!withWork || appWork);
}

uint64_t tgm_hold_due(nvkm_device *, const struct tgm_window *w, int resident, int *preempt)
{
	EXPECT_EQ(spinDepth, 1u) << "the holds are taken under dl.lock, as hold_work takes them";
	lastResident = resident;
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
	++releases;
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
		schedules = statusReads = dues = releases = 0;
		slow = true;
		appWork = false;
		workAsksUntilKick = -1;
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
	EXPECT_EQ(schedules, 0u)
		<< "hold_work and expire_work scheduled with no app work in flight";
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

// The app's own first kick inside the frame, before its doorbell: the window kept at the frame end opens and the holds
// due by then are taken on the kicking thread, with no register read, so a held channel never reaches the GPU first.
TEST_F(DeadlineWindow, AnAppKickInTheFrameIsHeldBeforeItsDoorbell)
{
	const u64 vblank = now + kPeriod;
	frameEnd();
	tgm_deadline_kicked(1, 0);
	runDue();
	EXPECT_EQ(schedules, 0u);
	now += kPeriod / 4;
	appWork = true;
	tgm_deadline_app_kicked();
	EXPECT_EQ(dues, 1u) << "the holds, taken before the kick returns";
	EXPECT_EQ(dueVblank, vblank);
	EXPECT_EQ(lastResident, -1);
	EXPECT_EQ(statusReads, 0u);
	EXPECT_EQ(schedules, 1u) << "the window's expiry alone";
	tgm_deadline_app_kicked();
	EXPECT_EQ(dues, 1u) << "a second kick finds the window open";
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

// The compositor kicked before the app did: the window the app's kick opens counts that kick, so at its vblank it gets
// one more interval for the frame in flight instead of closing as one with no frame (as the frame end's window did).
TEST_F(DeadlineWindow, AKeptWindowCountsTheCompositorsEarlierKick)
{
	const u64 vblank = now + kPeriod;
	frameEnd();
	tgm_deadline_kicked(1, 0);
	now += kPeriod / 4;
	appWork = true;
	tgm_deadline_app_kicked();
	ASSERT_EQ(dues, 1u);
	now = vblank;
	runDue();
	EXPECT_EQ(releases, 0u)
		<< "the holds released at the vblank with the compositor's frame still in flight";
	EXPECT_EQ(schedules, 2u) << "the expiry again, one interval on";
	now += kPeriod;
	runDue();
	EXPECT_EQ(releases, 1u);
}

// The frame's present came before any app kicked: the kept window is that frame's and goes with it.
TEST_F(DeadlineWindow, APresentEndsTheKeptWindow)
{
	frameEnd();
	tgm_deadline_kicked(1, 0);
	now += kPeriod / 4;
	tgm_deadline_presented(now);
	now += 1000;
	appWork = true;
	tgm_deadline_app_kicked();
	EXPECT_EQ(dues, 0u);
	EXPECT_EQ(schedules, 0u);
}

// An app's kick lands between the frame end's look at the busy word and its keeping the window: the frame end's second
// look finds it and opens the window there.
TEST_F(DeadlineWindow, AKickBetweenTheLookAndTheKeepIsSeen)
{
	workAsksUntilKick = 1;
	frameEnd();
	EXPECT_EQ(schedules, 2u) << "hold_work and expire_work, as for app work at the frame end";
	runDue();
	EXPECT_EQ(dues, 1u);
}
