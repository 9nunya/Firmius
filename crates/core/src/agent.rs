use crate::host::{Host, LocalHost};
use crate::persona::{PersonaError, PersonaManager};
use crate::providers::{Provider, ProviderError, ProviderEvent, manager::ProviderManager};
use crate::tools::{ToolContext, ToolRegistry};
use crate::types::{
    Context, EffortMode, Message, MessagePart, MessageRole, ProviderRequest, StopReason, Usage,
    repair_dangling_tool_calls, validate_context,
};
use futures::StreamExt;
use std::collections::{HashSet, VecDeque};
use std::path::PathBuf;
use std::sync::{Arc, RwLock, Weak};
use tokio::sync::broadcast;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

use crate::user_settings::UserSettings;

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
    #[error("agent is busy: a turn is running (or a mutation was attempted mid-turn)")]
    Busy,
    #[error("persona error: {0}")]
    Persona(#[from] PersonaError),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PersonaUse {
    Main,
    Delegate,
}

/// Backwards-compatible name used by the initial persona implementation.
pub type PersonaRuntimeContext = PersonaUse;

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
    /// Active persona id. `None` preserves legacy unrestricted tools and
    /// `system_prompt` behavior. `Some` fails closed if the persona is missing.
    pub persona: Option<String>,
    pub temperature: Option<f32>,
    pub max_tokens: Option<u32>,
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
            persona: None,
            temperature: None,
            max_tokens: None,
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
    personas: Arc<PersonaManager>,
    persona_context: RwLock<PersonaUse>,
    provider_manager: RwLock<Option<Arc<std::sync::Mutex<ProviderManager>>>>,
    user_settings: RwLock<Option<Arc<std::sync::Mutex<UserSettings>>>>,
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
    /// User messages submitted while this agent is busy. The queue is owned by
    /// the agent so submissions never race with trajectory mutation and are
    /// retained when a turn is cancelled.
    mailbox: std::sync::Mutex<VecDeque<String>>,
}

/// Streamed observation of what the agent is doing, for a UI/CLI to render.
#[derive(Debug, Clone)]
pub enum AgentEvent {
    Thinking(String),
    /// A queued user message was injected into the active trajectory.
    UserMessage(String),
    Text(String),
    ToolCallDelta {
        index: u32,
        id: String,
        name_delta: String,
        args_delta: String,
    },
    ToolCallStarted {
        index: u32,
        id: String,
        name: String,
        args: String,
    },
    ToolResult {
        index: u32,
        id: String,
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
        Self::new_with_personas(
            provider,
            tools,
            config,
            session_id,
            Arc::new(PersonaManager::empty()),
        )
    }

    pub fn new_with_personas(
        provider: Arc<dyn Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
        session_id: impl Into<String>,
        personas: Arc<PersonaManager>,
    ) -> Self {
        let mut state = AgentState::default();
        if config.persona.is_none()
            && let Some(system) = &config.system_prompt
        {
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
            personas,
            persona_context: RwLock::new(PersonaUse::Main),
            provider_manager: RwLock::new(None),
            user_settings: RwLock::new(None),
            config: RwLock::new(config),
            stop_policy: Arc::new(DefaultStopPolicy),
            host: Arc::new(LocalHost::new()),
            session_handle: RwLock::new(None),
            bus: RwLock::new(None),
            busy: tokio::sync::Mutex::new(()),
            mailbox: std::sync::Mutex::new(VecDeque::new()),
        }
    }

    pub fn new_with_persona_manager(
        provider: Arc<dyn Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
        session_id: impl Into<String>,
        personas: Arc<PersonaManager>,
    ) -> Self {
        Self::new_with_personas(provider, tools, config, session_id, personas)
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

    pub fn persona_manager(&self) -> Arc<PersonaManager> {
        self.personas.clone()
    }

    pub fn persona_context(&self) -> PersonaUse {
        *self.persona_context.read().unwrap()
    }

    /// Attach process-lifetime services used for persona model preferences.
    /// These handles are intentionally not persisted with the session.
    pub fn attach_runtime(
        &self,
        provider_manager: Arc<std::sync::Mutex<ProviderManager>>,
        user_settings: Arc<std::sync::Mutex<UserSettings>>,
    ) {
        *self.provider_manager.write().unwrap() = Some(provider_manager);
        *self.user_settings.write().unwrap() = Some(user_settings);
    }

    pub fn provider_manager_handle(&self) -> Option<Arc<std::sync::Mutex<ProviderManager>>> {
        self.provider_manager.read().unwrap().clone()
    }

    pub fn user_settings_handle(&self) -> Option<Arc<std::sync::Mutex<UserSettings>>> {
        self.user_settings.read().unwrap().clone()
    }

    pub fn is_busy(&self) -> bool {
        self.busy.try_lock().is_err()
    }

    /// Submit a message to this agent's FIFO mailbox. Messages submitted while
    /// a turn is running are consumed together immediately before the next
    /// provider turn. Submissions are always accepted, including during
    /// cancellation, so callers can retry or resume the queued work.
    pub fn submit(&self, message: impl Into<String>) {
        self.mailbox.lock().unwrap().push_back(message.into());
    }

    /// Snapshot pending user messages for presentation by interactive clients.
    pub fn pending_messages(&self) -> Vec<String> {
        self.mailbox.lock().unwrap().iter().cloned().collect()
    }

    /// Atomically remove all currently pending messages. Messages submitted
    /// after the lock is released remain queued for the following turn.
    fn drain_mailbox(&self) -> Vec<String> {
        self.mailbox.lock().unwrap().drain(..).collect()
    }

    #[cfg(test)]
    fn mailbox_len(&self) -> usize {
        self.mailbox.lock().unwrap().len()
    }

    pub fn set_persona_context(&self, context: PersonaUse) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        self.validate_persona_for_context(self.config.read().unwrap().persona.as_deref(), context)?;
        *self.persona_context.write().unwrap() = context;
        Ok(())
    }

    /// Change both the persona and its use context atomically between turns.
    pub fn set_persona(
        &self,
        persona: Option<String>,
        context: PersonaUse,
    ) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        self.validate_persona_for_context(persona.as_deref(), context)?;
        self.config.write().unwrap().persona = persona;
        *self.persona_context.write().unwrap() = context;
        Ok(())
    }

    pub fn switch_persona(&self, persona: Option<String>) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        let context = *self.persona_context.read().unwrap();
        self.validate_persona_for_context(persona.as_deref(), context)?;
        self.config.write().unwrap().persona = persona;
        Ok(())
    }

    fn validate_persona_for_context(
        &self,
        persona_id: Option<&str>,
        context: PersonaUse,
    ) -> Result<(), AgentError> {
        let Some(id) = persona_id else {
            return Ok(());
        };
        let persona = self.personas.require(id)?;
        if context == PersonaUse::Main && persona.background {
            return Err(PersonaError::BackgroundOnly(id.to_string()).into());
        }
        Ok(())
    }

    // ------------------------------------------------------------------
    // Between-turn mutation — guarded by `busy`
    // ------------------------------------------------------------------

    /// Apply a mutation to this agent's config between turns. Refuses while a
    /// turn is running (`AgentError::Busy`). Safe because every mutable field
    /// is re-read into `ProviderRequest` on each generation.
    pub fn update_config(&self, f: impl FnOnce(&mut AgentConfig)) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        let mut next = self.config.read().unwrap().clone();
        f(&mut next);
        let context = *self.persona_context.read().unwrap();
        self.validate_persona_for_context(next.persona.as_deref(), context)?;
        *self.config.write().unwrap() = next;
        Ok(())
    }

    fn persona_turn_snapshot(
        &self,
        config: &AgentConfig,
    ) -> Result<(Option<String>, Option<HashSet<String>>), AgentError> {
        let Some(id) = config.persona.as_deref() else {
            return Ok((config.system_prompt.clone(), None));
        };
        let context = *self.persona_context.read().unwrap();
        self.validate_persona_for_context(Some(id), context)?;
        let persona = self.personas.require(id)?;
        Ok((
            Some(persona.system_prompt.clone()),
            Some(persona.tool_scopes.iter().cloned().collect()),
        ))
    }

    fn messages_for_request(&self, system_prompt: Option<&str>) -> Context {
        let mut history = self.state.read().unwrap().history.clone();
        let leading_systems = history
            .iter()
            .take_while(|message| message.role == MessageRole::System)
            .count();
        if leading_systems > 0 {
            history.drain(..leading_systems);
        }
        if let Some(system) = system_prompt {
            history.insert(0, Message::text(MessageRole::System, system.to_string()));
        }
        history
    }

    /// Persona prompts are loaded from the current persona file on resume, so
    /// never duplicate a leading system message into the session record.
    pub fn history_for_persistence(&self) -> Context {
        let mut history = self.history();
        if self.config.read().unwrap().persona.is_some() {
            let leading_systems = history
                .iter()
                .take_while(|message| message.role == MessageRole::System)
                .count();
            history.drain(..leading_systems);
        }
        history
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

        // Config and persona permissions are snapshotted before any trajectory
        // mutation. Invalid or missing personas therefore fail closed without
        // leaving a phantom user message in history.
        let config = self.config.read().unwrap().clone();
        let (turn_system_prompt, turn_scopes) = self.persona_turn_snapshot(&config)?;

        // Heal interruptions: a previous turn cancelled or crashed mid-tool
        // may have left calls without results, which providers refuse.
        {
            let mut s = self.state.write().unwrap();
            repair_dangling_tool_calls(&mut s.history);
        }

        if cancellation.is_cancelled() {
            return Err(AgentError::Cancelled(String::new()));
        }
        self.submit(user_input);
        let mut initial_submission = true;

        // Config can't change while we hold `busy`, so the snapshot above is
        // valid for every iteration of the loop.
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

        loop {
            if cancellation.is_cancelled() {
                return Err(AgentError::Cancelled(final_text));
            }
            // Check cancellation before draining. Thus a cancelled turn never
            // loses messages that are still waiting in the mailbox.
            let pending = self.drain_mailbox();
            if !pending.is_empty() {
                let mut state = self.state.write().unwrap();
                let injected = pending.len().saturating_sub(initial_submission as usize);
                for (index, message) in pending.into_iter().enumerate() {
                    if !initial_submission || index < injected {
                        emit(AgentEvent::UserMessage(message.clone()));
                    }
                    state
                        .history
                        .push(Message::text(MessageRole::User, message));
                }
                initial_submission = false;
            }
            {
                let history = &self.state.read().unwrap().history;
                validate_context(history).map_err(AgentError::Trajectory)?;
            }

            let request = ProviderRequest {
                model: config.model.clone(),
                messages: self.messages_for_request(turn_system_prompt.as_deref()),
                tools: self.tools.definitions_scoped(turn_scopes.as_ref()),
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
                // A response that would normally end the run must yield to
                // input submitted while it was in flight. Drain the complete
                // batch before the next provider request so queued messages
                // are handled together, not as separate turns.
                let pending = self.drain_mailbox();
                if !pending.is_empty() {
                    let mut state = self.state.write().unwrap();
                    for message in pending {
                        emit(AgentEvent::UserMessage(message.clone()));
                        state
                            .history
                            .push(Message::text(MessageRole::User, message));
                    }
                    continue;
                }
                return Ok(final_text);
            }

            // Execute every tool call from this generation, append results.
            let calls: Vec<(u32, String, String, String)> = assistant
                .content
                .iter()
                .filter_map(|part| match part {
                    MessagePart::ToolCall { id, name, args } => {
                        Some((0, id.clone(), name.clone(), args.clone()))
                    }
                    _ => None,
                })
                .enumerate()
                .map(|(index, (_, id, name, args))| (index as u32, id, name, args))
                .collect();

            let mut results = Vec::new();
            for (index, call_id, name, args) in calls {
                if cancellation.is_cancelled() {
                    return Err(AgentError::Cancelled(final_text));
                }
                emit(AgentEvent::ToolCallStarted {
                    index,
                    id: call_id.clone(),
                    name: name.clone(),
                    args: args.clone(),
                });
                let parsed = serde_json::from_str(&args).unwrap_or(serde_json::Value::Null);
                let ctx = ToolContext {
                    workdir: config.workdir.clone(),
                    cancellation: cancellation.clone(),
                    tool_call_id: call_id.clone(),
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
                let (content, ok) = match self
                    .tools
                    .call_scoped(&name, parsed, ctx, turn_scopes.as_ref())
                    .await
                {
                    Ok(output) => (crate::tools::redirect_large_tool_result(output), true),
                    Err(e) => (e.to_string(), false),
                };
                emit(AgentEvent::ToolResult {
                    index,
                    id: call_id.clone(),
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
        // `Provider::stream` performs the request and only returns once the
        // response headers arrive.  Keep it inside the cancellation select as
        // well as the event-reading loop below, otherwise a provider that
        // never responds makes cancellation ineffective.
        let mut stream = tokio::select! {
            biased;
            _ = cancellation.cancelled() => {
                return Ok((
                    build_assistant_message(String::new(), None, String::new(), Vec::new()),
                    StopReason::Cancelled,
                    String::new(),
                    Usage::default(),
                ));
            }
            result = provider.stream(request) => result?,
        };

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
                        ProviderEvent::ToolCallDelta { index, id, name_delta, args_delta } => {
                            observer(AgentEvent::ToolCallDelta {
                                index,
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

    struct HangingProvider {
        started: Arc<tokio::sync::Notify>,
    }

    struct CaptureProvider {
        request: Arc<RwLock<Option<ProviderRequest>>>,
    }

    struct MailboxCaptureProvider {
        requests: Arc<std::sync::Mutex<Vec<ProviderRequest>>>,
    }

    #[async_trait::async_trait]
    impl Provider for HangingProvider {
        fn id(&self) -> &str {
            "hanging"
        }

        async fn stream(
            &self,
            _request: ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            self.started.notify_one();
            std::future::pending().await
        }
    }

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

    #[async_trait::async_trait]
    impl Provider for CaptureProvider {
        fn id(&self) -> &str {
            "capture"
        }

        async fn stream(
            &self,
            request: ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            *self.request.write().unwrap() = Some(request);
            Ok(futures::stream::iter([Ok(ProviderEvent::Done {
                reason: StopReason::Stop,
            })])
            .boxed())
        }
    }

    #[async_trait::async_trait]
    impl Provider for MailboxCaptureProvider {
        fn id(&self) -> &str {
            "mailbox-capture"
        }

        async fn stream(
            &self,
            request: ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            self.requests.lock().unwrap().push(request);
            Ok(futures::stream::iter([Ok(ProviderEvent::Done {
                reason: StopReason::Stop,
            })])
            .boxed())
        }
    }

    fn persona_manager_with(files: &[(&str, &str)]) -> Arc<PersonaManager> {
        let directory =
            std::env::temp_dir().join(format!("firmius-agent-persona-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&directory).unwrap();
        for (name, contents) in files {
            std::fs::write(directory.join(name), contents).unwrap();
        }
        Arc::new(PersonaManager::load_from(directory).unwrap())
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

    #[tokio::test]
    async fn persona_prompt_replaces_config_prompt_without_history_duplication() {
        let personas = persona_manager_with(&[(
            "main.md",
            "---\nname: Main\nscopes: []\nbackground: false\n---\nPersona prompt.",
        )]);
        let request = Arc::new(RwLock::new(None));
        let config = AgentConfig {
            provider_id: "capture".into(),
            model: "capture-model".into(),
            system_prompt: Some("Default prompt.".into()),
            persona: Some("main".into()),
            ..Default::default()
        };
        let agent = Agent::new_with_personas(
            Arc::new(CaptureProvider {
                request: request.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            config,
            "test-session",
            personas,
        );

        agent
            .prompt("hello", CancellationToken::new(), |_| {})
            .await
            .unwrap();

        assert_eq!(agent.history()[0].role, MessageRole::User);
        let request = request.read().unwrap().clone().unwrap();
        assert!(matches!(request.messages[0].role, MessageRole::System));
        assert_eq!(
            request.messages[0].content,
            vec![MessagePart::Text("Persona prompt.".into())]
        );
    }

    #[tokio::test]
    async fn switching_from_default_replaces_the_seeded_system_prompt() {
        let personas = persona_manager_with(&[(
            "main.md",
            "---\nname: Main\nscopes: []\nbackground: false\n---\nPersona prompt.",
        )]);
        let request = Arc::new(RwLock::new(None));
        let agent = Agent::new_with_personas(
            Arc::new(CaptureProvider {
                request: request.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "capture".into(),
                model: "capture-model".into(),
                system_prompt: Some("Default prompt.".into()),
                ..Default::default()
            },
            "test-session",
            personas,
        );
        agent
            .set_persona(Some("main".into()), PersonaUse::Main)
            .unwrap();

        agent
            .prompt("hello", CancellationToken::new(), |_| {})
            .await
            .unwrap();

        let request = request.read().unwrap().clone().unwrap();
        assert_eq!(
            request.messages[0].content,
            vec![MessagePart::Text("Persona prompt.".into())]
        );
        assert_eq!(agent.history()[0].role, MessageRole::System);
        assert_eq!(agent.history_for_persistence()[0].role, MessageRole::User);
    }

    #[tokio::test]
    async fn invalid_persona_prompt_does_not_mutate_history() {
        let agent = Agent::new_with_personas(
            Arc::new(DummyProvider),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "dummy".into(),
                model: "dummy-model".into(),
                persona: Some("missing".into()),
                ..Default::default()
            },
            "test-session",
            Arc::new(PersonaManager::empty()),
        );

        let result = agent
            .prompt("must not persist", CancellationToken::new(), |_| {})
            .await;
        assert!(matches!(
            result,
            Err(AgentError::Persona(PersonaError::NotFound(_)))
        ));
        assert!(agent.history().is_empty());
    }

    #[tokio::test]
    async fn persona_scopes_filter_tool_definitions_sent_to_provider() {
        let personas = persona_manager_with(&[(
            "reader.md",
            "---\nname: Reader\ntool_scopes: [fs_read]\nbackground: false\n---\nRead only.",
        )]);
        let request = Arc::new(RwLock::new(None));
        let mut tools = ToolRegistry::default();
        tools.register(
            crate::tools::TypedTool::new(
                "read_like",
                "read",
                |_args: serde_json::Value, _ctx: ToolContext| {
                    Box::pin(async { Ok("ok".to_string()) })
                },
            )
            .with_required_scopes(["fs_read"]),
        );
        tools.register(
            crate::tools::TypedTool::new(
                "write_like",
                "write",
                |_args: serde_json::Value, _ctx: ToolContext| {
                    Box::pin(async { Ok("ok".to_string()) })
                },
            )
            .with_required_scopes(["fs_write"]),
        );
        let agent = Agent::new_with_personas(
            Arc::new(CaptureProvider {
                request: request.clone(),
            }),
            Arc::new(tools),
            AgentConfig {
                provider_id: "capture".into(),
                model: "capture-model".into(),
                persona: Some("reader".into()),
                ..Default::default()
            },
            "test-session",
            personas,
        );

        agent
            .prompt("inspect", CancellationToken::new(), |_| {})
            .await
            .unwrap();

        let captured = request.read().unwrap().clone().unwrap();
        let names = captured
            .tools
            .iter()
            .map(|tool| tool.name.as_str())
            .collect::<Vec<_>>();
        assert_eq!(names, vec!["read_like"]);
    }

    #[test]
    fn missing_and_background_personas_fail_closed_for_main() {
        let personas = persona_manager_with(&[(
            "worker.md",
            "---\nname: Worker\nscopes: []\nbackground: true\n---\nWorker prompt.",
        )]);
        let config = AgentConfig {
            provider_id: "dummy".into(),
            model: "dummy-model".into(),
            ..Default::default()
        };
        let agent = Agent::new_with_personas(
            Arc::new(DummyProvider),
            Arc::new(ToolRegistry::default()),
            config,
            "test-session",
            personas,
        );

        assert!(matches!(
            agent.switch_persona(Some("missing".into())),
            Err(AgentError::Persona(PersonaError::NotFound(_)))
        ));
        assert!(matches!(
            agent.switch_persona(Some("worker".into())),
            Err(AgentError::Persona(PersonaError::BackgroundOnly(_)))
        ));
        agent
            .set_persona_context(PersonaRuntimeContext::Delegate)
            .unwrap();
        agent.switch_persona(Some("worker".into())).unwrap();
        assert_eq!(agent.config().persona.as_deref(), Some("worker"));
    }

    #[tokio::test]
    async fn cancellation_interrupts_hanging_provider_request() {
        let started = Arc::new(tokio::sync::Notify::new());
        let config = AgentConfig {
            provider_id: "hanging".into(),
            model: "hanging-model".into(),
            ..Default::default()
        };
        let agent = Arc::new(Agent::new(
            Arc::new(HangingProvider {
                started: started.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            config,
            "test-session",
        ));
        let cancellation = CancellationToken::new();
        let task_cancellation = cancellation.clone();
        let task =
            tokio::spawn(async move { agent.prompt("hang", task_cancellation, |_| {}).await });

        started.notified().await;
        cancellation.cancel();

        let result = tokio::time::timeout(std::time::Duration::from_secs(1), task)
            .await
            .expect("cancellation should interrupt the provider request")
            .expect("prompt task should not panic");
        assert!(matches!(result, Ok(text) if text.is_empty()));
    }

    #[test]
    fn submit_is_fifo_and_cancellation_preserves_pending_messages() {
        let agent = agent_with_history(Vec::new());
        agent.submit("first");
        agent.submit("second");
        agent.submit("third");
        assert_eq!(agent.mailbox_len(), 3);

        let cancellation = CancellationToken::new();
        cancellation.cancel();
        let runtime = tokio::runtime::Runtime::new().unwrap();
        let result = runtime.block_on(agent.prompt("ignored", cancellation, |_| {}));
        assert!(matches!(result, Err(AgentError::Cancelled(_))));
        assert_eq!(agent.mailbox_len(), 3);
        assert_eq!(agent.drain_mailbox(), ["first", "second", "third"]);
    }

    #[tokio::test]
    async fn prompt_drains_all_submissions_in_fifo_order_before_provider_turn() {
        let requests = Arc::new(std::sync::Mutex::new(Vec::<ProviderRequest>::new()));
        let agent = Agent::new(
            Arc::new(MailboxCaptureProvider {
                requests: requests.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "mailbox-capture".into(),
                model: "test-model".into(),
                ..Default::default()
            },
            "test-session",
        );
        agent.submit("queued one");
        agent.submit("queued two");
        agent
            .prompt("initial", CancellationToken::new(), |_| {})
            .await
            .unwrap();

        let requests = requests.lock().unwrap();
        let users = requests[0]
            .messages
            .iter()
            .filter(|message| message.role == MessageRole::User)
            .map(|message| message.content[0].clone())
            .collect::<Vec<_>>();
        assert_eq!(
            users,
            vec![
                MessagePart::Text("queued one".into()),
                MessagePart::Text("queued two".into()),
                MessagePart::Text("initial".into()),
            ]
        );
    }
}
