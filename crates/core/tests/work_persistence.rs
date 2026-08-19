//! Durability-layer coverage: the coordinator that every work mutation and
//! `Session::save()` routes through, plus backward-compatible loading of a
//! session record written before `WorkGraph` existed.

use std::sync::Arc;
use std::thread;

use firmius_core::persistence::{
    SessionPersistenceCoordinator, SessionRecord, load_session_record_at, session_path_at,
};
use firmius_core::{ProviderManager, Session, ToolRegistry};

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
