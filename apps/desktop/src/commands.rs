//! Desktop action catalog, completion and typed argument validation.
//! This layer contains no Slint or daemon handles.
#[derive(Debug, PartialEq, Eq)]
pub enum Command {
    Route(String),
    Rename(String),
    Export(String),
    Rewind(usize),
    Search(String),
    GoalAction {
        action: String,
        goal_id: String,
        revision: u64,
    },
    WorkflowAction {
        action: String,
        path: String,
    },
}
pub const CATALOG: &[(&str, &str, &str)] = &[
    ("new", "New task", "Choose a model and workspace"),
    (
        "model",
        "Select model",
        "Search provider models and reasoning effort",
    ),
    ("persona", "Change persona", "Focused agent persona"),
    ("title", "Rename task", "title <text>"),
    ("export", "Export Markdown", "export <path>"),
    (
        "rewind",
        "Rewind conversation",
        "rewind <positive turn count>",
    ),
    (
        "search",
        "Hosted search",
        "search off | cached | indexed | live",
    ),
    (
        "memory",
        "Memory Inspector",
        "Search cited user, project, and session memory",
    ),
    ("settings", "Settings", "Runtime preferences and appearance"),
    ("permissions", "Permissions", "Pending decisions"),
    (
        "runtime",
        "Runtime inspector",
        "Agent and process telemetry",
    ),
    (
        "work",
        "Work inspector",
        "Tasks, dependencies and workflows",
    ),
    ("sessions", "All sessions", "Saved conversations"),
    ("save", "Save session", "Persist the current session"),
    ("compact", "Compact context", "Focused agent context"),
    ("edit:undo", "Undo edit", "Focused agent edit history"),
    ("edit:redo", "Redo edit", "Focused agent edit history"),
    ("copy", "Copy reply", "Last assistant reply"),
    ("copy:all", "Copy conversation", "Visible conversation"),
    (
        "mcp:manage",
        "Manage MCP servers",
        "Add, start, stop or remove a server",
    ),
    ("goals", "Inspect goals", "Durable goal state"),
    ("goal:new", "Create goal", "Objective and success condition"),
    (
        "goal:activate",
        "Activate goal",
        "goal:activate <goal-id> <revision>",
    ),
    (
        "goal:cancel",
        "Cancel goal",
        "goal:cancel <goal-id> <revision>",
    ),
    (
        "goal:approve",
        "Approve goal",
        "goal:approve <goal-id> <revision>",
    ),
    (
        "goal:reject",
        "Reject goal",
        "goal:reject <goal-id> <revision>",
    ),
    ("workflows", "Prompt workflows", "Browse workflow files"),
    ("workflow:new", "Build workflow", "Steps and shared brief"),
    (
        "workflow:insert",
        "Insert workflow",
        "workflow:insert <discovered path>",
    ),
    (
        "workflow:run",
        "Run workflow",
        "workflow:run <discovered path>",
    ),
    ("ssh", "Remote workspace", "Saved SSH targets"),
    ("accounts", "Accounts", "Configured providers"),
    ("login", "Add account", "Provider credentials"),
    ("update-check", "Check for updates", "Installed release"),
];
pub fn known(id: &str) -> bool {
    CATALOG.iter().any(|(key, _, _)| *key == id)
}

pub fn complete(query: &str) -> Vec<(&'static str, &'static str, &'static str)> {
    let q = query.trim().trim_start_matches('/').to_lowercase();
    CATALOG
        .iter()
        .copied()
        .filter(|(id, title, hint)| {
            format!("{id} {title} {hint}").to_lowercase().contains(&q)
                || q.starts_with(&format!("{id} "))
        })
        .collect()
}
pub fn parse(input: &str) -> Result<Command, String> {
    let input = input.trim().trim_start_matches('/');
    let (id, args) = input.split_once(char::is_whitespace).unwrap_or((input, ""));
    let args = args.trim();
    if !CATALOG.iter().any(|(key, _, _)| *key == id) {
        return Err(format!("Unknown command: {id}"));
    }
    if args.is_empty() {
        return Ok(Command::Route(id.into()));
    }
    match id {
        "title" => Ok(Command::Rename(args.into())),
        "export" => Ok(Command::Export(args.into())),
        "rewind" => args
            .parse::<usize>()
            .ok()
            .filter(|n| *n > 0)
            .map(Command::Rewind)
            .ok_or("Turn count must be a positive integer".into()),
        "search" if matches!(args, "off" | "cached" | "indexed" | "live") => {
            Ok(Command::Search(args.into()))
        }
        "search" => Err("Search mode: off, cached, indexed or live".into()),
        "goal:activate" | "goal:cancel" | "goal:approve" | "goal:reject" => {
            let mut parts = args.split_whitespace();
            let Some(goal_id) = parts.next().filter(|value| !value.is_empty()) else {
                return Err(format!("{id} requires <goal-id> <current-revision>"));
            };
            let Some(revision) = parts.next().and_then(|value| value.parse::<u64>().ok()) else {
                return Err(format!("{id} requires <goal-id> <current-revision>"));
            };
            if parts.next().is_some() {
                return Err(format!("{id} requires <goal-id> <current-revision>"));
            }
            Ok(Command::GoalAction {
                action: id.trim_start_matches("goal:").into(),
                goal_id: goal_id.into(),
                revision,
            })
        }
        "workflow:insert" | "workflow:run" => Ok(Command::WorkflowAction {
            action: id.trim_start_matches("workflow:").into(),
            path: args.into(),
        }),
        _ => Err(format!(
            "{id} uses a dedicated form; press Enter on its completion"
        )),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn typed_args_preserve_spaces_and_reject_invalid_counts() {
        assert_eq!(
            parse("/title A task with spaces"),
            Ok(Command::Rename("A task with spaces".into()))
        );
        assert_eq!(parse("rewind 12"), Ok(Command::Rewind(12)));
        assert!(parse("rewind 0").is_err());
        assert!(parse("rewind -1").is_err());
        assert!(parse("search made-up").is_err());
    }
    #[test]
    fn completion_matches_labels_and_argument_prefixes() {
        assert!(complete("reasoning").iter().any(|r| r.0 == "model"));
        assert!(complete("memory").iter().any(|r| r.0 == "memory"));
        assert_eq!(complete("rewind 2")[0].0, "rewind");
    }

    #[test]
    fn goal_actions_require_explicit_identity_and_current_revision() {
        let id = "5e1fd186-42f8-4629-b221-995a2a9b27c3";
        assert_eq!(
            parse(&format!("goal:activate {id} 7")),
            Ok(Command::GoalAction {
                action: "activate".into(),
                goal_id: id.into(),
                revision: 7,
            })
        );
        assert!(parse(&format!("goal:cancel {id}")).is_err());
        assert!(parse(&format!("goal:cancel {id} zero")).is_err());
        assert!(parse(&format!("goal:cancel {id} 7 extra")).is_err());
    }

    #[test]
    fn workflow_actions_preserve_explicit_paths_with_spaces() {
        assert_eq!(
            parse("workflow:run /tmp/My Work/release.md"),
            Ok(Command::WorkflowAction {
                action: "run".into(),
                path: "/tmp/My Work/release.md".into(),
            })
        );
        assert!(parse("workflow:run").is_ok_and(|command| {
            matches!(command, Command::Route(id) if id == "workflow:run")
        }));
    }

    #[test]
    fn catalog_ids_are_unique() {
        let mut ids: Vec<_> = CATALOG.iter().map(|entry| entry.0).collect();
        ids.sort_unstable();
        let before = ids.len();
        ids.dedup();
        assert_eq!(ids.len(), before);
    }

    #[test]
    fn catalog_covers_runtime_configuration_surfaces() {
        for id in [
            "goals",
            "goal:new",
            "goal:activate",
            "goal:cancel",
            "goal:approve",
            "goal:reject",
            "workflows",
            "workflow:new",
            "workflow:insert",
            "workflow:run",
            "permissions",
            "settings",
            "accounts",
            "login",
            "mcp:manage",
            "memory",
            "search",
            "ssh",
        ] {
            assert!(known(id), "missing catalog entry: {id}");
            assert_eq!(parse(id), Ok(Command::Route(id.into())));
        }
        assert!(!known("made-up"));
        assert!(parse("made-up").is_err());
        assert!(parse("permissions extra").is_err());
        assert!(complete("goal").iter().any(|row| row.0 == "goal:activate"));
        assert!(
            complete("workflow")
                .iter()
                .any(|row| row.0 == "workflow:run")
        );
        assert!(
            complete("permission")
                .iter()
                .any(|row| row.0 == "permissions")
        );
    }
}
