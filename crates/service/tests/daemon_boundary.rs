//! End-to-end checks for the daemon's local IPC boundary.
//!
//! These tests deliberately use a provider implemented in this crate.  They
//! exercise the daemon, client, persistence, and event plumbing without
//! making an external API request.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use async_trait::async_trait;
use firmius_client::{DaemonClient, read_frame, write_message};
use firmius_core::{
    AccountKind, AccountRecord, ApiType, FirmiusConfig, McpManager, McpSettings, Message,
    MessageRole, PersonaManager, Provider, ProviderError, ProviderEvent, ProviderManager,
    ProviderRequest, ProviderSchema, SetupWizard, Step, ToolRegistry, Usage, UserSettings,
    memory::resolve_project_identity,
};
use firmius_protocol::{
    CreateSessionRequest, DaemonEvent, ErrorCode, MemoryKindDto, MemoryOperationRequest,
    MemoryOperationResponse, MemoryRememberRequest, MemoryRequest, MemoryRetrieveRequest,
    MemoryScopeDto, MemoryViewDto, NewMemoryDto, Request, RequestEnvelope, Response,
    ResponseEnvelope, SubmitTurnRequest, decode_payload,
};
use firmius_service::{DaemonOptions, start_daemon};
use futures::StreamExt;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::Notify;
use tokio_util::sync::CancellationToken;

// The core persistence API intentionally follows HOME.  Keep these tests
// serialized while each test gives itself a private HOME.
static HOME_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

fn test_root(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!(
        "firmius-boundary-{name}-{}-{}",
        std::process::id(),
        uuid::Uuid::new_v4()
    ))
}

#[tokio::test]
async fn health_endpoint_returns_http_ok() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("health");
    use_home(&root);
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Immediate,
                started: Arc::new(Notify::new()),
            }),
        )),
    })
    .await
    .unwrap();

    let mut stream = TcpStream::connect(&running.endpoint.address).await.unwrap();
    stream
        .write_all(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
        .await
        .unwrap();
    let mut response = Vec::new();
    stream.read_to_end(&mut response).await.unwrap();
    let response = String::from_utf8(response).unwrap();
    assert!(response.starts_with("HTTP/1.1 200 OK\r\n"));
    assert!(response.ends_with("\r\n\r\nok"));

    stop(running).await;
    std::fs::remove_dir_all(root).ok();
}

fn use_home(root: &Path) {
    // `dirs::home_dir` reads HOME on Unix.  This is safe here because the
    // process-wide lock above excludes all other tests in this module.
    unsafe { std::env::set_var("HOME", root) };
}

fn fixture_parts(root: &Path, provider: Arc<dyn Provider>) -> firmius_service::RuntimeParts {
    let mut manager = ProviderManager::new().with_data_dir(root.join("provider-data"));
    manager.register_kind(Arc::new(FixtureKind { provider }));
    manager.register_account(AccountRecord {
        id: "fixture".into(),
        kind: "fixture".into(),
        schema: ProviderSchema {
            id: "fixture".into(),
            api_type: ApiType::OpenAI,
            base_url: None,
            api_key_env: None,
            models: vec![],
        },
        credentials: serde_json::json!({}),
    });
    firmius_service::RuntimeParts {
        manager: Arc::new(std::sync::Mutex::new(manager)),
        // Exercise the same scoped Lead persona that normal interactive
        // sessions use, rather than an empty test-only persona registry.
        personas: Arc::new(PersonaManager::load_from(root.join("personas")).unwrap()),
        settings: Arc::new(std::sync::Mutex::new(UserSettings::default())),
        config: Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
        tools: Arc::new(ToolRegistry::default()),
        mcp: Arc::new(McpManager::from_settings(McpSettings::default())),
    }
}

fn create_request() -> Request {
    Request::CreateSession(CreateSessionRequest {
        provider_id: "fixture".into(),
        model: "fixture-model".into(),
        effort: None,
        persona: None,
        workdir: None,
    })
}

async fn wait_for_idle(client: &DaemonClient) {
    tokio::time::timeout(Duration::from_secs(3), async {
        loop {
            let Response::Status(status) = client.request(Request::DaemonStatus).await.unwrap()
            else {
                panic!("status request returned the wrong response")
            };
            if status.active_turns == 0 {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("turn did not become idle");
}

async fn stop(running: firmius_service::RunningDaemon) {
    running.shutdown();
    running.wait().await.unwrap();
}

struct FixtureKind {
    provider: Arc<dyn Provider>,
}

impl AccountKind for FixtureKind {
    fn name(&self) -> &str {
        "fixture"
    }
    fn display_name(&self) -> &str {
        "test fixture"
    }
    fn build_provider(
        &self,
        _schema: &ProviderSchema,
        _credentials: &serde_json::Value,
    ) -> Result<Arc<dyn Provider>, String> {
        Ok(self.provider.clone())
    }
    fn wizard(&self) -> Box<dyn SetupWizard> {
        Box::new(NoopWizard)
    }
}

struct NoopWizard;

#[async_trait]
impl SetupWizard for NoopWizard {
    async fn start(&mut self) -> Step {
        Step::Prompt {
            label: "unused".into(),
            secret: false,
        }
    }
    async fn answer(
        &mut self,
        _input: String,
    ) -> Result<firmius_core::Outcome, firmius_core::WizardError> {
        Err(firmius_core::WizardError::InvalidAnswer("unused".into()))
    }
}

struct LocalProvider {
    mode: ProviderMode,
    started: Arc<Notify>,
}

struct CapturingProvider {
    requests: Arc<Mutex<Vec<ProviderRequest>>>,
}

#[async_trait]
impl Provider for CapturingProvider {
    fn id(&self) -> &str {
        "fixture"
    }

    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<
        futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
        ProviderError,
    > {
        self.requests.lock().unwrap().push(request);
        Ok(futures::stream::iter([
            Ok(ProviderEvent::TextDelta { delta: "ok".into() }),
            Ok(ProviderEvent::Done {
                reason: firmius_core::StopReason::Stop,
            }),
        ])
        .boxed())
    }
}

#[derive(Clone, Copy)]
enum ProviderMode {
    Immediate,
    Delayed,
    Hanging,
    Burst,
}

#[async_trait]
impl Provider for LocalProvider {
    fn id(&self) -> &str {
        "fixture"
    }

    async fn stream(
        &self,
        _request: ProviderRequest,
    ) -> Result<
        futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
        ProviderError,
    > {
        self.started.notify_one();
        let stream: futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>> =
            match self.mode {
                ProviderMode::Immediate => futures::stream::iter([
                    Ok(ProviderEvent::Usage {
                        usage: Usage {
                            input_tokens: 120,
                            output_tokens: 30,
                            cache_read_tokens: 7,
                            cache_write_tokens: 3,
                        },
                    }),
                    Ok(ProviderEvent::TextDelta { delta: "ok".into() }),
                    Ok(ProviderEvent::Done {
                        reason: firmius_core::StopReason::Stop,
                    }),
                ])
                .boxed(),
                ProviderMode::Delayed => futures::stream::once(async {
                    tokio::time::sleep(Duration::from_millis(100)).await;
                    Ok(ProviderEvent::TextDelta {
                        delta: "after disconnect".into(),
                    })
                })
                .chain(futures::stream::once(async {
                    Ok(ProviderEvent::Done {
                        reason: firmius_core::StopReason::Stop,
                    })
                }))
                .boxed(),
                ProviderMode::Hanging => futures::stream::once(async {
                    Ok(ProviderEvent::TextDelta {
                        delta: "partial".into(),
                    })
                })
                .chain(futures::stream::pending())
                .boxed(),
                ProviderMode::Burst => futures::stream::iter(
                    (0..20000).map(|_| Ok(ProviderEvent::TextDelta { delta: "x".into() })),
                )
                .chain(futures::stream::once(async {
                    Ok(ProviderEvent::Done {
                        reason: firmius_core::StopReason::Stop,
                    })
                }))
                .boxed(),
            };
        Ok(stream)
    }
}

#[tokio::test]
async fn lock_auth_and_endpoint_cleanup() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("lock");
    use_home(&root);
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Immediate,
                started: Arc::new(Notify::new()),
            }),
        )),
    })
    .await
    .unwrap();
    assert!(start_daemon(DaemonOptions::new(&root)).await.is_err());

    let mut stream = TcpStream::connect(&running.endpoint.address).await.unwrap();
    write_message(
        &mut stream,
        &RequestEnvelope::new(Request::Ping, "wrong-token"),
    )
    .await
    .unwrap();
    let response: ResponseEnvelope =
        decode_payload(&read_frame(&mut stream).await.unwrap()).unwrap();
    assert_eq!(response.result.unwrap_err().code, ErrorCode::Unauthorized);
    drop(stream);
    stop(running).await;
    assert!(!firmius_service::endpoint_path(&root).exists());
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn multiple_clients_receive_events_and_detach_independently() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("exclusive");
    use_home(&root);
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Immediate,
                started: Arc::new(Notify::new()),
            }),
        )),
    })
    .await
    .unwrap();
    let first = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let Response::Snapshot(created) = first.request(create_request()).await.unwrap() else {
        panic!()
    };
    let second = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let mut first_events = first.subscribe();
    let mut second_events = second.subscribe();
    assert!(matches!(
        second
            .request(Request::AttachSession {
                session_id: created.session_id.clone(),
                workdir: None,
            })
            .await
            .unwrap(),
        Response::Snapshot(_)
    ));
    first.register_permission_approver().await.unwrap();
    assert!(matches!(second.register_permission_approver().await,
        Err(firmius_client::ClientError::Remote(e)) if e.code == ErrorCode::Conflict));
    first
        .request(Request::SubmitTurn(SubmitTurnRequest {
            agent_id: created.primary_agent_id.clone(),
            message: Message::text(MessageRole::User, "hello"),
        }))
        .await
        .unwrap();
    for events in [&mut first_events, &mut second_events] {
        tokio::time::timeout(Duration::from_secs(3), async {
            loop {
                if let DaemonEvent::Session(event) = events.recv().await.unwrap() {
                    assert_eq!(event.session_id, created.session_id);
                    break;
                }
            }
        })
        .await
        .unwrap();
    }
    let Response::SessionEvents {
        events,
        earliest,
        latest,
    } = second
        .request(Request::SessionEvents { after: 0 })
        .await
        .unwrap()
    else {
        panic!("expected session replay")
    };
    assert!(!events.is_empty());
    assert!(earliest <= latest);
    assert!(
        events
            .windows(2)
            .all(|pair| pair[0].sequence < pair[1].sequence)
    );
    assert!(
        events
            .iter()
            .all(|event| event.session_id == created.session_id)
    );
    let Response::SessionEvents { events: tail, .. } = second
        .request(Request::SessionEvents { after: latest })
        .await
        .unwrap()
    else {
        panic!("expected session replay")
    };
    assert!(tail.iter().all(|event| event.sequence > latest));
    first.request(Request::DetachSession).await.unwrap();
    assert!(matches!(
        second.request(Request::Snapshot).await.unwrap(),
        Response::Snapshot(_)
    ));
    second.register_permission_approver().await.unwrap();
    first.close().await;
    assert!(matches!(
        second.request(Request::Snapshot).await.unwrap(),
        Response::Snapshot(_)
    ));
    second.close().await;
    stop(running).await;
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn create_save_attach_returns_snapshot() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("attach");
    use_home(&root);
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Immediate,
                started: Arc::new(Notify::new()),
            }),
        )),
    })
    .await
    .unwrap();
    let client = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let Response::Snapshot(created) = client.request(create_request()).await.unwrap() else {
        panic!()
    };
    client
        .request(Request::SubmitTurn(SubmitTurnRequest {
            agent_id: created.primary_agent_id.clone(),
            message: Message::text(MessageRole::User, "measure"),
        }))
        .await
        .unwrap();
    wait_for_idle(&client).await;
    client
        .request(Request::SetTitle {
            title: Some("boundary".into()),
        })
        .await
        .unwrap();
    client.request(Request::SaveSession).await.unwrap();
    drop(client);
    let second = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let Response::Snapshot(attached) = second
        .request(Request::AttachSession {
            session_id: created.session_id.clone(),
            workdir: None,
        })
        .await
        .unwrap()
    else {
        panic!()
    };
    assert_eq!(attached.session_id, created.session_id);
    assert_eq!(attached.agents[0].usage.input_tokens, 120);
    assert_eq!(attached.agents[0].total_usage.input_tokens, 120);
    assert_eq!(attached.agents[0].total_usage.output_tokens, 30);
    assert_eq!(attached.title.as_deref(), Some("boundary"));
    drop(second);
    stop(running).await;
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn memory_mutations_are_scoped_durable_and_retrievable_over_ipc() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("memory-ipc");
    let project = root.join("project-a");
    std::fs::create_dir_all(&project).unwrap();
    use_home(&root);
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Immediate,
                started: Arc::new(Notify::new()),
            }),
        )),
    })
    .await
    .unwrap();
    let client = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let Response::Snapshot(snapshot) = client
        .request(Request::CreateSession(CreateSessionRequest {
            workdir: Some(project.to_string_lossy().into_owned()),
            ..match create_request() {
                Request::CreateSession(request) => request,
                _ => unreachable!(),
            }
        }))
        .await
        .unwrap()
    else {
        panic!("expected session snapshot")
    };
    let project_id = resolve_project_identity(&project).project_id;
    let memory = NewMemoryDto {
        scope: MemoryScopeDto::Project {
            project_id: project_id.clone(),
        },
        kind: MemoryKindDto::Decision,
        title: "Preferred build command".into(),
        body: "Use cargo nextest for the full Rust test suite.".into(),
        tags: vec!["testing".into()],
        evidence: vec![],
        confidence: 1.0,
    };
    let Response::Memory(remembered) = client
        .request(Request::Memory(MemoryRequest::new(
            MemoryOperationRequest::Remember(MemoryRememberRequest {
                memory,
                expected_revision: Some(0),
            }),
        )))
        .await
        .unwrap()
    else {
        panic!("expected memory response")
    };
    let record_id = match remembered.result {
        MemoryOperationResponse::Remembered { record } => record.id,
        other => panic!("unexpected memory response: {other:?}"),
    };
    assert_eq!(remembered.revision, 1);

    let Response::Memory(retrieved) = client
        .request(Request::Memory(MemoryRequest::new(
            MemoryOperationRequest::Retrieve(MemoryRetrieveRequest {
                query: "nextest full Rust suite".into(),
                view: MemoryViewDto {
                    include_user: true,
                    project_id: Some(project_id.clone()),
                    session_id: Some(snapshot.session_id.clone()),
                },
                limit: 20,
            }),
        )))
        .await
        .unwrap()
    else {
        panic!("expected memory response")
    };
    let hits = match retrieved.result {
        MemoryOperationResponse::Retrieved { hits } => hits,
        other => panic!("unexpected memory response: {other:?}"),
    };
    assert_eq!(hits.len(), 1);
    assert_eq!(hits[0].record.id, record_id);
    assert_eq!(
        hits[0].record.body,
        "Use cargo nextest for the full Rust test suite."
    );

    let rejected = client
        .request(Request::Memory(MemoryRequest::new(
            MemoryOperationRequest::Retrieve(MemoryRetrieveRequest {
                query: "nextest".into(),
                view: MemoryViewDto {
                    include_user: true,
                    project_id: Some("different-project".into()),
                    session_id: None,
                },
                limit: 20,
            }),
        )))
        .await;
    assert!(matches!(rejected,
        Err(firmius_client::ClientError::Remote(error)) if error.code == ErrorCode::Unauthorized));

    client.close().await;
    stop(running).await;
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn lead_turn_receives_bounded_untrusted_memory_context() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("memory-injection");
    let project = root.join("project-a");
    std::fs::create_dir_all(&project).unwrap();
    use_home(&root);
    let requests = Arc::new(Mutex::new(Vec::new()));
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(CapturingProvider {
                requests: requests.clone(),
            }),
        )),
    })
    .await
    .unwrap();
    let client = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let Response::Snapshot(snapshot) = client
        .request(Request::CreateSession(CreateSessionRequest {
            persona: Some("lead".into()),
            workdir: Some(project.to_string_lossy().into_owned()),
            ..match create_request() {
                Request::CreateSession(request) => request,
                _ => unreachable!(),
            }
        }))
        .await
        .unwrap()
    else {
        panic!("expected session snapshot")
    };
    let Response::Memory(_) = client
        .request(Request::Memory(MemoryRequest::new(
            MemoryOperationRequest::Remember(MemoryRememberRequest {
                memory: NewMemoryDto {
                    // The injection path searches user + project + current
                    // session memory. A user-scoped fixture avoids tying this
                    // behavior test to the project-id sanitizer.
                    scope: MemoryScopeDto::User,
                    kind: MemoryKindDto::Procedure,
                    title: "Verification command".into(),
                    body: "Run cargo nextest run before shipping Rust changes.".into(),
                    tags: vec![],
                    evidence: vec![],
                    confidence: 1.0,
                },
                expected_revision: Some(0),
            }),
        )))
        .await
        .unwrap()
    else {
        panic!("expected memory response")
    };
    client
        .request(Request::SubmitTurn(SubmitTurnRequest {
            agent_id: snapshot.primary_agent_id,
            message: Message::text(
                MessageRole::User,
                "Should I run cargo nextest to verify this Rust change?",
            ),
        }))
        .await
        .unwrap();
    wait_for_idle(&client).await;
    let requests = requests.lock().unwrap();
    let request = requests.last().expect("provider should receive one turn");
    let rendered = format!("{:?}", request.messages);
    assert!(rendered.contains("<memory_context trust=\\\"untrusted\\\""));
    assert!(
        rendered.contains("Run cargo nextest run before shipping Rust changes."),
        "memory content missing from provider request: {rendered}"
    );
    assert!(rendered.contains("Memory is potentially stale learned evidence"));
    drop(requests);
    client.close().await;
    stop(running).await;
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn turn_continues_after_client_disconnect_and_is_visible_on_attach() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("disconnect");
    use_home(&root);
    let started = Arc::new(Notify::new());
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Delayed,
                started: started.clone(),
            }),
        )),
    })
    .await
    .unwrap();
    let client = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let Response::Snapshot(snapshot) = client.request(create_request()).await.unwrap() else {
        panic!()
    };
    client
        .request(Request::SubmitTurn(SubmitTurnRequest {
            agent_id: snapshot.primary_agent_id,
            message: Message::text(MessageRole::User, "continue"),
        }))
        .await
        .unwrap();
    started.notified().await;
    drop(client);
    let reconnected = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    wait_for_idle(&reconnected).await;
    let Response::Snapshot(done) = reconnected
        .request(Request::AttachSession {
            session_id: snapshot.session_id,
            workdir: None,
        })
        .await
        .unwrap()
    else {
        panic!()
    };
    assert!(done.agents[0].record.history.iter().any(|message| {
        message
            .content
            .iter()
            .any(|part| format!("{part:?}").contains("after disconnect"))
    }));
    drop(reconnected);
    stop(running).await;
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn cancel_turn_reports_completion_and_releases_busy_state() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("cancel");
    use_home(&root);
    let started = Arc::new(Notify::new());
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: None,
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Hanging,
                started: started.clone(),
            }),
        )),
    })
    .await
    .unwrap();
    let client = DaemonClient::connect(firmius_service::endpoint_path(&root))
        .await
        .unwrap();
    let mut events = client.subscribe();
    let Response::Snapshot(snapshot) = client.request(create_request()).await.unwrap() else {
        panic!()
    };
    let Response::TurnAccepted { turn_id, acceptance_sequence } = client
        .request(Request::SubmitTurn(SubmitTurnRequest {
            agent_id: snapshot.primary_agent_id,
            message: Message::text(MessageRole::User, "cancel"),
        }))
        .await
        .unwrap()
    else {
        panic!()
    };
    assert!(acceptance_sequence.is_some_and(|sequence| sequence >= snapshot.sequence));
    started.notified().await;
    client
        .request(Request::CancelTurn { turn_id })
        .await
        .unwrap();
    let completed = tokio::time::timeout(Duration::from_secs(3), async {
        loop {
            if let DaemonEvent::TurnCompleted {
                turn_id: completed_id,
                result,
                ..
            } = events.recv().await.unwrap()
                && completed_id == turn_id
            {
                break result;
            }
        }
    })
    .await
    .unwrap();
    // Cancellation is represented as a completed turn with a cancelled stop
    // reason (rather than a transport/protocol failure).
    assert!(completed.is_ok());
    wait_for_idle(&client).await;
    drop(client);
    stop(running).await;
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn idle_timeout_stops_daemon_and_removes_endpoint() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("idle");
    use_home(&root);
    let endpoint = firmius_service::endpoint_path(&root);
    let running = start_daemon(DaemonOptions {
        root: root.clone(),
        idle_timeout: Some(Duration::from_millis(120)),
        runtime_parts: Some(fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Immediate,
                started: Arc::new(Notify::new()),
            }),
        )),
    })
    .await
    .unwrap();
    tokio::time::timeout(Duration::from_secs(3), running.wait())
        .await
        .unwrap()
        .unwrap();
    assert!(!endpoint.exists());
    std::fs::remove_dir_all(root).ok();
}

#[tokio::test]
async fn event_receiver_reports_lag_when_burst_exceeds_journal_capacity() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("lag");
    use_home(&root);
    let shutdown = CancellationToken::new();
    let runtime = firmius_service::DaemonRuntime::new(
        fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Burst,
                started: Arc::new(Notify::new()),
            }),
        ),
        uuid::Uuid::new_v4(),
        uuid::Uuid::new_v4(),
        shutdown.clone(),
    );
    let mut events = runtime.subscribe();
    let attached = Arc::new(tokio::sync::RwLock::new(None));
    let connection = uuid::Uuid::new_v4();
    runtime.connect_client(connection);
    let Response::Snapshot(snapshot) = runtime
        .handle(connection, &attached, create_request())
        .await
        .unwrap()
    else {
        panic!()
    };
    runtime
        .handle(
            connection,
            &attached,
            Request::SubmitTurn(SubmitTurnRequest {
                agent_id: snapshot.primary_agent_id,
                message: Message::text(MessageRole::User, "burst"),
            }),
        )
        .await
        .unwrap();
    wait_for_runtime_idle(&runtime).await;
    // The relay's session receiver is intentionally left idle while the
    // provider emits a large burst. It must convert broadcast lag into an
    // explicit recovery signal for clients.
    let snapshot_required = tokio::time::timeout(Duration::from_secs(3), async {
        loop {
            if let DaemonEvent::SnapshotRequired { session_id, .. } = events.recv().await.unwrap()
                && session_id == snapshot.session_id
            {
                break true;
            }
        }
    })
    .await
    .unwrap();
    assert!(snapshot_required);
    shutdown.cancel();
    std::fs::remove_dir_all(root).ok();
}

async fn wait_for_runtime_idle(runtime: &firmius_service::DaemonRuntime) {
    tokio::time::timeout(Duration::from_secs(3), async {
        while runtime.active_turn_count().await != 0 {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .unwrap();
}

/// A failed independent goal check must re-arm itself: the retry turn is
/// mailbox-driven, so unless the runtime tracks its completion, the checks
/// would only ever run after the FIRST goal turn. This drives the whole
/// loop end to end: a command check that fails once (touching a marker),
/// then passes on the retry — the goal must reach Succeeded on its own.
#[tokio::test]
async fn goal_check_failure_retries_and_rechecks_until_passed() {
    let _guard = HOME_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let root = test_root("goal-retry");
    use_home(&root);
    let marker = root.join("goal-check-marker");
    let marker_script = format!(
        "if [ -f '{path}' ]; then exit 0; else touch '{path}'; exit 1; fi",
        path = marker.display()
    );
    let shutdown = CancellationToken::new();
    let runtime = firmius_service::DaemonRuntime::new(
        fixture_parts(
            &root,
            Arc::new(LocalProvider {
                mode: ProviderMode::Immediate,
                started: Arc::new(Notify::new()),
            }),
        ),
        uuid::Uuid::new_v4(),
        uuid::Uuid::new_v4(),
        shutdown.clone(),
    );
    let attached = Arc::new(tokio::sync::RwLock::new(None));
    let connection = uuid::Uuid::new_v4();
    runtime.connect_client(connection);
    let Response::Snapshot(snapshot) = runtime
        .handle(connection, &attached, create_request())
        .await
        .unwrap()
    else {
        panic!()
    };
    let check = firmius_core::GoalCheck {
        id: "marker-check".into(),
        kind: firmius_core::GoalCheckKind::Command(firmius_core::CommandCheck {
            command: "sh".into(),
            args: vec!["-c".into(), marker_script],
            cwd: None,
            expected_exit_code: Some(0),
        }),
    };
    let create = firmius_protocol::GoalRequest::Create(firmius_protocol::CreateGoalRequest {
        description: "fail once, then pass".into(),
        success_conditions: vec!["marker exists".into()],
        owner: firmius_core::GoalOwner::User {
            user_id: "test".into(),
        },
        provenance: firmius_core::GoalProvenance {
            actor: firmius_core::GoalActor::User {
                user_id: "test".into(),
            },
            source: firmius_core::GoalSource::UserRequest,
            created_at: chrono::Utc::now(),
        },
        checks: vec![check],
        deadline: None,
        budget: None,
        approval_required: false,
        client_request_id: None,
    });
    let Response::Goal(firmius_protocol::GoalResponse::Created(goal)) = runtime
        .handle(connection, &attached, Request::Goal(create))
        .await
        .unwrap()
    else {
        panic!()
    };
    assert_eq!(goal.status, firmius_core::GoalStatus::Active);

    // The first check fails (creating the marker); the runtime must queue a
    // retry turn and re-run the checks after it — without any further input.
    let goal_id = goal.id;
    let settled = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            let Response::Goal(firmius_protocol::GoalResponse::Retrieved(current)) = runtime
                .handle(
                    connection,
                    &attached,
                    Request::Goal(firmius_protocol::GoalRequest::Get(
                        firmius_protocol::GetGoalRequest { goal_id },
                    )),
                )
                .await
                .unwrap()
            else {
                panic!()
            };
            if current.status.terminal() {
                break current;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    })
    .await
    .expect("goal never settled: the check-failed retry turn was not tracked");
    assert_eq!(settled.status, firmius_core::GoalStatus::Succeeded);
    assert!(
        settled.evaluations.len() >= 2,
        "the check must run once per goal turn: {:?}",
        settled.evaluations
    );
    assert!(
        settled
            .evaluations
            .first()
            .is_some_and(|evaluation| { evaluation.state == firmius_core::CheckState::Failed }),
        "the first evaluation must be the failing one: {:?}",
        settled.evaluations
    );

    // The worker must have been told about the failed check and retried.
    let Response::Snapshot(done) = runtime
        .handle(connection, &attached, Request::Snapshot)
        .await
        .unwrap()
    else {
        panic!()
    };
    let worker_record = done
        .agents
        .iter()
        .find(|agent| agent.record.id == snapshot.primary_agent_id)
        .expect("worker agent record");
    assert!(
        worker_record.record.history.iter().any(|message| {
            message.role == MessageRole::User
                && message.content.iter().any(|part| {
                    matches!(part, firmius_core::MessagePart::Text(text) if text.contains("untrusted_goal_retry_evidence"))
                })
        }),
        "worker history should contain the retry instruction"
    );
    shutdown.cancel();
    std::fs::remove_dir_all(root).ok();
}