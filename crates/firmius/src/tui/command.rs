//! Slash commands. First pass: parse + metadata only, `/quit`, `/exit`,
//! `/help`. The full typed command set (model, effort, rewind, resume, ...)
//! replaces the internals; `parse` and `Command` are the pinned surface.

pub enum Command {
    Quit,
    Help,
    Unknown(String),
}

pub fn parse(line: &str) -> Command {
    let name = line.trim().split_whitespace().next().unwrap_or("");
    match name {
        "/quit" | "/exit" => Command::Quit,
        "/help" => Command::Help,
        other => Command::Unknown(other.to_string()),
    }
}

pub fn help_text() -> String {
    "/quit, /exit — leave (session is saved)\n/help — this list".to_string()
}