use super::{Provider, ProviderError, ProviderEvent, dump_provider_request, parse_sse_lines};
use crate::types::{ImageSource, MessagePart, MessageRole, ProviderRequest, StopReason, Usage};
use async_trait::async_trait;
use futures::{StreamExt, stream::BoxStream};
use serde_json::{Value, json};
use std::collections::HashMap;

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
        if let Some(effort) = &request.reasoning_effort {
            body["reasoning"] = json!({ "effort": effort });
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

#[async_trait]
impl Provider for CodexProvider {
    fn id(&self) -> &str {
        &self.id
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
                    match value.get("type").and_then(Value::as_str).unwrap_or_default() {
                        "response.output_text.delta" => {
                            if let Some(delta) = value.get("delta").and_then(Value::as_str) {
                                yield ProviderEvent::TextDelta { delta: delta.to_string() };
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
                                yield ProviderEvent::ToolCallDelta {
                                    index: 0,
                                    id: Some(call_id),
                                    name_delta: String::new(),
                                    args_delta: delta.to_string(),
                                };
                            }
                        }
                        "response.output_item.added" => {
                            let Some(item) = value.get("item") else { continue };
                            if item.get("type").and_then(Value::as_str) != Some("function_call") {
                                continue;
                            }
                            let Some(item_id) = item.get("id").and_then(Value::as_str) else { continue };
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
                            tool_calls.insert(item_id.to_string(), (call_id.clone(), name.clone(), args.clone()));
                            yield ProviderEvent::ToolCallDelta {
                                index: 0,
                                id: Some(call_id),
                                name_delta: name,
                                args_delta: args,
                            };
                        }
                        "response.function_call_arguments.done" => {
                            let Some(item_id) = value.get("item_id").and_then(Value::as_str) else { continue };
                            let Some((call_id, name, mut args)) = tool_calls.remove(item_id) else { continue };
                            if let Some(final_args) = value.get("arguments").and_then(Value::as_str) {
                                args = final_args.to_string();
                            }
                            yield ProviderEvent::ToolCall { id: call_id, name, args };
                        }
                        "response.completed" => if let Some(response) = value.get("response") {
                            if let Some(usage_value) = response.get("usage") {
                                usage.input_tokens = usage_value.get("input_tokens").and_then(Value::as_u64).unwrap_or(0) as u32;
                                usage.output_tokens = usage_value.get("output_tokens").and_then(Value::as_u64).unwrap_or(0) as u32;
                            }
                            if response.get("status").and_then(Value::as_str) == Some("incomplete") {
                                reason = StopReason::MaxTokens;
                            }
                        },
                        _ => {}
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
    use crate::types::{ImagePart, Message};

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
