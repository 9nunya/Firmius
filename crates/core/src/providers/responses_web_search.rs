//! Shared OpenAI Responses-API hosted web-search mapping.
//!
//! Codex and Grok both speak this dialect: inject `{type: web_search}` next to
//! function tools, replay `MessagePart::WebSearch` as `web_search_call`, and
//! parse `response.output_item.{added,done}` items of that type as hosted
//! search events — never as `ToolCall`.
//!
//! Capability advertisement stays per-provider (Codex claims TextAndImage;
//! Grok claims Text) so a shared handle cannot over-promise.

use super::ProviderEvent;
use crate::types::{WebSearchAction, WebSearchMode, WebSearchRequest};
use serde_json::{Value, json};

/// `{type: web_search, ...}` with mode flags from the openai/codex wire.
pub(crate) fn web_search_tool_object(mode: WebSearchMode) -> Value {
    match mode {
        WebSearchMode::Cached => json!({
            "type": "web_search",
            "external_web_access": false,
        }),
        WebSearchMode::Indexed => json!({
            "type": "web_search",
            "external_web_access": true,
            "indexed_web_access": true,
        }),
        WebSearchMode::Live => json!({
            "type": "web_search",
            "external_web_access": true,
        }),
    }
}

/// Append the hosted search tool when the request asks for it. No-op if
/// `web_search` is `None` (user policy off). Creates `tools` if needed.
pub(crate) fn inject_web_search_tool(body: &mut Value, web_search: Option<&WebSearchRequest>) {
    let Some(request) = web_search else {
        return;
    };
    let tool = web_search_tool_object(request.mode);
    match body.get_mut("tools") {
        Some(Value::Array(tools)) => tools.push(tool),
        _ => body["tools"] = json!([tool]),
    }
}

/// Replay an assistant-owned hosted search as a completed `web_search_call`.
pub(crate) fn web_search_call_item(id: &str, action: &WebSearchAction) -> Value {
    json!({
        "type": "web_search_call",
        "id": id,
        "status": "completed",
        "action": action,
    })
}

fn parse_web_search_action(action: &Value) -> WebSearchAction {
    serde_json::from_value(action.clone()).unwrap_or(WebSearchAction::Other)
}

/// Map a Responses `item` of type `web_search_call` to a hosted-search event.
/// `done` is true for `response.output_item.done`.
///
/// Partial items with no action start the search; a finish without action
/// becomes [`WebSearchAction::Other`]. Returns `None` for any other item type
/// so the function-call path stays untouched.
pub(crate) fn web_search_event_from_item(item: &Value, done: bool) -> Option<ProviderEvent> {
    if item.get("type").and_then(Value::as_str) != Some("web_search_call") {
        return None;
    }
    let id = item.get("id").and_then(Value::as_str)?.to_string();
    if done {
        let action = item
            .get("action")
            .map(parse_web_search_action)
            .unwrap_or(WebSearchAction::Other);
        Some(ProviderEvent::WebSearchFinished { id, action })
    } else {
        Some(ProviderEvent::WebSearchStarted { id })
    }
}
