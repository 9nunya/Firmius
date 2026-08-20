//! Typed work events and authoritative snapshot/projection envelopes.

use super::ids::*;
use super::model::{FileChange, NodeAttempt, NodeResult, WorkGraph, WorkNode, WorkState};
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
    /// A managed run started driving `graph_id`.
    ///
    /// Node transitions are already streamed by the events above, so this
    /// exists purely so an observer knows a run is in flight at all: that
    /// is what lets a UI expand into a live view when one begins and fall
    /// back to a compact checklist when none is running.
    RunStarted {
        run_id: String,
        graph_id: GraphId,
    },
    RunConcluded {
        run_id: String,
        graph_id: GraphId,
        /// `settled`, `stalled`, or `cancelled`.
        conclusion: String,
    },
    /// Emitted by the built-in `edit` tool after a successful operation,
    /// carrying the exact changed paths. Only emitted when the calling
    /// agent holds an active work binding, so overlapping peers can be
    /// notified. Never derived from `bash` output.
    FilesChanged {
        graph_id: GraphId,
        agent_id: String,
        changes: Vec<FileChange>,
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
    /// Watermark for this work snapshot specifically — the bus sequence at
    /// which `state` is known to be current. Distinct from any bus-wide
    /// contiguity watermark a consumer may track separately (e.g. the TUI's
    /// `session_event_sequence`), which advances on every observed event,
    /// not just work events.
    pub work_sequence: u64,
    pub state: WorkState,
}

impl WorkSnapshot {
    pub fn new(session_id: impl Into<String>, work_sequence: u64, state: WorkState) -> Self {
        Self {
            session_id: session_id.into(),
            work_sequence,
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
