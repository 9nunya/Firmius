mod repl;
mod tui;

use std::io::IsTerminal;
use std::sync::Arc;

use firmius_core::{
    AccountRecord, AlibabaTokenPlanKind, AnthropicSubscriptionKind, ApiType, ClinePassKind,
    CodexKind, McpManager, McpSettings, OpencodeGoKind, PersonaManager, ProviderManager,
    ProviderSchema, Session, ToolRegistry, UserSettings, register_bash_tool, register_edit_tool,
    register_glob_tool, register_grep_tool, register_list_tool, register_message_tool,
    register_read_tool, register_task_tool, register_tool_specs, register_yield_tool,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut mgr = ProviderManager::new();
    let personas = Arc::new(PersonaManager::load_default().unwrap_or_else(|e| {
        eprintln!("warning: could not load personas: {e}");
        PersonaManager::default()
    }));
    let settings = Arc::new(std::sync::Mutex::new(UserSettings::load().unwrap_or_else(
        |e| {
            eprintln!("warning: could not load settings: {e}");
            UserSettings::default()
        },
    )));
    // Umbrella config (retry policy + general options). Shared with the TUI so
    // the settings modal can edit and persist it live.
    let config = Arc::new(std::sync::Mutex::new(
        firmius_core::FirmiusConfig::load().unwrap_or_else(|e| {
            eprintln!("warning: could not load config: {e}");
            firmius_core::FirmiusConfig::default()
        }),
    ));
    // Credential families beyond plain api-key: subscription products.
    mgr.register_kind(Arc::new(OpencodeGoKind));
    mgr.register_kind(Arc::new(AlibabaTokenPlanKind));
    mgr.register_kind(Arc::new(AnthropicSubscriptionKind));
    mgr.register_kind(Arc::new(CodexKind));
    mgr.register_kind(Arc::new(ClinePassKind));
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

    let mut changed = false;

    if let Ok(key) = std::env::var("CLINE_API_KEY")
        && !key.is_empty()
        && mgr.account("cline-pass").is_none()
    {
        let schema = firmius_core::kinds::cline_pass::schema_template();
        mgr.register_account(AccountRecord {
            id: schema.id.clone(),
            kind: "cline-pass".into(),
            schema,
            credentials: serde_json::json!({ "api_key": key }),
        });
        changed = true;
    }

    // Subscription product accounts: bootstrap from env keys when present.
    if let Ok(key) = std::env::var("OPENCODE_API_KEY")
        && !key.is_empty()
        && mgr.account("opencode-go").is_none()
    {
        let schema = firmius_core::kinds::opencode_go::schema_template();
        let id = schema.id.clone();
        mgr.register_account(AccountRecord {
            id,
            kind: "opencode-go".to_string(),
            schema,
            credentials: serde_json::json!({ "api_key": key }),
        });
        changed = true;
    }
    if let Ok(key) = std::env::var("ALIBABA_TOKEN_PLAN_API_KEY")
        && !key.is_empty()
        && mgr.account("alibaba-token-plan").is_none()
    {
        let region = std::env::var("ALIBABA_REGION").unwrap_or_else(|_| "international".into());
        let schema = firmius_core::kinds::alibaba::schema_template(&region);
        let id = schema.id.clone();
        mgr.register_account(AccountRecord {
            id,
            kind: "alibaba-token-plan".to_string(),
            schema,
            credentials: serde_json::json!({ "api_key": key, "region": region }),
        });
        changed = true;
    }

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
            changed = true;
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
            changed = true;
        }
    }
    // Save so the next launch doesn't need env vars.
    if changed && let Err(e) = mgr.save() {
        eprintln!("warning: could not save accounts: {e}");
    }

    // Pick a provider. Prefer the one specified by env, else first available.
    let preferred = std::env::var("FIRMIUS_PROVIDER").unwrap_or_default();
    let provider_id = if mgr.schema(&preferred).is_some() {
        preferred
    } else {
        mgr.provider_ids()
            .first()
            .copied()
            .unwrap_or("")
            .to_string()
    };

    let tools = ToolRegistry::default();
    register_read_tool(&tools);
    register_list_tool(&tools);
    register_edit_tool(&tools);
    register_bash_tool(&tools);
    register_grep_tool(&tools);
    register_glob_tool(&tools);
    firmius_core::register_delegate_tool(&tools);
    register_task_tool(&tools);
    register_yield_tool(&tools);
    register_message_tool(&tools);

    let tools = Arc::new(tools);

    // MCP: load persisted servers, start the enabled ones, and register their
    // tools so agents can call them through the shared registry.
    let mcp = Arc::new(McpManager::from_settings(
        McpSettings::load().unwrap_or_else(|e| {
            eprintln!("warning: could not load MCP settings: {e}");
            McpSettings::default()
        }),
    ));
    for result in mcp.start_all().await {
        match result {
            Ok(specs) => register_tool_specs(tools.as_ref(), mcp.clone(), specs),
            Err(error) => eprintln!("warning: could not start an MCP server: {error}"),
        }
    }

    let manager = Arc::new(std::sync::Mutex::new(mgr.clone()));
    let (session, agent, active_provider_id) = if let Some(id) = resume_id {
        let session = Session::resume_with_personas(&id, &mgr, tools.clone(), personas.clone())?
            .into_handle();
        for agent in session.agents.read().unwrap().values() {
            agent.attach_runtime(manager.clone(), settings.clone());
        }
        let agent = session
            .agents
            .read()
            .unwrap()
            .values()
            .next()
            .cloned()
            .ok_or("session has no available agents")?;
        let provider_id = agent.config().provider_id.clone();
        (Some(session), Some(agent), provider_id)
    } else {
        (None, None, provider_id)
    };

    if std::io::stdout().is_terminal() && std::io::stdin().is_terminal() {
        tui::run(
            session,
            agent,
            active_provider_id,
            model,
            tools,
            manager,
            personas,
            settings,
            config,
            mcp,
        )
        .await?;
    } else {
        let (Some(session), Some(agent)) = (session, agent) else {
            return Err(
                "interactive terminal required to configure Firmius without a provider".into(),
            );
        };
        repl::run(session, agent).await?;
    }
    Ok(())
}
