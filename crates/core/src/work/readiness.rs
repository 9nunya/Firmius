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
    let ok = match edge.condition {
        EdgeCondition::Completed => true,
        EdgeCondition::Succeeded => predecessor.status == ExecutionStatus::Succeeded,
        EdgeCondition::Failed => predecessor.status == ExecutionStatus::Failed,
        EdgeCondition::Blocked => predecessor.status == ExecutionStatus::Blocked,
        EdgeCondition::Outcome => predecessor.effective_outcome.is_some(),
        EdgeCondition::Verification => predecessor.verification != VerificationLevel::None,
    };
    Some(ok)
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
    let mut incoming: BTreeMap<NodeId, Vec<&WorkEdge>> = BTreeMap::new();
    for edge in graph.edges.values() {
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
            condition,
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
}
