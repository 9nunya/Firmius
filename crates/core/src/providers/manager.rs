use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use crate::kinds::{AccountKind, ApiKeyKind};
use crate::persistence::{self, AccountRecord};
use crate::providers::Provider;
use crate::providers::schema::ProviderSchema;
use crate::quota::QuotaCapability;
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
}

impl ProviderManager {
    pub fn new() -> Self {
        let mut mgr = Self {
            schemas: HashMap::new(),
            accounts: HashMap::new(),
            kinds: HashMap::new(),
            data_dir: persistence::data_dir(),
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
