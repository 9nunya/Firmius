#include "tools/DelegatePresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"

#include <rapidjson/document.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace firmius::tui2 {

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

std::vector<std::string> DelegatePresenter::render(const ToolCallItem& item, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Delegate")};
  }

  auto args = parseArgs(item.args());

  // Stop action — quick inline
  if (args.action == "Stop") {
    if (item.phase() == ToolPhase::Called) {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Stopping " + args.agentId)};
    }
    return {ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 Stopped " + args.agentId)};
  }

  // Wait action
  if (args.action == "Wait") {
    if (item.phase() == ToolPhase::Called) {
      std::string text = "  \xe2\x9f\xb3 Waiting on " + args.agentId + "...";
      return {ansi::fgRgb(220, 180, 80, text),
              ansi::dim(ansi::fgRgb(120, 120, 140, "  " + formatDuration(item.elapsed())))};
    }
    auto res = parseResult(item.result());
    std::string name = res.friendlyName.empty() ? args.agentId : res.friendlyName;
    if (item.success()) {
      return {ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 " + name + " \xe2\x80\x94 completed")};
    }
    return {ansi::fgRgb(220, 80, 80, "  \xe2\x9c\x97 " + name + " \xe2\x80\x94 failed")};
  }

  // Spawn action — complex presenter
  if (item.phase() == ToolPhase::Called) {
    std::vector<std::string> result;
    std::string title = args.title.empty() ? args.name : args.title;
    if (title.empty()) title = args.persona;
    if (title.empty()) title = "agent";

    result.push_back(ansi::fgRgb(220, 180, 80, "  \xe2\x9f\xb3 Summoning ") +
                     ansi::bold(ansi::fgRgb(220, 220, 230, title)));

    // Body: persona, model/category, task preview
    if (!args.persona.empty()) {
      result.push_back(ansi::dim(ansi::fgRgb(140, 140, 160, "  persona: " + args.persona)));
    }
    if (!args.category.empty()) {
      result.push_back(ansi::dim(ansi::fgRgb(140, 140, 160, "  category: " + args.category)));
    }
    if (!args.task.empty()) {
      // Show first 2 lines of task
      std::istringstream stream(args.task);
      std::string line;
      int lineCount = 0;
      while (std::getline(stream, line) && lineCount < 2) {
        result.push_back(ansi::dim(ansi::fgRgb(160, 160, 180, "  " + line)));
        lineCount++;
      }
    }

    // Live footer
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140,
        "  working \xe2\x80\xa2 " + formatDuration(item.elapsed()))));
    return result;
  }

  // Finished Spawn
  auto res = parseResult(item.result());
  std::string title = args.title.empty() ? args.name : args.title;
  if (title.empty()) title = args.persona;
  if (title.empty()) title = "agent";

  if (item.success()) {
    std::vector<std::string> result;
    result.push_back(ansi::fgRgb(100, 200, 120, "  \xe2\x9c\x93 ") +
                     ansi::bold(ansi::fgRgb(220, 220, 230, title)) +
                     ansi::fgRgb(100, 200, 120, " \xe2\x80\x94 completed"));

    if (!res.result.empty()) {
      // Show first 2 lines of result
      std::istringstream stream(res.result);
      std::string line;
      int lineCount = 0;
      while (std::getline(stream, line) && lineCount < 2) {
        result.push_back(ansi::dim(ansi::fgRgb(160, 160, 180, "  " + line)));
        lineCount++;
      }
    }

    // Footer with duration
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140,
        "  " + formatDuration(item.elapsed()))));
    return result;
  }

  // Failed Spawn
  std::vector<std::string> result;
  result.push_back(ansi::fgRgb(220, 80, 80, "  \xe2\x9c\x97 ") +
                   ansi::bold(ansi::fgRgb(220, 220, 230, title)) +
                   ansi::fgRgb(220, 80, 80, " \xe2\x80\x94 failed"));
  if (!res.result.empty()) {
    result.push_back(ansi::fgRgb(220, 80, 80, "  " + res.result));
  }
  return result;
}

} // namespace firmius::tui2
