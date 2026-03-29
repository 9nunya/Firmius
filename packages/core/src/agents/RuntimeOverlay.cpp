#include "agents/RuntimeOverlay.hpp"

#include "agents/PurposeLoader.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/Hashline.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace firmius::core::runtime_overlay {

namespace {

constexpr std::size_t kMaxRememberedWatchedFiles = 8;
constexpr std::size_t kMaxRenderedWatchedFiles = 4;
constexpr int kMaxRenderedWatchedLines = 800;
constexpr std::size_t kMaxFieldLen = 240;

struct LocatedChunk {
  shared::Plan plan;
  shared::WorkChunk chunk;
};

std::uint64_t nowEpochMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string fingerprintText(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (char ch : value) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }

  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

std::vector<std::string> splitLines(const std::string& content) {
  std::vector<std::string> lines;
  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

void mergeRange(std::vector<WatchedLineRange>& ranges, int startLine, int endLine) {
  if (startLine <= 0 || endLine < startLine) {
    return;
  }

  ranges.push_back({startLine, endLine});
  std::sort(ranges.begin(), ranges.end(),
            [](const WatchedLineRange& lhs, const WatchedLineRange& rhs) {
              if (lhs.startLine != rhs.startLine) {
                return lhs.startLine < rhs.startLine;
              }
              return lhs.endLine < rhs.endLine;
            });

  std::vector<WatchedLineRange> merged;
  for (const auto& range : ranges) {
    if (merged.empty() || range.startLine > merged.back().endLine + 1) {
      merged.push_back(range);
      continue;
    }
    merged.back().endLine = std::max(merged.back().endLine, range.endLine);
  }
  ranges = std::move(merged);
}

bool isFullyCovered(const WatchedFileState& watchedFile) {
  if (!watchedFile.terminalLine.has_value() || watchedFile.ranges.empty()) {
    return false;
  }

  return watchedFile.ranges.size() == 1 &&
         watchedFile.ranges.front().startLine <= 1 &&
         watchedFile.ranges.front().endLine >= *watchedFile.terminalLine;
}

std::string trimForPrompt(const std::string& value, std::size_t maxLen = kMaxFieldLen) {
  const std::string trimmed = shared::StringUtil::trim(value);
  if (trimmed.size() <= maxLen) {
    return trimmed;
  }
  return trimmed.substr(0, maxLen) + "...";
}

const char* chunkStatusLabel(shared::WorkChunkStatus status) {
  switch (status) {
  case shared::WorkChunkStatus::Ready:
    return "Ready";
  case shared::WorkChunkStatus::InProgress:
    return "InProgress";
  case shared::WorkChunkStatus::Implemented:
    return "Implemented";
  case shared::WorkChunkStatus::Verifying:
    return "Verifying";
  case shared::WorkChunkStatus::Done:
    return "Done";
  case shared::WorkChunkStatus::Blocked:
    return "Blocked";
  case shared::WorkChunkStatus::Failed:
    return "Failed";
  case shared::WorkChunkStatus::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

const char* planStatusLabel(shared::PlanStatus status) {
  switch (status) {
  case shared::PlanStatus::Draft:
    return "Draft";
  case shared::PlanStatus::Active:
    return "Active";
  case shared::PlanStatus::Paused:
    return "Paused";
  case shared::PlanStatus::Done:
    return "Done";
  case shared::PlanStatus::Abandoned:
    return "Abandoned";
  }
  return "Unknown";
}

const char* todoStatusLabel(shared::TodoStatus status) {
  switch (status) {
  case shared::TodoStatus::Pending:
    return "Pending";
  case shared::TodoStatus::InProgress:
    return "InProgress";
  case shared::TodoStatus::Done:
    return "Done";
  }
  return "Unknown";
}

shared::AgentTurn makeOverlayTurn(const std::string& turnId,
                                  const std::string& text) {
  shared::AgentTurn turn;
  turn.turnId = turnId;

  shared::Message msg;
  msg.role = shared::Role::System;
  msg.visibility = shared::MessageVisibility::Internal;
  msg.content.push_back(shared::TextContent{text});
  msg.timestamp = nowEpochMs();
  turn.messages.push_back(std::move(msg));
  return turn;
}

shared::AgentTodoList readTodoList(const shared::AgentContext& context,
                                   ThreadManager& tm) {
  shared::AgentTodoList todo;
  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty()) {
    return todo;
  }

  return tm.getAgentTodo(context.history->threadId, context.identity.id);
}

std::string renderTodoList(const shared::AgentTodoList& todo) {
  std::ostringstream out;
  out << "Todo\n";
  if (todo.items.empty()) {
    out << "- (none)\n";
    return out.str();
  }

  for (const auto& item : todo.items) {
    out << "- #" << item.id << " [" << todoStatusLabel(item.status) << "] "
        << trimForPrompt(item.text, 200) << "\n";
  }
  return out.str();
}

void appendChunkDetails(std::ostringstream& out, const shared::WorkChunk& chunk,
                        bool includeTasks) {
  out << "- [" << chunkStatusLabel(chunk.status) << "] " << chunk.title;
  if (!chunk.id.empty()) {
    out << " (id=" << chunk.id << ")";
  }
  if (!chunk.assignedAgentId.empty()) {
    out << " assignee=" << chunk.assignedAgentId;
  }
  out << "\n";
  if (!shared::StringUtil::trim(chunk.goal).empty()) {
    out << "  goal: " << trimForPrompt(chunk.goal) << "\n";
  }
  if (!shared::StringUtil::trim(chunk.context).empty()) {
    out << "  context: " << trimForPrompt(chunk.context) << "\n";
  }
  if (!shared::StringUtil::trim(chunk.constraints).empty()) {
    out << "  constraints: " << trimForPrompt(chunk.constraints) << "\n";
  }
  if (!shared::StringUtil::trim(chunk.verificationCondition).empty()) {
    out << "  verify: " << trimForPrompt(chunk.verificationCondition) << "\n";
  }
  if (!includeTasks || chunk.tasks.empty()) {
    return;
  }

  for (const auto& task : chunk.tasks) {
    out << "  task[" << chunkStatusLabel(task.status) << "]: "
        << trimForPrompt(task.title, 180) << "\n";
    if (!shared::StringUtil::trim(task.goal).empty()) {
      out << "    goal: " << trimForPrompt(task.goal) << "\n";
    }
    if (!shared::StringUtil::trim(task.verificationCondition).empty()) {
      out << "    verify: " << trimForPrompt(task.verificationCondition) << "\n";
    }
  }
}

std::optional<LocatedChunk> locateAssignedChunk(ThreadManager& tm,
                                                const std::string& threadId,
                                                const std::string& agentId) {
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }

  try {
    const shared::ThreadMetadata metadata = tm.getMetadata(threadId);
    if (!metadata.activePlanId.empty()) {
      const shared::Plan activePlan = tm.getPlan(threadId, metadata.activePlanId);
      for (const auto& chunk : activePlan.chunks) {
        if (chunk.assignedAgentId == agentId) {
          return LocatedChunk{activePlan, chunk};
        }
      }
    }
  } catch (...) {
  }

  for (const auto& plan : tm.listPlans(threadId)) {
    for (const auto& chunk : plan.chunks) {
      if (chunk.assignedAgentId == agentId) {
        return LocatedChunk{plan, chunk};
      }
    }
  }

  return std::nullopt;
}

std::string buildLeadOverlay(const shared::AgentContext& context,
                             ThreadManager& tm) {
  std::ostringstream out;
  out << "## LIVE WORK STATE\n";
  out << "Role: lead\n";

  if (!context.history || context.history->threadId.empty()) {
    out << "Thread: unavailable\n\n";
    out << "Todo\n- (none)\n";
    return out.str();
  }

  try {
    const shared::ThreadMetadata metadata = tm.getMetadata(context.history->threadId);
    if (metadata.activePlanId.empty()) {
      out << "Active plan: none\n\n";
    } else {
      const shared::Plan plan = tm.getPlan(context.history->threadId, metadata.activePlanId);
      out << "Plan ID: " << plan.id << "\n";
      out << "Plan Title: " << trimForPrompt(plan.title) << "\n";
      out << "Plan Status: " << planStatusLabel(plan.status) << "\n";
      if (!shared::StringUtil::trim(plan.objective).empty()) {
        out << "Objective: " << trimForPrompt(plan.objective) << "\n";
      }
      if (!shared::StringUtil::trim(plan.strategy).empty()) {
        out << "Strategy: " << trimForPrompt(plan.strategy) << "\n";
      }
      out << "\nPlan Chunks\n";
      if (plan.chunks.empty()) {
        out << "- (none)\n";
      } else {
        for (const auto& chunk : plan.chunks) {
          appendChunkDetails(out, chunk, true);
        }
      }
      out << "\n";
    }
  } catch (...) {
    out << "Active plan: unavailable\n\n";
  }

  out << renderTodoList(readTodoList(context, tm));
  return out.str();
}

std::string buildExecutorOverlay(const shared::AgentContext& context,
                                 ThreadManager& tm) {
  std::ostringstream out;
  out << "## LIVE WORK STATE\n";
  out << "Role: executor\n";

  const auto located = locateAssignedChunk(
      tm, context.history ? context.history->threadId : "", context.identity.id);
  if (!located.has_value()) {
    out << "Assigned chunk: none\n\n";
    out << renderTodoList(readTodoList(context, tm));
    return out.str();
  }

  out << "Plan ID: " << located->plan.id << "\n";
  out << "Plan Title: " << trimForPrompt(located->plan.title) << "\n";
  if (!shared::StringUtil::trim(located->plan.objective).empty()) {
    out << "Plan Objective: " << trimForPrompt(located->plan.objective) << "\n";
  }
  out << "\nAssigned Chunk\n";
  appendChunkDetails(out, located->chunk, true);
  out << "\n";
  out << renderTodoList(readTodoList(context, tm));
  return out.str();
}

std::string buildWorkerOverlay(const shared::AgentContext& context,
                               ThreadManager& tm, const char* roleLabel) {
  std::ostringstream out;
  out << "## LIVE WORK STATE\n";
  out << "Role: " << roleLabel << "\n\n";
  out << renderTodoList(readTodoList(context, tm));
  return out.str();
}

std::string buildWorkOverlay(const shared::AgentContext& context) {
  if (!context.history || context.history->threadId.empty()) {
    return "## LIVE WORK STATE\nThread: unavailable\n";
  }

  ThreadManager tm(ThreadManager::defaultBasePath());
  switch (PurposeLoader::resolveWorkRole(context.config.personaName)) {
  case PurposeWorkRole::Lead:
    return buildLeadOverlay(context, tm);
  case PurposeWorkRole::Executor:
  case PurposeWorkRole::Auditor:
    return buildExecutorOverlay(context, tm);
  case PurposeWorkRole::Worker:
    return buildWorkerOverlay(context, tm, "worker");
  case PurposeWorkRole::Scout:
    return buildWorkerOverlay(context, tm, "scout");
  case PurposeWorkRole::Unknown:
    return buildWorkerOverlay(context, tm, "unknown");
  }
  return buildWorkerOverlay(context, tm, "unknown");
}

void touchWatchedEntry(AgentLiveState& liveState, const std::string& absolutePath) {
  auto it = std::find_if(liveState.watchedFiles.begin(), liveState.watchedFiles.end(),
                         [&](const WatchedFileState& watchedFile) {
                           return watchedFile.path == absolutePath;
                         });
  if (it == liveState.watchedFiles.end()) {
    return;
  }

  WatchedFileState moved = *it;
  liveState.watchedFiles.erase(it);
  liveState.watchedFiles.push_back(std::move(moved));
}

WatchedFileState& ensureWatchedEntry(AgentLiveState& liveState,
                                     const std::string& absolutePath) {
  auto it = std::find_if(liveState.watchedFiles.begin(), liveState.watchedFiles.end(),
                         [&](const WatchedFileState& watchedFile) {
                           return watchedFile.path == absolutePath;
                         });
  if (it == liveState.watchedFiles.end()) {
    liveState.watchedFiles.push_back(WatchedFileState{});
    it = std::prev(liveState.watchedFiles.end());
    it->path = absolutePath;
  }

  touchWatchedEntry(liveState, absolutePath);
  return liveState.watchedFiles.back();
}

void trimRememberedWatchedFiles(AgentLiveState& liveState) {
  if (liveState.watchedFiles.size() <= kMaxRememberedWatchedFiles) {
    return;
  }
  liveState.watchedFiles.erase(
      liveState.watchedFiles.begin(),
      liveState.watchedFiles.begin() +
          static_cast<std::ptrdiff_t>(liveState.watchedFiles.size() -
                                      kMaxRememberedWatchedFiles));
}

std::string renderWatchedFileSlice(const std::string& content,
                                   const WatchedFileState& watchedFile,
                                   int* lineCountOut = nullptr) {
  const auto lines = splitLines(content);
  int emitted = 0;
  std::ostringstream out;

  auto emitLine = [&](int lineNumber) {
    if (lineNumber <= 0 || lineNumber > static_cast<int>(lines.size())) {
      return;
    }
    out << shared::utils::Hashline::formatLine(lines, lineNumber) << "\n";
    ++emitted;
  };

  if (watchedFile.fullyRead) {
    for (int lineNumber = 1; lineNumber <= static_cast<int>(lines.size()); ++lineNumber) {
      emitLine(lineNumber);
    }
  } else {
    bool firstRange = true;
    for (const auto& range : watchedFile.ranges) {
      if (!firstRange) {
        out << "... gap ...\n";
      }
      firstRange = false;
      const int startLine = std::max(1, range.startLine);
      const int endLine = std::min(range.endLine, static_cast<int>(lines.size()));
      for (int lineNumber = startLine; lineNumber <= endLine; ++lineNumber) {
        emitLine(lineNumber);
      }
    }
  }

  if (lineCountOut) {
    *lineCountOut = emitted;
  }
  return out.str();
}

void synchronizeWatchedHash(ThreadManager& tm, const std::string& threadId,
                            const std::string& agentId,
                            const std::vector<std::pair<std::string, std::string>>& hashes) {
  if (hashes.empty()) {
    return;
  }

  tm.mutateAgentLiveState(threadId, agentId, [&](AgentLiveState& liveState) {
    for (const auto& [path, hash] : hashes) {
      auto it = std::find_if(liveState.watchedFiles.begin(), liveState.watchedFiles.end(),
                             [&](const WatchedFileState& watchedFile) {
                               return watchedFile.path == path;
                             });
      if (it != liveState.watchedFiles.end()) {
        it->lastContentHash = hash;
        it->updatedAt = nowEpochMs();
      }
    }
  });
}

std::string buildWatchedFilesOverlay(const shared::AgentContext& context,
                                     shared::IHost& host,
                                     const shared::IWorkspace&) {
  std::ostringstream out;
  out << "## WATCHED FILES\n";

  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty()) {
    out << "(unavailable)\n";
    return out.str();
  }

  ThreadManager tm(ThreadManager::defaultBasePath());
  const AgentLiveState liveState =
      tm.getAgentLiveState(context.history->threadId, context.identity.id);
  if (liveState.watchedFiles.empty()) {
    out << "(none)\n";
    return out.str();
  }

  std::vector<const WatchedFileState*> selected;
  selected.reserve(std::min(kMaxRenderedWatchedFiles, liveState.watchedFiles.size()));
  int renderedLineBudget = 0;

  for (auto it = liveState.watchedFiles.rbegin(); it != liveState.watchedFiles.rend(); ++it) {
    if (selected.size() >= kMaxRenderedWatchedFiles ||
        renderedLineBudget >= kMaxRenderedWatchedLines) {
      break;
    }
    selected.push_back(&*it);
    if (it->fullyRead && it->terminalLine.has_value()) {
      renderedLineBudget += *it->terminalLine;
    } else {
      for (const auto& range : it->ranges) {
        renderedLineBudget += std::max(0, range.endLine - range.startLine + 1);
      }
    }
  }
  std::reverse(selected.begin(), selected.end());

  std::vector<std::pair<std::string, std::string>> updatedHashes;
  int actualRenderedLines = 0;
  for (const auto* watchedFile : selected) {
    out << "<file path=\"" << watchedFile->path << "\">\n";
    try {
      const auto bytes = host.readFile(watchedFile->path);
      const std::string content(bytes.begin(), bytes.end());
      int linesRendered = 0;
      const std::string rendered =
          renderWatchedFileSlice(content, *watchedFile, &linesRendered);
      if (!watchedFile->fullyRead) {
        out << "# note: partial watch only; read the entire file before editing this file\n";
      }
      const std::string currentHash = fingerprintText(rendered);
      if (!watchedFile->lastContentHash.empty() &&
          watchedFile->lastContentHash != currentHash) {
        out << "# status: updated from disk since last sync\n";
      }
      if (shared::StringUtil::trim(rendered).empty()) {
        out << "(empty)\n";
      } else {
        out << rendered;
      }
      updatedHashes.emplace_back(watchedFile->path, currentHash);
      actualRenderedLines += linesRendered;
    } catch (const std::exception& ex) {
      out << "# status: unavailable (" << ex.what() << ")\n";
    }
    out << "</file>\n";
  }

  const std::size_t omitted =
      liveState.watchedFiles.size() > selected.size()
          ? liveState.watchedFiles.size() - selected.size()
          : 0;
  if (omitted > 0) {
    out << "# " << omitted
        << " additional watched file(s) omitted for runtime budget\n";
  }
  if (actualRenderedLines > kMaxRenderedWatchedLines) {
    out << "# watched file line budget exceeded; recent files were prioritized\n";
  }

  synchronizeWatchedHash(tm, context.history->threadId, context.identity.id,
                         updatedHashes);
  return out.str();
}

std::optional<std::string> buildWatchedFilesPayload(
    const shared::AgentContext& context, shared::IHost& host,
    shared::IWorkspace& workspace) {
  const std::string overlay = buildWatchedFilesOverlay(context, host, workspace);
  const std::string prefix = "## WATCHED FILES\n";
  if (!overlay.starts_with(prefix)) {
    return std::nullopt;
  }

  const std::string payload = overlay.substr(prefix.size());
  if (payload == "(none)\n" || payload == "(unavailable)\n") {
    return std::nullopt;
  }
  return payload;
}

bool rewriteToolResultJsonField(std::string& jsonText, const char* fieldName,
                                const std::optional<std::string>& fieldValue) {
  rapidjson::Document doc;
  doc.Parse(jsonText.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return false;
  }

  auto& alloc = doc.GetAllocator();
  if (doc.HasMember(fieldName)) {
    doc.RemoveMember(fieldName);
  }
  if (fieldValue.has_value()) {
    doc.AddMember(rapidjson::Value(fieldName, alloc).Move(),
                  rapidjson::Value(fieldValue->c_str(), alloc).Move(), alloc);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  jsonText = buffer.GetString();
  return true;
}

void injectWatchedFilesIntoLatestFileReadResult(
    shared::AgentHistory& history, const std::optional<std::string>& payload) {
  std::unordered_set<std::string> fileReadCallIds;
  std::unordered_set<std::string> fileEditCallIds;
  for (const auto& turn : history.turns) {
    for (const auto& msg : turn.messages) {
      for (const auto& part : msg.content) {
        if (const auto* call = std::get_if<shared::ToolCallContent>(&part);
            call) {
          if (call->name == "file_read") {
            fileReadCallIds.insert(call->id);
          } else if (call->name == "file_edit" || call->name == "file_write") {
            fileEditCallIds.insert(call->id);
          }
        }
      }
    }
  }

  shared::ToolResultContent* latestFileReadResult = nullptr;
  shared::ToolResultContent* latestFileEditResult = nullptr;
  for (auto& turn : history.turns) {
    for (auto& msg : turn.messages) {
      for (auto& part : msg.content) {
        auto* result = std::get_if<shared::ToolResultContent>(&part);
        if (!result) {
          continue;
        }
        if (fileReadCallIds.find(result->toolCallId) != fileReadCallIds.end()) {
          rewriteToolResultJsonField(result->result, "content", std::nullopt);
          latestFileReadResult = result;
        }
        if (fileEditCallIds.find(result->toolCallId) != fileEditCallIds.end()) {
          rewriteToolResultJsonField(result->result, "updated_files",
                                     std::nullopt);
          latestFileEditResult = result;
        }
      }
    }
  }

  if (!payload.has_value()) {
    return;
  }
  if (latestFileEditResult) {
    rewriteToolResultJsonField(latestFileEditResult->result, "updated_files",
                               payload);
    return;
  }
  if (latestFileReadResult) {
    rewriteToolResultJsonField(latestFileReadResult->result, "content",
                               payload);
  }
}

void recordFileReadWatch(const shared::AgentContext& context,
                         const shared::IWorkspace& workspace,
                         const std::string& toolArgsJson,
                         const std::string& resultJson) {
  rapidjson::Document args;
  args.Parse(toolArgsJson.c_str());
  rapidjson::Document result;
  result.Parse(resultJson.c_str());
  if (args.HasParseError() || result.HasParseError() || !args.IsObject() ||
      !result.IsObject() || !args.HasMember("path") ||
      !args["path"].IsString()) {
    return;
  }

  const std::string absolutePath =
      workspace.resolvePath(args["path"].GetString());
  const int lineStart =
      result.HasMember("line_start") && result["line_start"].IsInt()
          ? result["line_start"].GetInt()
          : 1;
  const int lineEnd =
      result.HasMember("line_end") && result["line_end"].IsInt()
          ? result["line_end"].GetInt()
          : lineStart;
  const bool readFull =
      result.HasMember("read_full") && result["read_full"].IsBool()
          ? result["read_full"].GetBool()
          : false;
  const bool reachedEnd =
      result.HasMember("reached_end") && result["reached_end"].IsBool()
          ? result["reached_end"].GetBool()
          : readFull;

  ThreadManager tm(ThreadManager::defaultBasePath());
  tm.mutateAgentLiveState(context.history->threadId, context.identity.id,
                          [&](AgentLiveState& liveState) {
    liveState.threadId = context.history->threadId;
    liveState.agentId = context.identity.id;
    auto& watchedFile = ensureWatchedEntry(liveState, absolutePath);
    watchedFile.updatedAt = nowEpochMs();
    watchedFile.lastContentHash.clear();

    if (readFull) {
      watchedFile.fullyRead = true;
      watchedFile.ranges = {{1, std::max(1, lineEnd)}};
      watchedFile.terminalLine = std::max(1, lineEnd);
    } else {
      watchedFile.fullyRead = false;
      mergeRange(watchedFile.ranges, lineStart, lineEnd);
      if (reachedEnd && lineEnd >= lineStart) {
        watchedFile.terminalLine = lineEnd;
      }
      if (isFullyCovered(watchedFile)) {
        watchedFile.fullyRead = true;
        watchedFile.ranges = {{1, *watchedFile.terminalLine}};
      }
    }

    trimRememberedWatchedFiles(liveState);
  });
}

void recordFileRefreshWatch(const shared::AgentContext& context, shared::IHost& host,
                            shared::IWorkspace& workspace,
                            const std::string& toolArgsJson) {
  rapidjson::Document args;
  args.Parse(toolArgsJson.c_str());
  if (args.HasParseError() || !args.IsObject() || !args.HasMember("path") ||
      !args["path"].IsString()) {
    return;
  }

  const std::string absolutePath =
      workspace.resolvePath(args["path"].GetString());

  try {
    const auto bytes = host.readFile(absolutePath);
    const std::string content(bytes.begin(), bytes.end());
    const auto lines = splitLines(content);
    const int terminalLine = std::max(1, static_cast<int>(lines.size()));

    ThreadManager tm(ThreadManager::defaultBasePath());
    tm.mutateAgentLiveState(context.history->threadId, context.identity.id,
                            [&](AgentLiveState& liveState) {
      liveState.threadId = context.history->threadId;
      liveState.agentId = context.identity.id;
      auto& watchedFile = ensureWatchedEntry(liveState, absolutePath);
      watchedFile.fullyRead = true;
      watchedFile.ranges = {{1, terminalLine}};
      watchedFile.terminalLine = terminalLine;
      watchedFile.lastContentHash.clear();
      watchedFile.updatedAt = nowEpochMs();
      trimRememberedWatchedFiles(liveState);
    });
    workspace.markFileAsFullyRead(absolutePath);
  } catch (...) {
  }
}

} // namespace

shared::AgentHistory buildRequestHistoryWithRuntimeOverlays(
    const shared::AgentContext& context, shared::IHost& host,
    shared::IWorkspace& workspace) {
  shared::AgentHistory requestHistory;
  if (context.history) {
    requestHistory = *context.history;
  }
  injectWatchedFilesIntoLatestFileReadResult(
      requestHistory, buildWatchedFilesPayload(context, host, workspace));
  requestHistory.turns.push_back(
      makeOverlayTurn("runtime-overlay-work-state", buildWorkOverlay(context)));
  return requestHistory;
}

void reconcileSuccessfulToolResult(const shared::AgentContext& context,
                                   shared::IHost& host,
                                   shared::IWorkspace& workspace,
                                   const std::string& toolName,
                                   const std::string& toolArgsJson,
                                   const std::string& resultJson) {
  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty()) {
    return;
  }

  if (toolName == "file_read") {
    recordFileReadWatch(context, workspace, toolArgsJson, resultJson);
    return;
  }

  if (toolName == "file_edit" || toolName == "file_write") {
    recordFileRefreshWatch(context, host, workspace, toolArgsJson);
  }
}

void refreshFileWatch(const shared::AgentContext& context,
                      shared::IHost& host,
                      shared::IWorkspace& workspace,
                      const std::string& path) {
  if (!context.history || context.history->threadId.empty() ||
      context.identity.id.empty() || path.empty()) {
    return;
  }

  const std::string absolutePath = workspace.resolvePath(path);
  ThreadManager tm(ThreadManager::defaultBasePath());

  try {
    const auto bytes = host.readFile(absolutePath);
    const std::string content(bytes.begin(), bytes.end());
    const auto lines = splitLines(content);
    const int terminalLine = std::max(1, static_cast<int>(lines.size()));

    tm.mutateAgentLiveState(context.history->threadId, context.identity.id,
                            [&](AgentLiveState& liveState) {
      liveState.threadId = context.history->threadId;
      liveState.agentId = context.identity.id;
      auto& watchedFile = ensureWatchedEntry(liveState, absolutePath);
      watchedFile.fullyRead = true;
      watchedFile.ranges = {{1, terminalLine}};
      watchedFile.terminalLine = terminalLine;
      watchedFile.lastContentHash.clear();
      watchedFile.updatedAt = nowEpochMs();
      trimRememberedWatchedFiles(liveState);
    });
    workspace.markFileAsFullyRead(absolutePath);
  } catch (...) {
  }
}

} // namespace firmius::core::runtime_overlay
