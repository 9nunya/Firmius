//! Native desktop surface for the Firmius daemon.
//!
//! Every tab attaches to durable daemon-owned session state. The desktop
//! therefore shares the TUI's agent tree, transcript, undo history, and
//! autonomous execution rather than launching a separate harness.

use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use chrono::Utc;
use firmius_core::{
    AccountRecord, AgentEvent, FirmiusConfig, McpServerConfig, Message, MessagePart, MessageRole,
    SessionSummary, UserSettings, save_account, session_to_markdown,
};
use firmius_protocol::{
    ActivateGoalRequest, ApprovalDecision, ApproveGoalRequest, CancelGoalRequest,
    CreateGoalRequest, CreateSessionRequest, DaemonEvent, GoalActor, GoalOwner, GoalProvenance,
    GoalRequest, GoalResponse, GoalSource, ListGoalsRequest, McpCommand, PermissionResolution,
    Request, Response, SessionSnapshot, SetModelRequest, SetPersonaRequest, SubmitTurnRequest,
};
use slint::{Model, ModelRc, VecModel};

slint::include_modules!();
mod dispatch;
#[cfg(target_os = "macos")]
mod macos_window;
mod model_picker;
mod navigation;
mod permission_inbox;
use permission_inbox::*;
mod remote_workspaces;
mod session_actions;
mod view_actions;
mod work_actions;

mod command_ui;
mod composer;
mod presenters;
mod preview;
mod settings;
mod terminal;
mod transcript;
use transcript::*;
mod agents;
use agents::*;
mod mcp;
mod memory;
use mcp::*;
mod goals;
use goals::*;
mod library;
use library::*;
mod preferences;
use preferences::*;
mod connection;
use connection::*;

use crate::daemon::{attached_snapshot, runtime, shared_client, shared_snapshot};
use crate::presentation::{tool_call_preview, workflow_prompt};
use crate::shell::{Route, View};
use crate::state::DesktopState;
mod shell_ui;
mod sidebar;
mod storage;
mod tool_actions;
mod ui_models;
use crate::workflows::discover as discover_workflow_files;

fn invoke(weak: slint::Weak<MainWindow>, update: impl FnOnce(&MainWindow) + Send + 'static) {
    let _ = slint::invoke_from_event_loop(move || {
        if let Some(ui) = weak.upgrade() {
            update(&ui);
        }
    });
}

fn set_notice(ui: &MainWindow, text: impl Into<String>) {
    let text = text.into();
    invoke(ui.as_weak(), move |ui| ui.set_notice(text.into()));
}

fn copy_text(ui: &MainWindow, text: &str, ok: &str, empty: &str) {
    if text.trim().is_empty() {
        set_notice(ui, empty);
        return;
    }
    let mut copied = false;
    #[cfg(target_os = "macos")]
    {
        if let Ok(mut child) = std::process::Command::new("pbcopy")
            .stdin(std::process::Stdio::piped())
            .spawn()
        {
            if child
                .stdin
                .take()
                .and_then(|mut stdin| stdin.write_all(text.as_bytes()).ok())
                .is_some()
                && child.wait().is_ok()
            {
                copied = true;
            }
        }
    }
    #[cfg(not(target_os = "macos"))]
    for program in ["wl-copy", "xclip"] {
        let mut command = std::process::Command::new(program);
        if program == "xclip" {
            command.args(["-selection", "clipboard"]);
        }
        if let Ok(mut child) = command.stdin(std::process::Stdio::piped()).spawn() {
            if child
                .stdin
                .take()
                .and_then(|mut stdin| stdin.write_all(text.as_bytes()).ok())
                .is_some()
                && child.wait().is_ok()
            {
                copied = true;
                break;
            }
        }
    }
    if copied {
        set_notice(ui, ok);
    } else {
        set_notice(ui, "No native clipboard utility was available");
    }
}

fn copy_transcript(ui: &MainWindow, all: bool) {
    let model = ui.get_transcript();
    let mut text = String::new();
    if all {
        for index in 0..model.row_count() {
            if let Some(row) = model.row_data(index) {
                text.push_str(&format!("{}\n{}\n\n", row.author, row.body));
            }
        }
    } else {
        for index in (0..model.row_count()).rev() {
            if let Some(row) = model.row_data(index)
                && matches!(row.tone.as_str(), "assistant" | "thinking")
            {
                text.push_str(&row.body);
                break;
            }
        }
    }
    copy_text(
        ui,
        &text,
        if all {
            "Transcript copied to clipboard"
        } else {
            "Last assistant reply copied to clipboard"
        },
        "There is no transcript content to copy",
    );
}

fn format_session(session: &SessionSummary) -> SessionRow {
    let workspace = firmius_core::persistence::load_session_record(&session.id)
        .ok()
        .and_then(|record| {
            record
                .agents
                .first()
                .map(|a| a.workdir.to_string_lossy().into_owned())
        })
        .unwrap_or_default();
    let (host, path) = if let Some(remote) = workspace.strip_prefix("ssh://") {
        let (host, path) = remote.split_once('/').unwrap_or((remote, ""));
        (format!("{host} (SSH)"), path)
    } else {
        ("Local".to_owned(), workspace.as_str())
    };
    let project = path
        .trim_end_matches('/')
        .rsplit('/')
        .next()
        .filter(|s| !s.is_empty())
        .unwrap_or("Unknown workspace");
    SessionRow {
        kind: "thread".into(),
        key: session.id.clone().into(),
        collapsed: false,
        host: host.into(),
        workspace: workspace.clone().into(),
        project: project.into(),
        id: session.id.clone().into(),
        title: session.title.clone().into(),
        detail: format!(
            "{} agent{} · {}",
            session.agent_count,
            if session.agent_count == 1 { "" } else { "s" },
            session.preview
        )
        .into(),
    }
}

fn open_tab(state: &mut DesktopState, id: String, title: String) {
    state.shell.open(
        Route {
            session: Some(id),
            agent: None,
            view: View::Conversation,
        },
        title,
    );
}

fn refresh(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let client = shared_client(&state).await?;
                let status = client
                    .request(Request::DaemonStatus)
                    .await
                    .map_err(|error| error.to_string())?;
                let sessions = client
                    .request(Request::ListSessions)
                    .await
                    .map_err(|error| error.to_string())?;
                let active = state
                    .lock()
                    .map_err(|_| "desktop state lock poisoned".to_string())?
                    .active_session();
                let snapshot = match active {
                    Some(id) => Some(shared_snapshot(&state, &id).await?.1),
                    None => None,
                };
                Ok::<_, String>((status, sessions, snapshot))
            })
        });
        match result {
            Ok((status, sessions, snapshot)) => {
                let detail = match status {
                    Response::Status(status) => format!(
                        "pid {} · {} active session{} · {} active turn{}",
                        status.pid,
                        status.active_sessions,
                        if status.active_sessions == 1 { "" } else { "s" },
                        status.active_turns,
                        if status.active_turns == 1 { "" } else { "s" },
                    ),
                    _ => "daemon returned an unexpected status".into(),
                };
                let rows = match sessions {
                    Response::Sessions(sessions) => {
                        let mut rows: Vec<_> = sessions.iter().map(format_session).collect();
                        rows.sort_by(|a, b| {
                            (a.host.as_str(), a.workspace.as_str())
                                .cmp(&(b.host.as_str(), b.workspace.as_str()))
                        });
                        rows
                    }
                    _ => Vec::new(),
                };
                invoke(weak, move |ui| {
                    ui.set_connection_status("Daemon connected".into());
                    ui.set_daemon_detail(detail.into());
                    sidebar::set_sessions(ui, rows);
                    if let Some(snapshot) = snapshot {
                        show_snapshot(ui, &state, snapshot);
                    }
                });
            }
            Err(error) => invoke(weak, move |ui| {
                ui.set_connection_status("Daemon unavailable".into());
                ui.set_notice(error.into());
            }),
        }
    });
}

fn refresh_active(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else { return };
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (_, snapshot) = shared_snapshot(&state, &active).await?;
                Ok(snapshot)
            })
        });
        match result {
            Ok(snapshot) => invoke(weak, move |ui| show_snapshot(ui, &state, snapshot)),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

pub fn run() -> Result<(), slint::PlatformError> {
    let ui = MainWindow::new()?;
    #[cfg(target_os = "macos")]
    {
        ui.set_native_titlebar_overlay(true);
        macos_window::install(&ui);
    }
    let state = Arc::new(Mutex::new(DesktopState::default()));
    command_ui::bind(&ui, state.clone());
    memory::bind(&ui, state.clone());
    settings::bind(&ui, state.clone());
    composer::bind(&ui, state.clone());
    tool_actions::bind(&ui, state.clone());
    let workspace = std::env::current_dir()
        .unwrap_or_default()
        .display()
        .to_string();
    ui.set_workspace(workspace.clone().into());
    ui.set_new_workspace(workspace.into());
    ui.set_model_options(ModelRc::new(VecModel::from(model_options())));
    ui.set_ssh_hosts(ModelRc::new(VecModel::from(saved_ssh_rows())));
    if let Ok(settings) = UserSettings::load() {
        ui.set_theme_name(
            settings
                .theme
                .clone()
                .unwrap_or_else(|| "firmius".into())
                .into(),
        );
        if settings.onboarding.completed_version < firmius_core::ONBOARDING_VERSION {
            ui.set_onboarding_open(true);
        }
        if let Some(model) = settings.preferred_default_model() {
            ui.set_new_provider(model.provider_id.clone().into());
            ui.set_new_model(model.model.clone().into());
        }
    }

    navigation::bind(&ui, state.clone());
    model_picker::bind(&ui, state.clone());
    permission_inbox::bind(&ui, state.clone());
    view_actions::bind(&ui, state.clone());
    work_actions::bind(&ui, state.clone());
    dispatch::bind(&ui, state.clone());
    session_actions::bind(&ui, state.clone());
    remote_workspaces::bind(&ui, state.clone());
    sidebar::bind(&ui, state.clone());
    if let Some(path) =
        std::env::args().find_map(|arg| arg.strip_prefix("--preview=").map(str::to_owned))
    {
        return preview::run(&ui, &path, state.clone());
    }
    if let Some(shell) = storage::load() {
        let mut needs_welcome = false;
        if let Ok(mut s) = state.lock() {
            s.shell = shell;
            needs_welcome = s.shell.active().is_none();
            if !needs_welcome {
                navigation::restore(&ui, &mut s);
            }
        }
        if needs_welcome {
            navigation::new_task(&ui, &state);
        }
    } else {
        navigation::new_task(&ui, &state);
    }
    if std::env::args().any(|arg| arg == "--gallery") {
        ui.invoke_set_view("gallery".into());
        return ui.run();
    }
    refresh(&ui, state.clone());
    monitor_active_session(ui.as_weak(), state.clone());
    let result = ui.run();

    result
}

#[cfg(test)]
mod tests {
    use crate::presentation::{
        diff_preview, markdown_preview, tool_args_preview, tool_call_preview, workflow_prompt,
    };

    #[test]
    fn markdown_preview_preserves_structure_without_raw_markers() {
        let rendered = markdown_preview("# Heading\n\n- **item**\n\n```rust\nlet x = 1;\n```");
        assert!(rendered.contains("▌ Heading"));
        assert!(rendered.contains("• item"));
        assert!(rendered.contains("│ let x = 1;"));
        assert!(!rendered.contains("**"));
    }

    #[test]
    fn tool_args_preview_pretty_prints_json() {
        let rendered = tool_args_preview(r#"{"path":"src/main.rs","line":3}"#);
        assert!(rendered.contains("\"path\": \"src/main.rs\""));
        assert!(rendered.contains("\n"));
    }

    #[test]
    fn tool_call_preview_specializes_delegate_metadata() {
        let (body, detail) = tool_call_preview(
            "delegate",
            r#"{"persona":"reviewer","prompt":"Inspect the patch","workdir":"/tmp/project"}"#,
        );
        assert_eq!(body, "Inspect the patch");
        assert!(detail.contains("reviewer"));
        assert!(detail.contains("/tmp/project"));
    }

    #[test]
    fn diff_preview_extracts_path_and_patch() {
        let (preview, path) =
            diff_preview(r#"{"path":"src/lib.rs","patch":"@@ -1 +1 @@\n-old\n+new"}"#);
        assert_eq!(path, "src/lib.rs");
        assert!(preview.contains("+new"));
    }

    #[test]
    fn workflow_prompt_keeps_brief_and_ordered_steps() {
        let prompt = workflow_prompt(
            "Release",
            "Keep the quality bar high",
            &["Inspect".into(), "Implement".into(), "Verify".into()],
        );
        assert!(prompt.contains("\"Release\""));
        assert!(prompt.contains("Keep the quality bar high"));
        assert!(prompt.contains("1. Inspect\n2. Implement\n3. Verify"));
    }
}
