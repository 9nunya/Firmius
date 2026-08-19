//! Domain-level WorkGraph coverage that sits above `work/mod.rs`'s unit
//! tests: multi-agent focus switching and cross-graph isolation.

use firmius_core::{AuthorizationContext, GraphMode, WorkGraph, WorkState};

fn auth(agent: &str) -> AuthorizationContext {
    AuthorizationContext {
        agent_id: agent.into(),
        can_manage: false,
        assignment_ids: Default::default(),
    }
}

/// Each agent's `active_graph_by_agent` entry is independent: switching one
/// agent's focus must not disturb another agent's, and `set_active_graph`
/// is the only sanctioned mutation path (no direct map surgery).
#[test]
fn focus_switching_is_independent_per_agent() {
    let mut state = WorkState::default();
    let graph_a = WorkGraph::new("alpha", Some("agent-a".into()), GraphMode::Advisory);
    let graph_a_id = graph_a.id;
    let graph_b = WorkGraph::new("beta", Some("agent-b".into()), GraphMode::Advisory);
    let graph_b_id = graph_b.id;
    state.create_graph(graph_a, None).unwrap();
    state.create_graph(graph_b, None).unwrap();

    state.set_active_graph("agent-a", graph_a_id).unwrap();
    state.set_active_graph("agent-b", graph_b_id).unwrap();
    assert_eq!(state.active_graph_by_agent["agent-a"], graph_a_id);
    assert_eq!(state.active_graph_by_agent["agent-b"], graph_b_id);

    // Agent A switches to B's graph; agent B's own focus is untouched.
    state.set_active_graph("agent-a", graph_b_id).unwrap();
    assert_eq!(state.active_graph_by_agent["agent-a"], graph_b_id);
    assert_eq!(state.active_graph_by_agent["agent-b"], graph_b_id);

    // Switching to an unknown graph is rejected and leaves focus untouched.
    let bogus = firmius_core::GraphId::new();
    assert!(state.set_active_graph("agent-a", bogus).is_err());
    assert_eq!(state.active_graph_by_agent["agent-a"], graph_b_id);
}

/// Node mutations on one graph never touch a sibling graph owned by a
/// different agent, even when both are live in the same `WorkState`.
#[test]
fn graphs_owned_by_different_agents_are_isolated() {
    let mut state = WorkState::default();
    let mut graph_a = WorkGraph::new("alpha", Some("agent-a".into()), GraphMode::Advisory);
    let graph_a_id = graph_a.id;
    graph_a.objective = Some("a's work".into());
    let graph_b = WorkGraph::new("beta", Some("agent-b".into()), GraphMode::Advisory);
    let graph_b_id = graph_b.id;
    state.create_graph(graph_a, None).unwrap();
    state.create_graph(graph_b, None).unwrap();

    let node = state
        .add_node(
            graph_a_id,
            0,
            &auth("agent-a"),
            firmius_core::NodeInput {
                key: "one".into(),
                title: "First".into(),
                description: None,
            },
        )
        .unwrap();
    assert!(state.graph(graph_a_id).unwrap().nodes.contains_key(&node));
    assert!(state.graph(graph_b_id).unwrap().nodes.is_empty());
    assert_eq!(state.graph(graph_b_id).unwrap().revision, 0);
}
