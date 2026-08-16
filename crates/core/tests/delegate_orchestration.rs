//! Orchestration tests: the delegate lifecycle over the session bus, and
//! the seams where coordination frays — double-wait ambiguity, dangling
//! tool calls, hierarchy provenance.

use std::sync::Arc;

use firmius_core::{
    register_delegate_tool, AgentConfig, AgentEvent, Message, MessagePart, MessageRole,
    Provider, ProviderError, ProviderEvent, ProviderRequest, Session, SessionEvent, StopReason,
    ToolRegistry, validate_context,
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
                Ok(ProviderEvent::TextDelta { delta: "subagent output".into() }),
                Ok(ProviderEvent::Done { reason: StopReason::Stop }),
            ]
        } else if has_tool_message(&request) {
            vec![
                Ok(ProviderEvent::TextDelta { delta: "done".into() }),
                Ok(ProviderEvent::Done { reason: StopReason::Stop }),
            ]
        } else {
            vec![
                Ok(ProviderEvent::ToolCall {
                    id: "call_1".into(),
                    name: "delegate".into(),
                    args: r#"{"mode":"spawn","prompt":"do the work"}"#.into(),
                }),
                Ok(ProviderEvent::Done { reason: StopReason::ToolUse }),
            ]
        };
        Ok(futures::stream::iter(events).boxed())
    }
}

async fn scripted_session() -> (Arc<Mutex<Session>>, Arc<firmius_core::Agent>) {
    let provider: Arc<dyn Provider> = Arc::new(ScriptedProvider);
    let mut tools = ToolRegistry::default();
    register_delegate_tool(&mut tools);
    let tools = Arc::new(tools);

    let session = Arc::new(Mutex::new(Session::new()));
    session.lock().await.bind_self(&session);
    let config = AgentConfig {
        provider_id: "scripted".into(),
        model: "scripted-model".into(),
        max_turns: 8,
        ..Default::default()
    };
    let parent = session.lock().await.spawn_agent(provider, tools, config);
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

    // One backgrounded delegate is tracked; hierarchy recorded — but the
    // provenance field is dead: ToolContext carries no tool-call id, so
    // spawned_via_tool_call_id is always None.
    let delegates = session.lock().await.active_delegates().await;
    assert_eq!(delegates.len(), 1);
    let delegate_id = delegates[0].delegate_id.clone();
    let subagent_id = delegates[0].agent_id.clone();
    assert_ne!(subagent_id, parent_id);
    {
        let s = session.lock().await;
        let node = &s.hierarchy[&subagent_id];
        assert_eq!(node.parent_id.as_deref(), Some(parent_id.as_str()));
        assert!(node.spawned_via_tool_call_id.is_none());
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
    assert_eq!(result, "subagent output");

    // The bus carried the subagent's events even though its prompt ran with
    // an empty `|_| {}` observer — visibility no longer depends on callers.
    let deadline = tokio::time::Instant::now() + tokio::time::Duration::from_secs(2);
    loop {
        let seen = collected
            .lock()
            .await
            .iter()
            .any(|e| {
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
async fn double_wait_is_indistinguishable_from_never_existing() {
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
    s.wait_delegate(&delegate_id).await.expect("first wait").expect("ok result");
    let again = s.wait_delegate(&delegate_id).await.unwrap_err();
    let never = s
        .wait_delegate("00000000-0000-0000-0000-000000000000")
        .await
        .unwrap_err();
    // Same error kind for both: "already collected" and "never existed"
    // cannot be told apart.
    assert!(again.starts_with("unknown delegate_id:"));
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