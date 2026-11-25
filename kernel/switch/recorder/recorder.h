// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's decisions, its dump header and its status line (recorder.c, capture.c, store.c, watch.c), free
 * of kernel types so kernel/tests runs them on the host; the including file declares snprintf and strncmp.
 * Times are mach ticks.
 */
#ifndef SWITCH_RECORDER_H
#define SWITCH_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum switch_recorder_reason {
    SWITCH_RECORDER_NONE = 0,
    SWITCH_RECORDER_LATE_FRAME, /* the display: a present six periods or more after the one before */
    SWITCH_RECORDER_HANG,       /* a UI main thread's busy stretch (sched/busy.c) reached SWITCH_RECORDER_HANG_NS */
    SWITCH_RECORDER_SLOW_WORK, /* user space: a state creation or an import over its bound (debug.switch_recorder_slow) */
    SWITCH_RECORDER_MARK,      /* the left Joy-Con's Capture, a Bluetooth controller's, or debug.switch_recorder_mark */
    SWITCH_RECORDER_REASONS
};

static inline const char *switch_recorder_reason_name(uint32_t reason)
{
    static const char *const names[SWITCH_RECORDER_REASONS] = {"none", "late_frame", "hang", "slow_work", "mark"};

    return names[reason < SWITCH_RECORDER_REASONS ? reason : 0];
}

#define SWITCH_RECORDER_HANG_NS    250000000ull  /* HangTracer's bound for a hang */
#define SWITCH_RECORDER_GAP_NS     2000000000ull /* the least time between two automatic captures */
#define SWITCH_RECORDER_TAIL_NS    300000000ull  /* how long after a frame or a mark the ring is frozen */
#define SWITCH_RECORDER_EXPECT_NS  5000000000ull /* how long debug.switch_recorder_expect holds other triggers */
#define SWITCH_RECORDER_REFUSAL_NS 1000000000ull /* one refusal line per reason and thread in this long */
#define SWITCH_RECORDER_SLOW_MS    100u /* the least creation or import time debug.switch_recorder_slow takes */
#define SWITCH_RECORDER_ASKS       32u  /* triggers taken and not yet captured */
#define SWITCH_RECORDER_REFUSALS   16u  /* refusals not yet logged */
#define SWITCH_RECORDER_BOOT_KEEP  8u   /* a boot's first automatic captures, kept against its later ones */
#define SWITCH_RECORDER_SLOTS_MIN  16u
#define SWITCH_RECORDER_SLOTS_MAX  128u
#define SWITCH_RECORDER_SLOT_BYTES (6ull << 20) /* a slot's share of the volume: a compressed ring and stackshot */
#define SWITCH_RECORDER_FREE_SHARE 10u          /* the store takes at most 1/10 of the volume's free space */
#define SWITCH_RECORDER_HEADER     512u         /* a dump file's text header; the compressed body follows */
#define SWITCH_RECORDER_VERSION    2u

/* One trigger, taken and waiting for its capture. */
struct switch_recorder_ask {
    uint64_t at;  /* the trigger's time */
    uint64_t tid; /* the thread it names, 0: none */
    uint64_t value;
    uint32_t reason;
    uint32_t seq; /* the trigger's number since boot */
};

/* Which ask a capture serves first: the owner's mark, then a hang (its thread is still stuck), then the rest. */
static inline unsigned switch_recorder_rank(uint32_t reason)
{
    switch (reason) {
    case SWITCH_RECORDER_MARK:
        return 3;
    case SWITCH_RECORDER_HANG:
        return 2;
    case SWITCH_RECORDER_SLOW_WORK:
        return 1;
    default:
        return 0;
    }
}

static inline bool switch_recorder_automatic(uint32_t reason)
{
    return reason != SWITCH_RECORDER_MARK;
}

/* When an ask's ring may be frozen: a hang's at once, so the stackshot catches the hung thread; any other's a tail
 * later, so the ring holds what followed. */
static inline uint64_t switch_recorder_due(const struct switch_recorder_ask *a, uint64_t tail)
{
    return a->reason == SWITCH_RECORDER_HANG ? a->at : a->at + tail;
}

struct switch_recorder_asks {
    struct switch_recorder_ask a[SWITCH_RECORDER_ASKS];
    uint32_t n;
};

enum switch_recorder_refusal {
    SWITCH_RECORDER_ADMITTED = 0,
    SWITCH_RECORDER_REFUSED_GAP,   /* an automatic capture within the gap of the last one */
    SWITCH_RECORDER_REFUSED_QUEUED, /* an automatic ask of the same or a higher rank is already waiting */
    SWITCH_RECORDER_REFUSED_HELD,  /* debug.switch_recorder_expect holds the recorder for another thread */
    SWITCH_RECORDER_REFUSED_FULL,  /* no room for the ask: every place holds a mark */
    SWITCH_RECORDER_REFUSALS_KINDS
};

static inline const char *switch_recorder_refusal_name(unsigned refusal)
{
    static const char *const names[SWITCH_RECORDER_REFUSALS_KINDS] = {"admitted", "gap", "queued", "held", "full"};

    return names[refusal < SWITCH_RECORDER_REFUSALS_KINDS ? refusal : 0];
}

/* The waiting automatic ask, or n: none. At most one waits, so marks keep the rest of the queue. */
static inline uint32_t switch_recorder_queued_auto(const struct switch_recorder_asks *q)
{
    for (uint32_t i = 0; i < q->n; i++) {
        if (switch_recorder_automatic(q->a[i].reason)) {
            return i;
        }
    }
    return q->n;
}

/*
 * Whether an ask is taken, and if so into the queue. A mark is always taken while the queue has room, and takes the
 * waiting automatic ask's place when it has none. An automatic ask is turned away inside the gap since the last
 * automatic capture, while HW Test's hold (expect_tid until expect_until) names another thread, or while an automatic
 * ask of its rank or higher waits; one of a higher rank replaces the waiting one (a hang cannot wait behind a late
 * frame's tail). The held thread's own ask is never turned away by the gap.
 */
static inline unsigned switch_recorder_admit(struct switch_recorder_asks *q, const struct switch_recorder_ask *ask,
                                             uint64_t last_auto, uint64_t gap, uint64_t expect_tid,
                                             uint64_t expect_until)
{
    const uint32_t waiting = switch_recorder_queued_auto(q);
    const bool held = expect_tid != 0 && ask->at < expect_until;

    if (!switch_recorder_automatic(ask->reason)) {
        if (q->n < SWITCH_RECORDER_ASKS) {
            q->a[q->n++] = *ask;
            return SWITCH_RECORDER_ADMITTED;
        }
        if (waiting == q->n) {
            return SWITCH_RECORDER_REFUSED_FULL;
        }
        q->a[waiting] = *ask;
        return SWITCH_RECORDER_ADMITTED;
    }
    if (held && ask->tid != expect_tid) {
        return SWITCH_RECORDER_REFUSED_HELD;
    }
    if (!(held && ask->tid == expect_tid) && last_auto != 0 && ask->at - last_auto < gap) {
        return SWITCH_RECORDER_REFUSED_GAP;
    }
    if (waiting < q->n) {
        if (switch_recorder_rank(ask->reason) <= switch_recorder_rank(q->a[waiting].reason)) {
            return SWITCH_RECORDER_REFUSED_QUEUED;
        }
        q->a[waiting] = *ask;
        return SWITCH_RECORDER_ADMITTED;
    }
    if (q->n == SWITCH_RECORDER_ASKS) {
        return SWITCH_RECORDER_REFUSED_FULL;
    }
    q->a[q->n++] = *ask;
    return SWITCH_RECORDER_ADMITTED;
}

/*
 * The next ask to capture at `now`: the highest-ranked of those due, the earliest of equal rank, taken out of the
 * queue. False when none is due; *next is then the earliest due time left (UINT64_MAX: the queue is empty).
 */
static inline bool switch_recorder_take(struct switch_recorder_asks *q, uint64_t now, uint64_t tail,
                                        struct switch_recorder_ask *out, uint64_t *next)
{
    uint32_t best = q->n;

    *next = UINT64_MAX;
    for (uint32_t i = 0; i < q->n; i++) {
        const uint64_t due = switch_recorder_due(&q->a[i], tail);

        if (due > now) {
            *next = due < *next ? due : *next;
            continue;
        }
        if (best == q->n || switch_recorder_rank(q->a[i].reason) > switch_recorder_rank(q->a[best].reason) ||
            (switch_recorder_rank(q->a[i].reason) == switch_recorder_rank(q->a[best].reason) &&
             q->a[i].at < q->a[best].at)) {
            best = i;
        }
    }
    if (best == q->n) {
        return false;
    }
    *out = q->a[best];
    for (uint32_t i = best + 1; i < q->n; i++) {
        q->a[i - 1] = q->a[i];
    }
    q->n--;
    return true;
}

/*
 * A refusal is logged unless the last one logged for the same reason and thread came within `every`: a hang the gap
 * turns away is asked again at every scheduler tick it lasts.
 */
struct switch_recorder_refusal_seen {
    uint64_t at, tid;
    uint32_t reason;
};

static inline bool switch_recorder_refusal_logged(struct switch_recorder_refusal_seen *seen,
                                                  const struct switch_recorder_ask *ask, uint64_t every)
{
    if (seen->reason == ask->reason && seen->tid == ask->tid && seen->at != 0 && ask->at - seen->at < every) {
        return false;
    }
    seen->at = ask->at;
    seen->tid = ask->tid;
    seen->reason = ask->reason;
    return true;
}

/* A slot of the store as its file's header says, or unused. */
struct switch_recorder_slot {
    bool used;
    bool this_boot;
    bool mark;
    uint32_t seq; /* the capture's number in its boot */
};

/* The slots the store keeps for `free_bytes` on the volume. */
static inline unsigned switch_recorder_slots_for(uint64_t free_bytes)
{
    const uint64_t n = free_bytes / SWITCH_RECORDER_FREE_SHARE / SWITCH_RECORDER_SLOT_BYTES;

    if (n < SWITCH_RECORDER_SLOTS_MIN) {
        return SWITCH_RECORDER_SLOTS_MIN;
    }
    return n > SWITCH_RECORDER_SLOTS_MAX ? SWITCH_RECORDER_SLOTS_MAX : (unsigned)n;
}

/* Whether slot a goes before slot b among those a capture may overwrite: the older capture first. */
static inline bool switch_recorder_older(const struct switch_recorder_slot *a, const struct switch_recorder_slot *b)
{
    return a->seq < b->seq;
}

/*
 * The slot the next capture is written to, or -1 to drop it: a free slot; else another boot's, automatic before marks,
 * oldest first; else this boot's oldest automatic capture past its first SWITCH_RECORDER_BOOT_KEEP. A mark may then
 * take this boot's oldest automatic capture, then its oldest mark: an automatic capture never replaces a mark.
 */
static inline int switch_recorder_pick_slot(const struct switch_recorder_slot *s, unsigned n, bool mark)
{
    int best = -1;
    unsigned best_class = 0;

    for (unsigned i = 0; i < n; i++) {
        unsigned cls = 0;

        if (!s[i].used) {
            return (int)i;
        }
        if (!s[i].this_boot) {
            cls = s[i].mark ? 5 : 6;
        } else if (!s[i].mark && s[i].seq >= SWITCH_RECORDER_BOOT_KEEP) {
            cls = 4;
        } else if (mark && !s[i].mark) {
            cls = 3;
        } else if (mark) {
            cls = 2;
        }
        if (cls == 0) {
            continue;
        }
        if (best < 0 || cls > best_class || (cls == best_class && switch_recorder_older(&s[i], &s[best]))) {
            best = (int)i;
            best_class = cls;
        }
    }
    return best;
}

/* A capture as its dump file's header names it; the body is the stackshot then the ring, one raw deflate stream. */
struct switch_recorder_head {
    char boot[40];   /* the boot session's UUID */
    char kernel[40]; /* the kernel's UUID */
    uint32_t seq;    /* the capture's number in this boot */
    uint32_t ask;    /* its trigger's number in this boot */
    uint32_t reason;
    uint64_t tid, value;
    uint64_t at, shot_at, frozen_at, done_at; /* trigger, stackshot taken, ring frozen, both in memory */
    uint64_t shot_us, ring_us;                /* each step's own time */
    uint64_t tod_s, tod_us;                   /* the wall time the ring was frozen */
    uint64_t shot_bytes, ring_bytes, deflate_bytes;
    uint32_t ring_lost; /* 0, or why the capture has no ring: 1 the ring was another process's, 2 no memory for it */
};

/* The header text, padded with spaces to SWITCH_RECORDER_HEADER and ended by a newline; false when it does not fit. */
static inline bool switch_recorder_header(char *out, const struct switch_recorder_head *h)
{
    const int n = snprintf(
        out, SWITCH_RECORDER_HEADER,
        "SwitchRecorder %u boot=%s kernel=%s seq=%u ask=%u reason=%s tid=%llu value=%llu at=%llu shot_at=%llu "
        "frozen_at=%llu done_at=%llu shot_us=%llu ring_us=%llu tod=%llu.%06llu shot=%llu ring=%llu deflate=%llu "
        "ring_lost=%u",
        SWITCH_RECORDER_VERSION, h->boot, h->kernel, h->seq, h->ask, switch_recorder_reason_name(h->reason),
        (unsigned long long)h->tid, (unsigned long long)h->value, (unsigned long long)h->at,
        (unsigned long long)h->shot_at, (unsigned long long)h->frozen_at, (unsigned long long)h->done_at,
        (unsigned long long)h->shot_us, (unsigned long long)h->ring_us, (unsigned long long)h->tod_s,
        (unsigned long long)h->tod_us, (unsigned long long)h->shot_bytes, (unsigned long long)h->ring_bytes,
        (unsigned long long)h->deflate_bytes, h->ring_lost);

    if (n < 0 || (unsigned)n >= SWITCH_RECORDER_HEADER - 1) {
        return false;
    }
    for (unsigned i = (unsigned)n; i < SWITCH_RECORDER_HEADER - 1; i++) {
        out[i] = ' ';
    }
    out[SWITCH_RECORDER_HEADER - 1] = '\n';
    return true;
}

/* The value of " key=" in a header, 0 when absent. */
static inline uint64_t switch_recorder_header_field(const char *h, size_t n, const char *key)
{
    const size_t k = __builtin_strlen(key);

    for (size_t i = 0; i + k + 2 <= n; i++) {
        if (h[i] == ' ' && strncmp(h + i + 1, key, k) == 0 && h[i + 1 + k] == '=') {
            uint64_t v = 0;

            for (size_t j = i + 2 + k; j < n && h[j] >= '0' && h[j] <= '9'; j++) {
                v = v * 10 + (uint64_t)(h[j] - '0');
            }
            return v;
        }
    }
    return 0;
}

/* A slot as the header at its start says: in use when it is this version's header with its body written. */
static inline struct switch_recorder_slot switch_recorder_slot_of(const char *h, size_t n, const char *boot)
{
    struct switch_recorder_slot s = {false, false, false, 0};
    static const char magic[] = "SwitchRecorder 2 boot=";
    const size_t m = sizeof(magic) - 1;

    if (n < SWITCH_RECORDER_HEADER || strncmp(h, magic, m) != 0) {
        return s;
    }
    s.used = true;
    s.this_boot = strncmp(h + m, boot, __builtin_strlen(boot)) == 0 && h[m + __builtin_strlen(boot)] == ' ';
    s.seq = (uint32_t)switch_recorder_header_field(h, n, "seq");
    for (size_t i = 0; i + 13 <= n; i++) {
        if (strncmp(h + i, " reason=mark ", 13) == 0) {
            s.mark = true;
            break;
        }
    }
    return s;
}

/* The last capture the store wrote, and what it cost. */
struct switch_recorder_last {
    struct switch_recorder_head head;
    unsigned slot;
    uint64_t write_us, deflate_us;
};

/* The recorder's own steady cost, read at the status line. */
struct switch_recorder_cost {
    uint32_t ring;    /* events the ring holds */
    uint64_t span_ms; /* from its oldest event to now, once it has wrapped; 0 before */
    uint64_t samples; /* kperf timer samples taken since boot, all CPUs */
};

static inline uint64_t switch_recorder_events_s(const struct switch_recorder_cost *c)
{
    return c->span_ms ? (uint64_t)c->ring * 1000 / c->span_ms : 0;
}

struct switch_recorder_counts {
    uint32_t triggers[SWITCH_RECORDER_REASONS];
    uint32_t refused[SWITCH_RECORDER_REFUSALS_KINDS];
    uint32_t taken;    /* captures whose ring was another process's */
    uint32_t captures; /* captures in memory, since boot */
    uint32_t dumps;    /* captures written to the store */
    uint32_t dropped;  /* captures the store had no slot for */
    uint32_t no_ring;  /* captures kept without their ring: no memory for it */
    uint32_t pending;  /* captures in memory, not yet written */
    uint64_t pending_bytes;
    uint32_t slots;
};

/* debug.switch_recorder: key=value fields HW Test reads (KernelLine.fields). */
static inline int switch_recorder_status(char *out, size_t size, const struct switch_recorder_cost *c,
                                         const struct switch_recorder_counts *n, const struct switch_recorder_last *l,
                                         uint64_t age_ms)
{
    const struct switch_recorder_head *h = &l->head;

    return snprintf(
        out, size,
        "ring=%u span_ms=%llu events_s=%llu samples=%llu captures=%u dumps=%u pending=%u pending_kib=%llu slots=%u "
        "skipped=%u held=%u queued=%u full=%u dropped=%u no_ring=%u taken=%u late_frame=%u hang=%u slow_work=%u mark=%u "
        "last_reason=%s last_tid=%llu last_value=%llu last_slot=%u last_seq=%u last_trace_bytes=%llu "
        "last_shot_bytes=%llu last_deflate_bytes=%llu last_trace_us=%llu last_shot_us=%llu last_latency_us=%llu "
        "last_deflate_us=%llu last_write_us=%llu last_age_ms=%llu",
        c->ring, (unsigned long long)c->span_ms, (unsigned long long)switch_recorder_events_s(c),
        (unsigned long long)c->samples, n->captures, n->dumps, n->pending, (unsigned long long)(n->pending_bytes >> 10),
        n->slots, n->refused[SWITCH_RECORDER_REFUSED_GAP], n->refused[SWITCH_RECORDER_REFUSED_HELD],
        n->refused[SWITCH_RECORDER_REFUSED_QUEUED], n->refused[SWITCH_RECORDER_REFUSED_FULL], n->dropped, n->no_ring,
        n->taken, n->triggers[SWITCH_RECORDER_LATE_FRAME], n->triggers[SWITCH_RECORDER_HANG],
        n->triggers[SWITCH_RECORDER_SLOW_WORK], n->triggers[SWITCH_RECORDER_MARK], switch_recorder_reason_name(h->reason),
        (unsigned long long)h->tid, (unsigned long long)h->value, l->slot, h->seq, (unsigned long long)h->ring_bytes,
        (unsigned long long)h->shot_bytes, (unsigned long long)h->deflate_bytes, (unsigned long long)h->ring_us,
        (unsigned long long)h->shot_us, (unsigned long long)(h->done_at > h->at ? h->done_at - h->at : 0),
        (unsigned long long)l->deflate_us, (unsigned long long)l->write_us, (unsigned long long)age_ms);
}

/*
 * A UI main thread's hang: its busy stretch (sched/busy.c's, from its departures and dispatches) open since `woke` for
 * the bound. One capture per stretch, latched only once a trigger took it, so a stretch the gap or a waiting ask
 * turned away is asked again at the next tick it still lasts.
 */
struct switch_recorder_watch {
    uint64_t fired_woke; /* the stretch a trigger took, 0: none */
};

static inline bool switch_recorder_watch_due(const struct switch_recorder_watch *w, bool open, uint64_t woke,
                                             uint64_t now, uint64_t bound)
{
    return open && woke != 0 && now > woke && now - woke >= bound && woke != w->fired_woke;
}

static inline void switch_recorder_watch_fired(struct switch_recorder_watch *w, uint64_t woke)
{
    w->fired_woke = woke;
}

#endif
