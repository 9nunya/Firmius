use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum MessageRole {
    User,
    Assistant,
    System,
    Tool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum MessagePart {
    Text(String),
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
    pub fn tool_results(results: impl IntoIterator<Item = MessagePart>) -> Self {
        Self {
            role: MessageRole::Tool,
            content: results.into_iter().collect(),
        }
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
