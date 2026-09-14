//! The TUI proper: terminal lifecycle, the event loop, and async side
//! effects (prompt tasks, session saves, background-count refreshes).

pub mod clipboard;
pub mod command;
pub mod composer;
pub mod event;
pub mod markdown;
pub mod modal;
pub mod model;
pub mod present;
pub mod presentation;
pub mod run;
pub mod runtime_state;
pub mod settings;
pub mod style;
pub mod theme;
pub mod todo;
pub mod view;
pub mod work;
pub mod workflow;

use std::io;
use std::sync::Arc;
use std::time::{Duration, Instant};

use chrono::Utc;
use crossterm::ExecutableCommand;
use crossterm::event::{
    DisableBracketedPaste, DisableMouseCapture, EnableBracketedPaste, EnableMouseCapture,
    Event as TermEvent, KeyCode, KeyEvent, KeyEventKind, KeyModifiers,
};
use crossterm::terminal::{
    EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode,
};
use firmius_client::DaemonClient;
use firmius_core::{
    AccountRecord, Agent, GoalCheck, McpManager, McpServerConfig, PersonaManager, ProcStatus,
    ProviderManager, Session, SessionHandle, UserSettings, register_tool_specs,
    unregister_tool_specs,
};
use firmius_protocol::{
    CancelGoalRequest, CheckGoalRequest, CreateGoalRequest, CreateSessionRequest, GetGoalRequest,
    GoalActor, GoalOwner, GoalProvenance, GoalRequest, GoalResponse, GoalSource, ListGoalsRequest,
    McpCommand, MemoryOperationResponse, MemoryRetrieveRequest, MemoryScopeDto, MemoryViewDto,
    Request as DaemonRequest, Response as DaemonResponse, SessionSnapshot, SetModelRequest,
    SetPersonaRequest, SubmitTurnRequest,
};
use ratatui::Terminal;
use ratatui::backend::CrosstermBackend;
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;

use command::{McpAction, McpTransportSpec};
use event::AppEvent;
use modal::{
    AccountRow, AccountsModal, CommandPalette, KindPickerModal, ModalAction, OnboardingModal,
    PermissionGateDeck, PermissionsPolicyDeck, PersonasModal, SettingsModal, WizardModal,
    WorkflowPicker,
};
use model::{Action, Item, Model, items_from_history};
use settings::{CompactionSection, GeneralSection, RetrySection, SettingsSection};

fn session_workdir() -> Option<String> {
    std::env::var("FIRMIUS_REMOTE_WORKSPACE").ok().or_else(|| {
        std::env::current_dir()
            .ok()
            .map(|path| path.to_string_lossy().into_owned())
    })
}

/// Start one coalesced remote housekeeping request. All daemon I/O happens in
/// this task; only the compact result is sent back through the UI event queue.
fn spawn_remote_refresh(model: &mut Model, tx: mpsc::Sender<AppEvent>) {
    let Some(daemon) = model.daemon.clone() else {
        return;
    };
    let epoch = daemon.endpoint().epoch;
    let focused_id = model.focused_id.clone();
    let offsets = model.host_tail_state.clone();
    model.remote_refresh_in_flight = true;
    tokio::spawn(async move {
        let result = async {
            let response = tokio::time::timeout(
                Duration::from_secs(5),
                daemon.request(DaemonRequest::Snapshot),
            )
            .await
            .map_err(|_| "snapshot timed out".to_string())?
            .map_err(|error| error.to_string())?;
            let snapshot = match response {
                DaemonResponse::Snapshot(snapshot) => snapshot,
                _ => return Err("daemon refresh returned an invalid snapshot".into()),
            };
            let process_ids = snapshot
                .agents
                .iter()
                .find(|agent| agent.record.id == focused_id)
                .map(|agent| {
                    agent
                        .processes
                        .iter()
                        .map(|process| process.id)
                        .collect::<Vec<_>>()
                })
                .unwrap_or_default();
            let mut peeks = Vec::with_capacity(process_ids.len());
            for proc_id in process_ids {
                let since = offsets.get(&proc_id).map(|state| state.offset).unwrap_or(0);
                let response = tokio::time::timeout(
                    Duration::from_secs(5),
                    daemon.request(DaemonRequest::HostPeek {
                        agent_id: focused_id.clone(),
                        proc_id,
                        since,
                    }),
                )
                .await
                .map_err(|_| "host output refresh timed out".to_string())?
                .map_err(|error| error.to_string())?;
                if let DaemonResponse::HostPeek(peek) = response {
                    peeks.push((proc_id, peek));
                }
            }
            Ok((snapshot, peeks))
        }
        .await;
        let _ = tx
            .send(AppEvent::RemoteRefresh {
                epoch,
                snapshot: result,
            })
            .await;
    });
}

pub async fn run(
    session: Option<SessionHandle>,
    primary: Option<Arc<Agent>>,
    provider_id: String,
    model_name: String,
    tools: Arc<firmius_core::ToolRegistry>,
    manager: Arc<std::sync::Mutex<ProviderManager>>,
    personas: Arc<PersonaManager>,
    settings: Arc<std::sync::Mutex<UserSettings>>,
    config: Arc<std::sync::Mutex<firmius_core::FirmiusConfig>>,
    mcp: Arc<McpManager>,
    daemon: Option<(
        DaemonClient,
        Option<firmius_protocol::SessionSnapshot>,
        tokio::sync::broadcast::Receiver<firmius_protocol::DaemonEvent>,
    )>,
) -> Result<(), String> {
    // Panic hook: restore the terminal before printing, or the backtrace
    // lands on a raw-mode alternate screen nobody can read.
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let _ = disable_raw_mode();
        let _ = io::stdout().execute(DisableMouseCapture);
        let _ = io::stdout().execute(LeaveAlternateScreen);
        default_hook(info);
    }));

    enable_raw_mode().map_err(|e| e.to_string())?;
    io::stdout()
        .execute(EnterAlternateScreen)
        .and_then(|s| s.execute(EnableBracketedPaste))
        .and_then(|s| s.execute(EnableMouseCapture))
        .map_err(|e| e.to_string())?;

    let backend = CrosstermBackend::new(io::stdout());
    let mut terminal = Terminal::new(backend).map_err(|e| e.to_string())?;

    // Keep model traffic bounded and keyboard input independent from it. A
    // single unbounded FIFO used to retain streaming deltas indefinitely and
    // leave keystrokes hours behind a large tool call.
    let (tx, mut rx) = mpsc::channel::<AppEvent>(1024);
    let (term_tx, mut term_rx) = mpsc::channel::<TermEvent>(256);
    event::spawn_term_pump(term_tx);
    let mut session = session;
    if let Some(session) = &session {
        let bus_rx = session.subscribe();
        event::spawn_bus_bridge(bus_rx, tx.clone());
    }
    let mut model = Model::new(
        session.clone(),
        primary.clone(),
        provider_id,
        manager.clone(),
        model_name,
        tools,
        personas,
        settings,
        config,
        mcp,
    );
    let daemon_events = daemon.map(|(client, snapshot, events)| {
        model.attach_daemon(client, snapshot);
        events
    });
    if let Some(events) = daemon_events {
        event::spawn_daemon_bridge(events, tx.clone());
    }
    if let Some(daemon) = model.daemon.clone() {
        // The saved policy is available on welcome; only approval routing
        // requires an attached session.
        if model.remote_snapshot.is_some()
            && let Err(error) = daemon.register_permission_approver().await
        {
            model.flash(&format!("permission approver unavailable: {error}"));
        }
        match daemon.permission_policy().await {
            Ok(policy) => model.permission_policy = Some(policy),
            Err(error) => model.flash(&format!("permission policy unavailable: {error}")),
        }
    }
    // OOBE is only constructed inside the interactive TUI path. Resumes and
    // non-interactive commands remain interruption-free.
    if model.primary.is_none() && model.settings.lock().unwrap().needs_onboarding() {
        open_onboarding(&mut model);
    }
    // The subscription above is established before this authoritative read,
    // so any event racing initialization is either folded or causes a later
    // snapshot recovery.
    model.reload_work_snapshot();
    let mut ticks = tokio::time::interval(Duration::from_millis(33));
    ticks.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut quota_ticks = tokio::time::interval(Duration::from_secs(30));
    quota_ticks.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    let outcome: Result<(), String> = loop {
        terminal
            .draw(|f| view::draw(&mut model, f))
            .map_err(|e| e.to_string())?;

        tokio::select! {
            biased;
            // Input wins over ticks and model traffic whenever both are ready.
            incoming = term_rx.recv() => {
                let Some(ev) = incoming else { break Ok(()) };
                let action = handle_event(
                    AppEvent::Term(ev),
                    &mut model,
                    &mut session,
                    &manager,
                    &tx,
                ).await;
                if matches!(action, Action::Quit) { break Ok(()) }
            }
            // Drain application events before periodic ticks. A continuously
            // ready tick can otherwise starve the async session lookup result.
            incoming = rx.recv() => {
                let Some(first) = incoming else { break Ok(()) };
                let mut batch = Vec::with_capacity(128);
                batch.push(first);
                while batch.len() < 128 {
                    match rx.try_recv() {
                        Ok(ev) => batch.push(ev),
                        Err(mpsc::error::TryRecvError::Empty) => break,
                        Err(mpsc::error::TryRecvError::Disconnected) => break,
                    }
                }
                let mut quit = false;
                for ev in batch {
                    if matches!(handle_event(ev, &mut model, &mut session, &manager, &tx).await, Action::Quit) {
                        quit = true;
                        break;
                    }
                }
                if quit { break Ok(()) }
            }
            _ = quota_ticks.tick() => {
                spawn_quota_refresh(&mut model, &tx);
            }
            _ = ticks.tick() => {
                model.update(AppEvent::Tick);
                if model.modal.is_some() {
                    let action = handle_modal_tick(&mut model).await;
                    match action {
                        Action::Quit => break Ok(()),
                        Action::RegisterAccount { record } => {
                            register_account(&mut model, record).await;
                        }
                        _ => {}
                    }
                }
                refresh_async(&mut model, &tx).await;
            }
        }
    };

    // Teardown: leave the alternate screen, then persist.
    let _ = io::stdout()
        .execute(DisableBracketedPaste)
        .and_then(|s| s.execute(DisableMouseCapture))
        .and_then(|s| s.execute(LeaveAlternateScreen));
    let _ = disable_raw_mode();
    if let Some(daemon) = model.daemon.clone() {
        let _ = daemon.unregister_permission_approver().await;
    }
    if let Some(session) = &session
        && let Err(e) = session.save()
    {
        eprintln!("warning: could not save session: {e}");
    }
    outcome
}

fn spawn_session_refresh(tx: mpsc::Sender<AppEvent>) {
    tokio::spawn(async move {
        let workdir = std::env::current_dir().unwrap_or_default();
        let result = tokio::task::spawn_blocking(move || {
            firmius_core::list_sessions_for_workdir(Some(&workdir))
        })
        .await
        .unwrap_or_else(|error| Err(format!("session lookup failed: {error}")));
        let _ = tx.send(AppEvent::Sessions(result)).await;
    });
}

async fn goal_revision(
    daemon: &DaemonClient,
    goal_id: firmius_protocol::GoalId,
) -> Result<u64, String> {
    match daemon
        .request(DaemonRequest::Goal(GoalRequest::Get(GetGoalRequest {
            goal_id,
        })))
        .await
        .map_err(|error| error.to_string())?
    {
        DaemonResponse::Goal(GoalResponse::Retrieved(goal)) => Ok(goal.revision),
        _ => Err("goal API returned an invalid status response".into()),
    }
}

async fn handle_goal_action(model: &mut Model, action: command::GoalAction) {
    let Some(daemon) = model.daemon.clone() else {
        model.flash("goal API requires the daemon");
        return;
    };
    let actor = GoalActor::User {
        user_id: "user".into(),
    };
    let auto_activation_actor = actor.clone();
    let mut auto_activate = false;
    let request = match action {
        command::GoalAction::Create {
            description,
            check,
            max_steps,
            approval,
            auto_activate: activate,
        } => {
            auto_activate = activate;
            let parsed_check = match check {
                Some(command) => match GoalCheck::try_command(command) {
                    Ok(check) => Some(check),
                    Err(error) => {
                        model.flash(&format!("goal failed: invalid check command: {error}"));
                        return;
                    }
                },
                None => None,
            };
            // Goals launch inside the attached daemon session. Without a
            // session the daemon can only record an unlaunched intent, so
            // create one lazily the same way a first submission does.
            if model.remote_snapshot.is_none() {
                let create = CreateSessionRequest {
                    provider_id: model.provider_id.clone(),
                    model: model.model.clone(),
                    effort: model.effort.clone(),
                    // The welcome picker keeps `None` as its neutral UI
                    // selection.  A real interactive session, however,
                    // starts with the Lead workflow unless the user selected
                    // a different main persona.
                    persona: model
                        .pending_persona
                        .clone()
                        .or_else(|| Some("lead".into())),
                    workdir: session_workdir(),
                };
                match daemon.request(DaemonRequest::CreateSession(create)).await {
                    Ok(DaemonResponse::Snapshot(snapshot)) => {
                        model.replace_remote_snapshot(snapshot);
                    }
                    Ok(_) => {
                        model.flash("goal failed: invalid daemon session response");
                        return;
                    }
                    Err(error) => {
                        model.flash(&format!("goal failed: {error}"));
                        return;
                    }
                }
            }
            DaemonRequest::Goal(GoalRequest::Create(CreateGoalRequest {
                description: description.clone(),
                success_conditions: vec![description.clone()],
                owner: GoalOwner::User {
                    user_id: "user".into(),
                },
                provenance: GoalProvenance {
                    actor: actor.clone(),
                    source: GoalSource::ExplicitCommand,
                    created_at: Utc::now(),
                },
                // Default check is an independent reviewer, never the worker.
                // The worker may not grade itself; the daemon runs this check
                // when the worker turn ends and rewakes on failure.
                checks: parsed_check
                    .into_iter()
                    .chain(std::iter::once(GoalCheck::independent_agent(
                        model.primary_id.clone(),
                        vec![description.clone()],
                    )))
                    .collect(),
                deadline: None,
                budget: max_steps.map(|max_steps| firmius_core::GoalBudget {
                    max_cost: None,
                    max_steps: Some(max_steps),
                }),
                approval_required: approval,
                client_request_id: None,
            }))
        }
        command::GoalAction::List => {
            DaemonRequest::Goal(GoalRequest::List(ListGoalsRequest::default()))
        }
        command::GoalAction::Lifecycle { action, goal_id } => {
            let Ok(goal_id) = firmius_protocol::GoalId::parse(&goal_id) else {
                model.flash("goal command failed: invalid goal id");
                return;
            };
            let revision = match goal_revision(&daemon, goal_id).await {
                Ok(value) => value,
                Err(error) => {
                    model.flash(&format!("goal command failed: {error}"));
                    return;
                }
            };
            match action.as_str() {
                "activate" => DaemonRequest::Goal(GoalRequest::Activate(
                    firmius_protocol::ActivateGoalRequest {
                        goal_id,
                        actor: Some(actor),
                        expected_revision: revision,
                        client_request_id: None,
                    },
                )),
                "approve" => DaemonRequest::Goal(GoalRequest::Approve(
                    firmius_protocol::ApproveGoalRequest {
                        goal_id,
                        decision: firmius_protocol::ApprovalDecision::Approve,
                        actor: Some(actor),
                        approval_id: None,
                        reason: None,
                        expected_revision: revision,
                        client_request_id: None,
                    },
                )),
                "reject" => DaemonRequest::Goal(GoalRequest::Approve(
                    firmius_protocol::ApproveGoalRequest {
                        goal_id,
                        decision: firmius_protocol::ApprovalDecision::Reject,
                        actor: Some(actor),
                        approval_id: None,
                        reason: None,
                        expected_revision: revision,
                        client_request_id: None,
                    },
                )),
                _ => unreachable!(),
            }
        }
        command::GoalAction::Status { goal_id } => {
            let Ok(goal_id) = firmius_protocol::GoalId::parse(&goal_id) else {
                model.flash("status failed: invalid goal id");
                return;
            };
            DaemonRequest::Goal(GoalRequest::Get(GetGoalRequest { goal_id }))
        }
        command::GoalAction::Check { goal_id, check_id } => {
            let Ok(goal_id) = firmius_protocol::GoalId::parse(&goal_id) else {
                model.flash("check failed: invalid goal id");
                return;
            };
            let expected_revision = match goal_revision(&daemon, goal_id).await {
                Ok(revision) => revision,
                Err(error) => {
                    model.flash(&format!("check failed: {error}"));
                    return;
                }
            };
            DaemonRequest::Goal(GoalRequest::Check(CheckGoalRequest {
                goal_id,
                check_id,
                actor: Some(actor),
                evaluation: None,
                expected_revision,
                client_request_id: None,
            }))
        }
        command::GoalAction::Cancel { goal_id, reason } => {
            let Ok(goal_id) = firmius_protocol::GoalId::parse(&goal_id) else {
                model.flash("cancel failed: invalid goal id");
                return;
            };
            let expected_revision = match goal_revision(&daemon, goal_id).await {
                Ok(revision) => revision,
                Err(error) => {
                    model.flash(&format!("cancel failed: {error}"));
                    return;
                }
            };
            DaemonRequest::Goal(GoalRequest::Cancel(CancelGoalRequest {
                goal_id,
                actor: Some(actor),
                reason,
                expected_revision,
                client_request_id: None,
            }))
        }
    };
    match daemon.request(request).await {
        Ok(DaemonResponse::Goal(response)) => match response {
            GoalResponse::Created(goal) => {
                push_note(
                    model,
                    format!("goal created: {} · {}", goal.id, goal.description),
                );
                if auto_activate && !goal.approval.required {
                    let _ = daemon
                        .request(DaemonRequest::Goal(GoalRequest::Activate(
                            firmius_protocol::ActivateGoalRequest {
                                goal_id: goal.id,
                                actor: Some(auto_activation_actor),
                                expected_revision: goal.revision,
                                client_request_id: None,
                            },
                        )))
                        .await;
                }
            }
            GoalResponse::Listed { goals, .. } => {
                let text = if goals.is_empty() {
                    "no goals".into()
                } else {
                    goals
                        .into_iter()
                        .map(|goal| {
                            format!("{} · {:?} · {}", goal.id, goal.status, goal.description)
                        })
                        .collect::<Vec<_>>()
                        .join("\n")
                };
                push_note(model, text);
            }
            GoalResponse::Retrieved(goal)
            | GoalResponse::Activated(goal)
            | GoalResponse::Cancelled(goal)
            | GoalResponse::Approved(goal) => {
                push_note(
                    model,
                    format!(
                        "goal {} · {:?} · {}",
                        goal.id, goal.status, goal.description
                    ),
                );
            }
            GoalResponse::Checked { goal, evaluation } => {
                push_note(
                    model,
                    format!(
                        "goal {} · {:?} · check {}",
                        goal.id, goal.status, evaluation.check_id
                    ),
                );
            }
            _ => push_note(model, "goal updated".into()),
        },
        Ok(_) => model.flash("goal request returned an invalid response"),
        Err(error) => model.flash(&format!("goal request failed: {error}")),
    }
}

async fn sync_daemon_preferences(model: &mut Model) {
    let Some(daemon) = model.daemon.clone() else {
        return;
    };
    let settings = model.settings.lock().unwrap().clone();
    if let Err(error) = daemon
        .request(DaemonRequest::UpdateSettings { settings })
        .await
    {
        model.flash(&format!("daemon settings update failed: {error}"));
        return;
    }
    let config = model.config.lock().unwrap().clone();
    if let Err(error) = daemon.request(DaemonRequest::UpdateConfig { config }).await {
        model.flash(&format!("daemon config update failed: {error}"));
    }
}

async fn restore_remote_session(
    model: &mut Model,
    daemon: &DaemonClient,
    previous: Option<SessionSnapshot>,
) {
    let Some(previous) = previous else {
        return;
    };
    match daemon
        .request(DaemonRequest::AttachSession {
            session_id: previous.session_id,
            workdir: None,
        })
        .await
    {
        Ok(DaemonResponse::Snapshot(snapshot)) => model.replace_remote_snapshot(snapshot),
        Ok(_) => model.flash("previous session could not be restored"),
        Err(error) => model.flash(&format!("previous session could not be restored: {error}")),
    }
}

/// Reconnect independently of the rendering loop.  The endpoint is reread by
/// `DaemonClient::reconnect` on every attempt, which handles daemon restarts
/// that publish a new address, token, or epoch.  Attach is deliberately done
/// only after subscribing so no session event can race the authoritative
/// snapshot response.
fn spawn_daemon_reconnect(
    client: DaemonClient,
    session_id: Option<String>,
    tx: mpsc::Sender<AppEvent>,
) {
    tokio::spawn(async move {
        let mut delay = Duration::from_millis(100);
        // Keep trying while the TUI is alive. The delay is capped so a daemon
        // restarted after a long outage is still picked up promptly, without
        // spinning or creating a request storm during the outage.
        loop {
            match client.reconnect().await {
                Ok(next) => {
                    let events = next.subscribe();
                    let snapshot = if let Some(session_id) = session_id.as_deref() {
                        match next
                            .request(DaemonRequest::AttachSession {
                                session_id: session_id.to_string(),
                                workdir: std::env::current_dir()
                                    .ok()
                                    .map(|path| path.to_string_lossy().into_owned()),
                            })
                            .await
                        {
                            Ok(DaemonResponse::Snapshot(snapshot)) => Some(snapshot),
                            Ok(_other) => {
                                tokio::time::sleep(delay).await;
                                delay = (delay * 2).min(Duration::from_secs(2));
                                continue;
                            }
                            Err(_error) => {
                                tokio::time::sleep(delay).await;
                                delay = (delay * 2).min(Duration::from_secs(2));
                                continue;
                            }
                        }
                    } else {
                        None
                    };
                    let _ = tx
                        .send(AppEvent::RemoteReconnected {
                            client: next,
                            snapshot,
                            events,
                        })
                        .await;
                    return;
                }
                Err(_) => {}
            }
            tokio::time::sleep(delay).await;
            delay = (delay * 2).min(Duration::from_secs(2));
        }
    });
}

fn open_onboarding(model: &mut Model) {
    let provider_ready = !model.manager.lock().unwrap().provider_ids().is_empty();
    let install_summary = crate::install::detect().summary();
    model.completion = None;
    model.modal = Some(Box::new(OnboardingModal::new(
        provider_ready,
        install_summary,
        model.settings.clone(),
    )));
}

async fn handle_event(
    ev: AppEvent,
    model: &mut Model,
    session: &mut Option<SessionHandle>,
    manager: &Arc<std::sync::Mutex<ProviderManager>>,
    tx: &mpsc::Sender<AppEvent>,
) -> Action {
    // While a modal is open it owns keyboard and paste input. Ctrl+C stays
    // global, but the background composer must not consume bracketed pastes.
    let modal_action = if model.modal.is_some() {
        match &ev {
            AppEvent::Term(TermEvent::Key(k)) if k.kind != KeyEventKind::Release => {
                Some(handle_modal_key(model, *k).await)
            }
            AppEvent::Term(TermEvent::Paste(text)) => {
                handle_modal_paste(model, text);
                return Action::Continue;
            }
            _ => None,
        }
    } else {
        None
    };
    let action = match modal_action {
        Some(action) => action,
        None => match ev {
            AppEvent::Term(TermEvent::Key(k))
                if k.kind != KeyEventKind::Release
                    && k.code == KeyCode::Char('v')
                    && k.modifiers.contains(KeyModifiers::CONTROL) =>
            {
                handle_clipboard_paste(model);
                Action::Continue
            }
            AppEvent::PermissionRequested(request) => {
                model.update(AppEvent::PermissionRequested(request));
                // Interactive approval is meaningful only for Default/ask
                // mode. Auto and Yolo are resolved in the daemon and must
                // never expose a human approval surface (even if a stale or
                // misrouted event arrives at this client).
                if model.permission_policy.as_ref().is_some_and(|policy| {
                    matches!(policy.mode, firmius_core::PermissionMode::Default)
                }) {
                    if let Some(policy) = model.permission_policy.clone() {
                        if let Some(request) = model.pending_permission.clone() {
                            model.modal = Some(Box::new(PermissionGateDeck::new(policy, request)));
                        }
                    }
                }
                // The request gate is a distinct approval surface. Returning
                // Continue prevents the generic OpenPermissions action below
                // from replacing it with the policy editor.
                Action::Continue
            }
            AppEvent::PermissionResolved {
                request_id,
                decision,
            } => {
                // A resolver timeout, disconnect, or another approver can
                // settle a request while its modal is still visible.  Close
                // that stale surface instead of leaving controls that would
                // submit an already-invalid nonce/digest.
                let action = model.update(AppEvent::PermissionResolved {
                    request_id,
                    decision,
                });
                if model
                    .pending_permission
                    .as_ref()
                    .is_none_or(|request| request.request_id == request_id)
                {
                    model.modal = None;
                }
                action
            }
            AppEvent::Sessions(result) => {
                model.apply_session_summaries(result.clone());
                Action::Continue
            }
            AppEvent::RemoteRefresh { epoch, snapshot } => {
                model.remote_refresh_in_flight = false;
                // A reconnect may replace the daemon while an older refresh
                // is still in flight. Never apply that stale response.
                let Some(daemon) = model.daemon.as_ref() else {
                    return Action::Continue;
                };
                if daemon.endpoint().epoch != epoch {
                    return Action::Continue;
                }
                match snapshot {
                    Ok((snapshot, peeks)) => {
                        model.refresh_remote_snapshot(snapshot, true);
                        for (proc_id, peek) in peeks {
                            let state = model.host_tail_state.entry(proc_id).or_default();
                            state.offset = peek.total;
                            state.push(&peek.bytes);
                            model.host_tails.insert(proc_id, state.tail());
                        }
                    }
                    Err(error) => model.flash(&format!("daemon refresh failed: {error}")),
                }
                Action::Continue
            }
            AppEvent::RemoteStatus(status) => {
                model.apply_remote_status(status);
                Action::Continue
            }
            AppEvent::Bus(event) => {
                let todo_changed = matches!(
                    &event.payload,
                    firmius_core::SessionEventPayload::Todo { .. }
                );
                let action = model.update(AppEvent::Bus(event));
                // Older daemons and a lagged dedicated Todo stream still
                // deliver the committed session event. Reload the typed
                // snapshot so the rail cannot remain empty indefinitely.
                if todo_changed && model.daemon.is_some() && !model.remote_refresh_in_flight {
                    spawn_remote_refresh(model, tx.clone());
                }
                action
            }
            ev @ AppEvent::BusLagged(n) => {
                model.flash(&format!("bus lagged ({n} events) — rebuilding"));
                model.update(ev)
            }
            AppEvent::RemoteDisconnected(reason) => {
                let action = model.update(AppEvent::RemoteDisconnected(reason.clone()));
                if !model.reconnect_in_progress {
                    if let Some(client) = model.daemon.clone() {
                        // Do not let periodic snapshot refreshes continue to
                        // write to a dead socket while the reconnect task is
                        // working. The last remote snapshot remains intact.
                        model.daemon = None;
                        model.reconnect_in_progress = true;
                        let session_id = model
                            .remote_snapshot
                            .as_ref()
                            .map(|snapshot| snapshot.session_id.clone());
                        spawn_daemon_reconnect(client, session_id, tx.clone());
                    }
                }
                action
            }
            AppEvent::RemoteShutdown => {
                model.daemon = None;
                model.reconnect_in_progress = false;
                model.update(AppEvent::RemoteDisconnected(
                    "daemon is shutting down".into(),
                ))
            }
            AppEvent::RemoteReconnected {
                client,
                snapshot,
                events,
            } => {
                model.attach_daemon(client, snapshot);
                model.reconnect_in_progress = false;
                if let Some(daemon) = model.daemon.clone() {
                    if let Err(error) = daemon.register_permission_approver().await {
                        model.flash(&format!("permission approver unavailable: {error}"));
                    }
                    match daemon.permission_policy().await {
                        Ok(policy) => model.permission_policy = Some(policy),
                        Err(error) => {
                            model.flash(&format!("permission policy unavailable: {error}"))
                        }
                    }
                }
                event::spawn_daemon_bridge(events, tx.clone());
                model.flash("daemon reconnected");
                Action::Continue
            }
            AppEvent::TurnDone(res) => {
                let action = model.update(AppEvent::TurnDone(res));
                if let Some(session) = session
                    && let Err(e) = session.save()
                {
                    model.flash(&format!("save failed: {e}"));
                }
                action
            }
            other => model.update(other),
        },
    };

    match action {
        Action::OpenPermissions => {
            model.completion = None;
            if let Some(policy) = model.permission_policy.clone() {
                model.modal = Some(Box::new(PermissionsPolicyDeck::new(
                    policy,
                    model.permission_activity.clone(),
                )));
            } else {
                model.flash("permission policy unavailable");
            }
        }
        Action::MemoryQuery(query) => {
            let Some(daemon) = model.daemon.clone() else {
                model.flash("memory search requires the daemon");
                return Action::Continue;
            };
            let Some(snapshot) = model.remote_snapshot.as_ref() else {
                model.flash("memory search requires an attached session");
                return Action::Continue;
            };
            let project_id = snapshot
                .agents
                .iter()
                .find(|agent| agent.record.id == snapshot.primary_agent_id)
                .map(|agent| {
                    firmius_core::memory::resolve_project_identity(&agent.record.workdir).project_id
                });
            match daemon
                .retrieve_memory(MemoryRetrieveRequest {
                    query: query.clone(),
                    view: MemoryViewDto {
                        include_user: true,
                        project_id,
                        session_id: Some(snapshot.session_id.clone()),
                    },
                    limit: 20,
                })
                .await
            {
                Ok(response) => match response.result {
                    MemoryOperationResponse::Retrieved { hits } => {
                        let text = if hits.is_empty() {
                            format!("MEMORY · {query}\nNo durable memories matched this workspace.")
                        } else {
                            let rows = hits
                                .into_iter()
                                .map(|hit| {
                                    let scope = match hit.record.scope {
                                        MemoryScopeDto::User => "user",
                                        MemoryScopeDto::Project { .. } => "project",
                                        MemoryScopeDto::Session { .. } => "session",
                                    };
                                    let evidence = if hit.record.evidence.is_empty() {
                                        "No retained evidence excerpt.".to_string()
                                    } else {
                                        hit.record
                                            .evidence
                                            .iter()
                                            .map(|evidence| {
                                                let locator = evidence
                                                    .locator
                                                    .as_deref()
                                                    .map(|locator| format!(" · {locator}"))
                                                    .unwrap_or_default();
                                                format!(
                                                    "- {}{}: {}",
                                                    evidence.kind, locator, evidence.excerpt
                                                )
                                            })
                                            .collect::<Vec<_>>()
                                            .join("\n")
                                    };
                                    let tags = if hit.record.tags.is_empty() {
                                        "untagged".to_string()
                                    } else {
                                        hit.record.tags.join(", ")
                                    };
                                    format!(
                                        "[{} · active · relevance {:.2} · confidence {:.0}% · v{}]\n{}\n{}\n\nEvidence\n{}\n\nTags: {}\nRecord: #{}",
                                        scope,
                                        hit.score,
                                        hit.record.confidence * 100.0,
                                        hit.record.record_version,
                                        hit.record.title,
                                        hit.record.body,
                                        evidence,
                                        tags,
                                        hit.record.id
                                    )
                                })
                                .collect::<Vec<_>>()
                                .join("\n\n");
                            format!(
                                "MEMORY · {}\nTreat these as potentially stale evidence; current instructions and verification win.\n\n{}",
                                query, rows
                            )
                        };
                        model
                            .transcripts
                            .entry(model.primary_id.clone())
                            .or_default()
                            .push(Item::Note(text));
                    }
                    _ => model.flash("memory search failed: invalid daemon response"),
                },
                Err(error) => model.flash(&format!("memory search failed: {error}")),
            }
        }
        Action::ResolvePermission { resolution } => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon.resolve_permission(resolution).await {
                    Ok(()) => {
                        model.pending_permission = None;
                        model.modal = None;
                    }
                    Err(error) => model.flash(&format!("permission response failed: {error}")),
                }
            }
        }
        Action::SetPermissionPolicy {
            policy,
            expected_revision,
        } => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon
                    .update_permission_policy(policy, expected_revision)
                    .await
                {
                    Ok(updated) => {
                        model.permission_policy = Some(updated);
                        model.modal = None;
                        model.flash("permission policy updated");
                    }
                    Err(error) => model.flash(&format!("permission policy update failed: {error}")),
                }
            } else if let Some(session) = model.session.clone() {
                let current = session.permission_broker.policy();
                if current.revision != expected_revision {
                    model.flash("permission policy update failed: stale revision");
                } else {
                    let mut updated = policy;
                    updated.revision = expected_revision.saturating_add(1);
                    session.permission_broker.set_policy(updated.clone());
                    model.permission_policy = Some(updated);
                    model.modal = None;
                    model.flash("permission policy updated");
                }
            }
        }
        Action::OpenLogin { kind } => {
            open_login(model, kind).await;
        }
        Action::OpenAccounts { provider } => {
            open_accounts(model, provider).await;
        }
        Action::OpenPersonas => {
            model.completion = None;
            model.modal = Some(Box::new(PersonasModal::new(
                model.personas.list(),
                model.settings.clone(),
                model.manager.clone(),
            )));
        }
        Action::OpenOnboarding => open_onboarding(model),
        Action::BeginOnboardingTour => {
            model
                .transcripts
                .entry(model.primary_id.clone())
                .or_default()
                .push(Item::Note(
                    "THE 30-SECOND WORKFLOW\n\n1. Describe an outcome, not a sequence of keystrokes.\n2. Firmius turns large work into a durable task graph.\n3. Focused agents can execute independent nodes in parallel.\n4. Agents can message their parent, peers, siblings, or the fleet while they work.\n5. Review gates verify the result before the graph closes.\n6. You can inspect, interrupt, resume, and audit the whole run.\n\nTry it: describe a real goal below. For a large task, ask Firmius to plan and manage the workflow.".into(),
                ));
            model.composer.replace_text(
                "Plan a workflow for this goal, show me the graph, then wait for approval: ",
            );
            model.clear_render_cache();
            model.flash("tour loaded · replace the placeholder with your goal");
        }
        Action::OpenCommandPalette => {
            model.completion = None;
            model.modal = Some(Box::new(CommandPalette::new()));
        }
        Action::OpenWorkflowPicker => {
            model.completion = None;
            model.modal = Some(Box::new(WorkflowPicker::new()));
        }
        Action::InsertCommand(command) => {
            model.modal = None;
            model.composer.replace_text(&command);
            model.refresh_completion();
        }
        Action::LoadWorkflow { path, run } => match workflow::read(&path) {
            Ok(content) => {
                model.composer.replace_text(&content);
                model.refresh_completion();
                if run {
                    let action = model.submit_loaded_workflow();
                    if !matches!(action, Action::Continue) {
                        return action;
                    }
                } else {
                    model.flash(&format!("workflow loaded · {}", path));
                }
            }
            Err(error) => model.flash(&error),
        },
        Action::NewSession => {
            if model.busy {
                model.flash("busy — wait for the turn to finish");
            } else if let Some(daemon) = model.daemon.clone()
                && model.remote_snapshot.is_some()
            {
                match daemon.request(DaemonRequest::SaveSession).await {
                    Ok(DaemonResponse::Ack) => {
                        let _ = daemon.request(DaemonRequest::DetachSession).await;
                        model.reset_to_welcome();
                        model.flash("saved · new session");
                    }
                    Ok(_) => model.flash("save failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("save failed: {error}")),
                }
            } else if let Some(current) = session.take() {
                if let Err(e) = current.save() {
                    model.flash(&format!("save failed: {e}"));
                    *session = Some(current);
                } else {
                    model.reset_to_welcome();
                    model.flash("saved · new session");
                }
            } else {
                model.reset_to_welcome();
                model.flash("new session");
            }
        }
        Action::OpenRemoteSession { workspace } => {
            let Some(daemon) = model.daemon.clone() else {
                model.flash("remote SSH sessions require the daemon");
                return Action::Continue;
            };
            let previous = model.remote_snapshot.clone();
            if model.remote_snapshot.is_some() {
                match daemon.request(DaemonRequest::SaveSession).await {
                    Ok(DaemonResponse::Ack) => {
                        if let Err(error) = daemon.request(DaemonRequest::DetachSession).await {
                            model.flash(&format!("could not close current session: {error}"));
                            return Action::Continue;
                        }
                    }
                    Ok(_) => {
                        model.flash("could not close current session: invalid save response");
                        return Action::Continue;
                    }
                    Err(error) => {
                        model.flash(&format!("could not save current session: {error}"));
                        return Action::Continue;
                    }
                }
            }
            let create = CreateSessionRequest {
                provider_id: model.provider_id.clone(),
                model: model.model.clone(),
                effort: model.effort.clone(),
                persona: model
                    .pending_persona
                    .clone()
                    .or_else(|| Some("lead".into())),
                workdir: Some(workspace.clone()),
            };
            match daemon.request(DaemonRequest::CreateSession(create)).await {
                Ok(DaemonResponse::Snapshot(snapshot)) => {
                    model.reset_to_welcome();
                    model.replace_remote_snapshot(snapshot);
                    if let Err(error) = daemon.register_permission_approver().await {
                        model.flash(&format!("permission approver unavailable: {error}"));
                    }
                    match daemon.permission_policy().await {
                        Ok(policy) => model.permission_policy = Some(policy),
                        Err(error) => {
                            model.flash(&format!("permission policy unavailable: {error}"))
                        }
                    }
                    model.flash(&format!("remote session: {workspace}"));
                }
                Ok(_) => {
                    model.flash("remote session failed: invalid daemon response");
                    restore_remote_session(model, &daemon, previous).await;
                }
                Err(error) => {
                    model.flash(&format!("remote session failed: {error}"));
                    restore_remote_session(model, &daemon, previous).await;
                }
            }
        }
        Action::CopyText(text) => match clipboard::write_clipboard_text(&text) {
            Ok(()) => model.flash("copied"),
            Err(error) => model.flash(&format!("copy failed: {error}")),
        },
        Action::OpenSettings => {
            model.completion = None;
            // Scope keys the Retry tab can target for per-provider overrides:
            // every provider id plus every account-kind name, deduplicated.
            let mut scopes: Vec<String> = Vec::new();
            {
                let manager = model.manager.lock().unwrap();
                scopes.extend(manager.provider_ids().iter().map(|id| id.to_string()));
                scopes.extend(manager.kinds().iter().map(|kind| kind.name().to_string()));
            }
            let sections: Vec<Box<dyn SettingsSection>> = vec![
                Box::new(RetrySection::new(scopes)),
                Box::new(GeneralSection),
                Box::new(CompactionSection),
            ];
            model.modal = Some(Box::new(SettingsModal::new(
                sections,
                model.config.clone(),
                Some(model.manager.clone()),
            )));
        }
        Action::RegisterAccount { record } => {
            register_account(model, record).await;
        }
        Action::Mcp(action) => {
            handle_mcp_action(model, action).await;
        }
        Action::Goal(action) => {
            handle_goal_action(model, action).await;
        }
        Action::RebuildTranscripts => {
            if model.daemon.is_some() {
                refresh_remote_snapshot(model, false).await;
                return Action::Continue;
            }
            if let Some(session) = session {
                for agent in session.agents.read().unwrap().values() {
                    model
                        .transcripts
                        .insert(agent.id.clone(), items_from_history(&agent.history()));
                }
                model.reload_work_snapshot();
            }
        }
        Action::Save => {
            if let Some(daemon) = model.daemon.clone()
                && model.remote_snapshot.is_some()
            {
                match daemon.request(DaemonRequest::SaveSession).await {
                    Ok(DaemonResponse::Ack) => model.flash("session saved"),
                    Ok(_) => model.flash("save failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("save failed: {error}")),
                }
            } else if let Some(session) = session {
                if let Err(e) = session.save() {
                    model.flash(&format!("save failed: {e}"));
                } else {
                    model.flash("session saved");
                }
            } else {
                model.flash("no active session");
            }
        }
        Action::UpdateCheck => {
            let result = async {
                let client = reqwest::Client::builder()
                    .user_agent(format!("firmius/{}", env!("CARGO_PKG_VERSION")))
                    .build()
                    .map_err(|e| e.to_string())?;
                let value: serde_json::Value = client
                    .get("https://api.github.com/repos/9nunya/Firmius/releases/latest")
                    .send()
                    .await
                    .map_err(|e| e.to_string())?
                    .error_for_status()
                    .map_err(|e| e.to_string())?
                    .json()
                    .await
                    .map_err(|e| e.to_string())?;
                Ok::<String, String>(
                    value
                        .get("tag_name")
                        .and_then(|v| v.as_str())
                        .unwrap_or("unknown")
                        .to_string(),
                )
            }
            .await;
            match result {
                Ok(version) => model.flash(&format!("latest Firmius release: {version}")),
                Err(error) => model.flash(&format!("update check failed: {error}")),
            }
        }
        Action::Resume(requested_id) => {
            if model.busy {
                model.flash("busy — wait for the turn to finish");
                return Action::Continue;
            }
            let current_workdir = std::env::current_dir().unwrap_or_default();
            let id = requested_id.or_else(|| {
                firmius_core::list_sessions_for_workdir(Some(&current_workdir))
                    .ok()
                    .and_then(|items| items.first().map(|item| item.id.clone()))
            });
            let Some(id) = id else {
                model.flash("no saved sessions");
                return Action::Continue;
            };
            if let Some(daemon) = model.daemon.clone() {
                match daemon
                    .request(DaemonRequest::AttachSession {
                        session_id: id.clone(),
                        workdir: Some(current_workdir.to_string_lossy().into_owned()),
                    })
                    .await
                {
                    Ok(DaemonResponse::Snapshot(snapshot)) => {
                        model.replace_remote_snapshot(snapshot);
                        model.flash(&format!("resumed {id}"));
                        if let Err(error) = daemon.register_permission_approver().await {
                            model.flash(&format!("permission approver unavailable: {error}"));
                        }
                        match daemon.permission_policy().await {
                            Ok(policy) => model.permission_policy = Some(policy),
                            Err(error) => {
                                model.flash(&format!("permission policy unavailable: {error}"))
                            }
                        }
                    }
                    Ok(_) => model.flash("resume failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("resume failed: {error}")),
                }
                return Action::Continue;
            }
            let resumed = {
                let manager = manager.lock().unwrap();
                let workdir = std::env::current_dir().unwrap_or_default();
                match firmius_core::load_session_record(&id) {
                    Ok(record) if firmius_core::session_matches_workdir(&record, &workdir) => {
                        Session::resume_with_personas(
                            &id,
                            &manager,
                            model.tools.clone(),
                            model.personas.clone(),
                        )
                    }
                    Ok(_) => Err("session does not belong to the current workdir".into()),
                    Err(error) => Err(error),
                }
            };
            match resumed {
                Ok(next) => {
                    let next = next.into_handle();
                    let (next_agent, next_bus) = {
                        for agent in next.agents.read().unwrap().values() {
                            agent.attach_runtime(model.manager.clone(), model.settings.clone());
                            agent.attach_firmius_config(model.config.clone());
                        }
                        (
                            next.agents.read().unwrap().values().next().cloned(),
                            next.subscribe(),
                        )
                    };
                    let Some(next_agent) = next_agent else {
                        model.flash("saved session has no available agents");
                        return Action::Continue;
                    };
                    event::spawn_bus_bridge(next_bus, tx.clone());
                    model.replace_session(next.clone(), next_agent);
                    model.update(AppEvent::WorkRecovery);
                    *session = Some(next);
                    model.flash(&format!("resumed {id}"));
                }
                Err(e) => model.flash(&format!("resume failed: {e}")),
            }
        }
        Action::Submit {
            agent_id,
            message,
            token,
        } => {
            bridge_model_session(model, session, tx).await;
            let agent = if agent_id == model.primary_id {
                model.primary.clone()
            } else if let Some(current) = session {
                current.agent(&agent_id)
            } else {
                None
            };
            let Some(agent) = agent else {
                model.flash("no active model");
                return Action::Continue;
            };
            // A welcome-screen submission creates its session lazily inside
            // `Model::ensure_started`. The event loop's session slot was
            // still empty, so no bus bridge existed and the prompt's
            // response events were invisible even though the request
            // completed successfully. Subscribe before spawning the prompt.
            let tx2 = tx.clone();
            tokio::spawn(async move {
                let res = agent.prompt_message_or_submit(message, token, |_| {}).await;
                let _ = tx2
                    .send(AppEvent::TurnDone(
                        res.map(|_| ()).map_err(|e| e.to_string()),
                    ))
                    .await;
            });
        }
        Action::SubmitRemote { agent_id, message } => {
            let Some(daemon) = model.daemon.clone() else {
                model.flash("daemon client is unavailable");
                return Action::Continue;
            };
            let agent_id = match agent_id {
                Some(agent_id) => agent_id,
                None => {
                    let create = CreateSessionRequest {
                        provider_id: model.provider_id.clone(),
                        model: model.model.clone(),
                        effort: model.effort.clone(),
                        persona: model
                            .pending_persona
                            .clone()
                            .or_else(|| Some("lead".into())),
                        workdir: session_workdir(),
                    };
                    match daemon.request(DaemonRequest::CreateSession(create)).await {
                        Ok(DaemonResponse::Snapshot(snapshot)) => {
                            let agent_id = snapshot.primary_agent_id.clone();
                            model.replace_remote_snapshot(snapshot);
                            // Approval routing becomes available once the welcome
                            // connection has created its first session.
                            if let Err(error) = daemon.register_permission_approver().await {
                                model.flash(&format!("permission approver unavailable: {error}"));
                            }
                            match daemon.permission_policy().await {
                                Ok(policy) => model.permission_policy = Some(policy),
                                Err(error) => {
                                    model.flash(&format!("permission policy unavailable: {error}"))
                                }
                            }
                            agent_id
                        }
                        Ok(_) => {
                            model.update(AppEvent::TurnDone(Err(
                                "daemon returned an invalid create response".into(),
                            )));
                            return Action::Continue;
                        }
                        Err(error) => {
                            model.update(AppEvent::TurnDone(Err(error.to_string())));
                            return Action::Continue;
                        }
                    }
                }
            };
            // Keep a copy for the busy fallback: the primary request takes
            // ownership of the message, and a busy rejection must not lose
            // the user's input.
            let fallback = message.clone();
            match daemon
                .request(DaemonRequest::SubmitTurn(SubmitTurnRequest {
                    agent_id: agent_id.clone(),
                    message,
                }))
                .await
            {
                Ok(DaemonResponse::TurnAccepted { turn_id, acceptance_sequence }) => {
                    // The daemon owns the turn now: the local submission
                    // intent is accounted for and the authoritative turn id
                    // drives cancel/completion from here on.
                    model.local_turn_intent.remove(&agent_id);
                    model.draft_before_remote_submit = None;
                    // A status tick queued before the acknowledgement may
                    // still describe this agent as idle. Fence that status at
                    // the sequence observed when the turn was accepted; a
                    // strictly newer idle status is allowed to settle it.
                    // Older daemons cannot supply a cross-stream boundary; retain
                    // their acknowledgement until an explicit completion instead.
                    let sequence = acceptance_sequence.unwrap_or(u64::MAX);
                    if let Some(snapshot) = model.remote_snapshot.as_ref() {
                        model.acknowledged_remote_turn = Some(crate::tui::model::AcknowledgedRemoteTurn {
                            turn_id,
                            sequence,
                            epoch: model.daemon.as_ref().map(|d| d.endpoint().epoch),
                            session_id: snapshot.session_id.clone(),
                            agent_id: agent_id.clone(),
                        });
                    }
                    model.remote_turn_id = Some(turn_id);
                }
                Ok(_) => {
                    model.update(AppEvent::TurnDone(Err(
                        "daemon returned an invalid turn response".into(),
                    )));
                }
                Err(error) => {
                    // A busy rejection means another turn already owns the
                    // agent (e.g. a goal or mailbox turn started between the
                    // client's idle probe and the request). The message is
                    // still wanted: deliver it through the mailbox instead
                    // of discarding it, which is exactly what the queue path
                    // would have done had the client known the agent was
                    // busy. The echoed row stays truthful — the text really
                    // will be consumed by the running turn.
                    let busy_rejected = matches!(
                        &error,
                        firmius_client::ClientError::Remote(protocol_error)
                            if matches!(protocol_error.code, firmius_protocol::ErrorCode::Busy)
                    );
                    if busy_rejected {
                        match daemon
                            .request(DaemonRequest::QueueMessage {
                                agent_id: agent_id.clone(),
                                message: fallback,
                            })
                            .await
                        {
                            Ok(_) => {
                                model.local_turn_intent.remove(&agent_id);
                                model.draft_before_remote_submit = None;
                            }
                            Err(queue_error) => {
                                model.update(AppEvent::TurnDone(Err(queue_error.to_string())));
                            }
                        }
                    } else {
                        // A real failure: the submission never landed, so the
                        // echoed row would be a lie. Remove it and restore the
                        // draft so the user can retry without retyping.
                        if let Some(draft) = model.draft_before_remote_submit.take() {
                            model.composer.replace_text(&draft);
                        }
                        if let Some(items) = model.transcripts.get_mut(&model.focused_id)
                            && items
                                .last()
                                .is_some_and(|item| matches!(item, Item::User(_)))
                        {
                            items.pop();
                        }
                        model.local_turn_intent.remove(&agent_id);
                        model.update(AppEvent::TurnDone(Err(error.to_string())));
                    }
                }
            }
        }
        Action::QueueRemote { agent_id, message } => {
            if let Some(daemon) = model.daemon.clone()
                && let Err(error) = daemon
                    .request(DaemonRequest::QueueMessage { agent_id, message })
                    .await
            {
                model.flash(&format!("message save failed: {error}"));
            }
        }
        Action::CancelRemote { turn_id } => {
            if let Some(daemon) = model.daemon.clone()
                && let Err(error) = daemon.request(DaemonRequest::CancelTurn { turn_id }).await
            {
                model.flash(&format!("cancel failed: {error}"));
            }
        }
        Action::RewindRemote { agent_id, turns } => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon
                    .request(DaemonRequest::Rewind { agent_id, turns })
                    .await
                {
                    Ok(DaemonResponse::Rewound { removed }) => {
                        refresh_remote_snapshot(model, true).await;
                        model.flash(&format!("rewound {removed} messages"));
                    }
                    Ok(_) => model.flash("rewind failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("rewind failed: {error}")),
                }
            }
        }
        Action::EditHistory { agent_id, action } => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon
                    .request(DaemonRequest::EditHistory { agent_id, action })
                    .await
                {
                    Ok(DaemonResponse::EditHistory { result }) => {
                        refresh_remote_snapshot(model, true).await;
                        model.flash(&result);
                    }
                    Ok(_) => model.flash("edit history returned an invalid response"),
                    Err(error) => model.flash(&format!("edit history failed: {error}")),
                }
            } else if let Some(agent) = model
                .agents
                .get(&agent_id)
                .cloned()
                .or_else(|| model.primary.clone())
            {
                match agent.edit_history(&action).await {
                    Ok(result) => {
                        model
                            .transcripts
                            .entry(agent_id)
                            .or_default()
                            .push(Item::Note(result));
                    }
                    Err(error) => model.flash(&format!("edit history failed: {error}")),
                }
            } else {
                model.flash("no active agent");
            }
        }
        Action::SetModelRemote {
            agent_id,
            provider_id,
            model: model_id,
            effort,
        } => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon
                    .request(DaemonRequest::SetModel(SetModelRequest {
                        agent_id,
                        provider_id: provider_id.clone(),
                        model: model_id.clone(),
                        effort,
                    }))
                    .await
                {
                    Ok(DaemonResponse::Ack) => {
                        sync_daemon_preferences(model).await;
                        refresh_remote_snapshot(model, true).await;
                        model.flash(&format!("model: {provider_id}/{model_id}"));
                    }
                    Ok(_) => model.flash("model change failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("model change failed: {error}")),
                }
            }
        }
        Action::SetPersonaRemote {
            agent_id,
            persona,
            delegated,
        } => {
            if let Some(daemon) = model.daemon.clone() {
                let requested_persona = persona.clone();
                match daemon
                    .request(DaemonRequest::SetPersona(SetPersonaRequest {
                        agent_id: agent_id.clone(),
                        persona,
                        delegated,
                    }))
                    .await
                {
                    Ok(DaemonResponse::Ack) => {
                        let preferred = {
                            let settings = model.settings.lock().unwrap();
                            requested_persona
                                .as_deref()
                                .and_then(|id| settings.preferred_model(id).cloned())
                                .or_else(|| settings.preferred_default_model().cloned())
                        };
                        if let Some(preferred) = preferred {
                            let _ = daemon
                                .request(DaemonRequest::SetModel(SetModelRequest {
                                    agent_id,
                                    provider_id: preferred.provider_id,
                                    model: preferred.model,
                                    effort: preferred
                                        .effort
                                        .as_deref()
                                        .map(model::effort_from_name),
                                }))
                                .await;
                        }
                        sync_daemon_preferences(model).await;
                        refresh_remote_snapshot(model, true).await;
                    }
                    Ok(_) => model.flash("persona change failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("persona change failed: {error}")),
                }
            }
        }
        Action::SetTitleRemote(title) => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon.request(DaemonRequest::SetTitle { title }).await {
                    Ok(DaemonResponse::Ack) => {
                        refresh_remote_snapshot(model, true).await;
                        model.flash(&format!("title: {}", model.session_title_label()));
                    }
                    Ok(_) => model.flash("title change failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("title change failed: {error}")),
                }
            }
        }
        Action::ExportRemote(path) => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon.request(DaemonRequest::ExportSession).await {
                    Ok(DaemonResponse::Export(record)) => {
                        let markdown = firmius_core::session_to_markdown(&record);
                        let destination = path.unwrap_or_else(|| {
                            let source = record
                                .title
                                .clone()
                                .unwrap_or_else(|| format!("session-{}", record.id));
                            let slug: String = source
                                .chars()
                                .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
                                .collect();
                            format!("{}.md", slug.trim_matches('-'))
                        });
                        match std::fs::write(&destination, markdown) {
                            Ok(()) => model.flash(&format!("exported {destination}")),
                            Err(error) => model.flash(&format!("export failed: {error}")),
                        }
                    }
                    Ok(_) => model.flash("export failed: invalid daemon response"),
                    Err(error) => model.flash(&format!("export failed: {error}")),
                }
            }
        }
        Action::RefreshRemote => {
            refresh_remote_snapshot(model, true).await;
        }
        Action::Compact => {
            if let Some(daemon) = model.daemon.clone() {
                match daemon
                    .request(DaemonRequest::Compact {
                        agent_id: model.primary_id.clone(),
                    })
                    .await
                {
                    Ok(DaemonResponse::TurnAccepted { turn_id, acceptance_sequence }) => {
                        if model.focused_id == model.primary_id {
                            if let Some(snapshot) = model.remote_snapshot.as_ref() {
                                model.acknowledged_remote_turn = Some(crate::tui::model::AcknowledgedRemoteTurn {
                                    turn_id,
                                    sequence: acceptance_sequence.unwrap_or(u64::MAX),
                                    epoch: model.daemon.as_ref().map(|d| d.endpoint().epoch),
                                    session_id: snapshot.session_id.clone(),
                                    agent_id: model.primary_id.clone(),
                                });
                            }
                            model.remote_turn_id = Some(turn_id);
                        }
                    }
                    Ok(_) => {
                        model.update(AppEvent::TurnDone(Err(
                            "daemon returned an invalid compaction response".into(),
                        )));
                    }
                    Err(error) => {
                        model.update(AppEvent::TurnDone(Err(error.to_string())));
                    }
                };
                return Action::Continue;
            }
            let Some(agent) = model.primary.clone() else {
                model.flash("no active session");
                return Action::Continue;
            };
            let tx2 = tx.clone();
            let agent_id = agent.id.clone();
            let token = CancellationToken::new();
            model.cancel = Some(token.clone());
            tokio::spawn(async move {
                let result = agent
                    .compact(token, |event| {
                        let _ = tx2.try_send(AppEvent::Compaction {
                            agent_id: agent_id.clone(),
                            event,
                        });
                    })
                    .await
                    .map_err(|error| error.to_string());
                let _ = tx2.send(AppEvent::TurnDone(result)).await;
            });
        }
        Action::Quit => return Action::Quit,
        Action::Continue => {}
    }
    Action::Continue
}

async fn refresh_remote_snapshot(model: &mut Model, preserve_transcript: bool) {
    let Some(daemon) = model.daemon.clone() else {
        return;
    };
    match tokio::time::timeout(
        Duration::from_secs(5),
        daemon.request(DaemonRequest::Snapshot),
    )
    .await
    {
        Err(_) => model.flash("daemon resync timed out"),
        Ok(result) => match result {
            Ok(DaemonResponse::Snapshot(snapshot)) => {
                let focused = snapshot
                    .agents
                    .iter()
                    .find(|agent| agent.record.id == model.focused_id)
                    .map(|agent| {
                        agent
                            .processes
                            .iter()
                            .map(|process| process.id)
                            .collect::<Vec<_>>()
                    })
                    .unwrap_or_default();
                model.refresh_remote_snapshot(snapshot, preserve_transcript);
                for proc_id in focused {
                    let offset = model
                        .host_tail_state
                        .get(&proc_id)
                        .map(|state| state.offset)
                        .unwrap_or(0);
                    let response = daemon
                        .request(DaemonRequest::HostPeek {
                            agent_id: model.focused_id.clone(),
                            proc_id,
                            since: offset,
                        })
                        .await;
                    let Ok(DaemonResponse::HostPeek(peek)) = response else {
                        continue;
                    };
                    let state = model.host_tail_state.entry(proc_id).or_default();
                    state.offset = peek.total;
                    state.push(&peek.bytes);
                    model.host_tails.insert(proc_id, state.tail());
                }
            }
            Ok(_) => model.flash("daemon resync returned invalid response"),
            Err(error) => model.flash(&format!("daemon resync failed: {error}")),
        },
    }
}

async fn handle_mcp_action(model: &mut Model, action: McpAction) {
    if let Some(daemon) = model.daemon.clone() {
        let command = match action {
            McpAction::List => McpCommand::List,
            McpAction::Add { name, transport } => {
                let config = match transport {
                    McpTransportSpec::Stdio { command, args } => {
                        McpServerConfig::stdio(name, command, args)
                    }
                    McpTransportSpec::Http { url } => McpServerConfig::http(name, url),
                };
                McpCommand::Add { config }
            }
            McpAction::Remove { name } => McpCommand::Remove { name },
            McpAction::Start { name } => McpCommand::Start { name },
            McpAction::Stop { name } => McpCommand::Stop { name },
            McpAction::Restart { name } => McpCommand::Restart { name },
        };
        match daemon.request(DaemonRequest::Mcp { command }).await {
            Ok(DaemonResponse::Mcp(statuses)) => {
                let text = if statuses.is_empty() {
                    "no MCP servers configured".to_string()
                } else {
                    statuses
                        .iter()
                        .map(|status| {
                            format!(
                                "{} ({}) running={} tools={} enabled={}",
                                status.name,
                                status.transport,
                                status.running,
                                status.tool_count,
                                status.enabled
                            )
                        })
                        .collect::<Vec<_>>()
                        .join("\n")
                };
                push_note(model, text);
            }
            Ok(DaemonResponse::Ack) => model.flash("MCP configuration updated"),
            Ok(_) => model.flash("MCP request returned an invalid response"),
            Err(error) => model.flash(&format!("MCP request failed: {error}")),
        }
        return;
    }
    match action {
        McpAction::List => {
            let statuses = model.mcp.status().await;
            let text = if statuses.is_empty() {
                "no MCP servers configured".to_string()
            } else {
                statuses
                    .iter()
                    .map(|status| {
                        format!(
                            "{} ({}) running={} tools={} enabled={}",
                            status.name,
                            status.transport,
                            status.running,
                            status.tool_count,
                            status.enabled
                        )
                    })
                    .collect::<Vec<_>>()
                    .join("\n")
            };
            push_note(model, text);
        }
        McpAction::Add { name, transport } => {
            let config = match transport {
                McpTransportSpec::Stdio { command, args } => {
                    McpServerConfig::stdio(name.clone(), command, args)
                }
                McpTransportSpec::Http { url } => McpServerConfig::http(name.clone(), url),
            };
            if let Err(error) = model.mcp.add_server(config).await {
                model.flash(&format!("mcp add failed: {error}"));
                return;
            }
            match model.mcp.start(&name).await {
                Ok(specs) => {
                    register_tool_specs(model.tools.as_ref(), model.mcp.clone(), specs);
                    model.flash(&format!("started {name}"));
                }
                Err(error) => model.flash(&format!("saved {name} (start failed: {error})")),
            }
        }
        McpAction::Remove { name } => {
            let specs = model.mcp.stop(&name).await.unwrap_or_default();
            unregister_tool_specs(model.tools.as_ref(), &specs);
            match model.mcp.remove_server(&name).await {
                Ok(()) => model.flash(&format!("removed {name}")),
                Err(error) => model.flash(&format!("remove failed: {error}")),
            }
        }
        McpAction::Start { name } => match model.mcp.start(&name).await {
            Ok(specs) => {
                register_tool_specs(model.tools.as_ref(), model.mcp.clone(), specs);
                model.flash(&format!("started {name}"));
            }
            Err(error) => model.flash(&format!("start failed: {error}")),
        },
        McpAction::Stop { name } => {
            let specs = model.mcp.stop(&name).await.unwrap_or_default();
            unregister_tool_specs(model.tools.as_ref(), &specs);
            model.flash(&format!("stopped {name}"));
        }
        McpAction::Restart { name } => match model.mcp.restart(&name).await {
            Ok(specs) => {
                register_tool_specs(model.tools.as_ref(), model.mcp.clone(), specs);
                model.flash(&format!("restarted {name}"));
            }
            Err(error) => model.flash(&format!("restart failed: {error}")),
        },
    }
}

fn push_note(model: &mut Model, text: String) {
    model
        .transcripts
        .entry(model.primary_id.clone())
        .or_default()
        .push(Item::Note(text));
}

fn handle_clipboard_paste(model: &mut Model) {
    match clipboard::read_clipboard_paste() {
        Ok(clipboard::ClipboardPaste::Text(text)) => {
            if text.chars().count() > composer::PASTE_BLOCK_THRESHOLD {
                model.pastes.push(clipboard::into_stored_paste(
                    clipboard::ClipboardPaste::Text(text),
                ));
                model.composer.insert_paste_block(model.pastes.len());
            } else {
                model.composer.insert_str(&text);
            }
            model.refresh_completion();
        }
        Ok(clipboard::ClipboardPaste::Image(image)) => {
            model.pastes.push(clipboard::into_stored_paste(
                clipboard::ClipboardPaste::Image(image),
            ));
            model.composer.insert_paste_block(model.pastes.len());
            model.refresh_completion();
            model.flash("pasted image from clipboard");
        }
        Err(error) => model.flash(&format!("paste failed: {error}")),
    }
}

async fn bridge_model_session(
    model: &Model,
    session: &mut Option<SessionHandle>,
    tx: &mpsc::Sender<AppEvent>,
) {
    if session.is_none()
        && let Some(lazy_session) = model.session.clone()
    {
        let bus_rx = lazy_session.subscribe();
        event::spawn_bus_bridge(bus_rx, tx.clone());
        *session = Some(lazy_session);
    }
}

async fn register_account(model: &mut Model, record: AccountRecord) {
    let daemon_record = record.clone();
    let id = record.id.clone();
    let (saved, welcome_selection) = {
        let mut mgr = model.manager.lock().unwrap();
        mgr.register_account(record);
        let selection = model.primary.is_none().then(|| {
            mgr.schema(&id).and_then(|schema| {
                schema
                    .models
                    .iter()
                    .find(|info| info.id == "claude-sonnet-5")
                    .or_else(|| schema.models.first())
                    .map(|info| {
                        let effort = info
                            .effort_mode("medium")
                            .cloned()
                            .or_else(|| info.effort_modes.first().cloned());
                        (info.id.clone(), effort)
                    })
            })
        });
        (mgr.save_account_file(&id), selection.flatten())
    };
    match saved {
        Ok(()) => {
            if let Some(daemon) = model.daemon.clone()
                && let Err(error) = daemon
                    .request(DaemonRequest::RegisterAccount {
                        record: daemon_record,
                    })
                    .await
            {
                model.flash(&format!(
                    "account saved locally, daemon update failed: {error}"
                ));
                return;
            }
            if let Some((selected_model, effort)) = welcome_selection {
                model.provider_id = id.clone();
                model.model = selected_model;
                model.effort = effort;
            } else if model.provider_id.is_empty() {
                model.provider_id = id.clone();
            }
            model.flash(&format!("account added: {id}"));
        }
        Err(e) => model.flash(&format!("account added, save failed: {e}")),
    }
}

/// Route one key to the open modal; closes it on `Close`/`Emit`.
async fn handle_modal_key(model: &mut Model, k: KeyEvent) -> Action {
    if k.code == KeyCode::Char('c') && k.modifiers.contains(KeyModifiers::CONTROL) {
        return Action::Quit;
    }
    let Some(modal) = model.modal.as_mut() else {
        return Action::Continue;
    };
    match modal.key(k).await {
        ModalAction::Stay => Action::Continue,
        ModalAction::Close => {
            model.modal = None;
            sync_daemon_preferences(model).await;
            Action::Continue
        }
        ModalAction::Emit(action) => {
            model.modal = None;
            sync_daemon_preferences(model).await;
            action
        }
    }
}

async fn handle_modal_tick(model: &mut Model) -> Action {
    let Some(modal) = model.modal.as_mut() else {
        return Action::Continue;
    };
    match modal.tick().await {
        ModalAction::Stay => Action::Continue,
        ModalAction::Close => {
            model.modal = None;
            sync_daemon_preferences(model).await;
            Action::Continue
        }
        ModalAction::Emit(action) => {
            model.modal = None;
            sync_daemon_preferences(model).await;
            action
        }
    }
}

fn handle_modal_paste(model: &mut Model, text: &str) {
    if let Some(modal) = model.modal.as_mut() {
        modal.paste(text);
    }
}

/// Open the right modal for `/login`: a kind picker when bare, the kind's
/// own wizard when named.
async fn open_login(model: &mut Model, kind: Option<String>) {
    match kind {
        Some(name) => {
            let built = {
                let mgr = model.manager.lock().unwrap();
                mgr.kind(&name)
                    .map(|kind| (kind.display_name().to_string(), kind.wizard()))
            };
            match built {
                Some((label, wizard)) => {
                    let modal = WizardModal::start(name.clone(), label, wizard).await;
                    model.completion = None;
                    model.modal = Some(Box::new(modal));
                }
                None => model.flash(&format!("unknown kind: {name}")),
            }
        }
        None => {
            let options: Vec<(String, String)> = model
                .manager
                .lock()
                .unwrap()
                .kinds()
                .iter()
                .map(|kind| (kind.name().to_string(), kind.display_name().to_string()))
                .collect();
            if options.is_empty() {
                model.flash("no account kinds registered");
                return;
            }
            model.completion = None;
            model.modal = Some(Box::new(KindPickerModal::new(options)));
        }
    }
}

async fn open_accounts(model: &mut Model, provider: String) {
    let accounts = model.manager.lock().unwrap().accounts_for(&provider);
    if accounts.is_empty() {
        model.flash(&format!("no stored accounts for {provider}"));
        return;
    }
    let mut rows = Vec::with_capacity(accounts.len());
    for account in accounts {
        let capability = model
            .manager
            .lock()
            .unwrap()
            .quota_capability(&account.id)
            .ok()
            .flatten();
        let (descriptor, source) = capability
            .map(|capability| (Some(capability.descriptor), capability.source))
            .unwrap_or((None, None));
        let mut row = AccountRow {
            id: account.id,
            kind: account.kind,
            descriptor,
            source,
            snapshot: None,
            error: None,
        };
        if let Some(source) = &row.source {
            match source.fetch().await {
                Ok(snapshot) => row.snapshot = Some(snapshot),
                Err(error) => row.error = Some(error.to_string()),
            }
        }
        rows.push(row);
    }
    model.completion = None;
    model.modal = Some(Box::new(AccountsModal::new(provider, rows)));
}

/// Async state refreshes the synchronous update loop can't do itself:
/// the agent roster, background counts, and bash output tails.
async fn refresh_async(model: &mut Model, tx: &mpsc::Sender<AppEvent>) {
    // The frame clock is intentionally 30fps for smooth activity animation,
    // but this housekeeping path walks all agents and snapshots their
    // histories.  Running that work on every frame causes a periodic pause
    // as transcripts grow, especially while an edit patch is streaming.
    let now = Instant::now();
    if !model::async_refresh_due(model.last_async_refresh, now) {
        return;
    }
    model.last_async_refresh = Some(now);
    if model.session_completion_needed()
        && matches!(
            model.session_completion,
            model::SessionCompletionState::Ready
        )
    {
        model.session_completion = model::SessionCompletionState::Loading;
        spawn_session_refresh(tx.clone());
    }
    if model.daemon.is_some() {
        // Routine remote state arrives as compact SessionStatus events. Full
        // snapshots are recovery-only and are requested by explicit actions
        // or daemon gap/reconnect notifications.
        return;
    }
    let Some(session_handle) = &model.session else {
        model.bg_agents = 0;
        model.bg_procs = 0;
        return;
    };
    let session = session_handle;
    model.roster = session
        .agents
        .read()
        .unwrap()
        .iter()
        .enumerate()
        .map(|(i, (id, _))| {
            (
                id.clone(),
                if i == 0 {
                    "main".to_string()
                } else {
                    format!("agent {}", i)
                },
            )
        })
        .collect();
    model.agents = session
        .agents
        .read()
        .unwrap()
        .iter()
        .map(|(id, agent)| (id.clone(), agent.clone()))
        .collect();
    // A child may have completed its first prompt before the UI first focuses
    // it. Seed that transcript from durable agent history so its prompt is
    // visible immediately when focus switches to the child.
    for agent in model.agents.values() {
        let history = agent.history();
        let transcript = model.transcripts.entry(agent.id.clone()).or_default();
        if transcript.is_empty() && !history.is_empty() {
            *transcript = items_from_history(&history);
        }
    }
    for agent in model.agents.values() {
        if agent.provider_manager_handle().is_none() || agent.user_settings_handle().is_none() {
            agent.attach_runtime(model.manager.clone(), model.settings.clone());
            agent.attach_firmius_config(model.config.clone());
        }
    }
    model.agent_efforts.clear();
    {
        let manager = model.manager.lock().unwrap();
        for (agent_id, agent) in session.agents.read().unwrap().iter() {
            let config = agent.config();
            if let Some(info) = manager.model_info_for(&config.provider_id, &config.model) {
                model
                    .agent_efforts
                    .insert(agent_id.clone(), info.effort_modes.clone());
            }
        }
    }
    model.delegate_children.clear();
    model.parent_by_agent.clear();
    // Preserve session insertion order and retain the tool-call id. A plain
    // ordinal lookup can attach an old child's window to a newly streaming
    // delegate call.
    for (agent_id, _) in session.agents.read().unwrap().iter() {
        if let Some(node) = session.hierarchy.read().unwrap().get(agent_id)
            && let Some(parent_id) = &node.parent_id
        {
            model
                .parent_by_agent
                .insert(agent_id.clone(), parent_id.clone());
            model
                .delegate_children
                .entry(parent_id.clone())
                .or_default()
                .push((node.spawned_via_tool_call_id.clone(), agent_id.clone()));
        }
    }
    model.bg_agents = session
        .active_delegates()
        .await
        .iter()
        .filter(|d| !d.finished)
        .count();
    if let Some(agent) = session.agent(&model.focused_id) {
        let usage = agent.usage();
        let config = agent.config();
        model.ctx_used = usage.input_tokens;
        model.ctx_max = model
            .manager
            .lock()
            .unwrap()
            .model_info_for(&config.provider_id, &config.model)
            .map(|info| info.context_window)
            .unwrap_or(0);
        let host = agent.host();
        let infos = host.list_info();
        model.process_statuses = infos.clone();
        model.bg_procs = infos
            .iter()
            .filter(|p| matches!(p.status, ProcStatus::Running))
            .count();
        // Capture output for every process. The presenter decides whether the
        // current bash mode should display it. Keeping exited output here is
        // what lets exec/spawn calls retain their output window after exit.
        let mut tails_changed = false;
        for info in &infos {
            let state = model.host_tail_state.entry(info.id).or_default();
            // Read only what is new. `peek` copies `buffer[since..]`, so a
            // `since` of 0 re-copied the process's whole output buffer on
            // every tick and grew the heap without bound while idle.
            let Ok((chunk, total, _)) = host.peek(info.id, state.offset) else {
                continue;
            };
            if chunk.is_empty() && state.offset == total {
                continue;
            }
            state.offset = total;
            state.push(&chunk);
            let tail = state.tail();
            if model.host_tails.get(&info.id) != Some(&tail) {
                tails_changed = true;
                model.host_tails.insert(info.id, tail);
            }
        }
        // Forget processes the host no longer tracks. These maps were only
        // ever inserted into, so every process a long session ever spawned
        // kept its tail window resident for the lifetime of the TUI.
        if model.host_tails.len() > infos.len() || model.host_tail_state.len() > infos.len() {
            let live: std::collections::HashSet<_> = infos.iter().map(|info| info.id).collect();
            model.host_tails.retain(|id, _| live.contains(id));
            model.host_tail_state.retain(|id, _| live.contains(id));
            tails_changed = true;
        }
        if tails_changed {
            model.clear_render_cache();
        }
    }
    // Completion is refreshed by composer edits and focus changes. Rebuilding
    // it here made every housekeeping pass scan model/account/session lists,
    // even while the user was idle, which amplified the periodic render
    // pause. Metadata changes are picked up on the next input/focus change.
    let _ = session;
}

fn focused_provider_id(model: &Model) -> Option<String> {
    let (provider_id, _, _) = model.focused_model_status();
    (!provider_id.is_empty()).then_some(provider_id)
}

fn spawn_quota_refresh(model: &mut Model, tx: &mpsc::Sender<AppEvent>) {
    let Some(provider_id) = focused_provider_id(model) else {
        model.quota = None;
        model.quota_error = None;
        model.quota_provider_id = None;
        return;
    };
    if model.quota_provider_id.as_deref() != Some(provider_id.as_str()) {
        model.quota = None;
        model.quota_error = None;
        model.quota_provider_id = Some(provider_id.clone());
    }
    let source = {
        let manager = model.manager.lock().unwrap();
        manager
            .quota_capability(&provider_id)
            .ok()
            .flatten()
            .and_then(|capability| capability.source)
    };
    let Some(source) = source else {
        model.quota = None;
        return;
    };
    let tx = tx.clone();
    tokio::spawn(async move {
        let result = source.fetch().await.map_err(|error| error.to_string());
        let _ = tx.send(AppEvent::Quota(result)).await;
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tui::modal::ModalSurface;
    use async_trait::async_trait;
    use crossterm::event::KeyModifiers;
    use firmius_core::{
        AccountKind, AgentConfig, AnthropicSubscriptionKind, FirmiusConfig, OpencodeGoKind,
        PersonaManager, Provider, ProviderError, ProviderEvent, ProviderRequest, StopReason,
        ToolRegistry, UserSettings,
    };
    use futures::stream::{BoxStream, StreamExt};
    use ratatui::layout::Rect;

    struct EmptyProvider(&'static str);

    #[async_trait]
    impl Provider for EmptyProvider {
        fn id(&self) -> &str {
            self.0
        }

        async fn stream(
            &self,
            _request: ProviderRequest,
        ) -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError>
        {
            Ok(futures::stream::iter([Ok(ProviderEvent::Done {
                reason: StopReason::Stop,
            })])
            .boxed())
        }
    }

    struct EmitOnTick(Option<AccountRecord>);

    #[async_trait]
    impl ModalSurface for EmitOnTick {
        fn title(&self) -> String {
            "test".into()
        }

        fn height_hint(&self, _width: u16) -> u16 {
            1
        }

        fn render(
            &self,
            _area: Rect,
            _frame: &mut ratatui::Frame,
            _theme: &crate::tui::theme::Theme,
        ) {
        }

        async fn key(&mut self, _key: KeyEvent) -> ModalAction {
            ModalAction::Stay
        }

        async fn tick(&mut self) -> ModalAction {
            self.0
                .take()
                .map(|record| ModalAction::Emit(Action::RegisterAccount { record }))
                .unwrap_or(ModalAction::Stay)
        }

        fn cursor(&self, _area: Rect) -> Option<(u16, u16)> {
            None
        }
    }

    #[tokio::test]
    async fn daemon_permission_request_opens_modal_and_allow_emits_matching_answer() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager.clone(),
            "test".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.permission_policy = Some(firmius_core::PermissionPolicy::default());
        let descriptor =
            firmius_core::describe_tool_call("bash", &serde_json::json!({"command": "ls"}), None);
        let request = firmius_core::PendingPermissionRequest {
            request_id: uuid::Uuid::new_v4(),
            nonce: uuid::Uuid::new_v4(),
            session_id: "session".into(),
            agent_id: "agent".into(),
            tool: "bash".into(),
            action_digest: firmius_core::permissions::descriptor_digest(&descriptor),
            descriptor,
            expected_revision: 0,
        };
        let (events, receiver) = tokio::sync::broadcast::channel(16);
        let (tx, mut rx) = mpsc::channel(16);
        event::spawn_daemon_bridge(receiver, tx.clone());
        events
            .send(firmius_protocol::DaemonEvent::PermissionRequested(
                request.clone(),
            ))
            .unwrap();
        let event = tokio::time::timeout(Duration::from_secs(2), rx.recv())
            .await
            .unwrap()
            .unwrap();
        handle_event(event, &mut model, &mut None, &manager, &tx).await;
        assert!(model.modal.is_some());
        let answer = handle_modal_key(
            &mut model,
            KeyEvent::new(KeyCode::Char('a'), KeyModifiers::NONE),
        )
        .await;
        let Action::ResolvePermission { resolution } = answer else {
            panic!("allow must resolve the pending request");
        };
        assert_eq!(resolution.request_id, request.request_id);
        assert_eq!(resolution.nonce, request.nonce);
        assert_eq!(resolution.action_digest, request.action_digest);
        assert_eq!(resolution.decision, firmius_core::PermissionDecision::Allow);
    }

    #[tokio::test]
    async fn modal_emit_registers_account_and_populates_model_choices() {
        let data_dir =
            std::env::temp_dir().join(format!("firmius-modal-test-{}", std::process::id()));
        let mut manager = ProviderManager::new().with_data_dir(data_dir.clone());
        manager.register_kind(Arc::new(OpencodeGoKind));
        let manager = Arc::new(std::sync::Mutex::new(manager));
        let kind = OpencodeGoKind;
        let modal = WizardModal::start(
            "opencode-go".into(),
            kind.display_name().into(),
            kind.wizard(),
        )
        .await;
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager.clone(),
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.modal = Some(Box::new(modal));
        handle_modal_paste(&mut model, "oc-test-key");
        let (tx, _rx) = mpsc::channel(16);
        let action = handle_event(
            AppEvent::Term(TermEvent::Key(KeyEvent::new(
                KeyCode::Enter,
                KeyModifiers::NONE,
            ))),
            &mut model,
            &mut None,
            &manager,
            &tx,
        )
        .await;

        assert!(matches!(action, Action::Continue));
        assert!(
            manager
                .lock()
                .unwrap()
                .model_choices()
                .iter()
                .any(|(provider, model)| provider == "opencode-go" && model == "kimi-k2.7-code")
        );
        model.composer.insert_str("/model ");
        model.refresh_completion();
        assert!(model.completion.as_ref().is_some_and(|completion| {
            completion
                .items
                .iter()
                .any(|item| item.label == "opencode-go/kimi-k2.7-code")
        }));
        let _ = std::fs::remove_dir_all(data_dir);
    }

    #[tokio::test]
    async fn modal_tick_registers_account_from_async_completion() {
        let data_dir =
            std::env::temp_dir().join(format!("firmius-modal-tick-test-{}", std::process::id()));
        let mut manager = ProviderManager::new().with_data_dir(data_dir.clone());
        manager.register_kind(Arc::new(OpencodeGoKind));
        let manager = Arc::new(std::sync::Mutex::new(manager));
        let kind = OpencodeGoKind;
        let mut schema = firmius_core::kinds::opencode_go::schema_template();
        schema.id = "tick-account".into();
        let record = AccountRecord {
            id: "tick-account".into(),
            kind: kind.name().into(),
            schema,
            credentials: serde_json::json!({"api_key": "test-key"}),
        };
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager.clone(),
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.modal = Some(Box::new(EmitOnTick(Some(record))));

        let action = handle_modal_tick(&mut model).await;
        assert!(matches!(action, Action::RegisterAccount { .. }));
        if let Action::RegisterAccount { record } = action {
            register_account(&mut model, record).await;
        }

        assert_eq!(manager.lock().unwrap().accounts_for("opencode-go").len(), 1);
        assert!(data_dir.join("accounts/tick-account.json").is_file());
        let _ = std::fs::remove_dir_all(data_dir);
    }

    #[tokio::test]
    async fn welcome_anthropic_login_selects_a_valid_default_model_and_effort() {
        let data_dir = std::env::temp_dir().join(format!(
            "firmius-anthropic-login-test-{}",
            std::process::id()
        ));
        let mut manager = ProviderManager::new().with_data_dir(data_dir.clone());
        manager.register_kind(Arc::new(AnthropicSubscriptionKind));
        let manager = Arc::new(std::sync::Mutex::new(manager));
        let mut model = Model::new(
            None,
            None,
            "missing-provider".into(),
            manager,
            "missing-model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        register_account(
            &mut model,
            AccountRecord {
                id: "anthropic-test".into(),
                kind: "anthropic".into(),
                schema: firmius_core::kinds::anthropic_subscription::schema_template(
                    "anthropic-test",
                ),
                credentials: serde_json::json!({
                    "access_token": "access",
                    "refresh_token": "refresh",
                    "expires_at": 4_102_444_800_i64,
                }),
            },
        )
        .await;

        assert_eq!(model.provider_id, "anthropic-test");
        assert_eq!(model.model, "claude-sonnet-5");
        assert_eq!(
            model.effort.as_ref().map(|effort| effort.name.as_str()),
            Some("medium")
        );
        let _ = std::fs::remove_dir_all(data_dir);
    }

    #[tokio::test]
    async fn lazy_session_is_bridged_before_prompt_events_start() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        let lazy_session = Session::new_handle();
        model.session = Some(lazy_session.clone());
        let (tx, mut rx) = mpsc::channel(16);
        let mut session = None;

        bridge_model_session(&model, &mut session, &tx).await;
        assert!(session.is_some());

        lazy_session
            .event_sender()
            .send(firmius_core::SessionEvent {
                session_id: lazy_session.id.clone(),
                sequence: 1,
                at: chrono::Utc::now(),
                payload: firmius_core::SessionEventPayload::Agent {
                    agent_id: "lazy-agent".into(),
                    event: firmius_core::AgentEvent::Text("visible".into()),
                },
            })
            .unwrap();
        let event = tokio::time::timeout(std::time::Duration::from_secs(1), rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert!(matches!(
            event,
            AppEvent::Bus(firmius_core::SessionEvent { payload, .. })
                if matches!(
                    &payload,
                    firmius_core::SessionEventPayload::Agent { agent_id, event }
                        if agent_id == "lazy-agent"
                            && matches!(event, firmius_core::AgentEvent::Text(text) if text == "visible")
                )
        ));
    }

    #[tokio::test]
    async fn composer_submission_targets_the_focused_subagent() {
        let tools = Arc::new(ToolRegistry::default());
        let raw_session = Session::new_handle();
        let primary = raw_session.spawn_agent(
            Arc::new(EmptyProvider("primary")),
            tools.clone(),
            AgentConfig {
                provider_id: "primary".into(),
                model: "test".into(),
                ..Default::default()
            },
        );
        let child = raw_session.spawn_agent(
            Arc::new(EmptyProvider("child")),
            tools.clone(),
            AgentConfig {
                provider_id: "child".into(),
                model: "test".into(),
                ..Default::default()
            },
        );
        let session = raw_session;
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            Some(session.clone()),
            Some(primary.clone()),
            "primary".into(),
            manager.clone(),
            "test".into(),
            tools,
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.roster = vec![
            (primary.id.clone(), "main".into()),
            (child.id.clone(), "child".into()),
        ];
        model.focused_id = child.id.clone();
        model.composer.insert_str("hello child");
        let (tx, _rx) = mpsc::channel(16);
        let mut session_slot = Some(session.clone());

        let action = handle_event(
            AppEvent::Term(TermEvent::Key(KeyEvent::new(
                KeyCode::Enter,
                KeyModifiers::NONE,
            ))),
            &mut model,
            &mut session_slot,
            &manager,
            &tx,
        )
        .await;
        assert!(matches!(action, Action::Continue));

        tokio::time::timeout(std::time::Duration::from_secs(1), async {
            loop {
                if child
                    .history()
                    .iter()
                    .any(|message| message.role == firmius_core::MessageRole::User)
                {
                    break;
                }
                tokio::time::sleep(std::time::Duration::from_millis(10)).await;
            }
        })
        .await
        .expect("focused child should receive the prompt");
        assert!(
            !primary
                .history()
                .iter()
                .any(|message| message.role == firmius_core::MessageRole::User)
        );
    }
}