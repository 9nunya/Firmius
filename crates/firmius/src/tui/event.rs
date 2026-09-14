//! Terminal input has a dedicated bounded channel so model-event bursts can
//! never leave keystrokes waiting behind thousands of streaming deltas. Other
//! asynchronous events share a second bounded channel.

use crossterm::event::Event as TermEvent;
use firmius_client::DaemonClient;
use firmius_core::PendingPermissionRequest;
use firmius_core::SessionSummary;
use firmius_core::{AgentEvent, QuotaSnapshot, SessionEvent};
use firmius_protocol::{DaemonEvent, HostPeekResponse, SessionSnapshot, SessionStatus};
use tokio::sync::{broadcast, mpsc};

pub enum AppEvent {
    Term(TermEvent),
    /// One agent's event, tagged with the agent id (see `SessionEvent`).
    Bus(SessionEvent),
    /// Compact daemon-owned roster/work/process state. Full snapshots are
    /// reserved for attach and recovery paths.
    RemoteStatus(SessionStatus),
    /// The bus receiver fell behind; transcripts should be re-derived from
    /// agent histories (the same path used for resume rendering).
    BusLagged(u64),
    /// The canonical session snapshot must be re-read after resume/focus
    /// changes or a receiver gap.  Kept separate from transcript rebuilding
    /// so work never falls back to task result prose.
    WorkRecovery,
    /// Daemon requested an authoritative snapshot (reconnect or event gap).
    RemoteRecovery(String),
    /// The daemon connection ended; the UI keeps its last snapshot visible.
    RemoteDisconnected(String),
    /// The daemon is intentionally shutting down; do not start reconnecting.
    RemoteShutdown,
    /// Goal lifecycle notification from the daemon.
    Goal(firmius_protocol::GoalEventEnvelope),
    /// Typed native todo projection. Session-scoped, but carried on its own
    /// envelope so the rail updates before the following status refresh.
    Todo(firmius_protocol::TodoEvent),
    PermissionRequested(PendingPermissionRequest),
    PermissionResolved {
        request_id: uuid::Uuid,
        decision: firmius_core::PermissionDecision,
    },
    /// A reconnect task established a fresh client and authoritative snapshot.
    RemoteReconnected {
        client: DaemonClient,
        snapshot: Option<firmius_protocol::SessionSnapshot>,
        events: broadcast::Receiver<DaemonEvent>,
    },
    /// The primary agent's `prompt()` returned. `Err` carries the error text.
    TurnDone(Result<(), String>),
    /// A daemon-owned turn completed. Unlike `TurnDone`, this is tagged so a
    /// background agent cannot incorrectly mark the whole session idle.
    RemoteTurnDone {
        agent_id: String,
        turn_id: uuid::Uuid,
        result: Result<(), String>,
    },
    /// Result and lifecycle events from a manual compaction job.
    Compaction {
        agent_id: String,
        event: AgentEvent,
    },
    Tick,
    /// Background quota poll for the focused agent's provider.
    Quota(Result<QuotaSnapshot, String>),
    /// Result of the background saved-session lookup used by autocomplete and
    /// the sessions picker.  Keeping it on the app queue prevents filesystem
    /// scanning from blocking keystrokes or rendering.
    Sessions(Result<Vec<SessionSummary>, String>),
    /// Result of a periodic remote snapshot and host-output refresh. The
    /// requests run in a background task so daemon latency never stalls the
    /// render/input loop.
    RemoteRefresh {
        epoch: uuid::Uuid,
        snapshot: Result<
            (
                SessionSnapshot,
                Vec<(firmius_core::ProcId, HostPeekResponse)>,
            ),
            String,
        >,
    },
}

/// Bridge an already-established daemon subscription. The receiver is
/// created before attach/create so no first-turn events can race the TUI.
pub fn spawn_daemon_bridge(mut rx: broadcast::Receiver<DaemonEvent>, tx: mpsc::Sender<AppEvent>) {
    tokio::spawn(async move {
        loop {
            let app_event = match rx.recv().await {
                Ok(DaemonEvent::Session(event)) => AppEvent::Bus(event),
                Ok(DaemonEvent::SessionStatus(status)) => AppEvent::RemoteStatus(status),
                Ok(DaemonEvent::SnapshotRequired { reason, .. }) => {
                    AppEvent::RemoteRecovery(reason)
                }
                Ok(DaemonEvent::TurnCompleted {
                    agent_id,
                    turn_id,
                    result,
                    ..
                }) => AppEvent::RemoteTurnDone {
                    agent_id,
                    turn_id,
                    result,
                },
                Ok(DaemonEvent::ConnectionLost { reason }) => {
                    let _ = tx.send(AppEvent::RemoteDisconnected(reason)).await;
                    break;
                }
                Ok(DaemonEvent::ShuttingDown) => AppEvent::RemoteShutdown,
                Ok(DaemonEvent::Goal(event)) => AppEvent::Goal(event),
                Ok(DaemonEvent::Todo(event)) => AppEvent::Todo(event),
                // Memory is daemon-level and has no TUI surface yet; the
                // session status/snapshot paths remain authoritative for the
                // domains the TUI renders.
                Ok(DaemonEvent::Memory(_)) => continue,
                Ok(DaemonEvent::PermissionRequested(request)) => {
                    AppEvent::PermissionRequested(request)
                }
                Ok(DaemonEvent::PermissionResolved {
                    request_id,
                    decision,
                    ..
                }) => AppEvent::PermissionResolved {
                    request_id,
                    decision,
                },
                Ok(DaemonEvent::Attached(_)) | Ok(DaemonEvent::Ready { .. }) => continue,
                Err(broadcast::error::RecvError::Lagged(n)) => AppEvent::BusLagged(n),
                Err(broadcast::error::RecvError::Closed) => {
                    let _ = tx
                        .send(AppEvent::RemoteDisconnected(
                            "daemon connection closed".into(),
                        ))
                        .await;
                    break;
                }
            };
            if tx.send(app_event).await.is_err() {
                break;
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn closed_daemon_bridge_reports_disconnect_once_and_exits() {
        let (source, receiver) = broadcast::channel(1);
        let (sender, mut events) = mpsc::channel(4);
        spawn_daemon_bridge(receiver, sender);
        drop(source);
        assert!(matches!(
            events.recv().await,
            Some(AppEvent::RemoteDisconnected(_))
        ));
        assert!(
            tokio::time::timeout(std::time::Duration::from_secs(1), events.recv())
                .await
                .unwrap()
                .is_none()
        );
    }
}

/// Blocking crossterm reader on its own thread. Mouse motion is disposable;
/// keyboard and paste input apply backpressure rather than consuming memory.
pub fn spawn_term_pump(tx: mpsc::Sender<TermEvent>) {
    std::thread::spawn(move || {
        while let Ok(ev) = crossterm::event::read() {
            if matches!(ev, TermEvent::Mouse(_)) {
                let _ = tx.try_send(ev);
            } else if tx.blocking_send(ev).is_err() {
                break;
            }
        }
    });
}

/// Bridge a session bus receiver into the app channel.
pub fn spawn_bus_bridge(mut rx: broadcast::Receiver<SessionEvent>, tx: mpsc::Sender<AppEvent>) {
    tokio::spawn(async move {
        loop {
            match rx.recv().await {
                Ok(ev) => {
                    if tx.send(AppEvent::Bus(ev)).await.is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Lagged(n)) => {
                    if tx.send(AppEvent::BusLagged(n)).await.is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Closed) => break,
            }
        }
    });
}
