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
    provider: Arc<dyn Provider>,
    tools: Arc<ToolRegistry>,
    config: AgentConfig,
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
            provider,
            tools,
            config,
            stop_policy: Arc::new(DefaultStopPolicy),
            host: Arc::new(LocalHost::new()),
            session_handle: RwLock::new(None),
        }
    }

    /// Wire this agent to its owning session so its tools can reach
    /// `Session::spawn_subagent` (e.g. the `delegate` tool). Idempotent;
    /// call again to re-attach after a resume.
    pub fn attach_session(&self, session: Arc<tokio::sync::Mutex<crate::session::Session>>) {
        *self.session_handle.write().unwrap() = Some(Arc::downgrade(&session));
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

    /// This agent's configuration (model, provider id, effort, etc).
    /// Read-only: build a new `Agent` to change it.
    pub fn config(&self) -> &AgentConfig {
        &self.config
    }

    /// This agent's provider handle — reused as-is when spawning a subagent
    /// via the `delegate` tool (same account/base_url/api_key already
    /// resolved, no need to re-resolve through a `ProviderManager`).
    pub fn provider(&self) -> Arc<dyn Provider> {
        self.provider.clone()
    }

    /// This agent's tool registry — reused as-is when spawning a subagent.
    pub fn tools(&self) -> Arc<ToolRegistry> {
        self.tools.clone()
    }

    /// Run the loop until the stop policy fires, returning the final text.
    /// On cancellation the error carries the partial text accumulated so far.
    pub async fn prompt(
        &self,
        user_input: impl Into<String>,
        cancellation: CancellationToken,
        mut observer: impl FnMut(AgentEvent),
    ) -> Result<String, AgentError> {
        self.state
            .write()
            .unwrap()
            .history
            .push(Message::text(MessageRole::User, user_input.into()));

        let mut final_text = String::new();

        for _turn in 0..self.config.max_turns {
            if cancellation.is_cancelled() {
                return Err(AgentError::Cancelled(final_text));
            }
            {
                let history = &self.state.read().unwrap().history;
                validate_context(history).map_err(AgentError::Trajectory)?;
            }

            let request = ProviderRequest {
                model: self.config.model.clone(),
                messages: self.state.read().unwrap().history.clone(),
                tools: self.tools.definitions(),
                temperature: self.config.temperature,
                max_tokens: self.config.max_tokens,
                reasoning_effort: self
                    .config
                    .effort
                    .as_ref()
                    .and_then(|e| e.reasoning_effort.clone()),
                thinking_budget_tokens: self
                    .config
                    .effort
                    .as_ref()
                    .and_then(|e| e.thinking_budget_tokens),
            };

            let (assistant, reason, turn_text, turn_usage) = self
                .run_generation(request, &cancellation, &mut observer)
                .await?;
            {
                let mut s = self.state.write().unwrap();
                s.usage = turn_usage;
                s.history.push(assistant.clone());
            }
            final_text = turn_text;
            observer(AgentEvent::TurnFinished);

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
                observer(AgentEvent::ToolCallStarted {
                    name: name.clone(),
                    args: args.clone(),
                });
                let parsed = serde_json::from_str(&args).unwrap_or(serde_json::Value::Null);
                let ctx = ToolContext {
                    workdir: self.config.workdir.clone(),
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
                observer(AgentEvent::ToolResult {
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

        Err(AgentError::MaxTurns(self.config.max_turns))
    }

    /// Consume one provider stream into a single assistant message.
    /// On cancellation, returns the partial content instead of discarding it.
    async fn run_generation(
        &self,
        request: ProviderRequest,
        cancellation: &CancellationToken,
        observer: &mut impl FnMut(AgentEvent),
    ) -> Result<(Message, StopReason, String, Usage), AgentError> {
        let mut stream = self.provider.stream(request).await?;

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
