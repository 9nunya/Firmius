//! Session Actions bindings.
use super::*;
use crate::daemon::shared_client;
pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_set_title(move |title| {
            let title = title.trim().to_string();
            if let Some(ui) = weak.upgrade() {
                mutate_active(&ui, state.clone(), "Title updated", move |_| {
                    Ok(Request::SetTitle {
                        title: (!title.is_empty()).then_some(title),
                    })
                });
                ui.set_title_editor_open(false);
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_export_session(move |requested_path| {
            let active = state.lock().ok().and_then(|state| state.active_session());
            let Some(active) = active else {
                if let Some(ui) = weak.upgrade() {
                    set_notice(&ui, "Open a session before exporting");
                }
                return;
            };
            let path = requested_path.trim().to_string();
            let weak = weak.clone();
            let state = state.clone();
            thread::spawn(move || {
                let result = runtime().and_then(|runtime| {
                    runtime.block_on(async {
                        let (client, _) = shared_snapshot(&state, &active).await?;
                        let response = client
                            .request(Request::ExportSession)
                            .await
                            .map_err(|error| error.to_string())?;
                        let Response::Export(record) = response else {
                            return Err("daemon returned an unexpected export response".into());
                        };
                        let destination = if path.is_empty() {
                            let source = record
                                .title
                                .clone()
                                .unwrap_or_else(|| format!("session-{}", record.id));
                            let slug: String = source
                                .chars()
                                .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
                                .collect();
                            format!("{}.md", slug.trim_matches('-'))
                        } else {
                            path
                        };
                        fs::write(&destination, session_to_markdown(&record))
                            .map_err(|error| format!("export failed: {error}"))?;
                        Ok::<_, String>(destination)
                    })
                });
                match result {
                    Ok(destination) => invoke(weak, move |ui| {
                        ui.set_export_path(destination.clone().into());
                        ui.set_notice(format!("Exported {destination}").into());
                    }),
                    Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
                }
            });
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_cancel_turn(move || {
            if let Some(ui) = weak.upgrade() {
                mutate_active(&ui, state.clone(), "Cancellation requested", |snapshot| {
                    snapshot
                        .active_turns
                        .values()
                        .next()
                        .copied()
                        .map(|turn_id| Request::CancelTurn { turn_id })
                        .ok_or_else(|| "No active turn to cancel".into())
                });
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_save_session(move || {
            if let Some(ui) = weak.upgrade() {
                mutate_active(&ui, state.clone(), "Session saved", |_| {
                    Ok(Request::SaveSession)
                });
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_compact(move || {
            if let Some(ui) = weak.upgrade() {
                let agent_id = ui.get_focused_agent_id().to_string();
                mutate_active(
                    &ui,
                    state.clone(),
                    "Compaction requested",
                    move |snapshot| {
                        Ok(Request::Compact {
                            agent_id: snapshot
                                .agents
                                .iter()
                                .find(|agent| agent.record.id == agent_id)
                                .map(|agent| agent.record.id.clone())
                                .unwrap_or_else(|| snapshot.primary_agent_id.clone()),
                        })
                    },
                );
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_rewind(move || {
            if let Some(ui) = weak.upgrade() {
                let agent_id = ui.get_focused_agent_id().to_string();
                mutate_active(&ui, state.clone(), "Transcript rewound", move |snapshot| {
                    Ok(Request::Rewind {
                        agent_id: snapshot
                            .agents
                            .iter()
                            .find(|agent| agent.record.id == agent_id)
                            .map(|agent| agent.record.id.clone())
                            .unwrap_or_else(|| snapshot.primary_agent_id.clone()),
                        turns: 1,
                    })
                });
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_edit_history(move |action| {
            let action = action.to_string();
            if let Some(ui) = weak.upgrade() {
                let agent_id = ui.get_focused_agent_id().to_string();
                mutate_active(
                    &ui,
                    state.clone(),
                    "Edit history updated",
                    move |snapshot| {
                        Ok(Request::EditHistory {
                            agent_id: snapshot
                                .agents
                                .iter()
                                .find(|agent| agent.record.id == agent_id)
                                .map(|agent| agent.record.id.clone())
                                .unwrap_or_else(|| snapshot.primary_agent_id.clone()),
                            action,
                        })
                    },
                );
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_create_session(move |provider, model, workdir| {
            let provider = provider.trim().to_string();
            let model = model.trim().to_string();
            let workdir = workdir.trim().to_string();
            if provider.is_empty() || model.is_empty() {
                if let Some(ui) = weak.upgrade() {
                    set_notice(&ui, "Set a provider and model before creating a session");
                }
                return;
            }
            let weak = weak.clone();
            let state = state.clone();
            thread::spawn(move || {
                let result = runtime().and_then(|runtime| {
                    runtime.block_on(async {
                        let client = shared_client(&state).await?;
                        match client
                            .request(Request::CreateSession(CreateSessionRequest {
                                provider_id: provider,
                                model,
                                effort: None,
                                persona: Some("lead".into()),
                                workdir: (!workdir.is_empty()).then_some(workdir),
                            }))
                            .await
                            .map_err(|error| error.to_string())?
                        {
                            Response::Snapshot(snapshot) => Ok(snapshot),
                            other => Err(format!(
                                "daemon returned an unexpected create response: {other:?}"
                            )),
                        }
                    })
                });
                match result {
                    Ok(snapshot) => {
                        let title = snapshot
                            .title
                            .clone()
                            .unwrap_or_else(|| "Untitled session".into());
                        if let Ok(mut state) = state.lock() {
                            open_tab(&mut state, snapshot.session_id.clone(), title);
                        }
                        invoke(weak, move |ui| {
                            show_snapshot(ui, &state, snapshot);
                            ui.set_notice("New session ready".into());
                            if !ui.get_composer_text().trim().is_empty() {
                                ui.invoke_send_message(ui.get_composer_text());
                            }
                        });
                    }
                    Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
                }
            });
        });
    }
}
