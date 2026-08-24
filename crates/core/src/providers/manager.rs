use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, RwLock};

use crate::kinds::{AccountKind, ApiKeyKind};
use crate::persistence::{self, AccountRecord};
use crate::providers::Provider;
use crate::providers::schema::ProviderSchema;
use crate::quota::{QuotaCapability, QuotaSnapshot, QuotaSource};
use crate::types::{ModelCapability, ModelInfo};

// ---------------------------------------------------------------------------
// Provider manager
// ---------------------------------------------------------------------------

/// Not a singleton. Create one, load accounts, build providers.
///
/// Accounts (schema + credentials, one file each under
/// `<data_dir>/accounts/`) are the unit of persistence; schemas are the
/// in-memory view over them. Providers are built through the account's
/// [`AccountKind`], which interprets the credential blob.
///
/// ```ignore
/// let mut mgr = ProviderManager::new();
/// mgr.load().unwrap();
/// let provider = mgr.build("my-openai").unwrap();
/// ```
#[derive(Clone)]
pub struct ProviderManager {
    schemas: HashMap<String, ProviderSchema>,
    accounts: HashMap<String, AccountRecord>,
    kinds: HashMap<String, Arc<dyn AccountKind>>,
    data_dir: PathBuf,
    account_runtime: Arc<RwLock<HashMap<String, AccountRuntime>>>,
}

#[derive(Clone)]
struct AccountRuntime {
    quota: Option<QuotaSnapshot>,
    unavailable_until: Option<chrono::DateTime<chrono::Utc>>,
}

impl ProviderManager {
    pub fn new() -> Self {
        let mut mgr = Self {
            schemas: HashMap::new(),
            accounts: HashMap::new(),
            kinds: HashMap::new(),
            data_dir: persistence::data_dir(),
            account_runtime: Arc::new(RwLock::new(HashMap::new())),
        };
        mgr.register_kind(Arc::new(ApiKeyKind));
        mgr
    }

    /// Use a custom data directory instead of `~/.firmius`.
    pub fn with_data_dir(mut self, dir: PathBuf) -> Self {
        self.data_dir = dir;
        self
    }

    /// Register a credential family. The `api-key` kind is always present.
    pub fn register_kind(&mut self, kind: Arc<dyn AccountKind>) {
        self.kinds.insert(kind.name().to_string(), kind);
    }

    /// Look up a registered kind by name (e.g. to run its wizard).
    pub fn kind(&self, name: &str) -> Option<Arc<dyn AccountKind>> {
        self.kinds.get(name).cloned()
    }

    /// Names of all registered kinds, sorted — for pickers and completion.
    pub fn kind_names(&self) -> Vec<String> {
        let mut names: Vec<String> = self.kinds.keys().cloned().collect();
        names.sort();
        names
    }

    /// All registered kinds, sorted by name — for pickers that need both
    /// the id and the display label.
    pub fn kinds(&self) -> Vec<Arc<dyn AccountKind>> {
        let mut kinds: Vec<Arc<dyn AccountKind>> = self.kinds.values().cloned().collect();
        kinds.sort_by_key(|kind| kind.name().to_string());
        kinds
    }

    // ------------------------------------------------------------------
    // Loading / saving
    // ------------------------------------------------------------------

    /// Migrate any legacy `auth.json` + `providers.json` into per-account
    /// files, then load every account from `<data_dir>/accounts/`. On first
    /// run this is a no-op; add accounts with [`register_schema`] /
    /// [`register_account`] and persist with [`save`].
    pub fn load(&mut self) -> Result<(), String> {
        persistence::migrate_legacy(&self.data_dir)?;
        for summary in persistence::list_accounts_at(&self.data_dir) {
            match persistence::load_account_at(&self.data_dir, &summary.id) {
                Ok(record) => self.insert_account(record),
                Err(e) => eprintln!("warning: {e}"),
            }
        }
        Ok(())
    }

    /// Persist every in-memory account to its own file.
    pub fn save(&self) -> Result<(), String> {
        for record in self.accounts.values() {
            persistence::save_account_at(&self.data_dir, record)?;
        }
        Ok(())
    }

    /// Persist one account file immediately (e.g. right after a wizard).
    pub fn save_account_file(&self, provider_id: &str) -> Result<(), String> {
        let record = self
            .accounts
            .get(provider_id)
            .ok_or_else(|| format!("unknown provider: {provider_id}"))?;
        persistence::save_account_at(&self.data_dir, record)
    }

    // ------------------------------------------------------------------
    // Registration
    // ------------------------------------------------------------------

    fn insert_account(&mut self, record: AccountRecord) {
        let mut record = record;
        if let Some(kind) = self.kinds.get(&record.kind) {
            record.schema = kind.refresh_schema(&record.schema);
        }
        record.schema.id = record.id.clone();
        self.schemas
            .insert(record.id.clone(), record.schema.clone());
        self.accounts.insert(record.id.clone(), record);
    }

    /// Register (or replace) an account produced by a wizard or by code.
    /// The record id is authoritative: the schema's id is aligned to it.
    pub fn register_account(&mut self, mut record: AccountRecord) {
        record.schema.id = record.id.clone();
        self.insert_account(record);
    }

    /// Register a provider schema as an `api-key` account. If the schema has
    /// `api_key_env` set, the env var is present, and no key is stored yet,
    /// the key is captured into the account for next launch.
    pub fn register_schema(&mut self, schema: ProviderSchema) {
        let mut credentials = self
            .accounts
            .get(&schema.id)
            .map(|a| a.credentials.clone())
            .unwrap_or_else(|| serde_json::json!({}));
        let has_key = credentials
            .get("api_key")
            .and_then(serde_json::Value::as_str)
            .is_some_and(|k| !k.trim().is_empty());
        if !has_key
            && let Some(env_var) = &schema.api_key_env
            && let Ok(val) = std::env::var(env_var)
            && !val.is_empty()
        {
            credentials = serde_json::json!({ "api_key": val });
        }
        self.register_account(AccountRecord {
            id: schema.id.clone(),
            kind: "api-key".to_string(),
            schema,
            credentials,
        });
    }

    /// Set an API key directly (e.g. from a config dialog). No-op for
    /// unknown providers — an account must exist before it can hold a key.
    pub fn set_api_key(&mut self, provider_id: &str, key: impl Into<String>) {
        if let Some(record) = self.accounts.get_mut(provider_id) {
            record.credentials["api_key"] = serde_json::json!(key.into());
        }
    }

    /// The stored account record for an id, if registered.
    pub fn account(&self, provider_id: &str) -> Option<&AccountRecord> {
        self.accounts.get(provider_id)
    }

    /// Return the stable credential kind for a stored account id.
    pub fn provider_kind(&self, provider_id: &str) -> Option<&str> {
        self.accounts
            .get(provider_id)
            .map(|account| account.kind.as_str())
    }

    /// Stored accounts whose credential kind or id matches `provider`.
    pub fn accounts_for(&self, provider: &str) -> Vec<AccountRecord> {
        let mut accounts: Vec<_> = self
            .accounts
            .values()
            .filter(|account| account.kind == provider || account.id == provider)
            .cloned()
            .collect();
        accounts.sort_by(|a, b| a.id.cmp(&b.id));
        accounts
    }

    /// Quota sources whose cached snapshot is missing or stale. The returned
    /// sources are detached from the manager lock and safe to fetch in parallel.
    pub fn account_selection_sources(&self, provider: &str) -> Vec<(String, Arc<dyn QuotaSource>)> {
        let now = chrono::Utc::now();
        let cache = self.account_runtime.read().unwrap();
        self.accounts_for(provider)
            .into_iter()
            .filter(|account| {
                cache.get(&account.id).is_none_or(|entry| {
                    entry.unavailable_until.is_some_and(|until| until > now)
                        || entry
                            .quota
                            .as_ref()
                            .is_none_or(|snapshot| (now - snapshot.observed_at).num_seconds() >= 60)
                })
            })
            .filter_map(|account| {
                self.quota_capability(&account.id)
                    .ok()
                    .flatten()
                    .and_then(|capability| capability.source)
                    .map(|source| (account.id, source))
            })
            .collect()
    }

    pub fn cache_account_quota(&self, account_id: &str, snapshot: QuotaSnapshot) {
        let mut cache = self.account_runtime.write().unwrap();
        let entry = cache
            .entry(account_id.to_string())
            .or_insert(AccountRuntime {
                quota: None,
                unavailable_until: None,
            });
        entry.quota = Some(snapshot);
        entry.unavailable_until = None;
    }

    /// Temporarily remove an account from first-attempt selection after a
    /// quota/auth wall. A later successful quota refresh makes it eligible.
    pub fn mark_account_unavailable(&self, account_id: &str, until: chrono::DateTime<chrono::Utc>) {
        let mut cache = self.account_runtime.write().unwrap();
        let entry = cache
            .entry(account_id.to_string())
            .or_insert(AccountRuntime {
                quota: None,
                unavailable_until: None,
            });
        entry.unavailable_until = Some(until);
    }

    /// Rank sibling accounts for a credential kind. Known exhausted accounts
    /// are omitted. Accounts with fresh comparable quota sort before unknowns.
    pub fn ranked_accounts_for(&self, provider_id: &str) -> Vec<String> {
        let kind_name = self.provider_kind(provider_id).unwrap_or(provider_id);
        let meter = self
            .kinds
            .get(kind_name)
            .and_then(|kind| kind.account_selection_meter());
        let now = chrono::Utc::now();
        let cache = self.account_runtime.read().unwrap();
        let mut ranked = self
            .accounts_for(kind_name)
            .into_iter()
            .filter_map(|account| {
                let runtime = cache.get(&account.id);
                if runtime
                    .and_then(|entry| entry.unavailable_until)
                    .is_some_and(|until| until > now)
                {
                    return None;
                }
                let utilization = meter.and_then(|meter_id| {
                    runtime
                        .and_then(|entry| entry.quota.as_ref())
                        .and_then(|snapshot| {
                            snapshot.meters.iter().find(|value| value.id == meter_id)
                        })
                        .and_then(|value| value.utilization_percent)
                });
                if utilization.is_some_and(|value| value >= 100.0) {
                    return None;
                }
                Some((account.id, utilization))
            })
            .collect::<Vec<_>>();
        ranked.sort_by(|a, b| match (a.1, b.1) {
            (Some(a_score), Some(b_score)) => {
                a_score.total_cmp(&b_score).then_with(|| a.0.cmp(&b.0))
            }
            (Some(_), None) => std::cmp::Ordering::Less,
            (None, Some(_)) => std::cmp::Ordering::Greater,
            (None, None) => (a.0 != provider_id)
                .cmp(&(b.0 != provider_id))
                .then_with(|| a.0.cmp(&b.0)),
        });
        ranked.into_iter().map(|(id, _)| id).collect()
    }

    pub fn has_account_selection_policy(&self, provider_id: &str) -> bool {
        self.provider_kind(provider_id)
            .and_then(|kind| self.kinds.get(kind))
            .and_then(|kind| kind.account_selection_meter())
            .is_some()
    }

    /// Return the optional quota capability for one stored account.
    pub fn quota_capability(&self, account_id: &str) -> Result<Option<QuotaCapability>, String> {
        let account = self
            .accounts
            .get(account_id)
            .ok_or_else(|| format!("unknown provider: {account_id}"))?;
        let kind = self
            .kinds
            .get(&account.kind)
            .ok_or_else(|| format!("unknown account kind '{}' for {account_id}", account.kind))?;
        kind.quota_capability_at(&account.schema, &account.credentials, &self.data_dir)
    }

    // ------------------------------------------------------------------
    // Building
    // ------------------------------------------------------------------

    /// Build an `Arc<dyn Provider>` by routing the account through its kind.
    pub fn build(&self, provider_id: &str) -> Result<Arc<dyn Provider>, String> {
        let account = self
            .accounts
            .get(provider_id)
            .ok_or_else(|| format!("unknown provider: {provider_id}"))?;
        let kind = self
            .kinds
            .get(&account.kind)
            .ok_or_else(|| format!("unknown account kind '{}' for {provider_id}", account.kind))?;
        kind.build_provider_at(&account.schema, &account.credentials, &self.data_dir)
    }

    // ------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------

    /// All registered provider ids.
    pub fn provider_ids(&self) -> Vec<&str> {
        self.schemas.keys().map(|s| s.as_str()).collect()
    }

    /// Get a schema by id.
    pub fn schema(&self, provider_id: &str) -> Option<&ProviderSchema> {
        self.schemas.get(provider_id)
    }

    /// Look up model info for a specific model, searching all providers.
    pub fn model_info(&self, model_id: &str) -> Option<&ModelInfo> {
        self.schemas.values().find_map(|s| s.model(model_id))
    }

    /// Look up model info for a specific provider + model.
    pub fn model_info_for(&self, provider_id: &str, model_id: &str) -> Option<&ModelInfo> {
        self.schema(provider_id)?.model(model_id)
    }

    /// Whether a specific provider model advertises one capability.
    pub fn model_supports(
        &self,
        provider_id: &str,
        model_id: &str,
        capability: ModelCapability,
    ) -> bool {
        self.model_info_for(provider_id, model_id)
            .is_some_and(|model| model.supports(capability))
    }

    /// All configured `(provider_id, model_id)` pairs that advertise one
    /// capability. Useful for future routing and picker filtering.
    pub fn model_choices_with_capability(
        &self,
        capability: ModelCapability,
    ) -> Vec<(String, String)> {
        let mut choices = self
            .schemas
            .values()
            .flat_map(|schema| {
                schema.models.iter().filter_map(|model| {
                    model
                        .supports(capability)
                        .then(|| (schema.id.clone(), model.id.clone()))
                })
            })
            .collect::<Vec<_>>();
        choices.sort();
        choices
    }

    /// Resolve a user-facing kind/model pair to a configured account.
    pub fn account_for_model(&self, kind: &str, model: &str) -> Option<(String, String)> {
        self.model_choices_by_kind()
            .into_iter()
            .find(|(_, choice_kind, choice_model)| choice_kind == kind && choice_model == model)
            .map(|(account, _, model)| (account, model))
    }

    /// Model choices grouped by credential kind for user-facing pickers.
    ///
    /// Account ids are implementation details (and may contain generated
    /// suffixes).  Keep the account id alongside the display pair so callers
    /// can still route a selected model to the correct account, while showing
    /// one choice per kind/model rather than one choice per account.
    pub fn model_choices_by_kind(&self) -> Vec<(String, String, String)> {
        let mut choices = self
            .schemas
            .values()
            .flat_map(|schema| {
                let account_id = schema.id.clone();
                let kind = self
                    .accounts
                    .get(&account_id)
                    .map(|account| account.kind.clone())
                    .unwrap_or_else(|| account_id.clone());
                let kind = (kind == "api-key")
                    .then(|| account_id.clone())
                    .unwrap_or(kind);
                schema
                    .models
                    .iter()
                    .map(move |model| (account_id.clone(), kind.clone(), model.id.clone()))
            })
            .collect::<Vec<_>>();
        choices.sort_by(|a, b| (&a.1, &a.2, &a.0).cmp(&(&b.1, &b.2, &b.0)));
        choices.dedup_by(|a, b| a.1 == b.1 && a.2 == b.2);
        choices
    }

    /// All statically configured model ids for a provider, in schema order.
    /// Dynamic providers may return an empty list until their models are
    /// fetched, so callers should treat this as completion metadata rather
    /// than an exhaustive discovery API.
    pub fn model_ids(&self, provider_id: &str) -> Vec<String> {
        self.schema(provider_id)
            .map(|schema| schema.models.iter().map(|model| model.id.clone()).collect())
            .unwrap_or_default()
    }

    /// All statically configured `(provider_id, model_id)` choices. This is
    /// intended for model pickers that need to show the provider alongside a
    /// model name.
    pub fn model_choices(&self) -> Vec<(String, String)> {
        let mut choices = self
            .schemas
            .values()
            .flat_map(|schema| {
                schema
                    .models
                    .iter()
                    .map(|model| (schema.id.clone(), model.id.clone()))
            })
            .collect::<Vec<_>>();
        choices.sort();
        choices
    }

    /// Get the current input token usage for a model (helpful for
    /// context-window checks). Returns `None` if the model is unknown.
    pub fn context_window(&self, model_id: &str) -> Option<u32> {
        self.model_info(model_id).map(|m| m.context_window)
    }
}

impl Default for ProviderManager {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::kinds::CodexKind;
    use crate::providers::schema::ApiType;
    use crate::quota::QuotaMeter;

    fn codex_account(id: &str) -> AccountRecord {
        AccountRecord {
            id: id.into(),
            kind: "codex".into(),
            schema: ProviderSchema {
                id: id.into(),
                api_type: ApiType::OpenAI,
                base_url: None,
                api_key_env: None,
                models: Vec::new(),
            },
            credentials: serde_json::json!({}),
        }
    }

    fn quota(account_id: &str, weekly: f64) -> QuotaSnapshot {
        QuotaSnapshot {
            account_id: account_id.into(),
            observed_at: chrono::Utc::now(),
            meters: vec![QuotaMeter {
                id: "7d".into(),
                label: "Weekly".into(),
                window: Some("7d".into()),
                used: None,
                limit: None,
                remaining: None,
                utilization_percent: Some(weekly),
                unit: Some("percent".into()),
                reset_at: None,
                reset_in_seconds: None,
            }],
            note: None,
        }
    }

    #[test]
    fn codex_ranking_prefers_lowest_weekly_utilization_and_fails_closed() {
        let mut manager = ProviderManager::new();
        manager.register_kind(Arc::new(CodexKind));
        manager.register_account(codex_account("codex-a"));
        manager.register_account(codex_account("codex-b"));
        manager.cache_account_quota("codex-a", quota("codex-a", 80.0));
        manager.cache_account_quota("codex-b", quota("codex-b", 20.0));

        assert_eq!(
            manager.ranked_accounts_for("codex-a"),
            ["codex-b", "codex-a"]
        );
        manager
            .mark_account_unavailable("codex-b", chrono::Utc::now() + chrono::Duration::minutes(5));
        assert_eq!(manager.ranked_accounts_for("codex-a"), ["codex-a"]);
        manager
            .mark_account_unavailable("codex-a", chrono::Utc::now() + chrono::Duration::minutes(5));
        assert!(manager.ranked_accounts_for("codex-a").is_empty());
    }
}
