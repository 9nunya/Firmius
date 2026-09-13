pub(crate) mod install;
mod repl;
mod tui;

use std::io::IsTerminal;
use std::sync::Arc;

use chrono::Utc;
use firmius_client::DaemonClient;
use firmius_core::{
    AccountRecord, AlibabaTokenPlanKind, AnthropicSubscriptionKind, ApiType, ClinePassKind,
    CodexKind, FreebuffKind, GrokBuildKind, McpManager, McpSettings, OpencodeGoKind,
    PersonaManager, ProviderManager, ProviderSchema, Session, ToolRegistry, UserSettings,
    register_bash_tool, register_edit_tool, register_glob_tool, register_grep_tool,
    register_list_tool, register_message_tool, register_read_tool, register_task_tool,
    register_todo_tool, register_tool_specs,
};
use firmius_protocol::{
    CancelGoalRequest, CheckGoalRequest, CreateGoalRequest, CreateSessionRequest, GetGoalRequest,
    GoalActor, GoalId, GoalOwner, GoalProvenance, GoalRequest, GoalResponse, GoalSource,
    ListGoalsRequest, Request as DaemonRequest, Response as DaemonResponse,
};
use tokio::sync::broadcast;

#[derive(Debug, Clone, PartialEq, Eq)]
enum CliMode {
    Start {
        resume_id: Option<String>,
        reset_onboarding: bool,
        remote_workspace: Option<String>,
    },
    ListSessions,
    SshHosts(SshHostsAction),
    Daemon,
    Goal(Vec<String>),
    InspectPrompt(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum SshHostsAction {
    List,
    Add { alias: String, directory: String },
    Remove { alias: String },
    Open { alias: String },
}

async fn goal_revision(client: &DaemonClient, goal_id: &GoalId) -> Result<u64, String> {
    match client
        .request(DaemonRequest::Goal(GoalRequest::Get(GetGoalRequest {
            goal_id: *goal_id,
        })))
        .await
        .map_err(|error| error.to_string())?
    {
        DaemonResponse::Goal(GoalResponse::Retrieved(goal)) => Ok(goal.revision),
        _ => Err("goal API returned an invalid status response".into()),
    }
}

async fn run_goal_cli(args: &[String]) -> Result<(), String> {
    // Goals are a first-class autonomous surface, so their CLI must have the
    // same startup behavior as the TUI. Requiring users to launch `daemon`
    // first made the documented first command fail on a clean install and
    // invited a second process to race the profile lease.
    let (client, _snapshot, _events) = connect_or_spawn_daemon(None).await?;
    let actor = GoalActor::User {
        user_id: "user".into(),
    };
    let is_create = matches!(args.first().map(String::as_str), Some("create") | None);
    if is_create && args.len() < 2 {
        return Err("goal create requires a description".into());
    }
    if is_create {
        if let Ok(provider_id) = std::env::var("FIRMIUS_PROVIDER")
            && !provider_id.trim().is_empty()
        {
            let model = std::env::var("FIRMIUS_MODEL").unwrap_or_else(|_| "gpt-4o-mini".into());
            let workdir = std::env::var("FIRMIUS_REMOTE_WORKSPACE").ok().or_else(|| {
                std::env::current_dir()
                    .ok()
                    .map(|path| path.to_string_lossy().into_owned())
            });
            client
                .request(DaemonRequest::CreateSession(CreateSessionRequest {
                    provider_id,
                    model,
                    effort: None,
                    persona: Some("lead".into()),
                    workdir,
                }))
                .await
                .map_err(|error| {
                    format!(
                        "could not create the autonomous goal session: {error}; check FIRMIUS_PROVIDER, FIRMIUS_MODEL, and provider credentials"
                    )
                })?;
        }
    }
    let create = |description: String| {
        GoalRequest::Create(CreateGoalRequest {
            description,
            success_conditions: vec!["goal completed".into()],
            owner: GoalOwner::User {
                user_id: "user".into(),
            },
            provenance: GoalProvenance {
                actor: actor.clone(),
                source: GoalSource::ExplicitCommand,
                created_at: Utc::now(),
            },
            checks: vec![],
            deadline: None,
            budget: None,
            approval_required: false,
            client_request_id: None,
        })
    };
    let request = match args.first().map(String::as_str) {
        Some("create") => create(args[1..].join(" ")),
        Some("list") => {
            if args.len() != 1 {
                return Err("goal list does not accept additional arguments".into());
            }
            GoalRequest::List(ListGoalsRequest::default())
        }
        Some("status") => {
            if args.len() != 2 {
                return Err("goal status requires exactly one goal id".into());
            }
            GoalRequest::Get(GetGoalRequest {
                goal_id: GoalId::parse(&args[1])
                    .map_err(|_| "status requires a valid goal id".to_string())?,
            })
        }
        Some("check") => {
            if args.len() != 3 {
                return Err("goal check requires a goal id and check id".into());
            }
            let goal_id = GoalId::parse(&args[1])
                .map_err(|_| "check requires a valid goal id".to_string())?;
            let expected_revision = goal_revision(&client, &goal_id).await?;
            GoalRequest::Check(CheckGoalRequest {
                goal_id,
                check_id: args[2].clone(),
                actor: Some(actor.clone()),
                evaluation: None,
                expected_revision,
                client_request_id: None,
            })
        }
        Some("cancel") => {
            if args.len() < 2 {
                return Err("goal cancel requires a goal id".into());
            }
            let goal_id = GoalId::parse(&args[1])
                .map_err(|_| "cancel requires a valid goal id".to_string())?;
            let expected_revision = goal_revision(&client, &goal_id).await?;
            GoalRequest::Cancel(CancelGoalRequest {
                goal_id,
                actor: Some(actor.clone()),
                reason: (args.len() > 2).then(|| args[2..].join(" ")),
                expected_revision,
                client_request_id: None,
            })
        }
        _ => create(args.join(" ")),
    };
    let response = client
        .request(DaemonRequest::Goal(request))
        .await
        .map_err(|error| error.to_string())?;
    match response {
        DaemonResponse::Goal(GoalResponse::Created(goal)) => {
            let execution = if goal.links.session_id.is_some() {
                "dispatched"
            } else {
                "proposed (set FIRMIUS_PROVIDER to dispatch from the CLI, or open Firmius)"
            };
            println!(
                "{}\t{:?}\t{}\t{}",
                goal.id, goal.status, execution, goal.description
            );
        }
        DaemonResponse::Goal(GoalResponse::Listed { goals, .. }) => {
            for goal in goals {
                println!("{}\t{:?}\t{}", goal.id, goal.status, goal.description);
            }
        }
        DaemonResponse::Goal(GoalResponse::Retrieved(goal))
        | DaemonResponse::Goal(GoalResponse::Cancelled(goal)) => {
            println!("{}\t{:?}\t{}", goal.id, goal.status, goal.description);
        }
        DaemonResponse::Goal(GoalResponse::Checked { goal, evaluation }) => {
            println!(
                "{}\t{:?}\tcheck {}",
                goal.id, goal.status, evaluation.check_id
            );
        }
        _ => return Err("goal API returned an invalid response".into()),
    }
    Ok(())
}

async fn connect_or_spawn_daemon(
    resume_id: Option<&str>,
) -> Result<
    (
        DaemonClient,
        Option<firmius_protocol::SessionSnapshot>,
        broadcast::Receiver<firmius_protocol::DaemonEvent>,
    ),
    String,
> {
    let endpoint = firmius_service::endpoint_path(&firmius_core::data_dir());
    let client = match DaemonClient::connect(&endpoint).await {
        Ok(client) => client,
        Err(first_error) => {
            // A held lease is authoritative even when endpoint publication is
            // in progress (or the daemon is briefly restarting). Do not spawn
            // a competing child and do not fall through to embedded mode.
            if firmius_service::is_locked(&firmius_core::data_dir()) {
                return Err(format!(
                    "daemon profile lease is held while endpoint is unavailable: {first_error}"
                ));
            }
            let executable = std::env::current_exe()
                .map_err(|error| format!("locate Firmius executable: {error}"))?;
            let mut command = std::process::Command::new(executable);
            command
                .arg("daemon")
                .stdin(std::process::Stdio::null())
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null());
            command
                .spawn()
                .map_err(|error| format!("start daemon after {first_error}: {error}"))?;
            let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(5);
            loop {
                match DaemonClient::connect(&endpoint).await {
                    Ok(client) => break client,
                    Err(error) if tokio::time::Instant::now() >= deadline => {
                        return Err(format!("daemon did not become ready: {error}"));
                    }
                    Err(_) => tokio::time::sleep(std::time::Duration::from_millis(50)).await,
                }
            }
        }
    };
    // Subscribe before attach so events racing the snapshot response are
    // buffered and later deduplicated by the snapshot sequence watermark.
    let events = client.subscribe();
    let snapshot = if let Some(session_id) = resume_id {
        match client
            .request(DaemonRequest::AttachSession {
                session_id: session_id.to_string(),
                workdir: std::env::var("FIRMIUS_REMOTE_WORKSPACE").ok().or_else(|| {
                    std::env::current_dir()
                        .ok()
                        .map(|path| path.to_string_lossy().into_owned())
                }),
            })
            .await
            .map_err(|error| error.to_string())?
        {
            DaemonResponse::Snapshot(snapshot) => Some(snapshot),
            _ => return Err("daemon returned an invalid attach response".into()),
        }
    } else {
        None
    };
    Ok((client, snapshot, events))
}

fn startup_session_workdir() -> std::path::PathBuf {
    startup_session_workdir_from(std::env::var("FIRMIUS_REMOTE_WORKSPACE").ok().as_deref())
}

fn startup_session_workdir_from(override_value: Option<&str>) -> std::path::PathBuf {
    if let Some(rest) = override_value.and_then(|workspace| workspace.strip_prefix("ssh://"))
        && let Some((_, remote_dir)) = rest.split_once('/')
    {
        return std::path::PathBuf::from(if remote_dir.is_empty() {
            "/".into()
        } else {
            format!("/{remote_dir}")
        });
    }
    std::env::current_dir().unwrap_or_default()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Frontend {
    Tui,
    Repl,
}

fn select_frontend(interactive: bool, has_resume: bool) -> Result<Frontend, &'static str> {
    match (interactive, has_resume) {
        (true, _) => Ok(Frontend::Tui),
        (false, true) => Ok(Frontend::Repl),
        (false, false) => {
            Err("interactive terminal required to configure Firmius without a provider")
        }
    }
}

fn validate_early_command_shape(args: &[String]) -> Result<(), String> {
    let Some(command) = args.get(1).map(String::as_str) else {
        return Ok(());
    };
    if matches!(
        command,
        "--help"
            | "-h"
            | "help"
            | "--version"
            | "-V"
            | "doctor"
            | "install-info"
            | "daemon-stop"
            | "update-check"
    ) && args.len() != 2
    {
        return Err(format!("{command} does not accept additional arguments"));
    }
    Ok(())
}

fn parse_cli_mode(args: &[String]) -> Result<CliMode, String> {
    let mut resume_id = None;
    let mut reset_onboarding = false;
    let mut remote_workspace = None;
    let mut list_sessions = false;
    let mut index = 1;

    while index < args.len() {
        match args[index].as_str() {
            "prompt" => {
                if index != 1 || args.len() > 3 {
                    return Err("usage: firmius prompt [persona]".into());
                }
                return Ok(CliMode::InspectPrompt(
                    args.get(2).cloned().unwrap_or_else(|| "lead".into()),
                ));
            }
            "goal" => {
                if index + 1 >= args.len() {
                    return Err("goal requires a description or subcommand".into());
                }
                return Ok(CliMode::Goal(args[index + 1..].to_vec()));
            }
            "ssh-hosts" => {
                return match args.get(2).map(String::as_str) {
                    None if args.len() == 2 => Ok(CliMode::SshHosts(SshHostsAction::List)),
                    Some("list") if args.len() == 3 => Ok(CliMode::SshHosts(SshHostsAction::List)),
                    Some("add") if args.len() == 5 => {
                        let alias = args[3].clone();
                        let directory = args[4].clone();
                        if !directory.starts_with('/') {
                            return Err(
                                "ssh-hosts add requires an absolute remote directory".into()
                            );
                        }
                        Ok(CliMode::SshHosts(SshHostsAction::Add { alias, directory }))
                    }
                    Some("remove") if args.len() == 4 => {
                        Ok(CliMode::SshHosts(SshHostsAction::Remove {
                            alias: args[3].clone(),
                        }))
                    }
                    Some("open") if args.len() == 4 => Ok(CliMode::SshHosts(
                        SshHostsAction::Open {
                            alias: args[3].clone(),
                        },
                    )),
                    _ => Err(
                        "usage: firmius ssh-hosts [list|add <alias> <absolute-dir>|remove <alias>|open <alias>]"
                            .into(),
                    ),
                };
            }
            "daemon" => {
                if args.len() != 2 {
                    return Err("daemon does not accept additional arguments".into());
                }
                return Ok(CliMode::Daemon);
            }
            "--resume" => {
                if resume_id.is_some() {
                    return Err("--resume may only be specified once".into());
                }
                let id = args
                    .get(index + 1)
                    .filter(|id| !id.starts_with('-'))
                    .ok_or("--resume requires a session id")?;
                resume_id = Some(id.clone());
                index += 2;
            }
            "--reset-onboarding" => {
                if reset_onboarding {
                    return Err("--reset-onboarding may only be specified once".into());
                }
                reset_onboarding = true;
                index += 1;
            }
            "--ssh" => {
                let alias = args
                    .get(index + 1)
                    .filter(|value| !value.starts_with('-'))
                    .ok_or("--ssh requires an SSH alias")?;
                let dir = args
                    .get(index + 2)
                    .filter(|value| !value.starts_with('-'))
                    .ok_or("--ssh requires a remote directory")?;
                if !dir.starts_with('/') {
                    return Err("--ssh remote directory must be an absolute path (for example /srv/project)".into());
                }
                if remote_workspace.is_some() {
                    return Err("--ssh may only be specified once".into());
                }
                remote_workspace = Some(format!("ssh://{alias}/{}", dir.trim_start_matches('/')));
                index += 3;
            }
            "--list-sessions" => {
                if list_sessions {
                    return Err("--list-sessions may only be specified once".into());
                }
                list_sessions = true;
                index += 1;
            }
            arg => return Err(format!("unrecognized argument: {arg}")),
        }
    }

    if list_sessions && (resume_id.is_some() || reset_onboarding) {
        return Err("--list-sessions cannot be combined with other arguments".into());
    }
    if resume_id.is_some() && reset_onboarding {
        return Err("--reset-onboarding cannot be combined with --resume".into());
    }
    if list_sessions {
        Ok(CliMode::ListSessions)
    } else {
        Ok(CliMode::Start {
            resume_id,
            reset_onboarding,
            remote_workspace,
        })
    }
}

fn load_user_settings(
    reset_onboarding: bool,
    load: impl FnOnce() -> Result<UserSettings, firmius_core::UserSettingsError>,
) -> Result<UserSettings, firmius_core::UserSettingsError> {
    match load() {
        Ok(mut settings) => {
            if reset_onboarding {
                settings.reset_onboarding();
                settings.save()?;
            }
            Ok(settings)
        }
        // Never replace settings that could not be loaded with writable
        // defaults. The OOBE may persist a dismissal later in startup, which
        // would otherwise overwrite an unreadable or malformed file.
        Err(error) => Err(error),
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    validate_early_command_shape(&args)?;
    if let Some(result) = install::handle_early_command(&args).await {
        return result;
    }
    let mut mode = parse_cli_mode(&args)?;
    if let CliMode::InspectPrompt(id) = &mode {
        let personas = PersonaManager::load_default()?;
        let persona = personas.require(id)?;
        let system = format!(
            "{}\n\n{}",
            firmius_core::prompts::OPERATING_PROMPT,
            persona.system_prompt
        );
        let tools = ToolRegistry::default();
        register_read_tool(&tools);
        register_edit_tool(&tools);
        register_grep_tool(&tools);
        register_glob_tool(&tools);
        register_list_tool(&tools);
        register_bash_tool(&tools);
        register_task_tool(&tools);
        register_message_tool(&tools);
        register_todo_tool(&tools);
        firmius_core::register_delegate_tool(&tools);
        let scopes = persona.tool_scopes.iter().cloned().collect();
        println!(
            "{}",
            serde_json::to_string_pretty(&serde_json::json!({
                "persona":id,"source":persona.path,"bundled":persona.bundled,
                "system_prompt":system,"tool_scopes":persona.tool_scopes,
                "tools":tools.definitions_scoped(Some(&scopes)),
                "note":"Startup policy and built-in tools for this persona. Active sessions may add operator overrides, assignment context, provider tools, and connected MCP tools."
            }))?
        );
        return Ok(());
    }
    if mode == CliMode::Daemon {
        firmius_service::run_default().await?;
        return Ok(());
    }
    if let CliMode::Goal(goal_args) = mode {
        run_goal_cli(&goal_args).await?;
        return Ok(());
    }
    if mode == CliMode::ListSessions {
        let workdir = std::env::current_dir().unwrap_or_default();
        for summary in firmius_core::list_sessions_for_workdir(Some(&workdir))? {
            println!(
                "{}\t{}\t{} agents\t{}\t{}",
                summary.id,
                summary.title,
                summary.agent_count,
                summary.model.as_deref().unwrap_or("-"),
                summary.updated_at.to_rfc3339()
            )
        }
        return Ok(());
    }
    if let CliMode::SshHosts(action) = mode.clone() {
        let hosts = firmius_core::discover_ssh_hosts()?;
        match action {
            SshHostsAction::List => {
                let settings = UserSettings::load()?;
                if hosts.is_empty() {
                    println!("No concrete SSH hosts found in ~/.ssh/config or ~/.ssh/known_hosts.");
                    println!("Add a Host entry, then run `firmius ssh-hosts` again.");
                }
                for host in hosts {
                    let saved = settings
                        .remote_hosts
                        .iter()
                        .find(|saved| saved.alias == host.alias)
                        .map(|saved| saved.directory.as_str())
                        .unwrap_or("-");
                    println!(
                        "{}\t{}\t{}\t{}\tsource:{}\tknown_hosts:{}\tsaved_dir:{}\tstart: firmius --ssh {} /absolute/remote/path",
                        host.alias,
                        host.hostname.as_deref().unwrap_or("-"),
                        host.user.as_deref().unwrap_or("-"),
                        host.port
                            .map(|port| port.to_string())
                            .unwrap_or_else(|| "22".into()),
                        if host.configured {
                            "config"
                        } else {
                            "known_hosts"
                        },
                        if host.known_hosts_present {
                            "present"
                        } else {
                            "missing"
                        },
                        saved,
                        host.alias,
                    );
                }
            }
            SshHostsAction::Add { alias, directory } => {
                if !hosts.iter().any(|host| host.alias == alias) {
                    return Err(format!(
                        "SSH host '{alias}' was not found in discovered SSH config or known_hosts"
                    )
                    .into());
                }
                let mut settings = UserSettings::load()?;
                settings.save_remote_host(alias.clone(), directory.clone())?;
                settings.save().map_err(|error| error.to_string())?;
                println!("Saved SSH host {alias} with workspace {directory}");
            }
            SshHostsAction::Remove { alias } => {
                let mut settings = UserSettings::load()?;
                if settings.remove_remote_host(&alias) {
                    settings.save().map_err(|error| error.to_string())?;
                    println!("Removed saved SSH host {alias}");
                } else {
                    println!("SSH host {alias} was not saved");
                }
                return Ok(());
            }
            SshHostsAction::Open { alias } => {
                let settings = UserSettings::load()?;
                let Some(saved) = settings
                    .remote_hosts
                    .iter()
                    .find(|host| host.alias == alias)
                else {
                    return Err(format!(
                        "SSH host '{alias}' has no saved workspace; run `firmius ssh-hosts add {alias} /absolute/remote/path` first"
                    )
                    .into());
                };
                let remote_workspace =
                    format!("ssh://{alias}/{}", saved.directory.trim_start_matches('/'));
                mode = CliMode::Start {
                    resume_id: None,
                    reset_onboarding: false,
                    remote_workspace: Some(remote_workspace),
                };
            }
        }
        if matches!(mode, CliMode::SshHosts(_)) {
            return Ok(());
        }
    }
    let CliMode::Start {
        resume_id,
        reset_onboarding,
        remote_workspace,
    } = mode
    else {
        unreachable!()
    };
    if let Some(workspace) = remote_workspace {
        // The TUI and daemon are created in this process; set the inherited
        // startup value before either constructs a session.
        unsafe {
            std::env::set_var("FIRMIUS_REMOTE_WORKSPACE", workspace);
        }
    }
    let interactive = std::io::stdout().is_terminal() && std::io::stdin().is_terminal();
    let frontend = select_frontend(interactive, resume_id.is_some())?;
    let daemon = if frontend == Frontend::Tui {
        match connect_or_spawn_daemon(resume_id.as_deref()).await {
            Ok(daemon) => Some(daemon),
            Err(error) => {
                if firmius_service::is_locked(&firmius_core::data_dir()) {
                    return Err(format!(
                        "daemon is still starting or unavailable while its profile lease is held: {error}"
                    ).into());
                }
                eprintln!("warning: daemon unavailable; using embedded runtime: {error}");
                None
            }
        }
    } else {
        None
    };
    let mut mgr = ProviderManager::new();
    let personas = Arc::new(PersonaManager::load_default().unwrap_or_else(|e| {
        eprintln!("warning: could not load personas: {e}");
        PersonaManager::default()
    }));
    let settings = Arc::new(std::sync::Mutex::new(load_user_settings(
        reset_onboarding,
        UserSettings::load,
    )?));
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
    mgr.register_kind(Arc::new(GrokBuildKind));
    mgr.register_kind(Arc::new(FreebuffKind));
    // Load any persisted providers/auth. On first run this is a no-op.
    mgr.load().unwrap_or_else(|e| eprintln!("warning: {e}"));

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
    register_message_tool(&tools);
    register_todo_tool(&tools);

    let tools = Arc::new(tools);

    // MCP: load persisted servers, start the enabled ones, and register their
    // tools so agents can call them through the shared registry.
    let mcp = Arc::new(McpManager::from_settings(
        McpSettings::load().unwrap_or_else(|e| {
            eprintln!("warning: could not load MCP settings: {e}");
            McpSettings::default()
        }),
    ));
    // Start MCP servers in the background so the TUI comes up instantly.
    // Each server's tools register into the shared registry as it finishes
    // its handshake; agents pick them up on their next request.
    let mcp_startup = mcp.clone();
    let tools_startup = tools.clone();
    tokio::spawn(async move {
        for result in mcp_startup.start_all().await {
            match result {
                Ok(specs) => {
                    register_tool_specs(tools_startup.as_ref(), mcp_startup.clone(), specs)
                }
                Err(error) => eprintln!("warning: could not start an MCP server: {error}"),
            }
        }
    });

    let manager = Arc::new(std::sync::Mutex::new(mgr.clone()));
    let (session, agent, active_provider_id) = if daemon.is_some() {
        (None, None, provider_id)
    } else if let Some(id) = resume_id {
        // Embedded fallback must validate a remote resume against the
        // persisted remote directory, not the local directory that launched
        // the TUI. The daemon normally handles this, but a daemon restart or
        // startup failure should not make an otherwise resumable SSH session
        // look like it belongs to another workspace.
        let workdir = startup_session_workdir();
        let record = firmius_core::load_session_record(&id)?;
        if !firmius_core::session_matches_workdir(&record, &workdir) {
            return Err("session does not belong to the current workdir".into());
        }
        let session = Session::resume_with_personas(&id, &mgr, tools.clone(), personas.clone())?
            .into_handle();
        for agent in session.agents.read().unwrap().values() {
            agent.attach_runtime(manager.clone(), settings.clone());
            agent.attach_firmius_config(config.clone());
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

    if frontend == Frontend::Tui {
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
            daemon,
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

#[cfg(test)]
mod tests {
    #[test]
    fn prompt_inspection_is_available_without_starting_a_session() {
        assert_eq!(
            super::parse_cli_mode(&["firmius".into(), "prompt".into()]),
            Ok(super::CliMode::InspectPrompt("lead".into()))
        );
        assert_eq!(
            super::parse_cli_mode(&["firmius".into(), "prompt".into(), "coder".into()]),
            Ok(super::CliMode::InspectPrompt("coder".into()))
        );
        assert!(
            super::parse_cli_mode(&[
                "firmius".into(),
                "prompt".into(),
                "coder".into(),
                "extra".into()
            ])
            .is_err()
        );
    }
    use super::*;

    fn args(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).into()).collect()
    }

    #[test]
    fn early_commands_are_exact_and_reject_extra_arguments() {
        for command in [
            "--help",
            "-h",
            "help",
            "--version",
            "-V",
            "doctor",
            "install-info",
            "daemon-stop",
            "update-check",
        ] {
            assert!(validate_early_command_shape(&args(&["firmius", command])).is_ok());
            assert!(validate_early_command_shape(&args(&["firmius", command, "extra"])).is_err());
        }
        // Update owns its stricter error so safety-sensitive options such as
        // `--yes` cannot accidentally fall through into normal startup.
        for command in ["update", "--update"] {
            assert!(validate_early_command_shape(&args(&["firmius", command, "--yes"])).is_ok());
        }
    }

    #[test]
    fn list_sessions_is_a_standalone_early_mode() {
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "--list-sessions"])),
            Ok(CliMode::ListSessions)
        );
        assert!(
            parse_cli_mode(&args(&["firmius", "--list-sessions", "--reset-onboarding"])).is_err()
        );
        assert!(parse_cli_mode(&args(&["firmius", "--resume", "abc", "--list-sessions"])).is_err());
    }

    #[test]
    fn ssh_host_commands_parse_saved_workspace_operations() {
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "ssh-hosts"])),
            Ok(CliMode::SshHosts(SshHostsAction::List))
        );
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "ssh-hosts", "list"])),
            Ok(CliMode::SshHosts(SshHostsAction::List))
        );
        assert_eq!(
            parse_cli_mode(&args(&[
                "firmius",
                "ssh-hosts",
                "add",
                "build",
                "/srv/project"
            ])),
            Ok(CliMode::SshHosts(SshHostsAction::Add {
                alias: "build".into(),
                directory: "/srv/project".into(),
            }))
        );
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "ssh-hosts", "remove", "build"])),
            Ok(CliMode::SshHosts(SshHostsAction::Remove {
                alias: "build".into(),
            }))
        );
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "ssh-hosts", "open", "build"])),
            Ok(CliMode::SshHosts(SshHostsAction::Open {
                alias: "build".into(),
            }))
        );
        assert!(
            parse_cli_mode(&args(
                &["firmius", "ssh-hosts", "add", "build", "relative",]
            ))
            .is_err()
        );
    }

    #[test]
    fn start_mode_parses_flags_and_never_resets_onboarding_during_resume() {
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "--resume", "session-1"])),
            Ok(CliMode::Start {
                resume_id: Some("session-1".into()),
                reset_onboarding: false,
                remote_workspace: None,
            })
        );
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "--reset-onboarding"])),
            Ok(CliMode::Start {
                resume_id: None,
                reset_onboarding: true,
                remote_workspace: None,
            })
        );
        assert!(
            parse_cli_mode(&args(&[
                "firmius",
                "--resume",
                "session-1",
                "--reset-onboarding"
            ]))
            .is_err()
        );
        assert!(parse_cli_mode(&args(&["firmius", "--resume"])).is_err());
        assert!(parse_cli_mode(&args(&["firmius", "--resume", "--list-sessions"])).is_err());
        assert!(parse_cli_mode(&args(&["firmius", "--unknown"])).is_err());
    }

    #[test]
    fn ssh_start_requires_an_absolute_remote_directory() {
        assert!(parse_cli_mode(&args(&["firmius", "--ssh", "build", "relative"])).is_err());
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "--ssh", "build", "/srv/project"])),
            Ok(CliMode::Start {
                resume_id: None,
                reset_onboarding: false,
                remote_workspace: Some("ssh://build/srv/project".into()),
            })
        );
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "--ssh", "build", "/"])),
            Ok(CliMode::Start {
                resume_id: None,
                reset_onboarding: false,
                remote_workspace: Some("ssh://build/".into()),
            })
        );
    }

    #[test]
    fn goal_mode_requires_arguments_and_preserves_subcommands() {
        assert!(parse_cli_mode(&args(&["firmius", "goal"])).is_err());
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "goal", "create", "ship", "it"])),
            Ok(CliMode::Goal(vec![
                "create".into(),
                "ship".into(),
                "it".into()
            ]))
        );
        assert_eq!(
            parse_cli_mode(&args(&["firmius", "goal", "list"])),
            Ok(CliMode::Goal(vec!["list".into()]))
        );
    }

    #[test]
    fn non_tty_requires_resume_and_resumed_non_tty_uses_repl() {
        assert_eq!(
            select_frontend(false, false),
            Err("interactive terminal required to configure Firmius without a provider")
        );
        assert_eq!(select_frontend(false, true), Ok(Frontend::Repl));
        assert_eq!(select_frontend(true, false), Ok(Frontend::Tui));
    }

    #[test]
    fn embedded_resume_uses_remote_directory_when_startup_override_is_set() {
        assert_eq!(
            startup_session_workdir_from(Some("ssh://build/srv/work tree")),
            std::path::PathBuf::from("/srv/work tree")
        );
        assert_eq!(
            startup_session_workdir_from(Some("/local/workspace")),
            std::env::current_dir().unwrap()
        );
    }

    #[test]
    fn reset_onboarding_does_not_overwrite_malformed_settings() {
        let root = std::env::temp_dir().join(format!(
            "firmius-reset-onboarding-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let path = root.join("settings.json");
        let malformed = b"{ definitely not valid settings JSON";
        std::fs::write(&path, malformed).unwrap();

        let result = load_user_settings(true, || UserSettings::load_from_path(&path));

        assert!(result.is_err());
        assert_eq!(std::fs::read(&path).unwrap(), malformed);
        std::fs::remove_dir_all(root).ok();
    }

    #[test]
    fn ordinary_load_does_not_make_malformed_settings_writable() {
        let root = std::env::temp_dir().join(format!(
            "firmius-load-settings-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let path = root.join("settings.json");
        let malformed = b"{ settings must not be replaced by OOBE persistence";
        std::fs::write(&path, malformed).unwrap();

        let result = load_user_settings(false, || UserSettings::load_from_path(&path));

        assert!(result.is_err());
        assert_eq!(std::fs::read(&path).unwrap(), malformed);
        std::fs::remove_dir_all(root).ok();
    }
}
