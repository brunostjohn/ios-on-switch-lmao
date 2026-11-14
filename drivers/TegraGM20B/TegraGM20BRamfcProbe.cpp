// SPDX-License-Identifier: MIT
// debug.switch_gpu_ramfc (debug kernels, read-only): each live client channel's RAMFC timeslice read back against the
// value last written to it, once it has completed a job since (tgm_ramfc_probe), for HW Test gpu.ramfc_timeslice.
#include <IOKit/IOLib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "TegraGM20BGpu.h"

#if TGM_STATS
namespace
{
struct sysctl_oid gOid;

int ramfcSysctl SYSCTL_HANDLER_ARGS
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	if (req->newptr)
		return EPERM;
	struct tgm_ramfc_look look = {};
	if (tgm_ramfc_probe(&look))
		return EBUSY;
	char line[160];
	snprintf(line, sizeof(line),
		 "channels=%u checked=%u high=%u mismatched=%u first_chid=%u first_ramfc=%08x "
		 "first_expected=%08x",
		 look.channels, look.checked, look.high, look.mismatched, look.chid, look.ramfc,
		 look.expected);
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}
}  // namespace

extern "C" void tgm_ramfc_probe_register(void)
{
	switch_sysctl_oid(
		&gOid, &sysctl__debug_children, "switch_gpu_ramfc",
		static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED),
		ramfcSysctl, "A",
		"each client channel's RAMFC timeslice against the value last written");
	sysctl_register_oid(&gOid);
}
#endif
