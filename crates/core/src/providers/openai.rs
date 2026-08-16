use super::{Provider, ProviderError, ProviderEvent, parse_sse_lines};
use crate::types::{MessagePart, MessageRole, ProviderRequest, StopReason, Usage};
use async_trait::async_trait;
use futures::{StreamExt, stream::BoxStream};
use serde_json::{Value, json};

/// OpenAI-compatible chat completions backend (OpenAI, OpenRouter, Qwen, etc.).
pub struct OpenAiProvider {
    id: String,
    base_url: String,
    api_key: String,
    reasoning_field: String,
    client: reqwest::Client,
}

impl OpenAiProvider {
    pub fn new(
        id: impl Into<String>,
        base_url: impl Into<String>,
        api_key: impl Into<String>,
    ) -> Self {
        Self {
            id: id.into(),
            base_url: base_url.into(),
            api_key: api_key.into(),
            reasoning_field: "reasoning_content".to_string(),
            client: reqwest::Client::new(),
        }
    }

    /// Field name used for reasoning deltas (`reasoning_content`, `reasoning`, ...).
    pub fn with_reasoning_field(mut self, field: impl Into<String>) -> Self {
        self.reasoning_field = field.into();
        self
    }

    fn build_body(&self, request: &ProviderRequest) -> Value {
        let mut messages: Vec<Value> = Vec::new();
        for message in &request.messages {
            append_openai_messages(message, &mut messages);
        }
        let mut body = json!({
            "model": request.model,
            "messages": messages,
            "stream": true,
        });
        if !request.tools.is_empty() {
            body["tools"] = request
                .tools
                .iter()
                .map(|t| {
                    json!({
                        "type": "function",
                        "function": {
                            "name": t.name,
                            "description": t.description,
                            "parameters": t.input_schema,
                        }
                    })
                })
                .collect();
        }
        if let Some(t) = request.temperature {
            body["temperature"] = json!(t);
        }
        if let Some(m) = request.max_tokens {
            body["max_tokens"] = json!(m);
        }
        if let Some(ref effort) = request.reasoning_effort {
            body["reasoning_effort"] = json!(effort);
        }
        body
    }
}

/// Map one neutral message into one or more OpenAI chat messages. Tool result
/// messages expand into one `role: tool` object per result.
fn append_openai_messages(message: &crate::types::Message, out: &mut Vec<Value>) {
    match message.role {
        MessageRole::Tool => {
            for part in &message.content {
                if let MessagePart::ToolResult { id, content, .. } = part {
                    out.push(json!({ "role": "tool", "tool_call_id": id, "content": content }));
                }
            }
        }
        role => {
            let role_str = match role {
                MessageRole::User => "user",
                MessageRole::Assistant => "assistant",
                MessageRole::System => "system",
                MessageRole::Tool => unreachable!(),
            };
            let mut text = String::new();
            let mut tool_calls: Vec<Value> = Vec::new();
            for part in &message.content {
                match part {
                    MessagePart::Text(t) => text.push_str(t),
                    MessagePart::ToolCall { id, name, args } => tool_calls.push(json!({
                        "id": id,
                        "type": "function",
                        "function": { "name": name, "arguments": args },
                    })),
                    // OpenAI can't represent thinking blocks natively; folding
                    // reasoning into text preserves it for the model.
                    MessagePart::Thinking { content, .. } => text.push_str(content),
                    MessagePart::ToolResult { .. } => {}
                }
            }
            let mut obj = json!({ "role": role_str, "content": text });
            if !tool_calls.is_empty() {
                obj["tool_calls"] = json!(tool_calls);
            }
            out.push(obj);
        }
    }
}

/// Accumulator that stitches partial tool-call deltas into finalized calls.
#[derive(Default)]
struct ToolCallAccumulator {
    slots: Vec<(String, String, String)>, // (id, name, args)
}

impl ToolCallAccumulator {
    fn ingest(&mut self, index: usize, id: Option<&str>, name: &str, args: &str) {
        if self.slots.len() <= index {
            self.slots
                .resize(index + 1, (String::new(), String::new(), String::new()));
        }
        let slot = &mut self.slots[index];
        if let Some(id) = id
            && !id.is_empty()
        {
            slot.0 = id.to_string();
        }
        slot.1.push_str(name);
        slot.2.push_str(args);
    }

    fn finalize(self) -> Vec<ProviderEvent> {
        self.slots
            .into_iter()
            .filter(|(id, name, _)| !id.is_empty() || !name.is_empty())
            .map(|(id, name, args)| ProviderEvent::ToolCall { id, name, args })
            .collect()
    }
}

#[async_trait]
impl Provider for OpenAiProvider {
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
                "{}/chat/completions",
                self.base_url.trim_end_matches('/')
            ))
            .bearer_auth(&self.api_key)
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

        let reasoning_field = self.reasoning_field.clone();
        let mut byte_stream = response.bytes_stream();

        let stream = async_stream::try_stream! {
            let mut buffer = String::new();
            let mut accumulator = ToolCallAccumulator::default();
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

                    // Grab usage when it appears (some providers emit it per-chunk).
                    if let Some(u) = value.get("usage") {
                        usage = parse_openai_usage(u);
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
                        .get(reasoning_field.as_str())
                        .or_else(|| delta.get("reasoning"))
                        .and_then(Value::as_str)
                        && !reason.is_empty()
                    {
                        yield ProviderEvent::ThinkingDelta { delta: reason.to_string(), signature: None };
                    }

                    if let Some(calls) = delta.get("tool_calls").and_then(Value::as_array) {
                        for call in calls {
                            let index = call.get("index").and_then(Value::as_u64).unwrap_or(0) as usize;
                            let id = call.get("id").and_then(Value::as_str);
                            let name = call
                                .get("function")
                                .and_then(|f| f.get("name"))
                                .and_then(Value::as_str)
                                .unwrap_or("");
                            let args = call
                                .get("function")
                                .and_then(|f| f.get("arguments"))
                                .and_then(Value::as_str)
                                .unwrap_or("");
                            accumulator.ingest(index, id, name, args);
                            yield ProviderEvent::ToolCallDelta {
                                index: index as u32,
                                id: id.map(str::to_string),
                                name_delta: name.to_string(),
                                args_delta: args.to_string(),
                            };
                        }
                    }
                }
            }

            for finalized in accumulator.finalize() {
                yield finalized;
            }
            yield ProviderEvent::Usage { usage };
            yield ProviderEvent::Done { reason: finish_reason };
        };

        Ok(stream.boxed())
    }
}

fn parse_openai_usage(value: &Value) -> Usage {
    Usage {
        input_tokens: value.get("prompt_tokens").and_then(Value::as_u64).unwrap_or(0) as u32,
        output_tokens: value.get("completion_tokens").and_then(Value::as_u64).unwrap_or(0) as u32,
        cache_read_tokens: value
            .get("prompt_tokens_details")
            .and_then(|d| d.get("cached_tokens"))
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        cache_write_tokens: 0,
    }
}
