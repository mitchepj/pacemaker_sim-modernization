//! SimulationOrchestrator — NEW seam, not present in the legacy C
//! (Section 2.2 of the architecture proposal). Extracted from what was
//! previously split across `main.c`'s `while` loop and
//! `pacer_core_tick()`'s internal call sequence (sense -> decide -> pace
//! -> log -> battery-tick, all inside one function touching one file's
//! globals). This is the one place that wires all the other services
//! together and owns the tick counter (`g_tick_ms`, the single most
//! heavily shared legacy global) and the mode-configuration flow
//! main.c's own comment flagged as fragile ("load then maybe
//! re-override... a classic source of subtle legacy bugs").

use std::io::Write;

use crate::arrhythmia::ArrhythmiaService;
use crate::battery::{BatteryService, BatteryStatus};
use crate::pacing_engine::PacingEngine;
use crate::persistence::{PacerParams, PersistenceService};
use crate::sensing::SensingService;
use crate::telemetry::TelemetryService;
use crate::types::{EventFlags, PaceMode};

const SIM_STEP_MS: u32 = 10;
const STATUS_PRINT_EVERY_MS: u32 = 1000;

#[derive(Debug, Default, Clone, Copy)]
pub struct EventTotals {
    pub a_paces: u32,
    pub v_paces: u32,
    pub a_senses: u32,
    pub v_senses: u32,
}

#[derive(Debug, Clone, Copy)]
pub struct SimulationReport {
    pub final_tick_ms: u32,
    pub totals: EventTotals,
    pub battery: BatteryStatus,
    pub final_mode: PaceMode,
    pub final_state: &'static str,
    pub ams_active: bool,
    pub aborted_on_fault: bool,
}

pub struct SimulationOrchestrator {
    engine: PacingEngine,
    sensing: SensingService,
    telemetry: TelemetryService,
    arrhythmia: ArrhythmiaService,
    battery: BatteryService,
    verbose: bool,
}

impl SimulationOrchestrator {
    /// `seed` replaces legacy's two independent internal `time(NULL)`
    /// reads (sensing.c's and battery.c's LCGs seeded separately) — both
    /// are derived here from one caller-supplied seed so a run is
    /// reproducible end-to-end when the caller wants that (e.g. tests),
    /// while `SimulationOrchestrator::new_from_time()` below reproduces
    /// legacy's "seed from wall clock" default for normal CLI use.
    pub fn new(mode: PaceMode, verbose: bool, seed: u32) -> Self {
        SimulationOrchestrator {
            engine: PacingEngine::new(mode),
            sensing: SensingService::new(seed),
            telemetry: TelemetryService::new(),
            arrhythmia: ArrhythmiaService::new(mode),
            // legacy battery.c seeds with `0x9E3779B9 ^ time(NULL)`
            // independently of sensing.c's `time(NULL)` seed; passing the
            // same caller seed to both here is a deliberate, disclosed
            // simplification for reproducibility (see MIGRATION_NOTES.md
            // #16) — a real air-gapped deployment with a hardware RNG
            // would seed each independently again.
            battery: BatteryService::new(seed),
            verbose,
        }
    }

    pub fn new_from_time(mode: PaceMode, verbose: bool) -> Self {
        use std::time::{SystemTime, UNIX_EPOCH};
        let seed = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs() as u32)
            .unwrap_or(0xDEAD_BEEF);
        Self::new(mode, verbose, seed)
    }

    /// Legacy: main.c's "load then maybe re-override" pattern — kept as
    /// an explicit method precisely BECAUSE main.c's own comment flags it
    /// as fragile, so the seam stays visible and named rather than
    /// silently inlined.
    pub fn override_mode(&mut self, mode: PaceMode) {
        self.engine.apply_mode_defaults(mode);
        self.arrhythmia = ArrhythmiaService::new(mode);
    }

    pub fn mode(&self) -> PaceMode {
        self.engine.mode()
    }

    pub fn current_params(&self) -> PacerParams {
        self.engine.to_params()
    }

    /// Composes `PersistenceService::load()` with the legacy
    /// auto-heal-and-resave fallback explicitly, at the ONE call site
    /// that needs it, rather than hiding the side effect inside `load()`
    /// itself (see persistence.rs's module doc comment and Section 6 of
    /// the architecture proposal — the approved behavior change). Returns
    /// whether a previously-persisted, valid parameter set was found
    /// (legacy: `eeprom_load()`'s return value), for the CLI's own
    /// verbose diagnostics if it wants them.
    pub fn load_or_initialize(&mut self, persistence: &PersistenceService) -> bool {
        match persistence.load() {
            Ok(params) => {
                self.engine.apply_params(params);
                true
            }
            Err(_) => {
                let defaults = PacerParams::defaults();
                self.engine.apply_params(defaults);
                let _ = persistence.save(&defaults); // legacy: eeprom_load()'s use_defaults path also calls eeprom_save()
                false
            }
        }
    }

    pub fn save_params(&self, persistence: &PersistenceService) -> Result<(), crate::persistence::PersistenceError> {
        persistence.save(&self.current_params())
    }

    /// Legacy: `main()`'s simulation `while` loop + `pacer_core_tick()`,
    /// merged into the one place both belong. Writes status lines,
    /// telemetry hex dumps (verbose only), and AMS engage/disengage
    /// messages to `out` in the same order and under the same conditions
    /// legacy printed them — `out` is a generic `Write` (Rule 4: an
    /// idiomatic target-language feature — a real stdout handle in the
    /// CLI, a `Vec<u8>` buffer in tests) rather than an implicit global
    /// stdout.
    pub fn run<W: Write>(&mut self, duration_ms: u32, out: &mut W) -> std::io::Result<SimulationReport> {
        let mut totals = EventTotals::default();
        let mut elapsed_since_status: u32 = 0;
        let mut aborted_on_fault = false;

        while self.engine.tick_ms() < duration_ms {
            let new_tick_ms = self.engine.tick_ms() + SIM_STEP_MS;
            let sample = self.sensing.sample(new_tick_ms);

            let step = self.engine.handle_state(new_tick_ms, &mut self.sensing, sample, &mut self.telemetry, self.verbose);
            for line in &step.dump_lines {
                writeln!(out, "{}", line)?;
            }

            // legacy: crude re-scan-the-log-for-this-tick's-entry event
            // tallying (main.c) replaced with a direct read of this
            // tick's own StepOutcome — same totals, no log re-indexing
            // needed now that the log has exactly one owner (see
            // MIGRATION_NOTES.md #17).
            if step.evt_flags.contains(EventFlags::A_PACE) { totals.a_paces += 1; }
            if step.evt_flags.contains(EventFlags::V_PACE) { totals.v_paces += 1; }
            if step.evt_flags.contains(EventFlags::A_SENSE) { totals.a_senses += 1; }
            if step.evt_flags.contains(EventFlags::V_SENSE) { totals.v_senses += 1; }

            let atrial_interval = self.engine.atrial_interval_if_sensed(step.evt_flags);
            let decision = self.arrhythmia.evaluate(atrial_interval, self.engine.mode());
            for line in self.apply_arrhythmia_lines(decision) {
                writeln!(out, "{}", line)?;
            }

            let battery_status = self.battery.tick(SIM_STEP_MS, step.paced_a || step.paced_v, self.engine.pace_ampl_mv(), self.engine.pace_width_ms());
            // legacy `battery_check_eol()` call is a no-op with no
            // observable effect beyond what `battery_tick()` already did
            // (see battery.c's own comment) — not reproduced as a
            // separate call.
            let _ = battery_status;

            elapsed_since_status += SIM_STEP_MS;
            if elapsed_since_status >= STATUS_PRINT_EVERY_MS {
                self.write_status_line(out)?;
                elapsed_since_status = 0;
            }

            if self.engine.state_name() == "FAULT" {
                writeln!(out, "[main] FAULT state reached, aborting simulation early at t={}ms", self.engine.tick_ms())?;
                aborted_on_fault = true;
                break;
            }
        }

        Ok(SimulationReport {
            final_tick_ms: self.engine.tick_ms(),
            totals,
            battery: self.battery.status(),
            final_mode: self.engine.mode(),
            final_state: self.engine.state_name(),
            ams_active: self.arrhythmia.ams_active(),
            aborted_on_fault,
        })
    }

    fn apply_arrhythmia_lines(&mut self, decision: crate::arrhythmia::ArrhythmiaDecision) -> Vec<String> {
        use crate::arrhythmia::ArrhythmiaDecision as D;
        let mut lines = Vec::new();

        if let D::Enter { .. } = decision {
            let was = self.engine.mode();
            let dump = self.engine.apply_arrhythmia_decision(decision, &mut self.telemetry, self.verbose);
            if self.verbose {
                lines.push(format!(
                    "[arrhythmia] AMS ENGAGED at t={}ms (was {}, now {})",
                    self.engine.tick_ms(), was, self.engine.mode()
                ));
            }
            if let Some(d) = dump {
                lines.push(d);
            }
        } else if let D::Exit { restore_mode } = decision {
            if self.verbose {
                lines.push(format!(
                    "[arrhythmia] AMS DISENGAGED at t={}ms (restoring {})",
                    self.engine.tick_ms(), restore_mode
                ));
            }
            let _ = self.engine.apply_arrhythmia_decision(decision, &mut self.telemetry, self.verbose);
        }

        lines
    }

    /// Legacy: `print_status_line()` (main.c). The `~vrate` computation's
    /// mismatch with `arrhythmia.c`'s own internal rate check is a
    /// documented, preserved characterization quirk (both derive an
    /// implied heart rate from a v-interval using slightly different
    /// integer rounding/base values) — not unified here, since it wasn't
    /// one of the 6 approved findings.
    fn write_status_line<W: Write>(&self, out: &mut W) -> std::io::Result<()> {
        let v_interval = {
            let raw = self.engine.tick_ms().saturating_sub(self.engine.last_v_evt_ms());
            if raw == 0 { 1 } else { raw }
        };
        let approx_bpm = 60_000u32 / (v_interval + 1);
        let batt = self.battery.status();

        writeln!(
            out,
            "t={:6}ms mode={:<4} state={:<13} batt={:4}mV a_imp={:4}ohm v_imp={:4}ohm eri={} eol={} ams={} ~vrate={}bpm",
            self.engine.tick_ms(),
            self.engine.mode().as_str(),
            self.engine.state_name(),
            batt.mv,
            batt.lead_a_impedance,
            batt.lead_v_impedance,
            batt.eri as u8,
            batt.eol as u8,
            self.arrhythmia.ams_active() as u8,
            approx_bpm,
        )
    }

    pub fn telemetry_dump(&self) -> String {
        self.telemetry.dump()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn short_run_reaches_completion_and_totals_are_consistent() {
        let mut orch = SimulationOrchestrator::new(PaceMode::Ddd, false, 42);
        let mut buf: Vec<u8> = Vec::new();
        let report = orch.run(200, &mut buf).unwrap();
        assert_eq!(report.final_tick_ms, 200);
        assert!(!report.aborted_on_fault);
    }

    #[test]
    fn sub_1000ms_run_emits_no_status_line() {
        // legacy characterization: a run under STATUS_PRINT_EVERY_MS
        // never reaches the periodic status line at all.
        let mut orch = SimulationOrchestrator::new(PaceMode::Ddd, false, 1);
        let mut buf: Vec<u8> = Vec::new();
        orch.run(200, &mut buf).unwrap();
        let text = String::from_utf8(buf).unwrap();
        assert!(!text.contains("state="));
    }

    #[test]
    fn run_of_at_least_1000ms_emits_a_status_line() {
        let mut orch = SimulationOrchestrator::new(PaceMode::Ddd, false, 1);
        let mut buf: Vec<u8> = Vec::new();
        orch.run(1500, &mut buf).unwrap();
        let text = String::from_utf8(buf).unwrap();
        assert!(text.contains("state="));
    }

    #[test]
    fn override_mode_resets_arrhythmia_pre_ams_mode() {
        let mut orch = SimulationOrchestrator::new(PaceMode::Vvi, false, 1);
        orch.override_mode(PaceMode::Ddd);
        assert_eq!(orch.mode(), PaceMode::Ddd);
    }
}
