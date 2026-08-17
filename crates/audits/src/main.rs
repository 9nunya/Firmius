use std::process::ExitCode;
use std::sync::Arc;

use firmius_core::{
    AlibabaTokenPlanKind, CodexKind, Message, MessagePart, MessageRole, OpencodeGoKind,
    ProviderEvent, ProviderManager, ProviderRequest, ToolDefinition,
};
use futures::StreamExt;

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

async fn run(args: Vec<String>) -> Result<(), String> {
    let command = args.first().map(String::as_str).unwrap_or("help");
    let (provider, flags) = parse_flags(&args[1..])?;
    match command {
        "provider_stream" => provider_stream(provider.as_deref(), flags).await,
        "quota" => quota(provider.as_deref()).await,
        "help" | "--help" | "-h" => {
            print_help();
            Ok(())
        }
        other => Err(format!("unknown audit `{other}`; use `help`")),
    }
}

fn print_help() {
    println!(
        "Usage:\n  firmius-audits provider_stream [--provider <id-or-kind>] [--model <id>]\n  firmius-audits quota [--provider <id-or-kind>]\n\nAudits read accounts from ~/.firmius."
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
    manager.register_kind(Arc::new(firmius_core::ClinePassKind));
    manager.load()?;
    Ok(manager)
}

fn account_ids(manager: &ProviderManager, provider: Option<&str>) -> Vec<String> {
    let ids = manager
        .provider_ids()
        .into_iter()
        .map(str::to_string)
        .collect::<Vec<_>>();
    match provider {
        None => ids,
        Some(provider) if ids.iter().any(|id| id == provider) => vec![provider.to_string()],
        Some(provider) => manager
            .accounts_for(provider)
            .into_iter()
            .map(|account| account.id)
            .collect(),
    }
}

async fn quota(provider: Option<&str>) -> Result<(), String> {
    let manager = manager()?;
    let ids = account_ids(&manager, provider);
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
    let ids = account_ids(&manager, provider);
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
    require_text_and_done(&text_events)?;

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
    require_text_and_done(&result_events)
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

fn require_text_and_done(events: &[ProviderEvent]) -> Result<(), String> {
    let text = events
        .iter()
        .filter_map(|event| match event {
            ProviderEvent::TextDelta { delta } => Some(delta.as_str()),
            _ => None,
        })
        .collect::<String>();
    if text.is_empty() {
        return Err("stream produced no text".into());
    }
    if !events
        .iter()
        .any(|event| matches!(event, ProviderEvent::Done { .. }))
    {
        return Err("stream ended without a Done event".into());
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
}
