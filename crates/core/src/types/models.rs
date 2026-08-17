use serde::de::Deserializer;
use serde::ser::Serializer;
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;

// ---------------------------------------------------------------------------
// Model capabilities
// ---------------------------------------------------------------------------

/// One discrete capability a model may support.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ModelCapability {
    Text,
    Image,
    Video,
    Pdf,
    Audio,
    ToolUse,
    /// Extended thinking / reasoning controls.
    Reasoning,
}

/// A stable, deduplicated set of model capabilities.
///
/// Serializes as `[
///   "text",
///   "image"
/// ]` and still accepts the legacy boolean-object shape while persisted data
/// migrates forward naturally on the next save.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelCapabilities(BTreeSet<ModelCapability>);

impl ModelCapabilities {
    pub fn new(capabilities: impl IntoIterator<Item = ModelCapability>) -> Self {
        Self(capabilities.into_iter().collect())
    }

    pub fn text() -> Self {
        Self::new([ModelCapability::Text])
    }

    pub fn iter(&self) -> impl Iterator<Item = ModelCapability> + '_ {
        self.0.iter().copied()
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    pub fn supports(&self, capability: ModelCapability) -> bool {
        self.0.contains(&capability)
    }

    pub fn supports_all(&self, required: &ModelCapabilities) -> bool {
        required.iter().all(|capability| self.supports(capability))
    }

    pub fn insert(&mut self, capability: ModelCapability) {
        self.0.insert(capability);
    }

    pub fn extend(&mut self, capabilities: impl IntoIterator<Item = ModelCapability>) {
        self.0.extend(capabilities);
    }
}

impl Default for ModelCapabilities {
    fn default() -> Self {
        Self::text()
    }
}

impl<const N: usize> From<[ModelCapability; N]> for ModelCapabilities {
    fn from(value: [ModelCapability; N]) -> Self {
        Self::new(value)
    }
}

impl IntoIterator for ModelCapabilities {
    type Item = ModelCapability;
    type IntoIter = std::collections::btree_set::IntoIter<ModelCapability>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.into_iter()
    }
}

impl Serialize for ModelCapabilities {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        let ordered: Vec<ModelCapability> = self.iter().collect();
        ordered.serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for ModelCapabilities {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum Repr {
            List(Vec<ModelCapability>),
            Legacy(LegacyModelCapabilities),
        }

        #[derive(Deserialize, Default)]
        struct LegacyModelCapabilities {
            #[serde(default = "default_true")]
            text: bool,
            #[serde(default)]
            image: bool,
            #[serde(default)]
            video: bool,
            #[serde(default)]
            pdf: bool,
            #[serde(default)]
            audio: bool,
            #[serde(default)]
            tool_use: bool,
            #[serde(default)]
            reasoning: bool,
        }

        match Repr::deserialize(deserializer)? {
            Repr::List(values) => Ok(Self::new(values)),
            Repr::Legacy(legacy) => {
                let mut capabilities = Self::new([]);
                if legacy.text {
                    capabilities.insert(ModelCapability::Text);
                }
                if legacy.image {
                    capabilities.insert(ModelCapability::Image);
                }
                if legacy.video {
                    capabilities.insert(ModelCapability::Video);
                }
                if legacy.pdf {
                    capabilities.insert(ModelCapability::Pdf);
                }
                if legacy.audio {
                    capabilities.insert(ModelCapability::Audio);
                }
                if legacy.tool_use {
                    capabilities.insert(ModelCapability::ToolUse);
                }
                if legacy.reasoning {
                    capabilities.insert(ModelCapability::Reasoning);
                }
                Ok(capabilities)
            }
        }
    }
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

    pub fn supports(&self, capability: ModelCapability) -> bool {
        self.capabilities.supports(capability)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serializes_as_capability_list() {
        let caps = ModelCapabilities::from([ModelCapability::Text, ModelCapability::Image]);
        assert_eq!(
            serde_json::to_value(caps).unwrap(),
            serde_json::json!(["text", "image"])
        );
    }

    #[test]
    fn deserializes_legacy_boolean_shape() {
        let caps: ModelCapabilities = serde_json::from_value(serde_json::json!({
            "text": true,
            "image": true,
            "tool_use": true,
        }))
        .unwrap();
        assert!(caps.supports(ModelCapability::Text));
        assert!(caps.supports(ModelCapability::Image));
        assert!(caps.supports(ModelCapability::ToolUse));
        assert!(!caps.supports(ModelCapability::Audio));
    }

    #[test]
    fn deduplicates_capabilities() {
        let caps: ModelCapabilities =
            serde_json::from_value(serde_json::json!(["text", "image", "image"])).unwrap();
        assert_eq!(caps.iter().count(), 2);
    }
}
