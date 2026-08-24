//! xAI Grok Responses API backend.
//!
//! Targets the SuperGrok subscription gateway (`cli-chat-proxy.grok.com/v1`),
//! which speaks OpenAI's Responses protocol with a few xAI-specific quirks:
//! system messages must move to top-level `instructions`, images use
//! `input_image` content parts, and conversation caching rides on the
//! `prompt_cache_key` field plus `reasoning.encrypted_content` replay.

use super::responses_web_search::{
    inject_web_search_tool, web_search_call_item, web_search_event_from_item,
};
use super::{
    Provider, ProviderError, ProviderEvent, TokenSupplier, dump_provider_request, parse_sse_lines,
};
use crate::types::{
    ImageSource, LLMWebSearch, MessagePart, MessageRole, ProviderCapabilities, ProviderRequest,
    StopReason, Usage, WebSearchContent, WebSearchMode,
};
use async_trait::async_trait;
use futures::{StreamExt, stream::BoxStream};
use serde_json::{Value, json};
use std::collections::HashMap;
use std::sync::Arc;

const GROK_WEB_SEARCH: LLMWebSearch = LLMWebSearch {
    modes: &[
        WebSearchMode::Cached,
        WebSearchMode::Indexed,
        WebSearchMode::Live,
    ],
    default_mode: WebSearchMode::Cached,
    content: WebSearchContent::Text,
    supports_filters: false,
    supports_location: false,
};

pub struct GrokProvider {
    id: String,
    base_url: String,
    auth: Arc<dyn TokenSupplier>,
    client: reqwest::Client,
    client_version: String,
    client_name: String,
}

impl GrokProvider {
    pub fn new(
        id: impl Into<String>,
        base_url: impl Into<String>,
        api_key: impl Into<String>,
    ) -> Self {
        Self::with_auth(id, base_url, Arc::new(super::StaticToken::bearer(api_key)))
    }

    pub fn with_auth(
        id: impl Into<String>,
        base_url: impl Into<String>,
        auth: Arc<dyn TokenSupplier>,
    ) -> Self {
        Self {
            id: id.into(),
            base_url: base_url.into(),
            auth,
            client: reqwest::Client::new(),
            client_version: env_or("PI_XAI_CLIENT_VERSION", "0.2.101"),
            client_name: env_or("PI_XAI_CLIENT_NAME", "grok-shell"),
        }
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

    /// Identity headers the cli-chat-proxy expects from a native client.
    /// `x-grok-model-override` routes the inference to a specific model.
    fn proxy_headers(&self, model: Option<&str>) -> Vec<(String, String)> {
        let mut headers = vec![
            (
                "User-Agent".to_string(),
                format!(
                    "{}/{}{}",
                    self.client_name,
                    self.client_version,
                    format!(" ({})", Self::platform_label())
                ),
            ),
            (
                "x-grok-client-identifier".to_string(),
                self.client_name.clone(),
            ),
            (
                "x-grok-client-version".to_string(),
                self.client_version.clone(),
            ),
            ("x-grok-client-mode".to_string(), "interactive".to_string()),
            ("X-XAI-Token-Auth".to_string(), "xai-grok-cli".to_string()),
            (
                "x-authenticateresponse".to_string(),
                "authenticate-response".to_string(),
            ),
        ];
        if let Some(model) = model {
            headers.push(("x-grok-model-override".to_string(), model.to_string()));
        }
        headers
    }

    fn build_body(&self, request: &ProviderRequest) -> Result<Value, ProviderError> {
        // xAI rejects `role: system` inside `input`; leading system messages
        // move to the top-level `instructions` field instead.
        let mut instructions: Vec<String> = Vec::new();
        let mut input: Vec<Value> = Vec::new();
        for message in &request.messages {
            if message.role == MessageRole::System {
                let mut text = String::new();
                for part in &message.content {
                    if let MessagePart::Text(t) = part {
                        text.push_str(t);
                    }
                }
                if !text.is_empty() {
                    instructions.push(text);
                }
                continue;
            }
            append_grok_input(message, &mut input);
        }

        let mut body = json!({
            "model": request.model,
            "input": input,
            "stream": true,
        });
        if !instructions.is_empty() {
            body["instructions"] = json!(instructions.join("\n\n"));
        }
        if !request.tools.is_empty() {
            body["tools"] = request
                .tools
                .iter()
                .map(|tool| {
                    json!({
                        "type": "function",
                        "name": tool.name,
                        "description": tool.description,
                        "parameters": tool.input_schema,
                    })
                })
                .collect();
        }
        inject_web_search_tool(&mut body, request.web_search.as_ref());
        if let Some(effort) = &request.reasoning_effort {
            body["reasoning"] = json!({ "effort": effort });
        }
        // Encrypted reasoning replay: lets prior reasoning ride across turns
        // without re-deriving it (the cli-chat-proxy honors this include).
        body["include"] = json!(["reasoning.encrypted_content"]);
        if let Some(t) = request.temperature {
            body["temperature"] = json!(t.clamp(0.0, 2.0));
        }
        if let Some(m) = request.max_tokens {
            body["max_output_tokens"] = json!(m);
        }
        if let Some(session_id) = &request.session_id {
            body["prompt_cache_key"] = json!(session_id);
        }
        Ok(body)
    }
}

fn env_or(name: &str, default: &str) -> String {
    std::env::var(name)
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| default.to_string())
}

/// Map one neutral message into Responses-API input items. Tool calls and
/// results become top-level `function_call` / `function_call_output` items;
/// text and images stay nested inside role items.
fn append_grok_input(message: &crate::types::Message, input: &mut Vec<Value>) {
    let mut content: Vec<Value> = Vec::new();
    for part in &message.content {
        match part {
            MessagePart::Text(value) => {
                content.push(json!({ "type": grok_text_type(message.role), "text": value }));
            }
            MessagePart::Image(image) => content.push(grok_image_part(image)),
            MessagePart::ToolCall { id, name, args } => {
                input.push(json!({
                    "type": "function_call",
                    "call_id": id,
                    "name": name,
                    "arguments": args,
                }));
            }
            MessagePart::ToolResult {
                id, content: value, ..
            } => {
                input.push(json!({
                    "type": "function_call_output",
                    "call_id": id,
                    "output": value,
                }));
            }
            // xAI rejects replayed `reasoning` items; encrypted replay via
            // `include` carries prior reasoning instead.
            MessagePart::Thinking { .. } => {}
            MessagePart::WebSearch { id, action } => {
                input.push(web_search_call_item(id, action));
            }
        }
    }
    match message.role {
        MessageRole::User if !content.is_empty() => input.push(json!({
            "role": "user",
            "content": content,
        })),
        MessageRole::Assistant if !content.is_empty() => input.push(json!({
            "role": "assistant",
            "content": content,
        })),
        _ => {}
    }
}

fn grok_text_type(role: MessageRole) -> &'static str {
    if role == MessageRole::Assistant {
        "output_text"
    } else {
        "input_text"
    }
}

fn grok_image_part(image: &crate::types::ImagePart) -> Value {
    let url = match &image.source {
        ImageSource::Url { url } => url.clone(),
        ImageSource::Base64 { media_type, data } => format!("data:{media_type};base64,{data}"),
    };
    let mut part = json!({ "type": "input_image", "image_url": url });
    if let Some(detail) = &image.detail {
        part["detail"] = json!(match detail {
            crate::types::ImageDetail::Low => "low",
            crate::types::ImageDetail::High => "high",
            crate::types::ImageDetail::Auto => "auto",
        });
    } else {
        part["detail"] = json!("auto");
    }
    part
}

fn parse_grok_sse_payload(
    value: &Value,
    tool_calls: &mut HashMap<String, (String, String, String)>,
    usage: &mut Usage,
    reason: &mut StopReason,
) -> Vec<ProviderEvent> {
    let mut events = Vec::new();
    match value
        .get("type")
        .and_then(Value::as_str)
        .unwrap_or_default()
    {
        "response.output_text.delta" => {
            if let Some(delta) = value.get("delta").and_then(Value::as_str) {
                events.push(ProviderEvent::TextDelta {
                    delta: delta.to_string(),
                });
            }
        }
        "response.reasoning_summary_text.delta"
        | "response.reasoning_text.delta"
        | "response.reasoning_summary_text.done" => {
            if let Some(delta) = value
                .get("delta")
                .or_else(|| value.get("text"))
                .and_then(Value::as_str)
            {
                events.push(ProviderEvent::ThinkingDelta {
                    delta: delta.to_string(),
                    signature: None,
                });
            }
        }
        "response.function_call_arguments.delta" => {
            if let (Some(item_id), Some(delta)) = (
                value.get("item_id").and_then(Value::as_str),
                value.get("delta").and_then(Value::as_str),
            ) {
                let call_id = tool_calls
                    .get(item_id)
                    .map(|(call_id, _, _)| call_id.clone())
                    .unwrap_or_else(|| item_id.to_string());
                let entry = tool_calls
                    .entry(item_id.to_string())
                    .or_insert_with(|| (call_id.clone(), String::new(), String::new()));
                entry.2.push_str(delta);
                events.push(ProviderEvent::ToolCallDelta {
                    index: 0,
                    id: Some(call_id),
                    name_delta: String::new(),
                    args_delta: delta.to_string(),
                });
            }
        }
        "response.output_item.added" => {
            let Some(item) = value.get("item") else {
                return events;
            };
            if let Some(event) = web_search_event_from_item(item, false) {
                events.push(event);
                return events;
            }
            if item.get("type").and_then(Value::as_str) == Some("reasoning") {
                if let Some(summary) = item.get("summary").and_then(Value::as_array) {
                    for part in summary {
                        if let Some(text) = part.get("text").and_then(Value::as_str) {
                            events.push(ProviderEvent::ThinkingDelta {
                                delta: text.to_string(),
                                signature: None,
                            });
                        }
                    }
                }
                return events;
            }
            if item.get("type").and_then(Value::as_str) != Some("function_call") {
                return events;
            }
            let Some(item_id) = item.get("id").and_then(Value::as_str) else {
                return events;
            };
            let call_id = item
                .get("call_id")
                .and_then(Value::as_str)
                .unwrap_or(item_id)
                .to_string();
            let name = item
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string();
            let args = item
                .get("arguments")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string();
            tool_calls.insert(
                item_id.to_string(),
                (call_id.clone(), name.clone(), args.clone()),
            );
            events.push(ProviderEvent::ToolCallDelta {
                index: 0,
                id: Some(call_id),
                name_delta: name,
                args_delta: args,
            });
        }
        "response.output_item.done" => {
            if let Some(item) = value.get("item") {
                if let Some(event) = web_search_event_from_item(item, true) {
                    events.push(event);
                }
            }
        }
        "response.function_call_arguments.done" => {
            let Some(item_id) = value.get("item_id").and_then(Value::as_str) else {
                return events;
            };
            let Some((call_id, name, mut args)) = tool_calls.remove(item_id) else {
                return events;
            };
            if let Some(final_args) = value.get("arguments").and_then(Value::as_str) {
                args = final_args.to_string();
            }
            events.push(ProviderEvent::ToolCall {
                id: call_id,
                name,
                args,
            });
        }
        "response.completed" => {
            if let Some(response) = value.get("response") {
                if let Some(usage_value) = response.get("usage") {
                    *usage = parse_grok_usage(usage_value);
                }
                if response.get("status").and_then(Value::as_str) == Some("incomplete") {
                    *reason = StopReason::MaxTokens;
                }
            }
        }
        _ => {}
    }
    events
}

fn parse_grok_usage(value: &Value) -> Usage {
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
            .get("input_tokens_details")
            .and_then(|d| d.get("cached_tokens"))
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        cache_write_tokens: 0,
    }
}

#[async_trait]
impl Provider for GrokProvider {
    fn id(&self) -> &str {
        &self.id
    }

    fn capabilities(&self) -> ProviderCapabilities {
        // Advertised only because body/replay/SSE unit tests below prove the
        // mapping. The cli-chat-proxy speaks the same Responses dialect as
        // Codex. If a live proxy later rejects `{type: web_search}`, drop
        // this to None rather than inventing a local tool named web_search.
        ProviderCapabilities {
            web_search: Some(GROK_WEB_SEARCH),
        }
    }

    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError> {
        let body = self.build_body(&request)?;
        dump_provider_request(&self.id, &body);
        let model = request.model.clone();
        let mut builder = self
            .client
            .post(format!("{}/responses", self.base_url.trim_end_matches('/')))
            .json(&body);
        for (name, value) in self.auth.headers().await? {
            builder = builder.header(name, value);
        }
        for (name, value) in self.proxy_headers(Some(&model)) {
            builder = builder.header(name, value);
        }
        let response = builder
            .send()
            .await
            .map_err(|error| ProviderError::Http(error.to_string()))?;
        let status = response.status();
        if !status.is_success() {
            return Err(ProviderError::Api {
                status: status.as_u16(),
                body: response.text().await.unwrap_or_default(),
            });
        }
        let mut bytes = response.bytes_stream();
        let stream = async_stream::try_stream! {
            let mut buffer = String::new();
            let mut usage = Usage::default();
            let mut reason = StopReason::Stop;
            let mut tool_calls: HashMap<String, (String, String, String)> = HashMap::new();
            while let Some(chunk) = bytes.next().await {
                let chunk = chunk.map_err(|error| ProviderError::Http(error.to_string()))?;
                buffer.push_str(&String::from_utf8_lossy(&chunk));
                for payload in parse_sse_lines(&mut buffer) {
                    if payload == "[DONE]" { continue; }
                    let value: Value = serde_json::from_str(&payload)
                        .map_err(|error| ProviderError::Decode(error.to_string()))?;
                    for event in parse_grok_sse_payload(
                        &value,
                        &mut tool_calls,
                        &mut usage,
                        &mut reason,
                    ) {
                        yield event;
                    }
                }
            }
            for (_, (id, name, args)) in tool_calls {
                yield ProviderEvent::ToolCall { id, name, args };
            }
            yield ProviderEvent::Usage { usage };
            yield ProviderEvent::Done { reason };
        };
        Ok(Box::pin(stream))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{
        ImageDetail, ImagePart, Message, ToolDefinition, WebSearchAction, WebSearchMode,
        WebSearchRequest,
    };
    use std::collections::HashMap;

    fn base_request() -> ProviderRequest {
        ProviderRequest {
            model: "grok-build".to_string(),
            messages: vec![
                Message::text(MessageRole::System, "you are firmius"),
                Message::text(MessageRole::User, "inspect this"),
            ],
            tools: vec![],
            temperature: None,
            max_tokens: None,
            reasoning_effort: Some("high".to_string()),
            thinking_budget_tokens: None,
            session_id: Some("session-1".to_string()),
            web_search: None,
        }
    }

    fn provider() -> GrokProvider {
        GrokProvider::new("grok", "https://cli-chat-proxy.grok.com/v1", "token")
    }

    #[test]
    fn body_moves_system_to_instructions_and_sets_caching_fields() {
        let body = provider().build_body(&base_request()).unwrap();
        assert_eq!(body["instructions"], "you are firmius");
        assert!(
            body.get("input")
                .and_then(Value::as_array)
                .is_some_and(|a| a.len() == 1)
        );
        assert_eq!(body["reasoning"], json!({ "effort": "high" }));
        assert_eq!(body["include"], json!(["reasoning.encrypted_content"]));
        assert_eq!(body["prompt_cache_key"], "session-1");
        assert!(body.get("store").is_none());
    }

    #[test]
    fn body_serializes_images_as_input_image_data_uris() {
        let request = ProviderRequest {
            model: "grok-build".into(),
            messages: vec![Message::with_parts(
                MessageRole::User,
                [
                    MessagePart::Text("look".into()),
                    MessagePart::Image(ImagePart::from_base64("image/png", "Zm9v")),
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
        let body = provider().build_body(&request).unwrap();
        let content = body["input"][0]["content"].as_array().unwrap();
        assert_eq!(content[0]["type"], "input_text");
        assert_eq!(
            content[1],
            json!({ "type": "input_image", "image_url": "data:image/png;base64,Zm9v", "detail": "auto" })
        );
        assert_eq!(
            content[2],
            json!({
                "type": "input_image",
                "image_url": "https://example.test/cat.png",
                "detail": "high"
            })
        );
    }

    #[test]
    fn body_emits_function_call_items_and_outputs() {
        let request = ProviderRequest {
            model: "grok-build".into(),
            messages: vec![
                Message::text(MessageRole::User, "call it"),
                Message {
                    role: MessageRole::Assistant,
                    content: vec![MessagePart::ToolCall {
                        id: "call-1".into(),
                        name: "bash".into(),
                        args: "{\"command\":\"pwd\"}".into(),
                    }],
                },
                Message::tool_results([MessagePart::ToolResult {
                    id: "call-1".into(),
                    content: "/home".into(),
                    ok: true,
                }]),
            ],
            tools: vec![ToolDefinition {
                name: "bash".into(),
                description: "run".into(),
                input_schema: json!({ "type": "object" }),
            }],
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: None,
            web_search: None,
        };
        let body = provider().build_body(&request).unwrap();
        let input = body["input"].as_array().unwrap();
        assert_eq!(input.len(), 3);
        assert_eq!(input[1]["type"], "function_call");
        assert_eq!(input[1]["call_id"], "call-1");
        assert_eq!(input[2]["type"], "function_call_output");
        assert_eq!(input[2]["output"], "/home");
        assert_eq!(body["tools"][0]["type"], "function");
    }

    #[test]
    fn body_clamps_temperature_into_xai_range() {
        let mut request = base_request();
        request.temperature = Some(9.0);
        let body = provider().build_body(&request).unwrap();
        assert_eq!(body["temperature"], json!(2.0));
    }

    #[test]
    fn usage_maps_cached_tokens_from_input_details() {
        let usage = parse_grok_usage(&json!({
            "input_tokens": 120,
            "output_tokens": 40,
            "input_tokens_details": { "cached_tokens": 100 },
            "output_tokens_details": { "reasoning_tokens": 12 },
        }));
        assert_eq!(
            usage,
            Usage {
                input_tokens: 120,
                output_tokens: 40,
                cache_read_tokens: 100,
                cache_write_tokens: 0,
            }
        );
    }

    fn with_search(mut request: ProviderRequest, mode: WebSearchMode) -> ProviderRequest {
        request.web_search = Some(WebSearchRequest { mode });
        request
    }

    fn bash_tool() -> ToolDefinition {
        ToolDefinition {
            name: "bash".into(),
            description: "run".into(),
            input_schema: json!({ "type": "object" }),
        }
    }

    fn web_search_tool(body: &Value) -> Option<&Value> {
        body.get("tools")
            .and_then(Value::as_array)
            .and_then(|tools| {
                tools
                    .iter()
                    .find(|t| t.get("type") == Some(&json!("web_search")))
            })
    }

    fn drive_sse(payloads: &[Value]) -> Vec<ProviderEvent> {
        let mut tool_calls = HashMap::new();
        let mut usage = Usage::default();
        let mut reason = StopReason::Stop;
        let mut events = Vec::new();
        for payload in payloads {
            events.extend(parse_grok_sse_payload(
                payload,
                &mut tool_calls,
                &mut usage,
                &mut reason,
            ));
        }
        events
    }

    #[test]
    fn capabilities_advertise_web_search() {
        let caps = provider().capabilities();
        let search = caps.web_search.expect("grok advertises hosted search");
        assert!(search.modes.contains(&WebSearchMode::Cached));
        assert!(search.modes.contains(&WebSearchMode::Live));
        assert!(search.modes.contains(&WebSearchMode::Indexed));
    }

    #[test]
    fn body_omits_web_search_when_request_field_is_none() {
        let mut request = base_request();
        request.tools = vec![bash_tool()];
        let body = provider().build_body(&request).unwrap();
        assert!(web_search_tool(&body).is_none());
        assert_eq!(body["tools"].as_array().unwrap().len(), 1);
        assert_eq!(body["tools"][0]["type"], "function");
        assert_eq!(body["tools"][0]["name"], "bash");
    }

    #[test]
    fn body_injects_web_search_next_to_function_tools() {
        let mut request = base_request();
        request.tools = vec![bash_tool()];
        let body = provider()
            .build_body(&with_search(request, WebSearchMode::Live))
            .unwrap();
        let tools = body["tools"].as_array().unwrap();
        assert_eq!(tools.len(), 2);
        assert_eq!(tools[0]["type"], "function");
        assert_eq!(tools[0]["name"], "bash");
        assert_eq!(
            tools[1],
            json!({ "type": "web_search", "external_web_access": true })
        );
    }

    #[test]
    fn body_maps_web_search_modes_to_codex_flags() {
        let cached = provider()
            .build_body(&with_search(base_request(), WebSearchMode::Cached))
            .unwrap();
        assert_eq!(
            web_search_tool(&cached).cloned().unwrap(),
            json!({ "type": "web_search", "external_web_access": false })
        );

        let indexed = provider()
            .build_body(&with_search(base_request(), WebSearchMode::Indexed))
            .unwrap();
        assert_eq!(
            web_search_tool(&indexed).cloned().unwrap(),
            json!({
                "type": "web_search",
                "external_web_access": true,
                "indexed_web_access": true,
            })
        );

        let live = provider()
            .build_body(&with_search(base_request(), WebSearchMode::Live))
            .unwrap();
        assert_eq!(
            web_search_tool(&live).cloned().unwrap(),
            json!({ "type": "web_search", "external_web_access": true })
        );
        assert!(live["tools"][0].get("indexed_web_access").is_none());
    }

    #[test]
    fn replay_serializes_web_search_as_web_search_call_not_function_call() {
        let request = ProviderRequest {
            model: "grok-build".into(),
            messages: vec![
                Message::text(MessageRole::User, "search rust"),
                Message::with_parts(
                    MessageRole::Assistant,
                    [
                        MessagePart::WebSearch {
                            id: "ws-1".into(),
                            action: WebSearchAction::Search {
                                query: Some("rust async".into()),
                                queries: None,
                            },
                        },
                        MessagePart::Text("here is what I found".into()),
                    ],
                ),
            ],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: None,
            web_search: None,
        };
        let input = provider().build_body(&request).unwrap()["input"]
            .as_array()
            .unwrap()
            .clone();
        assert_eq!(input.len(), 3);
        assert_eq!(input[0]["role"], "user");
        assert_eq!(input[1]["type"], "web_search_call");
        assert_eq!(input[1]["id"], "ws-1");
        assert_eq!(input[1]["status"], "completed");
        assert_eq!(input[1]["action"]["type"], "search");
        assert_eq!(input[1]["action"]["query"], "rust async");
        assert_ne!(input[1]["type"], "function_call");
        assert!(input[1].get("name").is_none());
        assert_eq!(input[2]["role"], "assistant");
        assert_eq!(input[2]["content"][0]["type"], "output_text");
    }

    #[test]
    fn sse_web_search_call_emits_started_and_finished_never_tool_call() {
        let events = drive_sse(&[
            json!({
                "type": "response.output_item.added",
                "item": {
                    "type": "web_search_call",
                    "id": "ws-1",
                    "status": "in_progress",
                }
            }),
            json!({
                "type": "response.output_item.done",
                "item": {
                    "type": "web_search_call",
                    "id": "ws-1",
                    "status": "completed",
                    "action": { "type": "search", "query": "rust async" },
                }
            }),
            json!({
                "type": "response.output_text.delta",
                "delta": "found it",
            }),
        ]);
        assert_eq!(
            events,
            vec![
                ProviderEvent::WebSearchStarted { id: "ws-1".into() },
                ProviderEvent::WebSearchFinished {
                    id: "ws-1".into(),
                    action: WebSearchAction::Search {
                        query: Some("rust async".into()),
                        queries: None,
                    },
                },
                ProviderEvent::TextDelta {
                    delta: "found it".into(),
                },
            ]
        );
        assert!(events.iter().all(|event| !matches!(
            event,
            ProviderEvent::ToolCall { .. } | ProviderEvent::ToolCallDelta { .. }
        )));
    }

    #[test]
    fn sse_web_search_finish_without_action_is_other() {
        let events = drive_sse(&[
            json!({
                "type": "response.output_item.added",
                "item": { "type": "web_search_call", "id": "ws-2" }
            }),
            json!({
                "type": "response.output_item.done",
                "item": { "type": "web_search_call", "id": "ws-2", "status": "completed" }
            }),
        ]);
        assert_eq!(
            events,
            vec![
                ProviderEvent::WebSearchStarted { id: "ws-2".into() },
                ProviderEvent::WebSearchFinished {
                    id: "ws-2".into(),
                    action: WebSearchAction::Other,
                },
            ]
        );
    }

    #[test]
    fn sse_function_call_path_unchanged() {
        let events = drive_sse(&[
            json!({
                "type": "response.output_item.added",
                "item": {
                    "type": "function_call",
                    "id": "item-1",
                    "call_id": "call-1",
                    "name": "bash",
                    "arguments": "",
                }
            }),
            json!({
                "type": "response.function_call_arguments.delta",
                "item_id": "item-1",
                "delta": "{\"command\":\"pwd\"}",
            }),
            json!({
                "type": "response.function_call_arguments.done",
                "item_id": "item-1",
                "arguments": "{\"command\":\"pwd\"}",
            }),
        ]);
        assert_eq!(
            events,
            vec![
                ProviderEvent::ToolCallDelta {
                    index: 0,
                    id: Some("call-1".into()),
                    name_delta: "bash".into(),
                    args_delta: "".into(),
                },
                ProviderEvent::ToolCallDelta {
                    index: 0,
                    id: Some("call-1".into()),
                    name_delta: String::new(),
                    args_delta: "{\"command\":\"pwd\"}".into(),
                },
                ProviderEvent::ToolCall {
                    id: "call-1".into(),
                    name: "bash".into(),
                    args: "{\"command\":\"pwd\"}".into(),
                },
            ]
        );
        assert!(events.iter().all(|event| !matches!(
            event,
            ProviderEvent::WebSearchStarted { .. } | ProviderEvent::WebSearchFinished { .. }
        )));
    }

    #[test]
    fn sse_open_page_and_find_in_page_actions() {
        let events = drive_sse(&[
            json!({
                "type": "response.output_item.done",
                "item": {
                    "type": "web_search_call",
                    "id": "ws-open",
                    "action": { "type": "open_page", "url": "https://example.test" },
                }
            }),
            json!({
                "type": "response.output_item.done",
                "item": {
                    "type": "web_search_call",
                    "id": "ws-find",
                    "action": {
                        "type": "find_in_page",
                        "url": "https://example.test",
                        "pattern": "async",
                    },
                }
            }),
        ]);
        assert_eq!(
            events,
            vec![
                ProviderEvent::WebSearchFinished {
                    id: "ws-open".into(),
                    action: WebSearchAction::OpenPage {
                        url: Some("https://example.test".into()),
                    },
                },
                ProviderEvent::WebSearchFinished {
                    id: "ws-find".into(),
                    action: WebSearchAction::FindInPage {
                        url: Some("https://example.test".into()),
                        pattern: Some("async".into()),
                    },
                },
            ]
        );
    }
}
