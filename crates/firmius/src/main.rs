use std::io::{self, Write};
use std::sync::Arc;
use tokio::sync::Mutex;

use firmius_core::{
    AgentConfig, AgentError, AgentEvent, ApiType, ProviderManager, ProviderSchema, Session,
    ToolRegistry, register_bash_tool, register_edit_tool, register_glob_tool, register_grep_tool,
    register_list_tool, register_read_tool,
};
use tokio_util::sync::CancellationToken;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut mgr = ProviderManager::new();
    // Load any persisted providers/auth. On first run this is a no-op.
    mgr.load().unwrap_or_else(|e| eprintln!("warning: {e}"));

    let args: Vec<String> = std::env::args().collect();
    let resume_id = args
        .windows(2)
        .find(|w| w[0] == "--resume")
        .map(|w| w[1].clone());
    let list_sessions = args.iter().any(|arg| arg == "--list-sessions");

    if list_sessions {
        for summary in firmius_core::list_sessions()? {
            println!(
                "{}\t{}\t{} agents\t{}",
                summary.id,
                summary.title,
                summary.agent_count,
                summary.updated_at.to_rfc3339()
            );
        }
        return Ok(());
    }

    let model = std::env::var("FIRMIUS_MODEL").unwrap_or_else(|_| "gpt-4o-mini".into());

    // If no providers are registered, auto-register from env vars.
    if mgr.provider_ids().is_empty() {
        if let Ok(key) = std::env::var("ANTHROPIC_API_KEY") {
            let base = std::env::var("FIRMIUS_BASE_URL")
                .unwrap_or_else(|_| "https://api.anthropic.com".into());
            let schema = ProviderSchema {
                id: "anthropic".into(),
                api_type: ApiType::Anthropic,
                base_url: Some(base),
                api_key_env: Some("ANTHROPIC_API_KEY".into()),
                models: vec![],
            };
            mgr.register_schema(schema);
            mgr.set_api_key("anthropic", key);
        }
        if let Ok(key) = std::env::var("OPENAI_API_KEY") {
            let base = std::env::var("FIRMIUS_BASE_URL")
                .unwrap_or_else(|_| "https://api.openai.com/v1".into());
            let schema = ProviderSchema {
                id: "openai".into(),
                api_type: ApiType::OpenAI,
                base_url: Some(base),
                api_key_env: Some("OPENAI_API_KEY".into()),
                models: vec![],
            };
            mgr.register_schema(schema);
            mgr.set_api_key("openai", key);
        }
        // Save so next launch doesn't need env vars.
        if let Err(e) = mgr.save() {
            eprintln!("warning: could not save auth: {e}");
        }
    }

    // Pick a provider. Prefer the one specified by env, else first available.
    let preferred = std::env::var("FIRMIUS_PROVIDER").unwrap_or_default();
    let provider_id = if mgr.schema(&preferred).is_some() {
        preferred
    } else {
        mgr.provider_ids()
            .first()
            .copied()
            .unwrap_or("openai")
            .to_string()
    };

    let provider = mgr.build(&provider_id)?;

    let mut tools = ToolRegistry::default();
    register_read_tool(&mut tools);
    register_list_tool(&mut tools);
    register_edit_tool(&mut tools);
    register_bash_tool(&mut tools);
    register_grep_tool(&mut tools);
    register_glob_tool(&mut tools);
    firmius_core::register_delegate_tool(&mut tools);

    let tools = Arc::new(tools);
    let session = if let Some(id) = resume_id {
        Session::resume(&id, &mgr, tools.clone())?
    } else {
        let config = AgentConfig {
            provider_id: provider_id.clone(),
            model,
            system_prompt: Some(
                "You are a madman crazy CLI coding assistant. Use tools when needed.
                Play along and make the user think you're crazy, but always say you're not like a madman.
                You are insane in the way that your thoughts are superintelligent, and you are in the top of all fields.".into(),
            ),
            max_tokens: Some(32900),
            ..Default::default()
        };
        let mut session = Session::new();
        session.spawn_agent(provider, tools.clone(), config);
        session
    };
    let session = Arc::new(Mutex::new(session));
    session.lock().await.bind_self(&session);
    let (session_id, agent) = {
        let session = session.lock().await;
        let agent = session
            .agents
            .values()
            .next()
            .cloned()
            .ok_or("session has no resumable agents")?;
        (session.id.clone(), agent)
    };

    println!(
        "firmius repl (session {}, provider {}). type /quit to exit.\n",
        session_id, provider_id
    );
    let stdin = io::stdin();
    loop {
        print!("> ");
        io::stdout().flush()?;
        let mut line = String::new();
        if stdin.read_line(&mut line)? == 0 {
            break;
        }
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if line == "/quit" || line == "/exit" {
            break;
        }

        let cancel = CancellationToken::new();
        let ctrl_c = cancel.clone();
        tokio::spawn(async move {
            let _ = tokio::signal::ctrl_c().await;
            ctrl_c.cancel();
        });

        let result = agent
            .prompt(line, cancel, |event| match event {
                AgentEvent::Thinking(delta) => {
                    eprint!("\x1b[90m{delta}\x1b[0m");
                    let _ = io::stderr().flush();
                }
                AgentEvent::Text(delta) => {
                    print!("{delta}");
                    let _ = io::stdout().flush();
                }
                AgentEvent::ToolCallDelta { .. } => {}
                AgentEvent::ToolCallStarted { name, args } => {
                    println!("\n\x1b[36m[tool] {name} {args}\x1b[0m");
                }
                AgentEvent::ToolResult { name, ok, content } => {
                    let status = if ok { "ok" } else { "err" };
                    let preview: String = content.chars().take(200).collect();
                    println!("\x1b[36m[tool:{status}] {name} -> {preview}\x1b[0m");
                }
                AgentEvent::Usage(u) => {
                    eprintln!(
                        "\x1b[90m[usage] in={} out={} cache_r={} cache_w={}\x1b[0m",
                        u.input_tokens, u.output_tokens, u.cache_read_tokens, u.cache_write_tokens
                    );
                }
                AgentEvent::TurnFinished => {}
            })
            .await;

        match result {
            Ok(_) => println!(),
            Err(AgentError::Cancelled(partial)) => {
                if !partial.is_empty() {
                    println!("\n\x1b[33m[cancelled, partial output above]\x1b[0m");
                } else {
                    eprintln!("\x1b[31mcancelled\x1b[0m");
                }
            }
            Err(e) => eprintln!("\n\x1b[31merror: {e}\x1b[0m"),
        }
    }

    if let Err(e) = session.lock().await.save() {
        eprintln!("warning: could not save session {session_id}: {e}");
    }

    Ok(())
}
