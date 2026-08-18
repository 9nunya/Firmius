//! Pure rendering: model in, frame out. No mutation, no I/O beyond drawing.

use ratatui::Frame;
use ratatui::layout::{Constraint, Layout, Position, Rect};
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Clear, Paragraph};
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

use super::markdown;
use super::model::{Item, LivePhraseAnim, Model, RenderCache};
use super::present;
use super::style;

const WELCOME_LOGO: &str = r#"
███████╗██╗██████╗ ███╗   ███╗██╗██╗   ██╗███████╗
██╔════╝██║██╔══██╗████╗ ████║██║██║   ██║██╔════╝
█████╗  ██║██████╔╝██╔████╔██║██║██║   ██║███████╗
██╔══╝  ██║██╔══██╗██║╚██╔╝██║██║██║   ██║╚════██║
██║     ██║██║  ██║██║ ╚═╝ ██║██║╚██████╔╝███████║
╚═╝     ╚═╝╚═╝  ╚═╝╚═╝     ╚═╝╚═╝ ╚═════╝ ╚══════╝
"#;
const PHRASE_STEP_TICKS: usize = 2;

pub fn draw(model: &Model, frame: &mut Frame) {
    let area = frame.area();
    let composer_lines = model
        .composer
        .lines_with_width(&model.pastes, area.width.saturating_sub(2) as usize);
    let composer_h = (composer_lines.len() as u16 + 2)
        .clamp(3, 10)
        .min(area.height / 2 + 3);
    let pending_h = model
        .agents
        .get(&model.focused_id)
        .map(|agent| agent.pending_messages().len() as u16)
        .unwrap_or(0);
    let chunks = Layout::vertical([
        Constraint::Min(1),
        Constraint::Length(1),
        Constraint::Length(pending_h),
        Constraint::Length(composer_h),
        Constraint::Length(1),
        Constraint::Length(1),
    ])
    .split(area);

    draw_transcript(model, frame, chunks[0]);
    draw_top_bar(model, frame, chunks[1]);
    draw_pending_messages(model, frame, chunks[2]);
    // Modal inputs own the foreground editing surface. Keep the area in the
    // layout so the modal remains anchored consistently, but do not render the
    // background composer underneath it.
    if model.modal.is_none() {
        draw_composer(model, frame, chunks[3], &composer_lines);
    }
    draw_completion(model, frame, chunks[3]);
    draw_context_bar(model, frame, chunks[4]);
    draw_bottom_bar(model, frame, chunks[5]);
    draw_modal(model, frame, chunks[3]);
}

fn composer_scroll_offset(cursor_row: usize, line_count: usize, visible_rows: usize) -> usize {
    let max_offset = line_count.saturating_sub(visible_rows);
    cursor_row
        .saturating_sub(visible_rows.saturating_sub(1))
        .min(max_offset)
}

fn truncate_width(text: &str, width: usize) -> String {
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

fn gradient_text_spans(text: &str, theme: &super::theme::Theme, phase: f32) -> Vec<Span<'static>> {
    let chars: Vec<char> = text.chars().collect();
    let len = chars.len().max(1);
    chars
        .into_iter()
        .enumerate()
        .map(|(index, ch)| {
            Span::styled(
                ch.to_string(),
                Style::new().fg(super::theme::gradient_at(theme, phase, len, index)).bold(),
            )
        })
        .collect()
}

fn chip(
    text: impl Into<String>,
    fg: ratatui::style::Color,
    bg: ratatui::style::Color,
) -> Span<'static> {
    Span::styled(format!(" {text} ", text = text.into()), Style::new().fg(fg).bg(bg))
}

fn padded_plain(width: usize, text: &str) -> Line<'static> {
    let mut padded = text.to_string();
    let used = padded.width();
    if used < width {
        padded.push_str(&" ".repeat(width - used));
    }
    Line::styled(padded, Style::default())
}

fn animated_phrase_spans(model: &Model, theme: &super::theme::Theme) -> Vec<Span<'static>> {
    let gradient_phase = (model.tick_phase as f32 / 18.0) % 1.0;
    let chars = match &model.live_phrase_anim {
        LivePhraseAnim::Steady => model.live_phrase.chars().collect::<Vec<_>>(),
        LivePhraseAnim::FadingOut { from, .. } => from.chars().collect::<Vec<_>>(),
        LivePhraseAnim::FadingIn { to, .. } => to.chars().collect::<Vec<_>>(),
    };
    let len = chars.len().max(1);
    let mut fade_edge = None;
    let visible = match &model.live_phrase_anim {
        LivePhraseAnim::Steady => chars.len(),
        LivePhraseAnim::FadingOut { started_tick, .. } => {
            let elapsed = model.tick_phase.saturating_sub(*started_tick);
            let removed = elapsed / PHRASE_STEP_TICKS;
            let partial = (elapsed % PHRASE_STEP_TICKS) as f32 / PHRASE_STEP_TICKS as f32;
            let visible = chars.len().saturating_sub(removed);
            if visible > 0 {
                fade_edge = Some((visible - 1, 1.0 - partial));
            }
            visible
        }
        LivePhraseAnim::FadingIn { started_tick, .. } => {
            let elapsed = model.tick_phase.saturating_sub(*started_tick);
            let grown = elapsed / PHRASE_STEP_TICKS;
            let partial = (elapsed % PHRASE_STEP_TICKS) as f32 / PHRASE_STEP_TICKS as f32;
            let visible = (grown + 1).min(chars.len());
            if visible > 0 {
                fade_edge = Some((visible - 1, partial));
            }
            visible
        }
    };
    chars
        .into_iter()
        .take(visible)
        .enumerate()
        .map(|(index, ch)| {
            let mut color = super::theme::gradient_at(theme, gradient_phase, len, index);
            if let Some((edge, amount)) = fade_edge
                && edge == index
            {
                color = super::theme::lerp_color(theme.bg, color, amount);
            }
            Span::styled(ch.to_string(), Style::new().fg(color).bold())
        })
        .collect()
}

fn draw_pending_messages(model: &Model, frame: &mut Frame, area: Rect) {
    let Some(agent) = model.agents.get(&model.focused_id) else {
        return;
    };
    let messages = agent.pending_messages();
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

/// The open modal, anchored above the composer and centered. Rendered last
/// so it overlays transcript and completion; capped so it always fits.
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
    // Modal widgets intentionally leave unused inner cells untouched. Clear the
    // full popup rectangle first so transcript text cannot bleed through them.
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
    let height = (completion.items.len() as u16).min(8).saturating_add(2);
    let y = composer_area.y.saturating_sub(height);
    let area = Rect {
        x: composer_area.x,
        y,
        width: composer_area.width,
        height,
    };
    let start = completion.selected.saturating_sub(7);
    let inner_width = area.width.saturating_sub(2) as usize;
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
                "▸ "
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
    frame.render_widget(
        Paragraph::new(lines).block(Block::bordered().border_style(style::border(theme))),
        area,
    );
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
        } else if rows.is_empty() && !pending_space.is_empty() && word_width + space_width <= width {
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

fn user_block(text: &str, theme: &super::theme::Theme, width: u16) -> Vec<Line<'static>> {
    let card_style = Style::new().fg(theme.fg).bg(theme.dim_bg);
    let width = usize::from(width).max(1);
    let fill = |text: String| Line::from(vec![Span::styled(text, card_style)]);
    let mut lines = vec![fill(" ".repeat(width))];
    for line in wrap(text, card_style, width as u16) {
        let text = line.to_string();
        let used = text.width();
        let mut padded = text;
        if used < width {
            padded.push_str(&" ".repeat(width - used));
        }
        lines.push(fill(padded));
    }
    lines.push(fill(" ".repeat(width)));
    lines
}

fn item_lines(
    model: &Model,
    item: &Item,
    width: u16,
    delegate_child: Option<&str>,
) -> Vec<Line<'static>> {
    let theme = &model.theme;
    match item {
        Item::User(t) => user_block(t, theme, width),
        Item::Text(t) => markdown::render(t, style::assistant(theme), theme),
        Item::Thinking(t) => markdown::render(t, style::thinking(theme), theme),
        Item::ToolCall {
            name, args, result, state, ..
        } => {
            let related_intent = match name.as_str() {
                "bash" => serde_json::from_str::<serde_json::Value>(args)
                    .ok()
                    .and_then(|value| value.get("proc_id").and_then(|v| v.as_str()).map(str::to_owned))
                    .and_then(|id| model.proc_intents.get(&id).cloned()),
                "delegate" => serde_json::from_str::<serde_json::Value>(args)
                    .ok()
                    .and_then(|value| value.get("delegate_id").and_then(|v| v.as_str()).map(str::to_owned))
                    .and_then(|id| model.delegate_intents.get(&id).cloned()),
                _ => None,
            };
            let tail = if name == "bash" && present::bash_mode_shows_output(args) {
                result
                    .as_deref()
                    .and_then(|result| present::proc_id_from_result(Some(result)))
                    .and_then(|id| model.host_tails.get(&id))
                    .map(String::as_str)
            } else {
                None
            };
            let nested = delegate_child.map(|child_id| nested_tool_lines(model, child_id, width));
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
                _ => present::tool_lines(name, args, state, tail, width, theme),
            };
            if let Some(nested) = nested.as_deref() {
                lines.extend(nested.iter().cloned().map(|mut line| {
                    let mut spans = vec![Span::styled("  │ ", style::dim(theme))];
                    spans.append(&mut line.spans);
                    Line::from(spans)
                }));
            }
            lines
        }
        Item::Note(t) => wrap(t, style::note(theme), width),
    }
}

fn quick_group_kind(item: &Item) -> Option<&'static str> {
    match item {
        Item::ToolCall { name, .. } if name == "read" => Some("read"),
        Item::ToolCall { name, .. } if name == "list" => Some("list"),
        _ => None,
    }
}

fn grouped_quick_tool_lines(items: &[&Item], kind: &str, width: u16, theme: &super::theme::Theme) -> Vec<Line<'static>> {
    let prefix = match kind {
        "read" => "read ",
        "list" => "listed ",
        _ => "",
    };
    let indent = " ".repeat(prefix.width());
    let mut rendered = Vec::new();
    let mut current = prefix.to_string();
    let max_width = width as usize;
    for (index, item) in items.iter().enumerate() {
        let Some(label) = grouped_quick_tool_label(item, kind) else {
            continue;
        };
        let chunk = if index + 1 < items.len() {
            format!("{label}, ")
        } else {
            label
        };
        let candidate = format!("{current}{chunk}");
        if current.width() > prefix.width() && candidate.width() > max_width {
            rendered.push(Line::from(vec![
                Span::styled(current.clone(), style::dim(theme)),
            ]));
            current = format!("{indent}{chunk}");
        } else {
            current.push_str(&chunk);
        }
    }
    if !current.is_empty() {
        let (head, tail) = current.split_at(prefix.len().min(current.len()));
        rendered.push(Line::from(vec![
            Span::styled(head.to_string(), style::tool(theme)),
            Span::styled(tail.to_string(), style::dim(theme)),
        ]));
    }
    rendered
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
                .or_else(|| value.get("offset").and_then(|v| v.as_u64()).map(|v| v as usize + 1));
            let limit = value
                .get("limit")
                .and_then(|v| v.as_u64())
                .map(|v| v as usize)
                .or_else(|| value.get("max_lines").and_then(|v| v.as_u64()).map(|v| v as usize));
            let end_line = value.get("end_line").and_then(|v| v.as_u64()).map(|v| v as usize);
            let summary = match (start, limit, end_line) {
                (_, _, Some(end)) => format!("{path}:{}-{end}", start.unwrap_or(1)),
                (Some(start), Some(limit), None) if limit != usize::MAX => {
                    format!("{path}:{start}-{}", start.saturating_add(limit.saturating_sub(1)))
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
    spans.push(Span::raw(" "));
    spans.extend(line.spans);
    spans.push(Span::raw(" "));
    Line::from(spans).style(line.style)
}

fn item_is_active_tool(item: &Item) -> bool {
    matches!(
        item,
        Item::ToolCall {
            state: super::model::ToolState::Preparing(_) | super::model::ToolState::Running(_),
            ..
        }
    )
}

fn transcript_has_active_tools(model: &Model, agent_id: &str) -> bool {
    let Some(items) = model.transcripts.get(agent_id) else {
        return false;
    };
    let mut delegate_ordinal = 0;
    for item in items {
        if item_is_active_tool(item) {
            return true;
        }
        if let Item::ToolCall {
            name, stream_id, ..
        } = item
            && name == "delegate"
        {
            if let Some(child_id) =
                model.delegate_child(agent_id, delegate_ordinal, stream_id.as_deref())
                && transcript_has_active_tools(model, child_id)
            {
                return true;
            }
            delegate_ordinal += 1;
        }
    }
    false
}

fn draw_transcript(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let theme = &model.theme;
    if !model.has_agent() {
        let width = area.width as usize;
        let mut lines = Vec::new();
        for (idx, raw) in WELCOME_LOGO.trim_matches('\n').lines().enumerate() {
            let logo_width = raw.width();
            let left_pad = width.saturating_sub(logo_width) / 2;
            let mut spans = vec![Span::raw(" ".repeat(left_pad))];
            spans.extend(gradient_text_spans(raw, theme, (idx as f32 / 8.0) % 1.0));
            lines.push(Line::from(spans));
        }
        lines.push(padded_plain(width, ""));
        lines.push(Line::styled(
            format!("  Welcome back. Theme: {}  ", theme.name),
            Style::new().fg(theme.fg),
        ));
        lines.push(Line::styled(
            "  /login to connect a provider, then ask your first question.  ".to_string(),
            style::assistant(theme),
        ));
        lines.push(Line::styled(
            "  /model to pick a model, /theme to switch colors, /resume to reopen a session.  ".to_string(),
            style::bar(theme),
        ));
        lines.push(Line::styled(
            "  Shift-Tab cycles personas before the first turn.  ".to_string(),
            style::bar(theme),
        ));
        frame.render_widget(Paragraph::new(lines), area);
        return;
    }
    let width = area.width;
    let content_width = width.saturating_sub(2);
    let animation_epoch = transcript_has_active_tools(model, &model.focused_id)
        .then_some(model.tick_phase / 2);
    let cache_miss = model
        .render_cache
        .borrow()
        .as_ref()
        .is_none_or(|cache| {
            cache.focused_id != model.focused_id
                || cache.width != width
                || cache.animation_epoch != animation_epoch
        });
    if cache_miss {
        let mut lines: Vec<Line<'static>> = Vec::new();
        let mut delegate_ordinal = 0;
        let transcript = model.focused_transcript();
        let mut index = 0;
        while index < transcript.len() {
            let item = &transcript[index];
            if let Some(kind) = quick_group_kind(item) {
                let mut grouped = vec![item];
                let mut next = index + 1;
                while next < transcript.len() && quick_group_kind(&transcript[next]) == Some(kind) {
                    grouped.push(&transcript[next]);
                    next += 1;
                }
                if grouped.len() > 1 {
                    lines.extend(grouped_quick_tool_lines(&grouped, kind, content_width, theme));
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
            lines.extend(item_lines(model, item, content_width, child));
            index += 1;
        }
        *model.render_cache.borrow_mut() = Some(RenderCache {
            focused_id: model.focused_id.clone(),
            width,
            animation_epoch,
            lines: wrap_lines(lines, content_width)
                .into_iter()
                .map(add_gutter)
                .collect(),
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
                name, args, result, state, ..
            } = item
            else {
                return None;
            };
            let tail = if name == "bash" && present::bash_mode_shows_output(args) {
                result
                    .as_deref()
                    .and_then(|result| present::proc_id_from_result(Some(result)))
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
                "edit" => present::edit_lines_compact(args, state, width, &model.theme, 3),
                _ => present::tool_lines(name, args, state, tail, width, &model.theme),
            })
        })
        .take(3)
        .flat_map(|lines| lines.into_iter())
        .collect()
}

// ---------------------------------------------------------------------------
// Top bar: activity + usage
// ---------------------------------------------------------------------------

fn draw_top_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let theme = &model.theme;
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
    let mut spans = if model.busy {
        let frame_idx = model.tick_phase % style::SPINNER.len();
        let spin = style::SPINNER[frame_idx];
        let elapsed = model
            .turn_started
            .map(|t| format!(" {}s", present::elapsed_secs(t)))
            .unwrap_or_default();
        let mut spans = gradient_text_spans(spin, theme, (model.tick_phase as f32 / 18.0) % 1.0);
        spans.push(Span::raw(" "));
        spans.extend(animated_phrase_spans(model, theme));
        spans.push(Span::styled(elapsed, style::bar(theme)));
        spans
    } else {
        vec![Span::styled("idle", style::bar(theme))]
    };
    let left_width = spans.iter().map(Span::width).sum::<usize>();
    let pad = w.saturating_sub(left_width).saturating_sub(right.chars().count());
    spans.push(Span::styled(" ".repeat(pad.max(1)), Style::default()));
    spans.push(Span::styled(right, style::bar(theme)));
    let line = Line::from(spans);
    frame.render_widget(Paragraph::new(line), area);
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

fn draw_composer(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect, lines: &[String]) {
    let theme = &model.theme;
    let inner_height = area.height.saturating_sub(2) as usize;
    let (cursor_row, _) = model
        .composer
        .cursor_pos_with_width(&model.pastes, area.width.saturating_sub(2) as usize);
    let max_offset = lines.len().saturating_sub(inner_height);
    let scroll_offset = composer_scroll_offset(cursor_row, lines.len(), inner_height);
    let clipped_above = scroll_offset > 0;
    let clipped_below = scroll_offset < max_offset;
    let indicator = match (clipped_above, clipped_below) {
        (true, true) => Some("⋯"),
        (true, false) => Some("↑"),
        (false, true) => Some("↓"),
        (false, false) => None,
    };
    let mut block = Block::bordered()
        .border_type(BorderType::Rounded)
        .border_style(style::border(theme));
    if let Some(indicator) = indicator {
        block = block.title_bottom(Line::styled(indicator, style::dim(theme)));
    }
    let inner = block.inner(area);
    frame.render_widget(block, area);

    let empty = model.composer.is_empty();
    let paragraph = if empty {
        Paragraph::new(Line::styled(
            "ask something…  (enter sends · alt+enter newline)",
            style::placeholder(theme),
        ))
    } else {
        Paragraph::new(
            lines
                .iter()
                .map(|l| {
                    if l.starts_with("[Pasted text") {
                        Line::styled(l.clone(), style::paste_block(theme))
                    } else {
                        Line::styled(l.clone(), Style::default())
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
                .cursor_pos_with_width(&model.pastes, inner.width as usize);
            let x = inner.x + col as u16;
            let y = inner.y
                + row
                    .saturating_sub(scroll_offset)
                    .min(inner.height.saturating_sub(1) as usize) as u16;
            frame.set_cursor_position(Position { x, y });
        }
    }
}

// ---------------------------------------------------------------------------
// Bottom bar: identity + focus + background counts + hints
// ---------------------------------------------------------------------------

fn draw_bottom_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let theme = &model.theme;
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
    let mut left = format!("{} · {} · {} · theme {}", model_name, effort, provider_kind, theme.name);
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
    let note = model.note.as_ref().map(|(note, _)| note.as_str()).unwrap_or("");
    let left_text = truncate_width(&left, w / 2 + 12);
    let right_text = right.to_string();
    let note_text = if note.is_empty() {
        String::new()
    } else {
        format!("  {note}  ")
    };
    let pad = w
        .saturating_sub(left_text.width())
        .saturating_sub(note_text.width())
        .saturating_sub(right_text.width());
    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled(left_text, style::bar(theme)),
            Span::styled(" ".repeat((pad / 2).max(1)), Style::default()),
            Span::styled(note_text, style::note(theme)),
            Span::styled(" ".repeat((pad / 2).max(1)), Style::default()),
            Span::styled(right_text, style::bar(theme)),
        ])),
        area,
    );
}

fn draw_context_bar(model: &Model, frame: &mut Frame, area: ratatui::layout::Rect) {
    let theme = &model.theme;
    let bar = present::progress_bar(model.ctx_used as u64, model.ctx_max as u64, 18);
    let usage = present::format_context_usage(model.ctx_used, model.ctx_max);
    let ratio = if model.ctx_max == 0 {
        0.0
    } else {
        model.ctx_used as f32 / model.ctx_max as f32
    };
    let bar_color = if ratio < 0.8 {
        super::theme::lerp_color(theme.gradient_lo, theme.gradient_hi, ratio / 0.8)
    } else {
        super::theme::lerp_color(theme.gradient_hi, theme.warn, ((ratio - 0.8) / 0.2).clamp(0.0, 1.0))
    };
    let persona_id = model.focused_persona_id();
    let persona = persona_id
        .as_deref()
        .and_then(|id| model.personas.get(id).map(|p| p.name.as_str()))
        .unwrap_or("Default");
    frame.render_widget(
        Paragraph::new(Line::from(vec![
            chip("CTX", theme.bg, theme.border),
            Span::raw(" "),
            Span::styled(bar, Style::new().fg(bar_color).bold()),
            Span::styled(format!(" {usage}"), style::bar(theme)),
            Span::styled("  ", Style::default()),
            chip(format!("persona {persona}"), theme.fg, theme.selection_bg),
        ])),
        area,
    );
}

#[cfg(test)]
mod tests {
    use super::{composer_scroll_offset, draw_modal_surface, wrap_lines};
    use crate::tui::modal::{ModalAction, ModalSurface};
    use crate::tui::theme;
    use async_trait::async_trait;
    use crossterm::event::KeyEvent;
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;
    use ratatui::layout::Rect;
    use ratatui::style::Style;
    use ratatui::text::Line;
    use ratatui::widgets::{Block, Paragraph};

    struct SparseModal;

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

        fn render(&self, area: Rect, frame: &mut ratatui::Frame, _theme: &super::super::theme::Theme) {
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
    fn user_card_fills_short_and_padding_rows_with_dim_background() {
        let theme = theme::default_theme();
        let lines = super::user_block("hello\nworld", &theme, 10);
        assert_eq!(lines.len(), 4);
        assert!(lines.iter().all(|line| line.width() == 10));
        assert!(lines.iter().all(|line| {
            line.spans
                .iter()
                .all(|span| span.style.bg == Some(theme.dim_bg))
        }));
        assert_eq!(lines[1].to_string(), "hello     ");
        assert_eq!(lines[0].to_string(), "          ");
        assert_eq!(lines[3].to_string(), "          ");

        let backend = TestBackend::new(10, 4);
        let mut terminal = Terminal::new(backend).unwrap();
        terminal
            .draw(|frame| frame.render_widget(Paragraph::new(lines), frame.area()))
            .unwrap();
        for y in 0..4 {
            for x in 0..10 {
                assert_eq!(
                    terminal.backend().buffer().cell((x, y)).unwrap().style().bg,
                    Some(theme.dim_bg)
                );
            }
        }
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

        // Popup is x=10..30 and y=9..14. Its untouched interior must be blank,
        // rather than retaining the transcript's X cells.
        assert_eq!(
            terminal.backend().buffer().cell((11, 10)).unwrap().symbol(),
            " "
        );
    }
}
