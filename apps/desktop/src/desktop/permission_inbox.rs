//! Permission Inbox bindings.
use super::*;

pub(super) fn parse_permission_mode(mode: &str) -> Result<firmius_core::PermissionMode, String> {
    match mode.trim().to_ascii_lowercase().as_str() {
        "default" => Ok(firmius_core::PermissionMode::Default),
        "auto" => Ok(firmius_core::PermissionMode::Auto),
        "yolo" => Ok(firmius_core::PermissionMode::Yolo),
        other => Err(format!(
            "Permission mode must be default, auto, or yolo (not {other})"
        )),
    }
}

pub(super) fn gated_permission_mode(
    selected: &firmius_core::PermissionMode,
    current: &firmius_core::PermissionPolicy,
) -> Result<(), String> {
    if matches!(selected, firmius_core::PermissionMode::Yolo) && !current.yolo_confirmed {
        return Err(
            "YOLO mode has not been explicitly confirmed in the permission policy editor".into(),
        );
    }
    Ok(())
}

pub(super) fn parse_permission_decision(
    decision: &str,
) -> Result<firmius_core::PermissionDecision, String> {
    match decision.trim().to_ascii_lowercase().as_str() {
        "allow" => Ok(firmius_core::PermissionDecision::Allow),
        "deny" => Ok(firmius_core::PermissionDecision::Deny),
        other => Err(format!("Unsupported permission decision: {other}")),
    }
}

pub(super) fn permission_queue_index(
    pending: &[firmius_core::PendingPermissionRequest],
    request_id: &str,
) -> Option<usize> {
    pending
        .iter()
        .position(|request| request.request_id.to_string() == request_id)
}

pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_resolve_permission(move |decision| {
            let decision = decision.to_string();
            let Some(current_ui) = weak.upgrade() else {
                return;
            };
            let request_id = current_ui.get_permission_request_id().to_string();
            let parsed = match parse_permission_decision(&decision) {
                Ok(decision) => decision,
                Err(error) => {
                    set_notice(&current_ui, error);
                    return;
                }
            };
            let pending = match state.lock() {
                Ok(state) => state
                    .pending_permissions
                    .iter()
                    .find(|request| request.request_id.to_string() == request_id)
                    .cloned(),
                Err(_) => {
                    set_notice(&current_ui, "Permission queue is unavailable");
                    return;
                }
            };
            let Some(request) = pending else {
                set_notice(
                    &current_ui,
                    "Permission request expired; refresh the session",
                );
                current_ui.set_permission_open(false);
                return;
            };
            let state = state.clone();
            let weak = current_ui.as_weak();
            let resolved_request_id = request.request_id;
            match runtime() {
                Ok(runtime) => {
                    let _ = runtime.spawn(async move {
                        let result = async {
                            let client =
                                crate::daemon::client_for(&state, Some(&request.session_id))
                                    .await?;
                            client
                                .resolve_permission(PermissionResolution {
                                    request_id: request.request_id,
                                    nonce: request.nonce,
                                    session_id: request.session_id,
                                    agent_id: request.agent_id,
                                    tool: request.tool,
                                    action_digest: request.action_digest,
                                    expected_revision: request.expected_revision,
                                    decision: parsed,
                                })
                                .await
                                .map_err(|e| e.to_string())
                        }
                        .await;
                        invoke(weak, move |ui| match result {
                            Ok(()) => match state.lock() {
                                Ok(mut state) => {
                                    state.pending_permissions.retain(|pending| {
                                        pending.request_id != resolved_request_id
                                    });
                                    show_permission_queue(ui, &state);
                                }
                                Err(_) => {
                                    ui.set_notice("Permission queue is unavailable".into());
                                }
                            },
                            Err(e) => ui.set_notice(
                                format!("Decision outcome unknown—refresh before retrying: {e}")
                                    .into(),
                            ),
                        });
                    });
                }
                Err(_) => set_notice(&current_ui, "Permission runtime is unavailable"),
            }
        });
    }
}

pub(super) fn show_permission(ui: &MainWindow, request: &firmius_core::PendingPermissionRequest) {
    show_permission_with_queue(ui, request, None);
}

pub(super) fn show_permission_with_queue(
    ui: &MainWindow,
    request: &firmius_core::PendingPermissionRequest,
    pending: Option<&[firmius_core::PendingPermissionRequest]>,
) {
    let index = pending
        .and_then(|queue| permission_queue_index(queue, &request.request_id.to_string()))
        .unwrap_or(0);
    let total = pending.map(|queue| queue.len().max(1)).unwrap_or(1);
    ui.set_permission_request_id(request.request_id.to_string().into());
    ui.set_permission_tool(
        format!(
            "{} · session {} · agent {} · {} of {total}",
            request.tool,
            request.session_id,
            request.agent_id,
            index + 1
        )
        .into(),
    );
    let detail = if request.descriptor.actions.is_empty() {
        format!(
            "{} · review the requested operation\nnonce {} · digest {} · revision {}",
            request.descriptor.operation,
            request.nonce,
            request.action_digest,
            request.expected_revision
        )
    } else {
        let previews = request
            .descriptor
            .actions
            .iter()
            .map(|action| action.preview.clone())
            .collect::<Vec<_>>()
            .join("\n");
        format!(
            "{previews}\nnonce {} · digest {} · revision {}",
            request.nonce, request.action_digest, request.expected_revision
        )
    };
    ui.set_permission_detail(detail.into());
    ui.set_permission_open(true);
}

pub(super) fn show_permission_queue(ui: &MainWindow, state: &DesktopState) {
    if let Some(next) = state.pending_permissions.first() {
        show_permission_with_queue(ui, next, Some(&state.pending_permissions));
    } else {
        ui.set_permission_open(false);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(id: &str) -> firmius_core::PendingPermissionRequest {
        let id = firmius_core::GoalId::parse(id).expect("uuid").as_uuid();
        firmius_core::PendingPermissionRequest {
            request_id: id,
            nonce: firmius_core::GoalId::parse("00000000-0000-0000-0000-000000000000")
                .expect("nil")
                .as_uuid(),
            session_id: "session".into(),
            agent_id: "agent".into(),
            tool: "edit".into(),
            descriptor: firmius_core::ToolActionDescriptor {
                tool: "edit".into(),
                operation: "edit".into(),
                actions: Vec::new(),
                require_all_actions: true,
                unknown: false,
            },
            action_digest: "digest".into(),
            expected_revision: 3,
        }
    }

    #[test]
    fn permission_mode_requires_named_values_and_yolo_confirmation() {
        assert_eq!(
            parse_permission_mode("Default"),
            Ok(firmius_core::PermissionMode::Default)
        );
        assert_eq!(
            parse_permission_mode("auto"),
            Ok(firmius_core::PermissionMode::Auto)
        );
        assert!(parse_permission_mode("custom").is_err());
        let mut policy = firmius_core::PermissionPolicy::default();
        assert!(gated_permission_mode(&firmius_core::PermissionMode::Yolo, &policy).is_err());
        policy.yolo_confirmed = true;
        assert!(gated_permission_mode(&firmius_core::PermissionMode::Yolo, &policy).is_ok());
    }

    #[test]
    fn permission_decisions_are_explicit_and_reject_unknown_verbs() {
        assert_eq!(
            parse_permission_decision("allow"),
            Ok(firmius_core::PermissionDecision::Allow)
        );
        assert_eq!(
            parse_permission_decision("DENY"),
            Ok(firmius_core::PermissionDecision::Deny)
        );
        assert!(parse_permission_decision("maybe").is_err());
        assert!(parse_permission_decision("").is_err());
    }

    #[test]
    fn permission_queue_is_fifo_and_identity_matched() {
        let first = "11111111-1111-1111-1111-111111111111";
        let second = "22222222-2222-2222-2222-222222222222";
        let queue = vec![request(first), request(second)];
        assert_eq!(permission_queue_index(&queue, first), Some(0));
        assert_eq!(permission_queue_index(&queue, second), Some(1));
        assert_eq!(permission_queue_index(&queue, "missing"), None);
    }
}
