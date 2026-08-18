//! Single home for every color and weight the TUI uses. Every function takes
//! a [`Theme`] reference so a palette swap is one change, not a per-file diff.
//! No widget ever names a color directly — it goes through one of these fns.

use ratatui::style::{Modifier, Style};

use super::theme::Theme;

pub fn user(theme: &Theme) -> Style {
    Style::new().fg(theme.accent).add_modifier(Modifier::BOLD)
}
pub fn assistant(theme: &Theme) -> Style { Style::new().fg(theme.fg) }
pub fn thinking(theme: &Theme) -> Style { Style::new().fg(theme.thinking) }
pub fn tool(theme: &Theme) -> Style { Style::new().fg(theme.accent) }
pub fn tool_ok(theme: &Theme) -> Style { Style::new().fg(theme.ok) }
pub fn tool_err(theme: &Theme) -> Style { Style::new().fg(theme.err) }
pub fn note(theme: &Theme) -> Style { Style::new().fg(theme.warn) }
pub fn dim(theme: &Theme) -> Style { Style::new().fg(theme.dim) }
pub fn bar(theme: &Theme) -> Style { Style::new().fg(theme.dim) }
pub fn placeholder(theme: &Theme) -> Style {
    Style::new().fg(theme.dim).add_modifier(Modifier::ITALIC)
}
pub fn paste_block(theme: &Theme) -> Style { Style::new().fg(theme.warn) }
pub fn border(theme: &Theme) -> Style { Style::new().fg(theme.border) }
pub fn spinner(theme: &Theme) -> Style {
    Style::new().fg(theme.accent).add_modifier(Modifier::BOLD)
}

pub const SPINNER: &[&str] = &["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];