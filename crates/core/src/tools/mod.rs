use async_trait::async_trait;
use schemars::JsonSchema;
use serde::de::DeserializeOwned;
use serde_json::Value;
use std::{
    collections::{HashMap, HashSet},
    path::PathBuf,
    sync::Arc,
};
use tokio_util::sync::CancellationToken;

use crate::agent::AgentState;
use crate::artifact::SessionArtifacts;
use crate::host::Host;
use crate::session::SessionHandle;

pub mod bash;
pub mod delegate;
pub mod edit;
pub mod flex;
pub mod glob;
pub mod grep;
pub mod list;
pub mod message;
pub mod read;
pub mod task;

pub use bash::register_bash_tool;
pub use delegate::register_delegate_tool;
pub use edit::register_edit_tool;
pub use glob::register_glob_tool;
pub use grep::register_grep_tool;
pub use list::register_list_tool;
pub use message::register_message_tool;
pub use read::register_read_tool;
pub use task::{WORK_READ_SCOPE, WORK_WRITE_SCOPE, register_task_tool};

pub const WORKER_YIELD_SCOPE: &str = "worker_yield";
pub use yield_tool::register_yield_tool;
mod yield_tool;

/// Maximum amount of a tool result that is kept in the model context.
///
/// Tool output can be arbitrarily large (for example, `read` on a generated
/// bundle or `bash` running a verbose command). Keeping a generous inline
/// ceiling prevents one result from stalling the agent or consuming its whole
/// context. The complete result is written to a temporary file instead.
pub const MAX_INLINE_TOOL_RESULT_BYTES: usize = 256 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ToolOutput {
    Content(String),
    StopTurn { content: String },
}

/// Replace an oversized result with a small, actionable pointer. The complete
/// output remains available on disk so the agent can use `read` with a region
/// when it needs to inspect it carefully.
pub fn redirect_large_tool_result(content: String) -> String {
    if content.len() <= MAX_INLINE_TOOL_RESULT_BYTES {
        return content;
    }

    let directory = std::env::temp_dir().join("firmius-tool-results");
    let path = directory.join(format!("{}.txt", uuid::Uuid::new_v4()));
    if let Err(error) =
        std::fs::create_dir_all(&directory).and_then(|_| std::fs::write(&path, content.as_bytes()))
    {
        // Do not put the original multi-megabyte value back into the context
        // if the filesystem is unavailable. This fallback is deliberately
        // bounded as well.
        let preview = String::from_utf8_lossy(
            &content.as_bytes()[..MAX_INLINE_TOOL_RESULT_BYTES.min(content.len())],
        );
        return format!(
            "tool output was {} bytes, but could not be redirected to a temporary file ({error});\n\n{}\n\n[output preview truncated]",
            content.len(),
            preview
        );
    }

    format!(
        "Tool output was {} bytes and has been redirected to the temporary file:\n{}\n\nRead that file carefully with the read tool, preferably by requesting only the relevant region (start_line/end_line or offset/max_lines).",
        content.len(),
        path.display()
    )
}

#[derive(Clone)]
pub struct ToolContext {
    pub workdir: PathBuf,
    pub cancellation: CancellationToken,
    /// Id of the tool call currently executing — lets tools that spawn
    /// agents (e.g. `delegate`) record exactly which call created them.
    pub tool_call_id: String,
    /// Which agent invoked this tool.
    pub agent_id: String,
    /// Which session the agent belongs to.
    pub session_id: String,
    /// Shared agent state — tools can read history, usage, or inject messages.
    pub state: Arc<std::sync::RwLock<AgentState>>,
    /// OS boundary: process control, shared by every tool call an agent makes
    /// so that a `bash spawn` in one turn is still visible to `bash poll` in
    /// the next. One `Host` per agent (see `Agent::host()`), not per call.
    pub host: Arc<dyn Host>,
    /// Handle to the owning session, for tools that need to spawn/inspect
    /// other agents (e.g. `delegate`). `None` when an agent is run outside
    /// a session (e.g. in unit tests) — those tools then fail cleanly with
    /// `ToolError::Failed` rather than panicking.
    pub session: Option<SessionHandle>,
    /// The calling agent's allowed persona scopes for this turn, mirroring the
    /// set `ToolRegistry::call_scoped` used. `None` means unrestricted (legacy
    /// agents without a persona). Tools with mode-specific permissions (e.g.
    /// `delegate`) use this for fine-grained checks.
    pub allowed_scopes: Option<HashSet<String>>,
}
impl ToolContext {
    pub fn cancelled(&self) -> bool {
        self.cancellation.is_cancelled()
    }
}

/// Resolve the calling agent's session artifact store, if any. Tools use this
/// to route `artifact://...` paths to session memory instead of the filesystem.
pub async fn session_artifacts(ctx: &ToolContext) -> Option<Arc<SessionArtifacts>> {
    let session = ctx.session.as_ref()?;
    Some(session.artifacts.clone())
}

#[derive(Debug, thiserror::Error)]
pub enum ToolError {
    #[error("invalid arguments: {0}")]
    InvalidArguments(String),
    #[error("tool failed: {0}")]
    Failed(String),
    #[error(
        "permission denied for tool `{tool}`: requires scope(s) {required:?}; allowed scope(s): {allowed:?}"
    )]
    PermissionDenied {
        tool: String,
        required: Vec<String>,
        allowed: Vec<String>,
    },
    #[error("cancelled")]
    Cancelled,
}

#[async_trait]
pub trait Tool: Send + Sync {
    fn name(&self) -> &str;
    fn description(&self) -> &str;
    fn input_schema(&self) -> Value;
    fn required_scopes(&self) -> &[String] {
        &[]
    }
    async fn call(&self, args: Value, ctx: ToolContext) -> Result<String, ToolError>;
    async fn call_output(&self, args: Value, ctx: ToolContext) -> Result<ToolOutput, ToolError> {
        self.call(args, ctx).await.map(ToolOutput::Content)
    }
}

pub struct TypedTool<A, F> {
    pub name: String,
    pub description: String,
    pub schema: Value,
    pub required_scopes: Vec<String>,
    pub handler: F,
    _args: std::marker::PhantomData<A>,
}
impl<A, F> TypedTool<A, F>
where
    A: DeserializeOwned + JsonSchema + Send + Sync + 'static,
    F: Fn(A, ToolContext) -> futures::future::BoxFuture<'static, Result<String, ToolError>>
        + Send
        + Sync,
{
    pub fn new(name: impl Into<String>, description: impl Into<String>, handler: F) -> Self {
        Self {
            name: name.into(),
            description: description.into(),
            schema: serde_json::to_value(schemars::schema_for!(A)).unwrap_or(Value::Null),
            required_scopes: Vec::new(),
            handler,
            _args: std::marker::PhantomData,
        }
    }

    pub fn with_required_scopes<I, S>(mut self, scopes: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        self.required_scopes = scopes.into_iter().map(Into::into).collect();
        self
    }
}
#[async_trait]
impl<A, F> Tool for TypedTool<A, F>
where
    A: DeserializeOwned + JsonSchema + Send + Sync + 'static,
    F: Fn(A, ToolContext) -> futures::future::BoxFuture<'static, Result<String, ToolError>>
        + Send
        + Sync,
{
    fn name(&self) -> &str {
        &self.name
    }
    fn description(&self) -> &str {
        &self.description
    }
    fn input_schema(&self) -> Value {
        self.schema.clone()
    }
    fn required_scopes(&self) -> &[String] {
        &self.required_scopes
    }
    async fn call(&self, args: Value, ctx: ToolContext) -> Result<String, ToolError> {
        let args =
            serde_json::from_value(args).map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
        (self.handler)(args, ctx).await
    }
}

#[derive(Default)]
pub struct ToolRegistry {
    tools: std::sync::RwLock<HashMap<String, Arc<dyn Tool>>>,
}
impl ToolRegistry {
    pub fn register<T: Tool + 'static>(&self, tool: T) {
        self.tools
            .write()
            .unwrap()
            .insert(tool.name().to_owned(), Arc::new(tool));
    }

    /// Remove and return a previously registered tool by name. Used to drop
    /// dynamic tools (e.g. MCP tools) when their backing server stops.
    pub fn unregister(&self, name: &str) -> Option<Arc<dyn Tool>> {
        self.tools.write().unwrap().remove(name)
    }

    pub fn definitions(&self) -> Vec<crate::ToolDefinition> {
        self.definitions_scoped(None)
    }

    pub fn definitions_scoped(
        &self,
        allowed_scopes: Option<&HashSet<String>>,
    ) -> Vec<crate::ToolDefinition> {
        let tools = self.tools.read().unwrap();
        tools
            .values()
            .filter(|t| Self::scope_allowed(t.required_scopes(), allowed_scopes))
            .map(|t| crate::ToolDefinition {
                name: t.name().into(),
                description: t.description().into(),
                input_schema: t.input_schema(),
            })
            .collect()
    }
    pub async fn call(
        &self,
        name: &str,
        args: Value,
        ctx: ToolContext,
    ) -> Result<String, ToolError> {
        self.call_scoped(name, args, ctx, None).await
    }

    pub async fn call_scoped(
        &self,
        name: &str,
        args: Value,
        ctx: ToolContext,
        allowed_scopes: Option<&HashSet<String>>,
    ) -> Result<String, ToolError> {
        let tool = self
            .tools
            .read()
            .unwrap()
            .get(name)
            .cloned()
            .ok_or_else(|| ToolError::Failed(format!("unknown tool: {name}")))?;
        Self::ensure_scope_allowed(tool.name(), tool.required_scopes(), allowed_scopes)?;
        tool.call(args, ctx).await
    }

    pub async fn call_output_scoped(
        &self,
        name: &str,
        args: Value,
        ctx: ToolContext,
        allowed_scopes: Option<&HashSet<String>>,
    ) -> Result<ToolOutput, ToolError> {
        let tool = self
            .tools
            .read()
            .unwrap()
            .get(name)
            .cloned()
            .ok_or_else(|| ToolError::Failed(format!("unknown tool: {name}")))?;
        Self::ensure_scope_allowed(tool.name(), tool.required_scopes(), allowed_scopes)?;
        tool.call_output(args, ctx).await
    }

    fn scope_allowed(required: &[String], allowed: Option<&HashSet<String>>) -> bool {
        allowed.is_none_or(|allowed| required.iter().all(|scope| allowed.contains(scope)))
    }

    fn ensure_scope_allowed(
        tool: &str,
        required: &[String],
        allowed: Option<&HashSet<String>>,
    ) -> Result<(), ToolError> {
        if Self::scope_allowed(required, allowed) {
            return Ok(());
        }
        Err(ToolError::PermissionDenied {
            tool: tool.to_owned(),
            required: required.to_vec(),
            allowed: allowed
                .map(|scopes| {
                    let mut scopes = scopes.iter().cloned().collect::<Vec<_>>();
                    scopes.sort();
                    scopes
                })
                .unwrap_or_default(),
        })
    }
}
