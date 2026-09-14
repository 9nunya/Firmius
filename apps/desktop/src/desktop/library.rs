//! Desktop library feature.
use super::*;

pub(super) fn refresh_workflows(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let files = discover_workflow_files();
    let selected = state
        .lock()
        .ok()
        .and_then(|state| state.last_workflow_path.clone());
    let rows = if files.is_empty() {
        vec![transcript_row(
            "WORKFLOWS",
            "No prompt workflows found. Add .md, .workflow, .yaml, or .toml files under .firmius/workflows, workflows, or the global workflows directory.",
            "assistant",
            "empty library",
        )]
    } else {
        files
            .iter()
            .map(|path| {
                let selected_mark = selected
                    .as_ref()
                    .filter(|chosen| *chosen == path)
                    .map(|_| "Selected · ")
                    .unwrap_or_default();
                transcript_row(
                    "WORKFLOW",
                    path.display().to_string(),
                    "tool",
                    format!(
                        "{selected_mark}Select explicitly: /workflow:insert {} or /workflow:run {}",
                        path.display(),
                        path.display()
                    ),
                )
            })
            .collect()
    };
    ui.set_active_view("work".into());
    ui.invoke_show_document("Workflows".into(), ModelRc::new(VecModel::from(rows)));
    set_notice(ui, format!("{} workflow file(s) discovered", files.len()));
}

pub(super) fn workflow_action(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    action: &str,
    selected_path: &str,
) {
    let Some(path) = resolve_workflow_path(selected_path, &discover_workflow_files()) else {
        set_notice(ui, "Select a workflow using its exact discovered path");
        return;
    };
    if let Ok(mut state) = state.lock() {
        state.last_workflow_path = Some(path.clone());
    }
    let content = match fs::read_to_string(&path) {
        Ok(content) if !content.trim().is_empty() => content,
        Ok(_) => {
            set_notice(ui, "Selected workflow is empty");
            return;
        }
        Err(error) => {
            set_notice(ui, format!("Could not read workflow: {error}"));
            return;
        }
    };
    match action {
        "insert" => {
            ui.invoke_set_view("conversation".into());
            if let Ok(mut s) = state.lock() {
                if let Some(tab) = s.shell.active_mut() {
                    tab.composer.edit(content.clone());
                }
                shell_ui::render(ui, &s);
            }
            ui.set_composer_text(content.into());
            set_notice(ui, format!("Inserted {}", path.display()));
        }
        "run" => {
            ui.set_composer_text(content.clone().into());
            ui.invoke_send_message(content.into());
            ui.set_composer_text("".into());
            set_notice(ui, format!("Ran {}", path.display()));
        }
        _ => set_notice(ui, "Unknown workflow action"),
    }
}

fn resolve_workflow_path(
    selected: &str,
    discovered: &[std::path::PathBuf],
) -> Option<std::path::PathBuf> {
    let selected = selected.trim();
    if selected.is_empty() {
        return None;
    }
    let matches: Vec<_> = discovered
        .iter()
        .filter(|path| path.to_string_lossy() == selected)
        .cloned()
        .collect();
    match matches.as_slice() {
        [path] => Some(path.clone()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::resolve_workflow_path;
    use std::path::PathBuf;

    #[test]
    fn workflow_selection_requires_an_exact_discovered_path() {
        let files = vec![
            PathBuf::from("/tmp/alpha.md"),
            PathBuf::from("/tmp/beta.md"),
        ];
        assert_eq!(
            resolve_workflow_path("/tmp/beta.md", &files),
            Some(PathBuf::from("/tmp/beta.md"))
        );
        assert_eq!(resolve_workflow_path("beta.md", &files), None);
        assert_eq!(resolve_workflow_path("", &files), None);
        let duplicates = vec![
            PathBuf::from("/tmp/alpha.md"),
            PathBuf::from("/tmp/alpha.md"),
        ];
        assert_eq!(resolve_workflow_path("/tmp/alpha.md", &duplicates), None);
    }
}

pub(super) fn refresh_ssh_hosts(ui: &MainWindow) {
    let rows = match firmius_core::discover_ssh_hosts() {
        Ok(hosts) if hosts.is_empty() => vec![transcript_row(
            "SSH HOSTS",
            "No concrete SSH hosts were found in ~/.ssh/config or ~/.ssh/known_hosts.",
            "assistant",
            "empty inventory",
        )],
        Ok(hosts) => hosts
            .into_iter()
            .map(|host| {
                transcript_row(
                    "SSH HOST",
                    host.alias,
                    "tool",
                    format!(
                        "{} · {} · port {} · {}",
                        host.hostname.unwrap_or_else(|| "hostname unknown".into()),
                        host.user.unwrap_or_else(|| "user default".into()),
                        host.port.unwrap_or(22),
                        if host.configured {
                            "config"
                        } else {
                            "known_hosts"
                        }
                    ),
                )
            })
            .collect(),
        Err(error) => vec![transcript_row(
            "SSH HOSTS",
            error,
            "error",
            "discovery failed",
        )],
    };
    ui.set_active_view("runtime".into());
    ui.invoke_show_document("SSH hosts".into(), ModelRc::new(VecModel::from(rows)));
    set_notice(ui, "SSH host inventory refreshed");
}

pub(super) fn refresh_accounts(ui: &MainWindow) {
    let accounts = firmius_core::list_accounts();
    ui.set_accounts(ModelRc::new(VecModel::from(
        accounts
            .into_iter()
            .map(|account| AccountRow {
                id: account.id.into(),
                provider: account.kind.into(),
            })
            .collect::<Vec<_>>(),
    )));
    ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
    ui.set_accounts_open(true);
}
