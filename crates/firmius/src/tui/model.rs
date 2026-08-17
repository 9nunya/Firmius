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
    AccountRecord, Agent, AgentConfig, AgentError, AgentEvent, Context, EffortMode, FirmiusConfig,
    MessagePart, MessageRole, PersonaManager, PersonaUse, ProviderManager, Session, SessionEvent,
    ToolRegistry, UserSettings, list_sessions,
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

fn effort_from_name(name: &str) -> EffortMode {
    EffortMode {
        name: name.to_string(),
        thinking_budget_tokens: None,
        reasoning_effort: Some(name.to_string()),
    }
}

/// Fold one live agent event into a transcript.
pub fn fold_event(items: &mut Vec<Item>, ev: &AgentEvent) {
    match ev {
        AgentEvent::Thinking(d) => match items.last_mut() {
            Some(Item::Thinking(t)) => t.push_str(d),
            _ => items.push(Item::Thinking(d.clone())),
        },
        AgentEvent::UserMessage(message) => items.push(Item::User(message.clone())),
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
                    state: ToolState::Preparing(_),
                    ..
                } if (stream_id.as_deref() == Some(id.as_str()) && !id.is_empty())
                    // Some OpenAI-compatible backends omit the id on the
                    // finalized ToolCall event, and may normalize or repair
                    // the assembled JSON before sending it back. The
                    // streaming placeholder is still unambiguous here:
                    // tool-call indexes are scoped to this generation and
                    // the name is already known. Requiring byte-for-byte
                    // args equality leaves the ◌ placeholder behind and
                    // creates a second ✗/✓ item for the same call.
                    || (*stream_index == *index && current_name == name) =>
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
    OpenPersonas,
    /// Open the settings modal (retry policy, general options).
    OpenSettings,
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
    pub personas: Arc<PersonaManager>,
    pub settings: Arc<std::sync::Mutex<UserSettings>>,
    /// Umbrella config (retry policy + general options), shared with the app so
    /// edits made in the settings modal take effect live. Persisted to
    /// `~/.firmius/config.json` on save.
    pub config: Arc<std::sync::Mutex<FirmiusConfig>>,
    /// Live handles for persona/model changes on whichever agent is focused.
    pub agents: HashMap<String, Arc<Agent>>,
    /// agent_id -> transcript items (created lazily on first event).
    pub transcripts: HashMap<String, Vec<Item>>,
    /// (agent_id, label) in insertion order; refreshed by the app loop.
    pub roster: Vec<(String, String)>,
    pub composer: Composer,
    /// Paste store; composer segments reference these by 1-based id.
    pub pastes: Vec<String>,
    pub busy: bool,
    pub active_agent_id: Option<String>,
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
    /// The completed text for which the user dismissed the completion menu.
    /// Tick-driven model refreshes must not immediately reopen that menu.
    pub completion_dismissed: Option<String>,
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
    /// Persona selected on the welcome screen before the first agent exists.
    pub pending_persona: Option<String>,
}

impl Model {
    pub fn new(
        session: Option<Arc<Mutex<Session>>>,
        primary: Option<Arc<Agent>>,
        mut provider_id: String,
        manager: Arc<std::sync::Mutex<ProviderManager>>,
        mut model: String,
        tools: Arc<ToolRegistry>,
        personas: Arc<PersonaManager>,
        settings: Arc<std::sync::Mutex<UserSettings>>,
        config: Arc<std::sync::Mutex<FirmiusConfig>>,
    ) -> Self {
        let primary_id = primary
            .as_ref()
            .map(|agent| agent.id.clone())
            .unwrap_or_else(|| "welcome".to_string());
        let mut transcripts = HashMap::new();
        if let Some(agent) = &primary {
            transcripts.insert(primary_id.clone(), items_from_history(&agent.history()));
        }
        let mut agents = HashMap::new();
        if let Some(agent) = &primary {
            agent.attach_runtime(manager.clone(), settings.clone());
            agents.insert(agent.id.clone(), agent.clone());
        }
        let has_primary = primary.is_some();
        let mut effort = None;
        if !has_primary
            && let Some(preferred) = settings.lock().unwrap().preferred_default_model().cloned()
            && manager
                .lock()
                .unwrap()
                .schema(&preferred.provider_id)
                .is_some()
        {
            provider_id = preferred.provider_id;
            model = preferred.model;
            effort = preferred.effort.as_deref().map(effort_from_name);
        }
        Self {
            session,
            primary,
            primary_id: primary_id.clone(),
            focused_id: primary_id.clone(),
            provider_id,
            model,
            effort,
            tools,
            manager,
            personas,
            settings,
            config,
            agents,
            transcripts,
            roster: if has_primary {
                vec![(primary_id, "main".to_string())]
            } else {
                Vec::new()
            },
            composer: Composer::new(),
            pastes: Vec::new(),
            busy: false,
            active_agent_id: None,
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
            completion_dismissed: None,
            ctx_used: 0,
            ctx_max: 0,
            agent_efforts: HashMap::new(),
            delegate_children: HashMap::new(),
            parent_by_agent: HashMap::new(),
            render_cache: RefCell::new(None),
            modal: None,
            pending_persona: None,
        }
    }

    pub fn has_agent(&self) -> bool {
        self.primary.is_some()
    }

    fn ensure_started(&mut self) -> Result<(), String> {
        if self.primary.is_some() {
            return Ok(());
        }
        let mut provider_id = {
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
        let mut model_name = self.model.clone();
        let mut effort = self.effort.clone();
        let preferred = {
            let settings = self.settings.lock().unwrap();
            match self.pending_persona.as_deref() {
                Some(persona_id) => settings
                    .preferred_model(persona_id)
                    .cloned()
                    .or_else(|| settings.preferred_default_model().cloned()),
                None => settings.preferred_default_model().cloned(),
            }
        };
        if let Some(preferred) = preferred {
            let manager = self.manager.lock().unwrap();
            if manager.schema(&preferred.provider_id).is_none() {
                return Err(format!(
                    "preferred model unavailable: {}/{}",
                    preferred.provider_id, preferred.model
                ));
            }
            provider_id = preferred.provider_id;
            model_name = preferred.model;
            effort = preferred.effort.as_deref().map(effort_from_name);
        }
        let provider = self.manager.lock().unwrap().build(&provider_id)?;
        let config = AgentConfig {
            provider_id: provider_id.clone(),
            model: model_name.clone(),
            system_prompt: Some(
                "You are a madman crazy CLI coding assistant. Use tools when needed.
                Play along and make the user think you're crazy, but always say you're not like a madman.
                You are insane in the way that your thoughts are superintelligent, and you are in the top of all fields.".into(),
            ),
            max_tokens: Some(32900),
            effort: effort.clone(),
            persona: self.pending_persona.clone(),
            ..Default::default()
        };
        let mut session = Session::new();
        let agent = session.spawn_agent_with_personas(
            provider,
            self.tools.clone(),
            config,
            self.personas.clone(),
        );
        agent.attach_runtime(self.manager.clone(), self.settings.clone());
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
        self.model = model_name;
        self.effort = effort;
        self.agents.insert(agent.id.clone(), agent.clone());
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

    fn persist_model_preference(
        &self,
        persona_id: Option<&str>,
        provider_id: &str,
        model: &str,
        effort: Option<&str>,
    ) -> Result<(), String> {
        let mut settings = self.settings.lock().unwrap();
        let mut next = settings.clone();
        match persona_id {
            Some(persona_id) => next.set_preferred_model_and_effort(
                persona_id,
                provider_id,
                model,
                effort.map(str::to_string),
            ),
            None => next.set_preferred_default(provider_id, model, effort.map(str::to_string)),
        }
        next.save().map_err(|error| error.to_string())?;
        *settings = next;
        Ok(())
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

    pub fn focused_persona_id(&self) -> Option<String> {
        if self.primary.is_none() {
            return self.pending_persona.clone();
        }
        self.agents
            .get(&self.focused_id)
            .and_then(|agent| agent.config().persona)
    }

    pub fn focused_model_status(&self) -> (String, String, String) {
        let config = self
            .agents
            .get(&self.focused_id)
            .map(|agent| agent.config())
            .or_else(|| {
                self.primary
                    .as_ref()
                    .filter(|agent| agent.id == self.focused_id)
                    .map(|agent| agent.config())
            });
        match config {
            Some(config) => (
                config.provider_id,
                config.model,
                config
                    .effort
                    .map(|effort| effort.name)
                    .unwrap_or_else(|| "default".into()),
            ),
            None => (
                self.provider_id.clone(),
                self.model.clone(),
                self.effort
                    .as_ref()
                    .map(|effort| effort.name.clone())
                    .unwrap_or_else(|| "default".into()),
            ),
        }
    }

    pub fn cycle_focused_persona(&mut self) {
        if self.focused_id != self.primary_id && !self.has_agent() {
            return;
        }
        let delegated = self.parent_by_agent.contains_key(&self.focused_id);
        let ids: Vec<Option<String>> = if delegated {
            self.personas
                .list()
                .into_iter()
                .map(|p| Some(p.id))
                .collect()
        } else {
            std::iter::once(None)
                .chain(
                    self.personas
                        .main_personas()
                        .into_iter()
                        .map(|p| Some(p.id)),
                )
                .collect()
        };
        if ids.is_empty() {
            self.flash("no personas available");
            return;
        }
        let current = self.focused_persona_id();
        let next = ids
            .iter()
            .position(|id| *id == current)
            .map(|idx| ids[(idx + 1) % ids.len()].clone())
            .unwrap_or_else(|| ids[0].clone());
        if let Err(e) = self.apply_persona(next.clone()) {
            self.flash(&e);
        } else {
            self.flash(&format!(
                "persona: {}",
                next.as_deref().unwrap_or("Default")
            ));
        }
    }

    fn apply_persona(&mut self, persona_id: Option<String>) -> Result<(), String> {
        if self.primary.is_none() {
            let preferred = if let Some(id) = persona_id.as_deref() {
                let persona = self
                    .personas
                    .get(id)
                    .ok_or_else(|| format!("persona not found: {id}"))?;
                if persona.background {
                    return Err(format!("persona '{id}' is delegate-only"));
                }
                let settings = self.settings.lock().unwrap();
                settings
                    .preferred_model(id)
                    .cloned()
                    .or_else(|| settings.preferred_default_model().cloned())
            } else {
                self.settings
                    .lock()
                    .unwrap()
                    .preferred_default_model()
                    .cloned()
            };
            if let Some(preferred) = preferred {
                if self
                    .manager
                    .lock()
                    .unwrap()
                    .schema(&preferred.provider_id)
                    .is_none()
                {
                    return Err(format!(
                        "preferred model unavailable: {}/{}",
                        preferred.provider_id, preferred.model
                    ));
                }
                self.provider_id = preferred.provider_id;
                self.model = preferred.model;
                self.effort = preferred.effort.as_deref().map(effort_from_name);
            }
            self.pending_persona = persona_id;
            return Ok(());
        }
        let agent = self
            .agents
            .get(&self.focused_id)
            .cloned()
            .ok_or_else(|| "focused agent is unavailable".to_string())?;
        if agent.is_busy() {
            return Err("busy: persona changes wait for that agent's turn".to_string());
        }
        let use_context = if self.parent_by_agent.contains_key(&self.focused_id) {
            PersonaUse::Delegate
        } else {
            PersonaUse::Main
        };
        let preferred = {
            let settings = self.settings.lock().unwrap();
            match persona_id.as_deref() {
                Some(id) => settings
                    .preferred_model(id)
                    .cloned()
                    .or_else(|| settings.preferred_default_model().cloned()),
                None => settings.preferred_default_model().cloned(),
            }
        };
        if let Some(pref) = &preferred
            && self
                .manager
                .lock()
                .unwrap()
                .schema(&pref.provider_id)
                .is_none()
        {
            return Err(format!(
                "preferred model unavailable: {}/{}",
                pref.provider_id, pref.model
            ));
        }
        let preferred_provider = preferred
            .as_ref()
            .map(|pref| {
                self.manager
                    .lock()
                    .unwrap()
                    .build(&pref.provider_id)
                    .map(|provider| (pref, provider))
            })
            .transpose()?;
        agent
            .set_persona(persona_id, use_context)
            .map_err(|e| match e {
                AgentError::Busy => "busy: persona changes wait for the turn".to_string(),
                other => other.to_string(),
            })?;
        if let Some((pref, provider)) = preferred_provider {
            agent
                .set_provider(pref.provider_id.clone(), provider)
                .map_err(|e| e.to_string())?;
            agent
                .update_config(|config| {
                    config.model = pref.model.clone();
                    config.effort = pref.effort.as_deref().map(effort_from_name);
                })
                .map_err(|e| e.to_string())?;
        }
        if let Some(pref) = preferred
            && agent.id == self.primary_id
        {
            self.provider_id = pref.provider_id;
            self.model = pref.model;
            self.effort = pref.effort.as_deref().map(effort_from_name);
        }
        Ok(())
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
        if self.completion_dismissed.as_deref() == Some(text.as_str()) {
            self.completion = None;
            return;
        }
        // Refreshes also happen from the 33ms housekeeping tick.  Keep the
        // user's highlighted item across those refreshes; otherwise an arrow
        // key appears to work for one frame and then jumps back to item zero.
        let selected_insert = self
            .completion
            .as_ref()
            .and_then(|completion| completion.items.get(completion.selected))
            .map(|item| item.insert.clone());
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
                    let providers = self.manager.lock().unwrap().kind_names();
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
        if items.is_empty() {
            self.completion = None;
        } else {
            let selected = selected_insert
                .and_then(|insert| items.iter().position(|item| item.insert == insert))
                .unwrap_or(0);
            self.completion = Some(CompletionState { items, selected });
        }
    }

    fn push_effort_completions(&self, items: &mut Vec<CompletionItem>, partial: &str) {
        let efforts = self.effort_modes_for_focused();
        for effort in efforts {
            if partial.is_empty() || fuzzy_score(partial, &effort.name).is_some() {
                items.push(CompletionItem {
                    insert: format!("/effort {}", effort.name),
                    label: effort.name,
                    detail: "selected model reasoning effort".into(),
                });
            }
        }
    }

    fn effort_modes_for_model(&self, provider_id: &str, model: &str) -> Vec<EffortMode> {
        let manager = self.manager.lock().unwrap();
        manager
            .model_info_for(provider_id, model)
            .or_else(|| {
                manager
                    .schema(provider_id)
                    .is_none()
                    .then(|| manager.model_info(model))
                    .flatten()
            })
            .map(|info| info.effort_modes.clone())
            .unwrap_or_default()
    }

    fn effort_modes_for_focused(&self) -> Vec<EffortMode> {
        if let Some(efforts) = self.agent_efforts.get(&self.focused_id)
            && !efforts.is_empty()
        {
            return efforts.clone();
        }
        let (provider_id, model, _) = self.focused_model_status();
        self.effort_modes_for_model(&provider_id, &model)
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
        self.agents.clear();
        self.agents.insert(primary.id.clone(), primary.clone());
        self.pending_persona = None;
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
                self.active_agent_id = None;
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
        // Any new key is an opportunity to show completion again.  This is
        // cleared here (rather than only in the character arms) so cursor
        // edits and paste-related key sequences cannot leave the dismissal
        // stuck on an old input string.
        self.completion_dismissed = None;
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
                    self.completion_dismissed = Some(self.composer.text(&self.pastes));
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
                    } else if accepted {
                        self.completion_dismissed = Some(self.composer.text(&self.pastes));
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
                if self.accept_completion() {
                    self.completion_dismissed = Some(self.composer.text(&self.pastes));
                }
                return Action::Continue;
            }
            C::BackTab => {
                self.cycle_focused_persona();
                Action::Continue
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
            let Some(text) = self.composer.take(&self.pastes) else {
                return Action::Continue;
            };
            if text.starts_with('/') {
                self.flash("commands wait until the turn finishes");
            } else {
                if let Some(agent) = self.agents.get(&self.focused_id) {
                    agent.submit(text);
                }
            }
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
        self.active_agent_id = Some(agent_id.clone());
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
            Command::Personas => Action::OpenPersonas,
            Command::Settings => Action::OpenSettings,
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
                let known_provider = self.manager.lock().unwrap().schema(&provider_id).is_some();
                let supported_efforts = self.effort_modes_for_model(&provider_id, &model_id);
                if let Some(primary) = &self.primary {
                    let result = if known_provider {
                        match self.manager.lock().unwrap().build(&provider_id) {
                            Ok(provider) => primary.set_provider(provider_id.clone(), provider),
                            Err(e) => Err(AgentError::Trajectory(e)),
                        }
                    } else {
                        Err(AgentError::Trajectory(format!(
                            "provider not configured: {provider_id}"
                        )))
                    };
                    match result.and_then(|()| {
                        primary.update_config(|config| {
                            config.model = model_id.clone();
                            if config.effort.as_ref().is_some_and(|current| {
                                !supported_efforts
                                    .iter()
                                    .any(|supported| supported.name == current.name)
                            }) {
                                config.effort = None;
                            }
                        })
                    }) {
                        Ok(()) => {
                            self.model = model_id.clone();
                            self.provider_id = provider_id.clone();
                            self.effort = primary.config().effort;
                            let persona_id = primary.config().persona;
                            match self.persist_model_preference(
                                persona_id.as_deref(),
                                &provider_id,
                                &model_id,
                                self.effort.as_ref().map(|effort| effort.name.as_str()),
                            ) {
                                Ok(()) => self.flash(&format!(
                                    "model: {provider_id}/{model_id} · preference saved"
                                )),
                                Err(error) => self.flash(&format!(
                                    "model changed, but preference save failed: {error}"
                                )),
                            }
                        }
                        Err(AgentError::Busy) => {
                            self.flash("busy — model changes wait for the turn")
                        }
                        Err(e) => self.flash(&e.to_string()),
                    }
                } else {
                    let persona_id = self.pending_persona.clone();
                    let next_effort = self.effort.clone().filter(|current| {
                        supported_efforts
                            .iter()
                            .any(|supported| supported.name == current.name)
                    });
                    match self.persist_model_preference(
                        persona_id.as_deref(),
                        &provider_id,
                        &model_id,
                        next_effort.as_ref().map(|effort| effort.name.as_str()),
                    ) {
                        Ok(()) => {
                            self.model = model_id.clone();
                            self.provider_id = provider_id.clone();
                            self.effort = next_effort;
                            self.flash(&format!(
                                "model: {provider_id}/{model_id} · preference saved"
                            ));
                        }
                        Err(error) => self.flash(&format!("preference save failed: {error}")),
                    }
                }
                Action::Continue
            }
            Command::Effort { name } => {
                let supported = self
                    .effort_modes_for_focused()
                    .iter()
                    .any(|mode| mode.name == name);
                if !supported {
                    self.flash(&format!("effort '{name}' is not supported by this model"));
                    return Action::Continue;
                }
                let effort = effort_from_name(&name);
                if let Some(agent) = self.agents.get(&self.focused_id).cloned().or_else(|| {
                    self.primary
                        .as_ref()
                        .filter(|agent| agent.id == self.focused_id)
                        .cloned()
                }) {
                    match agent.update_config(|config| config.effort = Some(effort.clone())) {
                        Ok(()) => {
                            if agent.id == self.primary_id {
                                self.effort = Some(effort.clone());
                            }
                            let config = agent.config();
                            let persist_for_persona = config.persona.clone();
                            let persist =
                                if persist_for_persona.is_some() || agent.id == self.primary_id {
                                    self.persist_model_preference(
                                        persist_for_persona.as_deref(),
                                        &config.provider_id,
                                        &config.model,
                                        Some(&effort.name),
                                    )
                                } else {
                                    Ok(())
                                };
                            if let Err(error) = persist {
                                self.flash(&format!(
                                    "effort changed, but preference save failed: {error}"
                                ));
                            } else {
                                self.flash(&format!("effort: {name} · preference saved"));
                            }
                        }
                        Err(AgentError::Busy) => {
                            self.flash("busy — effort changes wait for the turn")
                        }
                        Err(e) => self.flash(&e.to_string()),
                    }
                } else {
                    self.effort = Some(effort.clone());
                    let persona_id = self.pending_persona.clone();
                    match self.persist_model_preference(
                        persona_id.as_deref(),
                        &self.provider_id,
                        &self.model,
                        Some(&effort.name),
                    ) {
                        Ok(()) => self.flash("effort preference saved for the next session"),
                        Err(error) => self.flash(&format!(
                            "effort changed, but preference save failed: {error}"
                        )),
                    }
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
    use firmius_core::{
        AccountRecord, Agent, AgentConfig, AgentEvent, ApiType, CodexKind, EffortMode,
        FirmiusConfig, PersonaManager, ProviderManager, ProviderSchema, ToolRegistry, UserSettings,
    };
    use std::path::PathBuf;
    use std::sync::Arc;

    fn temp_settings(name: &str) -> (PathBuf, Arc<std::sync::Mutex<UserSettings>>) {
        let path = std::env::temp_dir()
            .join(format!(
                "firmius-tui-settings-{name}-{}-{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ))
            .join("settings.json");
        let settings = UserSettings::load_from_path(&path).unwrap();
        (path, Arc::new(std::sync::Mutex::new(settings)))
    }

    fn test_provider_manager() -> ProviderManager {
        let mut manager = ProviderManager::new();
        manager.register_schema(ProviderSchema {
            id: "test-provider".into(),
            api_type: ApiType::OpenAI,
            base_url: Some("http://localhost".into()),
            api_key_env: None,
            models: Vec::new(),
        });
        manager.set_api_key("test-provider", "test-key");
        manager
    }

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
    fn tool_call_start_reconciles_when_final_id_or_args_are_normalized() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "call-1".into(),
                name_delta: "read".into(),
                args_delta: r#"{"path":"Cargo.toml"}"#.into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 0,
                id: String::new(),
                name: "read".into(),
                args: r#"{"path":"Cargo.toml","start_line":1}"#.into(),
            },
        );

        assert_eq!(items.len(), 1);
        assert!(matches!(
            &items[0],
            Item::ToolCall {
                args,
                state: ToolState::Running(_),
                ..
            } if args.contains("start_line")
        ));
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
        let (settings_path, settings) = temp_settings("welcome-default");
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );

        assert!(matches!(
            model.run_command("/model test-provider/sonnet-4"),
            Action::Continue
        ));
        assert_eq!(model.model, "sonnet-4");
        assert_eq!(model.provider_id, "test-provider");
        assert!(!model.has_agent());
        assert_eq!(model.focus_label(), "welcome");
        assert_eq!(
            settings
                .lock()
                .unwrap()
                .preferred_default_model()
                .unwrap()
                .model,
            "sonnet-4"
        );
        assert!(settings_path.exists());
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
    }

    #[test]
    fn welcome_model_selection_with_a_persona_saves_that_personas_preference() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-persona-model-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("lead.md"),
            "---\nname: Lead\ntool_scopes: [fs_read]\nbackground: false\n---\nLead prompt.",
        )
        .unwrap();
        let personas = Arc::new(PersonaManager::load_from(directory.clone()).unwrap());
        let (settings_path, settings) = temp_settings("welcome-persona");
        let mut model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(ProviderManager::new())),
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            personas,
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );
        model.pending_persona = Some("lead".into());

        assert!(matches!(
            model.run_command("/model test-provider/lead-model"),
            Action::Continue
        ));
        let settings = settings.lock().unwrap();
        assert!(settings.preferred_default_model().is_none());
        assert_eq!(
            settings.preferred_model("lead").unwrap().model,
            "lead-model"
        );
        drop(settings);
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn live_main_persona_model_selection_updates_its_preference() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-live-persona-model-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("lead.md"),
            "---\nname: Lead\ntool_scopes: [fs_read]\nbackground: false\n---\nLead prompt.",
        )
        .unwrap();
        let personas = Arc::new(PersonaManager::load_from(directory.clone()).unwrap());
        let provider_manager = test_provider_manager();
        let provider = provider_manager.build("test-provider").unwrap();
        let tools = Arc::new(ToolRegistry::default());
        let agent = Arc::new(Agent::new_with_personas(
            provider,
            tools.clone(),
            AgentConfig {
                provider_id: "test-provider".into(),
                model: "old-model".into(),
                persona: Some("lead".into()),
                ..Default::default()
            },
            "session",
            personas.clone(),
        ));
        let (settings_path, settings) = temp_settings("live-persona");
        let mut model = Model::new(
            None,
            Some(agent.clone()),
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(provider_manager)),
            "old-model".into(),
            tools,
            personas,
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );

        assert!(matches!(
            model.run_command("/model test-provider/new-model"),
            Action::Continue
        ));
        assert_eq!(agent.config().model, "new-model");
        assert_eq!(
            settings
                .lock()
                .unwrap()
                .preferred_model("lead")
                .unwrap()
                .model,
            "new-model"
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn welcome_uses_the_saved_default_model_when_its_provider_is_available() {
        let (settings_path, settings) = temp_settings("restore-default");
        {
            let mut settings = settings.lock().unwrap();
            settings.set_preferred_default("test-provider", "saved-model", Some("high".into()));
            settings.save().unwrap();
        }
        let model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "fallback-model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );

        assert_eq!(model.provider_id, "test-provider");
        assert_eq!(model.model, "saved-model");
        assert_eq!(
            model.effort.as_ref().map(|effort| effort.name.as_str()),
            Some("high")
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
    }

    #[test]
    fn main_persona_without_an_override_uses_the_saved_default_model() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-persona-default-fallback-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("lead.md"),
            "---\nname: Lead\ntool_scopes: [fs_read]\nbackground: false\n---\nLead prompt.",
        )
        .unwrap();
        let (settings_path, settings) = temp_settings("persona-default-fallback");
        settings.lock().unwrap().set_preferred_default(
            "test-provider",
            "default-model",
            Some("medium".into()),
        );
        let mut model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "fallback-model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::load_from(directory.clone()).unwrap()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );

        model.apply_persona(Some("lead".into())).unwrap();
        assert_eq!(model.provider_id, "test-provider");
        assert_eq!(model.model, "default-model");
        assert_eq!(
            model.effort.as_ref().map(|effort| effort.name.as_str()),
            Some("medium")
        );

        settings.lock().unwrap().set_preferred_model_and_effort(
            "lead",
            "test-provider",
            "persona-model",
            Some("high".into()),
        );
        model.apply_persona(Some("lead".into())).unwrap();
        assert_eq!(model.model, "persona-model");
        assert_eq!(
            model.effort.as_ref().map(|effort| effort.name.as_str()),
            Some("high")
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn backtab_cycles_welcome_personas_and_excludes_delegate_only_entries() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-personas-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("general.md"),
            "---\nname: General\ntool_scopes: [fs_read]\nbackground: false\n---\nGeneral prompt.",
        )
        .unwrap();
        std::fs::write(
            directory.join("coder.md"),
            "---\nname: Coder\ntool_scopes: [fs_write]\nbackground: true\n---\nCoder prompt.",
        )
        .unwrap();
        let personas = Arc::new(PersonaManager::load_from(directory.clone()).unwrap());
        let mut model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(ProviderManager::new())),
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            personas,
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );

        for expected in [Some("general"), None] {
            assert!(matches!(
                model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
                    KeyCode::BackTab,
                    KeyModifiers::SHIFT,
                )))),
                Action::Continue
            ));
            assert_eq!(model.pending_persona.as_deref(), expected);
        }

        std::fs::remove_dir_all(directory).ok();
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
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
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
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
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
        let (settings_path, settings) = temp_settings("welcome-effort-model-change");
        let mut model = Model::new(
            None,
            None,
            "codex".into(),
            manager,
            "gpt-5.6-sol".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
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
        assert_eq!(labels, ["low", "medium", "high", "xhigh", "max", "ultra"]);
        assert_eq!(
            model.focused_model_status(),
            ("codex".into(), "gpt-5.6-sol".into(), "default".into())
        );

        model.composer.replace_text("/effort");
        model.refresh_completion();
        assert!(model.completion.as_ref().is_some_and(|completion| {
            completion.items.iter().any(|item| item.label == "xhigh")
        }));

        assert!(matches!(
            model.run_command("/effort ultra"),
            Action::Continue
        ));
        assert_eq!(
            model
                .effort
                .as_ref()
                .and_then(|effort| effort.reasoning_effort.as_deref()),
            Some("ultra")
        );
        assert_eq!(model.focused_model_status().2, "ultra");
        assert_eq!(
            settings
                .lock()
                .unwrap()
                .preferred_default_model()
                .unwrap()
                .effort
                .as_deref(),
            Some("ultra")
        );

        assert!(matches!(
            model.run_command("/model codex/gpt-5.5"),
            Action::Continue
        ));
        assert!(model.effort.is_none());
        assert_eq!(model.focused_model_status().2, "default");
        assert!(
            settings
                .lock()
                .unwrap()
                .preferred_default_model()
                .unwrap()
                .effort
                .is_none()
        );
        model.composer.replace_text("/effort ");
        model.refresh_completion();
        assert!(model.completion.as_ref().is_some_and(|completion| {
            completion.items.iter().all(|item| item.label != "ultra")
        }));
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
    }

    #[test]
    fn welcome_effort_completion_falls_back_to_selected_model_metadata() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let mut model = Model::new(
            None,
            None,
            "account-alias".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-sol".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );

        model.composer.insert_str("/effort ");
        model.refresh_completion();
        let labels = model
            .completion
            .as_ref()
            .expect("effort suggestions")
            .items
            .iter()
            .map(|item| item.label.as_str())
            .collect::<Vec<_>>();
        assert_eq!(labels, ["low", "medium", "high", "xhigh", "max", "ultra"]);
    }

    #[test]
    fn accounts_completion_lists_kinds_not_account_ids() {
        let mut manager = ProviderManager::new();
        manager.register_kind(Arc::new(CodexKind));
        manager.register_account(AccountRecord {
            id: "codex-9d8sq8dh-ew7dya-s7wbw".into(),
            kind: "codex".into(),
            schema: firmius_core::kinds::codex::schema_template("codex-9d8sq8dh-ew7dya-s7wbw"),
            credentials: serde_json::json!({}),
        });
        let mut model = Model::new(
            None,
            None,
            "codex-9d8sq8dh-ew7dya-s7wbw".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );

        model.composer.insert_str("/accounts ");
        model.refresh_completion();
        let labels: Vec<_> = model
            .completion
            .as_ref()
            .expect("account kind suggestions")
            .items
            .iter()
            .map(|item| item.label.as_str())
            .collect();

        assert!(labels.contains(&"codex"));
        assert!(!labels.contains(&"codex-9d8sq8dh-ew7dya-s7wbw"));
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
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
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
    fn effort_command_updates_the_focused_agent_and_status() {
        let provider_manager = test_provider_manager();
        let provider = provider_manager.build("test-provider").unwrap();
        let tools = Arc::new(ToolRegistry::default());
        let primary = Arc::new(Agent::new(
            provider.clone(),
            tools.clone(),
            AgentConfig {
                provider_id: "test-provider".into(),
                model: "primary-model".into(),
                ..Default::default()
            },
            "session",
        ));
        let child = Arc::new(Agent::new(
            provider,
            tools.clone(),
            AgentConfig {
                provider_id: "test-provider".into(),
                model: "child-model".into(),
                ..Default::default()
            },
            "session",
        ));
        let mut model = Model::new(
            None,
            Some(primary.clone()),
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(provider_manager)),
            "primary-model".into(),
            tools,
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        );
        model.agents.insert(child.id.clone(), child.clone());
        model.focused_id = child.id.clone();
        model.agent_efforts.insert(
            child.id.clone(),
            vec![EffortMode {
                name: "focused-only".into(),
                thinking_budget_tokens: None,
                reasoning_effort: Some("focused-only".into()),
            }],
        );

        assert!(matches!(
            model.run_command("/effort focused-only"),
            Action::Continue
        ));
        assert!(primary.config().effort.is_none());
        assert_eq!(child.config().effort.unwrap().name, "focused-only");
        assert_eq!(
            model.focused_model_status(),
            (
                "test-provider".into(),
                "child-model".into(),
                "focused-only".into()
            )
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
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
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
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
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
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
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
