use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

use firmius_client::{read_message, write_message};
use firmius_protocol::{
    DaemonEndpoint, DaemonEvent, ErrorCode, EventEnvelope, PROTOCOL_VERSION, ProtocolError,
    RequestEnvelope, ResponseEnvelope, decode_payload,
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::RwLock;
use tokio::task::JoinHandle;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

use crate::lifecycle::DaemonLease;
use crate::runtime::{DaemonRuntime, RuntimeParts, load_runtime_parts};

pub struct DaemonOptions {
    pub root: PathBuf,
    pub idle_timeout: Option<Duration>,
    pub runtime_parts: Option<RuntimeParts>,
}

/// Serve either the framed daemon protocol or the lightweight HTTP health
/// endpoint on the same loopback listener.  HTTP clients begin with a method
/// token (e.g. `GET `), while protocol clients begin with a length-prefixed
/// frame, so peeking at the first bytes is sufficient to distinguish them
/// without consuming any protocol data.
async fn serve_connection_or_health(
    mut stream: TcpStream,
    runtime: Arc<DaemonRuntime>,
    auth_token: String,
) -> Result<(), String> {
    let mut prefix = [0_u8; 12];
    let count = stream
        .peek(&mut prefix)
        .await
        .map_err(|error| error.to_string())?;
    // A single `peek` is allowed to return only a partial TCP segment.  Use
    // the first method byte for protocol discrimination rather than requiring
    // the complete `GET /` prefix to arrive in one read.
    if count > 0 && prefix[0] == b'G' {
        let mut request = [0_u8; 1024];
        let mut read = 0;
        // Requests may be split across multiple TCP packets; collect the
        // headers before parsing the request line.
        while read < request.len() && !request[..read].windows(4).any(|w| w == b"\r\n\r\n") {
            let received = stream
                .read(&mut request[read..])
                .await
                .map_err(|error| error.to_string())?;
            if received == 0 {
                break;
            }
            read += received;
        }
        let is_health = request[..read]
            .split(|byte| *byte == b'\r' || *byte == b'\n')
            .next()
            .is_some_and(|line| line == b"GET /health HTTP/1.1" || line == b"GET /health HTTP/1.0");
        let response = if is_health {
            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nok".as_slice()
        } else {
            b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".as_slice()
        };
        stream
            .write_all(response)
            .await
            .map_err(|error| error.to_string())?;
        stream.shutdown().await.map_err(|error| error.to_string())?;
        return Ok(());
    }
    serve_connection(stream, runtime, auth_token).await
}

impl Drop for RunningDaemon {
    fn drop(&mut self) {
        // Dropping the handle must still initiate the same cooperative
        // shutdown as an explicit `shutdown()`.  Tokio detaches the server
        // task when its JoinHandle is dropped; cancelling here ensures its
        // connection/turn children cannot outlive the profile lease.
        self.shutdown.cancel();
    }
}

impl DaemonOptions {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self {
            root: root.into(),
            idle_timeout: None,
            runtime_parts: None,
        }
    }
}

pub struct RunningDaemon {
    pub endpoint: DaemonEndpoint,
    shutdown: CancellationToken,
    join: Option<JoinHandle<Result<(), String>>>,
}

impl RunningDaemon {
    pub fn shutdown(&self) {
        self.shutdown.cancel();
    }

    pub async fn wait(mut self) -> Result<(), String> {
        let Some(join) = self.join.take() else {
            return Err("daemon wait called more than once".into());
        };
        join.await
            .map_err(|error| format!("daemon task stopped: {error}"))?
    }
}

pub async fn start_daemon(options: DaemonOptions) -> Result<RunningDaemon, String> {
    let lease = DaemonLease::acquire(&options.root)?;
    let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0))
        .await
        .map_err(|error| format!("bind daemon endpoint: {error}"))?;
    let address = listener
        .local_addr()
        .map_err(|error| format!("inspect daemon endpoint: {error}"))?;
    let daemon_id = Uuid::new_v4();
    let epoch = lease.epoch();
    // A 256-bit random bearer token makes a loopback TCP port equivalent to
    // a private local socket even on platforms without Unix domain sockets.
    let auth_token = format!("{}{}", Uuid::new_v4().simple(), Uuid::new_v4().simple());
    let endpoint = lease.publish(address.to_string(), daemon_id, auth_token.clone())?;
    let shutdown = CancellationToken::new();
    let runtime = DaemonRuntime::new_with_root(
        match options.runtime_parts {
            Some(parts) => parts,
            None => load_runtime_parts()?,
        },
        daemon_id,
        epoch,
        shutdown.clone(),
        options.root.clone(),
    )?;
    runtime.start_mcp().await;
    let clients = Arc::new(AtomicUsize::new(0));
    let join_shutdown = shutdown.clone();
    let join = tokio::spawn(run_server(
        listener,
        lease,
        runtime,
        auth_token,
        options.idle_timeout,
        clients,
        join_shutdown,
    ));
    Ok(RunningDaemon {
        endpoint,
        shutdown,
        join: Some(join),
    })
}

async fn run_server(
    listener: TcpListener,
    _lease: DaemonLease,
    runtime: Arc<DaemonRuntime>,
    auth_token: String,
    idle_timeout: Option<Duration>,
    clients: Arc<AtomicUsize>,
    shutdown: CancellationToken,
) -> Result<(), String> {
    let idle_shutdown = shutdown.clone();
    let idle_runtime = runtime.clone();
    let idle_clients = clients.clone();
    if let Some(timeout) = idle_timeout {
        tokio::spawn(async move {
            let mut became_idle = None;
            let mut ticks = tokio::time::interval(Duration::from_millis(100));
            loop {
                tokio::select! {
                    _ = idle_shutdown.cancelled() => break,
                    _ = ticks.tick() => {
                        let idle = idle_clients.load(Ordering::Acquire) == 0
                            && idle_runtime.active_turn_count().await == 0;
                        if idle {
                            let since = became_idle.get_or_insert_with(tokio::time::Instant::now);
                            if since.elapsed() >= timeout {
                                idle_shutdown.cancel();
                                break;
                            }
                        } else {
                            became_idle = None;
                        }
                    }
                }
            }
        });
    }

    loop {
        tokio::select! {
            _ = shutdown.cancelled() => break,
            accepted = listener.accept() => {
                let (stream, _) = match accepted {
                    Ok(connection) => connection,
                    Err(error) => {
                        // A transient accept failure must not take down the
                        // daemon (and all connected clients).
                        eprintln!("warning: accept daemon client: {error}");
                        continue;
                    }
                };
                clients.fetch_add(1, Ordering::AcqRel);
                let runtime = runtime.clone();
                let token = auth_token.clone();
                let clients = clients.clone();
                tokio::spawn(async move {
                    if let Err(error) = serve_connection_or_health(stream, runtime, token).await {
                        eprintln!("warning: daemon client disconnected: {error}");
                    }
                    clients.fetch_sub(1, Ordering::AcqRel);
                });
            }
        }
    }
    // Cancellation is cooperative inside provider streams. Keep the lease
    // held until all turn tasks have observed it and released their session
    // references; otherwise a replacement daemon could race their cleanup.
    while runtime.active_turn_count().await != 0 {
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    let _ = runtime.events_send_for_shutdown();
    runtime.save_all().await;
    Ok(())
}

async fn serve_connection(
    stream: TcpStream,
    runtime: Arc<DaemonRuntime>,
    auth_token: String,
) -> Result<(), String> {
    let connection = Uuid::new_v4();
    runtime.connect_client(connection);
    let (mut reader, writer) = stream.into_split();
    let writer = Arc::new(tokio::sync::Mutex::new(writer));
    let attached = Arc::new(RwLock::new(None));
    let mut events = runtime.subscribe();
    let event_writer = writer.clone();
    let event_attached = attached.clone();
    let event_runtime = runtime.clone();
    let event_task = tokio::spawn(async move {
        loop {
            match events.recv().await {
                Ok(event) if event_for_connection(&event, &event_attached).await => {
                    if let DaemonEvent::PermissionRequested(request) = &event
                        && !event_runtime.is_permission_approver(connection, &request.session_id)
                    {
                        continue;
                    }
                    let envelope = EventEnvelope {
                        version: PROTOCOL_VERSION,
                        event,
                    };
                    if write_message(&mut *event_writer.lock().await, &envelope)
                        .await
                        .is_err()
                    {
                        break;
                    }
                }
                Ok(_) => {}
                Err(tokio::sync::broadcast::error::RecvError::Lagged(count)) => {
                    let Some(session_id) = event_attached.read().await.clone() else {
                        continue;
                    };
                    let envelope = EventEnvelope {
                        version: PROTOCOL_VERSION,
                        event: DaemonEvent::SnapshotRequired {
                            session_id,
                            reason: format!("client event stream lagged by {count} events"),
                        },
                    };
                    if write_message(&mut *event_writer.lock().await, &envelope)
                        .await
                        .is_err()
                    {
                        break;
                    }
                }
                Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
            }
        }
    });

    let outcome = async {
        loop {
            let payload = tokio::select! {
                _ = runtime.shutdown.cancelled() => return Ok::<(), String>(()),
                payload = read_message(&mut reader) => payload.map_err(|error| error.to_string())?,
            };
            let envelope: RequestEnvelope =
                decode_payload(&payload).map_err(|error| error.message)?;
            let result = if envelope.version != PROTOCOL_VERSION {
                Err(ProtocolError::new(
                    ErrorCode::UnsupportedVersion,
                    format!("unsupported protocol {}", envelope.version),
                ))
            } else if !constant_time_eq(envelope.auth_token.as_bytes(), auth_token.as_bytes()) {
                Err(ProtocolError::new(
                    ErrorCode::Unauthorized,
                    "invalid daemon token",
                ))
            } else {
                runtime
                    .handle(connection, &attached, envelope.request)
                    .await
            };
            let response = ResponseEnvelope {
                version: PROTOCOL_VERSION,
                id: envelope.id,
                result,
            };
            write_message(&mut *writer.lock().await, &response)
                .await
                .map_err(|error| error.to_string())?;
        }
    }
    .await;
    runtime.release_client(connection);
    event_task.abort();
    outcome
}

async fn event_for_connection(event: &DaemonEvent, attached: &RwLock<Option<String>>) -> bool {
    match event {
        DaemonEvent::Session(event) => attached.read().await.as_deref() == Some(&event.session_id),
        DaemonEvent::SessionStatus(status) => {
            attached.read().await.as_deref() == Some(&status.session_id)
        }
        DaemonEvent::SnapshotRequired { session_id, .. }
        | DaemonEvent::TurnCompleted { session_id, .. } => {
            attached.read().await.as_deref() == Some(session_id)
        }
        DaemonEvent::PermissionRequested(request) => {
            attached.read().await.as_deref() == Some(request.session_id.as_str())
        }
        DaemonEvent::PermissionResolved { session_id, .. } => {
            attached.read().await.as_deref() == Some(session_id.as_str())
        }
        DaemonEvent::Todo(event) => {
            attached.read().await.as_deref() == Some(event.session_id.as_str())
        }
        // Memory records may be user- or project-scoped. Until subscriptions
        // carry an authenticated memory view, never broadcast their payloads
        // to arbitrary daemon clients.
        DaemonEvent::Memory(_) => false,
        _ => true,
    }
}

fn constant_time_eq(left: &[u8], right: &[u8]) -> bool {
    if left.len() != right.len() {
        return false;
    }
    left.iter()
        .zip(right)
        .fold(0_u8, |diff, (a, b)| diff | (a ^ b))
        == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn token_comparison_checks_every_byte() {
        assert!(constant_time_eq(b"abc", b"abc"));
        assert!(!constant_time_eq(b"abc", b"abd"));
        assert!(!constant_time_eq(b"abc", b"ab"));
    }
}
