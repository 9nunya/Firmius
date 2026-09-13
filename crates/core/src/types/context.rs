use super::{ModelCapabilities, ModelCapability, ModelInfo, WebSearchAction, WebSearchRequest};
use serde::{Deserialize, Deserializer, Serialize, Serializer};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum MessageRole {
    #[default]
    User,
    Assistant,
    System,
    Tool,
}

/// Where model-visible message content came from and what authority it has.
///
/// This is deliberately metadata rather than a provider role. In particular,
/// model-authored summaries, peer messages, and workflow assignments remain
/// user-level context even when Firmius injects them into a request.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct MessageProvenance {
    pub origin: MessageOrigin,
    pub trust: MessageTrust,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MessageOrigin {
    PromptStack,
    Human,
    Assistant,
    Tool,
    Peer,
    Assignment,
    Compaction,
    Legacy,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MessageTrust {
    SystemAuthority,
    UserProvided,
    DerivedUntrusted,
}

impl MessageProvenance {
    pub const fn new(origin: MessageOrigin, trust: MessageTrust) -> Self {
        Self { origin, trust }
    }

    pub const fn prompt_stack() -> Self {
        Self::new(MessageOrigin::PromptStack, MessageTrust::SystemAuthority)
    }

    pub const fn compaction() -> Self {
        Self::new(MessageOrigin::Compaction, MessageTrust::DerivedUntrusted)
    }
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

#[derive(Debug, Clone, PartialEq, Eq)]
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
    /// Hosted web-search action owned by the assistant. Not a ToolCall:
    /// validate_context / repair_dangling_tool_calls ignore it, and it must
    /// never enter ToolRegistry.
    WebSearch {
        id: String,
        action: WebSearchAction,
    },
}

/// Internally tagged on-disk / on-wire form. New records always serialize this
/// way so `WebSearch` is distinguishable from a `ToolCall`.
#[derive(Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum TaggedMessagePart {
    Text {
        text: String,
    },
    Image {
        source: crate::types::ImageSource,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        detail: Option<ImageDetail>,
    },
    Thinking {
        content: String,
        #[serde(default)]
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
    WebSearch {
        id: String,
        action: WebSearchAction,
    },
}

/// Pre-capability externally tagged form (`{"Text":"..."}`, `{"ToolCall":{...}}`).
#[derive(Deserialize)]
enum LegacyMessagePart {
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

impl From<TaggedMessagePart> for MessagePart {
    fn from(value: TaggedMessagePart) -> Self {
        match value {
            TaggedMessagePart::Text { text } => Self::Text(text),
            TaggedMessagePart::Image { source, detail } => {
                Self::Image(ImagePart { source, detail })
            }
            TaggedMessagePart::Thinking { content, signature } => {
                Self::Thinking { content, signature }
            }
            TaggedMessagePart::ToolCall { id, name, args } => Self::ToolCall { id, name, args },
            TaggedMessagePart::ToolResult { id, content, ok } => {
                Self::ToolResult { id, content, ok }
            }
            TaggedMessagePart::WebSearch { id, action } => Self::WebSearch { id, action },
        }
    }
}

impl From<&MessagePart> for TaggedMessagePart {
    fn from(value: &MessagePart) -> Self {
        match value {
            MessagePart::Text(text) => Self::Text { text: text.clone() },
            MessagePart::Image(image) => Self::Image {
                source: image.source.clone(),
                detail: image.detail.clone(),
            },
            MessagePart::Thinking { content, signature } => Self::Thinking {
                content: content.clone(),
                signature: signature.clone(),
            },
            MessagePart::ToolCall { id, name, args } => Self::ToolCall {
                id: id.clone(),
                name: name.clone(),
                args: args.clone(),
            },
            MessagePart::ToolResult { id, content, ok } => Self::ToolResult {
                id: id.clone(),
                content: content.clone(),
                ok: *ok,
            },
            MessagePart::WebSearch { id, action } => Self::WebSearch {
                id: id.clone(),
                action: action.clone(),
            },
        }
    }
}

impl From<LegacyMessagePart> for MessagePart {
    fn from(value: LegacyMessagePart) -> Self {
        match value {
            LegacyMessagePart::Text(text) => Self::Text(text),
            LegacyMessagePart::Image(image) => Self::Image(image),
            LegacyMessagePart::Thinking { content, signature } => {
                Self::Thinking { content, signature }
            }
            LegacyMessagePart::ToolCall { id, name, args } => Self::ToolCall { id, name, args },
            LegacyMessagePart::ToolResult { id, content, ok } => {
                Self::ToolResult { id, content, ok }
            }
        }
    }
}

impl Serialize for MessagePart {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        TaggedMessagePart::from(self).serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for MessagePart {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum MessagePartDe {
            Tagged(TaggedMessagePart),
            Legacy(LegacyMessagePart),
        }
        match MessagePartDe::deserialize(deserializer)? {
            MessagePartDe::Tagged(part) => Ok(part.into()),
            MessagePartDe::Legacy(part) => Ok(part.into()),
        }
    }
}

/// Optional correlation envelope for a message. Empty by default so legacy
/// `{role, content}` JSON still deserializes and uncorrelated messages still
/// serialize that way. Identity (`message_id`) and per-thread ordering
/// (`thread_id`/`thread_seq`) are assigned at durable delivery when present.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct MessageCorrelation {
    /// Durable sender identity for an inter-agent delivery. Optional so old
    /// `{role, content}` records remain wire-compatible.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sender_id: Option<String>,
    /// Stable identity assigned at first durable append. Reused on resend.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub message_id: Option<String>,
    /// Conversation thread this message belongs to.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thread_id: Option<String>,
    /// Monotonic sequence within `thread_id`, assigned on first durable store.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thread_seq: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub goal_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub run_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_goal_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workflow_node_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub assignment_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub in_reply_to: Option<String>,
}

impl MessageCorrelation {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }

    /// True when this envelope names a goal that should stay isolated from
    /// a recipient currently executing a different goal.
    pub fn is_goal_scoped(&self) -> bool {
        self.goal_id.is_some()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct Message {
    pub role: MessageRole,
    pub content: Vec<MessagePart>,
    /// Optional correlation envelope. Flattened so legacy `{role, content}`
    /// JSON still loads, and uncorrelated messages still serialize that way.
    #[serde(flatten, default, skip_serializing_if = "MessageCorrelation::is_empty")]
    pub correlation: MessageCorrelation,
    /// Explicit origin/trust metadata for generated context. Omitted for old
    /// records and ordinary messages to preserve the legacy JSON shape.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub provenance: Option<MessageProvenance>,
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

    pub fn saturating_add(self, other: Self) -> Self {
        Self {
            input_tokens: self.input_tokens.saturating_add(other.input_tokens),
            output_tokens: self.output_tokens.saturating_add(other.output_tokens),
            cache_read_tokens: self
                .cache_read_tokens
                .saturating_add(other.cache_read_tokens),
            cache_write_tokens: self
                .cache_write_tokens
                .saturating_add(other.cache_write_tokens),
        }
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
    /// Stable session id, used by providers with server-side conversation
    /// caching (e.g. xAI's `prompt_cache_key`). `None` for one-shot calls
    /// like compaction.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    /// Hosted web-search request. `None` (the default) means do not advertise
    /// search to the provider, even if the provider capability is present.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub web_search: Option<WebSearchRequest>,
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
            correlation: MessageCorrelation::default(),
            provenance: None,
        }
    }

    pub fn with_parts(role: MessageRole, parts: impl IntoIterator<Item = MessagePart>) -> Self {
        Self {
            role,
            content: parts.into_iter().collect(),
            correlation: MessageCorrelation::default(),
            provenance: None,
        }
    }

    pub fn tool_results(results: impl IntoIterator<Item = MessagePart>) -> Self {
        Self {
            role: MessageRole::Tool,
            content: results.into_iter().collect(),
            correlation: MessageCorrelation::default(),
            provenance: None,
        }
    }

    pub fn with_correlation(mut self, correlation: MessageCorrelation) -> Self {
        self.correlation = correlation;
        self
    }

    pub fn with_provenance(mut self, provenance: MessageProvenance) -> Self {
        self.provenance = Some(provenance);
        self
    }

    /// Return explicit provenance when present, otherwise conservatively
    /// infer it from durable correlation and provider role. This gives legacy
    /// peer/assignment records useful provenance without rewriting them.
    pub fn effective_provenance(&self) -> MessageProvenance {
        if let Some(provenance) = self.provenance {
            return provenance;
        }
        if self.correlation.assignment_id.is_some() || self.correlation.workflow_node_id.is_some() {
            return MessageProvenance::new(
                MessageOrigin::Assignment,
                MessageTrust::DerivedUntrusted,
            );
        }
        if !self.correlation.is_empty() {
            return MessageProvenance::new(MessageOrigin::Peer, MessageTrust::DerivedUntrusted);
        }
        match self.role {
            MessageRole::System => {
                MessageProvenance::new(MessageOrigin::Legacy, MessageTrust::SystemAuthority)
            }
            MessageRole::User => {
                MessageProvenance::new(MessageOrigin::Human, MessageTrust::UserProvided)
            }
            MessageRole::Assistant => {
                MessageProvenance::new(MessageOrigin::Assistant, MessageTrust::DerivedUntrusted)
            }
            MessageRole::Tool => {
                MessageProvenance::new(MessageOrigin::Tool, MessageTrust::DerivedUntrusted)
            }
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
                    MessagePart::Text(_) | MessagePart::WebSearch { .. } => {}
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

    #[test]
    fn usage_saturating_add_accumulates_each_counter() {
        let left = Usage {
            input_tokens: 10,
            output_tokens: 20,
            cache_read_tokens: 30,
            cache_write_tokens: 40,
        };
        let right = Usage {
            input_tokens: 1,
            output_tokens: 2,
            cache_read_tokens: 3,
            cache_write_tokens: 4,
        };
        assert_eq!(left.saturating_add(right).total(), 110);
    }

    fn call(id: &str) -> Message {
        Message {
            role: MessageRole::Assistant,
            content: vec![MessagePart::ToolCall {
                id: id.into(),
                name: "bash".into(),
                args: "{}".into(),
            }],
            correlation: MessageCorrelation::default(),
            provenance: None,
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
            session_id: None,
            web_search: None,
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
                ..Default::default()
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

    fn web_search(id: &str) -> MessagePart {
        MessagePart::WebSearch {
            id: id.into(),
            action: WebSearchAction::Search {
                query: Some("rust".into()),
                queries: None,
            },
        }
    }

    #[test]
    fn validate_context_accepts_assistant_web_search_then_text() {
        let ctx = vec![
            Message::text(MessageRole::User, "search rust"),
            Message::with_parts(
                MessageRole::Assistant,
                [
                    web_search("ws-1"),
                    MessagePart::Text("here is what I found".into()),
                ],
            ),
        ];
        assert!(validate_context(&ctx).is_ok());
    }

    #[test]
    fn repair_dangling_tool_calls_leaves_web_search_alone() {
        let mut ctx = vec![
            Message::text(MessageRole::User, "search rust"),
            Message::with_parts(
                MessageRole::Assistant,
                [web_search("ws-1"), MessagePart::Text("done".into())],
            ),
        ];
        let before = ctx.clone();
        assert_eq!(repair_dangling_tool_calls(&mut ctx), 0);
        assert_eq!(ctx, before);
        assert!(validate_context(&ctx).is_ok());
    }

    #[test]
    fn repair_does_not_treat_web_search_as_a_tool_call() {
        // A mixed assistant turn: hosted search plus a real tool call that
        // was interrupted. Repair must synthesize a result only for the
        // ToolCall, never for the WebSearch id.
        let mut ctx = vec![
            Message::text(MessageRole::User, "hi"),
            Message::with_parts(
                MessageRole::Assistant,
                [
                    web_search("ws-1"),
                    MessagePart::ToolCall {
                        id: "c1".into(),
                        name: "bash".into(),
                        args: "{}".into(),
                    },
                ],
            ),
        ];
        assert_eq!(repair_dangling_tool_calls(&mut ctx), 1);
        assert_eq!(ctx.len(), 3);
        assert!(matches!(
            &ctx[1].content[0],
            MessagePart::WebSearch { id, .. } if id == "ws-1"
        ));
        assert!(matches!(
            &ctx[2].content[0],
            MessagePart::ToolResult { id, ok, .. } if id == "c1" && !ok
        ));
        assert!(validate_context(&ctx).is_ok());
    }

    #[test]
    fn old_session_json_without_web_search_still_deserializes() {
        // Pre-change records were externally tagged: {"Text":"..."},
        // {"ToolCall":{...}}, {"ToolResult":{...}}. Adding WebSearch as a
        // tagged variant must not break those records.
        let json = r#"{
            "role": "Assistant",
            "content": [
                {"Text": "hello"},
                {"ToolCall": {"id": "c1", "name": "bash", "args": "{}"}},
                {"ToolResult": {"id": "c1", "content": "ok", "ok": true}}
            ]
        }"#;
        let msg: Message = serde_json::from_str(json).unwrap();
        assert_eq!(msg.role, MessageRole::Assistant);
        assert!(matches!(msg.content[0], MessagePart::Text(ref t) if t == "hello"));
        assert!(matches!(
            msg.content[1],
            MessagePart::ToolCall { ref id, ref name, .. } if id == "c1" && name == "bash"
        ));
        assert!(matches!(
            msg.content[2],
            MessagePart::ToolResult { ref id, ok, .. } if id == "c1" && ok
        ));
    }

    #[test]
    fn new_web_search_parts_round_trip() {
        let msg = Message::with_parts(
            MessageRole::Assistant,
            [
                web_search("ws-1"),
                MessagePart::Text("here is what I found".into()),
            ],
        );
        let value = serde_json::to_value(&msg).unwrap();
        assert_eq!(value["content"][0]["type"], "web_search");
        assert_eq!(value["content"][0]["id"], "ws-1");
        assert_eq!(value["content"][0]["action"]["type"], "search");
        assert_eq!(value["content"][1]["type"], "text");
        let restored: Message = serde_json::from_value(value).unwrap();
        assert_eq!(restored, msg);
    }

    #[test]
    fn provider_request_web_search_defaults_to_none() {
        let value = serde_json::json!({
            "model": "m",
            "messages": [],
            "tools": [],
            "temperature": null,
            "max_tokens": null,
            "reasoning_effort": null,
            "thinking_budget_tokens": null
        });
        let request: ProviderRequest = serde_json::from_value(value).unwrap();
        assert!(request.web_search.is_none());
        assert!(request.session_id.is_none());
    }

    #[test]
    fn legacy_message_json_round_trips_without_correlation_fields() {
        let msg = Message::text(MessageRole::User, "hello");
        let value = serde_json::to_value(&msg).unwrap();
        assert_eq!(value["role"], "User");
        assert!(value.get("message_id").is_none());
        assert!(value.get("thread_id").is_none());
        assert!(value.get("goal_id").is_none());
        let restored: Message = serde_json::from_value(value).unwrap();
        assert_eq!(restored, msg);
        assert!(restored.correlation.is_empty());

        let legacy = r#"{"role":"User","content":[{"type":"text","text":"hello"}]}"#;
        let from_legacy: Message = serde_json::from_str(legacy).unwrap();
        assert_eq!(from_legacy.content, msg.content);
        assert!(from_legacy.correlation.is_empty());
    }

    #[test]
    fn correlated_message_round_trips_stable_ids() {
        let msg =
            Message::text(MessageRole::User, "progress").with_correlation(MessageCorrelation {
                sender_id: Some("agent-a".into()),
                message_id: Some("msg-1".into()),
                thread_id: Some("thread-a".into()),
                thread_seq: Some(3),
                goal_id: Some("goal-9".into()),
                run_id: Some("run-2".into()),
                parent_goal_id: Some("goal-1".into()),
                workflow_node_id: Some("node-4".into()),
                assignment_id: Some("asg-7".into()),
                in_reply_to: Some("msg-0".into()),
            });
        let value = serde_json::to_value(&msg).unwrap();
        assert_eq!(value["message_id"], "msg-1");
        assert_eq!(value["thread_id"], "thread-a");
        assert_eq!(value["thread_seq"], 3);
        assert_eq!(value["goal_id"], "goal-9");
        assert_eq!(value["in_reply_to"], "msg-0");
        let restored: Message = serde_json::from_value(value).unwrap();
        assert_eq!(restored, msg);
    }

    #[test]
    fn provenance_is_optional_and_legacy_correlation_is_inferred() {
        let legacy: Message = serde_json::from_str(
            r#"{"role":"User","content":[{"type":"text","text":"assigned"}],"assignment_id":"asg-1"}"#,
        )
        .unwrap();
        assert!(legacy.provenance.is_none());
        assert_eq!(
            legacy.effective_provenance(),
            MessageProvenance::new(MessageOrigin::Assignment, MessageTrust::DerivedUntrusted)
        );

        let summary = Message::text(MessageRole::User, "summary")
            .with_provenance(MessageProvenance::compaction());
        let value = serde_json::to_value(&summary).unwrap();
        assert_eq!(value["provenance"]["origin"], "compaction");
        assert_eq!(value["provenance"]["trust"], "derived_untrusted");
        assert_eq!(serde_json::from_value::<Message>(value).unwrap(), summary);
    }
}
