//! Account kinds: credential families and their provider factories.
//!
//! A *kind* knows three things: how to interpret an account's credential
//! blob into a live [`Provider`], what to call itself, and which
//! [`SetupWizard`] collects those credentials for a new account. Storage is
//! kind-agnostic (adjacently tagged JSON, one file per account); adding a
//! new credential family — OAuth next — is a new module here plus a
//! registration, nothing else.

use crate::providers::schema::ProviderSchema;
use crate::types::{EffortMode, ModelCapabilities, ModelInfo};
use crate::wizard::SetupWizard;
use crate::{Provider, QuotaCapability};
use serde_json::Value;
use std::path::Path;
use std::sync::Arc;

pub mod alibaba;
pub mod anthropic_subscription;
pub mod api_key;
pub mod cline_pass;
pub mod codex;
pub mod opencode_go;

pub use alibaba::AlibabaTokenPlanKind;
pub use anthropic_subscription::AnthropicSubscriptionKind;
pub use api_key::{ApiKeyKind, GenericApiKeyWizard};
pub use cline_pass::ClinePassKind;
pub use codex::CodexKind;
pub use opencode_go::OpencodeGoKind;

/// A credential family: interprets stored credentials into providers and
/// supplies the wizard that collects them.
pub trait AccountKind: Send + Sync {
    /// Stable id persisted in account records, e.g. `"api-key"`.
    fn name(&self) -> &str;
    /// Human-facing name for pickers and help text.
    fn display_name(&self) -> &str;
    /// Build a live provider from a stored schema + credential blob.
    /// Credential *shape* is validated here; deeper validity (does the key
    /// actually work) is left to first contact with the API.
    fn build_provider(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Arc<dyn Provider>, String>;
    /// Build a live provider with access to the manager's data directory.
    /// Existing account kinds can continue implementing [`build_provider`];
    /// context-aware kinds may override this method.
    fn build_provider_at(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
        _data_dir: &Path,
    ) -> Result<Arc<dyn Provider>, String> {
        self.build_provider(schema, credentials)
    }
    /// Refresh static model metadata while preserving account-specific
    /// endpoint and identity fields. Persisted accounts may outlive the
    /// catalog version that created them.
    fn refresh_schema(&self, schema: &ProviderSchema) -> ProviderSchema {
        schema.clone()
    }
    /// The setup flow for a new account of this kind.
    fn wizard(&self) -> Box<dyn SetupWizard>;
    /// Optional subscription quota capability for this account kind.
    fn quota_capability(
        &self,
        _schema: &ProviderSchema,
        _credentials: &Value,
    ) -> Result<Option<QuotaCapability>, String> {
        Ok(None)
    }
    /// Optional subscription quota capability with access to the manager's data
    /// directory. Existing account kinds can continue implementing
    /// [`quota_capability`]; context-aware kinds may override this method.
    fn quota_capability_at(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
        _data_dir: &Path,
    ) -> Result<Option<QuotaCapability>, String> {
        self.quota_capability(schema, credentials)
    }
}

/// Shared builder for statically-known models: text + tools + reasoning,
/// no effort modes until a gateway's effort semantics are verified.
pub(crate) fn model(id: &str, context_window: u32, max_output_tokens: u32) -> ModelInfo {
    ModelInfo {
        id: id.to_string(),
        context_window,
        max_output_tokens: Some(max_output_tokens),
        capabilities: ModelCapabilities {
            text: true,
            image: false,
            video: false,
            pdf: false,
            audio: false,
            tool_use: true,
            reasoning: true,
        },
        effort_modes: Vec::new(),
    }
}

/// Convert the `reasoning_options` effort values from models.dev into the
/// provider's normalized model metadata. Toggle and budget-token options are
/// intentionally not represented here because `/effort` selects named effort
/// values only.
pub(crate) fn effort_modes(names: &[&str]) -> Vec<EffortMode> {
    names
        .iter()
        .map(|name| EffortMode {
            name: (*name).to_string(),
            thinking_budget_tokens: None,
            reasoning_effort: Some((*name).to_string()),
        })
        .collect()
}
