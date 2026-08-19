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
    let mut updated = session.artifacts.read(&path).unwrap_or_default();
    if !updated.is_empty() && !updated.ends_with('\n') {
        updated.push('\n');
    }
    updated.push_str(&format!(
        "[{}] from={sender} {body}\n",
        Utc::now().to_rfc3339()
    ));
    session
        .artifacts
        .write(&path, updated, Some(sender), ArtifactSource::Manual)
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(())
}

/// Persist then, if the target is a currently live agent handle, inject the
/// message into its mailbox for its next turn. A target that only exists as
/// a persistent session descriptor (not currently live) still receives the
/// durable record.
fn deliver(session: &SessionHandle, sender: &str, target_id: &str, body: &str) -> String {
    if let Err(error) = persist_message(session, sender, target_id, body) {
        return format!("{target_id}: failed to persist message: {error}");
    }
    match session.agent(target_id) {
        Some(target) => {
            target.submit(format!("message from {sender}: {body}"));
            format!("{target_id}: delivered")
        }
        None => format!("{target_id}: queued (target not currently live; durable mailbox updated)"),
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
    use crate::{AgentState, LocalHost, Session};
    use futures::StreamExt;
    use std::sync::Arc;

    struct NoopProvider;

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
        assert!(
            parent
                .pending_messages()
                .iter()
                .any(|m| m.contains("hello parent"))
        );
        let log = session
            .artifacts
            .read(&format!("mailbox/{}.log", parent.id))
            .unwrap();
        assert!(log.contains("hello parent"));
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
