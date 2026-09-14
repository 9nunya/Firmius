//! Versioned, data-only boundary shared by the Firmius daemon and clients.
//!
//! The protocol deliberately contains no live `Agent`, `Session`, provider,
//! process, or MCP handles. Those resources remain exclusively daemon-owned.

use std::collections::HashMap;

use firmius_core::{
    AccountRecord, AgentRecord, EffortMode, FirmiusConfig, McpServerConfig, Message,
    PendingPermissionRequest, PermissionDecision, ProcInfo, ProcStatus, SessionEvent,
    SessionRecord, SessionSummary, Usage, UserSettings, WorkSnapshot,
};
pub use firmius_core::{PermissionMode, PermissionPolicy};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use uuid::Uuid;

mod goal;
pub use goal::*;

/// Wire compatibility version. Version 2 adds native memory/todo enum
/// variants, so version 1 peers must fail the handshake rather than attempt
/// to deserialize an incompatible message.
pub const PROTOCOL_VERSION: u32 = 2;
pub const MEMORY_DTO_VERSION: u32 = 1;
pub const TODO_DTO_VERSION: u32 = 1;
/// Maximum serialized message size. Sessions can contain large snapshots and
/// transcripts; keep a generous cap while still bounding allocations from a
/// hostile peer (the wire length is a u32).
pub const MAX_FRAME_BYTES: usize = 128 * 1024 * 1024;
/// Logical messages may span a bounded sequence of frames.  This keeps the
/// legacy frame limit useful for allocation safety while allowing large
/// snapshots/images to cross the daemon boundary.
pub const MAX_MESSAGE_BYTES: usize = 512 * 1024 * 1024;
pub const CHUNK_DATA_BYTES: usize = 4 * 1024 * 1024;
pub const MAX_CHUNKS: usize = MAX_MESSAGE_BYTES.div_ceil(CHUNK_DATA_BYTES);

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChunkFrame {
    #[serde(rename = "_firmius_chunk")]
    pub marker: bool,
    pub transfer_id: Uuid,
    pub index: u32,
    pub total: u32,
    pub aggregate_len: u64,
    pub data: Vec<u8>,
}

pub fn encode_frames<T: Serialize>(value: &T) -> Result<Vec<Vec<u8>>, ProtocolError> {
    let payload = serde_json::to_vec(value)
        .map_err(|error| ProtocolError::new(ErrorCode::InvalidRequest, error.to_string()))?;
    if payload.len() <= MAX_FRAME_BYTES {
        return Ok(vec![encode_frame_bytes(&payload)]);
    }
    if payload.len() > MAX_MESSAGE_BYTES {
        return Err(ProtocolError::new(
            ErrorCode::InvalidRequest,
            format!("logical message exceeds {MAX_MESSAGE_BYTES} bytes"),
        ));
    }
    let total = payload.len().div_ceil(CHUNK_DATA_BYTES);
    if total > MAX_CHUNKS || total > u32::MAX as usize {
        return Err(ProtocolError::new(
            ErrorCode::InvalidRequest,
            "too many message chunks",
        ));
    }
    let transfer_id = Uuid::new_v4();
    payload
        .chunks(CHUNK_DATA_BYTES)
        .enumerate()
        .map(|(index, data)| {
            let chunk = ChunkFrame {
                marker: true,
                transfer_id,
                index: index as u32,
                total: total as u32,
                aggregate_len: payload.len() as u64,
                data: data.to_vec(),
            };
            let bytes = serde_json::to_vec(&chunk).map_err(|error| {
                ProtocolError::new(ErrorCode::InvalidRequest, error.to_string())
            })?;
            if bytes.len() > MAX_FRAME_BYTES {
                return Err(ProtocolError::new(
                    ErrorCode::InvalidRequest,
                    "chunk envelope exceeds frame limit",
                ));
            }
            Ok(encode_frame_bytes(&bytes))
        })
        .collect()
}

fn encode_frame_bytes(payload: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(4 + payload.len());
    frame.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    frame.extend_from_slice(payload);
    frame
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RequestEnvelope {
    pub version: u32,
    pub id: Uuid,
    pub auth_token: String,
    pub request: Request,
}

impl std::fmt::Display for ProtocolError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{:?}: {}", self.code, self.message)
    }
}

impl std::error::Error for ProtocolError {}

impl RequestEnvelope {
    pub fn new(request: Request, auth_token: impl Into<String>) -> Self {
        Self {
            version: PROTOCOL_VERSION,
            id: Uuid::new_v4(),
            auth_token: auth_token.into(),
            request,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DaemonEndpoint {
    pub version: u32,
    pub address: String,
    pub auth_token: String,
    pub daemon_id: Uuid,
    pub epoch: Uuid,
    pub pid: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ResponseEnvelope {
    pub version: u32,
    pub id: Uuid,
    pub result: Result<Response, ProtocolError>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EventEnvelope {
    pub version: u32,
    pub event: DaemonEvent,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ErrorCode {
    InvalidRequest,
    UnsupportedVersion,
    NotFound,
    Conflict,
    Busy,
    Unauthorized,
    Internal,
    Unavailable,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProtocolError {
    pub code: ErrorCode,
    pub message: String,
}

impl ProtocolError {
    pub fn new(code: ErrorCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    pub fn internal(message: impl Into<String>) -> Self {
        Self::new(ErrorCode::Internal, message)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "method", content = "params", rename_all = "snake_case")]
pub enum Request {
    Ping,
    DaemonStatus,
    ListSessions,
    CreateSession(CreateSessionRequest),
    AttachSession {
        session_id: String,
        #[serde(default)]
        workdir: Option<String>,
    },
    DetachSession,
    Snapshot,
    /// Replay the attached session bus after a client watermark.
    SessionEvents {
        after: u64,
    },
    SaveSession,
    SetTitle {
        title: Option<String>,
    },
    SubmitTurn(SubmitTurnRequest),
    QueueMessage {
        agent_id: String,
        message: Message,
    },
    CancelTurn {
        turn_id: Uuid,
    },
    Compact {
        agent_id: String,
    },
    Rewind {
        agent_id: String,
        turns: usize,
    },
    EditHistory {
        agent_id: String,
        action: String,
    },
    SetModel(SetModelRequest),
    SetPersona(SetPersonaRequest),
    ExportSession,
    RegisterAccount {
        record: AccountRecord,
    },
    UpdateSettings {
        settings: UserSettings,
    },
    UpdateConfig {
        config: FirmiusConfig,
    },
    Mcp {
        command: McpCommand,
    },
    HostPeek {
        agent_id: String,
        proc_id: firmius_core::ProcId,
        since: usize,
    },
    Shutdown,
    /// Versioned durable-goal API.  Goal handlers may be unavailable on an
    /// older daemon, in which case it returns `UnsupportedVersion` or
    /// `Unavailable` rather than interpreting the payload as a session call.
    Goal(GoalRequest),
    // Direct spellings are retained alongside `Goal` for clients that use
    // the same one-variant-per-method convention as the session API.
    CreateGoal(CreateGoalRequest),
    ListGoals(ListGoalsRequest),
    GetGoal(GetGoalRequest),
    ActivateGoal(ActivateGoalRequest),
    CancelGoal(CancelGoalRequest),
    CheckGoal(CheckGoalRequest),
    ApproveGoal(ApproveGoalRequest),
    /// Register the attached connection as the sole interactive approver for
    /// this session. Agent/tool code has no protocol path to this method.
    RegisterPermissionApprover,
    UnregisterPermissionApprover,
    GetPermissionPolicy,
    UpdatePermissionPolicy {
        policy: PermissionPolicy,
        expected_revision: u64,
    },
    SetPermissionMode {
        mode: PermissionMode,
        expected_revision: u64,
    },
    ResolvePermission(PermissionResolution),
    /// Cross-session memory is daemon-owned. The nested DTO version permits
    /// future memory schema evolution without silently changing semantics.
    Memory(MemoryRequest),
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MemoryRequest {
    pub version: u32,
    pub operation: MemoryOperationRequest,
}

impl MemoryRequest {
    pub fn new(operation: MemoryOperationRequest) -> Self {
        Self {
            version: MEMORY_DTO_VERSION,
            operation,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "action", content = "params", rename_all = "snake_case")]
pub enum MemoryOperationRequest {
    Inspect(MemoryInspectRequest),
    Retrieve(MemoryRetrieveRequest),
    Remember(MemoryRememberRequest),
    Correct(MemoryCorrectRequest),
    Forget(MemoryForgetRequest),
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum MemoryScopeDto {
    User,
    Project { project_id: String },
    Session { session_id: String },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct MemoryViewDto {
    #[serde(default = "default_true")]
    pub include_user: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub project_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
}

impl Default for MemoryViewDto {
    fn default() -> Self {
        Self {
            include_user: true,
            project_id: None,
            session_id: None,
        }
    }
}

fn default_true() -> bool {
    true
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum MemoryKindDto {
    Fact,
    Preference,
    Decision,
    Constraint,
    Procedure,
    Note,
}

impl Default for MemoryKindDto {
    fn default() -> Self {
        Self::Note
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct MemoryEvidenceDto {
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub locator: Option<String>,
    pub excerpt: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct NewMemoryDto {
    pub scope: MemoryScopeDto,
    #[serde(default)]
    pub kind: MemoryKindDto,
    pub title: String,
    pub body: String,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub evidence: Vec<MemoryEvidenceDto>,
    #[serde(default = "default_confidence")]
    pub confidence: f32,
}

fn default_confidence() -> f32 {
    1.0
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum MemoryRecordStateDto {
    Candidate,
    Active,
    Disputed,
    Superseded { by: String },
    Expired,
    Forgotten,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MemoryRecordDto {
    pub id: String,
    pub scope: MemoryScopeDto,
    pub kind: MemoryKindDto,
    pub title: String,
    pub body: String,
    pub tags: Vec<String>,
    pub evidence: Vec<MemoryEvidenceDto>,
    pub confidence: f32,
    pub state: MemoryRecordStateDto,
    pub created_at: chrono::DateTime<chrono::Utc>,
    pub updated_at: chrono::DateTime<chrono::Utc>,
    pub record_version: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct MemoryInspectRequest {
    pub target_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct MemoryRetrieveRequest {
    pub query: String,
    #[serde(default)]
    pub view: MemoryViewDto,
    #[serde(default = "default_memory_limit")]
    pub limit: usize,
}

fn default_memory_limit() -> usize {
    20
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MemoryRememberRequest {
    pub memory: NewMemoryDto,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub expected_revision: Option<u64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MemoryCorrectRequest {
    pub target_id: String,
    pub replacement: NewMemoryDto,
    pub expected_revision: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct MemoryForgetRequest {
    pub target_id: String,
    pub expected_revision: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MemorySearchHitDto {
    pub record: MemoryRecordDto,
    pub score: f32,
    pub matched_terms: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MemoryResponse {
    pub version: u32,
    pub revision: u64,
    pub result: MemoryOperationResponse,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "action", content = "result", rename_all = "snake_case")]
pub enum MemoryOperationResponse {
    Inspected {
        records: Vec<MemoryRecordDto>,
    },
    Retrieved {
        hits: Vec<MemorySearchHitDto>,
    },
    Remembered {
        record: MemoryRecordDto,
    },
    Corrected {
        previous: MemoryRecordDto,
        replacement: MemoryRecordDto,
    },
    Forgotten {
        target_id: String,
        forgotten_at: chrono::DateTime<chrono::Utc>,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MemoryEvent {
    pub version: u32,
    pub revision: u64,
    pub change: MemoryEventKind,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum MemoryEventKind {
    Remembered {
        record: MemoryRecordDto,
    },
    Corrected {
        previous_id: String,
        replacement: MemoryRecordDto,
    },
    Forgotten {
        target_id: String,
        forgotten_at: chrono::DateTime<chrono::Utc>,
        reason: Option<String>,
    },
}

/// Compact per-agent todo projection used for recovery and live status. It is
/// intentionally independent from the richer, persisted core ledger.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TodoProjectionDto {
    pub version: u32,
    pub agent_id: String,
    pub revision: u64,
    pub pending: usize,
    pub in_progress: usize,
    pub blocked: usize,
    pub completed: usize,
    pub items: Vec<TodoItemDto>,
    /// Typed completion state. Renderers must use this instead of inferring
    /// finality from counts or status labels.
    #[serde(default)]
    pub completion: TodoCompletionDto,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum TodoOutcomeKindDto {
    #[default]
    Completed,
    Cancelled,
}

/// Typed mirror of the core `CompletionEvaluation`. `Ready` is intentionally
/// not final: only an explicit assessment records a terminal outcome.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum TodoCompletionDto {
    #[default]
    NoActiveCycle,
    Waiting {
        unfinished: usize,
        blocked: usize,
        evidence_deficits: usize,
    },
    Ready {
        completed: usize,
        cancelled: usize,
        evidence_receipts: usize,
    },
    Final {
        outcome: TodoOutcomeKindDto,
        completed: usize,
        cancelled: usize,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TodoItemDto {
    pub id: String,
    pub title: String,
    pub status: TodoStatusDto,
    /// Evidence already attached to this item.
    #[serde(default)]
    pub evidence_count: usize,
    /// Whether completion requires at least one evidence receipt.
    #[serde(default)]
    pub evidence_required: bool,
    /// Why a blocked item cannot proceed.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub waiting_reason: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TodoStatusDto {
    Pending,
    InProgress,
    Blocked,
    Completed,
    Cancelled,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TodoEvent {
    pub version: u32,
    pub session_id: String,
    pub agent_id: String,
    pub projection: TodoProjectionDto,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PermissionResolution {
    pub request_id: Uuid,
    pub nonce: Uuid,
    pub session_id: String,
    pub agent_id: String,
    pub tool: String,
    pub action_digest: String,
    pub expected_revision: u64,
    pub decision: PermissionDecision,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CreateSessionRequest {
    pub provider_id: String,
    pub model: String,
    pub effort: Option<EffortMode>,
    pub persona: Option<String>,
    pub workdir: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SubmitTurnRequest {
    pub agent_id: String,
    pub message: Message,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SetModelRequest {
    pub agent_id: String,
    pub provider_id: String,
    pub model: String,
    pub effort: Option<EffortMode>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SetPersonaRequest {
    pub agent_id: String,
    pub persona: Option<String>,
    pub delegated: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "action", rename_all = "snake_case")]
pub enum McpCommand {
    List,
    Add { config: McpServerConfig },
    Remove { name: String },
    Start { name: String },
    Stop { name: String },
    Restart { name: String },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum Response {
    Pong {
        daemon_id: Uuid,
        epoch: Uuid,
    },
    Ack,
    Status(DaemonStatus),
    Sessions(Vec<SessionSummary>),
    Snapshot(SessionSnapshot),
    SessionEvents {
        events: Vec<SessionEvent>,
        earliest: u64,
        latest: u64,
    },
    TurnAccepted {
        turn_id: Uuid,
        /// Session event boundary captured when the daemon registers the turn.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        acceptance_sequence: Option<u64>,
    },
    Rewound {
        removed: usize,
    },
    EditHistory {
        result: String,
    },
    Export(SessionRecord),
    Mcp(Vec<McpStatus>),
    HostPeek(HostPeekResponse),
    Goal(GoalResponse),
    PermissionPolicy(PermissionPolicy),
    PermissionUpdated(PermissionPolicy),
    Memory(MemoryResponse),
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DaemonStatus {
    pub daemon_id: Uuid,
    pub epoch: Uuid,
    pub pid: u32,
    pub active_sessions: usize,
    pub active_turns: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionSnapshot {
    pub session_id: String,
    pub title: Option<String>,
    /// Unified session-bus watermark used to discard older live events after
    /// an authoritative reconnect snapshot.
    pub sequence: u64,
    pub primary_agent_id: String,
    pub agents: Vec<AgentSnapshot>,
    pub hierarchy: HashMap<String, HierarchySnapshot>,
    pub work: WorkSnapshot,
    pub active_turns: HashMap<String, Uuid>,
    pub active_delegates: usize,
    /// Uncommitted current-turn events, replayed after histories on attach.
    /// This keeps a reconnect from losing text already streamed by a turn
    /// whose final assistant message has not reached durable history yet.
    pub live_events: Vec<SessionEvent>,
}

/// Lightweight live state for attached clients. Routine updates use this
/// shape instead of rebuilding and shipping a full recovery snapshot.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionStatus {
    pub session_id: String,
    pub title: Option<String>,
    pub sequence: u64,
    pub primary_agent_id: String,
    pub agents: Vec<AgentStatus>,
    pub hierarchy: HashMap<String, HierarchySnapshot>,
    pub work: WorkSnapshot,
    pub active_turns: HashMap<String, Uuid>,
    pub active_delegates: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AgentStatus {
    pub id: String,
    pub provider_id: String,
    pub model: String,
    pub effort: Option<EffortMode>,
    pub workdir: std::path::PathBuf,
    pub label: Option<String>,
    pub usage: Usage,
    pub total_usage: Usage,
    pub busy: bool,
    pub processes: Vec<ProcInfo>,
    /// Optional until every persisted AgentRecord carries a native ledger.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub todo: Option<TodoProjectionDto>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AgentSnapshot {
    pub record: AgentRecord,
    pub usage: Usage,
    /// Usage accumulated across all turns. `usage` is the current-turn
    /// counter and is reset after each completed request; keeping the
    /// cumulative value in the snapshot lets remote UIs render the same
    /// counters as an embedded session.
    #[serde(default)]
    pub total_usage: Usage,
    pub busy: bool,
    pub processes: Vec<ProcInfo>,
    /// Compact reconnect projection; the canonical todo ledger remains in
    /// the daemon-owned agent record.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub todo: Option<TodoProjectionDto>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct HierarchySnapshot {
    pub parent_id: Option<String>,
    pub spawned_via_tool_call_id: Option<String>,
    pub label: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct McpStatus {
    pub name: String,
    pub transport: String,
    pub running: bool,
    pub tool_count: usize,
    pub enabled: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct HostPeekResponse {
    pub bytes: Vec<u8>,
    pub total: usize,
    pub status: ProcStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "kind", content = "data", rename_all = "snake_case")]
pub enum DaemonEvent {
    Ready {
        daemon_id: Uuid,
        epoch: Uuid,
    },
    /// The client reader observed that the daemon connection ended.  This is
    /// deliberately sent on the client's local event bus (rather than over
    /// the broken socket) so subscribers can preserve their last snapshot
    /// and begin reconnecting.
    ConnectionLost {
        reason: String,
    },
    Session(SessionEvent),
    /// Compact live state emitted alongside session events. Unlike a
    /// recovery snapshot this never contains agent histories or artifacts.
    SessionStatus(SessionStatus),
    SnapshotRequired {
        session_id: String,
        reason: String,
    },
    TurnCompleted {
        session_id: String,
        agent_id: String,
        turn_id: Uuid,
        result: Result<(), String>,
    },
    Attached(SessionSnapshot),
    ShuttingDown,
    Goal(GoalEventEnvelope),
    PermissionRequested(PendingPermissionRequest),
    PermissionResolved {
        request_id: Uuid,
        session_id: String,
        decision: PermissionDecision,
    },
    Memory(MemoryEvent),
    Todo(TodoEvent),
}

pub fn encode_frame<T: Serialize>(value: &T) -> Result<Vec<u8>, ProtocolError> {
    let payload = serde_json::to_vec(value)
        .map_err(|error| ProtocolError::new(ErrorCode::InvalidRequest, error.to_string()))?;
    if payload.len() > MAX_FRAME_BYTES {
        return Err(ProtocolError::new(
            ErrorCode::InvalidRequest,
            format!("frame exceeds {MAX_FRAME_BYTES} bytes"),
        ));
    }
    let mut frame = Vec::with_capacity(4 + payload.len());
    frame.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    frame.extend_from_slice(&payload);
    Ok(frame)
}

pub fn decode_payload<T: DeserializeOwned>(payload: &[u8]) -> Result<T, ProtocolError> {
    if payload.len() > MAX_FRAME_BYTES {
        return Err(ProtocolError::new(
            ErrorCode::InvalidRequest,
            format!("frame exceeds {MAX_FRAME_BYTES} bytes"),
        ));
    }
    serde_json::from_slice(payload)
        .map_err(|error| ProtocolError::new(ErrorCode::InvalidRequest, error.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn turn_acceptance_boundary_is_optional_on_the_wire() {
        let turn_id = Uuid::new_v4();
        let response = Response::TurnAccepted { turn_id, acceptance_sequence: Some(42) };
        let mut wire = serde_json::to_value(response).unwrap();
        let decoded: Response = serde_json::from_value(wire.clone()).unwrap();
        assert!(matches!(decoded, Response::TurnAccepted { acceptance_sequence: Some(42), .. }));
        // Locate the response payload without coupling this regression to
        // the envelope's tagging convention.
        fn remove_boundary(value: &mut serde_json::Value) {
            if let Some(object) = value.as_object_mut() {
                object.remove("acceptance_sequence");
                for value in object.values_mut() { remove_boundary(value); }
            }
        }
        remove_boundary(&mut wire);
        let decoded: Response = serde_json::from_value(wire).unwrap();
        assert!(matches!(decoded, Response::TurnAccepted { acceptance_sequence: None, .. }));
    }

    #[test]
    fn protocol_version_and_memory_contract_are_explicit() {
        assert_eq!(PROTOCOL_VERSION, 2);
        let request = Request::Memory(MemoryRequest::new(MemoryOperationRequest::Retrieve(
            MemoryRetrieveRequest {
                query: "preferred formatter".into(),
                view: MemoryViewDto {
                    project_id: Some("project-1".into()),
                    ..Default::default()
                },
                limit: 7,
            },
        )));
        let json = serde_json::to_value(&request).unwrap();
        assert_eq!(json["method"], "memory");
        assert_eq!(json["params"]["version"], MEMORY_DTO_VERSION);
        let decoded: Request = serde_json::from_value(json).unwrap();
        assert!(matches!(
            decoded,
            Request::Memory(MemoryRequest {
                operation: MemoryOperationRequest::Retrieve(MemoryRetrieveRequest { limit: 7, .. }),
                ..
            })
        ));
    }

    #[test]
    fn optional_todo_projection_is_backward_compatible() {
        let status: AgentStatus = serde_json::from_value(serde_json::json!({
            "id": "agent",
            "provider_id": "provider",
            "model": "model",
            "effort": null,
            "workdir": ".",
            "label": null,
            "usage": Usage::default(),
            "total_usage": Usage::default(),
            "busy": false,
            "processes": []
        }))
        .unwrap();
        assert!(status.todo.is_none());

        let projection = TodoProjectionDto {
            version: TODO_DTO_VERSION,
            agent_id: "agent".into(),
            revision: 4,
            pending: 1,
            in_progress: 0,
            blocked: 0,
            completed: 2,
            items: vec![TodoItemDto {
                id: "todo-1".into(),
                title: "Verify reconnect".into(),
                status: TodoStatusDto::Pending,
                evidence_count: 0,
                evidence_required: false,
                waiting_reason: None,
            }],
            completion: TodoCompletionDto::Waiting {
                unfinished: 1,
                blocked: 0,
                evidence_deficits: 0,
            },
        };
        let event = DaemonEvent::Todo(TodoEvent {
            version: TODO_DTO_VERSION,
            session_id: "session".into(),
            agent_id: "agent".into(),
            projection,
        });
        let decoded: DaemonEvent =
            serde_json::from_value(serde_json::to_value(event).unwrap()).unwrap();
        assert!(matches!(decoded, DaemonEvent::Todo(_)));
    }

    #[test]
    fn request_envelope_round_trips() {
        let value = RequestEnvelope::new(Request::Ping, "secret");
        let json = serde_json::to_vec(&value).unwrap();
        let decoded: RequestEnvelope = decode_payload(&json).unwrap();
        assert_eq!(decoded.id, value.id);
        assert!(matches!(decoded.request, Request::Ping));
    }

    #[test]
    fn frame_is_big_endian_length_prefixed() {
        let value = RequestEnvelope::new(Request::Ping, "secret");
        let frame = encode_frame(&value).unwrap();
        let length = u32::from_be_bytes(frame[..4].try_into().unwrap()) as usize;
        assert_eq!(length, frame.len() - 4);
        let decoded: RequestEnvelope = decode_payload(&frame[4..]).unwrap();
        assert_eq!(decoded.id, value.id);
        assert!(matches!(decoded.request, Request::Ping));
    }

    #[test]
    fn oversized_frame_is_rejected() {
        let payload = vec![b' '; MAX_FRAME_BYTES + 1];
        assert_eq!(
            decode_payload::<serde_json::Value>(&payload)
                .unwrap_err()
                .code,
            ErrorCode::InvalidRequest
        );
    }

    #[test]
    fn oversized_logical_payload_is_bounded_and_chunked() {
        let value = serde_json::json!({
            "payload": "x".repeat(MAX_FRAME_BYTES + 1),
        });
        let frames = encode_frames(&value).unwrap();
        assert!(frames.len() >= 2);
        assert!(
            frames
                .iter()
                .all(|frame| frame.len() <= 4 + MAX_FRAME_BYTES)
        );
        let chunks: Vec<ChunkFrame> = frames
            .iter()
            .map(|frame| {
                let payload = &frame[4..];
                serde_json::from_slice(payload).unwrap()
            })
            .collect();
        assert_eq!(chunks.len(), 33);
        assert_eq!(chunks[0].index, 0);
        assert_eq!(chunks.last().unwrap().index as usize + 1, chunks.len());
        assert!(
            chunks
                .iter()
                .all(|chunk| chunk.data.len() <= CHUNK_DATA_BYTES)
        );
        assert_eq!(chunks[0].aggregate_len as usize, MAX_FRAME_BYTES + 15);
    }

    #[test]
    fn permission_messages_are_additive_and_round_trip() {
        let request = RequestEnvelope::new(Request::RegisterPermissionApprover, "secret");
        let decoded: RequestEnvelope =
            decode_payload(&serde_json::to_vec(&request).unwrap()).unwrap();
        assert!(matches!(
            decoded.request,
            Request::RegisterPermissionApprover
        ));

        let resolution = PermissionResolution {
            request_id: Uuid::new_v4(),
            nonce: Uuid::new_v4(),
            session_id: "session".into(),
            agent_id: "agent".into(),
            tool: "edit".into(),
            action_digest: "digest".into(),
            expected_revision: 4,
            decision: PermissionDecision::Deny,
        };
        let wire = serde_json::to_vec(&Request::ResolvePermission(resolution.clone())).unwrap();
        let decoded: Request = serde_json::from_slice(&wire).unwrap();
        assert!(
            matches!(decoded, Request::ResolvePermission(value) if value.expected_revision == 4)
        );
    }
}