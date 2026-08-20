//! Drive a managed [`WorkGraph`] to completion without a supervising turn.
//!
//! Before this module, a "workflow" was whatever the parent agent did
//! between two tool calls: it had to poll readiness itself, spawn each
//! worker, wait for it, notice what that unblocked, and spawn the next
//! wave. The orchestration lived in a context window, so it ended when the
//! turn ended and nothing about it survived a restart.
//!
//! The driver moves that loop into durable state. It repeatedly derives
//! readiness (`work::readiness`), claims each ready node in its own
//! revisioned transaction, launches it, and settles the result — until the
//! graph reaches a terminal state. The parent starts a run and then either
//! polls it or waits for it; it never has to know which node comes next.
//!
//! Launching is injected rather than imported. Only the session/tool layer
//! has the persona, provider, and tool-registry wiring needed to create a
//! subagent, so this module stays a testable orchestration boundary and the
//! real launcher is supplied by the caller (mirroring `execute_agent`).
//!
//! Two invariants keep a runaway impossible:
//!
//! - Every claim goes through `WorkState::assign`, which enforces the
//!   node's `retry_policy` cap, so a node cannot be attempted forever.
//! - A pass that claims nothing and has nothing running is terminal, so the
//!   loop cannot spin on a graph whose remaining nodes are blocked.

use super::event::WorkEvent;
use super::ids::{GraphId, NodeId};
use super::model::*;
use super::readiness::evaluate_readiness;
use crate::session::Session;
use futures::future::BoxFuture;
use std::sync::Arc;

/// Creates and runs the subagent for one `Executor::Agent` node.
///
/// Spawning is split from running so the driver can make the assignment
/// durable *before* any work starts: the child must already own its node
/// (and see it via `task view`) by the time its first turn executes.
pub trait NodeLauncher: Send + Sync {
    /// Create the agent for `spec` and return its id. The agent must exist
    /// in the session (so it can be assigned to) but must not have run yet.
    fn spawn(&self, spec: &AgentSpec) -> BoxFuture<'_, Result<String, String>>;

    /// Run an already-spawned agent to completion with `prompt`, returning
    /// its final text. The driver settles the assignment afterwards if the
    /// agent did not settle it itself.
    fn run(&self, agent_id: String, prompt: String) -> BoxFuture<'_, Result<String, String>>;
}

/// What one node did during a run.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NodeOutcome {
    pub node_key: String,
    pub status: ExecutionStatus,
    pub summary: String,
}

/// Why a run stopped.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RunConclusion {
    /// Every node reached a terminal state.
    Settled,
    /// Progress stopped while work remained: the rest is blocked, or capped
    /// by retry policy. Not an error, but the graph is not finished.
    Stalled,
    /// The run was cancelled before it could conclude.
    Cancelled,
}

#[derive(Debug, Clone)]
pub struct RunReport {
    pub graph_id: GraphId,
    pub conclusion: RunConclusion,
    pub outcomes: Vec<NodeOutcome>,
}

/// Bounds one run's fan-out. Independent of `SchedulerLimits` because a
/// single driven graph is a narrower scope than the whole session.
#[derive(Debug, Clone, Copy)]
pub struct RunLimits {
    pub max_concurrent: usize,
    /// Hard ceiling on launched attempts for the whole run. Retry caps
    /// already bound each node; this bounds the run as a whole so an
    /// unexpectedly large graph cannot spend without limit.
    pub max_attempts_total: usize,
}

impl Default for RunLimits {
    fn default() -> Self {
        Self {
            max_concurrent: 8,
            max_attempts_total: 200,
        }
    }
}

/// Claim `node_id` for `agent_id` in its own durable transaction.
///
/// Returns `Ok(false)` when the node is no longer claimable (another pass,
/// a manual `task start`, or a retry cap got there first), which the caller
/// treats as "skip", never as a failure: readiness is re-derived next pass.
fn claim(
    session: &Arc<Session>,
    graph_id: GraphId,
    node_id: NodeId,
    agent_id: &str,
    owner: &str,
) -> Result<bool, String> {
    let agent_id = agent_id.to_string();
    let owner = owner.to_string();
    let claimed = session.mutate_work(move |state| {
        let expected = state.graph(graph_id)?.revision;
        let auth = AuthorizationContext {
            agent_id: owner.clone(),
            can_manage: true,
            assignment_ids: Default::default(),
        };
        let (attempt_id, _) = state.assign(
            graph_id,
            expected,
            &auth,
            node_id,
            agent_id.clone(),
            Some(owner.clone()),
            None,
        )?;
        let attempt = state.graph(graph_id)?.attempts[&attempt_id].clone();
        Ok(((), WorkEvent::AttemptChanged { graph_id, attempt }))
    });
    match claimed {
        Ok(()) => Ok(true),
        // Lost the race or hit the node's retry cap. Both mean "not ours
        // to run", and neither should abort the run.
        Err(_) => Ok(false),
    }
}

/// Settle a node the driver launched, unless the agent already settled it
/// (by calling `yield`), in which case its own result stands.
fn settle_if_unsettled(
    session: &Arc<Session>,
    graph_id: GraphId,
    agent_id: &str,
    status: ExecutionStatus,
    outcome: Outcome,
    summary: String,
) {
    let binding = {
        let state = session.work.read().unwrap();
        state.binding_for_agent(agent_id).cloned()
    };
    let Some(binding) = binding else {
        return;
    };
    let agent_id = agent_id.to_string();
    let agent_id_for_log = agent_id.clone();
    let result = session.mutate_work(move |state| {
        let expected = state.graph(binding.graph_id)?.revision;
        let auth = AuthorizationContext {
            agent_id: agent_id.clone(),
            assignment_ids: [binding.assignment_id].into_iter().collect(),
            ..Default::default()
        };
        let result_id = state.settle_assignment(
            binding.graph_id,
            expected,
            &auth,
            binding.assignment_id,
            status,
            Some(outcome.clone()),
            summary.clone(),
            None,
            Vec::new(),
            Vec::new(),
            Vec::new(),
            Vec::new(),
            VerificationLevel::None,
        )?;
        let result = state.graph(binding.graph_id)?.results[&result_id].clone();
        Ok(((), WorkEvent::ResultRecorded { graph_id, result }))
    });
    if let Err(error) = result {
        eprintln!("warning: run could not settle node for {agent_id_for_log}: {error}");
    }
}

/// True while any attempt in the graph is still running.
fn has_running(session: &Arc<Session>, graph_id: GraphId) -> bool {
    session
        .work
        .read()
        .unwrap()
        .graph(graph_id)
        .map(|g| {
            g.attempts
                .values()
                .any(|a| a.state == ExecutionStatus::Running)
        })
        .unwrap_or(false)
}

/// Drive `graph_id` until it settles, stalls, or is cancelled.
///
/// Each wave claims every currently-ready node (up to `max_concurrent`),
/// runs them concurrently, and settles them; the next wave re-derives
/// readiness from the freshly settled state, which is what lets a fan-in
/// node become ready the moment its last predecessor lands.
pub async fn drive_run(
    session: Arc<Session>,
    graph_id: GraphId,
    launcher: Arc<dyn NodeLauncher>,
    limits: RunLimits,
    cancellation: tokio_util::sync::CancellationToken,
) -> RunReport {
    let owner = session
        .work
        .read()
        .unwrap()
        .graph(graph_id)
        .ok()
        .and_then(|g| g.owner_agent_id.clone())
        .unwrap_or_default();

    let mut launched_total = 0usize;
    let conclusion = loop {
        if cancellation.is_cancelled() {
            break RunConclusion::Cancelled;
        }

        // Derive readiness from a snapshot, then claim durably. The
        // snapshot can go stale between these two steps; `claim` failing is
        // exactly how that race is resolved.
        let ready: Vec<(NodeId, Executor, Option<AgentSpec>)> = {
            let state = session.work.read().unwrap();
            let Ok(graph) = state.graph(graph_id) else {
                break RunConclusion::Stalled;
            };
            if graph.status != GraphStatus::Active {
                break RunConclusion::Settled;
            }
            evaluate_readiness(graph)
                .ready
                .into_iter()
                .filter_map(|id| {
                    let node = graph.nodes.get(&id)?;
                    // Manual nodes belong to a human or the owning agent;
                    // the driver never claims them out from under them.
                    (node.executor != Executor::Manual)
                        .then(|| (id, node.executor, node.agent.clone()))
                })
                .take(limits.max_concurrent)
                .collect()
        };

        if ready.is_empty() {
            // Nothing to start. If work is still in flight, wait for it to
            // land and re-derive; otherwise the run cannot progress.
            if has_running(&session, graph_id) {
                tokio::select! {
                    _ = cancellation.cancelled() => break RunConclusion::Cancelled,
                    _ = tokio::time::sleep(std::time::Duration::from_millis(50)) => continue,
                }
            }
            let settled = {
                let state = session.work.read().unwrap();
                state
                    .graph(graph_id)
                    .map(|g| {
                        g.nodes.values().all(|n| {
                            matches!(
                                n.status,
                                ExecutionStatus::Succeeded
                                    | ExecutionStatus::Cancelled
                                    | ExecutionStatus::Skipped
                            )
                        })
                    })
                    .unwrap_or(false)
            };
            break if settled {
                RunConclusion::Settled
            } else {
                RunConclusion::Stalled
            };
        }

        let mut wave = Vec::new();
        for (node_id, executor, spec) in ready {
            if launched_total >= limits.max_attempts_total {
                break;
            }
            let Some(spec) = spec else {
                // A managed graph rejects an agent node with no spec at
                // author time, so this is only reachable for a non-agent
                // executor the driver does not handle yet.
                debug_assert!(executor != Executor::Agent);
                continue;
            };
            let agent_id = match launcher.spawn(&spec).await {
                Ok(id) => id,
                Err(error) => {
                    eprintln!("warning: run could not spawn agent for a node: {error}");
                    continue;
                }
            };
            // Claim BEFORE running, so the child already owns its node (and
            // sees it via `task view`) on its very first turn.
            match claim(&session, graph_id, node_id, &agent_id, &owner) {
                Ok(true) => {}
                Ok(false) | Err(_) => continue,
            }
            launched_total += 1;

            // Compose from state that now includes this attempt's frozen
            // manifest, so the worker receives exactly its bound inputs.
            let prompt = {
                let state = session.work.read().unwrap();
                let Ok(graph) = state.graph(graph_id) else {
                    continue;
                };
                let Some(node) = graph.nodes.get(&node_id) else {
                    continue;
                };
                let manifest = state
                    .binding_for_agent(&agent_id)
                    .and_then(|b| graph.attempts.get(&b.attempt_id))
                    .and_then(|a| a.input_manifest_id)
                    .and_then(|id| graph.manifests.get(&id));
                super::inputs::compose_node_context(graph, node, manifest, &spec.prompt)
            };

            let launcher = launcher.clone();
            let session = session.clone();
            wave.push(async move {
                let outcome = launcher.run(agent_id.clone(), prompt).await;
                let (status, outcome_kind, summary) = match outcome {
                    Ok(text) => (ExecutionStatus::Succeeded, Outcome::Success, text),
                    Err(error) => (
                        ExecutionStatus::Failed,
                        Outcome::Failure,
                        format!("node agent failed: {error}"),
                    ),
                };
                settle_if_unsettled(
                    &session,
                    graph_id,
                    &agent_id,
                    status,
                    outcome_kind,
                    summary,
                );
            });
        }

        if wave.is_empty() {
            break RunConclusion::Stalled;
        }
        tokio::select! {
            _ = cancellation.cancelled() => break RunConclusion::Cancelled,
            _ = futures::future::join_all(wave) => {}
        }
    };

    let outcomes = {
        let state = session.work.read().unwrap();
        state
            .graph(graph_id)
            .map(|graph| {
                graph
                    .view_order
                    .iter()
                    .filter_map(|id| graph.nodes.get(id))
                    .map(|node| NodeOutcome {
                        node_key: node.key.clone(),
                        status: node.status,
                        summary: node
                            .attempt_ids
                            .last()
                            .and_then(|a| graph.attempts.get(a))
                            .and_then(|a| a.result_id)
                            .and_then(|r| graph.results.get(&r))
                            .map(|r| r.summary.clone())
                            .unwrap_or_default(),
                    })
                    .collect()
            })
            .unwrap_or_default()
    };
    RunReport {
        graph_id,
        conclusion,
        outcomes,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::work::{PlannedEdge, PlannedNode, WorkGraph};
    use std::sync::Mutex;

    /// Records what each node was told, and lets a test script failures.
    /// Standing in for the provider/persona wiring keeps these tests about
    /// orchestration: ordering, fan-in, settlement, and bounding.
    struct FakeLauncher {
        seen: Mutex<Vec<(String, String)>>,
        fail_prompts_containing: Option<String>,
        next_id: Mutex<usize>,
    }

    impl FakeLauncher {
        fn new() -> Arc<Self> {
            Arc::new(Self {
                seen: Mutex::new(Vec::new()),
                fail_prompts_containing: None,
                next_id: Mutex::new(0),
            })
        }
        fn failing(marker: &str) -> Arc<Self> {
            Arc::new(Self {
                seen: Mutex::new(Vec::new()),
                fail_prompts_containing: Some(marker.to_string()),
                next_id: Mutex::new(0),
            })
        }
        fn prompt_for(&self, fragment: &str) -> Option<String> {
            self.seen
                .lock()
                .unwrap()
                .iter()
                .find(|(_, prompt)| prompt.contains(fragment))
                .map(|(_, prompt)| prompt.clone())
        }
    }

    impl NodeLauncher for FakeLauncher {
        fn spawn(&self, _spec: &AgentSpec) -> BoxFuture<'_, Result<String, String>> {
            Box::pin(async move {
                let mut next = self.next_id.lock().unwrap();
                *next += 1;
                Ok(format!("child-{next}"))
            })
        }
        fn run(&self, agent_id: String, prompt: String) -> BoxFuture<'_, Result<String, String>> {
            Box::pin(async move {
                self.seen.lock().unwrap().push((agent_id, prompt.clone()));
                if let Some(marker) = &self.fail_prompts_containing
                    && prompt.contains(marker.as_str())
                {
                    return Err("scripted failure".into());
                }
                Ok(format!("done: {}", prompt.lines().last().unwrap_or("")))
            })
        }
    }

    fn spec(prompt: &str) -> Option<AgentSpec> {
        Some(AgentSpec {
            persona: "coder".into(),
            prompt: prompt.into(),
            model: None,
            effort: None,
        })
    }

    /// Author a fan-in: `count` workers feeding one synthesizer.
    fn fan_in_session(count: usize) -> (Arc<Session>, GraphId) {
        let session = Session::new_handle();
        let graph = WorkGraph::new("run", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        session
            .mutate_work(move |state| {
                state.create_graph(graph, None)?;
                let auth = AuthorizationContext {
                    agent_id: "owner".into(),
                    ..Default::default()
                };
                let mut nodes: Vec<PlannedNode> = (1..=count)
                    .map(|i| PlannedNode {
                        key: format!("w{i}"),
                        title: format!("worker {i}"),
                        executor: Executor::Agent,
                        agent: spec(&format!("do slice {i}")),
                        ..Default::default()
                    })
                    .collect();
                nodes.push(PlannedNode {
                    key: "syn".into(),
                    title: "synthesize".into(),
                    executor: Executor::Agent,
                    agent: spec("merge the findings"),
                    join: Some(JoinPolicy::AllSucceeded),
                    ..Default::default()
                });
                let edges: Vec<PlannedEdge> = (1..=count)
                    .map(|i| PlannedEdge {
                        from: format!("w{i}"),
                        to: "syn".into(),
                        condition: EdgeCondition::Succeeded,
                        required: true,
                        binding_alias: Some(format!("finding_{i}")),
                        binding_field: None,
                    })
                    .collect();
                state.plan(
                    graph_id,
                    state.graph(graph_id)?.revision,
                    &auth,
                    nodes,
                    edges,
                    Some("Shared brief for the whole run.".into()),
                    Some(true),
                )?;
                let revision = state.graph(graph_id)?.revision;
                Ok((
                    (),
                    WorkEvent::GraphChanged {
                        graph_id,
                        revision,
                    },
                ))
            })
            .unwrap();
        (session, graph_id)
    }

    /// The workflow this whole design exists for, run unattended: ten
    /// workers in parallel, then a synthesizer that starts only once every
    /// one of them has landed and receives all ten results.
    #[tokio::test]
    async fn drives_a_fan_in_to_completion_without_a_supervising_turn() {
        let (session, graph_id) = fan_in_session(10);
        let launcher = FakeLauncher::new();
        let report = drive_run(
            session.clone(),
            graph_id,
            launcher.clone(),
            RunLimits::default(),
            tokio_util::sync::CancellationToken::new(),
        )
        .await;

        assert_eq!(report.conclusion, RunConclusion::Settled);
        assert_eq!(report.outcomes.len(), 11);
        for outcome in &report.outcomes {
            assert_eq!(
                outcome.status,
                ExecutionStatus::Succeeded,
                "{} did not succeed",
                outcome.node_key
            );
        }

        // The synthesizer ran last and received every worker's result.
        let syn_prompt = launcher
            .prompt_for("merge the findings")
            .expect("synthesizer ran");
        for i in 1..=10 {
            assert!(
                syn_prompt.contains(&format!("finding_{i}")),
                "synthesizer missing finding_{i}: {syn_prompt}"
            );
        }
        // And the shared brief reached every agent.
        let worker_prompt = launcher.prompt_for("do slice 1").expect("worker ran");
        assert!(worker_prompt.contains("Shared brief for the whole run."));
    }

    /// A failed predecessor must not silently strand its successor: the
    /// join can no longer be satisfied, so the run stops with the graph
    /// unfinished rather than spinning or reporting success.
    #[tokio::test]
    async fn a_failed_predecessor_stalls_the_dependent_node() {
        let (session, graph_id) = fan_in_session(2);
        let launcher = FakeLauncher::failing("do slice 2");
        let report = drive_run(
            session.clone(),
            graph_id,
            launcher,
            RunLimits::default(),
            tokio_util::sync::CancellationToken::new(),
        )
        .await;

        assert_eq!(report.conclusion, RunConclusion::Stalled);
        let by_key = |key: &str| {
            report
                .outcomes
                .iter()
                .find(|o| o.node_key == key)
                .unwrap()
                .status
        };
        assert_eq!(by_key("w1"), ExecutionStatus::Succeeded);
        assert_eq!(by_key("w2"), ExecutionStatus::Failed);
        assert_eq!(
            by_key("syn"),
            ExecutionStatus::Pending,
            "the synthesizer must not run on an unsatisfiable join"
        );
    }

    /// Manual nodes belong to a person or the owning agent. A run must
    /// leave them alone rather than claiming them automatically.
    #[tokio::test]
    async fn manual_nodes_are_never_claimed_by_a_run() {
        let session = Session::new_handle();
        let graph = WorkGraph::new("run", Some("owner".into()), GraphMode::Managed);
        let graph_id = graph.id;
        session
            .mutate_work(move |state| {
                state.create_graph(graph, None)?;
                let auth = AuthorizationContext {
                    agent_id: "owner".into(),
                    ..Default::default()
                };
                state.plan(
                    graph_id,
                    0,
                    &auth,
                    vec![PlannedNode {
                        key: "manual".into(),
                        title: "a human decides".into(),
                        ..Default::default()
                    }],
                    Vec::new(),
                    None,
                    Some(true),
                )?;
                let revision = state.graph(graph_id)?.revision;
                Ok((
                    (),
                    WorkEvent::GraphChanged {
                        graph_id,
                        revision,
                    },
                ))
            })
            .unwrap();

        let report = drive_run(
            session.clone(),
            graph_id,
            FakeLauncher::new(),
            RunLimits::default(),
            tokio_util::sync::CancellationToken::new(),
        )
        .await;
        assert_eq!(report.conclusion, RunConclusion::Stalled);
        assert_eq!(report.outcomes[0].status, ExecutionStatus::Pending);
    }

    /// A run is bounded: once the total attempt ceiling is reached it stops
    /// launching, so an unexpectedly large graph cannot spend without end.
    #[tokio::test]
    async fn a_run_stops_at_its_total_attempt_ceiling() {
        let (session, graph_id) = fan_in_session(5);
        let report = drive_run(
            session.clone(),
            graph_id,
            FakeLauncher::new(),
            RunLimits {
                max_concurrent: 2,
                max_attempts_total: 3,
            },
            tokio_util::sync::CancellationToken::new(),
        )
        .await;
        assert_eq!(report.conclusion, RunConclusion::Stalled);
        let succeeded = report
            .outcomes
            .iter()
            .filter(|o| o.status == ExecutionStatus::Succeeded)
            .count();
        assert_eq!(succeeded, 3, "exactly the ceiling was launched");
    }
}
