#include "agents/RuntimeOverlay.hpp"

#include "agents/UserMemoryWorkspace.hpp"
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

std::string buildWorkOverlay(const shared::AgentContext& context) {
  if (!context.history || context.history->threadId.empty()) {
    return "## LIVE WORK STATE\nThread: unavailable\n";
  }

  ThreadManager tm(ThreadManager::defaultBasePath());
  std::ostringstream out;
  out << "## LIVE WORK STATE\n";
  out << "Agent: " << context.config.personaName << "\n\n";
  out << renderTodoList(readTodoList(context, tm));
  return out.str();
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
      context.history ? *context.history
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
  if (toolName != "skill_load") {
    return;
  }

  rapidjson::Document result;
  result.Parse(resultJson.c_str());
  if (result.HasParseError() || !result.IsObject()) {
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
