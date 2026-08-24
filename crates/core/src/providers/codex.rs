use super::responses_web_search::{
    inject_web_search_tool, web_search_call_item, web_search_event_from_item,
};
use super::{Provider, ProviderError, ProviderEvent, dump_provider_request, parse_sse_lines};
use crate::types::{
    ImageSource, LLMWebSearch, MessagePart, MessageRole, ProviderCapabilities, ProviderRequest,
    StopReason, Usage, WebSearchContent, WebSearchMode,
};
use async_trait::async_trait;
use futures::{StreamExt, stream::BoxStream};
use serde_json::{Value, json};
use std::collections::HashMap;

const CODEX_WEB_SEARCH: LLMWebSearch = LLMWebSearch {
    modes: &[
        WebSearchMode::Cached,
        WebSearchMode::Indexed,
        WebSearchMode::Live,
    ],
    default_mode: WebSearchMode::Cached,
    content: WebSearchContent::TextAndImage,
    supports_filters: false,
    supports_location: false,
};

pub struct CodexProvider {
    id: String,
    access_token: String,
    account_id: Option<String>,
    client: reqwest::Client,
}

impl CodexProvider {
    pub fn new(
        id: impl Into<String>,
        access_token: impl Into<String>,
        account_id: Option<String>,
    ) -> Self {
        Self {
            id: id.into(),
            access_token: access_token.into(),
            account_id,
            client: reqwest::Client::new(),
        }
    }

    fn body(request: &ProviderRequest) -> Value {
        let mut input = Vec::new();
        let mut instruction_parts: Vec<String> = Vec::new();
        for message in &request.messages {
            let mut text = String::new();
            let mut content = Vec::new();
            for part in &message.content {
                match part {
                    MessagePart::Text(value) | MessagePart::Thinking { content: value, .. } => {
                        text.push_str(value);
                        let part_type = if message.role == MessageRole::Assistant {
                            "output_text"
                        } else {
                            "input_text"
                        };
                        content.push(json!({ "type": part_type, "text": value }));
                    }
                    MessagePart::Image(image) => content.push(codex_image_part(image)),
                    MessagePart::ToolCall { id, name, args } => input.push(json!({
                        "type": "function_call",
                        "call_id": id,
                        "name": name,
                        "arguments": args,
                    })),
                    MessagePart::ToolResult { id, content, .. } => input.push(json!({
                        "type": "function_call_output",
                        "call_id": id,
                        "output": content,
                    })),
                    MessagePart::WebSearch { id, action } => {
                        input.push(web_search_call_item(id, action));
                    }
                }
            }
            match message.role {
                MessageRole::System if !text.is_empty() => instruction_parts.push(text),
                MessageRole::User if !content.is_empty() => {
                    input.push(json!({
                        "role": "user",
                        "content": content,
                    }));
                }
                MessageRole::Assistant if !content.is_empty() => {
                    input.push(json!({
                        "role": "assistant",
                        "content": content,
                    }));
                }
                MessageRole::System
                | MessageRole::User
                | MessageRole::Assistant
                | MessageRole::Tool => {}
            }
        }
        let instructions = instruction_parts.join("\n\n");
        let mut body = json!({
            "model": request.model,
            "input": input,
            "store": false,
            "stream": true,
        });
        if !instructions.is_empty() {
            body["instructions"] = json!(instructions);
        }
        // Codex only sends visible reasoning summaries when a summary mode is
        // requested.  Effort controls how much reasoning is done; summary
        // controls whether the provider exposes a trace on the stream.
        let mut reasoning = json!({ "summary": "auto" });
        if let Some(effort) = &request.reasoning_effort {
            reasoning["effort"] = json!(effort);
        }
        body["reasoning"] = reasoning;
        body["include"] = json!(["reasoning.encrypted_content"]);
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
        body
    }
}

fn codex_image_part(image: &crate::types::ImagePart) -> Value {
    let url = match &image.source {
        ImageSource::Url { url } => url.clone(),
        ImageSource::Base64 { media_type, data } => format!("data:{media_type};base64,{data}"),
    };
    json!({ "type": "input_image", "image_url": url })
}

struct CodexStreamState {
    usage: Usage,
    reason: StopReason,
    tool_calls: HashMap<String, (String, String, String)>,
}

impl Default for CodexStreamState {
    fn default() -> Self {
        Self {
            usage: Usage::default(),
            reason: StopReason::Stop,
            tool_calls: HashMap::new(),
        }
    }
}

impl CodexStreamState {
    fn apply_payload(&mut self, payload: &str) -> Result<Vec<ProviderEvent>, ProviderError> {
        if payload == "[DONE]" {
            return Ok(Vec::new());
        }
        let value: Value = serde_json::from_str(payload)
            .map_err(|error| ProviderError::Decode(error.to_string()))?;
        Ok(self.apply_event(&value))
    }

    fn apply_event(&mut self, value: &Value) -> Vec<ProviderEvent> {
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
                    let call_id = self
                        .tool_calls
                        .get(item_id)
                        .map(|(call_id, _, _)| call_id.clone())
                        .unwrap_or_else(|| item_id.to_string());
                    let entry = self
                        .tool_calls
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
                let item_type = item.get("type").and_then(Value::as_str).unwrap_or_default();
                if item_type == "reasoning" {
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
                if let Some(event) = web_search_event_from_item(item, false) {
                    events.push(event);
                    return events;
                }
                if item_type != "function_call" {
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
                self.tool_calls.insert(
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
                let Some((call_id, name, mut args)) = self.tool_calls.remove(item_id) else {
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
                        self.usage.input_tokens = usage_value
                            .get("input_tokens")
                            .and_then(Value::as_u64)
                            .unwrap_or(0) as u32;
                        self.usage.output_tokens = usage_value
                            .get("output_tokens")
                            .and_then(Value::as_u64)
                            .unwrap_or(0) as u32;
                    }
                    if response.get("status").and_then(Value::as_str) == Some("incomplete") {
                        self.reason = StopReason::MaxTokens;
                    }
                }
            }
            _ => {}
        }
        events
    }

    fn finish(self) -> Vec<ProviderEvent> {
        let mut events: Vec<ProviderEvent> = self
            .tool_calls
            .into_iter()
            .map(|(_, (id, name, args))| ProviderEvent::ToolCall { id, name, args })
            .collect();
        events.push(ProviderEvent::Usage { usage: self.usage });
        events.push(ProviderEvent::Done {
            reason: self.reason,
        });
        events
    }
}

#[async_trait]
impl Provider for CodexProvider {
    fn id(&self) -> &str {
        &self.id
    }

    fn capabilities(&self) -> ProviderCapabilities {
        ProviderCapabilities {
            web_search: Some(CODEX_WEB_SEARCH),
        }
    }

    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError> {
        let body = Self::body(&request);
        dump_provider_request(&self.id, &body);
        let mut builder = self
            .client
            .post("https://chatgpt.com/backend-api/codex/responses")
            .bearer_auth(&self.access_token)
            .header("originator", "firmius")
            .json(&body);
        if let Some(account_id) = &self.account_id {
            builder = builder.header("chatgpt-account-id", account_id);
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
            let mut state = CodexStreamState::default();
            while let Some(chunk) = bytes.next().await {
                let chunk = chunk.map_err(|error| ProviderError::Http(error.to_string()))?;
                buffer.push_str(&String::from_utf8_lossy(&chunk));
                for payload in parse_sse_lines(&mut buffer) {
                    for event in state.apply_payload(&payload)? {
                        yield event;
                    }
                }
            }
            for event in state.finish() {
                yield event;
            }
        };
        Ok(Box::pin(stream))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{ImagePart, Message, ToolDefinition, WebSearchAction, WebSearchRequest};

    fn base_request() -> ProviderRequest {
        ProviderRequest {
            model: "gpt-5.6-luna".into(),
            messages: vec![Message::text(MessageRole::User, "hello")],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: None,
            web_search: None,
        }
    }

    fn bash_tool() -> ToolDefinition {
        ToolDefinition {
            name: "bash".into(),
            description: "run a shell command".into(),
            input_schema: json!({
                "type": "object",
                "properties": { "command": { "type": "string" } },
            }),
        }
    }

    fn parse_sse_events(sse: &str) -> Vec<ProviderEvent> {
        let mut buffer = sse.to_string();
        if !buffer.ends_with('\n') {
            buffer.push('\n');
        }
        let mut state = CodexStreamState::default();
        let mut events = Vec::new();
        for payload in parse_sse_lines(&mut buffer) {
            events.extend(state.apply_payload(&payload).unwrap());
        }
        events
    }

    #[test]
    fn capabilities_advertise_cached_indexed_live_web_search() {
        let provider = CodexProvider::new("codex", "token", None);
        let search = provider
            .capabilities()
            .web_search
            .expect("codex advertises search");
        assert!(search.modes.contains(&WebSearchMode::Cached));
        assert!(search.modes.contains(&WebSearchMode::Indexed));
        assert!(search.modes.contains(&WebSearchMode::Live));
        assert_eq!(search.default_mode, WebSearchMode::Cached);
        assert_eq!(search.content, WebSearchContent::TextAndImage);
        assert!(!search.supports_filters);
        assert!(!search.supports_location);
    }

    #[test]
    fn body_omits_web_search_tool_when_request_has_none() {
        let mut request = base_request();
        request.tools = vec![bash_tool()];
        let body = CodexProvider::body(&request);
        let tools = body["tools"].as_array().unwrap();
        assert_eq!(tools.len(), 1);
        assert_eq!(tools[0]["type"], "function");
        assert_eq!(tools[0]["name"], "bash");
        assert!(tools.iter().all(|tool| tool["type"] != "web_search"));
    }

    #[test]
    fn body_appends_live_web_search_next_to_function_tools() {
        let mut request = base_request();
        request.tools = vec![bash_tool()];
        request.web_search = Some(WebSearchRequest {
            mode: WebSearchMode::Live,
        });
        let body = CodexProvider::body(&request);
        let tools = body["tools"].as_array().unwrap();
        assert_eq!(tools.len(), 2);
        assert_eq!(tools[0]["type"], "function");
        assert_eq!(tools[0]["name"], "bash");
        assert_eq!(tools[0]["description"], "run a shell command");
        assert!(tools[0].get("parameters").is_some());
        assert_eq!(tools[1]["type"], "web_search");
        assert_eq!(tools[1]["external_web_access"], true);
        assert!(tools[1].get("indexed_web_access").is_none());
        assert!(tools[1].get("name").is_none());
        assert!(tools[1].get("parameters").is_none());
    }

    #[test]
    fn body_maps_web_search_modes_to_access_flags() {
        for (mode, expected) in [
            (
                WebSearchMode::Cached,
                json!({"type": "web_search", "external_web_access": false}),
            ),
            (
                WebSearchMode::Indexed,
                json!({
                    "type": "web_search",
                    "external_web_access": true,
                    "indexed_web_access": true
                }),
            ),
            (
                WebSearchMode::Live,
                json!({"type": "web_search", "external_web_access": true}),
            ),
        ] {
            let mut request = base_request();
            request.web_search = Some(WebSearchRequest { mode });
            let tools = CodexProvider::body(&request)["tools"]
                .as_array()
                .unwrap()
                .clone();
            assert_eq!(tools, vec![expected]);
            assert!(
                tools[0].get("indexed_web_access").is_some() == (mode == WebSearchMode::Indexed)
            );
        }
    }

    #[test]
    fn replay_serializes_web_search_as_web_search_call_not_function_call() {
        let mut request = base_request();
        request.messages = vec![
            Message::text(MessageRole::User, "weather?"),
            Message::with_parts(
                MessageRole::Assistant,
                [
                    MessagePart::WebSearch {
                        id: "ws_1".into(),
                        action: WebSearchAction::Search {
                            query: Some("weather seattle".into()),
                            queries: None,
                        },
                    },
                    MessagePart::Text("It is raining.".into()),
                ],
            ),
        ];
        let input = CodexProvider::body(&request)["input"]
            .as_array()
            .unwrap()
            .clone();
        assert_eq!(input[0]["role"], "user");
        assert_eq!(input[1]["type"], "web_search_call");
        assert_eq!(input[1]["id"], "ws_1");
        assert_eq!(input[1]["status"], "completed");
        assert_eq!(input[1]["action"]["type"], "search");
        assert_eq!(input[1]["action"]["query"], "weather seattle");
        assert_ne!(input[1]["type"], "function_call");
        assert!(input.iter().all(|item| item["type"] != "function_call"));
        assert_eq!(input[2]["role"], "assistant");
        assert_eq!(input[2]["content"][0]["type"], "output_text");
    }

    #[test]
    fn sse_web_search_call_emits_started_then_finished_never_tool_call() {
        let sse = concat!(
            "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"ws_1\",\"type\":\"web_search_call\",\"status\":\"in_progress\"}}\n",
            "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"ws_1\",\"type\":\"web_search_call\",\"status\":\"completed\",\"action\":{\"type\":\"search\",\"query\":\"weather seattle\"}}}\n",
        );
        let events = parse_sse_events(sse);
        assert_eq!(
            events,
            vec![
                ProviderEvent::WebSearchStarted { id: "ws_1".into() },
                ProviderEvent::WebSearchFinished {
                    id: "ws_1".into(),
                    action: WebSearchAction::Search {
                        query: Some("weather seattle".into()),
                        queries: None,
                    },
                },
            ]
        );
        assert!(events.iter().all(|event| !matches!(
            event,
            ProviderEvent::ToolCall { .. } | ProviderEvent::ToolCallDelta { .. }
        )));
    }

    #[test]
    fn sse_web_search_call_without_action_finishes_as_other() {
        let sse = concat!(
            "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"ws_partial\",\"type\":\"web_search_call\"}}\n",
            "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"ws_partial\",\"type\":\"web_search_call\",\"status\":\"completed\"}}\n",
        );
        let events = parse_sse_events(sse);
        assert_eq!(
            events,
            vec![
                ProviderEvent::WebSearchStarted {
                    id: "ws_partial".into(),
                },
                ProviderEvent::WebSearchFinished {
                    id: "ws_partial".into(),
                    action: WebSearchAction::Other,
                },
            ]
        );
    }

    #[test]
    fn sse_function_call_path_is_unchanged() {
        let sse = concat!(
            "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"item_1\",\"type\":\"function_call\",\"call_id\":\"call-1\",\"name\":\"bash\",\"arguments\":\"\"}}\n",
            "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"item_1\",\"delta\":\"{\\\"command\\\":\\\"ls\\\"}\"}\n",
            "data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"item_1\",\"arguments\":\"{\\\"command\\\":\\\"ls\\\"}\"}\n",
        );
        let events = parse_sse_events(sse);
        assert_eq!(
            events,
            vec![
                ProviderEvent::ToolCallDelta {
                    index: 0,
                    id: Some("call-1".into()),
                    name_delta: "bash".into(),
                    args_delta: String::new(),
                },
                ProviderEvent::ToolCallDelta {
                    index: 0,
                    id: Some("call-1".into()),
                    name_delta: String::new(),
                    args_delta: "{\"command\":\"ls\"}".into(),
                },
                ProviderEvent::ToolCall {
                    id: "call-1".into(),
                    name: "bash".into(),
                    args: "{\"command\":\"ls\"}".into(),
                },
            ]
        );
        assert!(events.iter().all(|event| !matches!(
            event,
            ProviderEvent::WebSearchStarted { .. } | ProviderEvent::WebSearchFinished { .. }
        )));
    }

    #[test]
    fn tool_result_body_uses_responses_function_call_items_without_empty_assistant_text() {
        let request = ProviderRequest {
            model: "gpt-5.6-luna".into(),
            messages: vec![
                Message::text(MessageRole::User, "call the tool"),
                Message {
                    role: MessageRole::Assistant,
                    content: vec![MessagePart::ToolCall {
                        id: "call-1".into(),
                        name: "audit_echo".into(),
                        args: "{\"text\":\"ok\"}".into(),
                    }],
                },
                Message::tool_results([MessagePart::ToolResult {
                    id: "call-1".into(),
                    content: "ok".into(),
                    ok: true,
                }]),
            ],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: None,
            web_search: None,
        };
        let input = CodexProvider::body(&request)["input"]
            .as_array()
            .unwrap()
            .clone();
        assert_eq!(input.len(), 3);
        assert_eq!(input[1]["type"], "function_call");
        assert_eq!(input[1]["call_id"], "call-1");
        assert_eq!(input[2]["type"], "function_call_output");
        assert_eq!(input[2]["output"], "ok");
    }

    #[test]
    fn assistant_history_uses_output_text_not_input_text() {
        let request = ProviderRequest {
            model: "gpt-5.6-luna".into(),
            messages: vec![
                Message::text(MessageRole::User, "Hai!"),
                Message::text(MessageRole::Assistant, "Hello!"),
                Message::text(MessageRole::User, "WTF!"),
            ],
            tools: Vec::new(),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: None,
            web_search: None,
        };
        let input = CodexProvider::body(&request)["input"]
            .as_array()
            .unwrap()
            .clone();
        assert_eq!(input[0]["content"][0]["type"], "input_text");
        assert_eq!(input[1]["content"][0]["type"], "output_text");
        assert_eq!(input[2]["content"][0]["type"], "input_text");
    }

    #[test]
    fn user_image_history_uses_input_image_items() {
        let request = ProviderRequest {
            model: "gpt-5.6-luna".into(),
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
            session_id: None,
            web_search: None,
        };
        let content = CodexProvider::body(&request)["input"][0]["content"]
            .as_array()
            .unwrap()
            .clone();
        assert_eq!(content[0]["type"], "input_text");
        assert_eq!(
            content[1],
            json!({
                "type": "input_image",
                "image_url": "data:image/png;base64,Zm9v"
            })
        );
    }
}
