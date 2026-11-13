// SPDX-License-Identifier: MIT
// sched.c's high kick over one interleaved runlist (GR and its PBDMA), every register read counted: what a compositor kick
// reads of PFIFO and the PBDMA, by what has work in flight.
#include <gtest/gtest.h>
#include <cstdarg>
#include <map>
#include "drivers/TegraGM20B/test/gm20b_sched_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BHost.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BSched.h"
}
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"

namespace
{
std::map<u32, u32> regs;
unsigned reads, preempts;

struct Model {
	nvkm_device device{};
	nvkm_fifo fifo{};
	nvkm_fifo_func fifoFunc{};
	nvkm_chan_func chanFunc{};
	nvkm_timer timer{};
	nvkm_runl runl{};
	nvkm_runl_func runlFunc{};
	nvkm_chid chid{};
	nvkm_runq runq{};
	nvkm_engine gr{};
	nvkm_engn engn{};
	nvkm_engn_func engnFunc{};
	nvkm_memory memory{};
};

Model *model;

bool pending(nvkm_runl *)
{
	return false;
}

void preempt(nvkm_chan *)
{
}

constexpr unsigned kCompositor = 3, kApp = 7;
}  // namespace

extern "C" {
void mutex_lock(struct mutex *m)
{
	m->held = 1;
}

void mutex_unlock(struct mutex *m)
{
	m->held = 0;
}

int mutex_trylock(struct mutex *m)
{
	if (m->held)
		return 0;
	m->held = 1;
	return 1;
}

u32 nvkm_rd32(nvkm_device *, u32 addr)
{
	++reads;
	return regs[addr];
}

void nvkm_wr32(nvkm_device *, u32 addr, u32 value)
{
	if (addr == PFIFO_PREEMPT)
		++preempts;
	regs[addr] = value;
}

u64 nvkm_timer_read(nvkm_timer *)
{
	return 1000;
}

int nvkm_memory_new(nvkm_device *, int, u64, u32, bool, nvkm_memory **out)
{
	*out = &model->memory;
	return 0;
}

int nv50_runl_update(nvkm_runl *)
{
	return 0;
}

void nvkm_runl_update_locked(nvkm_runl *, bool)
{
}

void nvkm_runl_rc_cgrp(nvkm_cgrp *)
{
}

bool irqs_disabled(void)
{
	return false;
}

void nvxnu_log(const char *, ...)
{
}

nvkm_fifo_func *tgm_fifo_func(nvkm_fifo *fifo)
{
	return const_cast<nvkm_fifo_func *>(fifo->func);
}

void tgm_hold_init(void)
{
}

void tgm_deadline_init(nvkm_device *)
{
}

void tgm_hold_track(nvkm_device *, unsigned int, int)
{
}

int tgm_hold_needed(unsigned int)
{
	return 0;
}

uint64_t tgm_hold_asked(unsigned int, uint64_t asked_pt)
{
	return asked_pt;
}
}

class SchedKick : public ::testing::Test
{
protected:
	Model m;

	void SetUp() override
	{
		model = &m;
		regs.clear();
		m.device.fifo = &m.fifo;
		m.device.timer = &m.timer;
		m.chanFunc.preempt = preempt;
		m.fifoFunc.chan.func = &m.chanFunc;
		m.fifo.func = &m.fifoFunc;
		m.fifo.engine.subdev.device = &m.device;
		INIT_LIST_HEAD(&m.fifo.runls);
		m.runlFunc.size = 8;
		m.runlFunc.update = nv50_runl_update;
		m.runlFunc.pending = pending;
		m.runl.func = &m.runlFunc;
		m.runl.fifo = &m.fifo;
		m.chid.nr = 512;
		m.chid.used[kCompositor / 64] |= 1ul << (kCompositor % 64);
		m.chid.used[kApp / 64] |= 1ul << (kApp % 64);
		m.runl.chid = &m.chid;
		INIT_LIST_HEAD(&m.runl.cgrps);
		INIT_LIST_HEAD(&m.runl.engns);
		m.runq.id = 0;
		m.runl.runq[0] = &m.runq;
		m.runl.runq_nr = 1;
		m.gr.subdev.type = NVKM_ENGINE_GR;
		m.engn.id = 0;
		m.engn.engine = &m.gr;
		m.engn.func = &m.engnFunc;
		list_add_tail(&m.engn.head, &m.runl.engns);
		list_add_tail(&m.runl.head, &m.fifo.runls);
		tgm_sched_init(&m.device);
		tgm_sched_level(&m.device, kCompositor, TGM_LEVEL_HIGH);
		reads = preempts = 0;
	}

	tgm_sched_kick kick(unsigned *read)
	{
		tgm_sched_queued queued{};
		const unsigned before = reads;
		const tgm_sched_kick result = tgm_sched_kicked(&m.device, kCompositor, &queued);
		*read = reads - before;
		return result;
	}
};

// The compositor's kick while no app channel has work: GR and the PBDMA can only be idle or on a high channel.
TEST_F(SchedKick, NoAppWorkReadsNoRegister)
{
	unsigned read = 0;
	EXPECT_EQ(kick(&read), TGM_KICK_CLEAR);
	EXPECT_EQ(read, 0u) << "PFIFO and PBDMA status read with no app channel's work in flight";
	EXPECT_EQ(preempts, 0u);
	unsigned resident = reads;
	EXPECT_EQ(tgm_sched_resident(&m.device), TGM_VICTIM_NONE);
	EXPECT_EQ(reads - resident, 0u) << "the hold's resident look read registers with no app work";
}
