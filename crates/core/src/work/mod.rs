//! Pure WorkGraph domain.
//!
//! This module intentionally has no session, persistence, tool, or UI
//! dependencies. Callers can clone a [`WorkState`], apply one of the
//! revisioned mutations, persist the candidate, and publish an event only
//! after the durable commit succeeds.

pub mod driver;
pub mod event;
pub mod executor;
pub mod ids;
pub mod inputs;
pub mod model;
pub mod projection;
pub mod readiness;
pub mod scheduler;
pub mod transition;

pub use event::{WorkEvent, WorkEventEnvelope, WorkProjection, WorkSnapshot};
pub use driver::{
    NodeLauncher, NodeOutcome, RunConclusion, RunLimits, RunReport, drive_run,
};
pub use inputs::{compose_node_context, render_manifest};
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
                Vec::new(),
                VerificationLevel::None,
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
            .complete(
                graph_id,
                5,
                &auth(),
                node,
                "fixed",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_ne!(attempt, second);
        assert_eq!(graph.results.len(), 2);
        assert_eq!(graph.attempts[&attempt].result_id, Some(result));
    }

    /// A rejected mutation must leave the graph byte-for-byte unchanged.
    /// A stale revision is the cheapest way to force the rejection without
    /// depending on any particular transition rule.
    #[test]
    fn failed_transition_does_not_change_state() {
        let (mut state, graph_id, node) = graph_with_item();
        let before = state.clone();
        assert!(matches!(
            state.complete(
                graph_id,
                99,
                &auth(),
                node,
                "stale revision",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            ),
            Err(WorkError::StaleRevision { .. })
        ));
        assert_eq!(state, before);
    }

    /// Completing a never-started node is a first-class path: forcing a
    /// `start` round trip just to tick off a todo item is waste. The
    /// synthetic attempt keeps provenance honest — a result always belongs
    /// to an attempt, even one that never ran.
    #[test]
    fn complete_settles_a_node_that_was_never_started() {
        let (mut state, graph_id, node) = graph_with_item();
        let result = state
            .complete(
                graph_id,
                1,
                &auth(),
                node,
                "done without starting",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.nodes[&node].status, ExecutionStatus::Succeeded);
        assert_eq!(graph.nodes[&node].attempt_ids.len(), 1);
        let attempt_id = graph.nodes[&node].attempt_ids[0];
        let attempt = &graph.attempts[&attempt_id];
        assert_eq!(attempt.result_id, Some(result));
        assert_eq!(attempt.state, ExecutionStatus::Succeeded);
        assert!(
            attempt.started_at.is_some() && attempt.finished_at.is_some(),
            "a synthesized attempt is still a real, bounded attempt"
        );
        assert_eq!(graph.results[&result].summary, "done without starting");
    }

    /// The instant path must not become a back door around an assignment:
    /// a node a worker is holding belongs to that worker until it yields.
    #[test]
    fn complete_refuses_a_node_held_by_another_agents_assignment() {
        let (mut state, graph_id, node) = graph_with_item();
        state
            .assign(graph_id, 1, &auth(), node, "worker", None, None)
            .unwrap();
        let before = state.clone();
        let revision = state.graph(graph_id).unwrap().revision;
        assert!(matches!(
            state.complete(
                graph_id,
                revision,
                &auth(),
                node,
                "stolen from the worker",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            ),
            Err(WorkError::AssignmentNotOwned)
        ));
        assert_eq!(state, before);
    }

    /// Batch complete is one transaction and ONE revision bump, over a mix
    /// of started and never-started nodes.
    #[test]
    fn complete_many_settles_every_node_in_one_revision() {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("checklist", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        let ids: Vec<NodeId> = ["a", "b", "c"]
            .into_iter()
            .map(|key| {
                let node = WorkNode::new(key, key);
                let id = node.id;
                graph.view_order.push(id);
                graph.nodes.insert(id, node);
                id
            })
            .collect();
        state.create_graph(graph, None).unwrap();
        // One node is already Running; the other two were never started.
        state.start(graph_id, 0, &auth(), ids[0], None).unwrap();

        let revision = state.graph(graph_id).unwrap().revision;
        let results = state
            .complete_many(
                graph_id,
                revision,
                &auth(),
                ids.iter().map(|id| (*id, "done".to_string())).collect(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        assert_eq!(results.len(), 3);
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(
            graph.revision,
            revision + 1,
            "a batch is one revision bump, not one per node"
        );
        for id in &ids {
            assert_eq!(graph.nodes[id].status, ExecutionStatus::Succeeded);
            assert_eq!(graph.nodes[id].attempt_ids.len(), 1);
        }
        // The already-running node reused its open attempt rather than
        // opening a second one.
        assert_eq!(graph.attempts.len(), 3);
    }

    /// A batch is all-or-nothing: one bad node rejects the whole call and
    /// leaves every other node untouched.
    #[test]
    fn complete_many_is_atomic_across_the_batch() {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("checklist", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        let good = WorkNode::new("a", "A");
        let held = WorkNode::new("b", "B");
        let (good_id, held_id) = (good.id, held.id);
        graph.view_order.extend([good_id, held_id]);
        graph.nodes.insert(good_id, good);
        graph.nodes.insert(held_id, held);
        state.create_graph(graph, None).unwrap();
        state
            .assign(graph_id, 0, &auth(), held_id, "worker", None, None)
            .unwrap();

        let before = state.clone();
        let revision = state.graph(graph_id).unwrap().revision;
        assert!(matches!(
            state.complete_many(
                graph_id,
                revision,
                &auth(),
                vec![
                    (good_id, "fine".to_string()),
                    (held_id, "held by worker".to_string()),
                ],
                Vec::new(),
                VerificationLevel::None,
            ),
            Err(WorkError::AssignmentNotOwned)
        ));
        assert_eq!(
            state, before,
            "a rejected batch must not partially apply the nodes before it"
        );

        // Duplicates are rejected before anything is applied.
        assert!(matches!(
            state.complete_many(
                graph_id,
                revision,
                &auth(),
                vec![
                    (good_id, "once".to_string()),
                    (good_id, "twice".to_string()),
                ],
                Vec::new(),
                VerificationLevel::None,
            ),
            Err(WorkError::InvalidGraph(_))
        ));
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
    fn reassign_hands_a_started_node_to_a_worker_without_a_second_attempt() {
        let (mut state, graph_id, node) = graph_with_item();
        let parent_attempt = state
            .start(graph_id, 1, &auth(), node, Some("owner".into()))
            .unwrap();
        let expected = state.graph(graph_id).unwrap().revision;
        let (attempt, assignment) = state
            .reassign(
                graph_id,
                expected,
                &auth(),
                node,
                "worker",
                Some("owner".into()),
                Some("item-1".into()),
            )
            .unwrap();
        assert_eq!(attempt, parent_attempt, "reassign reuses the open attempt");
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.nodes[&node].status, ExecutionStatus::Running);
        assert_eq!(graph.nodes[&node].attempt_ids.len(), 1);
        assert_eq!(graph.attempts[&attempt].agent_id.as_deref(), Some("worker"));
        assert_eq!(graph.attempts[&attempt].assignment_id, Some(assignment));
        let binding = state.binding_for_agent("worker").unwrap();
        assert_eq!(binding.node_id, node);
        assert_eq!(binding.assignment_id, assignment);
        assert!(
            state.binding_for_agent("owner").is_none(),
            "parent binding for this node is cleared"
        );

        let expected = state.graph(graph_id).unwrap().revision;
        assert!(
            matches!(
                state.assign(graph_id, expected, &auth(), node, "other", None, None),
                Err(WorkError::InvalidTransition { .. })
            ),
            "fresh assign on a Running node still fails"
        );
    }

    /// The fan-in shape the whole design exists for: N workers feeding one
    /// synthesizer, authored in a SINGLE revision. Building this by hand
    /// cost one revision per node, per edge, and per configure call.
    #[test]
    fn plan_authors_a_fan_in_dag_in_one_revision() {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("audit", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();

        let agent = |prompt: &str| {
            Some(AgentSpec {
                persona: "coder".into(),
                prompt: prompt.into(),
                model: None,
                effort: None,
            })
        };
        let mut nodes: Vec<PlannedNode> = (1..=3)
            .map(|i| PlannedNode {
                key: format!("w{i}"),
                title: format!("worker {i}"),
                executor: Executor::Agent,
                agent: agent(&format!("audit slice {i}")),
                ..Default::default()
            })
            .collect();
        nodes.push(PlannedNode {
            key: "syn".into(),
            title: "synthesize".into(),
            executor: Executor::Agent,
            agent: agent("merge the findings"),
            join: Some(JoinPolicy::AllSucceeded),
            max_attempts: Some(2),
            ..Default::default()
        });
        let edges: Vec<PlannedEdge> = (1..=3)
            .map(|i| PlannedEdge {
                from: format!("w{i}"),
                to: "syn".into(),
                condition: EdgeCondition::Succeeded,
                required: true,
                binding_alias: Some(format!("finding_{i}")),
                ..Default::default()
            })
            .collect();

        let created = state
            .plan(
                graph_id,
                0,
                &auth(),
                nodes,
                edges,
                Some("audit the routes".into()),
                Some(true),
            )
            .unwrap();
        assert_eq!(created.len(), 4);

        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.revision, 1, "the whole DAG is one revision bump");
        assert_eq!(graph.mode, GraphMode::Managed);
        assert_eq!(graph.brief.as_deref(), Some("audit the routes"));
        assert_eq!(graph.edges.len(), 3);

        // Only the workers are ready; the synthesizer waits on its join.
        let report = crate::work::evaluate_readiness(graph);
        assert_eq!(report.ready.len(), 3);
        assert!(report.blocked.is_empty());

        // Settle the workers, and the synthesizer becomes ready with every
        // predecessor result bound under its alias.
        let worker_ids: Vec<NodeId> = graph
            .view_order
            .iter()
            .copied()
            .filter(|id| graph.nodes[id].key.starts_with('w'))
            .collect();
        for id in worker_ids {
            let revision = state.graph(graph_id).unwrap().revision;
            state.start(graph_id, revision, &auth(), id, None).unwrap();
            let revision = state.graph(graph_id).unwrap().revision;
            state
                .complete(
                    graph_id,
                    revision,
                    &auth(),
                    id,
                    "found things",
                    Vec::new(),
                    Vec::new(),
                    VerificationLevel::None,
                )
                .unwrap();
        }
        let graph = state.graph(graph_id).unwrap();
        let syn = graph.nodes.values().find(|n| n.key == "syn").unwrap();
        let report = crate::work::evaluate_readiness(graph);
        assert_eq!(report.ready, vec![syn.id]);

        let manifest = graph.freeze_manifest(syn.id);
        let mut aliases: Vec<&str> = manifest.results.keys().map(|k| k.as_str()).collect();
        aliases.sort();
        assert_eq!(aliases, vec!["finding_1", "finding_2", "finding_3"]);
    }

    /// `plan` is additive, so a graph can grow as the work is understood:
    /// a later plan may attach new nodes to nodes an earlier plan created.
    #[test]
    fn plan_extends_an_existing_graph_and_links_to_prior_nodes() {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Advisory);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        state
            .plan(
                graph_id,
                0,
                &auth(),
                vec![PlannedNode {
                    key: "build".into(),
                    title: "build".into(),
                    ..Default::default()
                }],
                Vec::new(),
                None,
                None,
            )
            .unwrap();
        state
            .plan(
                graph_id,
                1,
                &auth(),
                vec![PlannedNode {
                    key: "verify".into(),
                    title: "verify".into(),
                    ..Default::default()
                }],
                vec![PlannedEdge {
                    from: "build".into(),
                    to: "verify".into(),
                    condition: EdgeCondition::Succeeded,
                    required: true,
                    binding_alias: Some("built".into()),
                    ..Default::default()
                }],
                None,
                None,
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.nodes.len(), 2);
        assert_eq!(graph.edges.len(), 1);
        // A duplicate key is rejected rather than silently renamed.
        let revision = graph.revision;
        assert!(matches!(
            state.plan(
                graph_id,
                revision,
                &auth(),
                vec![PlannedNode {
                    key: "build".into(),
                    title: "again".into(),
                    ..Default::default()
                }],
                Vec::new(),
                None,
                None,
            ),
            Err(WorkError::DuplicateKey(_))
        ));
    }

    /// A plan is validated as one candidate revision, so a structural
    /// error rejects the whole call instead of leaving a half-built graph.
    #[test]
    fn plan_rejects_bad_structure_atomically() {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        let before = state.clone();

        // An edge to a node nobody declared.
        assert!(matches!(
            state.plan(
                graph_id,
                0,
                &auth(),
                vec![PlannedNode {
                    key: "a".into(),
                    title: "A".into(),
                    ..Default::default()
                }],
                vec![PlannedEdge {
                    from: "a".into(),
                    to: "ghost".into(),
                    condition: EdgeCondition::Completed,
                    required: true,
                    ..Default::default()
                }],
                None,
                None,
            ),
            Err(WorkError::InvalidGraph(_))
        ));
        assert_eq!(state, before, "a rejected plan applies nothing");

        // A cycle in a managed graph would spin the scheduler forever.
        assert!(matches!(
            state.plan(
                graph_id,
                0,
                &auth(),
                vec![
                    PlannedNode {
                        key: "a".into(),
                        title: "A".into(),
                        ..Default::default()
                    },
                    PlannedNode {
                        key: "b".into(),
                        title: "B".into(),
                        ..Default::default()
                    },
                ],
                vec![
                    PlannedEdge {
                        from: "a".into(),
                        to: "b".into(),
                        condition: EdgeCondition::Completed,
                        required: true,
                        ..Default::default()
                    },
                    PlannedEdge {
                        from: "b".into(),
                        to: "a".into(),
                        condition: EdgeCondition::Completed,
                        required: true,
                        ..Default::default()
                    },
                ],
                None,
                None,
            ),
            Err(WorkError::InvalidGraph(_))
        ));
        assert_eq!(state, before);
    }

    /// An agent node with no persona/prompt is a promise the graph cannot
    /// keep. Rejecting it at author time beats discovering it when the
    /// driver has nothing to launch.
    #[test]
    fn plan_rejects_an_agent_node_without_a_spec() {
        let mut state = WorkState::default();
        let graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        state.create_graph(graph, None).unwrap();
        assert!(matches!(
            state.plan(
                graph_id,
                0,
                &auth(),
                vec![PlannedNode {
                    key: "a".into(),
                    title: "A".into(),
                    executor: Executor::Agent,
                    agent: None,
                    ..Default::default()
                }],
                Vec::new(),
                None,
                None,
            ),
            Err(WorkError::InvalidGraph(_))
        ));
        // And the inverse: a spec on a node that does not run an agent.
        assert!(matches!(
            state.plan(
                graph_id,
                0,
                &auth(),
                vec![PlannedNode {
                    key: "b".into(),
                    title: "B".into(),
                    executor: Executor::Command,
                    agent: Some(AgentSpec {
                        persona: "coder".into(),
                        prompt: "x".into(),
                        model: None,
                        effort: None,
                    }),
                    ..Default::default()
                }],
                Vec::new(),
                None,
                None,
            ),
            Err(WorkError::InvalidGraph(_))
        ));
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
            .complete(
                graph_id,
                4,
                &auth(),
                node,
                "done",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
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
            .complete(
                graph_id2,
                2,
                &auth(),
                node2,
                "done",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
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
                Vec::new(),
                VerificationLevel::None,
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
            .complete(
                graph_id,
                1,
                &worker_auth,
                a_id,
                "done",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
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
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        assert!(
            state.graph(graph_id).unwrap().claims[&claim_id]
                .released_at
                .is_some()
        );
    }

    #[test]
    fn configure_sets_verification_and_acceptance_criteria() {
        let (mut state, graph_id, node) = graph_with_item();
        let criteria = vec![AcceptanceCriterion::new("tests pass")];
        state
            .configure_node(
                graph_id,
                1,
                &auth(),
                node,
                None,
                None,
                None,
                Some(VerificationLevel::Reviewed),
                Some(criteria.clone()),
                Some(ReviewPolicy {
                    requires_independent_reviewer: true,
                }),
            )
            .unwrap();
        let n = &state.graph(graph_id).unwrap().nodes[&node];
        assert_eq!(n.verification, VerificationLevel::Reviewed);
        assert_eq!(n.acceptance_criteria, criteria);
        assert!(n.review_policy.requires_independent_reviewer);
    }

    /// M5.1: a node can succeed in execution while remaining unverified —
    /// the quality digest surfaces this without changing
    /// `ExecutionStatus::Succeeded`.
    #[test]
    fn succeeded_node_without_required_verification_is_reported_unverified() {
        let (mut state, graph_id, node) = graph_with_item();
        state
            .configure_node(
                graph_id,
                1,
                &auth(),
                node,
                None,
                None,
                None,
                Some(VerificationLevel::Reviewed),
                None,
                None,
            )
            .unwrap();
        state.start(graph_id, 2, &auth(), node, None).unwrap();
        state
            .complete(
                graph_id,
                3,
                &auth(),
                node,
                "done",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.nodes[&node].status, ExecutionStatus::Succeeded);
        let digest = crate::work::transition::quality_digest(graph);
        assert_eq!(digest.verification_unmet, 1);
        assert_eq!(digest.verification_met, 0);
    }

    /// Once the achieved verification meets the requirement, the digest
    /// reports the node as verified.
    #[test]
    fn succeeded_node_with_sufficient_verification_is_reported_verified() {
        let (mut state, graph_id, node) = graph_with_item();
        state
            .configure_node(
                graph_id,
                1,
                &auth(),
                node,
                None,
                None,
                None,
                Some(VerificationLevel::Reviewed),
                None,
                None,
            )
            .unwrap();
        state.start(graph_id, 2, &auth(), node, None).unwrap();
        state
            .complete(
                graph_id,
                3,
                &auth(),
                node,
                "done",
                Vec::new(),
                Vec::new(),
                VerificationLevel::Reviewed,
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        let digest = crate::work::transition::quality_digest(graph);
        assert_eq!(digest.verification_met, 1);
        assert_eq!(digest.verification_unmet, 0);
    }

    #[test]
    fn annotate_result_is_append_only_and_bumps_revision() {
        let (mut state, graph_id, node) = graph_with_item();
        state.start(graph_id, 1, &auth(), node, None).unwrap();
        let result_id = state
            .complete(
                graph_id,
                2,
                &auth(),
                node,
                "done",
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        let revision = state.graph(graph_id).unwrap().revision;
        state
            .annotate_result(
                graph_id,
                revision,
                &auth(),
                result_id,
                AnnotationKind::Approval,
                "looks good",
            )
            .unwrap();
        let revision = state.graph(graph_id).unwrap().revision;
        // A second, conflicting annotation from another agent is preserved
        // alongside the first, not merged or overwritten.
        let other = AuthorizationContext {
            agent_id: "owner".into(),
            can_manage: false,
            assignment_ids: Default::default(),
        };
        state
            .annotate_result(
                graph_id,
                revision,
                &other,
                result_id,
                AnnotationKind::Rejection,
                "actually, no",
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.annotations.len(), 2);
        assert!(
            graph
                .annotations
                .values()
                .any(|a| a.kind == AnnotationKind::Approval)
        );
        assert!(
            graph
                .annotations
                .values()
                .any(|a| a.kind == AnnotationKind::Rejection)
        );
        // The original result is untouched.
        assert_eq!(graph.results[&result_id].summary, "done");
        let digest = crate::work::transition::quality_digest(graph);
        assert_eq!(digest.annotations_count, 2);
    }

    #[test]
    fn close_graph_requires_owner_and_expected_revision() {
        let (mut state, graph_id, _node) = graph_with_item();
        let expected = state.graph(graph_id).unwrap().revision;
        let stranger = AuthorizationContext {
            agent_id: "other".into(),
            ..Default::default()
        };
        assert!(matches!(
            state.close_graph(graph_id, expected, &stranger, GraphStatus::Completed),
            Err(WorkError::Unauthorized { .. })
        ));
        assert_eq!(state.graph(graph_id).unwrap().status, GraphStatus::Active);
        assert_eq!(state.graph(graph_id).unwrap().revision, expected);

        assert!(matches!(
            state.close_graph(
                graph_id,
                expected.saturating_add(1),
                &auth(),
                GraphStatus::Completed
            ),
            Err(WorkError::StaleRevision { .. })
        ));
        assert_eq!(state.graph(graph_id).unwrap().status, GraphStatus::Active);

        state
            .close_graph(graph_id, expected, &auth(), GraphStatus::Completed)
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.status, GraphStatus::Completed);
        assert_eq!(graph.revision, expected.saturating_add(1));
    }

    #[test]
    fn reviewer_on_a_separate_node_may_annotate_producer_result() {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        let producer_node = WorkNode::new("a", "A");
        let reviewer_node = WorkNode::new("b", "B");
        let (a_id, b_id) = (producer_node.id, reviewer_node.id);
        graph.view_order.extend([a_id, b_id]);
        graph.nodes.insert(a_id, producer_node);
        graph.nodes.insert(b_id, reviewer_node);
        state.create_graph(graph, None).unwrap();
        let owner_auth = auth();

        let result_id = {
            let revision = state.graph(graph_id).unwrap().revision;
            state
                .start(
                    graph_id,
                    revision,
                    &owner_auth,
                    a_id,
                    Some("producer".into()),
                )
                .unwrap();
            let revision = state.graph(graph_id).unwrap().revision;
            state
                .complete(
                    graph_id,
                    revision,
                    &owner_auth,
                    a_id,
                    "done",
                    Vec::new(),
                    Vec::new(),
                    VerificationLevel::None,
                )
                .unwrap()
        };

        let (_, assignment) = {
            let revision = state.graph(graph_id).unwrap().revision;
            state
                .assign(
                    graph_id,
                    revision,
                    &owner_auth,
                    b_id,
                    "reviewer",
                    None,
                    None,
                )
                .unwrap()
        };
        let reviewer_auth = AuthorizationContext {
            agent_id: "reviewer".into(),
            can_manage: false,
            assignment_ids: [assignment].into_iter().collect(),
        };
        let stranger = AuthorizationContext {
            agent_id: "stranger".into(),
            ..Default::default()
        };

        let revision = state.graph(graph_id).unwrap().revision;
        assert!(matches!(
            state.annotate_result(
                graph_id,
                revision,
                &stranger,
                result_id,
                AnnotationKind::Comment,
                "nope",
            ),
            Err(WorkError::Unauthorized { .. })
        ));
        assert_eq!(state.graph(graph_id).unwrap().annotations.len(), 0);

        state
            .annotate_result(
                graph_id,
                revision,
                &reviewer_auth,
                result_id,
                AnnotationKind::Approval,
                "looks good from review node",
            )
            .unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(graph.annotations.len(), 1);
        assert_eq!(graph.revision, revision.saturating_add(1));
        let annotation = graph.annotations.values().next().unwrap();
        assert_eq!(annotation.annotator_agent_id, "reviewer");
        assert_eq!(annotation.result_id, result_id);
        assert_eq!(annotation.kind, AnnotationKind::Approval);
    }

    #[test]
    fn assign_rejects_the_producer_as_an_independent_reviewer() {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        let producer_node = WorkNode::new("a", "A");
        let mut reviewer_node = WorkNode::new("b", "B");
        reviewer_node.verification = VerificationLevel::IndependentlyVerified;
        let (a_id, b_id) = (producer_node.id, reviewer_node.id);
        graph.view_order.extend([a_id, b_id]);
        graph.nodes.insert(a_id, producer_node);
        graph.nodes.insert(b_id, reviewer_node);
        state.create_graph(graph, None).unwrap();
        let owner_auth = auth();
        let (_, producer_assignment) = state
            .assign(graph_id, 0, &owner_auth, a_id, "producer", None, None)
            .unwrap();
        // The node is held by `producer`, so it settles through the
        // assignment path — the owner may not complete it out from under
        // the worker.
        let producer_auth = AuthorizationContext {
            agent_id: "producer".into(),
            can_manage: false,
            assignment_ids: [producer_assignment].into_iter().collect(),
        };
        let revision = state.graph(graph_id).unwrap().revision;
        state
            .settle_assignment(
                graph_id,
                revision,
                &producer_auth,
                producer_assignment,
                ExecutionStatus::Succeeded,
                Some(Outcome::Success),
                "done",
                None,
                Vec::new(),
                Vec::new(),
                Vec::new(),
                Vec::new(),
                VerificationLevel::None,
            )
            .unwrap();
        graph = state.graph(graph_id).unwrap().clone();
        let edge = crate::work::WorkEdge {
            id: crate::work::EdgeId::new(),
            from: a_id,
            to: b_id,
            kind: crate::work::EdgeKind::Dependency,
            condition: crate::work::EdgeCondition::Succeeded,
            on_outcome: None,
            required: true,
            binding: None,
        };
        // Insert the edge out of band (topology helper not needed for this
        // test's focus) so the reviewer's manifest can bind to `a`'s result.
        let revision = graph.revision;
        state
            .add_edge(
                graph_id,
                revision,
                &owner_auth,
                edge.from,
                edge.to,
                edge.condition,
                edge.required,
                edge.binding,
            )
            .unwrap();
        let revision = state.graph(graph_id).unwrap().revision;
        assert!(matches!(
            state.assign(
                graph_id,
                revision,
                &owner_auth,
                b_id,
                "producer",
                None,
                None
            ),
            Err(WorkError::ReviewerNotIndependent)
        ));
        let revision = state.graph(graph_id).unwrap().revision;
        assert!(
            state
                .assign(
                    graph_id,
                    revision,
                    &owner_auth,
                    b_id,
                    "reviewer",
                    None,
                    None
                )
                .is_ok()
        );
    }
}
