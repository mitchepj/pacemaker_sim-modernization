//! ArrhythmiaService — migrated from `src/arrhythmia.c`.
//!
//! Structural resolution of the concrete mechanism behind **Finding #4**:
//! legacy `arrhythmia_run_ams_logic()` wrote directly into the shared
//! event log with its OWN indexing formula —
//! `g_log[g_log_count % MAX_LOG_ENTRIES].flags |= EVT_MODE_SWITCH;` —
//! which does not match `telemetry.c`'s own ring-buffer write formula
//! (`(g_log_head + g_log_count) % MAX_LOG_ENTRIES`). Once the ring buffer
//! wraps (`g_log_head != 0`), arrhythmia.c's write silently corrupts an
//! unrelated, already-logged entry's flags field instead of marking the
//! mode-switch event. `ArrhythmiaService` never touches the log at all
//! now — it returns an `ArrhythmiaDecision`, and the caller (see
//! `pacing_engine.rs`) asks `TelemetryService` — the log's sole owner —
//! to record a fresh, correctly-indexed entry.

use crate::types::PaceMode;

const ATR_DETECT_COUNT: i32 = 8;
const ATR_EXIT_COUNT: i32 = 6;
const DEFAULT_ATR_RATE_BPM: u32 = 180;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ArrhythmiaDecision {
    NoChange,
    /// AMS just engaged. `override_mode` is `Some` only when the current
    /// mode is one of the modes legacy explicitly downgrades (DDD/DDDR ->
    /// DDI, VDD -> VVI); for every other mode AMS engages (the flag and
    /// the log entry) without an actual mode change, exactly as legacy.
    Enter { override_mode: Option<PaceMode> },
    /// AMS just disengaged; legacy unconditionally restores + re-applies
    /// defaults for the pre-AMS mode, even when it never actually changed
    /// on entry — preserved exactly, since `mode_apply_defaults()`
    /// resets ALL timing parameters, not just the mode field.
    Exit { restore_mode: PaceMode },
}

pub struct ArrhythmiaService {
    fast_streak: i32,
    slow_streak: i32,
    atr_counter: u32,
    ams_active: bool,
    pre_ams_mode: PaceMode,
}

impl ArrhythmiaService {
    /// Legacy: `arrhythmia_init()`.
    pub fn new(initial_mode: PaceMode) -> Self {
        ArrhythmiaService {
            fast_streak: 0,
            slow_streak: 0,
            atr_counter: 0,
            ams_active: false,
            pre_ams_mode: initial_mode,
        }
    }

    pub fn atr_counter(&self) -> u32 {
        self.atr_counter
    }

    pub fn ams_active(&self) -> bool {
        self.ams_active
    }

    /// Legacy: `arrhythmia_reset_counters()`.
    fn reset_counters(&mut self) {
        self.fast_streak = 0;
        self.slow_streak = 0;
        self.atr_counter = 0;
    }

    /// Legacy: `arrhythmia_check_atrial_tachy()` folded into
    /// `run_ams_logic`'s internal streak bookkeeping — called only when
    /// `atrial_interval_ms` is `Some` (an atrial SENSE happened this
    /// tick; legacy gated the call the same way on `EVT_A_SENSE`).
    fn check_atrial_tachy(&mut self, interval_ms: u32) {
        if interval_ms == 0 {
            return;
        }
        let rate_bpm = 60_000u32 / interval_ms;

        if rate_bpm >= DEFAULT_ATR_RATE_BPM {
            self.fast_streak += 1;
            self.slow_streak = 0;
        } else {
            self.slow_streak += 1;
            if self.fast_streak > 0 && self.slow_streak >= 2 {
                self.fast_streak = 0;
            }
        }

        self.atr_counter = self.fast_streak as u32;
    }

    /// Legacy: `arrhythmia_check_atrial_tachy()` call (conditional on an
    /// atrial sense this tick) followed unconditionally by
    /// `arrhythmia_run_ams_logic()` — combined into one call matching
    /// `pacer_core_tick()`'s own call order exactly.
    pub fn evaluate(&mut self, atrial_interval_ms: Option<u32>, current_mode: PaceMode) -> ArrhythmiaDecision {
        if let Some(interval_ms) = atrial_interval_ms {
            self.check_atrial_tachy(interval_ms);
        }

        if !self.ams_active {
            if self.fast_streak >= ATR_DETECT_COUNT {
                self.ams_active = true;
                self.pre_ams_mode = current_mode;

                let override_mode = match current_mode {
                    PaceMode::Ddd | PaceMode::Dddr => Some(PaceMode::Ddi),
                    PaceMode::Vdd => Some(PaceMode::Vvi),
                    _ => None,
                };

                return ArrhythmiaDecision::Enter { override_mode };
            }
        } else if self.slow_streak >= ATR_EXIT_COUNT {
            self.ams_active = false;
            let restore_mode = self.pre_ams_mode;
            self.reset_counters();
            return ArrhythmiaDecision::Exit { restore_mode };
        }

        ArrhythmiaDecision::NoChange
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fast_interval() -> u32 {
        // 60000 / interval >= 180 -> interval <= 333
        300
    }
    fn slow_interval() -> u32 {
        // 60000 / interval < 180 -> interval > 333
        500
    }

    #[test]
    fn engages_after_detect_count_fast_intervals() {
        let mut a = ArrhythmiaService::new(PaceMode::Ddd);
        let mut decision = ArrhythmiaDecision::NoChange;
        for _ in 0..ATR_DETECT_COUNT {
            decision = a.evaluate(Some(fast_interval()), PaceMode::Ddd);
        }
        assert_eq!(decision, ArrhythmiaDecision::Enter { override_mode: Some(PaceMode::Ddi) });
        assert!(a.ams_active());
    }

    #[test]
    fn vdd_downgrades_to_vvi_on_engage() {
        let mut a = ArrhythmiaService::new(PaceMode::Vdd);
        let mut decision = ArrhythmiaDecision::NoChange;
        for _ in 0..ATR_DETECT_COUNT {
            decision = a.evaluate(Some(fast_interval()), PaceMode::Vdd);
        }
        assert_eq!(decision, ArrhythmiaDecision::Enter { override_mode: Some(PaceMode::Vvi) });
    }

    #[test]
    fn non_tracking_mode_engages_without_mode_override() {
        let mut a = ArrhythmiaService::new(PaceMode::Vvi);
        let mut decision = ArrhythmiaDecision::NoChange;
        for _ in 0..ATR_DETECT_COUNT {
            decision = a.evaluate(Some(fast_interval()), PaceMode::Vvi);
        }
        assert_eq!(decision, ArrhythmiaDecision::Enter { override_mode: None });
    }

    #[test]
    fn slow_streak_debounces_fast_streak_decay() {
        // legacy: fast_streak only resets to 0 once slow_streak >= 2
        let mut a = ArrhythmiaService::new(PaceMode::Ddd);
        for _ in 0..3 {
            a.evaluate(Some(fast_interval()), PaceMode::Ddd);
        }
        a.evaluate(Some(slow_interval()), PaceMode::Ddd); // slow_streak=1, fast_streak untouched (still 3)
        assert_eq!(a.atr_counter(), 3);
        a.evaluate(Some(slow_interval()), PaceMode::Ddd); // slow_streak=2 -> fast_streak resets
        assert_eq!(a.atr_counter(), 0);
    }

    #[test]
    fn exits_after_exit_count_slow_intervals_and_restores_mode() {
        let mut a = ArrhythmiaService::new(PaceMode::Ddd);
        for _ in 0..ATR_DETECT_COUNT {
            a.evaluate(Some(fast_interval()), PaceMode::Ddd);
        }
        assert!(a.ams_active());
        let mut decision = ArrhythmiaDecision::NoChange;
        for _ in 0..ATR_EXIT_COUNT {
            decision = a.evaluate(Some(slow_interval()), PaceMode::Ddi);
        }
        assert_eq!(decision, ArrhythmiaDecision::Exit { restore_mode: PaceMode::Ddd });
        assert!(!a.ams_active());
    }

    #[test]
    fn no_atrial_sense_this_tick_still_evaluates_ams_state() {
        // legacy: run_ams_logic() runs every tick unconditionally, even
        // with no atrial sense (atrial_interval_ms = None here).
        let mut a = ArrhythmiaService::new(PaceMode::Ddd);
        let decision = a.evaluate(None, PaceMode::Ddd);
        assert_eq!(decision, ArrhythmiaDecision::NoChange);
    }

    #[test]
    fn zero_interval_is_ignored_exactly_like_legacy() {
        let mut a = ArrhythmiaService::new(PaceMode::Ddd);
        a.evaluate(Some(0), PaceMode::Ddd);
        assert_eq!(a.atr_counter(), 0);
    }
}
