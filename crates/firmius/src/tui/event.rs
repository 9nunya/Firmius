//! Terminal input has a dedicated bounded channel so model-event bursts can
//! never leave keystrokes waiting behind thousands of streaming deltas. Other
//! asynchronous events share a second bounded channel.

use crossterm::event::Event as TermEvent;
use firmius_core::{AgentEvent, QuotaSnapshot, SessionEvent};
use tokio::sync::{broadcast, mpsc};

pub enum AppEvent {
    Term(TermEvent),
    /// One agent's event, tagged with the agent id (see `SessionEvent`).
    Bus(SessionEvent),
    /// The bus receiver fell behind; transcripts should be re-derived from
    /// agent histories (the same path used for resume rendering).
    BusLagged(u64),
    /// The canonical session snapshot must be re-read after resume/focus
    /// changes or a receiver gap.  Kept separate from transcript rebuilding
    /// so work never falls back to task result prose.
    WorkRecovery,
    /// The primary agent's `prompt()` returned. `Err` carries the error text.
    TurnDone(Result<(), String>),
    /// Result and lifecycle events from a manual compaction job.
    Compaction {
        agent_id: String,
        event: AgentEvent,
    },
    Tick,
    /// Background quota poll for the focused agent's provider.
    Quota(Result<QuotaSnapshot, String>),
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
