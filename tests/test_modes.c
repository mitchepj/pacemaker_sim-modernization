/* ------------------------------------------------------------------------
 * test_modes.c
 *
 * CHARACTERIZATION TEST SUITE for modes.c.
 *
 * NEW FINDING confirmed here, not merely inferred from reading: mode_step()'s
 * own header comment claims "mode_step() is presently only used for CLI
 * dry-run mode checks in main.c." `grep -rn "mode_step" src/ include/`
 * shows exactly ONE call site: none. It is declared in pacer.h, defined
 * here, and never called from main.c, pacer_core.c, or anywhere else in
 * the codebase. The comment's claim is false (or describes a caller that
 * was removed and never updated) - mode_step() is dead code as of this
 * codebase snapshot. It is still fully characterized below, because (a)
 * it is part of the module's public API surface per pacer.h, (b) a
 * refactor agent needs a ground truth for its behavior BEFORE deciding
 * whether to drop it or keep it, and (c) Risk & Triage's coverage
 * requirement applies to the file, not just its reachable-from-main
 * subset.
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

/* ---------------------------------------------------------------- */

static void test_mode_to_string_and_from_string(void)
{
    static const char *names[MODE_COUNT] = {
        "AOO", "VOO", "AAI", "VVI", "AAT", "VVT",
        "AAIR", "VVIR", "VDD", "DDI", "DDD", "DDDR"
    };
    int i;

    printf("-- mode_to_string / mode_from_string: full round-trip over all %d modes --\n", MODE_COUNT);

    for (i = 0; i < MODE_COUNT; i++) {
        char desc[64];
        const char *s = mode_to_string((PaceMode_t)i);
        snprintf(desc, sizeof(desc), "mode_to_string(%d) == \"%s\"", i, names[i]);
        CHECK(strcmp(s, names[i]) == 0, desc);

        {
            PaceMode_t m = mode_from_string(names[i]);
            snprintf(desc, sizeof(desc), "mode_from_string(\"%s\") round-trips to %d", names[i], i);
            CHECK((int)m == i, desc);
        }
    }

    CHECK(strcmp(mode_to_string((PaceMode_t)-1), "???") == 0, "mode_to_string(-1) returns \"???\" (out-of-range guard)");
    CHECK(strcmp(mode_to_string((PaceMode_t)MODE_COUNT), "???") == 0,
          "mode_to_string(MODE_COUNT) returns \"???\" (one-past-end guard)");

    CHECK(mode_from_string(NULL) == MODE_VVI, "mode_from_string(NULL) falls back to MODE_VVI");
    CHECK(mode_from_string("not_a_real_mode") == MODE_VVI,
          "mode_from_string(\"not_a_real_mode\") falls back to MODE_VVI - no error reporting, "
          "indistinguishable from an explicit VVI request (pinned as documented smell)");
    CHECK(mode_from_string("vvi") == MODE_VVI,
          "mode_from_string is case-sensitive: lowercase \"vvi\" does NOT match \"VVI\" and also "
          "falls back to MODE_VVI - so this particular case happens to look like it 'worked' by accident");
    CHECK(mode_from_string("AAI ") == MODE_VVI, "trailing-space variant \"AAI \" does not match \"AAI\", falls back to VVI");
}

static void test_mode_capability_predicates_full_truth_table(void)
{
    /* Ground-truth table, transcribed directly from the switch/if bodies
     * in modes.c, one row per mode: {atrial_paced, ventricular_paced,
     * atrial_sensed, ventricular_sensed, rate_responsive, dual_chamber}.
     * This IS the specification as currently coded - pinning it here
     * means any future divergence between the six independently-
     * implemented predicate functions (the file's own "duplication"
     * smell) shows up as a test failure instead of silent drift. */
    struct { PaceMode_t m; const char *name; int ap, vp, as_, vs, rr, dc; } table[] = {
        { MODE_AOO,  "AOO",  1, 0, 0, 0, 0, 0 },
        { MODE_VOO,  "VOO",  0, 1, 0, 0, 0, 0 },
        { MODE_AAI,  "AAI",  1, 0, 1, 0, 0, 0 },
        { MODE_VVI,  "VVI",  0, 1, 0, 1, 0, 0 },
        { MODE_AAT,  "AAT",  1, 0, 1, 0, 0, 0 },
        { MODE_VVT,  "VVT",  0, 1, 0, 1, 0, 0 },
        { MODE_AAIR, "AAIR", 1, 0, 1, 0, 1, 0 },
        { MODE_VVIR, "VVIR", 0, 1, 0, 1, 1, 0 },
        { MODE_VDD,  "VDD",  0, 1, 1, 1, 0, 1 },
        { MODE_DDI,  "DDI",  1, 1, 1, 1, 0, 1 },
        { MODE_DDD,  "DDD",  1, 1, 1, 1, 0, 1 },
        { MODE_DDDR, "DDDR", 1, 1, 1, 1, 1, 1 },
    };
    size_t i;

    printf("-- mode_is_* capability predicates: full truth table over all 12 modes --\n");

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        char desc[96];
        snprintf(desc, sizeof(desc), "%s: mode_is_atrial_paced == %d", table[i].name, table[i].ap);
        CHECK(mode_is_atrial_paced(table[i].m) == table[i].ap, desc);
        snprintf(desc, sizeof(desc), "%s: mode_is_ventricular_paced == %d", table[i].name, table[i].vp);
        CHECK(mode_is_ventricular_paced(table[i].m) == table[i].vp, desc);
        snprintf(desc, sizeof(desc), "%s: mode_is_atrial_sensed == %d", table[i].name, table[i].as_);
        CHECK(mode_is_atrial_sensed(table[i].m) == table[i].as_, desc);
        snprintf(desc, sizeof(desc), "%s: mode_is_ventricular_sensed == %d", table[i].name, table[i].vs);
        CHECK(mode_is_ventricular_sensed(table[i].m) == table[i].vs, desc);
        snprintf(desc, sizeof(desc), "%s: mode_is_rate_responsive == %d", table[i].name, table[i].rr);
        CHECK(mode_is_rate_responsive(table[i].m) == table[i].rr, desc);
        snprintf(desc, sizeof(desc), "%s: mode_is_dual_chamber == %d", table[i].name, table[i].dc);
        CHECK(mode_is_dual_chamber(table[i].m) == table[i].dc, desc);
    }

    /* IMPORTANT cross-check that matters for pacer_core.c's state
     * machine dispatch: every mode is EITHER dual_chamber OR falls into
     * exactly one of (atrial_paced XOR ventricular_paced) among the
     * single-chamber modes - see test_pacer_core.c's
     * "fallback branch is dead code" finding, which depends on this
     * exact property holding. */
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!table[i].dc) {
            char desc[160];
            int exclusive_or = (table[i].ap && !table[i].vp) || (!table[i].ap && table[i].vp);
            snprintf(desc, sizeof(desc),
                     "%s (single-chamber): atrial_paced XOR ventricular_paced holds (required for the "
                     "pacer_core.c dead-fallback-branch finding)", table[i].name);
            CHECK(exclusive_or, desc);
        }
    }
}

static void test_mode_apply_defaults(void)
{
    printf("-- mode_apply_defaults: baseline defaults + per-mode overrides --\n");

    /* Baseline defaults applied before the per-mode switch, for every mode */
    mode_apply_defaults(MODE_DDI);
    CHECK(g_lrl_ms == DEFAULT_LRL_MS, "mode_apply_defaults always resets g_lrl_ms to DEFAULT_LRL_MS first");
    CHECK(g_pvarp_ms == DEFAULT_PVARP_MS, "...and g_pvarp_ms to DEFAULT_PVARP_MS");

    mode_apply_defaults(MODE_AOO);
    CHECK(g_a_sense_thresh_mv == 9999, "MODE_AOO widens g_a_sense_thresh_mv to 9999 (never accidentally senses)");

    mode_apply_defaults(MODE_VOO);
    CHECK(g_v_sense_thresh_mv == 9999, "MODE_VOO widens g_v_sense_thresh_mv to 9999");

    mode_apply_defaults(MODE_AAI);
    CHECK(g_avi_ms == 0, "MODE_AAI sets g_avi_ms == 0 (not applicable)");

    mode_apply_defaults(MODE_VVI);
    CHECK(g_avi_ms == 0, "MODE_VVI sets g_avi_ms == 0");

    mode_apply_defaults(MODE_AAT);
    CHECK(g_avi_ms == 0, "MODE_AAT sets g_avi_ms == 0");

    mode_apply_defaults(MODE_VVT);
    CHECK(g_avi_ms == 0, "MODE_VVT sets g_avi_ms == 0");

    mode_apply_defaults(MODE_AAIR);
    CHECK(g_rate_resp_enabled == 1 && g_avi_ms == 0, "MODE_AAIR enables rate response and zeroes g_avi_ms");

    mode_apply_defaults(MODE_VVIR);
    CHECK(g_rate_resp_enabled == 1 && g_avi_ms == 0, "MODE_VVIR enables rate response and zeroes g_avi_ms");

    mode_apply_defaults(MODE_VDD);
    CHECK(g_avi_ms == 130, "MODE_VDD sets g_avi_ms == 130 (mode-specific tweak, not a DEFAULT_* constant)");

    mode_apply_defaults(MODE_DDI);
    CHECK(g_avi_ms == 150, "MODE_DDI sets g_avi_ms == 150");

    mode_apply_defaults(MODE_DDD);
    CHECK(g_avi_ms == 150, "MODE_DDD sets g_avi_ms == 150");

    mode_apply_defaults(MODE_DDDR);
    CHECK(g_rate_resp_enabled == 1 && g_avi_ms == 140, "MODE_DDDR enables rate response and sets g_avi_ms == 140");

    /* out-of-range mode: falls through to baseline defaults, no crash,
     * verbose warning branch (test both g_verbose settings for coverage) */
    g_verbose = 0;
    mode_apply_defaults((PaceMode_t)99);
    CHECK(g_avi_ms == DEFAULT_AVI_MS, "out-of-range mode (99) falls through to plain baseline defaults, no crash");

    g_verbose = 1;
    mode_apply_defaults((PaceMode_t)99);
    CHECK(g_avi_ms == DEFAULT_AVI_MS, "same, with g_verbose=1 (exercises the warning printf branch)");
    g_verbose = 0;
}

static void test_mode_step_all_modes_and_default(void)
{
    printf("-- mode_step: full per-mode + evt_flags coverage (DEAD CODE per the finding above, still pinned) --\n");

    g_lrl_ms = 1000;
    g_avi_ms = 150;

    /* AOO: paces atrial once due, regardless of evt_flags (async, ignores sensing) */
    g_tick_ms = 2000; g_last_a_evt_ms = 0; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_AOO, EVT_NONE) == 1, "AOO: since_a >= lrl_ms -> paces atrial, returns 1");
    g_tick_ms = 500; g_last_a_evt_ms = 0;
    CHECK(mode_step(MODE_AOO, EVT_NONE) == 0, "AOO: since_a < lrl_ms -> no pace, returns 0");

    /* VOO: mirror of AOO for ventricle */
    g_tick_ms = 2000; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_VOO, EVT_NONE) == 2, "VOO: since_v >= lrl_ms -> paces ventricle, returns 2");

    /* AAI: inhibited by EVT_A_SENSE, otherwise paces like AOO */
    g_tick_ms = 2000; g_last_a_evt_ms = 0;
    CHECK(mode_step(MODE_AAI, EVT_A_SENSE) == 0, "AAI: EVT_A_SENSE inhibits pacing even though due -> returns 0");
    CHECK(mode_step(MODE_AAI, EVT_NONE) == 1, "AAI: no sense, due -> paces atrial -> returns 1");

    /* VVI: mirror of AAI */
    g_tick_ms = 2000; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_VVI, EVT_V_SENSE) == 0, "VVI: EVT_V_SENSE inhibits pacing -> returns 0");
    CHECK(mode_step(MODE_VVI, EVT_NONE) == 2, "VVI: no sense, due -> paces ventricle -> returns 2");

    /* AAT: TRIGGERS on the sensed event itself, opposite polarity from AAI */
    g_tick_ms = 100; g_last_a_evt_ms = 0; /* NOT yet due by LRL */
    CHECK(mode_step(MODE_AAT, EVT_A_SENSE) == 1,
          "AAT: EVT_A_SENSE triggers an immediate pace (not inhibition) even though not yet due by LRL - "
          "the opposite behavior from AAI despite looking superficially similar, pinned explicitly");
    /* AAT's OTHER branch: no sense at all -> falls back to the same plain
     * LRL-timeout pace as AAI/AOO (the else side of the trigger check) */
    g_tick_ms = 2000; g_last_a_evt_ms = 0;
    CHECK(mode_step(MODE_AAT, EVT_NONE) == 1, "AAT: no A-sense, LRL-overdue -> falls back to plain timeout pace -> returns 1");
    g_tick_ms = 500; g_last_a_evt_ms = 0;
    CHECK(mode_step(MODE_AAT, EVT_NONE) == 0, "AAT: no A-sense, not yet LRL-overdue -> returns 0");

    /* VVT: mirror of AAT */
    g_tick_ms = 100; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_VVT, EVT_V_SENSE) == 2, "VVT: EVT_V_SENSE triggers an immediate pace");
    g_tick_ms = 2000; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_VVT, EVT_NONE) == 2, "VVT: no V-sense, LRL-overdue -> falls back to plain timeout pace -> returns 2");
    g_tick_ms = 500; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_VVT, EVT_NONE) == 0, "VVT: no V-sense, not yet LRL-overdue -> returns 0");

    /* AAIR / VVIR: same inhibition shape as AAI/VVI (rate-responsiveness itself isn't modeled in mode_step) */
    g_tick_ms = 2000; g_last_a_evt_ms = 0;
    CHECK(mode_step(MODE_AAIR, EVT_A_SENSE) == 0, "AAIR: EVT_A_SENSE inhibits, same shape as AAI");
    CHECK(mode_step(MODE_AAIR, EVT_NONE) == 1, "AAIR: no sense, LRL-overdue -> paces atrial, same shape as AAI -> returns 1");
    g_tick_ms = 2000; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_VVIR, EVT_V_SENSE) == 0, "VVIR: EVT_V_SENSE inhibits, same shape as VVI");
    CHECK(mode_step(MODE_VVIR, EVT_NONE) == 2, "VVIR: no sense, LRL-overdue -> paces ventricle, same shape as VVI -> returns 2");

    /* VDD: tracks atrium (paces V after AVI following an A-sense), else LRL-paces V */
    g_tick_ms = 300; g_last_a_evt_ms = 100; g_avi_ms = 150; /* since_a=200 >= avi_ms=150 */
    CHECK(mode_step(MODE_VDD, EVT_A_SENSE) == 2, "VDD: A-sense followed by AVI timeout -> paces ventricle (tracking)");
    g_tick_ms = 2000; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_VDD, EVT_NONE) == 2, "VDD: no A-sense, V overdue by LRL -> paces ventricle (backup)");

    /* DDI: independent A and V timers, both can pace in the same call ("3") */
    g_lrl_ms = 1000; g_avi_ms = 150;
    g_tick_ms = 2000; g_last_a_evt_ms = 0; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_DDI, EVT_NONE) == 3, "DDI: both A and V overdue with no sensing -> returns 3 (both paced)");
    g_tick_ms = 2000; g_last_a_evt_ms = 0; g_last_v_evt_ms = 1900; /* V not overdue (lrl+avi=1150 window from 1900) */
    CHECK(mode_step(MODE_DDI, EVT_NONE) == 1, "DDI: only A overdue -> returns 1");

    /* DDD: like DDI but V-pace after an A-sense uses AVI window and is
     * suppressed if V was itself sensed */
    g_tick_ms = 300; g_last_a_evt_ms = 100; g_avi_ms = 150; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_DDD, EVT_A_SENSE) == 2, "DDD: A-sense + AVI timeout, no V-sense -> paces V (tracking)");

    g_tick_ms = 300; g_last_a_evt_ms = 100;
    CHECK(mode_step(MODE_DDD, (BYTE)(EVT_A_SENSE | EVT_V_SENSE)) == 0,
          "DDD: A-sense + AVI timeout but V ALSO sensed -> V-pace suppressed, returns 0");

    g_lrl_ms = 1000; g_avi_ms = 150;
    g_tick_ms = 2000; g_last_a_evt_ms = 0; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_DDD, EVT_NONE) == 3, "DDD: no sensing at all, both overdue -> returns 3 (both paced, backup mode)");

    /* DDDR: identical shape to DDD in mode_step (rate-responsiveness not modeled here either) */
    g_tick_ms = 300; g_last_a_evt_ms = 100; g_avi_ms = 150; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_DDDR, EVT_A_SENSE) == 2, "DDDR: same A-sense + AVI tracking behavior as DDD");
    /* DDDR's other branch pair: no sensing at all, both independent
     * timers overdue -> both the A-pace and the V-backup-pace branches
     * fire, exercising the two lines the A-sense-only test above never
     * reaches */
    g_lrl_ms = 1000; g_avi_ms = 150;
    g_tick_ms = 2000; g_last_a_evt_ms = 0; g_last_v_evt_ms = 0;
    CHECK(mode_step(MODE_DDDR, EVT_NONE) == 3,
          "DDDR: no sensing at all, both overdue -> returns 3 (both paced, backup mode, same shape as DDD)");

    /* unrecognized mode -> -1 */
    CHECK(mode_step((PaceMode_t)99, EVT_NONE) == -1, "mode_step with an out-of-range mode returns -1 (unrecognized)");
}

int main(void)
{
    printf("=== modes.c characterization test suite ===\n");
    printf("(pins CURRENT behavior of the real, linked modes.c as a\n");
    printf(" regression baseline ahead of AI-agent-driven refactoring)\n\n");

    test_mode_to_string_and_from_string();
    test_mode_capability_predicates_full_truth_table();
    test_mode_apply_defaults();
    test_mode_step_all_modes_and_default();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);

    printf("\nFINDING (not a coverage gap, a code-health finding): mode_step()\n");
    printf("is never called anywhere in this codebase (confirmed via grep\n");
    printf("across src/ and include/), despite its own header comment\n");
    printf("claiming it backs 'CLI dry-run mode checks in main.c'. Flagged\n");
    printf("for the Architecture & Decoupling Agent: decide whether to drop\n");
    printf("it, wire it in, or explicitly document it as retained dead code\n");
    printf("before Code Generation touches modes.c.\n");

    return (g_fail == 0) ? 0 : 1;
}
