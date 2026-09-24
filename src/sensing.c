/* ------------------------------------------------------------------------
 * sensing.c
 *
 * FAKE sense-amplifier + synthetic intracardiac electrogram (EGM) model.
 * NOT REAL. See README.md at project root.
 *
 * Legacy smells seeded here on purpose:
 *   - a hand-rolled pseudo-random generator instead of using rand()
 *     consistently (half the file uses rand(), half uses this one)
 *   - deeply nested threshold/noise logic
 *   - magic numbers mixed with the #defines from pacer.h
 *   - a long function (sensing_generate_sample) doing several unrelated
 *     things: waveform synthesis, noise injection, AND history pushing
 * ------------------------------------------------------------------------
 */

#include "pacer.h"

/* home-grown LCG prng, seeded once, because "rand() drifts between
 * platforms" according to a comment nobody left an explanation for */
static UINT32 s_lcg_state = 0x2545F491UL;

static UINT32 lcg_next(void)
{
    s_lcg_state = (1103515245UL * s_lcg_state + 12345UL) & 0x7FFFFFFFUL;
    return s_lcg_state;
}

static int lcg_range(int lo, int hi)
{
    int span;
    UINT32 r;

    span = hi - lo;
    if (span <= 0) {
        return lo;
    }
    r = lcg_next();
    return lo + (int)(r % (UINT32)(span + 1));
}

/* running phase counters for the fake cardiac cycle waveform; these
 * really ought to be local state carried in a struct, but the original
 * (fictional) author put them here as statics instead */
static UINT32 s_a_phase_ms = 0;
static UINT32 s_v_phase_ms = 0;
static int    s_intrinsic_a_period_ms = 850;   /* ~70 bpm intrinsic atrial */
static int    s_intrinsic_v_period_ms = 900;   /* slightly slower AV conduction */
static int    s_dropped_beat_counter = 0;

void sensing_init(void)
{
    int i;

    s_lcg_state = (UINT32)time(NULL);
    if (s_lcg_state == 0) {
        s_lcg_state = 0xDEADBEEFUL;
    }

    s_a_phase_ms = 0;
    s_v_phase_ms = 0;
    s_dropped_beat_counter = 0;

    for (i = 0; i < EGM_HISTORY_LEN; i++) {
        g_egm_hist[i].a_mv = 0;
        g_egm_hist[i].v_mv = 0;
        g_egm_hist[i].t_ms = 0;
    }
    g_egm_hist_idx = 0;
}

/* Returns TRUE-ish (nonzero) if the sample should register as noise
 * rather than a legitimate depolarization. Written with an early-return
 * ladder mixed with a nested if, on purpose, to be inconsistent with
 * the rest of the file which prefers deep nesting. */
int sensing_is_noise(INT16 mv, INT16 thresh)
{
    int abs_mv;

    abs_mv = (mv < 0) ? -mv : mv;

    if (abs_mv < NOISE_FLOOR_MV) {
        return 1;
    }

    if (abs_mv >= thresh) {
        /* still might be noise if it's a single-sample spike; roll the
         * dice with the same skew the original telemetry team tuned
         * by hand in the field (undocumented, kept for "authenticity") */
        if (lcg_range(0, 999) < 3) {
            return 1;
        } else {
            return 0;
        }
    } else {
        if (abs_mv > (thresh - thresh / 4)) {
            if (lcg_range(0, 999) < 40) {
                return 1;
            }
        }
    }

    return 0;
}

/* This function does far too much: it synthesizes both channels for
 * this tick, occasionally drops a beat to emulate intrinsic bradycardia,
 * injects noise bursts, and then also pushes to history - all in one
 * place. That's deliberate. */
void sensing_generate_sample(EgmSample_t *out)
{
    INT16 a_mv;
    INT16 v_mv;
    int   a_edge;
    int   v_edge;
    int   noise_burst;

    if (out == NULL) {
        return;
    }

    a_mv = 0;
    v_mv = 0;
    a_edge = 0;
    v_edge = 0;

    s_a_phase_ms += 10; /* caller ticks in 10ms-ish steps in this sim */
    s_v_phase_ms += 10;

    if (s_a_phase_ms >= (UINT32)s_intrinsic_a_period_ms) {
        s_a_phase_ms = 0;

        s_dropped_beat_counter++;
        if (s_dropped_beat_counter >= 11 && (s_dropped_beat_counter % 11) == 0) {
            /* simulate an occasional dropped intrinsic atrial beat so
             * the LRL escape logic in pacer_core.c has something to do */
            a_edge = 0;
        } else {
            a_edge = 1;
        }
    }

    if (s_v_phase_ms >= (UINT32)s_intrinsic_v_period_ms) {
        s_v_phase_ms = 0;
        v_edge = 1;
    }

    if (a_edge) {
        a_mv = (INT16)(A_SENSE_THRESH_MV + lcg_range(50, 400));
    } else {
        a_mv = (INT16)lcg_range(-30, 30);
    }

    if (v_edge) {
        v_mv = (INT16)(V_SENSE_THRESH_MV + lcg_range(100, 900));
    } else {
        v_mv = (INT16)lcg_range(-40, 40);
    }

    /* occasional myopotential / EMI noise burst, tuned by feel */
    noise_burst = lcg_range(0, 4999);
    if (noise_burst < 7) {
        a_mv = (INT16)(a_mv + lcg_range(200, 600));
        v_mv = (INT16)(v_mv + lcg_range(200, 600));
    }

    out->a_mv = a_mv;
    out->v_mv = v_mv;
    out->t_ms = g_tick_ms;

    sensing_push_history(out);
}

void sensing_push_history(EgmSample_t *s)
{
    if (s == NULL) {
        return;
    }

    g_egm_hist[g_egm_hist_idx].a_mv = s->a_mv;
    g_egm_hist[g_egm_hist_idx].v_mv = s->v_mv;
    g_egm_hist[g_egm_hist_idx].t_ms = s->t_ms;

    g_egm_hist_idx++;
    if (g_egm_hist_idx >= EGM_HISTORY_LEN) {
        g_egm_hist_idx = 0;
    }
}

/* Returns 1 if this sample counts as a genuine atrial sense event,
 * taking refractory state and noise rejection into account. Nested
 * deeply on purpose; a refactor should probably extract guard clauses. */
int sensing_check_atrial(INT16 mv)
{
    int result;

    result = 0;

    if (g_a_refractory) {
        result = 0;
    } else {
        if (mv >= (INT16)g_a_sense_thresh_mv || mv <= -(INT16)g_a_sense_thresh_mv) {
            if (!sensing_is_noise(mv, (INT16)g_a_sense_thresh_mv)) {
                if ((g_tick_ms - g_last_a_evt_ms) > 40) {
                    result = 1;
                } else {
                    /* too close to previous event, treat as
                     * double-counting of the same depolarization */
                    result = 0;
                }
            } else {
                result = 0;
            }
        } else {
            result = 0;
        }
    }

    return result;
}

/* Same shape as sensing_check_atrial but not factored into a shared
 * helper - classic copy/paste divergence: the debounce window here is
 * 60ms instead of 40ms, and nobody documented why. */
int sensing_check_ventricular(INT16 mv)
{
    int result;

    result = 0;

    if (g_v_refractory) {
        return 0;
    }

    if (mv >= (INT16)g_v_sense_thresh_mv || mv <= -(INT16)g_v_sense_thresh_mv) {
        if (!sensing_is_noise(mv, (INT16)g_v_sense_thresh_mv)) {
            if ((g_tick_ms - g_last_v_evt_ms) > 60) {
                result = 1;
            } else {
                result = 0;
            }
        } else {
            result = 0;
        }
    } else {
        result = 0;
    }

    return result;
}
