//! OpenCode Go — subscription gateway, API-key auth, one-step setup.
//! Facts extracted from the `pi` reference implementation and models.dev:
//! base URL `https://opencode.ai/zen/go/v1`, bearer auth via
//! `OPENCODE_API_KEY`, OpenAI chat-completions wire format.

use super::{AccountKind, api_key::build_api_key_provider, effort_modes, model};
use crate::Provider;
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::types::ModelInfo;
use crate::wizard::{Outcome, SetupWizard, Step, WizardError};
use async_trait::async_trait;
use serde_json::Value;
use std::sync::Arc;

pub const OPENCODE_GO_BASE_URL: &str = "https://opencode.ai/zen/go/v1";
pub const OPENCODE_GO_API_KEY_ENV: &str = "OPENCODE_API_KEY";

/// Static model table (models.dev `opencode-go`, captured 2026).
fn models() -> Vec<ModelInfo> {
    let mut models = vec![
        model("kimi-k2.7-code", 262144, 262144),
        model("qwen3.7-max", 1000000, 65536),
        model("kimi-k3", 1048576, 131072),
        model("deepseek-v4-flash", 1000000, 384000),
        model("mimo-v2.5", 1000000, 128000),
        model("grok-4.5", 500000, 500000),
        model("deepseek-v4-pro", 1000000, 384000),
        model("qwen3.5-plus", 262144, 65536),
        model("gpt-5.6-luna", 1050000, 128000),
        model("glm-5", 202752, 32768),
        model("minimax-m3", 1000000, 131072),
        model("minimax-m2.7", 204800, 131072),
        model("qwen3.8-max", 1000000, 131072),
        model("mimo-v2-pro", 1048576, 128000),
        model("qwen3.7-plus", 1000000, 65536),
        model("glm-5.3", 1000000, 131072),
        model("kimi-k2.5", 262144, 65536),
        model("glm-5.2", 1000000, 131072),
        model("minimax-m2.5", 204800, 65536),
        model("mimo-v2-omni", 262144, 128000),
        model("qwen3.6-plus", 1000000, 65536),
        model("glm-5.1", 202752, 32768),
        model("mimo-v2.5-pro", 1048576, 128000),
        model("hy3", 256000, 64000),
        model("kimi-k2.6", 262144, 65536),
    ];
    for info in &mut models {
        info.effort_modes = match info.id.as_str() {
            "kimi-k3" => effort_modes(&["max"]),
            "deepseek-v4-flash" => effort_modes(&["low", "high", "max"]),
            "grok-4.5" => effort_modes(&["low", "medium", "high"]),
            "deepseek-v4-pro" => effort_modes(&["high", "max"]),
            "gpt-5.6-luna" => effort_modes(&["none", "low", "medium", "high", "xhigh", "max"]),
            "glm-5.3" => effort_modes(&["low", "high", "max"]),
            "glm-5.2" => effort_modes(&["high", "max"]),
            "hy3" => effort_modes(&["none", "low", "high"]),
            _ => Vec::new(),
        };
    }
    models
}

/// The completed schema for an OpenCode Go account.
pub fn schema_template() -> ProviderSchema {
    ProviderSchema {
        id: "opencode-go".to_string(),
        api_type: ApiType::OpenAI,
        base_url: Some(OPENCODE_GO_BASE_URL.to_string()),
        api_key_env: Some(OPENCODE_GO_API_KEY_ENV.to_string()),
        models: models(),
    }
}

pub struct OpencodeGoKind;

impl AccountKind for OpencodeGoKind {
    fn name(&self) -> &str {
        "opencode-go"
    }
    fn display_name(&self) -> &str {
        "OpenCode Go"
    }
    fn build_provider(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Arc<dyn Provider>, String> {
        build_api_key_provider(schema, credentials)
    }

    fn refresh_schema(&self, schema: &ProviderSchema) -> ProviderSchema {
        let mut refreshed = schema_template();
        refreshed.id = schema.id.clone();
        if schema.base_url.is_some() {
            refreshed.base_url = schema.base_url.clone();
        }
        refreshed
    }
    fn wizard(&self) -> Box<dyn SetupWizard> {
        Box::new(OpencodeGoWizard)
    }
}

/// One step: the key. The schema is fixed by the kind.
pub struct OpencodeGoWizard;

#[async_trait]
impl SetupWizard for OpencodeGoWizard {
    async fn start(&mut self) -> Step {
        Step::Prompt {
            label: "OpenCode Go API key".to_string(),
            secret: true,
        }
    }

    async fn answer(&mut self, input: String) -> Result<Outcome, WizardError> {
        let key = input.trim().to_string();
        if key.is_empty() {
            return Err(WizardError::InvalidAnswer(
                "api key must not be empty".into(),
            ));
        }
        Ok(Outcome::Done {
            schema: schema_template(),
            credentials: serde_json::json!({ "api_key": key }),
        })
    }
}
