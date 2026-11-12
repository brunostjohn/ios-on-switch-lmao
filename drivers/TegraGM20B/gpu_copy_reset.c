// SPDX-License-Identifier: MIT
/* Debug kernels: TegraGM20BCopyResetABI.h's copy engine reset under the display's present, which hw_test_app's
 * gpu.present.copy_reset drives. Present channel nonstall interrupts come for every channel on its runlist (nvkm's
 * nonstall event is the runlist's), so each one while the armed present is in flight looks again whether the copy engine
 * is busy with the present's context. */
#include "gpu_internal.h"
#include "TegraGM20BCopyResetABI.h"
#include "TegraGM20BRecover.h"

#if TGM_STATS
#define COPY_RESET_LOOKING \
	100 /* internal: a work item is looking at the copy engine; answered as InFlight */

static struct {
	int state; /* TegraGM20BCopyReset* or COPY_RESET_LOOKING (atomic) */
	int status;
	u32 engine_status;
	u32 redone;
	void *work;
} copy_reset;

static void copy_reset_mark(void)
{
	mutex_lock(&tgm_live_lock);
	tgm_ctxsw_redo_mark(tgm_present.chan);
	mutex_unlock(&tgm_live_lock);
}

static void copy_reset_work(void *unused)
{
	u32 status = 0;
	int looking = COPY_RESET_LOOKING;
	const int reset =
		tgm_recover_copy_reset((int)tgm_present.chan->chid, copy_reset_mark, &status);

	(void)unused;
	WRITE_ONCE(copy_reset.engine_status, status);
	__atomic_compare_exchange_n(&copy_reset.state, &looking,
				    reset ? TegraGM20BCopyResetReset : TegraGM20BCopyResetInFlight,
				    false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

int tgm_copy_reset_armed(void)
{
	return __atomic_load_n(&copy_reset.state, __ATOMIC_ACQUIRE) == TegraGM20BCopyResetArmed;
}

int tgm_copy_reset_take(void)
{
	int armed = TegraGM20BCopyResetArmed;
	return __atomic_compare_exchange_n(&copy_reset.state, &armed, TegraGM20BCopyResetInFlight,
					   false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

void tgm_copy_reset_nonstall(void)
{
	int in_flight = TegraGM20BCopyResetInFlight;
	if (__atomic_compare_exchange_n(&copy_reset.state, &in_flight, COPY_RESET_LOOKING, false,
					__ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
		nvxnu_work_schedule(copy_reset.work);
}

void tgm_copy_reset_redone(void)
{
	__atomic_fetch_add(&copy_reset.redone, 1, __ATOMIC_RELAXED);
}

/* The display's completion of a present: the armed one's, reset under it or not (one present is in flight at a time). */
void tgm_copy_reset_presented(int status)
{
	int reset = TegraGM20BCopyResetReset;
	int in_flight = TegraGM20BCopyResetInFlight;

	WRITE_ONCE(copy_reset.status, status);
	if (!__atomic_compare_exchange_n(&copy_reset.state, &reset, TegraGM20BCopyResetDone, false,
					 __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
		__atomic_compare_exchange_n(&copy_reset.state, &in_flight,
					    TegraGM20BCopyResetMissed, false, __ATOMIC_SEQ_CST,
					    __ATOMIC_RELAXED);
}

int tgm_copy_reset_call(const struct TegraGM20BAbiCopyReset *in,
			struct TegraGM20BAbiCopyResetReply *reply)
{
	int state = 0;

	if (in->reserved || in->op > TegraGM20BCopyResetQuery)
		return -EINVAL;
	if (!copy_reset.work) {
		void *work = nvxnu_work_alloc(copy_reset_work, NULL);
		if (!work)
			return -ENOMEM;
		copy_reset.work = work;
	}
	if (in->op == TegraGM20BCopyResetArm) {
		__atomic_store_n(&copy_reset.redone, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&copy_reset.state, TegraGM20BCopyResetArmed, __ATOMIC_RELEASE);
	} else if (in->op == TegraGM20BCopyResetDisarm) {
		state = TegraGM20BCopyResetArmed;
		__atomic_compare_exchange_n(&copy_reset.state, &state, TegraGM20BCopyResetIdle,
					    false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
	}
	state = __atomic_load_n(&copy_reset.state, __ATOMIC_ACQUIRE);
	reply->state = state == COPY_RESET_LOOKING ? TegraGM20BCopyResetInFlight : state;
	reply->status = READ_ONCE(copy_reset.status);
	reply->engine_status = READ_ONCE(copy_reset.engine_status);
	reply->redone = __atomic_load_n(&copy_reset.redone, __ATOMIC_RELAXED);
	return 0;
}
#endif
