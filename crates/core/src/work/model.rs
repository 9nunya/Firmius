//! Persistent, target-shaped WorkGraph records.

use super::ids::*;
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum GraphMode {
    #[default]
    Advisory,
    Managed,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkNotification {
    pub id: ResultId,
    pub parent_agent_id: String,
    pub child_agent_id: String,
    pub assignment_id: AssignmentId,
    pub result_id: ResultId,
    pub message: String,
    pub created_at: DateTime<Utc>,
    #[serde(default)]
    pub delivered: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AgentWorkBinding {
    pub graph_id: GraphId,
    pub node_id: NodeId,
    pub attempt_id: AttemptId,
    pub assignment_id: AssignmentId,
    #[serde(default)]
    pub task_id: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum GraphStatus {
    #[default]
    Active,
    Completed,
    Cancelled,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum ExecutionStatus {
    #[default]
    Pending,
    Ready,
    Running,
    Succeeded,
    Failed,
    Blocked,
    Cancelled,
    Skipped,
    Interrupted,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    Success,
    Failure,
    TestFailed,
    Blocked,
    Cancelled,
    Interrupted,
    Custom(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum VerificationLevel {
    #[default]
    None,
    SelfVerified,
    Reviewed,
    IndependentlyVerified,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum Executor {
    #[default]
    Manual,
    Agent,
    Command,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum JoinPolicy {
    #[default]
    AllSucceeded,
    AllSettled,
    AnySucceeded,
    MinimumSucceeded(u32),
    Quorum {
        required: u32,
        total: u32,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum EdgeCondition {
    #[default]
    Completed,
    Succeeded,
    Failed,
    Blocked,
    Outcome,
    Verification,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ResultSelection {
    /// A stable field path in the predecessor result. `None` means the whole result.
    #[serde(default)]
    pub field: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct InputBinding {
    pub alias: String,
    pub selection: ResultSelection,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkEdge {
    pub id: EdgeId,
    pub from: NodeId,
    pub to: NodeId,
    #[serde(default)]
    pub condition: EdgeCondition,
    #[serde(default = "default_true")]
    pub required: bool,
    #[serde(default)]
    pub binding: Option<InputBinding>,
}
fn default_true() -> bool {
    true
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct RetryPolicy {
    #[serde(default)]
    pub max_attempts: u32,
    #[serde(default)]
    pub retryable_outcomes: BTreeSet<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct InputContract {
    #[serde(default)]
    pub required_fields: BTreeSet<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct OutputContract {
    #[serde(default)]
    pub required_fields: BTreeSet<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct FileScope {
    #[serde(default)]
    pub planned: Vec<String>,
    #[serde(default)]
    pub advisory: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkNode {
    pub id: NodeId,
    pub key: String,
    pub title: String,
    #[serde(default)]
    pub description: Option<String>,
    #[serde(default)]
    pub status: ExecutionStatus,
    #[serde(default)]
    pub effective_outcome: Option<Outcome>,
    #[serde(default)]
    pub verification: VerificationLevel,
    #[serde(default)]
    pub executor: Executor,
    #[serde(default)]
    pub join: Option<JoinPolicy>,
    #[serde(default)]
    pub input_contract: InputContract,
    #[serde(default)]
    pub output_contract: OutputContract,
    #[serde(default)]
    pub retry_policy: RetryPolicy,
    #[serde(default)]
    pub acceptance_criteria: Vec<String>,
    #[serde(default)]
    pub file_scope: FileScope,
    #[serde(default)]
    pub attempt_ids: Vec<AttemptId>,
    #[serde(default)]
    pub revision: u64,
}

impl WorkNode {
    pub fn new(key: impl Into<String>, title: impl Into<String>) -> Self {
        Self {
            id: NodeId::new(),
            key: key.into(),
            title: title.into(),
            description: None,
            status: ExecutionStatus::Pending,
            effective_outcome: None,
            verification: VerificationLevel::None,
            executor: Executor::Manual,
            join: None,
            input_contract: InputContract::default(),
            output_contract: OutputContract::default(),
            retry_policy: RetryPolicy::default(),
            acceptance_criteria: Vec::new(),
            file_scope: FileScope::default(),
            attempt_ids: Vec::new(),
            revision: 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkAssignment {
    pub id: AssignmentId,
    pub node_id: NodeId,
    pub attempt_id: AttemptId,
    pub agent_id: String,
    /// Agent which requested this assignment.  This is persisted rather than
    /// inferred from the live hierarchy so completion notifications survive a
    /// restart.
    #[serde(default)]
    pub parent_agent_id: Option<String>,
    pub assigned_at: DateTime<Utc>,
    #[serde(default)]
    pub released_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NodeAttempt {
    pub id: AttemptId,
    pub node_id: NodeId,
    pub number: u32,
    pub state: ExecutionStatus,
    pub started_at: Option<DateTime<Utc>>,
    pub finished_at: Option<DateTime<Utc>>,
    #[serde(default)]
    pub agent_id: Option<String>,
    #[serde(default)]
    pub assignment_id: Option<AssignmentId>,
    #[serde(default)]
    pub result_id: Option<ResultId>,
    #[serde(default)]
    pub input_manifest_id: Option<ManifestId>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NodeResult {
    pub id: ResultId,
    pub node_id: NodeId,
    pub attempt_id: AttemptId,
    pub execution_status: ExecutionStatus,
    pub outcome: Option<Outcome>,
    pub verification: VerificationLevel,
    pub summary: String,
    #[serde(default)]
    pub structured_output: Option<serde_json::Value>,
    #[serde(default)]
    pub artifacts: Vec<String>,
    #[serde(default)]
    pub evidence: Vec<String>,
    #[serde(default)]
    pub changed_files: Vec<String>,
    #[serde(default)]
    pub producer: Option<String>,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct InputManifest {
    pub id: ManifestId,
    pub node_id: NodeId,
    pub graph_revision: u64,
    /// Exact result IDs, rather than live predecessor nodes: this is frozen at claim time.
    #[serde(default)]
    pub results: BTreeMap<String, ResultId>,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkGraph {
    pub id: GraphId,
    pub title: String,
    #[serde(default)]
    pub objective: Option<String>,
    #[serde(default)]
    pub mode: GraphMode,
    #[serde(default)]
    pub owner_agent_id: Option<String>,
    #[serde(default)]
    pub parent_assignment_id: Option<AssignmentId>,
    #[serde(default)]
    pub revision: u64,
    #[serde(default)]
    pub status: GraphStatus,
    /// Authored display order. It is intentionally independent from edges.
    #[serde(default)]
    pub view_order: Vec<NodeId>,
    #[serde(default)]
    pub nodes: BTreeMap<NodeId, WorkNode>,
    #[serde(default)]
    pub edges: BTreeMap<EdgeId, WorkEdge>,
    #[serde(default)]
    pub attempts: BTreeMap<AttemptId, NodeAttempt>,
    #[serde(default)]
    pub assignments: BTreeMap<AssignmentId, WorkAssignment>,
    #[serde(default)]
    pub results: BTreeMap<ResultId, NodeResult>,
    #[serde(default)]
    pub manifests: BTreeMap<ManifestId, InputManifest>,
    #[serde(default)]
    pub notifications: Vec<WorkNotification>,
    /// Advisory file claims — see [`FileClaim`]. Not filesystem-enforced.
    #[serde(default)]
    pub claims: BTreeMap<String, FileClaim>,
}

impl WorkGraph {
    pub fn new(title: impl Into<String>, owner_agent_id: Option<String>, mode: GraphMode) -> Self {
        Self {
            id: GraphId::new(),
            title: title.into(),
            objective: None,
            mode,
            owner_agent_id,
            parent_assignment_id: None,
            revision: 0,
            status: GraphStatus::Active,
            view_order: Vec::new(),
            nodes: BTreeMap::new(),
            edges: BTreeMap::new(),
            attempts: BTreeMap::new(),
            assignments: BTreeMap::new(),
            results: BTreeMap::new(),
            manifests: BTreeMap::new(),
            notifications: Vec::new(),
            claims: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct WorkState {
    #[serde(default)]
    pub revision: u64,
    #[serde(default)]
    pub graphs: BTreeMap<GraphId, WorkGraph>,
    #[serde(default)]
    pub active_graph_by_agent: BTreeMap<String, GraphId>,
    /// The currently claimed task for each live agent.  It is deliberately a
    /// projection in the durable state, not process-local bookkeeping.
    #[serde(default)]
    pub active_binding_by_agent: BTreeMap<String, AgentWorkBinding>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct AuthorizationContext {
    pub agent_id: String,
    #[serde(default)]
    pub can_manage: bool,
    #[serde(default)]
    pub assignment_ids: BTreeSet<AssignmentId>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NodeInput {
    pub key: String,
    pub title: String,
    #[serde(default)]
    pub description: Option<String>,
}

// ---------------------------------------------------------------------------
// M3.2 — advisory planned-file claims
// ---------------------------------------------------------------------------

/// An advisory record of files an agent intends to touch while holding an
/// assignment. Claims do not lock the filesystem or block other agents —
/// they exist so overlapping work can be surfaced (via a work event) to
/// peers and reviewers, not enforced.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FileClaim {
    pub id: String,
    pub agent_id: String,
    pub assignment_id: AssignmentId,
    pub attempt_id: AttemptId,
    pub paths: Vec<String>,
    pub acquired_at: DateTime<Utc>,
    #[serde(default)]
    pub expiry: Option<DateTime<Utc>>,
    #[serde(default)]
    pub released_at: Option<DateTime<Utc>>,
}

impl FileClaim {
    pub fn is_active(&self, now: DateTime<Utc>) -> bool {
        self.released_at.is_none() && self.expiry.is_none_or(|expiry| expiry > now)
    }
}

// ---------------------------------------------------------------------------
// M3.3 — built-in edit change events
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum FileChangeKind {
    Added,
    Updated,
    Deleted,
    Moved,
}

/// One exact filesystem or artifact change performed by the built-in `edit`
/// tool. `from_path` is set only for `Moved`, and carries the source path.
/// Never derived by parsing `bash` command strings — bash output is
/// explicitly best-effort/unobserved.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FileChange {
    pub kind: FileChangeKind,
    pub path: String,
    #[serde(default)]
    pub from_path: Option<String>,
}
