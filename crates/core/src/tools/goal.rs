//! Agent-scoped access to the daemon goal coordinator.
//!
//! Core owns the tool and its authorization envelope, but not the durable
//! coordinator service. A daemon attaches an [`AgentGoalBackend`] to each
//! agent. This keeps `firmius-core` independent of protocol and service.

use async_trait::async_trait;
use chrono::{DateTime, Utc};
use schemars::JsonSchema;
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use serde_json::Value;

use crate::goal::GoalBudget;
use crate::persona::{AGENT_MESSAGE_SCOPE, DELEGATION_SCOPE};
use crate::{ToolContext, ToolError, ToolRegistry, TypedTool};

pub const GOAL_READ_SCOPE: &str = "goal_read";
pub const GOAL_WRITE_SCOPE: &str = "goal_write";
pub const GOAL_CONTROL_SCOPE: &str = "goal_control";

/// Authenticated identity supplied to a goal backend. These fields are made
/// exclusively from `ToolContext`; goal tool arguments have no actor, owner,
/// controller, session, or sender fields.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AgentGoalIdentity {
    pub session_id: String,
    pub agent_id: String,
    /// The caller's current fenced goal, when one occupies its slot.
    pub active_goal_id: Option<String>,
    /// Immediate parent in the authenticated session hierarchy.
    pub parent_agent_id: Option<String>,
    /// Immediate children are the only other agents this caller may target.
    pub child_agent_ids: Vec<String>,
}

/// Protocol-independent command sent to the daemon-owned backend. Arbitrary
/// operation data remains JSON so the service can translate to its versioned
/// protocol without core depending on that crate.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AgentGoalRequest {
    pub identity: AgentGoalIdentity,
    pub operation: AgentGoalOperation,
    #[serde(default)]
    pub params: Value,
    /// Stable idempotency/correlation key. Defaults to the tool call id.
    pub request_id: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AgentGoalOperation {
    Inspect,
    List,
    Propose,
    CreateChild,
    Enqueue,
    RequestActivation,
    Yield,
    SubmitCandidate,
    Cancel,
    Message,
}

/// Three-state authority value used by child/self-goal policy overlays.
///
/// Omitted (or the default) inherits the parent/root ceiling; JSON `null`
/// explicitly clears the constraint; a value must only narrow inherited
/// authority. The overlay's field-level serde attributes preserve these three
/// states so the service can compare them against the durable parent policy.
/// Core cannot perform that comparison itself because the tool has no durable
/// coordinator state; it enforces shape and never accepts identity in args.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Inherited<T> {
    Inherited,
    Null,
    Value(T),
}

impl<T> Default for Inherited<T> {
    fn default() -> Self {
        Self::Inherited
    }
}

impl<T> Inherited<T> {
    fn is_inherited(&self) -> bool {
        matches!(self, Self::Inherited)
    }

    #[allow(dead_code)]
    fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }

    #[allow(dead_code)]
    fn as_explicit(&self) -> Option<Option<&T>> {
        match self {
            Self::Inherited => None,
            Self::Null => Some(None),
            Self::Value(value) => Some(Some(value)),
        }
    }
}

impl<T: Serialize> Serialize for Inherited<T> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Inherited | Self::Null => serializer.serialize_none(),
            Self::Value(value) => value.serialize(serializer),
        }
    }
}

impl<'de, T: Deserialize<'de>> Deserialize<'de> for Inherited<T> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        match Option::<T>::deserialize(deserializer)? {
            None => Ok(Self::Null),
            Some(value) => Ok(Self::Value(value)),
        }
    }
}

/// External-effect ceiling a child or self goal may inherit, clear, or narrow.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum AgentGoalExternalEffects {
    Deny,
    ReadOnly,
    Allow,
}

/// Authority a child or self goal may inherit, explicitly clear, or narrow.
/// Mirrors the versioned protocol overlays conceptually without importing
/// `firmius-protocol` into core.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
struct AgentGoalPolicyOverlay {
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    budget: Inherited<GoalBudget>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    deadline: Inherited<DateTime<Utc>>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    tool_scopes: Inherited<Vec<String>>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    max_delegation_depth: Inherited<u32>,
    #[serde(default, skip_serializing_if = "Inherited::is_inherited")]
    external_effects: Inherited<AgentGoalExternalEffects>,
}

impl AgentGoalPolicyOverlay {
    fn is_inherit_all(&self) -> bool {
        self.budget.is_inherited()
            && self.deadline.is_inherited()
            && self.tool_scopes.is_inherited()
            && self.max_delegation_depth.is_inherited()
            && self.external_effects.is_inherited()
    }
}

#[async_trait]
pub trait AgentGoalBackend: Send + Sync {
    /// Execute against the service's authoritative coordinator transaction.
    /// Implementations must serialize mutations through the same commit lock,
    /// persistence store, and occupancy arbiter used by external clients; a
    /// detached coordinator clone is not a valid implementation. It is also
    /// responsible for authorizing the requested goal against `identity`
    /// (self goal or a direct child controlled by the caller), validating
    /// run/generation fences, and enforcing inherited ancestry, depth,
    /// budget, capability, retry, and deduplication ceilings for child goals.
    async fn execute(&self, request: AgentGoalRequest) -> Result<Value, String>;
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, JsonSchema, Default)]
#[serde(rename_all = "snake_case")]
enum Mode {
    #[default]
    Inspect,
    List,
    Propose,
    CreateChild,
    Enqueue,
    /// Activation is always scheduler-mediated. The tool requests it; the
    /// backend/coordinator remains the exclusive slot arbiter.
    #[serde(alias = "activate")]
    RequestActivation,
    Yield,
    #[serde(alias = "candidate")]
    SubmitCandidate,
    Cancel,
    Message,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
struct GoalArgs {
    /// Operation. Default inspect. Inspect first to obtain current goal/run IDs and revisions; never invent them.
    #[serde(default)]
    mode: Mode,
    /// Goal being inspected or controlled. For yield/submit_candidate/create_child, use your active goal ID.
    #[serde(default)]
    goal_id: Option<String>,
    /// Existing immediate child agent to address; create_child assigns a goal to this child, it does not spawn one.
    #[serde(default)]
    child_agent_id: Option<String>,
    /// Objective for propose/create_child. State the observable end state.
    #[serde(default)]
    description: Option<String>,
    /// Observable acceptance conditions for the goal. Required and nonempty for propose.
    #[serde(default)]
    success_conditions: Vec<String>,
    /// Compare-and-swap revision from the latest coordinator inspection. Required by lifecycle mutations.
    #[serde(default)]
    expected_coordinator_revision: Option<u64>,
    /// Compare-and-swap revision of this goal from inspect. Refresh after a stale revision error.
    #[serde(default)]
    expected_goal_revision: Option<u64>,
    /// Current goal run identity from inspect or launch context. Not a task workflow run ID.
    #[serde(default)]
    run_id: Option<String>,
    /// Current activation generation from inspect or launch context; prevents stale attempts controlling a new run.
    #[serde(default)]
    generation: Option<u64>,
    /// For child creation: whether the parent releases its execution slot while the child works.
    #[serde(default)]
    parent_yields: Option<bool>,
    /// Goal IDs that must settle before the yielded goal can continue.
    #[serde(default)]
    wait_for: Vec<String>,
    #[serde(default)]
    dependency_policy: Option<String>,
    /// Whether restarting this work can safely repeat its effects. Do not assert this for non-idempotent external actions.
    #[serde(default)]
    retry_safe: Option<bool>,
    /// Maximum goal attempts, including retries.
    #[serde(default)]
    max_attempts: Option<u32>,
    /// Maximum worker steps allowed for this goal.
    #[serde(default)]
    max_steps: Option<u32>,
    #[serde(default)]
    max_cost: Option<u64>,
    #[serde(default)]
    capabilities: Vec<String>,
    #[serde(default)]
    dedupe_key: Option<String>,
    /// Concrete reason for yielding, cancellation, or a lifecycle change.
    #[serde(default)]
    reason: Option<String>,
    #[serde(default)]
    child_policy: Option<String>,
    /// Structured completion candidate; submit_candidate requests validation, it does not declare success.
    #[serde(default)]
    result: Option<Value>,
    /// Observed validation evidence supporting the completion candidate.
    #[serde(default)]
    evidence: Vec<Value>,
    #[serde(default)]
    message: Option<String>,
    #[serde(default)]
    thread_id: Option<String>,
    #[serde(default)]
    parent_goal_id: Option<String>,
    #[serde(default)]
    in_reply_to: Option<String>,
    #[serde(default)]
    message_kind: Option<String>,
    #[serde(default)]
    workflow_node_id: Option<String>,
    #[serde(default)]
    assignment_id: Option<String>,
    #[serde(default)]
    priority: Option<i32>,
    #[serde(default)]
    priority_class: Option<String>,
    #[serde(default)]
    limit: Option<u32>,
    #[serde(default)]
    cursor: Option<String>,
    /// Reserved for future policy. Goal-created agents inherit their parent's
    /// model; `null`/omitted and the string `inherited` are the only accepted
    /// values today.
    #[serde(default)]
    model: Option<String>,
    #[serde(
        default,
        skip_serializing_if = "AgentGoalPolicyOverlay::is_inherit_all"
    )]
    #[schemars(skip)]
    policy: AgentGoalPolicyOverlay,
    /// Stable request identity for retries of the same operation. Reuse after an uncertain response.
    #[serde(default)]
    request_id: Option<String>,
}

fn require_scopes(ctx: &ToolContext, required: &[&str]) -> Result<(), ToolError> {
    let Some(allowed) = &ctx.allowed_scopes else {
        return Ok(()); // backwards-compatible unrestricted legacy agents
    };
    if required.iter().all(|scope| allowed.contains(*scope)) {
        return Ok(());
    }
    let mut actual = allowed.iter().cloned().collect::<Vec<_>>();
    actual.sort();
    Err(ToolError::PermissionDenied {
        tool: "goal".into(),
        required: required.iter().map(|scope| (*scope).into()).collect(),
        allowed: actual,
    })
}

fn required_scopes(mode: Mode) -> &'static [&'static str] {
    match mode {
        Mode::Inspect | Mode::List => &[GOAL_READ_SCOPE],
        Mode::Propose => &[GOAL_WRITE_SCOPE],
        Mode::CreateChild => &[GOAL_WRITE_SCOPE, GOAL_CONTROL_SCOPE, DELEGATION_SCOPE],
        Mode::Message => &[GOAL_READ_SCOPE, AGENT_MESSAGE_SCOPE],
        Mode::Enqueue
        | Mode::RequestActivation
        | Mode::Yield
        | Mode::SubmitCandidate
        | Mode::Cancel => &[GOAL_CONTROL_SCOPE],
    }
}

fn require<'a>(value: &'a Option<String>, name: &str) -> Result<&'a str, ToolError> {
    value
        .as_deref()
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| {
            ToolError::InvalidArguments(format!("{} requires '{name}'", name.replace('_', " ")))
        })
}

fn validate_args(args: &GoalArgs, identity: &AgentGoalIdentity) -> Result<(), ToolError> {
    if args
        .model
        .as_deref()
        .is_some_and(|model| model != "inherited")
    {
        return Err(ToolError::InvalidArguments(
            "goal model must be null, omitted, or 'inherited'".into(),
        ));
    }
    if let Some(target) = args.child_agent_id.as_deref()
        && target != identity.agent_id
        && identity.parent_agent_id.as_deref() != Some(target)
        && !identity.child_agent_ids.iter().any(|id| id == target)
    {
        return Err(ToolError::PermissionDenied {
            tool: "goal".into(),
            required: vec!["target must be self, parent, or an immediate child".into()],
            allowed: identity.child_agent_ids.clone(),
        });
    }
    match args.mode {
        Mode::Inspect => {}
        Mode::List => {}
        Mode::Propose => {
            if args.child_agent_id.is_some() {
                return Err(ToolError::InvalidArguments(
                    "propose always creates a self goal; omit 'child_agent_id'".into(),
                ));
            }
            require(&args.description, "description")?;
            if args.success_conditions.is_empty() {
                return Err(ToolError::InvalidArguments(
                    "propose requires 'success_conditions'".into(),
                ));
            }
        }
        Mode::CreateChild => {
            let parent_goal = require(&args.goal_id, "goal_id")?;
            if identity.active_goal_id.as_deref() != Some(parent_goal) {
                return Err(ToolError::PermissionDenied {
                    tool: "goal".into(),
                    required: vec!["control of the caller's active parent goal".into()],
                    allowed: identity.active_goal_id.clone().into_iter().collect(),
                });
            }
            let child = require(&args.child_agent_id, "child_agent_id")?;
            if !identity.child_agent_ids.iter().any(|id| id == child) {
                return Err(ToolError::PermissionDenied {
                    tool: "goal".into(),
                    required: vec!["target must be an immediate child agent".into()],
                    allowed: identity.child_agent_ids.clone(),
                });
            }
            require(&args.description, "description")?;
            require(&args.run_id, "run_id")?;
            if args.generation.is_none()
                || args.expected_coordinator_revision.is_none()
                || args.expected_goal_revision.is_none()
            {
                return Err(ToolError::InvalidArguments(
                    "create_child requires generation and both expected revisions".into(),
                ));
            }
        }
        Mode::Yield | Mode::SubmitCandidate => {
            let goal = require(&args.goal_id, "goal_id")?;
            if identity.active_goal_id.as_deref() != Some(goal) {
                return Err(ToolError::PermissionDenied {
                    tool: "goal".into(),
                    required: vec!["the caller's active goal".into()],
                    allowed: identity.active_goal_id.clone().into_iter().collect(),
                });
            }
            require(&args.run_id, "run_id")?;
            if args.generation.is_none() {
                return Err(ToolError::InvalidArguments("generation is required".into()));
            }
        }
        Mode::Enqueue | Mode::RequestActivation | Mode::Cancel | Mode::Message => {
            require(&args.goal_id, "goal_id")?;
        }
    }
    if matches!(args.mode, Mode::Message) {
        require(&args.message, "message")?;
        require(&args.thread_id, "thread_id")?;
        require(&args.run_id, "run_id")?;
        if args.generation.is_none() {
            return Err(ToolError::InvalidArguments(
                "message requires 'generation'".into(),
            ));
        }
        let child = require(&args.child_agent_id, "child_agent_id")?;
        let related = identity.parent_agent_id.as_deref() == Some(child)
            || identity.child_agent_ids.iter().any(|id| id == child)
            || child == identity.agent_id;
        if !related {
            return Err(ToolError::PermissionDenied {
                tool: "goal".into(),
                required: vec!["recipient must be self, parent, or immediate child".into()],
                allowed: identity.child_agent_ids.clone(),
            });
        }
    }
    if !matches!(args.mode, Mode::Inspect | Mode::List)
        && args.expected_coordinator_revision.is_none()
    {
        return Err(ToolError::InvalidArguments(
            "mutation requires 'expected_coordinator_revision'".into(),
        ));
    }
    if matches!(
        args.mode,
        Mode::Enqueue
            | Mode::RequestActivation
            | Mode::Yield
            | Mode::SubmitCandidate
            | Mode::Cancel
            | Mode::Message
    ) && args.expected_goal_revision.is_none()
    {
        return Err(ToolError::InvalidArguments(
            "operation requires 'expected_goal_revision'".into(),
        ));
    }
    Ok(())
}

fn operation(mode: Mode) -> AgentGoalOperation {
    match mode {
        Mode::Inspect => AgentGoalOperation::Inspect,
        Mode::List => AgentGoalOperation::List,
        Mode::Propose => AgentGoalOperation::Propose,
        Mode::CreateChild => AgentGoalOperation::CreateChild,
        Mode::Enqueue => AgentGoalOperation::Enqueue,
        Mode::RequestActivation => AgentGoalOperation::RequestActivation,
        Mode::Yield => AgentGoalOperation::Yield,
        Mode::SubmitCandidate => AgentGoalOperation::SubmitCandidate,
        Mode::Cancel => AgentGoalOperation::Cancel,
        Mode::Message => AgentGoalOperation::Message,
    }
}

async fn goal(args: GoalArgs, ctx: ToolContext) -> Result<String, ToolError> {
    require_scopes(&ctx, required_scopes(args.mode))?;
    let session = ctx
        .session
        .as_ref()
        .ok_or_else(|| ToolError::Failed("goal requires an attached session".into()))?;
    if session.id != ctx.session_id
        || !session
            .hierarchy
            .read()
            .unwrap()
            .contains_key(&ctx.agent_id)
    {
        return Err(ToolError::Failed(
            "tool context identity is not in its session".into(),
        ));
    }
    let (parent_agent_id, mut child_agent_ids) = {
        let hierarchy = session.hierarchy.read().unwrap();
        let parent = hierarchy
            .get(&ctx.agent_id)
            .and_then(|node| node.parent_id.clone());
        let children = hierarchy
            .iter()
            .filter(|(_, node)| node.parent_id.as_deref() == Some(ctx.agent_id.as_str()))
            .map(|(id, _)| id.clone())
            .collect::<Vec<_>>();
        (parent, children)
    };
    child_agent_ids.sort();
    let caller = session
        .agent(&ctx.agent_id)
        .ok_or_else(|| ToolError::Failed("calling agent is not live".into()))?;
    let identity = AgentGoalIdentity {
        session_id: ctx.session_id.clone(),
        agent_id: ctx.agent_id.clone(),
        active_goal_id: caller.active_goal_id(),
        parent_agent_id,
        child_agent_ids,
    };
    validate_args(&args, &identity)?;
    let backend = caller
        .goal_backend()
        .ok_or_else(|| ToolError::Failed("goal backend is not attached".into()))?;
    let request_id = args
        .request_id
        .clone()
        .filter(|id| !id.trim().is_empty())
        .unwrap_or_else(|| ctx.tool_call_id.clone());
    let mut params = serde_json::to_value(&args)
        .map_err(|error| ToolError::Failed(format!("encode goal request: {error}")))?;
    if let Value::Object(map) = &mut params {
        map.remove("mode");
        map.remove("request_id");
        map.remove("model"); // inherited/null is normalized, never delegated
    }
    backend
        .execute(AgentGoalRequest {
            identity,
            operation: operation(args.mode),
            params,
            request_id,
        })
        .await
        .and_then(|value| serde_json::to_string_pretty(&value).map_err(|error| error.to_string()))
        .map_err(ToolError::Failed)
}

pub fn register_goal_tool(registry: &ToolRegistry) -> &ToolRegistry {
    registry.register(TypedTool::new(
        "goal",
        "Manage durable outcome activation and validation for your own goals or direct children. Use task/workflow to organize implementation inside a goal; delegate spawns workers. Start with inspect (default) to obtain current identities and revisions. Propose takes description and success_conditions. Submit_candidate submits observed results for runtime validation; it never self-grants success. Only use operations allowed by your scopes. Identity is derived from ToolContext. Modes: inspect, list, propose, create_child, enqueue, request_activation (alias activate), yield, submit_candidate (alias candidate), cancel, message. Goal-created agents inherit model selection; explicit models are rejected. Operations require goal_read, goal_write, goal_control, delegation, and/or agent_message scopes as appropriate.",
        |args: GoalArgs, ctx: ToolContext| Box::pin(goal(args, ctx)),
    ).with_visibility_scopes(&[GOAL_READ_SCOPE, GOAL_WRITE_SCOPE, GOAL_CONTROL_SCOPE]));
    registry
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::AgentConfig;
    use crate::providers::{Provider, ProviderError, ProviderEvent};
    use crate::{AgentState, LocalHost, Session};
    use futures::stream;
    use std::collections::HashSet;
    use std::sync::Arc;

    struct NoopProvider;
    #[async_trait]
    impl Provider for NoopProvider {
        fn id(&self) -> &str {
            "noop"
        }

        async fn stream(
            &self,
            _: crate::ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<'static, Result<ProviderEvent, ProviderError>>,
            ProviderError,
        > {
            Ok(Box::pin(stream::empty()))
        }
    }

    #[derive(Default)]
    struct Capture(std::sync::Mutex<Vec<AgentGoalRequest>>);
    #[async_trait]
    impl AgentGoalBackend for Capture {
        async fn execute(&self, request: AgentGoalRequest) -> Result<Value, String> {
            self.0.lock().unwrap().push(request);
            Ok(serde_json::json!({"ok": true}))
        }
    }

    fn context(session: &Arc<Session>, agent_id: &str, scopes: &[&str]) -> ToolContext {
        ToolContext {
            workdir: std::env::temp_dir(),
            cancellation: Default::default(),
            tool_call_id: "call-7".into(),
            agent_id: agent_id.into(),
            session_id: session.id.clone(),
            state: Arc::new(std::sync::RwLock::new(AgentState::default())),
            host: Arc::new(LocalHost::new()),
            session: Some(session.clone()),
            allowed_scopes: Some(
                scopes
                    .iter()
                    .map(|scope| (*scope).into())
                    .collect::<HashSet<_>>(),
            ),
        }
    }

    #[test]
    fn goal_is_hidden_when_no_operation_is_authorized() {
        let registry = ToolRegistry::default();
        register_goal_tool(&registry);
        let ordinary = ["fs_read".to_string(), "work_write".to_string()]
            .into_iter()
            .collect();
        assert!(registry.definitions_scoped(Some(&ordinary)).is_empty());
        for scope in [GOAL_READ_SCOPE, GOAL_WRITE_SCOPE, GOAL_CONTROL_SCOPE] {
            let allowed = [scope.to_string()].into_iter().collect();
            assert_eq!(registry.definitions_scoped(Some(&allowed)).len(), 1);
        }
    }

    #[tokio::test]
    async fn identity_is_derived_and_explicit_model_is_rejected() {
        let session = Session::new_handle();
        let tools = Arc::new(ToolRegistry::default());
        register_goal_tool(&tools);
        let agent = session.spawn_agent(
            Arc::new(NoopProvider),
            tools.clone(),
            AgentConfig::default(),
        );
        let backend = Arc::new(Capture::default());
        agent.attach_goal_backend(backend.clone());
        assert!(tools.definitions().iter().any(|tool| tool.name == "goal"));
        tools
            .call_scoped(
                "goal",
                serde_json::json!({
                    "mode":"propose", "description":"do it", "success_conditions":["done"],
                    "expected_coordinator_revision":0
                }),
                context(&session, &agent.id, &[GOAL_WRITE_SCOPE]),
                Some(&[GOAL_WRITE_SCOPE.into()].into_iter().collect()),
            )
            .await
            .unwrap();
        let request = backend.0.lock().unwrap()[0].clone();
        assert_eq!(request.identity.agent_id, agent.id);
        assert_eq!(request.identity.session_id, session.id);
        assert_eq!(request.request_id, "call-7");

        let error = tools
            .call(
                "goal",
                serde_json::json!({
                    "mode":"propose", "description":"x", "success_conditions":["x"],
                    "model":"other", "expected_coordinator_revision":0
                }),
                context(&session, &agent.id, &[GOAL_WRITE_SCOPE]),
            )
            .await
            .unwrap_err();
        assert!(error.to_string().contains("must be null"));
    }

    #[tokio::test]
    async fn child_goal_requires_direct_child_and_active_parent_fence() {
        let session = Session::new_handle();
        let tools = Arc::new(ToolRegistry::default());
        register_goal_tool(&tools);
        let parent = session.spawn_agent(
            Arc::new(NoopProvider),
            tools.clone(),
            AgentConfig::default(),
        );
        let child = session.spawn_subagent(
            &parent.id,
            None,
            Arc::new(NoopProvider),
            tools.clone(),
            AgentConfig::default(),
        );
        parent.attach_goal_backend(Arc::new(Capture::default()));
        parent.set_active_goal_id(Some("parent-goal".into()));
        let scopes = [GOAL_WRITE_SCOPE, GOAL_CONTROL_SCOPE, DELEGATION_SCOPE];
        tools
            .call(
                "goal",
                serde_json::json!({
                    "mode":"create_child", "goal_id":"parent-goal", "child_agent_id":child.id,
                    "description":"child work", "run_id":"run-1", "generation":2,
                    "expected_coordinator_revision":4, "expected_goal_revision":3
                }),
                context(&session, &parent.id, &scopes),
            )
            .await
            .unwrap();
        let error = tools
            .call(
                "goal",
                serde_json::json!({
                    "mode":"create_child", "goal_id":"other", "child_agent_id":child.id,
                    "description":"child work", "run_id":"run-1", "generation":2,
                    "expected_coordinator_revision":4, "expected_goal_revision":3
                }),
                context(&session, &parent.id, &scopes),
            )
            .await
            .unwrap_err();
        assert!(matches!(error, ToolError::PermissionDenied { .. }));
    }

    #[tokio::test]
    async fn control_and_message_scopes_fail_closed() {
        let session = Session::new_handle();
        let tools = Arc::new(ToolRegistry::default());
        register_goal_tool(&tools);
        let agent = session.spawn_agent(
            Arc::new(NoopProvider),
            tools.clone(),
            AgentConfig::default(),
        );
        agent.attach_goal_backend(Arc::new(Capture::default()));
        let error = tools
            .call(
                "goal",
                serde_json::json!({"mode":"cancel","goal_id":"g"}),
                context(&session, &agent.id, &[GOAL_READ_SCOPE]),
            )
            .await
            .unwrap_err();
        assert!(matches!(error, ToolError::PermissionDenied { .. }));
        let error = tools
            .call(
                "goal",
                serde_json::json!({
                    "mode":"message", "goal_id":"g", "child_agent_id":agent.id,
                    "message":"hi", "thread_id":"t"
                }),
                context(&session, &agent.id, &[GOAL_READ_SCOPE]),
            )
            .await
            .unwrap_err();
        assert!(matches!(error, ToolError::PermissionDenied { .. }));
    }

    #[tokio::test]
    async fn optional_backend_and_cas_revision_checks_fail_closed() {
        let session = Session::new_handle();
        let tools = Arc::new(ToolRegistry::default());
        register_goal_tool(&tools);
        let agent = session.spawn_agent(
            Arc::new(NoopProvider),
            tools.clone(),
            AgentConfig::default(),
        );
        let error = tools
            .call(
                "goal",
                serde_json::json!({"mode":"inspect"}),
                context(&session, &agent.id, &[GOAL_READ_SCOPE]),
            )
            .await
            .unwrap_err();
        assert!(error.to_string().contains("backend is not attached"));

        agent.attach_goal_backend(Arc::new(Capture::default()));
        let error = tools
            .call(
                "goal",
                serde_json::json!({
                    "mode":"cancel", "goal_id":"g", "expected_goal_revision":1
                }),
                context(&session, &agent.id, &[GOAL_CONTROL_SCOPE]),
            )
            .await
            .unwrap_err();
        assert!(error.to_string().contains("expected_coordinator_revision"));
    }

    #[tokio::test]
    async fn spoofed_identity_fields_are_rejected_before_backend_dispatch() {
        let session = Session::new_handle();
        let tools = Arc::new(ToolRegistry::default());
        register_goal_tool(&tools);
        let agent = session.spawn_agent(
            Arc::new(NoopProvider),
            tools.clone(),
            AgentConfig::default(),
        );
        let backend = Arc::new(Capture::default());
        agent.attach_goal_backend(backend.clone());
        let error = tools
            .call(
                "goal",
                serde_json::json!({
                    "mode":"propose", "description":"x", "success_conditions":["x"],
                    "expected_coordinator_revision":0, "actor":"admin", "owner":"victim"
                }),
                context(&session, &agent.id, &[GOAL_WRITE_SCOPE]),
            )
            .await
            .unwrap_err();
        assert!(matches!(error, ToolError::InvalidArguments(_)));
        assert!(backend.0.lock().unwrap().is_empty());
    }
}
