//! Pure readiness derivation for managed `WorkGraph`s.
//!
//! `ExecutionStatus::Ready` is never persisted (see [`WorkGraph::validate`]);
//! readiness is instead a pure function of edges, join policies, and
//! predecessor results, recomputed on demand. This module has no session,
//! persistence, or scheduling dependency — it only reads a `&WorkGraph` and
//! reports which pending nodes could be claimed right now, and which are
//! structurally blocked.

use super::ids::NodeId;
use super::model::*;
use std::collections::BTreeMap;

/// The result of evaluating one graph's readiness. Never mutates the graph.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ReadinessReport {
    /// Pending nodes whose join policy is currently satisfied and which
    /// could be claimed by a scheduler.
    pub ready: Vec<NodeId>,
    /// Pending nodes that can never become ready given the current
    /// (permanently settled) predecessor state, with a human-readable
    /// reason. These should transition to `Blocked` rather than wait
    /// forever.
    pub blocked: Vec<(NodeId, String)>,
}

enum NodeReadiness {
    Ready,
    Waiting,
    Blocked(String),
}

/// True once a node's execution has permanently settled — no further
/// attempt can change its `effective_outcome` without an explicit `retry`.
fn is_settled(status: ExecutionStatus) -> bool {
    matches!(
        status,
        ExecutionStatus::Succeeded
            | ExecutionStatus::Failed
            | ExecutionStatus::Cancelled
            | ExecutionStatus::Skipped
            | ExecutionStatus::Interrupted
            | ExecutionStatus::Blocked
    )
}

/// Evaluate one edge against its predecessor's current state.
/// `None` — predecessor has not settled yet (still pending/running).
/// `Some(true)` — the edge's condition is satisfied.
/// `Some(false)` — the predecessor settled, but the condition was not met
/// (e.g. a `Succeeded` edge whose predecessor failed).
fn edge_satisfied(graph: &WorkGraph, edge: &WorkEdge) -> Option<bool> {
    let predecessor = graph.nodes.get(&edge.from)?;
    if !is_settled(predecessor.status) {
        return None;
    }
    Some(condition_holds(edge, predecessor))
}

/// Whether `edge`'s condition holds for its already-settled predecessor.
///
/// Shared by readiness and by feedback-edge firing so a condition can never
/// mean one thing when gating work and another when bouncing it back.
pub(crate) fn condition_holds(edge: &WorkEdge, predecessor: &WorkNode) -> bool {
    match edge.condition {
        EdgeCondition::Completed => true,
        EdgeCondition::Succeeded => predecessor.status == ExecutionStatus::Succeeded,
        EdgeCondition::Failed => predecessor.status == ExecutionStatus::Failed,
        EdgeCondition::Blocked => predecessor.status == ExecutionStatus::Blocked,
        // With `on_outcome` set, match that exact outcome; otherwise any
        // outcome at all satisfies the edge.
        EdgeCondition::Outcome => match (&edge.on_outcome, &predecessor.effective_outcome) {
            (Some(expected), Some(actual)) => expected == actual,
            (None, actual) => actual.is_some(),
            (Some(_), None) => false,
        },
        EdgeCondition::Verification => predecessor.verification != VerificationLevel::None,
    }
}

fn evaluate_node(graph: &WorkGraph, node: &WorkNode, edges: &[&WorkEdge]) -> NodeReadiness {
    let join = node.join.unwrap_or(JoinPolicy::AllSucceeded);

    let mut required_pending = false;
    let mut succeeded: u32 = 0;
    let mut settled: u32 = 0;
    let mut all_required_ok = true;

    for edge in edges {
        match edge_satisfied(graph, edge) {
            None => {
                if edge.required {
                    required_pending = true;
                    all_required_ok = false;
                }
                // Optional edges: skip-if-not-settled — they simply don't
                // count toward the join yet.
            }
            Some(ok) => {
                settled += 1;
                if ok {
                    succeeded += 1;
                } else if edge.required {
                    all_required_ok = false;
                }
            }
        }
    }

    match join {
        JoinPolicy::AllSucceeded => {
            if required_pending {
                NodeReadiness::Waiting
            } else if all_required_ok {
                NodeReadiness::Ready
            } else {
                NodeReadiness::Blocked("a required predecessor did not succeed".into())
            }
        }
        JoinPolicy::AllSettled => {
            if required_pending {
                NodeReadiness::Waiting
            } else {
                NodeReadiness::Ready
            }
        }
        JoinPolicy::AnySucceeded => {
            if succeeded > 0 {
                // At most one successor attempt: once the node itself is
                // claimed it leaves `Pending`, so a later evaluation will
                // no longer see it here — readiness is naturally
                // idempotent, the scheduler enforces at-most-once claiming
                // via the durable revisioned transaction.
                NodeReadiness::Ready
            } else if required_pending {
                NodeReadiness::Waiting
            } else {
                NodeReadiness::Blocked("no predecessor succeeded".into())
            }
        }
        JoinPolicy::MinimumSucceeded(minimum) => {
            if succeeded >= minimum {
                NodeReadiness::Ready
            } else if required_pending {
                NodeReadiness::Waiting
            } else {
                NodeReadiness::Blocked(format!(
                    "minimum {minimum} successes unreachable ({succeeded} succeeded of {settled} settled)"
                ))
            }
        }
        JoinPolicy::Quorum { required, total } => {
            if succeeded >= required {
                return NodeReadiness::Ready;
            }
            // Total predecessors still outstanding (never settled, or
            // still pending): if even every one of them succeeded, would
            // the quorum still be unreachable?
            let outstanding = total.saturating_sub(settled);
            if succeeded.saturating_add(outstanding) < required {
                NodeReadiness::Blocked(format!(
                    "quorum {required}/{total} is no longer reachable ({succeeded} succeeded, {settled} settled)"
                ))
            } else {
                NodeReadiness::Waiting
            }
        }
    }
}

/// Derive readiness for every pending node in `graph`. Pure: never mutates
/// `graph`. Nodes with `Executor::Manual` are still reported ready (a human
/// or the owning agent claims them via `task start`); the scheduler decides
/// which executors it drives automatically.
pub fn evaluate_readiness(graph: &WorkGraph) -> ReadinessReport {
    // Feedback edges are not dependencies: they never gate readiness, they
    // only re-open a node after some other node settles. Including them
    // here would deadlock every loop, since the target would wait on a
    // predecessor that only runs after the target itself.
    let mut incoming: BTreeMap<NodeId, Vec<&WorkEdge>> = BTreeMap::new();
    for edge in graph.edges.values() {
        if edge.kind == EdgeKind::Feedback {
            continue;
        }
        incoming.entry(edge.to).or_default().push(edge);
    }

    let mut ready = Vec::new();
    let mut blocked = Vec::new();
    for node in graph.nodes.values() {
        if node.status != ExecutionStatus::Pending {
            continue;
        }
        match incoming.get(&node.id) {
            None => ready.push(node.id),
            Some(edges) => match evaluate_node(graph, node, edges) {
                NodeReadiness::Ready => ready.push(node.id),
                NodeReadiness::Waiting => {}
                NodeReadiness::Blocked(reason) => blocked.push((node.id, reason)),
            },
        }
    }
    ReadinessReport { ready, blocked }
}

/// M5.2 — true if `candidate_agent_id` may be assigned to `node_id` under
/// its reviewer-independence gate. A node requires independence when its
/// required verification level is `IndependentlyVerified`, or its
/// `review_policy.requires_independent_reviewer` is set; in that case the
/// candidate must not be the producer of any bound predecessor result (the
/// result this node's attempt would be reviewing).
pub fn is_independent_reviewer(
    graph: &WorkGraph,
    node_id: NodeId,
    candidate_agent_id: &str,
) -> bool {
    let Some(node) = graph.nodes.get(&node_id) else {
        return true;
    };
    let requires_independence = node.verification == VerificationLevel::IndependentlyVerified
        || node.review_policy.requires_independent_reviewer;
    if !requires_independence {
        return true;
    }
    for edge in graph.edges.values().filter(|e| e.to == node_id) {
        let Some(predecessor) = graph.nodes.get(&edge.from) else {
            continue;
        };
        let Some(attempt_id) = predecessor.attempt_ids.last() else {
            continue;
        };
        let Some(attempt) = graph.attempts.get(attempt_id) else {
            continue;
        };
        let Some(result_id) = attempt.result_id else {
            continue;
        };
        let Some(result) = graph.results.get(&result_id) else {
            continue;
        };
        if result.producer.as_deref() == Some(candidate_agent_id) {
            return false;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::work::ids::EdgeId;
    use crate::work::model::GraphMode;

    fn node(key: &str) -> WorkNode {
        WorkNode::new(key, key)
    }

    fn edge(from: NodeId, to: NodeId, condition: EdgeCondition, required: bool) -> WorkEdge {
        WorkEdge {
            id: super::super::ids::EdgeId::new(),
            from,
            to,
            kind: EdgeKind::Dependency,
            condition,
            on_outcome: None,
            required,
            binding: None,
        }
    }

    fn base_graph() -> WorkGraph {
        WorkGraph::new("g", None, GraphMode::Managed)
    }

    #[test]
    fn a_node_with_no_predecessors_is_ready() {
        let mut g = base_graph();
        let a = node("a");
        let id = a.id;
        g.view_order.push(id);
        g.nodes.insert(id, a);
        let report = evaluate_readiness(&g);
        assert_eq!(report.ready, vec![id]);
        assert!(report.blocked.is_empty());
    }

    #[test]
    fn all_succeeded_waits_then_becomes_ready() {
        let mut g = base_graph();
        let mut a = node("a");
        a.status = ExecutionStatus::Succeeded;
        let b = node("b");
        let (a_id, b_id) = (a.id, b.id);
        g.view_order.extend([a_id, b_id]);
        g.nodes.insert(a_id, a);
        g.nodes.insert(b_id, b);
        let e = edge(a_id, b_id, EdgeCondition::Succeeded, true);
        g.edges.insert(e.id, e);
        let report = evaluate_readiness(&g);
        assert_eq!(report.ready, vec![b_id]);
    }

    #[test]
    fn all_succeeded_blocks_when_predecessor_failed() {
        let mut g = base_graph();
        let mut a = node("a");
        a.status = ExecutionStatus::Failed;
        let b = node("b");
        let (a_id, b_id) = (a.id, b.id);
        g.view_order.extend([a_id, b_id]);
        g.nodes.insert(a_id, a);
        g.nodes.insert(b_id, b);
        let e = edge(a_id, b_id, EdgeCondition::Succeeded, true);
        g.edges.insert(e.id, e);
        let report = evaluate_readiness(&g);
        assert!(report.ready.is_empty());
        assert_eq!(report.blocked[0].0, b_id);
    }

    #[test]
    fn optional_edge_does_not_block_readiness_when_unsettled() {
        let mut g = base_graph();
        let a = node("a"); // still pending
        let b = node("b");
        let (a_id, b_id) = (a.id, b.id);
        g.view_order.extend([a_id, b_id]);
        g.nodes.insert(a_id, a);
        g.nodes.insert(b_id, b);
        let e = edge(a_id, b_id, EdgeCondition::Completed, false);
        g.edges.insert(e.id, e);
        let report = evaluate_readiness(&g);
        // `a` has no predecessors of its own, so it is ready too; `b`'s
        // only edge is optional and its predecessor hasn't settled, which
        // must not block `b`.
        let mut ready = report.ready.clone();
        ready.sort();
        let mut expected = vec![a_id, b_id];
        expected.sort();
        assert_eq!(ready, expected);
    }

    #[test]
    fn any_succeeded_ready_after_one_success() {
        let mut g = base_graph();
        let mut a = node("a");
        a.status = ExecutionStatus::Succeeded;
        let mut b = node("b");
        b.status = ExecutionStatus::Failed;
        let mut c = node("c");
        c.join = Some(JoinPolicy::AnySucceeded);
        let (a_id, b_id, c_id) = (a.id, b.id, c.id);
        g.view_order.extend([a_id, b_id, c_id]);
        g.nodes.insert(a_id, a);
        g.nodes.insert(b_id, b);
        g.nodes.insert(c_id, c);
        g.edges.insert(
            EdgeId::new(),
            edge(a_id, c_id, EdgeCondition::Succeeded, true),
        );
        g.edges.insert(
            EdgeId::new(),
            edge(b_id, c_id, EdgeCondition::Succeeded, true),
        );
        let report = evaluate_readiness(&g);
        assert_eq!(report.ready, vec![c_id]);
    }

    #[test]
    fn quorum_blocks_when_impossible_and_waits_otherwise() {
        let mut g = base_graph();
        let mut a = node("a");
        a.status = ExecutionStatus::Failed;
        let mut b = node("b");
        b.status = ExecutionStatus::Failed;
        let c = node("c"); // still pending
        let mut d = node("d");
        d.join = Some(JoinPolicy::Quorum {
            required: 2,
            total: 3,
        });
        let (a_id, b_id, c_id, d_id) = (a.id, b.id, c.id, d.id);
        g.view_order.extend([a_id, b_id, c_id, d_id]);
        g.nodes.insert(a_id, a);
        g.nodes.insert(b_id, b);
        g.nodes.insert(c_id, c);
        g.nodes.insert(d_id, d);
        for from in [a_id, b_id, c_id] {
            g.edges.insert(
                EdgeId::new(),
                edge(from, d_id, EdgeCondition::Succeeded, true),
            );
        }
        let report = evaluate_readiness(&g);
        // `c` itself has no predecessors and is still pending, so it is
        // ready; only `d`'s quorum is unreachable.
        assert_eq!(report.ready, vec![c_id]);
        assert_eq!(report.blocked[0].0, d_id);

        let mut g2 = base_graph();
        let mut a2 = node("a");
        a2.status = ExecutionStatus::Failed;
        let b2 = node("b");
        let c2 = node("c");
        let mut d2 = node("d");
        d2.join = Some(JoinPolicy::Quorum {
            required: 2,
            total: 3,
        });
        let (a2_id, b2_id, c2_id, d2_id) = (a2.id, b2.id, c2.id, d2.id);
        g2.view_order.extend([a2_id, b2_id, c2_id, d2_id]);
        g2.nodes.insert(a2_id, a2);
        g2.nodes.insert(b2_id, b2);
        g2.nodes.insert(c2_id, c2);
        g2.nodes.insert(d2_id, d2);
        for from in [a2_id, b2_id, c2_id] {
            g2.edges.insert(
                EdgeId::new(),
                edge(from, d2_id, EdgeCondition::Succeeded, true),
            );
        }
        let report = evaluate_readiness(&g2);
        let mut ready = report.ready.clone();
        ready.sort();
        let mut expected = vec![b2_id, c2_id];
        expected.sort();
        assert_eq!(ready, expected);
        assert!(report.blocked.is_empty());
    }

    #[test]
    fn minimum_succeeded_is_ready_once_threshold_met() {
        let mut g = base_graph();
        let mut a = node("a");
        a.status = ExecutionStatus::Succeeded;
        let mut b = node("b");
        b.status = ExecutionStatus::Succeeded;
        let mut c = node("c");
        c.join = Some(JoinPolicy::MinimumSucceeded(2));
        let (a_id, b_id, c_id) = (a.id, b.id, c.id);
        g.view_order.extend([a_id, b_id, c_id]);
        g.nodes.insert(a_id, a);
        g.nodes.insert(b_id, b);
        g.nodes.insert(c_id, c);
        for from in [a_id, b_id] {
            g.edges.insert(
                EdgeId::new(),
                edge(from, c_id, EdgeCondition::Succeeded, true),
            );
        }
        let report = evaluate_readiness(&g);
        assert_eq!(report.ready, vec![c_id]);
    }

    #[test]
    fn independent_reviewer_gate_rejects_the_producer_and_allows_others() {
        let mut g = base_graph();
        let mut producer_node = node("a");
        producer_node
            .attempt_ids
            .push(super::super::ids::AttemptId::new());
        let attempt_id = producer_node.attempt_ids[0];
        let mut reviewer_node = node("b");
        reviewer_node.verification = VerificationLevel::IndependentlyVerified;
        let (a_id, b_id) = (producer_node.id, reviewer_node.id);
        g.view_order.extend([a_id, b_id]);
        let result_id = super::super::ids::ResultId::new();
        g.results.insert(
            result_id,
            NodeResult {
                id: result_id,
                node_id: a_id,
                attempt_id,
                execution_status: ExecutionStatus::Succeeded,
                outcome: Some(Outcome::Success),
                verification: VerificationLevel::None,
                summary: "done".into(),
                structured_output: None,
                artifacts: Vec::new(),
                evidence: Vec::new(),
                evidence_links: Vec::new(),
                changed_files: Vec::new(),
                producer: Some("producer".into()),
                created_at: chrono::Utc::now(),
            },
        );
        g.attempts.insert(
            attempt_id,
            NodeAttempt {
                id: attempt_id,
                node_id: a_id,
                number: 1,
                state: ExecutionStatus::Succeeded,
                started_at: None,
                finished_at: None,
                agent_id: Some("producer".into()),
                assignment_id: None,
                result_id: Some(result_id),
                input_manifest_id: None,
            },
        );
        g.nodes.insert(a_id, producer_node);
        g.nodes.insert(b_id, reviewer_node);
        g.edges.insert(
            EdgeId::new(),
            edge(a_id, b_id, EdgeCondition::Succeeded, true),
        );
        assert!(!is_independent_reviewer(&g, b_id, "producer"));
        assert!(is_independent_reviewer(&g, b_id, "reviewer"));
        // No independence requirement: anyone may be assigned, including
        // the producer.
        assert!(is_independent_reviewer(&g, a_id, "producer"));
    }
}
