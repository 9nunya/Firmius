//! Claude Pro/Max subscription account kind using Anthropic OAuth.

use super::{AccountKind, effort_modes, model};
use crate::persistence::{self};
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::providers::{AnthropicProvider, TokenSupplier};
use crate::quota::{
    QuotaAuth, QuotaCapability, QuotaDescriptor, QuotaError, QuotaMeter, QuotaSnapshot, QuotaSource,
};
use crate::types::ModelInfo;
use crate::wizard::{Outcome, SetupWizard, Step, WizardError};
use crate::{Provider, ProviderError};
use async_trait::async_trait;
use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use chrono::{TimeZone, Utc};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::path::PathBuf;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio::sync::{Mutex, oneshot};
use uuid::Uuid;

pub const ANTHROPIC_AUTH_URL: &str = "https://claude.ai/oauth/authorize";
pub const ANTHROPIC_TOKEN_URL: &str = "https://platform.claude.com/v1/oauth/token";
pub const ANTHROPIC_CLIENT_ID: &str = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
pub const ANTHROPIC_REDIRECT_URI: &str = "http://localhost:53692/callback";
pub const ANTHROPIC_SCOPES: &str = "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
const ANTHROPIC_BASE_URL: &str = "https://api.anthropic.com";
const REFRESH_SKEW_SECONDS: i64 = 300;

fn models() -> Vec<ModelInfo> {
    let mut models = vec![
        model("claude-opus-5", 200_000, 64_000),
        model("claude-opus-4-8", 200_000, 64_000),
        model("claude-sonnet-5", 200_000, 64_000),
        model("claude-fable-5", 200_000, 64_000),
    ];
    for info in &mut models {
        info.effort_modes = effort_modes(&["low", "medium", "high", "xhigh", "max"]);
    }
    models
}

pub fn schema_template(account_id: &str) -> ProviderSchema {
    ProviderSchema {
        id: account_id.to_string(),
        api_type: ApiType::Anthropic,
        base_url: Some(ANTHROPIC_BASE_URL.to_string()),
        api_key_env: None,
        models: models(),
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct AnthropicOAuthCredentials {
    access_token: String,
    refresh_token: String,
    #[serde(default)]
    expires_at: Option<i64>,
    #[serde(default)]
    account_id: Option<String>,
}

pub struct AnthropicSubscriptionKind;

impl AccountKind for AnthropicSubscriptionKind {
    fn name(&self) -> &str {
        "anthropic"
    }

    fn display_name(&self) -> &str {
        "Anthropic (Claude Pro/Max)"
    }

    fn build_provider(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Arc<dyn Provider>, String> {
        self.build_provider_at(schema, credentials, &persistence::data_dir())
    }

    fn build_provider_at(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
        data_dir: &std::path::Path,
    ) -> Result<Arc<dyn Provider>, String> {
        let creds = parse_credentials(credentials)?;
        Ok(Arc::new(AnthropicProvider::with_oauth(
            schema.id.clone(),
            Arc::new(AnthropicOAuthTokenSupplier::new(
                data_dir.to_path_buf(),
                schema.id.clone(),
                creds,
            )),
        )
        .with_base_url(schema.effective_base_url())))
    }

    fn refresh_schema(&self, schema: &ProviderSchema) -> ProviderSchema {
        let mut refreshed = schema_template(&schema.id);
        if schema.base_url.is_some() {
            refreshed.base_url = schema.base_url.clone();
        }
        refreshed
    }

    fn wizard(&self) -> Box<dyn SetupWizard> {
        Box::new(AnthropicSubscriptionWizard::default())
    }

    fn quota_capability_at(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
        data_dir: &std::path::Path,
    ) -> Result<Option<QuotaCapability>, String> {
        let creds = parse_credentials(credentials)?;
        Ok(Some(QuotaCapability {
            descriptor: anthropic_quota_descriptor(),
            source: Some(Arc::new(AnthropicQuotaSource {
                auth: Arc::new(AnthropicOAuthTokenSupplier::new(
                    data_dir.to_path_buf(),
                    schema.id.clone(),
                    creds,
                )),
            })),
        }))
    }
}

fn parse_credentials(credentials: &Value) -> Result<AnthropicOAuthCredentials, String> {
    let creds: AnthropicOAuthCredentials = serde_json::from_value(credentials.clone())
        .map_err(|_| "Anthropic account credentials are malformed".to_string())?;
    if creds.access_token.trim().is_empty() {
        return Err("Anthropic account is missing an access token".into());
    }
    if creds.refresh_token.trim().is_empty() {
        return Err("Anthropic account is missing a refresh token".into());
    }
    Ok(creds)
}

struct AnthropicOAuthTokenSupplier {
    data_dir: PathBuf,
    account_id: String,
    credentials: Mutex<AnthropicOAuthCredentials>,
}

impl AnthropicOAuthTokenSupplier {
    fn new(data_dir: PathBuf, account_id: String, credentials: AnthropicOAuthCredentials) -> Self {
        Self {
            data_dir,
            account_id,
            credentials: Mutex::new(credentials),
        }
    }

    async fn refresh_if_needed(
        &self,
        creds: &mut AnthropicOAuthCredentials,
    ) -> Result<(), ProviderError> {
        let should_refresh = creds
            .expires_at
            .is_some_and(|ts| ts <= Utc::now().timestamp() + REFRESH_SKEW_SECONDS);
        if !should_refresh {
            return Ok(());
        }
        let response = reqwest::Client::new()
            .post(ANTHROPIC_TOKEN_URL)
            .header("Accept", "application/json")
            .header("Content-Type", "application/json")
            .header("User-Agent", "Firmius")
            .json(&serde_json::json!({
                "grant_type": "refresh_token",
                "refresh_token": creds.refresh_token,
                "client_id": ANTHROPIC_CLIENT_ID,
            }))
            .send()
            .await
            .map_err(|error| ProviderError::Http(error.to_string()))?;
        if response.status() == reqwest::StatusCode::UNAUTHORIZED
            || response.status() == reqwest::StatusCode::FORBIDDEN
        {
            return Err(ProviderError::Auth(
                "Anthropic OAuth refresh was rejected".into(),
            ));
        }
        if !response.status().is_success() {
            return Err(ProviderError::Auth(format!(
                "Anthropic OAuth refresh failed: HTTP {}",
                response.status()
            )));
        }
        let token: OAuthTokenResponse = response
            .json()
            .await
            .map_err(|e| ProviderError::Decode(e.to_string()))?;
        creds.access_token = token.access_token;
        if let Some(refresh_token) = token.refresh_token.filter(|token| !token.trim().is_empty()) {
            creds.refresh_token = refresh_token;
        }
        if let Some(expires_in) = token.expires_in {
            creds.expires_at = Some(Utc::now().timestamp() + expires_in.max(0));
        }
        persist_credentials(&self.data_dir, &self.account_id, creds).map_err(|_| {
            ProviderError::Auth("could not persist refreshed Anthropic OAuth credentials".into())
        })?;
        Ok(())
    }
}

#[async_trait]
impl TokenSupplier for AnthropicOAuthTokenSupplier {
    async fn headers(&self) -> Result<Vec<(String, String)>, ProviderError> {
        let mut creds = self.credentials.lock().await;
        self.refresh_if_needed(&mut creds).await?;
        Ok(vec![(
            "Authorization".into(),
            format!("Bearer {}", creds.access_token),
        )])
    }
}

fn persist_credentials(
    data_dir: &std::path::Path,
    account_id: &str,
    creds: &AnthropicOAuthCredentials,
) -> Result<(), String> {
    let mut record = persistence::load_account_at(data_dir, account_id)?;
    record.credentials["access_token"] = Value::String(creds.access_token.clone());
    record.credentials["refresh_token"] = Value::String(creds.refresh_token.clone());
    record.credentials["expires_at"] = creds.expires_at.map(Value::from).unwrap_or(Value::Null);
    persistence::save_account_at(data_dir, &record)
}

#[derive(Debug, Deserialize)]
struct OAuthTokenResponse {
    access_token: String,
    #[serde(default)]
    refresh_token: Option<String>,
    #[serde(default)]
    expires_in: Option<i64>,
}

fn anthropic_quota_descriptor() -> QuotaDescriptor {
    QuotaDescriptor {
        label: "Anthropic subscription usage".into(),
        auth: QuotaAuth::WebSession,
        meters: vec![
            "five_hour".into(),
            "seven_day".into(),
            "seven_day_opus".into(),
            "seven_day_sonnet".into(),
            "weekly_scoped".into(),
            "spend".into(),
        ],
    }
}

struct AnthropicQuotaSource {
    auth: Arc<dyn TokenSupplier>,
}

#[async_trait]
impl QuotaSource for AnthropicQuotaSource {
    fn descriptor(&self) -> QuotaDescriptor {
        anthropic_quota_descriptor()
    }

    async fn fetch(&self) -> Result<QuotaSnapshot, QuotaError> {
        let headers = self
            .auth
            .headers()
            .await
            .map_err(|e| QuotaError::Request(e.to_string()))?;
        let mut request = reqwest::Client::new()
            .get("https://api.anthropic.com/api/oauth/usage")
            .header("Accept", "application/json")
            .header("User-Agent", "Firmius")
            .header("anthropic-beta", "oauth-2025-04-20");
        for (name, value) in headers {
            request = request.header(name, value);
        }
        let response = request
            .send()
            .await
            .map_err(|e| QuotaError::Request(e.to_string()))?;
        let status = response.status();
        let body = response
            .text()
            .await
            .map_err(|e| QuotaError::Request(e.to_string()))?;
        if status == reqwest::StatusCode::UNAUTHORIZED || status == reqwest::StatusCode::FORBIDDEN {
            return Err(QuotaError::InvalidCredentials);
        }
        if !status.is_success() {
            return Err(QuotaError::Api(format!("HTTP {status}")));
        }
        parse_usage(&body)
    }
}

#[derive(Debug, Deserialize)]
struct AnthropicUsageResponse {
    #[serde(default)]
    account_id: Option<String>,
    #[serde(default)]
    five_hour: Option<AnthropicUsageWindow>,
    #[serde(default)]
    seven_day: Option<AnthropicUsageWindow>,
    #[serde(default)]
    seven_day_opus: Option<AnthropicUsageWindow>,
    #[serde(default)]
    seven_day_sonnet: Option<AnthropicUsageWindow>,
    #[serde(default)]
    limits: Option<Value>,
    #[serde(default)]
    spend: Option<AnthropicSpend>,
}

#[derive(Debug, Deserialize)]
struct AnthropicLimitsObject {
    #[serde(default)]
    weekly_scoped: Option<AnthropicUsageWindow>,
}

#[derive(Debug, Deserialize)]
struct AnthropicSpend {
    #[serde(default)]
    enabled: Option<bool>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    percent: Option<f64>,
    #[serde(default)]
    used: Option<AnthropicMoney>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    used_amount: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    limit: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    remaining: Option<f64>,
    #[serde(default)]
    reset_at: Option<i64>,
}

#[derive(Debug, Deserialize)]
struct AnthropicMoney {
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    amount_minor: Option<f64>,
    #[serde(default)]
    exponent: Option<i32>,
    #[serde(default)]
    currency: Option<String>,
}

#[derive(Debug, Deserialize)]
struct LegacyAnthropicSpend {
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    used: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    limit: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    remaining: Option<f64>,
    #[serde(default)]
    reset_at: Option<i64>,
}

#[derive(Debug, Deserialize)]
struct AnthropicUsageWindow {
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    used: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    limit: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    remaining: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    used_percent: Option<f64>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    utilization: Option<f64>,
    #[serde(default)]
    reset_at: Option<i64>,
    #[serde(default)]
    resets_at: Option<String>,
    #[serde(default)]
    reset_after_seconds: Option<u64>,
}

impl AnthropicUsageWindow {
    fn meter(&self, id: &str, label: &str) -> QuotaMeter {
        let used = self.used.map(nonnegative_u64);
        let limit = self.limit.map(nonnegative_u64);
        let remaining = self
            .remaining
            .map(nonnegative_u64)
            .or_else(|| limit.zip(used).map(|(l, u)| l.saturating_sub(u)));
        let utilization_percent = self.used_percent.or(self.utilization).or_else(|| {
            self.used
                .zip(self.limit)
                .and_then(|(u, l)| (l > 0.0).then_some(u / l * 100.0))
        });
        QuotaMeter {
            id: id.into(),
            label: label.into(),
            window: None,
            used,
            limit,
            remaining,
            utilization_percent,
            unit: Some("tokens".into()),
            reset_at: self
                .reset_at
                .and_then(epoch)
                .or_else(|| self.resets_at.as_deref().and_then(parse_iso_time)),
            reset_in_seconds: self.reset_after_seconds,
        }
    }
}

fn parse_usage(body: &str) -> Result<QuotaSnapshot, QuotaError> {
    let payload: AnthropicUsageResponse =
        serde_json::from_str(body).map_err(|e| QuotaError::Decode(e.to_string()))?;
    let mut meters = Vec::new();
    if let Some(window) = payload.five_hour {
        meters.push(window.meter("five_hour", "5-hour"));
    }
    if let Some(window) = payload.seven_day {
        meters.push(window.meter("seven_day", "7-day"));
    }
    if let Some(window) = payload.seven_day_opus {
        meters.push(window.meter("seven_day_opus", "7-day Opus"));
    }
    if let Some(window) = payload.seven_day_sonnet {
        meters.push(window.meter("seven_day_sonnet", "7-day Sonnet"));
    }
    parse_limits(payload.limits, &mut meters)?;
    if let Some(spend) = payload.spend.filter(|spend| spend.enabled.unwrap_or(true)) {
        let used = spend.used.as_ref().and_then(money_minor_as_major).map(nonnegative_u64)
            .or_else(|| spend.used_amount.map(nonnegative_u64));
        meters.push(QuotaMeter {
            id: "spend".into(),
            label: "Spend".into(),
            window: None,
            used,
            limit: spend.limit.map(nonnegative_u64),
            remaining: spend.remaining.map(nonnegative_u64),
            utilization_percent: spend.percent,
            unit: Some(spend.used.as_ref().and_then(|m| m.currency.clone()).unwrap_or_else(|| "usd".into()).to_lowercase()),
            reset_at: spend.reset_at.and_then(epoch),
            reset_in_seconds: None,
        });
    }
    if false { let spend = LegacyAnthropicSpend { used: None, limit: None, remaining: None, reset_at: None };
        let used = spend.used.map(nonnegative_u64);
        let limit = spend.limit.map(nonnegative_u64);
        let remaining = spend
            .remaining
            .map(nonnegative_u64)
            .or_else(|| limit.zip(used).map(|(l, u)| l.saturating_sub(u)));
        meters.push(QuotaMeter {
            id: "spend".into(),
            label: "Spend".into(),
            window: None,
            used,
            limit,
            remaining,
            utilization_percent: spend
                .used
                .zip(spend.limit)
                .and_then(|(u, l)| (l > 0.0).then_some(u / l * 100.0)),
            unit: Some("usd".into()),
            reset_at: spend.reset_at.and_then(epoch),
            reset_in_seconds: None,
        });
    }
    if meters.is_empty() {
        return Err(QuotaError::Decode(
            "Anthropic usage response contained no quota meters".into(),
        ));
    }
    Ok(QuotaSnapshot {
        account_id: payload.account_id.unwrap_or_else(|| "anthropic".into()),
        observed_at: Utc::now(),
        meters,
        note: None,
    })
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

fn nonnegative_u64(value: f64) -> u64 {
    value.max(0.0) as u64
}
fn epoch(seconds: i64) -> Option<chrono::DateTime<Utc>> {
    Utc.timestamp_opt(seconds, 0).single()
}

#[derive(Default)]
pub struct AnthropicSubscriptionWizard {
    callback: Option<oneshot::Receiver<Result<AnthropicOAuthCredentials, String>>>,
    started: bool,
}

impl AnthropicSubscriptionWizard {
    async fn start_server(&mut self) -> Result<String, String> {
        let listener = TcpListener::bind(("127.0.0.1", 53692))
            .await
            .map_err(|e| format!("could not bind OAuth callback on localhost:53692: {e}"))?;
        let verifier = Uuid::new_v4().to_string() + &Uuid::new_v4().to_string();
        let challenge = URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()));
        let (tx, rx) = oneshot::channel();
        self.callback = Some(rx);
        let expected_state = verifier.clone();
        tokio::spawn(async move {
            let (mut stream, _) = match listener.accept().await {
                Ok(c) => c,
                Err(e) => {
                    let _ = tx.send(Err(e.to_string()));
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
                    return Err(format!("Anthropic OAuth failed: {error}"));
                }
                let code = url
                    .query_pairs()
                    .find(|(key, _)| key == "code")
                    .map(|(_, value)| value.to_string())
                    .ok_or_else(|| "OAuth callback did not include a code".to_string())?;
                let response = reqwest::Client::new()
                    .post(ANTHROPIC_TOKEN_URL)
                    .header("Accept", "application/json")
                    .header("Content-Type", "application/json")
                    .header("User-Agent", "Firmius")
                    .json(&serde_json::json!({
                        "grant_type": "authorization_code",
                        "code": code,
                        "redirect_uri": ANTHROPIC_REDIRECT_URI,
                        "client_id": ANTHROPIC_CLIENT_ID,
                        "code_verifier": expected_state,
                    }))
                    .send()
                    .await
                    .map_err(|e| e.to_string())?;
                if !response.status().is_success() {
                    return Err(format!(
                        "Anthropic token exchange failed: HTTP {}",
                        response.status()
                    ));
                }
                let token: OAuthTokenResponse = response.json().await.map_err(|e| e.to_string())?;
                Ok(AnthropicOAuthCredentials {
                    access_token: token.access_token,
                    refresh_token: token.refresh_token.unwrap_or_default(),
                    expires_at: token.expires_in.map(|s| Utc::now().timestamp() + s.max(0)),
                    account_id: None,
                })
            }
            .await;
            let body = if result.is_ok() {
                "<html><body>Anthropic login complete. You can close this window.</body></html>"
            } else {
                "<html><body>Anthropic login failed. Return to Firmius for details.</body></html>"
            };
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body.len(),
                body
            );
            let _ = stream.write_all(response.as_bytes()).await;
            let _ = tx.send(result);
        });
        let mut url = reqwest::Url::parse(ANTHROPIC_AUTH_URL).unwrap();
        url.query_pairs_mut()
            .append_pair("response_type", "code")
            .append_pair("client_id", ANTHROPIC_CLIENT_ID)
            .append_pair("redirect_uri", ANTHROPIC_REDIRECT_URI)
            .append_pair("scope", ANTHROPIC_SCOPES)
            .append_pair("code_challenge", &challenge)
            .append_pair("code_challenge_method", "S256")
            .append_pair("state", &verifier);
        Ok(url.to_string())
    }
}

#[async_trait]
impl SetupWizard for AnthropicSubscriptionWizard {
    async fn start(&mut self) -> Step {
        self.started = true;
        match self.start_server().await {
            Ok(url) => Step::OpenUrl {
                label: "Anthropic authorization started".into(),
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
            "Anthropic login completes in the browser".into(),
        ))
    }

    async fn poll(&mut self) -> Result<Option<Outcome>, WizardError> {
        let Some(callback) = &mut self.callback else {
            return Ok(None);
        };
        match callback.try_recv() {
            Ok(Ok(tokens)) => {
                let account_id = tokens
                    .account_id
                    .clone()
                    .unwrap_or_else(|| Uuid::new_v4().to_string());
                Ok(Some(Outcome::Done {
                    schema: schema_template(&format!("anthropic-{account_id}")),
                    credentials: serde_json::json!({
                        "access_token": tokens.access_token,
                        "refresh_token": tokens.refresh_token,
                        "expires_at": tokens.expires_at,
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::persistence::AccountRecord;

    #[test]
    fn schema_has_subscription_models_and_effort() {
        let schema = schema_template("anthropic-account");
        assert_eq!(schema.api_type, ApiType::Anthropic);
        assert_eq!(schema.base_url.as_deref(), Some(ANTHROPIC_BASE_URL));
        let ids: Vec<_> = schema.models.iter().map(|m| m.id.as_str()).collect();
        assert_eq!(
            ids,
            [
                "claude-opus-5",
                "claude-opus-4-8",
                "claude-sonnet-5",
                "claude-fable-5"
            ]
        );
        for model in &schema.models {
            assert_eq!(model.context_window, 200_000);
            assert_eq!(model.max_output_tokens, Some(64_000));
            assert_eq!(
                model
                    .effort_modes
                    .iter()
                    .map(|m| m.name.as_str())
                    .collect::<Vec<_>>(),
                ["low", "medium", "high", "xhigh", "max"]
            );
        }
    }

    #[test]
    fn quota_parser_maps_all_known_meters() {
        let snapshot = parse_usage(&serde_json::json!({
            "account_id": "user-id",
            "five_hour": { "used": "10", "limit": "100", "remaining": "90", "used_percent": "10", "reset_after_seconds": 300 },
            "seven_day": { "used": 20, "limit": 200 },
            "seven_day_opus": { "used": 30, "limit": 300 },
            "seven_day_sonnet": { "used": 40, "limit": 400 },
            "limits": { "weekly_scoped": { "used": 50, "limit": 500 } },
            "spend": { "used": 6.5, "limit": 10.0, "remaining": 3.5, "reset_at": 1_800_000_000 }
        }).to_string()).unwrap();
        assert_eq!(snapshot.account_id, "user-id");
        assert_eq!(
            snapshot
                .meters
                .iter()
                .map(|m| m.id.as_str())
                .collect::<Vec<_>>(),
            [
                "five_hour",
                "seven_day",
                "seven_day_opus",
                "seven_day_sonnet",
                "weekly_scoped",
                "spend"
            ]
        );
        assert_eq!(snapshot.meters[0].remaining, Some(90));
        assert_eq!(snapshot.meters[5].unit.as_deref(), Some("usd"));
    }

    #[test]
    fn refresh_token_rotation_preserves_existing_refresh_when_absent() {
        let mut creds = AnthropicOAuthCredentials {
            access_token: "old".into(),
            refresh_token: "keep".into(),
            expires_at: None,
            account_id: None,
        };
        let token = OAuthTokenResponse {
            access_token: "new".into(),
            refresh_token: None,
            expires_in: Some(60),
        };
        creds.access_token = token.access_token;
        if let Some(refresh_token) = token.refresh_token.filter(|token| !token.trim().is_empty()) {
            creds.refresh_token = refresh_token;
        }
        assert_eq!(creds.access_token, "new");
        assert_eq!(creds.refresh_token, "keep");
    }

    #[test]
    fn persist_updates_only_oauth_token_fields() {
        let base = std::env::temp_dir().join(format!("firmius-anthropic-test-{}", Uuid::new_v4()));
        std::fs::create_dir_all(&base).unwrap();
        let record = AccountRecord {
            id: "anthropic-test".into(),
            kind: "anthropic".into(),
            schema: schema_template("anthropic-test"),
            credentials: serde_json::json!({ "access_token": "old", "refresh_token": "old-r", "expires_at": 1, "account_id": "acct" }),
        };
        persistence::save_account_at(&base, &record).unwrap();
        let creds = AnthropicOAuthCredentials {
            access_token: "new".into(),
            refresh_token: "new-r".into(),
            expires_at: Some(2),
            account_id: Some("acct".into()),
        };
        persist_credentials(&base, "anthropic-test", &creds).unwrap();
        let loaded = persistence::load_account_at(&base, "anthropic-test").unwrap();
        assert_eq!(loaded.credentials["access_token"], "new");
        assert_eq!(loaded.credentials["refresh_token"], "new-r");
        assert_eq!(loaded.credentials["expires_at"], 2);
        assert_eq!(loaded.credentials["account_id"], "acct");
    }
}
