//! MCP (Model Context Protocol) client support.
//!
//! Firmius acts as an MCP *client*: it spawns or connects to external MCP
//! servers, discovers their tools, and exposes those tools to agents through
//! the normal [`ToolRegistry`]. Server definitions are persisted in
//! `~/.firmius/mcp.json` and managed live from the TUI (`/mcp` command).
//!
//! Transports:
//! - `stdio`: launch a child process and speak JSON-RPC over its pipes.
//! - `http`: connect to a Streamable HTTP MCP endpoint over HTTPS/HTTP.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::Arc;

use async_trait::async_trait;
use rmcp::ServiceExt;
use rmcp::model::{CallToolRequestParams, CallToolResult, ContentBlock};
use rmcp::service::{RoleClient, RunningService};
use rmcp::transport::StreamableHttpClientTransport;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use tokio::sync::Mutex as AsyncMutex;

use crate::{Tool, ToolContext, ToolError, ToolRegistry};

pub const MCP_SETTINGS_VERSION: u32 = 1;

fn default_true() -> bool {
    true
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

#[derive(Debug, thiserror::Error)]
pub enum McpError {
    #[error("MCP server not found: {0}")]
    NotFound(String),
    #[error("MCP server '{0}' is not running")]
    NotRunning(String),
    #[error("invalid MCP server config: {0}")]
    Invalid(String),
    #[error("failed to spawn MCP server: {0}")]
    Spawn(String),
    #[error("MCP handshake failed: {0}")]
    Handshake(String),
    #[error("MCP call failed: {0}")]
    Call(String),
    #[error("MCP settings I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        source: std::io::Error,
    },
    #[error("MCP settings JSON error at {path}: {source}")]
    Json {
        path: PathBuf,
        source: serde_json::Error,
    },
}

impl From<McpError> for ToolError {
    fn from(error: McpError) -> Self {
        ToolError::Failed(error.to_string())
    }
}

// ---------------------------------------------------------------------------
// Persisted configuration
// ---------------------------------------------------------------------------

/// One MCP server definition. `command` selects stdio transport, `url` selects
/// Streamable HTTP; exactly one must be present.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct McpServerConfig {
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub command: Option<String>,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default)]
    pub env: BTreeMap<String, String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    /// Working directory to launch the stdio child process in.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cwd: Option<String>,
    /// Start this server automatically when Firmius launches.
    #[serde(default = "default_true")]
    pub enabled: bool,
}

impl McpServerConfig {
    pub fn stdio(name: impl Into<String>, command: impl Into<String>, args: Vec<String>) -> Self {
        Self {
            name: name.into(),
            command: Some(command.into()),
            args,
            env: BTreeMap::new(),
            url: None,
            cwd: None,
            enabled: true,
        }
    }

    pub fn http(name: impl Into<String>, url: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            command: None,
            args: Vec::new(),
            env: BTreeMap::new(),
            url: Some(url.into()),
            cwd: None,
            enabled: true,
        }
    }

    pub fn transport(&self) -> &'static str {
        if self.command.is_some() {
            "stdio"
        } else {
            "http"
        }
    }

    fn validate(&self) -> Result<(), McpError> {
        if self.name.trim().is_empty() {
            return Err(McpError::Invalid("server name must not be empty".into()));
        }
        match (self.command.as_deref(), self.url.as_deref()) {
            (Some(cmd), None) if !cmd.trim().is_empty() => Ok(()),
            (None, Some(url)) if !url.trim().is_empty() => Ok(()),
            _ => Err(McpError::Invalid(
                "server needs exactly one of `command` (stdio) or `url` (http)".into(),
            )),
        }
    }
}

/// Persisted set of MCP servers, mirroring the `UserSettings` store shape.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct McpSettings {
    pub version: u32,
    #[serde(default)]
    pub servers: BTreeMap<String, McpServerConfig>,
    #[serde(skip)]
    storage_path: Option<PathBuf>,
}

impl McpSettings {
    pub fn load() -> Result<Self, McpError> {
        Self::load_from(default_mcp_settings_path()?)
    }

    pub fn load_from(path: impl Into<PathBuf>) -> Result<Self, McpError> {
        let path = path.into();
        if !path.exists() {
            return Ok(Self {
                storage_path: Some(path),
                ..Self::default()
            });
        }
        let content = std::fs::read_to_string(&path).map_err(|source| McpError::Io {
            path: path.clone(),
            source,
        })?;
        let mut settings: Self =
            serde_json::from_str(&content).map_err(|source| McpError::Json {
                path: path.clone(),
                source,
            })?;
        settings.storage_path = Some(path);
        Ok(settings)
    }

    pub fn save(&self) -> Result<(), McpError> {
        let path = match &self.storage_path {
            Some(path) => path.clone(),
            None => default_mcp_settings_path()?,
        };
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|source| McpError::Io {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        let bytes = serde_json::to_vec_pretty(self).map_err(|source| McpError::Json {
            path: path.clone(),
            source,
        })?;
        let tmp = path.with_extension(format!("json.tmp.{}", std::process::id()));
        std::fs::write(&tmp, bytes).map_err(|source| McpError::Io {
            path: tmp.clone(),
            source,
        })?;
        std::fs::rename(&tmp, &path).map_err(|source| McpError::Io {
            path: path.clone(),
            source,
        })?;
        Ok(())
    }

    pub fn upsert(&mut self, config: McpServerConfig) -> Result<(), McpError> {
        config.validate()?;
        self.servers.insert(config.name.clone(), config);
        Ok(())
    }

    pub fn remove(&mut self, name: &str) {
        self.servers.remove(name);
    }
}

pub fn default_mcp_settings_path() -> Result<PathBuf, McpError> {
    Ok(crate::persistence::data_dir().join("mcp.json"))
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

/// A tool discovered from a running MCP server, converted into Firmius's tool
/// shape.
#[derive(Debug, Clone)]
pub struct McpToolSpec {
    pub server: String,
    pub name: String,
    pub description: String,
    pub input_schema: Value,
}

/// Live status for one configured server.
#[derive(Debug, Clone)]
pub struct McpServerStatus {
    pub name: String,
    pub transport: &'static str,
    pub running: bool,
    pub tool_count: usize,
    pub enabled: bool,
}

struct RunningMcpServer {
    client: RunningService<RoleClient, ()>,
    child: Option<tokio::process::Child>,
    tools: Vec<McpToolSpec>,
}

/// A [`Tool`] adapter that forwards calls to a live MCP server. Registered into
/// the shared [`ToolRegistry`] under `mcp__<server>__<tool>`.
pub struct McpTool {
    manager: Arc<McpManager>,
    server: String,
    name: String,
    description: String,
    input_schema: Value,
}

impl McpTool {
    fn new(
        manager: Arc<McpManager>,
        server: String,
        name: String,
        description: String,
        input_schema: Value,
    ) -> Self {
        Self {
            manager,
            server,
            name,
            description,
            input_schema,
        }
    }

    /// Registry name for this tool: `mcp__<server>__<tool>`.
    pub fn registry_name(server: &str, tool: &str) -> String {
        format!("mcp__{server}__{tool}")
    }
}

#[async_trait]
impl Tool for McpTool {
    fn name(&self) -> &str {
        &self.name
    }

    fn description(&self) -> &str {
        &self.description
    }

    fn input_schema(&self) -> Value {
        self.input_schema.clone()
    }

    async fn call(&self, args: Value, _ctx: ToolContext) -> Result<String, ToolError> {
        self.manager
            .call_tool(&self.server, &self.name, args)
            .await
            .map_err(ToolError::from)
    }
}

/// Shared MCP client manager. Cheap to clone via `Arc`.
pub struct McpManager {
    settings: AsyncMutex<McpSettings>,
    running: AsyncMutex<BTreeMap<String, RunningMcpServer>>,
}

impl Default for McpManager {
    fn default() -> Self {
        Self {
            settings: AsyncMutex::new(McpSettings::default()),
            running: AsyncMutex::new(BTreeMap::new()),
        }
    }
}

impl McpManager {
    pub fn from_settings(settings: McpSettings) -> Self {
        Self {
            settings: AsyncMutex::new(settings),
            running: AsyncMutex::new(BTreeMap::new()),
        }
    }

    pub async fn settings(&self) -> McpSettings {
        self.settings.lock().await.clone()
    }

    /// Persist a new or updated server definition.
    pub async fn add_server(&self, config: McpServerConfig) -> Result<(), McpError> {
        let mut settings = self.settings.lock().await;
        settings.upsert(config)?;
        settings.save()
    }

    /// Remove a server definition and stop it if running.
    pub async fn remove_server(&self, name: &str) -> Result<(), McpError> {
        let _ = self.stop(name).await;
        let mut settings = self.settings.lock().await;
        settings.remove(name);
        settings.save()
    }

    /// Connect to one configured server and discover its tools.
    pub async fn start(&self, name: &str) -> Result<Vec<McpToolSpec>, McpError> {
        let config = self
            .settings
            .lock()
            .await
            .servers
            .get(name)
            .cloned()
            .ok_or_else(|| McpError::NotFound(name.to_string()))?;
        self.start_config(config).await
    }

    /// Start every enabled server, collecting per-server results.
    pub async fn start_all(&self) -> Vec<Result<Vec<McpToolSpec>, McpError>> {
        let enabled: Vec<McpServerConfig> = self
            .settings
            .lock()
            .await
            .servers
            .values()
            .filter(|config| config.enabled)
            .cloned()
            .collect();
        let mut results = Vec::with_capacity(enabled.len());
        for config in enabled {
            results.push(self.start_config(config).await);
        }
        results
    }

    async fn start_config(&self, config: McpServerConfig) -> Result<Vec<McpToolSpec>, McpError> {
        config.validate()?;
        let _ = self.stop(&config.name).await;

        let (client, child) = connect(&config).await?;
        let discovered = client
            .list_all_tools()
            .await
            .map_err(|e| McpError::Call(e.to_string()))?;
        let mut specs = Vec::with_capacity(discovered.len());
        for tool in discovered {
            let name = tool.name.to_string();
            let description = tool.description.as_deref().unwrap_or_default().to_string();
            let input_schema = tool.schema_as_json_value();
            specs.push(McpToolSpec {
                server: config.name.clone(),
                name,
                description,
                input_schema,
            });
        }

        self.running.lock().await.insert(
            config.name.clone(),
            RunningMcpServer {
                client,
                child,
                tools: specs.clone(),
            },
        );
        Ok(specs)
    }

    /// Stop a running server: close the MCP session and kill its child process
    /// (stdio) when present. Returns the tools that were registered for it.
    pub async fn stop(&self, name: &str) -> Result<Vec<McpToolSpec>, McpError> {
        let Some(mut running) = self.running.lock().await.remove(name) else {
            return Ok(Vec::new());
        };
        let _ = running.client.close().await;
        if let Some(mut child) = running.child {
            let _ = child.kill().await;
            let _ = child.wait().await;
        }
        Ok(running.tools)
    }

    pub async fn restart(&self, name: &str) -> Result<Vec<McpToolSpec>, McpError> {
        let _ = self.stop(name).await;
        self.start(name).await
    }

    pub async fn status(&self) -> Vec<McpServerStatus> {
        let settings = self.settings.lock().await;
        let running = self.running.lock().await;
        settings
            .servers
            .values()
            .map(|config| McpServerStatus {
                name: config.name.clone(),
                transport: config.transport(),
                running: running.contains_key(&config.name),
                tool_count: running
                    .get(&config.name)
                    .map(|r| r.tools.len())
                    .unwrap_or(0),
                enabled: config.enabled,
            })
            .collect()
    }

    /// Invoke one tool on a live server, returning rendered text.
    pub async fn call_tool(
        &self,
        server: &str,
        tool: &str,
        args: Value,
    ) -> Result<String, McpError> {
        let running = self.running.lock().await;
        let Some(entry) = running.get(server) else {
            return Err(McpError::NotRunning(server.to_string()));
        };
        let arguments = match args {
            Value::Object(map) => map,
            Value::Null => serde_json::Map::new(),
            other => {
                let mut map = serde_json::Map::new();
                map.insert("value".to_string(), other);
                map
            }
        };
        let params = CallToolRequestParams::new(tool.to_string()).with_arguments(arguments);
        let result = entry
            .client
            .call_tool(params)
            .await
            .map_err(|e| McpError::Call(e.to_string()))?;
        render_call_tool_result(result)
    }
}

async fn connect(
    config: &McpServerConfig,
) -> Result<
    (
        RunningService<RoleClient, ()>,
        Option<tokio::process::Child>,
    ),
    McpError,
> {
    if let Some(command) = &config.command {
        let mut cmd = tokio::process::Command::new(command);
        cmd.args(&config.args)
            .envs(&config.env)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true);
        if let Some(cwd) = &config.cwd {
            cmd.current_dir(cwd);
        }
        let mut child = cmd.spawn().map_err(|e| McpError::Spawn(e.to_string()))?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| McpError::Spawn("child has no stdin".into()))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| McpError::Spawn("child has no stdout".into()))?;
        let client =
            ().serve((stdout, stdin))
                .await
                .map_err(|e| McpError::Handshake(e.to_string()))?;
        Ok((client, Some(child)))
    } else if let Some(url) = &config.url {
        let transport = StreamableHttpClientTransport::from_uri(url.as_str());
        let client = ().serve(transport).await.map_err(|e| McpError::Handshake(e.to_string()))?;
        Ok((client, None))
    } else {
        Err(McpError::Invalid(
            "server needs a `command` (stdio) or `url` (http)".into(),
        ))
    }
}

fn render_call_tool_result(result: CallToolResult) -> Result<String, McpError> {
    let text = result
        .content
        .iter()
        .map(render_content_block)
        .collect::<Vec<_>>()
        .join("\n");
    if result.is_error == Some(true) {
        return Err(McpError::Call(if text.is_empty() {
            "tool reported an error".to_string()
        } else {
            text
        }));
    }
    Ok(if text.is_empty() {
        "(no output)".to_string()
    } else {
        text
    })
}

fn render_content_block(block: &ContentBlock) -> String {
    if let Some(text) = block.as_text() {
        return text.text.clone();
    }
    serde_json::to_string(block).unwrap_or_else(|_| format!("{block:?}"))
}

/// Register the tools of every configured-and-enabled server that is already
/// running. Callers typically start servers first, then pass the returned
/// specs here.
pub fn register_tool_specs(
    registry: &ToolRegistry,
    manager: Arc<McpManager>,
    specs: Vec<McpToolSpec>,
) {
    for spec in specs {
        let registry_name = McpTool::registry_name(&spec.server, &spec.name);
        registry.register(McpTool::new(
            manager.clone(),
            spec.server,
            registry_name,
            spec.description,
            spec.input_schema,
        ));
    }
}

/// Unregister every tool belonging to a server (called when it stops).
pub fn unregister_tool_specs(registry: &ToolRegistry, specs: &[McpToolSpec]) {
    for spec in specs {
        registry.unregister(&McpTool::registry_name(&spec.server, &spec.name));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_round_trips_and_validates() {
        let config = McpServerConfig::stdio("ast-grep", "ast-grep", vec!["mcp".into()]);
        assert_eq!(config.transport(), "stdio");
        config.validate().unwrap();

        let http = McpServerConfig::http("remote", "https://example.com/mcp");
        assert_eq!(http.transport(), "http");
        http.validate().unwrap();

        let broken = McpServerConfig {
            name: "broken".into(),
            command: None,
            args: vec![],
            env: BTreeMap::new(),
            url: None,
            cwd: None,
            enabled: true,
        };
        assert!(broken.validate().is_err());
    }

    #[test]
    fn tool_registry_names_are_namespaced() {
        assert_eq!(
            McpTool::registry_name("ast-grep", "search"),
            "mcp__ast-grep__search"
        );
    }
}
