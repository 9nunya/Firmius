//! Orchestration tests: the delegate lifecycle over the session bus, and
//! the seams where coordination frays — double-wait ambiguity, dangling
//! tool calls, hierarchy provenance.

use std::sync::Arc;

use firmius_core::persistence::session_path;
use firmius_core::{
    AgentConfig, AgentEvent, AgentState, ApiType, ArtifactSource, LocalHost, Message, MessagePart,
    MessageRole, PersonaManager, Provider, ProviderError, ProviderEvent, ProviderManager,
    ProviderRequest, ProviderSchema, Session, SessionEvent, StopReason, ToolContext, ToolError,
    ToolRegistry, load_session_record, register_delegate_tool, validate_context,
};
use futures::StreamExt;
use tokio::sync::Mutex;
use tokio_util::sync::CancellationToken;

/// A provider scripted by request content:
/// - parent, first generation (no tool results yet) → call `delegate` spawn
/// - parent, second generation (tool result present) → "done", stop
/// - subagent (its user prompt contains "work")      → "subagent output", stop
struct ScriptedProvider;

fn last_user_text(req: &ProviderRequest) -> String {
    req.messages
        .iter()
        .rev()
        .find(|m| m.role == MessageRole::User)
        .map(|m| {
            m.content
                .iter()
                .filter_map(|p| match p {
                    MessagePart::Text(t) => Some(t.as_str()),
                    _ => None,
                })
                .collect::<Vec<_>>()
                .join(" ")
        })
        .unwrap_or_default()
}

fn has_tool_message(req: &ProviderRequest) -> bool {
    req.messages.iter().any(|m| m.role == MessageRole::Tool)
}

#[async_trait::async_trait]
impl Provider for ScriptedProvider {
    fn id(&self) -> &str {
        "scripted"
    }

    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<
        futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
        ProviderError,
    > {
        let events = if last_user_text(&request).contains("work") {
            vec![
                Ok(ProviderEvent::TextDelta {
                    delta: "subagent output".into(),
                }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                }),
            ]
        } else if has_tool_message(&request) {
            vec![
                Ok(ProviderEvent::TextDelta {
                    delta: "done".into(),
                }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                }),
            ]
        } else {
            vec![
                Ok(ProviderEvent::ToolCall {
                    id: "call_1".into(),
                    name: "delegate".into(),
                    args: r#"{"mode":"spawn","prompt":"do the work","persona":"coder"}"#.into(),
                }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::ToolUse,
                }),
            ]
        };
        Ok(futures::stream::iter(events).boxed())
    }
}

async fn scripted_session() -> (Arc<Mutex<Session>>, Arc<firmius_core::Agent>) {
    let provider: Arc<dyn Provider> = Arc::new(ScriptedProvider);
    let tools = ToolRegistry::default();
    register_delegate_tool(&tools);
    let tools = Arc::new(tools);

    let session = Arc::new(Mutex::new(Session::new()));
    session.lock().await.bind_self(&session);
    let config = AgentConfig {
        provider_id: "scripted".into(),
        model: "scripted-model".into(),
        ..Default::default()
    };
    let personas = Arc::new(
        PersonaManager::load_from_dir(
            std::env::temp_dir().join(format!("firmius-delegate-test-{}", uuid::Uuid::new_v4())),
        )
        .expect("stock personas"),
    );
    let parent = session
        .lock()
        .await
        .spawn_agent_with_personas(provider, tools, config, personas);
    (session, parent)
}

#[tokio::test]
async fn delegate_spawn_lifecycle_over_bus() {
    let (session, parent) = scripted_session().await;
    let parent_id = parent.id.clone();

    // Collect everything the session bus carries.
    let rx = session.lock().await.subscribe();
    let collected: Arc<Mutex<Vec<SessionEvent>>> = Arc::new(Mutex::new(Vec::new()));
    {
        let collected = collected.clone();
        tokio::spawn(async move {
            let mut rx = rx;
            while let Ok(ev) = rx.recv().await {
                collected.lock().await.push(ev);
            }
        });
    }

    let final_text = parent
        .prompt("kick it off", CancellationToken::new(), |_| {})
        .await
        .expect("parent turn should succeed");
    assert_eq!(final_text, "done");

    // One backgrounded delegate is tracked; hierarchy recorded with full
    // provenance — the delegate tool passes its own tool-call id through.
    let delegates = session.lock().await.active_delegates().await;
    assert_eq!(delegates.len(), 1);
    let delegate_id = delegates[0].delegate_id.clone();
    let subagent_id = delegates[0].agent_id.clone();
    assert_ne!(subagent_id, parent_id);
    {
        let s = session.lock().await;
        let node = &s.hierarchy[&subagent_id];
        assert_eq!(node.parent_id.as_deref(), Some(parent_id.as_str()));
        assert_eq!(node.spawned_via_tool_call_id.as_deref(), Some("call_1"));
    }

    // Collect the result the way the delegate tool does: take the handle
    // and join OUTSIDE the session lock. (`session.lock().await.wait_delegate(..)`
    // would hold the mutex across the join — a deadlock for any delegate
    // that calls a session-aware tool.)
    let handle = session
        .lock()
        .await
        .take_delegate(&delegate_id)
        .await
        .expect("delegate registered");
    let result = handle.join.await.expect("task alive").expect("subagent ok");
    assert_eq!(
        result,
        "artifact://coder-agent-result-1.md\nsubagent output"
    );
    // The result was also recorded as a session artifact, readable through the
    // shared store (and, by extension, the artifact-aware tools).
    let s = session.lock().await;
    assert_eq!(
        s.artifacts.read("coder-agent-result-1.md").unwrap(),
        "subagent output"
    );

    // The bus carried the subagent's events even though its prompt ran with
    // an empty `|_| {}` observer — visibility no longer depends on callers.
    let deadline = tokio::time::Instant::now() + tokio::time::Duration::from_secs(2);
    loop {
        let seen = collected.lock().await.iter().any(|e| {
            e.agent_id == subagent_id
                && matches!(&e.event, AgentEvent::Text(t) if t.contains("subagent output"))
        });
        if seen {
            break;
        }
        assert!(
            tokio::time::Instant::now() < deadline,
            "subagent events never reached the session bus"
        );
        tokio::time::sleep(tokio::time::Duration::from_millis(20)).await;
    }
}

#[tokio::test]
async fn collected_and_unknown_delegate_errors_are_distinct() {
    let (session, parent) = scripted_session().await;
    parent
        .prompt("kick it off", CancellationToken::new(), |_| {})
        .await
        .expect("parent turn");

    let delegate_id = session.lock().await.active_delegates().await[0]
        .delegate_id
        .clone();

    let s = session.lock().await;
    // Safe in this test only because the scripted subagent never locks the
    // session; a session-aware delegate would deadlock inside wait_delegate.
    s.wait_delegate(&delegate_id)
        .await
        .expect("first wait")
        .expect("ok result");
    let again = s.wait_delegate(&delegate_id).await.unwrap_err();
    let never = s
        .wait_delegate("00000000-0000-0000-0000-000000000000")
        .await
        .unwrap_err();
    // Distinct errors: a collected id is not the same as one that never
    // existed.
    assert!(again.starts_with("delegate already collected:"));
    assert!(never.starts_with("unknown delegate_id:"));
}

#[test]
fn dangling_tool_calls_pass_validation() {
    // A trajectory interrupted mid-tool-execution ends on an assistant
    // message whose tool calls never received results. validate_context
    // accepts it — but provider APIs reject such contexts, so resuming an
    // interrupted session and prompting that agent yields a 400, not a
    // clean error.
    let ctx = vec![
        Message::text(MessageRole::System, "sys"),
        Message::text(MessageRole::User, "hi"),
        Message {
            role: MessageRole::Assistant,
            content: vec![MessagePart::ToolCall {
                id: "c1".into(),
                name: "bash".into(),
                args: "{}".into(),
            }],
        },
    ];
    assert!(validate_context(&ctx).is_ok());
}

#[test]
fn session_save_and_resume_preserves_agents_history_and_hierarchy() {
    let provider: Arc<dyn Provider> = Arc::new(ScriptedProvider);
    let tools = Arc::new(ToolRegistry::default());
    let mut session = Session::new();

    let parent = session.spawn_agent(
        provider.clone(),
        tools.clone(),
        AgentConfig {
            provider_id: "scripted".into(),
            model: "parent-model".into(),
            ..Default::default()
        },
    );
    let child = session.spawn_subagent(
        &parent.id,
        Some("call_1".into()),
        provider,
        tools.clone(),
        AgentConfig {
            provider_id: "scripted".into(),
            model: "child-model".into(),
            ..Default::default()
        },
    );

    parent.state_handle().write().unwrap().history = vec![
        Message::text(MessageRole::System, "system"),
        Message::text(
            MessageRole::User,
            "This is a deliberately long first request that should be truncated when the session title is derived from it.",
        ),
    ];
    child.state_handle().write().unwrap().history =
        vec![Message::text(MessageRole::User, "child request")];

    session
        .artifacts
        .write(
            "note.md",
            "artifact content",
            Some(&parent.id),
            ArtifactSource::Manual,
        )
        .expect("artifact write");

    let session_id = session.id.clone();
    session.save().expect("session should save");
    let path = session_path(&session_id);
    let record = load_session_record(&session_id).expect("saved record should load");

    assert_eq!(record.id, session_id);
    assert_eq!(record.agents.len(), 2);
    assert!(
        record
            .title
            .as_ref()
            .is_some_and(|title| title.ends_with('…'))
    );
    assert!(
        record
            .title
            .as_ref()
            .is_some_and(|title| title.chars().count() == 61)
    );
    assert_eq!(record.hierarchy[&parent.id].parent_id, None);
    assert_eq!(
        record.hierarchy[&child.id].parent_id.as_deref(),
        Some(parent.id.as_str())
    );
    assert_eq!(
        record.hierarchy[&child.id]
            .spawned_via_tool_call_id
            .as_deref(),
        Some("call_1")
    );
    assert_eq!(record.artifacts.len(), 1);
    assert_eq!(record.artifacts[0].path, "note.md");
    assert_eq!(record.artifacts[0].content, "artifact content");

    let mut manager = ProviderManager::new();
    manager.register_schema(ProviderSchema {
        id: "scripted".into(),
        api_type: ApiType::OpenAI,
        base_url: Some("http://127.0.0.1:1".into()),
        api_key_env: None,
        models: vec![],
    });
    manager.set_api_key("scripted", "test-key");
    let resumed = Session::from_record(record, &manager, tools);

    assert_eq!(resumed.id, session_id);
    assert_eq!(resumed.title, session.title);
    assert_eq!(resumed.agents.len(), 2);
    assert_eq!(
        resumed.agent(&parent.id).unwrap().config().model,
        "parent-model"
    );
    assert_eq!(
        resumed.agent(&parent.id).unwrap().history()[1],
        Message::text(
            MessageRole::User,
            "This is a deliberately long first request that should be truncated when the session title is derived from it.",
        )
    );
    assert_eq!(
        resumed.hierarchy[&child.id].parent_id.as_deref(),
        Some(parent.id.as_str())
    );
    assert_eq!(
        resumed.hierarchy[&child.id]
            .spawned_via_tool_call_id
            .as_deref(),
        Some("call_1")
    );
    assert_eq!(
        resumed.artifacts.read("note.md").unwrap(),
        "artifact content"
    );

    let _ = std::fs::remove_file(path);
}

// ---------------------------------------------------------------------------
// delegate send
// ---------------------------------------------------------------------------

fn delegate_ctx(
    session: &Arc<Mutex<Session>>,
    agent_id: &str,
    allowed_scopes: Option<std::collections::HashSet<String>>,
) -> ToolContext {
    ToolContext {
        workdir: std::env::temp_dir(),
        cancellation: CancellationToken::new(),
        tool_call_id: "send-call".into(),
        agent_id: agent_id.to_string(),
        session_id: "test-session".into(),
        state: Arc::new(std::sync::RwLock::new(AgentState::default())),
        host: Arc::new(LocalHost::new()),
        session: Some(session.clone()),
        allowed_scopes,
    }
}

#[tokio::test]
async fn delegate_send_queues_messages_across_the_tree() {
    let (session, parent) = scripted_session().await;
    let parent_id = parent.id.clone();
    parent
        .prompt("kick it off", CancellationToken::new(), |_| {})
        .await
        .expect("parent turn");

    let (delegate_id, child_id) = {
        let session = session.lock().await;
        let delegates = session.active_delegates().await;
        (
            delegates[0].delegate_id.clone(),
            delegates[0].agent_id.clone(),
        )
    };

    // Parent -> child, by delegate id. The child may have already finished, but
    // the message is still queued rather than erroring with `Busy`.
    let out = parent
        .tools()
        .call(
            "delegate",
            serde_json::json!({
                "mode": "send",
                "delegate_id": delegate_id,
                "message": "hi child"
            }),
            delegate_ctx(&session, &parent_id, None),
        )
        .await
        .expect("parent->child send");
    assert!(out.starts_with("queued target_agent_id="), "{out}");

    // Child -> parent, by tree position.
    let out = parent
        .tools()
        .call(
            "delegate",
            serde_json::json!({
                "mode": "send",
                "target": "parent",
                "message": "hi parent"
            }),
            delegate_ctx(&session, &child_id, None),
        )
        .await
        .expect("child->parent send");
    assert!(out.starts_with("queued target_agent_id="), "{out}");

    let child = session.lock().await.agent(&child_id).unwrap();
    assert_eq!(child.pending_messages(), vec!["hi child".to_string()]);
    let parent_agent = session.lock().await.agent(&parent_id).unwrap();
    assert_eq!(
        parent_agent.pending_messages(),
        vec!["hi parent".to_string()]
    );
}

#[tokio::test]
async fn delegate_send_requires_agent_message_scope() {
    let (session, parent) = scripted_session().await;
    let mut allowed = std::collections::HashSet::new();
    allowed.insert("fs_read".to_string());

    let error = parent
        .tools()
        .call(
            "delegate",
            serde_json::json!({
                "mode": "send",
                "target": "parent",
                "message": "hi"
            }),
            delegate_ctx(&session, &parent.id, Some(allowed)),
        )
        .await
        .expect_err("send without agent_message scope must be denied");
    assert!(
        matches!(error, ToolError::PermissionDenied { .. }),
        "{error:?}"
    );
}

#[tokio::test]
async fn delegate_run_requires_delegation_scope() {
    let (session, parent) = scripted_session().await;
    let mut allowed = std::collections::HashSet::new();
    allowed.insert("agent_message".to_string());

    let error = parent
        .tools()
        .call(
            "delegate",
            serde_json::json!({
                "mode": "run",
                "prompt": "do the work",
                "persona": "coder"
            }),
            delegate_ctx(&session, &parent.id, Some(allowed)),
        )
        .await
        .expect_err("run without delegation scope must be denied");
    assert!(
        matches!(error, ToolError::PermissionDenied { .. }),
        "{error:?}"
    );
}
