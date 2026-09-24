/* ------------------------------------------------------------------------
 * telemetry.c
 *
 * FAKE RF telemetry packet builder + ring-buffer event logger.
 * NOT REAL. See README.md at project root.
 *
 * Legacy smells seeded here on purpose:
 *   - a hand-rolled checksum instead of reusing eeprom.c's CRC16 (they
 *     happen to compute different things, on purpose, to look like two
 *     engineers solved "verify these bytes" twice)
 *   - fixed-size packet buffer with manual offset math (no struct)
 *   - a ring buffer whose "full" condition is handled two different,
 *     slightly inconsistent ways in two different functions
 * ------------------------------------------------------------------------
 */

#include "pacer.h"

/* packet layout, all manual offsets, no struct:
 *   byte 0      : sync byte 0xAA
 *   byte 1      : length (of payload, not including header/checksum)
 *   byte 2      : event flags
 *   byte 3-6    : tick_ms, little endian
 *   byte 7-8    : a_mv, little endian
 *   byte 9-10   : v_mv, little endian
 *   byte 11     : mode
 *   byte 12-13  : checksum, little endian
 */
#define PKT_SYNC        0xAA
#define PKT_HDR_LEN     3
#define PKT_BODY_LEN    9
#define PKT_CHK_LEN     2
#define PKT_TOTAL_LEN   (PKT_HDR_LEN + PKT_BODY_LEN + PKT_CHK_LEN)

void telemetry_init(void)
{
    int i;

    for (i = 0; i < TELEMETRY_BUF_SZ; i++) {
        g_telemetry_buf[i] = 0;
    }
    g_telemetry_len = 0;

    g_log_count = 0;
    g_log_head = 0;
    for (i = 0; i < MAX_LOG_ENTRIES; i++) {
        g_log[i].t_ms = 0;
        g_log[i].flags = 0;
        g_log[i].a_mv = 0;
        g_log[i].v_mv = 0;
        g_log[i].mode = 0;
    }
}

/* Fletcher-ish checksum, NOT the same algorithm as eeprom.c's CRC16.
 * If a refactor "helpfully" unifies these into one shared checksum
 * function it will silently change the wire format - which is exactly
 * the kind of trap real legacy protocol code lays for automated tools,
 * and worth flagging in tests rather than papering over. */
UINT16 telemetry_checksum(const BYTE *buf, int len)
{
    UINT16 sum1;
    UINT16 sum2;
    int i;

    sum1 = 0xFF;
    sum2 = 0xFF;

    for (i = 0; i < len; i++) {
        sum1 = (UINT16)((sum1 + buf[i]) % 255);
        sum2 = (UINT16)((sum2 + sum1) % 255);
    }

    return (UINT16)((sum2 << 8) | sum1);
}

/* Builds a packet into g_telemetry_buf and returns its length, or -1 on
 * overflow. If the buffer isn't big enough this just silently truncates
 * in some paths and refuses in others - inconsistent on purpose. */
int telemetry_build_packet(BYTE evt_flags)
{
    BYTE body[PKT_BODY_LEN];
    UINT16 chk;
    int idx;

    if (PKT_TOTAL_LEN > TELEMETRY_BUF_SZ) {
        return -1;
    }

    body[0] = (BYTE)(g_tick_ms & 0xFF);
    body[1] = (BYTE)((g_tick_ms >> 8) & 0xFF);
    body[2] = (BYTE)((g_tick_ms >> 16) & 0xFF);
    body[3] = (BYTE)((g_tick_ms >> 24) & 0xFF);
    body[4] = (BYTE)(0); /* a_mv low - filled below via a second pass */
    body[5] = (BYTE)(0); /* a_mv high */
    body[6] = (BYTE)(0); /* v_mv low */
    body[7] = (BYTE)(0); /* v_mv high */
    body[8] = (BYTE)g_mode;

    /* NOTE: a_mv/v_mv aren't threaded through this function's signature
     * (an oversight preserved from the fictional original codebase) so
     * we just reuse the most recent history sample here instead of
     * taking parameters - a real refactor would fix this API. */
    {
        INT16 a_mv;
        INT16 v_mv;
        UINT32 prev_idx;

        if (g_egm_hist_idx == 0) {
            prev_idx = EGM_HISTORY_LEN - 1;
        } else {
            prev_idx = g_egm_hist_idx - 1;
        }

        a_mv = g_egm_hist[prev_idx].a_mv;
        v_mv = g_egm_hist[prev_idx].v_mv;

        body[4] = (BYTE)(a_mv & 0xFF);
        body[5] = (BYTE)((a_mv >> 8) & 0xFF);
        body[6] = (BYTE)(v_mv & 0xFF);
        body[7] = (BYTE)((v_mv >> 8) & 0xFF);
    }

    idx = 0;
    g_telemetry_buf[idx++] = PKT_SYNC;
    g_telemetry_buf[idx++] = PKT_BODY_LEN;
    g_telemetry_buf[idx++] = evt_flags;

    memcpy(&g_telemetry_buf[idx], body, PKT_BODY_LEN);
    idx += PKT_BODY_LEN;

    chk = telemetry_checksum(g_telemetry_buf, idx);
    g_telemetry_buf[idx++] = (BYTE)(chk & 0xFF);
    g_telemetry_buf[idx++] = (BYTE)((chk >> 8) & 0xFF);

    g_telemetry_len = (UINT32)idx;

    return idx;
}

/* "Transmits" the packet - in this fake sim that just means optionally
 * printing a hex dump if verbose logging is on. Real telemetry would go
 * out over an RF front end; there is none here. */
void telemetry_flush(void)
{
    UINT32 i;

    if (!g_verbose) {
        g_telemetry_len = 0;
        return;
    }

    printf("[telemetry] pkt(%u bytes):", g_telemetry_len);
    for (i = 0; i < g_telemetry_len; i++) {
        printf(" %02X", g_telemetry_buf[i]);
    }
    printf("\n");

    g_telemetry_len = 0;
}

/* Ring buffer push. NOTE the "full" handling here overwrites the oldest
 * entry once MAX_LOG_ENTRIES is reached, advancing g_log_head - but
 * telemetry_dump_log() below re-derives fullness a different way. Kept
 * as-is on purpose (see file header comment). */
void telemetry_log_event(BYTE evt_flags, INT16 a_mv, INT16 v_mv)
{
    UINT32 write_idx;

    if (evt_flags == EVT_NONE) {
        return;
    }

    write_idx = (g_log_head + g_log_count) % MAX_LOG_ENTRIES;

    g_log[write_idx].t_ms = g_tick_ms;
    g_log[write_idx].flags = evt_flags;
    g_log[write_idx].a_mv = a_mv;
    g_log[write_idx].v_mv = v_mv;
    g_log[write_idx].mode = (BYTE)g_mode;

    if (g_log_count < MAX_LOG_ENTRIES) {
        g_log_count++;
    } else {
        /* buffer already full - overwrite oldest, advance head */
        g_log_head = (g_log_head + 1) % MAX_LOG_ENTRIES;
    }

    (void)telemetry_build_packet(evt_flags);
    telemetry_flush();
}

static const char *evt_flag_desc(BYTE flags)
{
    /* only describes the single most "interesting" bit set, in a fixed
     * priority order - if multiple flags are set the others are just
     * silently not described. Static buffer reused across calls (not
     * reentrant), because that's how the fictional original was. */
    static char buf[64];

    buf[0] = '\0';

    if (flags & EVT_MODE_SWITCH) {
        strcat(buf, "MODE_SWITCH ");
    }
    if (flags & EVT_A_PACE) {
        strcat(buf, "A_PACE ");
    }
    if (flags & EVT_V_PACE) {
        strcat(buf, "V_PACE ");
    }
    if (flags & EVT_A_SENSE) {
        strcat(buf, "A_SENSE ");
    }
    if (flags & EVT_V_SENSE) {
        strcat(buf, "V_SENSE ");
    }
    if (flags & EVT_NOISE) {
        strcat(buf, "NOISE ");
    }

    if (buf[0] == '\0') {
        strcat(buf, "NONE");
    }

    return buf;
}

void telemetry_dump_log(void)
{
    UINT32 i;
    UINT32 idx;
    UINT32 n;

    /* recompute "how many entries are valid" a second, slightly
     * different way than telemetry_log_event tracks it, using g_log_count
     * directly rather than checking wraparound explicitly. Both happen
     * to agree in this sim, but a refactor tool should notice the
     * duplicated bookkeeping. */
    n = (g_log_count > MAX_LOG_ENTRIES) ? MAX_LOG_ENTRIES : g_log_count;

    printf("[telemetry] --- log dump (%u entries) ---\n", n);
    for (i = 0; i < n; i++) {
        idx = (g_log_head + i) % MAX_LOG_ENTRIES;
        printf("  t=%6lums mode=%-4s a=%5dmv v=%5dmv flags=%s\n",
               (unsigned long)g_log[idx].t_ms,
               mode_to_string((PaceMode_t)g_log[idx].mode),
               (int)g_log[idx].a_mv,
               (int)g_log[idx].v_mv,
               evt_flag_desc(g_log[idx].flags));
    }
}
