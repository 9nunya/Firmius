//! The model-facing flat-schema checklist/work tool.
use schemars::JsonSchema;
use serde::{Deserialize, Deserializer};
use serde_json::json;

use crate::work::{
    AuthorizationContext, EdgeCondition, EdgeId, GraphId, GraphMode, GraphStatus, InputBinding,
    JoinPolicy, NodeId, Outcome, ResultSelection, WorkEvent, WorkGraph,
};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

pub const WORK_READ_SCOPE: &str = "work_read";
pub const WORK_WRITE_SCOPE: &str = "work_write";

#[derive(Debug, Clone, Copy, Deserialize, JsonSchema, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
enum Mode {
    Create,
    Init,
    List,
    View,
    Add,
    Plan,
    Launch,
    Park,
    Resume,
    Poll,
    #[serde(rename = "wait", alias = "await")]
    Await,
    Update,
    Move,
    Start,
    Complete,
    Fail,
    Block,
    Unblock,
    Cancel,
    Retry,
    SetActive,
    Close,
    Connect,
    Disconnect,
    Configure,
    Annotate,
    QualityDigest,
    Squad,
    SwarmPolicy,
    Claim,
    ReleaseClaim,
    Milestone,
    CoordinationRequest,
    CoordinationRespond,
    OwnershipTransfer,
    OwnershipDecision,
    PlanAnalysis,
    Integration,
}

#[derive(Debug, Clone, Default, Deserialize, JsonSchema)]
struct IntegrationArg {
    #[serde(default)]
    owner_node_key: Option<String>,
    #[serde(default)]
    authorized_paths: Vec<String>,
    #[serde(default)]
    authorized_contracts: Vec<String>,
    #[serde(default)]
    may_modify_released_code: bool,
    #[serde(default)]
    may_request_transfer: bool,
    #[serde(default)]
    escalation_agent_id: Option<String>,
    #[serde(default)]
    rationale: Option<String>,
}

impl From<&IntegrationArg> for crate::work::IntegrationStrategy {
    fn from(value: &IntegrationArg) -> Self {
        Self {
            owner_node_key: value.owner_node_key.clone(),
            authorized_paths: value.authorized_paths.clone(),
            authorized_contracts: value.authorized_contracts.clone(),
            may_modify_released_code: value.may_modify_released_code,
            may_request_transfer: value.may_request_transfer,
            escalation_agent_id: value.escalation_agent_id.clone(),
            rationale: value.rationale.clone(),
        }
    }
}

fn outcome_label(outcome: Option<&Outcome>) -> Option<&str> {
    outcome.map(|value| match value {
        Outcome::Success => "success",
        Outcome::Failure => "failure",
        Outcome::TestFailed => "test_failed",
        Outcome::Blocked => "blocked",
        Outcome::Cancelled => "cancelled",
        Outcome::Interrupted => "interrupted",
        Outcome::Custom(value) => value.as_str(),
    })
}

fn invalid_field(code: &str, field: &str, allowed: &[&str], hint: impl Into<String>) -> ToolError {
    ToolError::InvalidArguments(
        json!({
            "code": code,
            "field": field,
            "retryable": true,
            "allowed_values": allowed,
            "hint": hint.into(),
        })
        .to_string(),
    )
}

fn selected_mode(args: &TaskArgs) -> Result<Mode, ToolError> {
    match (args.action, args.mode) {
        (Some(action), Some(mode)) if action != mode => Err(invalid_field(
            "conflicting_operation",
            "action",
            &[],
            "provide only action, or make legacy mode match it",
        )),
        (Some(action), _) => Ok(action),
        (_, Some(mode)) => Ok(mode),
        _ => Ok(Mode::View),
    }
}

fn required_verification(
    args: &TaskArgs,
) -> Result<Option<crate::work::VerificationLevel>, ToolError> {
    parse_verification(
        &args
            .required_verification
            .clone()
            .or_else(|| args.verification.clone()),
    )
}

fn performed_verification(
    args: &TaskArgs,
) -> Result<Option<crate::work::VerificationLevel>, ToolError> {
    parse_verification(
        &args
            .performed_verification
            .clone()
            .or_else(|| args.verification.clone()),
    )
}

/// Presence-aware nullable update field. Missing means "leave unchanged";
/// JSON null means "clear"; a string means "replace".
#[derive(Debug, Clone, Default)]
enum DescriptionPatch {
    #[default]
    Missing,
    Set(Option<String>),
}

impl<'de> Deserialize<'de> for DescriptionPatch {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        Option::<String>::deserialize(deserializer).map(Self::Set)
    }
}

impl JsonSchema for DescriptionPatch {
    fn schema_name() -> std::borrow::Cow<'static, str> {
        "NullableDescription".into()
    }

    fn json_schema(generator: &mut schemars::SchemaGenerator) -> schemars::Schema {
        <Option<String>>::json_schema(generator)
    }
}

impl DescriptionPatch {
    fn value(&self) -> Option<String> {
        match self {
            Self::Missing => None,
            Self::Set(value) => value.clone(),
        }
    }

    fn into_update(self) -> Option<Option<String>> {
        match self {
            Self::Missing => None,
            Self::Set(value) => Some(value),
        }
    }
}

/// Checklist entries accept the original title-only string form as well as a
/// keyed object (`{"key":"build","title":"Build it"}`).
#[derive(Debug, Clone, Deserialize, JsonSchema)]
#[serde(untagged)]
enum ChecklistItem {
    Title(String),
    Keyed { key: String, title: String },
}

impl ChecklistItem {
    fn into_key_title(self, fallback: String) -> (String, String) {
        match self {
            Self::Title(title) => (fallback, title),
            Self::Keyed { key, title } if !key.is_empty() => (key, title),
            Self::Keyed { title, .. } => (fallback, title),
        }
    }
}

fn parse_verification(
    value: &Option<String>,
) -> Result<Option<crate::work::VerificationLevel>, ToolError> {
    use crate::work::VerificationLevel;
    Ok(match value.as_deref() {
        None => None,
        Some("none") => Some(VerificationLevel::None),
        Some("self_verified") | Some("self") => Some(VerificationLevel::SelfVerified),
        Some("reviewed") => Some(VerificationLevel::Reviewed),
        Some("independently_verified") | Some("independent") => {
            Some(VerificationLevel::IndependentlyVerified)
        }
        Some(other) => {
            return Err(ToolError::InvalidArguments(format!(
                "unknown verification level '{other}'"
            )));
        }
    })
}

fn parse_annotation_kind(value: &Option<String>) -> Result<crate::work::AnnotationKind, ToolError> {
    use crate::work::AnnotationKind;
    Ok(match value.as_deref() {
        None | Some("comment") => AnnotationKind::Comment,
        Some("approval") | Some("approve") => AnnotationKind::Approval,
        Some("rejection") | Some("reject") => AnnotationKind::Rejection,
        Some(other) => {
            return Err(ToolError::InvalidArguments(format!(
                "unknown annotation kind '{other}'"
            )));
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registration_exposes_one_flat_task_schema_and_read_scope() {
        let registry = ToolRegistry::default();
        register_task_tool(&registry);
        let definition = registry
            .definitions()
            .into_iter()
            .find(|definition| definition.name == "task")
            .expect("task is registered");
        assert_eq!(definition.input_schema["type"], "object");
        assert!(definition.input_schema.get("oneOf").is_none());
    }
}

impl Default for Mode {
    fn default() -> Self {
        Self::View
    }
}

/// One node in a `plan` call. Flat and explicit: models fill flat objects
/// reliably, and a node that silently defaulted to spawning an agent would
/// be a surprising and expensive default.
#[derive(Debug, Clone, Deserialize, JsonSchema)]
pub(super) struct PlanNodeArg {
    /// Stable identifier used by edges and later `start`/`complete` calls.
    key: String,
    title: String,
    #[serde(default)]
    description: Option<String>,
    /// `manual` (default, you or a worker claims it), or `agent` (the run
    /// spawns a subagent). Managed command execution is not model-facing
    /// until its process specification is part of the durable plan schema.
    #[serde(default)]
    executor: Option<String>,
    /// Required when `executor` is `agent`: which persona to spawn.
    #[serde(default)]
    persona: Option<String>,
    /// Required when `executor` is `agent`: this node's task sheet. The
    /// graph `brief` and bound predecessor results are supplied separately;
    /// do not paste them here.
    #[serde(default)]
    prompt: Option<String>,
    #[serde(default)]
    model: Option<String>,
    #[serde(default)]
    effort: Option<String>,
    /// When this node may run: `all_succeeded` (default), `all_settled`,
    /// `any_succeeded`, `minimum_succeeded`, `quorum`.
    #[serde(default)]
    join_policy: Option<String>,
    #[serde(default)]
    join_required: Option<u32>,
    #[serde(default)]
    join_total: Option<u32>,
    /// Caps total attempts, including re-attempts driven by a feedback edge.
    #[serde(default)]
    max_attempts: Option<u32>,
    /// Verification level this node's result is required to achieve.
    #[serde(default)]
    required_verification: Option<String>,
    /// Legacy name for `required_verification`; accepted but hidden.
    #[serde(default)]
    #[schemars(skip)]
    verification: Option<String>,
    #[serde(default)]
    acceptance_criteria: Vec<String>,
    #[serde(default)]
    requires_independent_reviewer: Option<bool>,
    /// Structured coordination contract. These paths and relationships do
    /// not expand the worker's actual tool or edit authority.
    #[serde(default)]
    assignment_contract: Option<AssignmentContractArg>,
    /// Explicit advisory file scope. When omitted, intended mutation paths
    /// from the assignment contract are projected here automatically.
    #[serde(default)]
    file_scope: Vec<String>,
}

#[derive(Debug, Clone, Default, Deserialize, JsonSchema)]
struct AssignmentContractArg {
    #[serde(default)]
    objective: Option<String>,
    #[serde(default)]
    intended_mutation_paths: Vec<String>,
    #[serde(default)]
    intended_inspection_paths: Vec<String>,
    #[serde(default)]
    published_contracts: Vec<String>,
    #[serde(default)]
    consumed_contracts: Vec<String>,
    /// Named milestones this assignment will publish. Publishing is a
    /// durable informational notification; it never blocks a peer.
    #[serde(default)]
    published_milestones: Vec<String>,
    /// Informational only. These do NOT gate scheduling: a consumer may
    /// start before the milestone exists. Use a required dependency edge for
    /// real execution ordering. Protective plans that declare a required
    /// milestone are rejected rather than silently ignoring it.
    #[serde(default)]
    required_milestones: Vec<String>,
    #[serde(default)]
    validation_scope: Option<String>,
    #[serde(default)]
    deferred_integration_validation: bool,
    #[serde(default)]
    integration_owner: Option<String>,
    #[serde(default)]
    integration_strategy: Option<String>,
    #[serde(default)]
    escalation: Option<String>,
    #[serde(default)]
    overlap_override: bool,
    #[serde(default)]
    overlap_override_rationale: Option<String>,
}

impl From<&AssignmentContractArg> for crate::work::AssignmentContract {
    fn from(value: &AssignmentContractArg) -> Self {
        Self {
            objective: value.objective.clone(),
            intended_mutation_paths: value.intended_mutation_paths.clone(),
            intended_inspection_paths: value.intended_inspection_paths.clone(),
            published_contracts: value.published_contracts.clone(),
            consumed_contracts: value.consumed_contracts.clone(),
            published_milestones: value.published_milestones.clone(),
            required_milestones: value.required_milestones.clone(),
            validation_scope: value.validation_scope.clone(),
            deferred_integration_validation: value.deferred_integration_validation,
            integration_owner: value.integration_owner.clone(),
            integration_strategy: value.integration_strategy.clone(),
            escalation: value.escalation.clone(),
            overlap_override: value.overlap_override,
            overlap_override_rationale: value.overlap_override_rationale.clone(),
        }
    }
}

/// One edge in a `plan` call. Readiness and data flow are independent: an
/// optional edge that does not gate its successor can still deliver its
/// result, and a gating edge can carry no data at all.
#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct PlanEdgeArg {
    /// Predecessor node key.
    from: String,
    /// Successor node key.
    to: String,
    /// `dependency` (default) means `to` waits for `from`. `feedback` means
    /// that when `from` settles matching this edge, `to` is sent back for
    /// another attempt — a gate bouncing work to any node, bounded by that
    /// node's `max_attempts`.
    #[serde(default)]
    kind: Option<String>,
    /// What must be true of `from`: `completed` (default), `succeeded`,
    /// `failed`, `blocked`, `outcome`, `verification`.
    #[serde(default)]
    condition: Option<String>,
    /// With `condition: "outcome"`, the exact outcome that fires this edge,
    /// e.g. `rejected` or `test_failed`. Any string is allowed, so a gate
    /// can define its own vocabulary and route each verdict differently.
    #[serde(default)]
    on_outcome: Option<String>,
    /// Whether this edge gates the successor's readiness. Defaults to true.
    #[serde(default)]
    required: Option<bool>,
    /// Name `to` sees this result under. Set it whenever the successor needs
    /// the predecessor's output, e.g. `finding_1` or `draft`.
    #[serde(default)]
    binding_alias: Option<String>,
    /// Narrow the bound value to one field of the predecessor's structured
    /// output instead of the whole result.
    #[serde(default)]
    binding_field: Option<String>,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct TaskArgs {
    /// Operation to perform. Default view.
    #[serde(default)]
    action: Option<Mode>,
    /// Legacy operation selector. Prefer `action` in new calls.
    #[serde(default)]
    #[schemars(skip)]
    mode: Option<Mode>,
    /// Graph to address. Omit to use your active graph after init or workflow run.
    #[serde(default)]
    graph_id: Option<String>,
    /// Node UUID. Prefer key for ordinary node operations; provide one selector.
    #[serde(default)]
    node_id: Option<String>,
    /// Stable node key for start, complete, fail, block, retry, or update.
    #[serde(default)]
    key: Option<String>,
    /// Node keys (or node_ids) to settle in ONE call. `complete` accepts
    /// several at once so finishing N checklist items is one transaction
    /// and one revision, not N round trips.
    #[serde(default)]
    keys: Vec<String>,
    /// User-facing graph or node title, depending on action.
    #[serde(default)]
    title: Option<String>,
    /// For `update`, omission preserves the description, null clears it, and
    /// a string replaces it. For create/add, null and omission mean no description.
    #[serde(default)]
    description: DescriptionPatch,
    #[serde(default)]
    before_node_id: Option<String>,
    /// Optional compare-and-swap guard. When omitted, a routine mutation uses
    /// the active graph's revision at commit time; when supplied, stale values
    /// fail without mutation.
    #[serde(default)]
    expected_revision: Option<u64>,
    /// Legacy result verdict; prefer verdict.
    #[serde(default)]
    #[schemars(skip)]
    outcome: Option<String>,
    /// Observed result summary. Required for successful completion; state what was verified.
    #[serde(default)]
    summary: Option<String>,
    /// Concrete observations or artifact references supporting this result.
    #[serde(default)]
    evidence: Vec<String>,
    /// Observable end state for graph creation.
    #[serde(default)]
    objective: Option<String>,
    /// Initial checklist titles for `create`/`init`. One item per distinct
    /// piece of work, including work you will do yourself.
    #[serde(default)]
    items: Vec<ChecklistItem>,
    /// Optional explicit keys for checklist items. When supplied, entries are
    /// paired by position with `items`; this keeps the compact flat schema
    /// backwards compatible while allowing callers to name every node.
    #[serde(default)]
    #[schemars(skip)]
    item_keys: Vec<String>,
    // M4.7 — connect/disconnect/configure.
    #[serde(default)]
    from_node_id: Option<String>,
    #[serde(default)]
    to_node_id: Option<String>,
    #[serde(default)]
    edge_id: Option<String>,
    /// Edge condition for connect: completed, succeeded, failed, blocked, outcome, or verification.
    #[serde(default)]
    condition: Option<String>,
    /// Whether the dependency gates the successor. Defaults to true.
    #[serde(default)]
    required: Option<bool>,
    /// Name under which the successor receives the predecessor result.
    #[serde(default)]
    binding_alias: Option<String>,
    /// Readiness policy: all_succeeded, all_settled, any_succeeded, minimum_succeeded, or quorum.
    #[serde(default)]
    join_policy: Option<String>,
    /// Required successful predecessor count for minimum_succeeded or quorum.
    #[serde(default)]
    join_required: Option<u32>,
    /// Total predecessor count for quorum.
    #[serde(default)]
    join_total: Option<u32>,
    /// Node attempt cap including retries; set a positive bound for feedback loops.
    #[serde(default)]
    max_attempts: Option<u32>,
    /// Node executor: manual or agent.
    #[serde(default)]
    executor: Option<String>,
    // M5 — required policy and performed result verification are deliberately
    // separate concepts. `verification` remains a decode-only legacy bridge.
    /// Verification level a configured node is required to achieve.
    #[serde(default)]
    required_verification: Option<String>,
    /// Verification level actually performed for `complete`/`fail`.
    #[serde(default)]
    performed_verification: Option<String>,
    /// Checks actually performed. These supplement durable `evidence` rather
    /// than changing the node's required verification policy.
    #[serde(default)]
    performed_checks: Vec<String>,
    /// Result verdict/outcome. Preferred over the legacy `outcome` field.
    #[serde(default)]
    verdict: Option<String>,
    /// Legacy overloaded verification name; accepted but hidden from new schema.
    #[serde(default)]
    #[schemars(skip)]
    verification: Option<String>,
    /// Concrete criteria against which completion evidence should be evaluated.
    #[serde(default)]
    acceptance_criteria: Vec<String>,
    /// Require reviewer identity independent of the producer.
    #[serde(default)]
    requires_independent_reviewer: Option<bool>,
    /// Producer result UUID to annotate; obtain from task view.
    #[serde(default)]
    result_id: Option<String>,
    /// Review annotation: approval, rejection, or comment. Defaults to comment.
    #[serde(default)]
    annotation_kind: Option<String>,
    /// Evidence-backed body for annotate. Explain what was checked or what must change.
    #[serde(default)]
    text: Option<String>,
    // `plan` — author a whole DAG in one call.
    /// Nodes to create. Keys must be unique within the graph.
    #[serde(default)]
    nodes: Vec<PlanNodeArg>,
    /// Edges between nodes, referenced by key. May reference nodes created
    /// by this same call or ones already in the graph.
    #[serde(default)]
    edges: Vec<PlanEdgeArg>,
    /// Shared context prepended to EVERY agent prompt in this graph. Put
    /// the codebase orientation, conventions, and quality bar here once
    /// instead of pasting them into each node's prompt.
    #[serde(default)]
    brief: Option<String>,
    /// Set true to let the run execute `agent`/`command` nodes on its own
    /// as their dependencies settle.
    #[serde(default)]
    managed: Option<bool>,
    /// Run id from `launch`, for `poll` / `wait`.
    #[serde(default)]
    run_id: Option<String>,
    /// Max nodes a run executes at once. Defaults to 8.
    #[serde(default)]
    max_concurrent: Option<u32>,
    /// Hard ceiling on attempts launched by one run. Defaults to 200.
    #[serde(default)]
    max_attempts_total: Option<u32>,
    /// Swarm policy for `swarm_policy`: disabled, advisory, or protective.
    #[serde(default)]
    policy: Option<String>,
    /// Workspace-relative paths for one atomic resource claim.
    #[serde(default)]
    resource_paths: Vec<String>,
    /// Resource access for `claim`: inspect or mutate (default mutate).
    #[serde(default)]
    resource_access: Option<String>,
    /// Durable claim id for release or ownership transfer.
    #[serde(default)]
    claim_id: Option<String>,
    /// Milestone name.
    #[serde(default)]
    milestone: Option<String>,
    /// Structured milestone/request/response payload.
    #[serde(default)]
    payload: Option<serde_json::Value>,
    /// Stable caller-provided key for idempotent coordination operations.
    #[serde(default)]
    idempotency_key: Option<String>,
    /// Application-defined coordination request kind.
    #[serde(default)]
    request_kind: Option<String>,
    /// Agent ids participating as responders in a coordination request.
    #[serde(default)]
    peer_agent_ids: Vec<String>,
    /// Coordination request id.
    #[serde(default)]
    request_id: Option<String>,
    /// accepted, rejected, declined, or cancelled.
    #[serde(default)]
    peer_state: Option<String>,
    /// Target assignment id for an ownership transfer.
    #[serde(default)]
    target_assignment_id: Option<String>,
    /// Ownership transfer id.
    #[serde(default)]
    transfer_id: Option<String>,
    /// Approval decision for an ownership transfer.
    #[serde(default)]
    approved: Option<bool>,
    /// Human-readable rationale or decision note.
    #[serde(default)]
    rationale: Option<String>,
    /// Graph-scoped integration ownership for plan or integration actions.
    #[serde(default)]
    integration: Option<IntegrationArg>,
}

/// Resolve a graph id, falling back to the caller's active graph so
/// `start`/`add`/`complete` work after `init` without repeating `graph_id`.
fn resolve_graph_id(
    session: &crate::session::SessionHandle,
    agent_id: &str,
    value: &Option<String>,
) -> Result<GraphId, ToolError> {
    if let Some(raw) = value.as_deref().filter(|s| !s.trim().is_empty()) {
        return GraphId::parse(raw)
            .map_err(|e| ToolError::InvalidArguments(format!("invalid graph_id: {e}")));
    }
    session
        .work
        .read()
        .unwrap()
        .active_graph_by_agent
        .get(agent_id)
        .copied()
        .ok_or_else(|| ToolError::InvalidArguments("no graph selected".into()))
}

fn parse_condition(value: &Option<String>) -> Result<EdgeCondition, ToolError> {
    Ok(match value.as_deref() {
        None | Some("completed") => EdgeCondition::Completed,
        Some("succeeded") => EdgeCondition::Succeeded,
        Some("failed") => EdgeCondition::Failed,
        Some("blocked") => EdgeCondition::Blocked,
        Some("outcome") => EdgeCondition::Outcome,
        Some("verification") => EdgeCondition::Verification,
        Some(other) => {
            return Err(ToolError::InvalidArguments(format!(
                "unknown edge condition '{other}'"
            )));
        }
    })
}

fn parse_join_policy(args: &TaskArgs) -> Result<Option<JoinPolicy>, ToolError> {
    let Some(name) = args.join_policy.as_deref() else {
        return Ok(None);
    };
    Ok(Some(match name {
        "all_succeeded" => JoinPolicy::AllSucceeded,
        "all_settled" => JoinPolicy::AllSettled,
        "any_succeeded" => JoinPolicy::AnySucceeded,
        "minimum_succeeded" => {
            JoinPolicy::MinimumSucceeded(args.join_required.ok_or_else(|| {
                ToolError::InvalidArguments("minimum_succeeded requires 'join_required'".into())
            })?)
        }
        "quorum" => JoinPolicy::Quorum {
            required: args.join_required.ok_or_else(|| {
                ToolError::InvalidArguments("quorum requires 'join_required'".into())
            })?,
            total: args.join_total.ok_or_else(|| {
                ToolError::InvalidArguments("quorum requires 'join_total'".into())
            })?,
        },
        other => {
            return Err(ToolError::InvalidArguments(format!(
                "unknown join policy '{other}'"
            )));
        }
    }))
}

fn parse_executor(value: &Option<String>) -> Result<Option<crate::work::Executor>, ToolError> {
    Ok(match value.as_deref() {
        None => None,
        Some("manual") => Some(crate::work::Executor::Manual),
        Some("agent") => Some(crate::work::Executor::Agent),
        Some("command") => {
            return Err(ToolError::InvalidArguments(
                "executor 'command' is not available in task plans; use an agent node or run the command through bash"
                    .into(),
            ));
        }
        Some(other) => {
            return Err(ToolError::InvalidArguments(format!(
                "unknown executor '{other}'"
            )));
        }
    })
}

/// Translate one planned node from the flat model-facing shape into the
/// domain record, resolving the executor/agent-spec pairing here so an
/// invalid combination is reported with the node's key attached.
fn plan_node(arg: &PlanNodeArg) -> Result<crate::work::PlannedNode, ToolError> {
    let executor = parse_executor(&arg.executor)?.unwrap_or_default();
    let agent = match (&arg.persona, &arg.prompt) {
        (Some(persona), Some(prompt)) => Some(crate::work::AgentSpec {
            persona: persona.clone(),
            prompt: prompt.clone(),
            model: arg.model.clone(),
            effort: arg.effort.clone(),
        }),
        (None, None) => None,
        _ => {
            return Err(ToolError::InvalidArguments(format!(
                "node '{}' needs both 'persona' and 'prompt' to run an agent",
                arg.key
            )));
        }
    };
    if executor == crate::work::Executor::Agent && agent.is_none() {
        return Err(ToolError::InvalidArguments(format!(
            "node '{}' has executor 'agent' but no 'persona'/'prompt'",
            arg.key
        )));
    }
    let join = match arg.join_policy.as_deref() {
        None => None,
        Some("all_succeeded") => Some(JoinPolicy::AllSucceeded),
        Some("all_settled") => Some(JoinPolicy::AllSettled),
        Some("any_succeeded") => Some(JoinPolicy::AnySucceeded),
        Some("minimum_succeeded") => Some(JoinPolicy::MinimumSucceeded(
            arg.join_required.ok_or_else(|| {
                ToolError::InvalidArguments(format!(
                    "node '{}': minimum_succeeded requires 'join_required'",
                    arg.key
                ))
            })?,
        )),
        Some("quorum") => Some(JoinPolicy::Quorum {
            required: arg.join_required.ok_or_else(|| {
                ToolError::InvalidArguments(format!(
                    "node '{}': quorum requires 'join_required'",
                    arg.key
                ))
            })?,
            total: arg.join_total.ok_or_else(|| {
                ToolError::InvalidArguments(format!(
                    "node '{}': quorum requires 'join_total'",
                    arg.key
                ))
            })?,
        }),
        Some(other) => {
            return Err(ToolError::InvalidArguments(format!(
                "node '{}': unknown join policy '{other}'",
                arg.key
            )));
        }
    };
    Ok(crate::work::PlannedNode {
        key: arg.key.clone(),
        title: arg.title.clone(),
        description: arg.description.clone(),
        executor,
        agent,
        join,
        verification: parse_verification(
            &arg.required_verification
                .clone()
                .or_else(|| arg.verification.clone()),
        )?
        .unwrap_or_default(),
        acceptance_criteria: arg
            .acceptance_criteria
            .iter()
            .cloned()
            .map(crate::work::AcceptanceCriterion::new)
            .collect(),
        review_policy: crate::work::ReviewPolicy {
            requires_independent_reviewer: arg.requires_independent_reviewer.unwrap_or(false),
        },
        max_attempts: arg.max_attempts,
        assignment_contract: arg
            .assignment_contract
            .as_ref()
            .map(Into::into)
            .unwrap_or_default(),
        file_scope: crate::work::FileScope {
            planned: arg.file_scope.clone(),
            advisory: !arg.file_scope.is_empty(),
        },
    })
}

fn plan_edge(arg: &PlanEdgeArg) -> Result<crate::work::PlannedEdge, ToolError> {
    let kind = match arg.kind.as_deref() {
        None | Some("dependency") => crate::work::EdgeKind::Dependency,
        Some("feedback") => crate::work::EdgeKind::Feedback,
        Some(other) => {
            return Err(ToolError::InvalidArguments(format!(
                "unknown edge kind '{other}': expected 'dependency' or 'feedback'"
            )));
        }
    };
    let on_outcome = arg.on_outcome.as_deref().map(|value| match value {
        "success" => crate::work::Outcome::Success,
        "failure" => crate::work::Outcome::Failure,
        "test_failed" => crate::work::Outcome::TestFailed,
        "blocked" => crate::work::Outcome::Blocked,
        "cancelled" => crate::work::Outcome::Cancelled,
        "interrupted" => crate::work::Outcome::Interrupted,
        other => crate::work::Outcome::Custom(other.to_string()),
    });
    Ok(crate::work::PlannedEdge {
        from: arg.from.clone(),
        to: arg.to.clone(),
        kind,
        condition: parse_condition(&arg.condition)?,
        on_outcome,
        required: arg.required.unwrap_or(true),
        binding_alias: arg.binding_alias.clone(),
        binding_field: arg.binding_field.clone(),
    })
}

/// Build a per-node authorization context. `can_manage` is intentionally
/// never derived from the coarse `work_write` tool scope: ownership and
/// per-node authority are decided in `work::transition` from the graph's
/// `owner_agent_id` and live assignments. This only surfaces which of the
/// caller's own live assignments on this graph they may act through.
fn auth(ctx: &ToolContext, graph: &WorkGraph) -> AuthorizationContext {
    let assignment_ids = graph
        .assignments
        .values()
        .filter(|a| a.agent_id == ctx.agent_id && a.released_at.is_none())
        .map(|a| a.id)
        .collect();
    AuthorizationContext {
        agent_id: ctx.agent_id.clone(),
        can_manage: false,
        assignment_ids,
    }
}

fn require_write(ctx: &ToolContext) -> Result<(), ToolError> {
    if ctx
        .allowed_scopes
        .as_ref()
        .is_some_and(|s| !s.contains(WORK_WRITE_SCOPE))
    {
        return Err(ToolError::PermissionDenied {
            tool: "task".into(),
            required: vec![WORK_WRITE_SCOPE.into()],
            allowed: ctx
                .allowed_scopes
                .clone()
                .unwrap_or_default()
                .into_iter()
                .collect(),
        });
    }
    Ok(())
}

fn node_id(
    graph: &WorkGraph,
    args: &TaskArgs,
    assignment: Option<crate::work::NodeId>,
) -> Result<NodeId, ToolError> {
    if let Some(value) = &args.node_id {
        return NodeId::parse(value)
            .map_err(|e| ToolError::InvalidArguments(format!("invalid node_id: {e}")));
    }
    if let Some(key) = args.key.as_deref() {
        return graph
            .nodes
            .values()
            .find(|node| node.key == key)
            .map(|node| node.id)
            .ok_or_else(|| ToolError::InvalidArguments(format!("unknown node key '{key}'")));
    }
    if let Some(assigned) = assignment {
        return Ok(assigned);
    }
    Err(ToolError::InvalidArguments(
        "missing 'node_id' or 'key'".into(),
    ))
}

fn assigned_node(
    session: &crate::session::SessionHandle,
    agent_id: &str,
) -> Option<crate::work::NodeId> {
    session
        .work
        .read()
        .unwrap()
        .binding_for_agent(agent_id)
        .map(|binding| binding.node_id)
}

/// Resolve one node reference, accepting either a node key or a node_id
/// string. Callers pass whichever they have from `view`.
fn resolve_node_ref(graph: &WorkGraph, value: &str) -> Result<NodeId, ToolError> {
    let value = value.trim().trim_matches('"');
    if let Some(node) = graph.nodes.values().find(|node| node.key == value) {
        return Ok(node.id);
    }
    NodeId::parse(value)
        .ok()
        .filter(|id| graph.nodes.contains_key(id))
        .ok_or_else(|| ToolError::InvalidArguments(format!("unknown node '{value}'")))
}

/// Every node a `complete` call targets, in caller order.
///
/// `keys` (batch) takes precedence; otherwise this falls back to the same
/// single-node resolution the other modes use, so `complete` with `key`,
/// `node_id`, or a bound worker's implicit assignment all still work.
fn complete_targets(
    graph: &WorkGraph,
    args: &TaskArgs,
    assignment: Option<NodeId>,
) -> Result<Vec<NodeId>, ToolError> {
    if args.keys.is_empty() {
        return Ok(vec![node_id(graph, args, assignment)?]);
    }
    let mut targets = Vec::new();
    for value in &args.keys {
        let id = resolve_node_ref(graph, value)?;
        if targets.contains(&id) {
            return Err(ToolError::InvalidArguments(format!(
                "duplicate node in 'keys': {value}"
            )));
        }
        targets.push(id);
    }
    Ok(targets)
}

pub fn register_task_tool(registry: &ToolRegistry) -> &ToolRegistry {
    registry.register(
        TypedTool::new(
            "task",
            "Keep a durable checklist or edit advanced workflow graphs. For a new agent workflow, prefer the simpler `workflow` tool. Checklist example: {\"action\":\"init\",\"title\":\"Fix login\",\"items\":[{\"key\":\"fix\",\"title\":\"Fix and test login\"}]}; then start/complete by key with an observed summary. Advanced runs require init, plan with managed=true, then launch; wait with the returned run_id. Create, inspect, and mutate the durable session work graph. Select the operation with `action`. Omit `graph_id` to use the active graph. Routine mutations may omit `expected_revision` and atomically use the current revision; supply it for explicit compare-and-swap. `plan` authors many nodes/edges in one call, and `launch` starts a managed graph. `view` returns bounded node status including required verification policy, latest performed verification/result evidence/artifacts/diagnostics, active assignment, and review annotations. Use `annotate` for append-only review verdicts. Required scopes: work_read for every call and work_write for mutations.",
            |args: TaskArgs, ctx: ToolContext| Box::pin(async move { task(args, ctx).await }),
        )
        .with_required_scopes([WORK_READ_SCOPE]),
    );
    super::workflow::register_workflow_tool(registry);
    registry
}

pub(super) async fn call_task(
    value: serde_json::Value,
    ctx: ToolContext,
) -> Result<String, ToolError> {
    let args =
        serde_json::from_value(value).map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
    task(args, ctx).await
}

/// Validate the whole graph off to the side before publishing any durable state.
pub(super) fn build_workflow(
    value: serde_json::Value,
    ctx: &ToolContext,
) -> Result<WorkGraph, ToolError> {
    let args: TaskArgs =
        serde_json::from_value(value).map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
    let nodes = args
        .nodes
        .iter()
        .map(plan_node)
        .collect::<Result<Vec<_>, _>>()?;
    let edges = args
        .edges
        .iter()
        .map(plan_edge)
        .collect::<Result<Vec<_>, _>>()?;
    let mut graph = WorkGraph::new(
        args.title.unwrap_or_else(|| "Workflow".into()),
        Some(ctx.agent_id.clone()),
        GraphMode::Managed,
    );
    graph.objective = args.objective;
    let gid = graph.id;
    let context = auth(ctx, &graph);
    let mut staged = crate::work::WorkState::default();
    staged
        .create_graph(graph, None)
        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
    let revision = staged.graphs[&gid].revision;
    staged
        .plan(
            gid,
            revision,
            &context,
            nodes,
            edges,
            args.brief,
            Some(true),
        )
        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
    if let Some(integration) = args.integration.as_ref() {
        staged
            .configure_integration_strategy(staged.revision, gid, &ctx.agent_id, integration.into())
            .map_err(|error| ToolError::InvalidArguments(error.to_string()))?;
    }
    Ok(staged.graphs.remove(&gid).expect("staged graph"))
}

async fn task(args: TaskArgs, ctx: ToolContext) -> Result<String, ToolError> {
    let session = ctx
        .session
        .clone()
        .ok_or_else(|| ToolError::Failed("task requires a session".into()))?;
    let selected_mode = selected_mode(&args)?;
    match selected_mode {
        Mode::List => {
            let state = session.work.read().unwrap();
            let graphs = state.graphs.values().map(|g| json!({"graph_id": g.id.to_string(), "title": g.title, "revision": g.revision, "status": g.status})).collect::<Vec<_>>();
            Ok(json!({"revision": state.revision, "graphs": graphs}).to_string())
        }
        Mode::View => {
            let state = session.work.read().unwrap();
            let gid = args
                .graph_id
                .as_ref()
                .map(|v| GraphId::parse(v))
                .transpose()
                .map_err(|e| ToolError::InvalidArguments(e.to_string()))?
                .or_else(|| state.active_graph_by_agent.get(&ctx.agent_id).copied())
                .ok_or_else(|| ToolError::InvalidArguments("no graph selected".into()))?;
            let graph = state
                .graphs
                .get(&gid)
                .ok_or_else(|| ToolError::InvalidArguments("graph not found".into()))?;
            // Bounded summary: a full graph carries every attempt, result,
            // and manifest ever recorded, which is unbounded tool output for
            // a long-lived checklist. Nodes carry their live status only.
            let nodes = graph
                .view_order
                .iter()
                .filter_map(|id| graph.nodes.get(id))
                .map(|node| {
                    let attempt = node
                        .attempt_ids
                        .last()
                        .and_then(|attempt_id| graph.attempts.get(attempt_id));
                    let result = attempt
                        .and_then(|attempt| attempt.result_id)
                        .and_then(|result_id| graph.results.get(&result_id));
                    let assignment = attempt
                        .and_then(|attempt| attempt.assignment_id)
                        .and_then(|assignment_id| graph.assignments.get(&assignment_id));
                    let review = result.map(|result| {
                        graph.annotations.values()
                            .filter(|annotation| annotation.result_id == result.id)
                            .collect::<Vec<_>>()
                    }).unwrap_or_default();
                    let diagnostics = result
                        .and_then(|result| result.structured_output.as_ref())
                        .and_then(|output| output.get("diagnostics"))
                        .cloned()
                        .unwrap_or_else(|| json!([]));
                    json!({
                        "node_id": node.id.to_string(),
                        "key": node.key,
                        "title": node.title,
                        "status": node.status,
                        "required_verification": node.verification,
                        "review_policy": node.review_policy,
                        "latest_result_id": result.map(|result| result.id.to_string()),
                        "performed_verification": result.map(|result| result.verification),
                        "artifacts": result.map(|result| result.artifacts.clone()).unwrap_or_default(),
                        "evidence": result.map(|result| result.evidence.clone()).unwrap_or_default(),
                        "evidence_links": result.map(|result| result.evidence_links.clone()).unwrap_or_default(),
                        "diagnostics": diagnostics,
                        "assignment": assignment,
                        "review": review,
                    })
                })
                .collect::<Vec<_>>();
            let assignment = state.binding_for_agent(&ctx.agent_id).and_then(|binding| {
                if binding.graph_id != gid {
                    return None;
                }
                graph.nodes.get(&binding.node_id).map(|node| {
                    json!({
                        "node_id": node.id.to_string(),
                        "key": node.key,
                        "title": node.title,
                        "status": node.status,
                        "assignment_id": binding.assignment_id.to_string(),
                    })
                })
            });
            Ok(json!({
                "graph_id": graph.id.to_string(),
                "title": graph.title,
                "objective": graph.objective,
                "status": graph.status,
                "revision": graph.revision,
                "owner_agent_id": graph.owner_agent_id,
                "your_assignment": assignment,
                "nodes": nodes,
                "attempt_count": graph.attempts.len(),
                "result_count": graph.results.len(),
            })
            .to_string())
        }
        Mode::Init => {
            let owner = ctx.agent_id.clone();
            // Init is idempotent: if the agent already has a live active
            // graph, reuse it rather than creating and orphaning a second
            // graph via a repeated `init` call.
            if let Some(gid) = session
                .work
                .read()
                .unwrap()
                .active_graph_by_agent
                .get(&owner)
                .copied()
                && let Some(graph) = session.work.read().unwrap().graphs.get(&gid)
            {
                return Ok(format!(
                    "graph_id={} revision={} (existing)",
                    gid, graph.revision
                ));
            }
            require_write(&ctx)?;
            let title = args
                .title
                .or(args.key)
                .unwrap_or_else(|| "Checklist".into());
            let items = args.items;
            let item_keys = args.item_keys;
            let result = session
                .mutate_work(move |state| {
                    let mut graph = WorkGraph::new(title, Some(owner.clone()), GraphMode::Advisory);
                    graph.objective = args.objective;
                    for (index, item) in items.into_iter().enumerate() {
                        let fallback = item_keys
                            .get(index)
                            .cloned()
                            .filter(|key| !key.is_empty())
                            .unwrap_or_else(|| format!("item-{}", index + 1));
                        let (key, title) = item.into_key_title(fallback);
                        let node = crate::work::WorkNode::new(key, title);
                        graph.view_order.push(node.id);
                        graph.nodes.insert(node.id, node);
                    }
                    let gid = graph.id;
                    state.create_graph(graph.clone(), None)?;
                    state.set_active_graph(owner.clone(), gid)?;
                    Ok((gid, WorkEvent::GraphCreated { graph }))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!(
                "created graph_id={} revision={}",
                result,
                session.work.read().unwrap().graphs[&result].revision
            ))
        }
        Mode::Create => {
            require_write(&ctx)?;
            let title = args
                .title
                .or(args.key)
                .unwrap_or_else(|| "Checklist".into());
            let owner = ctx.agent_id.clone();
            let items = args.items;
            let item_keys = args.item_keys;
            let result = session
                .mutate_work(move |state| {
                    let mut graph = WorkGraph::new(title, Some(owner.clone()), GraphMode::Advisory);
                    graph.objective = args.objective;
                    for (index, item) in items.into_iter().enumerate() {
                        let fallback = item_keys
                            .get(index)
                            .cloned()
                            .filter(|key| !key.is_empty())
                            .unwrap_or_else(|| format!("item-{}", index + 1));
                        let (key, title) = item.into_key_title(fallback);
                        let node = crate::work::WorkNode::new(key, title);
                        graph.view_order.push(node.id);
                        graph.nodes.insert(node.id, node);
                    }
                    let gid = graph.id;
                    state.create_graph(graph.clone(), None)?;
                    state.set_active_graph(owner.clone(), gid)?;
                    Ok((gid, WorkEvent::GraphCreated { graph }))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!(
                "created graph_id={} revision={}",
                result,
                session.work.read().unwrap().graphs[&result].revision
            ))
        }
        Mode::SetActive => {
            require_write(&ctx)?;
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            let owner = ctx.agent_id.clone();
            let result = session
                .mutate_work(move |state| {
                    state.set_active_graph(owner.clone(), gid)?;
                    Ok((
                        gid,
                        WorkEvent::ActiveGraphChanged {
                            agent_id: owner,
                            graph_id: gid,
                        },
                    ))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!("active graph set graph_id={result}"))
        }
        Mode::Launch => {
            require_write(&ctx)?;
            if args.max_concurrent == Some(0) || args.max_attempts_total == Some(0) {
                return Err(ToolError::InvalidArguments(
                    "run limits must be positive".into(),
                ));
            }
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            {
                let state = session.work.read().unwrap();
                let graph = state
                    .graph(gid)
                    .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                if graph
                    .nodes
                    .values()
                    .any(|node| node.executor == crate::work::Executor::Agent)
                    && ctx
                        .allowed_scopes
                        .as_ref()
                        .is_some_and(|scopes| !scopes.contains(crate::DELEGATION_SCOPE))
                {
                    return Err(ToolError::PermissionDenied {
                        tool: "task".into(),
                        required: vec![crate::DELEGATION_SCOPE.into()],
                        allowed: ctx
                            .allowed_scopes
                            .clone()
                            .unwrap_or_default()
                            .into_iter()
                            .collect(),
                    });
                }
                if graph.mode != GraphMode::Managed {
                    return Err(ToolError::InvalidArguments(
                        "launch requires a managed graph: plan it with managed=true".into(),
                    ));
                }
                if graph.owner_agent_id.as_deref() != Some(ctx.agent_id.as_str()) {
                    return Err(ToolError::InvalidArguments(
                        "only the graph owner may launch a run".into(),
                    ));
                }
                let analysis = crate::work::analyze_planned_graph(graph, state.swarm.policy);
                if !analysis.launchable {
                    return Err(ToolError::InvalidArguments(format!(
                        "swarm plan analysis rejected launch: {}",
                        serde_json::to_string(&analysis)
                            .unwrap_or_else(|_| "unserializable analysis".into())
                    )));
                }
            }
            if session.has_live_run_for_graph(gid).await {
                return Err(ToolError::Failed(format!(
                    "graph {gid} already has a live managed run"
                )));
            }
            let cancellation = tokio_util::sync::CancellationToken::new();
            let launcher = std::sync::Arc::new(crate::tools::delegate::WorkNodeLauncher::new(
                session.clone(),
                ctx.agent_id.clone(),
                ctx.tool_call_id.clone(),
                cancellation.clone(),
            ));
            let run_id = uuid::Uuid::new_v4().to_string();
            let limits = crate::work::RunLimits {
                max_concurrent: args.max_concurrent.unwrap_or(8) as usize,
                max_attempts_total: args.max_attempts_total.unwrap_or(200) as usize,
            };
            let durable_run_id = run_id.clone();
            let durable_owner = ctx.agent_id.clone();
            session
                .mutate_work(move |state| {
                    if state.managed_runs.values().any(|run| {
                        run.graph_id == gid
                            && matches!(
                                run.status,
                                crate::work::ManagedRunStatus::Running
                                    | crate::work::ManagedRunStatus::Parked
                            )
                    }) {
                        return Err(crate::work::WorkError::InvalidGraph(
                            "graph already has a running or parked managed run".into(),
                        ));
                    }
                    let now = chrono::Utc::now();
                    state.managed_runs.insert(
                        durable_run_id.clone(),
                        crate::work::ManagedRunRecord {
                            run_id: durable_run_id,
                            graph_id: gid,
                            owner_agent_id: durable_owner,
                            max_concurrent: limits.max_concurrent,
                            max_attempts_total: limits.max_attempts_total,
                            status: crate::work::ManagedRunStatus::Running,
                            created_at: now,
                            updated_at: now,
                            generation: 0,
                        },
                    );
                    state.revision = state.revision.saturating_add(1);
                    Ok((
                        (),
                        WorkEvent::GraphChanged {
                            graph_id: gid,
                            revision: state.graph(gid)?.revision,
                        },
                    ))
                })
                .map_err(ToolError::Failed)?;
            // Register before the driver can publish RunStarted or do work,
            // so an immediate event-driven cancel always knows the run id.
            let (start_tx, start_rx) = tokio::sync::oneshot::channel();
            let run_session = session.clone();
            let driven_run_id = run_id.clone();
            let run_cancellation = cancellation.clone();
            let join = tokio::spawn(async move {
                if start_rx.await.is_err() {
                    // Registration rejected this launch. Exit without
                    // publishing lifecycle events or touching the graph.
                    return crate::work::RunReport {
                        graph_id: gid,
                        conclusion: crate::work::RunConclusion::Cancelled,
                        outcomes: Vec::new(),
                        diagnostics: Vec::new(),
                    };
                }
                let report = crate::work::drive_run_observed(
                    run_session,
                    gid,
                    driven_run_id,
                    launcher,
                    limits,
                    run_cancellation,
                )
                .await;
                report
            });
            session
                .register_run(
                    run_id.clone(),
                    crate::session::RunHandle {
                        graph_id: gid,
                        cancellation: cancellation.clone(),
                        join,
                    },
                )
                .await
                .map_err(|error| {
                    cancellation.cancel();
                    ToolError::Failed(error)
                })?;
            let _ = start_tx.send(());
            Ok(format!(
                "run_id={run_id} graph_id={gid} (durable; `park` checkpoints it, `resume` starts a new driver, `wait` awaits this process)"
            ))
        }
        Mode::Park => {
            require_write(&ctx)?;
            let run_id = args
                .run_id
                .as_deref()
                .ok_or_else(|| ToolError::InvalidArguments("park requires 'run_id'".into()))?;
            let gid = session
                .park_run(run_id, &ctx.agent_id)
                .await
                .map_err(ToolError::Failed)?;
            Ok(format!(
                "parked run_id={run_id} graph_id={gid}; in-flight attempts were durably interrupted and are resumable"
            ))
        }
        Mode::Resume => {
            require_write(&ctx)?;
            let run_id = args
                .run_id
                .as_deref()
                .ok_or_else(|| ToolError::InvalidArguments("resume requires 'run_id'".into()))?
                .to_string();
            let record = session
                .work
                .read()
                .unwrap()
                .managed_runs
                .get(&run_id)
                .cloned()
                .ok_or_else(|| ToolError::Failed(format!("unknown run_id: {run_id}")))?;
            if record.owner_agent_id != ctx.agent_id {
                return Err(ToolError::Failed(format!(
                    "only the run owner may resume {run_id}"
                )));
            }
            if record.status != crate::work::ManagedRunStatus::Parked {
                return Err(ToolError::Failed(format!("run {run_id} is not parked")));
            }
            if session.has_live_run_for_graph(record.graph_id).await {
                return Err(ToolError::Failed(format!(
                    "graph {} already has a live managed run",
                    record.graph_id
                )));
            }
            let gid = record.graph_id;
            let owner = ctx.agent_id.clone();
            let durable_id = run_id.clone();
            session
                .mutate_work(move |state| {
                    let node_ids = state.graph(gid)?.view_order.clone();
                    let auth = AuthorizationContext {
                        agent_id: owner,
                        can_manage: true,
                        assignment_ids: Default::default(),
                    };
                    for node_id in node_ids {
                        if state.graph(gid)?.nodes[&node_id].status
                            == crate::work::ExecutionStatus::Interrupted
                        {
                            let revision = state.graph(gid)?.revision;
                            state.retry(gid, revision, &auth, node_id)?;
                        }
                    }
                    let run = state.managed_runs.get_mut(&durable_id).ok_or_else(|| {
                        crate::work::WorkError::InvalidGraph("managed run disappeared".into())
                    })?;
                    run.status = crate::work::ManagedRunStatus::Running;
                    run.generation = run.generation.saturating_add(1);
                    run.updated_at = chrono::Utc::now();
                    state.revision = state.revision.saturating_add(1);
                    Ok((
                        (),
                        WorkEvent::GraphChanged {
                            graph_id: gid,
                            revision: state.graph(gid)?.revision,
                        },
                    ))
                })
                .map_err(ToolError::Failed)?;
            let cancellation = tokio_util::sync::CancellationToken::new();
            let launcher = std::sync::Arc::new(crate::tools::delegate::WorkNodeLauncher::new(
                session.clone(),
                ctx.agent_id.clone(),
                ctx.tool_call_id.clone(),
                cancellation.clone(),
            ));
            let limits = crate::work::RunLimits {
                max_concurrent: record.max_concurrent,
                max_attempts_total: record.max_attempts_total,
            };
            let (start_tx, start_rx) = tokio::sync::oneshot::channel();
            let run_session = session.clone();
            let driven_id = run_id.clone();
            let run_cancellation = cancellation.clone();
            let join = tokio::spawn(async move {
                if start_rx.await.is_err() {
                    return crate::work::RunReport {
                        graph_id: gid,
                        conclusion: crate::work::RunConclusion::Cancelled,
                        outcomes: Vec::new(),
                        diagnostics: Vec::new(),
                    };
                }
                crate::work::drive_run_observed(
                    run_session,
                    gid,
                    driven_id,
                    launcher,
                    limits,
                    run_cancellation,
                )
                .await
            });
            session
                .register_run(
                    run_id.clone(),
                    crate::session::RunHandle {
                        graph_id: gid,
                        cancellation: cancellation.clone(),
                        join,
                    },
                )
                .await
                .map_err(|error| {
                    cancellation.cancel();
                    ToolError::Failed(error)
                })?;
            let _ = start_tx.send(());
            Ok(format!(
                "resumed run_id={run_id} graph_id={gid} generation={}",
                record.generation.saturating_add(1)
            ))
        }
        Mode::Poll => {
            let run_id = args
                .run_id
                .as_deref()
                .ok_or_else(|| ToolError::InvalidArguments("poll requires 'run_id'".into()))?;
            let live = session.poll_run(run_id).await;
            let durable = session
                .work
                .read()
                .unwrap()
                .managed_runs
                .get(run_id)
                .cloned();
            let (gid, status) = match (live, durable) {
                (Some((gid, finished)), _record) => (
                    gid,
                    if finished {
                        "finished".to_string()
                    } else {
                        "running".to_string()
                    },
                ),
                (None, Some(record)) => (
                    record.graph_id,
                    format!("{:?}", record.status).to_lowercase(),
                ),
                (None, None) => return Err(ToolError::Failed(format!("unknown run_id: {run_id}"))),
            };
            // Progress is derived from the graph, not from buffered events,
            // so a poll always reports durable state.
            let state = session.work.read().unwrap();
            let graph = state
                .graph(gid)
                .map_err(|e| ToolError::Failed(e.to_string()))?;
            let nodes = graph
                .view_order
                .iter()
                .filter_map(|id| graph.nodes.get(id))
                .map(|node| {
                    json!({
                        "key": node.key,
                        "status": node.status,
                        "outcome": outcome_label(node.effective_outcome.as_ref()),
                        "summary": node
                            .attempt_ids
                            .last()
                            .and_then(|a| graph.attempts.get(a))
                            .and_then(|a| a.result_id)
                            .and_then(|r| graph.results.get(&r))
                            .map(|r| r.summary.clone()),
                    })
                })
                .collect::<Vec<_>>();
            Ok(json!({
                "run_id": run_id,
                "graph_id": gid.to_string(),
                "status": status,
                "revision": graph.revision,
                "nodes": nodes,
            })
            .to_string())
        }
        Mode::Await => {
            let run_id = args
                .run_id
                .as_deref()
                .ok_or_else(|| ToolError::InvalidArguments("wait requires 'run_id'".into()))?;
            let (gid, _) = session.poll_run(run_id).await.ok_or_else(|| {
                let status = session
                    .work
                    .read()
                    .unwrap()
                    .managed_runs
                    .get(run_id)
                    .map(|r| format!("{:?}", r.status).to_lowercase());
                ToolError::Failed(match status {
                    Some(status) => {
                        format!("run {run_id} has no live driver (durable status: {status})")
                    }
                    None => format!("unknown run_id: {run_id}"),
                })
            })?;
            {
                let state = session.work.read().unwrap();
                let graph = state
                    .graph(gid)
                    .map_err(|error| ToolError::Failed(error.to_string()))?;
                if graph.owner_agent_id.as_deref() != Some(ctx.agent_id.as_str()) {
                    return Err(ToolError::Failed(format!(
                        "agent '{}' is not authorized to await run {run_id}",
                        ctx.agent_id
                    )));
                }
            }
            let report = session.wait_run(run_id).await.map_err(ToolError::Failed)?;
            let state = session.work.read().unwrap();
            let graph = state
                .graph(report.graph_id)
                .map_err(|e| ToolError::Failed(e.to_string()))?;
            Ok(json!({
                "run_id": run_id,
                "graph_id": report.graph_id.to_string(),
                "diagnostics": report.diagnostics.iter().map(|d| json!({"node":d.node_key,"stage":d.stage,"message":d.message,"fatal":d.fatal})).collect::<Vec<_>>(),
                "conclusion": format!("{:?}", report.conclusion),
                "nodes": report
                    .outcomes
                    .iter()
                    .map(|o| json!({
                        "key": o.node_key,
                        "status": o.status,
                        "outcome": outcome_label(graph.nodes.values().find(|n| n.key == o.node_key).and_then(|n| n.effective_outcome.as_ref())),
                        "summary": o.summary,
                    }))
                    .collect::<Vec<_>>(),
            })
            .to_string())
        }
        Mode::Cancel if args.run_id.is_some() => {
            require_write(&ctx)?;
            let run_id = args.run_id.as_deref().expect("guarded above");
            let gid = session
                .cancel_run(run_id, &ctx.agent_id)
                .await
                .map_err(ToolError::Failed)?;
            Ok(format!("cancelled run_id={run_id} graph_id={gid}"))
        }
        Mode::QualityDigest => {
            let state = session.work.read().unwrap();
            let gid = args
                .graph_id
                .as_ref()
                .map(|v| GraphId::parse(v))
                .transpose()
                .map_err(|e| ToolError::InvalidArguments(e.to_string()))?
                .or_else(|| state.active_graph_by_agent.get(&ctx.agent_id).copied())
                .ok_or_else(|| ToolError::InvalidArguments("no graph selected".into()))?;
            let graph = state
                .graphs
                .get(&gid)
                .ok_or_else(|| ToolError::InvalidArguments("graph not found".into()))?;
            let digest = crate::work::transition::quality_digest(graph);
            Ok(serde_json::to_string(&digest).unwrap_or_default())
        }
        Mode::PlanAnalysis => {
            let state = session.work.read().unwrap();
            let gid = args
                .graph_id
                .as_ref()
                .map(|value| GraphId::parse(value))
                .transpose()
                .map_err(|error| ToolError::InvalidArguments(error.to_string()))?
                .or_else(|| state.active_graph_by_agent.get(&ctx.agent_id).copied())
                .ok_or_else(|| ToolError::InvalidArguments("no graph selected".into()))?;
            let graph = state
                .graph(gid)
                .map_err(|error| ToolError::InvalidArguments(error.to_string()))?;
            Ok(serde_json::to_string(&crate::work::analyze_planned_graph(
                graph,
                state.swarm.policy,
            ))
            .unwrap_or_default())
        }
        Mode::Integration => {
            require_write(&ctx)?;
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            let strategy = args.integration.as_ref().ok_or_else(|| {
                ToolError::InvalidArguments("integration requires integration fields".into())
            })?;
            let strategy: crate::work::IntegrationStrategy = strategy.into();
            let requested_revision = args.expected_revision;
            let caller = ctx.agent_id.clone();
            session
                .mutate_work(move |state| {
                    if let Some(expected) = requested_revision
                        && state.graph(gid)?.revision != expected
                    {
                        return Err(crate::work::WorkError::StaleRevision {
                            expected,
                            actual: state.graph(gid)?.revision,
                        });
                    }
                    let expected = state.revision;
                    state
                        .configure_integration_strategy(expected, gid, &caller, strategy)
                        .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                    let revision = state.graph(gid)?.revision;
                    Ok((
                        (),
                        WorkEvent::GraphChanged {
                            graph_id: gid,
                            revision,
                        },
                    ))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!("integration strategy configured graph_id={gid}"))
        }
        Mode::Squad => {
            let state = session.work.read().unwrap();
            let gid = args
                .graph_id
                .as_ref()
                .map(|value| GraphId::parse(value))
                .transpose()
                .map_err(|error| ToolError::InvalidArguments(error.to_string()))?
                .or_else(|| state.active_graph_by_agent.get(&ctx.agent_id).copied())
                .or_else(|| {
                    state
                        .binding_for_agent(&ctx.agent_id)
                        .map(|binding| binding.graph_id)
                })
                .ok_or_else(|| ToolError::InvalidArguments("no graph selected".into()))?;
            let binding = state
                .binding_for_agent(&ctx.agent_id)
                .filter(|binding| binding.graph_id == gid);
            let snapshot =
                crate::work::SquadSnapshot::from_state(&state, gid, chrono::Utc::now(), 32);
            Ok(json!({
                "snapshot": snapshot,
                "you": binding.map(|binding| json!({
                    "assignment_id": binding.assignment_id.to_string(),
                    "generation": state.assignment_token(binding.assignment_id).map(|token| token.generation),
                })),
            })
            .to_string())
        }
        Mode::SwarmPolicy => {
            require_write(&ctx)?;
            let _swarm_edit = session.lock_swarm_edits().await;
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            let policy = match args.policy.as_deref() {
                Some("disabled") => crate::work::SwarmPolicy::Disabled,
                Some("advisory") => crate::work::SwarmPolicy::Advisory,
                Some("protective") => crate::work::SwarmPolicy::Protective,
                _ => {
                    return Err(invalid_field(
                        "invalid_policy",
                        "policy",
                        &["disabled", "advisory", "protective"],
                        "choose an explicit opt-in swarm policy",
                    ));
                }
            };
            let requested_revision = args.expected_revision;
            let caller = ctx.agent_id.clone();
            let caller_is_top_level = session
                .hierarchy
                .read()
                .unwrap()
                .get(&caller)
                .is_some_and(|node| node.parent_id.is_none());
            session
                .mutate_work(move |state| {
                    if state.graph(gid)?.owner_agent_id.as_deref() != Some(caller.as_str()) {
                        return Err(crate::work::WorkError::Unauthorized { agent: caller });
                    }
                    if !caller_is_top_level {
                        return Err(crate::work::WorkError::Unauthorized { agent: caller });
                    }
                    if let Some(expected) = requested_revision
                        && state.graph(gid)?.revision != expected
                    {
                        return Err(crate::work::WorkError::StaleRevision {
                            expected,
                            actual: state.graph(gid)?.revision,
                        });
                    }
                    let expected = state.revision;
                    state
                        .configure_swarm_policy(expected, &caller, policy)
                        .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                    let revision = state.graph(gid)?.revision;
                    Ok((
                        (),
                        WorkEvent::GraphChanged {
                            graph_id: gid,
                            revision,
                        },
                    ))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!("swarm policy={policy:?} graph_id={gid}"))
        }
        Mode::Claim
        | Mode::ReleaseClaim
        | Mode::Milestone
        | Mode::CoordinationRequest
        | Mode::CoordinationRespond
        | Mode::OwnershipTransfer
        | Mode::OwnershipDecision => {
            require_write(&ctx)?;
            let _swarm_edit = session.lock_swarm_edits().await;
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            let requested_revision = args.expected_revision;
            let caller = ctx.agent_id.clone();
            let workspace_id = ctx
                .workspace()
                .identity(&ctx.workdir)
                .map_err(|error| ToolError::Failed(error.to_string()))?
                .to_string();
            let claim_resources = if selected_mode == Mode::Claim {
                let transport = ctx.workspace();
                let identity = transport
                    .identity(&ctx.workdir)
                    .map_err(|error| ToolError::Failed(error.to_string()))?;
                Some(
                    args.resource_paths
                        .iter()
                        .map(|path| {
                            let kind = if path.ends_with('/') {
                                crate::work::ResourceKind::Directory
                            } else {
                                crate::work::ResourceKind::File
                            };
                            let resolved = transport
                                .normalize_write_resource(&ctx.workdir, path)
                                .map_err(|error| match error {
                                crate::workspace::WorkspaceError::InvalidPath(message) => {
                                    ToolError::InvalidArguments(message)
                                }
                                other => ToolError::Failed(other.to_string()),
                            })?;
                            let normalized = if let Some(root) = identity.canonical_local_root() {
                                let relative = resolved.strip_prefix(root).map_err(|_| {
                                    ToolError::Failed(
                                        "normalized claim resource escaped workspace".into(),
                                    )
                                })?;
                                relative
                                    .components()
                                    .map(|component| {
                                        component.as_os_str().to_str().ok_or_else(|| {
                                            ToolError::InvalidArguments(
                                                "resolved claim path is not valid UTF-8".into(),
                                            )
                                        })
                                    })
                                    .collect::<Result<Vec<_>, _>>()?
                                    .join("/")
                            } else {
                                crate::work::normalize_workspace_path(path).map_err(|error| {
                                    ToolError::InvalidArguments(error.to_string())
                                })?
                            };
                            Ok(crate::work::WorkspaceResource {
                                workspace_id: workspace_id.clone(),
                                path: normalized,
                                kind,
                                access: crate::work::ResourceAccess::Mutate,
                                resolution: crate::work::PathResolutionPolicy::LexicalOnly,
                            })
                        })
                        .collect::<Result<Vec<_>, ToolError>>()?,
                )
            } else {
                None
            };
            let detail = session
                .mutate_work(move |state| {
                    let binding = state
                        .binding_for_agent(&caller)
                        .filter(|binding| binding.graph_id == gid)
                        .cloned()
                        .ok_or_else(|| crate::work::WorkError::Unauthorized {
                            agent: caller.clone(),
                        })?;
                    let token = state
                        .assignment_token(binding.assignment_id)
                        .cloned()
                        .ok_or(crate::work::WorkError::StaleAssignmentGeneration)?;
                    if let Some(expected) = requested_revision
                        && state.graph(gid)?.revision != expected
                    {
                        return Err(crate::work::WorkError::StaleRevision {
                            expected,
                            actual: state.graph(gid)?.revision,
                        });
                    }
                    let expected = state.revision;
                    let value = match selected_mode {
                        Mode::Claim => {
                            if args.resource_paths.is_empty() {
                                return Err(crate::work::WorkError::InvalidGraph(
                                    "claim requires resource_paths".into(),
                                ));
                            }
                            let access = match args.resource_access.as_deref() {
                                None | Some("mutate") => crate::work::ResourceAccess::Mutate,
                                Some("inspect") => crate::work::ResourceAccess::Inspect,
                                Some(other) => return Err(crate::work::WorkError::InvalidGraph(
                                    format!("unknown resource_access '{other}'"),
                                )),
                            };
                            let mut resources = claim_resources.clone().unwrap_or_default();
                            for resource in &mut resources {
                                resource.access = access;
                            }
                            let (claim, conflicts) = state.register_resources(
                                expected, token, resources, None,
                            ).map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            json!({"claim_id": claim.to_string(), "conflicting_claim_ids": conflicts.iter().map(ToString::to_string).collect::<Vec<_>>()})
                        }
                        Mode::ReleaseClaim => {
                            let claim = crate::work::ResourceClaimId::parse(
                                args.claim_id.as_deref().ok_or_else(|| crate::work::WorkError::InvalidGraph("release_claim requires claim_id".into()))?
                            ).map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            state.release_resources(expected, token, claim)
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            json!({"released_claim_id": claim.to_string()})
                        }
                        Mode::Milestone => {
                            let name = args.milestone.clone().ok_or_else(|| crate::work::WorkError::InvalidGraph("milestone requires milestone".into()))?;
                            let id = state.record_milestone(expected, token, name, args.payload.clone().unwrap_or_else(|| json!({})))
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            json!({"milestone_id": id.to_string()})
                        }
                        Mode::CoordinationRequest => {
                            let key = args.idempotency_key.clone().ok_or_else(|| crate::work::WorkError::InvalidGraph("coordination_request requires idempotency_key".into()))?;
                            let kind = args.request_kind.clone().ok_or_else(|| crate::work::WorkError::InvalidGraph("coordination_request requires request_kind".into()))?;
                            let parties = args.peer_agent_ids.iter().cloned().map(|agent| (agent, crate::work::CoordinationRole::Responder)).collect();
                            let id = state.request_coordination(expected, token, key, kind, args.payload.clone().unwrap_or_else(|| json!({})), parties)
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            json!({"request_id": id.to_string()})
                        }
                        Mode::CoordinationRespond => {
                            let id = crate::work::CoordinationRequestId::parse(args.request_id.as_deref().ok_or_else(|| crate::work::WorkError::InvalidGraph("coordination_respond requires request_id".into()))?)
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            let peer_state = match args.peer_state.as_deref() {
                                Some("accepted") => crate::work::CoordinationPeerState::Accepted,
                                Some("rejected") => crate::work::CoordinationPeerState::Rejected,
                                Some("declined") => crate::work::CoordinationPeerState::Declined,
                                Some("cancelled") => crate::work::CoordinationPeerState::Cancelled,
                                _ => return Err(crate::work::WorkError::InvalidGraph("peer_state must be accepted, rejected, declined, or cancelled".into())),
                            };
                            state.settle_coordination_peer(expected, id, &caller, peer_state, args.payload.clone())
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            json!({"request_id": id.to_string(), "state": peer_state})
                        }
                        Mode::OwnershipTransfer => {
                            let claim = crate::work::ResourceClaimId::parse(args.claim_id.as_deref().ok_or_else(|| crate::work::WorkError::InvalidGraph("ownership_transfer requires claim_id".into()))?)
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            let target = crate::work::AssignmentId::parse(args.target_assignment_id.as_deref().ok_or_else(|| crate::work::WorkError::InvalidGraph("ownership_transfer requires target_assignment_id".into()))?)
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            let target = state.assignment_token(target).cloned().ok_or(crate::work::WorkError::StaleAssignmentGeneration)?;
                            let key = args.idempotency_key.clone().ok_or_else(|| crate::work::WorkError::InvalidGraph("ownership_transfer requires idempotency_key".into()))?;
                            let id = state.request_ownership_transfer(expected, token, target, claim, key, args.rationale.clone().unwrap_or_default())
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            json!({"transfer_id": id.to_string()})
                        }
                        Mode::OwnershipDecision => {
                            let id = crate::work::OwnershipTransferId::parse(args.transfer_id.as_deref().ok_or_else(|| crate::work::WorkError::InvalidGraph("ownership_decision requires transfer_id".into()))?)
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            let approved = args.approved.ok_or_else(|| crate::work::WorkError::InvalidGraph("ownership_decision requires approved".into()))?;
                            let replacement = state.decide_ownership_transfer(expected, id, &caller, approved, args.rationale.clone())
                                .map_err(|error| crate::work::WorkError::InvalidGraph(error.to_string()))?;
                            json!({"transfer_id": id.to_string(), "approved": approved, "replacement_claim_id": replacement.map(|id| id.to_string())})
                        }
                        _ => unreachable!(),
                    };
                    let revision = state.graph(gid)?.revision;
                    Ok((value, WorkEvent::GraphChanged { graph_id: gid, revision }))
                })
                .map_err(ToolError::Failed)?;
            Ok(detail.to_string())
        }
        Mode::Close => {
            require_write(&ctx)?;
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            let cancelled = matches!(
                args.verdict.as_deref().or(args.outcome.as_deref()),
                Some("cancelled")
            );
            let requested_revision = args.expected_revision;
            let status = if cancelled {
                GraphStatus::Cancelled
            } else {
                GraphStatus::Completed
            };
            let graph = session
                .work
                .read()
                .unwrap()
                .graph(gid)
                .map_err(|e| ToolError::Failed(e.to_string()))?
                .clone();
            let auth_ctx = auth(&ctx, &graph);
            session
                .mutate_work(move |state| {
                    let expected = requested_revision.unwrap_or(state.graph(gid)?.revision);
                    state.close_graph(gid, expected, &auth_ctx, status)?;
                    let revision = state.graph(gid)?.revision;
                    Ok((
                        (),
                        WorkEvent::GraphChanged {
                            graph_id: gid,
                            revision,
                        },
                    ))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!(
                "graph closed graph_id={} status={}",
                gid,
                if cancelled { "cancelled" } else { "completed" }
            ))
        }
        mode => {
            require_write(&ctx)?;
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            let requested_revision = args.expected_revision;
            let assigned = assigned_node(&session, &ctx.agent_id);
            let detail = session
                .mutate_work(move |state| {
                    let graph = state.graph(gid)?.clone();
                    // `mutate_work` serializes this read with the following
                    // write, so omission cannot race a separate mutation.
                    let expected = requested_revision.unwrap_or(graph.revision);
                    let context = auth(&ctx, &graph);
                    let (value, event) = match mode {
                        Mode::Connect => {
                            let from = args
                                .from_node_id
                                .as_deref()
                                .ok_or_else(|| {
                                    crate::work::WorkError::InvalidGraph(
                                        "connect requires 'from_node_id'".into(),
                                    )
                                })
                                .and_then(|v| {
                                    NodeId::parse(v).map_err(|e| {
                                        crate::work::WorkError::InvalidGraph(e.to_string())
                                    })
                                })?;
                            let to = args
                                .to_node_id
                                .as_deref()
                                .ok_or_else(|| {
                                    crate::work::WorkError::InvalidGraph(
                                        "connect requires 'to_node_id'".into(),
                                    )
                                })
                                .and_then(|v| {
                                    NodeId::parse(v).map_err(|e| {
                                        crate::work::WorkError::InvalidGraph(e.to_string())
                                    })
                                })?;
                            let condition = parse_condition(&args.condition)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let binding = args.binding_alias.clone().map(|alias| InputBinding {
                                alias,
                                selection: ResultSelection { field: None },
                            });
                            let edge_id = state.add_edge(
                                gid,
                                expected,
                                &context,
                                from,
                                to,
                                condition,
                                args.required.unwrap_or(true),
                                binding,
                            )?;
                            let _ = edge_id;
                            (
                                String::new(),
                                WorkEvent::GraphChanged {
                                    graph_id: gid,
                                    revision: state.graph(gid)?.revision,
                                },
                            )
                        }
                        Mode::Disconnect => {
                            let edge_id = args
                                .edge_id
                                .as_deref()
                                .ok_or_else(|| {
                                    crate::work::WorkError::InvalidGraph(
                                        "disconnect requires 'edge_id'".into(),
                                    )
                                })
                                .and_then(|v| {
                                    EdgeId::parse(v).map_err(|e| {
                                        crate::work::WorkError::InvalidGraph(e.to_string())
                                    })
                                })?;
                            state.remove_edge(gid, expected, &context, edge_id)?;
                            (
                                String::new(),
                                WorkEvent::GraphChanged {
                                    graph_id: gid,
                                    revision: state.graph(gid)?.revision,
                                },
                            )
                        }
                        Mode::Annotate => {
                            let result_id = args
                                .result_id
                                .as_deref()
                                .ok_or_else(|| {
                                    crate::work::WorkError::InvalidGraph(
                                        "annotate requires 'result_id'".into(),
                                    )
                                })
                                .and_then(|v| {
                                    crate::work::ResultId::parse(v).map_err(|e| {
                                        crate::work::WorkError::InvalidGraph(e.to_string())
                                    })
                                })?;
                            let kind = parse_annotation_kind(&args.annotation_kind)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let text = args.text.clone().unwrap_or_default();
                            let annotation_id = state
                                .annotate_result(gid, expected, &context, result_id, kind, text)?;
                            let _ = annotation_id;
                            (
                                String::new(),
                                WorkEvent::GraphChanged {
                                    graph_id: gid,
                                    revision: state.graph(gid)?.revision,
                                },
                            )
                        }
                        Mode::Configure => {
                            let nid = node_id(&graph, &args, assigned)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let join = parse_join_policy(&args)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?
                                .map(Some);
                            let retry_policy =
                                args.max_attempts
                                    .map(|max_attempts| crate::work::RetryPolicy {
                                        max_attempts,
                                        retryable_outcomes: Default::default(),
                                    });
                            let executor = parse_executor(&args.executor)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let verification = required_verification(&args)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let acceptance_criteria = if args.acceptance_criteria.is_empty() {
                                None
                            } else {
                                Some(
                                    args.acceptance_criteria
                                        .iter()
                                        .cloned()
                                        .map(crate::work::AcceptanceCriterion::new)
                                        .collect(),
                                )
                            };
                            let review_policy = args.requires_independent_reviewer.map(|value| {
                                crate::work::ReviewPolicy {
                                    requires_independent_reviewer: value,
                                }
                            });
                            state.configure_node(
                                gid,
                                expected,
                                &context,
                                nid,
                                join,
                                retry_policy,
                                executor,
                                verification,
                                acceptance_criteria,
                                review_policy,
                            )?;
                            (
                                String::new(),
                                WorkEvent::NodeChanged {
                                    graph_id: gid,
                                    node: state.graph(gid)?.nodes[&nid].clone(),
                                },
                            )
                        }
                        Mode::Add => {
                            let mut inputs: Vec<crate::work::NodeInput> = Vec::new();
                            let mut next_index = graph.nodes.len() + 1;
                            let mut taken: Vec<String> =
                                graph.nodes.values().map(|n| n.key.clone()).collect();
                            let mut alloc_key = |preferred: Option<String>| {
                                if let Some(key) = preferred {
                                    if !taken.iter().any(|k| k == &key) {
                                        taken.push(key.clone());
                                        return key;
                                    }
                                }
                                loop {
                                    let key = format!("item-{next_index}");
                                    next_index += 1;
                                    if !taken.iter().any(|k| k == &key) {
                                        taken.push(key.clone());
                                        return key;
                                    }
                                }
                            };
                            if let Some(title) = args.title.clone() {
                                inputs.push(crate::work::NodeInput {
                                    key: alloc_key(args.key.clone()),
                                    title,
                                    description: args.description.value(),
                                });
                            }
                            for (index, item) in args.items.iter().cloned().enumerate() {
                                let (key, title) =
                                    item.into_key_title(format!("item-{}", index + 1));
                                inputs.push(crate::work::NodeInput {
                                    key: alloc_key(Some(key)),
                                    title,
                                    description: None,
                                });
                            }
                            let ids = state.add_nodes(gid, expected, &context, inputs)?;
                            let added: Vec<String> = ids
                                .iter()
                                .filter_map(|id| state.graph(gid).ok()?.nodes.get(id))
                                .map(|n| format!("{} ({})", n.key, n.id))
                                .collect();
                            (
                                format!("added {}", added.join(", ")),
                                WorkEvent::GraphChanged {
                                    graph_id: gid,
                                    revision: state.graph(gid)?.revision,
                                },
                            )
                        }
                        // Complete is its own arm because it is the one
                        // mode that settles several nodes in a single
                        // revision. Everything below it is single-node.
                        Mode::Plan => {
                            let nodes = args
                                .nodes
                                .iter()
                                .map(plan_node)
                                .collect::<Result<Vec<_>, _>>()
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let edges = args
                                .edges
                                .iter()
                                .map(plan_edge)
                                .collect::<Result<Vec<_>, _>>()
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let created = state.plan(
                                gid,
                                expected,
                                &context,
                                nodes,
                                edges,
                                args.brief.clone(),
                                args.managed,
                            )?;
                            if let Some(integration) = args.integration.as_ref() {
                                state
                                    .configure_integration_strategy(
                                        state.revision,
                                        gid,
                                        &context.agent_id,
                                        integration.into(),
                                    )
                                    .map_err(|error| {
                                        crate::work::WorkError::InvalidGraph(error.to_string())
                                    })?;
                            }
                            let graph = state.graph(gid)?;
                            let summary = created
                                .iter()
                                .filter_map(|id| graph.nodes.get(id))
                                .map(|n| n.key.clone())
                                .collect::<Vec<_>>()
                                .join(", ");
                            (
                                format!(
                                    "planned {} node(s) [{summary}] and {} edge(s); mode={:?}",
                                    created.len(),
                                    args.edges.len(),
                                    graph.mode
                                ),
                                WorkEvent::GraphChanged {
                                    graph_id: gid,
                                    revision: graph.revision,
                                },
                            )
                        }
                        Mode::Complete => {
                            let targets = complete_targets(&graph, &args, assigned)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            let summary =
                                args.summary.clone().unwrap_or_else(|| "completed".into());
                            let verification = performed_verification(&args)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?
                                .unwrap_or_default();
                            let batch: Vec<(NodeId, String)> =
                                targets.iter().map(|id| (*id, summary.clone())).collect();
                            let results = state.complete_many(
                                gid,
                                expected,
                                &context,
                                batch,
                                args.evidence
                                    .iter()
                                    .chain(args.performed_checks.iter())
                                    .cloned()
                                    .collect(),
                                verification,
                            )?;
                            let settled = state.graph(gid)?;
                            let detail = targets
                                .iter()
                                .filter_map(|id| settled.nodes.get(id))
                                .map(|n| n.key.clone())
                                .collect::<Vec<_>>()
                                .join(", ");
                            let last = *results.last().expect("batch settled at least one node");
                            (
                                format!("completed {detail}"),
                                WorkEvent::ResultRecorded {
                                    graph_id: gid,
                                    result: settled.results[&last].clone(),
                                },
                            )
                        }
                        _ => {
                            let nid = node_id(&graph, &args, assigned)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            match mode {
                                Mode::Update => {
                                    state.update_node(
                                        gid,
                                        expected,
                                        &context,
                                        nid,
                                        args.title,
                                        args.description.into_update(),
                                    )?;
                                    (
                                        String::new(),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Move => {
                                    let before = args
                                        .before_node_id
                                        .as_deref()
                                        .map(|v| {
                                            NodeId::parse(v).map_err(|e| {
                                                crate::work::WorkError::InvalidGraph(e.to_string())
                                            })
                                        })
                                        .transpose()?;
                                    state.move_node(gid, expected, &context, nid, before)?;
                                    (
                                        String::new(),
                                        WorkEvent::GraphChanged {
                                            graph_id: gid,
                                            revision: state.graph(gid)?.revision,
                                        },
                                    )
                                }
                                Mode::Start => {
                                    let attempt = state.start(
                                        gid,
                                        expected,
                                        &context,
                                        nid,
                                        Some(ctx.agent_id.clone()),
                                    )?;
                                    (
                                        format!(
                                            "started key={} node_id={}",
                                            graph.nodes[&nid].key, nid
                                        ),
                                        WorkEvent::AttemptChanged {
                                            graph_id: gid,
                                            attempt: state.graph(gid)?.attempts[&attempt].clone(),
                                        },
                                    )
                                }
                                Mode::Fail => {
                                    let verification = performed_verification(&args)
                                        .map_err(|e| {
                                            crate::work::WorkError::InvalidGraph(e.to_string())
                                        })?
                                        .unwrap_or_default();
                                    let outcome =
                                        match args.verdict.as_deref().or(args.outcome.as_deref()) {
                                            Some("test_failed") => Outcome::TestFailed,
                                            Some("blocked") => Outcome::Blocked,
                                            Some("cancelled") => Outcome::Cancelled,
                                            Some(other) => Outcome::Custom(other.into()),
                                            None => Outcome::Failure,
                                        };
                                    let result = state.fail(
                                        gid,
                                        expected,
                                        &context,
                                        nid,
                                        outcome,
                                        args.summary.unwrap_or_else(|| "failed".into()),
                                        args.evidence
                                            .into_iter()
                                            .chain(args.performed_checks.into_iter())
                                            .collect(),
                                        Vec::new(),
                                        verification,
                                    )?;
                                    (
                                        String::new(),
                                        WorkEvent::ResultRecorded {
                                            graph_id: gid,
                                            result: state.graph(gid)?.results[&result].clone(),
                                        },
                                    )
                                }
                                Mode::Block => {
                                    state.block(gid, expected, &context, nid)?;
                                    (
                                        String::new(),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Unblock => {
                                    state.unblock(gid, expected, &context, nid)?;
                                    (
                                        String::new(),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Cancel => {
                                    state.cancel(gid, expected, &context, nid)?;
                                    (
                                        String::new(),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Retry => {
                                    state.retry(gid, expected, &context, nid)?;
                                    (
                                        String::new(),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                _ => unreachable!(),
                            }
                        }
                    };
                    Ok((value, event))
                })
                .map_err(ToolError::Failed)?;
            let revision = session.work.read().unwrap().graphs[&gid].revision;
            if detail.is_empty() {
                Ok(format!(
                    "task mutation committed graph_id={gid} revision={revision}"
                ))
            } else {
                Ok(format!(
                    "task mutation committed graph_id={gid} revision={revision} {detail}"
                ))
            }
        }
    }
}
