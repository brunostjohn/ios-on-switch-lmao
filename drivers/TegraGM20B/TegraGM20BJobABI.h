/* SPDX-License-Identifier: MIT */
/* The job interface: persistent resource sets and SubmitJob. The raw stack key does not hash this header, so no unit it
 * hashes (gpu/BUILD.bazel raw_stack, RawStackIncludes) may include it, and TegraGM20BABI.h never does. Records keep
 * TegraGM20BABI.h's rules: little endian, naturally aligned, reserved fields zero on input. */
#ifndef TEGRA_GM20B_JOB_ABI_H
#define TEGRA_GM20B_JOB_ABI_H
#include <stdint.h>
#include "TegraGM20BABI.h"

enum {
	TegraGM20BMethodResourceSetCreate = 31,
	TegraGM20BMethodResourceSetRelease = 32,
	TegraGM20BMethodSubmitJob = 33,
	TegraGM20BMethodWriterPromise = 34,

	TegraGM20BMaxWriterPromises = 16,
	TegraGM20BMaxPromisedWrites = 32,

	TegraGM20BMaxResourceSets = 16,
	TegraGM20BMaxSetMembers = 4096,
	/* The connection's fence page (ChannelCreate's fence_token) holds each set's generation at this word plus its id
	 * minus one, so a client that gets kIOReturnNotReady can tell a stale set from a stale native view. */
	TegraGM20BSetGenerationWord = 768,
};

/* Method 31. Members are the BOs a SubmitJob names by set instead of by use, named as TegraGM20BAbiSurfaceUse records
 * (a handle, or with TegraGM20BSurfaceGpuAddress a VA in a live binding; the whole BO is the member). Write: the GPU may
 * write it; otherwise the set's jobs only read it. Only private memory can be a member: an import, an IOSurface, a native
 * view or a BO whose pages another connection or engine reaches is refused, and so is a BO in another set.
 * set 0 makes a new set; a live set's id replaces its members and returns a new generation. */
struct TegraGM20BAbiResourceSetCreate {
	uint64_t uses_address;
	uint32_t count, set;
};

struct TegraGM20BAbiResourceSetCreateReply {
	uint32_t set, generation;
	uint32_t fence_token, generation_offset; /* the generation's mirror: byte offset in the fence page */
};

/* Method 32. Later SubmitJobs naming the set are refused; its members' GPU work is unaffected. */
struct TegraGM20BAbiResourceSetRelease {
	uint32_t set, reserved;
};

/* What ends a set's generation, the kernel's counter index for each */
enum {
	TegraGM20BSetCauseFree = 0,     /* a member's BO was freed */
	TegraGM20BSetCauseLayout = 1,   /* a member's layout changed */
	TegraGM20BSetCauseShared = 2,   /* a member's pages were reached from outside the set's connection */
	TegraGM20BSetCauseKill = 3,     /* a channel that used the set was killed */
	TegraGM20BSetCauseReplace = 4,  /* method 31 replaced the members */
	TegraGM20BSetCauses = 5,
};

/* TegraGM20BAbiJob.flags */
enum {
	TegraGM20BJobExternal = 1u << 0,    /* the job's writes reach DRAM before its fence (CPU, display, other engines) */
	TegraGM20BJobHostWritten = 1u << 1, /* the CPU wrote memory the job reads through the GPU L2 since the last job */
	TegraGM20BJobHasEvents = 1u << 2,   /* a TegraGM20BAbiSubmitEvents and its uses follow the shared uses */
	TegraGM20BJobQuiet = 1u << 3,       /* accepted and not yet honoured: the job still raises its interrupt */
};

/* Method 33, reply TegraGM20BAbiExecReply. Pushes and shared uses are as SubmitAddress's (TegraGM20BPushFence allowed);
 * push VAs are not resolved to BOs: pushes in GPU-cached memory need HostWritten after CPU writes.
 * With a set, the job runs after the latest job of each other channel of the connection that named the set, as an
 * acquire on that channel's fence word, and its members take no per-use work. kIOReturnNotReady: set_generation is not
 * the set's (its fence-page word says the current one), or a shared use's native view is stale. A member named as a
 * shared use is refused. Shared uses keep SubmitAddress's ownership exactly, CPU-lock exclusion included.
 * Order between two channels is the order their SubmitJobs return in; calls that overlap are not ordered by the set.
 * A job submitted without the set that uses a member is not ordered against the set's jobs. */
struct TegraGM20BAbiJob {
	uint32_t channel, push_count;
	uint32_t set, set_generation;
	uint32_t shared_count, flags;
};

/* TegraGM20BAbiWriterPromise.flags: exactly one */
enum {
	TegraGM20BPromiseBegin = 1u << 0,
	TegraGM20BPromiseEnd = 1u << 1,
};

/* Method 34. A command buffer that counts as scheduled before its work reaches the kernel (its queue waits on a shared
 * event its committer may be about to signal) promises the surfaces it will write, as an Apple GPU's scheduled command
 * buffer already holds them: until the promise ends no present latches one of them (the display defers it, as for a CPU
 * writer) and a CPU lock of one waits. Begin: count records (TegraGM20BAbiSurfaceUse, handles of the connection's
 * imports, TegraGM20BSurfaceWrite) at uses_address and a cookie the connection has no promise under. End (count 0,
 * uses_address 0): the promise under cookie ends, once the job that writes them has returned from its submission or
 * never will. kIOReturnNotFound: a handle is not an import of the connection's. No CPU owner refuses a promise, and
 * the promised job's own submission (its conversions, layouts and imports) never waits for it. Closing the connection
 * ends its promises. */
struct TegraGM20BAbiWriterPromise {
	uint64_t cookie, uses_address;
	uint32_t count, flags;
};

#ifdef __cplusplus
#define TEGRA_GM20B_JOB_ASSERT(x, n) static_assert(sizeof(x) == (n), #x " layout")
#else
#define TEGRA_GM20B_JOB_ASSERT(x, n) _Static_assert(sizeof(struct x) == (n), #x " layout")
#endif
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiResourceSetCreate, 16);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiResourceSetCreateReply, 16);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiResourceSetRelease, 8);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiJob, 24);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiWriterPromise, 24);
#undef TEGRA_GM20B_JOB_ASSERT
#endif
