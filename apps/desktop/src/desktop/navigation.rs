//! Shell navigation commands; no transport or presenter logic.
use super::*;
pub(super) fn restore(ui: &MainWindow, state: &mut DesktopState) {
    ui.set_turn_active(false);
    ui.set_active_title(
        state
            .shell
            .active()
            .map(|t| t.title.clone())
            .unwrap_or_else(|| "Open a session".into())
            .into(),
    );
    ui.set_agents(ModelRc::new(VecModel::default()));
    if state
        .shell
        .active()
        .is_none_or(|tab| tab.route.session.is_none())
    {
        ui.set_focus_status("No agent selected".into());
        ui.set_live_status("Ready for a task".into());
        ui.set_active_provider("".into());
        ui.set_active_model("".into());
    }
    ui.set_sidebar_collapsed(state.shell.sidebar_collapsed);
    ui.set_active_view(state.active_view().into());
    ui.set_focused_agent_id(state.focused_agent().unwrap_or_default().into());
    ui.set_composer_text(
        state
            .shell
            .active()
            .map(|t| t.composer.text.clone())
            .unwrap_or_default()
            .into(),
    );
    ui.set_expanded_tool_key(
        state
            .shell
            .active()
            .map(|t| t.expanded.clone())
            .unwrap_or_default()
            .into(),
    );
    shell_ui::render(ui, state);
    if let Err(error) = storage::save(&state.shell) {
        ui.set_notice(format!("Could not save desktop state: {error}").into());
    }
}
fn open_new(ui: &MainWindow, state: &mut DesktopState) {
    state.shell.open(
        Route {
            session: None,
            agent: None,
            view: View::Conversation,
        },
        "New task".into(),
    );
    if let Some(tab) = state.shell.active_mut() {
        tab.composer.provider = ui.get_new_provider().to_string();
        tab.composer.model = ui.get_new_model().to_string();
        tab.composer.effort = ui.get_effort_draft().to_string();
        tab.composer.workspace = ui.get_new_workspace().to_string();
    }
}
pub(super) fn new_task(ui: &MainWindow, state: &Arc<Mutex<DesktopState>>) {
    if let Ok(mut s) = state.lock() {
        open_new(ui, &mut s);
        restore(ui, &mut s);
    }
}
pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    let pane_focus = state.clone();
    ui.on_focus_pane(move |id| {
        let Some(ui) = weak.upgrade() else { return };
        let changed = if let Ok(mut state) = pane_focus.lock() {
            if state.shell.focus_pane(&id) {
                restore(&ui, &mut state);
                true
            } else {
                false
            }
        } else {
            false
        };
        if changed {
            refresh_active(&ui, pane_focus.clone());
        }
    });
    let weak = ui.as_weak();
    let shortcuts = state.clone();
    ui.on_shell_shortcut(move |action| {
        let Some(ui) = weak.upgrade() else { return };
        let changed = if let Ok(mut state) = shortcuts.lock() {
            match action.as_str() {
                "next-tab" => state.shell.select_tab_offset(1),
                "previous-tab" => state.shell.select_tab_offset(-1),
                "close-tab" => {
                    if let Some(id) = state.shell.active().map(|tab| tab.id.clone()) {
                        state.shell.close(&id);
                    }
                    if state.shell.active().is_none() {
                        open_new(&ui, &mut state);
                    }
                }
                "focus-left" => {
                    state.shell.focus_adjacent("left");
                }
                "focus-right" => {
                    state.shell.focus_adjacent("right");
                }
                "focus-up" => {
                    state.shell.focus_adjacent("up");
                }
                "focus-down" => {
                    state.shell.focus_adjacent("down");
                }
                "move-left" => {
                    state.shell.move_active_tab("left");
                }
                "move-right" => {
                    state.shell.move_active_tab("right");
                }
                "move-up" => {
                    state.shell.move_active_tab("up");
                }
                "move-down" => {
                    state.shell.move_active_tab("down");
                }
                "next-pane" => {
                    let index = state
                        .shell
                        .viewports
                        .iter()
                        .position(|pane| pane.id == state.shell.focused)
                        .unwrap_or_default();
                    let next = state.shell.viewports[(index + 1) % state.shell.viewports.len()]
                        .id
                        .clone();
                    state.shell.focus_pane(&next);
                }
                numbered if numbered.starts_with("tab-") => {
                    if let Ok(number) = numbered[4..].parse() {
                        state.shell.select_tab_number(number);
                    }
                }
                "send" => {
                    let Some(id) = state.shell.active().map(|tab| tab.id.clone()) else {
                        return;
                    };
                    drop(state);
                    ui.invoke_submit_view(id.into());
                    return;
                }
                _ => return,
            }
            restore(&ui, &mut state);
            true
        } else {
            false
        };
        if changed {
            refresh_active(&ui, shortcuts.clone());
        }
    });
    let weak = ui.as_weak();
    let workflows = state.clone();
    ui.on_workflow_toggle(move |id| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        if let Ok(mut state) = workflows.lock() {
            if let Some(tab) = state.shell.tab_mut(&id) {
                tab.workflow_open = !tab.workflow_open;
            }
            restore(&ui, &mut state);
        }
    });
    let weak = ui.as_weak();
    let drag_state = state.clone();
    ui.on_drag_tab(move |tab, x, y, released| {
        let Some(ui) = weak.upgrade() else { return };
        let panes = ui.get_viewports();
        // Compact mode shows only the focused pane filling the workspace. Hit
        // the visible surface rather than stored split fractions that no longer
        // match the on-screen geometry.
        let compact = ui.get_compact_layout();
        let target = if compact {
            panes.iter().find(|pane| pane.focused)
        } else {
            panes.iter().find(|pane| {
                x >= pane.x && y >= pane.y && x <= pane.x + pane.width && y <= pane.y + pane.height
            })
        };
        let Some(pane) = target else {
            ui.set_drop_visible(false);
            return;
        };
        let bounds = if compact {
            crate::layout::Rect {
                x: 0.0,
                y: 0.0,
                width: 1.0,
                height: 1.0,
            }
        } else {
            crate::layout::Rect {
                x: pane.x,
                y: pane.y,
                width: pane.width.max(0.001),
                height: pane.height.max(0.001),
            }
        };
        let local_x = ((x - bounds.x) / bounds.width).clamp(0.0, 1.0);
        let local_y = ((y - bounds.y) / bounds.height).clamp(0.0, 1.0);
        let edge = crate::layout::drop_edge(local_x, local_y);
        let preview = crate::layout::drop_preview(bounds, edge);
        ui.set_drop_x(preview.x);
        ui.set_drop_y(preview.y);
        ui.set_drop_width(preview.width);
        ui.set_drop_height(preview.height);
        ui.set_drop_visible(!released);
        if released {
            if let Ok(mut state) = drag_state.lock() {
                state.shell.drop_tab(&tab, &pane.id, edge);
                restore(&ui, &mut state);
                if let Err(error) = storage::save(&state.shell) {
                    ui.set_notice(error.into());
                }
            }
        }
    });
    let weak = ui.as_weak();
    ui.on_layout_resized(move |_width, _height| {
        if let Some(ui) = weak.upgrade() {
            // Compact pane-switcher mode is a window-size decision. Sidebar
            // rail/drawer motion changes leftover workspace but must not hide
            // splits or flap compact mode.
            let scale = ui.window().scale_factor().max(0.01);
            let size = ui.window().size();
            let compact = crate::layout::window_is_compact(
                size.width as f32 / scale,
                size.height as f32 / scale,
                ui.get_compact_layout(),
            );
            if compact != ui.get_compact_layout() {
                ui.set_compact_layout(compact);
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_save_layout(move || {
        if let Ok(mut s) = s.lock() {
            if let Some(ui) = weak.upgrade() {
                s.shell.sidebar_collapsed = ui.get_sidebar_collapsed();
            }
            if let Err(error) = storage::save(&s.shell) {
                if let Some(ui) = weak.upgrade() {
                    ui.set_notice(error.into());
                }
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_reopen_tab(move || {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                s.shell.reopen_closed();
                restore(&ui, &mut s);
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_next_pane(move || {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                let index = s
                    .shell
                    .viewports
                    .iter()
                    .position(|p| p.id == s.shell.focused)
                    .unwrap_or_default();
                s.shell.focused = s.shell.viewports[(index + 1) % s.shell.viewports.len()]
                    .id
                    .clone();
                restore(&ui, &mut s);
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_resize_split(move |id, ratio| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                s.shell.layout.resize(&id, ratio);
                // Live split geometry must update immediately. Full route
                // projection is reserved for drop/commit so animation frames
                // never persist or rebuild transcripts.
                shell_ui::apply_live_geometry(&ui, &s);
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_split_below(move || {
        if let Some(ui) = weak.upgrade() {
            if !can_split(&ui, false) {
                return;
            }
            if let Ok(mut s) = s.lock() {
                s.shell.split_axis(crate::layout::Axis::Vertical);
                restore(&ui, &mut s);
            }
        }
    });
    let s = state.clone();
    ui.on_view_scrolled(move |id, y, following| {
        if let Ok(mut s) = s.lock() {
            if let Some(tab) = s.shell.tab_mut(&id) {
                tab.reading.scroll(y, following);
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_row_measured(move |id, key, y, height| {
        let correction = s.lock().ok().and_then(|mut s| {
            s.shell
                .tab_mut(&id)?
                .reading
                .measure(key.to_string(), y, height)
        });
        if let (Some(ui), Some(y)) = (weak.upgrade(), correction) {
            let panes = ui.get_viewports();
            if let Some(index) = panes.iter().position(|p| p.tab_id == id) {
                if let Some(mut pane) = panes.row_data(index) {
                    pane.scroll_y = y;
                    panes.set_row_data(index, pane);
                }
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_rewind_view(move |id, key, body| {
        let Some(ui) = weak.upgrade() else { return };
        let target = s.lock().ok().and_then(|state| {
            let tab = state.shell.tab(&id)?;
            let session = tab.route.session.clone()?;
            let agent = tab.route.agent.clone();
            Some((session, agent, tab.composer.revision))
        });
        let Some((session, agent, draft_revision)) = target else { return };
        let state_for_request = s.clone();
        let weak_for_notice = ui.as_weak();
        std::thread::spawn(move || {
            let result = crate::daemon::runtime().and_then(|rt| rt.block_on(async {
                let (client, snapshot) = crate::daemon::shared_snapshot(&state_for_request, &session).await?;
                let agent_id = agent.unwrap_or(snapshot.primary_agent_id);
                let selected = snapshot.agents.iter().find(|a| a.record.id == agent_id).ok_or("Agent no longer exists")?;
                if selected.busy { return Err("Stop this agent before rewinding".into()); }
                let prefix = format!("{agent_id}:message:");
                let (message, part) = key.strip_prefix(&prefix).and_then(|k| k.split_once(":part:")).ok_or("This message cannot be rewound")?;
                let message: usize = message.parse().map_err(|_| "Invalid message position")?;
                let part: usize = part.parse().map_err(|_| "Invalid message part")?;
                let history = &selected.record.history;
                let valid = history.get(message).is_some_and(|m| m.role == MessageRole::User && matches!(m.content.get(part), Some(MessagePart::Text(text)) if text == body.as_str()));
                if !valid { return Err("History changed; reopen the message before rewinding".into()); }
                let turns = history[message..].iter().filter(|m| m.role == MessageRole::User).count();
                match client.request(Request::Rewind { agent_id, turns }).await.map_err(|e| e.to_string())? {
                    Response::Rewound { .. } => attached_snapshot(&client, &session).await,
                    _ => Err("The daemon did not confirm the rewind".into()),
                }
            }));
            invoke(weak_for_notice, move |ui| {
                if let Ok(mut state) = state_for_request.lock() {
                    match result {
                        Ok(snapshot) => {
                            state.accept_snapshot(&snapshot);
                            if let Some(tab) = state.shell.tab_mut(&id) {
                                if tab.composer.revision == draft_revision { tab.composer.edit(body.to_string()); }
                                else { tab.composer.error = "Rewound. Your newer draft was kept.".into(); }
                            }
                        }
                        Err(error) => { if let Some(tab) = state.shell.tab_mut(&id) { tab.composer.error = error; } }
                    }
                    shell_ui::render(ui, &state);
                }
            });
        });
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_show_document(move |name, rows| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                let name = name.to_string();
                let session = s.active_session();
                let content = rows
                    .iter()
                    .map(|r| crate::shell::DocumentRow {
                        author: r.author.to_string(),
                        body: r.body.to_string(),
                        tone: r.tone.to_string(),
                        detail: r.detail.to_string(),
                    })
                    .collect();
                s.documents.insert((session.clone(), name.clone()), content);
                s.shell.open(
                    Route {
                        session,
                        agent: None,
                        view: View::Feature(name.clone()),
                    },
                    name,
                );
                restore(&ui, &mut s);
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_viewport_toggle(move |id, key| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                for pane in &mut s.shell.viewports {
                    if let Some(t) = pane.tabs.iter_mut().find(|t| t.id == id.as_str()) {
                        t.expanded = if t.expanded == key.as_str() {
                            String::new()
                        } else {
                            key.to_string()
                        };
                    }
                }
                shell_ui::render(&ui, &s);
            }
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_refresh(move || {
        if let Some(ui) = weak.upgrade() {
            refresh(&ui, s.clone());
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_set_view(move |view| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                let view = View::parse(&view);
                let session = if view == View::Settings {
                    None
                } else {
                    s.active_session()
                };
                let agent = s.focused_agent();
                s.shell.open(
                    Route {
                        session,
                        agent,
                        view: view.clone(),
                    },
                    view.name().into(),
                );
                restore(&ui, &mut s);
            }
            refresh_active(&ui, s.clone());
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_open_session(move |id| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                open_tab(&mut s, id.to_string(), "Session".into());
                restore(&ui, &mut s);
            }
            refresh_active(&ui, s.clone());
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_select_agent(move |id| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                let session = s.active_session();
                s.shell.open(
                    Route {
                        session,
                        agent: Some(id.to_string()),
                        view: View::Conversation,
                    },
                    "Agent conversation".into(),
                );
                restore(&ui, &mut s);
            }
            refresh_active(&ui, s.clone());
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_select_tab(move |id| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                s.shell.select(&id);
                restore(&ui, &mut s);
            }
            refresh_active(&ui, s.clone());
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_close_tab(move |id| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                s.shell.close(&id);
                if s.shell.active().is_none() {
                    open_new(&ui, &mut s);
                }
                restore(&ui, &mut s);
            }
            refresh_active(&ui, s.clone());
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_split_view(move || {
        if let Some(ui) = weak.upgrade() {
            if !can_split(&ui, true) {
                return;
            }
            if let Ok(mut s) = s.lock() {
                s.shell.split();
                restore(&ui, &mut s);
            }
        }
    });
}

fn can_split(ui: &MainWindow, horizontal: bool) -> bool {
    let Some(pane) = ui.get_viewports().iter().find(|p| p.focused) else {
        return false;
    };
    let available = if horizontal {
        pane.width * ui.get_workspace_width()
    } else {
        pane.height * ui.get_workspace_height()
    };
    let allowed = available >= if horizontal { 720.0 } else { 560.0 };
    if !allowed {
        ui.set_notice(
            "This pane is too small to split. Enlarge the window or use another tab.".into(),
        );
    }
    allowed
}
