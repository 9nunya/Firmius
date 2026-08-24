//! Slash commands: a typed `Command` enum, a whitespace-tolerant parser,
//! and a static metadata table that is the single source of truth for
//! help text and busy-gating (and completion, later). `/exit` is an
//! alias for `/quit`; every other command gets exactly one [`table`] row.

/// A parsed slash command. Aliases fold into their canonical variant.
#[derive(Debug, Clone, PartialEq)]
pub enum Command {
    /// Leave the TUI; the session is saved on the way out (`/exit` too).
    Quit,
    /// Print [`help_text`] into the transcript as a note.
    Help,
    /// Show session, agent, and turn status.
    Status,
    /// Compact the focused agent's context now.
    Compact,
    /// Save the session now.
    Save,
    /// List the agents in this session.
    Agents,
    /// Rewind the transcript; defaults to one turn when no count is given.
    Rewind { turns: usize },
    /// Clear the transcript view.
    Clear,
    /// Switch the primary model.
    Model { id: String },
    /// Set the reasoning effort.
    Effort { name: String },
    /// Switch the color theme.
    Theme { name: String },
    /// Resume a saved session; the latest one when no id is given.
    Resume { id: Option<String> },
    /// Add a provider account via its setup wizard; bare `/login` picks
    /// the kind first.
    Login { kind: Option<String> },
    /// Show stored accounts and quota for a provider kind or account id.
    Accounts { provider: String },
    /// Configure persona preferred models.
    Personas,
    /// Open the settings modal (retry policy, general options).
    Settings,
    /// Manage MCP servers.
    Mcp { action: McpAction },
    /// Set or show the session title. Bare `/title` prints the current one.
    Title { title: Option<String> },
    /// Copy the last assistant reply, or the whole focused transcript.
    Copy { all: bool },
    /// Export the live session as markdown. Defaults to `./<title>.md`.
    Export { path: Option<String> },
    /// Open the searchable session picker.
    Sessions,
    /// Save the current session and return to the welcome screen.
    New,
    /// List or switch hosted web-search mode. `None` lists; `"off"` disables.
    Search { mode: Option<String> },
}

/// A sub-command of `/mcp`.
#[derive(Debug, Clone, PartialEq)]
pub enum McpAction {
    List,
    Add {
        name: String,
        transport: McpTransportSpec,
    },
    Remove {
        name: String,
    },
    Start {
        name: String,
    },
    Stop {
        name: String,
    },
    Restart {
        name: String,
    },
}

/// How a new MCP server is reached.
#[derive(Debug, Clone, PartialEq)]
pub enum McpTransportSpec {
    Stdio { command: String, args: Vec<String> },
    Http { url: String },
}

impl Command {
    /// Canonical slash name; aliases report their canonical form.
    pub fn name(&self) -> &'static str {
        match self {
            Command::Quit => "/quit",
            Command::Help => "/help",
            Command::Status => "/status",
            Command::Compact => "/compact",
            Command::Save => "/save",
            Command::Agents => "/agents",
            Command::Rewind { .. } => "/rewind",
            Command::Clear => "/clear",
            Command::Model { .. } => "/model",
            Command::Effort { .. } => "/effort",
            Command::Theme { .. } => "/theme",
            Command::Resume { .. } => "/resume",
            Command::Login { .. } => "/login",
            Command::Accounts { .. } => "/accounts",
            Command::Personas => "/personas",
            Command::Settings => "/settings",
            Command::Mcp { .. } => "/mcp",
            Command::Title { .. } => "/title",
            Command::Copy { .. } => "/copy",
            Command::Export { .. } => "/export",
            Command::Sessions => "/sessions",
            Command::New => "/new",
            Command::Search { .. } => "/search",
        }
    }
}

/// Why a composer line failed to parse.
#[derive(Debug, Clone, PartialEq)]
pub enum CmdError {
    /// No such command, e.g. `/foo`. Holds the offending token.
    Unknown(String),
    /// A required argument is missing, e.g. `"model id"`.
    MissingArg(&'static str),
    /// An argument is malformed or surplus, e.g. rewind `"abc"`.
    BadArg(String),
}

impl std::fmt::Display for CmdError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            CmdError::Unknown(name) => write!(f, "unknown command: {name}"),
            CmdError::MissingArg(what) => write!(f, "missing argument: {what}"),
            CmdError::BadArg(tok) => write!(f, "bad argument: {tok}"),
        }
    }
}

/// Metadata for one command: the single source of truth for help text,
/// busy-gating, and (later) completion.
pub struct CommandInfo {
    /// Canonical slash name, e.g. `"/rewind"`.
    pub name: &'static str,
    /// Argument synopsis shown beside the name, e.g. `"[turns]"`.
    pub args: &'static str,
    /// One-line blurb for the help text.
    pub help: &'static str,
    /// Whether the command may run while the primary agent is busy.
    pub busy_ok: bool,
}

/// One row per [`Command`] variant, aliases folded into their canonical
/// command. Tests police that this stays honest with the enum.
pub fn table() -> &'static [CommandInfo] {
    &[
        CommandInfo {
            name: "/quit",
            args: "",
            help: "leave (session is saved); /exit is an alias",
            busy_ok: false,
        },
        CommandInfo {
            name: "/theme",
            args: "<name>",
            help: "switch the TUI color theme",
            busy_ok: true,
        },
        CommandInfo {
            name: "/compact",
            args: "",
            help: "compact the current agent context now",
            busy_ok: false,
        },
        CommandInfo {
            name: "/help",
            args: "",
            help: "print this list",
            busy_ok: true,
        },
        CommandInfo {
            name: "/status",
            args: "",
            help: "show session, agents, and turn status",
            busy_ok: true,
        },
        CommandInfo {
            name: "/save",
            args: "",
            help: "save the session now",
            busy_ok: false,
        },
        CommandInfo {
            name: "/agents",
            args: "",
            help: "list the agents in this session",
            busy_ok: true,
        },
        CommandInfo {
            name: "/rewind",
            args: "[turns]",
            help: "rewind the transcript (default 1 turn)",
            busy_ok: false,
        },
        CommandInfo {
            name: "/clear",
            args: "",
            help: "clear the transcript view",
            busy_ok: false,
        },
        CommandInfo {
            name: "/model",
            args: "<provider>/<id>",
            help: "switch the primary model",
            busy_ok: false,
        },
        CommandInfo {
            name: "/effort",
            args: "<name>",
            help: "set the reasoning effort",
            busy_ok: false,
        },
        CommandInfo {
            name: "/resume",
            args: "[id]",
            help: "resume a saved session (latest if no id)",
            busy_ok: false,
        },
        CommandInfo {
            name: "/login",
            args: "[kind]",
            help: "add a provider account (setup wizard)",
            busy_ok: true,
        },
        CommandInfo {
            name: "/accounts",
            args: "<provider>",
            help: "show stored accounts and quota",
            busy_ok: true,
        },
        CommandInfo {
            name: "/personas",
            args: "",
            help: "configure persona preferred models",
            busy_ok: true,
        },
        CommandInfo {
            name: "/settings",
            args: "",
            help: "configure retry policy and general options",
            busy_ok: true,
        },
        CommandInfo {
            name: "/mcp",
            args: "[list|add|start|stop|restart|remove]",
            help: "manage MCP servers",
            busy_ok: true,
        },
        CommandInfo {
            name: "/title",
            args: "[name]",
            help: "name this session (bare prints the current title)",
            busy_ok: true,
        },
        CommandInfo {
            name: "/copy",
            args: "[last|all]",
            help: "copy the last reply (or the whole transcript) to the clipboard",
            busy_ok: true,
        },
        CommandInfo {
            name: "/export",
            args: "[path]",
            help: "write the session as markdown",
            busy_ok: true,
        },
        CommandInfo {
            name: "/sessions",
            args: "",
            help: "browse and resume a saved session",
            busy_ok: false,
        },
        CommandInfo {
            name: "/new",
            args: "",
            help: "save this session and start a fresh one",
            busy_ok: false,
        },
        CommandInfo {
            name: "/search",
            args: "[mode]",
            help: "list or set hosted web search (cached|indexed|live|off)",
            busy_ok: true,
        },
    ]
}

/// Reject surplus tokens; the first offender names the error.
fn no_extra(rest: &[&str]) -> Result<(), CmdError> {
    match rest.first() {
        Some(extra) => Err(CmdError::BadArg((*extra).to_string())),
        None => Ok(()),
    }
}

/// Parse one composer line into a [`Command`]. Tokens are split on any
/// whitespace run; the first token names the command, required arguments
/// are positional, and surplus tokens are rejected.
pub fn parse(line: &str) -> Result<Command, CmdError> {
    let toks: Vec<&str> = line.split_whitespace().collect();
    let Some((head, rest)) = toks.split_first() else {
        return Err(CmdError::Unknown(String::new()));
    };
    match *head {
        "/quit" | "/exit" => no_extra(rest).map(|()| Command::Quit),
        "/help" => no_extra(rest).map(|()| Command::Help),
        "/status" => no_extra(rest).map(|()| Command::Status),
        "/compact" => no_extra(rest).map(|()| Command::Compact),
        "/save" => no_extra(rest).map(|()| Command::Save),
        "/agents" => no_extra(rest).map(|()| Command::Agents),
        "/clear" => no_extra(rest).map(|()| Command::Clear),
        "/rewind" => {
            let (turns, rest) = match rest.split_first() {
                None => (1, rest),
                Some((tok, rest)) => match tok.parse::<usize>() {
                    Ok(n) if n > 0 => (n, rest),
                    _ => return Err(CmdError::BadArg((*tok).to_string())),
                },
            };
            no_extra(rest).map(|()| Command::Rewind { turns })
        }
        "/model" => {
            let Some((id, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("provider/model"));
            };
            no_extra(rest)?;
            if !id.contains('/') {
                return Err(CmdError::BadArg((*id).to_string()));
            }
            Ok(Command::Model {
                id: (*id).to_string(),
            })
        }
        "/theme" => {
            let Some((name, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("theme name"));
            };
            no_extra(rest).map(|()| Command::Theme {
                name: (*name).to_string(),
            })
        }
        "/effort" => {
            let Some((name, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("effort name"));
            };
            no_extra(rest).map(|()| Command::Effort {
                name: (*name).to_string(),
            })
        }
        "/resume" => match rest.split_first() {
            None => Ok(Command::Resume { id: None }),
            Some((id, rest)) => no_extra(rest).map(|()| Command::Resume {
                id: Some((*id).to_string()),
            }),
        },
        "/login" => match rest.split_first() {
            None => Ok(Command::Login { kind: None }),
            Some((kind, rest)) => no_extra(rest).map(|()| Command::Login {
                kind: Some((*kind).to_string()),
            }),
        },
        "/accounts" => {
            let Some((provider, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("provider"));
            };
            no_extra(rest).map(|()| Command::Accounts {
                provider: (*provider).to_string(),
            })
        }
        "/personas" => no_extra(rest).map(|()| Command::Personas),
        "/settings" => no_extra(rest).map(|()| Command::Settings),
        "/mcp" => parse_mcp(rest),
        "/title" => {
            let after = line
                .trim_start()
                .strip_prefix("/title")
                .unwrap_or("")
                .trim();
            if after.is_empty() {
                Ok(Command::Title { title: None })
            } else {
                Ok(Command::Title {
                    title: Some(after.to_string()),
                })
            }
        }
        "/copy" => match rest.split_first() {
            None => Ok(Command::Copy { all: false }),
            Some((tok, rest)) => {
                no_extra(rest)?;
                match *tok {
                    "last" => Ok(Command::Copy { all: false }),
                    "all" => Ok(Command::Copy { all: true }),
                    other => Err(CmdError::BadArg(other.to_string())),
                }
            }
        },
        "/export" => match rest.split_first() {
            None => Ok(Command::Export { path: None }),
            Some((path, rest)) => no_extra(rest).map(|()| Command::Export {
                path: Some((*path).to_string()),
            }),
        },
        "/sessions" => no_extra(rest).map(|()| Command::Sessions),
        "/new" => no_extra(rest).map(|()| Command::New),
        "/search" => match rest.split_first() {
            None => Ok(Command::Search { mode: None }),
            Some((mode, rest)) => no_extra(rest).map(|()| Command::Search {
                mode: Some((*mode).to_string()),
            }),
        },
        other => Err(CmdError::Unknown(other.to_string())),
    }
}

fn parse_mcp(rest: &[&str]) -> Result<Command, CmdError> {
    let Some((sub, subrest)) = rest.split_first() else {
        return Ok(Command::Mcp {
            action: McpAction::List,
        });
    };
    match *sub {
        "list" | "status" => no_extra(subrest).map(|()| Command::Mcp {
            action: McpAction::List,
        }),
        "add" => parse_mcp_add(subrest),
        "remove" => parse_mcp_target(subrest, |name| McpAction::Remove { name }),
        "start" => parse_mcp_target(subrest, |name| McpAction::Start { name }),
        "stop" => parse_mcp_target(subrest, |name| McpAction::Stop { name }),
        "restart" => parse_mcp_target(subrest, |name| McpAction::Restart { name }),
        other => Err(CmdError::Unknown(format!("/mcp {other}"))),
    }
}

fn parse_mcp_target(
    toks: &[&str],
    variant: impl FnOnce(String) -> McpAction,
) -> Result<Command, CmdError> {
    let Some((name, rest)) = toks.split_first() else {
        return Err(CmdError::MissingArg("server name"));
    };
    no_extra(rest)?;
    Ok(Command::Mcp {
        action: variant((*name).to_string()),
    })
}

fn parse_mcp_add(toks: &[&str]) -> Result<Command, CmdError> {
    let Some((name, rest)) = toks.split_first() else {
        return Err(CmdError::MissingArg("server name"));
    };
    let Some((transport, rest)) = rest.split_first() else {
        return Err(CmdError::MissingArg("stdio or http"));
    };
    let transport = match *transport {
        "stdio" => {
            let Some((command, args)) = rest.split_first() else {
                return Err(CmdError::MissingArg("command"));
            };
            McpTransportSpec::Stdio {
                command: (*command).to_string(),
                args: args.iter().map(|arg| (*arg).to_string()).collect(),
            }
        }
        "http" => {
            let Some((url, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("url"));
            };
            no_extra(rest)?;
            McpTransportSpec::Http {
                url: (*url).to_string(),
            }
        }
        other => return Err(CmdError::BadArg((*other).to_string())),
    };
    Ok(Command::Mcp {
        action: McpAction::Add {
            name: (*name).to_string(),
            transport,
        },
    })
}

/// Whether `cmd` may run while the primary agent is busy; derived from
/// [`table`] so the metadata and the enum stay honest together.
pub fn busy_ok(cmd: &Command) -> bool {
    table()
        .iter()
        .find(|info| info.name == cmd.name())
        .is_some_and(|info| info.busy_ok)
}

/// Help text generated from [`table`]: left column is name plus args,
/// right column the blurb, aligned to the widest left entry. Never
/// hand-maintain this; edit the table instead.
pub fn help_text() -> String {
    let lefts: Vec<String> = table()
        .iter()
        .map(|info| {
            if info.args.is_empty() {
                info.name.to_string()
            } else {
                format!("{} {}", info.name, info.args)
            }
        })
        .collect();
    let width = lefts.iter().map(String::len).max().unwrap_or(0);
    table()
        .iter()
        .zip(&lefts)
        .map(|(info, left)| format!("{left:<width$}  {}", info.help))
        .collect::<Vec<_>>()
        .join("\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quit_and_exit_are_aliases() {
        assert_eq!(parse("/quit"), Ok(Command::Quit));
        assert_eq!(parse("/exit"), Ok(Command::Quit));
    }

    #[test]
    fn bare_commands_parse() {
        assert_eq!(parse("/help"), Ok(Command::Help));
        assert_eq!(parse("/status"), Ok(Command::Status));
        assert_eq!(parse("/save"), Ok(Command::Save));
        assert_eq!(parse("/agents"), Ok(Command::Agents));
        assert_eq!(parse("/clear"), Ok(Command::Clear));
    }

    #[test]
    fn rewind_defaults_to_one_turn() {
        assert_eq!(parse("/rewind"), Ok(Command::Rewind { turns: 1 }));
    }

    #[test]
    fn rewind_parses_an_explicit_count() {
        assert_eq!(parse("/rewind 3"), Ok(Command::Rewind { turns: 3 }));
    }

    #[test]
    fn model_takes_an_id() {
        assert_eq!(
            parse("/model test-provider/sonnet-4"),
            Ok(Command::Model {
                id: "test-provider/sonnet-4".to_string()
            }),
        );
        assert_eq!(
            parse("/model sonnet-4"),
            Err(CmdError::BadArg("sonnet-4".to_string()))
        );
    }

    #[test]
    fn theme_takes_a_name() {
        assert_eq!(
            parse("/theme nord"),
            Ok(Command::Theme {
                name: "nord".to_string(),
            }),
        );
        assert_eq!(parse("/theme"), Err(CmdError::MissingArg("theme name")));
    }

    #[test]
    fn effort_takes_a_name() {
        assert_eq!(
            parse("/effort high"),
            Ok(Command::Effort {
                name: "high".to_string()
            }),
        );
    }

    #[test]
    fn resume_bare_and_with_id() {
        assert_eq!(parse("/resume"), Ok(Command::Resume { id: None }));
        assert_eq!(
            parse("/resume sess_123"),
            Ok(Command::Resume {
                id: Some("sess_123".to_string())
            }),
        );
    }

    #[test]
    fn login_bare_and_with_kind() {
        assert_eq!(parse("/login"), Ok(Command::Login { kind: None }));
        assert_eq!(
            parse("/login opencode-go"),
            Ok(Command::Login {
                kind: Some("opencode-go".to_string())
            }),
        );
        assert_eq!(parse("/login a b"), Err(CmdError::BadArg("b".to_string())));
    }

    #[test]
    fn whitespace_is_tolerated() {
        assert_eq!(parse("  /rewind \t 5  "), Ok(Command::Rewind { turns: 5 }));
        assert_eq!(parse("   /help   "), Ok(Command::Help));
    }

    #[test]
    fn unknown_command_is_reported() {
        assert_eq!(parse("/foo"), Err(CmdError::Unknown("/foo".to_string())));
    }

    #[test]
    fn empty_line_is_unknown() {
        assert_eq!(parse(""), Err(CmdError::Unknown(String::new())));
        assert_eq!(parse("   "), Err(CmdError::Unknown(String::new())));
    }

    #[test]
    fn missing_required_args_are_reported() {
        assert_eq!(parse("/model"), Err(CmdError::MissingArg("provider/model")));
        assert_eq!(parse("/effort"), Err(CmdError::MissingArg("effort name")));
        assert_eq!(parse("/accounts"), Err(CmdError::MissingArg("provider")));
    }

    #[test]
    fn accounts_takes_a_provider() {
        assert_eq!(
            parse("/accounts opencode-go"),
            Ok(Command::Accounts {
                provider: "opencode-go".to_string()
            })
        );
        assert_eq!(
            parse("/accounts opencode-go extra"),
            Err(CmdError::BadArg("extra".to_string()))
        );
    }

    #[test]
    fn rewind_rejects_zero_and_non_numeric() {
        assert_eq!(parse("/rewind 0"), Err(CmdError::BadArg("0".to_string())));
        assert_eq!(
            parse("/rewind abc"),
            Err(CmdError::BadArg("abc".to_string()))
        );
        assert_eq!(parse("/rewind -2"), Err(CmdError::BadArg("-2".to_string())));
    }

    #[test]
    fn extra_args_are_rejected_with_the_offender() {
        assert_eq!(parse("/quit now"), Err(CmdError::BadArg("now".to_string())));
        assert_eq!(
            parse("/help please"),
            Err(CmdError::BadArg("please".to_string()))
        );
        assert_eq!(parse("/rewind 2 3"), Err(CmdError::BadArg("3".to_string())));
        assert_eq!(parse("/model a b"), Err(CmdError::BadArg("b".to_string())));
        assert_eq!(parse("/resume a b"), Err(CmdError::BadArg("b".to_string())));
    }

    #[test]
    fn error_display_messages() {
        assert_eq!(
            CmdError::Unknown("/foo".to_string()).to_string(),
            "unknown command: /foo",
        );
        assert_eq!(
            CmdError::MissingArg("model id").to_string(),
            "missing argument: model id",
        );
        assert_eq!(
            CmdError::BadArg("abc".to_string()).to_string(),
            "bad argument: abc"
        );
    }

    #[test]
    fn table_has_one_row_per_command() {
        // One row per Command variant; /exit folds into /quit.
        assert_eq!(table().len(), 23);
        let mut names: Vec<&str> = table().iter().map(|info| info.name).collect();
        names.sort_unstable();
        names.dedup();
        assert_eq!(names.len(), table().len(), "table rows must be unique");
    }

    #[test]
    fn parses_personas_without_arguments() {
        assert_eq!(parse("/personas"), Ok(Command::Personas));
        assert_eq!(
            parse("/personas extra"),
            Err(CmdError::BadArg("extra".to_string()))
        );
    }

    #[test]
    fn title_keeps_spaces_and_bare_shows_current() {
        assert_eq!(parse("/title"), Ok(Command::Title { title: None }));
        assert_eq!(
            parse("/title fix the auth flow"),
            Ok(Command::Title {
                title: Some("fix the auth flow".to_string())
            })
        );
    }

    #[test]
    fn copy_defaults_to_last_and_accepts_all() {
        assert_eq!(parse("/copy"), Ok(Command::Copy { all: false }));
        assert_eq!(parse("/copy last"), Ok(Command::Copy { all: false }));
        assert_eq!(parse("/copy all"), Ok(Command::Copy { all: true }));
        assert_eq!(
            parse("/copy nope"),
            Err(CmdError::BadArg("nope".to_string()))
        );
    }

    #[test]
    fn export_sessions_and_new_parse() {
        assert_eq!(parse("/export"), Ok(Command::Export { path: None }));
        assert_eq!(
            parse("/export notes.md"),
            Ok(Command::Export {
                path: Some("notes.md".to_string())
            })
        );
        assert_eq!(parse("/sessions"), Ok(Command::Sessions));
        assert_eq!(parse("/new"), Ok(Command::New));
    }

    #[test]
    fn search_bare_and_with_mode() {
        assert_eq!(parse("/search"), Ok(Command::Search { mode: None }));
        assert_eq!(
            parse("/search live"),
            Ok(Command::Search {
                mode: Some("live".to_string())
            })
        );
        assert_eq!(
            parse("/search off"),
            Ok(Command::Search {
                mode: Some("off".to_string())
            })
        );
        assert_eq!(
            parse("/search cached extra"),
            Err(CmdError::BadArg("extra".to_string()))
        );
    }

    #[test]
    fn mcp_lists_by_default() {
        assert_eq!(
            parse("/mcp"),
            Ok(Command::Mcp {
                action: McpAction::List
            })
        );
        assert_eq!(
            parse("/mcp list"),
            Ok(Command::Mcp {
                action: McpAction::List
            })
        );
    }

    #[test]
    fn mcp_add_parses_both_transports() {
        assert_eq!(
            parse("/mcp add ast-grep stdio npx -y ast-grep-mcp"),
            Ok(Command::Mcp {
                action: McpAction::Add {
                    name: "ast-grep".to_string(),
                    transport: McpTransportSpec::Stdio {
                        command: "npx".to_string(),
                        args: vec!["-y".to_string(), "ast-grep-mcp".to_string()],
                    },
                }
            })
        );
        assert_eq!(
            parse("/mcp add remote http https://example.com/mcp"),
            Ok(Command::Mcp {
                action: McpAction::Add {
                    name: "remote".to_string(),
                    transport: McpTransportSpec::Http {
                        url: "https://example.com/mcp".to_string(),
                    },
                }
            })
        );
    }

    #[test]
    fn mcp_targets_parse() {
        for (input, expected) in [
            (
                "/mcp start ast-grep",
                Command::Mcp {
                    action: McpAction::Start {
                        name: "ast-grep".into(),
                    },
                },
            ),
            (
                "/mcp stop ast-grep",
                Command::Mcp {
                    action: McpAction::Stop {
                        name: "ast-grep".into(),
                    },
                },
            ),
            (
                "/mcp restart ast-grep",
                Command::Mcp {
                    action: McpAction::Restart {
                        name: "ast-grep".into(),
                    },
                },
            ),
            (
                "/mcp remove ast-grep",
                Command::Mcp {
                    action: McpAction::Remove {
                        name: "ast-grep".into(),
                    },
                },
            ),
        ] {
            assert_eq!(parse(input), Ok(expected), "{input}");
        }
    }

    #[test]
    fn help_text_lists_every_command() {
        let help = help_text();
        for info in table() {
            assert!(help.contains(info.name), "help is missing {}", info.name);
        }
    }

    #[test]
    fn busy_ok_matches_the_table() {
        let cases = [
            (Command::Quit, false),
            (Command::Compact, false),
            (Command::Help, true),
            (Command::Status, true),
            (Command::Save, false),
            (Command::Agents, true),
            (Command::Rewind { turns: 1 }, false),
            (Command::Clear, false),
            (
                Command::Model {
                    id: "m".to_string(),
                },
                false,
            ),
            (
                Command::Effort {
                    name: "e".to_string(),
                },
                false,
            ),
            (
                Command::Theme {
                    name: "firmius".to_string(),
                },
                true,
            ),
            (Command::Resume { id: None }, false),
            (Command::Login { kind: None }, true),
            (
                Command::Accounts {
                    provider: "opencode-go".to_string(),
                },
                true,
            ),
            (Command::Personas, true),
            (Command::Settings, true),
            (
                Command::Mcp {
                    action: McpAction::List,
                },
                true,
            ),
            (Command::Title { title: None }, true),
            (Command::Copy { all: false }, true),
            (Command::Export { path: None }, true),
            (Command::Sessions, false),
            (Command::New, false),
            (Command::Search { mode: None }, true),
        ];
        for (cmd, want) in &cases {
            assert_eq!(busy_ok(cmd), *want, "busy_ok for {}", cmd.name());
        }
        // Every variant is covered exactly once, one case per table row.
        assert_eq!(cases.len(), table().len());
    }
}
