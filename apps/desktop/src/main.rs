//! Firmius desktop application entrypoint.
//!
//! The UI/runtime implementation lives in `desktop.rs`; this file intentionally
//! stays tiny so the executable boundary never becomes a second application.

mod blocks;
mod commands;
mod daemon;
mod desktop;
mod layout;
mod presentation;
mod reading;
mod session_model;
mod shell;
mod state;
mod tool_content;
mod workflows;

fn main() -> Result<(), slint::PlatformError> {
    desktop::run()
}
