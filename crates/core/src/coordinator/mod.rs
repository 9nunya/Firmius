//! Durable goal coordinator: exclusive agent slots, queues, fenced runs,
//! outbox, and restart reconciliation. The aggregate is pure; callers clone a
//! candidate, apply one revisioned mutation, and persist only after it is
//! accepted. The daemon owns persistence and dispatch.
//!
//! # Public API inventory
//!
//! ## Aggregate lifecycle & snapshotting
//! - [`GoalCoordinator::new`], [`GoalCoordinator::to_snapshot`],
//!   [`GoalCoordinator::from_snapshot`], [`GoalCoordinator::validate`]
//!
//! ## Read-only queries
//! - [`GoalCoordinator::goal`], [`GoalCoordinator::run`],
//!   [`GoalCoordinator::assignment`], [`GoalCoordinator::slot`],
//!   [`GoalCoordinator::open_run`], [`GoalCoordinator::queue_for`],
//!   [`GoalCoordinator::pending_outbox`], [`GoalCoordinator::is_idle`],
//!   [`GoalCoordinator::occupancy`], [`GoalCoordinator::admit`],
//!   [`GoalCoordinator::steps_consumed`], [`GoalCoordinator::cost_consumed`]
//!
//! ## Scheduling commands
//! - [`GoalCoordinator::register`], [`GoalCoordinator::enqueue`],
//!   [`GoalCoordinator::activate`], [`GoalCoordinator::activate_next`],
//!   [`GoalCoordinator::activate_next_at`]
//!
//! ## Execution commands (all fenced by `run_id` + `generation`)
//! - [`GoalCoordinator::yield_run`], [`GoalCoordinator::complete`],
//!   [`GoalCoordinator::record_step`], [`GoalCoordinator::heartbeat`],
//!   [`GoalCoordinator::cancel`], [`GoalCoordinator::settle_cancel`]
//!
//! ## Recovery & bookkeeping
//! - [`GoalCoordinator::fence_expired`], [`GoalCoordinator::reconcile`],
//!   [`GoalCoordinator::acknowledge_outbox`], [`GoalCoordinator::add_dependency`],
//!   [`GoalCoordinator::apply_goal_transition`],
//!   [`GoalCoordinator::claim_occupancy`], [`GoalCoordinator::release_claim`]
//!
//! ## Domain records
//! - [`model::*`] exports scheduling records (`AgentRef`, `PriorityClass`,
//!   `GoalAssignment`, `GoalQueueEntry`, `AgentGoalSlot`, `AgentOccupancy`,
//!   `GoalRun`, `GoalRunState`, `GoalDependency`, `OutboxEntry`, etc.),
//!   command inputs (`EnqueueSpec`, `EnqueueResult`, `ActivateSpec`,
//!   `YieldSpec`, `CompleteSpec`, `CancelSpec`, `ReconcileReport`), and
//!   idempotency records.
//! - [`ids::GoalAssignmentId`], [`ids::GoalQueueEntryId`],
//!   [`ids::GoalRunId`], [`ids::GoalDependencyId`], [`ids::OutboxId`]
//!
//! ## Errors
//! - [`CoordinatorError`] is the commit-or-reject result of a mutation. A
//!   rejected mutation leaves the aggregate byte-for-byte unchanged.

mod coordinator;
mod error;
mod ids;
mod model;

pub use coordinator::GoalCoordinator;
pub use error::CoordinatorError;
pub use ids::{GoalAssignmentId, GoalDependencyId, GoalQueueEntryId, GoalRunId, OutboxId};
pub use model::*;

#[cfg(test)]
mod tests;
