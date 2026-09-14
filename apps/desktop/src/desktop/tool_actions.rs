//! Tool detail actions resolve their source view and domain IDs once.
use super::*;

pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_refresh_output(move |id| {
        let Some(ui) = weak.upgrade() else { return };
        let target = s.lock().ok().and_then(|s| {
            let tab = s.shell.tab(&id)?;
            let View::Output(key) = &tab.route.view else {
                return None;
            };
            let session = tab.route.session.clone()?;
            let request = s
                .output_requests
                .get(&(session.clone(), key.clone()))?
                .clone();
            Some((session, key.clone(), request))
        });
        if let Some((session, key, request)) = target {
            fetch_output(&ui, s.clone(), session, key, request, false);
        }
    });
    let weak = ui.as_weak();
    ui.on_activity_action(move |view, key, action, target| {
        let Some(ui) = weak.upgrade() else { return };
        if action == "copy-text" {
            copy_text(&ui, &target, "Copied", "There is no text to copy");
            return;
        }
        let source = state.lock().ok().and_then(|s| {
            let tab = s.shell.tab(&view)?;
            let session = tab.route.session.clone()?;
            let tool = s
                .models
                .get(&session)?
                .tools
                .iter()
                .find(|tool| tool.key == key.as_str())?
                .clone();
            Some((session, tool))
        });
        let Some((session, tool)) = source else {
            return;
        };
        if action == "copy-command" {
            copy_text(
                &ui,
                &presenters::tool_row(&tool).activity.command,
                "Command copied",
                "There is no command to copy",
            );
            return;
        }
        let mut request = None;
        if let Ok(mut s) = state.lock() {
            s.shell.select(&view);
            match action.as_str() {
                "agent" => {
                    let label = s
                        .snapshots
                        .get(&session)
                        .and_then(|snap| {
                            snap.agents.iter().find(|a| a.record.id == target.as_str())
                        })
                        .and_then(|a| a.record.label.clone())
                        .unwrap_or_else(|| "Agent".into());
                    let can_split = ui
                        .get_viewports()
                        .iter()
                        .find(|p| p.tab_id == view)
                        .is_some_and(|p| p.width * ui.get_workspace_width() >= 720.0);
                    if can_split {
                        s.shell.split();
                    }
                    s.shell.open(
                        Route {
                            session: Some(session.clone()),
                            agent: Some(target.to_string()),
                            view: View::Conversation,
                        },
                        label,
                    );
                }
                "changes" => {
                    s.shell.open(
                        Route {
                            session: Some(session.clone()),
                            agent: Some(tool.agent.clone()),
                            view: View::Changes,
                        },
                        "Changes".into(),
                    );
                    if let Some(tab) = s.shell.active_mut() {
                        tab.expanded = tool.key.clone();
                    }
                }
                "process" | "follow-process" => {
                    let resource: Option<firmius_core::ProcId> =
                        tool.resources.values().find_map(|r| match r {
                            firmius_core::ToolRuntimeResource::Process { id, .. }
                                if id == target.as_str() =>
                            {
                                id.parse().ok()
                            }
                            _ => None,
                        });
                    let Some(proc_id) = resource else { return };
                    let document = format!("process:{proc_id}");
                    let command = Request::HostPeek {
                        agent_id: tool.agent.clone(),
                        proc_id,
                        since: 0,
                    };
                    s.output_requests
                        .insert((session.clone(), document.clone()), command.clone());
                    s.documents.insert(
                        (Some(session.clone()), document.clone()),
                        vec![crate::shell::DocumentRow {
                            author: "Process output".into(),
                            body: "Loading retained process output…".into(),
                            tone: "assistant".into(),
                            detail: "Loading".into(),
                        }],
                    );
                    s.shell.open(
                        Route {
                            session: Some(session.clone()),
                            agent: Some(tool.agent.clone()),
                            view: View::Output(document.clone()),
                        },
                        "Process output".into(),
                    );
                    request = Some((document, command));
                }
                "unfollow-process" => {
                    let document = format!("process:{target}");
                    s.output_requests.remove(&(session.clone(), document));
                    set_notice(&ui, "Stopped following process output");
                }
                "work" => {
                    s.shell.open(
                        Route {
                            session: Some(session.clone()),
                            agent: Some(tool.agent.clone()),
                            view: View::Work,
                        },
                        "Work".into(),
                    );
                }
                "goal" => {
                    drop(s);
                    refresh_goals(&ui, state.clone());
                    return;
                }
                "memory" => {
                    drop(s);
                    ui.set_memory_open(true);
                    return;
                }
                "mcp" => {
                    drop(s);
                    ui.set_mcp_open(true);
                    return;
                }
                "output" | "raw-output" | "match" => {
                    let document = format!("tool:{}:{}", tool.key, target);
                    let body = if action == "match" {
                        let raw = tool.result.as_deref().unwrap_or_default();
                        let hits = crate::tool_content::search_matches(raw);
                        let listed = crate::tool_content::listed_paths(raw);
                        hits.iter()
                            .filter(|m| m.path == target.as_str())
                            .map(|m| format!("{}:{}: {}", m.path, m.line, m.text))
                            .chain(
                                listed
                                    .iter()
                                    .filter(|m| m.path == target.as_str())
                                    .map(|m| m.path.clone()),
                            )
                            .collect::<Vec<_>>()
                            .join("\n")
                    } else {
                        tool.result.clone().unwrap_or_default()
                    };
                    s.documents.insert(
                        (Some(session.clone()), document.clone()),
                        vec![crate::shell::DocumentRow {
                            author: tool.name.clone(),
                            body,
                            tone: "assistant".into(),
                            detail: if action == "raw-output" {
                                "Raw structured result · recorded evidence"
                            } else {
                                "Recorded tool result"
                            }
                            .into(),
                        }],
                    );
                    s.shell.open(
                        Route {
                            session: Some(session.clone()),
                            agent: Some(tool.agent.clone()),
                            view: View::Output(document),
                        },
                        if action == "raw-output" {
                            format!("{} raw result", tool.name)
                        } else if target.is_empty() {
                            format!("{} result", tool.name)
                        } else {
                            target.to_string()
                        },
                    );
                }
                _ => return,
            }
            navigation::restore(&ui, &mut s);
        }
        if let Some((document, request)) = request {
            fetch_output(
                &ui,
                state.clone(),
                session,
                document,
                request,
                action == "follow-process",
            );
        }
    });
}
fn fetch_output(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    session: String,
    document: String,
    request: Request,
    follow: bool,
) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        loop {
            let result = runtime().and_then(|rt| {
                rt.block_on(async {
                    crate::daemon::client_for(&state, Some(&session))
                        .await?
                        .request(request.clone())
                        .await
                        .map_err(|e| e.to_string())
                })
            });
            let running = matches!(
                result,
                Ok(Response::HostPeek(firmius_protocol::HostPeekResponse {
                    status: firmius_core::ProcStatus::Running,
                    ..
                }))
            );
            let state_for_ui = state.clone();
            let session_for_ui = session.clone();
            let document_for_ui = document.clone();
            invoke(weak.clone(), move |ui| {
                let (body, detail) = match result {
                    Ok(Response::HostPeek(output)) => {
                        let omitted = output.total.saturating_sub(output.bytes.len());
                        (
                            String::from_utf8_lossy(&output.bytes).into_owned(),
                            format!(
                                "{} · {} retained bytes · {} total bytes{}",
                                match output.status {
                                    firmius_core::ProcStatus::Running => "Following live output",
                                    firmius_core::ProcStatus::Exited { success: true, .. } =>
                                        "Exited successfully",
                                    firmius_core::ProcStatus::Exited { success: false, .. } =>
                                        "Exited with failure",
                                },
                                output.bytes.len(),
                                output.total,
                                if omitted > 0 {
                                    format!(" · {omitted} earlier bytes omitted")
                                } else {
                                    String::new()
                                }
                            ),
                        )
                    }
                    Ok(_) => (
                        "No output is available for this process yet.".into(),
                        "Output unavailable".into(),
                    ),
                    Err(error) => (error, "Output unavailable · retry when connected".into()),
                };
                if let Ok(mut s) = state_for_ui.lock() {
                    s.documents.insert(
                        (Some(session_for_ui), document_for_ui),
                        vec![crate::shell::DocumentRow {
                            author: "Process output".into(),
                            body,
                            tone: "assistant".into(),
                            detail,
                        }],
                    );
                    shell_ui::render(ui, &s);
                }
            });
            if !follow || !running {
                break;
            }
            thread::sleep(std::time::Duration::from_millis(500));
            if !state.lock().is_ok_and(|state| {
                state
                    .output_requests
                    .contains_key(&(session.clone(), document.clone()))
            }) {
                break;
            }
        }
    });
}
