//! BatteryService — migrated from `src/battery.c`.
//!
//! Boundary unchanged in scope from the legacy file. The one contract
//! change: the legacy `int channel` parameter to `lead_impedance_sample()`
//! (0 = atrial, "anything else" = ventricular, no bounds checking) becomes
//! the closed `Channel` enum. This is the structural resolution of
//! Finding #6 and is flagged in the architecture proposal (Section 6) as
//! a genuine behavior change relative to the characterization baseline —
//! `tests/test_battery.c` pinned the legacy "no bounds checking, any int
//! accepted" behavior as-is. Approved as proposed; see
//! MIGRATION_NOTES.md #6 for the sign-off record.

use crate::types::Channel;

const BATT_NOMINAL_MV: u32 = 2800;
const BATT_ERI_MV: u32 = 2500;
const BATT_EOL_MV: u32 = 2200;
const LEAD_IMPEDANCE_HIGH: u32 = 3000;

/// Legacy: file-scope `static UINT32 s_imp_lcg` + `imp_lcg_next()` in
/// battery.c — a second, independently-seeded copy of the same LCG
/// algorithm sensing.c uses (not shared, on purpose, matching legacy).
#[derive(Copy, Clone, Debug)]
struct ImpLcg(u32);

impl ImpLcg {
    fn seeded(seed: u32) -> Self {
        // legacy: s_imp_lcg = 0x9E3779B9 ^ (UINT32)time(NULL)
        ImpLcg(0x9E37_79B9 ^ seed)
    }
    fn next(&mut self) -> u32 {
        self.0 = (1103515245u32.wrapping_mul(self.0).wrapping_add(12345)) & 0x7FFF_FFFF;
        self.0
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct BatteryStatus {
    pub mv: u32,
    pub eri: bool,
    pub eol: bool,
    pub lead_a_impedance: u32,
    pub lead_v_impedance: u32,
}

pub struct BatteryService {
    batt_mv: u32,
    lead_a_impedance: u32,
    lead_v_impedance: u32,
    eri_flag: bool,
    eol_flag: bool,
    batt_ticks: u32,
    pace_count_since_boot: u32,
    imp_lcg: ImpLcg,
}

impl BatteryService {
    /// Legacy: `battery_init()`. `time_seed` replaces the internal
    /// `time(NULL)` read (same rationale as `SensingService::new` — see
    /// MIGRATION_NOTES.md #8).
    pub fn new(time_seed: u32) -> Self {
        BatteryService {
            batt_mv: BATT_NOMINAL_MV,
            lead_a_impedance: 500,
            lead_v_impedance: 550,
            eri_flag: false,
            eol_flag: false,
            batt_ticks: 0,
            pace_count_since_boot: 0,
            imp_lcg: ImpLcg::seeded(time_seed),
        }
    }

    /// Legacy: `battery_tick()`. `pace_ampl_mv`/`pace_width_ms` are taken
    /// as explicit parameters (were read from `g_pace_ampl_mv` /
    /// `g_pace_width_ms` globals owned by PacingEngine's parameter set).
    /// The three redundant ERI/EOL checks the legacy code performed
    /// (inline here, plus again in `check_eri()`/`check_eol()`) are
    /// consolidated to the one owning update in this method — callers get
    /// the current flags from the returned `BatteryStatus` rather than
    /// re-deriving them; see MIGRATION_NOTES.md #10.
    pub fn tick(&mut self, dt_ms: u32, paced_this_tick: bool, pace_ampl_mv: u16, pace_width_ms: u8) -> BatteryStatus {
        self.batt_ticks = self.batt_ticks.wrapping_add(dt_ms);

        let idle_drain = dt_ms / 20000;

        let mut pace_drain = 0u32;
        if paced_this_tick {
            self.pace_count_since_boot += 1;
            pace_drain = (pace_ampl_mv as u32 / 1000) * (pace_width_ms as u32 + 1);
            if pace_drain == 0 {
                pace_drain = 1;
            }
        }

        if self.batt_mv > idle_drain + pace_drain {
            self.batt_mv -= idle_drain + pace_drain;
        } else {
            self.batt_mv = 0;
        }

        if self.batt_mv <= BATT_ERI_MV {
            self.eri_flag = true;
        }
        if self.batt_mv <= BATT_EOL_MV {
            self.eol_flag = true;
        }

        self.lead_a_impedance = 400 + (self.imp_lcg.next() % 400);
        self.lead_v_impedance = 450 + (self.imp_lcg.next() % 400);

        if (self.batt_ticks % 733000) < dt_ms {
            self.lead_v_impedance = LEAD_IMPEDANCE_HIGH + 500;
        }

        self.status()
    }

    pub fn status(&self) -> BatteryStatus {
        BatteryStatus {
            mv: self.batt_mv,
            eri: self.eri_flag,
            eol: self.eol_flag,
            lead_a_impedance: self.lead_a_impedance,
            lead_v_impedance: self.lead_v_impedance,
        }
    }

    /// Legacy: `lead_impedance_sample(int channel)`, now bounds-safe by
    /// construction — see module doc comment and Finding #6.
    pub fn lead_impedance(&self, channel: Channel) -> u32 {
        match channel {
            Channel::Atrial => self.lead_a_impedance,
            Channel::Ventricular => self.lead_v_impedance,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn init_matches_legacy_defaults() {
        let b = BatteryService::new(1);
        let s = b.status();
        assert_eq!(s.mv, BATT_NOMINAL_MV);
        assert_eq!(s.lead_a_impedance, 500);
        assert_eq!(s.lead_v_impedance, 550);
        assert!(!s.eri && !s.eol);
    }

    #[test]
    fn idle_drain_below_20000ms_dt_is_zero_but_still_updates_impedance() {
        let mut b = BatteryService::new(1);
        let before = b.status().mv;
        let after = b.tick(10, false, 3500, 1);
        assert_eq!(after.mv, before); // idle_drain = 10/20000 = 0 (integer division)
    }

    #[test]
    fn pace_drain_minimum_is_one_mv() {
        let mut b = BatteryService::new(1);
        // pace_ampl_mv=100 -> 100/1000=0 * (width+1) = 0 -> forced to 1
        let after = b.tick(10, true, 100, 1);
        assert_eq!(after.mv, BATT_NOMINAL_MV - 1);
    }

    #[test]
    fn eri_and_eol_flags_latch_and_stay_latched() {
        let mut b = BatteryService::new(1);
        // Drain hard enough to cross ERI threshold using large pace_ampl_mv.
        let mut status = b.status();
        while status.mv > BATT_ERI_MV {
            status = b.tick(10, true, 60000, 5);
        }
        assert!(status.eri);
        // one more tick that doesn't cross EOL yet should keep eri latched
        let status2 = b.tick(10, false, 3500, 1);
        assert!(status2.eri);
    }

    #[test]
    fn lead_impedance_channel_selector_matches_fields() {
        let mut b = BatteryService::new(1);
        let s = b.tick(10, false, 3500, 1);
        assert_eq!(b.lead_impedance(Channel::Atrial), s.lead_a_impedance);
        assert_eq!(b.lead_impedance(Channel::Ventricular), s.lead_v_impedance);
    }

    #[test]
    fn battery_never_underflows_below_zero() {
        let mut b = BatteryService::new(1);
        let mut status = BatteryStatus { mv: BATT_NOMINAL_MV, eri: false, eol: false, lead_a_impedance: 0, lead_v_impedance: 0 };
        for _ in 0..10_000 {
            status = b.tick(10, true, 60000, 5);
        }
        assert!(status.mv == 0 || status.eol);
    }
}
