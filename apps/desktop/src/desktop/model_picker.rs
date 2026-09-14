//! Model Picker bindings.
use super::*;
pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_open_model_picker(move || {
            if let Some(ui) = weak.upgrade() {
                if let Ok(mut s) = state.lock() {
                    s.model_picker_target = s.shell.active().map(|t| t.id.clone());
                    if let Some(tab) = s.shell.active() {
                        ui.set_model_for_new(tab.route.session.is_none());
                        if tab.route.session.is_none() {
                            ui.set_new_provider(tab.composer.provider.clone().into());
                            ui.set_new_model(tab.composer.model.clone().into());
                            ui.set_new_workspace(tab.composer.workspace.clone().into());
                        }
                    }
                }
                ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
                ui.set_model_query("".into());
                ui.set_model_provider_draft(if ui.get_model_for_new() {
                    ui.get_new_provider()
                } else {
                    ui.get_active_provider()
                });
                ui.set_model_draft(if ui.get_model_for_new() {
                    ui.get_new_model()
                } else {
                    ui.get_active_model()
                });
                ui.set_effort_options(ModelRc::new(VecModel::from(effort_options(
                    ui.get_model_provider_draft().as_str(),
                    ui.get_model_draft().as_str(),
                ))));
                let effort = ui.get_active_effort();
                ui.set_effort_draft(if effort == "Default" {
                    "".into()
                } else {
                    effort
                });
                ui.set_model_picker_open(true);
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_apply_model(move |provider, model, effort| {
            if let Some(ui) = weak.upgrade() {
                if ui.get_model_for_new() {
                    if let Ok(mut s) = state.lock() {
                        let target = s.model_picker_target.clone();
                        if let Some(tab) = target.as_deref().and_then(|id| s.shell.tab_mut(id)) {
                            tab.composer.provider = provider.to_string();
                            tab.composer.model = model.to_string();
                            tab.composer.effort = effort.to_string();
                            tab.composer.workspace = ui.get_new_workspace().to_string();
                        }
                        shell_ui::render(&ui, &s);
                    }
                    ui.set_new_provider(provider);
                    ui.set_new_model(model);
                    ui.set_model_picker_open(false);
                    return;
                }
                let target = state.lock().ok().and_then(|s| {
                    s.model_picker_target
                        .as_deref()
                        .and_then(|id| s.shell.tab(id))
                        .map(|t| t.route.clone())
                });
                let Some(route) = target else { return };
                let Some(session) = route.session else { return };
                set_model_for_target(
                    &ui,
                    state.clone(),
                    session,
                    route.agent.unwrap_or_default(),
                    provider.trim().to_string(),
                    model.trim().to_string(),
                    effort.trim().to_string(),
                );
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_choose_model(move |provider, model| {
            if let Some(ui) = weak.upgrade() {
                ui.set_model_provider_draft(provider.to_string().into());
                ui.set_model_draft(model.to_string().into());
                ui.set_effort_options(ModelRc::new(VecModel::from(effort_options(
                    provider.as_str(),
                    model.as_str(),
                ))));
                ui.set_notice("Model selected; choose effort and apply".into());
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_choose_effort(move |effort| {
            if let Some(ui) = weak.upgrade() {
                ui.set_effort_draft(effort);
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_apply_persona(move |persona| {
            if let Some(ui) = weak.upgrade() {
                set_persona_for_active(&ui, state.clone(), persona.to_string());
            }
        });
    }
}
