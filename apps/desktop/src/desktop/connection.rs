//! Session subscriptions are independent from viewport focus. Transport and
//! model updates are immediate; rendering is coalesced to a frame boundary.
use super::*;
use firmius_client::DaemonClient;
use tokio::sync::broadcast::error::RecvError;

type SharedState = Arc<Mutex<DesktopState>>;
type Window = slint::Weak<MainWindow>;

pub(super) fn monitor_active_session(weak: Window, state: SharedState) {
    let Ok(rt) = runtime() else { return };
    rt.spawn(async move {
        let mut monitors = HashMap::<String, tokio::task::JoinHandle<()>>::new();
        loop {
            let sessions: std::collections::HashSet<String> = state
                .lock()
                .map(|s| {
                    s.shell
                        .viewports
                        .iter()
                        .flat_map(|p| &p.tabs)
                        .filter_map(|t| t.route.session.clone())
                        .collect()
                })
                .unwrap_or_default();
            monitors.retain(|id, task| {
                if sessions.contains(id) && !task.is_finished() {
                    return true;
                }
                task.abort();
                if !sessions.contains(id) {
                    let client = state.lock().ok().and_then(|mut s| s.clients.remove(id));
                    if let Some(client) = client {
                        tokio::spawn(async move {
                            client.close().await;
                        });
                    }
                }
                false
            });
            for id in sessions {
                if !monitors.contains_key(&id) {
                    let task = tokio::spawn(watch_session(weak.clone(), state.clone(), id.clone()));
                    monitors.insert(id, task);
                }
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    });
}

fn reconnect_notice(weak: Window, error: String) {
    invoke(weak, move |ui| {
        ui.set_connection_status("Reconnecting…".into());
        ui.set_notice(error.into());
    });
}

async fn reconcile(
    client: &DaemonClient,
    state: &SharedState,
    session: &str,
) -> Result<SessionSnapshot, String> {
    // Registration is idempotent and cannot steal another viewer's approval.
    let _ = client.register_permission_approver().await;
    let snapshot = attached_snapshot(client, session).await?;
    if let Ok(policy) = client.permission_policy().await
        && let Ok(mut state) = state.lock()
    {
        state.permission_policies.insert(session.to_owned(), policy);
    }
    let after = state
        .lock()
        .ok()
        .and_then(|s| s.models.get(session).map(|m| m.replay_sequence))
        .unwrap_or(0);
    match client.request(Request::SessionEvents { after }).await {
        Ok(Response::SessionEvents {
            events,
            earliest,
            latest,
        }) => {
            let mut s = state.lock().map_err(|_| "desktop state lock poisoned")?;
            let model = s.models.entry(session.into()).or_default();
            // `earliest` describes the retained event-log tail, not missing
            // conversation history. The authoritative snapshot already
            // contains the durable transcript, so a fresh desktop attach
            // must never turn this normal snapshot/replay boundary into a
            // user-visible "history lost" warning.
            let _ = earliest;
            model.history_gap = None;
            model.receive_batch(events);
            model.replay_sequence = latest;
        }
        Ok(_) => return Err("daemon returned an unexpected event replay response".into()),
        Err(error) => {
            return Err(format!(
                "Session replay failed (rebuild/restart the daemon if it is older): {error}"
            ));
        }
    }
    Ok(snapshot)
}

async fn watch_session(weak: Window, state: SharedState, session: String) {
    loop {
        let client = match crate::daemon::client_for(&state, Some(&session)).await {
            Ok(client) => client,
            Err(error) => {
                reconnect_notice(weak.clone(), error);
                tokio::time::sleep(Duration::from_secs(1)).await;
                continue;
            }
        };
        // Subscribe before snapshot/replay to close the attachment race.
        let mut events = client.subscribe();
        // 30 Hz is enough for legible token/process streaming and leaves the
        // UI thread time for layout, selection and scrolling.
        let mut frames = tokio::time::interval(Duration::from_millis(33));
        let mut dirty = false;
        // At most one queued frame per subscription. A slow UI must not
        // accumulate callbacks that each rebuild the same latest snapshot.
        let frame_pending = Arc::new(std::sync::atomic::AtomicBool::new(false));
        match reconcile(&client, &state, &session).await {
            Ok(snapshot) => publish_snapshot(weak.clone(), state.clone(), snapshot),
            Err(error) => {
                reconnect_notice(weak.clone(), error);
                break;
            }
        }
        loop {
            tokio::select! {
                _ = frames.tick(), if dirty => {
                    if frame_pending.swap(true, std::sync::atomic::Ordering::AcqRel) {
                        continue;
                    }
                    dirty = false;
                    let state = state.clone();
                    let pending = frame_pending.clone();
                    invoke(weak.clone(), move |ui| {
                        if let Ok(state) = state.lock() { shell_ui::render(ui, &state); }
                        pending.store(false, std::sync::atomic::Ordering::Release);
                    });
                }
                event = events.recv() => match event {
                    Ok(DaemonEvent::ConnectionLost { reason }) => {
                        reconnect_notice(weak.clone(), reason); break;
                    }
                    Ok(DaemonEvent::ShuttingDown) | Err(RecvError::Closed) => break,
                    Ok(DaemonEvent::SnapshotRequired { .. }) | Err(RecvError::Lagged(_)) => {
                        match reconcile(&client, &state, &session).await {
                            Ok(snapshot) => publish_snapshot(weak.clone(), state.clone(), snapshot),
                            Err(error) => { reconnect_notice(weak.clone(), error); break; }
                        }
                    }
                    Ok(DaemonEvent::TurnCompleted { .. }) => {
                        // The hot status projection intentionally omits history.
                        // Pull one authoritative snapshot at the turn boundary
                        // so streamed text becomes durable transcript without
                        // rebuilding histories on every token.
                        match attached_snapshot(&client, &session).await {
                            Ok(snapshot) => publish_snapshot(weak.clone(), state.clone(), snapshot),
                            Err(error) => { reconnect_notice(weak.clone(), error); break; }
                        }
                        dirty = false;
                    }
                    Ok(event) => { apply_event(weak.clone(), &state, event); dirty = true; }
                }
            }
        }
        client.close().await;
        tokio::time::sleep(Duration::from_millis(250)).await;
    }
}

fn apply_event(weak: Window, state: &SharedState, event: DaemonEvent) {
    match event {
        DaemonEvent::Session(event) => {
            if let Ok(mut s) = state.lock() {
                let accepted = s
                    .models
                    .entry(event.session_id.clone())
                    .or_default()
                    .receive(event.clone());
                let process_output = matches!(
                    &event.payload,
                    firmius_core::SessionEventPayload::Agent {
                        event: AgentEvent::ProcessOutput { .. },
                        ..
                    }
                );
                let turn_finished = matches!(
                    &event.payload,
                    firmius_core::SessionEventPayload::Agent {
                        event: AgentEvent::TurnFinished,
                        ..
                    }
                );
                if accepted
                    && !process_output
                    && !turn_finished
                    && let Some(snapshot) = s.snapshots.get_mut(&event.session_id)
                {
                    snapshot.live_events.push(event);
                }
            }
        }
        DaemonEvent::SessionStatus(status) => {
            if let Ok(mut s) = state.lock() {
                s.apply_status(&status);
            }
        }
        DaemonEvent::PermissionRequested(request) => {
            let visible = request.clone();
            if let Ok(mut s) = state.lock() {
                s.pending_permissions
                    .retain(|r| r.request_id != request.request_id);
                s.pending_permissions.push(request);
            }
            invoke(weak, move |ui| {
                show_permission(ui, &visible);
            });
        }
        DaemonEvent::PermissionResolved { request_id, .. } => {
            if let Ok(mut s) = state.lock() {
                s.pending_permissions.retain(|r| r.request_id != request_id);
            }
            let next = state
                .lock()
                .ok()
                .and_then(|state| state.pending_permissions.first().cloned());
            invoke(weak, move |ui| {
                if ui.get_permission_request_id() == request_id.to_string() {
                    if let Some(next) = next {
                        show_permission(ui, &next);
                    } else {
                        ui.set_permission_open(false);
                    }
                }
            });
        }
        _ => {}
    }
}
pub(super) fn mutate_active(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    success: &'static str,
    request: impl FnOnce(&SessionSnapshot) -> Result<Request, String> + Send + 'static,
) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session first");
        return;
    };
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, snapshot) = shared_snapshot(&state, &active).await?;
                let request = request(&snapshot)?;
                client
                    .request(request)
                    .await
                    .map_err(|error| error.to_string())
            })
        });
        match result {
            Ok(response) => {
                let notice = match response {
                    Response::Ack => success.to_string(),
                    Response::TurnAccepted { .. } => "Turn accepted".to_string(),
                    Response::Rewound { removed } => format!("Rewound {removed} message(s)"),
                    Response::EditHistory { result } => result,
                    _ => "Daemon returned an unexpected response".to_string(),
                };
                invoke(weak, move |ui| ui.set_notice(notice.into()))
            }
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}
