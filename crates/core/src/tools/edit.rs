use schemars::JsonSchema;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::artifact::{
    ArtifactSource, SessionArtifacts, is_artifact_path, normalize_artifact_path,
};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::edit_history::{self, Entry, Transaction};
use super::path;
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

fn validate_swarm_edit<'a>(
    ctx: &ToolContext,
    resource_paths: impl IntoIterator<Item = &'a str>,
) -> Result<(), ToolError> {
    let Some(session) = &ctx.session else {
        return Ok(());
    };
    let state = session.work.read().unwrap();
    if state.swarm.policy != crate::work::SwarmPolicy::Protective {
        return Ok(());
    }
    let caller_assignment = state
        .binding_for_agent(&ctx.agent_id)
        .map(|binding| binding.assignment_id);
    let workspace = ctx
        .workspace()
        .identity(&ctx.workdir)
        .map_err(coordination_error)?;
    for path in resource_paths {
        if let Some(claim) = crate::work::foreign_mutation_conflict(
            &state,
            caller_assignment,
            &workspace.to_string(),
            path,
        ) {
            return Err(ToolError::Failed(format!(
                "protective swarm collision: path '{path}' is held by assignment {} (claim {}); request coordination or ownership transfer before retrying",
                claim.owner.assignment_id, claim.id
            )));
        }
    }
    Ok(())
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
    r.register(
        TypedTool::new(
            "undo",
            "Undo or redo this agent's most recent successful edit transaction. Use action=undo or redo; action=status reports available history. Undo is scoped to this agent, so it cannot rewind a sibling agent's edits.",
            |a: UndoArgs, ctx: ToolContext| Box::pin(undo_edit(a, ctx)),
        )
        .with_required_scopes(["fs_write"]),
    );
    r
}

#[derive(serde::Deserialize, JsonSchema)]
struct UndoArgs {
    /// undo, redo, or status
    #[serde(default = "default_undo_action")]
    action: String,
}

fn default_undo_action() -> String {
    "undo".into()
}

async fn undo_edit(args: UndoArgs, ctx: ToolContext) -> Result<String, ToolError> {
    apply_history_action(&args.action, ctx).await
}

/// Apply durable edit history without requiring a model turn. Direct UI and
/// daemon shortcuts use this same path as the built-in `undo` tool.
pub(crate) async fn apply_history_action(
    requested: &str,
    ctx: ToolContext,
) -> Result<String, ToolError> {
    let action = requested.to_ascii_lowercase();
    if action == "status" {
        let (undo, redo) = edit_history::counts(&ctx.session_id, &ctx.agent_id);
        return Ok(format!("edit history: {undo} undoable, {redo} redoable"));
    }
    let is_undo = match action.as_str() {
        "undo" => true,
        "redo" => false,
        _ => {
            return Err(ToolError::InvalidArguments(
                "action must be undo, redo, or status".into(),
            ));
        }
    };
    let Some(tx) = (if is_undo {
        edit_history::take_undo(&ctx.session_id, &ctx.agent_id)
    } else {
        edit_history::take_redo(&ctx.session_id, &ctx.agent_id)
    }) else {
        return Ok(format!("nothing to {action}"));
    };
    let result = apply_transaction(&tx, is_undo, &ctx).await;
    match result {
        Ok(()) => {
            let label = tx.label.clone();
            if is_undo {
                edit_history::push_redo(&ctx.session_id, &ctx.agent_id, tx);
            } else {
                edit_history::push_undo(&ctx.session_id, &ctx.agent_id, tx);
            }
            Ok(format!("{action} complete: {label}"))
        }
        Err(error) => {
            if is_undo {
                edit_history::push_undo(&ctx.session_id, &ctx.agent_id, tx);
            } else {
                edit_history::push_redo(&ctx.session_id, &ctx.agent_id, tx);
            }
            Err(error)
        }
    }
}

async fn apply_transaction(
    tx: &Transaction,
    undo: bool,
    ctx: &ToolContext,
) -> Result<(), ToolError> {
    let store = session_artifacts(ctx).await;
    let prepared = prepare_edit_attempt(ctx, tx.entries.iter().map(|entry| entry.path.as_str()))?;

    // A sibling agent may have changed a shared file after this transaction
    // was recorded. Verify every entry before writing anything so undo/redo
    // never silently clobbers newer work. This is the important distinction
    // between per-agent history and unsafe per-agent overwriting.
    for entry in &tx.entries {
        let expected = if undo {
            entry.after.as_deref()
        } else {
            entry.before.as_deref()
        };
        let actual = current_bytes(&entry.path, ctx, store.as_ref()).await?;
        if actual.as_deref() != expected {
            return Err(ToolError::Failed(format!(
                "edit history conflict at {}: current content changed; inspect before retrying",
                entry.path
            )));
        }
    }

    let lease = match prepared.authority {
        Some(attempt) => Some(
            attempt
                .acquire_wait(&ctx.cancellation)
                .await
                .map_err(coordination_error)?,
        ),
        None => None,
    };
    // Claims can change while preflight or the ordinary EditAuthority wait is
    // in progress. Serialize the final protective check with claim/policy
    // mutations and hold it through the filesystem commit.
    let _swarm_edit = if let Some(session) = &ctx.session {
        Some(session.lock_swarm_edits().await)
    } else {
        None
    };
    if ctx.cancelled() {
        return Err(ToolError::Failed("edit cancelled before commit".into()));
    }
    if let Some(lease) = &lease {
        lease.revalidate().map_err(coordination_error)?;
    }
    validate_swarm_edit(ctx, prepared.swarm_paths.iter().map(String::as_str))?;
    // Content CAS under the acquired lease closes the preparation window:
    // shell/external writers are not authority participants, so generation
    // CAS alone cannot detect them.
    for entry in &tx.entries {
        let expected = if undo {
            entry.after.as_deref()
        } else {
            entry.before.as_deref()
        };
        let actual = current_bytes(&entry.path, ctx, store.as_ref()).await?;
        if actual.as_deref() != expected {
            return Err(ToolError::Failed(format!(
                "edit history conflict at {}: current content changed; inspect before retrying",
                entry.path
            )));
        }
    }

    for entry in &tx.entries {
        let bytes = if undo {
            entry.before.as_ref()
        } else {
            entry.after.as_ref()
        };
        if entry.path.starts_with("artifact://") {
            let store = store
                .as_ref()
                .ok_or_else(|| ToolError::Failed("artifacts unavailable".into()))?;
            let path = normalize_artifact_path(&entry.path)
                .map_err(|e| ToolError::Failed(e.to_string()))?;
            match bytes {
                Some(value) => {
                    store
                        .write(
                            &path,
                            String::from_utf8_lossy(value).into_owned(),
                            Some(&ctx.agent_id),
                            ArtifactSource::Manual,
                        )
                        .map_err(|e| ToolError::Failed(e.to_string()))?;
                }
                None => {
                    let _ = store.remove(&path);
                }
            }
        } else {
            match bytes {
                Some(value) => ctx
                    .workspace()
                    .write(&ctx.workdir, &entry.path, value)
                    .await
                    .map_err(|e| ToolError::Failed(e.to_string()))?,
                None => {
                    let _ = ctx.workspace().remove(&ctx.workdir, &entry.path).await;
                }
            }
        }
    }
    if let Some(lease) = lease {
        lease.commit().map_err(coordination_error)?;
    }
    Ok(())
}

async fn current_bytes(
    path: &str,
    ctx: &ToolContext,
    store: Option<&Arc<SessionArtifacts>>,
) -> Result<Option<Vec<u8>>, ToolError> {
    if path.starts_with("artifact://") {
        let store = store.ok_or_else(|| ToolError::Failed("artifacts unavailable".into()))?;
        let normalized =
            normalize_artifact_path(path).map_err(|error| ToolError::Failed(error.to_string()))?;
        return Ok(store
            .get(&normalized)
            .map(|artifact| artifact.content.into_bytes()));
    }

    match ctx.workspace().read(&ctx.workdir, path).await {
        Ok(bytes) => Ok(Some(bytes)),
        Err(error) if workspace_missing(&error) => Ok(None),
        Err(error) => Err(ToolError::Failed(format!(
            "read {} while checking edit history: {error}",
            path
        ))),
    }
}

fn workspace_missing(error: &crate::workspace::WorkspaceError) -> bool {
    let text = error.to_string().to_ascii_lowercase();
    text.contains("no such file") || text.contains("not found") || text.contains("does not exist")
}

// ---------------------------------------------------------------------------
// Patch AST
// ---------------------------------------------------------------------------

#[derive(Clone)]
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

#[derive(Clone)]
struct Hunk {
    header: Option<String>,
    lines: Vec<HunkLine>,
}

#[derive(Clone)]
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
    let prepared = prepare_edit_attempt(ctx, edit_op_paths(&ops))?;
    preflight(&ops, ctx).await?;
    let mut report = Vec::new();
    let mut changes: Vec<crate::work::FileChange> = Vec::new();
    let snapshots = snapshot_files(&ops, ctx).await?;
    let history_before = capture_history_entries(&ops, ctx).await?;
    let artifact_snapshot = session_artifacts(ctx).await.map(|s| s.snapshot());
    let lease = match prepared.authority {
        Some(attempt) => Some(
            attempt
                .acquire_wait(&ctx.cancellation)
                .await
                .map_err(coordination_error)?,
        ),
        None => None,
    };
    // Claims can change while preflight or the ordinary EditAuthority wait is
    // in progress. Serialize the final protective check with claim/policy
    // mutations and hold it through the filesystem commit.
    let _swarm_edit = if let Some(session) = &ctx.session {
        Some(session.lock_swarm_edits().await)
    } else {
        None
    };
    if ctx.cancelled() {
        return Err(ToolError::Failed("edit cancelled before commit".into()));
    }
    if let Some(lease) = &lease {
        lease.revalidate().map_err(coordination_error)?;
    }
    validate_swarm_edit(ctx, prepared.swarm_paths.iter().map(String::as_str))?;
    let current = capture_history_entries(&ops, ctx).await?;
    if current != history_before {
        return Err(ToolError::Failed(
            "edit conflict: content changed after preflight; inspect before retrying".into(),
        ));
    }

    for op in ops.clone() {
        let outcome = apply_one_op(op, ctx).await;
        match outcome {
            Ok((line, change)) => {
                report.push(line);
                changes.push(change);
            }
            Err(error) => {
                rollback_files(&snapshots).await;
                if let (Some(records), Some(store)) =
                    (artifact_snapshot.as_ref(), session_artifacts(ctx).await)
                {
                    store.restore_snapshot(records.clone());
                }
                return Err(error);
            }
        }
    }

    if let Err(error) = emit_file_changes(ctx, changes).await {
        // A file-change event is part of the edit transaction. If durable
        // publication cannot commit, do not leave the filesystem or artifact
        // store claiming a change that the work graph never observed.
        rollback_files(&snapshots).await;
        if let (Some(records), Some(store)) =
            (artifact_snapshot.as_ref(), session_artifacts(ctx).await)
        {
            store.restore_snapshot(records.clone());
        }
        return Err(error);
    }
    if let Some(lease) = lease
        && let Err(error) = lease.commit().map_err(coordination_error)
    {
        rollback_files(&snapshots).await;
        if let (Some(records), Some(store)) =
            (artifact_snapshot.as_ref(), session_artifacts(ctx).await)
        {
            store.restore_snapshot(records.clone());
        }
        return Err(error);
    }
    let history_after = capture_history_entries(&ops, ctx).await?;
    let entries = history_before
        .into_iter()
        .zip(history_after)
        .map(|(before, after)| Entry {
            path: before.path,
            before: before.bytes,
            after: after.bytes,
        })
        .collect();
    let history_warning = edit_history::record(
        &ctx.session_id,
        &ctx.agent_id,
        Transaction {
            label: report.join(", "),
            entries,
        },
    )
    .err();
    let mut result = report.join("\n");
    if let Some(error) = history_warning {
        result.push_str(&format!(
            "\nwarning: edit succeeded but per-agent history could not be persisted: {error}"
        ));
    }
    Ok(result)
}

fn edit_op_paths(ops: &[FileOp]) -> Vec<&str> {
    let mut paths = Vec::new();
    for op in ops {
        match op {
            FileOp::Add { path, .. } | FileOp::Delete { path } => {
                paths.push(path.as_str());
            }
            FileOp::Update { path, move_to, .. } => {
                paths.push(path.as_str());
                if let Some(path) = move_to {
                    paths.push(path.as_str());
                }
            }
        }
    }
    paths
}

struct PreparedEditAttempt {
    authority: Option<crate::EditAttemptGuard>,
    /// Workspace-relative paths in the same resolved namespace used by
    /// EditAuthority. In particular, local symlink aliases are resolved.
    swarm_paths: Vec<String>,
}

fn prepare_edit_attempt<'a>(
    ctx: &ToolContext,
    paths: impl IntoIterator<Item = &'a str>,
) -> Result<PreparedEditAttempt, ToolError> {
    let paths = paths.into_iter().collect::<Vec<_>>();
    if paths.is_empty() {
        return Ok(PreparedEditAttempt {
            authority: None,
            swarm_paths: Vec::new(),
        });
    }
    let transport = ctx.workspace();
    let workspace = transport
        .identity(&ctx.workdir)
        .map_err(coordination_error)?;
    let mut swarm_paths = Vec::new();
    let resources = paths
        .into_iter()
        .map(|path| {
            if is_artifact_path(path) {
                let path = normalize_artifact_path(path).map_err(coordination_error)?;
                Ok(std::path::PathBuf::from(format!(
                    ".firmius-artifacts/{}/{path}",
                    ctx.session_id
                )))
            } else {
                let resource = transport
                    .normalize_write_resource(&ctx.workdir, path)
                    .map_err(|error| match error {
                        crate::workspace::WorkspaceError::InvalidPath(message) => {
                            ToolError::InvalidArguments(message)
                        }
                        other => coordination_error(other),
                    })?;
                let claim_path = if let Some(root) = workspace.canonical_local_root() {
                    let relative = resource.strip_prefix(root).map_err(|_| {
                        coordination_error("normalized write resource escaped workspace")
                    })?;
                    relative
                        .components()
                        .map(|component| {
                            component.as_os_str().to_str().ok_or_else(|| {
                                ToolError::InvalidArguments(
                                    "resolved write path is not valid UTF-8".into(),
                                )
                            })
                        })
                        .collect::<Result<Vec<_>, _>>()?
                        .join("/")
                } else {
                    crate::work::normalize_workspace_path(path).map_err(coordination_error)?
                };
                swarm_paths.push(claim_path);
                Ok(resource)
            }
        })
        .collect::<Result<Vec<_>, ToolError>>()?;
    // A tool context normally carries a unique provider call id, but tests,
    // embedded callers, and undo/redo can intentionally reuse one context.
    // Coordination attempt ids must still be unique per mutation operation;
    // the authority's idempotency guarantee remains available to direct
    // callers that construct EditAttempt themselves.
    static NEXT_EDIT_ATTEMPT: AtomicU64 = AtomicU64::new(1);
    let attempt_id = format!(
        "{}:{}",
        ctx.tool_call_id,
        NEXT_EDIT_ATTEMPT.fetch_add(1, Ordering::Relaxed)
    );
    let attempt = crate::EditAttempt::normalized(
        workspace,
        ctx.session_id.clone(),
        ctx.agent_id.clone(),
        attempt_id,
        resources,
    )
    .map_err(coordination_error)?;
    let authority = ctx
        .edit_authority()
        .prepare(attempt)
        .map(Some)
        .map_err(coordination_error)?;
    Ok(PreparedEditAttempt {
        authority,
        swarm_paths,
    })
}

fn coordination_error(error: impl std::fmt::Display) -> ToolError {
    ToolError::Failed(error.to_string())
}

#[derive(Clone, PartialEq, Eq)]
struct HistoryCapture {
    path: String,
    bytes: Option<Vec<u8>>,
}

async fn capture_history_entries(
    ops: &[FileOp],
    ctx: &ToolContext,
) -> Result<Vec<HistoryCapture>, ToolError> {
    let mut paths = Vec::new();
    for op in ops {
        match op {
            FileOp::Add { path, .. } | FileOp::Delete { path } => paths.push(path.clone()),
            FileOp::Update { path, move_to, .. } => {
                paths.push(path.clone());
                if let Some(move_to) = move_to {
                    paths.push(move_to.clone());
                }
            }
        }
    }
    paths.sort();
    paths.dedup();
    let artifacts = session_artifacts(ctx).await;
    let mut out = Vec::with_capacity(paths.len());
    for path in paths {
        let bytes = if is_artifact_path(&path) {
            let store = artifacts
                .as_ref()
                .ok_or_else(|| ToolError::Failed("artifacts unavailable".into()))?;
            let key = normalize_artifact_path(&path)
                .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
            store.get(&key).map(|a| a.content.into_bytes())
        } else {
            ctx.workspace().read(&ctx.workdir, &path).await.ok()
        };
        out.push(HistoryCapture { path, bytes });
    }
    Ok(out)
}

async fn preflight(ops: &[FileOp], ctx: &ToolContext) -> Result<(), ToolError> {
    for op in ops {
        match op {
            FileOp::Add { path: p, .. } => {
                if is_artifact_path(p) {
                    let s = require_artifacts(ctx).await?;
                    let k = normalize_artifact_path(p)
                        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                    if s.get(&k).is_some() {
                        return Err(ToolError::InvalidArguments(format!(
                            "destination already exists: {p}"
                        )));
                    }
                } else {
                    if ctx.workspace().read(&ctx.workdir, p).await.is_ok() {
                        return Err(ToolError::InvalidArguments(format!(
                            "destination already exists: {p}"
                        )));
                    }
                }
            }
            FileOp::Delete { path: p } => {
                if !is_artifact_path(p) {
                    ctx.workspace()
                        .read(&ctx.workdir, p)
                        .await
                        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                }
            }
            FileOp::Update {
                path: p,
                move_to,
                hunks,
            } => {
                if is_artifact_path(p) {
                    let s = require_artifacts(ctx).await?;
                    let k = normalize_artifact_path(p)
                        .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                    let original = s.read(&k).map_err(|e| ToolError::Failed(e.to_string()))?;
                    apply_hunks(&original, hunks)
                        .map_err(|e| ToolError::Failed(format!("patch {p}: {e}")))?;
                    if let Some(m) = move_to {
                        let d = normalize_artifact_path(m)
                            .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                        if d != k && s.get(&d).is_some() {
                            return Err(ToolError::InvalidArguments(format!(
                                "destination already exists: {m}"
                            )));
                        }
                    }
                } else {
                    let original = String::from_utf8(
                        ctx.workspace()
                            .read(&ctx.workdir, p)
                            .await
                            .map_err(|e| ToolError::Failed(e.to_string()))?,
                    )
                    .map_err(|e| ToolError::Failed(e.to_string()))?;
                    apply_hunks(&original, hunks)
                        .map_err(|e| ToolError::Failed(format!("patch {p}: {e}")))?;
                    if let Some(m) = move_to {
                        if ctx.workspace().read(&ctx.workdir, m).await.is_ok() {
                            return Err(ToolError::InvalidArguments(format!(
                                "destination already exists: {m}"
                            )));
                        }
                    }
                }
            }
        }
    }
    Ok(())
}

struct FileSnapshot {
    path: std::path::PathBuf,
    bytes: Option<Vec<u8>>,
}
async fn snapshot_files(ops: &[FileOp], ctx: &ToolContext) -> Result<Vec<FileSnapshot>, ToolError> {
    if !ctx.workspace().is_local() {
        return Ok(Vec::new());
    }
    let mut paths = Vec::new();
    for op in ops {
        let (p, m) = match op {
            FileOp::Add { path, .. } | FileOp::Delete { path } => (path, None),
            FileOp::Update { path, move_to, .. } => (path, move_to.as_ref()),
        };
        if is_artifact_path(p) {
            continue;
        }
        paths.push(path::destination(&ctx.workdir, p).map_err(ToolError::InvalidArguments)?);
        if let Some(m) = m {
            paths.push(path::destination(&ctx.workdir, m).map_err(ToolError::InvalidArguments)?);
        }
    }
    paths.sort();
    paths.dedup();
    let mut out = Vec::new();
    for p in paths {
        out.push(FileSnapshot {
            bytes: tokio::fs::read(&p).await.ok(),
            path: p,
        });
    }
    Ok(out)
}
async fn rollback_files(snapshots: &[FileSnapshot]) {
    for s in snapshots {
        match &s.bytes {
            Some(bytes) => {
                let _ = tokio::fs::write(&s.path, bytes).await;
            }
            None => {
                let _ = tokio::fs::remove_file(&s.path).await;
            }
        }
    }
}

/// Emit a `WorkEvent::FilesChanged` for the calling agent's active work
/// binding, if any. No-op when the agent has no active graph/binding (e.g.
/// an unbound top-level agent), and best-effort: a failure to emit must not
/// undo the filesystem/artifact change that already happened.
async fn emit_file_changes(
    ctx: &ToolContext,
    changes: Vec<crate::work::FileChange>,
) -> Result<(), ToolError> {
    if changes.is_empty() {
        return Ok(());
    }
    let Some(session) = ctx.session.clone() else {
        return Ok(());
    };
    let graph_id = {
        let state = session.work.read().unwrap();
        match state.binding_for_agent(&ctx.agent_id) {
            Some(binding) => binding.graph_id,
            None => return Ok(()),
        }
    };
    let agent_id = ctx.agent_id.clone();
    if let Err(error) = session.mutate_work(move |state| {
        state.graph(graph_id)?;
        Ok((
            (),
            crate::work::WorkEvent::FilesChanged {
                graph_id,
                agent_id,
                changes,
            },
        ))
    }) {
        return Err(ToolError::Failed(format!(
            "filesystem changed but file-change notification failed: {error}"
        )));
    }
    Ok(())
}

/// Apply one file operation, returning its human-readable report line and
/// the exact [`FileChange`] it performed. Never derived by parsing `bash`
/// command strings — this is the built-in `edit` tool's own bookkeeping.
async fn apply_one_op(
    op: FileOp,
    ctx: &ToolContext,
) -> Result<(String, crate::work::FileChange), ToolError> {
    use crate::work::{FileChange, FileChangeKind};
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
                return Ok((
                    format!("created artifact://{artifact_path}"),
                    FileChange {
                        kind: FileChangeKind::Added,
                        path: format!("artifact://{artifact_path}"),
                        from_path: None,
                    },
                ));
            }
            ctx.workspace()
                .write(&ctx.workdir, &path, content.as_bytes())
                .await
                .map_err(|e| ToolError::Failed(format!("write {path}: {e}")))?;
            Ok((
                format!("created {}", path),
                FileChange {
                    kind: FileChangeKind::Added,
                    path,
                    from_path: None,
                },
            ))
        }
        FileOp::Delete { path } => {
            if is_artifact_path(&path) {
                let store = require_artifacts(ctx).await?;
                let artifact_path = normalize_artifact_path(&path)
                    .map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
                store
                    .remove(&artifact_path)
                    .map_err(|e| ToolError::Failed(e.to_string()))?;
                return Ok((
                    format!("deleted artifact://{artifact_path}"),
                    FileChange {
                        kind: FileChangeKind::Deleted,
                        path: format!("artifact://{artifact_path}"),
                        from_path: None,
                    },
                ));
            }
            ctx.workspace()
                .remove(&ctx.workdir, &path)
                .await
                .map_err(|e| ToolError::Failed(format!("delete {path}: {e}")))?;
            Ok((
                format!("deleted {}", path),
                FileChange {
                    kind: FileChangeKind::Deleted,
                    path,
                    from_path: None,
                },
            ))
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
                return if let Some((src, dst)) = move_to_path {
                    if src != dst {
                        store
                            .remove(&src)
                            .map_err(|e| ToolError::Failed(e.to_string()))?;
                        Ok((
                            format!("moved artifact://{src} -> artifact://{dst}"),
                            FileChange {
                                kind: FileChangeKind::Moved,
                                path: format!("artifact://{dst}"),
                                from_path: Some(format!("artifact://{src}")),
                            },
                        ))
                    } else {
                        Ok((
                            format!("updated artifact://{src}"),
                            FileChange {
                                kind: FileChangeKind::Updated,
                                path: format!("artifact://{src}"),
                                from_path: None,
                            },
                        ))
                    }
                } else {
                    Ok((
                        format!("updated artifact://{artifact_path}"),
                        FileChange {
                            kind: FileChangeKind::Updated,
                            path: format!("artifact://{artifact_path}"),
                            from_path: None,
                        },
                    ))
                };
            }

            if move_to.as_deref().is_some_and(is_artifact_path) {
                return Err(ToolError::InvalidArguments(
                    "cannot move a filesystem file to the artifact namespace".into(),
                ));
            }
            let original = String::from_utf8(
                ctx.workspace()
                    .read(&ctx.workdir, &path)
                    .await
                    .map_err(|e| ToolError::Failed(format!("read {path}: {e}")))?,
            )
            .map_err(|e| ToolError::Failed(format!("read {path}: {e}")))?;
            let new_content = apply_hunks(&original, &hunks)
                .map_err(|e| ToolError::Failed(format!("patch {}: {e}", path)))?;
            let write_path = move_to.as_deref().unwrap_or(&path);
            ctx.workspace()
                .write(&ctx.workdir, write_path, new_content.as_bytes())
                .await
                .map_err(|e| ToolError::Failed(format!("write {write_path}: {e}")))?;
            if let Some(mt) = &move_to {
                // Remove the source only after the destination is complete.
                // Never claim a move succeeded while both paths remain. The
                // outer transaction restores the pre-edit snapshot on error.
                if let Err(error) = ctx.workspace().remove(&ctx.workdir, &path).await {
                    return Err(ToolError::Failed(format!(
                        "remove source {path} after move: {error}"
                    )));
                }
                Ok((
                    format!("moved {} -> {}", path, mt),
                    FileChange {
                        kind: FileChangeKind::Moved,
                        path: mt.clone(),
                        from_path: Some(path),
                    },
                ))
            } else {
                Ok((
                    format!("updated {}", path),
                    FileChange {
                        kind: FileChangeKind::Updated,
                        path,
                        from_path: None,
                    },
                ))
            }
        }
    }
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
