//! Durable records owned by the goal coordinator aggregate.

use super::ids::*;
use crate::goal::{CheckVerification, GoalActor, GoalId};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

/// Qualified agent identity. Occupancy, dispatch, and fencing are always
/// scoped to `(session_id, agent_id)`, never to a bare agent id.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct AgentRef {
    pub session_id: String,
    pub agent_id: String,
}

/// A fenced candidate result submitted by the worker owning a run.  The
/// coordinator deliberately accepts only worker-level verification claims;
/// reviewer authority is granted only by the independent-check path.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CandidateSpec {
    pub goal_id: GoalId,
    /// Revision of the goal that the candidate was produced against. This
    /// fence is checked as part of the same atomic mutation as submission.
    pub expected_goal_revision: u64,
    pub run_id: GoalRunId,
    pub generation: u64,
    pub actor: GoalActor,
    pub result: serde_json::Value,
    pub evidence: Vec<String>,
    pub artifact_ids: Vec<String>,
    pub work_result_ids: Vec<String>,
    pub verification: crate::goal::CheckVerification,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_request_id: Option<Uuid>,
}

/// Durable message appended to a goal's coordinator journal.  The wire
/// protocol owns the shape of correlation and kind; keeping those values as
/// JSON here lets the aggregate remain independent of protocol crates while
/// preserving them across restart.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalMessage {
    pub id: Uuid,
    pub goal_id: GoalId,
    pub sender: AgentRef,
    pub recipient: AgentRef,
    pub correlation: serde_json::Value,
    pub kind: String,
    pub body: String,
    pub sequence: u64,
    pub created_at: DateTime<Utc>,
}

impl AgentRef {
    pub fn new(session_id: impl Into<String>, agent_id: impl Into<String>) -> Self {
        Self {
            session_id: session_id.into(),
            agent_id: agent_id.into(),
        }
    }

    /// Stable map key. Session and agent ids are kept distinct so two
    /// sessions cannot collide on a reused agent id.
    pub fn key(&self) -> String {
        format!("{}\u{1f}{}", self.session_id, self.agent_id)
    }
}

impl std::fmt::Display for AgentRef {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}/{}", self.session_id, self.agent_id)
    }
}

/// Explicit execution target for a goal. Alias of [`AgentRef`].
pub type GoalTarget = AgentRef;

/// Scheduling class. Lower discriminant is higher urgency. Aging may
/// promote a waiting entry toward [`PriorityClass::User`] but never to
/// [`PriorityClass::UserEmergency`].
#[derive(
    Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize, Default,
)]
#[serde(rename_all = "snake_case")]
pub enum PriorityClass {
    UserEmergency = 0,
    User = 1,
    ParentMilestone = 2,
    Workflow = 3,
    ApprovedFollowUp = 4,
    #[default]
    SelfMaintenance = 5,
}

impl PriorityClass {
    pub fn as_rank(self) -> u8 {
        self as u8
    }

    /// Promote toward user work by `steps` ranks, floored at [`Self::User`].
    pub fn age(self, steps: u8) -> Self {
        if matches!(self, Self::UserEmergency) {
            return self;
        }
        let rank = self
            .as_rank()
            .saturating_sub(steps)
            .max(Self::User.as_rank());
        match rank {
            0 => Self::UserEmergency,
            1 => Self::User,
            2 => Self::ParentMilestone,
            3 => Self::Workflow,
            4 => Self::ApprovedFollowUp,
            _ => Self::SelfMaintenance,
        }
    }
}

/// Why a run released its execution slot without terminating the goal.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum WaitReason {
    Verification,
    Child { child_goal_id: GoalId },
    Approval,
    Timer,
    Resource { name: String },
    Other { reason: String },
}

impl WaitReason {
    pub fn holds_verification_gate(&self) -> bool {
        matches!(self, Self::Verification)
    }
}

/// How child goals react when a parent is cancelled.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum ChildCascadePolicy {
    #[default]
    Cancel,
    Detach,
    Wait,
}

/// Kind of a recorded parent/child (or workflow) wait.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum DependencyKind {
    ChildCompletion,
    WorkGraphResult {
        result_id: String,
    },
    Review,
    /// Protocol-level dependency condition retained durably so Terminal and
    /// outcome predicates are not silently collapsed into success-only edges.
    GoalCondition {
        condition: String,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DependencyState {
    Open,
    Satisfied,
    Failed,
    Cancelled,
}

/// One fenced execution attempt. Step and cost counters never reset.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum GoalRunState {
    Open,
    Waiting,
    CancelRequested,
    Succeeded,
    Failed,
    Interrupted,
    Cancelled,
}

impl GoalRunState {
    pub fn terminal(self) -> bool {
        matches!(
            self,
            Self::Succeeded | Self::Failed | Self::Interrupted | Self::Cancelled
        )
    }

    /// Only an executing or fenced-for-cancel run owns the agent slot.
    pub fn holds_slot(self) -> bool {
        matches!(self, Self::Open | Self::CancelRequested)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalAssignment {
    pub id: GoalAssignmentId,
    pub goal_id: GoalId,
    pub target: AgentRef,
    pub controller: GoalActor,
    pub queue_order: u64,
    pub priority_class: PriorityClass,
    pub priority: i32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_run_id: Option<GoalRunId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_generation: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workflow_node_id: Option<String>,
    #[serde(default)]
    pub retry_safe: bool,
    #[serde(default)]
    pub max_attempts: u32,
    pub created_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub released_at: Option<DateTime<Utc>>,
    pub root_goal_id: GoalId,
    #[serde(default)]
    pub depth: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dedupe_key: Option<String>,
    /// Predecessor goals this new goal waits on before it can activate.
    #[serde(default)]
    pub wait_for: Vec<GoalId>,
}

impl GoalAssignment {
    pub fn live(&self) -> bool {
        self.released_at.is_none()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalQueueEntry {
    pub id: GoalQueueEntryId,
    pub assignment_id: GoalAssignmentId,
    pub goal_id: GoalId,
    pub target: AgentRef,
    pub enqueue_seq: u64,
    pub priority_class: PriorityClass,
    pub priority: i32,
    pub eligible_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub deadline: Option<DateTime<Utc>>,
    pub expected_goal_revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AgentGoalSlot {
    pub target: AgentRef,
    pub goal_id: GoalId,
    pub run_id: GoalRunId,
    pub assignment_id: GoalAssignmentId,
    pub generation: u64,
    pub acquired_at: DateTime<Utc>,
    pub last_heartbeat: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expires_at: Option<DateTime<Utc>>,
    pub daemon_epoch: u64,
}

/// Optional occupancy claimed by WorkGraph (or another exclusive executor)
/// so the coordinator and WorkGraph assignment projection share the
/// one-agent rule. The service must acquire this before starting unrelated
/// live work on the same agent.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExternalClaim {
    pub target: AgentRef,
    pub holder: String,
    pub kind: OccupancyKind,
    pub acquired_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum OccupancyKind {
    Goal,
    WorkGraph,
    External,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "state")]
pub enum AgentOccupancy {
    Idle,
    Goal {
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
    },
    Claimed {
        holder: String,
        kind: OccupancyKind,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalRun {
    pub id: GoalRunId,
    pub goal_id: GoalId,
    pub assignment_id: GoalAssignmentId,
    pub target: AgentRef,
    pub attempt: u32,
    pub generation: u64,
    pub daemon_epoch: u64,
    pub state: GoalRunState,
    pub started_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub finished_at: Option<DateTime<Utc>>,
    pub last_heartbeat: DateTime<Utc>,
    #[serde(default)]
    pub steps_consumed: u32,
    #[serde(default)]
    pub cost_consumed: u64,
    #[serde(default)]
    pub retry_safe: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wait_reason: Option<WaitReason>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub result_summary: Option<String>,
    /// Exact semantic outcome recorded when this run reached a terminal state.
    /// This is deliberately separate from the lifecycle state: a successful
    /// review may produce an application outcome such as `approved`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub outcome: Option<String>,
    /// Candidate payload durably submitted for this run, if any.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub result: Option<serde_json::Value>,
    /// Verification claimed for the durable candidate payload.
    #[serde(default)]
    pub verification: CheckVerification,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalDependency {
    pub id: GoalDependencyId,
    pub parent_goal_id: GoalId,
    pub child_goal_id: GoalId,
    pub kind: DependencyKind,
    pub state: DependencyState,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_run_id: Option<GoalRunId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub child_run_id: Option<GoalRunId>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum OutboxKind {
    Dispatch {
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
        target: AgentRef,
        assignment_id: GoalAssignmentId,
    },
    Cancel {
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
        target: AgentRef,
        assignment_id: GoalAssignmentId,
    },
    MilestoneReady {
        parent_goal_id: GoalId,
        child_goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum OutboxState {
    Pending,
    Acknowledged,
    Superseded,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct OutboxEntry {
    pub id: OutboxId,
    pub kind: OutboxKind,
    pub state: OutboxState,
    pub created_at: DateTime<Utc>,
}

impl OutboxKind {
    pub fn run_id(&self) -> Option<GoalRunId> {
        match self {
            Self::Dispatch { run_id, .. } | Self::Cancel { run_id, .. } => Some(*run_id),
            Self::MilestoneReady { run_id, .. } => Some(*run_id),
        }
    }

    pub fn generation(&self) -> Option<u64> {
        match self {
            Self::Dispatch { generation, .. }
            | Self::Cancel { generation, .. }
            | Self::MilestoneReady { generation, .. } => Some(*generation),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct IdempotencyRecord {
    pub client_identity: String,
    pub request_id: Uuid,
    pub fingerprint: String,
    pub coordinator_revision: u64,
    pub outcome: IdempotencyOutcome,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum IdempotencyOutcome {
    Success { encoded: serde_json::Value },
    Error { message: String },
}

/// Input for [`crate::coordinator::GoalCoordinator::enqueue`].
///
/// `actor` and `client_identity` are provenance and idempotency keys.
/// The service must authenticate them; this aggregate records the asserted
/// subject and fences parent yields by `parent_run_id` + `parent_generation`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct EnqueueSpec {
    #[serde(default)]
    pub priority_class: PriorityClass,
    #[serde(default)]
    pub priority: i32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_run_id: Option<GoalRunId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_generation: Option<u64>,
    /// When true, the parent run identified by `parent_run_id`/`parent_generation`
    /// yields its slot in the same transaction and a child-completion
    /// dependency is recorded.
    #[serde(default)]
    pub parent_yields: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workflow_node_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_request_id: Option<Uuid>,
    #[serde(default)]
    pub retry_safe: bool,
    /// `0` means unlimited attempts.
    #[serde(default)]
    pub max_attempts: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub eligible_at: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dedupe_key: Option<String>,
    #[serde(default)]
    pub wait_for: Vec<GoalId>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EnqueueResult {
    pub assignment_id: GoalAssignmentId,
    pub queue_id: GoalQueueEntryId,
    pub goal_id: GoalId,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ActivateSpec {
    pub goal_id: GoalId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expected_goal_revision: Option<u64>,
    pub actor: GoalActor,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_request_id: Option<Uuid>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct YieldSpec {
    pub goal_id: GoalId,
    pub run_id: GoalRunId,
    pub generation: u64,
    pub actor: GoalActor,
    pub reason: WaitReason,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum CompletionOutcome {
    Success { summary: String },
    Failure { reason: String, retry: bool },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompleteSpec {
    pub goal_id: GoalId,
    pub run_id: GoalRunId,
    pub generation: u64,
    pub actor: GoalActor,
    pub outcome: CompletionOutcome,
    /// Optional application-level outcome label. When omitted, the durable
    /// label is the canonical lifecycle outcome (`success` or `failure`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub outcome_label: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_request_id: Option<Uuid>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CancelSpec {
    pub goal_id: GoalId,
    pub actor: GoalActor,
    pub reason: String,
    #[serde(default)]
    pub cascade: ChildCascadePolicy,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub client_request_id: Option<Uuid>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct ReconcileReport {
    pub interrupted_runs: Vec<GoalRunId>,
    pub requeued: Vec<GoalId>,
    pub cancelled: Vec<GoalId>,
    pub blocked: Vec<GoalId>,
    pub epoch: u64,
}

/// Maximum descendant depth for self/child goals originating from another goal.
pub const MAX_GOAL_DEPTH: u32 = 8;
/// Maximum non-terminal descendants of one root goal.
pub const MAX_LIVE_DESCENDANTS: usize = 16;
/// Minutes of queue wait that promote one priority-class rank toward User.
pub const PRIORITY_AGING_MINUTES: i64 = 15;
/// Default claim expiry used when a heartbeat window is requested.
pub const DEFAULT_SLOT_TTL_SECS: i64 = 300;
