//! App state and the pure update function. One match block for keys, one
//! fold function for agent events, no I/O anywhere in this file.

use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::Arc;
use std::time::Instant;

use crossterm::event::{
    Event as TermEvent, KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseEvent, MouseEventKind,
};
use firmius_core::{
    AccountRecord, Agent, AgentConfig, AgentError, AgentEvent, Context, EffortMode, MessagePart,
    MessageRole, ProviderManager, Session, SessionEvent, ToolRegistry, list_sessions,
};
use ratatui::text::Line;
use tokio::sync::Mutex;
use tokio_util::sync::CancellationToken;

use super::command;
use super::composer::{Composer, PASTE_BLOCK_THRESHOLD};
use super::event::AppEvent;
use super::modal::ModalSurface;

// ---------------------------------------------------------------------------
// Transcript items — the data every renderer consumes
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub enum ToolState {
    /// The provider is still streaming the tool name/arguments.
    Preparing(Instant),
    Running(Instant),
    Done {
        ok: bool,
        bytes: usize,
    },
    /// Call recorded in persisted history with no result (turn was cut).
    Interrupted,
}

#[derive(Debug, Clone)]
pub enum Item {
    User(String),
    Text(String),
    Thinking(String),
    ToolCall {
        /// Provider tool-call id, when available, used to merge streaming
        /// deltas into the eventual started/result presentation.
        stream_id: Option<String>,
        stream_index: u32,
        name: String,
        args: String,
        state: ToolState,
    },
    Note(String),
}

#[derive(Debug, Clone)]
pub struct CompletionItem {
    pub insert: String,
    pub label: String,
    pub detail: String,
}

#[derive(Debug, Clone)]
pub struct CompletionState {
    pub items: Vec<CompletionItem>,
    pub selected: usize,
}

/// Return a small score when `query` is a case-insensitive subsequence of
/// `candidate`. Lower scores are better, with contiguous and prefix matches
/// naturally winning over scattered matches.
fn fuzzy_score(query: &str, candidate: &str) -> Option<usize> {
    let query = query.to_lowercase();
    let candidate = candidate.to_lowercase();
    if query.is_empty() {
        return Some(0);
    }
    let mut cursor = 0;
    let mut score = 0;
    let mut previous = None;
    for needle in query.chars() {
        let relative = candidate[cursor..].find(needle)?;
        let found = cursor + relative;
        score += relative + found / 32;
        if let Some(previous) = previous
            && found == previous + 1
        {
            score = score.saturating_sub(2);
        }
        previous = Some(found);
        cursor = found + needle.len_utf8();
    }
    Some(score)
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
        AgentEvent::ToolCallDelta {
            index,
            id,
            name_delta,
            args_delta,
        } => {
            // ToolCallStarted is emitted only after the provider has finished
            // streaming the assistant message. Create the running item now,
            // so the TUI can present the tool as soon as its first delta lands.
            let existing = items.iter_mut().rev().find_map(|item| match item {
                Item::ToolCall {
                    stream_id,
                    stream_index,
                    name,
                    args,
                    state: ToolState::Preparing(_),
                } if (stream_id.as_deref() == Some(id.as_str()) && !id.is_empty())
                    || (*stream_index == *index && (id.is_empty() || stream_id.is_none()))
                    || (id.is_empty()
                        && (name_delta.is_empty() || name.is_empty() || name == name_delta)) =>
                {
                    Some((name, args))
                }
                _ => None,
            });
            if let Some((name, args)) = existing {
                if !name_delta.is_empty() {
                    *name = name_delta.clone();
                }
                args.push_str(args_delta);
            } else {
                items.push(Item::ToolCall {
                    stream_id: (!id.is_empty()).then(|| id.clone()),
                    stream_index: *index,
                    name: name_delta.clone(),
                    args: args_delta.clone(),
                    state: ToolState::Preparing(Instant::now()),
                });
            }
        }
        AgentEvent::ToolCallStarted {
            index,
            id,
            name,
            args,
        } => {
            // Reconcile the placeholder with the final assembled call. Exact
            // args matching avoids merging two same-named calls incorrectly.
            let existing = items.iter_mut().rev().find_map(|item| match item {
                Item::ToolCall {
                    stream_id,
                    stream_index,
                    name: current_name,
                    args: current_args,
                    state: ToolState::Preparing(_),
                    ..
                } if (stream_id.as_deref() == Some(id.as_str()) && !id.is_empty())
                    || (*stream_index == *index
                        && current_name == name
                        && current_args == args) =>
                {
                    Some(item)
                }
                _ => None,
            });
            if let Some(Item::ToolCall {
                stream_id,
                args: current_args,
                state,
                ..
            }) = existing
            {
                if stream_id.is_none() && !id.is_empty() {
                    *stream_id = Some(id.clone());
                }
                *current_args = args.clone();
                *state = ToolState::Running(Instant::now());
            } else {
                items.push(Item::ToolCall {
                    stream_id: (!id.is_empty()).then(|| id.clone()),
                    stream_index: *index,
                    name: name.clone(),
                    args: args.clone(),
                    state: ToolState::Running(Instant::now()),
                });
            }
        }
        AgentEvent::ToolResult {
            index,
            id,
            name,
            ok,
            content,
        } => {
            // Finalize the most recent running call with this name.
            let existing = items.iter_mut().rev().find_map(|it| match it {
                Item::ToolCall {
                    stream_id,
                    stream_index,
                    name: n,
                    state: ToolState::Preparing(_) | ToolState::Running(_),
                    ..
                } if (stream_id.as_deref() == Some(id.as_str()) && !id.is_empty())
                    || (*stream_index == *index && n == name) =>
                {
                    Some(it)
                }
                _ => None,
            });
            match existing {
                Some(Item::ToolCall { state, .. }) => {
                    *state = ToolState::Done {
                        ok: *ok,
                        bytes: content.len(),
                    }
                }
                Some(_) => {}
                None => items.push(Item::ToolCall {
                    stream_id: (!id.is_empty()).then(|| id.clone()),
                    stream_index: *index,
                    name: name.clone(),
                    args: String::new(),
                    state: ToolState::Done {
                        ok: *ok,
                        bytes: content.len(),
                    },
                }),
            }
        }
        AgentEvent::Usage(_) | AgentEvent::TurnFinished => {}
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
                        MessagePart::ToolCall { id, name, args } => items.push(Item::ToolCall {
                            stream_id: Some(id.clone()),
                            stream_index: 0,
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
                            Item::ToolCall {
                                stream_id: _,
                                stream_index: _,
                                state: s @ ToolState::Interrupted,
                                ..
                            } => Some(s),
                            _ => None,
                        });
                        if let Some(s) = state {
                            *s = ToolState::Done {
                                ok: *ok,
                                bytes: content.len(),
                            };
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
    /// Number of rendered lines above the bottom position. Zero means follow
    /// the newest output, so scroll direction stays symmetric without needing
    /// the rendered transcript height in the input handler.
    pub offset: usize,
    pub follow: bool,
}

pub struct RenderCache {
    pub focused_id: String,
    pub width: u16,
    pub lines: Vec<Line<'static>>,
}

impl Viewport {
    pub fn scroll(&mut self, delta: isize) {
        if delta < 0 {
            self.offset = self.offset.saturating_add((-delta) as usize);
            self.follow = false;
        } else {
            self.offset = self.offset.saturating_sub(delta as usize);
            self.follow = self.offset == 0;
        }
    }
}

pub enum Action {
    Continue,
    Quit,
    /// Text to send to the focused agent, with the turn's cancel token
    /// (already stored in `Model.cancel` for Esc).
    Submit {
        agent_id: String,
        text: String,
        token: CancellationToken,
    },
    /// Bus lagged: transcripts must be re-derived from histories (async).
    RebuildTranscripts,
    Save,
    Resume(Option<String>),
    /// Open a setup wizard. `None` = pick the kind first.
    OpenLogin {
        kind: Option<String>,
    },
    /// Show stored accounts and quota for a provider.
    OpenAccounts {
        provider: String,
    },
    /// A wizard finished: persist and register the new account.
    RegisterAccount {
        record: AccountRecord,
    },
}

pub struct Model {
    pub session: Option<Arc<Mutex<Session>>>,
    pub primary: Option<Arc<Agent>>,
    pub primary_id: String,
    pub focused_id: String,
    pub provider_id: String,
    /// The model to use when the first message creates the agent.
    pub model: String,
    /// The reasoning effort to use when the first message creates the agent.
    pub effort: Option<EffortMode>,
    pub tools: Arc<ToolRegistry>,
    /// Interior-mutable: `/login` registers accounts into it mid-session.
    /// A std mutex — every access is a brief sync read/write, no awaits held.
    pub manager: Arc<std::sync::Mutex<ProviderManager>>,
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
    pub completion: Option<CompletionState>,
    /// Focused agent context usage, refreshed from its latest provider usage.
    pub ctx_used: u32,
    pub ctx_max: u32,
    /// Model-provided effort modes keyed by agent id.
    pub agent_efforts: HashMap<String, Vec<EffortMode>>,
    /// Parent agent id -> (delegate tool-call id, child agent id). Matching by
    /// tool-call id prevents a new streaming delegate call from inheriting a
    /// previous call's output window.
    pub delegate_children: HashMap<String, Vec<(Option<String>, String)>>,
    /// Child agent id -> parent agent id, refreshed from the session hierarchy.
    pub parent_by_agent: HashMap<String, String>,
    pub render_cache: RefCell<Option<RenderCache>>,
    /// The open modal, if any. While `Some`, keys go to it, not the composer.
    pub modal: Option<Box<dyn ModalSurface>>,
}

impl Model {
    pub fn new(
        session: Option<Arc<Mutex<Session>>>,
        primary: Option<Arc<Agent>>,
        provider_id: String,
        manager: Arc<std::sync::Mutex<ProviderManager>>,
        model: String,
        tools: Arc<ToolRegistry>,
    ) -> Self {
        let primary_id = primary
            .as_ref()
            .map(|agent| agent.id.clone())
            .unwrap_or_else(|| "welcome".to_string());
        let mut transcripts = HashMap::new();
        if let Some(agent) = &primary {
            transcripts.insert(primary_id.clone(), items_from_history(&agent.history()));
        }
        let has_primary = primary.is_some();
        Self {
            session,
            primary,
            primary_id: primary_id.clone(),
            focused_id: primary_id.clone(),
            provider_id,
            model,
            effort: None,
            tools,
            manager,
            transcripts,
            roster: if has_primary {
                vec![(primary_id, "main".to_string())]
            } else {
                Vec::new()
            },
            composer: Composer::new(),
            pastes: Vec::new(),
            busy: false,
            turn_started: None,
            cancel: None,
            tick_phase: 0,
            note: None,
            viewport: Viewport {
                offset: 0,
                follow: true,
            },
            bg_procs: 0,
            bg_agents: 0,
            host_tails: HashMap::new(),
            completion: None,
            ctx_used: 0,
            ctx_max: 0,
            agent_efforts: HashMap::new(),
            delegate_children: HashMap::new(),
            parent_by_agent: HashMap::new(),
            render_cache: RefCell::new(None),
            modal: None,
        }
    }

    pub fn has_agent(&self) -> bool {
        self.primary.is_some()
    }

    fn ensure_started(&mut self) -> Result<(), String> {
        if self.primary.is_some() {
            return Ok(());
        }
        let provider_id = {
            let manager = self.manager.lock().unwrap();
            if manager.schema(&self.provider_id).is_some() {
                self.provider_id.clone()
            } else {
                manager
                    .provider_ids()
                    .first()
                    .map(|id| (*id).to_string())
                    .ok_or_else(|| "no provider configured — use /login first".to_string())?
            }
        };
        let provider = self.manager.lock().unwrap().build(&provider_id)?;
        let config = AgentConfig {
            provider_id: provider_id.clone(),
            model: self.model.clone(),
            system_prompt: Some(
                "You are a madman crazy CLI coding assistant. Use tools when needed.
                Play along and make the user think you're crazy, but always say you're not like a madman.
                You are insane in the way that your thoughts are superintelligent, and you are in the top of all fields.".into(),
            ),
            max_tokens: Some(32900),
            effort: self.effort.clone(),
            ..Default::default()
        };
        let mut session = Session::new();
        let agent = session.spawn_agent(provider, self.tools.clone(), config);
        let session = Arc::new(Mutex::new(session));
        session
            .try_lock()
            .expect("new session mutex is uncontended")
            .bind_self(&session);
        self.session = Some(session);
        self.primary = Some(agent.clone());
        self.primary_id = agent.id.clone();
        self.focused_id = self.primary_id.clone();
        self.provider_id = provider_id;
        self.roster = vec![(self.primary_id.clone(), "main".to_string())];
        self.transcripts.insert(
            self.primary_id.clone(),
            items_from_history(&agent.history()),
        );
        Ok(())
    }

    pub fn clear_render_cache(&self) {
        self.render_cache.borrow_mut().take();
    }

    pub fn flash(&mut self, msg: &str) {
        self.note = Some((msg.to_string(), Instant::now()));
    }

    pub fn focused_transcript(&self) -> &[Item] {
        self.transcripts
            .get(&self.focused_id)
            .map_or(&[][..], |v| v)
    }

    pub fn focus_label(&self) -> String {
        if !self.has_agent() {
            return "welcome".to_string();
        }
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
        self.clear_render_cache();
        self.refresh_completion();
    }

    pub fn scroll(&mut self, delta: isize) {
        self.viewport.scroll(delta);
    }

    fn mouse(&mut self, event: MouseEvent) {
        match event.kind {
            MouseEventKind::ScrollUp => self.scroll(-3),
            MouseEventKind::ScrollDown => self.scroll(3),
            _ => {}
        }
    }

    pub(crate) fn refresh_completion(&mut self) {
        if self.modal.is_some() {
            self.completion = None;
            return;
        }
        let text = self.composer.text(&self.pastes);
        if !text.starts_with('/') {
            self.completion = None;
            return;
        }

        let mut items = Vec::new();
        if !text.contains(char::is_whitespace) {
            if text == "/effort" {
                self.push_effort_completions(&mut items, "");
            }
            for info in command::table() {
                if info.name.starts_with(&text) {
                    let insert = if info.args.is_empty() {
                        info.name.to_string()
                    } else {
                        format!("{} ", info.name)
                    };
                    items.push(CompletionItem {
                        insert,
                        label: info.name.to_string(),
                        detail: format!(
                            "{}{}",
                            info.args,
                            if info.args.is_empty() { "" } else { "  " }
                        ) + info.help,
                    });
                }
            }
        } else if let Some(space) = text.find(char::is_whitespace) {
            let head = &text[..space];
            let partial = text[space..].trim_start();
            match head {
                "/resume" => {
                    if let Ok(sessions) = list_sessions() {
                        for session in sessions {
                            let score = fuzzy_score(partial, &session.title)
                                .into_iter()
                                .chain(fuzzy_score(partial, &session.id))
                                .min();
                            if score.is_some() {
                                items.push(CompletionItem {
                                    insert: format!("/resume {}", session.id),
                                    label: session.id,
                                    detail: format!(
                                        "{}  ·  {} agents",
                                        session.title, session.agent_count
                                    ),
                                });
                            }
                        }
                    }
                }
                "/model" => {
                    for (provider, id) in self.manager.lock().unwrap().model_choices() {
                        let reference = format!("{provider}/{id}");
                        if fuzzy_score(partial, &reference).is_some()
                            || fuzzy_score(partial, &id).is_some()
                            || fuzzy_score(partial, &provider).is_some()
                        {
                            items.push(CompletionItem {
                                insert: format!("/model {reference}"),
                                label: reference,
                                detail: format!("model · provider {provider}"),
                            });
                        }
                    }
                }
                "/login" => {
                    for kind in self.manager.lock().unwrap().kinds() {
                        if fuzzy_score(partial, kind.name()).is_some() {
                            items.push(CompletionItem {
                                insert: format!("/login {}", kind.name()),
                                label: kind.name().to_string(),
                                detail: kind.display_name().to_string(),
                            });
                        }
                    }
                }
                "/accounts" => {
                    let mut providers = self.manager.lock().unwrap().kind_names();
                    providers.extend(
                        self.manager
                            .lock()
                            .unwrap()
                            .provider_ids()
                            .into_iter()
                            .map(str::to_string),
                    );
                    providers.sort();
                    providers.dedup();
                    for provider in providers {
                        if fuzzy_score(partial, &provider).is_some() {
                            items.push(CompletionItem {
                                insert: format!("/accounts {provider}"),
                                label: provider,
                                detail: "stored accounts and quota".into(),
                            });
                        }
                    }
                }
                "/effort" => self.push_effort_completions(&mut items, partial),
                _ => {}
            }
        }

        if let Some(space) = text.find(char::is_whitespace) {
            let query = text[space..].trim_start();
            items.sort_by_key(|item| {
                fuzzy_score(query, &item.label)
                    .into_iter()
                    .chain(fuzzy_score(query, &item.detail))
                    .min()
                    .unwrap_or(usize::MAX)
            });
        }
        self.completion = (!items.is_empty()).then_some(CompletionState { items, selected: 0 });
    }

    fn push_effort_completions(&self, items: &mut Vec<CompletionItem>, partial: &str) {
        let efforts = self
            .agent_efforts
            .get(&self.focused_id)
            .cloned()
            .or_else(|| {
                self.manager
                    .lock()
                    .unwrap()
                    .model_info_for(&self.provider_id, &self.model)
                    .map(|info| info.effort_modes.clone())
            })
            .unwrap_or_default();
        for effort in efforts {
            if partial.is_empty() || fuzzy_score(partial, &effort.name).is_some() {
                items.push(CompletionItem {
                    insert: format!("/effort {}", effort.name),
                    label: effort.name,
                    detail: "focused model reasoning effort".into(),
                });
            }
        }
    }

    fn completion_move(&mut self, dir: i32) -> bool {
        let Some(completion) = &mut self.completion else {
            return false;
        };
        let len = completion.items.len() as i32;
        completion.selected = (completion.selected as i32 + dir).rem_euclid(len) as usize;
        true
    }

    fn accept_completion(&mut self) -> bool {
        let Some(completion) = self.completion.take() else {
            return false;
        };
        let Some(item) = completion.items.get(completion.selected) else {
            return false;
        };
        self.composer.replace_text(&item.insert);
        true
    }

    pub fn replace_session(&mut self, session: Arc<Mutex<Session>>, primary: Arc<Agent>) {
        self.session = Some(session);
        self.primary = Some(primary.clone());
        self.primary_id = primary.id.clone();
        self.focused_id = self.primary_id.clone();
        self.provider_id = primary.config().provider_id.clone();
        self.model = primary.config().model.clone();
        self.effort = primary.config().effort.clone();
        self.transcripts.clear();
        self.transcripts.insert(
            self.primary_id.clone(),
            items_from_history(&primary.history()),
        );
        self.roster = vec![(self.primary_id.clone(), "main".into())];
        self.viewport.follow = true;
        self.completion = None;
        self.agent_efforts.clear();
        self.delegate_children.clear();
        self.parent_by_agent.clear();
        self.modal = None;
    }

    pub fn delegate_child(
        &self,
        parent_id: &str,
        _ordinal: usize,
        tool_call_id: Option<&str>,
    ) -> Option<&str> {
        self.delegate_children.get(parent_id).and_then(|children| {
            let id = tool_call_id.filter(|id| !id.is_empty())?;
            children
                .iter()
                .find(|(spawned_id, _)| spawned_id.as_deref() == Some(id))
                .map(|(_, child)| child.as_str())
        })
    }

    // ------------------------------------------------------------------
    // Update
    // ------------------------------------------------------------------

    pub fn update(&mut self, ev: AppEvent) -> Action {
        match ev {
            AppEvent::Tick => {
                self.clear_render_cache();
                self.tick_phase = self.tick_phase.wrapping_add(1);
                if let Some((_, at)) = &self.note
                    && at.elapsed().as_secs() >= 3
                {
                    self.note = None;
                }
                Action::Continue
            }
            AppEvent::Bus(SessionEvent { agent_id, event }) => {
                self.clear_render_cache();
                let items = self.transcripts.entry(agent_id).or_default();
                fold_event(items, &event);
                Action::Continue
            }
            AppEvent::BusLagged(_) => Action::RebuildTranscripts,
            AppEvent::TurnDone(res) => {
                self.clear_render_cache();
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
                TermEvent::Mouse(mouse) => {
                    self.mouse(mouse);
                    Action::Continue
                }
                TermEvent::Paste(text) => {
                    if text.chars().count() > PASTE_BLOCK_THRESHOLD {
                        self.pastes.push(text);
                        self.composer.insert_paste_block(self.pastes.len());
                    } else {
                        self.composer.insert_str(&text);
                    }
                    self.refresh_completion();
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
        let action = match k.code {
            C::Char('c') if m.contains(KeyModifiers::CONTROL) => return Action::Quit,
            C::Char('n') if m.contains(KeyModifiers::CONTROL) => {
                self.cycle_focus(1);
                Action::Continue
            }
            C::Char('p') if m.contains(KeyModifiers::CONTROL) => {
                if let Some(parent) = self.parent_by_agent.get(&self.focused_id).cloned() {
                    self.focused_id = parent;
                    self.viewport.follow = true;
                    self.clear_render_cache();
                }
                Action::Continue
            }
            C::Char('b')
                if m.contains(KeyModifiers::CONTROL) && m.contains(KeyModifiers::SHIFT) =>
            {
                self.cycle_focus(-1);
                Action::Continue
            }
            C::Char('u') if m.contains(KeyModifiers::CONTROL) => {
                self.composer.clear();
                Action::Continue
            }
            C::Esc => {
                if self.completion.take().is_some() {
                    return Action::Continue;
                }
                if let Some(c) = &self.cancel {
                    c.cancel();
                    self.flash("cancelling…");
                }
                Action::Continue
            }
            C::Enter => {
                if m.contains(KeyModifiers::ALT) || m.contains(KeyModifiers::SHIFT) {
                    self.composer.newline();
                    Action::Continue
                } else if self.completion.is_some() {
                    let accepted = self.accept_completion();
                    // If the command completion leaves the cursor at an
                    // empty argument, show that argument's suggestions
                    // immediately. Do not reopen the menu for a completed
                    // argument, since the next Enter should submit it.
                    if accepted
                        && self
                            .composer
                            .text(&self.pastes)
                            .ends_with(char::is_whitespace)
                    {
                        self.refresh_completion();
                    }
                    return Action::Continue;
                } else {
                    self.submit()
                }
            }
            C::Backspace => {
                self.composer.backspace();
                Action::Continue
            }
            C::Delete => {
                self.composer.delete();
                Action::Continue
            }
            C::Left => {
                self.composer.left();
                Action::Continue
            }
            C::Right => {
                self.composer.right();
                Action::Continue
            }
            C::Up if self.completion_move(-1) => return Action::Continue,
            C::Down if self.completion_move(1) => return Action::Continue,
            C::Up => {
                self.composer.up();
                Action::Continue
            }
            C::Down => {
                self.composer.down();
                Action::Continue
            }
            C::PageUp => {
                self.scroll(-10);
                Action::Continue
            }
            C::PageDown => {
                self.scroll(10);
                Action::Continue
            }
            C::Home => {
                self.composer.home();
                Action::Continue
            }
            C::End => {
                self.composer.end();
                Action::Continue
            }
            C::Tab if self.completion.is_some() => {
                self.accept_completion();
                return Action::Continue;
            }
            C::Char(c) if !m.contains(KeyModifiers::CONTROL) => {
                self.composer.insert_char(c);
                Action::Continue
            }
            _ => Action::Continue,
        };
        self.refresh_completion();
        action
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
        if let Err(e) = self.ensure_started() {
            self.flash(&e);
            return Action::Continue;
        }
        let agent_id = self.focused_id.clone();
        self.transcripts
            .entry(agent_id.clone())
            .or_default()
            .push(Item::User(text.clone()));
        self.clear_render_cache();
        self.busy = true;
        self.turn_started = Some(Instant::now());
        self.viewport.follow = true;
        let token = CancellationToken::new();
        self.cancel = Some(token.clone());
        Action::Submit {
            agent_id,
            text,
            token,
        }
    }

    fn run_command(&mut self, line: &str) -> Action {
        self.clear_render_cache();
        use command::Command;
        let parsed = match command::parse(line) {
            Ok(command) => command,
            Err(e) => {
                self.flash(&e.to_string());
                return Action::Continue;
            }
        };
        if self.busy && !command::busy_ok(&parsed) {
            self.flash("busy — try again after the turn");
            return Action::Continue;
        }
        match parsed {
            Command::Quit => Action::Quit,
            Command::Help => {
                let help = command::help_text();
                self.transcripts
                    .entry(self.primary_id.clone())
                    .or_default()
                    .push(Item::Note(help));
                Action::Continue
            }
            Command::Status => {
                let (session, provider, model) = self
                    .primary
                    .as_ref()
                    .map(|agent| {
                        (
                            agent.session_id.clone(),
                            agent.config().provider_id.clone(),
                            agent.config().model.clone(),
                        )
                    })
                    .unwrap_or_else(|| ("welcome".into(), "none".into(), self.model.clone()));
                self.transcripts
                    .entry(self.primary_id.clone())
                    .or_default()
                    .push(Item::Note(format!(
                        "session={} · agents={} · provider={} · model={} · {}",
                        session,
                        self.roster.len(),
                        provider,
                        model,
                        if self.busy { "busy" } else { "idle" },
                    )));
                Action::Continue
            }
            Command::Save => Action::Save,
            Command::Agents => {
                let roster = self
                    .roster
                    .iter()
                    .map(|(id, label)| format!("{label}: {id}"))
                    .collect::<Vec<_>>()
                    .join("\n");
                self.transcripts
                    .entry(self.primary_id.clone())
                    .or_default()
                    .push(Item::Note(if roster.is_empty() {
                        "no agents".into()
                    } else {
                        roster
                    }));
                Action::Continue
            }
            Command::Clear => {
                self.transcripts.insert(self.focused_id.clone(), Vec::new());
                Action::Continue
            }
            Command::Login { kind } => Action::OpenLogin { kind },
            Command::Accounts { provider } => Action::OpenAccounts { provider },
            Command::Rewind { turns } => {
                let Some(primary) = &self.primary else {
                    self.flash("no active session");
                    return Action::Continue;
                };
                match primary.rewind(turns) {
                    Ok(removed) => {
                        self.transcripts.insert(
                            self.primary_id.clone(),
                            items_from_history(&primary.history()),
                        );
                        self.flash(&format!("rewound {removed} messages"));
                    }
                    Err(e) => self.flash(&e.to_string()),
                }
                Action::Continue
            }
            Command::Model { id } => {
                let Some((provider_id, model_id)) = id.split_once('/') else {
                    self.flash("model must use provider/model format");
                    return Action::Continue;
                };
                if provider_id.is_empty() || model_id.is_empty() {
                    self.flash("model must use provider/model format");
                    return Action::Continue;
                }
                let provider_id = provider_id.to_string();
                let model_id = model_id.to_string();
                let known_model = self
                    .manager
                    .lock()
                    .unwrap()
                    .model_info_for(&provider_id, &model_id)
                    .is_some();
                self.model = model_id.clone();
                self.provider_id = provider_id.clone();
                if let Some(primary) = &self.primary {
                    let result = if known_model {
                        match self.manager.lock().unwrap().build(&provider_id) {
                            Ok(provider) => primary.set_provider(provider_id.clone(), provider),
                            Err(e) => Err(AgentError::Trajectory(e)),
                        }
                    } else {
                        Ok(())
                    };
                    match result.and_then(|()| {
                        primary.update_config(|config| config.model = model_id.clone())
                    }) {
                        Ok(()) => self.flash(&format!("model: {provider_id}/{model_id}")),
                        Err(AgentError::Busy) => {
                            self.flash("busy — model changes wait for the turn")
                        }
                        Err(e) => self.flash(&e.to_string()),
                    }
                } else {
                    self.flash(&format!(
                        "model: {provider_id}/{model_id} (will apply to the next session)"
                    ));
                }
                Action::Continue
            }
            Command::Effort { name } => {
                let supported = self
                    .manager
                    .lock()
                    .unwrap()
                    .model_info_for(&self.provider_id, &self.model)
                    .is_some_and(|info| info.effort_modes.iter().any(|mode| mode.name == name));
                if !supported {
                    self.flash(&format!("effort '{name}' is not supported by this model"));
                    return Action::Continue;
                }
                let effort = EffortMode {
                    name: name.clone(),
                    thinking_budget_tokens: None,
                    reasoning_effort: Some(name.clone()),
                };
                if let Some(primary) = &self.primary {
                    match primary.update_config(|config| config.effort = Some(effort.clone())) {
                        Ok(()) => {
                            self.effort = Some(effort);
                            self.flash(&format!("effort: {name}"));
                        }
                        Err(AgentError::Busy) => {
                            self.flash("busy — effort changes wait for the turn")
                        }
                        Err(e) => self.flash(&e.to_string()),
                    }
                } else {
                    self.effort = Some(effort);
                    self.flash("effort applies after the first session starts");
                }
                Action::Continue
            }
            Command::Resume { id } => Action::Resume(id),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        Action, CompletionItem, CompletionState, Item, Model, ToolState, Viewport, fold_event,
        fuzzy_score,
    };
    use crate::tui::composer::Composer;
    use crate::tui::event::AppEvent;
    use crossterm::event::{Event as TermEvent, KeyCode, KeyEvent, KeyModifiers};
    use firmius_core::{AgentEvent, EffortMode, ProviderManager, ToolRegistry};
    use std::sync::Arc;

    #[test]
    fn fuzzy_score_accepts_subsequences_and_rejects_non_matches() {
        assert!(fuzzy_score("rsm", "/resume").is_some());
        assert!(fuzzy_score("son", "sonnet-4").is_some());
        assert!(fuzzy_score("title", "Fix the title bug").is_some());
        assert!(fuzzy_score("xyz", "/resume").is_none());
    }

    #[test]
    fn fuzzy_score_prefers_contiguous_matches() {
        assert!(fuzzy_score("son", "sonnet") < fuzzy_score("son", "season-of-nothing"));
    }

    #[test]
    fn tool_call_delta_creates_running_presentation_before_tool_starts() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "call-1".into(),
                name_delta: "bash".into(),
                args_delta: r#"{"command":"ls"}"#.into(),
            },
        );

        assert!(matches!(
            items.as_slice(),
            [Item::ToolCall {
                stream_id: Some(id),
                name,
                args,
                state: ToolState::Preparing(_),
                ..
            }] if id == "call-1" && name == "bash" && args == r#"{"command":"ls"}"#
        ));
    }

    #[test]
    fn tool_call_start_reconciles_streaming_placeholder_without_duplicate() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "call-1".into(),
                name_delta: "read".into(),
                args_delta: r#"{"path":"src/lib.rs"}"#.into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 0,
                id: "call-1".into(),
                name: "read".into(),
                args: r#"{"path":"src/lib.rs"}"#.into(),
            },
        );

        assert_eq!(items.len(), 1);
        assert!(matches!(items[0], Item::ToolCall { .. }));
    }

    #[test]
    fn viewport_scrolls_up_and_back_down_from_follow_position() {
        let mut viewport = Viewport {
            offset: 0,
            follow: true,
        };
        viewport.scroll(-6);
        assert_eq!(viewport.offset, 6);
        assert!(!viewport.follow);
        viewport.scroll(3);
        assert_eq!(viewport.offset, 3);
        assert!(!viewport.follow);
        viewport.scroll(3);
        assert_eq!(viewport.offset, 0);
        assert!(viewport.follow);
        viewport.scroll(3);
        assert_eq!(viewport.offset, 0);
        assert!(viewport.follow);
    }

    #[test]
    fn welcome_model_accepts_theoretical_model_selection() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
        );

        assert!(matches!(
            model.run_command("/model test-provider/sonnet-4"),
            Action::Continue
        ));
        assert_eq!(model.model, "sonnet-4");
        assert_eq!(model.provider_id, "test-provider");
        assert!(!model.has_agent());
        assert_eq!(model.focus_label(), "welcome");
    }

    #[test]
    fn enter_accepts_completion_without_reopening_the_menu() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
        );
        model.composer.insert_str("/model ");
        model.completion = Some(CompletionState {
            items: vec![CompletionItem {
                insert: "/model codex/gpt-5.6-luna".into(),
                label: "codex/gpt-5.6-luna".into(),
                detail: "model · provider codex".into(),
            }],
            selected: 0,
        });

        assert!(matches!(
            model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
                KeyCode::Enter,
                KeyModifiers::NONE,
            )))),
            Action::Continue
        ));
        assert_eq!(
            model.composer.text(&model.pastes),
            "/model codex/gpt-5.6-luna"
        );
        assert!(model.completion.is_none());
    }

    #[test]
    fn enter_accepting_command_completion_opens_empty_argument_suggestions() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let mut model = Model::new(
            None,
            None,
            "codex".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
        );
        model.composer.insert_str("/mo");
        model.completion = Some(CompletionState {
            items: vec![CompletionItem {
                insert: "/model ".into(),
                label: "/model".into(),
                detail: "select a model".into(),
            }],
            selected: 0,
        });

        assert!(matches!(
            model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
                KeyCode::Enter,
                KeyModifiers::NONE,
            )))),
            Action::Continue
        ));
        assert_eq!(model.composer.text(&model.pastes), "/model ");
        assert!(
            model
                .completion
                .as_ref()
                .is_some_and(|completion| !completion.items.is_empty())
        );
    }

    #[test]
    fn effort_completion_uses_models_dev_values_for_the_selected_model() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let manager = Arc::new(std::sync::Mutex::new(manager));
        let mut model = Model::new(
            None,
            None,
            "codex".into(),
            manager,
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
        );
        model.composer.insert_str("/effort ");
        model.refresh_completion();
        let labels: Vec<_> = model
            .completion
            .as_ref()
            .expect("effort suggestions")
            .items
            .iter()
            .map(|item| item.label.as_str())
            .collect();
        assert_eq!(labels, ["none", "low", "medium", "high", "xhigh", "max"]);

        model.composer.replace_text("/effort");
        model.refresh_completion();
        assert!(model.completion.as_ref().is_some_and(|completion| {
            completion.items.iter().any(|item| item.label == "xhigh")
        }));

        assert!(matches!(
            model.run_command("/effort xhigh"),
            Action::Continue
        ));
        assert_eq!(
            model
                .effort
                .as_ref()
                .and_then(|effort| effort.reasoning_effort.as_deref()),
            Some("xhigh")
        );
    }

    #[test]
    fn effort_completion_uses_the_focused_agents_modes() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "unknown".into(),
            Arc::new(ToolRegistry::default()),
        );
        model.focused_id = "child".into();
        model.agent_efforts.insert(
            "child".into(),
            vec![EffortMode {
                name: "focused-only".into(),
                thinking_budget_tokens: None,
                reasoning_effort: Some("focused-only".into()),
            }],
        );
        model.composer.insert_str("/effort ");
        model.refresh_completion();
        assert_eq!(
            model.completion.as_ref().unwrap().items[0].label,
            "focused-only"
        );
    }

    #[test]
    fn argument_completion_opens_with_an_empty_partial() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let mut model = Model::new(
            None,
            None,
            "codex".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
        );
        model.composer.insert_str("/model ");
        model.refresh_completion();
        assert!(
            model
                .completion
                .as_ref()
                .is_some_and(|completion| { !completion.items.is_empty() })
        );
    }

    #[test]
    fn welcome_first_message_stays_local_without_a_provider() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
        );
        model.composer = Composer::new();
        model.composer.insert_str("hello");

        assert!(matches!(model.submit(), Action::Continue));
        assert!(!model.has_agent());
        assert!(
            model
                .note
                .as_ref()
                .is_some_and(|(note, _)| note.contains("no provider configured"))
        );
    }

    #[test]
    fn ctrl_p_returns_to_parent_agent_when_available() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "model".into(),
            Arc::new(ToolRegistry::default()),
        );
        model.focused_id = "child".into();
        model
            .parent_by_agent
            .insert("child".into(), "parent".into());
        model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
            KeyCode::Char('p'),
            KeyModifiers::CONTROL,
        ))));
        assert_eq!(model.focused_id, "parent");
    }
}
