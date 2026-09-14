//! Single home for every color and weight the TUI uses. Every function takes
//! a [`Theme`] reference so a palette swap is one change, not a per-file diff.
//! No widget ever names a color directly — it goes through one of these fns.

use ratatui::style::{Modifier, Style};

use super::theme::Theme;

pub fn user(theme: &Theme) -> Style {
    Style::new().fg(theme.accent).add_modifier(Modifier::BOLD)
}

/// User turns are rendered as full-width, gently recessed cards.  Keep the
/// foreground treatment of `user` while using the theme's dim background so
/// the card remains legible across palette changes.
pub fn user_block(theme: &Theme) -> Style {
    // Derive this from the active terminal background rather than a palette's
    // separately curated surface color: user cards should remain a dimmed
    // version of whatever background the current theme actually uses.
    user(theme).bg(super::theme::darken(theme.bg, 0.35))
}

pub fn user_block_bg(theme: &Theme) -> ratatui::style::Color {
    super::theme::darken(theme.bg, 0.35)
}
pub fn assistant(theme: &Theme) -> Style {
    Style::new().fg(theme.fg)
}
pub fn thinking(theme: &Theme) -> Style {
    Style::new().fg(theme.thinking)
}
pub fn tool(theme: &Theme) -> Style {
    Style::new().fg(theme.fg)
}
pub fn tool_ok(theme: &Theme) -> Style {
    Style::new().fg(theme.ok)
}
pub fn tool_err(theme: &Theme) -> Style {
    Style::new().fg(theme.err)
}
pub fn note(theme: &Theme) -> Style {
    Style::new().fg(theme.warn)
}
pub fn dim(theme: &Theme) -> Style {
    // Metadata, rails, and retained-output prefixes must recede from prose.
    // Using the normal foreground here made every supposedly subdued surface
    // compete with assistant text and defeated the semantic color palette.
    Style::new().fg(theme.dim)
}
pub fn bar(theme: &Theme) -> Style {
    Style::new().fg(theme.dim)
}
pub fn placeholder(theme: &Theme) -> Style {
    Style::new().fg(theme.dim).add_modifier(Modifier::ITALIC)
}
pub fn paste_block(theme: &Theme) -> Style {
    Style::new().fg(theme.warn)
}
pub fn border(theme: &Theme) -> Style {
    Style::new().fg(theme.border)
}
pub fn spinner(theme: &Theme) -> Style {
    Style::new().fg(theme.accent).add_modifier(Modifier::BOLD)
}

pub fn work_status(theme: &Theme, status: firmius_core::ExecutionStatus) -> Style {
    match status {
        firmius_core::ExecutionStatus::Succeeded => Style::new().fg(theme.ok),
        firmius_core::ExecutionStatus::Failed | firmius_core::ExecutionStatus::Blocked => {
            Style::new().fg(theme.err).add_modifier(Modifier::BOLD)
        }
        firmius_core::ExecutionStatus::Running => Style::new().fg(theme.accent),
        firmius_core::ExecutionStatus::Interrupted => Style::new().fg(theme.warn),
        _ => Style::new().fg(theme.dim),
    }
}

/// Style a checklist row from its presentation state. `Starting` is kept
/// separate from `Running`: amber/bold means the delegated worker is being
/// brought up, while cyan means it has actually begun execution.
pub fn work_presentation(theme: &Theme, presentation: super::work::WorkPresentation) -> Style {
    match presentation {
        super::work::WorkPresentation::Starting => {
            Style::new().fg(theme.warn).add_modifier(Modifier::BOLD)
        }
        super::work::WorkPresentation::Pending | super::work::WorkPresentation::Ready => {
            Style::new().fg(theme.dim)
        }
        super::work::WorkPresentation::Running => Style::new().fg(theme.accent),
        super::work::WorkPresentation::Succeeded => Style::new().fg(theme.ok),
        super::work::WorkPresentation::Failed | super::work::WorkPresentation::Blocked => {
            Style::new().fg(theme.err).add_modifier(Modifier::BOLD)
        }
        super::work::WorkPresentation::Interrupted => Style::new().fg(theme.warn),
        super::work::WorkPresentation::Cancelled | super::work::WorkPresentation::Skipped => {
            Style::new().fg(theme.dim)
        }
    }
}

/// Style a native todo rail row from its presentation state.  Blocked work is
/// the loudest state on the rail because it is the only one that needs a
/// decision; settled rows recede.
pub fn todo_presentation(theme: &Theme, presentation: super::todo::TodoPresentation) -> Style {
    match presentation {
        super::todo::TodoPresentation::Blocked => {
            Style::new().fg(theme.err).add_modifier(Modifier::BOLD)
        }
        super::todo::TodoPresentation::InProgress => Style::new().fg(theme.accent),
        super::todo::TodoPresentation::Pending => Style::new().fg(theme.dim),
        super::todo::TodoPresentation::Completed => Style::new().fg(theme.ok),
        super::todo::TodoPresentation::Cancelled => Style::new().fg(theme.dim),
    }
}

pub const SPINNER: &[&str] = &["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];