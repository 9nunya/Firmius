use super::{Provider, ProviderError, ProviderEvent, parse_sse_lines};
use crate::types::{MessagePart, MessageRole, ProviderRequest, StopReason, Usage};
use async_trait::async_trait;
use futures::{StreamExt, stream::BoxStream};
use serde_json::{Value, json};

/// Anthropic Messages API backend.
pub struct AnthropicProvider {
    id: String,
    base_url: String,
    api_key: String,
    version: String,
    client: reqwest::Client,
}

impl AnthropicProvider {
    pub fn new(id: impl Into<String>, api_key: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            base_url: "https://api.anthropic.com".to_string(),
            api_key: api_key.into(),
            version: "2023-06-01".to_string(),
            client: reqwest::Client::new(),
        }
    }

    pub fn with_base_url(mut self, base_url: impl Into<String>) -> Self {
        self.base_url = base_url.into();
        self
    }

    fn build_body(&self, request: &ProviderRequest) -> Value {
        // Anthropic wants system prompts as a top-level field, not in messages.
        let mut system = String::new();
        let mut messages: Vec<Value> = Vec::new();
        for message in &request.messages {
            if message.role == MessageRole::System {
                for part in &message.content {
                    if let MessagePart::Text(t) = part {
                        system.push_str(t);
                    }
                }
                continue;
            }
            messages.push(message_to_anthropic(message));
        }

        let mut body = json!({
            "model": request.model,
            "messages": messages,
            "stream": true,
            "max_tokens": request.max_tokens.unwrap_or(4096),
        });
        if !system.is_empty() {
            body["system"] = json!(system);
        }
        if !request.tools.is_empty() {
            body["tools"] = request
                .tools
                .iter()
                .map(|t| json!({ "name": t.name, "description": t.description, "input_schema": t.input_schema }))
                .collect();
        }
        if let Some(t) = request.temperature {
            body["temperature"] = json!(t);
        }
        if let Some(budget) = request.thinking_budget_tokens {
            body["thinking"] = json!({
                "type": "enabled",
                "budget_tokens": budget,
            });
        }
        body
    }
}

fn message_to_anthropic(message: &crate::types::Message) -> Value {
    let role = match message.role {
        MessageRole::Assistant => "assistant",
        // User and Tool both map to a user-role message; tool results are
        // user-side `tool_result` content blocks in Anthropic's model.
        _ => "user",
    };
    let blocks: Vec<Value> = message
        .content
        .iter()
        .filter_map(|part| match part {
            MessagePart::Text(t) => Some(json!({ "type": "text", "text": t })),
            MessagePart::ToolCall { id, name, args } => {
                let input: Value = serde_json::from_str(args).unwrap_or_else(|_| json!({}));
                Some(json!({ "type": "tool_use", "id": id, "name": name, "input": input }))
            }
            MessagePart::ToolResult { id, content, ok } => Some(json!({
                "type": "tool_result",
                "tool_use_id": id,
                "content": content,
                "is_error": !ok,
            })),
            MessagePart::Thinking { content, signature } => signature
                .as_ref()
                .map(|sig| json!({ "type": "thinking", "thinking": content, "signature": sig })),
        })
        .collect();
    json!({ "role": role, "content": blocks })
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
        let body = self.build_body(&request);
        let response = self
            .client
            .post(format!(
                "{}/v1/messages",
                self.base_url.trim_end_matches('/')
            ))
            .header("x-api-key", &self.api_key)
            .header("anthropic-version", &self.version)
            .json(&body)
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
                        "content_block_start" => {
                            let block = value.get("content_block");
                            let kind = block.and_then(|b| b.get("type")).and_then(Value::as_str).unwrap_or("");
                            if kind == "tool_use" {
                                block_is_tool = true;
                                block_id = block.and_then(|b| b.get("id")).and_then(Value::as_str).unwrap_or("").to_string();
                                block_name = block.and_then(|b| b.get("name")).and_then(Value::as_str).unwrap_or("").to_string();
                                block_args.clear();
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
                                            index: 0,
                                            id: Some(block_id.clone()),
                                            name_delta: block_name.clone(),
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
                                usage = parse_anthropic_usage(u);
                            }
                        }
                        "message_stop" => {
                            if let Some(u) = value.get("usage") {
                                usage = parse_anthropic_usage(u);
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

fn parse_anthropic_usage(value: &Value) -> Usage {
    Usage {
        input_tokens: value
            .get("input_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        output_tokens: value
            .get("output_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        cache_read_tokens: value
            .get("cache_read_input_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        cache_write_tokens: value
            .get("cache_creation_input_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
    }
}
