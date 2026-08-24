use super::{
    Provider, ProviderError, ProviderEvent, StaticToken, TokenSupplier, dump_provider_request,
    parse_sse_lines,
};
use crate::types::{
    ImageDetail, ImageSource, MessagePart, MessageRole, ProviderRequest, StopReason, Usage,
};
use async_trait::async_trait;
use futures::{StreamExt, stream::BoxStream};
use serde_json::{Value, json};
use std::sync::Arc;

/// OpenAI-compatible chat completions backend (OpenAI, OpenRouter, Qwen, etc.).
pub struct OpenAiProvider {
    id: String,
    base_url: String,
    auth: Arc<dyn TokenSupplier>,
    reasoning_field: String,
    client: reqwest::Client,
    extra_body: Value,
}

impl OpenAiProvider {
    /// Convenience constructor for bearer-token auth.
    pub fn new(
        id: impl Into<String>,
        base_url: impl Into<String>,
        api_key: impl Into<String>,
    ) -> Self {
        Self::with_auth(id, base_url, Arc::new(StaticToken::bearer(api_key)))
    }

    /// Build with any token supplier — static key today, OAuth refresh later.
    pub fn with_auth(
        id: impl Into<String>,
        base_url: impl Into<String>,
        auth: Arc<dyn TokenSupplier>,
    ) -> Self {
        Self {
            id: id.into(),
            base_url: base_url.into(),
            auth,
            reasoning_field: "reasoning_content".to_string(),
            client: reqwest::Client::new(),
            extra_body: Value::Object(Default::default()),
        }
    }

    /// Field name used for reasoning deltas (`reasoning_content`, `reasoning`, ...).
    pub fn with_reasoning_field(mut self, field: impl Into<String>) -> Self {
        self.reasoning_field = field.into();
        self
    }

    /// Merge extra JSON fields into every chat-completions body.
    pub fn with_extra_body(mut self, extra: Value) -> Self {
        self.extra_body = extra;
        self
    }

    fn build_body(&self, request: &ProviderRequest) -> Result<Value, ProviderError> {
        let mut messages: Vec<Value> = Vec::new();
        for message in &request.messages {
            append_openai_messages(message, &mut messages)?;
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
        if let Some(session_id) = &request.session_id {
            body["prompt_cache_key"] = json!(session_id);
        }
        if let Some(extra) = self.extra_body.as_object() {
            for (key, value) in extra {
                if body.get(key).is_none() {
                    body[key] = value.clone();
                }
            }
        }
        if body.get("runId").and_then(Value::as_str).is_none() {
            if let Some(session_id) = &request.session_id {
                body["runId"] = json!(session_id);
            }
        }
        Ok(body)
    }
}

/// Map one neutral message into one or more OpenAI chat messages. Tool result
/// messages expand into one `role: tool` object per result.
pub(crate) fn append_openai_messages(
    message: &crate::types::Message,
    out: &mut Vec<Value>,
) -> Result<(), ProviderError> {
    match message.role {
        MessageRole::Tool => {
            for part in &message.content {
                if let MessagePart::ToolResult { id, content, .. } = part {
                    out.push(json!({ "role": "tool", "tool_call_id": id, "content": content }));
                }
            }
            Ok(())
        }
        role => {
            let role_str = match role {
                MessageRole::User => "user",
                MessageRole::Assistant => "assistant",
                MessageRole::System => "system",
                MessageRole::Tool => unreachable!(),
            };
            let mut text = String::new();
            let mut content: Vec<Value> = Vec::new();
            let mut tool_calls: Vec<Value> = Vec::new();
            for part in &message.content {
                match part {
                    MessagePart::Text(t) => {
                        text.push_str(t);
                        content.push(json!({ "type": "text", "text": t }));
                    }
                    MessagePart::Image(image) => {
                        content.push(openai_image_part(image));
                    }
                    MessagePart::ToolCall { id, name, args } => tool_calls.push(json!({
                        "id": id,
                        "type": "function",
                        "function": { "name": name, "arguments": args },
                    })),
                    // OpenAI can't represent thinking blocks natively; folding
                    // reasoning into text preserves it for the model.
                    MessagePart::Thinking { content: value, .. } => {
                        text.push_str(value);
                        content.push(json!({ "type": "text", "text": value }));
                    }
                    MessagePart::ToolResult { .. } => {}
                    // Chat Completions cannot replay hosted search; omit.
                    MessagePart::WebSearch { .. } => {}
                }
            }
            let serialized_content = if content
                .iter()
                .all(|part| part.get("type").and_then(Value::as_str) == Some("text"))
            {
                json!(text)
            } else {
                json!(content)
            };
            let mut obj = json!({ "role": role_str, "content": serialized_content });
            if !tool_calls.is_empty() {
                obj["tool_calls"] = json!(tool_calls);
            }
            out.push(obj);
            Ok(())
        }
    }
}

fn openai_image_part(image: &crate::types::ImagePart) -> Value {
    let mut image_url = match &image.source {
        ImageSource::Url { url } => json!({ "url": url }),
        ImageSource::Base64 { media_type, data } => {
            json!({ "url": format!("data:{media_type};base64,{data}") })
        }
    };
    if let Some(detail) = &image.detail {
        image_url["detail"] = json!(match detail {
            ImageDetail::Low => "low",
            ImageDetail::High => "high",
            ImageDetail::Auto => "auto",
        });
    }
    json!({ "type": "image_url", "image_url": image_url })
}

/// Accumulator that stitches partial tool-call deltas into finalized calls.
///
/// Every slot gets a stable identity: the provider-supplied `id` when present,
/// otherwise a synthetic `call-{slot}` id. Live `ToolCallDelta` events carry
/// that same identity, so the TUI can correlate parallel tool calls without
/// depending on a backend-provided `index` (which some OpenAI-compatible
/// endpoints omit on intermediate chunks).
#[derive(Default)]
struct ToolCallAccumulator {
    slots: Vec<(String, String, String)>, // (id, name, args)
}

impl ToolCallAccumulator {
    fn ingest(
        &mut self,
        index: usize,
        id: Option<&str>,
        name: &str,
        args: &str,
    ) -> (usize, String) {
        let slot = match id.filter(|id| !id.is_empty()) {
            Some(id) => match self.slots.iter().position(|(slot_id, _, _)| slot_id == id) {
                Some(existing) => existing,
                None => {
                    // Reuse the index slot only when it is still unclaimed;
                    // otherwise append so a colliding index never merges two
                    // distinct tool calls.
                    if self
                        .slots
                        .get(index)
                        .is_none_or(|(slot_id, _, _)| slot_id.is_empty())
                    {
                        index
                    } else {
                        self.slots
                            .push((String::new(), String::new(), String::new()));
                        self.slots.len() - 1
                    }
                }
            },
            None => index,
        };
        if self.slots.len() <= slot {
            self.slots
                .resize(slot + 1, (String::new(), String::new(), String::new()));
        }
        let entry = &mut self.slots[slot];
        if entry.0.is_empty() {
            entry.0 = id
                .filter(|id| !id.is_empty())
                .map(str::to_string)
                .unwrap_or_else(|| format!("call-{slot}"));
        }
        entry.1.push_str(name);
        entry.2.push_str(args);
        (slot, entry.0.clone())
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
        let body = self.build_body(&request)?;
        dump_provider_request(&self.id, &body);
        let mut request_builder = self
            .client
            .post(format!(
                "{}/chat/completions",
                self.base_url.trim_end_matches('/')
            ))
            .json(&body);
        for (name, value) in self.auth.headers().await? {
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
                            let (slot, slot_id) = accumulator.ingest(index, id, name, args);
                            yield ProviderEvent::ToolCallDelta {
                                index: slot as u32,
                                id: Some(slot_id),
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{ImagePart, Message};

    #[test]
    fn build_body_uses_multimodal_content_for_image_messages() {
        let provider = OpenAiProvider::new("openai", "https://example.test/v1", "sk-test");
        let request = ProviderRequest {
            model: "gpt-4.1".into(),
            messages: vec![Message::with_parts(
                MessageRole::User,
                [
                    MessagePart::Text("what is in this image?".into()),
                    MessagePart::Image(
                        ImagePart::from_url("https://example.test/cat.png")
                            .with_detail(ImageDetail::High),
                    ),
                ],
            )],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: None,
            web_search: None,
        };

        let body = provider.build_body(&request).unwrap();
        assert_eq!(
            body["messages"][0]["content"][0],
            json!({ "type": "text", "text": "what is in this image?" })
        );
        assert_eq!(
            body["messages"][0]["content"][1],
            json!({
                "type": "image_url",
                "image_url": {
                    "url": "https://example.test/cat.png",
                    "detail": "high"
                }
            })
        );
    }

    #[test]
    fn build_body_sends_prompt_cache_key_from_session_id() {
        let provider = OpenAiProvider::new("openai", "https://example.test/v1", "sk-test");
        let request = ProviderRequest {
            model: "gpt-4.1".into(),
            messages: vec![Message::text(MessageRole::User, "hi")],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: Some("session-42".into()),
            web_search: None,
        };
        let body = provider.build_body(&request).unwrap();
        assert_eq!(body["prompt_cache_key"], "session-42");
        assert_eq!(body["reasoning_effort"], Value::Null);
        assert_eq!(body["runId"], "session-42");
    }

    #[test]
    fn accumulator_gives_parallel_calls_stable_distinct_ids() {
        // Simulate a backend that omits `index` on intermediate chunks (so it
        // defaults to 0) but supplies ids. Each call must stay distinct.
        let mut acc = ToolCallAccumulator::default();
        acc.ingest(0, Some("call-a"), "bash", r#"{"command":"pwd"}"#);
        acc.ingest(0, Some("call-b"), "mcp__srv__ver", "{}");
        acc.ingest(0, Some("call-a"), "", " && ls");
        acc.ingest(0, Some("call-b"), "", "");

        let finalized = acc.finalize();
        assert_eq!(finalized.len(), 2);
        let events: Vec<_> = finalized
            .into_iter()
            .map(|event| match event {
                ProviderEvent::ToolCall { id, name, args } => (id, name, args),
                _ => unreachable!(),
            })
            .collect();
        let bash = events.iter().find(|(id, _, _)| id == "call-a").unwrap();
        assert_eq!(bash.1, "bash");
        assert_eq!(bash.2, r#"{"command":"pwd"} && ls"#);
        let version = events.iter().find(|(id, _, _)| id == "call-b").unwrap();
        assert_eq!(version.1, "mcp__srv__ver");
        assert_eq!(version.2, "{}");
    }
}

fn parse_openai_usage(value: &Value) -> Usage {
    Usage {
        input_tokens: value
            .get("prompt_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        output_tokens: value
            .get("completion_tokens")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        cache_read_tokens: value
            .get("prompt_tokens_details")
            .and_then(|d| d.get("cached_tokens"))
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        cache_write_tokens: 0,
    }
}
