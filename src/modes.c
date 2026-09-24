/* ------------------------------------------------------------------------
 * modes.c
 *
 * FAKE pacing-mode logic (AOO/VOO/AAI/VVI/AAT/VVT/AAIR/VVIR/VDD/DDI/DDD/
 * DDDR). NOT REAL. See README.md at project root.
 *
 * Legacy smells seeded here on purpose:
 *   - two parallel tables (enum in pacer.h, string table here) that must
 *     be kept in sync by hand
 *   - a giant switch statement in mode_step() with heavily duplicated
 *     per-mode bodies instead of composed capability checks
 *   - capability-check functions (mode_is_atrial_paced etc.) that
 *     re-derive the same classification the switch statement in
 *     mode_step() *also* re-derives independently, instead of sharing it
 * ------------------------------------------------------------------------
 */

#include "pacer.h"

/* parallel string table - MUST stay in sync with PaceMode_t in pacer.h.
 * order matters and there is no compile-time check tying them together. */
static const char *s_mode_names[MODE_COUNT] = {
    "AOO",
    "VOO",
    "AAI",
    "VVI",
    "AAT",
    "VVT",
    "AAIR",
    "VVIR",
    "VDD",
    "DDI",
    "DDD",
    "DDDR"
};

const char *mode_to_string(PaceMode_t m)
{
    if (m < 0 || m >= MODE_COUNT) {
        return "???";
    }
    return s_mode_names[m];
}

/* linear scan string->enum lookup, case-sensitive, no error reporting
 * beyond returning the default mode - a caller has no way to tell
 * "unknown string" apart from "explicitly asked for VVI" */
PaceMode_t mode_from_string(const char *s)
{
    int i;

    if (s == NULL) {
        return MODE_VVI;
    }

    for (i = 0; i < MODE_COUNT; i++) {
        if (strcmp(s, s_mode_names[i]) == 0) {
            return (PaceMode_t)i;
        }
    }

    return MODE_VVI;
}

/* --- capability predicates. Each one re-implements its own little
 *     truth table instead of sharing a single classification array,
 *     which is exactly the kind of duplication a refactor should spot. */

int mode_is_atrial_paced(PaceMode_t m)
{
    switch (m) {
        case MODE_AOO:
        case MODE_AAI:
        case MODE_AAT:
        case MODE_AAIR:
        case MODE_DDI:
        case MODE_DDD:
        case MODE_DDDR:
            return 1;
        default:
            return 0;
    }
}

int mode_is_ventricular_paced(PaceMode_t m)
{
    switch (m) {
        case MODE_VOO:
        case MODE_VVI:
        case MODE_VVT:
        case MODE_VVIR:
        case MODE_VDD:
        case MODE_DDI:
        case MODE_DDD:
        case MODE_DDDR:
            return 1;
        default:
            return 0;
    }
}

int mode_is_atrial_sensed(PaceMode_t m)
{
    if (m == MODE_AAI || m == MODE_AAT || m == MODE_AAIR) {
        return 1;
    }
    if (m == MODE_VDD || m == MODE_DDI || m == MODE_DDD || m == MODE_DDDR) {
        return 1;
    }
    return 0;
}

int mode_is_ventricular_sensed(PaceMode_t m)
{
    if (m == MODE_VVI || m == MODE_VVT || m == MODE_VVIR) {
        return 1;
    }
    if (m == MODE_VDD || m == MODE_DDI || m == MODE_DDD || m == MODE_DDDR) {
        return 1;
    }
    return 0;
}

int mode_is_rate_responsive(PaceMode_t m)
{
    return (m == MODE_AAIR || m == MODE_VVIR || m == MODE_DDDR) ? 1 : 0;
}

int mode_is_dual_chamber(PaceMode_t m)
{
    return (m == MODE_VDD || m == MODE_DDI || m == MODE_DDD || m == MODE_DDDR) ? 1 : 0;
}

/* Resets timing parameters to "textbook" defaults for a given mode.
 * In reality most of these overlap with DEFAULT_* from pacer.h, but the
 * original author re-typed several of them here with slightly different
 * per-mode tweaks instead of parameterizing - kept for authenticity. */
void mode_apply_defaults(PaceMode_t m)
{
    g_lrl_ms = DEFAULT_LRL_MS;
    g_url_ms = DEFAULT_URL_MS;
    g_avi_ms = DEFAULT_AVI_MS;
    g_pvarp_ms = DEFAULT_PVARP_MS;
    g_vrp_ms = DEFAULT_VRP_MS;
    g_arp_ms = DEFAULT_ARP_MS;
    g_pace_ampl_mv = DEFAULT_PACE_AMPL_MV;
    g_pace_width_ms = DEFAULT_PACE_WIDTH_MS;
    g_a_sense_thresh_mv = A_SENSE_THRESH_MV;
    g_v_sense_thresh_mv = V_SENSE_THRESH_MV;
    g_rate_resp_enabled = 0;

    switch (m) {
        case MODE_AOO:
            /* asynchronous, no sensing - widen "refractory" bookkeeping
             * so accidental sensing never fires even if someone flips
             * a flag later */
            g_a_sense_thresh_mv = 9999;
            break;

        case MODE_VOO:
            g_v_sense_thresh_mv = 9999;
            break;

        case MODE_AAI:
            g_avi_ms = 0; /* not applicable, but left nonzero would be wrong */
            break;

        case MODE_VVI:
            g_avi_ms = 0;
            break;

        case MODE_AAT:
            g_avi_ms = 0;
            break;

        case MODE_VVT:
            g_avi_ms = 0;
            break;

        case MODE_AAIR:
            g_rate_resp_enabled = 1;
            g_avi_ms = 0;
            break;

        case MODE_VVIR:
            g_rate_resp_enabled = 1;
            g_avi_ms = 0;
            break;

        case MODE_VDD:
            /* tracks atrium, paces ventricle only */
            g_avi_ms = 130;
            break;

        case MODE_DDI:
            g_avi_ms = 150;
            break;

        case MODE_DDD:
            g_avi_ms = 150;
            break;

        case MODE_DDDR:
            g_rate_resp_enabled = 1;
            g_avi_ms = 140;
            break;

        default:
            /* unknown mode, fall through with plain defaults - a real
             * device would probably fault here, this sim just logs */
            if (g_verbose) {
                printf("[modes] WARNING unknown mode %d, using raw defaults\n", (int)m);
            }
            break;
    }
}

/* mode_step() is the classic "one function per mode, pasted, tweaked
 * slightly, never reunified" smell. It returns a small integer status:
 *   0  = nothing paced this call
 *   1  = atrial pace issued
 *   2  = ventricular pace issued
 *   3  = both paced (dual-chamber async fallback)
 *  -1  = mode not recognized
 *
 * NOTE: a good chunk of this duplicates timing logic that also lives in
 * pacer_core.c's state machine. In the original (fictional) codebase
 * this function was added later by a different engineer who didn't want
 * to touch the "scary" state machine file, so a parallel implementation
 * grew here instead. That's a deliberate seeded smell, not a bug in the
 * sense that the sim still behaves coherently end to end via
 * pacer_core.c; mode_step() is presently only used for CLI dry-run mode
 * checks in main.c.
 */
int mode_step(PaceMode_t m, BYTE evt_flags)
{
    int paced_a;
    int paced_v;

    paced_a = 0;
    paced_v = 0;

    switch (m) {
        case MODE_AOO:
            if ((g_tick_ms - g_last_a_evt_ms) >= g_lrl_ms) {
                paced_a = 1;
            }
            break;

        case MODE_VOO:
            if ((g_tick_ms - g_last_v_evt_ms) >= g_lrl_ms) {
                paced_v = 1;
            }
            break;

        case MODE_AAI:
            if (evt_flags & EVT_A_SENSE) {
                /* inhibited, reset timer implicitly via caller */
            } else {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_lrl_ms) {
                    paced_a = 1;
                }
            }
            break;

        case MODE_VVI:
            if (evt_flags & EVT_V_SENSE) {
                /* inhibited */
            } else {
                if ((g_tick_ms - g_last_v_evt_ms) >= g_lrl_ms) {
                    paced_v = 1;
                }
            }
            break;

        case MODE_AAT:
            if (evt_flags & EVT_A_SENSE) {
                paced_a = 1; /* triggered mode paces ON the sensed event */
            } else {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_lrl_ms) {
                    paced_a = 1;
                }
            }
            break;

        case MODE_VVT:
            if (evt_flags & EVT_V_SENSE) {
                paced_v = 1;
            } else {
                if ((g_tick_ms - g_last_v_evt_ms) >= g_lrl_ms) {
                    paced_v = 1;
                }
            }
            break;

        case MODE_AAIR:
            if (evt_flags & EVT_A_SENSE) {
                /* inhibited */
            } else {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_lrl_ms) {
                    paced_a = 1;
                }
            }
            break;

        case MODE_VVIR:
            if (evt_flags & EVT_V_SENSE) {
                /* inhibited */
            } else {
                if ((g_tick_ms - g_last_v_evt_ms) >= g_lrl_ms) {
                    paced_v = 1;
                }
            }
            break;

        case MODE_VDD:
            if (evt_flags & EVT_A_SENSE) {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_avi_ms) {
                    paced_v = 1;
                }
            } else {
                if ((g_tick_ms - g_last_v_evt_ms) >= g_lrl_ms) {
                    paced_v = 1;
                }
            }
            break;

        case MODE_DDI:
            if (!(evt_flags & EVT_A_SENSE)) {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_lrl_ms) {
                    paced_a = 1;
                }
            }
            if (!(evt_flags & EVT_V_SENSE)) {
                if ((g_tick_ms - g_last_v_evt_ms) >= (g_lrl_ms + g_avi_ms)) {
                    paced_v = 1;
                }
            }
            break;

        case MODE_DDD:
            if (!(evt_flags & EVT_A_SENSE)) {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_lrl_ms) {
                    paced_a = 1;
                }
            }
            if (evt_flags & EVT_A_SENSE) {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_avi_ms) {
                    if (!(evt_flags & EVT_V_SENSE)) {
                        paced_v = 1;
                    }
                }
            } else {
                if (!(evt_flags & EVT_V_SENSE)) {
                    if ((g_tick_ms - g_last_v_evt_ms) >= (g_lrl_ms + g_avi_ms)) {
                        paced_v = 1;
                    }
                }
            }
            break;

        case MODE_DDDR:
            if (!(evt_flags & EVT_A_SENSE)) {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_lrl_ms) {
                    paced_a = 1;
                }
            }
            if (evt_flags & EVT_A_SENSE) {
                if ((g_tick_ms - g_last_a_evt_ms) >= g_avi_ms) {
                    if (!(evt_flags & EVT_V_SENSE)) {
                        paced_v = 1;
                    }
                }
            } else {
                if (!(evt_flags & EVT_V_SENSE)) {
                    if ((g_tick_ms - g_last_v_evt_ms) >= (g_lrl_ms + g_avi_ms)) {
                        paced_v = 1;
                    }
                }
            }
            break;

        default:
            return -1;
    }

    if (paced_a && paced_v) {
        return 3;
    } else if (paced_v) {
        return 2;
    } else if (paced_a) {
        return 1;
    }

    return 0;
}
