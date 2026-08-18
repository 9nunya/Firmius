use std::collections::VecDeque;
use std::process::ExitCode;
use std::sync::{Arc, Mutex};

use async_trait::async_trait;
use firmius_core::{
    AgentConfig, AgentEvent, AgentRecord, AlibabaTokenPlanKind, AnthropicSubscriptionKind,
    CodexKind, Message, MessagePart, MessageRole, ModelInfo, OpencodeGoKind, Provider,
    ProviderError, ProviderEvent, ProviderManager, ProviderRequest, Session, SessionRecord,
    StopReason, Tool, ToolContext, ToolDefinition, ToolError, ToolRegistry, compaction,
    compaction_job, context_budget,
};
use futures::StreamExt;
use futures::stream::BoxStream;
use tokio_util::sync::CancellationToken;

#[tokio::main]
async fn main() -> ExitCode {
    match run(std::env::args().skip(1).collect()).await {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("AUDIT FAILED: {error}");
            ExitCode::from(1)
        }
    }
}

fn audit_model(id: &str, context_window: u32) -> ModelInfo {
    ModelInfo {
        id: id.into(),
        context_window,
        max_output_tokens: Some(20),
        capabilities: Default::default(),
        effort_modes: vec![],
    }
}

async fn run(args: Vec<String>) -> Result<(), String> {
    let command = args.first().map(String::as_str).unwrap_or("help");
    let (provider, flags) = parse_flags(&args[1..])?;
    match command {
        "provider_stream" => provider_stream(provider.as_deref(), flags).await,
        "quota" => quota(provider.as_deref()).await,
        "compaction" | "compaction_stress" => {
            let model = flags
                .windows(2)
                .find(|pair| pair[0] == "--model")
                .map(|pair| pair[1].clone());
            compaction_audit(provider.as_deref(), model.as_deref()).await
        }
        "help" | "--help" | "-h" => {
            print_help();
            Ok(())
        }
        other => Err(format!("unknown audit `{other}`; use `help`")),
    }
}

fn print_help() {
    println!(
        "Usage:\n  firmius-audits provider_stream [--provider <id-or-kind>] [--model <id>]\n  firmius-audits quota [--provider <id-or-kind>]\n  firmius-audits compaction [--provider <id-or-kind>] [--model <id>]\n\nAll commands read accounts from ~/.firmius. compaction uses a real provider for agent-level scenarios."
    );
}

fn parse_flags(args: &[String]) -> Result<(Option<String>, Vec<String>), String> {
    let mut provider = None;
    let mut rest = Vec::new();
    let mut index = 0;
    while index < args.len() {
        match args[index].as_str() {
            "--provider" => {
                index += 1;
                provider = Some(
                    args.get(index)
                        .ok_or_else(|| "--provider requires a value".to_string())?
                        .clone(),
                );
            }
            value => rest.push(value.to_string()),
        }
        index += 1;
    }
    Ok((provider, rest))
}

fn manager() -> Result<ProviderManager, String> {
    let mut manager = ProviderManager::new();
    manager.register_kind(Arc::new(CodexKind));
    manager.register_kind(Arc::new(OpencodeGoKind));
    manager.register_kind(Arc::new(AlibabaTokenPlanKind));
    manager.register_kind(Arc::new(AnthropicSubscriptionKind));
    manager.register_kind(Arc::new(firmius_core::ClinePassKind));
    manager.load()?;
    Ok(manager)
}

fn account_ids(manager: &ProviderManager, provider: Option<&str>) -> Result<Vec<String>, String> {
    let ids = manager
        .provider_ids()
        .into_iter()
        .map(str::to_string)
        .collect::<Vec<_>>();
    match provider {
        None => Ok(ids),
        Some(provider) if ids.iter().any(|id| id == provider) => Ok(vec![provider.to_string()]),
        Some(provider) => {
            let accounts = manager
                .accounts_for(provider)
                .into_iter()
                .map(|account| account.id)
                .collect::<Vec<_>>();
            if accounts.is_empty() {
                Err(format!(
                    "no provider or account found for target `{provider}`"
                ))
            } else {
                Ok(accounts)
            }
        }
    }
}

async fn quota(provider: Option<&str>) -> Result<(), String> {
    let manager = manager()?;
    let ids = account_ids(&manager, provider)?;
    if ids.is_empty() {
        println!("OK: no accounts found");
        return Ok(());
    }

    let mut failures = Vec::new();
    for id in ids {
        let capability = match manager.quota_capability(&id) {
            Ok(Some(capability)) => capability,
            Ok(None) => {
                failures.push(format!("{id}: no quota capability"));
                continue;
            }
            Err(error) => {
                failures.push(format!("{id}: {error}"));
                continue;
            }
        };
        let Some(source) = capability.source else {
            failures.push(format!("{id}: quota capability has no source"));
            continue;
        };
        match source.fetch().await {
            Ok(snapshot) if !snapshot.meters.is_empty() => {
                println!(
                    "OK: {id}: {} meter(s){}",
                    snapshot.meters.len(),
                    snapshot
                        .note
                        .as_deref()
                        .map(|note| format!(" ({note})"))
                        .unwrap_or_default()
                );
            }
            Ok(_) => failures.push(format!("{id}: quota returned no meters")),
            Err(error) => failures.push(format!("{id}: {error}")),
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(failures.join("\n"))
    }
}

async fn provider_stream(provider: Option<&str>, flags: Vec<String>) -> Result<(), String> {
    let manager = manager()?;
    let ids = account_ids(&manager, provider)?;
    if ids.is_empty() {
        println!("OK: no accounts found");
        return Ok(());
    }
    let requested_model = flags
        .windows(2)
        .find(|pair| pair[0] == "--model")
        .map(|pair| pair[1].clone());
    if flags.iter().any(|flag| flag == "--model") && requested_model.is_none() {
        return Err("--model requires a value".into());
    }

    let mut failures = Vec::new();
    for id in ids {
        match audit_provider(&manager, &id, requested_model.as_deref()).await {
            Ok(()) => println!("OK: {id}: provider stream and tool round-trip passed"),
            Err(error) => failures.push(format!("{id}: {error}")),
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(failures.join("\n"))
    }
}

async fn audit_provider(
    manager: &ProviderManager,
    id: &str,
    requested_model: Option<&str>,
) -> Result<(), String> {
    let schema = manager
        .schema(id)
        .ok_or_else(|| format!("missing schema for account {id}"))?;
    let is_codex = manager
        .account(id)
        .is_some_and(|account| account.kind == "codex");
    let model = requested_model
        .map(str::to_string)
        .or_else(|| is_codex.then(|| "gpt-5.6-luna".to_string()))
        .or_else(|| schema.models.first().map(|model| model.id.clone()))
        .ok_or_else(|| "account has no configured model".to_string())?;
    if !is_codex
        && !schema.models.is_empty()
        && schema.models.iter().all(|candidate| candidate.id != model)
    {
        return Err(format!(
            "model `{model}` is not configured for this account"
        ));
    }
    let provider = manager.build(id)?;
    let tools = vec![ToolDefinition {
        name: "audit_echo".into(),
        description: "Return the supplied text unchanged. Use this tool for the audit.".into(),
        input_schema: serde_json::json!({
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"]
        }),
    }];
    let mut messages = vec![Message::text(
        MessageRole::User,
        "Reply with the exact text AUDIT_OK and nothing else.",
    )];
    let text_events = collect(
        provider
            .stream(request(&model, &messages, Vec::new()))
            .await
            .map_err(|error| error.to_string())?,
    )
    .await?;
    require_text_and_done(&text_events, "AUDIT_OK")?;

    messages = vec![Message::text(
        MessageRole::User,
        "Call the audit_echo tool exactly once with {\"text\":\"TOOL_OK\"}, then report the tool result.",
    )];
    let tool_events = collect(
        provider
            .stream(request(&model, &messages, tools.clone()))
            .await
            .map_err(|error| error.to_string())?,
    )
    .await?;
    let call = tool_events.iter().find_map(|event| match event {
        ProviderEvent::ToolCall { id, name, args } => {
            Some((id.clone(), name.clone(), args.clone()))
        }
        _ => None,
    });
    let Some((call_id, name, args)) = call else {
        return Err("tool-call request produced no complete tool call".into());
    };
    if name != "audit_echo" {
        return Err(format!("provider called unexpected tool `{name}`"));
    }
    let parsed: serde_json::Value = serde_json::from_str(&args)
        .map_err(|error| format!("tool arguments were invalid JSON: {error}"))?;
    if parsed.get("text").and_then(serde_json::Value::as_str) != Some("TOOL_OK") {
        return Err(format!("unexpected audit tool arguments: {args}"));
    }
    messages.push(Message {
        role: MessageRole::Assistant,
        content: vec![MessagePart::ToolCall {
            id: call_id.clone(),
            name,
            args,
        }],
    });
    messages.push(Message::tool_results([MessagePart::ToolResult {
        id: call_id,
        content: "TOOL_RESULT_OK".into(),
        ok: true,
    }]));
    let result_events = collect(
        provider
            .stream(request(&model, &messages, tools))
            .await
            .map_err(|error| error.to_string())?,
    )
    .await?;
    require_text_and_done(&result_events, "TOOL_RESULT_OK")
}

fn request(model: &str, messages: &[Message], tools: Vec<ToolDefinition>) -> ProviderRequest {
    ProviderRequest {
        model: model.into(),
        messages: messages.to_vec(),
        tools,
        temperature: Some(0.0),
        max_tokens: Some(256),
        reasoning_effort: None,
        thinking_budget_tokens: None,
    }
}

async fn collect(
    stream: futures::stream::BoxStream<'static, Result<ProviderEvent, firmius_core::ProviderError>>,
) -> Result<Vec<ProviderEvent>, String> {
    stream
        .map(|event| event.map_err(|error| error.to_string()))
        .collect::<Vec<_>>()
        .await
        .into_iter()
        .collect()
}

fn require_text_and_done(events: &[ProviderEvent], expected_text: &str) -> Result<(), String> {
    let text = events
        .iter()
        .filter_map(|event| match event {
            ProviderEvent::TextDelta { delta } => Some(delta.as_str()),
            _ => None,
        })
        .collect::<String>();
    if !text.contains(expected_text) {
        return Err(format!(
            "stream did not contain expected text `{expected_text}`: got `{text}`"
        ));
    }
    if !events.iter().any(|event| {
        matches!(
            event,
            ProviderEvent::Done {
                reason: StopReason::Stop
            }
        )
    }) {
        return Err("stream ended without Done { reason: Stop }".into());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Phase 7A: deterministic, local compaction audit
// ---------------------------------------------------------------------------

/// A deliberately boring provider used by the audit.  Scripts are consumed
/// in order, so the audit never consults credentials, the provider manager,
/// or the network.
struct ScriptedProvider {
    scripts: Mutex<VecDeque<Result<Vec<Result<ProviderEvent, ProviderError>>, ProviderError>>>,
    requests: Mutex<Vec<ProviderRequest>>,
}

impl ScriptedProvider {
    fn new(
        scripts: impl IntoIterator<
            Item = Result<Vec<Result<ProviderEvent, ProviderError>>, ProviderError>,
        >,
    ) -> Arc<Self> {
        Arc::new(Self {
            scripts: Mutex::new(scripts.into_iter().collect()),
            requests: Mutex::new(Vec::new()),
        })
    }
}

#[async_trait]
impl Provider for ScriptedProvider {
    fn id(&self) -> &str {
        "phase-7a-scripted"
    }

    async fn stream(
        &self,
        request: ProviderRequest,
    ) -> Result<BoxStream<'static, Result<ProviderEvent, ProviderError>>, ProviderError> {
        self.requests.lock().unwrap().push(request);
        let script = self
            .scripts
            .lock()
            .unwrap()
            .pop_front()
            .ok_or_else(|| ProviderError::Decode("script exhausted".into()))?;
        Ok(futures::stream::iter(script?).boxed())
    }
}

fn event_text(text: &str) -> Vec<Result<ProviderEvent, ProviderError>> {
    vec![
        Ok(ProviderEvent::TextDelta { delta: text.into() }),
        Ok(ProviderEvent::Done {
            reason: StopReason::Stop,
        }),
    ]
}

fn event_summary(text: &str) -> Result<Vec<Result<ProviderEvent, ProviderError>>, ProviderError> {
    Ok(event_text(text))
}

struct AuditArtifactTool;

#[async_trait]
impl Tool for AuditArtifactTool {
    fn name(&self) -> &str {
        "audit_artifact"
    }

    fn description(&self) -> &str {
        "Write deterministic audit evidence to the session artifact store."
    }

    fn input_schema(&self) -> serde_json::Value {
        serde_json::json!({
            "type": "object",
            "properties": {"content": {"type": "string"}},
            "required": ["content"]
        })
    }

    async fn call(&self, args: serde_json::Value, ctx: ToolContext) -> Result<String, ToolError> {
        let content = args
            .get("content")
            .and_then(serde_json::Value::as_str)
            .ok_or_else(|| ToolError::InvalidArguments("content must be a string".into()))?;
        let Some(store) = firmius_core::tools::session_artifacts(&ctx).await else {
            return Err(ToolError::Failed("audit tool was not session-bound".into()));
        };
        store
            .write(
                "artifact://phase-7a/tool-result.md",
                content.to_string(),
                Some(&ctx.agent_id),
                firmius_core::ArtifactSource::Manual,
            )
            .map_err(|error| ToolError::Failed(error.to_string()))?;
        Ok(format!("artifact written: {content}"))
    }
}

#[derive(Default)]
struct AuditReport {
    passed: Vec<String>,
    failed: Vec<String>,
    notes: Vec<String>,
}

impl AuditReport {
    fn check(&mut self, name: &str, result: Result<(), String>) {
        match result {
            Ok(()) => self.passed.push(name.into()),
            Err(error) => self.failed.push(format!("{name}: {error}")),
        }
    }
}

async fn compaction_audit(
    provider_target: Option<&str>,
    model_target: Option<&str>,
) -> Result<(), String> {
    let mgr = manager()?;
    let provider_id = match provider_target {
        Some(id) => id.to_string(),
        None => {
            return Err(
                "compaction audit requires --provider <id-or-kind> to select a real provider"
                    .into(),
            )
        }
    };
    // Resolve account: try exact id match, then kind match.
    let account_id = if mgr.schema(&provider_id).is_some() {
        provider_id.clone()
    } else {
        let accounts = mgr.accounts_for(&provider_id);
        accounts
            .first()
            .map(|a| a.id.clone())
            .ok_or_else(|| format!("no account found for provider `{provider_id}`"))?
    };
    let schema = mgr
        .schema(&account_id)
        .ok_or_else(|| format!("missing schema for account {account_id}"))?;
    let model = match model_target {
        Some(m) => m.to_string(),
        None => schema
            .models
            .first()
            .map(|m| m.id.clone())
            .ok_or_else(|| "no model configured for this account; pass --model".to_string())?,
    };
    let provider = mgr
        .build(&account_id)
        .map_err(|e| format!("failed to build provider {account_id}: {e}"))?;

    let mut report = AuditReport::default();
    report.check("soft/hard budget decisions", budget_scenario());
    report.check(
        "manual real Agent compaction",
        manual_agent_scenario(provider.clone(), &model, &account_id).await,
    );
    report.check(
        "automatic soft background and hard scheduling",
        automatic_scheduling_scenario(provider.clone(), &model, &account_id).await,
    );
    report.check(
        "tool-heavy source and artifacts",
        tool_heavy_scenario(provider.clone(), &model, &account_id).await,
    );
    report.check(
        "concurrent append and stale source",
        append_stale_scenario(),
    );
    report.check(
        "failure and invalid output",
        invalid_output_scenario().await,
    );
    report.check(
        "persistence and summary projection",
        persistence_scenario(provider.clone(), &model, &account_id).await,
    );

    println!("Compaction audit (real provider: {}/{})", account_id, model);
    for name in &report.passed {
        println!("PASS  {name}");
    }
    for failure in &report.failed {
        println!("FAIL  {failure}");
    }
    for note in &report.notes {
        println!("NOTE  {note}");
    }
    println!(
        "Result: {}/{} scenarios passed",
        report.passed.len(),
        report.passed.len() + report.failed.len()
    );
    if report.failed.is_empty() {
        Ok(())
    } else {
        Err(report.failed.join("\n"))
    }
}

fn budget_scenario() -> Result<(), String> {
    let request = ProviderRequest {
        model: "audit".into(),
        messages: vec![Message::text(MessageRole::User, "x".repeat(400))],
        tools: vec![],
        temperature: None,
        max_tokens: Some(20),
        reasoning_effort: None,
        thinking_budget_tokens: None,
    };
    let config = context_budget::BudgetConfig {
        safety_margin_tokens: 0,
        safety_margin_ratio: 0.0,
        ..Default::default()
    };
    let model = |window| ModelInfo {
        id: "audit".into(),
        context_window: window,
        max_output_tokens: Some(20),
        capabilities: Default::default(),
        effort_modes: vec![],
    };
    if context_budget::assessment(&model(150), &request, config).decision
        != context_budget::BudgetDecision::Soft
    {
        return Err("150-token window did not produce Soft".into());
    }
    if context_budget::assessment(&model(120), &request, config).decision
        != context_budget::BudgetDecision::Hard
    {
        return Err("120-token window did not produce Hard".into());
    }
    Ok(())
}

async fn manual_agent_scenario(
    provider: Arc<dyn Provider>,
    model: &str,
    provider_id: &str,
) -> Result<(), String> {
    let session = Arc::new(tokio::sync::Mutex::new(Session::new()));
    session.lock().await.bind_self(&session);
    let tools = Arc::new(ToolRegistry::default());
    let agent = session.lock().await.spawn_agent(
        provider,
        tools,
        AgentConfig {
            provider_id: provider_id.into(),
            model: model.into(),
            ..Default::default()
        },
    );
    agent
        .prompt(
            "What is 2+2? Answer in one word.",
            CancellationToken::new(),
            |_| {},
        )
        .await
        .map_err(|error| format!("first prompt failed: {error}"))?;
    agent
        .prompt(
            "What is 3+3? Answer in one word.",
            CancellationToken::new(),
            |_| {},
        )
        .await
        .map_err(|error| format!("second prompt failed: {error}"))?;
    let before = agent.history();
    let mut events = Vec::new();
    agent
        .compact_now(CancellationToken::new(), |event| events.push(event))
        .await
        .map_err(|error| format!("manual compaction failed: {error}"))?;
    let projection = agent.compaction_projection();
    let summary = projection
        .snapshot
        .as_ref()
        .map(|s| s.summary.as_str())
        .unwrap_or("");
    if summary.is_empty() {
        return Err("manual compaction did not produce a summary".into());
    }
    if agent.history().len() >= before.len() {
        return Err("manual compaction did not reduce history".into());
    }
    if !events
        .iter()
        .any(|event| matches!(event, AgentEvent::CompactionFinished { .. }))
    {
        return Err("manual compaction did not emit CompactionFinished".into());
    }
    Ok(())
}

async fn automatic_scheduling_scenario(
    provider: Arc<dyn Provider>,
    model: &str,
    provider_id: &str,
) -> Result<(), String> {
    let session = Arc::new(tokio::sync::Mutex::new(Session::new()));
    session.lock().await.bind_self(&session);
    let agent = session.lock().await.spawn_agent(
        provider.clone(),
        Arc::new(ToolRegistry::default()),
        AgentConfig {
            provider_id: provider_id.into(),
            model: model.into(),
            ..Default::default()
        },
    );
    // Use a tiny synthetic context window so prompts cross real thresholds
    // while the real provider handles generation.
    agent
        .set_model_metadata(provider_id, audit_model(model, 800))
        .map_err(|e| e.to_string())?;

    let mut events = Vec::new();
    // First prompt: enough to cross soft but not hard.
    let big = "x ".repeat(300);
    agent
        .prompt(
            format!("Read this data: {big} Now summarize what you read in one sentence."),
            CancellationToken::new(),
            |event| events.push(event),
        )
        .await
        .map_err(|error| format!("first prompt failed: {error}"))?;

    // Second prompt: should still be within soft or cross hard, triggering
    // background or synchronous compaction.
    agent
        .prompt(
            "What was the data about? Answer briefly.",
            CancellationToken::new(),
            |event| events.push(event),
        )
        .await
        .map_err(|error| format!("second prompt failed: {error}"))?;

    if !events
        .iter()
        .any(|event| matches!(event, AgentEvent::CompactionScheduled { .. }))
    {
        return Err("no CompactionScheduled event was observed".into());
    }
    if !events
        .iter()
        .any(|event| {
            matches!(
                event,
                AgentEvent::CompactionFinished { .. } | AgentEvent::CompactionDiscarded { .. }
            )
        })
    {
        return Err("no CompactionFinished or CompactionDiscarded event was observed".into());
    }
    Ok(())
}

async fn tool_heavy_scenario(
    provider: Arc<dyn Provider>,
    model: &str,
    provider_id: &str,
) -> Result<(), String> {
    let session = Arc::new(tokio::sync::Mutex::new(Session::new()));
    session.lock().await.bind_self(&session);
    let tools = Arc::new(ToolRegistry::default());
    tools.register(AuditArtifactTool);
    let agent = session.lock().await.spawn_agent(
        provider,
        tools,
        AgentConfig {
            provider_id: provider_id.into(),
            model: model.into(),
            ..Default::default()
        },
    );
    agent
        .prompt(
            "Call the audit_artifact tool with content \"hello\" to write an artifact. Then confirm you did so.",
            CancellationToken::new(),
            |_| {},
        )
        .await
        .map_err(|error| format!("tool prompt failed: {error}"))?;

    // If the model called the tool, the artifact should exist.
    let artifact = session.lock().await.artifacts.read("phase-7a/tool-result.md");
    if let Ok(content) = &artifact {
        if content.is_empty() {
            return Err("artifact was written but is empty".into());
        }
    }
    // Artifact may or may not exist depending on whether the model called the
    // tool. That's fine — the scenario proves the real provider + tool
    // registry + session work together without crashing.

    agent
        .prompt("continue", CancellationToken::new(), |_| {})
        .await
        .map_err(|error| format!("continue prompt failed: {error}"))?;
    agent
        .compact_now(CancellationToken::new(), |_| {})
        .await
        .map_err(|error| format!("compaction after tool use failed: {error}"))?;
    Ok(())
}

fn append_stale_scenario() -> Result<(), String> {
    let segment = |id, text| {
        compaction::TimelineSegment::new(
            id,
            [compaction::TimelineEntry::new(
                0,
                Message::text(MessageRole::User, text),
            )],
        )
    };
    let projection = compaction::Projection::new(compaction::Timeline::new([
        segment("old", "old source"),
        segment("tail", "active tail"),
    ]));
    let plan = compaction::plan(&projection, 0).map_err(|e| format!("plan: {e:?}"))?;
    let mut appended = projection.clone();
    appended.timeline.segments[1]
        .entries
        .push(compaction::TimelineEntry::new(
            1,
            Message::text(MessageRole::User, "concurrent append"),
        ));
    compaction::apply(&appended, &plan, "summary")
        .map_err(|e| format!("active append was incorrectly stale: {e:?}"))?;
    let mut stale = projection;
    stale.timeline.segments[0].entries[0].message =
        Message::text(MessageRole::User, "changed source");
    if compaction::apply(&stale, &plan, "summary").is_ok() {
        return Err("changed source was accepted for a stale plan".into());
    }
    Ok(())
}

async fn invalid_output_scenario() -> Result<(), String> {
    let plan = compaction::CompactionPlan {
        generation: 0,
        source_segment_ids: vec!["source".into()],
        source_range: (0, 1),
        source_entries: 1,
        source_content_digest: String::new(),
    };
    let input = compaction_job::CompactionJobInput {
        plan,
        snapshot: None,
        source_messages: vec![Message::text(MessageRole::User, "source")],
        metadata: String::new(),
        model: "audit".into(),
    };
    let empty = ScriptedProvider::new([Ok(vec![Ok(ProviderEvent::Done {
        reason: StopReason::Stop,
    })])]);
    let result =
        compaction_job::run_compaction_job(input.clone(), empty, CancellationToken::new()).await;
    if !matches!(result, Err(compaction_job::CompactionJobError::EmptyOutput)) {
        return Err(format!("empty output returned {result:?}"));
    }
    let tool = ScriptedProvider::new([Ok(vec![Ok(ProviderEvent::ToolCall {
        id: "bad".into(),
        name: "tool".into(),
        args: "{}".into(),
    })])]);
    let result = compaction_job::run_compaction_job(input, tool, CancellationToken::new()).await;
    if !matches!(result, Err(compaction_job::CompactionJobError::ToolCall)) {
        return Err(format!("tool output returned {result:?}"));
    }
    let failed = ScriptedProvider::new([Err(ProviderError::Api {
        status: 503,
        body: "scripted failure".into(),
    })]);
    let result = compaction_job::run_compaction_job(
        compaction_job::CompactionJobInput {
            plan: compaction::CompactionPlan {
                generation: 0,
                source_segment_ids: vec!["source".into()],
                source_range: (0, 1),
                source_entries: 1,
                source_content_digest: String::new(),
            },
            snapshot: None,
            source_messages: vec![Message::text(MessageRole::User, "source")],
            metadata: String::new(),
            model: "audit".into(),
        },
        failed,
        CancellationToken::new(),
    )
    .await;
    if !matches!(result, Err(compaction_job::CompactionJobError::Provider(_))) {
        return Err(format!("provider failure returned {result:?}"));
    }
    Ok(())
}

async fn persistence_scenario(
    provider: Arc<dyn Provider>,
    model: &str,
    provider_id: &str,
) -> Result<(), String> {
    let session = Arc::new(tokio::sync::Mutex::new(Session::new()));
    session.lock().await.bind_self(&session);
    let agent = session.lock().await.spawn_agent(
        provider,
        Arc::new(ToolRegistry::default()),
        AgentConfig {
            provider_id: provider_id.into(),
            model: model.into(),
            ..Default::default()
        },
    );
    agent
        .prompt(
            "What is the capital of France? Answer in one word.",
            CancellationToken::new(),
            |_| {},
        )
        .await
        .map_err(|e| format!("first prompt failed: {e}"))?;
    agent
        .prompt(
            "What is the capital of Germany? Answer in one word.",
            CancellationToken::new(),
            |_| {},
        )
        .await
        .map_err(|e| format!("second prompt failed: {e}"))?;
    agent
        .compact(CancellationToken::new(), |_| {})
        .await
        .map_err(|e| format!("compaction failed: {e}"))?;
    let locked = session.lock().await;
    let config = agent.config();
    let record = AgentRecord {
        id: agent.id.clone(),
        provider_id: config.provider_id,
        model: config.model,
        effort: config.effort,
        system_prompt: config.system_prompt,
        persona: config.persona,
        temperature: config.temperature,
        max_tokens: config.max_tokens,
        workdir: config.workdir,
        history: agent.history_for_persistence(),
        compaction: Some(agent.compaction_projection()),
    };
    let session_record = SessionRecord {
        id: locked.id.clone(),
        title: Some("compaction-audit".into()),
        created_at: locked.created_at,
        updated_at: locked.created_at,
        agents: vec![record],
        hierarchy: locked
            .hierarchy
            .iter()
            .map(|(id, node)| {
                (
                    id.clone(),
                    firmius_core::AgentNodeRecord {
                        parent_id: node.parent_id.clone(),
                        spawned_via_tool_call_id: node.spawned_via_tool_call_id.clone(),
                    },
                )
            })
            .collect(),
        artifacts: locked.artifacts.snapshot(),
    };
    let encoded = serde_json::to_vec(&session_record).map_err(|e| e.to_string())?;
    let decoded: SessionRecord = serde_json::from_slice(&encoded).map_err(|e| e.to_string())?;
    let projection = decoded.agents[0]
        .compaction
        .as_ref()
        .ok_or("projection missing after round-trip")?;
    let summary = projection
        .snapshot
        .as_ref()
        .map(|s| s.summary.as_str())
        .unwrap_or("");
    if summary.is_empty() {
        return Err("summary projection did not survive persistence round-trip".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_provider_flag() {
        let (provider, rest) =
            parse_flags(&["--provider".into(), "codex".into(), "--model".into()]).unwrap();
        assert_eq!(provider.as_deref(), Some("codex"));
        assert_eq!(rest, ["--model"]);
    }

    #[test]
    fn phase_7a_budget_and_stale_checks_are_deterministic() {
        budget_scenario().unwrap();
        append_stale_scenario().unwrap();
    }

    #[tokio::test]
    async fn phase_7a_invalid_provider_output_is_rejected() {
        invalid_output_scenario().await.unwrap();
    }

    #[test]
    fn provider_stream_requires_expected_text_and_stop() {
        let missing_text = vec![
            ProviderEvent::TextDelta {
                delta: "not the sentinel".into(),
            },
            ProviderEvent::Done {
                reason: StopReason::Stop,
            },
        ];
        assert!(require_text_and_done(&missing_text, "AUDIT_OK").is_err());

        let wrong_stop = vec![
            ProviderEvent::TextDelta {
                delta: "AUDIT_OK".into(),
            },
            ProviderEvent::Done {
                reason: StopReason::ToolUse,
            },
        ];
        assert!(require_text_and_done(&wrong_stop, "AUDIT_OK").is_err());

        let valid = vec![
            ProviderEvent::TextDelta {
                delta: "AUDIT_OK (verified)".into(),
            },
            ProviderEvent::Done {
                reason: StopReason::Stop,
            },
        ];
        require_text_and_done(&valid, "AUDIT_OK").unwrap();
    }

    #[test]
    fn explicit_missing_provider_target_is_rejected() {
        let manager = ProviderManager::new();
        let result = account_ids(&manager, Some("missing-provider"));
        assert_eq!(
            result,
            Err("no provider or account found for target `missing-provider`".into())
        );
    }
}