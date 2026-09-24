/* ------------------------------------------------------------------------
 * test_battery.c
 *
 * CHARACTERIZATION TEST SUITE for battery.c.
 *
 * Like sensing.c, battery.c seeds an internal LCG from time(NULL)
 * (battery_init()) - the second source of the run-to-run non-determinism
 * found during Discovery. Neutralized here the same way, via
 * fake_time.c's link-time interposition of time(); battery.c itself is
 * NOT modified.
 *
 * Golden impedance-drift values were captured by direct execution
 * against the real, linked battery.c under a fixed fake-time seed.
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

/* ---------------------------------------------------------------- */

static void test_determinism_itself(void)
{
    DWORD a1, v1, a2, v2;

    printf("-- determinism-fix verification (the point of this whole suite) --\n");

    test_set_fake_time(777); battery_init();
    battery_tick(100, 0);
    a1 = g_lead_a_impedance; v1 = g_lead_v_impedance;

    test_set_fake_time(777); battery_init(); /* identical seed */
    battery_tick(100, 0);
    a2 = g_lead_a_impedance; v2 = g_lead_v_impedance;

    CHECK(a1 == a2 && v1 == v2,
          "identical fake-time seed produces byte-identical lead-impedance output across two runs "
          "(determinism fix in action, no source file modified)");

    test_set_fake_time(778); battery_init(); /* different seed */
    battery_tick(100, 0);
    a2 = g_lead_a_impedance; v2 = g_lead_v_impedance;

    CHECK(!(a1 == a2 && v1 == v2), "a different seed produces different impedance output");
}

static void test_battery_init_defaults(void)
{
    printf("-- battery_init defaults --\n");

    test_set_fake_time(4); battery_init();

    CHECK(g_batt_mv == BATT_NOMINAL_MV, "battery_init sets g_batt_mv == BATT_NOMINAL_MV");
    CHECK(g_lead_a_impedance == 500, "battery_init sets g_lead_a_impedance == 500");
    CHECK(g_lead_v_impedance == 550, "battery_init sets g_lead_v_impedance == 550");
    CHECK(g_eri_flag == FALSE, "battery_init sets g_eri_flag == FALSE");
    CHECK(g_eol_flag == FALSE, "battery_init sets g_eol_flag == FALSE");
}

static void test_idle_drain_integer_truncation(void)
{
    DWORD before;

    printf("-- battery_tick: idle-drain integer-truncation quirk (pinned, documented) --\n");

    test_set_fake_time(4); battery_init();
    before = g_batt_mv;
    battery_tick(5000, 0); /* dt_ms/20000 truncates to 0 for dt_ms < 20000 */
    CHECK(g_batt_mv == before,
          "dt_ms=5000 (< 20000) with idle draw: integer division truncates idle_drain to 0 - "
          "'no visible drain this tick', pinned as documented quirk");

    test_set_fake_time(4); battery_init();
    before = g_batt_mv;
    battery_tick(20000, 0);
    CHECK(g_batt_mv == before - 1, "dt_ms=20000 (exactly one drain unit) reduces g_batt_mv by exactly 1");
}

static void test_pace_drain_formula_and_floor_clamp(void)
{
    DWORD before;

    printf("-- battery_tick: pace-drain formula and floor-clamp-to-1 --\n");

    test_set_fake_time(4); battery_init();
    g_pace_ampl_mv = 3500; g_pace_width_ms = 1;
    before = g_batt_mv;
    battery_tick(0, 1);
    CHECK(g_batt_mv == before - 6,
          "pace_drain = (ampl_mv/1000)*(width_ms+1) = (3500/1000)*(1+1) = 3*2 = 6, matches default-parameter formula exactly");

    test_set_fake_time(4); battery_init();
    g_pace_ampl_mv = 500; g_pace_width_ms = 0; /* (500/1000)*(0+1) = 0*1 = 0, would be a free pace pulse */
    before = g_batt_mv;
    battery_tick(0, 1);
    CHECK(g_batt_mv == before - 1,
          "pace_drain formula would compute 0 for small ampl/width, but is floor-clamped to 1 "
          "('if (pace_drain == 0) pace_drain = 1') - pinned, a pacing pulse is never free");
}

static void test_underflow_clamps_to_zero(void)
{
    printf("-- battery_tick: underflow clamps to 0, does not wrap --\n");

    test_set_fake_time(4); battery_init();
    g_batt_mv = 3;
    battery_tick(20000, 0); /* drain=1, 3 > 1, no underflow yet */
    CHECK(g_batt_mv == 2, "g_batt_mv=3 with drain=1 decrements normally to 2");

    test_set_fake_time(4); battery_init();
    g_batt_mv = 0;
    battery_tick(20000, 0); /* drain=1 > g_batt_mv=0 */
    CHECK(g_batt_mv == 0, "g_batt_mv=0 with drain=1 clamps to 0 rather than underflowing (DWORD wraparound guarded against)");
}

static void test_redundant_eri_eol_side_effects(void)
{
    int r;

    printf("-- battery_check_eri/eol: documented surprising side effect (sets flag, not just reads) --\n");

    test_set_fake_time(4); battery_init();
    g_batt_mv = 2400; /* <= BATT_ERI_MV (2500) */
    g_eri_flag = FALSE;
    r = battery_check_eri();
    CHECK(r == 1, "battery_check_eri() returns 1 when g_batt_mv is at/below BATT_ERI_MV");
    CHECK(g_eri_flag == TRUE,
          "battery_check_eri() SETS g_eri_flag as a side effect, even though it reads like a pure query "
          "(pinned exactly as the source comment calls out)");

    test_set_fake_time(4); battery_init();
    g_batt_mv = 2600; /* > BATT_ERI_MV */
    g_eri_flag = FALSE;
    r = battery_check_eri();
    CHECK(r == 0, "battery_check_eri() returns 0 above the threshold");
    CHECK(g_eri_flag == FALSE, "and does not set the flag when the threshold isn't met");

    test_set_fake_time(4); battery_init();
    g_batt_mv = 2100; /* <= BATT_EOL_MV (2200) */
    g_eol_flag = FALSE;
    r = battery_check_eol();
    CHECK(r == 1 && g_eol_flag == TRUE, "battery_check_eol() mirrors the same pattern for BATT_EOL_MV");

    /* the "flag already set from an earlier tick" fallback path: battery
     * has recovered above threshold in THIS reading, but a prior tick
     * already latched the flag - battery_check_eol() has no way to
     * un-latch it (a one-way ratchet, consistent with a real
     * end-of-life indicator) */
    test_set_fake_time(4); battery_init();
    g_batt_mv = 2800; /* well above BATT_EOL_MV */
    g_eol_flag = TRUE; /* simulating: latched by an earlier, lower reading */
    r = battery_check_eol();
    CHECK(r == 1, "battery_check_eol() returns 1 via the already-latched g_eol_flag even though "
                  "g_batt_mv is currently well above BATT_EOL_MV - the flag is a one-way ratchet, pinned as-is");
}

static void test_lead_impedance_sample_no_bounds_check(void)
{
    printf("-- lead_impedance_sample: channel 0 vs 'anything else', no bounds checking (pinned) --\n");

    test_set_fake_time(4); battery_init();
    g_lead_a_impedance = 111;
    g_lead_v_impedance = 222;

    CHECK(lead_impedance_sample(0) == 111, "channel 0 returns g_lead_a_impedance");
    CHECK(lead_impedance_sample(1) == 222, "channel 1 returns g_lead_v_impedance");
    CHECK(lead_impedance_sample(-5) == 222,
          "channel -5 (invalid/out-of-range) still returns g_lead_v_impedance - no bounds checking, pinned as-is");
    CHECK(lead_impedance_sample(42) == 222, "channel 42 (invalid) also returns g_lead_v_impedance");
}

static void test_impedance_drift_golden_sequence(void)
{
    static const DWORD expected_a[5] = { 681, 735, 661, 619, 577 };
    static const DWORD expected_v[5] = { 496, 606, 828, 842, 552 };
    int i;

    printf("-- lead impedance drift: golden 5-tick sequence, seed=777 --\n");

    test_set_fake_time(777); battery_init();

    for (i = 0; i < 5; i++) {
        char desc_a[80], desc_v[80];
        battery_tick(100, 0);
        snprintf(desc_a, sizeof(desc_a), "tick %d: g_lead_a_impedance matches golden (%lu)", i, (unsigned long)expected_a[i]);
        CHECK(g_lead_a_impedance == expected_a[i], desc_a);
        snprintf(desc_v, sizeof(desc_v), "tick %d: g_lead_v_impedance matches golden (%lu)", i, (unsigned long)expected_v[i]);
        CHECK(g_lead_v_impedance == expected_v[i], desc_v);
    }
}

static void test_rare_lead_fault_window(void)
{
    printf("-- rare simulated lead-fault window: (s_batt_ticks %% 733000) < dt_ms --\n");

    test_set_fake_time(4); battery_init();

    battery_tick(732995, 0); /* prime s_batt_ticks close to the 733000 boundary */
    CHECK(g_lead_v_impedance != (DWORD)(LEAD_IMPEDANCE_HIGH + 500),
          "precondition: fault window has not fired yet before crossing the boundary");

    battery_tick(10, 0); /* crosses 733000 within this tick's dt_ms window */
    CHECK(g_lead_v_impedance == (DWORD)(LEAD_IMPEDANCE_HIGH + 500),
          "crossing the 733000-tick boundary within a single tick's dt_ms deterministically fires the rare "
          "lead-fault window, overriding the LCG-driven value for that tick");
}

int main(void)
{
    printf("=== battery.c characterization test suite ===\n");
    printf("(pins CURRENT behavior of the real, linked battery.c as a\n");
    printf(" regression baseline, using fake_time.c's link-time time()\n");
    printf(" interposition to make its time(NULL)-seeded LCG fully\n");
    printf(" deterministic WITHOUT modifying battery.c itself)\n\n");

    test_determinism_itself();
    test_battery_init_defaults();
    test_idle_drain_integer_truncation();
    test_pace_drain_formula_and_floor_clamp();
    test_underflow_clamps_to_zero();
    test_redundant_eri_eol_side_effects();
    test_lead_impedance_sample_no_bounds_check();
    test_impedance_drift_golden_sequence();
    test_rare_lead_fault_window();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);

    return (g_fail == 0) ? 0 : 1;
}
