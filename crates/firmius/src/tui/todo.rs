//! Native todo rail projection for the compact view under the busy phrase row.
//!
//! Todos are the focused agent's private execution checklist and are unrelated
//! to durable [`super::work::WorkView`] graphs.  This module is a pure adapter:
//! it never parses `todo` tool output, never reconstructs state from prose, and
//! never infers finality from a count or a status label.  Finality comes from
//! the typed `CompletionEvaluation` carried by the ledger or the typed
//! `TodoCompletionDto` carried by the daemon projection.
//!
//! Two sources feed the same rail shape: an embedded `TodoLedger` (when the
//! TUI owns the runtime) and the bounded `TodoProjectionDto` sent in status and
//! snapshot messages (when a daemon owns it).  A missing projection means the
//! ledger is quarantined or unavailable, which must render as a warning rather
//! than as an empty checklist.

use firmius_core::todo::{CompletionEvaluation, TodoItemProjection, TodoItemStatus, TodoLedger};
use firmius_protocol::{TodoCompletionDto, TodoOutcomeKindDto, TodoProjectionDto, TodoStatusDto};

/// Maximum item rows drawn in the rail.  With the header this keeps the rail
/// at five terminal rows, the budget the previous work rail used.
pub const RAIL_MAX_ROWS: usize = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TodoAvailability {
    /// A canonical projection exists (it may legitimately contain no items).
    #[default]
    Tracked,
    /// No ledger exists yet, or the ledger has nothing to show. No rail.
    Empty,
    /// Persisted state is malformed/unavailable. Warn, never look empty.
    Unavailable,
}

/// Presentation state of one todo row.  Kept separate from the canonical item
/// status so glyph and color choices cannot silently redefine domain state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TodoPresentation {
    Blocked,
    InProgress,
    Pending,
    Completed,
    Cancelled,
}

impl TodoPresentation {
    pub fn from_status(status: TodoItemStatus) -> Self {
        match status {
            TodoItemStatus::Blocked => Self::Blocked,
            TodoItemStatus::InProgress => Self::InProgress,
            TodoItemStatus::Pending => Self::Pending,
            TodoItemStatus::Completed => Self::Completed,
            TodoItemStatus::Cancelled => Self::Cancelled,
        }
    }

    pub fn glyph(self) -> &'static str {
        match self {
            Self::Blocked => "!",
            Self::InProgress => "◐",
            Self::Pending => "○",
            Self::Completed => "✓",
            Self::Cancelled => "⊘",
        }
    }

    /// Priority band used to choose which rows survive a tight budget.
    /// Blocked work is the reason to look at the rail at all, followed by
    /// active work, then everything still open.
    fn band(self) -> u8 {
        match self {
            Self::Blocked => 0,
            Self::InProgress => 1,
            Self::Pending => 2,
            Self::Completed => 3,
            Self::Cancelled => 4,
        }
    }

    pub fn is_unfinished(self) -> bool {
        matches!(self, Self::Blocked | Self::InProgress | Self::Pending)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TodoRow {
    pub id: String,
    pub title: String,
    pub presentation: TodoPresentation,
    pub evidence_required: bool,
    pub evidence_count: usize,
    pub waiting_reason: Option<String>,
}

impl TodoRow {
    /// Concise trailing detail: why a row is waiting, then its evidence state.
    pub fn detail(&self) -> Option<String> {
        let mut parts = Vec::new();
        if let Some(reason) = self.waiting_reason.as_deref() {
            parts.push(format!("waiting: {reason}"));
        }
        if self.evidence_required || self.evidence_count > 0 {
            parts.push(format!("evidence {}", self.evidence_count));
        }
        (!parts.is_empty()).then(|| parts.join(" · "))
    }
}

/// Compact mirror of the typed completion evaluation, so the rail never has to
/// keep a core ledger alive just to know whether completion may be claimed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TodoCompletion {
    #[default]
    Untracked,
    NoActiveCycle,
    Waiting {
        unfinished: usize,
        blocked: usize,
        evidence_deficits: usize,
    },
    Ready {
        completed: usize,
        cancelled: usize,
    },
    Final {
        outcome: TodoOutcomeKindDto,
        completed: usize,
        cancelled: usize,
    },
}

impl TodoCompletion {
    pub fn is_final(self) -> bool {
        matches!(self, Self::Final { .. })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TodoRail {
    pub agent_id: String,
    pub revision: u64,
    pub availability: TodoAvailability,
    pub completion: TodoCompletion,
    pub reason: Option<String>,
    rows: Vec<TodoRow>,
}

impl Default for TodoRail {
    fn default() -> Self {
        Self {
            agent_id: String::new(),
            revision: 0,
            availability: TodoAvailability::Empty,
            completion: TodoCompletion::Untracked,
            reason: None,
            rows: Vec::new(),
        }
    }
}

impl TodoRail {
    /// Compact counts for the renderer's progress meter. The counts are
    /// derived from typed row state, never from tool JSON or prose.
    pub fn progress_counts(&self) -> (usize, usize, usize, usize) {
        let total = self.rows.len();
        let completed = self
            .rows
            .iter()
            .filter(|row| row.presentation == TodoPresentation::Completed)
            .count();
        let cancelled = self
            .rows
            .iter()
            .filter(|row| row.presentation == TodoPresentation::Cancelled)
            .count();
        let active = self
            .rows
            .iter()
            .filter(|row| row.presentation == TodoPresentation::InProgress)
            .count();
        (total, completed, cancelled, active)
    }

    /// No agent, or an agent with nothing to show. Renders no permanent rail.
    pub fn absent() -> Self {
        Self::default()
    }

    /// Persisted todo state could not be read (quarantined, owner mismatch,
    /// or an agent that is no longer live). Never renders as "no todos".
    pub fn unavailable(reason: impl Into<String>) -> Self {
        Self {
            availability: TodoAvailability::Unavailable,
            completion: TodoCompletion::Untracked,
            reason: Some(reason.into()),
            ..Self::default()
        }
    }

    /// Build from the canonical ledger. Cycles are selected by creation time
    /// so projections are deterministic despite the id-keyed cycle map.
    pub fn from_ledger(ledger: &TodoLedger) -> Self {
        let projection = ledger.project(true);
        let mut cycles = projection.cycles.iter().collect::<Vec<_>>();
        cycles.sort_by_key(|cycle| (cycle.created_at, cycle.id.0.clone()));
        let active = projection
            .active_cycle_id
            .as_ref()
            .and_then(|id| cycles.iter().find(|cycle| &cycle.id == id).copied());
        let displayed = active.or_else(|| cycles.last().copied());
        let rows = displayed
            .map(|cycle| {
                cycle
                    .items
                    .iter()
                    .map(row_from_projection)
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        let completion = match ledger.evaluate_completion() {
            CompletionEvaluation::NoActiveCycle => TodoCompletion::NoActiveCycle,
            CompletionEvaluation::Waiting {
                unfinished_items,
                blocked_items,
                evidence_deficits,
                ..
            } => TodoCompletion::Waiting {
                unfinished: unfinished_items.len(),
                blocked: blocked_items.len(),
                evidence_deficits: evidence_deficits.len(),
            },
            CompletionEvaluation::Ready {
                completed_items,
                cancelled_items,
                ..
            } => TodoCompletion::Ready {
                completed: completed_items,
                cancelled: cancelled_items,
            },
            CompletionEvaluation::Final { outcome, .. } => TodoCompletion::Final {
                outcome: match outcome.kind {
                    firmius_core::todo::TodoOutcomeKind::Completed => TodoOutcomeKindDto::Completed,
                    firmius_core::todo::TodoOutcomeKind::Cancelled => TodoOutcomeKindDto::Cancelled,
                },
                completed: outcome.completed_items,
                cancelled: outcome.cancelled_items,
            },
        };
        Self::assemble(ledger.agent_id(), ledger.revision(), rows, completion)
    }

    /// Build from the daemon's bounded wire projection.  Item detail travels
    /// with the projection, so the remote rail is as informative as the
    /// embedded one.
    pub fn from_dto(dto: &TodoProjectionDto) -> Self {
        let rows = dto
            .items
            .iter()
            .map(|item| TodoRow {
                id: item.id.clone(),
                title: item.title.clone(),
                presentation: match item.status {
                    TodoStatusDto::Pending => TodoPresentation::Pending,
                    TodoStatusDto::InProgress => TodoPresentation::InProgress,
                    TodoStatusDto::Blocked => TodoPresentation::Blocked,
                    TodoStatusDto::Completed => TodoPresentation::Completed,
                    TodoStatusDto::Cancelled => TodoPresentation::Cancelled,
                },
                evidence_required: item.evidence_required,
                evidence_count: item.evidence_count,
                waiting_reason: item.waiting_reason.clone(),
            })
            .collect::<Vec<_>>();
        let completion = match dto.completion {
            TodoCompletionDto::NoActiveCycle => TodoCompletion::NoActiveCycle,
            TodoCompletionDto::Waiting {
                unfinished,
                blocked,
                evidence_deficits,
            } => TodoCompletion::Waiting {
                unfinished,
                blocked,
                evidence_deficits,
            },
            TodoCompletionDto::Ready {
                completed,
                cancelled,
                ..
            } => TodoCompletion::Ready {
                completed,
                cancelled,
            },
            TodoCompletionDto::Final {
                outcome,
                completed,
                cancelled,
            } => TodoCompletion::Final {
                outcome,
                completed,
                cancelled,
            },
        };
        Self::assemble(&dto.agent_id, dto.revision, rows, completion)
    }

    fn assemble(
        agent_id: &str,
        revision: u64,
        rows: Vec<TodoRow>,
        completion: TodoCompletion,
    ) -> Self {
        // Blocked, then active, then open, then settled. `sort_by_key` is
        // stable, so authored order is preserved inside every band.
        let mut rows = rows;
        rows.sort_by_key(|row| row.presentation.band());
        // An active cycle is meaningful even before its first item is added.
        // The begin mutation must therefore produce a visible zero-item rail
        // instead of being mistaken for an absent ledger.
        let availability = if rows.is_empty()
            && matches!(
                completion,
                TodoCompletion::Untracked | TodoCompletion::NoActiveCycle
            ) {
            TodoAvailability::Empty
        } else {
            TodoAvailability::Tracked
        };
        Self {
            agent_id: agent_id.to_string(),
            revision,
            availability,
            completion,
            reason: None,
            rows,
        }
    }

    pub fn rows(&self) -> &[TodoRow] {
        &self.rows
    }

    pub fn is_empty(&self) -> bool {
        self.availability == TodoAvailability::Empty && self.reason.is_none()
    }

    pub fn unfinished(&self) -> usize {
        self.rows
            .iter()
            .filter(|row| row.presentation.is_unfinished())
            .count()
    }

    pub fn blocked(&self) -> usize {
        self.rows
            .iter()
            .filter(|row| row.presentation == TodoPresentation::Blocked)
            .count()
    }

    pub fn completed(&self) -> usize {
        self.rows
            .iter()
            .filter(|row| row.presentation == TodoPresentation::Completed)
            .count()
    }

    /// Settled items as reported by the typed evaluation.  Preferring the
    /// evaluation keeps a cancellation from being presented as a completion.
    fn settled(&self) -> usize {
        match self.completion {
            TodoCompletion::Final {
                completed,
                cancelled,
                ..
            } => completed + cancelled,
            _ => self.completed(),
        }
    }

    /// Whether the typed completion state allows a completion claim. Rows may
    /// all look settled while the evaluation still waits for evidence or work.
    pub fn completion_confirmed(&self) -> bool {
        match self.completion {
            TodoCompletion::Final { .. } => true,
            TodoCompletion::Ready { .. } => self.unfinished() == 0,
            _ => false,
        }
    }

    /// Rows that fit the budget plus the exact overflow count.
    pub fn visible(&self, max_rows: usize) -> (Vec<TodoRow>, usize) {
        let budget = max_rows.min(RAIL_MAX_ROWS);
        if budget == 0 {
            return (Vec::new(), self.rows.len());
        }
        let overflow = self.rows.len().saturating_sub(budget);
        (self.rows.iter().take(budget).cloned().collect(), overflow)
    }

    /// Header text without theme styling. Domain status text stays
    /// authoritative; glyph mapping is presentation-only.
    pub fn headline(&self) -> String {
        if self.availability == TodoAvailability::Unavailable {
            return "TODOS  unavailable".to_string();
        }
        match self.completion {
            TodoCompletion::Final {
                outcome: TodoOutcomeKindDto::Completed,
                ..
            } => {
                format!(
                    "✓ TODOS · {} complete · rev {}",
                    self.settled(),
                    self.revision
                )
            }
            TodoCompletion::Final {
                outcome: TodoOutcomeKindDto::Cancelled,
                ..
            } => {
                format!(
                    "⊘ TODOS · {} cancelled · rev {}",
                    self.settled(),
                    self.revision
                )
            }
            _ => {
                let mut text = format!("TODOS  {} open", self.unfinished());
                if self.blocked() > 0 {
                    text.push_str(&format!(" · {} waiting", self.blocked()));
                }
                if let TodoCompletion::Waiting {
                    evidence_deficits, ..
                } = self.completion
                    && evidence_deficits > 0
                {
                    text.push_str(&format!(" · {evidence_deficits} need evidence"));
                }
                text.push_str(&format!(" · rev {}", self.revision));
                text
            }
        }
    }

    /// One-line fallback used when the transcript and the rail cannot share
    /// the available height.
    pub fn summary_line(&self) -> String {
        if self.availability == TodoAvailability::Unavailable {
            return "Todos unavailable; persisted data preserved".to_string();
        }
        if self.completion_confirmed() {
            return match self.completion {
                TodoCompletion::Final {
                    outcome: TodoOutcomeKindDto::Cancelled,
                    ..
                } => format!("⊘ Todos · {} cancelled", self.settled()),
                _ => format!("✓ Todos · {} complete", self.settled()),
            };
        }
        let mut text = format!("Todos: {} open", self.unfinished());
        if self.blocked() > 0 {
            text.push_str(&format!(" · {} waiting", self.blocked()));
        }
        text
    }

    /// Warning text for degraded states (`None` when the rail is healthy).
    pub fn warning(&self) -> Option<String> {
        if self.availability != TodoAvailability::Unavailable {
            return None;
        }
        let reason = self.reason.as_deref().unwrap_or("state unavailable");
        Some(format!(
            "Todos unavailable; persisted data preserved ({reason})"
        ))
    }
}

fn row_from_projection(item: &TodoItemProjection) -> TodoRow {
    TodoRow {
        id: item.id.to_string(),
        title: item.title.clone(),
        presentation: TodoPresentation::from_status(item.status),
        evidence_required: item.evidence_required,
        evidence_count: item.evidence_count,
        waiting_reason: item.blocking_reason.clone(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use firmius_core::todo::{
        EvidenceAttachment, NewTodoItem, TodoIntent, TodoItemPatch, TodoItemStatus,
    };

    /// Build a ledger with one item per entry, returning the ledger and the
    /// created item ids in authored order.
    fn ledger_with(
        items: &[(&str, TodoItemStatus, bool)],
    ) -> (TodoLedger, Vec<firmius_core::todo::TodoItemId>) {
        let mut ledger = TodoLedger::new("agent-a");
        ledger
            .begin(
                0,
                TodoIntent {
                    summary: "ship the rail".into(),
                    completion_criteria: vec![],
                },
            )
            .unwrap();
        let mut ids = Vec::new();
        for (title, status, evidence_required) in items {
            let revision = ledger.revision();
            let item = ledger
                .add(
                    revision,
                    NewTodoItem {
                        title: (*title).into(),
                        description: None,
                        evidence_required: *evidence_required,
                    },
                )
                .unwrap();
            ids.push(item.id.clone());
            let revision = ledger.revision();
            match status {
                TodoItemStatus::Pending => {}
                TodoItemStatus::InProgress => {
                    ledger
                        .update(
                            revision,
                            &item.id,
                            TodoItemPatch {
                                status: Some(TodoItemStatus::InProgress),
                                ..Default::default()
                            },
                        )
                        .unwrap();
                }
                TodoItemStatus::Blocked => {
                    ledger
                        .block(revision, &item.id, "service api".into())
                        .unwrap();
                }
                TodoItemStatus::Completed => {
                    ledger.complete(revision, &item.id, None).unwrap();
                }
                TodoItemStatus::Cancelled => {
                    ledger
                        .cancel(revision, &item.id, "not needed".into())
                        .unwrap();
                }
            }
        }
        (ledger, ids)
    }

    #[test]
    fn blocked_then_active_then_open_with_authored_order_inside_each_band() {
        let (ledger, _) = ledger_with(&[
            ("first pending", TodoItemStatus::Pending, false),
            ("blocked one", TodoItemStatus::Blocked, false),
            ("active", TodoItemStatus::InProgress, false),
            ("second pending", TodoItemStatus::Pending, false),
        ]);
        let rail = TodoRail::from_ledger(&ledger);
        let titles = rail
            .rows()
            .iter()
            .map(|row| row.title.as_str())
            .collect::<Vec<_>>();
        assert_eq!(
            titles,
            vec!["blocked one", "active", "first pending", "second pending"]
        );
        assert_eq!(rail.unfinished(), 4);
        assert_eq!(rail.blocked(), 1);
    }

    #[test]
    fn a_blocked_row_explains_itself_and_reports_evidence() {
        let (mut ledger, ids) = ledger_with(&[("verify", TodoItemStatus::Pending, true)]);
        let revision = ledger.revision();
        ledger
            .attach_evidence(
                revision,
                EvidenceAttachment {
                    item_id: Some(ids[0].clone()),
                    kind: "test".into(),
                    reference: "cargo test".into(),
                    summary: None,
                },
            )
            .unwrap();
        let revision = ledger.revision();
        ledger
            .block(revision, &ids[0], "waiting on ci".into())
            .unwrap();
        let rail = TodoRail::from_ledger(&ledger);
        let detail = rail.rows()[0].detail().unwrap();
        assert!(detail.contains("waiting: waiting on ci"), "{detail}");
        assert!(detail.contains("evidence 1"), "{detail}");
    }

    #[test]
    fn budget_keeps_the_attention_rows_and_reports_exact_overflow() {
        let (ledger, _) = ledger_with(&[
            ("a", TodoItemStatus::Completed, false),
            ("b", TodoItemStatus::Pending, false),
            ("c", TodoItemStatus::Pending, false),
            ("d", TodoItemStatus::Blocked, false),
            ("e", TodoItemStatus::InProgress, false),
            ("f", TodoItemStatus::Pending, false),
        ]);
        let rail = TodoRail::from_ledger(&ledger);
        let (visible, overflow) = rail.visible(4);
        assert_eq!(visible.len(), 4);
        assert_eq!(overflow, 2);
        // The blocked row is never the one cut.
        assert_eq!(visible[0].title, "d");
        // A zero budget yields no rows and an exact overflow count.
        let (none, overflow) = rail.visible(0);
        assert!(none.is_empty());
        assert_eq!(overflow, 6);
        // More budget than the rail cap still bounds the drawn rows.
        let (capped, _) = rail.visible(usize::MAX);
        assert_eq!(capped.len(), RAIL_MAX_ROWS);
    }

    #[test]
    fn completion_is_never_inferred_from_counts() {
        // Every row looks settled, but the typed evaluation still waits for
        // evidence, so the rail must not claim completion.
        let (mut ledger, ids) = ledger_with(&[("gated", TodoItemStatus::Completed, true)]);
        let rail = TodoRail::from_ledger(&ledger);
        assert!(matches!(rail.completion, TodoCompletion::Waiting { .. }));
        assert!(!rail.completion_confirmed());
        assert!(
            rail.summary_line().starts_with("Todos:"),
            "{}",
            rail.summary_line()
        );

        let revision = ledger.revision();
        ledger
            .attach_evidence(
                revision,
                EvidenceAttachment {
                    item_id: Some(ids[0].clone()),
                    kind: "test".into(),
                    reference: "cargo test".into(),
                    summary: None,
                },
            )
            .unwrap();
        let ready = TodoRail::from_ledger(&ledger);
        assert!(matches!(ready.completion, TodoCompletion::Ready { .. }));
        // Ready is not final, and `assess` has not run.
        assert!(!ready.completion.is_final());
        assert!(ready.completion_confirmed());

        let revision = ledger.revision();
        ledger.assess(revision).unwrap();
        let final_rail = TodoRail::from_ledger(&ledger);
        assert!(final_rail.completion_confirmed());
        assert!(
            final_rail.headline().starts_with("✓ TODOS"),
            "{}",
            final_rail.headline()
        );
        assert_eq!(final_rail.settled(), 1);
    }

    #[test]
    fn active_empty_cycle_still_renders_a_todo_header() {
        let mut ledger = TodoLedger::new("agent-a");
        ledger
            .begin(
                0,
                TodoIntent {
                    summary: "inspect the workspace".into(),
                    completion_criteria: vec!["report findings".into()],
                },
            )
            .unwrap();
        let rail = TodoRail::from_ledger(&ledger);
        assert!(!rail.is_empty());
        assert_eq!(rail.unfinished(), 0);
        assert!(
            rail.headline().starts_with("TODOS  0 open"),
            "{}",
            rail.headline()
        );
    }

    #[test]
    fn a_cancelled_cycle_is_not_presented_as_completed() {
        let (mut ledger, ids) = ledger_with(&[("dropped", TodoItemStatus::Pending, false)]);
        let revision = ledger.revision();
        ledger.cancel(revision, &ids[0], "obsolete".into()).unwrap();
        let revision = ledger.revision();
        ledger.assess(revision).unwrap();
        let rail = TodoRail::from_ledger(&ledger);
        assert!(matches!(rail.completion, TodoCompletion::Final { .. }));
        assert!(
            rail.headline().starts_with("⊘ TODOS"),
            "{}",
            rail.headline()
        );
        assert!(
            rail.headline().contains("1 cancelled"),
            "{}",
            rail.headline()
        );
    }

    #[test]
    fn unavailable_state_warns_instead_of_looking_empty() {
        let rail = TodoRail::unavailable("quarantined envelope");
        assert!(!rail.is_empty());
        assert!(rail.headline().contains("unavailable"));
        assert!(
            rail.warning().unwrap().contains("persisted data preserved"),
            "{:?}",
            rail.warning()
        );
        assert!(TodoRail::absent().is_empty());
        assert!(TodoRail::absent().warning().is_none());
    }

    #[test]
    fn dto_projection_matches_the_ledger_projection() {
        let (ledger, _) = ledger_with(&[
            ("open", TodoItemStatus::Pending, false),
            ("done", TodoItemStatus::Completed, false),
        ]);
        let dto = TodoProjectionDto {
            version: firmius_protocol::TODO_DTO_VERSION,
            agent_id: "agent-a".into(),
            revision: ledger.revision(),
            pending: 1,
            in_progress: 0,
            blocked: 0,
            completed: 1,
            items: vec![
                firmius_protocol::TodoItemDto {
                    id: "a".into(),
                    title: "open".into(),
                    status: TodoStatusDto::Pending,
                    evidence_count: 0,
                    evidence_required: false,
                    waiting_reason: None,
                },
                firmius_protocol::TodoItemDto {
                    id: "b".into(),
                    title: "done".into(),
                    status: TodoStatusDto::Completed,
                    evidence_count: 0,
                    evidence_required: false,
                    waiting_reason: None,
                },
            ],
            completion: TodoCompletionDto::Waiting {
                unfinished: 1,
                blocked: 0,
                evidence_deficits: 0,
            },
        };
        let from_dto = TodoRail::from_dto(&dto);
        let from_ledger = TodoRail::from_ledger(&ledger);
        assert_eq!(from_dto.agent_id, from_ledger.agent_id);
        assert_eq!(from_dto.revision, from_ledger.revision);
        assert_eq!(from_dto.unfinished(), from_ledger.unfinished());
        assert_eq!(from_dto.completed(), from_ledger.completed());
        assert_eq!(from_dto.completion, from_ledger.completion);
        assert_eq!(from_dto.summary_line(), from_ledger.summary_line());
        assert_eq!(from_dto.headline(), from_ledger.headline());
    }
}

/// Layout tests: what the terminal actually shows, not just the projection.
#[cfg(test)]
mod layout_tests {
    use super::*;
    use crate::tui::model::Model;
    use crate::tui::view;
    use firmius_core::{
        ExecutionStatus, FirmiusConfig, GraphMode, McpManager, PersonaManager, ProviderManager,
        ToolRegistry, UserSettings, WorkGraph, WorkNode, WorkSnapshot, WorkState,
    };
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;
    use std::sync::{Arc, Mutex};

    fn test_model(cols: u16, rows: u16) -> (Model, Terminal<TestBackend>) {
        let model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(Mutex::new(ProviderManager::new())),
            "test-model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(Mutex::new(UserSettings::default())),
            Arc::new(Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        let terminal = Terminal::new(TestBackend::new(cols, rows)).unwrap();
        (model, terminal)
    }

    fn screen(terminal: &Terminal<TestBackend>) -> Vec<String> {
        let buffer = terminal.backend().buffer();
        let width = buffer.area.width as usize;
        buffer
            .content
            .chunks(width)
            .map(|row| row.iter().map(|cell| cell.symbol()).collect::<String>())
            .collect()
    }

    fn row_of(rows: &[String], needle: &str) -> Option<usize> {
        rows.iter().position(|row| row.contains(needle))
    }

    fn dto(items: &[(&str, TodoStatusDto)], completion: TodoCompletionDto) -> TodoProjectionDto {
        let mut pending = 0;
        let mut in_progress = 0;
        let mut blocked = 0;
        let mut completed = 0;
        for (_, status) in items {
            match status {
                TodoStatusDto::Pending => pending += 1,
                TodoStatusDto::InProgress => in_progress += 1,
                TodoStatusDto::Blocked => blocked += 1,
                TodoStatusDto::Completed => completed += 1,
                TodoStatusDto::Cancelled => {}
            }
        }
        TodoProjectionDto {
            version: firmius_protocol::TODO_DTO_VERSION,
            agent_id: "agent".into(),
            revision: 7,
            pending,
            in_progress,
            blocked,
            completed,
            items: items
                .iter()
                .enumerate()
                .map(|(index, (title, status))| firmius_protocol::TodoItemDto {
                    id: format!("item-{index}"),
                    title: (*title).to_string(),
                    status: status.clone(),
                    evidence_count: 0,
                    evidence_required: false,
                    waiting_reason: None,
                })
                .collect(),
            completion,
        }
    }

    /// The todos rail replaces the old task-list rail: it sits directly beneath
    /// the busy phrase row, above the composer/bottom bar, and names itself
    /// distinctly from Work.
    #[test]
    fn todo_rail_renders_between_the_phrase_row_and_the_bottom_bar() {
        let (mut model, mut terminal) = test_model(60, 24);
        model.busy = true;
        model.live_phrase = "thinking about the rail".into();
        model.todo_rail_override = Some(TodoRail::from_dto(&dto(
            &[
                ("run focused tests", TodoStatusDto::InProgress),
                ("document retry behavior", TodoStatusDto::Pending),
            ],
            TodoCompletionDto::Waiting {
                unfinished: 2,
                blocked: 0,
                evidence_deficits: 0,
            },
        )));
        terminal
            .draw(|frame| view::draw(&mut model, frame))
            .unwrap();
        let rows = screen(&terminal);
        let phrase = row_of(&rows, "thinking about the rail").expect("phrase row is drawn");
        let header = row_of(&rows, "TODOS").expect("todo header is drawn");
        let in_progress = row_of(&rows, "run focused tests").expect("an item row is drawn");
        let bottom = row_of(&rows, "perm:").expect("the bottom bar is drawn");
        assert!(
            phrase < header,
            "rail must sit below the phrase row: {rows:?}"
        );
        assert!(header < in_progress, "rows follow the header");
        assert!(in_progress < bottom, "rail must stay above the bottom bar");
        assert!(rows[header].contains("2 open"), "{}", rows[header]);
        assert!(rows[header].contains("rev 7"), "{}", rows[header]);
    }

    /// A committed begin event is still visible while no item rows exist.
    /// This protects older ledgers (and intentionally empty cycles) from
    /// disappearing at the exact phrase-row boundary where the rail belongs.
    #[test]
    fn active_empty_todo_cycle_renders_below_the_phrase_row() {
        let (mut model, mut terminal) = test_model(60, 24);
        model.busy = true;
        model.live_phrase = "thinking about the rail".into();
        model.todo_rail_override = Some(TodoRail::from_dto(&dto(
            &[],
            TodoCompletionDto::Waiting {
                unfinished: 0,
                blocked: 0,
                evidence_deficits: 0,
            },
        )));

        terminal
            .draw(|frame| view::draw(&mut model, frame))
            .unwrap();
        let rows = screen(&terminal);
        let phrase = row_of(&rows, "thinking about the rail").expect("phrase row is drawn");
        let header = row_of(&rows, "TODOS  0 open").expect("empty active header is drawn");
        let bottom = row_of(&rows, "perm:").expect("the bottom bar is drawn");
        assert!(
            phrase < header,
            "rail must sit below the phrase row: {rows:?}"
        );
        assert!(header < bottom, "rail must stay above the bottom bar");
    }

    /// Durable work now renders in the transcript: it appears above the phrase
    /// row and the todo rail, and it is never drawn in the todo rail's slot.
    #[test]
    fn work_renders_in_the_transcript_above_the_todo_rail() {
        let (mut model, mut terminal) = test_model(70, 24);
        // A live session (here, an attached daemon snapshot) is what makes the
        // TUI show a transcript rather than the welcome screen.
        model.replace_remote_snapshot(firmius_protocol::SessionSnapshot {
            session_id: "session".into(),
            title: None,
            sequence: 1,
            primary_agent_id: "agent".into(),
            agents: Vec::new(),
            hierarchy: Default::default(),
            work: graph_with_nodes(
                "agent",
                &[ExecutionStatus::Running, ExecutionStatus::Pending],
            ),
            active_turns: Default::default(),
            active_delegates: 0,
            live_events: Vec::new(),
        });
        model.busy = true;
        model.live_phrase = "working".into();
        model.todo_rail_override = Some(TodoRail::from_dto(&dto(
            &[("ship the rail", TodoStatusDto::InProgress)],
            TodoCompletionDto::Waiting {
                unfinished: 1,
                blocked: 0,
                evidence_deficits: 0,
            },
        )));
        terminal
            .draw(|frame| view::draw(&mut model, frame))
            .unwrap();
        let rows = screen(&terminal);
        let work = row_of(&rows, "WORK").expect("the task list renders as transcript content");
        let phrase = row_of(&rows, "working").expect("phrase row");
        let todos = row_of(&rows, "TODOS").expect("todo rail");
        assert!(
            work < phrase,
            "work must be above the rail region: {rows:?}"
        );
        assert!(phrase < todos, "todos follow the phrase row");
        assert!(row_of(&rows, "Item 0").is_some(), "node rows are drawn");
    }

    /// A terminal too short for the rail keeps the composer and collapses the
    /// rail to one truthful summary line instead of stealing protected rows.
    #[test]
    fn a_short_terminal_collapses_the_rail_and_protects_the_composer() {
        let (mut model, mut terminal) = test_model(44, 9);
        model.todo_rail_override = Some(TodoRail::from_dto(&dto(
            &[
                ("first item", TodoStatusDto::Pending),
                ("second item", TodoStatusDto::Pending),
                ("third item", TodoStatusDto::Pending),
                ("fourth item", TodoStatusDto::Pending),
            ],
            TodoCompletionDto::Waiting {
                unfinished: 4,
                blocked: 0,
                evidence_deficits: 0,
            },
        )));
        terminal
            .draw(|frame| view::draw(&mut model, frame))
            .unwrap();
        let rows = screen(&terminal);
        assert!(
            row_of(&rows, "perm:").is_some(),
            "the bottom bar survives a short terminal"
        );
        assert!(
            row_of(&rows, "Todos: 4 open").is_some(),
            "the rail collapses to its summary: {rows:?}"
        );
        assert!(
            row_of(&rows, "first item").is_none(),
            "item rows are withheld when they cannot fit"
        );
    }

    /// A finished rail is never presented as unfinished work and vice versa.
    #[test]
    fn a_final_rail_shows_completion_and_not_open_counts() {
        let (mut model, mut terminal) = test_model(60, 24);
        model.todo_rail_override = Some(TodoRail::from_dto(&dto(
            &[
                ("done item", TodoStatusDto::Completed),
                ("dropped item", TodoStatusDto::Cancelled),
            ],
            TodoCompletionDto::Final {
                outcome: TodoOutcomeKindDto::Completed,
                completed: 1,
                cancelled: 1,
            },
        )));
        terminal
            .draw(|frame| view::draw(&mut model, frame))
            .unwrap();
        let rows = screen(&terminal);
        let header = row_of(&rows, "TODOS").expect("final header is drawn");
        assert!(rows[header].contains("2 complete"), "{}", rows[header]);
        assert!(!rows[header].contains("open"), "{}", rows[header]);
    }

    /// Quarantined state warns and never renders as an empty checklist.
    #[test]
    fn unavailable_state_draws_a_warning_row() {
        let (mut model, mut terminal) = test_model(70, 24);
        model.todo_rail_override = Some(TodoRail::unavailable("quarantined envelope"));
        terminal
            .draw(|frame| view::draw(&mut model, frame))
            .unwrap();
        let rows = screen(&terminal);
        assert!(
            row_of(&rows, "persisted data preserved").is_some(),
            "{rows:?}"
        );
    }

    fn graph_with_nodes(agent: &str, statuses: &[ExecutionStatus]) -> WorkSnapshot {
        let mut state = WorkState::default();
        let mut graph = WorkGraph::new("checklist", Some(agent.to_string()), GraphMode::Advisory);
        for (index, status) in statuses.iter().enumerate() {
            let mut node = WorkNode::new(format!("n{index}"), format!("Item {index}"));
            node.status = *status;
            graph.view_order.push(node.id);
            graph.nodes.insert(node.id, node);
        }
        let id = graph.id;
        state.graphs.insert(id, graph);
        state.active_graph_by_agent.insert(agent.to_string(), id);
        WorkSnapshot::new("session", 0, state)
    }
}
