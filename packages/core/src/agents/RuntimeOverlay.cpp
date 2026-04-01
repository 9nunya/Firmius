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





std::string trimForPrompt(const std::string& value, std::size_t maxLen = 240) {
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




} // namespace

shared::AgentHistory buildRequestHistoryWithRuntimeOverlays(
    const shared::AgentContext& context, shared::IHost&,
    shared::IWorkspace&) {
  shared::AgentHistory requestHistory;
  if (context.history) {
    requestHistory = *context.history;
  }
  requestHistory.turns.push_back(
      makeOverlayTurn("runtime-overlay-work-state", buildWorkOverlay(context)));
  return requestHistory;
}

void reconcileSuccessfulToolResult(const shared::AgentContext&,
                                   shared::IHost&,
                                   shared::IWorkspace&,
                                   const std::string&,
                                   const std::string&,
                                   const std::string&) {
}

void refreshFileWatch(const shared::AgentContext&,
                      shared::IHost&,
                      shared::IWorkspace&,
                      const std::string&) {
}

} // namespace firmius::core::runtime_overlay
