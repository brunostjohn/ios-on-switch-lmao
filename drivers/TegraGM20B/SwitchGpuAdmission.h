// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"

class SwitchGpuAdmission
{
public:
	SwitchGpuAdmission() = default;
	~SwitchGpuAdmission();
	SwitchGpuAdmission(const SwitchGpuAdmission &) = delete;
	SwitchGpuAdmission &operator=(const SwitchGpuAdmission &) = delete;
	int begin(const switch_gpu_backing_use *uses, uint32_t count,
		  const switch_gpu_backing_guard *guards, uint32_t guardCount,
		  switch_gpu_cpu_lease *const *leases, uint32_t leaseCount);
	// A job's writes before the job exists (TegraGM20BMethodWriterPromise): promised writers, which the display's
	// snapshots and CPU locks wait out and nothing else reads, refused by no CPU owner, as the GM20B submission it stands
	// for is not.
	int promise(const switch_gpu_backing_use *uses, uint32_t count);
	bool covers(const switch_gpu_backing_use *uses, uint32_t count) const;
	void end();

	bool active() const
	{
		return active_;
	}

private:
	int copyUses(const switch_gpu_backing_use *uses, uint32_t count, uint32_t *unique);

	switch_gpu_backing_use *uses_ = nullptr;
	uint32_t count_ = 0, capacity_ = 0;
	bool active_ = false, promised_ = false;
};

extern "C" int switch_gpu_backing_admission_ready(switch_gpu_backing *, uint32_t access);
extern "C" int switch_gpu_backing_conversion_ready(switch_gpu_backing *, uint32_t access);
extern "C" void switch_gpu_admission_changed(void);
