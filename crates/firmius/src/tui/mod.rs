//! The TUI proper: terminal lifecycle, the event loop, and async side
//! effects (prompt tasks, session saves, background-count refreshes).

pub mod command;
pub mod composer;
pub mod event;
pub mod model;
pub mod present;
pub mod style;
pub mod view;

use std::io;
use std::sync::Arc;
use std::time::Duration;

use crossterm::event::{DisableBracketedPaste, EnableBracketedPaste};
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use crossterm::ExecutableCommand;
use firmius_core::{Agent, ProcStatus, Session};
use ratatui::backend::CrosstermBackend;
use ratatui::Terminal;
use tokio::sync::{mpsc, Mutex};

use event::AppEvent;
use model::{items_from_history, Action, Model};

pub async fn run(
    session: Arc<Mutex<Session>>,
    primary: Arc<Agent>,
    provider_id: String,
) -> Result<(), String> {
    // Panic hook: restore the terminal before printing, or the backtrace
    // lands on a raw-mode alternate screen nobody can read.
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let _ = disable_raw_mode();
        let _ = io::stdout().execute(LeaveAlternateScreen);
        default_hook(info);
    }));

    enable_raw_mode().map_err(|e| e.to_string())?;
    io::stdout()
        .execute(EnterAlternateScreen)
        .and_then(|s| s.execute(EnableBracketedPaste))
        .map_err(|e| e.to_string())?;

    let backend = CrosstermBackend::new(io::stdout());
    let mut terminal = Terminal::new(backend).map_err(|e| e.to_string())?;

    let (tx, mut rx) = mpsc::unbounded_channel::<AppEvent>();
    event::spawn_term_pump(tx.clone());
    let bus_rx = session.lock().await.subscribe();
    event::spawn_bus_bridge(bus_rx, tx.clone());

    let mut model = Model::new(session.clone(), primary.clone(), provider_id);
    let mut ticks = tokio::time::interval(Duration::from_millis(250));
    ticks.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    let outcome: Result<(), String> = loop {
        terminal
            .draw(|f| view::draw(&model, f))
            .map_err(|e| e.to_string())?;

        tokio::select! {
            _ = ticks.tick() => {
                model.update(AppEvent::Tick);
                refresh_async(&mut model).await;
            }
            incoming = rx.recv() => {
                let Some(ev) = incoming else { break Ok(()) };
                let action = match ev {
                    ev @ AppEvent::BusLagged(n) => {
                        model.flash(&format!("bus lagged ({n} events) — rebuilding"));
                        model.update(ev)
                    }
                    AppEvent::TurnDone(res) => {
                        let a = model.update(AppEvent::TurnDone(res));
                        if let Err(e) = session.lock().await.save() {
                            model.flash(&format!("save failed: {e}"));
                        }
                        a
                    }
                    other => model.update(other),
                };
                match action {
                    Action::Quit => break Ok(()),
                    Action::RebuildTranscripts => {
                        let s = session.lock().await;
                        for agent in s.agents.values() {
                            model.transcripts.insert(
                                agent.id.clone(),
                                items_from_history(&agent.history()),
                            );
                        }
                    }
                    Action::Submit(text, token) => {
                        let agent = model.primary.clone();
                        let tx2 = tx.clone();
                        tokio::spawn(async move {
                            // The session bus carries every event; the
                            // observer has nothing left to do.
                            let res = agent.prompt(text, token, |_| {}).await;
                            let _ = tx2.send(AppEvent::TurnDone(
                                res.map(|_| ()).map_err(|e| e.to_string()),
                            ));
                        });
                    }
                    Action::Continue => {}
                }
            }
        }
    };

    // Teardown: leave the alternate screen, then persist.
    let _ = io::stdout()
        .execute(DisableBracketedPaste)
        .and_then(|s| s.execute(LeaveAlternateScreen));
    let _ = disable_raw_mode();
    if let Err(e) = session.lock().await.save() {
        eprintln!("warning: could not save session: {e}");
    }
    outcome
}

/// Async state refreshes the synchronous update loop can't do itself:
/// the agent roster, background counts, and bash output tails.
async fn refresh_async(model: &mut Model) {
    let session = model.session.lock().await;
    model.roster = session
        .agents
        .iter()
        .enumerate()
        .map(|(i, (id, _))| {
            (
                id.clone(),
                if i == 0 { "main".to_string() } else { format!("agent {}", i) },
            )
        })
        .collect();
    model.bg_agents = session
        .active_delegates()
        .await
        .iter()
        .filter(|d| !d.finished)
        .count();
    if let Some(agent) = session.agent(&model.focused_id) {
        let host = agent.host();
        let infos = host.list_info();
        model.bg_procs = infos
            .iter()
            .filter(|p| matches!(p.status, ProcStatus::Running))
            .count();
        // Live tails for running processes (last ~2KB, lossy).
        for info in infos.iter().filter(|p| matches!(p.status, ProcStatus::Running)) {
            if let Ok((bytes, _, _)) = host.peek(info.id, 0) {
                let start = bytes.len().saturating_sub(2048);
                let tail = String::from_utf8_lossy(&bytes[start..]).to_string();
                model.host_tails.insert(info.cmdline.clone(), tail);
            }
        }
    }
}