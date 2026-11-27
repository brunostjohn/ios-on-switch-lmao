// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchRefusal.h"

#include <stdarg.h>
#include <stdio.h>

#define SWITCH_REFUSAL_SLOTS  1024u
#define SWITCH_REFUSAL_PROBES 8u
#define SWITCH_REFUSAL_TEXT   224u

static uint64_t switch_refusal_seen[SWITCH_REFUSAL_SLOTS];
static uint32_t switch_refusal_overflows;

static uint64_t switch_refusal_tag(uint64_t key)
{
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebULL;
    key ^= key >> 31;
    return key != 0 ? key : 1;
}

bool switch_refusal_first(uint64_t key)
{
    const uint64_t tag = switch_refusal_tag(key);
    for (uint32_t probe = 0; probe < SWITCH_REFUSAL_PROBES; probe++) {
        uint64_t *slot = &switch_refusal_seen[(tag + probe) & (SWITCH_REFUSAL_SLOTS - 1)];
        uint64_t seen = __atomic_load_n(slot, __ATOMIC_RELAXED);
        if (seen == tag) {
            return false;
        }
        if (seen == 0) {
            if (__atomic_compare_exchange_n(slot, &seen, tag, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                return true;
            }
            if (seen == tag) {
                return false;
            }
        }
    }
    const uint32_t overflows = __atomic_add_fetch(&switch_refusal_overflows, 1, __ATOMIC_RELAXED);
    return (overflows & (overflows - 1)) == 0;
}

void switch_refusal_log(const char *domain, const char *reason_key, const char *text)
{
    fprintf(stderr, "refusal %s %s: %s\n", domain, reason_key, text);
}

static void switch_refusal_logv(const char *domain, const char *reason_key, const char *format, va_list args)
{
    char text[SWITCH_REFUSAL_TEXT];
    vsnprintf(text, sizeof(text), format, args);
    switch_refusal_log(domain, reason_key, text);
}

void switch_refusal(const char *domain, const char *reason_key, const char *format, ...)
{
    if (!switch_refusal_first(switch_refusal_key(domain, reason_key))) {
        return;
    }
    va_list args;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(args, format);
    switch_refusal_logv(domain, reason_key, format, args);
    va_end(args);
}

void switch_refusal_k(uint64_t key, const char *domain, const char *reason_key, const char *format, ...)
{
    if (!switch_refusal_first(key)) {
        return;
    }
    va_list args;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(args, format);
    switch_refusal_logv(domain, reason_key, format, args);
    va_end(args);
}
