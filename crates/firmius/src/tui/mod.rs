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
pub mod settings;
pub mod style;
pub mod theme;
pub mod view;

use std::io;
use std::sync::Arc;
use std::time::Duration;

use crossterm::ExecutableCommand;
use crossterm::event::{
    DisableBracketedPaste, DisableMouseCapture, EnableBracketedPaste, EnableMouseCapture,
    Event as TermEvent, KeyCode, KeyEvent, KeyEventKind, KeyModifiers,
};
use crossterm::terminal::{
    EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode,
};
use firmius_core::{
    AccountRecord, Agent, AgentEvent, McpManager, McpServerConfig, PersonaManager, ProcStatus,
    ProviderManager, Session, SessionEvent, UserSettings, register_tool_specs,
    unregister_tool_specs,
};
use ratatui::Terminal;
use ratatui::backend::CrosstermBackend;
use tokio::sync::{Mutex, mpsc};
use tokio_util::sync::CancellationToken;

use command::{McpAction, McpTransportSpec};
use event::AppEvent;
use modal::{
    AccountRow, AccountsModal, KindPickerModal, ModalAction, PersonasModal, SettingsModal,
    WizardModal,
};
use model::{Action, Item, Model, items_from_history};
use settings::{CompactionSection, GeneralSection, RetrySection, SettingsSection};

pub async fn run(
    session: Option<Arc<Mutex<Session>>>,
    primary: Option<Arc<Agent>>,
    provider_id: String,
    model_name: String,
    tools: Arc<firmius_core::ToolRegistry>,
    manager: Arc<std::sync::Mutex<ProviderManager>>,
    personas: Arc<PersonaManager>,
    settings: Arc<std::sync::Mutex<UserSettings>>,
    config: Arc<std::sync::Mutex<firmius_core::FirmiusConfig>>,
    mcp: Arc<McpManager>,
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

    // Keep the queue finite. The terminal pump drops excess mouse events, but
    // never lets an input flood grow without bound.
    let (tx, mut rx) = mpsc::unbounded_channel::<AppEvent>();
    event::spawn_term_pump(tx.clone());
    let mut session = session;
    if let Some(session) = &session {
        let bus_rx = session.lock().await.subscribe();
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
    let mut ticks = tokio::time::interval(Duration::from_millis(33));
    ticks.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    let outcome: Result<(), String> = loop {
        terminal
            .draw(|f| view::draw(&model, f))
            .map_err(|e| e.to_string())?;

        tokio::select! {
            _ = ticks.tick() => {
                model.update(AppEvent::Tick);
                if model.modal.is_some() {
                    let action = handle_modal_tick(&mut model).await;
                    match action {
                        Action::Quit => break Ok(()),
                        Action::RegisterAccount { record } => {
                            register_account(&mut model, record);
                        }
                        _ => {}
                    }
                }
                refresh_async(&mut model).await;
            }
            incoming = rx.recv() => {
                let Some(first) = incoming else { break Ok(()) };
                // One terminal draw can be much more expensive than one key
                // event when a resumed transcript is large. Drain a bounded
                // batch and render once, rather than rendering once per event.
                let mut batch = Vec::with_capacity(128);
                batch.push(first);
                while batch.len() < 128 {
                    match rx.try_recv() {
                        Ok(ev) => {
                            let urgent = matches!(
                                &ev,
                                AppEvent::Bus(SessionEvent {
                                    event: AgentEvent::ToolCallDelta { .. },
                                    ..
                                })
                            );
                            batch.push(ev);
                            // Render a tool argument delta immediately instead
                            // of draining a burst behind unrelated events.
                            if urgent {
                                break;
                            }
                        }
                        Err(mpsc::error::TryRecvError::Empty) => break,
                        Err(mpsc::error::TryRecvError::Disconnected) => break,
                    }
                }
                let mut quit = false;
                for ev in batch {
                    let action = handle_event(ev, &mut model, &mut session, &manager, &tx).await;
                    if matches!(action, Action::Quit) {
                        quit = true;
                        break;
                    }
                }
                if quit { break Ok(()) }
            }
        }
    };

    // Teardown: leave the alternate screen, then persist.
    let _ = io::stdout()
        .execute(DisableBracketedPaste)
        .and_then(|s| s.execute(DisableMouseCapture))
        .and_then(|s| s.execute(LeaveAlternateScreen));
    let _ = disable_raw_mode();
    if let Some(session) = &session
        && let Err(e) = session.lock().await.save()
    {
        eprintln!("warning: could not save session: {e}");
    }
    outcome
}

async fn handle_event(
    ev: AppEvent,
    model: &mut Model,
    session: &mut Option<Arc<Mutex<Session>>>,
    manager: &Arc<std::sync::Mutex<ProviderManager>>,
    tx: &mpsc::UnboundedSender<AppEvent>,
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
            ev @ AppEvent::BusLagged(n) => {
                model.flash(&format!("bus lagged ({n} events) — rebuilding"));
                model.update(ev)
            }
            AppEvent::TurnDone(res) => {
                let action = model.update(AppEvent::TurnDone(res));
                if let Some(session) = session
                    && let Err(e) = session.lock().await.save()
                {
                    model.flash(&format!("save failed: {e}"));
                }
                action
            }
            other => model.update(other),
        },
    };

    match action {
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
            register_account(model, record);
        }
        Action::Mcp(action) => {
            handle_mcp_action(model, action).await;
        }
        Action::RebuildTranscripts => {
            if let Some(session) = session {
                let current = session.lock().await;
                for agent in current.agents.values() {
                    model
                        .transcripts
                        .insert(agent.id.clone(), items_from_history(&agent.history()));
                }
            }
        }
        Action::Save => {
            if let Some(session) = session {
                if let Err(e) = session.lock().await.save() {
                    model.flash(&format!("save failed: {e}"));
                } else {
                    model.flash("session saved");
                }
            } else {
                model.flash("no active session");
            }
        }
        Action::Resume(requested_id) => {
            if model.busy {
                model.flash("busy — wait for the turn to finish");
                return Action::Continue;
            }
            let id = requested_id.or_else(|| {
                firmius_core::list_sessions()
                    .ok()
                    .and_then(|items| items.first().map(|item| item.id.clone()))
            });
            let Some(id) = id else {
                model.flash("no saved sessions");
                return Action::Continue;
            };
            let resumed = {
                let manager = manager.lock().unwrap();
                Session::resume_with_personas(
                    &id,
                    &manager,
                    model.tools.clone(),
                    model.personas.clone(),
                )
            };
            match resumed {
                Ok(next) => {
                    let next = Arc::new(Mutex::new(next));
                    let (next_agent, next_bus) = {
                        let mut session = next.lock().await;
                        session.bind_self(&next);
                        for agent in session.agents.values() {
                            agent.attach_runtime(model.manager.clone(), model.settings.clone());
                        }
                        (session.agents.values().next().cloned(), session.subscribe())
                    };
                    let Some(next_agent) = next_agent else {
                        model.flash("saved session has no available agents");
                        return Action::Continue;
                    };
                    event::spawn_bus_bridge(next_bus, tx.clone());
                    model.replace_session(next.clone(), next_agent);
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
                current.lock().await.agent(&agent_id)
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
                let res = agent.prompt_message(message, token, |_| {}).await;
                let _ = tx2.send(AppEvent::TurnDone(
                    res.map(|_| ()).map_err(|e| e.to_string()),
                ));
            });
        }
        Action::Compact => {
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
                        let _ = tx2.send(AppEvent::Compaction {
                            agent_id: agent_id.clone(),
                            event,
                        });
                    })
                    .await
                    .map_err(|error| error.to_string());
                let _ = tx2.send(AppEvent::TurnDone(result));
            });
        }
        Action::Quit => return Action::Quit,
        Action::Continue => {}
    }
    Action::Continue
}

async fn handle_mcp_action(model: &mut Model, action: McpAction) {
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
    session: &mut Option<Arc<Mutex<Session>>>,
    tx: &mpsc::UnboundedSender<AppEvent>,
) {
    if session.is_none()
        && let Some(lazy_session) = model.session.clone()
    {
        let bus_rx = lazy_session.lock().await.subscribe();
        event::spawn_bus_bridge(bus_rx, tx.clone());
        *session = Some(lazy_session);
    }
}

fn register_account(model: &mut Model, record: AccountRecord) {
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
            Action::Continue
        }
        ModalAction::Emit(action) => {
            model.modal = None;
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
            Action::Continue
        }
        ModalAction::Emit(action) => {
            model.modal = None;
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
async fn refresh_async(model: &mut Model) {
    let Some(session_handle) = &model.session else {
        model.bg_agents = 0;
        model.bg_procs = 0;
        return;
    };
    let session = session_handle.lock().await;
    model.roster = session
        .agents
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
        }
    }
    model.agent_efforts.clear();
    {
        let manager = model.manager.lock().unwrap();
        for (agent_id, agent) in &session.agents {
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
    for (agent_id, _) in &session.agents {
        if let Some(node) = session.hierarchy.get(agent_id)
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
        model.bg_procs = infos
            .iter()
            .filter(|p| matches!(p.status, ProcStatus::Running))
            .count();
        // Capture output for every process. The presenter decides whether the
        // current bash mode should display it. Keeping exited output here is
        // what lets exec/spawn calls retain their output window after exit.
        let mut tails_changed = false;
        for info in &infos {
            if let Ok((bytes, _, _)) = host.peek(info.id, 0) {
                let start = bytes.len().saturating_sub(2048);
                let tail = String::from_utf8_lossy(&bytes[start..]).to_string();
                let changed = model.host_tails.get(&info.id) != Some(&tail);
                if changed {
                    tails_changed = true;
                }
                model.host_tails.insert(info.id, tail);
            }
        }
        if tails_changed {
            model.clear_render_cache();
        }
    }
    // The focused agent may have changed since the last key event. Rebuild
    // the menu after its model metadata is available so `/effort ` opens
    // immediately instead of waiting for another typed character.
    drop(session);
    model.refresh_completion();
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
        let (tx, _rx) = mpsc::unbounded_channel();
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
            register_account(&mut model, record);
        }

        assert_eq!(manager.lock().unwrap().accounts_for("opencode-go").len(), 1);
        assert!(data_dir.join("accounts/tick-account.json").is_file());
        let _ = std::fs::remove_dir_all(data_dir);
    }

    #[test]
    fn welcome_anthropic_login_selects_a_valid_default_model_and_effort() {
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
        );

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
        let lazy_session = Arc::new(Mutex::new(Session::new()));
        model.session = Some(lazy_session.clone());
        let (tx, mut rx) = mpsc::unbounded_channel();
        let mut session = None;

        bridge_model_session(&model, &mut session, &tx).await;
        assert!(session.is_some());

        lazy_session
            .lock()
            .await
            .event_sender()
            .send(firmius_core::SessionEvent {
                agent_id: "lazy-agent".into(),
                event: firmius_core::AgentEvent::Text("visible".into()),
            })
            .unwrap();
        let event = tokio::time::timeout(std::time::Duration::from_secs(1), rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert!(matches!(
            event,
            AppEvent::Bus(firmius_core::SessionEvent { agent_id, event })
                if agent_id == "lazy-agent"
                    && matches!(&event, firmius_core::AgentEvent::Text(text) if text == "visible")
        ));
    }

    #[tokio::test]
    async fn composer_submission_targets_the_focused_subagent() {
        let tools = Arc::new(ToolRegistry::default());
        let mut raw_session = Session::new();
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
        let session = Arc::new(Mutex::new(raw_session));
        session.lock().await.bind_self(&session);
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
        let (tx, _rx) = mpsc::unbounded_channel();
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