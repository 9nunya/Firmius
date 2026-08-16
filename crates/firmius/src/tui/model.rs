//! App state and the pure update function. One match block for keys, one
//! fold function for agent events, no I/O anywhere in this file.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Instant;

use crossterm::event::{Event as TermEvent, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use firmius_core::{
    Agent, AgentEvent, Context, MessagePart, MessageRole, Session, SessionEvent,
};
use tokio::sync::Mutex;
use tokio_util::sync::CancellationToken;

use super::composer::{Composer, PASTE_BLOCK_THRESHOLD};
use super::command;
use super::event::AppEvent;

// ---------------------------------------------------------------------------
// Transcript items — the data every renderer consumes
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub enum ToolState {
    Running(Instant),
    Done { ok: bool, bytes: usize },
    /// Call recorded in persisted history with no result (turn was cut).
    Interrupted,
}

#[derive(Debug, Clone)]
pub enum Item {
    User(String),
    Text(String),
    Thinking(String),
    ToolCall { name: String, args: String, state: ToolState },
    Note(String),
}

/// Fold one live agent event into a transcript.
pub fn fold_event(items: &mut Vec<Item>, ev: &AgentEvent) {
    match ev {
        AgentEvent::Thinking(d) => match items.last_mut() {
            Some(Item::Thinking(t)) => t.push_str(d),
            _ => items.push(Item::Thinking(d.clone())),
        },
        AgentEvent::Text(d) => match items.last_mut() {
            Some(Item::Text(t)) => t.push_str(d),
            _ => items.push(Item::Text(d.clone())),
        },
        AgentEvent::ToolCallStarted { name, args } => items.push(Item::ToolCall {
            name: name.clone(),
            args: args.clone(),
            state: ToolState::Running(Instant::now()),
        }),
        AgentEvent::ToolResult { name, ok, content } => {
            // Finalize the most recent running call with this name.
            let state = items.iter_mut().rev().find_map(|it| match it {
                Item::ToolCall { name: n, state: s @ ToolState::Running(_), .. } if n == name => {
                    Some(s)
                }
                _ => None,
            });
            match state {
                Some(s) => *s = ToolState::Done { ok: *ok, bytes: content.len() },
                None => items.push(Item::ToolCall {
                    name: name.clone(),
                    args: String::new(),
                    state: ToolState::Done { ok: *ok, bytes: content.len() },
                }),
            }
        }
        AgentEvent::ToolCallDelta { .. } | AgentEvent::Usage(_) | AgentEvent::TurnFinished => {}
    }
}

/// Derive transcript items from a persisted/live history. Also the recovery
/// path for bus lag, resume rendering, and subagent views. Positional
/// call/result pairing is fine for display purposes.
pub fn items_from_history(history: &Context) -> Vec<Item> {
    let mut items = Vec::new();
    for msg in history {
        match msg.role {
            MessageRole::System => {}
            MessageRole::User => {
                let text: String = msg
                    .content
                    .iter()
                    .filter_map(|p| match p {
                        MessagePart::Text(t) => Some(t.as_str()),
                        _ => None,
                    })
                    .collect::<Vec<_>>()
                    .join("\n");
                if !text.is_empty() {
                    items.push(Item::User(text));
                }
            }
            MessageRole::Assistant => {
                for part in &msg.content {
                    match part {
                        MessagePart::Thinking { content, .. } if !content.is_empty() => {
                            items.push(Item::Thinking(content.clone()))
                        }
                        MessagePart::Text(t) if !t.is_empty() => items.push(Item::Text(t.clone())),
                        MessagePart::ToolCall { name, args, .. } => items.push(Item::ToolCall {
                            name: name.clone(),
                            args: args.clone(),
                            state: ToolState::Interrupted,
                        }),
                        _ => {}
                    }
                }
            }
            MessageRole::Tool => {
                for part in &msg.content {
                    if let MessagePart::ToolResult { content, ok, .. } = part {
                        let state = items.iter_mut().rev().find_map(|it| match it {
                            Item::ToolCall { state: s @ ToolState::Interrupted, .. } => Some(s),
                            _ => None,
                        });
                        if let Some(s) = state {
                            *s = ToolState::Done { ok: *ok, bytes: content.len() };
                        }
                    }
                }
            }
        }
    }
    items
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

#[derive(Debug, Default)]
pub struct Viewport {
    pub offset: usize,
    pub follow: bool,
}

pub enum Action {
    Continue,
    Quit,
    /// Text to send to the primary agent, with the turn's cancel token
    /// (already stored in `Model.cancel` for Esc).
    Submit(String, CancellationToken),
    /// Bus lagged: transcripts must be re-derived from histories (async).
    RebuildTranscripts,
}

pub struct Model {
    pub session: Arc<Mutex<Session>>,
    pub primary: Arc<Agent>,
    pub primary_id: String,
    pub focused_id: String,
    pub provider_id: String,
    /// agent_id -> transcript items (created lazily on first event).
    pub transcripts: HashMap<String, Vec<Item>>,
    /// (agent_id, label) in insertion order; refreshed by the app loop.
    pub roster: Vec<(String, String)>,
    pub composer: Composer,
    /// Paste store; composer segments reference these by 1-based id.
    pub pastes: Vec<String>,
    pub busy: bool,
    pub turn_started: Option<Instant>,
    pub cancel: Option<CancellationToken>,
    pub tick_phase: usize,
    /// Transient status-bar note with its creation time (TTL applied in view).
    pub note: Option<(String, Instant)>,
    pub viewport: Viewport,
    /// Background counts for the bottom bar, refreshed by the app loop.
    pub bg_procs: usize,
    pub bg_agents: usize,
    /// cmdline -> last output tail, for bash live-tail presentations.
    pub host_tails: HashMap<String, String>,
}

impl Model {
    pub fn new(session: Arc<Mutex<Session>>, primary: Arc<Agent>, provider_id: String) -> Self {
        let primary_id = primary.id.clone();
        let mut transcripts = HashMap::new();
        transcripts.insert(primary_id.clone(), items_from_history(&primary.history()));
        Self {
            session,
            primary,
            primary_id: primary_id.clone(),
            focused_id: primary_id.clone(),
            provider_id,
            transcripts,
            roster: vec![(primary_id, "main".to_string())],
            composer: Composer::new(),
            pastes: Vec::new(),
            busy: false,
            turn_started: None,
            cancel: None,
            tick_phase: 0,
            note: None,
            viewport: Viewport { offset: 0, follow: true },
            bg_procs: 0,
            bg_agents: 0,
            host_tails: HashMap::new(),
        }
    }

    pub fn flash(&mut self, msg: &str) {
        self.note = Some((msg.to_string(), Instant::now()));
    }

    pub fn focused_transcript(&self) -> &[Item] {
        self.transcripts.get(&self.focused_id).map_or(&[][..], |v| v)
    }

    pub fn focus_label(&self) -> String {
        self.roster
            .iter()
            .find(|(id, _)| id == &self.focused_id)
            .map(|(_, label)| label.clone())
            .unwrap_or_else(|| "agent".to_string())
    }

    pub fn cycle_focus(&mut self, dir: i32) {
        if self.roster.len() <= 1 {
            return;
        }
        let idx = self
            .roster
            .iter()
            .position(|(id, _)| id == &self.focused_id)
            .unwrap_or(0);
        let n = self.roster.len() as i32;
        let next = (idx as i32 + dir).rem_euclid(n) as usize;
        self.focused_id = self.roster[next].0.clone();
        self.viewport.follow = true;
    }

    // ------------------------------------------------------------------
    // Update
    // ------------------------------------------------------------------

    pub fn update(&mut self, ev: AppEvent) -> Action {
        match ev {
            AppEvent::Tick => {
                self.tick_phase = self.tick_phase.wrapping_add(1);
                if let Some((_, at)) = &self.note
                    && at.elapsed().as_secs() >= 3
                {
                    self.note = None;
                }
                Action::Continue
            }
            AppEvent::Bus(SessionEvent { agent_id, event }) => {
                let items = self.transcripts.entry(agent_id).or_default();
                fold_event(items, &event);
                Action::Continue
            }
            AppEvent::BusLagged(_) => Action::RebuildTranscripts,
            AppEvent::TurnDone(res) => {
                self.busy = false;
                self.turn_started = None;
                self.cancel = None;
                if let Err(e) = res {
                    if e.contains("cancelled") {
                        self.flash("cancelled");
                    } else {
                        self.transcripts
                            .entry(self.primary_id.clone())
                            .or_default()
                            .push(Item::Note(format!("error: {e}")));
                        self.flash("turn failed");
                    }
                }
                Action::Continue
            }
            AppEvent::Term(te) => match te {
                TermEvent::Key(k) if k.kind != KeyEventKind::Release => self.key(k),
                TermEvent::Paste(text) => {
                    if text.chars().count() > PASTE_BLOCK_THRESHOLD {
                        self.pastes.push(text);
                        self.composer.insert_paste_block(self.pastes.len());
                    } else {
                        self.composer.insert_str(&text);
                    }
                    Action::Continue
                }
                _ => Action::Continue,
            },
        }
    }

    /// The one and only key dispatch.
    fn key(&mut self, k: KeyEvent) -> Action {
        use KeyCode as C;
        let m = k.modifiers;
        match k.code {
            C::Char('c') if m.contains(KeyModifiers::CONTROL) => return Action::Quit,
            C::Char('n') if m.contains(KeyModifiers::CONTROL) => {
                self.cycle_focus(1);
                return Action::Continue;
            }
            C::Char('b')
                if m.contains(KeyModifiers::CONTROL) && m.contains(KeyModifiers::SHIFT) =>
            {
                self.cycle_focus(-1);
                return Action::Continue;
            }
            C::Char('u') if m.contains(KeyModifiers::CONTROL) => {
                self.composer.clear();
                return Action::Continue;
            }
            C::Esc => {
                if let Some(c) = &self.cancel {
                    c.cancel();
                    self.flash("cancelling…");
                }
                return Action::Continue;
            }
            C::Enter => {
                if m.contains(KeyModifiers::ALT) || m.contains(KeyModifiers::SHIFT) {
                    self.composer.newline();
                    return Action::Continue;
                }
                return self.submit();
            }
            C::Backspace => self.composer.backspace(),
            C::Delete => self.composer.delete(),
            C::Left => self.composer.left(),
            C::Right => self.composer.right(),
            C::Up => self.composer.up(),
            C::Down => self.composer.down(),
            C::Home => self.composer.home(),
            C::End => self.composer.end(),
            C::Tab => {} // completion popup: later phase
            C::Char(c) if !m.contains(KeyModifiers::CONTROL) => self.composer.insert_char(c),
            _ => {}
        }
        Action::Continue
    }

    fn submit(&mut self) -> Action {
        if self.busy {
            self.flash("busy — esc to cancel");
            return Action::Continue;
        }
        let Some(text) = self.composer.take(&self.pastes) else {
            return Action::Continue;
        };
        if text.starts_with('/') {
            return self.run_command(&text);
        }
        self.transcripts
            .entry(self.primary_id.clone())
            .or_default()
            .push(Item::User(text.clone()));
        self.busy = true;
        self.turn_started = Some(Instant::now());
        self.viewport.follow = true;
        let token = CancellationToken::new();
        self.cancel = Some(token.clone());
        Action::Submit(text, token)
    }

    fn run_command(&mut self, line: &str) -> Action {
        use command::Command;
        match command::parse(line) {
            Ok(Command::Quit) => Action::Quit,
            Ok(Command::Help) => {
                let help = command::help_text();
                self.transcripts
                    .entry(self.primary_id.clone())
                    .or_default()
                    .push(Item::Note(help));
                Action::Continue
            }
            Ok(other) => {
                if self.busy && !command::busy_ok(&other) {
                    self.flash("busy — try again after the turn");
                } else {
                    self.flash(&format!("not wired yet: {}", other.name()));
                }
                Action::Continue
            }
            Err(e) => {
                self.flash(&e.to_string());
                Action::Continue
            }
        }
    }
}