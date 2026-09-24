//! Shared domain types.
//!
//! MIGRATION SOURCE: `include/pacer.h` (legacy shared header).
//!
//! `pacer.h` defined its typedefs (BYTE/WORD/DWORD/UINT32/...), the
//! `PaceMode_t` enum, `PacerState_t` enum, `EVT_*` bitmask #defines,
//! `LogEntry_t`, `EgmSample_t`, and `NvramParams_t`, plus every global
//! variable extern'd across all 8 modules. This file is the target-side
//! home for the *type* definitions; the globals themselves do not have a
//! direct target-side equivalent (see MIGRATION_NOTES.md #1 — that is the
//! entire point of this migration: Task #3 required designing the shared
//! global state out, not relocating it under a new name).

use std::fmt;

// ---------------------------------------------------------------------
// PaceMode - legacy PaceMode_t (pacer.h) + its string table (modes.c
// s_mode_names[]). Faithfully reproduces the legacy ordering (the C enum
// values 0..11 are load-bearing: eeprom.c persists `mode` as a raw BYTE
// and validates it with `mode >= MODE_COUNT`), so this enum's variant
// order must not be reordered independently of PersistenceService's
// on-disk format (see persistence.rs).
// ---------------------------------------------------------------------
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum PaceMode {
    Aoo = 0,
    Voo = 1,
    Aai = 2,
    Vvi = 3,
    Aat = 4,
    Vvt = 5,
    Aair = 6,
    Vvir = 7,
    Vdd = 8,
    Ddi = 9,
    Ddd = 10,
    Dddr = 11,
}

pub const MODE_COUNT: u8 = 12;

impl PaceMode {
    /// Legacy: `mode_to_string()` in modes.c. Unlike the legacy version,
    /// there is no "???" fallback case to write here: PaceMode is a closed
    /// Rust enum, so an out-of-range mode value cannot exist downstream of
    /// the one place raw integers become a PaceMode (`PaceMode::from_u8`,
    /// used only by PersistenceService at the file-load boundary). This is
    /// the direct structural resolution of Finding #2 (pacer_core.c's
    /// dead fallback branch, reachable only via an out-of-range mode
    /// value) — see MIGRATION_NOTES.md #2.
    pub fn as_str(self) -> &'static str {
        match self {
            PaceMode::Aoo => "AOO",
            PaceMode::Voo => "VOO",
            PaceMode::Aai => "AAI",
            PaceMode::Vvi => "VVI",
            PaceMode::Aat => "AAT",
            PaceMode::Vvt => "VVT",
            PaceMode::Aair => "AAIR",
            PaceMode::Vvir => "VVIR",
            PaceMode::Vdd => "VDD",
            PaceMode::Ddi => "DDI",
            PaceMode::Ddd => "DDD",
            PaceMode::Dddr => "DDDR",
        }
    }

    /// Legacy: `mode_from_string()` in modes.c — case-sensitive linear
    /// scan, unknown strings fall back to VVI. Behavior preserved exactly
    /// (see MIGRATION_NOTES.md #3): a real refactor might make "unknown
    /// mode string" a reportable error instead of a silent VVI fallback,
    /// but that is a behavior change outside this stage's scope (Task #2
    /// governs interfaces/boundaries, not CLI UX fixes) and was not one
    /// of the six findings flagged for architectural resolution.
    pub fn from_str_legacy(s: &str) -> PaceMode {
        for i in 0..MODE_COUNT {
            let m = PaceMode::from_u8(i).expect("0..MODE_COUNT is always valid");
            if m.as_str() == s {
                return m;
            }
        }
        PaceMode::Vvi
    }

    pub fn from_u8(v: u8) -> Option<PaceMode> {
        match v {
            0 => Some(PaceMode::Aoo),
            1 => Some(PaceMode::Voo),
            2 => Some(PaceMode::Aai),
            3 => Some(PaceMode::Vvi),
            4 => Some(PaceMode::Aat),
            5 => Some(PaceMode::Vvt),
            6 => Some(PaceMode::Aair),
            7 => Some(PaceMode::Vvir),
            8 => Some(PaceMode::Vdd),
            9 => Some(PaceMode::Ddi),
            10 => Some(PaceMode::Ddd),
            11 => Some(PaceMode::Dddr),
            _ => None,
        }
    }

    pub fn all() -> [PaceMode; 12] {
        [
            PaceMode::Aoo, PaceMode::Voo, PaceMode::Aai, PaceMode::Vvi,
            PaceMode::Aat, PaceMode::Vvt, PaceMode::Aair, PaceMode::Vvir,
            PaceMode::Vdd, PaceMode::Ddi, PaceMode::Ddd, PaceMode::Dddr,
        ]
    }

    // --- capability predicates: legacy modes.c mode_is_*() functions.
    // Preserved as a single classification match per predicate (same
    // shape as legacy) rather than collapsed into one shared table, to
    // keep this migration a faithful, reviewable translation; a follow-on
    // cleanup pass could unify these, flagged in MIGRATION_NOTES.md #4.

    pub fn is_atrial_paced(self) -> bool {
        matches!(self, PaceMode::Aoo | PaceMode::Aai | PaceMode::Aat | PaceMode::Aair
            | PaceMode::Ddi | PaceMode::Ddd | PaceMode::Dddr)
    }

    pub fn is_ventricular_paced(self) -> bool {
        matches!(self, PaceMode::Voo | PaceMode::Vvi | PaceMode::Vvt | PaceMode::Vvir
            | PaceMode::Vdd | PaceMode::Ddi | PaceMode::Ddd | PaceMode::Dddr)
    }

    pub fn is_atrial_sensed(self) -> bool {
        matches!(self, PaceMode::Aai | PaceMode::Aat | PaceMode::Aair
            | PaceMode::Vdd | PaceMode::Ddi | PaceMode::Ddd | PaceMode::Dddr)
    }

    pub fn is_ventricular_sensed(self) -> bool {
        matches!(self, PaceMode::Vvi | PaceMode::Vvt | PaceMode::Vvir
            | PaceMode::Vdd | PaceMode::Ddi | PaceMode::Ddd | PaceMode::Dddr)
    }

    pub fn is_rate_responsive(self) -> bool {
        matches!(self, PaceMode::Aair | PaceMode::Vvir | PaceMode::Dddr)
    }

    pub fn is_dual_chamber(self) -> bool {
        matches!(self, PaceMode::Vdd | PaceMode::Ddi | PaceMode::Ddd | PaceMode::Dddr)
    }
}

impl fmt::Display for PaceMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

// ---------------------------------------------------------------------
// PacerState - legacy PacerState_t + s_state_names[] (pacer_core.c).
// ST_FAULT is retained as a variant (Verification & Test's characterization
// suite pins it as confirmed-unreachable-but-present state, and main.c's
// CLI-level FAULT check is part of the pinned CLI behavior baseline) —
// see MIGRATION_NOTES.md #5 on why this one enum variant stays even
// though nothing in PacingEngine's new state machine can ever produce it.
// ---------------------------------------------------------------------
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum PacerState {
    Init,
    WaitLrl,
    AviWait,
    Fault,
}

impl PacerState {
    pub fn name(self) -> &'static str {
        match self {
            PacerState::Init => "INIT",
            PacerState::WaitLrl => "WAIT_LRL",
            PacerState::AviWait => "AVI_WAIT",
            PacerState::Fault => "FAULT",
        }
    }
}

// ---------------------------------------------------------------------
// EventFlags - legacy EVT_* bitmask #defines (pacer.h). Reproduced as a
// typed bitmask newtype (no external `bitflags` crate — see
// Cargo.toml's zero-dependency note) rather than a raw integer, so a
// caller cannot pass an arbitrary int where a flag set is expected.
// ---------------------------------------------------------------------
#[derive(Copy, Clone, Debug, PartialEq, Eq, Default)]
pub struct EventFlags(u8);

impl EventFlags {
    pub const NONE: EventFlags = EventFlags(0x00);
    pub const A_PACE: EventFlags = EventFlags(0x01);
    pub const A_SENSE: EventFlags = EventFlags(0x02);
    pub const V_PACE: EventFlags = EventFlags(0x04);
    pub const V_SENSE: EventFlags = EventFlags(0x08);
    #[allow(dead_code)]
    pub const A_REFRACTORY: EventFlags = EventFlags(0x10);
    #[allow(dead_code)]
    pub const V_REFRACTORY: EventFlags = EventFlags(0x20);
    pub const NOISE: EventFlags = EventFlags(0x40);
    pub const MODE_SWITCH: EventFlags = EventFlags(0x80);

    pub fn contains(self, other: EventFlags) -> bool {
        (self.0 & other.0) == other.0 && other.0 != 0
    }

    pub fn is_none(self) -> bool {
        self.0 == 0
    }

    pub fn raw(self) -> u8 {
        self.0
    }

    pub fn from_raw(v: u8) -> EventFlags {
        EventFlags(v)
    }

    /// Legacy: telemetry.c's static `evt_flag_desc()` — describes only
    /// the single most "interesting" bit set, fixed priority order,
    /// silently drops other set bits. Behavior preserved exactly
    /// (characterization-tested quirk, not treated as a bug to fix here).
    pub fn describe(self) -> String {
        let mut parts: Vec<&str> = Vec::new();
        if self.contains(EventFlags::MODE_SWITCH) { parts.push("MODE_SWITCH"); }
        if self.contains(EventFlags::A_PACE) { parts.push("A_PACE"); }
        if self.contains(EventFlags::V_PACE) { parts.push("V_PACE"); }
        if self.contains(EventFlags::A_SENSE) { parts.push("A_SENSE"); }
        if self.contains(EventFlags::V_SENSE) { parts.push("V_SENSE"); }
        if self.contains(EventFlags::NOISE) { parts.push("NOISE"); }
        if parts.is_empty() {
            "NONE".to_string()
        } else {
            let mut s = parts.join(" ");
            s.push(' '); // legacy strcat() left a trailing space on every non-empty case
            s
        }
    }
}

impl std::ops::BitOr for EventFlags {
    type Output = EventFlags;
    fn bitor(self, rhs: EventFlags) -> EventFlags {
        EventFlags(self.0 | rhs.0)
    }
}

impl std::ops::BitOrAssign for EventFlags {
    fn bitor_assign(&mut self, rhs: EventFlags) {
        self.0 |= rhs.0;
    }
}

// ---------------------------------------------------------------------
// Channel - NEW type, not present in the legacy C. Structural resolution
// of Finding #6 (battery.c's `lead_impedance_sample(int channel)` /
// `battery_tick`'s implicit channel-0-is-atrial convention, unchecked).
// See MIGRATION_NOTES.md #6 for the explicit behavior-change flag.
// ---------------------------------------------------------------------
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Channel {
    Atrial,
    Ventricular,
}

// ---------------------------------------------------------------------
// LogEvent - legacy LogEntry_t (pacer.h), one entry of the event log
// owned exclusively by TelemetryService (structural resolution of
// Finding #4 — see telemetry.rs).
// ---------------------------------------------------------------------
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct LogEvent {
    pub t_ms: u32,
    pub flags: EventFlags,
    pub a_mv: i16,
    pub v_mv: i16,
    pub mode: PaceMode,
}

// ---------------------------------------------------------------------
// SenseSample - legacy EgmSample_t (pacer.h), as returned by
// SensingService::sample() (sensing.rs).
// ---------------------------------------------------------------------
#[derive(Copy, Clone, Debug, PartialEq, Eq, Default)]
pub struct SenseSample {
    pub a_mv: i16,
    pub v_mv: i16,
    pub t_ms: u32,
}
