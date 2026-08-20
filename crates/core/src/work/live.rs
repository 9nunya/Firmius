//! A live, structure-aware projection of one work graph.
//!
//! [`MiniProjection`] answers "what are the next few things on the list",
//! which is right for a checklist and wrong for a run: it is flat, capped at
//! a handful of rows, and has no notion of which nodes feed which. Ten
//! workers executing in parallel collapse into "4 rows and an overflow
//! count", which is exactly the moment the user most wants to see what is
//! happening.
//!
//! This module derives the shape instead. Nodes are grouped into stages by
//! dependency depth, so a fan-out renders as one stage of ten rather than
//! ten unrelated rows, and a pipeline renders as one row per step. Depth is
//! computed purely from dependency edges, so nothing has to be authored for
//! the view to be useful, and a plain checklist (no edges at all) is simply
//! a single stage, which keeps the todo path and the run path on one code
//! path rather than two that drift.
//!
//! Everything here is pure and derived from canonical state. The renderer
//! never accumulates its own idea of progress, so a projection taken after a
//! restart, or by a second observer, is identical to the live one.

use super::ids::NodeId;
use super::model::*;
use super::readiness::evaluate_readiness;
use std::collections::BTreeMap;

/// How a node is doing right now, from a viewer's perspective.
///
/// This is deliberately coarser than [`ExecutionStatus`]: a viewer cares
/// that something is waiting, not whether it is `Pending` versus derived
/// `Ready`, and cares that something is stuck, not the exact terminal
/// variant. Keeping the display vocabulary separate means adding an
/// execution state never silently changes what the UI paints.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LiveState {
    /// Waiting on predecessors (or on a person, for a manual node).
    Waiting,
    /// Claimed and executing right now.
    Running,
    Succeeded,
    Failed,
    /// Cannot proceed: blocked, cancelled, or its join can no longer be met.
    Stuck,
}

impl LiveState {
    fn of(node: &WorkNode, blocked: bool) -> Self {
        match node.status {
            ExecutionStatus::Running => Self::Running,
            ExecutionStatus::Succeeded => Self::Succeeded,
            ExecutionStatus::Failed | ExecutionStatus::Interrupted => Self::Failed,
            ExecutionStatus::Blocked | ExecutionStatus::Cancelled => Self::Stuck,
            ExecutionStatus::Skipped => Self::Succeeded,
            _ if blocked => Self::Stuck,
            _ => Self::Waiting,
        }
    }
}

/// One node, as a viewer sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LiveNode {
    pub node_id: NodeId,
    pub key: String,
    pub title: String,
    pub state: LiveState,
    /// Which attempt this is. Shows a retry loop making progress rather
    /// than appearing to redo the same work for no reason.
    pub attempt: u32,
    pub max_attempts: Option<u32>,
    /// The agent currently working this node, when it is running.
    pub agent_id: Option<String>,
    /// Why this node is not moving. Populated for waiting and stuck nodes,
    /// because "waiting" without "on what" is the least useful thing a
    /// progress view can say.
    pub detail: Option<String>,
    /// Settlement summary, once there is one.
    pub summary: Option<String>,
}

/// A set of nodes at the same dependency depth.
///
/// Depth, not authored order: nodes that can run concurrently belong
/// together regardless of how they were written down, which is what lets a
/// ten-way fan-out render as a single legible row.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LiveStage {
    pub depth: usize,
    pub nodes: Vec<LiveNode>,
}

impl LiveStage {
    pub fn counts(&self) -> StageCounts {
        let mut counts = StageCounts::default();
        for node in &self.nodes {
            match node.state {
                LiveState::Waiting => counts.waiting += 1,
                LiveState::Running => counts.running += 1,
                LiveState::Succeeded => counts.succeeded += 1,
                LiveState::Failed => counts.failed += 1,
                LiveState::Stuck => counts.stuck += 1,
            }
        }
        counts
    }
    /// A one-line label for the stage: the shared work it represents.
    /// A single-node stage names that node; a wider one describes the shape.
    pub fn label(&self) -> String {
        match self.nodes.len() {
            0 => String::new(),
            1 => self.nodes[0].title.clone(),
            n => format!("{n} parallel tasks"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct StageCounts {
    pub waiting: usize,
    pub running: usize,
    pub succeeded: usize,
    pub failed: usize,
    pub stuck: usize,
}

impl StageCounts {
    pub fn total(&self) -> usize {
        self.waiting + self.running + self.succeeded + self.failed + self.stuck
    }
    pub fn settled(&self) -> usize {
        self.succeeded + self.failed + self.stuck
    }
}

/// The whole graph, ready to render.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LiveGraph {
    pub title: String,
    pub stages: Vec<LiveStage>,
    /// True when this graph has structure worth drawing as stages. A plain
    /// checklist renders as a simple list instead, so the run view never
    /// makes a todo list look like something it is not.
    pub structured: bool,
}

impl LiveGraph {
    pub fn counts(&self) -> StageCounts {
        let mut total = StageCounts::default();
        for stage in &self.stages {
            let counts = stage.counts();
            total.waiting += counts.waiting;
            total.running += counts.running;
            total.succeeded += counts.succeeded;
            total.failed += counts.failed;
            total.stuck += counts.stuck;
        }
        total
    }

    /// Nodes executing right now, across every stage. The natural place for
    /// a viewer's attention, and what per-node live activity attaches to.
    pub fn running(&self) -> impl Iterator<Item = &LiveNode> {
        self.stages
            .iter()
            .flat_map(|stage| stage.nodes.iter())
            .filter(|node| node.state == LiveState::Running)
    }

    pub fn is_finished(&self) -> bool {
        let counts = self.counts();
        counts.running == 0 && counts.waiting == 0
    }
}

/// Longest dependency path to each node, which is the earliest wave it can
/// possibly execute in.
///
/// Longest rather than shortest: a node must wait for its slowest chain, so
/// the longest path is the wave it actually runs in, and grouping by it puts
/// genuinely concurrent work on the same row. Feedback edges are excluded —
/// they point backwards by design, and counting them would collapse a
/// gate-and-retry graph into meaningless depths.
fn depths(graph: &WorkGraph) -> BTreeMap<NodeId, usize> {
    let mut incoming: BTreeMap<NodeId, Vec<NodeId>> = BTreeMap::new();
    for edge in graph.edges.values() {
        if edge.kind == EdgeKind::Feedback {
            continue;
        }
        incoming.entry(edge.to).or_default().push(edge.from);
    }

    let mut depths: BTreeMap<NodeId, usize> = BTreeMap::new();
    // Iterate to a fixed point. The dependency subgraph is acyclic (managed
    // graphs validate this), so this converges in at most one pass per node,
    // and the bound also protects an advisory graph that is allowed cycles.
    for _ in 0..graph.nodes.len().max(1) {
        let mut changed = false;
        for id in graph.nodes.keys() {
            let depth = incoming
                .get(id)
                .map(|parents| {
                    parents
                        .iter()
                        // A parent not yet visited is depth 0 so far, which
                        // still puts this node at least one wave later.
                        .map(|parent| depths.get(parent).copied().unwrap_or(0) + 1)
                        .max()
                        .unwrap_or(0)
                })
                .unwrap_or(0);
            if depths.get(id).copied().unwrap_or(0) != depth {
                depths.insert(*id, depth);
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    for id in graph.nodes.keys() {
        depths.entry(*id).or_insert(0);
    }
    depths
}

/// Explain why a node is not moving, in the terms a viewer thinks in.
fn waiting_detail(
    graph: &WorkGraph,
    node: &WorkNode,
    blocked_reason: Option<&str>,
) -> Option<String> {
    if let Some(reason) = blocked_reason {
        return Some(reason.to_string());
    }
    if node.status != ExecutionStatus::Pending {
        return None;
    }
    // Name the predecessors that have not settled yet. This is the single
    // most useful thing to show next to a waiting node, and it is exactly
    // what the join policy is waiting on.
    let pending: Vec<&str> = graph
        .edges
        .values()
        .filter(|edge| edge.kind != EdgeKind::Feedback && edge.to == node.id)
        .filter_map(|edge| graph.nodes.get(&edge.from))
        .filter(|predecessor| {
            !matches!(
                predecessor.status,
                ExecutionStatus::Succeeded
                    | ExecutionStatus::Failed
                    | ExecutionStatus::Blocked
                    | ExecutionStatus::Cancelled
                    | ExecutionStatus::Skipped
                    | ExecutionStatus::Interrupted
            )
        })
        .map(|predecessor| predecessor.key.as_str())
        .collect();
    match pending.len() {
        0 => {
            if node.executor == Executor::Manual && !graph.edges.is_empty() {
                Some("ready for you".into())
            } else {
                None
            }
        }
        1 => Some(format!("waiting on {}", pending[0])),
        n => Some(format!("waiting on {n} tasks")),
    }
}

/// Project `graph` into its live, structure-aware form.
pub fn project(graph: &WorkGraph) -> LiveGraph {
    let depths = depths(graph);
    let readiness = evaluate_readiness(graph);
    let blocked: BTreeMap<NodeId, String> = readiness.blocked.into_iter().collect();

    let mut by_depth: BTreeMap<usize, Vec<LiveNode>> = BTreeMap::new();
    // Walk authored order so nodes within a stage keep a stable, predictable
    // order rather than shuffling as ids change.
    for node_id in &graph.view_order {
        let Some(node) = graph.nodes.get(node_id) else {
            continue;
        };
        let blocked_reason = blocked.get(node_id).map(|s| s.as_str());
        let latest = node
            .attempt_ids
            .last()
            .and_then(|id| graph.attempts.get(id));
        let live = LiveNode {
            node_id: *node_id,
            key: node.key.clone(),
            title: node.title.clone(),
            state: LiveState::of(node, blocked_reason.is_some()),
            attempt: node.attempt_ids.len() as u32,
            max_attempts: (node.retry_policy.max_attempts > 0)
                .then_some(node.retry_policy.max_attempts),
            agent_id: graph
                .assignments
                .values()
                .find(|a| a.node_id == *node_id && a.released_at.is_none())
                .map(|a| a.agent_id.clone())
                .or_else(|| {
                    (node.status == ExecutionStatus::Running)
                        .then(|| latest.and_then(|a| a.agent_id.clone()))
                        .flatten()
                }),
            detail: waiting_detail(graph, node, blocked_reason),
            summary: latest
                .and_then(|a| a.result_id)
                .and_then(|id| graph.results.get(&id))
                .map(|r| r.summary.clone()),
        };
        by_depth
            .entry(depths.get(node_id).copied().unwrap_or(0))
            .or_default()
            .push(live);
    }

    let stages: Vec<LiveStage> = by_depth
        .into_iter()
        .map(|(depth, nodes)| LiveStage { depth, nodes })
        .collect();
    // Structure means "more than one wave": that is precisely when a flat
    // list stops conveying what is happening.
    let structured = stages.len() > 1;
    LiveGraph {
        title: graph.title.clone(),
        stages,
        structured,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::work::{AuthorizationContext, PlannedEdge, PlannedNode, WorkState};

    fn auth() -> AuthorizationContext {
        AuthorizationContext {
            agent_id: "owner".into(),
            ..Default::default()
        }
    }

    fn fan_in(workers: usize) -> (WorkState, crate::work::GraphId) {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("audit", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        let mut nodes: Vec<PlannedNode> = (1..=workers)
            .map(|i| PlannedNode {
                key: format!("w{i}"),
                title: format!("audit slice {i}"),
                ..Default::default()
            })
            .collect();
        nodes.push(PlannedNode {
            key: "syn".into(),
            title: "synthesize".into(),
            join: Some(JoinPolicy::AllSucceeded),
            ..Default::default()
        });
        let edges: Vec<PlannedEdge> = (1..=workers)
            .map(|i| PlannedEdge {
                from: format!("w{i}"),
                to: "syn".into(),
                condition: EdgeCondition::Succeeded,
                binding_alias: Some(format!("finding_{i}")),
                ..Default::default()
            })
            .collect();
        state
            .plan(graph_id, 0, &auth(), nodes, edges, None, None)
            .unwrap();
        (state, graph_id)
    }

    /// The case the flat checklist cannot express: ten concurrent workers
    /// are ONE stage, not ten unrelated rows, and their successor is a
    /// second stage that is visibly waiting on them.
    #[test]
    fn a_fan_in_projects_as_two_stages() {
        let (state, graph_id) = fan_in(10);
        let live = project(state.graph(graph_id).unwrap());
        assert!(live.structured);
        assert_eq!(live.stages.len(), 2);
        assert_eq!(live.stages[0].nodes.len(), 10);
        assert_eq!(live.stages[1].nodes.len(), 1);
        assert_eq!(live.stages[0].label(), "10 parallel tasks");
        assert_eq!(live.stages[1].label(), "synthesize");
        assert_eq!(
            live.stages[1].nodes[0].detail.as_deref(),
            Some("waiting on 10 tasks"),
            "a waiting node must say what it is waiting for"
        );
    }

    /// A plain checklist has no structure to draw, and must not be dressed
    /// up as a pipeline.
    #[test]
    fn a_flat_checklist_is_a_single_unstructured_stage() {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("todo", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        state
            .plan(
                graph_id,
                0,
                &auth(),
                (1..=3)
                    .map(|i| PlannedNode {
                        key: format!("item-{i}"),
                        title: format!("thing {i}"),
                        ..Default::default()
                    })
                    .collect(),
                Vec::new(),
                None,
                None,
            )
            .unwrap();
        let live = project(state.graph(graph_id).unwrap());
        assert!(!live.structured, "a checklist must not render as stages");
        assert_eq!(live.stages.len(), 1);
        assert_eq!(live.stages[0].nodes.len(), 3);
    }

    /// Progress and attention: counts move as work lands, and the running
    /// node is directly reachable for live activity.
    #[test]
    fn counts_and_running_nodes_track_execution() {
        let (mut state, graph_id) = fan_in(3);
        let ids: Vec<NodeId> = state.graph(graph_id).unwrap().view_order.clone();

        let revision = state.graph(graph_id).unwrap().revision;
        state
            .assign(graph_id, revision, &auth(), ids[0], "worker-a", None, None)
            .unwrap();
        let live = project(state.graph(graph_id).unwrap());
        let counts = live.counts();
        assert_eq!(counts.running, 1);
        assert_eq!(counts.waiting, 3);
        let running: Vec<&LiveNode> = live.running().collect();
        assert_eq!(running.len(), 1);
        assert_eq!(
            running[0].agent_id.as_deref(),
            Some("worker-a"),
            "a running node must expose its agent so live activity can attach"
        );
        assert_eq!(running[0].attempt, 1);

        // Settle it; the stage's progress advances.
        let revision = state.graph(graph_id).unwrap().revision;
        let assignment = state
            .graph(graph_id)
            .unwrap()
            .assignments
            .values()
            .next()
            .unwrap()
            .id;
        state
            .settle_assignment(
                graph_id,
                revision,
                &AuthorizationContext {
                    agent_id: "worker-a".into(),
                    assignment_ids: [assignment].into_iter().collect(),
                    ..Default::default()
                },
                assignment,
                ExecutionStatus::Succeeded,
                Some(Outcome::Success),
                "found an issue",
                None,
                Vec::new(),
                Vec::new(),
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        let live = project(state.graph(graph_id).unwrap());
        assert_eq!(live.stages[0].counts().succeeded, 1);
        assert_eq!(
            live.stages[0].nodes[0].summary.as_deref(),
            Some("found an issue")
        );
    }

    /// A retry loop must look like progress, not like the same work being
    /// mysteriously redone, so attempt count and cap are both surfaced.
    #[test]
    fn attempts_are_surfaced_for_retry_loops() {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("loop", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        state
            .plan(
                graph_id,
                0,
                &auth(),
                vec![
                    PlannedNode {
                        key: "code".into(),
                        title: "implement".into(),
                        max_attempts: Some(3),
                        ..Default::default()
                    },
                    PlannedNode {
                        key: "gate".into(),
                        title: "review".into(),
                        ..Default::default()
                    },
                ],
                vec![
                    PlannedEdge {
                        from: "code".into(),
                        to: "gate".into(),
                        condition: EdgeCondition::Succeeded,
                        ..Default::default()
                    },
                    PlannedEdge {
                        from: "gate".into(),
                        to: "code".into(),
                        kind: EdgeKind::Feedback,
                        condition: EdgeCondition::Outcome,
                        on_outcome: Some(Outcome::Custom("rejected".into())),
                        required: false,
                        ..Default::default()
                    },
                ],
                None,
                None,
            )
            .unwrap();

        // The feedback edge must not distort the stage layout: this is
        // still a two-stage pipeline, not a cycle collapsed into one.
        let live = project(state.graph(graph_id).unwrap());
        assert_eq!(live.stages.len(), 2);

        let code_id = state.graph(graph_id).unwrap().view_order[0];
        let revision = state.graph(graph_id).unwrap().revision;
        state
            .start(graph_id, revision, &auth(), code_id, None)
            .unwrap();
        let revision = state.graph(graph_id).unwrap().revision;
        state
            .complete(
                graph_id,
                revision,
                &auth(),
                code_id,
                "v1",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        let live = project(state.graph(graph_id).unwrap());
        assert_eq!(live.stages[0].nodes[0].attempt, 1);
        assert_eq!(live.stages[0].nodes[0].max_attempts, Some(3));
    }

    /// A node whose join can no longer be satisfied reads as stuck, with
    /// the reason, rather than waiting forever with no explanation.
    #[test]
    fn an_unsatisfiable_join_reads_as_stuck_with_a_reason() {
        let (mut state, graph_id) = fan_in(2);
        let ids: Vec<NodeId> = state.graph(graph_id).unwrap().view_order.clone();
        for id in [ids[0], ids[1]] {
            let revision = state.graph(graph_id).unwrap().revision;
            state.start(graph_id, revision, &auth(), id, None).unwrap();
        }
        let revision = state.graph(graph_id).unwrap().revision;
        state
            .fail(
                graph_id,
                revision,
                &auth(),
                ids[0],
                Outcome::Failure,
                "broke",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        let revision = state.graph(graph_id).unwrap().revision;
        state
            .complete(
                graph_id,
                revision,
                &auth(),
                ids[1],
                "fine",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();

        let live = project(state.graph(graph_id).unwrap());
        let syn = &live.stages[1].nodes[0];
        assert_eq!(syn.state, LiveState::Stuck);
        assert!(
            syn.detail.is_some(),
            "a stuck node must explain why it can never run"
        );
    }
}
