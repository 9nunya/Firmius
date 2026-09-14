//! Desktop mcp feature.
use super::*;

pub(super) fn refresh_mcp(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session before inspecting MCP servers");
        return;
    };
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, _) = shared_snapshot(&state, &active).await?;
                match client
                    .request(Request::Mcp {
                        command: McpCommand::List,
                    })
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Mcp(servers) => Ok(servers),
                    other => Err(format!(
                        "daemon returned an unexpected MCP response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok(servers) => {
                invoke(weak, move |ui| {
                    let rows = if servers.is_empty() {
                        vec![transcript_row(
                            "MCP",
                            "No MCP servers are configured for this session.",
                            "assistant",
                            "ready",
                        )]
                    } else {
                        servers
                            .into_iter()
                            .map(|server| {
                                transcript_row(
                                    "MCP SERVER",
                                    format!(
                                        "{} · {} · {} tool(s)",
                                        server.name, server.transport, server.tool_count
                                    ),
                                    if server.running { "tool" } else { "error" },
                                    format!(
                                        "{} · {}",
                                        if server.running { "running" } else { "stopped" },
                                        if server.enabled {
                                            "enabled"
                                        } else {
                                            "disabled"
                                        }
                                    ),
                                )
                            })
                            .collect()
                    };
                    ui.set_active_view("runtime".into());
                    ui.invoke_show_document("MCP".into(), ModelRc::new(VecModel::from(rows)));
                    ui.set_notice("MCP inventory refreshed".into());
                });
            }
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

pub(super) fn manage_mcp(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    action: String,
    name: String,
) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session before managing MCP servers");
        return;
    };
    let name = name.trim().to_string();
    if name.is_empty() {
        set_notice(ui, "MCP server name is required");
        return;
    }
    let command = match action.as_str() {
        "start" => McpCommand::Start { name: name.clone() },
        "stop" => McpCommand::Stop { name: name.clone() },
        "restart" => McpCommand::Restart { name: name.clone() },
        "remove" => McpCommand::Remove { name: name.clone() },
        _ => {
            set_notice(ui, format!("Unsupported MCP action: {action}"));
            return;
        }
    };
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, _) = shared_snapshot(&state, &active).await?;
                match client
                    .request(Request::Mcp { command })
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Mcp(_) => Ok(()),
                    other => Err(format!(
                        "daemon returned an unexpected MCP response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok(()) => invoke(weak, move |ui| {
                ui.set_mcp_open(false);
                let verb = match action.as_str() {
                    "remove" => "removed",
                    "restart" => "restarted",
                    "start" => "started",
                    "stop" => "stopped",
                    _ => action.as_str(),
                };
                ui.set_notice(format!("MCP server {name} {verb}").into());
                ui.invoke_run_command("mcp".into());
            }),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

pub(super) fn prepare_mcp_config(
    name: &str,
    command: &str,
    url: &str,
    args: &str,
) -> Result<McpServerConfig, String> {
    let name = name.trim();
    let command = command.trim();
    let url = url.trim();
    if name.is_empty() {
        return Err("MCP server name is required".into());
    }
    match (command.is_empty(), url.is_empty()) {
        (false, true) => Ok(McpServerConfig::stdio(
            name,
            command,
            args.split_whitespace().map(ToOwned::to_owned).collect(),
        )),
        (true, false) => {
            if !(url.starts_with("http://") || url.starts_with("https://")) {
                return Err("HTTP MCP URL must start with http:// or https://".into());
            }
            if !args.trim().is_empty() {
                return Err("HTTP MCP servers do not take stdio arguments".into());
            }
            Ok(McpServerConfig::http(name, url))
        }
        _ => Err("Provide exactly one stdio command or HTTP URL".into()),
    }
}

pub(super) fn add_mcp(
    ui: &MainWindow,
    state: Arc<Mutex<DesktopState>>,
    name: String,
    command: String,
    url: String,
    args: String,
) {
    let active = state.lock().ok().and_then(|state| state.active_session());
    let Some(active) = active else {
        set_notice(ui, "Open a session before adding an MCP server");
        return;
    };
    let config = match prepare_mcp_config(&name, &command, &url, &args) {
        Ok(config) => config,
        Err(error) => {
            set_notice(ui, error);
            return;
        }
    };
    let name = config.name.clone();
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = runtime().and_then(|runtime| {
            runtime.block_on(async {
                let (client, _) = shared_snapshot(&state, &active).await?;
                match client
                    .request(Request::Mcp {
                        command: McpCommand::Add { config },
                    })
                    .await
                    .map_err(|error| error.to_string())?
                {
                    Response::Mcp(_) => Ok(()),
                    other => Err(format!(
                        "daemon returned an unexpected MCP response: {other:?}"
                    )),
                }
            })
        });
        match result {
            Ok(()) => invoke(weak, move |ui| {
                ui.set_mcp_open(false);
                ui.set_mcp_command("".into());
                ui.set_mcp_url("".into());
                ui.set_mcp_args("".into());
                ui.set_notice(format!("MCP server {name} added").into());
                ui.invoke_run_command("mcp".into());
            }),
            Err(error) => invoke(weak, move |ui| ui.set_notice(error.into())),
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mcp_add_requires_a_name_and_exactly_one_transport() {
        assert!(prepare_mcp_config("", "npx", "", "").is_err());
        assert!(prepare_mcp_config("docs", "", "", "").is_err());
        assert!(prepare_mcp_config("docs", "npx", "https://example.com/mcp", "").is_err());
        let stdio = prepare_mcp_config("docs", "npx", "", "-y server").unwrap();
        assert_eq!(stdio.transport(), "stdio");
        assert_eq!(stdio.command.as_deref(), Some("npx"));
        assert_eq!(stdio.args, ["-y", "server"]);
        assert!(prepare_mcp_config("remote", "", "example.com/mcp", "").is_err());
        assert!(prepare_mcp_config("remote", "", "https://example.com/mcp", "-y extra").is_err());
        let http = prepare_mcp_config("remote", "", "https://example.com/mcp", "").unwrap();
        assert_eq!(http.transport(), "http");
        assert_eq!(http.url.as_deref(), Some("https://example.com/mcp"));
        assert!(http.args.is_empty());
    }
}
