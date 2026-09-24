#!/bin/sh
# ---------------------------------------------------------------------------
# test_main.sh
#
# CHARACTERIZATION TEST SUITE for main.c - the CLI driver / simulation loop.
#
# main.c defines its own global main(), so unlike every other module in
# this suite it CANNOT be linked into a test binary alongside a
# test-driver main() (that's a "multiple definition of main" link error).
# So this suite is black-box: it builds a separate, coverage-instrumented
# copy of the REAL, unmodified src/*.c (all 8 files, including main.c -
# the exact same set the top-level Makefile builds bin/pacemaker_sim
# from) into this tests/ directory, then drives it via repeated
# subprocess invocations, asserting on stdout, exit code, and the
# persisted pacer_nvram.bin side effect - never on internal state, since
# there is none to reach from outside a subprocess.
#
# Every expected string/behavior below was captured by actually running
# the real, unmodified binary first and reading its output (this
# project's "probe first, then pin" rule applies to shell/CLI testing
# exactly as it does to the C suites) - not derived by reading main.c's
# source and assuming what it would print.
#
# gcov coverage data accumulates ADDITIVELY across sequential
# invocations of the same instrumented binary run in the same directory
# (the runtime merges each run's counts into the existing .gcda), so
# every invocation below (including ones just testing CLI parsing
# quirks) also contributes to main.c's real, tool-measured coverage
# number - see the "coverage" target in tests.mk.
#
# Uses only POSIX sh + standard tools (no bashisms), matching this
# project's "no new dependencies for testing" posture.
# ---------------------------------------------------------------------------

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

BIN=./pacemaker_sim_cov
PASS=0
FAIL=0

# check_contains DESC NEEDLE < FILE   (asserts NEEDLE appears in FILE's content, via stdin)
check_contains() {
    desc="$1"
    needle="$2"
    if grep -qF "$needle" "$3"; then
        PASS=$((PASS + 1))
        printf '  [PASS] %s\n' "$desc"
    else
        FAIL=$((FAIL + 1))
        printf '  [FAIL] %s  (expected to find: %s)\n' "$desc" "$needle"
    fi
}

check_not_contains() {
    desc="$1"
    needle="$2"
    if grep -qF "$needle" "$3"; then
        FAIL=$((FAIL + 1))
        printf '  [FAIL] %s  (should NOT have found: %s)\n' "$desc" "$needle"
    else
        PASS=$((PASS + 1))
        printf '  [PASS] %s\n' "$desc"
    fi
}

check_eq() {
    desc="$1"
    expected="$2"
    actual="$3"
    if [ "$expected" = "$actual" ]; then
        PASS=$((PASS + 1))
        printf '  [PASS] %s\n' "$desc"
    else
        FAIL=$((FAIL + 1))
        printf '  [FAIL] %s  (expected \"%s\", got \"%s\")\n' "$desc" "$expected" "$actual"
    fi
}

echo "=== main.c characterization test suite (black-box, subprocess) ==="
echo "(pins CURRENT CLI behavior of the real, unmodified, linked main.c"
echo " as a regression baseline ahead of AI-agent-driven refactoring)"
echo ""

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not found or not executable - run 'make -f tests.mk $BIN' first" >&2
    exit 1
fi

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

# -- --help / -h -------------------------------------------------------
echo "-- --help / -h: usage output, exits before any simulation setup --"

"$BIN" --help > "$OUT" 2>&1
check_eq   "--help exits 0" "0" "$?"
check_contains "--help prints a usage line" "usage:" "$OUT"
check_contains "--help lists all 12 mode names" "AOO VOO AAI VVI AAT VVT AAIR VVIR VDD DDI DDD DDDR" "$OUT"
check_not_contains "--help returns before ever printing the sim-start banner" "starting simulation" "$OUT"

"$BIN" -h > "$OUT" 2>&1
check_eq   "-h exits 0" "0" "$?"
check_contains "-h produces the identical usage output as --help" "usage:" "$OUT"

# -- default args (no mode, no duration) --------------------------------
echo "-- default args: no existing NVRAM file -> DDD, 10000ms --"

rm -f pacer_nvram.bin
"$BIN" > "$OUT" 2>&1
check_eq "clean-slate default run exits 0" "0" "$?"
check_contains "clean-slate default run: mode=DDD, duration=10000ms (eeprom_load()'s use_defaults path)" \
    "[main] starting simulation: mode=DDD duration=10000ms verbose=0" "$OUT"
check_contains "run completes (reaches the end-of-sim summary)" "[main] simulation complete" "$OUT"

# -- eeprom persistence across runs -------------------------------------
echo "-- eeprom persistence across runs (the 'load then maybe re-override' ordering main.c's own comment calls out) --"

rm -f pacer_nvram.bin
"$BIN" AAI 200 > "$OUT" 2>&1
check_contains "run 1: explicit 'AAI 200' -> mode=AAI duration=200ms" \
    "[main] starting simulation: mode=AAI duration=200ms verbose=0" "$OUT"

"$BIN" > "$OUT" 2>&1
check_contains "run 2: NO mode given -> picks up run 1's persisted AAI via eeprom_load() (duration back to the 10000ms default, since duration isn't persisted)" \
    "[main] starting simulation: mode=AAI duration=10000ms verbose=0" "$OUT"

"$BIN" VVI 200 > "$OUT" 2>&1
check_contains "run 3: explicit CLI mode 'VVI' OVERRIDES the persisted AAI (the re-apply-after-eeprom_load() branch)" \
    "[main] starting simulation: mode=VVI duration=200ms verbose=0" "$OUT"

# -- unknown mode name ----------------------------------------------------
echo "-- unknown mode name: silently falls back to VVI via mode_from_string() --"

"$BIN" NOTAREALMODE 200 > "$OUT" 2>&1
check_contains "an unrecognized mode string falls back to VVI, with NO error message distinguishing it from a real request" \
    "[main] starting simulation: mode=VVI duration=200ms verbose=0" "$OUT"
check_not_contains "...and prints no error/warning about the bad mode name at all" "unknown mode" "$OUT"

# -- zero / invalid duration ----------------------------------------------
echo "-- zero/invalid duration: falls back to DEFAULT_DURATION_MS (10000) --"

"$BIN" DDD 0 > "$OUT" 2>&1
check_contains "explicit duration '0' falls back to the 10000ms default (atol()=0 triggers the fallback)" \
    "[main] starting simulation: mode=DDD duration=10000ms verbose=0" "$OUT"

"$BIN" DDD abc > "$OUT" 2>&1
check_contains "a non-numeric duration string ('abc') ALSO falls back to 10000ms (atol(\"abc\")==0, same branch, no parse error reported)" \
    "[main] starting simulation: mode=DDD duration=10000ms verbose=0" "$OUT"

# -- extra unrecognized trailing args --------------------------------------
echo "-- extra trailing args: silently ignored, no crash, no error --"

"$BIN" DDD 200 foo bar baz > "$OUT" 2>&1
check_eq "extra trailing args ('foo bar baz') -> still exits 0" "0" "$?"
check_contains "...and the run completes normally, using mode=DDD duration=200ms as if the extras weren't there" \
    "[main] starting simulation: mode=DDD duration=200ms verbose=0" "$OUT"
check_contains "...and reaches the summary" "[main] simulation complete at t=200ms" "$OUT"

# -- -v verbose flag --------------------------------------------------------
echo "-- -v flag: adds the end-of-run telemetry hex/log dump; periodic status lines print either way --"

# NOTE: STATUS_PRINT_EVERY_MS is 1000ms, so this check needs a run of at
# least that long - the 200ms runs used elsewhere in this suite never
# reach the first periodic status line at all (that's a real, separately
# worth-noting characterization: a sub-1000ms simulation prints NO
# periodic status lines whatsoever, only the start banner and end
# summary), so this specific check uses its own longer duration.
"$BIN" DDD 200 > "$OUT" 2>&1
check_not_contains "non-verbose run: NO telemetry log dump at the end" "[telemetry]" "$OUT"
check_not_contains "a sub-1000ms run (200ms here) never reaches STATUS_PRINT_EVERY_MS, so it prints NO periodic status line at all" "state=" "$OUT"

"$BIN" DDD 1500 > "$OUT" 2>&1
check_contains "a run of at least 1000ms DOES print a periodic status line, with -v NOT required (print_status_line() is unconditional, unlike the end-of-run telemetry dump)" "state=" "$OUT"

"$BIN" DDD 200 -v > "$OUT" 2>&1
check_contains "verbose run (-v): telemetry_dump_log() output IS present at the end" "[telemetry] --- log dump" "$OUT"
check_contains "verbose run: starting banner reflects verbose=1" \
    "[main] starting simulation: mode=DDD duration=200ms verbose=1" "$OUT"

# -v can appear before the positional args too (the parser scans all of
# argv in order and doesn't care about position for the flag itself)
"$BIN" -v DDD 200 > "$OUT" 2>&1
check_contains "-v works when placed BEFORE the positional mode/duration args too (order-independent flag scan)" \
    "[main] starting simulation: mode=DDD duration=200ms verbose=1" "$OUT"

echo ""
echo "=== RESULTS: $PASS passed, $FAIL failed ==="
echo ""
echo "KNOWN, CONFIRMED GAP (not covered, and why): main.c's in-loop"
echo "\"if (g_state == ST_FAULT) { ...; break; }\" early-exit branch is"
echo "never reached by any of the invocations above, or by any reachable"
echo "CLI invocation at all. Per pacer_core.c's own comment, ST_FAULT is"
echo "only ever set by pacer_core_handle_state()'s switch default case,"
echo "which requires g_state to ALREADY hold an invalid enum value - not"
echo "something any documented CLI input, mode name, or duration can"
echo "cause. Confirmed structurally unreachable via the public CLI, the"
echo "same class of gap already documented for eeprom.c's I/O-failure"
echo "branches, telemetry.c's NONE fallback, and sensing.c's lcg_range()"
echo "span<=0 branch."

rm -f pacer_nvram.bin

if [ "$FAIL" -eq 0 ]; then
    exit 0
else
    exit 1
fi
