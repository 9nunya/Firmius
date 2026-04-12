#include "agents/RuntimeOverlay.hpp"

#include "agents/UserMemoryWorkspace.hpp"
#include "agents/RollingContextManager.hpp"
#include "agents/PurposeLoader.hpp"
#include "agents/SkillLoader.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/StringUtil.hpp"
#include "utils/FSUtil.hpp"

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

std::string buildLoadedSkillsOverlay(const shared::AgentContext& context,
                                     shared::IHost& host) {
  if (context.state.loadedSkills.empty() && context.state.loadedAgentMds.empty()) {
    return "";
  }

  std::ostringstream out;
  out << "## LOADED SKILLS\n";

  if (!context.state.loadedSkills.empty()) {
    out << "Skills\n";
    for (const auto& skillId : context.state.loadedSkills) {
      out << "- " << skillId << "\n";
    }
    out << "\n";
  }

  if (!context.state.loadedAgentMds.empty()) {
    out << "Loaded Files\n";
    for (const auto& path : context.state.loadedAgentMds) {
      // Require a recorded skill root for all loaded files.
      auto it = context.state.loadedSkillRoots.find(path);
      if (it == context.state.loadedSkillRoots.end()) {
        continue;
      }
      if (!shared::FSUtil::isCanonicalSubpath(path, it->second)) {
        continue;
      }

      out << "### " << path << "\n";
      try {
        const auto data = host.readFile(path);
        out << std::string(data.begin(), data.end()) << "\n";
      } catch (...) {
        out << "(unavailable)\n";
      }
      out << "\n";
    }
  }

  return out.str();
}

std::string buildLoadedMcpOverlay(const shared::AgentContext& context) {
  if (context.state.loadedMcpServers.empty()) {
    return "";
  }

  std::ostringstream out;
  out << "## LOADED MCP\n";
  for (const auto& serverName : context.state.loadedMcpServers) {
    out << "- server: " << serverName << "\n";

    const auto toolsIt = context.state.loadedMcpTools.find(serverName);
    if (toolsIt != context.state.loadedMcpTools.end() && !toolsIt->second.empty()) {
      out << "  tools:";
      for (const auto& toolName : toolsIt->second) {
        out << " " << toolName;
      }
      out << "\n";
    }

    const auto resourcesIt = context.state.loadedMcpResources.find(serverName);
    if (resourcesIt != context.state.loadedMcpResources.end() &&
        !resourcesIt->second.empty()) {
      out << "  resources:";
      for (const auto& uri : resourcesIt->second) {
        out << " " << uri;
      }
      out << "\n";
    }

    const auto promptsIt = context.state.loadedMcpPrompts.find(serverName);
    if (promptsIt != context.state.loadedMcpPrompts.end() &&
        !promptsIt->second.empty()) {
      out << "  prompts:";
      for (const auto& promptName : promptsIt->second) {
        out << " " << promptName;
      }
      out << "\n";
    }
  }

  return out.str();
}

std::string buildWatchedFilesOverlay(const shared::AgentContext& context) {
  std::ostringstream out;
  out << "## WATCHED FILES\n";

  out << "Read Files\n";
  if (context.state.readFiles.empty()) {
    out << "- (none)\n";
  } else {
    for (const auto& path : context.state.readFiles) {
      out << "- " << path << "\n";
    }
  }
  out << "\n";

  out << "Fully Read Files\n";
  if (context.state.fullyReadFiles.empty()) {
    out << "- (none)\n";
  } else {
    for (const auto& path : context.state.fullyReadFiles) {
      out << "- " << path << "\n";
    }
  }
  out << "\n";

  out << "Edited Files\n";
  if (context.state.editedFiles.empty()) {
    out << "- (none)\n";
  } else {
    for (const auto& path : context.state.editedFiles) {
      out << "- " << path << "\n";
    }
  }

  return out.str();
}

} // namespace

shared::AgentHistory buildRequestHistoryWithRuntimeOverlays(
    const shared::AgentContext& context, shared::IHost& host,
    shared::IWorkspace&) {
  shared::AgentHistory requestHistory =
      context.history ? RollingContextManager::filterHistoryForRequest(
                            context, *context.history)
                      : shared::AgentHistory{};
  requestHistory.turns.push_back(
      makeOverlayTurn("runtime-overlay-work-state", buildWorkOverlay(context)));

  requestHistory.turns.push_back(makeOverlayTurn(
      "runtime-overlay-watched-files", buildWatchedFilesOverlay(context)));

  const std::string loadedSkillsOverlay = buildLoadedSkillsOverlay(context, host);
  if (!loadedSkillsOverlay.empty()) {
    requestHistory.turns.push_back(
        makeOverlayTurn("runtime-overlay-loaded-skills", loadedSkillsOverlay));
  }

  const std::string loadedMcpOverlay = buildLoadedMcpOverlay(context);
  if (!loadedMcpOverlay.empty()) {
    requestHistory.turns.push_back(
        makeOverlayTurn("runtime-overlay-loaded-mcp", loadedMcpOverlay));
  }

  const std::string rollingStatusOverlay =
      RollingContextManager::buildStatusOverlay(context);
  if (!rollingStatusOverlay.empty()) {
    requestHistory.turns.push_back(
        makeOverlayTurn("runtime-overlay-rolling-status", rollingStatusOverlay));
  }

  const std::string rollingMemoryOverlay =
      RollingContextManager::buildMemoryOverlay(context);
  if (!rollingMemoryOverlay.empty()) {
    requestHistory.turns.push_back(
        makeOverlayTurn("runtime-overlay-rolling-memory", rollingMemoryOverlay));
  }

  bool benchmarkThread = false;
  if (context.history && !context.history->threadId.empty()) {
    try {
      ThreadManager tm(ThreadManager::defaultBasePath());
      benchmarkThread = tm.getMetadata(context.history->threadId).isBenchmarkRun;
    } catch (...) {
    }
  }

  if (!benchmarkThread) {
    const std::string userMemoryOverlay =
        buildUserMemoryOverlay(context.environment.cwd);
    if (!userMemoryOverlay.empty()) {
      requestHistory.turns.push_back(
          makeOverlayTurn("runtime-overlay-user-memory", userMemoryOverlay));
    }
  }

  return requestHistory;
}

void reconcileSuccessfulToolResult(shared::AgentContext& context,
                                   shared::IHost&,
                                   shared::IWorkspace&,
                                   const std::string& toolName,
                                   const std::string&,
                                   const std::string& resultJson) {
  if (toolName != "skill_load" && toolName != "mcp_load") {
    return;
  }

  rapidjson::Document result;
  result.Parse(resultJson.c_str());
  if (result.HasParseError() || !result.IsObject()) {
    return;
  }

  if (toolName == "mcp_load") {
    auto readStringArray = [](const rapidjson::Value& value) {
      std::vector<std::string> out;
      if (!value.IsArray()) {
        return out;
      }
      for (const auto& entry : value.GetArray()) {
        if (entry.IsString()) {
          out.push_back(entry.GetString());
        }
      }
      return out;
    };

    std::string serverName;
    if (result.HasMember("server_name") && result["server_name"].IsString()) {
      serverName = result["server_name"].GetString();
    } else if (result.HasMember("server") && result["server"].IsString()) {
      serverName = result["server"].GetString();
    }
    if (serverName.empty()) {
      return;
    }

    if (std::find(context.state.loadedMcpServers.begin(),
                  context.state.loadedMcpServers.end(),
                  serverName) == context.state.loadedMcpServers.end()) {
      context.state.loadedMcpServers.push_back(serverName);
    }

    if (result.HasMember("loaded_tools")) {
      context.state.loadedMcpTools[serverName] =
          readStringArray(result["loaded_tools"]);
    } else if (result.HasMember("tools")) {
      context.state.loadedMcpTools[serverName] =
          readStringArray(result["tools"]);
    }

    if (result.HasMember("loaded_resources")) {
      context.state.loadedMcpResources[serverName] =
          readStringArray(result["loaded_resources"]);
    } else if (result.HasMember("resources")) {
      context.state.loadedMcpResources[serverName] =
          readStringArray(result["resources"]);
    }

    if (result.HasMember("loaded_prompts")) {
      context.state.loadedMcpPrompts[serverName] =
          readStringArray(result["loaded_prompts"]);
    } else if (result.HasMember("prompts")) {
      context.state.loadedMcpPrompts[serverName] =
          readStringArray(result["prompts"]);
    }
    return;
  }

  if (result.HasMember("skill_id") && result["skill_id"].IsString()) {
    const std::string skillId = result["skill_id"].GetString();
    if (!skillId.empty() &&
        std::find(context.state.loadedSkills.begin(),
                  context.state.loadedSkills.end(),
                  skillId) == context.state.loadedSkills.end()) {
      context.state.loadedSkills.push_back(skillId);
    }
  }

  if (result.HasMember("path") && result["path"].IsString()) {
    const std::string path = result["path"].GetString();
    if (result.HasMember("skill_root") && result["skill_root"].IsString()) {
      const std::string skillRoot = result["skill_root"].GetString();
      const auto allowedDirs = SkillLoader::resolveSkillsDirs();
      bool allowed = false;
      for (const auto& dir : allowedDirs) {
        if (shared::FSUtil::isCanonicalSubpath(skillRoot, dir)) {
          allowed = true;
          break;
        }
      }
      if (allowed && !path.empty()) {
        if (std::find(context.state.loadedAgentMds.begin(),
                      context.state.loadedAgentMds.end(),
                      path) == context.state.loadedAgentMds.end()) {
          context.state.loadedAgentMds.push_back(path);
        }
        context.state.loadedSkillRoots[path] = skillRoot;
      }
    }
  }
}

void reconstructStateFromHistory(shared::AgentContext& context,
                                 shared::IHost& host,
                                 shared::IWorkspace& workspace) {
  if (!context.history) return;

  std::unordered_map<std::string, shared::ToolCallContent> pendingCalls;

  for (const auto& turn : context.history->turns) {
    for (const auto& msg : turn.messages) {
      for (const auto& part : msg.content) {
        if (const auto* tc = std::get_if<shared::ToolCallContent>(&part)) {
          if (!tc->id.empty()) {
            pendingCalls[tc->id] = *tc;
          }
        } else if (const auto* tr = std::get_if<shared::ToolResultContent>(&part)) {
          if (tr->success) {
            auto it = pendingCalls.find(tr->toolCallId);
            if (it != pendingCalls.end()) {
              reconcileSuccessfulToolResult(context, host, workspace, it->second.name, it->second.args, tr->result);
            }
          }
        }
      }
    }
  }
}


void refreshFileWatch(const shared::AgentContext&,
                      shared::IHost&,
                      shared::IWorkspace&,
                      const std::string&) {
}

} // namespace firmius::core::runtime_overlay
