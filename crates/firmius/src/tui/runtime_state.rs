//! Renderer-independent state fixtures for the runtime surfaces.
//!
//! These types intentionally describe state rather than prescribe a widget.
//! They are useful to the gallery and to future renderers, while the existing
//! event protocol and model remain unchanged.

use chrono::{DateTime, Utc};
use firmius_core::{CheckState, CheckVerification, GoalCheck, GoalCheckKind, GoalStatus};
use firmius_protocol::{GoalEventEnvelope, GoalEventPayload, GoalEvidence};
use serde::{Deserialize, Serialize};

/// The policy being edited is account/session state, not an individual
/// permission request.  Keep the core policy vocabulary here so a renderer
/// cannot accidentally use a gate status as an administration mode.
pub type PermissionPolicyMode = firmius_core::PermissionMode;
pub type PermissionPolicyRule = firmius_core::PermissionRule;
pub type PermissionPolicyExactBehavior = firmius_core::PermissionDecision;

/// Lifecycle of one typed tool execution.  `Settled` is a successful result;
/// `Failed` is terminal but retains a classifier reason for presentation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ToolExecutionLifecycle {
    Preparing,
    Queued,
    WaitingPermission,
    Running,
    Settled,
    Failed,
}

/// Where an administered policy is durable.  The scope is explicit because a
/// session overlay must never be mistaken for the account policy.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind", content = "id")]
pub enum PermissionPolicyPersistenceScope {
    Account,
    Session(String),
    Persona(String),
    Agent(String),
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionPolicyScope {
    pub name: String,
    pub persistence: PermissionPolicyPersistenceScope,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionPolicyDraft {
    pub mode: PermissionPolicyMode,
    pub exact_behavior: PermissionPolicyExactBehavior,
    #[serde(default)]
    pub rules: Vec<PermissionPolicyRule>,
    #[serde(default)]
    pub scopes: Vec<PermissionPolicyScope>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionPolicyAuditAction {
    Staged,
    Applied,
    Discarded,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionPolicyAuditEntry {
    pub action: PermissionPolicyAuditAction,
    pub revision: u64,
    pub at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub summary: Option<String>,
}

/// Renderer-independent administration state.  `PermissionGateState` above
/// remains request-local; this state is the durable policy editor with an
/// intentionally separate staged draft and explicit apply/discard lifecycle.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionPolicyAdministrationState {
    pub active_mode: PermissionPolicyMode,
    pub active_exact_behavior: PermissionPolicyExactBehavior,
    #[serde(default)]
    pub active_rules: Vec<PermissionPolicyRule>,
    #[serde(default)]
    pub active_scopes: Vec<PermissionPolicyScope>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub staged_draft: Option<PermissionPolicyDraft>,
    #[serde(default)]
    pub audit_entries: Vec<PermissionPolicyAuditEntry>,
    pub revision: u64,
    pub persistence_scope: PermissionPolicyPersistenceScope,
}

impl PermissionPolicyAdministrationState {
    pub fn new(
        mode: PermissionPolicyMode,
        exact_behavior: PermissionPolicyExactBehavior,
        persistence_scope: PermissionPolicyPersistenceScope,
    ) -> Self {
        Self {
            active_mode: mode,
            active_exact_behavior: exact_behavior,
            active_rules: Vec::new(),
            active_scopes: Vec::new(),
            staged_draft: None,
            audit_entries: Vec::new(),
            revision: 0,
            persistence_scope,
        }
    }

    pub fn stage(&mut self, draft: PermissionPolicyDraft, at: DateTime<Utc>) {
        self.staged_draft = Some(draft);
        self.audit_entries.push(PermissionPolicyAuditEntry {
            action: PermissionPolicyAuditAction::Staged,
            revision: self.revision,
            at,
            summary: None,
        });
    }

    /// Apply is atomic from the presentation contract's perspective: the
    /// active fields and revision change together, and the draft disappears.
    pub fn apply_staged(&mut self, at: DateTime<Utc>) -> bool {
        let Some(draft) = self.staged_draft.take() else {
            return false;
        };
        self.active_mode = draft.mode;
        self.active_exact_behavior = draft.exact_behavior;
        self.active_rules = draft.rules;
        self.active_scopes = draft.scopes;
        self.revision = self.revision.saturating_add(1);
        self.audit_entries.push(PermissionPolicyAuditEntry {
            action: PermissionPolicyAuditAction::Applied,
            revision: self.revision,
            at,
            summary: None,
        });
        true
    }

    pub fn discard_staged(&mut self, at: DateTime<Utc>) -> bool {
        if self.staged_draft.take().is_none() {
            return false;
        }
        self.audit_entries.push(PermissionPolicyAuditEntry {
            action: PermissionPolicyAuditAction::Discarded,
            revision: self.revision,
            at,
            summary: None,
        });
        true
    }
}

/// Why a permission gate reached its current decision.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionProvenance {
    User,
    Auto,
    Policy,
    Inherited,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionDecision {
    pub allowed: bool,
    pub provenance: PermissionProvenance,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
    pub decided_at: DateTime<Utc>,
}

/// A gate is deliberately separate from policy administration: it represents
/// one request attached to one tool call, not the account-wide permission
/// policy currently being edited in Settings.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionGateStatus {
    Requested,
    Waiting,
    UserAllowed,
    UserDenied,
    AutoAllowed,
    AutoDenied,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionGateState {
    pub request_id: String,
    pub status: PermissionGateStatus,
    pub requested_at: DateTime<Utc>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub decision: Option<PermissionDecision>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ToolExecutionTimestamps {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub prepared_at: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub queued_at: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub started_at: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub settled_at: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub failed_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ToolExecutionState {
    pub batch_id: String,
    pub queue_position: u32,
    pub queue_total: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub predecessor: Option<String>,
    pub lifecycle: ToolExecutionLifecycle,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub permission: Option<PermissionGateState>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub classifier_reason: Option<String>,
    #[serde(default)]
    pub timestamps: ToolExecutionTimestamps,
    /// Bounded, renderer-ready output captured while the call is live.  The
    /// transcript remains the source of truth; this is only the newest tail
    /// so an active process never forces the card to grow without a limit.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_tail: Option<String>,
    /// Aggregate child progress for delegate calls.  Child transcripts remain
    /// independently addressable; this summary is the compact parent signal.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub delegate: Option<DelegateChildAggregate>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct DelegateChildAggregate {
    pub total: u32,
    pub running: u32,
    pub settled: u32,
    pub failed: u32,
}

impl DelegateChildAggregate {
    pub fn summary(&self) -> String {
        format!(
            "{}/{} settled · {} running · {} failed",
            self.settled, self.total, self.running, self.failed
        )
    }
}

/// Explicit availability for goal validation.  `None` is not treated as an
/// empty goal: callers can render the unavailable reason without inventing a
/// candidate or check state.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GoalValidationAvailability<'a> {
    Available(&'a GoalValidationFixture),
    Unavailable { reason: Option<&'a str> },
}

/// Availability and retry are independent of freshness.  A stale value may
/// still be rendered while a scoped retry is in flight.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RetryScope {
    Item,
    Surface,
    Session,
    Account,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RetryRequest {
    pub scope: RetryScope,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

/// Generic async state shared by accounts/quota, sessions, agent/model/
/// capability, and workflow/artifact surfaces.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum SurfaceState<T> {
    Immediate(T),
    Request {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        previous: Option<T>,
    },
    Partial {
        value: T,
        missing: Vec<String>,
    },
    Fresh {
        value: T,
        age_seconds: u64,
    },
    Stale {
        value: T,
        age_seconds: u64,
    },
    Failed {
        error: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        previous: Option<T>,
        retry: RetryRequest,
    },
    UnavailableAuth {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
}

pub type AccountsQuotaState<T> = SurfaceState<T>;
pub type SessionsState<T> = SurfaceState<T>;
pub type AgentModelCapabilityState<T> = SurfaceState<T>;
pub type WorkflowArtifactState<T> = SurfaceState<T>;

/// Deliberately compact row data: persona administration presents the name
/// and attached model, while loading/error state belongs to its own surface.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompactPersonaRow {
    pub persona_name: String,
    pub attached_model: String,
}

pub type PersonaRowFixture = CompactPersonaRow;

/// These fixtures are separate even when a caller loads them together.  This
/// prevents a quota failure from making account or session data look absent.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AccountLoadingFixture<T: Default> {
    pub accounts: SurfaceState<T>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct QuotaLoadingFixture<T: Default> {
    pub quota: SurfaceState<T>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionLoadingFixture<T: Default> {
    pub sessions: SurfaceState<T>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SurfaceResource {
    AccountsQuota,
    Sessions,
    AgentModelCapability,
    WorkflowArtifact,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionOperation {
    Loading,
    Resume,
    Attach,
    Replay,
}

/// A candidate and its checks are intentionally fixture data, not a second
/// goal protocol.  This is enough to exercise pending, active, reviewer,
/// evidence, feedback, retry, and composite rendering states deterministically.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CandidateLifecycle {
    Candidate,
    Submitted,
    Checking,
    Settled,
    Retry,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CandidateSubmission {
    pub id: String,
    pub summary: String,
    pub lifecycle: CandidateLifecycle,
    pub submitted_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CheckLifecycle {
    Pending,
    Active,
    Settled,
    Failed,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CheckActivity {
    Queued,
    Running,
    WaitingReviewer,
    WaitingEvidence,
    Complete,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ValidationEvidence {
    pub reference: String,
    pub description: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ValidationFeedback {
    pub author: String,
    pub message: String,
    pub actionable: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReviewerState {
    pub reviewer: String,
    pub independent: bool,
    pub settled: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub feedback: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ValidationCheck {
    pub id: String,
    pub label: String,
    pub lifecycle: CheckLifecycle,
    pub activity: CheckActivity,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub settled: Option<bool>,
    #[serde(default)]
    pub evidence: Vec<ValidationEvidence>,
    #[serde(default)]
    pub feedback: Vec<ValidationFeedback>,
    #[serde(default)]
    pub retry: Option<RetryRequest>,
    #[serde(default)]
    pub composite: Option<ValidationComposite>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum ValidationComposite {
    All(Vec<ValidationCheck>),
    Any(Vec<ValidationCheck>),
    Quorum {
        required: u32,
        checks: Vec<ValidationCheck>,
    },
    Ordered(Vec<ValidationCheck>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ValidationLifecycle {
    Draft,
    Submitted,
    Checking,
    Settled,
    Rejected,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ValidationSettlement {
    pub passed: bool,
    pub settled_at: DateTime<Utc>,
    pub reason: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalValidationFixture {
    pub candidate: CandidateSubmission,
    pub lifecycle: ValidationLifecycle,
    pub checks: Vec<ValidationCheck>,
    #[serde(default)]
    pub evidence: Vec<ValidationEvidence>,
    #[serde(default)]
    pub feedback: Vec<ValidationFeedback>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reviewer: Option<ReviewerState>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub settled: Option<ValidationSettlement>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub retry: Option<RetryRequest>,
}

/// Durable goal events are an input boundary, not transcript messages.  This
/// adapter folds the fields that the protocol actually carries into the
/// renderer-independent validation fixture used by the gallery and future
/// surfaces.  In particular, candidate submission never becomes a generic
/// note, user message, or assistant message.
#[derive(Debug, Default)]
pub struct GoalValidationAdapter {
    fixtures: std::collections::HashMap<firmius_core::GoalId, GoalValidationFixture>,
}

impl GoalValidationAdapter {
    pub fn fixture(&self, goal_id: firmius_core::GoalId) -> Option<&GoalValidationFixture> {
        self.fixtures.get(&goal_id)
    }

    pub fn availability(&self, goal_id: firmius_core::GoalId) -> GoalValidationAvailability<'_> {
        match self.fixtures.get(&goal_id) {
            Some(fixture) => GoalValidationAvailability::Available(fixture),
            None => GoalValidationAvailability::Unavailable {
                reason: Some("goal validation is unavailable"),
            },
        }
    }

    pub fn apply(&mut self, envelope: &GoalEventEnvelope) {
        let at = envelope.at.unwrap_or_else(Utc::now);
        match &envelope.event {
            GoalEventPayload::Created { goal } => {
                self.fixtures.insert(goal.id, fixture_from_goal(goal, at));
            }
            GoalEventPayload::CandidateSubmitted {
                goal_id, evidence, ..
            } => {
                let fixture = self
                    .fixtures
                    .entry(*goal_id)
                    .or_insert_with(|| empty_fixture(*goal_id, at));
                fixture.lifecycle = ValidationLifecycle::Submitted;
                fixture.candidate.lifecycle = CandidateLifecycle::Submitted;
                fixture.evidence.extend(evidence.iter().map(evidence_view));
            }
            GoalEventPayload::RunDispatched { goal_id, .. }
            | GoalEventPayload::Activated { goal_id, .. } => {
                let fixture = self
                    .fixtures
                    .entry(*goal_id)
                    .or_insert_with(|| empty_fixture(*goal_id, at));
                fixture.lifecycle = ValidationLifecycle::Checking;
                fixture.candidate.lifecycle = CandidateLifecycle::Checking;
            }
            GoalEventPayload::Waiting {
                goal_id, reason, ..
            } => {
                let fixture = self
                    .fixtures
                    .entry(*goal_id)
                    .or_insert_with(|| empty_fixture(*goal_id, at));
                fixture.lifecycle = ValidationLifecycle::Checking;
                fixture.candidate.lifecycle = CandidateLifecycle::Checking;
                let activity = match reason {
                    firmius_protocol::GoalWaitReason::Verification => {
                        CheckActivity::WaitingReviewer
                    }
                    _ => CheckActivity::WaitingEvidence,
                };
                for check in &mut fixture.checks {
                    if check.lifecycle == CheckLifecycle::Active {
                        check.activity = activity;
                    }
                }
            }
            GoalEventPayload::Checked {
                goal_id,
                evaluation,
                ..
            } => {
                let fixture = self
                    .fixtures
                    .entry(*goal_id)
                    .or_insert_with(|| empty_fixture(*goal_id, at));
                let passed = evaluation.state == CheckState::Passed;
                if let Some(check) = fixture
                    .checks
                    .iter_mut()
                    .find(|c| c.id == evaluation.check_id)
                {
                    check.lifecycle = if passed {
                        CheckLifecycle::Settled
                    } else {
                        CheckLifecycle::Failed
                    };
                    check.activity = CheckActivity::Complete;
                    check.settled = Some(passed);
                    check
                        .evidence
                        .extend(
                            evaluation
                                .evidence
                                .iter()
                                .map(|reference| ValidationEvidence {
                                    reference: reference.clone(),
                                    description: "durable check evidence".into(),
                                }),
                        );
                    if let Some(reviewer) = reviewer_from_evaluation(evaluation) {
                        fixture.reviewer = Some(reviewer);
                    }
                }
                fixture.lifecycle = ValidationLifecycle::Checking;
            }
            GoalEventPayload::StatusChanged {
                goal_id,
                status,
                reason,
                ..
            } => {
                self.apply_status(*goal_id, *status, reason.clone(), at);
            }
            GoalEventPayload::Transition {
                goal_id,
                to: status,
                ..
            } => {
                self.apply_status(*goal_id, *status, None, at);
            }
            GoalEventPayload::Cancelled {
                goal_id, reason, ..
            } => {
                let fixture = self
                    .fixtures
                    .entry(*goal_id)
                    .or_insert_with(|| empty_fixture(*goal_id, at));
                fixture.lifecycle = ValidationLifecycle::Rejected;
                fixture.candidate.lifecycle = CandidateLifecycle::Retry;
                fixture.settled = Some(ValidationSettlement {
                    passed: false,
                    settled_at: at,
                    reason: reason.clone().unwrap_or_else(|| "goal cancelled".into()),
                });
            }
            _ => {}
        }
    }

    fn apply_status(
        &mut self,
        goal_id: firmius_core::GoalId,
        status: GoalStatus,
        reason: Option<String>,
        at: DateTime<Utc>,
    ) {
        let fixture = self
            .fixtures
            .entry(goal_id)
            .or_insert_with(|| empty_fixture(goal_id, at));
        match status {
            GoalStatus::Succeeded => {
                fixture.lifecycle = ValidationLifecycle::Settled;
                fixture.candidate.lifecycle = CandidateLifecycle::Settled;
                fixture.settled = Some(ValidationSettlement {
                    passed: true,
                    settled_at: at,
                    reason: reason.unwrap_or_else(|| "goal succeeded".into()),
                });
            }
            GoalStatus::Failed | GoalStatus::Cancelled => {
                fixture.lifecycle = ValidationLifecycle::Rejected;
                fixture.candidate.lifecycle = CandidateLifecycle::Retry;
                fixture.settled = Some(ValidationSettlement {
                    passed: false,
                    settled_at: at,
                    reason: reason.unwrap_or_else(|| "goal did not settle successfully".into()),
                });
            }
            GoalStatus::Active | GoalStatus::Waiting => {
                fixture.lifecycle = ValidationLifecycle::Checking;
                fixture.candidate.lifecycle = CandidateLifecycle::Checking;
            }
            GoalStatus::Queued | GoalStatus::Blocked => {
                fixture.lifecycle = ValidationLifecycle::Submitted;
                fixture.candidate.lifecycle = CandidateLifecycle::Submitted;
            }
            GoalStatus::Proposed | GoalStatus::Cancelling => {}
        }
    }
}

fn empty_fixture(goal_id: firmius_core::GoalId, at: DateTime<Utc>) -> GoalValidationFixture {
    GoalValidationFixture {
        candidate: CandidateSubmission {
            id: goal_id.to_string(),
            summary: "durable goal candidate".into(),
            lifecycle: CandidateLifecycle::Candidate,
            submitted_at: at,
        },
        lifecycle: ValidationLifecycle::Draft,
        checks: Vec::new(),
        evidence: Vec::new(),
        feedback: Vec::new(),
        reviewer: None,
        settled: None,
        retry: None,
    }
}

fn fixture_from_goal(goal: &firmius_core::Goal, at: DateTime<Utc>) -> GoalValidationFixture {
    let mut fixture = empty_fixture(goal.id, at);
    fixture.candidate.summary = goal.description.clone();
    fixture.checks = goal.checks.iter().map(validation_check).collect();
    fixture
}

fn validation_check(check: &GoalCheck) -> ValidationCheck {
    let (label, composite) = match &check.kind {
        GoalCheckKind::Command(command) => (command.command.clone(), None),
        GoalCheckKind::Agent(agent) => (agent.criteria.join("; "), None),
        GoalCheckKind::Artifact(artifact) => (artifact.reference.clone(), None),
        GoalCheckKind::Event(event) => (event.event_type.clone(), None),
        GoalCheckKind::Composite(composite) => (
            format!("{:?} composite", composite.operator),
            Some(composite_view(composite)),
        ),
    };
    let activity = match &check.kind {
        GoalCheckKind::Agent(agent) if agent.independent_review => CheckActivity::WaitingReviewer,
        _ => CheckActivity::Queued,
    };
    ValidationCheck {
        id: check.id.clone(),
        label,
        lifecycle: CheckLifecycle::Pending,
        activity,
        settled: None,
        evidence: Vec::new(),
        feedback: Vec::new(),
        retry: None,
        composite,
    }
}

fn composite_view(check: &firmius_core::CompositeCheck) -> ValidationComposite {
    let checks = check.checks.iter().map(validation_check).collect();
    match check.operator {
        firmius_core::CompositeOperator::All => ValidationComposite::All(checks),
        firmius_core::CompositeOperator::Any => ValidationComposite::Any(checks),
        firmius_core::CompositeOperator::Quorum => ValidationComposite::Quorum {
            required: check.required.unwrap_or(1),
            checks,
        },
        firmius_core::CompositeOperator::Ordered => ValidationComposite::Ordered(checks),
    }
}

fn evidence_view(evidence: &GoalEvidence) -> ValidationEvidence {
    ValidationEvidence {
        reference: evidence.reference.clone(),
        description: evidence
            .media_type
            .clone()
            .unwrap_or_else(|| "durable candidate evidence".into()),
    }
}

fn reviewer_from_evaluation(evaluation: &firmius_core::CheckEvaluation) -> Option<ReviewerState> {
    let reviewer = match &evaluation.actor {
        firmius_core::GoalActor::Agent { agent_id } => agent_id.clone(),
        firmius_core::GoalActor::User { user_id } => user_id.clone(),
        firmius_core::GoalActor::Workflow { workflow_id } => workflow_id.clone(),
        firmius_core::GoalActor::System => "system".into(),
    };
    let independent = matches!(
        evaluation.verification,
        CheckVerification::IndependentlyVerified | CheckVerification::Reviewed
    );
    independent.then_some(ReviewerState {
        reviewer,
        independent,
        settled: true,
        feedback: None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn durable_goal_events_fold_without_transcript_fallback() {
        let goal = firmius_core::Goal::new(
            "ship endpoint",
            vec!["tests pass".into()],
            firmius_core::GoalOwner::User {
                user_id: "u".into(),
            },
            firmius_core::GoalProvenance {
                actor: firmius_core::GoalActor::User {
                    user_id: "u".into(),
                },
                source: firmius_core::GoalSource::UserRequest,
                created_at: Utc::now(),
            },
        )
        .unwrap();
        let envelope = GoalEventEnvelope {
            version: firmius_protocol::GOAL_PROTOCOL_VERSION,
            sequence: 1,
            event_id: uuid::Uuid::new_v4(),
            at: Some(Utc::now()),
            goal_id: goal.id,
            event: GoalEventPayload::Created { goal },
        };
        let mut adapter = GoalValidationAdapter::default();
        adapter.apply(&envelope);
        let fixture = adapter.fixture(envelope.goal_id).unwrap();
        assert_eq!(fixture.candidate.summary, "ship endpoint");
        assert!(fixture.feedback.is_empty());
        assert!(fixture.retry.is_none());
    }

    #[test]
    fn permission_administration_supports_staging_apply_and_discard() {
        let now = Utc::now();
        let mut state = PermissionPolicyAdministrationState::new(
            firmius_core::PermissionMode::Default,
            firmius_core::PermissionDecision::Ask,
            PermissionPolicyPersistenceScope::Account,
        );
        state.stage(
            PermissionPolicyDraft {
                mode: firmius_core::PermissionMode::Auto,
                exact_behavior: firmius_core::PermissionDecision::Allow,
                rules: vec![],
                scopes: vec![],
            },
            now,
        );
        assert!(state.apply_staged(now));
        assert_eq!(state.revision, 1);
        assert_eq!(state.audit_entries.len(), 2);
    }

    fn check(id: &str, lifecycle: CheckLifecycle, activity: CheckActivity) -> ValidationCheck {
        ValidationCheck {
            id: id.into(),
            label: format!("{id} check"),
            lifecycle,
            activity,
            settled: None,
            evidence: Vec::new(),
            feedback: Vec::new(),
            retry: None,
            composite: None,
        }
    }

    #[test]
    fn tool_state_preserves_permission_and_queue_context() {
        let now = Utc::now();
        let state = ToolExecutionState {
            batch_id: "batch-1".into(),
            queue_position: 2,
            queue_total: 3,
            predecessor: Some("read-1".into()),
            lifecycle: ToolExecutionLifecycle::WaitingPermission,
            permission: Some(PermissionGateState {
                request_id: "perm-1".into(),
                status: PermissionGateStatus::Waiting,
                requested_at: now,
                decision: None,
            }),
            classifier_reason: Some("writes outside workspace".into()),
            timestamps: ToolExecutionTimestamps::default(),
            output_tail: None,
            delegate: None,
        };
        let json = serde_json::to_value(&state).unwrap();
        assert_eq!(json["queue_position"], 2);
        assert_eq!(json["permission"]["status"], "waiting");
    }

    #[test]
    fn tool_batch_fixture_covers_queue_and_every_lifecycle() {
        let lifecycles = [
            ToolExecutionLifecycle::Queued,
            ToolExecutionLifecycle::Preparing,
            ToolExecutionLifecycle::Running,
            ToolExecutionLifecycle::Settled,
        ];
        for (position, lifecycle) in lifecycles.into_iter().enumerate() {
            let state = ToolExecutionState {
                batch_id: "batch-3-of-5".into(),
                queue_position: (position + 1) as u32,
                queue_total: 5,
                predecessor: (position > 0).then(|| format!("tool-{}", position)),
                lifecycle,
                permission: None,
                classifier_reason: None,
                timestamps: ToolExecutionTimestamps::default(),
                output_tail: None,
                delegate: None,
            };
            let json = serde_json::to_value(&state).unwrap();
            assert_eq!(json["batch_id"], "batch-3-of-5");
            assert_eq!(json["queue_total"], 5);
            assert_eq!(json["lifecycle"], serde_json::to_value(lifecycle).unwrap());
        }

        // A settled item can still carry the queue context and a terminal
        // classifier without being confused with a failed execution.
        let settled = ToolExecutionState {
            batch_id: "batch-3-of-5".into(),
            queue_position: 3,
            queue_total: 5,
            predecessor: None,
            lifecycle: ToolExecutionLifecycle::Settled,
            permission: None,
            classifier_reason: Some("exit status 0".into()),
            timestamps: ToolExecutionTimestamps::default(),
            output_tail: None,
            delegate: None,
        };
        assert_eq!(settled.classifier_reason.as_deref(), Some("exit status 0"));
    }

    #[test]
    fn unavailable_goal_validation_is_explicit() {
        let adapter = GoalValidationAdapter::default();
        let result = adapter.availability(firmius_core::GoalId::new());
        assert!(matches!(
            result,
            GoalValidationAvailability::Unavailable { .. }
        ));
    }

    #[test]
    fn permission_fixture_distinguishes_waiting_user_and_auto_reasons() {
        let now = Utc::now();
        let waiting = PermissionGateState {
            request_id: "perm-waiting".into(),
            status: PermissionGateStatus::Waiting,
            requested_at: now,
            decision: None,
        };
        assert!(waiting.decision.is_none());

        let decisions = [
            (
                PermissionGateStatus::UserAllowed,
                true,
                PermissionProvenance::User,
                "approved once",
            ),
            (
                PermissionGateStatus::UserDenied,
                false,
                PermissionProvenance::User,
                "not requested",
            ),
            (
                PermissionGateStatus::AutoAllowed,
                true,
                PermissionProvenance::Auto,
                "workspace policy",
            ),
            (
                PermissionGateStatus::AutoDenied,
                false,
                PermissionProvenance::Auto,
                "outside workspace",
            ),
        ];
        for (status, allowed, provenance, reason) in decisions {
            let gate = PermissionGateState {
                request_id: format!("perm-{reason}"),
                status,
                requested_at: now,
                decision: Some(PermissionDecision {
                    allowed,
                    provenance,
                    reason: Some(reason.into()),
                    decided_at: now,
                }),
            };
            let json = serde_json::to_value(&gate).unwrap();
            assert_eq!(json["decision"]["allowed"], allowed);
            assert_eq!(json["decision"]["reason"], reason);
        }
    }

    #[test]
    fn surface_states_cover_stale_failure_and_auth() {
        let immediate: SurfaceState<u32> = SurfaceState::Immediate(1);
        assert!(matches!(immediate, SurfaceState::Immediate(1)));
        let partial: SurfaceState<u32> = SurfaceState::Partial {
            value: 2,
            missing: vec!["sessions".into(), "quota".into()],
        };
        assert!(matches!(partial, SurfaceState::Partial { missing, .. } if missing.len() == 2));
        let stale: SurfaceState<u32> = SurfaceState::Stale {
            value: 7,
            age_seconds: 42,
        };
        assert!(matches!(
            stale,
            SurfaceState::Stale {
                age_seconds: 42,
                ..
            }
        ));
        let failed: SurfaceState<u32> = SurfaceState::Failed {
            error: "timeout".into(),
            previous: Some(7),
            retry: RetryRequest {
                scope: RetryScope::Item,
                reason: None,
            },
        };
        assert!(matches!(failed, SurfaceState::Failed { .. }));
        let auth: SurfaceState<u32> = SurfaceState::UnavailableAuth { reason: None };
        assert!(matches!(auth, SurfaceState::UnavailableAuth { .. }));

        for operation in [
            SessionOperation::Loading,
            SessionOperation::Resume,
            SessionOperation::Attach,
            SessionOperation::Replay,
        ] {
            let encoded = serde_json::to_value(operation).unwrap();
            assert!(encoded.as_str().is_some());
        }
    }

    #[test]
    fn goal_fixture_records_reviewer_evidence_feedback_and_composite() {
        let check = ValidationCheck {
            id: "tests".into(),
            label: "tests pass".into(),
            lifecycle: CheckLifecycle::Settled,
            activity: CheckActivity::Complete,
            settled: Some(true),
            evidence: vec![ValidationEvidence {
                reference: "artifact://test.log".into(),
                description: "cargo test".into(),
            }],
            feedback: vec![ValidationFeedback {
                author: "reviewer".into(),
                message: "Looks good".into(),
                actionable: false,
            }],
            retry: None,
            composite: None,
        };
        let composite = ValidationComposite::All(vec![check.clone()]);
        assert!(matches!(composite, ValidationComposite::All(_)));
        let fixture = GoalValidationFixture {
            candidate: CandidateSubmission {
                id: "candidate-1".into(),
                summary: "patch".into(),
                lifecycle: CandidateLifecycle::Settled,
                submitted_at: Utc::now(),
            },
            lifecycle: ValidationLifecycle::Settled,
            checks: vec![check],
            evidence: vec![],
            feedback: vec![],
            reviewer: Some(ReviewerState {
                reviewer: "reviewer".into(),
                independent: true,
                settled: true,
                feedback: None,
            }),
            settled: Some(ValidationSettlement {
                passed: true,
                settled_at: Utc::now(),
                reason: "all checks passed".into(),
            }),
            retry: None,
        };
        assert!(fixture.settled.as_ref().is_some_and(|s| s.passed));
    }

    #[test]
    fn goal_fixture_covers_candidate_check_activity_retry_and_all_composites() {
        let mut pending = check("pending", CheckLifecycle::Pending, CheckActivity::Queued);
        pending.retry = Some(RetryRequest {
            scope: RetryScope::Item,
            reason: Some("flaky test".into()),
        });
        let active = check("active", CheckLifecycle::Active, CheckActivity::Running);
        let reviewer = check(
            "reviewer",
            CheckLifecycle::Active,
            CheckActivity::WaitingReviewer,
        );
        let evidence = check(
            "evidence",
            CheckLifecycle::Active,
            CheckActivity::WaitingEvidence,
        );
        let settled = check("settled", CheckLifecycle::Settled, CheckActivity::Complete);

        let composites = vec![
            ValidationComposite::All(vec![pending.clone(), active.clone()]),
            ValidationComposite::Any(vec![reviewer.clone(), evidence.clone()]),
            ValidationComposite::Quorum {
                required: 2,
                checks: vec![active.clone(), settled.clone()],
            },
            ValidationComposite::Ordered(vec![pending.clone(), settled.clone()]),
        ];
        assert!(matches!(composites[0], ValidationComposite::All(_)));
        assert!(matches!(composites[1], ValidationComposite::Any(_)));
        assert!(matches!(
            composites[2],
            ValidationComposite::Quorum { required: 2, .. }
        ));
        assert!(matches!(composites[3], ValidationComposite::Ordered(_)));

        let fixture = GoalValidationFixture {
            candidate: CandidateSubmission {
                id: "candidate-retry".into(),
                summary: "candidate with a retry".into(),
                lifecycle: CandidateLifecycle::Retry,
                submitted_at: Utc::now(),
            },
            lifecycle: ValidationLifecycle::Checking,
            checks: vec![pending],
            evidence: vec![ValidationEvidence {
                reference: "artifact://coverage".into(),
                description: "coverage report".into(),
            }],
            feedback: vec![ValidationFeedback {
                author: "reviewer".into(),
                message: "retry this check".into(),
                actionable: true,
            }],
            reviewer: Some(ReviewerState {
                reviewer: "independent-reviewer".into(),
                independent: true,
                settled: false,
                feedback: Some("needs another run".into()),
            }),
            settled: None,
            retry: Some(RetryRequest {
                scope: RetryScope::Surface,
                reason: Some("review feedback".into()),
            }),
        };
        assert_eq!(
            fixture.checks[0].retry.as_ref().unwrap().scope,
            RetryScope::Item
        );
        assert!(fixture.feedback[0].actionable);
        assert!(fixture.reviewer.as_ref().unwrap().independent);
    }
}
