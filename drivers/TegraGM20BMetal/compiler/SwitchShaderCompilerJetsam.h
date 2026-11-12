/* SPDX-License-Identifier: MIT */
/* The shader compiler's jetsam band and memory limit, the same on every iOS version. iOS 17's BlueTool stand-in spawns
 * it with them (posix_spawnattr_setjetsam_ext, as xpcproxy does for launchd). iOS 27's launchd ignores a daemon plist's
 * JetsamProperties and takes them from the Supplemental cryptex's jetsam properties overlay
 * (overlay/supplemental/com.apple.jetsamproperties.cryptex.plist, held to these by tools/package/shader_compiler_launch_test.py);
 * without an entry there a daemon gets J181's Daemon Override Global: band 40, ActiveSoft and InactiveHard 6 MB.
 *
 * The daemon declares no transactions (EnableTransactions false in its launchd plist, untracked on 17), so it is never
 * clean, ages into no idle band and is never idle-exited: the active limit is the one in force. Like Apple's compiler
 * services (J181's MTLCompilerService: ActiveSoft 150, InactiveHard 50) the active limit is soft, an EXC_RESOURCE
 * report and no kill: losing the compiler stalls every GPU client waiting on it. */
#ifndef SWITCH_SHADER_COMPILER_JETSAM_H
#define SWITCH_SHADER_COMPILER_JETSAM_H

/* J181's band for the always-on services the shell depends on (lsd, locationd, videocodecd, cameracaptured): above the
 * foreground app (100), below SpringBoard (160) and backboardd (170), whose GPU states it compiles */
#define SWITCH_SHADER_COMPILER_JETSAM_PRIORITY 140
/* MB, the active (soft) and the inactive (hard) limit alike */
#define SWITCH_SHADER_COMPILER_MEMORY_LIMIT_MB 64

#endif
