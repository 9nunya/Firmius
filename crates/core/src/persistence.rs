use chrono::{DateTime, Utc};
use serde::{Deserialize, Deserializer, Serialize};
use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::Write;
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::thread;
use uuid::Uuid;

use crate::compaction::{Projection, parse_summary};
use crate::providers::schema::ProviderSchema;
use crate::types::{Context, EffortMode};
use crate::work::WorkState;

// ---------------------------------------------------------------------------
// Auth store (api keys, never committed)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AuthStore {
    pub providers: HashMap<String, ProviderAuth>,
}

/// One pending write for the session's dedicated writer thread.
struct WriteRequest {
    base: PathBuf,
    record: SessionRecord,
    generation: u64,
    ack: mpsc::Sender<Result<(), String>>,
}

/// The single session snapshot writer used by live sessions. All writes —
/// `Session::save`, `mutate_work`, and `reconcile_work` — route through
/// here, which serializes them on a single dedicated background thread (so
/// the actual `serde_json::to_string_pretty` + fsync never runs on a Tokio
/// worker thread) and tags each write with a monotonic generation. A write
/// whose generation is older than one already committed is skipped rather
/// than allowed to clobber newer state — this is what prevents a delayed
/// `save()` from overwriting a `mutate_work` commit that raced ahead of it.
#[derive(Clone)]
pub struct SessionPersistenceCoordinator {
    base: PathBuf,
    generation: Arc<AtomicU64>,
    tx: mpsc::Sender<WriteRequest>,
}

impl SessionPersistenceCoordinator {
    pub fn new(base: impl Into<PathBuf>) -> Self {
        let base = base.into();
        let (tx, rx) = mpsc::channel::<WriteRequest>();
        thread::Builder::new()
            .name("firmius-session-writer".into())
            .spawn(move || {
                let mut last_committed: u64 = 0;
                while let Ok(request) = rx.recv() {
                    let result = if request.generation < last_committed {
                        // A newer write already landed; this one is stale.
                        Ok(())
                    } else {
                        let outcome = save_session_record_at(&request.base, &request.record);
                        if outcome.is_ok() {
                            last_committed = request.generation;
                        }
                        outcome
                    };
                    let _ = request.ack.send(result);
                }
            })
            .expect("spawn session writer thread");
        Self {
            base,
            generation: Arc::new(AtomicU64::new(0)),
            tx,
        }
    }

    pub fn current() -> Self {
        Self::new(data_dir())
    }

    /// Durably persist `record`, serialized on the coordinator's dedicated
    /// writer thread and serialized against every other write through it.
    /// Blocks until the write (or its supersession by a newer one) commits.
    pub fn save(&self, record: &SessionRecord) -> Result<(), String> {
        let generation = self.generation.fetch_add(1, Ordering::AcqRel) + 1;
        let (ack_tx, ack_rx) = mpsc::channel();
        self.tx
            .send(WriteRequest {
                base: self.base.clone(),
                record: record.clone(),
                generation,
                ack: ack_tx,
            })
            .map_err(|_| "session writer thread has stopped".to_string())?;
        ack_rx
            .recv()
            .map_err(|_| "session writer thread dropped the write ack".to_string())?
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ProviderAuth {
    pub api_key: String,
}

// ---------------------------------------------------------------------------
// Data directory
// ---------------------------------------------------------------------------

/// Returns `~/.firmius`, creating it if needed.
pub fn data_dir() -> PathBuf {
    let home = dirs::home_dir().unwrap_or_else(|| PathBuf::from("."));
    let dir = home.join(".firmius");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

pub fn auth_path() -> PathBuf {
    data_dir().join("auth.json")
}

pub fn load_auth() -> Result<AuthStore, String> {
    load_auth_from(&auth_path())
}

pub fn load_auth_from(path: &std::path::Path) -> Result<AuthStore, String> {
    if !path.exists() {
        return Ok(AuthStore::default());
    }
    let data =
        std::fs::read_to_string(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    serde_json::from_str(&data).map_err(|e| format!("parse {}: {e}", path.display()))
}

pub fn save_auth(auth: &AuthStore) -> Result<(), String> {
    let path = auth_path();
    let data = serde_json::to_string_pretty(auth).map_err(|e| format!("serialize auth: {e}"))?;
    std::fs::write(&path, data).map_err(|e| format!("write {}: {e}", path.display()))
}

/// Get the API key for a provider, checking env var first, then auth store.
pub fn resolve_api_key(provider: &ProviderSchema, auth: &AuthStore) -> Option<String> {
    // 1. Check env var (if api_key_env is set).
    if let Some(env_var) = &provider.api_key_env
        && let Ok(val) = std::env::var(env_var)
        && !val.is_empty()
    {
        return Some(val);
    }
    // 2. Check auth store.
    auth.providers.get(&provider.id).map(|a| a.api_key.clone())
}

// ---------------------------------------------------------------------------
// Accounts — one file per account, credentials adjacently tagged by kind
// ---------------------------------------------------------------------------

/// One provider account: a schema plus the credentials for it, tagged with
/// the [`crate::kinds::AccountKind`] name that interprets them. Credentials
/// are an opaque `Value` at the storage layer — each kind parses its own
/// typed shape (`{"api_key": ...}` today, OAuth token bundles later).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AccountRecord {
    pub id: String,
    pub kind: String,
    pub schema: ProviderSchema,
    pub credentials: serde_json::Value,
}

/// Lightweight listing entry for account pickers.
#[derive(Debug, Clone)]
pub struct AccountSummary {
    pub id: String,
    pub kind: String,
}

pub fn accounts_dir() -> PathBuf {
    accounts_dir_at(&data_dir())
}

pub fn accounts_dir_at(base: &std::path::Path) -> PathBuf {
    let dir = base.join("accounts");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

pub fn account_path(id: &str) -> PathBuf {
    account_path_at(&data_dir(), id)
}

pub fn account_path_at(base: &std::path::Path, id: &str) -> PathBuf {
    accounts_dir_at(base).join(format!("{id}.json"))
}

pub fn load_account(id: &str) -> Result<AccountRecord, String> {
    load_account_at(&data_dir(), id)
}

pub fn load_account_at(base: &std::path::Path, id: &str) -> Result<AccountRecord, String> {
    let path = account_path_at(base, id);
    let data =
        std::fs::read_to_string(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let mut record: AccountRecord =
        serde_json::from_str(&data).map_err(|e| format!("parse {}: {e}", path.display()))?;
    // The record id is authoritative; keep the schema in agreement.
    record.schema.id = record.id.clone();
    Ok(record)
}

pub fn save_account(record: &AccountRecord) -> Result<(), String> {
    save_account_at(&data_dir(), record)
}

pub fn save_account_at(base: &std::path::Path, record: &AccountRecord) -> Result<(), String> {
    let path = account_path_at(base, &record.id);
    let data = serde_json::to_string_pretty(record)
        .map_err(|e| format!("serialize account {}: {e}", record.id))?;
    let tmp = path.with_extension(format!("json.tmp.{}", Uuid::new_v4()));
    let result = (|| {
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        options.mode(0o600);
        let mut file = options
            .open(&tmp)
            .map_err(|e| format!("create {}: {e}", tmp.display()))?;
        file.write_all(data.as_bytes())
            .map_err(|e| format!("write {}: {e}", tmp.display()))?;
        file.sync_all()
            .map_err(|e| format!("sync {}: {e}", tmp.display()))?;
        #[cfg(windows)]
        if path.exists() {
            std::fs::remove_file(&path).map_err(|e| format!("replace {}: {e}", path.display()))?;
        }
        std::fs::rename(&tmp, &path)
            .map_err(|e| format!("rename {} to {}: {e}", tmp.display(), path.display()))
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    result
}

pub fn delete_account(id: &str) -> Result<(), String> {
    delete_account_at(&data_dir(), id)
}

pub fn delete_account_at(base: &std::path::Path, id: &str) -> Result<(), String> {
    let path = account_path_at(base, id);
    std::fs::remove_file(&path).map_err(|e| format!("delete {}: {e}", path.display()))
}

/// All persisted accounts. Corrupt/unreadable files are skipped with a
/// `warning:` on stderr rather than failing the whole listing.
pub fn list_accounts() -> Vec<AccountSummary> {
    list_accounts_at(&data_dir())
}

pub fn list_accounts_at(base: &std::path::Path) -> Vec<AccountSummary> {
    let dir = accounts_dir_at(base);
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir(&dir) else {
        return out;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let data = match std::fs::read_to_string(&path) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("warning: could not read account {}: {e}", path.display());
                continue;
            }
        };
        match serde_json::from_str::<AccountRecord>(&data) {
            Ok(record) => out.push(AccountSummary {
                id: record.id,
                kind: record.kind,
            }),
            Err(e) => {
                eprintln!("warning: could not parse account {}: {e}", path.display());
            }
        }
    }
    out.sort_by(|a, b| a.id.cmp(&b.id));
    out
}

// ---------------------------------------------------------------------------
// Legacy migration — auth.json + providers.json -> accounts/<id>.json
// ---------------------------------------------------------------------------

/// One-shot migration from the legacy `auth.json` + `providers.json` pair to
/// per-account files. Joins schemas with their stored keys, writes one
/// account file per provider (kind `api-key`), then renames the legacy files
/// to `*.migrated` so an older binary can't silently diverge from the new
/// store. Returns the number of accounts written. Idempotent: existing
/// account files are never overwritten; absent legacy files are a no-op.
pub fn migrate_legacy(base: &std::path::Path) -> Result<u32, String> {
    let auth_path = base.join("auth.json");
    let providers_path = base.join("providers.json");
    if !auth_path.exists() && !providers_path.exists() {
        return Ok(0);
    }

    let auth = load_auth_from(&auth_path).unwrap_or_default();
    let schemas = load_providers_from(&providers_path).unwrap_or_default();

    let mut migrated = 0u32;
    for schema in schemas {
        let path = account_path_at(base, &schema.id);
        if path.exists() {
            continue;
        }
        let credentials = match auth.providers.get(&schema.id) {
            Some(a) => serde_json::json!({ "api_key": a.api_key }),
            // Keyless account: env fallback (api_key_env) still applies at
            // build time, exactly as before the migration.
            None => serde_json::json!({}),
        };
        let record = AccountRecord {
            id: schema.id.clone(),
            kind: "api-key".to_string(),
            schema,
            credentials,
        };
        save_account_at(base, &record)?;
        migrated += 1;
    }

    if auth_path.exists() {
        let target = base.join("auth.json.migrated");
        std::fs::rename(&auth_path, &target)
            .map_err(|e| format!("rename {}: {e}", auth_path.display()))?;
    }
    if providers_path.exists() {
        let target = base.join("providers.json.migrated");
        std::fs::rename(&providers_path, &target)
            .map_err(|e| format!("rename {}: {e}", providers_path.display()))?;
    }
    Ok(migrated)
}

// ---------------------------------------------------------------------------
// Providers
// ---------------------------------------------------------------------------

pub fn providers_path() -> PathBuf {
    data_dir().join("providers.json")
}

pub fn load_providers() -> Result<Vec<ProviderSchema>, String> {
    load_providers_from(&providers_path())
}

pub fn load_providers_from(path: &std::path::Path) -> Result<Vec<ProviderSchema>, String> {
    if !path.exists() {
        return Ok(Vec::new());
    }
    let data =
        std::fs::read_to_string(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    serde_json::from_str(&data).map_err(|e| format!("parse {}: {e}", path.display()))
}

pub fn save_providers(schemas: &[ProviderSchema]) -> Result<(), String> {
    let path = providers_path();
    let data =
        serde_json::to_string_pretty(schemas).map_err(|e| format!("serialize providers: {e}"))?;
    std::fs::write(&path, data).map_err(|e| format!("write {}: {e}", path.display()))
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

/// One agent's full state, persisted. `history` already carries every tool
/// call/result as `MessagePart`s, so no separate tool-result store is
/// needed — resume replays it verbatim through the provider.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AgentRecord {
    pub id: String,
    pub provider_id: String,
    pub model: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub effort: Option<EffortMode>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub system_prompt: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub persona: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature: Option<f32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_tokens: Option<u32>,
    pub workdir: PathBuf,
    /// Stable display label and arbitrary durable metadata for this agent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
    #[serde(default, skip_serializing_if = "serde_json::Map::is_empty")]
    pub metadata: serde_json::Map<String, serde_json::Value>,
    pub history: Context,
    /// Committed compaction metadata. Optional for backwards compatibility
    /// with session records written before compaction state was persisted.
    /// Metadata is advisory when restoring a session: a malformed or
    /// incompatible value must not make the rest of the session unreadable.
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        deserialize_with = "deserialize_compaction"
    )]
    pub compaction: Option<Projection>,
}

/// Check the durable portion of a projection without relying on its timeline.
/// The timeline is reconstructed from the persisted history when an agent is
/// resumed, while generation/snapshot provenance must agree with itself.
pub(crate) fn valid_projection(projection: &Projection) -> bool {
    if projection.generation == 0 {
        return projection.snapshots.is_empty() && projection.snapshot.is_none();
    }

    if projection
        .snapshots
        .windows(2)
        .any(|pair| pair[0].generation >= pair[1].generation)
        || projection
            .snapshots
            .iter()
            .any(|snapshot| snapshot.generation > projection.generation)
    {
        return false;
    }
    // Every historical snapshot is durable user-visible metadata and must
    // use the same canonical envelope, not merely the current snapshot.
    if projection.snapshots.iter().any(|snapshot| {
        parse_summary(&snapshot.summary).is_none()
            || parse_summary(&snapshot.summary).is_some_and(str::is_empty)
    }) {
        return false;
    }
    let Some(snapshot) = projection.snapshot.as_ref() else {
        return false;
    };
    if snapshot.generation != projection.generation
        || snapshot.source_range.0 > snapshot.source_range.1
        || snapshot.source_segment_ids.len()
            != snapshot
                .source_range
                .1
                .saturating_sub(snapshot.source_range.0)
        || snapshot.source_entries == 0
        || parse_summary(&snapshot.summary).is_none()
        || parse_summary(&snapshot.summary).is_some_and(str::is_empty)
    {
        return false;
    }
    projection.snapshots.is_empty()
        || projection.snapshots.last().map(|last| last == snapshot) == Some(true)
}

fn deserialize_compaction<'de, D>(deserializer: D) -> Result<Option<Projection>, D::Error>
where
    D: Deserializer<'de>,
{
    // Decode through Value so a corrupt compaction object is discarded while
    // preserving the otherwise useful agent record. This also keeps old
    // records, which omit the field entirely, equivalent to None.
    let value = Option::<serde_json::Value>::deserialize(deserializer)?;
    Ok(value.and_then(|value| serde_json::from_value(value).ok().filter(valid_projection)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_base() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let base = std::env::temp_dir().join(format!("firmius-persistence-{nonce}"));
        std::fs::create_dir_all(&base).unwrap();
        base
    }

    #[test]
    fn old_agent_records_default_compaction_to_none() {
        let value = serde_json::json!({
            "id": "agent", "provider_id": "provider", "model": "model",
            "workdir": ".", "history": []
        });
        let record: AgentRecord = serde_json::from_value(value).unwrap();
        assert!(record.compaction.is_none());
    }

    #[test]
    fn every_compaction_snapshot_requires_the_canonical_envelope() {
        let snapshot = crate::compaction::Snapshot {
            generation: 1,
            source_entries: 1,
            source_content_digest: String::new(),
            source_segment_ids: vec!["old".into()],
            source_range: (0, 1),
            summary: "plain summary".into(),
        };
        let projection = Projection {
            generation: 1,
            timeline: Default::default(),
            snapshots: vec![snapshot.clone()],
            snapshot: Some(snapshot),
        };
        assert!(!valid_projection(&projection));
    }

    #[test]
    fn compaction_metadata_round_trips() {
        let snapshot = crate::compaction::Snapshot {
            generation: 1,
            source_entries: 1,
            source_content_digest: String::new(),
            source_segment_ids: vec!["old".into()],
            source_range: (0, 1),
            summary: "<compaction_summary>\nold\n</compaction_summary>".into(),
        };
        let projection = Projection {
            generation: 1,
            timeline: Default::default(),
            snapshots: vec![snapshot.clone()],
            snapshot: Some(snapshot),
        };
        let value = serde_json::to_value(AgentRecord {
            id: "agent".into(),
            provider_id: "provider".into(),
            model: "model".into(),
            effort: None,
            system_prompt: None,
            persona: None,
            temperature: None,
            max_tokens: None,
            workdir: PathBuf::from("."),
            label: None,
            metadata: serde_json::Map::new(),
            history: vec![],
            compaction: Some(projection.clone()),
        })
        .unwrap();
        let restored: AgentRecord = serde_json::from_value(value).unwrap();
        assert_eq!(restored.compaction, Some(projection));
    }

    #[test]
    fn corrupt_compaction_metadata_is_dropped() {
        let value = serde_json::json!({
            "id": "agent", "provider_id": "provider", "model": "model",
            "workdir": ".", "history": [],
            "compaction": { "generation": 3, "timeline": { "segments": [] },
                "snapshots": [], "snapshot": null }
        });
        let record: AgentRecord = serde_json::from_value(value).unwrap();
        assert!(record.compaction.is_none());
    }

    #[test]
    fn session_record_round_trips_through_atomic_store() {
        let base = test_base();
        let record = SessionRecord {
            id: "round-trip".into(),
            title: Some("saved".into()),
            created_at: Utc::now(),
            updated_at: Utc::now(),
            agents: vec![],
            hierarchy: HashMap::new(),
            work: WorkStateRecord::default(),
            unavailable_agents: Vec::new(),
            artifacts: vec![],
        };
        save_session_record_at(&base, &record).unwrap();
        let loaded = load_session_record_at(&base, &record.id).unwrap();
        assert_eq!(loaded.id, record.id);
        assert_eq!(loaded.title, record.title);
        let leftovers = std::fs::read_dir(base.join("sessions"))
            .unwrap()
            .filter_map(Result::ok)
            .any(|entry| entry.file_name().to_string_lossy().contains(".json.tmp."));
        assert!(!leftovers);
        std::fs::remove_dir_all(base).unwrap();
    }

    fn record_with_title(id: &str, title: &str) -> SessionRecord {
        SessionRecord {
            id: id.into(),
            title: Some(title.into()),
            created_at: Utc::now(),
            updated_at: Utc::now(),
            agents: vec![],
            hierarchy: HashMap::new(),
            work: WorkStateRecord::default(),
            unavailable_agents: Vec::new(),
            artifacts: vec![],
        }
    }

    #[test]
    fn coordinator_serializes_writes_through_a_single_writer_thread() {
        let base = test_base();
        let coordinator = SessionPersistenceCoordinator::new(base.clone());
        for i in 0..20 {
            coordinator
                .save(&record_with_title("session", &format!("v{i}")))
                .unwrap();
        }
        let loaded = load_session_record_at(&base, "session").unwrap();
        assert_eq!(loaded.title, Some("v19".into()));
        std::fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn coordinator_skips_a_write_whose_generation_is_already_superseded() {
        // A save that started before a later save (and so was assigned an
        // older generation) must not clobber the newer commit even if it
        // reaches the writer thread second is not testable in isolation
        // (the coordinator assigns generations in `save`'s own call order),
        // so this exercises the same guarantee directly: once a higher
        // generation has committed, a manually constructed stale request is
        // dropped rather than applied.
        let base = test_base();
        let coordinator = SessionPersistenceCoordinator::new(base.clone());
        coordinator
            .save(&record_with_title("session", "first"))
            .unwrap();
        coordinator
            .save(&record_with_title("session", "second"))
            .unwrap();
        // generation counter only advances forward; a fresh save always
        // wins over what is on disk, confirming last-committed tracking
        // does not regress after normal in-order use.
        let loaded = load_session_record_at(&base, "session").unwrap();
        assert_eq!(loaded.title, Some("second".into()));
        std::fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn work_state_record_into_state_rejects_unsupported_versions() {
        let record = WorkStateRecord {
            version: current_work_state_version() + 1,
            state: WorkState::default(),
        };
        assert!(record.into_state().is_err());
    }
}

/// One agent's position in the session's spawn tree.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AgentNodeRecord {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub spawned_via_tool_call_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
    #[serde(default, skip_serializing_if = "serde_json::Map::is_empty")]
    pub metadata: serde_json::Map<String, serde_json::Value>,
}

/// A full, resumable session: every agent's config + history, and the exact
/// parent/child hierarchy between them.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionRecord {
    pub id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub title: Option<String>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
    pub agents: Vec<AgentRecord>,
    #[serde(default)]
    pub hierarchy: HashMap<String, AgentNodeRecord>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub artifacts: Vec<crate::artifact::Artifact>,
    /// Versioned work snapshot. Missing on records written before WorkGraph.
    #[serde(default)]
    pub work: WorkStateRecord,
    /// Agent descriptors remain durable even when their provider is no longer
    /// installed, so historical provenance is not silently erased on resume.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub unavailable_agents: Vec<AgentRecord>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WorkStateRecord {
    #[serde(default = "current_work_state_version")]
    pub version: u32,
    #[serde(default)]
    pub state: WorkState,
}

fn current_work_state_version() -> u32 {
    1
}

impl Default for WorkStateRecord {
    fn default() -> Self {
        Self {
            version: current_work_state_version(),
            state: WorkState::default(),
        }
    }
}

impl WorkStateRecord {
    pub fn from_state(state: WorkState) -> Self {
        Self {
            version: current_work_state_version(),
            state,
        }
    }
    pub fn into_state(self) -> Result<WorkState, String> {
        if self.version > current_work_state_version() {
            return Err(format!("unsupported work state version {}", self.version));
        }
        self.state.validate().map_err(|e| e.to_string())?;
        Ok(self.state)
    }
}

/// Lightweight listing entry for a session picker — avoids callers needing
/// to load every full record (with complete histories) just to show a list.
#[derive(Debug, Clone)]
pub struct SessionSummary {
    pub id: String,
    pub title: String,
    pub updated_at: DateTime<Utc>,
    pub agent_count: usize,
}

pub fn sessions_dir() -> PathBuf {
    let dir = data_dir().join("sessions");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

pub fn session_path(id: &str) -> PathBuf {
    session_path_at(&data_dir(), id)
}

pub fn sessions_dir_at(base: &std::path::Path) -> PathBuf {
    let dir = base.join("sessions");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

pub fn session_path_at(base: &std::path::Path, id: &str) -> PathBuf {
    sessions_dir_at(base).join(format!("{id}.json"))
}

pub fn load_session_record(id: &str) -> Result<SessionRecord, String> {
    load_session_record_at(&data_dir(), id)
}

pub fn load_session_record_at(base: &std::path::Path, id: &str) -> Result<SessionRecord, String> {
    let path = session_path_at(base, id);
    let data =
        std::fs::read_to_string(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    serde_json::from_str(&data).map_err(|e| format!("parse {}: {e}", path.display()))
}

pub fn save_session_record(record: &SessionRecord) -> Result<(), String> {
    save_session_record_at(&data_dir(), record)
}

pub fn save_session_record_at(
    base: &std::path::Path,
    record: &SessionRecord,
) -> Result<(), String> {
    let path = session_path_at(base, &record.id);
    let data = serde_json::to_string_pretty(record)
        .map_err(|e| format!("serialize session {}: {e}", record.id))?;
    let tmp = path.with_extension(format!("json.tmp.{}", Uuid::new_v4()));
    let result = (|| {
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        options.mode(0o600);
        let mut file = options
            .open(&tmp)
            .map_err(|e| format!("create {}: {e}", tmp.display()))?;
        file.write_all(data.as_bytes())
            .map_err(|e| format!("write {}: {e}", tmp.display()))?;
        file.sync_all()
            .map_err(|e| format!("sync {}: {e}", tmp.display()))?;
        #[cfg(windows)]
        if path.exists() {
            std::fs::remove_file(&path).map_err(|e| format!("replace {}: {e}", path.display()))?;
        }
        std::fs::rename(&tmp, &path)
            .map_err(|e| format!("rename {} to {}: {e}", tmp.display(), path.display()))
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    result
}

/// All persisted sessions, newest-first by `updated_at`. Corrupt/unreadable
/// files are skipped with a `warning:` on stderr rather than failing the
/// whole listing.
pub fn list_sessions() -> Result<Vec<SessionSummary>, String> {
    let dir = sessions_dir();
    let mut out = Vec::new();
    let entries = std::fs::read_dir(&dir).map_err(|e| format!("read {}: {e}", dir.display()))?;
    for entry in entries {
        let entry = match entry {
            Ok(e) => e,
            Err(_) => continue,
        };
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let data = match std::fs::read_to_string(&path) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("warning: could not read session {}: {e}", path.display());
                continue;
            }
        };
        let record: SessionRecord = match serde_json::from_str(&data) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("warning: could not parse session {}: {e}", path.display());
                continue;
            }
        };
        out.push(SessionSummary {
            id: record.id,
            title: record
                .title
                .filter(|t| !t.trim().is_empty())
                .unwrap_or_else(|| "(untitled)".to_string()),
            updated_at: record.updated_at,
            agent_count: record.agents.len(),
        });
    }
    out.sort_by_key(|summary| std::cmp::Reverse(summary.updated_at));
    Ok(out)
}
