//! Coordinator errors. Callers treat these as the commit-or-reject result of
//! a candidate mutation; a rejected mutation must leave the aggregate
//! unchanged.

use super::ids::{GoalAssignmentId, GoalRunId};
use super::model::AgentRef;
use crate::goal::{GoalError, GoalId};
use thiserror::Error;

#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum CoordinatorError {
    #[error("stale coordinator revision: expected {expected}, actual {actual}")]
    StaleRevision { expected: u64, actual: u64 },
    #[error("stale goal revision: expected {expected}, actual {actual}")]
    StaleGoalRevision { expected: u64, actual: u64 },
    #[error("stale run generation: expected {expected}, actual {actual}")]
    StaleGeneration { expected: u64, actual: u64 },
    #[error("agent {target} already has an active goal {goal_id} run {run_id}")]
    SlotOccupied {
        target: AgentRef,
        goal_id: GoalId,
        run_id: GoalRunId,
    },
    #[error("agent {target} is occupied by an external claim ({holder})")]
    ClaimOccupied { target: AgentRef, holder: String },
    #[error("goal not found: {0}")]
    GoalNotFound(GoalId),
    #[error("run not found: {0}")]
    RunNotFound(GoalRunId),
    #[error("assignment not found: {0}")]
    AssignmentNotFound(GoalAssignmentId),
    #[error("queue entry not found for goal {0}")]
    QueueEntryNotFound(GoalId),
    #[error("outbox entry not found: {0}")]
    OutboxNotFound(super::ids::OutboxId),
    #[error("goal dependencies are not ready")]
    DependenciesNotReady,
    #[error("dependency cycle detected")]
    CycleDetected,
    #[error("goal cannot succeed until all required checks pass")]
    ChecksNotSatisfied,
    #[error("approval is required before activation")]
    ApprovalRequired,
    #[error("goal has no checks")]
    NoChecks,
    #[error("goal budget would be exceeded")]
    BudgetExceeded,
    #[error("goal deadline has passed")]
    DeadlineExceeded,
    #[error("a later run already exists for this goal")]
    StaleRun,
    #[error("idempotency key reused with a different payload")]
    IdempotencyConflict,
    #[error("idempotency replay of a recorded error: {0}")]
    IdempotentError(String),
    #[error("daemon epoch must strictly increase (current {current}, requested {requested})")]
    EpochNotMonotonic { current: u64, requested: u64 },
    #[error("outbox entry was superseded and cannot be acknowledged")]
    LateAck,
    #[error("parent yield requires matching parent_run_id and parent_generation")]
    ParentYieldFence,
    #[error("same-agent child requires the parent to yield first")]
    ParentMustYield,
    #[error("activation is gated by an earlier unverified run")]
    VerificationHold,
    #[error("goal depth limit exceeded")]
    DepthLimit,
    #[error("too many live descendants of the root goal")]
    DescendantLimit,
    #[error("duplicate live goal dedupe key")]
    DedupeConflict,
    #[error("invalid coordinator state: {0}")]
    Invalid(String),
    #[error("goal error: {0}")]
    Goal(#[from] GoalError),
    #[error("snapshot error: {0}")]
    Snapshot(String),
}

impl CoordinatorError {
    pub fn from_goal(err: GoalError) -> Self {
        match err {
            GoalError::StaleRevision { expected, actual } => {
                Self::StaleGoalRevision { expected, actual }
            }
            GoalError::ChecksNotSatisfied => Self::ChecksNotSatisfied,
            GoalError::ApprovalRequired => Self::ApprovalRequired,
            GoalError::NoChecks => Self::NoChecks,
            other => Self::Goal(other),
        }
    }

    pub fn retryable(&self) -> bool {
        matches!(
            self,
            Self::StaleRevision { .. }
                | Self::StaleGoalRevision { .. }
                | Self::SlotOccupied { .. }
                | Self::ClaimOccupied { .. }
                | Self::DependenciesNotReady
                | Self::VerificationHold
        )
    }
}
