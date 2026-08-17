//! Single home for every color and weight the TUI uses. A future theme is a
//! change to this file and nothing else — no widget ever names a color.

use ratatui::style::{Color, Modifier, Style};

pub const ACCENT: Color = Color::Cyan;
pub const OK: Color = Color::Green;
pub const ERR: Color = Color::Red;
pub const WARN: Color = Color::Yellow;
pub const DIM: Color = Color::DarkGray;
pub const THINKING: Color = Color::Magenta;

pub fn user() -> Style {
    Style::new().fg(ACCENT).add_modifier(Modifier::BOLD)
}
pub fn assistant() -> Style {
    Style::default()
}
pub fn thinking() -> Style {
    Style::new().fg(THINKING)
}
pub fn tool() -> Style {
    Style::new().fg(ACCENT)
}
pub fn tool_ok() -> Style {
    Style::new().fg(OK)
}
pub fn tool_err() -> Style {
    Style::new().fg(ERR)
}
pub fn note() -> Style {
    Style::new().fg(WARN)
}
pub fn dim() -> Style {
    Style::new().fg(DIM)
}
pub fn bar() -> Style {
    Style::new().fg(DIM)
}
pub fn placeholder() -> Style {
    Style::new().fg(DIM).add_modifier(Modifier::ITALIC)
}
pub fn paste_block() -> Style {
    Style::new().fg(WARN)
}
pub fn border() -> Style {
    Style::new().fg(DIM)
}
pub fn spinner() -> Style {
    Style::new().fg(ACCENT).add_modifier(Modifier::BOLD)
}

pub const SPINNER: &[&str] = &["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
