use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use crate::persistence::{self, AuthStore};
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::providers::{AnthropicProvider, OpenAiProvider, Provider};
use crate::types::ModelInfo;

// ---------------------------------------------------------------------------
// Provider manager
// ---------------------------------------------------------------------------

/// Not a singleton. Create one, load schemas, build providers.
///
/// ```ignore
/// let mut mgr = ProviderManager::new();
/// mgr.load().unwrap();
/// let provider = mgr.build("my-openai").unwrap();
/// ```
#[derive(Clone)]
pub struct ProviderManager {
    schemas: HashMap<String, ProviderSchema>,
    auth: AuthStore,
    data_dir: PathBuf,
}

impl ProviderManager {
    pub fn new() -> Self {
        Self {
            schemas: HashMap::new(),
            auth: AuthStore::default(),
            data_dir: persistence::data_dir(),
        }
    }

    /// Use a custom data directory instead of `~/.firmius`.
    pub fn with_data_dir(mut self, dir: PathBuf) -> Self {
        self.data_dir = dir;
        self
    }

    // ------------------------------------------------------------------
    // Loading / saving
    // ------------------------------------------------------------------

    /// Load `providers.json` and `auth.json` from the data directory.
    /// On first run this is a no-op; register schemas with
    /// [`register_schema`] and save with [`save`].
    pub fn load(&mut self) -> Result<(), String> {
        self.auth = persistence::load_auth()?;
        let schemas = persistence::load_providers()?;
        for s in schemas {
            self.schemas.insert(s.id.clone(), s);
        }
        Ok(())
    }

    /// Save `auth.json` (keys only) and `providers.json` (schemas only).
    pub fn save(&self) -> Result<(), String> {
        persistence::save_auth(&self.auth)?;
        let schemas: Vec<&ProviderSchema> = self.schemas.values().collect();
        persistence::save_providers(&schemas.iter().map(|&s| s.clone()).collect::<Vec<_>>())?;
        Ok(())
    }

    // ------------------------------------------------------------------
    // Registration
    // ------------------------------------------------------------------

    /// Register a provider schema. If the schema has `api_key_env` set
    /// and the env var is present, the key is automatically saved to
    /// the auth store for next launch.
    pub fn register_schema(&mut self, schema: ProviderSchema) {
        // Auto-capture env key if available.
        if let Some(env_var) = &schema.api_key_env
            && let Ok(val) = std::env::var(env_var)
            && !val.is_empty()
            && !self.auth.providers.contains_key(&schema.id)
        {
            self.auth.providers.insert(
                schema.id.clone(),
                persistence::ProviderAuth { api_key: val },
            );
        }
        self.schemas.insert(schema.id.clone(), schema);
    }

    /// Set an API key directly (e.g. from a config dialog).
    pub fn set_api_key(&mut self, provider_id: &str, key: impl Into<String>) {
        self.auth.providers.insert(
            provider_id.to_string(),
            persistence::ProviderAuth {
                api_key: key.into(),
            },
        );
    }

    // ------------------------------------------------------------------
    // Building
    // ------------------------------------------------------------------

    /// Build an `Arc<dyn Provider>` from a registered schema + resolved key.
    pub fn build(&self, provider_id: &str) -> Result<Arc<dyn Provider>, String> {
        let schema = self
            .schemas
            .get(provider_id)
            .ok_or_else(|| format!("unknown provider: {provider_id}"))?;
        let api_key = persistence::resolve_api_key(schema, &self.auth)
            .ok_or_else(|| format!("no API key for {provider_id}"))?;

        let base_url = schema.effective_base_url();

        let provider: Arc<dyn Provider> = match schema.api_type {
            ApiType::OpenAI => Arc::new(OpenAiProvider::new(
                schema.id.clone(),
                base_url.to_string(),
                api_key,
            )),
            ApiType::Anthropic => Arc::new(
                AnthropicProvider::new(schema.id.clone(), api_key)
                    .with_base_url(base_url.to_string()),
            ),
        };
        Ok(provider)
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
        self.schemas
            .values()
            .find_map(|s| s.model(model_id))
    }

    /// Look up model info for a specific provider + model.
    pub fn model_info_for(&self, provider_id: &str, model_id: &str) -> Option<&ModelInfo> {
        self.schema(provider_id)?.model(model_id)
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
