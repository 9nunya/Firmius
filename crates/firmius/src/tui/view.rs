//! Rendering: model in, frame out. Layout measurement may normalize viewport state.

use std::collections::HashMap;

use ratatui::Frame;
use ratatui::layout::{Constraint, Layout, Position, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph};
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

use super::markdown;
use super::modal::DeckSurface;
use super::model::{
    CompactionPhase, Item, Model, RenderCache, SearchState, ToolState, is_system_message,
    visible_message_text,
};
use super::present;
use super::presentation::{
    DisclosurePolicy, HitSubtarget, SemanticId, TranscriptEvent, disclosure_decision,
};
use super::run;
use super::style;
use super::theme::Theme;
use super::todo;
use super::work;
use firmius_core::PermissionMode;

const WELCOME_LOGO: &str = r#"
███████╗██╗██████╗ ███╗   ███╗██╗██╗   ██╗███████╗
██╔════╝██║██╔══██╗████╗ ████║██║██║   ██║██╔════╝
█████╗  ██║██████╔╝██╔████╔██║██║██║   ██║███████╗
██╔══╝  ██║██╔══██╗██║╚██╔╝██║██║██║   ██║╚════██║
██║     ██║██║  ██║██║ ╚═╝ ██║██║╚██████╔╝███████║
╚═╝     ╚═╝╚═╝  ╚═╝╚═╝     ╚═╝╚═╝ ╚═════╝ ╚══════╝
"#;
fn thinking_is_active(item: &Item, model: &Model, index: usize, transcript_len: usize) -> bool {
    matches!(item, Item::Thinking { .. }) && model.busy && index + 1 == transcript_len
}

fn glint_phase(tick: usize) -> f32 {
    // Run a short one-way pass, then return zero intensity for the remainder
    // of the cycle. This keeps the base text stable instead of recoloring the
    // entire presenter/thinking block continuously.
    const CYCLE: usize = 120;
    const PASS: usize = 28;
    let tick = tick % CYCLE;
    if tick < PASS {
        tick as f32 / (PASS - 1) as f32
    } else {
        2.0
    }
}

/// Apply the narrow eased glint to a title without changing its text width.
fn glint_title(
    text: &str,
    base: Style,
    theme: &super::theme::Theme,
    tick: usize,
) -> Vec<Span<'static>> {
    let len = text.chars().count().max(1);
    let phase = glint_phase(tick);
    text.chars()
        .enumerate()
        .map(|(i, ch)| {
            Span::styled(
                ch.to_string(),
                base.fg(super::theme::phrase_glint_at(
                    theme,
                    base.fg.unwrap_or(theme.fg),
                    phase,
                    len,
                    i,
                )),
            )
        })
        .collect()
}

fn glint_content_len(line: &Line<'static>) -> usize {
    line.spans
        .iter()
        .filter(|source| source.content.chars().any(char::is_alphanumeric))
        .map(|source| source.content.chars().count())
        .sum()
}

fn glint_line(
    line: &mut Line<'static>,
    theme: &super::theme::Theme,
    phase: f32,
    total_len: usize,
    content_offset: usize,
) -> usize {
    let mut spans = Vec::new();
    let mut offset = content_offset;
    let mut content_started = false;
    for source in line.spans.iter() {
        // Keep structural prefixes (gutter, tree bars, and status glyphs)
        // stable; animate only the actual title/content spans.
        if !content_started && source.content.chars().all(|ch| !ch.is_alphanumeric()) {
            spans.push(source.clone());
            continue;
        }
        content_started = true;
        spans.extend(source.content.chars().enumerate().map(|(index, ch)| {
            Span::styled(
                ch.to_string(),
                source.style.fg(super::theme::phrase_glint_at(
                    theme,
                    source.style.fg.unwrap_or(theme.fg),
                    phase,
                    total_len,
                    offset + index,
                )),
            )
        }));
        offset += source.content.chars().count();
    }
    *line = Line::from(spans).style(line.style);
    offset - content_offset
}

/// Return the compact permission mode name that fits in the status bar. The
/// policy editor carries the longer explanations; the bar only needs to make
/// the active mode (and a missing policy) immediately visible.
fn permission_mode_status(model: &Model) -> String {
    let mode = model.permission_policy.as_ref().map(|policy| &policy.mode);
    let label = match mode {
        Some(PermissionMode::Default) => "Default".to_string(),
        Some(PermissionMode::Auto) => "Auto".to_string(),
        Some(PermissionMode::Yolo) => "YOLO".to_string(),
        Some(PermissionMode::Custom(name)) => format!("Custom: {name}"),
        None => "unavailable".to_string(),
    };

    format!("perm: {label}")
}

pub fn draw(model: &mut Model, frame: &mut Frame) {
    let area = frame.area();
    // Paint the complete viewport. Besides giving themes a real canvas rather
    // than depending on the terminal's default color, this prevents stale
    // cells during fast resizes and makes overlays feel like part of one UI.
    frame.render_widget(Block::new().style(Style::new().bg(model.theme.bg)), area);
    // Each composer row reserves two cells for its prompt prefix in addition
    // to the two horizontal border cells. Keep this width in sync with the
    // cursor calculation in `draw_composer`; using the outer width here lets
    // the first row run past the actual text area before it wraps.
    let composer_width = area.width.saturating_sub(4) as usize;
    let composer_lines = model
        .composer
        .lines_with_width(&model.pastes, composer_width);
    // The composer has one unboxed status row above its input rows.  Prefixes
    // are drawn inside the input renderer and therefore do not need a second
    // row of border chrome.
    let requested_composer_h = (composer_lines.len().clamp(1, 6).saturating_add(2) as u16)
        .max(2)
        .min(area.height / 2 + 3);
    // Daemon-backed sessions do not expose live Agent handles. Use the model
    // projection so mailbox messages and optimistic queue echoes reserve room
    // and remain visible while the active turn is running.
    let pending_h = model.pending_user_messages().len() as u16;
    let pending_h = pending_h.min(area.height.saturating_sub(1));
    let composer_h = requested_composer_h
        .min(area.height.saturating_sub(pending_h.saturating_add(4)))
        .max(3);
    // The transcript is the flexible region and now owns the durable work
    // view (a live run or the task list). Directly beneath the busy phrase row
    // sits the native todo rail, which replaces the old work rail: todos are
    // the focused agent's own checklist, while work is session-durable and
    // belongs in the reading flow.
    let todo_rail = model.visible_todo_rail();
    // A confirmed-final rail stays up briefly, then collapses so a finished
    // checklist never occupies a permanent row; the bottom bar keeps the count.
    let todo_collapsed_final = model.todo_rail_collapsed();
    let requested_todo_h = if todo_collapsed_final {
        0
    } else {
        todo_rail.as_ref().map(todo_rail_height).unwrap_or(0)
    };
    let protected = composer_h.saturating_add(pending_h).saturating_add(3);
    // The composer, context bar, bottom bar, and at least one transcript row
    // are protected. A rail that cannot get its header plus one item collapses
    // to a one-line summary (or the bottom-bar count).
    let todo_h = requested_todo_h.min(area.height.saturating_sub(protected).saturating_sub(1));
    let todo_collapsed = requested_todo_h > 0 && todo_h < requested_todo_h;
    // The busy phrase row is a peer of the todo rail, not a fallback for its
    // absence: while a turn is live it stays up even when the rail has rows,
    // so the phrase row can never silently vanish mid-turn.
    let busy_h = u16::from(effective_busy(model) && area.height >= 8);
    let chunks = Layout::vertical([
        Constraint::Min(1),
        Constraint::Length(busy_h),
        Constraint::Length(todo_h),
        Constraint::Length(pending_h),
        Constraint::Length(composer_h),
        Constraint::Length(1),
        Constraint::Length(1),
    ])
    .split(area);
    if let Some(rail) = &todo_rail
        && todo_h > 0
    {
        if todo_collapsed {
            draw_todo_summary(model, rail, frame, chunks[2]);
        } else {
            draw_todo_rail(model, rail, frame, chunks[2]);
        }
    }

    if busy_h > 0 {
        draw_busy_row(model, frame, chunks[1]);
    }

    draw_transcript(model, frame, chunks[0]);
    draw_pending_messages(model, frame, chunks[3]);
    // Modal inputs own the foreground editing surface. Keep the area in the
    // layout so the modal remains anchored consistently, but do not render the
    // background composer underneath it.
    if model.modal.is_none() {
        draw_composer(model, frame, chunks[4], &composer_lines);
    }
    draw_completion(model, frame, chunks[4]);
    draw_context_bar(model, frame, chunks[5]);
    // Only when the rail itself has no room (or has collapsed after finishing)
    // does the bottom bar carry the todo count.
    let todo_footer = match (&todo_rail, todo_h) {
        (Some(rail), 0) if !rail.is_empty() => Some(rail),
        _ => None,
    };
    draw_bottom_bar(model, frame, chunks[6], todo_footer);
    draw_modal(model, frame, chunks[4]);
}

/// A busy turn without a graph still deserves a truthful, compact row directly
/// above the composer.  It intentionally consumes no permanent idle space.
fn draw_busy_row(model: &Model, frame: &mut Frame, area: Rect) {
    if area.height == 0 {
        return;
    }
    let phrase = if model.live_phrase == "idle" {
        "Thinking…"
    } else {
        model.live_phrase.as_str()
    };
    let phrase = truncate_width(phrase, area.width.saturating_sub(12) as usize);
    let mut spans = vec![
        Span::styled(
            style::SPINNER[model.tick_phase % style::SPINNER.len()],
            style::spinner(&model.theme),
        ),
        Span::raw(" "),
    ];
    spans.extend(glint_title(
        &phrase,
        Style::new().fg(model.theme.fg),
        &model.theme,
        model.tick_phase,
    ));
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}

/// Rows the native todo rail wants: a header, the budgeted item rows, and one
/// overflow row; a single warning row for degraded state; zero when there is
/// nothing to show.
fn todo_rail_height(rail: &todo::TodoRail) -> u16 {
    if rail.warning().is_some() {
        return 1;
    }
    if rail.is_empty() {
        return 0;
    }
    let (rows, overflow) = rail.visible(todo::RAIL_MAX_ROWS);
    (1 + rows.len() + usize::from(overflow > 0)) as u16
}

/// The native todo rail: the focused agent's own checklist, directly beneath
/// the busy phrase row.  It is intentionally bounded and never crowds the
/// composer, context bar, or transcript.
fn draw_todo_rail(model: &Model, rail: &todo::TodoRail, frame: &mut Frame, area: Rect) {
    if area.height == 0 {
        return;
    }
    let theme = &model.theme;
    if let Some(warning) = rail.warning() {
        frame.render_widget(
            Paragraph::new(Line::styled(
                truncate_width(&format!("  {warning}"), area.width as usize),
                style::note(theme),
            )),
            area,
        );
        return;
    }
    let header_style = if rail.completion_confirmed() {
        style::work_status(theme, firmius_core::ExecutionStatus::Succeeded)
    } else {
        style::bar(theme)
    };
    let (total, completed, cancelled, active) = rail.progress_counts();
    let meter = present::progress_bar((completed + cancelled) as u64, total as u64, 10);
    let header = format!(
        "{}  {} {}/{} settled{}{}",
        rail.headline(),
        meter,
        completed + cancelled,
        total,
        if active > 0 {
            format!(" · {active} active")
        } else {
            String::new()
        },
        if rail.blocked() > 0 {
            format!(" · {} blocked", rail.blocked())
        } else {
            String::new()
        }
    );
    let mut lines = vec![Line::styled(
        truncate_width(&header, area.width as usize),
        header_style,
    )];
    let (rows, overflow) = rail.visible(todo::RAIL_MAX_ROWS);
    for row in &rows {
        let mut spans = vec![
            Span::styled(
                format!("  {} ", row.presentation.glyph()),
                style::todo_presentation(theme, row.presentation),
            ),
            Span::styled(
                truncate_width(&row.title, area.width.saturating_sub(6) as usize),
                style::assistant(theme),
            ),
        ];
        if let Some(detail) = row.detail() {
            spans.push(Span::styled(format!("  ·  {detail}"), style::dim(theme)));
        }
        lines.push(Line::from(spans));
    }
    if overflow > 0 {
        lines.push(Line::styled(
            format!("  … {overflow} more · {} done", rail.completed()),
            style::dim(theme),
        ));
    }
    frame.render_widget(Paragraph::new(lines), area);
}

/// Collapsed rail: one truthful line when the terminal cannot spare the rows.
fn draw_todo_summary(model: &Model, rail: &todo::TodoRail, frame: &mut Frame, area: Rect) {
    if area.height == 0 {
        return;
    }
    let text = rail.summary_line();
    frame.render_widget(
        Paragraph::new(Line::styled(
            truncate_width(&format!("  {text}"), area.width as usize),
            style::bar(&model.theme),
        )),
        area,
    );
}

/// Apply a cue after wrapping so the effect cannot alter line count, widths,
/// or hit targets.  The range is character-based and is local to the event's
/// text; unrelated transcript lines retain their existing styles.
fn apply_arrival_glint(
    lines: &mut [Line<'static>],
    span: std::ops::Range<usize>,
    revision: u64,
    theme: &super::theme::Theme,
) {
    let mut offset = 0usize;
    for line in lines {
        let mut styled = Vec::new();
        for source in line.spans.iter() {
            let mut chunk = String::new();
            let chunk_style = source.style;
            for ch in source.content.chars() {
                let index = offset;
                offset += 1;
                let active = span.contains(&index);
                if active {
                    if !chunk.is_empty() {
                        styled.push(Span::styled(std::mem::take(&mut chunk), chunk_style));
                    }
                    let glint_style = chunk_style
                        .fg(super::theme::arrival_glint_at(
                            theme,
                            revision,
                            span.len(),
                            index,
                        ))
                        .add_modifier(Modifier::BOLD);
                    styled.push(Span::styled(ch.to_string(), glint_style));
                } else {
                    chunk.push(ch);
                }
            }
            if !chunk.is_empty() {
                styled.push(Span::styled(chunk, chunk_style));
            }
        }
        if !styled.is_empty() {
            *line = Line::from(styled).style(line.style);
        }
        // Wrapped rows are distinct visual text, so keep the newline out of
        // the character coordinate space used by an event cue.
    }
}

/// Rows a structured run may occupy inside the transcript. The run keeps its
/// previous ceiling so a wide fan-out cannot turn the conversation into a
/// status table; the transcript scrolls, so this is a density budget rather
/// than a layout constraint.
const RUN_BLOCK_ROWS: usize = 12;
/// Rows a plain task list may occupy inside the transcript.
const WORK_BLOCK_ROWS: usize = 5;
/// A worker can explain its current node without monopolizing the transcript.
const NODE_TODO_ROWS: usize = 3;
/// Nested ledgers are extra detail, but remain bounded across a wide fan-out.
const WORK_TODO_ROWS: usize = 12;

/// Allocate nested todo rows fairly: every eligible worker gets one row
/// before any worker gets a second or third.
fn nested_todo_budgets<'a>(
    model: &Model,
    agents: impl IntoIterator<Item = &'a str>,
) -> HashMap<String, usize> {
    let eligible = agents
        .into_iter()
        .filter_map(|agent_id| {
            let rail = model.todo_rail_for(agent_id);
            (!rail.rows().is_empty()).then(|| (agent_id.to_string(), rail.rows().len()))
        })
        .collect::<Vec<_>>();
    let mut budgets = HashMap::new();
    let mut remaining = WORK_TODO_ROWS;
    for round in 0..NODE_TODO_ROWS {
        for (agent_id, rows) in &eligible {
            if remaining == 0 {
                return budgets;
            }
            if *rows > round {
                *budgets.entry(agent_id.clone()).or_default() += 1;
                remaining -= 1;
            }
        }
    }
    budgets
}

fn append_nested_todos(
    rows: &mut Vec<(Line<'static>, bool)>,
    model: &Model,
    theme: &Theme,
    agent_id: &str,
    budget: usize,
    indent: usize,
) {
    if budget == 0 {
        return;
    }
    let rail = model.todo_rail_for(agent_id);
    let (visible, overflow) = rail.visible(budget);
    for item in visible {
        let detail = item.detail();
        let mut spans = vec![
            Span::raw(format!("{}│ ", "  ".repeat(indent))),
            Span::styled(
                format!("{} ", item.presentation.glyph()),
                style::todo_presentation(theme, item.presentation),
            ),
            Span::styled(item.title, style::assistant(theme)),
        ];
        if let Some(detail) = detail {
            spans.push(Span::styled(format!(" · {detail}"), style::dim(theme)));
        }
        rows.push((Line::from(spans), false));
    }
    if overflow > 0 {
        rows.push((
            Line::styled(
                format!("{}│ … {overflow} more", "  ".repeat(indent)),
                style::dim(theme),
            ),
            false,
        ));
    }
}

/// Render the focused agent's durable work (a live run or a plain task list)
/// as transcript content.
///
/// Returns the rendered lines plus the indices (within the block) of rows that
/// are currently executing, so the caller can glint them without re-deriving
/// state. Nothing is reconstructed from task tool output: the canonical
/// snapshot is the only input.
#[allow(clippy::type_complexity)]
fn work_block(model: &Model, width: u16, theme: &Theme) -> (Vec<Line<'static>>, Vec<usize>) {
    // (line, currently executing) so the caller can glint live rows after the
    // block has been wrapped to the reading column.
    let mut rows: Vec<(Line<'static>, bool)> = Vec::new();
    if let Some(live) = model.live_run() {
        let run_rows = run::rows(&live, &model.run_liveness, RUN_BLOCK_ROWS);
        let todo_budgets = nested_todo_budgets(
            model,
            run_rows
                .iter()
                .filter(|row| row.state == firmius_core::work::LiveState::Running)
                .filter_map(|row| row.agent_id.as_deref()),
        );
        for row in run_rows {
            let status = match row.state {
                firmius_core::work::LiveState::Waiting => firmius_core::ExecutionStatus::Pending,
                firmius_core::work::LiveState::Running => firmius_core::ExecutionStatus::Running,
                firmius_core::work::LiveState::Succeeded => {
                    firmius_core::ExecutionStatus::Succeeded
                }
                firmius_core::work::LiveState::Failed => firmius_core::ExecutionStatus::Failed,
                firmius_core::work::LiveState::Stuck => firmius_core::ExecutionStatus::Blocked,
            };
            let mut spans = vec![
                Span::raw("  ".repeat(row.indent + 1)),
                Span::styled(format!("{} ", row.glyph), style::work_status(theme, status)),
                Span::styled(row.text, style::assistant(theme)),
            ];
            if let Some(detail) = row.detail {
                spans.push(Span::styled(format!(" · {detail}"), style::dim(theme)));
            }
            let running = row.state == firmius_core::work::LiveState::Running;
            rows.push((Line::from(spans), running));
            if running && let Some(agent_id) = row.agent_id.as_deref() {
                append_nested_todos(
                    &mut rows,
                    model,
                    theme,
                    agent_id,
                    todo_budgets.get(agent_id).copied().unwrap_or(0),
                    row.indent + 2,
                );
            }
        }
        return wrap_rows(rows, width);
    }

    let view = model.work_view(WORK_BLOCK_ROWS);
    if view.lines.is_empty() && view.overflow == 0 {
        if view.all_completed {
            let title = view.graph_title.as_deref().unwrap_or("work");
            return wrap_rows(
                vec![(
                    Line::styled(
                        format!("  ✓ {title} · {} completed", view.completed),
                        style::work_status(theme, firmius_core::ExecutionStatus::Succeeded),
                    ),
                    false,
                )],
                width,
            );
        }
        return (Vec::new(), Vec::new());
    }

    // A distinct heading keeps the durable task list separable from the native
    // Todos rail, which is a different domain with different ownership.
    let title = view.graph_title.as_deref().unwrap_or("work");
    rows.push((
        Line::from(vec![
            Span::styled("  WORK ", style::bar(theme)),
            Span::styled(title.to_string(), style::dim(theme)),
            Span::styled(format!("  ·  {} done", view.completed), style::dim(theme)),
            Span::styled(
                view.coordination_summary
                    .as_ref()
                    .map(|summary| format!("  ·  {summary}"))
                    .unwrap_or_default(),
                style::dim(theme),
            ),
        ]),
        false,
    ));
    if let Some(context) = &view.parent_context {
        rows.push((
            Line::styled(format!("  ↑ {context}"), style::dim(theme)),
            false,
        ));
    }
    let todo_budgets = nested_todo_budgets(
        model,
        view.lines
            .iter()
            .filter(|row| row.presentation == work::WorkPresentation::Running)
            .filter_map(|row| row.agent_id.as_deref()),
    );
    for row in &view.lines {
        let presentation = model.work_presentation(row);
        // Preserve gate-aware glyphs such as `✓?` and `✗~` for ordinary
        // statuses; only delegated startup replaces the canonical glyph.
        let glyph = if presentation == work::WorkPresentation::Starting {
            presentation.glyph()
        } else {
            row.glyph
        };
        let running = presentation == work::WorkPresentation::Running;
        let mut spans = vec![Span::styled(
            format!("  {} ", glyph),
            style::work_presentation(theme, presentation),
        )];
        if running {
            spans.extend(glint_title(
                &row.title,
                style::assistant(theme),
                theme,
                model.tick_phase,
            ));
        } else {
            spans.push(Span::styled(row.title.clone(), style::assistant(theme)));
        }
        let fallback_detail = match row.status {
            firmius_core::ExecutionStatus::Failed => Some("retry or inspect"),
            firmius_core::ExecutionStatus::Blocked => Some("inspect blockers"),
            firmius_core::ExecutionStatus::Interrupted => Some("resume or retry"),
            _ => None,
        };
        if let Some(detail) = row.detail.as_deref().or(fallback_detail) {
            spans.push(Span::styled(format!("  ·  {detail}"), style::dim(theme)));
        }
        rows.push((Line::from(spans), running));
        if running && let Some(agent_id) = row.agent_id.as_deref() {
            append_nested_todos(
                &mut rows,
                model,
                theme,
                agent_id,
                todo_budgets.get(agent_id).copied().unwrap_or(0),
                2,
            );
        }
    }
    if view.overflow > 0 {
        rows.push((
            Line::styled(
                format!("  … {} more · {} completed", view.overflow, view.completed),
                style::dim(theme),
            ),
            false,
        ));
    }
    wrap_rows(rows, width)
}

/// Wrap the work block to the reading column and report the line index of each
/// executing row so animation cannot drift from the rendered layout.
fn wrap_rows(rows: Vec<(Line<'static>, bool)>, width: u16) -> (Vec<Line<'static>>, Vec<usize>) {
    let mut lines = Vec::new();
    let mut running = Vec::new();
    for (line, is_running) in rows {
        if is_running {
            running.push(lines.len());
        }
        lines.extend(wrap_lines(vec![line], width));
    }
    (lines, running)
}

/// Resolve the host process for a bash call. Completed calls carry their id in
/// the tool result. While a command is still running, however, that result has
/// not arrived yet, so correlate the command with the live PTY's command line.
/// This is deliberately kept in the view layer: the core tool protocol does
/// not expose a process id until the bash tool has returned.
fn bash_proc_id(
    model: &Model,
    agent_id: &str,
    args: &str,
    result: Option<&str>,
) -> Option<firmius_core::ProcId> {
    if let Some(id) = present::proc_id_from_result(result) {
        return Some(id);
    }
    let command = present::bash_cmdline(args)?;
    if command.is_empty() {
        return None;
    }
    let agent = model.agents.get(agent_id)?;
    agent
        .host()
        .list_info()
        .into_iter()
        .find(|info| info.cmdline.ends_with(&command))
        .map(|info| info.id)
}

fn composer_scroll_offset(cursor_row: usize, line_count: usize, visible_rows: usize) -> usize {
    let max_offset = line_count.saturating_sub(visible_rows);
    cursor_row
        .saturating_sub(visible_rows.saturating_sub(1))
        .min(max_offset)
}

fn truncate_width(text: &str, width: usize) -> String {
    if width == 0 {
        return String::new();
    }
    if text.width() <= width {
        return text.to_string();
    }
    let mut out = String::new();
    let mut used = 0;
    for ch in text.chars() {
        let cw = ch.width().unwrap_or(1);
        if used + cw + 1 > width {
            break;
        }
        out.push(ch);
        used += cw;
    }
    out.push('…');
    out
}

fn chip(
    text: impl Into<String>,
    fg: ratatui::style::Color,
    bg: ratatui::style::Color,
) -> Span<'static> {
    Span::styled(
        format!(" {text} ", text = text.into()),
        Style::new().fg(fg).bg(bg),
    )
}

fn padded_plain(width: usize, text: &str) -> Line<'static> {
    let mut padded = text.to_string();
    let used = padded.width();
    if used < width {
        padded.push_str(&" ".repeat(width - used));
    }
    Line::styled(padded, Style::default())
}

fn draw_pending_messages(model: &Model, frame: &mut Frame, area: Rect) {
    let messages = model.pending_user_messages();
    if messages.is_empty() {
        return;
    }
    let width = area.width as usize;
    let lines = messages
        .iter()
        .enumerate()
        .map(|(i, message)| {
            Line::styled(
                truncate_width(&format!("{}  {}", i + 1, message), width),
                style::note(&model.theme),
            )
        })
        .collect::<Vec<_>>();
    frame.render_widget(Paragraph::new(lines), area);
}

/// The open modal, anchored as a full-width bottom deck above the composer.
/// Rendered last so it overlays transcript and completion; capped to half the
/// body so the live transcript remains visible.
fn draw_modal(model: &Model, frame: &mut Frame, composer_area: Rect) {
    let Some(modal) = &model.modal else {
        return;
    };
    draw_modal_surface(modal.as_ref(), frame, composer_area, &model.theme);
}

fn draw_modal_surface(
    modal: &dyn super::modal::ModalSurface,
    frame: &mut Frame,
    composer_area: Rect,
    theme: &super::theme::Theme,
) {
    let width = composer_area.width;
    // Keep at least half of the transcript/body visible while a deck is open.
    let max_height = (composer_area.y / 2).max(1);
    let height = modal.height_hint(width).min(max_height);
    if height < 3 || width == 0 {
        return;
    }
    let area = Rect {
        x: composer_area.x,
        y: composer_area.y.saturating_sub(height),
        width,
        height,
    };
    // Clear the full deck rectangle first so transcript text cannot bleed
    // through sparse modal content.
    frame.render_widget(Clear, area);
    modal.render(area, frame, theme);
    if let Some((cx, cy)) = modal.cursor(area) {
        frame.set_cursor_position(Position { x: cx, y: cy });
    }
}

fn draw_completion(model: &Model, frame: &mut Frame, composer_area: Rect) {
    let theme = &model.theme;
    if model.modal.is_some() {
        return;
    }
    let Some(completion) = &model.completion else {
        return;
    };
    let height = (completion.items.len() as u16).min(8).saturating_add(3);
    let y = composer_area.y.saturating_sub(height);
    let area = Rect {
        x: composer_area.x,
        y,
        width: composer_area.width,
        height,
    };
    let start = completion.selected.saturating_sub(7);
    let inner_width = area.width as usize;
    let label_width = completion
        .items
        .iter()
        .skip(start)
        .take(8)
        .map(|item| item.label.width())
        .max()
        .unwrap_or(0)
        .min(inner_width.saturating_sub(8));
    let lines = completion
        .items
        .iter()
        .enumerate()
        .skip(start)
        .take(8)
        .map(|(index, item)| {
            let marker = if index == completion.selected {
                "› "
            } else {
                "  "
            };
            let style = if index == completion.selected {
                style::user(theme).bg(theme.selection_bg)
            } else {
                style::bar(theme)
            };
            let detail_style = if index == completion.selected {
                style::dim(theme).bg(theme.selection_bg)
            } else {
                style::dim(theme)
            };
            let available_detail = inner_width.saturating_sub(2 + label_width + 2);
            Line::from(vec![
                Span::styled(marker, style),
                Span::styled(
                    format!("{:<width$}", item.label, width = label_width),
                    style,
                ),
                Span::styled("  ", style),
                Span::styled(truncate_width(&item.detail, available_detail), detail_style),
            ])
        })
        .collect::<Vec<_>>();
    let rows_area = DeckSurface::render("Completions", area, frame, theme);
    let rows_area = Rect {
        x: rows_area.x,
        y: rows_area.y,
        width: rows_area.width,
        height: rows_area.height.saturating_sub(1),
    };
    frame.render_widget(Paragraph::new(lines), rows_area);
    if rows_area.height > 0 {
        frame.render_widget(
            Paragraph::new(Line::styled(
                "  ↑↓ choose · tab/enter accept · esc close",
                style::dim(theme),
            )),
            Rect {
                x: rows_area.x,
                y: area.y + area.height.saturating_sub(1),
                width: rows_area.width,
                height: 1,
            },
        );
    }
}

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

/// Wrap text to `width`, preferring whitespace boundaries and hard-breaking
/// only words which cannot fit on one row.
fn wrap(text: &str, style: Style, width: u16) -> Vec<Line<'static>> {
    text.split('\n')
        .flat_map(|line| wrap_line(Line::styled(line.to_string(), style), width))
        .collect()
}

#[derive(Clone)]
struct StyledChar {
    text: String,
    width: usize,
    style: Style,
    whitespace: bool,
}

fn line_from_chars(chars: Vec<StyledChar>, line_style: Style) -> Line<'static> {
    Line::from(
        chars
            .into_iter()
            .map(|ch| Span::styled(ch.text, ch.style))
            .collect::<Vec<_>>(),
    )
    .style(line_style)
}

fn wrap_line(line: Line<'static>, width: u16) -> Vec<Line<'static>> {
    let width = usize::from(width).max(1);
    let line_style = line.style;
    // User cards have already been padded to their content width. Do not
    // interpret that deliberate background fill as wrapping whitespace.
    if line.spans.iter().all(|span| span.style.bg.is_some()) && line.width() == width {
        return vec![line];
    }
    let chars = line
        .spans
        .into_iter()
        .flat_map(|span| {
            let style = span.style;
            span.content
                .chars()
                .map(|ch| {
                    let ch = if ch == '\t' { ' ' } else { ch };
                    StyledChar {
                        text: ch.to_string(),
                        width: ch.width().unwrap_or(1),
                        style,
                        whitespace: ch.is_whitespace(),
                    }
                })
                .collect::<Vec<_>>()
        })
        .collect::<Vec<_>>();

    if chars.is_empty() {
        return vec![Line::from(Vec::<Span<'static>>::new()).style(line_style)];
    }

    let mut rows = Vec::new();
    let mut current = Vec::new();
    let mut current_width = 0;
    let mut pending_space = Vec::new();
    let mut index = 0;
    while index < chars.len() {
        if chars[index].whitespace {
            pending_space.push(chars[index].clone());
            index += 1;
            continue;
        }

        let start = index;
        while index < chars.len() && !chars[index].whitespace {
            index += 1;
        }
        let word = &chars[start..index];
        let word_width = word.iter().map(|ch| ch.width).sum::<usize>();
        let space_width = pending_space.iter().map(|ch| ch.width).sum::<usize>();

        // Whitespace at a wrap boundary is a separator, not content to carry
        // onto the next row. This also avoids rows beginning with spaces.
        if !current.is_empty() && current_width + space_width + word_width <= width {
            current.extend(pending_space.drain(..));
            current_width += space_width;
        } else if !current.is_empty() {
            rows.push(line_from_chars(std::mem::take(&mut current), line_style));
            current_width = 0;
            pending_space.clear();
        } else if rows.is_empty() && !pending_space.is_empty() && word_width + space_width <= width
        {
            // Preserve intentional indentation at the start of a logical
            // line (notably the nested-tool `  │ ` prefix). Separators after
            // a wrap are discarded, but source-line indentation is content.
            for ch in pending_space.drain(..) {
                let ch_width = ch.width;
                if current_width > 0 && current_width + ch.width > width {
                    rows.push(line_from_chars(std::mem::take(&mut current), line_style));
                    current_width = 0;
                }
                current.push(ch);
                current_width += ch_width;
            }
        } else {
            pending_space.clear();
        }

        // A long word is split into width-sized chunks. Keep the final,
        // possibly short chunk in `current` so a following word can share it
        // only when it fits (there is no implicit whitespace in that case).
        for ch in word {
            if current_width > 0 && current_width + ch.width > width {
                rows.push(line_from_chars(std::mem::take(&mut current), line_style));
                current_width = 0;
            }
            current.push(ch.clone());
            current_width += ch.width;
        }
        pending_space.clear();
    }

    // Preserve an explicitly blank logical line, but do not render trailing
    // wrapping whitespace as visible padding (user cards add their own fill).
    if !current.is_empty() {
        rows.push(line_from_chars(current, line_style));
    } else if rows.is_empty() {
        rows.push(Line::from(Vec::<Span<'static>>::new()).style(line_style));
    }
    rows
}

fn item_lines(
    model: &Model,
    item: &Item,
    width: u16,
    delegate_child: Option<&str>,
) -> Vec<Line<'static>> {
    let theme = &model.theme;
    match item {
        Item::User(t) => wrap(visible_message_text(t), style::user_block(theme), width),
        Item::Text(t) => markdown::render(visible_message_text(t), style::assistant(theme), theme),
        Item::AgentMessage { sender_id, text } => {
            let label = format!("from {sender_id}: {}", visible_message_text(text));
            wrap(&label, style::note(theme), width)
        }
        Item::SystemMessage { text } => wrap(visible_message_text(text), style::dim(theme), width),
        Item::AssignmentCompletion {
            child_agent_id,
            text,
            ..
        } => {
            let label = format!("assignment from {child_agent_id}: {text}");
            wrap(&label, style::assistant(theme), width)
        }
        Item::Thinking { text, .. } => markdown::render(text, style::thinking(theme), theme),
        Item::ToolCall {
            name,
            args,
            result,
            state,
            stream_id,
            ..
        } => {
            let related_intent = match name.as_str() {
                "bash" => serde_json::from_str::<serde_json::Value>(args)
                    .ok()
                    .and_then(|value| {
                        value
                            .get("proc_id")
                            .and_then(|v| v.as_str())
                            .map(str::to_owned)
                    })
                    .and_then(|id| model.proc_intents.get(&id).cloned()),
                "delegate" => serde_json::from_str::<serde_json::Value>(args)
                    .ok()
                    .and_then(|value| {
                        value
                            .get("delegate_id")
                            .and_then(|v| v.as_str())
                            .map(str::to_owned)
                    })
                    .and_then(|id| model.delegate_intents.get(&id).cloned()),
                _ => None,
            };
            let tail = if name == "bash" && present::bash_mode_shows_output(args) {
                bash_proc_id(model, &model.focused_id, args, result.as_deref())
                    .and_then(|id| model.host_tails.get(&id))
                    .map(String::as_str)
            } else if matches!(name.as_str(), "read" | "list" | "grep" | "glob") {
                result.as_deref()
            } else {
                None
            };
            let nested =
                delegate_child.map(|child_id| nested_tool_lines(model, child_id, width, 3));
            let mut lines = match name.as_str() {
                "bash" => present::bash_lines_progressive(
                    args,
                    state,
                    tail,
                    width,
                    theme,
                    related_intent.as_deref(),
                ),
                "delegate" => present::delegate_lines_progressive(
                    args,
                    state,
                    width,
                    theme,
                    related_intent.as_deref(),
                ),
                "message" => {
                    present::message_lines_progressive(args, result.as_deref(), state, width, theme)
                }
                "memory" => {
                    present::memory_lines_progressive(args, result.as_deref(), state, width, theme)
                }
                "task" => {
                    present::task_lines_progressive(args, result.as_deref(), state, width, theme)
                }
                "workflow" => present::workflow_lines_progressive(
                    args,
                    result.as_deref(),
                    state,
                    width,
                    theme,
                ),
                // Keep the actual patch visible while it streams. Very large
                // completed patches use the same bounded presenter so a
                // single edit cannot allocate an unbounded transcript card.
                "edit" if matches!(state, ToolState::Preparing(_) | ToolState::Running(_)) => {
                    present::edit_lines_compact(args, state, width, theme, 8)
                }
                "edit" if args.len() > 64 * 1024 => {
                    present::edit_lines_compact(args, state, width, theme, 8)
                }
                "edit" => present::tool_lines(name, args, state, tail, width, theme),
                _ => present::tool_lines(name, args, state, tail, width, theme),
            };
            if let Some(nested) = nested.as_deref() {
                lines.extend(nested.iter().cloned().map(|mut line| {
                    let mut spans = vec![Span::styled("  │ ", style::dim(theme))];
                    spans.append(&mut line.spans);
                    Line::from(spans)
                }));
            }
            // Control-plane execution state is an intentionally compact,
            // non-expandable continuation. Ordinary tools remain concise;
            // only calls with typed queue/permission state get this row.
            if let Some(execution_id) = stream_id.as_deref()
                && let Some(execution) = model.tool_execution.get(execution_id)
            {
                lines.extend(present::tool_execution_lines(execution, width, theme));
            }
            lines
        }
        Item::Note(t) => wrap(visible_message_text(t), style::note(theme), width),
        Item::WebSearch { action, state, .. } => present::search_lines(action, state, width, theme),
        Item::Compaction(item) => present::compaction_lines(item, width, theme),
    }
}

fn quick_group_kind(item: &Item) -> Option<&'static str> {
    match item {
        // Grouping starts at preparation, not completion: the group row is
        // stable while members stream, run, and settle, so a read followed
        // by a second preparing read evolves one group in place.
        Item::ToolCall { name, .. } if name == "read" => Some("read"),
        Item::ToolCall { name, .. } if name == "list" => Some("list"),
        _ => None,
    }
}

/// Render one tool call with a caller-owned output window. Expanded events
/// must show visibly more retained content than the collapsed three-row
/// default; this is what makes `▸ expand` a real control.
fn item_lines_with_window(
    model: &Model,
    item: &Item,
    width: u16,
    delegate_child: Option<&str>,
    tail_lines: usize,
) -> Vec<Line<'static>> {
    let Item::ToolCall {
        name,
        args,
        result,
        state,
        stream_id,
        ..
    } = item
    else {
        return item_lines(model, item, width, delegate_child);
    };
    let related_intent = match name.as_str() {
        "bash" => serde_json::from_str::<serde_json::Value>(args)
            .ok()
            .and_then(|value| {
                value
                    .get("proc_id")
                    .and_then(|v| v.as_str())
                    .map(str::to_owned)
            })
            .and_then(|id| model.proc_intents.get(&id).cloned()),
        "delegate" => serde_json::from_str::<serde_json::Value>(args)
            .ok()
            .and_then(|value| {
                value
                    .get("delegate_id")
                    .and_then(|v| v.as_str())
                    .map(str::to_owned)
            })
            .and_then(|id| model.delegate_intents.get(&id).cloned()),
        _ => None,
    };
    let tail = if name == "bash" && present::bash_mode_shows_output(args) {
        bash_proc_id(model, &model.focused_id, args, result.as_deref())
            .and_then(|id| model.host_tails.get(&id))
            .map(String::as_str)
    } else {
        None
    };
    let nested =
        delegate_child.map(|child_id| nested_tool_lines(model, child_id, width, tail_lines));
    let mut lines = match name.as_str() {
        "bash" => present::bash_lines_progressive_window(
            args,
            state,
            tail,
            width,
            theme_of(model),
            related_intent.as_deref(),
            tail_lines,
        ),
        "delegate" => {
            let children = nested.unwrap_or_default();
            present::delegate_lines_progressive_window(
                args,
                state,
                width,
                theme_of(model),
                related_intent.as_deref(),
                tail_lines,
                &children,
            )
        }
        "memory" => present::memory_lines_progressive(
            args,
            result.as_deref(),
            state,
            width,
            theme_of(model),
        ),
        _ => {
            let mut lines = item_lines(model, item, width, delegate_child);
            // Expanded non-process tools may keep a deeper result tail, but
            // they must not invent output they do not have.
            if name != "memory"
                && tail_lines > 3
                && let Some(result) = result
            {
                let extra = tail_lines.saturating_sub(3);
                for row in result
                    .lines()
                    .rev()
                    .take(extra)
                    .collect::<Vec<_>>()
                    .into_iter()
                    .rev()
                {
                    lines.push(Line::styled(
                        format!("  │ {row}"),
                        style::dim(theme_of(model)),
                    ));
                }
            }
            lines
        }
    };
    if let Some(execution_id) = stream_id.as_deref()
        && let Some(execution) = model.tool_execution.get(execution_id)
    {
        lines.extend(present::tool_execution_lines(
            execution,
            width,
            theme_of(model),
        ));
    }
    lines
}

fn theme_of(model: &Model) -> &super::theme::Theme {
    &model.theme
}

fn grouped_quick_tool_lines(
    items: &[&Item],
    kind: &str,
    width: u16,
    theme: &super::theme::Theme,
) -> Vec<Line<'static>> {
    // One compact header carries the group identity and truthful counts;
    // the paths live on neutral detail rows beneath it, wrapped under the
    // detail indentation. Nothing here paints every path in the accent
    // color: the header owns the restrained status accent, the details are
    // readable neutral text.
    let verb = match kind {
        "read" => "Read",
        "list" => "Listed",
        _ => "Read",
    };
    let running = items
        .iter()
        .any(|item| matches!(item, Item::ToolCall { state, .. } if matches!(state, ToolState::Preparing(_) | ToolState::Running(_))));
    let (glyph, glyph_style) = if running {
        ("⠋ ", style::spinner(theme))
    } else {
        ("✓ ", style::tool_ok(theme))
    };
    let failed = items.iter().any(|item| {
        matches!(
            item,
            Item::ToolCall {
                state: ToolState::Done { ok: false, .. },
                ..
            }
        )
    });
    let (glyph, glyph_style) = if failed {
        ("× ", style::tool_err(theme))
    } else {
        (glyph, glyph_style)
    };
    let mut lines = vec![Line::from(vec![
        Span::styled(glyph, glyph_style),
        Span::styled(format!("{verb} {} files", items.len()), style::tool(theme)),
    ])];
    let detail_width = width.saturating_sub(4) as usize;
    let mut row = String::new();
    let flush = |row: &mut String, out: &mut Vec<Line<'static>>, last: bool| {
        if row.is_empty() {
            return;
        }
        let rail = if last { "└─ " } else { "│ " };
        out.push(Line::from(vec![
            Span::styled(format!("{rail}"), style::dim(theme)),
            Span::styled(std::mem::take(row), style::assistant(theme)),
        ]));
    };
    for (index, item) in items.iter().enumerate() {
        let Some(label) = grouped_quick_tool_label(item, kind) else {
            continue;
        };
        let last = index + 1 == items.len();
        let chunk = if last { label } else { format!("{label}, ") };
        let candidate = if row.is_empty() {
            chunk.clone()
        } else {
            format!("{row}{chunk}")
        };
        if !row.is_empty() && candidate.width() > detail_width {
            flush(&mut row, &mut lines, false);
            row = chunk;
        } else {
            row = candidate;
        }
        if last {
            flush(&mut row, &mut lines, true);
        }
    }
    flush(&mut row, &mut lines, true);
    lines
}

fn grouped_quick_tool_label(item: &Item, kind: &str) -> Option<String> {
    let Item::ToolCall { args, .. } = item else {
        return None;
    };
    let value: serde_json::Value = serde_json::from_str(args).ok()?;
    match kind {
        "read" => {
            let path = value.get("path")?.as_str()?.to_string();
            let start = value
                .get("start_line")
                .and_then(|v| v.as_u64())
                .map(|v| v as usize)
                .or_else(|| {
                    value
                        .get("offset")
                        .and_then(|v| v.as_u64())
                        .map(|v| v as usize + 1)
                });
            let limit = value
                .get("limit")
                .and_then(|v| v.as_u64())
                .map(|v| v as usize)
                .or_else(|| {
                    value
                        .get("max_lines")
                        .and_then(|v| v.as_u64())
                        .map(|v| v as usize)
                });
            let end_line = value
                .get("end_line")
                .and_then(|v| v.as_u64())
                .map(|v| v as usize);
            let summary = match (start, limit, end_line) {
                (_, _, Some(end)) => format!("{path}:{}-{end}", start.unwrap_or(1)),
                (Some(start), Some(limit), None) if limit != usize::MAX => {
                    format!(
                        "{path}:{start}-{}",
                        start.saturating_add(limit.saturating_sub(1))
                    )
                }
                (Some(start), None, None) if start > 1 => format!("{path}:{start}-…"),
                (None, Some(limit), None) if limit != usize::MAX => format!("{path}:1-{limit}"),
                _ => path,
            };
            Some(summary)
        }
        "list" => Some(
            value
                .get("path")
                .and_then(|v| v.as_str())
                .unwrap_or(".")
                .to_string(),
        ),
        _ => None,
    }
}

fn wrap_lines(lines: Vec<Line<'static>>, width: u16) -> Vec<Line<'static>> {
    lines
        .into_iter()
        .flat_map(|line| wrap_line(line, width))
        .collect()
}

fn add_gutter(line: Line<'static>) -> Line<'static> {
    let mut spans = Vec::with_capacity(line.spans.len() + 2);
    let gutter_style = line
        .style
        .bg
        .map_or_else(Style::default, |bg| Style::default().bg(bg));
    spans.push(Span::styled(" ", gutter_style));
    spans.extend(line.spans);
    spans.push(Span::styled(" ", gutter_style));
    Line::from(spans).style(line.style)
}

fn draw_transcript(model: &mut Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let theme = &model.theme;
    let general = model.config.lock().unwrap().general.clone();
    if !model.has_agent() {
        let width = area.width as usize;
        let mut lines = Vec::new();
        for raw in WELCOME_LOGO.trim_matches('\n').lines() {
            let logo_width = raw.width();
            let left_pad = width.saturating_sub(logo_width) / 2;
            let mut spans = vec![Span::raw(" ".repeat(left_pad))];
            spans.push(Span::styled(raw.to_string(), style::user(theme)));
            lines.push(Line::from(spans));
        }
        lines.push(padded_plain(width, ""));
        lines.push(Line::styled(
            truncate_width(&format!("  Welcome back. Theme: {}  ", theme.name), width),
            Style::new().fg(theme.fg),
        ));
        lines.push(Line::styled(
            truncate_width(
                "  /login to connect a provider, then ask your first question.  ",
                width,
            ),
            style::assistant(theme),
        ));
        lines.push(Line::styled(
            truncate_width(
                "  /model to pick a model, /theme to switch colors, /resume to reopen work.  ",
                width,
            ),
            style::bar(theme),
        ));
        lines.push(Line::styled(
            truncate_width(
                "  Shift-Tab cycles personas.  Up/Down recalls prompts.  /title names this session.  ",
                width,
            ),
            style::bar(theme),
        ));
        // The optional OOBE tour is stored on the welcome transcript. Render
        // those notes below the static launch copy instead of hiding them
        // behind the `!has_agent` early return.
        if let Some(items) = model.transcripts.get(&model.primary_id) {
            for item in items {
                if let Item::Note(text) = item
                    && !(model
                        .config
                        .lock()
                        .unwrap()
                        .general
                        .autohide_system_messages
                        && is_system_message(text))
                {
                    lines.push(padded_plain(width, ""));
                    lines.extend(wrap(
                        visible_message_text(text),
                        style::note(theme),
                        area.width,
                    ));
                }
            }
        }
        let height = area.height as usize;
        let bottom = lines.len().saturating_sub(height);
        // Welcome/OOBE notes are transcript content too. Measure their
        // extent before selecting the viewport so stale browsing state cannot
        // survive a short launch screen or a tour collapsing.
        model.viewport.set_max_offset(bottom);
        let offset = if model.viewport.follow {
            bottom
        } else {
            bottom.saturating_sub(model.viewport.offset)
        };
        frame.render_widget(
            Paragraph::new(
                lines
                    .into_iter()
                    .skip(offset)
                    .take(height)
                    .collect::<Vec<_>>(),
            ),
            area,
        );
        return;
    }
    let width = area.width;
    // The settings modal edits the shared daemon config. Project the relevant
    // presentation flags on every draw so edits are visible before the modal
    // closes, without requiring a second event or restart.
    model.presentation_settings.disclosure = if general.auto_expand_presenters {
        super::presentation::DisclosureMode::AutoAll
    } else {
        super::presentation::DisclosureMode::Manual
    };
    // A bounded reading column gives prose/cards deliberate breathing room on
    // wide terminals instead of stretching every operation across 160 cells.
    // Narrow terminals still retain a one-cell safety gutter.
    let content_width = width.saturating_sub(if width >= 110 { 10 } else { 2 });
    let settings = model.presentation_settings;
    let cache_miss = model.render_cache.borrow().as_ref().is_none_or(|cache| {
        cache.focused_id != model.focused_id
            || cache.width != width
            || cache.disclosure != settings.disclosure
            || cache.thinking != settings.thinking
            || cache.tail_lines != settings.tail_lines
            || cache.auto_expand_presenters != general.auto_expand_presenters
            || cache.hide_task_tools != general.hide_task_tools
            || cache.hide_todo_tools != general.hide_todo_tools
            || cache.autohide_system_messages != general.autohide_system_messages
            || cache.work_signature != (model.session_event_sequence, model.run_liveness.graph_id())
    });
    if cache_miss {
        let mut lines: Vec<Line<'static>> = Vec::new();
        let mut animated_lines = Vec::new();
        let mut event_ranges = Vec::new();
        let mut affordances: Vec<(SemanticId, HitSubtarget, usize, usize, usize)> = Vec::new();
        let mut delegate_ordinal = 0;
        let transcript = model.focused_transcript();
        let current_disclosable = transcript
            .iter()
            .enumerate()
            .rev()
            .find(|(index, _)| {
                model
                    .semantic_event(&model.focused_id, *index)
                    .is_some_and(|event| event.is_disclosable())
            })
            .map(|(index, _)| index);
        let mut index = 0;
        while index < transcript.len() {
            let item = &transcript[index];
            if (general.autohide_system_messages
                && matches!(item, Item::User(text) | Item::Note(text) | Item::Text(text) if is_system_message(text)))
                || (general.hide_task_tools
                    && matches!(item, Item::ToolCall { name, .. } if name == "task"))
                || (general.hide_todo_tools
                    && matches!(item, Item::ToolCall { name, .. } if name == "todo"))
            {
                index += 1;
                continue;
            }
            // Contiguous same-kind quick calls group in place from the first
            // preparing call, so the group identity is stable while members
            // finish. A completed read followed by a still-preparing read
            // evolves one row, not two rows that later merge.
            if let Some(kind) = quick_group_kind(item) {
                let mut grouped = vec![item];
                let mut next = index + 1;
                while next < transcript.len() && quick_group_kind(&transcript[next]) == Some(kind) {
                    grouped.push(&transcript[next]);
                    next += 1;
                }
                if grouped.len() > 1 {
                    let start = lines.len();
                    let group_lines =
                        grouped_quick_tool_lines(&grouped, kind, content_width, theme);
                    let end = start + group_lines.len().saturating_sub(1);
                    let first = model
                        .semantic_event(&model.focused_id, index)
                        .unwrap_or_else(|| TranscriptEvent::from_item(item, index as u64 + 1));
                    event_ranges.push((first.id.clone(), start, end));
                    lines.extend(group_lines);
                    lines.push(Line::default());
                    index = next;
                    continue;
                }
            }
            let child = if let Item::ToolCall {
                name, stream_id, ..
            } = item
                && name == "delegate"
            {
                let child =
                    model.delegate_child(&model.focused_id, delegate_ordinal, stream_id.as_deref());
                delegate_ordinal += 1;
                child
            } else {
                None
            };
            let event = model
                .semantic_event(&model.focused_id, index)
                .unwrap_or_else(|| TranscriptEvent::from_item(item, index as u64 + 1));
            let is_current = current_disclosable == Some(index);
            let auto_expanded = general.auto_expand_presenters
                || !matches!(
                    disclosure_decision(settings.disclosure, &event, is_current),
                    super::presentation::DisclosureDecision::None
                );
            let expanded = auto_expanded
                || model
                    .expanded_events
                    .contains(&(model.focused_id.clone(), event.id.clone()));
            // The disclosure window is caller-owned: presenters render a
            // substantially deeper output window when the event is expanded,
            // so clicking `expand` changes the actual visible content.
            let tail_window = if expanded {
                super::presentation::TailLines::Five.count() * 6
            } else {
                settings.tail_lines.count()
            };
            let is_thinking = matches!(item, Item::Thinking { .. });
            let mut rendered = if let Item::Thinking { text, .. } = item {
                markdown::render(text, style::thinking(theme), theme)
            } else if expanded {
                item_lines_with_window(
                    model,
                    item,
                    content_width.saturating_sub(4),
                    child,
                    tail_window,
                )
            } else {
                item_lines(model, item, content_width.saturating_sub(4), child)
            };
            rendered = wrap_lines(rendered, content_width.saturating_sub(4));
            let running_presenter = matches!(
                item,
                Item::ToolCall {
                    state: ToolState::Running(_),
                    ..
                }
            );
            if is_thinking
                && !expanded
                && matches!(
                    settings.thinking,
                    super::presentation::ThinkingMode::CollapseAfterActivity
                )
                && rendered.len() > settings.tail_lines.count()
            {
                rendered = rendered
                    .into_iter()
                    .rev()
                    .take(settings.tail_lines.count())
                    .collect::<Vec<_>>();
                rendered.reverse();
            }
            let mut affordance: Option<(HitSubtarget, usize, usize)> = None;
            if matches!(
                event.disclosure,
                DisclosurePolicy::Thinking | DisclosurePolicy::LiveOutput
            ) {
                let thinking = matches!(
                    event.kind,
                    super::presentation::TranscriptEventKind::Thinking
                );
                let affordance_label = if expanded {
                    "▾ collapse"
                } else {
                    "▸ expand"
                };
                let mut semantic = Vec::new();
                if thinking {
                    // Keep the header explicit about whether this is the
                    // current reasoning tail or a settled thought.
                    let live = thinking_is_active(item, model, index, transcript.len());
                    let word = if live { "Thinking" } else { "Thought" };
                    let header_text = word.to_string();
                    let start = "│ ".width() + header_text.width() + "  ".width();
                    affordance = Some((
                        HitSubtarget::ThinkingHeader,
                        start,
                        start + affordance_label.width(),
                    ));
                    let mut header = vec![Span::styled("│ ", style::tool(theme))];
                    header.push(Span::styled(header_text, style::thinking(theme)));
                    header.push(Span::styled(
                        format!("  {affordance_label}"),
                        style::dim(theme),
                    ));
                    semantic.push(Line::from(header));
                }
                for (line_index, mut line) in rendered.into_iter().enumerate() {
                    let prefix = if line_index == 0 && !thinking {
                        // Presenters own their compact summary; this only
                        // appends the real retained-output control after the
                        // header so the click target matches the visible
                        // label instead of a fixed handful of cells.
                        let used = line.spans.iter().map(Span::width).sum::<usize>() + "  ".width();
                        let start = used;
                        affordance = Some((
                            HitSubtarget::LiveOutputHeader,
                            start,
                            start + affordance_label.width(),
                        ));
                        line.spans.push(Span::styled(
                            format!("  {affordance_label}"),
                            style::dim(theme),
                        ));
                        line
                    } else {
                        let prefix = if thinking { "│ " } else { "  │ " };
                        let mut spans = vec![Span::styled(prefix, style::tool(theme))];
                        spans.append(&mut line.spans);
                        Line::from(spans)
                    };
                    semantic.push(prefix);
                }
                rendered = semantic;
            }
            // Prefixes and nested output can change line width.  Wrap the
            // final block before recording ranges so semantic coordinates
            // continue to describe the actual lines at this terminal width.
            rendered = wrap_lines(rendered, content_width);
            // User turns are deliberately substantial visual bands: reserve
            // one dim-background row above and below, and fill every row to
            // the reading-column width so short messages still read as cards.
            if matches!(item, Item::User(_)) {
                let fill = |mut line: Line<'static>| {
                    let used = line.width();
                    if used < content_width as usize {
                        line.spans.push(Span::styled(
                            " ".repeat(content_width as usize - used),
                            style::user_block(theme),
                        ));
                    }
                    line
                };
                rendered = rendered.into_iter().map(fill).collect();
                let blank = || {
                    Line::from(vec![Span::styled(
                        " ".repeat(content_width as usize),
                        style::user_block(theme),
                    )])
                    .style(style::user_block(theme))
                };
                rendered.insert(0, blank());
                rendered.push(blank());
            }
            let start = lines.len();
            let end = start + rendered.len().saturating_sub(1);
            if (running_presenter
                || (is_thinking && thinking_is_active(item, model, index, transcript.len())))
                && start <= end
            {
                // The first line is the stable title/header for both a live
                // presenter and an active thinking block. Its color animation
                // is applied after cache lookup below, not baked into this
                // static layout.
                animated_lines.extend(start..=end);
            }
            if let Some((subtarget, col_start, col_end)) = affordance {
                affordances.push((event.id.clone(), subtarget, start, col_start, col_end));
            }
            event_ranges.push((event.id.clone(), start, end));
            lines.extend(rendered);
            // Separate turns and major operation blocks without introducing
            // a blank row between every compact sibling.
            if !matches!(item, Item::ToolCall { .. }) {
                lines.push(Line::default());
            }
            index += 1;
        }
        // The durable work view renders as transcript content. It is appended
        // after the conversation items so it scrolls with them and cannot
        // compete with the composer for protected rows.
        let (work_lines, work_running) = work_block(model, content_width, theme);
        if !work_lines.is_empty() {
            if !lines.is_empty() {
                lines.push(Line::default());
            }
            let start = lines.len();
            lines.extend(work_lines);
            animated_lines.extend(work_running.into_iter().map(|offset| start + offset));
        }
        *model.render_cache.borrow_mut() = Some(RenderCache {
            focused_id: model.focused_id.clone(),
            width,
            disclosure: settings.disclosure,
            thinking: settings.thinking,
            tail_lines: settings.tail_lines,
            auto_expand_presenters: general.auto_expand_presenters,
            hide_task_tools: general.hide_task_tools,
            hide_todo_tools: general.hide_todo_tools,
            autohide_system_messages: general.autohide_system_messages,
            work_signature: (model.session_event_sequence, model.run_liveness.graph_id()),
            lines: lines
                .into_iter()
                .map(|line| {
                    let mut line = add_gutter(line);
                    if width > content_width {
                        let pad = usize::from(width.saturating_sub(content_width) / 2);
                        let bg = line
                            .style
                            .bg
                            .map_or_else(Style::default, |bg| Style::default().bg(bg));
                        let mut spans = vec![Span::styled(" ".repeat(pad), bg)];
                        spans.append(&mut line.spans);
                        line = Line::from(spans).style(line.style);
                    }
                    // Paragraphs do not paint trailing cells with a line's
                    // style. Explicitly fill user-card rows after centering
                    // so the dim background reaches the terminal edge.
                    if line.style.bg == Some(style::user_block_bg(theme))
                        && line.width() < width as usize
                    {
                        line.spans.push(Span::styled(
                            " ".repeat(width as usize - line.width()),
                            Style::new().bg(style::user_block_bg(theme)),
                        ));
                    }
                    line
                })
                .collect(),
            animated_lines,
            event_ranges,
            affordances,
        });
    }
    let cache = model.render_cache.borrow();
    let cached = cache.as_ref().expect("render cache populated");
    let lines = &cached.lines;
    let animated_lines = cached.animated_lines.clone();
    let height = area.height as usize;
    let total = lines.len();
    let bottom = total.saturating_sub(height);
    // Clamp navigation as soon as the real content extent is known. This is
    // what makes a wheel burst at the top harmless and lets one reverse wheel
    // tick move immediately rather than paying down hidden offset debt.
    // This handles content contraction as well as growth: an expanded event
    // collapsing or a resize must not leave hidden scroll debt behind.
    model.viewport.set_max_offset(bottom);
    let key = (model.focused_id.clone(), width);
    let offset = if model.viewport.follow {
        model.viewport.anchor_top.set(None);
        *model.viewport.anchor_key.borrow_mut() = Some(key);
        bottom
    } else if model.viewport.anchor_key.borrow().as_ref() != Some(&key) {
        // The first draw after a resize/focus change establishes the current
        // absolute line. Subsequent streamed growth leaves it untouched.
        let top = bottom.saturating_sub(model.viewport.offset);
        model.viewport.anchor_top.set(Some(top));
        *model.viewport.anchor_key.borrow_mut() = Some(key);
        top
    } else {
        let top = model
            .viewport
            .anchor_top
            .get()
            .unwrap_or_else(|| bottom.saturating_sub(model.viewport.offset));
        model.viewport.anchor_top.set(Some(top));
        top.min(bottom)
    };
    let mut visible = Vec::with_capacity(height);
    if !model.viewport.follow && height > 0 {
        let above = offset;
        let below = total.saturating_sub(offset.saturating_add(height.saturating_sub(1)));
        let position = if above == 0 && below == 0 {
            "PAUSED · all content visible · End to resume live follow".to_string()
        } else {
            format!("PAUSED · {above} above · {below} below · End to follow")
        };
        visible.push(Line::styled(
            truncate_width(&format!("  {position}"), width as usize),
            style::note(&model.theme),
        ));
    }
    let content_height = height.saturating_sub(visible.len());
    let mut content = lines
        .iter()
        .skip(offset)
        .take(content_height)
        .cloned()
        .collect::<Vec<_>>();
    // Layout/presenter work is intentionally cached, but live title colors
    // must follow the 30 FPS model tick. Reapply only the animated first-line
    // pass here so every transcript item updates smoothly without rebuilding
    // the entire transcript on every frame.
    let phase = glint_phase(model.tick_phase);
    for line_index in animated_lines {
        if line_index >= offset && line_index < offset.saturating_add(content_height) {
            let visible_index = line_index - offset;
            if let Some(line) = content.get_mut(visible_index) {
                let total_len = glint_content_len(line).max(1);
                glint_line(line, theme, phase, total_len, 0);
            }
        }
    }
    // Cues are claimed only after viewport selection.  Thus an off-screen
    // arrival remains unread rather than being consumed by an invisible draw.
    for (event_id, start, end) in &cache.as_ref().expect("render cache populated").event_ranges {
        if *start >= offset.saturating_add(content_height) || *end < offset {
            continue;
        }
        let Some(cue) = model.arrival_cues.claim_for_render(event_id) else {
            continue;
        };
        let first = start.saturating_sub(offset);
        let last = end
            .saturating_sub(offset)
            .saturating_add(1)
            .min(content.len());
        if first < last {
            apply_arrival_glint(
                &mut content[first..last],
                cue.text_span,
                cue.source_revision,
                theme,
            );
        }
    }
    visible.extend(content);
    let screen_start = area.y + u16::from(!model.viewport.follow);
    // Hit rectangles come from the measured affordances recorded during the
    // same render pass that drew the visible labels, so a click on the text
    // the user sees toggles that event. `add_gutter` contributes one leading
    // cell and wide layouts add centered padding before content begins.
    let lead = if width > content_width {
        width.saturating_sub(content_width) / 2 + 1
    } else {
        1
    } as usize;
    let hits = cache
        .as_ref()
        .expect("render cache populated")
        .affordances
        .iter()
        .filter_map(|(event_id, subtarget, line_index, col_start, col_end)| {
            let visible_row = (*line_index).checked_sub(offset)?;
            if visible_row >= content_height {
                return None;
            }
            Some(super::model::TranscriptHitRegion {
                event_id: event_id.clone(),
                subtarget: *subtarget,
                left: (area.x as usize + lead + col_start).min(u16::MAX as usize) as u16,
                right: (area.x as usize + lead + *col_end).min(u16::MAX as usize) as u16,
                top: screen_start + visible_row as u16,
                bottom: screen_start + visible_row as u16,
            })
        })
        .collect();
    *model.transcript_hits.borrow_mut() = hits;
    frame.render_widget(Paragraph::new(visible), area);
}

/// Depth of the child-activity window a delegate shows. The collapsed
/// default is a bounded recent tail; expanding the delegate must reveal the
/// subagent's full retained history, not merely re-render the same tail.
const NESTED_COLLAPSED_LINES: usize = 3;

/// Render a delegate's child-activity preview. A workflow the subagent
/// authored itself (its own `task init`/`create` checklist or run) is the
/// most accurate live progress available and replaces the assigned task
/// list; the assignment board only appears when the child has no workflow
/// of its own.
fn nested_tool_lines(
    model: &Model,
    child_id: &str,
    width: u16,
    max_lines: usize,
) -> Vec<Line<'static>> {
    let Some(items) = model.transcripts.get(child_id) else {
        return Vec::new();
    };
    let mut out = vec![Line::styled(
        format!(
            "CHILD · {} · Ctrl+N inspect",
            model
                .roster
                .iter()
                .find(|(id, _)| id == child_id)
                .map(|(_, label)| label.as_str())
                .unwrap_or(child_id)
        ),
        style::tool(&model.theme).bold(),
    )];
    // The child's own durable workflow replaces the assigned task list. A
    // graph the child authored is identified by graph ownership, not by the
    // active pointer alone: a bound child's active graph is the PARENT's
    // assignment board and must keep the fallback rendering.
    let mut child_workflow_shown = false;
    if let Some(snapshot) = &model.work_snapshot
        && let Some(graph_id) = snapshot.state.active_graph_by_agent.get(child_id).copied()
        && let Some(graph) = snapshot.state.graphs.get(&graph_id)
        && graph.owner_agent_id.as_deref() == Some(child_id)
    {
        child_workflow_shown = true;
        let child_view = work::WorkView::from_graph(graph, max_lines, Some(graph_id));
        let title = child_view
            .graph_title
            .clone()
            .unwrap_or_else(|| graph.title.clone());
        out.push(Line::styled(title, style::tool(&model.theme).bold()));
        for row in child_view.lines {
            let presentation = model.work_presentation(&row);
            let mut spans = vec![Span::styled(
                format!("{} ", presentation.glyph()),
                style::work_status(&model.theme, row.status),
            )];
            if presentation == work::WorkPresentation::Running {
                spans.extend(glint_title(
                    &row.title,
                    style::assistant(&model.theme),
                    &model.theme,
                    model.tick_phase,
                ));
            } else {
                spans.push(Span::styled(row.title, style::assistant(&model.theme)));
            }
            spans.push(Span::styled(
                row.detail
                    .map(|detail| format!(" · {detail}"))
                    .unwrap_or_default(),
                style::dim(&model.theme),
            ));
            out.push(Line::from(spans));
        }
        if child_view.overflow > 0 {
            out.push(Line::styled(
                format!(
                    "… {} more checklist item{}",
                    child_view.overflow,
                    if child_view.overflow == 1 { "" } else { "s" }
                ),
                style::dim(&model.theme),
            ));
        }
    }
    if !child_workflow_shown && let Some(snapshot) = &model.work_snapshot {
        let child_work = work::WorkView::for_child(snapshot, child_id, None, max_lines);
        if let Some(context) = child_work.parent_context {
            out.push(Line::styled(context, style::dim(&model.theme)));
        }
        for row in child_work.lines {
            let presentation = model.work_presentation(&row);
            let mut spans = vec![Span::styled(
                format!("{} ", row.glyph),
                style::work_status(&model.theme, row.status),
            )];
            if presentation == work::WorkPresentation::Running {
                spans.extend(glint_title(
                    &row.title,
                    style::assistant(&model.theme),
                    &model.theme,
                    model.tick_phase,
                ));
            } else {
                spans.push(Span::styled(row.title, style::assistant(&model.theme)));
            }
            spans.push(Span::styled(
                row.detail
                    .map(|detail| format!(" · {detail}"))
                    .unwrap_or_default(),
                style::dim(&model.theme),
            ));
            out.push(Line::from(spans));
        }
    }
    // The workflow rows ARE the live progress when the child owns one, so a
    // collapsed preview skips the redundant tool tail; expanding still adds
    // the subagent's actual tool-call history beneath it.
    let tail_budget = if child_workflow_shown {
        max_lines.saturating_sub(NESTED_COLLAPSED_LINES).max(1) * 2
    } else {
        max_lines
    };
    if tail_budget > 0 {
        out.extend(
            items
                .iter()
                .rev()
                .filter_map(|item| {
                    let Item::ToolCall {
                        name,
                        args,
                        result,
                        state,
                        ..
                    } = item
                    else {
                        return None;
                    };
                    let tail = if name == "bash" && present::bash_mode_shows_output(args) {
                        bash_proc_id(model, child_id, args, result.as_deref())
                            .and_then(|id| model.host_tails.get(&id))
                            .map(String::as_str)
                    } else {
                        None
                    };
                    Some(match name.as_str() {
                        "bash" => present::bash_lines_progressive(
                            args,
                            state,
                            tail,
                            width,
                            &model.theme,
                            None,
                        ),
                        "delegate" => present::delegate_lines_progressive(
                            args,
                            state,
                            width,
                            &model.theme,
                            None,
                        ),
                        "message" => present::message_lines_progressive(
                            args,
                            result.as_deref(),
                            state,
                            width,
                            &model.theme,
                        ),
                        "task" => present::task_lines_progressive(
                            args,
                            result.as_deref(),
                            state,
                            width,
                            &model.theme,
                        ),
                        "workflow" => present::workflow_lines_progressive(
                            args,
                            result.as_deref(),
                            state,
                            width,
                            &model.theme,
                        ),
                        "edit" if matches!(state, ToolState::Preparing(_)) => {
                            present::edit_lines_compact("{}", state, width, &model.theme, 1)
                        }
                        "edit" if args.len() > 64 * 1024 => {
                            present::edit_lines_compact(args, state, width, &model.theme, 8)
                        }
                        "edit" => present::tool_lines(name, args, state, None, width, &model.theme),
                        _ => present::tool_lines(name, args, state, tail, width, &model.theme),
                    })
                })
                .take(tail_budget)
                .collect::<Vec<_>>()
                .into_iter()
                .rev()
                .flat_map(|lines| lines.into_iter()),
        );
    }
    out
}

// ---------------------------------------------------------------------------
// Top bar: activity + usage
// ---------------------------------------------------------------------------

/// The turn flag covers agent turns, but a lone streaming tool/search keeps
/// the screen alive even when `Model::busy` has not been set yet. Treat any
/// in-flight transcript tail the same way so the status phrase never lies:
/// live reasoning, streaming tools, search and compaction all count.
fn effective_busy(model: &Model) -> bool {
    model.busy
        || model
            .agents
            .get(&model.focused_id)
            .is_some_and(|agent| !agent.pending_messages().is_empty())
        || model
            .focused_transcript()
            .last()
            .is_some_and(|item| match item {
                Item::Thinking { .. } => model.busy,
                Item::ToolCall { state, .. } => {
                    matches!(state, ToolState::Preparing(_) | ToolState::Running(_))
                }
                Item::WebSearch { state, .. } => {
                    matches!(state, SearchState::Preparing(_))
                }
                Item::Compaction(item) => matches!(item.phase, CompactionPhase::Running(_)),
                _ => false,
            })
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

fn draw_composer(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect, lines: &[String]) {
    let theme = &model.theme;
    let block = Block::default()
        .borders(Borders::TOP | Borders::BOTTOM)
        .border_style(style::border(theme));
    let inner = block.inner(area);
    let inner_height = inner.height as usize;
    let composer_width = area.width.saturating_sub(4) as usize;
    let (cursor_row, _) = model
        .composer
        .cursor_pos_with_width(&model.pastes, composer_width);
    let max_offset = lines.len().saturating_sub(inner_height);
    let scroll_offset = composer_scroll_offset(cursor_row, lines.len(), inner_height);
    let clipped_above = scroll_offset > 0;
    let clipped_below = scroll_offset < max_offset;
    let _indicator = match (clipped_above, clipped_below) {
        (true, true) => Some("⋯"),
        (true, false) => Some("↑"),
        (false, true) => Some("↓"),
        (false, false) => None,
    };
    frame.render_widget(block, area);

    let empty = model.composer.is_empty() && lines.len() <= 1;
    let paragraph = if empty {
        Paragraph::new(Line::from(vec![
            Span::styled("› ", style::user(theme)),
            Span::styled(
                "ask something…  (enter sends · ↑ history · /resume resume)",
                style::placeholder(theme),
            ),
        ]))
    } else {
        Paragraph::new(
            lines
                .iter()
                .enumerate()
                .map(|(row, l)| {
                    let prefix = if row == 0 { "› " } else { "· " };
                    let prefix_style = if row == 0 {
                        style::user(theme)
                    } else {
                        style::dim(theme)
                    };
                    if l.starts_with("[Pasted text") {
                        Line::from(vec![
                            Span::styled(prefix, prefix_style),
                            Span::styled(l.clone(), style::paste_block(theme)),
                        ])
                    } else {
                        Line::from(vec![
                            Span::styled(prefix, prefix_style),
                            Span::styled(l.clone(), Style::default()),
                        ])
                    }
                })
                .collect::<Vec<_>>(),
        )
    };
    frame.render_widget(paragraph.scroll((scroll_offset as u16, 0)), inner);

    if !empty {
        if model.modal.is_none() {
            let (row, col) = model
                .composer
                .cursor_pos_with_width(&model.pastes, composer_width);
            let x = inner.x + col as u16;
            let x = x.saturating_add(2);
            let y = inner.y
                + row
                    .saturating_sub(scroll_offset)
                    .min(inner.height.saturating_sub(1) as usize) as u16;
            frame.set_cursor_position(Position { x, y });
        }
    }
}

// ---------------------------------------------------------------------------
// Bottom bar: identity + permission mode + focus + background counts + hints
// ---------------------------------------------------------------------------

fn draw_bottom_bar(
    model: &Model,
    frame: &mut Frame,
    area: ratatui::layout::Rect,
    todo: Option<&todo::TodoRail>,
) {
    let theme = &model.theme;
    let w = area.width as usize;
    let mut left_text = bottom_bar_left_text(model, w);
    if let Some(rail) = todo {
        // A collapsed/final rail is the only todo signal left on screen, so
        // keep it beside the identity text rather than dropping it silently.
        let summary = rail.summary_line();
        if !summary.is_empty() {
            left_text = format!("{left_text} · {summary}");
        }
    }
    // Keep both sides bounded independently.  A long model/persona or a
    // translated note must never push the keyboard hints past the viewport.
    let right = truncate_width(input_hints(model.busy, w), w / 2);
    let note = model
        .note
        .as_ref()
        .map(|(note, _)| note.as_str())
        .unwrap_or("");
    let right_text = right.to_string();
    let note_text = if note.is_empty() || w < 64 {
        String::new()
    } else {
        format!("  {}  ", truncate_width(note, w / 4))
    };
    let separators = if note_text.is_empty() { 2 } else { 4 };
    let left_limit = w
        .saturating_sub(right_text.width())
        .saturating_sub(note_text.width())
        .saturating_sub(separators);
    left_text = truncate_width(&left_text, left_limit);
    let pad = w
        .saturating_sub(left_text.width())
        .saturating_sub(note_text.width())
        .saturating_sub(right_text.width());
    let left_gap = if note_text.is_empty() { pad } else { pad / 2 };
    let right_gap = if note_text.is_empty() {
        0
    } else {
        pad.saturating_sub(left_gap)
    };
    let left_style = style::bar(theme);
    let permission_style = Style::new().fg(theme.accent).bg(theme.selection_bg).bold();
    let model_style = Style::new().fg(theme.fg).bg(theme.dim_bg).bold();
    let effort_style = Style::new().fg(theme.gradient_hi).bg(theme.bg).bold();
    let focus_style = Style::new().fg(theme.thinking).bg(theme.bg).bold();
    let metric_style = Style::new().fg(theme.gradient_hi).bg(theme.bg).bold();
    let separator_style = Style::new().fg(theme.border);
    let mut decorated = left_text
        .split("  ")
        .enumerate()
        .map(|(index, segment)| {
            let style = if segment.trim_start().starts_with("▣") {
                permission_style
            } else if index == 0 {
                model_style
            } else if index == 1 {
                effort_style
            } else if index == 2 {
                focus_style
            } else if segment.contains('↑') || segment.contains('↓') || segment.contains('≈')
            {
                metric_style
            } else {
                left_style
            };
            Span::styled(segment.trim().to_string(), style)
        })
        .enumerate()
        .flat_map(|(index, span)| {
            if index == 0 {
                vec![span]
            } else {
                vec![Span::styled(" · ", separator_style), span]
            }
        })
        .collect::<Vec<_>>();
    decorated.extend([
        Span::styled(" ".repeat(left_gap), Style::default()),
        Span::styled(note_text, style::note(theme)),
        Span::styled(" ".repeat(right_gap), Style::default()),
        Span::styled(right_text, style::bar(theme)),
    ]);
    frame.render_widget(Paragraph::new(Line::from(decorated)), area);
}

fn bottom_bar_left_text(model: &Model, width: usize) -> String {
    let mode_text = permission_mode_status(model);
    let (_, model_name, effort) = model.focused_model_status();
    let usage = focused_usage(model);
    let mut left = format!(
        "◆ {}  ◇ {}  ◎ {}  ↑{}  ↓{}  ≈{}",
        model_name,
        effort,
        model.focus_label(),
        format_token_count(usage.input_tokens),
        format_token_count(usage.output_tokens),
        format_token_count(usage.cache_read_tokens),
    );
    if model.bg_procs > 0 {
        left.push_str(&format!("  tasks {}", model.bg_procs));
    }
    if model.bg_agents > 0 {
        left.push_str(&format!("  agents {}", model.bg_agents));
    }
    // Keep the permission indicator visible even when the identity/focus
    // portion is long. Previously truncating the combined string could hide
    // the active mode at exactly the point where it is most useful.
    let left_limit = width / 2 + 12;
    let mode_width = mode_text.width().saturating_add(3);
    let identity_limit = left_limit.saturating_sub(mode_width);
    let identity_text = truncate_width(&left, identity_limit);
    if identity_text.is_empty() {
        mode_text.clone()
    } else {
        format!("{identity_text}  ▣ {mode_text}")
    }
}

fn focused_usage(model: &Model) -> firmius_core::Usage {
    if let Some(snapshot) = &model.remote_snapshot {
        if let Some(agent) = snapshot
            .agents
            .iter()
            .find(|agent| agent.record.id == model.focused_id)
        {
            return agent.total_usage;
        }
    }
    model
        .agents
        .get(&model.focused_id)
        .map(|agent| agent.total_usage())
        .or_else(|| {
            model
                .primary
                .as_ref()
                .filter(|agent| agent.id == model.focused_id)
                .map(|agent| agent.total_usage())
        })
        .unwrap_or_default()
}

fn format_token_count(value: u32) -> String {
    match value {
        0..=999 => value.to_string(),
        1_000..=999_999 => format!("{:.1}k", value as f32 / 1_000.0),
        _ => format!("{:.1}m", value as f32 / 1_000_000.0),
    }
}

/// Keep the most useful keyboard shortcuts visible without allowing hints to
/// crowd the identity/status side of the bar on small terminals. The
/// completion popup has its own navigation hint; these cover the composer.
fn input_hints(busy: bool, width: usize) -> &'static str {
    if busy {
        if width >= 90 {
            "esc cancel · tab complete · ^K palette · ^C quit"
        } else {
            "esc cancel · ^C quit"
        }
    } else if width >= 110 {
        "↵ send · ⇧↵ newline · tab complete · ^K palette · ^C quit"
    } else if width >= 80 {
        "↵ send · ⇧↵ newline · ^K palette · ^C quit"
    } else {
        "↵ send · ^C quit"
    }
}

fn draw_context_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let theme = &model.theme;
    let width = area.width as usize;
    if width == 0 || area.height == 0 {
        return;
    }
    let context_bar_width = if width >= 80 {
        18
    } else if width >= 52 {
        10
    } else {
        6
    };
    let bar = present::progress_bar(
        model.ctx_used as u64,
        model.ctx_max as u64,
        context_bar_width,
    );
    let usage = present::format_context_usage(model.ctx_used, model.ctx_max);
    let ratio = if model.ctx_max == 0 {
        0.0
    } else {
        model.ctx_used as f32 / model.ctx_max as f32
    };
    let bar_color = if ratio < 0.8 {
        super::theme::lerp_color(theme.gradient_lo, theme.gradient_hi, ratio / 0.8)
    } else {
        super::theme::lerp_color(
            theme.gradient_hi,
            theme.warn,
            ((ratio - 0.8) / 0.2).clamp(0.0, 1.0),
        )
    };
    let persona_id = model.focused_persona_id();
    let persona = persona_id
        .as_deref()
        .and_then(|id| model.personas.get(id).map(|p| p.name.as_str()))
        .unwrap_or("Default");
    let persona = truncate_width(&format!("persona {persona}"), width.saturating_sub(28));
    let mut spans = vec![
        chip("CTX", theme.bg, theme.border),
        Span::raw(" "),
        Span::styled(bar, Style::new().fg(bar_color).bold()),
        Span::styled(format!(" {usage}"), style::bar(theme)),
    ];
    if width >= 44 {
        spans.push(Span::styled("  ", Style::default()));
        spans.push(chip(persona, theme.fg, theme.selection_bg));
    }
    if let Some(snapshot) = &model.quota {
        let quota_bar_width = if width >= 100 {
            8
        } else if width >= 70 {
            5
        } else {
            0
        };
        for meter in snapshot.meters.iter().take(if width >= 70 { 2 } else { 1 }) {
            let (used, limit) = match (meter.used, meter.limit) {
                (Some(used), Some(limit)) if limit > 0 => (used, limit),
                _ => continue,
            };
            let ratio = used as f32 / limit as f32;
            let color = if ratio < 0.8 {
                super::theme::lerp_color(theme.gradient_lo, theme.gradient_hi, ratio / 0.8)
            } else {
                super::theme::lerp_color(
                    theme.gradient_hi,
                    theme.warn,
                    ((ratio - 0.8) / 0.2).clamp(0.0, 1.0),
                )
            };
            let short = meter.id.chars().take(3).collect::<String>().to_uppercase();
            if quota_bar_width > 0 {
                spans.push(Span::raw("  "));
                spans.push(chip(short, theme.bg, theme.border));
                spans.push(Span::raw(" "));
                spans.push(Span::styled(
                    present::progress_bar(used, limit, quota_bar_width),
                    Style::new().fg(color).bold(),
                ));
                spans.push(Span::styled(
                    format!(" {}", present::format_quota_percent(used, limit)),
                    style::bar(theme),
                ));
            }
        }
    } else if let Some(error) = &model.quota_error {
        spans.push(Span::raw("  "));
        spans.push(chip("QUOTA", theme.bg, theme.warn));
        spans.push(Span::styled(
            format!(" {}", truncate_width(error, 24)),
            style::note(theme),
        ));
    }
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}

#[cfg(test)]
mod tests {
    use super::{
        apply_arrival_glint, bottom_bar_left_text, composer_scroll_offset, draw,
        draw_modal_surface, glint_phase, input_hints, nested_todo_budgets, permission_mode_status,
        work_block, wrap_lines,
    };
    use crate::tui::modal::{ModalAction, ModalSurface};
    use crate::tui::model::{Item, Model};
    use crate::tui::theme;
    use async_trait::async_trait;
    use crossterm::event::KeyEvent;
    use firmius_core::{
        FirmiusConfig, McpManager, PermissionMode, PermissionPolicy, PersonaManager,
        ProviderManager, ToolRegistry, UserSettings,
    };
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;
    use ratatui::layout::Rect;
    use ratatui::style::{Modifier, Style};
    use ratatui::text::{Line, Span};
    use ratatui::widgets::{Block, Paragraph};
    use std::sync::{Arc, Mutex};

    #[test]
    fn transcript_glint_phase_runs_once_then_stays_static() {
        assert!(glint_phase(14) < glint_phase(15));
        assert_eq!(glint_phase(28), 2.0);
        assert_eq!(glint_phase(119), 2.0);
        assert_eq!(glint_phase(120), 0.0);
    }

    struct SparseModal;

    fn welcome_model() -> Model {
        Model::new(
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
        )
    }

    fn todo_projection(agent_id: &str, titles: &[&str]) -> firmius_protocol::TodoProjectionDto {
        firmius_protocol::TodoProjectionDto {
            version: firmius_protocol::TODO_DTO_VERSION,
            agent_id: agent_id.into(),
            revision: 1,
            pending: titles.len(),
            in_progress: 0,
            blocked: 0,
            completed: 0,
            items: titles
                .iter()
                .enumerate()
                .map(|(index, title)| firmius_protocol::TodoItemDto {
                    id: format!("{agent_id}-{index}"),
                    title: (*title).into(),
                    status: firmius_protocol::TodoStatusDto::Pending,
                    evidence_count: 0,
                    evidence_required: false,
                    waiting_reason: None,
                })
                .collect(),
            completion: firmius_protocol::TodoCompletionDto::Waiting {
                unfinished: titles.len(),
                blocked: 0,
                evidence_deficits: 0,
            },
        }
    }

    fn agent_snapshot(agent_id: &str, titles: &[&str]) -> firmius_protocol::AgentSnapshot {
        firmius_protocol::AgentSnapshot {
            record: firmius_core::AgentRecord {
                id: agent_id.into(),
                provider_id: "test".into(),
                model: "test".into(),
                effort: None,
                system_prompt: None,
                persona: None,
                temperature: None,
                max_tokens: None,
                workdir: std::env::temp_dir(),
                label: None,
                metadata: Default::default(),
                history: Default::default(),
                mailbox: Vec::new(),
                active_goal_id: None,
                todo: Default::default(),
                compaction: None,
            },
            usage: Default::default(),
            total_usage: Default::default(),
            busy: true,
            processes: Vec::new(),
            todo: Some(todo_projection(agent_id, titles)),
        }
    }

    fn model_with_running_workers(workers: &[(&str, &[&str])]) -> Model {
        let mut model = welcome_model();
        let mut state = firmius_core::WorkState::default();
        let mut graph = firmius_core::WorkGraph::new(
            "Reconnoiter swarm architecture",
            Some("parent".into()),
            firmius_core::GraphMode::Advisory,
        );
        for (index, (agent_id, _)) in workers.iter().enumerate() {
            let mut node = firmius_core::WorkNode::new(format!("node-{index}"), *agent_id);
            node.status = firmius_core::ExecutionStatus::Running;
            let node_id = node.id;
            graph.view_order.push(node_id);
            graph.nodes.insert(node_id, node);
            let assignment_id = firmius_core::AssignmentId::new();
            graph.assignments.insert(
                assignment_id,
                firmius_core::WorkAssignment {
                    id: assignment_id,
                    node_id,
                    attempt_id: firmius_core::AttemptId::new(),
                    agent_id: (*agent_id).into(),
                    parent_agent_id: Some("parent".into()),
                    assigned_at: chrono::Utc::now(),
                    released_at: None,
                },
            );
        }
        let graph_id = graph.id;
        state.graphs.insert(graph_id, graph);
        state
            .active_graph_by_agent
            .insert("parent".into(), graph_id);
        let mut agents = vec![agent_snapshot("parent", &[])];
        agents.extend(
            workers
                .iter()
                .map(|(agent_id, titles)| agent_snapshot(agent_id, titles)),
        );
        model.replace_remote_snapshot(firmius_protocol::SessionSnapshot {
            session_id: "session".into(),
            title: None,
            sequence: 1,
            primary_agent_id: "parent".into(),
            agents,
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new("session", 1, state),
            active_turns: Default::default(),
            active_delegates: workers.len(),
            live_events: Vec::new(),
        });
        model.focused_id = "parent".into();
        model
    }

    #[test]
    fn nested_todo_budgets_are_fair_and_bounded() {
        let model = model_with_running_workers(&[
            ("work", &["w1", "w2", "w3", "w4"]),
            ("runtime", &["r1", "r2"]),
            ("tools", &["t1"]),
        ]);
        let budgets = nested_todo_budgets(&model, ["work", "runtime", "tools"]);
        assert_eq!(budgets["work"], 3);
        assert_eq!(budgets["runtime"], 2);
        assert_eq!(budgets["tools"], 1);
        assert!(budgets.values().sum::<usize>() <= super::WORK_TODO_ROWS);
    }

    #[test]
    fn work_block_nests_each_workers_live_todos_with_exact_overflow() {
        let model = model_with_running_workers(&[
            ("work", &["work one", "work two", "work three", "work four"]),
            ("runtime", &["runtime one"]),
        ]);
        let (lines, _) = work_block(&model, 80, &model.theme);
        let rendered = lines.iter().map(Line::to_string).collect::<Vec<_>>();
        let work_node = rendered.iter().position(|line| line == "  ◐ work").unwrap();
        let work_item = rendered
            .iter()
            .position(|line| line.contains("work one"))
            .unwrap();
        let runtime_node = rendered
            .iter()
            .position(|line| line == "  ◐ runtime")
            .unwrap();
        let runtime_item = rendered
            .iter()
            .position(|line| line.contains("runtime one"))
            .unwrap();
        assert!(work_node < work_item && work_item < runtime_node);
        assert!(runtime_node < runtime_item);
        assert!(rendered.iter().any(|line| line.contains("│ … 1 more")));
        assert!(rendered[work_item].contains("│ ○ work one"));
    }

    #[test]
    fn nested_todos_wrap_safely_in_a_narrow_work_block() {
        let model = model_with_running_workers(&[(
            "work",
            &["inspect an intentionally long nested todo title without losing content"],
        )]);
        let (lines, _) = work_block(&model, 24, &model.theme);
        assert!(lines.len() > 3, "expected wrapped rows: {lines:?}");
        assert!(
            lines
                .iter()
                .map(Line::to_string)
                .collect::<String>()
                .contains("without losing content")
        );
    }

    #[test]
    fn arrival_glint_styles_only_the_recorded_character_span() {
        let theme = theme::default_theme();
        let base = Style::default().fg(theme.fg);
        let mut lines = vec![Line::from(Span::styled("unchanged changed", base))];
        apply_arrival_glint(&mut lines, 10..17, 3, &theme);
        assert_eq!(lines[0].to_string(), "unchanged changed");
        assert_eq!(lines[0].spans[0].content, "unchanged ");
        assert!(
            lines[0].spans[1..]
                .iter()
                .all(|span| span.style.add_modifier.contains(Modifier::BOLD))
        );
    }

    #[async_trait]
    impl ModalSurface for SparseModal {
        fn title(&self) -> String {
            "Sparse".into()
        }

        fn height_hint(&self, _width: u16) -> u16 {
            5
        }

        fn width_hint(&self, _available: u16) -> u16 {
            20
        }

        fn render(
            &self,
            area: Rect,
            frame: &mut ratatui::Frame,
            _theme: &super::super::theme::Theme,
        ) {
            frame.render_widget(Block::bordered(), area);
        }

        async fn key(&mut self, _key: KeyEvent) -> ModalAction {
            ModalAction::Stay
        }

        fn cursor(&self, _area: Rect) -> Option<(u16, u16)> {
            None
        }
    }

    #[test]
    fn input_hints_prioritize_shortcuts_by_terminal_width() {
        assert_eq!(input_hints(false, 60), "↵ send · ^C quit");
        assert!(input_hints(false, 80).contains("⇧↵ newline"));
        assert!(input_hints(false, 110).contains("tab complete"));
        assert!(!input_hints(true, 70).contains("tab complete"));
        assert!(!input_hints(true, 90).contains("^O sessions"));
    }

    #[test]
    fn permission_mode_status_shows_active_mode() {
        let mut model = welcome_model();
        model.permission_policy = Some(PermissionPolicy::default());
        assert_eq!(permission_mode_status(&model), "perm: Default");
        model.permission_policy.as_mut().unwrap().mode = PermissionMode::Auto;
        assert_eq!(permission_mode_status(&model), "perm: Auto");
        model.permission_policy.as_mut().unwrap().mode = PermissionMode::Yolo;
        assert_eq!(permission_mode_status(&model), "perm: YOLO");
        model.permission_policy.as_mut().unwrap().mode = PermissionMode::Custom("safe".into());
        assert_eq!(permission_mode_status(&model), "perm: Custom: safe");
        model.permission_policy = None;
        assert_eq!(permission_mode_status(&model), "perm: unavailable");
    }

    #[test]
    fn bottom_bar_left_text_keeps_permission_mode_when_identity_is_long() {
        let mut model = welcome_model();
        model.permission_policy = Some(PermissionPolicy::default());
        assert!(bottom_bar_left_text(&model, 40).contains("perm: Default"));
    }

    #[test]
    fn wraps_long_logical_lines_into_scrollable_visual_rows() {
        let wrapped = wrap_lines(vec![Line::from("abcdefghij")], 4);
        assert_eq!(wrapped.len(), 3);
        assert_eq!(wrapped[0].to_string(), "abcd");
        assert_eq!(wrapped[1].to_string(), "efgh");
        assert_eq!(wrapped[2].to_string(), "ij");
    }

    #[test]
    fn composer_scroll_keeps_cursor_in_visible_window() {
        let line_count = 20;
        let visible_rows = 8;
        for cursor_row in 0..line_count {
            let offset = composer_scroll_offset(cursor_row, line_count, visible_rows);
            assert!(cursor_row >= offset);
            assert!(cursor_row < offset + visible_rows);
        }
    }

    #[test]
    fn wraps_at_word_boundaries_before_hard_breaking_words() {
        let wrapped = wrap_lines(vec![Line::from("one two three")], 7);
        assert_eq!(
            wrapped.iter().map(Line::to_string).collect::<Vec<_>>(),
            vec!["one two", "three"]
        );

        let wrapped = wrap_lines(vec![Line::from("abcdef")], 3);
        assert_eq!(
            wrapped.iter().map(Line::to_string).collect::<Vec<_>>(),
            vec!["abc", "def"]
        );
    }

    #[test]
    fn user_messages_are_plain_unboxed_lines() {
        let theme = theme::default_theme();
        let lines = super::wrap("hello\nworld", Style::new().fg(theme.fg), 10);
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0].to_string(), "hello");
        assert_eq!(lines[1].to_string(), "world");
        assert!(
            lines
                .iter()
                .all(|line| line.spans.iter().all(|span| span.style.bg.is_none()))
        );
    }

    #[test]
    fn gutter_adds_unstyled_space_to_both_sides() {
        let line = super::add_gutter(Line::styled(
            "text",
            Style::new().fg(ratatui::style::Color::Red),
        ));
        assert_eq!(line.to_string(), " text ");
        assert_eq!(line.spans.first().unwrap().style, Style::default());
        assert_eq!(line.spans.last().unwrap().style, Style::default());
    }

    #[test]
    fn modal_clears_transcript_cells_beneath_sparse_content() {
        let backend = TestBackend::new(40, 20);
        let mut terminal = Terminal::new(backend).unwrap();
        terminal
            .draw(|frame| {
                let background = vec![Line::from("X".repeat(40)); 20];
                frame.render_widget(Paragraph::new(background), frame.area());
                draw_modal_surface(
                    &SparseModal,
                    frame,
                    Rect {
                        x: 0,
                        y: 15,
                        width: 40,
                        height: 3,
                    },
                    &theme::default_theme(),
                );
            })
            .unwrap();

        // Deck is full width and starts at y=12. Its untouched interior must be
        // blank, rather than retaining the transcript's X cells.
        assert_eq!(
            terminal.backend().buffer().cell((11, 13)).unwrap().symbol(),
            " "
        );
    }

    #[test]
    fn welcome_render_normalizes_stale_viewport_state() {
        let mut model = welcome_model();
        model.transcripts.insert(
            model.primary_id.clone(),
            vec![Item::Note("tour note\n".repeat(40))],
        );
        model.viewport.offset = usize::MAX / 4;
        model.viewport.follow = false;
        model.viewport.anchor_top.set(Some(99));
        *model.viewport.anchor_key.borrow_mut() = Some(("welcome".into(), 40));
        let mut terminal = Terminal::new(TestBackend::new(40, 12)).unwrap();

        terminal.draw(|frame| draw(&mut model, frame)).unwrap();

        assert!(model.viewport.offset <= model.viewport.max_offset.get());
        assert!(model.viewport.anchor_top.get().is_none());
        assert!(model.viewport.anchor_key.borrow().is_none());
    }

    #[test]
    fn reset_to_welcome_discards_browsing_viewport_state() {
        let mut model = welcome_model();
        model.viewport.offset = 7;
        model.viewport.max_offset.set(10);
        model.viewport.follow = false;
        model.viewport.anchor_top.set(Some(3));
        *model.viewport.anchor_key.borrow_mut() = Some(("agent".into(), 80));

        model.reset_to_welcome();

        assert_eq!(model.viewport.offset, 0);
        assert_eq!(model.viewport.max_offset.get(), 0);
        assert!(model.viewport.follow);
        assert_eq!(model.viewport.anchor_top.get(), None);
        assert!(model.viewport.anchor_key.borrow().is_none());
    }
}
