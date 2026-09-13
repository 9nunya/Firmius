//! Daemon-owned capabilities for messaging participants in an active edit
//! conflict. Capabilities are process-local and opaque; delivered messages
//! use each recipient session's durable mailbox API.

use async_trait::async_trait;
use firmius_core::tools::message::{ConflictMessageBackend, ConflictMessageRequest};
use firmius_core::{AgentRef, Message, MessageRole, SendOutcome, SessionHandle};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, Weak};
use uuid::Uuid;

/// Structured, non-authoritative details callers may display alongside the
/// opaque capability. Routing authority comes only from `conflict_id` plus
/// the authenticated qualified sender.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConflictMetadata {
    pub path: PathBuf,
    pub holder_attempt: String,
    pub contender_attempt: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConflictRegistration {
    pub conflict_id: String,
    pub participants: Vec<AgentRef>,
    pub metadata: ConflictMetadata,
}

#[derive(Default)]
struct ConflictState {
    sessions: HashMap<String, Weak<firmius_core::Session>>,
    capabilities: HashMap<String, ConflictRegistration>,
}

/// In-memory conflict capability store owned by the daemon service.
///
/// The store never resolves a bare agent id globally. Every endpoint is a
/// qualified `(session_id, agent_id)`, and authorization requires the exact
/// qualified caller to be a registered participant.
#[derive(Clone, Default)]
pub struct ConflictMessenger {
    state: Arc<Mutex<ConflictState>>,
}

impl ConflictMessenger {
    pub fn new() -> Self {
        Self::default()
    }

    /// Make a loaded session available for conflict delivery. Re-registering
    /// replaces only that session id's weak runtime handle.
    pub fn register_session(&self, session: &SessionHandle) {
        self.state
            .lock()
            .unwrap()
            .sessions
            .insert(session.id.clone(), Arc::downgrade(session));
    }

    /// Mint an opaque capability for exactly two qualified participants.
    /// Both endpoints must currently belong to registered sessions.
    pub fn register_conflict(
        &self,
        holder: AgentRef,
        contender: AgentRef,
        metadata: ConflictMetadata,
    ) -> Result<ConflictRegistration, String> {
        if holder == contender {
            return Err("an edit conflict requires two distinct participants".into());
        }
        let mut state = self.state.lock().unwrap();
        for participant in [&holder, &contender] {
            let session = state
                .sessions
                .get(&participant.session_id)
                .and_then(Weak::upgrade)
                .ok_or_else(|| "conflict participant session is not loaded".to_string())?;
            if !session
                .hierarchy
                .read()
                .unwrap()
                .contains_key(&participant.agent_id)
            {
                return Err("conflict participant agent is not in its qualified session".into());
            }
        }
        let registration = ConflictRegistration {
            conflict_id: Uuid::new_v4().to_string(),
            participants: vec![holder, contender],
            metadata,
        };
        state
            .capabilities
            .insert(registration.conflict_id.clone(), registration.clone());
        Ok(registration)
    }

    /// Revoke a capability when its active conflict ends. Revocation is
    /// idempotent and does not affect already-durable mailbox deliveries.
    pub fn revoke(&self, conflict_id: &str) -> bool {
        self.state
            .lock()
            .unwrap()
            .capabilities
            .remove(conflict_id)
            .is_some()
    }
}

#[async_trait]
impl ConflictMessageBackend for ConflictMessenger {
    async fn send_conflict_message(
        &self,
        request: ConflictMessageRequest,
    ) -> Result<String, String> {
        let sender = AgentRef::new(&request.sender_session_id, &request.sender_agent_id);

        // Resolve and clone every route while holding the capability lock,
        // then release it before entering Session persistence/mailbox code.
        // This prevents re-entrant session delivery from deadlocking the
        // conflict store and keeps locks out of wake/provider paths.
        let routes: Vec<(AgentRef, SessionHandle)> = {
            let state = self.state.lock().unwrap();
            let capability = state
                .capabilities
                .get(&request.conflict_id)
                .ok_or_else(|| "conflict capability is unknown or unauthorized".to_string())?;
            if !capability.participants.contains(&sender) {
                return Err("conflict capability is unknown or unauthorized".into());
            }
            capability
                .participants
                .iter()
                .filter(|participant| **participant != sender)
                .map(|participant| {
                    let session = state
                        .sessions
                        .get(&participant.session_id)
                        .and_then(Weak::upgrade)
                        .ok_or_else(|| "conflict participant session is unavailable".to_string())?;
                    Ok((participant.clone(), session))
                })
                .collect::<Result<_, String>>()?
        };

        let sender_id = sender.to_string();
        let mut outcomes = Vec::with_capacity(routes.len());
        for (recipient, session) in routes {
            let mut context = request.context.clone();
            context
                .thread_id
                .get_or_insert_with(|| format!("edit-conflict:{}", request.conflict_id));
            // A caller-supplied message id is stable per recipient session;
            // conflict capabilities currently have one peer, so retries are
            // idempotent through Session's durable mailbox index.
            let (outcome, _) = session.send_message_with_context(
                &sender_id,
                &recipient.agent_id,
                Message::text(MessageRole::User, request.body.clone()),
                context,
            )?;
            let status = match outcome {
                SendOutcome::Delivered => "delivered",
                SendOutcome::Deferred => "deferred",
                SendOutcome::Duplicate => "duplicate",
                SendOutcome::QueuedUnavailable => "queued",
                SendOutcome::AuditOnly => "audit_only",
            };
            outcomes.push(format!("{recipient}: {status}"));
        }
        Ok(outcomes.join("\n"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use firmius_core::providers::{Provider, ProviderError, ProviderEvent};
    use firmius_core::{Agent, AgentConfig, AgentNode, ToolRegistry};
    use futures::StreamExt;

    struct NoopProvider;

    #[async_trait]
    impl Provider for NoopProvider {
        fn id(&self) -> &str {
            "noop"
        }

        async fn stream(
            &self,
            _request: firmius_core::ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            Ok(futures::stream::empty().boxed())
        }
    }

    fn session_with_agent(agent_id: &str) -> (SessionHandle, Arc<Agent>) {
        let session = firmius_core::Session::new_handle();
        let agent = Arc::new(
            Agent::new(
                Arc::new(NoopProvider),
                Arc::new(ToolRegistry::default()),
                AgentConfig::default(),
                session.id.clone(),
            )
            .with_id(agent_id),
        );
        agent.attach_session(session.clone());
        session
            .agents
            .write()
            .unwrap()
            .insert(agent_id.to_string(), agent.clone());
        session
            .hierarchy
            .write()
            .unwrap()
            .insert(agent_id.to_string(), AgentNode::default());
        (session, agent)
    }

    fn metadata() -> ConflictMetadata {
        ConflictMetadata {
            path: PathBuf::from("src/lib.rs"),
            holder_attempt: "hold-1".into(),
            contender_attempt: "try-1".into(),
        }
    }

    #[tokio::test]
    async fn authorized_participant_routes_to_qualified_durable_mailbox() {
        let messenger = ConflictMessenger::new();
        let (holder_session, _) = session_with_agent("holder");
        let (contender_session, _) = session_with_agent("contender");
        messenger.register_session(&holder_session);
        messenger.register_session(&contender_session);
        let registration = messenger
            .register_conflict(
                AgentRef::new(&holder_session.id, "holder"),
                AgentRef::new(&contender_session.id, "contender"),
                metadata(),
            )
            .unwrap();

        let result = messenger
            .send_conflict_message(ConflictMessageRequest {
                conflict_id: registration.conflict_id.clone(),
                sender_session_id: contender_session.id.clone(),
                sender_agent_id: "contender".into(),
                body: "I can retry after your attempt".into(),
                context: firmius_core::SendContext {
                    message_id: Some("conflict-message-1".into()),
                    ..Default::default()
                },
            })
            .await
            .unwrap();

        assert!(result.contains("delivered"));
        let record = holder_session
            .mailbox_state()
            .records
            .get("conflict-message-1")
            .cloned()
            .expect("delivery must be durable");
        assert_eq!(
            record.sender_id,
            format!("{}/contender", contender_session.id)
        );
        assert_eq!(record.recipient_id, "holder");
        assert_eq!(
            record.thread_id,
            format!("edit-conflict:{}", registration.conflict_id)
        );
    }

    #[tokio::test]
    async fn same_bare_agent_id_in_another_session_is_not_authorized() {
        let messenger = ConflictMessenger::new();
        let (holder_session, _) = session_with_agent("shared-agent");
        let (contender_session, _) = session_with_agent("contender");
        let (intruder_session, _) = session_with_agent("shared-agent");
        for session in [&holder_session, &contender_session, &intruder_session] {
            messenger.register_session(session);
        }
        let registration = messenger
            .register_conflict(
                AgentRef::new(&holder_session.id, "shared-agent"),
                AgentRef::new(&contender_session.id, "contender"),
                metadata(),
            )
            .unwrap();

        let error = messenger
            .send_conflict_message(ConflictMessageRequest {
                conflict_id: registration.conflict_id,
                sender_session_id: intruder_session.id.clone(),
                sender_agent_id: "shared-agent".into(),
                body: "spoof".into(),
                context: Default::default(),
            })
            .await
            .unwrap_err();

        assert_eq!(error, "conflict capability is unknown or unauthorized");
        assert!(contender_session.mailbox_state().records.is_empty());
    }

    #[tokio::test]
    async fn revoked_and_unknown_capabilities_fail_closed() {
        let messenger = ConflictMessenger::new();
        let (holder_session, _) = session_with_agent("holder");
        let (contender_session, _) = session_with_agent("contender");
        messenger.register_session(&holder_session);
        messenger.register_session(&contender_session);
        let registration = messenger
            .register_conflict(
                AgentRef::new(&holder_session.id, "holder"),
                AgentRef::new(&contender_session.id, "contender"),
                metadata(),
            )
            .unwrap();
        assert!(messenger.revoke(&registration.conflict_id));

        let error = messenger
            .send_conflict_message(ConflictMessageRequest {
                conflict_id: registration.conflict_id,
                sender_session_id: holder_session.id.clone(),
                sender_agent_id: "holder".into(),
                body: "stale".into(),
                context: Default::default(),
            })
            .await
            .unwrap_err();
        assert_eq!(error, "conflict capability is unknown or unauthorized");
    }
}
