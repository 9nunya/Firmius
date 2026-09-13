//! The `grep` tool: fast, parallel, gitignore-aware content search.
//!
//! Walks with the `ignore` crate (parallel, respects .gitignore) and matches
//! each file's lines with `regex`. Not a `ripgrep` subprocess shell-out —
//! everything stays in-process so results are structured and cancellable.

use schemars::JsonSchema;
use serde::Deserialize;
use std::io::{BufRead, BufReader};
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;

use globset::Glob;
use ignore::WalkBuilder;
use regex::{Regex, RegexBuilder};

use crate::artifact::{SessionArtifacts, is_artifact_path, normalize_artifact_dir};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::path;
use super::{flex, session_artifacts};

const DEFAULT_LIMIT: usize = 200;
/// Skip files above this size rather than regex-scanning them line by line;
/// keeps a stray binary/log file from stalling a search.
const MAX_FILE_BYTES: u64 = 8 * 1024 * 1024;

#[derive(Deserialize, JsonSchema)]
struct GrepArgs {
    /// Regex pattern to search for (Rust `regex` syntax).
    pattern: String,
    /// Directory to search from. Defaults to the working directory.
    #[serde(default)]
    path: Option<String>,
    /// Only search files matching this glob, e.g. "*.rs".
    #[serde(default)]
    glob: Option<String>,
    /// Case-insensitive matching. Default false.
    #[serde(default, deserialize_with = "flex::bool_")]
    ignore_case: bool,
    /// Lines of context before/after each match. Default 0.
    #[serde(default, deserialize_with = "flex::usize_")]
    context: usize,
    /// Max number of matching lines to return. Default 200.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    limit: Option<usize>,
}

pub fn register_grep_tool(r: &ToolRegistry) -> &ToolRegistry {
    r.register(
        TypedTool::new(
            "grep",
            "Search file contents with a regex pattern. Fast, parallel, gitignore-aware. \
         Optionally filter by glob (e.g. glob=\"*.rs\") and request surrounding context lines. \
         Returns `path:line: text` per match. Set path=\"artifact://...\" to search session \
         artifacts.",
            |a: GrepArgs, ctx: ToolContext| {
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
                        if !ctx.workspace().is_local() {
                            run_remote(a, ctx).await
                        } else {
                            tokio::task::spawn_blocking(move || run(a, ctx))
                                .await
                                .unwrap()
                        }
                    }
                })
            },
        )
        .with_required_scopes(["fs_read"]),
    );
    r
}

async fn run_remote(args: GrepArgs, ctx: ToolContext) -> Result<String, ToolError> {
    let regex = RegexBuilder::new(&args.pattern)
        .case_insensitive(args.ignore_case)
        .build()
        .map_err(|e| ToolError::InvalidArguments(format!("bad pattern '{}': {e}", args.pattern)))?;
    let matcher = args
        .glob
        .as_deref()
        .map(|g| {
            Glob::new(g)
                .map(|g| g.compile_matcher())
                .map_err(|e| ToolError::InvalidArguments(format!("bad glob '{g}': {e}")))
        })
        .transpose()?;
    let limit = args.limit.unwrap_or(DEFAULT_LIMIT);
    let mut out = Vec::new();
    let files = ctx
        .workspace()
        .find_files(&ctx.workdir, args.path.as_deref(), false)
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    for file in files {
        if out.len() >= limit {
            break;
        }
        if matcher.as_ref().is_some_and(|m| !m.is_match(&file)) {
            continue;
        }
        let Ok(bytes) = ctx.workspace().read(&ctx.workdir, &file).await else {
            continue;
        };
        if bytes.len() as u64 > MAX_FILE_BYTES || bytes.contains(&0) {
            continue;
        }
        let Ok(content) = String::from_utf8(bytes) else {
            continue;
        };
        let lines: Vec<&str> = content.lines().collect();
        for (i, line) in lines.iter().enumerate() {
            if out.len() >= limit {
                break;
            }
            if !regex.is_match(line) {
                continue;
            }
            if args.context > 0 {
                for (j, ctx_line) in lines[i.saturating_sub(args.context)..i].iter().enumerate() {
                    out.push(format!(
                        "{}-{}: {}",
                        file,
                        i.saturating_sub(args.context) + j + 1,
                        ctx_line
                    ));
                }
            }
            out.push(format!("{}:{}: {}", file, i + 1, line));
            let end = (i + 1 + args.context).min(lines.len());
            for (j, ctx_line) in lines[i + 1..end].iter().enumerate() {
                out.push(format!("{}-{}: {}", file, i + 2 + j, ctx_line));
            }
        }
    }
    if out.is_empty() {
        return Ok("no matches".into());
    }
    let mut text = out.join("\n");
    if out.len() >= limit {
        text.push_str(&format!("\n[...truncated at {limit} matches...]"));
    }
    Ok(text)
}

fn run_artifacts(args: GrepArgs, store: Arc<SessionArtifacts>) -> Result<String, ToolError> {
    let dir = normalize_artifact_dir(args.path.as_deref().unwrap_or(""))
        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
    let regex = RegexBuilder::new(&args.pattern)
        .case_insensitive(args.ignore_case)
        .build()
        .map_err(|e| ToolError::InvalidArguments(format!("bad pattern '{}': {e}", args.pattern)))?;
    let glob_matcher = args
        .glob
        .as_deref()
        .map(|g| {
            Glob::new(g)
                .map(|g| g.compile_matcher())
                .map_err(|e| ToolError::InvalidArguments(format!("bad glob '{g}': {e}")))
        })
        .transpose()?;

    let limit = args.limit.unwrap_or(DEFAULT_LIMIT);
    let context = args.context;
    let mut remaining = limit;
    let mut out: Vec<String> = Vec::new();

    for path in store.list(&dir) {
        if remaining == 0 {
            break;
        }
        if let Some(matcher) = &glob_matcher
            && !matcher.is_match(&path)
        {
            continue;
        }
        let Ok(content) = store.read(&path) else {
            continue;
        };
        let lines: Vec<&str> = content.lines().collect();
        let display = format!("artifact://{path}");
        for (i, line) in lines.iter().enumerate() {
            if remaining == 0 {
                break;
            }
            if !regex.is_match(line) {
                continue;
            }
            if context > 0 {
                let start = i.saturating_sub(context);
                for ctx_line in &lines[start..i] {
                    out.push(format!("{display}-{}: {ctx_line}", start + 1));
                }
            }
            out.push(format!("{display}:{}: {line}", i + 1));
            if context > 0 {
                let end = (i + 1 + context).min(lines.len());
                for (offset, ctx_line) in lines[i + 1..end].iter().enumerate() {
                    out.push(format!("{display}-{}: {ctx_line}", i + 2 + offset));
                }
            }
            remaining = remaining.saturating_sub(1);
        }
    }

    if out.is_empty() {
        return Ok("no matches".to_string());
    }
    let mut text = out.join("\n");
    if remaining == 0 {
        text.push_str(&format!("\n[...truncated at {limit} matches...]"));
    }
    Ok(text)
}

fn run(args: GrepArgs, ctx: ToolContext) -> Result<String, ToolError> {
    let root = path::directory_read(&ctx.workdir, args.path.as_deref())
        .map_err(ToolError::InvalidArguments)?;

    let regex = RegexBuilder::new(&args.pattern)
        .case_insensitive(args.ignore_case)
        .build()
        .map_err(|e| ToolError::InvalidArguments(format!("bad pattern '{}': {e}", args.pattern)))?;

    let glob_matcher = args
        .glob
        .as_deref()
        .map(|g| {
            Glob::new(g)
                .map(|g| g.compile_matcher())
                .map_err(|e| ToolError::InvalidArguments(format!("bad glob '{g}': {e}")))
        })
        .transpose()?;

    let limit = args.limit.unwrap_or(DEFAULT_LIMIT);
    let context = args.context;

    let walker = WalkBuilder::new(&root)
        .hidden(false)
        .threads(num_cpus())
        .build_parallel();

    let (tx, rx) = mpsc::channel::<Vec<String>>();
    let remaining = std::sync::Arc::new(AtomicUsize::new(limit));

    walker.run(|| {
        let tx = tx.clone();
        let root = root.clone();
        let regex = regex.clone();
        let glob_matcher = glob_matcher.clone();
        let remaining = remaining.clone();
        Box::new(move |entry| {
            use ignore::WalkState;
            if remaining.load(Ordering::Relaxed) == 0 {
                return WalkState::Quit;
            }
            let Ok(entry) = entry else {
                return WalkState::Continue;
            };
            if !entry.file_type().is_some_and(|t| t.is_file()) {
                return WalkState::Continue;
            }
            let path = entry.path();
            if let Some(m) = &glob_matcher {
                let rel = path.strip_prefix(&root).unwrap_or(path);
                if !m.is_match(rel) {
                    return WalkState::Continue;
                }
            }
            if entry.metadata().map(|m| m.len()).unwrap_or(0) > MAX_FILE_BYTES {
                return WalkState::Continue;
            }
            if let Some(hits) = search_file(path, &root, &regex, context, &remaining) {
                let _ = tx.send(hits);
            }
            if remaining.load(Ordering::Relaxed) == 0 {
                WalkState::Quit
            } else {
                WalkState::Continue
            }
        })
    });
    drop(tx);

    let mut out: Vec<String> = rx.into_iter().flatten().collect();
    let truncated = remaining.load(Ordering::Relaxed) == 0;
    out.sort();

    if out.is_empty() {
        return Ok("no matches".to_string());
    }
    let mut text = out.join("\n");
    if truncated {
        text.push_str(&format!("\n[...truncated at {limit} matches...]"));
    }
    Ok(text)
}

/// Line-scan one file, returning formatted `path:line: text` hits (plus
/// `-`-prefixed context lines when requested), respecting the shared
/// `remaining` match budget so we stop early across the whole walk.
fn search_file(
    path: &Path,
    root: &Path,
    regex: &Regex,
    context: usize,
    remaining: &AtomicUsize,
) -> Option<Vec<String>> {
    let mut file = std::fs::File::open(path).ok()?;
    // Binary sniff: bail on a NUL in the first 8KB, cheap and reliable enough.
    {
        use std::io::Read;
        let mut probe = [0u8; 8192];
        let n = file.read(&mut probe).ok()?;
        if probe[..n].contains(&0) {
            return None;
        }
    }
    use std::io::Seek;
    file.seek(std::io::SeekFrom::Start(0)).ok()?;
    let reader = BufReader::new(file);
    let rel = path.strip_prefix(root).unwrap_or(path).display();

    let lines: Vec<String> = reader.lines().map_while(Result::ok).collect();
    let mut hits = Vec::new();
    for (i, line) in lines.iter().enumerate() {
        if remaining.load(Ordering::Relaxed) == 0 {
            break;
        }
        if regex.is_match(line) {
            // Claim the global budget before emitting this hit. Multiple
            // walker threads may race here; a load followed by a decrement
            // would otherwise allow each thread to emit past the limit.
            if remaining
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_sub(1))
                .is_err()
            {
                break;
            }
            if context > 0 {
                let start = i.saturating_sub(context);
                for ctx_line in &lines[start..i] {
                    hits.push(format!("{rel}-{}: {ctx_line}", start + 1));
                }
            }
            hits.push(format!("{rel}:{}: {line}", i + 1));
            if context > 0 {
                let end = (i + 1 + context).min(lines.len());
                for (off, ctx_line) in lines[i + 1..end].iter().enumerate() {
                    hits.push(format!("{rel}-{}: {ctx_line}", i + 2 + off));
                }
            }
        }
    }
    if hits.is_empty() { None } else { Some(hits) }
}

fn num_cpus() -> usize {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
}
