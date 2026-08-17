use schemars::JsonSchema;
use serde::Deserialize;
use tokio::io::{AsyncBufReadExt, BufReader};

use crate::artifact::{is_artifact_path, normalize_artifact_path};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::{flex, session_artifacts};

/// A generous safety ceiling for an unscoped read. Oversized tool results are
/// redirected by the agent loop, but refusing truly enormous files here also
/// avoids loading them into memory unnecessarily.
const MAX_READ_BYTES: u64 = 16 * 1024 * 1024;

#[derive(Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
struct ReadArgs {
    /// File to read. Relative paths are resolved from the agent's working directory.
    path: String,
    /// One-based first line. Defaults to 1. May be used without `limit` to read
    /// from that line through the end of the file.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    start_line: Option<usize>,
    /// Maximum number of lines to return. May be used without `start_line` to
    /// read from the beginning of the file.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    limit: Option<usize>,
    /// Legacy zero-based offset. Hidden from the generated schema.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    #[schemars(skip)]
    offset: Option<usize>,
    /// Legacy line count paired with `offset`. Hidden from the generated schema.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    #[schemars(skip)]
    max_lines: Option<usize>,
    /// Legacy inclusive last line paired with `start_line`. Hidden from the schema.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    #[schemars(skip)]
    end_line: Option<usize>,
}

pub fn register_read_tool(r: &mut ToolRegistry) -> &mut ToolRegistry {
    r.register(TypedTool::new(
        "read",
        "Read a UTF-8 file or session artifact. Usually provide only `path`. Relative paths use \
         the agent's working directory; `artifact://<path>` reads from session memory. For a \
         region, add one-based `start_line` and/or `limit`; either may be used alone.",
        |a: ReadArgs, ctx: ToolContext| Box::pin(read_file(a, ctx)),
    )
    .with_required_scopes(["fs_read"]));
    r
}

async fn read_file(a: ReadArgs, ctx: ToolContext) -> Result<String, ToolError> {
    let region = if a.offset.is_some() || a.max_lines.is_some() {
        if a.start_line.is_some() || a.limit.is_some() || a.end_line.is_some() {
            return Err(ToolError::InvalidArguments(
                "legacy offset + max_lines cannot be combined with start_line, limit, or end_line"
                    .into(),
            ));
        }
        match (a.offset, a.max_lines) {
            (Some(offset), Some(max_lines)) => Some((offset.saturating_add(1), max_lines)),
            _ => {
                return Err(ToolError::InvalidArguments(
                    "'offset' and 'max_lines' must be provided together".into(),
                ));
            }
        }
    } else if let Some(end) = a.end_line {
        if a.limit.is_some() {
            return Err(ToolError::InvalidArguments(
                "legacy end_line cannot be combined with limit".into(),
            ));
        }
        match a.start_line {
            Some(start) => {
                if start == 0 {
                    return Err(ToolError::InvalidArguments(
                        "'start_line' is one-based and must be at least 1".into(),
                    ));
                }
                if end < start {
                    return Err(ToolError::InvalidArguments(
                        "'end_line' must be greater than or equal to 'start_line'".into(),
                    ));
                }
                Some((start, end - start + 1))
            }
            None => {
                return Err(ToolError::InvalidArguments(
                    "'end_line' requires 'start_line'".into(),
                ));
            }
        }
    } else if a.start_line.is_some() || a.limit.is_some() {
        let start = a.start_line.unwrap_or(1);
        if start == 0 {
            return Err(ToolError::InvalidArguments(
                "'start_line' is one-based and must be at least 1".into(),
            ));
        }
        Some((start, a.limit.unwrap_or(usize::MAX)))
    } else {
        None
    };

    let path = std::path::PathBuf::from(&a.path);

    if is_artifact_path(&a.path) {
        let store = session_artifacts(&ctx).await.ok_or_else(|| {
            ToolError::Failed(
                "artifacts are unavailable: this agent is not attached to a session".into(),
            )
        })?;
        let artifact_path = normalize_artifact_path(&a.path)
            .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
        let content = store
            .read(&artifact_path)
            .map_err(|e| ToolError::Failed(e.to_string()))?;
        return Ok(apply_region(&content, region));
    }

    let path = if path.is_absolute() {
        path
    } else {
        ctx.workdir.join(path)
    };

    if let Some((first_line, max_lines)) = region {
        return read_region(&path, first_line, max_lines).await;
    }

    let metadata = tokio::fs::metadata(&path)
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    if metadata.len() > MAX_READ_BYTES {
        return Err(ToolError::InvalidArguments(format!(
            "file is {} bytes, exceeding the {} MiB unscoped read limit; use start_line and/or limit",
            metadata.len(),
            MAX_READ_BYTES / (1024 * 1024)
        )));
    }

    tokio::fs::read_to_string(&path)
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))
}

async fn read_region(
    path: &std::path::Path,
    first_line: usize,
    max_lines: usize,
) -> Result<String, ToolError> {
    if max_lines == 0 {
        return Ok(String::new());
    }

    let file = tokio::fs::File::open(path)
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    let mut lines = BufReader::new(file).lines();
    let mut line_number = 1usize;
    while line_number < first_line {
        if lines
            .next_line()
            .await
            .map_err(|e| ToolError::Failed(e.to_string()))?
            .is_none()
        {
            return Ok(String::new());
        }
        line_number += 1;
    }

    let mut output = String::new();
    for index in 0..max_lines {
        let Some(line) = lines
            .next_line()
            .await
            .map_err(|e| ToolError::Failed(e.to_string()))?
        else {
            break;
        };
        if index > 0 {
            output.push('\n');
        }
        output.push_str(&line);
    }
    Ok(output)
}

/// Slice an in-memory artifact the same way `read_region` slices a file:
/// one-based `first_line`, up to `max_lines` lines.
fn apply_region(content: &str, region: Option<(usize, usize)>) -> String {
    let Some((first_line, max_lines)) = region else {
        return content.to_string();
    };
    if max_lines == 0 {
        return String::new();
    }
    content
        .lines()
        .skip(first_line.saturating_sub(1))
        .take(max_lines)
        .collect::<Vec<_>>()
        .join("\n")
}
