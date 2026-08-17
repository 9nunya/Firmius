//! Claude Pro/Max subscription account kind using Anthropic OAuth.

use super::{AccountKind, effort_modes, model};
use crate::persistence::{self};
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::providers::{AnthropicProvider, TokenSupplier};
use crate::quota::{
    QuotaAuth, QuotaCapability, QuotaDescriptor, QuotaError, QuotaMeter, QuotaSnapshot, QuotaSource,
};
use crate::types::{ModelCapability, ModelInfo};
use crate::wizard::{Outcome, SetupWizard, Step, WizardError};
use crate::{Provider, ProviderError};
use async_trait::async_trait;
use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use chrono::{TimeZone, Utc};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};
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
const CLAUDE_CODE_USER_AGENT: &str = "claude-cli/2.1.206";
const REFRESH_SKEW_SECONDS: i64 = 300;
static REFRESH_LOCKS: OnceLock<std::sync::Mutex<HashMap<String, Arc<Mutex<()>>>>> = OnceLock::new();

fn models() -> Vec<ModelInfo> {
    let mut models = vec![
        model("claude-opus-5", 1_000_000, 64_000),
        model("claude-opus-4-8", 1_000_000, 64_000),
        model("claude-sonnet-5", 1_000_000, 64_000),
        model("claude-fable-5", 1_000_000, 64_000),
    ];
    for info in &mut models {
        info.capabilities.insert(ModelCapability::Image);
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
    #[serde(default)]
    access_token: String,
    #[serde(default)]
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
        Ok(Arc::new(
            AnthropicProvider::with_oauth(
                schema.id.clone(),
                Arc::new(AnthropicOAuthTokenSupplier::new(
                    data_dir.to_path_buf(),
                    schema.id.clone(),
                    creds,
                )),
            )
            .with_base_url(schema.effective_base_url()),
        ))
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
                account_id: schema.id.clone(),
                auth: Arc::new(AnthropicOAuthTokenSupplier::new(
                    data_dir.to_path_buf(),
                    schema.id.clone(),
                    creds,
                )),
            })),
        }))
    }

    fn quota_capability(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Option<QuotaCapability>, String> {
        self.quota_capability_at(schema, credentials, &persistence::data_dir())
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
        let refresh_lock = account_refresh_lock(&self.data_dir, &self.account_id);
        let _refresh_guard = refresh_lock.lock().await;
        // Providers and quota sources have independent suppliers. Reload the
        // account first so either one sees token rotation persisted by the other.
        if let Ok(record) = persistence::load_account_at(&self.data_dir, &self.account_id)
            && let Ok(latest) = parse_credentials(&record.credentials)
            && credentials_are_newer(&latest, creds)
        {
            *creds = latest;
        }
        let should_refresh = credentials_need_refresh(creds, Utc::now().timestamp());
        if !should_refresh {
            return Ok(());
        }
        let response = reqwest::Client::new()
            .post(ANTHROPIC_TOKEN_URL)
            .header("Accept", "application/json")
            .header("Content-Type", "application/json")
            .header("User-Agent", CLAUDE_CODE_USER_AGENT)
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
        if token.access_token.trim().is_empty() {
            return Err(ProviderError::Auth(
                "Anthropic OAuth refresh returned an empty access token".into(),
            ));
        }
        creds.access_token = token.access_token;
        if let Some(refresh_token) = token.refresh_token.filter(|token| !token.trim().is_empty()) {
            creds.refresh_token = refresh_token;
        }
        creds.expires_at = Some(Utc::now().timestamp() + token.expires_in.unwrap_or(3600).max(60));
        persist_credentials(&self.data_dir, &self.account_id, creds).map_err(|_| {
            ProviderError::Auth("could not persist refreshed Anthropic OAuth credentials".into())
        })?;
        Ok(())
    }
}

fn account_refresh_lock(data_dir: &std::path::Path, account_id: &str) -> Arc<Mutex<()>> {
    let key = format!("{}\0{account_id}", data_dir.display());
    REFRESH_LOCKS
        .get_or_init(|| std::sync::Mutex::new(HashMap::new()))
        .lock()
        .expect("Anthropic refresh lock registry poisoned")
        .entry(key)
        .or_insert_with(|| Arc::new(Mutex::new(())))
        .clone()
}

fn credentials_are_newer(
    candidate: &AnthropicOAuthCredentials,
    current: &AnthropicOAuthCredentials,
) -> bool {
    candidate.access_token != current.access_token
        && candidate.expires_at.unwrap_or(i64::MIN) >= current.expires_at.unwrap_or(i64::MIN)
}

fn credentials_need_refresh(creds: &AnthropicOAuthCredentials, now: i64) -> bool {
    creds
        .expires_at
        .is_none_or(|expires_at| expires_at <= now + REFRESH_SKEW_SECONDS)
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
    account_id: String,
    auth: Arc<dyn TokenSupplier>,
}

#[async_trait]
impl QuotaSource for AnthropicQuotaSource {
    fn descriptor(&self) -> QuotaDescriptor {
        anthropic_quota_descriptor()
    }

    async fn fetch(&self) -> Result<QuotaSnapshot, QuotaError> {
        let headers = self.auth.headers().await.map_err(|error| match error {
            ProviderError::Auth(_) => QuotaError::InvalidCredentials,
            other => QuotaError::Request(other.to_string()),
        })?;
        let mut request = reqwest::Client::new()
            .get("https://api.anthropic.com/api/oauth/usage")
            .header("Accept", "application/json")
            .header("User-Agent", "claude-cli (external, cli)")
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
        parse_usage(&body, &self.account_id)
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
    used: Option<Value>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    used_amount: Option<f64>,
    #[serde(default)]
    limit: Option<Value>,
    #[serde(default)]
    remaining: Option<Value>,
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

fn parse_usage(body: &str, fallback_account_id: &str) -> Result<QuotaSnapshot, QuotaError> {
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
        let used = spend
            .used
            .as_ref()
            .and_then(quota_amount)
            .map(nonnegative_u64)
            .or_else(|| spend.used_amount.map(nonnegative_u64));
        let limit = spend
            .limit
            .as_ref()
            .and_then(quota_amount)
            .map(nonnegative_u64);
        let remaining = spend
            .remaining
            .as_ref()
            .and_then(quota_amount)
            .map(nonnegative_u64)
            .or_else(|| {
                limit
                    .zip(used)
                    .map(|(limit, used)| limit.saturating_sub(used))
            });
        meters.push(QuotaMeter {
            id: "spend".into(),
            label: "Spend".into(),
            window: None,
            used,
            limit,
            remaining,
            utilization_percent: spend.percent,
            unit: Some(
                spend
                    .used
                    .as_ref()
                    .and_then(money_currency)
                    .unwrap_or_else(|| "usd".into())
                    .to_lowercase(),
            ),
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
        account_id: payload
            .account_id
            .unwrap_or_else(|| fallback_account_id.to_string()),
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

fn money_minor_as_major(value: &Value) -> Option<f64> {
    let money: AnthropicMoney = serde_json::from_value(value.clone()).ok()?;
    let amount = money.amount_minor?;
    let exponent = money.exponent.unwrap_or(0);
    Some(amount * 10f64.powi(-exponent))
}

fn quota_amount(value: &Value) -> Option<f64> {
    value_as_f64(value).or_else(|| money_minor_as_major(value))
}

fn money_currency(value: &Value) -> Option<String> {
    serde_json::from_value::<AnthropicMoney>(value.clone())
        .ok()
        .and_then(|money| money.currency)
}

fn parse_limits(value: Option<Value>, meters: &mut Vec<QuotaMeter>) -> Result<(), QuotaError> {
    let Some(value) = value else {
        return Ok(());
    };
    if let Some(items) = value.as_array() {
        for item in items {
            if item.get("kind").and_then(Value::as_str) != Some("weekly_scoped") {
                continue;
            }
            let percent = item.get("percent").and_then(value_as_f64);
            let label = item
                .pointer("/scope/model/display_name")
                .and_then(Value::as_str)
                .map(|model| format!("Weekly scoped ({model})"))
                .unwrap_or_else(|| "Weekly scoped".into());
            meters.push(QuotaMeter {
                id: "weekly_scoped".into(),
                label,
                window: None,
                used: None,
                limit: None,
                remaining: None,
                utilization_percent: percent,
                unit: Some("percent".into()),
                reset_at: item
                    .get("resets_at")
                    .and_then(Value::as_str)
                    .and_then(parse_iso_time),
                reset_in_seconds: None,
            });
        }
        return Ok(());
    }
    if let Ok(object) = serde_json::from_value::<AnthropicLimitsObject>(value)
        && let Some(window) = object.weekly_scoped
    {
        meters.push(window.meter("weekly_scoped", "Weekly scoped"));
    }
    Ok(())
}

fn value_as_f64(value: &Value) -> Option<f64> {
    value
        .as_f64()
        .or_else(|| value.as_str().and_then(|value| value.parse().ok()))
}

fn parse_iso_time(value: &str) -> Option<chrono::DateTime<Utc>> {
    chrono::DateTime::parse_from_rfc3339(value)
        .ok()
        .map(|time| time.with_timezone(&Utc))
}

fn epoch(seconds: i64) -> Option<chrono::DateTime<Utc>> {
    Utc.timestamp_opt(seconds, 0).single()
}

#[derive(Default)]
pub struct AnthropicSubscriptionWizard {
    callback: Option<oneshot::Receiver<Result<AnthropicOAuthCredentials, String>>>,
    cancel: Option<oneshot::Sender<()>>,
    started: bool,
}

impl AnthropicSubscriptionWizard {
    async fn start_server(&mut self) -> Result<String, String> {
        if let Some(cancel) = self.cancel.take() {
            let _ = cancel.send(());
        }
        let listener = TcpListener::bind(("127.0.0.1", 53692))
            .await
            .map_err(|e| format!("could not bind OAuth callback on localhost:53692: {e}"))?;
        let verifier = Uuid::new_v4().to_string() + &Uuid::new_v4().to_string();
        let challenge = URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()));
        let (tx, rx) = oneshot::channel();
        let (cancel_tx, cancel_rx) = oneshot::channel();
        self.callback = Some(rx);
        self.cancel = Some(cancel_tx);
        let expected_state = verifier.clone();
        tokio::spawn(async move {
            let (mut stream, _) = tokio::select! {
                connection = listener.accept() => match connection {
                    Ok(connection) => connection,
                    Err(error) => {
                        let _ = tx.send(Err(error.to_string()));
                        return;
                    }
                },
                _ = cancel_rx => return,
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
                        "state": expected_state,
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
                if token.access_token.trim().is_empty() {
                    return Err("Anthropic token response contained an empty access token".into());
                }
                let refresh_token = token
                    .refresh_token
                    .filter(|token| !token.trim().is_empty())
                    .ok_or_else(|| {
                        "Anthropic token response did not include a refresh token".to_string()
                    })?;
                Ok(AnthropicOAuthCredentials {
                    access_token: token.access_token,
                    refresh_token,
                    expires_at: Some(
                        Utc::now().timestamp() + token.expires_in.unwrap_or(3600).max(60),
                    ),
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
        Ok(authorization_url(&verifier, &challenge))
    }
}

impl Drop for AnthropicSubscriptionWizard {
    fn drop(&mut self) {
        if let Some(cancel) = self.cancel.take() {
            let _ = cancel.send(());
        }
    }
}

fn authorization_url(verifier: &str, challenge: &str) -> String {
    let mut url = reqwest::Url::parse(ANTHROPIC_AUTH_URL).expect("Anthropic auth URL is static");
    url.query_pairs_mut()
        .append_pair("response_type", "code")
        .append_pair("client_id", ANTHROPIC_CLIENT_ID)
        .append_pair("redirect_uri", ANTHROPIC_REDIRECT_URI)
        .append_pair("scope", ANTHROPIC_SCOPES)
        .append_pair("code", "true")
        .append_pair("code_challenge", challenge)
        .append_pair("code_challenge_method", "S256")
        .append_pair("state", verifier);
    url.to_string()
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
            assert_eq!(model.context_window, 1_000_000);
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
    fn authorization_url_matches_anthropic_pkce_contract() {
        let url = reqwest::Url::parse(&authorization_url("verifier", "challenge")).unwrap();
        let query = url
            .query_pairs()
            .collect::<std::collections::HashMap<_, _>>();
        assert_eq!(url.origin().ascii_serialization(), "https://claude.ai");
        assert_eq!(url.path(), "/oauth/authorize");
        assert_eq!(query.get("response_type").map(|v| v.as_ref()), Some("code"));
        assert_eq!(
            query.get("client_id").map(|v| v.as_ref()),
            Some(ANTHROPIC_CLIENT_ID)
        );
        assert_eq!(
            query.get("redirect_uri").map(|v| v.as_ref()),
            Some(ANTHROPIC_REDIRECT_URI)
        );
        assert_eq!(
            query.get("scope").map(|v| v.as_ref()),
            Some(ANTHROPIC_SCOPES)
        );
        assert_eq!(query.get("code").map(|v| v.as_ref()), Some("true"));
        assert_eq!(
            query.get("code_challenge").map(|v| v.as_ref()),
            Some("challenge")
        );
        assert_eq!(
            query.get("code_challenge_method").map(|v| v.as_ref()),
            Some("S256")
        );
        assert_eq!(query.get("state").map(|v| v.as_ref()), Some("verifier"));
    }

    #[test]
    fn quota_parser_maps_all_known_meters() {
        let snapshot = parse_usage(
            &serde_json::json!({
                "five_hour": { "utilization": 10.0, "resets_at": "2027-01-15T08:00:00Z" },
                "seven_day": { "utilization": 20.0, "resets_at": "2027-01-16T08:00:00Z" },
                "seven_day_opus": { "utilization": 30.0, "resets_at": "2027-01-16T08:00:00Z" },
                "seven_day_sonnet": { "utilization": 40.0, "resets_at": "2027-01-16T08:00:00Z" },
                "limits": [{ "kind": "weekly_scoped", "percent": 55.5, "resets_at": "2027-01-16T08:00:00Z", "scope": { "model": { "display_name": "Opus" } } }],
                "spend": {
                    "enabled": true,
                    "percent": 65.0,
                    "used": { "amount_minor": 650, "exponent": 2, "currency": "USD" },
                    "limit": { "amount_minor": 4000, "exponent": 2, "currency": "USD" },
                    "remaining": { "amount_minor": 3350, "exponent": 2, "currency": "USD" }
                }
            })
            .to_string(),
            "anthropic-account",
        )
        .unwrap();
        assert_eq!(snapshot.account_id, "anthropic-account");
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
        assert_eq!(snapshot.meters[0].remaining, None);
        assert_eq!(snapshot.meters[0].utilization_percent, Some(10.0));
        assert!(snapshot.meters[0].reset_at.is_some());
        assert_eq!(snapshot.meters[4].label, "Weekly scoped (Opus)");
        assert_eq!(snapshot.meters[4].utilization_percent, Some(55.5));
        assert_eq!(snapshot.meters[5].used, Some(6));
        assert_eq!(snapshot.meters[5].limit, Some(40));
        assert_eq!(snapshot.meters[5].remaining, Some(33));
        assert_eq!(snapshot.meters[5].utilization_percent, Some(65.0));
        assert_eq!(snapshot.meters[5].unit.as_deref(), Some("usd"));
    }

    #[test]
    fn quota_parser_accepts_live_disabled_spend_shape() {
        let snapshot = parse_usage(
            &serde_json::json!({
                "five_hour": { "utilization": 2.0, "resets_at": "2026-08-17T22:09:59.687686+00:00" },
                "seven_day": { "utilization": 8.0, "resets_at": "2026-08-22T05:59:59.687710+00:00" },
                "limits": [
                    { "kind": "session", "percent": 2, "resets_at": "2026-08-17T22:09:59.687686+00:00" },
                    { "kind": "weekly_all", "percent": 8, "resets_at": "2026-08-22T05:59:59.687710+00:00" }
                ],
                "spend": {
                    "used": { "amount_minor": 0, "currency": "USD", "exponent": 2 },
                    "limit": { "amount_minor": 4000, "currency": "USD", "exponent": 2 },
                    "percent": 0,
                    "enabled": false,
                    "cap": { "money": null, "credits": { "amount_minor": 4000, "exponent": 2 } },
                    "balance": null
                }
            })
            .to_string(),
            "anthropic-account",
        )
        .unwrap();

        assert_eq!(
            snapshot
                .meters
                .iter()
                .map(|meter| meter.id.as_str())
                .collect::<Vec<_>>(),
            ["five_hour", "seven_day"]
        );
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
    fn missing_or_near_expiry_tokens_refresh_proactively() {
        let mut creds = AnthropicOAuthCredentials {
            access_token: "access".into(),
            refresh_token: "refresh".into(),
            expires_at: None,
            account_id: None,
        };
        assert!(credentials_need_refresh(&creds, 1_000));
        creds.expires_at = Some(1_299);
        assert!(credentials_need_refresh(&creds, 1_000));
        creds.expires_at = Some(1_301);
        assert!(!credentials_need_refresh(&creds, 1_000));
    }

    #[tokio::test]
    async fn token_supplier_reloads_a_newer_persisted_rotation_before_refreshing() {
        let base = std::env::temp_dir().join(format!("firmius-anthropic-test-{}", Uuid::new_v4()));
        std::fs::create_dir_all(&base).unwrap();
        persistence::save_account_at(
            &base,
            &AccountRecord {
                id: "anthropic-test".into(),
                kind: "anthropic".into(),
                schema: schema_template("anthropic-test"),
                credentials: serde_json::json!({
                    "access_token": "fresh",
                    "refresh_token": "fresh-refresh",
                    "expires_at": Utc::now().timestamp() + 3600,
                }),
            },
        )
        .unwrap();
        let supplier = AnthropicOAuthTokenSupplier::new(
            base,
            "anthropic-test".into(),
            AnthropicOAuthCredentials {
                access_token: "stale".into(),
                refresh_token: "stale-refresh".into(),
                expires_at: Some(Utc::now().timestamp() - 1),
                account_id: None,
            },
        );

        assert_eq!(
            supplier.headers().await.unwrap(),
            [("Authorization".into(), "Bearer fresh".into())]
        );
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
