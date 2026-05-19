#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <utility>

namespace firmius::core {

/**
 * Argument type for workflow arguments
 */
enum class WorkflowArgType {
  String,
  Number,
  Filepath,
  AgentId,
  ThreadId
};

/**
 * Represents a workflow argument definition from YAML frontmatter
 */
struct WorkflowArg {
  std::string name;         // Argument name for autocomplete
  WorkflowArgType type;     // Type of the argument
  std::string description;  // Description for autocomplete help
  bool optional = false;    // Whether the argument is optional
};

// ─── Triggers ───────────────────────────────────────────────────────────────
// A workflow with `kind == Manual` is what the user calls "a workflow".
// A workflow with `kind == OnEvent` is what users call "a hook" — same data
// model, different invocation path. Both go through the same loader and
// registry. This is the "unified workflow + hook" surface.

/**
 * Event types the dispatcher fires. Adding a new event = adding one enumerator
 * here + one fire-site in the agent loop. Events that allow `block: true` in
 * their hook config can short-circuit the originating action.
 */
enum class WorkflowEventKind {
  Unknown,
  PreToolUse,        // before any tool call. Blockable.
  PostToolUse,       // after a tool call completes.
  UserMessage,       // after user submits a message. Blockable.
  AgentStop,         // when an agent ends a turn or finalises.
  ThreadStart,       // first turn of a fresh thread. Blockable.
  ThreadResume,      // when a thread resumes. Blockable.
  ModeEntered,       // mode_switch produced a new active mode.
  ModeExited,        // previous mode lost activation.
  SubagentReturn,    // a delegated branch returned to its parent.
  WorkflowComplete,  // a workflow emitted its return trophy.
};

WorkflowEventKind workflowEventKindFromString(const std::string &s);
std::string workflowEventKindToString(WorkflowEventKind kind);
bool workflowEventIsBlockable(WorkflowEventKind kind);

/**
 * A predicate restricting which event instances fire this workflow. Empty
 * fields mean "any". Match keys are interpreted by the dispatcher per event
 * kind (e.g. `tool: edit`, `success: true`, `persona: forge`).
 */
struct WorkflowMatch {

  // Presence checks keyed by match key. When true, the key must resolve to a
  // value at dispatch time. When false, the key must be absent.
  std::map<std::string, bool> present;
  std::map<std::string, std::string> equals;
};

/**
 * Trigger spec attached to a workflow. Manual triggers are the legacy form
 * (slash command, explicit invocation). Event triggers turn the workflow
 * into a hook.
 */
struct WorkflowTrigger {
  enum class Kind { Manual, OnEvent };
  Kind kind = Kind::Manual;
  WorkflowEventKind event = WorkflowEventKind::Unknown;
  WorkflowMatch match;
  bool block = false;  // honoured only on blockable events
};

// ─── Action ─────────────────────────────────────────────────────────────────
// The action describes WHAT the workflow does when fired. Hooks lean on
// `Shell` (lint-after-edit, run tests, notify); workflows lean on `Prompt`
// (the legacy macro body sent to the agent); composability uses `Workflow`
// (call another workflow); critic loops use `Agent` (spawn a persona, e.g.
// Shrike on `pact_resolved`).

enum class WorkflowActionKind {
  Prompt,    // body string sent to the agent (legacy default)
  Shell,     // run a shell command, optionally inject output
  Workflow,  // invoke another workflow by id with args
  Agent,     // spawn a persona on a branched subagent
  ToolIntercept,  // pre_tool_use only: replace/wrap the tool call
  // Hook-platform action kinds (Day-3+):
  Script,    // run sandboxed Luau (FIRMIUS_ENABLE_LUAU_HOOKS)
  State,     // mutate HookState without other side effects
  Compose,   // sequential chain of sub-actions
  Tool,      // alongside defines_tool: this hook IS a user-space tool
};

WorkflowActionKind workflowActionKindFromString(const std::string &s);
std::string workflowActionKindToString(WorkflowActionKind kind);

/// Declarative state write applied either as a state-action body or via
/// the post-action `emit.state_writes` channel.
struct WorkflowStateWrite {
  std::string scope;          // "global"|"thread"|"agent"|"hook"
  std::string path;           // dotted path; trailing [] = append
  std::string valueTemplate;  // raw string; resolved by the templater at fire time
  bool append = false;        // mirrors trailing [] for explicit YAML
};

/// One step inside a Compose action. Each step has the same shape as a
/// top-level WorkflowAction kind; the dispatcher executes them in order
/// and threads the previous step's outcome label / outputs to the next.
struct WorkflowComposeStep {
  std::string kind;           // "shell"|"agent"|"prompt"|"state"|...
  std::map<std::string, std::string> params;  // flat for now; nested in a follow-up
  std::string body;           // for kinds that take a freeform body (prompt, script)
  std::vector<WorkflowStateWrite> stateWrites;
};

struct WorkflowAction {
  WorkflowActionKind kind = WorkflowActionKind::Prompt;
  std::string command;          // for Shell
  std::string targetWorkflow;   // for Workflow
  std::vector<std::string> targetArgs;
  std::string targetPersona;    // for Agent
  std::string agentTask;        // for Agent
  int timeoutSec = 30;
  bool injectStdout = false;    // shell stdout → system reminder
  bool injectStderrOnFail = true;

  // ─── Hook-platform additions ─────────────────────────────────────────────
  // Shell envelope (Claude Code / opencode compat)
  bool passEnvelope = false;        // pipe the JSON envelope on stdin
  bool claudeCodeCompat = true;     // exit 2 = block, JSON stdout = structured

  // Agent extensions
  std::string initialMode;          // initial mode for the spawned subagent
  std::string returnSchema;         // expected schema name of the trophy

  // Script (Luau) action
  std::string scriptLanguage;       // "luau" (only one supported today)
  std::string scriptBody;           // inline source
  std::string scriptFile;           // optional pack-relative Lua file

  // State action — declarative writes
  std::vector<WorkflowStateWrite> stateWrites;

  // Compose action
  std::vector<WorkflowComposeStep> composeSteps;
};

// ─── Contract / guards ──────────────────────────────────────────────────────

/**
 * Return contract: a structured shape the workflow is expected to produce.
 * When `post_pact = true`, Shrike (Day 3) verifies the trophy against this
 * schema on workflow completion.
 */
struct WorkflowReturnContract {
  std::string schemaName;       // e.g. "DiagnosisVerdict"
  std::string schemaJson;       // optional inline JSON schema
};

struct WorkflowPrecondition {
  std::string kind;             // file_exists | shell_succeeds | mode_active
  std::map<std::string, std::string> params;
};

struct WorkflowGuard {
  std::vector<WorkflowPrecondition> preconditions;
  std::optional<std::string> requiresMode;  // refuse to start outside mode
  bool lockMode = false;        // forbid mode_switch while running
};

// ─── Hook platform: state surface, emit channel, tool-defining hooks ───────

/// Declared state surface for a hook. The runtime can refuse writes
/// outside this surface and surface the surface to the user on install.
struct WorkflowHookState {
  std::string scope;                       // "thread"|"global"|"agent"|"hook"
  std::vector<std::string> reads;          // documented for tooling
  std::vector<std::string> writes;
};

/// Pack-level state capability declaration loaded from pack.yaml.
struct WorkflowPackStateSurface {
  std::vector<std::string> scopes;
  std::vector<std::string> paths;
};

/// Post-action emission channels. Once the action has produced its
/// trophy, the dispatcher applies `stateWrites` and evaluates
/// `blockDecision` to decide whether the originating tool/event proceeds.
/// `outcomeTemplate` is a string that becomes the named
/// outcome label downstream Compose steps and chained hooks match on.
struct WorkflowBlockDecision {
  std::string condition;       // template; truthy → `then`, else `else`
  std::string thenBranch;      // "allow" | "block"
  std::string elseBranch;      // "allow" | "block"
  std::string injectToAgent;   // optional template; injected on block
};

struct WorkflowEmit {
  std::string outcomeTemplate;
  std::vector<WorkflowStateWrite> stateWrites;
  std::optional<WorkflowBlockDecision> blockDecision;
};

/// `defines_tool`: this hook id is also a user-space tool. The dispatcher
/// registers the tool at boot; invocations route through the action chain.
struct WorkflowDefinesTool {
  std::string name;
  std::string description;
  std::string schemaJson;                  // JSON Schema serialized
  std::string requiredScope;               // ToolScope name
  std::vector<std::string> applicablePersonas;
};

/**
 * Represents a workflow definition loaded from a .md file.
 * Workflows support YAML frontmatter and argument placeholders ($1, $2, etc.)
 *
 * The new fields (trigger, action, returns, guard, slashCommand) are all
 * optional. Existing workflow files keep working as plain prompt macros;
 * new files can opt into the full contract progressively.
 */
struct Workflow {
  std::string id;           // Filename without extension (e.g., "parallel_exploration")
  std::string name;         // Human-readable name from frontmatter
  std::string description;  // Description from frontmatter for autocomplete help
  std::string body;         // The workflow body with $1, $2, etc. placeholders
  std::string sourcePath;   // Absolute/relative file path the workflow loaded from
  std::string sourceDir;    // Parent directory for resolving script_file
  size_t argCount = 0;      // Number of arguments detected in body (legacy: from $N placeholders)
  std::vector<WorkflowArg> args;  // Typed argument definitions from YAML frontmatter

  // Day-2 unified surface fields. All optional / default-constructed for
  // backward compatibility with the existing prompt-macro workflows.
  std::optional<std::string> slashCommand;  // e.g. "/repro" — auto-registers
  bool rawRemainder = false;  // slash command passes trailing text as one arg
  WorkflowTrigger trigger;
  WorkflowAction action;
  WorkflowReturnContract returns;
  WorkflowGuard guard;

  // Day-3+ hook platform fields. All optional.
  std::optional<WorkflowHookState> hookState;
  std::string packId;
  std::optional<WorkflowPackStateSurface> packStateSurface;

  // Tool result payload templates (for `defines_tool` workflows).
  // Parsed from frontmatter:
  //   result:
  //     return:
  //       key: "{{template}}"
  std::map<std::string, std::string> resultReturn;
  std::optional<WorkflowEmit> emit;
  std::optional<WorkflowDefinesTool> definesTool;

  /// True when this workflow is fired by an event (treat as a hook in UX).
  bool isHook() const { return trigger.kind == WorkflowTrigger::Kind::OnEvent; }

  /**
   * Replace $1, $2, etc. placeholders with actual argument values.
   * @param args Vector of argument values to substitute.
   * @return The workflow body with all placeholders replaced.
   */
  std::string build(const std::vector<std::string> &args) const;
};

} // namespace firmius::core
