//! Provider capability handles. Presence is the upper bound: a provider that
//! does not advertise a handle cannot perform that hosted action, and config
//! may only intersect (never union) with this set.
//!
//! These are runtime descriptors, not model-card flags. Do not fold them into
//! [`crate::types::ModelCapability`].

use serde::{Deserialize, Serialize};

/// How a provider can satisfy a hosted web-search request.
///
/// There is no `Disabled` variant: that is a user-policy concern, not a
/// capability. Default user policy is off even when a provider advertises
/// search.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WebSearchMode {
    Cached,
    Indexed,
    Live,
}

impl WebSearchMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Cached => "cached",
            Self::Indexed => "indexed",
            Self::Live => "live",
        }
    }
}

impl std::str::FromStr for WebSearchMode {
    type Err = ();

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "cached" => Ok(Self::Cached),
            "indexed" => Ok(Self::Indexed),
            "live" => Ok(Self::Live),
            _ => Err(()),
        }
    }
}

/// What a hosted search result may contain.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WebSearchContent {
    Text,
    TextAndImage,
}

/// A completed hosted web-search action, persisted on the assistant message.
///
/// Internally tagged so provider wire (`type: search | open_page | ...`) and
/// session JSON share a shape. Unknown `type` values deserialize as [`Self::Other`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum WebSearchAction {
    Search {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        query: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        queries: Option<Vec<String>>,
    },
    OpenPage {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        url: Option<String>,
    },
    FindInPage {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        url: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        pattern: Option<String>,
    },
    #[serde(other)]
    Other,
}

impl WebSearchAction {
    /// Query, URL, or pattern used as the TUI subject. `Other` and empty
    /// fields yield `None` so presenters can still render a verb-only line.
    pub fn subject(&self) -> Option<&str> {
        match self {
            Self::Search { query, queries } => query.as_deref().or_else(|| {
                queries
                    .as_ref()
                    .and_then(|qs| qs.first())
                    .map(String::as_str)
            }),
            Self::OpenPage { url } => url.as_deref(),
            Self::FindInPage { pattern, url } => pattern.as_deref().or(url.as_deref()),
            Self::Other => None,
        }
        .map(str::trim)
        .filter(|s| !s.is_empty())
    }
}

/// Provider-side description of hosted web search. Advertise by wrapping in
/// `Some`; `None` on [`ProviderCapabilities::web_search`] means the provider
/// cannot do it (and must not inject a hosted tool).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LLMWebSearch {
    pub modes: &'static [WebSearchMode],
    pub default_mode: WebSearchMode,
    pub content: WebSearchContent,
    pub supports_filters: bool,
    pub supports_location: bool,
}

/// Per-request hosted search. `None` on the request means "do not advertise".
/// v1 carries mode only; filters/location are later extensions.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WebSearchRequest {
    pub mode: WebSearchMode,
}

/// Optional handles for hosted provider capabilities. A missing handle is
/// inability, not an unset preference.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ProviderCapabilities {
    pub web_search: Option<LLMWebSearch>,
}

impl ProviderCapabilities {
    pub fn none() -> Self {
        Self::default()
    }
}

impl LLMWebSearch {
    pub fn supports(&self, mode: WebSearchMode) -> bool {
        self.modes.contains(&mode)
    }
}

/// Intersect user policy with a provider capability handle.
///
/// `None` policy is off. An unrecognized policy string is off. A missing
/// handle, or a handle that does not list the requested mode, means the
/// request must not advertise search (config never unions).
pub fn intersect_web_search(
    policy: Option<&str>,
    capability: Option<&LLMWebSearch>,
) -> Option<WebSearchRequest> {
    let policy = policy.filter(|s| !s.is_empty())?;
    let mode: WebSearchMode = policy.parse().ok()?;
    let cap = capability?;
    cap.supports(mode).then_some(WebSearchRequest { mode })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn none_and_default_are_empty() {
        assert_eq!(
            ProviderCapabilities::none(),
            ProviderCapabilities::default()
        );
        assert!(ProviderCapabilities::none().web_search.is_none());
    }

    #[test]
    fn web_search_action_round_trips_tagged() {
        let search = WebSearchAction::Search {
            query: Some("rust async".into()),
            queries: None,
        };
        let value = serde_json::to_value(&search).unwrap();
        assert_eq!(value["type"], "search");
        assert_eq!(value["query"], "rust async");
        assert!(value.get("queries").is_none());
        assert_eq!(
            serde_json::from_value::<WebSearchAction>(value).unwrap(),
            search
        );

        let other: WebSearchAction =
            serde_json::from_value(serde_json::json!({"type": "unknown_future"})).unwrap();
        assert_eq!(other, WebSearchAction::Other);
    }

    #[test]
    fn web_search_mode_parses_config_strings() {
        assert_eq!("cached".parse(), Ok(WebSearchMode::Cached));
        assert_eq!("indexed".parse(), Ok(WebSearchMode::Indexed));
        assert_eq!("live".parse(), Ok(WebSearchMode::Live));
        assert!("off".parse::<WebSearchMode>().is_err());
        assert_eq!(WebSearchMode::Live.as_str(), "live");
    }

    #[test]
    fn intersect_web_search_is_off_by_default_and_requires_capability() {
        const CACHED_LIVE: &[WebSearchMode] = &[WebSearchMode::Cached, WebSearchMode::Live];
        let cap = LLMWebSearch {
            modes: CACHED_LIVE,
            default_mode: WebSearchMode::Cached,
            content: WebSearchContent::Text,
            supports_filters: false,
            supports_location: false,
        };
        assert!(intersect_web_search(None, Some(&cap)).is_none());
        assert!(intersect_web_search(Some("live"), None).is_none());
        assert!(intersect_web_search(Some("off"), Some(&cap)).is_none());
        assert!(intersect_web_search(Some("indexed"), Some(&cap)).is_none());
        assert_eq!(
            intersect_web_search(Some("live"), Some(&cap)),
            Some(WebSearchRequest {
                mode: WebSearchMode::Live
            })
        );
        assert_eq!(
            intersect_web_search(Some("cached"), Some(&cap)),
            Some(WebSearchRequest {
                mode: WebSearchMode::Cached
            })
        );
    }
}
