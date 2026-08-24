//! ChatGPT Plus/Pro Codex provider using the OpenAI browser OAuth flow.

use super::{AccountKind, effort_modes, model};
use crate::Provider;
use crate::providers::CodexProvider;
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::quota::{
    QuotaAuth, QuotaCapability, QuotaDescriptor, QuotaError, QuotaMeter, QuotaSnapshot, QuotaSource,
};
use crate::types::{ModelCapability, ModelInfo};
use crate::wizard::{Outcome, SetupWizard, Step, WizardError};
use async_trait::async_trait;
use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use chrono::{TimeZone, Utc};
use serde::Deserialize;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio::sync::oneshot;
use uuid::Uuid;

pub const CODEX_BASE_URL: &str = "https://chatgpt.com/backend-api/codex";
pub const CODEX_ISSUER: &str = "https://auth.openai.com";
pub const CODEX_CLIENT_ID: &str = "app_EMoamEEZ73f0CkXaXp7hrann";
pub const CODEX_REDIRECT_URI: &str = "http://localhost:1455/auth/callback";

/// Sourced from the official Codex repository's
/// `codex-rs/models-manager/models.json`:
/// <https://github.com/openai/codex/blob/main/codex-rs/models-manager/models.json>
fn models() -> Vec<ModelInfo> {
    let mut models = vec![
        model("gpt-5.6-sol", 272_000, 128_000),
        model("gpt-5.6-terra", 272_000, 128_000),
        model("gpt-5.6-luna", 272_000, 128_000),
        model("gpt-5.5", 272_000, 128_000),
        model("gpt-5.4", 272_000, 128_000),
        model("gpt-5.4-mini", 272_000, 128_000),
        model("gpt-5.2", 272_000, 128_000),
        model("codex-auto-review", 272_000, 128_000),
    ];
    for info in &mut models {
        info.capabilities.insert(ModelCapability::Image);
        info.effort_modes = match info.id.as_str() {
            "gpt-5.6-sol" | "gpt-5.6-terra" => {
                effort_modes(&["low", "medium", "high", "xhigh", "max", "ultra"])
            }
            "gpt-5.6-luna" => effort_modes(&["low", "medium", "high", "xhigh", "max"]),
            "gpt-5.5" | "gpt-5.4" | "gpt-5.4-mini" | "gpt-5.2" | "codex-auto-review" => {
                effort_modes(&["low", "medium", "high", "xhigh"])
            }
            _ => Vec::new(),
        };
    }
    models
}

pub fn schema_template(account_id: &str) -> ProviderSchema {
    ProviderSchema {
        id: account_id.to_string(),
        api_type: ApiType::OpenAI,
        base_url: Some(CODEX_BASE_URL.to_string()),
        api_key_env: None,
        models: models(),
    }
}

struct CodexTokens {
    access_token: String,
    refresh_token: String,
    id_token: String,
}

pub struct CodexKind;

impl AccountKind for CodexKind {
    fn name(&self) -> &str {
        "codex"
    }

    fn display_name(&self) -> &str {
        "Codex (ChatGPT Plus/Pro)"
    }

    fn account_selection_meter(&self) -> Option<&str> {
        Some("7d")
    }

    fn build_provider(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Arc<dyn Provider>, String> {
        let access_token = credentials
            .get("access_token")
            .and_then(Value::as_str)
            .filter(|token| !token.is_empty())
            .ok_or_else(|| "Codex account is missing an access token".to_string())?;
        Ok(Arc::new(CodexProvider::new(
            schema.id.clone(),
            access_token,
            credentials
                .get("account_id")
                .and_then(Value::as_str)
                .map(str::to_string),
        )))
    }

    fn refresh_schema(&self, schema: &ProviderSchema) -> ProviderSchema {
        let mut refreshed = schema_template(&schema.id);
        if schema.base_url.is_some() {
            refreshed.base_url = schema.base_url.clone();
        }
        refreshed
    }

    fn wizard(&self) -> Box<dyn SetupWizard> {
        Box::new(CodexWizard::default())
    }

    fn quota_capability(
        &self,
        _schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Option<QuotaCapability>, String> {
        let access_token = credentials
            .get("access_token")
            .and_then(Value::as_str)
            .filter(|token| !token.is_empty())
            .ok_or_else(|| "Codex account is missing an access token".to_string())?;
        let account_id = credentials
            .get("account_id")
            .and_then(Value::as_str)
            .filter(|id| !id.is_empty())
            .map(str::to_string);
        Ok(Some(QuotaCapability {
            descriptor: QuotaDescriptor {
                label: "Codex subscription usage".into(),
                auth: QuotaAuth::WebSession,
                meters: vec!["5h".into(), "7d".into(), "credits".into()],
            },
            source: Some(Arc::new(CodexQuotaSource {
                access_token: access_token.to_string(),
                account_id,
            })),
        }))
    }
}

struct CodexQuotaSource {
    access_token: String,
    account_id: Option<String>,
}

#[async_trait]
impl QuotaSource for CodexQuotaSource {
    fn descriptor(&self) -> QuotaDescriptor {
        QuotaDescriptor {
            label: "Codex subscription usage".into(),
            auth: QuotaAuth::WebSession,
            meters: vec!["5h".into(), "7d".into(), "credits".into()],
        }
    }

    async fn fetch(&self) -> Result<QuotaSnapshot, QuotaError> {
        let client = reqwest::Client::new();
        let mut request = client
            .get("https://chatgpt.com/backend-api/wham/usage")
            .bearer_auth(&self.access_token)
            .header("User-Agent", "Firmius")
            .header("Accept", "application/json");
        if let Some(account_id) = &self.account_id {
            request = request.header("ChatGPT-Account-Id", account_id);
        }
        let response = request
            .send()
            .await
            .map_err(|error| QuotaError::Request(error.to_string()))?;
        let status = response.status();
        let body = response
            .text()
            .await
            .map_err(|error| QuotaError::Request(error.to_string()))?;
        if status == reqwest::StatusCode::UNAUTHORIZED || status == reqwest::StatusCode::FORBIDDEN {
            return Err(QuotaError::InvalidCredentials);
        }
        if !status.is_success() {
            return Err(QuotaError::Api(format!("HTTP {status}: {body}")));
        }
        let payload: CodexUsageResponse =
            serde_json::from_str(&body).map_err(|error| QuotaError::Decode(error.to_string()))?;
        let account_id = payload
            .account_id
            .clone()
            .or_else(|| self.account_id.clone())
            .unwrap_or_else(|| "codex".into());
        let mut meters = Vec::new();
        if let Some(window) = payload
            .rate_limit
            .as_ref()
            .and_then(|rate| rate.primary_window.as_ref())
        {
            meters.push(window.meter("5h", "5-hour"));
        }
        if let Some(window) = payload
            .rate_limit
            .as_ref()
            .and_then(|rate| rate.secondary_window.as_ref())
        {
            meters.push(window.meter("7d", "Weekly"));
        }
        if let Some(limit) = payload.resolved_individual_limit() {
            if let Some(used) = limit.used {
                let total = limit.limit;
                let remaining = limit
                    .remaining_percent
                    .map(|percent| percent.max(0.0) / 100.0 * total.unwrap_or(0.0));
                meters.push(QuotaMeter {
                    id: "credits".into(),
                    label: "Credits".into(),
                    window: Some("monthly".into()),
                    used: Some(used.max(0.0) as u64),
                    limit: total.map(|value| value.max(0.0) as u64),
                    remaining: remaining.map(|value| value.max(0.0) as u64),
                    utilization_percent: total
                        .filter(|value| *value > 0.0)
                        .map(|value| used / value * 100.0),
                    unit: Some("credits".into()),
                    reset_at: limit.resets_at.and_then(epoch),
                    reset_in_seconds: None,
                });
            }
        }
        if let Some(credits) = payload.credits.as_ref()
            && meters.iter().all(|meter| meter.id != "credits")
            && let Some(balance) = credits.balance
        {
            meters.push(QuotaMeter {
                id: "credits".into(),
                label: "Credits".into(),
                window: None,
                used: None,
                limit: None,
                remaining: Some(balance.max(0.0) as u64),
                utilization_percent: None,
                unit: Some("credits".into()),
                reset_at: None,
                reset_in_seconds: None,
            });
        }
        if meters.is_empty() {
            return Err(QuotaError::Decode(
                "Codex usage response contained no quota meters".into(),
            ));
        }
        Ok(QuotaSnapshot {
            account_id,
            observed_at: Utc::now(),
            meters,
            note: payload.plan_type.map(|plan| format!("plan: {plan}")),
        })
    }
}

#[derive(Debug, Deserialize)]
struct CodexUsageResponse {
    #[serde(default, alias = "accountId")]
    account_id: Option<String>,
    #[serde(default)]
    plan_type: Option<String>,
    #[serde(default)]
    rate_limit: Option<CodexRateLimit>,
    #[serde(default)]
    credits: Option<CodexCredits>,
    #[serde(default)]
    individual_limit: Option<CodexCreditLimit>,
    #[serde(default)]
    spend_control: Option<CodexSpendControl>,
}

impl CodexUsageResponse {
    fn resolved_individual_limit(&self) -> Option<&CodexCreditLimit> {
        self.individual_limit
            .as_ref()
            .or_else(|| {
                self.rate_limit
                    .as_ref()
                    .and_then(|rate| rate.individual_limit.as_ref())
            })
            .or_else(|| {
                self.spend_control
                    .as_ref()
                    .and_then(|control| control.individual_limit.as_ref())
            })
    }
}

#[derive(Debug, Deserialize)]
struct CodexRateLimit {
    #[serde(default)]
    primary_window: Option<CodexWindow>,
    #[serde(default)]
    secondary_window: Option<CodexWindow>,
    #[serde(default, alias = "individualLimit")]
    individual_limit: Option<CodexCreditLimit>,
}

#[derive(Debug, Deserialize)]
struct CodexSpendControl {
    #[serde(default, alias = "individualLimit")]
    individual_limit: Option<CodexCreditLimit>,
}

#[derive(Debug, Deserialize)]
struct CodexWindow {
    #[serde(deserialize_with = "deserialize_u64")]
    used_percent: u64,
    #[serde(default, deserialize_with = "deserialize_opt_i64")]
    reset_at: Option<i64>,
    #[serde(default, deserialize_with = "deserialize_opt_u64")]
    limit_window_seconds: Option<u64>,
    #[serde(default, deserialize_with = "deserialize_opt_u64")]
    reset_after_seconds: Option<u64>,
}

impl CodexWindow {
    fn meter(&self, id: &str, label: &str) -> QuotaMeter {
        let used = self.used_percent.min(100);
        let (id, label) = match self.limit_window_seconds {
            Some(seconds) if seconds <= 6 * 60 * 60 => ("5h", "5-hour"),
            Some(seconds) if seconds >= 6 * 24 * 60 * 60 => ("7d", "Weekly"),
            _ => (id, label),
        };
        let reset_at = self.reset_at.and_then(epoch);
        let reset_in_seconds = self.reset_after_seconds.or_else(|| {
            reset_at.and_then(|reset| (reset - Utc::now()).num_seconds().try_into().ok())
        });
        QuotaMeter {
            id: id.into(),
            label: label.into(),
            window: self
                .limit_window_seconds
                .map(|seconds| format!("{}h", seconds / 3600)),
            used: Some(used),
            limit: Some(100),
            remaining: Some(100 - used),
            utilization_percent: Some(used as f64),
            unit: Some("percent".into()),
            reset_at,
            reset_in_seconds,
        }
    }
}

#[derive(Debug, Deserialize)]
struct CodexCredits {
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    balance: Option<f64>,
}

#[derive(Debug, Deserialize)]
struct CodexCreditLimit {
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    limit: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    used: Option<f64>,
    #[serde(
        default,
        alias = "remaining_percent",
        deserialize_with = "deserialize_opt_f64"
    )]
    remaining_percent: Option<f64>,
    #[serde(default, alias = "reset_at", deserialize_with = "deserialize_opt_i64")]
    resets_at: Option<i64>,
}

fn deserialize_u64<'de, D>(deserializer: D) -> Result<u64, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let value = Value::deserialize(deserializer)?;
    match value {
        Value::Number(number) => number
            .as_u64()
            .ok_or_else(|| serde::de::Error::custom("expected unsigned integer")),
        Value::String(value) => value
            .trim()
            .parse()
            .map_err(|_| serde::de::Error::custom("expected unsigned integer string")),
        _ => Err(serde::de::Error::custom("expected unsigned integer")),
    }
}

fn deserialize_opt_u64<'de, D>(deserializer: D) -> Result<Option<u64>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let value = Option::<Value>::deserialize(deserializer)?;
    value
        .map(|value| match value {
            Value::Number(number) => number
                .as_u64()
                .ok_or_else(|| serde::de::Error::custom("expected unsigned integer")),
            Value::String(value) => value
                .trim()
                .parse()
                .map_err(|_| serde::de::Error::custom("expected unsigned integer string")),
            _ => Err(serde::de::Error::custom("expected unsigned integer")),
        })
        .transpose()
}

fn deserialize_opt_i64<'de, D>(deserializer: D) -> Result<Option<i64>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let value = Option::<Value>::deserialize(deserializer)?;
    value
        .map(|value| match value {
            Value::Number(number) => number
                .as_i64()
                .or_else(|| number.as_f64().map(|value| value as i64))
                .ok_or_else(|| serde::de::Error::custom("expected integer")),
            Value::String(value) => value
                .trim()
                .parse()
                .map_err(|_| serde::de::Error::custom("expected integer string")),
            _ => Err(serde::de::Error::custom("expected integer")),
        })
        .transpose()
}

fn deserialize_opt_f64<'de, D>(deserializer: D) -> Result<Option<f64>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let value = Option::<Value>::deserialize(deserializer)?;
    value
        .map(|value| match value {
            Value::Number(number) => number
                .as_f64()
                .ok_or_else(|| serde::de::Error::custom("expected number")),
            Value::String(value) => value
                .trim()
                .parse()
                .map_err(|_| serde::de::Error::custom("expected number string")),
            _ => Err(serde::de::Error::custom("expected number")),
        })
        .transpose()
}

fn epoch(seconds: i64) -> Option<chrono::DateTime<Utc>> {
    Utc.timestamp_opt(seconds, 0).single()
}

#[derive(Default)]
pub struct CodexWizard {
    callback: Option<oneshot::Receiver<Result<CodexTokens, String>>>,
    state: Option<String>,
    started: bool,
}

impl CodexWizard {
    async fn start_server(&mut self) -> Result<String, String> {
        let listener = TcpListener::bind(("127.0.0.1", 1455))
            .await
            .map_err(|error| format!("could not bind OAuth callback on localhost:1455: {error}"))?;
        let state = Uuid::new_v4().to_string();
        let verifier = Uuid::new_v4().to_string() + &Uuid::new_v4().to_string();
        let challenge = URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()));
        let (tx, rx) = oneshot::channel();
        self.callback = Some(rx);
        self.state = Some(state.clone());
        let expected_state = state.clone();
        tokio::spawn(async move {
            let (mut stream, _) = match listener.accept().await {
                Ok(connection) => connection,
                Err(error) => {
                    let _ = tx.send(Err(error.to_string()));
                    return;
                }
            };
            let result = async {
                let mut buffer = vec![0; 8192];
                let size = stream.read(&mut buffer).await.map_err(|e| e.to_string())?;
                let request = String::from_utf8_lossy(&buffer[..size]);
                let target = request
                    .lines()
                    .next()
                    .and_then(|line| line.split_whitespace().nth(1))
                    .ok_or_else(|| "malformed OAuth callback request".to_string())?;
                let url = reqwest::Url::parse(&format!("http://localhost{target}"))
                    .map_err(|e| e.to_string())?;
                let received_state = url
                    .query_pairs()
                    .find(|(key, _)| key == "state")
                    .map(|(_, value)| value.to_string());
                if received_state.as_deref() != Some(expected_state.as_str()) {
                    return Err("OAuth state mismatch".to_string());
                }
                if let Some(error) = url
                    .query_pairs()
                    .find(|(key, _)| key == "error")
                    .map(|(_, value)| value.to_string())
                {
                    return Err(format!("OpenAI OAuth failed: {error}"));
                }
                let code = url
                    .query_pairs()
                    .find(|(key, _)| key == "code")
                    .map(|(_, value)| value.to_string())
                    .ok_or_else(|| "OAuth callback did not include a code".to_string())?;
                let response = reqwest::Client::new()
                    .post(format!("{CODEX_ISSUER}/oauth/token"))
                    .form(&[
                        ("grant_type", "authorization_code"),
                        ("code", code.as_str()),
                        ("redirect_uri", CODEX_REDIRECT_URI),
                        ("client_id", CODEX_CLIENT_ID),
                        ("code_verifier", verifier.as_str()),
                    ])
                    .send()
                    .await
                    .map_err(|e| e.to_string())?;
                if !response.status().is_success() {
                    return Err(format!(
                        "OpenAI token exchange failed: {}",
                        response.status()
                    ));
                }
                let tokens: Value = response.json().await.map_err(|e| e.to_string())?;
                let parsed = CodexTokens {
                    access_token: tokens
                        .get("access_token")
                        .and_then(Value::as_str)
                        .ok_or_else(|| "OAuth response missing access_token".to_string())?
                        .to_string(),
                    refresh_token: tokens
                        .get("refresh_token")
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string(),
                    id_token: tokens
                        .get("id_token")
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string(),
                };
                Ok(parsed)
            }
            .await;
            let body = if result.is_ok() {
                "<html><body>Codex login complete. You can close this window.</body></html>"
            } else {
                "<html><body>Codex login failed. Return to Firmius for details.</body></html>"
            };
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body.len(),
                body
            );
            let _ = stream.write_all(response.as_bytes()).await;
            let _ = tx.send(result);
        });
        let mut url = reqwest::Url::parse(&format!("{CODEX_ISSUER}/oauth/authorize")).unwrap();
        url.query_pairs_mut()
            .append_pair("response_type", "code")
            .append_pair("client_id", CODEX_CLIENT_ID)
            .append_pair("redirect_uri", CODEX_REDIRECT_URI)
            .append_pair("scope", "openid profile email offline_access")
            .append_pair("code_challenge", &challenge)
            .append_pair("code_challenge_method", "S256")
            .append_pair("id_token_add_organizations", "true")
            .append_pair("codex_cli_simplified_flow", "true")
            .append_pair("state", &state)
            .append_pair("originator", "firmius");
        Ok(url.to_string())
    }
}

#[async_trait]
impl SetupWizard for CodexWizard {
    async fn start(&mut self) -> Step {
        self.started = true;
        match self.start_server().await {
            Ok(url) => Step::OpenUrl {
                label: "OpenAI authorization started".into(),
                url,
            },
            Err(error) => Step::Prompt {
                label: format!("OAuth unavailable: {error}"),
                secret: false,
            },
        }
    }

    async fn answer(&mut self, _input: String) -> Result<Outcome, WizardError> {
        Err(WizardError::InvalidAnswer(
            "Codex login completes in the browser".into(),
        ))
    }

    async fn poll(&mut self) -> Result<Option<Outcome>, WizardError> {
        let Some(callback) = &mut self.callback else {
            return Ok(None);
        };
        match callback.try_recv() {
            Ok(Ok(tokens)) => {
                let account_id = extract_account_id(&tokens.id_token)
                    .or_else(|| extract_account_id(&tokens.access_token))
                    .unwrap_or_else(|| Uuid::new_v4().to_string());
                Ok(Some(Outcome::Done {
                    schema: schema_template(&format!("codex-{account_id}")),
                    credentials: serde_json::json!({
                        "access_token": tokens.access_token,
                        "refresh_token": tokens.refresh_token,
                        "id_token": tokens.id_token,
                        "account_id": account_id,
                    }),
                }))
            }
            Ok(Err(error)) => Err(WizardError::InvalidAnswer(error)),
            Err(oneshot::error::TryRecvError::Empty) => Ok(None),
            Err(oneshot::error::TryRecvError::Closed) => Err(WizardError::InvalidAnswer(
                "OAuth callback closed unexpectedly".into(),
            )),
        }
    }
}

fn extract_account_id(token: &str) -> Option<String> {
    let payload = token.split('.').nth(1)?;
    let bytes = URL_SAFE_NO_PAD.decode(payload).ok()?;
    let claims: Value = serde_json::from_slice(&bytes).ok()?;
    claims
        .get("chatgpt_account_id")
        .and_then(Value::as_str)
        .or_else(|| {
            claims
                .get("https://api.openai.com/auth")
                .and_then(|auth| auth.get("chatgpt_account_id"))
                .and_then(Value::as_str)
        })
        .or_else(|| {
            claims
                .get("organizations")
                .and_then(Value::as_array)
                .and_then(|items| items.first())
                .and_then(|item| item.get("id"))
                .and_then(Value::as_str)
        })
        .map(str::to_string)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_has_codex_models_and_endpoint() {
        let schema = schema_template("codex-account");
        assert_eq!(schema.base_url.as_deref(), Some(CODEX_BASE_URL));
        assert!(schema.models.iter().any(|model| model.id == "gpt-5.6-luna"));
    }

    #[test]
    fn quota_capability_uses_oauth_credentials() {
        let capability = CodexKind
            .quota_capability(
                &schema_template("codex-account"),
                &serde_json::json!({
                    "access_token": "access-token",
                    "account_id": "workspace-id"
                }),
            )
            .unwrap()
            .unwrap();
        assert_eq!(capability.descriptor.auth, QuotaAuth::WebSession);
        assert_eq!(capability.descriptor.meters, ["5h", "7d", "credits"]);
        assert!(capability.source.is_some());
    }

    #[test]
    fn quota_response_maps_five_hour_weekly_and_credit_meters() {
        let payload: CodexUsageResponse = serde_json::from_value(serde_json::json!({
            "account_id": "workspace-id",
            "plan_type": "plus",
            "rate_limit": {
                "primary_window": {
                    "used_percent": 25,
                    "reset_at": 1_800_000_000,
                    "limit_window_seconds": 18_000
                },
                "secondary_window": {
                    "used_percent": 60,
                    "reset_at": 1_800_600_000,
                    "limit_window_seconds": 604_800
                }
            },
            "individual_limit": {
                "limit": 1000,
                "used": 125,
                "remaining_percent": 87.5,
                "reset_at": 1_801_000_000
            }
        }))
        .unwrap();
        let primary = payload
            .rate_limit
            .as_ref()
            .unwrap()
            .primary_window
            .as_ref()
            .unwrap();
        let weekly = payload
            .rate_limit
            .as_ref()
            .unwrap()
            .secondary_window
            .as_ref()
            .unwrap();
        assert_eq!(primary.meter("5h", "5-hour").remaining, Some(75));
        assert_eq!(weekly.meter("7d", "Weekly").utilization_percent, Some(60.0));
        let credits = payload.resolved_individual_limit().unwrap();
        assert_eq!(credits.used, Some(125.0));
        assert_eq!(credits.remaining_percent, Some(87.5));
    }

    #[test]
    fn quota_response_classifies_a_live_weekly_primary_window() {
        let payload: CodexUsageResponse = serde_json::from_value(serde_json::json!({
            "account_id": "workspace-id",
            "plan_type": "plus",
            "rate_limit": {
                "allowed": true,
                "primary_window": {
                    "used_percent": "5",
                    "reset_after_seconds": "541052",
                    "reset_at": "1787466040",
                    "limit_window_seconds": "604800"
                },
                "secondary_window": null
            },
            "credits": {
                "has_credits": false,
                "unlimited": false,
                "balance": 12.5
            }
        }))
        .unwrap();
        let window = payload.rate_limit.unwrap().primary_window.unwrap();
        let meter = window.meter("5h", "5-hour");
        assert_eq!(meter.id, "7d");
        assert_eq!(meter.label, "Weekly");
        assert_eq!(meter.reset_in_seconds, Some(541052));
        assert_eq!(payload.credits.unwrap().balance, Some(12.5));
    }
}
