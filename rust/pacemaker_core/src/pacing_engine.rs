//! PacingEngine — migrated from `src/pacer_core.c` MERGED with
//! `src/modes.c` (Section 2.2 of the architecture proposal: "these two
//! files already share one conceptual state machine ... splitting them
//! was a file-size accident, not a design decision").
//!
//! Structural resolutions applied here:
//!
//! - **Finding #1** (`mode_step()` dead code, modes.c): not carried into
//!   this module's public surface at all — `grep` confirmed zero call
//!   sites in the legacy codebase before this migration began, and a
//!   trait method with no caller in the new design simply has no reason
//!   to exist. See MIGRATION_NOTES.md #1 for the explicit "confirm no
//!   external caller was planned" flag this still carries.
//!
//! - **Finding #2** (pacer_core.c's confirmed-dead single-chamber
//!   "neither, or both" pacing fallback, reachable in legacy C only via
//!   an out-of-range `PaceMode` value): `handle_state_wait_lrl_single()`
//!   below is an exhaustive 2-arm match over "ventricular-paced-only" vs
//!   "atrial-paced-only" — verified by hand against all 12 real
//!   `PaceMode` variants (every single-chamber mode is exactly one or the
//!   other; see MIGRATION_NOTES.md #2 for the per-mode table) — so the
//!   third, legacy "neither/both" arm has no arm to exist in, i.e. is
//!   unreachable BY CONSTRUCTION, not by convention (`PaceMode` is a
//!   closed Rust enum — there is no out-of-range value to reach it with).
//!
//! One additional legacy dead-code path was found and eliminated during
//! translation that was NOT one of the 6 approved findings:
//! `update_refractory_windows()`'s dual-chamber and non-dual-chamber
//! branches were byte-for-byte identical logic (confirmed by direct
//! reading, not dynamic testing) — collapsed to one branch. See
//! MIGRATION_NOTES.md #13.

use crate::arrhythmia::ArrhythmiaDecision;
use crate::persistence::PacerParams;
use crate::sensing::SensingService;
use crate::telemetry::TelemetryService;
use crate::types::{EventFlags, PaceMode, PacerState, SenseSample};

const DEFAULT_LRL_MS: u16 = 1000;
const DEFAULT_URL_MS: u16 = 461;
const DEFAULT_AVI_MS: u16 = 150;
const DEFAULT_PVARP_MS: u16 = 250;
const DEFAULT_VRP_MS: u16 = 200;
const DEFAULT_ARP_MS: u16 = 150;
const DEFAULT_PACE_AMPL_MV: u16 = 3500;
const DEFAULT_PACE_WIDTH_MS: u8 = 1;
const A_SENSE_THRESH_MV: u16 = 250;
const V_SENSE_THRESH_MV: u16 = 500;

/// Result of one `PacingEngine::handle_state()` call — replaces the
/// legacy module-static `s_evt_flags_this_tick` / `s_paced_a_this_tick` /
/// `s_paced_v_this_tick` (pacer_core.c file-scope statics) with an
/// explicit return value.
#[derive(Debug, Default)]
pub struct StepOutcome {
    pub evt_flags: EventFlags,
    pub paced_a: bool,
    pub paced_v: bool,
    /// Verbose telemetry hex-dump lines produced this tick, in the exact
    /// order legacy would have printed them (sense events, then pace
    /// events, then any AMS engage/disengage line) — see `orchestrator.rs`
    /// for where these get printed.
    pub dump_lines: Vec<String>,
}

/// Bundles per-call context (the current sense sample, the telemetry
/// sink, verbosity, and the outcome being built) so internal helper
/// methods take one argument instead of four — purely an internal
/// ergonomics refactor (satisfies clippy's `too_many_arguments` lint);
/// no public API or behavior is affected.
struct TickCtx<'a> {
    sample: SenseSample,
    telemetry: &'a mut TelemetryService,
    verbose: bool,
    out: StepOutcome,
}

pub struct PacingEngine {
    state: PacerState,
    mode: PaceMode,

    tick_ms: u32,
    /// Legacy: `g_ms_since_boot` — incremented every tick in
    /// `pacer_core_tick()` but never read anywhere in the legacy
    /// codebase (grep-confirmed). Kept for structural parity rather than
    /// dropped outright, since Code Generation's mandate is translation,
    /// not unrequested cleanup of every dead field discovered along the
    /// way — flagged in MIGRATION_NOTES.md #19 as a candidate for
    /// removal in a future pass.
    #[allow(dead_code)]
    ms_since_boot: u32,
    last_a_evt_ms: u32,
    last_v_evt_ms: u32,
    a_refractory: bool,
    v_refractory: bool,

    // per-mode timing parameters — legacy g_lrl_ms / g_url_ms / etc.,
    // owned here exclusively instead of as pacer.h externs.
    lrl_ms: u16,
    url_ms: u16,
    avi_ms: u16,
    pvarp_ms: u16,
    vrp_ms: u16,
    arp_ms: u16,
    pace_ampl_mv: u16,
    pace_width_ms: u8,
    a_sense_thresh_mv: u16,
    v_sense_thresh_mv: u16,
    rate_resp_enabled: bool,
}

impl PacingEngine {
    /// Legacy: `pacer_core_init()` (mode application + `reset_cycle()`
    /// only — `sensing_init()`/`telemetry_init()`/`battery_init()`/
    /// `arrhythmia_init()` are now each owned by their own service's own
    /// constructor, called by `SimulationOrchestrator::new()`).
    pub fn new(initial_mode: PaceMode) -> Self {
        let mut engine = PacingEngine {
            state: PacerState::Init,
            mode: initial_mode,
            tick_ms: 0,
            ms_since_boot: 0,
            last_a_evt_ms: 0,
            last_v_evt_ms: 0,
            a_refractory: false,
            v_refractory: false,
            lrl_ms: DEFAULT_LRL_MS,
            url_ms: DEFAULT_URL_MS,
            avi_ms: DEFAULT_AVI_MS,
            pvarp_ms: DEFAULT_PVARP_MS,
            vrp_ms: DEFAULT_VRP_MS,
            arp_ms: DEFAULT_ARP_MS,
            pace_ampl_mv: DEFAULT_PACE_AMPL_MV,
            pace_width_ms: DEFAULT_PACE_WIDTH_MS,
            a_sense_thresh_mv: A_SENSE_THRESH_MV,
            v_sense_thresh_mv: V_SENSE_THRESH_MV,
            rate_resp_enabled: false,
        };
        engine.apply_mode_defaults(initial_mode);
        engine.reset_cycle();
        engine
    }

    pub fn state_name(&self) -> &'static str {
        self.state.name()
    }

    pub fn mode(&self) -> PaceMode {
        self.mode
    }

    pub fn tick_ms(&self) -> u32 {
        self.tick_ms
    }

    pub fn pace_ampl_mv(&self) -> u16 { self.pace_ampl_mv }
    pub fn pace_width_ms(&self) -> u8 { self.pace_width_ms }
    pub fn last_v_evt_ms(&self) -> u32 { self.last_v_evt_ms }

    /// Legacy: `pacer_core_reset_cycle()`.
    pub fn reset_cycle(&mut self) {
        self.last_a_evt_ms = self.tick_ms;
        self.last_v_evt_ms = self.tick_ms;
        self.a_refractory = false;
        self.v_refractory = false;
        self.state = PacerState::WaitLrl;
    }

    /// Legacy: `mode_apply_defaults()` (modes.c). The per-mode `match`
    /// arms are a direct, unreduced translation of the legacy switch —
    /// no attempt was made here to factor out the shared "textbook
    /// defaults" prefix further than legacy already did; see Rule 2
    /// (preserve behavior first).
    pub fn apply_mode_defaults(&mut self, m: PaceMode) {
        self.mode = m;
        self.lrl_ms = DEFAULT_LRL_MS;
        self.url_ms = DEFAULT_URL_MS;
        self.avi_ms = DEFAULT_AVI_MS;
        self.pvarp_ms = DEFAULT_PVARP_MS;
        self.vrp_ms = DEFAULT_VRP_MS;
        self.arp_ms = DEFAULT_ARP_MS;
        self.pace_ampl_mv = DEFAULT_PACE_AMPL_MV;
        self.pace_width_ms = DEFAULT_PACE_WIDTH_MS;
        self.a_sense_thresh_mv = A_SENSE_THRESH_MV;
        self.v_sense_thresh_mv = V_SENSE_THRESH_MV;
        self.rate_resp_enabled = false;

        match m {
            PaceMode::Aoo => { self.a_sense_thresh_mv = 9999; }
            PaceMode::Voo => { self.v_sense_thresh_mv = 9999; }
            PaceMode::Aai | PaceMode::Vvi | PaceMode::Aat | PaceMode::Vvt => { self.avi_ms = 0; }
            PaceMode::Aair => { self.rate_resp_enabled = true; self.avi_ms = 0; }
            PaceMode::Vvir => { self.rate_resp_enabled = true; self.avi_ms = 0; }
            PaceMode::Vdd => { self.avi_ms = 130; }
            PaceMode::Ddi => { self.avi_ms = 150; }
            PaceMode::Ddd => { self.avi_ms = 150; }
            PaceMode::Dddr => { self.rate_resp_enabled = true; self.avi_ms = 140; }
        }
        // legacy's `default:` arm (unknown mode -> plain defaults, log a
        // warning) has no arm to exist in: PaceMode is exhaustively
        // matched above over its 12 real variants.
    }

    /// Legacy: `update_refractory_windows()`. The legacy dual-chamber and
    /// non-dual-chamber branches were identical logic (see module doc
    /// comment #13) — collapsed to one branch here.
    fn update_refractory_windows(&mut self) {
        if self.a_refractory && (self.tick_ms - self.last_a_evt_ms) >= self.arp_ms as u32 {
            self.a_refractory = false;
        }
        if self.v_refractory && (self.tick_ms - self.last_v_evt_ms) >= self.vrp_ms as u32 {
            self.v_refractory = false;
        }
    }

    fn issue_a_pace(&mut self, ctx: &mut TickCtx) {
        self.last_a_evt_ms = self.tick_ms;
        self.a_refractory = true;
        ctx.out.paced_a = true;
        ctx.out.evt_flags |= EventFlags::A_PACE;
        if let Some(line) = ctx.telemetry.record(EventFlags::A_PACE, ctx.sample.a_mv, ctx.sample.v_mv, self.tick_ms, self.mode, ctx.verbose) {
            ctx.out.dump_lines.push(line);
        }
    }

    fn issue_v_pace(&mut self, ctx: &mut TickCtx) {
        self.last_v_evt_ms = self.tick_ms;
        self.v_refractory = true;
        ctx.out.paced_v = true;
        ctx.out.evt_flags |= EventFlags::V_PACE;
        if let Some(line) = ctx.telemetry.record(EventFlags::V_PACE, ctx.sample.a_mv, ctx.sample.v_mv, self.tick_ms, self.mode, ctx.verbose) {
            ctx.out.dump_lines.push(line);
        }
    }

    /// Legacy: `pacer_core_handle_state()`. `tick_ms` must already
    /// reflect this tick's advance (caller — `SimulationOrchestrator` —
    /// owns the tick counter, matching Task #3's "no shared global
    /// state": `g_tick_ms` was pacer.h's single most heavily shared
    /// global, touched by all 8 legacy files).
    pub fn handle_state(&mut self, tick_ms: u32, sensing: &mut SensingService, sample: SenseSample, telemetry: &mut TelemetryService, verbose: bool) -> StepOutcome {
        self.ms_since_boot += tick_ms.saturating_sub(self.tick_ms);
        self.tick_ms = tick_ms;
        self.update_refractory_windows();

        let mut ctx = TickCtx { sample, telemetry, verbose, out: StepOutcome::default() };
        let mut a_sensed = false;
        let mut v_sensed = false;

        if !sensing.is_noise(sample.a_mv, self.a_sense_thresh_mv as i16) {
            if sensing.check_atrial(sample.a_mv, self.a_sense_thresh_mv as i16, self.a_refractory, self.tick_ms, self.last_a_evt_ms) {
                a_sensed = true;
            }
        } else {
            ctx.out.evt_flags |= EventFlags::NOISE;
        }

        if !sensing.is_noise(sample.v_mv, self.v_sense_thresh_mv as i16) {
            if sensing.check_ventricular(sample.v_mv, self.v_sense_thresh_mv as i16, self.v_refractory, self.tick_ms, self.last_v_evt_ms) {
                v_sensed = true;
            }
        } else {
            ctx.out.evt_flags |= EventFlags::NOISE;
        }

        if a_sensed && self.mode.is_atrial_sensed() {
            self.last_a_evt_ms = self.tick_ms;
            ctx.out.evt_flags |= EventFlags::A_SENSE;
            if let Some(line) = ctx.telemetry.record(EventFlags::A_SENSE, sample.a_mv, sample.v_mv, self.tick_ms, self.mode, verbose) {
                ctx.out.dump_lines.push(line);
            }
        } else {
            a_sensed = false;
        }

        if v_sensed && self.mode.is_ventricular_sensed() {
            self.last_v_evt_ms = self.tick_ms;
            ctx.out.evt_flags |= EventFlags::V_SENSE;
            if let Some(line) = ctx.telemetry.record(EventFlags::V_SENSE, sample.a_mv, sample.v_mv, self.tick_ms, self.mode, verbose) {
                ctx.out.dump_lines.push(line);
            }
        } else {
            v_sensed = false;
        }

        let since_a = self.tick_ms - self.last_a_evt_ms;
        let since_v = self.tick_ms - self.last_v_evt_ms;

        match self.state {
            PacerState::Init => {
                self.state = PacerState::WaitLrl;
            }

            PacerState::WaitLrl => {
                if self.mode.is_dual_chamber() {
                    self.handle_wait_lrl_dual(a_sensed, since_a, since_v, &mut ctx);
                } else {
                    self.handle_wait_lrl_single(a_sensed, v_sensed, since_a, since_v, &mut ctx);
                }
            }

            PacerState::AviWait => {
                if v_sensed {
                    self.state = PacerState::WaitLrl;
                } else if since_a >= self.avi_ms as u32 {
                    if !self.v_refractory {
                        self.issue_v_pace(&mut ctx);
                    }
                    self.state = PacerState::WaitLrl;
                } else if since_a >= self.url_ms as u32 {
                    // URL safety fallback (Finding #5's context): don't
                    // let AVI wait run away forever. Reachability of this
                    // arm depends on avi_ms < url_ms, now enforced by
                    // PersistenceService::validate() at load time.
                    if !self.v_refractory {
                        self.issue_v_pace(&mut ctx);
                    }
                    self.state = PacerState::WaitLrl;
                }
                // else: remain in AviWait
            }

            PacerState::Fault => {
                // Structurally unreachable: nothing in this match ever
                // transitions state -> Fault (the legacy state-switch's
                // `default:` arm required an out-of-range g_state value,
                // impossible for a closed Rust enum). Retained only so
                // the CLI-level "if state == Fault, abort" check
                // (main.c-derived, see cli.rs) stays parity-testable
                // against the legacy binary's pinned-unreachable branch.
            }
        }

        ctx.out
    }

    fn handle_wait_lrl_dual(&mut self, a_sensed: bool, since_a: u32, since_v: u32, ctx: &mut TickCtx) {
        if a_sensed {
            self.state = PacerState::AviWait;
            return;
        }

        let mut advanced = false;
        if self.mode.is_atrial_paced() {
            if since_a >= self.lrl_ms as u32 && !self.a_refractory {
                self.issue_a_pace(ctx);
                self.state = PacerState::AviWait;
                advanced = true;
            }
        } else if since_a >= self.lrl_ms as u32 {
            self.state = PacerState::AviWait;
            advanced = true;
        }

        if advanced {
            return;
        }

        // Dual-chamber ventricular safety-net fallback. NOT proven dead
        // by the characterization suite (unlike the single-chamber
        // fallback below) — preserved as literal, reachable translation
        // rather than eliminated. See module doc comment and
        // MIGRATION_NOTES.md #14.
        if since_v >= (self.lrl_ms as u32 + self.avi_ms as u32) && !self.v_refractory {
            self.issue_v_pace(ctx);
        }
    }

    /// Legacy single-chamber dispatch. Exhaustive 2-arm match — see
    /// module doc comment on Finding #2's elimination.
    fn handle_wait_lrl_single(&mut self, a_sensed: bool, v_sensed: bool, since_a: u32, since_v: u32, ctx: &mut TickCtx) {
        let v_only = self.mode.is_ventricular_paced() && !self.mode.is_atrial_paced();

        if v_only {
            if v_sensed {
                return;
            }
            if since_v >= self.lrl_ms as u32 && !self.v_refractory {
                self.issue_v_pace(ctx);
            }
        } else {
            // is_atrial_paced && !is_ventricular_paced for every real
            // remaining single-chamber mode (AOO/AAI/AAT/AAIR) — see
            // MIGRATION_NOTES.md #2's per-mode table.
            if a_sensed {
                return;
            }
            if since_a >= self.lrl_ms as u32 && !self.a_refractory {
                self.issue_a_pace(ctx);
            }
        }
    }

    /// Applies an `ArrhythmiaDecision` (see arrhythmia.rs): mode
    /// override on Enter, mode restore on Exit, and asks
    /// `TelemetryService` — the log's sole owner — to record the
    /// mode-switch event (structural resolution of Finding #4's concrete
    /// manifestation; see arrhythmia.rs's module doc comment).
    pub fn apply_arrhythmia_decision(&mut self, decision: ArrhythmiaDecision, telemetry: &mut TelemetryService, verbose: bool) -> Option<String> {
        match decision {
            ArrhythmiaDecision::NoChange => None,
            ArrhythmiaDecision::Enter { override_mode } => {
                if let Some(new_mode) = override_mode {
                    self.apply_mode_defaults(new_mode);
                }
                telemetry.record(EventFlags::MODE_SWITCH, 0, 0, self.tick_ms, self.mode, verbose)
            }
            ArrhythmiaDecision::Exit { restore_mode } => {
                self.apply_mode_defaults(restore_mode);
                None
            }
        }
    }

    /// Legacy: the `atrial_interval` computation in `pacer_core_tick()`,
    /// PRESERVED EXACTLY INCLUDING ITS DOCUMENTED SMELL: legacy's own
    /// comment reads "this recomputes an interval that's basically
    /// always going to be ~0 right after we just set g_last_a_evt_ms
    /// above in handle_state() ... the intended interval calc should use
    /// the PREVIOUS event time, not the one we just overwrote." Because
    /// `last_a_evt_ms` is set to `tick_ms` inside `handle_state()`
    /// whenever `EVT_A_SENSE` fires, the interval computed here is always
    /// `0` (forced to `1`) — meaning `ArrhythmiaService` always receives
    /// `interval_ms = 1` on any atrial sense, classifying it as "fast"
    /// regardless of actual physiological rate. This was NOT one of the
    /// 6 approved findings and is deliberately NOT fixed here (see
    /// MIGRATION_NOTES.md #15) — fixing it would change arrhythmia
    /// detection's observable behavior for every dual-chamber mode,
    /// which is outside this stage's approved scope.
    /// Legacy: the manual field-by-field copy into `NvramParams_t` that
    /// both `eeprom_load()`'s `use_defaults:` path and `eeprom_save()`
    /// duplicated independently (eeprom.c's own header comment flags
    /// this duplication as a smell). One conversion here, used by both
    /// directions via `PersistenceService`.
    pub fn to_params(&self) -> PacerParams {
        PacerParams {
            mode: self.mode,
            lrl_ms: self.lrl_ms,
            url_ms: self.url_ms,
            avi_ms: self.avi_ms,
            pvarp_ms: self.pvarp_ms,
            vrp_ms: self.vrp_ms,
            arp_ms: self.arp_ms,
            pace_ampl_mv: self.pace_ampl_mv,
            pace_width_ms: self.pace_width_ms,
            a_sense_thresh_mv: self.a_sense_thresh_mv,
            v_sense_thresh_mv: self.v_sense_thresh_mv,
            rate_resp_enabled: self.rate_resp_enabled,
        }
    }

    /// Applies a loaded/persisted parameter set wholesale (legacy:
    /// `eeprom_load()`'s field-by-field copy into globals). Mode is
    /// applied via `apply_mode_defaults()` first (matching legacy's
    /// order of operations), then every other field is overwritten with
    /// the persisted value, since a persisted file may hold values that
    /// differ from that mode's textbook defaults (e.g. a future config
    /// UI could have customized them).
    pub fn apply_params(&mut self, p: PacerParams) {
        self.apply_mode_defaults(p.mode);
        self.lrl_ms = p.lrl_ms;
        self.url_ms = p.url_ms;
        self.avi_ms = p.avi_ms;
        self.pvarp_ms = p.pvarp_ms;
        self.vrp_ms = p.vrp_ms;
        self.arp_ms = p.arp_ms;
        self.pace_ampl_mv = p.pace_ampl_mv;
        self.pace_width_ms = p.pace_width_ms;
        self.a_sense_thresh_mv = p.a_sense_thresh_mv;
        self.v_sense_thresh_mv = p.v_sense_thresh_mv;
        self.rate_resp_enabled = p.rate_resp_enabled;
    }

    pub fn atrial_interval_if_sensed(&self, evt_flags: EventFlags) -> Option<u32> {
        if evt_flags.contains(EventFlags::A_SENSE) {
            let interval = self.tick_ms - self.last_a_evt_ms;
            Some(if interval == 0 { 1 } else { interval })
        } else {
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine_for(mode: PaceMode) -> PacingEngine {
        PacingEngine::new(mode)
    }

    #[test]
    fn init_transitions_to_wait_lrl_on_first_tick() {
        let e = engine_for(PaceMode::Ddd);
        assert_eq!(e.state_name(), "WAIT_LRL"); // reset_cycle() already set this at construction
    }

    #[test]
    fn aoo_paces_atrium_on_lrl_timeout_with_zero_sample() {
        let mut e = engine_for(PaceMode::Aoo);
        let mut sensing = SensingService::new(1);
        let mut telemetry = TelemetryService::new();
        // zero-sample technique: an all-zero sample plus AOO's widened
        // a_sense_thresh_mv=9999 guarantees is_noise()=true, so a_sensed
        // is always false and only the LRL timeout path can pace.
        let sample = SenseSample::default();
        let mut ever_paced_a = false;
        let mut ever_paced_v = false;
        for t in (10..=1010).step_by(10) {
            let out = e.handle_state(t, &mut sensing, sample, &mut telemetry, false);
            ever_paced_a |= out.paced_a;
            ever_paced_v |= out.paced_v;
        }
        assert!(ever_paced_a, "AOO should pace the atrium once LRL elapses with no sensing");
        assert!(!ever_paced_v);
    }

    #[test]
    fn voo_paces_ventricle_on_lrl_timeout() {
        let mut e = engine_for(PaceMode::Voo);
        let mut sensing = SensingService::new(1);
        let mut telemetry = TelemetryService::new();
        let sample = SenseSample::default();
        let mut ever_paced_a = false;
        let mut ever_paced_v = false;
        for t in (10..=1010).step_by(10) {
            let out = e.handle_state(t, &mut sensing, sample, &mut telemetry, false);
            ever_paced_a |= out.paced_a;
            ever_paced_v |= out.paced_v;
        }
        assert!(ever_paced_v);
        assert!(!ever_paced_a);
    }

    #[test]
    fn ddd_dual_chamber_no_sense_paces_atrium_then_transitions_to_avi_wait() {
        let mut e = engine_for(PaceMode::Ddd);
        let mut sensing = SensingService::new(1);
        let mut telemetry = TelemetryService::new();
        let sample = SenseSample::default(); // zero-sample: guarantees noise on real thresholds too (below floor)
        let mut ever_paced_a = false;
        for t in (10..=1010).step_by(10) {
            let out = e.handle_state(t, &mut sensing, sample, &mut telemetry, false);
            ever_paced_a |= out.paced_a;
        }
        assert!(ever_paced_a);
        assert_eq!(e.state_name(), "AVI_WAIT");
    }

    #[test]
    fn avi_wait_paces_ventricle_after_avi_elapses() {
        let mut e = engine_for(PaceMode::Ddd);
        let mut sensing = SensingService::new(1);
        let mut telemetry = TelemetryService::new();
        let sample = SenseSample::default();
        let mut t: u32 = 0;
        for _ in 0..101 { // reach AVI_WAIT (atrial pace at t=1000)
            t += 10;
            e.handle_state(t, &mut sensing, sample, &mut telemetry, false);
        }
        assert_eq!(e.state_name(), "AVI_WAIT");
        let mut out = StepOutcome::default();
        for _ in 0..20 { // avi_ms=150 for DDD
            t += 10;
            out = e.handle_state(t, &mut sensing, sample, &mut telemetry, false);
            if out.paced_v { break; }
        }
        assert!(out.paced_v);
        assert_eq!(e.state_name(), "WAIT_LRL");
    }

    #[test]
    fn apply_mode_defaults_widens_thresholds_for_aoo_voo() {
        let e_aoo = engine_for(PaceMode::Aoo);
        assert_eq!(e_aoo.a_sense_thresh_mv, 9999);
        let e_voo = engine_for(PaceMode::Voo);
        assert_eq!(e_voo.v_sense_thresh_mv, 9999);
    }

    #[test]
    fn atrial_interval_quirk_is_preserved_as_characterized() {
        let mut e = engine_for(PaceMode::Ddd);
        e.tick_ms = 500;
        e.last_a_evt_ms = 500; // simulating handle_state() having just set this on A_SENSE
        let interval = e.atrial_interval_if_sensed(EventFlags::A_SENSE);
        assert_eq!(interval, Some(1)); // 0 forced to 1, per the documented legacy smell
    }

    #[test]
    fn arrhythmia_enter_applies_ddi_override_for_ddd() {
        let mut e = engine_for(PaceMode::Ddd);
        let mut telemetry = TelemetryService::new();
        e.tick_ms = 5000;
        let line = e.apply_arrhythmia_decision(
            ArrhythmiaDecision::Enter { override_mode: Some(PaceMode::Ddi) },
            &mut telemetry,
            false,
        );
        assert_eq!(e.mode(), PaceMode::Ddi);
        assert_eq!(telemetry.count(), 1);
        assert!(line.is_none()); // verbose=false -> no dump line, but still logged
    }
}
