//! ClinePass: the Cline subscription gateway using an API key.

use super::{AccountKind, api_key::build_api_key_provider, model};
use crate::Provider;
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::quota::{
    QuotaAuth, QuotaCapability, QuotaDescriptor, QuotaError, QuotaMeter, QuotaSnapshot, QuotaSource,
};
use crate::types::ModelInfo;
use crate::wizard::{Outcome, SetupWizard, Step, WizardError};
use async_trait::async_trait;
use chrono::{DateTime, Duration, Utc};
use serde_json::Value;
use std::sync::Arc;

pub const CLINE_PASS_BASE_URL: &str = "https://api.cline.bot/api/v1";
pub const CLINE_PASS_API_KEY_ENV: &str = "CLINE_API_KEY";
const USERS_ME: &str = "/users/me";
const RECOMMENDED_MODELS: &str = "/ai/cline/recommended-models";

/// The list is intentionally local rather than fetched from the gateway: it
/// keeps account setup usable when the models endpoint is unavailable.
pub fn models() -> Vec<ModelInfo> {
    [
        ("kimi-k2.7-code", 262_144, 262_144),
        ("qwen3.7-max", 1_000_000, 65_536),
        ("kimi-k3", 1_000_000, 131_072),
        ("deepseek-v4-pro", 1_000_000, 384_000),
        ("deepseek-v4-flash", 1_000_000, 384_000),
        ("mimo-v2.5", 1_000_000, 128_000),
        ("mimo-v2.5-pro", 1_048_576, 128_000),
        ("minimax-m3", 1_000_000, 131_072),
        ("qwen3.7-plus", 1_000_000, 65_536),
        ("glm-5.2", 1_000_000, 131_072),
        ("kimi-k2.6", 262_144, 262_144),
    ]
    .into_iter()
    .map(|(id, context, output)| {
        let mut info = model(&format!("cline-pass/{id}"), context, output);
        info.effort_modes = super::effort_modes(&["none", "low", "medium", "high", "xhigh"]);
        info
    })
    .collect()
}

fn parse_live_models(body: &Value) -> Vec<ModelInfo> {
    let payload = body.get("data").unwrap_or(body);
    let mut result = Vec::new();
    if let Some(entries) = payload.get("clinePass").and_then(Value::as_array) {
        for entry in entries {
            let Some(id) = entry.get("id").and_then(Value::as_str) else {
                continue;
            };
            let id = if id.contains('/') {
                id.to_owned()
            } else {
                format!("cline-pass/{id}")
            };
            let mut info = model(&id, 128_000, 8_192);
            info.effort_modes = super::effort_modes(&["none", "low", "medium", "high", "xhigh"]);
            result.push(info);
        }
    }

    result.dedup_by(|a, b| a.id == b.id);
    result
}

/// Fetch the current Cline model catalogue.  This is deliberately separate
/// from [`schema_template`]: setup and provider registration are synchronous,
/// while callers which can afford a network request can refresh the catalogue.
pub async fn fetch_live_models(_api_key: &str) -> Result<Vec<ModelInfo>, String> {
    let response = reqwest::Client::new()
        .get(format!("{CLINE_PASS_BASE_URL}{RECOMMENDED_MODELS}"))
        .send()
        .await
        .map_err(|e| e.to_string())?;
    let status = response.status();
    let body: Value = response.json().await.map_err(|e| e.to_string())?;
    if !status.is_success() {
        return Err(format!("HTTP {status}"));
    }

    let result = parse_live_models(&body);
    if result.is_empty() {
        return Err("recommended-models response contained no model ids".into());
    }
    Ok(result)
}

pub fn schema_template() -> ProviderSchema {
    ProviderSchema {
        id: "cline-pass".into(),
        api_type: ApiType::OpenAI,
        base_url: Some(CLINE_PASS_BASE_URL.into()),
        api_key_env: Some(CLINE_PASS_API_KEY_ENV.into()),
        models: models(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_has_fixed_cline_pass_models_and_key_env() {
        let schema = schema_template();
        assert_eq!(schema.id, "cline-pass");
        assert_eq!(schema.base_url.as_deref(), Some(CLINE_PASS_BASE_URL));
        assert_eq!(schema.api_key_env.as_deref(), Some(CLINE_PASS_API_KEY_ENV));
        assert_eq!(
            schema.models.first().map(|model| model.id.as_str()),
            Some("cline-pass/kimi-k2.7-code")
        );
        assert_eq!(schema.models.len(), 11);
    }

    #[test]
    fn live_model_payload_only_returns_cline_pass_models() {
        let payload = serde_json::json!({"data":{"clinePass":[{"id":"glm-5.2"},{"id":"cline-pass/already-prefixed"},{"name":"invalid"}],"free":[{"id":"provider/free-model"}]}});
        let ids: Vec<_> = parse_live_models(&payload)
            .into_iter()
            .map(|model| model.id)
            .collect();
        assert_eq!(ids, ["cline-pass/glm-5.2", "cline-pass/already-prefixed"]);
    }

    #[test]
    fn quota_helpers_parse_confirmed_envelope_and_balance() {
        let user = ClinePassQuotaSource::envelope_data(serde_json::json!({
            "success": true,
            "data": {"id": "user-1"}
        }));
        assert_eq!(user.get("id").and_then(Value::as_str), Some("user-1"));

        let balance = ClinePassQuotaSource::envelope_data(serde_json::json!({
            "success": true,
            "data": {"balance": 12}
        }));
        assert_eq!(
            ClinePassQuotaSource::balance_meter(&balance).and_then(|meter| meter.remaining),
            Some(12)
        );
        assert!(
            ClinePassQuotaSource::balance_meter(&serde_json::json!({
                "items": [{"used": 1, "limit": 2}]
            }))
            .is_none()
        );
    }

    #[test]
    fn quota_helpers_parse_usage_windows_and_thresholds() {
        let now = DateTime::parse_from_rfc3339("2025-01-31T12:00:00Z")
            .unwrap()
            .with_timezone(&Utc);
        let usages = serde_json::json!({"items": [
            {"createdAt": "2025-01-31T11:00:00Z", "costUsd": 58029},
            {"createdAt": "2025-01-24T12:00:00Z", "costUsd": "70000"},
            {"createdAt": "2024-12-01T11:00:00Z", "costUsd": 999999}
        ]});
        let plan = serde_json::json!({"plan":{"entitlements":{"cline_pass":{
            "inferenceCapThreshold": {
                "last5HoursUsageCostUSDPerUser": 1000000000,
                "last7daysUsageCostUSDPerUser": "2500000000",
                "last30daysUsageCostUSDPerUser": "5000000000"
            }
        }}}});
        let meters = ClinePassQuotaSource::usage_meters(&usages, &plan, now);
        assert_eq!(
            meters
                .iter()
                .map(|meter| meter.id.as_str())
                .collect::<Vec<_>>(),
            ["5h", "7d", "30d"]
        );
        assert_eq!(meters[0].used, Some(58029));
        assert_eq!(meters[1].used, Some(128029));
        assert_eq!(meters[2].limit, Some(5000000000));
        assert_eq!(meters[0].unit.as_deref(), Some("micro-USD"));
        assert_eq!(meters[0].reset_in_seconds, Some(5 * 60 * 60));
    }

    #[tokio::test]
    async fn api_key_wizard_is_one_step_and_rejects_empty_keys() {
        let mut wizard = ClinePassWizard;
        assert!(matches!(
            wizard.start().await,
            Step::Prompt { secret: true, .. }
        ));
        assert!(wizard.answer("  ".into()).await.is_err());
        let outcome = wizard.answer("secret".into()).await.unwrap();
        match outcome {
            Outcome::Done {
                schema,
                credentials,
            } => {
                assert_eq!(schema.id, "cline-pass");
                assert_eq!(credentials["api_key"], "secret");
            }
            Outcome::Next(_) => panic!("wizard did not finish"),
        }
    }
}

pub struct ClinePassKind;

impl AccountKind for ClinePassKind {
    fn name(&self) -> &str {
        "cline-pass"
    }
    fn display_name(&self) -> &str {
        "ClinePass"
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
        Box::new(ClinePassWizard)
    }
    fn quota_capability(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Option<QuotaCapability>, String> {
        let env_key = schema
            .api_key_env
            .as_deref()
            .and_then(|v| std::env::var(v).ok());
        let key = credentials
            .get("api_key")
            .and_then(Value::as_str)
            .map(str::trim)
            .filter(|key| !key.is_empty())
            .map(str::to_owned)
            .or(env_key)
            .ok_or_else(|| "ClinePass account is missing an API key".to_string())?;
        Ok(Some(QuotaCapability {
            descriptor: ClinePassQuotaSource::descriptor_static(),
            source: Some(Arc::new(ClinePassQuotaSource { key })),
        }))
    }
}

struct ClinePassWizard;
#[async_trait]
impl SetupWizard for ClinePassWizard {
    async fn start(&mut self) -> Step {
        Step::Prompt {
            label: "ClinePass API key".into(),
            secret: true,
        }
    }
    async fn answer(&mut self, input: String) -> Result<Outcome, WizardError> {
        let key = input.trim();
        if key.is_empty() {
            return Err(WizardError::InvalidAnswer(
                "api key must not be empty".into(),
            ));
        }
        Ok(Outcome::Done {
            schema: schema_template(),
            credentials: serde_json::json!({"api_key": key}),
        })
    }
}

/// Best-effort quota support. Cline has changed the account endpoint shape;
/// accept either of the known paths and tolerate unknown fields/shapes.
struct ClinePassQuotaSource {
    key: String,
}
impl ClinePassQuotaSource {
    fn descriptor_static() -> QuotaDescriptor {
        QuotaDescriptor {
            label: "ClinePass usage".into(),
            auth: QuotaAuth::ApiKey,
            meters: vec!["credits".into(), "5h".into(), "7d".into(), "30d".into()],
        }
    }
    fn number(value: Option<&Value>) -> Option<u64> {
        value.and_then(|v| {
            v.as_u64()
                .or_else(|| v.as_i64().filter(|n| *n >= 0).map(|n| n as u64))
                .or_else(|| v.as_str()?.trim().parse().ok())
        })
    }
    fn balance_meter(value: &Value) -> Option<QuotaMeter> {
        let balance = Self::number(value.get("balance"))?;
        Some(QuotaMeter {
            id: "credits".into(),
            label: "Credits".into(),
            window: None,
            used: None,
            limit: None,
            remaining: Some(balance),
            utilization_percent: None,
            unit: Some("microcredits".into()),
            reset_at: None,
            reset_in_seconds: None,
        })
    }

    fn envelope_data(value: Value) -> Value {
        if value.get("success").and_then(Value::as_bool) == Some(false) {
            return Value::Null;
        }
        value.get("data").cloned().unwrap_or(value)
    }

    fn usage_meters(usages: &Value, plan: &Value, now: DateTime<Utc>) -> Vec<QuotaMeter> {
        let Some(items) = usages.get("items").and_then(Value::as_array) else {
            return Vec::new();
        };
        let Some(thresholds) = plan
            .get("plan")
            .and_then(|v| v.get("entitlements"))
            .and_then(|v| v.get("cline_pass"))
            .and_then(|v| v.get("inferenceCapThreshold"))
        else {
            return Vec::new();
        };
        [
            ("5h", "last5HoursUsageCostUSDPerUser", 5 * 60 * 60),
            ("7d", "last7daysUsageCostUSDPerUser", 7 * 24 * 60 * 60),
            ("30d", "last30daysUsageCostUSDPerUser", 30 * 24 * 60 * 60),
        ]
        .into_iter()
        .filter_map(|(id, threshold_key, seconds)| {
            let limit = Self::number(thresholds.get(threshold_key))?;
            let since = now - Duration::seconds(seconds);
            let used = items.iter().fold(0u64, |sum, item| {
                let created = item
                    .get("createdAt")
                    .and_then(Value::as_str)
                    .and_then(|value| {
                        DateTime::parse_from_rfc3339(value)
                            .ok()
                            .map(|date| date.with_timezone(&Utc))
                    });
                if created.is_some_and(|created| created >= since && created <= now) {
                    sum.saturating_add(Self::number(item.get("costUsd")).unwrap_or(0))
                } else {
                    sum
                }
            });
            Some(QuotaMeter {
                id: id.into(),
                label: format!("{id} usage"),
                window: Some(id.into()),
                used: Some(used),
                limit: Some(limit),
                remaining: Some(limit.saturating_sub(used)),
                utilization_percent: Some((used as f64 / limit.max(1) as f64 * 100.0).min(100.0)),
                unit: Some("micro-USD".into()),
                reset_at: None,
                reset_in_seconds: Some(seconds as u64),
            })
        })
        .collect()
    }
}
#[async_trait]
impl QuotaSource for ClinePassQuotaSource {
    fn descriptor(&self) -> QuotaDescriptor {
        Self::descriptor_static()
    }
    async fn fetch(&self) -> Result<QuotaSnapshot, QuotaError> {
        let client = reqwest::Client::new();
        async fn get(client: &reqwest::Client, key: &str, path: &str) -> Result<Value, QuotaError> {
            let response = client
                .get(format!("{CLINE_PASS_BASE_URL}{path}"))
                .bearer_auth(key)
                .send()
                .await
                .map_err(|e| QuotaError::Request(e.to_string()))?;
            let status = response.status();
            let body = response
                .text()
                .await
                .map_err(|e| QuotaError::Request(e.to_string()))?;
            if status == reqwest::StatusCode::UNAUTHORIZED
                || status == reqwest::StatusCode::FORBIDDEN
            {
                return Err(QuotaError::InvalidCredentials);
            }
            if !status.is_success() {
                return Err(QuotaError::Api(format!("HTTP {status}: {body}")));
            }
            let value: Value =
                serde_json::from_str(&body).map_err(|e| QuotaError::Decode(e.to_string()))?;
            Ok(ClinePassQuotaSource::envelope_data(value))
        }
        let me = get(&client, &self.key, USERS_ME).await?;
        let id = me
            .get("id")
            .or_else(|| me.get("userId"))
            .and_then(Value::as_str)
            .ok_or_else(|| QuotaError::Decode("/users/me contained no user id".into()))?;
        let balance = get(&client, &self.key, &format!("/users/{id}/balance")).await?;
        let mut meters: Vec<QuotaMeter> = Self::balance_meter(&balance).into_iter().collect();
        let mut notes: Vec<String> = Vec::new();
        let usages = get(&client, &self.key, &format!("/users/{id}/usages"))
            .await
            .ok();
        let plan = get(&client, &self.key, "/users/me/plan").await.ok();
        if let (Some(usages), Some(plan)) = (&usages, &plan) {
            meters.extend(Self::usage_meters(usages, plan, Utc::now()));
        }
        if plan
            .as_ref()
            .and_then(|value| value.get("plan").and_then(|v| v.get("entitlements")))
            .and_then(|value| value.get("cline_pass"))
            .and_then(|value| value.get("inferenceCapThreshold"))
            .is_some()
        {
            notes.push("plan: cline_pass".into());
        }
        if meters.is_empty() {
            return Err(QuotaError::Unavailable(
                "confirmed quota endpoints returned no meters".into(),
            ));
        }
        Ok(QuotaSnapshot {
            account_id: id.into(),
            observed_at: Utc::now(),
            meters,
            note: (!notes.is_empty()).then(|| notes.join("; ")),
        })
    }
}
