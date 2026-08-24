mod anthropic;
mod codex;
mod grok;
mod openai;
mod responses_web_search;

pub mod manager;
pub mod schema;

pub use anthropic::AnthropicProvider;
pub use codex::CodexProvider;
pub use grok::GrokProvider;
pub use openai::OpenAiProvider;
pub(crate) use openai::append_openai_messages;
pub use schema::ProviderSchema;

use crate::types::{ProviderCapabilities, ProviderRequest, StopReason, Usage, WebSearchAction};
use async_trait::async_trait;
use futures::stream::BoxStream;
use serde_json::Value;
use std::fs::OpenOptions;
use std::io::Write;

/// A normalized, provider-agnostic streaming event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProviderEvent {
    TextDelta {
        delta: String,
    },
    ThinkingDelta {
        delta: String,
        signature: Option<String>,
    },
    ToolCallDelta {
        index: u32,
        id: Option<String>,
        name_delta: String,
        args_delta: String,
    },
    ToolCall {
        id: String,
        name: String,
        args: String,
    },
    Usage {
        usage: Usage,
    },
    Done {
        reason: StopReason,
    },
    /// Hosted web search began. Never a ToolCall — the agent must not execute it.
    WebSearchStarted {
        id: String,
    },
    /// Hosted web search completed. The agent records this as MessagePart::WebSearch.
    WebSearchFinished {
        id: String,
        action: WebSearchAction,
    },
}

#[derive(Debug, Clone, thiserror::Error)]
pub enum ProviderError {
    #[error("http error: {0}")]
    Http(String),
    #[error("api error {status}: {body}")]
    Api { status: u16, body: String },
    #[error("decode error: {0}")]
    Decode(String),
    /// Authentication failed before or without reaching the API — missing,
    /// malformed, or rejected credentials. OAuth kinds will grow this into a
    /// `NeedsReauth` variant that carries the provider id back to the UI.
    #[error("auth error: {0}")]
    Auth(String),
}

/// A backend that turns a request into a stream of normalized events.
#[async_trait]
pub trait Provider: Send + Sync {
    fn id(&self) -> &str;
    /// Hosted capabilities this provider can actually replay. Default is none,
    /// so existing providers and test doubles stay compiling and do not
    /// advertise web search.
    fn capabilities(&self) -> ProviderCapabilities {
        ProviderCapabilities::none()
    }
    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError>;
}

// ---------------------------------------------------------------------------
// Token suppliers — the auth seam
// ---------------------------------------------------------------------------

/// Produces the complete auth headers for one request. Providers own one of
/// these instead of a raw key, so the *shape* of authentication (static API
/// key today, refreshing OAuth token later) is a swappable strategy rather
/// than something baked into each provider.
///
/// The supplier decides the header names: Anthropic-style keys arrive as
/// `x-api-key`, OpenAI-style and OAuth bearer tokens as `Authorization`.
#[async_trait]
pub trait TokenSupplier: Send + Sync {
    /// Auth headers to attach to the next request. May refresh credentials
    /// internally (OAuth kinds), which is why this is async.
    async fn headers(&self) -> Result<Vec<(String, String)>, ProviderError>;
}

/// A fixed set of auth headers — what every API-key kind uses today.
pub struct StaticToken {
    headers: Vec<(String, String)>,
}

impl StaticToken {
    pub fn new(headers: Vec<(String, String)>) -> Self {
        Self { headers }
    }

    /// OpenAI-style bearer auth.
    pub fn bearer(key: impl Into<String>) -> Self {
        Self::new(vec![(
            "Authorization".to_string(),
            format!("Bearer {}", key.into()),
        )])
    }

    /// Anthropic-style `x-api-key` auth.
    pub fn x_api_key(key: impl Into<String>) -> Self {
        Self::new(vec![("x-api-key".to_string(), key.into())])
    }
}

#[async_trait]
impl TokenSupplier for StaticToken {
    async fn headers(&self) -> Result<Vec<(String, String)>, ProviderError> {
        Ok(self.headers.clone())
    }
}

/// Append the exact provider JSON body to a JSONL file when explicitly
/// requested. This is intentionally opt-in because it contains the complete
/// conversation and tool schemas, but never the API key or request headers.
///
/// Set `FIRMIUS_DUMP_REQUESTS=/path/to/requests.jsonl` while reproducing a
/// provider issue. Each provider call appends one self-contained JSON object.
pub(crate) fn dump_provider_request(provider_id: &str, body: &Value) {
    let Ok(path) = std::env::var("FIRMIUS_DUMP_REQUESTS") else {
        return;
    };
    let record = serde_json::json!({
        "provider": provider_id,
        "body": body,
    });
    let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) else {
        return;
    };
    let _ = writeln!(file, "{record}");
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

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn bearer_supplier_emits_authorization_header() {
        let headers = StaticToken::bearer("sk-test").headers().await.unwrap();
        assert_eq!(
            headers,
            vec![("Authorization".to_string(), "Bearer sk-test".to_string())]
        );
    }

    #[tokio::test]
    async fn x_api_key_supplier_emits_anthropic_header() {
        let headers = StaticToken::x_api_key("sk-ant").headers().await.unwrap();
        assert_eq!(
            headers,
            vec![("x-api-key".to_string(), "sk-ant".to_string())]
        );
    }

    #[tokio::test]
    async fn custom_header_sets_pass_through() {
        let supplier = StaticToken::new(vec![
            ("Authorization".to_string(), "Bearer t".to_string()),
            ("x-extra".to_string(), "1".to_string()),
        ]);
        let headers = supplier.headers().await.unwrap();
        assert_eq!(headers.len(), 2);
    }

    #[test]
    fn providers_construct_from_static_suppliers() {
        // Construction is the seam: both providers accept any supplier.
        let _ = AnthropicProvider::with_auth("a", std::sync::Arc::new(StaticToken::x_api_key("k")));
        let _ = OpenAiProvider::with_auth(
            "o",
            "https://example.test/v1",
            std::sync::Arc::new(StaticToken::bearer("k")),
        );
        // Convenience constructors still work as before.
        let _ = AnthropicProvider::new("a", "k");
        let _ = OpenAiProvider::new("o", "https://example.test/v1", "k");
    }
}
