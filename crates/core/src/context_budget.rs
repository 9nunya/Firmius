//! Conservative, provider-independent context window estimates.
//!
//! These are deliberately estimates rather than tokenization.  The module is
//! useful before a provider request is made and must not be treated as usage
//! accounting (provider-reported usage remains authoritative).

use crate::types::{ImageDetail, Message, MessagePart, ModelInfo, ProviderRequest};

/// Approximate token costs for image inputs.  These match the rough pricing
/// buckets commonly used by multimodal APIs, not the bytes in an image.
pub const IMAGE_LOW_TOKENS: u32 = 85;
pub const IMAGE_AUTO_TOKENS: u32 = 1_000;
pub const IMAGE_HIGH_TOKENS: u32 = 1_700;
pub const DEFAULT_OUTPUT_RESERVE: u32 = 4_096;

/// The individual components make estimates explainable in diagnostics and
/// tests, while `total` is the value normally used for budget decisions.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TokenEstimate {
    pub text: u32,
    pub tool_args: u32,
    pub tool_results: u32,
    pub thinking: u32,
    pub images: u32,
    pub tool_schemas: u32,
    pub overhead: u32,
}

impl TokenEstimate {
    pub fn total(self) -> u32 {
        self.text
            .saturating_add(self.tool_args)
            .saturating_add(self.tool_results)
            .saturating_add(self.thinking)
            .saturating_add(self.images)
            .saturating_add(self.tool_schemas)
            .saturating_add(self.overhead)
    }
}

/// Estimate the input side of a normalized provider request.
pub fn estimate_request(request: &ProviderRequest) -> TokenEstimate {
    let mut estimate = estimate_messages(&request.messages);
    estimate.tool_schemas = request
        .tools
        .iter()
        .map(|tool| {
            // JSON is a stable approximation of the payload sent to a
            // provider and includes names, descriptions, and schema shape.
            approx_tokens(
                &serde_json::json!({
                    "name": tool.name,
                    "description": tool.description,
                    "input_schema": tool.input_schema,
                })
                .to_string(),
            )
        })
        .fold(0, u32::saturating_add);
    estimate
}

pub fn estimate_messages(messages: &[Message]) -> TokenEstimate {
    let mut out = TokenEstimate {
        overhead: (messages.len() as u32).saturating_mul(4),
        ..Default::default()
    };
    for message in messages {
        for part in &message.content {
            match part {
                MessagePart::Text(value) => {
                    out.text = out.text.saturating_add(approx_tokens(value))
                }
                MessagePart::ToolCall { name, args, id } => {
                    out.tool_args = out.tool_args.saturating_add(approx_tokens(args));
                    out.overhead = out
                        .overhead
                        .saturating_add(approx_tokens(name).saturating_add(approx_tokens(id)));
                }
                MessagePart::ToolResult { id, content, .. } => {
                    out.tool_results = out.tool_results.saturating_add(approx_tokens(content));
                    out.overhead = out.overhead.saturating_add(approx_tokens(id));
                }
                MessagePart::Thinking { content, signature } => {
                    out.thinking = out.thinking.saturating_add(approx_tokens(content));
                    if let Some(signature) = signature {
                        out.thinking = out.thinking.saturating_add(approx_tokens(signature));
                    }
                }
                MessagePart::Image(image) => {
                    out.images = out.images.saturating_add(match image.detail.as_ref() {
                        Some(ImageDetail::Low) => IMAGE_LOW_TOKENS,
                        Some(ImageDetail::High) => IMAGE_HIGH_TOKENS,
                        Some(ImageDetail::Auto) | None => IMAGE_AUTO_TOKENS,
                    });
                }
                MessagePart::WebSearch { id, action } => {
                    out.overhead = out.overhead.saturating_add(approx_tokens(id));
                    let encoded = serde_json::to_string(action).unwrap_or_default();
                    out.text = out.text.saturating_add(approx_tokens(&encoded));
                }
            }
        }
    }
    out
}

/// A safety margin and output reservation leave room for provider-specific
/// serialization overhead and the generated response.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BudgetConfig {
    pub safety_margin_tokens: u32,
    pub safety_margin_ratio: f32,
    pub soft_threshold: f32,
    pub hard_threshold: f32,
}

impl Default for BudgetConfig {
    fn default() -> Self {
        Self {
            safety_margin_tokens: 256,
            safety_margin_ratio: 0.10,
            soft_threshold: 0.80,
            hard_threshold: 0.95,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BudgetDecision {
    Within,
    Soft,
    Hard,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BudgetAssessment {
    pub estimated_input: u32,
    pub reserved_output: u32,
    pub safety_margin: u32,
    pub usable_input: u32,
    pub decision: BudgetDecision,
}

impl BudgetAssessment {
    /// Whether the estimated request cannot fit after reserving output and
    /// the safety margin. This is deliberately separate from `Hard`, which
    /// is also used as a proactive compaction threshold below full capacity.
    pub fn exceeds_usable_input(self) -> bool {
        self.estimated_input > self.usable_input
    }
}

/// Calculate available input tokens after reserving output and safety room.
pub fn usable_budget(model: &ModelInfo, request: &ProviderRequest, config: BudgetConfig) -> u32 {
    assessment(model, request, config).usable_input
}

pub fn assessment(
    model: &ModelInfo,
    request: &ProviderRequest,
    config: BudgetConfig,
) -> BudgetAssessment {
    let estimated_input = estimate_request(request).total();
    let requested = request
        .max_tokens
        .or(model.max_output_tokens)
        .unwrap_or(DEFAULT_OUTPUT_RESERVE);
    let reserved_output = model
        .max_output_tokens
        .map_or(requested, |max| requested.min(max));
    let safety_margin = config
        .safety_margin_tokens
        .max((model.context_window as f32 * config.safety_margin_ratio).ceil() as u32);
    let usable_input = model
        .context_window
        .saturating_sub(reserved_output)
        .saturating_sub(safety_margin);
    let ratio = if usable_input == 0 {
        None
    } else {
        Some(estimated_input as f32 / usable_input as f32)
    };
    let decision = if usable_input == 0 && estimated_input > 0 {
        BudgetDecision::Hard
    } else if ratio.is_some_and(|ratio| ratio >= config.hard_threshold) {
        BudgetDecision::Hard
    } else if ratio.is_some_and(|ratio| ratio >= config.soft_threshold) {
        BudgetDecision::Soft
    } else {
        BudgetDecision::Within
    };
    BudgetAssessment {
        estimated_input,
        reserved_output,
        safety_margin,
        usable_input,
        decision,
    }
}

fn approx_tokens(value: &str) -> u32 {
    ((value.chars().count() as u32).saturating_add(3) / 4).max(if value.is_empty() { 0 } else { 1 })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{MessageRole, ToolDefinition};

    fn model(window: u32, output: Option<u32>) -> ModelInfo {
        ModelInfo {
            id: "test".into(),
            context_window: window,
            max_output_tokens: output,
            capabilities: Default::default(),
            effort_modes: vec![],
        }
    }
    fn request(messages: Vec<Message>) -> ProviderRequest {
        ProviderRequest {
            model: "test".into(),
            messages,
            tools: vec![],
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget_tokens: None,
            session_id: None,
            web_search: None,
        }
    }

    #[test]
    fn counts_parts_and_tool_schema_separately() {
        let mut req = request(vec![Message::with_parts(
            MessageRole::User,
            [
                MessagePart::Text("abcd".into()),
                MessagePart::Image(crate::types::ImagePart::from_url("x")),
            ],
        )]);
        req.tools.push(ToolDefinition {
            name: "run".into(),
            description: "do it".into(),
            input_schema: serde_json::json!({"type":"object"}),
        });
        let e = estimate_request(&req);
        assert_eq!(e.text, 1);
        assert_eq!(e.images, IMAGE_AUTO_TOKENS);
        assert!(e.tool_schemas > 0);
    }

    #[test]
    fn reserves_output_and_margin_without_underflow() {
        let req = request(vec![Message::text(MessageRole::User, "hello")]);
        let a = assessment(
            &model(100, Some(80)),
            &req,
            BudgetConfig {
                safety_margin_tokens: 30,
                safety_margin_ratio: 0.0,
                ..Default::default()
            },
        );
        assert_eq!(a.reserved_output, 80);
        assert_eq!(a.usable_input, 0);
    }

    #[test]
    fn soft_and_hard_thresholds_are_distinct() {
        let config = BudgetConfig {
            safety_margin_tokens: 10,
            safety_margin_ratio: 0.0,
            ..Default::default()
        };
        // Output reservation and safety margin leave 70 usable input tokens.
        // The one-message overhead is four tokens, so these inputs estimate
        // exactly 80% and 95% of that usable capacity.
        let req = request(vec![Message::text(MessageRole::User, &"x".repeat(208))]);
        let soft = assessment(&model(100, Some(20)), &req, config);
        assert_eq!(soft.estimated_input, 56);
        assert_eq!(soft.usable_input, 70);
        assert_eq!(soft.decision, BudgetDecision::Soft);

        let req = request(vec![Message::text(MessageRole::User, &"x".repeat(252))]);
        let hard = assessment(&model(100, Some(20)), &req, config);
        assert_eq!(hard.estimated_input, 67);
        assert_eq!(hard.usable_input, 70);
        assert_eq!(hard.decision, BudgetDecision::Hard);
    }

    #[test]
    fn reported_post_compaction_estimate_fits_usable_budget() {
        // Regression for the observed failure: the old two-state hard gate
        // rejected this request because 205,552 is about 97% of 211,900,
        // even though it still fits the already safety-reduced input budget.
        let req = request(vec![Message::text(MessageRole::User, &"x".repeat(822_192))]);
        let assessment = assessment(
            &model(215_996, Some(4_096)),
            &req,
            BudgetConfig {
                safety_margin_tokens: 0,
                safety_margin_ratio: 0.0,
                ..Default::default()
            },
        );

        assert_eq!(assessment.estimated_input, 205_552);
        assert_eq!(assessment.usable_input, 211_900);
        assert_eq!(assessment.decision, BudgetDecision::Hard);
        assert!(!assessment.exceeds_usable_input());
    }

    #[test]
    fn nonzero_input_with_no_usable_capacity_exceeds_budget() {
        let req = request(vec![Message::text(MessageRole::User, "x")]);
        let assessment = assessment(
            &model(100, Some(100)),
            &req,
            BudgetConfig {
                safety_margin_tokens: 0,
                safety_margin_ratio: 0.0,
                ..Default::default()
            },
        );

        assert_eq!(assessment.usable_input, 0);
        assert!(assessment.estimated_input > 0);
        assert_eq!(assessment.decision, BudgetDecision::Hard);
        assert!(assessment.exceeds_usable_input());
    }
}
