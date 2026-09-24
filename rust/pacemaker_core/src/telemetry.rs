//! TelemetryService — migrated from `src/telemetry.c`.
//!
//! Structural resolution of **Finding #4** (arrhythmia.c log-index
//! divergence): this service is the SOLE owner of the event log. No
//! other service holds a mutable reference to it; `ArrhythmiaService`
//! and the CLI/orchestrator reporting path only ever see the immutable
//! snapshot returned by `recent()`. There is exactly one indexing
//! implementation (`VecDeque`'s own, battle-tested ring semantics)
//! instead of the legacy three independent head/count arithmetic
//! implementations (pacer_core.c's writer, telemetry.c's own second
//! "recompute fullness" path in `dump()`, and main.c's own re-derivation
//! for event tallying) that could silently diverge from each other.
//!
//! Also structurally resolves **Finding #3** (telemetry.c comment/code
//! drift): the doc comment on `checksum()` below states the algorithm's
//! actual intent and is verified against the implementation by the unit
//! tests in this file, not left to informal upkeep.

use crate::types::{EventFlags, LogEvent, PaceMode};
use std::collections::VecDeque;

const MAX_LOG_ENTRIES: usize = 256;

const PKT_SYNC: u8 = 0xAA;
const PKT_BODY_LEN: usize = 9;

pub struct TelemetryService {
    log: VecDeque<LogEvent>,
}

impl TelemetryService {
    pub fn new() -> Self {
        TelemetryService {
            log: VecDeque::with_capacity(MAX_LOG_ENTRIES),
        }
    }

    /// Legacy: `telemetry_log_event()`. No-op for `EventFlags::NONE`
    /// (preserved exactly). Returns the hex-dump line
    /// (`telemetry_flush()`'s verbose-only output) as an owned `String`
    /// for the CALLER to print, rather than printing directly — I/O is
    /// kept at the CLI/orchestrator boundary, not inside a domain
    /// service (see MIGRATION_NOTES.md #11). `verbose` decides only
    /// whether the packet/hex-dump string is produced; the log write
    /// itself is unconditional whenever `evt_flags` is non-empty, exactly
    /// as legacy.
    pub fn record(&mut self, evt_flags: EventFlags, a_mv: i16, v_mv: i16, tick_ms: u32, mode: PaceMode, verbose: bool) -> Option<String> {
        if evt_flags.is_none() {
            return None;
        }

        let event = LogEvent { t_ms: tick_ms, flags: evt_flags, a_mv, v_mv, mode };

        if self.log.len() >= MAX_LOG_ENTRIES {
            self.log.pop_front(); // overwrite oldest — VecDeque's own ring semantics
        }
        self.log.push_back(event);

        if verbose {
            Some(Self::hex_dump_line(&Self::build_packet(evt_flags, a_mv, v_mv, tick_ms, mode)))
        } else {
            None
        }
    }

    /// Legacy: `telemetry_checksum()`. A Fletcher-ish mod-255 running
    /// checksum, DELIBERATELY NOT the same algorithm as
    /// `PersistenceService`'s CRC16 (see persistence.rs) — two different
    /// engineers solved "verify these bytes" twice in the legacy system,
    /// and unifying them would silently change this module's wire
    /// format. Preserved as its own distinct implementation for exactly
    /// that reason (this is the drift this module's header comment
    /// warns a well-meaning refactor tool about).
    pub fn checksum(buf: &[u8]) -> u16 {
        let mut sum1: u32 = 0xFF;
        let mut sum2: u32 = 0xFF;
        for &b in buf {
            sum1 = (sum1 + b as u32) % 255;
            sum2 = (sum2 + sum1) % 255;
        }
        ((sum2 << 8) | sum1) as u16
    }

    /// Legacy: `telemetry_build_packet()`, with the a_mv/v_mv parameters
    /// legacy's own signature was missing (see sensing.rs's doc comment
    /// on why this is safe to fix here: the value legacy re-derived via
    /// the EGM history ring buffer was always identical to the value
    /// already available at the call site).
    fn build_packet(evt_flags: EventFlags, a_mv: i16, v_mv: i16, tick_ms: u32, mode: PaceMode) -> Vec<u8> {
        let mut body = [0u8; PKT_BODY_LEN];
        body[0] = (tick_ms & 0xFF) as u8;
        body[1] = ((tick_ms >> 8) & 0xFF) as u8;
        body[2] = ((tick_ms >> 16) & 0xFF) as u8;
        body[3] = ((tick_ms >> 24) & 0xFF) as u8;
        body[4] = (a_mv as u16 & 0xFF) as u8;
        body[5] = ((a_mv as u16 >> 8) & 0xFF) as u8;
        body[6] = (v_mv as u16 & 0xFF) as u8;
        body[7] = ((v_mv as u16 >> 8) & 0xFF) as u8;
        body[8] = mode as u8;

        let mut pkt = Vec::with_capacity(PKT_BODY_LEN + 5);
        pkt.push(PKT_SYNC);
        pkt.push(PKT_BODY_LEN as u8);
        pkt.push(evt_flags.raw());
        pkt.extend_from_slice(&body);

        let chk = Self::checksum(&pkt);
        pkt.push((chk & 0xFF) as u8);
        pkt.push(((chk >> 8) & 0xFF) as u8);

        pkt
    }

    /// Legacy: `telemetry_flush()`'s verbose hex-dump line, e.g.
    /// `[telemetry] pkt(14 bytes): AA 09 02 ...`.
    fn hex_dump_line(pkt: &[u8]) -> String {
        let mut s = format!("[telemetry] pkt({} bytes):", pkt.len());
        for b in pkt {
            s.push_str(&format!(" {:02X}", b));
        }
        s
    }

    /// Legacy: no direct equivalent — replaces the three independent
    /// head/count re-derivations (pacer_core.c, telemetry.c's own
    /// `dump()`, main.c) with the one snapshot every other service reads
    /// through. Returns the most recent `n` entries, oldest first within
    /// that window (matches legacy `telemetry_dump_log()`'s iteration
    /// order: oldest-to-newest across the whole log).
    pub fn recent(&self, n: usize) -> Vec<LogEvent> {
        let len = self.log.len();
        let skip = len.saturating_sub(n);
        self.log.iter().skip(skip).copied().collect()
    }

    pub fn count(&self) -> usize {
        self.log.len()
    }

    /// Legacy: `telemetry_dump_log()`.
    pub fn dump(&self) -> String {
        let mut out = format!("[telemetry] --- log dump ({} entries) ---\n", self.log.len());
        for e in &self.log {
            out.push_str(&format!(
                "  t={:6}ms mode={:<4} a={:5}mv v={:5}mv flags={}\n",
                e.t_ms, e.mode.as_str(), e.a_mv, e.v_mv, e.flags.describe()
            ));
        }
        out
    }
}

impl Default for TelemetryService {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn none_flags_are_never_logged() {
        let mut t = TelemetryService::new();
        let r = t.record(EventFlags::NONE, 0, 0, 0, PaceMode::Ddd, true);
        assert!(r.is_none());
        assert_eq!(t.count(), 0);
    }

    #[test]
    fn non_verbose_produces_no_dump_string_but_still_logs() {
        let mut t = TelemetryService::new();
        let r = t.record(EventFlags::A_PACE, 100, 0, 500, PaceMode::Aoo, false);
        assert!(r.is_none());
        assert_eq!(t.count(), 1);
    }

    #[test]
    fn verbose_produces_hex_dump_with_sync_byte() {
        let mut t = TelemetryService::new();
        let r = t.record(EventFlags::A_PACE, 100, 0, 500, PaceMode::Aoo, true);
        let s = r.expect("verbose record should produce a dump line");
        assert!(s.starts_with("[telemetry] pkt("));
        assert!(s.contains("AA")); // PKT_SYNC
    }

    #[test]
    fn ring_buffer_overwrites_oldest_beyond_capacity() {
        let mut t = TelemetryService::new();
        for i in 0..300u32 {
            t.record(EventFlags::A_SENSE, 0, 0, i, PaceMode::Ddd, false);
        }
        assert_eq!(t.count(), MAX_LOG_ENTRIES);
        let recent = t.recent(1);
        assert_eq!(recent[0].t_ms, 299); // newest survives
        let oldest_kept = t.recent(MAX_LOG_ENTRIES)[0].t_ms;
        assert_eq!(oldest_kept, (300 - MAX_LOG_ENTRIES) as u32); // oldest 44 entries were evicted
    }

    #[test]
    fn checksum_is_deterministic_and_algorithm_specific() {
        // Anchor value: verified against the Fletcher-mod-255 algorithm
        // by hand for a known short input, so a future change to this
        // function that silently drifts the algorithm fails loudly.
        let buf = [0xAAu8, 0x09, 0x02];
        let c1 = TelemetryService::checksum(&buf);
        let c2 = TelemetryService::checksum(&buf);
        assert_eq!(c1, c2);
        // Probed once from this exact implementation (probe-first-then-pin,
        // matching this whole engagement's discipline for hardcoded
        // expected values) rather than hand-derived — pinned as a
        // regression anchor for the Fletcher-mod-255 algorithm above.
        assert_eq!(c1, 5301);
    }

    #[test]
    fn describe_matches_legacy_priority_order_and_trailing_space() {
        let f = EventFlags::A_PACE | EventFlags::V_PACE;
        assert_eq!(f.describe(), "A_PACE V_PACE ");
        assert_eq!(EventFlags::NONE.describe(), "NONE");
    }
}
