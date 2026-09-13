//! Pure, serializable goal-coordinator aggregate.
//!
//! Callers clone a candidate, apply one revisioned mutation, validate, and
//! persist only after the candidate is accepted. A rejected mutation leaves
//! the original aggregate byte-for-byte unchanged.

use super::error::CoordinatorError;
use super::ids::*;
use super::model::*;
use crate::goal::{Goal, GoalActor, GoalId, GoalStatus, GoalTransition};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use std::collections::{BTreeMap, BTreeSet};
use uuid::Uuid;

/// Durable coordinator snapshot. This is the transactional aggregate the
/// daemon persists; it is not a SQLite schema.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct GoalCoordinator {
    #[serde(default)]
    pub revision: u64,
    #[serde(default)]
    pub daemon_epoch: u64,
    #[serde(default)]
    pub enqueue_seq: u64,
    #[serde(default)]
    pub goals: BTreeMap<GoalId, Goal>,
    #[serde(default)]
    pub assignments: BTreeMap<GoalAssignmentId, GoalAssignment>,
    #[serde(default)]
    pub queue: BTreeMap<GoalQueueEntryId, GoalQueueEntry>,
    #[serde(default)]
    pub slots: BTreeMap<String, AgentGoalSlot>,
    #[serde(default)]
    pub generations: BTreeMap<String, u64>,
    #[serde(default)]
    pub runs: BTreeMap<GoalRunId, GoalRun>,
    #[serde(default)]
    pub dependencies: BTreeMap<GoalDependencyId, GoalDependency>,
    #[serde(default)]
    pub outbox: BTreeMap<OutboxId, OutboxEntry>,
    #[serde(default)]
    pub messages: BTreeMap<Uuid, GoalMessage>,
    #[serde(default)]
    pub idempotency: Vec<IdempotencyRecord>,
    #[serde(default)]
    pub claims: BTreeMap<String, ExternalClaim>,
}

impl GoalCoordinator {
    pub fn new() -> Self {
        Self::default()
    }

    fn submit_candidate_inner(&mut self, spec: CandidateSpec) -> Result<GoalRun, CoordinatorError> {
        let goal_revision = self.goal(spec.goal_id)?.revision;
        if goal_revision != spec.expected_goal_revision {
            return Err(CoordinatorError::Invalid(format!(
                "stale goal revision: expected {}, actual {}",
                spec.expected_goal_revision, goal_revision
            )));
        }
        let run = self
            .runs
            .get(&spec.run_id)
            .ok_or(CoordinatorError::RunNotFound(spec.run_id))?
            .clone();
        if run.goal_id != spec.goal_id
            || run.generation != spec.generation
            || !run.state.holds_slot()
        {
            return Err(CoordinatorError::StaleRun);
        }
        if !matches!(&spec.actor, GoalActor::Agent { agent_id } if *agent_id == run.target.agent_id)
        {
            return Err(CoordinatorError::Invalid(
                "candidate actor does not own run".into(),
            ));
        }
        // A worker cannot self-assert independent/reviewer verification.
        if matches!(
            spec.verification,
            crate::goal::CheckVerification::Reviewed
                | crate::goal::CheckVerification::IndependentlyVerified
        ) {
            return Err(CoordinatorError::Invalid(
                "worker cannot claim reviewer verification".into(),
            ));
        }
        for reference in spec.evidence {
            let revision = self.goal(spec.goal_id)?.revision;
            let new_revision = {
                let goal = self
                    .goals
                    .get_mut(&spec.goal_id)
                    .ok_or(CoordinatorError::GoalNotFound(spec.goal_id))?;
                goal.apply(
                    revision,
                    spec.actor.clone(),
                    GoalTransition::LinkEvidence { reference },
                )
                .map_err(CoordinatorError::from_goal)?;
                goal.revision
            };
            self.refresh_queue_revision(spec.goal_id, new_revision);
        }
        for reference in spec.artifact_ids.into_iter().chain(spec.work_result_ids) {
            let revision = self.goal(spec.goal_id)?.revision;
            let new_revision = {
                let goal = self
                    .goals
                    .get_mut(&spec.goal_id)
                    .ok_or(CoordinatorError::GoalNotFound(spec.goal_id))?;
                goal.apply(
                    revision,
                    spec.actor.clone(),
                    GoalTransition::LinkArtifact { reference },
                )
                .map_err(CoordinatorError::from_goal)?;
                goal.revision
            };
            self.refresh_queue_revision(spec.goal_id, new_revision);
        }
        if let Some(stored) = self.runs.get_mut(&spec.run_id) {
            stored.result = Some(spec.result);
            stored.verification = spec.verification;
        }
        self.yield_inner(YieldSpec {
            goal_id: spec.goal_id,
            run_id: spec.run_id,
            generation: spec.generation,
            actor: spec.actor,
            reason: WaitReason::Verification,
        })
    }

    /// Persist a worker candidate and release its execution slot.  This is an
    /// idempotent command: retries with the same client key and payload replay
    /// the original run, while a changed payload is rejected.
    pub fn submit_candidate(
        &mut self,
        expected: u64,
        spec: CandidateSpec,
    ) -> Result<GoalRun, CoordinatorError> {
        let fingerprint = fingerprint_of(&("submit_candidate", &spec))?;
        let identity = spec.client_identity.clone();
        let request_id = spec.client_request_id;
        self.mutate_idempotent(
            expected,
            identity.as_deref(),
            request_id,
            &fingerprint,
            |c| c.submit_candidate_inner(spec),
        )
    }

    pub fn add_dependency_with_kind(
        &mut self,
        expected: u64,
        parent: GoalId,
        child: GoalId,
        kind: DependencyKind,
    ) -> Result<GoalDependency, CoordinatorError> {
        if let DependencyKind::GoalCondition { condition } = &kind
            && condition
                .strip_prefix("outcome:")
                .is_some_and(str::is_empty)
        {
            return Err(CoordinatorError::Invalid(
                "outcome dependency must name a non-empty outcome".into(),
            ));
        }
        self.mutate(expected, |c| {
            c.goal(parent)?;
            c.goal(child)?;
            c.add_edge(parent, child, None, kind)
        })
    }

    /// Undo an activation when the runtime cannot hand the run to its agent.
    /// The run is retained for fencing/audit, but the goal is returned to the
    /// queue and the execution slot is released.
    pub fn rollback_activation(
        &mut self,
        expected: u64,
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
        reason: String,
    ) -> Result<(), CoordinatorError> {
        self.mutate(expected, |c| {
            let run = c.fence_run(goal_id, run_id, generation)?.clone();
            if run.state != GoalRunState::Open {
                return Err(CoordinatorError::Invalid(
                    "only an open run can be rolled back".into(),
                ));
            }
            c.release_slot(&run.target, run.id, run.generation)?;
            c.supersede_dispatch(run.id);
            let current = c.runs.get_mut(&run.id).unwrap();
            current.state = GoalRunState::Waiting;
            current.wait_reason = Some(WaitReason::Other {
                reason: reason.clone(),
            });
            current.last_heartbeat = Utc::now();
            c.requeue_goal(goal_id, GoalActor::System, reason)
        })
    }

    /// Append a goal message using the same revision and persistence fence as
    /// every other coordinator mutation.  Message ids are idempotent: a
    /// retry returns the original durable record without creating a second
    /// sequence number.
    pub fn append_message(
        &mut self,
        expected: u64,
        message: GoalMessage,
    ) -> Result<GoalMessage, CoordinatorError> {
        self.mutate(expected, |c| {
            c.goal(message.goal_id)?;
            if let Some(existing) = c.messages.get(&message.id) {
                return Ok(existing.clone());
            }
            let mut message = message;
            if message.sequence == 0 {
                message.sequence = c.messages.values().map(|m| m.sequence).max().unwrap_or(0) + 1;
            }
            c.messages.insert(message.id, message.clone());
            Ok(message)
        })
    }

    pub fn to_snapshot(&self) -> Result<Vec<u8>, CoordinatorError> {
        serde_json::to_vec(self).map_err(|e| CoordinatorError::Snapshot(e.to_string()))
    }

    pub fn from_snapshot(bytes: &[u8]) -> Result<Self, CoordinatorError> {
        let state: Self =
            serde_json::from_slice(bytes).map_err(|e| CoordinatorError::Snapshot(e.to_string()))?;
        state.validate()?;
        Ok(state)
    }

    pub fn goal(&self, id: GoalId) -> Result<&Goal, CoordinatorError> {
        self.goals
            .get(&id)
            .ok_or(CoordinatorError::GoalNotFound(id))
    }

    pub fn run(&self, id: GoalRunId) -> Result<&GoalRun, CoordinatorError> {
        self.runs.get(&id).ok_or(CoordinatorError::RunNotFound(id))
    }

    pub fn assignment(&self, id: GoalAssignmentId) -> Result<&GoalAssignment, CoordinatorError> {
        self.assignments
            .get(&id)
            .ok_or(CoordinatorError::AssignmentNotFound(id))
    }

    pub fn slot(&self, target: &AgentRef) -> Option<&AgentGoalSlot> {
        self.slots.get(&target.key())
    }

    pub fn open_run(&self, target: &AgentRef) -> Option<&GoalRun> {
        let slot = self.slot(target)?;
        self.runs.get(&slot.run_id)
    }

    pub fn queue_for(&self, target: &AgentRef) -> Vec<&GoalQueueEntry> {
        let mut entries: Vec<_> = self
            .queue
            .values()
            .filter(|e| e.target == *target)
            .collect();
        entries.sort_by(|a, b| queue_ord(a, b, Utc::now()));
        entries
    }

    pub fn pending_outbox(&self) -> Vec<&OutboxEntry> {
        self.outbox
            .values()
            .filter(|e| e.state == OutboxState::Pending)
            .collect()
    }

    pub fn is_idle(&self, target: &AgentRef) -> bool {
        matches!(self.occupancy(target), AgentOccupancy::Idle)
            && self.verification_hold(target).is_none()
    }

    pub fn occupancy(&self, target: &AgentRef) -> AgentOccupancy {
        if let Some(slot) = self.slot(target) {
            return AgentOccupancy::Goal {
                goal_id: slot.goal_id,
                run_id: slot.run_id,
                generation: slot.generation,
            };
        }
        if let Some(claim) = self.claims.get(&target.key()) {
            return AgentOccupancy::Claimed {
                holder: claim.holder.clone(),
                kind: claim.kind,
            };
        }
        AgentOccupancy::Idle
    }

    /// Shared occupancy predicate. WorkGraph assignment and goal activation
    /// must both consult this before claiming an agent.
    pub fn admit(&self, target: &AgentRef) -> Result<(), CoordinatorError> {
        match self.occupancy(target) {
            AgentOccupancy::Idle => Ok(()),
            AgentOccupancy::Goal {
                goal_id, run_id, ..
            } => Err(CoordinatorError::SlotOccupied {
                target: target.clone(),
                goal_id,
                run_id,
            }),
            AgentOccupancy::Claimed { holder, .. } => Err(CoordinatorError::ClaimOccupied {
                target: target.clone(),
                holder,
            }),
        }
    }

    pub fn claim_occupancy(
        &mut self,
        expected: u64,
        target: AgentRef,
        holder: impl Into<String>,
        kind: OccupancyKind,
    ) -> Result<(), CoordinatorError> {
        let holder = holder.into();
        self.mutate(expected, |c| {
            c.admit(&target)?;
            c.claims.insert(
                target.key(),
                ExternalClaim {
                    target: target.clone(),
                    holder,
                    kind,
                    acquired_at: Utc::now(),
                },
            );
            Ok(())
        })
    }

    pub fn release_claim(
        &mut self,
        expected: u64,
        target: &AgentRef,
        holder: &str,
    ) -> Result<(), CoordinatorError> {
        self.mutate(expected, |c| {
            match c.claims.get(&target.key()) {
                Some(claim) if claim.holder == holder => {}
                Some(claim) => {
                    return Err(CoordinatorError::ClaimOccupied {
                        target: target.clone(),
                        holder: claim.holder.clone(),
                    });
                }
                None => {
                    return Err(CoordinatorError::Invalid(
                        "no occupancy claim to release".into(),
                    ));
                }
            }
            c.claims.remove(&target.key());
            Ok(())
        })
    }

    /// Persist a Proposed goal without queueing or occupying a slot.
    /// Targeted execution must go through [`Self::enqueue`].
    pub fn register(&mut self, expected: u64, goal: Goal) -> Result<Goal, CoordinatorError> {
        self.mutate(expected, |c| c.register_inner(goal))
    }

    pub fn enqueue(
        &mut self,
        expected: u64,
        actor: GoalActor,
        goal: Goal,
        target: AgentRef,
        spec: EnqueueSpec,
    ) -> Result<EnqueueResult, CoordinatorError> {
        let fingerprint = fingerprint_of(&("enqueue", &goal, &target, &spec))?;
        let client_identity = spec.client_identity.clone();
        let client_request_id = spec.client_request_id;
        self.mutate_idempotent(
            expected,
            client_identity.as_deref(),
            client_request_id,
            &fingerprint,
            |c| c.enqueue_inner(actor, goal, target, spec),
        )
    }

    pub fn activate(
        &mut self,
        expected: u64,
        spec: ActivateSpec,
    ) -> Result<GoalRun, CoordinatorError> {
        let fingerprint = fingerprint_of(&("activate", &spec))?;
        let client_identity = spec.client_identity.clone();
        let client_request_id = spec.client_request_id;
        self.mutate_idempotent(
            expected,
            client_identity.as_deref(),
            client_request_id,
            &fingerprint,
            |c| c.activate_inner(spec, Utc::now()),
        )
    }

    /// Promote the next eligible queued goal for `target`. Returns `Ok(None)`
    /// without a revision bump when the agent is busy, gated, or idle.
    pub fn activate_next(
        &mut self,
        expected: u64,
        actor: GoalActor,
        target: AgentRef,
    ) -> Result<Option<GoalRun>, CoordinatorError> {
        self.activate_next_at(expected, actor, target, Utc::now())
    }

    pub fn activate_next_at(
        &mut self,
        expected: u64,
        actor: GoalActor,
        target: AgentRef,
        now: DateTime<Utc>,
    ) -> Result<Option<GoalRun>, CoordinatorError> {
        self.check_revision(expected)?;
        if self.admit(&target).is_err() || self.verification_hold(&target).is_some() {
            return Ok(None);
        }
        let Some(entry_id) = self.select_next(&target, now) else {
            return Ok(None);
        };
        let goal_id = self.queue[&entry_id].goal_id;
        self.mutate(expected, |c| {
            c.activate_inner(
                ActivateSpec {
                    goal_id,
                    expected_goal_revision: None,
                    actor: actor.clone(),
                    client_identity: None,
                    client_request_id: None,
                },
                now,
            )
            .map(Some)
        })
    }

    pub fn yield_run(
        &mut self,
        expected: u64,
        spec: YieldSpec,
    ) -> Result<GoalRun, CoordinatorError> {
        self.mutate(expected, |c| c.yield_inner(spec))
    }

    pub fn complete(
        &mut self,
        expected: u64,
        spec: CompleteSpec,
    ) -> Result<GoalRun, CoordinatorError> {
        let fingerprint = fingerprint_of(&("complete", &spec))?;
        let client_identity = spec.client_identity.clone();
        let client_request_id = spec.client_request_id;
        self.mutate_idempotent(
            expected,
            client_identity.as_deref(),
            client_request_id,
            &fingerprint,
            |c| c.complete_inner(spec),
        )
    }

    pub fn cancel(&mut self, expected: u64, spec: CancelSpec) -> Result<Goal, CoordinatorError> {
        let fingerprint = fingerprint_of(&("cancel", &spec))?;
        let client_identity = spec.client_identity.clone();
        let client_request_id = spec.client_request_id;
        self.mutate_idempotent(
            expected,
            client_identity.as_deref(),
            client_request_id,
            &fingerprint,
            |c| c.cancel_inner(spec),
        )
    }

    pub fn settle_cancel(
        &mut self,
        expected: u64,
        actor: GoalActor,
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
    ) -> Result<Goal, CoordinatorError> {
        self.mutate(expected, |c| {
            c.settle_cancel_inner(actor, goal_id, run_id, generation)
        })
    }

    pub fn record_step(
        &mut self,
        expected: u64,
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
        cost: Option<u64>,
    ) -> Result<GoalRun, CoordinatorError> {
        self.mutate(expected, |c| {
            c.record_step_inner(goal_id, run_id, generation, cost)
        })
    }

    pub fn heartbeat(
        &mut self,
        expected: u64,
        run_id: GoalRunId,
        generation: u64,
        expires_at: Option<DateTime<Utc>>,
    ) -> Result<GoalRun, CoordinatorError> {
        self.mutate(expected, |c| {
            c.heartbeat_inner(run_id, generation, expires_at)
        })
    }

    /// Interrupt Open runs whose claim has expired. Hung live-daemon work is
    /// fenced the same way as a restart: the run is Interrupted and retry-safe
    /// goals are requeued.
    pub fn fence_expired(
        &mut self,
        expected: u64,
        now: DateTime<Utc>,
    ) -> Result<ReconcileReport, CoordinatorError> {
        self.mutate(expected, |c| c.fence_expired_inner(now))
    }

    pub fn reconcile(
        &mut self,
        expected: u64,
        new_epoch: u64,
    ) -> Result<ReconcileReport, CoordinatorError> {
        self.mutate(expected, |c| c.reconcile_inner(new_epoch))
    }

    pub fn acknowledge_outbox(
        &mut self,
        expected: u64,
        id: OutboxId,
    ) -> Result<OutboxEntry, CoordinatorError> {
        self.mutate(expected, |c| {
            let entry = c
                .outbox
                .get_mut(&id)
                .ok_or(CoordinatorError::OutboxNotFound(id))?;
            match entry.state {
                OutboxState::Pending => entry.state = OutboxState::Acknowledged,
                OutboxState::Acknowledged => {}
                OutboxState::Superseded => return Err(CoordinatorError::LateAck),
            }
            Ok(entry.clone())
        })
    }

    pub fn add_dependency(
        &mut self,
        expected: u64,
        parent: GoalId,
        child: GoalId,
    ) -> Result<GoalDependency, CoordinatorError> {
        self.mutate(expected, |c| {
            c.goal(parent)?;
            c.goal(child)?;
            c.add_edge(parent, child, None, DependencyKind::ChildCompletion)
        })
    }

    /// Apply a non-scheduling goal mutation (checks, approval, evidence).
    /// Slot-affecting lifecycle transitions must go through coordinator APIs.
    pub fn apply_goal_transition(
        &mut self,
        expected: u64,
        goal_id: GoalId,
        expected_goal_revision: u64,
        actor: GoalActor,
        transition: GoalTransition,
    ) -> Result<Goal, CoordinatorError> {
        if !is_local_goal_transition(&transition) {
            return Err(CoordinatorError::Invalid(
                "lifecycle transitions must use coordinator scheduling APIs".into(),
            ));
        }
        self.mutate(expected, |c| {
            let goal = c
                .goals
                .get_mut(&goal_id)
                .ok_or(CoordinatorError::GoalNotFound(goal_id))?;
            goal.apply(expected_goal_revision, actor, transition)
                .map_err(CoordinatorError::from_goal)?;
            let revision = goal.revision;
            c.refresh_queue_revision(goal_id, revision);
            Ok(c.goals[&goal_id].clone())
        })
    }

    pub fn validate(&self) -> Result<(), CoordinatorError> {
        validate_coordinator(self)
    }
}

impl GoalCoordinator {
    fn check_revision(&self, expected: u64) -> Result<(), CoordinatorError> {
        if self.revision != expected {
            Err(CoordinatorError::StaleRevision {
                expected,
                actual: self.revision,
            })
        } else {
            Ok(())
        }
    }

    fn mutate<F, R>(&mut self, expected: u64, op: F) -> Result<R, CoordinatorError>
    where
        F: FnOnce(&mut Self) -> Result<R, CoordinatorError>,
    {
        self.check_revision(expected)?;
        let mut candidate = self.clone();
        let result = op(&mut candidate)?;
        candidate.revision = expected.saturating_add(1);
        candidate.validate()?;
        *self = candidate;
        Ok(result)
    }

    fn mutate_idempotent<F, R>(
        &mut self,
        expected: u64,
        identity: Option<&str>,
        request_id: Option<Uuid>,
        fingerprint: &str,
        op: F,
    ) -> Result<R, CoordinatorError>
    where
        F: FnOnce(&mut Self) -> Result<R, CoordinatorError>,
        R: Serialize + DeserializeOwned,
    {
        if let (Some(identity), Some(request_id)) = (identity, request_id)
            && let Some(replay) = self.find_idempotency(identity, request_id, fingerprint)?
        {
            return replay_outcome(replay);
        }
        self.check_revision(expected)?;
        let mut candidate = self.clone();
        let result = op(&mut candidate)?;
        if let (Some(identity), Some(request_id)) = (identity, request_id) {
            let encoded = serde_json::to_value(&result)
                .map_err(|e| CoordinatorError::Snapshot(e.to_string()))?;
            candidate.idempotency.push(IdempotencyRecord {
                client_identity: identity.to_string(),
                request_id,
                fingerprint: fingerprint.to_string(),
                coordinator_revision: expected.saturating_add(1),
                outcome: IdempotencyOutcome::Success { encoded },
            });
        }
        candidate.revision = expected.saturating_add(1);
        candidate.validate()?;
        *self = candidate;
        Ok(result)
    }

    fn find_idempotency(
        &self,
        identity: &str,
        request_id: Uuid,
        fingerprint: &str,
    ) -> Result<Option<&IdempotencyOutcome>, CoordinatorError> {
        match self
            .idempotency
            .iter()
            .find(|r| r.client_identity == identity && r.request_id == request_id)
        {
            Some(record) if record.fingerprint == fingerprint => Ok(Some(&record.outcome)),
            Some(_) => Err(CoordinatorError::IdempotencyConflict),
            None => Ok(None),
        }
    }

    fn register_inner(&mut self, goal: Goal) -> Result<Goal, CoordinatorError> {
        if self.goals.contains_key(&goal.id) {
            return Err(CoordinatorError::Invalid(
                "goal is already registered with the coordinator".into(),
            ));
        }
        if goal.status != GoalStatus::Proposed {
            return Err(CoordinatorError::Invalid(
                "only Proposed goals can be registered without a target".into(),
            ));
        }
        goal.validate().map_err(CoordinatorError::from_goal)?;
        let stored = goal.clone();
        self.goals.insert(goal.id, goal);
        Ok(stored)
    }

    fn enqueue_inner(
        &mut self,
        actor: GoalActor,
        mut goal: Goal,
        target: AgentRef,
        spec: EnqueueSpec,
    ) -> Result<EnqueueResult, CoordinatorError> {
        if target.session_id.trim().is_empty() || target.agent_id.trim().is_empty() {
            return Err(CoordinatorError::Invalid(
                "target session_id and agent_id must be non-empty".into(),
            ));
        }
        if let Some(existing) = self.goals.get(&goal.id) {
            if existing.status != GoalStatus::Proposed {
                return Err(CoordinatorError::Invalid(
                    "only Proposed goals can be enqueued".into(),
                ));
            }
            if self.live_assignment(goal.id).is_ok() {
                return Err(CoordinatorError::Invalid("goal is already assigned".into()));
            }
            goal = existing.clone();
        } else if goal.status != GoalStatus::Proposed {
            return Err(CoordinatorError::Invalid(
                "only Proposed goals can be enqueued".into(),
            ));
        }
        goal.validate().map_err(CoordinatorError::from_goal)?;
        if let Some(deadline) = goal.deadline
            && deadline < Utc::now()
        {
            return Err(CoordinatorError::DeadlineExceeded);
        }

        let parent_meta = self.resolve_parent(&spec, &target)?;
        if spec.parent_yields {
            let (parent_id, parent_run_id, parent_generation) = match (
                spec.parent_goal_id,
                spec.parent_run_id,
                spec.parent_generation,
            ) {
                (Some(id), Some(run), Some(generation)) => (id, run, generation),
                _ => return Err(CoordinatorError::ParentYieldFence),
            };
            self.yield_inner(YieldSpec {
                goal_id: parent_id,
                run_id: parent_run_id,
                generation: parent_generation,
                actor: actor.clone(),
                reason: WaitReason::Child {
                    child_goal_id: goal.id,
                },
            })?;
        }

        let (root_goal_id, depth) = match parent_meta {
            Some((root, depth)) => (root, depth),
            None => (goal.id, 0),
        };
        if depth > MAX_GOAL_DEPTH {
            return Err(CoordinatorError::DepthLimit);
        }
        let live_descendants = self
            .assignments
            .values()
            .filter(|a| a.live() && a.root_goal_id == root_goal_id)
            .count();
        if live_descendants >= MAX_LIVE_DESCENDANTS {
            return Err(CoordinatorError::DescendantLimit);
        }
        if let Some(key) = spec.dedupe_key.as_ref()
            && self.assignments.values().any(|a| {
                a.live()
                    && a.dedupe_key.as_deref() == Some(key.as_str())
                    && a.root_goal_id == root_goal_id
            })
        {
            return Err(CoordinatorError::DedupeConflict);
        }

        for dep in &spec.wait_for {
            self.goal(*dep)?;
        }

        goal.links.session_id = Some(target.session_id.clone());
        goal.links.agent_id = Some(target.agent_id.clone());
        goal.transition(actor.clone(), GoalTransition::Queue)
            .map_err(CoordinatorError::from_goal)?;

        self.enqueue_seq = self.enqueue_seq.saturating_add(1);
        let assignment_id = GoalAssignmentId::new();
        let queue_id = GoalQueueEntryId::new();
        let assignment = GoalAssignment {
            id: assignment_id,
            goal_id: goal.id,
            target: target.clone(),
            controller: actor,
            queue_order: self.enqueue_seq,
            priority_class: spec.priority_class,
            priority: spec.priority,
            parent_goal_id: spec.parent_goal_id,
            parent_run_id: spec.parent_run_id,
            parent_generation: spec.parent_generation,
            workflow_node_id: spec.workflow_node_id,
            retry_safe: spec.retry_safe,
            max_attempts: spec.max_attempts,
            created_at: Utc::now(),
            released_at: None,
            root_goal_id,
            depth,
            dedupe_key: spec.dedupe_key,
            wait_for: spec.wait_for.clone(),
        };
        let entry = GoalQueueEntry {
            id: queue_id,
            assignment_id,
            goal_id: goal.id,
            target,
            enqueue_seq: self.enqueue_seq,
            priority_class: spec.priority_class,
            priority: spec.priority,
            eligible_at: spec.eligible_at.unwrap_or_else(Utc::now),
            deadline: goal.deadline,
            expected_goal_revision: goal.revision,
        };
        let result = EnqueueResult {
            assignment_id,
            queue_id,
            goal_id: goal.id,
        };
        self.goals.insert(goal.id, goal);
        self.assignments.insert(assignment_id, assignment);
        self.queue.insert(queue_id, entry);
        for dep in &spec.wait_for {
            // This new goal waits on each predecessor before it can activate.
            self.add_edge(result.goal_id, *dep, None, DependencyKind::ChildCompletion)?;
        }
        if spec.parent_yields
            && let Some(parent_id) = spec.parent_goal_id
        {
            self.add_edge(
                parent_id,
                result.goal_id,
                spec.parent_run_id,
                DependencyKind::ChildCompletion,
            )?;
        }
        Ok(result)
    }

    fn resolve_parent(
        &self,
        spec: &EnqueueSpec,
        child_target: &AgentRef,
    ) -> Result<Option<(GoalId, u32)>, CoordinatorError> {
        let Some(parent_id) = spec.parent_goal_id else {
            return Ok(None);
        };
        let parent = self.goal(parent_id)?;
        let parent_assignment = self.live_assignment(parent_id)?;
        if parent_assignment.target == *child_target && !spec.parent_yields {
            return Err(CoordinatorError::ParentMustYield);
        }
        if spec.parent_yields {
            let run = self
                .runs
                .get(
                    &spec
                        .parent_run_id
                        .ok_or(CoordinatorError::ParentYieldFence)?,
                )
                .ok_or(CoordinatorError::ParentYieldFence)?;
            if run.goal_id != parent_id
                || run.generation != spec.parent_generation.unwrap_or(u64::MAX)
                || !run.state.holds_slot()
            {
                return Err(CoordinatorError::ParentYieldFence);
            }
        }
        let _ = parent;
        Ok(Some((
            parent_assignment.root_goal_id,
            parent_assignment.depth + 1,
        )))
    }

    fn activate_inner(
        &mut self,
        spec: ActivateSpec,
        now: DateTime<Utc>,
    ) -> Result<GoalRun, CoordinatorError> {
        let goal_id = spec.goal_id;
        let goal = self.goal(goal_id)?.clone();
        if let Some(expected) = spec.expected_goal_revision
            && expected != goal.revision
        {
            return Err(CoordinatorError::StaleGoalRevision {
                expected,
                actual: goal.revision,
            });
        }
        if goal.deadline.is_some_and(|d| d < now) {
            return Err(CoordinatorError::DeadlineExceeded);
        }
        let assignment = self.live_assignment(goal_id)?.clone();
        let target = assignment.target.clone();
        self.admit(&target)?;
        if self.verification_hold(&target).is_some() {
            return Err(CoordinatorError::VerificationHold);
        }
        if !self.dependencies_ready(goal_id) {
            return Err(CoordinatorError::DependenciesNotReady);
        }
        if !matches!(
            goal.status,
            GoalStatus::Queued | GoalStatus::Waiting | GoalStatus::Blocked
        ) {
            return Err(CoordinatorError::Invalid(format!(
                "goal {} cannot be activated from {:?}",
                goal_id, goal.status
            )));
        }

        let entry_id = self
            .queue
            .values()
            .find(|e| e.goal_id == goal_id)
            .map(|e| e.id)
            .ok_or(CoordinatorError::QueueEntryNotFound(goal_id))?;
        self.queue.remove(&entry_id);

        let attempt = self
            .runs
            .values()
            .filter(|r| r.goal_id == goal_id)
            .map(|r| r.attempt)
            .max()
            .unwrap_or(0)
            .saturating_add(1);
        if assignment.max_attempts != 0 && attempt > assignment.max_attempts {
            return Err(CoordinatorError::Invalid("attempt budget exhausted".into()));
        }
        self.check_budget(&goal, 0, 0)?;

        let key = target.key();
        let generation = self
            .generations
            .get(&key)
            .copied()
            .unwrap_or(0)
            .saturating_add(1);
        self.generations.insert(key.clone(), generation);
        let run_id = GoalRunId::new();
        let run = GoalRun {
            id: run_id,
            goal_id,
            assignment_id: assignment.id,
            target: target.clone(),
            attempt,
            generation,
            daemon_epoch: self.daemon_epoch,
            state: GoalRunState::Open,
            started_at: now,
            finished_at: None,
            last_heartbeat: now,
            steps_consumed: 0,
            cost_consumed: 0,
            retry_safe: assignment.retry_safe,
            wait_reason: None,
            result_summary: None,
            result: None,
            verification: crate::goal::CheckVerification::Unverified,
            outcome: None,
        };
        let slot = AgentGoalSlot {
            target: target.clone(),
            goal_id,
            run_id,
            assignment_id: assignment.id,
            generation,
            acquired_at: now,
            last_heartbeat: now,
            expires_at: Some(now + chrono::Duration::seconds(DEFAULT_SLOT_TTL_SECS)),
            daemon_epoch: self.daemon_epoch,
        };
        self.slots.insert(key, slot);
        self.runs.insert(run_id, run.clone());

        let goal = self
            .goals
            .get_mut(&goal_id)
            .ok_or(CoordinatorError::GoalNotFound(goal_id))?;
        goal.transition(spec.actor, GoalTransition::Activate)
            .map_err(CoordinatorError::from_goal)?;

        self.push_outbox(OutboxKind::Dispatch {
            goal_id,
            run_id,
            generation,
            target,
            assignment_id: assignment.id,
        });
        Ok(run)
    }

    fn yield_inner(&mut self, spec: YieldSpec) -> Result<GoalRun, CoordinatorError> {
        let run = self.fence_run(spec.goal_id, spec.run_id, spec.generation)?;
        if run.state != GoalRunState::Open {
            return Err(CoordinatorError::Invalid(
                "only an open run can yield".into(),
            ));
        }
        let target = run.target.clone();
        self.release_slot(&target, spec.run_id, spec.generation)?;
        self.supersede_dispatch(spec.run_id);
        let run = self
            .runs
            .get_mut(&spec.run_id)
            .ok_or(CoordinatorError::RunNotFound(spec.run_id))?;
        run.state = GoalRunState::Waiting;
        run.wait_reason = Some(spec.reason.clone());
        run.last_heartbeat = Utc::now();
        let snapshot = run.clone();
        let goal = self
            .goals
            .get_mut(&spec.goal_id)
            .ok_or(CoordinatorError::GoalNotFound(spec.goal_id))?;
        goal.transition(
            spec.actor,
            GoalTransition::Wait {
                reason: wait_reason_text(&spec.reason),
            },
        )
        .map_err(CoordinatorError::from_goal)?;
        Ok(snapshot)
    }

    fn complete_inner(&mut self, spec: CompleteSpec) -> Result<GoalRun, CoordinatorError> {
        let run = self
            .runs
            .get(&spec.run_id)
            .ok_or(CoordinatorError::RunNotFound(spec.run_id))?
            .clone();
        if run.goal_id != spec.goal_id {
            return Err(CoordinatorError::Invalid(
                "run does not belong to goal".into(),
            ));
        }
        if run.generation != spec.generation {
            return Err(CoordinatorError::StaleGeneration {
                expected: spec.generation,
                actual: run.generation,
            });
        }
        if self
            .runs
            .values()
            .any(|other| other.goal_id == spec.goal_id && other.attempt > run.attempt)
        {
            return Err(CoordinatorError::StaleRun);
        }
        if run.state.terminal() {
            return Ok(run);
        }
        match run.state {
            GoalRunState::Open => {
                let slot = self
                    .slot(&run.target)
                    .ok_or_else(|| CoordinatorError::Invalid("open run has no slot".into()))?;
                if slot.run_id != run.id || slot.generation != run.generation {
                    return Err(CoordinatorError::StaleRun);
                }
            }
            GoalRunState::Waiting => {
                if self.slot(&run.target).is_some_and(|s| s.run_id == run.id) {
                    return Err(CoordinatorError::Invalid(
                        "waiting run must not hold a slot".into(),
                    ));
                }
            }
            _ => {
                return Err(CoordinatorError::Invalid(format!(
                    "run {:?} cannot complete",
                    run.state
                )));
            }
        }

        let now = Utc::now();
        match spec.outcome {
            CompletionOutcome::Success { summary } => {
                let goal = self.goal(spec.goal_id)?;
                if !goal.checks_satisfied() {
                    return Err(CoordinatorError::ChecksNotSatisfied);
                }
                if run.state == GoalRunState::Open {
                    self.release_slot(&run.target, run.id, run.generation)?;
                    self.supersede_dispatch(run.id);
                }
                {
                    let run = self.runs.get_mut(&spec.run_id).unwrap();
                    run.state = GoalRunState::Succeeded;
                    run.finished_at = Some(now);
                    run.result_summary = Some(summary);
                    run.outcome = Some(
                        spec.outcome_label
                            .clone()
                            .unwrap_or_else(|| "success".into()),
                    );
                    run.wait_reason = None;
                }
                let goal = self.goals.get_mut(&spec.goal_id).unwrap();
                goal.transition(spec.actor.clone(), GoalTransition::Succeed)
                    .map_err(CoordinatorError::from_goal)?;
                self.release_assignment(spec.goal_id, now);
                self.propagate_success(spec.goal_id, spec.run_id, spec.generation, spec.actor)?;
            }
            CompletionOutcome::Failure { reason, retry } => {
                if run.state == GoalRunState::Open {
                    self.release_slot(&run.target, run.id, run.generation)?;
                    self.supersede_dispatch(run.id);
                }
                {
                    let run = self.runs.get_mut(&spec.run_id).unwrap();
                    run.state = GoalRunState::Failed;
                    run.finished_at = Some(now);
                    run.result_summary = Some(reason.clone());
                    // A failed lifecycle can never claim an application
                    // success label (for example `approved`).
                    run.outcome = Some("failure".into());
                    run.wait_reason = None;
                }
                let assignment = self.live_assignment(spec.goal_id)?.clone();
                let attempts = self
                    .runs
                    .values()
                    .filter(|r| r.goal_id == spec.goal_id)
                    .count() as u32;
                let can_retry = retry
                    && assignment.retry_safe
                    && (assignment.max_attempts == 0 || attempts < assignment.max_attempts);
                if can_retry {
                    self.requeue_goal(
                        spec.goal_id,
                        spec.actor,
                        format!("retry after failure: {reason}"),
                    )?;
                } else {
                    let goal = self.goals.get_mut(&spec.goal_id).unwrap();
                    goal.transition(spec.actor.clone(), GoalTransition::Fail { reason })
                        .map_err(CoordinatorError::from_goal)?;
                    self.release_assignment(spec.goal_id, now);
                    self.fail_dependents(spec.goal_id)?;
                }
            }
        }
        Ok(self.runs[&spec.run_id].clone())
    }

    fn cancel_inner(&mut self, spec: CancelSpec) -> Result<Goal, CoordinatorError> {
        let goal_id = spec.goal_id;
        let status = self.goal(goal_id)?.status;
        if status.terminal() {
            return Ok(self.goals[&goal_id].clone());
        }
        self.cascade_children(goal_id, spec.cascade, &spec.actor, &spec.reason)?;
        match status {
            GoalStatus::Active | GoalStatus::Cancelling => {
                let run = self
                    .runs
                    .values()
                    .find(|r| r.goal_id == goal_id && r.state.holds_slot())
                    .cloned()
                    .ok_or_else(|| {
                        CoordinatorError::Invalid("active goal has no slot-holding run".into())
                    })?;
                self.supersede_dispatch(run.id);
                if run.state == GoalRunState::Open {
                    let run_mut = self.runs.get_mut(&run.id).unwrap();
                    run_mut.state = GoalRunState::CancelRequested;
                    run_mut.wait_reason = None;
                    self.push_outbox(OutboxKind::Cancel {
                        goal_id,
                        run_id: run.id,
                        generation: run.generation,
                        target: run.target.clone(),
                        assignment_id: run.assignment_id,
                    });
                }
                let goal = self.goals.get_mut(&goal_id).unwrap();
                if goal.status != GoalStatus::Cancelling {
                    goal.transition(
                        spec.actor,
                        GoalTransition::RequestCancel {
                            reason: spec.reason,
                        },
                    )
                    .map_err(CoordinatorError::from_goal)?;
                }
            }
            GoalStatus::Queued
            | GoalStatus::Waiting
            | GoalStatus::Blocked
            | GoalStatus::Proposed => {
                self.drop_queue(goal_id);
                self.supersede_goal_dispatch(goal_id);
                for run in self
                    .runs
                    .values_mut()
                    .filter(|r| r.goal_id == goal_id && !r.state.terminal())
                {
                    run.state = GoalRunState::Cancelled;
                    run.finished_at = Some(Utc::now());
                    run.outcome = Some("cancelled".into());
                }
                if let Some(run) = self
                    .runs
                    .values()
                    .find(|r| r.goal_id == goal_id && r.state.holds_slot())
                    .cloned()
                {
                    self.release_slot(&run.target, run.id, run.generation)?;
                }
                let goal = self.goals.get_mut(&goal_id).unwrap();
                goal.transition(
                    spec.actor,
                    GoalTransition::Cancel {
                        reason: spec.reason,
                    },
                )
                .map_err(CoordinatorError::from_goal)?;
                self.release_assignment(goal_id, Utc::now());
                self.cancel_dependents(goal_id)?;
            }
            _ => {
                return Err(CoordinatorError::Invalid(format!(
                    "cannot cancel goal in {status:?}"
                )));
            }
        }
        Ok(self.goals[&goal_id].clone())
    }

    fn settle_cancel_inner(
        &mut self,
        actor: GoalActor,
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
    ) -> Result<Goal, CoordinatorError> {
        let run = self.fence_run(goal_id, run_id, generation)?;
        if run.state != GoalRunState::CancelRequested
            && self.goal(goal_id)?.status != GoalStatus::Cancelling
        {
            return Err(CoordinatorError::Invalid(
                "settle_cancel requires a fenced cancellation".into(),
            ));
        }
        self.release_slot(&run.target, run_id, generation)?;
        self.supersede_goal_dispatch(goal_id);
        let run = self.runs.get_mut(&run_id).unwrap();
        run.state = GoalRunState::Cancelled;
        run.finished_at = Some(Utc::now());
        run.outcome = Some("cancelled".into());
        let goal = self.goals.get_mut(&goal_id).unwrap();
        if !goal.status.terminal() {
            goal.transition(
                actor,
                GoalTransition::Cancel {
                    reason: goal
                        .status_reason
                        .clone()
                        .unwrap_or_else(|| "cancelled".into()),
                },
            )
            .map_err(CoordinatorError::from_goal)?;
        }
        self.release_assignment(goal_id, Utc::now());
        self.cancel_dependents(goal_id)?;
        Ok(self.goals[&goal_id].clone())
    }

    fn record_step_inner(
        &mut self,
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
        cost: Option<u64>,
    ) -> Result<GoalRun, CoordinatorError> {
        let run = self.fence_run(goal_id, run_id, generation)?;
        if run.state != GoalRunState::Open {
            return Err(CoordinatorError::Invalid(
                "steps can only be recorded on an open run".into(),
            ));
        }
        let add_cost = cost.unwrap_or(0);
        let goal = self.goal(goal_id)?.clone();
        self.check_budget(&goal, 1, add_cost)?;
        let now = Utc::now();
        let run = self.runs.get_mut(&run_id).unwrap();
        run.steps_consumed = run.steps_consumed.saturating_add(1);
        run.cost_consumed = run.cost_consumed.saturating_add(add_cost);
        run.last_heartbeat = now;
        if let Some(slot) = self.slots.get_mut(&run.target.key())
            && slot.run_id == run_id
        {
            slot.last_heartbeat = now;
        }
        Ok(run.clone())
    }

    fn heartbeat_inner(
        &mut self,
        run_id: GoalRunId,
        generation: u64,
        expires_at: Option<DateTime<Utc>>,
    ) -> Result<GoalRun, CoordinatorError> {
        let run = self
            .runs
            .get(&run_id)
            .ok_or(CoordinatorError::RunNotFound(run_id))?
            .clone();
        if run.generation != generation {
            return Err(CoordinatorError::StaleGeneration {
                expected: generation,
                actual: run.generation,
            });
        }
        if !run.state.holds_slot() {
            return Err(CoordinatorError::Invalid(
                "heartbeat requires a slot-holding run".into(),
            ));
        }
        let now = Utc::now();
        let run = self.runs.get_mut(&run_id).unwrap();
        run.last_heartbeat = now;
        if let Some(slot) = self.slots.get_mut(&run.target.key())
            && slot.run_id == run_id
        {
            slot.last_heartbeat = now;
            if expires_at.is_some() {
                slot.expires_at = expires_at;
            }
        }
        Ok(run.clone())
    }

    fn fence_expired_inner(
        &mut self,
        now: DateTime<Utc>,
    ) -> Result<ReconcileReport, CoordinatorError> {
        let expired: Vec<GoalRunId> = self
            .slots
            .values()
            .filter(|s| s.expires_at.is_some_and(|e| e <= now))
            .map(|s| s.run_id)
            .collect();
        self.interrupt_runs(expired, "claim expired")
    }

    fn reconcile_inner(&mut self, new_epoch: u64) -> Result<ReconcileReport, CoordinatorError> {
        if new_epoch <= self.daemon_epoch {
            return Err(CoordinatorError::EpochNotMonotonic {
                current: self.daemon_epoch,
                requested: new_epoch,
            });
        }
        let stale: Vec<GoalRunId> = self
            .runs
            .values()
            .filter(|r| r.state.holds_slot() && r.daemon_epoch < new_epoch)
            .map(|r| r.id)
            .collect();
        let mut report = self.interrupt_runs(stale, "daemon epoch fenced")?;
        // Force-settle cancellations that never received a worker ack.
        let cancelling: Vec<GoalId> = self
            .goals
            .values()
            .filter(|g| g.status == GoalStatus::Cancelling)
            .map(|g| g.id)
            .collect();
        for goal_id in cancelling {
            if let Some(run) = self
                .runs
                .values()
                .find(|r| r.goal_id == goal_id && r.state == GoalRunState::CancelRequested)
                .cloned()
            {
                self.settle_cancel_inner(GoalActor::System, goal_id, run.id, run.generation)?;
                report.cancelled.push(goal_id);
            }
        }
        self.daemon_epoch = new_epoch;
        report.epoch = new_epoch;
        Ok(report)
    }

    fn interrupt_runs(
        &mut self,
        run_ids: Vec<GoalRunId>,
        reason: &str,
    ) -> Result<ReconcileReport, CoordinatorError> {
        let mut report = ReconcileReport::default();
        let now = Utc::now();
        for run_id in run_ids {
            let run = match self.runs.get(&run_id) {
                Some(run) if run.state.holds_slot() => run.clone(),
                _ => continue,
            };
            self.release_slot(&run.target, run.id, run.generation)?;
            self.supersede_dispatch(run.id);
            let was_cancelling = self
                .goals
                .get(&run.goal_id)
                .is_some_and(|g| g.status == GoalStatus::Cancelling)
                || run.state == GoalRunState::CancelRequested;
            {
                let run_mut = self.runs.get_mut(&run_id).unwrap();
                run_mut.state = GoalRunState::Interrupted;
                run_mut.finished_at = Some(now);
                run_mut.result_summary = Some(reason.into());
                run_mut.outcome = Some("interrupted".into());
            }
            report.interrupted_runs.push(run_id);
            if was_cancelling {
                self.supersede_goal_dispatch(run.goal_id);
                let goal = self.goals.get_mut(&run.goal_id).unwrap();
                if !goal.status.terminal() {
                    goal.transition(
                        GoalActor::System,
                        GoalTransition::Cancel {
                            reason: reason.into(),
                        },
                    )
                    .map_err(CoordinatorError::from_goal)?;
                }
                self.release_assignment(run.goal_id, now);
                report.cancelled.push(run.goal_id);
            } else if run.retry_safe {
                self.requeue_goal(run.goal_id, GoalActor::System, reason.into())?;
                report.requeued.push(run.goal_id);
            } else {
                let goal = self.goals.get_mut(&run.goal_id).unwrap();
                goal.transition(
                    GoalActor::System,
                    GoalTransition::Block {
                        reason: reason.into(),
                    },
                )
                .map_err(CoordinatorError::from_goal)?;
                report.blocked.push(run.goal_id);
            }
        }
        Ok(report)
    }

    fn requeue_goal(
        &mut self,
        goal_id: GoalId,
        actor: GoalActor,
        reason: String,
    ) -> Result<(), CoordinatorError> {
        self.drop_queue(goal_id);
        let assignment = self.live_assignment(goal_id)?.clone();
        let goal = self.goals.get_mut(&goal_id).unwrap();
        let from = goal.status;
        let transition = if from == GoalStatus::Queued {
            None
        } else {
            Some(GoalTransition::Requeue { reason })
        };
        if let Some(transition) = transition {
            goal.transition(actor, transition)
                .map_err(CoordinatorError::from_goal)?;
        }
        self.enqueue_seq = self.enqueue_seq.saturating_add(1);
        let queue_id = GoalQueueEntryId::new();
        let entry = GoalQueueEntry {
            id: queue_id,
            assignment_id: assignment.id,
            goal_id,
            target: assignment.target,
            enqueue_seq: self.enqueue_seq,
            priority_class: assignment.priority_class,
            priority: assignment.priority,
            eligible_at: Utc::now(),
            deadline: self.goals[&goal_id].deadline,
            expected_goal_revision: self.goals[&goal_id].revision,
        };
        self.queue.insert(queue_id, entry);
        Ok(())
    }

    fn propagate_success(
        &mut self,
        child: GoalId,
        run_id: GoalRunId,
        generation: u64,
        actor: GoalActor,
    ) -> Result<(), CoordinatorError> {
        let parent_ids: Vec<(GoalDependencyId, GoalId)> = self
            .dependencies
            .values()
            .filter(|d| d.child_goal_id == child && d.state == DependencyState::Open)
            .map(|d| (d.id, d.parent_goal_id))
            .collect();
        for (dep_id, parent_id) in parent_ids {
            let satisfied = match &self.dependencies[&dep_id].kind {
                DependencyKind::GoalCondition { condition } if condition == "terminal" => true,
                DependencyKind::GoalCondition { condition }
                    if condition.starts_with("outcome:") =>
                {
                    self.runs
                        .get(&run_id)
                        .and_then(|run| run.outcome.as_deref())
                        .is_some_and(|actual| {
                            actual == condition.strip_prefix("outcome:").unwrap_or_default()
                        })
                }
                _ => true,
            };
            if !satisfied {
                continue;
            }
            {
                let dep = self.dependencies.get_mut(&dep_id).unwrap();
                dep.state = DependencyState::Satisfied;
                dep.child_run_id = Some(run_id);
            }
            self.push_outbox(OutboxKind::MilestoneReady {
                parent_goal_id: parent_id,
                child_goal_id: child,
                run_id,
                generation,
            });
            if self
                .goals
                .get(&parent_id)
                .is_some_and(|g| g.status == GoalStatus::Waiting)
                && self.live_assignment(parent_id).is_ok()
                && self.dependencies_ready(parent_id)
            {
                self.requeue_goal(parent_id, actor.clone(), format!("child {child} succeeded"))?;
            }
        }
        Ok(())
    }

    fn fail_dependents(&mut self, child: GoalId) -> Result<(), CoordinatorError> {
        let parent_ids: Vec<(GoalDependencyId, GoalId)> = self
            .dependencies
            .values()
            .filter(|d| d.child_goal_id == child && d.state == DependencyState::Open)
            .map(|d| (d.id, d.parent_goal_id))
            .collect();
        for (dep_id, parent_id) in parent_ids {
            self.dependencies.get_mut(&dep_id).unwrap().state = DependencyState::Failed;
            if let Some(parent) = self.goals.get_mut(&parent_id) {
                if !parent.status.terminal() && parent.status != GoalStatus::Active {
                    parent
                        .transition(
                            GoalActor::System,
                            GoalTransition::Block {
                                reason: format!("child {child} failed"),
                            },
                        )
                        .map_err(CoordinatorError::from_goal)?;
                    self.drop_queue(parent_id);
                }
            }
        }
        Ok(())
    }

    fn cancel_dependents(&mut self, child: GoalId) -> Result<(), CoordinatorError> {
        for dep in self.dependencies.values_mut() {
            if dep.child_goal_id == child && dep.state == DependencyState::Open {
                dep.state = DependencyState::Cancelled;
            }
        }
        Ok(())
    }

    fn cascade_children(
        &mut self,
        parent: GoalId,
        policy: ChildCascadePolicy,
        actor: &GoalActor,
        reason: &str,
    ) -> Result<(), CoordinatorError> {
        let children: Vec<GoalId> = self
            .dependencies
            .values()
            .filter(|d| d.parent_goal_id == parent && d.state == DependencyState::Open)
            .map(|d| d.child_goal_id)
            .collect();
        match policy {
            ChildCascadePolicy::Detach => {
                for child in children {
                    if let Some(dep) = self
                        .dependencies
                        .values_mut()
                        .find(|d| d.parent_goal_id == parent && d.child_goal_id == child)
                    {
                        dep.state = DependencyState::Cancelled;
                    }
                }
            }
            ChildCascadePolicy::Wait => {
                let live = children
                    .iter()
                    .any(|id| self.goals.get(id).is_some_and(|g| !g.status.terminal()));
                if live {
                    return Err(CoordinatorError::Invalid(
                        "children still live; cascade policy is wait".into(),
                    ));
                }
            }
            ChildCascadePolicy::Cancel => {
                for child in children {
                    if self.goals.get(&child).is_some_and(|g| !g.status.terminal()) {
                        self.cancel_inner(CancelSpec {
                            goal_id: child,
                            actor: actor.clone(),
                            reason: reason.to_string(),
                            cascade: ChildCascadePolicy::Cancel,
                            client_identity: None,
                            client_request_id: None,
                        })?;
                    }
                }
            }
        }
        Ok(())
    }

    fn add_edge(
        &mut self,
        parent: GoalId,
        child: GoalId,
        parent_run_id: Option<GoalRunId>,
        kind: DependencyKind,
    ) -> Result<GoalDependency, CoordinatorError> {
        if parent == child {
            return Err(CoordinatorError::CycleDetected);
        }
        if self.would_cycle(parent, child) {
            return Err(CoordinatorError::CycleDetected);
        }
        if self.dependencies.values().any(|d| {
            d.parent_goal_id == parent
                && d.child_goal_id == child
                && d.state == DependencyState::Open
        }) {
            return self
                .dependencies
                .values()
                .find(|d| d.parent_goal_id == parent && d.child_goal_id == child)
                .cloned()
                .ok_or_else(|| CoordinatorError::Invalid("missing dependency".into()));
        }
        let dep = GoalDependency {
            id: GoalDependencyId::new(),
            parent_goal_id: parent,
            child_goal_id: child,
            kind,
            state: DependencyState::Open,
            parent_run_id,
            child_run_id: None,
        };
        self.dependencies.insert(dep.id, dep.clone());
        Ok(dep)
    }

    fn would_cycle(&self, parent: GoalId, child: GoalId) -> bool {
        // parent waits on child. A cycle exists if child already (transitively)
        // waits on parent.
        let mut stack = vec![parent];
        let mut seen = BTreeSet::new();
        while let Some(node) = stack.pop() {
            if !seen.insert(node) {
                continue;
            }
            if node == child {
                return true;
            }
            for dep in self.dependencies.values() {
                if dep.state == DependencyState::Open && dep.child_goal_id == node {
                    stack.push(dep.parent_goal_id);
                }
            }
        }
        false
    }

    fn dependencies_ready(&self, goal_id: GoalId) -> bool {
        self.dependencies
            .values()
            .filter(|d| d.parent_goal_id == goal_id)
            .all(|d| {
                if d.state != DependencyState::Open {
                    return d.state == DependencyState::Satisfied;
                }
                self.goals
                    .get(&d.child_goal_id)
                    .is_some_and(|g| match &d.kind {
                        DependencyKind::GoalCondition { condition } if condition == "terminal" => {
                            g.status.terminal()
                        }
                        // Outcome strings are represented by the terminal goal
                        // status until the coordinator stores result labels.
                        DependencyKind::GoalCondition { condition }
                            if condition.starts_with("outcome:") =>
                        {
                            let expected = condition.strip_prefix("outcome:").unwrap_or_default();
                            self.runs
                                .values()
                                .filter(|run| run.goal_id == g.id && run.state.terminal())
                                .max_by_key(|run| run.finished_at)
                                .and_then(|run| run.outcome.as_deref())
                                .is_some_and(|actual| actual == expected)
                        }
                        _ => g.status == GoalStatus::Succeeded,
                    })
            })
    }

    fn verification_hold(&self, target: &AgentRef) -> Option<GoalRunId> {
        self.runs.values().find_map(|run| {
            if run.target == *target
                && run.state == GoalRunState::Waiting
                && run
                    .wait_reason
                    .as_ref()
                    .is_some_and(WaitReason::holds_verification_gate)
                && self
                    .goals
                    .get(&run.goal_id)
                    .is_some_and(|g| g.status == GoalStatus::Waiting)
            {
                Some(run.id)
            } else {
                None
            }
        })
    }

    fn select_next(&self, target: &AgentRef, now: DateTime<Utc>) -> Option<GoalQueueEntryId> {
        let mut eligible: Vec<&GoalQueueEntry> = self
            .queue
            .values()
            .filter(|e| {
                e.target == *target
                    && e.eligible_at <= now
                    && e.deadline.is_none_or(|d| d >= now)
                    && self.dependencies_ready(e.goal_id)
                    && self.goals.get(&e.goal_id).is_some_and(|g| {
                        matches!(g.status, GoalStatus::Queued | GoalStatus::Waiting)
                    })
            })
            .collect();
        eligible.sort_by(|a, b| queue_ord(a, b, now));
        eligible.first().map(|e| e.id)
    }

    fn fence_run(
        &self,
        goal_id: GoalId,
        run_id: GoalRunId,
        generation: u64,
    ) -> Result<GoalRun, CoordinatorError> {
        let run = self
            .runs
            .get(&run_id)
            .ok_or(CoordinatorError::RunNotFound(run_id))?
            .clone();
        if run.goal_id != goal_id {
            return Err(CoordinatorError::Invalid(
                "run does not belong to goal".into(),
            ));
        }
        if run.generation != generation {
            return Err(CoordinatorError::StaleGeneration {
                expected: generation,
                actual: run.generation,
            });
        }
        Ok(run)
    }

    fn release_slot(
        &mut self,
        target: &AgentRef,
        run_id: GoalRunId,
        generation: u64,
    ) -> Result<(), CoordinatorError> {
        let key = target.key();
        match self.slots.get(&key) {
            Some(slot) if slot.run_id == run_id && slot.generation == generation => {
                self.slots.remove(&key);
                Ok(())
            }
            Some(slot) => Err(CoordinatorError::StaleGeneration {
                expected: generation,
                actual: slot.generation,
            }),
            None => Ok(()),
        }
    }

    fn live_assignment(&self, goal_id: GoalId) -> Result<&GoalAssignment, CoordinatorError> {
        self.assignments
            .values()
            .find(|a| a.goal_id == goal_id && a.live())
            .ok_or_else(|| CoordinatorError::Invalid(format!("no live assignment for {goal_id}")))
    }

    fn release_assignment(&mut self, goal_id: GoalId, now: DateTime<Utc>) {
        for assignment in self.assignments.values_mut() {
            if assignment.goal_id == goal_id && assignment.live() {
                assignment.released_at = Some(now);
            }
        }
    }

    fn drop_queue(&mut self, goal_id: GoalId) {
        self.queue.retain(|_, e| e.goal_id != goal_id);
    }

    fn refresh_queue_revision(&mut self, goal_id: GoalId, revision: u64) {
        for entry in self.queue.values_mut() {
            if entry.goal_id == goal_id {
                entry.expected_goal_revision = revision;
            }
        }
    }

    fn push_outbox(&mut self, kind: OutboxKind) {
        let id = OutboxId::new();
        self.outbox.insert(
            id,
            OutboxEntry {
                id,
                kind,
                state: OutboxState::Pending,
                created_at: Utc::now(),
            },
        );
    }

    fn supersede_dispatch(&mut self, run_id: GoalRunId) {
        for entry in self.outbox.values_mut() {
            if entry.state == OutboxState::Pending
                && matches!(entry.kind, OutboxKind::Dispatch { run_id: id, .. } if id == run_id)
            {
                entry.state = OutboxState::Superseded;
            }
        }
    }

    fn supersede_goal_dispatch(&mut self, goal_id: GoalId) {
        for entry in self.outbox.values_mut() {
            if entry.state == OutboxState::Pending {
                match &entry.kind {
                    OutboxKind::Dispatch { goal_id: id, .. }
                    | OutboxKind::Cancel { goal_id: id, .. }
                        if *id == goal_id =>
                    {
                        entry.state = OutboxState::Superseded;
                    }
                    _ => {}
                }
            }
        }
    }

    fn check_budget(
        &self,
        goal: &Goal,
        extra_steps: u32,
        extra_cost: u64,
    ) -> Result<(), CoordinatorError> {
        let Some(budget) = &goal.budget else {
            return Ok(());
        };
        let steps: u32 = self
            .runs
            .values()
            .filter(|r| r.goal_id == goal.id)
            .map(|r| r.steps_consumed)
            .fold(0, u32::saturating_add)
            .saturating_add(extra_steps);
        let cost: u64 = self
            .runs
            .values()
            .filter(|r| r.goal_id == goal.id)
            .map(|r| r.cost_consumed)
            .fold(0, u64::saturating_add)
            .saturating_add(extra_cost);
        if budget.max_steps.is_some_and(|max| steps > max)
            || budget.max_cost.is_some_and(|max| cost > max)
        {
            return Err(CoordinatorError::BudgetExceeded);
        }
        Ok(())
    }

    fn consumed_steps(&self, goal_id: GoalId) -> u32 {
        self.runs
            .values()
            .filter(|r| r.goal_id == goal_id)
            .map(|r| r.steps_consumed)
            .fold(0, u32::saturating_add)
    }

    fn consumed_cost(&self, goal_id: GoalId) -> u64 {
        self.runs
            .values()
            .filter(|r| r.goal_id == goal_id)
            .map(|r| r.cost_consumed)
            .fold(0, u64::saturating_add)
    }
}

impl GoalCoordinator {
    pub fn steps_consumed(&self, goal_id: GoalId) -> u32 {
        self.consumed_steps(goal_id)
    }
    pub fn cost_consumed(&self, goal_id: GoalId) -> u64 {
        self.consumed_cost(goal_id)
    }
}

fn is_local_goal_transition(transition: &GoalTransition) -> bool {
    matches!(
        transition,
        GoalTransition::Approve
            | GoalTransition::Evaluate(_)
            | GoalTransition::AddCheck(_)
            | GoalTransition::LinkArtifact { .. }
            | GoalTransition::LinkEvidence { .. }
    )
}

fn wait_reason_text(reason: &WaitReason) -> String {
    match reason {
        WaitReason::Verification => "verification".into(),
        WaitReason::Child { child_goal_id } => format!("child {child_goal_id}"),
        WaitReason::Approval => "approval".into(),
        WaitReason::Timer => "timer".into(),
        WaitReason::Resource { name } => format!("resource {name}"),
        WaitReason::Other { reason } => reason.clone(),
    }
}

fn queue_ord(a: &GoalQueueEntry, b: &GoalQueueEntry, now: DateTime<Utc>) -> std::cmp::Ordering {
    effective_class(a, now)
        .as_rank()
        .cmp(&effective_class(b, now).as_rank())
        .then_with(|| b.priority.cmp(&a.priority))
        .then_with(|| {
            let da = a.deadline.map(|d| d.timestamp_millis()).unwrap_or(i64::MAX);
            let db = b.deadline.map(|d| d.timestamp_millis()).unwrap_or(i64::MAX);
            da.cmp(&db)
        })
        .then_with(|| a.enqueue_seq.cmp(&b.enqueue_seq))
}

fn effective_class(entry: &GoalQueueEntry, now: DateTime<Utc>) -> PriorityClass {
    let waited = now.signed_duration_since(entry.eligible_at).num_minutes();
    let steps = if waited <= 0 {
        0
    } else {
        (waited / PRIORITY_AGING_MINUTES) as u8
    };
    entry.priority_class.age(steps)
}

fn fingerprint_of<T: Serialize>(value: &T) -> Result<String, CoordinatorError> {
    serde_json::to_string(value).map_err(|e| CoordinatorError::Snapshot(e.to_string()))
}

fn replay_outcome<R: DeserializeOwned>(
    outcome: &IdempotencyOutcome,
) -> Result<R, CoordinatorError> {
    match outcome {
        IdempotencyOutcome::Success { encoded } => serde_json::from_value(encoded.clone())
            .map_err(|e| CoordinatorError::Snapshot(e.to_string())),
        IdempotencyOutcome::Error { message } => {
            Err(CoordinatorError::IdempotentError(message.clone()))
        }
    }
}

fn validate_coordinator(c: &GoalCoordinator) -> Result<(), CoordinatorError> {
    let mut slot_by_goal: BTreeMap<GoalId, &AgentGoalSlot> = BTreeMap::new();
    for (key, slot) in &c.slots {
        if key != &slot.target.key() {
            return Err(CoordinatorError::Invalid(
                "slot map key does not match target".into(),
            ));
        }
        if slot_by_goal.insert(slot.goal_id, slot).is_some() {
            return Err(CoordinatorError::Invalid(
                "multiple slots for one goal".into(),
            ));
        }
        let run = c
            .runs
            .get(&slot.run_id)
            .ok_or_else(|| CoordinatorError::Invalid("slot references missing run".into()))?;
        if run.goal_id != slot.goal_id
            || run.target != slot.target
            || run.generation != slot.generation
            || run.daemon_epoch != slot.daemon_epoch
            || !run.state.holds_slot()
        {
            return Err(CoordinatorError::Invalid(
                "slot/run disagree on occupancy".into(),
            ));
        }
        let current_gen = c.generations.get(key).copied().unwrap_or(0);
        if slot.generation != current_gen {
            return Err(CoordinatorError::Invalid(
                "slot generation is not the agent's current generation".into(),
            ));
        }
        let goal = c
            .goals
            .get(&slot.goal_id)
            .ok_or_else(|| CoordinatorError::Invalid("slot references missing goal".into()))?;
        if !goal.status.occupies_slot() {
            return Err(CoordinatorError::Invalid(
                "slot held for a goal that does not occupy a slot".into(),
            ));
        }
        if !c.assignments.contains_key(&slot.assignment_id) {
            return Err(CoordinatorError::Invalid(
                "slot references missing assignment".into(),
            ));
        }
    }

    for (key, claim) in &c.claims {
        if key != &claim.target.key() {
            return Err(CoordinatorError::Invalid(
                "claim map key does not match target".into(),
            ));
        }
        if c.slots.contains_key(key) {
            return Err(CoordinatorError::Invalid(
                "external claim overlaps a goal slot".into(),
            ));
        }
    }

    let mut live_assignment_by_goal: BTreeMap<GoalId, GoalAssignmentId> = BTreeMap::new();
    for assignment in c.assignments.values() {
        if !c.goals.contains_key(&assignment.goal_id) {
            return Err(CoordinatorError::Invalid(
                "assignment references missing goal".into(),
            ));
        }
        if assignment.live()
            && live_assignment_by_goal
                .insert(assignment.goal_id, assignment.id)
                .is_some()
        {
            return Err(CoordinatorError::Invalid(
                "multiple live assignments for one goal".into(),
            ));
        }
    }

    let mut queue_by_goal: BTreeMap<GoalId, u32> = BTreeMap::new();
    let mut seqs = BTreeSet::new();
    for entry in c.queue.values() {
        if !seqs.insert(entry.enqueue_seq) {
            return Err(CoordinatorError::Invalid("duplicate enqueue_seq".into()));
        }
        let assignment = c.assignments.get(&entry.assignment_id).ok_or_else(|| {
            CoordinatorError::Invalid("queue entry references missing assignment".into())
        })?;
        if assignment.goal_id != entry.goal_id || assignment.target != entry.target {
            return Err(CoordinatorError::Invalid(
                "queue entry disagrees with assignment".into(),
            ));
        }
        let goal = c.goals.get(&entry.goal_id).ok_or_else(|| {
            CoordinatorError::Invalid("queue entry references missing goal".into())
        })?;
        if goal.status.terminal() || goal.status.occupies_slot() {
            return Err(CoordinatorError::Invalid(
                "queue entry for a slot-holding or terminal goal".into(),
            ));
        }
        *queue_by_goal.entry(entry.goal_id).or_insert(0) += 1;
    }
    if queue_by_goal.values().any(|n| *n > 1) {
        return Err(CoordinatorError::Invalid(
            "goal has more than one queue entry".into(),
        ));
    }

    let mut holding_run_by_target: BTreeMap<String, GoalRunId> = BTreeMap::new();
    for run in c.runs.values() {
        if !c.goals.contains_key(&run.goal_id) {
            return Err(CoordinatorError::Invalid(
                "run references missing goal".into(),
            ));
        }
        if !c.assignments.contains_key(&run.assignment_id) {
            return Err(CoordinatorError::Invalid(
                "run references missing assignment".into(),
            ));
        }
        if run.state.holds_slot() {
            if holding_run_by_target
                .insert(run.target.key(), run.id)
                .is_some()
            {
                return Err(CoordinatorError::Invalid(
                    "multiple slot-holding runs for one agent".into(),
                ));
            }
            let slot = c
                .slots
                .get(&run.target.key())
                .ok_or_else(|| CoordinatorError::Invalid("slot-holding run has no slot".into()))?;
            if slot.run_id != run.id {
                return Err(CoordinatorError::Invalid(
                    "slot-holding run is not the slot occupant".into(),
                ));
            }
        }
        if run.state == GoalRunState::Waiting
            && c.slots
                .get(&run.target.key())
                .is_some_and(|s| s.run_id == run.id)
        {
            return Err(CoordinatorError::Invalid("waiting run holds a slot".into()));
        }
    }

    for goal in c.goals.values() {
        goal.validate().map_err(CoordinatorError::from_goal)?;
        let holding: Vec<_> = c
            .runs
            .values()
            .filter(|r| r.goal_id == goal.id && r.state.holds_slot())
            .collect();
        match goal.status {
            GoalStatus::Active => {
                if holding.len() != 1 {
                    return Err(CoordinatorError::Invalid(
                        "Active goal must have exactly one open run".into(),
                    ));
                }
                if holding[0].state != GoalRunState::Open {
                    return Err(CoordinatorError::Invalid(
                        "Active goal run must be Open".into(),
                    ));
                }
                if !slot_by_goal.contains_key(&goal.id) {
                    return Err(CoordinatorError::Invalid(
                        "Active goal must own a slot".into(),
                    ));
                }
                if queue_by_goal.contains_key(&goal.id) {
                    return Err(CoordinatorError::Invalid(
                        "Active goal must not be queued".into(),
                    ));
                }
            }
            GoalStatus::Cancelling => {
                if holding.len() != 1 || holding[0].state != GoalRunState::CancelRequested {
                    return Err(CoordinatorError::Invalid(
                        "Cancelling goal must have exactly one CancelRequested run".into(),
                    ));
                }
                if !slot_by_goal.contains_key(&goal.id) {
                    return Err(CoordinatorError::Invalid(
                        "Cancelling goal must own a slot".into(),
                    ));
                }
            }
            GoalStatus::Queued => {
                if !holding.is_empty() || slot_by_goal.contains_key(&goal.id) {
                    return Err(CoordinatorError::Invalid(
                        "Queued goal must not hold a slot or open run".into(),
                    ));
                }
                if !queue_by_goal.contains_key(&goal.id) {
                    return Err(CoordinatorError::Invalid(
                        "Queued goal must have a queue entry".into(),
                    ));
                }
            }
            GoalStatus::Waiting | GoalStatus::Blocked => {
                if !holding.is_empty() || slot_by_goal.contains_key(&goal.id) {
                    return Err(CoordinatorError::Invalid(
                        "waiting/blocked goal must not hold a slot".into(),
                    ));
                }
            }
            GoalStatus::Succeeded | GoalStatus::Failed | GoalStatus::Cancelled => {
                if !holding.is_empty()
                    || slot_by_goal.contains_key(&goal.id)
                    || queue_by_goal.contains_key(&goal.id)
                {
                    return Err(CoordinatorError::Invalid(
                        "terminal goal has residual slot, run, or queue".into(),
                    ));
                }
                let pending = c.outbox.values().any(|e| {
                    e.state == OutboxState::Pending
                        && match &e.kind {
                            OutboxKind::Dispatch { goal_id: id, .. }
                            | OutboxKind::Cancel { goal_id: id, .. } => *id == goal.id,
                            OutboxKind::MilestoneReady { .. } => false,
                        }
                });
                if pending {
                    return Err(CoordinatorError::Invalid(
                        "terminal goal has pending dispatch or cancel".into(),
                    ));
                }
            }
            GoalStatus::Proposed => {
                if slot_by_goal.contains_key(&goal.id) || queue_by_goal.contains_key(&goal.id) {
                    return Err(CoordinatorError::Invalid(
                        "Proposed goal must not have queue or slot".into(),
                    ));
                }
            }
        }
        if !goal.status.terminal()
            && goal.status != GoalStatus::Proposed
            && !live_assignment_by_goal.contains_key(&goal.id)
        {
            return Err(CoordinatorError::Invalid(
                "non-terminal goal needs a live assignment".into(),
            ));
        }
    }

    for dep in c.dependencies.values() {
        if !c.goals.contains_key(&dep.parent_goal_id) || !c.goals.contains_key(&dep.child_goal_id) {
            return Err(CoordinatorError::Invalid(
                "dependency references missing goal".into(),
            ));
        }
        if dep.parent_goal_id == dep.child_goal_id {
            return Err(CoordinatorError::CycleDetected);
        }
    }
    for dep in c.dependencies.values() {
        if dep.state == DependencyState::Open
            && c.would_cycle(dep.parent_goal_id, dep.child_goal_id)
        {
            return Err(CoordinatorError::CycleDetected);
        }
    }

    for entry in c.outbox.values() {
        match &entry.kind {
            OutboxKind::Dispatch {
                goal_id,
                run_id,
                assignment_id,
                ..
            }
            | OutboxKind::Cancel {
                goal_id,
                run_id,
                assignment_id,
                ..
            } => {
                if !c.goals.contains_key(goal_id)
                    || !c.runs.contains_key(run_id)
                    || !c.assignments.contains_key(assignment_id)
                {
                    return Err(CoordinatorError::Invalid(
                        "outbox references missing records".into(),
                    ));
                }
            }
            OutboxKind::MilestoneReady {
                parent_goal_id,
                child_goal_id,
                run_id,
                ..
            } => {
                if !c.goals.contains_key(parent_goal_id)
                    || !c.goals.contains_key(child_goal_id)
                    || !c.runs.contains_key(run_id)
                {
                    return Err(CoordinatorError::Invalid(
                        "milestone outbox references missing records".into(),
                    ));
                }
            }
        }
    }
    Ok(())
}
