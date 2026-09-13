//! Exhaustive unit tests for the durable goal coordinator.

use super::*;
use crate::goal::{
    CheckEvaluation, CheckState, CheckVerification, Goal, GoalActor, GoalBudget, GoalCheck,
    GoalOwner, GoalProvenance, GoalSource, GoalStatus, GoalTransition,
};
use chrono::{Duration, Utc};
use serde_json::Value;
use uuid::Uuid;

fn actor() -> GoalActor {
    GoalActor::User {
        user_id: "user".into(),
    }
}

#[test]
fn candidate_submission_replays_and_rejects_worker_review_claim() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("candidate"), worker());
    let run = activate_goal(&mut c, queued.goal_id);
    let request_id = Uuid::new_v4();
    let spec = CandidateSpec {
        goal_id: queued.goal_id,
        expected_goal_revision: c.goal(queued.goal_id).unwrap().revision,
        run_id: run.id,
        generation: run.generation,
        actor: GoalActor::Agent {
            agent_id: "worker".into(),
        },
        result: Value::String("done".into()),
        evidence: vec!["artifact://evidence".into()],
        artifact_ids: vec![],
        work_result_ids: vec![],
        verification: CheckVerification::SelfVerified,
        client_identity: Some("agent:worker".into()),
        client_request_id: Some(request_id),
    };
    let mut stale = spec.clone();
    stale.expected_goal_revision += 1;
    let before_stale = c.clone();
    assert!(matches!(
        c.submit_candidate(c.revision, stale),
        Err(CoordinatorError::Invalid(message)) if message.contains("stale goal revision")
    ));
    assert_eq!(c, before_stale);
    let first = c.submit_candidate(c.revision, spec.clone()).unwrap();
    let revision = c.revision;
    let replay = c.submit_candidate(revision, spec).unwrap();
    assert_eq!(first, replay);
    assert_eq!(c.revision, revision);

    let conflict = CandidateSpec {
        goal_id: queued.goal_id,
        expected_goal_revision: c.goal(queued.goal_id).unwrap().revision,
        run_id: first.id,
        generation: first.generation,
        actor: GoalActor::Agent {
            agent_id: "worker".into(),
        },
        result: Value::String("changed".into()),
        evidence: vec!["artifact://evidence".into()],
        artifact_ids: vec![],
        work_result_ids: vec![],
        verification: CheckVerification::SelfVerified,
        client_identity: Some("agent:worker".into()),
        client_request_id: Some(request_id),
    };
    assert!(matches!(
        c.submit_candidate(revision, conflict),
        Err(CoordinatorError::IdempotencyConflict)
    ));

    let mut invalid = c.clone();
    let bad = CandidateSpec {
        goal_id: queued.goal_id,
        expected_goal_revision: invalid.goal(queued.goal_id).unwrap().revision,
        run_id: first.id,
        generation: first.generation,
        actor: GoalActor::Agent {
            agent_id: "worker".into(),
        },
        result: Value::Null,
        evidence: vec![],
        artifact_ids: vec![],
        work_result_ids: vec![],
        verification: CheckVerification::IndependentlyVerified,
        client_identity: None,
        client_request_id: None,
    };
    assert!(invalid.submit_candidate(invalid.revision, bad).is_err());
}

#[test]
fn outcome_dependency_requires_matching_durable_outcome() {
    let mut c = GoalCoordinator::new();
    let prerequisite = enqueue_ready(&mut c, proposed("review"), parent_agent());
    let dependent = enqueue_ready(&mut c, proposed("follow up"), worker());
    c.add_dependency_with_kind(
        c.revision,
        dependent.goal_id,
        prerequisite.goal_id,
        DependencyKind::GoalCondition {
            condition: "outcome:approved".into(),
        },
    )
    .unwrap();
    let run = activate_goal(&mut c, prerequisite.goal_id);
    satisfy_checks(&mut c, prerequisite.goal_id);
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: prerequisite.goal_id,
            run_id: run.id,
            generation: run.generation,
            actor: actor(),
            outcome: CompletionOutcome::Success {
                summary: "reviewed".into(),
            },
            outcome_label: Some("approved".into()),
            client_identity: None,
            client_request_id: None,
        },
    )
    .unwrap();
    assert!(
        c.activate(
            c.revision,
            ActivateSpec {
                goal_id: dependent.goal_id,
                expected_goal_revision: None,
                actor: actor(),
                client_identity: None,
                client_request_id: None,
            }
        )
        .is_ok()
    );
}

#[test]
fn failed_and_cancelled_runs_do_not_satisfy_outcome_dependency() {
    for cancelled in [false, true] {
        let mut c = GoalCoordinator::new();
        let prerequisite = enqueue_ready(&mut c, proposed("review"), parent_agent());
        let dependent = enqueue_ready(&mut c, proposed("follow up"), worker());
        c.add_dependency_with_kind(
            c.revision,
            dependent.goal_id,
            prerequisite.goal_id,
            DependencyKind::GoalCondition {
                condition: "outcome:approved".into(),
            },
        )
        .unwrap();
        let run = activate_goal(&mut c, prerequisite.goal_id);
        if cancelled {
            c.cancel(
                c.revision,
                CancelSpec {
                    goal_id: prerequisite.goal_id,
                    actor: actor(),
                    reason: "stop".into(),
                    cascade: ChildCascadePolicy::Cancel,
                    client_identity: None,
                    client_request_id: None,
                },
            )
            .unwrap();
            c.settle_cancel(
                c.revision,
                actor(),
                prerequisite.goal_id,
                run.id,
                run.generation,
            )
            .unwrap();
        } else {
            satisfy_checks(&mut c, prerequisite.goal_id);
            c.complete(
                c.revision,
                CompleteSpec {
                    goal_id: prerequisite.goal_id,
                    run_id: run.id,
                    generation: run.generation,
                    actor: actor(),
                    outcome: CompletionOutcome::Failure {
                        reason: "rejected".into(),
                        retry: false,
                    },
                    outcome_label: None,
                    client_identity: None,
                    client_request_id: None,
                },
            )
            .unwrap();
        }
        let error = c
            .activate(
                c.revision,
                ActivateSpec {
                    goal_id: dependent.goal_id,
                    expected_goal_revision: None,
                    actor: actor(),
                    client_identity: None,
                    client_request_id: None,
                },
            )
            .unwrap_err();
        assert!(!matches!(error, CoordinatorError::Invalid(_)));
        assert_ne!(
            c.goal(dependent.goal_id).unwrap().status,
            GoalStatus::Active
        );
    }
}

#[test]
fn rollback_activation_releases_slot_and_requeues_goal() {
    let mut c = GoalCoordinator::new();
    let target = worker();
    let queued = enqueue_ready(&mut c, proposed("rollback"), target.clone());
    let run = c
        .activate_next(c.revision, GoalActor::System, target.clone())
        .unwrap()
        .unwrap();
    assert!(c.slot(&target).is_some());
    c.rollback_activation(
        c.revision,
        queued.goal_id,
        run.id,
        run.generation,
        "agent unavailable".into(),
    )
    .unwrap();
    assert!(c.slot(&target).is_none());
    assert_eq!(c.goal(queued.goal_id).unwrap().status, GoalStatus::Queued);
    assert_eq!(c.queue_for(&target).len(), 1);
    c.validate().unwrap();
}

fn provenance() -> GoalProvenance {
    GoalProvenance {
        actor: actor(),
        source: GoalSource::UserRequest,
        created_at: Utc::now(),
    }
}

fn proposed(description: &str) -> Goal {
    let mut goal = Goal::new(
        description,
        vec!["done".into()],
        GoalOwner::User {
            user_id: "user".into(),
        },
        provenance(),
    )
    .unwrap();
    goal.transition(
        actor(),
        GoalTransition::AddCheck(GoalCheck::command("true")),
    )
    .unwrap();
    goal
}

fn worker() -> AgentRef {
    AgentRef::new("session-a", "worker")
}

fn parent_agent() -> AgentRef {
    AgentRef::new("session-a", "parent")
}

fn pass(check_id: &str) -> CheckEvaluation {
    CheckEvaluation {
        check_id: check_id.into(),
        state: CheckState::Passed,
        inputs: Value::Null,
        output: Some(Value::String("ok".into())),
        evaluated_at: Utc::now(),
        actor: GoalActor::System,
        evidence: vec!["artifact://result".into()],
        verification: CheckVerification::Reviewed,
        review: None,
    }
}

fn enqueue_ready(coord: &mut GoalCoordinator, goal: Goal, target: AgentRef) -> EnqueueResult {
    let rev = coord.revision;
    coord
        .enqueue(rev, actor(), goal, target, EnqueueSpec::default())
        .unwrap()
}

fn activate_goal(coord: &mut GoalCoordinator, goal_id: crate::goal::GoalId) -> GoalRun {
    let rev = coord.revision;
    coord
        .activate(
            rev,
            ActivateSpec {
                goal_id,
                expected_goal_revision: None,
                actor: actor(),
                client_identity: None,
                client_request_id: None,
            },
        )
        .unwrap()
}

fn satisfy_checks(coord: &mut GoalCoordinator, goal_id: crate::goal::GoalId) {
    let goal = coord.goal(goal_id).unwrap().clone();
    let check_id = goal.checks[0].id.clone();
    let rev = coord.revision;
    coord
        .apply_goal_transition(
            rev,
            goal_id,
            goal.revision,
            GoalActor::System,
            GoalTransition::Evaluate(pass(&check_id)),
        )
        .unwrap();
}

#[test]
fn enqueue_does_not_activate_and_queues_multiple_goals() {
    let mut c = GoalCoordinator::new();
    let a = enqueue_ready(&mut c, proposed("one"), worker());
    let b = enqueue_ready(&mut c, proposed("two"), worker());
    assert_ne!(a.goal_id, b.goal_id);
    assert_eq!(c.goal(a.goal_id).unwrap().status, GoalStatus::Queued);
    assert_eq!(c.goal(b.goal_id).unwrap().status, GoalStatus::Queued);
    assert!(c.slot(&worker()).is_none());
    assert_eq!(c.queue_for(&worker()).len(), 2);
    assert_eq!(c.pending_outbox().len(), 0);
}

#[test]
fn activate_acquires_unique_slot_and_rejects_second() {
    let mut c = GoalCoordinator::new();
    let first = enqueue_ready(&mut c, proposed("one"), worker());
    let second = enqueue_ready(&mut c, proposed("two"), worker());
    let run = activate_goal(&mut c, first.goal_id);
    assert_eq!(c.goal(first.goal_id).unwrap().status, GoalStatus::Active);
    assert_eq!(c.slot(&worker()).unwrap().run_id, run.id);
    assert_eq!(c.slot(&worker()).unwrap().generation, 1);
    assert_eq!(c.pending_outbox().len(), 1);
    match &c.pending_outbox()[0].kind {
        OutboxKind::Dispatch {
            goal_id,
            generation,
            ..
        } => {
            assert_eq!(*goal_id, first.goal_id);
            assert_eq!(*generation, 1);
        }
        other => panic!("unexpected outbox {other:?}"),
    }
    let before = c.clone();
    let err = c
        .activate(
            c.revision,
            ActivateSpec {
                goal_id: second.goal_id,
                expected_goal_revision: None,
                actor: actor(),
                client_identity: None,
                client_request_id: None,
            },
        )
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::SlotOccupied { .. }));
    assert_eq!(c, before, "rejected mutation must not change state");
    assert_eq!(c.queue_for(&worker()).len(), 1);
}

#[test]
fn stale_generation_is_rejected() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("one"), worker());
    let run = activate_goal(&mut c, queued.goal_id);
    let before = c.clone();
    let err = c
        .yield_run(
            c.revision,
            YieldSpec {
                goal_id: queued.goal_id,
                run_id: run.id,
                generation: run.generation.saturating_add(9),
                actor: actor(),
                reason: WaitReason::Approval,
            },
        )
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::StaleGeneration { .. }));
    assert_eq!(c, before);
}

#[test]
fn yield_releases_slot_and_waiting_holds_no_execution() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("one"), worker());
    let run = activate_goal(&mut c, queued.goal_id);
    c.yield_run(
        c.revision,
        YieldSpec {
            goal_id: queued.goal_id,
            run_id: run.id,
            generation: run.generation,
            actor: actor(),
            reason: WaitReason::Child {
                child_goal_id: queued.goal_id,
            },
        },
    )
    .unwrap();
    assert_eq!(c.goal(queued.goal_id).unwrap().status, GoalStatus::Waiting);
    assert!(c.slot(&worker()).is_none());
    assert!(c.open_run(&worker()).is_none());
    assert_eq!(c.run(run.id).unwrap().state, GoalRunState::Waiting);
    assert!(
        c.pending_outbox()
            .iter()
            .all(|e| e.state != OutboxState::Pending
                || !matches!(e.kind, OutboxKind::Dispatch { .. }))
    );
}

#[test]
fn verification_wait_blocks_next_activation() {
    let mut c = GoalCoordinator::new();
    let first = enqueue_ready(&mut c, proposed("m1"), worker());
    let second = enqueue_ready(&mut c, proposed("m2"), worker());
    let run = activate_goal(&mut c, first.goal_id);
    c.yield_run(
        c.revision,
        YieldSpec {
            goal_id: first.goal_id,
            run_id: run.id,
            generation: run.generation,
            actor: actor(),
            reason: WaitReason::Verification,
        },
    )
    .unwrap();
    assert!(c.slot(&worker()).is_none());
    let next = c.activate_next(c.revision, actor(), worker()).unwrap();
    assert!(next.is_none(), "verification hold must gate N+1");
    assert_eq!(c.goal(second.goal_id).unwrap().status, GoalStatus::Queued);
    satisfy_checks(&mut c, first.goal_id);
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: first.goal_id,
            run_id: run.id,
            generation: run.generation,
            actor: actor(),
            outcome: CompletionOutcome::Success {
                summary: "ok".into(),
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    let next = c
        .activate_next(c.revision, actor(), worker())
        .unwrap()
        .expect("m2 should activate after verified success");
    assert_eq!(next.goal_id, second.goal_id);
}

#[test]
fn complete_success_requires_checks_and_releases_slot() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("one"), worker());
    let run = activate_goal(&mut c, queued.goal_id);
    let before = c.clone();
    let err = c
        .complete(
            c.revision,
            CompleteSpec {
                goal_id: queued.goal_id,
                run_id: run.id,
                generation: run.generation,
                actor: actor(),
                outcome: CompletionOutcome::Success {
                    summary: "premature".into(),
                },
                client_identity: None,
                client_request_id: None,
                outcome_label: None,
            },
        )
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::ChecksNotSatisfied));
    assert_eq!(c, before);
    satisfy_checks(&mut c, queued.goal_id);
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: queued.goal_id,
            run_id: run.id,
            generation: run.generation,
            actor: actor(),
            outcome: CompletionOutcome::Success {
                summary: "ok".into(),
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    assert_eq!(
        c.goal(queued.goal_id).unwrap().status,
        GoalStatus::Succeeded
    );
    assert!(c.slot(&worker()).is_none());
    assert!(
        c.assignment(queued.assignment_id)
            .unwrap()
            .released_at
            .is_some()
    );
}

#[test]
fn retryable_failure_requeues_within_budget() {
    let mut c = GoalCoordinator::new();
    let mut spec = EnqueueSpec::default();
    spec.retry_safe = true;
    spec.max_attempts = 3;
    let queued = c
        .enqueue(0, actor(), proposed("retry"), worker(), spec)
        .unwrap();
    let run = activate_goal(&mut c, queued.goal_id);
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: queued.goal_id,
            run_id: run.id,
            generation: run.generation,
            actor: actor(),
            outcome: CompletionOutcome::Failure {
                reason: "tests failed".into(),
                retry: true,
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    assert_eq!(c.goal(queued.goal_id).unwrap().status, GoalStatus::Queued);
    assert!(c.slot(&worker()).is_none());
    assert_eq!(c.queue_for(&worker()).len(), 1);
    let run2 = activate_goal(&mut c, queued.goal_id);
    assert_eq!(run2.attempt, 2);
    assert_eq!(run2.generation, 2);
}

#[test]
fn cancel_queued_is_immediate_and_active_is_fenced() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("queued"), worker());
    c.cancel(
        c.revision,
        CancelSpec {
            goal_id: queued.goal_id,
            actor: actor(),
            reason: "never mind".into(),
            cascade: ChildCascadePolicy::Cancel,
            client_identity: None,
            client_request_id: None,
        },
    )
    .unwrap();
    assert_eq!(
        c.goal(queued.goal_id).unwrap().status,
        GoalStatus::Cancelled
    );
    assert!(c.queue_for(&worker()).is_empty());

    let active = enqueue_ready(&mut c, proposed("active"), worker());
    let run = activate_goal(&mut c, active.goal_id);
    c.cancel(
        c.revision,
        CancelSpec {
            goal_id: active.goal_id,
            actor: actor(),
            reason: "stop".into(),
            cascade: ChildCascadePolicy::Cancel,
            client_identity: None,
            client_request_id: None,
        },
    )
    .unwrap();
    assert_eq!(
        c.goal(active.goal_id).unwrap().status,
        GoalStatus::Cancelling
    );
    assert_eq!(c.run(run.id).unwrap().state, GoalRunState::CancelRequested);
    assert!(c.slot(&worker()).is_some());
    assert!(
        c.pending_outbox()
            .iter()
            .any(|e| matches!(e.kind, OutboxKind::Cancel { .. }))
    );
    c.settle_cancel(c.revision, actor(), active.goal_id, run.id, run.generation)
        .unwrap();
    assert_eq!(
        c.goal(active.goal_id).unwrap().status,
        GoalStatus::Cancelled
    );
    assert!(c.slot(&worker()).is_none());
}

#[test]
fn parent_yield_and_child_completion_requeue_parent() {
    let mut c = GoalCoordinator::new();
    let parent = enqueue_ready(&mut c, proposed("parent"), parent_agent());
    let parent_run = activate_goal(&mut c, parent.goal_id);
    let mut spec = EnqueueSpec::default();
    spec.parent_goal_id = Some(parent.goal_id);
    spec.parent_run_id = Some(parent_run.id);
    spec.parent_generation = Some(parent_run.generation);
    spec.parent_yields = true;
    spec.priority_class = PriorityClass::ParentMilestone;
    let child = c
        .enqueue(c.revision, actor(), proposed("child"), worker(), spec)
        .unwrap();
    assert_eq!(c.goal(parent.goal_id).unwrap().status, GoalStatus::Waiting);
    assert!(c.slot(&parent_agent()).is_none());
    assert_eq!(c.goal(child.goal_id).unwrap().status, GoalStatus::Queued);
    let child_run = activate_goal(&mut c, child.goal_id);
    satisfy_checks(&mut c, child.goal_id);
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: child.goal_id,
            run_id: child_run.id,
            generation: child_run.generation,
            actor: actor(),
            outcome: CompletionOutcome::Success {
                summary: "milestone".into(),
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    assert_eq!(c.goal(parent.goal_id).unwrap().status, GoalStatus::Queued);
    assert!(
        c.outbox
            .values()
            .any(|e| matches!(e.kind, OutboxKind::MilestoneReady { .. }))
    );
    let parent2 = c
        .activate_next(c.revision, actor(), parent_agent())
        .unwrap()
        .expect("parent should be eligible after child success");
    assert_eq!(parent2.goal_id, parent.goal_id);
    assert_eq!(parent2.attempt, 2);
}

#[test]
fn same_agent_child_requires_parent_yield() {
    let mut c = GoalCoordinator::new();
    let parent = enqueue_ready(&mut c, proposed("parent"), worker());
    let parent_run = activate_goal(&mut c, parent.goal_id);
    let mut spec = EnqueueSpec::default();
    spec.parent_goal_id = Some(parent.goal_id);
    spec.parent_run_id = Some(parent_run.id);
    spec.parent_generation = Some(parent_run.generation);
    let before = c.clone();
    let err = c
        .enqueue(c.revision, actor(), proposed("child"), worker(), spec)
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::ParentMustYield));
    assert_eq!(c, before);
}

#[test]
fn stale_parent_yield_is_fenced() {
    let mut c = GoalCoordinator::new();
    let parent = enqueue_ready(&mut c, proposed("parent"), parent_agent());
    let parent_run = activate_goal(&mut c, parent.goal_id);
    let mut spec = EnqueueSpec::default();
    spec.parent_goal_id = Some(parent.goal_id);
    spec.parent_run_id = Some(parent_run.id);
    spec.parent_generation = Some(parent_run.generation.saturating_add(1));
    spec.parent_yields = true;
    let err = c
        .enqueue(c.revision, actor(), proposed("child"), worker(), spec)
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::ParentYieldFence));
}

#[test]
fn dependency_cycles_are_rejected() {
    let mut c = GoalCoordinator::new();
    let a = enqueue_ready(&mut c, proposed("a"), worker());
    let b = enqueue_ready(&mut c, proposed("b"), AgentRef::new("session-a", "other"));
    c.add_dependency(c.revision, a.goal_id, b.goal_id).unwrap();
    let before = c.clone();
    let err = c
        .add_dependency(c.revision, b.goal_id, a.goal_id)
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::CycleDetected));
    assert_eq!(c, before);
}

#[test]
fn wait_for_blocks_activation_until_predecessor_succeeds() {
    let mut c = GoalCoordinator::new();
    let pred = enqueue_ready(&mut c, proposed("pred"), parent_agent());
    let mut spec = EnqueueSpec::default();
    spec.wait_for = vec![pred.goal_id];
    let dependent = c
        .enqueue(c.revision, actor(), proposed("dep"), worker(), spec)
        .unwrap();
    let before = c.clone();
    let err = c
        .activate(
            c.revision,
            ActivateSpec {
                goal_id: dependent.goal_id,
                expected_goal_revision: None,
                actor: actor(),
                client_identity: None,
                client_request_id: None,
            },
        )
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::DependenciesNotReady));
    assert_eq!(c, before);
    let pred_run = activate_goal(&mut c, pred.goal_id);
    satisfy_checks(&mut c, pred.goal_id);
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: pred.goal_id,
            run_id: pred_run.id,
            generation: pred_run.generation,
            actor: actor(),
            outcome: CompletionOutcome::Success {
                summary: "pred".into(),
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    activate_goal(&mut c, dependent.goal_id);
    assert_eq!(
        c.goal(dependent.goal_id).unwrap().status,
        GoalStatus::Active
    );
}

#[test]
fn restart_interrupts_and_requeues_retry_safe_goals() {
    let mut c = GoalCoordinator::new();
    let mut spec = EnqueueSpec::default();
    spec.retry_safe = true;
    let queued = c
        .enqueue(0, actor(), proposed("live"), worker(), spec)
        .unwrap();
    let run = activate_goal(&mut c, queued.goal_id);
    c.record_step(c.revision, queued.goal_id, run.id, run.generation, Some(4))
        .unwrap();
    assert_eq!(c.steps_consumed(queued.goal_id), 1);
    assert_eq!(c.cost_consumed(queued.goal_id), 4);
    let report = c.reconcile(c.revision, 1).unwrap();
    assert_eq!(report.interrupted_runs, vec![run.id]);
    assert_eq!(report.requeued, vec![queued.goal_id]);
    assert_eq!(c.goal(queued.goal_id).unwrap().status, GoalStatus::Queued);
    assert!(c.slot(&worker()).is_none());
    assert_eq!(c.run(run.id).unwrap().state, GoalRunState::Interrupted);
    assert_eq!(c.steps_consumed(queued.goal_id), 1);
    assert_eq!(c.cost_consumed(queued.goal_id), 4);
    let before = c.clone();
    let err = c.reconcile(c.revision, 1).unwrap_err();
    assert!(matches!(err, CoordinatorError::EpochNotMonotonic { .. }));
    assert_eq!(c, before);
    let run2 = activate_goal(&mut c, queued.goal_id);
    assert_eq!(run2.attempt, 2);
    assert_eq!(run2.generation, 2);
    let err = c
        .complete(
            c.revision,
            CompleteSpec {
                goal_id: queued.goal_id,
                run_id: run.id,
                generation: run.generation,
                actor: actor(),
                outcome: CompletionOutcome::Success {
                    summary: "stale".into(),
                },
                client_identity: None,
                client_request_id: None,
                outcome_label: None,
            },
        )
        .unwrap_err();
    assert!(matches!(
        err,
        CoordinatorError::StaleGeneration { .. } | CoordinatorError::StaleRun
    ));
}

#[test]
fn non_retry_safe_interrupt_blocks() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("unsafe"), worker());
    activate_goal(&mut c, queued.goal_id);
    let report = c.reconcile(c.revision, 7).unwrap();
    assert_eq!(report.blocked, vec![queued.goal_id]);
    assert_eq!(c.goal(queued.goal_id).unwrap().status, GoalStatus::Blocked);
}

#[test]
fn hung_claim_expiry_interrupts_like_restart() {
    let mut c = GoalCoordinator::new();
    let mut spec = EnqueueSpec::default();
    spec.retry_safe = true;
    let queued = c
        .enqueue(0, actor(), proposed("hung"), worker(), spec)
        .unwrap();
    let run = activate_goal(&mut c, queued.goal_id);
    let expired = Utc::now() - Duration::seconds(1);
    c.heartbeat(c.revision, run.id, run.generation, Some(expired))
        .unwrap();
    let report = c.fence_expired(c.revision, Utc::now()).unwrap();
    assert_eq!(report.interrupted_runs, vec![run.id]);
    assert_eq!(c.goal(queued.goal_id).unwrap().status, GoalStatus::Queued);
}

#[test]
fn idempotency_replays_and_conflicts_on_fingerprint() {
    let mut c = GoalCoordinator::new();
    let request = Uuid::new_v4();
    let goal = proposed("idem");
    let mut spec = EnqueueSpec::default();
    spec.client_identity = Some("client".into());
    spec.client_request_id = Some(request);
    let first = c
        .enqueue(0, actor(), goal.clone(), worker(), spec.clone())
        .unwrap();
    let rev = c.revision;
    let replay = c
        .enqueue(99, actor(), goal.clone(), worker(), spec.clone())
        .unwrap();
    assert_eq!(first, replay);
    assert_eq!(c.revision, rev, "replay must not bump revision");
    spec.priority = 9;
    let err = c
        .enqueue(c.revision, actor(), goal.clone(), worker(), spec)
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::IdempotencyConflict));
}

#[test]
fn stale_coordinator_revision_is_rejected() {
    let mut c = GoalCoordinator::new();
    let before = c.clone();
    let err = c
        .enqueue(
            3,
            actor(),
            proposed("stale"),
            worker(),
            EnqueueSpec::default(),
        )
        .unwrap_err();
    assert!(matches!(
        err,
        CoordinatorError::StaleRevision {
            expected: 3,
            actual: 0
        }
    ));
    assert_eq!(c, before);
}

#[test]
fn snapshot_round_trip_and_direct_active_without_slot_fails_validate() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("snap"), worker());
    activate_goal(&mut c, queued.goal_id);
    let bytes = c.to_snapshot().unwrap();
    let restored = GoalCoordinator::from_snapshot(&bytes).unwrap();
    assert_eq!(restored, c);

    let mut broken = c.clone();
    broken.slots.clear();
    assert!(broken.validate().is_err());
}

#[test]
fn occupancy_claim_blocks_goal_activation() {
    let mut c = GoalCoordinator::new();
    c.claim_occupancy(0, worker(), "workgraph", OccupancyKind::WorkGraph)
        .unwrap();
    let queued = enqueue_ready(&mut c, proposed("blocked-by-wg"), worker());
    let err = c
        .activate(
            c.revision,
            ActivateSpec {
                goal_id: queued.goal_id,
                expected_goal_revision: None,
                actor: actor(),
                client_identity: None,
                client_request_id: None,
            },
        )
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::ClaimOccupied { .. }));
    c.release_claim(c.revision, &worker(), "workgraph").unwrap();
    activate_goal(&mut c, queued.goal_id);
    assert!(c.slot(&worker()).is_some());
}

#[test]
fn budget_is_monotonic_across_runs_and_activate_does_not_charge_a_step() {
    let mut c = GoalCoordinator::new();
    let mut goal = proposed("budgeted");
    goal.budget = Some(GoalBudget {
        max_cost: Some(10),
        max_steps: Some(2),
    });
    let mut spec = EnqueueSpec::default();
    spec.retry_safe = true;
    spec.max_attempts = 5;
    let queued = c.enqueue(0, actor(), goal, worker(), spec).unwrap();
    let run = activate_goal(&mut c, queued.goal_id);
    assert_eq!(c.steps_consumed(queued.goal_id), 0);
    c.record_step(c.revision, queued.goal_id, run.id, run.generation, Some(3))
        .unwrap();
    c.record_step(c.revision, queued.goal_id, run.id, run.generation, Some(3))
        .unwrap();
    let err = c
        .record_step(c.revision, queued.goal_id, run.id, run.generation, Some(1))
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::BudgetExceeded));
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: queued.goal_id,
            run_id: run.id,
            generation: run.generation,
            actor: actor(),
            outcome: CompletionOutcome::Failure {
                reason: "retry".into(),
                retry: true,
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    assert_eq!(c.steps_consumed(queued.goal_id), 2);
    let run2 = activate_goal(&mut c, queued.goal_id);
    let err = c
        .record_step(
            c.revision,
            queued.goal_id,
            run2.id,
            run2.generation,
            Some(1),
        )
        .unwrap_err();
    assert!(matches!(err, CoordinatorError::BudgetExceeded));
}

#[test]
fn late_outbox_ack_is_rejected_after_supersede() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("ack"), worker());
    let run = activate_goal(&mut c, queued.goal_id);
    let dispatch_id = c
        .pending_outbox()
        .iter()
        .find(|e| matches!(e.kind, OutboxKind::Dispatch { .. }))
        .unwrap()
        .id;
    c.cancel(
        c.revision,
        CancelSpec {
            goal_id: queued.goal_id,
            actor: actor(),
            reason: "stop".into(),
            cascade: ChildCascadePolicy::Cancel,
            client_identity: None,
            client_request_id: None,
        },
    )
    .unwrap();
    assert_eq!(c.outbox[&dispatch_id].state, OutboxState::Superseded);
    let err = c.acknowledge_outbox(c.revision, dispatch_id).unwrap_err();
    assert!(matches!(err, CoordinatorError::LateAck));
    let _ = run;
}

#[test]
fn queue_order_uses_class_then_priority_then_seq_and_ages() {
    let mut c = GoalCoordinator::new();
    let mut low = EnqueueSpec::default();
    low.priority_class = PriorityClass::SelfMaintenance;
    low.priority = 100;
    let low_id = c
        .enqueue(0, actor(), proposed("low"), worker(), low)
        .unwrap()
        .goal_id;
    let mut high = EnqueueSpec::default();
    high.priority_class = PriorityClass::User;
    high.priority = 1;
    let high_id = c
        .enqueue(c.revision, actor(), proposed("high"), worker(), high)
        .unwrap()
        .goal_id;
    let ordered: Vec<_> = c.queue_for(&worker()).iter().map(|e| e.goal_id).collect();
    assert_eq!(ordered, vec![high_id, low_id]);

    let mut aged = EnqueueSpec::default();
    aged.priority_class = PriorityClass::SelfMaintenance;
    aged.eligible_at = Some(Utc::now() - Duration::minutes(PRIORITY_AGING_MINUTES * 4));
    let aged_id = c
        .enqueue(c.revision, actor(), proposed("aged"), worker(), aged)
        .unwrap()
        .queue_id;
    let now = Utc::now();
    let aged_entry = &c.queue[&aged_id];
    assert_eq!(
        aged_entry.priority_class.age(4),
        PriorityClass::User,
        "four aging steps promote self-maintenance to user"
    );
    let _ = now;
}

#[test]
fn apply_goal_transition_refreshes_queue_revision() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("rev"), worker());
    let before_rev = c.queue_for(&worker())[0].expected_goal_revision;
    let goal = c.goal(queued.goal_id).unwrap().clone();
    c.apply_goal_transition(
        c.revision,
        queued.goal_id,
        goal.revision,
        actor(),
        GoalTransition::AddCheck(GoalCheck::command("cargo test")),
    )
    .unwrap();
    let after = c.queue_for(&worker())[0].expected_goal_revision;
    assert!(after > before_rev);
}

#[test]
fn validate_enforces_one_open_slot_per_agent() {
    let mut c = GoalCoordinator::new();
    let a = enqueue_ready(&mut c, proposed("a"), worker());
    activate_goal(&mut c, a.goal_id);
    let mut clone = c.clone();
    let extra_target = worker();
    clone.slots.insert(
        format!("{}-dup", extra_target.key()),
        clone.slot(&worker()).unwrap().clone(),
    );
    assert!(clone.validate().is_err());
}

#[test]
fn settle_cancel_supersedes_cancel_outbox_and_validates() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("cancel-live"), worker());
    let run = activate_goal(&mut c, queued.goal_id);
    c.cancel(
        c.revision,
        CancelSpec {
            goal_id: queued.goal_id,
            actor: actor(),
            reason: "stop".into(),
            cascade: ChildCascadePolicy::Cancel,
            client_identity: None,
            client_request_id: None,
        },
    )
    .unwrap();
    let cancel_id = c
        .pending_outbox()
        .iter()
        .find(|e| matches!(e.kind, OutboxKind::Cancel { .. }))
        .expect("an active cancel must publish a Cancel fence")
        .id;
    c.settle_cancel(c.revision, actor(), queued.goal_id, run.id, run.generation)
        .unwrap();
    assert_eq!(
        c.goal(queued.goal_id).unwrap().status,
        GoalStatus::Cancelled
    );
    assert_eq!(c.outbox[&cancel_id].state, OutboxState::Superseded);
    assert!(
        c.pending_outbox()
            .iter()
            .all(|e| !matches!(e.kind, OutboxKind::Cancel { .. })),
        "settled cancellation must not leave a pending Cancel fence"
    );
    c.validate().unwrap();
}

#[test]
fn reconcile_interrupting_cancellation_supersedes_cancel_outbox() {
    let mut c = GoalCoordinator::new();
    let queued = enqueue_ready(&mut c, proposed("cancel-live"), worker());
    let run = activate_goal(&mut c, queued.goal_id);
    c.cancel(
        c.revision,
        CancelSpec {
            goal_id: queued.goal_id,
            actor: actor(),
            reason: "stop".into(),
            cascade: ChildCascadePolicy::Cancel,
            client_identity: None,
            client_request_id: None,
        },
    )
    .unwrap();
    let cancel_id = c
        .pending_outbox()
        .iter()
        .find(|e| matches!(e.kind, OutboxKind::Cancel { .. }))
        .expect("an active cancel must publish a Cancel fence")
        .id;
    let report = c.reconcile(c.revision, 1).unwrap();
    assert_eq!(report.interrupted_runs, vec![run.id]);
    assert!(report.cancelled.contains(&queued.goal_id));
    assert_eq!(
        c.goal(queued.goal_id).unwrap().status,
        GoalStatus::Cancelled
    );
    assert_eq!(c.outbox[&cancel_id].state, OutboxState::Superseded);
    c.validate().unwrap();
}

#[test]
fn completion_is_fenced_per_attempt_but_terminal_attempt_is_idempotent() {
    let mut c = GoalCoordinator::new();
    let mut spec = EnqueueSpec::default();
    spec.retry_safe = true;
    spec.max_attempts = 3;
    let queued = c
        .enqueue(0, actor(), proposed("per-attempt"), worker(), spec)
        .unwrap();
    let run1 = activate_goal(&mut c, queued.goal_id);
    satisfy_checks(&mut c, queued.goal_id);
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: queued.goal_id,
            run_id: run1.id,
            generation: run1.generation,
            actor: actor(),
            outcome: CompletionOutcome::Failure {
                reason: "retry".into(),
                retry: true,
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    let run2 = activate_goal(&mut c, queued.goal_id);
    assert_eq!(run2.attempt, 2);

    // run1 is already terminal and superseded by attempt 2. Completing it
    // again must not be mistaken for an idempotent current-terminal replay.
    let err = c
        .complete(
            c.revision,
            CompleteSpec {
                goal_id: queued.goal_id,
                run_id: run1.id,
                generation: run1.generation,
                actor: actor(),
                outcome: CompletionOutcome::Success {
                    summary: "late".into(),
                },
                client_identity: None,
                client_request_id: None,
                outcome_label: None,
            },
        )
        .unwrap_err();
    assert!(matches!(
        err,
        CoordinatorError::StaleGeneration { .. } | CoordinatorError::StaleRun
    ));

    // The current attempt may succeed, and replaying that exact completion is Ok.
    c.complete(
        c.revision,
        CompleteSpec {
            goal_id: queued.goal_id,
            run_id: run2.id,
            generation: run2.generation,
            actor: actor(),
            outcome: CompletionOutcome::Success {
                summary: "ok".into(),
            },
            client_identity: None,
            client_request_id: None,
            outcome_label: None,
        },
    )
    .unwrap();
    assert_eq!(
        c.goal(queued.goal_id).unwrap().status,
        GoalStatus::Succeeded
    );
    let terminal_run = c.run(run2.id).unwrap().clone();
    let replay = c
        .complete(
            c.revision,
            CompleteSpec {
                goal_id: queued.goal_id,
                run_id: run2.id,
                generation: run2.generation,
                actor: actor(),
                outcome: CompletionOutcome::Success {
                    summary: "ok".into(),
                },
                client_identity: None,
                client_request_id: None,
                outcome_label: None,
            },
        )
        .unwrap();
    assert_eq!(replay, terminal_run);
}

fn xorshift64(state: &mut u64) -> u64 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    x
}

fn pick(state: &mut u64, n: usize) -> usize {
    (xorshift64(state) % n.max(1) as u64) as usize
}

#[test]
fn property_one_active_slot_and_validate_preserved_under_random_mutations() {
    let mut c = GoalCoordinator::new();
    let targets = [
        worker(),
        parent_agent(),
        AgentRef::new("session-b", "other"),
    ];
    let mut rng = 0xA11CE_5EED_u64;
    let mut epoch = 1_u64;

    let assert_invariant = |c: &GoalCoordinator| {
        c.validate().expect("coordinator invariant violated");
        for target in &targets {
            let slots = c
                .slots
                .values()
                .filter(|slot| slot.target == *target)
                .count();
            assert!(slots <= 1, "agent {target} holds {slots} slots");
        }
    };

    for step in 0..2500 {
        assert_invariant(&c);
        let target = targets[pick(&mut rng, targets.len())].clone();
        match pick(&mut rng, 10) {
            0 => {
                if c.goals.len() < 32 {
                    let mut spec = EnqueueSpec::default();
                    spec.retry_safe = pick(&mut rng, 2) == 0;
                    spec.max_attempts = (pick(&mut rng, 4) + 1) as u32;
                    spec.priority = (pick(&mut rng, 8) as i32) - 4;
                    if let Ok(result) = c.enqueue(
                        c.revision,
                        actor(),
                        proposed(&format!("g{step}")),
                        target,
                        spec,
                    ) {
                        let _ = result;
                    }
                }
            }
            1 => {
                let _ = c.activate_next(c.revision, actor(), target);
            }
            2 => {
                let Some((goal_id, run_id, generation)) = slot_parts(&c, &target) else {
                    continue;
                };
                let cost = pick(&mut rng, 4) as u64;
                let _ = c.record_step(c.revision, goal_id, run_id, generation, Some(cost));
            }
            3 => {
                let Some((goal_id, run_id, generation)) = slot_parts(&c, &target) else {
                    continue;
                };
                let reason = if pick(&mut rng, 3) == 0 {
                    WaitReason::Verification
                } else {
                    WaitReason::Timer
                };
                let _ = c.yield_run(
                    c.revision,
                    YieldSpec {
                        goal_id,
                        run_id,
                        generation,
                        actor: actor(),
                        reason,
                    },
                );
            }
            4 => {
                let Some((goal_id, run_id, generation)) = slot_parts(&c, &target) else {
                    continue;
                };
                satisfy_checks(&mut c, goal_id);
                let outcome = if pick(&mut rng, 3) == 0 {
                    CompletionOutcome::Failure {
                        reason: "random failure".into(),
                        retry: pick(&mut rng, 2) == 0,
                    }
                } else {
                    CompletionOutcome::Success {
                        summary: "random success".into(),
                    }
                };
                let _ = c.complete(
                    c.revision,
                    CompleteSpec {
                        goal_id,
                        run_id,
                        generation,
                        actor: actor(),
                        outcome,
                        client_identity: None,
                        client_request_id: None,
                        outcome_label: None,
                    },
                );
            }
            5 => {
                if c.goals.len() > 4 {
                    let ids: Vec<_> = c.goals.keys().copied().collect();
                    let goal_id = ids[pick(&mut rng, ids.len())];
                    let _ = c.cancel(
                        c.revision,
                        CancelSpec {
                            goal_id,
                            actor: actor(),
                            reason: "random cancel".into(),
                            cascade: ChildCascadePolicy::Cancel,
                            client_identity: None,
                            client_request_id: None,
                        },
                    );
                }
            }
            6 => {
                let mut settle = None;
                for target in &targets {
                    if let Some((goal_id, run_id, generation)) = slot_parts(&c, target) {
                        if c.goal(goal_id)
                            .is_ok_and(|goal| goal.status == GoalStatus::Cancelling)
                        {
                            settle = Some((goal_id, run_id, generation));
                            break;
                        }
                    }
                }
                if let Some((goal_id, run_id, generation)) = settle {
                    let _ = c.settle_cancel(c.revision, actor(), goal_id, run_id, generation);
                }
            }
            7 => {
                let Some((_goal_id, run_id, generation)) = slot_parts(&c, &target) else {
                    continue;
                };
                let expires = if pick(&mut rng, 2) == 0 {
                    Utc::now() - Duration::seconds(1)
                } else {
                    Utc::now() + Duration::seconds(300)
                };
                let _ = c.heartbeat(c.revision, run_id, generation, Some(expires));
            }
            8 => {
                let _ = c.reconcile(c.revision, epoch);
                epoch = c.daemon_epoch.saturating_add(1);
            }
            _ => {
                let _ = c.fence_expired(c.revision, Utc::now());
            }
        }
        assert_invariant(&c);
    }
}

fn slot_parts(
    c: &GoalCoordinator,
    target: &AgentRef,
) -> Option<(crate::goal::GoalId, GoalRunId, u64)> {
    c.slot(target)
        .map(|slot| (slot.goal_id, slot.run_id, slot.generation))
}
