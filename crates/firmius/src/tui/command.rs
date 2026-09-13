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
    /// Check the official release endpoint without changing the install.
    UpdateCheck,
    /// List SSH aliases discovered from the user's SSH config.
    SshHosts,
    /// Remember a discovered SSH alias and its default workspace directory.
    SshAdd { alias: String, directory: String },
    /// Start a new daemon-backed session in an SSH workspace.
    Ssh { alias: String, directory: String },
    /// Start a new daemon-backed session using a saved SSH workspace.
    SshSaved { alias: String },
    /// Compact the focused agent's context now.
    Compact,
    /// Save the session now.
    Save,
    /// List the agents in this session.
    Agents,
    /// Rewind the transcript; defaults to one turn when no count is given.
    Rewind { turns: usize },
    /// Undo, redo, or inspect the focused agent's file edit history.
    EditHistory { action: String },
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
    /// Reopen the optional first-run launchpad.
    Onboarding,
    /// Manage MCP servers.
    Mcp { action: McpAction },
    /// Set or show the session title. Bare `/title` prints the current one.
    Title { title: Option<String> },
    /// Copy the last assistant reply, or the whole focused transcript.
    Copy { all: bool },
    /// Export the live session as markdown. Defaults to `./<title>.md`.
    Export { path: Option<String> },
    /// Save the current session and return to the welcome screen.
    New,
    /// List or switch hosted web-search mode. `None` lists; `"off"` disables.
    Search { mode: Option<String> },
    /// Create or inspect a durable goal through the daemon goal API.
    Goal { action: GoalAction },
    /// Discover and use prompt workflow files.
    Workflow { action: WorkflowAction },
    /// Open the daemon-backed permission policy and activity surface.
    Permissions,
    /// Search durable, scope-filtered memory in the attached workspace.
    Memory { query: String },
}

fn parse_workflow(rest: &[&str]) -> Result<Command, CmdError> {
    let Some((sub, args)) = rest.split_first() else {
        return Ok(Command::Workflow {
            action: WorkflowAction::Picker,
        });
    };
    let Some(path) = args.first() else {
        return match *sub {
            "list" => Ok(Command::Workflow {
                action: WorkflowAction::List,
            }),
            "insert" => Err(CmdError::MissingArg("workflow path")),
            "run" => Err(CmdError::MissingArg("workflow path")),
            other => Err(CmdError::BadArg(other.to_string())),
        };
    };
    no_extra(&args[1..])?;
    match *sub {
        "insert" => Ok(Command::Workflow {
            action: WorkflowAction::Insert {
                path: (*path).to_string(),
            },
        }),
        "run" => Ok(Command::Workflow {
            action: WorkflowAction::Run {
                path: (*path).to_string(),
            },
        }),
        "list" => Err(CmdError::BadArg((*path).to_string())),
        other => Err(CmdError::BadArg(other.to_string())),
    }
}

/// Operations exposed by the `/workflow` command.
#[derive(Debug, Clone, PartialEq)]
pub enum WorkflowAction {
    /// Open the fuzzy workflow picker.
    Picker,
    /// Print available workflow files to the transcript.
    List,
    /// Insert a workflow's content into the composer.
    Insert { path: String },
    /// Insert a workflow and immediately submit it as a prompt.
    Run { path: String },
}

fn shell_tokens(line: &str) -> Result<Vec<String>, CmdError> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quote = None;
    let mut escaped = false;
    let mut started = false;
    for c in line.chars() {
        if escaped {
            cur.push(c);
            escaped = false;
            started = true;
            continue;
        }
        match quote {
            Some(q) if c == q => quote = None,
            Some('\'') => {
                cur.push(c);
                started = true;
            }
            Some(_) if c == '\\' => escaped = true,
            Some(_) => {
                cur.push(c);
                started = true;
            }
            None => match c {
                // Apostrophes inside ordinary words (for example
                // `daemon's`) are natural-language punctuation, not the
                // beginning of a quoted argument.  Only treat a quote as a
                // delimiter at an argument boundary; this keeps `/goal`
                // usable with unquoted prose while preserving `--check
                // 'cargo test'`.
                '\'' | '"' if !started => {
                    quote = Some(c);
                    started = true;
                }
                '\'' | '"' => {
                    cur.push(c);
                    started = true;
                }
                '\\' => {
                    escaped = true;
                    started = true;
                }
                c if c.is_whitespace() => {
                    if started {
                        out.push(std::mem::take(&mut cur));
                        started = false;
                    }
                }
                _ => {
                    cur.push(c);
                    started = true;
                }
            },
        }
    }
    if escaped {
        return Err(CmdError::BadArg("trailing escape".into()));
    }
    if quote.is_some() {
        return Err(CmdError::BadArg("unterminated quote".into()));
    }
    if started {
        out.push(cur);
    }
    Ok(out)
}

fn parse_goal(rest: &[&str]) -> Result<Command, CmdError> {
    let Some((sub, subrest)) = rest.split_first() else {
        return Err(CmdError::MissingArg("goal subcommand"));
    };
    let action = match *sub {
        "approve" | "activate" | "reject" => {
            let Some(goal_id) = subrest.first() else {
                return Err(CmdError::MissingArg("goal id"));
            };
            no_extra(&subrest[1..])?;
            GoalAction::Lifecycle {
                action: (*sub).to_string(),
                goal_id: (*goal_id).to_string(),
            }
        }
        "list" => {
            no_extra(subrest)?;
            GoalAction::List
        }
        "create" => {
            let mut parts = Vec::new();
            let mut check = None;
            let mut max_steps = None;
            let mut approval = false;
            let mut i = 0;
            while i < subrest.len() {
                match subrest[i] {
                    "--check" => {
                        i += 1;
                        let Some(value) = subrest.get(i) else {
                            return Err(CmdError::MissingArg("check"));
                        };
                        if value.starts_with("--") {
                            return Err(CmdError::MissingArg("check"));
                        }
                        check = Some((*value).to_string());
                    }
                    "--max-steps" => {
                        i += 1;
                        let Some(value) = subrest.get(i).filter(|value| !value.starts_with("--"))
                        else {
                            return Err(CmdError::MissingArg("max-steps"));
                        };
                        let parsed = value
                            .parse::<u32>()
                            .map_err(|_| CmdError::BadArg((*value).to_string()))?;
                        if parsed == 0 {
                            return Err(CmdError::BadArg((*value).to_string()));
                        }
                        max_steps = Some(parsed);
                    }
                    "--approval" => approval = true,
                    value => parts.push(value),
                }
                i += 1;
            }
            let description = parts.join(" ");
            if description.trim().is_empty() {
                return Err(CmdError::MissingArg("goal description"));
            }
            GoalAction::Create {
                description,
                check,
                max_steps,
                approval,
                auto_activate: false,
            }
        }
        "status" => {
            let Some((goal_id, rest)) = subrest.split_first() else {
                return Err(CmdError::MissingArg("goal id"));
            };
            no_extra(rest)?;
            GoalAction::Status {
                goal_id: (*goal_id).to_string(),
            }
        }
        "check" => {
            let Some((goal_id, rest)) = subrest.split_first() else {
                return Err(CmdError::MissingArg("goal id"));
            };
            let Some((check_id, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("check id"));
            };
            no_extra(rest)?;
            GoalAction::Check {
                goal_id: (*goal_id).to_string(),
                check_id: (*check_id).to_string(),
            }
        }
        "cancel" => {
            let Some((goal_id, reason)) = subrest.split_first() else {
                return Err(CmdError::MissingArg("goal id"));
            };
            GoalAction::Cancel {
                goal_id: (*goal_id).to_string(),
                reason: (!reason.is_empty()).then(|| reason.join(" ")),
            }
        }
        // `/goal <natural language>` is the primary shorthand. Only the
        // reserved lifecycle words above are subcommands; any other first
        // word belongs to the description instead of being mistaken for a
        // command name.
        _ => GoalAction::Create {
            description: std::iter::once(*sub)
                .chain(subrest.iter().copied())
                .filter(|part| !part.is_empty())
                .collect::<Vec<_>>()
                .join(" "),
            check: None,
            max_steps: None,
            approval: false,
            auto_activate: false,
        },
    };
    Ok(Command::Goal { action })
}

/// Operations exposed by the convenient `/goal` command.
#[derive(Debug, Clone, PartialEq)]
pub enum GoalAction {
    /// Explicit goal creation with a natural-language description.
    Create {
        description: String,
        check: Option<String>,
        max_steps: Option<u32>,
        approval: bool,
        auto_activate: bool,
    },
    Lifecycle {
        action: String,
        goal_id: String,
    },
    List,
    Status {
        goal_id: String,
    },
    Check {
        goal_id: String,
        check_id: String,
    },
    Cancel {
        goal_id: String,
        reason: Option<String>,
    },
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
            Command::UpdateCheck => "/update-check",
            Command::SshHosts => "/ssh-hosts",
            Command::SshAdd { .. } => "/ssh-add",
            Command::Ssh { .. } => "/ssh",
            Command::SshSaved { .. } => "/ssh-saved",
            Command::Compact => "/compact",
            Command::Save => "/save",
            Command::Agents => "/agents",
            Command::Rewind { .. } => "/rewind",
            Command::EditHistory { .. } => "/edit-history",
            Command::Clear => "/clear",
            Command::Model { .. } => "/model",
            Command::Effort { .. } => "/effort",
            Command::Theme { .. } => "/theme",
            Command::Resume { .. } => "/resume",
            Command::Login { .. } => "/login",
            Command::Accounts { .. } => "/accounts",
            Command::Personas => "/personas",
            Command::Settings => "/settings",
            Command::Onboarding => "/onboarding",
            Command::Mcp { .. } => "/mcp",
            Command::Title { .. } => "/title",
            Command::Copy { .. } => "/copy",
            Command::Export { .. } => "/export",
            Command::New => "/new",
            Command::Search { .. } => "/search",
            Command::Goal { .. } => "/goal",
            Command::Workflow { .. } => "/workflow",
            Command::Permissions => "/permissions",
            Command::Memory { .. } => "/memory",
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
            name: "/permissions",
            args: "",
            help: "review permission mode, rules, session, activity, and tools",
            busy_ok: true,
        },
        CommandInfo {
            name: "/goal",
            args: "create <description>|[list|status|check|cancel]",
            help: "create or inspect a durable goal via the daemon",
            busy_ok: true,
        },
        CommandInfo {
            name: "/workflow",
            args: "[list|insert|run] [path]",
            help: "browse, insert, or run a workflow prompt file",
            busy_ok: true,
        },
        CommandInfo {
            name: "/onboarding",
            args: "",
            help: "reopen the getting-started launchpad",
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
            name: "/update-check",
            args: "",
            help: "check the official latest release",
            busy_ok: true,
        },
        CommandInfo {
            name: "/ssh-hosts",
            args: "",
            help: "list SSH aliases available for remote sessions",
            busy_ok: true,
        },
        CommandInfo {
            name: "/ssh-add",
            args: "<alias> <absolute-dir>",
            help: "save a discovered SSH alias and default workspace",
            busy_ok: false,
        },
        CommandInfo {
            name: "/ssh",
            args: "<alias> <absolute-dir>",
            help: "save this session and start a new remote SSH workspace",
            busy_ok: false,
        },
        CommandInfo {
            name: "/ssh-saved",
            args: "<alias>",
            help: "open a saved SSH workspace",
            busy_ok: false,
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
            name: "/edit-history",
            args: "[undo|redo|status]",
            help: "undo or redo the focused agent's file edits",
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
        CommandInfo {
            name: "/memory",
            args: "<query>",
            help: "search cited durable memory for this workspace",
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
    let owned = shell_tokens(line)?;
    let toks: Vec<&str> = owned.iter().map(String::as_str).collect();
    let Some((head, rest)) = toks.split_first() else {
        return Err(CmdError::Unknown(String::new()));
    };
    match *head {
        "/quit" | "/exit" => no_extra(rest).map(|()| Command::Quit),
        "/help" => no_extra(rest).map(|()| Command::Help),
        "/status" => no_extra(rest).map(|()| Command::Status),
        "/update-check" => no_extra(rest).map(|()| Command::UpdateCheck),
        "/ssh-hosts" => no_extra(rest).map(|()| Command::SshHosts),
        "/ssh-add" => {
            let Some((alias, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("SSH alias"));
            };
            let Some((directory, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("absolute remote directory"));
            };
            no_extra(rest)?;
            if directory != &"/" && !directory.starts_with('/') {
                return Err(CmdError::BadArg(
                    "remote directory must be absolute (for example /srv/project)".into(),
                ));
            }
            Ok(Command::SshAdd {
                alias: (*alias).to_string(),
                directory: (*directory).to_string(),
            })
        }
        "/ssh" => {
            let Some((alias, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("SSH alias"));
            };
            let Some((directory, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("absolute remote directory"));
            };
            no_extra(rest)?;
            if directory != &"/" && !directory.starts_with('/') {
                return Err(CmdError::BadArg(
                    "remote directory must be absolute (for example /srv/project)".into(),
                ));
            }
            Ok(Command::Ssh {
                alias: (*alias).to_string(),
                directory: (*directory).to_string(),
            })
        }
        "/ssh-saved" => {
            let Some((alias, rest)) = rest.split_first() else {
                return Err(CmdError::MissingArg("SSH alias"));
            };
            no_extra(rest).map(|()| Command::SshSaved {
                alias: (*alias).to_string(),
            })
        }
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
        "/undo" => no_extra(rest).map(|()| Command::EditHistory {
            action: "undo".into(),
        }),
        "/redo" => no_extra(rest).map(|()| Command::EditHistory {
            action: "redo".into(),
        }),
        "/edit-history" => {
            let (action, remaining) = rest
                .split_first()
                .map_or(("status", &rest[..]), |(action, remaining)| {
                    (*action, remaining)
                });
            if !matches!(action, "undo" | "redo" | "status") {
                return Err(CmdError::BadArg(action.to_string()));
            }
            no_extra(remaining).map(|()| Command::EditHistory {
                action: action.into(),
            })
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
        "/onboarding" => no_extra(rest).map(|()| Command::Onboarding),
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
        "/new" => no_extra(rest).map(|()| Command::New),
        "/search" => match rest.split_first() {
            None => Ok(Command::Search { mode: None }),
            Some((mode, rest)) => no_extra(rest).map(|()| Command::Search {
                mode: Some((*mode).to_string()),
            }),
        },
        "/workflow" => parse_workflow(rest),
        "/permissions" => no_extra(rest).map(|()| Command::Permissions),
        "/memory" => {
            if rest.is_empty() {
                return Err(CmdError::MissingArg("memory query"));
            }
            Ok(Command::Memory {
                query: rest.join(" "),
            })
        }
        "/goal" => parse_goal(rest),
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
    fn workflow_commands_parse_with_paths() {
        assert_eq!(
            parse("/workflow"),
            Ok(Command::Workflow {
                action: WorkflowAction::Picker
            })
        );
        assert_eq!(
            parse("/workflow list"),
            Ok(Command::Workflow {
                action: WorkflowAction::List
            })
        );
        assert_eq!(
            parse("/workflow insert \"my workflow.md\""),
            Ok(Command::Workflow {
                action: WorkflowAction::Insert {
                    path: "my workflow.md".into()
                }
            })
        );
        assert_eq!(
            parse("/workflow run flow.md"),
            Ok(Command::Workflow {
                action: WorkflowAction::Run {
                    path: "flow.md".into()
                }
            })
        );
    }

    #[test]
    fn workflow_commands_reject_invalid_arguments() {
        assert_eq!(
            parse("/workflow insert"),
            Err(CmdError::MissingArg("workflow path"))
        );
        assert_eq!(
            parse("/workflow list extra"),
            Err(CmdError::BadArg("extra".into()))
        );
        assert_eq!(
            parse("/workflow unknown"),
            Err(CmdError::BadArg("unknown".into()))
        );
    }

    #[test]
    fn memory_query_is_preserved_as_one_search_request() {
        assert_eq!(
            parse("/memory project database decision"),
            Ok(Command::Memory {
                query: "project database decision".into()
            })
        );
        assert_eq!(parse("/memory"), Err(CmdError::MissingArg("memory query")));
    }

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
    fn goal_accepts_operations() {
        assert_eq!(
            parse("/goal create ship the release"),
            Ok(Command::Goal {
                action: GoalAction::Create {
                    description: "ship the release".into(),
                    check: None,
                    max_steps: None,
                    approval: false,
                    auto_activate: false,
                },
            })
        );
        assert_eq!(
            parse("/goal create ship it --check cargo\\ test\\ -p\\ firmius-service"),
            Ok(Command::Goal {
                action: GoalAction::Create {
                    description: "ship it".into(),
                    check: Some("cargo test -p firmius-service".into()),
                    max_steps: None,
                    approval: false,
                    auto_activate: false,
                },
            })
        );
        assert_eq!(
            parse("/goal list"),
            Ok(Command::Goal {
                action: GoalAction::List,
            })
        );
        assert_eq!(
            parse("/goal status 123"),
            Ok(Command::Goal {
                action: GoalAction::Status {
                    goal_id: "123".into(),
                },
            })
        );
        assert_eq!(
            parse("/goal check 123 tests"),
            Ok(Command::Goal {
                action: GoalAction::Check {
                    goal_id: "123".into(),
                    check_id: "tests".into(),
                },
            })
        );
        assert_eq!(
            parse("/goal cancel 123 no longer needed"),
            Ok(Command::Goal {
                action: GoalAction::Cancel {
                    goal_id: "123".into(),
                    reason: Some("no longer needed".into()),
                },
            })
        );
    }

    #[test]
    fn goal_create_parses_check_and_max_steps() {
        assert_eq!(
            parse("/goal create ship it --check 'cargo test' --max-steps 4 --approval"),
            Ok(Command::Goal {
                action: GoalAction::Create {
                    description: "ship it".into(),
                    check: Some("cargo test".into()),
                    max_steps: Some(4),
                    approval: true,
                    auto_activate: false,
                },
            })
        );
    }

    #[test]
    fn goal_create_rejects_missing_or_invalid_option_values() {
        assert_eq!(
            parse("/goal create ship it --check"),
            Err(CmdError::MissingArg("check"))
        );
        assert_eq!(
            parse("/goal create ship it --check --approval"),
            Err(CmdError::MissingArg("check"))
        );
        assert_eq!(
            parse("/goal create ship it --max-steps"),
            Err(CmdError::MissingArg("max-steps"))
        );
        assert_eq!(
            parse("/goal create ship it --max-steps nope"),
            Err(CmdError::BadArg("nope".into()))
        );
        assert_eq!(
            parse("/goal create ship it --max-steps 0"),
            Err(CmdError::BadArg("0".into()))
        );
        assert_eq!(
            parse("/goal create ship it --check 'cargo test"),
            Err(CmdError::BadArg("unterminated quote".into()))
        );
    }

    #[test]
    fn goal_treats_unknown_words_as_natural_language() {
        assert_eq!(
            parse("/goal frobnicate 123"),
            Ok(Command::Goal {
                action: GoalAction::Create {
                    description: "frobnicate 123".into(),
                    check: None,
                    max_steps: None,
                    approval: false,
                    auto_activate: false,
                }
            })
        );
    }

    #[test]
    fn goal_requires_description_or_operation_arguments() {
        assert_eq!(parse("/goal"), Err(CmdError::MissingArg("goal subcommand")));
        assert_eq!(parse("/goal status"), Err(CmdError::MissingArg("goal id")));
        assert_eq!(
            parse("/goal check id"),
            Err(CmdError::MissingArg("check id"))
        );
        assert_eq!(parse("/goal cancel"), Err(CmdError::MissingArg("goal id")));
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
    fn edit_history_commands_target_the_focused_agent() {
        assert_eq!(
            parse("/undo"),
            Ok(Command::EditHistory {
                action: "undo".into()
            })
        );
        assert_eq!(
            parse("/redo"),
            Ok(Command::EditHistory {
                action: "redo".into()
            })
        );
        assert_eq!(
            parse("/edit-history"),
            Ok(Command::EditHistory {
                action: "status".into()
            })
        );
        assert_eq!(
            parse("/edit-history redo"),
            Ok(Command::EditHistory {
                action: "redo".into()
            })
        );
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
        assert_eq!(
            parse("/sessions"),
            Err(CmdError::Unknown("/sessions".to_string()))
        );
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
        assert_eq!(table().len(), 33);
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
    fn parses_ssh_hosts_without_arguments() {
        assert_eq!(parse("/ssh-hosts"), Ok(Command::SshHosts));
    }

    #[test]
    fn parses_remote_ssh_session_and_rejects_relative_directories() {
        assert_eq!(
            parse("/ssh build /srv/project"),
            Ok(Command::Ssh {
                alias: "build".into(),
                directory: "/srv/project".into(),
            })
        );
        assert!(parse("/ssh build relative").is_err());
        assert!(parse("/ssh build").is_err());
    }

    #[test]
    fn parses_saved_ssh_host_and_rejects_relative_directory() {
        assert_eq!(
            parse("/ssh-add build /srv/project"),
            Ok(Command::SshAdd {
                alias: "build".into(),
                directory: "/srv/project".into(),
            })
        );
        assert!(parse("/ssh-add build relative").is_err());
        assert_eq!(
            parse("/ssh-saved build"),
            Ok(Command::SshSaved {
                alias: "build".into(),
            })
        );
        assert!(parse("/ssh-saved build extra").is_err());
    }

    #[test]
    fn parses_update_check_without_arguments() {
        assert_eq!(parse("/update-check"), Ok(Command::UpdateCheck));
        assert_eq!(
            parse("/update-check extra"),
            Err(CmdError::BadArg("extra".to_string()))
        );
    }

    #[test]
    fn onboarding_reopens_without_arguments() {
        assert_eq!(parse("/onboarding"), Ok(Command::Onboarding));
        assert_eq!(
            parse("/onboarding again"),
            Err(CmdError::BadArg("again".to_string()))
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
    fn export_and_new_parse() {
        assert_eq!(parse("/export"), Ok(Command::Export { path: None }));
        assert_eq!(
            parse("/export notes.md"),
            Ok(Command::Export {
                path: Some("notes.md".to_string())
            })
        );
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
            (Command::SshHosts, true),
            (
                Command::SshAdd {
                    alias: "build".to_string(),
                    directory: "/srv/project".to_string(),
                },
                false,
            ),
            (
                Command::SshSaved {
                    alias: "build".to_string(),
                },
                false,
            ),
            (
                Command::Ssh {
                    alias: "build".to_string(),
                    directory: "/srv/project".to_string(),
                },
                false,
            ),
            (Command::UpdateCheck, true),
            (Command::Save, false),
            (Command::Agents, true),
            (Command::Rewind { turns: 1 }, false),
            (
                Command::EditHistory {
                    action: "undo".into(),
                },
                false,
            ),
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
            (Command::Onboarding, true),
            (
                Command::Mcp {
                    action: McpAction::List,
                },
                true,
            ),
            (Command::Title { title: None }, true),
            (Command::Copy { all: false }, true),
            (Command::Export { path: None }, true),
            (Command::New, false),
            (Command::Search { mode: None }, true),
            (
                Command::Workflow {
                    action: WorkflowAction::List,
                },
                true,
            ),
            (
                Command::Goal {
                    action: GoalAction::List,
                },
                true,
            ),
            (Command::Permissions, true),
            (
                Command::Memory {
                    query: "database decision".to_string(),
                },
                true,
            ),
        ];
        for (cmd, want) in &cases {
            assert_eq!(busy_ok(cmd), *want, "busy_ok for {}", cmd.name());
        }
        // Every variant is covered exactly once, one case per table row.
        assert_eq!(cases.len(), table().len());
    }
}
