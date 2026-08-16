//! Pure rendering: model in, frame out. No mutation, no I/O beyond drawing.

use ratatui::layout::{Constraint, Layout, Position};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Paragraph};
use ratatui::Frame;
use ratatui::style::Style;

use super::model::{Item, Model};
use super::present;
use super::style;

const VERBS: &[&str] = &[
    "Thinking",
    "Cogitating",
    "Conspiring",
    "Muttering",
    "Scheming",
    "Unhinging",
];

pub fn draw(model: &Model, frame: &mut Frame) {
    let area = frame.area();
    let composer_lines = model.composer.lines(&model.pastes);
    let composer_h = (composer_lines.len() as u16 + 2).clamp(3, 10).min(area.height / 2 + 3);
    let chunks = Layout::vertical([
        Constraint::Min(1),
        Constraint::Length(1),
        Constraint::Length(composer_h),
        Constraint::Length(1),
    ])
    .split(area);

    draw_transcript(model, frame, chunks[0]);
    draw_top_bar(model, frame, chunks[1]);
    draw_composer(model, frame, chunks[2], &composer_lines);
    draw_bottom_bar(model, frame, chunks[3]);
}

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

/// Wrap text to `width` (char-based; word wrap is future polish).
fn wrap(text: &str, style: Style, width: u16) -> Vec<Line<'static>> {
    let w = (width as usize).max(1);
    let mut out = Vec::new();
    for raw in text.split('\n') {
        let mut line = String::new();
        let mut used = 0usize;
        for ch in raw.chars() {
            if used >= w {
                out.push(Line::styled(std::mem::take(&mut line), style));
                used = 0;
            }
            line.push(if ch == '\t' { ' ' } else { ch });
            used += 1;
        }
        out.push(Line::styled(line, style));
    }
    out
}

fn item_lines(model: &Model, item: &Item, width: u16) -> Vec<Line<'static>> {
    match item {
        Item::User(t) => wrap(&format!("you: {t}"), style::user(), width),
        Item::Text(t) => wrap(t, style::assistant(), width),
        Item::Thinking(t) => wrap(t, style::thinking(), width),
        Item::ToolCall { name, args, state } => {
            let tail = if name == "bash" {
                bash_cmdline(args).and_then(|c| model.host_tails.get(&c))
                    .map(String::as_str)
            } else {
                None
            };
            present::tool_lines(name, args, state, tail, width)
        }
        Item::Note(t) => wrap(t, style::note(), width),
    }
}

/// Reconstruct the cmdline a bash tool call will spawn, for tail matching.
fn bash_cmdline(args: &str) -> Option<String> {
    let v = serde_json::from_str::<serde_json::Value>(args).ok()?;
    let cmd = v.get("command")?.as_str()?;
    let mut line = cmd.to_string();
    if let Some(extra) = v.get("args").and_then(|a| a.as_array()) {
        for a in extra {
            if let Some(s) = a.as_str() {
                line.push(' ');
                line.push_str(s);
            }
        }
    }
    Some(line)
}

fn draw_transcript(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let width = area.width;
    let mut lines: Vec<Line<'static>> = Vec::new();
    for item in model.focused_transcript() {
        lines.extend(item_lines(model, item, width));
    }
    let height = area.height as usize;
    let total = lines.len();
    let offset = if model.viewport.follow {
        total.saturating_sub(height)
    } else {
        model.viewport.offset.min(total.saturating_sub(height))
    };
    let visible: Vec<Line<'static>> = lines.into_iter().skip(offset).take(height).collect();
    frame.render_widget(Paragraph::new(visible), area);
}

// ---------------------------------------------------------------------------
// Top bar: activity + usage
// ---------------------------------------------------------------------------

fn draw_top_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let w = area.width as usize;
    let usage = model.primary.usage();
    let right = format!(
        "↑{} ↓{} ⚡{}",
        present::fmt_tokens(usage.input_tokens),
        present::fmt_tokens(usage.output_tokens),
        present::fmt_tokens(usage.cache_read_tokens + usage.cache_write_tokens),
    );
    let left = if model.busy {
        let frame_idx = model.tick_phase % style::SPINNER.len();
        let spin = style::SPINNER[frame_idx];
        let phrase = activity_phrase(model);
        let elapsed = model
            .turn_started
            .map(|t| format!(" {}s", present::elapsed_secs(t)))
            .unwrap_or_default();
        format!("{spin} {phrase}{elapsed}")
    } else {
        "idle".to_string()
    };
    let pad = w
        .saturating_sub(left.chars().count())
        .saturating_sub(right.chars().count());
    let line = Line::from(vec![
        Span::styled(left, if model.busy { style::spinner() } else { style::bar() }),
        Span::styled(" ".repeat(pad.max(1)), Style::default()),
        Span::styled(right, style::bar()),
    ]);
    frame.render_widget(Paragraph::new(line), area);
}

fn activity_phrase(model: &Model) -> String {
    // Name the running tool when one is in flight; otherwise rotate verbs.
    if let Some(Item::ToolCall { name, state, .. }) = model.focused_transcript().last()
        && matches!(state, super::model::ToolState::Running(_))
    {
        return format!("Running {name}…");
    }
    let verb = VERBS[model.tick_phase % VERBS.len()];
    format!("{verb}…")
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

fn draw_composer(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect, lines: &[String]) {
    let block = Block::bordered()
        .border_type(BorderType::Rounded)
        .border_style(style::border());
    let inner = block.inner(area);
    frame.render_widget(block, area);

    let empty = model.composer.is_empty();
    let paragraph = if empty {
        Paragraph::new(Line::styled(
            "ask something…  (enter sends · alt+enter newline)",
            style::placeholder(),
        ))
    } else {
        Paragraph::new(
            lines
                .iter()
                .map(|l| {
                    if l.starts_with("[Pasted text") {
                        Line::styled(l.clone(), style::paste_block())
                    } else {
                        Line::styled(l.clone(), Style::default())
                    }
                })
                .collect::<Vec<_>>(),
        )
    };
    frame.render_widget(paragraph, inner);

    if !empty {
        let (row, col) = model.composer.cursor_pos(&model.pastes);
        let x = inner.x + col as u16;
        let y = inner.y + row.min(inner.height.saturating_sub(1) as usize) as u16;
        frame.set_cursor_position(Position { x, y });
    }
}

// ---------------------------------------------------------------------------
// Bottom bar: identity + focus + background counts + hints
// ---------------------------------------------------------------------------

fn draw_bottom_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let w = area.width as usize;
    let cfg = model.primary.config();
    let mut left = format!("{} · {}", cfg.model, model.provider_id);
    left.push_str(&format!("  ▸ {}", model.focus_label()));
    if model.bg_procs > 0 {
        left.push_str(&format!(" · {} bg tasks", model.bg_procs));
    }
    if model.bg_agents > 0 {
        left.push_str(&format!(" · {} bg agents", model.bg_agents));
    }
    let right = if model.busy {
        "esc cancel · ^C quit"
    } else {
        "↵ send · ^C quit"
    };
    if let Some((note, _)) = &model.note {
        let mid = format!(" {note} ");
        let pad = w
            .saturating_sub(left.chars().count())
            .saturating_sub(right.chars().count())
            .saturating_sub(mid.chars().count());
        frame.render_widget(
            Paragraph::new(Line::from(vec![
                Span::styled(left, style::bar()),
                Span::styled(" ".repeat((pad / 2).max(1)), Style::default()),
                Span::styled(mid, style::note()),
                Span::styled(" ".repeat((pad / 2).max(1)), Style::default()),
                Span::styled(right, style::bar()),
            ])),
            area,
        );
        return;
    }
    let pad = w
        .saturating_sub(left.chars().count())
        .saturating_sub(right.chars().count());
    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled(left, style::bar()),
            Span::styled(" ".repeat(pad.max(1)), Style::default()),
            Span::styled(right, style::bar()),
        ])),
        area,
    );
}