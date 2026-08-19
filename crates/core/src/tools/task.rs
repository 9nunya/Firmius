//! The model-facing flat-schema checklist/work tool.
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::json;

use crate::work::{
    AuthorizationContext, GraphId, GraphMode, NodeId, Outcome, WorkEvent, WorkGraph,
};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

pub const WORK_READ_SCOPE: &str = "work_read";
pub const WORK_WRITE_SCOPE: &str = "work_write";

#[derive(Debug, Clone, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Mode {
    Create,
    Init,
    List,
    View,
    Add,
    Update,
    Move,
    Start,
    Complete,
    Fail,
    Block,
    Unblock,
    Cancel,
    Retry,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registration_exposes_one_flat_task_schema_and_read_scope() {
        let registry = ToolRegistry::default();
        register_task_tool(&registry);
        let definition = registry
            .definitions()
            .into_iter()
            .find(|definition| definition.name == "task")
            .expect("task is registered");
        assert_eq!(definition.input_schema["type"], "object");
        assert!(definition.input_schema.get("oneOf").is_none());
    }
}

impl Default for Mode {
    fn default() -> Self {
        Self::View
    }
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
struct TaskArgs {
    #[serde(default)]
    mode: Mode,
    #[serde(default)]
    graph_id: Option<String>,
    #[serde(default)]
    node_id: Option<String>,
    #[serde(default)]
    key: Option<String>,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    description: Option<String>,
    #[serde(default)]
    before_node_id: Option<String>,
    #[serde(default)]
    expected_revision: Option<u64>,
    #[serde(default)]
    outcome: Option<String>,
    #[serde(default)]
    summary: Option<String>,
    #[serde(default)]
    evidence: Vec<String>,
    #[serde(default)]
    objective: Option<String>,
    #[serde(default)]
    items: Vec<String>,
}

fn graph_id(value: &Option<String>) -> Result<GraphId, ToolError> {
    value
        .as_deref()
        .ok_or_else(|| ToolError::InvalidArguments("missing 'graph_id'".into()))
        .and_then(|v| {
            GraphId::parse(v)
                .map_err(|e| ToolError::InvalidArguments(format!("invalid graph_id: {e}")))
        })
}

fn auth(ctx: &ToolContext, write: bool) -> AuthorizationContext {
    AuthorizationContext {
        agent_id: ctx.agent_id.clone(),
        can_manage: write
            && ctx
                .allowed_scopes
                .as_ref()
                .is_some_and(|s| s.contains(WORK_WRITE_SCOPE)),
        assignment_ids: Default::default(),
    }
}

fn require_write(ctx: &ToolContext) -> Result<(), ToolError> {
    if ctx
        .allowed_scopes
        .as_ref()
        .is_some_and(|s| !s.contains(WORK_WRITE_SCOPE))
    {
        return Err(ToolError::PermissionDenied {
            tool: "task".into(),
            required: vec![WORK_WRITE_SCOPE.into()],
            allowed: ctx
                .allowed_scopes
                .clone()
                .unwrap_or_default()
                .into_iter()
                .collect(),
        });
    }
    Ok(())
}

fn node_id(graph: &WorkGraph, args: &TaskArgs) -> Result<NodeId, ToolError> {
    if let Some(value) = &args.node_id {
        return NodeId::parse(value)
            .map_err(|e| ToolError::InvalidArguments(format!("invalid node_id: {e}")));
    }
    let key = args
        .key
        .as_deref()
        .ok_or_else(|| ToolError::InvalidArguments("missing 'node_id' or 'key'".into()))?;
    graph
        .nodes
        .values()
        .find(|node| node.key == key)
        .map(|node| node.id)
        .ok_or_else(|| ToolError::InvalidArguments(format!("unknown node key '{key}'")))
}

pub fn register_task_tool(registry: &ToolRegistry) -> &ToolRegistry {
    registry.register(
        TypedTool::new(
            "task",
            "Create and mutate the durable session work checklist.",
            |args: TaskArgs, ctx: ToolContext| Box::pin(async move { task(args, ctx).await }),
        )
        .with_required_scopes([WORK_READ_SCOPE]),
    );
    registry
}

async fn task(args: TaskArgs, ctx: ToolContext) -> Result<String, ToolError> {
    let session = ctx
        .session
        .clone()
        .ok_or_else(|| ToolError::Failed("task requires a session".into()))?;
    match args.mode.clone() {
        Mode::List => {
            let state = session.work.read().unwrap();
            let graphs = state.graphs.values().map(|g| json!({"graph_id": g.id.to_string(), "title": g.title, "revision": g.revision, "status": g.status})).collect::<Vec<_>>();
            Ok(json!({"revision": state.revision, "graphs": graphs}).to_string())
        }
        Mode::View => {
            let state = session.work.read().unwrap();
            let gid = args
                .graph_id
                .as_ref()
                .map(|v| GraphId::parse(v))
                .transpose()
                .map_err(|e| ToolError::InvalidArguments(e.to_string()))?
                .or_else(|| state.active_graph_by_agent.get(&ctx.agent_id).copied())
                .ok_or_else(|| ToolError::InvalidArguments("no graph selected".into()))?;
            serde_json::to_string(
                state
                    .graphs
                    .get(&gid)
                    .ok_or_else(|| ToolError::InvalidArguments("graph not found".into()))?,
            )
            .map_err(|e| ToolError::Failed(e.to_string()))
        }
        Mode::Create | Mode::Init => {
            require_write(&ctx)?;
            let title = args
                .title
                .or(args.key)
                .unwrap_or_else(|| "Checklist".into());
            let owner = ctx.agent_id.clone();
            let items = args.items;
            let result = session
                .mutate_work(move |state| {
                    let mut graph = WorkGraph::new(title, Some(owner.clone()), GraphMode::Advisory);
                    graph.objective = args.objective;
                    for (index, title) in items.into_iter().enumerate() {
                        let node = crate::work::WorkNode::new(format!("item-{}", index + 1), title);
                        graph.view_order.push(node.id);
                        graph.nodes.insert(node.id, node);
                    }
                    let gid = graph.id;
                    state.create_graph(graph.clone(), None)?;
                    state.active_graph_by_agent.insert(owner.clone(), gid);
                    state.revision = state.revision.saturating_add(1);
                    Ok((gid, WorkEvent::GraphCreated { graph }))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!(
                "created graph_id={} revision={}",
                result,
                session.work.read().unwrap().graphs[&result].revision
            ))
        }
        mode => {
            require_write(&ctx)?;
            let gid = graph_id(&args.graph_id)?;
            let expected = args.expected_revision.ok_or_else(|| {
                ToolError::InvalidArguments("mutations require 'expected_revision'".into())
            })?;
            let context = auth(&ctx, false);
            let result = session
                .mutate_work(move |state| {
                    let graph = state.graph(gid)?.clone();
                    let (value, event) = match mode {
                        Mode::Add => {
                            let title = args.title.clone().ok_or_else(|| {
                                crate::work::WorkError::InvalidGraph("add requires 'title'".into())
                            })?;
                            let key = args
                                .key
                                .clone()
                                .unwrap_or_else(|| format!("item-{}", graph.nodes.len() + 1));
                            let new_node = state.add_node(
                                gid,
                                expected,
                                &context,
                                crate::work::NodeInput {
                                    key,
                                    title,
                                    description: args.description.clone(),
                                },
                            )?;
                            (
                                (),
                                WorkEvent::NodeChanged {
                                    graph_id: gid,
                                    node: state.graph(gid)?.nodes[&new_node].clone(),
                                },
                            )
                        }
                        _ => {
                            let nid = node_id(&graph, &args)
                                .map_err(|e| crate::work::WorkError::InvalidGraph(e.to_string()))?;
                            match mode {
                                Mode::Update => {
                                    state.update_node(
                                        gid,
                                        expected,
                                        &context,
                                        nid,
                                        args.title,
                                        Some(args.description),
                                    )?;
                                    (
                                        (),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Move => {
                                    let before = args
                                        .before_node_id
                                        .as_deref()
                                        .map(|v| {
                                            NodeId::parse(v).map_err(|e| {
                                                crate::work::WorkError::InvalidGraph(e.to_string())
                                            })
                                        })
                                        .transpose()?;
                                    state.move_node(gid, expected, &context, nid, before)?;
                                    (
                                        (),
                                        WorkEvent::GraphChanged {
                                            graph_id: gid,
                                            revision: state.graph(gid)?.revision,
                                        },
                                    )
                                }
                                Mode::Start => {
                                    let attempt = state.start(
                                        gid,
                                        expected,
                                        &context,
                                        nid,
                                        Some(ctx.agent_id.clone()),
                                    )?;
                                    (
                                        (),
                                        WorkEvent::AttemptChanged {
                                            graph_id: gid,
                                            attempt: state.graph(gid)?.attempts[&attempt].clone(),
                                        },
                                    )
                                }
                                Mode::Complete => {
                                    let result = state.complete(
                                        gid,
                                        expected,
                                        &context,
                                        nid,
                                        args.summary.unwrap_or_else(|| "completed".into()),
                                        args.evidence,
                                    )?;
                                    (
                                        (),
                                        WorkEvent::ResultRecorded {
                                            graph_id: gid,
                                            result: state.graph(gid)?.results[&result].clone(),
                                        },
                                    )
                                }
                                Mode::Fail => {
                                    let outcome = match args.outcome.as_deref() {
                                        Some("test_failed") => Outcome::TestFailed,
                                        Some("blocked") => Outcome::Blocked,
                                        Some("cancelled") => Outcome::Cancelled,
                                        Some(other) => Outcome::Custom(other.into()),
                                        None => Outcome::Failure,
                                    };
                                    let result = state.fail(
                                        gid,
                                        expected,
                                        &context,
                                        nid,
                                        outcome,
                                        args.summary.unwrap_or_else(|| "failed".into()),
                                        args.evidence,
                                    )?;
                                    (
                                        (),
                                        WorkEvent::ResultRecorded {
                                            graph_id: gid,
                                            result: state.graph(gid)?.results[&result].clone(),
                                        },
                                    )
                                }
                                Mode::Block => {
                                    state.block(gid, expected, &context, nid)?;
                                    (
                                        (),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Unblock => {
                                    state.unblock(gid, expected, &context, nid)?;
                                    (
                                        (),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Cancel => {
                                    state.cancel(gid, expected, &context, nid)?;
                                    (
                                        (),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                Mode::Retry => {
                                    state.retry(gid, expected, &context, nid)?;
                                    (
                                        (),
                                        WorkEvent::NodeChanged {
                                            graph_id: gid,
                                            node: state.graph(gid)?.nodes[&nid].clone(),
                                        },
                                    )
                                }
                                _ => unreachable!(),
                            }
                        }
                    };
                    Ok((value, event))
                })
                .map_err(ToolError::Failed)?;
            Ok(format!(
                "task mutation committed graph_id={} revision={}",
                gid,
                session.work.read().unwrap().graphs[&gid].revision
            ))
        }
    }
}
