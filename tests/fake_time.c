/* ------------------------------------------------------------------------
 * fake_time.c
 *
 * THE DETERMINISM FIX, implemented as a test-harness-only technique that
 * requires ZERO changes to sensing.c, battery.c, or any other original
 * pacemaker_sim source file.
 *
 * Background: sensing_init() and battery_init() each seed a hand-rolled
 * LCG PRNG from time(NULL), making sensing.c and battery.c's output
 * non-deterministic run-to-run (confirmed during Discovery: running the
 * compiled binary twice with identical arguments produced diverging
 * output). That non-determinism makes naive "capture current output as
 * the golden baseline" characterization testing unreliable.
 *
 * This file provides its own definition of time() with the standard
 * libc signature (time_t time(time_t *tloc)). When linked directly
 * against object files that call time() - as every test binary in this
 * suite does, compiling straight from the .c sources rather than
 * against a prebuilt shared libc.so - the linker resolves the time()
 * symbol from THIS object first and never pulls the libc archive's
 * version in for that symbol. Every call to time() anywhere in the
 * linked binary (including inside sensing.c and battery.c) is
 * transparently redirected to test_set_fake_time()'s controlled value.
 *
 * This is a test-only, purely additive interposition trick - not a
 * source patch, not a #define, not a build flag threaded through the
 * original files. sensing.c and battery.c are completely unaware of it.
 * It is only linked into test binaries (see tests.mk); it must never be
 * linked into the real bin/pacemaker_sim build.
 *
 * Verified empirically before use here: two runs seeded identically via
 * test_set_fake_time() produce byte-identical sensing.c/battery.c
 * output; different seeds produce different output. See
 * test_sensing.c / test_battery.c for the golden-sequence tests this
 * unlocks.
 * ------------------------------------------------------------------------
 */

#include <time.h>

static time_t s_fake_time_value = 1;

time_t time(time_t *tloc)
{
    if (tloc != NULL) {
        *tloc = s_fake_time_value;
    }
    return s_fake_time_value;
}

void test_set_fake_time(time_t v)
{
    s_fake_time_value = v;
}
