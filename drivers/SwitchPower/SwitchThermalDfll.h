// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchThermal's DFLL mode (top of SwitchThermal.cpp): going on the DFLL, and what happens when it cannot. Included by
// SwitchThermal.cpp; test/switch_thermal_dfll_test.cpp includes it with a stand-in SwitchThermal. Work loop only.
//
// Only -ENODEV from tegra_dfll_cpu_prepare (the chip's revision, speedo fuses or CVB row rule the DFLL out) keeps the
// CPU off the DFLL for the boot. Any other failure (prepare, enable, the governor's start, the DFLL's restore or a
// PLLX relock after SC7, a DFLL no longer enabled under the governor) leaves the CPU on the PLL ladder, and the DFLL
// is tried again at the first thermal evaluation (a SOC_THERM degree, the TMP451 ALERT, a raise) once 1 s has passed,
// then 2, 4 ... up to 512 s for each further failure in a row. A rate the DFLL refuses keeps the DFLL and its governor:
// switchperf::refused holds that row back a while and the governor asks the nearest one meanwhile.

// A DFLL step that failed: the PLL ladder from here (raiseCpu goes on to it), the DFLL again after its wait.
void SwitchThermal::dfllFailed(const char *what, const char *why)
{
    dfllMode = governed = false;
    dfllQuiet = !switch_gov_retry_failed(&dfllRetry, mach_absolute_time(), dfllRetryUnit);
    if (!dfllQuiet) {
        IOLog("SwitchPerf: %s (%s); the PLL ladder until the DFLL is tried again, %u failed in a row\n", what, why,
              dfllRetry.failures);
    }
}

// Back to the DFLL mode once the wait after a failure is over, its registers restored first if SC7 lost them.
bool SwitchThermal::dfllAgain()
{
    if (dfllMode || !dfllWanted || !dfllRetry.failures || !switch_gov_retry_due(&dfllRetry, mach_absolute_time()))
        return false;
    if (dfllResume) {
        char why[160] = "";
        const int err = tegra_dfll_cpu_resume(why, sizeof(why));
        if (err) {
            char line[176];
            snprintf(line, sizeof(line), "%d: %s", err, why);
            dfllFailed("DFLL not restored after sleep", line);
            return false;
        }
        dfllResume = false;
    }
    dfllMode = governed = true;
    return true;
}

// An evaluation with the CPU raised on the PLL ladder: once the DFLL's wait is over, down to the inherited clocks
// (enable starts from PLLX at the boot rate) and up again, on the DFLL if it starts.
bool SwitchThermal::retryDfll(int soc)
{
    if (!perfRaised || perfSettled || !dfllAgain())
        return false;
    perfRaised = false;
    if (!lowerLadder("the DFLL is tried again", nullptr)) {
        dfllMode = governed = false;
        return false;
    }
    return raiseCpu(soc);
}

// After SC7 (switch_thermal_resume): the DFLL's registers, which enable needs again.
void SwitchThermal::resumeDfll()
{
    if (!dfllWanted)
        return;
    char why[160] = "";
    const int err = tegra_dfll_cpu_resume(why, sizeof(why));
    dfllResume = err != 0;
    if (err) {
        char line[176];
        snprintf(line, sizeof(line), "%d: %s", err, why);
        dfllFailed("DFLL not restored after sleep", line);
    }
}

// The rate of `index` refused by the DFLL, which still runs the rate applied before: the governor asks that row again
// after a wait. One line for the first refusal in a row and each power of two.
void SwitchThermal::dfllRefused(unsigned index, unsigned long hz, int err)
{
    const unsigned inRow = switchperf::refused(index);
    if (inRow & (inRow - 1u))
        return;
    IOLog("SwitchPerf: the DFLL refused %lu MHz (%d), %u in a row; the governor asks it again after a wait\n",
          hz / 1000000, err, inRow);
}

// DFLL mode raise: prepare once, set the thermal indices for this temperature, then enable at the 1785 MHz row.
bool SwitchThermal::raiseDfll(int soc)
{
    char why[256] = "";
    char line[272];
    int err = dfllPrepared ? 0 : tegra_dfll_cpu_prepare(why, sizeof(why));
    if (err == -ENODEV) {
        dfllWanted = dfllMode = governed = false;
        IOLog("SwitchPerf: the DFLL does not fit this chip (%s); PLL mode for this boot\n", why);
        return false;
    }
    if (!err) {
        dfllPrepared = true;
        dfllFloor = dfllCap = ~0u;
        updateDfllThermal(soc);
        err = tegra_dfll_cpu_enable(kDfllSafeHz, why, sizeof(why));
    }
    if (err) {
        snprintf(line, sizeof(line), "%d: %s", err, why);
        dfllFailed("DFLL not used", line);
        return false;
    }
    perfRaised = true;
    ++perfRaises;
    dfllTopHz = settleDfllTop();  // 1785 MHz only as measured at or under it (fixed and governed modes alike)
    if (governed && !startGovernor(why, sizeof(why))) {
        restoreCpu("no governor", false);
        dfllFailed("CPU governor not started", why);
        return false;
    }
    const uint32_t failed = switch_gov_retry_cleared(&dfllRetry);
    char state[224];
    tegra_dfll_cpu_describe(dfllTopHz, state, sizeof(state));
    const unsigned mhz = switchclk::readCpu(car).kHz / 1000;
    publish("CPUClockMHz", mhz);
    publish("CPURaises", perfRaises);
    snprintf(line, sizeof(line), "; the DFLL works again after %u failed tries", failed);
    IOLog("SwitchPerf: CPU on the DFLL at %lu MHz%s (raise %u; SoC %d.%d C)%s: %s\n", dfllTopHz / 1000000,
          governed ? ", on demand from here" : "", perfRaises, soc / 1000, soc % 1000 / 100, failed ? line : "", state);
    if (governed)
        IOLog("SwitchPerf: %s\n", why);  // the governor's table and tunables
    return true;
}
