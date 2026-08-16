use schemars::JsonSchema;

use crate::{ToolError, ToolRegistry, TypedTool};

#[derive(serde::Deserialize, JsonSchema)]
struct ListArgs {
    path: Option<String>,
}

pub fn register_list_tool(r: &mut ToolRegistry) -> &mut ToolRegistry {
    r.register(TypedTool::new(
        "list",
        "List a directory",
        |a: ListArgs, _| {
            Box::pin(async move {
                let mut d = tokio::fs::read_dir(a.path.unwrap_or_else(|| ".".into()))
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
    ));

    r
}
