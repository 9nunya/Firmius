//! The `bash` tool: gives the agent direct process control via [`crate::host::Host`].
//!
//! One tool, eight `mode`s. `BashArgs` is intentionally a **flat struct**
//! with every field optional, not a `#[serde(tag = "mode")]` enum. Tagged
//! enums make `schemars` emit a top-level `oneOf` of N object schemas, and
//! tool-use models (Anthropic included) are unreliable at filling nested
//! `oneOf` schemas — arrays inside them tend to arrive JSON-stringified
//! instead of as real arrays. A flat object with a `mode` discriminator and
//! optional fields is the schema shape models fill correctly; each handler
//! validates that its own required fields are present.

use schemars::JsonSchema;
use serde::Deserialize;
use std::time::Duration;

use crate::host::{OnOrphan, ProcId, ProcSpec, ProcStatus, PtySize};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::flex;

/// Cap on bytes returned inline in a single tool result. Beyond this we
/// truncate and say so, rather than blowing the model's context on one
/// chatty command. (Full output is still readable via repeated `poll`.)
const MAX_INLINE_BYTES: usize = 64 * 1024;
/// Default ceiling for `exec`'s blocking wait before it hands back control
/// with the process still running in the background.
const DEFAULT_EXEC_TIMEOUT_MS: u64 = 30_000;

// ---------------------------------------------------------------------------
// Args — flat, one struct, every field optional. Omitted mode means `exec`.
// ---------------------------------------------------------------------------

#[derive(Default, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum Mode {
    #[default]
    Exec,
    Spawn,
    Poll,
    Wait,
    Input,
    Resize,
    Kill,
    List,
}

#[derive(Deserialize, JsonSchema)]
struct BashArgs {
    /// One short phrase describing what this command accomplishes, e.g.
    /// "run the test suite" or "start the dev server". Required for `exec`
    /// and `spawn`; shown to the user in place of the raw command while it
    /// runs. Not needed for poll/wait/input/resize/kill/list.
    #[serde(default)]
    intent: Option<String>,
    /// Which operation to perform. Defaults to `exec`, so normal commands only
    /// need `command`. Use `spawn` for a server or other long-lived process, and
    /// `poll` to collect incremental output from a spawned process.
    #[serde(default)]
    mode: Mode,
    /// The command to run. Write it exactly as you would in a terminal, including
    /// arguments, quoting, pipes, redirects, and `&&`. For `exec`/`spawn` only.
    /// The legacy `command` + `args` argv form remains supported when `args` is
    /// non-empty.
    #[serde(default)]
    command: Option<String>,
    /// Legacy direct-exec arguments. Prefer putting the complete shell command in
    /// `command`. When this array is non-empty, `command` is treated as a single
    /// executable and these values are passed as argv without shell parsing.
    #[serde(default)]
    args: Vec<String>,
    /// Working directory. Defaults to the tool's current workdir. Use a
    /// repository-relative path when possible. For `exec`/`spawn` only.
    #[serde(default)]
    cwd: Option<String>,
    /// Milliseconds to wait. For `exec` and `wait`, defaults to 30000. An
    /// explicit value overrides the default. If the timeout elapses, the
    /// process remains running and can be polled or killed.
    #[serde(default, deserialize_with = "flex::u64_opt")]
    timeout_ms: Option<u64>,
    /// Initial/target terminal rows. For `spawn` (default 24) and `resize`
    /// (required).
    #[serde(default, deserialize_with = "flex::u16_opt")]
    rows: Option<u16>,
    /// Initial/target terminal columns. For `spawn` (default 80) and
    /// `resize` (required).
    #[serde(default, deserialize_with = "flex::u16_opt")]
    cols: Option<u16>,
    /// Process id from a prior `spawn`/`exec` (its `proc_id`). Required for
    /// `poll`, `wait`, `input`, `resize`, `kill`.
    #[serde(default)]
    proc_id: Option<String>,
    /// Byte offset previously returned by `poll` (or 0 for the first call).
    /// Always pass the prior `next_offset` to avoid rereading output. For
    /// `poll` only.
    #[serde(default, deserialize_with = "flex::usize_opt")]
    since: Option<usize>,
    /// Text to write to the process's stdin. For `input` only.
    #[serde(default)]
    text: Option<String>,
}

fn require<'a>(field: &'a Option<String>, name: &str) -> Result<&'a str, ToolError> {
    field
        .as_deref()
        .filter(|s| !s.is_empty())
        .ok_or_else(|| ToolError::InvalidArguments(format!("mode requires '{name}'")))
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

pub fn register_bash_tool(r: &ToolRegistry) -> &ToolRegistry {
    r.register(
        TypedTool::new(
            "bash",
            "\
Run a command through Bash in a real PTY, so ordinary shell syntax and interactive/TUI programs \
work. Put the complete command line in `command` and a short `intent` phrase
describing what it does, e.g. \"run the test suite\", \"start the dev server\",
or \"install dependencies\". `intent` is required for `exec` and `spawn` and is
shown to the user while the command runs. The older direct-exec form with a
single executable in `command` and an `args` array is still accepted.

Use this workflow:
1. Use `exec` for short, bounded commands such as tests, `git diff`, or a
   focused search. Put the whole terminal command in `command`. Always use
   `git --no-pager ...` for Git commands that may page, such as `diff`, `log`,
   `show`, and `blame`, so the process cannot get stuck waiting in a pager.
2. Use `spawn` for servers, watchers, REPLs, or anything that does not exit.
   Save the returned `proc_id` immediately.
3. Use `poll` with `since=0` first, then pass each returned `next_offset` to
   the next poll. Use `wait` when you need completion, and `kill` for cleanup.
4. Use `input` only for a process that is known to read stdin. Use `resize` for
   full-screen terminal programs.

Avoid commands that dump entire files or build artifacts. Prefer `grep`, the
`read` tool with a region, or a narrowly scoped command. Large results are
redirected to a temporary file; read that file carefully in regions rather
than requesting it all at once. The current working directory is not
necessarily the repository root, so set `cwd` when the location matters.

One tool, several modes (`mode` defaults to `exec`):

- exec: run a command and wait up to timeout_ms (default 30s) for it to finish;
  returns combined stdout+stderr and the exit code. If it times out, the process
  keeps running in the background — use its proc_id with poll/wait/kill.
- spawn: start a long-running/background process (dev server, watcher, REPL) and
  return its proc_id immediately, without waiting.
- poll: non-blocking; returns output produced since a byte offset (`since`, start
  at 0), the next offset, and current status. Use the next offset on the next poll.
- wait: block until a process exits, or until timeout_ms elapses.
- input: write text to a process's stdin (answer a prompt, send a command to a REPL).
- resize: change a process's terminal size (rows/cols); TUI apps repaint on this.
- kill: forcibly terminate a process.
- list: show every process this agent has touched, with status and command line.

Always prefer `exec` for short commands. Use `spawn` for anything that does not
exit on its own. For a normal command, omit both `mode` and `args` and provide
one complete `command` string.",
            |a: BashArgs, ctx: ToolContext| Box::pin(handle(a, ctx)),
        )
        .with_required_scopes(["processes"]),
    );
    r
}

// ---------------------------------------------------------------------------
// Handler
// ---------------------------------------------------------------------------

async fn handle(a: BashArgs, ctx: ToolContext) -> Result<String, ToolError> {
    match a.mode {
        Mode::Exec => {
            let command = require(&a.command, "command")?.to_string();
            let _intent = require(&a.intent, "intent")?;
            exec(&ctx, command, a.args, a.cwd, a.timeout_ms).await
        }
        Mode::Spawn => {
            let command = require(&a.command, "command")?.to_string();
            let _intent = require(&a.intent, "intent")?;
            spawn(&ctx, command, a.args, a.cwd, a.rows, a.cols).await
        }
        Mode::Poll => {
            let proc_id = require(&a.proc_id, "proc_id")?.to_string();
            poll(&ctx, proc_id, a.since.unwrap_or(0))
        }
        Mode::Wait => {
            let proc_id = require(&a.proc_id, "proc_id")?.to_string();
            wait(&ctx, proc_id, a.timeout_ms).await
        }
        Mode::Input => {
            let proc_id = require(&a.proc_id, "proc_id")?.to_string();
            let text = a.text.ok_or_else(|| {
                ToolError::InvalidArguments("mode 'input' requires 'text'".into())
            })?;
            input(&ctx, proc_id, text).await
        }
        Mode::Resize => {
            let proc_id = require(&a.proc_id, "proc_id")?.to_string();
            let rows = a.rows.ok_or_else(|| {
                ToolError::InvalidArguments("mode 'resize' requires 'rows'".into())
            })?;
            let cols = a.cols.ok_or_else(|| {
                ToolError::InvalidArguments("mode 'resize' requires 'cols'".into())
            })?;
            resize(&ctx, proc_id, rows, cols)
        }
        Mode::Kill => {
            let proc_id = require(&a.proc_id, "proc_id")?.to_string();
            kill(&ctx, proc_id).await
        }
        Mode::List => list(&ctx),
    }
}

fn parse_id(proc_id: &str) -> Result<ProcId, ToolError> {
    proc_id
        .parse()
        .map_err(|_| ToolError::InvalidArguments(format!("invalid proc_id: '{proc_id}'")))
}

fn build_spec(
    ctx: &ToolContext,
    command: String,
    args: Vec<String>,
    cwd: Option<String>,
    rows: Option<u16>,
    cols: Option<u16>,
) -> Result<ProcSpec, ToolError> {
    if command.trim().is_empty() {
        return Err(ToolError::InvalidArguments(
            "'command' must not be empty".into(),
        ));
    }
    let (program, args) = if args.is_empty() {
        ("bash".to_string(), vec!["-lc".to_string(), command])
    } else {
        if command.contains(char::is_whitespace) {
            return Err(ToolError::InvalidArguments(
                "when 'args' is provided, 'command' must be one executable; otherwise omit 'args' and put the complete shell command in 'command'".into(),
            ));
        }
        (command, args)
    };
    let size = PtySize::new(rows.unwrap_or(24), cols.unwrap_or(80));
    let cwd = cwd.unwrap_or_else(|| ctx.workdir.display().to_string());
    Ok(ProcSpec::new(program)
        .args(args)
        .cwd(cwd)
        .size(size)
        .on_orphan(OnOrphan::Kill))
}

/// Truncate to the last `MAX_INLINE_BYTES` (most recent output matters most),
/// noting how much was dropped. Operates on a UTF-8 lossy view so we never
/// split a multi-byte codepoint into garbage.
fn truncate_output(bytes: &[u8]) -> String {
    let text = String::from_utf8_lossy(bytes);
    if text.len() <= MAX_INLINE_BYTES {
        return text.into_owned();
    }
    let dropped = text.len() - MAX_INLINE_BYTES;
    let tail = &text[text.len() - MAX_INLINE_BYTES..];
    // Avoid cutting mid-line where possible.
    let tail = tail.find('\n').map(|i| &tail[i + 1..]).unwrap_or(tail);
    format!("[...{dropped} bytes truncated...]\n{tail}")
}

async fn exec(
    ctx: &ToolContext,
    command: String,
    args: Vec<String>,
    cwd: Option<String>,
    timeout_ms: Option<u64>,
) -> Result<String, ToolError> {
    let spec = build_spec(ctx, command, args, cwd, None, None)?;
    let id = ctx
        .host
        .spawn(spec)
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))?;

    let timeout = Duration::from_millis(timeout_ms.unwrap_or(DEFAULT_EXEC_TIMEOUT_MS));
    let result = tokio::select! {
        _ = ctx.cancellation.cancelled() => {
            let _ = ctx.host.kill(id).await;
            return Err(ToolError::Failed(format!("cancelled; killed proc_id={id}")));
        }
        result = tokio::time::timeout(timeout, ctx.host.wait(id)) => result,
    };
    match result {
        Ok(Ok(status)) => {
            let (bytes, _, _) = ctx
                .host
                .peek(id, 0)
                .map_err(|e| ToolError::Failed(e.to_string()))?;
            Ok(format!(
                "exit_code={} success={}\nproc_id={id}\n{}",
                status.code,
                status.success,
                truncate_output(&bytes)
            ))
        }
        Ok(Err(e)) => Err(ToolError::Failed(e.to_string())),
        Err(_elapsed) => {
            let (bytes, offset, _) = ctx
                .host
                .peek(id, 0)
                .map_err(|e| ToolError::Failed(e.to_string()))?;
            Ok(format!(
                "still running after {}ms, proc_id={id} (use poll/wait/kill with this id)\noutput so far (offset={offset}):\n{}",
                timeout.as_millis(),
                truncate_output(&bytes)
            ))
        }
    }
}

async fn spawn(
    ctx: &ToolContext,
    command: String,
    args: Vec<String>,
    cwd: Option<String>,
    rows: Option<u16>,
    cols: Option<u16>,
) -> Result<String, ToolError> {
    let spec = build_spec(ctx, command, args, cwd, rows, cols)?;
    let id = ctx
        .host
        .spawn(spec)
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(format!("proc_id={id}"))
}

fn poll(ctx: &ToolContext, proc_id: String, since: usize) -> Result<String, ToolError> {
    let id = parse_id(&proc_id)?;
    let (bytes, next_offset, status) = ctx
        .host
        .peek(id, since)
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(format!(
        "status={}\nnext_offset={next_offset}\n{}",
        describe_status(status),
        truncate_output(&bytes)
    ))
}

async fn wait(
    ctx: &ToolContext,
    proc_id: String,
    timeout_ms: Option<u64>,
) -> Result<String, ToolError> {
    let id = parse_id(&proc_id)?;
    let timeout = Duration::from_millis(timeout_ms.unwrap_or(DEFAULT_EXEC_TIMEOUT_MS));
    let result = tokio::select! {
        _ = ctx.cancellation.cancelled() => {
            let _ = ctx.host.kill(id).await;
            return Err(ToolError::Failed(format!("cancelled; killed proc_id={id}")));
        }
        result = tokio::time::timeout(timeout, ctx.host.wait(id)) => {
            result.map_err(|_| {
                ToolError::Failed(format!(
                    "wait timed out after {}ms; proc_id={id} still running",
                    timeout.as_millis()
                ))
            })?
        }
    };
    let status = result.map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(format!(
        "exit_code={} success={}",
        status.code, status.success
    ))
}

async fn input(ctx: &ToolContext, proc_id: String, text: String) -> Result<String, ToolError> {
    let id = parse_id(&proc_id)?;
    ctx.host
        .write_stdin(id, text.as_bytes())
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(format!("wrote {} bytes to proc_id={id}", text.len()))
}

fn resize(ctx: &ToolContext, proc_id: String, rows: u16, cols: u16) -> Result<String, ToolError> {
    let id = parse_id(&proc_id)?;
    ctx.host
        .resize(id, PtySize::new(rows, cols))
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(format!("resized proc_id={id} to {rows}x{cols}"))
}

async fn kill(ctx: &ToolContext, proc_id: String) -> Result<String, ToolError> {
    let id = parse_id(&proc_id)?;
    ctx.host
        .kill(id)
        .await
        .map_err(|e| ToolError::Failed(e.to_string()))?;
    Ok(format!("killed proc_id={id}"))
}

fn list(ctx: &ToolContext) -> Result<String, ToolError> {
    let mut infos = ctx.host.list_info();
    if infos.is_empty() {
        return Ok("no processes".to_string());
    }
    infos.sort_by_key(|i| i.id.to_string());
    let lines: Vec<String> = infos
        .into_iter()
        .map(|i| {
            format!(
                "proc_id={} status={} bytes={} cmd={}",
                i.id,
                describe_status(i.status),
                i.bytes_captured,
                i.cmdline
            )
        })
        .collect();
    Ok(lines.join("\n"))
}

fn describe_status(status: ProcStatus) -> String {
    match status {
        ProcStatus::Running => "running".to_string(),
        ProcStatus::Exited { code, success } => format!("exited(code={code}, success={success})"),
    }
}