//! Canonical WorkGraph projection for the compact focused-agent checklist.
//!
//! This module deliberately contains no task-tool output handling.  A work
//! view is derived only from the typed snapshot owned by the session, and a
//! typed event can only advance that snapshot's sequence.  If an event does
//! not contain enough information to fold a complete graph, callers reload
//! the canonical snapshot rather than guessing from prose.

use firmius_core::{
    ExecutionStatus, GraphId, GraphStatus, NodeId, SessionEventPayload, WorkEvent,
    WorkEventEnvelope, WorkGraph, WorkSnapshot,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkLine {
    pub node_id: NodeId,
    pub title: String,
    pub status: ExecutionStatus,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
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
        Self::from_graph(graph, max_lines, Some(graph_id))
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
        Self::for_agent(snapshot, child_agent_id, max_lines)
    }

    /// Hook for a focused parent board.  The parent owns the graph shown by
    /// the mini view; child assignment summaries are a later presentation
    /// concern and must not replace the parent's canonical node state.
    pub fn for_parent(snapshot: &WorkSnapshot, parent_agent_id: &str, max_lines: usize) -> Self {
        Self::for_agent(snapshot, parent_agent_id, max_lines)
    }

    /// Typed hook for parent/child-ready presentation.  Readiness is derived
    /// from the child graph's canonical nodes; no delegate completion prose is
    /// consulted.  The hook is intentionally small until assignment rows are
    /// part of the Milestone 2 mini view.
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

    fn from_graph(graph: &WorkGraph, max_lines: usize, graph_id: Option<GraphId>) -> Self {
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
        Self {
            graph_id,
            graph_title: Some(graph.title.clone()),
            lines: selected
                .into_iter()
                .map(|node| WorkLine {
                    node_id: node.id,
                    title: node.title.clone(),
                    status: node.status,
                })
                .collect(),
            overflow,
            completed,
            all_completed: false,
        }
    }
}

fn is_completed(status: ExecutionStatus) -> bool {
    matches!(
        status,
        ExecutionStatus::Succeeded | ExecutionStatus::Cancelled | ExecutionStatus::Skipped
    )
}

/// Fold the typed event when possible.  Partial work events are intentionally
/// reported as `false`; the model then reloads the canonical session snapshot.
pub fn fold_event(snapshot: &mut WorkSnapshot, event: &WorkEventEnvelope) -> bool {
    if event.session_id != snapshot.session_id || event.sequence <= snapshot.sequence {
        return false;
    }
    // Work events are typed fold inputs, never task-result prose.  Events
    // carrying complete records can be folded locally; compact invalidation
    // events cause the caller to reload the canonical session snapshot.
    let folded = match &event.event {
        WorkEvent::GraphCreated { graph } => {
            snapshot.state.graphs.insert(graph.id, graph.clone());
            true
        }
        WorkEvent::NodeChanged { graph_id, node } => snapshot
            .state
            .graphs
            .get_mut(graph_id)
            .map(|graph| {
                graph.nodes.insert(node.id, node.clone());
                true
            })
            .unwrap_or(false),
        WorkEvent::AttemptChanged { graph_id, attempt } => snapshot
            .state
            .graphs
            .get_mut(graph_id)
            .map(|graph| {
                graph.attempts.insert(attempt.id, attempt.clone());
                true
            })
            .unwrap_or(false),
        WorkEvent::ResultRecorded { graph_id, result } => snapshot
            .state
            .graphs
            .get_mut(graph_id)
            .map(|graph| {
                graph.results.insert(result.id, result.clone());
                true
            })
            .unwrap_or(false),
        WorkEvent::ActiveGraphChanged { agent_id, graph_id } => {
            snapshot
                .state
                .active_graph_by_agent
                .insert(agent_id.clone(), *graph_id);
            true
        }
        // A revision-only event cannot update authored order, status, or
        // active-agent selection.  Let the canonical snapshot replace it.
        WorkEvent::GraphChanged { .. } => false,
    };
    if folded {
        snapshot.sequence = event.sequence;
    }
    folded
}

/// Typed envelope extraction used by the model's unified session bus fold.
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

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::Utc;
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
    fn partial_events_request_canonical_recovery() {
        let mut snapshot = snapshot(1);
        let graph_id = *snapshot.state.graphs.keys().next().unwrap();
        let event = WorkEventEnvelope {
            session_id: "session".into(),
            sequence: 1,
            at: Utc::now(),
            event: WorkEvent::GraphChanged {
                graph_id,
                revision: 1,
            },
        };
        assert!(!fold_event(&mut snapshot, &event));
        assert_eq!(snapshot.sequence, 0);
    }

    #[test]
    fn duplicate_and_wrong_session_events_never_advance_snapshot() {
        let mut snapshot = snapshot(1);
        snapshot.sequence = 3;
        let event = WorkEventEnvelope {
            session_id: "other-session".into(),
            sequence: 4,
            at: Utc::now(),
            event: WorkEvent::GraphChanged {
                graph_id: *snapshot.state.graphs.keys().next().unwrap(),
                revision: 1,
            },
        };
        assert!(!fold_event(&mut snapshot, &event));
        assert_eq!(snapshot.sequence, 3);
        let mut old = event;
        old.session_id = "session".into();
        old.sequence = 3;
        assert!(!fold_event(&mut snapshot, &old));
        assert_eq!(status_glyph(ExecutionStatus::Running), "◐");
    }

    #[test]
    fn glyphs_are_static_and_terminal_limit_is_bounded() {
        assert_eq!(status_glyph(ExecutionStatus::Succeeded), "✓");
        assert_eq!(status_glyph(ExecutionStatus::Failed), "!");
        assert_eq!(row_limit_for_terminal(3), 3);
        assert_eq!(row_limit_for_terminal(9), 5);
    }
}
