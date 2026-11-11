// SPDX-License-Identifier: BSD-2-Clause-Patent
// An 'mp4v' session over test/MPEG4Clip.h's clip A (Advanced Simple) or B (Simple) (mpeg4_session.c), for avd_host.
#ifndef MPEG4_SESSION_H
#define MPEG4_SESSION_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stddef.h>

// One result line; true when every frame came out without an error from a session on (or off) hardware.
bool mpeg4_session_check(CFDictionaryRef spec, bool simple, bool wantHardware, char *line, size_t size);

#endif
