use chrono::{DateTime, Utc};
use indexmap::IndexMap;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use tokio::sync::Mutex as AsyncMutex;
use tokio::sync::broadcast;
use tokio::task::JoinHandle;
use uuid::Uuid;

use crate::AgentConfig;
use crate::agent::{Agent, AgentError, AgentEvent, PersonaUse};
use crate::artifact::SessionArtifacts;
use crate::persistence::{self, AgentNodeRecord, AgentRecord, SessionRecord};
use crate::persona::PersonaManager;
use crate::providers::manager::ProviderManager;
use crate::tools::ToolRegistry;

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

/// Where an agent sits in the session's spawn tree. Top-level agents (spawned
/// directly by the host program, not by a tool) have `parent_id: None`.
#[derive(Debug, Clone, Default)]
pub struct AgentNode {
    pub parent_id: Option<String>,
    /// The tool_use id (in the parent's history) that spawned this agent,
    /// if any — lets you trace exactly which `delegate` call created it.
    pub spawned_via_tool_call_id: Option<String>,
}

// ---------------------------------------------------------------------------
// Event bus
// ---------------------------------------------------------------------------

/// Capacity of the session event bus. Receivers that fall behind get
/// `RecvError::Lagged` and should re-derive state from the relevant agent's
/// `history()` — cheap, and the same path used to render resumed sessions.
pub const SESSION_EVENT_CAPACITY: usize = 4096;

/// One agent's event, tagged so a single channel can carry a whole session's
/// activity to any number of subscribers (TUI, loggers, replay tools).
#[derive(Debug, Clone)]
pub struct SessionEvent {
    pub agent_id: String,
    pub event: AgentEvent,
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/// Agents live behind `Arc` so they can be handed out as cheap, stable
/// handles (e.g. into `ToolContext` for a `delegate` tool to spawn more
/// agents while another agent's turn is still running) without invalidating
/// references on `Vec` reallocation.
pub type Agents = IndexMap<String, Arc<Agent>>;

/// A backgrounded `delegate` call (mode `spawn`), trackable via `poll`/`wait`.
pub struct DelegateHandle {
    pub agent_id: String,
    pub join: JoinHandle<Result<String, AgentError>>,
}

/// Read-only status of a backgrounded delegate, for UIs (counts, trees).
#[derive(Debug, Clone)]
pub struct DelegateStatus {
    pub delegate_id: String,
    pub agent_id: String,
    pub finished: bool,
}

pub struct Session {
    pub id: String,
    pub title: Option<String>,
    pub created_at: DateTime<Utc>,
    pub agents: Agents,
    pub hierarchy: HashMap<String, AgentNode>,
    /// Set once via `bind_self` right after wrapping a `Session` in
    /// `Arc<tokio::sync::Mutex<_>>`. Lets `spawn_agent`/`spawn_subagent`
    /// wire each new agent's `session_handle` automatically, so a `delegate`
    /// tool call — itself running inside a spawned agent — can spawn further
    /// subagents without the caller having to remember to re-attach.
    self_handle: Option<std::sync::Weak<tokio::sync::Mutex<Session>>>,
    /// Backgrounded delegate calls (`delegate` tool, `mode: "spawn"`),
    /// keyed by a fresh `delegate_id` returned to the caller. Not persisted:
    /// a background task can't survive a process restart any more than a
    /// spawned OS process can (see `Host`) — on resume, in-flight delegates
    /// are simply gone; their agent's history up to that point still is.
    delegates: AsyncMutex<HashMap<String, DelegateHandle>>,
    /// Ids already collected via `take_delegate`/`wait` — tombstones so a
    /// finished-and-gone delegate is distinguishable from one that never
    /// existed. Not persisted: like the delegates themselves, this is
    /// process-lifetime state.
    collected: AsyncMutex<HashSet<String>>,
    /// Broadcast bus carrying every agent's events (see `SessionEvent`).
    /// Agents are wired into it by `bind_self` / `spawn_agent` /
    /// `spawn_subagent`; `prompt()` tees into it automatically.
    events_tx: broadcast::Sender<SessionEvent>,
    /// Session-wide artifact store, shared by every agent and persisted with
    /// the session record. Addressable as `artifact://<path>`.
    pub artifacts: Arc<SessionArtifacts>,
}

impl Default for Session {
    fn default() -> Self {
        Self::new()
    }
}

impl Session {
    pub fn new() -> Self {
        let (events_tx, _) = broadcast::channel(SESSION_EVENT_CAPACITY);
        Session {
            agents: Agents::new(),
            hierarchy: HashMap::new(),
            id: Uuid::new_v4().to_string(),
            title: None,
            created_at: Utc::now(),
            self_handle: None,
            delegates: AsyncMutex::new(HashMap::new()),
            collected: AsyncMutex::new(HashSet::new()),
            events_tx,
            artifacts: Arc::new(SessionArtifacts::new()),
        }
    }

    /// Record this session's own `Arc<Mutex<_>>` handle so future
    /// `spawn_agent`/`spawn_subagent` calls can attach it to new agents.
    /// Call once, right after wrapping a `Session` for shared use:
    /// ```ignore
    /// let session = Arc::new(tokio::sync::Mutex::new(Session::new()));
    /// session.lock().await.bind_self(&session);
    /// ```
    pub fn bind_self(&mut self, handle: &Arc<tokio::sync::Mutex<Session>>) {
        self.self_handle = Some(Arc::downgrade(handle));
        for agent in self.agents.values() {
            agent.attach_session(handle.clone());
            agent.attach_bus(self.events_tx.clone());
        }
    }

    fn attach_self_handle(&self, agent: &Agent) {
        agent.attach_bus(self.events_tx.clone());
        if let Some(weak) = &self.self_handle
            && let Some(strong) = weak.upgrade()
        {
            agent.attach_session(strong);
        }
    }

    /// Subscribe to every event from every agent in this session. Late
    /// subscribers are fine: render existing state from agent histories
    /// first, then consume live events from the receiver.
    pub fn subscribe(&self) -> broadcast::Receiver<SessionEvent> {
        self.events_tx.subscribe()
    }

    /// The bus sender, for wiring agents spawned outside the usual
    /// `spawn_agent`/`spawn_subagent` paths.
    pub fn event_sender(&self) -> broadcast::Sender<SessionEvent> {
        self.events_tx.clone()
    }

    /// Create a top-level agent bound to this session (no parent).
    pub fn spawn_agent(
        &mut self,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
    ) -> Arc<Agent> {
        let agent = Arc::new(Agent::new(provider, tools, config, self.id.clone()));
        self.agents.insert(agent.id.clone(), agent.clone());
        self.hierarchy.insert(
            agent.id.clone(),
            AgentNode {
                parent_id: None,
                spawned_via_tool_call_id: None,
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    pub fn spawn_agent_with_personas(
        &mut self,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
        personas: Arc<PersonaManager>,
    ) -> Arc<Agent> {
        let agent = Arc::new(Agent::new_with_personas(
            provider,
            tools,
            config,
            self.id.clone(),
            personas,
        ));
        self.agents.insert(agent.id.clone(), agent.clone());
        self.hierarchy.insert(
            agent.id.clone(),
            AgentNode {
                parent_id: None,
                spawned_via_tool_call_id: None,
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    /// Create a subagent under `parent_id`, recording the tool call (if any)
    /// that spawned it so the tree is fully traceable.
    pub fn spawn_subagent(
        &mut self,
        parent_id: &str,
        spawned_via_tool_call_id: Option<String>,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
    ) -> Arc<Agent> {
        let agent = Arc::new(Agent::new(provider, tools, config, self.id.clone()));
        self.agents.insert(agent.id.clone(), agent.clone());
        self.hierarchy.insert(
            agent.id.clone(),
            AgentNode {
                parent_id: Some(parent_id.to_string()),
                spawned_via_tool_call_id,
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    pub fn spawn_subagent_with_personas(
        &mut self,
        parent_id: &str,
        spawned_via_tool_call_id: Option<String>,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
        personas: Arc<PersonaManager>,
    ) -> Arc<Agent> {
        let agent = Agent::new_with_personas(provider, tools, config, self.id.clone(), personas);
        if let Err(error) = agent.set_persona_context(PersonaUse::Delegate) {
            eprintln!(
                "warning: session {}: spawned subagent has an invalid persona ({error})",
                self.id
            );
        }
        let agent = Arc::new(agent);
        self.agents.insert(agent.id.clone(), agent.clone());
        self.hierarchy.insert(
            agent.id.clone(),
            AgentNode {
                parent_id: Some(parent_id.to_string()),
                spawned_via_tool_call_id,
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    /// Look up a live agent handle by id.
    pub fn agent(&self, id: &str) -> Option<Arc<Agent>> {
        self.agents.get(id).cloned()
    }

    // ------------------------------------------------------------------
    // Delegate tracking (backgrounded `delegate` tool calls)
    // ------------------------------------------------------------------

    /// Register a backgrounded delegate task under a fresh id.
    pub async fn register_delegate(
        &self,
        agent_id: String,
        join: JoinHandle<Result<String, AgentError>>,
    ) -> String {
        let delegate_id = Uuid::new_v4().to_string();
        self.register_delegate_named(delegate_id.clone(), agent_id, join)
            .await;
        delegate_id
    }

    /// Register a backgrounded delegate task under a caller-chosen id. Used by
    /// the `delegate spawn` tool so the spawned task can record its own
    /// `delegate_id` in the result artifact before the id is returned.
    pub async fn register_delegate_named(
        &self,
        delegate_id: String,
        agent_id: String,
        join: JoinHandle<Result<String, AgentError>>,
    ) {
        self.delegates
            .lock()
            .await
            .insert(delegate_id, DelegateHandle { agent_id, join });
    }

    /// Non-blocking status check: `Some(agent_id)` while still running
    /// (caller can read live progress via `Session::agent(agent_id).history()`),
    /// or `None` if `delegate_id` is unknown/already collected via `wait`.
    /// Does not consume the handle — call `wait_delegate` to collect the result.
    pub async fn poll_delegate(&self, delegate_id: &str) -> Option<(String, bool)> {
        let delegates = self.delegates.lock().await;
        let handle = delegates.get(delegate_id)?;
        Some((handle.agent_id.clone(), handle.join.is_finished()))
    }

    /// Read-only snapshot of every backgrounded delegate still tracked
    /// (running, or finished but not yet collected via `wait`). For UIs:
    /// background-agent counts, agent trees, delegate panes.
    pub async fn active_delegates(&self) -> Vec<DelegateStatus> {
        let delegates = self.delegates.lock().await;
        delegates
            .iter()
            .map(|(id, h)| DelegateStatus {
                delegate_id: id.clone(),
                agent_id: h.agent_id.clone(),
                finished: h.join.is_finished(),
            })
            .collect()
    }

    /// Block until a backgrounded delegate finishes, removing it from
    /// tracking and returning its final text (or the `AgentError` it failed
    /// with). Errors with a message if `delegate_id` is unknown.
    pub async fn wait_delegate(
        &self,
        delegate_id: &str,
    ) -> Result<Result<String, AgentError>, String> {
        let handle = self.take_delegate(delegate_id).await?;
        handle
            .join
            .await
            .map_err(|e| format!("delegate task panicked: {e}"))
    }

    /// Remove a delegate handle without awaiting it. Callers that already
    /// hold the session mutex can use this to release the mutex before
    /// awaiting the task, avoiding a deadlock when the delegate itself calls
    /// a session-aware tool.
    pub async fn take_delegate(&self, delegate_id: &str) -> Result<DelegateHandle, String> {
        let removed = self.delegates.lock().await.remove(delegate_id);
        if let Some(handle) = removed {
            self.collected.lock().await.insert(delegate_id.to_string());
            return Ok(handle);
        }
        if self.collected.lock().await.contains(delegate_id) {
            return Err(format!("delegate already collected: {delegate_id}"));
        }
        Err(format!("unknown delegate_id: {delegate_id}"))
    }

    /// Whether a delegate id was already collected via `wait`/`take` —
    /// lets callers distinguish "finished and gone" from "never existed".
    pub async fn was_delegate_collected(&self, delegate_id: &str) -> bool {
        self.collected.lock().await.contains(delegate_id)
    }

    // ------------------------------------------------------------------
    // Persistence
    // ------------------------------------------------------------------

    /// Rebuild a session from a persisted record. Every agent is rebuilt
    /// exactly: same id, same history, same place in the hierarchy. Each
    /// agent's provider is resolved from its own `provider_id` via `mgr`;
    /// if that provider is no longer registered, the agent is skipped with
    /// a warning (rather than failing the whole resume) since a session
    /// can otherwise still be inspected/continued for its other agents.
    pub fn from_record(
        record: SessionRecord,
        mgr: &ProviderManager,
        tools: Arc<ToolRegistry>,
    ) -> Self {
        Self::from_record_with_personas(record, mgr, tools, Arc::new(PersonaManager::empty()))
    }

    pub fn from_record_with_personas(
        record: SessionRecord,
        mgr: &ProviderManager,
        tools: Arc<ToolRegistry>,
        personas: Arc<PersonaManager>,
    ) -> Self {
        let mut session = Session {
            id: record.id,
            title: record.title,
            created_at: record.created_at,
            agents: Agents::new(),
            hierarchy: HashMap::new(),
            self_handle: None,
            delegates: AsyncMutex::new(HashMap::new()),
            collected: AsyncMutex::new(HashSet::new()),
            events_tx: {
                let (tx, _) = broadcast::channel(SESSION_EVENT_CAPACITY);
                tx
            },
            artifacts: Arc::new(SessionArtifacts::from_records(record.artifacts)),
        };

        for ar in record.agents {
            let provider = match mgr.build(&ar.provider_id) {
                Ok(p) => p,
                Err(e) => {
                    eprintln!(
                        "warning: session {}: agent {} used provider '{}' which is unavailable ({e}); skipping",
                        session.id, ar.id, ar.provider_id
                    );
                    continue;
                }
            };
            let config = AgentConfig {
                provider_id: ar.provider_id,
                model: ar.model,
                effort: ar.effort,
                system_prompt: ar.system_prompt,
                persona: ar.persona,
                temperature: ar.temperature,
                max_tokens: ar.max_tokens,
                workdir: ar.workdir,
            };
            // Heal trajectories interrupted mid-tool before they become
            // promptable again (providers reject dangling tool calls).
            let mut history = ar.history;
            crate::types::repair_dangling_tool_calls(&mut history);
            let node = record
                .hierarchy
                .get(&ar.id)
                .map(|n| AgentNode {
                    parent_id: n.parent_id.clone(),
                    spawned_via_tool_call_id: n.spawned_via_tool_call_id.clone(),
                })
                .unwrap_or_default();
            let agent = Agent::new_with_personas(
                provider,
                tools.clone(),
                config,
                session.id.clone(),
                personas.clone(),
            )
            .with_history(history)
            .with_compaction(ar.compaction.unwrap_or_else(|| {
                crate::compaction::Projection::new(crate::compaction::Timeline::default())
            }));
            if node.parent_id.is_some()
                && let Err(error) = agent.set_persona_context(PersonaUse::Delegate)
            {
                eprintln!(
                    "warning: session {}: agent {} has an invalid delegated persona ({error}); skipping",
                    session.id, ar.id
                );
                continue;
            }
            // Preserve the original agent id so the hierarchy map (keyed on
            // it) still lines up, and so any external references (e.g. a
            // saved delegate_id) remain valid across resume.
            let agent = agent.with_id(ar.id.clone());
            let agent = Arc::new(agent);
            session.agents.insert(ar.id.clone(), agent);

            session.hierarchy.insert(ar.id, node);
        }

        session
    }

    /// Load + rebuild a session in one call.
    pub fn resume(
        id: &str,
        mgr: &ProviderManager,
        tools: Arc<ToolRegistry>,
    ) -> Result<Self, String> {
        let record = persistence::load_session_record(id)?;
        Ok(Self::from_record(record, mgr, tools))
    }

    pub fn resume_with_personas(
        id: &str,
        mgr: &ProviderManager,
        tools: Arc<ToolRegistry>,
        personas: Arc<PersonaManager>,
    ) -> Result<Self, String> {
        let record = persistence::load_session_record(id)?;
        Ok(Self::from_record_with_personas(
            record, mgr, tools, personas,
        ))
    }

    /// Snapshot every agent's config + history + the hierarchy into a
    /// record, deriving a title from the first user message if one hasn't
    /// been set yet, and persist it to `~/.firmius/sessions/<id>.json`.
    pub fn save(&mut self) -> Result<(), String> {
        if self.title.is_none() {
            self.title = self.derive_title();
        }

        let agents = self
            .agents
            .values()
            .map(|agent| {
                let cfg = agent.config().clone();
                let (history, compaction) = agent.persistence_snapshot();
                AgentRecord {
                    id: agent.id.clone(),
                    provider_id: cfg.provider_id,
                    model: cfg.model,
                    effort: cfg.effort,
                    system_prompt: cfg.system_prompt,
                    persona: cfg.persona,
                    temperature: cfg.temperature,
                    max_tokens: cfg.max_tokens,
                    workdir: cfg.workdir,
                    history,
                    compaction: Some(compaction),
                }
            })
            .collect();

        let hierarchy = self
            .hierarchy
            .iter()
            .map(|(id, node)| {
                (
                    id.clone(),
                    AgentNodeRecord {
                        parent_id: node.parent_id.clone(),
                        spawned_via_tool_call_id: node.spawned_via_tool_call_id.clone(),
                    },
                )
            })
            .collect();

        let record = SessionRecord {
            id: self.id.clone(),
            title: self.title.clone(),
            created_at: self.created_at,
            updated_at: Utc::now(),
            agents,
            hierarchy,
            artifacts: self.artifacts.snapshot(),
        };
        persistence::save_session_record(&record)
    }

    /// First user message across all agents (by insertion order), truncated
    /// to a short title. `None` if no agent has been prompted yet.
    fn derive_title(&self) -> Option<String> {
        for agent in self.agents.values() {
            for msg in agent.history() {
                if msg.role != crate::types::MessageRole::User {
                    continue;
                }
                let text: String = msg
                    .content
                    .iter()
                    .filter_map(|part| match part {
                        crate::types::MessagePart::Text(t) => Some(t.as_str()),
                        _ => None,
                    })
                    .collect::<Vec<_>>()
                    .join(" ");
                let text = text.trim();
                if text.is_empty() {
                    continue;
                }
                let collapsed: String = text.split_whitespace().collect::<Vec<_>>().join(" ");
                let mut title: String = collapsed.chars().take(60).collect();
                if collapsed.chars().count() > 60 {
                    title.push('…');
                }
                return Some(title);
            }
        }
        None
    }
}
