//! Per-tool presentation rendering.
//!
//! Contract (pinned): given a tool call's name, its JSON args, its state, an
//! optional live output tail (for bash processes the app loop polls), and
//! the pane width — produce styled lines. First pass renders a generic
//! shape; rich per-tool presentations (bash live tail, delegate window,
//! edit diff) replace the internals without changing this signature.

use ratatui::text::{Line, Span};

use super::model::ToolState;
use super::style;
use std::time::Instant;

/// Truncate to `max` chars with an ellipsis; strips control chars.
fn trunc(s: &str, max: usize) -> String {
    let clean: String = s.chars().filter(|c| !c.is_control()).collect();
    let mut out: String = clean.chars().take(max).collect();
    if clean.chars().count() > max {
        out.push('…');
    }
    out
}

/// One-line human description of a tool call's JSON args: prefer the
/// interesting field (command, pattern, path...), else truncate raw JSON.
fn describe_args(args: &str, max: usize) -> String {
    if let Ok(v) = serde_json::from_str::<serde_json::Value>(args) {
        if let Some(cmd) = v.get("command").and_then(|c| c.as_str()) {
            let mut line = cmd.to_string();
            if let Some(extra) = v.get("args").and_then(|a| a.as_array()) {
                for a in extra {
                    if let Some(s) = a.as_str() {
                        line.push(' ');
                        line.push_str(s);
                    }
                }
            }
            return trunc(&line, max);
        }
        if let Some(pattern) = v.get("pattern").and_then(|p| p.as_str()) {
            return trunc(&format!("pattern={pattern}"), max);
        }
        if let Some(path) = v.get("path").and_then(|p| p.as_str()) {
            return trunc(path, max);
        }
    }
    trunc(args, max)
}

pub fn tool_lines(
    name: &str,
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
) -> Vec<Line<'static>> {
    let w = (width as usize).saturating_sub(2).max(8);
    match state {
        ToolState::Running(started) => {
            let secs = started.elapsed().as_secs();
            let head = Line::from(vec![
                Span::styled("⠹ ", style::spinner()),
                Span::styled(name.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(describe_args(args, w), style::dim()),
                Span::styled(format!(" · {secs}s"), style::dim()),
            ]);
            let mut out = vec![head];
            if let Some(tail) = tail {
                let lines: Vec<&str> = tail.lines().rev().take(3).collect();
                for line in lines.into_iter().rev() {
                    out.push(Line::styled(format!("  │ {line}"), style::dim()));
                }
            }
            out
        }
        ToolState::Done { ok, bytes } => {
            let (mark, st) = if *ok {
                ("✓", style::tool_ok())
            } else {
                ("✗", style::tool_err())
            };
            vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(name.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(describe_args(args, w), style::dim()),
                Span::styled(format!(" · {}", fmt_bytes(*bytes)), style::dim()),
            ])]
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err()),
            Span::styled(name.to_string(), style::tool()),
            Span::raw(" "),
            Span::styled(describe_args(args, w), style::dim()),
        ])],
    }
}

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
        format!("{:.1}k", n as f64 / 1_000.0)
    } else {
        n.to_string()
    }
}

/// Elapsed seconds since `started`, for status displays.
pub fn elapsed_secs(started: Instant) -> u64 {
    started.elapsed().as_secs()
}