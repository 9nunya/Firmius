use chrono::{Duration, Utc};
use firmius_core::work::*;

fn owner_auth() -> AuthorizationContext {
    AuthorizationContext {
        agent_id: "owner".into(),
        can_manage: true,
        ..Default::default()
    }
}

#[test]
fn edit_collision_lookup_conflicts_with_foreign_inspect_claim() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    let inspector = assign(&mut state, graph_id, nodes[0], "worker-a");
    let editor = assign(&mut state, graph_id, nodes[1], "worker-b");
    let (claim_id, _) = state
        .register_resources(
            state.revision,
            inspector,
            vec![resource(
                "src/lib.rs",
                ResourceKind::File,
                ResourceAccess::Inspect,
            )],
            None,
        )
        .unwrap();

    assert_eq!(
        foreign_mutation_conflict(
            &state,
            Some(editor.assignment_id),
            "workspace-a",
            "src/lib.rs",
        )
        .map(|claim| claim.id),
        Some(claim_id),
        "a mutate request conflicts with a foreign inspect claim"
    );
}

#[test]
fn disabled_policy_does_not_squat_session_ownership() {
    let (mut state, _, _) = state_with_nodes(1);
    state
        .configure_swarm_policy(state.revision, "no-op-owner", SwarmPolicy::Disabled)
        .unwrap();
    assert_eq!(state.swarm.policy, SwarmPolicy::Disabled);
    assert!(state.swarm.policy_owner_agent_id.is_none());

    state
        .configure_swarm_policy(state.revision, "first-opt-in", SwarmPolicy::Advisory)
        .unwrap();
    assert_eq!(
        state.swarm.policy_owner_agent_id.as_deref(),
        Some("first-opt-in")
    );
}

#[test]
fn structural_cycles_block_even_when_swarm_policy_is_advisory() {
    let mut graph = WorkGraph::new("cycle", Some("owner".into()), GraphMode::Managed);
    let first = WorkNode::new("first", "First");
    let second = WorkNode::new("second", "Second");
    let first_id = first.id;
    let second_id = second.id;
    graph.view_order.extend([first_id, second_id]);
    graph.nodes.insert(first_id, first);
    graph.nodes.insert(second_id, second);
    for (from, to) in [(first_id, second_id), (second_id, first_id)] {
        let edge = WorkEdge {
            id: EdgeId::new(),
            from,
            to,
            kind: EdgeKind::Dependency,
            condition: EdgeCondition::Succeeded,
            on_outcome: None,
            required: true,
            binding: None,
        };
        graph.edges.insert(edge.id, edge);
    }

    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Advisory);
    assert!(!analysis.launchable);
    assert!(
        analysis
            .hazards
            .iter()
            .any(|hazard| hazard.kind == "cycle" && hazard.severity == "blocking")
    );
}

#[test]
fn session_policy_owner_is_durable_and_exclusive() {
    let (mut state, _, _) = state_with_nodes(1);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    assert_eq!(state.swarm.policy_owner_agent_id.as_deref(), Some("owner"));
    let snapshot = state.clone();
    assert!(
        state
            .configure_swarm_policy(state.revision, "other-owner", SwarmPolicy::Disabled)
            .is_err()
    );
    assert_eq!(state, snapshot, "unauthorized policy change must be atomic");
}

#[test]
fn advisory_plan_analysis_reports_coordination_hazards_without_blocking_launch() {
    let mut graph = WorkGraph::new("plan", Some("owner".into()), GraphMode::Managed);
    let mut first = WorkNode::new("first", "First");
    first.assignment_contract.intended_mutation_paths = vec!["src/".into()];
    let mut second = WorkNode::new("second", "Second");
    second.assignment_contract.intended_mutation_paths = vec!["src/api.rs".into()];
    second.assignment_contract.consumed_contracts = vec!["missing-api".into()];
    graph.view_order.extend([first.id, second.id]);
    graph.nodes.insert(first.id, first);
    graph.nodes.insert(second.id, second);

    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Advisory);
    assert!(analysis.launchable, "{:?}", analysis.hazards);
    assert!(!analysis.hazards.is_empty());
    assert!(
        analysis
            .hazards
            .iter()
            .all(|hazard| hazard.severity == "warning")
    );
}

#[test]
fn integration_strategy_is_owner_authorized_revisioned_and_path_validated() {
    let (mut state, graph_id, _) = state_with_nodes(1);
    let before_graph_revision = state.graph(graph_id).unwrap().revision;
    let strategy = IntegrationStrategy {
        owner_node_key: Some("node-0".into()),
        authorized_paths: vec!["src/".into()],
        escalation_agent_id: Some("owner".into()),
        rationale: Some("single merge authority".into()),
        ..Default::default()
    };
    state
        .configure_integration_strategy(state.revision, graph_id, "owner", strategy.clone())
        .unwrap();
    assert_eq!(state.graph(graph_id).unwrap().integration, strategy);
    assert_eq!(
        state.graph(graph_id).unwrap().revision,
        before_graph_revision + 1
    );

    let snapshot = state.clone();
    assert!(
        state
            .configure_integration_strategy(
                state.revision,
                graph_id,
                "worker",
                IntegrationStrategy::default(),
            )
            .is_err()
    );
    assert_eq!(state, snapshot, "rejected mutation must remain atomic");

    let snapshot = state.clone();
    assert!(
        state
            .configure_integration_strategy(
                state.revision,
                graph_id,
                "owner",
                IntegrationStrategy {
                    owner_node_key: Some("node-0".into()),
                    authorized_paths: vec!["../escape".into()],
                    ..Default::default()
                },
            )
            .is_err()
    );
    assert_eq!(state, snapshot, "invalid path must not partially apply");
}

fn state_with_nodes(count: usize) -> (WorkState, GraphId, Vec<NodeId>) {
    let mut state = WorkState::default();
    let mut graph = WorkGraph::new("swarm", Some("owner".into()), GraphMode::Advisory);
    let graph_id = graph.id;
    let mut node_ids = Vec::new();
    for index in 0..count {
        let node = WorkNode::new(format!("node-{index}"), format!("Node {index}"));
        node_ids.push(node.id);
        graph.view_order.push(node.id);
        graph.nodes.insert(node.id, node);
    }
    state.create_graph(graph, None).unwrap();
    (state, graph_id, node_ids)
}

fn assign(
    state: &mut WorkState,
    graph_id: GraphId,
    node_id: NodeId,
    agent: &str,
) -> AssignmentToken {
    let revision = state.graph(graph_id).unwrap().revision;
    let (_, assignment_id) = state
        .assign(
            graph_id,
            revision,
            &owner_auth(),
            node_id,
            agent,
            Some("owner".into()),
            None,
        )
        .unwrap();
    state.assignment_token(assignment_id).unwrap().clone()
}

fn resource(path: &str, kind: ResourceKind, access: ResourceAccess) -> WorkspaceResource {
    WorkspaceResource {
        workspace_id: "workspace-a".into(),
        path: path.into(),
        kind,
        access,
        resolution: PathResolutionPolicy::LexicalOnly,
    }
}

#[test]
fn old_work_state_defaults_to_disabled_empty_swarm_state() {
    let state: WorkState = serde_json::from_value(serde_json::json!({
        "revision": 0,
        "graphs": {},
        "active_graph_by_agent": {},
        "local_graph_by_agent": {},
        "active_binding_by_agent": {}
    }))
    .unwrap();
    assert_eq!(state.swarm.policy, SwarmPolicy::Disabled);
    assert!(state.swarm.assignment_fences.is_empty());
    state.validate().unwrap();
}

#[test]
fn paths_are_workspace_relative_and_traversal_is_rejected() {
    assert_eq!(
        normalize_workspace_path("src/./lib.rs").unwrap(),
        "src/lib.rs"
    );
    for invalid in [
        "/tmp/file",
        "../secret",
        "src/../secret",
        "C:/temp",
        "src\\lib.rs",
    ] {
        assert!(
            matches!(
                normalize_workspace_path(invalid),
                Err(SwarmError::InvalidResource(_))
            ),
            "{invalid} must be rejected"
        );
    }
}

#[test]
fn protective_registration_is_atomic_and_expiry_only_marks_suspect() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    let first = assign(&mut state, graph_id, nodes[0], "worker-a");
    let second = assign(&mut state, graph_id, nodes[1], "worker-b");
    let expired = Utc::now() - Duration::seconds(1);
    let (claim_id, conflicts) = state
        .register_resources(
            state.revision,
            first,
            vec![resource(
                "src",
                ResourceKind::Directory,
                ResourceAccess::Mutate,
            )],
            Some(expired),
        )
        .unwrap();
    assert!(conflicts.is_empty());
    let claim = &state.swarm.resource_claims[&claim_id];
    assert!(claim.is_held());
    assert!(claim.is_suspect(Utc::now()));

    let before = state.clone();
    let error = state
        .register_resources(
            state.revision,
            second,
            vec![
                resource("README.md", ResourceKind::File, ResourceAccess::Inspect),
                resource("src/lib.rs", ResourceKind::File, ResourceAccess::Mutate),
            ],
            None,
        )
        .unwrap_err();
    assert!(matches!(error, SwarmError::ResourceConflict { claim_id: id, .. } if id == claim_id));
    assert_eq!(
        state, before,
        "the whole multi-resource request must reject"
    );
}

#[test]
fn advisory_conflicts_are_recorded_but_inspect_inspect_does_not_conflict() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Advisory)
        .unwrap();
    let first = assign(&mut state, graph_id, nodes[0], "worker-a");
    let second = assign(&mut state, graph_id, nodes[1], "worker-b");
    let (first_claim, _) = state
        .register_resources(
            state.revision,
            first,
            vec![resource(
                "src/lib.rs",
                ResourceKind::File,
                ResourceAccess::Inspect,
            )],
            None,
        )
        .unwrap();
    let (_, conflicts) = state
        .register_resources(
            state.revision,
            second.clone(),
            vec![resource(
                "src/lib.rs",
                ResourceKind::File,
                ResourceAccess::Inspect,
            )],
            None,
        )
        .unwrap();
    assert!(conflicts.is_empty());
    let (_, conflicts) = state
        .register_resources(
            state.revision,
            second,
            vec![resource(
                "src/lib.rs",
                ResourceKind::File,
                ResourceAccess::Mutate,
            )],
            None,
        )
        .unwrap();
    assert_eq!(conflicts, vec![first_claim]);
    assert_eq!(
        state.swarm.incidents.len(),
        1,
        "inspect/inspect is not a conflict; only the later mutate overlapping an inspect is recorded"
    );
}

#[test]
fn reassign_advances_generation_and_fences_late_milestones() {
    let (mut state, graph_id, nodes) = state_with_nodes(1);
    let old = assign(&mut state, graph_id, nodes[0], "worker-a");
    let graph_revision = state.graph(graph_id).unwrap().revision;
    let (_, new_assignment_id) = state
        .reassign(
            graph_id,
            graph_revision,
            &owner_auth(),
            nodes[0],
            "worker-b",
            Some("owner".into()),
            None,
        )
        .unwrap();
    let new = state.assignment_token(new_assignment_id).unwrap().clone();
    assert_eq!(new.attempt_id, old.attempt_id);
    assert_eq!(new.generation, old.generation + 1);
    assert_eq!(
        state.swarm.assignment_fences[&old.assignment_id].phase,
        AssignmentPhase::Superseded
    );
    assert!(matches!(
        state.record_milestone(state.revision, old, "late", serde_json::json!({})),
        Err(SwarmError::StaleAssignment)
    ));
}

#[test]
fn milestones_are_generation_fenced_idempotent_and_persist_before_notify() {
    let (mut state, graph_id, nodes) = state_with_nodes(1);
    let token = assign(&mut state, graph_id, nodes[0], "worker");
    let revision = state.revision;
    let id = state
        .record_milestone(
            revision,
            token.clone(),
            "tests-passed",
            serde_json::json!({"count": 12}),
        )
        .unwrap();
    assert!(state.swarm.milestones.contains_key(&id));
    assert!(state.swarm.outbox.values().any(|entry| {
        matches!(entry.event, SwarmOutboxKind::MilestoneRecorded { milestone_id } if milestone_id == id)
    }));
    let after_first = state.clone();
    assert_eq!(
        state
            .record_milestone(
                revision,
                token.clone(),
                "tests-passed",
                serde_json::json!({"count": 12}),
            )
            .unwrap(),
        id
    );
    assert_eq!(
        state, after_first,
        "an exact replay must not create another event"
    );
    assert!(matches!(
        state.record_milestone(
            state.revision,
            token,
            "tests-passed",
            serde_json::json!({"count": 13}),
        ),
        Err(SwarmError::IdempotencyMismatch)
    ));
}

#[test]
fn coordination_settles_only_when_every_peer_is_terminal() {
    let (mut state, graph_id, nodes) = state_with_nodes(1);
    let token = assign(&mut state, graph_id, nodes[0], "requester");
    let request_id = state
        .request_coordination(
            state.revision,
            token.clone(),
            "review-1",
            "review",
            serde_json::json!({"artifact": "patch"}),
            vec![
                ("reviewer-a".into(), CoordinationRole::Responder),
                ("reviewer-b".into(), CoordinationRole::Approver),
            ],
        )
        .unwrap();
    let replay = state
        .request_coordination(
            0,
            token,
            "review-1",
            "review",
            serde_json::json!({"artifact": "patch"}),
            vec![
                ("reviewer-a".into(), CoordinationRole::Responder),
                ("reviewer-b".into(), CoordinationRole::Approver),
            ],
        )
        .unwrap();
    assert_eq!(replay, request_id);
    state
        .settle_coordination_peer(
            state.revision,
            request_id,
            "reviewer-a",
            CoordinationPeerState::Accepted,
            None,
        )
        .unwrap();
    assert_eq!(
        state.swarm.coordination_requests[&request_id].phase,
        CoordinationRequestPhase::Open
    );
    state
        .settle_coordination_peer(
            state.revision,
            request_id,
            "reviewer-b",
            CoordinationPeerState::Rejected,
            Some(serde_json::json!({"reason": "unsafe"})),
        )
        .unwrap();
    assert_eq!(
        state.swarm.coordination_requests[&request_id].phase,
        CoordinationRequestPhase::Settled
    );
    let after = state.clone();
    state
        .settle_coordination_peer(
            0,
            request_id,
            "reviewer-b",
            CoordinationPeerState::Rejected,
            Some(serde_json::json!({"reason": "unsafe"})),
        )
        .unwrap();
    assert_eq!(state, after);
}

#[test]
fn approved_ownership_transfer_replaces_claim_atomically() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    let from = assign(&mut state, graph_id, nodes[0], "worker-a");
    let to = assign(&mut state, graph_id, nodes[1], "worker-b");
    let (claim_id, _) = state
        .register_resources(
            state.revision,
            from.clone(),
            vec![resource(
                "src",
                ResourceKind::Directory,
                ResourceAccess::Mutate,
            )],
            None,
        )
        .unwrap();
    let transfer_id = state
        .request_ownership_transfer(
            state.revision,
            from,
            to.clone(),
            claim_id,
            "handoff-1",
            "worker-b owns the next phase",
        )
        .unwrap();
    let replacement = state
        .decide_ownership_transfer(
            state.revision,
            transfer_id,
            "worker-b",
            true,
            Some("accepted".into()),
        )
        .unwrap()
        .unwrap();
    assert!(!state.swarm.resource_claims[&claim_id].is_held());
    assert_eq!(state.swarm.resource_claims[&replacement].owner, to);
    assert_eq!(
        state.swarm.ownership_transfers[&transfer_id].phase,
        OwnershipTransferPhase::Applied
    );
    state.validate().unwrap();
}

#[test]
fn settlement_and_recovery_release_resources_without_losing_history() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Advisory)
        .unwrap();
    let settled = assign(&mut state, graph_id, nodes[0], "worker-a");
    let (settled_claim, _) = state
        .register_resources(
            state.revision,
            settled.clone(),
            vec![resource(
                "a",
                ResourceKind::Directory,
                ResourceAccess::Mutate,
            )],
            None,
        )
        .unwrap();
    let worker_auth = AuthorizationContext {
        agent_id: "worker-a".into(),
        assignment_ids: [settled.assignment_id].into_iter().collect(),
        ..Default::default()
    };
    state
        .settle_assignment_fenced(
            graph_id,
            state.graph(graph_id).unwrap().revision,
            &worker_auth,
            &settled,
            ExecutionStatus::Succeeded,
            Some(Outcome::Success),
            "done",
            None,
            vec![],
            vec![],
            vec![],
            vec![],
            VerificationLevel::None,
        )
        .unwrap();
    assert!(!state.swarm.resource_claims[&settled_claim].is_held());
    assert_eq!(
        state.swarm.assignment_fences[&settled.assignment_id].phase,
        AssignmentPhase::Succeeded
    );

    let interrupted = assign(&mut state, graph_id, nodes[1], "worker-b");
    let (interrupted_claim, _) = state
        .register_resources(
            state.revision,
            interrupted.clone(),
            vec![resource(
                "b",
                ResourceKind::Directory,
                ResourceAccess::Mutate,
            )],
            None,
        )
        .unwrap();
    assert!(state.reconcile_interrupted());
    assert!(!state.swarm.resource_claims[&interrupted_claim].is_held());
    assert_eq!(
        state.swarm.assignment_fences[&interrupted.assignment_id].phase,
        AssignmentPhase::Interrupted
    );
    assert!(state.swarm.incidents.values().any(|incident| {
        matches!(
            incident.incident,
            SwarmIncidentKind::RecoveryInterrupted { assignment_id, .. }
                if assignment_id == interrupted.assignment_id
        )
    }));
    state.validate().unwrap();
}

#[test]
fn squad_projection_is_bounded_and_plan_compiler_is_deterministic() {
    let spec = SquadPlanSpec {
        workers: vec![
            SquadRoleSpec {
                key: "b".into(),
                title: "B".into(),
                persona: "coder".into(),
                prompt: "build b".into(),
            },
            SquadRoleSpec {
                key: "a".into(),
                title: "A".into(),
                persona: "coder".into(),
                prompt: "build a".into(),
            },
        ],
        synthesizer: Some(SquadRoleSpec {
            key: "merge".into(),
            title: "Merge".into(),
            persona: "reviewer".into(),
            prompt: "merge results".into(),
        }),
    };
    let first = compile_squad_plan(spec.clone()).unwrap();
    let second = compile_squad_plan(spec).unwrap();
    assert_eq!(first, second);
    assert_eq!(
        first
            .nodes
            .iter()
            .map(|node| node.key.as_str())
            .collect::<Vec<_>>(),
        vec!["a", "b", "merge"]
    );
    assert_eq!(first.edges.len(), 2);

    let (mut state, graph_id, nodes) = state_with_nodes(3);
    for (index, node) in nodes.into_iter().enumerate() {
        assign(&mut state, graph_id, node, &format!("worker-{index}"));
    }
    let projected = SquadProjection::from_state(&state, graph_id, Utc::now(), 2);
    assert_eq!(projected.members.len(), 2);
    assert_eq!(projected.omitted, 1);
    assert_eq!(projected.members[0].agent_id, "worker-0");
}

#[test]
fn plan_analysis_blocks_protective_overlap_and_missing_contract_publishers() {
    let mut graph = WorkGraph::new("plan", Some("owner".into()), GraphMode::Managed);
    let mut producer = WorkNode::new("producer", "Producer");
    producer.assignment_contract.intended_mutation_paths = vec!["src/".into()];
    let mut consumer = WorkNode::new("consumer", "Consumer");
    consumer.assignment_contract.intended_mutation_paths = vec!["src/api.rs".into()];
    consumer.assignment_contract.consumed_contracts = vec!["api-v1".into()];
    graph.view_order.extend([producer.id, consumer.id]);
    graph.nodes.insert(producer.id, producer);
    graph.nodes.insert(consumer.id, consumer);

    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(!analysis.launchable);
    assert!(
        analysis
            .hazards
            .iter()
            .any(|hazard| { hazard.kind == "directory_overlap" && hazard.severity == "blocking" })
    );
    assert!(analysis.hazards.iter().any(|hazard| {
        hazard.kind == "missing_publisher" && hazard.resource.as_deref() == Some("api-v1")
    }));
    assert!(
        analysis
            .hazards
            .iter()
            .any(|hazard| hazard.kind == "missing_integration")
    );
}

#[test]
fn plan_analysis_accepts_serialized_contract_flow_with_integration_owner() {
    let mut graph = WorkGraph::new("plan", Some("owner".into()), GraphMode::Managed);
    let mut producer = WorkNode::new("producer", "Producer");
    producer.assignment_contract.intended_mutation_paths = vec!["src/api.rs".into()];
    producer.assignment_contract.published_contracts = vec!["api-v1".into()];
    let mut consumer = WorkNode::new("consumer", "Consumer");
    consumer.assignment_contract.intended_mutation_paths = vec!["src/api.rs".into()];
    consumer.assignment_contract.consumed_contracts = vec!["api-v1".into()];
    let edge = WorkEdge {
        id: EdgeId::new(),
        from: producer.id,
        to: consumer.id,
        kind: EdgeKind::Dependency,
        condition: EdgeCondition::Succeeded,
        on_outcome: None,
        required: true,
        binding: None,
    };
    graph.integration.owner_node_key = Some("consumer".into());
    graph.integration.escalation_agent_id = Some("owner".into());
    graph.view_order.extend([producer.id, consumer.id]);
    graph.nodes.insert(producer.id, producer);
    graph.nodes.insert(consumer.id, consumer);
    graph.edges.insert(edge.id, edge);

    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(analysis.launchable, "{:?}", analysis.hazards);
    assert!(analysis.hazards.is_empty(), "{:?}", analysis.hazards);
}

#[test]
fn optional_contract_edge_does_not_claim_serialization() {
    let mut graph = WorkGraph::new("plan", Some("owner".into()), GraphMode::Managed);
    let mut producer = WorkNode::new("producer", "Producer");
    producer.assignment_contract.published_contracts = vec!["api-v1".into()];
    let mut consumer = WorkNode::new("consumer", "Consumer");
    consumer.assignment_contract.consumed_contracts = vec!["api-v1".into()];
    let edge = WorkEdge {
        id: EdgeId::new(),
        from: producer.id,
        to: consumer.id,
        kind: EdgeKind::Dependency,
        condition: EdgeCondition::Succeeded,
        on_outcome: None,
        required: false,
        binding: None,
    };
    graph.integration.owner_node_key = Some("consumer".into());
    graph.integration.escalation_agent_id = Some("owner".into());
    graph.view_order.extend([producer.id, consumer.id]);
    graph.nodes.insert(producer.id, producer);
    graph.nodes.insert(consumer.id, consumer);
    graph.edges.insert(edge.id, edge);

    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(!analysis.launchable);
    assert!(
        analysis.hazards.iter().any(|hazard| {
            hazard.kind == "interface_dependency" && hazard.severity == "blocking"
        })
    );
}

#[test]
fn edit_collision_lookup_is_session_workspace_and_assignment_scoped() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    let owner = assign(&mut state, graph_id, nodes[0], "worker-a");
    let caller = assign(&mut state, graph_id, nodes[1], "worker-b");
    let (claim_id, _) = state
        .register_resources(
            state.revision,
            owner.clone(),
            vec![WorkspaceResource {
                workspace_id: "canonical-workspace".into(),
                path: "src/".into(),
                kind: ResourceKind::Directory,
                access: ResourceAccess::Mutate,
                resolution: PathResolutionPolicy::LexicalOnly,
            }],
            None,
        )
        .unwrap();

    assert_eq!(
        foreign_mutation_conflict(
            &state,
            Some(caller.assignment_id),
            "canonical-workspace",
            "src/api.rs",
        )
        .map(|claim| claim.id),
        Some(claim_id)
    );
    assert!(
        foreign_mutation_conflict(
            &state,
            Some(owner.assignment_id),
            "canonical-workspace",
            "src/api.rs",
        )
        .is_none()
    );
    assert!(
        foreign_mutation_conflict(
            &state,
            Some(caller.assignment_id),
            "different-workspace",
            "src/api.rs",
        )
        .is_none()
    );
}

// ---------------------------------------------------------------------------
// Transitive contract ordering (P2)
// ---------------------------------------------------------------------------

fn contract_graph(
    producer: &mut WorkNode,
    middle: &mut WorkNode,
    consumer: &mut WorkNode,
    edges: Vec<(NodeId, NodeId, EdgeKind, bool)>,
) -> WorkGraph {
    let mut graph = WorkGraph::new("plan", Some("owner".into()), GraphMode::Managed);
    graph.integration.owner_node_key = Some(consumer.key.clone());
    graph.integration.escalation_agent_id = Some("owner".into());
    for node in [&*producer, &*middle, &*consumer] {
        graph.view_order.push(node.id);
    }
    let (p, m, c) = (producer.id, middle.id, consumer.id);
    graph.nodes.insert(p, producer.clone());
    graph.nodes.insert(m, middle.clone());
    graph.nodes.insert(c, consumer.clone());
    for (from, to, kind, required) in edges {
        let edge = WorkEdge {
            id: EdgeId::new(),
            from,
            to,
            kind,
            condition: EdgeCondition::Succeeded,
            on_outcome: None,
            required,
            binding: None,
        };
        graph.edges.insert(edge.id, edge);
    }
    graph
}

fn contract_nodes() -> (WorkNode, WorkNode, WorkNode) {
    let mut producer = WorkNode::new("producer", "Producer");
    producer.assignment_contract.published_contracts = vec!["api-v1".into()];
    let middle = WorkNode::new("middle", "Middle");
    let mut consumer = WorkNode::new("consumer", "Consumer");
    consumer.assignment_contract.consumed_contracts = vec!["api-v1".into()];
    (producer, middle, consumer)
}

fn interface_blocking(analysis: &PlanAnalysis) -> bool {
    analysis
        .hazards
        .iter()
        .any(|hazard| hazard.kind == "interface_dependency" && hazard.severity == "blocking")
}

#[test]
fn transitive_required_dependency_serializes_contract_flow() {
    let (mut producer, mut middle, mut consumer) = contract_nodes();
    let (p, m, c) = (producer.id, middle.id, consumer.id);
    let graph = contract_graph(
        &mut producer,
        &mut middle,
        &mut consumer,
        vec![
            (p, m, EdgeKind::Dependency, true),
            (m, c, EdgeKind::Dependency, true),
        ],
    );
    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(analysis.launchable, "{:?}", analysis.hazards);
    assert!(!interface_blocking(&analysis), "{:?}", analysis.hazards);
}

#[test]
fn optional_transitive_path_is_not_serialization() {
    let (mut producer, mut middle, mut consumer) = contract_nodes();
    let (p, m, c) = (producer.id, middle.id, consumer.id);
    let graph = contract_graph(
        &mut producer,
        &mut middle,
        &mut consumer,
        vec![
            (p, m, EdgeKind::Dependency, true),
            (m, c, EdgeKind::Dependency, false),
        ],
    );
    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(interface_blocking(&analysis), "{:?}", analysis.hazards);
}

#[test]
fn feedback_transitive_path_is_not_serialization() {
    let (mut producer, mut middle, mut consumer) = contract_nodes();
    let (p, m, c) = (producer.id, middle.id, consumer.id);
    let graph = contract_graph(
        &mut producer,
        &mut middle,
        &mut consumer,
        vec![
            (p, m, EdgeKind::Dependency, true),
            (m, c, EdgeKind::Feedback, true),
        ],
    );
    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(interface_blocking(&analysis), "{:?}", analysis.hazards);
}

#[test]
fn reverse_dependency_does_not_serialize_contract_flow() {
    let (mut producer, mut middle, mut consumer) = contract_nodes();
    let (p, m, c) = (producer.id, middle.id, consumer.id);
    let graph = contract_graph(
        &mut producer,
        &mut middle,
        &mut consumer,
        vec![
            (c, m, EdgeKind::Dependency, true),
            (m, p, EdgeKind::Dependency, true),
        ],
    );
    let analysis = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(interface_blocking(&analysis), "{:?}", analysis.hazards);
}

#[test]
fn required_milestones_are_rejected_in_protective_mode() {
    let mut graph = WorkGraph::new("plan", Some("owner".into()), GraphMode::Managed);
    let mut consumer = WorkNode::new("consumer", "Consumer");
    consumer.assignment_contract.required_milestones = vec!["api-ready".into()];
    graph.view_order.push(consumer.id);
    graph.nodes.insert(consumer.id, consumer);
    let protective = analyze_planned_graph(&graph, SwarmPolicy::Protective);
    assert!(!protective.launchable);
    assert!(protective.hazards.iter().any(|hazard| {
        hazard.kind == "unsupported_milestone_gate" && hazard.severity == "blocking"
    }));
    let advisory = analyze_planned_graph(&graph, SwarmPolicy::Advisory);
    assert!(advisory.launchable);
    assert!(advisory.hazards.iter().any(|hazard| {
        hazard.kind == "unsupported_milestone_gate" && hazard.severity == "warning"
    }));
}

// ---------------------------------------------------------------------------
// Coordination party authority (P1)
// ---------------------------------------------------------------------------

#[test]
fn requester_cannot_be_its_own_peer() {
    let (mut state, graph_id, nodes) = state_with_nodes(1);
    let token = assign(&mut state, graph_id, nodes[0], "solo");
    let error = state
        .request_coordination(
            state.revision,
            token,
            "solo-1",
            "kind",
            serde_json::json!({}),
            vec![("solo".into(), CoordinationRole::Responder)],
        )
        .unwrap_err();
    assert!(matches!(error, SwarmError::InvalidTransition(_)));
}

#[test]
fn cross_graph_peer_is_rejected() {
    let (mut state, graph_id, nodes) = state_with_nodes(1);
    let mut other = WorkGraph::new("other", Some("owner".into()), GraphMode::Advisory);
    let other_node = WorkNode::new("other-node", "Other");
    other.view_order.push(other_node.id);
    other.nodes.insert(other_node.id, other_node.clone());
    let other_id = state.create_graph(other, None).unwrap();
    let revision = state.graph(other_id).unwrap().revision;
    let (_, _foreign) = state
        .assign(
            other_id,
            revision,
            &owner_auth(),
            other_node.id,
            "foreign",
            Some("owner".into()),
            None,
        )
        .unwrap();
    let token = assign(&mut state, graph_id, nodes[0], "requester");
    let error = state
        .request_coordination(
            state.revision,
            token,
            "cross-1",
            "kind",
            serde_json::json!({}),
            vec![("foreign".into(), CoordinationRole::Responder)],
        )
        .unwrap_err();
    assert!(matches!(error, SwarmError::InvalidTransition(_)));
}

#[test]
fn protective_policy_rejects_unbound_peer() {
    let (mut state, graph_id, nodes) = state_with_nodes(1);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    let token = assign(&mut state, graph_id, nodes[0], "requester");
    let error = state
        .request_coordination(
            state.revision,
            token,
            "ghost-1",
            "kind",
            serde_json::json!({}),
            vec![("ghost".into(), CoordinationRole::Responder)],
        )
        .unwrap_err();
    assert!(matches!(error, SwarmError::InvalidTransition(_)));
}

// ---------------------------------------------------------------------------
// Terminal peers (P1)
// ---------------------------------------------------------------------------

#[test]
fn successful_responder_without_reply_settles_the_request() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    let requester = assign(&mut state, graph_id, nodes[0], "requester");
    let responder = assign(&mut state, graph_id, nodes[1], "responder");
    let request_id = state
        .request_coordination(
            state.revision,
            requester,
            "blocked-1",
            "blocked",
            serde_json::json!({"reason": "compile failure"}),
            vec![("responder".into(), CoordinationRole::Responder)],
        )
        .unwrap();
    let party = &state.swarm.coordination_requests[&request_id].parties["responder"];
    assert_eq!(party.assignment_id, Some(responder.assignment_id));
    assert_eq!(party.generation, Some(responder.generation));

    let auth = AuthorizationContext {
        agent_id: "responder".into(),
        can_manage: true,
        ..Default::default()
    };
    let revision = state.graph(graph_id).unwrap().revision;
    state
        .complete(
            graph_id,
            revision,
            &auth,
            nodes[1],
            "finished without replying",
            Vec::new(),
            Vec::new(),
            VerificationLevel::None,
        )
        .unwrap();

    let request = &state.swarm.coordination_requests[&request_id];
    assert_eq!(request.phase, CoordinationRequestPhase::Settled);
    assert_eq!(
        request.parties["responder"].state,
        CoordinationPeerState::Succeeded
    );
    assert!(
        request.parties["responder"].response.is_none(),
        "a terminal peer is never interpreted as acceptance"
    );
    assert!(state.swarm.outbox.values().any(|entry| {
        matches!(
            entry.event,
            SwarmOutboxKind::CoordinationSettled { request_id: id } if id == request_id
        )
    }));
    state.validate().unwrap();
}

#[test]
fn cancelled_responder_resolves_the_request() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    let requester = assign(&mut state, graph_id, nodes[0], "requester");
    let _responder = assign(&mut state, graph_id, nodes[1], "responder");
    let request_id = state
        .request_coordination(
            state.revision,
            requester,
            "cancel-1",
            "blocked",
            serde_json::json!({}),
            vec![("responder".into(), CoordinationRole::Responder)],
        )
        .unwrap();
    let auth = AuthorizationContext {
        agent_id: "responder".into(),
        can_manage: true,
        ..Default::default()
    };
    let revision = state.graph(graph_id).unwrap().revision;
    state.cancel(graph_id, revision, &auth, nodes[1]).unwrap();
    let request = &state.swarm.coordination_requests[&request_id];
    assert_eq!(request.phase, CoordinationRequestPhase::Settled);
    assert_eq!(
        request.parties["responder"].state,
        CoordinationPeerState::Cancelled
    );
    state.validate().unwrap();
}

// ---------------------------------------------------------------------------
// Outbox resolution and bounded acknowledgement
// ---------------------------------------------------------------------------

#[test]
fn outbox_coordination_request_targets_only_pending_parties() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    let requester = assign(&mut state, graph_id, nodes[0], "requester");
    let _responder = assign(&mut state, graph_id, nodes[1], "responder");
    let request_id = state
        .request_coordination(
            state.revision,
            requester,
            "req-1",
            "review",
            serde_json::json!({}),
            vec![("responder".into(), CoordinationRole::Responder)],
        )
        .unwrap();
    let entry = state
        .swarm
        .outbox
        .values()
        .find(|entry| {
            matches!(entry.event, SwarmOutboxKind::CoordinationRequested { request_id: id } if id == request_id)
        })
        .cloned()
        .unwrap();
    let deliveries = resolve_swarm_outbox_entry(&state, &entry);
    assert_eq!(deliveries.len(), 1);
    assert_eq!(deliveries[0].recipient, "responder");
    assert_eq!(deliveries[0].sender, "requester");
    assert!(
        deliveries[0].message_id.contains(&entry.id.to_string()),
        "message id must be derived from stable outbox identity"
    );

    state
        .settle_coordination_peer(
            state.revision,
            request_id,
            "responder",
            CoordinationPeerState::Accepted,
            None,
        )
        .unwrap();
    let settled = state
        .swarm
        .outbox
        .values()
        .find(|entry| {
            matches!(entry.event, SwarmOutboxKind::CoordinationSettled { request_id: id } if id == request_id)
        })
        .cloned()
        .unwrap();
    let deliveries = resolve_swarm_outbox_entry(&state, &settled);
    assert_eq!(deliveries.len(), 1);
    assert_eq!(deliveries[0].recipient, "requester");
}

#[test]
fn outbox_milestone_notice_targets_declared_consumers_only() {
    let (mut state, graph_id, nodes) = state_with_nodes(3);
    state
        .graphs
        .get_mut(&graph_id)
        .unwrap()
        .nodes
        .get_mut(&nodes[1])
        .unwrap()
        .assignment_contract
        .required_milestones = vec!["api-ready".into()];
    let publisher = assign(&mut state, graph_id, nodes[0], "publisher");
    let _consumer = assign(&mut state, graph_id, nodes[1], "consumer");
    let _unrelated = assign(&mut state, graph_id, nodes[2], "unrelated");
    let milestone_id = state
        .record_milestone(
            state.revision,
            publisher,
            "api-ready",
            serde_json::json!({"commit": "abc"}),
        )
        .unwrap();
    let entry = state
        .swarm
        .outbox
        .values()
        .find(|entry| {
            matches!(entry.event, SwarmOutboxKind::MilestoneRecorded { milestone_id: id } if id == milestone_id)
        })
        .cloned()
        .unwrap();
    let deliveries = resolve_swarm_outbox_entry(&state, &entry);
    assert_eq!(deliveries.len(), 1, "{deliveries:?}");
    assert_eq!(deliveries[0].recipient, "consumer");
    assert!(!deliveries.iter().any(|d| d.recipient == "unrelated"));
}

#[test]
fn acknowledge_batch_compacts_acknowledged_but_never_pending() {
    let (mut state, graph_id, nodes) = state_with_nodes(1);
    let token = assign(&mut state, graph_id, nodes[0], "worker");
    for index in 0..8 {
        state
            .record_milestone(
                state.revision,
                token.clone(),
                format!("m{index}"),
                serde_json::json!({}),
            )
            .unwrap();
    }
    assert_eq!(state.swarm.outbox.len(), 8);
    let ids: Vec<_> = state.swarm.outbox.keys().copied().take(4).collect();
    state
        .acknowledge_swarm_outbox_batch(state.revision, &ids, 2)
        .unwrap();
    let acknowledged = state
        .swarm
        .outbox
        .values()
        .filter(|entry| entry.state == SwarmOutboxState::Acknowledged)
        .count();
    let pending = state
        .swarm
        .outbox
        .values()
        .filter(|entry| entry.state == SwarmOutboxState::Pending)
        .count();
    assert_eq!(acknowledged, 2, "acknowledged history is bounded");
    assert_eq!(pending, 4, "pending entries are never compacted");
}

#[test]
fn settling_the_source_cancels_and_timestamps_a_pending_transfer() {
    let (mut state, graph_id, nodes) = state_with_nodes(2);
    state
        .configure_swarm_policy(state.revision, "owner", SwarmPolicy::Protective)
        .unwrap();
    let from = assign(&mut state, graph_id, nodes[0], "worker-a");
    let to = assign(&mut state, graph_id, nodes[1], "worker-b");
    let (claim_id, _) = state
        .register_resources(
            state.revision,
            from.clone(),
            vec![resource(
                "src",
                ResourceKind::Directory,
                ResourceAccess::Mutate,
            )],
            None,
        )
        .unwrap();
    let transfer_id = state
        .request_ownership_transfer(
            state.revision,
            from,
            to,
            claim_id,
            "handoff-cancel",
            "next phase",
        )
        .unwrap();
    let auth = AuthorizationContext {
        agent_id: "worker-a".into(),
        can_manage: true,
        ..Default::default()
    };
    let revision = state.graph(graph_id).unwrap().revision;
    state
        .complete(
            graph_id,
            revision,
            &auth,
            nodes[0],
            "source finished before approval",
            Vec::new(),
            Vec::new(),
            VerificationLevel::None,
        )
        .unwrap();
    let transfer = &state.swarm.ownership_transfers[&transfer_id];
    assert_eq!(transfer.phase, OwnershipTransferPhase::Cancelled);
    assert!(transfer.cancelled_at.is_some());
    assert_eq!(
        state.swarm.resource_claims[&claim_id].phase,
        ResourceClaimPhase::Released
    );
    assert!(state.swarm.outbox.values().any(|entry| matches!(
        entry.event,
        SwarmOutboxKind::OwnershipTransferSettled { transfer_id: id } if id == transfer_id
    )));
    state.validate().unwrap();
}
