mod anthropic;
mod openai;

pub mod manager;
pub mod schema;

pub use anthropic::AnthropicProvider;
pub use openai::OpenAiProvider;
pub use schema::ProviderSchema;

use crate::types::{ProviderRequest, StopReason, Usage};
use async_trait::async_trait;
use futures::stream::BoxStream;

/// A normalized, provider-agnostic streaming event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProviderEvent {
    TextDelta { delta: String },
    ThinkingDelta { delta: String, signature: Option<String> },
    ToolCallDelta { index: u32, id: Option<String>, name_delta: String, args_delta: String },
    ToolCall { id: String, name: String, args: String },
    Usage { usage: Usage },
    Done { reason: StopReason },
}

#[derive(Debug, thiserror::Error)]
pub enum ProviderError {
    #[error("http error: {0}")] Http(String),
    #[error("api error {status}: {body}")] Api { status: u16, body: String },
    #[error("decode error: {0}")] Decode(String),
}

/// A backend that turns a request into a stream of normalized events.
#[async_trait]
pub trait Provider: Send + Sync {
    fn id(&self) -> &str;
    async fn stream(&self, request: ProviderRequest)
        -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError>;
}

/// Shared SSE line iterator: splits a chunked byte stream into `data:` payloads.
pub(crate) fn parse_sse_lines(buffer: &mut String) -> Vec<String> {
    let mut payloads = Vec::new();
    while let Some(pos) = buffer.find('\n') {
        let line = buffer[..pos].trim_end_matches('\r').to_string();
        buffer.replace_range(..=pos, "");
        if let Some(rest) = line.strip_prefix("data:") {
            payloads.push(rest.trim().to_string());
        }
    }
    payloads
}
