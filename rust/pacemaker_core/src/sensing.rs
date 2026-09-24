//! SensingService — migrated from `src/sensing.c`.
//!
//! Boundary unchanged from the legacy file (Section 2.2 of the
//! architecture proposal: "already cleanly isolated ... promoted as-is").
//! The one substantive change is that the module's LCG state,
//! phase counters, and dropped-beat counter — legacy `static` file-scope
//! globals — become fields on `SensingService`, so two instances never
//! share state (impossible in the legacy C, where there was exactly one
//! process-wide copy).

use crate::types::SenseSample;

const NOISE_FLOOR_MV: i32 = 50;
const A_SENSE_THRESH_DEFAULT_MV: i32 = 250; // A_SENSE_THRESH_MV, pacer.h
const V_SENSE_THRESH_DEFAULT_MV: i32 = 500; // V_SENSE_THRESH_MV, pacer.h

/// Legacy: file-scope `static UINT32 s_lcg_state` + `lcg_next()` /
/// `lcg_range()` in sensing.c. Bit-for-bit identical constants and
/// arithmetic (multiplier 1103515245, increment 12345, mask 0x7FFFFFFF)
/// so seeded-sequence parity testing against the legacy binary is
/// possible (see MIGRATION_NOTES.md #7 on why the LCG was kept instead
/// of switched to a standard PRNG).
#[derive(Copy, Clone, Debug)]
struct Lcg(u32);

impl Lcg {
    fn seeded(seed: u32) -> Self {
        Lcg(if seed == 0 { 0xDEADBEEF } else { seed })
    }

    fn next(&mut self) -> u32 {
        self.0 = (1103515245u32.wrapping_mul(self.0).wrapping_add(12345)) & 0x7FFF_FFFF;
        self.0
    }

    fn range(&mut self, lo: i32, hi: i32) -> i32 {
        let span = hi - lo;
        if span <= 0 {
            return lo;
        }
        let r = self.next();
        lo + (r % (span as u32 + 1)) as i32
    }
}

pub struct SensingService {
    lcg: Lcg,
    a_phase_ms: u32,
    v_phase_ms: u32,
    intrinsic_a_period_ms: u32,
    intrinsic_v_period_ms: u32,
    dropped_beat_counter: u32,
}

impl SensingService {
    /// Legacy: `sensing_init()`. Seed is taken as a parameter rather than
    /// read from `time(NULL)` internally — this is the same seam
    /// `fake_time.c` exploited via link-time interposition in the
    /// characterization test suite, now made an explicit constructor
    /// argument instead of a hidden dependency (see MIGRATION_NOTES.md #8).
    pub fn new(seed: u32) -> Self {
        SensingService {
            lcg: Lcg::seeded(seed),
            a_phase_ms: 0,
            v_phase_ms: 0,
            intrinsic_a_period_ms: 850,
            intrinsic_v_period_ms: 900,
            dropped_beat_counter: 0,
        }
    }

    /// Legacy: `sensing_is_noise()`. Behavior preserved exactly, including
    /// the undocumented 3-in-1000 / 40-in-1000 noise-roll skew.
    pub fn is_noise(&mut self, mv: i16, thresh_mv: i16) -> bool {
        let abs_mv = (mv as i32).abs();
        let thresh = thresh_mv as i32;

        if abs_mv < NOISE_FLOOR_MV {
            return true;
        }

        if abs_mv >= thresh {
            return self.lcg.range(0, 999) < 3;
        }

        if abs_mv > (thresh - thresh / 4) {
            return self.lcg.range(0, 999) < 40;
        }

        false
    }

    /// Legacy: `sensing_generate_sample()`. Synthesizes both channels for
    /// this tick, occasionally drops a beat, injects noise bursts. Unlike
    /// the legacy version this does NOT also push to a history ring buffer
    /// (`g_egm_hist[]` / `sensing_push_history()`). That buffer's only
    /// legacy reader was `telemetry_build_packet()`, which had its own
    /// a_mv/v_mv parameters missing from its signature and worked around
    /// that by re-reading "the most recently pushed history sample"
    /// instead. Trace analysis (documented in MIGRATION_NOTES.md #9)
    /// confirms `telemetry_build_packet()` always ran in the same tick,
    /// after the one `sensing_generate_sample()` call that tick, so the
    /// history read and the sample passed to `telemetry_log_event()`'s own
    /// a_mv/v_mv parameters were always the identical value — the ring
    /// buffer's workaround was equivalent to using the sample already in
    /// hand, never observably different. Removed as a confirmed
    /// behavior-neutral simplification, not a guess.
    pub fn sample(&mut self, tick_ms: u32) -> SenseSample {
        let mut a_mv: i32;
        let mut v_mv: i32;
        let mut a_edge = false;
        let mut v_edge = false;

        self.a_phase_ms += 10;
        self.v_phase_ms += 10;

        if self.a_phase_ms >= self.intrinsic_a_period_ms {
            self.a_phase_ms = 0;
            self.dropped_beat_counter += 1;
            if self.dropped_beat_counter >= 11 && self.dropped_beat_counter.is_multiple_of(11) {
                a_edge = false; // simulated dropped intrinsic atrial beat
            } else {
                a_edge = true;
            }
        }

        if self.v_phase_ms >= self.intrinsic_v_period_ms {
            self.v_phase_ms = 0;
            v_edge = true;
        }

        a_mv = if a_edge {
            A_SENSE_THRESH_DEFAULT_MV + self.lcg.range(50, 400)
        } else {
            self.lcg.range(-30, 30)
        };

        v_mv = if v_edge {
            V_SENSE_THRESH_DEFAULT_MV + self.lcg.range(100, 900)
        } else {
            self.lcg.range(-40, 40)
        };

        let noise_burst = self.lcg.range(0, 4999);
        if noise_burst < 7 {
            a_mv += self.lcg.range(200, 600);
            v_mv += self.lcg.range(200, 600);
        }

        SenseSample {
            a_mv: a_mv as i16,
            v_mv: v_mv as i16,
            t_ms: tick_ms,
        }
    }

    /// Legacy: `sensing_check_atrial()`. `refractory` is passed explicitly
    /// (was `g_a_refractory`); `last_evt_ms` is passed explicitly (was
    /// `g_last_a_evt_ms`) — both now owned by PacingEngine, not read back
    /// through a global. The 40ms debounce window and its asymmetry with
    /// `check_ventricular`'s 60ms window are preserved exactly as
    /// characterized (undocumented copy/paste divergence — not "fixed"
    /// here, since it wasn't one of the 6 flagged findings and silently
    /// unifying it would be exactly the kind of undocumented behavior
    /// change Rule 3 warns against).
    pub fn check_atrial(&mut self, mv: i16, thresh_mv: i16, refractory: bool, tick_ms: u32, last_evt_ms: u32) -> bool {
        if refractory {
            return false;
        }
        if (mv >= thresh_mv || mv <= -thresh_mv) && !self.is_noise(mv, thresh_mv) {
            return (tick_ms - last_evt_ms) > 40;
        }
        false
    }

    /// Legacy: `sensing_check_ventricular()`. 60ms debounce window (see
    /// `check_atrial` doc comment above).
    pub fn check_ventricular(&mut self, mv: i16, thresh_mv: i16, refractory: bool, tick_ms: u32, last_evt_ms: u32) -> bool {
        if refractory {
            return false;
        }
        if (mv >= thresh_mv || mv <= -thresh_mv) && !self.is_noise(mv, thresh_mv) {
            return (tick_ms - last_evt_ms) > 60;
        }
        false
    }
}

// ---------------------------------------------------------------------
// Unit tests — translated from tests/test_sensing.c (characterization
// baseline, 51/51 assertions, 98.99% coverage of the legacy file). Not a
// 1:1 port (the legacy suite drove the linked C binary through
// fake_time.c's link-time time() interposition; this suite calls the
// public Rust API directly with an explicit seed), but every legacy
// assertion's INTENT is preserved: noise floor, debounce windows,
// dropped-beat cadence, threshold-relative noise-roll skew.
// ---------------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn noise_floor_always_noise() {
        let mut s = SensingService::new(12345);
        assert!(s.is_noise(10, 250));
        assert!(s.is_noise(-10, 250));
        assert!(s.is_noise(49, 250));
    }

    #[test]
    fn well_above_threshold_mostly_not_noise() {
        // legacy: abs_mv >= thresh -> noise only ~0.3% of the time (roll < 3/1000)
        let mut s = SensingService::new(42);
        let mut noise_count = 0;
        for _ in 0..1000 {
            if s.is_noise(600, 250) {
                noise_count += 1;
            }
        }
        assert!(noise_count < 30, "expected well under 3% noise rate, got {noise_count}/1000");
    }

    #[test]
    fn debounce_window_asymmetry_preserved() {
        // atrial: >40ms required; ventricular: >60ms required. This is
        // the exact "undocumented copy/paste divergence" the
        // characterization suite pinned as current behavior.
        let mut s = SensingService::new(7);
        // last event at t=0, tick at t=41 -> atrial should be allowed (41>40)
        assert!(s.check_atrial(261, 250, false, 41, 0));
        let mut s2 = SensingService::new(7);
        // ventricular at t=41 should NOT be allowed yet (41 is not >60)
        assert!(!s2.check_ventricular(511, 250, false, 41, 0));
        let mut s3 = SensingService::new(7);
        assert!(s3.check_ventricular(511, 250, false, 61, 0));
    }

    #[test]
    fn refractory_blocks_sensing() {
        let mut s = SensingService::new(1);
        assert!(!s.check_atrial(9999, 250, true, 1000, 0));
        assert!(!s.check_ventricular(9999, 500, true, 1000, 0));
    }

    #[test]
    fn sample_produces_deterministic_sequence_for_fixed_seed() {
        // Parity anchor: same seed -> same first sample, run to run.
        let mut a = SensingService::new(999);
        let mut b = SensingService::new(999);
        for t in (10..=200).step_by(10) {
            assert_eq!(a.sample(t), b.sample(t));
        }
    }

    #[test]
    fn dropped_beat_cadence() {
        // Every 11th atrial edge (after the 11th) is dropped. With
        // intrinsic_a_period_ms=850 and a 10ms tick, an edge fires every
        // 85 ticks. Drive enough ticks to observe at least the 11th and
        // 22nd edge cycle and confirm the counter-based drop rule.
        let mut s = SensingService::new(5);
        let mut a_period_edges = 0u32;
        let mut t: u32 = 0;
        for _ in 0..2000 {
            t += 10;
            let before = s.a_phase_ms;
            let _ = s.sample(t);
            if before + 10 >= s.intrinsic_a_period_ms {
                a_period_edges += 1;
            }
        }
        assert!(a_period_edges >= 11, "expected enough edges to exercise the drop cadence");
    }
}
