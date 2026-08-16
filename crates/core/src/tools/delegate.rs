//! The `delegate` tool: lets a running agent spawn a subagent to work on a
//! focused sub-task, optionally in the background.
//!
//! Modes mirror `bash`'s spawn/poll/wait pattern:
//! - `run` (default): spawn a subagent, run it to completion inline, return
//!   its final text. Simple one-shot delegation.
//! - `spawn`: create the subagent and start it in the background
//!   immediately, returning a `delegate_id` instead of waiting.
//! - `poll`: non-blocking status check for a `spawn`ed delegate — reports
//!   whether it's still running, plus its live transcript so far (there's
//!   no incremental streaming across the task boundary, but the subagent's
//!   history *is* readable mid-run since it's shared state).
//! - `wait`: block until a `spawn`ed delegate finishes, returning its
//!   result and forgetting the handle.
//!
//! `DelegateArgs` is a flat struct for the same schema reason documented in
//! `bash.rs`: tool-use models fill flat objects reliably, tagged unions less so.

use schemars::JsonSchema;
use serde::Deserialize;

use crate::agent::{Agent, AgentConfig};
use crate::types::{EffortMode, MessagePart, MessageRole};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::flex;

#[derive(Deserialize, JsonSchema, Default)]
#[serde(rename_all = "snake_case")]
enum Mode {
    #[default]
    Run,
    Spawn,
    Poll,
    Wait,
}

#[derive(Deserialize, JsonSchema)]
struct DelegateArgs {
    /// Which operation to perform. Defaults to `run` (spawn + wait inline).
    #[serde(default)]
    mode: Mode,
    /// The task to give the subagent. Required for `run`/`spawn`.
    #[serde(default)]
    prompt: Option<String>,
    /// Model id for the subagent. Defaults to the calling agent's model.
    #[serde(default)]
    model: Option<String>,
    /// Effort/reasoning mode name (from the model's published effort modes).
    /// Defaults to the calling agent's effort.
    #[serde(default)]
    effort: Option<String>,
    /// System prompt for the subagent. Defaults to the calling agent's.
    #[serde(default)]
    system_prompt: Option<String>,
    /// Delegate id from a prior `spawn`. Required for `poll`/`wait`.
    #[serde(default)]
    delegate_id: Option<String>,
    /// Max turns for the subagent. Defaults to the calling agent's.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    max_turns: Option<usize>,
}

fn require<'a, T>(field: &'a Option<T>, name: &str) -> Result<&'a T, ToolError> {
    field
        .as_ref()
        .ok_or_else(|| ToolError::InvalidArguments(format!("mode requires '{name}'")))
}

/// Build the subagent's config, inheriting from the parent unless overridden.
fn build_subagent_config(args: &DelegateArgs, parent: &AgentConfig) -> AgentConfig {
    let effort = match &args.effort {
        Some(name) => Some(EffortMode {
            name: name.clone(),
            thinking_budget_tokens: None,
            reasoning_effort: None,
        }),
        None => parent.effort.clone(),
    };

    AgentConfig {
        provider_id: parent.provider_id.clone(),
        model: args.model.clone().unwrap_or_else(|| parent.model.clone()),
        effort,
        system_prompt: args
            .system_prompt
            .clone()
            .or_else(|| parent.system_prompt.clone()),
        temperature: parent.temperature,
        max_tokens: parent.max_tokens,
        max_turns: args
            .max_turns
            .map(|n| n as u32)
            .unwrap_or(parent.max_turns),
        workdir: parent.workdir.clone(),
    }
}

/// Render a subagent's transcript back to the caller: the last assistant
/// text part, or a short note if it produced none (e.g. only tool calls
/// before stopping, or nothing yet if still running).
fn last_assistant_text(agent: &Agent) -> String {
    agent
        .history()
        .into_iter()
        .rev()
        .find(|m| m.role == MessageRole::Assistant)
        .map(|m| {
            m.content
                .into_iter()
                .filter_map(|part| match part {
                    MessagePart::Text(t) if !t.is_empty() => Some(t),
                    _ => None,
                })
                .collect::<Vec<_>>()
                .join("\n")
        })
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "(no assistant text yet)".to_string())
}

pub fn register_delegate_tool(r: &mut ToolRegistry) -> &mut ToolRegistry {
    r.register(TypedTool::new(
        "delegate",
        "\
Spawn a subagent to work on a focused sub-task, using the same provider/tools as you (config \
overridable: model, effort, system_prompt, max_turns). The subagent has its own tool loop and \
its own trajectory — it does not see your conversation history unless you put it in the prompt.

Modes (set `mode`):

- run (default): spawn a subagent, wait for it to finish, return its final text. Simple, \
  synchronous, one shot. Use this for most delegation.
- spawn: start the subagent in the background and return a `delegate_id` immediately, without \
  waiting. Use this to parallelize independent sub-tasks.
- poll: non-blocking check on a `spawn`ed delegate — reports running/done plus its transcript so \
  far.
- wait: block until a `spawn`ed delegate finishes; returns its final text and forgets the handle.

Requires prompt for run/spawn, delegate_id for poll/wait.",
        move |args: DelegateArgs, ctx: ToolContext| {
            Box::pin(async move {
                let session = ctx.session.clone().ok_or_else(|| {
                    ToolError::Failed(
                        "delegate is unavailable: this agent is not attached to a session"
                            .to_string(),
                    )
                })?;

                match args.mode {
                    Mode::Poll => {
                        let delegate_id = require(&args.delegate_id, "delegate_id")?;
                        let session = session.lock().await;
                        let (agent_id, finished) =
                            session.poll_delegate(delegate_id).await.ok_or_else(|| {
                                ToolError::Failed(format!(
                                    "unknown or already-collected delegate_id: {delegate_id}"
                                ))
                            })?;
                        let transcript = session
                            .agent(&agent_id)
                            .map(|a| last_assistant_text(&a))
                            .unwrap_or_default();
                        Ok(format!(
                            "status={} agent_id={agent_id}\n{transcript}",
                            if finished { "done" } else { "running" }
                        ))
                    }

                    Mode::Wait => {
                        let delegate_id = require(&args.delegate_id, "delegate_id")?.clone();
                        let handle = {
                            let session = session.lock().await;
                            session.take_delegate(&delegate_id).await
                        }
                        .map_err(ToolError::Failed)?;
                        let result = handle
                            .join
                            .await
                            .map_err(|e| ToolError::Failed(format!("delegate task panicked: {e}")))?;
                        result.map_err(|e| ToolError::Failed(format!("subagent failed: {e}")))
                    }

                    Mode::Run => {
                        let prompt = require(&args.prompt, "prompt")?.clone();
                        let agent = spawn(&session, &ctx.agent_id, &args).await?;
                        agent
                            .prompt(prompt, ctx.cancellation.clone(), |_| {})
                            .await
                            .map_err(|e| ToolError::Failed(format!("subagent failed: {e}")))
                    }

                    Mode::Spawn => {
                        let prompt = require(&args.prompt, "prompt")?.clone();
                        let agent = spawn(&session, &ctx.agent_id, &args).await?;
                        let cancellation = ctx.cancellation.clone();
                        let agent_for_task = agent.clone();
                        let join = tokio::spawn(async move {
                            agent_for_task.prompt(prompt, cancellation, |_| {}).await
                        });
                        let delegate_id = session
                            .lock()
                            .await
                            .register_delegate(agent.id.clone(), join)
                            .await;
                        Ok(format!("delegate_id={delegate_id}"))
                    }
                }
            }) as futures::future::BoxFuture<'static, Result<String, ToolError>>
        },
    ));
    r
}

/// Spawn the subagent under `parent_id`, reusing the parent's live provider
/// and tool registry (already resolved with credentials/base_url; no need
/// to touch a `ProviderManager` here).
async fn spawn(
    session: &std::sync::Arc<tokio::sync::Mutex<crate::session::Session>>,
    parent_id: &str,
    args: &DelegateArgs,
) -> Result<std::sync::Arc<Agent>, ToolError> {
    let mut session = session.lock().await;
    let parent = session
        .agent(parent_id)
.ok_or_else(|| ToolError::Failed("calling agent not found in its own session".into()))?;
    let parent_config = parent.config();
    let config = build_subagent_config(args, &parent_config);
    Ok(session.spawn_subagent(
        parent_id,
        None,
        parent.provider(),
        parent.tools(),
        config,
    ))
}