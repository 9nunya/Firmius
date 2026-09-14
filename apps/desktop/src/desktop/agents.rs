//! Desktop agents feature.
use super::*;
use crate::daemon::shared_snapshot;

pub(super) fn set_persona_for_active(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    persona: String,
) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session before changing its persona");
        return;
    };
    let focused = ui.get_focused_agent_id().to_string();
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, snapshot) = shared_snapshot(&state, &active).await?;
                let agent_id = snapshot
                    .agents
                    .iter()
                    .find(|agent| agent.record.id == focused)
                    .map(|agent| agent.record.id.clone())
                    .unwrap_or_else(|| snapshot.primary_agent_id.clone());
                match client
                    .request(Request::SetPersona(SetPersonaRequest {
                        agent_id,
                        persona: (!persona.trim().is_empty()).then_some(persona.clone()),
                        delegated: false,
                    }))
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Ack => Ok(attached_snapshot(&client, &active).await?),
                    other => Err(format!(
                        "daemon returned an unexpected persona response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok(snapshot) => invoke(weak, move |ui| {
                ui.set_persona_open(false);
                show_snapshot(ui, &state, snapshot);
                ui.set_notice("Persona applied".into());
            }),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

pub(super) fn set_model_for_target(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    active: String,
    focused: String,
    provider_id: String,
    model: String,
    effort_name: String,
) {
    if provider_id.trim().is_empty() || model.trim().is_empty() {
        set_notice(ui, "Provider and model are required");
        return;
    }
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, snapshot) = shared_snapshot(&state, &active).await?;
                let agent_id = if snapshot
                    .agents
                    .iter()
                    .any(|agent| agent.record.id == focused)
                {
                    focused.clone()
                } else {
                    snapshot.primary_agent_id.clone()
                };
                if snapshot.active_turns.contains_key(&agent_id) {
                    return Err(
                        "Wait for the focused agent to finish before changing its model".into(),
                    );
                }
                let manager = model_catalog();
                let effort = if effort_name.trim().is_empty() {
                    None
                } else {
                    Some(
                        manager
                            .model_info_for(&provider_id, &model)
                            .and_then(|info| {
                                info.effort_modes
                                    .iter()
                                    .find(|effort| effort.name == effort_name)
                            })
                            .cloned()
                            .ok_or_else(|| {
                                "Unsupported reasoning effort for this model".to_string()
                            })?,
                    )
                };
                match client
                    .request(Request::SetModel(SetModelRequest {
                        agent_id,
                        provider_id: provider_id.clone(),
                        model: model.clone(),
                        effort,
                    }))
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Ack => {}
                    other => {
                        return Err(format!(
                            "daemon returned an unexpected model response: {other:?}"
                        ));
                    }
                }
                let preference_warning = match UserSettings::load() {
                    Ok(mut settings) => {
                        settings.set_preferred_default(
                            provider_id,
                            model,
                            (!effort_name.trim().is_empty()).then_some(effort_name),
                        );
                        settings.save().err().map(|error| {
                            format!("model applied, but default was not saved: {error}")
                        })
                    }
                    Err(error) => {
                        Some(format!("model applied, but default was not saved: {error}"))
                    }
                };
                Ok::<_, String>((
                    attached_snapshot(&client, &active).await?,
                    preference_warning,
                ))
            })
        });
        match result {
            Ok((snapshot, preference_warning)) => invoke(weak, move |ui| {
                ui.set_model_picker_open(false);
                show_snapshot(ui, &state, snapshot);
                ui.set_notice(
                    preference_warning
                        .unwrap_or_else(|| "Model applied and saved as the default".into())
                        .into(),
                );
            }),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}
