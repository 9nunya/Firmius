use chrono::{DateTime, Utc};
use indexmap::IndexMap;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex, OnceLock, RwLock, Weak};
use tokio::sync::Mutex as AsyncMutex;
use tokio::sync::broadcast;
use tokio::task::JoinHandle;
use uuid::Uuid;

use crate::AgentConfig;
use crate::agent::{Agent, AgentError, AgentEvent, PersonaUse};
use crate::artifact::SessionArtifacts;
use crate::permissions::PermissionBroker;
use crate::persistence::{
    self, AgentNodeRecord, AgentRecord, MailboxDeliveryRecord, MailboxDeliveryState,
    SessionMailboxState, SessionPersistenceCoordinator, SessionRecord, WorkStateRecord,
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
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionEvent {
    pub session_id: String,
    pub sequence: u64,
    pub at: DateTime<Utc>,
    /// The only fold input. Do not add parallel `agent_id`/`event` fields
    /// here — any such field is fabricated for non-agent payloads (work
    /// mutations, notifications) and consumers must not rely on it.
    pub payload: SessionEventPayload,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum SessionEventPayload {
    Agent {
        agent_id: String,
        event: AgentEvent,
    },
    Work(WorkEventEnvelope),
    /// A committed native todo mutation.  The payload is deliberately an
    /// invalidation rather than a copy of the ledger: the revision identifies
    /// the durable state, while the bounded projection travels in the same
    /// status/snapshot that every other client reads.  Consumers must not
    /// reconstruct todo state from this event alone.
    Todo {
        agent_id: String,
        revision: u64,
    },
    Directory {
        path: String,
    },
    Notification {
        agent_id: String,
        message: String,
    },
    /// Durable assignment result notification. Kept distinct from the legacy
    /// free-form notification payload so UIs never have to parse worker text.
    AssignmentCompletion {
        agent_id: String,
        child_agent_id: String,
        assignment_id: String,
        message: String,
    },
    Workspace {
        name: String,
    },
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
#[derive(Debug, Clone, Serialize, Deserialize)]
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
    /// Latest resource state survives event-journal eviction for this process.
    tool_runtime: StdMutex<HashMap<(String, String, String), SessionEvent>>,
    pub work: RwLock<WorkState>,
    work_transaction: StdMutex<()>,
    /// Serializes protective swarm claim changes with built-in edit commits.
    /// The ordinary work transaction cannot cover an edit because edit
    /// preflight and authority acquisition are asynchronous.
    swarm_edit_transaction: AsyncMutex<()>,
    unavailable_agents: RwLock<Vec<AgentRecord>>,
    mailbox: RwLock<SessionMailboxState>,
    persistence: SessionPersistenceCoordinator,
    /// Session-wide artifact store, shared by every agent and persisted with
    /// the session record. Addressable as `artifact://<path>`.
    pub artifacts: Arc<SessionArtifacts>,
    pub permission_broker: Arc<PermissionBroker>,
    edit_authority: RwLock<Arc<dyn crate::EditAuthority>>,
}

/// Outcome of a context-aware durable send. `Duplicate` means the same
/// `message_id` was already recorded; the original record is returned.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SendOutcome {
    Delivered,
    Deferred,
    Duplicate,
    QueuedUnavailable,
    AuditOnly,
}

/// Optional addressing context for a durable send. Sender identity is taken
/// from the authenticated session/tool caller, never from this envelope.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SendContext {
    pub message_id: Option<String>,
    pub thread_id: Option<String>,
    pub goal_id: Option<String>,
    pub run_id: Option<String>,
    pub parent_goal_id: Option<String>,
    pub workflow_node_id: Option<String>,
    pub assignment_id: Option<String>,
    pub in_reply_to: Option<String>,
}

impl Default for Session {
    fn default() -> Self {
        Self::new()
    }
}

impl Session {
    pub fn edit_authority(&self) -> Arc<dyn crate::EditAuthority> {
        self.edit_authority.read().unwrap().clone()
    }

    /// Attach one authority to this session. Existing agents are updated and
    /// `attach_self_handle` propagates it to every future/resumed subagent.
    pub fn attach_edit_authority(&self, authority: Arc<dyn crate::EditAuthority>) {
        *self.edit_authority.write().unwrap() = authority.clone();
        for agent in self.agents.read().unwrap().values() {
            agent.attach_edit_authority(authority.clone());
        }
    }

    /// Reconcile durable work after loading and expose any unsettled
    /// completion notifications to consumers without requiring polling.
    pub fn reconcile_work(&self) -> Result<(), String> {
        {
            // Hold the work transaction across reconciliation *and* mailbox
            // notification delivery: `deliver_pending_notifications` writes
            // its own candidate back into `self.work`, so it must not race a
            // concurrent commit. It does not take the work transaction
            // itself, so this cannot deadlock.
            let _transaction = self.work_transaction.lock().unwrap();
            let candidate = {
                let state = self.work.read().unwrap();
                let mut candidate = state.clone();
                candidate.reconcile_interrupted().then_some(candidate)
            };
            if let Some(candidate) = candidate {
                let record = self.snapshot_record_with_work(candidate.clone())?;
                self.persistence.save(&record)?;
                *self.work.write().unwrap() = candidate;
            }
            self.deliver_pending_notifications()?;
        }
        // Replay swarm outbox entries left pending by a crash between
        // persistence and delivery, plus any created by reconciliation. The
        // dispatcher persists through `send_message_with_context`, which
        // takes the work transaction, so it runs after the guard is dropped.
        self.drain_pending_swarm_outbox()?;
        Ok(())
    }

    /// Hold while either changing protective swarm ownership or committing a
    /// built-in edit. This closes the gap between claim validation and the
    /// filesystem write without holding a synchronous lock across `.await`.
    pub async fn lock_swarm_edits(&self) -> tokio::sync::MutexGuard<'_, ()> {
        self.swarm_edit_transaction.lock().await
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
        let pending: Vec<(
            crate::work::GraphId,
            crate::work::ResultId,
            String,
            String,
            String,
            String,
        )> = {
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
                                n.child_agent_id.clone(),
                                n.assignment_id.to_string(),
                            )
                        })
                })
                .collect()
        };
        if pending.is_empty() {
            return Ok(());
        }
        let mut wake = Vec::new();
        for (_, _, parent_agent_id, message, child_agent_id, assignment_id) in &pending {
            let message =
                crate::types::Message::text(crate::types::MessageRole::User, message.clone())
                    .with_correlation(crate::types::MessageCorrelation {
                        sender_id: Some(child_agent_id.clone()),
                        assignment_id: Some(assignment_id.clone()),
                        ..Default::default()
                    })
                    .with_provenance(crate::types::MessageProvenance::new(
                        crate::types::MessageOrigin::Assignment,
                        crate::types::MessageTrust::DerivedUntrusted,
                    ));
            if let Some(agent) = self.agent(parent_agent_id) {
                agent.submit_message(message);
                wake.push(agent);
            } else {
                self.submit_to_unavailable_agent(parent_agent_id, message);
            }
        }
        let mut candidate = self.work.read().unwrap().clone();
        for (graph_id, result_id, _, _, _, _) in &pending {
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
        for agent in wake {
            self.wake_agent(agent);
        }
        Ok(())
    }

    /// Maximum acknowledged swarm-outbox entries retained for audit. Pending
    /// entries are never compacted.
    const SWARM_OUTBOX_ACK_RETENTION: usize = 256;

    /// Deliver every pending swarm outbox entry to its canonical recipients
    /// through the existing durable correlated mailbox, then acknowledge the
    /// fully-delivered ones in a *separate* persisted work transaction.
    ///
    /// Ordering guarantees:
    ///   - The canonical request/milestone/transfer and its pending outbox
    ///     entry are committed together by the originating `mutate_work`, so
    ///     delivery always happens after persistence.
    ///   - `message_id` is derived from the stable outbox identity plus the
    ///     recipient, so a repeated dispatch, a crash between delivery and
    ///     acknowledgement, or two concurrent drains all collapse to one
    ///     durable mailbox record.
    ///   - Acknowledgement means "the durable mailbox accepted the message",
    ///     not that the model read or acted on it. A delivery failure leaves
    ///     the entry pending and retryable.
    ///   - No work, mailbox, or persistence lock is held across delivery;
    ///     `send_message_with_context` takes its own short locks.
    ///
    /// Safe to call repeatedly and from concurrent callers.
    pub fn drain_pending_swarm_outbox(&self) -> Result<usize, String> {
        let pending: Vec<crate::work::SwarmOutboxEntry> = {
            let state = self.work.read().unwrap();
            state
                .swarm
                .outbox
                .values()
                .filter(|entry| entry.state == crate::work::SwarmOutboxState::Pending)
                .cloned()
                .collect()
        };
        if pending.is_empty() {
            return Ok(0);
        }
        let mut acknowledged = Vec::new();
        for entry in &pending {
            let deliveries = {
                let state = self.work.read().unwrap();
                crate::work::resolve_swarm_outbox_entry(&state, entry)
            };
            let mut all_delivered = true;
            for delivery in deliveries {
                let context = crate::SendContext {
                    message_id: Some(delivery.message_id.clone()),
                    thread_id: Some(delivery.thread_id.clone()),
                    assignment_id: delivery.assignment_id.clone(),
                    ..Default::default()
                };
                let message = crate::types::Message::text(
                    crate::types::MessageRole::User,
                    delivery.body.clone(),
                )
                .with_correlation(crate::types::MessageCorrelation {
                    sender_id: Some(delivery.sender.clone()),
                    assignment_id: delivery.assignment_id.clone(),
                    ..Default::default()
                })
                .with_provenance(crate::types::MessageProvenance::new(
                    crate::types::MessageOrigin::Assignment,
                    crate::types::MessageTrust::DerivedUntrusted,
                ));
                if let Err(error) = self.send_message_with_context(
                    &delivery.sender,
                    &delivery.recipient,
                    message,
                    context,
                ) {
                    all_delivered = false;
                    eprintln!(
                        "warning: session {}: swarm outbox delivery to {} failed: {error}",
                        self.id, delivery.recipient
                    );
                }
            }
            if all_delivered {
                acknowledged.push(entry.id);
            }
        }
        if acknowledged.is_empty() {
            return Ok(0);
        }
        // Separate persisted transaction: acknowledgement must not share the
        // delivery path, so a mailbox failure leaves the entry pending.
        let _transaction = self.work_transaction.lock().unwrap();
        let mut candidate = self.work.read().unwrap().clone();
        candidate
            .acknowledge_swarm_outbox_batch(
                candidate.revision,
                &acknowledged,
                Self::SWARM_OUTBOX_ACK_RETENTION,
            )
            .map_err(|error| error.to_string())?;
        let record = self.snapshot_record_with_work(candidate.clone())?;
        self.persistence.save(&record)?;
        *self.work.write().unwrap() = candidate;
        drop(_transaction);
        Ok(acknowledged.len())
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

    /// Queue input for an agent whose provider is currently unavailable.
    /// Its persisted record remains resumable and will restore this mailbox
    /// once that provider is configured again.
    pub fn submit_to_unavailable_agent(
        &self,
        agent_id: &str,
        message: crate::types::Message,
    ) -> bool {
        let mut agents = self.unavailable_agents.write().unwrap();
        let Some(agent) = agents.iter_mut().find(|agent| agent.id == agent_id) else {
            return false;
        };
        agent.mailbox.push(message);
        true
    }

    pub(crate) fn publish_agent_event(&self, agent_id: String, event: AgentEvent) {
        self.publish(|_, _| SessionEventPayload::Agent { agent_id, event });
    }

    /// Schedule an immediate mailbox turn. Multiple concurrent deliveries are
    /// safe: each waiter takes the agent's turn lock, and only the first one
    /// that still finds queued input dispatches a provider request.
    pub fn wake_agent(&self, agent: Arc<Agent>) {
        let Some(session) = self.self_handle.get().and_then(std::sync::Weak::upgrade) else {
            return;
        };
        let Ok(runtime) = tokio::runtime::Handle::try_current() else {
            return;
        };
        runtime.spawn(async move {
            let result = agent
                .wake_mailbox(tokio_util::sync::CancellationToken::new(), |_| {})
                .await;
            if matches!(result, Ok(Some(_)) | Err(_)) {
                if let Err(error) = session.save() {
                    eprintln!(
                        "warning: session {}: failed to save mailbox-triggered turn: {error}",
                        session.id
                    );
                }
            }
            if let Err(error) = result {
                eprintln!(
                    "warning: session {}: mailbox-triggered turn failed for agent {}: {error}",
                    session.id, agent.id
                );
            }
        });
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
        let envelope = SessionEvent {
            session_id: self.id.clone(),
            sequence,
            at,
            payload,
        };
        if let SessionEventPayload::Agent {
            agent_id,
            event: AgentEvent::ToolRuntime { id, resource },
        } = &envelope.payload
        {
            let resource_id = match resource {
                crate::ToolRuntimeResource::Process { id, .. } => format!("process:{id}"),
                crate::ToolRuntimeResource::Delegate { id, .. } => format!("delegate:{id}"),
            };
            self.tool_runtime.lock().unwrap().insert(
                (agent_id.clone(), id.clone(), resource_id),
                envelope.clone(),
            );
        }
        let _ = self.events_tx.send(envelope);
        sequence
    }

    pub fn tool_runtime_events(&self) -> Vec<SessionEvent> {
        let mut events: Vec<_> = self
            .tool_runtime
            .lock()
            .unwrap()
            .values()
            .cloned()
            .collect();
        events.sort_by_key(|e| e.sequence);
        events
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

    /// Latest sequence assigned on the unified session event bus. Consumers
    /// use this as a reconnect watermark even when no work event was emitted.
    pub fn event_sequence(&self) -> u64 {
        self.event_sequence.load(Ordering::Acquire)
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

    /// Publish a work event that carries no state change.
    ///
    /// Run lifecycle is observable, not durable: the graph itself already
    /// records every node transition, so a run announcement needs the bus
    /// but must not force a persist or a revision bump it did not earn.
    pub fn publish_work_event(&self, event: WorkEvent) {
        self.publish(|sequence, at| {
            SessionEventPayload::Work(WorkEventEnvelope {
                session_id: self.id.clone(),
                sequence,
                at,
                event,
            })
        });
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
            let child_agent_id = note.child_agent_id.clone();
            let assignment_id = note.assignment_id.to_string();
            self.publish(|_, _| SessionEventPayload::AssignmentCompletion {
                agent_id,
                child_agent_id,
                assignment_id,
                message,
            });
        }
        // Deliver into the parent's mailbox and mark delivered. Best-effort:
        // a delivery failure must not undo the already-committed mutation.
        if let Err(err) = self.deliver_pending_notifications() {
            eprintln!(
                "warning: session {}: failed to deliver work notification: {err}",
                self.id
            );
        }
        // Release the work transaction before outbox dispatch: mailbox
        // delivery persists the session and takes the same lock, so the
        // dispatcher must never run while it is held.
        drop(_transaction);
        if let Err(err) = self.drain_pending_swarm_outbox() {
            eprintln!(
                "warning: session {}: failed to drain swarm outbox: {err}",
                self.id
            );
        }
        Ok(result)
    }

    /// Return the canonical todo ledger for one live agent. The returned
    /// value is a snapshot; all mutations must go through
    /// [`Self::mutate_agent_todo`].
    pub fn agent_todo(&self, agent_id: &str) -> Result<crate::todo::TodoLedger, String> {
        let agent = self
            .agent(agent_id)
            .ok_or_else(|| format!("agent not found: {agent_id}"))?;
        let state = agent.state_handle();
        let state = state.read().unwrap();
        if let Some(quarantine) = &state.todo_quarantine {
            let reason = match quarantine {
                crate::todo::PersistedTodoState::Quarantined { reason, .. } => reason.as_str(),
                _ => "invalid persisted todo state",
            };
            return Err(format!("todo state is quarantined: {reason}"));
        }
        if state.todo.agent_id() != agent_id {
            return Err(format!(
                "todo owner mismatch: expected {agent_id}, actual {}",
                state.todo.agent_id()
            ));
        }
        Ok(state.todo.clone())
    }

    /// Atomically mutate and persist one agent's canonical todo ledger.
    ///
    /// The operation runs on a cloned candidate. The exact full session
    /// record containing that candidate is persisted before the candidate is
    /// installed in live AgentState, so provider projections and completion
    /// evaluation can never observe an uncommitted mutation. The shared
    /// session transaction also serializes this operation against every
    /// full-session save and WorkGraph commit.
    pub fn mutate_agent_todo<R, F>(&self, agent_id: &str, operation: F) -> Result<R, String>
    where
        F: FnOnce(&mut crate::todo::TodoLedger) -> Result<R, crate::todo::TodoError>,
    {
        let _transaction = self.work_transaction.lock().unwrap();
        let agent = self
            .agent(agent_id)
            .ok_or_else(|| format!("agent not found: {agent_id}"))?;
        let state_handle = agent.state_handle();
        let (result, candidate) = {
            let state = state_handle.read().unwrap();
            if let Some(quarantine) = &state.todo_quarantine {
                let reason = match quarantine {
                    crate::todo::PersistedTodoState::Quarantined { reason, .. } => reason.as_str(),
                    _ => "invalid persisted todo state",
                };
                return Err(format!("todo state is quarantined: {reason}"));
            }
            if state.todo.agent_id() != agent_id {
                return Err(format!(
                    "todo owner mismatch: expected {agent_id}, actual {}",
                    state.todo.agent_id()
                ));
            }
            let mut candidate = state.todo.clone();
            let result = operation(&mut candidate).map_err(|error| error.to_string())?;
            candidate.validate().map_err(|error| error.to_string())?;
            if candidate.agent_id() != agent_id {
                return Err(format!(
                    "todo mutation changed owner: expected {agent_id}, actual {}",
                    candidate.agent_id()
                ));
            }
            (result, candidate)
        };

        let work = self.work.read().unwrap().clone();
        let record = self.snapshot_record_with_todo_candidate(work, agent_id, &candidate)?;
        self.persistence.save(&record)?;

        // No todo mutation can race this install because every supported
        // writer enters through this transaction. Fail closed if an internal
        // direct writer violated that contract while persistence was in
        // progress rather than overwriting its newer state.
        let mut state = state_handle.write().unwrap();
        let persisted_revision = record
            .agents
            .iter()
            .find(|record| record.id == agent_id)
            .and_then(|record| match &record.todo {
                crate::todo::PersistedTodoState::Valid(envelope) => {
                    Some(envelope.ledger.revision())
                }
                _ => None,
            })
            .ok_or_else(|| "persisted todo candidate is missing".to_string())?;
        if candidate.revision() != persisted_revision {
            return Err("persisted todo candidate revision changed unexpectedly".to_string());
        }
        let revision = candidate.revision();
        state.todo = candidate;
        state.todo_quarantine = None;
        // Release the state guard before publishing so a subscriber that
        // reads the ledger cannot deadlock against the install.
        drop(state);
        // Published only after the exact candidate record is durable and the
        // live state has been installed, so no client can observe an
        // uncommitted mutation.
        self.publish(|_, _| SessionEventPayload::Todo {
            agent_id: agent_id.to_string(),
            revision,
        });
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
            tool_runtime: StdMutex::new(HashMap::new()),
            work: RwLock::new(WorkState::default()),
            work_transaction: StdMutex::new(()),
            swarm_edit_transaction: AsyncMutex::new(()),
            unavailable_agents: RwLock::new(Vec::new()),
            mailbox: RwLock::new(SessionMailboxState::default()),
            persistence: SessionPersistenceCoordinator::current(),
            artifacts: Arc::new(SessionArtifacts::new()),
            permission_broker: Arc::new(PermissionBroker::new(
                crate::permissions::PermissionPolicy::default(),
            )),
            edit_authority: RwLock::new(Arc::new(crate::InProcessEditAuthority::new())),
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

    /// Return the canonical shared handle for a live session. Agents use this
    /// when a direct UI action needs the same session-scoped tool context as a
    /// normal model turn.
    pub fn handle(&self) -> Option<SessionHandle> {
        self.self_handle.get().and_then(Weak::upgrade)
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
            if !agent.pending_messages().is_empty() {
                session.wake_agent(agent);
            }
        }
        session
    }

    fn attach_self_handle(&self, agent: &Agent) {
        agent.attach_bus(self.events_tx.clone());
        agent.attach_edit_authority(self.edit_authority.read().unwrap().clone());
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

    pub fn spawn_agent_with_personas_and_host(
        &self,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
        personas: Arc<PersonaManager>,
        host: Arc<dyn crate::Host>,
    ) -> Arc<Agent> {
        let agent = Arc::new(
            Agent::new_with_personas(provider, tools, config, self.id.clone(), personas)
                .with_host(host),
        );
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
        let host = self.agent(parent_id).map(|parent| parent.host());
        let agent = Agent::new(provider, tools, config, self.id.clone());
        let agent = if let Some(host) = host {
            agent.with_host(host)
        } else {
            agent
        };
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

    pub fn spawn_subagent_with_personas(
        &self,
        parent_id: &str,
        spawned_via_tool_call_id: Option<String>,
        provider: Arc<dyn crate::Provider>,
        tools: Arc<ToolRegistry>,
        config: AgentConfig,
        personas: Arc<PersonaManager>,
    ) -> Arc<Agent> {
        let host = self.agent(parent_id).map(|parent| parent.host());
        let agent = Agent::new_with_personas(provider, tools, config, self.id.clone(), personas);
        let agent = if let Some(host) = host {
            agent.with_host(host)
        } else {
            agent
        };
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
    pub async fn register_run(&self, run_id: String, handle: RunHandle) -> Result<(), String> {
        let mut runs = self.runs.lock().await;
        if runs.contains_key(&run_id) {
            return Err(format!("run_id already registered: {run_id}"));
        }
        if runs
            .values()
            .any(|existing| existing.graph_id == handle.graph_id && !existing.join.is_finished())
        {
            return Err(format!(
                "graph {} already has a live managed run",
                handle.graph_id
            ));
        }
        runs.insert(run_id, handle);
        Ok(())
    }

    /// Non-blocking status for a run: whether it has finished, plus the
    /// graph it drives. Progress detail is read from the graph itself
    /// rather than buffered here, so a poll always reflects durable state.
    pub async fn poll_run(&self, run_id: &str) -> Option<(crate::work::GraphId, bool)> {
        let runs = self.runs.lock().await;
        let handle = runs.get(run_id)?;
        Some((handle.graph_id, handle.join.is_finished()))
    }

    /// True when an unfinished managed run already drives `graph_id`.
    pub async fn has_live_run_for_graph(&self, graph_id: crate::work::GraphId) -> bool {
        self.runs
            .lock()
            .await
            .values()
            .any(|handle| handle.graph_id == graph_id && !handle.join.is_finished())
    }

    /// Stop a process-local managed driver at a durable boundary. Dropping a
    /// future is never itself considered a checkpoint: after the join exits,
    /// reconciliation records every abandoned assignment as interrupted and
    /// only then is the run published as parked.
    pub async fn park_run(
        &self,
        run_id: &str,
        requesting_agent_id: &str,
    ) -> Result<crate::work::GraphId, String> {
        let record = self
            .work
            .read()
            .unwrap()
            .managed_runs
            .get(run_id)
            .cloned()
            .ok_or_else(|| format!("unknown run_id: {run_id}"))?;
        if record.owner_agent_id != requesting_agent_id {
            return Err(format!(
                "agent '{requesting_agent_id}' is not authorized to park run {run_id}"
            ));
        }
        if record.status != crate::work::ManagedRunStatus::Running {
            return Err(format!("run {run_id} is not running"));
        }
        let parking_id = run_id.to_string();
        self.mutate_work(move |state| {
            let graph_id = {
                let run = state.managed_runs.get_mut(&parking_id).ok_or_else(|| {
                    crate::work::WorkError::InvalidGraph("managed run disappeared".into())
                })?;
                if run.status != crate::work::ManagedRunStatus::Running {
                    return Err(crate::work::WorkError::InvalidGraph(
                        "managed run is no longer running".into(),
                    ));
                }
                run.status = crate::work::ManagedRunStatus::Parking;
                run.updated_at = chrono::Utc::now();
                run.graph_id
            };
            state.revision = state.revision.saturating_add(1);
            Ok((
                (),
                crate::work::WorkEvent::GraphChanged {
                    graph_id,
                    revision: state.graph(graph_id)?.revision,
                },
            ))
        })?;
        let handle = self
            .runs
            .lock()
            .await
            .remove(run_id)
            .ok_or_else(|| format!("run {run_id} has no live driver"))?;
        handle.cancellation.cancel();
        handle
            .join
            .await
            .map_err(|error| format!("run task panicked while parking: {error}"))?;
        self.reconcile_work()?;
        let id = run_id.to_string();
        self.mutate_work(move |state| {
            let graph_id = {
                let run = state.managed_runs.get_mut(&id).ok_or_else(|| {
                    crate::work::WorkError::InvalidGraph("managed run disappeared".into())
                })?;
                run.status = crate::work::ManagedRunStatus::Parked;
                run.updated_at = chrono::Utc::now();
                run.graph_id
            };
            state.revision = state.revision.saturating_add(1);
            Ok((
                (),
                crate::work::WorkEvent::GraphChanged {
                    graph_id,
                    revision: state.graph(graph_id)?.revision,
                },
            ))
        })?;
        Ok(record.graph_id)
    }

    /// Block until a run concludes, removing its handle and returning the
    /// report. Errors if `run_id` is unknown or was already collected.
    pub async fn wait_run(&self, run_id: &str) -> Result<crate::work::RunReport, String> {
        // Keep the handle registered while it is live so a concurrent cancel
        // can always reach its token. Removing before awaiting made `await`
        // race `cancel` into a spurious unknown-run failure.
        let handle = loop {
            let mut runs = self.runs.lock().await;
            let finished = runs
                .get(run_id)
                .ok_or_else(|| format!("unknown run_id: {run_id}"))?
                .join
                .is_finished();
            if finished {
                break runs
                    .remove(run_id)
                    .expect("run checked under the same lock");
            }
            drop(runs);
            tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        };
        handle
            .join
            .await
            .map_err(|e| format!("run task panicked: {e}"))
    }

    /// Ask a run to stop and return the graph it drives. In-flight node
    /// agents observe the same token, so cancellation reaches the work
    /// rather than only the driver loop. The handle remains awaitable.
    pub async fn cancel_run(
        &self,
        run_id: &str,
        requesting_agent_id: &str,
    ) -> Result<crate::work::GraphId, String> {
        let (graph_id, cancellation) = {
            let runs = self.runs.lock().await;
            let handle = runs
                .get(run_id)
                .ok_or_else(|| format!("unknown run_id: {run_id}"))?;
            if handle.join.is_finished() {
                return Err(format!("run {run_id} has already finished"));
            }
            (handle.graph_id, handle.cancellation.clone())
        };

        let owner = requesting_agent_id.to_string();
        self.mutate_work(move |state| {
            let graph = state.graph(graph_id)?.clone();
            if graph.owner_agent_id.as_deref() != Some(owner.as_str()) {
                return Err(crate::work::WorkError::Unauthorized { agent: owner });
            }
            let auth = crate::work::AuthorizationContext {
                agent_id: owner,
                can_manage: true,
                assignment_ids: Default::default(),
            };
            for node_id in graph.view_order {
                let status = state.graph(graph_id)?.nodes[&node_id].status;
                if matches!(
                    status,
                    crate::work::ExecutionStatus::Pending
                        | crate::work::ExecutionStatus::Ready
                        | crate::work::ExecutionStatus::Running
                        | crate::work::ExecutionStatus::Failed
                        | crate::work::ExecutionStatus::Blocked
                ) {
                    let revision = state.graph(graph_id)?.revision;
                    state.cancel(graph_id, revision, &auth, node_id)?;
                }
            }
            let revision = state.graph(graph_id)?.revision;
            state.close_graph(
                graph_id,
                revision,
                &auth,
                crate::work::GraphStatus::Cancelled,
            )?;
            Ok((
                (),
                crate::work::WorkEvent::GraphChanged {
                    graph_id,
                    revision: state.graph(graph_id)?.revision,
                },
            ))
        })?;
        // Signal only after the durable state is committed. The driver can
        // therefore never publish a cancelled report with live attempts.
        cancellation.cancel();
        Ok(graph_id)
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
            tool_runtime: StdMutex::new(HashMap::new()),
            work: RwLock::new(work_state),
            work_transaction: StdMutex::new(()),
            swarm_edit_transaction: AsyncMutex::new(()),
            unavailable_agents: RwLock::new(Vec::new()),
            mailbox: RwLock::new(record.mailbox.clone()),
            persistence: SessionPersistenceCoordinator::current(),
            artifacts: Arc::new(SessionArtifacts::from_records(record.artifacts)),
            permission_broker: Arc::new(PermissionBroker::new(
                crate::permissions::PermissionPolicy::default(),
            )),
            edit_authority: RwLock::new(Arc::new(crate::InProcessEditAuthority::new())),
        };
        crate::tools::edit_history::restore(&session.id);

        // Retry agents whose provider was unavailable at the previous resume.
        // Their durable mailbox must become live once credentials/provider
        // configuration is restored, rather than remaining quarantined
        // forever in `unavailable_agents`.
        let mut unavailable = Vec::new();
        for ar in record.agents.into_iter().chain(record.unavailable_agents) {
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
            .with_mailbox(ar.mailbox.clone())
            .with_active_goal_id(ar.active_goal_id.clone())
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
            let agent = if let (Some(target), Some(remote_dir)) = (
                ar.metadata
                    .get("firmius.remote_target")
                    .and_then(|v| v.as_str()),
                ar.metadata
                    .get("firmius.remote_dir")
                    .and_then(|v| v.as_str()),
            ) {
                agent.with_host(Arc::new(crate::RemoteHost::new(
                    target,
                    Some(remote_dir.into()),
                )))
            } else {
                agent
            };
            let agent = agent
                .with_id(ar.id.clone())
                .with_todo_state(ar.todo.clone());
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
        self.snapshot_record_with_todo_candidate(work, "", &crate::todo::TodoLedger::new(""))
    }

    /// Build the exact record used by a todo transaction without installing
    /// its candidate into live AgentState. An empty `candidate_agent_id`
    /// means an ordinary snapshot with no override.
    fn snapshot_record_with_todo_candidate(
        &self,
        work: WorkState,
        candidate_agent_id: &str,
        candidate: &crate::todo::TodoLedger,
    ) -> Result<SessionRecord, String> {
        let agents = self
            .agents
            .read()
            .unwrap()
            .values()
            .map(|agent| {
                let cfg = agent.config().clone();
                let (history, compaction, mailbox, mut todo) = agent.durable_snapshot_with_todo();
                if !candidate_agent_id.is_empty() && agent.id == candidate_agent_id {
                    todo = crate::todo::PersistedTodoState::Valid(crate::todo::TodoEnvelope::new(
                        candidate.clone(),
                    ));
                }
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
                    mailbox,
                    active_goal_id: agent.active_goal_id(),
                    todo,
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
            mailbox: self.mailbox.read().unwrap().clone(),
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

    /// Durable mailbox bookkeeping for this session.
    pub fn mailbox_state(&self) -> SessionMailboxState {
        self.mailbox.read().unwrap().clone()
    }

    /// Record the goal occupying `agent_id`'s execution slot and release any
    /// previously deferred messages that now match. Passing `None` clears the
    /// slot without activating anything.
    pub fn set_agent_active_goal(
        &self,
        agent_id: &str,
        goal_id: Option<String>,
    ) -> Result<Vec<MailboxDeliveryRecord>, String> {
        if let Some(agent) = self.agent(agent_id) {
            agent.set_active_goal_id(goal_id.clone());
        } else {
            let mut unavailable = self.unavailable_agents.write().unwrap();
            if let Some(record) = unavailable.iter_mut().find(|agent| agent.id == agent_id) {
                record.active_goal_id = goal_id.clone();
            } else {
                return Err(format!("agent not found: {agent_id}"));
            }
        }
        let released = self.release_deferred_for(agent_id, goal_id.as_deref());
        self.save()?;
        Ok(released)
    }

    /// Existing send path: inject `message` as-is. Uncorrelated messages are
    /// not assigned ids or isolated by goal.
    pub fn send_message(
        &self,
        sender_id: &str,
        target_id: &str,
        mut message: crate::types::Message,
    ) -> String {
        message.correlation.sender_id = Some(sender_id.to_string());
        if message.provenance.is_none() {
            message.provenance = Some(crate::types::MessageProvenance::new(
                crate::types::MessageOrigin::Peer,
                crate::types::MessageTrust::DerivedUntrusted,
            ));
        }
        self.deliver_existing(sender_id, target_id, message)
    }

    /// Context-aware send: assign a stable id and per-thread sequence, persist
    /// a delivery record, and either inject into the live mailbox or defer
    /// when the target is executing a different goal.
    pub fn send_message_with_context(
        &self,
        sender_id: &str,
        target_id: &str,
        mut message: crate::types::Message,
        context: SendContext,
    ) -> Result<(SendOutcome, MailboxDeliveryRecord), String> {
        if let Some(message_id) = context.message_id.clone() {
            message.correlation.message_id = Some(message_id);
        }
        if let Some(thread_id) = context.thread_id.clone() {
            message.correlation.thread_id = Some(thread_id);
        }
        if let Some(goal_id) = context.goal_id.clone() {
            message.correlation.goal_id = Some(goal_id);
        }
        if let Some(run_id) = context.run_id.clone() {
            message.correlation.run_id = Some(run_id);
        }
        if let Some(parent_goal_id) = context.parent_goal_id.clone() {
            message.correlation.parent_goal_id = Some(parent_goal_id);
        }
        if let Some(workflow_node_id) = context.workflow_node_id.clone() {
            message.correlation.workflow_node_id = Some(workflow_node_id);
        }
        if let Some(assignment_id) = context.assignment_id.clone() {
            message.correlation.assignment_id = Some(assignment_id);
        }
        if let Some(in_reply_to) = context.in_reply_to.clone() {
            message.correlation.in_reply_to = Some(in_reply_to);
        }
        self.deliver_correlated(sender_id, target_id, message)
    }

    fn deliver_existing(
        &self,
        _sender_id: &str,
        target_id: &str,
        message: crate::types::Message,
    ) -> String {
        match self.agent(target_id) {
            Some(target) => {
                target.submit_message(message);
                if let Err(error) = self.save() {
                    return format!("{target_id}: failed to persist delivery: {error}");
                }
                self.wake_agent(target);
                format!("{target_id}: delivered and wake scheduled")
            }
            None if self.submit_to_unavailable_agent(target_id, message) => match self.save() {
                Ok(()) => {
                    format!(
                        "{target_id}: queued (target not currently live; durable mailbox updated)"
                    )
                }
                Err(error) => format!("{target_id}: failed to persist delivery: {error}"),
            },
            None => format!("{target_id}: queued in audit log (target not currently restorable)"),
        }
    }

    fn deliver_correlated(
        &self,
        sender_id: &str,
        target_id: &str,
        mut message: crate::types::Message,
    ) -> Result<(SendOutcome, MailboxDeliveryRecord), String> {
        let known_target = self.agent(target_id).is_some()
            || self
                .unavailable_agents
                .read()
                .unwrap()
                .iter()
                .any(|agent| agent.id == target_id)
            || self.hierarchy.read().unwrap().contains_key(target_id);

        let record = {
            let mut mailbox = self.mailbox.write().unwrap();
            if let Some(message_id) = message.correlation.message_id.as_deref()
                && let Some(existing) = mailbox.records.get(message_id)
            {
                return Ok((SendOutcome::Duplicate, existing.clone()));
            }

            let message_id = message
                .correlation
                .message_id
                .clone()
                .unwrap_or_else(|| Uuid::new_v4().to_string());
            let thread_id = message.correlation.thread_id.clone().unwrap_or_else(|| {
                message
                    .correlation
                    .goal_id
                    .clone()
                    .map(|goal| format!("goal:{goal}"))
                    .unwrap_or_else(|| format!("agent:{sender_id}->{target_id}"))
            });
            let next = mailbox
                .next_thread_seq
                .entry(thread_id.clone())
                .or_insert(1);
            let thread_seq = *next;
            *next = next.saturating_add(1);
            message.correlation.message_id = Some(message_id.clone());
            message.correlation.thread_id = Some(thread_id.clone());
            message.correlation.thread_seq = Some(thread_seq);

            let defer = self.should_defer(target_id, message.correlation.goal_id.as_deref());
            let state = if defer {
                MailboxDeliveryState::Deferred
            } else {
                MailboxDeliveryState::Delivered
            };
            let record = MailboxDeliveryRecord {
                message_id,
                recipient_id: target_id.to_string(),
                sender_id: sender_id.to_string(),
                thread_id,
                thread_seq,
                message: message.clone(),
                state,
                created_at: Utc::now(),
            };
            mailbox
                .records
                .insert(record.message_id.clone(), record.clone());
            record
        };

        if record.state == MailboxDeliveryState::Deferred {
            self.save()?;
            return Ok((SendOutcome::Deferred, record));
        }

        let outcome = match self.agent(target_id) {
            Some(target) => {
                target.submit_message(message);
                self.save()?;
                self.wake_agent(target);
                SendOutcome::Delivered
            }
            None if self.submit_to_unavailable_agent(target_id, message) => {
                self.save()?;
                SendOutcome::QueuedUnavailable
            }
            None if known_target => {
                self.save()?;
                SendOutcome::AuditOnly
            }
            None => {
                self.save()?;
                SendOutcome::AuditOnly
            }
        };
        Ok((outcome, record))
    }

    fn should_defer(&self, target_id: &str, goal_id: Option<&str>) -> bool {
        let Some(goal_id) = goal_id else {
            return false;
        };
        let active = if let Some(agent) = self.agent(target_id) {
            agent.active_goal_id()
        } else {
            self.unavailable_agents
                .read()
                .unwrap()
                .iter()
                .find(|agent| agent.id == target_id)
                .and_then(|agent| agent.active_goal_id.clone())
        };
        match active {
            Some(active) if active != goal_id => true,
            // A goal-scoped message for a recipient with no open slot stays
            // in the goal thread rather than mixing into unrelated live work.
            None => true,
            Some(_) => false,
        }
    }

    fn release_deferred_for(
        &self,
        agent_id: &str,
        goal_id: Option<&str>,
    ) -> Vec<MailboxDeliveryRecord> {
        let mut released = Vec::new();
        {
            let mut mailbox = self.mailbox.write().unwrap();
            let mut ids: Vec<(u64, String)> = mailbox
                .records
                .values()
                .filter(|record| {
                    record.recipient_id == agent_id
                        && record.state == MailboxDeliveryState::Deferred
                        && goal_id.is_some_and(|goal| {
                            record.message.correlation.goal_id.as_deref() == Some(goal)
                        })
                })
                .map(|record| (record.thread_seq, record.message_id.clone()))
                .collect();
            ids.sort_by_key(|(seq, id)| (*seq, id.clone()));
            for (_, id) in ids {
                if let Some(record) = mailbox.records.get_mut(&id) {
                    record.state = MailboxDeliveryState::Delivered;
                    released.push(record.clone());
                }
            }
        }
        for record in &released {
            if let Some(agent) = self.agent(agent_id) {
                agent.submit_message(record.message.clone());
                self.wake_agent(agent);
            } else {
                let _ = self.submit_to_unavailable_agent(agent_id, record.message.clone());
            }
        }
        released
    }
}
