// SPDX-License-Identifier: MIT
// A watched present (SwitchGpuPresentWatch.h) over producers whose jobs may end quiet (switch_gpu_quiet_model.h), as
// gpu_present.c drives it: the offer arms the watch, asks each producer's channel for the interrupt at its seqno
// (tgm_chan_need_tokens, by switch_gpu_quiet_token_on), then looks at the producer once more. A producer's pass makes
// the watch due and the pass's end delivers it; the work item takes what the last look found, a kill or a teardown. A
// present the watch does not take is the fence-only present job: its acquires wait for the producers on the GPU, and its
// fence, on a kernel channel, always interrupts. The deadline (gpu_present_deadline.c watch_late) never delivers.
#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
#include "../SwitchGpuPresentWatch.h"
#include "switch_gpu_quiet_model.h"

namespace present_model
{
using quiet_model::Channel;

constexpr uint64_t kA = (uint64_t(2) << 32) | 17u;  // reset generation 3, uid 17
constexpr uint64_t kB = 18u;                        // reset generation 1, uid 18
constexpr uint64_t kWakeGraceNs = 50000000;         // TGM_PRESENT_WAKE_GRACE_NS

struct Producer {
	uint64_t id;  // tgm_chan.id: (reset generation - 1) << 32 | uid
	Channel channel;

	switch_gpu_backing_token token(uint32_t seqno) const
	{
		return {seqno, (id >> 32) + 1, SWITCH_GPU_ENGINE_GM20B, uint32_t(id)};
	}
};

enum class Deadline {
	Idle,     // nothing armed: delivered, or its work item has it
	Producer, // still running: the pending producer's hang path
	Follows,  // its interrupt is queued: the hang path at the wake's position (`followed`)
	Rearmed,  // its wake entry is owed by the channel's owner
	Grace,    // its interrupt is due: the pass's grace
	Panic,    // lost
};

class Display
{
public:
	using Step = std::function<void()>;

	explicit Display(std::vector<Producer *> producers) : producers_(std::move(producers))
	{
		current_ = this;
		for (auto *producer : producers_)
			producer->channel.passed = [this, producer] { pass(*producer); };
	}

	// tgm_present_watch_dependencies; 1: the caller queues the present job
	int offer(const switch_gpu_backing_token *tokens, uint32_t count, const Step &afterArm = {},
		  const Step &afterAsk = {})
	{
		if (!switch_gpu_present_watchable(tokens, count)) {
			job.assign(tokens, tokens + count);
			return 1;
		}
		late = 0;
		switch_gpu_present_watch_arm(&watch, &tokens[0]);
		if (afterArm)
			afterArm();
		if (asks)
			needTokens(tokens, count);
		if (afterAsk)
			afterAsk();
		if (physical(&watch.token))
			work();
		return 0;
	}

	// tgm_chan_need_tokens over the live channels
	void needTokens(const switch_gpu_backing_token *tokens, uint32_t count)
	{
		for (uint32_t i = 0; i < count; ++i)
			for (auto *producer : producers_)
				if (switch_gpu_quiet_token_on(&tokens[i], producer->id))
					producer->channel.need(uint32_t(tokens[i].value));
	}

	// the owners' thread calls
	void ownerWakes()
	{
		for (auto *producer : producers_)
			producer->channel.ownerWake();
	}

	// The present job's acquires pass once every producer is there; then its own fence interrupts.
	bool runJob()
	{
		for (const auto &token : job)
			if (physical(&token) != 1)
				return false;
		job.clear();
		++jobInterrupts;
		++presented;
		return true;
	}

	// tgm_chan_del: no pass of the channel comes after it
	void teardown(const Producer &producer)
	{
		if (switch_gpu_present_watch_fenced(&watch, producer.id, producer.channel.fence))
			work();
	}

	// gpu_present_deadline.c at or after TGM_PRESENT_NS
	Deadline deadline(uint64_t now)
	{
		if (watch.state != SWITCH_GPU_PRESENT_WATCH_ARMED)
			return Deadline::Idle;
		const int status = physical(&watch.token);
		if (!status)
			return Deadline::Producer;
		const Producer *producer = named(&watch.token);
		int where = SWITCH_GPU_QUIET_UNASKED;
		followed = uint32_t(watch.token.value);
		if (status > 0 && producer) {
			int recorded = 0;
			followed = switch_gpu_jobs_announced_by(&producer->channel.jobs, followed, &recorded);
			where = switch_gpu_quiet_interrupt(&producer->channel.quiet, uint32_t(watch.token.value),
							   producer->channel.fence, &followed, recorded);
		}
		if (where == SWITCH_GPU_QUIET_QUEUED)
			return Deadline::Follows;
		if (where == SWITCH_GPU_QUIET_OWED)
			return Deadline::Rearmed;
		if (!late)
			late = now;
		return now - late < kWakeGraceNs ? Deadline::Grace : Deadline::Panic;
	}

	switch_gpu_present_watch watch{};
	std::vector<switch_gpu_backing_token> job;
	uint32_t syncpoints[256] = {};
	bool asks = true;
	unsigned presented = 0, workItems = 0, jobInterrupts = 0;
	uint64_t late = 0;
	uint32_t followed = 0;

private:
	static inline Display *current_ = nullptr;
	std::vector<Producer *> producers_;

	static const Producer *named(const switch_gpu_backing_token *token)
	{
		for (auto *producer : current_->producers_)
			if (switch_gpu_quiet_token_on(token, producer->id))
				return producer;
		return nullptr;
	}

	static int physical(const switch_gpu_backing_token *token)
	{
		if (token->engine != SWITCH_GPU_ENGINE_GM20B)
			return int32_t(current_->syncpoints[token->queue] - uint32_t(token->value)) >= 0;
		const Producer *producer = named(token);
		return producer ? int32_t(producer->channel.fence - uint32_t(token->value)) >= 0 : -19;
	}

	// present_watch_fenced in the producer's pass, then tgm_present_pass at its end
	void pass(const Producer &producer)
	{
		switch_gpu_present_watch_fenced(&watch, producer.id, producer.channel.fence);
		if (watch.state != SWITCH_GPU_PRESENT_WATCH_DUE)
			return;
		if (switch_gpu_present_watch_take_visible(&watch, physical, physical))
			deliver();
		else
			work();
	}

	// present_watch_work
	void work()
	{
		++workItems;
		if (switch_gpu_present_watch_claim(&watch, physical, physical) !=
		    SWITCH_GPU_PRESENT_WATCH_PENDING)
			deliver();
	}

	void deliver()
	{
		switch_gpu_present_watch_reset(&watch);
		++presented;
	}
};
}  // namespace present_model
