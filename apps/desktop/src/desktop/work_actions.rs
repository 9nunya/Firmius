//! Work Actions bindings.
use super::*;
pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    {
        let weak = ui.as_weak();
        ui.on_build_workflow(move |title, brief, steps| {
            let title = title.trim().to_string();
            let brief = brief.trim().to_string();
            let steps: Vec<_> = steps
                .lines()
                .map(str::trim)
                .filter(|step| !step.is_empty())
                .map(ToOwned::to_owned)
                .collect();
            if title.is_empty() || steps.is_empty() {
                if let Some(ui) = weak.upgrade() {
                    set_notice(&ui, "Workflow title and at least one step are required");
                }
                return;
            }
            let prompt = workflow_prompt(&title, &brief, &steps);
            if let Some(ui) = weak.upgrade() {
                ui.set_workflow_editor_open(false);
                ui.set_composer_text(prompt.clone().into());
                ui.invoke_send_message(prompt.into());
                ui.set_composer_text("".into());
                set_notice(&ui, "Workflow submitted to the focused agent");
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_create_goal(move |description, check, approval_required| {
            if let Some(ui) = weak.upgrade() {
                create_goal(
                    &ui,
                    state.clone(),
                    description.to_string(),
                    check.to_string(),
                    approval_required,
                );
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_manage_mcp(move |action, name| {
            if let Some(ui) = weak.upgrade() {
                manage_mcp(&ui, state.clone(), action.to_string(), name.to_string());
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_add_mcp(move |name, command, url, args| {
            if let Some(ui) = weak.upgrade() {
                add_mcp(
                    &ui,
                    state.clone(),
                    name.to_string(),
                    command.to_string(),
                    url.to_string(),
                    args.to_string(),
                );
            }
        });
    }
}
