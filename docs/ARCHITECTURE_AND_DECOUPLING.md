# Architecture & Decoupling Proposal — `pacemaker_sim`

**Orchestrator stage:** Architecture & Decoupling Agent
**Input artifact:** `tests/` characterization suite (535/535 assertions, 8/8 modules ≥85% coverage — ENGAGEMENT COMPLETE, confirmed)
**Status:** PROPOSAL FOR HUMAN REVIEW — no code has been generated against this design
**Phase-1 approver of record:** Percy Mitchell
**Deployment context:** Air-gapped / FedRAMP / NIST 800-53 (this run is a dry-run stand-in against the synthetic, non-medical `pacemaker_sim` benchmark fixture; the real air-gapped target and self-hosted LLM are a separate, later redirection)

---

## 0. Bottom Line Up Front

`pacemaker_sim`'s eight source files map to eight *accidental* boundaries — one per legacy file — not eight *intentional* ones. The characterization-testing phase surfaced six concrete defects, and every one of them is a symptom of the same root cause: **eleven global variables (`g_mode`, `g_state`, `g_tick_ms`, `g_log[]`/`g_log_head`/`g_log_count`, `g_batt_mv`, `g_a_sense_thresh_mv`, `g_v_sense_thresh_mv`, `g_avi_ms`, `g_url_ms`, plus the NVRAM parameter set) are readable and writable from any module, with no single owner accountable for their invariants.**

The recommended target state replaces the 8-file layout with **7 boundaries defined by data ownership and call-graph cohesion**, each exposing a typed interface instead of shared globals, plus **one new seam** (`SimulationOrchestrator`) that does not exist in the legacy code today. This is a decoupling exercise, not a rewrite of behavior: every target boundary is built to reproduce the exact behavior pinned by the 535-assertion characterization suite, including the four confirmed-dead branches — those stay dead by construction (unreachable in the type system) rather than by convention (an `if` no caller ever satisfies).

**Recommendation: target language Rust**, on the strength of one property that maps directly to this project's own Task #3 mandate ("design out shared global state... never let generated code recreate global mutable state under a new name"): Rust's ownership model makes a second, accidental owner of pacemaker state a *compile-time error*, not a code-review finding. Go is presented as the credible alternative and scored against Rust in Section 5.

All six findings resolve structurally under this design — not by patching the symptom, but because the boundary that caused each one is redrawn. Section 4 maps each finding to its specific resolution.

**This document is the Phase 2 human gate.** Per the Orchestrator's own governing framework, Architecture & Decoupling is the one stage where human judgment is designed to dominate machine output, and Code Generation is explicitly barred from starting without sign-off here. Nothing downstream proceeds until Percy Mitchell approves, amends, or rejects this proposal (Section 6).

---

## 1. Diagnostic: How We Got Here

Before proposing a target state, it's worth being precise about what's actually broken, because "decouple the modules" is not by itself an actionable instruction.

| Symptom class | Root cause | Evidence from characterization testing |
|---|---|---|
| Cross-module bugs that only show up under specific sequencing | No single module owns the event log; three modules (`pacer_core.c`, `arrhythmia.c`, `main.c`) read or write `g_log`/`g_log_head`/`g_log_count` directly | Finding #4 (arrhythmia log-index divergence) |
| Silent correctness gaps that pass every unit test in isolation | Cross-cutting invariants (e.g., `avi_ms < url_ms`) are assumed by one module (`pacer_core.c`) but enforced by nobody — `eeprom_validate()` checks unrelated bounds | Finding #5 (AVI/URL safety-fallback gap) |
| Dead code that survives refactors because no one can prove it's dead | Reachability depends on runtime values of global enums (`g_mode`, `g_state`) that any module can set to any value, defeating static analysis | Findings #1, #2 (mode_step, AOO/VOO fallback) |
| Documentation and behavior silently diverging over time | Comments describe intent; globals let implementation drift from that intent with no compiler or contract to catch it | Finding #3 (telemetry.c comment drift) |
| "It compiles, so it's probably fine" defects in numeric/enum handling | Untyped `int`/`BYTE` used where a closed, checkable type (a channel selector, a mode) would catch the error at the boundary | Finding #6 (battery.c channel bounds-check smell) |

**Diagnostic conclusion:** this is not eight modules with eight independent bugs. It's one architectural failure mode (uncontrolled shared mutable state) expressing itself six different ways. A decoupling plan that doesn't eliminate shared global state will reproduce all six defects under new file names — which is precisely what Task #3 of this stage's mandate warns against.

---

## 2. Proposed Service/Module Boundaries

### 2.1 Boundary diagram (text form)

```
                        ┌───────────────────────────┐
                        │   CLI / Driver             │
                        │   (from main.c, minus      │
                        │   the tick loop)           │
                        └─────────────┬─────────────┘
                                      │ configure(mode, duration, verbose)
                                      │ run() -> SimulationReport
                                      ▼
                        ┌───────────────────────────┐
                        │   SimulationOrchestrator   │  ◄── NEW SEAM (not in legacy code)
                        │   owns: tick loop,         │
                        │   wall-clock/tick_ms,      │
                        │   wiring between services  │
                        └──┬───────┬───────┬─────────┘
             tick(dt_ms)   │       │       │  tick(dt_ms)
        ┌────────────────────┘       │       └────────────────────┐
        ▼                            │                             ▼
┌───────────────────┐                │                   ┌───────────────────┐
│  PacingEngine      │◄───────────────┘                   │  BatteryService    │
│  (merged           │  read_sense()                      │  (from battery.c)  │
│  pacer_core.c +    │──────────────┐                     │  owns: g_batt_mv,  │
│  modes.c)          │              ▼                     │  channel-scoped    │
│  owns: g_state,     │   ┌───────────────────┐            │  impedance model   │
│  refractory timers, │   │  SensingService    │            └─────────┬─────────┘
│  mode table         │   │  (from sensing.c)  │                      │
└──────┬──────┬───────┘   │  owns: LCG sensing  │           tick_status()
       │      │           │  model, noise gen   │                      │
       │      │           └───────────────────┘                      │
       │      │ emit(event)                                          │
       │      ▼                                                      │
       │ ┌───────────────────┐        decision(context)              │
       │ │ TelemetryService   │◄────────────────────┐                 │
       │ │ (from telemetry.c) │                      │                 │
       │ │ SOLE owner of the  │            ┌───────────────────┐      │
       │ │ event log          │            │ ArrhythmiaService  │      │
       │ │ (g_log/head/count) │            │ (from arrhythmia.c)│      │
       │ └───────────────────┘            │ reads log via      │      │
       │                                   │ TelemetryService,   │      │
       │                                   │ returns a decision  │      │
       │                                   │ (no direct mutation)│      │
       │                                   └───────────────────┘      │
       │ load()/save(params) -> Result                                │
       ▼                                                              │
┌───────────────────┐                                                 │
│ PersistenceService │                                                 │
│ (from eeprom.c)     │◄────────────────────────────────────────────────┘
│ owns: NVRAM file    │        status snapshot flows back to
│ I/O, CRC, validation│        SimulationOrchestrator for
│ (incl. NEW avi/url  │        reporting — no module reaches
│ cross-field check)  │        into another's state directly
└───────────────────┘
```

### 2.2 Boundary rationale (why these seven-plus-one, not the legacy eight)

| Target boundary | Legacy source(s) | Why this grouping |
|---|---|---|
| **`PacingEngine`** | `pacer_core.c` + `modes.c` | These two files already share one conceptual state machine — `modes.c`'s per-mode capability table only makes sense in terms of `pacer_core.c`'s state transitions, and `pacer_core_handle_state()` already switches on mode-derived capability flags. Splitting them was a file-size accident, not a design decision. Merging them removes a cross-file dependency without merging anything that shouldn't be merged. |
| **`SensingService`** | `sensing.c` | Already cleanly isolated (99.02% coverage achieved with zero cross-module test coupling beyond the LCG seed). No change to its boundary — it's promoted as-is to a formal service with an explicit interface replacing its global threshold reads. |
| **`TelemetryService`** | `telemetry.c` | Elevated from "a module with its own checksum" to **sole owner and sole writer of the event log.** This is the direct structural fix for Finding #4. |
| **`ArrhythmiaService`** | `arrhythmia.c` | Changed from a module that reads *and mutates* shared state to one that reads a log snapshot (via `TelemetryService`'s read API) and *returns a decision* for the orchestrator to apply. This is the direct structural fix for the arrhythmia-detection side of Finding #4, and removes the class of "who's allowed to touch `g_ams_active`" bugs entirely. |
| **`BatteryService`** | `battery.c` | Unchanged scope, but its channel parameter becomes a closed enum type instead of an unchecked `int`/`BYTE` — the direct structural fix for Finding #6. |
| **`PersistenceService`** | `eeprom.c` | Scope narrowed: `load()` now returns either a valid parameter set or an explicit "not found/invalid" result — it no longer silently self-heals and re-saves inside the same call (see Section 4 trade-off note on this behavior change). Gains the missing `avi_ms < url_ms` cross-field check — the direct structural fix for Finding #5. |
| **`CLI/Driver`** | `main.c` (argument parsing, banner, summary printing) | Everything in `main.c` that is *not* the tick loop. Pure I/O boundary — no simulation logic. |
| **`SimulationOrchestrator`** *(new)* | *(extracted from `main.c`'s `while` loop + `pacer_core_tick()`'s current internal sequencing)* | This seam does not exist in the legacy code — today, "run one tick" is conflated across `main.c`'s loop body and `pacer_core_tick()`'s internal call sequence (sense → decide → pace → log → battery-tick → refractory-update, all in one function on one file's global state). Extracting it is what makes every other boundary's interface *testable in isolation*, and it is the natural home for the tick-sequencing contract every other service currently assumes implicitly. |

Total: **7 service boundaries + 1 CLI boundary**, replacing 8 files. Line count goes up slightly (interface boilerplate); coupling goes down sharply (measured by: number of globals each unit can reach — see Section 3).

---

## 3. Interface Contracts

Per Task #2, every boundary above gets an explicit contract. Given this is a single-process, single-host simulation with no network or process boundary today or anticipated in the air-gapped target, **typed internal API (language-level traits/interfaces) is recommended over REST/gRPC/message-schema** — introducing a wire protocol for in-process calls would be over-engineering with a latency and complexity cost and no corresponding benefit (flagged explicitly for human review in Section 5, since this is a judgment call, not a default).

Contracts below are given in Rust trait form (see Section 5 for the Rust-vs-Go recommendation); each has a direct one-page translation to a Go interface if that alternative is selected instead.

```rust
// -- SensingService ---------------------------------------------------
trait SensingService {
    /// Replaces direct reads of g_a_sense_thresh_mv / g_v_sense_thresh_mv
    /// and the module-internal LCG state.
    fn sample(&mut self, tick_ms: u32) -> SenseSample;
    fn is_noise(&self, sample: &SenseSample) -> bool;
}
struct SenseSample { a_mv: u32, v_mv: u32 }

// -- TelemetryService ---------------------------------------------------
trait TelemetryService {
    /// SOLE write path for the event log. No other boundary may append.
    fn record(&mut self, event: LogEvent);
    /// Read-only snapshot — replaces ArrhythmiaService's and main.c's
    /// direct g_log/g_log_head/g_log_count reads.
    fn recent(&self, n: usize) -> Vec<LogEvent>;
    fn dump(&self) -> String; // replaces telemetry_dump_log()
}
struct LogEvent { t_ms: u32, flags: EventFlags }

// -- ArrhythmiaService ---------------------------------------------------
trait ArrhythmiaService {
    /// Pure function of a log snapshot + current tick -> a decision.
    /// Never mutates g_ams_active or any other shared field directly;
    /// SimulationOrchestrator applies the returned decision.
    fn evaluate(&self, log: &[LogEvent], tick_ms: u32) -> ArrhythmiaDecision;
}
enum ArrhythmiaDecision { NoChange, EnterModeSwitch, ExitModeSwitch }

// -- BatteryService ---------------------------------------------------
#[derive(Copy, Clone)]
enum Channel { Atrial, Ventricular }   // replaces the unchecked int/BYTE channel param (Finding #6)

trait BatteryService {
    fn tick(&mut self, dt_ms: u32, pace_events: &[Channel]) -> BatteryStatus;
}
struct BatteryStatus { mv: u32, eri: bool, eol: bool, impedance: [ (Channel, u32); 2] }

// -- PersistenceService ---------------------------------------------------
trait PersistenceService {
    /// No more load-or-initialize conflation: caller decides what
    /// "not found or invalid" means. See Section 4/5 for the behavior
    /// change this implies vs. legacy eeprom_load().
    fn load(&self) -> Result<PacerParams, PersistenceError>;
    fn save(&mut self, params: &PacerParams) -> Result<(), PersistenceError>;
}
enum PersistenceError { NotFound, ShortRead, CrcMismatch, InvalidField(&'static str) }

struct PacerParams {
    mode: PaceMode,
    lrl_ms: u32, url_ms: u32, avi_ms: u32,
    pvarp_ms: u32, vrp_ms: u32, arp_ms: u32,
    pace_ampl_mv: u32, pace_width_ms: u32,
    a_sense_thresh_mv: u32, v_sense_thresh_mv: u32,
    rate_resp_enabled: bool,
}
impl PacerParams {
    /// NEW validation, closing Finding #5: eeprom_validate() today checks
    /// lrl_ms/url_ms/mode bounds but never this cross-field invariant,
    /// which pacer_core.c's ST_AVI_WAIT branch silently assumes.
    fn validate(&self) -> Result<(), PersistenceError> {
        if self.avi_ms >= self.url_ms {
            return Err(PersistenceError::InvalidField("avi_ms must be < url_ms"));
        }
        // ... existing lrl_ms/url_ms/mode bounds checks carried over unchanged
        Ok(())
    }
}

// -- PacingEngine (merged pacer_core.c + modes.c) ------------------------
trait PacingEngine {
    fn reset(&mut self, mode: PaceMode);
    /// One state-machine step. Takes sensing input and arrhythmia
    /// decision explicitly instead of reaching into globals for them.
    fn handle_state(&mut self, sense: &SenseSample, arrhythmia: ArrhythmiaDecision, tick_ms: u32) -> StepOutcome;
    fn state_name(&self) -> &'static str;
}
struct StepOutcome { events: Vec<LogEvent>, new_state: PacerState }

// -- SimulationOrchestrator (NEW) ---------------------------------------
trait SimulationOrchestrator {
    fn configure(&mut self, mode: PaceMode, duration_ms: u32, verbose: bool);
    /// Owns the sequencing contract that pacer_core_tick() currently
    /// hides inside one function body: sense -> arrhythmia-evaluate ->
    /// pacing-step -> telemetry-record -> battery-tick, once per SIM_STEP_MS.
    fn run(&mut self) -> SimulationReport;
}
struct SimulationReport {
    final_tick_ms: u32,
    totals: EventTotals,
    battery: BatteryStatus,
    final_state: &'static str,
}

// -- CLI/Driver -----------------------------------------------------------
trait Cli {
    fn parse_args(&self, argv: &[String]) -> Result<CliConfig, CliError>;
    fn print_usage(&self);
    fn print_report(&self, report: &SimulationReport, verbose: bool);
}
```

---

## 4. Findings → Architectural Resolution Map

| # | Finding (from characterization testing) | Legacy root cause | Structural resolution in target design |
|---|---|---|---|
| 1 | `modes.c`: `mode_step()` is dead code (zero call sites, confirmed via `grep`) | Function retained after its caller was removed; nothing enforces "every exported function has a caller" | Not carried into `PacingEngine`'s trait surface at all — a trait method with no caller in the new design simply doesn't exist to compile. Human reviewers should confirm no external/future caller was planned before this is finalized as a deletion (see Section 5 risk note). |
| 2 | `pacer_core.c`: second dead-fallback branch — confirmed live code, but unreachable for any of the 12 real pacing modes (only reachable via an out-of-range mode value) | `g_mode` is a plain global any code can set to an invalid value; the switch's `default`/fallback case exists to handle that | `PaceMode` becomes a closed enum validated once at the `PersistenceService`/`Cli` boundary (`PacerParams::validate()`); nothing downstream of that boundary can hold an invalid mode value, so `PacingEngine::handle_state()` has no fallback case to write in the first place — unreachable by construction, not by convention. |
| 3 | `telemetry.c`: comment describes intent that the implementation doesn't match (comment/code drift) | Comments are the only place intent is recorded; nothing checks implementation against them | `TelemetryService`'s trait signature + `LogEvent` type *is* the contract; intent lives in the type signature and doc-comment on the trait, one artifact instead of two, reviewed together at every call site by the compiler (borrow/ownership checks on `&[LogEvent]` snapshots) rather than left to informal comment upkeep. |
| 4 | `arrhythmia.c`: log-index divergence — cross-module bug from multiple modules independently indexing into `g_log`/`g_log_head`/`g_log_count` | Three modules (`pacer_core.c`, `arrhythmia.c`, `main.c`) each do their own head/count arithmetic against the same shared ring buffer | `TelemetryService` becomes the **sole** owner and sole indexer of the log; every other boundary receives only an immutable `Vec<LogEvent>` snapshot via `recent()`. There is no second indexing implementation to diverge from the first because there is no second implementation. |
| 5 | `pacer_core.c` / `eeprom.c`: `ST_AVI_WAIT` URL-safety-fallback branch depends on `avi_ms < url_ms`, an invariant `eeprom_validate()` never checks | Invariant lives only in `pacer_core.c`'s logic, assumed but never asserted at the point where the values are actually set (`eeprom.c`) | `PacerParams::validate()` in `PersistenceService` checks `avi_ms < url_ms` explicitly (new check, shown in Section 3) — enforced at the one place values enter the system, not assumed downstream. |
| 6 | `battery.c`: channel parameter is an unchecked `int`/`BYTE`, no bounds checking on the channel selector | No type distinguishes "a valid channel" from "any integer" | `Channel` becomes a 2-variant enum (`Atrial`/`Ventricular`) in `BatteryService`'s contract — an invalid channel is a compile error, not a runtime bounds-check gap. **Flagged for explicit confirmation** below: `test_battery.c`'s existing suite pins the *current* no-bounds-checking behavior as-is; changing this is a behavior change relative to the characterization baseline, not a pure refactor, and needs sign-off before Code Generation treats it as safe (see Section 5). |

---

## 5. Concurrency & State-Ownership Design

**Recommendation: single-threaded, ownership-based state management — no actor model, no message-passing runtime, no thread pool.**

**Rationale:** `pacemaker_sim`'s entire workload is one deterministic tick loop, `SIM_STEP_MS` at a time, with no concurrent inputs, no network I/O in the hot path, and no requirement (stated by the user: "None exist") for a performance SLA that would justify parallelism. Introducing actors, channels, or threads here would add synchronization complexity — and a whole new class of nondeterminism bugs — to a system whose defining testability property today (and the reason 535/535 characterization assertions could be written deterministically at all) is that it's single-threaded and single-threaded only. Task #3 asks that shared global state be designed out; it does not ask that concurrency be added where none exists in the problem. **This is presented as a recommendation, not a foregone conclusion — flagged explicitly for reviewer override if the real air-gapped target's future requirements (e.g., a hardware sensing thread separate from the simulation/decision loop) differ from this synthetic fixture's.**

Per-boundary ownership under this model:

| Boundary | Owns exclusively | Never reachable from |
|---|---|---|
| `TelemetryService` | The event log (backing array, head, count) | Any other boundary — all others get read-only snapshots |
| `PacingEngine` | `PacerState`, refractory timers | `BatteryService`, `SensingService`, `ArrhythmiaService` |
| `BatteryService` | Battery voltage, per-channel impedance model | Everything except `SimulationOrchestrator`, which calls `tick()` once per step |
| `PersistenceService` | The NVRAM file handle, CRC computation | Everything except `SimulationOrchestrator`, at load (startup) and save (shutdown) only |
| `SimulationOrchestrator` | The tick counter (`tick_ms` equivalent), the wiring between all of the above | Nobody — it is the composition root; nothing owns it |

Because Rust's borrow checker enforces "one owner, explicit borrows" at compile time, a future contributor adding a ninth module cannot silently reintroduce a shared global — the same mistake the legacy codebase made eight times cannot compile a ninth time. This is the concrete, verifiable form of Task #3's mandate, not just a stated intention.

---

## 6. Trade-Off Notes for Human Review

Each row below is a judgment call this document is making on the human reviewer's behalf, surfaced explicitly rather than buried in code. **None of these should be read as decided — they are the specific points where Percy Mitchell's sign-off is the actual gate, not a formality.**

| Decision | Option A (recommended) | Option B | Why A is recommended | What could change the call |
|---|---|---|---|---|
| **Target language** | **Rust** | Go | Ownership model directly enforces Task #3 (no shared global state, compiler-checked); strong closed-enum typing eliminates the exact class of bug behind Findings #2 and #6; no GC pause behavior to reason about if this ever needs real-time guarantees later. Cost: steeper learning curve for a team without Rust experience, longer compile times. | If the team maintaining this long-term has strong Go experience and none in Rust, Go's interfaces + a documented "no package-level `var`" lint rule can achieve most of the same discipline with a shallower learning curve, at the cost of the compiler no longer being the enforcement mechanism. |
| **`modes.c` + `pacer_core.c` merge into `PacingEngine`** | Merge | Keep separate | They already share one conceptual state machine; separate files added a cross-file dependency with no isolation benefit (confirmed: `modes.c`'s capability table is meaningless without `pacer_core.c`'s state enum). | If the real air-gapped target's mode table is swapped independently of the state machine (e.g., a config-driven mode set), keeping them separate preserves that swap point. |
| **New `SimulationOrchestrator` seam** | Add it | Keep tick-sequencing implicit inside `PacingEngine` (closer to legacy `pacer_core_tick()`) | Every other boundary's contract in Section 3 assumes an external caller owns sequencing; without this seam, `PacingEngine` re-absorbs cross-cutting responsibility and the decoupling regresses toward the legacy shape. | None identified — this seam is low-risk and high-leverage; flagged here for completeness, not because it's contentious. |
| **`PersistenceService::load()` behavior change** | Return `Result` (no silent self-heal-and-resave) | Preserve legacy "load-or-initialize" behavior exactly | Legacy `eeprom_load()` conflates "read" with "read, and if that fails, write defaults back out" — a side effect inside what looks like a query. Splitting these is a correctness improvement, not just a style change. | **This is a genuine behavior change**, not a pure refactor — any caller relying on `eeprom_load()`'s auto-heal side effect (e.g., a corrupt-NVRAM recovery path) needs that behavior re-added explicitly at the `SimulationOrchestrator` or `Cli` layer. Needs explicit confirmation this is acceptable before Code Generation. |
| **Findings #1/#2 (dead branches) become type-system-unreachable** | Remove/make unreachable | Keep as defensive dead code (current state) | Task #3's "no shared global state" and the general modernization goal both favor eliminating states that can't be reached rather than defending against them forever. | If the real air-gapped target has a requirement for defensive-depth coding standards (common in safety-critical certification regimes) that mandate explicit fault-handling code even for "impossible" states, Option B (keep as an explicit, tested `Result::Err` arm rather than deleting) should be adopted instead of outright deletion. **Recommend treating this as a certification-context question, not a pure engineering one.** |
| **Finding #6 (`Channel` enum, bounds-checked)** | Adopt strict enum | Keep unchecked `int`/`BYTE` channel param | Closes a real correctness gap; low implementation cost. | **`test_battery.c`'s existing characterization suite explicitly pins "no bounds checking" as the CURRENT behavior.** Adopting the strict enum is a deliberate behavior change relative to that baseline, not a refactor-preserving-behavior move — flagged per this engagement's own rule that behavior changes must be called out, never silently introduced under refactoring cover. Needs explicit sign-off that this is a wanted correctness fix, not just a wanted decoupling. |
| **Interface contract style (Section 3)** | Typed internal API (in-process traits) | REST/gRPC/async message schema | No process or network boundary exists or is anticipated for this workload; a wire protocol here adds serialization cost and complexity with no corresponding benefit. | If the target architecture for the real air-gapped system separates this into multiple processes or hosts (e.g., sensing hardware on a separate real-time core from decision logic), the `SensingService` boundary specifically is the one most likely to need to cross a real IPC/wire boundary — its contract in Section 3 was deliberately kept minimal and serialization-friendly (`SenseSample` is a plain data struct) for exactly this reason. |

---

## 7. What This Document Is Not

Consistent with this stage's own governing rule ("this is the one stage where human judgment dominates — output is a proposal for human review"):

- No code has been generated against this design.
- No original `pacemaker_sim` source file has been modified, read-modified, or touched in producing this document (it is delivered as a new file, `docs/ARCHITECTURE_AND_DECOUPLING.md`, alongside the existing `tests/` directory — the same zero-source-modification discipline held throughout the testing phase).
- The Rust-vs-Go call, the `PersistenceService` behavior change, and the `Channel`-enum behavior change are explicitly flagged as decisions requiring sign-off, not conclusions already acted on.
- This remains a synthetic, non-medical, non-hardware benchmark fixture per `pacemaker_sim`'s own README; nothing in this document should be read as, or represented as, an architecture for a real medical device.

---

## 8. Requested Action

**Requesting Percy Mitchell's Phase 2 human-gate sign-off** on one of the following before Code Generation work begins:

1. **Approve as proposed** — proceed to Code Generation Agent using the boundaries, contracts, concurrency model, and Rust recommendation above, with the trade-off calls in Section 6 resolved as recommended.
2. **Approve with modifications** — specify which Section 6 trade-off(s) should resolve differently (most likely candidates: language choice, or the `PersistenceService`/`Channel`-enum behavior changes) before Code Generation begins.
3. **Hold** — return with further questions or request a deeper look at a specific boundary before any Code Generation work is authorized.

No downstream pipeline stage will proceed on this engagement without one of the above.
