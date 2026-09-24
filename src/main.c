/* ------------------------------------------------------------------------
 * main.c
 *
 * FAKE pacemaker firmware simulator - CLI driver / simulation loop.
 * NOT REAL. See README.md at project root for the full disclaimer.
 *
 * Usage:
 *   pacemaker_sim [MODE] [DURATION_MS] [-v]
 *
 * MODE defaults to DDD, DURATION_MS defaults to 10000.
 *
 * Legacy smells seeded here on purpose:
 *   - hand-rolled argv parsing with positional-then-flag mixing instead
 *     of getopt
 *   - a long main() that does argument parsing, setup, the simulation
 *     loop, AND summary reporting all inline
 *   - status printing duplicated between the periodic in-loop printout
 *     and the end-of-run summary instead of one shared reporter
 * ------------------------------------------------------------------------
 */

#include "pacer.h"

#define SIM_STEP_MS         10
#define DEFAULT_DURATION_MS 10000
#define STATUS_PRINT_EVERY_MS 1000

static void print_banner(void)
{
    printf("======================================================\n");
    printf(" pacemaker_sim - FAKE / SYNTHETIC pacemaker simulator\n");
    printf(" fw %d.%d.%d (build %s) - NOT A REAL MEDICAL DEVICE\n",
           FW_MAJOR, FW_MINOR, FW_PATCH, FW_BUILD_DATE);
    printf(" see README.md - this is a legacy-code refactoring\n");
    printf(" benchmark fixture only.\n");
    printf("======================================================\n");
}

static void print_usage(const char *prog)
{
    int i;

    printf("usage: %s [MODE] [DURATION_MS] [-v]\n", prog);
    printf("  MODE:          one of ");
    for (i = 0; i < MODE_COUNT; i++) {
        printf("%s ", mode_to_string((PaceMode_t)i));
    }
    printf("\n");
    printf("  DURATION_MS:   how many simulated milliseconds to run (default %d)\n",
           DEFAULT_DURATION_MS);
    printf("  -v:            verbose (per-event + telemetry hex dump logging)\n");
}

/* prints a periodic one-line status snapshot - NOTE this recomputes the
 * implied heart rate using a slightly different rounding than
 * arrhythmia.c's internal check (integer division on a different base
 * value), so the two numbers can disagree by a beat or two. Left as-is
 * on purpose; see arrhythmia.c header comment for why. */
static void print_status_line(void)
{
    UINT32 v_interval;
    UINT32 approx_bpm;

    v_interval = g_tick_ms - g_last_v_evt_ms;
    if (v_interval == 0) {
        v_interval = 1;
    }
    approx_bpm = 60000UL / (v_interval + 1);

    printf("t=%6lums mode=%-4s state=%-13s batt=%4lumV a_imp=%4luohm v_imp=%4luohm "
           "eri=%d eol=%d ams=%d ~vrate=%lubpm\n",
           (unsigned long)g_tick_ms,
           mode_to_string(g_mode),
           pacer_state_name(g_state),
           (unsigned long)g_batt_mv,
           (unsigned long)g_lead_a_impedance,
           (unsigned long)g_lead_v_impedance,
           g_eri_flag ? 1 : 0,
           g_eol_flag ? 1 : 0,
           g_ams_active ? 1 : 0,
           (unsigned long)approx_bpm);
}

/* Very old-school manual argv scan: positional args are consumed in
 * order (mode, then duration) and "-v" can appear anywhere after that.
 * No error messages distinguish "bad mode name" from "typo" - both
 * just silently fall back to VVI via mode_from_string(). */
int main(int argc, char *argv[])
{
    int i;
    int got_mode;
    int got_duration;
    PaceMode_t requested_mode;
    UINT32 duration_ms;
    UINT32 elapsed_since_status;
    UINT32 total_a_paces;
    UINT32 total_v_paces;
    UINT32 total_a_senses;
    UINT32 total_v_senses;

    got_mode = 0;
    got_duration = 0;
    requested_mode = MODE_DDD;
    duration_ms = DEFAULT_DURATION_MS;
    g_verbose = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            if (!got_mode) {
                g_mode = mode_from_string(argv[i]);
                requested_mode = g_mode;
                got_mode = 1;
            } else if (!got_duration) {
                duration_ms = (UINT32)atol(argv[i]);
                if (duration_ms == 0) {
                    duration_ms = DEFAULT_DURATION_MS;
                }
                got_duration = 1;
            } else {
                /* extra unrecognized args are silently ignored -
                 * another authentic-feeling legacy CLI wart */
            }
        }
    }

    print_banner();

    pacer_core_init();
    eeprom_load();

    /* eeprom_load() may have overwritten g_mode with whatever was
     * persisted from a previous run; if the user explicitly asked for
     * a mode on the command line, re-apply it now. This "load then
     * maybe re-override" ordering is a classic source of subtle legacy
     * bugs (imagine a third code path that also touches g_mode between
     * these two calls someday) - kept intentionally. */
    if (got_mode) {
        g_mode = requested_mode;
        mode_apply_defaults(g_mode);
    }

    printf("[main] starting simulation: mode=%s duration=%lums verbose=%d\n",
           mode_to_string(g_mode), (unsigned long)duration_ms, g_verbose);

    total_a_paces = 0;
    total_v_paces = 0;
    total_a_senses = 0;
    total_v_senses = 0;
    elapsed_since_status = 0;

    while (g_tick_ms < duration_ms) {

        pacer_core_tick(SIM_STEP_MS);

        elapsed_since_status += SIM_STEP_MS;
        if (elapsed_since_status >= STATUS_PRINT_EVERY_MS) {
            print_status_line();
            elapsed_since_status = 0;
        }

        if (g_state == ST_FAULT) {
            printf("[main] FAULT state reached, aborting simulation early at t=%lums\n",
                   (unsigned long)g_tick_ms);
            break;
        }

        /* crude event tallying by re-scanning the most recent log
         * entry each tick instead of accumulating counters where the
         * events are actually generated (pacer_core.c) - duplicated
         * bookkeeping, on purpose */
        if (g_log_count > 0) {
            UINT32 last_idx;
            BYTE flags;

            last_idx = (g_log_head + g_log_count - 1) % MAX_LOG_ENTRIES;
            flags = g_log[last_idx].flags;

            if (g_log[last_idx].t_ms == g_tick_ms) {
                if (flags & EVT_A_PACE) {
                    total_a_paces++;
                }
                if (flags & EVT_V_PACE) {
                    total_v_paces++;
                }
                if (flags & EVT_A_SENSE) {
                    total_a_senses++;
                }
                if (flags & EVT_V_SENSE) {
                    total_v_senses++;
                }
            }
        }
    }

    eeprom_save();

    printf("------------------------------------------------------\n");
    printf("[main] simulation complete at t=%lums\n", (unsigned long)g_tick_ms);
    printf("[main] totals: A_PACE=%lu V_PACE=%lu A_SENSE=%lu V_SENSE=%lu\n",
           (unsigned long)total_a_paces, (unsigned long)total_v_paces,
           (unsigned long)total_a_senses, (unsigned long)total_v_senses);
    printf("[main] final battery=%lumV eri=%d eol=%d ams_active=%d\n",
           (unsigned long)g_batt_mv, g_eri_flag ? 1 : 0, g_eol_flag ? 1 : 0,
           g_ams_active ? 1 : 0);

    if (g_verbose) {
        telemetry_dump_log();
    }

    return 0;
}
