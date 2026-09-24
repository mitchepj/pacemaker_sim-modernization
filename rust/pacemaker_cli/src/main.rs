//! pacemaker_sim CLI — migrated from `src/main.c`, MINUS the tick loop
//! (now `pacemaker_core::orchestrator::SimulationOrchestrator::run()`).
//! This binary owns only: argument parsing, the startup banner and
//! usage text, and final-report formatting — exactly the "CLI/Driver"
//! boundary from the architecture proposal (Section 2.2).
//!
//! Usage (unchanged from legacy): pacemaker_sim [MODE] [DURATION_MS] [-v]

use std::io::{self, Write};

use pacemaker_core::{PaceMode, SimulationOrchestrator};
use pacemaker_core::persistence::PersistenceService;

const FW_MAJOR: u32 = 4;
const FW_MINOR: u32 = 12;
const FW_PATCH: u32 = 3;
const FW_BUILD_DATE: &str = "2011-06-30"; // kept stale on purpose, matching legacy pacer.h

const DEFAULT_DURATION_MS: u32 = 10000;
const NVRAM_FILE: &str = "pacer_nvram.bin";

fn print_banner<W: Write>(out: &mut W) -> io::Result<()> {
    writeln!(out, "======================================================")?;
    writeln!(out, " pacemaker_sim - FAKE / SYNTHETIC pacemaker simulator")?;
    writeln!(out, " fw {}.{}.{} (build {}) - NOT A REAL MEDICAL DEVICE", FW_MAJOR, FW_MINOR, FW_PATCH, FW_BUILD_DATE)?;
    writeln!(out, " see README.md - this is a legacy-code refactoring")?;
    writeln!(out, " benchmark fixture only.")?;
    writeln!(out, "======================================================")
}

fn print_usage<W: Write>(out: &mut W, prog: &str) -> io::Result<()> {
    writeln!(out, "usage: {} [MODE] [DURATION_MS] [-v]", prog)?;
    write!(out, "  MODE:          one of ")?;
    for m in PaceMode::all() {
        write!(out, "{} ", m.as_str())?;
    }
    writeln!(out)?;
    writeln!(out, "  DURATION_MS:   how many simulated milliseconds to run (default {})", DEFAULT_DURATION_MS)?;
    writeln!(out, "  -v:            verbose (per-event + telemetry hex dump logging)")
}

struct ParsedArgs {
    mode: Option<PaceMode>,
    duration_ms: u32,
    verbose: bool,
    help: bool,
}

/// Legacy: `main()`'s hand-rolled argv scan — positional args consumed
/// in order (mode, then duration), `-v`/`-h`/`--help` recognized
/// anywhere. Preserved exactly, including the "unknown mode name falls
/// back to VVI with no error" and "non-numeric/zero duration falls back
/// to the 10000ms default" characterized quirks (see
/// `PaceMode::from_str_legacy` in pacemaker_core::types).
fn parse_args(argv: &[String]) -> ParsedArgs {
    let mut mode: Option<PaceMode> = None;
    let mut duration_ms = DEFAULT_DURATION_MS;
    let mut verbose = false;
    let mut got_mode = false;
    let mut got_duration = false;

    for arg in argv {
        if arg == "-v" {
            verbose = true;
        } else if arg == "-h" || arg == "--help" {
            return ParsedArgs { mode: None, duration_ms, verbose, help: true };
        } else if !got_mode {
            mode = Some(PaceMode::from_str_legacy(arg));
            got_mode = true;
        } else if !got_duration {
            let parsed: u32 = arg.parse().unwrap_or(0);
            duration_ms = if parsed == 0 { DEFAULT_DURATION_MS } else { parsed };
            got_duration = true;
        }
        // extra unrecognized trailing args: silently ignored, matching legacy
    }

    ParsedArgs { mode, duration_ms, verbose, help: false }
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let prog = argv.first().cloned().unwrap_or_else(|| "pacemaker_sim".to_string());
    let stdout = io::stdout();
    let mut out = stdout.lock();

    let parsed = parse_args(&argv[1..]);

    if parsed.help {
        print_usage(&mut out, &prog).ok();
        return;
    }

    print_banner(&mut out).ok();

    let mut orch = SimulationOrchestrator::new_from_time(PaceMode::Ddd, parsed.verbose);
    let persistence = PersistenceService::new(NVRAM_FILE);

    // Legacy: pacer_core_init() (mode=DDD baseline) then eeprom_load(),
    // which may overwrite mode with whatever was previously persisted.
    orch.load_or_initialize(&persistence);

    // Legacy: "eeprom_load() may have overwritten g_mode with whatever
    // was persisted from a previous run; if the user explicitly asked
    // for a mode on the command line, re-apply it now." — main.c's own
    // comment flags this ordering as a classic source of subtle bugs;
    // kept exactly, seam now named explicitly (see
    // SimulationOrchestrator::override_mode).
    if let Some(requested_mode) = parsed.mode {
        orch.override_mode(requested_mode);
    }

    writeln!(
        out,
        "[main] starting simulation: mode={} duration={}ms verbose={}",
        orch.mode().as_str(), parsed.duration_ms, parsed.verbose as u8
    ).ok();

    let report = orch.run(parsed.duration_ms, &mut out).expect("stdout write failed");

    let _ = orch.save_params(&persistence);

    writeln!(out, "------------------------------------------------------").ok();
    writeln!(out, "[main] simulation complete at t={}ms", report.final_tick_ms).ok();
    writeln!(
        out,
        "[main] totals: A_PACE={} V_PACE={} A_SENSE={} V_SENSE={}",
        report.totals.a_paces, report.totals.v_paces, report.totals.a_senses, report.totals.v_senses
    ).ok();
    writeln!(
        out,
        "[main] final battery={}mV eri={} eol={} ams_active={}",
        report.battery.mv, report.battery.eri as u8, report.battery.eol as u8, report.ams_active as u8,
    ).ok();

    if parsed.verbose {
        write!(out, "{}", orch.telemetry_dump()).ok();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn help_flag_detected_before_positional_args() {
        let argv = vec!["--help".to_string()];
        let parsed = parse_args(&argv);
        assert!(parsed.help);
    }

    #[test]
    fn unknown_mode_falls_back_to_vvi() {
        let argv = vec!["NOTAREALMODE".to_string(), "200".to_string()];
        let parsed = parse_args(&argv);
        assert_eq!(parsed.mode, Some(PaceMode::Vvi));
        assert_eq!(parsed.duration_ms, 200);
    }

    #[test]
    fn zero_or_non_numeric_duration_falls_back_to_default() {
        let argv = vec!["DDD".to_string(), "0".to_string()];
        let parsed = parse_args(&argv);
        assert_eq!(parsed.duration_ms, DEFAULT_DURATION_MS);

        let argv2 = vec!["DDD".to_string(), "abc".to_string()];
        let parsed2 = parse_args(&argv2);
        assert_eq!(parsed2.duration_ms, DEFAULT_DURATION_MS);
    }

    #[test]
    fn v_flag_works_before_or_after_positional_args() {
        let argv = vec!["-v".to_string(), "DDD".to_string(), "200".to_string()];
        let parsed = parse_args(&argv);
        assert!(parsed.verbose);
        assert_eq!(parsed.mode, Some(PaceMode::Ddd));

        let argv2 = vec!["DDD".to_string(), "200".to_string(), "-v".to_string()];
        let parsed2 = parse_args(&argv2);
        assert!(parsed2.verbose);
    }

    #[test]
    fn extra_trailing_args_are_ignored() {
        let argv = vec!["DDD".to_string(), "200".to_string(), "foo".to_string(), "bar".to_string()];
        let parsed = parse_args(&argv);
        assert_eq!(parsed.mode, Some(PaceMode::Ddd));
        assert_eq!(parsed.duration_ms, 200);
    }
}
