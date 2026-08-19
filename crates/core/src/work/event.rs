//! Typed work events and authoritative snapshot/projection envelopes.

use super::ids::*;
use super::model::{NodeAttempt, NodeResult, WorkGraph, WorkNode, WorkState};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum WorkEvent {
    GraphCreated {
        graph: WorkGraph,
    },
    GraphChanged {
        graph_id: GraphId,
        revision: u64,
    },
    NodeChanged {
        graph_id: GraphId,
        node: WorkNode,
    },
    AttemptChanged {
        graph_id: GraphId,
        attempt: NodeAttempt,
    },
    ResultRecorded {
        graph_id: GraphId,
        result: NodeResult,
    },
    ActiveGraphChanged {
        agent_id: String,
        graph_id: GraphId,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkEventEnvelope {
    pub session_id: String,
    pub sequence: u64,
    pub at: DateTime<Utc>,
    pub event: WorkEvent,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkSnapshot {
    pub session_id: String,
    pub sequence: u64,
    pub state: WorkState,
}

impl WorkSnapshot {
    pub fn new(session_id: impl Into<String>, sequence: u64, state: WorkState) -> Self {
        Self {
            session_id: session_id.into(),
            sequence,
            state,
        }
    }
    pub fn graph(&self, id: GraphId) -> Option<&WorkGraph> {
        self.state.graphs.get(&id)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkProjection {
    pub graph_id: GraphId,
    pub graph_revision: u64,
    pub mini: super::projection::MiniProjection,
}
