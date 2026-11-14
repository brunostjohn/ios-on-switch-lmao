// SPDX-License-Identifier: MIT
// deadline.c as the kernel builds it, its #include lines left out for gm20b_sched_stubs.h's stand-ins.
#include "drivers/TegraGM20B/test/gm20b_sched_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BSched.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"
#include "drivers/TegraGM20B/TegraGM20BCostHist.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"
#include "drivers/TegraGM20B/test/deadline_source.h"
