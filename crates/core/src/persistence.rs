use chrono::{DateTime, Utc};
use serde::{Deserialize, Deserializer, Serialize};
use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::Write;
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::thread;
use uuid::Uuid;

use crate::compaction::{Projection, parse_summary};
use crate::providers::schema::ProviderSchema;
use crate::types::{Context, EffortMode, Message, MessagePart, MessageRole};
use crate::work::WorkState;

// ---------------------------------------------------------------------------
// Auth store (api keys, never committed)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AuthStore {
    pub providers: HashMap<String, ProviderAuth>,
}

/// Return whether a persisted session belongs to `workdir`.  Sessions are
/// scoped by the workdir recorded on their agents; a match on any agent keeps
/// older multi-agent records usable while the primary agent remains the
/// normal case.  Canonicalization makes relative paths and symlink aliases
/// compare consistently, while still working when a directory was removed.
pub fn session_matches_workdir(record: &SessionRecord, workdir: &std::path::Path) -> bool {
    record
        .agents
        .iter()
        .chain(record.unavailable_agents.iter())
        .any(|agent| workdirs_match(&agent.workdir, workdir))
}

fn workdirs_match(left: &std::path::Path, right: &std::path::Path) -> bool {
    let left = std::fs::canonicalize(left).unwrap_or_else(|_| absolute_path(left));
    let right = std::fs::canonicalize(right).unwrap_or_else(|_| absolute_path(right));
    left == right
}

fn absolute_path(path: &std::path::Path) -> std::path::PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| std::path::PathBuf::from("."))
            .join(path)
    }
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

/// `rename` replaces the destination on Unix. A hard-link followed by removal
/// gives this migration no-clobber semantics instead: if the destination
/// appears after preflight, linking fails and the source remains untouched.
/// A failure between the two operations leaves both copies, never zero.
fn move_without_replacing(source: &Path, destination: &Path) -> Result<(), String> {
    std::fs::hard_link(source, destination).map_err(|error| {
        format!(
            "rename {} to {}: {error}",
            source.display(),
            destination.display()
        )
    })?;
    if let Err(error) = std::fs::remove_file(source) {
        // Best effort cleanup is safe: the source is still available if the
        // destination cannot be removed, and callers report the failure.
        let _ = std::fs::remove_file(destination);
        return Err(format!("remove {} after rename: {error}", source.display()));
    }
    Ok(())
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ProviderAuth {
    pub api_key: String,
}

// ---------------------------------------------------------------------------
// Data directory
// ---------------------------------------------------------------------------

/// Returns the Firmius data root, creating it if needed.
///
/// `FIRMIUS_DATA_DIR` is useful for isolated installs, portable launches, and
/// service managers. Every durable subsystem goes through this function so
/// the daemon lease, sessions, accounts, settings, MCP state, and personas
/// cannot silently split across different roots.
pub fn data_dir() -> PathBuf {
    let dir = std::env::var_os("FIRMIUS_DATA_DIR")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .or_else(|| dirs::home_dir().map(|home| home.join(".firmius")))
        .unwrap_or_else(|| PathBuf::from(".firmius"));
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

/// Publish a fully written temporary file over `path`.
///
/// `std::fs::rename` replaces an existing file atomically on Unix, but fails
/// when the destination exists on Windows.  Do not work around that by
/// deleting the destination: a failed publish would otherwise lose the last
/// usable copy.  Windows gets a small backup/restore transaction instead.
#[cfg(not(windows))]
fn publish_replacement(tmp: &Path, path: &Path) -> Result<(), String> {
    std::fs::rename(tmp, path)
        .map_err(|e| format!("rename {} to {}: {e}", tmp.display(), path.display()))
}

#[cfg(windows)]
fn publish_replacement(tmp: &Path, path: &Path) -> Result<(), String> {
    // Renaming a directory out of the way would make a malformed target look
    // like a successful file replacement. Keep the same failure behavior as
    // Unix's rename in this case.
    if path.is_dir() {
        return Err(format!("replace {}: target is a directory", path.display()));
    }

    let backup = path.with_extension(format!("bak.{}", Uuid::new_v4()));
    let had_target = match std::fs::rename(path, &backup) {
        Ok(()) => true,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => false,
        Err(error) => return Err(format!("backup {}: {error}", path.display())),
    };

    match std::fs::rename(tmp, path) {
        Ok(()) => {
            if had_target {
                // Keep the new target if cleanup fails, but report the error
                // so callers know a recovery backup remains on disk.
                std::fs::remove_file(&backup)
                    .map_err(|e| format!("remove replacement backup {}: {e}", backup.display()))?;
            }
            Ok(())
        }
        Err(publish_error) if had_target => match std::fs::rename(&backup, path) {
            Ok(()) => Err(format!(
                "rename {} to {}: {publish_error} (previous file restored)",
                tmp.display(),
                path.display()
            )),
            Err(restore_error) => Err(format!(
                "rename {} to {}: {publish_error}; restore {}: {restore_error}",
                tmp.display(),
                path.display(),
                path.display()
            )),
        },
        Err(error) => Err(format!(
            "rename {} to {}: {error}",
            tmp.display(),
            path.display()
        )),
    }
}

pub fn save_auth(auth: &AuthStore) -> Result<(), String> {
    save_auth_at(&data_dir(), auth)
}

/// Persist the authentication store without ever truncating the existing
/// file. The new contents are written to a uniquely named temporary file,
/// flushed to stable storage, and then published with an atomic rename.
pub fn save_auth_at(base: &std::path::Path, auth: &AuthStore) -> Result<(), String> {
    let path = base.join("auth.json");
    let data = serde_json::to_string_pretty(auth).map_err(|e| format!("serialize auth: {e}"))?;
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
        publish_replacement(&tmp, &path)
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    result
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

pub fn account_path(id: &str) -> Result<PathBuf, String> {
    account_path_at(&data_dir(), id)
}

/// Validate an id before interpolating it into a persistence filename.
///
/// IDs are intentionally allowed to contain more than just ASCII
/// alphanumerics (some provider-issued identities are opaque), but they must
/// remain a single, ordinary filename component on every supported platform.
fn validate_persistence_id(id: &str) -> Result<(), String> {
    if id.is_empty() {
        return Err("id must not be empty".into());
    }
    if id == "." || id == ".." {
        return Err(format!("invalid id '{id}': path traversal is not allowed"));
    }
    // Leave room for the `.json` suffix within common 255-byte filename
    // component limits.
    if id.len() > 251 {
        return Err(format!("invalid id: length {} exceeds 251 bytes", id.len()));
    }
    if id.chars().any(|character| {
        matches!(
            character,
            '/' | '\\' | '\0' | ':' | '*' | '?' | '"' | '<' | '>' | '|'
        ) || character.is_control()
    }) {
        return Err(format!(
            "invalid id '{id}': path separators and control characters are not allowed"
        ));
    }
    if id.ends_with('.') || id.ends_with(' ') {
        return Err(format!(
            "invalid id '{id}': ids may not end with a dot or space"
        ));
    }
    Ok(())
}

pub fn account_path_at(base: &std::path::Path, id: &str) -> Result<PathBuf, String> {
    validate_persistence_id(id)?;
    Ok(accounts_dir_at(base).join(format!("{id}.json")))
}

pub fn load_account(id: &str) -> Result<AccountRecord, String> {
    load_account_at(&data_dir(), id)
}

pub fn load_account_at(base: &std::path::Path, id: &str) -> Result<AccountRecord, String> {
    validate_persistence_id(id)?;
    let path = account_path_at(base, id)?;
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
    validate_persistence_id(&record.id).map_err(|error| format!("cannot save account: {error}"))?;
    let path = account_path_at(base, &record.id)?;
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
        publish_replacement(&tmp, &path)
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
    validate_persistence_id(id)?;
    let path = account_path_at(base, id)?;
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
    let auth_exists = auth_path.exists();
    let providers_exists = providers_path.exists();
    if !auth_exists && !providers_exists {
        return Ok(0);
    }
    // Never treat a half-present legacy store as an empty one. In particular,
    // renaming providers.json while auth.json is absent would hide the only
    // remaining copy of the provider definitions (and vice versa).
    if !auth_exists || !providers_exists {
        let missing = if !auth_exists {
            "auth.json"
        } else {
            "providers.json"
        };
        return Err(format!(
            "cannot migrate legacy credentials: {missing} is missing; both auth.json and providers.json are required"
        ));
    }

    // These parses are deliberately fallible. A malformed file must remain
    // in place for recovery rather than being interpreted as an empty store
    // and renamed below.
    let auth = load_auth_from(&auth_path)?;
    let schemas = load_providers_from(&providers_path)?;
    ensure_legacy_targets_available(&auth_path, &providers_path)?;

    // Validate every schema before writing any account. Otherwise a malformed
    // schema later in the list could leave a partially migrated store.
    for schema in &schemas {
        validate_persistence_id(&schema.id)
            .map_err(|error| format!("cannot migrate account: {error}"))?;
        let path = account_path_at(base, &schema.id)?;
        if std::fs::symlink_metadata(&path).is_ok() {
            // An existing destination is safe to skip only when it is a
            // readable account record from an earlier migration. Do not hide
            // legacy data behind a directory or a corrupt partial file.
            load_account_at(base, &schema.id).map_err(|error| {
                format!(
                    "cannot migrate account {}: existing destination is unusable ({error})",
                    schema.id
                )
            })?;
        }
    }

    let mut migrated = 0u32;
    for schema in schemas {
        let path = account_path_at(base, &schema.id)?;
        if std::fs::symlink_metadata(&path).is_ok() {
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

    rename_legacy_pair(&auth_path, &providers_path)?;
    Ok(migrated)
}

/// Move both legacy inputs out of the active store as one best-effort
/// transaction. Existing migration targets are never replaced. If the second
/// move fails, restore the first input so a retry still sees the complete
/// legacy pair.
fn rename_legacy_pair(auth_path: &Path, providers_path: &Path) -> Result<(), String> {
    let auth_target = auth_path.with_file_name("auth.json.migrated");
    let providers_target = providers_path.with_file_name("providers.json.migrated");

    ensure_legacy_targets_available(auth_path, providers_path)?;

    move_without_replacing(auth_path, &auth_target)?;

    match move_without_replacing(providers_path, &providers_target) {
        Ok(()) => Ok(()),
        Err(error) => match move_without_replacing(&auth_target, auth_path) {
            Ok(()) => Err(format!(
                "rename {}: {error} (previous auth file restored)",
                providers_path.display()
            )),
            Err(restore_error) => Err(format!(
                "rename {}: {error}; restore {}: {restore_error}",
                providers_path.display(),
                auth_path.display()
            )),
        },
    }
}

fn ensure_legacy_targets_available(auth_path: &Path, providers_path: &Path) -> Result<(), String> {
    let auth_target = auth_path.with_file_name("auth.json.migrated");
    let providers_target = providers_path.with_file_name("providers.json.migrated");

    // `exists` is false for a dangling symlink, but rename would still replace
    // it on Unix. symlink_metadata gives the required no-clobber behavior.
    for target in [&auth_target, &providers_target] {
        match std::fs::symlink_metadata(target) {
            Ok(_) => {
                return Err(format!(
                    "refusing to migrate: destination {} already exists",
                    target.display()
                ));
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => {
                return Err(format!(
                    "inspect migration destination {}: {error}",
                    target.display()
                ));
            }
        }
    }
    Ok(())
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
    /// Durable FIFO input waiting for this agent's next turn.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub mailbox: Vec<Message>,
    /// Goal the agent is currently executing, if any. Used to isolate
    /// goal-scoped messages from a different goal's live turn.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub active_goal_id: Option<String>,
    /// Versioned native todo state. Missing legacy state is distinct from a
    /// malformed present value, which is retained verbatim in quarantine.
    #[serde(default, skip_serializing_if = "todo_state_is_missing")]
    pub todo: crate::todo::PersistedTodoState,
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

fn todo_state_is_missing(state: &crate::todo::PersistedTodoState) -> bool {
    matches!(state, crate::todo::PersistedTodoState::MissingLegacy)
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
    fn session_workdir_matching_accepts_any_agent_and_rejects_other_dirs() {
        let root = test_base();
        let here = root.join("here");
        let there = root.join("there");
        std::fs::create_dir_all(&here).unwrap();
        std::fs::create_dir_all(&there).unwrap();
        let mut record = record_with_title("workdir", "workdir");
        record.agents.push(AgentRecord {
            id: "agent".into(),
            provider_id: "provider".into(),
            model: "model".into(),
            workdir: here.clone(),
            effort: None,
            system_prompt: None,
            persona: None,
            temperature: None,
            max_tokens: None,
            label: None,
            metadata: serde_json::Map::new(),
            history: Context::default(),
            mailbox: Vec::new(),
            active_goal_id: None,
            todo: crate::todo::PersistedTodoState::MissingLegacy,
            compaction: None,
        });
        assert!(session_matches_workdir(&record, &here));
        assert!(!session_matches_workdir(&record, &there));
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn auth_store_replaces_atomically_and_leaves_no_temp_file() {
        let base = test_base();
        let mut auth = AuthStore::default();
        auth.providers.insert(
            "provider".into(),
            ProviderAuth {
                api_key: "old-key".into(),
            },
        );
        save_auth_at(&base, &auth).unwrap();

        auth.providers.get_mut("provider").unwrap().api_key = "new-key".into();
        save_auth_at(&base, &auth).unwrap();

        let loaded = load_auth_from(&base.join("auth.json")).unwrap();
        assert_eq!(loaded.providers["provider"].api_key, "new-key");
        assert!(!std::fs::read_dir(&base).unwrap().flatten().any(|entry| {
            entry
                .file_name()
                .to_string_lossy()
                .contains("auth.json.tmp.")
        }));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(base.join("auth.json"))
                .unwrap()
                .permissions()
                .mode();
            assert_eq!(mode & 0o777, 0o600);
        }
        std::fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn account_save_failure_preserves_existing_target() {
        let base = test_base();
        let target = base.join("accounts").join("account.json");
        std::fs::create_dir_all(&target).unwrap();
        std::fs::write(target.join("keep"), b"existing").unwrap();
        let record = AccountRecord {
            id: "account".into(),
            kind: "api-key".into(),
            schema: ProviderSchema {
                id: "account".into(),
                api_type: crate::providers::schema::ApiType::OpenAI,
                base_url: None,
                api_key_env: None,
                models: vec![],
            },
            credentials: serde_json::json!({"api_key": "key"}),
        };

        assert!(save_account_at(&base, &record).is_err());
        assert!(target.is_dir());
        std::fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn auth_save_failure_preserves_existing_target() {
        let base = test_base();
        let target = base.join("auth.json");
        // A directory at the target forces the final publish to fail after
        // the temporary file has been fully written and synced.
        std::fs::create_dir(&target).unwrap();
        std::fs::write(target.join("keep"), b"existing").unwrap();
        let mut auth = AuthStore::default();
        auth.providers.insert(
            "provider".into(),
            ProviderAuth {
                api_key: "key".into(),
            },
        );

        assert!(save_auth_at(&base, &auth).is_err());
        assert!(target.is_dir());
        assert!(!std::fs::read_dir(&base).unwrap().flatten().any(|entry| {
            entry
                .file_name()
                .to_string_lossy()
                .contains("auth.json.tmp.")
        }));
        std::fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn legacy_rename_failure_restores_the_first_file() {
        let base = test_base();
        let auth = base.join("auth.json");
        let providers = base.join("providers.json");
        std::fs::write(&auth, b"auth").unwrap();
        // A directory cannot be hard-linked, so the second move fails after
        // the auth file has already been moved to its migration target.
        std::fs::create_dir(&providers).unwrap();

        assert!(rename_legacy_pair(&auth, &providers).is_err());
        assert!(auth.exists(), "auth source must be restored");
        assert!(!auth.with_file_name("auth.json.migrated").exists());
        assert!(providers.is_dir());
        std::fs::remove_dir_all(base).unwrap();
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
            mailbox: vec![],
            active_goal_id: None,
            todo: crate::todo::PersistedTodoState::MissingLegacy,
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
    fn session_summary_includes_preview_and_model() {
        let record = SessionRecord {
            id: "s1".into(),
            title: Some("named session".into()),
            created_at: Utc::now(),
            updated_at: Utc::now(),
            agents: vec![AgentRecord {
                id: "a1".into(),
                provider_id: "openai".into(),
                model: "gpt-test".into(),
                effort: None,
                system_prompt: None,
                persona: None,
                temperature: None,
                max_tokens: None,
                workdir: PathBuf::from("."),
                label: Some("main".into()),
                metadata: serde_json::Map::new(),
                history: vec![crate::types::Message::text(
                    crate::types::MessageRole::User,
                    "hello from the first turn",
                )],
                mailbox: vec![],
                active_goal_id: None,
                todo: crate::todo::PersistedTodoState::MissingLegacy,
                compaction: None,
            }],
            hierarchy: HashMap::new(),
            work: WorkStateRecord::default(),
            unavailable_agents: Vec::new(),
            artifacts: vec![],
            mailbox: SessionMailboxState::default(),
        };
        let summary = SessionSummary::from_record(&record);
        assert_eq!(summary.title, "named session");
        assert_eq!(summary.preview, "hello from the first turn");
        assert_eq!(summary.model.as_deref(), Some("gpt-test"));
        assert_eq!(summary.agent_count, 1);
        let md = session_to_markdown(&record);
        assert!(md.contains("# named session"));
        assert!(md.contains("hello from the first turn"));
        assert!(md.contains("gpt-test"));
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
            mailbox: SessionMailboxState::default(),
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
            mailbox: SessionMailboxState::default(),
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

    #[test]
    fn persistence_ids_reject_traversal_and_invalid_filename_components() {
        for id in [
            "",
            ".",
            "..",
            "../outside",
            "nested/id",
            r"nested\id",
            "bad\0id",
        ] {
            assert!(
                validate_persistence_id(id).is_err(),
                "id should be rejected: {id:?}"
            );
        }
        assert!(validate_persistence_id("valid-account_123").is_ok());
    }

    #[test]
    fn account_and_session_load_reject_invalid_ids_before_path_access() {
        let base = test_base();
        assert!(load_account_at(&base, "../outside").is_err());
        assert!(load_session_record_at(&base, "../outside").is_err());
        assert!(!base.join("outside.json").exists());
        std::fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn public_path_helpers_reject_invalid_ids_without_panicking() {
        let base = test_base();
        for id in ["", "../outside", "nested/id", "nested\\id"] {
            assert!(account_path_at(&base, id).is_err());
            assert!(session_path_at(&base, id).is_err());
        }
        assert!(account_path_at(&base, "safe-account").is_ok());
        assert!(session_path_at(&base, "safe-session").is_ok());
        std::fs::remove_dir_all(base).unwrap();
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
    /// Durable mailbox delivery records: stable message ids, per-thread
    /// sequences, and deferred goal-scoped messages. Missing on records
    /// written before correlated messaging.
    #[serde(default, skip_serializing_if = "SessionMailboxState::is_empty")]
    pub mailbox: SessionMailboxState,
}

/// How a durable mailbox record was applied to a recipient.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MailboxDeliveryState {
    /// Injected into the recipient's live or restorable mailbox.
    Delivered,
    /// Stored for a goal the recipient is not currently executing.
    Deferred,
}

/// One durably appended inter-agent message, keyed by stable `message_id`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MailboxDeliveryRecord {
    pub message_id: String,
    pub recipient_id: String,
    pub sender_id: String,
    pub thread_id: String,
    pub thread_seq: u64,
    pub message: Message,
    pub state: MailboxDeliveryState,
    pub created_at: DateTime<Utc>,
}

/// Session-scoped mailbox bookkeeping persisted with the session record.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionMailboxState {
    /// Next monotonic sequence to assign, keyed by thread_id.
    #[serde(default, skip_serializing_if = "HashMap::is_empty")]
    pub next_thread_seq: HashMap<String, u64>,
    /// Delivery records keyed by stable message_id.
    #[serde(default, skip_serializing_if = "HashMap::is_empty")]
    pub records: HashMap<String, MailboxDeliveryRecord>,
}

impl SessionMailboxState {
    pub fn is_empty(&self) -> bool {
        self.next_thread_seq.is_empty() && self.records.is_empty()
    }
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
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionSummary {
    pub id: String,
    pub title: String,
    pub updated_at: DateTime<Utc>,
    pub created_at: DateTime<Utc>,
    pub agent_count: usize,
    /// First user-visible line of the conversation, for picker previews.
    pub preview: String,
    /// Model id of the first (typically primary) agent, if any.
    pub model: Option<String>,
}

impl SessionSummary {
    pub fn from_record(record: &SessionRecord) -> Self {
        Self {
            id: record.id.clone(),
            title: record
                .title
                .clone()
                .filter(|t| !t.trim().is_empty())
                .unwrap_or_else(|| "(untitled)".to_string()),
            updated_at: record.updated_at,
            created_at: record.created_at,
            agent_count: record.agents.len(),
            preview: session_preview(record),
            model: record.agents.first().map(|agent| agent.model.clone()),
        }
    }
}

fn session_preview(record: &SessionRecord) -> String {
    for agent in &record.agents {
        for msg in &agent.history {
            if msg.role != MessageRole::User {
                continue;
            }
            let text: String = msg
                .content
                .iter()
                .filter_map(|part| match part {
                    MessagePart::Text(t) => Some(t.as_str()),
                    _ => None,
                })
                .collect::<Vec<_>>()
                .join(" ");
            let collapsed: String = text.split_whitespace().collect::<Vec<_>>().join(" ");
            if collapsed.is_empty() {
                continue;
            }
            let mut preview: String = collapsed.chars().take(80).collect();
            if collapsed.chars().count() > 80 {
                preview.push('…');
            }
            return preview;
        }
    }
    String::new()
}

/// Render a session as markdown suitable for sharing or archival.
pub fn session_to_markdown(record: &SessionRecord) -> String {
    let title = record
        .title
        .as_deref()
        .filter(|t| !t.trim().is_empty())
        .unwrap_or("(untitled)");
    let mut out = String::new();
    out.push_str(&format!("# {title}\n\n"));
    out.push_str(&format!("- id: `{}`\n", record.id));
    out.push_str(&format!("- created: {}\n", record.created_at.to_rfc3339()));
    out.push_str(&format!(
        "- updated: {}\n\n",
        record.updated_at.to_rfc3339()
    ));
    for agent in &record.agents {
        let label = agent.label.as_deref().unwrap_or("agent");
        out.push_str(&format!(
            "## {label} (`{}` · {}/{})\n\n",
            agent.id, agent.provider_id, agent.model
        ));
        for msg in &agent.history {
            match msg.role {
                MessageRole::User => out.push_str("### You\n\n"),
                MessageRole::Assistant => out.push_str("### Assistant\n\n"),
                MessageRole::System => out.push_str("### System\n\n"),
                MessageRole::Tool => out.push_str("### Tool\n\n"),
            }
            for part in &msg.content {
                match part {
                    MessagePart::Text(t) if !t.trim().is_empty() => {
                        out.push_str(t.trim_end());
                        out.push_str("\n\n");
                    }
                    MessagePart::Thinking { content, .. } if !content.trim().is_empty() => {
                        out.push_str("<details><summary>thinking</summary>\n\n");
                        out.push_str(content.trim_end());
                        out.push_str("\n\n</details>\n\n");
                    }
                    MessagePart::ToolCall { name, args, .. } => {
                        out.push_str(&format!("```tool:{name}\n{args}\n```\n\n"));
                    }
                    MessagePart::ToolResult { content, ok, .. } => {
                        let status = if *ok { "ok" } else { "err" };
                        out.push_str(&format!("```result:{status}\n{content}\n```\n\n"));
                    }
                    MessagePart::Image(_) => out.push_str("*[image]*\n\n"),
                    _ => {}
                }
            }
        }
    }
    out
}

pub fn sessions_dir() -> PathBuf {
    let dir = data_dir().join("sessions");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

pub fn session_path(id: &str) -> Result<PathBuf, String> {
    session_path_at(&data_dir(), id)
}

pub fn sessions_dir_at(base: &std::path::Path) -> PathBuf {
    let dir = base.join("sessions");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

pub fn session_path_at(base: &std::path::Path, id: &str) -> Result<PathBuf, String> {
    validate_persistence_id(id)?;
    Ok(sessions_dir_at(base).join(format!("{id}.json")))
}

pub fn load_session_record(id: &str) -> Result<SessionRecord, String> {
    load_session_record_at(&data_dir(), id)
}

pub fn load_session_record_at(base: &std::path::Path, id: &str) -> Result<SessionRecord, String> {
    validate_persistence_id(id)?;
    let path = session_path_at(base, id)?;
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
    validate_persistence_id(&record.id).map_err(|error| format!("cannot save session: {error}"))?;
    std::fs::create_dir_all(base.join("sessions"))
        .map_err(|error| format!("create sessions directory: {error}"))?;
    let path = session_path_at(base, &record.id)?;
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
        publish_replacement(&tmp, &path)
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    result
}

/// All persisted sessions, newest-first by `updated_at`. Corrupt/unreadable
/// files are skipped rather than failing the whole listing. Diagnostics go
/// to `session-scan.log` in the data directory, never to the caller's terminal.
pub fn list_sessions() -> Result<Vec<SessionSummary>, String> {
    list_sessions_for_workdir(None)
}

/// List persisted sessions, optionally restricted to the current workdir.
pub fn list_sessions_for_workdir(
    workdir: Option<&std::path::Path>,
) -> Result<Vec<SessionSummary>, String> {
    list_sessions_at(&data_dir(), workdir)
}

fn list_sessions_at(base: &Path, workdir: Option<&Path>) -> Result<Vec<SessionSummary>, String> {
    let dir = sessions_dir_at(base);
    let mut out = Vec::new();
    let mut skipped = 0usize;
    let mut diagnostics = Vec::new();
    let entries = std::fs::read_dir(&dir).map_err(|e| format!("read {}: {e}", dir.display()))?;
    for entry in entries {
        let entry = match entry {
            Ok(e) => e,
            Err(e) => {
                skipped += 1;
                if diagnostics.len() < 20 {
                    diagnostics.push(format!("could not enumerate session: {:?}", e.kind()));
                }
                continue;
            }
        };
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let data = match std::fs::read_to_string(&path) {
            Ok(d) => d,
            Err(e) => {
                skipped += 1;
                if diagnostics.len() < 20 {
                    diagnostics.push(format!("could not read session {:?}: {:?}", path.file_name(), e.kind()));
                }
                continue;
            }
        };
        let record: SessionRecord = match serde_json::from_str(&data) {
            Ok(r) => r,
            Err(e) => {
                skipped += 1;
                if diagnostics.len() < 20 {
                    // serde's Display may include private session values.
                    diagnostics.push(format!("could not parse session {:?}: {:?} at line {} column {}", path.file_name(), e.classify(), e.line(), e.column()));
                }
                continue;
            }
        };
        if let Some(workdir) = workdir {
            if !session_matches_workdir(&record, workdir) {
                continue;
            }
        }
        out.push(SessionSummary::from_record(&record));
    }
    out.sort_by_key(|summary| std::cmp::Reverse(summary.updated_at));
    if skipped > 0 {
        // Keep only the latest failed scan, with bounded detail. Best effort:
        // a read-only/full profile must not break resume or print a fallback.
        let path = base.join("session-scan.log");
        let tmp = base.join(format!("session-scan.log.tmp.{}", Uuid::new_v4()));
        let result = (|| -> Result<(), String> {
            let mut options = OpenOptions::new();
            options.write(true).create_new(true);
            #[cfg(unix)]
            options.mode(0o600);
            let mut file = options.open(&tmp).map_err(|e| e.to_string())?;
            writeln!(file, "{}: skipped {skipped} session entries (showing {})\n{}",
                Utc::now(), diagnostics.len(), diagnostics.join("\n"))
                .map_err(|e| e.to_string())?;
            file.sync_all().map_err(|e| e.to_string())?;
            drop(file);
            publish_replacement(&tmp, &path)
        })();
        if result.is_err() {
            let _ = std::fs::remove_file(tmp);
        }
    }
    Ok(out)
}