//! Dispatch bindings.
use super::*;
pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    ui.on_remove_account(move |id| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        match firmius_core::persistence::delete_account(id.as_str()) {
            Ok(()) => {
                set_notice(&ui, "Provider account removed");
                refresh_accounts(&ui);
                ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
            }
            Err(error) => set_notice(&ui, format!("Account could not be removed: {error}")),
        }
    });
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_run_command(move |command| {
            let Some(ui) = weak.upgrade() else { return };
            let command = command.to_string();
            ui.set_command_palette_open(false);
            match command.as_str() {
                "status" | "sessions" => refresh(&ui, state.clone()),
                "agents" => refresh_active(&ui, state.clone()),
                "help" => set_notice(
                    &ui,
                    "Use the composer for prompts; command center covers sessions, models, personas, workflows, goals, MCP, permissions, SSH, accounts, settings, and export.",
                ),
                "update-check" => update_check(&ui),
                "resume" => refresh(&ui, state.clone()),
                "new" => {
                    navigation::new_task(&ui, &state);
                    ui.set_focused_agent_id("".into());
                    ui.set_active_model("".into());
                    ui.set_active_provider("".into());
                    ui.set_agents(ModelRc::new(VecModel::default()));
                    ui.set_focus_status("New task · choose a model".into());
                    ui.set_active_title("New task".into());
                    ui.set_active_detail("Choose a saved session or create a new one.".into());
                    ui.set_transcript(ModelRc::new(VecModel::from(Vec::<TranscriptRow>::new())));
                    ui.set_notice("Ready for a new session".into());
                }
                "search" => {
                    if let Ok(config) = FirmiusConfig::load() {
                        ui.set_search_mode(
                            config
                                .general
                                .web_search
                                .unwrap_or_else(|| "off".into())
                                .into(),
                        );
                    }
                    ui.set_search_open(true);
                }
                "memory" => ui.set_memory_open(true),
                "copy" => copy_transcript(&ui, false),
                "copy:all" => copy_transcript(&ui, true),
                "mcp" => refresh_mcp(&ui, state.clone()),
                "mcp:manage" => ui.set_mcp_open(true),
                "goals" | "goal" => refresh_goals(&ui, state.clone()),
                "goal:new" => ui.set_goal_open(true),
                "workflows" | "workflow" => refresh_workflows(&ui, state.clone()),
                "workflow:new" => ui.set_workflow_editor_open(true),
                "goal:activate" | "goal:cancel" | "goal:approve" | "goal:reject" => set_notice(
                    &ui,
                    format!("Use /{command} <goal-id> <current-revision> from the command center"),
                ),
                "workflow:insert" | "workflow:run" => set_notice(
                    &ui,
                    format!("Use /{command} <discovered-path> from the command center"),
                ),
                "ssh:hosts" | "ssh-hosts" => refresh_ssh_hosts(&ui),
                "ssh" | "ssh-add" | "ssh-saved" => {
                    ui.set_ssh_open(true);
                    set_notice(&ui, "Enter an SSH alias and directory; save or connect from the sheet");
                }
                "accounts" => refresh_accounts(&ui),
                "providers" => refresh_providers(&ui),
                "settings" => settings::open(&ui),
                "theme" => refresh_settings(&ui),
                "theme:firmius" => set_theme(&ui, "firmius"),
                "theme:nord" => set_theme(&ui, "nord"),
                "theme:monochrome" => set_theme(&ui, "monochrome"),
                "login" => ui.set_login_open(true),
                "login:save" => save_login(&ui),
                "clear" => {
                    ui.invoke_show_document("Cleared".into(), ModelRc::new(VecModel::from(Vec::<TranscriptRow>::new())))
                }
                "save" => mutate_active(&ui, state.clone(), "Session saved", |_| {
                    Ok(Request::SaveSession)
                }),
                "compact" => ui.invoke_compact(),
                "rewind" => ui.invoke_rewind(),
                "edit-history" => {
                    ui.set_active_view("changes".into());
                    refresh_active(&ui, state.clone());
                }
                "edit:undo" => ui.invoke_edit_history("undo".into()),
                "edit:redo" => ui.invoke_edit_history("redo".into()),
                "export" => ui.invoke_export_session(ui.get_export_path()),
                "title" => {
                    ui.set_title_draft(ui.get_active_title());
                    ui.set_title_editor_open(true);
                }
                "model" => ui.invoke_open_model_picker(),
                "persona" | "personas" => ui.set_persona_open(true),
                "effort" => ui.invoke_open_model_picker(),
                "permissions" => match state.lock() {
                    Ok(locked) => {
                        if locked.pending_permissions.is_empty() {
                            set_notice(&ui, "No pending permission requests");
                            ui.set_permission_open(false);
                        } else {
                            show_permission_queue(&ui, &locked);
                        }
                    }
                    Err(_) => set_notice(&ui, "Permission queue is unavailable"),
                },
                "onboarding" => ui.set_onboarding_open(true),
                "runtime" | "work" => {
                    ui.set_active_view(command.clone().into());
                    refresh_active(&ui, state.clone());
                }
                _ if crate::commands::known(&command) => set_notice(
                    &ui,
                    format!("{command} uses a dedicated form; press Enter on its completion"),
                ),
                _ => set_notice(&ui, format!("Unknown command: {command}")),
            }
        });
    }
}
