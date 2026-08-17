use schemars::JsonSchema;
use std::sync::Arc;

use crate::artifact::{
    ArtifactSource, SessionArtifacts, is_artifact_path, normalize_artifact_path,
};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::session_artifacts;

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

#[derive(serde::Deserialize, JsonSchema)]
struct EditArgs {
    /// The full patch text in apply_patch format. Inspect the relevant file
    /// first, then use enough exact context in each hunk to make the intended
    /// match unique. Keep unrelated changes out of the patch.
    patch: String,
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

pub fn register_edit_tool(r: &ToolRegistry) -> &ToolRegistry {
    r.register(
        TypedTool::new(
            "edit",
            "\
Edit files using the apply_patch format. Inspect first, then submit one small,
targeted patch. The patch is applied relative to the agent workdir, and file
operations are performed in order. The patch language is a stripped-down,
file-oriented diff format:

*** Begin Patch
[ one or more file operations ]
*** End Patch

File operations:
  *** Add File: <path>        create a new file; every following line must start with '+'
  *** Delete File: <path>     remove an existing file
  *** Update File: <path>     patch an existing file with hunks
    *** Move to: <new-path>   (optional) rename the file
    @@ [header]               hunk anchor (class/function name)
     <context>                unchanged line (starts with space)
    -<removed>                line to remove
    +<added>                  line to add
    *** End of File           (optional) end of hunk block

Example:
*** Begin Patch
*** Add File: hello.txt
+Hello, world!
*** Update File: src/app.py
@@ def greet():
-print(\"Hi\")
+print(\"Hello, world!\")
*** End of File
*** End Patch

Rules and workflow:
  - Filesystem paths must be relative to the workdir. Absolute paths and '..'
    traversal are rejected.
  - Session artifacts use the `artifact://<path>` form and support Add, Delete,
    Update, and Move. To remove an artifact, use `*** Delete File:
    artifact://<name>`. Artifacts live in session memory, not the filesystem.
  - Use `*** Add File` only for new files, `*** Delete File` only when removal
    is intentional, and `*** Move to` when renaming an updated file.
  - In update hunks, unchanged lines start with one space. Copy context exactly
    and do not include line numbers from another diff format.
  - Make the smallest patch that solves the task. Do not rewrite whole files
    when a focused hunk is sufficient.
  - If a hunk fails, reread the current file and regenerate it with fresh
    context instead of guessing. Verify the result with a read or a test.

The tool reports every created, updated, deleted, or moved path. It does not
run formatters or tests automatically, so run the appropriate checks with the
bash tool afterward.",
            |a: EditArgs, ctx: ToolContext| {
                Box::pin(async move {
                    let ops = parse_patch(&a.patch).map_err(ToolError::InvalidArguments)?;
                    apply_patch(ops, &ctx).await
                })
            },
        )
        .with_required_scopes(["fs_write"]),
    );
    r
}

// ---------------------------------------------------------------------------
// Patch AST
// ---------------------------------------------------------------------------

enum FileOp {
    Add {
        path: String,
        content: String,
    },
    Delete {
        path: String,
    },
    Update {
        path: String,
        move_to: Option<String>,
        hunks: Vec<Hunk>,
    },
}

struct Hunk {
    header: Option<String>,
    lines: Vec<HunkLine>,
}

enum HunkLine {
    Context(String),
    Remove(String),
    Add(String),
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

fn parse_patch(input: &str) -> Result<Vec<FileOp>, String> {
    let lines: Vec<&str> = input.lines().collect();
    let mut ops: Vec<FileOp> = Vec::new();
    let mut i = 0;

    // Expect "*** Begin Patch"
    if lines.get(i).map(|l| l.trim()) != Some("*** Begin Patch") {
        return Err("patch must start with '*** Begin Patch'".into());
    }
    i += 1;

    while i < lines.len() {
        let line = lines[i].trim();
        if line == "*** End Patch" {
            break;
        }
        if let Some(rest) = line.strip_prefix("*** Add File:") {
            let path = rest.trim().to_string();
            let mut content = String::new();
            i += 1;
            while i < lines.len() {
                let l = lines[i];
                if let Some(text) = l.strip_prefix('+') {
                    if !content.is_empty() {
                        content.push('\n');
                    }
                    content.push_str(text);
                } else if is_file_op_header(l.trim()) || l.trim() == "*** End Patch" {
                    break;
                } else {
                    return Err(format!(
                        "line {} in Add File '{}': expected '+', got '{}'",
                        i + 1,
                        path,
                        l
                    ));
                }
                i += 1;
            }
            ops.push(FileOp::Add { path, content });
        } else if let Some(rest) = line.strip_prefix("*** Delete File:") {
            let path = rest.trim().to_string();
            i += 1;
            ops.push(FileOp::Delete { path });
        } else if let Some(rest) = line.strip_prefix("*** Update File:") {
            let path = rest.trim().to_string();
            i += 1;
            let mut move_to = None;
            if i < lines.len() && lines[i].trim().starts_with("*** Move to:") {
                move_to = Some(
                    lines[i]
                        .trim()
                        .strip_prefix("*** Move to:")
                        .unwrap()
                        .trim()
                        .to_string(),
                );
                i += 1;
            }
            let mut hunks: Vec<Hunk> = Vec::new();
            while i < lines.len() {
                let l = lines[i].trim();
                if is_file_op_header(l) || l == "*** End Patch" {
                    break;
                }
                if l == "*** End of File" {
                    i += 1;
                    continue;
                }
                if l.starts_with("@@") {
                    let header = l.strip_prefix("@@").unwrap().trim();
                    let header = if header.is_empty() {
                        None
                    } else {
                        Some(header.to_string())
                    };
                    i += 1;
                    let mut hunk_lines: Vec<HunkLine> = Vec::new();
                    while i < lines.len() {
                        let hl = lines[i];
                        if hl.starts_with("@@")
                            || hl.trim() == "*** End of File"
                            || is_file_op_header(hl.trim())
                            || hl.trim() == "*** End Patch"
                        {
                            break;
                        }
                        hunk_lines.push(if let Some(stripped) = hl.strip_prefix(' ') {
                            HunkLine::Context(stripped.to_string())
                        } else if let Some(stripped) = hl.strip_prefix('-') {
                            HunkLine::Remove(stripped.to_string())
                        } else if let Some(stripped) = hl.strip_prefix('+') {
                            HunkLine::Add(stripped.to_string())
                        } else {
                            return Err(format!(
                                "line {}: hunk line must start with ' ', '-', or '+', got '{}'",
                                i + 1,
                                hl
                            ));
                        });
                        i += 1;
                    }
                    if !hunk_lines.is_empty() {
                        hunks.push(Hunk {
                            header,
                            lines: hunk_lines,
                        });
                    }
                } else {
                    return Err(format!(
                        "line {}: expected '@@' hunk header, got '{}'",
                        i + 1,
                        l
                    ));
                }
            }
            if hunks.is_empty() {
                return Err(format!("Update File '{}' has no hunks", path));
            }
            ops.push(FileOp::Update {
                path,
                move_to,
                hunks,
            });
        } else {
            return Err(format!(
                "line {}: expected file operation header, got '{}'",
                i + 1,
                line
            ));
        }
    }

    if ops.is_empty() {
        return Err("patch contains no file operations".into());
    }
    Ok(ops)
}

fn is_file_op_header(line: &str) -> bool {
    line.starts_with("*** Add File:")
        || line.starts_with("*** Delete File:")
        || line.starts_with("*** Update File:")
}

// ---------------------------------------------------------------------------
// Path validation
// ---------------------------------------------------------------------------

fn validate_path(path: &str) -> Result<(), String> {
    if path.is_empty() {
        return Err("empty path".into());
    }
    if path.starts_with('/') {
        return Err(format!("absolute paths are not allowed: '{path}'"));
    }
    for component in path.split('/') {
        if component == ".." {
            return Err(format!("path traversal '..' is not allowed: '{path}'"));
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Applier
// ---------------------------------------------------------------------------

async fn require_artifacts(ctx: &ToolContext) -> Result<Arc<SessionArtifacts>, ToolError> {
    session_artifacts(ctx).await.ok_or_else(|| {
        ToolError::Failed(
            "artifacts are unavailable: this agent is not attached to a session".into(),
        )
    })
}

async fn apply_patch(ops: Vec<FileOp>, ctx: &ToolContext) -> Result<String, ToolError> {
    let mut report = Vec::new();

    for op in ops {
        match op {
            FileOp::Add { path, content } => {
                if is_artifact_path(&path) {
                    let store = require_artifacts(ctx).await?;
                    let artifact_path = normalize_artifact_path(&path)
                        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                    store
                        .write(
                            &artifact_path,
                            content,
                            Some(&ctx.agent_id),
                            ArtifactSource::Manual,
                        )
                        .map_err(|e| ToolError::Failed(e.to_string()))?;
                    report.push(format!("created artifact://{artifact_path}"));
                    continue;
                }
                validate_path(&path).map_err(ToolError::InvalidArguments)?;
                let dest = ctx.workdir.join(&path);
                if let Some(parent) = dest.parent() {
                    tokio::fs::create_dir_all(parent).await.map_err(|e| {
                        ToolError::Failed(format!("mkdir {}: {e}", parent.display()))
                    })?;
                }
                tokio::fs::write(&dest, &content)
                    .await
                    .map_err(|e| ToolError::Failed(format!("write {}: {e}", dest.display())))?;
                report.push(format!("created {}", path));
            }
            FileOp::Delete { path } => {
                if is_artifact_path(&path) {
                    let store = require_artifacts(ctx).await?;
                    let artifact_path = normalize_artifact_path(&path)
                        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                    store
                        .remove(&artifact_path)
                        .map_err(|e| ToolError::Failed(e.to_string()))?;
                    report.push(format!("deleted artifact://{artifact_path}"));
                    continue;
                }
                validate_path(&path).map_err(ToolError::InvalidArguments)?;
                let dest = ctx.workdir.join(&path);
                tokio::fs::remove_file(&dest)
                    .await
                    .map_err(|e| ToolError::Failed(format!("delete {}: {e}", dest.display())))?;
                report.push(format!("deleted {}", path));
            }
            FileOp::Update {
                path,
                move_to,
                hunks,
            } => {
                if is_artifact_path(&path) {
                    let store = require_artifacts(ctx).await?;
                    let artifact_path = normalize_artifact_path(&path)
                        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                    let original = store
                        .read(&artifact_path)
                        .map_err(|e| ToolError::Failed(e.to_string()))?;
                    let new_content = apply_hunks(&original, &hunks)
                        .map_err(|e| ToolError::Failed(format!("patch {}: {e}", path)))?;

                    let (write_path, move_to_path) = match move_to {
                        Some(ref mt) if !is_artifact_path(mt) => {
                            return Err(ToolError::InvalidArguments(
                                "cannot move an artifact to the filesystem".into(),
                            ));
                        }
                        Some(mt) => {
                            let dst = normalize_artifact_path(&mt)
                                .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                            (dst.clone(), Some((artifact_path.clone(), dst)))
                        }
                        None => (artifact_path.clone(), None),
                    };
                    store
                        .write(
                            &write_path,
                            new_content,
                            Some(&ctx.agent_id),
                            ArtifactSource::Manual,
                        )
                        .map_err(|e| ToolError::Failed(e.to_string()))?;
                    if let Some((src, dst)) = move_to_path {
                        if src != dst {
                            store
                                .remove(&src)
                                .map_err(|e| ToolError::Failed(e.to_string()))?;
                            report.push(format!("moved artifact://{src} -> artifact://{dst}"));
                        } else {
                            report.push(format!("updated artifact://{src}"));
                        }
                    } else {
                        report.push(format!("updated artifact://{artifact_path}"));
                    }
                    continue;
                }

                if move_to.as_deref().is_some_and(is_artifact_path) {
                    return Err(ToolError::InvalidArguments(
                        "cannot move a filesystem file to the artifact namespace".into(),
                    ));
                }
                validate_path(&path).map_err(ToolError::InvalidArguments)?;
                let src = ctx.workdir.join(&path);
                let original = tokio::fs::read_to_string(&src)
                    .await
                    .map_err(|e| ToolError::Failed(format!("read {}: {e}", src.display())))?;
                let new_content = apply_hunks(&original, &hunks)
                    .map_err(|e| ToolError::Failed(format!("patch {}: {e}", path)))?;
                let write_path = if let Some(ref mt) = move_to {
                    validate_path(mt).map_err(ToolError::InvalidArguments)?;
                    ctx.workdir.join(mt)
                } else {
                    src.clone()
                };
                if let Some(parent) = write_path.parent() {
                    tokio::fs::create_dir_all(parent).await.map_err(|e| {
                        ToolError::Failed(format!("mkdir {}: {e}", parent.display()))
                    })?;
                }
                tokio::fs::write(&write_path, &new_content)
                    .await
                    .map_err(|e| {
                        ToolError::Failed(format!("write {}: {e}", write_path.display()))
                    })?;
                if let Some(mt) = &move_to {
                    // Remove original after successful write to new location.
                    let _ = tokio::fs::remove_file(&src).await;
                    report.push(format!("moved {} -> {}", path, mt));
                } else {
                    report.push(format!("updated {}", path));
                }
            }
        }
    }

    Ok(report.join("\n"))
}

// ---------------------------------------------------------------------------
// Hunk application
// ---------------------------------------------------------------------------

fn apply_hunks(original: &str, hunks: &[Hunk]) -> Result<String, String> {
    let mut result = original.to_string();

    for hunk in hunks {
        result = apply_one_hunk(&result, hunk)?;
    }

    Ok(result)
}

fn apply_one_hunk(original: &str, hunk: &Hunk) -> Result<String, String> {
    let orig_lines: Vec<&str> = original.lines().collect();

    // Build the search pattern from context + removal lines.
    let search_lines: Vec<&str> = hunk
        .lines
        .iter()
        .filter_map(|l| match l {
            HunkLine::Context(s) | HunkLine::Remove(s) => Some(s.as_str()),
            HunkLine::Add(_) => None,
        })
        .collect();

    if search_lines.is_empty() {
        return Err("hunk has no context or removal lines to match against".into());
    }

    let anchor = hunk.header.as_deref();

    // Find the best match position.
    let pos = find_match(&orig_lines, &search_lines, anchor)?;
    let end = pos + search_lines.len();

    // Build replacement: context lines are kept, add lines are inserted,
    // remove lines are skipped.
    let mut replacement: Vec<String> = Vec::new();
    for line in &hunk.lines {
        match line {
            HunkLine::Context(s) => replacement.push(s.clone()),
            HunkLine::Remove(_) => {} // skip
            HunkLine::Add(s) => replacement.push(s.clone()),
        }
    }

    // Assemble result.
    let mut out: Vec<String> = Vec::with_capacity(orig_lines.len());
    out.extend(orig_lines[..pos].iter().map(|s| s.to_string()));
    out.extend(replacement);
    out.extend(orig_lines[end..].iter().map(|s| s.to_string()));

    Ok(out.join("\n"))
}

/// Find the position in `orig_lines` where `search` matches, using `anchor` as
/// a narrowing hint if provided.
fn find_match(orig_lines: &[&str], search: &[&str], anchor: Option<&str>) -> Result<usize, String> {
    // If anchor is provided, restrict search to lines near the anchor.
    let candidates: Vec<usize> = if let Some(anchor) = anchor {
        orig_lines
            .iter()
            .enumerate()
            .filter(|(_, line)| line.contains(anchor))
            .map(|(i, _)| i)
            .collect()
    } else {
        (0..orig_lines.len()).collect()
    };

    if candidates.is_empty() && anchor.is_some() {
        // Fall back to full search if anchor not found.
        return find_match(orig_lines, search, None);
    }

    for &start in &candidates {
        if start + search.len() > orig_lines.len() {
            continue;
        }
        if search
            .iter()
            .enumerate()
            .all(|(j, &s)| orig_lines[start + j] == s)
        {
            return Ok(start);
        }
    }

    // Try fuzzy match: ignore leading whitespace differences.
    for &start in &candidates {
        if start + search.len() > orig_lines.len() {
            continue;
        }
        if search
            .iter()
            .enumerate()
            .all(|(j, &s)| orig_lines[start + j].trim() == s.trim())
        {
            return Ok(start);
        }
    }

    let anchor_info = anchor
        .map(|a| format!(" with anchor '{}'", a))
        .unwrap_or_default();
    Err(format!(
        "could not find match for hunk{} (searched {} candidates, {} search lines)",
        anchor_info,
        candidates.len(),
        search.len()
    ))
}
