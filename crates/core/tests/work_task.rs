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

#[tokio::test]
async fn start_without_graph_id_uses_the_active_graph() {
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
            serde_json::json!({"mode": "init", "title": "list", "items": ["one"]}),
            full.clone(),
        )
        .await
        .unwrap();
    let output = tools
        .call(
            "task",
            serde_json::json!({"mode": "start", "key": "item-1", "expected_revision": 0}),
            full,
        )
        .await
        .expect("start should fall back to the active graph");
    assert!(output.contains("started key=item-1"), "{output}");
    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    let node = graph.nodes.values().find(|n| n.key == "item-1").unwrap();
    assert_eq!(node.status, firmius_core::work::ExecutionStatus::Running);
}

#[tokio::test]
async fn add_accepts_items_batch_and_quoted_graph_id() {
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
            serde_json::json!({"mode": "init", "title": "list"}),
            full.clone(),
        )
        .await
        .unwrap();
    let gid = session
        .work
        .read()
        .unwrap()
        .graphs
        .keys()
        .next()
        .unwrap()
        .to_string();
    let quoted = format!("\"{gid}\"");
    let output = tools
        .call(
            "task",
            serde_json::json!({
                "mode": "add",
                "graph_id": quoted,
                "expected_revision": 0,
                "items": ["alpha", "beta"]
            }),
            full,
        )
        .await
        .expect("quoted graph_id and items batch should succeed");
    assert!(output.contains("added"), "{output}");
    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    assert_eq!(graph.nodes.len(), 2);
    assert_eq!(graph.revision, 1, "batch add is one revision bump");
    let keys: std::collections::BTreeSet<_> =
        graph.nodes.values().map(|n| n.key.as_str()).collect();
    assert_eq!(keys, ["item-1", "item-2"].into_iter().collect());
}

/// Ticking off a checklist item that was never `start`ed must be ONE call.
/// Forcing `start` then `complete` is pure overhead for work the agent is
/// tracking as a todo list rather than executing as a claimed attempt.
#[tokio::test]
async fn complete_settles_an_unstarted_item_in_one_call() {
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
            serde_json::json!({"mode": "init", "title": "list", "items": ["one"]}),
            full.clone(),
        )
        .await
        .unwrap();

    let output = tools
        .call(
            "task",
            serde_json::json!({
                "mode": "complete",
                "key": "item-1",
                "expected_revision": 0,
                "summary": "did it"
            }),
            full,
        )
        .await
        .expect("completing a never-started item should succeed");
    assert!(output.contains("completed item-1"), "{output}");

    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    let node = graph.nodes.values().find(|n| n.key == "item-1").unwrap();
    assert_eq!(node.status, firmius_core::work::ExecutionStatus::Succeeded);
    assert_eq!(
        node.attempt_ids.len(),
        1,
        "the result must still hang off a real attempt"
    );
    let result = graph
        .results
        .values()
        .find(|r| r.node_id == node.id)
        .expect("a result was recorded");
    assert_eq!(result.summary, "did it");
}

/// Several items finish together in ONE call and ONE revision. Previously
/// each completion invalidated the caller's `expected_revision`, so ticking
/// off three items meant three sequential view/complete round trips.
#[tokio::test]
async fn complete_settles_many_items_in_one_revision() {
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
            serde_json::json!({
                "mode": "init",
                "title": "list",
                "items": ["one", "two", "three"]
            }),
            full.clone(),
        )
        .await
        .unwrap();
    // Mix a claimed node in with the untouched ones.
    tools
        .call(
            "task",
            serde_json::json!({"mode": "start", "key": "item-2", "expected_revision": 0}),
            full.clone(),
        )
        .await
        .unwrap();

    let revision = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .unwrap()
        .revision;
    let output = tools
        .call(
            "task",
            serde_json::json!({
                "mode": "complete",
                "keys": ["item-1", "item-2", "item-3"],
                "expected_revision": revision,
                "summary": "batch done"
            }),
            full,
        )
        .await
        .expect("batch complete should succeed");
    assert!(
        output.contains("completed item-1, item-2, item-3"),
        "{output}"
    );

    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    for key in ["item-1", "item-2", "item-3"] {
        let node = graph.nodes.values().find(|n| n.key == key).unwrap();
        assert_eq!(
            node.status,
            firmius_core::work::ExecutionStatus::Succeeded,
            "{key} should be settled"
        );
    }
    assert_eq!(
        graph.revision,
        revision + 1,
        "the whole batch is a single revision bump"
    );
}

/// The batch is all-or-nothing. An unknown key rejects the entire call so
/// the caller never has to reconcile a partially applied checklist.
#[tokio::test]
async fn batch_complete_rejects_the_whole_call_on_a_bad_key() {
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
            serde_json::json!({"mode": "init", "title": "list", "items": ["one", "two"]}),
            full.clone(),
        )
        .await
        .unwrap();

    tools
        .call(
            "task",
            serde_json::json!({
                "mode": "complete",
                "keys": ["item-1", "item-nope"],
                "expected_revision": 0
            }),
            full,
        )
        .await
        .expect_err("an unknown key must reject the batch");

    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    assert_eq!(graph.revision, 0, "a rejected batch must not bump revision");
    for node in graph.nodes.values() {
        assert_eq!(
            node.status,
            firmius_core::work::ExecutionStatus::Pending,
            "no node may settle when the batch is rejected"
        );
    }
}

/// The fan-in shape, authored through the TOOL in one call: ten workers
/// feeding a synthesizer. Previously this needed one `add` plus one
/// `connect` per edge plus one `configure` for the join, each its own
/// revision with a `view` in between, which is why the DAG machinery went
/// unused despite existing.
#[tokio::test]
async fn plan_authors_a_fan_in_workflow_in_one_call() {
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
            serde_json::json!({"mode": "init", "title": "audit"}),
            full.clone(),
        )
        .await
        .unwrap();

    let mut nodes: Vec<serde_json::Value> = (1..=10)
        .map(|i| {
            serde_json::json!({
                "key": format!("w{i}"),
                "title": format!("audit slice {i}"),
                "executor": "agent",
                "persona": "coder",
                "prompt": format!("Audit slice {i} for missing auth checks.")
            })
        })
        .collect();
    nodes.push(serde_json::json!({
        "key": "syn",
        "title": "synthesize findings",
        "executor": "agent",
        "persona": "general",
        "prompt": "Merge the bound findings into one ranked list.",
        "join_policy": "all_succeeded",
        "max_attempts": 2
    }));
    let edges: Vec<serde_json::Value> = (1..=10)
        .map(|i| {
            serde_json::json!({
                "from": format!("w{i}"),
                "to": "syn",
                "condition": "succeeded",
                "binding_alias": format!("finding_{i}")
            })
        })
        .collect();

    let output = tools
        .call(
            "task",
            serde_json::json!({
                "mode": "plan",
                "expected_revision": 0,
                "brief": "Repo conventions apply. Report evidence, not guesses.",
                "managed": true,
                "nodes": nodes,
                "edges": edges
            }),
            full,
        )
        .await
        .expect("plan should author the whole DAG in one call");
    assert!(output.contains("planned 11 node(s)"), "{output}");

    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    assert_eq!(graph.revision, 1, "the whole DAG is a single revision bump");
    assert_eq!(graph.mode, firmius_core::GraphMode::Managed);
    assert_eq!(graph.nodes.len(), 11);
    assert_eq!(graph.edges.len(), 10);
    assert_eq!(
        graph.brief.as_deref(),
        Some("Repo conventions apply. Report evidence, not guesses.")
    );

    // Only the ten workers are ready; the synthesizer waits on its join.
    let report = firmius_core::work::evaluate_readiness(&graph);
    assert_eq!(report.ready.len(), 10);
    assert!(report.blocked.is_empty());

    // Every worker carries its own task sheet.
    let worker = graph.nodes.values().find(|n| n.key == "w3").unwrap();
    let spec = worker.agent.as_ref().expect("agent node carries a spec");
    assert_eq!(spec.persona, "coder");
    assert!(spec.prompt.contains("slice 3"));
}

/// An `agent` node with no persona/prompt is rejected at author time, not
/// discovered later when there is nothing to launch.
#[tokio::test]
async fn plan_rejects_an_agent_node_missing_its_spec() {
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
            serde_json::json!({"mode": "init", "title": "g"}),
            full.clone(),
        )
        .await
        .unwrap();

    tools
        .call(
            "task",
            serde_json::json!({
                "mode": "plan",
                "expected_revision": 0,
                "nodes": [{"key": "a", "title": "A", "executor": "agent"}]
            }),
            full,
        )
        .await
        .expect_err("an agent node without persona/prompt must be rejected");

    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    assert_eq!(graph.revision, 0, "a rejected plan applies nothing");
    assert!(graph.nodes.is_empty());
}

#[tokio::test]
async fn cancel_with_run_id_stops_the_run_and_cancels_unfinished_nodes() {
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
            serde_json::json!({"mode": "init", "title": "managed cancellation"}),
            full.clone(),
        )
        .await
        .unwrap();
    tools
        .call(
            "task",
            serde_json::json!({
                "mode": "plan",
                "expected_revision": 0,
                "managed": true,
                "nodes": [{"key": "manual", "title": "in flight"}]
            }),
            full.clone(),
        )
        .await
        .unwrap();
    tools
        .call(
            "task",
            serde_json::json!({
                "mode": "start",
                "key": "manual",
                "expected_revision": 1
            }),
            full.clone(),
        )
        .await
        .unwrap();

    let launched = tools
        .call("task", serde_json::json!({"mode": "launch"}), full.clone())
        .await
        .unwrap();
    let run_id = launched
        .split_whitespace()
        .find_map(|part| part.strip_prefix("run_id="))
        .expect("launch returns a run id")
        .to_string();

    let cancelled = tools
        .call(
            "task",
            serde_json::json!({"mode": "cancel", "run_id": run_id}),
            full,
        )
        .await
        .expect("one run-level cancel should succeed");
    assert!(cancelled.contains("cancelled run_id="), "{cancelled}");

    let graph = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    assert_eq!(graph.status, firmius_core::work::GraphStatus::Cancelled);
    let node = graph
        .nodes
        .values()
        .find(|node| node.key == "manual")
        .unwrap();
    assert_eq!(node.status, firmius_core::work::ExecutionStatus::Cancelled);
    let attempt = graph
        .attempts
        .get(node.attempt_ids.last().unwrap())
        .unwrap();
    assert_eq!(
        attempt.state,
        firmius_core::work::ExecutionStatus::Cancelled
    );

    let report = tokio::time::timeout(std::time::Duration::from_secs(1), session.wait_run(&run_id))
        .await
        .expect("cancelled run should conclude promptly")
        .unwrap();
    assert_eq!(
        report.conclusion,
        firmius_core::work::RunConclusion::Cancelled
    );
}

#[tokio::test]
async fn second_live_run_for_the_same_graph_is_rejected() {
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
            serde_json::json!({"mode": "init", "title": "one driver"}),
            full.clone(),
        )
        .await
        .unwrap();
    tools
        .call(
            "task",
            serde_json::json!({
                "mode": "plan",
                "expected_revision": 0,
                "managed": true,
                "nodes": [{"key": "manual", "title": "wait"}]
            }),
            full.clone(),
        )
        .await
        .unwrap();
    let first = tools
        .call("task", serde_json::json!({"mode": "launch"}), full.clone())
        .await
        .unwrap();
    let run_id = first
        .split_whitespace()
        .find_map(|part| part.strip_prefix("run_id="))
        .unwrap()
        .to_string();

    let error = tools
        .call("task", serde_json::json!({"mode": "launch"}), full.clone())
        .await
        .expect_err("a graph may have only one live driver");
    assert!(error.to_string().contains("already has a live managed run"));

    tools
        .call(
            "task",
            serde_json::json!({"mode": "cancel", "run_id": run_id}),
            full,
        )
        .await
        .unwrap();
}

#[tokio::test]
async fn unauthorized_run_cancel_does_not_change_graph_state() {
    let session = Session::new_handle();
    let tools = registry();
    let owner = ctx(
        &session,
        "owner",
        Some(scopes(&[WORK_READ_SCOPE, WORK_WRITE_SCOPE])),
    );
    let stranger = ctx(
        &session,
        "stranger",
        Some(scopes(&[WORK_READ_SCOPE, WORK_WRITE_SCOPE])),
    );
    tools
        .call(
            "task",
            serde_json::json!({"mode": "init", "title": "owned"}),
            owner.clone(),
        )
        .await
        .unwrap();
    tools
        .call(
            "task",
            serde_json::json!({
                "mode": "plan",
                "expected_revision": 0,
                "managed": true,
                "nodes": [{"key": "manual", "title": "wait"}]
            }),
            owner.clone(),
        )
        .await
        .unwrap();
    let launched = tools
        .call("task", serde_json::json!({"mode": "launch"}), owner.clone())
        .await
        .unwrap();
    let run_id = launched
        .split_whitespace()
        .find_map(|part| part.strip_prefix("run_id="))
        .unwrap()
        .to_string();
    let before = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();

    tools
        .call(
            "task",
            serde_json::json!({"mode": "cancel", "run_id": run_id}),
            stranger,
        )
        .await
        .expect_err("only the graph owner may cancel its run");
    let after = session
        .work
        .read()
        .unwrap()
        .graphs
        .values()
        .next()
        .cloned()
        .unwrap();
    assert_eq!(after.revision, before.revision);
    assert_eq!(after.status, before.status);

    tools
        .call(
            "task",
            serde_json::json!({"mode": "cancel", "run_id": run_id}),
            owner,
        )
        .await
        .unwrap();
}
