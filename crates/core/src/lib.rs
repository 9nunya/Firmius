pub mod agent;
pub mod artifact;
pub mod compaction;
pub mod compaction_job;
pub mod compaction_selection;
pub mod config;
pub mod context_budget;
pub mod coordinator;
pub mod edit_coordination;
pub mod goal;
pub mod host;
pub mod kinds;
pub mod mcp;
pub mod memory;
pub mod partial_json;
pub mod permissions;
pub mod persistence;
pub mod persona;
pub mod project;
pub mod prompts;
pub mod providers;
pub mod quota;
pub mod retry;
pub mod session;
pub mod ssh;
pub mod todo;
pub mod tool_permissions;
pub mod tools;
pub mod types;
pub mod user_settings;
pub mod wizard;
pub mod work;
pub mod workspace;

pub use agent::{
    Agent, AgentConfig, AgentError, AgentEvent, AgentState, CompactionState, DefaultStopPolicy,
    PersonaRuntimeContext, PersonaUse, SYSTEM_MESSAGE_MARKER, StopPolicy, ToolRuntimeResource,
    mark_system_message,
};
pub use artifact::{
    ARTIFACT_SCHEME, Artifact, ArtifactError, ArtifactSource, SessionArtifacts, is_artifact_path,
    normalize_artifact_dir, normalize_artifact_path,
};
pub use config::{
    BackoffConfig, BackoffStrategy, ConfigError, FailureClasses, FirmiusConfig, GeneralSettings,
    RetryConfig, RetryOverride, RetrySettings, default_config_path,
};
pub use coordinator::{
    ActivateSpec, AgentGoalSlot, AgentOccupancy, AgentRef, CancelSpec, CandidateSpec,
    ChildCascadePolicy, CompleteSpec, CompletionOutcome, CoordinatorError, DEFAULT_SLOT_TTL_SECS,
    DependencyKind, DependencyState, EnqueueResult, EnqueueSpec, ExternalClaim, GoalAssignment,
    GoalAssignmentId, GoalCoordinator, GoalDependency, GoalDependencyId, GoalQueueEntry,
    GoalQueueEntryId, GoalRun, GoalRunId, GoalRunState, GoalTarget, MAX_GOAL_DEPTH,
    MAX_LIVE_DESCENDANTS, OccupancyKind, OutboxEntry, OutboxId, OutboxKind, OutboxState,
    PRIORITY_AGING_MINUTES, PriorityClass, ReconcileReport, WaitReason, YieldSpec,
};
pub use edit_coordination::{
    AppliedEdit, EditAttempt, EditAttemptGuard, EditAttemptId, EditAuthority, EditConflict,
    EditCoordinationError, EditLease, InProcessEditAuthority,
};
pub use goal::{
    AgentCheck, ArtifactCheck, ArtifactCheckKind, CheckEvaluation, CheckState, CheckVerification,
    CommandCheck, CompositeCheck, CompositeOperator, EventCheck, Goal, GoalActor, GoalApproval,
    GoalBudget, GoalCheck, GoalCheckKind, GoalError, GoalEvent, GoalId, GoalLinks, GoalOwner,
    GoalProvenance, GoalSource, GoalStatus, GoalTransition, ReviewAttestation,
    ReviewEvidenceCapsule, parse_command_line,
};
pub use host::{
    ExitStatus, Host, HostError, LocalHost, OnOrphan, ProcChunk, ProcId, ProcInfo, ProcSpec,
    ProcStatus, PtySize, RemoteHost,
};
pub use kinds::cline_pass::fetch_live_models as fetch_cline_pass_live_models;
pub use kinds::{
    AccountKind, AlibabaTokenPlanKind, AnthropicSubscriptionKind, ApiKeyKind, ClinePassKind,
    CodexKind, GenericApiKeyWizard, GrokBuildKind, OpencodeGoKind,
};
pub use mcp::{
    McpError, McpManager, McpServerConfig, McpServerStatus, McpSettings, McpToolSpec,
    register_tool_specs, unregister_tool_specs,
};
pub use permissions::{
    AutoAdjudication, AutoAdjudicationContext, AutoAdjudicationDecision, AutoAdjudicationError,
    AutoAdjudicationEvent, AutoAdjudicationEventKind, AutoAdjudicator, AutoModelPreference,
    AutoProviderResolver, AutoProviderSelection, PendingPermissionRequest, PermissionAction,
    PermissionAuditEvent, PermissionAuditKind, PermissionBroker, PermissionDecision,
    PermissionMode, PermissionPolicy, PermissionProfile, PermissionResolver, PermissionRule,
    PermissionStore, PermissionStoreError, PolicyRule, ProviderAutoAdjudicator, SessionGrant,
    SessionPermissionOverlay, default_permission_store_path, deterministic_decision,
    validate_adjudication,
};
pub use persistence::{
    AccountRecord, AccountSummary, AgentNodeRecord, AgentRecord, AuthStore, MailboxDeliveryRecord,
    MailboxDeliveryState, ProviderAuth, SessionMailboxState, SessionPersistenceCoordinator,
    SessionRecord, SessionSummary, WorkStateRecord, data_dir, list_accounts, list_sessions,
    list_sessions_for_workdir, load_account, load_auth, load_session_record, save_account,
    save_auth, save_session_record, session_matches_workdir, session_to_markdown,
};
pub use persona::{
    AGENT_MESSAGE_SCOPE, DELEGATION_SCOPE, FS_READ_SCOPE, FS_WRITE_SCOPE, PROCESSES_SCOPE, Persona,
    PersonaDiagnostic, PersonaError, PersonaManager, default_personas_dir,
};
pub use providers::{
    AnthropicProvider, GrokProvider, OpenAiProvider, Provider, ProviderError, ProviderEvent,
    ProviderSchema, manager::ProviderManager, schema::ApiType,
};
pub use providers::{StaticToken, TokenSupplier};
pub use quota::{
    QuotaAuth, QuotaCapability, QuotaDescriptor, QuotaError, QuotaMeter, QuotaSnapshot, QuotaSource,
};
pub use retry::{ExhaustionReason, FailureClass, RetryController, RetryDecision, classify};
pub use session::{
    AgentNode, Agents, DelegateStatus, SESSION_EVENT_CAPACITY, SendContext, SendOutcome, Session,
    SessionEvent, SessionEventPayload, SessionHandle,
};
pub use ssh::{SshHostEntry, discover as discover_ssh_hosts};
pub use todo::{
    CompletionAction, CompletionEvaluation, EvidenceAttachment, EvidenceDeficit, EvidenceJournal,
    EvidenceReceipt, EvidenceReceiptId, NewTodoItem, PersistedTodoState, TODO_SCHEMA_VERSION,
    TodoCompletionReceipt, TodoCycle, TodoCycleId, TodoCycleProjection, TodoCycleStatus,
    TodoEnvelope, TodoError, TodoIntent, TodoItem, TodoItemId, TodoItemPatch, TodoItemProjection,
    TodoItemStatus, TodoLedger, TodoOutcome, TodoOutcomeKind, TodoProjection,
};
pub use tool_permissions::{
    Action, ActionDescriptor, ActionSeverity, PermissionActionDescriptor, ToolAction,
    ToolActionDescriptor, classify_tool_action, classify_tool_call, describe_tool_call,
    is_known_action_kind,
};
pub use tools::{
    AgentGoalBackend, AgentGoalIdentity, AgentGoalOperation, AgentGoalRequest,
    AuthenticatedMemoryContext, AuthenticatedMemoryContextRequest, AuthenticatedMemoryRequest,
    ConflictMessageBackend, ConflictMessageRequest, GOAL_CONTROL_SCOPE, GOAL_READ_SCOPE,
    GOAL_WRITE_SCOPE, MEMORY_READ_SCOPE, MEMORY_WRITE_SCOPE, MemoryBackend, MemoryIntent,
    MemoryPurposeToken, MemoryScopeHint, TODO_OBSERVE_SCOPE, TODO_READ_SCOPE, TODO_WRITE_SCOPE,
    Tool, ToolContext, ToolError, ToolOutput, ToolRegistry, TypedTool, WORK_READ_SCOPE,
    WORK_WRITE_SCOPE, register_bash_tool, register_delegate_tool, register_edit_tool,
    register_glob_tool, register_goal_tool, register_grep_tool, register_list_tool,
    register_memory_tool, register_memory_tool_with_backend, register_message_tool,
    register_message_tool_with_conflicts, register_read_tool, register_task_tool,
    register_todo_tool,
};
pub use types::*;
pub use user_settings::{
    ONBOARDING_VERSION, OnboardingState, PROMPT_HISTORY_CAP, PreferredModel, SavedSshHost,
    USER_SETTINGS_VERSION, UserSettings, UserSettingsError, default_user_settings_path,
};
pub use wizard::{Outcome, SelectOption, SetupWizard, Step, WizardError, match_select, run_wizard};
pub use work::*;
pub use workspace::{
    LocalWorkspace, RemoteWorkspace, Workspace, WorkspaceError, WorkspaceIdentity,
    WorkspaceLeaseError, WorkspaceWriterLease, normalize_write_path,
};
