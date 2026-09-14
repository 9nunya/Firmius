//! Desktop transcript feature.
use super::*;

fn execution_status_label(status: firmius_core::ExecutionStatus) -> &'static str {
    match status {
        firmius_core::ExecutionStatus::Pending => "Pending",
        firmius_core::ExecutionStatus::Ready => "Ready",
        firmius_core::ExecutionStatus::Running => "Running",
        firmius_core::ExecutionStatus::Succeeded => "Completed",
        firmius_core::ExecutionStatus::Failed => "Failed",
        firmius_core::ExecutionStatus::Blocked => "Blocked",
        firmius_core::ExecutionStatus::Cancelled => "Cancelled",
        firmius_core::ExecutionStatus::Skipped => "Skipped",
        firmius_core::ExecutionStatus::Interrupted => "Interrupted",
    }
}

fn graph_status_label(status: firmius_core::GraphStatus) -> &'static str {
    match status {
        firmius_core::GraphStatus::Active => "Active",
        firmius_core::GraphStatus::Completed => "Completed",
        firmius_core::GraphStatus::Cancelled => "Cancelled",
    }
}

fn verification_label(level: firmius_core::VerificationLevel) -> &'static str {
    match level {
        firmius_core::VerificationLevel::None => "Not verified",
        firmius_core::VerificationLevel::SelfVerified => "Self verified",
        firmius_core::VerificationLevel::Reviewed => "Reviewed",
        firmius_core::VerificationLevel::IndependentlyVerified => "Independently verified",
    }
}

fn block_row(block: crate::blocks::Block) -> BlockRow {
    if block.kind == "code" {
        if let Some((language, rest)) = block.text.split_once('\n') {
            let language = language.trim();
            if !language.is_empty()
                && language.len() <= 24
                && language
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '-' | '_' | '#' | '.'))
            {
                return BlockRow {
                    text: rest.into(),
                    kind: block.kind.into(),
                    label: language.into(),
                };
            }
        }
        return BlockRow {
            text: block.text.into(),
            kind: block.kind.into(),
            label: "Code".into(),
        };
    }
    BlockRow {
        text: block.text.into(),
        kind: block.kind.into(),
        label: String::new().into(),
    }
}

pub(super) fn transcript_row(
    author: impl Into<String>,
    body: impl Into<String>,
    tone: impl Into<String>,
    detail: impl Into<String>,
) -> TranscriptRow {
    let author = author.into();
    let body = body.into();
    let tone = tone.into();
    let detail = detail.into();
    let presenter = if tone == "thinking" { "thinking" } else { "" };
    let blocks = crate::blocks::render(&body, matches!(tone.as_str(), "tool" | "live" | "error"));
    TranscriptRow {
        activity: ActivityData::default(),
        blocks: ModelRc::new(VecModel::from(
            blocks.into_iter().map(block_row).collect::<Vec<_>>(),
        )),
        key: format!("{author}:{detail}").into(),
        author: author.into(),
        body: body.into(),
        presenter: presenter.into(),
        expandable: matches!(tone.as_str(), "tool" | "error" | "live"),
        tone: tone.into(),
        detail: detail.into(),
    }
}

pub(super) fn snapshot_rows(
    snapshot: &SessionSnapshot,
    model: Option<&crate::session_model::SessionModel>,
    focused: Option<&str>,
) -> Vec<TranscriptRow> {
    let mut rows = Vec::new();
    // A busy turn can have durable tool-call parts followed by live reasoning
    // and prose deltas. Keep those tool parts in a tail until the live stream
    // is merged below; otherwise a snapshot refresh makes the tool jump above
    // the assistant passage it belongs to.
    let mut busy_deferred = Vec::new();
    for agent in &snapshot.agents {
        if focused.is_some_and(|id| id != agent.record.id) {
            continue;
        }
        let name = agent.record.label.clone().unwrap_or_else(|| {
            if agent.record.id == snapshot.primary_agent_id {
                "FIRMIUS".into()
            } else {
                "SUBAGENT".into()
            }
        });
        let mut tool_names = HashMap::new();
        // A persisted trajectory can contain tool-only assistant messages
        // split away from the reasoning/text that preceded them. Hold those
        // rows briefly so the viewport follows the semantic turn order:
        // reasoning, prose, then the tool invocation and its runtime state.
        let mut deferred_tools = Vec::new();
        for (message_index, message) in agent.record.history.iter().enumerate() {
            if message.role == MessageRole::System {
                continue;
            }
            if message.role != MessageRole::Assistant && !deferred_tools.is_empty() {
                if agent.busy {
                    busy_deferred.append(&mut deferred_tools);
                } else {
                    rows.append(&mut deferred_tools);
                }
            }
            let mut visible_assistant_passage = false;
            for (part_index, part) in message.content.iter().enumerate() {
                let row_start = rows.len();
                match part {
                    MessagePart::Text(value) if !value.trim().is_empty() => {
                        let generated_correlation = message.correlation.assignment_id.is_some()
                            || message.correlation.workflow_node_id.is_some()
                            || message.correlation.run_id.is_some()
                            || message.correlation.goal_id.is_some();
                        let human = message.role == MessageRole::User
                            && !generated_correlation
                            && message.provenance.as_ref().is_none_or(|p| {
                                matches!(
                                    p.origin,
                                    firmius_core::MessageOrigin::Human
                                        | firmius_core::MessageOrigin::Legacy
                                )
                            });
                        rows.push(transcript_row(
                            if human { "YOU" } else { &name },
                            value.clone(),
                            if human { "user" } else { "assistant" },
                            if value.contains('#')
                                || value.contains("```")
                                || value.contains("[http")
                            {
                                "markdown"
                            } else {
                                "message"
                            },
                        ));
                        visible_assistant_passage |= !human;
                    }
                    MessagePart::Thinking { content, .. } if !content.trim().is_empty() => {
                        rows.push(transcript_row(&name, content, "thinking", "reasoning"));
                        // Reasoning is part of the visible assistant passage;
                        // a preceding tool call must remain below it even
                        // when the provider stores reasoning separately from
                        // the final prose message.
                        visible_assistant_passage = true;
                    }
                    MessagePart::ToolCall {
                        id,
                        name: tool,
                        args,
                    } => {
                        tool_names.insert(id.clone(), tool.clone());
                        if let Some(tool) = model.and_then(|m| {
                            m.tools
                                .iter()
                                .find(|t| t.agent == agent.record.id && t.id == *id)
                        }) {
                            deferred_tools.push(presenters::tool_row(tool));
                            continue;
                        }
                        let (_preview, _detail) = tool_call_preview(tool, args);
                        let mut row = transcript_row(
                            tool.to_uppercase(),
                            "Preparing tool activity…",
                            "tool",
                            "Preparing",
                        );
                        row.key = format!(
                            "{}:message:{message_index}:part:{part_index}",
                            agent.record.id
                        )
                        .into();
                        deferred_tools.push(row);
                    }
                    MessagePart::ToolResult { id, content, ok } => {
                        if model.is_some_and(|m| {
                            m.tools
                                .iter()
                                .any(|t| t.agent == agent.record.id && t.id == *id)
                        }) {
                            continue;
                        }
                        let tool = tool_names
                            .get(id)
                            .cloned()
                            .unwrap_or_else(|| "unknown".into());
                        let mut row = transcript_row(
                            tool.to_uppercase(),
                            content.clone(),
                            if *ok { "tool" } else { "error" },
                            if *ok { "Completed" } else { "Failed" },
                        );
                        row.key = format!(
                            "{}:message:{message_index}:part:{part_index}",
                            agent.record.id
                        )
                        .into();
                        deferred_tools.push(row);
                    }
                    MessagePart::Image(_) => rows.push(transcript_row(
                        &name,
                        "Image attachment",
                        "assistant",
                        "image",
                    )),
                    MessagePart::WebSearch { .. } => rows.push(transcript_row(
                        "WEB SEARCH",
                        "Search result available in the runtime inspector",
                        "tool",
                        "completed",
                    )),
                    _ => {}
                }
                for row in &mut rows[row_start..] {
                    row.key = format!(
                        "{}:message:{message_index}:part:{part_index}",
                        agent.record.id
                    )
                    .into();
                }
            }
            // Flush after the complete assistant message, rather than after
            // its first part. This keeps a tool below both reasoning and
            // prose when a provider stores them as separate parts/messages.
            if message.role == MessageRole::Assistant
                && visible_assistant_passage
                && !deferred_tools.is_empty()
            {
                if agent.busy {
                    busy_deferred.append(&mut deferred_tools);
                } else {
                    rows.append(&mut deferred_tools);
                }
            }
        }
        // Tool-only turns still need to be visible when no following prose
        // exists (for example a permission failure or an interrupted turn).
        if agent.busy {
            busy_deferred.append(&mut deferred_tools);
        } else {
            rows.append(&mut deferred_tools);
        }
    }
    // The daemon event log is a delta stream, not a second transcript. Fold it
    // into stable presenters keyed by agent and tool call so a JSON token or a
    // reasoning token updates its existing surface instead of adding a row.
    let live_rows: Vec<_> = presenters::live_rows_with_sequence(snapshot)
        .into_iter()
        .filter(|(_, r)| focused.is_none_or(|id| r.key.starts_with(&format!("{id}:live:"))))
        .collect();
    if let Some(model) = model {
        // SessionModel is keyed for updates, so its insertion order can differ
        // from the daemon sequence after reconnects. Project runtime tools by
        // their first observed sequence to keep the transcript chronological.
        let mut ordered_tools: Vec<_> = model.tools.iter().collect();
        ordered_tools.sort_by_key(|tool| {
            tool.first_sequence
                .or(tool.last_sequence)
                .unwrap_or(u64::MAX)
        });
        let mut live_ids = std::collections::HashSet::new();
        let mut live_sequences = std::collections::HashSet::new();
        for e in &snapshot.live_events {
            if let firmius_core::SessionEventPayload::Agent { agent_id, event } = &e.payload {
                if let AgentEvent::ToolCallDelta { id, .. }
                | AgentEvent::ToolCallStarted { id, .. }
                | AgentEvent::ToolResult { id, .. } = event
                {
                    if !id.is_empty() {
                        live_ids.insert((agent_id.as_str(), id.as_str()));
                    }
                    live_sequences.insert(e.sequence);
                }
            }
        }
        let mut displayed: std::collections::HashSet<_> =
            rows.iter().map(|r| r.key.to_string()).collect();
        let mut runtime_rows: Vec<(u64, TranscriptRow)> = Vec::new();
        for tool in ordered_tools {
            if focused.is_some_and(|id| id != tool.agent) {
                continue;
            }
            let live = live_ids.contains(&(tool.agent.as_str(), tool.id.as_str()))
                || tool
                    .first_sequence
                    .is_some_and(|seq| live_sequences.contains(&seq));
            if live
                && snapshot.agents.iter().any(|a| a.record.id == tool.agent)
                && displayed.insert(tool.key.clone())
            {
                runtime_rows.push((
                    tool.first_sequence
                        .or(tool.last_sequence)
                        .unwrap_or(u64::MAX),
                    presenters::tool_row(tool),
                ));
            }
        }
        runtime_rows.extend(live_rows);
        runtime_rows.sort_by_key(|(sequence, _)| *sequence);
        rows.extend(runtime_rows.into_iter().map(|(_, row)| row));
    } else {
        rows.extend(live_rows.into_iter().map(|(_, row)| row));
    }
    // Complete the busy-turn tail after the live semantic stream. This is the
    // stable ordering users expect: reasoning/prose, then its tool activity.
    rows.append(&mut busy_deferred);
    if let Some(model) = model {
        let tools: HashMap<_, _> = model.tools.iter().map(|t| (t.key.as_str(), t)).collect();
        for row in &mut rows {
            let Some(tool) = tools.get(row.key.as_str()) else {
                continue;
            };
            let mut resources: Vec<_> = row.activity.resources.iter().collect();
            let mut children = Vec::new();
            for resource in &mut resources {
                if resource.kind == "process" {
                    if let Some((_, bytes)) = model
                        .process_output
                        .get(&(tool.agent.clone(), resource.id.to_string()))
                    {
                        let output = String::from_utf8_lossy(bytes).into_owned();
                        if let Some(notice) = crate::tool_content::omitted_notice(&output) {
                            row.activity.omitted = notice.into();
                        }
                        row.activity.output = output.into();
                    }
                } else if resource.kind == "delegate" {
                    if let Some(agent) = snapshot
                        .agents
                        .iter()
                        .find(|a| a.record.id == resource.agent.as_str())
                    {
                        resource.label = agent
                            .record
                            .label
                            .clone()
                            .unwrap_or_else(|| "Subagent".into())
                            .into();
                    }
                    let mut recent: Vec<_> = model
                        .tools
                        .iter()
                        .rev()
                        .filter(|t| t.agent == resource.agent.as_str())
                        .take(3)
                        .collect();
                    recent.reverse();
                    for child in recent {
                        let preview = presenters::tool_row(child);
                        children.push(ActivityPreview {
                            kind: preview.presenter,
                            summary: preview.activity.summary,
                            state: preview.activity.state,
                            command: preview.activity.command,
                            path: preview.activity.path,
                            query: preview.activity.query,
                            output: preview.activity.output,
                        });
                    }
                }
            }
            row.activity.resources = ModelRc::new(VecModel::from(resources));
            row.activity.children = ModelRc::new(VecModel::from(children));
            if row.presenter == "bash" && !row.activity.output.is_empty() {
                row.activity.terminal =
                    ModelRc::new(VecModel::from(terminal::render(&row.activity.output)));
            }
        }
    }
    rows
}

pub(super) fn rows_for_view(
    snapshot: &SessionSnapshot,
    view: &str,
    model: Option<&crate::session_model::SessionModel>,
    focused: Option<&str>,
) -> Vec<TranscriptRow> {
    let transcript = snapshot_rows(snapshot, model, focused);
    match view {
        "runtime" => {
            let mut rows = vec![transcript_row(
                "SESSION RUNTIME",
                format!(
                    "{} agent(s) · {} active turn(s) · {} active delegate(s)",
                    snapshot.agents.len(),
                    snapshot.active_turns.len(),
                    snapshot.active_delegates
                ),
                "tool",
                "Current session runtime",
            )];
            for agent in &snapshot.agents {
                let label = agent.record.label.clone().unwrap_or_else(|| "Agent".into());
                rows.push(transcript_row(
                    "AGENT",
                    format!(
                        "{label} · {}/{} · input {} · output {}",
                        agent.record.provider_id,
                        agent.record.model,
                        agent.total_usage.input_tokens,
                        agent.total_usage.output_tokens,
                    ),
                    if agent.busy { "tool" } else { "assistant" },
                    if agent.busy { "running" } else { "idle" },
                ));
                for process in &agent.processes {
                    rows.push(transcript_row(
                        "PROCESS",
                        process.cmdline.clone(),
                        "tool",
                        format!(
                            "{} · {} byte(s) captured",
                            match process.status {
                                firmius_core::ProcStatus::Running => "Running",
                                firmius_core::ProcStatus::Exited { success: true, .. } =>
                                    "Exited successfully",
                                firmius_core::ProcStatus::Exited { success: false, .. } =>
                                    "Exited with failure",
                            },
                            process.bytes_captured
                        ),
                    ));
                }
            }
            rows.extend(transcript.into_iter().filter(|row| {
                row.tone == "tool"
                    || row.tone == "error"
                    || row.tone == "thinking"
                    || row.detail == "streaming"
            }));
            rows
        }
        "changes" => model
            .map(|m| {
                m.tools
                    .iter()
                    .filter(|t| t.name == "edit")
                    .map(presenters::tool_row)
                    .collect()
            })
            .unwrap_or_default(),
        "work" => {
            let mut rows = Vec::new();
            for graph in snapshot.work.state.graphs.values() {
                rows.push(transcript_row(
                    "WORK GRAPH",
                    format!(
                        "{} · {} node(s) · {} edge(s)",
                        graph.title,
                        graph.nodes.len(),
                        graph.edges.len()
                    ),
                    "tool",
                    format!(
                        "{} · revision {}",
                        graph_status_label(graph.status),
                        graph.revision
                    ),
                ));
                for node in graph.nodes.values() {
                    rows.push(transcript_row(
                        "TASK",
                        node.title.clone(),
                        if matches!(
                            node.status,
                            firmius_core::ExecutionStatus::Failed
                                | firmius_core::ExecutionStatus::Blocked
                        ) {
                            "error"
                        } else {
                            "tool"
                        },
                        format!(
                            "{} · {} · {} attempt(s)",
                            execution_status_label(node.status),
                            verification_label(node.verification),
                            node.attempt_ids.len()
                        ),
                    ));
                    if !node.acceptance_criteria.is_empty()
                        || !node.file_scope.planned.is_empty()
                        || node.review_policy.requires_independent_reviewer
                    {
                        rows.push(transcript_row(
                            "CRITERIA",
                            node.acceptance_criteria
                                .iter()
                                .map(|criterion| format!("• {}", criterion.text))
                                .chain(
                                    node.file_scope
                                        .planned
                                        .iter()
                                        .map(|file| format!("file: {file}")),
                                )
                                .collect::<Vec<_>>()
                                .join("\n"),
                            "thinking",
                            format!(
                                "{} criterion(s) · {} · {}",
                                node.acceptance_criteria.len(),
                                if node.review_policy.requires_independent_reviewer {
                                    "independent review required"
                                } else {
                                    "review optional"
                                },
                                if node.file_scope.advisory {
                                    "advisory file scope"
                                } else {
                                    "tracked file scope"
                                }
                            ),
                        ));
                    }
                    for attempt in graph
                        .attempts
                        .values()
                        .filter(|attempt| attempt.node_id == node.id)
                    {
                        let result = attempt
                            .result_id
                            .as_ref()
                            .and_then(|result_id| graph.results.get(result_id));
                        rows.push(transcript_row(
                            "ATTEMPT",
                            result
                                .map(|result| result.summary.clone())
                                .unwrap_or_else(|| "No result recorded yet".into()),
                            if matches!(attempt.state, firmius_core::ExecutionStatus::Failed) {
                                "error"
                            } else {
                                "tool"
                            },
                            format!(
                                "Attempt #{} · {} · {} · {}",
                                attempt.number,
                                execution_status_label(attempt.state),
                                result
                                    .and_then(|result| result.outcome.as_ref())
                                    .map(|outcome| match outcome {
                                        firmius_core::work::Outcome::Success =>
                                            "Succeeded".to_string(),
                                        firmius_core::work::Outcome::Failure =>
                                            "Failed".to_string(),
                                        firmius_core::work::Outcome::TestFailed =>
                                            "Tests failed".to_string(),
                                        firmius_core::work::Outcome::Blocked =>
                                            "Blocked".to_string(),
                                        firmius_core::work::Outcome::Cancelled =>
                                            "Cancelled".to_string(),
                                        firmius_core::work::Outcome::Interrupted =>
                                            "Interrupted".to_string(),
                                        firmius_core::work::Outcome::Custom(value) => value.clone(),
                                    })
                                    .unwrap_or_else(|| "No outcome yet".into()),
                                result
                                    .map(|result| verification_label(result.verification))
                                    .unwrap_or("Not verified")
                            ),
                        ));
                    }
                }
                let node_label = |id| {
                    graph
                        .nodes
                        .get(id)
                        .map(|node| node.key.clone())
                        .unwrap_or_else(|| id.to_string())
                };
                for edge in graph.edges.values() {
                    rows.push(transcript_row(
                        "DEPENDENCY",
                        format!("{}  →  {}", node_label(&edge.from), node_label(&edge.to)),
                        "tool",
                        format!(
                            "{} · {} · condition {}{}",
                            if matches!(edge.kind, firmius_core::EdgeKind::Feedback) {
                                "Feedback"
                            } else {
                                "Dependency"
                            },
                            if edge.required {
                                "required"
                            } else {
                                "optional"
                            },
                            match edge.condition {
                                firmius_core::EdgeCondition::Completed => "completed",
                                firmius_core::EdgeCondition::Succeeded => "succeeded",
                                firmius_core::EdgeCondition::Failed => "failed",
                                firmius_core::EdgeCondition::Blocked => "blocked",
                                firmius_core::EdgeCondition::Outcome => "outcome",
                                firmius_core::EdgeCondition::Verification => "verification",
                            },
                            edge.on_outcome
                                .as_ref()
                                .map(|outcome| format!(
                                    " · on {}",
                                    match outcome {
                                        firmius_core::work::Outcome::Success =>
                                            "success".to_string(),
                                        firmius_core::work::Outcome::Failure =>
                                            "failure".to_string(),
                                        firmius_core::work::Outcome::TestFailed =>
                                            "test failure".to_string(),
                                        firmius_core::work::Outcome::Blocked =>
                                            "blocked".to_string(),
                                        firmius_core::work::Outcome::Cancelled =>
                                            "cancelled".to_string(),
                                        firmius_core::work::Outcome::Interrupted =>
                                            "interrupted".to_string(),
                                        firmius_core::work::Outcome::Custom(value) => value.clone(),
                                    }
                                ))
                                .unwrap_or_default()
                        ),
                    ));
                }
            }
            if rows.is_empty() {
                rows.push(transcript_row(
                    "WORK",
                    "No durable work graph is active. Task and workflow tool activity will create inspectable work here.",
                    "assistant",
                    "ready",
                ));
            }
            rows
        }
        _ => transcript,
    }
}

pub(super) fn show_snapshot(
    ui: &MainWindow,
    state: &Arc<Mutex<DesktopState>>,
    snapshot: SessionSnapshot,
) {
    if !state
        .lock()
        .is_ok_and(|mut state| state.accept_snapshot(&snapshot))
    {
        return;
    }
    if let Ok(s) = state.lock() {
        shell_ui::render(ui, &s);
        if s.active_session().as_deref() != Some(snapshot.session_id.as_str()) {
            return;
        }
        ui.set_active_view(s.active_view().into());
        ui.set_focused_agent_id(s.focused_agent().unwrap_or_default().into());
    }
    ui.set_connection_status("Daemon connected".into());
    let busy = !snapshot.active_turns.is_empty();
    let focused_id = {
        let current = ui.get_focused_agent_id().to_string();
        if snapshot
            .agents
            .iter()
            .any(|agent| agent.record.id == current)
        {
            current
        } else {
            snapshot.primary_agent_id.clone()
        }
    };
    ui.set_focused_agent_id(focused_id.clone().into());
    let primary = snapshot
        .agents
        .iter()
        .find(|agent| agent.record.id == focused_id);
    ui.set_active_title(
        snapshot
            .title
            .clone()
            .unwrap_or_else(|| "Untitled session".into())
            .into(),
    );
    ui.set_active_detail(
        format!(
            "{} · persona {} · {} agent{} · {} delegate{} · {}",
            primary
                .map(|agent| agent.record.model.as_str())
                .unwrap_or("model pending"),
            primary
                .and_then(|agent| agent.record.persona.as_deref())
                .unwrap_or("default"),
            snapshot.agents.len(),
            if snapshot.agents.len() == 1 { "" } else { "s" },
            snapshot.active_delegates,
            if snapshot.active_delegates == 1 {
                ""
            } else {
                "s"
            },
            if busy { "working" } else { "ready" },
        )
        .into(),
    );
    ui.set_turn_active(primary.is_some_and(|agent| agent.busy));
    if let Some(agent) = primary {
        let manager = model_catalog();
        let window = manager
            .model_info_for(&agent.record.provider_id, &agent.record.model)
            .map(|info| info.context_window.to_string())
            .unwrap_or_else(|| "unknown".into());
        let input = agent
            .usage
            .input_tokens
            .saturating_add(agent.usage.cache_read_tokens)
            .saturating_add(agent.usage.cache_write_tokens);
        ui.set_focus_status(
            format!(
                "{} · {} · persona {} · CTX last input {} / {}",
                agent.record.label.as_deref().unwrap_or("Lead"),
                agent.record.model,
                agent.record.persona.as_deref().unwrap_or("default"),
                input,
                window
            )
            .into(),
        );
    }
    let work = snapshot
        .work
        .state
        .graphs
        .values()
        .map(|graph| {
            let tasks = graph
                .nodes
                .values()
                .map(|node| format!("{}: {}", node.title, execution_status_label(node.status)))
                .collect::<Vec<_>>()
                .join(" · ");
            format!("{} — {}", graph.title, tasks)
        })
        .collect::<Vec<_>>()
        .join(" | ");
    ui.set_live_status(
        if work.is_empty() {
            format!(
                "{} · {} delegates",
                if busy { "Working" } else { "Ready" },
                snapshot.active_delegates
            )
        } else {
            work
        }
        .into(),
    );
    ui.set_active_provider(
        primary
            .map(|agent| agent.record.provider_id.clone())
            .unwrap_or_default()
            .into(),
    );
    ui.set_active_model(
        primary
            .map(|agent| agent.record.model.clone())
            .unwrap_or_default()
            .into(),
    );
    ui.set_active_effort(
        primary
            .and_then(|agent| agent.record.effort.as_ref())
            .map(|effort| effort.name.clone())
            .unwrap_or_else(|| "Default".into())
            .into(),
    );
    let agents: Vec<_> = snapshot
        .agents
        .iter()
        .map(|agent| AgentRow {
            id: agent.record.id.clone().into(),
            label: agent
                .record
                .label
                .clone()
                .unwrap_or_else(|| {
                    if agent.record.id == snapshot.primary_agent_id {
                        "Lead agent".into()
                    } else {
                        "Subagent".into()
                    }
                })
                .into(),
            detail: format!(
                "{}/{} · {} · persona {} · effort {}",
                agent.record.provider_id,
                agent.record.model,
                if agent.busy { "working" } else { "ready" },
                agent.record.persona.as_deref().unwrap_or("default"),
                agent
                    .record
                    .effort
                    .as_ref()
                    .map(|effort| effort.name.as_str())
                    .unwrap_or("default")
            )
            .into(),
            active: agent.record.id == focused_id,
        })
        .collect();
    ui.set_agents(ModelRc::new(VecModel::from(agents)));
}

pub(super) fn publish_snapshot(
    weak: slint::Weak<MainWindow>,
    state: Arc<Mutex<DesktopState>>,
    snapshot: SessionSnapshot,
) {
    invoke(weak, move |ui| show_snapshot(ui, &state, snapshot));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn language_label_is_split_from_code_body() {
        let row = transcript_row(
            "FIRMIUS",
            "```rust\nlet ok = true;\n```",
            "assistant",
            "markdown",
        );
        let block = row.blocks.row_data(0).unwrap();
        assert_eq!(block.kind, "code");
        assert_eq!(block.label, "rust");
        assert!(block.text.contains("let ok = true;"));
        assert!(!block.text.starts_with("rust"));
    }
}
