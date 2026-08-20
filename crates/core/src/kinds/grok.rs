//! xAI Grok (SuperGrok subscription) account kind.
//!
//! Port of the pi-grok OAuth provider: device-code login against
//! `auth.x.ai`, inference through the `cli-chat-proxy.grok.com` gateway, and
//! subscription-credit usage via the proxy's `/billing?format=credits`
//! endpoint. The access token refreshes itself 5 minutes before expiry.

use super::{AccountKind, effort_modes};
use crate::persistence::{self};
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::providers::{GrokProvider, TokenSupplier};
use crate::quota::{
    QuotaAuth, QuotaCapability, QuotaDescriptor, QuotaError, QuotaMeter, QuotaSnapshot, QuotaSource,
};
use crate::types::{ModelCapabilities, ModelCapability, ModelInfo};
use crate::wizard::{Outcome, SetupWizard, Step, WizardError};
use crate::{Provider, ProviderError};
use async_trait::async_trait;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};
use tokio::sync::Mutex;
use uuid::Uuid;

pub const GROK_CLI_PROXY_BASE_URL: &str = "https://cli-chat-proxy.grok.com/v1";
const GROK_DEVICE_CODE_URL: &str = "https://auth.x.ai/oauth2/device/code";
const GROK_DEVICE_TOKEN_URL: &str = "https://auth.x.ai/oauth2/token";
const GROK_CLIENT_ID: &str = "b1a00492-073a-47ea-816f-4c329264a828";
const GROK_SCOPE: &str = "openid profile email offline_access grok-cli:access api:access conversations:read conversations:write";
const GROK_DEFAULT_CLIENT_VERSION: &str = "0.2.101";
const GROK_DEFAULT_CLIENT_NAME: &str = "grok-shell";
const GROK_DEVICE_GRANT_TYPE: &str = "urn:ietf:params:oauth:grant-type:device_code";
const REFRESH_SKEW_SECONDS: i64 = 300;
const MAX_USER_ID_LENGTH: usize = 256;

static REFRESH_LOCKS: OnceLock<std::sync::Mutex<HashMap<String, Arc<Mutex<()>>>>> = OnceLock::new();

fn client_version() -> String {
    std::env::var("PI_XAI_CLIENT_VERSION")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| GROK_DEFAULT_CLIENT_VERSION.to_string())
}

fn client_name() -> String {
    std::env::var("PI_XAI_CLIENT_NAME")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| GROK_DEFAULT_CLIENT_NAME.to_string())
}

fn client_id() -> String {
    std::env::var("PI_XAI_OAUTH_CLIENT_ID")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| GROK_CLIENT_ID.to_string())
}

fn platform_label() -> String {
    let os = match std::env::consts::OS {
        "macos" => "macos",
        "windows" => "windows",
        other => other,
    };
    let arch = match std::env::consts::ARCH {
        "arm64" => "aarch64",
        other => other,
    };
    format!("{os}; {arch}")
}

/// Proxy identity headers for account/usage calls (no model override).
fn proxy_headers() -> Vec<(String, String)> {
    vec![
        (
            "User-Agent".to_string(),
            format!(
                "{}/{} ({})",
                client_name(),
                client_version(),
                platform_label()
            ),
        ),
        ("x-grok-client-identifier".to_string(), client_name()),
        ("x-grok-client-version".to_string(), client_version()),
        ("x-grok-client-mode".to_string(), "interactive".to_string()),
        ("X-XAI-Token-Auth".to_string(), "xai-grok-cli".to_string()),
        (
            "x-authenticateresponse".to_string(),
            "authenticate-response".to_string(),
        ),
    ]
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

fn grok_model(
    id: &str,
    context_window: u32,
    max_output_tokens: u32,
    reasoning: bool,
    effort_capable: bool,
) -> ModelInfo {
    let mut capabilities = ModelCapabilities::from([
        ModelCapability::Text,
        ModelCapability::ToolUse,
        ModelCapability::Image,
    ]);
    if reasoning {
        capabilities.insert(ModelCapability::Reasoning);
    }
    ModelInfo {
        id: id.to_string(),
        context_window,
        max_output_tokens: Some(max_output_tokens),
        capabilities,
        effort_modes: if effort_capable {
            effort_modes(&["low", "medium", "high", "xhigh"])
        } else {
            Vec::new()
        },
    }
}

fn models() -> Vec<ModelInfo> {
    vec![
        grok_model("grok-composer-2.5-fast", 200_000, 30_000, true, false),
        grok_model("grok-build", 500_000, 30_000, true, false),
        grok_model("grok-4.6", 500_000, 131_072, true, true),
        grok_model("grok-4.5", 500_000, 131_072, true, true),
        grok_model("grok-4.3", 1_000_000, 131_072, true, true),
        grok_model("grok-4.20-0309-reasoning", 2_000_000, 131_072, true, false),
        grok_model(
            "grok-4.20-0309-non-reasoning",
            2_000_000,
            131_072,
            false,
            false,
        ),
        grok_model("grok-4.20-multi-agent-0309", 2_000_000, 131_072, true, true),
    ]
}

pub fn schema_template(account_id: &str) -> ProviderSchema {
    ProviderSchema {
        id: account_id.to_string(),
        api_type: ApiType::OpenAI,
        base_url: Some(GROK_CLI_PROXY_BASE_URL.to_string()),
        api_key_env: None,
        models: models(),
    }
}

// ---------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
struct GrokCredentials {
    #[serde(default)]
    access_token: String,
    #[serde(default)]
    refresh_token: String,
    #[serde(default)]
    expires_at: Option<i64>,
    #[serde(default)]
    token_endpoint: Option<String>,
}

pub struct GrokBuildKind;

impl AccountKind for GrokBuildKind {
    fn name(&self) -> &str {
        "grok"
    }

    fn display_name(&self) -> &str {
        "xAI (SuperGrok subscription)"
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
        Ok(Arc::new(GrokProvider::with_auth(
            schema.id.clone(),
            schema.effective_base_url(),
            Arc::new(GrokTokenSupplier::new(
                data_dir.to_path_buf(),
                schema.id.clone(),
                creds,
            )),
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
        Box::<GrokWizard>::default()
    }

    fn quota_capability(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Option<QuotaCapability>, String> {
        self.quota_capability_at(schema, credentials, &persistence::data_dir())
    }

    fn quota_capability_at(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
        data_dir: &std::path::Path,
    ) -> Result<Option<QuotaCapability>, String> {
        let creds = parse_credentials(credentials)?;
        Ok(Some(QuotaCapability {
            descriptor: grok_quota_descriptor(),
            source: Some(Arc::new(GrokQuotaSource {
                account_id: schema.id.clone(),
                auth: Arc::new(GrokTokenSupplier::new(
                    data_dir.to_path_buf(),
                    schema.id.clone(),
                    creds,
                )),
            })),
        }))
    }
}

fn parse_credentials(credentials: &Value) -> Result<GrokCredentials, String> {
    let creds: GrokCredentials = serde_json::from_value(credentials.clone())
        .map_err(|_| "Grok account credentials are malformed".to_string())?;
    if creds.access_token.trim().is_empty() {
        return Err("Grok account is missing an access token".into());
    }
    if creds.refresh_token.trim().is_empty() {
        return Err("Grok account is missing a refresh token".into());
    }
    Ok(creds)
}

// ---------------------------------------------------------------------------
// Token supplier
// ---------------------------------------------------------------------------

struct GrokTokenSupplier {
    data_dir: PathBuf,
    account_id: String,
    credentials: Mutex<GrokCredentials>,
}

impl GrokTokenSupplier {
    fn new(data_dir: PathBuf, account_id: String, credentials: GrokCredentials) -> Self {
        Self {
            data_dir,
            account_id,
            credentials: Mutex::new(credentials),
        }
    }

    async fn refresh_if_needed(&self, creds: &mut GrokCredentials) -> Result<(), ProviderError> {
        let refresh_lock = account_refresh_lock(&self.data_dir, &self.account_id);
        let _refresh_guard = refresh_lock.lock().await;
        // Provider and quota sources hold independent suppliers. Reload the
        // account first so either one sees token rotation persisted by the other.
        if let Ok(record) = persistence::load_account_at(&self.data_dir, &self.account_id)
            && let Ok(latest) = parse_credentials(&record.credentials)
            && credentials_are_newer(&latest, creds)
        {
            *creds = latest;
        }
        if !credentials_need_refresh(creds, chrono::Utc::now().timestamp()) {
            return Ok(());
        }
        let token_endpoint = creds
            .token_endpoint
            .clone()
            .unwrap_or_else(|| GROK_DEVICE_TOKEN_URL.to_string());
        let response = reqwest::Client::new()
            .post(token_endpoint.as_str())
            .header("Accept", "application/json")
            .header("Content-Type", "application/x-www-form-urlencoded")
            .form(&[
                ("grant_type", "refresh_token"),
                ("client_id", client_id().as_str()),
                ("refresh_token", creds.refresh_token.as_str()),
            ])
            .send()
            .await
            .map_err(|error| ProviderError::Http(error.to_string()))?;
        if response.status() == reqwest::StatusCode::UNAUTHORIZED
            || response.status() == reqwest::StatusCode::FORBIDDEN
        {
            return Err(ProviderError::Auth(
                "Grok OAuth refresh was rejected".into(),
            ));
        }
        if !response.status().is_success() {
            return Err(ProviderError::Auth(format!(
                "Grok OAuth refresh failed: HTTP {}",
                response.status()
            )));
        }
        let token: GrokTokenResponse = response
            .json()
            .await
            .map_err(|e| ProviderError::Decode(e.to_string()))?;
        if token.access_token.trim().is_empty() {
            return Err(ProviderError::Auth(
                "Grok OAuth refresh returned an empty access token".into(),
            ));
        }
        creds.access_token = token.access_token;
        if let Some(refresh_token) = token.refresh_token.filter(|token| !token.trim().is_empty()) {
            creds.refresh_token = refresh_token;
        }
        creds.expires_at =
            Some(chrono::Utc::now().timestamp() + token.expires_in.unwrap_or(3600).max(60));
        persist_credentials(&self.data_dir, &self.account_id, creds).map_err(|_| {
            ProviderError::Auth("could not persist refreshed Grok OAuth credentials".into())
        })?;
        Ok(())
    }
}

#[async_trait]
impl TokenSupplier for GrokTokenSupplier {
    async fn headers(&self) -> Result<Vec<(String, String)>, ProviderError> {
        let mut creds = self.credentials.lock().await;
        self.refresh_if_needed(&mut creds).await?;
        Ok(vec![(
            "Authorization".into(),
            format!("Bearer {}", creds.access_token),
        )])
    }
}

fn account_refresh_lock(data_dir: &std::path::Path, account_id: &str) -> Arc<Mutex<()>> {
    let key = format!("{}\0{account_id}", data_dir.display());
    REFRESH_LOCKS
        .get_or_init(|| std::sync::Mutex::new(HashMap::new()))
        .lock()
        .expect("Grok refresh lock registry poisoned")
        .entry(key)
        .or_insert_with(|| Arc::new(Mutex::new(())))
        .clone()
}

fn credentials_are_newer(candidate: &GrokCredentials, current: &GrokCredentials) -> bool {
    candidate.access_token != current.access_token
        && candidate.expires_at.unwrap_or(i64::MIN) >= current.expires_at.unwrap_or(i64::MIN)
}

fn credentials_need_refresh(creds: &GrokCredentials, now: i64) -> bool {
    creds
        .expires_at
        .is_none_or(|expires_at| expires_at <= now + REFRESH_SKEW_SECONDS)
}

fn persist_credentials(
    data_dir: &std::path::Path,
    account_id: &str,
    creds: &GrokCredentials,
) -> Result<(), String> {
    let mut record = persistence::load_account_at(data_dir, account_id)?;
    record.credentials["access_token"] = Value::String(creds.access_token.clone());
    record.credentials["refresh_token"] = Value::String(creds.refresh_token.clone());
    record.credentials["expires_at"] = creds.expires_at.map(Value::from).unwrap_or(Value::Null);
    record.credentials["token_endpoint"] = creds
        .token_endpoint
        .clone()
        .map(Value::from)
        .unwrap_or(Value::Null);
    persistence::save_account_at(data_dir, &record)
}

#[derive(Debug, Deserialize)]
struct GrokTokenResponse {
    access_token: String,
    #[serde(default)]
    refresh_token: Option<String>,
    #[serde(default)]
    expires_in: Option<i64>,
}

// ---------------------------------------------------------------------------
// Quota
// ---------------------------------------------------------------------------

fn grok_quota_descriptor() -> QuotaDescriptor {
    QuotaDescriptor {
        label: "xAI subscription usage".into(),
        auth: QuotaAuth::WebSession,
        meters: vec!["included".into(), "on_demand".into(), "prepaid".into()],
    }
}

struct GrokQuotaSource {
    account_id: String,
    auth: Arc<dyn TokenSupplier>,
}

#[async_trait]
impl QuotaSource for GrokQuotaSource {
    fn descriptor(&self) -> QuotaDescriptor {
        grok_quota_descriptor()
    }

    async fn fetch(&self) -> Result<QuotaSnapshot, QuotaError> {
        let auth_headers = self.auth.headers().await.map_err(|error| match error {
            ProviderError::Auth(_) => QuotaError::InvalidCredentials,
            other => QuotaError::Request(other.to_string()),
        })?;
        let user_id = fetch_user_id(&auth_headers).await?;
        let billing = fetch_billing(&auth_headers, &user_id).await?;
        parse_usage(&billing, &self.account_id)
    }
}

async fn fetch_user_id(auth_headers: &[(String, String)]) -> Result<String, QuotaError> {
    let mut request = reqwest::Client::new()
        .get(format!("{GROK_CLI_PROXY_BASE_URL}/user"))
        .header("Accept", "application/json");
    for (name, value) in proxy_headers() {
        request = request.header(name, value);
    }
    for (name, value) in auth_headers {
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
        return Err(QuotaError::Api(format!("HTTP {status}: {body}")));
    }
    let user: GrokUser =
        serde_json::from_str(&body).map_err(|e| QuotaError::Decode(e.to_string()))?;
    parse_user_id(&user)
}

async fn fetch_billing(
    auth_headers: &[(String, String)],
    user_id: &str,
) -> Result<String, QuotaError> {
    let mut request = reqwest::Client::new()
        .get(format!("{GROK_CLI_PROXY_BASE_URL}/billing?format=credits"))
        .header("Accept", "application/json")
        .header("x-userid", user_id);
    for (name, value) in proxy_headers() {
        request = request.header(name, value);
    }
    for (name, value) in auth_headers {
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
        return Err(QuotaError::Api(format!("HTTP {status}: {body}")));
    }
    Ok(body)
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct GrokUser {
    #[serde(default)]
    user_id: Option<String>,
}

fn parse_user_id(user: &GrokUser) -> Result<String, QuotaError> {
    let Some(user_id) = user.user_id.as_deref() else {
        return Err(QuotaError::Decode(
            "xAI account identity could not be verified".into(),
        ));
    };
    let valid = !user_id.is_empty()
        && user_id.len() <= MAX_USER_ID_LENGTH
        && user_id.bytes().all(|byte| (0x21..=0x7e).contains(&byte));
    if !valid {
        return Err(QuotaError::Decode(
            "xAI account identity could not be verified".into(),
        ));
    }
    Ok(user_id.to_string())
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct GrokBillingResponse {
    #[serde(default)]
    subscription_tier: Option<String>,
    #[serde(default)]
    config: Option<GrokBillingConfig>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct GrokBillingConfig {
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    credit_usage_percent: Option<f64>,
    #[serde(default)]
    monthly_limit: Option<GrokCents>,
    #[serde(default)]
    used: Option<GrokCents>,
    #[serde(default)]
    on_demand_cap: Option<GrokCents>,
    #[serde(default)]
    on_demand_used: Option<GrokCents>,
    #[serde(default)]
    prepaid_balance: Option<GrokCents>,
    #[serde(default)]
    current_period: Option<GrokPeriod>,
    #[serde(default)]
    billing_period_end: Option<String>,
    #[serde(default)]
    product_usage: Option<Vec<GrokProductUsage>>,
}

#[derive(Debug, Deserialize)]
struct GrokCents {
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    val: Option<f64>,
}

#[derive(Debug, Deserialize)]
struct GrokPeriod {
    #[serde(default)]
    end: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct GrokProductUsage {
    #[serde(default)]
    product: Option<String>,
    #[serde(default, deserialize_with = "deserialize_opt_f64")]
    usage_percent: Option<f64>,
}

fn parse_usage(body: &str, fallback_account_id: &str) -> Result<QuotaSnapshot, QuotaError> {
    let payload: GrokBillingResponse =
        serde_json::from_str(body).map_err(|e| QuotaError::Decode(e.to_string()))?;
    let config = payload.config.unwrap_or_default();
    let mut meters = Vec::new();

    let percent = config
        .credit_usage_percent
        .map(|value| value.clamp(0.0, 100.0));
    if config.used.is_some() || config.monthly_limit.is_some() || percent.is_some() {
        let used = config.used.and_then(|c| c.val).map(nonnegative_u64);
        let limit = config
            .monthly_limit
            .and_then(|c| c.val)
            .map(nonnegative_u64);
        let remaining = limit
            .zip(used)
            .map(|(limit, used)| limit.saturating_sub(used));
        meters.push(QuotaMeter {
            id: "included".into(),
            label: "Included".into(),
            window: Some("monthly".into()),
            used,
            limit,
            remaining,
            utilization_percent: percent,
            unit: Some("cents".into()),
            reset_at: config
                .current_period
                .as_ref()
                .and_then(|period| period.end.as_deref())
                .or(config.billing_period_end.as_deref())
                .and_then(parse_iso_time),
            reset_in_seconds: None,
        });
    }
    if config.on_demand_used.is_some() || config.on_demand_cap.is_some() {
        let used = config
            .on_demand_used
            .and_then(|c| c.val)
            .map(nonnegative_u64);
        let limit = config
            .on_demand_cap
            .and_then(|c| c.val)
            .map(nonnegative_u64);
        let remaining = limit
            .zip(used)
            .map(|(limit, used)| limit.saturating_sub(used));
        meters.push(QuotaMeter {
            id: "on_demand".into(),
            label: "On-demand".into(),
            window: None,
            used,
            limit,
            remaining,
            utilization_percent: None,
            unit: Some("cents".into()),
            reset_at: None,
            reset_in_seconds: None,
        });
    }
    if let Some(balance) = config.prepaid_balance.and_then(|c| c.val) {
        meters.push(QuotaMeter {
            id: "prepaid".into(),
            label: "Prepaid balance".into(),
            window: None,
            used: None,
            limit: None,
            remaining: Some(nonnegative_u64(balance)),
            utilization_percent: None,
            unit: Some("cents".into()),
            reset_at: None,
            reset_in_seconds: None,
        });
    }
    for product in config.product_usage.unwrap_or_default() {
        let Some(name) = product.product.filter(|name| !name.trim().is_empty()) else {
            continue;
        };
        let Some(percent) = product.usage_percent else {
            continue;
        };
        meters.push(QuotaMeter {
            id: format!("product:{name}"),
            label: name,
            window: None,
            used: None,
            limit: None,
            remaining: None,
            utilization_percent: Some(percent.clamp(0.0, 100.0)),
            unit: Some("percent".into()),
            reset_at: None,
            reset_in_seconds: None,
        });
    }
    if meters.is_empty() {
        return Err(QuotaError::Decode(
            "xAI usage response contained no quota meters".into(),
        ));
    }
    Ok(QuotaSnapshot {
        account_id: fallback_account_id.to_string(),
        observed_at: chrono::Utc::now(),
        meters,
        note: payload
            .subscription_tier
            .map(|tier| format!("tier: {tier}")),
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

fn parse_iso_time(value: &str) -> Option<chrono::DateTime<chrono::Utc>> {
    chrono::DateTime::parse_from_rfc3339(value)
        .ok()
        .map(|time| time.with_timezone(&chrono::Utc))
}

// ---------------------------------------------------------------------------
// Device-code wizard
// ---------------------------------------------------------------------------

#[derive(Debug, Deserialize)]
struct DeviceCodeResponse {
    device_code: String,
    user_code: String,
    verification_uri: String,
    #[serde(default)]
    verification_uri_complete: Option<String>,
    expires_in: i64,
    #[serde(default)]
    interval: Option<i64>,
}

struct DeviceState {
    device_code: String,
    token_endpoint: String,
    interval: Duration,
    next_poll_at: Instant,
    deadline: Instant,
}

#[derive(Default)]
pub struct GrokWizard {
    state: Option<DeviceState>,
}

impl GrokWizard {
    async fn request_device_code(&self) -> Result<DeviceCodeResponse, WizardError> {
        let response = reqwest::Client::new()
            .post(GROK_DEVICE_CODE_URL)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .header("Accept", "application/json")
            .header("x-grok-client-version", client_version())
            .header("x-grok-client-surface", "cli")
            .form(&[
                ("client_id", client_id().as_str()),
                ("scope", GROK_SCOPE),
                ("referrer", "grok-build"),
            ])
            .send()
            .await
            .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;
        if !response.status().is_success() {
            return Err(WizardError::InvalidAnswer(format!(
                "xAI device-code request failed: HTTP {}",
                response.status()
            )));
        }
        response
            .json()
            .await
            .map_err(|error| WizardError::InvalidAnswer(error.to_string()))
    }
}

#[async_trait]
impl SetupWizard for GrokWizard {
    async fn start(&mut self) -> Step {
        match self.request_device_code().await {
            Ok(device) => {
                let interval =
                    Duration::from_secs(device.interval.unwrap_or(5).clamp(1, 120) as u64);
                let now = Instant::now();
                self.state = Some(DeviceState {
                    device_code: device.device_code,
                    token_endpoint: GROK_DEVICE_TOKEN_URL.to_string(),
                    interval,
                    // Poll only after the first interval, mirroring pi-grok's
                    // "sleep first" behavior for a fresh code.
                    next_poll_at: now + interval,
                    deadline: now + Duration::from_secs(device.expires_in.max(60) as u64),
                });
                let url = device
                    .verification_uri_complete
                    .unwrap_or(device.verification_uri);
                Step::OpenUrl {
                    label: format!(
                        "open the link and enter code {} (or approve at the page)",
                        device.user_code
                    ),
                    url,
                }
            }
            Err(error) => Step::Prompt {
                label: format!("xAI login unavailable: {error}"),
                secret: false,
            },
        }
    }

    async fn answer(&mut self, _input: String) -> Result<Outcome, WizardError> {
        Err(WizardError::InvalidAnswer(
            "xAI login completes in the browser".into(),
        ))
    }

    async fn poll(&mut self) -> Result<Option<Outcome>, WizardError> {
        let Some(state) = self.state.as_ref() else {
            return Ok(None);
        };
        if Instant::now() < state.next_poll_at {
            return Ok(None);
        }
        let response = reqwest::Client::new()
            .post(state.token_endpoint.as_str())
            .header("Content-Type", "application/x-www-form-urlencoded")
            .header("Accept", "application/json")
            .header("x-grok-client-version", client_version())
            .header("x-grok-client-surface", "cli")
            .form(&[
                ("grant_type", GROK_DEVICE_GRANT_TYPE),
                ("device_code", state.device_code.as_str()),
                ("client_id", client_id().as_str()),
            ])
            .send()
            .await
            .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;

        let status = response.status();
        let body = response
            .text()
            .await
            .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;

        if status.is_success() {
            let token: GrokTokenResponse = serde_json::from_str(&body)
                .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;
            let refresh_token = token
                .refresh_token
                .filter(|token| !token.trim().is_empty())
                .ok_or_else(|| {
                    WizardError::InvalidAnswer(
                        "xAI device login did not return a refresh token".into(),
                    )
                })?;
            let expires_in = token.expires_in.unwrap_or(3600).max(60);
            let account_id = format!("grok-{}", Uuid::new_v4());
            return Ok(Some(Outcome::Done {
                schema: schema_template(&account_id),
                credentials: serde_json::json!({
                    "access_token": token.access_token,
                    "refresh_token": refresh_token,
                    "expires_at": chrono::Utc::now().timestamp() + expires_in - REFRESH_SKEW_SECONDS,
                    "token_endpoint": GROK_DEVICE_TOKEN_URL,
                }),
            }));
        }

        // Advance the poll clock before handling the terminal-vs-pending split.
        let state = self.state.as_mut().expect("state checked above");
        state.next_poll_at = Instant::now() + state.interval;
        let error_code: String = serde_json::from_str::<Value>(&body)
            .ok()
            .and_then(|value| {
                value
                    .get("error")
                    .and_then(Value::as_str)
                    .map(str::to_string)
            })
            .unwrap_or_default();
        match error_code.as_str() {
            "authorization_pending" => {
                if Instant::now() > state.deadline {
                    return Err(WizardError::InvalidAnswer(
                        "xAI device code expired. Restart /login.".into(),
                    ));
                }
                Ok(None)
            }
            "slow_down" => {
                state.interval += Duration::from_secs(5);
                state.next_poll_at = Instant::now() + state.interval;
                Ok(None)
            }
            "access_denied" => Err(WizardError::InvalidAnswer(
                "xAI device login denied.".into(),
            )),
            "expired_token" => Err(WizardError::InvalidAnswer(
                "xAI device code expired. Restart /login.".into(),
            )),
            other => Err(WizardError::InvalidAnswer(format!(
                "xAI device login failed: {}",
                if other.is_empty() {
                    format!("HTTP {status}")
                } else {
                    other.to_string()
                }
            ))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_has_grok_models_with_correct_windows_and_efforts() {
        let schema = schema_template("grok-account");
        assert_eq!(schema.api_type, ApiType::OpenAI);
        assert_eq!(schema.base_url.as_deref(), Some(GROK_CLI_PROXY_BASE_URL));
        let ids: Vec<_> = schema
            .models
            .iter()
            .map(|model| model.id.as_str())
            .collect();
        assert_eq!(
            ids,
            [
                "grok-composer-2.5-fast",
                "grok-build",
                "grok-4.6",
                "grok-4.5",
                "grok-4.3",
                "grok-4.20-0309-reasoning",
                "grok-4.20-0309-non-reasoning",
                "grok-4.20-multi-agent-0309",
            ]
        );
        let build = schema.model("grok-build").unwrap();
        assert_eq!(build.context_window, 500_000);
        assert_eq!(build.max_output_tokens, Some(30_000));
        assert!(build.supports(ModelCapability::Image));
        assert!(build.supports(ModelCapability::Reasoning));
        assert!(build.effort_modes.is_empty());
        let four_five = schema.model("grok-4.5").unwrap();
        assert_eq!(
            four_five
                .effort_modes
                .iter()
                .map(|mode| mode.name.as_str())
                .collect::<Vec<_>>(),
            ["low", "medium", "high", "xhigh"]
        );
        let non_reasoning = schema.model("grok-4.20-0309-non-reasoning").unwrap();
        assert!(!non_reasoning.supports(ModelCapability::Reasoning));
    }

    #[test]
    fn credentials_missing_tokens_are_rejected() {
        assert!(parse_credentials(&serde_json::json!({})).is_err());
        assert!(
            parse_credentials(&serde_json::json!({
                "access_token": "a",
                "refresh_token": "r",
            }))
            .is_ok()
        );
    }

    #[test]
    fn near_expiry_tokens_refresh_proactively() {
        let mut creds = GrokCredentials {
            access_token: "a".into(),
            refresh_token: "r".into(),
            expires_at: None,
            token_endpoint: None,
        };
        assert!(credentials_need_refresh(&creds, 1_000));
        creds.expires_at = Some(1_299);
        assert!(credentials_need_refresh(&creds, 1_000));
        creds.expires_at = Some(1_301);
        assert!(!credentials_need_refresh(&creds, 1_000));
    }

    #[test]
    fn user_id_validation_rejects_unprintable_or_missing() {
        assert_eq!(
            parse_user_id(&GrokUser {
                user_id: Some("abc-123".into())
            })
            .unwrap(),
            "abc-123"
        );
        assert!(parse_user_id(&GrokUser { user_id: None }).is_err());
        assert!(
            parse_user_id(&GrokUser {
                user_id: Some(String::new())
            })
            .is_err()
        );
        assert!(
            parse_user_id(&GrokUser {
                user_id: Some("bad id".into())
            })
            .is_err()
        );
    }

    #[test]
    fn quota_parser_maps_usage_meters() {
        let snapshot = parse_usage(
            &serde_json::json!({
                "subscriptionTier": "supergrok",
                "onDemandEnabled": true,
                "config": {
                    "creditUsagePercent": 62.5,
                    "monthlyLimit": { "val": 5000 },
                    "used": { "val": 3125 },
                    "onDemandCap": { "val": 2000 },
                    "onDemandUsed": { "val": 400 },
                    "prepaidBalance": { "val": 1000 },
                    "currentPeriod": { "type": "USAGE_PERIOD_TYPE_MONTHLY", "start": "2026-08-01T00:00:00Z", "end": "2026-09-01T00:00:00Z" },
                    "productUsage": [
                        { "product": "GrokBuild", "usagePercent": 10.0 }
                    ]
                }
            })
            .to_string(),
            "grok-account",
        )
        .unwrap();
        assert_eq!(snapshot.account_id, "grok-account");
        assert_eq!(
            snapshot
                .meters
                .iter()
                .map(|meter| meter.id.as_str())
                .collect::<Vec<_>>(),
            ["included", "on_demand", "prepaid", "product:GrokBuild"]
        );
        let included = &snapshot.meters[0];
        assert_eq!(included.used, Some(3125));
        assert_eq!(included.limit, Some(5000));
        assert_eq!(included.remaining, Some(1875));
        assert_eq!(included.utilization_percent, Some(62.5));
        assert!(included.reset_at.is_some());
        assert_eq!(snapshot.meters[3].utilization_percent, Some(10.0));
    }

    #[test]
    fn quota_parser_rejects_empty_response() {
        assert!(parse_usage(&serde_json::json!({}).to_string(), "grok-account").is_err());
    }
}
