// SPDX-License-Identifier: MIT
// An engine stuck loading a live channel's context: reset under it, nothing killed (TegraGM20BRecover.h tgm_load_verdict).
#include "gpu_internal.h"
#include "../../kits/SwitchLinux/SwitchLinux.h"

/* tgm_live_lock: the line names the channel and every engine, past nvxnu_log's 512 characters and within os_log's 1,024 */
static char load_line[1024];

/* tgm_live_lock held */
static enum tgm_ctxsw load_verdict(struct tgm_chan *chan, const char *engine, const char *who)
{
	char state[TGM_HANG_LINE];
	char engines[TGM_HANG_LINE];
	char regs[TGM_HANG_LINE];
	const u32 done = chan ? tgm_chan_completed(chan) : 0;
	const enum tgm_ctxsw verdict = tgm_load_verdict(
		chan != NULL, chan && READ_ONCE(chan->error) != 0,
		chan && tgm_load_reloaded(chan->load_reset, chan->load_reset_done, done));

	if (verdict != TGM_CTXSW_RELOAD && verdict != TGM_CTXSW_FATAL)
		return verdict;
	tgm_chan_describe(chan, state, sizeof(state));
	tgm_engines_line(engines, sizeof(engines));
	if (verdict == TGM_CTXSW_FATAL) {
		tgm_engine_state(regs, sizeof(regs));
		nvxnu_panic("tegra-gm20b: %s stuck loading %s's channel %u context again after an "
			    "engine reset under it, nothing completed since: %s; engines %s; %s",
			    engine, chan->owner, chan->chid, state, engines, regs);
	}
	chan->load_reset = 1;
	chan->load_reset_done = done;
	scnprintf(load_line, sizeof(load_line),
		  "tegra-gm20b: %s stuck loading %s's channel %u context (%s): the engine reset "
		  "under it, nothing killed: %s; engines %s\n",
		  engine, chan->owner, chan->chid, who, state, engines);
	switch_linux_log_line(load_line);
	return verdict;
}

enum tgm_ctxsw tgm_ctxsw_load_decide(int chid, const char *engine)
{
	enum tgm_ctxsw verdict = TGM_CTXSW_RESET;

	mutex_lock(&tgm_live_lock);
	verdict = load_verdict(live_by(0, chid), engine, "context-switch timeout");
	mutex_unlock(&tgm_live_lock);
	return verdict;
}

int tgm_load_recover_locked(struct tgm_chan *chan, const char *who)
{
	const char *engine = tgm_recover_loading(tgm_gpu.device, (int)chan->chid);

	if (!engine || load_verdict(chan, engine, who) != TGM_CTXSW_RELOAD)
		return 0;
	return tgm_recover_reload(tgm_gpu.device, (int)chan->chid) > 0;
}
