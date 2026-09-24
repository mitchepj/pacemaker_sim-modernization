/* ------------------------------------------------------------------------
 * test_telemetry.c
 *
 * CHARACTERIZATION TEST SUITE for telemetry.c.
 *
 * Same philosophy as test_eeprom.c: pin CURRENT, OBSERVED behavior as a
 * regression baseline for the AI-agent-driven refactor, including
 * behavior the source comments call out as an intentional smell (the
 * two-different-checksums trap, the two-different-ways-to-track-
 * ring-buffer-fullness trap).
 *
 * NEW finding from writing this suite, not previously called out by the
 * codebase's own extensive self-documentation of seeded smells:
 * evt_flag_desc()'s header comment says it "only describes the single
 * most 'interesting' bit set... if multiple flags are set the others
 * are just silently not described." The ACTUAL code concatenates every
 * matching flag name via unconditional `if` checks (not an if/else-if
 * chain), so when multiple flags are set, ALL of them appear in the
 * description string. See test_evt_flag_desc_comment_drift() below,
 * which captures real stdout from telemetry_dump_log() to prove this.
 * This is a comment/code drift, not a functional bug -- but a refactor
 * agent that treated the comment as the spec (rather than the verified
 * behavior below) would silently change output format. This is exactly
 * the class of error characterization testing exists to catch.
 *
 * Links the real telemetry.c and eeprom.c (for the divergence check)
 * against the real, unmodified rest of the codebase, same as
 * test_eeprom.c. This file is NEW; nothing in src/ or include/ is
 * modified.
 * ------------------------------------------------------------------------
 */

#include "pacer.h"
#include <assert.h>
#include <unistd.h>

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

#define CAPTURE_FILE "telemetry_capture_tmp.txt"

/* Redirects stdout to CAPTURE_FILE, invokes fn(), restores stdout, and
 * returns the captured text in a caller-owned malloc'd buffer. Used to
 * verify telemetry_flush()/telemetry_dump_log() output as real observed
 * behavior rather than assuming it from source reading. */
static char *capture_stdout(void (*fn)(void))
{
    int saved_fd;
    long sz;
    char *buf;
    FILE *fp;

    fflush(stdout);
    saved_fd = dup(STDOUT_FILENO);
    assert(saved_fd != -1);

    fp = freopen(CAPTURE_FILE, "w", stdout);
    assert(fp != NULL);

    fn();

    fflush(stdout);
    dup2(saved_fd, STDOUT_FILENO);
    close(saved_fd);

    fp = fopen(CAPTURE_FILE, "rb");
    assert(fp != NULL);
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    buf = (char *)malloc((size_t)sz + 1);
    assert(buf != NULL);
    if (sz > 0) {
        size_t n = fread(buf, 1, (size_t)sz, fp);
        (void)n;
    }
    buf[sz] = '\0';
    fclose(fp);
    remove(CAPTURE_FILE);

    return buf;
}

static void reset_all_state(void)
{
    telemetry_init();
    g_tick_ms = 0;
    g_mode = MODE_DDD;
    g_verbose = 0;
}

/* ---------------------------------------------------------------- */

static void test_telemetry_checksum_golden_vectors(void)
{
    UINT16 c1, c2, c3, c1_again;
    UINT16 eeprom_crc_same_input;
    const BYTE vec1[] = "pacemaker_sim_test_vector"; /* 25 bytes, no NUL */
    const BYTE vec2[] = { 0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE };

    printf("-- telemetry_checksum golden-vector / determinism / divergence tests --\n");

    c1 = telemetry_checksum(vec1, 25);
    c2 = telemetry_checksum((const BYTE *)"", 0);
    c3 = telemetry_checksum(vec2, 6);
    c1_again = telemetry_checksum(vec1, 25);

    CHECK(c1 == 0xD16C, "telemetry_checksum(25-byte ASCII vector) == pinned golden 0xD16C");
    CHECK(c2 == 0xFFFF, "telemetry_checksum(empty buffer) == pinned golden 0xFFFF");
    CHECK(c3 == 0x1505, "telemetry_checksum(6-byte binary vector) == pinned golden 0x1505");
    CHECK(c1 == c1_again, "telemetry_checksum is deterministic across repeated calls");

    /* THE TRAP: same input, eeprom.c's crc16_calc must NOT match
     * telemetry.c's checksum. If a "helpful" refactor unifies these two
     * checksum implementations into one shared function, this assertion
     * fails immediately and loudly -- exactly the intent documented in
     * telemetry.c's own header comment. */
    eeprom_crc_same_input = crc16_calc(vec1, 25);
    CHECK(eeprom_crc_same_input == 0x6E36, "crc16_calc(same 25-byte vector) == pinned golden 0x6E36 (eeprom.c)");
    CHECK(c1 != eeprom_crc_same_input,
          "telemetry_checksum() and crc16_calc() MUST diverge on identical input (unification trap guard)");
}

static void test_telemetry_build_packet_byte_layout(void)
{
    int len;
    BYTE evt_flags;

    printf("-- telemetry_build_packet byte-layout tests --\n");

    reset_all_state();

    /* Seed one history sample so build_packet's "reuse the previous
     * sample" behavior (a documented API oversight, preserved on
     * purpose) has a known, non-zero value to pull from. */
    g_egm_hist[0].a_mv = 321;
    g_egm_hist[0].v_mv = -654;
    g_egm_hist[0].t_ms = 999;
    g_egm_hist_idx = 1; /* prev_idx will resolve to 0 */

    g_tick_ms = 0x01020304UL;
    g_mode = MODE_VVI; /* enum value 3 */
    evt_flags = EVT_A_PACE | EVT_V_SENSE;

    len = telemetry_build_packet(evt_flags);

    CHECK(len == 14, "telemetry_build_packet returns PKT_TOTAL_LEN == 14");
    CHECK(g_telemetry_len == 14, "telemetry_build_packet sets g_telemetry_len == 14");
    CHECK(g_telemetry_buf[0] == 0xAA, "byte 0 is sync byte 0xAA");
    CHECK(g_telemetry_buf[1] == 9, "byte 1 is payload length 9");
    CHECK(g_telemetry_buf[2] == evt_flags, "byte 2 is the evt_flags passed in");
    CHECK(g_telemetry_buf[3] == (BYTE)(0x04), "byte 3 is tick_ms LSB (0x04)");
    CHECK(g_telemetry_buf[4] == (BYTE)(0x03), "byte 4 is tick_ms byte 1 (0x03)");
    CHECK(g_telemetry_buf[5] == (BYTE)(0x02), "byte 5 is tick_ms byte 2 (0x02)");
    CHECK(g_telemetry_buf[6] == (BYTE)(0x01), "byte 6 is tick_ms MSB (0x01)");
    CHECK(g_telemetry_buf[7] == (BYTE)(321 & 0xFF), "byte 7 is a_mv LSB, from PREVIOUS history sample (321)");
    CHECK(g_telemetry_buf[8] == (BYTE)((321 >> 8) & 0xFF), "byte 8 is a_mv MSB (321)");
    {
        INT16 v = -654;
        CHECK(g_telemetry_buf[9] == (BYTE)(v & 0xFF), "byte 9 is v_mv LSB, from PREVIOUS history sample (-654)");
        CHECK(g_telemetry_buf[10] == (BYTE)((v >> 8) & 0xFF), "byte 10 is v_mv MSB (-654)");
    }
    CHECK(g_telemetry_buf[11] == (BYTE)MODE_VVI, "byte 11 is g_mode");
    {
        UINT16 expected_chk = telemetry_checksum(g_telemetry_buf, 12);
        CHECK(g_telemetry_buf[12] == (BYTE)(expected_chk & 0xFF), "byte 12 is checksum LSB over bytes[0..11]");
        CHECK(g_telemetry_buf[13] == (BYTE)((expected_chk >> 8) & 0xFF), "byte 13 is checksum MSB over bytes[0..11]");
    }
}

static void test_telemetry_build_packet_history_wraparound(void)
{
    int len;

    printf("-- telemetry_build_packet history-index wraparound test --\n");

    reset_all_state();

    /* g_egm_hist_idx == 0 must wrap to EGM_HISTORY_LEN - 1, not underflow */
    g_egm_hist[EGM_HISTORY_LEN - 1].a_mv = 111;
    g_egm_hist[EGM_HISTORY_LEN - 1].v_mv = 222;
    g_egm_hist_idx = 0;

    len = telemetry_build_packet(EVT_NOISE);

    CHECK(len == 14, "build_packet still returns 14 at history-index wraparound boundary");
    CHECK(g_telemetry_buf[7] == (BYTE)(111 & 0xFF), "wraparound: a_mv pulled from g_egm_hist[EGM_HISTORY_LEN-1], not index -1");
    CHECK(g_telemetry_buf[9] == (BYTE)(222 & 0xFF), "wraparound: v_mv pulled from g_egm_hist[EGM_HISTORY_LEN-1]");
}

static void test_telemetry_flush_verbose_vs_quiet(void)
{
    char *out;

    printf("-- telemetry_flush verbose/quiet behavior tests --\n");

    reset_all_state();
    (void)telemetry_build_packet(EVT_A_PACE);

    g_verbose = 0;
    out = capture_stdout(telemetry_flush);
    CHECK(out[0] == '\0', "telemetry_flush() prints nothing when g_verbose == 0");
    CHECK(g_telemetry_len == 0, "telemetry_flush() resets g_telemetry_len to 0 even when quiet");
    free(out);

    reset_all_state();
    (void)telemetry_build_packet(EVT_A_PACE);
    g_verbose = 1;
    out = capture_stdout(telemetry_flush);
    CHECK(strstr(out, "[telemetry] pkt(") != NULL, "telemetry_flush() prints a hex dump header when g_verbose == 1");
    CHECK(strstr(out, "AA") != NULL, "telemetry_flush() verbose output includes the sync byte AA in hex");
    CHECK(g_telemetry_len == 0, "telemetry_flush() resets g_telemetry_len to 0 after verbose print");
    free(out);

    g_verbose = 0;
}

static void test_telemetry_log_event_ignores_evt_none(void)
{
    printf("-- telemetry_log_event(EVT_NONE) no-op test --\n");

    reset_all_state();
    telemetry_log_event(EVT_NONE, 100, 200);

    CHECK(g_log_count == 0, "telemetry_log_event(EVT_NONE, ...) does not record a log entry");
}

static void test_telemetry_log_event_ring_buffer_wrap(void)
{
    UINT32 i;
    UINT32 push_count;
    UINT32 idx;

    printf("-- telemetry_log_event ring-buffer overwrite/ordering test --\n");

    reset_all_state();

    push_count = MAX_LOG_ENTRIES + 10; /* force wraparound */
    for (i = 0; i < push_count; i++) {
        g_tick_ms = i * 10; /* distinct, increasing marker per push */
        telemetry_log_event(EVT_V_PACE, (INT16)i, (INT16)(i * 2));
    }

    CHECK(g_log_count == MAX_LOG_ENTRIES,
          "g_log_count caps at MAX_LOG_ENTRIES after overwriting wraps (does not exceed capacity)");

    /* Oldest surviving entry should be push #10 (0-indexed), since pushes
     * 0..9 were evicted by the wrap. This directly exercises the same
     * index arithmetic telemetry_dump_log() uses (g_log_head + i) %
     * MAX_LOG_ENTRIES, confirming the file header's claim that the two
     * independently-computed "fullness" mechanisms currently agree. */
    idx = g_log_head;
    CHECK(g_log[idx].t_ms == 10 * 10,
          "oldest surviving entry after wrap is push #10 (t_ms == 100), confirming ring eviction order");

    idx = (g_log_head + MAX_LOG_ENTRIES - 1) % MAX_LOG_ENTRIES;
    CHECK(g_log[idx].t_ms == (push_count - 1) * 10,
          "newest entry after wrap is the very last push, at the expected wrapped index");
}

static void test_evt_flag_desc_comment_drift(void)
{
    char *out;
    int count_names;

    printf("-- evt_flag_desc(): comment-vs-code drift verification --\n");
    printf("   (header comment claims only ONE flag name is ever shown;\n");
    printf("    verifying actual behavior via real stdout capture)\n");

    reset_all_state();
    g_tick_ms = 555;
    /* Two flags set at once: per the comment, only "MODE_SWITCH" (higher
     * priority in the checked order) should appear and A_PACE should be
     * "silently not described". */
    telemetry_log_event((BYTE)(EVT_MODE_SWITCH | EVT_A_PACE), 10, 20);

    out = capture_stdout(telemetry_dump_log);

    count_names = 0;
    if (strstr(out, "MODE_SWITCH") != NULL) count_names++;
    if (strstr(out, "A_PACE") != NULL) count_names++;

    CHECK(strstr(out, "MODE_SWITCH") != NULL, "dump_log output includes MODE_SWITCH (higher-priority flag)");
    CHECK(strstr(out, "A_PACE") != NULL,
          "dump_log output ALSO includes A_PACE -- contradicts the source comment's claim that only "
          "one flag is ever shown; actual code uses unconditional ifs, not if/else-if. "
          "PINNED AS ACTUAL BEHAVIOR: do not 'fix' this to match the comment without a reviewed decision.");
    CHECK(count_names == 2, "exactly both set flag names appear in the description (full concatenation, not single-flag)");

    free(out);
}

static void test_evt_flag_desc_remaining_branches(void)
{
    char *out;

    printf("-- evt_flag_desc(): remaining flag/NONE branch coverage --\n");

    reset_all_state();
    g_tick_ms = 1;
    telemetry_log_event(EVT_V_PACE, 1, 1);
    out = capture_stdout(telemetry_dump_log);
    CHECK(strstr(out, "V_PACE") != NULL, "dump_log describes a lone EVT_V_PACE flag");
    free(out);

    reset_all_state();
    g_tick_ms = 2;
    telemetry_log_event(EVT_A_SENSE, 1, 1);
    out = capture_stdout(telemetry_dump_log);
    CHECK(strstr(out, "A_SENSE") != NULL, "dump_log describes a lone EVT_A_SENSE flag");
    free(out);

    reset_all_state();
    g_tick_ms = 3;
    telemetry_log_event(EVT_V_SENSE, 1, 1);
    out = capture_stdout(telemetry_dump_log);
    CHECK(strstr(out, "V_SENSE") != NULL, "dump_log describes a lone EVT_V_SENSE flag");
    free(out);

    reset_all_state();
    g_tick_ms = 4;
    telemetry_log_event(EVT_NOISE, 1, 1);
    out = capture_stdout(telemetry_dump_log);
    CHECK(strstr(out, "NOISE") != NULL, "dump_log describes a lone EVT_NOISE flag");
    free(out);

    /* evt_flag_desc's "NONE" fallback is reachable only when flags == 0,
     * but telemetry_log_event(EVT_NONE, ...) is a no-op (see test above)
     * and never stores a zero-flags entry. So this branch is only
     * reachable if a log entry's flags are zero for some OTHER reason. In
     * the current codebase, that can't happen through the public API -
     * telemetry_init() zero-fills g_log[] but g_log_count starts at 0,
     * so dump_log() never iterates a slot that was never legitimately
     * written by telemetry_log_event(). Confirmed unreachable through
     * the public API rather than assumed; left uncovered on purpose. */
    printf("  [SKIP] evt_flag_desc's \"NONE\" fallback: unreachable through the public API "
           "(telemetry_log_event(EVT_NONE,...) is a no-op, so no zero-flags entry is ever stored) - "
           "confirmed, not assumed; left uncovered on purpose\n");
}

int main(void)
{
    printf("=== telemetry.c characterization test suite ===\n");
    printf("(pins CURRENT behavior of the real, linked telemetry.c as a\n");
    printf(" regression baseline ahead of AI-agent-driven refactoring)\n\n");

    test_telemetry_checksum_golden_vectors();
    test_telemetry_build_packet_byte_layout();
    test_telemetry_build_packet_history_wraparound();
    test_telemetry_flush_verbose_vs_quiet();
    test_telemetry_log_event_ignores_evt_none();
    test_telemetry_log_event_ring_buffer_wrap();
    test_evt_flag_desc_comment_drift();
    test_evt_flag_desc_remaining_branches();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);

    printf("\nKNOWN GAP (not covered): telemetry_build_packet()'s\n");
    printf("PKT_TOTAL_LEN > TELEMETRY_BUF_SZ overflow-refusal branch (line\n");
    printf("~86) is unreachable with the codebase's own compile-time\n");
    printf("constants (14 <= 128 always) and is not exercised here.\n");

    return (g_fail == 0) ? 0 : 1;
}
