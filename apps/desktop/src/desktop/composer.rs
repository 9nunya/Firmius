//! View-scoped composition and submission. Background work never consults focus.
use super::*;

pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    let permissions = state.clone();
    ui.on_set_permission_mode(move |tab_id, mode| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        let target = permissions.lock().ok().and_then(|state| {
            let session = state.shell.tab(&tab_id)?.route.session.clone()?;
            Some(session)
        });
        let Some(session) = target else {
            ui.set_notice("Permission mode becomes available after the session starts".into());
            return;
        };
        let requested_mode = mode.to_string();
        let selected = match parse_permission_mode(&requested_mode) {
            Ok(mode) => mode,
            Err(error) => {
                ui.set_notice(error.into());
                return;
            }
        };
        let weak = ui.as_weak();
        let state = permissions.clone();
        thread::spawn(move || {
            let result = runtime().and_then(|rt| {
                rt.block_on(async {
                    let client = crate::daemon::client_for(&state, Some(&session)).await?;
                    let current = client
                        .permission_policy()
                        .await
                        .map_err(|e| e.to_string())?;
                    gated_permission_mode(&selected, &current)?;
                    client
                        .set_permission_mode(selected, current.revision)
                        .await
                        .map_err(|e| e.to_string())
                })
            });
            invoke(weak, move |ui| match result {
                Ok(policy) => {
                    if let Ok(mut state) = state.lock() {
                        state.permission_policies.insert(session, policy);
                        shell_ui::render(ui, &state);
                    }
                    ui.set_notice(format!("Permission mode set to {requested_mode}").into());
                }
                Err(error) => {
                    ui.set_notice(format!("Permission mode was not changed: {error}").into())
                }
            });
        });
    });
    let weak = ui.as_weak();
    let workspaces = state.clone();
    ui.on_edit_view_workspace(move |id, workspace| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        if let Ok(mut state) = workspaces.lock() {
            if let Some(tab) = state.shell.tab_mut(&id) {
                tab.composer.workspace = workspace.to_string();
            }
            shell_ui::render(&ui, &state);
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_edit_view_draft(move |id, text| {
        let Some(ui) = weak.upgrade() else { return };
        if let Ok(mut state) = s.lock() {
            if let Some(tab) = state.shell.tab_mut(&id) {
                tab.composer.edit(text.to_string());
            }
            if state.shell.active().is_some_and(|t| t.id == id.as_str()) {
                ui.set_composer_text(text);
            }
        }
        let state = s.clone();
        let weak = ui.as_weak();
        let revision = state
            .lock()
            .ok()
            .and_then(|s| s.shell.tab(&id).map(|t| t.composer.revision));
        slint::Timer::single_shot(Duration::from_millis(400), move || {
            if let Ok(s) = state.lock() {
                if s.shell.tab(&id).map(|t| t.composer.revision) == revision {
                    if let Err(error) = storage::save(&s.shell) {
                        if let Some(ui) = weak.upgrade() {
                            ui.set_notice(format!("Could not save draft: {error}").into());
                        }
                    }
                }
            }
        });
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_submit_view(move |id| {
        if let Some(ui) = weak.upgrade() {
            submit(&ui, s.clone(), id.to_string());
        }
    });
    // Commands/workflows may still submit text through the window action. Resolve
    // their source once, then use exactly the same per-view transaction.
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_send_message(move |text| {
        let Some(ui) = weak.upgrade() else { return };
        let id = s.lock().ok().and_then(|mut s| {
            let tab = s.shell.active_mut()?;
            tab.composer.edit(text.to_string());
            Some(tab.id.clone())
        });
        if let Some(id) = id {
            submit(&ui, s.clone(), id);
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_model_for_view(move |id| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                s.shell.select(&id);
                navigation::restore(&ui, &mut s);
                if let Some(tab) = s.shell.tab(&id) {
                    if let Some(snapshot) = tab
                        .route
                        .session
                        .as_ref()
                        .and_then(|id| s.snapshots.get(id))
                    {
                        let agent_id = tab
                            .route
                            .agent
                            .as_deref()
                            .unwrap_or(&snapshot.primary_agent_id);
                        if let Some(agent) =
                            snapshot.agents.iter().find(|a| a.record.id == agent_id)
                        {
                            ui.set_focused_agent_id(agent_id.into());
                            ui.set_active_provider(agent.record.provider_id.clone().into());
                            ui.set_active_model(agent.record.model.clone().into());
                        }
                    }
                }
            }
            ui.invoke_open_model_picker();
        }
    });
    let weak = ui.as_weak();
    let s = state.clone();
    ui.on_commands_for_view(move |id| {
        if let Some(ui) = weak.upgrade() {
            if let Ok(mut s) = s.lock() {
                s.shell.select(&id);
                navigation::restore(&ui, &mut s);
            }
            ui.set_command_palette_open(true);
        }
    });
    let weak = ui.as_weak();
    ui.on_stop_view(move |id| {
        let Some(ui) = weak.upgrade() else { return };
        let target = state.lock().ok().and_then(|s| {
            let tab = s.shell.tab(&id)?;
            let session = tab.route.session.clone()?;
            let snapshot = s.snapshots.get(&session)?;
            let agent = tab
                .route
                .agent
                .as_deref()
                .unwrap_or(&snapshot.primary_agent_id);
            let turn = *snapshot.active_turns.get(agent)?;
            Some((session, turn))
        });
        let Some((session, turn_id)) = target else {
            return;
        };
        let state = state.clone();
        let weak = ui.as_weak();
        thread::spawn(move || {
            let result = runtime().and_then(|rt| {
                rt.block_on(async {
                    crate::daemon::client_for(&state, Some(&session))
                        .await?
                        .request(Request::CancelTurn { turn_id })
                        .await
                        .map_err(|e| e.to_string())
                })
            });
            invoke(weak, move |ui| match result {
                Ok(Response::Ack) => ui.set_notice("Cancellation requested".into()),
                Ok(_) => ui.set_notice("Cancellation requested".into()),
                Err(error) => ui.set_notice(error.into()),
            });
        });
    });
}

fn submit(ui: &MainWindow, state: Arc<Mutex<DesktopState>>, id: String) {
    let text = state
        .lock()
        .ok()
        .and_then(|s| s.shell.tab(&id).map(|t| t.composer.text.clone()));
    if let Some(text) = text.as_ref().filter(|t| t.trim_start().starts_with('/')) {
        if let Ok(mut s) = state.lock() {
            s.shell.select(&id);
            navigation::restore(ui, &mut s);
        }
        ui.invoke_execute_command(text.clone().into());
        return;
    }
    let (provider, model, effort_name, workspace) = state
        .lock()
        .ok()
        .and_then(|s| {
            s.shell.tab(&id).map(|t| {
                (
                    t.composer.provider.clone(),
                    t.composer.model.clone(),
                    t.composer.effort.clone(),
                    t.composer.workspace.clone(),
                )
            })
        })
        .unwrap_or_default();
    let transaction = state.lock().ok().and_then(|mut s| {
        let tab = s.shell.tab_mut(&id)?;
        if tab.route.session.is_none() && model.is_empty() {
            return None;
        }
        let (revision, text) = tab.composer.begin()?;
        Some((tab.route.clone(), revision, text))
    });
    let Some((route, revision, text)) = transaction else {
        if model.is_empty() {
            ui.invoke_model_for_view(id.into());
        }
        return;
    };
    if let Ok(s) = state.lock() {
        shell_ui::render(ui, &s);
    }
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|rt| rt.block_on(async {
            let session = match route.session {
                Some(id) => id,
                None => {
                    let manager = model_catalog();
                    let effort = manager.model_info_for(&provider, &model)
                        .and_then(|i| i.effort_modes.iter().find(|e| e.name == effort_name)).cloned();
                    let client = crate::daemon::client_for(&state, None).await?;
                    let snapshot = match client.request(Request::CreateSession(CreateSessionRequest {
                        provider_id: provider, model, effort, persona: Some("lead".into()),
                        workdir: (!workspace.trim().is_empty()).then_some(workspace),
                    })).await.map_err(|e| e.to_string())? {
                        Response::Snapshot(snapshot) => snapshot,
                        other => return Err(format!("Unexpected create response: {other:?}")),
                    };
                    // Resolve the originating view in place. Never open into the
                    // currently focused pane, which may have changed meanwhile.
                    if let Ok(mut s) = state.lock() {
                        if let Some(tab) = s.shell.tab_mut(&id) {
                            tab.route.session = Some(snapshot.session_id.clone());
                            tab.route.agent = Some(snapshot.primary_agent_id.clone());
                            tab.title = snapshot.title.clone().unwrap_or_else(|| "New task".into());
                        }
                        s.accept_snapshot(&snapshot);
                    }
                    snapshot.session_id
                }
            };
            let client = crate::daemon::client_for(&state, Some(&session)).await?;
            let snapshot = attached_snapshot(&client, &session).await?;
            let agent_id = route.agent.unwrap_or(snapshot.primary_agent_id.clone());
            if !snapshot.agents.iter().any(|a| a.record.id == agent_id) {
                return Err("This agent is no longer available. Your draft has been kept.".into());
            }
            let mut message = Message::text(MessageRole::User, text);
            message.provenance = Some(firmius_core::MessageProvenance {
                origin: firmius_core::MessageOrigin::Human,
                trust: firmius_core::MessageTrust::UserProvided,
            });
            let request = if snapshot.active_turns.contains_key(&agent_id) {
                Request::QueueMessage { agent_id, message }
            } else { Request::SubmitTurn(SubmitTurnRequest { agent_id, message }) };
            match client.request(request).await.map_err(|e| format!("Submission outcome is unknown: {e}. Reconnect and inspect the conversation before retrying."))? {
                Response::Ack | Response::TurnAccepted { .. } => Ok(()),
                other => Err(format!("Message was not accepted: {other:?}")),
            }
        }));
        invoke(weak, move |ui| {
            if let Ok(mut s) = state.lock() {
                if let Some(tab) = s.shell.tab_mut(&id) {
                    tab.composer.finish(revision, result);
                }
                if let Some(tab) = s.shell.active() {
                    ui.set_composer_text(tab.composer.text.clone().into());
                }
                shell_ui::render(ui, &s);
            }
            refresh(ui, state);
        });
    });
}
