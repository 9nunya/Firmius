#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "ConfigLoader.hpp"
#include "Serialization.hpp"
#include "agents/Agent.hpp"
#include "agents/working_memory/WorkingMemoryWorker.hpp"
#include "agents/PurposeLoader.hpp"
#include "agents/RuntimeOverlay.hpp"
#include "environment/Environment.hpp"
#include "environment/Permissions.hpp"
#include "harness/Harness.hpp"
#include "hosts/DockerHost.hpp"
#include "hosts/LocalHost.hpp"
#include "persistence/HistoryEditor.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/AntigravityProvider.hpp"
#include "providers/ChutesProvider.hpp"
#include "providers/GitlawbProvider.hpp"
#include "providers/CodexProvider.hpp"
#include "providers/KimiProvider.hpp"
#include "providers/KiloProvider.hpp"
#include "providers/KiroProvider.hpp"
#include "providers/LMStudioProvider.hpp"
#include "providers/NanoGPTProvider.hpp"
#include "providers/NvidiaProvider.hpp"
#include "providers/OpenRouterProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/QwenProvider.hpp"
#include "providers/ZaiProvider.hpp"
#include "providers/ZenProvider.hpp"
#include "tools/ArtifactsTool.hpp"
#include "tools/LspTool.hpp"
#include "tools/DelegateTool.hpp"
#include "tools/FileEditTool.hpp"
#include "tools/ReadTool.hpp"
#include "tools/ListTool.hpp"
#include "tools/GrepTool.hpp"
#include "tools/GlobTool.hpp"
#include "tools/FleetTool.hpp"
#include "tools/FleetLockRespondTool.hpp"
#include "tools/FleetLockTool.hpp"
#include "tools/FleetStatusTool.hpp"
#include "tools/LspDiagnosticsTool.hpp"
#include "tools/ProcessTool.hpp"
#include "tools/PythonExecuteTool.hpp"
#include "tools/SkillLoadTool.hpp"
#include "tools/TodoWriteTool.hpp"
#include "tools/PinTool.hpp"
#include "tools/WebTool.hpp"
#include "lsp/LspServerManager.hpp"
#include "utils/HistoryMetrics.hpp"
#include "utils/JsonUtil.hpp"
#include "utils/StringUtil.hpp"
#include "utils/Hashline.hpp"
#include <Panic.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <rapidjson/document.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

namespace firmius::core {

namespace {
struct ReverseFileEditOperation {
  std::string path;
  std::string description;
  int startLine = 1;
  int endLine = 0;
  std::vector<std::string> oldLines;
  std::vector<std::string> newLines;
};

std::vector<std::string> splitLinesForUndo(const std::string &content) {
  std::vector<std::string> lines;
  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::string joinLinesForUndo(const std::vector<std::string> &lines) {
  std::ostringstream out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      out << '\n';
    }
    out << lines[i];
  }
  if (!lines.empty()) {
    out << '\n';
  }
  return out.str();
}

std::vector<std::string> splitStoredLines(const std::string &content) {
  std::vector<std::string> lines;
  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::string joinStoredLines(const std::vector<std::string> &lines) {
  std::ostringstream out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) out << '\n';
    out << lines[i];
  }
  return out.str();
}

std::vector<std::string> parseStringArrayMember(const rapidjson::Value &value,
                                                const char *key) {
  std::vector<std::string> lines;
  if (!value.IsObject() || !value.HasMember(key) || !value[key].IsArray()) {
    return lines;
  }
  for (const auto &entry : value[key].GetArray()) {
    if (entry.IsString()) {
      lines.emplace_back(entry.GetString());
    }
  }
  return lines;
}

std::vector<ReverseFileEditOperation>
extractReverseOperationsFromToolResult(const std::string &resultJson) {
  std::vector<ReverseFileEditOperation> operations;
  rapidjson::Document doc;
  doc.Parse(resultJson.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return operations;
  }

  auto appendFromObject = [&](const rapidjson::Value &value) {
    if (!value.IsObject() || !value.HasMember("path") || !value["path"].IsString() ||
        !value.HasMember("operations") || !value["operations"].IsArray()) {
      return;
    }
    const std::string path = value["path"].GetString();
    for (const auto &op : value["operations"].GetArray()) {
      if (!op.IsObject()) {
        continue;
      }
      ReverseFileEditOperation reverse;
      reverse.path = path;
      if (op.HasMember("description") && op["description"].IsString()) {
        reverse.description = op["description"].GetString();
      }
      if (op.HasMember("start_line") && op["start_line"].IsInt()) {
        reverse.startLine = op["start_line"].GetInt();
      }
      if (op.HasMember("end_line") && op["end_line"].IsInt()) {
        reverse.endLine = op["end_line"].GetInt();
      } else {
        reverse.endLine = reverse.startLine;
      }
      reverse.oldLines = parseStringArrayMember(op, "old_lines");
      reverse.newLines = parseStringArrayMember(op, "new_lines");
      operations.push_back(std::move(reverse));
    }
  };

  if (doc.HasMember("files") && doc["files"].IsArray()) {
    for (const auto &file : doc["files"].GetArray()) {
      appendFromObject(file);
    }
  } else {
    appendFromObject(doc);
  }
  return operations;
}

std::vector<ReverseFileEditOperation>
collectReverseFileEditsFromTurns(const std::vector<AgentTurn> &removedTurns) {
  std::vector<ReverseFileEditOperation> reverseOps;
  for (auto turnIt = removedTurns.rbegin(); turnIt != removedTurns.rend(); ++turnIt) {
    std::unordered_map<std::string, std::string> toolNamesById;
    for (const auto &msg : turnIt->messages) {
      for (const auto &part : msg.content) {
        if (const auto *tc = std::get_if<ToolCallContent>(&part)) {
          toolNamesById[tc->id] = tc->name;
        }
      }
    }
    for (const auto &msg : turnIt->messages) {
      for (const auto &part : msg.content) {
        const auto *tr = std::get_if<ToolResultContent>(&part);
        if (!tr || !tr->success) {
          continue;
        }
        const auto toolIt = toolNamesById.find(tr->toolCallId);
        if (toolIt == toolNamesById.end()) {
          continue;
        }
        const std::string &toolName = toolIt->second;
        if (toolName != "Edit" && toolName != "file_edit" &&
            toolName != "file_write" && toolName != "Write") {
          continue;
        }
        auto extracted = extractReverseOperationsFromToolResult(tr->result);
        std::reverse(extracted.begin(), extracted.end());
        reverseOps.insert(reverseOps.end(), extracted.begin(), extracted.end());
      }
    }
  }
  return reverseOps;
}

void applyReverseFileEdits(const std::shared_ptr<IAgent> &agent,
                           const std::vector<ReverseFileEditOperation> &operations) {
  if (!agent) {
    return;
  }
  for (const auto &op : operations) {
    if (op.path.empty()) {
      continue;
    }
    const std::string absolutePath =
        agent->getEnvironment()->getWorkspace().resolvePath(op.path);
    std::vector<std::string> currentLines;
    try {
      if (agent->getHost()->exists(absolutePath)) {
        const auto data = agent->getHost()->readFile(absolutePath);
        currentLines = splitLinesForUndo(
            std::string(data.begin(), data.end()));
      }
    } catch (...) {
    }

    const int startIndex = std::max(0, op.startLine - 1);
    const int expectedNewCount = static_cast<int>(op.newLines.size());
    const int eraseEnd =
        std::min(static_cast<int>(currentLines.size()), startIndex + expectedNewCount);

    if (startIndex <= static_cast<int>(currentLines.size())) {
      currentLines.erase(currentLines.begin() + startIndex,
                         currentLines.begin() + eraseEnd);
      currentLines.insert(currentLines.begin() + startIndex, op.oldLines.begin(),
                          op.oldLines.end());
    } else if (!op.oldLines.empty()) {
      currentLines.insert(currentLines.end(), op.oldLines.begin(), op.oldLines.end());
    }

    const std::string restored = joinLinesForUndo(currentLines);
    agent->getHost()->writeFile(
        absolutePath,
        std::vector<uint8_t>(restored.begin(), restored.end()));
    agent->getEnvironment()->getWorkspace().recordFileEdit(absolutePath);
  }
}

template <typename UndoFn>
UndoResult applyUndoAndRestoreFiles(const std::shared_ptr<IAgent> &agent,
                                    UndoFn &&undoFn) {
  UndoResult result;
  if (!agent || !agent->getContext().history) {
    return result;
  }
  auto &turns = agent->getMutableContext().history->turns;
  const auto beforeTurns = turns;
  result = undoFn(turns);
  const std::size_t remaining = turns.size();
  std::vector<AgentTurn> removedTurns;
  if (beforeTurns.size() > remaining) {
    removedTurns.assign(beforeTurns.begin() + static_cast<long>(remaining),
                        beforeTurns.end());
  }
  applyReverseFileEdits(agent, collectReverseFileEditsFromTurns(removedTurns));
  return result;
}

template <typename UndoFn>
std::pair<UndoResult, std::vector<AgentTurn>>
applyUndoAndCaptureRemovedTurns(const std::shared_ptr<IAgent> &agent,
                                UndoFn &&undoFn) {
  UndoResult result;
  std::vector<AgentTurn> removedTurns;
  if (!agent || !agent->getContext().history) {
    return {result, removedTurns};
  }
  auto &turns = agent->getMutableContext().history->turns;
  const auto beforeTurns = turns;
  result = undoFn(turns);
  const std::size_t remaining = turns.size();
  if (beforeTurns.size() > remaining) {
    removedTurns.assign(beforeTurns.begin() + static_cast<long>(remaining),
                        beforeTurns.end());
  }
  applyReverseFileEdits(agent, collectReverseFileEditsFromTurns(removedTurns));
  return {result, removedTurns};
}

shared::TranscriptUndoAction persistTranscriptUndo(
    const std::shared_ptr<IAgent> &agent, const std::string &agentId,
    const std::string &scopeType, const std::string &scopeArgJson,
    const std::string &reason, std::vector<AgentTurn> removedTurns) {
  if (!agent) {
    throw std::runtime_error("Cannot persist transcript undo without agent");
  }
  const auto &ctx = agent->getContext();
  if (!ctx.history) {
    throw std::runtime_error("Cannot persist transcript undo without history");
  }
  shared::TranscriptUndoAction action;
  action.undoActionId = shared::StringUtil::generateUuid();
  action.threadId = ctx.history->threadId;
  action.agentId = agentId;
  action.scopeType = scopeType;
  action.scopeArgJson = scopeArgJson;
  action.createdAt = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  action.redoAvailable = !removedTurns.empty();
  action.reason = reason;

  std::vector<shared::TranscriptRedoPayload> payloads;
  if (!removedTurns.empty()) {
    shared::TranscriptRedoPayload payload;
    payload.undoActionId = action.undoActionId;
    payload.threadId = action.threadId;
    payload.agentId = action.agentId;
    payload.ordinal = 0;
    payload.turns = std::move(removedTurns);
    payloads.push_back(std::move(payload));
  }

  ThreadManager tm(ThreadManager::defaultBasePath());
  tm.writeTranscriptUndoAction(action.threadId, action, payloads);
  return action;
}

void appendTurnsForTranscriptRedo(const std::shared_ptr<IAgent> &agent,
                                  const std::vector<AgentTurn> &turns) {
  if (!agent || !agent->getContext().history || turns.empty()) {
    return;
  }
  auto &historyTurns = agent->getMutableContext().history->turns;
  historyTurns.insert(historyTurns.end(), turns.begin(), turns.end());
}

std::string extractFinalSummary(const std::shared_ptr<IAgent> &agent) {
  if (!agent || !agent->getContext().history) {
    return "";
  }
  const auto &turns = agent->getContext().history->turns;
  if (turns.empty()) {
    return "";
  }
  // Find the last Assistant turn (not User or System)
  for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
    if (it->messages.empty()) {
      continue;
    }
    const auto &msg = it->messages.back();
    if (msg.role == Role::Assistant) {
      std::string content;
      for (const auto &part : msg.content) {
        if (auto *txt = std::get_if<TextContent>(&part)) {
          content += txt->text;
        }
      }
      return content;
    }
  }
  // No Assistant turn found
  return "";
}

AgentOutcome makeOutcome(const std::shared_ptr<IAgent> &agent,
                         const std::string &summary) {
  AgentOutcome outcome;
  if (agent && agent->getContext().state.currentStatus == AgentStatus::Error) {
    outcome.kind = AgentOutcome::Kind::Failed;
    if (agent->getContext().state.fatalError.has_value() &&
        !agent->getContext().state.fatalError->empty()) {
      outcome.text = *agent->getContext().state.fatalError;
    } else {
      outcome.text = summary;
    }
    return outcome;
  }
  if (agent &&
      agent->getContext().state.currentStatus == AgentStatus::Cancelled) {
    outcome.kind = AgentOutcome::Kind::Cancelled;
    outcome.text = "Cancelled";
    return outcome;
  }

  const std::string trimmedSummary = shared::StringUtil::trim(summary);
  if (trimmedSummary.empty()) {
    outcome.kind = AgentOutcome::Kind::NoSummary;
    outcome.text.clear();
  } else {
    outcome.kind = AgentOutcome::Kind::Response;
    outcome.text = summary;
  }
  return outcome;
}

AgentOutcome makeFailedOutcome(const std::string &message) {
  return AgentOutcome{AgentOutcome::Kind::Failed, message};
}

using ArtifactSnapshot =
    std::unordered_map<std::string, shared::ThreadArtifactMetadata>;
void releaseOwnedChunksForTerminalAgent(const std::shared_ptr<IAgent> &agent,
                                        const AgentOutcome &outcome) {
  if (!agent || !agent->getContext().history) {
    return;
  }

  const std::string threadId = agent->getContext().history->threadId;
  const std::string agentId = agent->getContext().identity.id;
  if (threadId.empty() || agentId.empty()) {
    return;
  }

  (void)outcome;
}

ArtifactSnapshot collectArtifactSnapshot(const std::string &threadId,
                                         const std::string &agentId) {
  ArtifactSnapshot snapshot;
  if (threadId.empty() || agentId.empty()) {
    return snapshot;
  }

  try {
    ThreadManager tm(ThreadManager::defaultBasePath());
    for (const auto &artifact : tm.listArtifactsForAgent(threadId, agentId)) {
      snapshot[artifact.filename] = artifact;
    }
  } catch (...) {
  }
  return snapshot;
}

void attachArtifactDeltasToOutcome(const std::string &threadId,
                                   const std::string &agentId,
                                   const ArtifactSnapshot &before,
                                   AgentOutcome &outcome) {
  const ArtifactSnapshot after = collectArtifactSnapshot(threadId, agentId);
  for (const auto &[filename, artifact] : after) {
    auto it = before.find(filename);
    if (it == before.end()) {
      outcome.artifacts_created.push_back(artifact);
      continue;
    }
    const auto &beforeArtifact = it->second;
    if (artifact.updatedAt != beforeArtifact.updatedAt ||
        artifact.storagePath != beforeArtifact.storagePath ||
        artifact.description != beforeArtifact.description ||
        artifact.kind != beforeArtifact.kind) {
      outcome.artifacts_updated.push_back(artifact);
    }
  }
}

AgentStatus inferPersistedStatus(const AgentHistory &history) {
  for (auto it = history.turns.rbegin(); it != history.turns.rend(); ++it) {
    if (it->stopReason == StopReason::Cancelled) {
      return AgentStatus::Cancelled;
    }
    if (it->turnId.rfind("cancelled-", 0) == 0) {
      return AgentStatus::Cancelled;
    }
    if (it->messages.empty()) {
      continue;
    }
    const auto &message = it->messages.back();
    if (message.role == Role::System) {
      for (const auto &part : message.content) {
        if (auto *notice = std::get_if<NoticeContent>(&part)) {
          if (notice->title == "Agent Cancelled") {
            return AgentStatus::Cancelled;
          }
        }
      }
    }
    if (message.role == Role::Error) {
      for (const auto &part : message.content) {
        if (auto *error = std::get_if<ErrorContent>(&part)) {
          if (error->errorName == "Agent Cancelled") {
            return AgentStatus::Cancelled;
          }
          return AgentStatus::Error;
        }
      }
      return AgentStatus::Error;
    }
    if (message.role == Role::Assistant || message.role == Role::ToolResult) {
      return AgentStatus::Idle;
    }
    if (message.role == Role::User && it->turnId.rfind("user-task-", 0) == 0) {
      return AgentStatus::Cancelled;
    }
  }
  return AgentStatus::Idle;
}

std::string threadStorageRootPathForUndo() { return ThreadManager::defaultBasePath(); }

bool hasCompactionMarker(const AgentTurn &turn) {
  return turn.turnId.rfind("compaction-start-", 0) == 0 ||
         turn.turnId.rfind("compaction-summary-", 0) == 0 ||
         turn.turnId.rfind("compaction-end-", 0) == 0;
}

std::optional<std::string> compactionIdFromTurnId(const std::string &turnId) {
  constexpr const char *prefixes[] = {"compaction-start-",
                                      "compaction-summary-", "compaction-end-"};
  for (const char *prefix : prefixes) {
    const std::string_view view(prefix);
    if (turnId.rfind(prefix, 0) == 0) {
      return turnId.substr(view.size());
    }
  }
  return std::nullopt;
}

std::optional<std::string>
latestCompactionIdInHistory(const std::vector<AgentTurn> &turns) {
  for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
    if (auto id = compactionIdFromTurnId(it->turnId); id.has_value()) {
      return id;
    }
  }
  return std::nullopt;
}

bool restoreCompactionSnapshot(AgentContext &ctx) {
  if (!ctx.history || ctx.history->threadId.empty() ||
      ctx.identity.id.empty()) {
    return false;
  }
  ThreadManager tm(threadStorageRootPathForUndo());
  auto snapshots =
      tm.loadCompactionSnapshots(ctx.history->threadId, ctx.identity.id);
  if (snapshots.empty()) {
    return false;
  }
  std::optional<std::string> targetCompactionId =
      latestCompactionIdInHistory(ctx.history->turns);
  auto it = snapshots.end() - 1;
  if (targetCompactionId.has_value()) {
    auto reverse_it =
        std::find_if(snapshots.rbegin(), snapshots.rend(),
                     [&](const CompactionSnapshot &snapshot) {
                       return snapshot.compactionId == *targetCompactionId;
                     });
    if (reverse_it != snapshots.rend()) {
      it = std::prev(reverse_it.base());
    }
  }
  if (it->turns.empty()) {
    return false;
  }

  ctx.history->turns = it->turns;
  ctx.aggregateMetrics.tokens.contextSize = it->previousContextSize;
  tm.popCompactionSnapshot(ctx.history->threadId, ctx.identity.id,
                           it->compactionId.empty()
                               ? std::optional<std::string>{}
                               : std::optional<std::string>{it->compactionId});
  return true;
}

bool shouldAttemptCompactionRestore(const AgentContext &ctx, int count) {
  if (!ctx.history || ctx.history->turns.size() <= 2 || count <= 0) {
    return false;
  }
  const auto latestCompactionId =
      latestCompactionIdInHistory(ctx.history->turns);
  if (!latestCompactionId.has_value()) {
    return false;
  }
  const int maxRemovable = static_cast<int>(ctx.history->turns.size()) - 2;
  const int toInspect = std::min(count, maxRemovable);
  for (int i = 0; i < toInspect; ++i) {
    const auto &turn = ctx.history->turns[ctx.history->turns.size() - 1 - i];
    const auto turnCompactionId = compactionIdFromTurnId(turn.turnId);
    if (turnCompactionId.has_value() &&
        *turnCompactionId == *latestCompactionId) {
      return true;
    }
  }
  return false;
}

void cancelAgentRuntime(const std::shared_ptr<IAgent> &agent) {
  if (!agent) {
    return;
  }
  agent->interrupt();
  auto procIds =
      agent->getEnvironment()->getProcessManager().getBlockingProcessIds();
  for (const auto &procId : procIds) {
    try {
      agent->getEnvironment()->getProcessManager().killProcess(procId);
    } catch (...) {
    }
    try {
      agent->getHost()->killBackgroundProcess(procId);
    } catch (...) {
    }
  }
}
} // namespace

Engine::Engine() {
  initProviders();

  shared::Panic::addExtraInfo("active_agents", []() -> std::string {
    std::stringstream ss;
    auto agentIds = AgentRegistry::instance().listAll();
    ss << "Count: " << agentIds.size() << "\n";
    for (const auto &id : agentIds) {
      auto agent = AgentRegistry::instance().getAgent(id);
      if (agent) {
        const auto &ctx = agent->getContext();
        ss << "  - " << id << ": " << ctx.identity.name << " ("
           << (agent->isRunning() ? "running" : "idle") << ", "
           << ctx.history->threadId << ")\n";
      }
    }
    return ss.str();
  });

  toolRegistry.registerToolFactory(
      "Read", []() { return std::make_unique<ReadTool>(); });
  toolRegistry.registerToolFactory(
      "List", []() { return std::make_unique<ListTool>(); });
  toolRegistry.registerToolFactory(
      "Grep", []() { return std::make_unique<GrepTool>(); });
  toolRegistry.registerToolFactory(
      "Glob", []() { return std::make_unique<GlobTool>(); });
  toolRegistry.registerToolFactory(
      "Edit", []() { return std::make_unique<FileEditTool>(); });
  toolRegistry.registerToolFactory(
      "EditWrite", []() { return std::make_unique<FileWriteTool>(); });
  toolRegistry.registerToolFactory(
      "EditReplace", []() { return std::make_unique<FileReplaceTool>(); });
  toolRegistry.registerToolFactory(
      "EditRange", []() { return std::make_unique<FileRangeTool>(); });
  toolRegistry.registerToolFactory(
      "Web", []() { return std::make_unique<WebTool>(); });
  toolRegistry.registerToolFactory(
      "Process", []() { return std::make_unique<ProcessTool>(); });
  toolRegistry.registerToolFactory(
      "Delegate", []() { return std::make_unique<DelegateTool>(); });
  toolRegistry.registerToolFactory(
      "Python", []() { return std::make_unique<PythonExecuteTool>(); });
  toolRegistry.registerToolFactory(
      "Skill", []() { return std::make_unique<SkillLoadTool>(); });
  toolRegistry.registerToolFactory(
      "Todo", []() { return std::make_unique<TodoWriteTool>(); });
  toolRegistry.registerToolFactory(
      "pin", []() { return std::make_unique<PinTool>(); });
  toolRegistry.registerToolFactory(
      "Artifacts", []() { return std::make_unique<ArtifactsTool>(); });
  toolRegistry.registerToolFactory(
      "Lsp", []() { return std::make_unique<LspTool>(); });
}

void Engine::initProviders() {
  auto &reg = firmius::provider::ProviderRegistry::instance();

  // Register providers lazily via factories - instantiated only when first used
  reg.registerProviderFactory("nanogpt", []() {
    return std::make_shared<firmius::provider::NanoGPTProvider>();
  });
  reg.registerProviderFactory("nvidia", []() {
    return std::make_shared<firmius::provider::NvidiaProvider>("");
  });
  reg.registerProviderFactory("openrouter", []() {
    return std::make_shared<firmius::provider::OpenRouterProvider>("");
  });
  reg.registerProviderFactory("zai", []() {
    return std::make_shared<firmius::provider::ZaiProvider>("");
  });
  reg.registerProviderFactory("zen", []() {
    return std::make_shared<firmius::provider::ZenProvider>("");
  });
  reg.registerProviderFactory("chutes", []() {
    return std::make_shared<firmius::provider::ChutesProvider>("");
  });
  reg.registerProviderFactory("gitlawb", []() {
    return std::make_shared<firmius::provider::GitlawbProvider>();
  });
  reg.registerProviderFactory("codex", []() {
    return std::make_shared<firmius::provider::CodexProvider>();
  });
  reg.registerProviderFactory("antigravity", []() {
    return std::make_shared<firmius::provider::AntigravityProvider>();
  });
  reg.registerProviderFactory("qwen", []() {
    return std::make_shared<firmius::provider::QwenProvider>();
  });
   reg.registerProviderFactory("kimi", []() {
     return std::make_shared<firmius::provider::KimiProvider>();
   });
   reg.registerProviderFactory("kilo", []() {
     return std::make_shared<firmius::provider::KiloProvider>();
   });
  reg.registerProviderFactory("kiro", []() {
     return std::make_shared<firmius::provider::KiroProvider>();
   });
  reg.registerProviderFactory("lmstudio", []() {
     return std::make_shared<firmius::provider::LMStudioProvider>();
   });
  reg.reloadConfigProviders(shared::ConfigLoader::instance().getConfig().providers);
  }

void Engine::reap() { std::lock_guard<std::mutex> lock(fleetMutex); }

std::string Engine::summonAgent(
    const std::string &threadId, const std::string &personaName,
    const std::string &task, bool persistHistory, const std::string &parentId,
    const std::string &friendlyName, const std::string &title,
    const std::string &requestedAgentId, const std::string &providerId,
    const std::string &modelId, const std::string &variantName,
    const std::vector<firmius::shared::ImageContent> &images,
    const std::optional<SummonAgentOverrides> &overrides) {
  reap();

  // No limit on concurrent agents - removed to allow unlimited parallel
  // exploration

  std::string agentId = requestedAgentId.empty()
                            ? shared::StringUtil::generateUuid()
                            : requestedAgentId;

  auto prom = std::make_shared<std::promise<AgentOutcome>>();
  {
    std::lock_guard<std::mutex> lock(futuresMutex);
    agentFutures[agentId] = prom->get_future().share();
  }

  {
    std::lock_guard<std::mutex> lock(fleetMutex);
    fleet.emplace_back([this, threadId, agentId, personaName, task, images,
                        prom, persistHistory, parentId, friendlyName, title,
                        providerId, modelId, variantName, overrides]() {
      auto errorBroadcast = std::make_shared<std::atomic<bool>>(false);
      ArtifactSnapshot runStartArtifacts;
      bool runStartArtifactsCaptured = false;
      try {
        // 1. Loading metadata in background thread
        auto metadata =
            ThreadManager(ThreadManager::defaultBasePath())
                .getMetadata(threadId);
        std::string effectivePersonaName = personaName;
        Persona persona;
        try {
          persona = PurposeLoader::load(personaName);
        } catch (const std::exception &e) {
          auto available = PurposeLoader::listPurposes();
          if (available.empty()) {
            throw;
          }
          effectivePersonaName = available.front();
          std::cerr << "[purpose] Failed to load persona '" << personaName
                    << "' (" << e.what() << "); falling back to '"
                    << effectivePersonaName << "'.\n";
          persona = PurposeLoader::load(effectivePersonaName);
        }

        AgentContext ctx;
        ctx.identity.id = agentId;
        ctx.identity.parentId = parentId;
        ctx.identity.friendlyName =
            parentId.empty() ? effectivePersonaName : friendlyName;

        const auto &userCfg = shared::ConfigLoader::instance().getConfig();
        ctx.config.providerId =
            providerId.empty() ? userCfg.defaultProviderId : providerId;
        ctx.config.modelId = modelId.empty() ? userCfg.defaultModelId : modelId;
        ctx.config.modelVariant =
            variantName.empty() ? userCfg.defaultModelVariant : variantName;
        ctx.config.temperature = userCfg.defaultTemperature;
        if (userCfg.defaultMaxTokens.has_value()) {
          ctx.config.maxTokens = userCfg.defaultMaxTokens.value();
        }
        ctx.config.insanityDetectionEnabled = userCfg.insanityDetectionEnabled;
        ctx.config.insanityRepetitionThreshold = userCfg.insanityRepetitionThreshold;
        ctx.config.insanityMaxTokenThreshold = userCfg.insanityMaxTokenThreshold;
        ctx.config.maxInsanityRetries = userCfg.maxInsanityRetries;
        ctx.config.workingMemory = userCfg.workingMemory;
        ctx.config.persistHistory = persistHistory;
        ctx.config.personaName = effectivePersonaName;
        ctx.identity.name = persona.name;
        ctx.identity.role = persona.title;
        ctx.permissions.allowedScopes = persona.allowedScopes;

        std::string home = getenv("HOME") ? getenv("HOME") : "/root";
        if (userCfg.dangerouslySkipPermissions) {
          ctx.permissions.allowedPaths = {"/**"};
        } else {
          ctx.permissions.allowedPaths = {
              metadata.cwd + "/**", "/tmp/**", "/work/**",
              home + "/.agents/skills/**", home + "/.gemini/**", home + "/.firmius/**"};
          ctx.permissions.allowedPaths.push_back(metadata.cwd);
        }
        if (overrides.has_value() &&
            overrides->allowedPathsOverride.has_value()) {
          ctx.permissions.allowedPaths = *overrides->allowedPathsOverride;
        }

        ctx.environment.cwd =
            (overrides.has_value() && overrides->cwdOverride.has_value())
                ? *overrides->cwdOverride
                : metadata.cwd;
        ctx.environment.identifier = metadata.hostIdentifier;
        ctx.environment.type = metadata.hostOptions.type;
        ctx.history = std::make_shared<AgentHistory>();
        ctx.history->threadId = threadId;

        std::unique_ptr<IHost> host;
        if (metadata.hostOptions.type == HostType::Docker) {
          host = std::make_unique<DockerHost>(metadata.hostOptions);
        } else {
          host = std::make_unique<LocalHost>();
        }

        std::shared_ptr<Journaler> jnl = nullptr;
        if (ctx.config.persistHistory) {
          jnl = std::make_shared<Journaler>(threadId, agentId);
        }

        // Create Environment and Permissions
        std::shared_ptr<IHost> hostPtr = std::move(host);
        auto errorBroadcast = std::make_shared<std::atomic<bool>>(false);
        auto environment = std::make_shared<Environment>(
            hostPtr, ctx.environment.cwd,
            [this, agentId, parentId, errorBroadcast](const StreamEvent &ev) {
              handleStreamEvent(agentId, parentId, ev, errorBroadcast);
            });
        auto permissions = std::make_shared<Permissions>(threadId, agentId);

        auto agent = std::make_shared<Agent>(ctx, environment, permissions,
                                             toolRegistry, jnl);
        permissions->bindContext(agent->getContext());
        agent->setBooting(true);
        AgentRegistry::instance().registerAgent(agentId, agent);

        // Pre-populate the initial history so UI fetches a complete snapshot.
        // We only seed the system turn here — the user-task turn is appended
        // by Agent::runImpl below via its own bootstrapHistory call. Passing
        // `task` + `images` here too caused runImpl to re-append the same
        // user-task turn a second time (visible in the transcript as a
        // phantom duplicate "> hai" row on the first prompt of every new
        // thread, and written twice to the journal as user-task-N and
        // user-task-N+1).
        agent->bootstrapHistory(std::nullopt, {});

        // 2. Initialize host
        std::string actualHostId = agent->getHost()->init();

        // Refresh metadata for potential host update
        auto currentMeta =
            ThreadManager(ThreadManager::defaultBasePath())
                .getMetadata(threadId);
        if (currentMeta.hostIdentifier != actualHostId) {
          ThreadManager(ThreadManager::defaultBasePath())
              .updateHostIdentifier(threadId, actualHostId);
          agent->getMutableContext().environment.identifier = actualHostId;
        }

        std::string agentTitle = title.empty() ? persona.title : title;
        broadcast(AgentSpawned{agentId, personaName, parentId,
                               agent->getContext().identity.friendlyName,
                               agentTitle, persistHistory});

        const bool idle_materialization = task.empty() && images.empty();
        if (idle_materialization) {
          agent->setBooting(false);
          agent->getMutableContext().state.currentStatus = AgentStatus::Idle;
          prom->set_value(makeOutcome(agent, ""));
          return;
        }

        // 3. Execution
        runStartArtifacts = collectArtifactSnapshot(threadId, agentId);
        runStartArtifactsCaptured = true;
        agent->run(
            task,
            [this, agentId, parentId, errorBroadcast](const StreamEvent &ev) {
              handleStreamEvent(agentId, parentId, ev, errorBroadcast);
            },
            images);

        const std::string finalSummary = extractFinalSummary(agent);
        AgentOutcome outcome = makeOutcome(agent, finalSummary);
        if (runStartArtifactsCaptured) {
          attachArtifactDeltasToOutcome(threadId, agentId, runStartArtifacts,
                                        outcome);
        }
        releaseOwnedChunksForTerminalAgent(agent, outcome);
        if (outcome.kind == AgentOutcome::Kind::Cancelled) {
          broadcast(AgentInterrupted{agentId, parentId});
        }
        broadcast(AgentFinished{agentId, outcome, parentId});
        prom->set_value(outcome);

      } catch (const std::exception &e) {
        if (!errorBroadcast->load(std::memory_order_relaxed)) {
          auto agent =
              firmius::core::AgentRegistry::instance().getAgent(agentId);
          if (agent && agent->getContext().history) {
            firmius::shared::AgentTurn errorTurn;
            errorTurn.turnId =
                "error-" +
                std::to_string(agent->getContext().history->turns.size());
            firmius::shared::Message errorMsg;
            errorMsg.role = firmius::shared::Role::Error;
            errorMsg.content.push_back(firmius::shared::ErrorContent{
                "Engine Error", "Summon agent failed.", std::string(e.what())});
            errorMsg.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            errorTurn.messages.push_back(errorMsg);
            agent->getMutableContext().history->turns.push_back(errorTurn);
            if (agent->getContext().config.persistHistory) {
              firmius::core::Journaler jnl(threadId, agentId);
              jnl.appendTurn(errorTurn);
            }
          }
          broadcast(AgentError{agentId, e.what(), parentId});
        }
        AgentOutcome outcome = makeFailedOutcome(std::string(e.what()));
        if (runStartArtifactsCaptured) {
          attachArtifactDeltasToOutcome(threadId, agentId, runStartArtifacts,
                                        outcome);
        }
        releaseOwnedChunksForTerminalAgent(
            firmius::core::AgentRegistry::instance().getAgent(agentId), outcome);
        broadcast(AgentFinished{agentId, outcome, parentId});
        prom->set_value(outcome);
      }
    });
  }

  return agentId;
}

std::string Engine::resumeAgent(const std::string &threadId,
                                const std::string &agentId,
                                const std::string &personaName,
                                const std::string &parentId,
                                const std::string &friendlyName,
                                const std::string &title, bool persistHistory) {
  reap();

  if (AgentRegistry::instance().getAgent(agentId)) {
    throw std::runtime_error("Agent already exists: " + agentId);
  }

  auto metadata =
      ThreadManager(ThreadManager::defaultBasePath())
          .getMetadata(threadId);
  std::string effectivePersonaName = personaName;
  Persona persona;
  try {
    persona = PurposeLoader::load(personaName);
  } catch (const std::exception &e) {
    auto available = PurposeLoader::listPurposes();
    if (available.empty()) {
      throw;
    }
    effectivePersonaName = available.front();
    std::cerr << "[purpose] Failed to load persona '" << personaName << "' ("
              << e.what() << "); falling back to '" << effectivePersonaName
              << "'.\n";
    persona = PurposeLoader::load(effectivePersonaName);
  }
  auto history =
      ThreadManager(ThreadManager::defaultBasePath())
          .loadAgentHistory(threadId, agentId);

  AgentContext ctx;
  ctx.identity.id = agentId;
  ctx.identity.parentId = parentId;
  ctx.identity.friendlyName =
      parentId.empty() ? effectivePersonaName : friendlyName;
  ctx.identity.name = persona.name;
  ctx.identity.role = title.empty() ? persona.title : title;
  const auto &userCfg = shared::ConfigLoader::instance().getConfig();
  ctx.config.providerId = userCfg.defaultProviderId;
  ctx.config.modelId = userCfg.defaultModelId;
  ctx.config.temperature = userCfg.defaultTemperature;
  if (userCfg.defaultMaxTokens.has_value()) {
    ctx.config.maxTokens = userCfg.defaultMaxTokens.value();
  }
  ctx.config.insanityDetectionEnabled = userCfg.insanityDetectionEnabled;
  ctx.config.insanityRepetitionThreshold = userCfg.insanityRepetitionThreshold;
  ctx.config.insanityMaxTokenThreshold = userCfg.insanityMaxTokenThreshold;
  ctx.config.maxInsanityRetries = userCfg.maxInsanityRetries;
  ctx.config.persistHistory = persistHistory;
  ctx.config.personaName = effectivePersonaName;
  ctx.permissions.allowedScopes = persona.allowedScopes;

  std::string home = getenv("HOME") ? getenv("HOME") : "/root";
  ctx.permissions.allowedPaths = {metadata.cwd + "/**", "/tmp/**", "/work/**",
                                  home + "/.agents/skills/**",
                                  home + "/.gemini/**", home + "/.firmius/**"};
  // Explicitly allow CWD and . (which resolves to CWD)
  ctx.permissions.allowedPaths.push_back(metadata.cwd);

  ctx.environment.cwd = metadata.cwd;
  ctx.environment.identifier = metadata.hostIdentifier;
  ctx.environment.type = metadata.hostOptions.type;
  ctx.history = std::make_shared<AgentHistory>(std::move(history));
  ctx.aggregateMetrics = aggregateHistoryMetrics(*ctx.history);
  ctx.state.currentStatus = inferPersistedStatus(*ctx.history);

  std::unique_ptr<IHost> host;
  if (metadata.hostOptions.type == HostType::Docker) {
    host = std::make_unique<DockerHost>(metadata.hostOptions);
  } else {
    host = std::make_unique<LocalHost>();
  }

  std::shared_ptr<Journaler> jnl = nullptr;
  if (ctx.config.persistHistory) {
    jnl = std::make_shared<Journaler>(threadId, agentId);
  }

  // Create Environment and Permissions
  std::shared_ptr<IHost> hostPtr = std::move(host);
  auto errorBroadcast = std::make_shared<std::atomic<bool>>(false);
  auto environment = std::make_shared<Environment>(
      hostPtr, ctx.environment.cwd,
      [this, agentId, parentId, errorBroadcast](const StreamEvent &ev) {
        handleStreamEvent(agentId, parentId, ev, errorBroadcast);
      });
  auto permissions = std::make_shared<Permissions>(threadId, agentId);

  auto agent =
      std::make_shared<Agent>(ctx, environment, permissions, toolRegistry, jnl);
  permissions->bindContext(agent->getContext());
  runtime_overlay::reconstructStateFromHistory(agent->getMutableContext(), *agent->getHost(), agent->getEnvironment()->getWorkspace());
  agent->setBooting(true);
  AgentRegistry::instance().registerAgent(agentId, agent);

  {
    std::lock_guard<std::mutex> lock(fleetMutex);
    fleet.emplace_back([this, threadId, agentId, personaName, parentId, title,
                        persistHistory]() {
      try {
        auto agent = AgentRegistry::instance().getAgent(agentId);
        if (!agent)
          throw std::runtime_error("Agent not found in registry");

        std::string actualHostId = agent->getHost()->init();

        auto metadata =
            ThreadManager(ThreadManager::defaultBasePath())
                .getMetadata(threadId);
        if (metadata.hostIdentifier != actualHostId) {
          ThreadManager(ThreadManager::defaultBasePath())
              .updateHostIdentifier(threadId, actualHostId);
          agent->getMutableContext().environment.identifier = actualHostId;
        }

        // Resume is complete — agent is idle and ready for messages.
        // Without this, booting stays true forever and Harness::send()
        // queues every message into the void.
        agent->setBooting(false);

        std::string effectivePersonaName = personaName;
        Persona persona;
        try {
          persona = PurposeLoader::load(personaName);
        } catch (const std::exception &e) {
          auto available = PurposeLoader::listPurposes();
          if (available.empty()) {
            throw;
          }
          effectivePersonaName = available.front();
          std::cerr << "[purpose] Failed to load persona '" << personaName
                    << "' (" << e.what() << "); falling back to '"
                    << effectivePersonaName << "'.\n";
          persona = PurposeLoader::load(effectivePersonaName);
        }

        // If the persona was invalid when resuming, update the agent's
        // persisted identity so subsequent operations reflect the fallback.
        agent->getMutableContext().config.personaName = effectivePersonaName;
        agent->getMutableContext().identity.name = persona.name;
        agent->getMutableContext().identity.role = title.empty() ? persona.title : title;
        agent->getMutableContext().permissions.allowedScopes = persona.allowedScopes;

        std::string agentTitle = title.empty() ? persona.title : title;
        broadcast(AgentSpawned{agentId, personaName, parentId,
                               agent->getContext().identity.friendlyName,
                               agentTitle, persistHistory});

      } catch (const std::exception &e) {
        auto agent = firmius::core::AgentRegistry::instance().getAgent(agentId);
        if (agent) {
          agent->setBooting(false);
        }
        if (agent && agent->getContext().history) {
          firmius::shared::AgentTurn errorTurn;
          errorTurn.turnId =
              "error-" +
              std::to_string(agent->getContext().history->turns.size());
          firmius::shared::Message errorMsg;
          errorMsg.role = firmius::shared::Role::Error;
          errorMsg.content.push_back(firmius::shared::ErrorContent{
              "Engine Error", "Resume agent failed.", std::string(e.what())});
          errorMsg.timestamp = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
          errorTurn.messages.push_back(errorMsg);
          agent->getMutableContext().history->turns.push_back(errorTurn);
          if (agent->getContext().config.persistHistory) {
            firmius::core::Journaler jnl(threadId, agentId);
            jnl.appendTurn(errorTurn);
          }
        }
        broadcast(AgentError{agentId, e.what(), parentId});
      }
    });
  }

  return agentId;
}

std::string
Engine::createAgent(const std::string &threadId, const std::string &personaName,
                    bool persistHistory, const std::string &parentId,
                    const std::string &friendlyName, const std::string &title) {
  std::string agentId = shared::StringUtil::generateUuid();
  return resumeAgent(threadId, agentId, personaName, parentId, friendlyName,
                     title, persistHistory);
}

std::optional<AgentOutcome>
Engine::waitForAgentOutcome(const std::string &agentId,
                            std::optional<std::chrono::milliseconds> timeout) {
  std::shared_future<AgentOutcome> fut;
  bool hasFuture = false;
  {
    std::lock_guard<std::mutex> lock(futuresMutex);
    auto it = agentFutures.find(agentId);
    if (it != agentFutures.end()) {
      // Agent has active/recent future (possibly re-tasked)
      fut = it->second;
      hasFuture = true;
    }
  }

  // If agent has a future, wait for it
  if (hasFuture) {
    if (timeout.has_value() &&
        fut.wait_for(*timeout) != std::future_status::ready) {
      return std::nullopt;
    }

    AgentOutcome outcome = fut.get();
    {
      std::lock_guard<std::mutex> lock(futuresMutex);
      agentFutures.erase(agentId);
    }
    return outcome;
  }

  // No future - check if agent exists and get outcome from its history
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (agent) {
    // Agent exists, extract outcome from its history
    const std::string finalSummary = extractFinalSummary(agent);
    return makeOutcome(agent, finalSummary);
  }

  // Agent not found anywhere
  return makeFailedOutcome("Agent not found or already waited on.");
}

std::optional<AgentOutcome>
Engine::peekAgentOutcome(const std::string &agentId,
                         std::optional<std::chrono::milliseconds> timeout) {
  std::shared_future<AgentOutcome> fut;
  bool hasFuture = false;
  {
    std::lock_guard<std::mutex> lock(futuresMutex);
    auto it = agentFutures.find(agentId);
    if (it != agentFutures.end()) {
      // Agent has active/recent future (possibly re-tasked)
      fut = it->second;
      hasFuture = true;
    }
  }

  // If agent has a future, peek at it
  if (hasFuture) {
    if (timeout.has_value() &&
        fut.wait_for(*timeout) != std::future_status::ready) {
      return std::nullopt;
    }
    return fut.get();
  }

  // No future - check if agent exists and get outcome from its history
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (agent) {
    // Agent exists, extract outcome from its history
    const std::string finalSummary = extractFinalSummary(agent);
    return makeOutcome(agent, finalSummary);
  }

  // Agent not found anywhere
  return makeFailedOutcome("Agent not found or already waited on.");
}

void Engine::addEventListener(std::function<void(const AppEvent &)> listener) {
  std::lock_guard<std::mutex> lock(listenerMutex);
  listeners.push_back(listener);
}

void Engine::cancelAgent(const std::string &agentId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (agent) {
    cancelAgentRuntime(agent);
    broadcast(AgentInterrupted{agentId, agent->getContext().identity.parentId});
  }
}

std::vector<std::string> Engine::listActiveAgents() const {
  return AgentRegistry::instance().listAll();
}

void Engine::broadcast(const AppEvent &event) {
  std::vector<std::function<void(const AppEvent &)>> listenersCopy;
  {
    std::lock_guard<std::mutex> lock(listenerMutex);
    listenersCopy = listeners;
  }
  for (const auto &listener : listenersCopy) {
    listener(event);
  }
}

void Engine::handleStreamEvent(
    const std::string &agentId, const std::string &parentId,
    const firmius::shared::StreamEvent &ev,
    const std::shared_ptr<std::atomic<bool>> &errorBroadcast) {
  if (auto *txt = std::get_if<TextChunk>(&ev)) {
    broadcast(AgentText{agentId, txt->delta, parentId});
  } else if (auto *thk = std::get_if<ThinkingChunk>(&ev)) {
    broadcast(AgentThinking{agentId, thk->delta, parentId});
  } else if (auto *tcc = std::get_if<ToolCallChunk>(&ev)) {
    broadcast(AgentToolCallChunk{tcc->index, agentId, tcc->id, tcc->nameDelta,
                                 tcc->argsDelta, parentId});
  } else if (auto *tc = std::get_if<ToolCall>(&ev)) {
    broadcast(
        AgentToolCall{agentId, tc->id, tc->name, tc->args, parentId});
  } else if (auto *am = std::get_if<AgentMetrics>(&ev)) {
    broadcast(AgentMetricsStreamed{agentId, *am, parentId});
  } else if (auto *tc = std::get_if<AgentTurnCompleted>(&ev)) {
    broadcast(*tc);
  } else if (auto *fe = std::get_if<AgentFileEdited>(&ev)) {
    broadcast(*fe);
  } else if (auto *ac = std::get_if<AgentCompacting>(&ev)) {
    broadcast(*ac);
  } else if (auto *act = std::get_if<AgentCompactionThinking>(&ev)) {
    broadcast(*act);
  } else if (auto *acx = std::get_if<AgentCompactionText>(&ev)) {
    broadcast(*acx);
  } else if (auto *cc = std::get_if<ContextCompacted>(&ev)) {
    broadcast(*cc);
  } else if (auto *pod = std::get_if<ProcessOutputDelta>(&ev)) {
    broadcast(AgentProcessOutput{agentId, pod->processId, pod->output,
                                 pod->isStderr, pod->finished, pod->exitCode,
                                 pod->durationMs, parentId});
  } else if (auto *sr = std::get_if<StreamRetrying>(&ev)) {
    broadcast(AgentRetrying{agentId, sr->attempt, sr->maxAttempts,
                            sr->httpStatus, sr->delayMs, sr->reason, parentId,
                            sr->accountLocator, sr->details});
  } else if (auto *sre = std::get_if<StreamRetryExhausted>(&ev)) {
    broadcast(
        AgentRetryFailed{agentId, sre->httpStatus, sre->reason, parentId});
  } else if (auto *serr = std::get_if<StreamError>(&ev)) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent && agent->isInterrupted()) {
      return;
    }
    errorBroadcast->store(true, std::memory_order_relaxed);
    std::string msg = serr->message;
    if (!serr->accountLocator.empty()) {
      msg += "\n\n[Account Used]: " + serr->accountLocator;
    }
    broadcast(AgentError{agentId, msg, parentId});
  } else if (auto *sw = std::get_if<StreamAccountSwitched>(&ev)) {
    broadcast(AgentAccountSwitched{agentId, sw->accountLocator, parentId});
  } else if (auto *ps = std::get_if<AgentProcessSpawned>(&ev)) {
    broadcast(AgentProcessSpawned{agentId, ps->processId, ps->toolCallId,
                                  ps->command, parentId});
  } else if (std::holds_alternative<ProviderWaiting>(ev)) {
    broadcast(AgentProviderWaiting{agentId, parentId});
  }
}

void Engine::terminateAgent(const std::string &agentId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (agent) {
    AgentRegistry::instance().unregisterAgent(agentId);
  }
}

void Engine::switchAgentModel(const std::string &agentId,
                              const std::string &providerId,
                              const std::string &modelId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    throw std::runtime_error("Agent not found: " + agentId);
  }

  std::string oldProviderId = agent->getContext().config.providerId;
  std::string oldModelId = agent->getContext().config.modelId;

  agent->setModel(providerId, modelId);

  broadcast(ModelSwitched{agentId, oldProviderId, oldModelId, providerId,
                          modelId, agent->getContext().identity.parentId});
}

void Engine::switchAgentModel(const std::string &agentId,
                              const std::string &providerId,
                              const std::string &modelId,
                              const std::string &variantName) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    throw std::runtime_error("Agent not found: " + agentId);
  }

  std::string oldProviderId = agent->getContext().config.providerId;
  std::string oldModelId = agent->getContext().config.modelId;
  std::string oldVariantName = agent->getContext().config.modelVariant;

  agent->setModel(providerId, modelId, variantName);

  broadcast(ModelSwitched{agentId, oldProviderId, oldModelId, providerId,
                          modelId, agent->getContext().identity.parentId});
}

void Engine::executeTask(
    const std::string &agentId, const std::string &task,
    const std::vector<firmius::shared::ImageContent> &images) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    throw std::runtime_error("Agent not found: " + agentId);
  }

  // Drain any pending internal queue messages (e.g., fleet edit notices)
  // before starting the new task, so the agent sees them on its next turn.
  std::string threadId;
  if (agent->getContext().history) {
    threadId = agent->getContext().history->threadId;
  }
  if (!threadId.empty()) {
    Harness::instance().drainInternalQueueForAgent(agentId, threadId);
  }

  // Mark as active before async dispatch to avoid observers seeing an
  // immediate idle state race while the worker thread is starting.

  auto prom = std::make_shared<std::promise<AgentOutcome>>();
  {
    std::lock_guard<std::mutex> lock(futuresMutex);
    agentFutures[agentId] = prom->get_future().share();
  }

  {
    std::lock_guard<std::mutex> lock(taskThreadsMutex_);
    taskThreads_.emplace_back([this, agentId, task, images, agent,
                               prom]() mutable {
      std::string parentId = "";
      std::string threadId = "";
      // Track if we already broadcast an error from the stream
      auto errorBroadcast = std::make_shared<std::atomic<bool>>(false);
      ArtifactSnapshot runStartArtifacts;
      bool runStartArtifactsCaptured = false;

      try {
        if (agent) {
          parentId = agent->getContext().identity.parentId;
          if (agent->getContext().history) {
            threadId = agent->getContext().history->threadId;
          }
        }
        runStartArtifacts = collectArtifactSnapshot(threadId, agentId);
        runStartArtifactsCaptured = true;
        agent->run(
            task,
            [this, agentId, parentId, errorBroadcast](const StreamEvent &ev) {
              handleStreamEvent(agentId, parentId, ev, errorBroadcast);
            },
            images);

        const std::string finalSummary = extractFinalSummary(agent);
        AgentOutcome outcome = makeOutcome(agent, finalSummary);
        if (runStartArtifactsCaptured) {
          attachArtifactDeltasToOutcome(threadId, agentId, runStartArtifacts,
                                        outcome);
        }
        releaseOwnedChunksForTerminalAgent(agent, outcome);
        if (outcome.kind == AgentOutcome::Kind::Cancelled) {
          broadcast(AgentInterrupted{agentId, parentId});
        }
        broadcast(AgentFinished{agentId, outcome, parentId});
        prom->set_value(outcome);

      } catch (const std::exception &e) {
        // Only broadcast error if we haven't already done so from StreamError
        if (!errorBroadcast) {
          if (agent && agent->getContext().history) {
            firmius::shared::AgentTurn errorTurn;
            errorTurn.turnId =
                "error-" +
                std::to_string(agent->getContext().history->turns.size());
            firmius::shared::Message errorMsg;
            errorMsg.role = firmius::shared::Role::Error;
            errorMsg.content.push_back(firmius::shared::ErrorContent{
                "Engine Error", "Task execution failed.",
                std::string(e.what())});
            errorMsg.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            errorTurn.messages.push_back(errorMsg);
            agent->getMutableContext().history->turns.push_back(errorTurn);
            if (agent->getContext().config.persistHistory) {
              firmius::core::Journaler jnl(
                  agent->getContext().history->threadId, agentId);
              jnl.appendTurn(errorTurn);
            }
          }
          broadcast(AgentError{agentId, e.what(), parentId});
        }
        AgentOutcome outcome = makeFailedOutcome(std::string(e.what()));
        if (runStartArtifactsCaptured) {
          attachArtifactDeltasToOutcome(threadId, agentId, runStartArtifacts,
                                        outcome);
        }
        releaseOwnedChunksForTerminalAgent(agent, outcome);
        broadcast(AgentFinished{agentId, outcome, parentId});
        prom->set_value(outcome);
      }
    });
  }
}

void Engine::resumeTask(const std::string &agentId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    throw std::runtime_error("Agent not found: " + agentId);
  }

  auto prom = std::make_shared<std::promise<AgentOutcome>>();
  {
    std::lock_guard<std::mutex> lock(futuresMutex);
    agentFutures[agentId] = prom->get_future().share();
  }

  {
    std::lock_guard<std::mutex> lock(taskThreadsMutex_);
    taskThreads_.emplace_back([this, agentId, agent, prom]() mutable {
      std::string parentId;
      std::string threadId;
      auto errorBroadcast = std::make_shared<std::atomic<bool>>(false);
      ArtifactSnapshot runStartArtifacts;
      bool runStartArtifactsCaptured = false;

      try {
        parentId = agent->getContext().identity.parentId;
        if (agent->getContext().history) {
          threadId = agent->getContext().history->threadId;
        }
        runStartArtifacts = collectArtifactSnapshot(threadId, agentId);
        runStartArtifactsCaptured = true;
        agent->resume(
            [this, agentId, parentId, errorBroadcast](const StreamEvent &ev) {
              handleStreamEvent(agentId, parentId, ev, errorBroadcast);
            });

        const std::string finalSummary = extractFinalSummary(agent);
        AgentOutcome outcome = makeOutcome(agent, finalSummary);
        if (runStartArtifactsCaptured) {
          attachArtifactDeltasToOutcome(threadId, agentId, runStartArtifacts,
                                        outcome);
        }
        releaseOwnedChunksForTerminalAgent(agent, outcome);
        if (outcome.kind == AgentOutcome::Kind::Cancelled) {
          broadcast(AgentInterrupted{agentId, parentId});
        }
        broadcast(AgentFinished{agentId, outcome, parentId});
        prom->set_value(outcome);
      } catch (const std::exception &e) {
        if (!errorBroadcast) {
          if (agent && agent->getContext().history) {
            firmius::shared::AgentTurn errorTurn;
            errorTurn.turnId =
                "error-" +
                std::to_string(agent->getContext().history->turns.size());
            firmius::shared::Message errorMsg;
            errorMsg.role = firmius::shared::Role::Error;
            errorMsg.content.push_back(firmius::shared::ErrorContent{
                "Engine Error", "Task execution failed.",
                std::string(e.what())});
            errorMsg.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            errorTurn.messages.push_back(errorMsg);
            agent->getMutableContext().history->turns.push_back(errorTurn);
            if (agent->getContext().config.persistHistory) {
              firmius::core::Journaler jnl(
                  agent->getContext().history->threadId, agentId);
              jnl.appendTurn(errorTurn);
            }
          }
          broadcast(AgentError{agentId, e.what(), parentId});
        }
        AgentOutcome outcome = makeFailedOutcome(std::string(e.what()));
        if (runStartArtifactsCaptured) {
          attachArtifactDeltasToOutcome(threadId, agentId, runStartArtifacts,
                                        outcome);
        }
        releaseOwnedChunksForTerminalAgent(agent, outcome);
        broadcast(AgentFinished{agentId, outcome, parentId});
        prom->set_value(outcome);
      }
    });
  }
}

void Engine::compactAgent(const std::string &agentId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    throw std::runtime_error("Agent not found: " + agentId);
  }
  if (agent->isRunning()) {
    throw std::runtime_error("Cannot compact while agent is running");
  }

  {
    std::lock_guard<std::mutex> lock(taskThreadsMutex_);
    taskThreads_.emplace_back([this, agentId, agent]() mutable {
      std::string parentId = "";
      auto errorBroadcast = std::make_shared<std::atomic<bool>>(false);
      try {
        if (agent) {
          parentId = agent->getContext().identity.parentId;
        }
        agent->clearInterrupt();
        agent->compactNow(
            [this, agentId, parentId, errorBroadcast](const StreamEvent &ev) {
              handleStreamEvent(agentId, parentId, ev, errorBroadcast);
            });
        agent->getMutableContext().state.currentStatus = AgentStatus::Idle;
      } catch (const std::exception &e) {
        if (!errorBroadcast->load(std::memory_order_relaxed)) {
          broadcast(AgentError{agentId, e.what(), parentId});
        }
      }
    });
  }
}

UndoResult Engine::undoAgentTurns(const std::string &agentId, int count) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent)
    throw std::runtime_error("Agent not found: " + agentId);
  if (agent->isRunning())
    throw std::runtime_error("Cannot undo while agent is running");

  auto &ctx = agent->getMutableContext();
  if (shouldAttemptCompactionRestore(ctx, count) &&
      restoreCompactionSnapshot(ctx)) {
    agent->saveHistory();
    UndoResult restored;
    restored.compactionReversed = true;
    restored.restoredTurns = static_cast<int>(ctx.history->turns.size());
    broadcast(HistoryUndone{agentId, ctx.history->threadId,
                            restored.turnsRemoved, restored.compactionReversed,
                            ctx.identity.parentId});
    return restored;
  }
  auto result = applyUndoAndRestoreFiles(agent, [&](std::vector<AgentTurn> &turns) {
    return HistoryEditor::undoTurns(turns, count);
  });

  agent->saveHistory();
  broadcast(HistoryUndone{agentId, ctx.history->threadId, result.turnsRemoved,
                          result.compactionReversed, ctx.identity.parentId});
  return result;
}

shared::TranscriptUndoAction Engine::undoAgentTurnsWithRedo(
    const std::string &agentId, int count) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent)
    throw std::runtime_error("Agent not found: " + agentId);
  if (agent->isRunning())
    throw std::runtime_error("Cannot undo while agent is running");
  auto [result, removedTurns] =
      applyUndoAndCaptureRemovedTurns(agent, [&](std::vector<AgentTurn> &turns) {
        return HistoryEditor::undoTurns(turns, count);
      });
  agent->saveHistory();
  const auto &ctx = agent->getContext();
  broadcast(HistoryUndone{agentId, ctx.history->threadId, result.turnsRemoved,
                          result.compactionReversed, ctx.identity.parentId});
  return persistTranscriptUndo(agent, agentId, "turns",
                               std::string("{\"count\":") +
                                   std::to_string(std::max(1, count)) + "}",
                               result.turnsRemoved > 0 ? "undone" : "no-op",
                               std::move(removedTurns));
}

shared::TranscriptUndoAction Engine::undoAgentMessagesWithRedo(
    const std::string &agentId, int count) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent)
    throw std::runtime_error("Agent not found: " + agentId);
  if (agent->isRunning())
    throw std::runtime_error("Cannot undo while agent is running");
  auto [result, removedTurns] =
      applyUndoAndCaptureRemovedTurns(agent, [&](std::vector<AgentTurn> &turns) {
        return HistoryEditor::undoMessages(turns, count);
      });
  agent->saveHistory();
  const auto &ctx = agent->getContext();
  broadcast(HistoryUndone{agentId, ctx.history->threadId, result.turnsRemoved,
                          result.compactionReversed, ctx.identity.parentId});
  return persistTranscriptUndo(agent, agentId, "messages",
                               std::string("{\"count\":") +
                                   std::to_string(std::max(1, count)) + "}",
                               result.turnsRemoved > 0 ? "undone" : "no-op",
                               std::move(removedTurns));
}

shared::TranscriptUndoAction Engine::undoAgentAfterTimestampWithRedo(
    const std::string &agentId, uint64_t timestamp) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent)
    throw std::runtime_error("Agent not found: " + agentId);
  if (agent->isRunning())
    throw std::runtime_error("Cannot undo while agent is running");
  auto [result, removedTurns] = applyUndoAndCaptureRemovedTurns(
      agent, [&](std::vector<AgentTurn> &turns) {
        return HistoryEditor::undoAfterTimestamp(turns, timestamp);
      });
  agent->saveHistory();
  const auto &ctx = agent->getContext();
  broadcast(HistoryUndone{agentId, ctx.history->threadId, result.turnsRemoved,
                          result.compactionReversed, ctx.identity.parentId});
  return persistTranscriptUndo(agent, agentId, "after_timestamp",
                               std::string("{\"timestamp\":") + std::to_string(timestamp) + "}",
                               result.turnsRemoved > 0 ? "undone" : "no-op",
                               std::move(removedTurns));
}

std::vector<shared::EditBatchSummary>
Engine::listAgentEditBatches(const std::string &threadId,
                             const shared::EditHistoryFilters &filters) const {
  if (threadId.empty()) {
    return {};
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  return tm.listEditBatches(threadId, filters);
}

shared::EditUndoEligibility
Engine::evaluateEditBatchUndo(const std::string &threadId,
                              const std::string &editBatchId) const {
  static const std::unordered_set<std::string> kEmpty;
  return evaluateEditBatchUndo(threadId, editBatchId, kEmpty);
}

shared::EditUndoEligibility
Engine::evaluateEditBatchUndo(const std::string &threadId,
                              const std::string &editBatchId,
                              const std::unordered_set<std::string> &coUndoBatchIds) const {
  shared::EditUndoEligibility eligibility;
  eligibility.editBatchId = editBatchId;
  if (threadId.empty() || editBatchId.empty()) {
    eligibility.resultStatus = shared::EditUndoResultStatus::RejectedBatchNotFullyUndoable;
    eligibility.reason = "Missing thread or edit batch id";
    return eligibility;
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  const auto detail = tm.getEditBatch(threadId, editBatchId);
  if (detail.summary.status == shared::EditBatchStatus::Undone) {
    eligibility.resultStatus = shared::EditUndoResultStatus::RejectedAlreadyUndone;
    eligibility.reason = "Edit batch is already undone";
    return eligibility;
  }
  // First pass: per-mutation undoability.
  // - Line-edit-style (had+has): always undoable; the reverse-line walk
  //   handles it.
  // - Create-style (!had+has): undoable IFF the file on disk still
  //   matches the post-write fingerprint. If the user touched it after
  //   creation, deleting blindly would be data loss.
  // - Delete-style (had+!has) and weird (!had+!has): not yet supported.
  //   Left as future work — would need to write back the recorded
  //   pre-image, which we don't always store fully.
  for (const auto &mutation : detail.files) {
    const bool createStyle = !mutation.hadFileBefore && mutation.hasFileAfter;
    const bool lineStyle   =  mutation.hadFileBefore && mutation.hasFileAfter;
    if (!createStyle && !lineStyle) {
      eligibility.resultStatus =
          shared::EditUndoResultStatus::RejectedBatchNotFullyUndoable;
      eligibility.reason =
          "Delete-style file undo is not yet supported safely: " + mutation.filePath;
      return eligibility;
    }
    if (createStyle) {
      // Verify the file on disk still matches what we created. We can't
      // call host->readFile from a const helper without knowing the host,
      // so the actual disk-tamper check is deferred to undoEditBatch
      // itself (which has the agent + host). Here we just gate on
      // the post-image fingerprint being known so undo has something to
      // verify against. If postHash is missing the mutation predates
      // this code path and we conservatively reject.
      if (mutation.postHash.empty()) {
        eligibility.resultStatus =
            shared::EditUndoResultStatus::RejectedBatchNotFullyUndoable;
        eligibility.reason =
            "Cannot safely undo create without a recorded post-image: " + mutation.filePath;
        return eligibility;
      }
    }
  }
  // Second pass: blocker check. Any later applied mutation on the same
  // file blocks this undo, UNLESS that mutation's batch is in the
  // co-undo set (caller is undoing both in the same compound).
  for (const auto &mutation : detail.files) {
    const auto history = tm.listEditFileMutationsForFile(threadId, mutation.filePath);
    for (const auto &candidate : history) {
      if (candidate.fileMutationId == mutation.fileMutationId) {
        break;
      }
      if (candidate.status != shared::EditFileMutationStatus::Applied) {
        continue;
      }
      if (coUndoBatchIds.count(candidate.editBatchId) > 0) {
        // Will unwind in the same compound — not a blocker.
        continue;
      }
      eligibility.resultStatus = shared::EditUndoResultStatus::RejectedBlocked;
      eligibility.reason = "Later edits exist on file: " + mutation.filePath;
      eligibility.blockingEditBatchIds.push_back(candidate.editBatchId);
      return eligibility;
    }
  }
  eligibility.undoable = true;
  eligibility.resultStatus = shared::EditUndoResultStatus::Succeeded;
  return eligibility;
}


shared::EditRedoEligibility
Engine::evaluateEditBatchRedo(const std::string &threadId,
                              const std::string &undoActionId) const {
  shared::EditRedoEligibility eligibility;
  eligibility.undoActionId = undoActionId;
  if (threadId.empty() || undoActionId.empty()) {
    eligibility.reason = "Missing thread or undo action id";
    return eligibility;
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  auto undoAction = tm.findEditUndoAction(threadId, undoActionId);
  if (!undoAction.has_value()) {
    eligibility.reason = "Edit undo action not found";
    return eligibility;
  }
  const auto detail = tm.getEditBatch(threadId, undoAction->targetEditBatchId);
  if (detail.summary.status != shared::EditBatchStatus::Undone) {
    eligibility.reason = "Target edit batch is not currently undone";
    return eligibility;
  }
  eligibility.redoable = true;
  eligibility.reason = "redoable";
  return eligibility;
}
shared::EditUndoAction
Engine::undoEditBatch(const std::string &agentId, const std::string &editBatchId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    throw std::runtime_error("Agent not found: " + agentId);
  }
  if (agent->isRunning()) {
    throw std::runtime_error("Cannot undo edit batch while agent is running");
  }
  const auto &ctx = agent->getContext();
  if (!ctx.history) {
    throw std::runtime_error("Agent has no persisted history");
  }
  const std::string threadId = ctx.history->threadId;
  ThreadManager tm(ThreadManager::defaultBasePath());
  auto eligibility = evaluateEditBatchUndo(threadId, editBatchId);
  shared::EditUndoAction action;

  action.undoActionId = shared::StringUtil::generateUuid();
  action.threadId = threadId;
  action.requestedByAgentId = agentId;
  action.targetEditBatchId = editBatchId;
  action.createdAt = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  action.resultStatus = eligibility.resultStatus;

  action.resultJson = firmius::shared::toJsonString(shared::toJson(eligibility));
  if (eligibility.undoable) {
    const auto detail = tm.getEditBatch(threadId, editBatchId);
    for (auto it = detail.files.rbegin(); it != detail.files.rend(); ++it) {
      const auto &mutation = *it;
      const std::string absolutePath =
          agent->getEnvironment()->getWorkspace().resolvePath(mutation.filePath);
      const bool createStyle = !mutation.hadFileBefore && mutation.hasFileAfter;

      if (createStyle) {
        // Verify the file on disk is exactly what we created. If the
        // user has touched it after creation we refuse — deleting their
        // changes would be data loss.
        if (agent->getHost()->exists(absolutePath)) {
          const auto bytes = agent->getHost()->readFile(absolutePath);
          const std::string current(bytes.begin(), bytes.end());
          // Normalize via the same split/join the recorder used so a
          // trailing-newline difference doesn't trip the check.
          const std::string normalized =
              joinStoredLines(splitStoredLines(current));
          // computeContentFingerprint = hash + "-" + size, but we don't
          // have the helper here. Using a size+hash equivalence: the
          // recorder stored postSize and postHash, both must match.
          const std::string currentFp =
              shared::utils::Hashline::computeHash(normalized) + "-" +
              std::to_string(normalized.size());
          if (currentFp != mutation.postHash) {
            // Tampered — abort the whole batch undo; the per-batch
            // status stays Applied so caller can surface the diverged
            // result to the user.
            action.resultStatus = shared::EditUndoResultStatus::RejectedDiverged;
            action.resultJson =
                std::string("{\"status\":\"diverged\",\"file\":\"") +
                mutation.filePath + "\"}";
            tm.writeEditUndoAction(threadId, action);
            return action;
          }
        }
        // Delete is idempotent on missing — host implementations all
        // tolerate the file being already gone.
        agent->getHost()->deleteFile(absolutePath);
        agent->getEnvironment()->getWorkspace().recordFileEdit(absolutePath);
        tm.updateEditFileMutationStatus(threadId, mutation.fileMutationId,
                                        shared::EditFileMutationStatus::Undone);
        continue;
      }

      std::vector<std::string> currentLines;
      if (agent->getHost()->exists(absolutePath)) {
        const auto bytes = agent->getHost()->readFile(absolutePath);
        currentLines = splitStoredLines(std::string(bytes.begin(), bytes.end()));
      }
      for (auto opIt = mutation.operations.rbegin(); opIt != mutation.operations.rend(); ++opIt) {
        const auto &op = *opIt;
        const int startIndex = std::max(0, op.startLine - 1);
        const int newCount = static_cast<int>(op.newLines.size());
        const int eraseEnd =
            std::min(static_cast<int>(currentLines.size()), startIndex + newCount);
        if (startIndex <= static_cast<int>(currentLines.size())) {
          currentLines.erase(currentLines.begin() + startIndex,
                             currentLines.begin() + eraseEnd);
          currentLines.insert(currentLines.begin() + startIndex,
                              op.oldLines.begin(), op.oldLines.end());
        }
      }
      const std::string restored = joinStoredLines(currentLines);
      agent->getHost()->writeFile(
          absolutePath, std::vector<uint8_t>(restored.begin(), restored.end()));
      agent->getEnvironment()->getWorkspace().recordFileEdit(absolutePath);
      tm.updateEditFileMutationStatus(threadId, mutation.fileMutationId,
                                      shared::EditFileMutationStatus::Undone);
    }
    tm.updateEditBatchStatus(threadId, editBatchId, shared::EditBatchStatus::Undone,
                             action.undoActionId);
    action.resultJson = "{\"status\":\"undone\"}";
    action.resultStatus = shared::EditUndoResultStatus::Succeeded;

  }
  tm.writeEditUndoAction(threadId, action);
  return action;
}


std::optional<shared::EditRedoAction>
Engine::redoEditUndoAction(const std::string &agentId,
                           const std::string &undoActionId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    throw std::runtime_error("Agent not found: " + agentId);
  }
  if (agent->isRunning()) {
    throw std::runtime_error("Cannot redo edit batch while agent is running");
  }
  const auto &ctx = agent->getContext();
  if (!ctx.history) {
    throw std::runtime_error("Agent has no persisted history");
  }
  const std::string threadId = ctx.history->threadId;
  ThreadManager tm(ThreadManager::defaultBasePath());
  auto eligibility = evaluateEditBatchRedo(threadId, undoActionId);
  if (!eligibility.redoable) {
    return std::nullopt;
  }
  auto undoAction = tm.findEditUndoAction(threadId, undoActionId);
  if (!undoAction.has_value()) {
    return std::nullopt;
  }
  const auto detail = tm.getEditBatch(threadId, undoAction->targetEditBatchId);
  for (const auto &mutation : detail.files) {
    const std::string absolutePath =
        agent->getEnvironment()->getWorkspace().resolvePath(mutation.filePath);
    const bool createStyle = !mutation.hadFileBefore && mutation.hasFileAfter;

    if (createStyle) {
      // The file should be absent right now (we deleted it during
      // undo). Re-create from the recorded post-image: the operations
      // array's first op carries newLines spanning the full file.
      // Even for multi-op create batches (rare but possible) we just
      // join all newLines in order — the recorder built them that way.
      std::vector<std::string> lines;
      for (const auto &op : mutation.operations) {
        for (const auto &ln : op.newLines) {
          lines.push_back(ln);
        }
      }
      const std::string content = joinStoredLines(lines);
      agent->getHost()->writeFile(
          absolutePath, std::vector<uint8_t>(content.begin(), content.end()));
      agent->getEnvironment()->getWorkspace().recordFileEdit(absolutePath);
      tm.updateEditFileMutationStatus(threadId, mutation.fileMutationId,
                                      shared::EditFileMutationStatus::Redone);
      continue;
    }

    std::vector<std::string> currentLines;
    if (agent->getHost()->exists(absolutePath)) {
      const auto bytes = agent->getHost()->readFile(absolutePath);
      currentLines = splitStoredLines(std::string(bytes.begin(), bytes.end()));
    }
    for (const auto &op : mutation.operations) {
      const int startIndex = std::max(0, op.startLine - 1);
      const int oldCount = static_cast<int>(op.oldLines.size());
      const int eraseEnd = std::min(static_cast<int>(currentLines.size()), startIndex + oldCount);
      if (startIndex <= static_cast<int>(currentLines.size())) {
        currentLines.erase(currentLines.begin() + startIndex, currentLines.begin() + eraseEnd);
        currentLines.insert(currentLines.begin() + startIndex, op.newLines.begin(), op.newLines.end());
      }
    }
    const std::string rewritten = joinStoredLines(currentLines);
    agent->getHost()->writeFile(absolutePath, std::vector<uint8_t>(rewritten.begin(), rewritten.end()));
    agent->getEnvironment()->getWorkspace().recordFileEdit(absolutePath);
    tm.updateEditFileMutationStatus(threadId, mutation.fileMutationId, shared::EditFileMutationStatus::Redone);
  }
  tm.updateEditBatchStatus(threadId, undoAction->targetEditBatchId, shared::EditBatchStatus::Redone, std::nullopt);
  shared::EditRedoAction redoAction{shared::StringUtil::generateUuid(), threadId, undoActionId, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()), "{\"status\":\"redone\"}"};
  tm.writeEditRedoAction(threadId, redoAction);
  return redoAction;
}

shared::TranscriptRedoEligibility Engine::evaluateTranscriptRedo(
    const std::string &threadId, const std::string &undoActionId) const {
  shared::TranscriptRedoEligibility eligibility;
  eligibility.undoActionId = undoActionId;
  if (threadId.empty() || undoActionId.empty()) {
    eligibility.reason = "Missing thread or undo action id";
    return eligibility;
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  auto action = tm.findTranscriptUndoAction(threadId, undoActionId);
  if (!action.has_value()) {
    eligibility.reason = "Transcript undo action not found";
    return eligibility;
  }
  eligibility.redoable = action->redoAvailable;
  eligibility.reason = action->redoAvailable ? "redoable" : "redo payload unavailable";
  return eligibility;
}

std::optional<shared::TranscriptRedoAction> Engine::redoTranscriptUndoAction(
    const std::string &agentId, const std::string &undoActionId) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent || agent->isRunning() || !agent->getContext().history) {
    return std::nullopt;
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  auto action = tm.findTranscriptUndoAction(agent->getContext().history->threadId, undoActionId);
  if (!action.has_value() || !action->redoAvailable) {
    return std::nullopt;
  }
  const auto payloads = tm.loadTranscriptRedoPayloads(action->threadId, undoActionId);
  for (const auto &payload : payloads) appendTurnsForTranscriptRedo(agent, payload.turns);
  agent->saveHistory();
  tm.markTranscriptUndoRedoAvailability(action->threadId, undoActionId, false);
  return shared::TranscriptRedoAction{shared::StringUtil::generateUuid(), undoActionId, action->threadId, agentId, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()), "{\"status\":\"redone\"}"};
}
UndoResult Engine::undoAgentMessages(const std::string &agentId, int count) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent)
    throw std::runtime_error("Agent not found: " + agentId);
  if (agent->isRunning())
    throw std::runtime_error("Cannot undo while agent is running");

  auto &ctx = agent->getMutableContext();
  if (shouldAttemptCompactionRestore(ctx, count) &&
      restoreCompactionSnapshot(ctx)) {
    agent->saveHistory();
    UndoResult restored;
    restored.compactionReversed = true;
    restored.restoredTurns = static_cast<int>(ctx.history->turns.size());
    broadcast(HistoryUndone{agentId, ctx.history->threadId,
                            restored.turnsRemoved, restored.compactionReversed,
                            ctx.identity.parentId});
    return restored;
  }
  auto result = applyUndoAndRestoreFiles(agent, [&](std::vector<AgentTurn> &turns) {
    return HistoryEditor::undoMessages(turns, count);
  });

  agent->saveHistory();

  broadcast(HistoryUndone{agentId, ctx.history->threadId, result.turnsRemoved,
                          result.compactionReversed, ctx.identity.parentId});
  return result;
}

UndoResult Engine::undoAgentAfterTimestamp(const std::string &agentId,
                                           uint64_t timestamp) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent)
    throw std::runtime_error("Agent not found: " + agentId);
  if (agent->isRunning())
    throw std::runtime_error("Cannot undo while agent is running");

  auto &ctx = agent->getMutableContext();
  bool compactionAfterTimestamp = false;
  for (std::size_t i = 2; i < ctx.history->turns.size(); ++i) {
    const auto &turn = ctx.history->turns[i];
    if (turn.messages.empty() || turn.messages.front().timestamp <= timestamp) {
      continue;
    }
    if (hasCompactionMarker(turn)) {
      compactionAfterTimestamp = true;
      break;
    }
  }
  if (compactionAfterTimestamp && restoreCompactionSnapshot(ctx)) {
    agent->saveHistory();
    UndoResult restored;
    restored.compactionReversed = true;
    restored.restoredTurns = static_cast<int>(ctx.history->turns.size());
    broadcast(HistoryUndone{agentId, ctx.history->threadId,
                            restored.turnsRemoved, restored.compactionReversed,
                            ctx.identity.parentId});
    return restored;
  }
  auto result = applyUndoAndRestoreFiles(agent, [&](std::vector<AgentTurn> &turns) {
    return HistoryEditor::undoAfterTimestamp(turns, timestamp);
  });

  agent->saveHistory();

  broadcast(HistoryUndone{agentId, ctx.history->threadId, result.turnsRemoved,
                          result.compactionReversed, ctx.identity.parentId});
  return result;
}

void Engine::shutdown() {
  LspServerManager::instance().shutdownAll();
  auto activeAgents = AgentRegistry::instance().listAll();
  for (const auto &id : activeAgents) {
    auto agent = AgentRegistry::instance().getAgent(id);
    if (agent) {
      cancelAgentRuntime(agent);
    }
  }

  std::vector<std::jthread> taskThreads;
  {
    std::lock_guard<std::mutex> lock(taskThreadsMutex_);
    taskThreads.swap(taskThreads_);
  }

  std::vector<std::jthread> fleetThreads;
  {
    std::lock_guard<std::mutex> lock(fleetMutex);
    fleetThreads.swap(fleet);
  }

  // Shut down all per-thread working-memory workers (background embedding
  // threads). Idempotent and safe even if no workers were ever created.
  working_memory::WorkingMemoryWorker::instance().shutdownAll();
}

std::vector<firmius::provider::ToolDefinition>
Engine::getAvailableToolDefinitionsForPermissions(
    const firmius::shared::AgentPermissions &permissions) const {
  return toolRegistry.getAvailableToolDefinitions(permissions);
}

} // namespace firmius::core
