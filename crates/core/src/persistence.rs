use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::Write;
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;
use uuid::Uuid;

use crate::providers::schema::ProviderSchema;
use crate::types::{Context, EffortMode};

// ---------------------------------------------------------------------------
// Auth store (api keys, never committed)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AuthStore {
    pub providers: HashMap<String, ProviderAuth>,
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
    pub history: Context,
}

/// One agent's position in the session's spawn tree.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AgentNodeRecord {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub spawned_via_tool_call_id: Option<String>,
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
    sessions_dir().join(format!("{id}.json"))
}

pub fn load_session_record(id: &str) -> Result<SessionRecord, String> {
    let path = session_path(id);
    let data =
        std::fs::read_to_string(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    serde_json::from_str(&data).map_err(|e| format!("parse {}: {e}", path.display()))
}

pub fn save_session_record(record: &SessionRecord) -> Result<(), String> {
    let path = session_path(&record.id);
    let data = serde_json::to_string_pretty(record)
        .map_err(|e| format!("serialize session {}: {e}", record.id))?;
    std::fs::write(&path, data).map_err(|e| format!("write {}: {e}", path.display()))
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
