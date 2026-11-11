/* SPDX-License-Identifier: MIT */
/* What a GPU hang's line (gpu.c tgm_chan_describe) says beyond the registers: where GR's context switch is, and whether the
 * hung submission turned on the pixel interlock, whose ticket wait is a loop in the shader itself. */
#ifndef TEGRA_GM20B_HANG_DECODE_H
#define TEGRA_GM20B_HANG_DECODE_H
#include <stdint.h>
#include "TegraGM20BGpu.h"

#define TGM_HANG_SUBS 64   /* submissions remembered, by seqno */
#define TGM_HANG_PUSHES 64 /* a hung submission's pushes read */
#define TGM_HANG_LINE 512  /* a channel's line with the decode */

/* Each recent submission's own GPFIFO entries [first, end), by seqno (one 8-byte store a submission) */
struct tgm_hang_log {
	struct {
		uint32_t seqno;
		uint16_t first, end;
	} sub[TGM_HANG_SUBS];
};

static inline void tgm_hang_log_submit(struct tgm_hang_log *log, uint32_t seqno, uint32_t first,
				       uint32_t end)
{
	log->sub[seqno % TGM_HANG_SUBS].seqno = seqno;
	log->sub[seqno % TGM_HANG_SUBS].first = (uint16_t)first;
	log->sub[seqno % TGM_HANG_SUBS].end = (uint16_t)end;
}

/* PFIFO_ENGINE_STATUS (0x2640 + 8 engine): context id 11:0 (a TSG's when 12), ctx_status 15:13, next id 27:16 (a TSG's
 * when 28). "ch 3 -> ch 26, ctxsw switch": GR still on channel 3's context, switching to channel 26's. */
static inline void tgm_hang_engine(uint32_t eng, char *buf, unsigned long len)
{
	static const char *const status[8] = {"invalid", "valid", "2",    "3",
					      "4",       "load",  "save", "switch"};

	snprintf(buf, len, "%s %u -> %s %u, ctxsw %s", (eng & (1u << 12)) ? "tsg" : "ch",
		 eng & 0xfff, (eng & (1u << 28)) ? "tsg" : "ch", (eng >> 16) & 0xfff,
		 status[(eng >> 13) & 7]);
}

/* Debug kernels: beside a hang's line, what the channel it blames was itself waiting for and how
 * the channel the wait ran out on (late) stood. A blamed channel whose own order acquire waits on
 * a channel that keeps completing work is behind slow or starved work, not hung; one whose source
 * does not move either is part of a stall the line alone cannot tell apart. */
struct tgm_hang_detail {
	uint32_t chid;
	uint64_t stalled_ms; /* since its fence last moved, or it last went from idle to work */
	int waits;           /* its last order acquire has not passed */
	uint32_t wait_chid, wait_seqno, wait_at;
	int source_live; /* the acquire's source is a live channel: */
	uint32_t source_done, source_submitted; /* its fence */
	uint64_t source_ms;                     /* and how long it has not moved */
	uint32_t late_chid, late_done, late_submitted;
	uint64_t late_ms;
};

static inline void tgm_hang_detail_format(char *buf, unsigned long len,
					  const struct tgm_hang_detail *d)
{
	char wait[120] = "waits on no other channel";
	char source[64] = "";

	if (d->waits && d->source_live)
		snprintf(source, sizeof(source), " (ch %u: %u/%u, moved %llu ms ago)", d->wait_chid,
			 d->source_done, d->source_submitted, (unsigned long long)d->source_ms);
	if (d->waits)
		snprintf(wait, sizeof(wait), "waits on ch %u fence %u, at %u%s", d->wait_chid,
			 d->wait_seqno, d->wait_at, d->source_live ? source : " (no live channel)");
	snprintf(buf, len,
		 "ch %u (no progress for %llu ms) %s; late ch %u: %u/%u, moved %llu ms ago",
		 d->chid, (unsigned long long)d->stalled_ms, wait, d->late_chid, d->late_done,
		 d->late_submitted, (unsigned long long)d->late_ms);
}

/* The hung submission (`hung`, the first one not completed; none when it is past `submitted`): its interlocked states
 * (SET_PIXEL_SHADER_INTERLOCK_CONTROL with conflict detection on), its pushes read through their connection
 * (tgm_client_interlocks) from the GPFIFO entries the log names. Entries in [kernel_va, kernel_va + kernel_bytes) are
 * the kernel's own and skipped. Empty when it has none and every push was read. */
static inline void tgm_hang_note(const struct tgm_hang_log *log, uint32_t hung, uint32_t submitted,
				 const volatile uint32_t *gpfifo, uint32_t gpmask,
				 uint64_t kernel_va, uint64_t kernel_bytes, const void *client,
				 char *buf, unsigned long len)
{
	uint64_t va[TGM_HANG_PUSHES];
	uint64_t interlocks = 0;
	uint32_t dwords[TGM_HANG_PUSHES];
	uint32_t n = 0;
	uint32_t i = 0;
	int read = 0;

	buf[0] = 0;
	if (!client || !gpfifo || (int32_t)(submitted - hung) < 0 ||
	    log->sub[hung % TGM_HANG_SUBS].seqno != hung)
		return;
	for (i = log->sub[hung % TGM_HANG_SUBS].first;
	     i != log->sub[hung % TGM_HANG_SUBS].end && n < TGM_HANG_PUSHES; i = (i + 1) & gpmask) {
		const uint64_t at =
			gpfifo[2 * (size_t)i] | (uint64_t)(gpfifo[2 * (size_t)i + 1] & 0xff) << 32;
		if (at >= kernel_va && at < kernel_va + kernel_bytes)
			continue;
		va[n] = at;
		dwords[n++] = (gpfifo[2 * i + 1] >> 10) & 0x1fffff;
	}
	read = tgm_client_interlocks(client, va, dwords, n, &interlocks);
	if (read < 0) {
		snprintf(buf, len, " (hung submit %u not read: its connection is busy)", hung);
	} else if (interlocks) {
		snprintf(buf, len,
			 " (interlock ticket wait: %llu interlocked states in the hung submit %u,"
			 " %d/%u pushes read)",
			 (unsigned long long)interlocks, hung, read, n);
	} else if ((uint32_t)read < n) {
		snprintf(buf, len, " (hung submit %u: %d of %u pushes read, no interlocked state)",
			 hung, read, n);
	}
}
#endif
