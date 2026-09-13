//! Versioned, typed messages for the goal API.
//!
//! These are wire DTOs rather than a second goal implementation.  The goal
//! aggregate and its check/evaluation types are owned by `firmius-core`; this
//! module only defines the request, response, and event envelopes used by a
//! daemon.  Optional request fields are defaulted so older clients continue
//! to decode when fields are added.

use chrono::{DateTime, Utc};
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use serde_json::Value;
use uuid::Uuid;

/// Version of the goal message contract.  This is deliberately independent
/// of the session protocol version: goal clients can be negotiated separately.
pub const GOAL_PROTOCOL_VERSION: u32 = 2;

/// Stable IDs used by the goal API.  Goal IDs use the domain newtype; command
/// IDs are opaque UUIDs and may be replaced by a durable ID type later.
pub type GoalVersionId = Uuid;
pub type PlanVersionId = Uuid;
pub type OperationId = Uuid;
pub type ApprovalId = Uuid;
pub type GoalAssignmentId = Uuid;
/// Deprecated name retained for source compatibility. This is a goal
/// coordinator assignment id, not a WorkGraph assignment id.
pub type AssignmentId = GoalAssignmentId;
pub type GoalRunId = Uuid;
pub type GoalDependencyId = Uuid;
pub type GoalThreadId = Uuid;
pub type GoalMessageId = Uuid;

pub use firmius_core::{
    AgentCheck, ArtifactCheck, ArtifactCheckKind, CheckEvaluation, CheckState, CheckVerification,
    CommandCheck, CompositeCheck, CompositeOperator, EventCheck, Goal, GoalActor, GoalBudget,
    GoalCheck, GoalCheckKind, GoalError, GoalId, GoalLinks, GoalOwner, GoalProvenance, GoalSource,
    GoalStatus, GoalTransition, ReviewAttestation, ReviewEvidenceCapsule,
};

/// Coordinator domain exports are exposed alongside the deliberately stable
/// wire views below. Names that would collide with a wire DTO use a `Core`
/// prefix; services may project these records without making clients depend
/// on the aggregate's internal representation.
pub use firmius_core::{
    AgentGoalSlot, ChildCascadePolicy, CoordinatorError, DependencyKind, DependencyState,
    GoalAssignment, GoalCoordinator, GoalDependency, GoalQueueEntry, GoalRun, OutboxEntry,
    OutboxKind, OutboxState, PriorityClass,
};
pub use firmius_core::{
    AgentRef as CoreAgentRef, GoalAssignmentId as CoreGoalAssignmentId,
    GoalDependencyId as CoreGoalDependencyId, GoalQueueEntryId as CoreGoalQueueEntryId,
    GoalRunId as CoreGoalRunId, GoalRunState as CoreGoalRunState, GoalTarget as CoreGoalTarget,
    WaitReason as CoreWaitReason,
};

/// The domain event is retained under an explicit name while the protocol
/// event payload below has a stable wire envelope of its own.
pub use firmius_core::GoalEvent as CoreGoalEvent;

/// Three-state policy field used by coordinator requests.
///
/// JSON omits the field (or leaves it at the type default) to inherit from a
/// parent or root goal. JSON `null` explicitly clears the constraint. A value
/// overrides and must only narrow inherited authority. Pair struct fields with
/// `#[serde(default, skip_serializing_if = "Inherited::is_inherited")]` so the
/// three states stay distinct on the wire.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Inherited<T> {
    Inherited,
    Null,
    Value(T),
}

/// Completes a previously requested active cancellation and releases the
/// fenced run's slot. This is intentionally distinct from requesting cancel.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SettleGoalCancellationRequest {
    pub goal_id: GoalId,
    pub run_id: GoalRunId,
    pub lease_generation: u64,
    pub expected_coordinator_revision: u64,
    pub expected_goal_revision: u64,
    pub client_request_id: Uuid,
}

impl From<CoreAgentRef> for GoalTarget {
    fn from(value: CoreAgentRef) -> Self {
        Self {
            session_id: value.session_id,
            agent_id: value.agent_id,
        }
    }
}

impl From<GoalTarget> for CoreAgentRef {
    fn from(value: GoalTarget) -> Self {
        Self {
            session_id: value.session_id,
            agent_id: value.agent_id,
        }
    }
}

/// Why a goal was introduced. This is persisted metadata; callers must not
/// infer self/child authority from an owner string.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalOrigin {
    User,
    Parent,
    Workflow,
    SelfGoal,
    FollowUp,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalAncestry {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<GoalId>,
    pub root_goal_id: GoalId,
    #[serde(default)]
    pub depth: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dedupe_key: Option<String>,
}

impl<T> Default for Inherited<T> {
    fn default() -> Self {
        Self::Inherited
    }
}

impl<T> Inherited<T> {
    pub const fn inherit() -> Self {
        Self::Inherited
    }

    pub const fn null() -> Self {
        Self::Null
    }

    pub fn value(value: T) -> Self {
        Self::Value(value)
    }

    pub const fn is_inherited(&self) -> bool {
        matches!(self, Self::Inherited)
    }

    pub const fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }

    /// `None` inherits, `Some(None)` is explicit null, `Some(Some(v))` is set.
    pub fn as_explicit(&self) -> Option<Option<&T>> {
        match self {
            Self::Inherited => None,
            Self::Null => Some(None),
            Self::Value(value) => Some(Some(value)),
        }
    }
}

impl<T> From<T> for Inherited<T> {
    fn from(value: T) -> Self {
        Self::Value(value)
    }
}

impl<T: Serialize> Serialize for Inherited<T> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Inherited | Self::Null => serializer.serialize_none(),
            Self::Value(value) => value.serialize(serializer),
        }
    }
}

impl<'de, T: Deserialize<'de>> Deserialize<'de> for Inherited<T> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        match Option::<T>::deserialize(deserializer)? {
            None => Ok(Self::Null),
            Some(value) => Ok(Self::Value(value)),
        }
    }
}

/// A qualified execution target. Agent ids are only meaningful within their
/// session and must never be used as a process-global occupancy key.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GoalTarget {
    pub session_id: String,
    pub agent_id: String,
}

/// Placement and durable queue metadata attached to a goal.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalAssignmentView {
    pub assignment_id: AssignmentId,
    pub goal_id: GoalId,
    pub target: GoalTarget,
    pub controller: GoalTarget,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workflow_node_id: Option<String>,
    #[serde(default)]
    pub priority: i32,
    #[serde(default)]
    pub priority_class: PriorityClass,
    #[serde(default)]
    pub retry_safe: bool,
    #[serde(default)]
    pub max_attempts: u32,
    pub revision: u64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalQueueState {
    Gated,
    Ready,
    Dispatching,
    Removed,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalQueueEntryView {
    pub queue_id: Uuid,
    pub assignment_id: AssignmentId,
    pub goal_id: GoalId,
    pub target: GoalTarget,
    pub order: u64,
    #[serde(default)]
    pub priority_class: PriorityClass,
    pub priority: i32,
    pub state: GoalQueueState,
    pub enqueued_at: DateTime<Utc>,
    pub eligible_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub deadline: Option<DateTime<Utc>>,
    pub expected_goal_revision: u64,
    pub revision: u64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
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

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalRunView {
    pub run_id: GoalRunId,
    pub goal_id: GoalId,
    pub assignment_id: AssignmentId,
    pub target: GoalTarget,
    pub lease_generation: u64,
    pub daemon_epoch: u64,
    pub attempt: u32,
    pub state: GoalRunState,
    pub started_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub released_at: Option<DateTime<Utc>>,
    #[serde(default)]
    pub steps_reserved: u32,
    #[serde(default)]
    pub steps_used: u32,
    #[serde(default)]
    pub cost_used: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wait_reason: Option<GoalWaitReason>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    /// Exact semantic outcome label, when the run has durably settled.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub outcome: Option<String>,
    #[serde(default)]
    pub verification: CheckVerification,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum GoalDependencyCondition {
    VerifiedSuccess,
    Terminal,
    Outcome(String),
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalDependencyState {
    Pending,
    Ready,
    Unsatisfiable,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalDependencyView {
    pub dependency_id: GoalDependencyId,
    pub prerequisite_goal_id: GoalId,
    pub dependent_goal_id: GoalId,
    pub condition: GoalDependencyCondition,
    pub state: GoalDependencyState,
    pub revision: u64,
}

/// Projection of the coordinator's exclusive per-agent execution slot.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct AgentGoalSlotView {
    pub target: GoalTarget,
    pub goal_id: GoalId,
    pub run_id: GoalRunId,
    pub assignment_id: GoalAssignmentId,
    pub lease_generation: u64,
    pub daemon_epoch: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalOutboxEntryView {
    pub outbox_id: Uuid,
    pub state: OutboxState,
    pub kind: OutboxKind,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalCoordinatorListView {
    pub coordinator_revision: u64,
    #[serde(default)]
    pub goals: Vec<GoalCoordinatorGoalView>,
    #[serde(default)]
    pub assignments: Vec<GoalAssignmentView>,
    #[serde(default)]
    pub queue: Vec<GoalQueueEntryView>,
    #[serde(default)]
    pub runs: Vec<GoalRunView>,
    #[serde(default)]
    pub dependencies: Vec<GoalDependencyView>,
    #[serde(default)]
    pub slots: Vec<AgentGoalSlotView>,
    #[serde(default)]
    pub pending_outbox: Vec<GoalOutboxEntryView>,
    #[serde(default)]
    pub messages: Vec<GoalMessageView>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub next_cursor: Option<String>,
}

/// Coordinator lifecycle used by coordinator-specific list projections. The
/// embedded `Goal` is retained in full so v1 clients still receive its exact
/// record shape.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalCoordinatorStatus {
    Proposed,
    Queued,
    Active,
    Waiting,
    Blocked,
    Succeeded,
    Failed,
    Cancelling,
    Cancelled,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalCoordinatorGoalView {
    pub goal: Goal,
    pub status: GoalCoordinatorStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalCoordinatorSnapshot {
    pub coordinator_revision: u64,
    pub epoch: Uuid,
    pub generated_at: DateTime<Utc>,
    pub view: GoalCoordinatorListView,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CreateGoalRequest {
    pub description: String,
    #[serde(default)]
    pub success_conditions: Vec<String>,
    pub owner: GoalOwner,
    /// Audit metadata only. Its actor is not an authorization principal.
    pub provenance: GoalProvenance,
    #[serde(default)]
    pub checks: Vec<GoalCheck>,
    #[serde(default)]
    pub deadline: Option<DateTime<Utc>>,
    #[serde(default)]
    pub budget: Option<GoalBudget>,
    #[serde(default)]
    pub approval_required: bool,
    #[serde(default)]
    pub client_request_id: Option<Uuid>,
}

pub type GoalEvent = GoalEventPayload;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct ListGoalsRequest {
    #[serde(default)]
    pub owner: Option<GoalOwner>,
    #[serde(default)]
    pub status: Option<GoalStatus>,
    #[serde(default)]
    pub limit: Option<u32>,
    #[serde(default)]
    pub cursor: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GetGoalRequest {
    pub goal_id: GoalId,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ActivateGoalRequest {
    pub goal_id: GoalId,
    /// Deprecated provenance hint. The service derives authority from the
    /// authenticated session/tool context, never from this value.
    #[serde(default)]
    pub actor: Option<GoalActor>,
    #[serde(default)]
    pub expected_revision: u64,
    #[serde(default)]
    pub client_request_id: Option<Uuid>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CancelGoalRequest {
    pub goal_id: GoalId,
    /// Deprecated provenance hint; not an authorization principal.
    #[serde(default)]
    pub actor: Option<GoalActor>,
    #[serde(default)]
    pub reason: Option<String>,
    #[serde(default)]
    pub expected_revision: u64,
    #[serde(default)]
    pub client_request_id: Option<Uuid>,
}

/// Evaluate one configured check.  `evaluation` is optional for commands
/// whose result is produced by the service; agent checks can submit a typed
/// evaluation directly.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CheckGoalRequest {
    pub goal_id: GoalId,
    pub check_id: String,
    /// Deprecated provenance hint; not an authorization principal.
    #[serde(default)]
    pub actor: Option<GoalActor>,
    #[serde(default)]
    pub evaluation: Option<CheckEvaluation>,
    #[serde(default)]
    pub expected_revision: u64,
    #[serde(default)]
    pub client_request_id: Option<Uuid>,
}

/// Submit or reject a pending approval.  Keeping the decision explicit avoids
/// treating an arbitrary comment as authorization.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ApproveGoalRequest {
    pub goal_id: GoalId,
    pub decision: ApprovalDecision,
    /// Deprecated provenance hint; not an authorization principal.
    #[serde(default)]
    pub actor: Option<GoalActor>,
    #[serde(default)]
    pub approval_id: Option<ApprovalId>,
    #[serde(default)]
    pub reason: Option<String>,
    #[serde(default)]
    pub expected_revision: u64,
    #[serde(default)]
    pub client_request_id: Option<Uuid>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ApprovalDecision {
    Approve,
    Reject,
}

/// Create a goal and its assignment in one coordinator mutation. The original
/// `CreateGoalRequest` remains unchanged for v1 clients.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CreateAssignedGoalRequest {
    pub goal: CreateGoalRequest,
    pub target: GoalTarget,
    pub controller: GoalTarget,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub origin: Option<GoalOrigin>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ancestry: Option<GoalAncestry>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workflow_node_id: Option<String>,
    #[serde(default)]
    pub priority: i32,
    #[serde(default)]
    pub priority_class: PriorityClass,
    #[serde(default)]
    pub retry_safe: bool,
    #[serde(default)]
    pub max_attempts: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub eligible_at: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub deadline: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_fence: Option<ParentRunFence>,
    #[serde(default)]
    pub parent_yields: bool,
    #[serde(default)]
    pub wait_for: Vec<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dependency_policy: Option<GoalDependencyOutcomePolicy>,
    pub expected_coordinator_revision: u64,
    pub client_request_id: Uuid,
    /// Child/self-goal policy. Omitted fields inherit; JSON `null` unsets.
    #[serde(default, skip_serializing_if = "GoalPolicyOverlay::is_inherit_all")]
    pub policy: GoalPolicyOverlay,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct EnqueueGoalRequest {
    pub goal_id: GoalId,
    pub target: GoalTarget,
    pub controller: GoalTarget,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workflow_node_id: Option<String>,
    #[serde(default)]
    pub priority: i32,
    #[serde(default)]
    pub priority_class: PriorityClass,
    #[serde(default)]
    pub retry_safe: bool,
    #[serde(default)]
    pub max_attempts: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub eligible_at: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub deadline: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_fence: Option<ParentRunFence>,
    #[serde(default)]
    pub parent_yields: bool,
    #[serde(default)]
    pub wait_for: Vec<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dependency_policy: Option<GoalDependencyOutcomePolicy>,
    pub expected_coordinator_revision: u64,
    pub expected_goal_revision: u64,
    pub client_request_id: Uuid,
    /// Child/self-goal policy. Omitted fields inherit; JSON `null` unsets.
    #[serde(default, skip_serializing_if = "GoalPolicyOverlay::is_inherit_all")]
    pub policy: GoalPolicyOverlay,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum GoalWaitReason {
    Approval,
    Child { goal_id: GoalId },
    Timer { resume_at: DateTime<Utc> },
    Resource { key: String },
    Verification,
    Other { reason: String },
}

/// External-effect gate a child or self-goal may inherit, deny, or narrow.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalExternalEffectPolicy {
    Deny,
    ReadOnly,
    Allow,
}

/// Authority a child or self-goal may inherit, explicitly clear, or narrow.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct GoalPolicyOverlay {
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub budget: Inherited<GoalBudget>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub deadline: Inherited<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub tool_scopes: Inherited<Vec<String>>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub max_delegation_depth: Inherited<u32>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub external_effects: Inherited<GoalExternalEffectPolicy>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub capability_ceiling: Inherited<String>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub max_descendants: Inherited<u32>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub policy_id: Inherited<String>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    pub approval_required_on_scope_expansion: Inherited<bool>,
}

impl GoalPolicyOverlay {
    pub fn is_inherit_all(&self) -> bool {
        self.budget.is_inherited()
            && self.deadline.is_inherited()
            && self.tool_scopes.is_inherited()
            && self.max_delegation_depth.is_inherited()
            && self.external_effects.is_inherited()
            && self.capability_ceiling.is_inherited()
            && self.max_descendants.is_inherited()
            && self.policy_id.is_inherited()
            && self.approval_required_on_scope_expansion.is_inherited()
    }
}

/// Fence for atomically yielding a parent while enqueuing same-agent child
/// work. A parent goal id without its current run generation is insufficient.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ParentRunFence {
    pub parent_goal_id: GoalId,
    pub parent_run_id: GoalRunId,
    pub parent_generation: u64,
    pub parent_expected_goal_revision: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum GoalDependencyOutcomePolicy {
    VerifiedSuccess,
    Terminal,
    Outcome(String),
}

/// Yield an active run, releasing its agent slot before entering `Waiting`.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct YieldGoalRequest {
    pub goal_id: GoalId,
    pub run_id: GoalRunId,
    pub lease_generation: u64,
    pub wait: GoalWaitReason,
    pub expected_coordinator_revision: u64,
    pub expected_goal_revision: u64,
    pub client_request_id: Uuid,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalEvidence {
    pub reference: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub media_type: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub digest: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub metadata: Option<Value>,
}

/// Candidate results are fenced to the exact run. Submission releases the
/// execution slot but does not imply verified goal success.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SubmitGoalCandidateRequest {
    pub goal_id: GoalId,
    pub run_id: GoalRunId,
    pub lease_generation: u64,
    #[serde(default)]
    pub result: Value,
    #[serde(default)]
    pub evidence: Vec<GoalEvidence>,
    #[serde(default)]
    pub artifact_ids: Vec<String>,
    #[serde(default)]
    pub work_result_ids: Vec<String>,
    #[serde(default)]
    pub verification: CheckVerification,
    pub expected_coordinator_revision: u64,
    pub expected_goal_revision: u64,
    pub client_request_id: Uuid,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalChildCancelPolicy {
    CancelChildren,
    DetachChildren,
    WaitForChildren,
}

/// Coordinator-aware cancellation. The legacy cancel request is retained for
/// v1 daemons; new callers should use this fenced form.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CancelCoordinatedGoalRequest {
    pub goal_id: GoalId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub run_id: Option<GoalRunId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lease_generation: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
    pub child_policy: GoalChildCancelPolicy,
    pub expected_coordinator_revision: u64,
    pub expected_goal_revision: u64,
    pub client_request_id: Uuid,
    /// Deprecated provenance hint; not an authorization principal.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub actor: Option<GoalActor>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SchedulerTickRequest {
    pub expected_coordinator_revision: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<GoalTarget>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_dispatches: Option<u32>,
    pub client_request_id: Uuid,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PromoteGoalRequest {
    pub goal_id: GoalId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<GoalTarget>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub queue_entry_id: Option<Uuid>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expected_queue_entry_revision: Option<u64>,
    pub expected_coordinator_revision: u64,
    pub expected_goal_revision: u64,
    pub client_request_id: Uuid,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CreateGoalDependencyRequest {
    pub prerequisite_goal_id: GoalId,
    pub dependent_goal_id: GoalId,
    pub condition: GoalDependencyCondition,
    pub expected_coordinator_revision: u64,
    /// Revision of the dependent goal, which the new edge mutates.
    pub expected_goal_revision: u64,
    pub expected_prerequisite_revision: u64,
    pub client_request_id: Uuid,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalMessageCorrelation {
    pub thread_id: GoalThreadId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub run_id: Option<GoalRunId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lease_generation: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workflow_node_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub assignment_id: Option<AssignmentId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reply_to: Option<GoalMessageId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub causation_id: Option<GoalMessageId>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalMessageKind {
    RunInput,
    Milestone,
    Notification,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SendGoalMessageRequest {
    pub message_id: GoalMessageId,
    pub goal_id: GoalId,
    pub recipient: GoalTarget,
    pub correlation: GoalMessageCorrelation,
    pub kind: GoalMessageKind,
    pub body: String,
    pub expected_coordinator_revision: u64,
    pub expected_goal_revision: u64,
    pub client_request_id: Uuid,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalMessageView {
    pub message_id: GoalMessageId,
    pub goal_id: GoalId,
    pub sender: GoalTarget,
    pub recipient: GoalTarget,
    pub correlation: GoalMessageCorrelation,
    pub kind: GoalMessageKind,
    pub sequence: u64,
    pub body: String,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct ListGoalCoordinatorRequest {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub goal_id: Option<GoalId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<GoalTarget>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub limit: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cursor: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct GetGoalCoordinatorSnapshotRequest {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<GoalTarget>,
}

/// The method-specific request payload.  It is kept separate from the
/// session `Request` enum so a goal service can be introduced without making
/// existing daemon handlers non-exhaustive.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "method", content = "params", rename_all = "snake_case")]
pub enum GoalRequest {
    Create(CreateGoalRequest),
    List(ListGoalsRequest),
    Get(GetGoalRequest),
    Activate(ActivateGoalRequest),
    Cancel(CancelGoalRequest),
    Check(CheckGoalRequest),
    Approve(ApproveGoalRequest),
    CreateAssigned(CreateAssignedGoalRequest),
    Enqueue(EnqueueGoalRequest),
    Yield(YieldGoalRequest),
    SubmitCandidate(SubmitGoalCandidateRequest),
    CancelCoordinated(CancelCoordinatedGoalRequest),
    SettleCancellation(SettleGoalCancellationRequest),
    SchedulerTick(SchedulerTickRequest),
    Promote(PromoteGoalRequest),
    CreateDependency(CreateGoalDependencyRequest),
    SendMessage(SendGoalMessageRequest),
    ListCoordinator(ListGoalCoordinatorRequest),
    CoordinatorSnapshot(GetGoalCoordinatorSnapshotRequest),
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalRequestEnvelope {
    pub version: u32,
    pub id: Uuid,
    pub request: GoalRequest,
}

impl GoalRequestEnvelope {
    pub fn new(request: GoalRequest) -> Self {
        Self {
            version: GOAL_PROTOCOL_VERSION,
            id: Uuid::new_v4(),
            request,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum GoalResponse {
    Created(Goal),
    Listed {
        goals: Vec<Goal>,
        next_cursor: Option<String>,
    },
    Retrieved(Goal),
    Activated(Goal),
    Cancelled(Goal),
    Checked {
        goal: Goal,
        evaluation: CheckEvaluation,
    },
    Approved(Goal),
    Assigned {
        goal: Goal,
        assignment: GoalAssignmentView,
        coordinator_revision: u64,
    },
    Queued {
        goal: Goal,
        assignment: GoalAssignmentView,
        queue_entry: GoalQueueEntryView,
        coordinator_revision: u64,
    },
    Yielded {
        goal: Goal,
        run: GoalRunView,
        coordinator_revision: u64,
    },
    RunDispatched {
        goal: Goal,
        run: GoalRunView,
        coordinator_revision: u64,
    },
    RunReleased {
        goal: Goal,
        run: GoalRunView,
        reason: GoalRunReleaseReason,
        coordinator_revision: u64,
    },
    Waiting {
        goal: Goal,
        reason: GoalWaitReason,
        coordinator_revision: u64,
    },
    CandidateSubmitted {
        goal: Goal,
        run: GoalRunView,
        coordinator_revision: u64,
    },
    CancellationAccepted {
        goal: Goal,
        coordinator_revision: u64,
    },
    SchedulerAdvanced {
        coordinator_revision: u64,
        #[serde(default)]
        dispatched: Vec<GoalRunView>,
    },
    Promoted {
        goal: Goal,
        run: GoalRunView,
        coordinator_revision: u64,
    },
    DependencyCreated {
        dependency: GoalDependencyView,
        coordinator_revision: u64,
    },
    DependencyReady {
        dependency: GoalDependencyView,
        coordinator_revision: u64,
    },
    MessageAccepted {
        message: GoalMessageView,
        coordinator_revision: u64,
    },
    CoordinatorListed(GoalCoordinatorListView),
    CoordinatorSnapshot(GoalCoordinatorSnapshot),
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalResponseEnvelope {
    pub version: u32,
    pub id: Uuid,
    pub result: Result<GoalResponse, crate::ProtocolError>,
}

/// Lifecycle notifications are separate from responses because a goal may
/// be changed by a scheduler or another client.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum GoalEventPayload {
    Created {
        goal: Goal,
    },
    Activated {
        goal_id: GoalId,
        revision: u64,
    },
    Cancelled {
        goal_id: GoalId,
        revision: u64,
        reason: Option<String>,
    },
    Checked {
        goal_id: GoalId,
        evaluation: CheckEvaluation,
        revision: u64,
    },
    Approved {
        goal_id: GoalId,
        revision: u64,
        decision: ApprovalDecision,
    },
    StatusChanged {
        goal_id: GoalId,
        revision: u64,
        status: GoalStatus,
        reason: Option<String>,
    },
    Transition {
        goal_id: GoalId,
        actor: GoalActor,
        transition: GoalTransition,
        from: GoalStatus,
        to: GoalStatus,
        revision: u64,
    },
    Queued {
        goal_id: GoalId,
        assignment: GoalAssignmentView,
        queue_entry: GoalQueueEntryView,
        coordinator_revision: u64,
    },
    RunDispatched {
        goal_id: GoalId,
        run: GoalRunView,
        coordinator_revision: u64,
    },
    RunReleased {
        goal_id: GoalId,
        run_id: GoalRunId,
        lease_generation: u64,
        reason: GoalRunReleaseReason,
        coordinator_revision: u64,
        goal_revision: u64,
    },
    CandidateSubmitted {
        goal_id: GoalId,
        run_id: GoalRunId,
        lease_generation: u64,
        #[serde(default)]
        evidence: Vec<GoalEvidence>,
        coordinator_revision: u64,
        goal_revision: u64,
    },
    Waiting {
        goal_id: GoalId,
        reason: GoalWaitReason,
        coordinator_revision: u64,
        goal_revision: u64,
    },
    DependencyReady {
        dependency: GoalDependencyView,
        coordinator_revision: u64,
    },
    CoordinatorSnapshot {
        snapshot: GoalCoordinatorSnapshot,
    },
    MessageAppended {
        message: GoalMessageView,
        coordinator_revision: u64,
    },
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum GoalRunReleaseReason {
    Yielded,
    CandidateSubmitted,
    Cancelled,
    Interrupted,
    Failed,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GoalEventEnvelope {
    pub version: u32,
    pub sequence: u64,
    pub event_id: Uuid,
    #[serde(default)]
    pub at: Option<DateTime<Utc>>,
    pub goal_id: GoalId,
    pub event: GoalEventPayload,
}

/// A domain event can be transported without losing its actor/from/to data.
/// This conversion is useful to service implementations that already emit
/// core goal events.
impl From<CoreGoalEvent> for GoalEventEnvelope {
    fn from(event: CoreGoalEvent) -> Self {
        Self {
            version: GOAL_PROTOCOL_VERSION,
            sequence: event.revision,
            event_id: event.id,
            at: Some(event.at),
            goal_id: event.goal_id,
            event: GoalEventPayload::Transition {
                goal_id: event.goal_id,
                actor: event.actor,
                transition: event.transition,
                from: event.from,
                to: event.to,
                revision: event.revision,
            },
        }
    }
}

/// Compatibility aliases matching the operation names used by older clients.
pub type GoalCreateRequest = CreateGoalRequest;
pub type GoalListRequest = ListGoalsRequest;
pub type GoalGetRequest = GetGoalRequest;
pub type GoalActivateRequest = ActivateGoalRequest;
pub type GoalCancelRequest = CancelGoalRequest;
pub type GoalCheckRequest = CheckGoalRequest;
pub type GoalApproveRequest = ApproveGoalRequest;
pub type GoalEnqueueRequest = EnqueueGoalRequest;
pub type GoalYieldRequest = YieldGoalRequest;
pub type GoalSubmitCandidateRequest = SubmitGoalCandidateRequest;
pub type GoalCreateDependencyRequest = CreateGoalDependencyRequest;
pub type GoalSendMessageRequest = SendGoalMessageRequest;
pub type GoalRecord = Goal;

#[cfg(test)]
mod tests {
    use super::*;

    fn round_trip<T>(value: &T)
    where
        T: Serialize + for<'de> Deserialize<'de> + PartialEq + std::fmt::Debug,
    {
        let bytes = serde_json::to_vec(value).unwrap();
        let decoded: T = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(&decoded, value);
    }

    fn fixture() -> Goal {
        Goal::new(
            "keep tests green",
            vec!["all tests pass".into()],
            GoalOwner::User {
                user_id: "user".into(),
            },
            GoalProvenance {
                actor: GoalActor::User {
                    user_id: "user".into(),
                },
                source: firmius_core::GoalSource::UserRequest,
                created_at: Utc::now(),
            },
        )
        .unwrap()
    }

    #[test]
    fn all_goal_operations_round_trip() {
        let goal_id = fixture().id;
        let actor = Some(GoalActor::User {
            user_id: "user".into(),
        });
        let requests = vec![
            GoalRequest::Create(CreateGoalRequest {
                description: "keep tests green".into(),
                success_conditions: vec!["pass".into()],
                owner: GoalOwner::User {
                    user_id: "user".into(),
                },
                provenance: fixture().provenance,
                checks: vec![],
                deadline: None,
                budget: None,
                approval_required: false,
                client_request_id: None,
            }),
            GoalRequest::List(ListGoalsRequest::default()),
            GoalRequest::Get(GetGoalRequest { goal_id }),
            GoalRequest::Activate(ActivateGoalRequest {
                goal_id,
                actor: actor.clone(),
                expected_revision: 0,
                client_request_id: None,
            }),
            GoalRequest::Cancel(CancelGoalRequest {
                goal_id,
                actor: actor.clone(),
                reason: None,
                expected_revision: 0,
                client_request_id: None,
            }),
            GoalRequest::Check(CheckGoalRequest {
                goal_id,
                check_id: "check".into(),
                actor: actor.clone(),
                evaluation: None,
                expected_revision: 0,
                client_request_id: None,
            }),
            GoalRequest::Approve(ApproveGoalRequest {
                goal_id,
                decision: ApprovalDecision::Approve,
                actor,
                approval_id: None,
                reason: None,
                expected_revision: 0,
                client_request_id: None,
            }),
        ];
        for request in requests {
            let envelope = GoalRequestEnvelope::new(request);
            let bytes = serde_json::to_vec(&envelope).unwrap();
            let decoded: GoalRequestEnvelope = serde_json::from_slice(&bytes).unwrap();
            assert_eq!(decoded.version, GOAL_PROTOCOL_VERSION);
            assert_eq!(decoded.id, envelope.id);
        }
    }

    #[test]
    fn goal_response_and_event_round_trip() {
        let goal = fixture();
        let response = GoalResponseEnvelope {
            version: GOAL_PROTOCOL_VERSION,
            id: Uuid::new_v4(),
            result: Ok(GoalResponse::Listed {
                goals: vec![goal.clone()],
                next_cursor: None,
            }),
        };
        let bytes = serde_json::to_vec(&response).unwrap();
        let decoded: GoalResponseEnvelope = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(decoded.version, response.version);
        assert!(matches!(
            decoded.result.unwrap(),
            GoalResponse::Listed { .. }
        ));

        let event = GoalEventEnvelope {
            version: GOAL_PROTOCOL_VERSION,
            sequence: 1,
            event_id: Uuid::new_v4(),
            at: Some(Utc::now()),
            goal_id: goal.id,
            event: GoalEventPayload::StatusChanged {
                goal_id: goal.id,
                revision: 1,
                status: GoalStatus::Active,
                reason: None,
            },
        };
        let bytes = serde_json::to_vec(&event).unwrap();
        let decoded: GoalEventEnvelope = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(decoded.goal_id, event.goal_id);
        assert!(matches!(
            decoded.event,
            GoalEventPayload::StatusChanged { .. }
        ));
    }

    #[test]
    fn every_goal_response_round_trips() {
        let (goal, target, assignment, queue, run, dependency, message) = coordinator_fixtures();
        let view = GoalCoordinatorListView {
            coordinator_revision: 10,
            goals: vec![GoalCoordinatorGoalView {
                goal: goal.clone(),
                status: GoalCoordinatorStatus::Queued,
            }],
            assignments: vec![assignment.clone()],
            queue: vec![queue.clone()],
            runs: vec![run.clone()],
            dependencies: vec![dependency.clone()],
            slots: vec![AgentGoalSlotView {
                target,
                goal_id: goal.id,
                run_id: run.run_id,
                assignment_id: run.assignment_id,
                lease_generation: run.lease_generation,
                daemon_epoch: run.daemon_epoch,
            }],
            pending_outbox: vec![],
            messages: vec![],
            next_cursor: None,
        };
        let snapshot = GoalCoordinatorSnapshot {
            coordinator_revision: 10,
            epoch: Uuid::new_v4(),
            generated_at: Utc::now(),
            view: view.clone(),
        };
        let evaluation = CheckEvaluation {
            check_id: "check".into(),
            state: CheckState::Passed,
            inputs: Value::Null,
            output: None,
            evaluated_at: Utc::now(),
            actor: GoalActor::System,
            evidence: vec!["artifact://evidence".into()],
            verification: CheckVerification::SelfVerified,
            review: None,
        };
        let responses = vec![
            GoalResponse::Created(goal.clone()),
            GoalResponse::Listed {
                goals: vec![goal.clone()],
                next_cursor: None,
            },
            GoalResponse::Retrieved(goal.clone()),
            GoalResponse::Activated(goal.clone()),
            GoalResponse::Cancelled(goal.clone()),
            GoalResponse::Checked {
                goal: goal.clone(),
                evaluation,
            },
            GoalResponse::Approved(goal.clone()),
            GoalResponse::Assigned {
                goal: goal.clone(),
                assignment: assignment.clone(),
                coordinator_revision: 10,
            },
            GoalResponse::Queued {
                goal: goal.clone(),
                assignment,
                queue_entry: queue,
                coordinator_revision: 10,
            },
            GoalResponse::Yielded {
                goal: goal.clone(),
                run: run.clone(),
                coordinator_revision: 10,
            },
            GoalResponse::RunDispatched {
                goal: goal.clone(),
                run: run.clone(),
                coordinator_revision: 10,
            },
            GoalResponse::RunReleased {
                goal: goal.clone(),
                run: run.clone(),
                reason: GoalRunReleaseReason::Yielded,
                coordinator_revision: 10,
            },
            GoalResponse::Waiting {
                goal: goal.clone(),
                reason: GoalWaitReason::Verification,
                coordinator_revision: 10,
            },
            GoalResponse::CandidateSubmitted {
                goal: goal.clone(),
                run: run.clone(),
                coordinator_revision: 10,
            },
            GoalResponse::CancellationAccepted {
                goal: goal.clone(),
                coordinator_revision: 10,
            },
            GoalResponse::SchedulerAdvanced {
                coordinator_revision: 10,
                dispatched: vec![run.clone()],
            },
            GoalResponse::Promoted {
                goal: goal.clone(),
                run,
                coordinator_revision: 10,
            },
            GoalResponse::DependencyCreated {
                dependency: dependency.clone(),
                coordinator_revision: 10,
            },
            GoalResponse::DependencyReady {
                dependency,
                coordinator_revision: 10,
            },
            GoalResponse::MessageAccepted {
                message,
                coordinator_revision: 10,
            },
            GoalResponse::CoordinatorListed(view),
            GoalResponse::CoordinatorSnapshot(snapshot),
        ];
        for response in responses {
            round_trip(&response);
        }
    }

    #[test]
    fn every_goal_event_round_trips() {
        let (goal, _, assignment, queue, run, dependency, message) = coordinator_fixtures();
        let evidence = GoalEvidence {
            reference: "artifact://result".into(),
            media_type: None,
            digest: None,
            metadata: None,
        };
        let evaluation = CheckEvaluation {
            check_id: "check".into(),
            state: CheckState::Pending,
            inputs: Value::Null,
            output: None,
            evaluated_at: Utc::now(),
            actor: GoalActor::System,
            evidence: vec![],
            verification: CheckVerification::Unverified,
            review: None,
        };
        let snapshot = GoalCoordinatorSnapshot {
            coordinator_revision: 10,
            epoch: Uuid::new_v4(),
            generated_at: Utc::now(),
            view: GoalCoordinatorListView {
                coordinator_revision: 10,
                goals: vec![],
                assignments: vec![],
                queue: vec![],
                runs: vec![],
                dependencies: vec![],
                slots: vec![],
                pending_outbox: vec![],
                messages: vec![],
                next_cursor: None,
            },
        };
        let events = vec![
            GoalEventPayload::Created { goal: goal.clone() },
            GoalEventPayload::Activated {
                goal_id: goal.id,
                revision: 1,
            },
            GoalEventPayload::Cancelled {
                goal_id: goal.id,
                revision: 1,
                reason: None,
            },
            GoalEventPayload::Checked {
                goal_id: goal.id,
                evaluation,
                revision: 1,
            },
            GoalEventPayload::Approved {
                goal_id: goal.id,
                revision: 1,
                decision: ApprovalDecision::Approve,
            },
            GoalEventPayload::StatusChanged {
                goal_id: goal.id,
                revision: 1,
                status: GoalStatus::Waiting,
                reason: Some("child".into()),
            },
            GoalEventPayload::Transition {
                goal_id: goal.id,
                actor: GoalActor::System,
                transition: GoalTransition::Wait {
                    reason: "child".into(),
                },
                from: GoalStatus::Active,
                to: GoalStatus::Waiting,
                revision: 1,
            },
            GoalEventPayload::Queued {
                goal_id: goal.id,
                assignment,
                queue_entry: queue,
                coordinator_revision: 10,
            },
            GoalEventPayload::RunDispatched {
                goal_id: goal.id,
                run: run.clone(),
                coordinator_revision: 10,
            },
            GoalEventPayload::RunReleased {
                goal_id: goal.id,
                run_id: run.run_id,
                lease_generation: run.lease_generation,
                reason: GoalRunReleaseReason::Yielded,
                coordinator_revision: 10,
                goal_revision: 2,
            },
            GoalEventPayload::CandidateSubmitted {
                goal_id: goal.id,
                run_id: run.run_id,
                lease_generation: run.lease_generation,
                evidence: vec![evidence],
                coordinator_revision: 10,
                goal_revision: 2,
            },
            GoalEventPayload::Waiting {
                goal_id: goal.id,
                reason: GoalWaitReason::Verification,
                coordinator_revision: 10,
                goal_revision: 2,
            },
            GoalEventPayload::DependencyReady {
                dependency,
                coordinator_revision: 10,
            },
            GoalEventPayload::CoordinatorSnapshot { snapshot },
            GoalEventPayload::MessageAppended {
                message,
                coordinator_revision: 10,
            },
        ];
        for event in events {
            round_trip(&event);
        }
    }

    #[test]
    fn v1_goal_json_still_decodes_without_new_fields() {
        let goal_id = GoalId::new();
        let request = serde_json::json!({
            "version": 1,
            "id": Uuid::new_v4(),
            "request": {
                "method": "activate",
                "params": { "goal_id": goal_id }
            }
        });
        let decoded: GoalRequestEnvelope = serde_json::from_value(request).unwrap();
        assert_eq!(decoded.version, 1);
        assert!(matches!(
            decoded.request,
            GoalRequest::Activate(ActivateGoalRequest {
                expected_revision: 0,
                client_request_id: None,
                actor: None,
                ..
            })
        ));

        let list: GoalRequest = serde_json::from_value(serde_json::json!({
            "method": "list",
            "params": {}
        }))
        .unwrap();
        assert_eq!(list, GoalRequest::List(ListGoalsRequest::default()));
    }

    #[test]
    fn v1_response_and_event_envelopes_still_decode() {
        // These fixtures deliberately carry the legacy envelope version, which
        // is no longer the current protocol version. Pinning the current
        // version here makes a future bump revisit this compatibility test
        // instead of silently letting the fixtures drift.
        assert_eq!(crate::PROTOCOL_VERSION, 2);
        let goal = fixture();
        let response_id = Uuid::new_v4();
        let response = serde_json::json!({
            "version": 1,
            "id": response_id,
            "result": {
                "Ok": {
                    "kind": "retrieved",
                    "data": goal
                }
            }
        });
        let decoded: GoalResponseEnvelope = serde_json::from_value(response).unwrap();
        assert_eq!(decoded.version, 1);
        assert_eq!(decoded.id, response_id);
        assert!(matches!(decoded.result, Ok(GoalResponse::Retrieved(_))));

        let event_id = Uuid::new_v4();
        let event = serde_json::json!({
            "version": 1,
            "sequence": 4,
            "event_id": event_id,
            "goal_id": goal.id,
            "event": {
                "kind": "activated",
                "data": {
                    "goal_id": goal.id,
                    "revision": 4
                }
            }
        });
        let decoded: GoalEventEnvelope = serde_json::from_value(event).unwrap();
        assert_eq!(decoded.version, 1);
        assert_eq!(decoded.event_id, event_id);
        assert!(matches!(decoded.event, GoalEventPayload::Activated { .. }));
    }

    fn coordinator_fixtures() -> (
        Goal,
        GoalTarget,
        GoalAssignmentView,
        GoalQueueEntryView,
        GoalRunView,
        GoalDependencyView,
        GoalMessageView,
    ) {
        let goal = Goal::new(
            "keep tests green",
            vec!["all tests pass".into()],
            GoalOwner::User {
                user_id: "user".into(),
            },
            GoalProvenance {
                actor: GoalActor::User {
                    user_id: "user".into(),
                },
                source: GoalSource::UserRequest,
                created_at: Utc::now(),
            },
        )
        .unwrap();
        let target = GoalTarget {
            session_id: "session-a".into(),
            agent_id: "agent-a".into(),
        };
        let assignment = GoalAssignmentView {
            assignment_id: Uuid::new_v4(),
            goal_id: goal.id,
            target: target.clone(),
            controller: GoalTarget {
                session_id: "session-a".into(),
                agent_id: "parent".into(),
            },
            parent_goal_id: None,
            workflow_node_id: Some("build".into()),
            priority: 7,
            priority_class: PriorityClass::ParentMilestone,
            retry_safe: true,
            max_attempts: 3,
            revision: 1,
        };
        let queue = GoalQueueEntryView {
            queue_id: Uuid::new_v4(),
            assignment_id: assignment.assignment_id,
            goal_id: goal.id,
            target: target.clone(),
            order: 3,
            priority_class: PriorityClass::ParentMilestone,
            priority: 7,
            state: GoalQueueState::Ready,
            enqueued_at: Utc::now(),
            eligible_at: Utc::now(),
            deadline: None,
            expected_goal_revision: 0,
            revision: 1,
        };
        let run = GoalRunView {
            run_id: Uuid::new_v4(),
            goal_id: goal.id,
            assignment_id: assignment.assignment_id,
            target: target.clone(),
            lease_generation: 2,
            daemon_epoch: 1,
            attempt: 1,
            state: GoalRunState::Open,
            started_at: Utc::now(),
            released_at: None,
            steps_reserved: 1,
            steps_used: 0,
            cost_used: 0,
            wait_reason: None,
            result: None,
            verification: CheckVerification::Unverified,
            outcome: None,
        };
        let dependency = GoalDependencyView {
            dependency_id: Uuid::new_v4(),
            prerequisite_goal_id: GoalId::new(),
            dependent_goal_id: goal.id,
            condition: GoalDependencyCondition::VerifiedSuccess,
            state: GoalDependencyState::Pending,
            revision: 1,
        };
        let message = GoalMessageView {
            message_id: Uuid::new_v4(),
            goal_id: goal.id,
            sender: assignment.controller.clone(),
            recipient: target.clone(),
            correlation: GoalMessageCorrelation {
                thread_id: Uuid::new_v4(),
                run_id: Some(run.run_id),
                lease_generation: Some(run.lease_generation),
                parent_goal_id: None,
                workflow_node_id: assignment.workflow_node_id.clone(),
                assignment_id: Some(assignment.assignment_id),
                reply_to: None,
                causation_id: None,
            },
            kind: GoalMessageKind::RunInput,
            sequence: 1,
            body: "continue".into(),
            created_at: Utc::now(),
        };
        (goal, target, assignment, queue, run, dependency, message)
    }

    #[test]
    fn all_coordinator_requests_round_trip() {
        let (goal, target, assignment, _, run, dependency, message) = coordinator_fixtures();
        let controller = assignment.controller.clone();
        let request_id = Uuid::new_v4();
        let create = CreateGoalRequest {
            description: goal.description.clone(),
            success_conditions: goal.success_conditions.clone(),
            owner: goal.owner.clone(),
            provenance: goal.provenance.clone(),
            checks: goal.checks.clone(),
            deadline: goal.deadline,
            budget: goal.budget.clone(),
            approval_required: goal.approval.required,
            client_request_id: Some(request_id),
        };
        let requests = vec![
            GoalRequest::CreateAssigned(CreateAssignedGoalRequest {
                goal: create,
                target: target.clone(),
                controller: controller.clone(),
                origin: Some(GoalOrigin::Parent),
                ancestry: None,
                parent_goal_id: None,
                workflow_node_id: Some("build".into()),
                priority: 7,
                priority_class: PriorityClass::ParentMilestone,
                retry_safe: true,
                max_attempts: 3,
                eligible_at: None,
                deadline: None,
                parent_fence: None,
                parent_yields: false,
                wait_for: vec![],
                dependency_policy: None,
                expected_coordinator_revision: 10,
                client_request_id: request_id,
                policy: GoalPolicyOverlay::default(),
            }),
            GoalRequest::Enqueue(EnqueueGoalRequest {
                goal_id: goal.id,
                target: target.clone(),
                controller,
                parent_goal_id: None,
                workflow_node_id: None,
                priority: 7,
                priority_class: PriorityClass::ParentMilestone,
                retry_safe: true,
                max_attempts: 3,
                eligible_at: None,
                deadline: None,
                parent_fence: None,
                parent_yields: false,
                wait_for: vec![],
                dependency_policy: None,
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                client_request_id: request_id,
                policy: GoalPolicyOverlay::default(),
            }),
            GoalRequest::Yield(YieldGoalRequest {
                goal_id: goal.id,
                run_id: run.run_id,
                lease_generation: run.lease_generation,
                wait: GoalWaitReason::Child {
                    goal_id: dependency.prerequisite_goal_id,
                },
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                client_request_id: request_id,
            }),
            GoalRequest::SubmitCandidate(SubmitGoalCandidateRequest {
                goal_id: goal.id,
                run_id: run.run_id,
                lease_generation: run.lease_generation,
                result: serde_json::json!({"outcome": "success"}),
                evidence: vec![GoalEvidence {
                    reference: "artifact://result.json".into(),
                    media_type: Some("application/json".into()),
                    digest: Some("sha256:abc".into()),
                    metadata: Some(serde_json::json!({"check": "tests"})),
                }],
                artifact_ids: vec!["artifact://result.json".into()],
                work_result_ids: vec![],
                verification: CheckVerification::SelfVerified,
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                client_request_id: request_id,
            }),
            GoalRequest::CancelCoordinated(CancelCoordinatedGoalRequest {
                goal_id: goal.id,
                run_id: Some(run.run_id),
                lease_generation: Some(run.lease_generation),
                reason: Some("superseded".into()),
                child_policy: GoalChildCancelPolicy::WaitForChildren,
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                client_request_id: request_id,
                actor: Some(GoalActor::System),
            }),
            GoalRequest::SettleCancellation(SettleGoalCancellationRequest {
                goal_id: goal.id,
                run_id: run.run_id,
                lease_generation: run.lease_generation,
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                client_request_id: request_id,
            }),
            GoalRequest::SchedulerTick(SchedulerTickRequest {
                expected_coordinator_revision: 10,
                target: Some(target.clone()),
                max_dispatches: Some(1),
                client_request_id: request_id,
            }),
            GoalRequest::Promote(PromoteGoalRequest {
                goal_id: goal.id,
                target: Some(target.clone()),
                queue_entry_id: None,
                expected_queue_entry_revision: None,
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                client_request_id: request_id,
            }),
            GoalRequest::CreateDependency(CreateGoalDependencyRequest {
                prerequisite_goal_id: dependency.prerequisite_goal_id,
                dependent_goal_id: goal.id,
                condition: GoalDependencyCondition::Outcome("approved".into()),
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                expected_prerequisite_revision: 4,
                client_request_id: request_id,
            }),
            GoalRequest::SendMessage(SendGoalMessageRequest {
                message_id: Uuid::new_v4(),
                goal_id: goal.id,
                recipient: target.clone(),
                correlation: message.correlation,
                kind: GoalMessageKind::RunInput,
                body: "continue".into(),
                expected_coordinator_revision: 10,
                expected_goal_revision: 2,
                client_request_id: request_id,
            }),
            GoalRequest::ListCoordinator(ListGoalCoordinatorRequest {
                goal_id: Some(goal.id),
                target: Some(target.clone()),
                limit: Some(20),
                cursor: Some("cursor".into()),
            }),
            GoalRequest::CoordinatorSnapshot(GetGoalCoordinatorSnapshotRequest {
                target: Some(target),
            }),
        ];
        for request in requests {
            let envelope = GoalRequestEnvelope::new(request);
            let bytes = serde_json::to_vec(&envelope).unwrap();
            let decoded: GoalRequestEnvelope = serde_json::from_slice(&bytes).unwrap();
            assert_eq!(decoded, envelope);
        }
    }

    #[test]
    fn inherited_omitted_null_and_value_are_distinct() {
        let goal = fixture();
        let target = GoalTarget {
            session_id: "session-a".into(),
            agent_id: "agent-a".into(),
        };
        let request_id = Uuid::new_v4();
        let create = CreateGoalRequest {
            description: goal.description.clone(),
            success_conditions: goal.success_conditions.clone(),
            owner: goal.owner.clone(),
            provenance: goal.provenance.clone(),
            checks: vec![],
            deadline: None,
            budget: None,
            approval_required: false,
            client_request_id: Some(request_id),
        };
        let base = CreateAssignedGoalRequest {
            goal: create,
            target: target.clone(),
            controller: target,
            origin: Some(GoalOrigin::SelfGoal),
            ancestry: None,
            parent_goal_id: None,
            workflow_node_id: None,
            priority: 0,
            priority_class: PriorityClass::SelfMaintenance,
            retry_safe: false,
            max_attempts: 1,
            eligible_at: None,
            deadline: None,
            parent_fence: None,
            parent_yields: false,
            wait_for: vec![],
            dependency_policy: None,
            expected_coordinator_revision: 1,
            client_request_id: request_id,
            policy: GoalPolicyOverlay::default(),
        };

        let omitted = serde_json::to_value(&base).unwrap();
        assert!(omitted.get("policy").is_none());
        let decoded: CreateAssignedGoalRequest = serde_json::from_value(omitted).unwrap();
        assert!(decoded.policy.is_inherit_all());
        assert_eq!(decoded.policy.budget, Inherited::Inherited);

        let mut explicit_null = base.clone();
        explicit_null.policy.budget = Inherited::Null;
        explicit_null.policy.deadline = Inherited::Null;
        explicit_null.policy.tool_scopes = Inherited::Null;
        explicit_null.policy.max_delegation_depth = Inherited::Null;
        explicit_null.policy.external_effects = Inherited::Null;
        let null_json = serde_json::to_value(&explicit_null).unwrap();
        assert_eq!(null_json["policy"]["budget"], Value::Null);
        assert_eq!(null_json["policy"]["deadline"], Value::Null);
        let decoded_null: CreateAssignedGoalRequest = serde_json::from_value(null_json).unwrap();
        assert_eq!(decoded_null.policy.budget, Inherited::Null);
        assert_eq!(decoded_null.policy.deadline, Inherited::Null);
        assert_eq!(decoded_null.policy.tool_scopes, Inherited::Null);
        assert_eq!(decoded_null.policy.max_delegation_depth, Inherited::Null);
        assert_eq!(decoded_null.policy.external_effects, Inherited::Null);

        let mut overlay = GoalPolicyOverlay::default();
        overlay.budget = Inherited::value(GoalBudget {
            max_cost: Some(12),
            max_steps: Some(3),
        });
        overlay.deadline = Inherited::value(Utc::now());
        overlay.tool_scopes = Inherited::value(vec!["fs_read".into()]);
        overlay.max_delegation_depth = Inherited::value(1);
        overlay.external_effects = Inherited::value(GoalExternalEffectPolicy::ReadOnly);
        let mut narrowed = base;
        narrowed.policy = overlay.clone();
        round_trip(&narrowed);
        let value_json = serde_json::to_value(&narrowed).unwrap();
        assert_eq!(value_json["policy"]["budget"]["max_steps"], 3);
        assert_eq!(value_json["policy"]["external_effects"], "read_only");
        assert_eq!(
            narrowed
                .policy
                .budget
                .as_explicit()
                .unwrap()
                .unwrap()
                .max_steps,
            Some(3)
        );
        assert!(overlay.deadline.as_explicit().unwrap().is_some());
    }
}
