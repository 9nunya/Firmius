use serde::{Deserialize, Serialize};

// ---------------------------------------------------------------------------
// Model capabilities
// ---------------------------------------------------------------------------

/// What a model can do. Used for routing and UI.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct ModelCapabilities {
    #[serde(default = "default_true")]
    pub text: bool,
    #[serde(default)]
    pub image: bool,
    #[serde(default)]
    pub video: bool,
    #[serde(default)]
    pub pdf: bool,
    #[serde(default)]
    pub audio: bool,
    #[serde(default)]
    pub tool_use: bool,
    /// Whether the model supports extended thinking / reasoning.
    #[serde(default)]
    pub reasoning: bool,
}

const fn default_true() -> bool {
    true
}

// ---------------------------------------------------------------------------
// Effort modes
// ---------------------------------------------------------------------------

/// One selectable effort / reasoning level for a model.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EffortMode {
    pub name: String,
    /// Anthropic-style: maximum tokens to spend on thinking.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thinking_budget_tokens: Option<u32>,
    /// OpenAI-style: "low", "medium", "high", "xhigh".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reasoning_effort: Option<String>,
}

// ---------------------------------------------------------------------------
// Model info
// ---------------------------------------------------------------------------

/// Static description of a model. Can come from providers.json or be
/// fetched dynamically from the provider's API.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ModelInfo {
    pub id: String,
    pub context_window: u32,
    #[serde(default)]
    pub max_output_tokens: Option<u32>,
    #[serde(default)]
    pub capabilities: ModelCapabilities,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub effort_modes: Vec<EffortMode>,
}

impl ModelInfo {
    /// Token budget remaining: context_window minus current usage.
    pub fn remaining(&self, used: u32) -> u32 {
        self.context_window.saturating_sub(used)
    }

    /// Whether the model is likely to hit its context limit soon.
    pub fn near_limit(&self, used: u32, threshold_pct: f64) -> bool {
        let threshold = (self.context_window as f64 * threshold_pct) as u32;
        used >= threshold
    }

    /// Find an effort mode by name.
    pub fn effort_mode(&self, name: &str) -> Option<&EffortMode> {
        self.effort_modes.iter().find(|m| m.name == name)
    }
}
