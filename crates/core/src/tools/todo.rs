//! Self-only model-facing access to native todos.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

use crate::todo::{
    EvidenceAttachment, NewTodoItem, TodoCycleId, TodoError, TodoIntent, TodoItem, TodoItemId,
    TodoItemPatch, TodoItemStatus, TodoLedger,
};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

pub const TODO_READ_SCOPE: &str = "todo_read";
pub const TODO_WRITE_SCOPE: &str = "todo_write";
pub const TODO_OBSERVE_SCOPE: &str = "todo_observe";

#[derive(Debug, Clone, Copy, Deserialize, JsonSchema, Default)]
#[serde(rename_all = "snake_case")]
enum Action {
    #[default]
    View,
    Begin,
    Add,
    Update,
    Complete,
    CompleteMany,
    Cancel,
    Block,
    Assess,
    AttachEvidence,
    Archive,
}

#[derive(Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct TodoArgs {
    /// Operation. View is the default.
    #[serde(default)]
    action: Action,
    /// Optional compare-and-swap revision. When omitted, the daemon resolves
    /// the current revision while holding the mutation transaction.
    #[serde(default)]
    expected_revision: Option<u64>,
    /// Compact view excludes archived cycles and includes stable text rendering.
    #[serde(default)]
    compact: Option<bool>,
    /// Cycle intent for begin.
    #[serde(default)]
    intent: Option<String>,
    #[serde(default)]
    /// Initial checklist entries for begin. Each criterion is atomically
    /// created as a pending todo item and also retained as cycle criteria.
    completion_criteria: Vec<String>,
    /// Existing item for item mutations.
    #[serde(default)]
    item_id: Option<String>,
    /// Existing items for complete_many. The batch is atomic and shares the
    /// optional outcome string.
    #[serde(default)]
    item_ids: Vec<String>,
    /// Existing cycle for archive.
    #[serde(default)]
    cycle_id: Option<String>,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    description: Option<String>,
    #[serde(default)]
    evidence_required: Option<bool>,
    /// For update: pending, in_progress, or blocked. Use dedicated complete/cancel actions for terminal states.
    #[serde(default)]
    status: Option<TodoItemStatusArg>,
    /// Observed outcome for complete, or required reason for cancel/block.
    #[serde(default)]
    outcome: Option<String>,
    #[serde(default)]
    reason: Option<String>,
    /// Evidence kind/reference/summary for attach_evidence.
    #[serde(default)]
    evidence_kind: Option<String>,
    #[serde(default)]
    evidence_reference: Option<String>,
    #[serde(default)]
    evidence_summary: Option<String>,
}

#[derive(Debug, Clone, Copy, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum TodoItemStatusArg {
    Pending,
    InProgress,
    Blocked,
}
impl From<TodoItemStatusArg> for TodoItemStatus {
    fn from(value: TodoItemStatusArg) -> Self {
        match value {
            TodoItemStatusArg::Pending => Self::Pending,
            TodoItemStatusArg::InProgress => Self::InProgress,
            TodoItemStatusArg::Blocked => Self::Blocked,
        }
    }
}

#[derive(Serialize)]
struct Response<T: Serialize> {
    revision: u64,
    result: T,
}

#[derive(Serialize)]
struct BeginResult {
    cycle_id: TodoCycleId,
    items: Vec<TodoItem>,
}

fn begin_with_criteria(
    ledger: &mut TodoLedger,
    expected_revision: u64,
    intent: TodoIntent,
) -> Result<BeginResult, TodoError> {
    let item_titles = intent.completion_criteria.clone();
    let cycle_id = ledger.begin(expected_revision, intent)?;
    let mut items = Vec::with_capacity(item_titles.len());
    for title in item_titles {
        items.push(ledger.add(
            ledger.revision(),
            NewTodoItem {
                title,
                description: None,
                evidence_required: false,
            },
        )?);
    }
    Ok(BeginResult { cycle_id, items })
}

fn required<T>(value: Option<T>, name: &str) -> Result<T, ToolError> {
    value
        .ok_or_else(|| ToolError::InvalidArguments(format!("'{name}' is required for this action")))
}
fn item(args: &TodoArgs) -> Result<TodoItemId, ToolError> {
    required(args.item_id.clone(), "item_id").map(TodoItemId)
}
fn cycle(args: &TodoArgs) -> Result<TodoCycleId, ToolError> {
    required(args.cycle_id.clone(), "cycle_id").map(TodoCycleId)
}

fn has_scope(ctx: &ToolContext, scope: &str) -> bool {
    ctx.allowed_scopes
        .as_ref()
        .is_none_or(|scopes| scopes.contains(scope))
}
fn authorize(action: Action, ctx: &ToolContext) -> Result<(), ToolError> {
    let required = match action {
        Action::View => TODO_READ_SCOPE,
        _ => TODO_WRITE_SCOPE,
    };
    if has_scope(ctx, required) {
        Ok(())
    } else {
        Err(ToolError::PermissionDenied {
            tool: "todo".into(),
            required: vec![required.into()],
            allowed: ctx
                .allowed_scopes
                .as_ref()
                .map(|v| v.iter().cloned().collect())
                .unwrap_or_default(),
        })
    }
}

async fn todo(args: TodoArgs, ctx: ToolContext) -> Result<String, ToolError> {
    authorize(args.action, &ctx)?;
    let session = ctx.session.as_ref().ok_or_else(|| {
        ToolError::Failed("todo requires an attached session with durable state".into())
    })?;
    if session.id != ctx.session_id {
        return Err(ToolError::Failed(
            "todo session context does not match the attached session".into(),
        ));
    }
    if matches!(args.action, Action::View) {
        let ledger = session
            .agent_todo(&ctx.agent_id)
            .map_err(ToolError::Failed)?;
        let projection = ledger.project(args.compact.unwrap_or(true));
        return serde_json::to_string_pretty(&serde_json::json!({
            "revision": ledger.revision(), "projection": projection,
            "compact": projection.render_compact(), "evaluation": ledger.evaluate_completion()
        }))
        .map_err(|e| ToolError::Failed(e.to_string()));
    }
    let (result, revision) = session
        .mutate_agent_todo(&ctx.agent_id, |ledger| {
            // Resolve the default inside the serialized daemon mutation, not
            // from a caller-provided snapshot that may already be stale.
            let expected = args.expected_revision.unwrap_or(ledger.revision());
            let value = match args.action {
                Action::Begin => serde_json::to_value(begin_with_criteria(
                    ledger,
                    expected,
                    TodoIntent {
                        summary: required(args.intent, "intent").map_err(tool_to_todo)?,
                        completion_criteria: args.completion_criteria,
                    },
                )?),
                Action::Add => serde_json::to_value(ledger.add(
                    expected,
                    NewTodoItem {
                        title: required(args.title, "title").map_err(tool_to_todo)?,
                        description: args.description,
                        evidence_required: args.evidence_required.unwrap_or(false),
                    },
                )?),
                Action::Update => serde_json::to_value(ledger.update(
                    expected,
                    &item(&args).map_err(tool_to_todo)?,
                    TodoItemPatch {
                        title: args.title,
                        description: args.description,
                        evidence_required: args.evidence_required,
                        status: args.status.map(Into::into),
                    },
                )?),
                Action::Complete => serde_json::to_value(ledger.complete(
                    expected,
                    &item(&args).map_err(tool_to_todo)?,
                    args.outcome,
                )?),
                Action::CompleteMany => serde_json::to_value(
                    ledger.complete_many(
                        expected,
                        &args
                            .item_ids
                            .into_iter()
                            .map(TodoItemId)
                            .collect::<Vec<_>>(),
                        args.outcome,
                    )?,
                ),
                Action::Cancel => serde_json::to_value(ledger.cancel(
                    expected,
                    &item(&args).map_err(tool_to_todo)?,
                    required(args.reason, "reason").map_err(tool_to_todo)?,
                )?),
                Action::Block => serde_json::to_value(ledger.block(
                    expected,
                    &item(&args).map_err(tool_to_todo)?,
                    required(args.reason, "reason").map_err(tool_to_todo)?,
                )?),
                Action::Assess => serde_json::to_value(ledger.assess(expected)?),
                Action::AttachEvidence => serde_json::to_value(
                    ledger.attach_evidence(
                        expected,
                        EvidenceAttachment {
                            item_id: args.item_id.map(TodoItemId),
                            kind: required(args.evidence_kind, "evidence_kind")
                                .map_err(tool_to_todo)?,
                            reference: required(args.evidence_reference, "evidence_reference")
                                .map_err(tool_to_todo)?,
                            summary: args.evidence_summary,
                        },
                    )?,
                ),
                Action::Archive => {
                    ledger.archive(expected, &cycle(&args).map_err(tool_to_todo)?)?;
                    Ok(serde_json::json!({"archived": true}))
                }
                Action::View => unreachable!(),
            }
            .map_err(|error| {
                crate::todo::TodoError::Storage(format!("serialize response: {error}"))
            })?;
            Ok((value, ledger.revision()))
        })
        .map_err(map_session_error)?;
    serde_json::to_string_pretty(&Response { revision, result })
        .map_err(|e| ToolError::Failed(e.to_string()))
}

fn map_session_error(error: String) -> ToolError {
    const INVALID_PREFIXES: &[&str] = &[
        "stale todo revision:",
        "an active todo cycle already exists",
        "there is no active todo cycle",
        "todo cycle not found:",
        "todo cycle is still active:",
        "todo item not found:",
        "todo item is outside the active cycle:",
        "todo item is terminal:",
        "invalid todo:",
    ];
    if INVALID_PREFIXES
        .iter()
        .any(|prefix| error.starts_with(prefix))
    {
        ToolError::InvalidArguments(error)
    } else {
        ToolError::Failed(error)
    }
}

fn tool_to_todo(error: ToolError) -> crate::todo::TodoError {
    crate::todo::TodoError::Validation(error.to_string())
}

pub fn register_todo_tool(registry: &ToolRegistry) -> &ToolRegistry {
    registry.register(TypedTool::new(
        "todo",
        "Manage your own live native todo cycle. Identity is always derived from the authenticated ToolContext; another agent cannot be named. Use view to inspect the current revision; expected_revision is optional for mutations and is resolved by the daemon when omitted. Actions: view, begin, add, update, complete, complete_many, cancel, block, assess, attach_evidence, archive. Begin atomically creates one pending todo item for every completion_criteria entry and returns their ids; use add only for later items. Mark the current item in_progress, and complete items when their outcomes are established, before starting later substantive work. Use complete_many to atomically finish multiple items that genuinely become complete at the same boundary; do not defer earlier boundaries just to batch them at the end. Assess deterministically reports waiting/actionable/evidence deficits or records a final outcome when ready, then clears the assessed cycle so the next task starts with an empty ledger.",
        |args: TodoArgs, ctx: ToolContext| Box::pin(todo(args, ctx)),
    ).with_visibility_scopes(&[TODO_READ_SCOPE, TODO_WRITE_SCOPE, TODO_OBSERVE_SCOPE]));
    registry
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn session_errors_preserve_argument_vs_storage_boundary() {
        assert!(matches!(
            map_session_error("stale todo revision: expected 1, actual 2".into()),
            ToolError::InvalidArguments(_)
        ));
        assert!(matches!(
            map_session_error("todo state is quarantined: malformed envelope".into()),
            ToolError::Failed(_)
        ));
        assert!(matches!(
            map_session_error("persist session: disk full".into()),
            ToolError::Failed(_)
        ));
    }

    #[test]
    fn schema_has_no_identity_fields_and_visibility_is_scoped() {
        let registry = ToolRegistry::default();
        register_todo_tool(&registry);
        let definition = registry
            .definitions()
            .into_iter()
            .find(|d| d.name == "todo")
            .unwrap();
        assert!(definition.description.contains("Use complete_many"));
        assert!(definition.description.contains("clears the assessed cycle"));
        let props = definition.input_schema["properties"].as_object().unwrap();
        for forbidden in ["agent_id", "session_id", "owner_agent_id"] {
            assert!(!props.contains_key(forbidden));
        }
        assert!(props.contains_key("item_ids"));
        let none = ["fs_read".to_string()].into_iter().collect();
        assert!(registry.definitions_scoped(Some(&none)).is_empty());
        let read = [TODO_READ_SCOPE.to_string()].into_iter().collect();
        assert_eq!(registry.definitions_scoped(Some(&read)).len(), 1);
    }

    #[test]
    fn begin_seeds_renderable_items_from_completion_criteria() {
        let mut ledger = TodoLedger::new("agent-a");
        let result = begin_with_criteria(
            &mut ledger,
            0,
            TodoIntent {
                summary: "understand compaction".into(),
                completion_criteria: vec![
                    "identify the data model".into(),
                    "trace the trigger".into(),
                    "verify behavior".into(),
                ],
            },
        )
        .unwrap();
        assert_eq!(result.items.len(), 3);
        assert_eq!(ledger.project(true).cycles[0].items.len(), 3);
        assert_eq!(ledger.revision(), 4);
        assert!(
            result
                .items
                .iter()
                .all(|item| item.status == TodoItemStatus::Pending)
        );
    }

    #[test]
    fn mutation_arguments_do_not_require_a_revision_snapshot() {
        let args: TodoArgs = serde_json::from_value(serde_json::json!({
            "action": "assess"
        }))
        .unwrap();
        assert_eq!(args.expected_revision, None);
    }
}
