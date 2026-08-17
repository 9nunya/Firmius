use schemars::JsonSchema;

use crate::artifact::{is_artifact_path, normalize_artifact_dir};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::session_artifacts;

#[derive(serde::Deserialize, JsonSchema)]
struct ListArgs {
    path: Option<String>,
}

pub fn register_list_tool(r: &ToolRegistry) -> &ToolRegistry {
    r.register(
        TypedTool::new(
            "list",
            "List a directory. Use path=\"artifact://\" (or an artifact subpath) to list session \
             artifacts instead of filesystem entries.",
            |a: ListArgs, ctx: ToolContext| {
                Box::pin(async move {
                    let path = a.path.unwrap_or_else(|| ".".into());
                    if is_artifact_path(&path) {
                        let store = session_artifacts(&ctx).await.ok_or_else(|| {
                            ToolError::Failed(
                            "artifacts are unavailable: this agent is not attached to a session"
                                .into(),
                        )
                        })?;
                        let dir = normalize_artifact_dir(&path)
                            .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                        let entries: Vec<String> = store
                            .list(&dir)
                            .into_iter()
                            .map(|path| format!("artifact://{path}"))
                            .collect();
                        return Ok(if entries.is_empty() {
                            "empty".to_string()
                        } else {
                            entries.join("\n")
                        });
                    }

                    let mut d = tokio::fs::read_dir(path)
                        .await
                        .map_err(|e| ToolError::Failed(e.to_string()))?;
                    let mut out = Vec::new();
                    while let Some(e) = d
                        .next_entry()
                        .await
                        .map_err(|e| ToolError::Failed(e.to_string()))?
                    {
                        out.push(e.file_name().to_string_lossy().into_owned());
                    }
                    Ok(out.join("\n"))
                })
            },
        )
        .with_required_scopes(["fs_read"]),
    );

    r
}
