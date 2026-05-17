#include "tools/FleetPresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace firmius::tui2 {

bool FleetPresenter::matches(const std::string& toolName) const {
  return toolName == "Fleet";
}

namespace {

std::string formatDuration(std::chrono::milliseconds ms) {
  double secs = static_cast<double>(ms.count()) / 1000.0;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << secs << "s";
  return oss.str();
}

} // namespace

std::vector<std::string> FleetPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Fleet")};
  }

  std::string action;
  std::string mode;
  std::string lockId;
  std::string targetAgent;
  std::string paths;
  std::string requestId;
  bool accept = false;
  std::string denyReason;

  if (!item.args().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.args().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (doc.HasMember("action") && doc["action"].IsString()) action = doc["action"].GetString();
      if (doc.HasMember("mode") && doc["mode"].IsString()) mode = doc["mode"].GetString();
      if (doc.HasMember("lock_id") && doc["lock_id"].IsString()) lockId = doc["lock_id"].GetString();
      if (doc.HasMember("target_agent_id") && doc["target_agent_id"].IsString()) targetAgent = doc["target_agent_id"].GetString();
      if (doc.HasMember("request_id") && doc["request_id"].IsString()) requestId = doc["request_id"].GetString();
      if (doc.HasMember("accept") && doc["accept"].IsBool()) accept = doc["accept"].GetBool();
      if (doc.HasMember("deny_reason") && doc["deny_reason"].IsString()) denyReason = doc["deny_reason"].GetString();
      if (doc.HasMember("paths") && doc["paths"].IsArray()) {
        for (const auto& p : doc["paths"].GetArray()) {
          if (p.IsString()) {
            if (!paths.empty()) paths += ", ";
            paths += p.GetString();
          }
        }
      }
    }
  }

  // Respond action
  if (action == "Respond") {
    if (item.phase() == ToolPhase::Called) {
      return {theme_ansi::warning("  \xe2\x9a\x99 Responding to lock request...")};
    }
    if (item.success()) {
      if (accept) return {theme_ansi::success("  \xe2\x9c\x93 Accepted lock request")};
      return {theme_ansi::success("  \xe2\x9c\x93 Denied lock request: " + denyReason)};
    }
    return {theme_ansi::error("  \xe2\x9c\x97 Fleet respond failed")};
  }

  // Status action
  if (action == "Status") {
    if (item.phase() == ToolPhase::Called) {
      return {theme_ansi::warning("  \xe2\x9a\x99 Fleet status...")};
    }
    if (!item.result().empty()) {
      rapidjson::Document doc;
      doc.Parse(item.result().c_str());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("locks") && doc["locks"].IsArray()) {
        int count = doc["locks"].Size();
        return {theme_ansi::success("  \xe2\x9c\x93 " + std::to_string(count) + " active locks")};
      }
    }
    return {theme_ansi::success("  \xe2\x9c\x93 Fleet status")};
  }

  // Lock action — mode-dependent
  if (item.phase() == ToolPhase::Called) {
    if (mode == "acquire") {
      return {theme_ansi::warning("  \xe2\x9a\x99 Acquiring lock on " + paths + "...")};
    }
    if (mode == "release") {
      return {theme_ansi::warning("  \xe2\x9a\x99 Releasing lock " + lockId)};
    }
    if (mode == "request") {
      return {theme_ansi::warning("  \xe2\x9f\xb3 Requesting lock from " + targetAgent + "..."),
              theme_ansi::dim("  " + formatDuration(item.elapsed()))};
    }
    if (mode == "wait") {
      return {theme_ansi::warning("  \xe2\x9f\xb3 Waiting for lock " + lockId + "..."),
              theme_ansi::dim("  " + formatDuration(item.elapsed()))};
    }
    if (mode == "check") {
      return {theme_ansi::warning("  \xe2\x9a\x99 Checking locks...")};
    }
    return {theme_ansi::warning("  \xe2\x9a\x99 Fleet.Lock " + mode)};
  }

  // Finished Lock
  bool success = item.success();
  auto color = [&](const std::string& s) {
    return success ? theme_ansi::success(s) : theme_ansi::error(s);
  };
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

  if (mode == "acquire") {
    if (success) return {color("  " + icon + " Lock acquired: " + lockId)};
    return {color("  " + icon + " Lock held by " + targetAgent)};
  }
  if (mode == "release") return {color("  " + icon + " Released lock " + lockId)};
  if (mode == "request") {
    if (success) return {color("  " + icon + " Lock request accepted")};
    return {color("  " + icon + " Lock request timed out")};
  }
  if (mode == "wait") return {color("  " + icon + " Lock released")};
  if (mode == "check") return {color("  " + icon + " Lock check complete")};

  return {color("  " + icon + " Fleet.Lock " + mode)};
}

} // namespace firmius::tui2
