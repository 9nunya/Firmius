//! Intent-oriented, model-facing access to cross-session memory.
//!
//! The durable store and policy live behind [`MemoryBackend`], normally in the
//! daemon.  In particular, the public tool never accepts identity, authority,
//! or a mutation capability as model arguments.  Identity and scopes come
//! from [`ToolContext`], while the opaque [`MemoryPurposeToken`] is installed
//! by trusted runtime code.

use std::collections::{BTreeSet, HashSet};
use std::fmt;
use std::path::PathBuf;
use std::sync::Arc;

use async_trait::async_trait;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use crate::memory::ContextPacket;
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

use super::delegate::run_memory_curator;

pub const MEMORY_READ_SCOPE: &str = "memory_read";
pub const MEMORY_WRITE_SCOPE: &str = "memory_write";

const DEFAULT_CONTEXT_LIMIT: usize = 8;
const DEFAULT_CONTEXT_BYTES: usize = 16 * 1024;

/// A trusted-runtime capability proving that a call has the dedicated memory
/// mutation purpose. It intentionally implements neither serde nor
/// `JsonSchema`, so it cannot enter model arguments or a wire payload by
/// accident. The daemon should retain and compare `capability_id()` when it
/// installs a backend and token in a registry.
#[derive(Clone, PartialEq, Eq)]
pub struct MemoryPurposeToken {
    capability_id: Arc<str>,
}

impl MemoryPurposeToken {
    /// Mint a process capability. This constructor is for daemon/runtime setup,
    /// never for request decoding. Public tools expose no path to call it.
    pub fn issue_for_runtime() -> Self {
        Self {
            capability_id: Arc::from(uuid::Uuid::now_v7().to_string()),
        }
    }

    pub fn capability_id(&self) -> &str {
        &self.capability_id
    }
}

impl fmt::Debug for MemoryPurposeToken {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("MemoryPurposeToken([sealed])")
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MemoryIntent {
    Retrieve,
    Propose,
    Promote,
    Remember,
    Correct,
    Forget,
    Inspect,
}

impl MemoryIntent {
    fn mutates(self) -> bool {
        matches!(
            self,
            Self::Propose | Self::Promote | Self::Remember | Self::Correct | Self::Forget
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum MemoryScopeHint {
    User,
    Project,
    Session,
}

/// Authenticated call identity assembled solely from `ToolContext`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthenticatedMemoryContext {
    pub session_id: String,
    pub agent_id: String,
    pub tool_call_id: String,
    pub workdir: PathBuf,
    pub allowed_scopes: Option<BTreeSet<String>>,
}

/// Protocol-independent request to the daemon-owned memory coordinator.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthenticatedMemoryRequest {
    pub context: AuthenticatedMemoryContext,
    pub intent: MemoryIntent,
    pub prompt: String,
    pub scope_hint: Option<MemoryScopeHint>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthenticatedMemoryContextRequest {
    pub context: AuthenticatedMemoryContext,
    pub query: String,
    pub scope_hint: Option<MemoryScopeHint>,
    pub limit: usize,
    pub max_bytes: usize,
}

/// Daemon-owned persistence/policy boundary. Implementations must validate a
/// mutation token against the capability installed by their runtime; a token
/// is additional authority, never a replacement for the authenticated scopes
/// present on the request.
#[async_trait]
pub trait MemoryBackend: Send + Sync {
    async fn execute(
        &self,
        request: AuthenticatedMemoryRequest,
        mutation_purpose: Option<MemoryPurposeToken>,
    ) -> Result<Value, String>;

    /// Build bounded, citation-bearing context for pre-turn injection.
    async fn context_packet(
        &self,
        request: AuthenticatedMemoryContextRequest,
    ) -> Result<ContextPacket, String>;
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum ModeHint {
    #[default]
    Auto,
    Retrieve,
    Candidate,
    Promote,
    Remember,
    Correct,
    Forget,
    Inspect,
}

#[derive(Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct MemoryArgs {
    /// State the information to retrieve or the memory change to make.
    prompt: String,
    /// Optional lifetime hint. The daemon resolves project identity from the
    /// authenticated working directory and session identity from ToolContext.
    #[serde(default)]
    scope_hint: Option<MemoryScopeHint>,
    /// Optional intent override. Auto is conservative and defaults to retrieve.
    #[serde(default)]
    mode_hint: ModeHint,
}

fn infer_intent(mode: ModeHint, prompt: &str) -> MemoryIntent {
    match mode {
        ModeHint::Retrieve => MemoryIntent::Retrieve,
        ModeHint::Candidate => MemoryIntent::Propose,
        ModeHint::Promote => MemoryIntent::Promote,
        ModeHint::Remember => MemoryIntent::Remember,
        ModeHint::Correct => MemoryIntent::Correct,
        ModeHint::Forget => MemoryIntent::Forget,
        ModeHint::Inspect => MemoryIntent::Inspect,
        ModeHint::Auto => {
            let normalized = prompt.trim_start().to_ascii_lowercase();
            let first = normalized
                .split(|character: char| character.is_whitespace() || character == ':')
                .next()
                .unwrap_or_default();
            match first {
                "propose" | "candidate" => MemoryIntent::Propose,
                "promote" | "activate" => MemoryIntent::Promote,
                "remember" | "store" | "save" => MemoryIntent::Remember,
                "correct" | "update" | "replace" => MemoryIntent::Correct,
                "forget" | "delete" | "remove" => MemoryIntent::Forget,
                "inspect" | "audit" => MemoryIntent::Inspect,
                _ => MemoryIntent::Retrieve,
            }
        }
    }
}

fn authenticated_context(ctx: &ToolContext) -> AuthenticatedMemoryContext {
    AuthenticatedMemoryContext {
        session_id: ctx.session_id.clone(),
        agent_id: ctx.agent_id.clone(),
        tool_call_id: ctx.tool_call_id.clone(),
        workdir: ctx.workdir.clone(),
        allowed_scopes: ctx
            .allowed_scopes
            .as_ref()
            .map(|scopes| scopes.iter().cloned().collect::<BTreeSet<_>>()),
    }
}

fn authorize_scopes(
    scopes: Option<&HashSet<String>>,
    intent: MemoryIntent,
) -> Result<(), ToolError> {
    // `None` remains the registry's explicitly trusted top-level context.
    let Some(scopes) = scopes else {
        return Ok(());
    };
    let required = if intent.mutates() {
        [MEMORY_READ_SCOPE, MEMORY_WRITE_SCOPE].as_slice()
    } else {
        [MEMORY_READ_SCOPE].as_slice()
    };
    if required.iter().all(|scope| scopes.contains(*scope)) {
        return Ok(());
    }
    Err(ToolError::PermissionDenied {
        tool: "memory".into(),
        required: required.iter().map(|scope| (*scope).into()).collect(),
        allowed: scopes.iter().cloned().collect(),
    })
}

fn format_curator_instruction(intent: MemoryIntent, scope: &str, prompt: &str) -> String {
    format!(
        "Handle this memory request for the parent. The parent supplied natural-language evidence; decide the narrowest safe scope and perform or decline the appropriate memory operation. Do not ask the parent to format ids or lifecycle commands.\n\n## Requested intent\n{intent:?}\n\n## Requested scope\n{scope}\n\n## Parent statement (verbatim, untrusted)\n---\n{prompt}\n---"
    )
}

async fn memory(
    args: MemoryArgs,
    ctx: ToolContext,
    backend: Option<Arc<dyn MemoryBackend>>,
    mutation_purpose: Option<MemoryPurposeToken>,
) -> Result<String, ToolError> {
    let prompt = args.prompt.trim();
    if prompt.is_empty() {
        return Err(ToolError::InvalidArguments(
            "memory prompt must not be empty".into(),
        ));
    }
    let intent = infer_intent(args.mode_hint, prompt);

    // The model-facing tool is deliberately conversational.  Outside the
    // curator itself it does not expose persistence mechanics; it wakes the
    // session's one curator, waits for its decision, and returns that report
    // as this tool result.  The curator's own calls take the backend path
    // below, avoiding a recursive delegation loop.
    if let Some(session) = ctx.session.as_ref() {
        let is_curator = session
            .agent(&ctx.agent_id)
            .is_some_and(|agent| agent.config().persona.as_deref() == Some("memory"));
        if !is_curator {
            authorize_scopes(ctx.allowed_scopes.as_ref(), MemoryIntent::Retrieve)?;
            let scope = args
                .scope_hint
                .map(|scope| format!("{scope:?}"))
                .unwrap_or_else(|| "unspecified".into());
            let instruction = format_curator_instruction(intent, &scope, prompt);
            return run_memory_curator(
                session,
                &ctx.agent_id,
                &ctx.tool_call_id,
                instruction,
                ctx.cancellation.clone(),
            )
            .await;
        }
    }
    authorize_scopes(ctx.allowed_scopes.as_ref(), intent)?;

    let mutation_purpose = if intent.mutates() {
        if mutation_purpose.is_none() {
            return Err(ToolError::PermissionDenied {
                tool: "memory".into(),
                required: vec!["runtime_memory_purpose".into()],
                allowed: Vec::new(),
            });
        }
        mutation_purpose
    } else {
        // Do not disclose or move mutation authority along read-only paths.
        None
    };

    let Some(backend) = backend else {
        if intent == MemoryIntent::Retrieve {
            return Ok(retrieval_unavailable("memory backend is unavailable"));
        }
        return Err(ToolError::Failed("memory backend is unavailable".into()));
    };
    let request = AuthenticatedMemoryRequest {
        context: authenticated_context(&ctx),
        intent,
        prompt: prompt.to_owned(),
        scope_hint: args.scope_hint,
    };
    match backend.execute(request, mutation_purpose).await {
        Ok(value) => serde_json::to_string_pretty(&value)
            .map_err(|error| ToolError::Failed(format!("encode memory response: {error}"))),
        Err(error) if intent == MemoryIntent::Retrieve => Ok(retrieval_unavailable(&error)),
        Err(error) => Err(ToolError::Failed(format!(
            "memory {:?} rejected: {error}",
            intent
        ))),
    }
}

fn retrieval_unavailable(reason: &str) -> String {
    json!({
        "status": "unavailable",
        "mode": "retrieve",
        "memories": [],
        "reason": reason,
    })
    .to_string()
}

/// Retrieve bounded memory context for injection before a model turn. Memory
/// is an optional aid: backend failures intentionally produce `None` rather
/// than making the turn fail.
pub async fn memory_context_packet(
    backend: Option<Arc<dyn MemoryBackend>>,
    ctx: &ToolContext,
    query: impl Into<String>,
    scope_hint: Option<MemoryScopeHint>,
) -> Option<ContextPacket> {
    let backend = backend?;
    if authorize_scopes(ctx.allowed_scopes.as_ref(), MemoryIntent::Retrieve).is_err() {
        return None;
    }
    backend
        .context_packet(AuthenticatedMemoryContextRequest {
            context: authenticated_context(ctx),
            query: query.into(),
            scope_hint,
            limit: DEFAULT_CONTEXT_LIMIT,
            max_bytes: DEFAULT_CONTEXT_BYTES,
        })
        .await
        .ok()
}

pub fn register_memory_tool(registry: &ToolRegistry) -> &ToolRegistry {
    register_memory_tool_with_backend(registry, None, None)
}

/// Register memory with a daemon backend and optional sealed mutation
/// capability. Omitting the capability deliberately produces a read-only tool.
pub fn register_memory_tool_with_backend(
    registry: &ToolRegistry,
    backend: Option<Arc<dyn MemoryBackend>>,
    mutation_purpose: Option<MemoryPurposeToken>,
) -> &ToolRegistry {
    registry.register(
        TypedTool::new(
            "memory",
            "Use cross-session memory by intent. Provide one prompt and optional scope_hint (user, project, session) and mode_hint (auto, retrieve, candidate, promote, remember, correct, forget, inspect). Candidate records are private, audited proposals and are not retrieved until explicitly promoted. Auto defaults conservatively to retrieval unless the prompt starts with an explicit mutation verb. Retrieval failures are non-fatal. Mutations require both memory_read and memory_write scopes plus a sealed runtime capability; identities and capabilities are never model arguments. Store credentials only when the user explicitly asks to remember them.",
            move |args: MemoryArgs, ctx: ToolContext| {
                let backend = backend.clone();
                let mutation_purpose = mutation_purpose.clone();
                Box::pin(async move { memory(args, ctx, backend, mutation_purpose).await })
            },
        )
        .with_visibility_scopes(&[MEMORY_READ_SCOPE, MEMORY_WRITE_SCOPE]),
    );
    registry
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::AgentState;
    use crate::host::LocalHost;
    use std::sync::Mutex;

    #[derive(Default)]
    struct CaptureBackend {
        calls: Mutex<Vec<(AuthenticatedMemoryRequest, bool)>>,
        fail: bool,
    }

    #[async_trait]
    impl MemoryBackend for CaptureBackend {
        async fn execute(
            &self,
            request: AuthenticatedMemoryRequest,
            purpose: Option<MemoryPurposeToken>,
        ) -> Result<Value, String> {
            self.calls
                .lock()
                .unwrap()
                .push((request, purpose.is_some()));
            if self.fail {
                Err("store offline".into())
            } else {
                Ok(json!({"status": "ok"}))
            }
        }

        async fn context_packet(
            &self,
            _request: AuthenticatedMemoryContextRequest,
        ) -> Result<ContextPacket, String> {
            Err("not used".into())
        }
    }

    fn context(scopes: &[&str]) -> ToolContext {
        ToolContext {
            workdir: PathBuf::from("/authenticated/workspace"),
            cancellation: Default::default(),
            tool_call_id: "call-7".into(),
            agent_id: "agent-authenticated".into(),
            session_id: "session-authenticated".into(),
            state: Arc::new(std::sync::RwLock::new(AgentState::new(
                "agent-authenticated",
            ))),
            host: Arc::new(LocalHost::new()),
            session: None,
            allowed_scopes: Some(scopes.iter().map(|scope| (*scope).into()).collect()),
        }
    }

    #[test]
    fn public_schema_has_only_intent_oriented_arguments() {
        let registry = ToolRegistry::default();
        register_memory_tool(&registry);
        let definition = registry
            .definitions()
            .into_iter()
            .find(|definition| definition.name == "memory")
            .expect("memory tool registered");
        let properties = definition.input_schema["properties"]
            .as_object()
            .expect("object properties");
        assert_eq!(properties.len(), 3);
        assert!(properties.contains_key("prompt"));
        assert!(properties.contains_key("scope_hint"));
        assert!(properties.contains_key("mode_hint"));
        for forbidden in ["agent_id", "session_id", "purpose", "token", "scopes"] {
            assert!(!properties.contains_key(forbidden));
        }
    }

    #[test]
    fn mutation_requires_both_sealed_scopes() {
        let read = HashSet::from([MEMORY_READ_SCOPE.to_owned()]);
        assert!(authorize_scopes(Some(&read), MemoryIntent::Retrieve).is_ok());
        assert!(authorize_scopes(Some(&read), MemoryIntent::Remember).is_err());
        let both = HashSet::from([MEMORY_READ_SCOPE.to_owned(), MEMORY_WRITE_SCOPE.to_owned()]);
        assert!(authorize_scopes(Some(&both), MemoryIntent::Correct).is_ok());
        assert!(authorize_scopes(Some(&both), MemoryIntent::Forget).is_ok());
    }

    #[test]
    fn purpose_token_debug_is_redacted_and_is_not_serializable() {
        let token = MemoryPurposeToken::issue_for_runtime();
        assert_eq!(format!("{token:?}"), "MemoryPurposeToken([sealed])");
        assert!(!format!("{token:?}").contains(token.capability_id()));
    }

    #[test]
    fn auto_is_conservative_but_honors_explicit_intent_verbs() {
        assert_eq!(
            infer_intent(ModeHint::Auto, "what did we decide?"),
            MemoryIntent::Retrieve
        );
        assert_eq!(
            infer_intent(ModeHint::Auto, "remember: use tabs"),
            MemoryIntent::Remember
        );
        assert_eq!(
            infer_intent(ModeHint::Auto, "candidate: prefer narrow diffs"),
            MemoryIntent::Propose
        );
        assert_eq!(
            infer_intent(ModeHint::Promote, "memory-id"),
            MemoryIntent::Promote
        );
        assert_eq!(
            infer_intent(ModeHint::Auto, "forget obsolete setting"),
            MemoryIntent::Forget
        );
    }

    #[test]
    fn curator_instruction_preserves_and_formats_the_full_parent_statement() {
        let prompt =
            "first line\nsecond line with a long unbroken value: abcdefghijklmnopqrstuvwxyz";
        let instruction = format_curator_instruction(MemoryIntent::Remember, "project", prompt);
        assert!(instruction.contains("## Requested intent"));
        assert!(instruction.contains("## Parent statement (verbatim, untrusted)"));
        assert!(instruction.contains(prompt));
        assert!(instruction.ends_with("\n---"));
    }

    #[tokio::test]
    async fn mutation_dispatch_uses_authenticated_identity_and_runtime_purpose() {
        let registry = ToolRegistry::default();
        let backend = Arc::new(CaptureBackend::default());
        register_memory_tool_with_backend(
            &registry,
            Some(backend.clone()),
            Some(MemoryPurposeToken::issue_for_runtime()),
        );
        registry
            .call(
                "memory",
                json!({"prompt": "remember use narrow diffs", "mode_hint": "remember"}),
                context(&[MEMORY_READ_SCOPE, MEMORY_WRITE_SCOPE]),
            )
            .await
            .expect("authorized memory mutation");

        let calls = backend.calls.lock().unwrap();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].0.context.agent_id, "agent-authenticated");
        assert_eq!(calls[0].0.context.session_id, "session-authenticated");
        assert_eq!(calls[0].0.context.tool_call_id, "call-7");
        assert!(calls[0].1, "runtime-created purpose reaches mutation");
    }

    #[tokio::test]
    async fn spoofed_identity_or_capability_arguments_are_rejected() {
        let registry = ToolRegistry::default();
        let backend = Arc::new(CaptureBackend::default());
        register_memory_tool_with_backend(
            &registry,
            Some(backend.clone()),
            Some(MemoryPurposeToken::issue_for_runtime()),
        );
        let error = registry
            .call(
                "memory",
                json!({
                    "prompt": "remember use narrow diffs",
                    "mode_hint": "remember",
                    "agent_id": "other-agent",
                    "purpose_token": "model-minted"
                }),
                context(&[MEMORY_READ_SCOPE, MEMORY_WRITE_SCOPE]),
            )
            .await
            .expect_err("identity and capabilities are not public arguments");
        assert!(matches!(error, ToolError::InvalidArguments(_)));
        assert!(backend.calls.lock().unwrap().is_empty());
    }

    #[tokio::test]
    async fn mutation_without_runtime_purpose_fails_before_backend_dispatch() {
        let registry = ToolRegistry::default();
        let backend = Arc::new(CaptureBackend::default());
        register_memory_tool_with_backend(&registry, Some(backend.clone()), None);
        let error = registry
            .call(
                "memory",
                json!({"prompt": "remember use narrow diffs", "mode_hint": "remember"}),
                context(&[MEMORY_READ_SCOPE, MEMORY_WRITE_SCOPE]),
            )
            .await
            .expect_err("runtime purpose is mandatory");
        assert!(matches!(error, ToolError::PermissionDenied { .. }));
        assert!(backend.calls.lock().unwrap().is_empty());
    }

    #[tokio::test]
    async fn rejected_mutation_is_error_but_retrieval_is_fail_soft() {
        let backend = Arc::new(CaptureBackend {
            fail: true,
            ..Default::default()
        });
        let registry = ToolRegistry::default();
        register_memory_tool_with_backend(
            &registry,
            Some(backend.clone()),
            Some(MemoryPurposeToken::issue_for_runtime()),
        );
        let retrieval = registry
            .call(
                "memory",
                json!({"prompt": "what did we decide?", "mode_hint": "retrieve"}),
                context(&[MEMORY_READ_SCOPE]),
            )
            .await
            .expect("retrieval failure is soft");
        assert!(retrieval.contains("unavailable"));
        assert!(!backend.calls.lock().unwrap()[0].1, "read gets no purpose");

        let mutation = registry
            .call(
                "memory",
                json!({"prompt": "remember use narrow diffs", "mode_hint": "remember"}),
                context(&[MEMORY_READ_SCOPE, MEMORY_WRITE_SCOPE]),
            )
            .await
            .expect_err("mutation rejection must remain explicit");
        assert!(mutation.to_string().contains("rejected"));
    }
}
