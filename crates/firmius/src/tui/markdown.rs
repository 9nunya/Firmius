//! Markdown-to-terminal rendering for agent output.
//!
//! `tui-markdown` handles CommonMark/GFM, tables, fenced code blocks, syntax
//! highlighting, and the terminal representation of math. The small
//! preprocessing pass makes common TeX commands readable in a plain terminal.

use ratatui::style::Style;
use ratatui::text::{Line, Span};
use tui_markdown::{Options, StyleSheet, from_str_with_options};

use super::theme::{self, Theme};

#[derive(Clone, Copy, Debug)]
struct FirmiusMarkdownStyle {
    theme: Theme,
}

impl StyleSheet for FirmiusMarkdownStyle {
    fn heading(&self, level: u8) -> Style {
        // Derive heading colors from the theme accent at different lightness
        // so headings stay theme-consistent instead of always being cyan.
        let color = match level {
            1 => self.theme.accent,
            2 => theme::lighten(self.theme.accent, 0.15),
            3 => theme::darken(self.theme.accent, 0.10),
            _ => theme::darken(self.theme.accent, 0.25),
        };
        Style::new().fg(color).bold()
    }

    fn heading_marker(&self, _level: u8) -> &str {
        ""
    }

    fn table_header(&self) -> Style {
        Style::new().fg(self.theme.accent).bold()
    }

    fn table_border(&self) -> Style {
        Style::new().fg(self.theme.dim)
    }
}

/// Render Markdown into owned Ratatui text so transcript items can be stored
/// and sliced without borrowing the original agent history.
pub fn render(input: &str, base: Style, theme: &Theme) -> Vec<Line<'static>> {
    let prepared = compact_tables(&terminal_math(input));
    let options = Options::new(FirmiusMarkdownStyle { theme: *theme });
    let text = from_str_with_options(&prepared, &options);
    text.lines
        .into_iter()
        .map(|line| {
            let line_style = line.style;
            Line::from_iter(line.spans.into_iter().map(|span| {
                Span::styled(
                    span.content.into_owned(),
                    base.patch(line_style).patch(span.style),
                )
            }))
        })
        .collect()
}

/// `tui-markdown` intentionally sizes tables to their complete cell content.
/// That is correct for a standalone document, but an agent transcript may
/// contain enormous prose cells. Compact those cells before rendering so a
/// table remains a table instead of becoming one off-screen horizontal rule.
fn compact_tables(input: &str) -> String {
    const MAX_CELL_WIDTH: usize = 42;
    let lines: Vec<&str> = input.lines().collect();
    let mut out = Vec::with_capacity(lines.len());
    let mut i = 0;
    while i < lines.len() {
        if i + 1 < lines.len() && is_table_row(lines[i]) && is_table_separator(lines[i + 1]) {
            out.push(compact_table_row(lines[i], MAX_CELL_WIDTH));
            out.push(lines[i + 1].to_string());
            i += 2;
            while i < lines.len() && is_table_row(lines[i]) {
                out.push(compact_table_row(lines[i], MAX_CELL_WIDTH));
                i += 1;
            }
        } else {
            out.push(lines[i].to_string());
            i += 1;
        }
    }
    out.join("\n")
}

fn is_table_row(line: &str) -> bool {
    let trimmed = line.trim();
    trimmed.starts_with('|') && trimmed.ends_with('|') && trimmed.matches('|').count() >= 2
}

fn is_table_separator(line: &str) -> bool {
    is_table_row(line)
        && line
            .split('|')
            .filter(|cell| !cell.trim().is_empty())
            .all(|cell| cell.trim_matches(|c: char| c == ':' || c == '-').is_empty())
}

fn compact_table_row(line: &str, max_width: usize) -> String {
    let mut cells = Vec::new();
    for cell in line.trim().trim_matches('|').split('|') {
        let text = cell.trim();
        let compact = if text.chars().count() > max_width {
            let mut value: String = text.chars().take(max_width.saturating_sub(1)).collect();
            value.push('…');
            value
        } else {
            text.to_string()
        };
        cells.push(compact);
    }
    format!("| {} |", cells.join(" | "))
}

/// Convert a practical subset of TeX notation to Unicode. This is deliberately
/// terminal-friendly rather than pretending to be a full TeX layout engine.
/// Fractions and complex environments remain readable source text.
fn terminal_math(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let mut in_fence = false;
    for line in input.lines() {
        if line.trim_start().starts_with("```") || line.trim_start().starts_with("~~~") {
            in_fence = !in_fence;
            out.push_str(line);
            out.push('\n');
            continue;
        }
        if in_fence {
            out.push_str(line);
        } else {
            out.push_str(&math_symbols(line));
        }
        out.push('\n');
    }
    if !input.ends_with('\n') {
        out.pop();
    }
    out
}

fn math_symbols(text: &str) -> String {
    let mut result = text.to_string();
    for (from, to) in [
        ("\\alpha", "α"),
        ("\\beta", "β"),
        ("\\gamma", "γ"),
        ("\\delta", "δ"),
        ("\\Delta", "Δ"),
        ("\\theta", "θ"),
        ("\\lambda", "λ"),
        ("\\mu", "μ"),
        ("\\pi", "π"),
        ("\\sigma", "σ"),
        ("\\phi", "φ"),
        ("\\Omega", "Ω"),
        ("\\sum", "∑"),
        ("\\infty", "∞"),
        ("\\sqrt", "√"),
        ("\\times", "×"),
        ("\\cdot", "·"),
        ("\\leq", "≤"),
        ("\\geq", "≥"),
        ("\\neq", "≠"),
        ("\\approx", "≈"),
        ("\\to", "→"),
        ("\\rightarrow", "→"),
        ("\\left", ""),
        ("\\right", ""),
    ] {
        result = result.replace(from, to);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use ratatui::text::Text;

    #[test]
    fn renders_tables_code_and_math_as_terminal_text() {
        let lines = render(
            "# hi\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n```rust\nlet x = 1;\n```\n\n$\\alpha^2$",
            Style::default(),
            &theme::default_theme(),
        );
        let text = Text::from_iter(lines).to_string();
        assert!(text.contains("a"));
        assert!(text.contains("let x = 1;"));
        assert!(text.contains("α"));
    }

    #[test]
    fn headings_are_bold_without_hash_markers_and_tables_are_compacted() {
        let lines = render(
            "## Section\n\n| Name | Description |\n|---|---|\n| a | this is an intentionally very long description that should be compacted for the transcript |",
            Style::default(),
            &theme::default_theme(),
        );
        let text = Text::from_iter(lines.clone()).to_string();
        assert!(text.contains("Section"));
        assert!(!text.contains("## Section"));
        assert!(text.contains('…'));
        assert!(lines.iter().any(|line| line.spans.iter().any(|span| {
            span.content.contains("Section")
                && span
                    .style
                    .add_modifier
                    .contains(ratatui::style::Modifier::BOLD)
        })));
    }
}
