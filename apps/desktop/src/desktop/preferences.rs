//! Desktop preferences feature.
use super::*;

pub(super) fn update_check(ui: &MainWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = std::process::Command::new("firmius")
            .arg("update-check")
            .output();
        match result {
            Ok(output) => {
                let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
                let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
                let message = if !stdout.is_empty() { stdout } else { stderr };
                invoke(weak, move |ui| {
                    ui.set_notice(if message.is_empty() {
                        "Update check completed".into()
                    } else {
                        message.into()
                    });
                });
            }
            Err(error) => invoke(weak, move |ui| {
                ui.set_notice(format!("Update check unavailable: {error}").into())
            }),
        }
    });
}

pub(super) fn apply_search_mode(ui: &MainWindow, state: Arc<Mutex<DesktopState>>, mode: String) {
    let mode = mode.trim().to_ascii_lowercase();
    if !matches!(mode.as_str(), "off" | "cached" | "indexed" | "live") {
        set_notice(ui, "Search mode must be off, cached, indexed, or live");
        return;
    }
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session before changing hosted search");
        return;
    };
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, _) = shared_snapshot(&state, &active).await?;
                let mut config = FirmiusConfig::load().map_err(|error| error.to_string())?;
                config.general.web_search = (mode != "off").then_some(mode.clone());
                match client
                    .request(Request::UpdateConfig {
                        config: config.clone(),
                    })
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Ack => {
                        config.save().map_err(|error| error.to_string())?;
                        Ok(())
                    }
                    other => Err(format!(
                        "daemon returned an unexpected config response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok(()) => invoke(weak, move |ui| {
                ui.set_search_open(false);
                ui.set_search_mode(mode.clone().into());
                ui.set_notice(format!("Hosted search mode set to {mode}").into());
            }),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

pub(super) fn saved_ssh_rows() -> Vec<SshRow> {
    UserSettings::load()
        .map(|settings| {
            settings
                .remote_hosts
                .into_iter()
                .map(|host| SshRow {
                    alias: host.alias.into(),
                    directory: host.directory.into(),
                })
                .collect()
        })
        .unwrap_or_default()
}

pub(super) fn save_ssh_target(ui: &MainWindow, alias: String, directory: String) {
    let alias = alias.trim().to_string();
    let directory = directory.trim().to_string();
    if alias.is_empty() || directory.is_empty() {
        set_notice(ui, "SSH alias and remote directory are required");
        return;
    }
    let result = match UserSettings::load() {
        Ok(mut settings) => match settings.save_remote_host(alias.clone(), directory.clone()) {
            Ok(()) => settings.save().map_err(|error| error.to_string()),
            Err(error) => Err(error),
        },
        Err(error) => Err(error.to_string()),
    };
    match result {
        Ok(()) => set_notice(ui, format!("Saved SSH target {alias} → {directory}")),
        Err(error) => set_notice(ui, format!("SSH target could not be saved: {error}")),
    }
}

pub(super) fn refresh_providers(ui: &MainWindow) {
    ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
    ui.set_accounts(ModelRc::new(VecModel::from(
        firmius_core::list_accounts()
            .into_iter()
            .map(|account| AccountRow {
                id: account.id.into(),
                provider: account.kind.into(),
            })
            .collect::<Vec<_>>(),
    )));
    let rows = match firmius_core::persistence::load_providers() {
        Ok(providers) if providers.is_empty() => vec![transcript_row(
            "PROVIDERS",
            "No provider schemas are configured.",
            "assistant",
            "empty provider catalog",
        )],
        Ok(providers) => providers
            .into_iter()
            .flat_map(|provider| {
                let model_count = provider.models.len();
                let header = transcript_row(
                    "PROVIDER",
                    provider.id.clone(),
                    "tool",
                    format!(
                        "{} · {} model(s) · {}",
                        match provider.api_type {
                            firmius_core::ApiType::OpenAI => "OpenAI-compatible",
                            firmius_core::ApiType::Anthropic => "Anthropic",
                        },
                        model_count,
                        provider.effective_base_url()
                    ),
                );
                let models = provider.models.into_iter().map(move |model| {
                    transcript_row(
                        "MODEL",
                        model.id,
                        "assistant",
                        format!(
                            "provider {} · context {} · {} effort mode(s)",
                            provider.id,
                            model.context_window,
                            model.effort_modes.len()
                        ),
                    )
                });
                std::iter::once(header).chain(models).collect::<Vec<_>>()
            })
            .collect(),
        Err(error) => vec![transcript_row("PROVIDERS", error, "error", "load failed")],
    };
    ui.set_active_view("runtime".into());
    ui.invoke_show_document("Providers".into(), ModelRc::new(VecModel::from(rows)));
    set_notice(ui, "Provider catalog refreshed");
}

pub(super) fn model_catalog() -> firmius_core::ProviderManager {
    let mut manager = firmius_core::ProviderManager::new();
    if let Err(error) = manager.load() {
        eprintln!("Model catalog: {error}");
    }
    manager
}
pub(super) fn model_options() -> Vec<ModelOption> {
    let manager = model_catalog();
    let mut rows: Vec<_> = manager
        .model_choices_by_kind()
        .into_iter()
        .map(|(account, kind, model)| {
            let detail = manager
                .model_info_for(&account, &model)
                .map(|info| {
                    format!(
                        "{kind} · {}k context · {} effort modes",
                        info.context_window / 1000,
                        info.effort_modes.len()
                    )
                })
                .unwrap_or(kind);
            ModelOption {
                provider: account.into(),
                model: model.into(),
                detail: detail.into(),
            }
        })
        .collect();
    rows.sort_by(|a, b| {
        (a.provider.as_str(), a.model.as_str()).cmp(&(b.provider.as_str(), b.model.as_str()))
    });
    rows
}
pub(super) fn effort_options(provider_id: &str, model_id: &str) -> Vec<EffortOption> {
    let manager = model_catalog();
    manager
        .model_info_for(provider_id, model_id)
        .map(|info| {
            info.effort_modes
                .iter()
                .map(|effort| EffortOption {
                    name: effort.name.clone().into(),
                    detail: effort.reasoning_effort.clone().unwrap_or_default().into(),
                })
                .collect()
        })
        .unwrap_or_default()
}

pub(super) fn refresh_settings(ui: &MainWindow) {
    let rows = match UserSettings::load() {
        Ok(settings) => vec![
            transcript_row(
                "SETTINGS",
                format!(
                    "Theme: {}",
                    settings.theme.clone().unwrap_or_else(|| "firmius".into())
                ),
                "assistant",
                "appearance",
            ),
            transcript_row(
                "DEFAULT MODEL",
                settings
                    .default_model
                    .as_ref()
                    .map(|model| format!("{}/{}", model.provider_id, model.model))
                    .unwrap_or_else(|| "not configured".into()),
                "tool",
                settings
                    .default_model
                    .as_ref()
                    .and_then(|model| model.effort.clone())
                    .unwrap_or_else(|| "default effort".into()),
            ),
            transcript_row(
                "REMOTE WORKSPACES",
                settings.remote_hosts.len().to_string(),
                "tool",
                "saved SSH targets",
            ),
            transcript_row(
                "ONBOARDING",
                if settings.onboarding.completed_version > 0 {
                    "completed"
                } else {
                    "not completed"
                },
                "assistant",
                format!("version {}", settings.onboarding.completed_version),
            ),
        ],
        Err(error) => vec![transcript_row(
            "SETTINGS",
            error.to_string(),
            "error",
            "load failed",
        )],
    };
    ui.set_active_view("runtime".into());
    ui.invoke_show_document("Preferences".into(), ModelRc::new(VecModel::from(rows)));
    set_notice(ui, "Settings inspected");
}

pub(super) fn set_theme(ui: &MainWindow, name: &str) {
    match UserSettings::load() {
        Ok(mut settings) => {
            settings.theme = Some(name.to_string());
            match settings.save() {
                Ok(()) => {
                    ui.set_theme_name(name.into());
                    set_notice(ui, format!("Theme set to {name}"));
                }
                Err(error) => set_notice(ui, format!("Theme could not be saved: {error}")),
            }
        }
        Err(error) => set_notice(ui, format!("Settings could not be loaded: {error}")),
    }
}

pub(super) fn save_login(ui: &MainWindow) {
    let provider_id = ui.get_login_provider().trim().to_string();
    let account_label = ui.get_login_account_id().trim().to_string();
    let secret = ui.get_login_secret().to_string();
    if provider_id.is_empty() || secret.trim().is_empty() {
        set_notice(ui, "Provider schema and API key are required");
        return;
    }
    let schema = match firmius_core::persistence::load_providers() {
        Ok(schemas) => schemas.into_iter().find(|schema| schema.id == provider_id),
        Err(error) => {
            set_notice(ui, format!("Provider schemas could not be loaded: {error}"));
            return;
        }
    };
    let Some(schema) = schema else {
        set_notice(ui, format!("Unknown provider schema: {provider_id}"));
        return;
    };
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();
    let id = if account_label.is_empty() {
        format!("desktop-{provider_id}-{stamp}")
    } else {
        account_label
    };
    let record = AccountRecord {
        id,
        kind: provider_id,
        schema,
        credentials: serde_json::json!({ "api_key": secret }),
    };
    match save_account(&record) {
        Ok(()) => {
            ui.set_login_secret("".into());
            ui.set_login_open(false);
            refresh_accounts(ui);
            ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
            set_notice(ui, "Provider account saved");
        }
        Err(error) => set_notice(ui, format!("Account could not be saved: {error}")),
    }
}
