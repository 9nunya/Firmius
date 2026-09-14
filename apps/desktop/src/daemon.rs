//! Daemon process ownership and typed connection lifecycle.

use std::path::PathBuf;
use std::process::Command;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use firmius_client::DaemonClient;
use firmius_core::data_dir;
use firmius_protocol::{Request, Response, SessionSnapshot};

use crate::state::DesktopState;

pub(crate) static STARTED_DAEMON: AtomicBool = AtomicBool::new(false);
static STARTING_DAEMON: AtomicBool = AtomicBool::new(false);

fn endpoint_path() -> PathBuf {
    data_dir().join("daemon.json")
}

fn firmius_executable() -> PathBuf {
    let Ok(current) = std::env::current_exe() else {
        return PathBuf::from("firmius");
    };
    current
        .parent()
        .map(|dir| dir.join("firmius"))
        .filter(|path| path.exists())
        .unwrap_or_else(|| PathBuf::from("firmius"))
}

pub(crate) fn spawn_firmius(args: &[&str]) -> Result<(), String> {
    Command::new(firmius_executable())
        .args(args)
        .spawn()
        .map(|_| ())
        .map_err(|error| format!("could not launch Firmius: {error}"))
}

/// All sockets and reader tasks live on the application runtime, never on a
/// temporary button-handler runtime. Borrowing this runtime cannot shut it down.
pub(crate) fn runtime() -> Result<&'static tokio::runtime::Runtime, String> {
    static RUNTIME: OnceLock<Result<tokio::runtime::Runtime, String>> = OnceLock::new();
    RUNTIME
        .get_or_init(|| {
            tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .thread_name("desktop-io")
                .enable_all()
                .build()
                .map_err(|error| format!("desktop runtime unavailable: {error}"))
        })
        .as_ref()
        .map_err(Clone::clone)
}

pub(crate) async fn connect_or_start_daemon() -> Result<DaemonClient, String> {
    let endpoint = endpoint_path();
    match DaemonClient::connect(endpoint.clone()).await {
        Ok(client) => Ok(client),
        Err(first_error) => {
            let owns_start = STARTING_DAEMON
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                .is_ok();
            if owns_start {
                if let Err(error) = spawn_firmius(&["daemon"]) {
                    STARTING_DAEMON.store(false, Ordering::Release);
                    return Err(error);
                }
                STARTED_DAEMON.store(true, Ordering::Release);
            }
            let mut last_error = first_error.to_string();
            for _ in 0..40 {
                tokio::time::sleep(Duration::from_millis(100)).await;
                match DaemonClient::connect(endpoint.clone()).await {
                    Ok(client) => {
                        if owns_start {
                            STARTING_DAEMON.store(false, Ordering::Release);
                        }
                        return Ok(client);
                    }
                    Err(error) => last_error = error.to_string(),
                }
            }
            if owns_start {
                STARTING_DAEMON.store(false, Ordering::Release);
            }
            Err(format!("daemon did not become ready: {last_error}"))
        }
    }
}

/// Application-level connection for catalog/configuration commands. Session
/// actions use `client_for` so attachment never follows viewport focus.
pub(crate) async fn shared_client(
    state: &std::sync::Arc<std::sync::Mutex<DesktopState>>,
) -> Result<DaemonClient, String> {
    client_for(state, None).await
}

/// A connection is attached to exactly one session for its entire lifetime.
/// The daemon scopes commands, event delivery and approvals to this socket.
pub(crate) async fn client_for(
    state: &std::sync::Arc<std::sync::Mutex<DesktopState>>,
    session: Option<&str>,
) -> Result<DaemonClient, String> {
    // Serialize establishment, not requests, so concurrent first users cannot
    // create competing approval owners for the same session.
    static CONNECT: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());
    let _guard = CONNECT.lock().await;
    let key = session.unwrap_or("").to_string();
    let previous = state.lock().ok().and_then(|s| s.clients.get(&key).cloned());
    if let Some(client) = &previous {
        if !client.is_closed() {
            return Ok(client.clone());
        }
    }
    let client = connect_or_start_daemon().await?;
    if let Some(session) = session {
        if previous
            .as_ref()
            .is_some_and(|old| old.endpoint().epoch != client.endpoint().epoch)
        {
            let mut s = state.lock().map_err(|_| "desktop state lock poisoned")?;
            s.models.remove(session);
            s.snapshots.remove(session);
            s.pending_permissions.retain(|p| p.session_id != session);
        }
        attached_snapshot(&client, session).await?;
        match client.register_permission_approver().await {
            Ok(()) => {}
            Err(firmius_client::ClientError::Remote(error))
                if error.code == firmius_protocol::ErrorCode::Conflict => {}
            Err(error) => return Err(error.to_string()),
        }
    }
    state
        .lock()
        .map_err(|_| "desktop state lock poisoned")?
        .clients
        .insert(key, client.clone());
    Ok(client)
}

pub(crate) async fn shared_snapshot(
    state: &std::sync::Arc<std::sync::Mutex<DesktopState>>,
    session_id: &str,
) -> Result<(DaemonClient, SessionSnapshot), String> {
    let client = client_for(state, Some(session_id)).await?;
    let snapshot = attached_snapshot(&client, session_id).await?;
    Ok((client, snapshot))
}

pub(crate) async fn attached_snapshot(
    client: &DaemonClient,
    session_id: &str,
) -> Result<SessionSnapshot, String> {
    match client
        .request(Request::AttachSession {
            session_id: session_id.to_string(),
            workdir: None,
        })
        .await
        .map_err(|error| error.to_string())?
    {
        Response::Snapshot(snapshot) => Ok(snapshot),
        other => Err(format!(
            "daemon returned an unexpected session response: {other:?}"
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn socket_and_reader_survive_the_creating_action_thread() {
        let rt = runtime().unwrap();
        let listener = rt
            .block_on(tokio::net::TcpListener::bind("127.0.0.1:0"))
            .unwrap();
        let address = listener.local_addr().unwrap().to_string();
        let endpoint: firmius_protocol::DaemonEndpoint =
            serde_json::from_value(serde_json::json!({
                "version": firmius_protocol::PROTOCOL_VERSION, "address": address,
                "auth_token": "fixture", "daemon_id": "00000000-0000-0000-0000-000000000001",
                "epoch": "00000000-0000-0000-0000-000000000002", "pid": 0,
            }))
            .unwrap();
        let path = std::env::temp_dir().join(format!(
            "desktop-runtime-{}-{}.json",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::write(&path, serde_json::to_vec(&endpoint).unwrap()).unwrap();
        let server = rt.spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            for _ in 0..3 {
                let bytes = firmius_client::read_message(&mut stream).await.unwrap();
                let request: firmius_protocol::RequestEnvelope =
                    firmius_protocol::decode_payload(&bytes).unwrap();
                firmius_client::write_message(
                    &mut stream,
                    &firmius_protocol::ResponseEnvelope {
                        version: firmius_protocol::PROTOCOL_VERSION,
                        id: request.id,
                        result: Ok(Response::Pong {
                            daemon_id: endpoint.daemon_id,
                            epoch: endpoint.epoch,
                        }),
                    },
                )
                .await
                .unwrap();
            }
        });
        let source = path.clone();
        let client = std::thread::spawn(move || {
            runtime()
                .unwrap()
                .block_on(DaemonClient::connect(source))
                .unwrap()
        })
        .join()
        .unwrap();
        let observer = client.subscribe();
        let client = std::thread::spawn(move || {
            assert!(matches!(
                runtime()
                    .unwrap()
                    .block_on(client.request(Request::Ping))
                    .unwrap(),
                Response::Pong { .. }
            ));
            client
        })
        .join()
        .unwrap();
        assert!(matches!(
            rt.block_on(client.request(Request::Ping)).unwrap(),
            Response::Pong { .. }
        ));
        rt.block_on(server).unwrap();
        drop(observer);
        rt.block_on(client.close());
        std::fs::remove_file(path).unwrap();
    }
}
