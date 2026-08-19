//! Atomic, revisioned WorkGraph mutations and invariant validation.

use super::ids::*;
use super::model::*;
use chrono::{DateTime, Utc};
use std::collections::BTreeSet;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum WorkError {
    #[error("graph not found: {0}")]
    GraphNotFound(GraphId),
    #[error("node not found: {0}")]
    NodeNotFound(NodeId),
    #[error("expected graph revision {expected}, actual revision {actual}")]
    StaleRevision { expected: u64, actual: u64 },
    #[error("graph is not active")]
    GraphNotActive,
    #[error("invalid transition for node {node}: {from:?} -> {to:?}")]
    InvalidTransition {
        node: NodeId,
        from: ExecutionStatus,
        to: ExecutionStatus,
    },
    #[error("duplicate node key: {0}")]
    DuplicateKey(String),
    #[error("unauthorized mutation: agent {agent} cannot mutate graph")]
    Unauthorized { agent: String },
    #[error("invalid graph: {0}")]
    InvalidGraph(String),
    #[error("attempt has no result")]
    MissingResult,
    #[error("retry is not available")]
    RetryUnavailable,
    #[error("assignment not found: {0}")]
    AssignmentNotFound(AssignmentId),
    #[error("assignment is owned by another agent")]
    AssignmentNotOwned,
    #[error("assignment is already settled")]
    AssignmentSettled,
    #[error("reviewer must be independent of the producer they are reviewing")]
    ReviewerNotIndependent,
    #[error("annotation references missing result: {0}")]
    ResultNotFound(ResultId),
}

/// The scope of a graph mutation, used to decide whether a non-owner
/// assignee may perform it. `Topology` covers structural graph changes
/// (add/remove/move nodes, edges) that only the owner may perform.
/// `Node` covers per-node attempt/result/status changes, which an assignee
/// may perform only for the node they hold a live assignment on.
/// `Annotate` is the M5 exception: any graph owner or any agent with an
/// active (unreleased) assignment in the graph may annotate a result,
/// including a reviewer assigned to a different node than the producer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WorkOp {
    Topology,
    Node(NodeId),
    Annotate,
}

/// Enforce the retry cap in one place: any path that starts a new attempt
/// on a node (direct `start`, atomic `assign`, or explicit `retry`) must go
/// through this so `Failed -> Running` cannot bypass `retry_policy`.
fn check_attempt_cap(node: &WorkNode) -> Result<(), WorkError> {
    if node.retry_policy.max_attempts != 0
        && node.attempt_ids.len() as u32 >= node.retry_policy.max_attempts
    {
        return Err(WorkError::RetryUnavailable);
    }
    Ok(())
}

/// Release every still-active claim tied to `assignment_id`. Called from
/// every settlement path (`settle_assignment`, `cancel`,
/// `reconcile_interrupted`) so an advisory claim never outlives the
/// assignment that acquired it.
fn release_claims_for_assignment(
    graph: &mut WorkGraph,
    assignment_id: AssignmentId,
    now: DateTime<Utc>,
) {
    for claim in graph.claims.values_mut() {
        if claim.assignment_id == assignment_id && claim.released_at.is_none() {
            claim.released_at = Some(now);
        }
    }
}

/// Normalize an advisory claim path: resolve `.`/`..` lexically and reject
/// any path that escapes the session/workspace boundary (an absolute path,
/// or one whose lexical traversal would climb above the root). This is
/// advisory bookkeeping only — it does not touch the filesystem.
fn normalize_claim_path(path: &str) -> Result<String, WorkError> {
    if path.trim().is_empty() {
        return Err(WorkError::InvalidGraph("claim path is empty".into()));
    }
    if path.starts_with('/') || path.contains('\\') {
        return Err(WorkError::InvalidGraph(format!(
            "claim path escapes the workspace boundary: '{path}'"
        )));
    }
    let mut stack: Vec<&str> = Vec::new();
    for component in path.split('/') {
        match component {
            "" | "." => continue,
            ".." => {
                if stack.pop().is_none() {
                    return Err(WorkError::InvalidGraph(format!(
                        "claim path escapes the workspace boundary: '{path}'"
                    )));
                }
            }
            other => stack.push(other),
        }
    }
    if stack.is_empty() {
        return Err(WorkError::InvalidGraph(format!(
            "claim path is empty after normalization: '{path}'"
        )));
    }
    Ok(stack.join("/"))
}

impl WorkState {
    /// Atomically claim a node for an agent. The attempt and assignment are
    /// created in the same candidate graph revision, so a durable snapshot
    /// can never expose an assignment without its attempt (or vice versa).
    pub fn assign(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node_id: NodeId,
        agent_id: impl Into<String>,
        parent_agent_id: Option<String>,
        task_id: Option<String>,
    ) -> Result<(AttemptId, AssignmentId), WorkError> {
        let attempt_id = AttemptId::new();
        let assignment_id = AssignmentId::new();
        let agent_id = agent_id.into();
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            if !super::readiness::is_independent_reviewer(g, node_id, &agent_id) {
                return Err(WorkError::ReviewerNotIndependent);
            }
            let node = g
                .nodes
                .get_mut(&node_id)
                .ok_or(WorkError::NodeNotFound(node_id))?;
            if !matches!(
                node.status,
                ExecutionStatus::Pending | ExecutionStatus::Ready | ExecutionStatus::Failed
            ) {
                return Err(WorkError::InvalidTransition {
                    node: node_id,
                    from: node.status,
                    to: ExecutionStatus::Running,
                });
            }
            check_attempt_cap(node)?;
            let number = node.attempt_ids.len() as u32 + 1;
            node.status = ExecutionStatus::Running;
            node.effective_outcome = None;
            node.revision = node.revision.saturating_add(1);
            node.attempt_ids.push(attempt_id);
            let manifest = g.freeze_manifest(node_id);
            let manifest_id = manifest.id;
            g.manifests.insert(manifest_id, manifest);
            g.attempts.insert(
                attempt_id,
                NodeAttempt {
                    id: attempt_id,
                    node_id,
                    number,
                    state: ExecutionStatus::Running,
                    started_at: Some(Utc::now()),
                    finished_at: None,
                    agent_id: Some(agent_id.clone()),
                    assignment_id: Some(assignment_id),
                    result_id: None,
                    input_manifest_id: Some(manifest_id),
                },
            );
            g.assignments.insert(
                assignment_id,
                WorkAssignment {
                    id: assignment_id,
                    node_id,
                    attempt_id,
                    agent_id: agent_id.clone(),
                    parent_agent_id: parent_agent_id.clone(),
                    assigned_at: Utc::now(),
                    released_at: None,
                },
            );
            Ok(())
        })?;
        self.active_binding_by_agent.insert(
            agent_id,
            AgentWorkBinding {
                graph_id: graph,
                node_id,
                attempt_id,
                assignment_id,
                task_id,
            },
        );
        Ok((attempt_id, assignment_id))
    }

    /// Hand a live `Running` node from the current assignee (typically the
    /// parent who `task start`ed it) to a worker. Reuses the open attempt
    /// instead of opening a second `Running` claim — `task start` then
    /// `delegate … task_id=` must compose, not explode with
    /// `Running -> Running`.
    ///
    /// Legal only while the node is `Running` with an unfinished attempt
    /// whose current assignment (if any) is still open. Terminal,
    /// pending, and already-settled nodes still go through [`assign`].
    pub fn reassign(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node_id: NodeId,
        agent_id: impl Into<String>,
        parent_agent_id: Option<String>,
        task_id: Option<String>,
    ) -> Result<(AttemptId, AssignmentId), WorkError> {
        let assignment_id = AssignmentId::new();
        let agent_id = agent_id.into();
        let mut attempt_id = AttemptId::new();
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            if !super::readiness::is_independent_reviewer(g, node_id, &agent_id) {
                return Err(WorkError::ReviewerNotIndependent);
            }
            let node = g
                .nodes
                .get(&node_id)
                .ok_or(WorkError::NodeNotFound(node_id))?;
            if node.status != ExecutionStatus::Running {
                return Err(WorkError::InvalidTransition {
                    node: node_id,
                    from: node.status,
                    to: ExecutionStatus::Running,
                });
            }
            let current_attempt_id = *node.attempt_ids.last().ok_or(WorkError::InvalidGraph(
                "running node has no attempt".into(),
            ))?;
            let current = g
                .attempts
                .get(&current_attempt_id)
                .ok_or(WorkError::InvalidGraph("missing attempt".into()))?;
            if current.finished_at.is_some() || current.result_id.is_some() {
                return Err(WorkError::InvalidGraph(
                    "cannot reassign a settled attempt".into(),
                ));
            }
            if let Some(existing) = current.assignment_id {
                let assignment = g
                    .assignments
                    .get_mut(&existing)
                    .ok_or(WorkError::AssignmentNotFound(existing))?;
                if assignment.released_at.is_none() {
                    if assignment.agent_id == agent_id {
                        return Err(WorkError::InvalidGraph(
                            "agent already holds the live assignment".into(),
                        ));
                    }
                    assignment.released_at = Some(Utc::now());
                }
            }
            attempt_id = current_attempt_id;
            let attempt = g
                .attempts
                .get_mut(&current_attempt_id)
                .ok_or(WorkError::InvalidGraph("missing attempt".into()))?;
            attempt.agent_id = Some(agent_id.clone());
            attempt.assignment_id = Some(assignment_id);
            g.assignments.insert(
                assignment_id,
                WorkAssignment {
                    id: assignment_id,
                    node_id,
                    attempt_id: current_attempt_id,
                    agent_id: agent_id.clone(),
                    parent_agent_id: parent_agent_id.clone(),
                    assigned_at: Utc::now(),
                    released_at: None,
                },
            );
            Ok(())
        })?;
        // Drop the parent's live binding if it still points at this node.
        self.active_binding_by_agent
            .retain(|_, binding| !(binding.graph_id == graph && binding.node_id == node_id));
        self.active_binding_by_agent.insert(
            agent_id,
            AgentWorkBinding {
                graph_id: graph,
                node_id,
                attempt_id,
                assignment_id,
                task_id,
            },
        );
        Ok((attempt_id, assignment_id))
    }

    /// Settle an assignment exactly once. Ownership is checked against the
    /// persisted assignment, never against a caller supplied task id.
    pub fn settle_assignment(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        assignment_id: AssignmentId,
        status: ExecutionStatus,
        outcome: Option<Outcome>,
        summary: impl Into<String>,
        structured_output: Option<serde_json::Value>,
        artifacts: Vec<String>,
        evidence: Vec<String>,
        evidence_links: Vec<EvidenceLink>,
        changed_files: Vec<String>,
        verification: VerificationLevel,
    ) -> Result<ResultId, WorkError> {
        let result_id = ResultId::new();
        let op = self
            .graph(graph)
            .ok()
            .and_then(|g| g.assignments.get(&assignment_id))
            .map(|a| WorkOp::Node(a.node_id))
            .unwrap_or(WorkOp::Topology);
        self.mutate(graph, expected, auth, op, |g| {
            let assignment = g
                .assignments
                .get(&assignment_id)
                .ok_or(WorkError::AssignmentNotFound(assignment_id))?
                .clone();
            if assignment.agent_id != auth.agent_id {
                return Err(WorkError::AssignmentNotOwned);
            }
            if assignment.released_at.is_some() {
                return Err(WorkError::AssignmentSettled);
            }
            let attempt =
                g.attempts
                    .get_mut(&assignment.attempt_id)
                    .ok_or(WorkError::InvalidGraph(
                        "assignment references missing attempt".into(),
                    ))?;
            if attempt.result_id.is_some() || attempt.state != ExecutionStatus::Running {
                return Err(WorkError::AssignmentSettled);
            }
            g.nodes
                .get(&assignment.node_id)
                .ok_or(WorkError::NodeNotFound(assignment.node_id))?;
            attempt.state = status;
            attempt.finished_at = Some(Utc::now());
            attempt.result_id = Some(result_id);
            g.assignments
                .get_mut(&assignment_id)
                .expect("assignment was validated")
                .released_at = Some(Utc::now());
            release_claims_for_assignment(g, assignment_id, Utc::now());
            let node = g
                .nodes
                .get_mut(&assignment.node_id)
                .ok_or(WorkError::NodeNotFound(assignment.node_id))?;
            node.status = status;
            node.effective_outcome = outcome.clone();
            node.revision = node.revision.saturating_add(1);
            g.results.insert(
                result_id,
                NodeResult {
                    id: result_id,
                    node_id: assignment.node_id,
                    attempt_id: assignment.attempt_id,
                    execution_status: status,
                    outcome,
                    verification,
                    summary: summary.into(),
                    structured_output,
                    artifacts,
                    evidence,
                    evidence_links,
                    changed_files,
                    producer: Some(auth.agent_id.clone()),
                    created_at: Utc::now(),
                },
            );
            if let Some(parent) = &assignment.parent_agent_id {
                g.notifications.push(WorkNotification {
                    id: result_id,
                    parent_agent_id: parent.clone(),
                    child_agent_id: auth.agent_id.clone(),
                    assignment_id,
                    result_id,
                    message: format!(
                        "worker {} settled assignment {}: {}",
                        auth.agent_id, assignment_id, g.results[&result_id].summary
                    ),
                    created_at: Utc::now(),
                    delivered: false,
                });
            }
            Ok(())
        })?;
        self.active_binding_by_agent
            .retain(|_, binding| binding.assignment_id != assignment_id);
        Ok(result_id)
    }

    pub fn binding_for_agent(&self, agent_id: &str) -> Option<&AgentWorkBinding> {
        self.active_binding_by_agent.get(agent_id)
    }

    /// Reconcile process-local execution state after a restart. Runtime
    /// handles are not resurrected, so every running attempt is retained as
    /// immutable history but marked interrupted and can be explicitly retried.
    pub fn reconcile_interrupted(&mut self) -> bool {
        let mut changed = false;
        for graph in self.graphs.values_mut() {
            let mut graph_changed = false;
            let mut to_settle: Vec<(NodeId, AssignmentId, AttemptId)> = Vec::new();
            for attempt in graph.attempts.values_mut() {
                if attempt.state == ExecutionStatus::Running {
                    attempt.state = ExecutionStatus::Interrupted;
                    attempt.finished_at = Some(Utc::now());
                    changed = true;
                    graph_changed = true;
                    if attempt.result_id.is_none()
                        && let Some(assignment_id) = attempt.assignment_id
                    {
                        to_settle.push((attempt.node_id, assignment_id, attempt.id));
                    }
                }
            }
            for node in graph.nodes.values_mut() {
                if node.status == ExecutionStatus::Running {
                    node.status = ExecutionStatus::Interrupted;
                    node.effective_outcome = Some(Outcome::Interrupted);
                    node.revision = node.revision.saturating_add(1);
                    changed = true;
                    graph_changed = true;
                }
            }
            // Release open assignments left behind by interrupted attempts,
            // record an immutable Interrupted result envelope for each (so
            // there is durable provenance of why the attempt ended), and
            // notify the parent — mirroring `settle_assignment`, but as a
            // system-level reconciliation rather than a worker action.
            for (node_id, assignment_id, attempt_id) in to_settle {
                let result_id = ResultId::new();
                graph.results.insert(
                    result_id,
                    NodeResult {
                        id: result_id,
                        node_id,
                        attempt_id,
                        execution_status: ExecutionStatus::Interrupted,
                        outcome: Some(Outcome::Interrupted),
                        verification: VerificationLevel::None,
                        summary: "attempt interrupted by session restart".into(),
                        structured_output: None,
                        artifacts: Vec::new(),
                        evidence: Vec::new(),
                        evidence_links: Vec::new(),
                        changed_files: Vec::new(),
                        producer: None,
                        created_at: Utc::now(),
                    },
                );
                if let Some(attempt) = graph.attempts.get_mut(&attempt_id) {
                    attempt.result_id = Some(result_id);
                }
                if let Some(assignment) = graph.assignments.get_mut(&assignment_id) {
                    assignment.released_at = Some(Utc::now());
                    if let Some(parent) = assignment.parent_agent_id.clone() {
                        graph.notifications.push(WorkNotification {
                            id: result_id,
                            parent_agent_id: parent,
                            child_agent_id: assignment.agent_id.clone(),
                            assignment_id,
                            result_id,
                            message: format!(
                                "assignment {assignment_id} on node {node_id} was interrupted by a session restart"
                            ),
                            created_at: Utc::now(),
                            delivered: false,
                        });
                    }
                }
                release_claims_for_assignment(graph, assignment_id, Utc::now());
                graph_changed = true;
            }
            if graph_changed {
                graph.revision = graph.revision.saturating_add(1);
            }
        }
        if changed {
            self.revision = self.revision.saturating_add(1);
            self.active_binding_by_agent.clear();
        }
        changed
    }

    pub fn validate(&self) -> Result<(), WorkError> {
        for graph in self.graphs.values() {
            graph.validate()?;
        }
        for graph in self.active_graph_by_agent.values() {
            if !self.graphs.contains_key(graph) {
                return Err(WorkError::InvalidGraph(
                    "active graph references missing graph".into(),
                ));
            }
        }
        for binding in self.active_binding_by_agent.values() {
            let graph = self.graphs.get(&binding.graph_id).ok_or_else(|| {
                WorkError::InvalidGraph("active binding references missing graph".into())
            })?;
            if !graph.nodes.contains_key(&binding.node_id)
                || !graph.attempts.contains_key(&binding.attempt_id)
                || !graph.assignments.contains_key(&binding.assignment_id)
            {
                return Err(WorkError::InvalidGraph(
                    "active binding references missing node, attempt, or assignment".into(),
                ));
            }
        }
        Ok(())
    }

    pub fn create_graph(
        &mut self,
        graph: WorkGraph,
        expected_revision: Option<u64>,
    ) -> Result<GraphId, WorkError> {
        if let Some(expected) = expected_revision
            && expected != self.revision
        {
            return Err(WorkError::StaleRevision {
                expected,
                actual: self.revision,
            });
        }
        graph.validate()?;
        let id = graph.id;
        if self.graphs.contains_key(&id) {
            return Err(WorkError::InvalidGraph("duplicate graph id".into()));
        }
        self.graphs.insert(id, graph);
        self.revision = self.revision.saturating_add(1);
        Ok(id)
    }

    pub fn list_graphs(&self) -> impl Iterator<Item = &WorkGraph> {
        self.graphs.values()
    }
    pub fn graph(&self, id: GraphId) -> Result<&WorkGraph, WorkError> {
        self.graphs.get(&id).ok_or(WorkError::GraphNotFound(id))
    }
    pub fn view_graph(&self, id: GraphId) -> Result<&WorkGraph, WorkError> {
        self.graph(id)
    }
    pub fn graph_mut(&mut self, id: GraphId) -> Result<&mut WorkGraph, WorkError> {
        self.graphs.get_mut(&id).ok_or(WorkError::GraphNotFound(id))
    }

    pub fn set_active_graph(
        &mut self,
        agent: impl Into<String>,
        graph: GraphId,
    ) -> Result<(), WorkError> {
        self.graph(graph)?;
        self.active_graph_by_agent.insert(agent.into(), graph);
        self.revision = self.revision.saturating_add(1);
        Ok(())
    }

    /// Close or cancel a graph. Goes through authorize + check_revision
    /// like every other mutation, so non-owners cannot close others' graphs.
    pub fn close_graph(
        &mut self,
        id: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        status: GraphStatus,
    ) -> Result<(), WorkError> {
        self.mutate(id, expected, auth, WorkOp::Topology, |g| {
            g.status = status;
            Ok(())
        })
    }

    /// Per-operation, per-node authorization. Owners may perform any
    /// operation on the graph (topology or any node). An assignee may only
    /// act on the node they hold a live assignment for, never on siblings
    /// or on graph topology.
    fn authorize(
        graph: &WorkGraph,
        auth: &AuthorizationContext,
        op: WorkOp,
    ) -> Result<(), WorkError> {
        let is_owner =
            auth.can_manage || graph.owner_agent_id.as_deref() == Some(auth.agent_id.as_str());
        if is_owner {
            return Ok(());
        }
        match op {
            WorkOp::Node(node_id) => {
                if graph.assignments.values().any(|a| {
                    a.node_id == node_id
                        && a.agent_id == auth.agent_id
                        && a.released_at.is_none()
                        && auth.assignment_ids.contains(&a.id)
                }) {
                    return Ok(());
                }
            }
            WorkOp::Annotate => {
                if graph
                    .assignments
                    .values()
                    .any(|a| a.agent_id == auth.agent_id && a.released_at.is_none())
                {
                    return Ok(());
                }
            }
            WorkOp::Topology => {}
        }
        Err(WorkError::Unauthorized {
            agent: auth.agent_id.clone(),
        })
    }

    fn check_revision(graph: &WorkGraph, expected: u64) -> Result<(), WorkError> {
        if graph.revision != expected {
            Err(WorkError::StaleRevision {
                expected,
                actual: graph.revision,
            })
        } else {
            Ok(())
        }
    }

    fn mutate<F>(
        &mut self,
        id: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        op: WorkOp,
        operation: F,
    ) -> Result<(), WorkError>
    where
        F: FnOnce(&mut WorkGraph) -> Result<(), WorkError>,
    {
        let original = self.graph(id)?.clone();
        Self::authorize(&original, auth, op)?;
        Self::check_revision(&original, expected)?;
        let mut candidate = original;
        operation(&mut candidate)?;
        candidate.revision = expected.saturating_add(1);
        candidate.validate()?;
        self.graphs.insert(id, candidate);
        self.revision = self.revision.saturating_add(1);
        Ok(())
    }

    pub fn add_node(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        input: NodeInput,
    ) -> Result<NodeId, WorkError> {
        let node = WorkNode::new(input.key, input.title);
        let id = node.id;
        self.mutate(graph, expected, auth, WorkOp::Topology, |g| {
            if g.nodes.values().any(|n| n.key == node.key) {
                return Err(WorkError::DuplicateKey(node.key.clone()));
            }
            g.view_order.push(id);
            g.nodes.insert(id, node);
            Ok(())
        })?;
        Ok(id)
    }

    pub fn update_node(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node_id: NodeId,
        title: Option<String>,
        description: Option<Option<String>>,
    ) -> Result<(), WorkError> {
        self.mutate(graph, expected, auth, WorkOp::Topology, |g| {
            let node = g
                .nodes
                .get_mut(&node_id)
                .ok_or(WorkError::NodeNotFound(node_id))?;
            if let Some(title) = title {
                node.title = title;
            }
            if let Some(description) = description {
                node.description = description;
            }
            node.revision = node.revision.saturating_add(1);
            Ok(())
        })
    }

    pub fn add_edge(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        from: NodeId,
        to: NodeId,
        condition: EdgeCondition,
        required: bool,
        binding: Option<InputBinding>,
    ) -> Result<EdgeId, WorkError> {
        let edge_id = EdgeId::new();
        self.mutate(graph, expected, auth, WorkOp::Topology, |g| {
            if !g.nodes.contains_key(&from) {
                return Err(WorkError::NodeNotFound(from));
            }
            if !g.nodes.contains_key(&to) {
                return Err(WorkError::NodeNotFound(to));
            }
            if from == to {
                return Err(WorkError::InvalidGraph(
                    "an edge cannot point to itself".into(),
                ));
            }
            g.edges.insert(
                edge_id,
                WorkEdge {
                    id: edge_id,
                    from,
                    to,
                    condition,
                    required,
                    binding,
                },
            );
            Ok(())
        })?;
        Ok(edge_id)
    }

    pub fn move_node(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node: NodeId,
        before: Option<NodeId>,
    ) -> Result<(), WorkError> {
        self.mutate(graph, expected, auth, WorkOp::Topology, |g| {
            let old = g
                .view_order
                .iter()
                .position(|id| *id == node)
                .ok_or(WorkError::NodeNotFound(node))?;
            g.view_order.remove(old);
            let target = before
                .and_then(|id| g.view_order.iter().position(|candidate| *candidate == id))
                .unwrap_or(g.view_order.len());
            g.view_order.insert(target, node);
            Ok(())
        })
    }

    /// M4.7: remove a previously added edge (`disconnect`). Topology-scoped,
    /// so only the owner may perform it.
    pub fn remove_edge(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        edge_id: EdgeId,
    ) -> Result<(), WorkError> {
        self.mutate(graph, expected, auth, WorkOp::Topology, |g| {
            if g.edges.remove(&edge_id).is_none() {
                return Err(WorkError::InvalidGraph(format!(
                    "edge not found: {edge_id}"
                )));
            }
            Ok(())
        })
    }

    /// M4.7: configure a node's join policy, retry policy, and executor.
    /// Topology-scoped (changes what it means for the node to be "ready" or
    /// retryable, not a per-attempt action).
    pub fn configure_node(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node_id: NodeId,
        join: Option<Option<JoinPolicy>>,
        retry_policy: Option<RetryPolicy>,
        executor: Option<Executor>,
        verification: Option<VerificationLevel>,
        acceptance_criteria: Option<Vec<AcceptanceCriterion>>,
        review_policy: Option<ReviewPolicy>,
    ) -> Result<(), WorkError> {
        self.mutate(graph, expected, auth, WorkOp::Topology, |g| {
            let node = g
                .nodes
                .get_mut(&node_id)
                .ok_or(WorkError::NodeNotFound(node_id))?;
            if let Some(join) = join {
                node.join = join;
            }
            if let Some(retry_policy) = retry_policy {
                node.retry_policy = retry_policy;
            }
            if let Some(executor) = executor {
                node.executor = executor;
            }
            if let Some(verification) = verification {
                node.verification = verification;
            }
            if let Some(acceptance_criteria) = acceptance_criteria {
                node.acceptance_criteria = acceptance_criteria;
            }
            if let Some(review_policy) = review_policy {
                node.review_policy = review_policy;
            }
            node.revision = node.revision.saturating_add(1);
            Ok(())
        })
    }

    /// M5.1/M5.3 — append an immutable annotation to an existing result.
    /// Never mutates `NodeResult` itself; multiple, even conflicting,
    /// annotations from different agents are all preserved. Bumps the
    /// graph (and state) revision like any other mutation.
    pub fn annotate_result(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        result_id: ResultId,
        kind: AnnotationKind,
        text: impl Into<String>,
    ) -> Result<AnnotationId, WorkError> {
        let annotation_id = AnnotationId::new();
        let text = text.into();
        let annotator = auth.agent_id.clone();
        self.mutate(graph, expected, auth, WorkOp::Annotate, |g| {
            if !g.results.contains_key(&result_id) {
                return Err(WorkError::ResultNotFound(result_id));
            }
            g.annotations.insert(
                annotation_id,
                ResultAnnotation {
                    id: annotation_id,
                    result_id,
                    annotator_agent_id: annotator.clone(),
                    kind,
                    text: text.clone(),
                    created_at: Utc::now(),
                },
            );
            Ok(())
        })?;
        Ok(annotation_id)
    }

    pub fn start(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node_id: NodeId,
        agent_id: Option<String>,
    ) -> Result<AttemptId, WorkError> {
        let attempt_id = AttemptId::new();
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            let node = g
                .nodes
                .get_mut(&node_id)
                .ok_or(WorkError::NodeNotFound(node_id))?;
            if !matches!(
                node.status,
                ExecutionStatus::Pending | ExecutionStatus::Ready | ExecutionStatus::Failed
            ) {
                return Err(WorkError::InvalidTransition {
                    node: node_id,
                    from: node.status,
                    to: ExecutionStatus::Running,
                });
            }
            check_attempt_cap(node)?;
            let number = node.attempt_ids.len() as u32 + 1;
            node.status = ExecutionStatus::Running;
            node.effective_outcome = None;
            node.revision += 1;
            node.attempt_ids.push(attempt_id);
            let manifest = g.freeze_manifest(node_id);
            let manifest_id = manifest.id;
            g.manifests.insert(manifest_id, manifest);
            g.attempts.insert(
                attempt_id,
                NodeAttempt {
                    id: attempt_id,
                    node_id,
                    number,
                    state: ExecutionStatus::Running,
                    started_at: Some(Utc::now()),
                    finished_at: None,
                    agent_id,
                    assignment_id: None,
                    result_id: None,
                    input_manifest_id: Some(manifest_id),
                },
            );
            Ok(())
        })?;
        Ok(attempt_id)
    }

    fn finish(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node_id: NodeId,
        status: ExecutionStatus,
        outcome: Option<Outcome>,
        summary: String,
        evidence: Vec<String>,
        evidence_links: Vec<EvidenceLink>,
        verification: VerificationLevel,
    ) -> Result<ResultId, WorkError> {
        let result_id = ResultId::new();
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            let attempt_id = {
                let node = g
                    .nodes
                    .get(&node_id)
                    .ok_or(WorkError::NodeNotFound(node_id))?;
                if node.status != ExecutionStatus::Running {
                    return Err(WorkError::InvalidTransition {
                        node: node_id,
                        from: node.status,
                        to: status,
                    });
                }
                *node.attempt_ids.last().ok_or(WorkError::MissingResult)?
            };
            let attempt = g
                .attempts
                .get_mut(&attempt_id)
                .ok_or(WorkError::InvalidGraph("missing attempt".into()))?;
            attempt.state = status;
            attempt.finished_at = Some(Utc::now());
            attempt.result_id = Some(result_id);
            let producer = attempt.agent_id.clone();
            let node = g
                .nodes
                .get_mut(&node_id)
                .ok_or(WorkError::NodeNotFound(node_id))?;
            node.status = status;
            node.effective_outcome = outcome.clone();
            node.revision += 1;
            g.results.insert(
                result_id,
                NodeResult {
                    id: result_id,
                    node_id,
                    attempt_id,
                    execution_status: status,
                    outcome,
                    verification,
                    summary,
                    structured_output: None,
                    artifacts: Vec::new(),
                    evidence,
                    evidence_links,
                    changed_files: Vec::new(),
                    producer,
                    created_at: Utc::now(),
                },
            );
            Ok(())
        })?;
        Ok(result_id)
    }

    pub fn complete(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node: NodeId,
        summary: impl Into<String>,
        evidence: Vec<String>,
        evidence_links: Vec<EvidenceLink>,
        verification: VerificationLevel,
    ) -> Result<ResultId, WorkError> {
        self.finish(
            graph,
            expected,
            auth,
            node,
            ExecutionStatus::Succeeded,
            Some(Outcome::Success),
            summary.into(),
            evidence,
            evidence_links,
            verification,
        )
    }
    pub fn fail(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node: NodeId,
        outcome: Outcome,
        summary: impl Into<String>,
        evidence: Vec<String>,
        evidence_links: Vec<EvidenceLink>,
        verification: VerificationLevel,
    ) -> Result<ResultId, WorkError> {
        self.finish(
            graph,
            expected,
            auth,
            node,
            ExecutionStatus::Failed,
            Some(outcome),
            summary.into(),
            evidence,
            evidence_links,
            verification,
        )
    }

    pub fn block(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node: NodeId,
    ) -> Result<(), WorkError> {
        self.mutate(graph, expected, auth, WorkOp::Node(node), |g| {
            let n = g
                .nodes
                .get_mut(&node)
                .ok_or(WorkError::NodeNotFound(node))?;
            if !matches!(
                n.status,
                ExecutionStatus::Pending
                    | ExecutionStatus::Ready
                    | ExecutionStatus::Running
                    | ExecutionStatus::Failed
            ) {
                return Err(WorkError::InvalidTransition {
                    node,
                    from: n.status,
                    to: ExecutionStatus::Blocked,
                });
            }
            n.status = ExecutionStatus::Blocked;
            n.effective_outcome = Some(Outcome::Blocked);
            n.revision += 1;
            Ok(())
        })
    }
    pub fn unblock(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node: NodeId,
    ) -> Result<(), WorkError> {
        self.mutate(graph, expected, auth, WorkOp::Node(node), |g| {
            let n = g
                .nodes
                .get_mut(&node)
                .ok_or(WorkError::NodeNotFound(node))?;
            if n.status != ExecutionStatus::Blocked {
                return Err(WorkError::InvalidTransition {
                    node,
                    from: n.status,
                    to: ExecutionStatus::Pending,
                });
            }
            n.status = ExecutionStatus::Pending;
            n.effective_outcome = None;
            n.revision += 1;
            Ok(())
        })
    }

    /// Cancel a node. If it currently has a running attempt, the attempt is
    /// settled with `Outcome::Cancelled`, its assignment (if any) is
    /// released in the same graph revision, and the agent's active binding
    /// for that node is cleared afterward — mirroring `settle_assignment`
    /// so a cancelled node can never leave a stranded running attempt or an
    /// open assignment behind.
    pub fn cancel(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node: NodeId,
    ) -> Result<(), WorkError> {
        let result_id = ResultId::new();
        let mut settled = false;
        self.mutate(graph, expected, auth, WorkOp::Node(node), |g| {
            let status = g
                .nodes
                .get(&node)
                .ok_or(WorkError::NodeNotFound(node))?
                .status;
            if !matches!(
                status,
                ExecutionStatus::Pending
                    | ExecutionStatus::Ready
                    | ExecutionStatus::Blocked
                    | ExecutionStatus::Running
                    | ExecutionStatus::Failed
            ) {
                return Err(WorkError::InvalidTransition {
                    node,
                    from: status,
                    to: ExecutionStatus::Cancelled,
                });
            }
            if status == ExecutionStatus::Running {
                let attempt_id = *g.nodes[&node]
                    .attempt_ids
                    .last()
                    .ok_or(WorkError::MissingResult)?;
                let attempt = g
                    .attempts
                    .get_mut(&attempt_id)
                    .ok_or(WorkError::InvalidGraph("missing attempt".into()))?;
                attempt.state = ExecutionStatus::Cancelled;
                attempt.finished_at = Some(Utc::now());
                attempt.result_id = Some(result_id);
                let producer = attempt.agent_id.clone();
                let assignment_id = attempt.assignment_id;
                if let Some(assignment_id) = assignment_id {
                    if let Some(assignment) = g.assignments.get_mut(&assignment_id) {
                        assignment.released_at = Some(Utc::now());
                    }
                    release_claims_for_assignment(g, assignment_id, Utc::now());
                }
                g.results.insert(
                    result_id,
                    NodeResult {
                        id: result_id,
                        node_id: node,
                        attempt_id,
                        execution_status: ExecutionStatus::Cancelled,
                        outcome: Some(Outcome::Cancelled),
                        verification: VerificationLevel::None,
                        summary: "cancelled".into(),
                        structured_output: None,
                        artifacts: Vec::new(),
                        evidence: Vec::new(),
                        evidence_links: Vec::new(),
                        changed_files: Vec::new(),
                        producer,
                        created_at: Utc::now(),
                    },
                );
                settled = true;
            }
            let n = g
                .nodes
                .get_mut(&node)
                .ok_or(WorkError::NodeNotFound(node))?;
            n.status = ExecutionStatus::Cancelled;
            n.effective_outcome = Some(Outcome::Cancelled);
            n.revision += 1;
            Ok(())
        })?;
        if settled {
            self.active_binding_by_agent
                .retain(|_, binding| !(binding.graph_id == graph && binding.node_id == node));
        }
        Ok(())
    }

    pub fn retry(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        node_id: NodeId,
    ) -> Result<(), WorkError> {
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            let node = g
                .nodes
                .get_mut(&node_id)
                .ok_or(WorkError::NodeNotFound(node_id))?;
            if !matches!(
                node.status,
                ExecutionStatus::Failed | ExecutionStatus::Interrupted | ExecutionStatus::Cancelled
            ) {
                return Err(WorkError::RetryUnavailable);
            }
            check_attempt_cap(node)?;
            node.status = ExecutionStatus::Pending;
            node.effective_outcome = None;
            node.revision += 1;
            Ok(())
        })
    }

    /// Acquire an advisory file claim tied to a live assignment. Overlap
    /// with any other active claim's paths in the same graph is detected
    /// and surfaced via the returned overlap list — callers are expected to
    /// turn that into a work event (a warning), never a hard block: claims
    /// are advisory bookkeeping, not filesystem locks.
    pub fn acquire_claim(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        assignment_id: AssignmentId,
        paths: Vec<String>,
        expiry: Option<DateTime<Utc>>,
    ) -> Result<(String, Vec<String>), WorkError> {
        let claim_id = uuid::Uuid::new_v4().to_string();
        let normalized: Vec<String> = paths
            .iter()
            .map(|p| normalize_claim_path(p))
            .collect::<Result<_, _>>()?;
        if normalized.is_empty() {
            return Err(WorkError::InvalidGraph(
                "acquire_claim requires at least one path".into(),
            ));
        }
        let node_id = self
            .graph(graph)?
            .assignments
            .get(&assignment_id)
            .ok_or(WorkError::AssignmentNotFound(assignment_id))?
            .node_id;
        let mut overlaps: Vec<String> = Vec::new();
        let claim_id_for_closure = claim_id.clone();
        let normalized_for_closure = normalized.clone();
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            let assignment = g
                .assignments
                .get(&assignment_id)
                .ok_or(WorkError::AssignmentNotFound(assignment_id))?
                .clone();
            if assignment.agent_id != auth.agent_id {
                return Err(WorkError::AssignmentNotOwned);
            }
            if assignment.released_at.is_some() {
                return Err(WorkError::AssignmentSettled);
            }
            let now = Utc::now();
            for existing in g.claims.values() {
                if !existing.is_active(now) || existing.assignment_id == assignment_id {
                    continue;
                }
                if existing
                    .paths
                    .iter()
                    .any(|p| normalized_for_closure.contains(p))
                {
                    overlaps.push(existing.id.clone());
                }
            }
            g.claims.insert(
                claim_id_for_closure.clone(),
                FileClaim {
                    id: claim_id_for_closure,
                    agent_id: auth.agent_id.clone(),
                    assignment_id,
                    attempt_id: assignment.attempt_id,
                    paths: normalized_for_closure,
                    acquired_at: now,
                    expiry,
                    released_at: None,
                },
            );
            Ok(())
        })?;
        Ok((claim_id, overlaps))
    }

    /// Release a previously acquired claim. Advisory only: releasing early
    /// (e.g. because a file's work is done before the assignment settles)
    /// is allowed, as is the automatic release wired into every settlement
    /// path.
    pub fn release_claim(
        &mut self,
        graph: GraphId,
        expected: u64,
        auth: &AuthorizationContext,
        claim_id: &str,
    ) -> Result<(), WorkError> {
        let node_id = self
            .graph(graph)?
            .claims
            .get(claim_id)
            .ok_or_else(|| WorkError::InvalidGraph(format!("claim not found: {claim_id}")))
            .and_then(|claim| {
                self.graph(graph)?
                    .assignments
                    .get(&claim.assignment_id)
                    .map(|a| a.node_id)
                    .ok_or(WorkError::AssignmentNotFound(claim.assignment_id))
            })?;
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            let claim = g
                .claims
                .get_mut(claim_id)
                .ok_or_else(|| WorkError::InvalidGraph(format!("claim not found: {claim_id}")))?;
            if claim.agent_id != auth.agent_id {
                return Err(WorkError::AssignmentNotOwned);
            }
            claim.released_at = Some(Utc::now());
            Ok(())
        })
    }
}

/// M5.3 — derive a [`QualityDigest`] from canonical graph state. Never
/// persisted: recomputed on demand so there is exactly one source of truth
/// for quality state (the graph itself), never a second quality database.
pub fn quality_digest(graph: &WorkGraph) -> QualityDigest {
    let mut digest = QualityDigest {
        graph_id: graph.id,
        total_nodes: graph.nodes.len(),
        ..Default::default()
    };
    for node in graph.nodes.values() {
        match node.status {
            ExecutionStatus::Succeeded => digest.succeeded += 1,
            ExecutionStatus::Failed | ExecutionStatus::Interrupted => digest.failed += 1,
            ExecutionStatus::Blocked => digest.blocked += 1,
            ExecutionStatus::Running => digest.running += 1,
            _ => digest.pending += 1,
        }

        let latest_result = node
            .attempt_ids
            .last()
            .and_then(|attempt_id| graph.attempts.get(attempt_id))
            .and_then(|attempt| attempt.result_id)
            .and_then(|result_id| graph.results.get(&result_id));

        // Verification coverage: nodes with no required level are trivially
        // met; otherwise the latest result's achieved level must be at
        // least the required level.
        let required = node.verification;
        if required == VerificationLevel::None {
            digest.verification_met += 1;
        } else {
            let achieved = latest_result.map(|r| r.verification).unwrap_or_default();
            if achieved >= required {
                digest.verification_met += 1;
            } else {
                digest.verification_unmet += 1;
            }
        }

        // Acceptance criteria coverage: a node with no declared criteria is
        // not counted toward met/unmet (there is nothing to satisfy).
        if !node.acceptance_criteria.is_empty() {
            let referenced: BTreeSet<&str> = latest_result
                .map(|r| {
                    r.evidence_links
                        .iter()
                        .filter_map(|link| link.criterion_id.as_deref())
                        .collect()
                })
                .unwrap_or_default();
            let met = node
                .acceptance_criteria
                .iter()
                .all(|criterion| referenced.contains(criterion.id.as_str()));
            if met {
                digest.acceptance_met += 1;
            } else {
                digest.acceptance_unmet += 1;
            }
        }

        // Reviewer gates: a node that requires independent verification.
        let requires_independence = node.verification == VerificationLevel::IndependentlyVerified
            || node.review_policy.requires_independent_reviewer;
        if requires_independence {
            let satisfied = latest_result
                .map(|r| r.verification == VerificationLevel::IndependentlyVerified)
                .unwrap_or(false);
            if satisfied {
                digest.reviewer_gates_satisfied += 1;
            } else {
                digest.reviewer_gates_pending += 1;
            }
        }
    }
    digest.annotations_count = graph.annotations.len();
    digest
}

impl WorkGraph {
    pub fn validate(&self) -> Result<(), WorkError> {
        let ids: BTreeSet<_> = self.nodes.keys().copied().collect();
        if self.view_order.len() != ids.len()
            || self.view_order.iter().collect::<BTreeSet<_>>().len() != ids.len()
            || self.view_order.iter().any(|id| !ids.contains(id))
        {
            return Err(WorkError::InvalidGraph(
                "view_order must contain every node exactly once".into(),
            ));
        }
        let mut keys = BTreeSet::new();
        for node in self.nodes.values() {
            if node.key.trim().is_empty() || !keys.insert(&node.key) {
                return Err(WorkError::InvalidGraph(
                    "node keys must be non-empty and unique".into(),
                ));
            }
            // M4.1: `ExecutionStatus::Ready` is a derived-only, never
            // persisted value (see `work::readiness`). Any code path that
            // would set it on a node or attempt is a bug — reject it here
            // rather than let a "ready bit" masquerade as durable status.
            if node.status == ExecutionStatus::Ready {
                return Err(WorkError::InvalidGraph(
                    "ExecutionStatus::Ready must never be persisted on a node; readiness is derived".into(),
                ));
            }
            for attempt in &node.attempt_ids {
                if !self.attempts.contains_key(attempt) {
                    return Err(WorkError::InvalidGraph(
                        "node references missing attempt".into(),
                    ));
                }
            }
        }
        for attempt in self.attempts.values() {
            if attempt.state == ExecutionStatus::Ready {
                return Err(WorkError::InvalidGraph(
                    "ExecutionStatus::Ready must never be persisted on an attempt; readiness is derived".into(),
                ));
            }
        }
        for edge in self.edges.values() {
            if !ids.contains(&edge.from) || !ids.contains(&edge.to) || edge.from == edge.to {
                return Err(WorkError::InvalidGraph(
                    "edge references invalid or self node".into(),
                ));
            }
        }
        // M4.2: a managed graph with cycles would spin the scheduler
        // forever (a node can never settle because a predecessor,
        // transitively, depends on it). Advisory graphs never execute, so
        // cycles there are harmless and left alone.
        if self.mode == GraphMode::Managed && self.has_cycle() {
            return Err(WorkError::InvalidGraph(
                "managed graph edges contain a cycle".into(),
            ));
        }
        for attempt in self.attempts.values() {
            if !ids.contains(&attempt.node_id) {
                return Err(WorkError::InvalidGraph(
                    "attempt references missing node".into(),
                ));
            }
            if let Some(result) = attempt.result_id
                && !self.results.contains_key(&result)
            {
                return Err(WorkError::InvalidGraph(
                    "attempt references missing result".into(),
                ));
            }
        }
        for assignment in self.assignments.values() {
            if !self.attempts.contains_key(&assignment.attempt_id)
                || !ids.contains(&assignment.node_id)
            {
                return Err(WorkError::InvalidGraph(
                    "assignment references missing provenance".into(),
                ));
            }
        }
        for notification in &self.notifications {
            if !self.results.contains_key(&notification.result_id)
                || !self.assignments.contains_key(&notification.assignment_id)
            {
                return Err(WorkError::InvalidGraph(
                    "notification references missing result or assignment".into(),
                ));
            }
        }
        for result in self.results.values() {
            if !self.attempts.contains_key(&result.attempt_id) || !ids.contains(&result.node_id) {
                return Err(WorkError::InvalidGraph(
                    "result references missing provenance".into(),
                ));
            }
        }
        Ok(())
    }

    /// DFS-based cycle detection over the edge graph (`from -> to`).
    fn has_cycle(&self) -> bool {
        use std::collections::BTreeMap;
        #[derive(Clone, Copy, PartialEq, Eq)]
        enum Mark {
            Visiting,
            Done,
        }
        let mut adjacency: BTreeMap<NodeId, Vec<NodeId>> = BTreeMap::new();
        for edge in self.edges.values() {
            adjacency.entry(edge.from).or_default().push(edge.to);
        }
        fn visit(
            node: NodeId,
            adjacency: &BTreeMap<NodeId, Vec<NodeId>>,
            marks: &mut BTreeMap<NodeId, Mark>,
        ) -> bool {
            match marks.get(&node) {
                Some(Mark::Visiting) => return true,
                Some(Mark::Done) => return false,
                None => {}
            }
            marks.insert(node, Mark::Visiting);
            if let Some(next) = adjacency.get(&node) {
                for &n in next {
                    if visit(n, adjacency, marks) {
                        return true;
                    }
                }
            }
            marks.insert(node, Mark::Done);
            false
        }
        let mut marks = BTreeMap::new();
        for &id in self.nodes.keys() {
            if visit(id, &adjacency, &mut marks) {
                return true;
            }
        }
        false
    }

    /// M4.4: freeze an [`InputManifest`] for `node_id` at claim time,
    /// selecting exact predecessor result IDs per incoming edge binding.
    /// Only edges whose predecessor already produced a result contribute —
    /// a manifest is a point-in-time selection, not a promise of
    /// completeness; readiness (`work::readiness`) is what decides whether
    /// a node may be claimed at all. `graph_revision` is recorded so a
    /// running attempt can prove which revision it read from.
    pub fn freeze_manifest(&self, node_id: NodeId) -> InputManifest {
        let mut results = std::collections::BTreeMap::new();
        for edge in self.edges.values().filter(|e| e.to == node_id) {
            let Some(predecessor) = self.nodes.get(&edge.from) else {
                continue;
            };
            let Some(attempt_id) = predecessor.attempt_ids.last() else {
                continue;
            };
            let Some(attempt) = self.attempts.get(attempt_id) else {
                continue;
            };
            let Some(result_id) = attempt.result_id else {
                continue;
            };
            let alias = edge
                .binding
                .as_ref()
                .map(|b| b.alias.clone())
                .unwrap_or_else(|| predecessor.key.clone());
            results.insert(alias, result_id);
        }
        InputManifest {
            id: ManifestId::new(),
            node_id,
            graph_revision: self.revision,
            results,
            created_at: Utc::now(),
        }
    }
}
