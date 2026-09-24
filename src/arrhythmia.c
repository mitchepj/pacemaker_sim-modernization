/* ------------------------------------------------------------------------
 * arrhythmia.c
 *
 * FAKE tachyarrhythmia detection + automatic mode switching (AMS).
 * NOT REAL. See README.md at project root.
 *
 * Legacy smells seeded here on purpose:
 *   - a counter-based detector that mixes "number of fast intervals in
 *     a row" with "elapsed time" in one variable, ambiguously
 *   - mode-switch entry/exit logic duplicated instead of being a small
 *     shared state helper
 *   - magic numbers for the detection window instead of pulling from
 *     pacer.h's DEFAULT_ATR_RATE_BPM in more than one spot
 * ------------------------------------------------------------------------
 */

#include "pacer.h"

#define ATR_DETECT_COUNT     8      /* consecutive fast intervals to declare AT */
#define ATR_EXIT_COUNT       6      /* consecutive normal intervals to exit AMS */

static int s_fast_streak = 0;
static int s_slow_streak = 0;
static UINT32 s_last_check_ms = 0;

void arrhythmia_init(void)
{
    s_fast_streak = 0;
    s_slow_streak = 0;
    s_last_check_ms = 0;

    g_atr_counter = 0;
    g_ams_active = FALSE;
    g_pre_ams_mode = g_mode;
}

void arrhythmia_reset_counters(void)
{
    s_fast_streak = 0;
    s_slow_streak = 0;
    g_atr_counter = 0;
}

/* interval_ms is the time since the previous atrial event. Converts
 * that to an implied instantaneous rate using integer math with a
 * fixed 60000 constant repeated here (also appears, differently
 * rounded, in main.c's status printout - another intentional seam). */
int arrhythmia_check_atrial_tachy(UINT32 interval_ms)
{
    UINT32 rate_bpm;

    if (interval_ms == 0) {
        return 0;
    }

    rate_bpm = 60000UL / interval_ms;

    if (rate_bpm >= DEFAULT_ATR_RATE_BPM) {
        s_fast_streak++;
        s_slow_streak = 0;
    } else {
        s_slow_streak++;
        if (s_fast_streak > 0) {
            /* only decay the fast streak once we've seen a couple of
             * normal beats in a row - an ad hoc debounce that isn't
             * documented anywhere except this comment */
            if (s_slow_streak >= 2) {
                s_fast_streak = 0;
            }
        }
    }

    g_atr_counter = (UINT32)s_fast_streak;

    if (s_fast_streak >= ATR_DETECT_COUNT) {
        return 1;
    }

    return 0;
}

/* Drives the mode-switch state transition. Called once per tick from
 * pacer_core.c. Has to reach into globals owned by modes.c
 * (mode_apply_defaults) and pacer_core.c (g_state) directly instead of
 * going through a narrower interface. */
void arrhythmia_run_ams_logic(void)
{
    if (!g_ams_active) {
        if (s_fast_streak >= ATR_DETECT_COUNT) {
            g_ams_active = TRUE;
            g_pre_ams_mode = g_mode;

            /* switch DDD/DDDR down to a non-tracking mode so the
             * ventricle doesn't get dragged along with the fast
             * atrial rate - this is the one bit of clinically
             * meaningful behavior seeded in this fake sim */
            if (g_mode == MODE_DDD || g_mode == MODE_DDDR) {
                g_mode = MODE_DDI;
                mode_apply_defaults(g_mode);
            } else if (g_mode == MODE_VDD) {
                g_mode = MODE_VVI;
                mode_apply_defaults(g_mode);
            }

            if (g_verbose) {
                printf("[arrhythmia] AMS ENGAGED at t=%lums (was %s, now %s)\n",
                       (unsigned long)g_tick_ms,
                       mode_to_string(g_pre_ams_mode),
                       mode_to_string(g_mode));
            }

            g_log[g_log_count % MAX_LOG_ENTRIES].flags |= EVT_MODE_SWITCH;
        }
    } else {
        if (s_slow_streak >= ATR_EXIT_COUNT) {
            g_ams_active = FALSE;

            if (g_verbose) {
                printf("[arrhythmia] AMS DISENGAGED at t=%lums (restoring %s)\n",
                       (unsigned long)g_tick_ms,
                       mode_to_string(g_pre_ams_mode));
            }

            g_mode = g_pre_ams_mode;
            mode_apply_defaults(g_mode);

            arrhythmia_reset_counters();
        }
    }

    s_last_check_ms = g_tick_ms;
    (void)s_last_check_ms; /* tracked but never read elsewhere - dead-ish
                             * state kept for authenticity; a refactor
                             * pass might reasonably flag/remove this */
}
