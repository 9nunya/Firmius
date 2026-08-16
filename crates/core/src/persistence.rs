use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;

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
    let path = auth_path();
    if !path.exists() {
        return Ok(AuthStore::default());
    }
    let data = std::fs::read_to_string(&path)
        .map_err(|e| format!("read {}: {e}", path.display()))?;
    serde_json::from_str(&data).map_err(|e| format!("parse {}: {e}", path.display()))
}

pub fn save_auth(auth: &AuthStore) -> Result<(), String> {
    let path = auth_path();
    let data =
        serde_json::to_string_pretty(auth).map_err(|e| format!("serialize auth: {e}"))?;
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
// Providers
// ---------------------------------------------------------------------------

pub fn providers_path() -> PathBuf {
    data_dir().join("providers.json")
}

pub fn load_providers() -> Result<Vec<ProviderSchema>, String> {
    let path = providers_path();
    if !path.exists() {
        return Ok(Vec::new());
    }
    let data = std::fs::read_to_string(&path)
        .map_err(|e| format!("read {}: {e}", path.display()))?;
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
    pub temperature: Option<f32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_tokens: Option<u32>,
    pub max_turns: u32,
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
    let data = std::fs::read_to_string(&path)
        .map_err(|e| format!("read {}: {e}", path.display()))?;
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
