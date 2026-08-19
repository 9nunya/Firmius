//! Pure WorkGraph domain.
//!
//! This module intentionally has no session, persistence, tool, or UI
//! dependencies. Callers can clone a [`WorkState`], apply one of the
//! revisioned mutations, persist the candidate, and publish an event only
//! after the durable commit succeeds.

pub mod event;
pub mod executor;
pub mod ids;
pub mod model;
pub mod projection;
pub mod readiness;
pub mod scheduler;
pub mod transition;

pub use event::{WorkEvent, WorkEventEnvelope, WorkProjection, WorkSnapshot};
pub use executor::{CommandSpec, ExecutorError, execute_agent, execute_command, settle_claim};
pub use ids::{AssignmentId, AttemptId, EdgeId, GraphId, ManifestId, NodeId, ResultId};
pub use model::*;
pub use projection::{MiniProjection, MiniRow};
pub use readiness::{ReadinessReport, evaluate_readiness};
pub use scheduler::{ClaimedAttempt, ScheduleOutcome, SchedulerLimits, schedule_ready_work};
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
        assert_eq!(restored.work_sequence, 9);
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

    /// D4: an interrupted assignment must not become permanently
    /// unsettleable. Reconciliation releases the assignment, records an
    /// immutable `Interrupted` result envelope, and leaves the node
    /// retryable — and it notifies the parent, exactly as a normal
    /// `settle_assignment` would.
    #[test]
    fn reconcile_interrupted_releases_assignment_and_notifies_parent() {
        let (mut state, graph_id, node) = graph_with_item();
        let owner_auth = auth();
        let (attempt_id, assignment_id) = state
            .assign(
                graph_id,
                1,
                &owner_auth,
                node,
                "worker",
                Some("owner".into()),
                None,
            )
            .unwrap();
        assert!(state.binding_for_agent("worker").is_some());

        assert!(state.reconcile_interrupted());

        let graph = state.graph(graph_id).unwrap();
        // Assignment released: settleable state going forward.
        let assignment = &graph.assignments[&assignment_id];
        assert!(assignment.released_at.is_some());
        // Attempt carries a durable result envelope.
        let attempt = &graph.attempts[&attempt_id];
        assert_eq!(attempt.state, ExecutionStatus::Interrupted);
        let result_id = attempt.result_id.expect("interrupted attempt has a result");
        let result = &graph.results[&result_id];
        assert_eq!(result.execution_status, ExecutionStatus::Interrupted);
        assert_eq!(result.outcome, Some(Outcome::Interrupted));
        // Node is retryable (not stuck Running).
        assert_eq!(graph.nodes[&node].status, ExecutionStatus::Interrupted);
        let revision = graph.revision;
        assert!(state.retry(graph_id, revision, &owner_auth, node).is_ok());
        // Parent notified.
        let graph = state.graph(graph_id).unwrap();
        assert!(
            graph
                .notifications
                .iter()
                .any(|n| n.parent_agent_id == "owner" && n.result_id == result_id)
        );
        // Binding was cleared, so the worker cannot double-settle.
        assert!(state.binding_for_agent("worker").is_none());
    }

    #[test]
    fn unblock_requires_a_blocked_node_and_block_rejects_terminal_nodes() {
        let (mut state, graph_id, node) = graph_with_item();
        // Cannot unblock a node that was never blocked.
        assert!(matches!(
            state.unblock(graph_id, 1, &auth(), node),
            Err(WorkError::InvalidTransition { .. })
        ));
        state.block(graph_id, 1, &auth(), node).unwrap();
        assert_eq!(
            state.graph(graph_id).unwrap().nodes[&node].status,
            ExecutionStatus::Blocked
        );
        state.unblock(graph_id, 2, &auth(), node).unwrap();
        assert_eq!(
            state.graph(graph_id).unwrap().nodes[&node].status,
            ExecutionStatus::Pending
        );
        // Cannot block a node that already succeeded.
        state.start(graph_id, 3, &auth(), node, None).unwrap();
        state
            .complete(graph_id, 4, &auth(), node, "done", Vec::new())
            .unwrap();
        assert!(matches!(
            state.block(graph_id, 5, &auth(), node),
            Err(WorkError::InvalidTransition { .. })
        ));
    }

    #[test]
    fn cancel_on_running_node_settles_attempt_and_releases_assignment() {
        let (mut state, graph_id, node) = graph_with_item();
        let (attempt, assignment) = state
            .assign(graph_id, 1, &auth(), node, "worker", None, None)
            .unwrap();
        assert!(state.binding_for_agent("worker").is_some());
        state.cancel(graph_id, 2, &auth(), node).unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.nodes[&node].status, ExecutionStatus::Cancelled);
        assert_eq!(graph.attempts[&attempt].state, ExecutionStatus::Cancelled);
        assert!(graph.attempts[&attempt].result_id.is_some());
        assert!(graph.assignments[&assignment].released_at.is_some());
        assert!(state.binding_for_agent("worker").is_none());
        // Cancelling an already-succeeded node is rejected, not silently
        // discarded.
        let (mut state2, graph_id2, node2) = graph_with_item();
        state2.start(graph_id2, 1, &auth(), node2, None).unwrap();
        state2
            .complete(graph_id2, 2, &auth(), node2, "done", Vec::new())
            .unwrap();
        assert!(matches!(
            state2.cancel(graph_id2, 3, &auth(), node2),
            Err(WorkError::InvalidTransition { .. })
        ));
    }

    #[test]
    fn retry_cap_is_enforced_by_start_and_assign_too() {
        let (mut state, graph_id, node) = graph_with_item();
        state
            .graph_mut(graph_id)
            .unwrap()
            .nodes
            .get_mut(&node)
            .unwrap()
            .retry_policy
            .max_attempts = 1;
        state.start(graph_id, 1, &auth(), node, None).unwrap();
        state
            .fail(
                graph_id,
                2,
                &auth(),
                node,
                Outcome::Failure,
                "failed",
                Vec::new(),
            )
            .unwrap();
        // Direct start must not bypass the cap that retry() enforces.
        assert!(matches!(
            state.start(graph_id, 3, &auth(), node, None),
            Err(WorkError::RetryUnavailable)
        ));
        assert!(matches!(
            state.assign(graph_id, 3, &auth(), node, "worker", None, None),
            Err(WorkError::RetryUnavailable)
        ));
        assert!(matches!(
            state.retry(graph_id, 3, &auth(), node),
            Err(WorkError::RetryUnavailable)
        ));
    }

    #[test]
    fn validate_rejects_a_dangling_active_binding() {
        let (mut state, graph_id, node) = graph_with_item();
        state
            .assign(graph_id, 1, &auth(), node, "worker", None, None)
            .unwrap();
        assert!(state.validate().is_ok());
        // Corrupt the binding so it no longer points at a live assignment.
        state
            .active_binding_by_agent
            .get_mut("worker")
            .unwrap()
            .assignment_id = crate::work::AssignmentId::new();
        assert!(matches!(state.validate(), Err(WorkError::InvalidGraph(_))));
    }

    #[test]
    fn assignee_may_only_act_on_their_own_assigned_node_not_siblings() {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("checklist", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        let a = WorkNode::new("a", "A");
        let b = WorkNode::new("b", "B");
        let (a_id, b_id) = (a.id, b.id);
        graph.view_order.push(a_id);
        graph.view_order.push(b_id);
        graph.nodes.insert(a_id, a);
        graph.nodes.insert(b_id, b);
        state.create_graph(graph, None).unwrap();
        let (_, assignment) = state
            .assign(graph_id, 0, &auth(), a_id, "worker", None, None)
            .unwrap();
        let worker_auth = AuthorizationContext {
            agent_id: "worker".into(),
            can_manage: false,
            assignment_ids: [assignment].into_iter().collect(),
        };
        // The assignee may complete their own assigned node.
        state
            .complete(graph_id, 1, &worker_auth, a_id, "done", Vec::new())
            .unwrap();
        // But not touch a sibling node they hold no assignment for.
        assert!(matches!(
            state.start(graph_id, 2, &worker_auth, b_id, None),
            Err(WorkError::Unauthorized { .. })
        ));
        // Nor mutate topology.
        assert!(matches!(
            state.move_node(graph_id, 2, &worker_auth, b_id, Some(a_id)),
            Err(WorkError::Unauthorized { .. })
        ));
    }

    #[test]
    fn acquire_claim_detects_overlap_and_is_released_on_settlement() {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("checklist", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        let a = WorkNode::new("a", "A");
        let b = WorkNode::new("b", "B");
        let (a_id, b_id) = (a.id, b.id);
        graph.view_order.push(a_id);
        graph.view_order.push(b_id);
        graph.nodes.insert(a_id, a);
        graph.nodes.insert(b_id, b);
        state.create_graph(graph, None).unwrap();

        let (_, assignment_one) = state
            .assign(graph_id, 0, &auth(), a_id, "worker-one", None, None)
            .unwrap();
        let (_, assignment_two) = state
            .assign(graph_id, 1, &auth(), b_id, "worker-two", None, None)
            .unwrap();

        let auth_one = AuthorizationContext {
            agent_id: "worker-one".into(),
            can_manage: false,
            assignment_ids: [assignment_one].into_iter().collect(),
        };
        let (claim_id, overlaps) = state
            .acquire_claim(
                graph_id,
                2,
                &auth_one,
                assignment_one,
                vec!["src/lib.rs".into(), "./src/../src/main.rs".into()],
                None,
            )
            .unwrap();
        assert!(overlaps.is_empty());
        // Path normalization: `./src/../src/main.rs` collapses to
        // `src/main.rs`.
        assert_eq!(
            state.graph(graph_id).unwrap().claims[&claim_id].paths,
            vec!["src/lib.rs".to_string(), "src/main.rs".to_string()]
        );
        // A traversal that escapes the workspace boundary is rejected.
        assert!(matches!(
            state.acquire_claim(
                graph_id,
                3,
                &auth_one,
                assignment_one,
                vec!["../outside.rs".into()],
                None,
            ),
            Err(WorkError::InvalidGraph(_))
        ));

        // A second, unrelated worker claiming an overlapping path is
        // warned via the overlap list — but never blocked.
        let auth_two = AuthorizationContext {
            agent_id: "worker-two".into(),
            can_manage: false,
            assignment_ids: [assignment_two].into_iter().collect(),
        };
        let (_second_claim_id, overlaps) = state
            .acquire_claim(
                graph_id,
                3,
                &auth_two,
                assignment_two,
                vec!["src/lib.rs".into()],
                None,
            )
            .unwrap();
        assert_eq!(overlaps, vec![claim_id.clone()]);

        // Settling worker-one's assignment releases its claim.
        state
            .settle_assignment(
                graph_id,
                4,
                &auth_one,
                assignment_one,
                ExecutionStatus::Succeeded,
                Some(Outcome::Success),
                "done",
                None,
                Vec::new(),
                Vec::new(),
                Vec::new(),
            )
            .unwrap();
        assert!(
            state.graph(graph_id).unwrap().claims[&claim_id]
                .released_at
                .is_some()
        );
    }
}
