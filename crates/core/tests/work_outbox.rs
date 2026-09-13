use firmius_core::Session;
use firmius_core::SessionHandle;
use firmius_core::work::*;

fn owner_auth() -> AuthorizationContext {
    AuthorizationContext {
        agent_id: "owner".into(),
        can_manage: true,
        ..Default::default()
    }
}

/// Build a canonical work state holding one open coordination request with a
/// single pending responder, plus the pending outbox entry that creation
/// would have produced.
fn state_with_request() -> (WorkState, CoordinationRequestId) {
    let mut state = WorkState::default();
    let mut graph = WorkGraph::new("outbox", Some("owner".into()), GraphMode::Advisory);
    let graph_id = graph.id;
    let a = WorkNode::new("a", "A");
    let b = WorkNode::new("b", "B");
    graph.view_order.extend([a.id, b.id]);
    graph.nodes.insert(a.id, a.clone());
    graph.nodes.insert(b.id, b.clone());
    state.create_graph(graph, None).unwrap();
    let revision = state.graph(graph_id).unwrap().revision;
    let (_, requester) = state
        .assign(
            graph_id,
            revision,
            &owner_auth(),
            a.id,
            "requester",
            Some("owner".into()),
            None,
        )
        .unwrap();
    let revision = state.graph(graph_id).unwrap().revision;
    state
        .assign(
            graph_id,
            revision,
            &owner_auth(),
            b.id,
            "responder",
            Some("owner".into()),
            None,
        )
        .unwrap();
    let token = state.assignment_token(requester).cloned().unwrap();
    let expected = state.revision;
    let request_id = state
        .request_coordination(
            expected,
            token,
            "req-1",
            "review",
            serde_json::json!({"artifact": "patch"}),
            vec![("responder".into(), CoordinationRole::Responder)],
        )
        .unwrap();
    (state, request_id)
}

/// A session whose persisted work state contains a pending outbox entry that
/// was never delivered — the crash window between persistence and delivery.
fn session_with_pending_outbox() -> (SessionHandle, CoordinationRequestId) {
    let session = Session::new_handle();
    let (state, request_id) = state_with_request();
    *session.work.write().unwrap() = state;
    (session, request_id)
}

fn pending_count(session: &SessionHandle) -> usize {
    session
        .work
        .read()
        .unwrap()
        .swarm
        .outbox
        .values()
        .filter(|entry| entry.state == SwarmOutboxState::Pending)
        .count()
}

#[test]
fn commit_auto_dispatches_and_acknowledges() {
    // A normal committed mutation persists the request and then dispatches:
    // by the time `mutate_work` returns, the durable mailbox has the notice
    // and the outbox entry is acknowledged.
    let session = Session::new_handle();
    session
        .mutate_work(|state| {
            let mut graph = WorkGraph::new("outbox", Some("owner".into()), GraphMode::Advisory);
            let graph_id = graph.id;
            let a = WorkNode::new("a", "A");
            let b = WorkNode::new("b", "B");
            graph.view_order.extend([a.id, b.id]);
            graph.nodes.insert(a.id, a.clone());
            graph.nodes.insert(b.id, b.clone());
            state.create_graph(graph, None)?;
            let revision = state.graph(graph_id).unwrap().revision;
            let (_, requester) = state.assign(
                graph_id,
                revision,
                &owner_auth(),
                a.id,
                "requester",
                Some("owner".into()),
                None,
            )?;
            let revision = state.graph(graph_id).unwrap().revision;
            state.assign(
                graph_id,
                revision,
                &owner_auth(),
                b.id,
                "responder",
                Some("owner".into()),
                None,
            )?;
            let token = state.assignment_token(requester).cloned().unwrap();
            let expected = state.revision;
            state
                .request_coordination(
                    expected,
                    token,
                    "req-1",
                    "review",
                    serde_json::json!({}),
                    vec![("responder".into(), CoordinationRole::Responder)],
                )
                .map_err(|error| WorkError::InvalidGraph(error.to_string()))?;
            let revision = state.graph(graph_id).unwrap().revision;
            Ok(((), WorkEvent::GraphChanged { graph_id, revision }))
        })
        .unwrap();
    assert_eq!(session.mailbox_state().records.len(), 1);
    let record = session
        .mailbox_state()
        .records
        .values()
        .next()
        .unwrap()
        .clone();
    assert_eq!(record.recipient_id, "responder");
    assert_eq!(pending_count(&session), 0);
    assert_eq!(session.drain_pending_swarm_outbox().unwrap(), 0);
}

#[test]
fn recovery_replays_a_pending_entry_into_the_durable_mailbox() {
    let (session, request_id) = session_with_pending_outbox();
    assert_eq!(pending_count(&session), 1);
    assert!(
        session.mailbox_state().records.is_empty(),
        "no delivery has happened yet"
    );
    assert_eq!(session.drain_pending_swarm_outbox().unwrap(), 1);
    let records = session.mailbox_state().records;
    assert_eq!(records.len(), 1);
    let record = records.values().next().unwrap();
    assert_eq!(record.recipient_id, "responder");
    assert_eq!(record.sender_id, "requester");
    assert!(matches!(
        session
            .work
            .read()
            .unwrap()
            .swarm
            .outbox
            .values()
            .next()
            .map(|entry| &entry.event),
        Some(SwarmOutboxKind::CoordinationRequested { request_id: id }) if *id == request_id
    ));
    assert_eq!(pending_count(&session), 0);
}

#[test]
fn repeated_dispatch_is_idempotent() {
    let (session, _) = session_with_pending_outbox();
    assert_eq!(session.drain_pending_swarm_outbox().unwrap(), 1);
    let first: Vec<(String, String)> = session
        .mailbox_state()
        .records
        .values()
        .map(|record| (record.message_id.clone(), record.recipient_id.clone()))
        .collect();
    // Acknowledged entries are never redelivered.
    assert_eq!(session.drain_pending_swarm_outbox().unwrap(), 0);
    let second: Vec<(String, String)> = session
        .mailbox_state()
        .records
        .values()
        .map(|record| (record.message_id.clone(), record.recipient_id.clone()))
        .collect();
    assert_eq!(first, second);
}

#[test]
fn crash_after_mailbox_acceptance_replays_without_a_duplicate_record() {
    let (session, _) = session_with_pending_outbox();
    assert_eq!(session.drain_pending_swarm_outbox().unwrap(), 1);
    let message_id = session
        .mailbox_state()
        .records
        .values()
        .next()
        .unwrap()
        .message_id
        .clone();
    // Model a crash after mailbox acceptance but before acknowledgement: the
    // durable mailbox holds the record while the outbox entry is pending.
    {
        let mut state = session.work.write().unwrap();
        for entry in state.swarm.outbox.values_mut() {
            entry.state = SwarmOutboxState::Pending;
        }
    }
    assert_eq!(session.drain_pending_swarm_outbox().unwrap(), 1);
    assert_eq!(session.mailbox_state().records.len(), 1);
    assert_eq!(
        session
            .mailbox_state()
            .records
            .values()
            .next()
            .unwrap()
            .message_id,
        message_id
    );
}
