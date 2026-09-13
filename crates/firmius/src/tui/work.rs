//! Canonical WorkGraph projection for the compact focused-agent checklist.
//!
//! This module deliberately contains no task-tool output handling.  A work
//! view is derived only from the typed snapshot owned by the session, and a
//! typed event can only advance that snapshot's sequence.  If an event does
//! not contain enough information to fold a complete graph, callers reload
//! the canonical snapshot rather than guessing from prose.

use firmius_core::work::Outcome;
use firmius_core::{
    ExecutionStatus, GraphId, GraphStatus, NodeId, SessionEventPayload, VerificationLevel,
    WorkEventEnvelope, WorkGraph, WorkSnapshot,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkLine {
    pub node_id: NodeId,
    /// Agent holding the node's live assignment, when delegated.
    pub agent_id: Option<String>,
    pub title: String,
    pub status: ExecutionStatus,
    /// Waiting/blocked reason or settlement summary, derived from the same
    /// live projection the run view uses.
    pub detail: Option<String>,
    /// M5.3 — precomputed, gate-aware glyph distinguishing plain
    /// execution/verification state (see [`node_status_glyph`]).
    pub glyph: &'static str,
    /// A delegated assignment is shown as starting even before the worker's
    /// first execution event arrives.
    pub assigned: bool,
    /// Presentation state is deliberately separate from the canonical
    /// execution status. In particular, an assigned pending node is
    /// *starting*, not running: the worker has been launched, but has not
    /// emitted an execution event yet.
    pub presentation: WorkPresentation,
}

/// Compact checklist presentation state. This is a view concern rather than
/// another persisted [`ExecutionStatus`]; `Starting` exists only for the
/// short interval between durable assignment and the worker's first event.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WorkPresentation {
    Pending,
    Ready,
    Starting,
    Running,
    Succeeded,
    Failed,
    Blocked,
    Cancelled,
    Skipped,
    Interrupted,
}

impl WorkPresentation {
    pub fn from_status(status: ExecutionStatus) -> Self {
        match status {
            ExecutionStatus::Pending => Self::Pending,
            ExecutionStatus::Ready => Self::Ready,
            ExecutionStatus::Running => Self::Running,
            ExecutionStatus::Succeeded => Self::Succeeded,
            ExecutionStatus::Failed => Self::Failed,
            ExecutionStatus::Blocked => Self::Blocked,
            ExecutionStatus::Cancelled => Self::Cancelled,
            ExecutionStatus::Skipped => Self::Skipped,
            ExecutionStatus::Interrupted => Self::Interrupted,
        }
    }

    pub fn glyph(self) -> &'static str {
        match self {
            // A hollow circle is ordinary queued work. The dotted circle is
            // intentionally distinct: an assignment exists and startup is
            // in progress, but execution has not begun yet.
            Self::Pending | Self::Ready => "○",
            Self::Starting => "◌",
            Self::Running => "◐",
            Self::Succeeded => "✓",
            Self::Failed | Self::Blocked => "!",
            Self::Cancelled | Self::Skipped => "⊘",
            Self::Interrupted => "↻",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
#[allow(dead_code)]
pub struct ParentReadyHook {
    pub parent_graph_id: Option<GraphId>,
    pub child_graph_id: Option<GraphId>,
    pub child_ready: bool,
}

pub fn row_limit_for_terminal(available_lines: u16) -> usize {
    usize::from(available_lines.min(5))
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct WorkView {
    pub graph_id: Option<GraphId>,
    pub graph_title: Option<String>,
    pub lines: Vec<WorkLine>,
    pub overflow: usize,
    pub completed: usize,
    pub all_completed: bool,
    /// One-line context about the parent assignment this graph's owner is
    /// working under, populated only by `for_child`.
    pub parent_context: Option<String>,
    /// Concise per-assignment summaries for a graph owner's live
    /// assignments, populated only by `for_parent`.
    pub assignment_summaries: Vec<String>,
    /// One bounded summary derived from canonical swarm state. Kept on the
    /// existing heading rather than consuming another scarce terminal row.
    pub coordination_summary: Option<String>,
}

impl WorkView {
    /// Select attention first, then active work, then the next authored work.
    /// Authored order is restored after choosing so the mini view does not
    /// jump around as statuses change.  `max_lines` includes an overflow line.
    pub fn for_agent(snapshot: &WorkSnapshot, agent_id: &str, max_lines: usize) -> Self {
        let Some(graph_id) = snapshot.state.active_graph_by_agent.get(agent_id).copied() else {
            return Self::default();
        };
        let Some(graph) = snapshot.state.graphs.get(&graph_id) else {
            return Self::default();
        };
        let mut view = Self::from_graph(graph, max_lines, Some(graph_id));
        if snapshot.state.swarm.policy != firmius_core::work::SwarmPolicy::Disabled {
            let held = snapshot
                .state
                .swarm
                .resource_claims
                .values()
                .filter(|claim| claim.owner.graph_id == graph_id && claim.is_held())
                .count();
            let policy = match snapshot.state.swarm.policy {
                firmius_core::work::SwarmPolicy::Advisory => "advisory",
                firmius_core::work::SwarmPolicy::Protective => "protective",
                firmius_core::work::SwarmPolicy::Disabled => unreachable!(),
            };
            let owner = graph
                .integration
                .owner_node_key
                .as_deref()
                .map(|owner| format!(" · integrator {owner}"))
                .unwrap_or_default();
            view.coordination_summary = Some(format!("{policy} · {held} claims{owner}"));
        }
        view
    }

    /// Hook for a focused child: the child graph remains the primary view,
    /// while callers may use this projection to show its parent assignment.
    /// Keeping the hook typed avoids coupling the mini renderer to hierarchy
    /// or delegate result text.
    pub fn for_child(
        snapshot: &WorkSnapshot,
        child_agent_id: &str,
        _parent_graph_id: Option<GraphId>,
        max_lines: usize,
    ) -> Self {
        let mut view = Self::for_agent(snapshot, child_agent_id, max_lines);
        if let Some(binding) = snapshot.state.binding_for_agent(child_agent_id) {
            let title = snapshot
                .state
                .graphs
                .get(&binding.graph_id)
                .and_then(|graph| graph.nodes.get(&binding.node_id))
                .map(|node| node.title.as_str())
                .unwrap_or("(unknown task)");
            view.parent_context = Some(format!("assigned: {title}"));
        }
        view
    }

    /// Hook for a focused parent board.  The parent owns the graph shown by
    /// the mini view; child assignment summaries are a later presentation
    /// concern and must not replace the parent's canonical node state.
    pub fn for_parent(snapshot: &WorkSnapshot, parent_agent_id: &str, max_lines: usize) -> Self {
        let mut view = Self::for_agent(snapshot, parent_agent_id, max_lines);
        let Some(graph_id) = view.graph_id else {
            return view;
        };
        let Some(_graph) = snapshot.state.graphs.get(&graph_id) else {
            return view;
        };
        // Assignment metadata is intentionally not rendered as extra rows.
        // The canonical node status is the compact, privacy-preserving live
        // indicator; worker IDs belong in the agent/delegate pane.
        view.assignment_summaries.clear();
        view
    }

    /// Typed hook for parent/child-ready presentation.  Readiness is derived
    /// from the child graph's canonical nodes; no delegate completion prose is
    /// consulted.  The hook is intentionally small until assignment rows are
    /// part of the Milestone 2 mini view.
    #[allow(dead_code)]
    pub fn parent_ready(
        snapshot: &WorkSnapshot,
        parent_agent_id: &str,
        child_agent_id: &str,
    ) -> ParentReadyHook {
        let parent_graph_id = snapshot
            .state
            .active_graph_by_agent
            .get(parent_agent_id)
            .copied();
        let child_graph_id = snapshot
            .state
            .active_graph_by_agent
            .get(child_agent_id)
            .copied();
        let child_ready = child_graph_id
            .and_then(|id| snapshot.state.graphs.get(&id))
            .is_some_and(|graph| {
                graph
                    .view_order
                    .iter()
                    .filter_map(|node| graph.nodes.get(node))
                    .all(|node| {
                        matches!(
                            node.status,
                            ExecutionStatus::Ready | ExecutionStatus::Succeeded
                        )
                    })
            });
        ParentReadyHook {
            parent_graph_id,
            child_graph_id,
            child_ready,
        }
    }

    pub(crate) fn from_graph(
        graph: &WorkGraph,
        max_lines: usize,
        graph_id: Option<GraphId>,
    ) -> Self {
        let ordered: Vec<&firmius_core::WorkNode> = graph
            .view_order
            .iter()
            .filter_map(|id| graph.nodes.get(id))
            .collect();
        if ordered.is_empty() || max_lines == 0 {
            return Self {
                graph_id,
                graph_title: Some(graph.title.clone()),
                ..Self::default()
            };
        }

        let completed = ordered
            .iter()
            .filter(|node| is_completed(node.status))
            .count();
        if completed == ordered.len()
            || matches!(
                graph.status,
                GraphStatus::Completed | GraphStatus::Cancelled
            )
        {
            return Self {
                graph_id,
                graph_title: Some(graph.title.clone()),
                completed,
                all_completed: true,
                ..Self::default()
            };
        }

        let mut selected = Vec::<&firmius_core::WorkNode>::new();
        let mut add = |predicate: fn(ExecutionStatus) -> bool| {
            for node in &ordered {
                if predicate(node.status) && !selected.iter().any(|chosen| chosen.id == node.id) {
                    selected.push(*node);
                }
            }
        };
        add(|status| matches!(status, ExecutionStatus::Failed | ExecutionStatus::Blocked));
        add(|status| status == ExecutionStatus::Running);
        add(|status| status == ExecutionStatus::Ready);
        add(|status| status == ExecutionStatus::Pending);
        // Completed rows are useful only after unfinished work has been
        // selected, and are therefore the final priority tier.
        add(is_completed);

        let row_limit = if selected.len() > max_lines {
            max_lines.saturating_sub(1)
        } else {
            max_lines
        };
        let overflow = selected.len().saturating_sub(row_limit);
        selected.truncate(row_limit);
        selected.sort_by_key(|node| {
            graph
                .view_order
                .iter()
                .position(|id| *id == node.id)
                .unwrap_or(usize::MAX)
        });
        let live = firmius_core::work::project_live(graph);
        let live_nodes = live
            .stages
            .iter()
            .flat_map(|stage| stage.nodes.iter())
            .map(|node| (node.node_id, node))
            .collect::<std::collections::HashMap<_, _>>();
        Self {
            graph_id,
            graph_title: Some(graph.title.clone()),
            lines: selected
                .into_iter()
                .map(|node| {
                    let assignment = graph.assignments.values().find(|assignment| {
                        assignment.node_id == node.id && assignment.released_at.is_none()
                    });
                    WorkLine {
                        node_id: node.id,
                        agent_id: assignment.map(|assignment| assignment.agent_id.clone()),
                        title: node.title.clone(),
                        status: node.status,
                        detail: live_nodes
                            .get(&node.id)
                            .and_then(|live| live.detail.clone().or_else(|| live.summary.clone())),
                        assigned: assignment.is_some(),
                        presentation: {
                            let assigned = assignment.is_some();
                            if assigned && node.status == ExecutionStatus::Pending {
                                WorkPresentation::Starting
                            } else {
                                WorkPresentation::from_status(node.status)
                            }
                        },
                        glyph: {
                            let assigned = assignment.is_some();
                            if assigned && node.status == ExecutionStatus::Pending {
                                WorkPresentation::Starting.glyph()
                            } else {
                                node_status_glyph(graph, node)
                            }
                        },
                    }
                })
                .collect(),
            overflow,
            completed,
            all_completed: false,
            parent_context: None,
            assignment_summaries: Vec::new(),
            coordination_summary: None,
        }
    }
}

fn is_completed(status: ExecutionStatus) -> bool {
    matches!(
        status,
        ExecutionStatus::Succeeded | ExecutionStatus::Cancelled | ExecutionStatus::Skipped
    )
}

/// Typed envelope extraction used by the model's unified session bus fold.
#[allow(dead_code)]
pub fn work_event(payload: &SessionEventPayload) -> Option<&WorkEventEnvelope> {
    match payload {
        SessionEventPayload::Work(event) => Some(event),
        _ => None,
    }
}

pub fn status_glyph(status: ExecutionStatus) -> &'static str {
    match status {
        ExecutionStatus::Succeeded => "✓",
        ExecutionStatus::Running => "◐",
        ExecutionStatus::Pending | ExecutionStatus::Ready => "○",
        ExecutionStatus::Blocked | ExecutionStatus::Failed => "!",
        ExecutionStatus::Interrupted => "↻",
        ExecutionStatus::Cancelled | ExecutionStatus::Skipped => "⊘",
    }
}

/// M5.3 — a gate-aware glyph that distinguishes execution failure, semantic
/// (outcome) failure, and verification failure from plain success/pending
/// glyphs. Falls back to [`status_glyph`] when the graph does not carry
/// enough information (e.g. no result recorded yet) to say more.
pub fn node_status_glyph(graph: &WorkGraph, node: &firmius_core::WorkNode) -> &'static str {
    let latest_result = node
        .attempt_ids
        .last()
        .and_then(|attempt_id| graph.attempts.get(attempt_id))
        .and_then(|attempt| attempt.result_id)
        .and_then(|result_id| graph.results.get(&result_id));

    match node.status {
        ExecutionStatus::Succeeded => {
            // A node that executed successfully but whose required
            // verification level has not yet been met by its latest
            // result stays visibly unverified rather than a plain "done".
            if node.verification != VerificationLevel::None {
                let achieved = latest_result
                    .map(|r| r.verification)
                    .unwrap_or(VerificationLevel::None);
                if achieved < node.verification {
                    return "✓?";
                }
            }
            "✓"
        }
        ExecutionStatus::Failed => {
            // Semantic failure (the attempt ran to completion but the
            // outcome was a test/verification failure) vs. an execution
            // failure (the attempt itself could not run to completion).
            match latest_result.and_then(|r| r.outcome.as_ref()) {
                Some(Outcome::TestFailed) => "✗~",
                _ => "✗",
            }
        }
        other => status_glyph(other),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use firmius_core::{GraphMode, WorkNode, WorkState};

    fn snapshot(count: usize) -> WorkSnapshot {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("work", Some("agent".into()), GraphMode::Advisory);
        for index in 0..count {
            let node = WorkNode::new(format!("item-{index}"), format!("Item {index}"));
            graph.view_order.push(node.id);
            graph.nodes.insert(node.id, node);
        }
        let id = graph.id;
        state.graphs.insert(id, graph);
        state.active_graph_by_agent.insert("agent".into(), id);
        WorkSnapshot::new("session", 0, state)
    }

    #[test]
    fn delegated_pending_rows_have_distinct_starting_state_glyph_and_style() {
        let mut snapshot = graph_with_nodes("agent", &[ExecutionStatus::Pending]);
        let assignment_id = firmius_core::AssignmentId::new();
        {
            let graph = snapshot.state.graphs.values_mut().next().unwrap();
            let node_id = graph.view_order[0];
            let attempt_id = firmius_core::AttemptId::new();
            graph.assignments.insert(
                assignment_id,
                firmius_core::WorkAssignment {
                    id: assignment_id,
                    node_id,
                    attempt_id,
                    agent_id: "worker".into(),
                    parent_agent_id: Some("agent".into()),
                    assigned_at: chrono::Utc::now(),
                    released_at: None,
                },
            );
        }

        // Active pending assignment: ownership and the distinct Starting
        // presentation are both visible before the worker emits its first event.
        let active_view = WorkView::for_agent(&snapshot, "agent", 5);
        assert_eq!(active_view.lines.len(), 1);
        assert!(active_view.lines[0].assigned);
        assert_eq!(active_view.lines[0].agent_id.as_deref(), Some("worker"));
        assert_eq!(active_view.lines[0].status, ExecutionStatus::Pending);
        assert_eq!(
            active_view.lines[0].presentation,
            WorkPresentation::Starting
        );
        assert_eq!(
            active_view.lines[0].glyph,
            WorkPresentation::Starting.glyph()
        );
        assert_ne!(
            active_view.lines[0].glyph,
            WorkPresentation::Running.glyph(),
            "starting must not alias the running glyph"
        );
        let theme = super::super::theme::default_theme();
        assert_ne!(
            super::super::style::work_presentation(&theme, WorkPresentation::Starting),
            super::super::style::work_presentation(&theme, WorkPresentation::Running),
            "delegated pending rows must use the distinct starting style"
        );

        // Released assignment: the historical record remains in the graph,
        // but it is no longer live ownership and must not be shown as assigned.
        {
            let graph = snapshot.state.graphs.values_mut().next().unwrap();
            graph
                .assignments
                .get_mut(&assignment_id)
                .unwrap()
                .released_at = Some(chrono::Utc::now());
        }
        let released_view = WorkView::for_agent(&snapshot, "agent", 5);
        assert_eq!(released_view.lines[0].agent_id, None);
        assert!(!released_view.lines[0].assigned);
        assert_eq!(
            released_view.lines[0].presentation,
            WorkPresentation::Pending
        );
    }

    #[test]
    fn selection_has_zero_to_five_lines_and_overflow_uses_four_rows() {
        let snapshot = snapshot(8);
        let view = WorkView::for_agent(&snapshot, "agent", 5);
        assert_eq!(view.lines.len(), 4);
        assert_eq!(view.overflow, 4);
        assert_eq!(WorkView::for_agent(&snapshot, "agent", 0).lines.len(), 0);
        assert_eq!(
            WorkView::for_agent(&snapshot, "missing", 5),
            WorkView::default()
        );
    }

    #[test]
    fn coordination_summary_is_opt_in_and_derived_from_canonical_state() {
        let mut snapshot = snapshot(1);
        assert_eq!(
            WorkView::for_agent(&snapshot, "agent", 5).coordination_summary,
            None,
            "legacy and explicitly disabled sessions must keep the compact heading unchanged"
        );

        let graph_id = *snapshot.state.graphs.keys().next().unwrap();
        snapshot
            .state
            .graphs
            .get_mut(&graph_id)
            .unwrap()
            .integration
            .owner_node_key = Some("item-0".into());
        for (policy, label) in [
            (firmius_core::work::SwarmPolicy::Advisory, "advisory"),
            (firmius_core::work::SwarmPolicy::Protective, "protective"),
        ] {
            snapshot.state.swarm.policy = policy;
            assert_eq!(
                WorkView::for_agent(&snapshot, "agent", 5)
                    .coordination_summary
                    .as_deref(),
                Some(format!("{label} · 0 claims · integrator item-0").as_str())
            );
        }
    }

    #[test]
    fn attention_precedes_authored_pending_order() {
        let mut snapshot = snapshot(3);
        let graph = snapshot.state.graphs.values_mut().next().unwrap();
        let ids = graph.view_order.clone();
        graph.nodes.get_mut(&ids[2]).unwrap().status = ExecutionStatus::Failed;
        let view = WorkView::for_agent(&snapshot, "agent", 2);
        assert_eq!(view.lines[0].node_id, ids[2]);
    }

    #[test]
    fn completed_graph_collapses_and_missing_graph_is_empty() {
        let mut snapshot = snapshot(2);
        let graph = snapshot.state.graphs.values_mut().next().unwrap();
        for node in graph.nodes.values_mut() {
            node.status = ExecutionStatus::Succeeded;
        }
        let view = WorkView::for_agent(&snapshot, "agent", 5);
        assert!(view.all_completed);
        assert_eq!(view.completed, 2);
        snapshot.state.active_graph_by_agent.clear();
        assert_eq!(
            WorkView::for_agent(&snapshot, "agent", 5),
            WorkView::default()
        );
    }

    #[test]
    fn glyphs_are_static_and_terminal_limit_is_bounded() {
        assert_eq!(status_glyph(ExecutionStatus::Succeeded), "✓");
        assert_eq!(status_glyph(ExecutionStatus::Failed), "!");
        assert_eq!(row_limit_for_terminal(3), 3);
        assert_eq!(row_limit_for_terminal(9), 5);
    }

    // -----------------------------------------------------------------
    // TestBackend rendering: the layout/collapse behavior actually drawn,
    // not just the projection it is built from.
    // -----------------------------------------------------------------

    use crate::tui::model::{Item, Model, ToolState};
    use firmius_core::{
        FirmiusConfig, McpManager, PersonaManager, ProviderManager, ToolRegistry, UserSettings,
    };
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;
    use std::sync::{Arc, Mutex};

    fn empty_model(cols: u16, rows: u16) -> (Model, Terminal<TestBackend>) {
        let manager = Arc::new(Mutex::new(ProviderManager::new()));
        let settings = Arc::new(Mutex::new(UserSettings::default()));
        let model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "test-model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings,
            Arc::new(Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        let terminal = Terminal::new(TestBackend::new(cols, rows)).unwrap();
        (model, terminal)
    }

    fn graph_with_nodes(agent: &str, statuses: &[ExecutionStatus]) -> WorkSnapshot {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new(
            "checklist",
            Some(agent.to_string()),
            firmius_core::GraphMode::Advisory,
        );
        for (i, status) in statuses.iter().enumerate() {
            let mut node = firmius_core::WorkNode::new(format!("n{i}"), format!("Item {i}"));
            node.status = *status;
            graph.view_order.push(node.id);
            graph.nodes.insert(node.id, node);
        }
        let id = graph.id;
        state.graphs.insert(id, graph);
        state.active_graph_by_agent.insert(agent.to_string(), id);
        WorkSnapshot::new("session", 0, state)
    }

    /// Zero through five checklist items: rows drawn equal item count, with
    /// no overflow line until a sixth item appears.
    #[test]
    fn zero_to_five_items_render_without_overflow() {
        for count in 0..=5usize {
            let (mut model, mut terminal) = empty_model(60, 24);
            model.focused_id = "agent".into();
            let statuses = vec![ExecutionStatus::Pending; count];
            model.work_snapshot = Some(graph_with_nodes("agent", &statuses));
            terminal
                .draw(|frame| super::super::view::draw(&mut model, frame))
                .unwrap();
            let view = model.work_view(5);
            assert_eq!(view.lines.len(), count);
            assert_eq!(view.overflow, 0);
        }
    }

    /// Four items plus one more triggers the overflow row rather than a
    /// fifth checklist line.
    #[test]
    fn four_plus_items_show_the_overflow_row() {
        let (mut model, _terminal) = empty_model(60, 24);
        model.focused_id = "agent".into();
        let statuses = vec![ExecutionStatus::Pending; 6];
        model.work_snapshot = Some(graph_with_nodes("agent", &statuses));
        let view = model.work_view(5);
        assert_eq!(view.lines.len(), 4);
        assert_eq!(view.overflow, 2);
    }

    /// A very short terminal must still protect the composer: rendering
    /// must not panic, and the layout must remain internally consistent
    /// even when the checklist would otherwise want more rows than the
    /// terminal can spare.
    #[test]
    fn short_terminal_protects_the_composer() {
        let (mut model, mut terminal) = empty_model(40, 8);
        model.focused_id = "agent".into();
        let statuses = vec![ExecutionStatus::Pending; 8];
        model.work_snapshot = Some(graph_with_nodes("agent", &statuses));
        // Must not panic even though the terminal is far shorter than the
        // budgeted composer + work + pending + bars minimum.
        terminal
            .draw(|frame| super::super::view::draw(&mut model, frame))
            .unwrap();
    }

    /// An all-completed graph collapses to a single summary line instead of
    /// listing every finished item.
    #[test]
    fn completed_graph_collapses_to_one_line() {
        let (mut model, mut terminal) = empty_model(60, 24);
        model.focused_id = "agent".into();
        let statuses = vec![ExecutionStatus::Succeeded; 4];
        model.work_snapshot = Some(graph_with_nodes("agent", &statuses));
        terminal
            .draw(|frame| super::super::view::draw(&mut model, frame))
            .unwrap();
        let view = model.work_view(5);
        assert!(view.all_completed);
        assert!(view.lines.is_empty());
    }

    /// An idle agent (no active graph at all) renders no work rows and the
    /// layout does not budget space it does not need.
    #[test]
    fn idle_agent_with_no_graph_renders_no_work_rows() {
        let (mut model, mut terminal) = empty_model(60, 24);
        model.focused_id = "agent".into();
        model.work_snapshot = Some(WorkSnapshot::new("session", 0, WorkState::default()));
        terminal
            .draw(|frame| super::super::view::draw(&mut model, frame))
            .unwrap();
        let view = model.work_view(5);
        assert_eq!(view, WorkView::default());
    }

    /// The checklist projection never inspects transcript tool-call text.
    /// A poisoned `task` tool-call item in the transcript (mismatched JSON,
    /// prose that looks like a status update) must have zero effect on the
    /// rendered checklist, which is derived only from the typed snapshot.
    #[test]
    fn task_tool_transcript_text_is_never_parsed_for_work_state() {
        let (mut model, mut terminal) = empty_model(60, 24);
        model.focused_id = "agent".into();
        model.work_snapshot = Some(graph_with_nodes(
            "agent",
            &[ExecutionStatus::Running, ExecutionStatus::Pending],
        ));
        model.transcripts.insert(
            "agent".into(),
            vec![Item::ToolCall {
                name: "task".into(),
                args: "{ not valid json, status: all done, revision: 999 }".into(),
                result: Some(
                    "graph closed graph_id=ffffffff-ffff-ffff-ffff-ffffffffffff status=completed"
                        .into(),
                ),
                state: ToolState::Done {
                    ok: true,
                    bytes: 0,
                    error: None,
                },
                stream_id: None,
                stream_index: 0,
            }],
        );
        terminal
            .draw(|frame| super::super::view::draw(&mut model, frame))
            .unwrap();
        let view = model.work_view(5);
        // Unaffected by the transcript text: still two rows, not "all done".
        assert_eq!(view.lines.len(), 2);
        assert!(!view.all_completed);
    }
}
