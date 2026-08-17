//! The generic API-key kind: any OpenAI- or Anthropic-compatible endpoint
//! plus one key. Subscription products that happen to be key-based build on
//! the same [`build_api_key_provider`] with a fixed schema instead.

use super::AccountKind;
use crate::Provider;
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::providers::{AnthropicProvider, OpenAiProvider, StaticToken};
use crate::types::ModelInfo;
use crate::wizard::{Outcome, SelectOption, SetupWizard, Step, WizardError, match_select};
use async_trait::async_trait;
use serde_json::Value;
use std::sync::Arc;

/// Resolve the key (stored credential first, then the schema's env var) and
/// build the matching wire provider. Shared by every key-based kind.
pub fn build_api_key_provider(
    schema: &ProviderSchema,
    credentials: &Value,
) -> Result<Arc<dyn Provider>, String> {
    let stored = credentials
        .get("api_key")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(String::from);
    let from_env = schema
        .api_key_env
        .as_ref()
        .and_then(|var| std::env::var(var).ok())
        .filter(|val| !val.is_empty());
    let Some(key) = stored.or(from_env) else {
        let hint = schema
            .api_key_env
            .as_deref()
            .map(|var| format!(" (or set {var})"))
            .unwrap_or_default();
        return Err(format!("no API key for {}{hint}", schema.id));
    };

    let base_url = schema.effective_base_url().to_string();
    Ok(match schema.api_type {
        ApiType::OpenAI => Arc::new(OpenAiProvider::with_auth(
            schema.id.clone(),
            base_url,
            Arc::new(StaticToken::bearer(key)),
        )),
        ApiType::Anthropic => Arc::new(
            AnthropicProvider::with_auth(schema.id.clone(), Arc::new(StaticToken::x_api_key(key)))
                .with_base_url(base_url),
        ),
    })
}

/// The generic API-key kind — endpoints and keys supplied by the user.
pub struct ApiKeyKind;

impl AccountKind for ApiKeyKind {
    fn name(&self) -> &str {
        "api-key"
    }
    fn display_name(&self) -> &str {
        "API key"
    }
    fn build_provider(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Arc<dyn Provider>, String> {
        build_api_key_provider(schema, credentials)
    }
    fn wizard(&self) -> Box<dyn SetupWizard> {
        Box::new(GenericApiKeyWizard::new())
    }
}

/// Four steps: account id, API type, base URL (blank for the default), key.
pub struct GenericApiKeyWizard {
    current: Step,
    id: String,
    api_type: ApiType,
    base_url: Option<String>,
}

impl GenericApiKeyWizard {
    pub fn new() -> Self {
        Self {
            current: Step::Prompt {
                label: "Account id".to_string(),
                secret: false,
            },
            id: String::new(),
            api_type: ApiType::OpenAI,
            base_url: None,
        }
    }

    fn api_type_step() -> Step {
        Step::Select {
            label: "API type".to_string(),
            options: vec![
                SelectOption::new("openai", "OpenAI-compatible"),
                SelectOption::new("anthropic", "Anthropic Messages"),
            ],
        }
    }
}

impl Default for GenericApiKeyWizard {
    fn default() -> Self {
        Self::new()
    }
}

#[async_trait]
impl SetupWizard for GenericApiKeyWizard {
    async fn start(&mut self) -> Step {
        self.current.clone()
    }

    async fn answer(&mut self, input: String) -> Result<Outcome, WizardError> {
        match &self.current {
            Step::Prompt { label, .. } if label == "Account id" => {
                let id = input.trim().to_string();
                if id.is_empty() {
                    return Err(WizardError::InvalidAnswer(
                        "account id must not be empty".into(),
                    ));
                }
                self.id = id;
                self.current = Self::api_type_step();
                Ok(Outcome::Next(self.current.clone()))
            }
            Step::Select { .. } => {
                let value = match_select(&self.current, &input)?;
                self.api_type = match value.as_str() {
                    "anthropic" => ApiType::Anthropic,
                    _ => ApiType::OpenAI,
                };
                self.current = Step::Prompt {
                    label: "Base URL (blank for default)".to_string(),
                    secret: false,
                };
                Ok(Outcome::Next(self.current.clone()))
            }
            Step::Prompt { label, .. } if label.starts_with("Base URL") => {
                let trimmed = input.trim();
                self.base_url = if trimmed.is_empty() {
                    None
                } else {
                    Some(trimmed.to_string())
                };
                self.current = Step::Prompt {
                    label: "API key".to_string(),
                    secret: true,
                };
                Ok(Outcome::Next(self.current.clone()))
            }
            Step::Prompt { .. } => {
                let key = input.trim().to_string();
                if key.is_empty() {
                    return Err(WizardError::InvalidAnswer(
                        "api key must not be empty".into(),
                    ));
                }
                let schema = ProviderSchema {
                    id: self.id.clone(),
                    api_type: self.api_type,
                    base_url: self.base_url.clone(),
                    api_key_env: None,
                    models: Vec::<ModelInfo>::new(),
                };
                Ok(Outcome::Done {
                    schema,
                    credentials: serde_json::json!({ "api_key": key }),
                })
            }
            Step::OpenUrl { .. } => Err(WizardError::InvalidAnswer(
                "API key wizard has no browser step".into(),
            )),
        }
    }
}
