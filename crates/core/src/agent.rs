use crate::host::{Host, LocalHost};
use crate::providers::{Provider, ProviderError, ProviderEvent};
use crate::tools::{ToolContext, ToolRegistry};
use crate::types::{
    validate_context, Context, EffortMode, Message, MessagePart, MessageRole, ProviderRequest,
    StopReason, Usage,
};
use futures::StreamExt;
use std::path::PathBuf;
use std::sync::{Arc, RwLock, Weak};
use tokio::sync::broadcast;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

// ---------------------------------------------------------------------------
// Agent state — shared with tools via ToolContext
// ---------------------------------------------------------------------------

/// Mutable state shared between the agent and any tool that needs to inspect
/// or modify the trajectory.
#[derive(Debug, Default)]
pub struct AgentState {
    pub history: Context,
    pub usage: Usage,
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    #[error("provider error: {0}")]
    Provider(#[from] ProviderError),
    #[error("invalid trajectory: {0}")]
    Trajectory(String),
    /// Cancelled with partial text accumulated so far.
    #[error("cancelled after producing: {0}")]
    Cancelled(String),
    #[error("hit max turns ({0}) without stopping")]
    MaxTurns(u32),
    #[error("agent is busy: a turn is running (or a mutation was attempted mid-turn)")]
    Busy,
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct AgentConfig {
    /// Which registered provider (via `ProviderManager`) this agent talks
    /// to. Persisted so a session can be resumed against the exact same
    /// provider, not whichever one happens to be first at hand.
    pub provider_id: String,
    pub model: String,
    pub system_prompt: Option<String>,
    pub temperature: Option<f32>,
    pub max_tokens: Option<u32>,
    pub max_turns: u32,
    pub workdir: PathBuf,
    /// Reasoning/effort mode in effect (OpenAI `reasoning_effort` string,
    /// Anthropic `thinking_budget_tokens`, or both) — reuses the same
    /// `EffortMode` type providers publish in their `ModelInfo`.
    pub effort: Option<EffortMode>,
}

impl Default for AgentConfig {
    fn default() -> Self {
        Self {
            provider_id: String::new(),
            model: String::new(),
            system_prompt: None,
            temperature: None,
            max_tokens: None,
            max_turns: 32,
            workdir: std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")),
            effort: None,
        }
    }
}

// ---------------------------------------------------------------------------
// Stop policy
// ---------------------------------------------------------------------------

/// Decides whether the ReAct loop should stop after a generation.
pub trait StopPolicy: Send + Sync {
    fn should_stop(&self, latest: &Message, reason: StopReason) -> bool;
}

pub struct DefaultStopPolicy;

impl StopPolicy for DefaultStopPolicy {
    fn should_stop(&self, latest: &Message, reason: StopReason) -> bool {
        if matches!(
            reason,
            StopReason::MaxTokens | StopReason::Error | StopReason::Cancelled
        ) {
            return true;
        }
        !latest
            .content
            .iter()
            .any(|part| matches!(part, MessagePart::ToolCall { .. }))
    }
}

// ---------------------------------------------------------------------------
// Agent
// ---------------------------------------------------------------------------

/// A reusable ReAct agent: prompt it repeatedly, it keeps a running trajectory,
/// manages the tool-call loop, and stops per its `StopPolicy`.
    pub struct Agent {
    pub id: String,
    pub session_id: String,
    state: Arc<RwLock<AgentState>>,
    /// Interior-mutable so reconfiguration (`set_provider`) can swap backends
    /// between turns without rebuilding the agent. Always kept in agreement
    /// with `config.provider_id` — change both via `set_provider` only.
    provider: RwLock<Arc<dyn Provider>>,
    tools: Arc<ToolRegistry>,
    /// Interior-mutable between turns via `update_config`/`set_provider`;
    /// both refuse to run while `busy` is held (i.e. mid-turn).
    config: RwLock<AgentConfig>,
    stop_policy: Arc<dyn StopPolicy>,
    /// This agent's OS boundary. One per agent, so a background process
    /// spawned via `bash` in one turn is still pollable/waitable in the next.
    host: Arc<dyn Host>,
    /// Handle back to the owning `Session`, so tools (e.g. `delegate`) can
    /// spawn subagents. `Weak` because `Session.agents` holds `Arc<Agent>` —
    /// a strong ref here would be a cycle that never frees. Set post-
    /// construction via `attach_session` once the caller has wrapped the
    /// `Session` in `Arc<tokio::sync::Mutex<_>>`.
    session_handle: RwLock<Option<Weak<tokio::sync::Mutex<crate::session::Session>>>>,
    /// Session event bus this agent tees every emitted `AgentEvent` into.
    /// `None` for agents that live outside a session (e.g. unit tests).
    bus: RwLock<Option<broadcast::Sender<crate::session::SessionEvent>>>,
    /// Held for the entire duration of `prompt()`. Serializes turns on this
    /// agent, and lets between-turn mutators refuse to run mid-turn.
    busy: tokio::sync::Mutex<()>,
}

/// Streamed observation of what the agent is doing, for a UI/CLI to render.
#[derive(Debug, Clone)]
pub enum AgentEvent {
    Thinking(String),
    Text(String),
    ToolCallDelta {
        id: String,
        name_delta: String,
        args_delta: String,
    },
    ToolCallStarted {
        name: String,
        args: String,
    },
    ToolResult {
        name: String,
        ok: bool,
        content: String,
    },
    Usage(Usage),
    TurnFinished,
}

impl Agent {
    pub fn new(
        provider: Arc<dyn Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
        session_id: impl Into<String>,
    ) -> Self {
        let mut state = AgentState::default();
        if let Some(system) = &config.system_prompt {
            state
                .history
                .push(Message::text(MessageRole::System, system.clone()));
        }
        Self {
            id: Uuid::new_v4().to_string(),
            session_id: session_id.into(),
            state: Arc::new(RwLock::new(state)),
            provider: RwLock::new(provider),
            tools,
            config: RwLock::new(config),
            stop_policy: Arc::new(DefaultStopPolicy),
            host: Arc::new(LocalHost::new()),
            session_handle: RwLock::new(None),
            bus: RwLock::new(None),
            busy: tokio::sync::Mutex::new(()),
        }
    }

    /// Wire this agent to its owning session so its tools can reach
    /// `Session::spawn_subagent` (e.g. the `delegate` tool). Idempotent;
    /// call again to re-attach after a resume.
    pub fn attach_session(&self, session: Arc<tokio::sync::Mutex<crate::session::Session>>) {
    *self.session_handle.write().unwrap() = Some(Arc::downgrade(&session));
    }

    /// Attach the session event bus: every event this agent emits is tee'd
    /// into it alongside the per-prompt observer. Wired by `Session`;
    /// idempotent.
    pub fn attach_bus(&self, tx: broadcast::Sender<crate::session::SessionEvent>) {
        *self.bus.write().unwrap() = Some(tx);
    }

    /// Use a custom `Host` instead of the default `LocalHost` (e.g. a
    /// sandboxed or remote implementation).
    pub fn with_host(mut self, host: Arc<dyn Host>) -> Self {
        self.host = host;
        self
    }

    /// This agent's `Host` handle. Shared with every `ToolContext` built for
    /// its tool calls, and reusable directly (e.g. for a UI polling loop).
    pub fn host(&self) -> Arc<dyn Host> {
        self.host.clone()
    }

    pub fn with_stop_policy(mut self, policy: Arc<dyn StopPolicy>) -> Self {
        self.stop_policy = policy;
        self
    }

    /// Replace the trajectory wholesale, e.g. when resuming a persisted
    /// session. Overrides whatever `system_prompt` seeded in `new()`.
    pub fn with_history(self, history: Context) -> Self {
        self.state.write().unwrap().history = history;
        self
    }

    /// Override the generated id, e.g. to restore the exact id an agent had
    /// before a save/resume round-trip (so hierarchy references and
    /// external ids like a saved `delegate_id` stay valid).
    pub fn with_id(mut self, id: impl Into<String>) -> Self {
        self.id = id.into();
        self
    }

    /// Read-only snapshot of the current history.
    pub fn history(&self) -> Context {
        self.state.read().unwrap().history.clone()
    }

    /// Read-only snapshot of accumulated usage.
    pub fn usage(&self) -> Usage {
        self.state.read().unwrap().usage
    }

    /// Shared handle to agent state — pass this to tools via ToolContext.
    pub fn state_handle(&self) -> Arc<RwLock<AgentState>> {
        self.state.clone()
    }

    /// Snapshot of this agent's configuration (model, provider id, effort…).
    /// To change it between turns, use `update_config` / `set_provider`.
    pub fn config(&self) -> AgentConfig {
        self.config.read().unwrap().clone()
    }

    /// This agent's provider handle — reused as-is when spawning a subagent
    /// via the `delegate` tool (same account/base_url/api_key already
    /// resolved, no need to re-resolve through a `ProviderManager`).
    pub fn provider(&self) -> Arc<dyn Provider> {
        self.provider.read().unwrap().clone()
    }

    /// This agent's tool registry — reused as-is when spawning a subagent.
    pub fn tools(&self) -> Arc<ToolRegistry> {
        self.tools.clone()
    }

    // ------------------------------------------------------------------
    // Between-turn mutation — guarded by `busy`
    // ------------------------------------------------------------------

    /// Apply a mutation to this agent's config between turns. Refuses while a
    /// turn is running (`AgentError::Busy`). Safe because every mutable field
    /// is re-read into `ProviderRequest` on each generation.
    pub fn update_config(&self, f: impl FnOnce(&mut AgentConfig)) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        f(&mut self.config.write().unwrap());
        Ok(())
    }

    /// Swap provider handle and `config.provider_id` atomically — the two
    /// must always agree, so they change in one guarded operation.
    pub fn set_provider(
        &self,
        provider_id: impl Into<String>,
        provider: Arc<dyn Provider>,
    ) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        self.config.write().unwrap().provider_id = provider_id.into();
        *self.provider.write().unwrap() = provider;
        Ok(())
    }

    /// Indexes of user messages — the only legal cut points for `rewind`,
    /// since `validate_context` glues each tool-result message to the
    /// assistant message preceding it.
    fn user_turn_boundaries(history: &Context) -> Vec<usize> {
        history
            .iter()
            .enumerate()
            .filter(|(_, m)| m.role == MessageRole::User)
            .map(|(i, _)| i)
            .collect()
    }

    /// Rewind the trajectory by `turns` complete user turns, cutting at a
    /// user-message boundary so tool call/result pairing is never broken.
    /// A count beyond available history clamps to right after the leading
    /// system messages (which is what powers `/clear`). Returns the number
    /// of messages removed. Refuses mid-turn.
    pub fn rewind(&self, turns: usize) -> Result<usize, AgentError> {
        if turns == 0 {
            return Ok(0);
        }
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        let mut s = self.state.write().unwrap();
        let cuts = Self::user_turn_boundaries(&s.history);
        let at = match cuts.iter().rev().nth(turns - 1) {
            Some(&i) => i,
            None => s
                .history
                .iter()
                .position(|m| m.role != MessageRole::System)
                .unwrap_or(s.history.len()),
        };
        let removed = s.history.len() - at;
        s.history.truncate(at);
        Ok(removed)
    }

    /// Run the loop until the stop policy fires, returning the final text.
    /// On cancellation the error carries the partial text accumulated so far.
    pub async fn prompt(
        &self,
        user_input: impl Into<String>,
        cancellation: CancellationToken,
    mut observer: impl FnMut(AgentEvent),
    ) -> Result<String, AgentError> {
        // Serialize turns on this agent; this same guard is what the
        // between-turn mutators check before touching config or history.
        let _turn_guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;

        self.state
            .write()
            .unwrap()
            .history
            .push(Message::text(MessageRole::User, user_input.into()));

        // Config can't change while we hold `busy`, so one snapshot is valid
        // for every iteration of the loop.
        let config = self.config.read().unwrap().clone();
        let bus = self.bus.read().unwrap().clone();
        // Tee every event into the session bus (when attached) and the
        // caller's observer.
        let mut emit = |event: AgentEvent| {
            if let Some(tx) = &bus {
                let _ = tx.send(crate::session::SessionEvent {
                    agent_id: self.id.clone(),
                    event: event.clone(),
                });
            }
            observer(event);
        };

        let mut final_text = String::new();

        for _turn in 0..config.max_turns {
            if cancellation.is_cancelled() {
                return Err(AgentError::Cancelled(final_text));
            }
            {
                let history = &self.state.read().unwrap().history;
                validate_context(history).map_err(AgentError::Trajectory)?;
            }

            let request = ProviderRequest {
                model: config.model.clone(),
                messages: self.state.read().unwrap().history.clone(),
                tools: self.tools.definitions(),
                temperature: config.temperature,
                max_tokens: config.max_tokens,
                reasoning_effort: config
                    .effort
                    .as_ref()
                    .and_then(|e| e.reasoning_effort.clone()),
                thinking_budget_tokens: config
                    .effort
                    .as_ref()
                    .and_then(|e| e.thinking_budget_tokens),
            };

            let (assistant, reason, turn_text, turn_usage) = self
                .run_generation(request, &cancellation, &mut emit)
                .await?;
            {
                let mut s = self.state.write().unwrap();
                s.usage = turn_usage;
                s.history.push(assistant.clone());
            }
            final_text = turn_text;
            emit(AgentEvent::TurnFinished);

            if self.stop_policy.should_stop(&assistant, reason) {
                return Ok(final_text);
            }

            // Execute every tool call from this generation, append results.
            let calls: Vec<(String, String, String)> = assistant
                .content
                .iter()
                .filter_map(|part| match part {
                    MessagePart::ToolCall { id, name, args } => {
                        Some((id.clone(), name.clone(), args.clone()))
                    }
                    _ => None,
                })
                .collect();

            let mut results = Vec::new();
            for (call_id, name, args) in calls {
                if cancellation.is_cancelled() {
                    return Err(AgentError::Cancelled(final_text));
                }
                emit(AgentEvent::ToolCallStarted {
                    name: name.clone(),
                    args: args.clone(),
                });
                let parsed = serde_json::from_str(&args).unwrap_or(serde_json::Value::Null);
                let ctx = ToolContext {
                    workdir: config.workdir.clone(),
                    cancellation: cancellation.clone(),
                    agent_id: self.id.clone(),
                    session_id: self.session_id.clone(),
                    state: self.state.clone(),
                    host: self.host.clone(),
                    session: self
                        .session_handle
                        .read()
                        .unwrap()
                        .as_ref()
                        .and_then(|w| w.upgrade()),
                };
                let (content, ok) = match self.tools.call(&name, parsed, ctx).await {
                    Ok(output) => (output, true),
                    Err(e) => (e.to_string(), false),
                };
                emit(AgentEvent::ToolResult {
                    name,
                    ok,
                    content: content.clone(),
                });
                results.push(MessagePart::ToolResult {
                    id: call_id,
                    content,
                    ok,
                });
            }
            self.state
                .write()
                .unwrap()
                .history
            .push(Message::tool_results(results));
        }

        Err(AgentError::MaxTurns(config.max_turns))
    }

    /// Consume one provider stream into a single assistant message.
    /// On cancellation, returns the partial content instead of discarding it.
    async fn run_generation(
        &self,
        request: ProviderRequest,
        cancellation: &CancellationToken,
        observer: &mut impl FnMut(AgentEvent),
    ) -> Result<(Message, StopReason, String, Usage), AgentError> {
        // Snapshot the provider handle first; never hold the lock across await.
        let provider = self.provider.read().unwrap().clone();
        let mut stream = provider.stream(request).await?;

        let mut text = String::new();
        let mut thinking = String::new();
        let mut signature: Option<String> = None;
        let mut tool_calls: Vec<MessagePart> = Vec::new();
        let mut reason = StopReason::Stop;
        let mut usage = Usage::default();

        loop {
            tokio::select! {
                biased;
                _ = cancellation.cancelled() => {
                    let partial = build_assistant_message(thinking, signature, text.clone(), tool_calls);
                    return Ok((partial, StopReason::Cancelled, text, usage));
                }
                next = stream.next() => {
                    let Some(event) = next else { break; };
                    match event? {
                        ProviderEvent::TextDelta { delta } => {
                            observer(AgentEvent::Text(delta.clone()));
                            text.push_str(&delta);
                        }
                        ProviderEvent::ThinkingDelta { delta, signature: sig } => {
                            if !delta.is_empty() {
                                observer(AgentEvent::Thinking(delta.clone()));
                                thinking.push_str(&delta);
                            }
                            if sig.is_some() {
                                signature = sig;
                            }
                        }
                        ProviderEvent::ToolCallDelta { id, name_delta, args_delta, .. } => {
                            observer(AgentEvent::ToolCallDelta {
                                id: id.unwrap_or_default(),
                                name_delta,
                                args_delta,
                            })
                        }
                        ProviderEvent::ToolCall { id, name, args } => {
                            tool_calls.push(MessagePart::ToolCall { id, name, args });
                        }
                        ProviderEvent::Usage { usage: u } => {
                            usage = u;
                            observer(AgentEvent::Usage(u));
                        }
                        ProviderEvent::Done { reason: r } => reason = r,
                    }
                }
            }
        }

        let assistant = build_assistant_message(thinking, signature, text.clone(), tool_calls);
        Ok((assistant, reason, text, usage))
    }
}

fn build_assistant_message(
    thinking: String,
    signature: Option<String>,
    text: String,
    tool_calls: Vec<MessagePart>,
) -> Message {
    let mut content = Vec::new();
    if !thinking.is_empty() || signature.is_some() {
        content.push(MessagePart::Thinking {
            content: thinking,
            signature,
        });
    }
    if !text.is_empty() {
        content.push(MessagePart::Text(text));
    }
    content.extend(tool_calls);
    if content.is_empty() {
        content.push(MessagePart::Text(String::new()));
    }
    Message {
        role: MessageRole::Assistant,
        content,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Emits nothing: enough to build an `Agent` for state-only tests
    /// (rewind, config mutation) without a real backend.
    struct DummyProvider;

    #[async_trait::async_trait]
    impl Provider for DummyProvider {
        fn id(&self) -> &str {
            "dummy"
        }
        async fn stream(
            &self,
            _request: ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            Ok(futures::stream::empty().boxed())
        }
    }

    fn agent_with_history(history: Context) -> Agent {
        let config = AgentConfig {
            provider_id: "dummy".into(),
            model: "dummy-model".into(),
            ..Default::default()
        };
        Agent::new(
            Arc::new(DummyProvider),
            Arc::new(ToolRegistry::default()),
            config,
            "test-session",
        )
        .with_history(history)
    }

    /// [system, user, assistant(+tool call), tool result, user, assistant]
    fn two_turn_history() -> Context {
        vec![
            Message::text(MessageRole::System, "sys"),
            Message::text(MessageRole::User, "one"),
            Message {
                role: MessageRole::Assistant,
                content: vec![
                    MessagePart::Text("ans".into()),
                    MessagePart::ToolCall {
                        id: "c1".into(),
                        name: "bash".into(),
                        args: "{}".into(),
                    },
                ],
            },
            Message::tool_results([MessagePart::ToolResult {
                id: "c1".into(),
                content: "ok".into(),
                ok: true,
            }]),
            Message::text(MessageRole::User, "two"),
            Message::text(MessageRole::Assistant, "final"),
        ]
    }

    #[test]
    fn rewind_cuts_at_user_boundary() {
        let agent = agent_with_history(two_turn_history());
        let removed = agent.rewind(1).unwrap();
        assert_eq!(removed, 2);
        let history = agent.history();
        assert_eq!(history.len(), 4);
        assert!(matches!(history.last().unwrap().role, MessageRole::Tool));
        assert!(validate_context(&history).is_ok());
    }

    #[test]
    fn rewind_clamps_to_system_prefix() {
        let agent = agent_with_history(two_turn_history());
        let removed = agent.rewind(99).unwrap();
        assert_eq!(removed, 5);
        let history = agent.history();
        assert_eq!(history.len(), 1);
        assert!(matches!(history[0].role, MessageRole::System));
    }

    #[test]
    fn rewind_zero_is_a_noop() {
        let agent = agent_with_history(two_turn_history());
        assert_eq!(agent.rewind(0).unwrap(), 0);
        assert_eq!(agent.history().len(), 6);
    }

    #[test]
    fn update_config_mutates_between_turns() {
        let agent = agent_with_history(Vec::new());
        agent
            .update_config(|c| c.model = "other-model".into())
            .unwrap();
        assert_eq!(agent.config().model, "other-model");
    }
}