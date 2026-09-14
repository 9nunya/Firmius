//! Remote Workspaces bindings.
use super::*;
pub(super) fn bind(ui: &MainWindow, _state: Arc<Mutex<DesktopState>>) {
    {
        let weak = ui.as_weak();
        ui.on_save_ssh(move |alias, directory| {
            if let Some(ui) = weak.upgrade() {
                save_ssh_target(&ui, alias.to_string(), directory.to_string());
                ui.set_ssh_hosts(ModelRc::new(VecModel::from(saved_ssh_rows())));
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_use_saved_ssh(move |alias| {
            let Some(ui) = weak.upgrade() else { return };
            let alias = alias.to_string();
            match UserSettings::load().ok().and_then(|settings| {
                settings
                    .remote_hosts
                    .into_iter()
                    .find(|host| host.alias == alias)
            }) {
                Some(host) => {
                    ui.set_ssh_alias(host.alias.into());
                    ui.set_ssh_directory(host.directory.into());
                    set_notice(&ui, "Saved SSH target selected");
                }
                None => set_notice(&ui, format!("Saved SSH target not found: {alias}")),
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_connect_ssh(move |alias, directory| {
            let Some(ui) = weak.upgrade() else { return };
            let alias = alias.trim().to_string();
            let directory = directory.trim().trim_start_matches('/').to_string();
            if alias.is_empty() || directory.is_empty() {
                set_notice(&ui, "SSH alias and remote directory are required");
                return;
            }
            ui.set_ssh_open(false);
            ui.invoke_create_session(
                ui.get_new_provider(),
                ui.get_new_model(),
                format!("ssh://{alias}/{directory}").into(),
            );
        });
    }
}
