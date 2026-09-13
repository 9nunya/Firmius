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

use futures::StreamExt;
use schemars::JsonSchema;
use serde::Deserialize;
use std::path::{Path, PathBuf};
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
/// `wait` is a blocking operation; short waits are almost always accidental
/// polling. Use `poll` when the caller needs sub-second/non-blocking checks.
const MIN_WAIT_TIMEOUT_MS: u64 = 5_000;
const MAX_INPUT_ACTIONS: usize = 256;
const MAX_INPUT_DELAY_MS: u64 = 30_000;
const MAX_INPUT_SEQUENCE_DELAY_MS: u64 = 60_000;

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

/// Named terminal keys accepted by an input sequence. Control-letter keys
/// encode the conventional ASCII control byte (for example, `ctrl_c` is 0x03).
#[derive(Clone, Copy, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum InputKey {
    Enter,
    Tab,
    Escape,
    Backspace,
    Delete,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    CtrlA,
    CtrlB,
    CtrlC,
    CtrlD,
    CtrlE,
    CtrlF,
    CtrlG,
    CtrlH,
    CtrlI,
    CtrlJ,
    CtrlK,
    CtrlL,
    CtrlM,
    CtrlN,
    CtrlO,
    CtrlP,
    CtrlQ,
    CtrlR,
    CtrlS,
    CtrlT,
    CtrlU,
    CtrlV,
    CtrlW,
    CtrlX,
    CtrlY,
    CtrlZ,
}

/// One step in a terminal input sequence. Set exactly one of `text`, `key`,
/// or `delay_ms` per element.
#[derive(Deserialize, JsonSchema)]
struct InputAction {
    /// Literal UTF-8 text to write.
    #[serde(default)]
    text: Option<String>,
    /// A named terminal/control key. The schema lists every accepted value.
    #[serde(default)]
    key: Option<InputKey>,
    /// Pause before continuing with the next action (maximum 30000ms per
    /// action and 60000ms total).
    #[serde(default, deserialize_with = "flex::u64_opt")]
    delay_ms: Option<u64>,
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
    /// need `command` and `intent`. Use `spawn` for a server or other long-lived process, and
    /// `poll` to collect incremental output from a spawned process.
    #[serde(default)]
    mode: Mode,
    /// The command to run. Write it exactly as you would in a terminal, including
    /// arguments, quoting, pipes, redirects, and `&&`. For `exec`/`spawn` only.
    #[serde(default)]
    command: Option<String>,
    /// Legacy direct-exec arguments. Prefer putting the complete shell command in
    /// `command`. When this array is non-empty, `command` is treated as a single
    /// executable and these values are passed as argv without shell parsing.
    #[serde(default)]
    #[schemars(skip)]
    args: Vec<String>,
    /// Working directory. Defaults to the tool's current workdir. Use a
    /// repository-relative path when possible. In an SSH session, `/` also
    /// means the session's remote workspace root. For `exec`/`spawn` only.
    #[serde(default)]
    cwd: Option<String>,
    /// Milliseconds to wait. For `exec`, defaults to 30000. For `wait`,
    /// omitted uses 30000 and explicit values below 5000 are raised to 5000;
    /// use `poll` for short/non-blocking checks. If the timeout elapses, the
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
    /// Text to write immediately to the process's stdin. For `input` only.
    /// For Enter, control keys, or timed interactions, prefer `sequence`.
    #[serde(default)]
    text: Option<String>,
    /// Ordered terminal input actions. For `input` only. Each action must set
    /// exactly one of `text`, `key`, or `delay_ms`. Example: type `Yes`, press
    /// Enter, wait one second, then press Enter. Mutually exclusive with the
    /// legacy top-level `text` field.
    #[serde(default)]
    sequence: Vec<InputAction>,
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
shown to the user while the command runs.

Use this workflow:
1. Use `exec` for short, bounded commands such as tests, `git diff`, or a
   focused search. Put the whole terminal command in `command`. Always use
   `git --no-pager ...` for Git commands that may page, such as `diff`, `log`,
   `show`, and `blame`, so the process cannot get stuck waiting in a pager.
2. Use `spawn` for servers, watchers, REPLs, or anything that does not exit.
   Save the returned `proc_id` immediately.
3. Use `poll` with `since=0` first, then pass each returned `next_offset` to
   the next poll. Use `wait` when you need completion, and `kill` for cleanup.
4. Use `input` only for a process that is known to read stdin. Plain `text`
   performs one immediate write. For keys or timing, use `sequence`, e.g.
   `[{\"text\":\"Yes\"},{\"key\":\"enter\"},{\"delay_ms\":1000},{\"key\":\"enter\"}]`.
   Use `resize` for full-screen terminal programs.

Avoid commands that dump entire files or build artifacts. Prefer `grep`, the
`read` tool with a region, or a narrowly scoped command. Large results are
stored as session artifacts; read that file carefully in regions rather
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
- wait: block until a process exits, or until timeout_ms elapses (default 30s;
  values below 5s are treated as 5s). Use poll for short checks.
- input: write `text`, or execute an ordered `sequence` of text, named keys,
  and delays. Named keys include Enter, navigation keys, and ctrl_a through
  ctrl_z. `text` and `sequence` are mutually exclusive.
- resize: change a process's terminal size (rows/cols); TUI apps repaint on this.
- kill: forcibly terminate a process.
- list: show every process this agent has touched, with status and command line.

Always prefer `exec` for short commands. Use `spawn` for anything that does not
exit on its own. For a normal command, omit `mode` and provide
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
            input(&ctx, proc_id, a.text, a.sequence).await
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn named_terminal_keys_have_expected_bytes() {
        assert_eq!(key_bytes(InputKey::Enter), b"\r");
        assert_eq!(key_bytes(InputKey::CtrlC), b"\x03");
        assert_eq!(key_bytes(InputKey::Up), b"\x1b[A");
        assert_eq!(key_bytes(InputKey::Delete), b"\x1b[3~");
    }

    #[test]
    fn input_sequence_deserializes_documented_interaction() {
        let actions: Vec<InputAction> = serde_json::from_value(serde_json::json!([
            {"text": "Yes"},
            {"key": "enter"},
            {"delay_ms": 1000},
            {"key": "enter"}
        ]))
        .unwrap();
        assert_eq!(validate_input_sequence(&actions).unwrap(), 1000);
        assert_eq!(actions[0].text.as_deref(), Some("Yes"));
        assert!(matches!(actions[1].key, Some(InputKey::Enter)));
    }

    #[test]
    fn input_sequence_rejects_ambiguous_and_excessive_actions_before_writing() {
        let ambiguous: Vec<InputAction> = serde_json::from_value(serde_json::json!([
            {"text": "Yes", "key": "enter"}
        ]))
        .unwrap();
        assert!(validate_input_sequence(&ambiguous).is_err());

        let excessive_delay: Vec<InputAction> = serde_json::from_value(serde_json::json!([
            {"delay_ms": 30001}
        ]))
        .unwrap();
        assert!(validate_input_sequence(&excessive_delay).is_err());
    }

    #[test]
    fn wait_uses_a_blocking_default_and_rejects_one_second_polling() {
        assert_eq!(effective_wait_timeout_ms(None), 30_000);
        assert_eq!(effective_wait_timeout_ms(Some(1_000)), 5_000);
        assert_eq!(effective_wait_timeout_ms(Some(10_000)), 10_000);
    }

    #[test]
    fn remote_cwd_stays_within_the_remote_session_without_local_stat_calls() {
        let root = Path::new("/srv/fir-project");
        assert_eq!(remote_cwd(root, None).unwrap(), root);
        assert_eq!(remote_cwd(root, Some("/")).unwrap(), root);
        assert_eq!(
            remote_cwd(root, Some("src/./app")).unwrap(),
            root.join("src/app")
        );
        assert!(remote_cwd(root, Some("../outside")).is_err());
        assert!(remote_cwd(root, Some("/etc")).is_err());
    }

    #[test]
    fn local_cwd_accepts_workspace_absolute_path_but_not_outside_path() {
        let root =
            std::env::temp_dir().join(format!("firmius-bash-cwd-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let nested = root.join("nested");
        std::fs::create_dir(&nested).unwrap();

        // `existing_directory` returns the canonicalized path, so compare
        // against the canonical form: on macOS `temp_dir()` is a symlink
        // (`/var` -> `/private/var`) and the raw join would never match.
        assert_eq!(
            crate::tools::path::existing_directory(&root, nested.to_str().unwrap()).unwrap(),
            nested.canonicalize().unwrap()
        );
        assert!(
            crate::tools::path::existing_directory(&root, std::env::temp_dir().to_str().unwrap())
                .is_err()
        );
        std::fs::remove_dir_all(root).unwrap();
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
    let cwd = if ctx.workspace().is_local() {
        match cwd {
            Some(path) if path.trim().is_empty() || path.trim() == "." => {
                std::fs::canonicalize(&ctx.workdir)
                    .map_err(|e| ToolError::InvalidArguments(format!("invalid workdir: {e}")))?
            }
            Some(path) => crate::tools::path::existing_directory(&ctx.workdir, &path)
                .map_err(ToolError::InvalidArguments)?,
            None => std::fs::canonicalize(&ctx.workdir)
                .map_err(|e| ToolError::InvalidArguments(format!("invalid workdir: {e}")))?,
        }
    } else {
        // A remote workdir is deliberately not present on the local machine.
        // Validating it with canonicalize() made every SSH bash call fail
        // before RemoteHost ever got a chance to run `ssh`. Keep paths
        // confined to the session root without trying to stat them locally.
        remote_cwd(&ctx.workdir, cwd.as_deref())?
    };
    Ok(ProcSpec::new(program)
        .args(args)
        .cwd(cwd.display().to_string())
        .size(size)
        .on_orphan(OnOrphan::Kill))
}

/// Resolve a terminal cwd for an SSH-backed workspace.
///
/// `cwd` is still workspace-relative.  Treat `/` as the remote session root:
/// models commonly use it after a directory listing, and allowing it avoids
/// accidentally interpreting it as the local or remote filesystem root.
fn remote_cwd(workdir: &Path, cwd: Option<&str>) -> Result<PathBuf, ToolError> {
    let Some(cwd) = cwd else {
        return Ok(workdir.to_path_buf());
    };
    let cwd = cwd.trim();
    if cwd.is_empty() || cwd == "." || cwd == "/" {
        return Ok(workdir.to_path_buf());
    }
    if Path::new(cwd).is_absolute() || cwd.starts_with('\\') {
        return Err(ToolError::InvalidArguments(
            "cwd must be relative to the remote session workspace".into(),
        ));
    }

    let mut resolved = workdir.to_path_buf();
    for part in cwd.split(['/', '\\']) {
        match part {
            "" | "." => {}
            ".." => {
                return Err(ToolError::InvalidArguments(
                    "cwd may not escape the remote session workspace".into(),
                ));
            }
            part => resolved.push(part),
        }
    }
    Ok(resolved)
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

fn track_process(ctx: &ToolContext, id: ProcId, mode: &'static str) {
    ctx.publish_runtime(crate::ToolRuntimeResource::Process {
        id: id.to_string(),
        mode: mode.into(),
        status: crate::ProcStatus::Running,
    });
    let wait_ctx = ctx.clone();
    tokio::spawn(async move {
        if let Ok(exit) = wait_ctx.host.wait(id).await {
            wait_ctx.publish_runtime(crate::ToolRuntimeResource::Process {
                id: id.to_string(),
                mode: mode.into(),
                status: crate::ProcStatus::Exited {
                    code: exit.code,
                    success: exit.success,
                },
            });
        }
    });
    let output_ctx = ctx.clone();
    tokio::spawn(async move {
        let Ok(mut output) = output_ctx.host.output(id) else {
            return;
        };
        let mut total: usize = 0;
        while let Some(chunk) = output.next().await {
            total = total.saturating_add(chunk.bytes.len());
            output_ctx.publish_process_output(id, chunk.bytes, total);
        }
    });
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

    track_process(ctx, id, "exec");
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
    track_process(ctx, id, "spawn");
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
    track_process(ctx, id, "exec");
    let timeout = Duration::from_millis(effective_wait_timeout_ms(timeout_ms));
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

fn effective_wait_timeout_ms(timeout_ms: Option<u64>) -> u64 {
    timeout_ms
        .unwrap_or(DEFAULT_EXEC_TIMEOUT_MS)
        .max(MIN_WAIT_TIMEOUT_MS)
}

fn key_bytes(key: InputKey) -> &'static [u8] {
    match key {
        InputKey::Enter => b"\r",
        InputKey::Tab => b"\t",
        InputKey::Escape => b"\x1b",
        InputKey::Backspace => b"\x7f",
        InputKey::Delete => b"\x1b[3~",
        InputKey::Up => b"\x1b[A",
        InputKey::Down => b"\x1b[B",
        InputKey::Right => b"\x1b[C",
        InputKey::Left => b"\x1b[D",
        InputKey::Home => b"\x1b[H",
        InputKey::End => b"\x1b[F",
        InputKey::PageUp => b"\x1b[5~",
        InputKey::PageDown => b"\x1b[6~",
        InputKey::CtrlA => b"\x01",
        InputKey::CtrlB => b"\x02",
        InputKey::CtrlC => b"\x03",
        InputKey::CtrlD => b"\x04",
        InputKey::CtrlE => b"\x05",
        InputKey::CtrlF => b"\x06",
        InputKey::CtrlG => b"\x07",
        InputKey::CtrlH => b"\x08",
        InputKey::CtrlI => b"\x09",
        InputKey::CtrlJ => b"\x0a",
        InputKey::CtrlK => b"\x0b",
        InputKey::CtrlL => b"\x0c",
        InputKey::CtrlM => b"\x0d",
        InputKey::CtrlN => b"\x0e",
        InputKey::CtrlO => b"\x0f",
        InputKey::CtrlP => b"\x10",
        InputKey::CtrlQ => b"\x11",
        InputKey::CtrlR => b"\x12",
        InputKey::CtrlS => b"\x13",
        InputKey::CtrlT => b"\x14",
        InputKey::CtrlU => b"\x15",
        InputKey::CtrlV => b"\x16",
        InputKey::CtrlW => b"\x17",
        InputKey::CtrlX => b"\x18",
        InputKey::CtrlY => b"\x19",
        InputKey::CtrlZ => b"\x1a",
    }
}

fn validate_input_sequence(sequence: &[InputAction]) -> Result<u64, ToolError> {
    if sequence.is_empty() {
        return Err(ToolError::InvalidArguments(
            "input sequence must not be empty".into(),
        ));
    }
    if sequence.len() > MAX_INPUT_ACTIONS {
        return Err(ToolError::InvalidArguments(format!(
            "input sequence exceeds {MAX_INPUT_ACTIONS} actions"
        )));
    }
    let mut total_delay = 0u64;
    for (index, action) in sequence.iter().enumerate() {
        let fields = usize::from(action.text.is_some())
            + usize::from(action.key.is_some())
            + usize::from(action.delay_ms.is_some());
        if fields != 1 {
            return Err(ToolError::InvalidArguments(format!(
                "input sequence action {index} must set exactly one of 'text', 'key', or 'delay_ms'"
            )));
        }
        if let Some(delay_ms) = action.delay_ms {
            if delay_ms > MAX_INPUT_DELAY_MS {
                return Err(ToolError::InvalidArguments(format!(
                    "input sequence action {index} delay exceeds {MAX_INPUT_DELAY_MS}ms"
                )));
            }
            total_delay = total_delay.saturating_add(delay_ms);
            if total_delay > MAX_INPUT_SEQUENCE_DELAY_MS {
                return Err(ToolError::InvalidArguments(format!(
                    "input sequence total delay exceeds {MAX_INPUT_SEQUENCE_DELAY_MS}ms"
                )));
            }
        }
    }
    Ok(total_delay)
}

async fn input(
    ctx: &ToolContext,
    proc_id: String,
    text: Option<String>,
    sequence: Vec<InputAction>,
) -> Result<String, ToolError> {
    let id = parse_id(&proc_id)?;
    if text.is_some() == !sequence.is_empty() {
        return Err(ToolError::InvalidArguments(
            "mode 'input' requires exactly one of 'text' or non-empty 'sequence'".into(),
        ));
    }
    if let Some(text) = text {
        ctx.host
            .write_stdin(id, text.as_bytes())
            .await
            .map_err(|e| ToolError::Failed(e.to_string()))?;
        return Ok(format!("wrote {} bytes to proc_id={id}", text.len()));
    }

    let total_delay = validate_input_sequence(&sequence)?;
    let mut bytes_written = 0usize;
    for action in sequence {
        if let Some(delay_ms) = action.delay_ms {
            tokio::select! {
                _ = ctx.cancellation.cancelled() => {
                    return Err(ToolError::Failed("input sequence cancelled".into()));
                }
                _ = tokio::time::sleep(Duration::from_millis(delay_ms)) => {}
            }
            continue;
        }
        let bytes = match (&action.text, action.key) {
            (Some(text), None) => text.as_bytes(),
            (None, Some(key)) => key_bytes(key),
            _ => unreachable!("action shape validated above"),
        };
        ctx.host
            .write_stdin(id, bytes)
            .await
            .map_err(|e| ToolError::Failed(e.to_string()))?;
        bytes_written += bytes.len();
    }
    Ok(format!(
        "completed input sequence: wrote {bytes_written} bytes after {total_delay}ms delay to proc_id={id}"
    ))
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
