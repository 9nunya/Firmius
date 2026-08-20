use chrono::{DateTime, Utc};
use indexmap::IndexMap;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex, OnceLock, RwLock};
use tokio::sync::Mutex as AsyncMutex;
use tokio::sync::broadcast;
use tokio::task::JoinHandle;
use uuid::Uuid;

use crate::AgentConfig;
use crate::agent::{Agent, AgentError, AgentEvent, PersonaUse};
use crate::artifact::SessionArtifacts;
use crate::persistence::{
    self, AgentNodeRecord, AgentRecord, SessionPersistenceCoordinator, SessionRecord,
    WorkStateRecord,
};
use crate::persona::PersonaManager;
use crate::providers::manager::ProviderManager;
use crate::tools::ToolRegistry;
use crate::work::{
    WorkError, WorkEvent, WorkEventEnvelope, WorkProjection, WorkSnapshot, WorkState,
};

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
    pub label: Option<String>,
    pub metadata: serde_json::Map<String, serde_json::Value>,
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
    pub session_id: String,
    pub sequence: u64,
    pub at: DateTime<Utc>,
    /// The only fold input. Do not add parallel `agent_id`/`event` fields
    /// here — any such field is fabricated for non-agent payloads (work
    /// mutations, notifications) and consumers must not rely on it.
    pub payload: SessionEventPayload,
}

#[derive(Debug, Clone)]
pub enum SessionEventPayload {
    Agent { agent_id: String, event: AgentEvent },
    Work(WorkEventEnvelope),
    Directory { path: String },
    Notification { agent_id: String, message: String },
    Workspace { name: String },
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/// Agents live behind `Arc` so they can be handed out as cheap, stable
/// handles (e.g. into `ToolContext` for a `delegate` tool to spawn more
/// agents while another agent's turn is still running) without invalidating
/// references on `Vec` reallocation.
pub type Agents = IndexMap<String, Arc<Agent>>;
/// The shared session ownership boundary.  Session state is protected by
/// short interior locks; callers must not wrap this handle in another mutex.
pub type SessionHandle = Arc<Session>;

/// A backgrounded `delegate` call (mode `spawn`), trackable via `poll`/`wait`.
pub struct DelegateHandle {
    pub agent_id: String,
    pub join: JoinHandle<Result<String, AgentError>>,
}

/// A backgrounded managed-graph run.
///
/// The run drives its graph to completion on its own, so the parent holds
/// only a handle: durable progress lives in the graph itself, which is why
/// `poll` reads state rather than buffering events here.
pub struct RunHandle {
    pub graph_id: crate::work::GraphId,
    pub cancellation: tokio_util::sync::CancellationToken,
    pub join: JoinHandle<crate::work::RunReport>,
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
    pub title: RwLock<Option<String>>,
    pub created_at: DateTime<Utc>,
    pub agents: RwLock<Agents>,
    pub hierarchy: RwLock<HashMap<String, AgentNode>>,
    /// Initialized by `new_handle`/`from_record_handle`.  A weak self-link
    /// avoids a Session -> Agent -> Session ownership cycle.
    self_handle: OnceLock<std::sync::Weak<Session>>,
    /// Backgrounded delegate calls (`delegate` tool, `mode: "spawn"`),
    /// keyed by a fresh `delegate_id` returned to the caller. Not persisted:
    /// a background task can't survive a process restart any more than a
    /// spawned OS process can (see `Host`) — on resume, in-flight delegates
    /// are simply gone; their agent's history up to that point still is.
    delegates: AsyncMutex<HashMap<String, DelegateHandle>>,
    /// Backgrounded managed-graph runs, keyed by run id.
    runs: AsyncMutex<HashMap<String, RunHandle>>,
    /// Ids already collected via `take_delegate`/`wait` — tombstones so a
    /// finished-and-gone delegate is distinguishable from one that never
    /// existed. Not persisted: like the delegates themselves, this is
    /// process-lifetime state.
    collected: AsyncMutex<HashSet<String>>,
    /// Broadcast bus carrying every agent's events (see `SessionEvent`).
    /// Agents are wired into it by the spawn methods; `prompt()` tees into it
    /// automatically.
    events_tx: broadcast::Sender<SessionEvent>,
    event_sequence: AtomicU64,
    /// Serializes sequence assignment with the corresponding broadcast send
    /// so that, even with parallel publishers (delegates running
    /// concurrently, work mutations racing agent events), the order events
    /// are delivered to subscribers always matches the order sequences were
    /// assigned in. Held only for the duration of one `fetch_add` + `send`.
    publish_lock: StdMutex<()>,
    pub work: RwLock<WorkState>,
    work_transaction: StdMutex<()>,
    unavailable_agents: RwLock<Vec<AgentRecord>>,
    persistence: SessionPersistenceCoordinator,
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
    /// Reconcile durable work after loading and expose any unsettled
    /// completion notifications to consumers without requiring polling.
    pub fn reconcile_work(&self) -> Result<(), String> {
        let candidate = {
            let mut state = self.work.write().unwrap();
            if state.reconcile_interrupted() {
                Some(state.clone())
            } else {
                None
            }
        };
        if let Some(candidate) = candidate {
            let record = self.snapshot_record_with_work(candidate.clone())?;
            self.persistence.save(&record)?;
            *self.work.write().unwrap() = candidate;
        }
        self.deliver_pending_notifications()?;
        Ok(())
    }

    /// M4.5/M4.8 — run one managed-graph scheduling pass. Snapshots and
    /// evaluates candidates outside any session lock, then durably claims
    /// each one through a revisioned `mutate_work` transaction, so
    /// competing schedulers (or a manual `task start` racing this pass)
    /// can never double-claim the same node. Callers must not call this
    /// before `reconcile_work()` has persisted on resume — see
    /// `crate::work::scheduler` for the full rationale.
    pub fn schedule_ready_work(
        &self,
        limits: &crate::work::SchedulerLimits,
    ) -> crate::work::ScheduleOutcome {
        crate::work::schedule_ready_work(self, limits)
    }

    /// Maximum number of already-delivered notifications retained per graph
    /// for history/inspection. Older delivered notifications are dropped so
    /// the vec never grows unbounded across a long-lived session.
    const DELIVERED_NOTIFICATION_CAP: usize = 50;

    /// Deliver every undelivered `WorkNotification` to its parent agent's
    /// mailbox (so the parent sees it without polling/waiting), mark it
    /// delivered, and bound the notifications vec. Safe to call repeatedly;
    /// notifications already marked delivered are skipped.
    pub fn deliver_pending_notifications(&self) -> Result<(), String> {
        let pending: Vec<(crate::work::GraphId, crate::work::ResultId, String, String)> = {
            let state = self.work.read().unwrap();
            state
                .graphs
                .iter()
                .flat_map(|(graph_id, graph)| {
                    graph
                        .notifications
                        .iter()
                        .filter(|n| !n.delivered)
                        .map(move |n| {
                            (
                                *graph_id,
                                n.id,
                                n.parent_agent_id.clone(),
                                n.message.clone(),
                            )
                        })
                })
                .collect()
        };
        if pending.is_empty() {
            return Ok(());
        }
        for (_, _, parent_agent_id, message) in &pending {
            if let Some(agent) = self.agent(parent_agent_id) {
                agent.submit(message.clone());
            }
        }
        let mut candidate = self.work.read().unwrap().clone();
        for (graph_id, result_id, _, _) in &pending {
            if let Some(graph) = candidate.graphs.get_mut(graph_id) {
                for note in graph.notifications.iter_mut() {
                    if note.id == *result_id {
                        note.delivered = true;
                    }
                }
                // Bound history: keep every undelivered notification, and at
                // most `DELIVERED_NOTIFICATION_CAP` of the most recent
                // delivered ones.
                let mut delivered_indices: Vec<usize> = graph
                    .notifications
                    .iter()
                    .enumerate()
                    .filter(|(_, n)| n.delivered)
                    .map(|(i, _)| i)
                    .collect();
                if delivered_indices.len() > Self::DELIVERED_NOTIFICATION_CAP {
                    delivered_indices.sort_by_key(|&i| graph.notifications[i].created_at);
                    let drop_count = delivered_indices.len() - Self::DELIVERED_NOTIFICATION_CAP;
                    let drop_ids: HashSet<crate::work::ResultId> = delivered_indices[..drop_count]
                        .iter()
                        .map(|&i| graph.notifications[i].id)
                        .collect();
                    graph.notifications.retain(|n| !drop_ids.contains(&n.id));
                }
            }
        }
        let record = self.snapshot_record_with_work(candidate.clone())?;
        self.persistence.save(&record)?;
        *self.work.write().unwrap() = candidate;
        Ok(())
    }

    pub fn set_agent_metadata(
        &self,
        agent_id: &str,
        label: Option<String>,
        metadata: serde_json::Map<String, serde_json::Value>,
    ) -> Result<(), String> {
        let agent = self
            .agent(agent_id)
            .ok_or_else(|| format!("agent not found: {agent_id}"))?;
        if let Some(label) = &label
            && let Some(existing) = self.agent_id_for_label(label)
            && existing != agent_id
        {
            return Err(format!(
                "label '{label}' is already in use by agent {existing}"
            ));
        }
        agent.set_label(label.clone()).map_err(|e| e.to_string())?;
        agent
            .set_metadata(metadata.clone())
            .map_err(|e| e.to_string())?;
        if let Some(node) = self.hierarchy.write().unwrap().get_mut(agent_id) {
            node.label = label;
            node.metadata = metadata;
        }
        self.save()
    }

    /// Resolve a unique human label to the agent id that holds it, if any.
    /// Labels are unique across the session, so this is unambiguous.
    pub fn agent_id_for_label(&self, label: &str) -> Option<String> {
        self.hierarchy
            .read()
            .unwrap()
            .iter()
            .find(|(_, node)| node.label.as_deref() == Some(label))
            .map(|(id, _)| id.clone())
    }

    /// Capture the authoritative work snapshot. Subscribe to the event bus
    /// before calling this when a client needs race-free recovery.
    pub fn snapshot(&self) -> WorkSnapshot {
        self.work_snapshot()
    }

    pub fn unavailable_agents(&self) -> Vec<AgentRecord> {
        self.unavailable_agents.read().unwrap().clone()
    }

    pub(crate) fn publish_agent_event(&self, agent_id: String, event: AgentEvent) {
        self.publish(|_, _| SessionEventPayload::Agent { agent_id, event });
    }

    /// Assign the next bus sequence and broadcast it atomically. The
    /// sequence/timestamp are handed to `build` so an envelope-carrying
    /// payload (e.g. `WorkEventEnvelope`) can embed the exact same values
    /// that end up on the outer `SessionEvent`. Held for the duration of
    /// exactly one `fetch_add` + `send`, so parallel publishers can never
    /// have their assigned sequence overtaken by another publisher's send.
    fn publish(&self, build: impl FnOnce(u64, DateTime<Utc>) -> SessionEventPayload) -> u64 {
        let _guard = self.publish_lock.lock().unwrap();
        let sequence = self.next_sequence();
        let at = Utc::now();
        let payload = build(sequence, at);
        let _ = self.events_tx.send(SessionEvent {
            session_id: self.id.clone(),
            sequence,
            at,
            payload,
        });
        sequence
    }

    fn next_sequence(&self) -> u64 {
        self.event_sequence.fetch_add(1, Ordering::AcqRel) + 1
    }

    pub fn work_snapshot(&self) -> WorkSnapshot {
        WorkSnapshot::new(
            self.id.clone(),
            self.event_sequence.load(Ordering::Acquire),
            self.work.read().unwrap().clone(),
        )
    }

    pub fn work_projection(&self, agent_id: &str) -> Option<WorkProjection> {
        let state = self.work.read().unwrap();
        let graph_id = state.active_graph_by_agent.get(agent_id).copied()?;
        let graph = state.graphs.get(&graph_id)?;
        Some(WorkProjection {
            graph_id,
            graph_revision: graph.revision,
            mini: crate::work::MiniProjection::from_graph(graph),
        })
    }

    /// Apply one short work transaction. The candidate is persisted before it
    /// replaces the in-memory state or becomes visible on the event bus.
    pub fn mutate_work<F, R>(&self, operation: F) -> Result<R, String>
    where
        F: FnOnce(&mut WorkState) -> Result<(R, WorkEvent), WorkError>,
    {
        let _transaction = self.work_transaction.lock().unwrap();
        let (result, event, candidate) = {
            let state = self.work.read().unwrap();
            let mut candidate = state.clone();
            let (result, event) = operation(&mut candidate).map_err(|e| e.to_string())?;
            candidate.validate().map_err(|e| e.to_string())?;
            (result, event, candidate)
        };
        let record = self.snapshot_record_with_work(candidate.clone())?;
        self.persistence.save(&record)?;
        *self.work.write().unwrap() = candidate;
        let mut envelope: Option<WorkEventEnvelope> = None;
        self.publish(|sequence, at| {
            let built = WorkEventEnvelope {
                session_id: self.id.clone(),
                sequence,
                at,
                event,
            };
            envelope = Some(built.clone());
            SessionEventPayload::Work(built)
        });
        let envelope = envelope.expect("publish always invokes build exactly once");
        if let WorkEvent::ResultRecorded { graph_id, result } = &envelope.event
            && let Some(graph) = self.work.read().unwrap().graphs.get(graph_id)
            && let Some(note) = graph
                .notifications
                .iter()
                .find(|n| n.result_id == result.id)
        {
            let agent_id = note.parent_agent_id.clone();
            let message = note.message.clone();
            self.publish(|_, _| SessionEventPayload::Notification { agent_id, message });
        }
        // Deliver into the parent's mailbox and mark delivered. Best-effort:
        // a delivery failure must not undo the already-committed mutation.
        if let Err(err) = self.deliver_pending_notifications() {
            eprintln!(
                "warning: session {}: failed to deliver work notification: {err}",
                self.id
            );
        }
        Ok(result)
    }
}

impl Session {
    pub fn new() -> Self {
        let (events_tx, _) = broadcast::channel(SESSION_EVENT_CAPACITY);
        Session {
            agents: RwLock::new(Agents::new()),
            hierarchy: RwLock::new(HashMap::new()),
            id: Uuid::new_v4().to_string(),
            title: RwLock::new(None),
            created_at: Utc::now(),
            self_handle: OnceLock::new(),
            delegates: AsyncMutex::new(HashMap::new()),
            runs: AsyncMutex::new(HashMap::new()),
            collected: AsyncMutex::new(HashSet::new()),
            events_tx,
            event_sequence: AtomicU64::new(0),
            publish_lock: StdMutex::new(()),
            work: RwLock::new(WorkState::default()),
            work_transaction: StdMutex::new(()),
            unavailable_agents: RwLock::new(Vec::new()),
            persistence: SessionPersistenceCoordinator::current(),
            artifacts: Arc::new(SessionArtifacts::new()),
        }
    }

    /// Create a session with its canonical shared handle wired before any
    /// agents are spawned. This replaces the old `bind_self` footgun.
    pub fn new_handle() -> SessionHandle {
        let session = Arc::new(Self::new());
        session
            .self_handle
            .set(Arc::downgrade(&session))
            .expect("new session self handle is unset");
        session
    }

    /// Wrap a reconstructed session in the canonical handle and attach the
    /// weak self link before it is exposed to agents/tools.
    pub fn into_handle(self) -> SessionHandle {
        let session = Arc::new(self);
        session
            .self_handle
            .set(Arc::downgrade(&session))
            .expect("reconstructed session self handle is unset");
        let agents = session
            .agents
            .read()
            .unwrap()
            .values()
            .cloned()
            .collect::<Vec<_>>();
        for agent in agents {
            session.attach_self_handle(&agent);
        }
        session
    }

    fn attach_self_handle(&self, agent: &Agent) {
        agent.attach_bus(self.events_tx.clone());
        if let Some(weak) = self.self_handle.get()
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
        &self,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
    ) -> Arc<Agent> {
        let agent = Arc::new(Agent::new(provider, tools, config, self.id.clone()));
        self.agents
            .write()
            .unwrap()
            .insert(agent.id.clone(), agent.clone());
        self.hierarchy.write().unwrap().insert(
            agent.id.clone(),
            AgentNode {
                parent_id: None,
                spawned_via_tool_call_id: None,
                label: None,
                metadata: serde_json::Map::new(),
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    pub fn spawn_agent_with_personas(
        &self,
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
        self.agents
            .write()
            .unwrap()
            .insert(agent.id.clone(), agent.clone());
        self.hierarchy.write().unwrap().insert(
            agent.id.clone(),
            AgentNode {
                parent_id: None,
                spawned_via_tool_call_id: None,
                label: None,
                metadata: serde_json::Map::new(),
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    /// Create a subagent under `parent_id`, recording the tool call (if any)
    /// that spawned it so the tree is fully traceable.
    pub fn spawn_subagent(
        &self,
        parent_id: &str,
        spawned_via_tool_call_id: Option<String>,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
    ) -> Arc<Agent> {
        let agent = Arc::new(Agent::new(provider, tools, config, self.id.clone()));
        self.agents
            .write()
            .unwrap()
            .insert(agent.id.clone(), agent.clone());
        self.hierarchy.write().unwrap().insert(
            agent.id.clone(),
            AgentNode {
                parent_id: Some(parent_id.to_string()),
                spawned_via_tool_call_id,
                label: None,
                metadata: serde_json::Map::new(),
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    pub fn spawn_subagent_with_personas(
        &self,
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
        self.agents
            .write()
            .unwrap()
            .insert(agent.id.clone(), agent.clone());
        self.hierarchy.write().unwrap().insert(
            agent.id.clone(),
            AgentNode {
                parent_id: Some(parent_id.to_string()),
                spawned_via_tool_call_id,
                label: None,
                metadata: serde_json::Map::new(),
            },
        );
        self.attach_self_handle(&agent);
        agent
    }

    /// Look up a live agent handle by id.
    pub fn agent(&self, id: &str) -> Option<Arc<Agent>> {
        self.agents.read().unwrap().get(id).cloned()
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

    // ------------------------------------------------------------------
    // Managed runs
    // ------------------------------------------------------------------

    /// Track a backgrounded run so the caller can poll or await it later.
    pub async fn register_run(&self, run_id: String, handle: RunHandle) {
        self.runs.lock().await.insert(run_id, handle);
    }

    /// Non-blocking status for a run: whether it has finished, plus the
    /// graph it drives. Progress detail is read from the graph itself
    /// rather than buffered here, so a poll always reflects durable state.
    pub async fn poll_run(&self, run_id: &str) -> Option<(crate::work::GraphId, bool)> {
        let runs = self.runs.lock().await;
        let handle = runs.get(run_id)?;
        Some((handle.graph_id, handle.join.is_finished()))
    }

    /// Block until a run concludes, removing its handle and returning the
    /// report. Errors if `run_id` is unknown or was already collected.
    pub async fn wait_run(&self, run_id: &str) -> Result<crate::work::RunReport, String> {
        let handle = self
            .runs
            .lock()
            .await
            .remove(run_id)
            .ok_or_else(|| format!("unknown run_id: {run_id}"))?;
        handle
            .join
            .await
            .map_err(|e| format!("run task panicked: {e}"))
    }

    /// Ask a run to stop. In-flight node agents observe the same token, so
    /// cancellation reaches the work rather than only the driver loop.
    pub async fn cancel_run(&self, run_id: &str) -> Result<(), String> {
        let runs = self.runs.lock().await;
        let handle = runs
            .get(run_id)
            .ok_or_else(|| format!("unknown run_id: {run_id}"))?;
        handle.cancellation.cancel();
        Ok(())
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
    ) -> Result<Self, String> {
        Self::from_record_with_personas(record, mgr, tools, Arc::new(PersonaManager::empty()))
    }

    pub fn from_record_with_personas(
        record: SessionRecord,
        mgr: &ProviderManager,
        tools: Arc<ToolRegistry>,
        personas: Arc<PersonaManager>,
    ) -> Result<Self, String> {
        let work_state = record.work.clone().into_state().map_err(|e| {
            format!(
                "session {}: persisted work state is invalid, refusing to load ({e})",
                record.id
            )
        })?;
        let session = Session {
            id: record.id,
            title: RwLock::new(record.title),
            created_at: record.created_at,
            agents: RwLock::new(Agents::new()),
            hierarchy: RwLock::new(HashMap::new()),
            self_handle: OnceLock::new(),
            delegates: AsyncMutex::new(HashMap::new()),
            runs: AsyncMutex::new(HashMap::new()),
            collected: AsyncMutex::new(HashSet::new()),
            events_tx: {
                let (tx, _) = broadcast::channel(SESSION_EVENT_CAPACITY);
                tx
            },
            event_sequence: AtomicU64::new(0),
            publish_lock: StdMutex::new(()),
            work: RwLock::new(work_state),
            work_transaction: StdMutex::new(()),
            unavailable_agents: RwLock::new(record.unavailable_agents.clone()),
            persistence: SessionPersistenceCoordinator::current(),
            artifacts: Arc::new(SessionArtifacts::from_records(record.artifacts)),
        };

        let mut unavailable = record.unavailable_agents;
        for ar in record.agents {
            let original_ar = ar.clone();
            let provider = match mgr.build(&ar.provider_id) {
                Ok(p) => p,
                Err(e) => {
                    eprintln!(
                        "warning: session {}: agent {} used provider '{}' which is unavailable ({e}); skipping",
                        session.id, ar.id, ar.provider_id
                    );
                    unavailable.push(original_ar);
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
                    label: n.label.clone(),
                    metadata: n.metadata.clone(),
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
            .with_compaction(ar.compaction.clone().unwrap_or_else(|| {
                crate::compaction::Projection::new(crate::compaction::Timeline::default())
            }));
            if node.parent_id.is_some()
                && let Err(error) = agent.set_persona_context(PersonaUse::Delegate)
            {
                eprintln!(
                    "warning: session {}: agent {} has an invalid delegated persona ({error}); skipping",
                    session.id, ar.id
                );
                unavailable.push(original_ar);
                continue;
            }
            // Preserve the original agent id so the hierarchy map (keyed on
            // it) still lines up, and so any external references (e.g. a
            // saved delegate_id) remain valid across resume.
            let agent = agent.with_id(ar.id.clone());
            let _ = agent.set_label(ar.label.clone());
            let _ = agent.set_metadata(ar.metadata.clone());
            let agent = Arc::new(agent);
            session.agents.write().unwrap().insert(ar.id.clone(), agent);

            session.hierarchy.write().unwrap().insert(ar.id, node);
        }

        *session.unavailable_agents.write().unwrap() = unavailable;

        // Reconcile any work left mid-attempt by an unclean shutdown, and
        // persist the reconciliation, before the session (and any agent in
        // it) is handed back to a caller that might immediately prompt an
        // agent or expose the `task`/`delegate` tools. `reconcile_work`
        // releases open assignments, records an `Outcome::Interrupted`
        // result envelope per interrupted attempt, and notifies the parent.
        session.reconcile_work()?;

        Ok(session)
    }

    /// Load + rebuild a session in one call.
    pub fn resume(
        id: &str,
        mgr: &ProviderManager,
        tools: Arc<ToolRegistry>,
    ) -> Result<Self, String> {
        let record = persistence::load_session_record(id)?;
        Self::from_record(record, mgr, tools)
    }

    pub fn resume_with_personas(
        id: &str,
        mgr: &ProviderManager,
        tools: Arc<ToolRegistry>,
        personas: Arc<PersonaManager>,
    ) -> Result<Self, String> {
        let record = persistence::load_session_record(id)?;
        Self::from_record_with_personas(record, mgr, tools, personas)
    }

    /// Current display title, if one has been set or derived.
    pub fn title(&self) -> Option<String> {
        self.title.read().unwrap().clone()
    }

    /// Set (or clear) the session title. An empty string is treated as
    /// "unset", so the next save will re-derive from the first user message.
    pub fn set_title(&self, title: Option<String>) {
        let next = title.and_then(|t| {
            let t = t.trim();
            if t.is_empty() {
                None
            } else {
                Some(t.to_string())
            }
        });
        *self.title.write().unwrap() = next;
    }

    /// Snapshot every agent's config + history + the hierarchy into a
    /// record, deriving a title from the first user message if one hasn't
    /// been set yet, and persist it to `~/.firmius/sessions/<id>.json`.
    pub fn save(&self) -> Result<(), String> {
        if self.title.read().unwrap().is_none() {
            *self.title.write().unwrap() = self.derive_title();
        }

        // Acquire the work-transaction lock so the snapshot and the
        // coordinator's write generation are atomic w.r.t. mutate_work.
        // Without this, save() can read stale work state, get preempted
        // by a mutate_work commit, then write the stale snapshot at a
        // higher generation — clobbering the committed mutation.
        let _guard = self.work_transaction.lock().unwrap();
        let work = self.work.read().unwrap().clone();
        self.snapshot_record_with_work(work)
            .and_then(|record| self.persistence.save(&record))
    }

    /// Build a SessionRecord from live state without writing it. Used by
    /// `/export` so a user can dump the in-memory conversation even if the
    /// last save is a few events behind.
    pub fn snapshot_record(&self) -> Result<SessionRecord, String> {
        let work = self.work.read().unwrap().clone();
        self.snapshot_record_with_work(work)
    }

    fn snapshot_record_with_work(&self, work: WorkState) -> Result<SessionRecord, String> {
        let agents = self
            .agents
            .read()
            .unwrap()
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
                    label: agent.label(),
                    metadata: agent.metadata(),
                    history,
                    compaction: Some(compaction),
                }
            })
            .collect::<Vec<_>>();
        let hierarchy = self
            .hierarchy
            .read()
            .unwrap()
            .iter()
            .map(|(id, node)| {
                (
                    id.clone(),
                    AgentNodeRecord {
                        parent_id: node.parent_id.clone(),
                        spawned_via_tool_call_id: node.spawned_via_tool_call_id.clone(),
                        label: node.label.clone(),
                        metadata: node.metadata.clone(),
                    },
                )
            })
            .collect::<HashMap<String, AgentNodeRecord>>();
        Ok(SessionRecord {
            id: self.id.clone(),
            title: self.title.read().unwrap().clone(),
            created_at: self.created_at,
            updated_at: Utc::now(),
            agents,
            hierarchy,
            artifacts: self.artifacts.snapshot(),
            work: WorkStateRecord::from_state(work),
            unavailable_agents: self.unavailable_agents.read().unwrap().clone(),
        })
    }

    /// First user message across all agents (by insertion order), truncated
    /// to a short title. `None` if no agent has been prompted yet.
    fn derive_title(&self) -> Option<String> {
        for agent in self.agents.read().unwrap().values() {
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
