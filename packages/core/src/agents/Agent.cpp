#include "agents/Agent.hpp"
#include "AgentRegistry.hpp"
#include "EnvLoader.hpp"
#include "Events.hpp"
#include "Message.hpp"
#include "Panic.hpp"
#include "agents/PurposeLoader.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/FSUtil.hpp"
#include "utils/InterruptibleSleep.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::core {

namespace {
std::string toLowerCopy(std::string value);
std::string latestUserText(const AgentContext &context);

constexpr std::uint32_t kMissingToolCallIndex =
    std::numeric_limits<std::uint32_t>::max();

bool hasToolCallIndex(const ToolCallChunk &chunk) {
  return chunk.index != kMissingToolCallIndex;
}

std::vector<ToolCallChunk>::iterator findMatchingToolCallChunk(
    std::vector<ToolCallChunk> &accumulated, const ToolCallChunk &incoming) {
  if (!incoming.id.empty()) {
    auto byId = std::find_if(accumulated.begin(), accumulated.end(),
                             [&](const ToolCallChunk &existing) {
                               return existing.id == incoming.id;
                             });
    if (byId != accumulated.end()) {
      return byId;
    }
  }

  if (hasToolCallIndex(incoming)) {
    auto byIndex = std::find_if(
        accumulated.begin(), accumulated.end(), [&](const ToolCallChunk &existing) {
          if (!hasToolCallIndex(existing) || existing.index != incoming.index) {
            return false;
          }
          return incoming.id.empty() || existing.id.empty() ||
                 existing.id == incoming.id;
        });
    if (byIndex != accumulated.end()) {
      return byIndex;
    }
  }

  return accumulated.end();
}

void mergeToolCallChunk(std::vector<ToolCallChunk> &accumulated,
                        const ToolCallChunk &incoming,
                        std::uint32_t syntheticIdSerial,
                        std::uint32_t turnCount) {
  auto it = findMatchingToolCallChunk(accumulated, incoming);
  if (it == accumulated.end()) {
    auto appended = incoming;
    if (appended.id.empty()) {
      appended.id = "call_" + std::to_string(turnCount) + "_" +
                    std::to_string(syntheticIdSerial);
    }
    accumulated.push_back(std::move(appended));
    return;
  }

  if (it->id.empty() && !incoming.id.empty()) {
    it->id = incoming.id;
  }
  if (!hasToolCallIndex(*it) && hasToolCallIndex(incoming)) {
    it->index = incoming.index;
  }
  it->nameDelta += incoming.nameDelta;
  it->argsDelta += incoming.argsDelta;
}

bool shouldRetryProviderFailureAtAgentLayer(int httpStatus) {
  if (httpStatus < 0) {
    return false;
  }
  if (httpStatus == 0 || httpStatus >= 500) {
    return true;
  }
  return httpStatus == 408 || httpStatus == 409 || httpStatus == 425;
}

std::string threadStorageRootPath() {
  if (const char *home = std::getenv("HOME")) {
    return std::string(home) + "/.firmius/threads";
  }
  return ".firmius/threads";
}

struct TodoStateSnapshot {
  bool hasAny = false;
  bool hasIncomplete = false;
};

struct PlanStateSnapshot {
  bool hasActivePlan = false;
  bool activePlanDone = true;
  bool activePlanKnown = true;
};

enum class TodoEnforcementRole { Lead, Executor, Auditor, Worker, Scout, Unknown };

TodoEnforcementRole todoRoleForPersona(const std::string &persona) {
  const std::string lowered =
      toLowerCopy(shared::StringUtil::trim(persona));
  if (lowered == "lead") {
    return TodoEnforcementRole::Lead;
  }
  if (lowered == "executor") {
    return TodoEnforcementRole::Executor;
  }
  if (lowered == "auditor") {
    return TodoEnforcementRole::Auditor;
  }
  if (lowered == "worker") {
    return TodoEnforcementRole::Worker;
  }
  if (lowered == "scout") {
    return TodoEnforcementRole::Scout;
  }
  return TodoEnforcementRole::Unknown;
}

bool hasAssignedChunk(const AgentContext &context) {
  if (!context.history || context.history->threadId.empty()) {
    return false;
  }
  if (context.identity.id.empty()) {
    return false;
  }

  try {
    ThreadManager tm(threadStorageRootPath());
    for (const auto &plan : tm.listPlans(context.history->threadId)) {
      for (const auto &chunk : plan.chunks) {
        if (chunk.assignedAgentId == context.identity.id) {
          return true;
        }
      }
    }
  } catch (...) {
  }
  return false;
}

std::unordered_map<std::string, shared::ToolScope>
toolScopeIndex(const ToolRegistry &toolRegistry) {
  std::unordered_map<std::string, shared::ToolScope> index;
  for (const auto &meta : toolRegistry.listToolMetadata()) {
    index.emplace(meta.name, meta.scope);
  }
  return index;
}

bool shouldEnforceTodoAfterProse(const AgentContext &context,
                                 const TodoStateSnapshot &todoState,
                                 const PlanStateSnapshot &planState,
                                 bool hasExecutionIntent,
                                 int consecutiveProseOnlyContinuationTurns) {
  if (todoState.hasAny || !hasExecutionIntent) {
    return false;
  }

  const std::string lastUser = latestUserText(context);
  if (lastUser.rfind("Todo required before continuing", 0) == 0) {
    return false;
  }

  const TodoEnforcementRole role =
      todoRoleForPersona(context.config.personaName);
  switch (role) {
  case TodoEnforcementRole::Lead:
    return planState.hasActivePlan &&
           (!planState.activePlanKnown || !planState.activePlanDone);
  case TodoEnforcementRole::Executor:
    if (hasAssignedChunk(context)) {
      return true;
    }
    return consecutiveProseOnlyContinuationTurns > 0;
  case TodoEnforcementRole::Worker:
  case TodoEnforcementRole::Auditor:
    return consecutiveProseOnlyContinuationTurns > 0;
  case TodoEnforcementRole::Scout:
  case TodoEnforcementRole::Unknown:
    return false;
  }
  return false;
}

bool shouldEnforceTodoBeforeTools(const AgentContext &context,
                                  const TodoStateSnapshot &todoState,
                                  const PlanStateSnapshot &planState,
                                  const std::vector<ToolCallChunk> &chunks,
                                  const ToolRegistry &toolRegistry) {
  if (todoState.hasAny) {
    return false;
  }

  bool hasNonTodoTool = false;
  for (const auto &chunk : chunks) {
    if (chunk.nameDelta != "todo_write") {
      hasNonTodoTool = true;
      break;
    }
  }
  if (!hasNonTodoTool) {
    return false;
  }

  const TodoEnforcementRole role =
      todoRoleForPersona(context.config.personaName);
  switch (role) {
  case TodoEnforcementRole::Executor:
  case TodoEnforcementRole::Worker:
  case TodoEnforcementRole::Auditor:
    return true;
  case TodoEnforcementRole::Lead: {
    if (planState.hasActivePlan &&
        (!planState.activePlanKnown || !planState.activePlanDone)) {
      return true;
    }
    const auto scopes = toolScopeIndex(toolRegistry);
    for (const auto &chunk : chunks) {
      if (chunk.nameDelta == "todo_write") {
        continue;
      }
      auto it = scopes.find(chunk.nameDelta);
      if (it == scopes.end()) {
        continue;
      }
      switch (it->second) {
      case shared::ToolScope::FilesystemWrite:
      case shared::ToolScope::Process:
      case shared::ToolScope::Semantic:
      case shared::ToolScope::Git:
      case shared::ToolScope::PlanWrite:
      case shared::ToolScope::ChunkWrite:
      case shared::ToolScope::ChunkAssign:
      case shared::ToolScope::ChunkReview:
      case shared::ToolScope::Delegation:
        return true;
      case shared::ToolScope::FilesystemRead:
      case shared::ToolScope::Web:
      case shared::ToolScope::PlanRead:
      case shared::ToolScope::ChunkRead:
        break;
      }
    }
    return false;
  }
  case TodoEnforcementRole::Scout:
  case TodoEnforcementRole::Unknown:
    return false;
  }
  return false;
}

std::string todoEnforcementMessage(const AgentContext &context,
                                   bool isToolGate) {
  const TodoEnforcementRole role =
      todoRoleForPersona(context.config.personaName);
  std::string roleLabel = "work";
  switch (role) {
  case TodoEnforcementRole::Lead:
    roleLabel = "coordination";
    break;
  case TodoEnforcementRole::Executor:
    roleLabel = "chunk work";
    break;
  case TodoEnforcementRole::Auditor:
    roleLabel = "review";
    break;
  case TodoEnforcementRole::Worker:
    roleLabel = "bounded work";
    break;
  case TodoEnforcementRole::Scout:
  case TodoEnforcementRole::Unknown:
    roleLabel = "work";
    break;
  }
  std::ostringstream msg;
  msg << "Todo required before continuing multi-step " << roleLabel << ". "
      << "Create or update your todo list with todo_write";
  if (isToolGate) {
    msg << " before executing tools";
  }
  msg << ".";
  return msg.str();
}

struct StopTokenFilterState {
  std::string carry;
  bool removedToken = false;
};

constexpr std::string_view kFinalSummaryStopToken = "<firmius_stop/>";

std::string stripFinalSummaryStopToken(const std::string &text, bool &removed) {
  std::string filtered = text;
  std::size_t pos = 0;
  while ((pos = filtered.find(kFinalSummaryStopToken, pos)) !=
         std::string::npos) {
    filtered.erase(pos, kFinalSummaryStopToken.size());
    removed = true;
  }
  return filtered;
}

std::string consumeVisibleDelta(StopTokenFilterState &state,
                                const std::string &delta) {
  if (delta.empty()) {
    return "";
  }

  state.carry += delta;
  std::string visible;
  const std::size_t tokenSize = kFinalSummaryStopToken.size();
  while (true) {
    const std::size_t tokenPos = state.carry.find(kFinalSummaryStopToken);
    if (tokenPos != std::string::npos) {
      visible += state.carry.substr(0, tokenPos);
      state.carry.erase(0, tokenPos + tokenSize);
      state.removedToken = true;
      continue;
    }
    if (state.carry.size() > tokenSize) {
      const std::size_t emitLen = state.carry.size() - tokenSize + 1;
      visible += state.carry.substr(0, emitLen);
      state.carry.erase(0, emitLen);
    }
    break;
  }
  return visible;
}

std::string flushVisibleDelta(StopTokenFilterState &state) {
  if (state.carry.empty()) {
    return "";
  }
  std::string emit = std::move(state.carry);
  state.carry.clear();
  return stripFinalSummaryStopToken(emit, state.removedToken);
}

TodoStateSnapshot readTodoState(const AgentContext &context) {
  TodoStateSnapshot snapshot;
  if (!context.history || context.history->threadId.empty()) {
    return snapshot;
  }
  if (context.identity.id.empty()) {
    return snapshot;
  }

  try {
    ThreadManager tm(threadStorageRootPath());
    const AgentTodoList todo =
        tm.getAgentTodo(context.history->threadId, context.identity.id);
    snapshot.hasAny = !todo.items.empty();
    snapshot.hasIncomplete =
        std::any_of(todo.items.begin(), todo.items.end(),
                    [](const TodoItem &item) {
                      return item.status != TodoStatus::Done;
                    });
    return snapshot;
  } catch (...) {
    return snapshot;
  }
}

PlanStateSnapshot readPlanState(const AgentContext &context) {
  PlanStateSnapshot snapshot;
  if (!context.history || context.history->threadId.empty()) {
    return snapshot;
  }

  try {
    ThreadManager tm(threadStorageRootPath());
    const ThreadMetadata metadata = tm.getMetadata(context.history->threadId);
    if (metadata.activePlanId.empty()) {
      return snapshot;
    }
    snapshot.hasActivePlan = true;
    try {
      const Plan plan =
          tm.getPlan(context.history->threadId, metadata.activePlanId);
      snapshot.activePlanDone = (plan.status == PlanStatus::Done);
      snapshot.activePlanKnown = true;
    } catch (...) {
      snapshot.activePlanDone = false;
      snapshot.activePlanKnown = false;
    }
    return snapshot;
  } catch (...) {
    return snapshot;
  }
}

bool isExecutionalStatus(AgentStatus status) {
  return status == AgentStatus::ExecutingTool ||
         status == AgentStatus::Compacting;
}

std::string toLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool containsAnyPhrase(const std::string &text,
                       const std::vector<std::string> &phrases) {
  for (const auto &phrase : phrases) {
    if (!phrase.empty() && text.find(phrase) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string latestUserText(const AgentContext &context) {
  if (!context.history) {
    return "";
  }
  for (auto turnIt = context.history->turns.rbegin();
       turnIt != context.history->turns.rend(); ++turnIt) {
    for (auto msgIt = turnIt->messages.rbegin();
         msgIt != turnIt->messages.rend(); ++msgIt) {
      if (msgIt->role != Role::User) {
        continue;
      }
      for (const auto &part : msgIt->content) {
        if (const auto *txt = std::get_if<TextContent>(&part)) {
          if (!shared::StringUtil::trim(txt->text).empty()) {
            return txt->text;
          }
        }
      }
    }
  }
  return "";
}

bool hasOpenExecutionIntent(const AgentContext &context) {
  const std::string request = toLowerCopy(latestUserText(context));
  if (request.empty()) {
    return false;
  }

  const std::vector<std::string> executionPhrases = {
      "implement",   "build",         "add feature", "add ",      "create ",
      "fix ",        "refactor",      "patch",       "modify",    "change ",
      "update ",     "write code",    "make changes","develop",   "continue working",
      "continue the", "keep going",   "finish this", "ship",      "execute",
      "run this task", "complete the task"};
  const std::vector<std::string> executionContextPhrases = {
      "codebase", "tests", "ctest", "compile", "bug", "feature", "hunk",
      "implementation", "agent loop", "runtime", "harness"};
  const std::vector<std::string> informationalPhrases = {
      "what is", "what are", "why is", "why are", "how does", "explain",
      "describe", "summarize", "summary", "tell me", "research", "find out"};

  const bool hasExecutionVerb = containsAnyPhrase(request, executionPhrases);
  const bool hasExecutionContext =
      hasExecutionVerb || containsAnyPhrase(request, executionContextPhrases);
  const bool isLikelyInformational =
      containsAnyPhrase(request, informationalPhrases);
  const bool isQuestion = request.find('?') != std::string::npos;

  if (hasExecutionVerb) {
    return true;
  }
  if (hasExecutionContext && !isLikelyInformational) {
    return true;
  }
  if (isLikelyInformational && !hasExecutionContext) {
    return false;
  }
  return !isQuestion && hasExecutionContext;
}

bool isDescendantAgentRunning(const std::string &candidateAgentId,
                              const std::string &ancestorAgentId) {
  if (candidateAgentId.empty() || ancestorAgentId.empty() ||
      candidateAgentId == ancestorAgentId) {
    return false;
  }

  auto candidate = AgentRegistry::instance().getAgent(candidateAgentId);
  int depth = 0;
  while (candidate && depth < 100) {
    const auto &parentId = candidate->getContext().identity.parentId;
    if (parentId.empty()) {
      return false;
    }
    if (parentId == ancestorAgentId) {
      return candidate->isRunning() || candidate->isBooting();
    }
    candidate = AgentRegistry::instance().getAgent(parentId);
    depth++;
  }
  return false;
}

struct ToolCallValidationFailure {
  std::string toolCallId;
  std::string message;
};

std::vector<ToolCallValidationFailure>
validateStreamedToolCalls(const std::vector<ToolCallChunk> &chunks) {
  std::vector<ToolCallValidationFailure> failures;
  for (const auto &chunk : chunks) {
    if (shared::StringUtil::trim(chunk.nameDelta).empty()) {
      failures.push_back({chunk.id, "missing tool name"});
      continue;
    }

    rapidjson::Document args;
    args.Parse(chunk.argsDelta.c_str());
    if (args.HasParseError()) {
      failures.push_back(
          {chunk.id, "invalid or truncated JSON arguments for tool '" +
                         chunk.nameDelta + "'"});
      continue;
    }

    if (!args.IsObject()) {
      failures.push_back(
          {chunk.id, "tool arguments for '" + chunk.nameDelta +
                         "' must be a JSON object"});
    }
  }
  return failures;
}
} // namespace

using namespace firmius::shared;

std::uint64_t Agent::nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

Agent::Agent(AgentContext ctx, std::shared_ptr<shared::IEnvironment> env,
             std::shared_ptr<shared::IPermissions> perms, ToolRegistry &reg,
             std::shared_ptr<Journaler> jnl)
    : context(std::move(ctx)), environment_(std::move(env)),
      permissions_(std::move(perms)), toolRegistry(reg), journaler(jnl) {
  if (!context.history) {
    context.history = std::make_shared<AgentHistory>();
  }

  provider = firmius::provider::ProviderRegistry::instance().getProvider(
      context.config.providerId);
  if (!provider)
    throw std::runtime_error("Unknown provider: " + context.config.providerId);
}

Agent::~Agent() {
  for (const auto &id : backgroundProcessIds) {
    try {
      environment_->getHost()->killBackgroundProcess(id);
    } catch (...) {
    }
  }
  if (environment_->getHost())
    environment_->getHost()->destroy();
}

void Agent::reset() {
  context.history->turns.clear();
  context.aggregateMetrics = {};
  context.state = {};
  interrupted = false;
  running = false;

  for (const auto &id : backgroundProcessIds) {
    try {
      environment_->getHost()->killBackgroundProcess(id);
    } catch (...) {
    }
  }
  backgroundProcessIds.clear();
}

std::string Agent::resolvePath(const std::string &inputPath) const {
  return environment_->getWorkspace().resolvePath(inputPath);
}

std::shared_ptr<IHost> Agent::getHost() { return environment_->getHost(); }

void Agent::interrupt() {
  interrupted = true;
  running = false; // Allow immediate re-tasking after interrupt
}

void Agent::clearInterrupt() { interrupted = false; }

void Agent::setModel(const std::string &providerId,
                     const std::string &modelId) {
  if (running.load()) {
    throw std::runtime_error("Cannot switch model while agent is running");
  }

  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerId);
  if (!newProvider) {
    throw std::runtime_error("Unknown provider: " + providerId);
  }

  context.config.providerId = providerId;
  context.config.modelId = modelId;
  provider = newProvider;
}

void Agent::compactNow(
    std::function<void(const shared::StreamEvent &)> onEvent) {
  compactContext(std::move(onEvent));
}

void Agent::setModel(const std::string &providerId, const std::string &modelId,
                     const std::string &variantName) {
  if (running.load()) {
    throw std::runtime_error("Cannot switch model while agent is running");
  }

  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerId);
  if (!newProvider) {
    throw std::runtime_error("Unknown provider: " + providerId);
  }

  context.config.providerId = providerId;
  context.config.modelId = modelId;
  context.config.modelVariant = variantName;
  provider = newProvider;
}

std::string Agent::spawnProcess(const std::string &command,
                                const std::string &toolCallId,
                                const std::string &cwd,
                                const std::map<std::string, std::string> &env,
                                bool monitorCompletion) {
  return environment_->getProcessManager().spawnProcess(command, toolCallId, cwd,
                                                        env, monitorCompletion);
}

shared::ProcessSnapshot Agent::inspectProcess(const std::string &id) {
  return environment_->getProcessManager().inspectProcess(id);
}

void Agent::writeToProcess(const std::string &id, const std::string &data) {
  environment_->getProcessManager().writeToProcess(id, data);
}

void Agent::registerProcessId(const std::string &id) {
  environment_->getProcessManager().registerProcessId(id);
}

void Agent::emitProcessSpawned(const std::string &processId,
                               const std::string &toolCallId,
                               const std::string &command) {
  environment_->getProcessManager().emitProcessSpawned(processId, toolCallId,
                                                       command);
}

void Agent::addBlockingProcessId(const std::string &id) {
  environment_->getProcessManager().addBlockingProcessId(id);
}

void Agent::removeBlockingProcessId(const std::string &id) {
  environment_->getProcessManager().removeBlockingProcessId(id);
}

std::vector<std::string> Agent::getBlockingProcessIds() {
  return environment_->getProcessManager().getBlockingProcessIds();
}

bool Agent::hasReadFile(const std::string &path) const {
  return environment_->getWorkspace().hasReadFile(path);
}

void Agent::markFileAsRead(const std::string &path) {
  environment_->getWorkspace().markFileAsRead(path);
}

bool Agent::hasFullyReadFile(const std::string &path) const {
  return environment_->getWorkspace().hasFullyReadFile(path);
}

void Agent::markFileAsFullyRead(const std::string &path) {
  environment_->getWorkspace().markFileAsFullyRead(path);
}

void Agent::run(const std::string &task,
                std::function<void(const shared::StreamEvent &)> onEvent,
                const std::vector<ImageContent> &images) {
  runImpl(task, std::move(onEvent), images);
}

void Agent::resume(std::function<void(const shared::StreamEvent &)> onEvent) {
  runImpl(std::nullopt, std::move(onEvent), {});
}

void Agent::runImpl(const std::optional<std::string> &task,
                    std::function<void(const shared::StreamEvent &)> onEvent,
                    const std::vector<ImageContent> &images) {
  {
    std::lock_guard<std::mutex> lock(callbackMutex);
    eventCallback = onEvent;
  }

  // Guard against concurrent runs with mutex
  std::lock_guard<std::mutex> lock(runMutex);
  if (running.load()) {
    throw std::runtime_error("Agent is already running");
  }
  running = true;
  booting = false;
  interrupted = false;
  context.state.currentStatus = AgentStatus::Idle;
  context.state.fatalError = std::nullopt;

  // 1. Bootstrap System Message
  if (context.history->turns.empty()) {
    auto toolDefs =
        toolRegistry.getAvailableToolDefinitions(context.permissions);
    std::string personaName = context.config.personaName.empty()
                                  ? "lead"
                                  : context.config.personaName;
    Persona persona = PurposeLoader::load(personaName);
    std::string toolBlock = PurposeLoader::buildToolsBlock(toolDefs);

    std::string protocolAddon =
        "\n\n# PROTOCOL STRICTNESS\n"
        "- If you are calling a tool, your message MUST contain ONLY the tool "
        "call JSON.\n"
        "- Do NOT include any other text or thinking when calling a tool.\n";

    std::string systemPrompt =
        PurposeLoader::composeSystemPrompt(persona, context, toolBlock) +
        protocolAddon;

    AgentTurn turn;
    turn.turnId = "bootstrap-system";
    Message msg;
    msg.role = Role::System;
    msg.content.push_back(TextContent{systemPrompt});
    auto now = std::chrono::system_clock::now();
    msg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    turn.messages.push_back(msg);
    context.history->turns.push_back(turn);
    if (context.config.persistHistory && journaler)
      journaler->appendTurn(turn);
  }

  // 2. Add Task as User Message when explicitly re-tasking.
  if (task.has_value()) {
    AgentTurn taskTurn;
    taskTurn.turnId =
        "user-task-" + std::to_string(context.history->turns.size());
    Message taskMsg;
    taskMsg.role = Role::User;
    taskMsg.content.push_back(TextContent{*task});

    for (const auto &img : images) {
      taskMsg.content.push_back(img);
    }
    auto now = std::chrono::system_clock::now();
    taskMsg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    taskTurn.messages.push_back(taskMsg);
    context.history->turns.push_back(taskTurn);
    if (context.config.persistHistory && journaler) {
      journaler->appendTurn(taskTurn);
    }
  }

  // 3. Autonomous Loop
  bool taskFinished = false;
  int maxTurns = context.config.maxTurns > 0 ? context.config.maxTurns : 200;
  int turnCount = 0;

  int consecutiveProviderFailures = 0;
  const int maxProviderRetries = 3;
  int consecutiveProseOnlyContinuationTurns = 0;
  const int maxProseOnlyContinuationTurns = 2;

  while (!taskFinished && turnCount < maxTurns && !interrupted.load()) {
    // --- CHECK FOR CONTEXT COMPACTION ---
    try {
      auto model = provider->getModelInfo(context.config.modelId);
      bool forceCompact = (std::getenv("FORCE_COMPACTION") != nullptr);
      if (forceCompact || (model.contextWindow > 0 &&
                           context.aggregateMetrics.tokens.contextSize >
                               model.contextWindow * 0.8)) {
        compactContext(onEvent);
      }
      if (interrupted.load())
        break;
    } catch (...) {
      // Compaction is best-effort
    }

    turnCount++;

    try {
      // --- State: ProviderWaiting ---
      context.state.currentStatus = AgentStatus::ProviderWaiting;
      onEvent(ProviderWaiting{});

      firmius::provider::ProviderOptions opts;
      opts.modelId = context.config.modelId;
      try {
        auto modelInfo = provider->getModelInfo(context.config.modelId);
        for (const auto &v : modelInfo.variants) {
          if (v.variantName == context.config.modelVariant) {
            opts.modelVariantJson = v.extraMetadataJson;
            break;
          }
        }
      } catch (...) {
      }
      opts.temperature = context.config.temperature;
      if (context.config.maxTokens.has_value()) {
        opts.maxTokens = context.config.maxTokens;
      }
      opts.stop = context.config.stop;
      opts.tools =
          toolRegistry.getAvailableToolDefinitions(context.permissions);
      opts.abortSignal = &interrupted;

      std::vector<ToolCallChunk> accumulatedToolChunks;
      std::string fullResponse;
      std::string fullThinking;
      std::string lastThinkingSignature;
      AgentMetrics turnMetrics;
      StopReason turnStopReason = StopReason::Stop;
      std::string streamError;
      int streamErrorStatus = 0;
      bool sawContent = false;
      bool sawThinking = false;
      bool sawTool = false;
      bool requestedFinalSummaryStop = false;
      std::uint32_t syntheticToolCallIdSerial = 0;
      StopTokenFilterState textStopTokenFilter;
      StopTokenFilterState thinkingStopTokenFilter;

      // Token repetition detection for hallucination loops
      bool tokenLoopDetected = false;
      char lastChar = '\0';
      int consecutiveRepeatCount = 0;
      const int MAX_CONSECUTIVE_REPEAT = 15;

      auto appendVisibleText = [&](const std::string &delta) {
        if (delta.empty()) {
          return;
        }
        onEvent(TextChunk{delta});
        for (unsigned char c : delta) {
          if (!std::isspace(c)) {
            sawContent = true;
            break;
          }
        }
        // Check for token loop (same character repeated)
        for (char c : delta) {
          if (c == lastChar && !tokenLoopDetected) {
            consecutiveRepeatCount++;
            if (consecutiveRepeatCount >= MAX_CONSECUTIVE_REPEAT) {
              tokenLoopDetected = true;
            }
          } else {
            consecutiveRepeatCount = 1;
            lastChar = c;
          }
        }
        if (!tokenLoopDetected) {
          fullResponse += delta;
        }
      };

      auto appendVisibleThinking = [&](const std::string &delta) {
        if (delta.empty()) {
          return;
        }
        onEvent(ThinkingChunk{delta, ""});
        sawThinking = true;
        fullThinking += delta;
      };

      auto flushText = [&]() {
        const std::string delta = flushVisibleDelta(textStopTokenFilter);
        appendVisibleText(delta);
      };

      auto flushThinking = [&]() {
        const std::string delta = flushVisibleDelta(thinkingStopTokenFilter);
        appendVisibleThinking(delta);
      };

      provider->stream(*context.history, opts, [&](const StreamEvent &ev) {
        if (context.state.currentStatus == AgentStatus::ProviderWaiting) {
          if (std::holds_alternative<TextChunk>(ev) ||
              std::holds_alternative<ThinkingChunk>(ev) ||
              std::holds_alternative<ToolCallChunk>(ev)) {
            context.state.currentStatus = AgentStatus::Streaming;
          }
        }

        if (auto *txt = std::get_if<TextChunk>(&ev)) {
          flushThinking();
          const std::string visible = consumeVisibleDelta(textStopTokenFilter, txt->delta);
          requestedFinalSummaryStop =
              requestedFinalSummaryStop || textStopTokenFilter.removedToken;
          appendVisibleText(visible);
        } else if (auto *thk = std::get_if<ThinkingChunk>(&ev)) {
          flushText();
          const std::string visible =
              consumeVisibleDelta(thinkingStopTokenFilter, thk->delta);
          appendVisibleThinking(visible);
          if (!thk->signature.empty()) {
            lastThinkingSignature = thk->signature;
          }
        } else if (auto *tcc = std::get_if<ToolCallChunk>(&ev)) {
          flushText();
          flushThinking();
          sawTool = true;
          // Emit immediately so TUI can show "Preparing" state
          onEvent(ev);
          mergeToolCallChunk(accumulatedToolChunks, *tcc,
                             syntheticToolCallIdSerial++, turnCount);
        } else if (auto *met = std::get_if<AgentMetrics>(&ev)) {
          flushText();
          flushThinking();
          onEvent(ev);
          turnMetrics = *met;
        } else if (auto *done = std::get_if<StreamDone>(&ev)) {
          flushText();
          flushThinking();
          onEvent(ev);
          turnStopReason = done->reason;
        } else if (auto *err = std::get_if<StreamError>(&ev)) {
          flushText();
          flushThinking();
          onEvent(ev);
          streamErrorStatus = err->httpStatus;
          // Don't treat abort/interrupt errors as stream errors
          if (err->message.find("interrupted") != std::string::npos ||
              err->message.find("aborted") != std::string::npos ||
              err->message.find("Cancelled") != std::string::npos) {
            context.state.currentStatus = AgentStatus::Cancelled;
            return;
          }
          streamError = err->message;
        } else {
          flushText();
          flushThinking();
          onEvent(ev);
        }
      });
      flushText();
      flushThinking();

      // ToolCallChunk events are now emitted immediately above
      // No need to re-emit buffered events

      if (sawTool) {
        turnStopReason = StopReason::ToolUse;
      } else if (sawContent) {
        accumulatedToolChunks.clear();
        if (turnStopReason == StopReason::ToolUse) {
          turnStopReason = StopReason::Stop;
        }
      }

      // If token loop was detected, inject a nudge to refocus the agent
      if (tokenLoopDetected && fullResponse.empty() &&
          accumulatedToolChunks.empty()) {
        AgentTurn loopNudgeTurn;
        loopNudgeTurn.turnId = "loop-nudge-" + std::to_string(turnCount);
        Message loopNudgeMsg;
        loopNudgeMsg.role = Role::User;
        loopNudgeMsg.content.push_back(TextContent{
            "Your output was cut off due to token repetition (hallucination "
            "loop). "
            "Please step back, refocus, and try a different approach. "
            "Do NOT repeat the same failed actions."});
        auto nudgeNow = std::chrono::system_clock::now();
        loopNudgeMsg.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                nudgeNow.time_since_epoch())
                .count());
        loopNudgeTurn.messages.push_back(loopNudgeMsg);
        context.history->turns.push_back(loopNudgeTurn);
        if (context.config.persistHistory && journaler)
          journaler->appendTurn(loopNudgeTurn);

        // Continue to next turn instead of erroring
        continue;
      }

      // If there was a stream error and no content came back, retry
      if (!streamError.empty() && fullResponse.empty() &&
          accumulatedToolChunks.empty()) {
        // Don't retry if user interrupted
        if (interrupted.load()) {
          context.state.currentStatus = AgentStatus::Cancelled;
          return;
        }
        if (!shouldRetryProviderFailureAtAgentLayer(streamErrorStatus)) {
          throw std::runtime_error("Provider stream error: " + streamError);
        }
        consecutiveProviderFailures++;
        if (consecutiveProviderFailures > maxProviderRetries) {
          throw std::runtime_error("Provider stream error: " + streamError);
        }
        // Emit retry event and wait briefly before retrying
        int retryDelaySec = 1 << (consecutiveProviderFailures - 1); // 1, 2, 4
        onEvent(StreamRetrying{consecutiveProviderFailures, maxProviderRetries,
                               429, retryDelaySec * 1000,
                               "Provider error, retrying", ""});
        // Use interruptible sleep to allow immediate cancellation
        if (!interruptibleSleep(std::chrono::seconds(retryDelaySec),
                                &interrupted)) {
          // Interrupted during retry delay
          context.state.currentStatus = AgentStatus::Cancelled;
          return;
        }
        continue;
      }

      const bool providerDeclaredToolStreamTruncation =
          streamError.find(
              "Provider stream truncated during tool-call generation") !=
              std::string::npos ||
          streamError.find("incomplete tool-call arguments for tool") !=
              std::string::npos;
      if (!streamError.empty() && providerDeclaredToolStreamTruncation) {
        throw std::runtime_error("Provider stream error: " + streamError);
      }

      const auto malformedToolCalls =
          validateStreamedToolCalls(accumulatedToolChunks);
      if (!malformedToolCalls.empty()) {
        std::ostringstream error;
        error << "Provider emitted malformed streamed tool call payload";
        if (malformedToolCalls.size() > 1) {
          error << "s";
        }
        error << ": ";
        for (std::size_t i = 0; i < malformedToolCalls.size(); ++i) {
          if (i > 0) {
            error << "; ";
          }
          const auto &failure = malformedToolCalls[i];
          error << "[" << (failure.toolCallId.empty() ? "unknown"
                                                      : failure.toolCallId)
                << "] " << failure.message;
        }
        throw std::runtime_error(error.str());
      }

      // Reset consecutive failure counter on success
      consecutiveProviderFailures = 0;

      // --- Build assistant turn ---
      AgentTurn assistantTurn;
      assistantTurn.turnId =
          "assistant-" + std::to_string(context.history->turns.size());
      assistantTurn.stopReason = turnStopReason;

      // Store per-turn metrics
      assistantTurn.metrics = turnMetrics;

      // Accumulate into session total
      context.aggregateMetrics += turnMetrics;

      Message assistantMsg;
      assistantMsg.role = Role::Assistant;
      if (!fullThinking.empty())
        assistantMsg.content.push_back(
            ThinkingContent{fullThinking, lastThinkingSignature});
      if (!fullResponse.empty())
        assistantMsg.content.push_back(TextContent{fullResponse});

      for (const auto &chunk : accumulatedToolChunks) {
        assistantMsg.content.push_back(
            ToolCallContent{chunk.id, chunk.nameDelta, chunk.argsDelta});
      }

      auto now_end = std::chrono::system_clock::now();
      assistantMsg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now_end.time_since_epoch())
              .count());

      assistantTurn.messages.push_back(assistantMsg);
      context.history->turns.push_back(assistantTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(assistantTurn);

      // Broadcast turn completion
      onEvent(AgentTurnCompleted{context.identity.id, assistantTurn,
                                 context.aggregateMetrics,
                                 context.identity.parentId});

      // --- Check for termination ---
      if (accumulatedToolChunks.empty()) {
        if (fullResponse.empty() && fullThinking.empty()) {
          // Don't error if user interrupted
          if (interrupted.load()) {
            context.state.currentStatus = AgentStatus::Cancelled;
            return;
          }
          throw std::runtime_error(
              "Provider returned empty response (timeout or model failure)");
        }

        const bool hasPendingToolCalls = !context.state.pendingToolCalls.empty();
        const bool hasBlockingProcesses = !getBlockingProcessIds().empty();
        const bool hasExecutionStatus =
            isExecutionalStatus(context.state.currentStatus);
        const bool hasRunningOwnedBackgroundProcess =
            std::any_of(context.state.ownedProcesses.begin(),
                        context.state.ownedProcesses.end(),
                        [&](const std::string &processId) {
                          if (processId.empty()) {
                            return false;
                          }
                          try {
                            return inspectProcess(processId).running;
                          } catch (...) {
                            return false;
                          }
                        });

        bool hasRunningDescendantSubagent = false;
        if (!context.identity.id.empty()) {
          for (const auto &agentId : AgentRegistry::instance().listAll()) {
            if (isDescendantAgentRunning(agentId, context.identity.id)) {
              hasRunningDescendantSubagent = true;
              break;
            }
          }
        }

        const TodoStateSnapshot todoState = readTodoState(context);
        const PlanStateSnapshot planState = readPlanState(context);
        const bool hasIncompleteTodoState = todoState.hasIncomplete;
        const bool hasRequestContinuationIntent = hasOpenExecutionIntent(context);
        const bool hasExecutionIntentWithoutTodo =
            !todoState.hasAny && hasRequestContinuationIntent;
        const bool todoAllowsStop =
            todoState.hasAny ? !hasIncompleteTodoState
                             : !hasExecutionIntentWithoutTodo;
        const bool planAllowsStop =
            !planState.hasActivePlan ||
            (planState.activePlanKnown && planState.activePlanDone);
        const bool hasPendingToolLifecycleActivity =
            hasPendingToolCalls || hasExecutionStatus;
        const bool hasHarnessOwnedActiveWork =
            hasPendingToolLifecycleActivity || hasBlockingProcesses ||
            hasRunningOwnedBackgroundProcess || hasRunningDescendantSubagent ||
            hasIncompleteTodoState || hasExecutionStatus;
        const bool shouldHonorFinalSummaryStop =
            requestedFinalSummaryStop && turnStopReason == StopReason::Stop &&
            !hasPendingToolLifecycleActivity && !hasBlockingProcesses &&
            !hasRunningOwnedBackgroundProcess &&
            !hasRunningDescendantSubagent && planAllowsStop && todoAllowsStop;

        if (shouldEnforceTodoAfterProse(context, todoState, planState,
                                        hasRequestContinuationIntent,
                                        consecutiveProseOnlyContinuationTurns)) {
          AgentTurn todoNudgeTurn;
          todoNudgeTurn.turnId =
              "todo-enforcement-" + std::to_string(turnCount);
          Message todoNudgeMsg;
          todoNudgeMsg.role = Role::User;
          todoNudgeMsg.content.push_back(
              TextContent{todoEnforcementMessage(context, false)});
          auto nudgeNow = std::chrono::system_clock::now();
          todoNudgeMsg.timestamp = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  nudgeNow.time_since_epoch())
                  .count());
          todoNudgeTurn.messages.push_back(todoNudgeMsg);
          context.history->turns.push_back(todoNudgeTurn);
          if (context.config.persistHistory && journaler) {
            journaler->appendTurn(todoNudgeTurn);
          }
          context.state.currentStatus = AgentStatus::Idle;
          consecutiveProseOnlyContinuationTurns = 0;
          continue;
        }

        if (shouldHonorFinalSummaryStop) {
          consecutiveProseOnlyContinuationTurns = 0;
          taskFinished = true;
          continue;
        }

        bool shouldContinueAfterProseTurn = false;
        if (hasHarnessOwnedActiveWork) {
          shouldContinueAfterProseTurn = true;
        } else if (!todoState.hasAny && hasRequestContinuationIntent &&
                   consecutiveProseOnlyContinuationTurns <
                       maxProseOnlyContinuationTurns) {
          shouldContinueAfterProseTurn = true;
        }

        if (shouldContinueAfterProseTurn &&
            consecutiveProseOnlyContinuationTurns <
                maxProseOnlyContinuationTurns) {
          consecutiveProseOnlyContinuationTurns++;
          context.state.currentStatus = AgentStatus::Idle;
          continue;
        }

        taskFinished = true;
      } else {
        consecutiveProseOnlyContinuationTurns = 0;
        const TodoStateSnapshot todoState = readTodoState(context);
        const PlanStateSnapshot planState = readPlanState(context);
        if (shouldEnforceTodoBeforeTools(context, todoState, planState,
                                         accumulatedToolChunks, toolRegistry)) {
          AgentTurn todoNudgeTurn;
          todoNudgeTurn.turnId =
              "todo-tool-enforcement-" + std::to_string(turnCount);
          Message todoNudgeMsg;
          todoNudgeMsg.role = Role::User;
          todoNudgeMsg.content.push_back(
              TextContent{todoEnforcementMessage(context, true)});
          auto nudgeNow = std::chrono::system_clock::now();
          todoNudgeMsg.timestamp = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  nudgeNow.time_since_epoch())
                  .count());
          todoNudgeTurn.messages.push_back(todoNudgeMsg);
          context.history->turns.push_back(todoNudgeTurn);
          if (context.config.persistHistory && journaler) {
            journaler->appendTurn(todoNudgeTurn);
          }
          context.state.currentStatus = AgentStatus::Idle;
          continue;
        }

        // --- State: ExecutingTool ---
        context.state.currentStatus = AgentStatus::ExecutingTool;

        // Track pending tool calls
        for (const auto &chunk : accumulatedToolChunks) {
          context.state.pendingToolCalls.push_back(chunk.id);
        }

        auto toolStartMs = nowMs();
        executeTools(accumulatedToolChunks, onEvent);
        auto toolEndMs = nowMs();

        // Update the turn metrics with tool execution time
        // (The turn is already pushed to history, so update the last assistant
        // turn in-place)
        auto &lastTurn =
            context.history
                ->turns[context.history->turns.size() -
                        2]; // assistant turn is 2nd-to-last (tool result turn
                            // was just pushed by executeTools)
        lastTurn.metrics.timing.toolExecutionMs = toolEndMs - toolStartMs;

        // Also update the aggregate (only the tool timing delta)
        context.aggregateMetrics.timing.toolExecutionMs +=
            (toolEndMs - toolStartMs);

        // Clear pending tool calls
        context.state.pendingToolCalls.clear();
      }

    } catch (const std::exception &e) {
      // --- State: Error ---
      context.state.currentStatus = AgentStatus::Error;
      context.state.fatalError = e.what();

      // Persist error as a system turn in history for journal survival
      AgentTurn errorTurn;
      errorTurn.turnId =
          "error-" + std::to_string(context.history->turns.size());
      Message errorMsg;
      errorMsg.role = Role::Error;
      errorMsg.content.push_back(
          ErrorContent{"Agent Runtime Error",
                       "The agent encountered a fatal runtime exception.",
                       std::string(e.what())});
      errorMsg.timestamp = nowMs();
      errorTurn.messages.push_back(errorMsg);
      context.history->turns.push_back(errorTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(errorTurn);

      // Emit error as a StreamError event
      onEvent(StreamError{e.what(), 0, ""});
      break;
    }
  }

  // --- Final state ---
  if (interrupted.load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
    if (!context.history->turns.empty()) {
      const auto &lastTurn = context.history->turns.back();
      const bool alreadyCancelledTurn =
          lastTurn.turnId.rfind("cancelled-", 0) == 0;
      if (!alreadyCancelledTurn) {
        AgentTurn cancelledTurn;
        cancelledTurn.turnId =
            "cancelled-" + std::to_string(context.history->turns.size());
        Message cancelledMsg;
        cancelledMsg.role = Role::Error;
        cancelledMsg.content.push_back(ErrorContent{
            "Agent Cancelled",
            "The agent execution was interrupted.",
            "Execution stopped before completion and can be resumed."});
        cancelledMsg.timestamp = nowMs();
        cancelledTurn.messages.push_back(cancelledMsg);
        context.history->turns.push_back(cancelledTurn);
        if (context.config.persistHistory && journaler) {
          journaler->appendTurn(cancelledTurn);
        }
      }
    }
  } else if (context.state.currentStatus != AgentStatus::Error) {
    context.state.currentStatus = AgentStatus::Idle;
  }

  running = false;
}

void Agent::executeTools(const std::vector<ToolCallChunk> &chunks,
                         std::function<void(const StreamEvent &)> onEvent) {
  // Check for insanity loop BEFORE executing tools
  for (const auto &chunk : chunks) {
    // Create signature: "toolName:args"
    std::string signature = chunk.nameDelta + ":" + chunk.argsDelta;

    // Check if this exact call has been repeated consecutively
    int repeatCount = 0;
    for (auto it = context.state.recentToolCallSignatures.rbegin();
         it != context.state.recentToolCallSignatures.rend(); ++it) {
      if (*it == signature) {
        repeatCount++;
      } else {
        break; // Only count consecutive repeats from the end
      }
    }

    if (repeatCount >= context.config.maxIdenticalToolCalls) {
      // Inject intervention nudge
      AgentTurn interventionTurn;
      interventionTurn.turnId =
          "insanity-nudge-" + std::to_string(context.history->turns.size());
      Message interventionMsg;
      interventionMsg.role = Role::User;
      interventionMsg.content.push_back(
          TextContent{"You are calling the same tool with identical arguments "
                      "repeatedly (" +
                      std::to_string(repeatCount + 1) +
                      " times). This indicates an insanity loop. " +
                      "Please stop and try a different approach. Do NOT repeat "
                      "this tool call."});
      auto nudgeNow = std::chrono::system_clock::now();
      interventionMsg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              nudgeNow.time_since_epoch())
              .count());
      interventionTurn.messages.push_back(interventionMsg);
      context.history->turns.push_back(interventionTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(interventionTurn);

      // Clear the recent signatures to allow recovery
      context.state.recentToolCallSignatures.clear();

      // Broadcast and return early (don't execute the repeated tool)
      onEvent(AgentTurnCompleted{context.identity.id, interventionTurn,
                                 context.aggregateMetrics,
                                 context.identity.parentId});
      return;
    }
  }

  AgentTurn toolResultTurn;
  toolResultTurn.turnId =
      "tools-" + std::to_string(context.history->turns.size());

  // Execute ALL tools in parallel using futures
  struct ToolExecution {
    std::string toolCallId;
    std::string name;
    std::string args;
    std::future<std::tuple<std::string, bool, std::string, std::string, bool>> future;
  };

  std::vector<ToolExecution> executions;

  // First pass: start all tool executions in parallel
  for (const auto &chunk : chunks) {
    if (interrupted.load())
      break;

    rapidjson::Document input;
    input.Parse(chunk.argsDelta.c_str());

    if (input.HasParseError()) {
      // Invalid JSON - create error result immediately
      Message msg;
      msg.role = Role::ToolResult;
      msg.content.push_back(ToolResultContent{
          chunk.id, "Invalid JSON arguments: " + chunk.argsDelta, false, "",
          ""});
      auto now = std::chrono::system_clock::now();
      msg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now.time_since_epoch())
              .count());
      toolResultTurn.messages.push_back(msg);
      continue;
    }

    // Capture chunk data for async execution
    std::string toolName = chunk.nameDelta;
    std::string toolArgs = chunk.argsDelta;
    std::string toolId = chunk.id;

    // Launch tool execution in async thread
    auto future = std::async(
        std::launch::async,
        [this, toolName, toolArgs,
         toolId]() -> std::tuple<std::string, bool, std::string, std::string, bool> {
          rapidjson::Document input;
          input.Parse(toolArgs.c_str());

          ToolContext toolCtx{*environment_->getHost(), *this, toolId};
          auto result = toolRegistry.execute(toolName, input, toolCtx);

          std::string resultStr;
          if (result.success) {
            resultStr = result.data;
          } else {
            resultStr = result.error;
          }

          return {resultStr, result.success, result.processId,
                  result.subagentId, result.is_background};
        });

    executions.push_back({toolId, toolName, toolArgs, std::move(future)});
  }

  // Second pass: collect all results (already running in parallel)
  for (auto &exec : executions) {
    if (interrupted.load())
      break;

    auto [resultStr, success, resultProcessId, resultSubagentId, isBackground] =
        exec.future.get();

    Message msg;
    msg.role = Role::ToolResult;
    msg.content.push_back(ToolResultContent{exec.toolCallId, resultStr, success,
                                            resultProcessId, resultSubagentId});
    auto now = std::chrono::system_clock::now();
    msg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    toolResultTurn.messages.push_back(msg);

    // Track edited files for file_edit and file_write tools
    if (success) {
      const bool owns_background_process =
          !resultProcessId.empty() &&
          (isBackground || exec.name == "process_spawn");
      if (owns_background_process &&
          std::find(context.state.ownedProcesses.begin(),
                    context.state.ownedProcesses.end(),
                    resultProcessId) == context.state.ownedProcesses.end()) {
        context.state.ownedProcesses.push_back(resultProcessId);
      }

      if (exec.name == "file_edit" || exec.name == "file_write") {
        rapidjson::Document input;
        input.Parse(exec.args.c_str());
        if (!input.HasParseError() && input.HasMember("path")) {
          std::string filePath = input["path"].GetString();
          if (std::find(context.state.editedFiles.begin(),
                        context.state.editedFiles.end(),
                        filePath) == context.state.editedFiles.end()) {
            context.state.editedFiles.push_back(filePath);
          }
          std::string actionDesc = "Edited file: " + filePath;
          context.state.completedActions.push_back(actionDesc);
        }
      }
    }

    // Track this tool call signature after successful execution
    std::string signature = exec.name + ":" + exec.args;
    context.state.recentToolCallSignatures.push_back(signature);
  }

  // Keep only last 20 signatures to prevent unbounded growth
  if (context.state.recentToolCallSignatures.size() > 20) {
    context.state.recentToolCallSignatures.erase(
        context.state.recentToolCallSignatures.begin(),
        context.state.recentToolCallSignatures.begin() +
            (context.state.recentToolCallSignatures.size() - 20));
  }

  context.history->turns.push_back(toolResultTurn);
  if (context.config.persistHistory && journaler)
    journaler->appendTurn(toolResultTurn);

  // Broadcast turn completion
  onEvent(AgentTurnCompleted{context.identity.id, toolResultTurn,
                             context.aggregateMetrics,
                             context.identity.parentId});
}

void Agent::saveHistory() {
  if (context.config.persistHistory && journaler) {
    journaler->rewriteJournal(context.history->turns);
  }
}

void Agent::compactContext(
    std::function<void(const shared::StreamEvent &)> onEvent) {
  context.state.currentStatus = AgentStatus::Compacting;
  onEvent(AgentCompacting{context.identity.id, context.identity.parentId});

  // Build factual state preamble to preserve actual work state
  std::string factualState = "\n## FACTUAL STATE (GROUND TRUTH)\n\n";

  if (!context.state.readFiles.empty()) {
    factualState += "**Files Read:** ";
    for (size_t i = 0; i < context.state.readFiles.size(); ++i) {
      factualState += context.state.readFiles[i];
      if (i < context.state.readFiles.size() - 1)
        factualState += ", ";
    }
    factualState += "\n\n";
  }

  if (!context.state.editedFiles.empty()) {
    factualState += "**Files Edited:** ";
    for (size_t i = 0; i < context.state.editedFiles.size(); ++i) {
      factualState += context.state.editedFiles[i];
      if (i < context.state.editedFiles.size() - 1)
        factualState += ", ";
    }
    factualState += "\n\n";
  }

  if (!context.state.completedActions.empty()) {
    factualState += "**Completed Actions:**\n";
    for (const auto &action : context.state.completedActions) {
      factualState += "- " + action + "\n";
    }
    factualState += "\n";
  }

  if (!context.state.ownedProcesses.empty()) {
    factualState += "**Active Background Processes:** ";
    for (size_t i = 0; i < context.state.ownedProcesses.size(); ++i) {
      factualState += context.state.ownedProcesses[i];
      if (i < context.state.ownedProcesses.size() - 1)
        factualState += ", ";
    }
    factualState += "\n\n";
  }

  if (context.state.fatalError.has_value()) {
    factualState +=
        "**Fatal Error:** " + context.state.fatalError.value() + "\n\n";
  }

  std::string compactionPrompt = PurposeLoader::loadCompactionPrompt();

  // Prepend factual state to compaction prompt
  std::string fullCompactionPrompt = factualState + compactionPrompt;
  std::string fullSummary;
  std::string fullThinking;

  if (interrupted.load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
    return;
  }

  AgentHistory historyToSummarize;
  historyToSummarize.threadId = context.history->threadId;
  
  std::vector<AgentTurn> preservedTurns;
  if (!context.history->turns.empty()) {
    preservedTurns.push_back(context.history->turns.front());
  }

  AgentTurn preservedLastTurn;
  bool hasPreservedLastTurn = false;
  if (context.history->turns.size() > 1) {
    auto& last = context.history->turns.back();
    if (last.messages.size() == 1 && last.messages[0].role == Role::User && last.turnId.find("user-task-") == 0) {
      preservedLastTurn = last;
      hasPreservedLastTurn = true;
    }
  }

  for (size_t i = 1; i < context.history->turns.size(); ++i) {
    if (hasPreservedLastTurn && i == context.history->turns.size() - 1) {
      break;
    }
    historyToSummarize.turns.push_back(context.history->turns[i]);
  }

  if (historyToSummarize.turns.empty()) {
    context.state.currentStatus = AgentStatus::Idle;
    return;
  }

  provider->generateSummary(
      context.config.modelId, historyToSummarize, fullCompactionPrompt,
      [&](const StreamEvent &ev) {
        if (interrupted.load()) {
          context.state.currentStatus = AgentStatus::Cancelled;
          return;
        }
        if (auto *act = std::get_if<AgentCompactionText>(&ev)) {
          fullSummary += act->delta;
          onEvent(AgentCompactionText{context.identity.id, act->delta,
                                      context.identity.parentId});
        } else if (auto *thk = std::get_if<AgentCompactionThinking>(&ev)) {
          fullThinking += thk->delta;
          onEvent(AgentCompactionThinking{context.identity.id, thk->delta,
                                          context.identity.parentId});
        } else {
          onEvent(ev);
        }
      },
      &interrupted);

  // Validate summary before clearing history
  if (fullSummary.empty()) {
    onEvent(StreamError{"Context compaction failed: Empty summary generated", 0,
                        ""});
    context.state.currentStatus = AgentStatus::Idle;
    return;
  }

  uint32_t oldTokens = context.aggregateMetrics.tokens.contextSize;

  // Rebuild history array
  std::vector<AgentTurn> newTurns;
  if (!preservedTurns.empty()) {
    newTurns.push_back(preservedTurns[0]);
  }

  // Create Synthetic Memory Turn
  AgentTurn summaryTurn;
  summaryTurn.turnId = "compaction-summary-" + std::to_string(nowMs());

  Message summaryMsg;
  summaryMsg.role = Role::User;
  if (!fullThinking.empty())
    summaryMsg.content.push_back(ThinkingContent{fullThinking, ""});
  summaryMsg.content.push_back(
      TextContent{"COMPACTION SUMMARY: " + fullSummary});
  summaryMsg.timestamp = nowMs();

  summaryTurn.messages.push_back(summaryMsg);
  newTurns.push_back(summaryTurn);

  if (hasPreservedLastTurn) {
    newTurns.push_back(preservedLastTurn);
  }

  context.history->turns = std::move(newTurns);

  if (context.config.persistHistory && journaler) {
    journaler->rewriteJournal(context.history->turns);
  }

  // Reset context size to conservative estimate (system + task + summary ~1000
  // tokens) This prevents immediate re-compaction on next turn
  context.aggregateMetrics.tokens.contextSize = 1000;

  uint32_t tokensSaved = (oldTokens > 1000) ? oldTokens - 1000 : 0;
  onEvent(ContextCompacted{context.identity.id, tokensSaved,
                           context.identity.parentId});
}

} // namespace firmius::core
