//! Per-tool presentation rendering.
//!
//! Contract (pinned): given a tool call's name, its JSON args, its state, an
//! optional live output tail (for bash processes the app loop polls), and
//! the pane width — produce styled lines. Rich per-tool presentations live
//! here (bash live tail, delegate prompt excerpt, edit mini diff); unknown
//! tools and malformed args fall back to the generic shape.

use ratatui::style::Style;
use ratatui::text::{Line, Span};
use firmius_core::partial_json::PartialJson;
use std::sync::OnceLock;
use syntect::easy::HighlightLines;
use syntect::highlighting::{Color as SynColor, Theme as SyntectTheme, ThemeSet};
use syntect::parsing::{SyntaxReference, SyntaxSet};
use unicode_width::UnicodeWidthChar;
use unicode_width::UnicodeWidthStr;

use super::model::ToolState;
use super::style;
use super::theme::Theme;
use std::time::Instant;

/// Max live output lines shown beneath a running bash call.
const BASH_TAIL_MAX: usize = 3;
/// Render a compact proportional progress bar using the shared quota/usage
/// glyphs. This is intentionally a string helper so modals and status bars can
/// compose it with their own labels and styles.
pub fn progress_bar(used: u64, max: u64, width: usize) -> String {
    let width = width.max(1);
    let filled = if max == 0 {
        0
    } else {
        ((used.min(max) * width as u64) / max) as usize
    };
    format!("{}{}", "▰".repeat(filled), "▱".repeat(width - filled))
}

/// Return a stable process id from a tool result (`proc_id=<uuid>`).
/// Presentation callers own the id-keyed tail map; this helper remains
/// independent of the model so the renderer is easy to test.
pub fn proc_id_from_result(result: Option<&str>) -> Option<firmius_core::ProcId> {
    let value = result?.lines().find_map(|line| {
        line.split_whitespace()
            .find_map(|word| word.strip_prefix("proc_id="))
    })?;
    value.parse().ok()
}

pub fn format_context_usage(used: u32, max: u32) -> String {
    let used_k = used / 1_000;
    let max_display = if max < 1_000_000 {
        format!("{}k", max / 1_000)
    } else {
        format!("{}M", max / 1_000_000)
    };
    format!("{used_k}k/{max_display}")
}

/// Render one tool call as styled transcript lines.
pub fn tool_lines(
    name: &str,
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    tool_lines_with_window(name, args, state, tail, width, None, theme)
}

pub fn tool_lines_with_window(
    name: &str,
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    nested: Option<&[Line<'static>]>,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let mut lines = match name {
        "bash" => bash_lines(args, state, tail, width, theme),
        "delegate" => delegate_lines(args, state, width, theme),
        "edit" => edit_lines(args, state, width, theme),
        "read" | "list" | "grep" | "glob" => quick_lines(name, args, state, width, theme),
        _ => generic_lines(name, args, state, tail, width, theme),
    };
    if let Some(nested) = nested {
        lines.extend(nested.iter().cloned().map(|mut line| {
            let mut spans = vec![Span::styled("  │ ", style::dim(theme))];
            spans.append(&mut line.spans);
            Line::from(spans)
        }));
    }
    lines
}

pub fn bash_lines_progressive(
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
    related_intent: Option<&str>,
) -> Vec<Line<'static>> {
    let tail = if bash_mode_shows_output(args) { tail } else { None };
    let parsed = PartialJson::parse(args);
    let mode = parsed.str("mode").unwrap_or("exec");
    let label = bash_progress_label(&parsed, mode, related_intent)
        .or_else(|| bash_cmdline(args))
        .unwrap_or_else(|| describe_args_live("bash", args, width as usize));
    status_line(&label, state, tail, width, theme)
}

pub fn delegate_lines_progressive(
    args: &str,
    state: &ToolState,
    width: u16,
    theme: &Theme,
    related_intent: Option<&str>,
) -> Vec<Line<'static>> {
    let parsed = PartialJson::parse(args);
    let mode = parsed.str("mode").unwrap_or("run");
    let label = delegate_progress_label(&parsed, mode, state, related_intent)
        .unwrap_or_else(|| "delegating".to_string());
    status_line(&label, state, None, width, theme)
}

pub fn edit_lines_compact(
    args: &str,
    state: &ToolState,
    width: u16,
    theme: &Theme,
    max_lines: usize,
) -> Vec<Line<'static>> {
    let patch = partial_string_field(args, "patch").unwrap_or_default();
    let files = edit_compact_entries(&patch);
    let summary = match files.len() {
        0 => "editing".to_string(),
        1 => format!("editing {}", files[0].0),
        n => format!("editing {n} files"),
    };
    let mut out = status_line(&summary, state, None, width, theme);
    if max_lines <= 1 || files.is_empty() {
        out.truncate(max_lines.max(1));
        return out;
    }
    let mut row = String::new();
    let mut extra = Vec::new();
    let max_width = width as usize;
    let total_files = files.len();
    let mut seen_files = 0usize;
    for (path, added, removed) in &files {
        seen_files += 1;
        let entry = format!("{path} +{added} -{removed}");
        let next = if row.is_empty() {
            entry.clone()
        } else {
            format!("{row}  ·  {entry}")
        };
        if !row.is_empty() && next.width() > max_width {
            extra.push(Line::from(vec![
                Span::styled(row.clone(), style::dim(theme)),
            ]));
            row = entry;
        } else {
            row = next;
        }
        if out.len() + extra.len() >= max_lines {
            break;
        }
    }
    if !row.is_empty() && out.len() + extra.len() < max_lines {
        extra.push(Line::from(vec![Span::styled(row, style::dim(theme))]));
    }
    let remaining = total_files.saturating_sub(seen_files);
    out.extend(extra.into_iter().take(max_lines.saturating_sub(out.len())));
    if remaining > 0 && out.len() < max_lines {
        out.push(Line::from(vec![Span::styled(
            format!("+{remaining} more files"),
            style::dim(theme),
        )]));
    }
    out.truncate(max_lines.max(1));
    out
}

// ---------------------------------------------------------------------------
// Per-tool renderers
// ---------------------------------------------------------------------------

/// bash: the command line is the headline; live output tail while running.
fn bash_lines(args: &str, state: &ToolState, tail: Option<&str>, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    let tail = if bash_mode_shows_output(args) {
        tail
    } else {
        None
    };
    let Some(cmd) = bash_cmdline(args) else {
        return generic_lines("bash", args, state, tail, width, theme);
    };
    match state {
        ToolState::Preparing(started) | ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            let mut out = vec![Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(trunc(&cmd, budget_for(width, 2, &suffix)), style::tool(theme)),
                Span::styled(suffix, style::dim(theme)),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            out
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            let mut out = vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(trunc(&cmd, budget_for(width, 2, &suffix)), style::tool(theme)),
                Span::styled(suffix, style::dim(theme)),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            out
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(trunc(&cmd, budget_for(width, 2, "")), style::tool(theme)),
        ])],
    }
}

fn bash_progress_label(parsed: &PartialJson, mode: &str, related_intent: Option<&str>) -> Option<String> {
    match mode {
        "list" => Some("listing processes".to_string()),
        "wait" => Some(match related_intent {
            Some(intent) => format!("waiting for \"{intent}\""),
            None => "waiting for process".to_string(),
        }),
        "poll" => Some(match related_intent {
            Some(intent) => format!("polling \"{intent}\""),
            None => "polling process".to_string(),
        }),
        "kill" => Some(match related_intent {
            Some(intent) => format!("killing \"{intent}\""),
            None => "killing process".to_string(),
        }),
        "input" => Some("sending input to process".to_string()),
        "resize" => Some("resizing process".to_string()),
        "exec" | "spawn" => parsed
            .str("intent")
            .map(|intent| format!("{mode} \"{}\"", one_line(intent))),
        _ => None,
    }
}

pub fn bash_mode_shows_output(args: &str) -> bool {
    let mode = serde_json::from_str::<serde_json::Value>(args)
        .ok()
        .and_then(|value| {
            value
                .get("mode")
                .and_then(|mode| mode.as_str())
                .map(str::to_string)
        })
        .unwrap_or_else(|| "exec".into());
    matches!(mode.as_str(), "exec" | "spawn")
}

/// delegate: a one-line excerpt of the instruction it was given.
fn delegate_lines(args: &str, state: &ToolState, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    const LABEL: &str = "delegate";
    let Some(prompt) =
        parse_args(args).and_then(|v| v.get("prompt").and_then(|p| p.as_str()).map(one_line))
    else {
        return generic_lines(LABEL, args, state, None, width, theme);
    };
    match state {
        ToolState::Preparing(started) | ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            let fixed = 2 + LABEL.len() + 1;
            // TODO(subagent-window): once per-subagent tool-call history is
            // plumbed into the model, append the subagent's last 3 tool calls
            // here as dim nested lines (same shape as the bash tail). No such
            // data source exists yet — do not invent one.
            vec![Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(LABEL.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    trunc(&prompt, budget_for(width, fixed, &suffix)),
                    style::dim(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])]
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(trunc(&prompt, budget_for(width, 2, &suffix)), style::dim(theme)),
                Span::styled(suffix, style::dim(theme)),
            ])]
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(trunc(&prompt, budget_for(width, 2, "")), style::dim(theme)),
        ])],
    }
}

fn delegate_progress_label(
    parsed: &PartialJson,
    mode: &str,
    state: &ToolState,
    related_intent: Option<&str>,
) -> Option<String> {
    match mode {
        "wait" => Some(match related_intent {
            Some(intent) => format!("waiting for \"{intent}\""),
            None => "waiting for delegate".to_string(),
        }),
        "send" => {
            let target = parsed.str("target").unwrap_or("child");
            let message = parsed.str("message").map(one_line).unwrap_or_else(|| "message".into());
            Some(format!("messaging {target}: \"{message}\""))
        }
        _ => {
            let intent = parsed.str("intent").map(one_line);
            let persona = parsed.str("persona");
            let mut label = match (persona, intent.as_deref()) {
                (Some(persona), Some(intent)) => format!("delegating to {persona}: \"{intent}\""),
                (None, Some(intent)) => format!("delegating \"{intent}\""),
                _ => "delegating".to_string(),
            };
            if matches!(state, ToolState::Done { .. }) {
                label = label.replacen("delegating", "delegated", 1);
                let model = parsed.complete_str("model").or_else(|| parsed.str("model"));
                let effort = parsed.complete_str("effort").or_else(|| parsed.str("effort"));
                match (model, effort) {
                    (Some(model), Some(effort)) => label.push_str(&format!(" [{model}, {effort}]")),
                    (Some(model), None) => label.push_str(&format!(" [{model}]")),
                    _ => {}
                }
            }
            Some(label)
        }
    }
}

fn status_line(
    label: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    match state {
        ToolState::Preparing(started) | ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            let mut out = vec![Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(trunc(label, budget_for(width, 2, &suffix)), style::dim(theme)),
                Span::styled(suffix, style::dim(theme)),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            out
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            let mut out = vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(trunc(label, budget_for(width, 2, &suffix)), style::dim(theme)),
                Span::styled(suffix, style::dim(theme)),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            out
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(trunc(label, budget_for(width, 2, "")), style::dim(theme)),
        ])],
    }
}

/// edit: a header naming the touched files, then a capped mini diff.
fn edit_lines(args: &str, state: &ToolState, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    const NAME: &str = "edit";
    let patch = partial_string_field(args, "patch").unwrap_or_default();
    let summary = edit_file_summary(&patch);
    let fixed = 2 + NAME.len() + 1; // glyph+space, name, separating space
    let head = match state {
        ToolState::Preparing(started) | ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(NAME.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    trunc(&summary, budget_for(width, fixed, &suffix)),
                    style::dim(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(NAME.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    trunc(&summary, budget_for(width, fixed, &suffix)),
                    style::dim(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])
        }
        ToolState::Interrupted => Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(NAME.to_string(), style::tool(theme)),
            Span::raw(" "),
            Span::styled(trunc(&summary, budget_for(width, fixed, "")), style::dim(theme)),
        ]),
    };
    let mut out = vec![head];
    out.extend(edit_diff_lines(&patch, state, width, theme));
    out
}

fn edit_compact_entries(patch: &str) -> Vec<(String, usize, usize)> {
    let mut out = Vec::new();
    let mut current: Option<usize> = None;
    for raw in patch.lines() {
        let trimmed = raw.trim();
        if let Some(path) = trimmed
            .strip_prefix("*** Add File:")
            .or_else(|| trimmed.strip_prefix("*** Update File:"))
            .or_else(|| trimmed.strip_prefix("*** Delete File:"))
        {
            out.push((path.trim().to_string(), 0, 0));
            current = out.len().checked_sub(1);
            continue;
        }
        let Some(index) = current else { continue };
        if raw.starts_with('+') && !raw.starts_with("+++") {
            out[index].1 += 1;
        } else if raw.starts_with('-') && !raw.starts_with("---") {
            out[index].2 += 1;
        }
    }
    out
}

/// read/list/grep/glob: one dense line — glyph, name, args summary, bytes.
fn quick_lines(name: &str, args: &str, state: &ToolState, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    let fixed = 2 + name.chars().count() + 1; // glyph+space, name, separating space
    let line = match state {
        ToolState::Preparing(_) | ToolState::Running(_) => Line::from(vec![
            Span::styled(tool_icon(state), tool_icon_style(state, theme)),
            Span::styled(name.to_string(), style::tool(theme)),
            Span::raw(" "),
            Span::styled(
                describe_args_live(name, args, budget_for(width, fixed, "")),
                style::dim(theme),
            ),
        ]),
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(name.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    describe_args_live(name, args, budget_for(width, fixed, &suffix)),
                    style::dim(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])
        }
        ToolState::Interrupted => Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(name.to_string(), style::tool(theme)),
            Span::raw(" "),
            Span::styled(
                describe_args(args, budget_for(width, fixed, "")),
                style::dim(theme),
            ),
        ]),
    };
    vec![line]
}

/// Fallback shape for unknown tools (and known tools with malformed args).
fn generic_lines(
    name: &str,
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let fixed = 2 + name.chars().count() + 1;
    match state {
        ToolState::Preparing(started) | ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            let head = Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(name.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    describe_args_live(name, args, budget_for(width, fixed, &suffix)),
                    style::dim(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ]);
            let mut out = vec![head];
            if let Some(tail) = tail {
                for line in tail_lines(tail, BASH_TAIL_MAX) {
                    out.push(Line::styled(
                        trunc(&format!("  │ {line}"), width as usize),
                        style::dim(theme),
                    ));
                }
            }
            out
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(name.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    describe_args(args, budget_for(width, fixed, &suffix)),
                    style::dim(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])]
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(name.to_string(), style::tool(theme)),
            Span::raw(" "),
            Span::styled(
                describe_args_live(name, args, budget_for(width, fixed, "")),
                style::dim(theme),
            ),
        ])],
    }
}

// ---------------------------------------------------------------------------
// Helpers (pure, testable)
// ---------------------------------------------------------------------------

/// Truncate to `max` chars with an ellipsis; strips control chars (a tab
/// becomes a space so aligned output keeps its shape).
fn trunc(s: &str, max: usize) -> String {
    let clean: String = s
        .chars()
        .filter_map(|c| match c {
            '\t' => Some(' '),
            c if c.is_control() => None,
            c => Some(c),
        })
        .collect();
    let mut out: String = clean.chars().take(max).collect();
    if clean.chars().count() > max {
        out.push('…');
    }
    out
}

/// Parse tool-call args; None on malformed JSON (callers fall back).
fn parse_args(args: &str) -> Option<serde_json::Value> {
    serde_json::from_str::<serde_json::Value>(args).ok()
}

/// Collapse all whitespace runs so any string fits on one line.
fn one_line(s: &str) -> String {
    s.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// Char budget left for the variable middle segment once the fixed prefix
/// width and suffix text are accounted for; keeps a small floor so narrow
/// panes still show something.
fn budget_for(width: u16, prefix: usize, suffix: &str) -> usize {
    (width as usize)
        .saturating_sub(prefix)
        .saturating_sub(suffix.chars().count())
        .max(8)
}

/// The ✓/✗ mark and its style for Done headers.
fn mark_ok(ok: bool, theme: &Theme) -> (&'static str, Style) {
    if ok {
        ("✓", style::tool_ok(theme))
    } else {
        ("✗", style::tool_err(theme))
    }
}

/// The last `n` lines of a process output blob, in original order.
fn tail_lines(tail: &str, n: usize) -> Vec<String> {
    if tail.is_empty() {
        return Vec::new();
    }
    let mut lines = Vec::new();
    let mut current = String::new();
    for ch in tail.chars() {
        match ch {
            '\r' => current.clear(),
            '\n' => {
                lines.push(std::mem::take(&mut current));
            }
            _ => current.push(ch),
        }
    }
    if !current.is_empty() {
        lines.push(current);
    }
    lines.into_iter().rev().take(n).collect::<Vec<_>>().into_iter().rev().collect()
}

fn append_ansi_tail(out: &mut Vec<Line<'static>>, tail: Option<&str>, width: u16, theme: &Theme) {
    let Some(tail) = tail else { return };
    let content_width = width.saturating_sub(4);
    for raw in tail_lines(tail, BASH_TAIL_MAX) {
        let mut line = clip_line_width(ansi_line(&raw), content_width as usize);
        let mut prefix = vec![Span::styled("  │ ", style::dim(theme))];
        prefix.append(&mut line.spans);
        out.push(Line::from(prefix));
    }
}

fn clip_line_width(line: Line<'static>, max_width: usize) -> Line<'static> {
    if line.width() <= max_width {
        return line;
    }
    if max_width == 0 {
        return Line::default();
    }
    let mut spans = Vec::new();
    let mut current = String::new();
    let mut current_style = Style::default();
    let mut used = 0usize;
    let mut truncated = false;
    let flush = |spans: &mut Vec<Span<'static>>, current: &mut String, style: Style| {
        if !current.is_empty() {
            spans.push(Span::styled(std::mem::take(current), style));
        }
    };
    'outer: for span in line.spans {
        if current_style != span.style {
            flush(&mut spans, &mut current, current_style);
            current_style = span.style;
        }
        for ch in span.content.chars() {
            let width = ch.width().unwrap_or(1);
            if used + width > max_width.saturating_sub(1) {
                truncated = true;
                break 'outer;
            }
            current.push(ch);
            used += width;
        }
    }
    flush(&mut spans, &mut current, current_style);
    if truncated {
        spans.push(Span::styled("…", current_style));
    }
    Line::from(spans)
}

fn ansi_line(input: &str) -> Line<'static> {
    use ratatui::style::{Color, Modifier};
    let mut spans = Vec::new();
    let mut style = Style::default();
    let mut text = String::new();
    let mut chars = input.chars().peekable();
    let flush = |spans: &mut Vec<Span<'static>>, text: &mut String, style: Style| {
        if !text.is_empty() {
            spans.push(Span::styled(std::mem::take(text), style));
        }
    };
    while let Some(ch) = chars.next() {
        if ch == '\x1b' && chars.peek() == Some(&'[') {
            chars.next();
            let mut code = String::new();
            for next in chars.by_ref() {
                if next == 'm' {
                    break;
                }
                code.push(next);
            }
            flush(&mut spans, &mut text, style);
            for value in code.split(';').filter_map(|v| v.parse::<u16>().ok()) {
                match value {
                    0 => style = Style::default(),
                    1 => style = style.add_modifier(Modifier::BOLD),
                    22 => style = style.remove_modifier(Modifier::BOLD),
                    30..=37 => style = style.fg(Color::Indexed((value - 30) as u8)),
                    90..=97 => style = style.fg(Color::Indexed((value - 90 + 8) as u8)),
                    39 => style.fg = None,
                    _ => {}
                }
            }
        } else {
            text.push(if ch == '\t' { ' ' } else { ch });
        }
    }
    flush(&mut spans, &mut text, style);
    Line::from(spans)
}

/// Assemble the cmdline a bash call spawns: `command` followed by the
/// strings in its `args` array. Mirrors the tail matcher in view.rs.
fn bash_cmdline(args: &str) -> Option<String> {
    bash_cmdline_from(&parse_args(args)?)
}

fn bash_cmdline_from(v: &serde_json::Value) -> Option<String> {
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

/// One-line human description of a tool call's JSON args: prefer the
/// interesting field (command, pattern, path, prompt...), else truncate the
/// raw JSON.
fn describe_args(args: &str, max: usize) -> String {
    if let Some(v) = parse_args(args) {
        if let Some(line) = bash_cmdline_from(&v) {
            return trunc(&line, max);
        }
        if let Some(pattern) = v.get("pattern").and_then(|p| p.as_str()) {
            return trunc(&format!("pattern={pattern}"), max);
        }
        if let Some(path) = v.get("path").and_then(|p| p.as_str()) {
            return trunc(path, max);
        }
        if let Some(prompt) = v.get("prompt").and_then(|p| p.as_str()) {
            return trunc(&one_line(prompt), max);
        }
    }
    trunc(args, max)
}

fn partial_string_field(args: &str, field: &str) -> Option<String> {
    if let Some(value) = parse_args(args).and_then(|v| v.get(field).cloned()) {
        if let Some(value) = value.as_str() {
            return Some(value.to_string());
        }
        if value.is_number() || value.is_boolean() {
            return Some(value.to_string());
        }
    }
    let marker = format!("\"{field}\"");
    let start = args.find(&marker)?;
    let rest = &args[start + marker.len()..];
    let colon = rest.find(':')?;
    let raw = rest[colon + 1..].trim_start();
    let raw = raw.strip_prefix('"')?;
    let mut value = raw.to_string();
    if let Some(end) = value.rfind('"') {
        value.truncate(end);
    }
    Some(
        value
            .replace("\\n", "\n")
            .replace("\\r", "\r")
            .replace("\\t", "\t")
            .replace("\\\"", "\"")
            .replace("\\\\", "\\"),
    )
}

fn describe_args_live(name: &str, args: &str, max: usize) -> String {
    let field = |key: &str| partial_string_field(args, key);
    let mut fields = Vec::new();
    match name {
        "read" | "list" => {
            if let Some(path) = field("path") {
                fields.push(format!("path={path}"));
            }
        }
        "grep" => {
            for key in ["pattern", "path", "glob", "ignore_case", "context", "limit"] {
                if let Some(value) = field(key) {
                    fields.push(format!("{key}={value}"));
                }
            }
        }
        "glob" => {
            for key in ["pattern", "path", "include_ignored", "limit"] {
                if let Some(value) = field(key) {
                    fields.push(format!("{key}={value}"));
                }
            }
        }
        "edit" => {
            if let Some(patch) = field("patch") {
                return trunc(&edit_file_summary(&patch), max);
            }
        }
        "bash" => {
            if let Some(value) = parse_args(args).and_then(|v| bash_cmdline_from(&v)) {
                return trunc(&value, max);
            }
            if let Some(command) = field("command") {
                return trunc(&format!("command={command}"), max);
            }
        }
        "delegate" => {
            if let Some(prompt) = field("prompt") {
                return trunc(&one_line(&prompt), max);
            }
        }
        _ => {}
    }
    if !fields.is_empty() {
        return trunc(&fields.join("  "), max);
    }
    describe_args(args, max)
}

fn tool_icon(state: &ToolState) -> &'static str {
    match state {
        ToolState::Preparing(_) => "◌ ",
        ToolState::Running(_) => "⠹ ",
        ToolState::Done { .. } | ToolState::Interrupted => "",
    }
}

fn tool_icon_style(state: &ToolState, theme: &Theme) -> Style {
    match state {
        ToolState::Preparing(_) => style::thinking(theme),
        ToolState::Running(_) => style::spinner(theme),
        ToolState::Done { .. } | ToolState::Interrupted => Style::default(),
    }
}

fn edit_diff_lines(patch: &str, _state: &ToolState, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    let mut out = Vec::new();
    let mut path = String::new();
    let mut line_no = 1usize;
    for raw in patch.lines() {
        let trimmed = raw.trim();
        if let Some(next) = trimmed
            .strip_prefix("*** Add File:")
            .or_else(|| trimmed.strip_prefix("*** Update File:"))
            .or_else(|| trimmed.strip_prefix("*** Delete File:"))
        {
            path = next.trim().to_string();
            let (added, removed) = file_change_counts(patch, &path);
            let marker = if trimmed.starts_with("*** Add") {
                "+"
            } else if trimmed.starts_with("*** Delete") {
                "-"
            } else {
                "~"
            };
            out.push(Line::from(vec![
                Span::styled(format!("{marker} edit "), style::tool(theme)),
                Span::styled(path.clone(), style::assistant(theme)),
                Span::styled(format!("  +{added} -{removed}"), style::dim(theme)),
            ]));
        } else if trimmed.starts_with("@@") {
            line_no = hunk_line_number(trimmed).unwrap_or(1);
            out.push(Line::styled(trimmed.to_string(), style::bar(theme)));
        } else if !path.is_empty()
            && (raw.starts_with('+') || raw.starts_with('-') || raw.starts_with(' '))
        {
            let kind = raw.as_bytes()[0] as char;
            let content = &raw[1..];
            let background = match kind {
                '+' => ratatui::style::Color::Rgb(25, 76, 38),
                '-' => ratatui::style::Color::Rgb(92, 35, 38),
                _ => ratatui::style::Color::Reset,
            };
            let display_no = line_no;
            if kind != '-' {
                line_no += 1;
            }
            let background_style = Style::default().bg(background);
            let mut spans = vec![Span::styled(
                format!("{display_no:>4} {kind} "),
                Style::default()
                    .fg(match kind {
                        '+' => ratatui::style::Color::Green,
                        '-' => ratatui::style::Color::Red,
                        _ => ratatui::style::Color::DarkGray,
                    })
                    .bg(background),
            )];
            spans.extend(highlight_diff_content(&path, content, background));
            let mut line = Line::from(spans);
            let remaining = width.saturating_sub(line.width() as u16);
            if remaining > 0 {
                line.spans.push(Span::styled(
                    " ".repeat(remaining as usize),
                    background_style,
                ));
            }
            out.push(line);
        }
    }
    if out.is_empty() && !patch.is_empty() {
        out.push(Line::styled(
            "◌ edit  preparing patch…".to_string(),
            style::dim(theme),
        ));
    }
    out
}

fn hunk_line_number(header: &str) -> Option<usize> {
    header
        .split_whitespace()
        .find_map(|token| token.trim_start_matches('+').parse::<usize>().ok())
}

fn file_change_counts(patch: &str, path: &str) -> (usize, usize) {
    let mut active = false;
    let mut added = 0;
    let mut removed = 0;
    for raw in patch.lines() {
        let trimmed = raw.trim();
        if trimmed.starts_with("*** Add File:")
            || trimmed.starts_with("*** Update File:")
            || trimmed.starts_with("*** Delete File:")
        {
            active = trimmed.ends_with(path);
            continue;
        }
        if active {
            if raw.starts_with('+') && !raw.starts_with("+++") {
                added += 1;
            } else if raw.starts_with('-') && !raw.starts_with("---") {
                removed += 1;
            }
        }
    }
    (added, removed)
}

fn highlight_diff_content(
    path: &str,
    content: &str,
    background: ratatui::style::Color,
) -> Vec<Span<'static>> {
    static SYNTAX: OnceLock<SyntaxSet> = OnceLock::new();
    static THEME: OnceLock<SyntectTheme> = OnceLock::new();
    let syntax = SYNTAX.get_or_init(SyntaxSet::load_defaults_newlines);
    let theme = THEME.get_or_init(|| {
        ThemeSet::load_defaults()
            .themes
            .get("base16-ocean.dark")
            .cloned()
            .unwrap_or_default()
    });
    let syntax_ref: &SyntaxReference = syntax
        .find_syntax_for_file(path)
        .ok()
        .flatten()
        .unwrap_or_else(|| syntax.find_syntax_plain_text());
    let mut highlighter = HighlightLines::new(syntax_ref, theme);
    let highlighted = highlighter
        .highlight_line(content, syntax)
        .unwrap_or_else(|_| vec![(syntect::highlighting::Style::default(), content)]);
    highlighted
        .into_iter()
        .map(|(foreground, text)| {
            let SynColor { r, g, b, .. } = foreground.foreground;
            Span::styled(
                text.to_string(),
                Style::default()
                    .fg(ratatui::style::Color::Rgb(r, g, b))
                    .bg(background),
            )
        })
        .collect()
}

/// Compact file summary from a patch's `*** Update File:` / `*** Add File:`
/// markers, e.g. `~ src/a.rs, + src/b.rs`; "patch" when no markers match.
fn edit_file_summary(patch: &str) -> String {
    let mut parts: Vec<String> = Vec::new();
    for line in patch.lines() {
        if let Some(path) = line.strip_prefix("*** Update File:") {
            parts.push(format!("~ {}", path.trim()));
        } else if let Some(path) = line.strip_prefix("*** Add File:") {
            parts.push(format!("+ {}", path.trim()));
        }
    }
    if parts.is_empty() {
        "patch".to_string()
    } else {
        parts.join(", ")
    }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

pub fn fmt_bytes(n: usize) -> String {
    if n >= 1024 * 1024 {
        format!("{:.1}MB", n as f64 / (1024.0 * 1024.0))
    } else if n >= 1024 {
        format!("{:.1}KB", n as f64 / 1024.0)
    } else {
        format!("{n}B")
    }
}

pub fn fmt_tokens(n: u32) -> String {
    if n >= 1_000_000 {
        format!("{:.1}M", n as f64 / 1_000_000.0)
    } else if n >= 1_000 {
        format!("{:.1}k", n as f64 / 1000.0)
    } else {
        n.to_string()
    }
}

/// Elapsed seconds since `started`, for status displays.
pub fn elapsed_secs(started: Instant) -> u64 {
    started.elapsed().as_secs()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tui::theme;

    fn test_theme() -> theme::Theme {
        theme::default_theme()
    }

    fn tool_lines(
        name: &str,
        args: &str,
        state: &ToolState,
        tail: Option<&str>,
        width: u16,
    ) -> Vec<Line<'static>> {
        super::tool_lines(name, args, state, tail, width, &test_theme())
    }

    /// Flatten a line to plain text, ignoring styles.
    fn plain(line: &Line<'_>) -> String {
        line.spans.iter().map(|s| s.content.as_ref()).collect()
    }

    /// Rendered char width of a line.
    fn plain_width(line: &Line<'_>) -> usize {
        line.spans.iter().map(|s| s.content.chars().count()).sum()
    }

    // trunc --------------------------------------------------------------

    #[test]
    fn trunc_keeps_short_strings() {
        assert_eq!(trunc("hello", 10), "hello");
        assert_eq!(trunc("hello", 5), "hello");
    }

    #[test]
    fn trunc_adds_ellipsis_on_overflow() {
        assert_eq!(trunc("abcdefgh", 5), "abcde…");
    }

    #[test]
    fn trunc_strips_control_chars_and_maps_tabs() {
        assert_eq!(trunc("a\u{7}b\nc", 10), "abc");
        assert_eq!(trunc("a\tb", 10), "a b");
    }

    // describe_args --------------------------------------------------------

    #[test]
    fn describe_args_prefers_command_plus_args() {
        let args = r#"{"command":"cargo","args":["test","-p","firmius"],"mode":"exec"}"#;
        assert_eq!(describe_args(args, 100), "cargo test -p firmius");
    }

    #[test]
    fn describe_args_pattern_before_path() {
        let args = r#"{"pattern":"TODO","path":"src"}"#;
        assert_eq!(describe_args(args, 100), "pattern=TODO");
    }

    #[test]
    fn describe_args_path() {
        assert_eq!(
            describe_args(r#"{"path":"src/main.rs"}"#, 100),
            "src/main.rs"
        );
    }

    #[test]
    fn describe_args_prompt_on_one_line() {
        assert_eq!(
            describe_args(r#"{"prompt":"do\nthe thing"}"#, 100),
            "do the thing"
        );
    }

    #[test]
    fn describe_args_malformed_falls_back_to_raw() {
        assert_eq!(describe_args("not json at all", 100), "not json at all");
    }

    #[test]
    fn describe_args_truncates() {
        assert_eq!(
            describe_args(r#"{"path":"abcdefghijklmnop"}"#, 8),
            "abcdefgh…"
        );
    }

    // bash cmdline assembly --------------------------------------------------

    #[test]
    fn bash_cmdline_joins_command_and_args() {
        let args = r#"{"command":"bash","args":["-c","echo hi && pwd"]}"#;
        assert_eq!(
            bash_cmdline(args).as_deref(),
            Some("bash -c echo hi && pwd")
        );
    }

    #[test]
    fn bash_cmdline_command_only() {
        assert_eq!(bash_cmdline(r#"{"command":"ls"}"#).as_deref(), Some("ls"));
    }

    #[test]
    fn bash_cmdline_skips_non_string_args() {
        let args = r#"{"command":"echo","args":["hi",42,null]}"#;
        assert_eq!(bash_cmdline(args).as_deref(), Some("echo hi"));
    }

    #[test]
    fn bash_cmdline_requires_command_and_valid_json() {
        assert_eq!(bash_cmdline(r#"{"args":["x"]}"#), None);
        assert_eq!(bash_cmdline("{broken"), None);
    }

    #[test]
    fn edit_file_summary_from_markers() {
        let patch = "*** Begin Patch\n*** Update File: src/a.rs\n+x\n*** Add File: src/b.rs\n+y\n*** End Patch";
        assert_eq!(edit_file_summary(patch), "~ src/a.rs, + src/b.rs");
    }

    #[test]
    fn edit_file_summary_fallback_when_no_markers() {
        assert_eq!(edit_file_summary("+x\n-y"), "patch");
    }

    // misc helpers -----------------------------------------------------------

    #[test]
    fn tail_lines_keeps_last_n_in_order() {
        assert_eq!(tail_lines("a\nb\nc\nd", 2), vec!["c", "d"]);
        assert_eq!(tail_lines("solo", 3), vec!["solo"]);
        assert_eq!(tail_lines("", 3), Vec::<String>::new());
    }

    #[test]
    fn tail_lines_applies_carriage_return_rewrites() {
        assert_eq!(tail_lines("Downloading 1%\rDownloading 50%\rDownloading 100%\nDone", 2), vec!["Downloading 100%", "Done"]);
    }

    #[test]
    fn one_line_collapses_whitespace() {
        assert_eq!(one_line("fix\nthe   bug\n"), "fix the bug");
    }

    // tool_lines shapes --------------------------------------------------------

    #[test]
    fn bash_running_shows_cmdline_elapsed_and_tail() {
        let args = r#"{"command":"cargo","args":["test"]}"#;
        let lines = tool_lines(
            "bash",
            args,
            &ToolState::Running(Instant::now()),
            Some("l1\nl2\nl3\nl4"),
            80,
        );
        assert_eq!(lines.len(), 4);
        let head = plain(&lines[0]);
        assert!(head.contains("cargo test"), "{head}");
        assert!(head.contains(" · "), "{head}");
        assert_eq!(plain(&lines[1]), "  │ l2");
        assert_eq!(plain(&lines[2]), "  │ l3");
        assert_eq!(plain(&lines[3]), "  │ l4");
    }

    #[test]
    fn bash_tail_clips_long_output_to_the_available_width() {
        let args = r#"{"command":"cargo","args":["test"]}"#;
        let lines = tool_lines(
            "bash",
            args,
            &ToolState::Running(Instant::now()),
            Some("12345678901234567890"),
            12,
        );
        assert_eq!(plain(&lines[1]), "  │ 1234567…");
        assert!(lines[1].width() <= 12);
    }

    #[test]
    fn bash_done_and_interrupted_shapes() {
        let args = r#"{"command":"make","args":["build"]}"#;
        let done = tool_lines(
            "bash",
            args,
            &ToolState::Done {
                ok: true,
                bytes: 2048,
            },
            None,
            80,
        );
        assert_eq!(plain(&done[0]), "✓ make build · 2.0KB");
        let fail = tool_lines(
            "bash",
            args,
            &ToolState::Done {
                ok: false,
                bytes: 3,
            },
            None,
            80,
        );
        assert_eq!(plain(&fail[0]), "✗ make build · 3B");
        let cut = tool_lines("bash", args, &ToolState::Interrupted, None, 80);
        assert_eq!(plain(&cut[0]), "⊘ make build");
    }

    #[test]
    fn bash_output_only_renders_for_process_modes() {
        let output = Some("\x1b[31mred\x1b[0m");
        let exec = tool_lines(
            "bash",
            r#"{"mode":"exec","command":"echo"}"#,
            &ToolState::Done { ok: true, bytes: 3 },
            output,
            80,
        );
        assert_eq!(plain(&exec[1]), "  │ red");
        assert!(exec[1].spans.iter().any(|span| span.style.fg.is_some()));

        let poll = tool_lines(
            "bash",
            r#"{"mode":"poll","proc_id":"1"}"#,
            &ToolState::Done { ok: true, bytes: 3 },
            output,
            80,
        );
        assert_eq!(poll.len(), 1);
    }

    #[test]
    fn delegate_running_excerpts_the_prompt() {
        let args = r#"{"prompt":"fix\nthe   flaky test"}"#;
        let lines = tool_lines(
            "delegate",
            args,
            &ToolState::Running(Instant::now()),
            None,
            80,
        );
        assert_eq!(lines.len(), 1);
        assert!(plain(&lines[0]).starts_with("⠹ delegate fix the flaky test"));
    }

    #[test]
    fn edit_diff_lines_styles_and_header() {
        let patch = "*** Begin Patch\n*** Update File: src/a.rs\n@@ fn f() {\n ctx\n-old\n+new\n*** End Patch";
        let args = serde_json::json!({ "patch": patch }).to_string();
        let lines = tool_lines(
            "edit",
            &args,
            &ToolState::Done {
                ok: true,
                bytes: 42,
            },
            None,
            80,
        );
        assert_eq!(lines.len(), 6); // tool header, file header, hunk, and 3 diff lines
        let head = plain(&lines[0]);
        assert!(head.starts_with("✓ edit"), "{head}");
        assert!(head.contains("~ src/a.rs"), "{head}");
        assert!(plain(&lines[4]).contains("- old"));
        assert!(plain(&lines[5]).contains("+ new"));
        assert!(lines[4].spans.iter().any(|span| span.style.bg.is_some()));
        assert!(lines[5].spans.iter().any(|span| span.style.bg.is_some()));
    }

    #[test]
    fn edit_diff_shows_all_hunks_without_elision() {
        let patch = format!(
            "*** Begin Patch\n*** Update File: src/a.rs\n{}\n*** End Patch",
            (0..20)
                .map(|i| format!("+l{i}"))
                .collect::<Vec<_>>()
                .join("\n")
        );
        let args = serde_json::json!({ "patch": patch }).to_string();
        let lines = tool_lines(
            "edit",
            &args,
            &ToolState::Done { ok: true, bytes: 5 },
            None,
            80,
        );
        assert!(lines.len() > 8);
        assert!(lines.iter().any(|line| plain(line).contains("+ l19")));
        assert!(!lines.iter().any(|line| plain(line).contains("more")));
    }

    #[test]
    fn quick_tools_render_one_line() {
        let args = r#"{"pattern":"foo","path":"src"}"#;
        let lines = tool_lines(
            "grep",
            args,
            &ToolState::Done { ok: true, bytes: 7 },
            None,
            80,
        );
        assert_eq!(lines.len(), 1);
        assert_eq!(plain(&lines[0]), "✓ grep pattern=foo  path=src · 7B");
        let run = tool_lines(
            "read",
            r#"{"path":"src/main.rs"}"#,
            &ToolState::Running(Instant::now()),
            None,
            80,
        );
        assert_eq!(plain(&run[0]), "⠹ read path=src/main.rs");
    }

    #[test]
    fn preparing_tools_show_neutral_icon_and_accumulated_fields() {
        let lines = tool_lines(
            "grep",
            r#"{"pattern":"ToolCall","path":"crates/"}"#,
            &ToolState::Preparing(Instant::now()),
            None,
            100,
        );
        let text = plain(&lines[0]);
        assert!(text.starts_with("◌ grep"), "{text}");
        assert!(text.contains("pattern=ToolCall"), "{text}");
        assert!(text.contains("path=crates/"), "{text}");
        assert!(!text.contains('✓'));
        assert!(!text.contains('✗'));
    }

    #[test]
    fn edit_preparation_renders_partial_patch_without_error_icon() {
        let patch = "*** Begin Patch\\n*** Update File: src/lib.rs\\n@@ fn main() {\\n-ol";
        let args = format!(r#"{{"patch":"{patch}"}}"#);
        let lines = tool_lines(
            "edit",
            &args,
            &ToolState::Preparing(Instant::now()),
            None,
            100,
        );
        assert!(plain(&lines[0]).starts_with("◌ edit"));
        assert!(!lines.iter().any(|line| plain(line).contains('✗')));
    }

    #[test]
    fn malformed_args_fall_back_to_generic() {
        let lines = tool_lines("bash", "{broken", &ToolState::Interrupted, None, 80);
        assert_eq!(lines.len(), 1);
        let text = plain(&lines[0]);
        assert!(text.starts_with("⊘ bash"), "{text}");
        assert!(text.contains("{broken"), "{text}");
    }

    #[test]
    fn lines_stay_within_width() {
        let args = r#"{"command":"some-very-long-command","args":["--flag","another-long-argument-here"]}"#;
        for w in [30u16, 80] {
            let lines = tool_lines(
                "bash",
                args,
                &ToolState::Done {
                    ok: true,
                    bytes: 999_999,
                },
                None,
                w,
            );
            for line in &lines {
                // +1 tolerates the ellipsis char trunc() appends on overflow.
                assert!(
                    plain_width(line) <= w as usize + 1,
                    "{} > {}",
                    plain_width(line),
                    w
                );
            }
        }
    }

    #[test]
    fn progress_bar_uses_filled_and_empty_blocks() {
        assert_eq!(progress_bar(50, 100, 10), "▰▰▰▰▰▱▱▱▱▱");
        assert_eq!(progress_bar(200, 100, 4), "▰▰▰▰");
        assert_eq!(progress_bar(0, 0, 3), "▱▱▱");
    }

    #[test]
    fn context_usage_formats_max_in_kilobytes_or_megabytes() {
        assert_eq!(format_context_usage(12_345, 200_000), "12k/200k");
        assert_eq!(format_context_usage(12_345, 2_000_000), "12k/2M");
    }
}
