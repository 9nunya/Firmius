//! Editable runtime preferences, committed by the daemon.
use super::*;
use crate::daemon::shared_client;

pub(super) fn apply_general_settings(
    config: &mut FirmiusConfig,
    autosave: bool,
    thinking: bool,
    output: &str,
    search_mode: Option<&str>,
) -> Result<(), String> {
    let output = output
        .trim()
        .parse::<u32>()
        .ok()
        .filter(|n| (256..=200_000).contains(n))
        .ok_or_else(|| "Output limit must be 256–200,000 tokens".to_string())?;
    config.general.autosave_sessions = autosave;
    config.general.show_thinking = thinking;
    config.general.default_max_output_tokens = output;
    if let Some(mode) = search_mode {
        let mode = mode.trim().to_ascii_lowercase();
        if !matches!(mode.as_str(), "" | "off" | "cached" | "indexed" | "live") {
            return Err("Search mode must be off, cached, indexed, or live".into());
        }
        config.general.web_search = (mode != "off" && !mode.is_empty()).then_some(mode);
    }
    Ok(())
}

pub(super) fn open(ui: &MainWindow) {
    match FirmiusConfig::load() {
        Ok(config) => {
            ui.set_settings_autosave(config.general.autosave_sessions);
            ui.set_settings_thinking(config.general.show_thinking);
            ui.set_settings_output(config.general.default_max_output_tokens.to_string().into());
            ui.set_search_mode(
                config
                    .general
                    .web_search
                    .clone()
                    .unwrap_or_else(|| "off".into())
                    .into(),
            );
            ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
            ui.invoke_set_view("settings".into());
        }
        Err(error) => ui.set_notice(format!("Cannot load settings: {error}").into()),
    }
}

pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    ui.on_save_settings(move || {
        let Some(ui) = weak.upgrade() else { return };
        let mut config = match FirmiusConfig::load() {
            Ok(config) => config,
            Err(error) => {
                ui.set_notice(error.to_string().into());
                return;
            }
        };
        if let Err(error) = apply_general_settings(
            &mut config,
            ui.get_settings_autosave(),
            ui.get_settings_thinking(),
            &ui.get_settings_output(),
            // Hosted search has a dedicated form. SettingsView cannot bind
            // web_search without new app.slint/viewport properties, so this
            // save must not clobber the last daemon-backed search mode.
            None,
        ) {
            ui.set_notice(error.into());
            return;
        }
        let weak = ui.as_weak();
        let state = state.clone();
        thread::spawn(move || {
            let result = runtime().and_then(|rt| {
                rt.block_on(async {
                    let client = shared_client(&state).await?;
                    match client
                        .request(Request::UpdateConfig {
                            config: config.clone(),
                        })
                        .await
                        .map_err(|e| e.to_string())?
                    {
                        Response::Ack => config.save().map_err(|error| error.to_string()),
                        other => Err(format!("Unexpected settings response: {other:?}")),
                    }
                })
            });
            invoke(weak, move |ui| match result {
                Ok(()) => {
                    ui.set_notice("Settings saved".into());
                    ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
                }
                Err(error) => ui.set_notice(error.into()),
            });
        });
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn general_settings_require_a_typed_token_budget() {
        let mut config = FirmiusConfig::default();
        assert!(apply_general_settings(&mut config, true, false, "12", None).is_err());
        assert!(apply_general_settings(&mut config, true, false, "words", None).is_err());
        apply_general_settings(&mut config, false, true, "4096", Some("live")).unwrap();
        assert!(!config.general.autosave_sessions);
        assert!(config.general.show_thinking);
        assert_eq!(config.general.default_max_output_tokens, 4096);
        assert_eq!(config.general.web_search.as_deref(), Some("live"));
        apply_general_settings(&mut config, true, true, "32000", Some("off")).unwrap();
        assert_eq!(config.general.web_search, None);
        assert!(apply_general_settings(&mut config, true, true, "32000", Some("made-up")).is_err());
        config.general.web_search = Some("cached".into());
        apply_general_settings(&mut config, false, false, "8192", None).unwrap();
        assert_eq!(config.general.web_search.as_deref(), Some("cached"));
        assert!(!config.general.autosave_sessions);
        assert_eq!(config.general.default_max_output_tokens, 8192);
    }
}
