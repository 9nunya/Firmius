//! A small model-facing entry point that compiles into the durable work graph.
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::{Value, json};

use super::task::{WORK_READ_SCOPE, WORK_WRITE_SCOPE, build_workflow, call_task};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

#[derive(Debug, Default, Deserialize, JsonSchema, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
enum Action {
    #[default]
    Run,
    Status,
    Wait,
    Cancel,
}

#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Step {
    /// Unique short key, e.g. "api". Successors receive your result under this key.
    key: String,
    /// Worker persona: coder for implementation, general for investigation,
    /// reviewer for independent verification, or an installed custom persona.
    persona: String,
    /// This worker's complete assignment: objective, boundaries, deliverable,
    /// and validation. Shared context belongs in the workflow brief.
    prompt: String,
    /// Keys that must succeed before this step starts. Their results are
    /// automatically supplied as named inputs. Omit for independent work.
    #[serde(default)]
    depends_on: Vec<String>,
    /// Structured coordination contract for this assignment. This is
    /// declarative intent and never expands edit or tool authority.
    #[serde(default)]
    assignment_contract: Option<Value>,
    /// Explicit advisory file scope for collision analysis and peer context.
    #[serde(default)]
    file_scope: Vec<String>,
}

#[derive(Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct Args {
    /// run creates and launches a new graph; status inspects it; wait returns
    /// the completed report; cancel stops it. Default run.
    #[serde(default)]
    action: Action,
    /// Required for run. Short user-facing title of the whole workflow.
    #[serde(default)]
    title: Option<String>,
    /// Shared objective, context, constraints, and quality bar for all workers.
    #[serde(default)]
    brief: Option<String>,
    /// Required for run. Independent steps run concurrently; depends_on
    /// controls ordering and passes predecessor results automatically.
    #[serde(default)]
    steps: Vec<Step>,
    /// Run id returned by run. Required for status, wait, and cancel.
    #[serde(default)]
    run_id: Option<String>,
    /// Concurrent workers, 1–16; defaults to 4. Workers share the working tree,
    /// so order overlapping edits with depends_on.
    #[serde(default)]
    max_concurrent: Option<u32>,
    /// Named graph-level integration owner and bounded integration scope.
    #[serde(default)]
    integration: Option<Value>,
}

pub(super) fn register_workflow_tool(registry: &ToolRegistry) {
    registry.register(TypedTool::new(
        "workflow",
        r#"Run a durable team workflow in one call. Independent steps run concurrently; depends_on orders stages and supplies predecessor results under their keys. The runtime owns spawning, handoffs, and settlement. Workers share the working tree: serialize overlapping edits. Use delegate for a single assignment, task for solo checklists or advanced graph/retry editing.

Example: {"title":"Investigate and synthesize","brief":"Find the cause of slow startup; cite code and measurements.","steps":[{"key":"io","persona":"general","prompt":"Investigate file I/O during startup."},{"key":"network","persona":"general","prompt":"Investigate startup network requests."},{"key":"synthesis","persona":"general","prompt":"Rank causes and recommend the smallest effective fix.","depends_on":["io","network"]}]}

Returns run_id and graph_id. Continue useful independent work, then {"action":"wait","run_id":"..."}. Status and wait include worker verdicts: a reviewer finishing successfully does not imply approval. Each step has one attempt; for bounded feedback/retry loops use task plan. Do not rerun after receiving a run_id; inspect that run instead. If launch fails after planning, the error identifies the saved graph to launch without duplicating it."#,
        |args: Args, ctx: ToolContext| Box::pin(execute(args, ctx)),
    ).with_required_scopes([WORK_READ_SCOPE, crate::DELEGATION_SCOPE]));
}

fn compile(args: &Args) -> Result<Value, ToolError> {
    let title = args
        .title
        .as_deref()
        .filter(|s| !s.trim().is_empty())
        .ok_or_else(|| {
            ToolError::InvalidArguments("workflow run requires a non-empty title".into())
        })?;
    if args.steps.is_empty() || args.steps.len() > 64 {
        return Err(ToolError::InvalidArguments(
            "workflow run requires 1–64 steps".into(),
        ));
    }

    if !(1..=16).contains(&args.max_concurrent.unwrap_or(4)) {
        return Err(ToolError::InvalidArguments(
            "max_concurrent must be between 1 and 16".into(),
        ));
    }
    let mut edges = Vec::new();
    let mut nodes = Vec::new();
    for step in &args.steps {
        if step.key.trim().is_empty()
            || step.persona.trim().is_empty()
            || step.prompt.trim().is_empty()
        {
            return Err(ToolError::InvalidArguments(
                "each step needs a non-empty key, persona, and prompt".into(),
            ));
        }
        nodes.push(json!({"key":step.key,"title":step.key,"executor":"agent",
            "persona":step.persona,"prompt":step.prompt,"max_attempts":1,
            "assignment_contract":step.assignment_contract,"file_scope":step.file_scope}));
        for predecessor in &step.depends_on {
            edges.push(
                json!({"from":predecessor,"to":step.key,"condition":"succeeded",
                "binding_alias":predecessor}),
            );
        }
    }
    Ok(
        json!({"title":title,"objective":args.brief,"brief":args.brief,"nodes":nodes,"edges":edges,
        "integration":args.integration}),
    )
}

async fn execute(args: Args, ctx: ToolContext) -> Result<String, ToolError> {
    if args.action != Action::Run {
        let run_id = args
            .run_id
            .as_deref()
            .filter(|s| !s.is_empty())
            .ok_or_else(|| {
                ToolError::InvalidArguments(
                    "status/wait/cancel require run_id from workflow run".into(),
                )
            })?;
        let action = match args.action {
            Action::Status => "poll",
            Action::Wait => "wait",
            Action::Cancel => "cancel",
            _ => unreachable!(),
        };
        return call_task(json!({"action":action,"run_id":run_id}), ctx).await;
    }
    if args.run_id.is_some() {
        return Err(ToolError::InvalidArguments(
            "run_id is for status/wait/cancel; omit it when creating a new workflow".into(),
        ));
    }
    if ctx
        .allowed_scopes
        .as_ref()
        .is_some_and(|s| !s.contains(WORK_WRITE_SCOPE))
    {
        return Err(ToolError::PermissionDenied {
            tool: "workflow".into(),
            required: vec![WORK_WRITE_SCOPE.into()],
            allowed: ctx
                .allowed_scopes
                .clone()
                .unwrap_or_default()
                .into_iter()
                .collect(),
        });
    }
    let plan = compile(&args)?;
    let session = ctx
        .session
        .as_ref()
        .ok_or_else(|| ToolError::Failed("workflow requires a session".into()))?;
    let parent = session
        .agent(&ctx.agent_id)
        .ok_or_else(|| ToolError::Failed("workflow caller is not a session agent".into()))?;
    for step in &args.steps {
        parent
            .persona_manager()
            .require(&step.persona)
            .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
    }
    let graph = build_workflow(plan, &ctx)?;
    let gid = graph.id;
    let owner = ctx.agent_id.clone();
    session
        .mutate_work(move |state| {
            state.create_graph(graph.clone(), None)?;
            state.set_active_graph(owner, gid)?;
            Ok(((), crate::work::WorkEvent::GraphCreated { graph }))
        })
        .map_err(ToolError::Failed)?;
    let result = call_task(
        json!({"action":"launch","graph_id":gid.to_string(),
        "max_concurrent":args.max_concurrent.unwrap_or(4),"max_attempts_total":args.steps.len()}),
        ctx,
    )
    .await;
    result.map_err(|e| ToolError::Failed(format!("Workflow saved as graph_id={gid}, but launch failed: {e}. Repair the cause and use task launch with this graph_id; do not create a duplicate workflow.")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dependencies_compile_to_gating_edges_and_named_inputs() {
        let args: Args = serde_json::from_value(json!({"title":"Review","steps":[
            {"key":"build","persona":"coder","prompt":"Implement"},
            {"key":"review","persona":"reviewer","prompt":"Review","depends_on":["build"]}
        ]}))
        .unwrap();
        let plan = compile(&args).unwrap();
        assert_eq!(
            plan["edges"][0],
            json!({"from":"build","to":"review","condition":"succeeded","binding_alias":"build"})
        );
        assert_eq!(plan["nodes"][0]["max_attempts"], 1);
    }

    #[test]
    fn structured_contract_and_file_scope_survive_compilation() {
        let args: Args = serde_json::from_value(json!({"title":"Build","steps":[{
            "key":"api","persona":"coder","prompt":"Implement API",
            "file_scope":["src/api.rs"],
            "assignment_contract":{
                "objective":"publish the API",
                "intended_mutation_paths":["src/api.rs"],
                "published_contracts":["api-v1"]
            }
        }]}))
        .unwrap();
        let plan = compile(&args).unwrap();
        assert_eq!(plan["nodes"][0]["file_scope"], json!(["src/api.rs"]));
        assert_eq!(
            plan["nodes"][0]["assignment_contract"]["published_contracts"],
            json!(["api-v1"])
        );
    }

    #[test]
    fn omitted_contract_compiles_to_nullable_task_input() {
        let args: Args = serde_json::from_value(json!({"title":"Build","steps":[{
            "key":"api","persona":"coder","prompt":"Implement API"
        }]}))
        .unwrap();
        let plan = compile(&args).unwrap();
        assert!(plan["nodes"][0]["assignment_contract"].is_null());
        assert!(
            serde_json::from_value::<crate::tools::task::PlanNodeArg>(plan["nodes"][0].clone(),)
                .is_ok(),
            "workflow's explicit null must deserialize at the task boundary"
        );
    }

    #[test]
    fn invalid_budgets_and_empty_assignments_are_rejected_before_mutation() {
        for value in [
            json!({"title":"Empty"}),
            json!({"title":"Bad","max_concurrent":0,"steps":[{"key":"a","persona":"coder","prompt":"Implement"}]}),
            json!({"title":"Bad","steps":[{"key":"a","persona":"coder","prompt":" "}]}),
        ] {
            assert!(compile(&serde_json::from_value(value).unwrap()).is_err());
        }
        assert!(serde_json::from_value::<Args>(json!({"titel":"typo"})).is_err());
    }

    #[test]
    fn lifecycle_actions_allow_only_their_run_id_at_execution_time() {
        let args: Args = serde_json::from_value(json!({
            "action": "wait",
            "run_id": "run-1"
        }))
        .unwrap();
        assert_eq!(args.action, Action::Wait);
        assert_eq!(args.run_id.as_deref(), Some("run-1"));
    }
}
