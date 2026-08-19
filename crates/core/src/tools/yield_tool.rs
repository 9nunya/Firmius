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
}

struct YieldTool;

#[async_trait]
impl Tool for YieldTool {
    fn name(&self) -> &str {
        "yield"
    }
    fn description(&self) -> &str {
        "Worker-only: settle your durable task assignment with an immutable result and stop this turn."
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
        let outcome = match args.outcome.as_deref() {
            Some("cancelled") => Some(Outcome::Cancelled),
            Some("interrupted") => Some(Outcome::Interrupted),
            Some("failure") | Some("failed") => Some(Outcome::Failure),
            Some(other) => Some(Outcome::Custom(other.to_string())),
            None => Some(Outcome::Success),
        };
        let agent_id = ctx.agent_id.clone();
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
                    args.summary.clone(),
                    args.output.clone(),
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
