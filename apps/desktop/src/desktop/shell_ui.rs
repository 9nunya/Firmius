//! Adapter from shell routes to viewport data. This is the single rendering
//! entry point: feature code opens a route or publishes a portable document.
use super::*;
use crate::shell::Tab;

fn context_display(provider: &str, model: &str, used: u64) -> (String, String) {
    // Catalog loading touches disk. Cache capacities outside the streaming path.
    thread_local! {
        static CAPACITIES: std::cell::RefCell<HashMap<(String, String), u64>> = Default::default();
    }
    let capacity = CAPACITIES.with(|cache| {
        *cache
            .borrow_mut()
            .entry((provider.into(), model.into()))
            .or_insert_with(|| {
                model_catalog()
                    .model_info_for(provider, model)
                    .map(|m| m.context_window as u64)
                    .unwrap_or(0)
            })
    });
    if capacity == 0 {
        return (String::new(), "Context usage unavailable".into());
    }
    let fraction = (used as f64 / capacity as f64).clamp(0.0, 1.0);
    let path = if fraction <= 0.0 {
        String::new()
    } else if fraction >= 1.0 {
        "M12 2 A10 10 0 1 1 12 22 A10 10 0 1 1 12 2".into()
    } else {
        let angle = fraction * std::f64::consts::TAU;
        format!(
            "M12 2 A10 10 0 {} 1 {} {}",
            u8::from(fraction > 0.5),
            12.0 + 10.0 * angle.sin(),
            12.0 - 10.0 * angle.cos()
        )
    };
    (
        path,
        format!(
            "Context: {used} / {capacity} tokens ({:.0}%)",
            fraction * 100.0
        ),
    )
}

fn cached_efforts(provider: &str, model: &str) -> Vec<EffortOption> {
    thread_local! {
        static OPTIONS: std::cell::RefCell<HashMap<(String, String), Vec<EffortOption>>> = Default::default();
    }
    OPTIONS.with(|cache| {
        cache
            .borrow_mut()
            .entry((provider.into(), model.into()))
            .or_insert_with(|| effort_options(provider, model))
            .clone()
    })
}

fn live_phrase(
    snapshot: &SessionSnapshot,
    model: Option<&crate::session_model::SessionModel>,
    agent: &str,
) -> &'static str {
    let choice = (snapshot.sequence / 64) as usize;
    let pick = |phrases: &'static [&'static str]| phrases[choice % phrases.len()];
    if let Some(event) = snapshot
        .live_events
        .iter()
        .rev()
        .find_map(|e| match &e.payload {
            firmius_core::SessionEventPayload::Agent { agent_id, event } if agent_id == agent => {
                Some(event)
            }
            _ => None,
        })
    {
        match event {
            AgentEvent::Text(_) => {
                return pick(&[
                    "pouring words into the answer…",
                    "churning the work into words…",
                    "streaming a tiny essay…",
                    "ink is still flowing…",
                ]);
            }
            AgentEvent::Thinking(_) => {
                return pick(&[
                    "counting thought-beans…",
                    "untangling the noodle logic…",
                    "asking the brain gremlins…",
                ]);
            }
            _ => {}
        }
    }
    if let Some(tool) = model.and_then(|m| {
        m.tools.iter().rev().find(|t| {
            t.agent == agent
                && (matches!(
                    t.phase,
                    crate::session_model::Phase::Preparing | crate::session_model::Phase::Running
                ) || t.resources.values().any(|r| match r {
                    firmius_core::ToolRuntimeResource::Process { status, .. } => {
                        matches!(status, firmius_core::ProcStatus::Running)
                    }
                    firmius_core::ToolRuntimeResource::Delegate { finished, .. } => !finished,
                }))
        })
    }) {
        return match tool.name.as_str() {
            "edit" => pick(&[
                "warming up the patch scissors…",
                "lining up the diff confetti…",
                "prepping a tasteful edit…",
            ]),
            "task" | "workflow" => pick(&[
                "packing the task backpack…",
                "sorting the checklist beans…",
                "setting the workgraph dominoes…",
            ]),
            "delegate" => pick(&[
                "waiting for the delegate dragon…",
                "the subagent is brewing…",
                "letting a helper hatch…",
            ]),
            "bash" | "process" => pick(&[
                "watching the process do its thing…",
                "tailing the shell-shaped comet…",
                "waiting for bash to spill the beans…",
            ]),
            _ => pick(&[
                "letting the tool do a little dance…",
                "turning the tool crank…",
                "waiting on the helpful gadget…",
            ]),
        };
    }
    if snapshot.work.state.graphs.values().any(|g| {
        g.owner_agent_id.as_deref() == Some(agent)
            && g.nodes
                .values()
                .any(|n| n.status == firmius_core::ExecutionStatus::Running)
    }) {
        return pick(&[
            "watching the durable dominoes…",
            "waiting for the workgraph to blink…",
            "keeping the checklist kettle warm…",
        ]);
    }
    pick(&[
        "counting thought-beans…",
        "untangling the noodle logic…",
        "asking the brain gremlins…",
    ])
}

fn work_graphs(snapshot: &SessionSnapshot, agent: &str) -> Vec<WorkGraphRow> {
    use firmius_core::ExecutionStatus as Status;
    let graphs = &snapshot.work.state.graphs;
    let assignments = graphs
        .values()
        .flat_map(|g| {
            g.assignments.values().filter_map(move |a| {
                (a.agent_id == agent && a.released_at.is_none())
                    .then(|| (a, g.nodes.get(&a.node_id)))
                    .and_then(|(a, n)| n.map(|n| (a, n)))
            })
        })
        .collect::<Vec<_>>();
    let mut rows = graphs
        .values()
        .filter(|g| {
            g.owner_agent_id
                .as_deref()
                .unwrap_or(&snapshot.primary_agent_id)
                == agent
        })
        .map(|graph| {
            let mut ordered = graph
                .view_order
                .iter()
                .filter_map(|id| graph.nodes.get(id))
                .collect::<Vec<_>>();
            for node in graph.nodes.values() {
                if !ordered.iter().any(|n| n.id == node.id) {
                    ordered.push(node);
                }
            }
            let nodes = ordered
                .iter()
                .map(|node| WorkNodeRow {
                    id: node.id.to_string().into(),
                    title: node.title.clone().into(),
                    detail: node.description.clone().unwrap_or_default().into(),
                    state: match node.status {
                        Status::Pending => "Pending",
                        Status::Ready => "Ready",
                        Status::Running => "Running",
                        Status::Succeeded => "Done",
                        Status::Failed => "Failed",
                        Status::Blocked => "Blocked",
                        Status::Cancelled => "Cancelled",
                        Status::Skipped => "Skipped",
                        Status::Interrupted => "Interrupted",
                    }
                    .into(),
                    active: node.status == Status::Running,
                    failed: matches!(
                        node.status,
                        Status::Failed | Status::Blocked | Status::Interrupted
                    ),
                    complete: node.status == Status::Succeeded,
                })
                .collect::<Vec<_>>();
            let edges = graph
                .edges
                .values()
                .filter_map(|edge| {
                    Some(WorkEdgeRow {
                        source: ordered.iter().position(|n| n.id == edge.from)? as i32,
                        target: ordered.iter().position(|n| n.id == edge.to)? as i32,
                    })
                })
                .collect::<Vec<_>>();
            WorkGraphRow {
                id: graph.id.to_string().into(),
                title: graph.title.clone().into(),
                assigned: graph
                    .parent_assignment_id
                    .as_ref()
                    .and_then(|id| assignments.iter().find(|(a, _)| &a.id == id))
                    .map(|(_, n)| n.title.clone())
                    .unwrap_or_default()
                    .into(),
                completed: nodes.iter().filter(|n| n.complete).count() as i32,
                nodes: ModelRc::new(VecModel::from(nodes)),
                edges: ModelRc::new(VecModel::from(edges)),
            }
        })
        .collect::<Vec<_>>();
    for (assignment, node) in assignments {
        if !graphs.values().any(|g| {
            g.owner_agent_id.as_deref() == Some(agent)
                && g.parent_assignment_id.as_ref() == Some(&assignment.id)
        }) {
            rows.push(WorkGraphRow {
                id: format!("assignment:{}", assignment.id).into(),
                title: node.title.clone().into(),
                assigned: node.title.clone().into(),
                ..Default::default()
            });
        }
    }
    rows
}

fn agent_todos(snapshot: &SessionSnapshot, agent: &str) -> Vec<WorkGraphRow> {
    let mut rows = Vec::new();
    let graphs = &snapshot.work.state.graphs;
    if let Some(todo) = snapshot
        .agents
        .iter()
        .find(|a| a.record.id == agent)
        .and_then(|a| a.todo.as_ref())
    {
        use firmius_protocol::TodoStatusDto;
        let assigned = graphs
            .values()
            .flat_map(|g| {
                g.assignments.values().filter_map(|a| {
                    (a.agent_id == agent && a.released_at.is_none())
                        .then(|| g.nodes.get(&a.node_id))
                        .flatten()
                })
            })
            .next()
            .map(|n| n.title.clone())
            .unwrap_or_default();
        let nodes = todo
            .items
            .iter()
            .map(|item| WorkNodeRow {
                id: item.id.clone().into(),
                title: item.title.clone().into(),
                detail: "".into(),
                state: match item.status {
                    TodoStatusDto::Pending => "Pending",
                    TodoStatusDto::InProgress => "Running",
                    TodoStatusDto::Blocked => "Blocked",
                    TodoStatusDto::Completed => "Done",
                    TodoStatusDto::Cancelled => "Cancelled",
                }
                .into(),
                active: item.status == TodoStatusDto::InProgress,
                failed: item.status == TodoStatusDto::Blocked,
                complete: item.status == TodoStatusDto::Completed,
            })
            .collect::<Vec<_>>();
        rows.push(WorkGraphRow {
            id: format!("todo:{agent}").into(),
            title: "Checklist".into(),
            assigned: assigned.into(),
            completed: nodes.iter().filter(|n| n.complete).count() as i32,
            nodes: ModelRc::new(VecModel::from(nodes)),
            edges: Default::default(),
        });
    }
    rows
}

fn project_tab(state: &DesktopState, tab: &Tab) -> Vec<TranscriptRow> {
    if let View::Feature(name) | View::Output(name) = &tab.route.view {
        return state
            .documents
            .get(&(tab.route.session.clone(), name.clone()))
            .map(|rows| {
                rows.iter()
                    .enumerate()
                    .map(|(index, content)| {
                        let mut row = transcript_row(
                            &content.author,
                            &content.body,
                            &content.tone,
                            &content.detail,
                        );
                        row.key = format!("{}:{index}", tab.id).into();
                        row
                    })
                    .collect()
            })
            .unwrap_or_default();
    }
    let Some(snapshot) = tab
        .route
        .session
        .as_ref()
        .and_then(|id| state.snapshots.get(id))
    else {
        return vec![];
    };
    let focused = (tab.route.view == View::Conversation).then(|| {
        tab.route
            .agent
            .as_deref()
            .unwrap_or(&snapshot.primary_agent_id)
    });
    let model = state.models.get(&snapshot.session_id);
    rows_for_view(snapshot, tab.route.view.name(), model, focused)
}

/// Patch only split geometry so live resize cannot lag behind the handle.
pub(super) fn apply_live_geometry(ui: &MainWindow, state: &DesktopState) {
    let (mut regions, mut separators) = (Vec::new(), Vec::new());
    state.shell.layout.regions(
        crate::layout::Rect::default(),
        &mut regions,
        &mut separators,
    );
    let panes = ui.get_viewports();
    for (index, pane) in panes.iter().enumerate() {
        if let Some((_, bounds)) = regions
            .iter()
            .find(|(id, _)| id.as_str() == pane.id.as_str())
        {
            if pane.x != bounds.x
                || pane.y != bounds.y
                || pane.width != bounds.width
                || pane.height != bounds.height
            {
                if let Some(mut row) = panes.row_data(index) {
                    row.x = bounds.x;
                    row.y = bounds.y;
                    row.width = bounds.width;
                    row.height = bounds.height;
                    panes.set_row_data(index, row);
                }
            }
        }
    }
    let handles = ui.get_split_handles();
    for (index, handle) in handles.iter().enumerate() {
        if let Some(separator) = separators.iter().find(|item| item.id == handle.id.as_str()) {
            if handle.ratio != separator.ratio
                || handle.x != separator.bounds.x
                || handle.y != separator.bounds.y
                || handle.width != separator.bounds.width
                || handle.height != separator.bounds.height
            {
                if let Some(mut row) = handles.row_data(index) {
                    row.ratio = separator.ratio;
                    row.x = separator.bounds.x;
                    row.y = separator.bounds.y;
                    row.width = separator.bounds.width;
                    row.height = separator.bounds.height;
                    handles.set_row_data(index, row);
                }
            }
        }
    }
}

pub(super) fn render(ui: &MainWindow, state: &DesktopState) {
    let (mut regions, mut separators) = (Vec::new(), Vec::new());
    state.shell.layout.regions(
        crate::layout::Rect::default(),
        &mut regions,
        &mut separators,
    );
    let previous = ui.get_viewports();
    // A restored layout can briefly reference a viewport that no longer
    // exists (for example after closing a tab while the sidebar is being
    // collapsed). Keep one deterministic focused pane so compact mode never
    // hides every viewport and leaves an empty canvas.
    let focused_id = state
        .shell
        .viewports
        .iter()
        .find(|pane| pane.id == state.shell.focused)
        .map(|pane| pane.id.as_str())
        .or_else(|| state.shell.viewports.first().map(|pane| pane.id.as_str()));
    let mut panes = Vec::new();
    for pane in &state.shell.viewports {
        let tab = pane
            .tabs
            .iter()
            .find(|t| Some(&t.id) == pane.selected.as_ref());
        let old = previous.iter().find(|p| p.id == pane.id);
        let rows = tab.map(|t| project_tab(state, t)).unwrap_or_default();
        let rows = ui_models::patch_rows(
            &old.as_ref().map(|p| p.rows.clone()).unwrap_or_default(),
            rows,
        );
        if Some(pane.id.as_str()) == focused_id {
            if ui.get_transcript() != rows {
                ui.set_transcript(rows.clone());
            }
        }
        let tabs = pane
            .tabs
            .iter()
            .map(|t| TabRow {
                id: t.id.clone().into(),
                title: if t.route.view == View::Conversation {
                    t.route
                        .session
                        .as_ref()
                        .and_then(|id| state.snapshots.get(id))
                        .and_then(|s| s.title.clone())
                        .unwrap_or_else(|| t.title.clone())
                } else if t.title.eq_ignore_ascii_case(t.route.view.name()) {
                    t.title.clone()
                } else {
                    format!(
                        "{} · {}",
                        t.title.chars().take(24).collect::<String>(),
                        t.route.view.name()
                    )
                }
                .into(),
                selected: Some(&t.id) == pane.selected.as_ref(),
            })
            .collect::<Vec<_>>();
        let snapshot = tab
            .and_then(|t| t.route.session.as_ref())
            .and_then(|id| state.snapshots.get(id));
        let agent = snapshot.and_then(|snapshot| {
            let id = tab
                .and_then(|t| t.route.agent.as_deref())
                .unwrap_or(&snapshot.primary_agent_id);
            snapshot.agents.iter().find(|a| a.record.id == id)
        });
        let bounds = regions
            .iter()
            .find(|(id, _)| id == &pane.id)
            .map(|(_, bounds)| *bounds)
            .unwrap_or_default();
        let (context_path, context_label) = agent
            .map(|a| {
                context_display(
                    &a.record.provider_id,
                    &a.record.model,
                    a.usage
                        .input_tokens
                        .saturating_add(a.usage.cache_read_tokens)
                        .saturating_add(a.usage.cache_write_tokens) as u64,
                )
            })
            .unwrap_or_default();
        let agents = snapshot
            .map(|snapshot| {
                fn visit(
                    snapshot: &SessionSnapshot,
                    parent: Option<&str>,
                    depth: usize,
                    focused: &str,
                    seen: &mut std::collections::HashSet<String>,
                    rows: &mut Vec<AgentRow>,
                ) {
                    for a in &snapshot.agents {
                        if snapshot
                            .hierarchy
                            .get(&a.record.id)
                            .and_then(|n| n.parent_id.as_deref())
                            != parent
                            || !seen.insert(a.record.id.clone())
                        {
                            continue;
                        }
                        rows.push(AgentRow {
                            id: a.record.id.clone().into(),
                            label: format!(
                                "{}{}",
                                "    ".repeat(depth),
                                a.record.label.as_deref().unwrap_or(if depth == 0 {
                                    "Lead"
                                } else {
                                    "Agent"
                                })
                            )
                            .into(),
                            detail: a
                                .record
                                .persona
                                .clone()
                                .unwrap_or_else(|| {
                                    if depth == 0 {
                                        "Lead".into()
                                    } else {
                                        "Subagent".into()
                                    }
                                })
                                .into(),
                            active: a.record.id == focused,
                        });
                        visit(snapshot, Some(&a.record.id), depth + 1, focused, seen, rows);
                    }
                }
                let mut rows = Vec::new();
                visit(
                    snapshot,
                    None,
                    0,
                    agent.map(|a| a.record.id.as_str()).unwrap_or(""),
                    &mut Default::default(),
                    &mut rows,
                );
                rows
            })
            .unwrap_or_default();
        let previous_graphs = old.as_ref().map(|p| p.graphs.clone()).unwrap_or_default();
        let previous_todos = old.as_ref().map(|p| p.todos.clone()).unwrap_or_default();
        let mut graphs = snapshot
            .map(|snapshot| {
                work_graphs(
                    snapshot,
                    agent
                        .map(|a| a.record.id.as_str())
                        .unwrap_or(&snapshot.primary_agent_id),
                )
            })
            .unwrap_or_default();
        for graph in &mut graphs {
            if let Some(previous) = previous_graphs.iter().find(|g| g.id == graph.id) {
                graph.nodes =
                    ui_models::patch(&previous.nodes, graph.nodes.iter().collect(), |n| {
                        n.id.clone()
                    });
                graph.edges =
                    ui_models::patch(&previous.edges, graph.edges.iter().collect(), |e| {
                        format!("{}:{}", e.source, e.target).into()
                    });
            }
        }
        let mut todos = snapshot
            .map(|snapshot| {
                agent_todos(
                    snapshot,
                    agent
                        .map(|a| a.record.id.as_str())
                        .unwrap_or(&snapshot.primary_agent_id),
                )
            })
            .unwrap_or_default();
        for todo in &mut todos {
            if let Some(previous) = previous_todos.iter().find(|g| g.id == todo.id) {
                todo.nodes = ui_models::patch(&previous.nodes, todo.nodes.iter().collect(), |n| {
                    n.id.clone()
                });
            }
        }
        panes.push(ViewportRow {
            workflow_open: tab.is_some_and(|t| t.workflow_open),
            todos: ui_models::patch(&previous_todos, todos, |g| g.id.clone()),
            welcome: tab.is_none()
                || tab.is_some_and(|t| {
                    t.route.view == View::Conversation && t.route.session.is_none()
                }),
            workspace: agent
                .map(|a| a.record.workdir.to_string_lossy().into_owned())
                .or_else(|| tab.map(|t| t.composer.workspace.clone()))
                .unwrap_or_default()
                .into(),
            live_phrase: snapshot
                .map(|snapshot| {
                    live_phrase(
                        snapshot,
                        state.models.get(&snapshot.session_id),
                        agent
                            .map(|a| a.record.id.as_str())
                            .unwrap_or(&snapshot.primary_agent_id),
                    )
                })
                .unwrap_or("counting thought-beans…")
                .into(),
            permission_mode: tab
                .and_then(|tab| tab.route.session.as_ref())
                .and_then(|session| state.permission_policies.get(session))
                .map(|policy| match &policy.mode {
                    firmius_core::PermissionMode::Default => "Default".to_owned(),
                    firmius_core::PermissionMode::Auto => "Auto".to_owned(),
                    firmius_core::PermissionMode::Yolo => "YOLO".to_owned(),
                    firmius_core::PermissionMode::Custom(name) => name.clone(),
                })
                .unwrap_or_else(|| "Default".into())
                .into(),
            provider: agent
                .map(|a| a.record.provider_id.clone())
                .or_else(|| tab.map(|t| t.composer.provider.clone()))
                .unwrap_or_default()
                .into(),
            effort: agent
                .and_then(|a| a.record.effort.as_ref().map(|e| e.name.clone()))
                .or_else(|| tab.map(|t| t.composer.effort.clone()))
                .unwrap_or_default()
                .into(),
            efforts: {
                let provider = agent
                    .map(|a| a.record.provider_id.as_str())
                    .or_else(|| tab.map(|t| t.composer.provider.as_str()))
                    .unwrap_or("");
                let model = agent
                    .map(|a| a.record.model.as_str())
                    .or_else(|| tab.map(|t| t.composer.model.as_str()))
                    .unwrap_or("");
                ui_models::patch(
                    &old.as_ref().map(|p| p.efforts.clone()).unwrap_or_default(),
                    cached_efforts(provider, model),
                    |e| e.name.clone(),
                )
            },
            context_path: context_path.into(),
            context_label: context_label.into(),
            agents: ui_models::patch(
                &old.as_ref().map(|p| p.agents.clone()).unwrap_or_default(),
                agents,
                |a| a.id.clone(),
            ),
            graphs: ui_models::patch(&previous_graphs, graphs, |g| g.id.clone()),
            x: bounds.x,
            y: bounds.y,
            width: bounds.width,
            height: bounds.height,
            draft: tab
                .map(|t| t.composer.text.clone())
                .unwrap_or_default()
                .into(),
            scroll_y: tab.map(|t| t.reading.y).unwrap_or_default(),
            following: tab.is_none_or(|t| t.reading.following),
            sending: tab.is_some_and(|t| t.composer.pending.is_some()),
            send_error: tab
                .map(|t| t.composer.error.clone())
                .unwrap_or_default()
                .into(),
            busy: agent.is_some_and(|a| a.busy),
            model_name: agent
                .map(|a| a.record.model.clone())
                .or_else(|| tab.map(|t| t.composer.model.clone()))
                .unwrap_or_default()
                .into(),
            agent_name: agent
                .map(|a| {
                    a.record.label.clone().unwrap_or_else(|| {
                        if snapshot.is_some_and(|s| s.primary_agent_id == a.record.id) {
                            "Lead".into()
                        } else {
                            "Agent".into()
                        }
                    })
                })
                .unwrap_or_default()
                .into(),
            id: pane.id.clone().into(),
            tab_id: tab.map(|t| t.id.clone()).unwrap_or_default().into(),
            focused: Some(pane.id.as_str()) == focused_id,
            view: tab
                .map(|t| t.route.view.name())
                .unwrap_or("conversation")
                .into(),
            expanded: tab.map(|t| t.expanded.clone()).unwrap_or_default().into(),
            tabs: ui_models::patch(
                &old.as_ref().map(|p| p.tabs.clone()).unwrap_or_default(),
                tabs,
                |t| t.id.clone(),
            ),
            rows,
        });
    }
    let handles = separators
        .into_iter()
        .map(|handle| SplitRow {
            id: handle.id.into(),
            horizontal: handle.axis == crate::layout::Axis::Horizontal,
            ratio: handle.ratio,
            x: handle.bounds.x,
            y: handle.bounds.y,
            width: handle.bounds.width,
            height: handle.bounds.height,
        })
        .collect();
    let old_handles = ui.get_split_handles();
    let new_handles = ui_models::patch(&old_handles, handles, |h| h.id.clone());
    if old_handles != new_handles {
        ui.set_split_handles(new_handles);
    }
    let next = ui_models::patch(&previous, panes, |p| p.id.clone());
    if next != previous {
        ui.set_viewports(next);
    }
}