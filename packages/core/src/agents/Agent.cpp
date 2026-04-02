#include "agents/Agent.hpp"
#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "EnvLoader.hpp"
#include "Events.hpp"
#include "Message.hpp"
#include "Panic.hpp"
#include "Serialization.hpp"
#include "agents/PurposeLoader.hpp"
#include "agents/RuntimeOverlay.hpp"
#include "harness/Harness.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/LLMSearchProviderRegistry.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/FSUtil.hpp"
#include "utils/InterruptibleSleep.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

namespace firmius::core {

namespace {
constexpr std::uint32_t kMissingToolCallIndex =
    std::numeric_limits<std::uint32_t>::max();

// Maximum accumulated response/thinking buffer size per turn (500KB)
static constexpr std::size_t kMaxAccumulatedResponseBytes = 500 * 1024;
static constexpr std::size_t kMaxAccumulatedThinkingBytes = 500 * 1024;

bool hasToolCallIndex(const ToolCallChunk &chunk) {
  return chunk.index != kMissingToolCallIndex;
}

bool isValidJsonObjectPayload(const std::string &payload) {
  const std::string trimmed = shared::StringUtil::trim(payload);
  if (trimmed.empty()) {
    return false;
  }

  rapidjson::Document parsed;
  parsed.Parse(trimmed.c_str());
  return !parsed.HasParseError() && parsed.IsObject();
}

std::vector<std::string> extractFileEditPaths(const std::string &toolArgs) {
  std::vector<std::string> paths;
  rapidjson::Document input;
  input.Parse(toolArgs.c_str());
  if (input.HasParseError() || !input.IsObject()) {
    return paths;
  }

  if (input.HasMember("files") && input["files"].IsArray()) {
    for (const auto &entry : input["files"].GetArray()) {
      if (entry.IsObject() && entry.HasMember("path") &&
          entry["path"].IsString()) {
        paths.emplace_back(entry["path"].GetString());
      }
    }
    return paths;
  }

  if (input.HasMember("path") && input["path"].IsString()) {
    paths.emplace_back(input["path"].GetString());
  }
  return paths;
}

struct EditedFileEventPayload {
  std::string path;
  std::string diffPreview;
  int addedLines = 0;
  int removedLines = 0;
};

std::vector<EditedFileEventPayload>
extractFileEditEventPayloads(const std::string &toolArgs,
                             const std::string &resultStr) {
  std::vector<EditedFileEventPayload> payloads;

  rapidjson::Document resultDoc;
  resultDoc.Parse(resultStr.c_str());
  if (!resultDoc.HasParseError() && resultDoc.IsObject()) {
    auto appendFromObject = [&](const rapidjson::Value &value) {
      if (!value.IsObject() || !value.HasMember("path") ||
          !value["path"].IsString()) {
        return;
      }
      EditedFileEventPayload payload;
      payload.path = value["path"].GetString();
      if (value.HasMember("diff_preview") && value["diff_preview"].IsString()) {
        payload.diffPreview = value["diff_preview"].GetString();
      }
      if (value.HasMember("added_lines") && value["added_lines"].IsInt()) {
        payload.addedLines = value["added_lines"].GetInt();
      }
      if (value.HasMember("removed_lines") && value["removed_lines"].IsInt()) {
        payload.removedLines = value["removed_lines"].GetInt();
      }
      payloads.push_back(std::move(payload));
    };

    if (resultDoc.HasMember("files") && resultDoc["files"].IsArray()) {
      for (const auto &entry : resultDoc["files"].GetArray()) {
        appendFromObject(entry);
      }
    } else {
      appendFromObject(resultDoc);
    }
  }

  if (!payloads.empty()) {
    return payloads;
  }

  for (const auto &path : extractFileEditPaths(toolArgs)) {
    payloads.push_back(EditedFileEventPayload{path, "", 0, 0});
  }
  return payloads;
}

void mergeToolCallName(std::string &existing, const std::string &incoming) {
  if (incoming.empty()) {
    return;
  }
  if (existing.empty()) {
    existing = incoming;
    return;
  }
  if (incoming == existing) {
    return;
  }
  if (incoming.rfind(existing, 0) == 0) {
    existing = incoming;
    return;
  }
  if (existing.rfind(incoming, 0) == 0) {
    return;
  }
  existing += incoming;
}

void mergeToolCallArgs(std::string &existing, const std::string &incoming) {
  if (incoming.empty()) {
    return;
  }
  if (existing.empty()) {
    existing = incoming;
    return;
  }
  if (incoming == existing) {
    return;
  }

  if (isValidJsonObjectPayload(incoming)) {
    existing = incoming;
    return;
  }

  existing += incoming;
}

std::vector<ToolCallChunk>::iterator
findMatchingToolCallChunk(std::vector<ToolCallChunk> &accumulated,
                          const ToolCallChunk &incoming) {
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
        accumulated.begin(), accumulated.end(),
        [&](const ToolCallChunk &existing) {
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
  mergeToolCallName(it->nameDelta, incoming.nameDelta);
  mergeToolCallArgs(it->argsDelta, incoming.argsDelta);
}

bool shouldRetryProviderFailureAtAgentLayer(int httpStatus) {
  if (httpStatus == -1 || httpStatus == 0 || httpStatus >= 500) {
    return true;
  }
  return httpStatus == 408 || httpStatus == 409 || httpStatus == 425;
}

std::string appendProviderModelContext(const AgentConfig &config,
                                       const std::string &message) {
  std::string detailed = message;
  if (!config.providerId.empty() &&
      detailed.find("\nProvider: ") == std::string::npos) {
    detailed += "\nProvider: " + config.providerId;
  }

  if (!config.modelId.empty() &&
      detailed.find("\nModel: ") == std::string::npos) {
    detailed += "\nModel: " + config.modelId;
  }

  if (!config.modelVariant.empty() &&
      detailed.find("\nVariant: ") == std::string::npos) {
    detailed += "\nVariant: " + config.modelVariant;
  }
  return detailed;
}

void saveCompactionSnapshot(const std::string &threadId,
                            const std::string &agentId,
                            const std::string &compactionId,
                            std::uint32_t previousContextSize,
                            const std::vector<AgentTurn> &turns) {
  if (threadId.empty() || agentId.empty() || compactionId.empty()) {
    return;
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  CompactionSnapshot snapshot;
  snapshot.compactionId = compactionId;
  snapshot.threadId = threadId;
  snapshot.agentId = agentId;
  snapshot.previousContextSize = previousContextSize;
  snapshot.createdAt = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  snapshot.turns = turns;
  tm.appendCompactionSnapshot(threadId, agentId, snapshot);
}

std::string buildPlanAndTodoSnapshot(const AgentContext &context) {
  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty()) {
    return "";
  }

  std::ostringstream state;
  auto planStatusLabel = [](PlanStatus status) -> const char * {
    switch (status) {
    case PlanStatus::Draft:
      return "Draft";
    case PlanStatus::Active:
      return "Active";
    case PlanStatus::Paused:
      return "Paused";
    case PlanStatus::Done:
      return "Done";
    case PlanStatus::Abandoned:
      return "Abandoned";
    }
    return "Unknown";
  };
  auto chunkStatusLabel = [](WorkChunkStatus status) -> const char * {
    switch (status) {
    case WorkChunkStatus::Ready:
      return "Ready";
    case WorkChunkStatus::InProgress:
      return "InProgress";
    case WorkChunkStatus::Implemented:
      return "Implemented";
    case WorkChunkStatus::Verifying:
      return "Verifying";
    case WorkChunkStatus::Done:
      return "Done";
    case WorkChunkStatus::Blocked:
      return "Blocked";
    case WorkChunkStatus::Failed:
      return "Failed";
    case WorkChunkStatus::Cancelled:
      return "Cancelled";
    }
    return "Unknown";
  };
  auto todoStatusLabel = [](TodoStatus status) -> const char * {
    switch (status) {
    case TodoStatus::Pending:
      return "Pending";
    case TodoStatus::InProgress:
      return "InProgress";
    case TodoStatus::Done:
      return "Done";
    }
    return "Unknown";
  };
  auto trimForPrompt = [](const std::string &value, std::size_t maxLen) {
    const std::string trimmed = shared::StringUtil::trim(value);
    if (trimmed.size() <= maxLen) {
      return trimmed;
    }
    return trimmed.substr(0, maxLen) + "...";
  };
  try {
    ThreadManager tm(ThreadManager::defaultBasePath());
    const ThreadMetadata metadata = tm.getMetadata(context.history->threadId);
    if (!metadata.activePlanId.empty()) {
      state << "**Active Plan ID:** " << metadata.activePlanId << "\n";
      try {
        const Plan plan =
            tm.getPlan(context.history->threadId, metadata.activePlanId);
        state << "**Active Plan Title:** " << plan.title << "\n";
        state << "**Active Plan Status:** " << planStatusLabel(plan.status)
              << "\n";
        if (!shared::StringUtil::trim(plan.objective).empty()) {
          state << "**Plan Objective:** " << trimForPrompt(plan.objective, 220)
                << "\n";
        }
        if (!shared::StringUtil::trim(plan.strategy).empty()) {
          state << "**Plan Strategy:** " << trimForPrompt(plan.strategy, 220)
                << "\n";
        }
        int incompleteChunks = 0;
        for (const auto &chunk : plan.chunks) {
          if (chunk.status != WorkChunkStatus::Done &&
              chunk.status != WorkChunkStatus::Cancelled) {
            incompleteChunks++;
          }
        }
        state << "**Incomplete Chunks:** " << incompleteChunks << "\n";
        if (!plan.chunks.empty()) {
          constexpr std::size_t kMaxChunks = 8;
          constexpr std::size_t kMaxTasksPerChunk = 4;
          state << "**Chunk Ledger:**\n";
          for (std::size_t i = 0; i < plan.chunks.size() && i < kMaxChunks;
               ++i) {
            const auto &chunk = plan.chunks[i];
            state << "- [" << chunkStatusLabel(chunk.status) << "] "
                  << trimForPrompt(chunk.title, 160);
            if (!chunk.id.empty()) {
              state << " (id=" << chunk.id << ")";
            }
            if (!chunk.assignedAgentId.empty()) {
              state << " assignee=" << chunk.assignedAgentId;
            }
            state << "\n";
            if (!shared::StringUtil::trim(chunk.goal).empty()) {
              state << "  goal: " << trimForPrompt(chunk.goal, 220) << "\n";
            }
            for (std::size_t taskIndex = 0; taskIndex < chunk.tasks.size() &&
                                            taskIndex < kMaxTasksPerChunk;
                 ++taskIndex) {
              const auto &task = chunk.tasks[taskIndex];
              state << "  task[" << chunkStatusLabel(task.status)
                    << "]: " << trimForPrompt(task.title, 160) << "\n";
            }
            if (chunk.tasks.size() > kMaxTasksPerChunk) {
              state << "  ... " << (chunk.tasks.size() - kMaxTasksPerChunk)
                    << " additional task(s)\n";
            }
          }
          if (plan.chunks.size() > kMaxChunks) {
            state << "- ... " << (plan.chunks.size() - kMaxChunks)
                  << " additional chunk(s)\n";
          }
        }
      } catch (...) {
        state << "**Active Plan:** unavailable\n";
      }
    }

    const AgentTodoList todo =
        tm.getAgentTodo(context.history->threadId, context.identity.id);
    if (!todo.items.empty()) {
      int incompleteTodo = 0;
      for (const auto &item : todo.items) {
        if (item.status != TodoStatus::Done) {
          incompleteTodo++;
        }
      }
      state << "**Todo Items:** " << todo.items.size() << "\n";
      state << "**Todo Incomplete:** " << incompleteTodo << "\n";
      constexpr std::size_t kMaxTodoItems = 12;
      state << "**Todo Ledger:**\n";
      for (std::size_t i = 0; i < todo.items.size() && i < kMaxTodoItems; ++i) {
        const auto &item = todo.items[i];
        state << "- (#" << item.id << ") [" << todoStatusLabel(item.status)
              << "] " << trimForPrompt(item.text, 220);
        if (!item.chunkId.empty()) {
          state << " chunk=" << item.chunkId;
        }
        if (!item.planId.empty()) {
          state << " plan=" << item.planId;
        }
        state << "\n";
      }
      if (todo.items.size() > kMaxTodoItems) {
        state << "- ... " << (todo.items.size() - kMaxTodoItems)
              << " additional todo item(s)\n";
      }
    }
  } catch (...) {
  }

  return state.str();
}

struct TodoStateSnapshot {
  bool hasAny = false;
  bool hasIncomplete = false;
  std::vector<TodoItem> incompleteItems;
};

std::string todoContinuationFingerprint(const TodoStateSnapshot &todoState) {
  std::ostringstream fingerprint;
  for (const auto &item : todoState.incompleteItems) {
    fingerprint << item.id << '\n'
                << static_cast<int>(item.status) << '\n'
                << item.text << '\n'
                << item.chunkId << '\n'
                << item.planId << "\n---\n";
  }
  return fingerprint.str();
}

std::string todoStatusLabel(TodoStatus status) {
  switch (status) {
  case TodoStatus::Pending:
    return "Pending";
  case TodoStatus::InProgress:
    return "InProgress";
  case TodoStatus::Done:
    return "Done";
  }
  return "Unknown";
}

std::string buildIncompleteTodoNudge(const TodoStateSnapshot &todoState) {
  std::ostringstream prompt;
  prompt << "Continue working through the remaining todo items: ";
  for (std::size_t i = 0; i < todoState.incompleteItems.size(); ++i) {
    const auto &item = todoState.incompleteItems[i];
    if (i > 0) {
      prompt << ", ";
    }
    prompt << "#" << item.id << " [" << todoStatusLabel(item.status) << "] ";
    if (!shared::StringUtil::trim(item.text).empty()) {
      prompt << item.text;
    } else {
      prompt << "(no text)";
    }
  }
  return prompt.str();
}

std::string buildEmptyProviderRetryNudge(int attempt) {
  std::ostringstream prompt;
  prompt << "The provider returned no visible reply, no thinking, and no tool "
            "calls. Continue the current task. Either provide the next useful "
            "response or call the next tool.";
  if (attempt > 1) {
    prompt << " This is empty response retry " << attempt << ".";
  }
  return prompt.str();
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
    ThreadManager tm(ThreadManager::defaultBasePath());
    const AgentTodoList todo =
        tm.getAgentTodo(context.history->threadId, context.identity.id);
    snapshot.hasAny = !todo.items.empty();
    for (const auto &item : todo.items) {
      if (item.status != TodoStatus::Done) {
        snapshot.hasIncomplete = true;
        snapshot.incompleteItems.push_back(item);
      }
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
      failures.push_back({chunk.id, "tool arguments for '" + chunk.nameDelta +
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
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

AgentTurn Agent::makeInternalNudgeTurn(const std::string &turnIdPrefix,
                                       const std::string &text,
                                       Role role) const {
  AgentTurn nudgeTurn;
  nudgeTurn.turnId =
      turnIdPrefix +
      std::to_string(context.history ? context.history->turns.size() : 0);

  Message nudgeMsg;
  nudgeMsg.role = role;
  nudgeMsg.visibility = MessageVisibility::Internal;
  nudgeMsg.content.push_back(TextContent{text});
  auto now = std::chrono::system_clock::now();
  nudgeMsg.timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count());
  nudgeTurn.messages.push_back(std::move(nudgeMsg));
  return nudgeTurn;
}

void Agent::appendTurnToHistory(const AgentTurn &turn) {
  context.history->turns.push_back(turn);
  if (context.config.persistHistory && journaler) {
    journaler->appendTurn(turn);
  }
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
  if (!provider) {
    auto preferred = getPreferredModel();
    provider = firmius::provider::ProviderRegistry::instance().getProvider(
        preferred.providerId);
    if (!provider) {
      throw std::runtime_error(
          "Unknown provider: " + context.config.providerId +
          " (Fallback failed: " + preferred.providerId + ")");
    }
    context.config.providerId = preferred.providerId;
    context.config.modelId = preferred.modelId;
    if (preferred.variantName) {
      context.config.modelVariant = *preferred.variantName;
    }
  }
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
  {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    activeRunCancelToken_.reset();
    activeRunAbortController_.reset();
  }

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

std::shared_ptr<shared::IHost> Agent::getHost() {
  return environment_->getHost();
}

ModelChoice Agent::getPreferredModel() const {
  const auto &config = shared::ConfigLoader::instance().getConfig();
  auto persona = context.config.personaName;

  auto useDefaultRoute = [&config]() {
    ModelChoice choice;
    choice.providerId = config.defaultProviderId;
    choice.modelId = config.defaultModelId;
    if (!config.defaultModelVariant.empty()) {
      choice.variantName = config.defaultModelVariant;
    }
    return choice;
  };

  auto findCategory =
      [&config](const std::string &name) -> const shared::ModelRouteCategory * {
    auto it = config.modelRouterCategories.find(name);
    if (it == config.modelRouterCategories.end()) {
      return nullptr;
    }
    return &it->second;
  };

  auto it_purpose = config.purposeRoutes.find(persona);
  if (it_purpose != config.purposeRoutes.end() && !it_purpose->second.empty()) {
    const std::string mapped_category = it_purpose->second;
    if (const auto *category = findCategory(mapped_category)) {
      ModelChoice choice;
      choice.providerId = category->providerId;
      choice.modelId = category->modelId;
      if (!category->variantName.empty()) {
        choice.variantName = category->variantName;
      }
      return choice;
    }
  }

  if (!config.defaultRouteCategory.empty()) {
    if (const auto *category = findCategory(config.defaultRouteCategory)) {
      ModelChoice choice;
      choice.providerId = category->providerId;
      choice.modelId = category->modelId;
      if (!category->variantName.empty()) {
        choice.variantName = category->variantName;
      }
      return choice;
    }
  }

  return useDefaultRoute();
}

void Agent::interrupt() {
  interrupted = true;
  std::shared_ptr<std::atomic<bool>> runCancelToken;
  std::shared_ptr<shared::AbortController> runAbortController;
  {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    runCancelToken = activeRunCancelToken_;
    runAbortController = activeRunAbortController_;
  }
  if (runCancelToken) {
    runCancelToken->store(true);
  }
  if (runAbortController) {
    runAbortController->cancel();
  }
}

void Agent::clearInterrupt() { interrupted = false; }

void Agent::setModel(const std::string &providerId,
                     const std::string &modelId) {
  setModelInternal(providerId, modelId, std::nullopt);
}

void Agent::compactNow(
    std::function<void(const shared::StreamEvent &)> onEvent) {
  compactContext(std::move(onEvent));
}

void Agent::setModel(const std::string &providerId, const std::string &modelId,
                     const std::string &variantName) {
  setModelInternal(providerId, modelId, variantName);
}

void Agent::setModelInternal(const std::string &providerId,
                             const std::string &modelId,
                             const std::optional<std::string> &variantName) {
  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(providerId);
  if (!newProvider) {
    auto preferred = getPreferredModel();
    newProvider = firmius::provider::ProviderRegistry::instance().getProvider(
        preferred.providerId);
    if (!newProvider) {
      throw std::runtime_error("Unknown provider: " + providerId +
                               " (Fallback failed: " + preferred.providerId +
                               ")");
    }
    std::lock_guard<std::mutex> lock(modelSwitchMutex);
    if (running.load()) {
      pendingModelSwitch_ = PendingModelSwitch{
          preferred.providerId, preferred.modelId, preferred.variantName};
      return;
    }
    context.config.providerId = preferred.providerId;
    context.config.modelId = preferred.modelId;
    context.config.modelVariant = preferred.variantName.value_or("");
    provider = newProvider;
    return;
  }

  std::lock_guard<std::mutex> lock(modelSwitchMutex);
  if (running.load()) {
    pendingModelSwitch_ = PendingModelSwitch{providerId, modelId, variantName};
    return;
  }

  context.config.providerId = providerId;
  context.config.modelId = modelId;
  if (variantName.has_value()) {
    context.config.modelVariant = *variantName;
  }
  provider = newProvider;
}

void Agent::applyPendingModelSwitchIfAny() {
  std::optional<PendingModelSwitch> pending;
  {
    std::lock_guard<std::mutex> lock(modelSwitchMutex);
    if (!pendingModelSwitch_.has_value()) {
      return;
    }
    pending = pendingModelSwitch_;
    pendingModelSwitch_.reset();
  }

  auto newProvider =
      firmius::provider::ProviderRegistry::instance().getProvider(
          pending->providerId);
  if (!newProvider) {
    throw std::runtime_error("Unknown provider: " + pending->providerId);
  }

  context.config.providerId = pending->providerId;
  context.config.modelId = pending->modelId;
  if (pending->variantName.has_value()) {
    context.config.modelVariant = *pending->variantName;
  }
  provider = newProvider;
}

std::string Agent::spawnProcess(const std::string &command,
                                const std::string &toolCallId,
                                const std::string &cwd,
                                const std::map<std::string, std::string> &env,
                                bool monitorCompletion) {
  return environment_->getProcessManager().spawnProcess(
      command, toolCallId, cwd, env, monitorCompletion);
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
  std::lock_guard<std::mutex> lock(runMutex_);
  if (running.load()) {
    throw std::runtime_error("Agent is already running");
  }
  running = true;
  interrupted = false;
  const auto runCancelToken = std::make_shared<std::atomic<bool>>(false);
  const auto runAbortController = std::make_shared<shared::AbortController>();
  {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    activeRunCancelToken_ = runCancelToken;
    activeRunAbortController_ = runAbortController;
  }
  auto markRunStopped = [this, runCancelToken, runAbortController]() {
    {
      std::lock_guard<std::mutex> runStateLock(runStateMutex_);
      running = false;
    }
    runStateCv_.notify_all();
    {
      std::lock_guard<std::mutex> cancelLock(cancelTokenMutex_);
      if (activeRunCancelToken_ == runCancelToken) {
        activeRunCancelToken_.reset();
      }
      if (activeRunAbortController_ == runAbortController) {
        activeRunAbortController_.reset();
      }
    }
  };
  struct RunFinalizer {
    std::function<void()> fn;
    ~RunFinalizer() {
      if (fn) {
        fn();
      }
    }
  } runFinalizer{markRunStopped};
  booting = false;
  context.state.currentStatus = AgentStatus::ProviderWaiting;
  context.state.fatalError = std::nullopt;
  applyPendingModelSwitchIfAny();

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
  bool taskFinished = false;
  int maxTurns = context.config.maxTurns > 0 ? context.config.maxTurns : 200;
  int turnCount = 0;
  int consecutiveProviderFailures = 0;
  const int maxProviderRetries = 3;
  int consecutiveEmptyProviderResponses = 0;
  const int maxEmptyProviderRetries = 2;
  std::optional<std::string> lastTodoContinuationFingerprint;
  auto hasQueuedUserTurnPending = [this]() {
    if (!context.history || context.history->turns.empty()) {
      return false;
    }
    const auto &lastTurn = context.history->turns.back();
    return lastTurn.turnId.rfind("user-task-", 0) == 0 &&
           !lastTurn.messages.empty() &&
           lastTurn.messages.front().role == Role::User;
  };
  while (!taskFinished && turnCount < maxTurns && !runCancelToken->load()) {
    applyPendingModelSwitchIfAny();

    // --- CHECK FOR CONTEXT COMPACTION ---
    try {
      auto model = provider->getModelInfo(context.config.modelId);
      bool forceCompact = (std::getenv("FORCE_COMPACTION") != nullptr);
      if (forceCompact || (model.contextWindow > 0 &&
                           context.aggregateMetrics.tokens.contextSize >
                               model.contextWindow * 0.8)) {
        compactContext(onEvent);
      }
      if (runCancelToken->load())
        break;
    } catch (...) {
      // Compaction is best-effort
    }

    // Drain any pending internal queue messages (e.g., fleet edit notices)
    // at the start of each turn, so the agent sees them before its next action.
    {
      std::string tid = context.history->threadId;
      std::string aid = context.identity.id;
      if (!tid.empty() && !aid.empty()) {
        Harness::instance().drainInternalQueueForAgent(aid, tid, true);
      }
    }

    turnCount++;

    std::vector<ToolCallChunk> accumulatedToolChunks;
    std::string fullResponse;
    std::string fullThinking;
    bool responseTruncated = false;
    bool thinkingTruncated = false;
    std::string lastThinkingSignature;
    AgentMetrics turnMetrics;
    StopReason turnStopReason = StopReason::Stop;
    std::string streamError;
    int streamErrorStatus = 0;
    bool sawContent = false;
    bool sawThinking = false;
    bool sawTool = false;
    std::uint32_t syntheticToolCallIdSerial = 0;

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
      if (!responseTruncated &&
          fullResponse.size() + delta.size() > kMaxAccumulatedResponseBytes) {
        std::size_t remaining = kMaxAccumulatedResponseBytes - fullResponse.size();
        if (remaining > 0) {
          fullResponse.append(delta, 0, remaining);
        }
        responseTruncated = true;
        std::cerr << "[FIRMIUS] Response buffer capped at "
                  << kMaxAccumulatedResponseBytes << " bytes for turn "
                  << turnCount << std::endl;
      } else if (!responseTruncated) {
        fullResponse += delta;
      }
    };

    auto appendVisibleThinking = [&](const std::string &delta) {
      if (delta.empty()) {
        return;
      }
      onEvent(ThinkingChunk{delta, ""});
      sawThinking = true;
      if (!thinkingTruncated &&
          fullThinking.size() + delta.size() > kMaxAccumulatedThinkingBytes) {
        std::size_t remaining = kMaxAccumulatedThinkingBytes - fullThinking.size();
        if (remaining > 0) {
          fullThinking.append(delta, 0, remaining);
        }
        thinkingTruncated = true;
        std::cerr << "[FIRMIUS] Thinking buffer capped at "
                  << kMaxAccumulatedThinkingBytes << " bytes for turn "
                  << turnCount << std::endl;
      } else if (!thinkingTruncated) {
        fullThinking += delta;
      }
    };

    auto persistAssistantTurn = [&](StopReason stopReason,
                                    bool includeToolCalls) {
      const bool hasVisibleContent =
          !fullThinking.empty() || !fullResponse.empty();
      const bool hasPersistableToolCalls =
          includeToolCalls && !accumulatedToolChunks.empty();
      if (!hasVisibleContent && !hasPersistableToolCalls) {
        return false;
      }

      AgentTurn assistantTurn;
      assistantTurn.turnId =
          "assistant-" + std::to_string(context.history->turns.size());
      assistantTurn.stopReason = stopReason;
      assistantTurn.metrics = turnMetrics;

      context.aggregateMetrics += turnMetrics;

      Message assistantMsg;
      assistantMsg.role = Role::Assistant;
      if (!fullThinking.empty()) {
        assistantMsg.content.push_back(
            ThinkingContent{fullThinking, lastThinkingSignature});
      }
      if (!fullResponse.empty()) {
        assistantMsg.content.push_back(TextContent{fullResponse});
      }
      if (hasPersistableToolCalls) {
        for (const auto &chunk : accumulatedToolChunks) {
          assistantMsg.content.push_back(
              ToolCallContent{chunk.id, chunk.nameDelta, chunk.argsDelta});
        }
      }

      assistantMsg.timestamp = nowMs();
      assistantTurn.messages.push_back(assistantMsg);
      context.history->turns.push_back(assistantTurn);
      if (context.config.persistHistory && journaler) {
        journaler->appendTurn(assistantTurn);
      }

      onEvent(AgentTurnCompleted{context.identity.id, assistantTurn,
                                 context.aggregateMetrics,
                                 context.identity.parentId});
      return true;
    };

    auto persistCancelledToolResults =
        [&](const std::vector<ToolCallChunk> &chunks,
            const std::string &message) {
          if (chunks.empty()) {
            return;
          }

          AgentTurn toolResultTurn;
          toolResultTurn.turnId =
              "tools-" + std::to_string(context.history->turns.size());
          for (const auto &chunk : chunks) {
            Message msg;
            msg.role = Role::ToolResult;
            msg.content.push_back(
                ToolResultContent{chunk.id, message, false, "", ""});
            msg.timestamp = nowMs();
            toolResultTurn.messages.push_back(std::move(msg));
          }

          context.history->turns.push_back(toolResultTurn);
          if (context.config.persistHistory && journaler) {
            journaler->appendTurn(toolResultTurn);
          }
          onEvent(AgentTurnCompleted{context.identity.id, toolResultTurn,
                                     context.aggregateMetrics,
                                     context.identity.parentId});
        };

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
      opts.abortSignal = runCancelToken.get();
      opts.abortController = runAbortController;

      const AgentHistory requestHistory =
          runtime_overlay::buildRequestHistoryWithRuntimeOverlays(
              context, *environment_->getHost(), environment_->getWorkspace());
      provider->stream(requestHistory, opts, [&](const StreamEvent &ev) {
        if (context.state.currentStatus == AgentStatus::ProviderWaiting) {
          if (std::holds_alternative<TextChunk>(ev) ||
              std::holds_alternative<ThinkingChunk>(ev) ||
              std::holds_alternative<ToolCallChunk>(ev)) {
            context.state.currentStatus = AgentStatus::Streaming;
          }
        }

        if (auto *txt = std::get_if<TextChunk>(&ev)) {
          appendVisibleText(txt->delta);
        } else if (auto *thk = std::get_if<ThinkingChunk>(&ev)) {
          appendVisibleThinking(thk->delta);
          if (!thk->signature.empty()) {
            lastThinkingSignature = thk->signature;
          }
        } else if (auto *tcc = std::get_if<ToolCallChunk>(&ev)) {
          sawTool = true;
          // Emit immediately so TUI can show "Preparing" state
          onEvent(ev);
          mergeToolCallChunk(accumulatedToolChunks, *tcc,
                             syntheticToolCallIdSerial++, turnCount);
        } else if (auto *met = std::get_if<AgentMetrics>(&ev)) {
          onEvent(ev);
          turnMetrics = *met;
        } else if (auto *done = std::get_if<StreamDone>(&ev)) {
          onEvent(ev);
          turnStopReason = done->reason;
        } else if (auto *err = std::get_if<StreamError>(&ev)) {
          streamErrorStatus = err->httpStatus;
          if (runCancelToken->load()) {
            context.state.currentStatus = AgentStatus::Cancelled;
            return;
          }
          onEvent(ev);
          streamError = err->message;
        } else {
          onEvent(ev);
        }
      });
      if (runCancelToken->load()) {
        persistAssistantTurn(StopReason::Cancelled, false);
        context.state.currentStatus = AgentStatus::Cancelled;
        break;
      }

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

      // If there was a stream error and no content came back, retry
      if (!streamError.empty() && fullResponse.empty() &&
          accumulatedToolChunks.empty()) {
        // Don't retry if user interrupted
        if (runCancelToken->load()) {
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
                               streamErrorStatus, retryDelaySec * 1000,
                               "Provider error, retrying", "", ""});
        // Use interruptible sleep to allow immediate cancellation
        if (!interruptibleSleep(std::chrono::seconds(retryDelaySec),
                                runAbortController, runCancelToken.get())) {
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
          error << "["
                << (failure.toolCallId.empty() ? "unknown" : failure.toolCallId)
                << "] " << failure.message;
        }
        throw std::runtime_error(error.str());
      }

      // Reset consecutive failure counter on success
      consecutiveProviderFailures = 0;

      if (runCancelToken->load()) {
        persistAssistantTurn(StopReason::Cancelled, false);
        context.state.currentStatus = AgentStatus::Cancelled;
        break;
      }

      // --- Build assistant turn ---
      persistAssistantTurn(turnStopReason, true);

      if (hasQueuedUserTurnPending()) {
        consecutiveEmptyProviderResponses = 0;
        context.state.currentStatus = AgentStatus::ProviderWaiting;
        onEvent(ProviderWaiting{});
        continue;
      }

      // --- Check for termination ---
      if (accumulatedToolChunks.empty()) {
        const bool emptyAssistantReply = fullResponse.empty();
        if (emptyAssistantReply) {
          consecutiveEmptyProviderResponses++;
        } else {
          consecutiveEmptyProviderResponses = 0;
        }

        // Don't error if user interrupted
        if (runCancelToken->load()) {
          context.state.currentStatus = AgentStatus::Cancelled;
          return;
        }

        const bool hasPendingToolCalls =
            !context.state.pendingToolCalls.empty();
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

        const bool hasPendingToolLifecycleActivity =
            hasPendingToolCalls || hasExecutionStatus;
        const bool hasHarnessOwnedActiveWork =
            hasPendingToolLifecycleActivity || hasBlockingProcesses ||
            hasRunningOwnedBackgroundProcess || hasRunningDescendantSubagent;
        if (hasHarnessOwnedActiveWork) {
          consecutiveEmptyProviderResponses = 0;
          context.state.currentStatus = AgentStatus::Idle;
          break; // Yield loop to wait for active work to complete
        }

        const TodoStateSnapshot todoState = readTodoState(context);
        if (todoState.hasIncomplete) {
          const std::string fingerprint =
              todoContinuationFingerprint(todoState);
          const bool repeatedTodoSnapshot =
              lastTodoContinuationFingerprint.has_value() &&
              *lastTodoContinuationFingerprint == fingerprint;

          std::string nudgeMessage = buildIncompleteTodoNudge(todoState);
          if (repeatedTodoSnapshot) {
            nudgeMessage = "CRITICAL: You have incomplete items on your todo "
                           "list but you stopped. You MUST use tools to make "
                           "progress, or mark them as done/cancelled using the "
                           "todo_write tool. Do not ignore your todo list.";
          }

          appendTurnToHistory(
              makeInternalNudgeTurn("todo-enforcement-", nudgeMessage));
          lastTodoContinuationFingerprint = fingerprint;
          continue; // Start a new iteration with the nudge turn in history
        }

        if (emptyAssistantReply) {
          if (consecutiveEmptyProviderResponses <= maxEmptyProviderRetries) {
            appendTurnToHistory(
                makeInternalNudgeTurn("empty-provider-retry-",
                                      buildEmptyProviderRetryNudge(
                                          consecutiveEmptyProviderResponses)));
            continue;
          }

          throw std::runtime_error(
              "Provider returned an empty response with no tool calls after " +
              std::to_string(consecutiveEmptyProviderResponses) + " attempts.");
        }

        lastTodoContinuationFingerprint.reset();
        taskFinished = true;
      } else {
        consecutiveEmptyProviderResponses = 0;
        // --- State: ExecutingTool ---
        context.state.currentStatus = AgentStatus::ExecutingTool;

        // Track pending tool calls
        for (const auto &chunk : accumulatedToolChunks) {
          context.state.pendingToolCalls.push_back(chunk.id);
        }

        auto toolStartMs = nowMs();
        if (runCancelToken->load()) {
          persistCancelledToolResults(accumulatedToolChunks,
                                      "User aborted tool manually.");
          context.state.pendingToolCalls.clear();
          context.state.currentStatus = AgentStatus::Cancelled;
          break;
        }
        executeTools(accumulatedToolChunks, onEvent, runCancelToken);
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

        const bool hasPendingToolCalls =
            !context.state.pendingToolCalls.empty();
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

        if (hasPendingToolCalls || hasExecutionStatus || hasBlockingProcesses ||
            hasRunningOwnedBackgroundProcess || hasRunningDescendantSubagent) {
          context.state.currentStatus = AgentStatus::ExecutingTool;
          continue;
        }

        if (hasQueuedUserTurnPending()) {
          consecutiveEmptyProviderResponses = 0;
          context.state.currentStatus = AgentStatus::ProviderWaiting;
          onEvent(ProviderWaiting{});
          continue;
        }

        const TodoStateSnapshot todoState = readTodoState(context);
        if (todoState.hasIncomplete) {
          const std::string fingerprint =
              todoContinuationFingerprint(todoState);
          const bool repeatedTodoSnapshot =
              lastTodoContinuationFingerprint.has_value() &&
              *lastTodoContinuationFingerprint == fingerprint;

          std::string nudgeMessage = buildIncompleteTodoNudge(todoState);
          if (repeatedTodoSnapshot) {
            nudgeMessage = "CRITICAL: You have incomplete items on your todo "
                           "list but you stopped. You MUST use tools to make "
                           "progress, or mark them as done/cancelled using the "
                           "todo_write tool. Do not ignore your todo list.";
          }

          appendTurnToHistory(
              makeInternalNudgeTurn("todo-enforcement-", nudgeMessage));
          lastTodoContinuationFingerprint = fingerprint;
          context.state.currentStatus = AgentStatus::ExecutingTool;
          continue;
        }
        lastTodoContinuationFingerprint.reset();
      }

    } catch (const std::exception &e) {
      if (runCancelToken->load()) {
        context.state.currentStatus = AgentStatus::Cancelled;
        break;
      }
      const std::string detailedError =
          appendProviderModelContext(context.config, e.what());
      // --- State: Error ---
      context.state.currentStatus = AgentStatus::Error;
      context.state.fatalError = detailedError;

      // Persist error as a system turn in history for journal survival
      AgentTurn errorTurn;
      errorTurn.turnId =
          "error-" + std::to_string(context.history->turns.size());
      Message errorMsg;
      errorMsg.role = Role::Error;
      errorMsg.content.push_back(ErrorContent{
          "Agent Runtime Error",
          "The agent encountered a fatal runtime exception.", detailedError});
      errorMsg.timestamp = nowMs();
      errorTurn.messages.push_back(errorMsg);
      context.history->turns.push_back(errorTurn);
      if (context.config.persistHistory && journaler)
        journaler->appendTurn(errorTurn);

      // Emit error as a StreamError event
      onEvent(StreamError{detailedError, 0, ""});
      break;
    }
  }

  // --- Final state ---
  if (runCancelToken->load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
  } else if (context.state.currentStatus != AgentStatus::Error) {
    context.state.currentStatus = AgentStatus::Idle;
  }
}

void Agent::executeTools(
    const std::vector<ToolCallChunk> &chunks,
    std::function<void(const StreamEvent &)> onEvent,
    const std::shared_ptr<std::atomic<bool>> &runCancelToken) {
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
      AgentTurn interventionTurn = makeInternalNudgeTurn(
          "insanity-nudge-",
          "You are calling the same tool with identical arguments repeatedly "
          "(" +
              std::to_string(repeatCount + 1) +
              " times). This indicates an insanity loop. "
              "Please stop and try a different approach. Do NOT repeat this "
              "tool call.");
      appendTurnToHistory(interventionTurn);

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

  struct ToolExecutionResult {
    std::size_t index = 0;
    std::string toolCallId;
    std::string toolName;
    std::string toolArgs;
    std::string resultStr;
    bool success = false;
    std::string resultProcessId;
    std::string resultSubagentId;
    bool isBackground = false;
  };
  struct SharedExecutionState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::optional<ToolExecutionResult>> results;
    std::size_t completed = 0;
  };

  struct RunnableChunk {
    ToolCallChunk chunk;
    std::size_t index = 0;
  };
  std::vector<ToolExecutionResult> immediateResults;
  std::vector<RunnableChunk> runnableChunks;
  runnableChunks.reserve(chunks.size());
  for (std::size_t idx = 0; idx < chunks.size(); ++idx) {
    const auto &chunk = chunks[idx];
    rapidjson::Document input;
    input.Parse(chunk.argsDelta.c_str());
    if (input.HasParseError()) {
      ToolExecutionResult result;
      result.index = idx;
      result.toolCallId = chunk.id;
      result.toolName = chunk.nameDelta;
      result.toolArgs = chunk.argsDelta;
      result.resultStr = "Invalid JSON arguments: " + chunk.argsDelta;
      result.success = false;
      immediateResults.push_back(std::move(result));
      continue;
    }
    runnableChunks.push_back(RunnableChunk{chunk, idx});
  }

  const auto sharedState = std::make_shared<SharedExecutionState>();
  sharedState->results.resize(runnableChunks.size());
  std::vector<std::thread> workers;
  workers.reserve(runnableChunks.size());

  std::shared_ptr<shared::IAgent> selfKeepAlive;
  if (!context.identity.id.empty()) {
    selfKeepAlive = AgentRegistry::instance().getAgent(context.identity.id);
  }

  for (std::size_t i = 0; i < runnableChunks.size(); ++i) {
    const auto runnable = runnableChunks[i];
    workers.emplace_back([this, sharedState, runnable, i, selfKeepAlive,
                          runCancelToken]() {
      (void)selfKeepAlive;
      ToolExecutionResult execResult;
      execResult.index = runnable.index;
      execResult.toolCallId = runnable.chunk.id;
      execResult.toolName = runnable.chunk.nameDelta;
      execResult.toolArgs = runnable.chunk.argsDelta;
      try {
        rapidjson::Document input;
        input.Parse(runnable.chunk.argsDelta.c_str());
        ToolContext toolCtx{*environment_->getHost(), *this, runnable.chunk.id,
                            runCancelToken.get(),
                            &provider::LLMSearchProviderRegistry::instance()};
        auto result =
            toolRegistry.execute(runnable.chunk.nameDelta, input, toolCtx);
        execResult.success = result.success;
        execResult.resultProcessId = result.processId;
        execResult.resultSubagentId = result.subagentId;
        execResult.isBackground = result.is_background;
        if (result.success) {
          execResult.resultStr = result.data;
        } else {
          const std::string trimmedData = shared::StringUtil::trim(result.data);
          execResult.resultStr = (!trimmedData.empty() && trimmedData != "{}")
                                     ? result.data
                                     : result.error;
        }
      } catch (const std::exception &e) {
        execResult.success = false;
        execResult.resultStr = e.what();
      }
      {
        std::lock_guard<std::mutex> lock(sharedState->mutex);
        sharedState->results[i] = std::move(execResult);
        ++sharedState->completed;
      }
      sharedState->cv.notify_one();
    });
  }

  std::vector<ToolExecutionResult> collectedResults;
  for (auto &result : immediateResults) {
    collectedResults.push_back(std::move(result));
  }
  {
    std::unique_lock<std::mutex> lock(sharedState->mutex);
    while (sharedState->completed < runnableChunks.size() &&
           !runCancelToken->load()) {
      sharedState->cv.wait_for(lock, std::chrono::milliseconds(10));
    }
    for (std::size_t i = 0; i < sharedState->results.size(); ++i) {
      if (sharedState->results[i].has_value()) {
        collectedResults.push_back(std::move(*sharedState->results[i]));
        sharedState->results[i].reset();
      }
    }
  }

  for (std::size_t i = 0; i < workers.size(); ++i) {
    if (!workers[i].joinable()) {
      continue;
    }
    // Always join worker threads to avoid use-after-free
    // Threads that haven't completed will be joined here (may block briefly)
    if (workers[i].joinable()) {
      workers[i].join();
    }
    if (i < sharedState->results.size() &&
        sharedState->results[i].has_value()) {
      collectedResults.push_back(std::move(sharedState->results[i].value()));
      sharedState->results[i].reset();
    }
  }

  std::unordered_set<std::string> collectedToolIds;
  for (const auto &result : collectedResults) {
    if (!result.toolCallId.empty()) {
      collectedToolIds.insert(result.toolCallId);
    }
  }
  if (runCancelToken->load()) {
    for (const auto &chunk : chunks) {
      if (chunk.id.empty() || collectedToolIds.count(chunk.id) > 0) {
        continue;
      }
      ToolExecutionResult cancelled;
      const auto it = std::find_if(runnableChunks.begin(), runnableChunks.end(),
                                   [&](const RunnableChunk &runnable) {
                                     return runnable.chunk.id == chunk.id;
                                   });
      cancelled.index =
          it != runnableChunks.end() ? it->index : collectedResults.size();
      cancelled.toolCallId = chunk.id;
      cancelled.toolName = chunk.nameDelta;
      cancelled.toolArgs = chunk.argsDelta;
      cancelled.resultStr = "User aborted tool manually.";
      cancelled.success = false;
      collectedResults.push_back(std::move(cancelled));
    }
  }

  std::sort(collectedResults.begin(), collectedResults.end(),
            [](const ToolExecutionResult &a, const ToolExecutionResult &b) {
              return a.index < b.index;
            });

  for (const auto &result : collectedResults) {
    Message msg;
    msg.role = Role::ToolResult;
    msg.content.push_back(
        ToolResultContent{result.toolCallId, result.resultStr, result.success,
                          result.resultProcessId, result.resultSubagentId});
    auto now = std::chrono::system_clock::now();
    msg.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
    toolResultTurn.messages.push_back(msg);

    if (result.success) {
      runtime_overlay::reconcileSuccessfulToolResult(
          context, *environment_->getHost(), environment_->getWorkspace(),
          result.toolName, result.toolArgs, result.resultStr);

      const bool owns_background_process =
          !result.resultProcessId.empty() &&
          (result.isBackground || result.toolName == "process_spawn");
      if (owns_background_process &&
          std::find(context.state.ownedProcesses.begin(),
                    context.state.ownedProcesses.end(),
                    result.resultProcessId) ==
              context.state.ownedProcesses.end()) {
        context.state.ownedProcesses.push_back(result.resultProcessId);
      }

      if (result.toolName == "file_edit" || result.toolName == "file_write") {
        for (const auto &file :
             extractFileEditEventPayloads(result.toolArgs, result.resultStr)) {
          if (file.path.empty()) {
            continue;
          }
          if (std::find(context.state.editedFiles.begin(),
                        context.state.editedFiles.end(),
                        file.path) == context.state.editedFiles.end()) {
            context.state.editedFiles.push_back(file.path);
          }
          std::string actionDesc = "Edited file: " + file.path;
          context.state.completedActions.push_back(actionDesc);
          onEvent(AgentFileEdited{context.identity.id,
                                  context.identity.parentId, file.path,
                                  result.toolCallId, file.diffPreview,
                                  file.addedLines, file.removedLines});
        }
      }
    } else if (result.toolName == "file_edit") {
      rapidjson::Document resultDoc;
      resultDoc.Parse(result.resultStr.c_str());
      if (!resultDoc.HasParseError() && resultDoc.IsObject() &&
          resultDoc.HasMember("stale_anchor") &&
          resultDoc["stale_anchor"].IsBool() &&
          resultDoc["stale_anchor"].GetBool() && resultDoc.HasMember("path") &&
          resultDoc["path"].IsString()) {
        runtime_overlay::refreshFileWatch(context, *environment_->getHost(),
                                          environment_->getWorkspace(),
                                          resultDoc["path"].GetString());
      }
    }

    std::string signature = result.toolName + ":" + result.toolArgs;
    context.state.recentToolCallSignatures.push_back(signature);
  }

  // Keep only last 20 signatures to prevent unbounded growth
  if (context.state.recentToolCallSignatures.size() > 20) {
    context.state.recentToolCallSignatures.erase(
        context.state.recentToolCallSignatures.begin(),
        context.state.recentToolCallSignatures.begin() +
            (context.state.recentToolCallSignatures.size() - 20));
  }

  if (!toolResultTurn.messages.empty()) {
    context.history->turns.push_back(toolResultTurn);
    if (context.config.persistHistory && journaler)
      journaler->appendTurn(toolResultTurn);
  }

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

void Agent::appendHistoryTurn(const AgentTurn &turn) { appendTurnToHistory(turn); }

void Agent::compactContext(
    std::function<void(const shared::StreamEvent &)> onEvent) {
  context.state.currentStatus = AgentStatus::Compacting;
  onEvent(AgentCompacting{context.identity.id, context.identity.parentId});
  const std::string compactionId = std::to_string(nowMs());

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
  const std::string planAndTodoState = buildPlanAndTodoSnapshot(context);
  if (!planAndTodoState.empty()) {
    factualState += "**Active Work State:**\n" + planAndTodoState + "\n";
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

  std::vector<AgentTurn> preservedTailTurns;
  std::size_t preserveTailCount = 0;
  if (context.history->turns.size() > 2) {
    preserveTailCount =
        std::min<std::size_t>(2, context.history->turns.size() - 1);
    for (std::size_t i = context.history->turns.size() - preserveTailCount;
         i < context.history->turns.size(); ++i) {
      preservedTailTurns.push_back(context.history->turns[i]);
    }
  } else if (context.history->turns.size() > 1) {
    preservedTailTurns.push_back(context.history->turns.back());
    preserveTailCount = 1;
  }

  const std::size_t summarizeEnd =
      context.history->turns.size() - preserveTailCount;
  std::optional<std::size_t> lastToolResultTurnIndex;
  for (std::size_t i = summarizeEnd; i > 1; --i) {
    const auto &candidate = context.history->turns[i - 1];
    bool hasToolResult = false;
    for (const auto &msg : candidate.messages) {
      for (const auto &part : msg.content) {
        if (std::holds_alternative<ToolResultContent>(part)) {
          hasToolResult = true;
          break;
        }
      }
      if (hasToolResult) {
        break;
      }
    }
    if (hasToolResult) {
      lastToolResultTurnIndex = i - 1;
      break;
    }
  }

  for (size_t i = 1; i < summarizeEnd; ++i) {
    if (lastToolResultTurnIndex.has_value() && i == *lastToolResultTurnIndex) {
      continue;
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
        } else if (auto *txt = std::get_if<TextChunk>(&ev)) {
          fullSummary += txt->delta;
          onEvent(AgentCompactionText{context.identity.id, txt->delta,
                                      context.identity.parentId});
        } else if (auto *thk = std::get_if<ThinkingChunk>(&ev)) {
          fullThinking += thk->delta;
          onEvent(AgentCompactionThinking{context.identity.id, thk->delta,
                                          context.identity.parentId});
        } else {
          onEvent(ev);
        }
      },
      &interrupted);

  if (interrupted.load()) {
    context.state.currentStatus = AgentStatus::Cancelled;
    return;
  }

  // Validate summary before clearing history
  if (fullSummary.empty()) {
    onEvent(StreamError{"Context compaction failed: Empty summary generated", 0,
                        ""});
    context.state.currentStatus = AgentStatus::Idle;
    return;
  }

  uint32_t oldTokens = context.aggregateMetrics.tokens.contextSize;
  saveCompactionSnapshot(context.history->threadId, context.identity.id,
                         compactionId, oldTokens, context.history->turns);

  // Rebuild history array
  std::vector<AgentTurn> newTurns;
  if (!preservedTurns.empty()) {
    newTurns.push_back(preservedTurns[0]);
  }

  AgentTurn startTurn;
  startTurn.turnId = "compaction-start-" + compactionId;
  Message startMsg;
  startMsg.role = Role::System;
  startMsg.content.push_back(
      TextContent{"Compaction started. Preserving active work state before "
                  "context reduction."});
  startMsg.timestamp = nowMs();
  startTurn.messages.push_back(startMsg);
  newTurns.push_back(startTurn);

  // Create durable compaction summary turn (system-visible, not fake user
  // input)
  AgentTurn summaryTurn;
  summaryTurn.turnId = "compaction-summary-" + compactionId;

  Message summaryMsg;
  summaryMsg.role = Role::System;
  if (!fullThinking.empty())
    summaryMsg.content.push_back(ThinkingContent{fullThinking, ""});
  summaryMsg.content.push_back(
      TextContent{"COMPACTION SUMMARY:\n" + fullSummary});
  summaryMsg.timestamp = nowMs();

  summaryTurn.messages.push_back(summaryMsg);
  newTurns.push_back(summaryTurn);

  const uint32_t compactedContextSize = 1000;
  const uint32_t tokensSaved =
      (oldTokens > compactedContextSize) ? oldTokens - compactedContextSize : 0;
  AgentTurn endTurn;
  endTurn.turnId = "compaction-end-" + compactionId;
  Message endMsg;
  endMsg.role = Role::System;
  endMsg.content.push_back(TextContent{
      "Compaction complete. tokens_saved=" + std::to_string(tokensSaved) +
      ", context_size_after=" + std::to_string(compactedContextSize) +
      ", compaction_id=" + compactionId});
  endMsg.timestamp = nowMs();
  endTurn.messages.push_back(endMsg);
  newTurns.push_back(endTurn);

  if (lastToolResultTurnIndex.has_value()) {
    const auto &toolTurn = context.history->turns[*lastToolResultTurnIndex];
    bool alreadyPreserved = std::any_of(
        preservedTailTurns.begin(), preservedTailTurns.end(),
        [&](const AgentTurn &turn) { return turn.turnId == toolTurn.turnId; });
    if (!alreadyPreserved) {
      newTurns.push_back(toolTurn);
    }
  }
  for (const auto &tailTurn : preservedTailTurns) {
    newTurns.push_back(tailTurn);
  }

  context.history->turns = std::move(newTurns);

  if (context.config.persistHistory && journaler) {
    journaler->rewriteJournal(context.history->turns);
  }

  // Reset context size to conservative estimate (system + task + summary ~1000
  // tokens) This prevents immediate re-compaction on next turn
  context.aggregateMetrics.tokens.contextSize = compactedContextSize;

  onEvent(ContextCompacted{context.identity.id, tokensSaved,
                           context.identity.parentId});
}

} // namespace firmius::core
