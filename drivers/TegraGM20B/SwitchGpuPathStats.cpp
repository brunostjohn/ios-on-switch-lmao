// SPDX-License-Identifier: MIT
#include "SwitchGpuPathStats.h"
#include "TegraGM20BGpu.h"
extern "C" {
#include "TegraGM20BSched.h"
}
#if SWITCH_GPU_PATH_STATS
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"

extern "C" int proc_selfpid(void);

namespace
{
const char *const kNames[SWITCH_GPU_PATH_COUNT] = {
	"internal_jobs", "external_jobs", "unlocked_writes", "volatile_binds", "lean_copy_jobs",
	"control_bos",   "cpu_completes", "ranged_uses",     "visible_waits",  "native_views",
	"native_reads",  "native_tiled_views", "event_jobs",
	"ready_arms",    "submit_jobs",   "set_jobs",        "stale_sets",     "address_jobs",
	"shared_uses",   "relayout_fresh", "relayout_deferred", "relayout_lazy", "relayout_never",
	"relayout_moved",
	"quiet_jobs",    "wake_asks",
};

// Every path is taken in the syscall or trap of the process it is for, so each process counts its own: a reader of
// kern.switch_gpu_paths sees what its own work took, which the compositor's frames do not move. The least recently
// counting process gives up its slot.
constexpr uint32_t kProcesses = 16;

struct Process {
	int pid;
	uint64_t last;
	uint64_t counts[SWITCH_GPU_PATH_COUNT];
};

IOLock *lock;
Process processes[kProcesses];
uint64_t window[SWITCH_GPU_PATH_COUNT];

IOLock *processLock()
{
	if (IOLock *made = __atomic_load_n(&lock, __ATOMIC_ACQUIRE))
		return made;
	IOLock *made = IOLockAlloc();
	IOLock *expected = nullptr;
	if (made && !__atomic_compare_exchange_n(&lock, &expected, made, false, __ATOMIC_ACQ_REL,
						 __ATOMIC_ACQUIRE)) {
		IOLockFree(made);
		return expected;
	}
	return made;
}

// processLock held
Process *slot(int pid, bool claim)
{
	Process *oldest = &processes[0];
	for (Process &p : processes) {
		if (p.pid == pid)
			return &p;
		if (p.last < oldest->last)
			oldest = &p;
	}
	if (!claim)
		return nullptr;
	*oldest = {pid, 0, {}};
	return oldest;
}

void format(char *line, size_t size, size_t used, const uint64_t *counts)
{
	for (uint32_t i = 0; i < SWITCH_GPU_PATH_COUNT && used < size; ++i) {
		const int wrote = snprintf(line + used, size - used, "%s%s=%llu", used ? " " : "", kNames[i],
					   static_cast<unsigned long long>(counts[i]));
		used += wrote > 0 ? static_cast<size_t>(wrote) : 0;
	}
}

int pathsSysctl SYSCTL_HANDLER_ARGS
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	char line[512];
	switch_gpu_path_line(line, sizeof(line));
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

struct sysctl_oid gPathsOid;
}  // namespace

extern "C" void switch_gpu_path_taken(enum switch_gpu_path path, uint32_t count)
{
	if (!count)
		return;
	__atomic_fetch_add(&window[path], count, __ATOMIC_RELAXED);
	IOLock *held = processLock();
	if (!held)
		return;
	IOLockLock(held);
	Process *p = slot(proc_selfpid(), true);
	p->last = mach_absolute_time();
	p->counts[path] += count;
	IOLockUnlock(held);
}

extern "C" void switch_gpu_path_job(uint32_t visibility, int unlocked_writes, int lean_copy,
				    uint32_t ranged_uses)
{
	if (visibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT)
		switch_gpu_path_taken(visibility & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY
					      ? SWITCH_GPU_PATH_EXTERNAL_JOB
					      : SWITCH_GPU_PATH_INTERNAL_JOB,
				      1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_UNLOCKED_WRITE, unlocked_writes ? 1 : 0);
	switch_gpu_path_taken(SWITCH_GPU_PATH_LEAN_COPY_JOB, lean_copy ? 1 : 0);
	switch_gpu_path_taken(SWITCH_GPU_PATH_RANGED_USE, ranged_uses);
	switch_gpu_path_taken(SWITCH_GPU_PATH_EVENT_JOB,
			      (visibility & SWITCH_GPU_SUBMIT_FORCE_FENCE) ? 1 : 0);
}

extern "C" void switch_gpu_path_window(char *line, size_t size)
{
	uint64_t counts[SWITCH_GPU_PATH_COUNT];
	for (uint32_t i = 0; i < SWITCH_GPU_PATH_COUNT; ++i)
		counts[i] = __atomic_exchange_n(&window[i], 0, __ATOMIC_RELAXED);
	if (size)
		line[0] = 0;
	format(line, size, 0, counts);
}

extern "C" void switch_gpu_path_line(char *line, size_t size)
{
	if (!size)
		return;
	const int pid = proc_selfpid();
	uint64_t counts[SWITCH_GPU_PATH_COUNT] = {};
	if (IOLock *held = processLock()) {
		IOLockLock(held);
		if (const Process *p = slot(pid, false))
			memcpy(counts, p->counts, sizeof(counts));
		IOLockUnlock(held);
	}
	const int head = snprintf(line, size, "flags=%#x pid=%d", switch_gpu_info_flags(), pid);
	format(line, size, head > 0 ? static_cast<size_t>(head) : 0, counts);
	uint32_t preempts = 0;
	uint64_t preemptMax = 0;
	tgm_hold_samples_take(pid, &preempts, &preemptMax);
	const size_t used = strlen(line);
	if (used < size)
		snprintf(line + used, size - used, " preempts=%u preempt_max_us=%llu", preempts,
			 static_cast<unsigned long long>(preemptMax / 1000));
}

extern "C" void switch_gpu_path_register(void)
{
	switch_sysctl_oid(&gPathsOid, &sysctl__kern_children, "switch_gpu_paths",
			  static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED),
			  pathsSysctl, "A",
			  "GM20B capability flags, and the paths behind them the reading process took");
	sysctl_register_oid(&gPathsOid);
}
#endif
