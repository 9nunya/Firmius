//! Durable goals and their state transitions.
//!
//! Goals are intentionally a small, pure domain model.  A service can persist
//! a [`Goal`] after each successful call to [`Goal::transition`], and recover
//! it without retaining any process-local futures or handles.

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeSet;
use uuid::Uuid;

/// A stable identifier for a goal.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct GoalId(Uuid);

impl GoalId {
    pub fn new() -> Self {
        Self(Uuid::new_v4())
    }
    pub fn parse(value: &str) -> Result<Self, uuid::Error> {
        Ok(Self(Uuid::parse_str(value.trim())?))
    }
    pub fn as_uuid(self) -> Uuid {
        self.0
    }
}

/// Immutable snapshot of the candidate and evidence presented to a goal
/// reviewer. The digest is computed by the runtime from the other fields;
/// model-authored text is never trusted as the identity of reviewed work.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReviewEvidenceCapsule {
    pub goal_id: GoalId,
    pub run_id: String,
    pub generation: u64,
    pub worker_id: String,
    pub candidate_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(default)]
    pub criteria: Vec<String>,
    #[serde(default)]
    pub changed_files: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub diff: Option<String>,
    #[serde(default)]
    pub artifact_refs: Vec<String>,
    #[serde(default)]
    pub check_evidence: Vec<String>,
    pub digest: String,
}

/// Runtime-issued provenance for a review verdict. Consumers can bind a
/// verdict to the exact evidence capsule without trusting a worker/config
/// claim of reviewer authority.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReviewAttestation {
    pub reviewer_id: String,
    pub worker_id: String,
    pub candidate_id: String,
    pub capsule_digest: String,
    pub verdict: String,
    #[serde(default)]
    pub independent: bool,
}

/// Split a command-line check into an executable and arguments without going
/// through a shell. Quotes group whitespace and backslashes escape the next
/// character; shell operators are intentionally treated as ordinary text.
///
/// This is also used when evaluating goals loaded from older files, where a
/// command may still be stored in the single `command` field.
pub fn parse_command_line(line: &str) -> Result<(String, Vec<String>), String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut quote = None;
    let mut escaped = false;
    let mut started = false;

    for ch in line.chars() {
        if escaped {
            current.push(ch);
            escaped = false;
            started = true;
            continue;
        }
        match quote {
            Some(q) if ch == q => {
                quote = None;
                started = true;
            }
            Some('\'') => {
                current.push(ch);
                started = true;
            }
            Some(_) => {
                if ch == '\\' {
                    escaped = true;
                } else {
                    current.push(ch);
                }
                started = true;
            }
            None => match ch {
                '\'' | '"' => {
                    quote = Some(ch);
                    started = true;
                }
                '\\' => {
                    escaped = true;
                    started = true;
                }
                ch if ch.is_whitespace() => {
                    if started {
                        tokens.push(std::mem::take(&mut current));
                        started = false;
                    }
                }
                _ => {
                    current.push(ch);
                    started = true;
                }
            },
        }
    }

    if escaped {
        return Err("command ends with an escape".into());
    }
    if quote.is_some() {
        return Err("command has unterminated quotes".into());
    }
    if started {
        tokens.push(current);
    }
    let Some((executable, args)) = tokens.split_first() else {
        return Err("command must not be empty".into());
    };
    if executable.is_empty() {
        return Err("command executable must not be empty".into());
    }
    Ok((executable.clone(), args.to_vec()))
}
impl Default for GoalId {
    fn default() -> Self {
        Self::new()
    }
}
impl std::fmt::Display for GoalId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.0.fmt(f)
    }
}
impl From<Uuid> for GoalId {
    fn from(value: Uuid) -> Self {
        Self(value)
    }
}
impl From<GoalId> for Uuid {
    fn from(value: GoalId) -> Self {
        value.0
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum GoalOwner {
    User {
        user_id: String,
    },
    ParentAgent {
        agent_id: String,
    },
    Subagent {
        agent_id: String,
        parent_agent_id: String,
    },
    Workflow {
        workflow_id: String,
    },
    Agent {
        agent_id: String,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum GoalActor {
    User { user_id: String },
    Agent { agent_id: String },
    Workflow { workflow_id: String },
    System,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum GoalSource {
    UserRequest,
    ExplicitCommand,
    AgentProposal { agent_id: String },
    ParentAgent { agent_id: String },
    Workflow { workflow_id: String },
    FollowUp { goal_id: GoalId },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalProvenance {
    pub actor: GoalActor,
    pub source: GoalSource,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum GoalStatus {
    #[default]
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

impl GoalStatus {
    pub fn terminal(self) -> bool {
        matches!(self, Self::Succeeded | Self::Failed | Self::Cancelled)
    }

    /// An agent execution slot is held only while the goal is actively
    /// running or fenced for cancellation. Waiting, queued, and terminal
    /// goals occupy no slot.
    pub fn occupies_slot(self) -> bool {
        matches!(self, Self::Active | Self::Cancelling)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalBudget {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_cost: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_steps: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct GoalApproval {
    pub required: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub approved_by: Option<GoalActor>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub approved_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CheckVerification {
    Unverified,
    SelfVerified,
    Reviewed,
    IndependentlyVerified,
}
impl Default for CheckVerification {
    fn default() -> Self {
        Self::Unverified
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CheckState {
    Pending,
    Passed,
    Failed,
}
impl Default for CheckState {
    fn default() -> Self {
        Self::Pending
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CommandCheck {
    pub command: String,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cwd: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expected_exit_code: Option<i32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AgentCheck {
    pub agent_id: String,
    pub criteria: Vec<String>,
    #[serde(default)]
    pub independent_review: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ArtifactCheckKind {
    Exists,
    Content,
    Diff,
    Schema,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ArtifactCheck {
    pub reference: String,
    pub kind: ArtifactCheckKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expected: Option<Value>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventCheck {
    pub event_type: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub key: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expected_state: Option<Value>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CompositeOperator {
    All,
    Any,
    Quorum,
    Ordered,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompositeCheck {
    pub operator: CompositeOperator,
    pub checks: Vec<GoalCheck>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub required: Option<u32>,
}

/// Pluggable evidence checks.  `id` is stable and evaluations are append-only.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalCheck {
    pub id: String,
    #[serde(flatten)]
    pub kind: GoalCheckKind,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", content = "data", rename_all = "snake_case")]
pub enum GoalCheckKind {
    Command(CommandCheck),
    Agent(AgentCheck),
    Artifact(ArtifactCheck),
    Event(EventCheck),
    Composite(CompositeCheck),
}

impl GoalCheck {
    pub fn command(command: impl Into<String>) -> Self {
        let command = command.into();
        // Keep this constructor infallible for compatibility with callers
        // that build checks programmatically. Evaluation re-parses the raw
        // value and rejects malformed quoting; valid command lines are split
        // here so newly-created goals persist the safe argv representation.
        let (command, args) =
            parse_command_line(&command).unwrap_or_else(|_| (command, Vec::new()));
        Self {
            id: Uuid::new_v4().to_string(),
            kind: GoalCheckKind::Command(CommandCheck {
                command,
                args,
                cwd: None,
                expected_exit_code: Some(0),
            }),
        }
    }

    /// Fallible command-check construction for user supplied command lines.
    /// Unlike [`Self::command`], this rejects malformed quoting immediately.
    pub fn try_command(command: impl Into<String>) -> Result<Self, GoalError> {
        let (command, args) =
            parse_command_line(&command.into()).map_err(GoalError::InvalidCheck)?;
        Ok(Self {
            id: Uuid::new_v4().to_string(),
            kind: GoalCheckKind::Command(CommandCheck {
                command,
                args,
                cwd: None,
                expected_exit_code: Some(0),
            }),
        })
    }
    pub fn agent(agent_id: impl Into<String>, criteria: Vec<String>) -> Self {
        Self {
            id: Uuid::new_v4().to_string(),
            kind: GoalCheckKind::Agent(AgentCheck {
                agent_id: agent_id.into(),
                criteria,
                independent_review: false,
            }),
        }
    }

    /// Default `/goal` check: a different agent must independently verify
    /// the worker's result. The worker cannot grade itself.
    pub fn independent_agent(agent_id: impl Into<String>, criteria: Vec<String>) -> Self {
        Self {
            id: Uuid::new_v4().to_string(),
            kind: GoalCheckKind::Agent(AgentCheck {
                agent_id: agent_id.into(),
                criteria,
                independent_review: true,
            }),
        }
    }
    pub fn artifact(reference: impl Into<String>, kind: ArtifactCheckKind) -> Self {
        Self {
            id: Uuid::new_v4().to_string(),
            kind: GoalCheckKind::Artifact(ArtifactCheck {
                reference: reference.into(),
                kind,
                expected: None,
            }),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CheckEvaluation {
    pub check_id: String,
    pub state: CheckState,
    #[serde(default)]
    pub inputs: Value,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output: Option<Value>,
    pub evaluated_at: DateTime<Utc>,
    pub actor: GoalActor,
    #[serde(default)]
    pub evidence: Vec<String>,
    #[serde(default)]
    pub verification: CheckVerification,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub review: Option<ReviewAttestation>,
}

impl CheckEvaluation {
    pub fn passed(&self) -> bool {
        self.state == CheckState::Passed
            && !self.evidence.is_empty()
            && !matches!(self.verification, CheckVerification::Unverified)
    }

    /// Independent verification is meaningful only when it is bound to a
    /// runtime-issued reviewer attestation. A verification enum by itself is
    /// merely a claim made by the evaluation payload.
    pub fn has_independent_attestation(&self) -> bool {
        matches!(self.verification, CheckVerification::IndependentlyVerified)
            && self.review.as_ref().is_some_and(|review| {
                review.independent
                    && review.reviewer_id == actor_id(&self.actor)
                    && review.worker_id != review.reviewer_id
                    && !review.candidate_id.trim().is_empty()
                    && !review.capsule_digest.trim().is_empty()
            })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct GoalLinks {
    /// Session and agent that accepted this goal, when it was launched from
    /// an attached daemon session.  These are optional so goals created by
    /// offline clients remain fully backwards compatible.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub agent_id: Option<String>,
    #[serde(default)]
    pub workflow_nodes: Vec<String>,
    #[serde(default)]
    pub operations: Vec<String>,
    #[serde(default)]
    pub artifacts: Vec<String>,
    #[serde(default)]
    pub evidence: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GoalEvent {
    pub id: Uuid,
    pub goal_id: GoalId,
    pub at: DateTime<Utc>,
    pub actor: GoalActor,
    pub transition: GoalTransition,
    pub from: GoalStatus,
    pub to: GoalStatus,
    pub revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum GoalTransition {
    Activate,
    Queue,
    Requeue { reason: String },
    Wait { reason: String },
    Block { reason: String },
    Succeed,
    Fail { reason: String },
    Cancel { reason: String },
    RequestCancel { reason: String },
    Approve,
    RejectApproval { reason: String },
    Evaluate(CheckEvaluation),
    AddCheck(GoalCheck),
    LinkArtifact { reference: String },
    LinkEvidence { reference: String },
}

#[derive(Debug, Clone, thiserror::Error, PartialEq, Eq)]
pub enum GoalError {
    #[error("goal description must not be empty")]
    EmptyDescription,
    #[error("goal must have at least one success condition")]
    EmptySuccessConditions,
    #[error("goal has no checks")]
    NoChecks,
    #[error("check id is empty or duplicated: {0}")]
    InvalidCheckId(String),
    #[error("check is invalid: {0}")]
    InvalidCheck(String),
    #[error("invalid lifecycle transition from {from:?} to {to:?}")]
    InvalidTransition { from: GoalStatus, to: GoalStatus },
    #[error("approval is required before activation")]
    ApprovalRequired,
    #[error("goal cannot succeed until all required checks pass")]
    ChecksNotSatisfied,
    #[error("unknown check: {0}")]
    UnknownCheck(String),
    #[error("check evaluation actor is not the designated reviewer")]
    WrongCheckActor,
    #[error("stale goal revision: expected {expected}, actual {actual}")]
    StaleRevision { expected: u64, actual: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Goal {
    pub id: GoalId,
    pub description: String,
    pub success_conditions: Vec<String>,
    pub owner: GoalOwner,
    pub provenance: GoalProvenance,
    #[serde(default)]
    pub status: GoalStatus,
    #[serde(default)]
    pub checks: Vec<GoalCheck>,
    #[serde(default)]
    pub evaluations: Vec<CheckEvaluation>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub deadline: Option<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub budget: Option<GoalBudget>,
    #[serde(default)]
    pub approval: GoalApproval,
    #[serde(default)]
    pub links: GoalLinks,
    #[serde(default)]
    pub events: Vec<GoalEvent>,
    #[serde(default)]
    pub revision: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub status_reason: Option<String>,
}

impl Goal {
    pub fn new(
        description: impl Into<String>,
        success_conditions: Vec<String>,
        owner: GoalOwner,
        provenance: GoalProvenance,
    ) -> Result<Self, GoalError> {
        let goal = Self {
            id: GoalId::new(),
            description: description.into(),
            success_conditions,
            owner,
            provenance,
            status: GoalStatus::Proposed,
            checks: vec![],
            evaluations: vec![],
            deadline: None,
            budget: None,
            approval: GoalApproval {
                required: false,
                approved_by: None,
                approved_at: None,
            },
            links: GoalLinks::default(),
            events: vec![],
            revision: 0,
            status_reason: None,
        };
        goal.validate()?;
        Ok(goal)
    }

    pub fn validate(&self) -> Result<(), GoalError> {
        if self.description.trim().is_empty() {
            return Err(GoalError::EmptyDescription);
        }
        if self.success_conditions.is_empty()
            || self.success_conditions.iter().all(|c| c.trim().is_empty())
        {
            return Err(GoalError::EmptySuccessConditions);
        }
        if self.success_conditions.iter().any(|c| c.trim().is_empty()) {
            return Err(GoalError::EmptySuccessConditions);
        }
        let mut ids = BTreeSet::new();
        for check in &self.checks {
            validate_check(check, &mut ids)?;
        }
        if self.approval.required
            && self.approval.approved_by.is_some() != self.approval.approved_at.is_some()
        {
            return Err(GoalError::InvalidCheck(
                "approval metadata is incomplete".into(),
            ));
        }
        Ok(())
    }

    pub fn checks_satisfied(&self) -> bool {
        if self.checks.is_empty() {
            return false;
        }
        self.checks.iter().all(|check| self.check_satisfied(check))
    }

    fn check_satisfied(&self, check: &GoalCheck) -> bool {
        match &check.kind {
            GoalCheckKind::Composite(c) => {
                let passed = c
                    .checks
                    .iter()
                    .filter(|child| self.check_satisfied(child))
                    .count() as u32;
                match c.operator {
                    CompositeOperator::All | CompositeOperator::Ordered => {
                        passed == c.checks.len() as u32
                    }
                    CompositeOperator::Any => passed > 0,
                    CompositeOperator::Quorum => passed >= c.required.unwrap_or(1),
                }
            }
            GoalCheckKind::Agent(spec) => self
                .evaluations
                .iter()
                .rev()
                .find(|e| e.check_id == check.id)
                .is_some_and(|evaluation| {
                    evaluation.passed()
                        && !matches!(evaluation.verification, CheckVerification::Unverified)
                        && (!spec.independent_review || evaluation.has_independent_attestation())
                }),
            _ => self
                .evaluations
                .iter()
                .rev()
                .find(|e| e.check_id == check.id)
                .is_some_and(CheckEvaluation::passed),
        }
    }

    fn find_check<'a>(checks: &'a [GoalCheck], id: &str) -> Option<&'a GoalCheck> {
        checks.iter().find_map(|check| {
            if check.id == id {
                Some(check)
            } else if let GoalCheckKind::Composite(composite) = &check.kind {
                Self::find_check(&composite.checks, id)
            } else {
                None
            }
        })
    }

    /// Apply a transition only if the caller read the current durable
    /// revision. This is the service-facing, optimistic-concurrency API.
    pub fn apply(
        &mut self,
        expected_revision: u64,
        actor: GoalActor,
        transition: GoalTransition,
    ) -> Result<GoalEvent, GoalError> {
        if expected_revision != self.revision {
            return Err(GoalError::StaleRevision {
                expected: expected_revision,
                actual: self.revision,
            });
        }
        self.transition(actor, transition)
    }

    pub fn transition(
        &mut self,
        actor: GoalActor,
        transition: GoalTransition,
    ) -> Result<GoalEvent, GoalError> {
        let from = self.status;
        if from.terminal() {
            return Err(GoalError::InvalidTransition { from, to: from });
        }
        let (to, reason) = match &transition {
            GoalTransition::Activate => {
                if self.approval.required && self.approval.approved_by.is_none() {
                    return Err(GoalError::ApprovalRequired);
                }
                if self.checks.is_empty() {
                    return Err(GoalError::NoChecks);
                }
                (GoalStatus::Active, None)
            }
            GoalTransition::Queue => (GoalStatus::Queued, None),
            GoalTransition::Requeue { reason } => (GoalStatus::Queued, Some(reason.clone())),
            GoalTransition::Wait { reason } => (GoalStatus::Waiting, Some(reason.clone())),
            GoalTransition::Block { reason } => (GoalStatus::Blocked, Some(reason.clone())),
            GoalTransition::Succeed => {
                if !self.checks_satisfied() {
                    return Err(GoalError::ChecksNotSatisfied);
                }
                (GoalStatus::Succeeded, None)
            }
            GoalTransition::Fail { reason } => (GoalStatus::Failed, Some(reason.clone())),
            GoalTransition::Cancel { reason } => (GoalStatus::Cancelled, Some(reason.clone())),
            GoalTransition::RequestCancel { reason } => {
                (GoalStatus::Cancelling, Some(reason.clone()))
            }
            GoalTransition::Approve => {
                self.approval.approved_by = Some(actor.clone());
                self.approval.approved_at = Some(Utc::now());
                return self.record_event(actor, transition, from, from, None);
            }
            GoalTransition::RejectApproval { reason } => {
                (GoalStatus::Blocked, Some(reason.clone()))
            }
            GoalTransition::Evaluate(evaluation) => {
                let check = Self::find_check(&self.checks, &evaluation.check_id)
                    .ok_or_else(|| GoalError::UnknownCheck(evaluation.check_id.clone()))?;
                if let GoalCheckKind::Agent(spec) = &check.kind {
                    if spec.independent_review {
                        if !evaluation.has_independent_attestation()
                            || actor_id(&actor) != actor_id(&evaluation.actor)
                        {
                            return Err(GoalError::WrongCheckActor);
                        }
                    } else if spec.agent_id != actor_id(&actor) {
                        return Err(GoalError::WrongCheckActor);
                    }
                }
                self.evaluations.push(evaluation.clone());
                return self.record_event(actor, transition, from, from, None);
            }
            GoalTransition::AddCheck(check) => {
                validate_check(check, &mut self.check_ids())?;
                self.checks.push(check.clone());
                return self.record_event(actor, transition, from, from, None);
            }
            GoalTransition::LinkArtifact { reference } => {
                if !self.links.artifacts.contains(reference) {
                    self.links.artifacts.push(reference.clone());
                }
                return self.record_event(actor, transition, from, from, None);
            }
            GoalTransition::LinkEvidence { reference } => {
                if !self.links.evidence.contains(reference) {
                    self.links.evidence.push(reference.clone());
                }
                return self.record_event(actor, transition, from, from, None);
            }
        };
        if from.terminal() || !valid_transition(from, to) {
            return Err(GoalError::InvalidTransition { from, to });
        }
        self.status = to;
        self.status_reason = reason.clone();
        self.record_event(actor, transition, from, to, reason)
    }

    fn check_ids(&self) -> BTreeSet<String> {
        self.checks.iter().map(|c| c.id.clone()).collect()
    }
    fn record_event(
        &mut self,
        actor: GoalActor,
        transition: GoalTransition,
        from: GoalStatus,
        to: GoalStatus,
        reason: Option<String>,
    ) -> Result<GoalEvent, GoalError> {
        self.revision = self.revision.saturating_add(1);
        if reason.is_some() {
            self.status_reason = reason;
        }
        let event = GoalEvent {
            id: Uuid::new_v4(),
            goal_id: self.id,
            at: Utc::now(),
            actor,
            transition,
            from,
            to,
            revision: self.revision,
        };
        self.events.push(event.clone());
        Ok(event)
    }
}

fn actor_id(actor: &GoalActor) -> &str {
    match actor {
        GoalActor::Agent { agent_id } => agent_id,
        GoalActor::User { user_id } => user_id,
        GoalActor::Workflow { workflow_id } => workflow_id,
        GoalActor::System => "system",
    }
}

fn validate_check(check: &GoalCheck, ids: &mut BTreeSet<String>) -> Result<(), GoalError> {
    if check.id.trim().is_empty() || !ids.insert(check.id.clone()) {
        return Err(GoalError::InvalidCheckId(check.id.clone()));
    }
    match &check.kind {
        GoalCheckKind::Command(c) if c.command.trim().is_empty() => {
            Err(GoalError::InvalidCheck("command must not be empty".into()))
        }
        // `command` is an executable name when `args` is empty. Do not run a
        // shell grammar validator over it: natural-language goal creation and
        // older persisted records may contain punctuation (including quotes),
        // and the OS is the authority on whether the executable can run.
        GoalCheckKind::Command(_) => Ok(()),
        GoalCheckKind::Agent(c)
            if c.agent_id.trim().is_empty()
                || c.criteria.is_empty()
                || c.criteria.iter().all(|v| v.trim().is_empty()) =>
        {
            Err(GoalError::InvalidCheck(
                "agent checks need an agent and criteria".into(),
            ))
        }
        GoalCheckKind::Artifact(c) if c.reference.trim().is_empty() => Err(
            GoalError::InvalidCheck("artifact reference must not be empty".into()),
        ),
        GoalCheckKind::Event(c) if c.event_type.trim().is_empty() => Err(GoalError::InvalidCheck(
            "event type must not be empty".into(),
        )),
        GoalCheckKind::Composite(c) if c.checks.is_empty() => Err(GoalError::InvalidCheck(
            "composite check must not be empty".into(),
        )),
        GoalCheckKind::Composite(c) => {
            for child in &c.checks {
                validate_check(child, ids)?;
            }
            Ok(())
        }
        _ => Ok(()),
    }
}

fn valid_transition(from: GoalStatus, to: GoalStatus) -> bool {
    matches!(
        (from, to),
        (
            GoalStatus::Proposed,
            GoalStatus::Queued
                | GoalStatus::Active
                | GoalStatus::Waiting
                | GoalStatus::Cancelled
                | GoalStatus::Blocked
        ) | (
            GoalStatus::Queued,
            GoalStatus::Active | GoalStatus::Waiting | GoalStatus::Blocked | GoalStatus::Cancelled
        ) | (
            GoalStatus::Active,
            GoalStatus::Waiting
                | GoalStatus::Blocked
                | GoalStatus::Succeeded
                | GoalStatus::Failed
                | GoalStatus::Cancelled
                | GoalStatus::Queued
                | GoalStatus::Cancelling
        ) | (
            GoalStatus::Waiting,
            GoalStatus::Active
                | GoalStatus::Blocked
                | GoalStatus::Cancelled
                | GoalStatus::Succeeded
                | GoalStatus::Failed
                | GoalStatus::Queued
        ) | (
            GoalStatus::Blocked,
            GoalStatus::Active | GoalStatus::Cancelled | GoalStatus::Failed | GoalStatus::Queued
        ) | (GoalStatus::Cancelling, GoalStatus::Cancelled)
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn command_lines_are_split_without_shell_execution() {
        assert_eq!(
            parse_command_line("cargo test -p firmius-service").unwrap(),
            (
                "cargo".to_string(),
                vec![
                    "test".to_string(),
                    "-p".to_string(),
                    "firmius-service".to_string()
                ]
            )
        );
        assert_eq!(
            parse_command_line("printf 'hello world'").unwrap(),
            ("printf".to_string(), vec!["hello world".to_string()])
        );
    }

    #[test]
    fn malformed_command_quotes_are_rejected() {
        assert!(parse_command_line("cargo test '").is_err());
        assert!(GoalCheck::try_command("cargo test \\").is_err());
    }

    #[test]
    fn command_constructor_persists_executable_and_args() {
        let GoalCheckKind::Command(check) = GoalCheck::try_command("cargo test -p firmius-service")
            .unwrap()
            .kind
        else {
            unreachable!()
        };
        assert_eq!(check.command, "cargo");
        assert_eq!(check.args, ["test", "-p", "firmius-service"]);
    }

    fn provenance() -> GoalProvenance {
        GoalProvenance {
            actor: GoalActor::User {
                user_id: "u".into(),
            },
            source: GoalSource::UserRequest,
            created_at: Utc::now(),
        }
    }
    fn goal() -> Goal {
        Goal::new(
            "keep tests green",
            vec!["exit zero".into()],
            GoalOwner::User {
                user_id: "u".into(),
            },
            provenance(),
        )
        .unwrap()
    }
    fn eval(id: String) -> CheckEvaluation {
        CheckEvaluation {
            check_id: id,
            state: CheckState::Passed,
            inputs: Value::Null,
            output: Some(Value::String("ok".into())),
            evaluated_at: Utc::now(),
            actor: GoalActor::System,
            evidence: vec!["artifact://result".into()],
            verification: CheckVerification::Reviewed,
            review: None,
        }
    }
    #[test]
    fn lifecycle_requires_evidence_and_records_events() {
        let mut g = goal();
        let check = GoalCheck::command("cargo test");
        let id = check.id.clone();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::AddCheck(check),
        )
        .unwrap();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Activate,
        )
        .unwrap();
        assert!(
            g.transition(
                GoalActor::User {
                    user_id: "u".into()
                },
                GoalTransition::Succeed
            )
            .is_err()
        );
        g.transition(GoalActor::System, GoalTransition::Evaluate(eval(id)))
            .unwrap();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Succeed,
        )
        .unwrap();
        assert_eq!(g.status, GoalStatus::Succeeded);
        assert_eq!(g.revision, 4);
    }

    #[test]
    fn independent_review_requires_a_runtime_attestation_not_only_a_verification_claim() {
        let mut g = goal();
        let check =
            GoalCheck::independent_agent("configured-reviewer", vec!["inspect diff".into()]);
        let check_id = check.id.clone();
        g.transition(GoalActor::System, GoalTransition::AddCheck(check))
            .unwrap();

        let reviewer = GoalActor::Agent {
            agent_id: "fresh-runtime-reviewer".into(),
        };
        let claimed = CheckEvaluation {
            check_id: check_id.clone(),
            state: CheckState::Passed,
            inputs: Value::Null,
            output: Some(Value::String("PASS".into())),
            evaluated_at: Utc::now(),
            actor: reviewer.clone(),
            evidence: vec!["artifact://review".into()],
            verification: CheckVerification::IndependentlyVerified,
            review: None,
        };
        assert_eq!(
            g.transition(reviewer.clone(), GoalTransition::Evaluate(claimed)),
            Err(GoalError::WrongCheckActor)
        );

        let attested = CheckEvaluation {
            check_id,
            state: CheckState::Passed,
            inputs: Value::Null,
            output: Some(Value::String("PASS".into())),
            evaluated_at: Utc::now(),
            actor: reviewer.clone(),
            evidence: vec!["artifact://review".into()],
            verification: CheckVerification::IndependentlyVerified,
            review: Some(ReviewAttestation {
                reviewer_id: "fresh-runtime-reviewer".into(),
                worker_id: "worker".into(),
                candidate_id: "candidate-v1".into(),
                capsule_digest: "capsule-v1".into(),
                verdict: "PASS".into(),
                independent: true,
            }),
        };
        g.transition(reviewer, GoalTransition::Evaluate(attested))
            .unwrap();
        assert!(g.checks_satisfied());
    }

    #[test]
    fn approval_and_composite_work() {
        let mut g = goal();
        g.approval.required = true;
        assert!(matches!(
            g.transition(
                GoalActor::User {
                    user_id: "u".into()
                },
                GoalTransition::Activate
            ),
            Err(GoalError::ApprovalRequired)
        ));
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::AddCheck(GoalCheck::command("true")),
        )
        .unwrap();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Approve,
        )
        .unwrap();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Activate,
        )
        .unwrap();
        assert_eq!(g.status, GoalStatus::Active);
    }
    #[test]
    fn serde_round_trip_preserves_durable_history() {
        let mut g = goal();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Wait {
                reason: "approval".into(),
            },
        )
        .unwrap();
        let encoded = serde_json::to_vec(&g).unwrap();
        let decoded: Goal = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(decoded, g);
    }

    #[test]
    fn queue_requeue_and_cancelling_are_compatible_lifecycle_states() {
        let mut g = goal();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Queue,
        )
        .unwrap();
        assert_eq!(g.status, GoalStatus::Queued);
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::AddCheck(GoalCheck::command("cargo test")),
        )
        .unwrap();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Activate,
        )
        .unwrap();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Requeue {
                reason: "retry".into(),
            },
        )
        .unwrap();
        assert_eq!(g.status, GoalStatus::Queued);
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Activate,
        )
        .unwrap();
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::RequestCancel {
                reason: "stop".into(),
            },
        )
        .unwrap();
        assert_eq!(g.status, GoalStatus::Cancelling);
        assert!(!g.status.terminal());
        assert!(g.status.occupies_slot());
        g.transition(
            GoalActor::User {
                user_id: "u".into(),
            },
            GoalTransition::Cancel {
                reason: "stopped".into(),
            },
        )
        .unwrap();
        assert_eq!(g.status, GoalStatus::Cancelled);
        assert!(g.status.terminal());
        // Direct Proposed -> Active remains valid for existing callers.
        let mut legacy = goal();
        legacy
            .transition(
                GoalActor::User {
                    user_id: "u".into(),
                },
                GoalTransition::AddCheck(GoalCheck::command("true")),
            )
            .unwrap();
        legacy
            .transition(
                GoalActor::User {
                    user_id: "u".into(),
                },
                GoalTransition::Activate,
            )
            .unwrap();
        assert_eq!(legacy.status, GoalStatus::Active);
    }
}
