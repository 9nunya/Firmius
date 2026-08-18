use super::{
    Provider, ProviderError, ProviderEvent, StaticToken, TokenSupplier, dump_provider_request,
    parse_sse_lines,
};
use crate::types::{ImageSource, MessagePart, MessageRole, ProviderRequest, StopReason, Usage};
use async_trait::async_trait;
use futures::{StreamExt, stream::BoxStream};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::sync::Arc;

const CLAUDE_CODE_VERSION: &str = "2.1.206";
const BILLING_HEADER_SALT: &str = "59cf53e54c78";
const BILLING_HEADER_POSITIONS: [usize; 3] = [4, 7, 20];
const CLAUDE_CODE_ENTRYPOINT: &str = "sdk-cli";
const CLAUDE_CODE_IDENTITY: &str = "You are Claude Code, Anthropic's official CLI for Claude.";
const CLAUDE_CODE_TOOL_NAMES: &[&str] = &[
    "Read",
    "Write",
    "Edit",
    "Bash",
    "Grep",
    "Glob",
    "AskUserQuestion",
    "EnterPlanMode",
    "ExitPlanMode",
    "KillShell",
    "NotebookEdit",
    "Skill",
    "Task",
    "TaskOutput",
    "TodoWrite",
    "WebFetch",
    "WebSearch",
];

/// Anthropic Messages API backend.
pub struct AnthropicProvider {
    id: String,
    base_url: String,
    auth: Arc<dyn TokenSupplier>,
    version: String,
    client: reqwest::Client,
    oauth: bool,
}

impl AnthropicProvider {
    /// Convenience constructor for API-key auth (`x-api-key` header).
    pub fn new(id: impl Into<String>, api_key: impl Into<String>) -> Self {
        Self::with_auth(id, Arc::new(StaticToken::x_api_key(api_key)))
    }

    /// Build with any token supplier — static key today, OAuth refresh later.
    pub fn with_auth(id: impl Into<String>, auth: Arc<dyn TokenSupplier>) -> Self {
        Self {
            id: id.into(),
            base_url: "https://api.anthropic.com".to_string(),
            auth,
            version: "2023-06-01".to_string(),
            client: reqwest::Client::new(),
            oauth: false,
        }
    }

    /// Build with an OAuth bearer supplier and Claude Code-compatible request shaping.
    pub fn with_oauth(id: impl Into<String>, auth: Arc<dyn TokenSupplier>) -> Self {
        Self {
            oauth: true,
            ..Self::with_auth(id, auth)
        }
    }

    pub fn with_base_url(mut self, base_url: impl Into<String>) -> Self {
        self.base_url = base_url.into();
        self
    }

    fn build_body(&self, request: &ProviderRequest) -> Result<Value, ProviderError> {
        // Anthropic wants system prompts as a top-level field, not in messages.
        let mut system_parts: Vec<String> = Vec::new();
        let mut messages: Vec<Value> = Vec::new();
        for message in &request.messages {
            if message.role == MessageRole::System {
                let mut text = String::new();
                for part in &message.content {
                    if let MessagePart::Text(t) = part {
                        text.push_str(t);
                    }
                }
                if !text.is_empty() {
                    system_parts.push(text);
                }
                continue;
            }
            messages.push(message_to_anthropic(message, self.oauth)?);
        }
        let system = system_parts.join("\n\n");

        if self.oauth {
            add_cache_control_to_last_user_content_block(&mut messages);
        }

        let mut body = json!({
            "model": request.model,
            "messages": messages,
            "stream": true,
            "max_tokens": request.max_tokens.unwrap_or(4096),
        });
        if self.oauth {
            body["system"] = json!(oauth_system_blocks(
                &system,
                body["messages"].as_array().unwrap()
            ));
        } else if !system.is_empty() {
            body["system"] = json!(system);
        }
        if !request.tools.is_empty() {
            body["tools"] = request
                .tools
                .iter()
                .map(|t| {
                    let name = if self.oauth {
                        to_claude_code_tool_name(&t.name)
                    } else {
                        t.name.clone()
                    };
                    json!({ "name": name, "description": t.description, "input_schema": t.input_schema })
                })
                .collect();
            if self.oauth {
                add_cache_control_to_last_tool(&mut body["tools"]);
            }
        }
        if let Some(t) = request.temperature.filter(|_| {
            request.thinking_budget_tokens.is_none() && request.reasoning_effort.is_none()
        }) {
            body["temperature"] = json!(t);
        }
        if let Some(budget) = request.thinking_budget_tokens {
            body["thinking"] = json!({
                "type": "enabled",
                "budget_tokens": budget,
            });
        } else if let Some(effort) = &request.reasoning_effort {
            body["thinking"] = json!({
                "type": "adaptive",
                "display": "summarized",
            });
            body["output_config"] = json!({ "effort": effort });
        }
        Ok(body)
    }

    async fn auth_and_compat_headers(
        &self,
        thinking_budget_tokens: Option<u32>,
    ) -> Result<Vec<(String, String)>, ProviderError> {
        let mut headers = self.auth.headers().await?;
        if self.oauth {
            let mut beta =
                "claude-code-20250219,oauth-2025-04-20,fine-grained-tool-streaming-2025-05-14"
                    .to_string();
            if thinking_budget_tokens.is_some() {
                beta.push_str(",interleaved-thinking-2025-05-14");
            }
            headers.extend([
                ("accept".to_string(), "application/json".to_string()),
                (
                    "anthropic-dangerous-direct-browser-access".to_string(),
                    "true".to_string(),
                ),
                ("anthropic-beta".to_string(), beta),
                (
                    "user-agent".to_string(),
                    format!("claude-cli/{CLAUDE_CODE_VERSION}"),
                ),
                ("x-app".to_string(), "cli".to_string()),
            ]);
        }
        Ok(headers)
    }
}

fn oauth_system_blocks(firmius_system: &str, messages: &[Value]) -> Vec<Value> {
    let mut blocks = Vec::new();
    if let Some(billing) = billing_header_text(messages) {
        blocks.push(json!({ "type": "text", "text": billing }));
    }
    blocks.push(json!({
        "type": "text",
        "text": CLAUDE_CODE_IDENTITY,
        "cache_control": { "type": "ephemeral" },
    }));
    if !firmius_system.is_empty() {
        blocks.push(json!({
            "type": "text",
            "text": firmius_system,
            "cache_control": { "type": "ephemeral" },
        }));
    }
    blocks
}

fn billing_header_text(messages: &[Value]) -> Option<String> {
    let message_text = messages
        .iter()
        .find(|m| m.get("role").and_then(Value::as_str) == Some("user"))
        .and_then(|m| m.get("content").and_then(Value::as_array))
        .and_then(|blocks| {
            blocks
                .iter()
                .find(|b| b.get("type").and_then(Value::as_str) == Some("text"))
        })
        .and_then(|b| b.get("text").and_then(Value::as_str))
        .unwrap_or("");
    if message_text.is_empty() {
        return None;
    }
    let cch = sha256_hex(message_text).chars().take(5).collect::<String>();
    let utf16: Vec<u16> = message_text.encode_utf16().collect();
    let sampled = BILLING_HEADER_POSITIONS
        .iter()
        .map(|idx| {
            utf16
                .get(*idx)
                .copied()
                .map(|unit| {
                    char::decode_utf16([unit])
                        .next()
                        .expect("one UTF-16 code unit")
                        .unwrap_or(char::REPLACEMENT_CHARACTER)
                })
                .unwrap_or('0')
        })
        .collect::<String>();
    let suffix = sha256_hex(&format!(
        "{BILLING_HEADER_SALT}{sampled}{CLAUDE_CODE_VERSION}"
    ))
    .chars()
    .take(3)
    .collect::<String>();
    Some(format!(
        "x-anthropic-billing-header: cc_version={CLAUDE_CODE_VERSION}.{suffix}; cc_entrypoint={CLAUDE_CODE_ENTRYPOINT}; cch={cch};"
    ))
}

fn sha256_hex(input: &str) -> String {
    let digest = Sha256::digest(input.as_bytes());
    digest.iter().map(|b| format!("{b:02x}")).collect()
}

fn add_cache_control_to_last_user_content_block(messages: &mut [Value]) {
    if let Some(block) = messages
        .iter_mut()
        .rev()
        .find(|m| m.get("role").and_then(Value::as_str) == Some("user"))
        .and_then(|m| m.get_mut("content"))
        .and_then(Value::as_array_mut)
        .and_then(|blocks| blocks.last_mut())
    {
        block["cache_control"] = json!({ "type": "ephemeral" });
    }
}

fn add_cache_control_to_last_tool(tools: &mut Value) {
    if let Some(tool) = tools.as_array_mut().and_then(|tools| tools.last_mut()) {
        tool["cache_control"] = json!({ "type": "ephemeral" });
    }
}

fn to_claude_code_tool_name(name: &str) -> String {
    CLAUDE_CODE_TOOL_NAMES
        .iter()
        .find(|canonical| canonical.eq_ignore_ascii_case(name))
        .copied()
        .unwrap_or(name)
        .to_string()
}

fn from_claude_code_tool_name(name: &str, tools: &[crate::types::ToolDefinition]) -> String {
    tools
        .iter()
        .find(|tool| tool.name.eq_ignore_ascii_case(name))
        .map(|tool| tool.name.clone())
        .unwrap_or_else(|| name.to_string())
}

fn message_to_anthropic(
    message: &crate::types::Message,
    oauth: bool,
) -> Result<Value, ProviderError> {
    let role = match message.role {
        MessageRole::Assistant => "assistant",
        // User and Tool both map to a user-role message; tool results are
        // user-side `tool_result` content blocks in Anthropic's model.
        _ => "user",
    };
    let mut blocks = Vec::new();
    for part in &message.content {
        match part {
            MessagePart::Text(t) => blocks.push(json!({ "type": "text", "text": t })),
            MessagePart::Image(image) => blocks.push(anthropic_image_block(image)?),
            MessagePart::ToolCall { id, name, args } => {
                let input: Value = serde_json::from_str(args).unwrap_or_else(|_| json!({}));
                let name = if oauth {
                    to_claude_code_tool_name(name)
                } else {
                    name.clone()
                };
                blocks.push(json!({ "type": "tool_use", "id": id, "name": name, "input": input }));
            }
            MessagePart::ToolResult { id, content, ok } => blocks.push(json!({
                "type": "tool_result",
                "tool_use_id": id,
                "content": content,
                "is_error": !ok,
            })),
            MessagePart::Thinking { content, signature } => {
                if let Some(sig) = signature.as_ref() {
                    blocks
                        .push(json!({ "type": "thinking", "thinking": content, "signature": sig }));
                }
            }
        }
    }
    Ok(json!({ "role": role, "content": blocks }))
}

fn anthropic_image_block(image: &crate::types::ImagePart) -> Result<Value, ProviderError> {
    match &image.source {
        ImageSource::Base64 { media_type, data } => Ok(json!({
            "type": "image",
            "source": {
                "type": "base64",
                "media_type": media_type,
                "data": data,
            }
        })),
        ImageSource::Url { .. } => Err(ProviderError::Decode(
            "anthropic image inputs must be provided as base64 data".into(),
        )),
    }
}

#[async_trait]
impl Provider for AnthropicProvider {
    fn id(&self) -> &str {
        &self.id
    }

    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError> {
        let body = self.build_body(&request)?;
        let oauth = self.oauth;
        let request_tools = request.tools.clone();
        dump_provider_request(&self.id, &body);
        let mut request_builder = self
            .client
            .post(format!(
                "{}/v1/messages",
                self.base_url.trim_end_matches('/')
            ))
            .header("anthropic-version", &self.version)
            .json(&body);
        for (name, value) in self
            .auth_and_compat_headers(request.thinking_budget_tokens)
            .await?
        {
            request_builder = request_builder.header(name, value);
        }
        let response = request_builder
            .send()
            .await
            .map_err(|e| ProviderError::Http(e.to_string()))?;

        let status = response.status();
        if !status.is_success() {
            let body = response.text().await.unwrap_or_default();
            return Err(ProviderError::Api {
                status: status.as_u16(),
                body,
            });
        }

        let mut byte_stream = response.bytes_stream();

        let stream = async_stream::try_stream! {
            let mut buffer = String::new();
            // Current in-flight content block: (index, kind, id, name, buf).
            let mut block_id = String::new();
            let mut block_name = String::new();
            let mut block_args = String::new();
            let mut block_is_tool = false;
            let mut block_index = 0_u32;
            let mut finish_reason = StopReason::Stop;
            let mut usage = Usage::default();

            while let Some(chunk) = byte_stream.next().await {
                let chunk = chunk.map_err(|e| ProviderError::Http(e.to_string()))?;
                buffer.push_str(&String::from_utf8_lossy(&chunk));

                for payload in parse_sse_lines(&mut buffer) {
                    let value: Value = match serde_json::from_str(&payload) {
                        Ok(v) => v,
                        Err(_) => continue,
                    };
                    let event_type = value.get("type").and_then(Value::as_str).unwrap_or("");

                    match event_type {
                        "message_start" => {
                            if let Some(u) = value.pointer("/message/usage") {
                                merge_anthropic_usage(&mut usage, u);
                            }
                        }
                        "content_block_start" => {
                            block_index = value.get("index").and_then(Value::as_u64).unwrap_or(0) as u32;
                            let block = value.get("content_block");
                            let kind = block.and_then(|b| b.get("type")).and_then(Value::as_str).unwrap_or("");
                            if kind == "tool_use" {
                                block_is_tool = true;
                                block_id = block.and_then(|b| b.get("id")).and_then(Value::as_str).unwrap_or("").to_string();
                                let wire_name = block.and_then(|b| b.get("name")).and_then(Value::as_str).unwrap_or("");
                                block_name = if oauth {
                                    from_claude_code_tool_name(wire_name, &request_tools)
                                } else {
                                    wire_name.to_string()
                                };
                                block_args.clear();
                                yield ProviderEvent::ToolCallDelta {
                                    index: block_index,
                                    id: Some(block_id.clone()),
                                    name_delta: block_name.clone(),
                                    args_delta: String::new(),
                                };
                            } else {
                                block_is_tool = false;
                            }
                        }
                        "content_block_delta" => {
                            let delta = value.get("delta");
                            let dtype = delta.and_then(|d| d.get("type")).and_then(Value::as_str).unwrap_or("");
                            match dtype {
                                "text_delta" => {
                                    if let Some(t) = delta.and_then(|d| d.get("text")).and_then(Value::as_str) {
                                        yield ProviderEvent::TextDelta { delta: t.to_string() };
                                    }
                                }
                                "thinking_delta" => {
                                    if let Some(t) = delta.and_then(|d| d.get("thinking")).and_then(Value::as_str) {
                                        yield ProviderEvent::ThinkingDelta { delta: t.to_string(), signature: None };
                                    }
                                }
                                "signature_delta" => {
                                    if let Some(sig) = delta.and_then(|d| d.get("signature")).and_then(Value::as_str) {
                                        yield ProviderEvent::ThinkingDelta { delta: String::new(), signature: Some(sig.to_string()) };
                                    }
                                }
                                "input_json_delta" => {
                                    if let Some(p) = delta.and_then(|d| d.get("partial_json")).and_then(Value::as_str) {
                                        block_args.push_str(p);
                                        yield ProviderEvent::ToolCallDelta {
                                            index: block_index,
                                            id: None,
                                            name_delta: String::new(),
                                            args_delta: p.to_string(),
                                        };
                                    }
                                }
                                _ => {}
                            }
                        }
                        "content_block_stop" => {
                            if block_is_tool && !block_id.is_empty() {
                                yield ProviderEvent::ToolCall {
                                    id: std::mem::take(&mut block_id),
                                    name: std::mem::take(&mut block_name),
                                    args: std::mem::take(&mut block_args),
                                };
                            }
                            block_is_tool = false;
                        }
                        "message_delta" => {
                            if let Some(sr) = value.get("delta").and_then(|d| d.get("stop_reason")).and_then(Value::as_str) {
                                finish_reason = match sr {
                                    "tool_use" => StopReason::ToolUse,
                                    "max_tokens" => StopReason::MaxTokens,
                                    _ => StopReason::Stop,
                                };
                            }
                            if let Some(u) = value.get("usage") {
                                merge_anthropic_usage(&mut usage, u);
                            }
                        }
                        "message_stop" => {
                            if let Some(u) = value.get("usage") {
                                merge_anthropic_usage(&mut usage, u);
                            }
                        }
                        _ => {}
                    }
                }
            }

            yield ProviderEvent::Usage { usage };
            yield ProviderEvent::Done { reason: finish_reason };
        };

        Ok(stream.boxed())
    }
}

fn merge_anthropic_usage(usage: &mut Usage, value: &Value) {
    if let Some(tokens) = value.get("input_tokens").and_then(Value::as_u64) {
        usage.input_tokens = tokens as u32;
    }
    if let Some(tokens) = value.get("output_tokens").and_then(Value::as_u64) {
        usage.output_tokens = tokens as u32;
    }
    if let Some(tokens) = value.get("cache_read_input_tokens").and_then(Value::as_u64) {
        usage.cache_read_tokens = tokens as u32;
    }
    if let Some(tokens) = value
        .get("cache_creation_input_tokens")
        .and_then(Value::as_u64)
    {
        usage.cache_write_tokens = tokens as u32;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{ImagePart, Message, ToolDefinition};

    fn base_request() -> ProviderRequest {
        ProviderRequest {
            model: "claude-sonnet-4-20250514".to_string(),
            messages: vec![
                Message::text(MessageRole::System, "Firmius system"),
                Message::text(MessageRole::User, "abcdefghijklmnopqrstuvwxyz"),
            ],
            tools: vec![],
            temperature: Some(0.5),
            max_tokens: Some(128),
            reasoning_effort: None,
            thinking_budget_tokens: None,
        }
    }

    fn oauth_provider() -> AnthropicProvider {
        AnthropicProvider::with_oauth("anthropic-oauth", Arc::new(StaticToken::bearer("oat")))
    }

    #[tokio::test]
    async fn oauth_headers_shape_claude_code_request() {
        let headers = oauth_provider()
            .auth_and_compat_headers(Some(1024))
            .await
            .unwrap();
        assert!(headers.contains(&("Authorization".to_string(), "Bearer oat".to_string())));
        assert!(headers.contains(&("accept".to_string(), "application/json".to_string())));
        assert!(headers.contains(&(
            "anthropic-dangerous-direct-browser-access".to_string(),
            "true".to_string()
        )));
        assert!(headers.contains(&("user-agent".to_string(), "claude-cli/2.1.206".to_string())));
        assert!(headers.contains(&("x-app".to_string(), "cli".to_string())));
        let beta = headers
            .iter()
            .find(|(name, _)| name == "anthropic-beta")
            .unwrap()
            .1
            .as_str();
        assert!(beta.contains("claude-code-20250219"));
        assert!(beta.contains("oauth-2025-04-20"));
        assert!(beta.contains("fine-grained-tool-streaming-2025-05-14"));
        assert!(beta.contains("interleaved-thinking-2025-05-14"));
    }

    #[test]
    fn oauth_body_prepends_deterministic_billing_identity_and_firmius_system() {
        let body = oauth_provider().build_body(&base_request()).unwrap();
        let system = body["system"].as_array().unwrap();
        assert_eq!(system.len(), 3);
        assert_eq!(
            system[0]["text"],
            "x-anthropic-billing-header: cc_version=2.1.206.d50; cc_entrypoint=sdk-cli; cch=71c48;"
        );
        assert_eq!(system[1]["text"], CLAUDE_CODE_IDENTITY);
        assert_eq!(system[2]["text"], "Firmius system");
    }

    #[test]
    fn oauth_body_omits_billing_header_without_user_text() {
        let mut request = base_request();
        request
            .messages
            .retain(|message| message.role == MessageRole::System);
        let body = oauth_provider().build_body(&request).unwrap();
        let system = body["system"].as_array().unwrap();
        assert_eq!(system.len(), 2);
        assert_eq!(system[0]["text"], CLAUDE_CODE_IDENTITY);
        assert_eq!(system[1]["text"], "Firmius system");
        assert!(system.iter().all(|block| {
            !block["text"]
                .as_str()
                .unwrap_or_default()
                .contains("x-anthropic-billing-header:")
        }));
    }

    #[test]
    fn oauth_body_sets_ephemeral_cache_breakpoints() {
        let mut request = base_request();
        request.tools = vec![
            ToolDefinition {
                name: "first".to_string(),
                description: "first tool".to_string(),
                input_schema: json!({ "type": "object" }),
            },
            ToolDefinition {
                name: "last".to_string(),
                description: "last tool".to_string(),
                input_schema: json!({ "type": "object" }),
            },
        ];
        let body = oauth_provider().build_body(&request).unwrap();
        let system = body["system"].as_array().unwrap();
        assert!(system[0].get("cache_control").is_none());
        for block in system.iter().skip(1) {
            assert_eq!(block["cache_control"], json!({ "type": "ephemeral" }));
        }
        assert!(body["tools"][0].get("cache_control").is_none());
        assert_eq!(
            body["tools"][1]["cache_control"],
            json!({ "type": "ephemeral" })
        );
        assert_eq!(
            body["messages"][0]["content"][0]["cache_control"],
            json!({ "type": "ephemeral" })
        );
    }

    #[test]
    fn oauth_body_uses_claude_code_tool_casing_and_restores_registry_names() {
        let mut request = base_request();
        request.tools = vec![
            ToolDefinition {
                name: "bash".into(),
                description: "run a command".into(),
                input_schema: json!({ "type": "object" }),
            },
            ToolDefinition {
                name: "read".into(),
                description: "read a file".into(),
                input_schema: json!({ "type": "object" }),
            },
        ];
        request.messages.push(Message {
            role: MessageRole::Assistant,
            content: vec![MessagePart::ToolCall {
                id: "call-1".into(),
                name: "bash".into(),
                args: "{}".into(),
            }],
        });

        let body = oauth_provider().build_body(&request).unwrap();
        assert_eq!(body["tools"][0]["name"], "Bash");
        assert_eq!(body["tools"][1]["name"], "Read");
        assert_eq!(body["messages"][1]["content"][0]["name"], "Bash");
        assert_eq!(from_claude_code_tool_name("Bash", &request.tools), "bash");
        assert_eq!(from_claude_code_tool_name("Read", &request.tools), "read");
    }

    #[test]
    fn usage_merge_preserves_message_start_input_when_delta_only_has_output() {
        let mut usage = Usage::default();
        merge_anthropic_usage(
            &mut usage,
            &json!({
                "input_tokens": 100,
                "output_tokens": 1,
                "cache_read_input_tokens": 80,
                "cache_creation_input_tokens": 20,
            }),
        );
        merge_anthropic_usage(&mut usage, &json!({ "output_tokens": 12 }));

        assert_eq!(
            usage,
            Usage {
                input_tokens: 100,
                output_tokens: 12,
                cache_read_tokens: 80,
                cache_write_tokens: 20,
            }
        );
    }

    #[test]
    fn adaptive_effort_uses_adaptive_thinking_and_output_config() {
        let mut request = base_request();
        request.reasoning_effort = Some("high".to_string());
        let body = oauth_provider().build_body(&request).unwrap();
        assert_eq!(
            body["thinking"],
            json!({ "type": "adaptive", "display": "summarized" })
        );
        assert_eq!(body["output_config"], json!({ "effort": "high" }));
        assert!(body.get("temperature").is_none());
    }

    #[test]
    fn budget_thinking_keeps_enabled_shape_and_omits_temperature() {
        let mut request = base_request();
        request.thinking_budget_tokens = Some(2048);
        let body = oauth_provider().build_body(&request).unwrap();
        assert_eq!(
            body["thinking"],
            json!({ "type": "enabled", "budget_tokens": 2048 })
        );
        assert!(body.get("temperature").is_none());
        assert!(body.get("output_config").is_none());
    }

    #[tokio::test]
    async fn api_key_auth_and_body_are_unchanged() {
        let provider = AnthropicProvider::new("anthropic", "sk-ant");
        let body = provider.build_body(&base_request()).unwrap();
        assert_eq!(body["system"], "Firmius system");
        assert!(body["system"].as_array().is_none());
        assert!(
            body["messages"][0]["content"][0]
                .get("cache_control")
                .is_none()
        );
        let headers = provider.auth_and_compat_headers(None).await.unwrap();
        assert_eq!(
            headers,
            vec![("x-api-key".to_string(), "sk-ant".to_string())]
        );
    }

    #[test]
    fn anthropic_body_serializes_base64_images() {
        let request = ProviderRequest {
            model: "claude-3-7-sonnet".into(),
            messages: vec![Message::with_parts(
                MessageRole::User,
                [
                    MessagePart::Text("inspect".into()),
                    MessagePart::Image(ImagePart::from_base64("image/png", "Zm9v")),
                ],
            )],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
        };
        let body = AnthropicProvider::new("anthropic", "sk-ant")
            .build_body(&request)
            .unwrap();
        assert_eq!(
            body["messages"][0]["content"][1],
            json!({
                "type": "image",
                "source": {
                    "type": "base64",
                    "media_type": "image/png",
                    "data": "Zm9v"
                }
            })
        );
    }

    #[test]
    fn anthropic_rejects_url_images() {
        let request = ProviderRequest {
            model: "claude-3-7-sonnet".into(),
            messages: vec![Message::with_parts(
                MessageRole::User,
                [MessagePart::Image(ImagePart::from_url(
                    "https://example.test/cat.png",
                ))],
            )],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
        };
        let error = AnthropicProvider::new("anthropic", "sk-ant")
            .build_body(&request)
            .unwrap_err();
        assert!(
            error
                .to_string()
                .contains("anthropic image inputs must be provided as base64 data")
        );
    }
}
