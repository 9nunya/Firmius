#include "tools/DelegatePresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "AppState.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <array>

namespace firmius::tui {

bool DelegatePresenter::matches(const std::string& toolName) const {
  return toolName == "Delegate";
}

namespace {

std::string formatDuration(std::chrono::milliseconds ms) {
  double secs = static_cast<double>(ms.count()) / 1000.0;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << secs << "s";
  return oss.str();
}

std::string activityStateLabel(const firmius::shared::AgentStatus status,
                               bool running,
                               bool booting) {
  switch (status) {
  case firmius::shared::AgentStatus::Streaming:
    return "Thinking...";
  case firmius::shared::AgentStatus::ExecutingTool:
    return "Running tool...";
  case firmius::shared::AgentStatus::ProviderWaiting:
    return "Waiting...";
  case firmius::shared::AgentStatus::Compacting:
    return "Compacting...";
  case firmius::shared::AgentStatus::Error:
    return "Failed";
  default:
    if (booting) return "Booting...";
    if (running) return "Working...";
    return "Completed";
  }
}

struct DelegateArgs {
  std::string action;
  std::string persona;
  std::string task;
  std::string name;
  std::string title;
  std::string category;
  std::string agentId;
  bool async = false;
};

DelegateArgs parseArgs(const std::string& json) {
  DelegateArgs a;
  if (json.empty()) return a;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return a;

  auto getString = [&](const char* key) -> std::string {
    if (doc.HasMember(key) && doc[key].IsString()) return doc[key].GetString();
    return "";
  };
  auto getBool = [&](const char* key) -> bool {
    if (doc.HasMember(key) && doc[key].IsBool()) return doc[key].GetBool();
    return false;
  };

  a.action = getString("action");
  a.persona = getString("persona");
  a.task = getString("task");
  a.name = getString("name");
  a.title = getString("title");
  a.category = getString("category");
  a.agentId = getString("agent_id");
  a.async = getBool("async");
  return a;
}

struct DelegateResult {
  std::string status;
  std::string result;
  std::string agentId;
  std::string friendlyName;
};

DelegateResult parseResult(const std::string& json) {
  DelegateResult r;
  if (json.empty()) return r;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return r;

  if (doc.HasMember("status") && doc["status"].IsString()) r.status = doc["status"].GetString();
  if (doc.HasMember("result") && doc["result"].IsString()) r.result = doc["result"].GetString();
  if (doc.HasMember("agentId") && doc["agentId"].IsString()) r.agentId = doc["agentId"].GetString();
  if (doc.HasMember("friendlyName") && doc["friendlyName"].IsString()) r.friendlyName = doc["friendlyName"].GetString();
  return r;
}

} // namespace

std::vector<std::string> DelegatePresenter::render(const ToolCallItem& item, const ToolRenderContext& ctx, int /*width*/) const {
  auto args = parseArgs(item.args());
  const std::string liveAgentId =
      !item.subagentId().empty() ? item.subagentId() : args.agentId;

  // Helper to resolve agent label
  auto resolveAgentLabel = [&](const std::string& agentId) -> std::string {
    if (ctx.state && !agentId.empty()) {
      const auto* agent = ctx.state->findAgentState(agentId);
      if (agent) {
        if (!agent->friendlyName.empty()) return agent->friendlyName;
        if (!agent->title.empty()) return agent->title;
      }
      // Try to find by iterating all agents (in case ID was renamed)
      for (const auto& a : ctx.state->agentList()) {
        if (a.friendlyName == agentId || a.title == agentId) {
          if (!a.friendlyName.empty()) return a.friendlyName;
          if (!a.title.empty()) return a.title;
        }
      }
    }
    // Truncate UUID to first 8 chars
    if (agentId.size() > 8 && agentId.find('-') != std::string::npos) {
      return agentId.substr(0, 8);
    }
    return agentId;
  };

  // Helper to get agent state label
  auto agentStateLabel = [&](const std::string& agentId) -> std::string {
    if (!ctx.state || agentId.empty()) return {};
    const auto* agent = ctx.state->findAgentState(agentId);
    if (!agent) return {};
    switch (agent->status) {
    case firmius::shared::AgentStatus::Streaming: return " thinking";
    case firmius::shared::AgentStatus::ExecutingTool: return " exec tool";
    case firmius::shared::AgentStatus::ProviderWaiting: return " waiting";
    case firmius::shared::AgentStatus::Compacting: return " compacting";
    default:
      if (agent->running) return " working";
      if (agent->booting) return " booting";
      return {};
    }
  };

  // Stop action
  if (args.action == "Stop") {
    if (item.phase() == ToolPhase::Preparing) {
      return {theme_ansi::warning("  \xe2\x9a\x99 Stopping " + args.agentId)};
    }
    if (item.phase() == ToolPhase::Called) {
      return {theme_ansi::warning("  \xe2\x9a\x99 Stopping " + args.agentId)};
    }
    return {theme_ansi::success("  \xe2\x9c\x93 Stopped " + args.agentId)};
  }

  // Wait action — show during both Preparing and Called phases
  if (args.action == "Wait") {
    std::string agentLabel = resolveAgentLabel(liveAgentId);
    if (ctx.state && (item.phase() == ToolPhase::Preparing || item.phase() == ToolPhase::Called)) {
      const auto* agent = ctx.state->findAgentState(liveAgentId);
      if (agent && !agent->running && !agent->booting) {
        return {theme_ansi::success("  \xe2\x9c\x93 " + agentLabel + " \xe2\x80\x94 completed")};
      }
    }
    if (item.phase() == ToolPhase::Preparing || item.phase() == ToolPhase::Called) {
      std::string stateStr = agentStateLabel(liveAgentId);
      std::string text = "  \xe2\x9f\xb3 Waiting on " + agentLabel + stateStr;
      return {theme_ansi::warning(text),
              theme_ansi::dim("  " + formatDuration(item.elapsed()))};
    }
    // Finished
    auto res = parseResult(item.result());
    std::string name = res.friendlyName.empty() ? agentLabel : res.friendlyName;
    if (item.success()) {
      return {theme_ansi::success("  \xe2\x9c\x93 " + name + " \xe2\x80\x94 completed")};
    }
    return {theme_ansi::error("  \xe2\x9c\x97 " + name + " \xe2\x80\x94 failed")};
  }

  // Spawn action — show during both Preparing and Called phases
  if (item.phase() == ToolPhase::Called || item.phase() == ToolPhase::Preparing) {
    std::vector<std::string> result;
    std::string title = args.title.empty() ? args.name : args.title;
    if (title.empty()) title = args.persona;
    if (title.empty()) title = "agent";

    result.push_back(theme_ansi::warning("  \xe2\x9f\xb3 Summoning ") +
                     ansi::bold(theme_ansi::foreground(title)));

    // Body: persona/model info (no task preview — too verbose)
    if (!args.persona.empty()) {
      result.push_back(theme_ansi::dim("  " + args.persona));
    }
    std::array<std::string, 3> logLines = {
        "  \xe2\x94\x82 Waiting for subagent...",
        "  \xe2\x94\x82",
        "  \xe2\x94\x82",
    };
    if (ctx.state) {
      const auto* agent = ctx.state->findAgentState(liveAgentId);
      if (agent) {
        logLines[0] = "  \xe2\x94\x82 " +
                      activityStateLabel(agent->status, agent->running, agent->booting);
      }
      auto activity = ctx.state->agentActivityLog(liveAgentId, 2);
      for (size_t i = 0; i < activity.size() && i < 2; ++i) {
        logLines[i + 1] = "  \xe2\x94\x82 " + activity[i];
      }
    }
    result.push_back(theme_ansi::dim(logLines[0]));
    result.push_back(theme_ansi::dim(logLines[1]));
    result.push_back(theme_ansi::dim(logLines[2]));
    result.push_back(theme_ansi::dim("  running \xe2\x80\xa2 " +
                                     formatDuration(item.elapsed())));
    return result;
  }

  // Finished Spawn
  auto res = parseResult(item.result());
  std::string title = args.title.empty() ? args.name : args.title;
  if (title.empty()) title = args.persona;
  if (title.empty()) title = "agent";

  // For async spawns, the tool finishes immediately but the subagent keeps running.
  // Show live state instead of "completed" if the subagent is still active.
  std::string childId = res.agentId.empty() ? liveAgentId : res.agentId;
  if (item.success() && ctx.state && !childId.empty()) {
    const auto* childAgent = ctx.state->findAgentState(childId);
    if (childAgent && childAgent->running) {
      // Subagent still running — show live state
      std::vector<std::string> result;
      result.push_back(theme_ansi::success("  \xe2\x9c\x93 ") +
                       ansi::bold(theme_ansi::foreground(title)) +
                       theme_ansi::success(" \xe2\x80\x94 running"));

      std::array<std::string, 3> logLines = {
          "  \xe2\x94\x82 " + activityStateLabel(childAgent->status, childAgent->running,
                                                childAgent->booting),
          "  \xe2\x94\x82",
          "  \xe2\x94\x82",
      };
      auto activity = ctx.state->agentActivityLog(childId, 2);
      for (size_t i = 0; i < activity.size() && i < 2; ++i) {
        logLines[i + 1] = "  \xe2\x94\x82 " + activity[i];
      }

      result.push_back(theme_ansi::dim(logLines[0]));
      result.push_back(theme_ansi::dim(logLines[1]));
      result.push_back(theme_ansi::dim(logLines[2]));
      result.push_back(theme_ansi::dim("  running \xe2\x80\xa2 " +
                                       formatDuration(item.elapsed())));
      return result;
    }
  }

  if (item.success()) {
    std::vector<std::string> result;
    result.push_back(theme_ansi::success("  \xe2\x9c\x93 ") +
                     ansi::bold(theme_ansi::foreground(title)) +
                     theme_ansi::success(" \xe2\x80\x94 completed"));

    if (!res.result.empty()) {
      // Show first 2 lines of result
      std::istringstream stream(res.result);
      std::string line;
      int lineCount = 0;
      while (std::getline(stream, line) && lineCount < 2) {
        result.push_back(theme_ansi::dim("  " + line));
        lineCount++;
      }
    }

    // Footer with duration
    result.push_back(theme_ansi::dim("  " + formatDuration(item.elapsed())));
    return result;
  }

  // Failed Spawn
  std::vector<std::string> result;
  result.push_back(theme_ansi::error("  \xe2\x9c\x97 ") +
                   ansi::bold(theme_ansi::foreground(title)) +
                   theme_ansi::error(" \xe2\x80\x94 failed"));
  if (!res.result.empty()) {
    result.push_back(theme_ansi::error("  " + res.result));
  }
  return result;
}

} // namespace firmius::tui
