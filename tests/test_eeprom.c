/* ------------------------------------------------------------------------
 * test_eeprom.c
 *
 * CHARACTERIZATION TEST SUITE for eeprom.c.
 *
 * Purpose: pin the CURRENT, OBSERVED behavior of eeprom.c as a safety net
 * for the AI-agent-driven refactor of pacemaker_sim. This is NOT a spec-
 * conformance suite -- several assertions below intentionally lock in
 * behavior that the source comments themselves flag as a "smell" (e.g.
 * eeprom_load() collapsing four distinct failure modes into one, or
 * eeprom_load() silently persisting defaults back to disk). That is by
 * design: a refactor tool must not be allowed to "fix" that behavior
 * silently. If a fix is wanted, it should make one of these assertions
 * fail on purpose, forcing a conscious, reviewed change to this file
 * alongside the source change.
 *
 * Scope: links the real eeprom.c against the real pacemaker_sim object
 * graph (pacer_core.c/modes.c/sensing.c/telemetry.c/battery.c/
 * arrhythmia.c) rather than mocking globals, so nothing about the real
 * struct layout, global storage, or constant values is guessed.
 *
 * This file is NEW. It does not modify any existing pacemaker_sim
 * source file. Run from within tests/ (see Makefile) so NVRAM_FILE
 * ("pacer_nvram.bin") is created/destroyed in this directory only.
 *
 * Golden CRC16 vectors below were captured by direct execution of
 * crc16_calc() against the vectors shown, not derived from an external
 * CRC-16/ARC reference implementation -- this is a characterization
 * baseline, not a spec conformance check.
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

static void reset_globals_to_zero(void)
{
    g_mode = (PaceMode_t)0;
    g_lrl_ms = 0;
    g_url_ms = 0;
    g_avi_ms = 0;
    g_pvarp_ms = 0;
    g_vrp_ms = 0;
    g_arp_ms = 0;
    g_pace_ampl_mv = 0;
    g_pace_width_ms = 0;
    g_a_sense_thresh_mv = 0;
    g_v_sense_thresh_mv = 0;
    g_rate_resp_enabled = 0;
}

static void set_globals_nondefault(void)
{
    g_mode = MODE_VVI;
    g_lrl_ms = 850;
    g_url_ms = 400;
    g_avi_ms = 175;
    g_pvarp_ms = 275;
    g_vrp_ms = 225;
    g_arp_ms = 175;
    g_pace_ampl_mv = 2500;
    g_pace_width_ms = 2;
    g_a_sense_thresh_mv = 300;
    g_v_sense_thresh_mv = 600;
    g_rate_resp_enabled = 1;
}

static void remove_nvram_file(void)
{
    remove(NVRAM_FILE);
}

/* ---------------------------------------------------------------- */

static void test_crc16_golden_vectors(void)
{
    UINT16 c1, c2, c3, c1_again;
    const BYTE vec1[] = "pacemaker_sim_test_vector"; /* 25 bytes, no NUL */
    const BYTE vec3[] = { 0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE };

    printf("-- crc16_calc golden-vector / determinism tests --\n");

    c1 = crc16_calc(vec1, 25);
    c2 = crc16_calc((const BYTE *)"", 0);
    c3 = crc16_calc(vec3, 6);
    c1_again = crc16_calc(vec1, 25);

    CHECK(c1 == 0x6E36, "crc16_calc(25-byte ASCII vector) == pinned golden 0x6E36");
    CHECK(c2 == 0xFFFF, "crc16_calc(empty buffer) == pinned golden 0xFFFF (initial CRC unchanged)");
    CHECK(c3 == 0x130C, "crc16_calc(6-byte binary vector) == pinned golden 0x130C");
    CHECK(c1 == c1_again, "crc16_calc is deterministic across repeated calls on identical input");
}

static void test_eeprom_defaults(void)
{
    NvramParams_t p;

    printf("-- eeprom_defaults tests --\n");

    memset(&p, 0xAA, sizeof(p)); /* poison first, so zero-fill is visible */
    eeprom_defaults(&p);

    CHECK(p.magic == NVRAM_MAGIC, "eeprom_defaults sets magic == NVRAM_MAGIC");
    CHECK(p.mode == (BYTE)MODE_DDD, "eeprom_defaults sets mode == MODE_DDD");
    CHECK(p.lrl_ms == DEFAULT_LRL_MS, "eeprom_defaults sets lrl_ms == DEFAULT_LRL_MS");
    CHECK(p.url_ms == DEFAULT_URL_MS, "eeprom_defaults sets url_ms == DEFAULT_URL_MS");
    CHECK(p.avi_ms == DEFAULT_AVI_MS, "eeprom_defaults sets avi_ms == DEFAULT_AVI_MS");
    CHECK(p.pvarp_ms == DEFAULT_PVARP_MS, "eeprom_defaults sets pvarp_ms == DEFAULT_PVARP_MS");
    CHECK(p.vrp_ms == DEFAULT_VRP_MS, "eeprom_defaults sets vrp_ms == DEFAULT_VRP_MS");
    CHECK(p.arp_ms == DEFAULT_ARP_MS, "eeprom_defaults sets arp_ms == DEFAULT_ARP_MS");
    CHECK(p.pace_ampl_mv == DEFAULT_PACE_AMPL_MV, "eeprom_defaults sets pace_ampl_mv == DEFAULT_PACE_AMPL_MV");
    CHECK(p.pace_width_ms == DEFAULT_PACE_WIDTH_MS, "eeprom_defaults sets pace_width_ms == DEFAULT_PACE_WIDTH_MS");
    CHECK(p.a_sense_thresh_mv == A_SENSE_THRESH_MV, "eeprom_defaults sets a_sense_thresh_mv == A_SENSE_THRESH_MV");
    CHECK(p.v_sense_thresh_mv == V_SENSE_THRESH_MV, "eeprom_defaults sets v_sense_thresh_mv == V_SENSE_THRESH_MV");
    CHECK(p.rate_resp_enabled == 0, "eeprom_defaults sets rate_resp_enabled == 0");

    eeprom_defaults(NULL); /* must not crash - defensive NULL branch */
    CHECK(1, "eeprom_defaults(NULL) returned without crashing (defensive-NULL branch)");
}

static void test_eeprom_validate(void)
{
    NvramParams_t p;
    UINT16 crc;

    printf("-- eeprom_validate tests --\n");

    eeprom_defaults(&p);
    crc = crc16_calc((const BYTE *)&p, (int)sizeof(NvramParams_t));
    CHECK(eeprom_validate(&p, crc) == 1, "eeprom_validate accepts defaults + correct CRC");

    CHECK(eeprom_validate(NULL, crc) == 0, "eeprom_validate rejects NULL params");

    eeprom_defaults(&p);
    p.magic = 0x1234;
    CHECK(eeprom_validate(&p, crc) == 0, "eeprom_validate rejects wrong magic (CRC now stale too, still must reject on magic)");

    eeprom_defaults(&p);
    CHECK(eeprom_validate(&p, (UINT16)(crc ^ 0xFFFF)) == 0, "eeprom_validate rejects mismatched CRC");

    eeprom_defaults(&p);
    p.lrl_ms = 299; /* just below documented [300,2000] bound */
    crc = crc16_calc((const BYTE *)&p, (int)sizeof(NvramParams_t));
    CHECK(eeprom_validate(&p, crc) == 0, "eeprom_validate rejects lrl_ms below lower bound (299 < 300)");

    eeprom_defaults(&p);
    p.lrl_ms = 2001; /* just above documented bound */
    crc = crc16_calc((const BYTE *)&p, (int)sizeof(NvramParams_t));
    CHECK(eeprom_validate(&p, crc) == 0, "eeprom_validate rejects lrl_ms above upper bound (2001 > 2000)");

    eeprom_defaults(&p);
    p.url_ms = 249;
    crc = crc16_calc((const BYTE *)&p, (int)sizeof(NvramParams_t));
    CHECK(eeprom_validate(&p, crc) == 0, "eeprom_validate rejects url_ms below lower bound (249 < 250)");

    eeprom_defaults(&p);
    p.url_ms = 1501;
    crc = crc16_calc((const BYTE *)&p, (int)sizeof(NvramParams_t));
    CHECK(eeprom_validate(&p, crc) == 0, "eeprom_validate rejects url_ms above upper bound (1501 > 1500)");

    eeprom_defaults(&p);
    p.mode = (BYTE)MODE_COUNT; /* one past last valid mode */
    crc = crc16_calc((const BYTE *)&p, (int)sizeof(NvramParams_t));
    CHECK(eeprom_validate(&p, crc) == 0, "eeprom_validate rejects mode == MODE_COUNT (out of range)");
}

static void test_eeprom_save_load_roundtrip(void)
{
    int rc;

    printf("-- eeprom_save/eeprom_load round-trip tests --\n");

    remove_nvram_file();
    set_globals_nondefault();

    rc = eeprom_save();
    CHECK(rc == 1, "eeprom_save() returns 1 on successful write");

    reset_globals_to_zero();
    rc = eeprom_load();

    CHECK(rc == 1, "eeprom_load() returns 1 when a valid file is loaded");
    CHECK(g_mode == MODE_VVI, "eeprom_load restores g_mode");
    CHECK(g_lrl_ms == 850, "eeprom_load restores g_lrl_ms");
    CHECK(g_url_ms == 400, "eeprom_load restores g_url_ms");
    CHECK(g_avi_ms == 175, "eeprom_load restores g_avi_ms");
    CHECK(g_pvarp_ms == 275, "eeprom_load restores g_pvarp_ms");
    CHECK(g_vrp_ms == 225, "eeprom_load restores g_vrp_ms");
    CHECK(g_arp_ms == 175, "eeprom_load restores g_arp_ms");
    CHECK(g_pace_ampl_mv == 2500, "eeprom_load restores g_pace_ampl_mv");
    CHECK(g_pace_width_ms == 2, "eeprom_load restores g_pace_width_ms");
    CHECK(g_a_sense_thresh_mv == 300, "eeprom_load restores g_a_sense_thresh_mv");
    CHECK(g_v_sense_thresh_mv == 600, "eeprom_load restores g_v_sense_thresh_mv");
    CHECK(g_rate_resp_enabled == 1, "eeprom_load restores g_rate_resp_enabled");

    remove_nvram_file();
}

static void test_eeprom_load_missing_file(void)
{
    int rc;

    printf("-- eeprom_load: missing-file behavior (documented smell, pinned) --\n");

    remove_nvram_file();
    reset_globals_to_zero();

    rc = eeprom_load();

    CHECK(rc == 0, "eeprom_load() returns 0 when NVRAM_FILE is missing");
    CHECK(g_mode == MODE_DDD, "eeprom_load falls back to default g_mode when file missing");
    CHECK(g_lrl_ms == DEFAULT_LRL_MS, "eeprom_load falls back to DEFAULT_LRL_MS when file missing");

    /* Pins the documented "load conflated with load-or-initialize" smell:
     * a missing-file load is expected to WRITE the defaults back out. If a
     * refactor removes this side effect, this assertion should fail and
     * force a deliberate decision, not a silent behavior change. */
    {
        FILE *fp = fopen(NVRAM_FILE, "rb");
        CHECK(fp != NULL, "eeprom_load() persists defaults back to NVRAM_FILE after a missing-file fallback (documented side effect)");
        if (fp != NULL) {
            fclose(fp);
        }
    }

    remove_nvram_file();
}

static void test_eeprom_load_short_read(void)
{
    FILE *fp;
    int rc;
    const char garbage[4] = { 1, 2, 3, 4 };

    printf("-- eeprom_load: truncated-file behavior (documented smell, pinned) --\n");

    remove_nvram_file();
    fp = fopen(NVRAM_FILE, "wb");
    assert(fp != NULL);
    fwrite(garbage, 1, sizeof(garbage), fp);
    fclose(fp);

    reset_globals_to_zero();
    rc = eeprom_load();

    CHECK(rc == 0, "eeprom_load() returns 0 on a short/truncated struct read, same as missing file");
    CHECK(g_mode == MODE_DDD, "eeprom_load falls back to defaults on short read");

    remove_nvram_file();
}

static void test_eeprom_load_missing_crc(void)
{
    FILE *fp;
    NvramParams_t p;
    int rc;

    printf("-- eeprom_load: struct-present-but-no-trailing-CRC behavior (pinned) --\n");

    remove_nvram_file();
    eeprom_defaults(&p);
    fp = fopen(NVRAM_FILE, "wb");
    assert(fp != NULL);
    fwrite(&p, 1, sizeof(p), fp); /* full struct, but no CRC bytes follow */
    fclose(fp);

    reset_globals_to_zero();
    rc = eeprom_load();

    CHECK(rc == 0, "eeprom_load() returns 0 when the struct is present but the trailing CRC is missing");

    remove_nvram_file();
}

static void test_eeprom_load_corrupt_crc(void)
{
    FILE *fp;
    NvramParams_t p;
    UINT16 bad_crc;
    int rc;

    printf("-- eeprom_load: valid struct + wrong CRC behavior (pinned) --\n");

    remove_nvram_file();
    eeprom_defaults(&p);
    bad_crc = (UINT16)(crc16_calc((const BYTE *)&p, (int)sizeof(p)) ^ 0x00FF);

    fp = fopen(NVRAM_FILE, "wb");
    assert(fp != NULL);
    fwrite(&p, 1, sizeof(p), fp);
    fwrite(&bad_crc, 1, sizeof(bad_crc), fp);
    fclose(fp);

    reset_globals_to_zero();
    rc = eeprom_load();

    CHECK(rc == 0, "eeprom_load() returns 0 when the stored CRC does not match the stored struct");
    CHECK(g_mode == MODE_DDD, "eeprom_load falls back to defaults on CRC mismatch");

    remove_nvram_file();
}

static void test_eeprom_verbose_branches(void)
{
    FILE *fp;
    NvramParams_t p;
    UINT16 bad_crc;
    const char garbage[4] = { 1, 2, 3, 4 };

    printf("-- eeprom_load: g_verbose=1 diagnostic-branch coverage --\n");
    g_verbose = 1;

    remove_nvram_file();
    reset_globals_to_zero();
    CHECK(eeprom_load() == 0, "eeprom_load() (verbose) still returns 0 on missing file");

    remove_nvram_file();
    fp = fopen(NVRAM_FILE, "wb");
    assert(fp != NULL);
    fwrite(garbage, 1, sizeof(garbage), fp);
    fclose(fp);
    reset_globals_to_zero();
    CHECK(eeprom_load() == 0, "eeprom_load() (verbose) still returns 0 on short read");

    remove_nvram_file();
    eeprom_defaults(&p);
    fp = fopen(NVRAM_FILE, "wb");
    assert(fp != NULL);
    fwrite(&p, 1, sizeof(p), fp);
    fclose(fp);
    reset_globals_to_zero();
    CHECK(eeprom_load() == 0, "eeprom_load() (verbose) still returns 0 on missing CRC");

    remove_nvram_file();
    eeprom_defaults(&p);
    bad_crc = (UINT16)(crc16_calc((const BYTE *)&p, (int)sizeof(p)) ^ 0x00FF);
    fp = fopen(NVRAM_FILE, "wb");
    assert(fp != NULL);
    fwrite(&p, 1, sizeof(p), fp);
    fwrite(&bad_crc, 1, sizeof(bad_crc), fp);
    fclose(fp);
    reset_globals_to_zero();
    CHECK(eeprom_load() == 0, "eeprom_load() (verbose) still returns 0 on CRC mismatch");

    g_verbose = 0;
    remove_nvram_file();
}

int main(void)
{
    printf("=== eeprom.c characterization test suite ===\n");
    printf("(pins CURRENT behavior of the real, linked eeprom.c as a\n");
    printf(" regression baseline ahead of AI-agent-driven refactoring)\n\n");

    test_crc16_golden_vectors();
    test_eeprom_defaults();
    test_eeprom_validate();
    test_eeprom_save_load_roundtrip();
    test_eeprom_load_missing_file();
    test_eeprom_load_short_read();
    test_eeprom_load_missing_crc();
    test_eeprom_load_corrupt_crc();
    test_eeprom_verbose_branches();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);

    /* Known, intentionally untested gap for the audit trail: eeprom_save()'s
     * fopen() failure branch (line ~219 of eeprom.c) is not exercised here.
     * Triggering it deterministically requires an unwritable NVRAM_FILE
     * path, which is platform/permission-dependent; flagged rather than
     * faked with an unreliable chmod trick. */
    printf("\nKNOWN GAP (not covered): eeprom_save() fopen()-failure branch\n");
    printf("(unwritable-path case) - requires a platform-specific\n");
    printf("permission setup outside the scope of this characterization\n");
    printf("pass; flagged for manual/CI-environment follow-up.\n");

    return (g_fail == 0) ? 0 : 1;
}
