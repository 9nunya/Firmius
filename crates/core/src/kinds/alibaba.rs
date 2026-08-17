//! Alibaba Model Studio Token Plan (Qwen) — subscription, API-key auth,
//! two-step setup: a region choice that fixes the base URL, then the key.
//! Facts extracted from the `pi` reference implementation and models.dev
//! (`alibaba-token-plan[-cn]`): OpenAI-compatible endpoints under
//! `token-plan.<region>.maas.aliyuncs.com/compatible-mode/v1`; China uses
//! separate keys (`sk-sp-` prefix).

use super::{AccountKind, api_key::build_api_key_provider, effort_modes, model};
use crate::Provider;
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::types::ModelInfo;
use crate::wizard::{Outcome, SelectOption, SetupWizard, Step, WizardError, match_select};
use async_trait::async_trait;
use serde_json::Value;
use std::sync::Arc;

pub const ALIBABA_INTL_BASE_URL: &str =
    "https://token-plan.ap-southeast-1.maas.aliyuncs.com/compatible-mode/v1";
pub const ALIBABA_CN_BASE_URL: &str =
    "https://token-plan.cn-beijing.maas.aliyuncs.com/compatible-mode/v1";
pub const ALIBABA_API_KEY_ENV: &str = "ALIBABA_TOKEN_PLAN_API_KEY";

pub const REGION_INTERNATIONAL: &str = "international";
pub const REGION_CHINA: &str = "china";

/// Static model table (models.dev `alibaba-token-plan`, filtered the way
/// `pi` does: tool-call-capable models only, retired preview excluded).
/// Both regions expose the same catalog.
fn models() -> Vec<ModelInfo> {
    let mut models = vec![
        model("kimi-k2.7-code", 262144, 262144),
        model("qwen3.7-max", 1000000, 131072),
        model("deepseek-v4-flash", 1000000, 384000),
        model("deepseek-v4-pro-0813", 1000000, 384000),
        model("deepseek-v4-pro", 1000000, 384000),
        model("deepseek-v3.2", 131072, 65536),
        model("glm-5", 202752, 16384),
        model("qwen3.8-max", 1000000, 131072),
        model("qwen3.7-plus", 1000000, 65536),
        model("kimi-k2.5", 262144, 98304),
        model("glm-5.2", 1000000, 131072),
        model("qwen3.6-plus", 1000000, 65536),
        model("glm-5.1", 202752, 128000),
        model("MiniMax-M2.5", 196608, 32768),
        model("deepseek-v4-flash-0731", 1000000, 384000),
        model("qwen3.6-flash", 1000000, 65536),
        model("kimi-k2.6", 262144, 262144),
    ];
    for info in &mut models {
        info.effort_modes = match info.id.as_str() {
            "qwen3.8-max" => effort_modes(&["low", "medium", "xhigh"]),
            "deepseek-v4-flash" | "deepseek-v4-pro" => effort_modes(&["high", "max"]),
            "glm-5.2" => {
                effort_modes(&["none", "minimal", "low", "medium", "high", "xhigh", "max"])
            }
            "deepseek-v4-flash-0731" => effort_modes(&["high", "max"]),
            _ => Vec::new(),
        };
    }
    models
}

fn base_url_for(region: &str) -> &'static str {
    match region {
        REGION_CHINA => ALIBABA_CN_BASE_URL,
        _ => ALIBABA_INTL_BASE_URL,
    }
}

/// The completed schema for one region of the token plan.
pub fn schema_template(region: &str) -> ProviderSchema {
    ProviderSchema {
        id: "alibaba-token-plan".to_string(),
        api_type: ApiType::OpenAI,
        base_url: Some(base_url_for(region).to_string()),
        api_key_env: Some(ALIBABA_API_KEY_ENV.to_string()),
        models: models(),
    }
}

pub struct AlibabaTokenPlanKind;

impl AccountKind for AlibabaTokenPlanKind {
    fn name(&self) -> &str {
        "alibaba-token-plan"
    }
    fn display_name(&self) -> &str {
        "Alibaba Model Studio (Token Plan)"
    }
    fn build_provider(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Arc<dyn Provider>, String> {
        build_api_key_provider(schema, credentials)
    }

    fn refresh_schema(&self, schema: &ProviderSchema) -> ProviderSchema {
        let region = schema
            .base_url
            .as_deref()
            .filter(|url| url.contains("cn-beijing"))
            .map(|_| REGION_CHINA)
            .unwrap_or(REGION_INTERNATIONAL);
        let mut refreshed = schema_template(region);
        refreshed.id = schema.id.clone();
        if schema.base_url.is_some() {
            refreshed.base_url = schema.base_url.clone();
        }
        refreshed
    }
    fn wizard(&self) -> Box<dyn SetupWizard> {
        Box::new(AlibabaTokenPlanWizard::new())
    }
}

/// Two steps: region (constrained select) then API key. The region decides
/// the base URL and is persisted alongside the key so the account stays
/// self-describing.
pub struct AlibabaTokenPlanWizard {
    current: Step,
    region: Option<String>,
}

impl AlibabaTokenPlanWizard {
    pub fn new() -> Self {
        Self {
            current: Self::region_step(),
            region: None,
        }
    }

    fn region_step() -> Step {
        Step::Select {
            label: "Region".to_string(),
            options: vec![
                SelectOption::new(REGION_INTERNATIONAL, "International"),
                SelectOption::new(REGION_CHINA, "China"),
            ],
        }
    }
}

impl Default for AlibabaTokenPlanWizard {
    fn default() -> Self {
        Self::new()
    }
}

#[async_trait]
impl SetupWizard for AlibabaTokenPlanWizard {
    async fn start(&mut self) -> Step {
        self.current.clone()
    }

    async fn answer(&mut self, input: String) -> Result<Outcome, WizardError> {
        match &self.current {
            Step::Select { .. } => {
                let region = match_select(&self.current, &input)?;
                self.region = Some(region.clone());
                self.current = Step::Prompt {
                    label: if region == REGION_CHINA {
                        "Alibaba Token Plan API key (sk-sp-…)".to_string()
                    } else {
                        "Alibaba Token Plan API key".to_string()
                    },
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
                let region = self
                    .region
                    .clone()
                    .expect("region collected before the key step");
                Ok(Outcome::Done {
                    schema: schema_template(&region),
                    credentials: serde_json::json!({
                        "api_key": key,
                        "region": region,
                    }),
                })
            }
            Step::OpenUrl { .. } => Err(WizardError::InvalidAnswer(
                "Alibaba wizard has no browser step".into(),
            )),
        }
    }
}
