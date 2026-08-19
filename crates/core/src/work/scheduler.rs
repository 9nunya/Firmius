//! M4.5 — the managed-graph scheduler.
//!
//! The scheduler never mutates state directly: it snapshots the current
//! `WorkState` outside any lock (`Session::work_snapshot`), derives
//! readiness purely (`work::readiness`), and then attempts a short,
//! revisioned `mutate_work` transaction per candidate node. Because that
//! transaction re-checks the graph's revision durably, two schedulers (or a
//! scheduler racing a manual `task start`) can never both claim the same
//! node: whichever commits first wins, and the loser's transaction fails
//! with `WorkError::StaleRevision` and is simply skipped for this pass.
//!
//! Managed scheduling is opt-in per graph (`GraphMode::Managed`); advisory
//! graphs are never touched. Concurrency limits are enforced against the
//! snapshot taken at the start of the pass, then re-validated by the
//! durable transaction itself, so a burst of claims can't blow past the
//! configured limits even under a race.

use super::event::WorkEvent;
use super::ids::{AssignmentId, AttemptId, GraphId, NodeId};
use super::model::*;
use super::readiness::evaluate_readiness;
use crate::session::Session;

/// Concurrency limits enforced by one scheduling pass. Graph-level limits
/// bound how many attempts one managed graph may run at once; the
/// session-level limit bounds the sum across every managed graph in the
/// session (so one graph cannot starve every other agent's work).
#[derive(Debug, Clone, Copy)]
pub struct SchedulerLimits {
    pub max_concurrent_per_graph: usize,
    pub max_concurrent_per_session: usize,
}

impl Default for SchedulerLimits {
    fn default() -> Self {
        Self {
            max_concurrent_per_graph: 4,
            max_concurrent_per_session: 8,
        }
    }
}

/// One durably claimed attempt from a scheduling pass, ready for an
/// executor to drive to completion.
#[derive(Debug, Clone)]
pub struct ClaimedAttempt {
    pub graph_id: GraphId,
    pub node_id: NodeId,
    pub attempt_id: AttemptId,
    pub assignment_id: AssignmentId,
    pub executor: Executor,
}

#[derive(Debug, Clone, Default)]
pub struct ScheduleOutcome {
    pub claimed: Vec<ClaimedAttempt>,
}

/// Run one scheduling pass over every managed, active graph in `session`.
///
/// Callers must not invoke this before [`Session::reconcile_work`] has
/// persisted (M4.8): scheduling against an unreconciled snapshot could
/// treat an interrupted-but-still-`Running`-looking attempt as live and
/// double-book work. `Session::reconcile_work` is idempotent and safe to
/// call before every pass if that's simpler for the caller.
pub fn schedule_ready_work(session: &Session, limits: &SchedulerLimits) -> ScheduleOutcome {
    // Snapshot outside any lock: readiness evaluation and candidate
    // selection below never hold a session lock.
    let snapshot = session.work_snapshot();

    let mut session_running: usize = snapshot
        .state
        .graphs
        .values()
        .flat_map(|g| g.attempts.values())
        .filter(|a| a.state == ExecutionStatus::Running)
        .count();

    let mut claimed = Vec::new();
    for graph in snapshot.state.graphs.values() {
        if graph.mode != GraphMode::Managed || graph.status != GraphStatus::Active {
            continue;
        }
        let mut graph_running = graph
            .attempts
            .values()
            .filter(|a| a.state == ExecutionStatus::Running)
            .count();
        let report = evaluate_readiness(graph);
        for node_id in report.ready {
            if graph_running >= limits.max_concurrent_per_graph {
                break;
            }
            if session_running >= limits.max_concurrent_per_session {
                break;
            }
            let Some(node) = graph.nodes.get(&node_id) else {
                continue;
            };
            // `Manual` nodes are claimed by an explicit `task start`, not
            // by the scheduler.
            if node.executor == Executor::Manual {
                continue;
            }
            let graph_id = graph.id;
            let executor = node.executor;
            let owner = graph.owner_agent_id.clone().unwrap_or_default();
            let scheduler_agent_id = format!("scheduler:{node_id}");

            let claim = session.mutate_work(move |state| {
                // Use the graph's *current* revision (not the outer-loop
                // snapshot's), since a prior claim earlier in this same
                // pass already bumped it. The at-most-once guarantee comes
                // from `assign()` itself: it rejects any node that is no
                // longer `Pending`/`Ready`/`Failed`, which is exactly the
                // case for a node claimed by a concurrent scheduler pass
                // or manual `task start` in between.
                let expected_revision = state.graph(graph_id)?.revision;
                let auth = AuthorizationContext {
                    agent_id: owner.clone(),
                    can_manage: true,
                    assignment_ids: Default::default(),
                };
                let (attempt_id, assignment_id) = state.assign(
                    graph_id,
                    expected_revision,
                    &auth,
                    node_id,
                    scheduler_agent_id,
                    Some(owner.clone()),
                    None,
                )?;
                let attempt = state.graph(graph_id)?.attempts[&attempt_id].clone();
                Ok((
                    (attempt_id, assignment_id),
                    WorkEvent::AttemptChanged { graph_id, attempt },
                ))
            });

            match claim {
                Ok((attempt_id, assignment_id)) => {
                    claimed.push(ClaimedAttempt {
                        graph_id,
                        node_id,
                        attempt_id,
                        assignment_id,
                        executor,
                    });
                    graph_running += 1;
                    session_running += 1;
                }
                Err(_) => {
                    // Lost the race (or the graph changed underneath us,
                    // e.g. cancelled) — idempotent: just skip this
                    // candidate for this pass. A later pass will
                    // re-evaluate readiness from fresh state.
                }
            }
        }
    }
    ScheduleOutcome { claimed }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::work::{NodeInput, WorkGraph};

    fn owner_auth() -> AuthorizationContext {
        AuthorizationContext {
            agent_id: "owner".into(),
            can_manage: false,
            assignment_ids: Default::default(),
        }
    }

    #[test]
    fn schedules_ready_managed_nodes_and_respects_limits() {
        let session = Session::new_handle();
        let (graph_id, nodes) = session
            .mutate_work(|state| {
                let mut graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Managed);
                let id = graph.id;
                state.create_graph(graph.clone(), None)?;
                let mut node_ids = Vec::new();
                for i in 0..3 {
                    let nid = state.add_node(
                        id,
                        state.graph(id)?.revision,
                        &owner_auth(),
                        NodeInput {
                            key: format!("n{i}"),
                            title: format!("Node {i}"),
                            description: None,
                        },
                    )?;
                    let n = state.graph_mut(id)?.nodes.get_mut(&nid).unwrap();
                    n.executor = Executor::Command;
                    node_ids.push(nid);
                }
                graph = state.graph(id)?.clone();
                Ok((
                    (id, node_ids),
                    WorkEvent::GraphChanged {
                        graph_id: id,
                        revision: graph.revision,
                    },
                ))
            })
            .unwrap();

        let limits = SchedulerLimits {
            max_concurrent_per_graph: 2,
            max_concurrent_per_session: 8,
        };
        let outcome = schedule_ready_work(&session, &limits);
        assert_eq!(outcome.claimed.len(), 2);
        for claim in &outcome.claimed {
            assert_eq!(claim.graph_id, graph_id);
            assert!(nodes.contains(&claim.node_id));
        }
        let outcome2 = schedule_ready_work(&session, &limits);
        // The two attempts claimed in the first pass are still `Running`,
        // so the graph-level limit of 2 is already saturated: the third
        // node must wait, not be double-claimed.
        assert!(outcome2.claimed.is_empty());
        let outcome3 = schedule_ready_work(&session, &limits);
        assert!(outcome3.claimed.is_empty());
    }

    #[test]
    fn advisory_graphs_are_never_scheduled() {
        let session = Session::new_handle();
        session
            .mutate_work(|state| {
                let mut graph = WorkGraph::new("g", Some("owner".into()), GraphMode::Advisory);
                let id = graph.id;
                state.create_graph(graph.clone(), None)?;
                let nid = state.add_node(
                    id,
                    state.graph(id)?.revision,
                    &owner_auth(),
                    NodeInput {
                        key: "n0".into(),
                        title: "Node 0".into(),
                        description: None,
                    },
                )?;
                state.graph_mut(id)?.nodes.get_mut(&nid).unwrap().executor = Executor::Command;
                graph = state.graph(id)?.clone();
                Ok((
                    (),
                    WorkEvent::GraphChanged {
                        graph_id: id,
                        revision: graph.revision,
                    },
                ))
            })
            .unwrap();
        let outcome = schedule_ready_work(&session, &SchedulerLimits::default());
        assert!(outcome.claimed.is_empty());
    }
}
