//! Desktop memory inspector: retrieval is daemon-authenticated and results are
//! projected into a normal feature document for readable, persistent browsing.
use super::*;

pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    ui.on_search_memory(move |query| {
        let query = query.trim().to_string();
        let Some(ui) = weak.upgrade() else { return; };
        if query.is_empty() {
            set_notice(&ui, "Enter a memory search query");
            return;
        }
        let target = state.lock().ok().and_then(|s| {
            let session = s.active_session()?;
            let snapshot = s.snapshots.get(&session)?;
            let project_id = snapshot.agents.iter()
                .find(|agent| agent.record.id == snapshot.primary_agent_id)
                .map(|agent| firmius_core::memory::resolve_project_identity(&agent.record.workdir).project_id);
            Some((session, project_id))
        });
        let Some((session, project_id)) = target else {
            set_notice(&ui, "Memory search requires an attached session");
            return;
        };
        let weak = ui.as_weak();
        let state = state.clone();
        thread::spawn(move || {
            let result = runtime().and_then(|rt| rt.block_on(async {
                let client = crate::daemon::client_for(&state, Some(&session)).await?;
                client.retrieve_memory(firmius_protocol::MemoryRetrieveRequest {
                    query: query.clone(),
                    view: firmius_protocol::MemoryViewDto { include_user: true, project_id, session_id: Some(session.clone()) },
                    limit: 20,
                }).await.map_err(|error| error.to_string())
            }));
            invoke(weak, move |ui| match result {
                Ok(response) => match response.result {
                    firmius_protocol::MemoryOperationResponse::Retrieved { hits } => {
                        let mut rows = vec![TranscriptRow {
                            key: "memory-warning".into(), author: "MEMORY".into(),
                            body: "Potentially stale learned evidence. Current user instructions and direct verification take precedence.".into(),
                            tone: "thinking".into(), detail: "Untrusted context".into(), presenter: "text".into(), expandable: false, blocks: Default::default(), activity: Default::default(),
                        }];
                        rows.extend(hits.into_iter().enumerate().map(|(index, hit)| {
                            let scope = match hit.record.scope { firmius_protocol::MemoryScopeDto::User => "User", firmius_protocol::MemoryScopeDto::Project { .. } => "Project", firmius_protocol::MemoryScopeDto::Session { .. } => "Session" };
                            let evidence = if hit.record.evidence.is_empty() {
                                "No retained evidence excerpt.".to_string()
                            } else {
                                hit.record.evidence.iter().map(|evidence| {
                                    let locator = evidence.locator.as_deref().map(|locator| format!(" · {locator}")).unwrap_or_default();
                                    format!("• {}{}\n  {}", evidence.kind, locator, evidence.excerpt)
                                }).collect::<Vec<_>>().join("\n")
                            };
                            let tags = if hit.record.tags.is_empty() { "untagged".to_string() } else { hit.record.tags.join(", ") };
                            TranscriptRow { key: format!("memory-{index}").into(), author: format!("{scope} memory · active").into(), body: format!("{}\n\nEvidence\n{}\n\nTags: {}\nRecord: {}", hit.record.body, evidence, tags, hit.record.id).into(), tone: "assistant".into(), detail: format!("{} · relevance {:.2} · confidence {:.0}% · v{}", hit.record.title, hit.score, hit.record.confidence * 100.0, hit.record.record_version).into(), presenter: "text".into(), expandable: false, blocks: Default::default(), activity: Default::default() }
                        }));
                        ui.set_memory_open(false);
                        ui.invoke_show_document("Memory Inspector".into(), ModelRc::new(VecModel::from(rows)));
                    }
                    _ => set_notice(ui, "Memory search returned an unexpected response"),
                },
                Err(error) => set_notice(ui, format!("Memory search failed: {error}")),
            });
        });
    });
}
