//! Conservative, model-independent descriptions of tool calls.
//!
//! This module deliberately does not grant permission.  It turns a proposed
//! call into a set of atomic actions which a policy/daemon can approve or
//! reject.  In particular, a compound edit is one decision containing every
//! file operation; callers must not approve only a prefix of the actions.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::{Component, Path};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ActionSeverity {
    NoRisk,
    Scoped,
    Risky,
    Unknown,
}

pub fn classify_tool_action(
    tool: &str,
    args: &Value,
    workdir: Option<&Path>,
) -> ToolActionDescriptor {
    describe_tool_call(tool, args, workdir)
}

/// Compatibility names used by policy adapters.
pub type ActionDescriptor = ToolActionDescriptor;
pub type PermissionActionDescriptor = ToolActionDescriptor;
pub type Action = ToolAction;

/// Whether a policy action kind belongs to Firmius's built-in permission
/// surface. Keep this list beside call classification so adding a native tool
/// cannot silently make confirmed YOLO mode deny it as "unknown".
///
/// `network` is a daemon-owned virtual action used for hosted provider search;
/// `mcp` represents the explicitly configured MCP permission boundary.
pub fn is_known_action_kind(kind: &str) -> bool {
    matches!(
        kind,
        "edit"
            | "edit_file"
            | "bash"
            | "glob"
            | "grep"
            | "read"
            | "list"
            | "message"
            | "delegate"
            | "task"
            | "workflow"
            | "goal"
            | "todo"
            | "memory"
            | "mcp"
            | "network"
    )
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ToolAction {
    pub tool: String,
    pub operation: String,
    /// A short, secret-redacted description suitable for a confirmation UI.
    pub preview: String,
    /// Stable effect labels (for example `read_file` or `spawn_process`).
    pub effects: Vec<String>,
    pub severity: ActionSeverity,
    /// Selector suggestions from broadest to most specific.
    pub suggested_selectors: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ToolActionDescriptor {
    pub tool: String,
    pub operation: String,
    pub actions: Vec<ToolAction>,
    /// Always true.  This is explicit so a broker cannot accidentally grant
    /// one operation in a compound call while denying another.
    pub require_all_actions: bool,
    pub unknown: bool,
}

impl ToolActionDescriptor {
    /// Built-in inspection and session bookkeeping do not cross the
    /// interactive permission boundary. Delegated agents still authorize
    /// their own filesystem, process, and network operations independently.
    pub fn is_permission_exempt(&self) -> bool {
        match self.tool.as_str() {
            "read" | "grep" | "glob" | "list" | "task" | "delegate" | "message" | "goal"
            | "workflow" => true,
            "bash" => matches!(self.operation.as_str(), "poll" | "wait" | "list"),
            _ => false,
        }
    }

    pub fn requires_all_actions(&self) -> bool {
        self.require_all_actions
    }

    pub fn severity(&self) -> ActionSeverity {
        self.actions
            .iter()
            .map(|a| a.severity)
            .max_by_key(|s| match s {
                ActionSeverity::NoRisk => 0,
                ActionSeverity::Scoped => 1,
                ActionSeverity::Risky => 2,
                ActionSeverity::Unknown => 3,
            })
            .unwrap_or(ActionSeverity::Unknown)
    }
}

/// Classify a tool call without executing it.  Unknown tools, including all
/// MCP tools, are intentionally unknown and therefore never no-risk.
pub fn describe_tool_call(
    tool: &str,
    args: &Value,
    workdir: Option<&Path>,
) -> ToolActionDescriptor {
    let name = tool.trim().to_ascii_lowercase();
    let operation = operation_for(&name, args);
    let mut actions = Vec::new();
    let mut unknown = false;
    match name.as_str() {
        "read" | "list" | "glob" | "grep" => {
            let path = string_arg(args, &["path", "cwd", "pattern"]);
            let contained = path
                .as_deref()
                .map(|p| is_contained(workdir, p))
                .unwrap_or(workdir.is_some());
            let severity = if contained {
                ActionSeverity::NoRisk
            } else {
                ActionSeverity::Risky
            };
            actions.push(action(
                &name,
                &operation,
                if contained {
                    "read-only inspection"
                } else {
                    "path is not proven contained"
                },
                vec!["read_data"],
                severity,
            ));
        }
        "edit" => {
            let patch = args.get("patch").and_then(Value::as_str);
            let (ops, valid) = patch.map(parse_patch_actions).unwrap_or_default();
            if ops.is_empty() || !valid {
                actions.push(action(
                    &name,
                    &operation,
                    "invalid or unparsed patch (no partial authorization)",
                    vec!["mutate_files"],
                    ActionSeverity::Unknown,
                ));
                unknown = true;
            } else {
                for (path, effect) in ops {
                    let contained = is_contained(workdir, &path);
                    actions.push(action(
                        &name,
                        &operation,
                        &format!("{effect} {}", redact(&path)),
                        vec!["mutate_files"],
                        if contained {
                            ActionSeverity::Risky
                        } else {
                            ActionSeverity::Unknown
                        },
                    ));
                    unknown |= !contained;
                }
            }
        }
        "bash" => describe_bash(args, &mut actions),
        "delegate" => describe_mode(
            args,
            &name,
            &mut actions,
            &[
                (&["run", "spawn"], ActionSeverity::Risky, "spawn_agent"),
                (&["poll", "wait"], ActionSeverity::Scoped, "inspect_agent"),
                (&["send"], ActionSeverity::Risky, "message_agent"),
            ],
        ),
        "message" => actions.push(action(
            &name,
            &operation,
            "send durable agent message",
            vec!["message_agent"],
            ActionSeverity::Risky,
        )),
        "task" | "workflow" => {
            let read = matches!(
                operation.as_str(),
                "view" | "list" | "poll" | "await" | "wait" | "status" | "quality_digest"
            );
            actions.push(action(
                &name,
                &operation,
                if read {
                    "inspect workflow state"
                } else {
                    "mutate workflow state"
                },
                vec![if read { "read_work" } else { "mutate_work" }],
                if read {
                    ActionSeverity::Scoped
                } else {
                    ActionSeverity::Risky
                },
            ));
        }
        "todo" => {
            let read = operation == "view";
            actions.push(action(
                &name,
                &operation,
                if read {
                    "inspect personal todo state"
                } else {
                    "mutate personal todo state"
                },
                vec![if read { "read_todo" } else { "mutate_todo" }],
                if read {
                    ActionSeverity::Scoped
                } else {
                    ActionSeverity::Risky
                },
            ));
        }
        "memory" => {
            let read = matches!(operation.as_str(), "auto" | "retrieve" | "inspect");
            actions.push(action(
                &name,
                &operation,
                if read {
                    "retrieve or inspect durable memory"
                } else {
                    "mutate durable memory"
                },
                vec![if read { "read_memory" } else { "mutate_memory" }],
                if read {
                    ActionSeverity::Scoped
                } else {
                    ActionSeverity::Risky
                },
            ));
        }
        "goal" => {
            let read = matches!(operation.as_str(), "inspect" | "list");
            actions.push(action(
                &name,
                &operation,
                if read {
                    "inspect goal state"
                } else {
                    "control goal or agent"
                },
                vec![if read { "read_goal" } else { "mutate_goal" }],
                if read {
                    ActionSeverity::Scoped
                } else {
                    ActionSeverity::Risky
                },
            ));
        }
        _ => {
            unknown = true;
            let effects = if name.starts_with("mcp__") || name.starts_with("mcp.") {
                vec!["external_mcp", "unknown_effect"]
            } else {
                vec!["unknown_effect"]
            };
            actions.push(action(
                &name,
                &operation,
                if name.starts_with("mcp__") || name.starts_with("mcp.") {
                    "external MCP behavior is unknown (confirmation required)"
                } else {
                    "unknown tool behavior (confirmation required)"
                },
                effects,
                ActionSeverity::Unknown,
            ));
        }
    }
    ToolActionDescriptor {
        tool: name,
        operation,
        actions,
        require_all_actions: true,
        unknown,
    }
}

/// Alias retained for brokers which call the operation a classification.
pub fn classify_tool_call(
    tool: &str,
    args: &Value,
    workdir: Option<&Path>,
) -> ToolActionDescriptor {
    describe_tool_call(tool, args, workdir)
}

fn action(
    tool: &str,
    operation: &str,
    preview: &str,
    effects: Vec<&str>,
    severity: ActionSeverity,
) -> ToolAction {
    ToolAction {
        tool: tool.into(),
        operation: operation.into(),
        preview: redact(preview),
        effects: effects.into_iter().map(str::to_owned).collect(),
        severity,
        suggested_selectors: vec![tool.into(), format!("{tool}.{operation}")],
    }
}

fn operation_for(tool: &str, args: &Value) -> String {
    let key = match tool {
        "bash" | "delegate" | "task" | "goal" | "workflow" => "mode",
        "todo" => "action",
        "memory" => "mode_hint",
        _ => "operation",
    };
    args.get("action")
        .or_else(|| args.get(key))
        .and_then(Value::as_str)
        .map(canonical)
        .unwrap_or_else(|| {
            match tool {
                "bash" => "exec",
                "delegate" | "workflow" => "run",
                "task" => "view",
                "goal" => "inspect",
                _ => "call",
            }
            .into()
        })
}

fn canonical(s: &str) -> String {
    s.trim().to_ascii_lowercase().replace('-', "_")
}
fn string_arg(args: &Value, keys: &[&str]) -> Option<String> {
    keys.iter()
        .find_map(|k| args.get(*k).and_then(Value::as_str).map(str::to_owned))
}

fn describe_mode(
    args: &Value,
    tool: &str,
    out: &mut Vec<ToolAction>,
    groups: &[(&[&str], ActionSeverity, &str)],
) {
    let op = operation_for(tool, args);
    for (modes, severity, effect) in groups {
        if modes.iter().any(|m| *m == op) {
            out.push(action(tool, &op, effect, vec![effect], *severity));
            return;
        }
    }
    out.push(action(
        tool,
        &op,
        "unknown mode (confirmation required)",
        vec!["unknown_effect"],
        ActionSeverity::Unknown,
    ));
}

fn describe_bash(args: &Value, out: &mut Vec<ToolAction>) {
    let op = operation_for("bash", args);
    let (severity, effect, preview) = match op.as_str() {
        "poll" | "wait" | "list" => (
            ActionSeverity::Scoped,
            "inspect_process",
            "inspect existing process",
        ),
        "input" | "resize" | "kill" => (
            ActionSeverity::Scoped,
            "control_process",
            "control existing process",
        ),
        "exec" | "spawn" => {
            let command = args.get("command").and_then(Value::as_str).unwrap_or("");
            if exact_read_only_command(command) {
                (
                    ActionSeverity::Scoped,
                    "run_read_only_process",
                    "exact read-only command (not workspace-contained)",
                )
            } else {
                (
                    ActionSeverity::Risky,
                    if op == "spawn" {
                        "spawn_process"
                    } else {
                        "run_process"
                    },
                    "run process; workspace containment is not claimed",
                )
            }
        }
        _ => (
            ActionSeverity::Unknown,
            "unknown_effect",
            "unknown bash mode (confirmation required)",
        ),
    };
    out.push(action("bash", &op, preview, vec![effect], severity));
}

fn exact_read_only_command(command: &str) -> bool {
    let c = command.trim();
    !c.is_empty() && !c.chars().any(|ch| ";&|><`$\n\r".contains(ch)) && {
        let mut words = c.split_whitespace();
        matches!(words.next(), Some("true" | "pwd" | "echo" | "printf"))
            && words.all(|w| !w.starts_with('-'))
    }
}

fn is_contained(root: Option<&Path>, path: &str) -> bool {
    if path.starts_with("artifact://") {
        return true;
    }
    let Some(_root) = root else {
        return false;
    };
    let p = Path::new(path);
    // A lexical prefix is not proof of containment: symlinks and platform
    // aliases can escape it. Only the real tool's path checker can establish
    // that, so descriptors stay conservative for absolute paths.
    if p.is_absolute() {
        return false;
    }
    !p.components().any(|c| matches!(c, Component::ParentDir))
}

fn parse_patch_actions(patch: &str) -> (Vec<(String, &'static str)>, bool) {
    let mut result = Vec::new();
    let mut valid = patch
        .lines()
        .next()
        .is_some_and(|line| line.trim() == "*** Begin Patch")
        && patch.lines().any(|line| line.trim() == "*** End Patch");
    let mut current: Option<String> = None;
    for line in patch.lines() {
        let trimmed = line.trim();
        for (header, effect) in [
            ("*** Add File:", "add"),
            ("*** Delete File:", "delete"),
            ("*** Update File:", "update"),
        ] {
            if let Some(path) = trimmed.strip_prefix(header) {
                if !path.trim().is_empty() {
                    result.push((path.trim().to_owned(), effect));
                    current = Some(path.trim().to_owned());
                } else {
                    valid = false;
                }
            }
        }
        if let Some(path) = trimmed.strip_prefix("*** Move to:") {
            if let Some(from) = current.as_deref() {
                if path.trim().is_empty() {
                    valid = false;
                } else {
                    result.push((path.trim().to_owned(), "move"));
                    result.push((from.to_owned(), "move"));
                }
            } else {
                valid = false;
            }
        }
    }
    (result, valid)
}

fn redact(value: &str) -> String {
    let mut out = value.to_owned();
    for key in ["password", "token", "secret", "api_key", "authorization"] {
        let lower = out.to_ascii_lowercase();
        if let Some(pos) = lower.find(key) {
            if let Some(eq) = out[pos..].find('=') {
                out.replace_range(pos + eq + 1.., "<redacted>");
            }
        }
    }
    if out.len() > 240 {
        out.truncate(240);
        out.push('…');
    }
    out
}

#[cfg(test)]
mod tests {
    #[test]
    fn public_actions_are_classified_instead_of_falling_back_to_defaults() {
        let status = super::describe_tool_call(
            "workflow",
            &serde_json::json!({"action":"status","run_id":"r"}),
            None,
        );
        assert_eq!(status.operation, "status");
        assert!(!status.unknown);
        assert!(status.is_permission_exempt());
        let spawn =
            super::describe_tool_call("delegate", &serde_json::json!({"action":"spawn"}), None);
        assert_eq!(spawn.operation, "spawn");

        for (tool, args, expected_operation) in [
            ("todo", serde_json::json!({"action":"view"}), "view"),
            (
                "memory",
                serde_json::json!({"mode_hint":"candidate","prompt":"Propose a fact"}),
                "candidate",
            ),
        ] {
            let descriptor = super::describe_tool_call(tool, &args, None);
            assert_eq!(descriptor.operation, expected_operation);
            assert!(!descriptor.unknown, "{tool} must not be unknown");
        }
    }
    use super::*;
    use serde_json::json;
    #[test]
    fn compound_edits_are_atomic() {
        let d = describe_tool_call(
            "edit",
            &json!({"patch":"*** Begin Patch\n*** Update File: a\n*** Update File: b\n*** End Patch"}),
            Some(Path::new("/tmp")),
        );
        assert!(d.requires_all_actions());
        assert_eq!(d.actions.len(), 2);
    }
    #[test]
    fn bash_does_not_claim_containment() {
        let d = describe_tool_call("bash", &json!({"command":"pwd"}), Some(Path::new("/tmp")));
        assert!(d.actions[0].preview.contains("not workspace-contained"));
    }
    #[test]
    fn unknown_mcp_is_closed() {
        let d = describe_tool_call("mcp__s__x", &json!({}), None);
        assert!(d.unknown);
        assert_eq!(d.severity(), ActionSeverity::Unknown);
    }
    #[test]
    fn traversal_is_not_no_risk() {
        let d = describe_tool_call(
            "read",
            &json!({"path":"../secret"}),
            Some(Path::new("/tmp")),
        );
        assert_ne!(d.severity(), ActionSeverity::NoRisk);
    }
}
