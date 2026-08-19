//! Atomic, revisioned WorkGraph mutations and invariant validation.

use super::ids::*;
use super::model::*;
use chrono::Utc;
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
}

/// The scope of a graph mutation, used to decide whether a non-owner
/// assignee may perform it. `Topology` covers structural graph changes
/// (add/remove/move nodes, edges) that only the owner may perform.
/// `Node` covers per-node attempt/result/status changes, which an assignee
/// may perform only for the node they hold a live assignment on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WorkOp {
    Topology,
    Node(NodeId),
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
                    input_manifest_id: None,
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
            let verification = g
                .nodes
                .get(&assignment.node_id)
                .ok_or(WorkError::NodeNotFound(assignment.node_id))?
                .verification;
            attempt.state = status;
            attempt.finished_at = Some(Utc::now());
            attempt.result_id = Some(result_id);
            g.assignments
                .get_mut(&assignment_id)
                .expect("assignment was validated")
                .released_at = Some(Utc::now());
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
                    artifacts: Vec::new(),
                    evidence: Vec::new(),
                    changed_files: Vec::new(),
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
            for attempt in graph.attempts.values_mut() {
                if attempt.state == ExecutionStatus::Running {
                    attempt.state = ExecutionStatus::Interrupted;
                    attempt.finished_at = Some(Utc::now());
                    changed = true;
                    graph_changed = true;
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
        if let WorkOp::Node(node_id) = op
            && graph.assignments.values().any(|a| {
                a.node_id == node_id
                    && a.agent_id == auth.agent_id
                    && a.released_at.is_none()
                    && auth.assignment_ids.contains(&a.id)
            })
        {
            return Ok(());
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
                    input_manifest_id: None,
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
    ) -> Result<ResultId, WorkError> {
        let result_id = ResultId::new();
        self.mutate(graph, expected, auth, WorkOp::Node(node_id), |g| {
            let (attempt_id, verification) = {
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
                (
                    *node.attempt_ids.last().ok_or(WorkError::MissingResult)?,
                    node.verification,
                )
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
                let verification = g.nodes[&node].verification;
                let attempt = g
                    .attempts
                    .get_mut(&attempt_id)
                    .ok_or(WorkError::InvalidGraph("missing attempt".into()))?;
                attempt.state = ExecutionStatus::Cancelled;
                attempt.finished_at = Some(Utc::now());
                attempt.result_id = Some(result_id);
                let producer = attempt.agent_id.clone();
                let assignment_id = attempt.assignment_id;
                if let Some(assignment_id) = assignment_id
                    && let Some(assignment) = g.assignments.get_mut(&assignment_id)
                {
                    assignment.released_at = Some(Utc::now());
                }
                g.results.insert(
                    result_id,
                    NodeResult {
                        id: result_id,
                        node_id: node,
                        attempt_id,
                        execution_status: ExecutionStatus::Cancelled,
                        outcome: Some(Outcome::Cancelled),
                        verification,
                        summary: "cancelled".into(),
                        structured_output: None,
                        artifacts: Vec::new(),
                        evidence: Vec::new(),
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
            for attempt in &node.attempt_ids {
                if !self.attempts.contains_key(attempt) {
                    return Err(WorkError::InvalidGraph(
                        "node references missing attempt".into(),
                    ));
                }
            }
        }
        for edge in self.edges.values() {
            if !ids.contains(&edge.from) || !ids.contains(&edge.to) || edge.from == edge.to {
                return Err(WorkError::InvalidGraph(
                    "edge references invalid or self node".into(),
                ));
            }
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
}
