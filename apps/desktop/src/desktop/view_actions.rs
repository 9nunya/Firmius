//! View Actions bindings.
use super::*;
pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    {
        let weak = ui.as_weak();
        ui.on_complete_onboarding(move || match UserSettings::load() {
            Ok(mut settings) => {
                settings.onboarding.completed_version = firmius_core::ONBOARDING_VERSION;
                match settings.save() {
                    Ok(()) => {
                        if let Some(ui) = weak.upgrade() {
                            ui.set_onboarding_open(false);
                            set_notice(&ui, "Onboarding complete");
                        }
                    }
                    Err(error) => {
                        if let Some(ui) = weak.upgrade() {
                            set_notice(&ui, format!("Could not save onboarding: {error}"));
                        }
                    }
                }
            }
            Err(error) => {
                if let Some(ui) = weak.upgrade() {
                    set_notice(&ui, format!("Could not load settings: {error}"));
                }
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_toggle_tool(move |key| {
            let Some(ui) = weak.upgrade() else { return };
            let key = key.to_string();
            let current = ui.get_expanded_tool_key().to_string();
            let next = if current == key {
                "__all__".to_string()
            } else {
                key
            };
            ui.set_expanded_tool_key(next.into());
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_apply_search(move |mode| {
            if let Some(ui) = weak.upgrade() {
                apply_search_mode(&ui, state.clone(), mode.to_string());
            }
        });
    }
}
