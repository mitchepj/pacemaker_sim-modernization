/* ------------------------------------------------------------------------
 * pacer_core.c
 *
 * FAKE core pacing timing-cycle state machine - the "brain" of the sim.
 * NOT REAL. See README.md at project root.
 *
 * This is the file with the most concentrated legacy smell:
 *   - EVERY global declared in pacer.h is DEFINED here, in one giant
 *     block, regardless of which module "really" owns it
 *   - pacer_core_handle_state() is a ~250 line function with a switch
 *     statement whose cases fall through to goto labels for shared
 *     cleanup, mixing three different control-flow styles
 *   - dual/single chamber logic is handled with nested ifs INSIDE the
 *     switch cases instead of being dispatched to modes.c (which has
 *     its own, only loosely related, parallel copy of similar logic)
 * ------------------------------------------------------------------------
 */

#include "pacer.h"

/* =========================================================================
 * GLOBAL STATE DEFINITIONS
 * ========================================================================= */

PacerState_t   g_state = ST_INIT;
PaceMode_t     g_mode = MODE_DDD;
UINT32         g_tick_ms = 0;
UINT32         g_last_a_evt_ms = 0;
UINT32         g_last_v_evt_ms = 0;
BOOL           g_a_refractory = FALSE;
BOOL           g_v_refractory = FALSE;
BOOL           g_mode_switched = FALSE;
UINT32         g_ms_since_boot = 0;

WORD           g_lrl_ms = DEFAULT_LRL_MS;
WORD           g_url_ms = DEFAULT_URL_MS;
WORD           g_avi_ms = DEFAULT_AVI_MS;
WORD           g_pvarp_ms = DEFAULT_PVARP_MS;
WORD           g_vrp_ms = DEFAULT_VRP_MS;
WORD           g_arp_ms = DEFAULT_ARP_MS;
WORD           g_pace_ampl_mv = DEFAULT_PACE_AMPL_MV;
BYTE           g_pace_width_ms = DEFAULT_PACE_WIDTH_MS;
WORD           g_a_sense_thresh_mv = A_SENSE_THRESH_MV;
WORD           g_v_sense_thresh_mv = V_SENSE_THRESH_MV;
BYTE           g_rate_resp_enabled = 0;

DWORD          g_batt_mv = BATT_NOMINAL_MV;
DWORD          g_lead_a_impedance = 500;
DWORD          g_lead_v_impedance = 550;
BOOL           g_eri_flag = FALSE;
BOOL           g_eol_flag = FALSE;

EgmSample_t    g_egm_hist[EGM_HISTORY_LEN];
UINT32         g_egm_hist_idx = 0;

LogEntry_t     g_log[MAX_LOG_ENTRIES];
UINT32         g_log_count = 0;
UINT32         g_log_head = 0;

BYTE           g_telemetry_buf[TELEMETRY_BUF_SZ];
UINT32         g_telemetry_len = 0;

UINT32         g_atr_counter = 0;
BOOL           g_ams_active = FALSE;
PaceMode_t     g_pre_ams_mode = MODE_DDD;

int            g_verbose = 0;

/* module-local scratch, not exposed via pacer.h, but still just a
 * second stash of near-global state instead of a proper context */
static EgmSample_t s_cur_sample;
static BYTE        s_evt_flags_this_tick;
static int         s_paced_a_this_tick;
static int         s_paced_v_this_tick;

static const char *s_state_names[] = {
    "INIT",
    "WAIT_LRL",
    "A_PACE",
    "AVI_WAIT",
    "V_PACE",
    "VRP",
    "PVARP",
    "ARP",
    "MODE_SWITCHED",
    "FAULT"
};

const char *pacer_state_name(PacerState_t st)
{
    if (st < 0 || st > ST_FAULT) {
        return "UNKNOWN";
    }
    return s_state_names[st];
}

void pacer_core_reset_cycle(void)
{
    g_last_a_evt_ms = g_tick_ms;
    g_last_v_evt_ms = g_tick_ms;
    g_a_refractory = FALSE;
    g_v_refractory = FALSE;
    g_state = ST_WAIT_LRL;
}

void pacer_core_init(void)
{
    g_tick_ms = 0;
    g_ms_since_boot = 0;
    g_state = ST_INIT;

    sensing_init();
    telemetry_init();
    battery_init();
    arrhythmia_init();

    mode_apply_defaults(g_mode);
    pacer_core_reset_cycle();

    s_evt_flags_this_tick = EVT_NONE;
    s_paced_a_this_tick = 0;
    s_paced_v_this_tick = 0;

    if (g_verbose) {
        printf("[pacer_core] init complete, mode=%s LRL=%ums URL=%ums\n",
               mode_to_string(g_mode), (unsigned)g_lrl_ms, (unsigned)g_url_ms);
    }
}

/* Emits a single atrial pace pulse: updates timers, refractory flags,
 * and logs the event. Called from several different places in
 * pacer_core_handle_state() below with slightly different surrounding
 * bookkeeping each time instead of being uniformly reused. */
static void issue_a_pace(void)
{
    g_last_a_evt_ms = g_tick_ms;
    g_a_refractory = TRUE;
    s_paced_a_this_tick = 1;
    s_evt_flags_this_tick |= EVT_A_PACE;

    telemetry_log_event(EVT_A_PACE, s_cur_sample.a_mv, s_cur_sample.v_mv);
}

static void issue_v_pace(void)
{
    g_last_v_evt_ms = g_tick_ms;
    g_v_refractory = TRUE;
    s_paced_v_this_tick = 1;
    s_evt_flags_this_tick |= EVT_V_PACE;

    telemetry_log_event(EVT_V_PACE, s_cur_sample.a_mv, s_cur_sample.v_mv);
}

/* Clears refractory windows once their timers expire. Called at the top
 * of every tick, before the state machine switch below. Nested ifs on
 * purpose - see file header. */
static void update_refractory_windows(void)
{
    if (g_a_refractory) {
        if (mode_is_dual_chamber(g_mode)) {
            if ((g_tick_ms - g_last_a_evt_ms) >= g_arp_ms) {
                g_a_refractory = FALSE;
            }
        } else {
            if ((g_tick_ms - g_last_a_evt_ms) >= g_arp_ms) {
                g_a_refractory = FALSE;
            }
        }
    }

    if (g_v_refractory) {
        if (mode_is_dual_chamber(g_mode)) {
            if ((g_tick_ms - g_last_v_evt_ms) >= g_vrp_ms) {
                g_v_refractory = FALSE;
            }
        } else {
            if ((g_tick_ms - g_last_v_evt_ms) >= g_vrp_ms) {
                g_v_refractory = FALSE;
            }
        }
    }
}

/* THE big one. Roughly 250 lines, one switch, several fallthrough-ish
 * gotos for shared "we paced something, log and bail" cleanup. This is
 * deliberately the messiest function in the whole codebase - a good
 * target for extract-function / extract-state-machine refactors.
 *
 * Returns 0 normally, -1 if it hit ST_FAULT (should not happen in this
 * sim, kept only because the fictional original had a fault path that
 * nothing could actually trigger anymore after several parameter range
 * checks were added elsewhere - dead-but-scary code, also authentic).
 */
int pacer_core_handle_state(void)
{
    int a_sensed;
    int v_sensed;
    UINT32 since_a;
    UINT32 since_v;

    a_sensed = 0;
    v_sensed = 0;

    if (!sensing_is_noise(s_cur_sample.a_mv, (INT16)g_a_sense_thresh_mv)) {
        if (sensing_check_atrial(s_cur_sample.a_mv)) {
            a_sensed = 1;
        }
    } else {
        s_evt_flags_this_tick |= EVT_NOISE;
    }

    if (!sensing_is_noise(s_cur_sample.v_mv, (INT16)g_v_sense_thresh_mv)) {
        if (sensing_check_ventricular(s_cur_sample.v_mv)) {
            v_sensed = 1;
        }
    } else {
        s_evt_flags_this_tick |= EVT_NOISE;
    }

    if (a_sensed && mode_is_atrial_sensed(g_mode)) {
        g_last_a_evt_ms = g_tick_ms;
        s_evt_flags_this_tick |= EVT_A_SENSE;
        telemetry_log_event(EVT_A_SENSE, s_cur_sample.a_mv, s_cur_sample.v_mv);
    } else {
        a_sensed = 0;
    }

    if (v_sensed && mode_is_ventricular_sensed(g_mode)) {
        g_last_v_evt_ms = g_tick_ms;
        s_evt_flags_this_tick |= EVT_V_SENSE;
        telemetry_log_event(EVT_V_SENSE, s_cur_sample.a_mv, s_cur_sample.v_mv);
    } else {
        v_sensed = 0;
    }

    since_a = g_tick_ms - g_last_a_evt_ms;
    since_v = g_tick_ms - g_last_v_evt_ms;

    switch (g_state) {

        case ST_INIT:
            g_state = ST_WAIT_LRL;
            goto done;

        case ST_WAIT_LRL:

            if (mode_is_dual_chamber(g_mode)) {

                if (a_sensed) {
                    g_state = ST_AVI_WAIT;
                    goto done;
                }

                if (mode_is_atrial_paced(g_mode)) {
                    if (since_a >= g_lrl_ms) {
                        if (!g_a_refractory) {
                            issue_a_pace();
                            g_state = ST_AVI_WAIT;
                            goto done;
                        }
                    }
                } else {
                    if (since_a >= g_lrl_ms) {
                        g_state = ST_AVI_WAIT;
                        goto done;
                    }
                }

                /* fallback: if we somehow got here with the ventricle
                 * overdue too, force a ventricular pace to be safe -
                 * this branch is the "shouldn't normally happen" path
                 * that nonetheless got left in from an old bug fix */
                if (since_v >= (UINT32)(g_lrl_ms + g_avi_ms)) {
                    if (!g_v_refractory) {
                        issue_v_pace();
                    }
                }

                goto done;

            } else {
                /* single chamber modes */

                if (mode_is_ventricular_paced(g_mode) && !mode_is_atrial_paced(g_mode)) {

                    if (v_sensed) {
                        goto done;
                    }

                    if (since_v >= g_lrl_ms) {
                        if (!g_v_refractory) {
                            issue_v_pace();
                        }
                    }

                    goto done;

                } else if (mode_is_atrial_paced(g_mode) && !mode_is_ventricular_paced(g_mode)) {

                    if (a_sensed) {
                        goto done;
                    }

                    if (since_a >= g_lrl_ms) {
                        if (!g_a_refractory) {
                            issue_a_pace();
                        }
                    }

                    goto done;

                } else {
                    /* neither, or both without dual-chamber tracking
                     * (VOO/AOO async): pace whichever is due */
                    if (since_a >= g_lrl_ms) {
                        if (!g_a_refractory) {
                            issue_a_pace();
                        }
                    }
                    if (since_v >= g_lrl_ms) {
                        if (!g_v_refractory) {
                            issue_v_pace();
                        }
                    }
                    goto done;
                }
            }
            /* unreachable */
            break;

        case ST_AVI_WAIT:

            if (v_sensed) {
                g_state = ST_WAIT_LRL;
                goto done;
            }

            if (since_a >= g_avi_ms) {
                if (!g_v_refractory) {
                    issue_v_pace();
                }
                g_state = ST_WAIT_LRL;
                goto done;
            }

            if (since_a >= g_url_ms) {
                /* URL safety fallback: don't let AVI wait run away
                 * forever if something upstream misbehaves */
                if (!g_v_refractory) {
                    issue_v_pace();
                }
                g_state = ST_WAIT_LRL;
                goto done;
            }

            goto done;

        case ST_A_PACE:
        case ST_V_PACE:
        case ST_VRP:
        case ST_PVARP:
        case ST_ARP:
            /* these states exist in the enum for readability/telemetry
             * purposes but the sim collapses their handling back into
             * ST_WAIT_LRL / ST_AVI_WAIT above - a remnant of an earlier
             * design that was never fully removed */
            g_state = ST_WAIT_LRL;
            goto done;

        case ST_MODE_SWITCHED:
            g_state = ST_WAIT_LRL;
            goto done;

        case ST_FAULT:
            if (g_verbose) {
                printf("[pacer_core] FAULT state reached at t=%lums\n",
                       (unsigned long)g_tick_ms);
            }
            return -1;

        default:
            g_state = ST_FAULT;
            return -1;
    }

done:
    return 0;
}

/* Top-level per-tick entry point, called from main.c's simulation loop.
 * dt_ms is how far to advance simulated time this call (the sim uses a
 * fixed 10ms step, but the function accepts an arbitrary dt for no
 * reason anyone documented). */
void pacer_core_tick(UINT32 dt_ms)
{
    UINT32 atrial_interval;

    g_tick_ms += dt_ms;
    g_ms_since_boot += dt_ms;

    s_evt_flags_this_tick = EVT_NONE;
    s_paced_a_this_tick = 0;
    s_paced_v_this_tick = 0;

    update_refractory_windows();

    sensing_generate_sample(&s_cur_sample);

    (void)pacer_core_handle_state();

    if (s_evt_flags_this_tick & EVT_A_SENSE) {
        atrial_interval = g_tick_ms - g_last_a_evt_ms;
        if (atrial_interval == 0) {
            atrial_interval = 1;
        }
        /* NOTE: this recomputes an interval that's basically always
         * going to be ~0 right after we just set g_last_a_evt_ms above
         * in handle_state(); the intended interval calc should use the
         * PREVIOUS event time, not the one we just overwrote. This is
         * a real, if harmless-in-this-sim, logic smell left in
         * deliberately - a keen refactor/analysis pass might flag it
         * even though it doesn't change observable output much given
         * how arrhythmia_check_atrial_tachy responds to it. */
        if (arrhythmia_check_atrial_tachy(atrial_interval)) {
            /* handled inside arrhythmia_run_ams_logic below */
        }
    }

    arrhythmia_run_ams_logic();

    battery_tick(dt_ms, s_paced_a_this_tick || s_paced_v_this_tick);

    if (battery_check_eol()) {
        if (g_state != ST_FAULT) {
            /* EOL doesn't actually halt the sim (a real device would
             * behave very differently); we just keep logging. This
             * mismatch between "flag says EOL" and "device keeps
             * pacing anyway" is intentional here purely as a synthetic
             * code smell (inconsistent handling of a critical flag),
             * NOT a statement about how real devices behave. */
        }
    }
}
