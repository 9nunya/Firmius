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

use futures::FutureExt;

use schemars::JsonSchema;
use serde::Deserialize;
use uuid::Uuid;

use crate::agent::{Agent, AgentConfig, AgentError, PersonaRuntimeContext};
use crate::persona::{AGENT_MESSAGE_SCOPE, DELEGATION_SCOPE, PersonaManager};
use crate::types::{EffortMode, MessagePart, MessageRole};
use crate::user_settings::PreferredModel;
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::session_artifacts;

#[derive(Deserialize, JsonSchema, Default)]
#[serde(rename_all = "snake_case")]
enum Mode {
    #[default]
    Run,
    Spawn,
    Poll,
    Wait,
    Send,
}

#[derive(Deserialize, JsonSchema)]
struct DelegateArgs {
    /// One short phrase describing the subagent's task, e.g. "integrate auth
    /// flow" or "investigate flaky test". Required for `run` and `spawn`.
    /// Shown to the user in place of the raw prompt.
    #[serde(default)]
    intent: Option<String>,
    #[serde(default)]
    label: Option<String>,
    /// Optional durable task binding. When present, `task_id` is retained in
    /// the worker's assignment metadata and `planned_files` is advisory file
    /// scope for the optional child-local graph.
    #[serde(default)]
    task_id: Option<String>,
    #[serde(default)]
    planned_files: Vec<String>,
    /// Which operation to perform. Defaults to `run` (spawn + wait inline).
    #[serde(default)]
    mode: Mode,
    /// The task to give the subagent. Required for `run`/`spawn`.
    #[serde(default)]
    prompt: Option<String>,
    /// Model id for the subagent. Defaults to the calling agent's model.
    #[serde(default)]
    model: Option<String>,
    /// Persona id for the subagent. Required for `run`/`spawn`.
    #[serde(default)]
    persona: Option<String>,
    /// Effort/reasoning mode name (from the model's published effort modes).
    /// Defaults to the calling agent's effort.
    #[serde(default)]
    effort: Option<String>,
    /// Delegate id from a prior `spawn`. Required for `poll`/`wait`/`send`
    /// when messaging a child.
    #[serde(default)]
    delegate_id: Option<String>,
    /// Message body for `send`. Required for `send`.
    #[serde(default)]
    message: Option<String>,
    /// Which relation to send along: `parent` or `child`. `child` requires
    /// `delegate_id`; `parent` uses the caller's position in the session tree.
    #[serde(default)]
    target: Option<String>,
}

fn require<'a, T>(field: &'a Option<T>, name: &str) -> Result<&'a T, ToolError> {
    field
        .as_ref()
        .ok_or_else(|| ToolError::InvalidArguments(format!("mode requires '{name}'")))
}

fn parse_model_selection(model: &str, parent_provider_id: &str) -> (String, String) {
    if let Some((provider_id, model_id)) = model.split_once(':') {
        return (provider_id.to_string(), model_id.to_string());
    }
    if let Some((provider_id, model_id)) = model.split_once('/') {
        return (provider_id.to_string(), model_id.to_string());
    }
    (parent_provider_id.to_string(), model.to_string())
}

/// Build the subagent's config, inheriting from the parent unless overridden.
fn build_subagent_config(
    args: &DelegateArgs,
    parent: &AgentConfig,
    persona_manager: &PersonaManager,
    preferred: Option<&PreferredModel>,
) -> Result<AgentConfig, ToolError> {
    let persona_id = require(&args.persona, "persona")?;
    persona_manager.require(persona_id).map_err(|e| {
        ToolError::InvalidArguments(format!("invalid delegate persona '{persona_id}': {e}"))
    })?;

    let effort = match &args.effort {
        Some(name) => Some(EffortMode {
            name: name.clone(),
            thinking_budget_tokens: None,
            reasoning_effort: Some(name.clone()),
        }),
        None => preferred
            .and_then(|selection| selection.effort.as_ref())
            .map(|name| EffortMode {
                name: name.clone(),
                thinking_budget_tokens: None,
                reasoning_effort: Some(name.clone()),
            })
            .or_else(|| parent.effort.clone()),
    };

    let (provider_id, model) = args
        .model
        .as_deref()
        .map(|model| parse_model_selection(model, &parent.provider_id))
        .or_else(|| preferred.map(|value| (value.provider_id.clone(), value.model.clone())))
        .unwrap_or_else(|| (parent.provider_id.clone(), parent.model.clone()));

    Ok(AgentConfig {
        provider_id,
        model,
        effort,
        system_prompt: parent.system_prompt.clone(),
        persona: Some(persona_id.clone()),
        temperature: parent.temperature,
        max_tokens: parent.max_tokens,
        workdir: parent.workdir.clone(),
    })
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

/// Settle a task-bound child's assignment on natural termination (success,
/// failure, cancellation, or panic) for a child that never called `yield`
/// itself. No-op for unbound delegates, or if the assignment was already
/// settled (e.g. by `yield`) — `binding_for_agent` returns `None` once an
/// assignment is settled, so this is safe to call unconditionally after
/// `agent.prompt()` returns.
fn settle_natural_termination(
    session: &crate::session::SessionHandle,
    child_id: &str,
    status: crate::work::ExecutionStatus,
    outcome: crate::work::Outcome,
    summary: String,
) {
    let binding = {
        let state = session.work.read().unwrap();
        state.binding_for_agent(child_id).cloned()
    };
    let Some(binding) = binding else {
        return;
    };
    let child_id_for_closure = child_id.to_string();
    let result = session.mutate_work(move |state| {
        let expected = state.graph(binding.graph_id)?.revision;
        let auth = crate::work::AuthorizationContext {
            agent_id: child_id_for_closure.clone(),
            assignment_ids: [binding.assignment_id].into_iter().collect(),
            ..Default::default()
        };
        let result_id = state.settle_assignment(
            binding.graph_id,
            expected,
            &auth,
            binding.assignment_id,
            status,
            Some(outcome.clone()),
            summary.clone(),
            None,
            Vec::new(),
            Vec::new(),
            Vec::new(),
        )?;
        let record = state.graph(binding.graph_id)?.results[&result_id].clone();
        Ok((
            (),
            crate::work::WorkEvent::ResultRecorded {
                graph_id: binding.graph_id,
                result: record,
            },
        ))
    });
    if let Err(err) = result {
        eprintln!(
            "warning: failed to settle assignment for agent {child_id} on natural termination: {err}"
        );
    }
}

fn panic_message(panic: &(dyn std::any::Any + Send)) -> String {
    if let Some(s) = panic.downcast_ref::<&str>() {
        s.to_string()
    } else if let Some(s) = panic.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic".to_string()
    }
}

/// Run a spawned child's prompt to completion, catching a panic in the
/// prompt future so it can still settle the assignment rather than
/// poisoning the caller. Returns the same shape `agent.prompt()` would,
/// converting a panic into a synthetic `AgentError` after settlement.
async fn run_child_prompt(
    session: &crate::session::SessionHandle,
    agent: &Agent,
    prompt: String,
    cancellation: tokio_util::sync::CancellationToken,
) -> Result<String, AgentError> {
    let fut = agent.prompt(prompt, cancellation, |_| {});
    match std::panic::AssertUnwindSafe(fut).catch_unwind().await {
        Ok(result) => {
            match &result {
                Ok(text) => settle_natural_termination(
                    session,
                    &agent.id,
                    crate::work::ExecutionStatus::Succeeded,
                    crate::work::Outcome::Success,
                    text.clone(),
                ),
                Err(AgentError::Cancelled(partial)) => settle_natural_termination(
                    session,
                    &agent.id,
                    crate::work::ExecutionStatus::Interrupted,
                    crate::work::Outcome::Cancelled,
                    format!("child cancelled: {partial}"),
                ),
                Err(error) => settle_natural_termination(
                    session,
                    &agent.id,
                    crate::work::ExecutionStatus::Failed,
                    crate::work::Outcome::Failure,
                    format!("child failed: {error}"),
                ),
            }
            result
        }
        Err(panic) => {
            let message = panic_message(&*panic);
            settle_natural_termination(
                session,
                &agent.id,
                crate::work::ExecutionStatus::Failed,
                crate::work::Outcome::Failure,
                format!("child panicked: {message}"),
            );
            Err(AgentError::Trajectory(format!("child panicked: {message}")))
        }
    }
}

fn ensure_scope(ctx: &ToolContext, scope: &str) -> Result<(), ToolError> {
    if ctx
        .allowed_scopes
        .as_ref()
        .is_some_and(|allowed| !allowed.contains(scope))
    {
        return Err(ToolError::PermissionDenied {
            tool: "delegate".to_string(),
            required: vec![scope.to_string()],
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

/// Resolve the target agent for `send`. `target: "parent"` walks the session
/// hierarchy; `target: "child"` (the default when `delegate_id` is present)
/// resolves a backgrounded delegate handle.
async fn resolve_send_target(
    session: &crate::session::SessionHandle,
    ctx: &ToolContext,
    args: &DelegateArgs,
) -> Result<std::sync::Arc<Agent>, ToolError> {
    match args.target.as_deref() {
        Some("parent") => {
            let parent_id = session
                .hierarchy
                .read()
                .unwrap()
                .get(&ctx.agent_id)
                .and_then(|node| node.parent_id.clone())
                .ok_or_else(|| {
                    ToolError::Failed("calling agent is not in the session hierarchy".into())
                })?;
            session
                .agent(&parent_id)
                .ok_or_else(|| ToolError::Failed(format!("parent agent {parent_id} not found")))
        }
        Some("child") | None => {
            let delegate_id = require(&args.delegate_id, "delegate_id")?;
            let Some((agent_id, _)) = session.poll_delegate(delegate_id).await else {
                let detail = if session.was_delegate_collected(delegate_id).await {
                    "delegate already collected"
                } else {
                    "unknown delegate_id"
                };
                return Err(ToolError::Failed(format!("{detail}: {delegate_id}")));
            };
            session
                .agent(&agent_id)
                .ok_or_else(|| ToolError::Failed(format!("delegate agent {agent_id} not found")))
        }
        Some(other) => Err(ToolError::InvalidArguments(format!(
            "unknown target '{other}': expected 'parent' or 'child'"
        ))),
    }
}

pub fn register_delegate_tool(r: &ToolRegistry) -> &ToolRegistry {
    r.register(
        TypedTool::new(
            "delegate",
            "\
	Spawn a persona-scoped subagent to work on a focused sub-task, using the same provider/tools as you \
	(config overridable: model, effort). Provide a short `intent` phrase describing
	the task, e.g. \"integrate auth flow\" or \"investigate flaky test\".
	`intent` is required for `run` and `spawn` and is shown to the user while the
	subagent works. The subagent has its own tool loop and \
	its own trajectory — it does not see your conversation history unless you put it in the prompt.

	You must choose `persona` for run/spawn. Pick `coder` for implementation, code changes, build/test \
	work, `reviewer` for critique, bug finding, or verification, and `general` for broad research, \
	planning, summarization, or non-code tasks. Do not recursively delegate from a delegate unless the \
	chosen persona's tool scope explicitly allows delegation.

		Model precedence is explicit `model`, then the chosen persona's saved preferred model, then the \
		parent model. Plain model ids use the parent provider. `provider:model` and `provider/model` select \
		a different configured provider explicitly.

	Whenever a subagent finishes, its final text is automagically saved as a session artifact at \
	`artifact://<persona>-agent-result-N.md`. `run` and `wait` return that path directly; a \
	`spawn`ed delegate saves it on completion, after which `poll` reports done and `list \
	artifact://` shows it. You can read, grep, or edit result artifacts like any other artifact.

Modes (set `mode`):

- run (default): spawn a subagent, wait for it to finish, return its final text plus the \
  automagically saved result artifact (`artifact://<persona>-agent-result-N.md`). Simple, \
  synchronous, one shot. Use this for most delegation.
- spawn: start the subagent in the background and return a `delegate_id` immediately, without \
  waiting. Use this to parallelize independent sub-tasks; each result is automagically saved to \
  an artifact when that delegate finishes.
- poll: non-blocking check on a `spawn`ed delegate — reports running/done plus its transcript so \
  far. Once it reports done, the delegate's result artifact is available.
- wait: block until a `spawn`ed delegate finishes; returns its final text plus the automagically \
  saved result artifact and forgets the handle.
- send: deliver `message` to another agent without ending the caller's turn. Use it to message a \
  child (`delegate_id`) or your parent (`target=\"parent\"`). Messages are queued and never fail \
  because the target is busy.

	Requires prompt and persona for run/spawn, delegate_id for poll/wait. `send` requires `message`. \
	run/spawn/poll/wait require the `delegation` scope; `send` requires the `agent_message` scope. \
	`system_prompt` is not accepted; the chosen persona supplies the child system prompt.",
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
                            ensure_scope(&ctx, DELEGATION_SCOPE)?;
                            let delegate_id = require(&args.delegate_id, "delegate_id")?;
                            let Some((agent_id, finished)) =
                                session.poll_delegate(delegate_id).await
                            else {
                                let detail = if session.was_delegate_collected(delegate_id).await {
                                    "delegate already collected"
                                } else {
                                    "unknown delegate_id"
                                };
                                return Err(ToolError::Failed(format!("{detail}: {delegate_id}")));
                            };
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
                            ensure_scope(&ctx, DELEGATION_SCOPE)?;
                            let delegate_id = require(&args.delegate_id, "delegate_id")?.clone();
                            let handle = session.take_delegate(&delegate_id).await
                                .map_err(ToolError::Failed)?;
                            let result = handle.join.await.map_err(|e| {
                                ToolError::Failed(format!("delegate task panicked: {e}"))
                            })?;
                            result.map_err(|e| ToolError::Failed(format!("subagent failed: {e}")))
                        }

                        Mode::Run => {
                            ensure_scope(&ctx, DELEGATION_SCOPE)?;
                            let prompt = require(&args.prompt, "prompt")?.clone();
                            let persona = require(&args.persona, "persona")?.clone();
                            let _intent = require(&args.intent, "intent")?;
                            let agent =
                                spawn(&session, &ctx.agent_id, &ctx.tool_call_id, &args).await?;
                            let text = run_child_prompt(
                                &session,
                                &agent,
                                prompt,
                                ctx.cancellation.clone(),
                            )
                                .await
                                .map_err(|e| ToolError::Failed(format!("subagent failed: {e}")))?;
                            let store = session_artifacts(&ctx).await.ok_or_else(|| {
                                ToolError::Failed(
                                    "artifacts are unavailable: this agent is not attached to a session"
                                        .into(),
                                )
                            })?;
                            let artifact =
                                store.store_delegate_result(&persona, text.clone(), &agent.id, None);
                            Ok(format!("artifact://{}\n{text}", artifact.path))
                        }

                        Mode::Spawn => {
                            ensure_scope(&ctx, DELEGATION_SCOPE)?;
                            let prompt = require(&args.prompt, "prompt")?.clone();
                            let persona = require(&args.persona, "persona")?.clone();
                            let _intent = require(&args.intent, "intent")?;
                            let agent =
                                spawn(&session, &ctx.agent_id, &ctx.tool_call_id, &args).await?;
                            let store = session_artifacts(&ctx).await.ok_or_else(|| {
                                ToolError::Failed(
                                    "artifacts are unavailable: this agent is not attached to a session"
                                        .into(),
                                )
                            })?;
                            let cancellation = ctx.cancellation.clone();
                            let agent_for_task = agent.clone();
                            let agent_id = agent.id.clone();
                            let delegate_id = Uuid::new_v4().to_string();
                            let task_delegate_id = delegate_id.clone();
                            let session_for_task = session.clone();
                            let join = tokio::spawn(async move {
                                let result = run_child_prompt(
                                    &session_for_task,
                                    &agent_for_task,
                                    prompt,
                                    cancellation,
                                )
                                .await;
                                match result {
                                    Ok(text) => {
                                        let artifact = store.store_delegate_result(
                                            &persona,
                                            text.clone(),
                                            &agent_id,
                                            Some(&task_delegate_id),
                                        );
                                        Ok(format!("artifact://{}\n{text}", artifact.path))
                                    }
                                    Err(error) => Err(error),
                                }
                            });
                            session
                                .register_delegate_named(delegate_id.clone(), agent.id.clone(), join)
                                .await;
                            Ok(format!("delegate_id={delegate_id}"))
                        }

                        Mode::Send => {
                            ensure_scope(&ctx, AGENT_MESSAGE_SCOPE)?;
                            let message = require(&args.message, "message")?.clone();
                            let target = resolve_send_target(&session, &ctx, &args).await?;
                            target.submit(message);
                            let pending = target.pending_messages().len();
                            let note = if target.is_busy() {
                                "target is busy; message queued for its next turn"
                            } else {
                                "target is idle; message will be consumed when it next runs"
                            };
                            Ok(format!(
                                "queued target_agent_id={} pending={pending} {note}",
                                target.id
                            ))
                        }
                    }
                }) as futures::future::BoxFuture<'static, Result<String, ToolError>>
            },
        ),
    );
    r
}

/// Spawn the subagent under `parent_id`, reusing the parent's live provider
/// and tool registry (already resolved with credentials/base_url; no need
/// to touch a `ProviderManager` here).
async fn spawn(
    session: &crate::session::SessionHandle,
    parent_id: &str,
    tool_call_id: &str,
    args: &DelegateArgs,
) -> Result<std::sync::Arc<Agent>, ToolError> {
    let parent = session
        .agent(parent_id)
        .ok_or_else(|| ToolError::Failed("calling agent not found in its own session".into()))?;
    let parent_config = parent.config();
    let persona_manager = parent.persona_manager();
    let settings_handle = parent.user_settings_handle();
    let preferred = if args.model.is_none() {
        settings_handle.as_ref().and_then(|settings| {
            let settings = settings.lock().unwrap();
            args.persona
                .as_deref()
                .and_then(|persona| settings.preferred_model(persona).cloned())
        })
    } else {
        None
    };
    let config = build_subagent_config(args, &parent_config, &persona_manager, preferred.as_ref())?;
    let provider_manager = parent.provider_manager_handle();
    let provider = if config.provider_id == parent_config.provider_id {
        parent.provider()
    } else {
        let manager = provider_manager.as_ref().ok_or_else(|| {
            ToolError::Failed(format!(
                "delegate cannot use provider '{}': the calling agent has no attached ProviderManager",
                config.provider_id
            ))
        })?;
        manager
            .lock()
            .unwrap()
            .build(&config.provider_id)
            .map_err(|error| {
                ToolError::Failed(format!(
                    "delegate could not build provider '{}': {error}",
                    config.provider_id
                ))
            })?
    };
    let child = session.spawn_subagent_with_personas(
        parent_id,
        Some(tool_call_id.to_string()),
        provider,
        parent.tools(),
        config,
        persona_manager,
    );
    // From here on, the child is already registered in session.agents and
    // session.hierarchy. If anything below fails, compensate by removing
    // it rather than leaving an orphan agent with no assignment and no
    // reachable trajectory.
    let compensate = |session: &crate::session::SessionHandle, child_id: &str| {
        session.agents.write().unwrap().shift_remove(child_id);
        session.hierarchy.write().unwrap().remove(child_id);
    };
    if let (Some(provider_manager), Some(settings)) = (provider_manager, settings_handle) {
        child.attach_runtime(provider_manager, settings);
    }
    if args.label.is_some() || !args.planned_files.is_empty() || args.task_id.is_some() {
        let mut metadata = child.metadata();
        if !args.planned_files.is_empty() {
            metadata.insert(
                "planned_files".into(),
                serde_json::json!(args.planned_files),
            );
        }
        if let Some(task_id) = &args.task_id {
            metadata.insert("task_id".into(), serde_json::json!(task_id));
        }
        if let Err(error) = session.set_agent_metadata(&child.id, args.label.clone(), metadata) {
            compensate(session, &child.id);
            return Err(ToolError::Failed(error));
        }
    }
    // The assignment is durable before the child trajectory is launched.
    // Unbound legacy delegates preserve the previous behavior.
    if let Some(task_key) = args.task_id.clone() {
        let graph_id = match session
            .work
            .read()
            .unwrap()
            .active_graph_by_agent
            .get(parent_id)
            .copied()
        {
            Some(id) => id,
            None => {
                compensate(session, &child.id);
                return Err(ToolError::Failed(
                    "parent agent has no active work graph".into(),
                ));
            }
        };
        let node_id = match session
            .work
            .read()
            .unwrap()
            .graphs
            .get(&graph_id)
            .and_then(|graph| {
                graph
                    .nodes
                    .values()
                    .find(|node| node.key == task_key || node.id.to_string() == task_key)
                    .map(|node| node.id)
            }) {
            Some(id) => id,
            None => {
                compensate(session, &child.id);
                return Err(ToolError::Failed(format!("task not found: {task_key}")));
            }
        };
        let parent = parent_id.to_string();
        let child_id = child.id.clone();
        let planned_files = args.planned_files.clone();
        if let Err(error) = session.mutate_work(move |state| {
            let expected = state.graph(graph_id)?.revision;
            let auth = crate::work::AuthorizationContext {
                agent_id: parent.clone(),
                ..Default::default()
            };
            let (_, assignment_id) = state.assign(
                graph_id,
                expected,
                &auth,
                node_id,
                child_id.clone(),
                Some(parent),
                Some(task_key),
            )?;
            // Always create the child-local graph for a bound spawn,
            // even with no planned files: a bound worker with no active
            // graph cannot use `task view`, and skipping it here would
            // sever `parent_assignment_id`'s link back to this
            // assignment for graphs the child creates later via
            // `task init`/`task create` picking up a stale default.
            let mut local = crate::work::WorkGraph::new(
                format!("{} checklist", child_id),
                Some(child_id.clone()),
                crate::work::GraphMode::Advisory,
            );
            local.parent_assignment_id = Some(assignment_id);
            for (index, file) in planned_files.iter().enumerate() {
                let mut item = crate::work::WorkNode::new(
                    format!("planned-file-{}", index + 1),
                    format!("Work in {file}"),
                );
                item.file_scope.planned.push(file.clone());
                item.file_scope.advisory = true;
                local.view_order.push(item.id);
                local.nodes.insert(item.id, item);
            }
            let local_id = local.id;
            state.create_graph(local, None)?;
            state.active_graph_by_agent.insert(child_id, local_id);
            Ok((
                (),
                crate::work::WorkEvent::GraphChanged {
                    graph_id,
                    revision: state.graph(graph_id)?.revision,
                },
            ))
        }) {
            compensate(session, &child.id);
            return Err(ToolError::Failed(error));
        }
    }
    debug_assert_eq!(child.persona_context(), PersonaRuntimeContext::Delegate);
    Ok(child)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::providers::{Provider, ProviderError, ProviderEvent};
    use crate::{ApiType, ProviderManager, ProviderRequest, ProviderSchema, Session, StopReason};
    use futures::StreamExt;
    use std::sync::Arc;

    struct ParentProvider;

    #[async_trait::async_trait]
    impl Provider for ParentProvider {
        fn id(&self) -> &str {
            "parent"
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
        let directory =
            std::env::temp_dir().join(format!("firmius-delegate-persona-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("coder.md"),
            "---\nname: Coder\ntool_scopes: [fs_read, fs_write, processes]\nbackground: true\n---\nCode.",
        )
        .unwrap();
        Arc::new(PersonaManager::load_from(directory).unwrap())
    }

    /// A provider that always produces a bit of assistant text and stops —
    /// standing in for a child that finishes its work but never calls the
    /// `yield` tool.
    struct QuietChildProvider;

    #[async_trait::async_trait]
    impl Provider for QuietChildProvider {
        fn id(&self) -> &str {
            "parent"
        }

        async fn stream(
            &self,
            _request: ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            Ok(futures::stream::iter([
                Ok(ProviderEvent::TextDelta {
                    delta: "done, no yield called".into(),
                }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                }),
            ])
            .boxed())
        }
    }

    /// Item 12 (D5): a task-bound `delegate run` whose child finishes
    /// without ever calling `yield` must still settle the assignment (with
    /// a `Succeeded` result derived from the final text) and notify the
    /// parent — not leave the node stuck `Running` forever.
    #[tokio::test]
    async fn task_bound_delegate_run_settles_on_natural_termination() {
        use crate::tools::{ToolContext, ToolRegistry};
        use crate::work::{ExecutionStatus, GraphMode, WorkGraph, WorkNode};
        use crate::{AgentState, LocalHost};

        let personas = test_personas();
        let session = Session::new_handle();
        let parent = session.spawn_agent_with_personas(
            Arc::new(QuietChildProvider),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "parent".into(),
                model: "parent-model".into(),
                ..Default::default()
            },
            personas.clone(),
        );

        let mut graph = WorkGraph::new("checklist", Some(parent.id.clone()), GraphMode::Advisory);
        let graph_id = graph.id;
        let node = WorkNode::new("item-1", "do the thing");
        let node_id = node.id;
        graph.view_order.push(node_id);
        graph.nodes.insert(node_id, node);
        session
            .mutate_work(|state| {
                state.create_graph(graph, None)?;
                state
                    .active_graph_by_agent
                    .insert(parent.id.clone(), graph_id);
                Ok((
                    (),
                    crate::work::WorkEvent::GraphChanged {
                        graph_id,
                        revision: 1,
                    },
                ))
            })
            .unwrap();

        let registry = ToolRegistry::default();
        register_delegate_tool(&registry);
        let registry = Arc::new(registry);

        let ctx = ToolContext {
            workdir: std::env::temp_dir(),
            cancellation: tokio_util::sync::CancellationToken::new(),
            tool_call_id: "call-1".into(),
            agent_id: parent.id.clone(),
            session_id: session.id.clone(),
            state: Arc::new(std::sync::RwLock::new(AgentState::default())),
            host: Arc::new(LocalHost::new()),
            session: Some(session.clone()),
            allowed_scopes: Some(
                [crate::persona::DELEGATION_SCOPE.to_string()]
                    .into_iter()
                    .collect(),
            ),
        };

        registry
            .call_scoped(
                "delegate",
                serde_json::json!({
                    "mode": "run",
                    "intent": "do the thing",
                    "prompt": "please do the thing",
                    "persona": "coder",
                    "task_id": "item-1",
                }),
                ctx,
                Some(
                    &[crate::persona::DELEGATION_SCOPE.to_string()]
                        .into_iter()
                        .collect(),
                ),
            )
            .await
            .expect("task-bound delegate run should complete even without yield");

        let state = session.work.read().unwrap();
        let graph = state.graph(graph_id).unwrap();
        assert_eq!(
            graph.nodes[&node_id].status,
            ExecutionStatus::Succeeded,
            "node should be settled Succeeded, not stuck Running"
        );
        let assignment = graph
            .assignments
            .values()
            .find(|a| a.node_id == node_id)
            .expect("assignment exists");
        assert!(
            assignment.released_at.is_some(),
            "assignment must be released by natural termination, not left open"
        );
        assert!(
            state.binding_for_agent(&assignment.agent_id).is_none(),
            "settled assignment must clear the active binding"
        );
        let attempt = &graph.attempts[&assignment.attempt_id];
        let result = &graph.results[&attempt.result_id.expect("result recorded")];
        assert_eq!(result.summary, "done, no yield called");
        drop(state);

        assert!(
            parent
                .pending_messages()
                .iter()
                .any(|m| m.contains("settled assignment")),
            "parent should be notified of the child's natural termination"
        );
    }

    #[tokio::test]
    async fn saved_persona_preference_switches_child_provider_and_model() {
        let personas = test_personas();
        let session = Session::new_handle();
        let parent = session.spawn_agent_with_personas(
            Arc::new(ParentProvider),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "parent".into(),
                model: "parent-model".into(),
                ..Default::default()
            },
            personas,
        );
        let mut manager = ProviderManager::new();
        manager.register_schema(ProviderSchema {
            id: "other".into(),
            api_type: ApiType::OpenAI,
            base_url: Some("http://127.0.0.1:1".into()),
            api_key_env: None,
            models: vec![],
        });
        manager.set_api_key("other", "test-key");
        let manager = Arc::new(std::sync::Mutex::new(manager));
        let mut settings = crate::UserSettings::default();
        settings.set_preferred_model_and_effort(
            "coder",
            "other",
            "preferred-model",
            Some("high".into()),
        );
        let settings = Arc::new(std::sync::Mutex::new(settings));
        parent.attach_runtime(manager.clone(), settings.clone());

        let child = spawn(
            &session,
            &parent.id,
            "call-1",
            &DelegateArgs {
                mode: Mode::Run,
                prompt: Some("implement".into()),
                model: None,
                persona: Some("coder".into()),
                effort: None,
                intent: Some("test delegate".into()),
                delegate_id: None,
                message: None,
                target: None,
                label: None,
                task_id: None,
                planned_files: vec![],
            },
        )
        .await
        .unwrap();

        assert_eq!(child.config().provider_id, "other");
        assert_eq!(child.config().model, "preferred-model");
        assert_eq!(
            child
                .config()
                .effort
                .as_ref()
                .map(|effort| effort.name.as_str()),
            Some("high")
        );
        assert_eq!(child.config().persona.as_deref(), Some("coder"));
        assert_eq!(child.persona_context(), PersonaRuntimeContext::Delegate);
        assert!(child.provider_manager_handle().is_some());
        assert!(child.user_settings_handle().is_some());
    }

    #[test]
    fn explicit_model_selection_beats_saved_preference() {
        let personas = test_personas();
        let preferred = PreferredModel {
            provider_id: "preferred".into(),
            model: "preferred-model".into(),
            effort: Some("high".into()),
        };
        let config = build_subagent_config(
            &DelegateArgs {
                mode: Mode::Run,
                prompt: Some("implement".into()),
                model: Some("parent/explicit-model".into()),
                persona: Some("coder".into()),
                effort: None,
                intent: Some("explicit model test".into()),
                delegate_id: None,
                message: None,
                target: None,
                label: None,
                task_id: None,
                planned_files: vec![],
            },
            &AgentConfig {
                provider_id: "parent".into(),
                model: "parent-model".into(),
                ..Default::default()
            },
            &personas,
            Some(&preferred),
        )
        .unwrap();
        assert_eq!(config.provider_id, "parent");
        assert_eq!(config.model, "explicit-model");
        assert_eq!(config.effort.unwrap().name, "high");
    }
}
