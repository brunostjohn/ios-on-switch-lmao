// SPDX-License-Identifier: BSD-2-Clause-Patent
// A kind of picture NVENC refuses (a P picture, or a B picture of a reordering session) is tried again at the next
// picture or group. After AVE_BACKOFF_IN_A_ROW refusals in a row the session codes without that kind for a run of
// pictures or groups, AVE_BACKOFF_FIRST at first and twice as long each time a try after a run is refused again, up to
// AVE_BACKOFF_MAX; one coded picture of the kind ends it.
#ifndef AVE_BACKOFF_H
#define AVE_BACKOFF_H
#include <stdbool.h>
#include <stdint.h>

enum {
    AVE_BACKOFF_IN_A_ROW = 3,
    AVE_BACKOFF_FIRST = 8,
    AVE_BACKOFF_MAX = 512,
};

typedef struct {
    uint32_t refusals;  // in a row
    uint32_t run;       // the last run's length, 0 before any
    uint32_t left;      // of the run under way
} AVEBackoff;

static inline bool ave_backoff_holds(const AVEBackoff *b)
{
    return b->left != 0;
}

// A picture or group coded without the kind.
static inline void ave_backoff_spend(AVEBackoff *b)
{
    if (b->left)
        b->left--;
}

// The run this refusal starts, for the caller's line; 0 when it starts none.
static inline uint32_t ave_backoff_refused(AVEBackoff *b)
{
    if (++b->refusals < AVE_BACKOFF_IN_A_ROW)
        return 0;
    b->run = !b->run ? AVE_BACKOFF_FIRST : b->run * 2 < AVE_BACKOFF_MAX ? b->run * 2 : AVE_BACKOFF_MAX;
    b->left = b->run;
    return b->run;
}

static inline void ave_backoff_coded(AVEBackoff *b)
{
    b->refusals = 0;
    b->run = 0;
}

#endif
