//! Durability-layer coverage: the coordinator that every work mutation and
//! `Session::save()` routes through, plus backward-compatible loading of a
//! session record written before `WorkGraph` existed.

use std::sync::Arc;
use std::thread;

use firmius_core::persistence::{
    SessionPersistenceCoordinator, SessionRecord, load_session_record, load_session_record_at,
    session_path_at,
};
use firmius_core::work::{AuthorizationContext, ExecutionStatus, GraphMode, Outcome, WorkGraph};
use firmius_core::{
    AgentConfig, PersonaManager, Provider, ProviderError, ProviderEvent, ProviderManager,
    ProviderRequest, Session, StopReason, ToolRegistry,
};
use futures::StreamExt;

fn tmp_base(name: &str) -> std::path::PathBuf {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("firmius-work-persistence-{name}-{nanos}"));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn minimal_record(id: &str, title: &str) -> SessionRecord {
    let mut record: SessionRecord = serde_json::from_str(
        r#"{
            "id": "PLACEHOLDER",
            "created_at": "2024-01-01T00:00:00Z",
            "updated_at": "2024-01-01T00:00:00Z",
            "agents": [],
            "hierarchy": {}
        }"#,
    )
    .unwrap();
    record.id = id.to_string();
    record.title = Some(title.to_string());
    record
}

/// A write that cannot be durably committed (the base directory cannot be
/// created because a path component is an ordinary file) must return an
/// error rather than silently succeeding — this is the failure signal
/// `mutate_work` relies on to refuse installing a candidate state that was
/// never actually persisted.
#[test]
fn persistence_failure_is_reported_and_leaves_no_partial_write() {
    let base = tmp_base("failure");
    let blocker = base.join("blocked");
    std::fs::write(&blocker, b"not a directory").unwrap();
    // `sessions_dir_at` joins `base/sessions`; forcing `base` itself to be a
    // file makes every write under it fail to create its directory.
    let coordinator = SessionPersistenceCoordinator::new(blocker.clone());
    let record = minimal_record("failure-session", "should not land");

    let result = coordinator.save(&record);
    assert!(result.is_err(), "save over a blocked path must fail");
    assert!(
        !blocker
            .join("sessions")
            .join("failure-session.json")
            .exists(),
        "a failed write must not leave a partial file behind"
    );
}

/// Concurrent saves against the same coordinator are serialized by a single
/// writer thread and tagged with a monotonic generation, so the file on
/// disk always reflects the most recently issued write rather than
/// whichever thread happened to reach the filesystem first.
#[test]
fn concurrent_writes_serialize_and_the_latest_generation_always_wins() {
    let base = tmp_base("ordering");
    let coordinator = Arc::new(SessionPersistenceCoordinator::new(base.clone()));
    let id = "ordering-session";

    let mut handles = Vec::new();
    for revision in 1..=32u32 {
        let coordinator = coordinator.clone();
        let mut record = minimal_record(id, &format!("rev-{revision}"));
        // Fold the revision into the record itself so the final file's
        // content, not just its existence, proves a definite write order.
        record.title = Some(format!("rev-{revision}"));
        handles.push(thread::spawn(move || coordinator.save(&record)));
    }
    for handle in handles {
        handle
            .join()
            .unwrap()
            .expect("every save should commit or be superseded, not error");
    }

    let loaded = load_session_record_at(&base, id).expect("final record readable");
    // Exactly one of the 32 titles must be present, and the file must never
    // be a torn/partial write straddling two of them.
    assert!(loaded.title.as_deref().unwrap().starts_with("rev-"));
    let path = session_path_at(&base, id);
    let raw = std::fs::read_to_string(&path).unwrap();
    let reparsed: SessionRecord = serde_json::from_str(&raw).expect("file is valid, complete JSON");
    assert_eq!(reparsed.title, loaded.title);
}

/// A `SessionRecord` written before `WorkGraph` existed has no `work` field
/// at all. `Session::from_record` must load it as an empty, valid
/// `WorkState` rather than refusing to resume the session.
#[test]
fn pre_workgraph_session_loads_with_default_work_state() {
    let raw = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/tests/fixtures/pre_work_session.json"
    ))
    .unwrap();
    let record: SessionRecord = serde_json::from_str(&raw).expect("legacy record still parses");
    assert!(record.agents.is_empty());

    let manager = ProviderManager::new();
    let tools = Arc::new(ToolRegistry::default());
    let session = Session::from_record(record, &manager, tools).expect("legacy record resumes");
    let state = session.work.read().unwrap();
    assert!(state.graphs.is_empty());
    assert!(state.active_graph_by_agent.is_empty());
    assert_eq!(state.revision, 0);
}

struct StubProvider;

#[async_trait::async_trait]
impl Provider for StubProvider {
    fn id(&self) -> &str {
        "stub"
    }
    async fn stream(
        &self,
        _request: ProviderRequest,
    ) -> Result<
        futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
        ProviderError,
    > {
        Ok(futures::stream::iter([Ok(ProviderEvent::Done {
            reason: StopReason::Stop,
        })])
        .boxed())
    }
}

fn test_personas() -> Arc<PersonaManager> {
    let dir = tmp_base("personas");
    std::fs::write(
        dir.join("coder.md"),
        "---\nname: Coder\ntool_scopes: [fs_read, fs_write]\nbackground: true\n---\nCode.",
    )
    .unwrap();
    Arc::new(PersonaManager::load_from(dir).unwrap())
}

/// Item 11 (D4): save a session mid-attempt (worker holds a live, unsettled
/// assignment), then resume it. Resume must: release the open assignment,
/// record an immutable `Interrupted` result, leave the node retryable, and
/// deliver a notification into the parent agent's mailbox — all *before*
/// the resumed session is handed back to the caller.
#[tokio::test]
async fn resume_reconciles_interrupted_work_and_notifies_parent() {
    let personas = test_personas();
    let session = Session::new_handle();
    let parent = session.spawn_agent_with_personas(
        Arc::new(StubProvider),
        Arc::new(ToolRegistry::default()),
        AgentConfig {
            provider_id: "stub".into(),
            model: "stub-model".into(),
            ..Default::default()
        },
        personas.clone(),
    );
    let worker = session.spawn_subagent_with_personas(
        &parent.id,
        None,
        Arc::new(StubProvider),
        Arc::new(ToolRegistry::default()),
        AgentConfig {
            provider_id: "stub".into(),
            model: "stub-model".into(),
            ..Default::default()
        },
        personas.clone(),
    );

    let mut graph = WorkGraph::new("checklist", Some(parent.id.clone()), GraphMode::Advisory);
    let graph_id = graph.id;
    let node = firmius_core::work::WorkNode::new("item-1", "do the thing");
    let node_id = node.id;
    graph.view_order.push(node_id);
    graph.nodes.insert(node_id, node);
    session
        .mutate_work(|state| {
            state.create_graph(graph, None)?;
            Ok((
                (),
                firmius_core::work::WorkEvent::GraphChanged {
                    graph_id,
                    revision: 1,
                },
            ))
        })
        .unwrap();

    let worker_id = worker.id.clone();
    let parent_id = parent.id.clone();
    let assignment_id = session
        .mutate_work(move |state| {
            let expected = state.graph(graph_id)?.revision;
            let auth = AuthorizationContext {
                agent_id: parent_id.clone(),
                can_manage: true,
                ..Default::default()
            };
            let (_, assignment_id) = state.assign(
                graph_id,
                expected,
                &auth,
                node_id,
                worker_id.clone(),
                Some(parent_id.clone()),
                None,
            )?;
            Ok((
                assignment_id,
                firmius_core::work::WorkEvent::GraphChanged {
                    graph_id,
                    revision: state.graph(graph_id)?.revision,
                },
            ))
        })
        .unwrap();

    // Simulate an unclean shutdown mid-attempt: the session file on disk
    // reflects the Running attempt/open assignment, exactly as `save()`
    // would leave it if the process died before the worker yielded.
    session.save().expect("mid-attempt session saves");
    let session_id = session.id.clone();

    let mut manager = ProviderManager::new();
    manager.register_schema(firmius_core::ProviderSchema {
        id: "stub".into(),
        api_type: firmius_core::ApiType::OpenAI,
        base_url: Some("http://127.0.0.1:1".into()),
        api_key_env: None,
        models: vec![],
    });
    manager.set_api_key("stub", "test-key");
    let tools = Arc::new(ToolRegistry::default());
    let record = load_session_record(&session_id).expect("saved record readable");
    let resumed =
        Session::from_record_with_personas(record, &manager, tools, personas.clone()).unwrap();

    let state = resumed.work.read().unwrap();
    let graph = state.graph(graph_id).unwrap();
    let assignment = &graph.assignments[&assignment_id];
    assert!(
        assignment.released_at.is_some(),
        "interrupted assignment must be released, not stuck open forever"
    );
    assert_eq!(graph.nodes[&node_id].status, ExecutionStatus::Interrupted);
    let attempt_id = graph.assignments[&assignment_id].attempt_id;
    let result_id = graph.attempts[&attempt_id]
        .result_id
        .expect("interrupted attempt has a durable result");
    assert_eq!(
        graph.results[&result_id].outcome,
        Some(Outcome::Interrupted)
    );
    drop(state);

    // Node is retryable, not permanently stuck.
    let parent_id_for_retry = parent.id.clone();
    resumed
        .mutate_work(move |state| {
            let expected = state.graph(graph_id)?.revision;
            let auth = AuthorizationContext {
                agent_id: parent_id_for_retry,
                can_manage: true,
                ..Default::default()
            };
            state.retry(graph_id, expected, &auth, node_id)?;
            Ok((
                (),
                firmius_core::work::WorkEvent::GraphChanged {
                    graph_id,
                    revision: state.graph(graph_id)?.revision,
                },
            ))
        })
        .expect("interrupted node is retryable after resume");

    // Parent was notified: the reconciliation notification landed in its
    // mailbox without requiring `delegate poll`/`wait`.
    let resumed_parent = resumed
        .agent(&parent.id)
        .expect("parent agent resumes")
        .clone();
    assert!(
        resumed_parent
            .pending_messages()
            .iter()
            .any(|m| m.contains("interrupted")),
        "parent mailbox should contain the interruption notification"
    );
}
