//! The `glob` tool: find files by pattern, fast and gitignore-aware.
//!
//! Built on the `ignore` crate's parallel walker (the same engine ripgrep
//! uses), so `.gitignore`/`.ignore` rules are respected by default and large
//! trees don't stall a single thread.
//!
//! `path` may also address the session artifact namespace (`artifact://...`),
//! in which case matching runs against in-memory artifact paths instead of
//! the filesystem.

use std::sync::Arc;
use std::sync::mpsc;

use schemars::JsonSchema;
use serde::Deserialize;

use globset::Glob;
use ignore::WalkBuilder;

use crate::artifact::{SessionArtifacts, is_artifact_path, normalize_artifact_dir};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::{flex, session_artifacts};

const DEFAULT_LIMIT: usize = 500;

#[derive(Deserialize, JsonSchema)]
struct GlobArgs {
    /// Glob pattern, e.g. "**/*.rs" or "src/**/test_*.py".
    pattern: String,
    /// Directory to search from. Defaults to the working directory.
    #[serde(default)]
    path: Option<String>,
    /// Include files normally hidden by .gitignore/.ignore. Default false.
    #[serde(default, deserialize_with = "flex::bool_")]
    include_ignored: bool,
    /// Max number of matches to return. Default 500.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    limit: Option<usize>,
}

pub fn register_glob_tool(r: &mut ToolRegistry) -> &mut ToolRegistry {
    r.register(
        TypedTool::new(
            "glob",
            "Find files matching a glob pattern (e.g. '**/*.rs'). Fast, parallel, \
         gitignore-aware by default. Results are relative paths, newest-first \
         is not guaranteed — for content search use `grep` instead. Set \
         path=\"artifact://...\" to match session artifact paths.",
            |a: GlobArgs, ctx: ToolContext| {
                Box::pin(async move {
                    if a.path.as_deref().is_some_and(is_artifact_path) {
                        let store = session_artifacts(&ctx).await.ok_or_else(|| {
                            ToolError::Failed(
                                "artifacts are unavailable: this agent is not attached to a session"
                                    .into(),
                            )
                        })?;
                        tokio::task::spawn_blocking(move || run_artifacts(a, store))
                            .await
                            .unwrap()
                    } else {
                        tokio::task::spawn_blocking(move || run(a, ctx))
                            .await
                            .unwrap()
                    }
                })
            },
        )
        .with_required_scopes(["fs_read"]),
    );
    r
}

fn run_artifacts(args: GlobArgs, store: Arc<SessionArtifacts>) -> Result<String, ToolError> {
    let dir = normalize_artifact_dir(args.path.as_deref().unwrap_or(""))
        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
    let matcher = Glob::new(&args.pattern)
        .map_err(|e| ToolError::InvalidArguments(format!("bad pattern '{}': {e}", args.pattern)))?
        .compile_matcher();
    let limit = args.limit.unwrap_or(DEFAULT_LIMIT);

    let mut results: Vec<String> = store
        .list(&dir)
        .into_iter()
        .filter(|path| matcher.is_match(path))
        .collect();
    results.sort();

    let truncated = results.len() > limit;
    results.truncate(limit);

    if results.is_empty() {
        return Ok("no matches".to_string());
    }
    let mut out: Vec<String> = results
        .into_iter()
        .map(|path| format!("artifact://{path}"))
        .collect();
    if truncated {
        out.push(format!("[...truncated at {limit} matches...]"));
    }
    Ok(out.join("\n"))
}

fn run(args: GlobArgs, ctx: ToolContext) -> Result<String, ToolError> {
    let root = args
        .path
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| ctx.workdir.clone());
    let root = if root.is_absolute() {
        root
    } else {
        ctx.workdir.join(root)
    };

    let matcher = Glob::new(&args.pattern)
        .map_err(|e| ToolError::InvalidArguments(format!("bad pattern '{}': {e}", args.pattern)))?
        .compile_matcher();
    let limit = args.limit.unwrap_or(DEFAULT_LIMIT);

    let walker = WalkBuilder::new(&root)
        .hidden(false)
        .ignore(!args.include_ignored)
        .git_ignore(!args.include_ignored)
        .git_global(!args.include_ignored)
        .git_exclude(!args.include_ignored)
        .threads(num_cpus())
        .build_parallel();

    let (tx, rx) = mpsc::channel::<String>();
    walker.run(|| {
        let tx = tx.clone();
        let root = root.clone();
        let matcher = matcher.clone();
        Box::new(move |entry| {
            use ignore::WalkState;
            let Ok(entry) = entry else {
                return WalkState::Continue;
            };
            if entry.file_type().is_some_and(|t| t.is_file()) {
                let rel = entry.path().strip_prefix(&root).unwrap_or(entry.path());
                if matcher.is_match(rel) {
                    // Ignore send errors: the receiver may have hit `limit`
                    // and dropped, which is expected early-exit behavior.
                    let _ = tx.send(rel.to_string_lossy().into_owned());
                }
            }
            WalkState::Continue
        })
    });
    drop(tx);

    let mut results: Vec<String> = rx.into_iter().take(limit + 1).collect();
    let truncated = results.len() > limit;
    results.truncate(limit);
    results.sort();

    if results.is_empty() {
        return Ok("no matches".to_string());
    }
    let mut out = results.join("\n");
    if truncated {
        out.push_str(&format!("\n[...truncated at {limit} matches...]"));
    }
    Ok(out)
}

fn num_cpus() -> usize {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
}
