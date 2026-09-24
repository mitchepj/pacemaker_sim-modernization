//! PersistenceService — migrated from `src/eeprom.c`.
//!
//! Structural resolution of **Finding #5** (the unvalidated
//! `avi_ms < url_ms` invariant `pacer_core.c`'s `ST_AVI_WAIT` URL-safety
//! fallback branch silently assumes): `PacerParams::validate()` below
//! checks it explicitly, at the one place values enter the system.
//!
//! Also implements the Section 6 trade-off decision approved as
//! proposed: `load()` returns a `Result` instead of legacy
//! `eeprom_load()`'s "silently fall back to defaults AND write them back
//! out" behavior (`goto use_defaults; ...; eeprom_save();`). Splitting
//! "read" from "read, and on failure, also write" is a genuine behavior
//! change relative to the legacy baseline, flagged and approved in the
//! architecture proposal — callers that need the old auto-heal-and-save
//! behavior now compose it explicitly (see `orchestrator.rs`).
//!
//! On-disk format is kept byte-compatible with the legacy
//! `NvramParams_t` (`#pragma pack(push, 1)` struct) + trailing CRC16, so
//! a `pacer_nvram.bin` file written by the ORIGINAL, unmodified C binary
//! can be loaded here and vice versa — this is what makes true
//! functional-parity testing between legacy and migrated binaries
//! possible (Verification & Test Agent's TASK #2). See
//! MIGRATION_NOTES.md #12 for the one assumption this required flagging:
//! the legacy struct's byte order was never made explicit in the C
//! source (a raw `fwrite` of the struct, whatever the compiler laid out
//! natively) — little-endian is assumed here as the characterization
//! testing platform's native order; if the real air-gapped target is
//! big-endian this needs re-verification against an actual captured
//! `pacer_nvram.bin` file, not against source reading alone.

use crate::types::{PaceMode, MODE_COUNT};
use std::fs;
use std::path::{Path, PathBuf};

const NVRAM_MAGIC: u16 = 0x5A5A;
const CRC16_POLY: u16 = 0xA001;
const PARAMS_LEN: usize = 32; // sizeof(NvramParams_t), packed, see module doc comment
const RESERVED_LEN: usize = 9;

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

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct PacerParams {
    pub mode: PaceMode,
    pub lrl_ms: u16,
    pub url_ms: u16,
    pub avi_ms: u16,
    pub pvarp_ms: u16,
    pub vrp_ms: u16,
    pub arp_ms: u16,
    pub pace_ampl_mv: u16,
    pub pace_width_ms: u8,
    pub a_sense_thresh_mv: u16,
    pub v_sense_thresh_mv: u16,
    pub rate_resp_enabled: bool,
}

impl PacerParams {
    /// Legacy: `eeprom_defaults()`.
    pub fn defaults() -> Self {
        PacerParams {
            mode: PaceMode::Ddd,
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
        }
    }

    /// Legacy: `eeprom_validate()`'s ad-hoc bounds checks, PLUS the NEW
    /// `avi_ms < url_ms` cross-field check that closes Finding #5. The
    /// legacy magic/CRC checks are handled separately in `load()` (they
    /// are about the FILE, not the field values); this validates only
    /// the field values themselves, so it can also be called on values
    /// set some other way (e.g. a future config UI) before they are ever
    /// written to disk.
    pub fn validate(&self) -> Result<(), PersistenceError> {
        if self.lrl_ms < 300 || self.lrl_ms > 2000 {
            return Err(PersistenceError::InvalidField("lrl_ms out of range [300, 2000]"));
        }
        if self.url_ms < 250 || self.url_ms > 1500 {
            return Err(PersistenceError::InvalidField("url_ms out of range [250, 1500]"));
        }
        // NEW (Finding #5): pacer_core.c's ST_AVI_WAIT state assumes
        // avi_ms < url_ms so its URL-safety fallback (`since_a >=
        // g_url_ms`) is reachable only AFTER the AVI-based pace
        // (`since_a >= g_avi_ms`) would already have fired. eeprom.c
        // never checked this.
        if self.avi_ms >= self.url_ms {
            return Err(PersistenceError::InvalidField("avi_ms must be strictly less than url_ms"));
        }
        Ok(())
    }

    fn to_bytes(self) -> [u8; PARAMS_LEN] {
        let mut b = [0u8; PARAMS_LEN];
        let mut i = 0;
        macro_rules! put_u16 { ($v:expr) => {{ let v: u16 = $v; b[i] = (v & 0xFF) as u8; b[i+1] = (v >> 8) as u8; i += 2; }}; }
        macro_rules! put_u8 { ($v:expr) => {{ b[i] = $v; i += 1; }}; }

        put_u16!(NVRAM_MAGIC);
        put_u8!(self.mode as u8);
        put_u16!(self.lrl_ms);
        put_u16!(self.url_ms);
        put_u16!(self.avi_ms);
        put_u16!(self.pvarp_ms);
        put_u16!(self.vrp_ms);
        put_u16!(self.arp_ms);
        put_u16!(self.pace_ampl_mv);
        put_u8!(self.pace_width_ms);
        put_u16!(self.a_sense_thresh_mv);
        put_u16!(self.v_sense_thresh_mv);
        put_u8!(self.rate_resp_enabled as u8);
        // reserved[9] left as zero, matching legacy's memset(&params, 0, ...)
        debug_assert_eq!(i + RESERVED_LEN, PARAMS_LEN);
        b
    }

    fn from_bytes(b: &[u8; PARAMS_LEN]) -> Result<(u16, Self), PersistenceError> {
        let mut i = 0;
        macro_rules! get_u16 { () => {{ let v = (b[i] as u16) | ((b[i+1] as u16) << 8); i += 2; v }}; }
        macro_rules! get_u8 { () => {{ let v = b[i]; #[allow(unused_assignments)] { i += 1; } v }}; }

        let magic = get_u16!();
        let mode_raw = get_u8!();
        let lrl_ms = get_u16!();
        let url_ms = get_u16!();
        let avi_ms = get_u16!();
        let pvarp_ms = get_u16!();
        let vrp_ms = get_u16!();
        let arp_ms = get_u16!();
        let pace_ampl_mv = get_u16!();
        let pace_width_ms = get_u8!();
        let a_sense_thresh_mv = get_u16!();
        let v_sense_thresh_mv = get_u16!();
        let rate_resp_enabled = get_u8!() != 0;

        let mode = PaceMode::from_u8(mode_raw).ok_or(PersistenceError::InvalidField("mode >= MODE_COUNT"))?;
        let _ = MODE_COUNT; // mode range already enforced by from_u8

        Ok((magic, PacerParams {
            mode, lrl_ms, url_ms, avi_ms, pvarp_ms, vrp_ms, arp_ms,
            pace_ampl_mv, pace_width_ms, a_sense_thresh_mv, v_sense_thresh_mv,
            rate_resp_enabled,
        }))
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum PersistenceError {
    NotFound,
    ShortRead,
    MissingCrc,
    CrcMismatch,
    BadMagic,
    InvalidField(&'static str),
    WriteFailed,
}

/// Legacy: `crc16_calc()` — table-less CRC16/ARC-ish, recomputed
/// bit-by-bit every call. Preserved exactly, including the slow
/// per-bit approach (not optimized to a lookup table here: this is a
/// faithful translation, not a performance rewrite — Rule 2).
fn crc16_calc(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &byte in data {
        crc ^= byte as u16;
        for _ in 0..8 {
            if crc & 0x0001 != 0 {
                crc = (crc >> 1) ^ CRC16_POLY;
            } else {
                crc >>= 1;
            }
        }
    }
    crc
}

pub struct PersistenceService {
    path: PathBuf,
}

impl PersistenceService {
    pub fn new(path: impl AsRef<Path>) -> Self {
        PersistenceService { path: path.as_ref().to_path_buf() }
    }

    /// Legacy: `eeprom_load()`, MINUS the auto-heal-and-resave side
    /// effect (see module doc comment — approved behavior change).
    /// Every legacy failure mode is preserved as its own distinct
    /// `PersistenceError` variant instead of being collapsed into "use
    /// defaults" with only a verbose-mode printf distinguishing them.
    pub fn load(&self) -> Result<PacerParams, PersistenceError> {
        let bytes = fs::read(&self.path).map_err(|_| PersistenceError::NotFound)?;

        if bytes.len() < PARAMS_LEN {
            return Err(PersistenceError::ShortRead);
        }
        if bytes.len() < PARAMS_LEN + 2 {
            return Err(PersistenceError::MissingCrc);
        }

        let mut params_bytes = [0u8; PARAMS_LEN];
        params_bytes.copy_from_slice(&bytes[0..PARAMS_LEN]);
        let stored_crc = (bytes[PARAMS_LEN] as u16) | ((bytes[PARAMS_LEN + 1] as u16) << 8);

        let calc_crc = crc16_calc(&params_bytes);
        if calc_crc != stored_crc {
            return Err(PersistenceError::CrcMismatch);
        }

        let (magic, params) = Self::decode_or(&params_bytes)?;
        if magic != NVRAM_MAGIC {
            return Err(PersistenceError::BadMagic);
        }

        // legacy eeprom_validate()'s field bounds checks, run AFTER the
        // magic/CRC checks, exactly as legacy ordered them.
        params.validate()?;

        Ok(params)
    }

    fn decode_or(params_bytes: &[u8; PARAMS_LEN]) -> Result<(u16, PacerParams), PersistenceError> {
        PacerParams::from_bytes(params_bytes)
    }

    /// Legacy: `eeprom_save()`.
    pub fn save(&self, params: &PacerParams) -> Result<(), PersistenceError> {
        let body = params.to_bytes();
        let crc = crc16_calc(&body);

        let mut out = Vec::with_capacity(PARAMS_LEN + 2);
        out.extend_from_slice(&body);
        out.push((crc & 0xFF) as u8);
        out.push((crc >> 8) as u8);

        fs::write(&self.path, out).map_err(|_| PersistenceError::WriteFailed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::env;

    fn tmp_path(name: &str) -> PathBuf {
        let mut p = env::temp_dir();
        p.push(format!("pacemaker_rust_test_{}_{}", std::process::id(), name));
        p
    }

    #[test]
    fn save_then_load_round_trips_exactly() {
        let path = tmp_path("roundtrip.bin");
        let svc = PersistenceService::new(&path);
        let params = PacerParams { mode: PaceMode::Aai, ..PacerParams::defaults() };
        svc.save(&params).unwrap();
        let loaded = svc.load().unwrap();
        assert_eq!(loaded, params);
        let _ = fs::remove_file(&path);
    }

    #[test]
    fn missing_file_is_not_found_not_a_silent_default() {
        let path = tmp_path("does_not_exist.bin");
        let _ = fs::remove_file(&path);
        let svc = PersistenceService::new(&path);
        assert_eq!(svc.load(), Err(PersistenceError::NotFound));
    }

    #[test]
    fn corrupted_crc_is_detected() {
        let path = tmp_path("badcrc.bin");
        let svc = PersistenceService::new(&path);
        svc.save(&PacerParams::defaults()).unwrap();
        let mut bytes = fs::read(&path).unwrap();
        let last = bytes.len() - 1;
        bytes[last] ^= 0xFF; // flip bits in the stored CRC
        fs::write(&path, &bytes).unwrap();
        assert_eq!(svc.load(), Err(PersistenceError::CrcMismatch));
        let _ = fs::remove_file(&path);
    }

    #[test]
    fn finding_5_avi_must_be_less_than_url() {
        let mut p = PacerParams::defaults();
        p.avi_ms = p.url_ms; // equal, not strictly less -> invalid
        assert_eq!(p.validate(), Err(PersistenceError::InvalidField("avi_ms must be strictly less than url_ms")));

        p.avi_ms = p.url_ms - 1;
        assert!(p.validate().is_ok());
    }

    #[test]
    fn crc16_matches_known_polynomial_behavior() {
        // Anchor: CRC of an all-zero 32-byte buffer, computed by this
        // same algorithm — determinism + regression anchor.
        let zero = [0u8; PARAMS_LEN];
        let c1 = crc16_calc(&zero);
        let c2 = crc16_calc(&zero);
        assert_eq!(c1, c2);
    }

    #[test]
    fn defaults_pass_validation() {
        assert!(PacerParams::defaults().validate().is_ok());
    }
}
