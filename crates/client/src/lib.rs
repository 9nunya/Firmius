//! Reconnecting typed client for the local Firmius daemon.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use firmius_protocol::{
    ChunkFrame, DaemonEndpoint, DaemonEvent, EventEnvelope, MAX_FRAME_BYTES, MAX_MESSAGE_BYTES,
    MemoryCorrectRequest, MemoryForgetRequest, MemoryInspectRequest, MemoryOperationRequest,
    MemoryOperationResponse, MemoryRememberRequest, MemoryRequest, MemoryResponse,
    MemoryRetrieveRequest, PROTOCOL_VERSION, PermissionMode, PermissionPolicy,
    PermissionResolution, ProtocolError, Request, RequestEnvelope, Response, ResponseEnvelope,
    decode_payload, encode_frames,
};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::{Mutex, broadcast, oneshot};
use tokio::time::{Duration, timeout};
use uuid::Uuid;

#[derive(Debug, thiserror::Error)]
pub enum ClientError {
    #[error("daemon endpoint {path} could not be read: {source}")]
    EndpointIo {
        path: PathBuf,
        source: std::io::Error,
    },
    #[error("daemon endpoint is invalid: {0}")]
    EndpointJson(serde_json::Error),
    #[error("unsupported daemon protocol {0}")]
    UnsupportedVersion(u32),
    #[error("daemon connection failed: {0}")]
    Connect(std::io::Error),
    #[error("daemon connection closed")]
    Disconnected,
    #[error("daemon I/O failed: {0}")]
    Io(std::io::Error),
    #[error("daemon protocol failed: {0}")]
    Protocol(String),
    #[error("daemon request failed: {0}")]
    Remote(ProtocolError),
}

pub async fn read_message<R: AsyncRead + Unpin>(reader: &mut R) -> Result<Vec<u8>, ClientError> {
    let first = read_frame(reader).await?;
    let value: serde_json::Value =
        decode_payload(&first).map_err(|error| ClientError::Protocol(error.message))?;
    if value.get("_firmius_chunk") != Some(&serde_json::Value::Bool(true)) {
        return Ok(first);
    }

    let chunk: ChunkFrame =
        serde_json::from_value(value).map_err(|error| ClientError::Protocol(error.to_string()))?;
    validate_chunk(&chunk)?;
    if chunk.index != 0 {
        return Err(ClientError::Protocol(
            "chunk transfer must start at index zero".into(),
        ));
    }
    let expected = chunk.aggregate_len as usize;
    let mut assembled = Vec::with_capacity(expected);
    assembled.extend_from_slice(&chunk.data);
    for index in 1..chunk.total {
        let payload = read_frame(reader).await?;
        let value: serde_json::Value =
            decode_payload(&payload).map_err(|error| ClientError::Protocol(error.message))?;
        let next: ChunkFrame = serde_json::from_value(value)
            .map_err(|error| ClientError::Protocol(error.to_string()))?;
        validate_chunk(&next)?;
        if next.transfer_id != chunk.transfer_id
            || next.total != chunk.total
            || next.aggregate_len != chunk.aggregate_len
            || next.index != index
        {
            return Err(ClientError::Protocol(
                "invalid chunk ordering or transfer metadata".into(),
            ));
        }
        assembled.extend_from_slice(&next.data);
        if assembled.len() > expected {
            return Err(ClientError::Protocol(
                "chunk aggregate exceeds declared size".into(),
            ));
        }
    }
    if assembled.len() != expected {
        return Err(ClientError::Protocol("incomplete chunk transfer".into()));
    }
    Ok(assembled)
}

fn validate_chunk(chunk: &ChunkFrame) -> Result<(), ClientError> {
    if chunk.total == 0
        || chunk.total as usize > firmius_protocol::MAX_CHUNKS
        || chunk.index >= chunk.total
        || chunk.aggregate_len == 0
        || chunk.aggregate_len as usize > MAX_MESSAGE_BYTES
        || chunk.data.is_empty()
        || chunk.data.len() > firmius_protocol::CHUNK_DATA_BYTES
    {
        return Err(ClientError::Protocol(
            "invalid chunk limits or metadata".into(),
        ));
    }
    Ok(())
}

struct Inner {
    endpoint: DaemonEndpoint,
    endpoint_path: PathBuf,
    writer: Mutex<tokio::net::tcp::OwnedWriteHalf>,
    pending: Arc<Mutex<HashMap<Uuid, oneshot::Sender<Result<Response, ClientError>>>>>,
    events: broadcast::Sender<DaemonEvent>,
    closed: Arc<AtomicBool>,
}

#[derive(Clone)]
pub struct DaemonClient {
    inner: Arc<Inner>,
}

impl DaemonClient {
    async fn memory_request(
        &self,
        operation: MemoryOperationRequest,
    ) -> Result<MemoryResponse, ClientError> {
        match self
            .request(Request::Memory(MemoryRequest::new(operation)))
            .await?
        {
            Response::Memory(response) => Ok(response),
            other => Err(ClientError::Protocol(format!(
                "unexpected response: {other:?}"
            ))),
        }
    }

    pub async fn inspect_memory(
        &self,
        request: MemoryInspectRequest,
    ) -> Result<MemoryResponse, ClientError> {
        self.memory_request(MemoryOperationRequest::Inspect(request))
            .await
    }

    pub async fn retrieve_memory(
        &self,
        request: MemoryRetrieveRequest,
    ) -> Result<MemoryResponse, ClientError> {
        self.memory_request(MemoryOperationRequest::Retrieve(request))
            .await
    }

    pub async fn remember_memory(
        &self,
        request: MemoryRememberRequest,
    ) -> Result<MemoryResponse, ClientError> {
        self.memory_request(MemoryOperationRequest::Remember(request))
            .await
    }

    pub async fn correct_memory(
        &self,
        request: MemoryCorrectRequest,
    ) -> Result<MemoryResponse, ClientError> {
        self.memory_request(MemoryOperationRequest::Correct(request))
            .await
    }

    pub async fn forget_memory(
        &self,
        request: MemoryForgetRequest,
    ) -> Result<MemoryResponse, ClientError> {
        self.memory_request(MemoryOperationRequest::Forget(request))
            .await
    }

    /// Convenience matcher for callers interested only in the typed result.
    pub fn memory_result(response: MemoryResponse) -> MemoryOperationResponse {
        response.result
    }

    pub async fn connect(endpoint_path: impl Into<PathBuf>) -> Result<Self, ClientError> {
        let endpoint_path = endpoint_path.into();
        let endpoint = read_endpoint(&endpoint_path)?;
        if endpoint.version != PROTOCOL_VERSION {
            return Err(ClientError::UnsupportedVersion(endpoint.version));
        }
        let stream = TcpStream::connect(&endpoint.address)
            .await
            .map_err(ClientError::Connect)?;
        let (reader, writer) = stream.into_split();
        let (events, _) = broadcast::channel(4096);
        let inner = Arc::new(Inner {
            endpoint,
            endpoint_path,
            writer: Mutex::new(writer),
            pending: Arc::new(Mutex::new(HashMap::new())),
            events,
            closed: Arc::new(AtomicBool::new(false)),
        });
        tokio::spawn(reader_loop(
            reader,
            inner.pending.clone(),
            inner.events.clone(),
            inner.closed.clone(),
        ));
        let client = Self { inner };
        let ping = client.request(Request::Ping).await;
        let response = match ping {
            Ok(response) => response,
            Err(error) => {
                client.close().await;
                return Err(error);
            }
        };
        match response {
            Response::Pong { daemon_id, epoch }
                if daemon_id == client.inner.endpoint.daemon_id
                    && epoch == client.inner.endpoint.epoch =>
            {
                Ok(client)
            }
            Response::Pong { .. } => {
                client.close().await;
                Err(ClientError::Protocol(
                    "daemon identity does not match endpoint metadata".into(),
                ))
            }
            other => {
                client.close().await;
                Err(ClientError::Protocol(format!(
                    "ping returned unexpected response: {other:?}"
                )))
            }
        }
    }

    /// Close the socket and fail all requests. This is also used when the
    /// handshake fails so a failed connect cannot leave a reader task alive.
    pub async fn close(&self) {
        if self.inner.closed.swap(true, Ordering::AcqRel) {
            return;
        }
        let _ = self.inner.writer.lock().await.shutdown().await;
        fail_pending(&self.inner.pending, ClientError::Disconnected).await;
    }

    pub async fn reconnect(&self) -> Result<Self, ClientError> {
        Self::connect(self.inner.endpoint_path.clone()).await
    }

    pub fn endpoint(&self) -> &DaemonEndpoint {
        &self.inner.endpoint
    }

    pub fn is_closed(&self) -> bool {
        self.inner.closed.load(Ordering::Acquire)
    }

    pub fn subscribe(&self) -> broadcast::Receiver<DaemonEvent> {
        self.inner.events.subscribe()
    }

    pub async fn request(&self, request: Request) -> Result<Response, ClientError> {
        if self.inner.closed.load(Ordering::Acquire) {
            return Err(ClientError::Disconnected);
        }
        let envelope = RequestEnvelope::new(request, self.inner.endpoint.auth_token.clone());
        let id = envelope.id;
        let frames =
            encode_frames(&envelope).map_err(|error| ClientError::Protocol(error.message))?;
        let (tx, rx) = oneshot::channel();
        self.inner.pending.lock().await.insert(id, tx);
        let mut writer = self.inner.writer.lock().await;
        if self.inner.closed.load(Ordering::Acquire) {
            self.inner.pending.lock().await.remove(&id);
            return Err(ClientError::Disconnected);
        }
        let write_result = async {
            for frame in frames {
                writer.write_all(&frame).await?;
            }
            Ok::<(), std::io::Error>(())
        }
        .await;
        if let Err(error) = write_result {
            self.inner.pending.lock().await.remove(&id);
            self.inner.closed.store(true, Ordering::Release);
            fail_pending(
                &self.inner.pending,
                ClientError::Protocol(error.to_string()),
            )
            .await;
            return Err(ClientError::Io(error));
        }
        drop(writer);
        match timeout(Duration::from_secs(60), rx).await {
            Ok(Ok(result)) => result,
            Ok(Err(_)) => Err(ClientError::Disconnected),
            Err(_) => {
                self.inner.pending.lock().await.remove(&id);
                Err(ClientError::Io(std::io::Error::new(
                    std::io::ErrorKind::TimedOut,
                    "daemon request timed out",
                )))
            }
        }
    }

    /// Claim the attached session's interactive approval authority. The
    /// daemon binds this claim to this authenticated connection.
    pub async fn register_permission_approver(&self) -> Result<(), ClientError> {
        match self.request(Request::RegisterPermissionApprover).await? {
            Response::Ack => Ok(()),
            other => Err(ClientError::Protocol(format!(
                "unexpected response: {other:?}"
            ))),
        }
    }

    pub async fn unregister_permission_approver(&self) -> Result<(), ClientError> {
        match self.request(Request::UnregisterPermissionApprover).await? {
            Response::Ack => Ok(()),
            other => Err(ClientError::Protocol(format!(
                "unexpected response: {other:?}"
            ))),
        }
    }

    pub async fn resolve_permission(
        &self,
        resolution: PermissionResolution,
    ) -> Result<(), ClientError> {
        match self.request(Request::ResolvePermission(resolution)).await? {
            Response::Ack => Ok(()),
            other => Err(ClientError::Protocol(format!(
                "unexpected response: {other:?}"
            ))),
        }
    }

    pub async fn permission_policy(&self) -> Result<PermissionPolicy, ClientError> {
        match self.request(Request::GetPermissionPolicy).await? {
            Response::PermissionPolicy(policy) => Ok(policy),
            other => Err(ClientError::Protocol(format!(
                "unexpected response: {other:?}"
            ))),
        }
    }

    pub async fn update_permission_policy(
        &self,
        policy: PermissionPolicy,
        expected_revision: u64,
    ) -> Result<PermissionPolicy, ClientError> {
        match self
            .request(Request::UpdatePermissionPolicy {
                policy,
                expected_revision,
            })
            .await?
        {
            Response::PermissionUpdated(policy) => Ok(policy),
            other => Err(ClientError::Protocol(format!(
                "unexpected response: {other:?}"
            ))),
        }
    }

    pub async fn set_permission_mode(
        &self,
        mode: PermissionMode,
        expected_revision: u64,
    ) -> Result<PermissionPolicy, ClientError> {
        match self
            .request(Request::SetPermissionMode {
                mode,
                expected_revision,
            })
            .await?
        {
            Response::PermissionUpdated(policy) => Ok(policy),
            other => Err(ClientError::Protocol(format!(
                "unexpected response: {other:?}"
            ))),
        }
    }
}

pub fn read_endpoint(path: &Path) -> Result<DaemonEndpoint, ClientError> {
    let bytes = std::fs::read(path).map_err(|source| ClientError::EndpointIo {
        path: path.to_path_buf(),
        source,
    })?;
    serde_json::from_slice(&bytes).map_err(ClientError::EndpointJson)
}

async fn reader_loop(
    mut reader: tokio::net::tcp::OwnedReadHalf,
    pending: Arc<Mutex<HashMap<Uuid, oneshot::Sender<Result<Response, ClientError>>>>>,
    events: broadcast::Sender<DaemonEvent>,
    closed: Arc<AtomicBool>,
) {
    struct ClosedOnDrop(Arc<AtomicBool>);
    impl Drop for ClosedOnDrop {
        fn drop(&mut self) {
            self.0.store(true, Ordering::Release);
        }
    }
    let _closed = ClosedOnDrop(closed);
    loop {
        let payload = match read_message(&mut reader).await {
            Ok(payload) => payload,
            Err(error) => {
                let _ = events.send(DaemonEvent::ConnectionLost {
                    reason: error.to_string(),
                });
                fail_pending(&pending, error).await;
                return;
            }
        };
        let value: serde_json::Value = match decode_payload(&payload) {
            Ok(value) => value,
            Err(error) => {
                let _ = events.send(DaemonEvent::ConnectionLost {
                    reason: error.message.clone(),
                });
                fail_pending(&pending, ClientError::Protocol(error.message)).await;
                return;
            }
        };
        if value.get("id").is_some() {
            let response: ResponseEnvelope = match serde_json::from_value(value) {
                Ok(response) => response,
                Err(error) => {
                    let _ = events.send(DaemonEvent::ConnectionLost {
                        reason: error.to_string(),
                    });
                    fail_pending(&pending, ClientError::Protocol(error.to_string())).await;
                    return;
                }
            };
            if response.version != PROTOCOL_VERSION {
                let _ = events.send(DaemonEvent::ConnectionLost {
                    reason: format!("unsupported protocol version: {}", response.version),
                });
                fail_pending(&pending, ClientError::UnsupportedVersion(response.version)).await;
                return;
            }
            if let Some(tx) = pending.lock().await.remove(&response.id) {
                let _ = tx.send(response.result.map_err(ClientError::Remote));
            }
        } else {
            let event: EventEnvelope = match serde_json::from_value(value) {
                Ok(event) => event,
                Err(error) => {
                    let _ = events.send(DaemonEvent::ConnectionLost {
                        reason: error.to_string(),
                    });
                    fail_pending(&pending, ClientError::Protocol(error.to_string())).await;
                    return;
                }
            };
            if event.version == PROTOCOL_VERSION {
                let _ = events.send(event.event);
            } else {
                let _ = events.send(DaemonEvent::ConnectionLost {
                    reason: format!("unsupported protocol version: {}", event.version),
                });
                fail_pending(&pending, ClientError::UnsupportedVersion(event.version)).await;
                return;
            }
        }
    }
}

async fn fail_pending(
    pending: &Arc<Mutex<HashMap<Uuid, oneshot::Sender<Result<Response, ClientError>>>>>,
    error: ClientError,
) {
    let message = error.to_string();
    let disconnected = matches!(error, ClientError::Disconnected);
    let mut pending = pending.lock().await;
    for (_, tx) in pending.drain() {
        let result = if disconnected {
            Err(ClientError::Disconnected)
        } else {
            Err(ClientError::Protocol(message.clone()))
        };
        let _ = tx.send(result);
    }
}

pub async fn read_frame<R: AsyncRead + Unpin>(reader: &mut R) -> Result<Vec<u8>, ClientError> {
    let mut length = [0_u8; 4];
    reader.read_exact(&mut length).await.map_err(|error| {
        if error.kind() == std::io::ErrorKind::UnexpectedEof {
            ClientError::Disconnected
        } else {
            ClientError::Io(error)
        }
    })?;
    let length = u32::from_be_bytes(length) as usize;
    if length > MAX_FRAME_BYTES {
        return Err(ClientError::Protocol(format!(
            "incoming frame exceeds {MAX_FRAME_BYTES} bytes"
        )));
    }
    let mut payload = vec![0_u8; length];
    reader.read_exact(&mut payload).await.map_err(|error| {
        if error.kind() == std::io::ErrorKind::UnexpectedEof {
            ClientError::Disconnected
        } else {
            ClientError::Io(error)
        }
    })?;
    Ok(payload)
}

pub async fn write_message<W: AsyncWrite + Unpin, T: serde::Serialize>(
    writer: &mut W,
    value: &T,
) -> Result<(), ClientError> {
    for frame in encode_frames(value).map_err(|error| ClientError::Protocol(error.message))? {
        writer.write_all(&frame).await.map_err(ClientError::Io)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use firmius_protocol::{CHUNK_DATA_BYTES, ChunkFrame, encode_frame};
    use std::time::Duration;

    #[tokio::test]
    async fn chunked_message_reassembles_and_rejects_out_of_order_chunks() {
        let (mut client, mut server) = tokio::io::duplex(8192);
        let transfer_id = Uuid::new_v4();
        let first = ChunkFrame {
            marker: true,
            transfer_id,
            index: 0,
            total: 2,
            aggregate_len: 6,
            data: b"abc".to_vec(),
        };
        let second = ChunkFrame {
            marker: true,
            transfer_id,
            index: 1,
            total: 2,
            aggregate_len: 6,
            data: b"def".to_vec(),
        };
        client
            .write_all(&encode_frame(&first).unwrap())
            .await
            .unwrap();
        client
            .write_all(&encode_frame(&second).unwrap())
            .await
            .unwrap();
        assert_eq!(read_message(&mut server).await.unwrap(), b"abcdef");

        let bad = ChunkFrame { index: 1, ..first };
        client
            .write_all(&encode_frame(&bad).unwrap())
            .await
            .unwrap();
        client
            .write_all(&encode_frame(&second).unwrap())
            .await
            .unwrap();
        assert!(matches!(
            read_message(&mut server).await,
            Err(ClientError::Protocol(_))
        ));
        assert!(CHUNK_DATA_BYTES > 0);
    }

    #[tokio::test]
    async fn framing_preserves_consecutive_messages() {
        let (mut client, mut server) = tokio::io::duplex(4096);
        let first = RequestEnvelope::new(Request::Ping, "one");
        let second = RequestEnvelope::new(Request::DaemonStatus, "two");
        write_message(&mut client, &first).await.unwrap();
        write_message(&mut client, &second).await.unwrap();
        let one: RequestEnvelope = decode_payload(&read_frame(&mut server).await.unwrap()).unwrap();
        let two: RequestEnvelope = decode_payload(&read_frame(&mut server).await.unwrap()).unwrap();
        assert_eq!(one.id, first.id);
        assert_eq!(two.id, second.id);
    }

    #[tokio::test]
    async fn oversized_incoming_length_is_rejected_without_allocating() {
        let (mut client, mut server) = tokio::io::duplex(16);
        client
            .write_all(&((MAX_FRAME_BYTES as u32) + 1).to_be_bytes())
            .await
            .unwrap();
        assert!(matches!(
            read_frame(&mut server).await,
            Err(ClientError::Protocol(_))
        ));
    }

    #[tokio::test]
    async fn reader_broadcasts_connection_lost_before_closing() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let (_stream, _) = listener.accept().await.unwrap();
            // Dropping the accepted stream makes the client's read half
            // observe EOF and exercises the same path as a daemon crash.
        });
        let stream = TcpStream::connect(address).await.unwrap();
        let (reader, writer) = stream.into_split();
        let (events, mut rx) = broadcast::channel(4);
        let endpoint = DaemonEndpoint {
            version: PROTOCOL_VERSION,
            address: address.to_string(),
            auth_token: "token".into(),
            daemon_id: Uuid::new_v4(),
            epoch: Uuid::new_v4(),
            pid: 0,
        };
        let inner = Arc::new(Inner {
            endpoint,
            endpoint_path: PathBuf::new(),
            writer: Mutex::new(writer),
            pending: Arc::new(Mutex::new(HashMap::new())),
            events,
            closed: Arc::new(AtomicBool::new(false)),
        });
        tokio::spawn(reader_loop(
            reader,
            inner.pending.clone(),
            inner.events.clone(),
            inner.closed.clone(),
        ));
        let event = tokio::time::timeout(Duration::from_secs(1), rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert!(matches!(event, DaemonEvent::ConnectionLost { .. }));
        server.await.unwrap();
    }

    #[tokio::test]
    async fn malformed_payload_notifies_subscribers_and_fails_pending_requests() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let stream = TcpStream::connect(listener.local_addr().unwrap())
            .await
            .unwrap();
        let (mut server, _) = listener.accept().await.unwrap();
        let (reader, _writer) = stream.into_split();
        let (events, mut rx) = broadcast::channel(4);
        let (tx, response) = oneshot::channel();
        let pending = Arc::new(Mutex::new(HashMap::from([(Uuid::new_v4(), tx)])));
        let closed = Arc::new(AtomicBool::new(false));
        let task = tokio::spawn(reader_loop(reader, pending, events, closed.clone()));
        server.write_all(&1u32.to_be_bytes()).await.unwrap();
        server.write_all(b"{").await.unwrap();
        assert!(matches!(
            timeout(Duration::from_secs(1), rx.recv())
                .await
                .unwrap()
                .unwrap(),
            DaemonEvent::ConnectionLost { .. }
        ));
        assert!(response.await.unwrap().is_err());
        task.await.unwrap();
        assert!(closed.load(Ordering::Acquire));
    }

    #[tokio::test]
    async fn closed_client_rejects_requests_without_writing() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let (_stream, _) = listener.accept().await.unwrap();
            tokio::time::sleep(Duration::from_secs(1)).await;
        });
        let stream = TcpStream::connect(address).await.unwrap();
        let (reader, writer) = stream.into_split();
        let (events, _) = broadcast::channel(4);
        let endpoint = DaemonEndpoint {
            version: PROTOCOL_VERSION,
            address: address.to_string(),
            auth_token: "token".into(),
            daemon_id: Uuid::new_v4(),
            epoch: Uuid::new_v4(),
            pid: 0,
        };
        let inner = Arc::new(Inner {
            endpoint,
            endpoint_path: PathBuf::new(),
            writer: Mutex::new(writer),
            pending: Arc::new(Mutex::new(HashMap::new())),
            events,
            closed: Arc::new(AtomicBool::new(false)),
        });
        let client = DaemonClient {
            inner: inner.clone(),
        };
        tokio::spawn(reader_loop(
            reader,
            inner.pending.clone(),
            inner.events.clone(),
            inner.closed.clone(),
        ));
        client.close().await;
        assert!(matches!(
            client.request(Request::Ping).await,
            Err(ClientError::Disconnected)
        ));
        server.abort();
    }
}
