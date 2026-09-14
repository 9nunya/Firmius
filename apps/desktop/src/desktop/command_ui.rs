//! Command palette adapter. Parsing and completion live in the portable catalog.
use super::*;
fn results(query: &str) -> ModelRc<CommandRow> {
    ModelRc::new(VecModel::from(
        crate::commands::complete(query)
            .into_iter()
            .map(|(id, title, hint)| CommandRow {
                id: id.into(),
                title: title.into(),
                hint: hint.into(),
            })
            .collect::<Vec<_>>(),
    ))
}
pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    ui.set_command_results(results(""));
    let weak = ui.as_weak();
    ui.on_filter_commands(move |query| {
        if let Some(ui) = weak.upgrade() {
            ui.set_command_results(results(&query));
        }
    });
    let weak = ui.as_weak();
    ui.on_filter_models(move |query| {
        if let Some(ui) = weak.upgrade() {
            let query = query.to_lowercase();
            ui.set_model_options(ModelRc::new(VecModel::from(
                model_options()
                    .into_iter()
                    .filter(|row| {
                        format!("{} {}", row.provider, row.model)
                            .to_lowercase()
                            .contains(&query)
                    })
                    .collect::<Vec<_>>(),
            )));
        }
    });
    let weak = ui.as_weak();
    ui.on_execute_command(move |input| {
        let Some(ui) = weak.upgrade() else { return };
        let command = match crate::commands::parse(&input) {
            Ok(command) => command,
            Err(error) => {
                ui.set_notice(error.into());
                return;
            }
        };
        ui.set_command_palette_open(false);
        match command {
            crate::commands::Command::Route(id) => ui.invoke_run_command(id.into()),
            crate::commands::Command::Rename(title) => ui.invoke_set_title(title.into()),
            crate::commands::Command::Export(path) => ui.invoke_export_session(path.into()),
            crate::commands::Command::Search(mode) => ui.invoke_apply_search(mode.into()),
            crate::commands::Command::GoalAction {
                action,
                goal_id,
                revision,
            } => mutate_goal(&ui, state.clone(), action, goal_id, revision),
            crate::commands::Command::WorkflowAction { action, path } => {
                workflow_action(&ui, state.clone(), &action, &path)
            }
            crate::commands::Command::Rewind(turns) => {
                let agent_id = ui.get_focused_agent_id().to_string();
                mutate_active(&ui, state.clone(), "Conversation rewound", move |_| {
                    Ok(Request::Rewind { agent_id, turns })
                });
            }
        }
    });
}
