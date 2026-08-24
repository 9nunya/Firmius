//! Freebuff — free-mode Codebuff gateway with browser device-code login.
//!
//! Login is the Freebuff CLI device-code flow against `freebuff.com`.
//! Completions and quota ride the Codebuff OpenAI-compatible API
//! (`https://codebuff.com/api/v1`) with the resulting bearer token.
//!
//! The real quota is the **premium-session pool** (and sibling pools such as
//! DeepSeek's one-a-day ceiling), returned by `GET /api/v1/freebuff/session`
//! as `rateLimitsByModel`. A 1-hour wall-clock window exists on the wire as
//! `expiresAt` but is not a backend kill switch — completions keep working
//! until a real pool or spend/message ceiling is exhausted. Firmius therefore
//! surfaces the session pools and never treats the hour as a hard meter.

use super::{AccountKind, effort_modes, model};
use crate::providers::append_openai_messages;
use crate::providers::schema::{ApiType, ProviderSchema};
use crate::providers::{dump_provider_request, parse_sse_lines};
use crate::quota::{
    QuotaAuth, QuotaCapability, QuotaDescriptor, QuotaError, QuotaMeter, QuotaSnapshot, QuotaSource,
};
use crate::types::{
    Message, MessagePart, MessageRole, ModelCapability, ModelInfo, ProviderRequest, StopReason,
    Usage,
};
use crate::wizard::{Outcome, SetupWizard, Step, WizardError};
use crate::{Provider, ProviderError, ProviderEvent};
use async_trait::async_trait;
use chrono::{DateTime, Utc};
use futures::{StreamExt, stream::BoxStream};
use serde::Deserialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::Mutex;
use uuid::Uuid;

pub const FREEBUFF_LOGIN_URL: &str = "https://freebuff.com";
pub const FREEBUFF_API_URL: &str = "https://www.codebuff.com";
pub const FREEBUFF_COMPLETIONS_BASE: &str = "https://www.codebuff.com/api/v1";

const LOGIN_CODE_PATH: &str = "/api/auth/cli/code";
const LOGIN_STATUS_PATH: &str = "/api/auth/cli/status";
const SESSION_PATH: &str = "/api/v1/freebuff/session";
const USAGE_PATH: &str = "/api/v1/usage";
const INCLUDE_UNUSED_RATE_LIMITS: &str = "x-freebuff-include-unused-rate-limits";
const MODEL_HEADER: &str = "x-freebuff-model";
const INSTANCE_HEADER: &str = "x-freebuff-instance-id";
const CLI_USER_AGENT: &str = "ai-sdk/openai-compatible/1.0.0/codebuff";
const BUFFY_SYSTEM: &str = "You are Buffy, the coding agent behind Codebuff. You help users with software engineering tasks: fixing bugs, adding functionality, refactoring, and explaining code.";

fn agent_id_for_model(model: &str) -> &'static str {
    match model {
        "openai/gpt-5.6-luna" => "base3-free-luna",
        "deepseek/deepseek-v4-flash" => "base3-free-deepseek-flash",
        "deepseek/deepseek-v4-pro" => "base3-free-deepseek",
        "minimax/minimax-m3" => "base3-free-minimax-m3",
        "mimo/mimo-v2.5" => "base3-free-mimo",
        "z-ai/glm-5.2" => "base3-free-glm",
        _ => "base3-free-luna",
    }
}

fn openrouter_provider(model: &str) -> Value {
    match model {
        "openai/gpt-5.6-luna" => json!({
            "order": ["openai"],
            "allow_fallbacks": true,
            "max_price": { "prompt": 0.5, "completion": 3.0 },
            "data_collection": "deny",
        }),
        "minimax/minimax-m3" => json!({
            "order": ["minimax"],
            "allow_fallbacks": true,
            "data_collection": "deny",
        }),
        "mimo/mimo-v2.5" => json!({
            "order": ["xiaomi", "novita"],
            "allow_fallbacks": true,
            "data_collection": "deny",
        }),
        _ => json!({
            "allow_fallbacks": true,
            "data_collection": "deny",
        }),
    }
}

fn models() -> Vec<ModelInfo> {
    let mut models = vec![
        model("openai/gpt-5.6-luna", 1_000_000, 128_000),
        model("deepseek/deepseek-v4-flash", 1_048_576, 384_000),
        model("minimax/minimax-m3", 524_288, 131_072),
        model("mimo/mimo-v2.5", 131_072, 128_000),
        model("deepseek/deepseek-v4-pro", 1_048_576, 384_000),
        model("z-ai/glm-5.2", 131_072, 131_072),
    ];
    for info in &mut models {
        match info.id.as_str() {
            "openai/gpt-5.6-luna" => {
                info.capabilities.insert(ModelCapability::Image);
                info.effort_modes = effort_modes(&["low", "medium", "high", "xhigh", "max"]);
            }
            "deepseek/deepseek-v4-flash" | "deepseek/deepseek-v4-pro" => {
                info.effort_modes = effort_modes(&["low", "high", "max"]);
            }
            "minimax/minimax-m3" | "mimo/mimo-v2.5" => {
                info.capabilities.insert(ModelCapability::Image);
            }
            _ => {}
        }
    }
    models
}

pub fn schema_template(account_id: &str) -> ProviderSchema {
    ProviderSchema {
        id: account_id.to_string(),
        api_type: ApiType::OpenAI,
        base_url: Some(FREEBUFF_COMPLETIONS_BASE.to_string()),
        api_key_env: None,
        models: models(),
    }
}

fn fingerprint_id() -> String {
    let mut hasher = Sha256::new();
    hasher.update(b"firmius-freebuff");
    for key in ["USER", "USERNAME", "LOGNAME", "HOSTNAME"] {
        if let Ok(value) = std::env::var(key) {
            hasher.update(value.as_bytes());
        }
    }
    if let Ok(home) = std::env::var("HOME").or_else(|_| std::env::var("USERPROFILE")) {
        hasher.update(home.as_bytes());
    }
    format!("{:x}", hasher.finalize())
}

fn parse_auth_token(credentials: &Value) -> Result<String, String> {
    credentials
        .get("auth_token")
        .or_else(|| credentials.get("access_token"))
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|token| !token.is_empty())
        .map(str::to_owned)
        .ok_or_else(|| "Freebuff account is missing an auth token".to_string())
}

pub struct FreebuffKind;

impl AccountKind for FreebuffKind {
    fn name(&self) -> &str {
        "freebuff"
    }

    fn display_name(&self) -> &str {
        "Freebuff"
    }

    fn build_provider(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Arc<dyn Provider>, String> {
        let token = parse_auth_token(credentials)?;
        Ok(Arc::new(FreebuffProvider::new(schema.id.clone(), token)))
    }

    fn refresh_schema(&self, schema: &ProviderSchema) -> ProviderSchema {
        let refreshed = schema_template(&schema.id);
        // Always rewrite onto www.codebuff.com. Apex codebuff.com 301s POSTs
        // and reqwest does not follow them, which showed up as 401 Unauthorized.
        refreshed
    }

    fn wizard(&self) -> Box<dyn SetupWizard> {
        Box::<FreebuffWizard>::default()
    }

    fn quota_capability(
        &self,
        schema: &ProviderSchema,
        credentials: &Value,
    ) -> Result<Option<QuotaCapability>, String> {
        let token = parse_auth_token(credentials)?;
        Ok(Some(QuotaCapability {
            descriptor: freebuff_quota_descriptor(),
            source: Some(Arc::new(FreebuffQuotaSource {
                account_id: schema.id.clone(),
                token,
            })),
        }))
    }
}

#[derive(Clone)]
struct LiveSlot {
    model: String,
    instance_id: String,
    run_id: String,
}

struct FreebuffProvider {
    id: String,
    token: String,
    client: reqwest::Client,
    slot: Arc<Mutex<Option<LiveSlot>>>,
}

impl FreebuffProvider {
    fn new(id: String, token: String) -> Self {
        Self {
            id,
            token,
            client: reqwest::Client::new(),
            slot: Arc::new(Mutex::new(None)),
        }
    }

    async fn request_json(
        &self,
        method: reqwest::Method,
        url: &str,
        body: Option<&Value>,
        extra_headers: &[(&str, &str)],
    ) -> Result<(reqwest::StatusCode, Value, String), ProviderError> {
        let mut request = self
            .client
            .request(method, url)
            .bearer_auth(&self.token)
            .header("Accept", "application/json")
            .header("User-Agent", CLI_USER_AGENT);
        for (name, value) in extra_headers {
            request = request.header(*name, *value);
        }
        if let Some(body) = body {
            request = request.json(body);
        }
        let response = request
            .send()
            .await
            .map_err(|error| ProviderError::Http(error.to_string()))?;
        let status = response.status();
        let text = response
            .text()
            .await
            .map_err(|error| ProviderError::Http(error.to_string()))?;
        let value = if text.trim().is_empty() {
            Value::Null
        } else {
            serde_json::from_str(&text).unwrap_or(Value::Null)
        };
        Ok((status, value, text))
    }

    async fn end_slot(&self, slot: &LiveSlot) {
        let _ = self
            .client
            .delete(format!("{FREEBUFF_API_URL}{SESSION_PATH}"))
            .bearer_auth(&self.token)
            .header("User-Agent", CLI_USER_AGENT)
            .header(INSTANCE_HEADER, &slot.instance_id)
            .send()
            .await;
        let _ = self
            .client
            .post(format!("{FREEBUFF_API_URL}/api/v1/agent-runs"))
            .bearer_auth(&self.token)
            .header("User-Agent", CLI_USER_AGENT)
            .json(&json!({
                "action": "FINISH",
                "runId": slot.run_id,
                "status": "cancelled",
            }))
            .send()
            .await;
    }

    async fn release_current_slot(&self) {
        let slot = self.slot.lock().await.take();
        if let Some(slot) = slot {
            self.end_slot(&slot).await;
        }
    }

    async fn admit(&self, model: &str) -> Result<LiveSlot, ProviderError> {
        {
            let mut guard = self.slot.lock().await;
            if let Some(slot) = guard.as_ref()
                && slot.model == model
            {
                return Ok(LiveSlot {
                    model: slot.model.clone(),
                    instance_id: slot.instance_id.clone(),
                    run_id: slot.run_id.clone(),
                });
            }
            if let Some(old) = guard.take() {
                drop(guard);
                self.end_slot(&old).await;
            }
        }

        let existing = self.fetch_remote_session().await?;
        match existing.as_ref() {
            Some(remote) if remote.model == model => {
                return self.attach_run(remote.clone()).await;
            }
            Some(remote) => {
                self.end_slot(remote).await;
            }
            None => {}
        }
        self.create_session(model).await
    }

    async fn fetch_remote_session(&self) -> Result<Option<LiveSlot>, ProviderError> {
        let (status, value, text) = self
            .request_json(
                reqwest::Method::GET,
                &format!("{FREEBUFF_API_URL}{SESSION_PATH}"),
                None,
                &[],
            )
            .await?;
        if status == reqwest::StatusCode::UNAUTHORIZED {
            return Err(ProviderError::Auth("Freebuff session unauthorized".into()));
        }
        if status == reqwest::StatusCode::NOT_FOUND {
            return Ok(None);
        }
        if !status.is_success() {
            return Err(ProviderError::Api {
                status: status.as_u16(),
                body: text,
            });
        }
        Ok(active_slot_from_session(&value))
    }

    async fn create_session(&self, model: &str) -> Result<LiveSlot, ProviderError> {
        let (status, value, text) = self
            .request_json(
                reqwest::Method::POST,
                &format!("{FREEBUFF_API_URL}{SESSION_PATH}"),
                None,
                &[(MODEL_HEADER, model)],
            )
            .await?;
        if status == reqwest::StatusCode::UNAUTHORIZED {
            return Err(ProviderError::Auth("Freebuff session unauthorized".into()));
        }
        let session_status = value.get("status").and_then(Value::as_str).unwrap_or("");
        if session_status == "model_locked" {
            if let Some(current) = active_slot_from_session(&value) {
                self.end_slot(&current).await;
            } else {
                let _ = self
                    .client
                    .delete(format!("{FREEBUFF_API_URL}{SESSION_PATH}"))
                    .bearer_auth(&self.token)
                    .header("User-Agent", CLI_USER_AGENT)
                    .send()
                    .await;
            }
            let (status, value, text) = self
                .request_json(
                    reqwest::Method::POST,
                    &format!("{FREEBUFF_API_URL}{SESSION_PATH}"),
                    None,
                    &[(MODEL_HEADER, model)],
                )
                .await?;
            return self
                .attach_run_from_session(model, status, value, text)
                .await;
        }
        self.attach_run_from_session(model, status, value, text)
            .await
    }

    async fn attach_run(&self, mut slot: LiveSlot) -> Result<LiveSlot, ProviderError> {
        if slot.run_id.is_empty() {
            slot.run_id = self.start_run(&slot.model, Some(&slot.instance_id)).await?;
        }
        *self.slot.lock().await = Some(slot.clone());
        Ok(slot)
    }

    async fn attach_run_from_session(
        &self,
        model: &str,
        status: reqwest::StatusCode,
        value: Value,
        text: String,
    ) -> Result<LiveSlot, ProviderError> {
        let Some(slot) = active_slot_from_session(&value).filter(|slot| slot.model == model) else {
            return Err(ProviderError::Api {
                status: status.as_u16(),
                body: text,
            });
        };
        if !status.is_success() {
            return Err(ProviderError::Api {
                status: status.as_u16(),
                body: text,
            });
        }
        self.attach_run(slot).await
    }

    async fn start_run(
        &self,
        model: &str,
        instance_id: Option<&str>,
    ) -> Result<String, ProviderError> {
        let (run_status, run_value, run_text) = self
            .request_json(
                reqwest::Method::POST,
                &format!("{FREEBUFF_API_URL}/api/v1/agent-runs"),
                Some(&json!({
                    "action": "START",
                    "agentId": agent_id_for_model(model),
                })),
                &[],
            )
            .await?;
        if !run_status.is_success() {
            if let Some(instance_id) = instance_id {
                let _ = self
                    .client
                    .delete(format!("{FREEBUFF_API_URL}{SESSION_PATH}"))
                    .bearer_auth(&self.token)
                    .header("User-Agent", CLI_USER_AGENT)
                    .header(INSTANCE_HEADER, instance_id)
                    .send()
                    .await;
            }
            return Err(ProviderError::Api {
                status: run_status.as_u16(),
                body: run_text,
            });
        }
        run_value
            .get("runId")
            .and_then(Value::as_str)
            .map(str::to_string)
            .ok_or_else(|| ProviderError::Decode("Freebuff agent-run missing runId".into()))
    }

    fn completion_body(
        &self,
        request: &ProviderRequest,
        slot: &LiveSlot,
    ) -> Result<Value, ProviderError> {
        let mut messages: Vec<Value> = Vec::new();
        let has_buffy = request.messages.iter().any(|message| {
            message.role == MessageRole::System
                && message.content.iter().any(|part| match part {
                    MessagePart::Text(text) => text.trim_start().starts_with("You are Buffy,"),
                    _ => false,
                })
        });
        if !has_buffy {
            append_openai_messages(
                &Message::text(MessageRole::System, BUFFY_SYSTEM),
                &mut messages,
            )?;
        }
        for message in &request.messages {
            append_openai_messages(message, &mut messages)?;
        }
        let mut body = json!({
            "model": request.model,
            "messages": messages,
            "stream": true,
            "codebuff_metadata": {
                "run_id": slot.run_id,
                "client_id": request.session_id.as_deref().unwrap_or("firmius"),
                "cost_mode": "free",
                "freebuff_instance_id": slot.instance_id,
            }
        });
        body["provider"] = openrouter_provider(&request.model);
        body["codebuff"] = json!({
            "codebuff_metadata": body["codebuff_metadata"].clone(),
            "provider": body["provider"].clone(),
        });
        if !request.tools.is_empty() {
            body["tools"] = request
                .tools
                .iter()
                .map(|tool| {
                    json!({
                        "type": "function",
                        "function": {
                            "name": tool.name,
                            "description": tool.description,
                            "parameters": tool.input_schema,
                        }
                    })
                })
                .collect();
        }
        if let Some(effort) = &request.reasoning_effort {
            body["reasoning_effort"] = json!(effort);
        }
        if let Some(session_id) = &request.session_id {
            body["prompt_cache_key"] = json!(session_id);
        }
        Ok(body)
    }

    async fn post_completions(
        &self,
        slot: &LiveSlot,
        body: &Value,
    ) -> Result<reqwest::Response, ProviderError> {
        let send = || {
            self.client
                .post(format!("{FREEBUFF_COMPLETIONS_BASE}/chat/completions"))
                .bearer_auth(&self.token)
                .header("User-Agent", CLI_USER_AGENT)
                .header("Accept", "text/event-stream")
                .header(INSTANCE_HEADER, &slot.instance_id)
                .header(MODEL_HEADER, &slot.model)
                .json(body)
        };
        let response = send()
            .send()
            .await
            .map_err(|error| ProviderError::Http(error.to_string()))?;
        if response.status().as_u16() != 404 {
            return Ok(response);
        }
        let _ = response.text().await;
        tokio::time::sleep(Duration::from_millis(500)).await;
        send()
            .send()
            .await
            .map_err(|error| ProviderError::Http(error.to_string()))
    }
}

impl Drop for FreebuffProvider {
    fn drop(&mut self) {
        if Arc::strong_count(&self.slot) > 1 {
            return;
        }
        let token = self.token.clone();
        let slot = self.slot.try_lock().ok().and_then(|mut guard| guard.take());
        if let Some(slot) = slot {
            tokio::spawn(async move {
                let client = reqwest::Client::new();
                let _ = client
                    .delete(format!("{FREEBUFF_API_URL}{SESSION_PATH}"))
                    .bearer_auth(&token)
                    .header("User-Agent", CLI_USER_AGENT)
                    .header(INSTANCE_HEADER, &slot.instance_id)
                    .send()
                    .await;
            });
        }
    }
}

#[async_trait]
impl Provider for FreebuffProvider {
    fn id(&self) -> &str {
        &self.id
    }

    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError> {
        let slot = self.admit(&request.model).await?;
        let body = self.completion_body(&request, &slot)?;
        dump_provider_request(&self.id, &body);
        let response = self.post_completions(&slot, &body).await?;
        let status = response.status();
        if status.as_u16() == 428
            || status.as_u16() == 410
            || (status.as_u16() == 409 && response.headers().get("content-type").is_some())
        {
            let text = response.text().await.unwrap_or_default();
            self.release_current_slot().await;
            return Err(ProviderError::Api {
                status: status.as_u16(),
                body: text,
            });
        }
        if !status.is_success() {
            let text = response.text().await.unwrap_or_default();
            // Keep the admitted slot on OpenRouter 404s. Releasing here made
            // the next retry 428 waiting_room_required, which then looked like
            // a routing failure.
            return Err(ProviderError::Api {
                status: status.as_u16(),
                body: text,
            });
        }

        let mut byte_stream = response.bytes_stream();
        let stream = async_stream::try_stream! {
            let mut buffer = String::new();
            let mut finish_reason = StopReason::Stop;
            let mut usage = Usage::default();
            while let Some(chunk) = byte_stream.next().await {
                let chunk = chunk.map_err(|e| ProviderError::Http(e.to_string()))?;
                buffer.push_str(&String::from_utf8_lossy(&chunk));
                for payload in parse_sse_lines(&mut buffer) {
                    if payload == "[DONE]" {
                        continue;
                    }
                    let value: Value = serde_json::from_str(&payload)
                        .map_err(|e| ProviderError::Decode(e.to_string()))?;
                    if let Some(u) = value.get("usage") {
                        usage.input_tokens = u.get("prompt_tokens").and_then(Value::as_u64).unwrap_or(0) as u32;
                        usage.output_tokens = u.get("completion_tokens").and_then(Value::as_u64).unwrap_or(0) as u32;
                    }
                    let Some(choice) = value.get("choices").and_then(|c| c.get(0)) else { continue; };
                    if let Some(fr) = choice.get("finish_reason").and_then(Value::as_str) {
                        finish_reason = match fr {
                            "tool_calls" => StopReason::ToolUse,
                            "length" => StopReason::MaxTokens,
                            _ => StopReason::Stop,
                        };
                    }
                    let Some(delta) = choice.get("delta") else { continue; };
                    if let Some(text) = delta.get("content").and_then(Value::as_str)
                        && !text.is_empty()
                    {
                        yield ProviderEvent::TextDelta { delta: text.to_string() };
                    }
                    if let Some(reason) = delta
                        .get("reasoning_content")
                        .or_else(|| delta.get("reasoning"))
                        .and_then(Value::as_str)
                        && !reason.is_empty()
                    {
                        yield ProviderEvent::ThinkingDelta { delta: reason.to_string(), signature: None };
                    }
                }
            }
            yield ProviderEvent::Usage { usage };
            yield ProviderEvent::Done { reason: finish_reason };
        };
        Ok(stream.boxed())
    }
}

fn freebuff_quota_descriptor() -> QuotaDescriptor {
    QuotaDescriptor {
        label: "Freebuff premium sessions".into(),
        auth: QuotaAuth::WebSession,
        meters: vec![
            "premium".into(),
            "deepseek".into(),
            "glm".into(),
            "limited".into(),
            "credits".into(),
        ],
    }
}

struct FreebuffQuotaSource {
    account_id: String,
    token: String,
}

#[async_trait]
impl QuotaSource for FreebuffQuotaSource {
    fn descriptor(&self) -> QuotaDescriptor {
        freebuff_quota_descriptor()
    }

    async fn fetch(&self) -> Result<QuotaSnapshot, QuotaError> {
        let client = reqwest::Client::new();
        let session = fetch_json(
            &client,
            &format!("{FREEBUFF_API_URL}{SESSION_PATH}"),
            &self.token,
            reqwest::Method::GET,
            None,
            &[(INCLUDE_UNUSED_RATE_LIMITS, "1")],
        )
        .await?;
        let mut meters = meters_from_session(&session);
        if let Ok(usage) = fetch_json(
            &client,
            &format!("{FREEBUFF_API_URL}{USAGE_PATH}"),
            &self.token,
            reqwest::Method::POST,
            Some(serde_json::json!({ "fingerprintId": "firmius" })),
            &[],
        )
        .await
            && let Some(meter) = credits_meter(&usage)
        {
            meters.push(meter);
        }
        if meters.is_empty() {
            return Err(QuotaError::Unavailable(
                "Freebuff session response contained no quota meters".into(),
            ));
        }
        let account_id = session
            .get("userId")
            .or_else(|| session.get("user_id"))
            .and_then(Value::as_str)
            .unwrap_or(&self.account_id)
            .to_string();
        let note = session
            .get("accessTier")
            .and_then(Value::as_str)
            .map(|tier| format!("access: {tier}"));
        Ok(QuotaSnapshot {
            account_id,
            observed_at: Utc::now(),
            meters,
            note,
        })
    }
}

async fn fetch_json(
    client: &reqwest::Client,
    url: &str,
    token: &str,
    method: reqwest::Method,
    body: Option<Value>,
    extra_headers: &[(&str, &str)],
) -> Result<Value, QuotaError> {
    let mut request = client
        .request(method, url)
        .bearer_auth(token)
        .header("Accept", "application/json")
        .header("User-Agent", "Firmius");
    for (name, value) in extra_headers {
        request = request.header(*name, *value);
    }
    if let Some(body) = body {
        request = request.json(&body);
    }
    let response = request
        .send()
        .await
        .map_err(|error| QuotaError::Request(error.to_string()))?;
    let status = response.status();
    let text = response
        .text()
        .await
        .map_err(|error| QuotaError::Request(error.to_string()))?;
    if status == reqwest::StatusCode::UNAUTHORIZED || status == reqwest::StatusCode::FORBIDDEN {
        return Err(QuotaError::InvalidCredentials);
    }
    // 404 on /session means "no live row"; the body may still carry pools.
    if !status.is_success() && status != reqwest::StatusCode::NOT_FOUND {
        return Err(QuotaError::Api(format!("HTTP {status}: {text}")));
    }
    if text.trim().is_empty() {
        return Ok(Value::Object(Default::default()));
    }
    serde_json::from_str(&text).map_err(|error| QuotaError::Decode(error.to_string()))
}

fn meters_from_session(session: &Value) -> Vec<QuotaMeter> {
    let Some(limits) = session
        .get("rateLimitsByModel")
        .and_then(Value::as_object)
        .cloned()
        .or_else(|| {
            session.get("rateLimit").cloned().map(|limit| {
                let key = limit
                    .get("model")
                    .and_then(Value::as_str)
                    .unwrap_or("premium")
                    .to_string();
                let mut map = serde_json::Map::new();
                map.insert(key, limit);
                map
            })
        })
    else {
        return Vec::new();
    };
    meters_from_rate_limits(&limits)
}

fn meters_from_rate_limits(limits: &serde_json::Map<String, Value>) -> Vec<QuotaMeter> {
    let mut by_pool: HashMap<String, QuotaMeter> = HashMap::new();
    for (model_id, limit) in limits {
        let Some(meter) = meter_from_rate_limit(model_id, limit) else {
            continue;
        };
        by_pool.entry(meter.id.clone()).or_insert(meter);
    }
    let mut meters: Vec<_> = by_pool.into_values().collect();
    meters.sort_by(|a, b| a.id.cmp(&b.id));
    meters
}

fn meter_from_rate_limit(model_id: &str, limit: &Value) -> Option<QuotaMeter> {
    let used = json_u64(limit.get("recentCount"))?;
    let cap = json_u64(limit.get("limit"))?;
    let pool = limit
        .get("pool")
        .and_then(Value::as_str)
        .filter(|pool| !pool.is_empty())
        .unwrap_or_else(|| infer_pool_id(model_id));
    let label = limit
        .get("poolLabel")
        .and_then(Value::as_str)
        .filter(|label| !label.is_empty())
        .map(str::to_owned)
        .unwrap_or_else(|| pool_label(pool));
    let reset_at = limit
        .get("resetAt")
        .and_then(Value::as_str)
        .and_then(parse_iso_time);
    let remaining = cap.saturating_sub(used);
    Some(QuotaMeter {
        id: pool.to_string(),
        label,
        window: limit
            .get("period")
            .and_then(Value::as_str)
            .map(str::to_owned),
        used: Some(used),
        limit: Some(cap),
        remaining: Some(remaining),
        utilization_percent: if cap == 0 {
            None
        } else {
            Some((used as f64 / cap as f64 * 100.0).min(100.0))
        },
        unit: Some("sessions".into()),
        reset_at,
        reset_in_seconds: reset_at.and_then(|at| {
            let delta = at.signed_duration_since(Utc::now()).num_seconds();
            (delta > 0).then_some(delta as u64)
        }),
    })
}

fn infer_pool_id(model_id: &str) -> &'static str {
    if model_id.contains("deepseek") {
        "deepseek"
    } else if model_id.contains("glm") || model_id.contains("z-ai") {
        "glm"
    } else if model_id.contains("mimo") {
        "limited"
    } else {
        "premium"
    }
}

fn pool_label(pool: &str) -> String {
    match pool {
        "premium" => "Premium sessions".into(),
        "deepseek" => "DeepSeek sessions".into(),
        "glm" => "GLM sessions".into(),
        "limited" => "Limited sessions".into(),
        other => format!("{other} sessions"),
    }
}

fn active_slot_from_session(value: &Value) -> Option<LiveSlot> {
    if value.get("status").and_then(Value::as_str) != Some("active") {
        return None;
    }
    Some(LiveSlot {
        model: value.get("model").and_then(Value::as_str)?.to_string(),
        instance_id: value.get("instanceId").and_then(Value::as_str)?.to_string(),
        run_id: String::new(),
    })
}

fn credits_meter(usage: &Value) -> Option<QuotaMeter> {
    let remaining = json_u64(
        usage
            .get("remainingBalance")
            .or_else(|| usage.get("remaining_balance")),
    )?;
    let used = json_u64(usage.get("usage"));
    let reset_at = usage
        .get("next_quota_reset")
        .and_then(Value::as_str)
        .and_then(parse_iso_time);
    Some(QuotaMeter {
        id: "credits".into(),
        label: "Credits".into(),
        window: None,
        used,
        limit: used.map(|used| used.saturating_add(remaining)),
        remaining: Some(remaining),
        utilization_percent: None,
        unit: Some("credits".into()),
        reset_at,
        reset_in_seconds: None,
    })
}

fn json_u64(value: Option<&Value>) -> Option<u64> {
    value.and_then(|value| {
        value
            .as_u64()
            .or_else(|| value.as_i64().filter(|n| *n >= 0).map(|n| n as u64))
            .or_else(|| value.as_f64().filter(|n| *n >= 0.0).map(|n| n as u64))
            .or_else(|| value.as_str()?.trim().parse().ok())
    })
}

fn parse_iso_time(value: &str) -> Option<DateTime<Utc>> {
    DateTime::parse_from_rfc3339(value)
        .ok()
        .map(|time| time.with_timezone(&Utc))
}

#[derive(Debug, Deserialize)]
struct LoginCodeResponse {
    #[serde(rename = "loginUrl")]
    login_url: String,
    #[serde(rename = "fingerprintHash")]
    fingerprint_hash: String,
    #[serde(rename = "expiresAt", deserialize_with = "deserialize_expires_at")]
    expires_at: String,
}

fn deserialize_expires_at<'de, D>(deserializer: D) -> Result<String, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let value = Value::deserialize(deserializer)?;
    match value {
        Value::String(s) if !s.trim().is_empty() => Ok(s),
        Value::Number(n) => Ok(n.to_string()),
        other => Err(serde::de::Error::custom(format!(
            "expiresAt must be a string or number, got {other}"
        ))),
    }
}

struct LoginState {
    fingerprint_id: String,
    fingerprint_hash: String,
    expires_at: String,
    next_poll_at: Instant,
    deadline: Instant,
}

#[derive(Default)]
pub struct FreebuffWizard {
    state: Option<LoginState>,
}

impl FreebuffWizard {
    async fn request_login_url(&self, fingerprint_id: &str) -> Result<LoginCodeResponse, String> {
        let response = reqwest::Client::new()
            .post(format!("{FREEBUFF_LOGIN_URL}{LOGIN_CODE_PATH}"))
            .header("Accept", "application/json")
            .header("User-Agent", "Firmius")
            .json(&serde_json::json!({ "fingerprintId": fingerprint_id }))
            .send()
            .await
            .map_err(|error| error.to_string())?;
        if !response.status().is_success() {
            return Err(format!(
                "Freebuff login code request failed: HTTP {}",
                response.status()
            ));
        }
        response.json().await.map_err(|error| error.to_string())
    }
}

#[async_trait]
impl SetupWizard for FreebuffWizard {
    async fn start(&mut self) -> Step {
        let fingerprint_id = fingerprint_id();
        match self.request_login_url(&fingerprint_id).await {
            Ok(code) => {
                let now = Instant::now();
                self.state = Some(LoginState {
                    fingerprint_id,
                    fingerprint_hash: code.fingerprint_hash,
                    expires_at: code.expires_at.clone(),
                    next_poll_at: now + Duration::from_secs(3),
                    deadline: now + Duration::from_secs(10 * 60),
                });
                Step::OpenUrl {
                    label: "Freebuff login started — approve in the browser".into(),
                    url: code.login_url,
                }
            }
            Err(error) => Step::Prompt {
                label: format!("Freebuff login unavailable: {error}"),
                secret: false,
            },
        }
    }

    async fn answer(&mut self, input: String) -> Result<Outcome, WizardError> {
        if self.state.is_some() {
            return Err(WizardError::InvalidAnswer(
                "Freebuff login completes in the browser".into(),
            ));
        }
        let token = input.trim();
        if token.is_empty() {
            return Err(WizardError::InvalidAnswer(
                "auth token must not be empty".into(),
            ));
        }
        Ok(Outcome::Done {
            schema: schema_template("freebuff"),
            credentials: serde_json::json!({
                "auth_token": token,
            }),
        })
    }

    async fn poll(&mut self) -> Result<Option<Outcome>, WizardError> {
        let Some(state) = self.state.as_ref() else {
            return Ok(None);
        };
        if Instant::now() >= state.deadline {
            return Err(WizardError::InvalidAnswer(
                "Freebuff login timed out".into(),
            ));
        }
        if Instant::now() < state.next_poll_at {
            return Ok(None);
        }
        let url = reqwest::Url::parse_with_params(
            &format!("{FREEBUFF_LOGIN_URL}{LOGIN_STATUS_PATH}"),
            [
                ("fingerprintId", state.fingerprint_id.as_str()),
                ("fingerprintHash", state.fingerprint_hash.as_str()),
                ("expiresAt", state.expires_at.as_str()),
            ],
        )
        .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;
        let response = reqwest::Client::new()
            .get(url)
            .header("Accept", "application/json")
            .header("User-Agent", "Firmius")
            .send()
            .await
            .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;
        let status = response.status();
        let body = response
            .text()
            .await
            .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;

        if let Some(state) = self.state.as_mut() {
            state.next_poll_at = Instant::now() + Duration::from_secs(4);
        }

        if status == reqwest::StatusCode::UNAUTHORIZED {
            return Ok(None);
        }
        if !status.is_success() {
            return Ok(None);
        }
        let payload: Value = serde_json::from_str(&body)
            .map_err(|error| WizardError::InvalidAnswer(error.to_string()))?;
        let Some(user) = payload.get("user").filter(|value| value.is_object()) else {
            return Ok(None);
        };
        let token = user
            .get("authToken")
            .or_else(|| user.get("auth_token"))
            .and_then(Value::as_str)
            .map(str::trim)
            .filter(|token| !token.is_empty())
            .ok_or_else(|| {
                WizardError::InvalidAnswer("Freebuff login did not return an auth token".into())
            })?;
        let user_id = user
            .get("id")
            .and_then(Value::as_str)
            .filter(|id| !id.is_empty())
            .map(str::to_owned)
            .unwrap_or_else(|| Uuid::new_v4().to_string());
        let fingerprint_id = self
            .state
            .as_ref()
            .map(|state| state.fingerprint_id.clone())
            .unwrap_or_else(fingerprint_id);
        Ok(Some(Outcome::Done {
            schema: schema_template(&format!("freebuff-{user_id}")),
            credentials: serde_json::json!({
                "auth_token": token,
                "user_id": user_id,
                "email": user.get("email").and_then(Value::as_str),
                "name": user.get("name").and_then(Value::as_str),
                "fingerprint_id": fingerprint_id,
            }),
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_has_freebuff_models_and_endpoint() {
        let schema = schema_template("freebuff-user");
        assert_eq!(schema.base_url.as_deref(), Some(FREEBUFF_COMPLETIONS_BASE));
        assert_eq!(schema.api_type, ApiType::OpenAI);
        assert!(
            schema
                .models
                .iter()
                .any(|model| model.id == "openai/gpt-5.6-luna")
        );
        let luna = schema.model("openai/gpt-5.6-luna").unwrap();
        assert!(luna.effort_mode("high").is_some());
        assert!(luna.capabilities.supports(ModelCapability::ToolUse));
        assert!(luna.capabilities.supports(ModelCapability::Reasoning));
        assert!(luna.capabilities.supports(ModelCapability::Image));
        let flash = schema.model("deepseek/deepseek-v4-flash").unwrap();
        assert!(flash.effort_mode("max").is_some());
        assert!(flash.effort_mode("medium").is_none());
    }

    #[test]
    fn quota_capability_uses_auth_token() {
        let capability = FreebuffKind
            .quota_capability(
                &schema_template("freebuff-user"),
                &serde_json::json!({ "auth_token": "sess-token" }),
            )
            .unwrap()
            .unwrap();
        assert_eq!(capability.descriptor.auth, QuotaAuth::WebSession);
        assert!(capability.descriptor.meters.contains(&"premium".into()));
        assert!(capability.source.is_some());
    }

    #[test]
    fn quota_parser_dedupes_premium_pool_and_keeps_deepseek() {
        let session = serde_json::json!({
            "status": "none",
            "accessTier": "full",
            "rateLimitsByModel": {
                "openai/gpt-5.6-luna": {
                    "model": "openai/gpt-5.6-luna",
                    "pool": "premium",
                    "poolLabel": "Premium",
                    "limit": 4,
                    "recentCount": 2,
                    "period": "pacific_day",
                    "resetAt": "2099-01-02T08:00:00Z"
                },
                "deepseek/deepseek-v4-flash": {
                    "model": "deepseek/deepseek-v4-flash",
                    "pool": "deepseek",
                    "poolLabel": "DeepSeek",
                    "limit": 1,
                    "recentCount": 1,
                    "period": "pacific_day",
                    "resetAt": "2099-01-02T08:00:00Z"
                },
                "minimax/minimax-m3": {
                    "model": "minimax/minimax-m3",
                    "pool": "premium",
                    "poolLabel": "Premium",
                    "limit": 4,
                    "recentCount": 2,
                    "period": "pacific_day",
                    "resetAt": "2099-01-02T08:00:00Z"
                }
            }
        });
        let meters = meters_from_session(&session);
        assert_eq!(
            meters
                .iter()
                .map(|meter| meter.id.as_str())
                .collect::<Vec<_>>(),
            ["deepseek", "premium"]
        );
        let premium = meters.iter().find(|meter| meter.id == "premium").unwrap();
        assert_eq!(premium.used, Some(2));
        assert_eq!(premium.limit, Some(4));
        assert_eq!(premium.remaining, Some(2));
        assert_eq!(premium.unit.as_deref(), Some("sessions"));
        let deepseek = meters.iter().find(|meter| meter.id == "deepseek").unwrap();
        assert_eq!(deepseek.used, Some(1));
        assert_eq!(deepseek.limit, Some(1));
        assert!(premium.reset_at.is_some());
    }

    #[test]
    fn credits_meter_maps_remaining_balance() {
        let meter = credits_meter(&serde_json::json!({
            "usage": 12,
            "remainingBalance": 88,
            "next_quota_reset": "2099-02-01T00:00:00Z"
        }))
        .unwrap();
        assert_eq!(meter.id, "credits");
        assert_eq!(meter.used, Some(12));
        assert_eq!(meter.remaining, Some(88));
        assert_eq!(meter.limit, Some(100));
    }

    #[tokio::test]
    async fn wizard_rejects_empty_pasted_token() {
        let mut wizard = FreebuffWizard::default();
        let err = wizard.answer("  ".into()).await.unwrap_err();
        assert!(err.to_string().contains("must not be empty"));
    }

    #[test]
    fn login_code_accepts_numeric_expires_at() {
        let code: LoginCodeResponse = serde_json::from_value(serde_json::json!({
            "loginUrl": "https://freebuff.com/login?auth_code=abc",
            "fingerprintHash": "hash",
            "expiresAt": 1_787_208_444_985_u64,
            "expiresInMs": 3_600_000
        }))
        .unwrap();
        assert_eq!(code.login_url, "https://freebuff.com/login?auth_code=abc");
        assert_eq!(code.expires_at, "1787208444985");
    }

    #[test]
    fn active_slot_parser_reads_live_row_and_ignores_none() {
        let active = active_slot_from_session(&serde_json::json!({
            "status": "active",
            "model": "openai/gpt-5.6-luna",
            "instanceId": "inst-9"
        }))
        .unwrap();
        assert_eq!(active.model, "openai/gpt-5.6-luna");
        assert_eq!(active.instance_id, "inst-9");
        assert!(active.run_id.is_empty());
        assert!(active_slot_from_session(&serde_json::json!({"status":"none"})).is_none());
    }

    #[test]
    fn completion_body_injects_buffy_and_metadata() {
        let provider = FreebuffProvider::new("freebuff".into(), "token".into());
        let slot = LiveSlot {
            model: "openai/gpt-5.6-luna".into(),
            instance_id: "inst-1".into(),
            run_id: "run-1".into(),
        };
        let body = provider
            .completion_body(
                &ProviderRequest {
                    model: "openai/gpt-5.6-luna".into(),
                    messages: vec![Message::text(MessageRole::User, "hi")],
                    tools: Vec::new(),
                    temperature: None,
                    max_tokens: None,
                    reasoning_effort: Some("high".into()),
                    thinking_budget_tokens: None,
                    session_id: Some("sess-1".into()),
                    web_search: None,
                },
                &slot,
            )
            .unwrap();
        assert_eq!(body["messages"][0]["role"], "system");
        assert!(
            body["messages"][0]["content"]
                .as_str()
                .unwrap()
                .starts_with("You are Buffy,")
        );
        assert_eq!(body["codebuff_metadata"]["run_id"], "run-1");
        assert_eq!(body["codebuff_metadata"]["cost_mode"], "free");
        assert_eq!(body["codebuff_metadata"]["freebuff_instance_id"], "inst-1");
        assert_eq!(body["prompt_cache_key"], "sess-1");
        assert_eq!(
            agent_id_for_model("deepseek/deepseek-v4-pro"),
            "base3-free-deepseek"
        );
        assert_eq!(body["provider"]["order"][0], "openai");
        assert_eq!(body["provider"]["allow_fallbacks"], true);
        assert_eq!(body["codebuff"]["provider"]["order"][0], "openai");
    }
}
