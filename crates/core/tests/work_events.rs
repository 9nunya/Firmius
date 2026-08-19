//! Session event bus coverage for work mutations: ordering relative to the
//! durable commit, subscribe/snapshot races, and lagged-receiver recovery.

use firmius_core::{GraphMode, Session, SessionEventPayload, WorkEvent, WorkGraph};

/// `mutate_work` persists the candidate before it installs it in memory or
/// publishes the event, so by the time a subscriber observes the event, the
/// mutation is already durable: reloading the on-disk record must already
/// reflect it.
#[test]
fn event_is_published_only_after_the_write_durably_commits() {
    let session = Session::new_handle();
    let mut rx = session.subscribe();

    let graph = WorkGraph::new("checklist", Some("agent".into()), GraphMode::Advisory);
    let graph_id = graph.id;
    session
        .mutate_work(move |state| {
            state.create_graph(graph.clone(), None)?;
            state.set_active_graph("agent", graph_id)?;
            Ok((graph_id, WorkEvent::GraphCreated { graph }))
        })
        .expect("mutation commits");

    // Once `mutate_work` returns, the event is already on the bus (the send
    // happens synchronously after the durable write, before the function
    // returns), and the record on disk already has the graph.
    let event = rx.try_recv().expect("event already published");
    assert!(matches!(event.payload, SessionEventPayload::Work(_)));
    let record = firmius_core::persistence::load_session_record(&session.id)
        .expect("session was durably written before the event was published");
    assert!(record.work.state.graphs.contains_key(&graph_id));
    let _ = std::fs::remove_file(firmius_core::persistence::session_path(&session.id));
}

/// Subscribing before capturing a snapshot (rather than after) is the
/// race-free pattern the session doc comment calls out: no mutation
/// committed after the subscribe-then-snapshot pair can be silently missed
/// by both the snapshot and the event stream.
#[test]
fn subscribe_before_snapshot_never_loses_an_interleaved_mutation() {
    let session = Session::new_handle();

    let mut rx = session.subscribe();
    let before = session.work_snapshot();
    assert!(before.state.graphs.is_empty());

    let graph = WorkGraph::new("checklist", Some("agent".into()), GraphMode::Advisory);
    let graph_id = graph.id;
    session
        .mutate_work(move |state| {
            state.create_graph(graph.clone(), None)?;
            Ok((graph_id, WorkEvent::GraphCreated { graph }))
        })
        .unwrap();

    // Either the snapshot already had it (it didn't, asserted above) or the
    // subscriber sees it — subscribing first guarantees at least one holds.
    let event = rx.try_recv().expect("mutation observed on the bus");
    match event.payload {
        SessionEventPayload::Work(envelope) => {
            assert!(matches!(
                envelope.event,
                WorkEvent::GraphCreated { graph } if graph.id == graph_id
            ));
        }
        other => panic!("unexpected payload: {other:?}"),
    }
    let _ = std::fs::remove_file(firmius_core::persistence::session_path(&session.id));
}

/// A receiver that falls behind the bus capacity gets `Lagged` rather than a
/// silent gap, and `work_snapshot()` remains the race-free recovery path: it
/// always reflects the latest committed state regardless of how far behind
/// the broadcast receiver has fallen.
#[tokio::test]
async fn a_lagged_receiver_recovers_full_state_from_the_snapshot() {
    let session = Session::new_handle();
    let graph = WorkGraph::new("checklist", Some("agent".into()), GraphMode::Advisory);
    let graph_id = graph.id;
    session
        .mutate_work(move |state| {
            state.create_graph(graph.clone(), None)?;
            Ok((graph_id, WorkEvent::GraphCreated { graph }))
        })
        .unwrap();

    let mut rx = session.subscribe();

    // Flood the bus well past its ring capacity without draining, so the
    // receiver is guaranteed to fall behind.
    // Each flooding mutation reuses `set_active_graph`, which keeps the
    // work state constant-size (no growing node/edge collections), so the
    // cost per durable write stays flat across the whole flood.
    for i in 0..(firmius_core::SESSION_EVENT_CAPACITY + 200) {
        let agent = if i % 2 == 0 { "agent" } else { "agent-alt" };
        session
            .mutate_work(move |state| {
                state.set_active_graph(agent, graph_id)?;
                Ok((
                    (),
                    WorkEvent::ActiveGraphChanged {
                        agent_id: agent.to_string(),
                        graph_id,
                    },
                ))
            })
            .unwrap();
    }

    let lagged = matches!(
        rx.recv().await,
        Err(tokio::sync::broadcast::error::RecvError::Lagged(_))
    );
    assert!(
        lagged,
        "a flooded receiver must observe Lagged, not silence"
    );

    // Regardless of how far the stream lagged, the canonical snapshot has
    // the final state the flood converged on.
    let snapshot = session.work_snapshot();
    let graph = snapshot.state.graphs.get(&graph_id).unwrap();
    assert_eq!(graph.id, graph_id);
    assert_eq!(
        snapshot
            .state
            .active_graph_by_agent
            .get("agent-alt")
            .copied(),
        Some(graph_id)
    );
    let _ = std::fs::remove_file(firmius_core::persistence::session_path(&session.id));
}
