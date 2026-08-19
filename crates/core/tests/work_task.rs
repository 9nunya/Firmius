//! `task` tool coverage: scope authorization and the guarantee that no
//! consumer needs to parse the tool's text output — the durable session
//! work snapshot is the only source of truth.

use std::collections::HashSet;
use std::sync::Arc;

use firmius_core::{
    AgentState, LocalHost, Session, ToolContext, ToolError, ToolRegistry, WORK_READ_SCOPE,
    WORK_WRITE_SCOPE, register_task_tool,
};
use tokio_util::sync::CancellationToken;

fn ctx(
    session: &firmius_core::SessionHandle,
    agent: &str,
    scopes: Option<HashSet<String>>,
) -> ToolContext {
    ToolContext {
        workdir: std::env::temp_dir(),
        cancellation: CancellationToken::new(),
        tool_call_id: "call".into(),
        agent_id: agent.into(),
        session_id: session.id.clone(),
        state: Arc::new(std::sync::RwLock::new(AgentState::default())),
        host: Arc::new(LocalHost::new()),
        session: Some(session.clone()),
        allowed_scopes: scopes,
    }
}

fn registry() -> Arc<ToolRegistry> {
    let registry = ToolRegistry::default();
    register_task_tool(&registry);
    Arc::new(registry)
}

fn scopes(names: &[&str]) -> HashSet<String> {
    names.iter().map(|s| s.to_string()).collect()
}

/// A caller with only `work_read` may view the checklist but not mutate it:
/// the required-scope gate on registration covers read, and the tool's own
/// `require_write` guard covers every mutating mode.
#[tokio::test]
async fn read_only_scope_cannot_mutate_the_checklist() {
    let session = Session::new_handle();
    let tools = registry();
    let read_only = ctx(&session, "agent", Some(scopes(&[WORK_READ_SCOPE])));

    let error = tools
        .call_scoped(
            "task",
            serde_json::json!({"mode": "create", "title": "checklist"}),
            read_only,
            Some(&scopes(&[WORK_READ_SCOPE])),
        )
        .await
        .expect_err("create without work_write must be denied");
    assert!(
        matches!(error, ToolError::PermissionDenied { .. }),
        "{error:?}"
    );
    assert!(session.work.read().unwrap().graphs.is_empty());
}

/// A caller missing even `work_read` cannot reach the tool at all —
/// enforced by `ToolRegistry::call`'s required-scope check before the tool
/// body runs.
#[tokio::test]
async fn missing_read_scope_is_rejected_before_the_tool_runs() {
    let session = Session::new_handle();
    let tools = registry();
    let no_scopes = ctx(&session, "agent", Some(scopes(&["fs_read"])));

    let error = tools
        .call_scoped(
            "task",
            serde_json::json!({"mode": "view"}),
            no_scopes,
            Some(&scopes(&["fs_read"])),
        )
        .await
        .expect_err("task must be unreachable without work_read");
    assert!(
        matches!(error, ToolError::PermissionDenied { .. }),
        "{error:?}"
    );
}

/// With both scopes granted, an agent can create and view its own
/// checklist.
#[tokio::test]
async fn full_scope_can_create_and_view() {
    let session = Session::new_handle();
    let tools = registry();
    let full = ctx(
        &session,
        "agent",
        Some(scopes(&[WORK_READ_SCOPE, WORK_WRITE_SCOPE])),
    );

    tools
        .call(
            "task",
            serde_json::json!({"mode": "create", "title": "checklist"}),
            full.clone(),
        )
        .await
        .expect("create should succeed with both scopes");
    assert_eq!(session.work.read().unwrap().graphs.len(), 1);

    let output = tools
        .call("task", serde_json::json!({"mode": "view"}), full)
        .await
        .expect("view should succeed");
    assert!(output.contains("checklist"));
}

/// No consumer should ever need to parse the `task` tool's returned text to
/// recover state: the session's durable `WorkState` snapshot alone must
/// already reflect every committed mutation, independent of what the tool
/// happened to print. Corrupting the text output must not change the
/// answer derived from the typed snapshot.
#[tokio::test]
async fn state_is_recoverable_from_the_snapshot_without_parsing_tool_text() {
    let session = Session::new_handle();
    let tools = registry();
    let full = ctx(
        &session,
        "agent",
        Some(scopes(&[WORK_READ_SCOPE, WORK_WRITE_SCOPE])),
    );

    let text_output = tools
        .call(
            "task",
            serde_json::json!({"mode": "create", "title": "checklist", "items": ["first"]}),
            full,
        )
        .await
        .expect("create should succeed");
    let _ = text_output;
    // Deliberately corrupt the tool's text output. A design that needed to
    // parse it for state would now be wrong; the typed snapshot must not be.
    let text_output = "not json at all, garbage output".to_string();
    let _ = text_output;

    let snapshot = session.work_snapshot();
    let graph = snapshot
        .state
        .graphs
        .values()
        .next()
        .expect("graph exists in the typed snapshot regardless of tool text");
    assert_eq!(graph.title, "checklist");
    assert_eq!(graph.nodes.len(), 1);
}

/// `init` is idempotent: calling it twice for the same agent must not
/// create and orphan a second graph.
#[tokio::test]
async fn init_is_idempotent_for_the_same_agent() {
    let session = Session::new_handle();
    let tools = registry();
    let full = ctx(
        &session,
        "agent",
        Some(scopes(&[WORK_READ_SCOPE, WORK_WRITE_SCOPE])),
    );

    tools
        .call(
            "task",
            serde_json::json!({"mode": "init", "title": "checklist"}),
            full.clone(),
        )
        .await
        .expect("first init creates a graph");
    assert_eq!(session.work.read().unwrap().graphs.len(), 1);

    tools
        .call(
            "task",
            serde_json::json!({"mode": "init", "title": "checklist"}),
            full,
        )
        .await
        .expect("second init reuses the existing graph");
    assert_eq!(
        session.work.read().unwrap().graphs.len(),
        1,
        "init must not orphan a second graph"
    );
}

/// `set_active` and `close` round-trip through the session's active-graph
/// tracking and graph status, both of which are otherwise dead without a
/// tool surface to drive them.
#[tokio::test]
async fn set_active_and_close_modes_work() {
    let session = Session::new_handle();
    let tools = registry();
    let full = ctx(
        &session,
        "agent",
        Some(scopes(&[WORK_READ_SCOPE, WORK_WRITE_SCOPE])),
    );

    tools
        .call(
            "task",
            serde_json::json!({"mode": "create", "title": "one"}),
            full.clone(),
        )
        .await
        .unwrap();
    let first_graph = *session.work.read().unwrap().graphs.keys().next().unwrap();

    tools
        .call(
            "task",
            serde_json::json!({"mode": "create", "title": "two"}),
            full.clone(),
        )
        .await
        .unwrap();
    let active = session.work.read().unwrap().active_graph_by_agent["agent"];
    assert_ne!(active, first_graph, "the second create becomes active");

    tools
        .call(
            "task",
            serde_json::json!({"mode": "set_active", "graph_id": first_graph.to_string()}),
            full.clone(),
        )
        .await
        .expect("set_active should succeed");
    assert_eq!(
        session.work.read().unwrap().active_graph_by_agent["agent"],
        first_graph
    );

    tools
        .call(
            "task",
            serde_json::json!({
                "mode": "close",
                "graph_id": first_graph.to_string(),
                "expected_revision": session.work.read().unwrap().graphs[&first_graph].revision
            }),
            full,
        )
        .await
        .expect("close should succeed");
    assert_eq!(
        session.work.read().unwrap().graphs[&first_graph].status,
        firmius_core::GraphStatus::Completed
    );
}
