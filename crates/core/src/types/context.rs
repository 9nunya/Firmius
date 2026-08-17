use super::{ModelCapabilities, ModelCapability, ModelInfo};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum MessageRole {
    User,
    Assistant,
    System,
    Tool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ImageDetail {
    Low,
    High,
    Auto,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ImageSource {
    Base64 { media_type: String, data: String },
    Url { url: String },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ImagePart {
    pub source: ImageSource,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub detail: Option<ImageDetail>,
}

impl ImagePart {
    pub fn from_base64(media_type: impl Into<String>, data: impl Into<String>) -> Self {
        Self {
            source: ImageSource::Base64 {
                media_type: media_type.into(),
                data: data.into(),
            },
            detail: None,
        }
    }

    pub fn from_url(url: impl Into<String>) -> Self {
        Self {
            source: ImageSource::Url { url: url.into() },
            detail: None,
        }
    }

    pub fn with_detail(mut self, detail: ImageDetail) -> Self {
        self.detail = Some(detail);
        self
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum MessagePart {
    Text(String),
    Image(ImagePart),
    Thinking {
        content: String,
        signature: Option<String>,
    },
    ToolCall {
        id: String,
        name: String,
        args: String,
    },
    ToolResult {
        id: String,
        content: String,
        ok: bool,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Message {
    pub role: MessageRole,
    pub content: Vec<MessagePart>,
}

pub type Context = Vec<Message>;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum StopReason {
    Stop,
    ToolUse,
    MaxTokens,
    Error,
    Cancelled,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct Usage {
    pub input_tokens: u32,
    pub output_tokens: u32,
    pub cache_read_tokens: u32,
    pub cache_write_tokens: u32,
}

impl Usage {
    pub fn total(&self) -> u32 {
        self.input_tokens + self.output_tokens + self.cache_read_tokens + self.cache_write_tokens
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ToolDefinition {
    pub name: String,
    pub description: String,
    pub input_schema: serde_json::Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProviderRequest {
    pub model: String,
    pub messages: Context,
    pub tools: Vec<ToolDefinition>,
    pub temperature: Option<f32>,
    pub max_tokens: Option<u32>,
    /// OpenAI-style: "low", "medium", "high", "xhigh".
    pub reasoning_effort: Option<String>,
    /// Anthropic-style: max tokens to spend on thinking.
    pub thinking_budget_tokens: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ToolCall {
    pub id: String,
    pub name: String,
    pub args: String,
}

impl Message {
    pub fn text(role: MessageRole, text: impl Into<String>) -> Self {
        Self {
            role,
            content: vec![MessagePart::Text(text.into())],
        }
    }

    pub fn with_parts(role: MessageRole, parts: impl IntoIterator<Item = MessagePart>) -> Self {
        Self {
            role,
            content: parts.into_iter().collect(),
        }
    }

    pub fn tool_results(results: impl IntoIterator<Item = MessagePart>) -> Self {
        Self {
            role: MessageRole::Tool,
            content: results.into_iter().collect(),
        }
    }
}

impl ProviderRequest {
    pub fn required_capabilities(&self) -> ModelCapabilities {
        let mut capabilities = ModelCapabilities::text();
        for message in &self.messages {
            for part in &message.content {
                match part {
                    MessagePart::Image(_) => capabilities.insert(ModelCapability::Image),
                    MessagePart::ToolCall { .. } | MessagePart::ToolResult { .. } => {
                        capabilities.insert(ModelCapability::ToolUse)
                    }
                    MessagePart::Thinking { .. } => capabilities.insert(ModelCapability::Reasoning),
                    MessagePart::Text(_) => {}
                }
            }
        }
        if !self.tools.is_empty() {
            capabilities.insert(ModelCapability::ToolUse);
        }
        if self.reasoning_effort.is_some() || self.thinking_budget_tokens.is_some() {
            capabilities.insert(ModelCapability::Reasoning);
        }
        capabilities
    }

    pub fn is_compatible_with(&self, model: &ModelInfo) -> bool {
        model
            .capabilities
            .supports_all(&self.required_capabilities())
    }
}

pub fn validate_context(context: &Context) -> Result<(), String> {
    for (i, message) in context.iter().enumerate() {
        if message.role != MessageRole::Tool {
            continue;
        }
        let Some(previous) = i.checked_sub(1).and_then(|n| context.get(n)) else {
            return Err("orphan tool message".into());
        };
        let calls: Vec<&str> = previous
            .content
            .iter()
            .filter_map(|part| {
                if let MessagePart::ToolCall { id, .. } = part {
                    Some(id.as_str())
                } else {
                    None
                }
            })
            .collect();
        for part in &message.content {
            if let MessagePart::ToolResult { id, .. } = part
                && !calls.contains(&id.as_str())
            {
                return Err(format!("tool result {id} has no preceding call"));
            }
        }
    }
    Ok(())
}

/// Append synthetic results for tool calls that never received one — the
/// shape a trajectory takes when its turn is interrupted mid-execution
/// (cancellation, crash, killed process). Provider APIs reject contexts
/// where a tool call has no matching result, so a dangling call makes its
/// agent unpromptable until repaired. Returns the number of synthetic
/// results added.
pub fn repair_dangling_tool_calls(history: &mut Context) -> usize {
    let mut added = 0;
    let mut i = 0;
    while i < history.len() {
        let calls: Vec<String> = history[i]
            .content
            .iter()
            .filter_map(|p| match p {
                MessagePart::ToolCall { id, .. } => Some(id.clone()),
                _ => None,
            })
            .collect();
        if calls.is_empty() {
            i += 1;
            continue;
        }
        // Ids already answered by the immediately following tool message.
        let next_is_tool = matches!(history.get(i + 1), Some(m) if m.role == MessageRole::Tool);
        let resolved: Vec<String> = if next_is_tool {
            history[i + 1]
                .content
                .iter()
                .filter_map(|p| match p {
                    MessagePart::ToolResult { id, .. } => Some(id.clone()),
                    _ => None,
                })
                .collect()
        } else {
            Vec::new()
        };
        let missing: Vec<MessagePart> = calls
            .into_iter()
            .filter(|id| !resolved.contains(id))
            .map(|id| MessagePart::ToolResult {
                id,
                content: "interrupted before completion".to_string(),
                ok: false,
            })
            .collect();
        if !missing.is_empty() {
            added += missing.len();
            if next_is_tool {
                // Partially-answered calls: append to the existing tool
                // message so call/result adjacency stays intact.
                history[i + 1].content.extend(missing);
            } else {
                history.insert(i + 1, Message::tool_results(missing));
            }
        }
        // Step past the call message and its (real or synthetic) results.
        i += 2;
    }
    added
}

#[cfg(test)]
mod tests {
    use super::*;

    fn call(id: &str) -> Message {
        Message {
            role: MessageRole::Assistant,
            content: vec![MessagePart::ToolCall {
                id: id.into(),
                name: "bash".into(),
                args: "{}".into(),
            }],
        }
    }

    fn result(id: &str) -> MessagePart {
        MessagePart::ToolResult {
            id: id.into(),
            content: "ok".into(),
            ok: true,
        }
    }

    #[test]
    fn request_infers_image_and_tool_capabilities() {
        let request = ProviderRequest {
            model: "test-model".into(),
            messages: vec![Message::with_parts(
                MessageRole::User,
                [
                    MessagePart::Text("describe".into()),
                    MessagePart::Image(ImagePart::from_url("https://example.test/cat.png")),
                ],
            )],
            tools: vec![ToolDefinition {
                name: "read".into(),
                description: "Read a file".into(),
                input_schema: serde_json::json!({"type": "object"}),
            }],
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
        };
        let required = request.required_capabilities();
        assert!(required.supports(ModelCapability::Text));
        assert!(required.supports(ModelCapability::Image));
        assert!(required.supports(ModelCapability::ToolUse));
    }

    #[test]
    fn repairs_dangling_call_at_end() {
        let mut ctx = vec![
            Message::text(MessageRole::System, "sys"),
            Message::text(MessageRole::User, "hi"),
            call("c1"),
        ];
        assert_eq!(repair_dangling_tool_calls(&mut ctx), 1);
        assert_eq!(ctx.len(), 4);
        assert!(validate_context(&ctx).is_ok());
        let last = &ctx[3].content[0];
        assert!(matches!(
            last,
            MessagePart::ToolResult { id, ok, .. } if id == "c1" && !ok
        ));
    }

    #[test]
    fn repairs_missing_result_in_partial_pair() {
        // Two calls, only one answered.
        let mut ctx = vec![
            Message::text(MessageRole::User, "hi"),
            Message {
                role: MessageRole::Assistant,
                content: vec![
                    MessagePart::ToolCall {
                        id: "c1".into(),
                        name: "bash".into(),
                        args: "{}".into(),
                    },
                    MessagePart::ToolCall {
                        id: "c2".into(),
                        name: "read".into(),
                        args: "{}".into(),
                    },
                ],
            },
            Message::tool_results([result("c1")]),
            Message::text(MessageRole::Assistant, "after"),
        ];
        assert_eq!(repair_dangling_tool_calls(&mut ctx), 1);
        assert_eq!(ctx.len(), 4);
        assert!(validate_context(&ctx).is_ok());
        // The synthetic result is appended INTO the existing tool message.
        assert!(matches!(
            ctx[2].content[1],
            MessagePart::ToolResult { ref id, ok, .. } if id == "c2" && !ok
        ));
    }

    #[test]
    fn leaves_clean_history_untouched() {
        let mut ctx = vec![
            Message::text(MessageRole::User, "hi"),
            call("c1"),
            Message::tool_results([result("c1")]),
            Message::text(MessageRole::Assistant, "done"),
        ];
        let before = ctx.clone();
        assert_eq!(repair_dangling_tool_calls(&mut ctx), 0);
        assert_eq!(ctx, before);
    }

    #[test]
    fn repairs_multiple_interruptions() {
        let mut ctx = vec![
            Message::text(MessageRole::User, "one"),
            call("c1"),
            Message::text(MessageRole::User, "two"),
            call("c2"),
        ];
        assert_eq!(repair_dangling_tool_calls(&mut ctx), 2);
        assert!(validate_context(&ctx).is_ok());
    }
}
