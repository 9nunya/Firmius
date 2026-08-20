//! The model-facing flat-schema checklist/work tool.
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::json;

use crate::work::{
    AuthorizationContext, EdgeCondition, EdgeId, GraphId, GraphMode, GraphStatus, InputBinding,
    JoinPolicy, NodeId, Outcome, ResultSelection, WorkEvent, WorkGraph,
};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

pub const WORK_READ_SCOPE: &str = "work_read";
pub const WORK_WRITE_SCOPE: &str = "work_write";

#[derive(Debug, Clone, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Mode {
    Create,
    Init,
    List,
    View,
    Add,
    Plan,
    Launch,
    Poll,
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
struct PlanNodeArg {
    /// Stable identifier used by edges and later `start`/`complete` calls.
    key: String,
    title: String,
    #[serde(default)]
    description: Option<String>,
    /// `manual` (default, you or a worker claims it), `agent` (the run
    /// spawns a subagent), or `command`.
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
    #[serde(default)]
    verification: Option<String>,
    #[serde(default)]
    acceptance_criteria: Vec<String>,
    #[serde(default)]
    requires_independent_reviewer: Option<bool>,
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
    #[serde(default)]
    mode: Mode,
    #[serde(default)]
    graph_id: Option<String>,
    #[serde(default)]
    node_id: Option<String>,
    #[serde(default)]
    key: Option<String>,
    /// Node keys (or node_ids) to settle in ONE call. `complete` accepts
    /// several at once so finishing N checklist items is one transaction
    /// and one revision, not N round trips.
    #[serde(default)]
    keys: Vec<String>,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    description: Option<String>,
    #[serde(default)]
    before_node_id: Option<String>,
    /// Graph revision from the last `view`. Required for every mutation
    /// except create/init/set_active. Stale values fail closed.
    #[serde(default)]
    expected_revision: Option<u64>,
    #[serde(default)]
    outcome: Option<String>,
    #[serde(default)]
    summary: Option<String>,
    #[serde(default)]
    evidence: Vec<String>,
    #[serde(default)]
    objective: Option<String>,
    /// Initial checklist titles for `create`/`init`. One item per distinct
    /// piece of work, including work you will do yourself.
    #[serde(default)]
    items: Vec<String>,
    // M4.7 — connect/disconnect/configure.
    #[serde(default)]
    from_node_id: Option<String>,
    #[serde(default)]
    to_node_id: Option<String>,
    #[serde(default)]
    edge_id: Option<String>,
    #[serde(default)]
    condition: Option<String>,
    #[serde(default)]
    required: Option<bool>,
    #[serde(default)]
    binding_alias: Option<String>,
    #[serde(default)]
    join_policy: Option<String>,
    #[serde(default)]
    join_required: Option<u32>,
    #[serde(default)]
    join_total: Option<u32>,
    #[serde(default)]
    max_attempts: Option<u32>,
    #[serde(default)]
    executor: Option<String>,
    // M5 — verification, acceptance criteria, and annotations.
    #[serde(default)]
    verification: Option<String>,
    #[serde(default)]
    acceptance_criteria: Vec<String>,
    #[serde(default)]
    requires_independent_reviewer: Option<bool>,
    #[serde(default)]
    result_id: Option<String>,
    #[serde(default)]
    annotation_kind: Option<String>,
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
    /// Run id from `launch`, for `poll` / `await`.
    #[serde(default)]
    run_id: Option<String>,
    /// Max nodes a run executes at once. Defaults to 8.
    #[serde(default)]
    max_concurrent: Option<u32>,
    /// Hard ceiling on attempts launched by one run. Defaults to 200.
    #[serde(default)]
    max_attempts_total: Option<u32>,
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
        Some("command") => Some(crate::work::Executor::Command),
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
        verification: parse_verification(&arg.verification)?.unwrap_or_default(),
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
            "Create and mutate the durable session work checklist. This is the session's source of truth for what you are doing — keep it current even when you work solo. The TUI renders it; later agents and resumes read it. Do not keep a private mental todo list instead of this graph.\n\nTypical flow:\n1. `init` (or `create`) a graph with `title`, `objective`, and `items` — one item per distinct piece of work. Init is idempotent for the calling agent.\n2. `view` often. After init, omit `graph_id` — writes use the active graph. Mutations other than create/init/set_active require `expected_revision` from the last view.\n3. Do the work yourself: `start` a node (bound workers may omit key), then `complete` / `fail` / `block`. Pass `expected_revision` every time.\n   Tracking an item as a plain todo? Just `complete` it — you do NOT need to `start` first. Finished several? `complete` with `keys: [\"item-1\", \"item-3\"]` settles them all in ONE call and ONE revision. A node a worker currently holds is never completable this way; let the worker `yield`.\n4. Expand the list with `add` `title` (one node) or `add` `items` (many nodes, one revision). Do not parallelize task mutations that share a revision; batch with `items` instead.\n5. Hand a node to a worker: `add` and leave it Pending, then `delegate` with `task_id` set to the node's `key` or `node_id`. Do NOT `task start` a node you are about to delegate.\n6. After a worker yields, `view` / `quality_digest` and only then close.\n\n`plan` — author a whole DAG in ONE call when work has real structure (fan-out, fan-in, gates). Pass `nodes` and `edges` as arrays keyed by `key`, plus an optional `brief`. It is additive: a later `plan` can attach new nodes to existing ones, so the graph can grow as you learn. Two independent axes: an edge's `condition`/`required` control WHEN the successor may run, and `binding_alias` controls WHAT data it receives. An optional edge can still deliver data; a gating edge can carry none. Put shared context in `brief` once rather than pasting it into every node's prompt. Set `managed: true` to let the run execute `agent` nodes as their dependencies settle. Example fan-in: nodes w1..w10 (executor `agent`, each with `persona`+`prompt`) plus `syn` (join `all_succeeded`), edges w1..w10 -> syn each with `binding_alias` finding_N.\n\n`launch` / `poll` / `await` — RUN a managed graph without supervising it. `launch` returns a `run_id` and drives the graph in the background: it claims each node as its dependencies settle, spawns the agent the node declared, hands it the brief plus its bound inputs, and settles the result, wave after wave. You do NOT spawn the workers yourself and do not need to know which node comes next. `poll` (with `run_id`) reports per-node status without blocking; `await` blocks and returns the final report. Manual nodes are never claimed by a run, so you can mix nodes you do yourself into the same graph.\n\nModes: create, init, list, view, add, plan, launch, poll, await, update, move, start, complete, fail, block, unblock, cancel, retry, set_active, close, connect, disconnect, configure, annotate, quality_digest.\n\n`start` claims the node for YOU. `delegate` with `task_id` claims (or reassigns) the node for the CHILD. Bound workers: `view` includes `your_assignment`; `start` with no key starts that node. One owner per node.\nRequired scopes: work_read for every call; work_write for mutations.",
            |args: TaskArgs, ctx: ToolContext| Box::pin(async move { task(args, ctx).await }),
        )
        .with_required_scopes([WORK_READ_SCOPE]),
    );
    registry
}

async fn task(args: TaskArgs, ctx: ToolContext) -> Result<String, ToolError> {
    let session = ctx
        .session
        .clone()
        .ok_or_else(|| ToolError::Failed("task requires a session".into()))?;
    match args.mode.clone() {
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
                    json!({
                        "node_id": node.id.to_string(),
                        "key": node.key,
                        "title": node.title,
                        "status": node.status,
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
            let result = session
                .mutate_work(move |state| {
                    let mut graph = WorkGraph::new(title, Some(owner.clone()), GraphMode::Advisory);
                    graph.objective = args.objective;
                    for (index, title) in items.into_iter().enumerate() {
                        let node = crate::work::WorkNode::new(format!("item-{}", index + 1), title);
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
            let result = session
                .mutate_work(move |state| {
                    let mut graph = WorkGraph::new(title, Some(owner.clone()), GraphMode::Advisory);
                    graph.objective = args.objective;
                    for (index, title) in items.into_iter().enumerate() {
                        let node = crate::work::WorkNode::new(format!("item-{}", index + 1), title);
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
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            {
                let state = session.work.read().unwrap();
                let graph = state
                    .graph(gid)
                    .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
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
            let join = tokio::spawn(crate::work::drive_run_observed(
                session.clone(),
                gid,
                run_id.clone(),
                launcher,
                limits,
                cancellation.clone(),
            ));
            session
                .register_run(
                    run_id.clone(),
                    crate::session::RunHandle {
                        graph_id: gid,
                        cancellation,
                        join,
                    },
                )
                .await;
            Ok(format!(
                "run_id={run_id} graph_id={gid} (running in the background; `poll` for progress, `await` for the report)"
            ))
        }
        Mode::Poll => {
            let run_id = args
                .run_id
                .as_deref()
                .ok_or_else(|| ToolError::InvalidArguments("poll requires 'run_id'".into()))?;
            let (gid, finished) = session
                .poll_run(run_id)
                .await
                .ok_or_else(|| ToolError::Failed(format!("unknown run_id: {run_id}")))?;
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
                "status": if finished { "finished" } else { "running" },
                "revision": graph.revision,
                "nodes": nodes,
            })
            .to_string())
        }
        Mode::Await => {
            let run_id = args
                .run_id
                .as_deref()
                .ok_or_else(|| ToolError::InvalidArguments("await requires 'run_id'".into()))?;
            let report = session.wait_run(run_id).await.map_err(ToolError::Failed)?;
            Ok(json!({
                "run_id": run_id,
                "graph_id": report.graph_id.to_string(),
                "conclusion": format!("{:?}", report.conclusion),
                "nodes": report
                    .outcomes
                    .iter()
                    .map(|o| json!({
                        "key": o.node_key,
                        "status": o.status,
                        "summary": o.summary,
                    }))
                    .collect::<Vec<_>>(),
            })
            .to_string())
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
        Mode::Close => {
            require_write(&ctx)?;
            let gid = resolve_graph_id(&session, &ctx.agent_id, &args.graph_id)?;
            let cancelled = matches!(args.outcome.as_deref(), Some("cancelled"));
            let expected = args.expected_revision.ok_or_else(|| {
                ToolError::InvalidArguments("mutations require 'expected_revision'".into())
            })?;
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
            let expected = args.expected_revision.ok_or_else(|| {
                ToolError::InvalidArguments("mutations require 'expected_revision'".into())
            })?;
            let assigned = assigned_node(&session, &ctx.agent_id);
            let detail = session
                .mutate_work(move |state| {
                    let graph = state.graph(gid)?.clone();
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
                            let verification = parse_verification(&args.verification)
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
                                    description: args.description.clone(),
                                });
                            }
                            for title in args.items.iter().cloned() {
                                inputs.push(crate::work::NodeInput {
                                    key: alloc_key(None),
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
                            let verification = parse_verification(&args.verification)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?
                                .unwrap_or_default();
                            let batch: Vec<(NodeId, String)> =
                                targets.iter().map(|id| (*id, summary.clone())).collect();
                            let results = state.complete_many(
                                gid,
                                expected,
                                &context,
                                batch,
                                args.evidence.clone(),
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
                                        Some(args.description),
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
                                    let outcome = match args.outcome.as_deref() {
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
                                        args.evidence,
                                        Vec::new(),
                                        parse_verification(&args.verification)
                                            .map_err(|e| {
                                                crate::work::WorkError::InvalidGraph(e.to_string())
                                            })?
                                            .unwrap_or_default(),
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
