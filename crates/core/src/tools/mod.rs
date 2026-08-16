use async_trait::async_trait;
use schemars::JsonSchema;
use serde::de::DeserializeOwned;
use serde_json::Value;
use std::{collections::HashMap, path::PathBuf, sync::Arc};
use tokio_util::sync::CancellationToken;

use crate::agent::AgentState;
use crate::host::Host;
use crate::session::Session;

pub mod bash;
pub mod delegate;
pub mod edit;
pub mod flex;
pub mod grep;
pub mod glob;
pub mod list;
pub mod read;

pub use bash::register_bash_tool;
pub use delegate::register_delegate_tool;
pub use edit::register_edit_tool;
pub use glob::register_glob_tool;
pub use grep::register_grep_tool;
pub use list::register_list_tool;
pub use read::register_read_tool;

#[derive(Clone)]
pub struct ToolContext {
    pub workdir: PathBuf,
    pub cancellation: CancellationToken,
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
    pub session: Option<Arc<tokio::sync::Mutex<Session>>>,
}
impl ToolContext {
    pub fn cancelled(&self) -> bool {
        self.cancellation.is_cancelled()
    }
}

#[derive(Debug, thiserror::Error)]
pub enum ToolError {
    #[error("invalid arguments: {0}")]
    InvalidArguments(String),
    #[error("tool failed: {0}")]
    Failed(String),
    #[error("cancelled")]
    Cancelled,
}

#[async_trait]
pub trait Tool: Send + Sync {
    fn name(&self) -> &str;
    fn description(&self) -> &str;
    fn input_schema(&self) -> Value;
    async fn call(&self, args: Value, ctx: ToolContext) -> Result<String, ToolError>;
}

pub struct TypedTool<A, F> {
    pub name: String,
    pub description: String,
    pub schema: Value,
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
            handler,
            _args: std::marker::PhantomData,
        }
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
    async fn call(&self, args: Value, ctx: ToolContext) -> Result<String, ToolError> {
        let args =
            serde_json::from_value(args).map_err(|e| ToolError::InvalidArguments(e.to_string()))?;
        (self.handler)(args, ctx).await
    }
}

#[derive(Default)]
pub struct ToolRegistry {
    tools: HashMap<String, Arc<dyn Tool>>,
}
impl ToolRegistry {
    pub fn register<T: Tool + 'static>(&mut self, tool: T) {
        self.tools.insert(tool.name().to_owned(), Arc::new(tool));
    }
    pub fn definitions(&self) -> Vec<crate::ToolDefinition> {
        self.tools
            .values()
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
        self.tools
            .get(name)
            .ok_or_else(|| ToolError::Failed(format!("unknown tool: {name}")))?
            .call(args, ctx)
            .await
    }
}