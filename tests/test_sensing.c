/* ------------------------------------------------------------------------
 * test_sensing.c
 *
 * CHARACTERIZATION TEST SUITE for sensing.c.
 *
 * sensing.c seeds its internal LCG PRNG from time(NULL) in sensing_init(),
 * which was the root cause of the run-to-run non-determinism found during
 * Discovery. This suite neutralizes that via fake_time.c's link-time
 * interposition of time() (see that file's header comment) - a
 * test-harness-only technique. sensing.c itself is NOT modified.
 *
 * Golden values below were captured by direct execution against the
 * real, linked sensing.c under specific fake-time seeds (chosen, where
 * relevant, to land on an interesting/non-trivial point in the LCG
 * sequence - e.g. a seed where the rare noise-injection branches
 * actually fire within a short window). These are characterization
 * baselines, not independently-derived expected values.
 * ------------------------------------------------------------------------
 */

#include "pacer.h"
#include <assert.h>

extern void test_set_fake_time(time_t v);

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, desc) \
    do { \
        if (cond) { \
            g_pass++; \
            printf("  [PASS] %s\n", desc); \
        } else { \
            g_fail++; \
            printf("  [FAIL] %s  (line %d)\n", desc, __LINE__); \
        } \
    } while (0)

static void reset_all_state(void)
{
    g_tick_ms = 0;
    g_a_refractory = FALSE;
    g_v_refractory = FALSE;
    g_a_sense_thresh_mv = A_SENSE_THRESH_MV;
    g_v_sense_thresh_mv = V_SENSE_THRESH_MV;
    g_last_a_evt_ms = 0;
    g_last_v_evt_ms = 0;
}

/* ---------------------------------------------------------------- */

static void test_determinism_itself(void)
{
    EgmSample_t s1, s2;

    printf("-- determinism-fix verification (the point of this whole suite) --\n");

    reset_all_state();
    test_set_fake_time(12345);
    sensing_init();
    g_tick_ms = 10;
    sensing_generate_sample(&s1);

    reset_all_state();
    test_set_fake_time(12345); /* identical seed */
    sensing_init();
    g_tick_ms = 10;
    sensing_generate_sample(&s2);

    CHECK(s1.a_mv == s2.a_mv && s1.v_mv == s2.v_mv,
          "identical fake-time seed produces byte-identical sensing output across two independent runs "
          "(THIS is the determinism fix in action - no source file was modified to achieve it)");

    reset_all_state();
    test_set_fake_time(99999); /* different seed */
    sensing_init();
    g_tick_ms = 10;
    sensing_generate_sample(&s2);

    CHECK(!(s1.a_mv == s2.a_mv && s1.v_mv == s2.v_mv),
          "a different seed produces different output (confirms the interposition genuinely drives the LCG, "
          "not a no-op)");
}

static void test_is_noise_floor_zone(void)
{
    printf("-- sensing_is_noise: noise-floor zone (abs_mv < NOISE_FLOOR_MV=50), seed-independent --\n");

    reset_all_state();
    test_set_fake_time(111); sensing_init();
    CHECK(sensing_is_noise(30, 250) == 1, "abs_mv=30 < NOISE_FLOOR_MV -> always noise=1 (seed 111)");

    reset_all_state();
    test_set_fake_time(222); sensing_init();
    CHECK(sensing_is_noise(-30, 250) == 1, "abs_mv=30 (negative mv) < NOISE_FLOOR_MV -> always noise=1 (seed 222)");
}

static void test_is_noise_deterministic_middle_zone(void)
{
    printf("-- sensing_is_noise: deterministic always-0 middle zone (NOISE_FLOOR_MV..thresh-thresh/4), seed-independent --\n");

    reset_all_state();
    test_set_fake_time(111); sensing_init();
    CHECK(sensing_is_noise(100, 250) == 0, "abs_mv=100, thresh=250 (in [50,188]) -> always 0 (seed 111)");

    reset_all_state();
    test_set_fake_time(333); sensing_init();
    CHECK(sensing_is_noise(100, 250) == 0, "same zone, different seed 333 -> still always 0 (no LCG call in this branch)");
    CHECK(sensing_is_noise(188, 250) == 0, "boundary value abs_mv=188 (== thresh - thresh/4) -> still 0");
}

static void test_is_noise_near_threshold_golden_sequence(void)
{
    int i;
    /* thresh - thresh/4 = 250 - 62 = 188 < 220 < 250 = thresh: the ~4%%
     * noise-injection zone. Golden sequence captured at seed=4, chosen
     * because it produces at least one "1" within 40 calls, proving
     * this branch is live and not merely theoretical. */
    const char *golden = "1010000000000000000000001100000000000000"; /* exactly 40 chars */
    int expected[40];

    printf("-- sensing_is_noise: near-threshold zone (~4%% chance), golden sequence, seed=4 --\n");

    for (i = 0; i < 40; i++) {
        expected[i] = golden[i] - '0';
    }

    reset_all_state();
    test_set_fake_time(4); sensing_init();

    {
        int mismatch = 0;
        int ones = 0;
        for (i = 0; i < 40; i++) {
            int r = sensing_is_noise(220, 250);
            if (r != expected[i]) {
                mismatch = 1;
            }
            if (r) ones++;
        }
        CHECK(mismatch == 0, "40-call sequence at mv=220,thresh=250,seed=4 exactly matches the captured golden sequence");
        CHECK(ones > 0, "the near-threshold noise-injection branch actually fires at least once in this window "
                        "(proves the ~4%% path is live, not just theoretically reachable)");
    }
}

static void test_is_noise_above_threshold_golden_sequence(void)
{
    int i;
    int ones = 0;

    printf("-- sensing_is_noise: above-threshold zone (~0.3%% chance), golden sequence, seed=4 --\n");

    reset_all_state();
    test_set_fake_time(4); sensing_init();

    for (i = 0; i < 100; i++) {
        int r = sensing_is_noise(300, 250);
        if (i == 75) {
            CHECK(r == 1, "call #75 (0-indexed) of the seed=4/mv=300/thresh=250 sequence is the pinned single flip to noise=1");
        } else {
            if (r) ones++;
        }
    }
    CHECK(ones == 0, "no OTHER call in the 100-call window flips to noise=1 besides the pinned one at index 75");
}

static void test_generate_sample_golden_waveform(void)
{
    static const INT16 expected_a[6] = { 28, -24, 20, 4, -30, 5 };
    static const INT16 expected_v[6] = { -26, -38, 20, 17, -6, 37 };
    int i;
    EgmSample_t s;

    printf("-- sensing_generate_sample: golden 6-tick waveform, seed=12345 --\n");

    reset_all_state();
    test_set_fake_time(12345); sensing_init();

    for (i = 0; i < 6; i++) {
        char desc[96];
        g_tick_ms = (UINT32)((i + 1) * 10);
        sensing_generate_sample(&s);
        snprintf(desc, sizeof(desc), "tick %d: a_mv matches golden (%d)", i, expected_a[i]);
        CHECK(s.a_mv == expected_a[i], desc);
        snprintf(desc, sizeof(desc), "tick %d: v_mv matches golden (%d)", i, expected_v[i]);
        CHECK(s.v_mv == expected_v[i], desc);
        snprintf(desc, sizeof(desc), "tick %d: t_ms == g_tick_ms", i);
        CHECK(s.t_ms == g_tick_ms, desc);
    }
}

static void test_push_history_wraparound(void)
{
    UINT32 i;
    UINT32 push_count;
    EgmSample_t s;

    printf("-- sensing_push_history: circular wraparound test --\n");

    reset_all_state();
    test_set_fake_time(1); sensing_init();

    push_count = EGM_HISTORY_LEN + 7;
    for (i = 0; i < push_count; i++) {
        s.a_mv = (INT16)i;
        s.v_mv = (INT16)(i * 2);
        s.t_ms = i;
        sensing_push_history(&s);
    }

    CHECK(g_egm_hist_idx == 7, "g_egm_hist_idx wraps correctly: (EGM_HISTORY_LEN+7) pushes -> idx == 7");
    CHECK(g_egm_hist[0].a_mv == (INT16)EGM_HISTORY_LEN,
          "slot 0 holds the (EGM_HISTORY_LEN)-th push (0-indexed), i.e. the first entry AFTER the first full wrap");
    CHECK(g_egm_hist[6].a_mv == (INT16)(EGM_HISTORY_LEN + 6),
          "slot 6 (last one written) holds the very last pushed sample");
}

static void test_sensing_init_resets_history(void)
{
    EgmSample_t s;

    printf("-- sensing_init: history buffer is zeroed on (re-)init --\n");

    reset_all_state();
    test_set_fake_time(1); sensing_init();
    s.a_mv = 999; s.v_mv = 999; s.t_ms = 999;
    sensing_push_history(&s);
    CHECK(g_egm_hist[0].a_mv == 999, "precondition: history slot 0 was written with a sentinel value");

    test_set_fake_time(2); sensing_init(); /* re-init should zero it again */
    CHECK(g_egm_hist[0].a_mv == 0, "sensing_init() zeroes the entire history buffer, including previously-written slots");
    CHECK(g_egm_hist_idx == 0, "sensing_init() resets g_egm_hist_idx to 0");
}

static void test_check_atrial_ventricular_debounce_and_refractory(void)
{
    printf("-- sensing_check_atrial / sensing_check_ventricular: debounce + refractory + copy-paste-divergence --\n");

    /* Refractory guards short-circuit before any noise/timing logic */
    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_a_refractory = TRUE;
    CHECK(sensing_check_atrial(300) == 0, "sensing_check_atrial returns 0 immediately when g_a_refractory is set");

    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_v_refractory = TRUE;
    CHECK(sensing_check_ventricular(600) == 0, "sensing_check_ventricular returns 0 immediately when g_v_refractory is set");

    /* Atrial debounce window is 40ms */
    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_a_sense_thresh_mv = 250;
    g_tick_ms = 1000; g_last_a_evt_ms = 0;
    CHECK(sensing_check_atrial(300) == 1, "atrial event 1000ms after the last one is accepted");

    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_a_sense_thresh_mv = 250;
    g_tick_ms = 1000; g_last_a_evt_ms = 970; /* diff = 30ms, <= 40ms window */
    CHECK(sensing_check_atrial(300) == 0, "atrial event 30ms after the last one is debounced (40ms window)");

    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_a_sense_thresh_mv = 250;
    g_tick_ms = 1000; g_last_a_evt_ms = 959; /* diff = 41ms, just over */
    CHECK(sensing_check_atrial(300) == 1, "atrial event 41ms after the last one clears the 40ms debounce window");

    /* Ventricular debounce window is 60ms - a DIFFERENT, undocumented
     * constant from atrial's 40ms (a copy-paste divergence per the file
     * header comment). Pinned explicitly: a gap that would clear
     * atrial's window does NOT clear ventricular's. */
    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_v_sense_thresh_mv = 500;
    g_tick_ms = 1000; g_last_v_evt_ms = 941; /* diff = 59ms: > atrial's 40ms, but <= ventricular's 60ms */
    CHECK(sensing_check_ventricular(600) == 0,
          "a 59ms gap is debounced for VENTRICULAR (60ms window) even though it would have cleared atrial's 40ms window");

    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_v_sense_thresh_mv = 500;
    g_tick_ms = 1000; g_last_v_evt_ms = 939; /* diff = 61ms, clears 60ms window */
    CHECK(sensing_check_ventricular(600) == 1, "a 61ms gap clears the ventricular 60ms debounce window");
}

static void test_sensing_init_zero_seed_fallback(void)
{
    printf("-- sensing_init: time(NULL)==0 fallback to 0xDEADBEEF (branch coverage) --\n");

    test_set_fake_time(0); /* the one seed value sensing_init() special-cases */
    sensing_init();
    CHECK(g_egm_hist_idx == 0, "sensing_init() with a zero seed still completes normally (0xDEADBEEF fallback taken, no crash)");
}

static void test_generate_sample_and_push_history_null_guards(void)
{
    printf("-- defensive NULL-pointer guards (branch coverage) --\n");

    test_set_fake_time(1); sensing_init();
    sensing_generate_sample(NULL); /* must not crash */
    CHECK(1, "sensing_generate_sample(NULL) returns without crashing (defensive-NULL branch)");

    sensing_push_history(NULL); /* must not crash */
    CHECK(1, "sensing_push_history(NULL) returns without crashing (defensive-NULL branch)");
}

static void test_check_atrial_ventricular_remaining_branches(void)
{
    int i;
    int rc = -1;

    printf("-- sensing_check_atrial/ventricular: below-threshold and noise-rejection branches --\n");

    reset_all_state();
    test_set_fake_time(1); sensing_init();
    g_a_sense_thresh_mv = 250;
    CHECK(sensing_check_atrial(100) == 0, "magnitude below threshold (100 < 250) -> result 0 via the outer-else branch");

    reset_all_state();
    test_set_fake_time(1); sensing_init();
    g_v_sense_thresh_mv = 500;
    CHECK(sensing_check_ventricular(200) == 0, "magnitude below threshold (200 < 500) -> result 0 via the outer-else branch");

    /* Noise-rejection branch: reuse the known seed=4/mv=300/thresh=250
     * sequence (see test_is_noise_above_threshold_golden_sequence),
     * where call #75 (0-indexed) is the pinned single is_noise()==1.
     * sensing_check_atrial(300) with g_a_sense_thresh_mv==250 consumes
     * the LCG identically to a raw sensing_is_noise(300,250) call, so
     * calling it 76 times in a row reaches the same flip. */
    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_a_sense_thresh_mv = 250;
    for (i = 0; i < 76; i++) {
        rc = sensing_check_atrial(300);
    }
    CHECK(rc == 0,
          "on the call where sensing_is_noise() internally flips to 1 (the same golden index 75 as the "
          "direct is_noise test), sensing_check_atrial short-circuits to 0 via the noise-rejection branch");

    /* Same trick for ventricular, reusing thresh=250 to land on the same
     * known golden index. */
    reset_all_state();
    test_set_fake_time(4); sensing_init();
    g_v_sense_thresh_mv = 250;
    for (i = 0; i < 76; i++) {
        rc = sensing_check_ventricular(300);
    }
    CHECK(rc == 0, "sensing_check_ventricular's noise-rejection branch is reached the same way");
}

static void test_long_run_phase_wraps_and_noise_burst(void)
{
    long i;
    EgmSample_t s;

    printf("-- sensing_generate_sample: extended run covering phase-wrap, dropped-beat, and noise-burst branches --\n");
    printf("   (935 ticks @ seed=1: exactly 11 atrial phase wraps - the 11th is the dropped-beat case -\n");
    printf("    10+ ventricular wraps, and at least one noise-burst injection; all deterministic)\n");

    reset_all_state();
    test_set_fake_time(1); sensing_init();

    for (i = 0; i < 935; i++) {
        g_tick_ms = (UINT32)((i + 1) * 10);
        sensing_generate_sample(&s);

        if (i == 84) { /* first atrial phase wrap: normal edge, not a drop */
            CHECK(s.a_mv == 561, "tick 85 (first atrial wrap, not a drop): a_mv matches golden edge value 561");
        }
        if (i == 934) { /* 11th atrial phase wrap: dropped_beat_counter==11, 11%%11==0 -> a_edge forced to 0 */
            CHECK(s.a_mv == 24,
                  "tick 935 (11th atrial wrap): a_mv matches golden NON-edge value 24, confirming the dropped-beat "
                  "branch actually suppressed the edge this time (compare to tick 85's large edge value)");
        }
    }
}

int main(void)
{
    printf("=== sensing.c characterization test suite ===\n");
    printf("(pins CURRENT behavior of the real, linked sensing.c as a\n");
    printf(" regression baseline, using fake_time.c's link-time time()\n");
    printf(" interposition to make its time(NULL)-seeded LCG fully\n");
    printf(" deterministic WITHOUT modifying sensing.c itself)\n\n");

    test_determinism_itself();
    test_is_noise_floor_zone();
    test_is_noise_deterministic_middle_zone();
    test_is_noise_near_threshold_golden_sequence();
    test_is_noise_above_threshold_golden_sequence();
    test_generate_sample_golden_waveform();
    test_push_history_wraparound();
    test_sensing_init_resets_history();
    test_check_atrial_ventricular_debounce_and_refractory();
    test_sensing_init_zero_seed_fallback();
    test_generate_sample_and_push_history_null_guards();
    test_check_atrial_ventricular_remaining_branches();
    test_long_run_phase_wraps_and_noise_burst();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);

    printf("\nKNOWN GAP (not covered): lcg_range()'s span<=0 early-return branch\n");
    printf("(line ~36) is confirmed structurally unreachable - every call site in\n");
    printf("the codebase passes a fixed literal range with hi > lo.\n");

    return (g_fail == 0) ? 0 : 1;
}
