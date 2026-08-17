//! Pure rendering: model in, frame out. No mutation, no I/O beyond drawing.

use ratatui::Frame;
use ratatui::layout::{Constraint, Layout, Position, Rect};
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Paragraph};
use unicode_width::UnicodeWidthChar;

use super::markdown;
use super::model::{Item, Model, RenderCache};
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
    let composer_lines = model
        .composer
        .lines_with_width(&model.pastes, area.width.saturating_sub(2) as usize);
    let composer_h = (composer_lines.len() as u16 + 2)
        .clamp(3, 10)
        .min(area.height / 2 + 3);
    let chunks = Layout::vertical([
        Constraint::Min(1),
        Constraint::Length(1),
        Constraint::Length(composer_h),
        Constraint::Length(1),
        Constraint::Length(1),
    ])
    .split(area);

    draw_transcript(model, frame, chunks[0]);
    draw_top_bar(model, frame, chunks[1]);
    // Modal inputs own the foreground editing surface. Keep the area in the
    // layout so the modal remains anchored consistently, but do not render the
    // background composer underneath it.
    if model.modal.is_none() {
        draw_composer(model, frame, chunks[2], &composer_lines);
    }
    draw_completion(model, frame, chunks[2]);
    draw_context_bar(model, frame, chunks[3]);
    draw_bottom_bar(model, frame, chunks[4]);
    draw_modal(model, frame, chunks[2]);
}

/// The open modal, anchored above the composer and centered. Rendered last
/// so it overlays transcript and completion; capped so it always fits.
fn draw_modal(model: &Model, frame: &mut Frame, composer_area: Rect) {
    let Some(modal) = &model.modal else {
        return;
    };
    let available_width = composer_area.width.saturating_sub(4);
    let width = modal.width_hint(available_width).min(available_width);
    let mut height = modal.height_hint(width);
    // Never taller than the space above the composer (minus a breath).
    let max_height = composer_area.y.saturating_sub(1);
    if height > max_height {
        height = max_height;
    }
    if height < 3 {
        return;
    }
    let x = composer_area.x + (composer_area.width.saturating_sub(width)) / 2;
    let y = composer_area.y.saturating_sub(height + 1);
    let area = Rect {
        x,
        y,
        width,
        height,
    };
    modal.render(area, frame);
    if let Some((cx, cy)) = modal.cursor(area) {
        frame.set_cursor_position(Position { x: cx, y: cy });
    }
}

fn draw_completion(model: &Model, frame: &mut Frame, composer_area: Rect) {
    if model.modal.is_some() {
        return;
    }
    let Some(completion) = &model.completion else {
        return;
    };
    let height = (completion.items.len() as u16).min(8).saturating_add(2);
    let y = composer_area.y.saturating_sub(height);
    let area = Rect {
        x: composer_area.x,
        y,
        width: composer_area.width,
        height,
    };
    let start = completion.selected.saturating_sub(7);
    let lines = completion
        .items
        .iter()
        .enumerate()
        .skip(start)
        .take(8)
        .map(|(index, item)| {
            let marker = if index == completion.selected {
                "▸ "
            } else {
                "  "
            };
            let style = if index == completion.selected {
                style::user()
            } else {
                style::bar()
            };
            Line::from(vec![
                Span::styled(marker, style),
                Span::styled(item.label.clone(), style),
                Span::styled(format!("  {}", item.detail), style::dim()),
            ])
        })
        .collect::<Vec<_>>();
    frame.render_widget(
        Paragraph::new(lines).block(Block::bordered().border_style(style::border())),
        area,
    );
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

fn item_lines(
    model: &Model,
    item: &Item,
    width: u16,
    delegate_child: Option<&str>,
) -> Vec<Line<'static>> {
    match item {
        Item::User(t) => wrap(&format!("you: {t}"), style::user(), width),
        Item::Text(t) => markdown::render(t, style::assistant()),
        Item::Thinking(t) => markdown::render(t, style::thinking()),
        Item::ToolCall {
            name, args, state, ..
        } => {
            let tail = if name == "bash" && present::bash_mode_shows_output(args) {
                bash_cmdline(args)
                    .and_then(|c| model.host_tails.get(&c))
                    .map(String::as_str)
            } else {
                None
            };
            let nested = delegate_child.map(|child_id| nested_tool_lines(model, child_id, width));
            present::tool_lines_with_window(name, args, state, tail, width, nested.as_deref())
        }
        Item::Note(t) => wrap(t, style::note(), width),
    }
}

fn wrap_lines(lines: Vec<Line<'static>>, width: u16) -> Vec<Line<'static>> {
    let width = usize::from(width).max(1);
    let mut wrapped = Vec::new();
    for line in lines {
        let mut current = Vec::new();
        let mut used = 0usize;
        for span in line.spans {
            for ch in span.content.chars() {
                let char_width = ch.width().unwrap_or(1);
                if used > 0 && used + char_width > width {
                    wrapped.push(Line::from(std::mem::take(&mut current)).style(line.style));
                    used = 0;
                }
                current.push(Span::styled(ch.to_string(), span.style));
                used += char_width;
            }
        }
        wrapped.push(Line::from(current).style(line.style));
    }
    wrapped
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
    if !model.has_agent() {
        let lines = vec![
            Line::styled("Welcome to Firmius", style::assistant()),
            Line::styled("", Style::default()),
            Line::styled(
                "Set up an account with /login, then ask your first question.",
                style::bar(),
            ),
            Line::styled(
                "You can choose the model ahead of time with /model <id>.",
                style::bar(),
            ),
            Line::styled("Resume a saved session with /resume.", style::bar()),
        ];
        frame.render_widget(Paragraph::new(lines), area);
        return;
    }
    let width = area.width;
    let cache_miss = model
        .render_cache
        .borrow()
        .as_ref()
        .is_none_or(|cache| cache.focused_id != model.focused_id || cache.width != width);
    if cache_miss {
        let mut lines: Vec<Line<'static>> = Vec::new();
        let mut delegate_ordinal = 0;
        for item in model.focused_transcript() {
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
            lines.extend(item_lines(model, item, width, child));
        }
        *model.render_cache.borrow_mut() = Some(RenderCache {
            focused_id: model.focused_id.clone(),
            width,
            lines: wrap_lines(lines, width),
        });
    }
    let cache = model.render_cache.borrow();
    let lines = &cache.as_ref().expect("render cache populated").lines;
    let height = area.height as usize;
    let total = lines.len();
    let bottom = total.saturating_sub(height);
    let offset = if model.viewport.follow {
        bottom
    } else {
        bottom.saturating_sub(model.viewport.offset)
    };
    let visible: Vec<Line<'static>> = lines.iter().skip(offset).take(height).cloned().collect();
    frame.render_widget(Paragraph::new(visible), area);
}

fn nested_tool_lines(model: &Model, child_id: &str, width: u16) -> Vec<Line<'static>> {
    let Some(items) = model.transcripts.get(child_id) else {
        return Vec::new();
    };
    items
        .iter()
        .rev()
        .filter_map(|item| {
            let Item::ToolCall {
                name, args, state, ..
            } = item
            else {
                return None;
            };
            let tail = if name == "bash" && present::bash_mode_shows_output(args) {
                bash_cmdline(args)
                    .and_then(|cmd| model.host_tails.get(&cmd))
                    .map(String::as_str)
            } else {
                None
            };
            Some(present::tool_lines(name, args, state, tail, width))
        })
        .take(3)
        .flat_map(|lines| lines.into_iter())
        .collect()
}

// ---------------------------------------------------------------------------
// Top bar: activity + usage
// ---------------------------------------------------------------------------

fn draw_top_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let w = area.width as usize;
    let usage = model.primary.as_ref().map(|agent| agent.usage());
    let right = format!(
        "↑{} ↓{} ⚡{}",
        present::fmt_tokens(usage.as_ref().map_or(0, |u| u.input_tokens)),
        present::fmt_tokens(usage.as_ref().map_or(0, |u| u.output_tokens)),
        present::fmt_tokens(
            usage
                .as_ref()
                .map_or(0, |u| u.cache_read_tokens + u.cache_write_tokens),
        ),
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
        Span::styled(
            left,
            if model.busy {
                style::spinner()
            } else {
                style::bar()
            },
        ),
        Span::styled(" ".repeat(pad.max(1)), Style::default()),
        Span::styled(right, style::bar()),
    ]);
    frame.render_widget(Paragraph::new(line), area);
}

fn activity_phrase(model: &Model) -> String {
    // Name the running tool when one is in flight; otherwise rotate verbs.
    if let Some(Item::ToolCall { name, state, .. }) = model.focused_transcript().last()
        && matches!(
            state,
            super::model::ToolState::Preparing(_) | super::model::ToolState::Running(_)
        )
    {
        return format!("Running {name}…");
    }
    // The frame ticker drives the spinner at ~30fps. Do not use that frame
    // counter for prose or the status text flickers every render.
    let phase = model
        .turn_started
        .map(|started| (started.elapsed().as_secs() / 3) as usize)
        .unwrap_or(0);
    let verb = VERBS[phase % VERBS.len()];
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
        if model.modal.is_none() {
            let (row, col) = model
                .composer
                .cursor_pos_with_width(&model.pastes, inner.width as usize);
            let x = inner.x + col as u16;
            let y = inner.y + row.min(inner.height.saturating_sub(1) as usize) as u16;
            frame.set_cursor_position(Position { x, y });
        }
    }
}

// ---------------------------------------------------------------------------
// Bottom bar: identity + focus + background counts + hints
// ---------------------------------------------------------------------------

fn draw_bottom_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let w = area.width as usize;
    let (provider_id, model_name, effort) = model.focused_model_status();
    let provider_kind = model
        .manager
        .lock()
        .unwrap()
        .provider_kind(&provider_id)
        .map(str::to_string)
        .unwrap_or_else(|| {
            if provider_id.is_empty() {
                "no provider".into()
            } else {
                provider_id
            }
        });
    let mut left = format!("{model_name} · effort {effort} · {provider_kind}");
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

fn draw_context_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let bar = present::progress_bar(model.ctx_used as u64, model.ctx_max as u64, 18);
    let usage = present::format_context_usage(model.ctx_used, model.ctx_max);
    let persona_id = model.focused_persona_id();
    let persona = persona_id
        .as_deref()
        .and_then(|id| model.personas.get(id).map(|p| p.name.as_str()))
        .unwrap_or("Default");
    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled("CTX ", style::bar()),
            Span::styled(bar, style::user()),
            Span::styled(format!(" {usage}"), style::bar()),
            Span::styled(format!(" · persona {persona}"), style::bar()),
        ])),
        area,
    );
}

#[cfg(test)]
mod tests {
    use super::wrap_lines;
    use ratatui::text::Line;

    #[test]
    fn wraps_long_logical_lines_into_scrollable_visual_rows() {
        let wrapped = wrap_lines(vec![Line::from("abcdefghij")], 4);
        assert_eq!(wrapped.len(), 3);
        assert_eq!(wrapped[0].to_string(), "abcd");
        assert_eq!(wrapped[1].to_string(), "efgh");
        assert_eq!(wrapped[2].to_string(), "ij");
    }
}
