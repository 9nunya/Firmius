//! Pure WorkGraph domain.
//!
//! This module intentionally has no session, persistence, tool, or UI
//! dependencies. Callers can clone a [`WorkState`], apply one of the
//! revisioned mutations, persist the candidate, and publish an event only
//! after the durable commit succeeds.

pub mod event;
pub mod ids;
pub mod model;
pub mod projection;
pub mod transition;

pub use event::{WorkEvent, WorkEventEnvelope, WorkProjection, WorkSnapshot};
pub use ids::{AssignmentId, AttemptId, EdgeId, GraphId, ManifestId, NodeId, ResultId};
pub use model::*;
pub use projection::{MiniProjection, MiniRow};
pub use transition::WorkError;

#[cfg(test)]
mod tests {
    use super::*;

    fn auth() -> AuthorizationContext {
        AuthorizationContext {
            agent_id: "owner".into(),
            can_manage: false,
            assignment_ids: Default::default(),
        }
    }

    fn graph_with_item() -> (WorkState, GraphId, NodeId) {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("checklist", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        let node_id = state
            .add_node(
                graph_id,
                0,
                &auth(),
                NodeInput {
                    key: "one".into(),
                    title: "First item".into(),
                    description: None,
                },
            )
            .unwrap();
        (state, graph_id, node_id)
    }

    #[test]
    fn typed_ids_are_distinct_and_round_trip() {
        let graph = GraphId::new();
        let text = graph.to_string();
        assert_eq!(GraphId::parse(&text).unwrap(), graph);
        assert_ne!(graph.as_uuid(), NodeId::new().as_uuid());
    }

    #[test]
    fn graph_has_explicit_order_and_revisioned_mutations() {
        let (mut state, graph_id, first) = graph_with_item();
        assert_eq!(state.graph(graph_id).unwrap().view_order, vec![first]);
        let second = state
            .add_node(
                graph_id,
                1,
                &auth(),
                NodeInput {
                    key: "two".into(),
                    title: "Second".into(),
                    description: None,
                },
            )
            .unwrap();
        assert_eq!(state.graph(graph_id).unwrap().revision, 2);
        state
            .move_node(graph_id, 2, &auth(), second, Some(first))
            .unwrap();
        assert_eq!(
            state.graph(graph_id).unwrap().view_order,
            vec![second, first]
        );
        assert!(matches!(
            state.update_node(graph_id, 2, &auth(), first, None, None),
            Err(WorkError::StaleRevision { .. })
        ));
    }

    #[test]
    fn execution_and_outcome_are_separate_and_results_are_immutable() {
        let (mut state, graph_id, node) = graph_with_item();
        let attempt = state
            .start(graph_id, 1, &auth(), node, Some("owner".into()))
            .unwrap();
        let result = state
            .fail(
                graph_id,
                2,
                &auth(),
                node,
                Outcome::TestFailed,
                "tests failed",
                vec!["log".into()],
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.nodes[&node].status, ExecutionStatus::Failed);
        assert_eq!(
            graph.nodes[&node].effective_outcome,
            Some(Outcome::TestFailed)
        );
        assert_eq!(graph.attempts[&attempt].result_id, Some(result));
        assert_eq!(graph.results[&result].summary, "tests failed");
        assert_eq!(graph.results.len(), 1);
        state.retry(graph_id, 3, &auth(), node).unwrap();
        let second = state.start(graph_id, 4, &auth(), node, None).unwrap();
        state
            .complete(graph_id, 5, &auth(), node, "fixed", Vec::new())
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_ne!(attempt, second);
        assert_eq!(graph.results.len(), 2);
        assert_eq!(graph.attempts[&attempt].result_id, Some(result));
    }

    #[test]
    fn failed_transition_does_not_change_state() {
        let (mut state, graph_id, node) = graph_with_item();
        let before = state.clone();
        assert!(
            state
                .complete(graph_id, 1, &auth(), node, "not started", Vec::new())
                .is_err()
        );
        assert_eq!(state, before);
    }

    #[test]
    fn owner_authorization_is_enforced() {
        let (mut state, graph_id, node) = graph_with_item();
        let stranger = AuthorizationContext {
            agent_id: "other".into(),
            ..Default::default()
        };
        assert!(matches!(
            state.start(graph_id, 1, &stranger, node, None),
            Err(WorkError::Unauthorized { .. })
        ));
        assert_eq!(state.graph(graph_id).unwrap().revision, 1);
    }

    #[test]
    fn graph_validation_catches_order_and_edge_errors() {
        let mut graph = WorkGraph::new("invalid", None, GraphMode::Managed);
        let node = WorkNode::new("a", "A");
        graph.nodes.insert(node.id, node);
        assert!(graph.validate().is_err());
        graph.view_order.push(NodeId::new());
        assert!(graph.validate().is_err());
    }

    #[test]
    fn mini_projection_is_prioritized_and_capped() {
        let mut graph = WorkGraph::new("many", None, GraphMode::Advisory);
        for i in 0..8 {
            let mut node = WorkNode::new(format!("n{i}"), format!("Node {i}"));
            if i == 6 {
                node.status = ExecutionStatus::Failed;
            }
            if i == 7 {
                node.status = ExecutionStatus::Running;
            }
            graph.view_order.push(node.id);
            graph.nodes.insert(node.id, node);
        }
        let projection = MiniProjection::from_graph(&graph);
        assert_eq!(projection.rows.len(), 4);
        assert_eq!(projection.overflow, 4);
        assert!(
            projection
                .rows
                .iter()
                .any(|row| row.status == ExecutionStatus::Failed)
        );
        assert!(
            projection
                .rows
                .iter()
                .any(|row| row.status == ExecutionStatus::Running)
        );
    }

    #[test]
    fn snapshots_and_records_serialize_with_defaults() {
        let (state, graph_id, _) = graph_with_item();
        let snapshot = WorkSnapshot::new("session", 9, state);
        let encoded = serde_json::to_string(&snapshot).unwrap();
        let restored: WorkSnapshot = serde_json::from_str(&encoded).unwrap();
        assert_eq!(restored.sequence, 9);
        assert_eq!(restored.graph(graph_id).unwrap().title, "checklist");
        let old = r#"{"revision":0,"graphs":{},"active_graph_by_agent":{}}"#;
        assert_eq!(
            serde_json::from_str::<WorkState>(old).unwrap(),
            WorkState::default()
        );
    }

    #[test]
    fn resume_reconciliation_preserves_attempt_and_marks_running_work_interrupted() {
        let (mut state, graph_id, node) = graph_with_item();
        let attempt = state
            .start(graph_id, 1, &auth(), node, Some("owner".into()))
            .unwrap();
        assert!(state.reconcile_interrupted());
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.nodes[&node].status, ExecutionStatus::Interrupted);
        assert_eq!(graph.attempts[&attempt].state, ExecutionStatus::Interrupted);
        assert!(!state.reconcile_interrupted());
    }
}
