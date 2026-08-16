use schemars::JsonSchema;

use crate::{ToolError, ToolRegistry, TypedTool};

#[derive(serde::Deserialize, JsonSchema)]
struct ReadArgs {
    path: String,
}

pub fn register_read_tool(r: &mut ToolRegistry) -> &mut ToolRegistry {
    r.register(TypedTool::new(
        "read",
        "Read a UTF-8 file",
        |a: ReadArgs, _| {
            Box::pin(async move {
                tokio::fs::read_to_string(a.path)
                    .await
                    .map_err(|e| ToolError::Failed(e.to_string()))
            })
        },
    ));
    r
}
