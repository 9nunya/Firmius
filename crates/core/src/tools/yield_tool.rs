use super::{ToolOutput, WORKER_YIELD_SCOPE};
use crate::work::{AuthorizationContext, ExecutionStatus, Outcome};
use crate::{Tool, ToolContext, ToolError, ToolRegistry};
use async_trait::async_trait;
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::Value;

#[derive(Debug, Deserialize, JsonSchema)]
struct YieldArgs {
    summary: String,
    #[serde(default)]
    outcome: Option<String>,
    #[serde(default)]
    output: Option<Value>,
    /// Paths or artifact:// references produced by this attempt.
    #[serde(default)]
    artifacts: Vec<String>,
    /// Evidence supporting the outcome (test output, log excerpts, etc.).
    #[serde(default)]
    evidence: Vec<String>,
    /// Files changed by this attempt.
    #[serde(default)]
    changed_files: Vec<String>,
    /// Free-form context for whoever picks this work up next (the parent,
    /// or a future retry). Folded into the durable result summary.
    #[serde(default)]
    handoff: Option<String>,
    /// Achieved verification level for this result (M5.1/M5.4). Distinct
    /// from the node's *required* verification level — a node that
    /// succeeded but whose result verification is below what the node
    /// requires remains visibly unverified in projections and the quality
    /// digest, even though `ExecutionStatus::Succeeded` is set.
    #[serde(default)]
    verification: Option<String>,
    /// Evidence links identifying which acceptance criterion (by id) each
    /// piece of evidence supports. `criterion_id: null` means general
    /// evidence not tied to a specific criterion.
    #[serde(default)]
    evidence_links: Vec<EvidenceLinkArg>,
}

#[derive(Debug, Deserialize, JsonSchema)]
struct EvidenceLinkArg {
    #[serde(default)]
    criterion_id: Option<String>,
    reference: String,
}

fn parse_verification(value: &Option<String>) -> Result<crate::work::VerificationLevel, ToolError> {
    use crate::work::VerificationLevel;
    Ok(match value.as_deref() {
        None => VerificationLevel::None,
        Some("none") => VerificationLevel::None,
        Some("self_verified") | Some("self") => VerificationLevel::SelfVerified,
        Some("reviewed") => VerificationLevel::Reviewed,
        Some("independently_verified") | Some("independent") => {
            VerificationLevel::IndependentlyVerified
        }
        Some(other) => {
            return Err(ToolError::InvalidArguments(format!(
                "unknown verification level '{other}'"
            )));
        }
    })
}

struct YieldTool;

#[async_trait]
impl Tool for YieldTool {
    fn name(&self) -> &str {
        "yield"
    }
    fn description(&self) -> &str {
        "Worker-only: settle your durable task assignment with an immutable result and stop this turn. Call this when you finish (or cannot finish) a node you were bound to via `delegate` `task_id`. The parent reads the result; do not also `task complete` the same node from the parent while you hold the assignment. Unbound agents (no assignment) cannot yield."
    }
    fn input_schema(&self) -> Value {
        serde_json::to_value(schemars::schema_for!(YieldArgs)).unwrap_or(Value::Null)
    }
    fn required_scopes(&self) -> &[String] {
        static SCOPES: std::sync::OnceLock<Vec<String>> = std::sync::OnceLock::new();
        SCOPES.get_or_init(|| vec![WORKER_YIELD_SCOPE.to_string()])
    }
    async fn call(&self, args: Value, ctx: ToolContext) -> Result<String, ToolError> {
        match self.call_output(args, ctx).await? {
            ToolOutput::Content(value) | ToolOutput::StopTurn { content: value } => Ok(value),
        }
    }
    async fn call_output(&self, args: Value, ctx: ToolContext) -> Result<ToolOutput, ToolError> {
        let args: YieldArgs =
            serde_json::from_value(args).map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
        let session = ctx
            .session
            .clone()
            .ok_or_else(|| ToolError::Failed("yield requires a session".into()))?;
        let binding = session
            .work
            .read()
            .unwrap()
            .binding_for_agent(&ctx.agent_id)
            .cloned()
            .ok_or_else(|| {
                ToolError::Failed("yield is only available to an assigned worker".into())
            })?;
        // Validate the structured output against the assigned node's
        // declared output contract, if it declares any required fields.
        {
            let state = session.work.read().unwrap();
            if let Ok(graph) = state.graph(binding.graph_id)
                && let Some(node) = graph.nodes.get(&binding.node_id)
                && !node.output_contract.required_fields.is_empty()
            {
                let provided = args
                    .output
                    .as_ref()
                    .and_then(|value| value.as_object())
                    .cloned()
                    .unwrap_or_default();
                let missing: Vec<&String> = node
                    .output_contract
                    .required_fields
                    .iter()
                    .filter(|field| !provided.contains_key(field.as_str()))
                    .collect();
                if !missing.is_empty() {
                    return Err(ToolError::InvalidArguments(format!(
                        "output is missing required fields: {}",
                        missing
                            .iter()
                            .map(|f| f.as_str())
                            .collect::<Vec<_>>()
                            .join(", ")
                    )));
                }
            }
        }
        let outcome = match args.outcome.as_deref() {
            Some("cancelled") => Some(Outcome::Cancelled),
            Some("interrupted") => Some(Outcome::Interrupted),
            Some("failure") | Some("failed") => Some(Outcome::Failure),
            Some(other) => Some(Outcome::Custom(other.to_string())),
            None => Some(Outcome::Success),
        };
        let agent_id = ctx.agent_id.clone();
        let summary = match &args.handoff {
            Some(handoff) if !handoff.is_empty() => {
                format!("{}\n\nHandoff: {handoff}", args.summary)
            }
            _ => args.summary.clone(),
        };
        let artifacts = args.artifacts.clone();
        let evidence = args.evidence.clone();
        let changed_files = args.changed_files.clone();
        let verification = parse_verification(&args.verification)?;
        let evidence_links: Vec<crate::work::EvidenceLink> = args
            .evidence_links
            .iter()
            .map(|link| crate::work::EvidenceLink {
                criterion_id: link.criterion_id.clone(),
                reference: link.reference.clone(),
            })
            .collect();
        let result = session
            .mutate_work(move |state| {
                let expected = state.graph(binding.graph_id)?.revision;
                let auth = AuthorizationContext {
                    agent_id: agent_id.clone(),
                    assignment_ids: [binding.assignment_id].into_iter().collect(),
                    ..Default::default()
                };
                let status = if outcome == Some(Outcome::Success) {
                    ExecutionStatus::Succeeded
                } else {
                    ExecutionStatus::Failed
                };
                let result = state.settle_assignment(
                    binding.graph_id,
                    expected,
                    &auth,
                    binding.assignment_id,
                    status,
                    outcome,
                    summary.clone(),
                    args.output.clone(),
                    artifacts.clone(),
                    evidence.clone(),
                    evidence_links.clone(),
                    changed_files.clone(),
                    verification,
                )?;
                let record = state.graph(binding.graph_id)?.results[&result].clone();
                Ok((
                    result,
                    crate::work::WorkEvent::ResultRecorded {
                        graph_id: binding.graph_id,
                        result: record,
                    },
                ))
            })
            .map_err(ToolError::Failed)?;
        Ok(ToolOutput::StopTurn {
            content: format!("yielded result_id={result}"),
        })
    }
}

pub fn register_yield_tool(registry: &ToolRegistry) -> &ToolRegistry {
    registry.register(YieldTool);
    registry
}
