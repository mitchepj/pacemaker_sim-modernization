/* ------------------------------------------------------------------------
 * test_arrhythmia.c
 *
 * CHARACTERIZATION TEST SUITE for arrhythmia.c.
 *
 * Same philosophy as test_eeprom.c / test_telemetry.c: pin CURRENT,
 * OBSERVED behavior of the real, unmodified source as a regression
 * baseline ahead of AI-agent-driven refactoring.
 *
 * NEW FINDING from writing this suite (not previously catalogued in the
 * codebase's own seeded-smell documentation): arrhythmia_run_ams_logic()
 * tags a mode-switch event by writing directly to
 * g_log[g_log_count % MAX_LOG_ENTRIES].flags. telemetry.c's OWN ring
 * buffer bookkeeping (telemetry_log_event) computes the "current" write
 * slot as (g_log_head + g_log_count) % MAX_LOG_ENTRIES, and its most
 * recently written slot as (g_log_head + g_log_count - 1) %
 * MAX_LOG_ENTRIES. These two formulas AGREE only while g_log_head == 0
 * (i.e. before the ring has ever wrapped). Once the log has wrapped
 * (any long-running session), arrhythmia.c's direct write lands on a
 * essentially arbitrary, stale slot - NOT the entry associated with the
 * tick when the mode switch actually happened. This is a real, latent
 * cross-module coupling bug (arrhythmia.c reaching into a data
 * structure it does not own, using a different indexing convention than
 * the module that does own it), consistent with the file's own header
 * comment ("has to reach into globals owned by modes.c and pacer_core.c
 * directly instead of going through a narrower interface") but more
 * concretely damaging than that comment implies. See
 * test_ams_log_flag_indexing_diverges_after_wrap() below, which
 * demonstrates it with real post-wrap state. PINNED AS-IS; flagged for
 * Architecture & Decoupling Agent review before Code Generation touches
 * either arrhythmia.c or telemetry.c - this needs a deliberate decision
 * (preserve for device parity vs. treat as a bug fix), not a silent
 * carry-over or a silent fix.
 *
 * Links the real arrhythmia.c, telemetry.c, and modes.c (for
 * mode_apply_defaults) against the real, unmodified rest of the
 * codebase. This file is NEW; nothing in src/ or include/ is modified.
 * ------------------------------------------------------------------------
 */

#include "pacer.h"
#include <assert.h>

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

static void reset_all_state(PaceMode_t starting_mode)
{
    telemetry_init();
    g_tick_ms = 0;
    g_verbose = 0;
    g_mode = starting_mode;
    arrhythmia_init(); /* captures g_pre_ams_mode = g_mode as of NOW */
}

/* ---------------------------------------------------------------- */

static void test_arrhythmia_init(void)
{
    printf("-- arrhythmia_init tests --\n");

    reset_all_state(MODE_DDD);

    CHECK(g_atr_counter == 0, "arrhythmia_init sets g_atr_counter == 0");
    CHECK(g_ams_active == FALSE, "arrhythmia_init sets g_ams_active == FALSE");
    CHECK(g_pre_ams_mode == MODE_DDD, "arrhythmia_init captures g_pre_ams_mode == current g_mode at init time");
}

static void test_check_atrial_tachy_zero_interval_guard(void)
{
    int rc;

    printf("-- arrhythmia_check_atrial_tachy(0) guard-clause test --\n");

    reset_all_state(MODE_DDD);
    rc = arrhythmia_check_atrial_tachy(0);

    CHECK(rc == 0, "check_atrial_tachy(0) returns 0 (zero-interval guard)");
    CHECK(g_atr_counter == 0, "check_atrial_tachy(0) does not touch streak state (guard returns before any accounting)");
}

static void test_check_atrial_tachy_rate_boundary(void)
{
    printf("-- arrhythmia_check_atrial_tachy rate-threshold boundary tests --\n");

    /* DEFAULT_ATR_RATE_BPM == 180. rate_bpm = 60000 / interval_ms (integer
     * division). interval_ms == 333 -> 60000/333 == 180 (truncated) ->
     * classified FAST. interval_ms == 334 -> 60000/334 == 179 -> SLOW.
     * Exact truncation boundary, pinned. */
    reset_all_state(MODE_DDD);
    (void)arrhythmia_check_atrial_tachy(333);
    CHECK(g_atr_counter == 1, "interval_ms=333 (rate=180bpm, truncated) classified FAST -> streak increments to 1");

    reset_all_state(MODE_DDD);
    (void)arrhythmia_check_atrial_tachy(334);
    CHECK(g_atr_counter == 0, "interval_ms=334 (rate=179bpm, truncated) classified SLOW -> streak stays 0");
}

static void test_check_atrial_tachy_debounce(void)
{
    printf("-- arrhythmia_check_atrial_tachy slow-streak debounce test --\n");

    reset_all_state(MODE_DDD);

    (void)arrhythmia_check_atrial_tachy(300); /* fast */
    (void)arrhythmia_check_atrial_tachy(300); /* fast */
    (void)arrhythmia_check_atrial_tachy(300); /* fast */
    CHECK(g_atr_counter == 3, "three consecutive fast intervals -> streak == 3");

    (void)arrhythmia_check_atrial_tachy(500); /* slow #1 */
    CHECK(g_atr_counter == 3,
          "ONE slow interval after a fast streak does NOT reset it (undocumented one-beat debounce, pinned)");

    (void)arrhythmia_check_atrial_tachy(300); /* fast again, resets slow_streak internally */
    CHECK(g_atr_counter == 4, "a fast interval following a single slow interval resumes accumulating (streak == 4)");

    (void)arrhythmia_check_atrial_tachy(500); /* slow #1 (again) */
    CHECK(g_atr_counter == 4, "first slow interval after fast streak of 4: still no reset");

    (void)arrhythmia_check_atrial_tachy(500); /* slow #2 in a row -> debounce triggers */
    CHECK(g_atr_counter == 0,
          "TWO consecutive slow intervals in a row resets the fast streak to 0 (debounce threshold, pinned)");
}

static void test_check_atrial_tachy_reaches_detect_count(void)
{
    int rc = 0;
    int i;

    printf("-- arrhythmia_check_atrial_tachy ATR_DETECT_COUNT threshold test --\n");

    reset_all_state(MODE_DDD);

    for (i = 1; i <= 8; i++) {
        rc = arrhythmia_check_atrial_tachy(300);
        if (i < 8) {
            CHECK(rc == 0, "check_atrial_tachy returns 0 before the 8th consecutive fast interval");
        }
    }

    CHECK(rc == 1, "check_atrial_tachy returns 1 exactly on the 8th consecutive fast interval (ATR_DETECT_COUNT)");
    CHECK(g_atr_counter == 8, "g_atr_counter == 8 at detection threshold");
}

static void drive_fast_streak_to_detect_count(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        (void)arrhythmia_check_atrial_tachy(300);
    }
}

static void test_ams_engage_downgrades_ddd(void)
{
    printf("-- arrhythmia_run_ams_logic AMS-engage tests: DDD -> DDI --\n");

    reset_all_state(MODE_DDD);
    drive_fast_streak_to_detect_count();

    arrhythmia_run_ams_logic();

    CHECK(g_ams_active == TRUE, "AMS engages once fast streak reaches ATR_DETECT_COUNT");
    CHECK(g_pre_ams_mode == MODE_DDD, "g_pre_ams_mode records the mode that was active when AMS engaged (DDD)");
    CHECK(g_mode == MODE_DDI, "DDD is downgraded to DDI on AMS engage (documented clinical behavior)");
}

static void test_ams_engage_downgrades_vdd(void)
{
    printf("-- arrhythmia_run_ams_logic AMS-engage tests: VDD -> VVI --\n");

    reset_all_state(MODE_VDD);
    drive_fast_streak_to_detect_count();

    arrhythmia_run_ams_logic();

    CHECK(g_ams_active == TRUE, "AMS engages from VDD as well");
    CHECK(g_pre_ams_mode == MODE_VDD, "g_pre_ams_mode records VDD");
    CHECK(g_mode == MODE_VVI, "VDD is downgraded to VVI on AMS engage");
}

static void test_ams_engage_leaves_other_modes_unchanged(void)
{
    printf("-- arrhythmia_run_ams_logic AMS-engage test: non-tracking mode is NOT downgraded --\n");

    reset_all_state(MODE_VVI);
    drive_fast_streak_to_detect_count();

    arrhythmia_run_ams_logic();

    CHECK(g_ams_active == TRUE,
          "AMS still engages (flag flips, event logged) even for a mode outside the DDD/DDDR/VDD downgrade list");
    CHECK(g_pre_ams_mode == MODE_VVI, "g_pre_ams_mode records VVI");
    CHECK(g_mode == MODE_VVI,
          "g_mode is left UNCHANGED for VVI - only DDD/DDDR/VDD have an explicit downgrade rule (pinned, "
          "not obviously stated in the surrounding comment)");
}

static void test_ams_disengage(void)
{
    int i;

    printf("-- arrhythmia_run_ams_logic AMS-disengage test --\n");

    reset_all_state(MODE_DDD);
    drive_fast_streak_to_detect_count();
    arrhythmia_run_ams_logic(); /* engage: g_mode -> DDI, g_pre_ams_mode = DDD */

    CHECK(g_ams_active == TRUE, "precondition: AMS is engaged before exit test");

    /* Drive ATR_EXIT_COUNT (6) consecutive slow intervals. Per the
     * debounce logic, the 2nd slow interval already zeroes the internal
     * fast streak; slow_streak keeps incrementing every call regardless. */
    for (i = 0; i < 6; i++) {
        (void)arrhythmia_check_atrial_tachy(500);
    }

    arrhythmia_run_ams_logic(); /* should now disengage */

    CHECK(g_ams_active == FALSE, "AMS disengages once slow streak reaches ATR_EXIT_COUNT");
    CHECK(g_mode == MODE_DDD, "g_mode is restored to g_pre_ams_mode (DDD) on disengage");
    CHECK(g_atr_counter == 0, "arrhythmia_reset_counters() zeroes g_atr_counter on disengage");

    /* Confirm reset_counters actually cleared internal statics, not just
     * the mirror variable: one fresh fast interval should read back as
     * streak == 1, not a stale accumulated value. */
    (void)arrhythmia_check_atrial_tachy(300);
    CHECK(g_atr_counter == 1, "internal fast-streak state was genuinely reset (next fast interval reads back as 1)");
}

static void test_arrhythmia_reset_counters_direct(void)
{
    printf("-- arrhythmia_reset_counters direct test --\n");

    reset_all_state(MODE_DDD);
    (void)arrhythmia_check_atrial_tachy(300);
    (void)arrhythmia_check_atrial_tachy(300);
    (void)arrhythmia_check_atrial_tachy(300);
    CHECK(g_atr_counter == 3, "precondition: streak built up to 3");

    arrhythmia_reset_counters();
    CHECK(g_atr_counter == 0, "arrhythmia_reset_counters() zeroes g_atr_counter");

    (void)arrhythmia_check_atrial_tachy(300);
    CHECK(g_atr_counter == 1, "next fast interval after reset reads back as 1, confirming internal statics were cleared");
}

static void test_ams_log_flag_indexing_diverges_after_wrap(void)
{
    UINT32 i;
    UINT32 arrhythmia_target_idx;
    UINT32 telemetry_latest_idx;

    printf("-- CROSS-MODULE FINDING: AMS log-flag write vs. telemetry's ring-buffer indexing --\n");

    reset_all_state(MODE_DDD);

    /* Push more than MAX_LOG_ENTRIES telemetry events so the ring
     * buffer wraps and g_log_head advances past 0 - the realistic state
     * of any long-running session. */
    for (i = 0; i < (UINT32)(MAX_LOG_ENTRIES + 20); i++) {
        g_tick_ms = i * 10;
        telemetry_log_event(EVT_A_PACE, (INT16)i, (INT16)i);
    }

    CHECK(g_log_count == MAX_LOG_ENTRIES, "precondition: log ring buffer is full (wrapped) before AMS engages");
    CHECK(g_log_head == 20, "precondition: g_log_head has advanced to 20 after wraparound");

    drive_fast_streak_to_detect_count();
    arrhythmia_run_ams_logic(); /* this is the write under test */

    /* What arrhythmia.c actually wrote to (per its own source, line
     * ~112 of arrhythmia.c): g_log[g_log_count % MAX_LOG_ENTRIES] */
    arrhythmia_target_idx = g_log_count % MAX_LOG_ENTRIES;

    /* What telemetry.c's OWN ring-buffer scheme considers the most
     * recently written entry: (g_log_head + g_log_count - 1) %
     * MAX_LOG_ENTRIES */
    telemetry_latest_idx = (g_log_head + g_log_count - 1) % MAX_LOG_ENTRIES;

    CHECK(arrhythmia_target_idx != telemetry_latest_idx,
          "POST-WRAP: arrhythmia.c's write index and telemetry.c's own 'most recent entry' index DIVERGE "
          "(0 vs 19 in this run) - the EVT_MODE_SWITCH flag lands on a stale/arbitrary slot, not the entry "
          "for the tick when the switch happened. Real cross-module bug, pinned as current behavior.");

    CHECK((g_log[arrhythmia_target_idx].flags & EVT_MODE_SWITCH) != 0,
          "the slot arrhythmia.c actually wrote to (g_log_count % MAX_LOG_ENTRIES) does carry EVT_MODE_SWITCH");

    CHECK((g_log[telemetry_latest_idx].flags & EVT_MODE_SWITCH) == 0,
          "the slot telemetry.c would consider 'most recent' does NOT carry EVT_MODE_SWITCH - "
          "confirms the mode-switch tag is effectively lost/misplaced after wraparound");
}

static void test_ams_verbose_branches(void)
{
    int i;

    printf("-- arrhythmia_run_ams_logic g_verbose=1 diagnostic-branch coverage --\n");

    reset_all_state(MODE_DDD); /* reset_all_state forces g_verbose = 0, so set it AFTER */
    g_verbose = 1;
    drive_fast_streak_to_detect_count();
    arrhythmia_run_ams_logic(); /* verbose ENGAGE branch */
    CHECK(g_ams_active == TRUE, "verbose engage still transitions g_ams_active correctly");

    for (i = 0; i < 6; i++) {
        (void)arrhythmia_check_atrial_tachy(500);
    }
    arrhythmia_run_ams_logic(); /* verbose DISENGAGE branch */
    CHECK(g_ams_active == FALSE, "verbose disengage still transitions g_ams_active correctly");

    g_verbose = 0;
}

int main(void)
{
    printf("=== arrhythmia.c characterization test suite ===\n");
    printf("(pins CURRENT behavior of the real, linked arrhythmia.c as a\n");
    printf(" regression baseline ahead of AI-agent-driven refactoring)\n\n");

    test_arrhythmia_init();
    test_check_atrial_tachy_zero_interval_guard();
    test_check_atrial_tachy_rate_boundary();
    test_check_atrial_tachy_debounce();
    test_check_atrial_tachy_reaches_detect_count();
    test_ams_engage_downgrades_ddd();
    test_ams_engage_downgrades_vdd();
    test_ams_engage_leaves_other_modes_unchanged();
    test_ams_disengage();
    test_arrhythmia_reset_counters_direct();
    test_ams_log_flag_indexing_diverges_after_wrap();
    test_ams_verbose_branches();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);

    printf("\nKNOWN GAP (not covered): mode_apply_defaults()'s effects on\n");
    printf("timing-cycle globals (g_lrl_ms/g_avi_ms/etc.) are exercised as a\n");
    printf("real linked call here (no crash) but not independently asserted -\n");
    printf("that belongs to modes.c's own characterization suite, not this one.\n");

    return (g_fail == 0) ? 0 : 1;
}
