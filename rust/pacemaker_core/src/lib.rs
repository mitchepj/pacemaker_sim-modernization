//! pacemaker_core — migrated `pacemaker_sim` domain services.
//!
//! See `/docs/ARCHITECTURE_AND_DECOUPLING.md` (human-approved, Phase 2
//! gate) for the boundary rationale, and `MIGRATION_NOTES.md` in this
//! crate's parent directory for every deviation from a literal
//! translation and the ambiguities flagged for human clarification.

pub mod arrhythmia;
pub mod battery;
pub mod orchestrator;
pub mod pacing_engine;
pub mod persistence;
pub mod sensing;
pub mod telemetry;
pub mod types;

pub use arrhythmia::{ArrhythmiaDecision, ArrhythmiaService};
pub use battery::{BatteryService, BatteryStatus};
pub use orchestrator::{EventTotals, SimulationOrchestrator, SimulationReport};
pub use pacing_engine::{PacingEngine, StepOutcome};
pub use persistence::{PacerParams, PersistenceError, PersistenceService};
pub use sensing::SensingService;
pub use telemetry::TelemetryService;
pub use types::{Channel, EventFlags, LogEvent, PaceMode, PacerState, SenseSample, MODE_COUNT};
