//! Explicit tool presentation selected by tool identity, never author text.
use super::*;
use crate::session_model::{Phase, Tool};
pub(super) fn kind(name: &str) -> &'static str {
    match name {
        "bash" => "bash",
        "edit" | "undo" => "edit",
        "delegate" => "delegate",
        "task" | "workflow" => "work",
        "goal" => "goal",
        "todo" => "todo",
        "memory" => "memory",
        "message" => "message",
        "artifact" => "artifact",
        "read" | "grep" | "glob" | "list" => "query",
        name if name == "mcp" || name.starts_with("mcp__") || name.starts_with("mcp.") => "mcp",
        _ => "tool",
    }
}

/// Convert structured tool results into language the visual presenter can
/// explain. Raw JSON remains in the session model for export/debugging, but
/// never becomes the default transcript body.
fn semantic_output(name: &str, output: &str) -> String {
    let trimmed = output.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    let Ok(value) = serde_json::from_str::<serde_json::Value>(trimmed) else {
        return output.to_owned();
    };
    if let Some(text) = value.as_str() {
        return text.to_owned();
    }
    if let Some(content) = value.get("content").and_then(serde_json::Value::as_array) {
        let texts: Vec<_> = content
            .iter()
            .filter_map(|item| item.get("text").and_then(serde_json::Value::as_str))
            .filter(|text| !text.trim().is_empty())
            .collect();
        if !texts.is_empty() {
            return texts.join("\n");
        }
    }
    let preferred = if name == "mcp" || name.starts_with("mcp__") || name.starts_with("mcp.") {
        ["status", "message", "error", "summary"].as_slice()
    } else {
        match name {
            "bash" => ["stdout", "output", "message", "error"].as_slice(),
            "edit" | "undo" => ["summary", "message", "error"].as_slice(),
            "goal" => ["status", "summary", "message", "error"].as_slice(),
            "todo" => ["outcome", "status", "summary", "message", "error"].as_slice(),
            "memory" => ["summary", "report", "status", "message", "error"].as_slice(),
            "message" => ["delivery", "status", "message", "error"].as_slice(),
            "artifact" => ["path", "summary", "message", "error"].as_slice(),
            "delegate" | "task" | "workflow" => {
                ["summary", "message", "status", "error"].as_slice()
            }
            _ => ["content", "text", "message", "summary", "error"].as_slice(),
        }
    };
    for key in preferred {
        if let Some(text) = value.get(*key).and_then(serde_json::Value::as_str) {
            if !text.trim().is_empty() {
                return text.to_owned();
            }
        }
    }
    if value.get("ok").and_then(serde_json::Value::as_bool) == Some(true) {
        return "Completed successfully".into();
    }
    if value.get("ok").and_then(serde_json::Value::as_bool) == Some(false) {
        return "The operation reported a failure".into();
    }
    "Structured result available".into()
}

fn semantic_summary(name: &str, field: &dyn Fn(&str) -> String, fallback: String) -> String {
    let first = |keys: &[&str]| {
        keys.iter()
            .map(|key| field(key))
            .find(|value| !value.is_empty())
            .unwrap_or_default()
    };
    match name {
        "goal" => first(&["description", "title", "goal_id"]),
        "todo" => first(&["title", "intent", "outcome"]),
        "memory" => first(&["prompt", "query", "scope_hint"]),
        "message" => first(&["message", "target", "label"]),
        "mcp" => first(&["name", "action"]),
        "artifact" => first(&["path", "name", "title"]),
        // Registry names already encode server · tool. Query/action belong in
        // intent/detail so they do not replace the specialized title.
        _ if name.starts_with("mcp__") || name.starts_with("mcp.") => String::new(),
        _ => String::new(),
    }
    .lines()
    .next()
    .filter(|value| !value.is_empty())
    .unwrap_or(&fallback)
    .to_owned()
}

fn semantic_intent(name: &str, field: &dyn Fn(&str) -> String, original: String) -> String {
    let action = field("action");
    let target = match name {
        "message" => field("target"),
        "memory" => field("scope_hint"),
        "mcp" => field("name"),
        "goal" => field("goal_id"),
        "todo" => field("item_id"),
        "artifact" => field("path"),
        name if name.starts_with("mcp__") || name.starts_with("mcp.") => field("name"),
        _ => String::new(),
    };
    if action.is_empty() && target.is_empty() {
        return original;
    }
    match (action.is_empty(), target.is_empty()) {
        (false, false) => format!("{action} · {target}"),
        (false, true) => action,
        (true, false) => target,
        (true, true) => original,
    }
}

pub(super) fn tool_row(tool: &Tool) -> TranscriptRow {
    let args = serde_json::from_str::<serde_json::Value>(&tool.args).ok();
    let partial = firmius_core::partial_json::PartialJson::parse(&tool.args);
    let text = |key: &str| partial.str(key).unwrap_or("").to_owned();
    // Tool names and arguments arrive independently. Infer a presenter from
    // the first semantic field so a preparing call immediately gets the same
    // visual treatment as its completed runtime state.
    let inferred_name = if !tool.name.is_empty() {
        tool.name.clone()
    } else if !text("command").is_empty() {
        "bash".into()
    } else if !text("patch").is_empty() || !text("old").is_empty() {
        "edit".into()
    } else if !text("pattern").is_empty() {
        "grep".into()
    } else if !text("goal_id").is_empty() || !text("success_conditions").is_empty() {
        "goal".into()
    } else if !text("item_id").is_empty() || !text("completion_criteria").is_empty() {
        "todo".into()
    } else if !text("scope_hint").is_empty() {
        "memory".into()
    } else if !text("target").is_empty() && !text("message").is_empty() {
        "message".into()
    } else if !text("prompt").is_empty() || !text("task").is_empty() {
        "delegate".into()
    } else if !text("path").is_empty() {
        "read".into()
    } else {
        String::new()
    };
    let resources: Vec<_> = tool
        .resources
        .values()
        .map(|resource| match resource {
            firmius_core::ToolRuntimeResource::Process { id, mode, status } => {
                let (state, active, failed) = match status {
                    firmius_core::ProcStatus::Running => ("Process running".into(), true, false),
                    firmius_core::ProcStatus::Exited { code, success } => {
                        (format!("Exited {code}"), false, !success)
                    }
                };
                ResourceRow {
                    id: id.to_string().into(),
                    kind: "process".into(),
                    label: "Process".into(),
                    mode: mode.clone().into(),
                    state: state.into(),
                    active,
                    failed,
                    agent: "".into(),
                }
            }
            firmius_core::ToolRuntimeResource::Delegate {
                id,
                agent_id,
                mode,
                finished,
                ok,
            } => ResourceRow {
                id: id.to_string().into(),
                kind: "delegate".into(),
                label: "Subagent".into(),
                mode: mode.clone().into(),
                state: (if !finished {
                    "Agent working"
                } else if *ok == Some(false) {
                    "Agent failed"
                } else {
                    "Agent finished"
                })
                .into(),
                active: !finished,
                failed: *ok == Some(false),
                agent: agent_id.clone().into(),
            },
        })
        .collect();
    let invocation = match tool.phase {
        Phase::Preparing => "Preparing",
        Phase::Running => "Running",
        Phase::Completed => "Completed",
        Phase::Failed => "Failed",
        Phase::Interrupted => "Interrupted",
    };
    let prominent = resources
        .iter()
        .find(|r| r.failed)
        .or_else(|| resources.iter().find(|r| r.active))
        .or_else(|| resources.last());
    let status = prominent
        .map(|r| r.state.to_string())
        .unwrap_or_else(|| invocation.into());
    let path = text("path");
    let mut command = text("command");
    if let Some(extra) = args
        .as_ref()
        .and_then(|value| value.get("args"))
        .and_then(serde_json::Value::as_array)
    {
        for argument in extra {
            if let Some(value) = argument.as_str() {
                if !command.is_empty() {
                    command.push(' ');
                }
                command.push_str(value);
            }
        }
    }
    let intent = text("intent");
    let query = if !text("pattern").is_empty() {
        text("pattern")
    } else {
        text("query")
    };
    let brief = if !text("prompt").is_empty() {
        text("prompt")
    } else {
        text("message")
    };
    let mut summary = match inferred_name.as_str() {
        "bash" if !intent.is_empty() => format!("Intent · {intent}"),
        "bash" if !command.is_empty() => command.clone(),
        "edit" if !path.is_empty() => format!("Edit {path}"),
        "read" if !path.is_empty() => format!("Read {path}"),
        "list" if !path.is_empty() => format!("List {path}"),
        "grep" | "glob" if !query.is_empty() => format!(
            "{} {query}",
            if inferred_name == "grep" {
                "Search"
            } else {
                "Find"
            }
        ),
        "delegate" if !intent.is_empty() => intent.clone(),
        "delegate" if !brief.is_empty() => brief.lines().next().unwrap_or_default().to_owned(),
        "task" | "workflow" | "goal" | "todo" => {
            let title = text("title");
            let description = text("description");
            let task = text("task");
            if !title.is_empty() {
                title
            } else if !description.is_empty() {
                description
            } else if !task.is_empty() {
                task
            } else {
                tool_call_preview(&inferred_name, &tool.args).0
            }
        }
        name if name.starts_with("mcp__") => {
            let parts: Vec<_> = inferred_name.split("__").collect();
            match (parts.get(1), parts.get(2)) {
                (Some(server), Some(tool)) => format!("{server} · {tool}"),
                _ => inferred_name.clone(),
            }
        }
        _ if matches!(tool.phase, Phase::Preparing) => format!(
            "Preparing {}…",
            if inferred_name.is_empty() {
                "tool"
            } else {
                &inferred_name
            }
        ),
        _ => inferred_name.clone(),
    };
    summary = semantic_summary(&inferred_name, &text, summary);
    let intent = semantic_intent(&inferred_name, &text, intent);
    let raw_output = tool.result.clone().unwrap_or_default();
    let output = semantic_output(&inferred_name, &raw_output);
    let patch = if inferred_name == "edit" {
        let streamed = text("patch");
        let streamed = if streamed.is_empty() {
            text("diff")
        } else {
            streamed
        };
        if streamed.is_empty() && args.is_some() {
            crate::presentation::diff_preview(&tool.args).0
        } else {
            streamed
        }
    } else {
        String::new()
    };
    let files: Vec<_> = crate::tool_content::patch_files(&patch)
        .into_iter()
        .map(|file| FileChangeRow {
            path: file.path.into(),
            operation: file.operation.into(),
            added: file.added.min(i32::MAX as usize) as i32,
            removed: file.removed.min(i32::MAX as usize) as i32,
            lines: ModelRc::new(VecModel::from(
                file.lines
                    .into_iter()
                    .map(|line| CodeLine {
                        text: line.text.into(),
                        kind: line.kind.into(),
                        old_number: line.old.into(),
                        new_number: line.new.into(),
                    })
                    .collect::<Vec<_>>(),
            )),
        })
        .collect();
    if inferred_name == "edit" && !files.is_empty() {
        let added: i32 = files.iter().map(|f| f.added).sum();
        let removed: i32 = files.iter().map(|f| f.removed).sum();
        summary = if files.len() == 1 {
            format!("{}  +{added} −{removed}", files[0].path)
        } else {
            format!("{} files  +{added} −{removed}", files.len())
        };
    }
    if inferred_name == "delegate" && !text("label").is_empty() {
        summary = text("label");
    }
    let matches = if inferred_name == "grep" {
        crate::tool_content::search_matches(&raw_output)
    } else if matches!(inferred_name.as_str(), "glob" | "list") {
        crate::tool_content::listed_paths(&raw_output)
    } else {
        vec![]
    };
    let matches: Vec<_> = matches
        .into_iter()
        .map(|m| MatchRow {
            path: m.path.into(),
            line: m.line.min(i32::MAX as usize) as i32,
            text: m.text.into(),
        })
        .collect();
    let first_line = args
        .as_ref()
        .and_then(|v| v["start_line"].as_u64())
        .unwrap_or(1);
    let lines: Vec<_> = if inferred_name == "read" {
        output
            .lines()
            .enumerate()
            .map(|(index, line)| CodeLine {
                text: line.into(),
                kind: "context".into(),
                old_number: String::new().into(),
                new_number: (first_line + index as u64).to_string().into(),
            })
            .collect()
    } else {
        vec![]
    };
    let (_, safe_detail) = crate::presentation::tool_call_preview(&inferred_name, &tool.args);
    let mut row = transcript_row(
        &inferred_name,
        format!("{summary}\n{output}"),
        if resources.iter().any(|r| r.failed)
            || matches!(tool.phase, Phase::Failed | Phase::Interrupted)
        {
            "error"
        } else {
            "tool"
        },
        &status,
    );
    row.activity = ActivityData {
        runtime_active: resources.iter().any(|r| r.active),
        terminal: Default::default(),
        children: Default::default(),
        cwd: text("cwd").into(),
        tool_name: inferred_name.clone().into(),
        files: ModelRc::new(VecModel::from(files)),
        matches: ModelRc::new(VecModel::from(matches)),
        lines: ModelRc::new(VecModel::from(lines)),
        summary: summary.into(),
        intent: intent.into(),
        state: status.into(),
        invocation: invocation.into(),
        command: command.into(),
        path: path.into(),
        query: query.into(),
        brief: brief.into(),
        mode: text("mode").into(),
        // Keep raw argument chunks in the daemon/session model only. The UI
        // receives a semantic state so partial JSON never leaks to users.
        arguments: safe_detail.into(),
        output: output.into(),
        patch: patch.into(),
        omitted: crate::tool_content::omitted_notice(&raw_output)
            .unwrap_or_default()
            .into(),
        resources: ModelRc::new(VecModel::from(resources)),
    };
    row.key = tool.key.clone().into();
    row.presenter = kind(&inferred_name).into();
    row
}
/// Preserve the first event sequence so live prose/reasoning can be merged
/// with runtime tool rows in transport order.
pub(super) fn live_rows_with_sequence(snapshot: &SessionSnapshot) -> Vec<(u64, TranscriptRow)> {
    let mut agents: std::collections::BTreeMap<String, (String, String, Option<u64>, Option<u64>)> =
        Default::default();
    for envelope in &snapshot.live_events {
        let firmius_core::SessionEventPayload::Agent { agent_id, event } = &envelope.payload else {
            continue;
        };
        let entry = agents.entry(agent_id.clone()).or_default();
        match event {
            AgentEvent::Text(delta) => {
                entry.2.get_or_insert(envelope.sequence);
                entry.0.push_str(delta);
            }
            AgentEvent::Thinking(delta) => {
                entry.3.get_or_insert(envelope.sequence);
                entry.1.push_str(delta);
            }
            AgentEvent::TurnFinished => {
                entry.0.clear();
                entry.1.clear();
                entry.2 = None;
                entry.3 = None;
            }
            _ => {}
        }
    }
    let mut rows = Vec::new();
    for (id, (text, thinking, text_start, thinking_start)) in agents {
        for (body, tone, sequence) in [
            (thinking, "thinking", thinking_start),
            (text, "assistant", text_start),
        ] {
            if body.is_empty() {
                continue;
            }
            let mut row = transcript_row("FIRMIUS", body, tone, "streaming");
            // A retained event window may evict its first delta during a long
            // turn. Identity belongs to the agent/stream kind, not that moving
            // boundary, or Slint destroys and recreates the visible row.
            row.key = format!("{id}:live:{tone}").into();
            rows.push((sequence.unwrap_or(u64::MAX), row));
        }
    }
    rows
}

#[cfg(test)]
mod tests {
    use super::*;
    fn tool() -> Tool {
        Tool {
            key: "lead:tool:1".into(),
            agent: "lead".into(),
            id: "call".into(),
            name: "bash".into(),
            args: r#"{"command":"cargo test","mode":"spawn"}"#.into(),
            phase: Phase::Completed,
            result: Some("spawned".into()),
            first_sequence: Some(1),
            last_sequence: Some(4),
            resources: Default::default(),
        }
    }
    #[test]
    fn successful_spawn_keeps_running_resource_and_later_failure_visible() {
        let mut tool = tool();
        tool.resources.insert(
            "process:1".into(),
            firmius_core::ToolRuntimeResource::Process {
                id: "00000000-0000-0000-0000-000000000001".parse().unwrap(),
                mode: "spawn".into(),
                status: firmius_core::ProcStatus::Running,
            },
        );
        let row = tool_row(&tool);
        assert_eq!(row.activity.summary, "cargo test");
        assert_eq!(row.activity.invocation, "Completed");
        assert_eq!(row.activity.state, "Process running");
        assert_eq!(row.activity.resources.row_count(), 1);
        tool.resources.insert(
            "process:1".into(),
            firmius_core::ToolRuntimeResource::Process {
                id: "00000000-0000-0000-0000-000000000001".parse().unwrap(),
                mode: "spawn".into(),
                status: firmius_core::ProcStatus::Exited {
                    code: 1,
                    success: false,
                },
            },
        );
        let row = tool_row(&tool);
        assert_eq!(row.activity.invocation, "Completed");
        assert_eq!(row.activity.state, "Exited 1");
        assert_eq!(row.tone, "error");
    }
    #[test]
    fn preparing_fields_stream_into_specialized_presenters() {
        let mut tool = tool();
        tool.phase = Phase::Preparing;
        for (name, args, expected) in [
            ("bash", r#"{"command":"cargo te"#, "cargo te"),
            ("read", r#"{"path":"src/ma"#, "Read src/ma"),
            ("grep", r#"{"pattern":"Session"#, "Search Session"),
            (
                "delegate",
                r#"{"prompt":"Review the layout"#,
                "Review the layout",
            ),
        ] {
            tool.name = name.into();
            tool.args = args.into();
            let row = tool_row(&tool);
            assert_eq!(row.activity.summary, expected);
            assert_eq!(row.activity.invocation, "Preparing");
            assert_eq!(row.key, tool.key);
        }
    }
    #[test]
    fn preparing_arguments_do_not_become_an_error_or_raw_json_summary() {
        let mut tool = tool();
        tool.phase = Phase::Preparing;
        tool.args = "{\"comm".into();
        let row = tool_row(&tool);
        assert_eq!(row.activity.summary, "Preparing bash…");
        assert_eq!(row.activity.arguments, "bash · mode exec · cwd workspace");
        assert_eq!(row.key, tool.key);
        assert_ne!(row.tone, "error");
    }

    #[test]
    fn structured_results_are_projected_into_human_output() {
        let mut tool = tool();
        tool.result = Some(r#"{"ok":true,"run_id":"internal-id"}"#.into());
        let row = tool_row(&tool);
        assert_eq!(row.activity.output, "Completed successfully");
        assert!(!row.activity.output.contains("internal-id"));
    }

    #[test]
    fn partial_semantic_fields_select_a_presenter_before_name_arrives() {
        let mut tool = tool();
        tool.name.clear();
        tool.args = r#"{"command":"cargo test"}"#.into();
        tool.phase = Phase::Preparing;
        let row = tool_row(&tool);
        assert_eq!(row.presenter, "bash");
        assert_eq!(row.activity.command, "cargo test");
        assert_eq!(row.activity.arguments, "bash · mode exec · cwd workspace");
    }

    #[test]
    fn control_plane_tools_have_specialized_semantic_state() {
        let mut tool = tool();
        for (name, args, summary, intent) in [
            (
                "goal",
                r#"{"action":"submit_candidate","description":"Ship desktop","goal_id":"g-1"}"#,
                "Ship desktop",
                "submit_candidate · g-1",
            ),
            (
                "todo",
                r#"{"action":"complete","title":"Run tests","item_id":"i-1"}"#,
                "Run tests",
                "complete · i-1",
            ),
            (
                "memory",
                r#"{"prompt":"Recall layout decisions","scope_hint":"project"}"#,
                "Recall layout decisions",
                "project",
            ),
            (
                "message",
                r#"{"target":"parent","message":"Presenter contract ready"}"#,
                "Presenter contract ready",
                "parent",
            ),
            (
                "mcp",
                r#"{"action":"restart","name":"docs"}"#,
                "docs",
                "restart · docs",
            ),
        ] {
            tool.name = name.into();
            tool.args = args.into();
            let row = tool_row(&tool);
            assert_eq!(row.presenter, name, "{name}");
            assert_eq!(row.activity.summary, summary, "{name}");
            assert_eq!(row.activity.intent, intent, "{name}");
            assert!(
                !row.activity.output.contains('{'),
                "{name} leaked structured JSON into the default body"
            );
        }
    }

    #[test]
    fn mcp_registry_names_select_the_mcp_presenter() {
        let mut tool = tool();
        tool.name = "mcp__docs__search".into();
        tool.args = r#"{"query":"presenter contract"}"#.into();
        tool.result = Some(r#"{"content":[{"type":"text","text":"12 matches"}]}"#.into());
        let row = tool_row(&tool);
        assert_eq!(row.presenter, "mcp");
        assert_eq!(row.activity.summary, "docs · search");
        assert_eq!(row.activity.output, "12 matches");
    }

    #[test]
    fn truncation_notice_is_projected_instead_of_hidden() {
        let mut tool = tool();
        tool.name = "grep".into();
        tool.args = r#"{"pattern":"Presenter"}"#.into();
        tool.result = Some("src/a.rs:4: Presenter\n[...truncated at 200 matches...]".into());
        let row = tool_row(&tool);
        assert_eq!(row.presenter, "query");
        assert!(row.activity.omitted.contains("truncated"));
        assert_eq!(row.activity.matches.row_count(), 1);
    }

    #[test]
    fn bash_args_join_the_visible_command() {
        let mut tool = tool();
        tool.args = r#"{"command":"cargo","args":["test","-p","desktop"]}"#.into();
        let row = tool_row(&tool);
        assert_eq!(row.activity.command, "cargo test -p desktop");
        assert_eq!(row.activity.summary, "cargo test -p desktop");
    }

    #[test]
    fn glob_list_and_artifact_project_specialized_state() {
        let mut tool = tool();
        tool.name = "glob".into();
        tool.args = r#"{"pattern":"*.rs"}"#.into();
        tool.result = Some("src/a.rs\nsrc/b.rs\n[...truncated at 500 matches...]".into());
        let row = tool_row(&tool);
        assert_eq!(row.presenter, "query");
        assert_eq!(row.activity.matches.row_count(), 2);
        assert_eq!(row.activity.matches.row_data(0).unwrap().line, 0);
        assert!(row.activity.omitted.contains("truncated"));

        tool.name = "list".into();
        tool.args = r#"{"path":"src"}"#.into();
        tool.result = Some("src/a.rs\nempty\n[...truncated at 200 matches...]".into());
        let row = tool_row(&tool);
        assert_eq!(row.presenter, "query");
        assert_eq!(row.activity.matches.row_count(), 1);
        assert!(row.activity.omitted.contains("truncated"));

        tool.name = "artifact".into();
        tool.args = r#"{"path":"artifact://report","action":"write"}"#.into();
        tool.result = Some(r#"{"path":"artifact://report","ok":true}"#.into());
        let row = tool_row(&tool);
        assert_eq!(row.presenter, "artifact");
        assert_eq!(row.activity.summary, "artifact://report");
        assert_eq!(row.activity.intent, "write · artifact://report");
        assert_eq!(row.activity.output, "artifact://report");
    }

    fn event(sequence: u64, event: AgentEvent) -> firmius_core::SessionEvent {
        firmius_core::SessionEvent {
            session_id: "s".into(),
            sequence,
            at: chrono::Utc::now(),
            payload: firmius_core::SessionEventPayload::Agent {
                agent_id: "lead".into(),
                event,
            },
        }
    }

    #[test]
    fn live_text_and_thinking_keep_their_first_sequence() {
        let snapshot = SessionSnapshot {
            session_id: "s".into(),
            title: None,
            sequence: 7,
            primary_agent_id: "lead".into(),
            agents: vec![],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new("s", 7, Default::default()),
            active_turns: Default::default(),
            active_delegates: 0,
            live_events: vec![
                event(4, AgentEvent::Thinking("why".into())),
                event(7, AgentEvent::Text("answer".into())),
            ],
        };
        let rows = live_rows_with_sequence(&snapshot);
        assert!(
            rows.iter()
                .any(|(sequence, row)| *sequence == 4 && row.tone == "thinking")
        );
        assert!(
            rows.iter()
                .any(|(sequence, row)| *sequence == 7 && row.tone == "assistant")
        );
    }
}
