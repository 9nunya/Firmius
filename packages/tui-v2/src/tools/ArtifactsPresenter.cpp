#include "tools/ArtifactsPresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"

#include <rapidjson/document.h>

namespace firmius::tui2 {

bool ArtifactsPresenter::matches(const std::string& toolName) const {
  return toolName == "Artifacts";
}

namespace {

struct ArtifactsArgs {
  std::string action;
  std::string name;
  std::string content;
  std::string reference;
};

ArtifactsArgs parseArgs(const std::string& json) {
  ArtifactsArgs a;
  if (json.empty()) return a;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return a;
  if (doc.HasMember("action") && doc["action"].IsString()) a.action = doc["action"].GetString();
  if (doc.HasMember("name") && doc["name"].IsString()) a.name = doc["name"].GetString();
  if (doc.HasMember("content") && doc["content"].IsString()) a.content = doc["content"].GetString();
  if (doc.HasMember("reference") && doc["reference"].IsString()) a.reference = doc["reference"].GetString();
  return a;
}

struct ArtifactsResult {
  std::string status;
  std::string reference;
  int count = 0;
};

ArtifactsResult parseResult(const std::string& json) {
  ArtifactsResult r;
  if (json.empty()) return r;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return r;
  if (doc.HasMember("status") && doc["status"].IsString()) r.status = doc["status"].GetString();
  if (doc.HasMember("reference") && doc["reference"].IsString()) r.reference = doc["reference"].GetString();
  if (doc.HasMember("count") && doc["count"].IsInt()) r.count = doc["count"].GetInt();
  return r;
}

} // namespace

std::vector<std::string> ArtifactsPresenter::render(const ToolCallItem& item, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Artifacts")};
  }

  auto args = parseArgs(item.args());

  if (item.phase() == ToolPhase::Called) {
    if (args.action == "Write") {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Writing artifact " + args.name + "...")};
    }
    if (args.action == "Read") {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Reading artifact " + args.name + "...")};
    }
    if (args.action == "List") {
      return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Listing artifacts...")};
    }
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 Artifacts." + args.action)};
  }

  // Finished
  auto res = parseResult(item.result());
  bool success = item.success();
  auto color = [&](const std::string& s) {
    return success ? ansi::fgRgb(100, 200, 120, s) : ansi::fgRgb(220, 80, 80, s);
  };
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";

  if (args.action == "Write") {
    std::vector<std::string> result;
    std::string text = "  " + icon + " Artifact " + args.name;
    if (!res.status.empty()) text += " " + res.status;
    if (!res.reference.empty()) text += " \xe2\x80\xa2 " + res.reference;
    result.push_back(color(text));

    // Show content from args
    if (!args.content.empty()) {
      std::istringstream stream(args.content);
      std::string line;
      int lineCount = 0;
      while (std::getline(stream, line) && lineCount < 20) {
        result.push_back(ansi::dim(ansi::fgRgb(160, 160, 180, "  " + line)));
        lineCount++;
      }
    }
    return result;
  }

  if (args.action == "Read") {
    return {color("  " + icon + " Read artifact " + args.name)};
  }

  if (args.action == "List") {
    return {color("  " + icon + " " + std::to_string(res.count) + " artifacts")};
  }

  return {color("  " + icon + " Artifacts." + args.action)};
}

} // namespace firmius::tui2
