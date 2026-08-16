use serde::{Deserialize, Serialize};

use crate::types::ModelInfo;

// ---------------------------------------------------------------------------
// API type
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ApiType {
    OpenAI,
    Anthropic,
}

// ---------------------------------------------------------------------------
// Provider schema (JSON-serializable, no secrets)
// ---------------------------------------------------------------------------

/// A provider definition that can be serialized to `providers.json`.
/// Does NOT contain the API key — that lives in `auth.json`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProviderSchema {
    /// Unique id, e.g. "my-openai", "anthropic-default".
    pub id: String,
    pub api_type: ApiType,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub base_url: Option<String>,
    /// Environment variable that holds the API key at runtime.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub api_key_env: Option<String>,
    /// Statically-defined models. If empty, the provider manager can
    /// try to fetch models dynamically from the API.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub models: Vec<ModelInfo>,
}

impl ProviderSchema {
    /// Default base URL for this API type.
    pub fn default_base_url(&self) -> &str {
        match self.api_type {
            ApiType::OpenAI => "https://api.openai.com/v1",
            ApiType::Anthropic => "https://api.anthropic.com",
        }
    }

    /// Effective base URL (user-specified or default).
    pub fn effective_base_url(&self) -> &str {
        self.base_url.as_deref().unwrap_or(self.default_base_url())
    }

    /// Look up a model by id.
    pub fn model(&self, id: &str) -> Option<&ModelInfo> {
        self.models.iter().find(|m| m.id == id)
    }
}