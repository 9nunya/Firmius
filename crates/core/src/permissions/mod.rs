//! Core permission policy types.
//!
//! This module deliberately contains no UI or daemon code.  Permission
//! decisions are made from a small, versioned data model so every caller can
//! apply the same fail-closed rules.

mod auto;
mod store;

use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::collections::HashMap;
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

// Call descriptors live separately from policy evaluation so the daemon can
// canonicalize a tool call before applying a profile. Re-exporting here keeps
// the policy namespace convenient for callers and avoids duplicate models.
pub use crate::tool_permissions::{
    Action, ActionDescriptor, ActionSeverity, PermissionActionDescriptor, ToolAction,
    ToolActionDescriptor, classify_tool_action, classify_tool_call, describe_tool_call,
    is_known_action_kind,
};

pub use auto::{
    AutoAdjudication, AutoAdjudicationContext, AutoAdjudicationDecision, AutoAdjudicationError,
    AutoAdjudicationEvent, AutoAdjudicationEventKind, AutoAdjudicator, AutoProviderResolver,
    AutoProviderSelection, ProviderAutoAdjudicator, deterministic_decision, validate_adjudication,
};
pub use store::{PermissionStore, PermissionStoreError, default_permission_store_path};

pub const PERMISSION_POLICY_VERSION: u32 = 2;
pub const PERMISSION_ACTION_DIGEST_VERSION: u32 = 1;

/// The policy selected for a session.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionMode {
    Default,
    Auto,
    Yolo,
    Custom(String),
}

/// A request presented to an interactive permission resolver.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PendingPermissionRequest {
    /// Filled by the daemon resolver before it crosses the process boundary.
    /// Core callers leave these as nil; resolvers must never trust a response
    /// that does not echo both values.
    #[serde(default)]
    pub request_id: Uuid,
    #[serde(default)]
    pub nonce: Uuid,
    pub session_id: String,
    pub agent_id: String,
    pub tool: String,
    pub descriptor: ToolActionDescriptor,
    pub action_digest: String,
    #[serde(default)]
    pub expected_revision: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionAuditKind {
    Requested,
    Allowed,
    Denied,
    Cancelled,
    TimedOut,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PermissionAuditEvent {
    pub session_id: String,
    pub agent_id: String,
    pub tool: String,
    pub action_digest: String,
    pub decision: PermissionDecision,
    pub kind: PermissionAuditKind,
}

#[async_trait::async_trait]
pub trait PermissionResolver: Send + Sync {
    async fn resolve(&self, request: PendingPermissionRequest) -> Option<PermissionDecision>;
}

/// Core authorization chokepoint used by tool dispatch.  It owns only
/// process-lifetime overlays; the durable policy remains in `PermissionStore`.
pub struct PermissionBroker {
    policy: RwLock<PermissionPolicy>,
    overlays: Mutex<HashMap<String, SessionPermissionOverlay>>,
    resolver: RwLock<Option<Arc<dyn PermissionResolver>>>,
    auto_adjudicator: RwLock<Option<Arc<dyn AutoAdjudicator>>>,
    auto_events: Mutex<Vec<AutoAdjudicationEvent>>,
    audit: Mutex<Vec<PermissionAuditEvent>>,
    timeout: Duration,
}

impl std::fmt::Debug for PermissionBroker {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PermissionBroker").finish_non_exhaustive()
    }
}

#[cfg(test)]
mod broker_tests {
    use super::*;
    use crate::tool_permissions::{ActionSeverity, ToolAction, ToolActionDescriptor};

    #[test]
    fn default_asks_for_unmatched_builtins_but_preserves_denies() {
        let mut policy = PermissionPolicy::default();
        for tool in ["bash", "task", "delegate", "message", "goal"] {
            assert_eq!(
                policy.evaluate(&PermissionAction::new(tool, Some("run"))),
                PermissionDecision::Ask
            );
        }
        assert_eq!(
            policy.evaluate(&PermissionAction::new("read", Some("read"))),
            PermissionDecision::Allow
        );
        assert_eq!(
            policy.evaluate(&PermissionAction::new("unknown", Some("run"))),
            PermissionDecision::Deny
        );
        let bash = PermissionAction::new("bash", Some("run"));
        policy
            .profiles
            .get_mut("default")
            .unwrap()
            .rules
            .push(PermissionRule {
                id: "deny-shell".into(),
                action_kind: Some("bash".into()),
                target: None,
                decision: PermissionDecision::Deny,
                priority: 0,
                enabled: true,
            });
        assert_eq!(policy.evaluate(&bash), PermissionDecision::Deny);
        policy.mode = PermissionMode::Custom("empty".into());
        policy
            .profiles
            .insert("empty".into(), PermissionProfile::default());
        assert_eq!(policy.evaluate(&bash), PermissionDecision::Deny);
        policy.mode = PermissionMode::Default;
        policy.profiles.remove("default");
        assert_eq!(policy.evaluate(&bash), PermissionDecision::Ask);
    }

    #[test]
    fn default_mode_with_no_saved_profiles_uses_builtin_rules() {
        let policy: PermissionPolicy = serde_json::from_value(serde_json::json!({
            "version": 2, "revision": 3, "mode": "default", "profiles": {},
            "yolo_confirmed": true, "startup_mode": null, "auto_model": null
        }))
        .unwrap();
        for tool in ["bash", "task", "edit"] {
            assert_eq!(
                policy.evaluate(&PermissionAction::new(tool, Some("run"))),
                PermissionDecision::Ask
            );
        }
        assert_eq!(
            policy.evaluate(&PermissionAction::new("read", Some("read"))),
            PermissionDecision::Allow
        );
        assert_eq!(
            policy.evaluate(&PermissionAction::new("unknown", Some("run"))),
            PermissionDecision::Deny
        );
    }

    #[test]
    fn confirmed_yolo_allows_every_classified_native_harness_tool() {
        let mut policy = PermissionPolicy::default();
        policy.mode = PermissionMode::Yolo;
        policy.yolo_confirmed = true;
        for (tool, args) in [
            (
                "workflow",
                serde_json::json!({"title":"triage","steps":[{"key":"investigate","persona":"general","prompt":"Find the cause."}]}),
            ),
            (
                "todo",
                serde_json::json!({"action":"begin","intent":"ship"}),
            ),
            (
                "memory",
                serde_json::json!({"mode_hint":"candidate","prompt":"Candidate: project uses SQLite."}),
            ),
        ] {
            let descriptor = crate::describe_tool_call(tool, &args, None);
            assert!(!descriptor.unknown, "{tool} must be classified");
            assert_eq!(
                policy.evaluate(&PermissionAction::new(
                    descriptor.tool,
                    Some(descriptor.operation),
                )),
                PermissionDecision::Allow,
                "confirmed YOLO must not deny native {tool}"
            );
        }
    }

    #[test]
    fn yolo_confirmation_allows_new_action_kinds_without_whitelist() {
        let mut policy = PermissionPolicy::default();
        policy.mode = PermissionMode::Yolo;
        let unknown = PermissionAction::new("future_tool", Some("new_operation"));
        assert_eq!(policy.evaluate(&unknown), PermissionDecision::Deny);
        policy.yolo_confirmed = true;
        assert_eq!(policy.evaluate(&unknown), PermissionDecision::Allow);
    }

    struct DelayedAllow;

    #[tokio::test]
    async fn exempt_tools_bypass_broker_without_audit_or_classifier() {
        for mode in [PermissionMode::Default, PermissionMode::Auto] {
            let mut policy = PermissionPolicy::default();
            policy.mode = mode;
            let broker = PermissionBroker::new(policy);
            for (tool, args) in [
                ("task", serde_json::json!({"mode":"create"})),
                ("delegate", serde_json::json!({"mode":"run"})),
                ("read", serde_json::json!({"path":"/outside"})),
                ("bash", serde_json::json!({"mode":"poll"})),
            ] {
                let descriptor = describe_tool_call(tool, &args, None);
                assert_eq!(
                    broker
                        .authorize("s", "a", tool, &descriptor, &CancellationToken::new())
                        .await,
                    PermissionDecision::Allow
                );
            }
            assert!(broker.audit_events().is_empty());
            assert!(broker.auto_events().is_empty());
            for (tool, args) in [
                ("bash", serde_json::json!({"command":"ls"})),
                ("bash", serde_json::json!({"mode":"input"})),
                ("bash", serde_json::json!({"mode":"kill"})),
                ("edit", serde_json::json!({})),
                ("mcp.unknown", serde_json::json!({})),
            ] {
                assert!(!broker.bypasses_permission_check(&describe_tool_call(tool, &args, None)));
            }
        }
    }

    #[async_trait::async_trait]
    impl PermissionResolver for DelayedAllow {
        async fn resolve(&self, _request: PendingPermissionRequest) -> Option<PermissionDecision> {
            tokio::time::sleep(Duration::from_millis(25)).await;
            Some(PermissionDecision::Allow)
        }
    }

    fn edit_descriptor() -> ToolActionDescriptor {
        ToolActionDescriptor {
            tool: "edit".into(),
            operation: "edit_file".into(),
            actions: vec![ToolAction {
                tool: "edit".into(),
                operation: "edit_file".into(),
                preview: "edit a file".into(),
                effects: vec!["write_file".into()],
                severity: ActionSeverity::Scoped,
                suggested_selectors: vec!["edit:*".into()],
            }],
            require_all_actions: true,
            unknown: false,
        }
    }

    #[tokio::test]
    async fn session_allow_cannot_override_durable_deny() {
        let mut policy = PermissionPolicy::default();
        policy.mode = PermissionMode::Custom("deny-all".into());
        let broker = PermissionBroker::new(policy);
        let descriptor = edit_descriptor();
        broker.grant(
            "s",
            SessionGrant {
                action_digest: descriptor_digest(&descriptor),
                decision: PermissionDecision::Allow,
                one_turn: false,
            },
        );
        assert_eq!(
            broker
                .authorize("s", "a", "edit", &descriptor, &CancellationToken::new(),)
                .await,
            PermissionDecision::Deny
        );
    }

    #[tokio::test]
    async fn policy_change_revokes_pending_approval() {
        let broker = Arc::new(PermissionBroker::new(PermissionPolicy::default()));
        broker.set_resolver(Some(Arc::new(DelayedAllow)));
        let pending_broker = broker.clone();
        let descriptor = edit_descriptor();
        let pending_descriptor = descriptor.clone();
        let task = tokio::spawn(async move {
            pending_broker
                .authorize(
                    "s",
                    "a",
                    "edit",
                    &pending_descriptor,
                    &CancellationToken::new(),
                )
                .await
        });
        tokio::time::sleep(Duration::from_millis(5)).await;
        let mut deny = PermissionPolicy::default();
        deny.revision = 1;
        deny.mode = PermissionMode::Custom("deny-all".into());
        broker.set_policy(deny);
        assert_eq!(task.await.unwrap(), PermissionDecision::Deny);
    }
}

impl PermissionBroker {
    pub fn new(policy: PermissionPolicy) -> Self {
        Self {
            policy: RwLock::new(policy),
            overlays: Mutex::new(HashMap::new()),
            resolver: RwLock::new(None),
            auto_adjudicator: RwLock::new(None),
            auto_events: Mutex::new(Vec::new()),
            audit: Mutex::new(Vec::new()),
            timeout: Duration::from_secs(60),
        }
    }
    pub fn from_store(store: &PermissionStore) -> Result<Self, PermissionStoreError> {
        Ok(Self::new(store.load()?))
    }
    pub fn set_resolver(&self, resolver: Option<Arc<dyn PermissionResolver>>) {
        *self.resolver.write().unwrap() = resolver;
    }
    pub fn set_auto_adjudicator(&self, adjudicator: Option<Arc<dyn AutoAdjudicator>>) {
        *self.auto_adjudicator.write().unwrap() = adjudicator;
    }
    pub fn policy(&self) -> PermissionPolicy {
        self.policy.read().unwrap().clone()
    }
    pub fn policy_revision(&self) -> u64 {
        self.policy.read().unwrap().revision
    }

    pub fn bypasses_permission_check(&self, descriptor: &ToolActionDescriptor) -> bool {
        matches!(
            self.policy.read().unwrap().mode,
            PermissionMode::Default | PermissionMode::Auto
        ) && descriptor.is_permission_exempt()
    }
    pub fn set_policy(&self, policy: PermissionPolicy) {
        *self.policy.write().unwrap() = policy;
    }
    pub fn audit_events(&self) -> Vec<PermissionAuditEvent> {
        self.audit.lock().unwrap().clone()
    }
    pub fn auto_events(&self) -> Vec<AutoAdjudicationEvent> {
        self.auto_events.lock().unwrap().clone()
    }
    pub fn grant(&self, session_id: impl Into<String>, grant: SessionGrant) {
        self.overlays
            .lock()
            .unwrap()
            .entry(session_id.into())
            .or_default()
            .grants
            .push(grant);
    }
    pub fn clear_session(&self, session_id: &str) {
        self.overlays.lock().unwrap().remove(session_id);
    }

    /// Authorize a fully-described call. Cancellation, resolver disconnect,
    /// timeout, unknown actions, and stale policy all fail closed.
    pub async fn authorize(
        &self,
        session_id: &str,
        agent_id: &str,
        tool: &str,
        descriptor: &ToolActionDescriptor,
        cancellation: &CancellationToken,
    ) -> PermissionDecision {
        self.authorize_with_context(
            session_id,
            agent_id,
            tool,
            descriptor,
            cancellation,
            None,
            None,
            None,
        )
        .await
    }

    /// Authorize with the calling agent's current request available to Auto.
    /// The adjudicator strips tools before sending this fork, preventing
    /// recursive permission calls while retaining relevant context.
    pub async fn authorize_with_context(
        &self,
        session_id: &str,
        agent_id: &str,
        tool: &str,
        descriptor: &ToolActionDescriptor,
        cancellation: &CancellationToken,
        contextual_request: Option<crate::types::ProviderRequest>,
        contextual_provider: Option<Arc<dyn crate::providers::Provider>>,
        proposed_tool_call: Option<serde_json::Value>,
    ) -> PermissionDecision {
        if self.bypasses_permission_check(descriptor) {
            return PermissionDecision::Allow;
        }
        let authorization_revision = self.policy_revision();
        let digest = descriptor_digest(descriptor);
        let action =
            PermissionAction::new(descriptor.tool.clone(), Some(descriptor.operation.clone()));
        let overlay_decision = {
            let mut overlays = self.overlays.lock().unwrap();
            overlays.get_mut(session_id).and_then(|overlay| {
                let pos = overlay
                    .grants
                    .iter()
                    .position(|g| g.action_digest == digest)?;
                let grant = overlay.grants[pos].clone();
                if grant.one_turn {
                    overlay.grants.remove(pos);
                }
                Some(grant.decision)
            })
        };
        let policy = self.policy.read().unwrap().clone();
        let policy_decision = policy.evaluate(&action);
        // Temporary authority may narrow or satisfy an Ask, but it can never
        // turn a durable policy Deny into Allow.
        let mut decision = match (policy_decision, overlay_decision) {
            (PermissionDecision::Deny, _) | (_, Some(PermissionDecision::Deny)) => {
                PermissionDecision::Deny
            }
            (_, Some(overlay)) => overlay,
            (policy, None) => policy,
        };
        if descriptor.unknown
            || descriptor
                .actions
                .iter()
                .any(|a| a.severity == ActionSeverity::Unknown)
        {
            decision = PermissionDecision::Deny;
        }
        let explicit_auto_deny = policy.profiles.get("auto").is_some_and(|profile| {
            profile
                .rules
                .iter()
                .any(|rule| rule.matches(&action) && rule.decision == PermissionDecision::Deny)
        });
        if matches!(policy.mode, PermissionMode::Auto)
            && overlay_decision.is_none()
            && !explicit_auto_deny
        {
            let deterministic = auto::deterministic_decision(descriptor);
            decision = deterministic;
            if deterministic == PermissionDecision::Deny
                && !descriptor.unknown
                && matches!(
                    descriptor.severity(),
                    ActionSeverity::Scoped | ActionSeverity::Risky
                )
            {
                decision = self
                    .adjudicate_auto(
                        session_id,
                        agent_id,
                        tool,
                        descriptor,
                        &digest,
                        cancellation,
                        policy.auto_model,
                        contextual_request,
                        contextual_provider,
                        proposed_tool_call,
                    )
                    .await;
            }
        }
        if decision == PermissionDecision::Ask {
            self.record(
                session_id,
                agent_id,
                tool,
                &digest,
                PermissionDecision::Ask,
                PermissionAuditKind::Requested,
            );
            let Some(resolver) = self.resolver.read().unwrap().clone() else {
                return self.finish(
                    session_id,
                    agent_id,
                    tool,
                    &digest,
                    PermissionDecision::Deny,
                    PermissionAuditKind::Denied,
                );
            };
            let request = PendingPermissionRequest {
                request_id: Uuid::nil(),
                nonce: Uuid::nil(),
                session_id: session_id.into(),
                agent_id: agent_id.into(),
                tool: tool.into(),
                descriptor: descriptor.clone(),
                action_digest: digest.clone(),
                expected_revision: self.policy.read().unwrap().revision,
            };
            decision = tokio::select! {
                _ = cancellation.cancelled() => PermissionDecision::Deny,
                result = tokio::time::timeout(self.timeout, resolver.resolve(request)) => result.ok().flatten().unwrap_or(PermissionDecision::Deny),
            };
            if decision != PermissionDecision::Allow {
                return self.finish(
                    session_id,
                    agent_id,
                    tool,
                    &digest,
                    PermissionDecision::Deny,
                    PermissionAuditKind::Denied,
                );
            }
        }
        // Async resolution is bound to the authority revision observed at
        // request creation. Any concurrent policy edit revokes the result.
        if self.policy_revision() != authorization_revision {
            decision = PermissionDecision::Deny;
        }
        self.finish(
            session_id,
            agent_id,
            tool,
            &digest,
            decision,
            if decision.is_allowed() {
                PermissionAuditKind::Allowed
            } else {
                PermissionAuditKind::Denied
            },
        )
    }

    async fn adjudicate_auto(
        &self,
        session_id: &str,
        agent_id: &str,
        tool: &str,
        descriptor: &ToolActionDescriptor,
        digest: &str,
        cancellation: &CancellationToken,
        preference: Option<AutoModelPreference>,
        contextual_request: Option<crate::types::ProviderRequest>,
        contextual_provider: Option<Arc<dyn crate::providers::Provider>>,
        proposed_tool_call: Option<serde_json::Value>,
    ) -> PermissionDecision {
        let adjudicator = self.auto_adjudicator.read().unwrap().clone().or_else(|| {
            contextual_provider.map(|provider| {
                Arc::new(auto::ProviderAutoAdjudicator::new(provider)) as Arc<dyn AutoAdjudicator>
            })
        });
        let Some(adjudicator) = adjudicator else {
            self.auto_event(
                AutoAdjudicationEventKind::Resolved,
                digest,
                PermissionDecision::Deny,
                "adjudicator unavailable",
            );
            return PermissionDecision::Deny;
        };
        self.auto_event(
            AutoAdjudicationEventKind::Started,
            digest,
            PermissionDecision::Ask,
            "contextual adjudication started",
        );
        let request = contextual_request.unwrap_or(crate::types::ProviderRequest {
            model: preference
                .as_ref()
                .and_then(|p| p.model.clone())
                .unwrap_or_default(),
            messages: vec![crate::types::Message::text(
                crate::types::MessageRole::User,
                "Authorize this proposed action conservatively.",
            )],
            tools: Vec::new(),
            temperature: None,
            max_tokens: Some(256),
            reasoning_effort: preference.as_ref().and_then(|p| p.effort.clone()),
            thinking_budget_tokens: None,
            session_id: Some(session_id.to_owned()),
            web_search: None,
        });
        self.auto_event(
            AutoAdjudicationEventKind::Progress,
            digest,
            PermissionDecision::Ask,
            "adjudicator evaluating action",
        );
        let result = adjudicator
            .adjudicate(
                auto::AutoAdjudicationContext {
                    descriptor: descriptor.clone(),
                    action_digest: digest.to_owned(),
                    proposed_tool_call: proposed_tool_call.unwrap_or_else(|| {
                        serde_json::json!({
                            "tool": tool,
                            "descriptor": descriptor,
                        })
                    }),
                    request,
                    preference,
                },
                cancellation,
            )
            .await;
        let (decision, reason) = match result {
            Ok(result)
                if auto::validate_adjudication(descriptor, result.clone())
                    == PermissionDecision::Allow =>
            {
                (PermissionDecision::Allow, result.reason)
            }
            Ok(result) => (PermissionDecision::Deny, result.reason),
            Err(error) => (PermissionDecision::Deny, error.to_string()),
        };
        self.auto_event(
            AutoAdjudicationEventKind::Resolved,
            digest,
            decision,
            &reason,
        );
        let _ = (agent_id, tool);
        decision
    }

    fn auto_event(
        &self,
        kind: AutoAdjudicationEventKind,
        digest: &str,
        decision: PermissionDecision,
        reason: &str,
    ) {
        self.auto_events
            .lock()
            .unwrap()
            .push(AutoAdjudicationEvent {
                kind,
                action_digest: digest.to_owned(),
                decision,
                reason: reason.to_owned(),
            });
    }

    fn record(
        &self,
        session: &str,
        agent: &str,
        tool: &str,
        digest: &str,
        decision: PermissionDecision,
        kind: PermissionAuditKind,
    ) {
        self.audit.lock().unwrap().push(PermissionAuditEvent {
            session_id: session.into(),
            agent_id: agent.into(),
            tool: tool.into(),
            action_digest: digest.into(),
            decision,
            kind,
        });
    }
    fn finish(
        &self,
        session: &str,
        agent: &str,
        tool: &str,
        digest: &str,
        decision: PermissionDecision,
        kind: PermissionAuditKind,
    ) -> PermissionDecision {
        self.record(session, agent, tool, digest, decision, kind);
        decision
    }
}

pub fn descriptor_digest(descriptor: &ToolActionDescriptor) -> String {
    let value = serde_json::to_vec(descriptor).expect("descriptor serializable");
    let mut h = Sha256::new();
    h.update(value);
    hex_digest(h.finalize().as_slice())
}

impl Default for PermissionMode {
    fn default() -> Self {
        Self::Default
    }
}

fn known_action_kind(kind: &str) -> bool {
    crate::tool_permissions::is_known_action_kind(kind)
}

/// A user's preferred adjudicator for Auto mode.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct AutoModelPreference {
    #[serde(default)]
    pub provider: Option<String>,
    #[serde(default)]
    pub model: Option<String>,
    #[serde(default)]
    pub effort: Option<String>,
}

/// The result of policy evaluation.  Ordering is intentionally deny-monotonic:
/// a deny can never be upgraded by a lower layer or a session grant.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PermissionDecision {
    Allow,
    Ask,
    Deny,
}

impl Default for PermissionDecision {
    fn default() -> Self {
        Self::Deny
    }
}

impl PermissionDecision {
    pub fn is_allowed(self) -> bool {
        matches!(self, Self::Allow)
    }
    pub fn is_denied(self) -> bool {
        matches!(self, Self::Deny)
    }
}

/// A normalized action being authorized.  Additional parameters are sorted by
/// serde's map representation before hashing, making the digest independent
/// of insertion order.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PermissionAction {
    pub kind: String,
    #[serde(default)]
    pub target: Option<String>,
    #[serde(default)]
    pub parameters: BTreeMap<String, Value>,
}

impl PermissionAction {
    pub fn new(kind: impl Into<String>, target: Option<impl Into<String>>) -> Self {
        Self {
            kind: kind.into(),
            target: target.map(Into::into),
            parameters: BTreeMap::new(),
        }
    }

    pub fn with_parameter(mut self, name: impl Into<String>, value: Value) -> Self {
        self.parameters.insert(name.into(), value);
        self
    }

    /// SHA-256 of the version marker and canonical JSON action representation.
    pub fn digest(&self) -> String {
        let canonical = serde_json::json!({
            "version": PERMISSION_ACTION_DIGEST_VERSION,
            "kind": self.kind,
            "target": self.target,
            "parameters": canonical_value(&Value::Object(self.parameters.clone().into_iter().collect())),
        });
        let mut hasher = Sha256::new();
        hasher.update(b"firmius-permission-action-v1\n");
        hasher.update(serde_json::to_vec(&canonical).expect("permission action is serializable"));
        hex_digest(hasher.finalize().as_slice())
    }

    pub fn digest_hex(&self) -> String {
        self.digest()
    }
}

fn hex_digest(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn canonical_value(value: &Value) -> Value {
    match value {
        Value::Object(map) => Value::Object(
            map.iter()
                .map(|(k, v)| (k.clone(), canonical_value(v)))
                .collect(),
        ),
        Value::Array(values) => Value::Array(values.iter().map(canonical_value).collect()),
        other => other.clone(),
    }
}

/// One editable rule.  A missing matcher is a wildcard.  `*` is supported in
/// matchers; unknown action kinds do not match a wildcard and are denied.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionRule {
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub action_kind: Option<String>,
    #[serde(default)]
    pub target: Option<String>,
    pub decision: PermissionDecision,
    #[serde(default)]
    pub priority: i32,
    #[serde(default = "default_true")]
    pub enabled: bool,
}

fn default_true() -> bool {
    true
}

pub type PolicyRule = PermissionRule;

impl PermissionRule {
    pub fn matches(&self, action: &PermissionAction) -> bool {
        self.enabled
            && self
                .action_kind
                .as_deref()
                .map_or(true, |p| wildcard_match(p, &action.kind))
            && self.target.as_deref().map_or(true, |p| {
                action
                    .target
                    .as_deref()
                    .is_some_and(|t| wildcard_match(p, t))
            })
    }
}

fn wildcard_match(pattern: &str, text: &str) -> bool {
    // Small glob matcher (only `*`) keeps policy evaluation deterministic and
    // avoids regex's differing Unicode/path semantics.
    let (mut pi, mut ti, mut star, mut mark) = (0usize, 0usize, None, 0usize);
    let p: Vec<char> = pattern.chars().collect();
    let t: Vec<char> = text.chars().collect();
    while ti < t.len() {
        if pi < p.len() && (p[pi] == t[ti]) {
            pi += 1;
            ti += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = Some(pi);
            pi += 1;
            mark = ti;
        } else if let Some(s) = star {
            pi = s + 1;
            mark += 1;
            ti = mark;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct PermissionProfile {
    #[serde(default)]
    pub rules: Vec<PermissionRule>,
}

impl PermissionProfile {
    pub fn evaluate(&self, action: &PermissionAction) -> PermissionDecision {
        let mut result = None;
        let mut rules: Vec<&PermissionRule> =
            self.rules.iter().filter(|r| r.matches(action)).collect();
        rules.sort_by(|a, b| b.priority.cmp(&a.priority).then_with(|| a.id.cmp(&b.id)));
        for rule in rules {
            // Deny is always retained, regardless of priority.  Among non-
            // denies, the deterministic priority order picks the first rule.
            if rule.decision == PermissionDecision::Deny {
                return PermissionDecision::Deny;
            }
            if result.is_none() {
                result = Some(rule.decision);
            }
        }
        result.unwrap_or(PermissionDecision::Deny)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionPolicy {
    #[serde(default = "default_policy_version")]
    pub version: u32,
    #[serde(default)]
    pub revision: u64,
    #[serde(default)]
    pub mode: PermissionMode,
    #[serde(default)]
    pub profiles: BTreeMap<String, PermissionProfile>,
    #[serde(default)]
    pub yolo_confirmed: bool,
    #[serde(default)]
    pub startup_mode: Option<PermissionMode>,
    #[serde(default)]
    pub auto_model: Option<AutoModelPreference>,
}

fn default_policy_version() -> u32 {
    PERMISSION_POLICY_VERSION
}

impl Default for PermissionPolicy {
    fn default() -> Self {
        let mut profiles = BTreeMap::new();
        profiles.insert(
            "default".into(),
            PermissionProfile {
                rules: vec![
                    PermissionRule {
                        id: "edit-asks".into(),
                        action_kind: Some("edit*".into()),
                        target: None,
                        decision: PermissionDecision::Ask,
                        priority: 100,
                        enabled: true,
                    },
                    PermissionRule {
                        id: "read-allow".into(),
                        action_kind: Some("read".into()),
                        target: None,
                        decision: PermissionDecision::Allow,
                        priority: 10,
                        enabled: true,
                    },
                    PermissionRule {
                        id: "list-allow".into(),
                        action_kind: Some("list".into()),
                        target: None,
                        decision: PermissionDecision::Allow,
                        priority: 10,
                        enabled: true,
                    },
                    PermissionRule {
                        id: "glob-allow".into(),
                        action_kind: Some("glob".into()),
                        target: None,
                        decision: PermissionDecision::Allow,
                        priority: 10,
                        enabled: true,
                    },
                    PermissionRule {
                        id: "grep-allow".into(),
                        action_kind: Some("grep".into()),
                        target: None,
                        decision: PermissionDecision::Allow,
                        priority: 10,
                        enabled: true,
                    },
                    PermissionRule {
                        id: "hosted-search-asks".into(),
                        action_kind: Some("network".into()),
                        target: None,
                        decision: PermissionDecision::Ask,
                        priority: 100,
                        enabled: true,
                    },
                ],
            },
        );
        Self {
            version: PERMISSION_POLICY_VERSION,
            revision: 0,
            mode: PermissionMode::Default,
            profiles,
            yolo_confirmed: false,
            startup_mode: None,
            auto_model: None,
        }
    }
}

impl PermissionPolicy {
    pub fn evaluate(&self, action: &PermissionAction) -> PermissionDecision {
        match &self.mode {
            PermissionMode::Yolo => {
                // YOLO is an explicit, persistent confirmation to bypass the
                // interactive policy.  Do not maintain a second whitelist of
                // action kinds here: newly added tools (and configured tools)
                // still pass through their inherent capability/security
                // boundaries, while an unconfirmed YOLO remains fail-closed.
                if self.yolo_confirmed {
                    PermissionDecision::Allow
                } else {
                    PermissionDecision::Deny
                }
            }
            PermissionMode::Default => self.profiles.get("default").map_or_else(
                || {
                    // A mode-only preference may have no saved profiles.
                    // Default still means the built-in interactive policy;
                    // missing custom profiles remain deny-all below.
                    Self::default().evaluate(action)
                },
                |p| {
                    // Default is interactive Ask mode. Older saved profiles
                    // only enumerate reads and edits, so unmatched built-in
                    // actions must reach the resolver rather than silently
                    // inheriting the custom-profile deny fallback.
                    if known_action_kind(&action.kind)
                        && !p.rules.iter().any(|rule| rule.matches(action))
                    {
                        PermissionDecision::Ask
                    } else {
                        p.evaluate(action)
                    }
                },
            ),
            PermissionMode::Auto => self
                .profiles
                .get("auto")
                .map_or(PermissionDecision::Deny, |p| p.evaluate(action)),
            PermissionMode::Custom(id) => self
                .profiles
                .get(id)
                .map_or(PermissionDecision::Deny, |p| p.evaluate(action)),
        }
    }
}

/// A temporary grant, never persisted with the policy.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionGrant {
    pub action_digest: String,
    pub decision: PermissionDecision,
    #[serde(default)]
    pub one_turn: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct SessionPermissionOverlay {
    #[serde(default)]
    pub grants: Vec<SessionGrant>,
}

impl SessionPermissionOverlay {
    pub fn evaluate(&self, action: &PermissionAction) -> Option<PermissionDecision> {
        self.grants
            .iter()
            .find(|g| g.action_digest == action.digest())
            .map(|g| g.decision)
    }
}