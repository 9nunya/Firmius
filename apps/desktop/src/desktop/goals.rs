//! Desktop goals feature.
use super::*;

fn goal_status_label(status: firmius_core::GoalStatus) -> &'static str {
    match status {
        firmius_core::GoalStatus::Proposed => "Proposed",
        firmius_core::GoalStatus::Queued => "Queued",
        firmius_core::GoalStatus::Active => "Active",
        firmius_core::GoalStatus::Waiting => "Waiting",
        firmius_core::GoalStatus::Blocked => "Blocked",
        firmius_core::GoalStatus::Succeeded => "Succeeded",
        firmius_core::GoalStatus::Failed => "Failed",
        firmius_core::GoalStatus::Cancelling => "Cancelling",
        firmius_core::GoalStatus::Cancelled => "Cancelled",
    }
}

pub(super) fn prepare_create_goal(
    description: &str,
    check: &str,
    approval_required: bool,
) -> Result<CreateGoalRequest, String> {
    let description = description.trim().to_string();
    if description.is_empty() {
        return Err("Goal description is required".into());
    }
    let check = check.trim();
    let checks = if check.is_empty() {
        Vec::new()
    } else {
        vec![
            firmius_core::GoalCheck::try_command(check)
                .map_err(|error| format!("Goal check is not a valid command: {error}"))?,
        ]
    };
    Ok(CreateGoalRequest {
        description: description.clone(),
        success_conditions: vec![description],
        owner: GoalOwner::User {
            user_id: "desktop".into(),
        },
        provenance: GoalProvenance {
            actor: GoalActor::User {
                user_id: "desktop".into(),
            },
            source: GoalSource::ExplicitCommand,
            created_at: Utc::now(),
        },
        checks,
        deadline: None,
        budget: None,
        approval_required,
        client_request_id: None,
    })
}

pub(super) fn goal_action_hint(goal_id: &firmius_core::GoalId, revision: u64) -> String {
    format!(
        "use /goal:activate {goal_id} {revision} · /goal:cancel {goal_id} {revision} · /goal:approve {goal_id} {revision} · /goal:reject {goal_id} {revision}"
    )
}

pub(super) fn refresh_goals(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session before inspecting goals");
        return;
    };
    let selected = state.lock().ok().and_then(|state| state.last_goal_id);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, _) = shared_snapshot(&state, &active).await?;
                match client
                    .request(Request::Goal(
                        GoalRequest::List(ListGoalsRequest::default()),
                    ))
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Goal(GoalResponse::Listed { goals, .. }) => Ok(goals),
                    other => Err(format!(
                        "daemon returned an unexpected goal response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok(goals) => {
                invoke(weak, move |ui| {
                    let rows = if goals.is_empty() {
                        vec![transcript_row(
                            "GOALS",
                            "No durable goals exist yet.",
                            "assistant",
                            "ready",
                        )]
                    } else {
                        goals
                            .into_iter()
                            .map(|goal| {
                                let status = goal_status_label(goal.status);
                                let selected_mark = selected
                                    .filter(|id| *id == goal.id)
                                    .map(|_| "Selected · ")
                                    .unwrap_or_default();
                                let approval = if goal.approval.required {
                                    "approval required · "
                                } else {
                                    ""
                                };
                                let conditions = if goal.success_conditions.is_empty() {
                                    "no success conditions".to_string()
                                } else {
                                    goal.success_conditions.join("; ")
                                };
                                transcript_row(
                                    "GOAL",
                                    format!(
                                        "{}\nID: {}\nSuccess: {conditions}",
                                        goal.description, goal.id
                                    ),
                                    "tool",
                                    format!(
                                        "{selected_mark}{status} · {approval}revision {} · {}",
                                        goal.revision,
                                        goal_action_hint(&goal.id, goal.revision)
                                    ),
                                )
                            })
                            .collect()
                    };
                    ui.set_active_view("work".into());
                    ui.invoke_show_document("Goals".into(), ModelRc::new(VecModel::from(rows)));
                    ui.set_notice("Goals refreshed".into());
                });
            }
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

pub(super) fn mutate_goal(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    action: String,
    goal_id: String,
    expected_revision: u64,
) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session before updating a goal");
        return;
    };
    let goal_id = match firmius_core::GoalId::parse(&goal_id) {
        Ok(goal_id) => goal_id,
        Err(_) => {
            set_notice(ui, "Goal id must be a valid UUID from the goal inventory");
            return;
        }
    };
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, _) = shared_snapshot(&state, &active).await?;
                let request = match action.as_str() {
                    "activate" => Request::Goal(GoalRequest::Activate(ActivateGoalRequest {
                        goal_id,
                        actor: None,
                        expected_revision,
                        client_request_id: None,
                    })),
                    "cancel" => Request::Goal(GoalRequest::Cancel(CancelGoalRequest {
                        goal_id,
                        actor: None,
                        reason: Some("Cancelled from Firmius desktop".into()),
                        expected_revision,
                        client_request_id: None,
                    })),
                    "approve" | "reject" => {
                        Request::Goal(GoalRequest::Approve(ApproveGoalRequest {
                            goal_id,
                            decision: if action == "approve" {
                                ApprovalDecision::Approve
                            } else {
                                ApprovalDecision::Reject
                            },
                            actor: None,
                            approval_id: None,
                            reason: Some(format!("{action}d from Firmius desktop")),
                            expected_revision,
                            client_request_id: None,
                        }))
                    }
                    _ => return Err(format!("unsupported goal action: {action}")),
                };
                match client
                    .request(request)
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Goal(_) => Ok(goal_id),
                    other => Err(format!(
                        "daemon returned an unexpected goal response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok(goal_id) => invoke(weak, move |ui| {
                if let Ok(mut state) = state.lock() {
                    state.last_goal_id = Some(goal_id);
                }
                let label = match action.as_str() {
                    "activate" => "activated",
                    "cancel" => "cancelled",
                    "approve" => "approved",
                    "reject" => "rejected",
                    _ => "updated",
                };
                ui.set_notice(format!("Goal {label}").into());
                ui.invoke_run_command("goals".into());
            }),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

pub(super) fn create_goal(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    description: String,
    check: String,
    activate: bool,
) {
    let request = match prepare_create_goal(&description, &check, false) {
        Ok(request) => request,
        Err(error) => {
            set_notice(ui, error);
            return;
        }
    };
    let Some(active) = state.lock().ok().and_then(|state| state.active_session()) else {
        set_notice(ui, "Attach a session before creating a goal");
        return;
    };
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, _) = shared_snapshot(&state, &active).await?;
                let response = client
                    .request(Request::Goal(GoalRequest::Create(request)))
                    .await
                    .map_err(|error| error.to_string())?;
                match response {
                    Response::Goal(GoalResponse::Created(goal)) if activate => {
                        if goal.approval.required {
                            return Ok((goal.id, false, true));
                        }
                        let goal_id = goal.id;
                        let expected_revision = goal.revision;
                        match client
                            .request(Request::Goal(GoalRequest::Activate(ActivateGoalRequest {
                                goal_id,
                                actor: None,
                                expected_revision,
                                client_request_id: None,
                            })))
                            .await
                            .map_err(|error| error.to_string())?
                        {
                            Response::Goal(GoalResponse::Activated(goal)) => {
                                Ok((goal.id, true, false))
                            }
                            other => Err(format!(
                                "goal was created but could not be activated: unexpected response {other:?}"
                            )),
                        }
                    }
                    Response::Goal(GoalResponse::Created(goal)) => Ok((goal.id, false, false)),
                    other => Err(format!(
                        "daemon returned an unexpected goal response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok((goal_id, activated, approval_blocked)) => invoke(weak, move |ui| {
                if let Ok(mut state) = state.lock() {
                    state.last_goal_id = Some(goal_id);
                }
                ui.set_goal_open(false);
                ui.set_goal_description("".into());
                ui.set_goal_check("".into());
                ui.set_notice(
                    if approval_blocked {
                        format!(
                            "Goal {} created; approval is required before activation (revision is in the goal inventory)",
                            goal_id.as_uuid()
                        )
                    } else if activated {
                        format!("Goal {} created and activated", goal_id.as_uuid())
                    } else {
                        format!("Goal {} created", goal_id.as_uuid())
                    }
                    .into(),
                );
                ui.invoke_run_command("goals".into());
            }),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn create_goal_requires_description_and_uses_it_as_success_condition() {
        assert!(prepare_create_goal("  ", "", false).is_err());
        let request = prepare_create_goal("Ship the inspector", "", false).unwrap();
        assert_eq!(request.description, "Ship the inspector");
        assert_eq!(request.success_conditions, ["Ship the inspector"]);
        assert!(request.checks.is_empty());
        assert!(!request.approval_required);
    }

    #[test]
    fn create_goal_parses_verification_command_and_rejects_malformed_quotes() {
        let request =
            prepare_create_goal("Cover goals", "cargo test -p firmius-desktop", true).unwrap();
        assert_eq!(request.checks.len(), 1);
        assert!(request.approval_required);
        assert!(prepare_create_goal("Cover goals", "cargo test \\", false).is_err());
    }

    #[test]
    fn inventory_actions_name_the_selected_identity_and_current_revision() {
        let id = firmius_core::GoalId::parse("5e1fd186-42f8-4629-b221-995a2a9b27c3").unwrap();
        let hint = goal_action_hint(&id, 7);
        assert!(hint.contains("goal:activate 5e1fd186-42f8-4629-b221-995a2a9b27c3 7"));
        assert!(hint.contains("goal:cancel 5e1fd186-42f8-4629-b221-995a2a9b27c3 7"));
        assert_eq!(
            goal_status_label(firmius_core::GoalStatus::Succeeded),
            "Succeeded"
        );
        assert_eq!(
            goal_status_label(firmius_core::GoalStatus::Cancelling),
            "Cancelling"
        );
    }
}
