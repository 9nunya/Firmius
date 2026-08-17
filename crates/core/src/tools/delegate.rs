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

use crate::agent::{Agent, AgentConfig, PersonaRuntimeContext};
use crate::persona::PersonaManager;
use crate::types::{EffortMode, MessagePart, MessageRole};
use crate::user_settings::PreferredModel;
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

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
    /// Persona id for the subagent. Required for `run`/`spawn`.
    #[serde(default)]
    persona: Option<String>,
    /// Effort/reasoning mode name (from the model's published effort modes).
    /// Defaults to the calling agent's effort.
    #[serde(default)]
    effort: Option<String>,
    /// Delegate id from a prior `spawn`. Required for `poll`/`wait`.
    #[serde(default)]
    delegate_id: Option<String>,
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

pub fn register_delegate_tool(r: &mut ToolRegistry) -> &mut ToolRegistry {
    r.register(
        TypedTool::new(
            "delegate",
            "\
	Spawn a persona-scoped subagent to work on a focused sub-task, using the same provider/tools as you \
	(config overridable: model, effort). The subagent has its own tool loop and \
	its own trajectory — it does not see your conversation history unless you put it in the prompt.

	You must choose `persona` for run/spawn. Pick `coder` for implementation, code changes, build/test \
	work, `reviewer` for critique, bug finding, or verification, and `general` for broad research, \
	planning, summarization, or non-code tasks. Do not recursively delegate from a delegate unless the \
	chosen persona's tool scope explicitly allows delegation.

		Model precedence is explicit `model`, then the chosen persona's saved preferred model, then the \
		parent model. Plain model ids use the parent provider. `provider:model` and `provider/model` select \
		a different configured provider explicitly.

Modes (set `mode`):

- run (default): spawn a subagent, wait for it to finish, return its final text. Simple, \
  synchronous, one shot. Use this for most delegation.
- spawn: start the subagent in the background and return a `delegate_id` immediately, without \
  waiting. Use this to parallelize independent sub-tasks.
- poll: non-blocking check on a `spawn`ed delegate — reports running/done plus its transcript so \
  far.
- wait: block until a `spawn`ed delegate finishes; returns its final text and forgets the handle.

	Requires prompt and persona for run/spawn, delegate_id for poll/wait. `system_prompt` is not accepted; \
	the chosen persona supplies the child system prompt.",
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
                            let delegate_id = require(&args.delegate_id, "delegate_id")?.clone();
                            let handle = {
                                let session = session.lock().await;
                                session.take_delegate(&delegate_id).await
                            }
                            .map_err(ToolError::Failed)?;
                            let result = handle.join.await.map_err(|e| {
                                ToolError::Failed(format!("delegate task panicked: {e}"))
                            })?;
                            result.map_err(|e| ToolError::Failed(format!("subagent failed: {e}")))
                        }

                        Mode::Run => {
                            let prompt = require(&args.prompt, "prompt")?.clone();
                            require(&args.persona, "persona")?;
                            let agent =
                                spawn(&session, &ctx.agent_id, &ctx.tool_call_id, &args).await?;
                            agent
                                .prompt(prompt, ctx.cancellation.clone(), |_| {})
                                .await
                                .map_err(|e| ToolError::Failed(format!("subagent failed: {e}")))
                        }

                        Mode::Spawn => {
                            let prompt = require(&args.prompt, "prompt")?.clone();
                            require(&args.persona, "persona")?;
                            let agent =
                                spawn(&session, &ctx.agent_id, &ctx.tool_call_id, &args).await?;
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
        )
        .with_required_scopes(["delegation"]),
    );
    r
}

/// Spawn the subagent under `parent_id`, reusing the parent's live provider
/// and tool registry (already resolved with credentials/base_url; no need
/// to touch a `ProviderManager` here).
async fn spawn(
    session: &std::sync::Arc<tokio::sync::Mutex<crate::session::Session>>,
    parent_id: &str,
    tool_call_id: &str,
    args: &DelegateArgs,
) -> Result<std::sync::Arc<Agent>, ToolError> {
    let mut session = session.lock().await;
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
    if let (Some(provider_manager), Some(settings)) = (provider_manager, settings_handle) {
        child.attach_runtime(provider_manager, settings);
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
    use tokio::sync::Mutex;

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

    #[tokio::test]
    async fn saved_persona_preference_switches_child_provider_and_model() {
        let personas = test_personas();
        let session = Arc::new(Mutex::new(Session::new()));
        session.lock().await.bind_self(&session);
        let parent = session.lock().await.spawn_agent_with_personas(
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
                delegate_id: None,
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
                delegate_id: None,
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
