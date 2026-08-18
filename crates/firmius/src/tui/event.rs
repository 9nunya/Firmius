//! Everything flows through one channel: terminal events (pumped from a
//! blocking thread), session-bus events (bridged from the broadcast bus),
//! turn completion, and ticks.

use crossterm::event::Event as TermEvent;
use firmius_core::{AgentEvent, SessionEvent};
use tokio::sync::{broadcast, mpsc};

pub enum AppEvent {
    Term(TermEvent),
    /// One agent's event, tagged with the agent id (see `SessionEvent`).
    Bus(SessionEvent),
    /// The bus receiver fell behind; transcripts should be re-derived from
    /// agent histories (the same path used for resume rendering).
    BusLagged(u64),
    /// The primary agent's `prompt()` returned. `Err` carries the error text.
    TurnDone(Result<(), String>),
    /// Result and lifecycle events from a manual compaction job.
    Compaction {
        agent_id: String,
        event: AgentEvent,
    },
    Tick,
}

/// Blocking crossterm reader on its own thread, pumping into the app channel.
/// Dies quietly when the receiver is dropped.
pub fn spawn_term_pump(tx: mpsc::UnboundedSender<AppEvent>) {
    std::thread::spawn(move || {
        while let Ok(ev) = crossterm::event::read() {
            if matches!(ev, TermEvent::Mouse(_)) {
                let _ = tx.send(AppEvent::Term(ev));
            } else if tx.send(AppEvent::Term(ev)).is_err() {
                break;
            }
        }
    });
}

/// Bridge a session bus receiver into the app channel.
pub fn spawn_bus_bridge(
    mut rx: broadcast::Receiver<SessionEvent>,
    tx: mpsc::UnboundedSender<AppEvent>,
) {
    tokio::spawn(async move {
        loop {
            match rx.recv().await {
                Ok(ev) => {
                    if tx.send(AppEvent::Bus(ev)).is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Lagged(n)) => {
                    if tx.send(AppEvent::BusLagged(n)).is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Closed) => break,
            }
        }
    });
}
