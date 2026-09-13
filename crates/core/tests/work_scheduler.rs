use std::sync::{Arc, Barrier};

use firmius_core::Session;
use firmius_core::work::*;

#[test]
fn concurrent_scheduler_passes_share_durable_capacity_limits() {
    let session = Session::new_handle();
    session
        .mutate_work(|state| {
            let mut graph = WorkGraph::new("concurrent", Some("owner".into()), GraphMode::Managed);
            let graph_id = graph.id;
            for index in 0..16 {
                let mut node = WorkNode::new(format!("node-{index}"), format!("Node {index}"));
                node.executor = Executor::Agent;
                node.agent = Some(AgentSpec {
                    persona: "coder".into(),
                    prompt: "run".into(),
                    model: None,
                    effort: None,
                });
                graph.view_order.push(node.id);
                graph.nodes.insert(node.id, node);
            }
            state.create_graph(graph.clone(), None)?;
            Ok((graph_id, WorkEvent::GraphCreated { graph }))
        })
        .unwrap();

    let barrier = Arc::new(Barrier::new(16));
    let limits = SchedulerLimits {
        max_concurrent_per_graph: 1,
        max_concurrent_per_session: 1,
    };
    let handles: Vec<_> = (0..16)
        .map(|_| {
            let session = session.clone();
            let barrier = barrier.clone();
            std::thread::spawn(move || {
                barrier.wait();
                session.schedule_ready_work(&limits).claimed.len()
            })
        })
        .collect();
    let claimed: usize = handles
        .into_iter()
        .map(|handle| handle.join().unwrap())
        .sum();
    let running = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .flat_map(|graph| graph.attempts.values())
        .filter(|attempt| attempt.state == ExecutionStatus::Running)
        .count();

    assert_eq!(claimed, 1);
    assert_eq!(running, 1);
}

#[test]
fn zero_limits_claim_nothing_even_with_ready_nodes() {
    let session = Session::new_handle();
    session
        .mutate_work(|state| {
            let mut graph = WorkGraph::new("zero", Some("owner".into()), GraphMode::Managed);
            let graph_id = graph.id;
            for index in 0..3 {
                let mut node = WorkNode::new(format!("node-{index}"), format!("Node {index}"));
                node.executor = Executor::Agent;
                node.agent = Some(AgentSpec {
                    persona: "coder".into(),
                    prompt: "run".into(),
                    model: None,
                    effort: None,
                });
                graph.view_order.push(node.id);
                graph.nodes.insert(node.id, node);
            }
            state.create_graph(graph.clone(), None)?;
            Ok((graph_id, WorkEvent::GraphCreated { graph }))
        })
        .unwrap();

    // A zero ceiling must be honored literally: an empty budget can never
    // admit an attempt, even though nodes are ready to claim.
    let limits = SchedulerLimits {
        max_concurrent_per_graph: 0,
        max_concurrent_per_session: 0,
    };
    let outcome = session.schedule_ready_work(&limits);
    assert!(outcome.claimed.is_empty());
    let running = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .flat_map(|graph| graph.attempts.values())
        .filter(|attempt| attempt.state == ExecutionStatus::Running)
        .count();
    assert_eq!(running, 0);
}
