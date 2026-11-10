// SPDX-License-Identifier: MIT
#pragma once

void tegra_gm20b_perf_deadline_ns(uint64_t until)
{
	const uint64_t now = nvxnu_nanotime();
	const uint64_t window = 20000000;
	if (!READ_ONCE(idle.on) || until <= now)
		return;
	if (until - now > window)
		until = now + window;
	uint64_t held = __atomic_load_n(&idle.service_until, __ATOMIC_SEQ_CST);
	while (held < until &&
	       !__atomic_compare_exchange_n(&idle.service_until, &held, until, false,
					    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
	}
	__atomic_store_n(&idle.last_busy, now, __ATOMIC_SEQ_CST);
	if (READ_ONCE(idle.low) || READ_ONCE(idle.dropping))
		nvxnu_work_schedule(idle.raise_w);
}
