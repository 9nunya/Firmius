//! Pure presentation transforms shared by transcript and inspector views.
//!
//! These helpers deliberately avoid Slint and daemon handles so they can be
//! tested independently from UI wiring.

#[cfg(test)]
pub(crate) fn tool_args_preview(args: &str) -> String {
    let trimmed = args.trim();
    if let Ok(value) = serde_json::from_str::<serde_json::Value>(trimmed)
        && let Ok(pretty) = serde_json::to_string_pretty(&value)
    {
        return pretty;
    }
    if trimmed.is_empty() {
        "No argument details yet".into()
    } else {
        "Argument details are still arriving…".into()
    }
}

pub(crate) fn tool_call_preview(name: &str, args: &str) -> (String, String) {
    let Ok(value) = serde_json::from_str::<serde_json::Value>(args) else {
        // The provider often streams an incomplete object for several frames.
        // PartialJson still exposes completed string fields, which lets the
        // presenter become useful immediately without showing the raw chunk.
        let partial = firmius_core::partial_json::PartialJson::parse(args);
        let field = |key: &str| partial.str(key).unwrap_or("");
        let first = |keys: &[&str], fallback: &str| {
            keys.iter()
                .map(|key| field(key))
                .find(|value| !value.is_empty())
                .unwrap_or(fallback)
                .to_owned()
        };
        let (body, detail) = match name {
            "bash" => (
                first(&["intent", "command"], "Preparing shell command…"),
                format!(
                    "bash · mode {} · cwd {}",
                    first(&["mode"], "exec"),
                    first(&["cwd"], "workspace")
                ),
            ),
            "edit" | "undo" => (
                first(&["path", "file"], "Preparing file edit…"),
                format!("{name} · receiving patch details"),
            ),
            "delegate" => (
                first(&["intent", "prompt", "task"], "Preparing delegation…"),
                format!("delegate · persona {}", first(&["persona"], "default")),
            ),
            "task" | "workflow" => (
                first(&["title", "description", "task"], "Preparing workflow…"),
                format!("{name} · action {}", first(&["action"], "starting")),
            ),
            "goal" => (
                first(&["description", "title", "goal_id"], "Preparing goal…"),
                format!("goal · {}", first(&["action"], "inspect")),
            ),
            "todo" => (
                first(&["title", "intent", "outcome"], "Preparing todo…"),
                format!("todo · {}", first(&["action"], "view")),
            ),
            "memory" => (
                first(&["prompt", "query"], "Preparing memory request…"),
                format!("memory · {}", first(&["scope_hint"], "session")),
            ),
            "message" => (
                first(&["message", "label"], "Preparing message…"),
                format!("message · {}", first(&["target"], "parent")),
            ),
            "artifact" => (
                first(&["path", "name", "title"], "Preparing artifact…"),
                format!("artifact · {}", first(&["action"], "inspect")),
            ),
            "read" | "list" => (
                first(&["path"], "Preparing file lookup…"),
                format!("{name} · receiving path"),
            ),
            "grep" | "glob" => (
                first(&["pattern"], "Preparing search…"),
                format!("{name} · receiving pattern"),
            ),
            name if name == "mcp" || name.starts_with("mcp__") || name.starts_with("mcp.") => (
                first(&["name", "query", "action"], "Preparing MCP call…"),
                format!("{name} · receiving details"),
            ),
            _ => (
                if name.is_empty() {
                    "Receiving tool details…".into()
                } else {
                    format!("Preparing {name}…")
                },
                format!("{name} · receiving details"),
            ),
        };
        return (body, detail);
    };
    let text = |key: &str| value.get(key).and_then(serde_json::Value::as_str);
    match name {
        "delegate" => (
            text("prompt")
                .or_else(|| text("task"))
                .unwrap_or("Delegated assignment")
                .to_string(),
            format!(
                "delegate · persona {} · {}",
                text("persona").unwrap_or("default"),
                text("workdir").unwrap_or("current workspace")
            ),
        ),
        "task" => (
            text("title")
                .or_else(|| text("description"))
                .unwrap_or("Task operation")
                .to_string(),
            format!(
                "task · {}",
                match text("action").unwrap_or("inspect") {
                    "create" => "creating task",
                    "update" => "updating task",
                    "complete" => "completing task",
                    _ => "inspecting task",
                }
            ),
        ),
        "workflow" => (
            text("title").unwrap_or("Workflow operation").to_string(),
            format!(
                "workflow · {}",
                match text("action").unwrap_or("run") {
                    "create" => "creating workflow",
                    "update" => "updating workflow",
                    "complete" => "workflow complete",
                    _ => "running workflow",
                }
            ),
        ),
        "goal" => (
            text("description")
                .or_else(|| text("title"))
                .unwrap_or("Goal operation")
                .to_string(),
            format!(
                "goal · {}",
                match text("action").unwrap_or("inspect") {
                    "propose" => "proposing goal",
                    "create_child" => "creating child goal",
                    "submit_candidate" => "submitting candidate",
                    "request_activation" => "requesting activation",
                    "cancel" => "cancelling goal",
                    "complete" => "goal complete",
                    other => other,
                }
            ),
        ),
        "todo" => (
            text("title")
                .or_else(|| text("intent"))
                .unwrap_or("Todo operation")
                .to_string(),
            format!("todo · {}", text("action").unwrap_or("view")),
        ),
        "memory" => (
            text("prompt")
                .or_else(|| text("query"))
                .unwrap_or("Memory request")
                .to_string(),
            format!("memory · {}", text("scope_hint").unwrap_or("session")),
        ),
        "message" => (
            text("message").unwrap_or("Outbound message").to_string(),
            format!("message · {}", text("target").unwrap_or("parent")),
        ),
        "artifact" => (
            text("path")
                .or_else(|| text("name"))
                .unwrap_or("Artifact")
                .to_string(),
            format!("artifact · {}", text("action").unwrap_or("inspect")),
        ),
        "bash" => (
            text("command").unwrap_or("Shell command").to_string(),
            format!(
                "bash · mode {} · cwd {}",
                text("mode").unwrap_or("exec"),
                text("cwd").unwrap_or("workspace")
            ),
        ),
        "edit" | "undo" => (
            text("path")
                .or_else(|| text("file"))
                .unwrap_or("File edit")
                .to_string(),
            format!("{name} · file operation"),
        ),
        "mcp" => (
            text("name").unwrap_or("MCP server operation").to_string(),
            format!("mcp · action {}", text("action").unwrap_or("inspect")),
        ),
        name if name.starts_with("mcp__") || name.starts_with("mcp.") => {
            let parts: Vec<_> = name.split("__").collect();
            let label = match (parts.get(1), parts.get(2)) {
                (Some(server), Some(tool)) => format!("{server} · {tool}"),
                _ => name.to_string(),
            };
            (
                text("query")
                    .or_else(|| text("name"))
                    .unwrap_or(&label)
                    .to_string(),
                format!("mcp · {label}"),
            )
        }
        _ => (
            if name.is_empty() {
                "Preparing tool…".to_string()
            } else {
                format!("Preparing {name}…")
            },
            format!("{name} · receiving details"),
        ),
    }
}

#[cfg(test)]
pub(crate) fn markdown_preview(input: &str) -> String {
    let mut in_code = false;
    let mut out = Vec::new();
    for line in input.lines() {
        let trimmed = line.trim_start();
        if trimmed.starts_with("```") {
            in_code = !in_code;
            out.push(if in_code {
                "┌─ code".to_string()
            } else {
                "└─ code".to_string()
            });
            continue;
        }
        if in_code {
            out.push(format!("│ {line}"));
            continue;
        }
        let mut rendered = if let Some(heading) = trimmed.strip_prefix("### ") {
            format!("▌ {heading}")
        } else if let Some(heading) = trimmed.strip_prefix("## ") {
            format!("▌ {heading}")
        } else if let Some(heading) = trimmed.strip_prefix("# ") {
            format!("▌ {heading}")
        } else if let Some(item) = trimmed
            .strip_prefix("- ")
            .or_else(|| trimmed.strip_prefix("* "))
        {
            format!("• {item}")
        } else if let Some(quote) = trimmed.strip_prefix("> ") {
            format!("│ {quote}")
        } else {
            line.to_string()
        };
        rendered = rendered.replace("**", "").replace("__", "");
        while let Some(start) = rendered.find('[') {
            let Some(mid) = rendered[start..].find("](") else {
                break;
            };
            let mid = start + mid;
            let Some(end) = rendered[mid + 2..].find(')') else {
                break;
            };
            let end = mid + 2 + end;
            let label = rendered[start + 1..mid].to_string();
            let url = rendered[mid + 2..end].to_string();
            rendered.replace_range(start..=end, &format!("{label} ↗ {url}"));
        }
        out.push(rendered);
    }
    out.join("\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn specialized_previews_avoid_raw_json() {
        let (body, detail) = tool_call_preview(
            "goal",
            r#"{"action":"submit_candidate","description":"Ship desktop"}"#,
        );
        assert_eq!(body, "Ship desktop");
        assert!(detail.contains("submitting candidate"));
        let (body, detail) =
            tool_call_preview("memory", r#"{"prompt":"layout","scope_hint":"project"}"#);
        assert_eq!(body, "layout");
        assert!(detail.contains("project"));
        let (body, _) = tool_call_preview("mcp__docs__search", r#"{"query":"presenters"}"#);
        assert_eq!(body, "presenters");
        let (body, detail) = tool_call_preview("artifact", r#"{"path":"artifact://report"}"#);
        assert_eq!(body, "artifact://report");
        assert!(detail.contains("inspect"));
    }

    #[test]
    fn markdown_preview_keeps_headings_and_code() {
        let rendered = markdown_preview("# Title\n\n```rs\nlet x = 1;\n```");
        assert!(rendered.contains("▌ Title"));
        assert!(rendered.contains("┌─ code"));
        assert!(rendered.contains("│ let x = 1;"));
    }
}

pub(crate) fn diff_preview(body: &str) -> (String, String) {
    let Ok(value) = serde_json::from_str::<serde_json::Value>(body) else {
        // Preparing payloads are frequently incomplete JSON. Never surface
        // the transport fragment; the edit presenter can still communicate
        // that a patch is arriving without exposing protocol syntax.
        return ("Receiving patch details…".into(), "edit in progress".into());
    };
    let path = value
        .get("path")
        .or_else(|| value.get("file"))
        .and_then(serde_json::Value::as_str)
        .unwrap_or("edited file")
        .to_string();
    if let Some(patch) = value
        .get("patch")
        .or_else(|| value.get("diff"))
        .and_then(serde_json::Value::as_str)
    {
        return (patch.to_string(), path);
    }
    let old = value.get("old").and_then(serde_json::Value::as_str);
    let new = value.get("new").and_then(serde_json::Value::as_str);
    if let (Some(old), Some(new)) = (old, new) {
        let mut lines = vec![format!("--- {path}"), format!("+++ {path}")];
        for line in old.lines() {
            lines.push(format!("- {line}"));
        }
        for line in new.lines() {
            lines.push(format!("+ {line}"));
        }
        return (lines.join("\n"), path);
    }
    ("Patch details are still arriving…".into(), path)
}

pub(crate) fn workflow_prompt(title: &str, brief: &str, steps: &[String]) -> String {
    let mut prompt =
        format!("Use the workflow tool to create and run a durable workflow titled \"{title}\".\n");
    if !brief.trim().is_empty() {
        prompt.push_str(&format!(
            "Shared brief and quality bar:\n{}\n\n",
            brief.trim()
        ));
    }
    prompt.push_str("Create these ordered steps:\n");
    for (index, step) in steps.iter().enumerate() {
        prompt.push_str(&format!("{}. {}\n", index + 1, step));
    }
    prompt
}
