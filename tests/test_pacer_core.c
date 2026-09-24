/* ------------------------------------------------------------------------
 * test_pacer_core.c
 *
 * CHARACTERIZATION TEST SUITE for pacer_core.c - the core timing-cycle
 * state machine and the messiest function in the codebase
 * (pacer_core_handle_state(), ~250 lines, CCN 47).
 *
 * KEY TECHNIQUE: pacer_core_handle_state() is itself a public function
 * (declared in pacer.h) that reads a module-local static, s_cur_sample,
 * which is NOT exposed outside pacer_core.c and is only ever written by
 * sensing_generate_sample() - which is only called from
 * pacer_core_tick(), never from handle_state() itself. A process that
 * calls pacer_core_handle_state() directly, WITHOUT ever having called
 * pacer_core_tick(), therefore sees a permanently zero-valued sample
 * (a_mv=0, v_mv=0 - the C static-storage default). Since
 * sensing_is_noise(0, thresh) unconditionally returns 1 ("this is
 * noise") for any thresh >= NOISE_FLOOR_MV (50) - both real sense
 * thresholds (250, 500) are far above that floor - a_sensed and
 * v_sensed are forced to 0, deterministically, with zero dependency on
 * sensing.c's time(NULL)-seeded LCG. This isolates the PURE
 * pacing/timing state machine from the sensing subsystem for the bulk
 * of this suite (Tier 1 below).
 *
 * The sensed=1 branches (a real intrinsic beat inhibiting or redirecting
 * a pace) cannot be reached this way, since a_sensed/v_sensed are always
 * forced to 0 by the all-zero sample. Those branches are covered
 * separately (Tier 2, at the end of this file) by driving the REAL
 * pacer_core_tick() pipeline - including real sensing.c-generated
 * samples - under a fixed fake-time seed (fake_time.c, already
 * established for sensing.c/battery.c's suites) and pinning golden
 * tick numbers and mV values captured by an actual probe run, per this
 * project's "probe first, then pin" rule. These golden values were
 * captured FRESH for this file rather than reused from test_sensing.c's
 * golden data, because pacer_core_handle_state() calls
 * sensing_is_noise() an EXTRA time per candidate sample on top of what
 * sensing_check_atrial/ventricular() already do internally, and
 * battery_tick() runs its own LCG-driven impedance drift every tick
 * too - none of which run when test_sensing.c calls
 * sensing_generate_sample() in isolation. That shifts the LCG
 * consumption sequence, so reusing the isolated golden values here
 * without re-verifying them empirically would have been exactly the
 * kind of un-verified assumption this project's methodology forbids.
 *
 * IMPORTANT ORDERING CONSTRAINT: because Tier 1 depends on s_cur_sample
 * staying all-zero, and Tier 2 deliberately calls pacer_core_tick()
 * (which overwrites s_cur_sample with real generated samples), ALL
 * Tier-1 tests must run before ANY Tier-2 test in this process. main()
 * below preserves that ordering; do not reorder it.
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

/* Common baseline for every Tier-1 (direct handle_state()) test: known
 * timing parameters, no refractory blocking, tick clock at a known
 * value. Does NOT touch s_cur_sample (can't - it's not exposed), which
 * is exactly the point: it stays whatever pacer_core_init()/reset_cycle
 * left it, i.e. all-zero for the whole of Tier 1. */
static void reset_baseline(PaceMode_t mode, PacerState_t state, UINT32 tick_ms)
{
    g_mode = mode;
    mode_apply_defaults(mode);
    g_state = state;
    g_tick_ms = tick_ms;
    g_last_a_evt_ms = 0;
    g_last_v_evt_ms = 0;
    g_a_refractory = FALSE;
    g_v_refractory = FALSE;
    g_verbose = 0;
}

/* ================================================================== *
 * TIER 1: pure state-machine tests via direct pacer_core_handle_state()
 * calls, relying on the permanently-zero s_cur_sample trick above.
 * ================================================================== */

static void test_pacer_state_name(void)
{
    printf("-- pacer_state_name: all 10 valid states + out-of-range guard --\n");

    CHECK(strcmp(pacer_state_name(ST_INIT), "INIT") == 0, "ST_INIT -> \"INIT\"");
    CHECK(strcmp(pacer_state_name(ST_WAIT_LRL), "WAIT_LRL") == 0, "ST_WAIT_LRL -> \"WAIT_LRL\"");
    CHECK(strcmp(pacer_state_name(ST_A_PACE), "A_PACE") == 0, "ST_A_PACE -> \"A_PACE\"");
    CHECK(strcmp(pacer_state_name(ST_AVI_WAIT), "AVI_WAIT") == 0, "ST_AVI_WAIT -> \"AVI_WAIT\"");
    CHECK(strcmp(pacer_state_name(ST_V_PACE), "V_PACE") == 0, "ST_V_PACE -> \"V_PACE\"");
    CHECK(strcmp(pacer_state_name(ST_VRP), "VRP") == 0, "ST_VRP -> \"VRP\"");
    CHECK(strcmp(pacer_state_name(ST_PVARP), "PVARP") == 0, "ST_PVARP -> \"PVARP\"");
    CHECK(strcmp(pacer_state_name(ST_ARP), "ARP") == 0, "ST_ARP -> \"ARP\"");
    CHECK(strcmp(pacer_state_name(ST_MODE_SWITCHED), "MODE_SWITCHED") == 0, "ST_MODE_SWITCHED -> \"MODE_SWITCHED\"");
    CHECK(strcmp(pacer_state_name(ST_FAULT), "FAULT") == 0, "ST_FAULT -> \"FAULT\"");

    CHECK(strcmp(pacer_state_name((PacerState_t)-1), "UNKNOWN") == 0, "negative state -> \"UNKNOWN\" (out-of-range guard)");
    CHECK(strcmp(pacer_state_name((PacerState_t)(ST_FAULT + 1)), "UNKNOWN") == 0, "one-past-ST_FAULT -> \"UNKNOWN\" (one-past-end guard)");
}

static void test_pacer_core_reset_cycle(void)
{
    printf("-- pacer_core_reset_cycle --\n");

    g_tick_ms = 12345;
    g_last_a_evt_ms = 0;
    g_last_v_evt_ms = 0;
    g_a_refractory = TRUE;
    g_v_refractory = TRUE;
    g_state = ST_FAULT;

    pacer_core_reset_cycle();

    CHECK(g_last_a_evt_ms == 12345, "reset_cycle sets g_last_a_evt_ms = current g_tick_ms");
    CHECK(g_last_v_evt_ms == 12345, "reset_cycle sets g_last_v_evt_ms = current g_tick_ms");
    CHECK(g_a_refractory == FALSE, "reset_cycle clears g_a_refractory");
    CHECK(g_v_refractory == FALSE, "reset_cycle clears g_v_refractory");
    CHECK(g_state == ST_WAIT_LRL, "reset_cycle unconditionally sets g_state = ST_WAIT_LRL");
}

static void test_pacer_core_init(void)
{
    printf("-- pacer_core_init (fake-time-seeded for sensing_init/battery_init determinism) --\n");

    test_set_fake_time(4);
    g_mode = MODE_DDD;
    g_verbose = 0;
    pacer_core_init();

    CHECK(g_tick_ms == 0, "pacer_core_init resets g_tick_ms to 0");
    CHECK(g_ms_since_boot == 0, "pacer_core_init resets g_ms_since_boot to 0");
    CHECK(g_state == ST_WAIT_LRL,
          "pacer_core_init ends in ST_WAIT_LRL (it sets ST_INIT, then immediately calls "
          "pacer_core_reset_cycle() which advances to ST_WAIT_LRL - ST_INIT is never "
          "actually observable from outside pacer_core_init())");
    CHECK(g_avi_ms == 150, "pacer_core_init applies mode_apply_defaults(g_mode) - DDD's avi_ms=150 took effect");
    CHECK(g_a_refractory == FALSE && g_v_refractory == FALSE, "pacer_core_init leaves both refractory flags clear");
    CHECK(g_batt_mv == BATT_NOMINAL_MV, "pacer_core_init's call to battery_init() took effect (battery reset to nominal)");

    /* verbose branch: no assertion beyond "doesn't crash and still leaves state consistent" -
     * this only exercises the printf for gcov line coverage, matching this project's
     * established pattern for other modules' verbose-only branches */
    test_set_fake_time(4);
    g_verbose = 1;
    pacer_core_init();
    CHECK(g_state == ST_WAIT_LRL, "pacer_core_init with g_verbose=1 (exercises the init-complete printf) still ends in ST_WAIT_LRL");
    g_verbose = 0;
}

static void test_handle_state_st_init(void)
{
    printf("-- pacer_core_handle_state: ST_INIT always advances to ST_WAIT_LRL --\n");

    reset_baseline(MODE_DDD, ST_INIT, 0);
    CHECK(pacer_core_handle_state() == 0, "handle_state from ST_INIT returns 0");
    CHECK(g_state == ST_WAIT_LRL, "ST_INIT unconditionally advances to ST_WAIT_LRL on the very next call");
}

static void test_handle_state_dual_chamber_atrial_paced(void)
{
    printf("-- pacer_core_handle_state: ST_WAIT_LRL, dual-chamber, atrial-paced (DDD) --\n");

    /* since_a >= lrl_ms, not refractory -> issue_a_pace(), -> ST_AVI_WAIT */
    reset_baseline(MODE_DDD, ST_WAIT_LRL, 2000);
    CHECK(pacer_core_handle_state() == 0, "DDD: A overdue, not refractory -> handle_state returns 0 (no fault)");
    CHECK(g_last_a_evt_ms == 2000, "DDD: A-pace issued -> g_last_a_evt_ms updated to g_tick_ms");
    CHECK(g_a_refractory == TRUE, "DDD: A-pace issued -> g_a_refractory set");
    CHECK(g_state == ST_AVI_WAIT, "DDD: A-pace issued -> state advances to ST_AVI_WAIT");

    /* since_a >= lrl_ms but g_a_refractory already TRUE -> pace suppressed,
     * falls through to the dual-chamber fallback check (since_v not yet
     * overdue here) -> stays ST_WAIT_LRL, no state change */
    reset_baseline(MODE_DDD, ST_WAIT_LRL, 2000);
    g_a_refractory = TRUE;
    CHECK(pacer_core_handle_state() == 0, "DDD: A overdue but refractory -> returns 0");
    CHECK(g_last_a_evt_ms == 0, "DDD: A overdue but refractory -> pace suppressed, g_last_a_evt_ms unchanged");
    CHECK(g_state == ST_WAIT_LRL,
          "DDD: A overdue but refractory, V not overdue either -> stays ST_WAIT_LRL "
          "(the refractory-blocked branch falls through to the fallback check without a goto, "
          "a real characterization detail, not just 'pace suppressed')");

    /* since_a < lrl_ms (not yet due) -> falls through to fallback check */
    reset_baseline(MODE_DDD, ST_WAIT_LRL, 500);
    CHECK(pacer_core_handle_state() == 0, "DDD: A not yet due -> returns 0");
    CHECK(g_state == ST_WAIT_LRL, "DDD: A not yet due, V not overdue -> stays ST_WAIT_LRL");

    /* dual-chamber safety fallback: A not due, but V's independent timer
     * (lrl_ms + avi_ms) has expired -> force a V-pace even though we're
     * still nominally waiting on the atrial side. This requires the two
     * event timestamps to have diverged (e.g. an earlier A-sense reset
     * g_last_a_evt_ms more recently than g_last_v_evt_ms) - modeled
     * directly rather than reached via simple elapsed simulated time. */
    reset_baseline(MODE_DDD, ST_WAIT_LRL, 0);
    g_tick_ms = 1400;
    g_last_a_evt_ms = 600;  /* since_a = 800, < lrl_ms(1000): A not due */
    g_last_v_evt_ms = 0;    /* since_v = 1400, >= lrl_ms+avi_ms(1150): V fallback-overdue */
    CHECK(pacer_core_handle_state() == 0, "DDD: A not due, V fallback-overdue -> returns 0");
    CHECK(g_last_v_evt_ms == 1400,
          "DDD dual-chamber safety fallback: A's timer not yet due, but V's independent "
          "(lrl_ms+avi_ms) timer has - fallback fires a V-pace anyway ('shouldn't normally "
          "happen' path per the code's own comment, but real and directly triggerable)");
    CHECK(g_v_refractory == TRUE, "...and sets g_v_refractory as a normal side effect of issue_v_pace()");
    CHECK(g_state == ST_WAIT_LRL, "...while g_state itself is left unchanged (no explicit transition in this fallback path)");

    /* same fallback, but V already refractory -> pace suppressed */
    reset_baseline(MODE_DDD, ST_WAIT_LRL, 0);
    g_tick_ms = 1400;
    g_last_a_evt_ms = 600;
    g_last_v_evt_ms = 0;
    g_v_refractory = TRUE;
    CHECK(pacer_core_handle_state() == 0, "DDD: A not due, V fallback-overdue but refractory -> returns 0");
    CHECK(g_last_v_evt_ms == 0, "...fallback V-pace suppressed by refractory, g_last_v_evt_ms unchanged");
}

static void test_handle_state_dual_chamber_non_atrial_paced(void)
{
    printf("-- pacer_core_handle_state: ST_WAIT_LRL, dual-chamber, NON-atrial-paced (VDD) --\n");

    /* VDD: mode_is_atrial_paced == 0, mode_is_dual_chamber == 1. The
     * "else" side of the inner if/else (not the outer single/dual
     * branch) - since_a >= lrl_ms transitions straight to ST_AVI_WAIT
     * with NO atrial pace ever issued (VDD never paces the atrium). */
    reset_baseline(MODE_VDD, ST_WAIT_LRL, 2000);
    CHECK(pacer_core_handle_state() == 0, "VDD: A-timer overdue -> returns 0");
    CHECK(g_last_a_evt_ms == 0, "VDD: A-timer overdue but VDD never paces atrium - g_last_a_evt_ms unchanged (no pace)");
    CHECK(g_a_refractory == FALSE, "VDD: correspondingly, g_a_refractory never gets set here");
    CHECK(g_state == ST_AVI_WAIT, "VDD: A-timer overdue still advances state to ST_AVI_WAIT (tracking window opens)");

    /* since_a < lrl_ms -> falls through to the same dual-chamber fallback */
    reset_baseline(MODE_VDD, ST_WAIT_LRL, 0);
    g_tick_ms = 1400;
    g_last_a_evt_ms = 600; /* since_a = 800 < lrl_ms(1000) */
    g_last_v_evt_ms = 0;   /* since_v = 1400 >= lrl_ms+avi_ms (1000+130=1130) */
    CHECK(pacer_core_handle_state() == 0, "VDD: A not due -> falls to same fallback check, returns 0");
    CHECK(g_last_v_evt_ms == 1400, "VDD: shares the identical dual-chamber V-fallback-pace path as DDD");
    CHECK(g_state == ST_WAIT_LRL, "VDD: state unchanged by the fallback path, same as DDD");
}

static void test_handle_state_single_chamber_ventricular_only(void)
{
    printf("-- pacer_core_handle_state: ST_WAIT_LRL, single-chamber, V-only (VVI) --\n");

    /* since_v >= lrl_ms, not refractory -> issue_v_pace(); state NEVER
     * transitions in single-chamber modes (no AVI concept) - stays
     * ST_WAIT_LRL forever */
    reset_baseline(MODE_VVI, ST_WAIT_LRL, 2000);
    CHECK(pacer_core_handle_state() == 0, "VVI: V overdue -> returns 0");
    CHECK(g_last_v_evt_ms == 2000, "VVI: V-pace issued");
    CHECK(g_v_refractory == TRUE, "VVI: refractory set after pace");
    CHECK(g_state == ST_WAIT_LRL, "VVI: single-chamber modes never leave ST_WAIT_LRL");

    /* refractory blocks the pace */
    reset_baseline(MODE_VVI, ST_WAIT_LRL, 2000);
    g_v_refractory = TRUE;
    CHECK(pacer_core_handle_state() == 0, "VVI: V overdue but refractory -> returns 0");
    CHECK(g_last_v_evt_ms == 0, "VVI: pace suppressed by refractory");

    /* not yet due -> no pace */
    reset_baseline(MODE_VVI, ST_WAIT_LRL, 500);
    CHECK(pacer_core_handle_state() == 0, "VVI: V not yet due -> returns 0");
    CHECK(g_last_v_evt_ms == 0, "VVI: not yet due -> no pace issued");
}

static void test_handle_state_single_chamber_atrial_only(void)
{
    printf("-- pacer_core_handle_state: ST_WAIT_LRL, single-chamber, A-only (AAI) --\n");

    reset_baseline(MODE_AAI, ST_WAIT_LRL, 2000);
    CHECK(pacer_core_handle_state() == 0, "AAI: A overdue -> returns 0");
    CHECK(g_last_a_evt_ms == 2000, "AAI: A-pace issued");
    CHECK(g_a_refractory == TRUE, "AAI: refractory set after pace");
    CHECK(g_state == ST_WAIT_LRL, "AAI: single-chamber modes never leave ST_WAIT_LRL");

    reset_baseline(MODE_AAI, ST_WAIT_LRL, 2000);
    g_a_refractory = TRUE;
    CHECK(pacer_core_handle_state() == 0, "AAI: A overdue but refractory -> returns 0");
    CHECK(g_last_a_evt_ms == 0, "AAI: pace suppressed by refractory");

    reset_baseline(MODE_AAI, ST_WAIT_LRL, 500);
    CHECK(pacer_core_handle_state() == 0, "AAI: A not yet due -> returns 0");
    CHECK(g_last_a_evt_ms == 0, "AAI: not yet due -> no pace issued");
}

static void test_handle_state_aoo_voo_dead_fallback_finding(void)
{
    /* THE finding flagged (but not yet proven) after reading modes.c and
     * pacer_core.c side by side: the single-chamber dispatch is
     *   if (vp && !ap)      { V-only branch, touches V ONLY }
     *   else if (ap && !vp) { A-only branch, touches A ONLY }
     *   else                { fallback: paces whichever of A/V is due }
     * modes.c's own truth table (cross-checked in test_modes.c) shows
     * EVERY one of the 8 currently-defined single-chamber modes
     * satisfies ap XOR vp - meaning AOO and VOO, despite the fallback
     * branch's comment explicitly naming them ("VOO/AOO async: pace
     * whichever is due"), actually route to the A-only / V-only
     * branches instead, and NEVER reach the fallback at all.
     *
     * This is empirically distinguishable: the A-only and V-only
     * branches are single-chamber-blind - they never look at, and never
     * pace, the OTHER chamber's timer, no matter how overdue it is. The
     * fallback branch pace BOTH independently. So: force AOO's
     * (unused-by-AOO) ventricular timer far past due, and confirm no
     * ventricular pace EVER fires. If the fallback branch were somehow
     * reached instead, it would have paced V. */
    printf("-- pacer_core_handle_state: CONFIRMING the AOO/VOO dead-fallback-branch finding --\n");

    reset_baseline(MODE_AOO, ST_WAIT_LRL, 5000); /* since_a = 5000, since_v = 5000, both wildly overdue */
    CHECK(pacer_core_handle_state() == 0, "AOO: handle_state with both timers overdue -> returns 0");
    CHECK(g_last_a_evt_ms == 5000, "AOO: A-pace fires as expected (AOO is atrial-paced)");
    CHECK(g_last_v_evt_ms == 0,
          "AOO: V-timer is ALSO wildly overdue (since_v=5000), yet NO ventricular pace fires - "
          "proves AOO is dispatched to the A-only branch (which never even looks at since_v), "
          "NOT the 'fallback: pace whichever is due' branch its comment claims handles AOO. "
          "If AOO had reached the fallback, this V-pace WOULD have fired.");
    CHECK(g_v_refractory == FALSE, "AOO: correspondingly g_v_refractory is never touched");

    reset_baseline(MODE_VOO, ST_WAIT_LRL, 5000);
    CHECK(pacer_core_handle_state() == 0, "VOO: handle_state with both timers overdue -> returns 0");
    CHECK(g_last_v_evt_ms == 5000, "VOO: V-pace fires as expected (VOO is ventricular-paced)");
    CHECK(g_last_a_evt_ms == 0,
          "VOO: A-timer is ALSO wildly overdue, yet NO atrial pace fires - proves VOO is "
          "dispatched to the V-only branch, not the fallback the comment claims covers it. "
          "CONFIRMED FINDING: the 'pace whichever is due' fallback branch in "
          "pacer_core_handle_state()'s single-chamber dispatch is dead code for all 12 "
          "currently-defined modes, contradicting its own comment - the same class of "
          "comment/code drift already found in telemetry.c and arrhythmia.c.");
    CHECK(g_a_refractory == FALSE, "VOO: correspondingly g_a_refractory is never touched");
}

static void test_handle_state_fallback_reachable_via_invalid_mode(void)
{
    /* The fallback branch is NOT unreachable in the absolute sense - it
     * is reachable for any mode value where mode_is_atrial_paced() AND
     * mode_is_ventricular_paced() BOTH return 0, which cannot happen for
     * any of the 12 real PaceMode_t values (confirmed above), but CAN
     * happen for an out-of-range mode integer, since both predicates'
     * switch statements fall through to a `default: return 0;`. This
     * closes the coverage gap on the fallback branch's lines without
     * papering over the finding above - it directly demonstrates the
     * exact (unrealistic, out-of-range) condition required to reach it. */
    printf("-- pacer_core_handle_state: fallback branch IS reachable, but only via an invalid mode value --\n");

    reset_baseline((PaceMode_t)99, ST_WAIT_LRL, 0);
    g_tick_ms = 5000; /* since_a = since_v = 5000, both overdue */
    CHECK(mode_is_atrial_paced((PaceMode_t)99) == 0 && mode_is_ventricular_paced((PaceMode_t)99) == 0,
          "precondition: mode 99 satisfies neither predicate, so it cannot match the V-only or "
          "A-only branches and must fall through to the final else");
    CHECK(pacer_core_handle_state() == 0, "mode=99 (invalid): handle_state returns 0 (not a fault path)");
    CHECK(g_last_a_evt_ms == 5000 && g_last_v_evt_ms == 5000,
          "mode=99: BOTH chambers paced independently in the same call - this is the fallback "
          "branch executing, proving it is live code, just unreachable for any real mode");
}

static void test_handle_state_avi_wait(void)
{
    printf("-- pacer_core_handle_state: ST_AVI_WAIT (v_sensed forced 0 by the zero-sample trick) --\n");

    /* since_a >= avi_ms -> AVI timeout, paces V (tracking pace), -> ST_WAIT_LRL */
    reset_baseline(MODE_DDD, ST_AVI_WAIT, 300);
    g_last_a_evt_ms = 100; /* since_a = 200 >= avi_ms(150) */
    CHECK(pacer_core_handle_state() == 0, "DDD AVI_WAIT: AVI timeout -> returns 0");
    CHECK(g_last_v_evt_ms == 300, "DDD AVI_WAIT: AVI timeout paces V (atrial-tracking pace)");
    CHECK(g_state == ST_WAIT_LRL, "DDD AVI_WAIT: AVI timeout -> back to ST_WAIT_LRL");

    /* AVI timeout, but V already refractory -> pace suppressed, state STILL advances */
    reset_baseline(MODE_DDD, ST_AVI_WAIT, 300);
    g_last_a_evt_ms = 100;
    g_v_refractory = TRUE;
    CHECK(pacer_core_handle_state() == 0, "DDD AVI_WAIT: AVI timeout, V refractory -> returns 0");
    CHECK(g_last_v_evt_ms == 0, "DDD AVI_WAIT: pace suppressed by refractory");
    CHECK(g_state == ST_WAIT_LRL,
          "DDD AVI_WAIT: even with the pace suppressed, the state transition back to "
          "ST_WAIT_LRL is unconditional once since_a >= avi_ms - a real characterization "
          "detail (state advances independently of whether a pace was actually delivered)");

    /* not yet due at all: since_a < avi_ms AND < url_ms -> stays ST_AVI_WAIT */
    reset_baseline(MODE_DDD, ST_AVI_WAIT, 200);
    g_last_a_evt_ms = 100; /* since_a = 100 < avi_ms(150) < url_ms(461) */
    CHECK(pacer_core_handle_state() == 0, "DDD AVI_WAIT: neither AVI nor URL timeout yet -> returns 0");
    CHECK(g_state == ST_AVI_WAIT, "DDD AVI_WAIT: stays in ST_AVI_WAIT, still waiting");
    CHECK(g_last_v_evt_ms == 0, "DDD AVI_WAIT: no pace issued yet");

    /* URL safety-fallback branch: per every one of the 12 built-in
     * mode_apply_defaults() presets, avi_ms (130-150) is always far
     * below url_ms (461 default) - so under normal operation the
     * since_a >= avi_ms check above ALWAYS fires first, making this
     * "since_a >= url_ms" branch structurally unreachable in practice.
     * BUT eeprom_validate() (eeprom.c) range-checks lrl_ms and url_ms
     * on load, and never validates avi_ms at all, nor any relationship
     * between avi_ms and url_ms - so a legitimately CRC-valid saved
     * NVRAM block with avi_ms > url_ms passes validation cleanly and
     * WOULD engage this branch on every cardiac cycle. Modeled directly
     * here rather than asserted from reading alone. */
    reset_baseline(MODE_DDD, ST_AVI_WAIT, 0);
    g_avi_ms = 1000; /* deliberately misconfigured: avi_ms > url_ms, unreachable via any */
    g_url_ms = 400;  /* built-in preset, but not rejected by eeprom_validate() either */
    g_tick_ms = 500;
    g_last_a_evt_ms = 0; /* since_a = 500: < avi_ms(1000), so AVI check does NOT fire ... */
    CHECK(pacer_core_handle_state() == 0,
          "DDD AVI_WAIT with avi_ms(1000) > url_ms(400) [unreachable via any built-in mode "
          "preset, but not rejected by eeprom_validate() either]: since_a=500 >= url_ms(400) "
          "-> URL safety-fallback fires -> returns 0");
    CHECK(g_last_v_evt_ms == 500,
          "URL safety-fallback branch confirmed live: paces V even though the AVI window "
          "itself hasn't nominally elapsed, specifically because AVI was configured longer "
          "than URL - a finding for the eeprom.c / Architecture agent, not just a coverage note");
    CHECK(g_state == ST_WAIT_LRL, "URL safety-fallback also returns state to ST_WAIT_LRL");
}

static void test_handle_state_collapsing_states(void)
{
    printf("-- pacer_core_handle_state: ST_A_PACE/V_PACE/VRP/PVARP/ARP/MODE_SWITCHED all collapse to ST_WAIT_LRL --\n");

    reset_baseline(MODE_DDD, ST_A_PACE, 0);
    CHECK(pacer_core_handle_state() == 0, "ST_A_PACE -> returns 0");
    CHECK(g_state == ST_WAIT_LRL, "ST_A_PACE collapses immediately to ST_WAIT_LRL (kept in the enum for telemetry only)");

    reset_baseline(MODE_DDD, ST_V_PACE, 0);
    CHECK(pacer_core_handle_state() == 0, "ST_V_PACE -> returns 0");
    CHECK(g_state == ST_WAIT_LRL, "ST_V_PACE collapses to ST_WAIT_LRL");

    reset_baseline(MODE_DDD, ST_VRP, 0);
    CHECK(pacer_core_handle_state() == 0, "ST_VRP -> returns 0");
    CHECK(g_state == ST_WAIT_LRL, "ST_VRP collapses to ST_WAIT_LRL");

    reset_baseline(MODE_DDD, ST_PVARP, 0);
    CHECK(pacer_core_handle_state() == 0, "ST_PVARP -> returns 0");
    CHECK(g_state == ST_WAIT_LRL, "ST_PVARP collapses to ST_WAIT_LRL");

    reset_baseline(MODE_DDD, ST_ARP, 0);
    CHECK(pacer_core_handle_state() == 0, "ST_ARP -> returns 0");
    CHECK(g_state == ST_WAIT_LRL, "ST_ARP collapses to ST_WAIT_LRL");

    reset_baseline(MODE_DDD, ST_MODE_SWITCHED, 0);
    CHECK(pacer_core_handle_state() == 0, "ST_MODE_SWITCHED -> returns 0");
    CHECK(g_state == ST_WAIT_LRL, "ST_MODE_SWITCHED collapses to ST_WAIT_LRL");
}

static void test_handle_state_fault_and_default(void)
{
    printf("-- pacer_core_handle_state: ST_FAULT and the invalid-state default branch --\n");

    reset_baseline(MODE_DDD, ST_FAULT, 999);
    g_verbose = 0;
    CHECK(pacer_core_handle_state() == -1, "ST_FAULT (non-verbose) returns -1");
    CHECK(g_state == ST_FAULT, "ST_FAULT is a sink - nothing moves it back on its own");

    reset_baseline(MODE_DDD, ST_FAULT, 999);
    g_verbose = 1;
    CHECK(pacer_core_handle_state() == -1, "ST_FAULT (verbose, exercises the FAULT-state printf) returns -1");
    g_verbose = 0;

    /* an invalid PacerState_t value (not one of the 10 named states) -
     * the switch's own default case, distinct from ST_FAULT itself */
    reset_baseline(MODE_DDD, (PacerState_t)123, 0);
    CHECK(pacer_core_handle_state() == -1, "invalid state value (123): returns -1 via the switch's default case");
    CHECK(g_state == ST_FAULT, "invalid state value: default case explicitly sets g_state = ST_FAULT before returning");
}

/* ================================================================== *
 * TIER 2: integration tests through the REAL pacer_core_tick() pipeline
 * (real sensing.c-generated samples, real battery.c ticking, real
 * arrhythmia.c hooks) - covers the a_sensed=1 / v_sensed=1 branches
 * Tier 1 cannot reach. MUST run after all Tier 1 tests (see file
 * header) since this is what first writes a non-zero s_cur_sample.
 *
 * All golden tick numbers and mV values below were captured by an
 * actual instrumented probe run against this exact linked binary
 * (COMMON_SRC + fake_time.c), not derived by reading sensing.c's math -
 * consistent with this project's "probe first, then pin" rule and
 * distinct from (not reused from) test_sensing.c's own golden data, for
 * the LCG-interleaving reason explained in the file header.
 * ------------------------------------------------------------------- */

static void test_integration_dual_chamber_real_sensing(void)
{
    int i;
    UINT32 seen = 0;
    int got_a = 0, got_v = 0;

    printf("-- INTEGRATION (real pacer_core_tick pipeline): DDD, fake-time seed=1 --\n");

    test_set_fake_time(1);
    pacer_core_init();
    g_mode = MODE_DDD;
    mode_apply_defaults(MODE_DDD);
    pacer_core_reset_cycle();
    g_verbose = 0;

    for (i = 0; i < 200 && !(got_a && got_v); i++) {
        pacer_core_tick(10);
        while (seen < g_log_count) {
            UINT32 idx = (g_log_head + seen) % MAX_LOG_ENTRIES;
            LogEntry_t *e = &g_log[idx];
            if ((e->flags & EVT_A_SENSE) && !got_a) {
                CHECK(i == 84, "DDD real-sensing golden: first genuine A-sense lands at tick 84 (t_ms=850)");
                CHECK(e->t_ms == 850, "...t_ms == 850");
                CHECK(e->a_mv == 561, "...a_mv == 561 (golden, captured by probe against this exact build)");
                got_a = 1;
            }
            if ((e->flags & EVT_V_SENSE) && !got_v) {
                CHECK(i == 89, "DDD real-sensing golden: first genuine V-sense lands at tick 89 (t_ms=900)");
                CHECK(e->t_ms == 900, "...t_ms == 900");
                CHECK(e->v_mv == 1053, "...v_mv == 1053 (golden)");
                got_v = 1;
            }
            seen++;
        }
    }

    CHECK(got_a && got_v, "both a genuine A-sense and a genuine V-sense were observed within 200 ticks");
    CHECK(g_state == ST_WAIT_LRL,
          "after the A-sense (-> ST_AVI_WAIT) followed 50ms later by the V-sense (since_a=50 < "
          "avi_ms=150, so this is genuine AV tracking, not an AVI timeout) -> back to "
          "ST_WAIT_LRL - exercises the dual-chamber a_sensed==1 transition (line ~250) AND "
          "the ST_AVI_WAIT v_sensed==1 transition (line ~334) in the same run, with real, "
          "non-forced sensing data");
}

static void test_integration_single_chamber_real_sensing(void)
{
    int i;
    UINT32 seen;
    int got_v;

    printf("-- INTEGRATION (real pacer_core_tick pipeline): VVI, fake-time seed=1 --\n");

    test_set_fake_time(1);
    pacer_core_init();
    g_mode = MODE_VVI;
    mode_apply_defaults(MODE_VVI);
    pacer_core_reset_cycle();
    g_verbose = 0;

    seen = 0;
    got_v = 0;
    for (i = 0; i < 200 && !got_v; i++) {
        pacer_core_tick(10);
        while (seen < g_log_count) {
            UINT32 idx = (g_log_head + seen) % MAX_LOG_ENTRIES;
            LogEntry_t *e = &g_log[idx];
            if ((e->flags & EVT_V_SENSE) && !got_v) {
                CHECK(i == 89, "VVI real-sensing golden: first genuine V-sense lands at tick 89 (t_ms=900)");
                CHECK(e->v_mv == 1053, "...v_mv == 1053 (golden, same underlying waveform as the DDD run above)");
                got_v = 1;
            }
            seen++;
        }
    }
    CHECK(got_v, "a genuine V-sense was observed within 200 ticks");
    CHECK(g_state == ST_WAIT_LRL,
          "VVI: the single-chamber V-only branch's v_sensed==1 early-return (line ~287) fires - "
          "inhibits the pace, stays in ST_WAIT_LRL (VVI never leaves it), exactly like the "
          "inhibition behavior already pinned for mode_step() in test_modes.c, but now proven "
          "in the REAL state machine that actually runs in production, not the dead mode_step()");
}

static void test_integration_single_chamber_atrial_real_sensing(void)
{
    int i;
    UINT32 seen;
    int got_a;

    printf("-- INTEGRATION (real pacer_core_tick pipeline): AAI, fake-time seed=1 --\n");

    test_set_fake_time(1);
    pacer_core_init();
    g_mode = MODE_AAI;
    mode_apply_defaults(MODE_AAI);
    pacer_core_reset_cycle();
    g_verbose = 0;

    seen = 0;
    got_a = 0;
    for (i = 0; i < 200 && !got_a; i++) {
        pacer_core_tick(10);
        while (seen < g_log_count) {
            UINT32 idx = (g_log_head + seen) % MAX_LOG_ENTRIES;
            LogEntry_t *e = &g_log[idx];
            if ((e->flags & EVT_A_SENSE) && !got_a) {
                CHECK(i == 84, "AAI real-sensing golden: first genuine A-sense lands at tick 84 (t_ms=850)");
                CHECK(e->a_mv == 561, "...a_mv == 561 (golden, same underlying waveform)");
                got_a = 1;
            }
            seen++;
        }
    }
    CHECK(got_a, "a genuine A-sense was observed within 200 ticks");
    CHECK(g_state == ST_WAIT_LRL,
          "AAI: the single-chamber A-only branch's a_sensed==1 early-return (line ~301) fires - "
          "inhibits the pace, stays in ST_WAIT_LRL");
}

static void test_integration_refractory_clearing_single_chamber(void)
{
    /* AOO and VOO are both async and unsensed (mode_apply_defaults widens
     * their OWN sense threshold to 9999, and neither is in modes.c's
     * "sensed" list anyway) - so their pace/refractory-clear timing is
     * PURE ARITHMETIC on g_tick_ms, g_lrl_ms, g_arp_ms/g_vrp_ms, with zero
     * dependency on sensing.c's LCG. Verified by an actual probe run
     * (not hand-derived) before pinning: this closes the coverage gap on
     * update_refractory_windows()'s single-chamber ("else") branches for
     * both the atrial and ventricular refractory windows, which none of
     * the tests above reach (they never let a real pace's refractory
     * window fully play out through the real pacer_core_tick() pipeline). */
    int i;

    printf("-- INTEGRATION: update_refractory_windows(), single-chamber (non-dual) branch --\n");

    test_set_fake_time(1);
    pacer_core_init();
    g_mode = MODE_AOO;
    mode_apply_defaults(MODE_AOO);
    pacer_core_reset_cycle();
    for (i = 0; i < 115; i++) {
        pacer_core_tick(10);
        if (i == 99) {
            CHECK(g_a_refractory == TRUE, "AOO golden: g_a_refractory becomes TRUE exactly at tick 99 (t=1000ms, the LRL pace)");
        }
    }
    CHECK(g_a_refractory == FALSE,
          "AOO golden: by tick 115 (t=1150ms = last pace + arp_ms(150)), update_refractory_windows()'s "
          "NON-dual-chamber atrial-clear branch has fired and cleared g_a_refractory");

    test_set_fake_time(1);
    pacer_core_init();
    g_mode = MODE_VOO;
    mode_apply_defaults(MODE_VOO);
    pacer_core_reset_cycle();
    for (i = 0; i < 120; i++) {
        pacer_core_tick(10);
        if (i == 99) {
            CHECK(g_v_refractory == TRUE, "VOO golden: g_v_refractory becomes TRUE exactly at tick 99 (t=1000ms, the LRL pace)");
        }
    }
    CHECK(g_v_refractory == FALSE,
          "VOO golden: by tick 120 (t=1200ms = last pace + vrp_ms(200)), update_refractory_windows()'s "
          "NON-dual-chamber ventricular-clear branch has fired and cleared g_v_refractory");
}

static void test_integration_refractory_clearing_dual_chamber(void)
{
    /* Same idea, but for a dual-chamber mode - both sense thresholds are
     * deliberately widened to 9999 here (NOT one of the 12 built-in
     * mode_apply_defaults() presets for DDD, which normally senses
     * normally) purely as a test-harness technique to suppress real
     * sensing so the LRL/AVI timers run their natural, deterministic
     * course through the real pacer_core_tick() pipeline - this closes
     * the coverage gap on update_refractory_windows()'s DUAL-chamber
     * branches, which are textually separate lines from the
     * single-chamber branches above despite doing the literally
     * identical comparison (a minor duplication smell of its own:
     * mode_is_dual_chamber() has no actual effect on refractory-clear
     * TIMING anywhere in this function, unlike everywhere else it's
     * used in this file). Golden tick numbers from an actual probe run. */
    int i;

    printf("-- INTEGRATION: update_refractory_windows(), dual-chamber branch --\n");

    test_set_fake_time(1);
    pacer_core_init();
    g_mode = MODE_DDD;
    mode_apply_defaults(MODE_DDD);
    g_a_sense_thresh_mv = 9999; /* suppress real sensing for this timing-only test */
    g_v_sense_thresh_mv = 9999;
    pacer_core_reset_cycle();

    for (i = 0; i < 135; i++) {
        pacer_core_tick(10);
        if (i == 99) {
            CHECK(g_a_refractory == TRUE, "DDD golden: g_a_refractory becomes TRUE at tick 99 (t=1000ms, LRL A-pace)");
        }
        if (i == 114) {
            CHECK(g_a_refractory == FALSE && g_v_refractory == TRUE,
                  "DDD golden: at tick 114 (t=1150ms), atrial refractory clears (arp_ms=150) in the SAME "
                  "tick the AVI timeout fires a V-pace (since_a=150 >= avi_ms=150) - both dual-chamber "
                  "update_refractory_windows() branches now exercised via the real state machine");
        }
    }
    CHECK(g_v_refractory == FALSE,
          "DDD golden: by tick 135 (t=1350ms = V-pace at 1150 + vrp_ms(200)), the dual-chamber "
          "ventricular-clear branch has fired");
    CHECK(g_state == ST_WAIT_LRL, "DDD golden: settled back in ST_WAIT_LRL after the full A-pace -> AVI -> V-pace cycle");
}

static void test_integration_battery_eol_branch(void)
{
    /* pacer_core_tick()'s own body (not handle_state()) checks
     * battery_check_eol() every tick; the sim deliberately does NOT
     * fault or halt on EOL (see the function's own comment) - it's an
     * intentional smell (inconsistent handling of a critical flag), not
     * a bug. Confirmed directly by forcing the battery below
     * BATT_EOL_MV and checking the sim keeps ticking normally. */
    printf("-- INTEGRATION: pacer_core_tick()'s battery_check_eol() branch --\n");

    test_set_fake_time(1);
    pacer_core_init();
    g_mode = MODE_AOO;
    mode_apply_defaults(MODE_AOO);
    pacer_core_reset_cycle();

    g_batt_mv = 2100; /* < BATT_EOL_MV (2200) */
    g_eol_flag = FALSE;
    CHECK(g_state != ST_FAULT, "precondition: state is not already ST_FAULT before the EOL tick");

    pacer_core_tick(10);

    CHECK(g_eol_flag == TRUE, "a tick with g_batt_mv already below BATT_EOL_MV sets g_eol_flag via battery_check_eol()");
    CHECK(g_state != ST_FAULT,
          "confirmed: reaching EOL does NOT transition the state machine to ST_FAULT or otherwise halt "
          "the sim - the sim just keeps pacing on a depleted battery, exactly as pacer_core_tick()'s own "
          "comment describes ('EOL doesn't actually halt the sim... intentional... as a synthetic code "
          "smell, NOT a statement about how real devices behave')");
}

int main(void)
{
    printf("=== pacer_core.c characterization test suite ===\n");
    printf("(pins CURRENT behavior of the real, linked pacer_core.c state\n");
    printf(" machine as a regression baseline ahead of AI-agent-driven\n");
    printf(" refactoring - the highest-complexity function in the codebase)\n\n");

    /* Tier 1 - MUST run before Tier 2, see file header */
    test_pacer_state_name();
    test_pacer_core_reset_cycle();
    test_pacer_core_init();
    test_handle_state_st_init();
    test_handle_state_dual_chamber_atrial_paced();
    test_handle_state_dual_chamber_non_atrial_paced();
    test_handle_state_single_chamber_ventricular_only();
    test_handle_state_single_chamber_atrial_only();
    test_handle_state_aoo_voo_dead_fallback_finding();
    test_handle_state_fallback_reachable_via_invalid_mode();
    test_handle_state_avi_wait();
    test_handle_state_collapsing_states();
    test_handle_state_fault_and_default();

    /* Tier 2 - real sensing, must come last */
    test_integration_dual_chamber_real_sensing();
    test_integration_single_chamber_real_sensing();
    test_integration_single_chamber_atrial_real_sensing();
    test_integration_refractory_clearing_single_chamber();
    test_integration_refractory_clearing_dual_chamber();
    test_integration_battery_eol_branch();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);

    printf("\nFINDING (confirmed empirically, not just inferred from reading):\n");
    printf("pacer_core_handle_state()'s single-chamber dispatch has a final\n");
    printf("'else' fallback branch whose own comment claims it handles\n");
    printf("VOO/AOO ('pace whichever is due'). It does not: every one of the\n");
    printf("12 currently-defined modes satisfies mode_is_atrial_paced() XOR\n");
    printf("mode_is_ventricular_paced() (or is dual-chamber), so AOO and VOO\n");
    printf("are actually dispatched to the A-only / V-only branches instead.\n");
    printf("The fallback is live code (proven reachable via an out-of-range\n");
    printf("mode value) but dead for every real mode - a second comment/code\n");
    printf("drift finding of the same kind already found in telemetry.c and\n");
    printf("arrhythmia.c. Separately: the ST_AVI_WAIT 'URL safety fallback'\n");
    printf("branch is unreachable under any of the 12 built-in mode presets\n");
    printf("(avi_ms is always well below url_ms), but eeprom_validate() never\n");
    printf("range-checks avi_ms or its relationship to url_ms - so a saved,\n");
    printf("CRC-valid NVRAM block with avi_ms > url_ms would engage it for\n");
    printf("real. Both flagged for the Architecture & Decoupling Agent.\n");

    return (g_fail == 0) ? 0 : 1;
}
