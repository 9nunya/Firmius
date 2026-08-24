use crate::FirmiusConfig;
use crate::compaction::{self, CompactionPlan, Projection, Timeline, TimelineSegment};
use crate::compaction_job::{self, CompactionJobError, CompactionJobInput, CompactionResult};
use crate::context_budget::{self, BudgetConfig, BudgetDecision};
use crate::host::{Host, LocalHost};
use crate::persona::{PersonaError, PersonaManager};
use crate::providers::{Provider, ProviderError, ProviderEvent, manager::ProviderManager};
use crate::retry::{FailureClass, RetryController, RetryDecision, classify};
use crate::tools::{ToolContext, ToolRegistry};
use crate::types::{
    Context, EffortMode, Message, MessagePart, MessageRole, ProviderRequest, StopReason, Usage,
    WebSearchRequest, intersect_web_search, repair_dangling_tool_calls, validate_context,
};
use futures::StreamExt;
use std::collections::{HashSet, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
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
#[derive(Debug, Clone, Default)]
pub struct AgentState {
    pub history: Context,
    /// Usage reported by the most recent provider turn. This drives the
    /// context-window meter, whose value is a property of that request.
    pub usage: Usage,
    /// Usage accumulated across all provider turns in this agent session.
    pub total_usage: Usage,
    /// Generation-checked compaction projection. Its committed metadata is
    /// persisted alongside the history; the timeline is rebuilt from history.
    pub compaction: CompactionState,
}

fn compaction_model_is_valid(
    schema: &crate::providers::schema::ProviderSchema,
    model: &str,
) -> bool {
    schema.models.is_empty() || schema.model(model).is_some()
}

fn timeline_from_history(history: &[Message]) -> Timeline {
    // Each completed user/tool exchange is a segment; the final message is
    // always kept as the active segment by the pure planner.  This
    // conservative grouping means no message or tool pair is split.
    //
    // Leading system messages are excluded: they are always re-derived at
    // request time from the persona/config and must never be summarized,
    // removed, or reprojected as a second system instruction by compaction.
    let non_system: Vec<Message> = history
        .iter()
        .skip_while(|m| m.role == MessageRole::System)
        .cloned()
        .collect();
    let mut segments = Vec::new();
    let mut entries = Vec::new();
    let mut id = 0u64;
    for (turn, message) in non_system.iter().enumerate() {
        entries.push(crate::compaction::TimelineEntry::new(
            turn as u64,
            message.clone(),
        ));
        if message.role == MessageRole::Tool
            || non_system
                .get(turn + 1)
                .is_some_and(|next| next.role == MessageRole::User)
        {
            segments.push(TimelineSegment::new(format!("segment-{id}"), entries));
            entries = Vec::new();
            id += 1;
        }
    }

    if !entries.is_empty() {
        segments.push(TimelineSegment::new(format!("segment-{id}"), entries));
    }
    Timeline::new(segments)
}

#[derive(Debug, Clone)]
pub struct CompactionState {
    pub projection: Projection,
}

impl Default for CompactionState {
    fn default() -> Self {
        Self {
            projection: Projection::new(Timeline::default()),
        }
    }
}

impl Agent {
    pub fn label(&self) -> Option<String> {
        self.label.read().unwrap().clone()
    }
    pub fn metadata(&self) -> serde_json::Map<String, serde_json::Value> {
        self.metadata.read().unwrap().clone()
    }
    pub fn set_label(&self, label: Option<String>) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        *self.label.write().unwrap() = label;
        Ok(())
    }
    pub fn set_metadata(
        &self,
        metadata: serde_json::Map<String, serde_json::Value>,
    ) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        *self.metadata.write().unwrap() = metadata;
        Ok(())
    }
    /// Invalidate an in-flight background job before replacing the trajectory.
    /// The epoch rejects late results even if projection generations repeat.
    fn invalidate_compaction(&self, timeline: Timeline) {
        self.compaction_epoch.fetch_add(1, Ordering::AcqRel);
        if let Some(handle) = self.compaction_job.lock().unwrap().take() {
            handle.cancellation.cancel();
        }
        let mut state = self.state.write().unwrap();
        let generation = state.compaction.projection.generation.saturating_add(1);
        let mut projection = Projection::new(timeline);
        projection.generation = generation;
        state.compaction.projection = projection;
    }

    fn model_info(&self, provider_id: &str, model_id: &str) -> Option<crate::types::ModelInfo> {
        if let Some((metadata_provider, metadata)) = self.model_metadata.read().unwrap().as_ref()
            && metadata_provider == provider_id
            && metadata.id == model_id
        {
            return Some(metadata.clone());
        }
        let manager = self.provider_manager.read().unwrap().clone()?;
        let manager = manager.lock().unwrap();
        manager.model_info_for(provider_id, model_id).cloned()
    }

    fn sync_compaction_projection(&self) {
        let history = self.state.read().unwrap().history.clone();
        let timeline = timeline_from_history(&history);
        let mut state = self.state.write().unwrap();
        if state.compaction.projection.timeline != timeline {
            state.compaction.projection.timeline = timeline;
        }
    }

    fn compaction_input(
        &self,
    ) -> Result<
        Option<(
            CompactionPlan,
            CompactionJobInput,
            String,
            Arc<dyn Provider>,
        )>,
        AgentError,
    > {
        let state = self.state.read().unwrap();
        let projection = &state.compaction.projection;
        let Some(plan) = compaction::plan(projection, projection.generation).ok() else {
            return Ok(None);
        };
        let (start, end) = plan.source_range;
        let source_messages = projection.timeline.segments[start..end]
            .iter()
            .flat_map(|segment| segment.entries.iter().map(|entry| entry.message.clone()))
            .collect();
        let config = self.config.read().unwrap().clone();
        let (provider_id, model, provider) = self.compaction_provider(&config)?;
        let snapshot = projection.snapshot.clone();
        let input = CompactionJobInput {
            plan: plan.clone(),
            snapshot: snapshot.clone(),
            source_messages,
            metadata: snapshot.map(|s| s.summary).unwrap_or_default(),
            model,
        };
        Ok(Some((plan, input, provider_id, provider)))
    }

    /// Resolve compaction independently from the active turn provider.  The
    /// manager lock is only used to clone/build the provider; no guard can
    /// survive into the async job.
    fn compaction_provider(
        &self,
        config: &AgentConfig,
    ) -> Result<(String, String, Arc<dyn Provider>), AgentError> {
        let general = FirmiusConfig::load().unwrap_or_default().general;
        let provider_id = general
            .compaction_provider
            .unwrap_or_else(|| config.provider_id.clone());
        let model = general
            .compaction_model
            .unwrap_or_else(|| config.model.clone());
        let Some(manager) = self.provider_manager.read().unwrap().clone() else {
            if provider_id == config.provider_id && model == config.model {
                return Ok((provider_id, model, self.provider.read().unwrap().clone()));
            }
            return Err(AgentError::Compaction(
                "configured compaction provider requires an attached ProviderManager".into(),
            ));
        };
        let (provider_id, provider, valid) = {
            let manager = manager.lock().unwrap();
            let selected_provider_id = if manager.has_account_selection_policy(&provider_id) {
                manager
                    .ranked_accounts_for(&provider_id)
                    .into_iter()
                    .next()
                    .ok_or_else(|| {
                        AgentError::Compaction(format!(
                            "no usable account remains for provider '{provider_id}'"
                        ))
                    })?
            } else {
                provider_id.clone()
            };
            // An empty catalog is intentionally treated as dynamic/unknown
            // (common for generic OpenAI-compatible providers), not as an
            // authoritative rejection of an otherwise configured model.
            let valid = manager
                .schema(&selected_provider_id)
                .is_some_and(|schema| compaction_model_is_valid(schema, &model));
            let provider = manager.build(&selected_provider_id).map_err(|error| {
                AgentError::Compaction(format!(
                    "invalid compaction provider '{selected_provider_id}': {error}"
                ))
            })?;
            (selected_provider_id, provider, valid)
        };
        if !valid {
            return Err(AgentError::Compaction(format!(
                "invalid compaction model '{model}' for provider '{provider_id}'"
            )));
        }
        Ok((provider_id, model, provider))
    }

    fn schedule_compaction(
        &self,
        plan: CompactionPlan,
        input: CompactionJobInput,
        provider_id: String,
        provider: Arc<dyn Provider>,
        observer: &mut impl FnMut(AgentEvent),
    ) {
        let mut slot = self.compaction_job.lock().unwrap();
        if slot.is_some() {
            return;
        }
        let generation = plan.generation;
        let epoch = self.compaction_epoch.load(Ordering::Acquire);
        let cancellation = CancellationToken::new();
        let task_cancellation = cancellation.clone();
        let (tx, result) = tokio::sync::oneshot::channel();
        let (delta_tx, deltas) = tokio::sync::mpsc::unbounded_channel();
        let task_input = input.clone();
        tokio::spawn(async move {
            let result = compaction_job::run_compaction_job_observed(
                task_input,
                provider,
                task_cancellation,
                |delta| {
                    let _ = delta_tx.send(delta.to_string());
                },
            )
            .await;
            let _ = tx.send(result);
        });
        *slot = Some(CompactionJobHandle {
            generation,
            epoch,
            plan,
            cancellation,
            input,
            provider_id,
            deltas,
            result,
        });
        observer(AgentEvent::CompactionScheduled { generation });
        observer(AgentEvent::CompactionStarted { generation });
    }

    /// Apply a completed job only while holding the normal state write lock;
    /// provider work is always outside this lock.
    fn commit_compaction(
        &self,
        plan: &CompactionPlan,
        result: CompactionResult,
    ) -> Result<(), AgentError> {
        let mut state = self.state.write().unwrap();
        if result.generation != plan.generation
            || result.source_segment_ids != plan.source_segment_ids
            || result.source_range != plan.source_range
            || result.source_entries != plan.source_entries
            || result.source_content_digest != plan.source_content_digest
        {
            return Err(AgentError::Compaction("stale compaction result".into()));
        }
        let (next, _) = compaction::apply(&state.compaction.projection, plan, result.summary)
            .map_err(|e| AgentError::Compaction(format!("{e:?}")))?;
        let (start, end) = plan.source_range;
        let source_start = state.compaction.projection.timeline.segments[..start]
            .iter()
            .map(|segment| segment.entries.len())
            .sum::<usize>();
        let source_end = source_start
            + state.compaction.projection.timeline.segments[start..end]
                .iter()
                .map(|segment| segment.entries.len())
                .sum::<usize>();
        let mut history: Vec<_> = state
            .compaction
            .projection
            .timeline
            .segments
            .iter()
            .flat_map(|segment| segment.entries.iter().map(|entry| entry.message.clone()))
            .collect();
        if source_end > history.len() {
            return Err(AgentError::Compaction("stale compaction source".into()));
        }
        history.drain(source_start..source_end);
        let leading_systems = state
            .history
            .iter()
            .take_while(|message| message.role == MessageRole::System)
            .cloned()
            .collect::<Vec<_>>();
        let system_count = history
            .iter()
            .take_while(|message| message.role == MessageRole::System)
            .count();
        history.drain(..system_count);
        state.history.clear();
        state.history.extend(leading_systems);
        state.history.extend(history);
        // Keep the projection's stable metadata while refreshing its
        // non-persisted timeline from the committed trajectory.  In
        // particular, this prevents the next preflight from treating the
        // compacted prefix as if it were still present.
        let mut next = next;
        next.timeline = timeline_from_history(&state.history);
        state.compaction.projection = next;
        // Provider usage describes the pre-compaction request. Keeping it
        // alive makes the TUI report stale context and prevents callers from
        // establishing a clean baseline for the next provider measurement.
        state.usage = Usage::default();
        Ok(())
    }

    fn poll_compaction(&self, observer: &mut impl FnMut(AgentEvent)) -> bool {
        // Include any messages added since the immutable job input.  The
        // generation-checked plan still applies when only the active tail
        // changed, while a changed source prefix is rejected transactionally.
        self.sync_compaction_projection();
        let mut slot = self.compaction_job.lock().unwrap();
        let Some(mut handle) = slot.take() else {
            return false;
        };
        while let Ok(delta) = handle.deltas.try_recv() {
            observer(AgentEvent::CompactionDelta {
                generation: handle.generation,
                delta,
            });
        }
        match handle.result.try_recv() {
            Ok(Ok(result)) => {
                let generation = handle.generation;
                if handle.epoch == self.compaction_epoch.load(Ordering::Acquire)
                    && self.commit_compaction(&handle.plan, result).is_ok()
                {
                    observer(AgentEvent::CompactionFinished { generation });
                    true
                } else {
                    observer(AgentEvent::CompactionDiscarded { generation });
                    false
                }
            }
            Ok(Err(error)) => {
                observer(AgentEvent::CompactionFailed {
                    generation: handle.generation,
                    error: error.to_string(),
                });
                false
            }
            Err(tokio::sync::oneshot::error::TryRecvError::Empty) => {
                *slot = Some(handle);
                false
            }
            Err(tokio::sync::oneshot::error::TryRecvError::Closed) => {
                observer(AgentEvent::CompactionFailed {
                    generation: handle.generation,
                    error: "compaction task stopped".into(),
                });
                false
            }
        }
    }

    async fn wait_compaction(
        &self,
        cancellation: &CancellationToken,
        observer: &mut impl FnMut(AgentEvent),
    ) -> Result<(), AgentError> {
        let Some(mut handle) = self.compaction_job.lock().unwrap().take() else {
            return Ok(());
        };
        let generation = handle.generation;
        let mut result = loop {
            tokio::select! {
                _ = cancellation.cancelled() => {
                    handle.cancellation.cancel();
                    return Err(AgentError::Cancelled(String::new()));
                }
                delta = handle.deltas.recv() => {
                    if let Some(delta) = delta {
                        observer(AgentEvent::CompactionDelta { generation, delta });
                    }
                }
                result = &mut handle.result => {
                    break result.map_err(|_| AgentError::Compaction("compaction task stopped".into()))?;
                }
            }
        };
        while let Ok(delta) = handle.deltas.try_recv() {
            observer(AgentEvent::CompactionDelta { generation, delta });
        }
        if let Err(CompactionJobError::Provider(error)) = result {
            result = self
                .retry_compaction(
                    handle.input.clone(),
                    handle.provider_id.clone(),
                    error,
                    cancellation,
                    observer,
                )
                .await;
        }
        match result {
            Ok(result) => {
                if handle.epoch != self.compaction_epoch.load(Ordering::Acquire)
                    || result.generation != handle.plan.generation
                    || result.source_segment_ids != handle.plan.source_segment_ids
                    || result.source_range != handle.plan.source_range
                    || result.source_entries != handle.plan.source_entries
                    || result.source_content_digest != handle.plan.source_content_digest
                {
                    observer(AgentEvent::CompactionDiscarded { generation });
                    Err(AgentError::Compaction("stale compaction result".into()))
                } else if self.commit_compaction(&handle.plan, result).is_ok() {
                    observer(AgentEvent::CompactionFinished { generation });
                    Ok(())
                } else {
                    observer(AgentEvent::CompactionDiscarded { generation });
                    Err(AgentError::Compaction("stale compaction result".into()))
                }
            }
            Err(error) => {
                observer(AgentEvent::CompactionFailed {
                    generation,
                    error: error.to_string(),
                });
                Err(AgentError::Compaction(error.to_string()))
            }
        }
    }

    async fn retry_compaction(
        &self,
        input: CompactionJobInput,
        configured_provider_id: String,
        first_error: ProviderError,
        cancellation: &CancellationToken,
        observer: &mut impl FnMut(AgentEvent),
    ) -> Result<CompactionResult, CompactionJobError> {
        let runtime = self.retry_runtime(&configured_provider_id);
        if runtime.accounts.is_empty() {
            return Err(CompactionJobError::Provider(ProviderError::Auth(format!(
                "no usable account remains for provider {configured_provider_id}"
            ))));
        }
        let mut controller = RetryController::new(runtime.config, runtime.accounts);
        let mut error = first_error;
        loop {
            let RetryDecision::Retry {
                account_id,
                delay,
                attempt,
                switched,
            } = controller.on_failure(&error)
            else {
                return Err(CompactionJobError::Provider(error));
            };
            observer(AgentEvent::RetryScheduled {
                account_id: account_id.clone(),
                attempt,
                delay_ms: delay.as_millis() as u64,
                switched,
                class: classify(&error),
            });
            let Some(manager) = self.provider_manager.read().unwrap().clone() else {
                return Err(CompactionJobError::Provider(error));
            };
            let provider = manager
                .lock()
                .unwrap()
                .build(&account_id)
                .map_err(|message| CompactionJobError::Provider(ProviderError::Auth(message)))?;
            tokio::select! {
                _ = cancellation.cancelled() => return Err(CompactionJobError::Cancelled),
                _ = tokio::time::sleep(delay) => {}
            }
            match compaction_job::run_compaction_job_observed(
                input.clone(),
                provider.clone(),
                cancellation.clone(),
                |delta| {
                    observer(AgentEvent::CompactionDelta {
                        generation: input.plan.generation,
                        delta: delta.to_string(),
                    });
                },
            )
            .await
            {
                Ok(result) => {
                    if configured_provider_id == self.config.read().unwrap().provider_id {
                        self.commit_successful_retry_provider(account_id, provider);
                    }
                    return Ok(result);
                }
                Err(CompactionJobError::Provider(next_error)) => error = next_error,
                Err(other) => return Err(other),
            }
        }
    }

    /// Run one compaction synchronously.  The operation is a between-turn
    /// mutation, and therefore cannot race a prompt.  A valid result is
    /// committed atomically; provider failures and stale results leave the
    /// trajectory untouched.
    pub async fn compact_now(
        &self,
        cancellation: CancellationToken,
        mut observer: impl FnMut(AgentEvent),
    ) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        self.sync_compaction_projection();
        let (plan, input, provider_id, provider) = self
            .compaction_input()?
            .ok_or_else(|| AgentError::Compaction("no safe compaction boundary".into()))?;
        observer(AgentEvent::CompactionStarted {
            generation: plan.generation,
        });
        let result = match compaction_job::run_compaction_job_observed(
            input.clone(),
            provider,
            cancellation.clone(),
            |delta| {
                observer(AgentEvent::CompactionDelta {
                    generation: plan.generation,
                    delta: delta.to_string(),
                });
            },
        )
        .await
        {
            Ok(result) => result,
            Err(CompactionJobError::Provider(error)) => self
                .retry_compaction(input, provider_id, error, &cancellation, &mut observer)
                .await
                .map_err(|e| AgentError::Compaction(e.to_string()))?,
            Err(error) => return Err(AgentError::Compaction(error.to_string())),
        };
        if self.commit_compaction(&plan, result).is_err() {
            observer(AgentEvent::CompactionDiscarded {
                generation: plan.generation,
            });
            return Err(AgentError::Compaction("stale compaction result".into()));
        }
        observer(AgentEvent::CompactionFinished {
            generation: plan.generation,
        });
        Ok(())
    }

    /// Alias for callers that use the shorter manual-compaction spelling.
    pub async fn compact(
        &self,
        cancellation: CancellationToken,
        observer: impl FnMut(AgentEvent),
    ) -> Result<(), AgentError> {
        self.compact_now(cancellation, observer).await
    }
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
    #[error("compaction unavailable: {0}")]
    Compaction(String),
    #[error("persistence error: {0}")]
    Persistence(String),
    #[error(
        "context input exceeds usable budget after compaction (estimated {estimated_input}, usable {usable_input})"
    )]
    ContextBudget {
        estimated_input: u32,
        usable_input: u32,
    },
    #[error("context input exceeds usable budget and no safe compaction boundary exists")]
    ContextBudgetNoSafeBoundary,
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
    label: RwLock<Option<String>>,
    metadata: RwLock<serde_json::Map<String, serde_json::Value>>,
    state: Arc<RwLock<AgentState>>,
    /// Interior-mutable so reconfiguration (`set_provider`) can swap backends
    /// between turns without rebuilding the agent. Always kept in agreement
    /// with `config.provider_id` — change both via `set_provider` only.
    provider: RwLock<Arc<dyn Provider>>,
    tools: Arc<ToolRegistry>,
    personas: Arc<PersonaManager>,
    persona_context: RwLock<PersonaUse>,
    provider_manager: RwLock<Option<Arc<std::sync::Mutex<ProviderManager>>>>,
    /// Optional metadata supplied by a runtime model catalog. This is kept
    /// separate from provider construction so callers with an already-built
    /// provider (and deterministic test providers) can still exercise the
    /// context-budget gate.
    model_metadata: RwLock<Option<(String, crate::types::ModelInfo)>>,
    user_settings: RwLock<Option<Arc<std::sync::Mutex<UserSettings>>>>,
    /// Live umbrella config (`~/.firmius/config.json`), so hosted-search
    /// policy changes take effect on the next request without a disk reload.
    firmius_config: RwLock<Option<Arc<std::sync::Mutex<FirmiusConfig>>>>,
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
    /// `Session` in its canonical `Arc` handle.
    session_handle: RwLock<Option<Weak<crate::session::Session>>>,
    /// Session event bus this agent tees every emitted `AgentEvent` into.
    /// `None` for agents that live outside a session (e.g. unit tests).
    bus: RwLock<Option<broadcast::Sender<crate::session::SessionEvent>>>,
    /// Held for the entire duration of `prompt()`. Serializes turns on this
    /// agent, and lets between-turn mutators refuse to run mid-turn.
    busy: tokio::sync::Mutex<()>,
    /// User messages submitted while this agent is busy. The queue is owned by
    /// the agent so submissions never race with trajectory mutation and are
    /// retained when a turn is cancelled.
    mailbox: std::sync::Mutex<VecDeque<Message>>,
    /// Serializes mailbox-to-history moves with persistence snapshots. The
    /// lock order is transaction -> mailbox/state whenever more than one is
    /// needed.
    persistence_transaction: std::sync::Mutex<()>,
    /// A provider-only job and its immutable input.  The task communicates
    /// only through this channel; it never obtains or mutates AgentState.
    compaction_job: std::sync::Mutex<Option<CompactionJobHandle>>,
    /// Monotonic trajectory epoch used to reject completed jobs after a
    /// rewind/clear, even if projection generations happen to repeat.
    compaction_epoch: AtomicU64,
}

struct CompactionJobHandle {
    generation: u64,
    epoch: u64,
    plan: CompactionPlan,
    cancellation: CancellationToken,
    input: CompactionJobInput,
    provider_id: String,
    deltas: tokio::sync::mpsc::UnboundedReceiver<String>,
    result: tokio::sync::oneshot::Receiver<Result<CompactionResult, CompactionJobError>>,
}

/// Streamed observation of what the agent is doing, for a UI/CLI to render.
#[derive(Debug, Clone)]
pub enum AgentEvent {
    Thinking(String),
    /// A queued user message was injected into the active trajectory.
    UserMessage(String),
    Text(String),
    /// The provider failed before any visible output landed, and the retry
    /// policy scheduled another attempt.
    RetryScheduled {
        account_id: String,
        attempt: u32,
        delay_ms: u64,
        switched: bool,
        class: FailureClass,
    },
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
    CompactionScheduled {
        generation: u64,
    },
    CompactionStarted {
        generation: u64,
    },
    CompactionDelta {
        generation: u64,
        delta: String,
    },
    CompactionFinished {
        generation: u64,
    },
    CompactionDiscarded {
        generation: u64,
    },
    CompactionFailed {
        generation: u64,
        error: String,
    },
    /// Hosted web search began. The TUI presents this as a search item, not a tool call.
    WebSearchStarted {
        id: String,
    },
    /// Hosted web search completed. The agent loop records MessagePart::WebSearch.
    WebSearchFinished {
        id: String,
        action: crate::types::WebSearchAction,
    },
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
            label: RwLock::new(None),
            metadata: RwLock::new(serde_json::Map::new()),
            state: Arc::new(RwLock::new(state)),
            provider: RwLock::new(provider),
            tools,
            personas,
            persona_context: RwLock::new(PersonaUse::Main),
            provider_manager: RwLock::new(None),
            model_metadata: RwLock::new(None),
            user_settings: RwLock::new(None),
            firmius_config: RwLock::new(None),
            config: RwLock::new(config),
            stop_policy: Arc::new(DefaultStopPolicy),
            host: Arc::new(LocalHost::new()),
            session_handle: RwLock::new(None),
            bus: RwLock::new(None),
            busy: tokio::sync::Mutex::new(()),
            mailbox: std::sync::Mutex::new(VecDeque::new()),
            persistence_transaction: std::sync::Mutex::new(()),
            compaction_job: std::sync::Mutex::new(None),
            compaction_epoch: AtomicU64::new(0),
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
    pub fn attach_session(&self, session: Arc<crate::session::Session>) {
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

    pub fn with_mailbox(self, mailbox: Vec<Message>) -> Self {
        *self.mailbox.lock().unwrap() = mailbox.into();
        self
    }

    /// Restore committed compaction metadata from a session record. The
    /// timeline is always rebuilt from persisted history; the generation and
    /// snapshot metadata are the durable part of the projection.
    pub fn with_compaction(self, mut projection: Projection) -> Self {
        if !crate::persistence::valid_projection(&projection) {
            projection = Projection::new(Timeline::default());
        }
        let history = self.history();
        projection.timeline = timeline_from_history(&history);
        self.state.write().unwrap().compaction.projection = projection;
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
        {
            let mut state = self.state.write().unwrap();
            state.history = history;
            state.compaction.projection = Projection::new(timeline_from_history(&state.history));
        }
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

    /// The committed projection to persist with the agent record.
    pub fn compaction_projection(&self) -> Projection {
        self.state.read().unwrap().compaction.projection.clone()
    }

    /// Capture the history and compaction metadata under one state read lock.
    /// The projection timeline is deliberately rebuilt from that exact
    /// history, so persistence cannot contain a pair from different moments
    /// while a turn is being finalized.
    pub fn persistence_snapshot(&self) -> (Context, Projection) {
        let strip_persona_system = self.config.read().unwrap().persona.is_some();
        let state = self.state.read().unwrap();
        let mut history = state.history.clone();
        if strip_persona_system {
            let leading_systems = history
                .iter()
                .take_while(|message| message.role == MessageRole::System)
                .count();
            history.drain(..leading_systems);
        }
        let mut projection = state.compaction.projection.clone();
        projection.timeline = timeline_from_history(&history);
        (history, projection)
    }

    pub(crate) fn durable_snapshot(&self) -> (Context, Projection, Vec<Message>) {
        self.durable_snapshot_with_hook(|| {})
    }

    fn durable_snapshot_with_hook(
        &self,
        after_history: impl FnOnce(),
    ) -> (Context, Projection, Vec<Message>) {
        let _transaction = self.persistence_transaction.lock().unwrap();
        let (history, projection) = self.persistence_snapshot();
        after_history();
        let mailbox = self.mailbox_snapshot();
        (history, projection, mailbox)
    }

    #[cfg(test)]
    pub(crate) fn durable_snapshot_paused_after_history(
        &self,
        entered: std::sync::mpsc::Sender<()>,
        release: std::sync::mpsc::Receiver<()>,
    ) -> (Context, Projection, Vec<Message>) {
        self.durable_snapshot_with_hook(|| {
            entered.send(()).unwrap();
            release.recv().unwrap();
        })
    }

    /// Read-only snapshot of accumulated usage.
    pub fn usage(&self) -> Usage {
        self.state.read().unwrap().usage
    }

    /// Read-only snapshot of usage accumulated across the session.
    pub fn total_usage(&self) -> Usage {
        self.state.read().unwrap().total_usage
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

    /// Intersect the persisted user policy with this provider's advertised
    /// capability. Missing capability or policy-off yields `None` (do not
    /// advertise). Config never unions.
    fn web_search_request(&self, provider: &Arc<dyn Provider>) -> Option<WebSearchRequest> {
        let policy = self
            .firmius_config
            .read()
            .unwrap()
            .as_ref()
            .map(|config| config.lock().unwrap().general.web_search.clone())
            .unwrap_or_else(|| FirmiusConfig::load().unwrap_or_default().general.web_search);
        intersect_web_search(
            policy.as_deref(),
            provider.capabilities().web_search.as_ref(),
        )
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

    /// Attach the live `FirmiusConfig` so request construction can intersect
    /// hosted-search policy without re-reading disk each generation.
    pub fn attach_firmius_config(&self, config: Arc<std::sync::Mutex<FirmiusConfig>>) {
        *self.firmius_config.write().unwrap() = Some(config);
    }

    /// Supply model metadata from a runtime catalog without changing provider
    /// resolution. This is useful for providers whose catalog is discovered
    /// separately from the provider instance; it is also a safe seam for
    /// deterministic providers used by integration audits.
    pub fn set_model_metadata(
        &self,
        provider_id: impl Into<String>,
        metadata: crate::types::ModelInfo,
    ) -> Result<(), AgentError> {
        let _guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;
        *self.model_metadata.write().unwrap() = Some((provider_id.into(), metadata));
        Ok(())
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
        self.submit_message(Message::text(MessageRole::User, message.into()));
    }

    /// Enqueue input, durably snapshot it when session-attached, and schedule
    /// a turn. The wake waiter serializes behind any active prompt/compaction.
    pub fn submit_and_wake(&self, message: impl Into<String>) -> Result<(), String> {
        self.submit_message_and_wake(Message::text(MessageRole::User, message.into()))
    }

    pub fn submit_message_and_wake(&self, message: Message) -> Result<(), String> {
        self.submit_message(message);
        let Some(session) = self
            .session_handle
            .read()
            .unwrap()
            .as_ref()
            .and_then(Weak::upgrade)
        else {
            return Ok(());
        };
        session.save()?;
        if let Some(agent) = session.agent(&self.id) {
            session.wake_agent(agent);
        }
        Ok(())
    }

    pub fn submit_message(&self, message: Message) {
        self.mailbox.lock().unwrap().push_back(message);
    }

    /// Snapshot pending user messages for presentation by interactive clients.
    pub fn pending_messages(&self) -> Vec<String> {
        self.mailbox
            .lock()
            .unwrap()
            .iter()
            .map(render_user_message)
            .collect()
    }

    pub fn mailbox_snapshot(&self) -> Vec<Message> {
        self.mailbox.lock().unwrap().iter().cloned().collect()
    }

    /// Atomically remove all currently pending messages. Messages submitted
    /// after the lock is released remain queued for the following turn.
    fn drain_mailbox(&self) -> Vec<Message> {
        self.mailbox.lock().unwrap().drain(..).collect()
    }

    fn drain_mailbox_into_history(
        &self,
        mut inject: impl FnMut(&[Message], &mut AgentState),
    ) -> Vec<Message> {
        let _transaction = self.persistence_transaction.lock().unwrap();
        let pending = self.drain_mailbox();
        if !pending.is_empty() {
            inject(&pending, &mut self.state.write().unwrap());
        }
        pending
    }

    #[cfg(test)]
    pub(crate) fn move_mailbox_to_history_for_test(&self) -> Vec<Message> {
        self.drain_mailbox_into_history(|pending, state| {
            state.history.extend(pending.iter().cloned());
        })
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
        let scopes: HashSet<String> = persona.tool_scopes.iter().cloned().collect();
        Ok((Some(persona.system_prompt.clone()), Some(scopes)))
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
        if let Some(summary) = self
            .state
            .read()
            .unwrap()
            .compaction
            .projection
            .snapshot
            .as_ref()
            .map(|snapshot| snapshot.summary.clone())
        {
            let at = history
                .iter()
                .take_while(|message| message.role == MessageRole::System)
                .count();
            history.insert(at, Message::text(MessageRole::System, summary));
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

    /// Commit a provider selected by the retry loop after that provider has
    /// completed a generation successfully. Unlike [`set_provider`], this is
    /// called from inside the active turn while `busy` is intentionally held.
    /// Failed and cancelled fallback attempts never reach this method, so they
    /// cannot displace the agent's last known-good account.
    fn commit_successful_retry_provider(&self, provider_id: String, provider: Arc<dyn Provider>) {
        self.config.write().unwrap().provider_id = provider_id;
        *self.provider.write().unwrap() = provider;
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
        let (removed, history) = {
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
            let history = s.history.clone();
            (removed, history)
        };
        self.invalidate_compaction(timeline_from_history(&history));
        Ok(removed)
    }

    /// Run the loop until the stop policy fires, returning the final text.
    /// On cancellation the error carries the partial text accumulated so far.
    pub async fn prompt(
        &self,
        user_input: impl Into<String>,
        cancellation: CancellationToken,
        observer: impl FnMut(AgentEvent),
    ) -> Result<String, AgentError> {
        self.prompt_message(
            Message::text(MessageRole::User, user_input.into()),
            cancellation,
            observer,
        )
        .await
    }

    pub async fn prompt_message(
        &self,
        user_message: Message,
        cancellation: CancellationToken,
        mut observer: impl FnMut(AgentEvent),
    ) -> Result<String, AgentError> {
        // Serialize turns on this agent; this same guard is what the
        // between-turn mutators check before touching config or history.
        let _turn_guard = self.busy.try_lock().map_err(|_| AgentError::Busy)?;

        self.run_prompt(Some(user_message), cancellation, &mut observer, None)
            .await
    }

    /// Start a turn when idle, or atomically enqueue the input when another
    /// turn owns the agent. Interactive clients use this instead of a racy
    /// `is_busy()` check followed by `prompt_message()`.
    pub async fn prompt_message_or_submit(
        &self,
        user_message: Message,
        cancellation: CancellationToken,
        mut observer: impl FnMut(AgentEvent),
    ) -> Result<Option<String>, AgentError> {
        let Ok(_turn_guard) = self.busy.try_lock() else {
            self.submit_message_and_wake(user_message)
                .map_err(AgentError::Persistence)?;
            return Ok(None);
        };
        self.run_prompt(Some(user_message), cancellation, &mut observer, None)
            .await
            .map(Some)
    }

    /// Wake an agent for messages already in its mailbox. This waits behind
    /// an active turn, then atomically checks the mailbox while holding the
    /// turn lock: either that turn consumed the messages, or this call starts
    /// the next turn. The check prevents a late wake task from producing an
    /// empty extra provider request.
    pub async fn wake_mailbox(
        &self,
        cancellation: CancellationToken,
        mut observer: impl FnMut(AgentEvent),
    ) -> Result<Option<String>, AgentError> {
        let _turn_guard = self.busy.lock().await;
        if self.mailbox.lock().unwrap().is_empty() {
            return Ok(None);
        }
        let mut rollback_state = self.state.read().unwrap().clone();
        let mut rollback_batch = Vec::new();
        match self
            .run_prompt(
                None,
                cancellation,
                &mut observer,
                Some((&mut rollback_state, &mut rollback_batch)),
            )
            .await
        {
            Ok(text) => Ok(Some(text)),
            Err(error) => {
                // An automatically-triggered turn has no caller available to
                // resubmit failed input. Roll back its trajectory and put the
                // original batch ahead of anything that arrived meanwhile.
                let _transaction = self.persistence_transaction.lock().unwrap();
                *self.state.write().unwrap() = rollback_state;
                let mut mailbox = self.mailbox.lock().unwrap();
                let arrived_during_turn = mailbox.drain(..).collect::<Vec<_>>();
                mailbox.extend(rollback_batch);
                mailbox.extend(arrived_during_turn);
                Err(error)
            }
        }
    }

    async fn run_prompt(
        &self,
        user_message: Option<Message>,
        cancellation: CancellationToken,
        mut observer: impl FnMut(AgentEvent),
        mut rollback: Option<(&mut AgentState, &mut Vec<Message>)>,
    ) -> Result<String, AgentError> {
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
        let mut initial_submission = user_message.is_some();
        if let Some(user_message) = user_message {
            self.submit_message(user_message);
        }

        // Config can't change while we hold `busy`, so the snapshot above is
        // valid for every iteration of the loop.
        let bus = self.bus.read().unwrap().clone();
        // Tee every event into the session bus (when attached) and the
        // caller's observer.
        let mut emit = |event: AgentEvent| {
            if let Some(tx) = &bus {
                // Session owns sequencing and the typed envelope. Keep this
                // compatibility path for agents constructed outside a live
                // Session, while session-attached agents use the coordinator.
                if let Some(session) = self
                    .session_handle
                    .read()
                    .unwrap()
                    .as_ref()
                    .and_then(Weak::upgrade)
                {
                    session.publish_agent_event(self.id.clone(), event.clone());
                } else {
                    let _ = tx.send(crate::session::SessionEvent {
                        session_id: self.session_id.clone(),
                        sequence: 0,
                        at: chrono::Utc::now(),
                        payload: crate::session::SessionEventPayload::Agent {
                            agent_id: self.id.clone(),
                            event: event.clone(),
                        },
                    });
                }
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
            let pending = self.drain_mailbox_into_history(|pending, state| {
                state.history.extend(pending.iter().cloned());
            });
            if !pending.is_empty() {
                if let Some((_, batch)) = rollback.as_mut() {
                    batch.extend(pending.iter().cloned());
                }
                let injected = pending.len().saturating_sub(initial_submission as usize);
                let mut events = Vec::new();
                for (index, message) in pending.iter().enumerate() {
                    if !initial_submission || index < injected {
                        events.push(AgentEvent::UserMessage(render_user_message(message)));
                    }
                }
                for event in events {
                    emit(event);
                }
                initial_submission = false;
            }
            {
                let history = &self.state.read().unwrap().history;
                validate_context(history).map_err(AgentError::Trajectory)?;
            }

            // Context metadata is optional.  If it is unavailable this is a
            // strict no-op, preserving the pre-Phase-4A request path.  This
            // preflight is deliberately run for every generation, including
            // generations following tool results.
            self.sync_compaction_projection();
            self.poll_compaction(&mut emit);
            let web_search = self.web_search_request(&self.provider());
            let build_request = || ProviderRequest {
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
                session_id: Some(self.session_id.clone()),
                web_search: web_search.clone(),
            };
            let request = build_request();
            let budget = self
                .model_info(&config.provider_id, &config.model)
                .as_ref()
                .map(|model| context_budget::assessment(model, &request, BudgetConfig::default()));
            if let Some(assessment) = budget {
                match assessment.decision {
                    BudgetDecision::Within => {}
                    BudgetDecision::Soft => {
                        if self.compaction_job.lock().unwrap().is_none()
                            && let Some((plan, input, provider_id, provider)) =
                                self.compaction_input()?
                        {
                            self.schedule_compaction(
                                plan.clone(),
                                input,
                                provider_id,
                                provider,
                                &mut emit,
                            );
                        }
                    }
                    BudgetDecision::Hard => {
                        let compaction = self.compaction_input()?;
                        if let Some((plan, input, provider_id, provider)) = compaction {
                            self.schedule_compaction(plan, input, provider_id, provider, &mut emit);
                            self.wait_compaction(&cancellation, &mut emit).await?;

                            // Reassess exactly once after the committed
                            // projection changed. Remaining above the
                            // proactive threshold is safe; exceeding the
                            // usable budget is not.
                            let rebuilt = build_request();
                            if let Some(model) = self.model_info(&config.provider_id, &config.model)
                            {
                                let assessment = context_budget::assessment(
                                    &model,
                                    &rebuilt,
                                    BudgetConfig::default(),
                                );
                                if assessment.exceeds_usable_input() {
                                    return Err(AgentError::ContextBudget {
                                        estimated_input: assessment.estimated_input,
                                        usable_input: assessment.usable_input,
                                    });
                                }
                            }
                        } else if assessment.exceeds_usable_input() {
                            // Never send a request that cannot fit merely
                            // because no complete segment is safe to remove.
                            return Err(AgentError::ContextBudgetNoSafeBoundary);
                        }
                    }
                }
            }

            // Hard-threshold compaction can complete synchronously above.  The
            // projection is now current, so rebuild the request unconditionally
            // before entering the provider.
            let request = build_request();

            let (assistant, reason, turn_text, turn_usage) = self
                .run_generation(request, &cancellation, &mut emit)
                .await?;
            {
                let mut s = self.state.write().unwrap();
                s.usage = turn_usage;
                s.total_usage = s.total_usage.saturating_add(turn_usage);
                s.history.push(assistant.clone());
            }
            final_text = turn_text;
            emit(AgentEvent::TurnFinished);

            // Provider output is an externally-observed commit boundary. A
            // later failure must preserve it and retry only input consumed
            // after this point (likewise avoiding repeated tool side effects).
            if let Some((state, batch)) = rollback.as_mut() {
                **state = self.state.read().unwrap().clone();
                batch.clear();
            }

            if self.stop_policy.should_stop(&assistant, reason) {
                // A response that would normally end the run must yield to
                // input submitted while it was in flight. Drain the complete
                // batch before the next provider request so queued messages
                // are handled together, not as separate turns.
                let pending = self.drain_mailbox_into_history(|pending, state| {
                    state.history.extend(pending.iter().cloned());
                });
                if !pending.is_empty() {
                    if let Some((_, batch)) = rollback.as_mut() {
                        batch.extend(pending.iter().cloned());
                    }
                    let events = pending
                        .iter()
                        .map(|message| AgentEvent::UserMessage(render_user_message(message)))
                        .collect::<Vec<_>>();
                    for event in events {
                        emit(event);
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
                    allowed_scopes: turn_scopes.clone(),
                };
                let (content, ok) = match self
                    .tools
                    .call_output_scoped(&name, parsed, ctx, turn_scopes.as_ref())
                    .await
                {
                    Ok(crate::tools::ToolOutput::Content(output)) => {
                        (crate::tools::redirect_large_tool_result(output), true)
                    }
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
            if let Some((state, batch)) = rollback.as_mut() {
                **state = self.state.read().unwrap().clone();
                batch.clear();
            }
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
        let configured_provider_id = self.config.read().unwrap().provider_id.clone();
        self.refresh_account_selection(&configured_provider_id)
            .await;
        let runtime = self.retry_runtime(&configured_provider_id);
        let Some(selected_provider_id) = runtime.accounts.first().cloned() else {
            return Err(AgentError::Provider(ProviderError::Auth(format!(
                "no usable account remains for provider {configured_provider_id}"
            ))));
        };
        let mut controller = RetryController::new(runtime.config, runtime.accounts);
        let mut current_provider_id = selected_provider_id.clone();
        let mut current_provider = if selected_provider_id == configured_provider_id {
            self.provider.read().unwrap().clone()
        } else {
            let Some(manager) = self.provider_manager.read().unwrap().clone() else {
                return Err(AgentError::Provider(ProviderError::Auth(
                    "best-account selection requires an attached ProviderManager".into(),
                )));
            };
            let provider = manager
                .lock()
                .unwrap()
                .build(&selected_provider_id)
                .map_err(|message| AgentError::Provider(ProviderError::Auth(message)))?;
            provider
        };

        'attempt: loop {
            // Retries are independent provider dispatches.  Keep the guard
            // here as well as in the outer ReAct loop so a retry can never
            // bypass the context-budget gate.
            if let Some(model) = self.model_info(&current_provider_id, &request.model) {
                let assessment =
                    context_budget::assessment(&model, &request, BudgetConfig::default());
                if assessment.exceeds_usable_input() {
                    return Err(AgentError::ContextBudget {
                        estimated_input: assessment.estimated_input,
                        usable_input: assessment.usable_input,
                    });
                }
            }
            let mut stream = match tokio::select! {
                biased;
                _ = cancellation.cancelled() => {
                    return Ok((
                        build_assistant_message(String::new(), None, String::new(), Vec::new()),
                        StopReason::Cancelled,
                        String::new(),
                        Usage::default(),
                    ));
                }
                result = current_provider.stream(request.clone()) => result,
            } {
                Ok(stream) => stream,
                Err(error) => match self
                    .retry_on_failure(
                        &mut controller,
                        &mut current_provider,
                        &mut current_provider_id,
                        error,
                        cancellation,
                        observer,
                    )
                    .await?
                {
                    RetryLoop::Continue => continue 'attempt,
                    RetryLoop::Stop(error) => return Err(AgentError::Provider(error)),
                },
            };

            let mut text = String::new();
            let mut thinking = String::new();
            let mut signature: Option<String> = None;
            // Hosted search and function tool-calls, in stream order relative
            // to each other. Text is coalesced from deltas, so these parts are
            // appended after thinking/text rather than interleaved with it.
            let mut extra_parts: Vec<MessagePart> = Vec::new();
            let mut in_progress_search: Vec<String> = Vec::new();
            let mut reason = StopReason::Stop;
            let mut usage = Usage::default();
            let mut saw_visible_output = false;

            loop {
                tokio::select! {
                    biased;
                    _ = cancellation.cancelled() => {
                        finish_open_searches(&mut in_progress_search, &mut extra_parts);
                        let partial = build_assistant_message(thinking, signature, text.clone(), extra_parts);
                        return Ok((partial, StopReason::Cancelled, text, usage));
                    }
                    next = stream.next() => {
                        let Some(event) = next else { break; };
                        match event {
                            Ok(ProviderEvent::TextDelta { delta }) => {
                                saw_visible_output = true;
                                observer(AgentEvent::Text(delta.clone()));
                                text.push_str(&delta);
                            }
                            Ok(ProviderEvent::ThinkingDelta { delta, signature: sig }) => {
                                if !delta.is_empty() {
                                    saw_visible_output = true;
                                    observer(AgentEvent::Thinking(delta.clone()));
                                    thinking.push_str(&delta);
                                }
                                if sig.is_some() {
                                    signature = sig;
                                }
                            }
                            Ok(ProviderEvent::ToolCallDelta { index, id, name_delta, args_delta }) => {
                                saw_visible_output = true;
                                observer(AgentEvent::ToolCallDelta {
                                    index,
                                    id: id.unwrap_or_default(),
                                    name_delta,
                                    args_delta,
                                })
                            }
                            Ok(ProviderEvent::ToolCall { id, name, args }) => {
                                saw_visible_output = true;
                                extra_parts.push(MessagePart::ToolCall { id, name, args });
                            }
                            Ok(ProviderEvent::Usage { usage: u }) => {
                                usage = u;
                                observer(AgentEvent::Usage(u));
                            }
                            Ok(ProviderEvent::Done { reason: r }) => reason = r,
                            Ok(ProviderEvent::WebSearchStarted { id }) => {
                                saw_visible_output = true;
                                observer(AgentEvent::WebSearchStarted { id: id.clone() });
                                in_progress_search.push(id);
                            }
                            Ok(ProviderEvent::WebSearchFinished { id, action }) => {
                                saw_visible_output = true;
                                observer(AgentEvent::WebSearchFinished {
                                    id: id.clone(),
                                    action: action.clone(),
                                });
                                in_progress_search.retain(|open| open != &id);
                                extra_parts.push(MessagePart::WebSearch { id, action });
                            }
                            Err(error) => {
                                if saw_visible_output {
                                    return Err(AgentError::Provider(error));
                                }
                                match self.retry_on_failure(
                                    &mut controller,
                                    &mut current_provider,
                                    &mut current_provider_id,
                                    error,
                                    cancellation,
                                    observer,
                                )
                                .await?
                                {
                                    RetryLoop::Continue => continue 'attempt,
                                    RetryLoop::Stop(error) => return Err(AgentError::Provider(error)),
                                }
                            }
                        }
                    }
                }
            }

            finish_open_searches(&mut in_progress_search, &mut extra_parts);
            // Some providers occasionally terminate a healthy stream without
            // producing any completion content (for example, a lone `Done`).
            // Treat that exactly like a transient decode failure while still
            // inside the base generation loop. This keeps callers and higher
            // layers from having to special-case empty final responses.
            if reason != StopReason::Cancelled
                && text.trim().is_empty()
                && thinking.trim().is_empty()
                && extra_parts.is_empty()
            {
                let error = ProviderError::Decode("provider returned an empty completion".into());
                match self
                    .retry_on_failure(
                        &mut controller,
                        &mut current_provider,
                        &mut current_provider_id,
                        error,
                        cancellation,
                        observer,
                    )
                    .await?
                {
                    RetryLoop::Continue => continue 'attempt,
                    RetryLoop::Stop(error) => return Err(AgentError::Provider(error)),
                }
            }
            let assistant = build_assistant_message(thinking, signature, text.clone(), extra_parts);
            if current_provider_id != configured_provider_id {
                self.commit_successful_retry_provider(
                    current_provider_id.clone(),
                    current_provider.clone(),
                );
            }
            return Ok((assistant, reason, text, usage));
        }
    }

    fn retry_runtime(&self, provider_id: &str) -> RetryRuntime {
        let mut accounts = vec![provider_id.to_string()];
        let mut kind = None;
        if let Some(manager) = self.provider_manager.read().unwrap().clone() {
            let manager = manager.lock().unwrap();
            kind = manager.provider_kind(provider_id).map(ToOwned::to_owned);
            let mut discovered = manager.ranked_accounts_for(provider_id);
            if discovered.is_empty() && !manager.has_account_selection_policy(provider_id) {
                discovered.push(provider_id.to_string());
            }
            discovered.dedup();
            accounts = discovered;
        }
        let config = FirmiusConfig::load()
            .unwrap_or_default()
            .retry
            .resolve(provider_id, kind.as_deref());
        RetryRuntime { config, accounts }
    }

    async fn refresh_account_selection(&self, provider_id: &str) {
        let Some(manager) = self.provider_manager.read().unwrap().clone() else {
            return;
        };
        let sources = {
            let manager = manager.lock().unwrap();
            let lookup = manager.provider_kind(provider_id).unwrap_or(provider_id);
            manager.account_selection_sources(lookup)
        };
        if sources.is_empty() {
            return;
        }
        let results = futures::future::join_all(
            sources
                .into_iter()
                .map(|(account_id, source)| async move { (account_id, source.fetch().await) }),
        )
        .await;
        let manager = manager.lock().unwrap();
        for (account_id, result) in results {
            if let Ok(snapshot) = result {
                manager.cache_account_quota(&account_id, snapshot);
            }
        }
    }

    async fn retry_on_failure(
        &self,
        controller: &mut RetryController,
        current_provider: &mut Arc<dyn Provider>,
        current_provider_id: &mut String,
        error: ProviderError,
        cancellation: &CancellationToken,
        observer: &mut impl FnMut(AgentEvent),
    ) -> Result<RetryLoop, AgentError> {
        if classify(&error) == FailureClass::Auth
            && let Some(manager) = self.provider_manager.read().unwrap().clone()
        {
            manager.lock().unwrap().mark_account_unavailable(
                current_provider_id,
                chrono::Utc::now() + chrono::Duration::minutes(15),
            );
        }
        match controller.on_failure(&error) {
            RetryDecision::Retry {
                account_id,
                delay,
                attempt,
                switched,
            } => {
                observer(AgentEvent::RetryScheduled {
                    account_id: account_id.clone(),
                    attempt,
                    delay_ms: delay.as_millis() as u64,
                    switched,
                    class: classify(&error),
                });
                if switched || account_id != *current_provider_id {
                    let Some(manager) = self.provider_manager.read().unwrap().clone() else {
                        return Ok(RetryLoop::Stop(error));
                    };
                    let provider = manager
                        .lock()
                        .unwrap()
                        .build(&account_id)
                        .map_err(|message| AgentError::Provider(ProviderError::Auth(message)))?;
                    *current_provider = provider;
                    *current_provider_id = account_id;
                }
                tokio::select! {
                    biased;
                    _ = cancellation.cancelled() => {
                        Err(AgentError::Cancelled(String::new()))
                    }
                    _ = tokio::time::sleep(delay) => Ok(RetryLoop::Continue),
                }
            }
            RetryDecision::Exhausted { last_error, .. } => Ok(RetryLoop::Stop(last_error)),
        }
    }
}

fn render_user_message(message: &Message) -> String {
    message
        .content
        .iter()
        .filter_map(|part| match part {
            MessagePart::Text(text) => Some(text.clone()),
            MessagePart::Image(_) => Some("[image]".to_string()),
            MessagePart::Thinking { .. }
            | MessagePart::ToolCall { .. }
            | MessagePart::ToolResult { .. }
            | MessagePart::WebSearch { .. } => None,
        })
        .collect::<Vec<_>>()
        .join("\n")
}

struct RetryRuntime {
    config: crate::config::RetryConfig,
    accounts: Vec<String>,
}

enum RetryLoop {
    Continue,
    Stop(ProviderError),
}

fn finish_open_searches(open: &mut Vec<String>, parts: &mut Vec<MessagePart>) {
    // A started search that never finished still belongs on the assistant
    // message. Record it as Other so repair_dangling_tool_calls has no
    // ToolCall to invent a result for.
    for id in open.drain(..) {
        parts.push(MessagePart::WebSearch {
            id,
            action: crate::types::WebSearchAction::Other,
        });
    }
}

fn build_assistant_message(
    thinking: String,
    signature: Option<String>,
    text: String,
    extra_parts: Vec<MessagePart>,
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
    // Search and tool-call parts follow thinking/text. Stream order among
    // those extra parts is preserved; text itself is coalesced from deltas.
    content.extend(extra_parts);
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

    struct FlakyProvider {
        calls: Arc<std::sync::Mutex<u32>>,
    }

    struct ScriptedProvider {
        turns: std::sync::Mutex<VecDeque<Vec<Result<ProviderEvent, ProviderError>>>>,
        hang_after: bool,
    }

    impl ScriptedProvider {
        fn new(turns: impl IntoIterator<Item = Vec<Result<ProviderEvent, ProviderError>>>) -> Self {
            Self {
                turns: std::sync::Mutex::new(turns.into_iter().collect()),
                hang_after: false,
            }
        }

        fn hang_after(mut self) -> Self {
            self.hang_after = true;
            self
        }
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
            Ok(futures::stream::iter([
                Ok(ProviderEvent::TextDelta { delta: "ok".into() }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                }),
            ])
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
            Ok(futures::stream::iter([
                Ok(ProviderEvent::TextDelta { delta: "ok".into() }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                }),
            ])
            .boxed())
        }
    }

    #[async_trait::async_trait]
    impl Provider for FlakyProvider {
        fn id(&self) -> &str {
            "flaky"
        }

        async fn stream(
            &self,
            _request: ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            let mut calls = self.calls.lock().unwrap();
            *calls += 1;
            if *calls == 1 {
                return Err(ProviderError::Api {
                    status: 503,
                    body: "temporary".into(),
                });
            }
            Ok(futures::stream::iter([
                Ok(ProviderEvent::TextDelta { delta: "ok".into() }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                }),
            ])
            .boxed())
        }
    }

    #[async_trait::async_trait]
    impl Provider for ScriptedProvider {
        fn id(&self) -> &str {
            "scripted"
        }

        async fn stream(
            &self,
            _request: ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            let events = self.turns.lock().unwrap().pop_front().unwrap_or_default();
            if self.hang_after {
                Ok(futures::stream::iter(events)
                    .chain(futures::stream::pending())
                    .boxed())
            } else {
                Ok(futures::stream::iter(events).boxed())
            }
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
    async fn prompt_message_or_submit_enqueues_instead_of_returning_busy() {
        let started = Arc::new(tokio::sync::Notify::new());
        let provider = Arc::new(HangingProvider {
            started: started.clone(),
        });
        let agent = Arc::new(Agent::new(
            provider,
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "blocking".into(),
                model: "test-model".into(),
                ..Default::default()
            },
            "test-session",
        ));
        let cancellation = CancellationToken::new();
        let task_cancellation = cancellation.clone();
        let first = {
            let agent = agent.clone();
            tokio::spawn(async move { agent.prompt("first", task_cancellation, |_| {}).await })
        };
        started.notified().await;

        let result = agent
            .prompt_message_or_submit(
                Message::text(MessageRole::User, "second"),
                CancellationToken::new(),
                |_| {},
            )
            .await
            .unwrap();
        assert!(result.is_none(), "busy path queues rather than starting");
        assert_eq!(agent.pending_messages(), vec!["second"]);

        cancellation.cancel();
        let _ = first.await;
    }

    #[tokio::test]
    async fn hard_but_fitting_request_without_compaction_boundary_is_dispatched_once() {
        let request = Arc::new(RwLock::new(None));
        let agent = Agent::new(
            Arc::new(CaptureProvider {
                request: request.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "capture".into(),
                model: "capture-model".into(),
                ..Default::default()
            },
            "test-session",
        );
        agent
            .set_model_metadata(
                "capture",
                crate::types::ModelInfo {
                    id: "capture-model".into(),
                    context_window: 4_000,
                    max_output_tokens: Some(400),
                    capabilities: Default::default(),
                    effort_modes: Vec::new(),
                },
            )
            .unwrap();

        // Usable input is 3,200 tokens. This one active segment estimates
        // 3,040 (exactly 95%), so it is Hard but still fits. With no older
        // segment there is no compaction boundary; it must fall through to
        // the provider rather than fail or spin the preflight loop.
        tokio::time::timeout(
            std::time::Duration::from_secs(1),
            agent.prompt(&"x".repeat(12_144), CancellationToken::new(), |_| {}),
        )
        .await
        .expect("hard-but-fitting preflight must not spin")
        .expect("hard-but-fitting request must be dispatched");
        assert!(request.read().unwrap().is_some());
    }

    #[tokio::test]
    async fn overflowing_request_without_compaction_boundary_fails_before_dispatch() {
        let request = Arc::new(RwLock::new(None));
        let agent = Agent::new(
            Arc::new(CaptureProvider {
                request: request.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "capture".into(),
                model: "capture-model".into(),
                ..Default::default()
            },
            "test-session",
        );
        agent
            .set_model_metadata(
                "capture",
                crate::types::ModelInfo {
                    id: "capture-model".into(),
                    context_window: 4_000,
                    max_output_tokens: Some(400),
                    capabilities: Default::default(),
                    effort_modes: Vec::new(),
                },
            )
            .unwrap();

        let error = agent
            .prompt(&"x".repeat(13_000), CancellationToken::new(), |_| {})
            .await
            .expect_err("a request above the usable budget must fail closed");
        assert!(matches!(error, AgentError::ContextBudgetNoSafeBoundary));
        assert!(
            request.read().unwrap().is_none(),
            "overflow must not reach the provider"
        );
    }

    #[tokio::test]
    async fn prompt_retries_an_empty_completion_before_returning() {
        let agent = Agent::new(
            Arc::new(ScriptedProvider::new([
                vec![Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                })],
                vec![
                    Ok(ProviderEvent::TextDelta {
                        delta: "recovered".into(),
                    }),
                    Ok(ProviderEvent::Done {
                        reason: StopReason::Stop,
                    }),
                ],
            ])),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "scripted".into(),
                model: "test".into(),
                ..Default::default()
            },
            "session",
        );
        let events = Arc::new(std::sync::Mutex::new(Vec::new()));
        let observed = events.clone();

        let result = agent
            .prompt("finish", CancellationToken::new(), move |event| {
                observed.lock().unwrap().push(event);
            })
            .await
            .unwrap();

        assert_eq!(result, "recovered");
        assert!(events.lock().unwrap().iter().any(|event| matches!(
            event,
            AgentEvent::RetryScheduled {
                class: FailureClass::Decode,
                ..
            }
        )));
    }

    #[test]
    fn committed_summary_survives_projection_sync_and_is_sent_once() {
        let agent = agent_with_history(vec![
            Message::text(MessageRole::System, "persona semantics"),
            Message::text(MessageRole::User, "old question"),
            Message::text(MessageRole::User, "active question"),
        ]);
        agent.sync_compaction_projection();
        let plan = {
            let state = agent.state.read().unwrap();
            compaction::plan(&state.compaction.projection, 0).unwrap()
        };
        let result = CompactionResult {
            summary: "old answer summary".into(),
            generation: plan.generation,
            source_segment_ids: plan.source_segment_ids.clone(),
            source_range: plan.source_range,
            source_entries: plan.source_entries,
            source_content_digest: plan.source_content_digest.clone(),
            usage: Usage::default(),
        };
        agent.state.write().unwrap().usage = Usage {
            input_tokens: 157_000,
            output_tokens: 5_200,
            ..Usage::default()
        };
        agent.commit_compaction(&plan, result).unwrap();
        assert_eq!(agent.usage(), Usage::default());
        agent.sync_compaction_projection();

        let request = agent.messages_for_request(Some("current persona"));
        let summary = MessagePart::Text(
            "<compaction_summary>\nold answer summary\n</compaction_summary>".into(),
        );
        assert_eq!(
            request
                .iter()
                .filter(|message| message.content.contains(&summary))
                .count(),
            1
        );
        assert_eq!(
            request[0].content,
            vec![MessagePart::Text("current persona".into())]
        );
        assert_eq!(request[1].content, vec![summary.clone()]);
        assert!(
            !agent
                .history()
                .iter()
                .any(|message| message.content.contains(&summary))
        );
    }

    #[test]
    fn completed_tool_cycles_create_repeatable_compaction_boundaries() {
        let history = vec![
            Message::text(MessageRole::User, "inspect everything"),
            Message {
                role: MessageRole::Assistant,
                content: vec![MessagePart::ToolCall {
                    id: "call-1".into(),
                    name: "read".into(),
                    args: "{}".into(),
                }],
            },
            Message::tool_results(vec![MessagePart::ToolResult {
                id: "call-1".into(),
                content: "first batch".into(),
                ok: true,
            }]),
            Message {
                role: MessageRole::Assistant,
                content: vec![MessagePart::ToolCall {
                    id: "call-2".into(),
                    name: "read".into(),
                    args: "{}".into(),
                }],
            },
            Message::tool_results(vec![MessagePart::ToolResult {
                id: "call-2".into(),
                content: "second batch".into(),
                ok: true,
            }]),
        ];
        let timeline = timeline_from_history(&history);
        assert_eq!(timeline.segments.len(), 2);
        assert_eq!(timeline.segments[0].entries.len(), 3);
        assert_eq!(timeline.segments[1].entries.len(), 2);
        assert!(compaction::plan(&Projection::new(timeline), 0).is_ok());
    }

    #[test]
    fn rewind_invalidates_committed_summary() {
        let agent = agent_with_history(vec![
            Message::text(MessageRole::User, "old question"),
            Message::text(MessageRole::User, "active question"),
        ]);
        agent.sync_compaction_projection();
        let plan = {
            let state = agent.state.read().unwrap();
            compaction::plan(&state.compaction.projection, 0).unwrap()
        };
        agent
            .commit_compaction(
                &plan,
                CompactionResult {
                    summary: "removed context".into(),
                    generation: plan.generation,
                    source_segment_ids: plan.source_segment_ids.clone(),
                    source_range: plan.source_range,
                    source_entries: plan.source_entries,
                    source_content_digest: plan.source_content_digest.clone(),
                    usage: Usage::default(),
                },
            )
            .unwrap();
        agent.rewind(1).unwrap();
        assert!(agent.compaction_projection().snapshot.is_none());
    }

    #[tokio::test]
    async fn rewind_cancels_pending_compaction_and_advances_generation() {
        let agent = agent_with_history(two_turn_history());
        agent.sync_compaction_projection();
        let (plan, input, provider_id, provider) = agent.compaction_input().unwrap().unwrap();
        agent.schedule_compaction(plan, input, provider_id, provider, &mut |_| {});
        assert!(agent.compaction_job.lock().unwrap().is_some());

        agent.rewind(1).unwrap();

        assert!(agent.compaction_job.lock().unwrap().is_none());
        assert_eq!(agent.compaction_projection().generation, 1);
        assert!(agent.compaction_projection().snapshot.is_none());
    }

    #[test]
    fn restored_projection_summary_is_projected_once() {
        let agent = agent_with_history(vec![Message::text(MessageRole::User, "active question")])
            .with_compaction(Projection {
                generation: 3,
                timeline: Timeline::default(),
                snapshots: vec![],
                snapshot: Some(crate::compaction::Snapshot {
                    generation: 3,
                    source_entries: 1,
                    source_content_digest: String::new(),
                    source_segment_ids: vec!["old".into()],
                    source_range: (0, 1),
                    summary: crate::compaction::format_summary("persisted summary"),
                }),
            });
        let request = agent.messages_for_request(None);
        let summary = MessagePart::Text(
            "<compaction_summary>\npersisted summary\n</compaction_summary>".into(),
        );
        assert_eq!(
            request
                .iter()
                .filter(|message| message.content.contains(&summary))
                .count(),
            1
        );
    }

    #[test]
    fn compaction_result_binding_rejects_mismatched_digest() {
        let agent = agent_with_history(vec![
            Message::text(MessageRole::User, "old question"),
            Message::text(MessageRole::User, "active question"),
        ]);
        agent.sync_compaction_projection();
        let plan = {
            let state = agent.state.read().unwrap();
            compaction::plan(&state.compaction.projection, 0).unwrap()
        };
        let result = CompactionResult {
            summary: "must not apply".into(),
            generation: plan.generation,
            source_segment_ids: plan.source_segment_ids.clone(),
            source_range: plan.source_range,
            source_entries: plan.source_entries,
            source_content_digest: "wrong-source".into(),
            usage: Usage::default(),
        };
        assert!(agent.commit_compaction(&plan, result).is_err());
        assert_eq!(agent.history().len(), 2);
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
        let tools = ToolRegistry::default();
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
        assert_eq!(
            agent.drain_mailbox(),
            vec![
                Message::text(MessageRole::User, "first"),
                Message::text(MessageRole::User, "second"),
                Message::text(MessageRole::User, "third"),
            ]
        );
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

    #[tokio::test]
    async fn prompt_emits_retry_event_before_successful_retry() {
        let calls = Arc::new(std::sync::Mutex::new(0));
        let agent = Agent::new(
            Arc::new(FlakyProvider {
                calls: calls.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "flaky".into(),
                model: "flaky-model".into(),
                ..Default::default()
            },
            "test-session",
        );

        let events = Arc::new(std::sync::Mutex::new(Vec::new()));
        let seen = events.clone();
        let result = agent
            .prompt("hello", CancellationToken::new(), move |event| {
                seen.lock().unwrap().push(event);
            })
            .await
            .unwrap();

        assert_eq!(result, "ok");
        assert_eq!(*calls.lock().unwrap(), 2);
        let events = events.lock().unwrap();
        assert!(events.iter().any(|event| matches!(
            event,
            AgentEvent::RetryScheduled {
                account_id,
                attempt: 2,
                switched: false,
                class: FailureClass::ServerError,
                ..
            } if account_id == "flaky"
        )));
        assert!(
            events
                .iter()
                .any(|event| matches!(event, AgentEvent::Text(text) if text == "ok"))
        );
    }

    #[test]
    fn successful_retry_provider_becomes_the_agents_next_default() {
        let calls = Arc::new(std::sync::Mutex::new(0));
        let agent = Agent::new(
            Arc::new(FlakyProvider {
                calls: calls.clone(),
            }),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "account-a".into(),
                model: "flaky-model".into(),
                ..Default::default()
            },
            "test-session",
        );
        let replacement: Arc<dyn Provider> = Arc::new(FlakyProvider { calls });

        agent.commit_successful_retry_provider("account-b".into(), replacement.clone());

        assert_eq!(agent.config().provider_id, "account-b");
        assert!(Arc::ptr_eq(&agent.provider(), &replacement));
    }

    fn search_action(query: &str) -> crate::types::WebSearchAction {
        crate::types::WebSearchAction::Search {
            query: Some(query.into()),
            queries: None,
        }
    }

    #[tokio::test]
    async fn hosted_search_is_recorded_without_calling_tools() {
        let calls = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let tools = ToolRegistry::default();
        let seen = calls.clone();
        tools.register(crate::tools::TypedTool::new(
            "echo",
            "echo",
            move |_args: serde_json::Value, _ctx: ToolContext| {
                seen.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                Box::pin(async { Ok("ran".to_string()) })
            },
        ));
        let agent = Agent::new(
            Arc::new(ScriptedProvider::new([vec![
                Ok(ProviderEvent::WebSearchStarted { id: "ws-1".into() }),
                Ok(ProviderEvent::WebSearchFinished {
                    id: "ws-1".into(),
                    action: search_action("weather seattle"),
                }),
                Ok(ProviderEvent::TextDelta { delta: "ok".into() }),
                Ok(ProviderEvent::Done {
                    reason: StopReason::Stop,
                }),
            ]])),
            Arc::new(tools),
            AgentConfig {
                provider_id: "scripted".into(),
                model: "scripted-model".into(),
                ..Default::default()
            },
            "test-session",
        );

        let events = Arc::new(std::sync::Mutex::new(Vec::new()));
        let seen_events = events.clone();
        let result = agent
            .prompt("search", CancellationToken::new(), move |event| {
                seen_events.lock().unwrap().push(event);
            })
            .await
            .unwrap();

        assert_eq!(result, "ok");
        assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 0);
        let history = agent.history();
        assert!(validate_context(&history).is_ok());
        let assistant = history
            .iter()
            .find(|message| message.role == MessageRole::Assistant)
            .expect("assistant message");
        assert!(
            assistant.content.iter().any(|part| matches!(
                part,
                MessagePart::WebSearch { id, action }
                    if id == "ws-1"
                        && *action == search_action("weather seattle")
            )),
            "history should record hosted search: {assistant:?}"
        );
        assert!(
            !history.iter().any(|message| message
                .content
                .iter()
                .any(|part| matches!(part, MessagePart::ToolResult { .. }))),
            "search-only turn must not invent a tool result"
        );
        let events = events.lock().unwrap();
        assert!(events.iter().any(|event| matches!(
            event,
            AgentEvent::WebSearchStarted { id } if id == "ws-1"
        )));
        assert!(events.iter().any(|event| matches!(
            event,
            AgentEvent::WebSearchFinished { id, action }
                if id == "ws-1" && *action == search_action("weather seattle")
        )));
        assert!(!events.iter().any(|event| matches!(
            event,
            AgentEvent::ToolCallStarted { .. } | AgentEvent::ToolResult { .. }
        )));
    }

    #[tokio::test]
    async fn hosted_search_plus_function_call_executes_only_the_function() {
        let calls = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let tools = ToolRegistry::default();
        let seen = calls.clone();
        tools.register(crate::tools::TypedTool::new(
            "echo",
            "echo",
            move |_args: serde_json::Value, _ctx: ToolContext| {
                seen.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                Box::pin(async { Ok("ran".to_string()) })
            },
        ));
        let agent = Agent::new(
            Arc::new(ScriptedProvider::new([
                vec![
                    Ok(ProviderEvent::WebSearchFinished {
                        id: "ws-1".into(),
                        action: search_action("weather seattle"),
                    }),
                    Ok(ProviderEvent::ToolCall {
                        id: "c1".into(),
                        name: "echo".into(),
                        args: "{}".into(),
                    }),
                    Ok(ProviderEvent::Done {
                        reason: StopReason::Stop,
                    }),
                ],
                vec![
                    Ok(ProviderEvent::TextDelta {
                        delta: "done".into(),
                    }),
                    Ok(ProviderEvent::Done {
                        reason: StopReason::Stop,
                    }),
                ],
            ])),
            Arc::new(tools),
            AgentConfig {
                provider_id: "scripted".into(),
                model: "scripted-model".into(),
                ..Default::default()
            },
            "test-session",
        );

        let result = agent
            .prompt("search then tool", CancellationToken::new(), |_| {})
            .await
            .unwrap();

        assert_eq!(result, "done");
        assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 1);
        let history = agent.history();
        assert!(validate_context(&history).is_ok());
        assert!(history.iter().any(|message| {
            message
                .content
                .iter()
                .any(|part| matches!(part, MessagePart::WebSearch { id, .. } if id == "ws-1"))
        }));
        assert!(history.iter().any(|message| {
            message.content.iter().any(|part| {
                matches!(
                    part,
                    MessagePart::ToolCall { id, name, .. } if id == "c1" && name == "echo"
                )
            })
        }));
        assert!(history.iter().any(|message| message.content.iter().any(
            |part| matches!(part, MessagePart::ToolResult { id, ok, .. } if id == "c1" && *ok)
        )));
    }

    #[tokio::test]
    async fn interrupted_in_progress_search_does_not_synthesize_a_tool_result() {
        let (started_tx, mut started_rx) = tokio::sync::mpsc::unbounded_channel();
        let agent = Arc::new(Agent::new(
            Arc::new(
                ScriptedProvider::new([vec![Ok(ProviderEvent::WebSearchStarted {
                    id: "ws-1".into(),
                })]])
                .hang_after(),
            ),
            Arc::new(ToolRegistry::default()),
            AgentConfig {
                provider_id: "scripted".into(),
                model: "scripted-model".into(),
                ..Default::default()
            },
            "test-session",
        ));
        let cancellation = CancellationToken::new();
        let task_cancellation = cancellation.clone();
        let agent_for_task = agent.clone();
        let task = tokio::spawn(async move {
            agent_for_task
                .prompt("hang", task_cancellation, move |event| {
                    if matches!(event, AgentEvent::WebSearchStarted { ref id } if id == "ws-1") {
                        let _ = started_tx.send(());
                    }
                })
                .await
        });

        started_rx
            .recv()
            .await
            .expect("search started event was observed");
        cancellation.cancel();

        let result = tokio::time::timeout(std::time::Duration::from_secs(1), task)
            .await
            .expect("cancellation should interrupt the in-progress search")
            .expect("prompt task should not panic");
        assert!(matches!(result, Ok(_)));

        let history = agent.history();
        assert!(
            history.iter().any(|message| message
                .content
                .iter()
                .any(|part| matches!(part, MessagePart::WebSearch { id, action }
                    if id == "ws-1" && *action == crate::types::WebSearchAction::Other))),
            "in-progress search must stay on the assistant message: {history:?}"
        );
        assert!(
            !history.iter().any(|message| message
                .content
                .iter()
                .any(|part| matches!(part, MessagePart::ToolResult { .. }))),
            "interrupt must not invent a ToolResult for hosted search: {history:?}"
        );
        assert!(validate_context(&history).is_ok());
        let mut repaired = history.clone();
        assert_eq!(repair_dangling_tool_calls(&mut repaired), 0);
        assert_eq!(repaired, history);
    }
}
