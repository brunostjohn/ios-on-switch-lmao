// SPDX-License-Identifier: GPL-2.0-only
/*
 * 24B119's dyld cache: its function-variant patch locations (the cache header's functionVariantInfo, all seven in
 * __TPRO_CONST of .25.dylddata and .54.dylddata) and the default implementation of each. Every table is per-process
 * and picks its other implementation only for per-process flag 2, has_sec_transition in apple[], which this kernel
 * never passes (bsd/kern/kern_exec.c pushes it only under HAS_MTE). kernel/tests/switch_fvariant_test.cpp derives
 * these from the cache's bytes.
 */
#ifndef SWITCH_FVARIANT_24B119_H
#define SWITCH_FVARIANT_24B119_H

#include "fvariant_types.h"

static const struct switch_fvariant_slot switch_fvariant_24B119_slots[] = {
    {UINT64_C(0x1ed9e8000), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 0 */
    {UINT64_C(0x1ed9e8008), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 1 */
    {UINT64_C(0x1ed9e8010), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 2 */
    {UINT64_C(0x1ed9e8018), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 3 */
    {UINT64_C(0x1ed9e8020), UINT64_C(0x1844f0cc8)}, /* QuartzCore, table 4 */
    {UINT64_C(0x1ed9e8028), UINT64_C(0x1af400988)}, /* libdispatch.dylib */
    {UINT64_C(0x2758d0000), UINT64_C(0x207abcb04)}, /* AVConference */
};

/* A2DCA908-9684-3507-B87F-FE22B886697A */
static const struct switch_fvariant_cache switch_fvariant_24B119 = {
    .uuid = {0xa2, 0xdc, 0xa9, 0x08, 0x96, 0x84, 0x35, 0x07, 0xb8, 0x7f, 0xfe, 0x22, 0xb8, 0x86, 0x69, 0x7a},
    .count = sizeof(switch_fvariant_24B119_slots) / sizeof(switch_fvariant_24B119_slots[0]),
    .slots = switch_fvariant_24B119_slots,
};

#endif
