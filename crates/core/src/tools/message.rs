//! The standalone `message` tool: durable, targeted messaging between
//! agents in a session. Complements `delegate send`'s parent/child-only
//! reach with `agent`, `label`, `siblings`, `fleet`, and `task` targets.
//!
//! Every message is written to a durable, append-only per-recipient
//! mailbox artifact (`artifact://mailbox/<agent_id>.log`) *before* it is
//! injected into the recipient's live mailbox, so a crash between "sent"
//! and "queued" cannot lose it — and so a target that is not currently
//! live (e.g. resumed later, or backgrounded) still receives it once it
//! next reads its mailbox or is resumed.

use chrono::Utc;
use schemars::JsonSchema;
use serde::Deserialize;

use crate::artifact::ArtifactSource;
use crate::persona::AGENT_MESSAGE_SCOPE;
use crate::session::SessionHandle;
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

#[derive(Debug, Clone, Copy, Deserialize, JsonSchema, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
enum Target {
    /// The caller's parent in the session hierarchy.
    #[default]
    Parent,
    /// A specific agent by id (`agent_id`).
    Agent,
    /// A specific agent by its unique human label (`label`).
    Label,
    /// Every other agent sharing the caller's parent.
    Siblings,
    /// Every other agent in the session.
    Fleet,
    /// The caller's parent, addressed through its task-assignment
    /// relationship rather than raw hierarchy (same resolution as
    /// `parent` today, but a distinct target so task-oriented callers do
    /// not need to reason about hierarchy directly).
    Task,
}

#[derive(Debug, Deserialize, JsonSchema)]
struct MessageArgs {
    /// Which relation to address. Defaults to the caller's parent.
    #[serde(default)]
    target: Target,
    /// Required when `target` is `agent`.
    #[serde(default)]
    agent_id: Option<String>,
    /// Required when `target` is `label`. Labels are unique per session;
    /// an ambiguous or unknown label fails explicitly rather than guessing.
    #[serde(default)]
    label: Option<String>,
    /// The message body.
    message: String,
}

fn require_scope(ctx: &ToolContext) -> Result<(), ToolError> {
    if ctx
        .allowed_scopes
        .as_ref()
        .is_some_and(|scopes| !scopes.contains(AGENT_MESSAGE_SCOPE))
    {
        return Err(ToolError::PermissionDenied {
            tool: "message".into(),
            required: vec![AGENT_MESSAGE_SCOPE.into()],
            allowed: ctx
                .allowed_scopes
                .clone()
                .unwrap_or_default()
                .into_iter()
                .collect(),
        });
    }

    Ok(())
}

pub fn register_message_tool(r: &ToolRegistry) -> &ToolRegistry {
    r.register(
        TypedTool::new(
            "message",
            "\
Send a durable, structured message to another agent in this session. Unlike \
`delegate send` (parent/child only), `message` can address a specific agent \
by id, by its unique human label, every sibling of the caller, or every \
agent in the session (\"fleet\").

Targets (set `target`):
  - parent (default): the caller's parent in the spawn hierarchy.
  - agent: a specific agent by id (`agent_id`).
  - label: a specific agent by its unique human label (`label`). An unknown \
    or ambiguous label fails explicitly rather than guessing.
  - siblings: every other agent sharing the caller's parent.
  - fleet: every other agent in the session.
  - task: the caller's parent, addressed through its task-assignment \
    relationship.

Every message is durably recorded before delivery, so it survives a crash \
between \"sent\" and \"queued\", and reaches a target that is not currently \
live once it next reads its mailbox.",
            |args: MessageArgs, ctx: ToolContext| Box::pin(async move { message(args, ctx).await }),
        )
        .with_required_scopes([AGENT_MESSAGE_SCOPE]),
    );
    r
}

/// Persist a message into the recipient's durable mailbox artifact before
/// any live delivery is attempted.
fn persist_message(
    session: &SessionHandle,
    sender: &str,
    target_id: &str,
    body: &str,
) -> Result<(), ToolError> {
    let path = format!("mailbox/{target_id}.log");
    let entry = format!("[{}] from={sender} {body}\n", Utc::now().to_rfc3339());
    session
        .artifacts
        .append(&path, &entry, Some(sender), ArtifactSource::Manual)
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(())
}

/// Append the audit record, enqueue the message in the target's durable FIFO,
/// persist the complete session, then wake a live target.
fn deliver(session: &SessionHandle, sender: &str, target_id: &str, body: &str) -> String {
    if let Err(error) = persist_message(session, sender, target_id, body) {
        return format!("{target_id}: failed to persist message: {error}");
    }
    let message = crate::Message::text(
        crate::MessageRole::User,
        format!("message from {sender}: {body}"),
    );
    match session.agent(target_id) {
        Some(target) => {
            target.submit_message(message);
            if let Err(error) = session.save() {
                return format!("{target_id}: failed to persist delivery: {error}");
            }
            session.wake_agent(target);
            format!("{target_id}: delivered and wake scheduled")
        }
        None if session.submit_to_unavailable_agent(target_id, message) => match session.save() {
            Ok(()) => {
                format!("{target_id}: queued (target not currently live; durable mailbox updated)")
            }
            Err(error) => format!("{target_id}: failed to persist delivery: {error}"),
        },
        None => format!("{target_id}: queued in audit log (target not currently restorable)"),
    }
}

async fn message(args: MessageArgs, ctx: ToolContext) -> Result<String, ToolError> {
    require_scope(&ctx)?;
    let session = ctx
        .session
        .clone()
        .ok_or_else(|| ToolError::Failed("message requires a session".into()))?;
    let sender = ctx.agent_id.clone();

    // Authorization: every target below is resolved from this session's own
    // persistent hierarchy — there is no cross-session addressing surface,
    // and a caller can never name an agent that isn't part of its own
    // session tree.
    let targets: Vec<String> = match args.target {
        Target::Parent | Target::Task => {
            let hierarchy = session.hierarchy.read().unwrap();
            let parent_id = hierarchy
                .get(&sender)
                .and_then(|node| node.parent_id.clone())
                .ok_or_else(|| {
                    ToolError::Failed("calling agent has no parent in this session".into())
                })?;
            vec![parent_id]
        }
        Target::Agent => {
            let agent_id = args.agent_id.clone().ok_or_else(|| {
                ToolError::InvalidArguments("target 'agent' requires 'agent_id'".into())
            })?;
            let hierarchy = session.hierarchy.read().unwrap();
            if !hierarchy.contains_key(&agent_id) {
                return Err(ToolError::Failed(format!(
                    "agent not found in this session: {agent_id}"
                )));
            }
            vec![agent_id]
        }
        Target::Label => {
            let label = args.label.clone().ok_or_else(|| {
                ToolError::InvalidArguments("target 'label' requires 'label'".into())
            })?;
            let hierarchy = session.hierarchy.read().unwrap();
            let matches: Vec<String> = hierarchy
                .iter()
                .filter(|(_, node)| node.label.as_deref() == Some(label.as_str()))
                .map(|(id, _)| id.clone())
                .collect();
            match matches.len() {
                0 => {
                    return Err(ToolError::Failed(format!(
                        "no agent in this session holds label '{label}'"
                    )));
                }
                1 => matches,
                n => {
                    return Err(ToolError::Failed(format!(
                        "label '{label}' is ambiguous: matched {n} agents"
                    )));
                }
            }
        }
        Target::Siblings => {
            let hierarchy = session.hierarchy.read().unwrap();
            let parent_id = hierarchy
                .get(&sender)
                .and_then(|node| node.parent_id.clone());
            hierarchy
                .iter()
                .filter(|(id, node)| id.as_str() != sender && node.parent_id == parent_id)
                .map(|(id, _)| id.clone())
                .collect()
        }
        Target::Fleet => {
            // Every other agent in the session's persistent hierarchy, not
            // only live runtime handles, so a fleet broadcast still records
            // a durable message for agents that are resumed later.
            let hierarchy = session.hierarchy.read().unwrap();
            hierarchy
                .keys()
                .filter(|id| id.as_str() != sender)
                .cloned()
                .collect()
        }
    };

    if targets.is_empty() {
        return Err(ToolError::Failed("no matching targets for message".into()));
    }

    let results: Vec<String> = targets
        .iter()
        .map(|target_id| deliver(&session, &sender, target_id, &args.message))
        .collect();
    Ok(results.join("\n"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::AgentConfig;
    use crate::providers::{Provider, ProviderError, ProviderEvent};
    use crate::tools::{ToolContext, ToolRegistry};
    use crate::{AgentState, LocalHost, MessagePart, MessageRole, Session};
    use futures::StreamExt;
    use std::sync::Arc;
    use tokio::sync::mpsc;

    struct NoopProvider;

    struct CaptureProvider {
        requests: mpsc::UnboundedSender<crate::ProviderRequest>,
    }

    #[async_trait::async_trait]
    impl Provider for CaptureProvider {
        fn id(&self) -> &str {
            "capture"
        }

        async fn stream(
            &self,
            request: crate::ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            self.requests.send(request).unwrap();
            Ok(futures::stream::iter([Ok(ProviderEvent::Done {
                reason: crate::StopReason::Stop,
            })])
            .boxed())
        }
    }

    #[async_trait::async_trait]
    impl Provider for NoopProvider {
        fn id(&self) -> &str {
            "test"
        }
        async fn stream(
            &self,
            _request: crate::ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            Ok(futures::stream::iter([]).boxed())
        }
    }

    fn ctx_for(session: &SessionHandle, agent_id: &str, scopes: &[&str]) -> ToolContext {
        ToolContext {
            workdir: std::env::temp_dir(),
            cancellation: tokio_util::sync::CancellationToken::new(),
            tool_call_id: "call-1".into(),
            agent_id: agent_id.into(),
            session_id: session.id.clone(),
            state: Arc::new(std::sync::RwLock::new(AgentState::default())),
            host: Arc::new(LocalHost::new()),
            session: Some(session.clone()),
            allowed_scopes: Some(scopes.iter().map(|s| s.to_string()).collect()),
        }
    }

    fn config() -> AgentConfig {
        AgentConfig {
            provider_id: "test".into(),
            model: "test-model".into(),
            ..Default::default()
        }
    }

    #[tokio::test]
    async fn parent_target_delivers_and_persists() {
        let session = Session::new_handle();
        let parent = session.spawn_agent(
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let child = session.spawn_subagent(
            &parent.id,
            None,
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );

        let registry = ToolRegistry::default();
        register_message_tool(&registry);
        let ctx = ctx_for(&session, &child.id, &[AGENT_MESSAGE_SCOPE]);
        let result = registry
            .call(
                "message",
                serde_json::json!({"target": "parent", "message": "hello parent"}),
                ctx,
            )
            .await
            .unwrap();
        assert!(result.contains("delivered"));
        tokio::time::timeout(std::time::Duration::from_secs(1), async {
            loop {
                if parent.history().iter().any(|message| {
                    message.content.iter().any(
                        |part| matches!(part, crate::MessagePart::Text(text) if text.contains("hello parent")),
                    )
                }) {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("live parent should consume its durable mailbox entry");
        let log = session
            .artifacts
            .read(&format!("mailbox/{}.log", parent.id))
            .unwrap();
        assert!(log.contains("hello parent"));
    }

    #[tokio::test]
    async fn parent_target_immediately_wakes_an_idle_parent() {
        let session = Session::new_handle();
        let (requests_tx, mut requests_rx) = mpsc::unbounded_channel();
        let parent = session.spawn_agent(
            Arc::new(CaptureProvider {
                requests: requests_tx,
            }),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let child = session.spawn_subagent(
            &parent.id,
            None,
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let registry = ToolRegistry::default();
        register_message_tool(&registry);

        registry
            .call(
                "message",
                serde_json::json!({"target": "parent", "message": "wake now"}),
                ctx_for(&session, &child.id, &[AGENT_MESSAGE_SCOPE]),
            )
            .await
            .unwrap();

        let request = tokio::time::timeout(std::time::Duration::from_secs(1), requests_rx.recv())
            .await
            .expect("idle parent must be woken immediately")
            .expect("capture provider remains live");
        assert!(request.messages.iter().any(|message| {
            message.content.iter().any(
                |part| matches!(part, crate::MessagePart::Text(text) if text.contains("wake now")),
            )
        }));
        assert!(parent.pending_messages().is_empty());
    }

    #[tokio::test]
    async fn failed_automatic_turn_restores_the_message_to_the_durable_mailbox() {
        struct FailingProvider;

        #[async_trait::async_trait]
        impl Provider for FailingProvider {
            fn id(&self) -> &str {
                "failing"
            }

            async fn stream(
                &self,
                _request: crate::ProviderRequest,
            ) -> Result<
                futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
                ProviderError,
            > {
                Err(ProviderError::Http("boom".into()))
            }
        }

        let session = Session::new_handle();
        let parent = session.spawn_agent(
            Arc::new(FailingProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let child = session.spawn_subagent(
            &parent.id,
            None,
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let registry = ToolRegistry::default();
        register_message_tool(&registry);
        registry
            .call(
                "message",
                serde_json::json!({"target": "parent", "message": "must survive"}),
                ctx_for(&session, &child.id, &[AGENT_MESSAGE_SCOPE]),
            )
            .await
            .unwrap();

        tokio::time::timeout(std::time::Duration::from_secs(1), async {
            loop {
                if parent
                    .pending_messages()
                    .iter()
                    .any(|m| m.contains("must survive"))
                {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("failed wake must restore its input");
        let record = session.snapshot_record().unwrap();
        let restored = record
            .agents
            .iter()
            .find(|agent| agent.id == parent.id)
            .unwrap();
        assert!(restored.mailbox.iter().any(|message| {
            message.content.iter().any(
                |part| matches!(part, crate::MessagePart::Text(text) if text.contains("must survive")),
            )
        }));
    }

    #[test]
    fn durable_snapshot_cannot_observe_between_mailbox_drain_and_history_insert() {
        let session = Session::new_handle();
        let parent = session.spawn_agent(
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        parent.submit("atomic");

        let (entered_tx, entered_rx) = std::sync::mpsc::channel();
        let (release_tx, release_rx) = std::sync::mpsc::channel();
        let snapshot_agent = parent.clone();
        let snapshot = std::thread::spawn(move || {
            snapshot_agent.durable_snapshot_paused_after_history(entered_tx, release_rx)
        });
        entered_rx.recv().unwrap();

        let move_agent = parent.clone();
        let moved = std::thread::spawn(move || move_agent.move_mailbox_to_history_for_test());
        std::thread::sleep(std::time::Duration::from_millis(10));
        release_tx.send(()).unwrap();

        let (history, _, mailbox) = snapshot.join().unwrap();
        moved.join().unwrap();
        assert!(
            history.iter().any(|message| {
                message
                    .content
                    .iter()
                    .any(|part| matches!(part, MessagePart::Text(text) if text == "atomic"))
            }) || mailbox.iter().any(|message| {
                message
                    .content
                    .iter()
                    .any(|part| matches!(part, MessagePart::Text(text) if text == "atomic"))
            })
        );
    }

    #[tokio::test]
    async fn failed_later_generation_retries_only_its_uncommitted_message() {
        struct LateFailureProvider {
            first_started: tokio::sync::Notify,
            release_first: std::sync::Mutex<Option<tokio::sync::oneshot::Receiver<()>>>,
            calls: std::sync::atomic::AtomicUsize,
        }

        #[async_trait::async_trait]
        impl Provider for LateFailureProvider {
            fn id(&self) -> &str {
                "late-failure"
            }

            async fn stream(
                &self,
                _request: crate::ProviderRequest,
            ) -> Result<
                futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
                ProviderError,
            > {
                let call = self.calls.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                if call == 0 {
                    self.first_started.notify_one();
                    let release = self.release_first.lock().unwrap().take().unwrap();
                    release.await.unwrap();
                    Ok(futures::stream::iter([Ok(ProviderEvent::Done {
                        reason: crate::StopReason::Stop,
                    })])
                    .boxed())
                } else {
                    Err(ProviderError::Auth("late boom".into()))
                }
            }
        }

        let session = Session::new_handle();
        let (release_tx, release_rx) = tokio::sync::oneshot::channel();
        let provider = Arc::new(LateFailureProvider {
            first_started: tokio::sync::Notify::new(),
            release_first: std::sync::Mutex::new(Some(release_rx)),
            calls: std::sync::atomic::AtomicUsize::new(0),
        });
        let parent = session.spawn_agent(
            provider.clone(),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        parent.submit("first");
        session.wake_agent(parent.clone());
        provider.first_started.notified().await;
        parent.submit("second");
        release_tx.send(()).unwrap();

        tokio::time::timeout(std::time::Duration::from_secs(1), async {
            loop {
                if parent.pending_messages() == ["second"] {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("rollback must preserve committed output and retry only the failed batch");

        let history = parent.history();
        assert!(history.iter().any(|message| {
            message.role == MessageRole::User
                && message
                    .content
                    .iter()
                    .any(|part| matches!(part, MessagePart::Text(text) if text == "first"))
        }));
        assert!(!history.iter().any(|message| {
            message.role == MessageRole::User
                && message
                    .content
                    .iter()
                    .any(|part| matches!(part, MessagePart::Text(text) if text == "second"))
        }));
    }

    #[test]
    fn concurrent_audit_appends_preserve_every_message() {
        let artifacts = Arc::new(crate::artifact::SessionArtifacts::new());
        let mut threads = Vec::new();
        for index in 0..16 {
            let artifacts = artifacts.clone();
            threads.push(std::thread::spawn(move || {
                artifacts
                    .append(
                        "mailbox/target.log",
                        &format!("message-{index}\n"),
                        Some("sender"),
                        ArtifactSource::Manual,
                    )
                    .unwrap();
            }));
        }
        for thread in threads {
            thread.join().unwrap();
        }
        let log = artifacts.read("mailbox/target.log").unwrap();
        for index in 0..16 {
            assert!(log.lines().any(|line| line == format!("message-{index}")));
        }
    }

    #[tokio::test]
    async fn parent_message_sent_during_a_turn_is_not_stranded_at_turn_exit() {
        struct BlockingProvider {
            requests: mpsc::UnboundedSender<crate::ProviderRequest>,
            release: std::sync::Mutex<Option<tokio::sync::oneshot::Receiver<()>>>,
        }

        #[async_trait::async_trait]
        impl Provider for BlockingProvider {
            fn id(&self) -> &str {
                "blocking"
            }

            async fn stream(
                &self,
                request: crate::ProviderRequest,
            ) -> Result<
                futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
                ProviderError,
            > {
                self.requests.send(request).unwrap();
                let release = self.release.lock().unwrap().take();
                if let Some(release) = release {
                    release.await.unwrap();
                }
                Ok(futures::stream::iter([
                    Ok(ProviderEvent::TextDelta { delta: "ok".into() }),
                    Ok(ProviderEvent::Done {
                        reason: crate::StopReason::Stop,
                    }),
                ])
                .boxed())
            }
        }

        let session = Session::new_handle();
        let (requests_tx, mut requests_rx) = mpsc::unbounded_channel();
        let (release_tx, release_rx) = tokio::sync::oneshot::channel();
        let parent = session.spawn_agent(
            Arc::new(BlockingProvider {
                requests: requests_tx,
                release: std::sync::Mutex::new(Some(release_rx)),
            }),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let child = session.spawn_subagent(
            &parent.id,
            None,
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let parent_task = {
            let parent = parent.clone();
            tokio::spawn(async move {
                parent
                    .prompt(
                        "first turn",
                        tokio_util::sync::CancellationToken::new(),
                        |_| {},
                    )
                    .await
            })
        };
        requests_rx.recv().await.expect("first request started");

        let registry = ToolRegistry::default();
        register_message_tool(&registry);
        registry
            .call(
                "message",
                serde_json::json!({"target": "parent", "message": "arrived during turn"}),
                ctx_for(&session, &child.id, &[AGENT_MESSAGE_SCOPE]),
            )
            .await
            .unwrap();
        release_tx.send(()).unwrap();
        parent_task.await.unwrap().unwrap();

        let second = tokio::time::timeout(std::time::Duration::from_secs(1), requests_rx.recv())
            .await
            .expect("queued message must trigger or join a follow-up request")
            .expect("capture channel remains open");
        assert!(second.messages.iter().any(|message| {
            message.content.iter().any(|part| {
                matches!(part, crate::MessagePart::Text(text) if text.contains("arrived during turn"))
            })
        }));
        assert!(parent.pending_messages().is_empty());
    }

    #[tokio::test]
    async fn label_target_is_unambiguous_and_missing_label_fails_explicitly() {
        let session = Session::new_handle();
        let parent = session.spawn_agent(
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let child = session.spawn_subagent(
            &parent.id,
            None,
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        session
            .set_agent_metadata(&child.id, Some("worker-a".into()), Default::default())
            .unwrap();

        let registry = ToolRegistry::default();
        register_message_tool(&registry);

        let ctx = ctx_for(&session, &parent.id, &[AGENT_MESSAGE_SCOPE]);
        let result = registry
            .call(
                "message",
                serde_json::json!({"target": "label", "label": "worker-a", "message": "hi"}),
                ctx,
            )
            .await
            .unwrap();
        assert!(result.contains("delivered"));

        let ctx = ctx_for(&session, &parent.id, &[AGENT_MESSAGE_SCOPE]);
        let err = registry
            .call(
                "message",
                serde_json::json!({"target": "label", "label": "no-such-label", "message": "hi"}),
                ctx,
            )
            .await
            .unwrap_err();
        assert!(matches!(err, ToolError::Failed(_)));
    }

    #[tokio::test]
    async fn message_requires_scope() {
        let session = Session::new_handle();
        let parent = session.spawn_agent(
            Arc::new(NoopProvider),
            Arc::new(ToolRegistry::default()),
            config(),
        );
        let registry = ToolRegistry::default();
        register_message_tool(&registry);
        let ctx = ctx_for(&session, &parent.id, &[]);
        let err = registry
            .call(
                "message",
                serde_json::json!({"target": "fleet", "message": "hi"}),
                ctx,
            )
            .await
            .unwrap_err();
        assert!(matches!(err, ToolError::PermissionDenied { .. }));
    }
}
