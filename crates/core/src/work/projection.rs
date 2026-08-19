//! Pure projections for compact clients such as the five-line TUI checklist.

use super::ids::NodeId;
use super::model::{ExecutionStatus, WorkGraph, WorkNode};
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MiniRow {
    pub node_id: NodeId,
    pub title: String,
    pub status: ExecutionStatus,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct MiniProjection {
    pub rows: Vec<MiniRow>,
    pub overflow: usize,
    pub completed: usize,
}

impl MiniProjection {
    /// The returned projection is never more than five lines. When there is
    /// overflow, four rows and one summary line are used.
    pub fn from_graph(graph: &WorkGraph) -> Self {
        let ordered: Vec<&WorkNode> = graph
            .view_order
            .iter()
            .filter_map(|id| graph.nodes.get(id))
            .collect();
        let completed = ordered
            .iter()
            .filter(|n| {
                matches!(
                    n.status,
                    ExecutionStatus::Succeeded
                        | ExecutionStatus::Cancelled
                        | ExecutionStatus::Skipped
                )
            })
            .count();
        let attention =
            |n: &&WorkNode| matches!(n.status, ExecutionStatus::Failed | ExecutionStatus::Blocked);
        let running = |n: &&WorkNode| n.status == ExecutionStatus::Running;
        let ready = |n: &&WorkNode| n.status == ExecutionStatus::Ready;
        let mut chosen: Vec<&WorkNode> = Vec::new();
        let mut selected = BTreeSet::new();
        let mut append = |predicate: &dyn Fn(&&WorkNode) -> bool| {
            for node in &ordered {
                if predicate(node) && selected.insert(node.id) {
                    chosen.push(*node);
                }
            }
        };
        append(&attention);
        append(&running);
        append(&ready);
        append(&|n| n.status == ExecutionStatus::Pending);
        append(&|_| true);
        let limit = if ordered.len() > 5 { 4 } else { 5 };
        let overflow = chosen.len().saturating_sub(limit);
        chosen.truncate(limit);
        // Preserve authored order after selecting by priority.
        chosen.sort_by_key(|node| {
            graph
                .view_order
                .iter()
                .position(|id| *id == node.id)
                .unwrap_or(usize::MAX)
        });
        Self {
            rows: chosen
                .into_iter()
                .map(|n| MiniRow {
                    node_id: n.id,
                    title: n.title.clone(),
                    status: n.status,
                })
                .collect(),
            overflow,
            completed,
        }
    }
}
