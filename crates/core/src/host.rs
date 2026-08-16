//! The `Host`: firmius-core's OS boundary for process control.
//!
//! A `Host` knows nothing about agents, LLMs, or the harness. It spawns
//! processes inside a PTY (so full-screen TUI apps render correctly),
//! streams their combined output losslessly, resizes the terminal, feeds
//! stdin, and kills on demand.
//!
//! ## Record vs. Runtime
//! Live processes are *runtime* resources: an OS PID, a PTY master, reader
//! threads. They are deliberately **not** serializable. The harness is
//! expected to keep its own serializable `Record` (command, cwd, status,
//! captured output) keyed by [`ProcId`]; this module only owns the live side.

use std::collections::HashMap;
use std::io::{Read, Write};
use std::sync::{Arc, Mutex};

use async_trait::async_trait;
use futures::stream::BoxStream;
use futures::StreamExt;
use portable_pty::{Child, ChildKiller, CommandBuilder, MasterPty, PtySize as RawPtySize};
use serde::{Deserialize, Serialize};
use tokio::sync::watch;
use uuid::Uuid;

// ---------------------------------------------------------------------------
// Identifiers & value types
// ---------------------------------------------------------------------------

/// Opaque, serializable handle to a spawned process. The model and the
/// harness pass these around; the live `Child` never leaves the `Host`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct ProcId(Uuid);

impl ProcId {
    fn new() -> Self {
        Self(Uuid::new_v4())
    }
}

impl std::fmt::Display for ProcId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl std::str::FromStr for ProcId {
    type Err = uuid::Error;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Ok(Self(Uuid::parse_str(s)?))
    }
}

/// Terminal dimensions for the PTY. `pixel_*` are optional and only matter
/// for apps that query pixel geometry (most ignore them).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct PtySize {
    pub rows: u16,
    pub cols: u16,
    pub pixel_width: u16,
    pub pixel_height: u16,
}

impl Default for PtySize {
    fn default() -> Self {
        Self {
            rows: 24,
            cols: 80,
            pixel_width: 0,
            pixel_height: 0,
        }
    }
}

impl PtySize {
    pub fn new(rows: u16, cols: u16) -> Self {
        Self {
            rows,
            cols,
            ..Default::default()
        }
    }

    fn to_raw(self) -> RawPtySize {
        RawPtySize {
            rows: self.rows,
            cols: self.cols,
            pixel_width: self.pixel_width,
            pixel_height: self.pixel_height,
        }
    }
}

/// How a live process should be treated if its owner drops the `Host` handle
/// while it is still running.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum OnOrphan {
    /// Kill the process (SIGKILL). Safe default: no zombie `cargo watch`es.
    #[default]
    Kill,
    /// Leave it running, detached from the Host.
    Detach,
}

/// Everything needed to launch a process. Plain data, cheap to construct.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProcSpec {
    pub program: String,
    pub args: Vec<String>,
    pub cwd: Option<String>,
    /// Extra environment overrides layered on top of the inherited env.
    pub env: Vec<(String, String)>,
    pub size: PtySize,
    pub on_orphan: OnOrphan,
}

impl ProcSpec {
    pub fn new(program: impl Into<String>) -> Self {
        Self {
            program: program.into(),
            args: Vec::new(),
            cwd: None,
            env: Vec::new(),
            size: PtySize::default(),
            on_orphan: OnOrphan::default(),
        }
    }

    pub fn arg(mut self, a: impl Into<String>) -> Self {
        self.args.push(a.into());
        self
    }

    pub fn args<I, S>(mut self, args: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        self.args.extend(args.into_iter().map(Into::into));
        self
    }

    pub fn cwd(mut self, dir: impl Into<String>) -> Self {
        self.cwd = Some(dir.into());
        self
    }

    pub fn env(mut self, key: impl Into<String>, val: impl Into<String>) -> Self {
        self.env.push((key.into(), val.into()));
        self
    }

    pub fn size(mut self, size: PtySize) -> Self {
        self.size = size;
        self
    }

    pub fn on_orphan(mut self, on_orphan: OnOrphan) -> Self {
        self.on_orphan = on_orphan;
        self
    }
}

/// A chunk of combined stdout+stderr bytes as they leave the PTY. Raw bytes,
/// so ANSI escape sequences and partial UTF-8 are preserved for the consumer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcChunk {
    pub bytes: Vec<u8>,
}

/// Live status of a process.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ProcStatus {
    Running,
    Exited { code: u32, success: bool },
}

impl ProcStatus {
    pub fn is_terminal(&self) -> bool {
        matches!(self, ProcStatus::Exited { .. })
    }
}

/// Result of waiting on a process to completion.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExitStatus {
    pub code: u32,
    pub success: bool,
}

/// A point-in-time snapshot of a process, for non-blocking polling.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcInfo {
    pub id: ProcId,
    pub cmdline: String,
    pub status: ProcStatus,
    pub bytes_captured: usize,
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

#[derive(Debug, thiserror::Error)]
pub enum HostError {
    #[error("failed to spawn process: {0}")]
    Spawn(String),
    #[error("no such process: {0}")]
    NotFound(ProcId),
    #[error("process i/o error: {0}")]
    Io(String),
}

// ---------------------------------------------------------------------------
// Host trait — the OS boundary
// ---------------------------------------------------------------------------

/// Process-control surface over the operating system. Implementations are
/// swappable (local, sandboxed, remote, recording) without any caller change.
#[async_trait]
pub trait Host: Send + Sync {
    /// Launch a process inside a fresh PTY. Returns immediately with a handle.
    async fn spawn(&self, spec: ProcSpec) -> Result<ProcId, HostError>;

    /// Subscribe to the process's combined output. The returned stream first
    /// replays everything captured so far, then yields live chunks until the
    /// process exits and its output is fully drained. Lossless: multiple
    /// concurrent subscribers each see the complete byte sequence.
    fn output(&self, id: ProcId) -> Result<BoxStream<'static, ProcChunk>, HostError>;

    /// Write bytes to the process's stdin (the PTY master).
    async fn write_stdin(&self, id: ProcId, data: &[u8]) -> Result<(), HostError>;

    /// Resize the PTY. TUI apps receive SIGWINCH and re-render.
    fn resize(&self, id: ProcId, size: PtySize) -> Result<(), HostError>;

    /// Kill the process (SIGKILL). Idempotent-ish: killing an exited process
    /// is a no-op error-wise.
    async fn kill(&self, id: ProcId) -> Result<(), HostError>;

    /// Await process termination and return its exit status. May be called
    /// by many waiters concurrently.
    async fn wait(&self, id: ProcId) -> Result<ExitStatus, HostError>;

    /// Current status without blocking.
    fn status(&self, id: ProcId) -> Result<ProcStatus, HostError>;

    /// Non-blocking read of output bytes captured since `since` (an offset
    /// previously returned by this call, start at 0). Returns the new bytes,
    /// the new total offset, and the current status. Never blocks on the
    /// process; safe to call repeatedly for polling.
    fn peek(&self, id: ProcId, since: usize) -> Result<(Vec<u8>, usize, ProcStatus), HostError>;

    /// Metadata snapshot for one process.
    fn info(&self, id: ProcId) -> Result<ProcInfo, HostError>;

    /// All process handles the Host currently tracks.
    fn list(&self) -> Vec<ProcId>;

    /// Metadata snapshots for every tracked process.
    fn list_info(&self) -> Vec<ProcInfo>;

    /// Kill (if running) and forget a process, releasing its resources.
    async fn remove(&self, id: ProcId) -> Result<(), HostError>;
}

// ---------------------------------------------------------------------------
// LiveProc — the runtime resource behind one ProcId
// ---------------------------------------------------------------------------

struct LiveProc {
    /// Human-readable command line, kept for `list`/`info` reporting.
    cmdline: String,
    /// Combined, append-only capture of all bytes seen. Source of truth for
    /// output replay, so streaming stays lossless even under backpressure.
    buffer: Arc<Mutex<Vec<u8>>>,
    /// Total bytes captured. Bumped by the reader thread; watched by streams.
    len_rx: watch::Receiver<usize>,
    /// Live status. Flipped to `Exited` by the wait thread.
    status_rx: watch::Receiver<ProcStatus>,
    /// PTY master, used for resize and stdin. Behind a lock for `Sync`.
    master: Mutex<Box<dyn MasterPty + Send>>,
    writer: Mutex<Box<dyn Write + Send>>,
    killer: Mutex<Box<dyn ChildKiller + Send + Sync>>,
    on_orphan: OnOrphan,
}

impl Drop for LiveProc {
    fn drop(&mut self) {
        if self.on_orphan == OnOrphan::Kill
            && !self.status_rx.borrow().is_terminal()
            && let Ok(mut killer) = self.killer.lock()
        {
            let _ = killer.kill();
        }
    }
}

// ---------------------------------------------------------------------------
// LocalHost — spawns on the local machine via a native PTY
// ---------------------------------------------------------------------------

/// Runs processes on the local machine. The default `Host`.
#[derive(Default)]
pub struct LocalHost {
    procs: Mutex<HashMap<ProcId, Arc<LiveProc>>>,
}

impl LocalHost {
    pub fn new() -> Self {
        Self::default()
    }

    fn get(&self, id: ProcId) -> Result<Arc<LiveProc>, HostError> {
        self.procs
            .lock()
            .unwrap()
            .get(&id)
            .cloned()
            .ok_or(HostError::NotFound(id))
    }
}

#[async_trait]
impl Host for LocalHost {
    async fn spawn(&self, spec: ProcSpec) -> Result<ProcId, HostError> {
        let pty_system = portable_pty::native_pty_system();
        let pair = pty_system
            .openpty(spec.size.to_raw())
            .map_err(|e| HostError::Spawn(e.to_string()))?;

        let cmdline = std::iter::once(spec.program.as_str())
            .chain(spec.args.iter().map(String::as_str))
            .collect::<Vec<_>>()
            .join(" ");

        let mut cmd = CommandBuilder::new(&spec.program);
        cmd.args(&spec.args);
        if let Some(cwd) = &spec.cwd {
            cmd.cwd(cwd);
        }
        for (k, v) in &spec.env {
            cmd.env(k, v);
        }

        let child = pair
            .slave
            .spawn_command(cmd)
            .map_err(|e| HostError::Spawn(e.to_string()))?;
        // Drop the slave so that when the child exits, the master reader sees
        // EOF instead of hanging forever.
        drop(pair.slave);

        let reader = pair
            .master
            .try_clone_reader()
            .map_err(|e| HostError::Spawn(e.to_string()))?;
        let writer = pair
            .master
            .take_writer()
            .map_err(|e| HostError::Spawn(e.to_string()))?;
        let killer = child.clone_killer();

        let buffer = Arc::new(Mutex::new(Vec::<u8>::new()));
        let (len_tx, len_rx) = watch::channel(0usize);
        let (status_tx, status_rx) = watch::channel(ProcStatus::Running);

        // Reader thread: blocking PTY reads bridged into the async world by
        // appending to the shared buffer and bumping the length watch.
        {
            let buffer = buffer.clone();
            std::thread::spawn(move || {
                let mut reader = reader;
                let mut chunk = [0u8; 8192];
                loop {
                    match reader.read(&mut chunk) {
                        Ok(0) | Err(_) => break, // EOF or PTY closed
                        Ok(n) => {
                            let new_len = {
                                let mut buf = buffer.lock().unwrap();
                                buf.extend_from_slice(&chunk[..n]);
                                buf.len()
                            };
                            // Fails only if all receivers dropped; harmless.
                            let _ = len_tx.send(new_len);
                        }
                    }
                }
            });
        }

        // Wait thread: block on the child, publish the terminal status.
        {
            std::thread::spawn(move || {
                let mut child: Box<dyn Child + Send + Sync> = child;
                let status = match child.wait() {
                    Ok(s) => ProcStatus::Exited {
                        code: s.exit_code(),
                        success: s.success(),
                    },
                    Err(_) => ProcStatus::Exited {
                        code: 1,
                        success: false,
                    },
                };
                let _ = status_tx.send(status);
            });
        }

        let live = Arc::new(LiveProc {
            cmdline,
            buffer,
            len_rx,
            status_rx,
            master: Mutex::new(pair.master),
            writer: Mutex::new(writer),
            killer: Mutex::new(killer),
            on_orphan: spec.on_orphan,
        });

        let id = ProcId::new();
        self.procs.lock().unwrap().insert(id, live);
        Ok(id)
    }

    fn output(&self, id: ProcId) -> Result<BoxStream<'static, ProcChunk>, HostError> {
        let live = self.get(id)?;
        let buffer = live.buffer.clone();
        let mut len_rx = live.len_rx.clone();
        let mut status_rx = live.status_rx.clone();

        let stream = async_stream::stream! {
            let mut emitted = 0usize;
            loop {
                // Drain everything the buffer holds beyond what we've emitted.
                let cur = *len_rx.borrow_and_update();
                if cur > emitted {
                    let data = {
                        let buf = buffer.lock().unwrap();
                        buf[emitted..cur].to_vec()
                    };
                    emitted = cur;
                    yield ProcChunk { bytes: data };
                }

                if status_rx.borrow_and_update().is_terminal() {
                    // Final drain: the reader thread may have appended tail
                    // bytes after our last read but before exit was observed.
                    let final_len = buffer.lock().unwrap().len();
                    if final_len > emitted {
                        let data = {
                            let buf = buffer.lock().unwrap();
                            buf[emitted..final_len].to_vec()
                        };
                        yield ProcChunk { bytes: data };
                    }
                    break;
                }

                // Wake on either more output or a status change. If a sender
                // was dropped (process forgotten), do one last drain and stop.
                let closed = tokio::select! {
                    r = len_rx.changed() => r.is_err(),
                    r = status_rx.changed() => r.is_err(),
                };
                if closed {
                    let final_len = buffer.lock().unwrap().len();
                    if final_len > emitted {
                        let data = {
                            let buf = buffer.lock().unwrap();
                            buf[emitted..final_len].to_vec()
                        };
                        yield ProcChunk { bytes: data };
                    }
                    break;
                }
            }
        };

        Ok(stream.boxed())
    }

    async fn write_stdin(&self, id: ProcId, data: &[u8]) -> Result<(), HostError> {
        let live = self.get(id)?;
        let mut writer = live.writer.lock().unwrap();
        writer
            .write_all(data)
            .and_then(|_| writer.flush())
            .map_err(|e| HostError::Io(e.to_string()))
    }

    fn resize(&self, id: ProcId, size: PtySize) -> Result<(), HostError> {
        let live = self.get(id)?;
        live.master
            .lock()
            .unwrap()
            .resize(size.to_raw())
            .map_err(|e| HostError::Io(e.to_string()))
    }

    async fn kill(&self, id: ProcId) -> Result<(), HostError> {
        let live = self.get(id)?;
        live.killer
            .lock()
            .unwrap()
            .kill()
            .map_err(|e| HostError::Io(e.to_string()))
    }

    async fn wait(&self, id: ProcId) -> Result<ExitStatus, HostError> {
        let live = self.get(id)?;
        let mut status_rx = live.status_rx.clone();
        loop {
            if let ProcStatus::Exited { code, success } = *status_rx.borrow_and_update() {
                return Ok(ExitStatus { code, success });
            }
            if status_rx.changed().await.is_err() {
                // Sender dropped without a terminal status: treat as failure.
                return Ok(ExitStatus {
                    code: 1,
                    success: false,
                });
            }
        }
    }

    fn status(&self, id: ProcId) -> Result<ProcStatus, HostError> {
        let live = self.get(id)?;
        let s = *live.status_rx.borrow();
        Ok(s)
    }

    fn peek(&self, id: ProcId, since: usize) -> Result<(Vec<u8>, usize, ProcStatus), HostError> {
        let live = self.get(id)?;
        let buf = live.buffer.lock().unwrap();
        let total = buf.len();
        let bytes = if since < total {
            buf[since..].to_vec()
        } else {
            Vec::new()
        };
        let status = *live.status_rx.borrow();
        Ok((bytes, total.max(since), status))
    }

    fn info(&self, id: ProcId) -> Result<ProcInfo, HostError> {
        let live = self.get(id)?;
        Ok(ProcInfo {
            id,
            cmdline: live.cmdline.clone(),
            status: *live.status_rx.borrow(),
            bytes_captured: live.buffer.lock().unwrap().len(),
        })
    }

    fn list(&self) -> Vec<ProcId> {
        self.procs.lock().unwrap().keys().copied().collect()
    }

    fn list_info(&self) -> Vec<ProcInfo> {
        self.procs
            .lock()
            .unwrap()
            .iter()
            .map(|(id, live)| ProcInfo {
                id: *id,
                cmdline: live.cmdline.clone(),
                status: *live.status_rx.borrow(),
                bytes_captured: live.buffer.lock().unwrap().len(),
            })
            .collect()
    }

    async fn remove(&self, id: ProcId) -> Result<(), HostError> {
        let live = self.procs.lock().unwrap().remove(&id);
        match live {
            Some(live) => {
                if !live.status_rx.borrow().is_terminal() {
                    let _ = live.killer.lock().unwrap().kill();
                }
                Ok(())
            }
            None => Err(HostError::NotFound(id)),
        }
    }
}
