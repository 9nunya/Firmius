//! Per-tool presentation rendering.
//!
//! Contract (pinned): given a tool call's name, its JSON args, its state, an
//! optional live output tail (for bash processes the app loop polls), and
//! the pane width — produce styled lines. Rich per-tool presentations live
//! here (bash live tail, delegate prompt excerpt, edit mini diff); unknown
//! tools and malformed args fall back to the generic shape.

use firmius_core::partial_json::PartialJson;
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use std::sync::OnceLock;
use syntect::easy::HighlightLines;
use syntect::highlighting::{Color as SynColor, Theme as SyntectTheme, ThemeSet};
use syntect::parsing::{SyntaxReference, SyntaxSet};
use unicode_width::UnicodeWidthChar;
use unicode_width::UnicodeWidthStr;

use super::model::{CompactionItem, CompactionPhase, SearchState, ToolState};
use super::runtime_state::{
    PermissionGateStatus, PermissionProvenance, ToolExecutionLifecycle, ToolExecutionState,
};
use super::style;
use super::theme::Theme;
use firmius_core::WebSearchAction;

/// The small set of failure classes that can be recovered from a tool result.
///
/// Tool results cross the agent boundary as text (the protocol deliberately
/// keeps the result payload provider-compatible), so the TUI cannot retain
/// the original [`firmius_core::ToolError`] value.  Keeping this classifier in
/// the presenter still gives users a stable, useful diagnostic without
/// changing the persisted wire format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ToolErrorKind {
    InvalidArguments,
    PermissionDenied,
    Cancelled,
    ProcessId,
    UnknownId,
    Failed,
}

fn array_progress(parsed: &PartialJson, key: &str) -> Option<String> {
    match parsed.get(key) {
        firmius_core::partial_json::Field::Complete(serde_json::Value::Array(values)) => {
            Some(format!("{} {key}", values.len()))
        }
        firmius_core::partial_json::Field::Partial(_) => Some(format!("reading {key}…")),
        firmius_core::partial_json::Field::Missing => None,
        _ => Some(format!("reading {key}…")),
    }
}

/// Render the compact control-plane state associated with a tool call.
///
/// Ordinary tool cards do not acquire an inspector or disclosure affordance
/// from this helper: callers only pass a row when the runtime has typed
/// execution state for that call.  The first row carries queue/lifecycle and
/// timing context; permission and terminal classifier details are kept on a
/// single bounded continuation row.
pub fn tool_execution_lines(
    execution: &ToolExecutionState,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let lifecycle = match execution.lifecycle {
        ToolExecutionLifecycle::Preparing => "preparing",
        ToolExecutionLifecycle::Queued => "queued",
        ToolExecutionLifecycle::WaitingPermission => "waiting permission",
        ToolExecutionLifecycle::Running => "running",
        ToolExecutionLifecycle::Settled => "settled",
        ToolExecutionLifecycle::Failed => "failed",
    };
    let mut context = format!("  · {lifecycle}");
    if execution.queue_total > 0 {
        context.push_str(&format!(
            " · {}/{}",
            execution.queue_position, execution.queue_total
        ));
    }
    if let Some(predecessor) = execution.predecessor.as_deref() {
        context.push_str(&format!(" · after {}", shorten_inline(predecessor, 18)));
    }
    // Active work should not present a repaint-dependent stopwatch. Once a
    // typed execution reaches a terminal lifecycle, retain its recorded
    // elapsed diagnostic (when both stream boundaries are available).
    if matches!(
        execution.lifecycle,
        ToolExecutionLifecycle::Settled | ToolExecutionLifecycle::Failed
    ) {
        if let Some(start) = execution
            .timestamps
            .started_at
            .or(execution.timestamps.queued_at)
            .or(execution.timestamps.prepared_at)
        {
            if let Some(end) = execution
                .timestamps
                .settled_at
                .or(execution.timestamps.failed_at)
            {
                let elapsed = end.signed_duration_since(start).num_seconds().max(0);
                context.push_str(&format!(" · elapsed {elapsed}s"));
            }
        }
    }
    if let Some(tail) = execution
        .output_tail
        .as_deref()
        .filter(|tail| !tail.is_empty())
    {
        context.push_str(&format!(
            " · output {}",
            shorten_inline(&one_line(tail), 28)
        ));
    }
    if let Some(delegate) = execution.delegate.as_ref() {
        context.push_str(&format!(" · {}", delegate.summary()));
    }
    let mut lines = vec![Line::styled(
        trunc(&context, usize::from(width).max(1)),
        style::dim(theme),
    )];

    let permission = execution.permission.as_ref().and_then(|gate| {
        let status = match gate.status {
            PermissionGateStatus::Requested => "requested",
            PermissionGateStatus::Waiting => "waiting",
            PermissionGateStatus::UserAllowed => "allowed",
            PermissionGateStatus::UserDenied => "denied",
            PermissionGateStatus::AutoAllowed => "allowed",
            PermissionGateStatus::AutoDenied => "denied",
        };
        let provenance = gate
            .decision
            .as_ref()
            .map(|decision| match decision.provenance {
                PermissionProvenance::User => "user",
                PermissionProvenance::Auto => "auto",
                PermissionProvenance::Policy => "policy",
                PermissionProvenance::Inherited => "inherited",
            });
        let reason = gate
            .decision
            .as_ref()
            .and_then(|decision| decision.reason.as_deref())
            .or(execution.classifier_reason.as_deref());
        let mut value = format!("  permission: {status}");
        if let Some(provenance) = provenance {
            value.push_str(&format!(" ({provenance})"));
        }
        if let Some(reason) = reason.filter(|reason| !reason.trim().is_empty()) {
            value.push_str(&format!(" · {}", one_line(reason)));
        }
        Some(value)
    });
    let detail = permission.or_else(|| {
        execution
            .classifier_reason
            .as_deref()
            .filter(|reason| !reason.trim().is_empty())
            .map(|reason| format!("  result: {}", one_line(reason)))
    });
    if let Some(detail) = detail {
        lines.push(Line::styled(
            trunc(&detail, usize::from(width).max(1)),
            style::dim(theme),
        ));
    }
    lines
}

impl ToolErrorKind {
    pub const fn label(self) -> &'static str {
        match self {
            Self::InvalidArguments => "invalid arguments",
            Self::PermissionDenied => "permission denied",
            Self::Cancelled => "cancelled",
            Self::ProcessId => "process id",
            Self::UnknownId => "unknown id",
            Self::Failed => "failed",
        }
    }
}

/// Classify the display text emitted by a tool.  The matching intentionally
/// accepts both the typed error's Display form (for example, `tool failed:
/// no such process`) and the underlying host/tool text, since persisted
/// sessions may contain either form.
pub fn classify_tool_error(error: &str) -> ToolErrorKind {
    let lower = error.to_ascii_lowercase();
    if lower.contains("permission denied") {
        ToolErrorKind::PermissionDenied
    } else if lower.contains("cancelled") || lower.contains("canceled") {
        ToolErrorKind::Cancelled
    } else if lower.contains("invalid proc_id") || lower.contains("invalid process_id") {
        ToolErrorKind::ProcessId
    } else if lower.contains("invalid arguments") || lower.contains("requires '") {
        ToolErrorKind::InvalidArguments
    } else if lower.contains("no such process")
        || lower.contains("process not found")
        || lower.contains("proc_id")
    {
        ToolErrorKind::ProcessId
    } else if lower.contains("unknown run_id")
        || lower.contains("unknown node")
        || lower.contains("unknown graph")
        || lower.contains("graph not found")
        || lower.contains("unknown id")
    {
        ToolErrorKind::UnknownId
    } else {
        ToolErrorKind::Failed
    }
}

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

/// Render a window of actual patch rows, returning the next row offset when
/// more rows remain.  Patch headers and hunk markers deliberately do not count
/// towards `limit`: streamed edit arguments can contain a large amount of
/// metadata before the first changed line, and that metadata must never hide
/// the content the user is waiting to see.  The offset makes this helper usable
/// by a caller with a taller/scrollable surface without reparsing its own
/// patch format.
fn edit_diff_lines_window(
    patch: &str,
    _width: u16,
    _theme: &Theme,
    start: usize,
    limit: usize,
) -> (Vec<Line<'static>>, Option<usize>) {
    if limit == 0 || patch.is_empty() {
        return (Vec::new(), None);
    }

    let mut out = Vec::new();
    let mut path = String::new();
    let mut line_no = 1usize;
    let mut in_hunk = false;
    let mut diff_index = 0usize;
    for raw in patch.lines() {
        let trimmed = raw.trim();
        if let Some(next) = trimmed
            .strip_prefix("*** Update File:")
            .or_else(|| trimmed.strip_prefix("*** Add File:"))
            .or_else(|| trimmed.strip_prefix("*** Delete File:"))
        {
            path = next.trim().to_string();
            line_no = 1;
            // Add/Delete patches have no @@ marker in the apply-patch format;
            // updates require one before context/change rows are content.
            in_hunk =
                trimmed.starts_with("*** Add File:") || trimmed.starts_with("*** Delete File:");
        } else if trimmed.starts_with("@@") {
            line_no = hunk_line_number(trimmed).unwrap_or(1);
            in_hunk = !path.is_empty();
        } else if (raw.starts_with('+') || raw.starts_with('-') || raw.starts_with(' '))
            && !raw.starts_with("+++")
            && !raw.starts_with("---")
            && !path.is_empty()
            // Some providers omit `@@` while streaming additions/removals.
            // Those prefixes are unambiguous; context rows require a hunk.
            && (in_hunk || raw.starts_with('+') || raw.starts_with('-'))
        {
            let kind = raw.as_bytes()[0] as char;
            let content = &raw[1..];
            let row_index = diff_index;
            diff_index += 1;
            if row_index < start {
                if kind != '-' {
                    line_no += 1;
                }
                continue;
            }
            if out.len() >= limit {
                return (out, Some(row_index));
            }
            let bg = diff_background(kind);
            let mut spans = vec![Span::styled(
                format!("{line_no:>4} {kind} "),
                Style::default().bg(bg),
            )];
            // Streaming arguments are reparsed whenever a provider delta
            // arrives. Keep this bounded preview plain: syntax highlighting
            // the growing patch on every frame rescans regex rules and causes
            // visible periodic TUI stalls. The finalized edit path still
            // highlights its content once.
            spans.push(Span::styled(content.to_string(), Style::default().bg(bg)));
            out.push(Line::from(spans));
            if kind != '-' {
                line_no += 1;
            }
        }
    }
    (out, None)
}

/// Compact used/limit text for a quota meter on the CTX bar.
pub fn format_quota_percent(used: u64, limit: u64) -> String {
    if limit == 0 {
        return "0%".into();
    }
    let percent = ((used as f64 / limit as f64) * 100.0).clamp(0.0, 999.0);
    if percent >= 10.0 || percent.fract() < 0.05 {
        format!("{:.0}%", percent)
    } else {
        format!("{percent:.1}%")
    }
}

/// Format cached-token counts for the lightning metric. Keep the unit in
/// uppercase so it is visually distinct from the regular token counters.
pub fn fmt_cached_tokens(n: u32) -> String {
    if n >= 1_000_000 {
        format!("{:.1}M", n as f64 / 1_000_000.0)
    } else if n >= 1_000 {
        format!("{:.1}K", n as f64 / 1000.0)
    } else {
        n.to_string()
    }
}

fn wrap_command(command: &str, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    let max = usize::from(width).max(1);
    let mut rows = Vec::new();
    let mut row = String::from("$ ");
    for word in command.split_whitespace() {
        let candidate = if row == "$ " {
            format!("{row}{word}")
        } else {
            format!("{row} {word}")
        };
        if candidate.chars().count() > max && row != "$ " {
            rows.push(Line::styled(row, style::tool(theme)));
            row = format!("  {word}");
        } else {
            row = candidate;
        }
    }
    if row != "$ " || rows.is_empty() {
        rows.push(Line::styled(row, style::tool(theme)));
    }
    rows
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
        "task" => task_lines_progressive(args, None, state, width, theme),
        "read" | "list" | "grep" | "glob" => quick_lines(name, args, state, tail, width, theme),
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

/// Hosted web-search line: a sibling of tool lines, never a ToolCall.
///
/// Preparing => searching, done => searched. Subject comes from the query,
/// URL, or pattern. Interrupted / Other still render without panicking.
pub fn search_lines(
    action: &WebSearchAction,
    state: &SearchState,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let verb = match state {
        SearchState::Preparing(_) => "searching",
        SearchState::Done | SearchState::Interrupted => "searched",
    };
    let subject = search_subject(action);
    let label = subject
        .map(|s| format!("{verb} \"{s}\""))
        .unwrap_or_else(|| verb.to_string());
    match state {
        SearchState::Preparing(started) => {
            let suffix = "";
            let out = vec![Line::from(vec![
                Span::styled(
                    tool_icon(&ToolState::Preparing(*started)),
                    tool_icon_style(&ToolState::Preparing(*started), theme),
                ),
                Span::styled(
                    trunc(&label, budget_for(width, 2, &suffix)),
                    style::assistant(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])];
            out
        }
        SearchState::Done => vec![Line::from(vec![
            Span::styled("✓ ", style::tool_ok(theme)),
            Span::styled(trunc(&label, budget_for(width, 2, "")), style::assistant(theme)),
        ])],
        SearchState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(trunc(&label, budget_for(width, 2, "")), style::assistant(theme)),
        ])],
    }
}

pub fn search_subject(action: &WebSearchAction) -> Option<String> {
    action.subject().map(one_line).filter(|s| !s.is_empty())
}

/// Live context-compaction card. The streamed summary is rendered in full so
/// it behaves like transcript text, not a collapsible/truncated preview.
pub fn compaction_lines(item: &CompactionItem, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    let (icon, icon_style, label, suffix) = match &item.phase {
        CompactionPhase::Scheduled => (
            "◌ ",
            style::dim(theme),
            "compaction queued".to_string(),
            format!(" · gen {}", item.generation),
        ),
        CompactionPhase::Running(started) => (
            tool_icon(&ToolState::Running(*started)),
            tool_icon_style(&ToolState::Running(*started), theme),
            "compacting context…".to_string(),
            format!(" · gen {}", item.generation),
        ),
        CompactionPhase::Finished => (
            "✓ ",
            style::tool_ok(theme),
            "context compacted".to_string(),
            format!(" · {} chars", item.summary.chars().count()),
        ),
        CompactionPhase::Discarded => (
            "⊘ ",
            style::dim(theme),
            "compaction superseded".to_string(),
            String::new(),
        ),
        CompactionPhase::Failed(error) => (
            "✗ ",
            style::tool_err(theme),
            "compaction failed".to_string(),
            format!(" · {}", one_line(error)),
        ),
    };
    let mut lines = vec![Line::from(vec![
        Span::styled(icon, icon_style),
        Span::styled(
            trunc(&label, budget_for(width, 2, &suffix)),
            style::dim(theme),
        ),
        Span::styled(
            trunc(&suffix, width.saturating_sub(2) as usize),
            style::dim(theme),
        ),
    ])];
    if !item.summary.is_empty() {
        // Keep every streamed character (including logical line breaks). The
        // transcript renderer wraps these lines to the available width after
        // this presenter returns, so no content needs a fixed-width preview.
        let mut summary_lines = item.summary.split('\n').peekable();
        if summary_lines.peek().is_some() {
            for (index, summary_line) in summary_lines.enumerate() {
                let prefix = if index == 0 { "  ╰─ " } else { "     " };
                lines.push(Line::from(vec![
                    Span::styled(prefix, style::dim(theme)),
                    Span::styled(summary_line.to_owned(), style::thinking(theme)),
                ]));
            }
        }
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
    bash_lines_progressive_window(args, state, tail, width, theme, related_intent, 3)
}

/// The same bash presentation with an explicit retained-output window.
/// `tail_lines` is the number of newest output rows the caller wants visible:
/// three for the collapsed default, substantially more when the event has
/// actually been expanded. Rendering an unchanged three-line tail for an
/// expanded event is a contract violation, so the window is caller-owned.
pub fn bash_lines_progressive_window(
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
    related_intent: Option<&str>,
    tail_lines: usize,
) -> Vec<Line<'static>> {
    let tail = if bash_mode_shows_output(args) {
        tail
    } else {
        None
    };
    let parsed = PartialJson::parse(args);
    let mode = parsed.str("mode").unwrap_or("exec");
    let label = bash_progress_label(&parsed, mode, related_intent)
        .or_else(|| bash_cmdline(args))
        .unwrap_or_else(|| describe_args_live("bash", args, width as usize));
    let mut lines = status_line(&label, state, None, "bash", width, theme);
    if matches!(mode, "exec" | "spawn")
        && let Some(command) = bash_cmdline(args)
    {
        lines.extend(wrap_command(&command, width, theme));
    }
    if tail.is_some_and(|value| !value.is_empty()) {
        append_ansi_tail_with_limit(&mut lines, tail, width, theme, tail_lines.max(1));
    }
    lines
}

pub fn delegate_lines_progressive(
    args: &str,
    state: &ToolState,
    width: u16,
    theme: &Theme,
    related_intent: Option<&str>,
) -> Vec<Line<'static>> {
    delegate_lines_progressive_window(args, state, width, theme, related_intent, 3, &[])
}

/// Delegate presentation with a caller-owned child-activity window. `children`
/// are pre-rendered child lines (bounded by the caller); the collapsed default
/// shows none beyond what the parent knows, an expanded delegate shows them.
pub fn delegate_lines_progressive_window(
    args: &str,
    state: &ToolState,
    width: u16,
    theme: &Theme,
    related_intent: Option<&str>,
    tail_lines: usize,
    children: &[Line<'static>],
) -> Vec<Line<'static>> {
    let parsed = PartialJson::parse(args);
    let mode = parsed
        .str("action")
        .or_else(|| parsed.str("mode"))
        .unwrap_or("run");
    let label = delegate_progress_label(&parsed, mode, state, related_intent)
        .unwrap_or_else(|| "delegating".to_string());
    let mut lines = status_line(&label, state, None, "delegate", width, theme);
    if tail_lines > 3 {
        let budget = tail_lines.saturating_sub(lines.len());
        for child in children.iter().take(budget) {
            let mut spans = vec![Span::styled("  │ ", style::dim(theme))];
            spans.extend(child.spans.iter().cloned());
            lines.push(Line::from(spans));
        }
    }
    lines
}

/// A semantic communication presentation. The tool name and raw JSON are
/// intentionally absent: this is an outbound message, not a generic call.
pub fn message_lines_progressive(
    args: &str,
    result: Option<&str>,
    state: &ToolState,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let parsed = PartialJson::parse(args);
    let target = match parsed.str("target").unwrap_or("parent") {
        "agent" => parsed.str("agent_id").unwrap_or("agent"),
        "label" => parsed.str("label").unwrap_or("label"),
        "siblings" => "siblings",
        "fleet" => "fleet",
        "task" => "task parent",
        _ => "parent",
    };
    let runtime = result.and_then(|value| {
        if value.contains("failed") {
            Some("delivery failed")
        } else if value.contains("queued") {
            Some("queued")
        } else if value.contains("delivered") {
            Some("delivered")
        } else {
            None
        }
    });
    let label = match state {
        ToolState::Preparing(_) => format!("composing message to {target}"),
        ToolState::Running(_) => format!("delivering to {target}"),
        ToolState::Done { ok: true, .. } => {
            format!("{target} · {}", runtime.unwrap_or("delivered"))
        }
        ToolState::Done { ok: false, .. } => format!("{target} · delivery failed"),
        ToolState::Interrupted => format!("{target} · interrupted"),
    };
    let mut lines = status_line(&label, state, None, "message", width, theme);
    if let Some(message) = parsed.str("message").filter(|value| !value.is_empty()) {
        let suffix = (!parsed.is_key_complete("message"))
            .then_some("…")
            .unwrap_or("");
        lines.push(Line::styled(
            format!(
                "  {}{suffix}",
                trunc(message, width.saturating_sub(3) as usize)
            ),
            style::dim(theme),
        ));
    }
    lines
}

/// Render the memory request as readable content instead of collapsing the
/// caller's prompt into the generic JSON argument preview. The result is the
/// memory curator's report, when one has arrived; it is deliberately retained
/// in full and the transcript viewport performs the width wrapping.
pub fn memory_lines_progressive(
    args: &str,
    result: Option<&str>,
    state: &ToolState,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let parsed = PartialJson::parse(args);
    let label = match state {
        ToolState::Preparing(_) => "preparing memory request",
        ToolState::Running(_) => "sending memory request",
        ToolState::Done { ok: true, .. } => "memory curator result",
        ToolState::Done { ok: false, .. } => "memory request failed",
        ToolState::Interrupted => "memory request interrupted",
    };
    let mut lines = status_line(label, state, None, "memory", width, theme);
    if let Some(prompt) = parsed.str("prompt").filter(|prompt| !prompt.is_empty()) {
        for (index, line) in prompt.lines().enumerate() {
            let prefix = if index == 0 {
                "  prompt: "
            } else {
                "          "
            };
            lines.push(Line::from(vec![
                Span::styled(prefix, style::dim(theme)),
                Span::styled(line.to_string(), style::assistant(theme)),
            ]));
        }
    }
    if let Some(result) = result.filter(|result| !result.trim().is_empty()) {
        let formatted = serde_json::from_str::<serde_json::Value>(result)
            .ok()
            .and_then(|value| serde_json::to_string_pretty(&value).ok())
            .unwrap_or_else(|| result.to_string());
        for line in formatted.lines() {
            lines.push(Line::styled(format!("  │ {line}"), style::dim(theme)));
        }
    }
    lines
}

fn complete_array_len(parsed: &PartialJson, key: &str) -> Option<usize> {
    match parsed.get(key) {
        firmius_core::partial_json::Field::Complete(serde_json::Value::Array(values)) => {
            Some(values.len())
        }
        _ => None,
    }
}

fn complete_number_text(parsed: &PartialJson, key: &str) -> Option<String> {
    match parsed.get(key) {
        firmius_core::partial_json::Field::Complete(serde_json::Value::Number(value)) => {
            Some(value.to_string())
        }
        _ => None,
    }
}

/// A semantic orchestration presentation. Durable graph progress belongs to
/// the live run panel; this row describes the command and its immediate
/// lifecycle without dumping task JSON into the transcript.
pub fn task_lines_progressive(
    args: &str,
    result: Option<&str>,
    state: &ToolState,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let parsed = PartialJson::parse(args);
    let mode = parsed
        .str("action")
        .or_else(|| parsed.str("mode"))
        .unwrap_or("task");
    let subject = parsed
        .str("title")
        .or_else(|| parsed.str("key"))
        .or_else(|| parsed.str("run_id"));
    let verb = match mode {
        "plan" => "planning workflow",
        "launch" => "launching workflow",
        "poll" => "checking run",
        "await" | "wait" => "awaiting run",
        "complete" => "completing work",
        "add" => "adding work",
        "start" => "starting work",
        "view" => "viewing work",
        "quality_digest" => "checking workflow quality",
        "create" | "init" => "creating workflow",
        "cancel" => "cancelling run",
        other => other,
    };
    let mut label = subject
        .filter(|value| !value.is_empty())
        .map(|subject| format!("{verb} · {}", shorten_inline(subject, 44)))
        .unwrap_or_else(|| verb.to_string());
    if let ToolState::Done { ok: true, .. } = state {
        label = match mode {
            "plan" => "workflow planned".into(),
            "launch" => "run launched".into(),
            "poll" => "run checked".into(),
            "await" | "wait" => {
                if result.is_some_and(|value| value.contains("Stalled")) {
                    "workflow stalled".into()
                } else {
                    "workflow settled".into()
                }
            }
            "complete" => "work completed".into(),
            "add" => "work added".into(),
            _ => label,
        };
    }
    let mut lines = status_line(&label, state, None, "workflow", width, theme);
    let detail = match mode {
        "plan" => {
            let mut parts = Vec::new();
            if let Some(nodes) = array_progress(&parsed, "nodes") {
                parts.push(nodes);
            }
            if let Some(edges) = array_progress(&parsed, "edges") {
                parts.push(edges.replace("edges", "dependencies"));
            }
            if parts.is_empty() {
                Some("reading workflow plan…".into())
            } else {
                Some(parts.join(" · "))
            }
        }
        "launch" => complete_number_text(&parsed, "max_concurrent")
            .as_deref()
            .map(|value| format!("concurrency {value}"))
            .or_else(|| result.and_then(run_identity)),
        "poll" | "await" | "wait" => result.and_then(run_counts),
        "complete" => {
            array_progress(&parsed, "keys").map(|keys| keys.replace("keys", "work items"))
        }
        "add" | "init" | "create" => array_progress(&parsed, "items")
            .or_else(|| array_progress(&parsed, "completion_criteria")),
        _ => None,
    };
    if let Some(detail) = detail {
        lines.push(Line::styled(
            format!("  {}", trunc(&detail, width.saturating_sub(2) as usize)),
            style::dim(theme),
        ));
    }
    lines
}

/// A workflow is an orchestration launch, not a generic JSON tool call. Keep
/// its card deliberately spare: one headline for the run and one line for the
/// swarm shape. The durable work panel carries the live node-by-node detail.
pub fn workflow_lines_progressive(
    args: &str,
    result: Option<&str>,
    state: &ToolState,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let parsed = PartialJson::parse(args);
    let action = parsed.str("action").unwrap_or("run");
    let title = parsed
        .str("title")
        .or_else(|| parsed.str("run_id"))
        .filter(|value| !value.is_empty());
    let verb = match action {
        "run" => "launching swarm",
        "status" => "checking swarm",
        "wait" => "awaiting swarm",
        "cancel" => "stopping swarm",
        other => other,
    };
    let mut label = title
        .map(|value| format!("{verb} · {}", shorten_inline(value, 44)))
        .unwrap_or_else(|| verb.to_string());
    if let ToolState::Done { ok: true, .. } = state {
        label = match action {
            "run" => "swarm launched".into(),
            "status" => "swarm checked".into(),
            "wait" => {
                if result.is_some_and(|value| value.contains("Stalled")) {
                    "swarm needs attention".into()
                } else {
                    "swarm settled".into()
                }
            }
            "cancel" => "swarm stopped".into(),
            _ => label,
        };
    }
    let mut lines = status_line(&label, state, None, "workflow", width, theme);
    let detail = match action {
        "run" => match complete_array_len(&parsed, "steps") {
            Some(steps) => {
                let concurrent =
                    complete_number_text(&parsed, "max_concurrent").unwrap_or_else(|| "4".into());
                Some(format!("{steps} agents · up to {concurrent} concurrent"))
            }
            None => Some("reading agent plan…".into()),
        },
        "status" | "wait" => result.and_then(run_counts),
        _ => None,
    };
    if let Some(detail) = detail {
        lines.push(Line::styled(
            format!("  {}", trunc(&detail, width.saturating_sub(2) as usize)),
            style::dim(theme),
        ));
    }
    lines
}

fn shorten_inline(value: &str, max: usize) -> String {
    trunc(&one_line(value), max)
}

fn run_identity(result: &str) -> Option<String> {
    let run = result
        .split_whitespace()
        .find_map(|word| word.strip_prefix("run_id="))?;
    Some(format!("run {}", &run[..run.len().min(8)]))
}

fn run_counts(result: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(result).ok()?;
    let nodes = value.get("nodes")?.as_array()?;
    let running = nodes
        .iter()
        .filter(|node| node.get("status").and_then(|v| v.as_str()) == Some("running"))
        .count();
    let settled = nodes
        .iter()
        .filter(|node| {
            matches!(
                node.get("status").and_then(|v| v.as_str()),
                Some("succeeded" | "failed" | "blocked" | "cancelled" | "interrupted")
            )
        })
        .count();
    let rejected = nodes
        .iter()
        .filter(|node| node.get("outcome").and_then(|v| v.as_str()) == Some("rejected"))
        .count();
    let mut summary = format!("{settled}/{} settled · {running} running", nodes.len());
    if rejected > 0 {
        summary.push_str(&format!(" · {rejected} rejected"));
    }
    Some(summary)
}

pub fn edit_lines_compact(
    args: &str,
    state: &ToolState,
    width: u16,
    theme: &Theme,
    max_lines: usize,
) -> Vec<Line<'static>> {
    edit_lines_compact_window(args, state, width, theme, 0, max_lines).0
}

/// Render a bounded page of the actual edit patch while arguments are
/// streaming. `start` is a content-row offset (not a raw patch-line offset),
/// and the returned offset can be passed back for the next page. This keeps a
/// caller from having to duplicate patch parsing merely to continue a large
/// edit preview.
pub fn edit_lines_compact_window(
    args: &str,
    state: &ToolState,
    width: u16,
    theme: &Theme,
    start: usize,
    max_lines: usize,
) -> (Vec<Line<'static>>, Option<usize>) {
    let patch = partial_string_field(args, "patch").unwrap_or_default();
    let files = edit_compact_entries(&patch);
    let summary = match files.len() {
        0 => "editing".to_string(),
        1 => format!("editing {}", files[0].0),
        n => format!("editing {n} files"),
    };
    let mut out = status_line(&summary, state, None, "edit", width, theme);
    if max_lines <= 1 {
        out.truncate(max_lines.max(1));
        return (out, None);
    }
    // Include a real, highlighted diff excerpt while arguments stream. The
    // diff parser walks only until the display budget is filled, so a huge
    // growing patch cannot monopolize a frame or accumulate styled output.
    let remaining = max_lines.saturating_sub(out.len());
    let (mut diff, next) = edit_diff_lines_window(&patch, width, theme, start, remaining);
    out.append(&mut diff);
    if out.len() >= max_lines {
        if next.is_some() {
            append_patch_truncation_indicator(&mut out, theme);
        }
        return (out, next);
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
            extra.push(Line::from(vec![Span::styled(
                row.clone(),
                style::dim(theme),
            )]));
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
    if next.is_some() {
        append_patch_truncation_indicator(&mut out, theme);
    }
    out.truncate(max_lines.max(1));
    (out, next)
}

fn append_patch_truncation_indicator(lines: &mut [Line<'static>], theme: &Theme) {
    if let Some(head) = lines.first_mut() {
        head.spans
            .push(Span::styled(" · … more patch lines", style::dim(theme)));
    }
}

// ---------------------------------------------------------------------------
// Per-tool renderers
// ---------------------------------------------------------------------------

/// bash: the command line is the headline; live output tail while running.
fn bash_lines(
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let tail = bash_mode_shows_output(args).then_some(tail).flatten();
    let Some(cmd) = bash_cmdline(args) else {
        return generic_lines("bash", args, state, tail, width, theme);
    };
    match state {
        ToolState::Preparing(_) | ToolState::Running(_) => {
            let mut out = vec![Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(trunc(&cmd, budget_for(width, 2, "")), style::tool(theme)),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            out
        }
        ToolState::Done { ok, bytes, error } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            let mut out = vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(
                    trunc(&cmd, budget_for(width, 2, &suffix)),
                    style::tool(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            if !*ok {
                append_tool_error(&mut out, "bash", error.as_deref(), width, theme);
            }
            out
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(trunc(&cmd, budget_for(width, 2, "")), style::tool(theme)),
        ])],
    }
}

fn bash_progress_label(
    parsed: &PartialJson,
    mode: &str,
    related_intent: Option<&str>,
) -> Option<String> {
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
        ToolState::Preparing(_) | ToolState::Running(_) => {
            let fixed = 2 + LABEL.len() + 1;
            let out = vec![Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(LABEL.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    trunc(&prompt, budget_for(width, fixed, "")),
                    style::dim(theme),
                ),
            ])];
            out
        }
        ToolState::Done { ok, bytes, error } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            let mut out = vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(
                    trunc(&prompt, budget_for(width, 2, &suffix)),
                    style::assistant(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])];
            if !*ok {
                append_tool_error(&mut out, LABEL, error.as_deref(), width, theme);
            }
            out
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(trunc(&prompt, budget_for(width, 2, "")), style::assistant(theme)),
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
            let message = parsed
                .str("message")
                .map(one_line)
                .unwrap_or_else(|| "message".into());
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
                let effort = parsed
                    .complete_str("effort")
                    .or_else(|| parsed.str("effort"));
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
    tool: &str,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    // Every tool headline is readable foreground text.  Details and retained
    // output rows remain dim, while the view may apply its narrow glint to the
    // title span only.
    let title_style = style::assistant(theme);
    match state {
        ToolState::Preparing(_) | ToolState::Running(_) => {
            let mut out = vec![Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(trunc(label, budget_for(width, 2, "")), title_style),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            out
        }
        ToolState::Done { ok, bytes, error } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            let mut out = vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(
                    trunc(label, budget_for(width, 2, &suffix)),
                    title_style,
                ),
                Span::styled(suffix, style::dim(theme)),
            ])];
            append_ansi_tail(&mut out, tail, width, theme);
            if !*ok {
                append_tool_error(&mut out, tool, error.as_deref(), width, theme);
            }
            out
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err(theme)),
            Span::styled(trunc(label, budget_for(width, 2, "")), title_style),
        ])],
    }
}

/// Render a failed result as a separate, tool-labelled diagnostic. Keeping
/// this out of the success headline preserves the useful subject (command,
/// prompt, or workflow operation) while making failures easy to scan.
fn append_tool_error(
    lines: &mut Vec<Line<'static>>,
    tool: &str,
    error: Option<&str>,
    width: u16,
    theme: &Theme,
) {
    let message = error
        .map(one_line)
        .filter(|message| !message.is_empty())
        .unwrap_or_else(|| "failed".to_string());
    let kind = classify_tool_error(&message);
    // Keep the stable `tool error` marker for existing transcripts while
    // adding the machine-derived class in a compact parenthetical.
    let prefix = format!("  {tool} error ({}): ", kind.label());
    let content = trunc(
        &format!("{prefix}{message}"),
        usize::from(width).max(prefix.chars().count()),
    );
    lines.push(Line::styled(content, style::tool_err(theme)));
}

/// edit: a header naming the touched files, then a capped mini diff.
fn edit_lines(args: &str, state: &ToolState, width: u16, theme: &Theme) -> Vec<Line<'static>> {
    const NAME: &str = "edit";
    let patch = partial_string_field(args, "patch").unwrap_or_default();
    let summary = edit_file_summary(&patch);
    let fixed = 2 + NAME.len() + 1; // glyph+space, name, separating space
    let head = match state {
        ToolState::Preparing(_) | ToolState::Running(_) => Line::from(vec![
            Span::styled(tool_icon(state), tool_icon_style(state, theme)),
            Span::styled(NAME.to_string(), style::tool(theme)),
            Span::raw(" "),
            Span::styled(
                trunc(&summary, budget_for(width, fixed, "")),
                style::dim(theme),
            ),
        ]),
        ToolState::Done { ok, bytes, .. } => {
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
            Span::styled(
                trunc(&summary, budget_for(width, fixed, "")),
                style::dim(theme),
            ),
        ]),
    };
    let mut out = vec![head];
    out.extend(edit_diff_lines(&patch, state, width, theme));
    if let ToolState::Done {
        ok: false, error, ..
    } = state
    {
        append_tool_error(&mut out, NAME, error.as_deref(), width, theme);
    }
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
fn quick_lines(
    name: &str,
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
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
        ToolState::Done { ok, bytes, .. } => {
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
    let mut out = vec![line];
    if let ToolState::Done {
        ok: false, error, ..
    } = state
    {
        append_tool_error(&mut out, name, error.as_deref(), width, theme);
    } else if matches!(state, ToolState::Done { ok: true, .. }) {
        // Read/search tools frequently return their only human-useful payload
        // through the captured result body. A compact preview makes the call
        // informative instead of reducing it to an opaque byte counter.
        if let Some(tail) = tail {
            for line in tail_lines(tail, 3) {
                out.push(Line::styled(
                    trunc(&format!("  │ {line}"), width as usize),
                    style::dim(theme),
                ));
            }
        }
    }
    out
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
        ToolState::Preparing(_) | ToolState::Running(_) => {
            let head = Line::from(vec![
                Span::styled(tool_icon(state), tool_icon_style(state, theme)),
                Span::styled(name.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    describe_args_live(name, args, budget_for(width, fixed, "")),
                    style::dim(theme),
                ),
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
        ToolState::Done { ok, bytes, error } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok, theme);
            let mut out = vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(name.to_string(), style::tool(theme)),
                Span::raw(" "),
                Span::styled(
                    describe_args(args, budget_for(width, fixed, &suffix)),
                    style::dim(theme),
                ),
                Span::styled(suffix, style::dim(theme)),
            ])];
            if !*ok {
                append_tool_error(&mut out, name, error.as_deref(), width, theme);
            }
            out
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
    let mut chars = tail.chars().peekable();
    while let Some(ch) = chars.next() {
        match ch {
            '\r' if chars.peek() == Some(&'\n') => {
                chars.next();
                lines.push(std::mem::take(&mut current));
            }
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
    lines
        .into_iter()
        .rev()
        .take(n)
        .collect::<Vec<_>>()
        .into_iter()
        .rev()
        .collect()
}

fn append_ansi_tail(out: &mut Vec<Line<'static>>, tail: Option<&str>, width: u16, theme: &Theme) {
    append_ansi_tail_with_limit(out, tail, width, theme, BASH_TAIL_MAX);
}

fn append_ansi_tail_with_limit(
    out: &mut Vec<Line<'static>>,
    tail: Option<&str>,
    width: u16,
    theme: &Theme,
    limit: usize,
) {
    let Some(tail) = tail else { return };
    let content_width = width.saturating_sub(4);
    for raw in tail_lines(tail, limit.max(1)) {
        let mut line = clip_line_width(ansi_line(&raw), content_width as usize);
        if line.width() == 0 {
            continue;
        }
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
        if ch == '\x1b' {
            match chars.peek().copied() {
                // CSI: consume through the standard final byte. Only SGR
                // changes presentation style; erase/cursor controls must not
                // leak into the transcript or swallow following output.
                Some('[') => {
                    chars.next();
                    let mut code = String::new();
                    let mut final_byte = None;
                    for next in chars.by_ref() {
                        if ('@'..='~').contains(&next) {
                            final_byte = Some(next);
                            break;
                        }
                        code.push(next);
                    }
                    if final_byte != Some('m') {
                        continue;
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
                }
                // OSC: consume metadata through BEL or ST (ESC followed by
                // backslash). This includes terminal progress/title/hyperlink
                // sequences which should never appear in command output.
                Some(']') => {
                    chars.next();
                    while let Some(next) = chars.next() {
                        if next == '\x07' {
                            break;
                        }
                        if next == '\x1b' && chars.peek() == Some(&'\\') {
                            chars.next();
                            break;
                        }
                    }
                }
                // Other ESC-prefixed controls are terminal metadata too.
                _ => {}
            }
        } else if ch.is_control() {
            // Do not expose C0 controls (including BEL) as transcript text.
        } else {
            text.push(if ch == '\t' { ' ' } else { ch });
        }
    }
    flush(&mut spans, &mut text, style);
    Line::from(spans)
}

/// Assemble the cmdline a bash call spawns: `command` followed by the
/// strings in its `args` array. Mirrors the tail matcher in view.rs.
pub fn bash_cmdline(args: &str) -> Option<String> {
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
        // Keep structured search metadata intact; the transcript wrapper will
        // wrap it to the pane width instead of replacing the tail with `…`.
        if matches!(name, "grep" | "glob") {
            return fields.join("  ");
        }
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

fn edit_diff_lines(
    patch: &str,
    _state: &ToolState,
    width: u16,
    theme: &Theme,
) -> Vec<Line<'static>> {
    let mut out = Vec::new();
    let mut path = String::new();
    let mut line_no = 1usize;
    let mut in_hunk = false;
    let mut highlighter = DiffHighlighter::default();
    for raw in patch.lines() {
        let trimmed = raw.trim();
        if let Some(next) = trimmed
            .strip_prefix("*** Add File:")
            .or_else(|| trimmed.strip_prefix("*** Update File:"))
            .or_else(|| trimmed.strip_prefix("*** Delete File:"))
        {
            path = next.trim().to_string();
            let (added, removed) = file_change_counts(patch, &path);
            in_hunk =
                trimmed.starts_with("*** Add File:") || trimmed.starts_with("*** Delete File:");
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
            in_hunk = !path.is_empty();
            out.push(Line::styled(trimmed.to_string(), style::bar(theme)));
        } else if !path.is_empty()
            && (in_hunk || raw.starts_with('+') || raw.starts_with('-'))
            && (raw.starts_with('+') || raw.starts_with('-') || raw.starts_with(' '))
            && !raw.starts_with("+++")
            && !raw.starts_with("---")
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
            // Highlight complete streamed lines as they arrive. The parser
            // operates on one line at a time and the shared syntax/theme
            // caches keep this bounded without retaining prior frames.
            spans.extend(highlighter.highlight(Some(&path), content, background));
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
    header.split_whitespace().find_map(|token| {
        token
            .strip_prefix('+')?
            .split(',')
            .next()?
            .parse::<usize>()
            .ok()
    })
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

fn diff_background(kind: char) -> ratatui::style::Color {
    match kind {
        '+' => ratatui::style::Color::Rgb(25, 76, 38),
        '-' => ratatui::style::Color::Rgb(92, 35, 38),
        _ => ratatui::style::Color::Reset,
    }
}

/// Stateful syntax highlighter for a patch excerpt.  Creating a syntect
/// `HighlightLines` for every streamed line is surprisingly expensive and
/// also loses multiline syntax state.  Keep one highlighter per current file;
/// changing files starts a fresh state while adjacent lines reuse it.
#[derive(Default)]
struct DiffHighlighter {
    path: Option<String>,
    highlighter: Option<HighlightLines<'static>>,
}

impl DiffHighlighter {
    fn highlight(
        &mut self,
        path: Option<&str>,
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
        let path = path.unwrap_or("");
        if self.path.as_deref() != Some(path) {
            let syntax_ref: &SyntaxReference = syntax
                .find_syntax_for_file(path)
                .ok()
                .flatten()
                .unwrap_or_else(|| syntax.find_syntax_plain_text());
            self.highlighter = Some(HighlightLines::new(syntax_ref, theme));
            self.path = Some(path.to_string());
        }
        let highlighted = self
            .highlighter
            .as_mut()
            .expect("diff highlighter initialized")
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tui::theme;
    use std::time::Instant;

    fn test_theme() -> theme::Theme {
        theme::default_theme()
    }

    #[test]
    fn tool_execution_lines_keep_control_plane_state_compact() {
        let now = chrono::Utc::now();
        let execution = ToolExecutionState {
            batch_id: "batch".into(),
            queue_position: 2,
            queue_total: 4,
            predecessor: Some("read-context".into()),
            lifecycle: ToolExecutionLifecycle::WaitingPermission,
            permission: Some(crate::tui::runtime_state::PermissionGateState {
                request_id: "request".into(),
                status: PermissionGateStatus::AutoDenied,
                requested_at: now,
                decision: Some(crate::tui::runtime_state::PermissionDecision {
                    allowed: false,
                    provenance: PermissionProvenance::Policy,
                    reason: Some("outside workspace".into()),
                    decided_at: now,
                }),
            }),
            classifier_reason: None,
            timestamps: crate::tui::runtime_state::ToolExecutionTimestamps {
                queued_at: Some(now - chrono::Duration::seconds(2)),
                ..Default::default()
            },
            output_tail: None,
            delegate: None,
        };
        let text = tool_execution_lines(&execution, 120, &test_theme())
            .into_iter()
            .map(|line| line.to_string())
            .collect::<Vec<_>>()
            .join("\n");
        assert!(text.contains("waiting permission"));
        assert!(text.contains("2/4"));
        assert!(text.contains("after read-context"));
        assert!(text.contains("permission: denied (policy)"));
        assert!(text.contains("outside workspace"));
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

    #[test]
    fn compaction_lines_keep_complete_streamed_summary_and_line_breaks() {
        let summary = "first line with a deliberately long unbroken summary that must not be truncated\nsecond line";
        let item = CompactionItem {
            generation: 7,
            summary: summary.into(),
            phase: CompactionPhase::Running(Instant::now()),
        };
        let rendered = compaction_lines(&item, 20, &test_theme())
            .into_iter()
            .map(|line| line.to_string())
            .collect::<Vec<_>>();
        let text = rendered.join("\n");
        assert!(text.contains(
            "first line with a deliberately long unbroken summary that must not be truncated"
        ));
        assert!(text.contains("second line"));
        assert_eq!(
            rendered.len(),
            3,
            "status plus one row per logical summary line"
        );
    }

    #[test]
    fn tool_errors_keep_distinct_recovery_classes() {
        let cases = [
            (
                "invalid arguments: mode requires 'proc_id'",
                ToolErrorKind::InvalidArguments,
                "invalid arguments",
            ),
            (
                "permission denied for tool `message`",
                ToolErrorKind::PermissionDenied,
                "permission denied",
            ),
            (
                "cancelled; killed proc_id=abc",
                ToolErrorKind::Cancelled,
                "cancelled",
            ),
            (
                "invalid proc_id: 'abc'",
                ToolErrorKind::ProcessId,
                "process id",
            ),
            (
                "no such process: abc",
                ToolErrorKind::ProcessId,
                "process id",
            ),
            (
                "unknown run_id: abc",
                ToolErrorKind::UnknownId,
                "unknown id",
            ),
            ("unknown tool: plugin", ToolErrorKind::Failed, "failed"),
        ];
        for (message, expected, label) in cases {
            assert_eq!(classify_tool_error(message), expected, "{message}");
            let lines = super::tool_lines(
                "task",
                r#"{"mode":"poll","run_id":"abc"}"#,
                &ToolState::Done {
                    ok: false,
                    bytes: message.len(),
                    error: Some(message.into()),
                },
                None,
                120,
                &test_theme(),
            );
            let rendered = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
            assert!(
                rendered.contains(&format!("workflow error ({label})")),
                "{rendered}"
            );
        }
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
        assert_eq!(
            tail_lines(
                "Downloading 1%\rDownloading 50%\rDownloading 100%\r\nDone",
                2
            ),
            vec!["Downloading 100%", "Done"]
        );
    }

    #[test]
    fn tail_lines_preserves_pty_crlf_lines() {
        assert_eq!(
            tail_lines("real output\r\nnext line\r\n", 3),
            vec!["real output", "next line"]
        );
    }

    #[test]
    fn ansi_line_strips_osc_progress_sequences() {
        let line = ansi_line("real output\x1b]9;4;0;\x1b\\\x1b]9;4;0;\x1b\\");
        assert_eq!(line.to_string(), "real output");
    }

    #[test]
    fn ansi_line_strips_bel_osc_and_keeps_following_text() {
        let line = ansi_line("before\x1b]0;title\x07after");
        assert_eq!(line.to_string(), "beforeafter");
    }

    #[test]
    fn ansi_line_discards_non_sgr_csi_without_swallowing_text() {
        let line = ansi_line("before\x1b[2Kafter");
        assert_eq!(line.to_string(), "beforeafter");
    }

    #[test]
    fn one_line_collapses_whitespace() {
        assert_eq!(one_line("fix\nthe   bug\n"), "fix the bug");
    }

    // tool_lines shapes --------------------------------------------------------

    #[test]
    fn bash_running_shows_cmdline_and_tail_without_elapsed() {
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
        assert!(!head.contains(" · "), "{head}");
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
                error: None,
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
                error: None,
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
            &ToolState::Done {
                ok: true,
                bytes: 3,
                error: None,
            },
            output,
            80,
        );
        assert_eq!(plain(&exec[1]), "  │ red");
        assert!(exec[1].spans.iter().any(|span| span.style.fg.is_some()));

        let poll = tool_lines(
            "bash",
            r#"{"mode":"poll","proc_id":"1"}"#,
            &ToolState::Done {
                ok: true,
                bytes: 3,
                error: None,
            },
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
                error: None,
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
            &ToolState::Done {
                ok: true,
                bytes: 5,
                error: None,
            },
            None,
            80,
        );
        assert!(lines.len() > 8);
        assert!(lines.iter().any(|line| plain(line).contains("+ l19")));
        assert!(!lines.iter().any(|line| plain(line).contains("more")));
    }

    #[test]
    fn compact_edit_output_stays_bounded_for_huge_streams() {
        let patch = format!(
            "*** Begin Patch\n*** Update File: src/huge.rs\n{}\n*** End Patch",
            (0..20_000)
                .map(|i| format!("+let value_{i} = {i};"))
                .collect::<Vec<_>>()
                .join("\n")
        );
        let args = serde_json::json!({ "patch": patch }).to_string();
        let lines = edit_lines_compact(
            &args,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
            4,
        );

        assert!(lines.len() <= 4);
        assert!(lines.iter().any(|line| plain(line).contains("src/huge.rs")));
        assert!(
            lines
                .iter()
                .any(|line| plain(line).contains("more patch lines"))
        );
    }

    #[test]
    fn compact_edit_prioritizes_diff_lines_over_patch_metadata() {
        let patch = "*** Begin Patch\n*** Update File: src/lib.rs\n@@ -4,2 +4,3 @@\n context\n-old\n+new\n*** End Patch";
        let args = serde_json::json!({ "patch": patch }).to_string();
        let lines = edit_lines_compact(
            &args,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
            4,
        );
        let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(text.contains("context"), "{text}");
        assert!(text.contains("- old"), "{text}");
        assert!(text.contains("+ new"), "{text}");
        assert!(!text.contains("patch.rs"), "{text}");
    }

    #[test]
    fn compact_edit_metadata_never_uses_content_budget() {
        let patch = format!(
            "*** Begin Patch\n*** Update File: src/lib.rs\n{}\n@@ -1 +1 @@\n-old\n+new\n*** End Patch",
            (0..50)
                .map(|i| format!(" metadata {i}"))
                .collect::<Vec<_>>()
                .join("\n")
        );
        let args = serde_json::json!({ "patch": patch }).to_string();
        let lines = edit_lines_compact(
            &args,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
            3,
        );
        let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(text.contains("- old"), "{text}");
        assert!(text.contains("+ new"), "{text}");
    }

    #[test]
    fn compact_edit_window_continues_at_content_row_boundary() {
        let patch = "*** Begin Patch\n*** Update File: src/lib.rs\n@@ -1,4 +1,4 @@\n-one\n+ONE\n-two\n+TWO\n-three\n+THREE\n*** End Patch";
        let args = serde_json::json!({ "patch": patch }).to_string();
        let (first, next) = edit_lines_compact_window(
            &args,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
            0,
            3,
        );
        let next = next.expect("first page should report continuation");
        let (second, end) = edit_lines_compact_window(
            &args,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
            next,
            3,
        );
        let first_text = first.iter().map(plain).collect::<Vec<_>>().join("\n");
        let second_text = second.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(first_text.contains("- one"), "{first_text}");
        assert!(second_text.contains("- two"), "{second_text}");
        assert!(second_text.contains("+ TWO"), "{second_text}");
        assert!(end.is_some());
    }

    #[test]
    fn quick_tools_render_one_line() {
        let args = r#"{"pattern":"foo","path":"src"}"#;
        let lines = tool_lines(
            "grep",
            args,
            &ToolState::Done {
                ok: true,
                bytes: 7,
                error: None,
            },
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
    fn failed_tools_render_dedicated_error_diagnostics() {
        let state = ToolState::Done {
            ok: false,
            bytes: 12,
            error: Some("permission denied\nuse sudo".into()),
        };
        let cases = [
            ("bash", r#"{"command":"cat","args":["x"]}"#, "bash error"),
            ("grep", r#"{"pattern":"x"}"#, "grep error"),
            ("delegate", r#"{"prompt":"inspect x"}"#, "delegate error"),
            ("task", r#"{"mode":"plan"}"#, "workflow error"),
            ("mcp_tool", r#"{"path":"x"}"#, "mcp_tool error"),
        ];
        for (name, args, expected) in cases {
            let lines = tool_lines(name, args, &state, None, 80);
            let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
            assert!(text.contains(expected), "{name}: {text}");
            assert!(
                text.contains("permission denied use sudo"),
                "{name}: {text}"
            );
        }
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
                    error: None,
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

    #[test]
    fn memory_lines_show_full_prompt_and_formatted_curator_result() {
        let prompt = "remember the deployment rule\nwith its second line";
        let lines = memory_lines_progressive(
            &serde_json::json!({"prompt": prompt}).to_string(),
            Some(r#"{"status":"ok","summary":"saved"}"#),
            &ToolState::Done {
                ok: true,
                bytes: 42,
                error: None,
            },
            40,
            &test_theme(),
        );
        let rendered = lines
            .into_iter()
            .map(|line| line.to_string())
            .collect::<Vec<_>>()
            .join("\n");
        assert!(rendered.contains("remember the deployment rule"));
        assert!(rendered.contains("with its second line"));
        assert!(rendered.contains("\"status\": \"ok\""));
        assert!(rendered.contains("\"summary\": \"saved\""));
    }

    #[test]
    fn cached_token_counts_use_an_uppercase_k() {
        assert_eq!(fmt_cached_tokens(999), "999");
        assert_eq!(fmt_cached_tokens(12_345), "12.3K");
        assert_eq!(fmt_cached_tokens(2_000_000), "2.0M");
    }

    #[test]
    fn quota_meter_formats_as_percent() {
        assert_eq!(format_quota_percent(2, 4), "50%");
        assert_eq!(format_quota_percent(1, 1), "100%");
        assert_eq!(format_quota_percent(60_760_272, 2_500_000_000), "2.4%");
        assert_eq!(format_quota_percent(0, 1_000_000_000), "0%");
    }

    #[test]
    fn search_lines_use_searching_then_searched_with_query() {
        let action = WebSearchAction::Search {
            query: Some("rust async".into()),
            queries: None,
        };
        let preparing = search_lines(
            &action,
            &SearchState::Preparing(Instant::now()),
            80,
            &test_theme(),
        );
        let head = plain(&preparing[0]);
        assert!(head.contains("searching"), "{head}");
        assert!(head.contains("rust async"), "{head}");
        assert!(!head.contains("web_search"), "{head}");

        let done = search_lines(&action, &SearchState::Done, 80, &test_theme());
        let head = plain(&done[0]);
        assert!(head.contains("searched"), "{head}");
        assert!(head.contains("rust async"), "{head}");
        assert!(head.starts_with("✓ "), "{head}");
    }

    #[test]
    fn search_lines_open_page_and_other_do_not_panic() {
        let open = search_lines(
            &WebSearchAction::OpenPage {
                url: Some("https://example.com".into()),
            },
            &SearchState::Done,
            80,
            &test_theme(),
        );
        assert!(plain(&open[0]).contains("https://example.com"));
        let other = search_lines(
            &WebSearchAction::Other,
            &SearchState::Interrupted,
            80,
            &test_theme(),
        );
        assert!(plain(&other[0]).contains("searched"));
        assert!(plain(&other[0]).starts_with("⊘ "));
    }

    #[test]
    fn message_streams_recipient_and_body_without_showing_raw_tool_json() {
        let lines = message_lines_progressive(
            r#"{"target":"label","label":"reviewer","message":"check the bound"#,
            None,
            &ToolState::Preparing(Instant::now()),
            80,
            &test_theme(),
        );
        let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(text.contains("composing message to reviewer"), "{text}");
        assert!(text.contains("check the bound…"), "{text}");
        assert!(!text.contains("{\"target\""), "{text}");
    }

    #[test]
    fn message_completion_uses_real_delivery_result() {
        let lines = message_lines_progressive(
            r#"{"target":"fleet","message":"pause"}"#,
            Some("agent-a: queued (target not currently live; durable mailbox updated)"),
            &ToolState::Done {
                ok: true,
                error: None,
                bytes: 10,
            },
            80,
            &test_theme(),
        );
        assert!(plain(&lines[0]).contains("fleet · queued"));
    }

    #[test]
    fn task_plan_presents_graph_shape_not_json() {
        let lines = task_lines_progressive(
            r#"{"mode":"plan","nodes":[{"key":"a"},{"key":"b"}],"edges":[{"from":"a","to":"b"}]}"#,
            None,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
        );
        let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(text.contains("planning workflow"), "{text}");
        assert!(text.contains("2 nodes · 1 dependencies"), "{text}");
        assert!(!text.contains("\"nodes\""), "{text}");
    }

    #[test]
    fn task_poll_summarizes_runtime_counts() {
        let result =
            r#"{"nodes":[{"status":"succeeded"},{"status":"running"},{"status":"pending"}]}"#;
        let lines = task_lines_progressive(
            r#"{"mode":"poll","run_id":"run-123"}"#,
            Some(result),
            &ToolState::Done {
                ok: true,
                error: None,
                bytes: result.len(),
            },
            80,
            &test_theme(),
        );
        assert!(plain(&lines[1]).contains("1/3 settled · 1 running"));
    }

    #[test]
    fn every_tool_title_uses_theme_foreground() {
        let theme = test_theme();
        for state in [ToolState::Running(Instant::now()), ToolState::Done { ok: true, bytes: 12, error: None }, ToolState::Interrupted] {
            for (tool, args) in [("bash", r#"{"command":"echo hello"}"#), ("delegate", r#"{"prompt":"inspect code"}"#), ("edit", "{}"), ("read", r#"{"path":"src/lib.rs"}"#), ("message", "{}"), ("memory", "{}"), ("workflow", "{}"), ("task", "{}"), ("unknown", "{}") ] {
                let lines = super::tool_lines(tool, args, &state, None, 80, &theme);
                let title = &lines[0].spans[1];
                assert_eq!(title.style.fg, Some(theme.fg), "{tool}: {state:?}");
            }
        }
    }

    #[test]
    fn workflow_presents_a_swarm_shape_without_raw_json() {
        let lines = workflow_lines_progressive(
            r#"{"action":"run","title":"Find startup regression","max_concurrent":3,"steps":[{"key":"io"},{"key":"network"},{"key":"synthesis"}]}"#,
            None,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
        );
        let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(
            text.contains("launching swarm · Find startup regression"),
            "{text}"
        );
        assert!(text.contains("3 agents · up to 3 concurrent"), "{text}");
        assert!(!text.contains("\"steps\""), "{text}");
    }

    #[test]
    fn workflow_wait_exposes_progress_and_rejected_verdicts() {
        let result = r#"{"nodes":[{"status":"succeeded","outcome":"approved"},{"status":"succeeded","outcome":"rejected"},{"status":"running"}]}"#;
        let lines = workflow_lines_progressive(
            r#"{"action":"wait","run_id":"run-123"}"#,
            Some(result),
            &ToolState::Done {
                ok: true,
                error: None,
                bytes: result.len(),
            },
            80,
            &test_theme(),
        );
        let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(text.contains("swarm settled"), "{text}");
        assert!(
            text.contains("2/3 settled · 1 running · 1 rejected"),
            "{text}"
        );
    }

    #[test]
    fn delegate_prefers_the_public_action_field() {
        let lines = delegate_lines_progressive(
            r#"{"action":"spawn","intent":"map the cache layer"}"#,
            &ToolState::Running(Instant::now()),
            80,
            &test_theme(),
            None,
        );
        let text = lines.iter().map(plain).collect::<Vec<_>>().join("\n");
        assert!(text.contains("delegating"), "{text}");
    }
}