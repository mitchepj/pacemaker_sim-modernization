# pacemaker_sim

## WHAT THIS IS

`pacemaker_sim` is a **synthetic, entirely fictional** cardiac pacemaker
firmware simulator. It exists for exactly one purpose: to give
code-refactoring agents (and humans) a realistic, compilable, *messy*
legacy C codebase to practice on.

**This is not a real or certified medical device.** It does not drive any
hardware, it is not validated against IEC 60601-1, ISO 14708-2, or any
other medical device standard, and it must never be used, adapted, or
referenced for the design, control, or simulation of an actual implantable
device or in any context involving real patient care. It is a desktop
program that prints simulated telemetry to your terminal.

The domain (pacing modes, timing cycles, sensing, mode switching, battery
longevity) was chosen because real embedded medical firmware has a
well-known reputation for exactly the kind of "legacy smell" that makes a
good refactoring benchmark: global mutable state, deeply nested control
flow, magic numbers, duplicated per-mode logic, inconsistent naming, and
functions that do far too much. All of that has been deliberately
reproduced here, on top of a functionally coherent simulation, so that a
refactoring pass has real, exercisable behavior to preserve.

## BUILD & RUN

```
make
./bin/pacemaker_sim            # runs a default DDD-mode simulation
./bin/pacemaker_sim VVI 20000  # mode name + simulated milliseconds
```

Supported mode names on the command line: `AOO VOO AAI VVI AAT VVT AAIR
VVIR VDD DDI DDD DDDR`. The program seeds a synthetic intracardiac signal,
runs the pacing/sensing state machine tick-by-tick, and logs paced/sensed
events, mode switches, and battery status to stdout. There is no
networking, no hardware access, and no persistence outside a local file
named `pacer_nvram.bin` (a stand-in for EEPROM, also fake).

## LAYOUT

```
include/pacer.h      shared legacy-style types, macros, globals, prototypes
src/main.c           simulation driver / CLI
src/pacer_core.c      core timing-cycle state machine (the "brain")
src/sensing.c        synthetic electrogram + sense-amplifier logic
src/modes.c          per-mode (AOO/VVI/DDD/etc.) pacing logic
src/telemetry.c      fake RF telemetry packets + ring-buffer logger
src/battery.c        battery voltage decay, impedance, ERI/EOL logic
src/eeprom.c         fake non-volatile parameter storage + CRC16
src/arrhythmia.c     fake tachycardia detection + automatic mode switching
```

Total: ~2,900 lines of intentionally legacy-style C across 9 files.

## SEEDED "LEGACY SMELLS" (for refactoring benchmarks)

This codebase intentionally contains, spread across the files above:

- A large block of shared global mutable state (`g_*` variables in
  `pacer.h` / `pacer_core.c`) instead of passed-in context objects.
- Long functions (several 150-300+ line functions with deep nesting).
- Magic numbers instead of named constants (timing values in raw
  milliseconds, thresholds in raw millivolts, etc.) mixed in with the
  `#define`s that do exist, inconsistently.
- Copy-pasted, near-duplicate logic across the per-mode branches in
  `modes.c` and `pacer_core.c` rather than shared helpers.
- Mixed naming conventions (`snake_case`, `camelCase`, Hungarian-ish
  prefixes, single-letter loop/temp variables) across and even within
  files.
- `goto`-based control flow for error/edge-case handling.
- Minimal / inconsistent comments, several stale or misleading ones.
- A hand-rolled CRC16 and ring buffer instead of using a shared utility.
- Return codes that mix `int` sentinel values, `bool`, and raw `0/-1`
  conventions across modules.

None of this is a critique of how real medical firmware is written today
(modern shops use MISRA-C, static analysis, and heavy test coverage) —
it's a deliberately constructed worst case for benchmarking automated
refactoring.
