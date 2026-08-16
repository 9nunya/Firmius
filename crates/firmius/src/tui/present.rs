//! Per-tool presentation rendering.
//!
//! Contract (pinned): given a tool call's name, its JSON args, its state, an
//! optional live output tail (for bash processes the app loop polls), and
//! the pane width — produce styled lines. Rich per-tool presentations live
//! here (bash live tail, delegate prompt excerpt, edit mini diff); unknown
//! tools and malformed args fall back to the generic shape.

use ratatui::style::Style;
use ratatui::text::{Line, Span};

use super::model::ToolState;
use super::style;
use std::time::Instant;

/// Max patch lines shown in the edit mini diff before elision.
const EDIT_DIFF_MAX: usize = 12;
/// Max live output lines shown beneath a running bash call.
const BASH_TAIL_MAX: usize = 3;

/// Render one tool call as styled transcript lines.
pub fn tool_lines(
    name: &str,
    args: &str,
    state: &ToolState,
    tail: Option<&str>,
    width: u16,
) -> Vec<Line<'static>> {
    match name {
        "bash" => bash_lines(args, state, tail, width),
        "delegate" => delegate_lines(args, state, width),
        "edit" => edit_lines(args, state, width),
        "read" | "list" | "grep" | "glob" => quick_lines(name, args, state, width),
        _ => generic_lines(name, args, state, tail, width),
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
) -> Vec<Line<'static>> {
    let Some(cmd) = bash_cmdline(args) else {
        return generic_lines("bash", args, state, tail, width);
    };
    match state {
        ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            let mut out = vec![Line::from(vec![
                Span::styled("⠹ ", style::spinner()),
                Span::styled(trunc(&cmd, budget_for(width, 2, &suffix)), style::tool()),
                Span::styled(suffix, style::dim()),
            ])];
            if let Some(tail) = tail {
                for line in tail_lines(tail, BASH_TAIL_MAX) {
                    out.push(Line::styled(
                        trunc(&format!("  │ {line}"), width as usize),
                        style::dim(),
                    ));
                }
            }
            out
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok);
            vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(trunc(&cmd, budget_for(width, 2, &suffix)), style::tool()),
                Span::styled(suffix, style::dim()),
            ])]
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err()),
            Span::styled(trunc(&cmd, budget_for(width, 2, "")), style::tool()),
        ])],
    }
}

/// delegate: a one-line excerpt of the instruction it was given.
fn delegate_lines(args: &str, state: &ToolState, width: u16) -> Vec<Line<'static>> {
    const LABEL: &str = "delegate";
    let Some(prompt) = parse_args(args)
        .and_then(|v| v.get("prompt").and_then(|p| p.as_str()).map(one_line))
    else {
        return generic_lines(LABEL, args, state, None, width);
    };
    match state {
        ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            let fixed = 2 + LABEL.len() + 1;
            // TODO(subagent-window): once per-subagent tool-call history is
            // plumbed into the model, append the subagent's last 3 tool calls
            // here as dim nested lines (same shape as the bash tail). No such
            // data source exists yet — do not invent one.
            vec![Line::from(vec![
                Span::styled("⠹ ", style::spinner()),
                Span::styled(LABEL.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(trunc(&prompt, budget_for(width, fixed, &suffix)), style::dim()),
                Span::styled(suffix, style::dim()),
            ])]
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok);
            vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(trunc(&prompt, budget_for(width, 2, &suffix)), style::dim()),
                Span::styled(suffix, style::dim()),
            ])]
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err()),
            Span::styled(trunc(&prompt, budget_for(width, 2, "")), style::dim()),
        ])],
    }
}

/// edit: a header naming the touched files, then a capped mini diff.
fn edit_lines(args: &str, state: &ToolState, width: u16) -> Vec<Line<'static>> {
    const NAME: &str = "edit";
    let Some(patch) = parse_args(args)
        .and_then(|v| v.get("patch").and_then(|p| p.as_str()).map(str::to_string))
    else {
        return generic_lines(NAME, args, state, None, width);
    };
    let summary = edit_file_summary(&patch);
    let fixed = 2 + NAME.len() + 1; // glyph+space, name, separating space
    let head = match state {
        ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            Line::from(vec![
                Span::styled("⠹ ", style::spinner()),
                Span::styled(NAME.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(trunc(&summary, budget_for(width, fixed, &suffix)), style::dim()),
                Span::styled(suffix, style::dim()),
            ])
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok);
            Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(NAME.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(trunc(&summary, budget_for(width, fixed, &suffix)), style::dim()),
                Span::styled(suffix, style::dim()),
            ])
        }
        ToolState::Interrupted => Line::from(vec![
            Span::styled("⊘ ", style::tool_err()),
            Span::styled(NAME.to_string(), style::tool()),
            Span::raw(" "),
            Span::styled(trunc(&summary, budget_for(width, fixed, "")), style::dim()),
        ]),
    };
    let mut out = vec![head];
    let body_w = (width as usize).saturating_sub(2).max(8);
    let (shown, hidden) = diff_preview(&patch, EDIT_DIFF_MAX);
    for line in shown {
        let st = match classify_diff_line(line) {
            DiffClass::Add => style::tool_ok(),
            DiffClass::Del => style::tool_err(),
            DiffClass::Header | DiffClass::Ctx => style::dim(),
        };
        out.push(Line::styled(trunc(line, body_w), st));
    }
    if hidden > 0 {
        out.push(Line::styled(format!("… {hidden} more"), style::dim()));
    }
    out
}

/// read/list/grep/glob: one dense line — glyph, name, args summary, bytes.
fn quick_lines(name: &str, args: &str, state: &ToolState, width: u16) -> Vec<Line<'static>> {
    let fixed = 2 + name.chars().count() + 1; // glyph+space, name, separating space
    let line = match state {
        ToolState::Running(_) => Line::from(vec![
            Span::styled("⠹ ", style::spinner()),
            Span::styled(name.to_string(), style::tool()),
            Span::raw(" "),
            Span::styled(describe_args(args, budget_for(width, fixed, "")), style::dim()),
        ]),
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok);
            Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(name.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(describe_args(args, budget_for(width, fixed, &suffix)), style::dim()),
                Span::styled(suffix, style::dim()),
            ])
        }
        ToolState::Interrupted => Line::from(vec![
            Span::styled("⊘ ", style::tool_err()),
            Span::styled(name.to_string(), style::tool()),
            Span::raw(" "),
            Span::styled(describe_args(args, budget_for(width, fixed, "")), style::dim()),
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
) -> Vec<Line<'static>> {
    let fixed = 2 + name.chars().count() + 1;
    match state {
        ToolState::Running(started) => {
            let suffix = format!(" · {}s", elapsed_secs(*started));
            let head = Line::from(vec![
                Span::styled("⠹ ", style::spinner()),
                Span::styled(name.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(describe_args(args, budget_for(width, fixed, &suffix)), style::dim()),
                Span::styled(suffix, style::dim()),
            ]);
            let mut out = vec![head];
            if let Some(tail) = tail {
                for line in tail_lines(tail, BASH_TAIL_MAX) {
                    out.push(Line::styled(
                        trunc(&format!("  │ {line}"), width as usize),
                        style::dim(),
                    ));
                }
            }
            out
        }
        ToolState::Done { ok, bytes } => {
            let suffix = format!(" · {}", fmt_bytes(*bytes));
            let (mark, st) = mark_ok(*ok);
            vec![Line::from(vec![
                Span::styled(format!("{mark} "), st),
                Span::styled(name.to_string(), style::tool()),
                Span::raw(" "),
                Span::styled(describe_args(args, budget_for(width, fixed, &suffix)), style::dim()),
                Span::styled(suffix, style::dim()),
            ])]
        }
        ToolState::Interrupted => vec![Line::from(vec![
            Span::styled("⊘ ", style::tool_err()),
            Span::styled(name.to_string(), style::tool()),
            Span::raw(" "),
            Span::styled(describe_args(args, budget_for(width, fixed, "")), style::dim()),
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
fn mark_ok(ok: bool) -> (&'static str, Style) {
    if ok { ("✓", style::tool_ok()) } else { ("✗", style::tool_err()) }
}

/// The last `n` lines of a process output blob, in original order.
fn tail_lines(tail: &str, n: usize) -> Vec<&str> {
    tail.lines()
        .rev()
        .take(n)
        .collect::<Vec<_>>()
        .into_iter()
        .rev()
        .collect()
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

// --- edit mini diff helpers -------------------------------------------------

/// What a patch line is, which decides its style in the mini diff.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DiffClass {
    /// `@@` hunk anchors and `***` patch/file markers.
    Header,
    /// An added (`+`) line.
    Add,
    /// A removed (`-`) line.
    Del,
    /// Context and anything else.
    Ctx,
}

fn classify_diff_line(line: &str) -> DiffClass {
    if line.starts_with("@@") || line.starts_with("***") {
        DiffClass::Header
    } else {
        match line.chars().next() {
            Some('+') => DiffClass::Add,
            Some('-') => DiffClass::Del,
            _ => DiffClass::Ctx,
        }
    }
}

/// The first `max` lines of a patch, plus how many were elided.
fn diff_preview(patch: &str, max: usize) -> (Vec<&str>, usize) {
    let lines: Vec<&str> = patch.lines().collect();
    let hidden = lines.len().saturating_sub(max);
    let shown = if hidden == 0 { lines } else { lines[..max].to_vec() };
    (shown, hidden)
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
    if parts.is_empty() { "patch".to_string() } else { parts.join(", ") }
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
        assert_eq!(describe_args(r#"{"path":"src/main.rs"}"#, 100), "src/main.rs");
    }

    #[test]
    fn describe_args_prompt_on_one_line() {
        assert_eq!(describe_args(r#"{"prompt":"do\nthe thing"}"#, 100), "do the thing");
    }

    #[test]
    fn describe_args_malformed_falls_back_to_raw() {
        assert_eq!(describe_args("not json at all", 100), "not json at all");
    }

    #[test]
    fn describe_args_truncates() {
        assert_eq!(describe_args(r#"{"path":"abcdefghijklmnop"}"#, 8), "abcdefgh…");
    }

    // bash cmdline assembly --------------------------------------------------

    #[test]
    fn bash_cmdline_joins_command_and_args() {
        let args = r#"{"command":"bash","args":["-c","echo hi && pwd"]}"#;
        assert_eq!(bash_cmdline(args).as_deref(), Some("bash -c echo hi && pwd"));
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

    // edit diff classification + caps -----------------------------------------

    #[test]
    fn classify_diff_line_kinds() {
        assert_eq!(classify_diff_line("*** Update File: a.rs"), DiffClass::Header);
        assert_eq!(classify_diff_line("*** Begin Patch"), DiffClass::Header);
        assert_eq!(classify_diff_line("@@ fn main() {"), DiffClass::Header);
        assert_eq!(classify_diff_line("+added"), DiffClass::Add);
        assert_eq!(classify_diff_line("-removed"), DiffClass::Del);
        assert_eq!(classify_diff_line(" context"), DiffClass::Ctx);
        assert_eq!(classify_diff_line(""), DiffClass::Ctx);
    }

    #[test]
    fn diff_preview_under_cap_keeps_all() {
        let (shown, hidden) = diff_preview("+a\n+b", EDIT_DIFF_MAX);
        assert_eq!(shown, vec!["+a", "+b"]);
        assert_eq!(hidden, 0);
    }

    #[test]
    fn diff_preview_over_cap_elides_the_rest() {
        let patch = (0..20).map(|i| format!("+l{i}")).collect::<Vec<_>>().join("\n");
        let (shown, hidden) = diff_preview(&patch, EDIT_DIFF_MAX);
        assert_eq!(shown.len(), EDIT_DIFF_MAX);
        assert_eq!(shown[0], "+l0");
        assert_eq!(hidden, 20 - EDIT_DIFF_MAX);
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
        assert_eq!(tail_lines("", 3), Vec::<&str>::new());
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
    fn bash_done_and_interrupted_shapes() {
        let args = r#"{"command":"make","args":["build"]}"#;
        let done = tool_lines("bash", args, &ToolState::Done { ok: true, bytes: 2048 }, None, 80);
        assert_eq!(plain(&done[0]), "✓ make build · 2.0KB");
        let fail = tool_lines("bash", args, &ToolState::Done { ok: false, bytes: 3 }, None, 80);
        assert_eq!(plain(&fail[0]), "✗ make build · 3B");
        let cut = tool_lines("bash", args, &ToolState::Interrupted, None, 80);
        assert_eq!(plain(&cut[0]), "⊘ make build");
    }

    #[test]
    fn delegate_running_excerpts_the_prompt() {
        let args = r#"{"prompt":"fix\nthe   flaky test","max_turns":3}"#;
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
        let lines = tool_lines("edit", &args, &ToolState::Done { ok: true, bytes: 42 }, None, 80);
        assert_eq!(lines.len(), 8); // header + 7 patch lines
        let head = plain(&lines[0]);
        assert!(head.starts_with("✓ edit"), "{head}");
        assert!(head.contains("~ src/a.rs"), "{head}");
        assert_eq!(plain(&lines[5]), "-old");
        assert_eq!(lines[5].style, style::tool_err());
        assert_eq!(plain(&lines[6]), "+new");
        assert_eq!(lines[6].style, style::tool_ok());
        assert_eq!(lines[1].style, style::dim());
    }

    #[test]
    fn edit_diff_caps_at_twelve_with_more_marker() {
        let patch = (0..20).map(|i| format!("+l{i}")).collect::<Vec<_>>().join("\n");
        let args = serde_json::json!({ "patch": patch }).to_string();
        let lines = tool_lines("edit", &args, &ToolState::Done { ok: true, bytes: 5 }, None, 80);
        assert_eq!(lines.len(), 1 + EDIT_DIFF_MAX + 1);
        assert_eq!(plain(&lines[lines.len() - 1]), "… 8 more");
    }

    #[test]
    fn quick_tools_render_one_line() {
        let args = r#"{"pattern":"foo","path":"src"}"#;
        let lines = tool_lines("grep", args, &ToolState::Done { ok: true, bytes: 7 }, None, 80);
        assert_eq!(lines.len(), 1);
        assert_eq!(plain(&lines[0]), "✓ grep pattern=foo · 7B");
        let run = tool_lines(
            "read",
            r#"{"path":"src/main.rs"}"#,
            &ToolState::Running(Instant::now()),
            None,
            80,
        );
        assert_eq!(plain(&run[0]), "⠹ read src/main.rs");
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
                &ToolState::Done { ok: true, bytes: 999_999 },
                None,
                w,
            );
            for line in &lines {
                // +1 tolerates the ellipsis char trunc() appends on overflow.
                assert!(plain_width(line) <= w as usize + 1, "{} > {}", plain_width(line), w);
            }
        }
    }
}