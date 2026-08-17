//! Modal surfaces: dialogs rendered above the composer that intercept all
//! keys while open. The [`ModalSurface`] trait is the extension point —
//! pickers, confirmations, and flows all implement it. Two impls today:
//! [`WizardModal`] (drives any core [`SetupWizard`]) and [`KindPickerModal`]
//! (chooses which wizard to run). Shared pieces: chrome, hints, and the
//! [`ListInput`] selection primitive.

use async_trait::async_trait;
use crossterm::event::{KeyCode, KeyEvent};
use firmius_core::{
    AccountRecord, Outcome, Persona, ProviderManager, QuotaAuth, QuotaDescriptor, QuotaSnapshot,
    QuotaSource, SetupWizard, Step, UserSettings,
};
use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Paragraph};
use std::process::Command as ProcessCommand;
use std::sync::Arc;

use super::composer::Composer;
use super::model::Action;
use super::present;
use super::style;

/// What a modal did with a key.
pub enum ModalAction {
    /// Key consumed; keep the modal open.
    Stay,
    /// Dismiss without a result.
    Close,
    /// Dismiss and hand the app loop a side effect.
    Emit(Action),
}

/// One interactive dialog. `key` is async because wizards are: a step may
/// validate or advance through an async state machine.
#[async_trait]
pub trait ModalSurface: Send {
    fn title(&self) -> String;
    /// Desired height for a given width; the app loop caps and positions.
    fn height_hint(&self, width: u16) -> u16;
    fn width_hint(&self, available: u16) -> u16 {
        available.min(64).max(20)
    }
    fn render(&self, area: Rect, frame: &mut Frame);
    async fn key(&mut self, k: KeyEvent) -> ModalAction;
    async fn tick(&mut self) -> ModalAction {
        ModalAction::Stay
    }
    /// Accept bracketed paste while the modal owns input. Most modals do not
    /// take text, so their default behavior is to ignore it.
    fn paste(&mut self, _text: &str) {}
    /// Terminal cursor position (absolute), when this surface takes input.
    fn cursor(&self, area: Rect) -> Option<(u16, u16)>;
}

/// Shared chrome: rounded bordered block with a title; returns the inner area.
pub fn draw_chrome(title: &str, area: Rect, frame: &mut Frame) -> Rect {
    let block = Block::bordered()
        .border_type(BorderType::Rounded)
        .border_style(style::border())
        .title(format!(" {title} "));
    let inner = block.inner(area);
    frame.render_widget(block, area);
    inner
}

pub fn hint_line(text: &str) -> Line<'static> {
    Line::styled(text.to_string(), style::dim())
}

/// Selection primitive: `(value, label)` options plus a selected index.
/// Used by wizard `Select` steps and plain picker modals alike.
#[derive(Debug, Default)]
pub struct ListInput {
    pub options: Vec<(String, String)>,
    pub selected: usize,
}

impl ListInput {
    pub fn new(options: Vec<(String, String)>) -> Self {
        Self {
            options,
            selected: 0,
        }
    }

    pub fn move_selection(&mut self, dir: i32) {
        if self.options.is_empty() {
            return;
        }
        let n = self.options.len() as i32;
        self.selected = (self.selected as i32 + dir).rem_euclid(n) as usize;
    }

    pub fn current_value(&self) -> Option<String> {
        self.options.get(self.selected).map(|(v, _)| v.clone())
    }

    pub fn render_lines(&self) -> Vec<Line<'static>> {
        self.options
            .iter()
            .enumerate()
            .map(|(i, (_, label))| {
                let (marker, st) = if i == self.selected {
                    ("▸ ", style::user())
                } else {
                    ("  ", style::bar())
                };
                Line::from(vec![
                    Span::styled(marker, st),
                    Span::styled(label.clone(), st),
                ])
            })
            .collect()
    }
}

fn list_for_step(step: &Step) -> ListInput {
    match step {
        Step::Select { options, .. } => ListInput::new(
            options
                .iter()
                .map(|o| (o.value.clone(), o.label.clone()))
                .collect(),
        ),
        Step::Prompt { .. } | Step::OpenUrl { .. } => ListInput::default(),
    }
}

// ---------------------------------------------------------------------------
// WizardModal — renders any core SetupWizard
// ---------------------------------------------------------------------------

pub struct WizardModal {
    kind_name: String,
    kind_label: String,
    wizard: Box<dyn SetupWizard>,
    step: Step,
    input: Composer,
    list: ListInput,
    error: Option<String>,
}

impl WizardModal {
    pub async fn start(
        kind_name: String,
        kind_label: String,
        mut wizard: Box<dyn SetupWizard>,
    ) -> Self {
        let step = wizard.start().await;
        let list = list_for_step(&step);
        let modal = Self {
            kind_name,
            kind_label,
            wizard,
            step,
            input: Composer::new(),
            list,
            error: None,
        };
        modal
    }

    fn launch_open_url(&self) {
        let Step::OpenUrl { url, .. } = &self.step else {
            return;
        };
        #[cfg(target_os = "macos")]
        let command = "open";
        #[cfg(target_os = "linux")]
        let command = "xdg-open";
        #[cfg(target_os = "windows")]
        let command = "cmd";
        #[cfg(target_os = "windows")]
        let _ = ProcessCommand::new(command)
            .args(["/C", "start", url])
            .spawn();
        #[cfg(not(target_os = "windows"))]
        let _ = ProcessCommand::new(command).arg(url).spawn();
    }

    fn sync_for_step(&mut self) {
        self.list = list_for_step(&self.step);
        self.input.clear();
    }

    async fn submit(&mut self, answer: String) -> ModalAction {
        match self.wizard.answer(answer).await {
            Ok(Outcome::Next(step)) => {
                self.step = step;
                self.error = None;
                self.sync_for_step();
                ModalAction::Stay
            }
            Ok(Outcome::Done {
                schema,
                credentials,
            }) => ModalAction::Emit(Action::RegisterAccount {
                record: AccountRecord {
                    id: schema.id.clone(),
                    kind: self.kind_name.clone(),
                    schema,
                    credentials,
                },
            }),
            Err(e) => {
                self.error = Some(e.to_string());
                ModalAction::Stay
            }
        }
    }
}

#[async_trait]
impl ModalSurface for WizardModal {
    fn title(&self) -> String {
        format!("{} — setup", self.kind_label)
    }

    fn height_hint(&self, _width: u16) -> u16 {
        let base = match &self.step {
            Step::Select { options, .. } => 4 + options.len() as u16,
            Step::Prompt { .. } | Step::OpenUrl { .. } => 6,
        };
        base + if self.error.is_some() { 1 } else { 0 }
    }

    fn render(&self, area: Rect, frame: &mut Frame) {
        let inner = draw_chrome(&self.title(), area, frame);
        let mut lines: Vec<Line<'static>> = Vec::new();
        match &self.step {
            Step::Prompt { label, secret } => {
                lines.push(Line::styled(label.clone(), style::bar()));
                for text in self.input.lines(&[]) {
                    let shown = if *secret {
                        "•".repeat(text.chars().count())
                    } else {
                        text
                    };
                    lines.push(Line::styled(shown, Style::default()));
                }
            }
            Step::Select { label, .. } => {
                lines.push(Line::styled(label.clone(), style::bar()));
                lines.extend(self.list.render_lines());
            }
            Step::OpenUrl { label, .. } => {
                lines.push(Line::styled(label.clone(), style::bar()));
                lines.push(hint_line("complete the login in your browser · esc cancel"));
            }
        }
        if let Some(err) = &self.error {
            lines.push(Line::styled(err.clone(), style::tool_err()));
        }
        lines.push(hint_line("enter confirm · esc cancel"));
        frame.render_widget(Paragraph::new(lines), inner);
    }

    async fn key(&mut self, k: KeyEvent) -> ModalAction {
        self.error = None;
        match k.code {
            KeyCode::Esc => ModalAction::Close,
            KeyCode::Enter => match &self.step {
                Step::Select { .. } => {
                    let Some(value) = self.list.current_value() else {
                        return ModalAction::Stay;
                    };
                    self.submit(value).await
                }
                Step::Prompt { .. } => {
                    let text = self.input.take(&[]).unwrap_or_default();
                    self.submit(text).await
                }
                Step::OpenUrl { .. } => {
                    self.launch_open_url();
                    ModalAction::Stay
                }
            },
            KeyCode::Up if matches!(self.step, Step::Select { .. }) => {
                self.list.move_selection(-1);
                ModalAction::Stay
            }
            KeyCode::Down if matches!(self.step, Step::Select { .. }) => {
                self.list.move_selection(1);
                ModalAction::Stay
            }
            KeyCode::Backspace => {
                self.input.backspace();
                ModalAction::Stay
            }
            KeyCode::Delete => {
                self.input.delete();
                ModalAction::Stay
            }
            KeyCode::Left => {
                self.input.left();
                ModalAction::Stay
            }
            KeyCode::Right => {
                self.input.right();
                ModalAction::Stay
            }
            KeyCode::Char(c) => {
                self.input.insert_char(c);
                ModalAction::Stay
            }
            _ => ModalAction::Stay,
        }
    }

    async fn tick(&mut self) -> ModalAction {
        if !matches!(self.step, Step::OpenUrl { .. }) {
            return ModalAction::Stay;
        }
        match self.wizard.poll().await {
            Ok(Some(Outcome::Next(step))) => {
                self.step = step;
                self.error = None;
                self.sync_for_step();
                ModalAction::Stay
            }
            Ok(Some(Outcome::Done {
                schema,
                credentials,
            })) => ModalAction::Emit(Action::RegisterAccount {
                record: AccountRecord {
                    id: schema.id.clone(),
                    kind: self.kind_name.clone(),
                    schema,
                    credentials,
                },
            }),
            Ok(None) => ModalAction::Stay,
            Err(error) => {
                self.error = Some(error.to_string());
                ModalAction::Stay
            }
        }
    }

    fn paste(&mut self, text: &str) {
        if matches!(self.step, Step::Prompt { .. }) {
            self.input.insert_str(text);
        }
    }

    fn cursor(&self, area: Rect) -> Option<(u16, u16)> {
        if !matches!(self.step, Step::Prompt { .. }) {
            return None;
        }
        let inner = Block::bordered().inner(area);
        let (row, col) = self.input.cursor_pos(&[]);
        // Layout matches render(): label line, then the input lines.
        Some((inner.x + col as u16, inner.y + 1 + row as u16))
    }
}

// ---------------------------------------------------------------------------
// KindPickerModal — bare /login: choose which kind to set up
// ---------------------------------------------------------------------------

pub struct KindPickerModal {
    list: ListInput,
}

pub struct PersonasModal {
    personas: Vec<Persona>,
    settings: Arc<std::sync::Mutex<UserSettings>>,
    manager: Arc<std::sync::Mutex<ProviderManager>>,
    selected: usize,
    picker: bool,
    query: String,
    model_selected: usize,
    pending_model: Option<(String, String)>,
    effort_selected: usize,
    last_error: Option<String>,
}

impl PersonasModal {
    pub fn new(
        personas: Vec<Persona>,
        settings: Arc<std::sync::Mutex<UserSettings>>,
        manager: Arc<std::sync::Mutex<ProviderManager>>,
    ) -> Self {
        Self {
            personas,
            settings,
            manager,
            selected: 0,
            picker: false,
            query: String::new(),
            model_selected: 0,
            pending_model: None,
            effort_selected: 0,
            last_error: None,
        }
    }

    fn model_options(&self) -> Vec<(Option<(String, String)>, String)> {
        let mut rows = vec![(None, "No preference".to_string())];
        rows.extend(
            self.manager
                .lock()
                .unwrap()
                .model_choices()
                .into_iter()
                .map(|(p, m)| (Some((p.clone(), m.clone())), format!("{p}/{m}"))),
        );
        let q = self.query.to_lowercase();
        if q.is_empty() {
            rows
        } else {
            rows.into_iter()
                .filter(|(_, label)| label.to_lowercase().contains(&q))
                .collect()
        }
    }

    fn current_preference_index(&self) -> usize {
        let Some(persona) = self.personas.get(self.selected) else {
            return 0;
        };
        let preferred = self
            .settings
            .lock()
            .unwrap()
            .preferred_model(&persona.id)
            .cloned();
        self.model_options()
            .iter()
            .position(|(choice, _)| {
                choice.as_ref().is_some_and(|(provider, model)| {
                    preferred.as_ref().is_some_and(|preferred| {
                        preferred.provider_id == *provider && preferred.model == *model
                    })
                })
            })
            .unwrap_or(0)
    }

    fn effort_options(&self, provider: &str, model: &str) -> Vec<Option<String>> {
        let mut options = vec![None];
        if let Some(info) = self.manager.lock().unwrap().model_info_for(provider, model) {
            options.extend(
                info.effort_modes
                    .iter()
                    .map(|effort| Some(effort.name.clone())),
            );
        }
        options
    }

    fn current_effort_index(&self, provider: &str, model: &str) -> usize {
        let Some(persona) = self.personas.get(self.selected) else {
            return 0;
        };
        let preferred = self
            .settings
            .lock()
            .unwrap()
            .preferred_model(&persona.id)
            .cloned();
        let Some(preferred) = preferred
            .filter(|preferred| preferred.provider_id == provider && preferred.model == model)
        else {
            return 0;
        };
        self.effort_options(provider, model)
            .iter()
            .position(|effort| *effort == preferred.effort)
            .unwrap_or(0)
    }

    fn save_preference(
        &mut self,
        selection: Option<(String, String, Option<String>)>,
    ) -> Result<(), String> {
        let persona = self
            .personas
            .get(self.selected)
            .ok_or_else(|| "no persona selected".to_string())?;
        let mut settings = self.settings.lock().unwrap();
        let mut next = settings.clone();
        match selection {
            Some((provider, model, effort)) => {
                next.set_preferred_model_and_effort(&persona.id, provider, model, effort)
            }
            None => {
                next.clear_preferred_model(&persona.id);
            }
        }
        next.save().map_err(|error| error.to_string())?;
        *settings = next;
        Ok(())
    }
}

#[async_trait]
impl ModalSurface for PersonasModal {
    fn title(&self) -> String {
        if self.pending_model.is_some() {
            "Persona effort".into()
        } else if self.picker {
            "Persona model".into()
        } else {
            "Personas".into()
        }
    }
    fn width_hint(&self, available: u16) -> u16 {
        available.min(96)
    }
    fn height_hint(&self, _width: u16) -> u16 {
        4 + if let Some((provider, model)) = &self.pending_model {
            self.effort_options(provider, model).len().min(12) as u16
        } else if self.picker {
            self.model_options().len().min(12) as u16
        } else {
            self.personas.len().min(14) as u16
        } + u16::from(self.picker && self.last_error.is_some())
    }
    fn render(&self, area: Rect, frame: &mut Frame) {
        let inner = draw_chrome(&self.title(), area, frame);
        let mut lines = Vec::new();
        if let Some((provider, model)) = &self.pending_model {
            let persona = self
                .personas
                .get(self.selected)
                .map(|p| p.name.as_str())
                .unwrap_or("persona");
            lines.push(Line::styled(
                format!("{persona}  {provider}/{model}"),
                style::bar(),
            ));
            let options = self.effort_options(provider, model);
            let start = self.effort_selected.saturating_sub(11);
            for (index, effort) in options.iter().enumerate().skip(start).take(12) {
                let label = effort.as_deref().unwrap_or("Default");
                lines.push(Line::styled(
                    format!(
                        "{}{}",
                        if index == self.effort_selected {
                            "▸ "
                        } else {
                            "  "
                        },
                        label
                    ),
                    if index == self.effort_selected {
                        style::user()
                    } else {
                        style::bar()
                    },
                ));
            }
            lines.push(hint_line("up/down choose · enter save · esc back"));
        } else if self.picker {
            let persona = self
                .personas
                .get(self.selected)
                .map(|p| p.name.as_str())
                .unwrap_or("persona");
            lines.push(Line::styled(
                format!("{persona}  search: {}", self.query),
                style::bar(),
            ));
            let options = self.model_options();
            let start = self.model_selected.saturating_sub(11);
            for (i, (_, label)) in options.iter().enumerate().skip(start).take(12) {
                let st = if i == self.model_selected {
                    style::user()
                } else {
                    style::bar()
                };
                lines.push(Line::styled(
                    format!(
                        "{}{}",
                        if i == self.model_selected {
                            "▸ "
                        } else {
                            "  "
                        },
                        label
                    ),
                    st,
                ));
            }
            lines.push(hint_line("type search · enter effort · esc back"));
        } else {
            let settings = self.settings.lock().unwrap();
            let start = self.selected.saturating_sub(13);
            for (i, p) in self.personas.iter().enumerate().skip(start).take(14) {
                let st = if i == self.selected {
                    style::user()
                } else {
                    style::bar()
                };
                let mode = if p.background {
                    "delegate-only"
                } else {
                    "main"
                };
                let scopes = if p.tool_scopes.is_empty() {
                    "none".to_string()
                } else {
                    p.tool_scopes.join(",")
                };
                let pref = settings
                    .preferred_model(&p.id)
                    .map(|m| {
                        format!(
                            "{}/{} · effort {}",
                            m.provider_id,
                            m.model,
                            m.effort.as_deref().unwrap_or("Default")
                        )
                    })
                    .unwrap_or_else(|| "No preference".into());
                lines.push(Line::styled(
                    format!(
                        "{}{} · {mode} · scopes: {scopes} · {pref}",
                        if i == self.selected { "▸ " } else { "  " },
                        p.name
                    ),
                    st,
                ));
            }
            lines.push(hint_line("up/down choose · enter model · esc close"));
        }
        if let Some(error) = &self.last_error {
            lines.push(Line::styled(error.clone(), style::tool_err()));
        }
        frame.render_widget(Paragraph::new(lines), inner);
    }
    async fn key(&mut self, k: KeyEvent) -> ModalAction {
        match k.code {
            KeyCode::Esc if self.pending_model.is_some() => {
                self.pending_model = None;
                self.effort_selected = 0;
                ModalAction::Stay
            }
            KeyCode::Esc if self.picker => {
                self.picker = false;
                self.pending_model = None;
                self.query.clear();
                ModalAction::Stay
            }
            KeyCode::Esc => ModalAction::Close,
            KeyCode::Up => {
                if self.pending_model.is_some() {
                    self.effort_selected = self.effort_selected.saturating_sub(1);
                } else if self.picker {
                    self.model_selected = self.model_selected.saturating_sub(1);
                } else if self.selected > 0 {
                    self.selected -= 1;
                }
                ModalAction::Stay
            }
            KeyCode::Down => {
                if let Some((provider, model)) = &self.pending_model {
                    let len = self.effort_options(provider, model).len();
                    self.effort_selected = (self.effort_selected + 1).min(len.saturating_sub(1));
                } else if self.picker {
                    let len = self.model_options().len();
                    self.model_selected = (self.model_selected + 1).min(len.saturating_sub(1));
                } else {
                    self.selected = (self.selected + 1).min(self.personas.len().saturating_sub(1));
                }
                ModalAction::Stay
            }
            KeyCode::Enter if self.pending_model.is_some() => {
                let (provider, model) = self.pending_model.clone().unwrap();
                let effort = self
                    .effort_options(&provider, &model)
                    .get(self.effort_selected)
                    .cloned()
                    .unwrap_or(None);
                if let Err(error) = self.save_preference(Some((provider, model, effort))) {
                    self.last_error = Some(format!("save failed: {error}"));
                } else {
                    self.picker = false;
                    self.pending_model = None;
                    self.query.clear();
                    self.last_error = None;
                }
                ModalAction::Stay
            }
            KeyCode::Enter if self.picker => {
                let options = self.model_options();
                let Some((option, _)) = options.get(self.model_selected).cloned() else {
                    self.last_error = Some("no models match that search".to_string());
                    return ModalAction::Stay;
                };
                if let Some((provider, model)) = option {
                    self.effort_selected = self.current_effort_index(&provider, &model);
                    self.pending_model = Some((provider, model));
                } else if let Err(error) = self.save_preference(None) {
                    self.last_error = Some(format!("save failed: {error}"));
                } else {
                    self.picker = false;
                    self.query.clear();
                    self.last_error = None;
                }
                ModalAction::Stay
            }
            KeyCode::Enter if !self.personas.is_empty() => {
                self.picker = true;
                self.pending_model = None;
                self.query.clear();
                self.model_selected = self.current_preference_index();
                ModalAction::Stay
            }
            KeyCode::Backspace if self.picker && self.pending_model.is_none() => {
                self.query.pop();
                self.model_selected = 0;
                ModalAction::Stay
            }
            KeyCode::Char(c) if self.picker && self.pending_model.is_none() => {
                self.query.push(c);
                self.model_selected = 0;
                ModalAction::Stay
            }
            _ => ModalAction::Stay,
        }
    }
    fn paste(&mut self, text: &str) {
        if self.picker && self.pending_model.is_none() {
            self.query.push_str(text);
            self.model_selected = 0;
        }
    }
    fn cursor(&self, _area: Rect) -> Option<(u16, u16)> {
        None
    }
}

pub struct AccountRow {
    pub id: String,
    pub kind: String,
    pub descriptor: Option<QuotaDescriptor>,
    pub source: Option<Arc<dyn QuotaSource>>,
    pub snapshot: Option<QuotaSnapshot>,
    pub error: Option<String>,
}

pub struct AccountsModal {
    provider: String,
    rows: Vec<AccountRow>,
    selected: usize,
    expanded: Vec<bool>,
}

impl AccountsModal {
    pub fn new(provider: String, rows: Vec<AccountRow>) -> Self {
        let expanded = vec![false; rows.len()];
        Self {
            provider,
            rows,
            selected: 0,
            expanded,
        }
    }

    fn move_selection(&mut self, delta: i32) {
        if self.rows.is_empty() {
            return;
        }
        let len = self.rows.len() as i32;
        self.selected = (self.selected as i32 + delta).rem_euclid(len) as usize;
    }

    fn meter_lines(row: &AccountRow) -> Vec<Line<'static>> {
        let Some(snapshot) = &row.snapshot else {
            let unavailable = row.descriptor.as_ref().map(|descriptor| {
                let auth = match &descriptor.auth {
                    QuotaAuth::ApiKey => "API key",
                    QuotaAuth::WebSession => "web session",
                    QuotaAuth::Custom(name) => name.as_str(),
                };
                format!("quota: {} (requires {auth})", descriptor.label)
            });
            return vec![Line::styled(
                row.error
                    .clone()
                    .or(unavailable)
                    .unwrap_or_else(|| "quota: unavailable".into()),
                style::dim(),
            )];
        };
        let mut lines = Vec::new();
        for meter in &snapshot.meters {
            if meter.used.is_none() && meter.limit.is_none() && meter.utilization_percent.is_none()
            {
                continue;
            }
            let amount = match (meter.used, meter.limit) {
                (Some(used), Some(limit)) => format!("{used}/{limit}"),
                _ => meter
                    .utilization_percent
                    .map(|percent| format!("{percent:.1}% used"))
                    .unwrap_or_else(|| "unknown".into()),
            };
            let reset = meter
                .reset_at
                .map(|at| format!(" · resets {}", at.format("%Y-%m-%d %H:%M UTC")))
                .or_else(|| {
                    meter
                        .reset_in_seconds
                        .map(|seconds| format!(" · resets in {seconds}s"))
                })
                .unwrap_or_default();
            let bar = match (meter.used, meter.limit, meter.utilization_percent) {
                (Some(used), Some(limit), _) => Some(present::progress_bar(used, limit, 12)),
                (_, _, Some(percent)) => Some(present::progress_bar(
                    (percent.max(0.0) * 100.0) as u64,
                    10_000,
                    12,
                )),
                _ => None,
            };
            lines.push(Line::styled(
                match bar {
                    Some(bar) => format!("{}: {bar} {amount}{reset}", meter.label),
                    None => format!("{}: {amount}{reset}", meter.label),
                },
                style::bar(),
            ));
        }
        if lines.is_empty() {
            lines.push(Line::styled("quota returned no meter data", style::dim()));
        }
        lines
    }
}

#[async_trait]
impl ModalSurface for AccountsModal {
    fn title(&self) -> String {
        format!("Accounts — {}", self.provider)
    }

    fn height_hint(&self, _width: u16) -> u16 {
        (5 + (self.rows.len() as u16).saturating_mul(4)).min(24)
    }

    fn render(&self, area: Rect, frame: &mut Frame) {
        let inner = draw_chrome(&self.title(), area, frame);
        let mut lines = vec![hint_line(
            "↑↓ select · enter expand · r refresh · esc close",
        )];
        if self.rows.is_empty() {
            lines.push(Line::styled("no stored accounts", style::dim()));
        } else {
            let available = (inner.height as usize).saturating_sub(2).max(1);
            let start = self.selected.saturating_sub(1);
            let mut used = 0;
            for (index, row) in self.rows.iter().enumerate().skip(start) {
                let meter_lines = if self.expanded.get(index).copied().unwrap_or(false) {
                    Self::meter_lines(row)
                } else {
                    Vec::new()
                };
                let row_height = 1 + meter_lines.len();
                if used > 0 && used + row_height > available {
                    break;
                }
                let marker = if index == self.selected {
                    if self.expanded.get(index).copied().unwrap_or(false) {
                        "▾ "
                    } else {
                        "▸ "
                    }
                } else {
                    "  "
                };
                lines.push(Line::styled(
                    format!("{marker}{} ({})", row.id, row.kind),
                    if index == self.selected {
                        style::user()
                    } else {
                        style::bar()
                    },
                ));
                lines.extend(meter_lines);
                used += row_height;
            }
        }
        frame.render_widget(Paragraph::new(lines), inner);
    }

    async fn key(&mut self, k: KeyEvent) -> ModalAction {
        match k.code {
            KeyCode::Esc => ModalAction::Close,
            KeyCode::Up => {
                self.move_selection(-1);
                ModalAction::Stay
            }
            KeyCode::Down => {
                self.move_selection(1);
                ModalAction::Stay
            }
            KeyCode::PageUp => {
                self.move_selection(-3);
                ModalAction::Stay
            }
            KeyCode::PageDown => {
                self.move_selection(3);
                ModalAction::Stay
            }
            KeyCode::Enter => {
                if let Some(expanded) = self.expanded.get_mut(self.selected) {
                    *expanded = !*expanded;
                }
                ModalAction::Stay
            }
            KeyCode::Char('r') => {
                if let Some(row) = self.rows.get_mut(self.selected)
                    && let Some(source) = &row.source
                {
                    match source.fetch().await {
                        Ok(snapshot) => {
                            row.snapshot = Some(snapshot);
                            row.error = None;
                        }
                        Err(error) => row.error = Some(error.to_string()),
                    }
                }
                ModalAction::Stay
            }
            _ => ModalAction::Stay,
        }
    }

    fn cursor(&self, _area: Rect) -> Option<(u16, u16)> {
        None
    }
}

impl KindPickerModal {
    pub fn new(options: Vec<(String, String)>) -> Self {
        Self {
            list: ListInput::new(options),
        }
    }
}

#[async_trait]
impl ModalSurface for KindPickerModal {
    fn title(&self) -> String {
        "Add account".to_string()
    }

    fn height_hint(&self, _width: u16) -> u16 {
        4 + self.list.options.len() as u16
    }

    fn render(&self, area: Rect, frame: &mut Frame) {
        let inner = draw_chrome(&self.title(), area, frame);
        let mut lines = self.list.render_lines();
        lines.push(hint_line("enter choose · esc cancel"));
        frame.render_widget(Paragraph::new(lines), inner);
    }

    async fn key(&mut self, k: KeyEvent) -> ModalAction {
        match k.code {
            KeyCode::Esc => ModalAction::Close,
            KeyCode::Up => {
                self.list.move_selection(-1);
                ModalAction::Stay
            }
            KeyCode::Down => {
                self.list.move_selection(1);
                ModalAction::Stay
            }
            KeyCode::Enter => {
                let Some(kind) = self.list.current_value() else {
                    return ModalAction::Stay;
                };
                ModalAction::Emit(Action::OpenLogin { kind: Some(kind) })
            }
            _ => ModalAction::Stay,
        }
    }

    fn cursor(&self, _area: Rect) -> Option<(u16, u16)> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossterm::event::KeyModifiers;
    use firmius_core::{AccountKind, AlibabaTokenPlanKind, OpencodeGoKind, PersonaManager};

    fn key(code: KeyCode) -> KeyEvent {
        KeyEvent::new(code, KeyModifiers::NONE)
    }

    #[tokio::test]
    async fn personas_modal_saves_model_and_effort_together() {
        let root = std::env::temp_dir().join(format!(
            "firmius-personas-modal-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let personas_dir = root.join("personas");
        std::fs::create_dir_all(&personas_dir).unwrap();
        std::fs::write(
            personas_dir.join("coder.md"),
            "---\nname: Coder\ntool_scopes: [fs_read]\nbackground: true\n---\nCode.",
        )
        .unwrap();
        let personas = PersonaManager::load_from(personas_dir).unwrap().list();
        let settings = Arc::new(std::sync::Mutex::new(
            UserSettings::load_from_path(root.join("settings.json")).unwrap(),
        ));
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let mut modal = PersonasModal::new(
            personas,
            settings.clone(),
            Arc::new(std::sync::Mutex::new(manager)),
        );

        assert!(matches!(
            modal.key(key(KeyCode::Enter)).await,
            ModalAction::Stay
        ));
        modal.paste("codex/gpt-5.6-sol");
        assert!(matches!(
            modal.key(key(KeyCode::Enter)).await,
            ModalAction::Stay
        ));
        assert_eq!(modal.title(), "Persona effort");
        modal.key(key(KeyCode::Down)).await;
        modal.key(key(KeyCode::Down)).await;
        assert!(matches!(
            modal.key(key(KeyCode::Enter)).await,
            ModalAction::Stay
        ));

        let settings = settings.lock().unwrap();
        let preferred = settings.preferred_model("coder").unwrap();
        assert_eq!(preferred.provider_id, "codex");
        assert_eq!(preferred.model, "gpt-5.6-sol");
        assert_eq!(preferred.effort.as_deref(), Some("medium"));
        drop(settings);
        std::fs::remove_dir_all(root).ok();
    }

    #[tokio::test]
    async fn kind_picker_navigates_and_emits_open_login() {
        let mut modal = KindPickerModal::new(vec![
            ("api-key".to_string(), "API key".to_string()),
            ("opencode-go".to_string(), "OpenCode Go".to_string()),
        ]);
        assert!(matches!(
            modal.key(key(KeyCode::Down)).await,
            ModalAction::Stay
        ));
        match modal.key(key(KeyCode::Enter)).await {
            ModalAction::Emit(Action::OpenLogin { kind }) => {
                assert_eq!(kind.as_deref(), Some("opencode-go"));
            }
            _ => panic!("unexpected modal action"),
        }
        // Esc closes without emitting.
        let mut modal = KindPickerModal::new(vec![("a".to_string(), "A".to_string())]);
        assert!(matches!(
            modal.key(key(KeyCode::Esc)).await,
            ModalAction::Close
        ));
    }

    #[tokio::test]
    async fn wizard_modal_types_a_key_and_emits_register_account() {
        let kind = OpencodeGoKind;
        let mut modal = WizardModal::start(
            "opencode-go".to_string(),
            kind.display_name().to_string(),
            kind.wizard(),
        )
        .await;
        for c in "oc-key".chars() {
            assert!(matches!(
                modal.key(key(KeyCode::Char(c))).await,
                ModalAction::Stay
            ));
        }
        match modal.key(key(KeyCode::Enter)).await {
            ModalAction::Emit(Action::RegisterAccount { record }) => {
                assert_eq!(record.id, "opencode-go");
                assert_eq!(record.kind, "opencode-go");
                assert_eq!(record.credentials["api_key"], "oc-key");
                assert_eq!(record.schema.models.len(), 25);
            }
            _ => panic!("unexpected modal action"),
        }
    }

    #[tokio::test]
    async fn wizard_modal_accepts_pasted_prompt_input() {
        let kind = OpencodeGoKind;
        let mut modal = WizardModal::start(
            "opencode-go".to_string(),
            kind.display_name().to_string(),
            kind.wizard(),
        )
        .await;

        modal.paste("oc-pasted-key");
        match modal.key(key(KeyCode::Enter)).await {
            ModalAction::Emit(Action::RegisterAccount { record }) => {
                assert_eq!(record.credentials["api_key"], "oc-pasted-key");
            }
            _ => panic!("unexpected modal action"),
        }
    }

    #[tokio::test]
    async fn wizard_modal_walks_a_select_then_prompt_flow() {
        let kind = AlibabaTokenPlanKind;
        let mut modal = WizardModal::start(
            "alibaba-token-plan".to_string(),
            kind.display_name().to_string(),
            kind.wizard(),
        )
        .await;
        // Region step: down selects China, enter advances to the key prompt.
        assert!(matches!(
            modal.key(key(KeyCode::Down)).await,
            ModalAction::Stay
        ));
        assert!(matches!(
            modal.key(key(KeyCode::Enter)).await,
            ModalAction::Stay
        ));
        // Key step: submit empty -> error state keeps the modal open.
        assert!(matches!(
            modal.key(key(KeyCode::Enter)).await,
            ModalAction::Stay
        ));
        assert!(modal.error.is_some());
        // A real key finishes the account.
        for c in "sk-sp-test".chars() {
            modal.key(key(KeyCode::Char(c))).await;
        }
        match modal.key(key(KeyCode::Enter)).await {
            ModalAction::Emit(Action::RegisterAccount { record }) => {
                assert_eq!(record.credentials["region"], "china");
                assert_eq!(
                    record.schema.base_url.as_deref(),
                    Some(firmius_core::kinds::alibaba::ALIBABA_CN_BASE_URL)
                );
            }
            _ => panic!("unexpected modal action"),
        }
    }

    #[tokio::test]
    async fn accounts_modal_navigates_and_closes() {
        let descriptor = QuotaDescriptor {
            label: "Test quota".into(),
            auth: QuotaAuth::ApiKey,
            meters: vec!["5-hour".into()],
        };
        let mut modal = AccountsModal::new(
            "test-provider".into(),
            vec![
                AccountRow {
                    id: "first".into(),
                    kind: "test-provider".into(),
                    descriptor: Some(descriptor.clone()),
                    source: None,
                    snapshot: None,
                    error: None,
                },
                AccountRow {
                    id: "second".into(),
                    kind: "test-provider".into(),
                    descriptor: Some(descriptor),
                    source: None,
                    snapshot: None,
                    error: None,
                },
            ],
        );
        assert!(matches!(
            modal.key(key(KeyCode::Down)).await,
            ModalAction::Stay
        ));
        assert_eq!(modal.selected, 1);
        assert!(matches!(
            modal.key(key(KeyCode::PageUp)).await,
            ModalAction::Stay
        ));
        assert_eq!(modal.selected, 0);
        assert!(matches!(
            modal.key(key(KeyCode::Enter)).await,
            ModalAction::Stay
        ));
        assert!(modal.expanded[0]);
        assert!(matches!(
            modal.key(key(KeyCode::Enter)).await,
            ModalAction::Stay
        ));
        assert!(!modal.expanded[0]);
        assert!(matches!(
            modal.key(key(KeyCode::Esc)).await,
            ModalAction::Close
        ));
    }
}
