// SPDX-License-Identifier: MIT
// A SubmitJob record (TegraGM20BJobABI.h method 33) as the kernel copied it: its parts, or null when malformed.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "TegraGM20BJobABI.h"

struct SwitchGpuJobRecord {
	TegraGM20BAbiJob *job;
	TegraGM20BAbiPush *pushes;
	const TegraGM20BAbiSurfaceUse *uses;
	const TegraGM20BAbiSubmitEvents *events;

	static constexpr uint32_t kFlags = TegraGM20BJobExternal | TegraGM20BJobHostWritten |
					   TegraGM20BJobHasEvents | TegraGM20BJobQuiet;
	static constexpr size_t kMaxBytes = sizeof(TegraGM20BAbiJob) + TegraGM20BMaxJobPushes * sizeof(TegraGM20BAbiPush) +
					    TegraGM20BMaxJobUses * sizeof(TegraGM20BAbiSurfaceUse) +
					    sizeof(TegraGM20BAbiSubmitEvents) +
					    TegraGM20BMaxJobEvents * sizeof(TegraGM20BAbiEventUse);

	static SwitchGpuJobRecord parse(void *bytes, size_t size)
	{
		SwitchGpuJobRecord record = {};
		if (!bytes || size < sizeof(TegraGM20BAbiJob) || size > kMaxBytes)
			return record;
		auto *job = static_cast<TegraGM20BAbiJob *>(bytes);
		if (!job->channel || (job->flags & ~kFlags) || job->set > TegraGM20BMaxResourceSets ||
		    (job->set && !job->set_generation) || (!job->set && job->set_generation) ||
		    job->push_count > TegraGM20BMaxJobPushes || job->shared_count > TegraGM20BMaxJobUses)
			return record;
		size_t expected = sizeof(*job) + job->push_count * sizeof(TegraGM20BAbiPush) +
				  job->shared_count * sizeof(TegraGM20BAbiSurfaceUse);
		auto *pushes = reinterpret_cast<TegraGM20BAbiPush *>(job + 1);
		const auto *uses = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(pushes + job->push_count);
		const TegraGM20BAbiSubmitEvents *events = nullptr;
		if (job->flags & TegraGM20BJobHasEvents) {
			if (size < expected + sizeof(TegraGM20BAbiSubmitEvents))
				return record;
			events = reinterpret_cast<const TegraGM20BAbiSubmitEvents *>(uses + job->shared_count);
			if (events->reserved || events->wait_count > TegraGM20BMaxJobEvents ||
			    events->signal_count > TegraGM20BMaxJobEvents - events->wait_count)
				return record;
			expected += sizeof(*events) +
				    (events->wait_count + events->signal_count) * sizeof(TegraGM20BAbiEventUse);
			const auto *records = reinterpret_cast<const TegraGM20BAbiEventUse *>(events + 1);
			for (uint32_t i = 0; size == expected && i < events->wait_count + events->signal_count; ++i)
				if (!records[i].handle)
					return record;
		}
		if (size != expected)
			return record;
		return {job, pushes, uses, events};
	}

	bool valid() const
	{
		return job != nullptr;
	}
};
