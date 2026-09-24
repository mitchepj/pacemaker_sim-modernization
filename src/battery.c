/* ------------------------------------------------------------------------
 * battery.c
 *
 * FAKE battery / lead-impedance model. NOT REAL. See README.md.
 *
 * Legacy smells seeded here on purpose:
 *   - decay rate computed inline with several magic constants instead
 *     of named/tunable parameters
 *   - ERI/EOL thresholds checked redundantly in three different places
 *     (battery_tick, battery_check_eri, battery_check_eol) instead of
 *     computed once and cached
 *   - impedance "sampling" that's really just more LCG noise dressed up
 *     to look like a hardware read
 * ------------------------------------------------------------------------
 */

#include "pacer.h"

static UINT32 s_batt_ticks = 0;
static UINT32 s_pace_count_since_boot = 0;
static UINT32 s_imp_lcg = 0x9E3779B9UL;

static UINT32 imp_lcg_next(void)
{
    s_imp_lcg = (1103515245UL * s_imp_lcg + 12345UL) & 0x7FFFFFFFUL;
    return s_imp_lcg;
}

void battery_init(void)
{
    g_batt_mv = BATT_NOMINAL_MV;
    g_lead_a_impedance = 500;
    g_lead_v_impedance = 550;
    g_eri_flag = FALSE;
    g_eol_flag = FALSE;

    s_batt_ticks = 0;
    s_pace_count_since_boot = 0;
    s_imp_lcg = 0x9E3779B9UL ^ (UINT32)time(NULL);
}

/* Advances the fake battery model by dt_ms of simulated time. Pacing
 * pulses drain more than idle sensing, which is at least directionally
 * realistic, but the actual numbers here were "eyeballed" (there is no
 * cited source, consistent with a lot of real internal firmware
 * comments that say "tuned in the lab" and leave it at that). */
void battery_tick(UINT32 dt_ms, int paced_this_tick)
{
    UINT32 idle_drain;
    UINT32 pace_drain;

    s_batt_ticks += dt_ms;

    /* idle current draw: roughly 1 mV lost per 20000 sim-ms, scaled by
     * dt_ms - deliberately coarse integer math, can underflow to 0 for
     * small dt_ms which just means "no visible drain this tick" */
    idle_drain = dt_ms / 20000;

    pace_drain = 0;
    if (paced_this_tick) {
        s_pace_count_since_boot++;
        /* pacing pulses cost more, and cost scales (badly) with
         * amplitude*width in a way that was clearly copy-pasted from a
         * spreadsheet rather than derived from a real charge model */
        pace_drain = (UINT32)((g_pace_ampl_mv / 1000) * (g_pace_width_ms + 1));
        if (pace_drain == 0) {
            pace_drain = 1;
        }
    }

    if (g_batt_mv > (idle_drain + pace_drain)) {
        g_batt_mv -= (idle_drain + pace_drain);
    } else {
        g_batt_mv = 0;
    }

    /* redundant check #1 - also checked again by the two functions
     * below when called separately from main.c */
    if (g_batt_mv <= BATT_ERI_MV) {
        g_eri_flag = TRUE;
    }
    if (g_batt_mv <= BATT_EOL_MV) {
        g_eol_flag = TRUE;
    }

    /* lead impedance drifts slowly and noisily; occasionally simulate a
     * lead issue (impedance out of the healthy band) so telemetry has
     * something alarming to report every so often */
    g_lead_a_impedance = 400 + (imp_lcg_next() % 400);
    g_lead_v_impedance = 450 + (imp_lcg_next() % 400);

    if ((s_batt_ticks % 733000) < dt_ms) {
        /* rare simulated lead fault window */
        g_lead_v_impedance = LEAD_IMPEDANCE_HIGH + 500;
    }
}

/* redundant check #2 - recomputes the same comparison battery_tick()
 * already did, and also has the side effect of setting the flag (not
 * just reading it), which is surprising for a function named
 * "check_*". Kept as-is: this is the kind of surprising side effect a
 * refactor pass should flag and probably remove. */
int battery_check_eri(void)
{
    if (g_batt_mv <= BATT_ERI_MV) {
        g_eri_flag = TRUE;
        return 1;
    }
    return g_eri_flag ? 1 : 0;
}

/* redundant check #3, same shape as above */
int battery_check_eol(void)
{
    if (g_batt_mv <= BATT_EOL_MV) {
        g_eol_flag = TRUE;
        return 1;
    }
    return g_eol_flag ? 1 : 0;
}

DWORD lead_impedance_sample(int channel)
{
    /* channel 0 = atrial, anything else = ventricular; no bounds
     * checking, no named constants for the channel numbers */
    if (channel == 0) {
        return g_lead_a_impedance;
    }
    return g_lead_v_impedance;
}
